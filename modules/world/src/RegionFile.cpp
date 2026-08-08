#include "aver/world/RegionFile.hpp"

#if AVER_MODULE_SCENE

#  include "aver/formats/Avr1.hpp"
#  include "aver/world/ChunkCodec.hpp"
#  include "ByteIo.hpp"

#  include <algorithm>

namespace aver::world {
namespace {

// Header bytes covered by the CRC, then the CRC itself. 92 + 4 = 96, padded to a whole sector.
constexpr usize kHeaderCoveredBytes = 92;
constexpr usize kHeaderTotalBytes   = 96;

usize sectorsFor(usize bytes) { return (bytes + kRegionSectorBytes - 1) / kRegionSectorBytes; }

void writeHeaderInto(ByteWriter& w, const RegionHeader& h) {
    w.u32v(kRegionMagic);
    w.u32v(h.version);
    w.u32v(h.chunkSizeCm);
    w.u32v(h.sectorBytes);
    w.i32v(h.coord.x); w.i32v(h.coord.y); w.i32v(h.coord.z);
    w.u64v(h.levelId);
    w.u64v(h.worldSeed);
    w.u32v(h.generatorVersion);
    w.u32v(h.groupCount);
    w.u32v(h.chunkCount);
    w.u32v(h.sectorCount);
    w.u64v(h.contentHash);
    w.u64v(h.serial);
    w.u32v(h.groupTableSector);
    w.u32v(h.freeListSector);
    w.u32v(h.freeListCount);
    // Padded out to the covered length so the CRC always spans the same range whatever fields a
    // later version adds below.
    while (w.bytes.size() < kHeaderCoveredBytes) w.u8v(0);
    w.u32v(fmt::avrCrc32c(w.bytes.data(), kHeaderCoveredBytes));
}

// Parses one header copy. False if the magic, version or CRC says it is not a header we wrote.
bool parseHeader(const u8* p, usize n, RegionHeader& out) {
    if (n < kHeaderTotalBytes) return false;
    ByteReader r(p, kHeaderTotalBytes);
    if (r.u32v() != kRegionMagic) return false;
    out.version = r.u32v();
    out.chunkSizeCm = r.u32v();
    out.sectorBytes = r.u32v();
    out.coord = RegionCoord{r.i32v(), r.i32v(), r.i32v()};
    out.levelId = r.u64v();
    out.worldSeed = r.u64v();
    out.generatorVersion = r.u32v();
    out.groupCount = r.u32v();
    out.chunkCount = r.u32v();
    out.sectorCount = r.u32v();
    out.contentHash = r.u64v();
    out.serial = r.u64v();
    out.groupTableSector = r.u32v();
    out.freeListSector = r.u32v();
    out.freeListCount = r.u32v();
    if (r.bad) return false;
    // The CRC is checked BEFORE any field above is trusted for anything -- the same order AVR1 uses
    // and for the same reason: a corrupt sectorCount used to size an allocation is worse than a
    // corrupt sectorCount that is rejected.
    const u32 stored = ByteReader(p + kHeaderCoveredBytes, 4).u32v();
    if (fmt::avrCrc32c(p, kHeaderCoveredBytes) != stored) return false;
    if (out.version != kRegionVersion) return false;
    if (out.sectorBytes != kRegionSectorBytes) return false;
    return true;
}

std::string chunkName(const ChunkLocal& l) {
    return "chunk (" + std::to_string(l.x) + "," + std::to_string(l.y) + "," + std::to_string(l.z) + ")";
}

} // namespace

bool writeRegion(const std::string& path, const RegionWriteDesc& desc,
                 std::vector<std::pair<ChunkLocal, ChunkPayload>> chunks, std::string* why) {
    const auto fail = [why](const std::string& m) { if (why) *why = m; return false; };
    if (!chunkSizeValid(desc.chunkSizeCm)) return fail("region chunk size is outside the precision budget");

    // DETERMINISM STARTS HERE. The caller's chunks arrive from an unordered_map, whose iteration
    // order is not a promise; sorting by (group, slot) makes two cooks of the same content produce
    // byte-identical files, which is what turns "write twice and memcmp" into a real test.
    std::sort(chunks.begin(), chunks.end(), [](const auto& a, const auto& b) {
        const u32 ga = groupIndexOf(a.first), gb = groupIndexOf(b.first);
        if (ga != gb) return ga < gb;
        return slotIndexOf(a.first) < slotIndexOf(b.first);
    });
    // Two payloads for one chunk would silently keep whichever landed last.
    for (usize i = 1; i < chunks.size(); ++i)
        if (chunks[i].first == chunks[i - 1].first)
            return fail("region was given two payloads for " + chunkName(chunks[i].first));

    // Which groups are occupied, in ascending order (the sort above already guarantees it).
    std::vector<u32> groups;
    for (const auto& c : chunks) {
        const u32 g = groupIndexOf(c.first);
        if (groups.empty() || groups.back() != g) groups.push_back(g);
    }

    // ---- lay the file out before writing a byte of it ----
    const usize groupTableBytes = groups.size() * 8;
    const usize groupTableSectors = sectorsFor(groupTableBytes);
    const usize firstDirSector = 2 + groupTableSectors;              // after both headers
    const usize payloadStart = firstDirSector + groups.size() * kGroupDirSectors;

    // Encode every payload up front: the directory needs each one's length and CRC, and encoding is
    // cheap next to the I/O.
    std::vector<std::vector<u8>> encoded;
    encoded.reserve(chunks.size());
    for (const auto& c : chunks) encoded.push_back(encodeChunk(c.second));

    std::vector<u32> firstSector(chunks.size(), 0);
    usize cursor = payloadStart;
    for (usize i = 0; i < chunks.size(); ++i) {
        firstSector[i] = static_cast<u32>(cursor);
        cursor += sectorsFor(encoded[i].size());
    }
    const usize totalSectors = cursor;

    // ---- the group directories, dense and mostly zero ----
    std::vector<std::vector<u8>> dirs(groups.size(), std::vector<u8>(kGroupDirBytes, 0));
    for (usize i = 0; i < chunks.size(); ++i) {
        const u32 g = groupIndexOf(chunks[i].first);
        const usize gi = static_cast<usize>(std::lower_bound(groups.begin(), groups.end(), g) - groups.begin());
        const u32 slot = slotIndexOf(chunks[i].first);
        ByteWriter e;
        e.u32v(firstSector[i]);
        e.u32v(static_cast<u32>(encoded[i].size()));
        e.u32v(fmt::avrCrc32c(encoded[i].data(), encoded[i].size()));
        std::memcpy(dirs[gi].data() + static_cast<usize>(slot) * kGroupDirEntryBytes, e.bytes.data(),
                    kGroupDirEntryBytes);
    }

    // ---- content hash, over the payloads in stored order ----
    // Concatenated rather than combined per chunk, so the hash also witnesses the ORDER -- two
    // regions with the same chunks laid out differently are not the same region.
    std::vector<u8> all;
    for (const auto& e : encoded) all.insert(all.end(), e.begin(), e.end());

    RegionHeader h;
    h.chunkSizeCm = static_cast<u32>(desc.chunkSizeCm);
    h.coord = desc.coord;
    h.levelId = desc.levelId;
    h.worldSeed = desc.worldSeed;
    h.generatorVersion = desc.generatorVersion;
    h.groupCount = static_cast<u32>(groups.size());
    h.chunkCount = static_cast<u32>(chunks.size());
    h.sectorCount = static_cast<u32>(totalSectors);
    h.groupTableSector = groups.empty() ? 0 : 2;
    h.freeListSector = 0;
    h.freeListCount = 0;   // a fresh cook has no holes -- that is what makes it the canonical form
    h.contentHash = all.empty() ? 0 : fmt::avrHash64(all.data(), all.size());
    h.serial = desc.serial;

    // ---- write ----
    aver::File f;
    if (!f.open(path, aver::File::Mode::Create)) return fail("could not open " + path + " for writing");
    // A cook REPLACES: an older, longer file left behind would leave stale sectors past the end that
    // a later reader could be pointed at by a corrupt directory.
    if (!f.setSize(0)) return fail("could not truncate " + path);

    ByteWriter hw;
    writeHeaderInto(hw, h);
    hw.pad(kRegionSectorBytes);
    // BOTH copies, identical, because a cook has nothing to recover from -- the two differ only once
    // something starts mutating the file in place.
    if (!f.writeAt(0, hw.bytes.data(), hw.bytes.size())) return fail("could not write header A");
    if (!f.writeAt(kRegionSectorBytes, hw.bytes.data(), hw.bytes.size())) return fail("could not write header B");

    ByteWriter gt;
    for (usize i = 0; i < groups.size(); ++i) {
        gt.u32v(groups[i]);
        gt.u32v(static_cast<u32>(firstDirSector + i * kGroupDirSectors));
    }
    gt.pad(kRegionSectorBytes);
    if (!gt.bytes.empty() && !f.writeAt(2ull * kRegionSectorBytes, gt.bytes.data(), gt.bytes.size()))
        return fail("could not write the group table");

    for (usize i = 0; i < dirs.size(); ++i)
        if (!f.writeAt((firstDirSector + i * kGroupDirSectors) * u64(kRegionSectorBytes),
                       dirs[i].data(), dirs[i].size()))
            return fail("could not write a group directory");

    for (usize i = 0; i < chunks.size(); ++i) {
        if (encoded[i].empty()) continue;
        if (!f.writeAt(u64(firstSector[i]) * kRegionSectorBytes, encoded[i].data(), encoded[i].size()))
            return fail("could not write " + chunkName(chunks[i].first));
    }

    // The file is sector-aligned even when the last payload is not, so a reader asking for a whole
    // sector at the end gets zeros rather than a short read.
    if (!f.setSize(u64(totalSectors) * kRegionSectorBytes)) return fail("could not size " + path);
    if (!f.sync()) return fail("could not flush " + path + " to the device");
    return true;
}

// ---- reading ---------------------------------------------------------------------------------

bool RegionFile::open(const std::string& path, std::string* why) {
    const auto fail = [why](const std::string& m) { if (why) *why = m; return false; };
    close();
    if (!file_.open(path, aver::File::Mode::Read)) return fail("could not open " + path);

    u8 buf[2][kHeaderTotalBytes] = {};
    RegionHeader ha, hb;
    const bool okA = file_.readAt(0, buf[0], kHeaderTotalBytes) && parseHeader(buf[0], kHeaderTotalBytes, ha);
    const bool okB = file_.readAt(kRegionSectorBytes, buf[1], kHeaderTotalBytes) &&
                     parseHeader(buf[1], kHeaderTotalBytes, hb);

    // THE HIGHER SERIAL THAT PASSES ITS OWN CRC. A crash between the two writes leaves one new and
    // one old, both individually valid; taking the newer is the recovery. A crash DURING one write
    // leaves it CRC-failing, and the other is taken.
    if (okA && okB) { header_ = ha.serial >= hb.serial ? ha : hb; acceptedCopy_ = ha.serial >= hb.serial ? 0 : 1; }
    else if (okA)   { header_ = ha; acceptedCopy_ = 0; }
    else if (okB)   { header_ = hb; acceptedCopy_ = 1; }
    else            { close(); return fail(path + ": neither header copy is valid -- not an .avrgn, or both are corrupt"); }

    if (header_.groupCount > kGroupsPerRegion) { close(); return fail(path + ": group count exceeds a region"); }

    groupTable_.clear();
    groupTable_.reserve(header_.groupCount);
    if (header_.groupCount) {
        std::vector<u8> gt(static_cast<usize>(header_.groupCount) * 8);
        if (header_.groupTableSector == 0 ||
            !file_.readAt(u64(header_.groupTableSector) * kRegionSectorBytes, gt.data(), gt.size())) {
            close();
            return fail(path + ": the group table is truncated");
        }
        ByteReader r(gt.data(), gt.size());
        for (u32 i = 0; i < header_.groupCount; ++i) {
            const u32 g = r.u32v(), s = r.u32v();
            groupTable_.emplace_back(g, s);
        }
        // Sorted is what makes the lookup a binary search; a file claiming otherwise is malformed
        // rather than merely slow, so it is rejected instead of silently sorted here.
        for (usize i = 1; i < groupTable_.size(); ++i)
            if (groupTable_[i].first <= groupTable_[i - 1].first) {
                close();
                return fail(path + ": the group table is not sorted or contains duplicates");
            }
    }
    return true;
}

void RegionFile::close() {
    file_.close();
    header_ = RegionHeader{};
    groupTable_.clear();
    cache_.clear();
    acceptedCopy_ = 0;
}

const RegionFile::GroupDir* RegionFile::groupDir(u32 groupIndex, std::string* why) {
    for (const GroupDir& g : cache_) if (g.groupIndex == groupIndex) return &g;

    const auto it = std::lower_bound(groupTable_.begin(), groupTable_.end(), groupIndex,
                                     [](const std::pair<u32, u32>& e, u32 v) { return e.first < v; });
    if (it == groupTable_.end() || it->first != groupIndex) return nullptr;   // absent, not an error

    GroupDir g;
    g.groupIndex = groupIndex;
    g.raw.resize(kGroupDirBytes);
    if (!file_.readAt(u64(it->second) * kRegionSectorBytes, g.raw.data(), g.raw.size())) {
        if (why) *why = "a group directory is truncated";
        return nullptr;
    }
    cache_.push_back(std::move(g));
    return &cache_.back();
}

bool RegionFile::hasChunk(const ChunkLocal& l, std::string* why) {
    const GroupDir* g = groupDir(groupIndexOf(l), why);
    if (!g) return false;
    ByteReader r(g->raw.data() + static_cast<usize>(slotIndexOf(l)) * kGroupDirEntryBytes,
                 kGroupDirEntryBytes);
    return r.u32v() != 0;   // firstSector 0 is the header sector, so it is a safe "empty"
}

bool RegionFile::readChunk(const ChunkLocal& l, ChunkPayload& out, std::string* why) {
    const auto fail = [why, &l](const std::string& m) { if (why) *why = chunkName(l) + ": " + m; return false; };

    const GroupDir* g = groupDir(groupIndexOf(l), why);
    if (!g) return fail("its group is not in this region");

    ByteReader e(g->raw.data() + static_cast<usize>(slotIndexOf(l)) * kGroupDirEntryBytes,
                 kGroupDirEntryBytes);
    const u32 firstSector = e.u32v();
    const u32 byteLength = e.u32v();
    const u32 crc = e.u32v();
    if (firstSector == 0) return fail("is not stored in this region");
    if (firstSector >= header_.sectorCount) return fail("its directory entry points past the end of the file");

    std::vector<u8> raw(byteLength);
    if (byteLength && !file_.readAt(u64(firstSector) * kRegionSectorBytes, raw.data(), raw.size()))
        return fail("its payload is truncated");

    // NAMED, not just "corrupt". A region is up to 16 km across; "this file is damaged" is not
    // something anyone can act on, and "chunk (12,-3,0) failed its checksum" is.
    if (fmt::avrCrc32c(raw.data(), raw.size()) != crc) return fail("failed its payload checksum");

    out.coord = chunkOf(header_.coord, l);
    out.chunkSizeCm = static_cast<i32>(header_.chunkSizeCm);
    std::string decodeWhy;
    if (!decodeChunk(raw.data(), raw.size(), out, &decodeWhy)) return fail(decodeWhy);
    return true;
}

std::vector<ChunkLocal> RegionFile::chunks(std::string* why) {
    std::vector<ChunkLocal> out;
    out.reserve(header_.chunkCount);
    for (const auto& gt : groupTable_) {
        const GroupDir* g = groupDir(gt.first, why);
        if (!g) continue;
        for (u32 slot = 0; slot < kChunksPerGroup; ++slot) {
            ByteReader r(g->raw.data() + static_cast<usize>(slot) * kGroupDirEntryBytes, kGroupDirEntryBytes);
            if (r.u32v() != 0) out.push_back(localOfGroupSlot(gt.first, slot));
        }
    }
    return out;
}

// ---- writing ---------------------------------------------------------------------------------

bool RegionWriter::open(const std::string& path, const RegionWriteDesc& desc, std::string* why) {
    const auto fail = [why](const std::string& m) { if (why) *why = m; return false; };
    close();
    path_ = path;
    desc_ = desc;
    if (!chunkSizeValid(desc.chunkSizeCm)) return fail("region chunk size is outside the precision budget");

    // A region that does not exist yet is cooked empty first, so open() has exactly one shape to
    // deal with afterwards rather than an "is this new" branch running through everything below.
    if (!fileExists(path)) {
        if (!writeRegion(path, desc, {}, why)) return false;
    }

    if (!file_.open(path, aver::File::Mode::ReadWrite)) return fail("could not open " + path + " for update");

    u8 buf[2][kHeaderTotalBytes] = {};
    RegionHeader ha, hb;
    const bool okA = file_.readAt(0, buf[0], kHeaderTotalBytes) && parseHeader(buf[0], kHeaderTotalBytes, ha);
    const bool okB = file_.readAt(kRegionSectorBytes, buf[1], kHeaderTotalBytes) &&
                     parseHeader(buf[1], kHeaderTotalBytes, hb);
    if (okA && okB) header_ = ha.serial >= hb.serial ? ha : hb;
    else if (okA)   header_ = ha;
    else if (okB)   header_ = hb;
    else { close(); return fail(path + ": neither header copy is valid"); }

    if (header_.groupCount > kGroupsPerRegion) { close(); return fail(path + ": group count exceeds a region"); }

    // The group table, then every directory. A writer holds them all: it has to know which sectors
    // are live to allocate around them, and the directories ARE that record.
    groups_.clear();
    if (header_.groupCount) {
        std::vector<u8> gt(static_cast<usize>(header_.groupCount) * 8);
        if (header_.groupTableSector == 0 ||
            !file_.readAt(u64(header_.groupTableSector) * kRegionSectorBytes, gt.data(), gt.size())) {
            close();
            return fail(path + ": the group table is truncated");
        }
        ByteReader r(gt.data(), gt.size());
        for (u32 i = 0; i < header_.groupCount; ++i) {
            Group g;
            g.index = r.u32v();
            g.dirSector = r.u32v();
            g.raw.resize(kGroupDirBytes);
            if (!file_.readAt(u64(g.dirSector) * kRegionSectorBytes, g.raw.data(), g.raw.size())) {
                close();
                return fail(path + ": a group directory is truncated");
            }
            groups_.push_back(std::move(g));
        }
    }

    free_.clear();
    if (header_.freeListCount) {
        std::vector<u8> fl(static_cast<usize>(header_.freeListCount) * 8);
        if (header_.freeListSector == 0 ||
            !file_.readAt(u64(header_.freeListSector) * kRegionSectorBytes, fl.data(), fl.size())) {
            close();
            return fail(path + ": the free list is truncated");
        }
        ByteReader r(fl.data(), fl.size());
        for (u32 i = 0; i < header_.freeListCount; ++i) {
            Extent e;
            e.first = r.u32v();
            e.count = r.u32v();
            if (e.count) free_.push_back(e);
        }
    }
    return true;
}

void RegionWriter::close() {
    file_.close();
    header_ = RegionHeader{};
    groups_.clear();
    free_.clear();
    groupTableDirty_ = false;
    path_.clear();
}

u32 RegionWriter::freeSectors() const {
    u32 n = 0;
    for (const Extent& e : free_) n += e.count;
    return n;
}

RegionWriter::Group* RegionWriter::group(u32 groupIndex) {
    const auto it = std::lower_bound(groups_.begin(), groups_.end(), groupIndex,
                                     [](const Group& g, u32 v) { return g.index < v; });
    if (it == groups_.end() || it->index != groupIndex) return nullptr;
    return &*it;
}

RegionWriter::Group* RegionWriter::groupOrCreate(u32 groupIndex, std::string* why) {
    if (Group* g = group(groupIndex)) return g;

    // A NEW GROUP NEEDS A NEW DIRECTORY AND A LONGER TABLE. This is the case v1's fixed layout could
    // not express and is the reason the table's position moved into the header.
    const u32 dirSector = allocate(kGroupDirSectors, why);
    if (!dirSector) return nullptr;

    Group g;
    g.index = groupIndex;
    g.dirSector = dirSector;
    g.raw.assign(kGroupDirBytes, 0);
    g.dirty = true;
    const auto it = std::lower_bound(groups_.begin(), groups_.end(), groupIndex,
                                     [](const Group& a, u32 v) { return a.index < v; });
    const auto ins = groups_.insert(it, std::move(g));
    groupTableDirty_ = true;
    return &*ins;
}

u32 RegionWriter::allocate(u32 sectors, std::string* why) {
    if (sectors == 0) return 0;
    // First fit over a list kept sorted and coalesced. Best fit would pack tighter; first fit keeps
    // allocation order a function of the free list alone, and compaction is what removes the
    // fragmentation either way.
    for (usize i = 0; i < free_.size(); ++i) {
        if (free_[i].count < sectors) continue;
        const u32 at = free_[i].first;
        free_[i].first += sectors;
        free_[i].count -= sectors;
        if (free_[i].count == 0) free_.erase(free_.begin() + static_cast<isize>(i));
        return at;
    }
    // Nothing big enough: grow the file. The header's sectorCount is the allocation cursor.
    const u32 at = header_.sectorCount;
    if (!file_.setSize(u64(at + sectors) * kRegionSectorBytes)) {
        if (why) *why = "could not grow " + path_;
        return 0;
    }
    header_.sectorCount = at + sectors;
    return at;
}

void RegionWriter::release(u32 first, u32 count) {
    if (count == 0) return;
    const auto it = std::lower_bound(free_.begin(), free_.end(), first,
                                     [](const Extent& e, u32 v) { return e.first < v; });
    const auto ins = free_.insert(it, Extent{first, count});
    // COALESCED IMMEDIATELY, both ways. Without it, releasing a hundred single-sector chunks leaves
    // a hundred entries that can never satisfy a two-sector request -- a free list that grows while
    // the space it describes becomes unusable.
    usize i = static_cast<usize>(ins - free_.begin());
    if (i > 0 && free_[i - 1].first + free_[i - 1].count == free_[i].first) {
        free_[i - 1].count += free_[i].count;
        free_.erase(free_.begin() + static_cast<isize>(i));
        --i;
    }
    if (i + 1 < free_.size() && free_[i].first + free_[i].count == free_[i + 1].first) {
        free_[i].count += free_[i + 1].count;
        free_.erase(free_.begin() + static_cast<isize>(i) + 1);
    }
}

bool RegionWriter::flushMetadata(std::string* why) {
    const auto fail = [why](const std::string& m) { if (why) *why = m; return false; };

    for (Group& g : groups_) {
        if (!g.dirty) continue;
        if (!file_.writeAt(u64(g.dirSector) * kRegionSectorBytes, g.raw.data(), g.raw.size()))
            return fail("could not write a group directory");
        g.dirty = false;
    }

    if (groupTableDirty_ || header_.groupTableSector == 0) {
        // RELOCATED rather than grown in place: the sectors after it belong to somebody else.
        const u32 need = static_cast<u32>(sectorsFor(groups_.size() * 8));
        const u32 old = header_.groupTableSector;
        const u32 oldNeed = static_cast<u32>(sectorsFor((groups_.size() ? groups_.size() - 1 : 0) * 8));
        const u32 at = allocate(need ? need : 1, why);
        if (!at) return false;
        ByteWriter gt;
        for (const Group& g : groups_) { gt.u32v(g.index); gt.u32v(g.dirSector); }
        gt.pad(kRegionSectorBytes);
        if (!file_.writeAt(u64(at) * kRegionSectorBytes, gt.bytes.data(), gt.bytes.size()))
            return fail("could not write the group table");
        header_.groupTableSector = at;
        header_.groupCount = static_cast<u32>(groups_.size());
        if (old) release(old, oldNeed ? oldNeed : 1);
        groupTableDirty_ = false;
    }

    // The free list, written AFTER any allocation it just satisfied, so what lands on disk is the
    // state that follows this whole operation rather than one from the middle of it.
    {
        const u32 oldSector = header_.freeListSector;
        const u32 oldCount = header_.freeListCount;
        if (free_.empty()) {
            header_.freeListSector = 0;
            header_.freeListCount = 0;
        } else {
            const u32 need = static_cast<u32>(sectorsFor(free_.size() * 8 + 8));
            const u32 at = allocate(need, why);
            if (!at) return false;
            ByteWriter fl;
            for (const Extent& e : free_) { fl.u32v(e.first); fl.u32v(e.count); }
            fl.pad(kRegionSectorBytes);
            if (!file_.writeAt(u64(at) * kRegionSectorBytes, fl.bytes.data(), fl.bytes.size()))
                return fail("could not write the free list");
            header_.freeListSector = at;
            header_.freeListCount = static_cast<u32>(free_.size());
        }
        if (oldSector) release(oldSector, static_cast<u32>(sectorsFor(oldCount * 8 + 8)));
    }

    if (!file_.sync()) return fail("could not flush metadata to the device");

    // THE HEADER LAST, to the copy that is NOT live, with serial + 1. So the two are never both
    // mid-write and a reader always has one intact.
    header_.serial += 1;
    header_.version = kRegionVersion;
    header_.sectorBytes = kRegionSectorBytes;
    ByteWriter hw;
    writeHeaderInto(hw, header_);
    hw.pad(kRegionSectorBytes);
    const u64 target = (header_.serial % 2 == 1) ? 0 : u64(kRegionSectorBytes);
    if (!file_.writeAt(target, hw.bytes.data(), hw.bytes.size())) return fail("could not write the header");
    if (!file_.sync()) return fail("could not flush the header to the device");
    return true;
}

bool RegionWriter::writeChunk(const ChunkLocal& l, const ChunkPayload& p, std::string* why) {
    const auto fail = [why](const std::string& m) { if (why) *why = m; return false; };
    if (!file_.isOpen()) return fail("region is not open for writing");

    const std::vector<u8> bytes = encodeChunk(p);
    const u32 need = static_cast<u32>(sectorsFor(bytes.size()));

    Group* g = groupOrCreate(groupIndexOf(l), why);
    if (!g) return false;

    const usize off = static_cast<usize>(slotIndexOf(l)) * kGroupDirEntryBytes;
    ByteReader old(g->raw.data() + off, kGroupDirEntryBytes);
    const u32 oldFirst = old.u32v();
    const u32 oldLen = old.u32v();

    // TO FREE SECTORS, NEVER OVER THE LIVE ONES. A crash before the directory is patched then loses
    // only space nothing points at; the old payload is still there and still referenced.
    const u32 at = allocate(need ? need : 1, why);
    if (!at) return false;
    if (!bytes.empty() && !file_.writeAt(u64(at) * kRegionSectorBytes, bytes.data(), bytes.size()))
        return fail("could not write " + chunkName(l));
    if (!file_.sync()) return fail("could not flush " + chunkName(l) + " to the device");

    ByteWriter e;
    e.u32v(at);
    e.u32v(static_cast<u32>(bytes.size()));
    e.u32v(fmt::avrCrc32c(bytes.data(), bytes.size()));
    std::memcpy(g->raw.data() + off, e.bytes.data(), kGroupDirEntryBytes);
    g->dirty = true;

    if (oldFirst != 0) release(oldFirst, static_cast<u32>(sectorsFor(oldLen)));
    else header_.chunkCount += 1;

    // contentHash is now MEANINGLESS as a description of the whole region until a compaction, and
    // saying so beats leaving a stale value that an index would compare against and believe. 0 is
    // the agreed "not computed", and writeRegion never produces it for a non-empty region.
    header_.contentHash = 0;
    return flushMetadata(why);
}

bool RegionWriter::removeChunk(const ChunkLocal& l, std::string* why) {
    const auto fail = [why](const std::string& m) { if (why) *why = m; return false; };
    if (!file_.isOpen()) return fail("region is not open for writing");
    Group* g = group(groupIndexOf(l));
    if (!g) return fail(chunkName(l) + " is not in this region");

    const usize off = static_cast<usize>(slotIndexOf(l)) * kGroupDirEntryBytes;
    ByteReader old(g->raw.data() + off, kGroupDirEntryBytes);
    const u32 first = old.u32v();
    const u32 len = old.u32v();
    if (first == 0) return fail(chunkName(l) + " is not in this region");

    std::memset(g->raw.data() + off, 0, kGroupDirEntryBytes);
    g->dirty = true;
    release(first, static_cast<u32>(sectorsFor(len)));
    if (header_.chunkCount) header_.chunkCount -= 1;
    header_.contentHash = 0;
    return flushMetadata(why);
}

bool RegionWriter::readAllChunks(std::vector<std::pair<ChunkLocal, ChunkPayload>>& out, std::string* why) {
    for (const Group& g : groups_) {
        for (u32 slot = 0; slot < kChunksPerGroup; ++slot) {
            ByteReader r(g.raw.data() + static_cast<usize>(slot) * kGroupDirEntryBytes, kGroupDirEntryBytes);
            const u32 first = r.u32v(), len = r.u32v(), crc = r.u32v();
            if (first == 0) continue;
            std::vector<u8> raw(len);
            const ChunkLocal l = localOfGroupSlot(g.index, slot);
            if (len && !file_.readAt(u64(first) * kRegionSectorBytes, raw.data(), raw.size())) {
                if (why) *why = chunkName(l) + ": payload is truncated";
                return false;
            }
            if (fmt::avrCrc32c(raw.data(), raw.size()) != crc) {
                if (why) *why = chunkName(l) + ": failed its payload checksum";
                return false;
            }
            ChunkPayload p;
            p.coord = chunkOf(header_.coord, l);
            p.chunkSizeCm = static_cast<i32>(header_.chunkSizeCm);
            if (!decodeChunk(raw.data(), raw.size(), p, why)) return false;
            out.emplace_back(l, std::move(p));
        }
    }
    return true;
}

bool RegionWriter::compact(std::string* why) {
    if (!file_.isOpen()) { if (why) *why = "region is not open for writing"; return false; }

    std::vector<std::pair<ChunkLocal, ChunkPayload>> all;
    if (!readAllChunks(all, why)) return false;

    RegionWriteDesc d = desc_;
    d.coord = header_.coord;
    d.chunkSizeCm = static_cast<i32>(header_.chunkSizeCm);
    d.levelId = header_.levelId;
    d.worldSeed = header_.worldSeed;
    d.generatorVersion = header_.generatorVersion;
    // Serial 1, exactly as a fresh cook produces: a compacted file IS a fresh cook, and carrying the
    // mutation history in the serial would make it byte-different from one for no reason.
    d.serial = 1;

    const std::string path = path_;
    // Through a temp file and a rename, so a crash during compaction leaves the ORIGINAL intact --
    // compaction is the one operation that touches every byte, and doing it in place would be the
    // riskiest write in the format.
    const std::string tmp = path + ".compact";
    file_.close();
    if (!writeRegion(tmp, d, std::move(all), why)) { deleteFile(tmp); return false; }
    if (!renameFile(tmp, path)) {
        deleteFile(tmp);
        if (why) *why = "could not replace " + path + " with its compacted form";
        return false;
    }
    return open(path, d, why);
}

} // namespace aver::world

#endif // AVER_MODULE_SCENE
