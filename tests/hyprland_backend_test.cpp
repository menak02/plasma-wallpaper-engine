#include <iostream>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

std::string getEnv(const char* name) {
    const char* value = getenv(name);
    return value ? std::string(value) : "";
}

std::string discoverHyprlandSignature() {
    std::string sig = getEnv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!sig.empty()) return sig;
    
    std::string runtimeDir = getEnv("XDG_RUNTIME_DIR");
    if (runtimeDir.empty()) return "";
    
    return "";
}

std::string buildIpcSocketPath(const std::string& signature) {
    std::string runtimeDir = getEnv("XDG_RUNTIME_DIR");
    if (runtimeDir.empty() || signature.empty()) return "";
    
    std::string path1 = runtimeDir + "/hypr/" + signature + "/daemon_ipc";
    std::string path2 = runtimeDir + "/hypr/" + signature + "/.socket.sock";
    
    if (access(path1.c_str(), F_OK) == 0) return path1;
    if (access(path2.c_str(), F_OK) == 0) return path2;
    
    return path1;
}

int main() {
    std::string sig = discoverHyprlandSignature();
    
    if (sig.empty()) {
        std::cerr << "ERROR: HYPRLAND_INSTANCE_SIGNATURE not set" << std::endl;
        return 1;
    }
    
    std::cout << "Instance signature: " << sig << std::endl;
    
    std::string socketPath = buildIpcSocketPath(sig);
    std::cout << "Socket path: " << socketPath << std::endl;
    
    if (socketPath.empty()) {
        std::cerr << "ERROR: Cannot build socket path" << std::endl;
        return 1;
    }
    
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "ERROR: Failed to create socket" << std::endl;
        return 1;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "ERROR: Failed to connect to IPC socket" << std::endl;
        close(sock);
        return 1;
    }
    
    std::cout << "Connected to Hyprland IPC socket" << std::endl;
    
    std::string testCmd = "MONITORS\n";
    send(sock, testCmd.c_str(), testCmd.size(), 0);
    
    char buffer[4096];
    ssize_t bytes = recv(sock, buffer, sizeof(buffer) - 1, 0);
    if (bytes > 0) {
        buffer[bytes] = '\0';
        std::cout << "Response: " << std::string(buffer, std::min(bytes, (ssize_t)500)) << std::endl;
    }
    
    close(sock);
    return 0;
}
