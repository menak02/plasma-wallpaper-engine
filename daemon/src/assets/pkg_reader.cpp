#include "pkg_reader.h"
#include <algorithm>
#include <iostream>

namespace WallpaperEngine::Assets {

static std::string normalizePath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    if (!path.empty() && path[0] == '/') {
        path.erase(0, 1);
    }
    // Reject path traversal attempts
    if (path.find("..") != std::string::npos) {
        return {};
    }
    return path;
}

bool PkgReader::open(const std::filesystem::path& path) {
    close();

    m_path = path;
    m_stream.open(path, std::ios::binary);
    if (!m_stream.is_open()) {
        return false;
    }

    std::error_code sizeErr;
    m_fileSize = std::filesystem::file_size(path, sizeErr);
    if (sizeErr) {
        m_fileSize = 0;
    }

    try {
        std::string header = readSizedString();
        if (header.size() < 4 || header.compare(0, 4, "PKGV") != 0) {
            std::cerr << "Invalid PKG header: " << header << std::endl;
            close();
            return false;
        }

        uint32_t fileCount = readUInt32();
        if (fileCount > 1024) {
            std::cerr << "PKG file has too many entries: " << fileCount << std::endl;
            close();
            return false;
        }
        m_entries.reserve(fileCount);

        for (uint32_t i = 0; i < fileCount; ++i) {
            std::string filename = normalizePath(readSizedString());
            if (filename.empty()) {
                // Skip malicious or empty filename
                continue;
            }
            uint32_t offset = readUInt32();
            uint32_t length = readUInt32();

            m_entries[filename] = PkgFileEntry{
                .filename = filename,
                .offset = offset,
                .length = length
            };
        }

        const std::streamoff base = m_stream.tellg();
        if (base < 0) {
            close();
            return false;
        }
        m_baseOffset = static_cast<uint32_t>(base);

        // Drop entries whose payload does not fit inside the actual file.
        // offset+length must be computed in 64 bits — in 32 bits a hostile
        // pair wraps around the check and allocates against a bogus length.
        for (auto it = m_entries.begin(); it != m_entries.end(); ) {
            const auto& entry = it->second;
            const uint64_t end = uint64_t(m_baseOffset) + entry.offset + entry.length;
            if (entry.length == 0 || end < uint64_t(m_baseOffset) + entry.offset || end > m_fileSize) {
                it = m_entries.erase(it);
            } else {
                ++it;
            }
        }
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to parse PKG " << path << ": " << e.what() << std::endl;
        close();
        return false;
    }
}

void PkgReader::close() {
    if (m_stream.is_open()) {
        m_stream.close();
    }
    m_entries.clear();
    m_baseOffset = 0;
    m_fileSize = 0;
    m_path.clear();
}

bool PkgReader::hasFile(const std::string& filename) const {
    return m_entries.find(normalizePath(filename)) != m_entries.end();
}

std::vector<std::string> PkgReader::listFiles() const {
    std::vector<std::string> list;
    list.reserve(m_entries.size());
    for (const auto& [name, _] : m_entries) {
        list.push_back(name);
    }
    return list;
}

std::vector<uint8_t> PkgReader::readFile(const std::string& filename) {
    auto it = m_entries.find(normalizePath(filename));
    if (it == m_entries.end()) {
        return {};
    }

    const auto& entry = it->second;
    const uint64_t end = uint64_t(m_baseOffset) + entry.offset + entry.length;
    if (entry.length == 0 || end < uint64_t(m_baseOffset) + entry.offset || end > m_fileSize) {
        return {};
    }

    std::vector<uint8_t> buffer(entry.length);

    // 64-bit addition: base+offset wraps in 32 bits on >4 GiB inputs.
    m_stream.seekg(static_cast<std::streamoff>(m_baseOffset) + entry.offset, std::ios::beg);
    m_stream.read(reinterpret_cast<char*>(buffer.data()), entry.length);
    if (m_stream.gcount() != static_cast<std::streamsize>(entry.length)) {
        m_stream.clear();
        return {};
    }

    return buffer;
}

std::string PkgReader::readTextFile(const std::string& filename) {
    auto bytes = readFile(filename);
    if (bytes.empty()) {
        return {};
    }
    // Limit text file size to 8MB — scene.json with scripts can be 1.5MB+ (e.g. 3122339805 = 1.5MB)
    // 256KB killed 6/72 wallpapers; 8MB WU
    if (bytes.size() > 8 * 1024 * 1024) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::string PkgReader::readSizedString() {
    uint32_t length = readUInt32();
    if (length == 0 || length > 4096) {
        return {};
    }
    std::string str(length, '\0');
    m_stream.read(&str[0], length);
    return str;
}

std::string PkgReader::readNullTerminatedString() {
    std::string str;
    char c;
    while (m_stream.get(c) && c != '\0') {
        str += c;
    }
    // Reject overly long strings (path traversal protection)
    if (str.size() > 4096) {
        return {};
    }
    return normalizePath(str);
}

uint32_t PkgReader::readUInt32() {
    // PKG integers are little-endian on disk; assemble byte-wise so parsing
    // does not depend on host endianness.
    uint8_t b[4] = {0, 0, 0, 0};
    m_stream.read(reinterpret_cast<char*>(b), sizeof(b));
    return static_cast<uint32_t>(b[0]) |
           (static_cast<uint32_t>(b[1]) << 8) |
           (static_cast<uint32_t>(b[2]) << 16) |
           (static_cast<uint32_t>(b[3]) << 24);
}

} // namespace WallpaperEngine::Assets
