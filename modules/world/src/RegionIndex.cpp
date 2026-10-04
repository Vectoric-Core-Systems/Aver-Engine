#include "aver/world/RegionIndex.hpp"

#include "aver/formats/Avr1.hpp"
#include "aver/platform/FileSystem.hpp"
#include "ByteIo.hpp"

#include <algorithm>

namespace aver::world {
namespace {

// The total order the index is kept in. Lexicographic on (x, y, z) -- see the header for why not
// Morton.
bool before(const RegionCoord& a, const RegionCoord& b) {
    if (a.x != b.x) return a.x < b.x;
    if (a.y != b.y) return a.y < b.y;
    return a.z < b.z;
}

// A ceiling on the region count, so a corrupt length cannot make the decoder reserve wildly before
// discovering the buffer is short.
constexpr u32 kMaxRegions = 4'000'000;

} // namespace

bool RegionIndex::add(const RegionEntry& e) {
    const auto it = std::lower_bound(regions.begin(), regions.end(), e.coord,
                                     [](const RegionEntry& r, const RegionCoord& c) { return before(r.coord, c); });
    const bool replaced = it != regions.end() && it->coord == e.coord;
    if (replaced) *it = e;
    else regions.insert(it, e);
    recomputeBounds();
    return replaced;
}

const RegionEntry* RegionIndex::find(const RegionCoord& c) const {
    const auto it = std::lower_bound(regions.begin(), regions.end(), c,
                                     [](const RegionEntry& r, const RegionCoord& v) { return before(r.coord, v); });
    if (it == regions.end() || !(it->coord == c)) return nullptr;
    return &*it;
}

void RegionIndex::recomputeBounds() {
    if (regions.empty()) { minRegion = RegionCoord{}; maxRegion = RegionCoord{}; return; }
    minRegion = maxRegion = regions.front().coord;
    for (const RegionEntry& r : regions) {
        minRegion.x = r.coord.x < minRegion.x ? r.coord.x : minRegion.x;
        minRegion.y = r.coord.y < minRegion.y ? r.coord.y : minRegion.y;
        minRegion.z = r.coord.z < minRegion.z ? r.coord.z : minRegion.z;
        maxRegion.x = r.coord.x > maxRegion.x ? r.coord.x : maxRegion.x;
        maxRegion.y = r.coord.y > maxRegion.y ? r.coord.y : maxRegion.y;
        maxRegion.z = r.coord.z > maxRegion.z ? r.coord.z : maxRegion.z;
    }
}

std::vector<u8> encodeIndex(const RegionIndex& ix) {
    ByteWriter w;
    w.u32v(kIndexMagic);
    w.u32v(kIndexVersion);
    w.u64v(ix.levelId);
    w.i32v(ix.chunkSizeCm);
    w.u64v(ix.worldSeed);
    w.u32v(ix.generatorVersion);
    w.u32v(static_cast<u32>(ix.regions.size()));
    w.i32v(ix.minRegion.x); w.i32v(ix.minRegion.y); w.i32v(ix.minRegion.z);
    w.i32v(ix.maxRegion.x); w.i32v(ix.maxRegion.y); w.i32v(ix.maxRegion.z);
    for (const RegionEntry& r : ix.regions) {
        w.i32v(r.coord.x); w.i32v(r.coord.y); w.i32v(r.coord.z);
        w.u64v(r.contentHash);
        w.u32v(r.chunkCount);
        w.str(r.relativePath);
    }
    // Over everything above, appended last -- so decode can check it before trusting a single field.
    w.u32v(fmt::avrCrc32c(w.bytes.data(), w.bytes.size()));
    return std::move(w.bytes);
}

bool decodeIndex(const u8* data, usize size, RegionIndex& out, std::string* why) {
    const auto fail = [why](const char* m) { if (why) *why = m; return false; };
    if (!data || size < 8 + 4) return fail("index is too short to be one");

    // The CRC first, before any field is used for anything. A corrupt regionCount that survives to
    // a reserve() is a much worse failure than one that is rejected here.
    const usize covered = size - 4;
    const u32 stored = ByteReader(data + covered, 4).u32v();
    if (fmt::avrCrc32c(data, covered) != stored) return fail("index checksum mismatch -- the file is corrupt");

    ByteReader r(data, covered);
    if (r.u32v() != kIndexMagic) return fail("index magic is wrong -- not an .ocindex");
    if (r.u32v() != kIndexVersion) return fail("index version is not understood");
    out.levelId = r.u64v();
    out.chunkSizeCm = r.i32v();
    out.worldSeed = r.u64v();
    out.generatorVersion = r.u32v();
    const u32 count = r.u32v();
    out.minRegion = RegionCoord{r.i32v(), r.i32v(), r.i32v()};
    out.maxRegion = RegionCoord{r.i32v(), r.i32v(), r.i32v()};
    if (r.bad) return fail("index is truncated in its header");
    if (count > kMaxRegions) return fail("index declares an implausible region count");
    // The chunk size is this file's whole reason for job 3; an invalid one must not propagate.
    if (!chunkSizeValid(out.chunkSizeCm)) return fail("index declares a chunk size outside the precision budget");

    out.regions.clear();
    out.regions.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        RegionEntry e;
        e.coord = RegionCoord{r.i32v(), r.i32v(), r.i32v()};
        e.contentHash = r.u64v();
        e.chunkCount = r.u32v();
        e.relativePath = r.str();
        if (r.bad) return fail("index is truncated in its region list");
        // Sortedness is a load-bearing property, not a nicety: find() binary-searches. A file that
        // is out of order is malformed rather than merely slow, so it is refused rather than quietly
        // re-sorted -- re-sorting would hide whatever produced it.
        if (i > 0 && !before(out.regions.back().coord, e.coord))
            return fail("index regions are not sorted or contain duplicates");
        out.regions.push_back(std::move(e));
    }
    if (r.left != 0) return fail("index has unread trailing bytes");
    return true;
}

bool writeIndex(const std::string& path, const RegionIndex& ix, std::string* why) {
    const std::vector<u8> bytes = encodeIndex(ix);
    if (!writeFileBytes(path, bytes.data(), bytes.size())) {
        if (why) *why = "could not write " + path;
        return false;
    }
    return true;
}

bool readIndex(const std::string& path, RegionIndex& out, std::string* why) {
    std::vector<u8> bytes;
    if (!readFileBytes(path, bytes)) {
        if (why) *why = "could not read " + path;
        return false;
    }
    return decodeIndex(bytes.data(), bytes.size(), out, why);
}

} // namespace aver::world
