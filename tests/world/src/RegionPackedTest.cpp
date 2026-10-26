// Packed (version 3) regions, index version 2, and the group-directory LRU (LARGE_WORLD 2.4.4). Exit code = failure count.
//
//   ROUND TRIP        every chunk written to a packed region reads back, with its global coord and its extras.
//   COOK DETERMINISM  two cooks, whatever order the chunks arrived in, are byte-identical.
//   SIZE              a handful of small chunks is a few KiB packed, against dozens of sectors at version 2.
//   VERSION 2 INTACT  an unpacked region still writes version 2, flags 0, 4 KiB sectors, and reads as before.
//   IMMUTABLE         RegionWriter refuses a packed region; a packed write cannot be asked of it.
//   NAMED DAMAGE      a flipped payload byte, a short file and a malformed directory are each refused, by name where a chunk
//                     is involved.
//   LRU               the group directories cached per region are capped, a hit costs no read, the eviction is least-recent.
//   INDEX V2          kind, source hash and the wide list round-trip; a version 1 index still reads; unknown kinds are refused.
//   SOURCE            RegionChunkSource serves a packed set through the index and refuses it when the hash is stale.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/Avr1.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/world/ChunkCodec.hpp"
#include "aver/world/ChunkPayloadExtras.hpp"
#include "aver/world/ChunkSource.hpp"
#include "aver/world/RegionFile.hpp"
#include "aver/world/RegionIndex.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::world;

static int g_checks = 0;
static int g_failures = 0;
static std::string why;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}
// The reason is appended only on failure: these calls leave `why` untouched when they succeed.
static void checkWhy(bool cond, const std::string& what) { check(cond, cond ? what : what + ": " + why); }

static ChunkPayload makePayload(u32 n, u32 salt) {
    ChunkPayload p;
    for (u32 i = 0; i < n; ++i) {
        PayloadEntity e;
        e.name = "Meshes/block_" + std::to_string(salt) + "_" + std::to_string(i) + ".ocmesh";
        e.objectId = 0xC0DE'0000'0000'0000ull + salt * 1000 + i;
        e.parent = -1;
        e.local.position = Vec3{static_cast<f32>(salt) + 0.5f, static_cast<f32>(i) * 11.0f, 25.0f};
        e.local.scale = Vec3{1.0f, 1.0f, 3.0f};
        e.tags = salt ^ i;
        e.hasMesh = true;
        e.mesh = 0x5555'0000'0000'0000ull ^ (salt * 31 + i);
        e.material = "M_Concrete";
        e.aabbMin[0] = -50.0f; e.aabbMax[0] = 50.0f;
        PlacementExtras x;
        x.hasPlacementId = true; x.placementId = u64(salt) * 100000 + i;
        x.hasCollide = true; x.collide = (i % 2) == 0;
        if (i % 3 == 0) { x.hasLabel = true; x.label = "label " + std::to_string(salt) + "." + std::to_string(i); }
        std::string w;
        setPlacementExtras(e, x, &w);
        p.entities.push_back(std::move(e));
    }
    return p;
}

// Equality by encoding: the codec is deterministic, so equal bytes is equal payload, extras included.
static bool samePayload(const ChunkPayload& a, const ChunkPayload& b) { return encodeChunk(a) == encodeChunk(b); }

static std::vector<u8> readWhole(const std::string& p) {
    std::vector<u8> b;
    readFileBytes(p, b);
    return b;
}

static u64 sizeOf(const std::string& p) {
    std::error_code ec;
    return static_cast<u64>(std::filesystem::file_size(p, ec));
}

// Where the packed directory of group `g` starts (byte), found through the file's own group table.
static u64 packedDirOffset(const std::string& path, u32 g) {
    const std::vector<u8> b = readWhole(path);
    RegionFile r;
    if (!r.open(path, nullptr)) return 0;
    const u32 groups = r.header().groupCount;
    for (u32 i = 0; i < groups; ++i) {
        const usize at = kPackedBodyStartBytes + static_cast<usize>(i) * 8;
        u32 gi = 0, unit = 0;
        std::memcpy(&gi, b.data() + at, 4);
        std::memcpy(&unit, b.data() + at + 4, 4);
        if (gi == g) return u64(unit) * kRegionUnitBytesPacked;
    }
    return 0;
}

static void patchBytes(const std::string& path, u64 at, const std::vector<u8>& bytes) {
    aver::File f;
    f.open(path, aver::File::Mode::ReadWrite);
    f.writeAt(at, bytes.data(), bytes.size());
    f.sync();
}

int main() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aver-region-packed-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    check(regionFileName(RegionCoord{2, -3, 0}) == "r.2.-3.0.avrgn", "regionFileName spells signed decimals");

    // Chunks spanning several groups and the extremes of the local range.
    std::vector<std::pair<ChunkLocal, ChunkPayload>> chunks;
    chunks.emplace_back(ChunkLocal{0, 0, 0},          makePayload(4, 1));
    chunks.emplace_back(ChunkLocal{-512, -512, -512}, makePayload(1, 2));
    chunks.emplace_back(ChunkLocal{511, 511, 511},    makePayload(7, 3));
    chunks.emplace_back(ChunkLocal{-1, -1, -1},       makePayload(3, 4));
    chunks.emplace_back(ChunkLocal{15, 0, 0},         makePayload(2, 5));   // same group as {0,0,0}, slot 15
    chunks.emplace_back(ChunkLocal{16, 0, 0},         makePayload(2, 6));   // the next group along
    chunks.emplace_back(ChunkLocal{1, 0, 0},          makePayload(5, 7));   // same group as {0,0,0}, slot 1

    RegionWriteDesc desc;
    desc.coord = RegionCoord{-2, 5, 0};
    desc.levelId = 0xFEED'0000'CAFE'0001ull;
    desc.worldSeed = 99;
    desc.generatorVersion = 2;
    desc.packed = true;

    const std::string packedA = (dir / "a.avrgn").string();
    const std::string packedB = (dir / "b.avrgn").string();
    const std::string plain = (dir / "plain.avrgn").string();

    checkWhy(writeRegion(packedA, desc, chunks, &why), "a packed region cooks");
    {
        std::vector<std::pair<ChunkLocal, ChunkPayload>> shuffled(chunks.rbegin(), chunks.rend());
        checkWhy(writeRegion(packedB, desc, shuffled, &why), "a second packed cook succeeds");
        check(readWhole(packedA) == readWhole(packedB), "TWO PACKED COOKS ARE BYTE-IDENTICAL whatever order the chunks arrived in");
    }

    RegionWriteDesc plainDesc = desc;
    plainDesc.packed = false;
    checkWhy(writeRegion(plain, plainDesc, chunks, &why), "the same content cooks unpacked");

    // ---- the packed header and the round trip ----------------------------------------------------------------------------
    {
        RegionFile r;
        checkWhy(r.open(packedA, &why), "the packed region opens");
        const RegionHeader& h = r.header();
        check(r.packed() && h.version == kRegionVersionPacked && h.flags == kRegionFlagPacked &&
              h.sectorBytes == kRegionUnitBytesPacked, "version 3, the packed flag, 16-byte units");
        check(h.chunkCount == chunks.size() && h.groupCount == 5, "seven chunks in five groups");
        check(h.coord == desc.coord && h.levelId == desc.levelId && h.worldSeed == 99 && h.generatorVersion == 2,
              "the descriptor's identity survived");
        check(h.contentHash != 0, "a content hash was computed");
        check(sizeOf(packedA) == u64(h.sectorCount) * kRegionUnitBytesPacked, "the file is exactly sectorCount units long");

        u32 matched = 0;
        for (const auto& c : chunks) {
            ChunkPayload got;
            if (!r.hasChunk(c.first)) continue;
            if (!r.readChunk(c.first, got, &why)) { AVER_ERROR("   read failed: {}", why); continue; }
            if (samePayload(got, c.second) && got.coord == chunkOf(desc.coord, c.first) &&
                got.chunkSizeCm == static_cast<i32>(h.chunkSizeCm)) ++matched;
        }
        check(matched == chunks.size(), "every chunk reads back with its extras and its global coord");

        const std::vector<ChunkLocal> listed = r.chunks();
        check(listed.size() == chunks.size(), "chunks() lists exactly what was written");
        bool inOrder = true, allKnown = true;
        for (usize i = 0; i < listed.size(); ++i) {
            if (i && !(groupIndexOf(listed[i - 1]) < groupIndexOf(listed[i]) ||
                       (groupIndexOf(listed[i - 1]) == groupIndexOf(listed[i]) && slotIndexOf(listed[i - 1]) < slotIndexOf(listed[i]))))
                inOrder = false;
            allKnown = allKnown && r.hasChunk(listed[i]);
        }
        check(inOrder && allKnown, "...sorted by (group, slot), each reported present");

        check(!r.hasChunk(ChunkLocal{7, 7, 7}), "a chunk that was never written is absent (its group too)");
        check(!r.hasChunk(ChunkLocal{2, 0, 0}), "...and so is an unwritten slot of an occupied group");
        ChunkPayload miss;
        check(!r.readChunk(ChunkLocal{2, 0, 0}, miss, &why) && why.find("(2,0,0)") != std::string::npos,
              "reading it fails naming the chunk: " + why);
    }

    // ---- size, against the unpacked layout ---------------------------------------------------------------------------------
    {
        const u64 packedSize = sizeOf(packedA), plainSize = sizeOf(plain);
        AVER_INFO("   packed {} bytes, unpacked {} bytes", packedSize, plainSize);
        check(packedSize < 4 * 4096, "seven small chunks pack into under four 4 KiB pages");
        check(packedSize * 10 < plainSize, "...and the unpacked layout of the same content is over ten times larger");
    }

    // ---- version 2 is untouched ---------------------------------------------------------------------------------------------
    {
        RegionFile r;
        checkWhy(r.open(plain, &why), "the unpacked region opens");
        check(!r.packed() && r.header().version == kRegionVersion && r.header().flags == 0 &&
              r.header().sectorBytes == kRegionSectorBytes, "still version 2, flags 0, 4 KiB sectors");
        u32 matched = 0;
        for (const auto& c : chunks) {
            ChunkPayload got;
            if (r.readChunk(c.first, got, &why) && samePayload(got, c.second)) ++matched;
        }
        check(matched == chunks.size(), "its chunks read back, extras included (the codec is shared)");
    }

    // ---- RegionWriter and packed regions -------------------------------------------------------------------------------------
    {
        RegionWriter w;
        check(!w.open(packedA, plainDesc, &why), "RegionWriter refuses to open a packed region");
        check(why.find("immutable") != std::string::npos, "...saying why: " + why);
        RegionWriter w2;
        check(!w2.open((dir / "new.avrgn").string(), desc, &why), "...and refuses a descriptor asking for a packed one");
        RegionWriter w3;
        checkWhy(w3.open(plain, plainDesc, &why), "an unpacked region still opens for mutation");
        checkWhy(w3.writeChunk(ChunkLocal{9, 9, 9}, makePayload(2, 9), &why), "...and takes a write");
        w3.close();
        RegionFile r;
        r.open(plain, nullptr);
        ChunkPayload got;
        checkWhy(r.readChunk(ChunkLocal{9, 9, 9}, got, &why) && samePayload(got, makePayload(2, 9)), "...which reads back with its extras");
    }

    // ---- an empty packed region -----------------------------------------------------------------------------------------------
    {
        const std::string empty = (dir / "empty.avrgn").string();
        checkWhy(writeRegion(empty, desc, {}, &why), "an empty packed region cooks");
        RegionFile r;
        checkWhy(r.open(empty, &why), "...and opens");
        check(r.header().chunkCount == 0 && r.header().groupCount == 0 && r.chunks().empty() && sizeOf(empty) == kPackedBodyStartBytes,
              "...with no chunks and exactly the two header pages");
    }

    // ---- named damage ------------------------------------------------------------------------------------------------------------
    {
        // A payload byte.
        const std::string flip = (dir / "flip.avrgn").string();
        std::filesystem::copy_file(packedA, flip, std::filesystem::copy_options::overwrite_existing, ec);
        u64 payloadAt = 0;
        {
            RegionFile r;
            r.open(flip, nullptr);
            // The first group in the table is group 0 ({-512,-512,-512}); its directory holds one entry.
            const u64 dir0 = packedDirOffset(flip, groupIndexOf(ChunkLocal{-512, -512, -512}));
            const std::vector<u8> b = readWhole(flip);
            u32 firstUnit = 0;
            std::memcpy(&firstUnit, b.data() + dir0 + kPackedDirHeaderBytes + 4, 4);
            payloadAt = u64(firstUnit) * kRegionUnitBytesPacked;
        }
        check(payloadAt != 0, "(located the first payload through the file's own directory)");
        patchBytes(flip, payloadAt + 12, {0xFF});
        RegionFile r;
        check(r.open(flip, &why), "a damaged payload does not stop the region opening");
        ChunkPayload got;
        check(!r.readChunk(ChunkLocal{-512, -512, -512}, got, &why) && why.find("chunk (") != std::string::npos &&
              why.find("checksum") != std::string::npos, "...and that chunk is refused by name, as a checksum failure: " + why);
        check(r.readChunk(ChunkLocal{511, 511, 511}, got, &why) && samePayload(got, chunks[2].second),
              "...while its neighbours still read");

        // A short file.
        const std::string shortF = (dir / "short.avrgn").string();
        std::vector<u8> b = readWhole(packedA);
        b.resize(b.size() - 64);
        writeFileBytes(shortF, b.data(), b.size());
        RegionFile s;
        check(!s.open(shortF, &why), "a packed region shorter than its header says is refused on open");

        // A directory with two entries claiming the same slot: out of order, so the binary search cannot be trusted.
        const std::string bad = (dir / "bad.avrgn").string();
        std::filesystem::copy_file(packedA, bad, std::filesystem::copy_options::overwrite_existing, ec);
        const u64 dirAt = packedDirOffset(bad, groupIndexOf(ChunkLocal{0, 0, 0}));
        // Entries are slot 0, 1, 15; make the second one slot 0.
        patchBytes(bad, dirAt + kPackedDirHeaderBytes + kPackedDirEntryBytes, {0, 0});
        RegionFile m;
        check(m.open(bad, &why), "a region with one malformed directory still opens");
        ChunkPayload g2;
        check(!m.readChunk(ChunkLocal{0, 0, 0}, g2, &why), "...but reading from that directory is refused");
        check(why.find("malformed") != std::string::npos, "...as malformed: " + why);
        check(m.readChunk(ChunkLocal{16, 0, 0}, g2, &why), "...while other groups read fine");

        // A directory entry pointing past the file.
        const std::string past = (dir / "past.avrgn").string();
        std::filesystem::copy_file(packedA, past, std::filesystem::copy_options::overwrite_existing, ec);
        const u64 dirP = packedDirOffset(past, groupIndexOf(ChunkLocal{16, 0, 0}));
        patchBytes(past, dirP + kPackedDirHeaderBytes + 4, {0xFF, 0xFF, 0xFF, 0x7F});
        RegionFile q;
        q.open(past, nullptr);
        check(!q.readChunk(ChunkLocal{16, 0, 0}, g2, &why) && why.find("malformed") != std::string::npos,
              "an entry pointing outside the file is refused when its directory loads: " + why);

        // Both headers gone is a refusal, as for version 2.
        const std::string dead = (dir / "dead.avrgn").string();
        std::filesystem::copy_file(packedA, dead, std::filesystem::copy_options::overwrite_existing, ec);
        patchBytes(dead, 0, std::vector<u8>(2 * kRegionSectorBytes, 0x5A));
        RegionFile d;
        check(!d.open(dead, &why), "a packed region with both headers destroyed is refused");

        // Torn header A: the packed header falls back to B like version 2.
        const std::string torn = (dir / "torn.avrgn").string();
        std::filesystem::copy_file(packedA, torn, std::filesystem::copy_options::overwrite_existing, ec);
        patchBytes(torn, 0, std::vector<u8>(128, 0x5A));
        RegionFile t;
        checkWhy(t.open(torn, &why) && t.acceptedHeaderCopy() == 1, "header copy B opens a packed region whose copy A is torn");
        check(t.readChunk(ChunkLocal{0, 0, 0}, g2, &why), "...and its chunks read");
    }

    // ---- the group directory LRU ----------------------------------------------------------------------------------------------------
    {
        // 200 chunks, each alone in its own group: x group 0..63, y group 0..3, at the bottom of z.
        std::vector<std::pair<ChunkLocal, ChunkPayload>> many;
        for (int gy = 0; gy < 4; ++gy)
            for (int gx = 0; gx < 64; ++gx) {
                if (many.size() == 200) break;
                many.emplace_back(ChunkLocal{static_cast<i16>(-512 + gx * 16), static_cast<i16>(-512 + gy * 16), -512},
                                  makePayload(1, static_cast<u32>(many.size())));
            }
        check(many.size() == 200, "(built 200 chunks in 200 groups)");

        for (const bool packed : {true, false}) {
            const std::string path = (dir / (packed ? "many.packed.avrgn" : "many.plain.avrgn")).string();
            RegionWriteDesc d = desc;
            d.packed = packed;
            checkWhy(writeRegion(path, d, many, &why), packed ? "the 200-group packed region cooks" : "the 200-group v2 region cooks");
            RegionFile r;
            r.open(path, nullptr);
            check(r.header().groupCount == 200, "200 groups");

            ChunkPayload got;
            for (const auto& c : many) r.readChunk(c.first, got, nullptr);
            check(r.groupDirReads() == 200, "reading every chunk once costs one directory read per group");
            check(r.cachedGroups() == kGroupCacheGroups, "...and the cache never holds more than its cap");

            const u64 reads = r.groupDirReads();
            r.readChunk(many.back().first, got, nullptr);
            check(r.groupDirReads() == reads, "the most recent group is a cache hit: no read");
            r.readChunk(many.front().first, got, nullptr);
            check(r.groupDirReads() == reads + 1, "the oldest group was evicted: one read to bring it back");
            check(r.cachedGroups() == kGroupCacheGroups, "...and the cap still holds");

            // Recency, not insertion order: touch the first group while filling past the cap and it must survive.
            RegionFile r2;
            r2.open(path, nullptr);
            r2.readChunk(many[0].first, got, nullptr);
            for (usize i = 1; i < kGroupCacheGroups; ++i) r2.readChunk(many[i].first, got, nullptr);
            r2.readChunk(many[0].first, got, nullptr);                               // touch: now the most recent
            r2.readChunk(many[kGroupCacheGroups].first, got, nullptr);               // forces one eviction
            const u64 r2reads = r2.groupDirReads();
            r2.readChunk(many[0].first, got, nullptr);
            check(r2.groupDirReads() == r2reads, "a group touched again is not the one evicted (least recently used goes first)");
            r2.readChunk(many[1].first, got, nullptr);
            check(r2.groupDirReads() == r2reads + 1, "...the untouched second group was");

            // Absence costs no directory read.
            const u64 before = r2.groupDirReads();
            r2.hasChunk(ChunkLocal{300, 300, 300});
            check(r2.groupDirReads() == before, "asking for a chunk in a group the region does not have reads nothing");
        }
    }

    // ---- index version 2 --------------------------------------------------------------------------------------------------------------
    {
        RegionIndex ix;
        ix.levelId = 0xAA55;
        ix.chunkSizeCm = 1600;
        ix.worldSeed = 0;
        ix.generatorVersion = 0;
        ix.kind = kIndexKindCooked;
        ix.sourceHash = 0x1122'3344'5566'7788ull;
        RegionEntry e; e.coord = RegionCoord{0, 0, 0}; e.chunkCount = 7; e.relativePath = regionFileName(e.coord); e.contentHash = 5;
        ix.add(e);
        WideItem w1; w1.owner = ChunkCoord{-5, 7, 0};
        w1.minCm[0] = -123'456'789'012ll; w1.minCm[1] = 5; w1.minCm[2] = -300;
        w1.maxCm[0] = 123'456'789'012ll;  w1.maxCm[1] = 9; w1.maxCm[2] = 51'128;
        WideItem w2; w2.owner = ChunkCoord{1, 1, 1}; w2.maxCm[0] = 8000;
        ix.wide = {w1, w2};

        const std::vector<u8> bytes = encodeIndex(ix);
        check(bytes == encodeIndex(ix), "encoding an index twice gives identical bytes");
        RegionIndex back;
        checkWhy(decodeIndex(bytes.data(), bytes.size(), back, &why), "a version 2 index decodes");
        check(back.kind == kIndexKindCooked && back.sourceHash == ix.sourceHash, "kind and source hash survived");
        check(back.wide.size() == 2 && back.wide[0] == w1 && back.wide[1] == w2, "the wide list survived, in order, with i64 extents past 2^32");
        check(back.regions.size() == 1 && back.regions[0].relativePath == "r.0.0.0.avrgn", "the regions survived");
        check(!(w1 == w2), "(WideItem equality distinguishes items)");

        // Unknown kind.
        RegionIndex weird = ix;
        weird.kind = 9;
        const std::vector<u8> wb = encodeIndex(weird);
        RegionIndex rej;
        check(!decodeIndex(wb.data(), wb.size(), rej, &why) && why.find("kind") != std::string::npos,
              "an index of a kind this reader does not know is refused: " + why);

        // A version 1 stream: the old layout, built by hand with a valid CRC.
        struct Le {
            std::vector<u8> b;
            void u32v(u32 v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(v >> (8 * i))); }
            void u64v(u64 v) { for (int i = 0; i < 8; ++i) b.push_back(static_cast<u8>(v >> (8 * i))); }
            void str(const std::string& s) { b.push_back(static_cast<u8>(s.size())); b.push_back(0); b.insert(b.end(), s.begin(), s.end()); }
        } v1;
        v1.u32v(kIndexMagic); v1.u32v(1);
        v1.u64v(77); v1.u32v(1600); v1.u64v(5); v1.u32v(3);
        v1.u32v(1);                                                   // one region
        for (int i = 0; i < 6; ++i) v1.u32v(0);                       // min and max region
        v1.u32v(0); v1.u32v(0); v1.u32v(0); v1.u64v(42); v1.u32v(3); v1.str("r.0.0.0.avrgn");
        v1.u32v(fmt::avrCrc32c(v1.b.data(), v1.b.size()));
        RegionIndex old;
        checkWhy(decodeIndex(v1.b.data(), v1.b.size(), old, &why), "a version 1 index still decodes");
        check(old.levelId == 77 && old.worldSeed == 5 && old.generatorVersion == 3 && old.regions.size() == 1 &&
              old.regions[0].contentHash == 42, "...with its fields");
        check(old.kind == kIndexKindOverrides && old.sourceHash == 0 && old.wide.empty(), "...as an overrides index with no cook fields");

        // On disk.
        const std::string ipath = (dir / "cooked.ocindex").string();
        checkWhy(writeIndex(ipath, ix, &why), "the index writes");
        RegionIndex disk;
        checkWhy(readIndex(ipath, disk, &why) && disk.wide.size() == 2 && disk.sourceHash == ix.sourceHash, "...and reads back from disk");
    }

    // ---- RegionChunkSource over a packed set ------------------------------------------------------------------------------------------
    {
        const std::string setDir = (dir / "set").string();
        std::filesystem::create_directories(setDir, ec);
        RegionWriteDesc d = desc;
        d.coord = RegionCoord{0, 0, 0};
        const std::string rel = regionFileName(d.coord);
        checkWhy(writeRegion(setDir + "/" + rel, d, chunks, &why), "a packed set's region cooks");
        RegionFile hdr;
        hdr.open(setDir + "/" + rel, nullptr);

        RegionIndex ix;
        ix.levelId = d.levelId;
        ix.kind = kIndexKindCooked;
        ix.sourceHash = 123;
        RegionEntry e; e.coord = d.coord; e.chunkCount = hdr.header().chunkCount; e.contentHash = hdr.header().contentHash; e.relativePath = rel;
        ix.add(e);
        checkWhy(writeIndex(setDir + "/world.ocindex", ix, &why), "its index writes");

        RegionChunkSource src;
        checkWhy(src.open(setDir + "/world.ocindex", setDir, &why), "a RegionChunkSource opens the cooked set");
        check(src.index().kind == kIndexKindCooked, "...and sees its kind");
        const ChunkCoord c0 = chunkOf(d.coord, ChunkLocal{0, 0, 0});
        ChunkPayload got;
        check(src.has(c0) && src.load(c0, got, &why) && samePayload(got, chunks[0].second), "it serves a chunk by global coord, extras intact");
        check(!src.has(chunkOf(d.coord, ChunkLocal{3, 3, 3})), "...and reports an absent one absent");

        // A stale index (hash of a different cook) is refused.
        ix.regions[0].contentHash ^= 1;
        writeIndex(setDir + "/world.ocindex", ix, nullptr);
        RegionChunkSource stale;
        stale.open(setDir + "/world.ocindex", setDir, nullptr);
        check(!stale.load(c0, got, &why) && why.find("stale") != std::string::npos, "a region whose hash disagrees with the index is refused as stale: " + why);
    }

    std::filesystem::remove_all(dir, ec);

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
