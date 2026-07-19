#include "aver/platform/FileSystem.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace aver {

bool fileExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

bool readFileBytes(const std::string& path, std::vector<u8>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff size = f.tellg();
    if (size < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<usize>(size));
    if (size > 0 && !f.read(reinterpret_cast<char*>(out.data()), size)) return false;
    return true;
}

bool readFileText(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff size = f.tellg();
    if (size < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<usize>(size));
    if (size > 0 && !f.read(out.data(), size)) return false;
    return true;
}

bool writeFileBytes(const std::string& path, const void* data, usize size) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (size > 0) f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(f);
}

} // namespace aver
