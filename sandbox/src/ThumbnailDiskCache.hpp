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
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

// ---- Mesh thumbnails: the readback half. Pure bookkeeping so it runs without a GPU.

// Strips the row padding a texture-to-buffer copy leaves (D3D12 pads rows to 256 bytes) into tightly packed rows.
inline void thumbnailUnpadRows(const u8* src, u32 rowPitch, u32 rowBytes, u32 rows, std::vector<u8>& out) {
    out.resize(static_cast<usize>(rowBytes) * rows);
    for (u32 r = 0; r < rows; ++r)
        std::copy(src + static_cast<usize>(r) * rowPitch, src + static_cast<usize>(r) * rowPitch + rowBytes,
                  out.begin() + static_cast<std::ptrdiff_t>(static_cast<usize>(r) * rowBytes));
}

// The source name a mesh thumbnail is keyed on: the mesh file plus its slot-0 material, so retargeting the
// material re-renders. Pass the name plus thumbnailDependencyTag() of its files so editing them re-renders too.
inline std::string meshThumbnailSource(const std::string& meshPath, const std::string& slot0Material) {
    return meshPath + "|mat:" + slot0Material;
}

// The graph an .ocmat references (its GRAPHREF record, a content-relative path), or "" when it has none.
inline std::string ocmatGraphRef(const std::string& ocmatPath) {
    std::ifstream in(ocmatPath);
    std::string line;
    const char* ws = " \t\r\n";
    while (std::getline(in, line)) {
        const usize b = line.find_first_not_of(ws);
        if (b == std::string::npos || line.size() < b + 9) continue;
        std::string key = line.substr(b, 8);
        for (char& c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (key != "GRAPHREF" || (line[b + 8] != ' ' && line[b + 8] != '\t')) continue;
        const usize v = line.find_first_not_of(ws, b + 8);
        if (v == std::string::npos) return {};
        return line.substr(v, line.find_last_not_of(ws) - v + 1);
    }
    return {};
}

// What the files a mesh's look depends on (its material's .ocmat, that material's graph) were when the thumbnail
// was drawn: path, mtime and size of each. A missing file stamps as 0:0, so deleting one also changes the tag.
inline std::string thumbnailDependencyTag(const std::vector<std::string>& files) {
    std::string tag;
    for (const std::string& f : files) {
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(f, ec);
        const u64 mt = ec ? 0 : static_cast<u64>(t.time_since_epoch().count());
        const u64 sz = ec ? 0 : static_cast<u64>(std::filesystem::file_size(f, ec));
        tag += "|" + f + ":" + std::to_string(mt) + ":" + std::to_string(sz);
    }
    return tag;
}

// A small ring of readback slots. A GPU copy is issued into a slot, and the slot's bytes are safe to read once
// `delay` frames have passed (the copy has certainly retired; nothing ever waits on the GPU). A slot is busy from
// acquire() until due() hands it back, and acquire() returns -1 when all are busy (the asset just misses the cache).
class ThumbnailReadbackRing {
public:
    struct Job { int slot = -1; u64 assetId = 0; std::string key; u64 issued = 0; };

    explicit ThumbnailReadbackRing(u32 slots = 3, u32 delay = 6) : busy_(slots, false), delay_(delay) {}

    int acquire(u64 assetId, const std::string& key, u64 frame) {
        for (usize i = 0; i < busy_.size(); ++i) {
            if (busy_[i]) continue;
            busy_[i] = true;
            jobs_.push_back(Job{static_cast<int>(i), assetId, key, frame});
            return static_cast<int>(i);
        }
        return -1;
    }
    // Jobs whose copy is old enough to read; their slots stay busy until release().
    std::vector<Job> due(u64 frame) {
        std::vector<Job> out, keep;
        for (Job& j : jobs_) (frame >= j.issued + delay_ ? out : keep).push_back(std::move(j));
        jobs_ = std::move(keep);
        return out;
    }
    void release(int slot) { if (slot >= 0 && static_cast<usize>(slot) < busy_.size()) busy_[static_cast<usize>(slot)] = false; }
    void clear() { std::fill(busy_.begin(), busy_.end(), false); jobs_.clear(); }
    usize slots() const { return busy_.size(); }
    usize pending() const { return jobs_.size(); }

private:
    std::vector<bool> busy_;
    std::vector<Job> jobs_;
    u32 delay_;
};

} // namespace aver::editor
