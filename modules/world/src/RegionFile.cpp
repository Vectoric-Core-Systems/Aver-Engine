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
    // 4+4+4+4 +12 +8+8 +4+4+4+4 +8+8 = 76... padded out to the covered length so the CRC always
    // spans the same span whatever fields are added below it in a later version.
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
        if (!file_.readAt(2ull * kRegionSectorBytes, gt.data(), gt.size())) {
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

} // namespace aver::world

#endif // AVER_MODULE_SCENE
