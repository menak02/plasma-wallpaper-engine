#pragma once

#include <string>
#include <vector>
#include <memory>
#include <fstream>
#include <unordered_map>
#include <cstdint>
#include <filesystem>
#include <span>

namespace WallpaperEngine::Assets {

struct PkgFileEntry {
    std::string filename;
    uint32_t offset = 0;
    uint32_t length = 0;
};

class PkgReader {
public:
    PkgReader() = default;
    ~PkgReader() = default;

    bool open(const std::filesystem::path& path);
    void close();

    bool hasFile(const std::string& filename) const;
    std::vector<std::string> listFiles() const;

    // Read full file content into a memory buffer
    std::vector<uint8_t> readFile(const std::string& filename);
    std::string readTextFile(const std::string& filename);

    const std::filesystem::path& getPath() const { return m_path; }
    bool isOpen() const { return m_stream.is_open(); }

private:
    std::filesystem::path m_path;
    std::ifstream m_stream;
    uint32_t m_baseOffset = 0;
    std::unordered_map<std::string, PkgFileEntry> m_entries;

    std::string readSizedString();
    uint32_t readUInt32();
};

} // namespace WallpaperEngine::Assets
