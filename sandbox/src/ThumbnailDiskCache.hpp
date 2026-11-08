#pragma once
// The bookkeeping behind the Content Browser's on-disk thumbnail cache (editor polish step 11): which
// PNG belongs to which asset version, and which to delete when the folder passes its size cap.
// ImGui-, RHI- and image-codec-free, so ThumbnailDiskCacheTest runs on a temp folder.
//
// A file is named by a key over asset path + the asset file's mtime and size + a format version, so an
// edited asset simply gets a new name and the old file ages out. Recency is the file's own write time
// (a hit touches it), which makes the LRU order survive restarts without an index file.
#include "aver/core/Hash.hpp"
#include "aver/core/Types.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace aver::editor {

// Bump when the thumbnail picture changes shape (size, padding, colour handling): every old file is orphaned.
inline constexpr u32 kThumbnailDiskVersion = 1;
inline constexpr u64 kDefaultThumbnailDiskCapMB = 256;

inline std::string thumbnailDiskKey(const std::string& assetPath, u64 mtime, u64 size,
                                    u32 version = kThumbnailDiskVersion) {
    const std::string s = assetPath + "|" + std::to_string(mtime) + "|" + std::to_string(size) + "|v" + std::to_string(version);
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(fnv1a64(std::string_view(s))));
    return hex;
}

class ThumbnailDiskCache {
public:
    // Points the cache at a folder (created on first write) with a size cap in bytes; 0 disables it.
    // Re-pointing at the same folder only updates the cap. Scans the folder's existing files.
    void configure(const std::string& dir, u64 capBytes) {
        capBytes_ = dir.empty() ? 0 : capBytes;
        if (dir != dir_) {
            dir_ = dir;
            entries_.clear();
            total_ = 0;
            counter_ = 0;
            if (!dir_.empty()) scan();
        }
        if (enabled()) evict();
    }
    bool enabled() const { return capBytes_ > 0 && !dir_.empty(); }
    u64  totalBytes() const { return total_; }
    usize fileCount() const { return entries_.size(); }
    u64  capBytes() const { return capBytes_; }

    std::string pathFor(const std::string& key) const {
        return (std::filesystem::path(dir_) / (key + ".png")).string();
    }

    // The file for `key` when one is cached; marks it most recently used (in memory and on disk).
    bool lookup(const std::string& key, std::string& pathOut) {
        const auto it = entries_.find(key);
        if (it == entries_.end()) return false;
        std::error_code ec;
        const std::string p = pathFor(key);
        if (!std::filesystem::exists(p, ec)) {   // deleted behind our back
            total_ -= std::min(total_, it->second.bytes);
            entries_.erase(it);
            return false;
        }
        it->second.used = ++counter_;
        std::filesystem::last_write_time(p, std::filesystem::file_time_type::clock::now(), ec);
        pathOut = p;
        return true;
    }

    // Registers a file the caller just wrote, then evicts down to the cap. Returns the files removed.
    std::vector<std::string> recordWritten(const std::string& key, u64 bytes) {
        const auto it = entries_.find(key);
        if (it != entries_.end()) total_ -= std::min(total_, it->second.bytes);
        entries_[key] = Entry{bytes, ++counter_};
        total_ += bytes;
        return evict();
    }

    // Deletes least-recently-used files until the folder fits the cap. The newest entry is never
    // removed (a single file over the cap stays rather than the cache emptying itself).
    std::vector<std::string> evict() {
        std::vector<std::string> removed;
        if (!enabled()) return removed;
        std::vector<std::pair<u64, std::string>> order;   // (used, key)
        order.reserve(entries_.size());
        for (const auto& kv : entries_) order.emplace_back(kv.second.used, kv.first);
        std::sort(order.begin(), order.end());
        for (usize i = 0; i + 1 < order.size() && total_ > capBytes_; ++i) {
            const std::string& key = order[i].second;
            std::error_code ec;
            std::filesystem::remove(pathFor(key), ec);
            total_ -= std::min(total_, entries_[key].bytes);
            entries_.erase(key);
            removed.push_back(key);
        }
        return removed;
    }

private:
    struct Entry { u64 bytes = 0; u64 used = 0; };

    void scan() {
        std::error_code ec;
        std::vector<std::pair<std::filesystem::file_time_type, std::pair<std::string, u64>>> found;
        for (std::filesystem::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
            const std::filesystem::path& p = it->path();
            if (p.extension() != ".png") continue;
            std::error_code e2;
            const u64 bytes = it->file_size(e2);
            const auto t = it->last_write_time(e2);
            if (e2) continue;
            found.push_back({t, {p.stem().string(), bytes}});
        }
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& f : found) {
            entries_[f.second.first] = Entry{f.second.second, ++counter_};
            total_ += f.second.second;
        }
    }

    std::string dir_;
    u64 capBytes_ = 0;
    u64 total_ = 0;
    u64 counter_ = 0;
    std::unordered_map<std::string, Entry> entries_;
};

} // namespace aver::editor
