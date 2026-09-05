// The .avrgn region archive and its .ocindex. Exit code = failure count.
//
// The properties here are the ones docs/CHUNKS.md section 9 calls baseline-free: they need no
// recorded reference, only arithmetic and the file itself.
//
//   COOK DETERMINISM      write the same content twice, memcmp. If this ever fails, "the content
//                         changed" and "the writer is nondeterministic" become indistinguishable,
//                         and every later staleness check built on contentHash is worthless.
//   ROUND TRIP            payload -> bytes -> file -> bytes -> payload, unchanged.
//   BOUNDARY CONSERVATION every chunk written is readable, none invented, none lost.
//   NAMED CORRUPTION      a damaged payload is refused BY CHUNK. "This region is corrupt" is not
//                         something anyone can act on when a region is 16 km across.
//   TORN-WRITE RECOVERY   one header destroyed, the other still opens the file.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/world/ChunkCodec.hpp"
#include "aver/world/RegionFile.hpp"
#include "aver/world/RegionIndex.hpp"

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::world;

static int g_checks = 0;
static int g_failures = 0;
// The reason string every fallible call here writes into.
static std::string why;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// Runs the call FIRST, then builds the message from `why`.
//
// `check(call(&why), "msg: " + why)` looks equivalent and is NOT: both are arguments to check(), so
// their relative evaluation order is unspecified, and MSVC built the message from the PREVIOUS
// call's `why`. Every assertion still passed -- but two of them reported a completely unrelated
// reason ("a parent index pointing forwards is refused: chunk payload has unread trailing bytes"),
// which is how somebody ends up debugging the wrong function. A diagnostic that lies is worse than
// no diagnostic, so the sequencing is made explicit rather than left to the compiler.
// The reason is appended ONLY on failure, because that is the only case in which it was set: these
// functions leave `why` untouched when they succeed, so showing it on a passing line reports the
// last thing that went wrong somewhere else entirely.
static void checkWhy(bool cond, const std::string& what) { check(cond, cond ? what : what + ": " + why); }

// A payload with `n` entities, seeded from `salt` so different chunks differ.
static ChunkPayload makePayload(u32 n, u32 salt) {
    ChunkPayload p;
    for (u32 i = 0; i < n; ++i) {
        PayloadEntity e;
        e.name = "Meshes/thing_" + std::to_string(salt) + "_" + std::to_string(i) + ".ocmesh";
        e.objectId = 0xABCD'0000'0000'0000ull + salt * 1000 + i;
        e.parent = i == 0 ? -1 : 0;                        // one root, the rest its children
        e.local.position = Vec3{static_cast<f32>(salt) + 0.25f, static_cast<f32>(i) * 3.5f, -7.75f};
        e.local.scale = Vec3{1.0f, 2.0f, 0.5f};
        e.tags = salt ^ i;
        e.hasMesh = (i % 3) != 2;
        if (e.hasMesh) {
            e.mesh = 0x1111'2222'3333'4444ull ^ i;
            e.material = i % 2 ? "M_Wall" : "M_Floor";      // NAMES, never tokens
            e.meshFlags = 1;
            e.aabbMin[0] = -50.0f; e.aabbMax[0] = 50.0f;
        }
        e.hasBody = (i % 4) == 1;
        if (e.hasBody) { e.bodyHalfExtentCm[0] = 100.0f; e.bodyHalfExtentCm[2] = 25.0f; }
        p.entities.push_back(std::move(e));
    }
    return p;
}

static bool samePayload(const ChunkPayload& a, const ChunkPayload& b) {
    if (a.entities.size() != b.entities.size()) return false;
    for (usize i = 0; i < a.entities.size(); ++i) {
        const PayloadEntity& x = a.entities[i];
        const PayloadEntity& y = b.entities[i];
        if (x.name != y.name || x.objectId != y.objectId || x.parent != y.parent) return false;
        if (x.local.position.x != y.local.position.x || x.local.position.y != y.local.position.y ||
            x.local.position.z != y.local.position.z) return false;
        if (x.local.scale.x != y.local.scale.x) return false;
        if (x.local.rotation.w != y.local.rotation.w) return false;
        if (x.tags != y.tags) return false;
        if (x.hasMesh != y.hasMesh || x.hasBody != y.hasBody) return false;
        if (x.hasMesh && (x.mesh != y.mesh || x.material != y.material || x.meshFlags != y.meshFlags ||
                          x.aabbMin[0] != y.aabbMin[0] || x.aabbMax[0] != y.aabbMax[0])) return false;
        if (x.hasBody && x.bodyHalfExtentCm[0] != y.bodyHalfExtentCm[0]) return false;
    }
    return true;
}

static std::vector<u8> readWhole(const std::string& p) {
    std::vector<u8> b;
    readFileBytes(p, b);
    return b;
}

int main() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aver-region-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    // ---- the codec on its own ---------------------------------------------------------------------
    {
        const ChunkPayload p = makePayload(6, 3);
        const std::vector<u8> a = encodeChunk(p);
        const std::vector<u8> b = encodeChunk(p);
        check(a == b, "encoding the same payload twice gives identical bytes");

        ChunkPayload back;
        checkWhy(decodeChunk(a.data(), a.size(), back, &why), "it decodes");
        check(samePayload(p, back), "...to exactly what went in");

        ChunkPayload junk;
        check(!decodeChunk(a.data(), a.size() - 1, junk, &why), "a truncated payload is REFUSED");
        check(!decodeChunk(a.data(), 3, junk, &why), "...and so is one too short for its header");

        // Trailing bytes mean writer and reader disagree, which is where "plausible and wrong" lives.
        std::vector<u8> extra = a;
        extra.push_back(0);
        check(!decodeChunk(extra.data(), extra.size(), junk, &why), "trailing bytes are refused");

        // A forward parent reference would build a cycle in an intrusive hierarchy: a hang, not a
        // wrong picture. Rejected at the boundary rather than trusted.
        ChunkPayload bad = p;
        bad.entities[1].parent = 5;
        const std::vector<u8> badBytes = encodeChunk(bad);
        checkWhy(!decodeChunk(badBytes.data(), badBytes.size(), junk, &why),
                 "a parent index pointing forwards is refused");
        check(why.find("parent index") != std::string::npos,
              "...for THAT reason, not incidentally for some other one");
    }

    // ---- cook a region ------------------------------------------------------------------------------
    const std::string pathA = (dir / "r.0.0.0.avrgn").string();
    const std::string pathB = (dir / "r.copy.avrgn").string();

    // Chunks chosen to span several groups AND the extremes of the local range, so the 6+4 bit split
    // is exercised rather than assumed.
    std::vector<std::pair<ChunkLocal, ChunkPayload>> chunks;
    chunks.emplace_back(ChunkLocal{0, 0, 0},          makePayload(4, 1));
    chunks.emplace_back(ChunkLocal{-512, -512, -512}, makePayload(1, 2));   // first slot of first group
    chunks.emplace_back(ChunkLocal{511, 511, 511},    makePayload(7, 3));   // last slot of last group
    chunks.emplace_back(ChunkLocal{-1, -1, -1},       makePayload(3, 4));
    chunks.emplace_back(ChunkLocal{15, 0, 0},         makePayload(2, 5));   // same group as {0,0,0}
    chunks.emplace_back(ChunkLocal{16, 0, 0},         makePayload(2, 6));   // the next group along

    RegionWriteDesc desc;
    desc.coord = RegionCoord{2, -3, 0};
    desc.chunkSizeCm = kDefaultChunkSizeCm;
    desc.levelId = 0xFEED'FACE'CAFE'BEEFull;
    desc.worldSeed = 20260808;
    desc.generatorVersion = 7;
    desc.serial = 1;

    checkWhy(writeRegion(pathA, desc, chunks, &why), "the region cooks");

    // DETERMINISM. Feed the chunks in a DIFFERENT order -- the writer sorts, so the bytes must not
    // care. This is the check that makes contentHash meaningful as a staleness signal.
    {
        std::vector<std::pair<ChunkLocal, ChunkPayload>> shuffled;
        for (auto it = chunks.rbegin(); it != chunks.rend(); ++it) shuffled.push_back(*it);
        checkWhy(writeRegion(pathB, desc, shuffled, &why), "a second cook succeeds");
        const std::vector<u8> a = readWhole(pathA), b = readWhole(pathB);
        check(!a.empty() && a.size() == b.size(), "both cooks are the same size");
        check(a == b, "TWO COOKS OF THE SAME CONTENT ARE BYTE-IDENTICAL, whatever order they arrived in");
    }

    // ---- read it back --------------------------------------------------------------------------------
    {
        RegionFile r;
        checkWhy(r.open(pathA, &why), "the region opens");
        check(r.header().coord == desc.coord, "the header's region coord survived");
        check(r.header().chunkSizeCm == static_cast<u32>(desc.chunkSizeCm), "...and its chunk size");
        check(r.header().levelId == desc.levelId, "...and its level id");
        check(r.header().worldSeed == desc.worldSeed, "...and its world seed");
        check(r.header().generatorVersion == 7, "...and its generator version");
        check(r.header().chunkCount == chunks.size(), "...and the chunk count");
        check(r.header().groupCount == 5, "the six chunks fall into five groups");
        check(r.header().contentHash != 0, "a content hash was computed");

        // BOUNDARY CONSERVATION: everything written is present, nothing else is.
        const std::vector<ChunkLocal> listed = r.chunks();
        check(listed.size() == chunks.size(), "the region lists exactly as many chunks as were written");

        u32 found = 0, matched = 0;
        for (const auto& c : chunks) {
            if (!r.hasChunk(c.first)) continue;
            ++found;
            ChunkPayload got;
            if (!r.readChunk(c.first, got, &why)) { AVER_ERROR("   read failed: {}", why); continue; }
            if (samePayload(c.second, got)) ++matched;
            // The reader reconstructs the GLOBAL chunk coord from the region's own, which is the
            // thing a streamer keys on.
            if (got.coord != chunkOf(desc.coord, c.first)) matched = 0xFFFF;
        }
        check(found == chunks.size(), "every chunk written is reported present");
        check(matched == chunks.size(), "every chunk round-trips byte-for-byte AND reports its global coord");

        // Absence is answered without an error, because "no" is a normal answer here.
        check(!r.hasChunk(ChunkLocal{7, 7, 7}), "a chunk that was never written is reported absent");
        ChunkPayload missing;
        check(!r.readChunk(ChunkLocal{7, 7, 7}, missing, &why), "...and reading it fails");
        check(why.find("(7,7,7)") != std::string::npos, "...naming the chunk that was asked for: " + why);
    }

    // ---- NAMED corruption ------------------------------------------------------------------------------
    {
        const std::string corrupt = (dir / "corrupt.avrgn").string();
        std::filesystem::copy_file(pathA, corrupt, std::filesystem::copy_options::overwrite_existing, ec);

        // Find where {0,0,0}'s payload lives and flip a byte in it. Done through the reader's own
        // view of the file rather than a guessed offset, so the test cannot drift from the layout.
        RegionFile probe;
        check(probe.open(corrupt, &why), "the copy opens");
        ChunkPayload before;
        check(probe.readChunk(ChunkLocal{0, 0, 0}, before, &why), "and reads cleanly before damage");
        probe.close();

        // The payload area begins after both headers, the group table and every group directory, so
        // anything at or past that is payload. Flip a byte well inside it.
        {
            aver::File f;
            check(f.open(corrupt, aver::File::Mode::ReadWrite), "the copy opens for writing");
            const u64 payloadStart = u64(2 + 1 + 5 * kGroupDirSectors) * kRegionSectorBytes;
            u8 b = 0;
            check(f.readAt(payloadStart + 16, &b, 1), "a payload byte is readable");
            b = static_cast<u8>(b ^ 0xFF);
            check(f.writeAt(payloadStart + 16, &b, 1), "and can be flipped");
            check(f.sync(), "and synced");
        }

        RegionFile r;
        check(r.open(corrupt, &why), "a region with a damaged PAYLOAD still opens -- the header is intact");
        u32 refused = 0, refusedByName = 0, stillGood = 0;
        for (const auto& c : chunks) {
            ChunkPayload got;
            std::string w2;
            if (r.readChunk(c.first, got, &w2)) { ++stillGood; continue; }
            ++refused;
            if (w2.find("chunk (") != std::string::npos && w2.find("checksum") != std::string::npos)
                ++refusedByName;
        }
        check(refused >= 1, "at least one chunk is refused");
        check(refused == refusedByName, "EVERY refusal NAMES the chunk and says it failed its checksum");
        check(stillGood == chunks.size() - refused,
              "the undamaged chunks still read -- one bad chunk does not condemn the region");
    }

    // ---- torn-write recovery -----------------------------------------------------------------------------
    {
        const std::string torn = (dir / "torn.avrgn").string();
        std::filesystem::copy_file(pathA, torn, std::filesystem::copy_options::overwrite_existing, ec);
        {
            // Destroy header copy A entirely, as a crash mid-write would.
            aver::File f;
            check(f.open(torn, aver::File::Mode::ReadWrite), "the torn copy opens for writing");
            std::vector<u8> rubbish(128, 0x5A);
            check(f.writeAt(0, rubbish.data(), rubbish.size()), "header A is destroyed");
            check(f.sync(), "and synced");
        }
        RegionFile r;
        checkWhy(r.open(torn, &why), "the region STILL OPENS on header copy B");
        check(r.acceptedHeaderCopy() == 1, "...and reports that it fell back to copy B");
        ChunkPayload got;
        checkWhy(r.readChunk(ChunkLocal{0, 0, 0}, got, &why), "and its chunks still read");
        check(samePayload(chunks[0].second, got), "...unchanged");
    }

    // Both headers gone is a refusal, not a guess.
    {
        const std::string dead = (dir / "dead.avrgn").string();
        std::filesystem::copy_file(pathA, dead, std::filesystem::copy_options::overwrite_existing, ec);
        {
            aver::File f;
            f.open(dead, aver::File::Mode::ReadWrite);
            std::vector<u8> rubbish(2 * kRegionSectorBytes, 0x5A);
            f.writeAt(0, rubbish.data(), rubbish.size());
            f.sync();
        }
        RegionFile r;
        check(!r.open(dead, &why), "a region with both headers destroyed is REFUSED, not guessed at");
        check(why.find("header") != std::string::npos, "...and says so: " + why);
    }

    // ---- the index --------------------------------------------------------------------------------------
    {
        RegionIndex ix;
        ix.levelId = desc.levelId;
        ix.chunkSizeCm = desc.chunkSizeCm;
        ix.worldSeed = desc.worldSeed;
        ix.generatorVersion = desc.generatorVersion;

        // Added out of order on purpose: the index must sort, because find() binary-searches.
        RegionEntry e2; e2.coord = RegionCoord{5, 0, 0};  e2.chunkCount = 2; e2.relativePath = "r.5.0.0.avrgn";
        RegionEntry e1; e1.coord = RegionCoord{2, -3, 0}; e1.chunkCount = 6; e1.relativePath = "r.2.-3.0.avrgn";
        RegionEntry e3; e3.coord = RegionCoord{-1, 4, 2}; e3.chunkCount = 1; e3.relativePath = "r.-1.4.2.avrgn";
        {
            RegionFile r;
            r.open(pathA, &why);
            e1.contentHash = r.header().contentHash;
        }
        check(!ix.add(e2), "adding a new region reports it was new");
        check(!ix.add(e1), "...and another");
        check(!ix.add(e3), "...and a third");
        check(ix.regions.size() == 3, "three regions are listed");
        check(ix.regions[0].coord == RegionCoord{-1, 4, 2}, "and they are sorted, whatever order they arrived");
        check(ix.minRegion == RegionCoord{-1, -3, 0} && ix.maxRegion == RegionCoord{5, 4, 2},
              "the bounds are the per-axis extremes, not any one region's corner");

        RegionEntry dup = e1;
        dup.chunkCount = 99;
        check(ix.add(dup), "re-adding a coord REPLACES rather than duplicating");
        check(ix.regions.size() == 3, "...and the count is unchanged");
        check(ix.find(RegionCoord{2, -3, 0})->chunkCount == 99, "...with the new value");

        const std::string ipath = (dir / "level.ocindex").string();
        checkWhy(writeIndex(ipath, ix, &why), "the index writes");
        check(encodeIndex(ix) == encodeIndex(ix), "encoding it twice gives identical bytes");

        RegionIndex back;
        checkWhy(readIndex(ipath, back, &why), "it reads back");
        check(back.levelId == ix.levelId && back.chunkSizeCm == ix.chunkSizeCm &&
              back.worldSeed == ix.worldSeed && back.generatorVersion == ix.generatorVersion,
              "every header field survived");
        check(back.regions.size() == 3, "every region survived");
        check(back.find(RegionCoord{2, -3, 0}) != nullptr, "and is findable by coord");
        check(back.find(RegionCoord{9, 9, 9}) == nullptr, "while an absent one is not");
        check(back.minRegion == ix.minRegion && back.maxRegion == ix.maxRegion, "the bounds survived");

        // JOB 2: staleness. The index's recorded hash must match the region's own.
        {
            RegionFile r;
            r.open(pathA, &why);
            const RegionEntry* rec = back.find(RegionCoord{2, -3, 0});
            check(rec && rec->contentHash == r.header().contentHash,
                  "the index's contentHash matches the region's own -- the staleness check has teeth");
        }

        // A corrupt index is refused rather than half-read.
        {
            std::vector<u8> bytes = encodeIndex(ix);
            bytes[16] = static_cast<u8>(bytes[16] ^ 0xFF);
            RegionIndex bad;
            checkWhy(!decodeIndex(bytes.data(), bytes.size(), bad, &why), "a damaged index is refused");
            check(why.find("checksum") != std::string::npos, "...by its checksum, before any field is trusted");
        }
    }

    std::filesystem::remove_all(dir, ec);

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
