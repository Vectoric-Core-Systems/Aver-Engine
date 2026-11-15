#pragma once
// Size + last-write time of a file: an asset tab records it after its own load or save, so the watcher's echo of that
// save (the file unchanged since) can be told from somebody else's edit.
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace aver::editor {

struct FileStamp {
    std::uintmax_t size = 0;
    long long mtime = 0;
    bool valid = false;

    static FileStamp of(const std::string& path) {
        FileStamp s;
        std::error_code ec;
        const std::uintmax_t size = std::filesystem::file_size(path, ec);
        if (ec) return s;
        const auto t = std::filesystem::last_write_time(path, ec);
        if (ec) return s;
        s.size = size;
        s.mtime = static_cast<long long>(t.time_since_epoch().count());
        s.valid = true;
        return s;
    }
    // Both valid and equal: the file is exactly what was last read or written.
    bool sameAs(const FileStamp& o) const { return valid && o.valid && size == o.size && mtime == o.mtime; }
};

} // namespace aver::editor
