// Hyprland IPC backend connection test.
//
// Connects to the running Hyprland compositor's IPC socket and issues a
// `["hyprctl", "version"]` request, mirroring what CompositorBackend's
// Hyprland implementation does at startup.
//
// Exit codes:
//   0  - connected and Hyprland answered a non-error response
//   77 - SKIP: not running Hyprland (no instance signature discoverable).
//        The test is integration-only and not applicable on other WMs
//        (XFCE, labwc, ...), so GPU-less/other-compositor machines stay
//        green via SKIP_RETURN_CODE 77 in the top-level CMakeLists.txt.
//        This mirrors tests/gpu_deform_probe.cpp, which skips the same way
//        when no Vulkan device is present.
//   1  - genuinely broken: Hyprland IS running (a signature was found) but
//        the socket path could not be built, the connect/write failed, or
//        the reply contained an error. Those are real regressions, not
//        "test not applicable", so they must not be turned into skips.

#include <iostream>
#include <string>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// CTest/autotools convention: 77 means "skipped, not applicable".
// Registered as SKIP_RETURN_CODE 77 on hyprland_backend_connect.
static const int kSkip = 77;

static std::string getEnv(const char* name) {
    const char* value = getenv(name);
    return value ? std::string(value) : std::string();
}

static std::string discoverHyprlandSignature() {
    std::string sig = getEnv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!sig.empty()) return sig;

    std::string runtimeDir = getEnv("XDG_RUNTIME_DIR");
    if (runtimeDir.empty()) return std::string();

    std::string hyprDir = runtimeDir + "/hypr";
    DIR* dir = opendir(hyprDir.c_str());
    if (!dir) return std::string();

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_DIR) {
            std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            if (name.find_first_not_of("0123456789abcdefABCDEF_") == std::string::npos) {
                closedir(dir);
                return name;
            }
        }
    }
    closedir(dir);
    return std::string();
}

static std::string buildIpcSocketPath(const std::string& signature) {
    std::string runtimeDir = getEnv("XDG_RUNTIME_DIR");
    if (runtimeDir.empty() || signature.empty()) return std::string();

    std::string path1 = runtimeDir + "/hypr/" + signature + "/daemon_ipc";
    std::string path2 = runtimeDir + "/hypr/" + signature + "/.socket.sock";

    if (access(path1.c_str(), F_OK) == 0) return path1;
    if (access(path2.c_str(), F_OK) == 0) return path2;

    return path1;
}

static int connectToSocket(const std::string& path) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "Failed to create socket" << std::endl;
        return -1;
    }

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to connect to IPC socket" << std::endl;
        close(sock);
        return -1;
    }
    return sock;
}

int main() {
    std::string sig = discoverHyprlandSignature();

    // No signature from either the env var or the XDG runtime dir means no
    // Hyprland instance is serving this session: the test does not apply.
    if (sig.empty()) {
        std::cout << "SKIP: not running Hyprland "
                     "(no HYPRLAND_INSTANCE_SIGNATURE and no $XDG_RUNTIME_DIR/hypr/<sig>)"
                  << std::endl;
        return kSkip;
    }

    std::cout << "Instance signature: " << sig << std::endl;

    // From here on a live Hyprland instance is confirmed, so every failure is
    // a real failure and must stay exit code 1.
    std::string socketPath = buildIpcSocketPath(sig);
    std::cout << "Socket path: " << socketPath << std::endl;

    if (socketPath.empty()) {
        std::cerr << "ERROR: Cannot build socket path (signature " << sig
                  << " present but XDG_RUNTIME_DIR missing or socket absent)"
                  << std::endl;
        return 1;
    }

    int sock = connectToSocket(socketPath);
    if (sock < 0) {
        std::cerr << "ERROR: Hyprland is running (signature " << sig
                  << ") but failed to connect to IPC socket" << std::endl;
        return 1;
    }

    std::cout << "Connected to Hyprland IPC socket" << std::endl;

    std::string request = "[\"hyprctl\", \"version\"]\n";
    if (write(sock, request.data(), request.size()) < 0) {
        std::cerr << "Failed to write to socket" << std::endl;
        close(sock);
        return 1;
    }

    std::string response;
    char buffer[4096];
    ssize_t bytesRead;
    while ((bytesRead = read(sock, buffer, sizeof(buffer))) > 0) {
        response.append(buffer, bytesRead);
        if (response.find('\n') != std::string::npos) break;
    }

    std::cout << "Response: " << response << std::endl;

    close(sock);

    if (response.find("error") == std::string::npos) {
        std::cout << "Hyprland IPC test passed" << std::endl;
        return 0;
    }

    std::cerr << "ERROR: Hyprland IPC version request returned an error" << std::endl;
    return 1;
}
