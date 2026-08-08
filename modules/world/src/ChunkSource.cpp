#include "aver/world/ChunkSource.hpp"

#if AVER_MODULE_SCENE

#  include <algorithm>

namespace aver::world {

bool RegionChunkSource::open(const std::string& indexPath, const std::string& contentDir,
                             std::string* why) {
    open_.clear();
    contentDir_ = contentDir;
    if (!readIndex(indexPath, index_, why)) return false;
    // The index is the authority on chunk size (docs/CHUNKS.md section 6, job 3), and decodeIndex
    // has already refused an invalid one -- so anything above this can take it as given.
    return true;
}

RegionFile* RegionChunkSource::regionFor(const RegionCoord& r, std::string* why) {
    ++tick_;
    for (Open& o : open_)
        if (o.coord == r) { o.lastUsed = tick_; return o.file.get(); }

    // Not listed is NOT an error: most of a world is empty, and answering "no" without touching the
    // disk is the whole reason the index exists.
    const RegionEntry* e = index_.find(r);
    if (!e) return nullptr;

    auto f = std::make_unique<RegionFile>();
    if (!f->open(contentDir_ + "/" + e->relativePath, why)) return nullptr;

    // STALENESS, checked here rather than trusted. A region whose own header disagrees with what the
    // index recorded means the index is out of date; streaming it anyway would serve content the
    // caller believes is something else. Refusing is the honest answer -- see section 6, job 2.
    if (f->header().contentHash != e->contentHash) {
        if (why) *why = e->relativePath + ": content hash disagrees with the index -- the index is stale";
        return nullptr;
    }

    if (open_.size() >= maxOpen_) {
        auto oldest = std::min_element(open_.begin(), open_.end(),
                                       [](const Open& a, const Open& b) { return a.lastUsed < b.lastUsed; });
        open_.erase(oldest);
    }
    open_.push_back(Open{r, std::move(f), tick_});
    return open_.back().file.get();
}

bool RegionChunkSource::has(const ChunkCoord& c) {
    RegionFile* f = regionFor(regionOf(c), nullptr);
    return f && f->hasChunk(localOf(c));
}

bool RegionChunkSource::load(const ChunkCoord& c, ChunkPayload& out, std::string* why) {
    RegionFile* f = regionFor(regionOf(c), why);
    if (!f) {
        if (why && why->empty()) *why = "no region file covers that chunk";
        return false;
    }
    return f->readChunk(localOf(c), out, why);
}

} // namespace aver::world

#endif // AVER_MODULE_SCENE
