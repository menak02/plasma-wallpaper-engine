#include "compositor_backend.h"
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
    
    // Try to find it in XDG_RUNTIME_DIR
    std::string runtimeDir = getEnv("XDG_RUNTIME_DIR");
    if (runtimeDir.empty()) return "";
    
    std::string hyprDir = runtimeDir + "/hypr";
    // List directories in hyprDir to find the signature
    // This is a simplified version; production code would use opendir/readdir
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

// Hyprland IPC backend implementation.

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
        
        std::string testResponse = sendHyprlandCommandViaHyprctl("VERSION");
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
        
        // Query Hyprland for monitor names using hyprctl
        std::string response = sendHyprlandCommandViaHyprctl("MONITORS");
        if (response.empty()) {
            return {};
        }
        
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

    bool isOutputCovered(const std::string& outputName) const override {
        (void)outputName;
        if (!m_initialized) {
            // No Hyprland backend available -> assume not covered
            return false;
        }
        
        // This is called from the pause gate. Returns the cached state
        // computed in update().
        return m_cachedCovered;
    }

    void update() override {
        if (!m_initialized) {
            m_cachedCovered = false;
            return;
        }
        
        std::string response = sendHyprlandCommandViaHyprctl("WORKSPACES");
        if (response.empty()) {
            m_cachedCovered = false;
            return;
        }
        
        JsonValue json = parseJson(response);
        bool hasFullscreen = false;
        
        if (json.isArray()) {
            for (size_t i = 0; i < json.size(); i++) {
                const JsonValue& workspace = json[i];
                if (workspace.isObject()) {
                    if (workspace["hasfullscreen"].isBool() && workspace["hasfullscreen"].asBool()) {
                        hasFullscreen = true;
                        break;
                    }
                }
            }
        }
        
        if (!hasFullscreen) {
            std::string clientsResponse = sendHyprlandCommandViaHyprctl("clients");
            if (!clientsResponse.empty()) {
                JsonValue clients = parseJson(clientsResponse);
                if (clients.isArray()) {
                    for (size_t i = 0; i < clients.size(); i++) {
                        const JsonValue& client = clients[i];
                        if (client.isObject()) {
                            if (client["fullscreen"].isNumber() && client["fullscreen"].asNumber() > 0) {
                                hasFullscreen = true;
                                break;
                            }
                            if (client["fullscreenOrPopupWindow"].isBool() && client["fullscreenOrPopupWindow"].asBool()) {
                                hasFullscreen = true;
                                break;
                            }
                            if (client["floating"].isBool() && client["floating"].asBool()) {
                                if (client["size"].isArray() && client["size"].size() == 2) {
                                    double width = client["size"][0].asNumber();
                                    double height = client["size"][1].asNumber();
                                    if (width > 1800 && height > 1000) {
                                        hasFullscreen = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        
        m_cachedCovered = hasFullscreen;
    }

    static std::string discoverControlSocket() {
        std::string signature = discoverHyprlandSignature();
        if (signature.empty()) return {};
        return buildIpcSocketPath(signature);
    }

private:
    std::string m_controlSocketPath;
    int m_socketFd;
    bool m_initialized;
    mutable bool m_cachedCovered = false;
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
