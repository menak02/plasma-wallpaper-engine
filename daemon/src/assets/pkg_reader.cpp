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

        m_baseOffset = static_cast<uint32_t>(m_stream.tellg());
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
    std::vector<uint8_t> buffer(entry.length);

    m_stream.seekg(m_baseOffset + entry.offset, std::ios::beg);
    m_stream.read(reinterpret_cast<char*>(buffer.data()), entry.length);

    return buffer;
}

std::string PkgReader::readTextFile(const std::string& filename) {
    auto bytes = readFile(filename);
    if (bytes.empty()) {
        return {};
    }
    // Limit text file size to 256KB to prevent excessive memory usage
    if (bytes.size() > 256 * 1024) {
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

uint32_t PkgReader::readUInt32() {
    uint32_t val = 0;
    m_stream.read(reinterpret_cast<char*>(&val), sizeof(val));
    return val;
}

} // namespace WallpaperEngine::Assets
