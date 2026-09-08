#include "compositor_backend.h"
#include <algorithm>
#include <string>
#include <vector>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <dirent.h>

namespace {

struct JsonValue {
    enum Type { Null, String, Number, Bool, Array, Object };
    Type type = Null;
    std::string stringValue;
    double numberValue = 0;
    bool boolValue = false;
    std::vector<JsonValue> arrayValue;
    std::vector<std::pair<std::string, JsonValue>> objectValue;
    
    bool isNull() const { return type == Null; }
    bool isString() const { return type == String; }
    bool isNumber() const { return type == Number; }
    bool isBool() const { return type == Bool; }
    bool isArray() const { return type == Array; }
    bool isObject() const { return type == Object; }
    
    const std::string& asString() const { return stringValue; }
    double asNumber() const { return numberValue; }
    bool asBool() const { return boolValue; }
    
    const JsonValue& operator[](const std::string& key) const {
        for (const auto& pair : objectValue) {
            if (pair.first == key) return pair.second;
        }
        static JsonValue nullValue;
        return nullValue;
    }
    
    const JsonValue& operator[](size_t index) const {
        if (index < arrayValue.size()) return arrayValue[index];
        static JsonValue nullValue;
        return nullValue;
    }
    
    size_t size() const {
        if (type == Array) return arrayValue.size();
        if (type == Object) return objectValue.size();
        return 0;
    }
};

JsonValue parseJsonValue(const std::string& json, size_t& pos);

void skipWhitespace(const std::string& json, size_t& pos) {
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r'))
        pos++;
}

JsonValue parseJsonValue(const std::string& json, size_t& pos) {
    skipWhitespace(json, pos);
    if (pos >= json.size()) return {};
    
    if (json[pos] == '"') {
        // String
        JsonValue val;
        val.type = JsonValue::String;
        pos++;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                pos++;
                switch (json[pos]) {
                    case 'n': val.stringValue += '\n'; break;
                    case 't': val.stringValue += '\t'; break;
                    case 'r': val.stringValue += '\r'; break;
                    case '"': val.stringValue += '"'; break;
                    case '\\': val.stringValue += '\\'; break;
                    default: val.stringValue += json[pos]; break;
                }
            } else {
                val.stringValue += json[pos];
            }
            pos++;
        }
        if (pos < json.size()) pos++; // skip closing quote
        return val;
    }
    
    if (json[pos] == '{') {
        // Object
        JsonValue val;
        val.type = JsonValue::Object;
        pos++;
        skipWhitespace(json, pos);
        if (pos < json.size() && json[pos] == '}') {
            pos++;
            return val;
        }
        while (pos < json.size()) {
            skipWhitespace(json, pos);
            if (json[pos] == '"') {
                std::string key;
                pos++;
                while (pos < json.size() && json[pos] != '"') {
                    key += json[pos];
                    pos++;
                }
                if (pos < json.size()) pos++;
                skipWhitespace(json, pos);
                if (pos < json.size() && json[pos] == ':') pos++;
                val.objectValue.push_back(std::make_pair(key, parseJsonValue(json, pos)));
            }
            skipWhitespace(json, pos);
            if (pos < json.size() && json[pos] == ',') pos++;
            else if (pos < json.size() && json[pos] == '}') { pos++; break; }
        }
        return val;
    }
    
    if (json[pos] == '[') {
        // Array
        JsonValue val;
        val.type = JsonValue::Array;
        pos++;
        skipWhitespace(json, pos);
        if (pos < json.size() && json[pos] == ']') {
            pos++;
            return val;
        }
        while (pos < json.size()) {
            val.arrayValue.push_back(parseJsonValue(json, pos));
            skipWhitespace(json, pos);
            if (pos < json.size() && json[pos] == ',') pos++;
            else if (pos < json.size() && json[pos] == ']') { pos++; break; }
        }
        return val;
    }
    
    if (json[pos] == 't' && json.substr(pos, 4) == "true") {
        JsonValue val;
        val.type = JsonValue::Bool;
        val.boolValue = true;
        pos += 4;
        return val;
    }
    
    if (json[pos] == 'f' && json.substr(pos, 5) == "false") {
        JsonValue val;
        val.type = JsonValue::Bool;
        val.boolValue = false;
        pos += 5;
        return val;
    }
    
    if (json[pos] == 'n' && json.substr(pos, 4) == "null") {
        pos += 4;
        return {};
    }
    
    // Number
    if (json[pos] == '-' || (json[pos] >= '0' && json[pos] <= '9')) {
        JsonValue val;
        val.type = JsonValue::Number;
        std::string numStr;
        while (pos < json.size() && (json[pos] == '-' || json[pos] == '.' || json[pos] == 'e' || json[pos] == 'E' || json[pos] == '+' || (json[pos] >= '0' && json[pos] <= '9'))) {
            numStr += json[pos];
            pos++;
        }
        try {
            val.numberValue = std::stod(numStr);
        } catch (...) {
            val.numberValue = 0;
        }
        return val;
    }
    
    return {};
}

JsonValue parseJson(const std::string& json) {
    size_t pos = 0;
    return parseJsonValue(json, pos);
}

std::string getEnv(const char* name) {
    const char* value = getenv(name);
    return value ? std::string(value) : "";
}

std::string discoverHyprlandSignature() {
    // Try HYPRLAND_INSTANCE_SIGNATURE environment variable first
    std::string sig = getEnv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!sig.empty()) return sig;

    // Try to find it by enumerating XDG_RUNTIME_DIR/hypr/
    std::string runtimeDir = getEnv("XDG_RUNTIME_DIR");
    if (runtimeDir.empty()) return "";

    std::string hyprDir = runtimeDir + "/hypr";
    DIR* dir = opendir(hyprDir.c_str());
    if (!dir) return "";

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_DIR) {
            std::string name = entry->d_name;
            // Signature directories are hex-ish, not '.' or '..'
            if (name == "." || name == "..") continue;
            if (name.find_first_not_of("0123456789abcdefABCDEF_") == std::string::npos) {
                closedir(dir);
                return name;
            }
        }
    }
    closedir(dir);
    return "";
}

std::string buildIpcSocketPath(const std::string& signature) {
    std::string runtimeDir = getEnv("XDG_RUNTIME_DIR");
    if (runtimeDir.empty() || signature.empty()) return "";
    
    // Try the standard daemon_ipc path first, then fall back to .socket.sock
    std::string path1 = runtimeDir + "/hypr/" + signature + "/daemon_ipc";
    std::string path2 = runtimeDir + "/hypr/" + signature + "/.socket.sock";
    
    // Check which one exists
    if (access(path1.c_str(), F_OK) == 0) return path1;
    if (access(path2.c_str(), F_OK) == 0) return path2;
    
    return path1; // Return the standard path even if it doesn't exist
}

std::string sendHyprlandCommandViaHyprctl(const std::string& command) {
    std::string cmd = "hyprctl -j " + command;
    
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        std::cerr << "HyprlandBackend: Failed to run hyprctl" << std::endl;
        return "";
    }
    
    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    
    int status = pclose(pipe);
    if (status != 0) {
        std::cerr << "HyprlandBackend: hyprctl command failed with status " << status << std::endl;
        return "";
    }
    
    // Remove trailing newlines
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    
    return result;
}

int connectToHyprlandIpc(const std::string& socketPath) {
    if (socketPath.empty()) return -1;
    
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    
    return sock;
}

} // anonymous namespace

namespace WallpaperEngine::Scene {



struct MonitorInfo {
    std::string name;
    uint32_t width = 0;
    uint32_t height = 0;
    int32_t monitorId = -1;
};

struct WorkspaceInfo {
    std::string name;
    int32_t monitorId = -1;
    std::string monitorName;
    bool hasFullscreen = false;
    int32_t windowCount = 0;
};

struct ClientInfo {
    bool mapped = false;
    bool visible = false;
    bool floating = false;
    int32_t monitorId = -1;
    int32_t workspaceId = -1;
    int32_t fullscreen = 0;
    bool fullscreenOrPopupWindow = false;
    double posX = 0;
    double posY = 0;
    double width = 0;
    double height = 0;
};

class HyprlandBackend : public CompositorBackend {
public:
    explicit HyprlandBackend(const std::string& controlSocketPath)
        : m_controlSocketPath(controlSocketPath), m_socketFd(-1), m_initialized(false) {}

    ~HyprlandBackend() override {
        if (m_socketFd >= 0) {
            close(m_socketFd);
            m_socketFd = -1;
        }
    }

    bool initialize() {
        if (m_controlSocketPath.empty()) {
            std::cerr << "HyprlandBackend: No control socket path provided" << std::endl;
            return false;
        }

        std::string testResponse = sendHyprlandCommandViaHyprctl("version");
        if (testResponse.empty()) {
            std::cerr << "HyprlandBackend: Failed to connect to Hyprland" << std::endl;
            return false;
        }

        m_socketFd = connectToHyprlandIpc(m_controlSocketPath);
        if (m_socketFd >= 0) {
            std::cout << "HyprlandBackend: Connected to IPC socket" << std::endl;
        } else {
            std::cout << "HyprlandBackend: Using hyprctl polling" << std::endl;
        }

        m_initialized = true;
        return true;
    }

    std::vector<std::string> outputNames() const override {
        if (!m_initialized) {
            return {};
        }
        // Return cached output names from the last update() so we do not
        // hit hyprctl on every call. If update() has not run yet, fall back
        // to a live query so the first call still works.
        if (!m_monitorInfos.empty()) {
            std::vector<std::string> names;
            names.reserve(m_monitorInfos.size());
            for (const auto& m : m_monitorInfos) {
                names.push_back(m.name);
            }
            return names;
        }
        return liveOutputNames();
    }

    bool isOutputCovered(const std::string& outputName) const override {
        if (!m_initialized) {
            return false;
        }
        if (m_monitorInfos.empty()) {
            return false;
        }

        // Look up the monitor by name.
        auto it = std::find_if(m_monitorInfos.begin(), m_monitorInfos.end(),
                               [&outputName](const MonitorInfo& m) { return m.name == outputName; });
        if (it == m_monitorInfos.end()) {
            // Unknown output — assume not covered.
            return false;
        }

        const MonitorInfo& mon = *it;

        // Find the active workspace for this monitor.
        const WorkspaceInfo* activeWs = nullptr;
        for (const auto& ws : m_workspaceInfos) {
            if (ws.monitorName == outputName) {
                if (!activeWs || ws.name > activeWs->name) {
                    activeWs = &ws;
                }
            }
        }

        if (!activeWs) {
            // No workspace on this monitor — desktop is visible.
            return false;
        }

        // If the active workspace has fullscreen, the output is covered.
        if (activeWs->hasFullscreen) {
            return true;
        }

        // Otherwise compute tiling coverage from clients on this monitor.
        double monitorArea = static_cast<double>(mon.width) * static_cast<double>(mon.height);
        if (monitorArea <= 0.0) {
            return false;
        }

        double coveredArea = 0.0;
        for (const auto& client : m_clientInfos) {
            if (!client.mapped || !client.visible) continue;
            if (client.monitorId != mon.monitorId) continue;
            // Skip fullscreen/popup-window clients — those are handled by
            // the workspace hasfullscreen flag above.
            if (client.fullscreen != 0 || client.fullscreenOrPopupWindow) continue;
            coveredArea += client.width * client.height;
        }

        double ratio = coveredArea / monitorArea;
        return ratio >= coverageThreshold();
    }

    void update() override {
        if (!m_initialized) {
            m_monitorInfos.clear();
            m_workspaceInfos.clear();
            m_clientInfos.clear();
            return;
        }

        // Refresh monitor list.
        m_monitorInfos = parseMonitors();

        // Refresh workspace list.
        m_workspaceInfos = parseWorkspaces();

        // Refresh client list.
        m_clientInfos = parseClients();
    }

    static std::string discoverControlSocket() {
        std::string signature = discoverHyprlandSignature();
        if (signature.empty()) return {};
        return buildIpcSocketPath(signature);
    }

private:
    std::string m_controlSocketPath;
    int m_socketFd = -1;
    bool m_initialized = false;

    std::vector<MonitorInfo> m_monitorInfos;
    std::vector<WorkspaceInfo> m_workspaceInfos;
    std::vector<ClientInfo> m_clientInfos;

    std::vector<std::string> liveOutputNames() const {
        std::string response = sendHyprlandCommandViaHyprctl("monitors");
        if (response.empty()) return {};

        JsonValue json = parseJson(response);
        std::vector<std::string> outputs;
        if (json.isArray()) {
            for (size_t i = 0; i < json.size(); i++) {
                const JsonValue& monitor = json[i];
                if (monitor.isObject() && monitor["name"].isString()) {
                    outputs.push_back(monitor["name"].asString());
                }
            }
        }
        return outputs;
    }

    std::vector<MonitorInfo> parseMonitors() const {
        std::vector<MonitorInfo> monitors;
        std::string response = sendHyprlandCommandViaHyprctl("monitors");
        if (response.empty()) return monitors;

        JsonValue json = parseJson(response);
        if (!json.isArray()) return monitors;

        for (size_t i = 0; i < json.size(); i++) {
            const JsonValue& m = json[i];
            if (!m.isObject()) continue;
            MonitorInfo info;
            if (m["name"].isString()) info.name = m["name"].asString();
            if (m["width"].isNumber()) info.width = static_cast<uint32_t>(m["width"].asNumber());
            if (m["height"].isNumber()) info.height = static_cast<uint32_t>(m["height"].asNumber());
            if (m["id"].isNumber()) info.monitorId = static_cast<int32_t>(m["id"].asNumber());
            if (!info.name.empty()) {
                monitors.push_back(info);
            }
        }
        return monitors;
    }

    std::vector<WorkspaceInfo> parseWorkspaces() const {
        std::vector<WorkspaceInfo> workspaces;
        std::string response = sendHyprlandCommandViaHyprctl("workspaces");
        if (response.empty()) return workspaces;

        JsonValue json = parseJson(response);
        if (!json.isArray()) return workspaces;

        for (size_t i = 0; i < json.size(); i++) {
            const JsonValue& ws = json[i];
            if (!ws.isObject()) continue;
            WorkspaceInfo info;
            if (ws["name"].isString()) info.name = ws["name"].asString();
            if (ws["monitor"].isString()) info.monitorName = ws["monitor"].asString();
            if (ws["monitorID"].isNumber()) info.monitorId = static_cast<int32_t>(ws["monitorID"].asNumber());
            if (ws["hasfullscreen"].isBool()) info.hasFullscreen = ws["hasfullscreen"].asBool();
            if (ws["windows"].isNumber()) info.windowCount = static_cast<int32_t>(ws["windows"].asNumber());
            workspaces.push_back(info);
        }
        return workspaces;
    }

    std::vector<ClientInfo> parseClients() const {
        std::vector<ClientInfo> clients;
        std::string response = sendHyprlandCommandViaHyprctl("clients");
        if (response.empty()) return clients;

        JsonValue json = parseJson(response);
        if (!json.isArray()) return clients;

        for (size_t i = 0; i < json.size(); i++) {
            const JsonValue& c = json[i];
            if (!c.isObject()) continue;
            ClientInfo info;
            info.mapped = c["mapped"].isBool() ? c["mapped"].asBool() : false;
            info.visible = c["visible"].isBool() ? c["visible"].asBool() : false;
            info.floating = c["floating"].isBool() ? c["floating"].asBool() : false;
            if (c["monitor"].isNumber()) info.monitorId = static_cast<int32_t>(c["monitor"].asNumber());
            if (c["workspace"].isObject()) {
                const JsonValue& ws = c["workspace"];
                if (ws["id"].isNumber()) info.workspaceId = static_cast<int32_t>(ws["id"].asNumber());
            }
            if (c["fullscreen"].isNumber()) info.fullscreen = static_cast<int32_t>(c["fullscreen"].asNumber());
            if (c["fullscreenOrPopupWindow"].isBool()) info.fullscreenOrPopupWindow = c["fullscreenOrPopupWindow"].asBool();
            if (c["at"].isArray() && c["at"].size() >= 2) {
                info.posX = c["at"][0].asNumber();
                info.posY = c["at"][1].asNumber();
            }
            if (c["size"].isArray() && c["size"].size() >= 2) {
                info.width = c["size"][0].asNumber();
                info.height = c["size"][1].asNumber();
            }
            if (info.width > 0 && info.height > 0) {
                clients.push_back(info);
            }
        }
        return clients;
    }
};

std::unique_ptr<CompositorBackend> makeHyprlandBackend() {
    std::string socketPath = HyprlandBackend::discoverControlSocket();
    if (socketPath.empty()) {
        std::cerr << "makeHyprlandBackend: No Hyprland IPC socket found" << std::endl;
        return nullptr;
    }

    auto backend = std::make_unique<HyprlandBackend>(socketPath);
    if (!backend->initialize()) {
        std::cerr << "makeHyprlandBackend: Failed to initialize" << std::endl;
        return nullptr;
    }

    return backend;
}

} // namespace WallpaperEngine::Scene
