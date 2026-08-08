// Runtime writes: the sector allocator, the free list, crash safety and compaction.
// Exit code = failure count.
//
// THE PROPERTY THAT MATTERS is not "a truncated region opens". It is that a truncated region NEVER
// RETURNS WRONG DATA -- it either opens and every chunk it serves is correct, or it refuses. That is
// checked at EVERY sector boundary rather than a sampled few, because a crash does not sample.
//
// Compaction is checked against a FRESH COOK, not against another mutated file. Two cooks of the same
// content are byte-identical; two files that reached the same content by different sequences of
// writes are not, because sector allocation depends on history. Compaction is the map back onto the
// one canonical form, so a fresh cook is the only thing worth comparing it to.
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/world/ChunkCodec.hpp"
#include "aver/world/RegionFile.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
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
static void checkWhy(bool cond, const std::string& what) { check(cond, cond ? what : what + ": " + why); }

static ChunkPayload makePayload(u32 n, u32 salt) {
    ChunkPayload p;
    for (u32 i = 0; i < n; ++i) {
        PayloadEntity e;
        e.name = "e_" + std::to_string(salt) + "_" + std::to_string(i);
        e.objectId = salt * 1000ull + i;
        e.parent = i == 0 ? -1 : 0;
        e.local.position = Vec3{static_cast<f32>(salt), static_cast<f32>(i), 0.5f};
        e.hasMesh = true;
        e.mesh = salt * 7ull + i;
        e.material = "M_Floor";
        p.entities.push_back(std::move(e));
    }
    return p;
}

static bool same(const ChunkPayload& a, const ChunkPayload& b) {
    if (a.entities.size() != b.entities.size()) return false;
    for (usize i = 0; i < a.entities.size(); ++i) {
        if (a.entities[i].name != b.entities[i].name) return false;
        if (a.entities[i].objectId != b.entities[i].objectId) return false;
        if (a.entities[i].mesh != b.entities[i].mesh) return false;
        if (a.entities[i].local.position.y != b.entities[i].local.position.y) return false;
    }
    return true;
}

static std::vector<u8> readWhole(const std::string& p) {
    std::vector<u8> b;
    readFileBytes(p, b);
    return b;
}

int main() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aver-regionwrite-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::string path = (dir / "live.avrgn").string();

    RegionWriteDesc desc;
    desc.coord = RegionCoord{1, -2, 0};
    desc.chunkSizeCm = kDefaultChunkSizeCm;
    desc.levelId = 0xAA55;

    // The content the file should end up holding, kept alongside so every read can be checked
    // against what was actually written rather than against itself.
    std::map<u32, ChunkPayload> expect;   // packed local -> payload
    const auto key = [](const ChunkLocal& l) { return packLocal(l); };

    // ---- create and grow ----------------------------------------------------------------------------
    {
        RegionWriter w;
        checkWhy(w.open(path, desc, &why), "a region that does not exist is created on open");
        check(w.header().chunkCount == 0, "and starts empty");
        check(w.header().serial >= 1, "with a serial");

        // Chunks in several groups, including ones that force a NEW group directory and therefore a
        // longer group table -- the case v1's fixed layout could not express.
        const ChunkLocal ls[] = {
            ChunkLocal{0, 0, 0}, ChunkLocal{1, 0, 0}, ChunkLocal{17, 0, 0},
            ChunkLocal{-512, -512, -512}, ChunkLocal{511, 511, 511}, ChunkLocal{-1, -1, -1},
        };
        u32 salt = 1;
        for (const ChunkLocal& l : ls) {
            ChunkPayload p = makePayload(2 + salt % 4, salt);
            checkWhy(w.writeChunk(l, p, &why), "a chunk writes into the live region");
            expect[key(l)] = p;
            ++salt;
        }
        check(w.header().chunkCount == 6, "the header counts all six");
        check(w.header().groupCount >= 4, "and they created several group directories");

        // Every one reads back through the ordinary reader, from the same open file.
        w.close();
        RegionFile r;
        checkWhy(r.open(path, &why), "the mutated region opens for reading");
        u32 good = 0;
        for (const auto& kv : expect) {
            ChunkPayload got;
            const ChunkLocal l = unpackLocal(kv.first);
            if (r.readChunk(l, got, &why) && same(kv.second, got)) ++good;
        }
        check(good == expect.size(), "every chunk written at runtime reads back correctly");
    }

    // ---- replace, and reuse the freed space ----------------------------------------------------------
    {
        RegionWriter w;
        checkWhy(w.open(path, desc, &why), "the region reopens");
        const u32 sectorsBefore = w.header().sectorCount;
        check(w.freeSectors() == 0, "a freshly grown region has no holes yet");

        // Replace a chunk with a SMALLER payload: the old sectors go to the free list.
        const ChunkLocal l{511, 511, 511};
        ChunkPayload small = makePayload(1, 99);
        checkWhy(w.writeChunk(l, small, &why), "a chunk is replaced with a smaller payload");
        expect[key(l)] = small;
        check(w.freeSectors() > 0, "...and its old sectors were freed");

        // The next write should REUSE that hole rather than growing the file.
        const ChunkLocal l2{2, 0, 0};
        ChunkPayload p = makePayload(2, 42);
        checkWhy(w.writeChunk(l2, p, &why), "a new chunk is written");
        expect[key(l2)] = p;
        check(w.header().sectorCount <= sectorsBefore + kGroupDirSectors + 2,
              "the file barely grew -- the freed sectors were REUSED, not abandoned");

        // Removal frees too.
        const ChunkLocal gone{1, 0, 0};
        checkWhy(w.removeChunk(gone, &why), "a chunk is removed");
        expect.erase(key(gone));
        check(w.header().chunkCount == expect.size(), "the header count follows the removal");
        check(!w.removeChunk(gone, &why), "removing it twice fails rather than double-freeing");

        w.close();
        RegionFile r;
        r.open(path, &why);
        ChunkPayload got;
        check(!r.readChunk(gone, got, &why), "the removed chunk is gone from the reader too");
        u32 good = 0;
        for (const auto& kv : expect)
            if (r.readChunk(unpackLocal(kv.first), got, &why) && same(kv.second, got)) ++good;
        check(good == expect.size(), "and everything else still reads correctly after the churn");
    }

    // ---- alternating headers -------------------------------------------------------------------------
    {
        RegionWriter w;
        w.open(path, desc, &why);
        const u64 s0 = w.header().serial;
        w.writeChunk(ChunkLocal{3, 0, 0}, makePayload(2, 7), &why);
        expect[key(ChunkLocal{3, 0, 0})] = makePayload(2, 7);
        const u64 s1 = w.header().serial;
        w.writeChunk(ChunkLocal{4, 0, 0}, makePayload(2, 8), &why);
        expect[key(ChunkLocal{4, 0, 0})] = makePayload(2, 8);
        const u64 s2 = w.header().serial;
        check(s1 > s0 && s2 > s1, "every durable write advances the serial");
        w.close();

        // The two copies must differ: that is the whole point of writing to the one that is not live.
        std::vector<u8> bytes = readWhole(path);
        check(bytes.size() >= 2 * kRegionSectorBytes, "the file has both header sectors");
        const bool differ = std::memcmp(bytes.data(), bytes.data() + kRegionSectorBytes, 96) != 0;
        check(differ, "the two header copies differ -- writes alternate rather than overwriting the live one");
    }

    // ---- COMPACTION EQUALS A FRESH COOK ---------------------------------------------------------------
    {
        // What the file holds now, read out so a fresh cook can be built from exactly the same set.
        std::vector<std::pair<ChunkLocal, ChunkPayload>> contents;
        {
            RegionFile r;
            checkWhy(r.open(path, &why), "the churned region opens");
            for (const ChunkLocal& l : r.chunks()) {
                ChunkPayload p;
                if (r.readChunk(l, p, &why)) contents.emplace_back(l, p);
            }
        }
        check(contents.size() == expect.size(), "it holds exactly what was written");

        const std::string cooked = (dir / "cooked.avrgn").string();
        RegionWriteDesc d = desc;
        d.serial = 1;
        checkWhy(writeRegion(cooked, d, contents, &why), "a fresh cook of the same content succeeds");

        RegionWriter w;
        checkWhy(w.open(path, desc, &why), "the churned region opens for compaction");
        const u32 before = w.header().sectorCount;
        checkWhy(w.compact(&why), "it compacts");
        const u32 after = w.header().sectorCount;
        w.close();

        check(after <= before, "compaction did not make the file bigger");
        const std::vector<u8> a = readWhole(path), b = readWhole(cooked);
        check(a.size() == b.size(), "the compacted file is the same size as a fresh cook");
        check(a == b, "COMPACTION EQUALS A FRESH COOK, byte for byte");
        AVER_INFO("   note  compaction: {} sectors -> {} ({} bytes)", before, after, a.size());
    }

    // ---- crash safety at EVERY sector boundary -------------------------------------------------------
    //
    // Truncate at every multiple of the sector size and demand the same thing each time: either the
    // region refuses to open, or every chunk it serves is CORRECT. A file that opens and hands back
    // something plausible and wrong is the failure this format's checksums exist to make impossible.
    {
        const std::vector<u8> whole = readWhole(path);
        const u32 sectors = static_cast<u32>(whole.size() / kRegionSectorBytes);
        check(sectors >= 4, "the compacted region spans several sectors to cut at");

        // What a healthy file serves, to compare truncated reads against.
        std::map<u32, ChunkPayload> truth;
        {
            RegionFile r;
            r.open(path, &why);
            for (const ChunkLocal& l : r.chunks()) {
                ChunkPayload p;
                if (r.readChunk(l, p, &why)) truth[packLocal(l)] = p;
            }
        }
        check(!truth.empty(), "and it serves chunks when healthy");

        const std::string cut = (dir / "cut.avrgn").string();
        u32 opened = 0, refused = 0, servedWrong = 0, chunksServed = 0;
        for (u32 k = 0; k <= sectors; ++k) {
            {
                aver::File f;
                if (!f.open(cut, aver::File::Mode::Create)) continue;
                f.setSize(0);
                if (k) f.writeAt(0, whole.data(), static_cast<usize>(k) * kRegionSectorBytes);
                f.sync();
            }
            RegionFile r;
            std::string w2;
            if (!r.open(cut, &w2)) { ++refused; continue; }
            ++opened;
            for (const ChunkLocal& l : r.chunks()) {
                ChunkPayload got;
                if (!r.readChunk(l, got, &w2)) continue;   // refusing is always acceptable
                ++chunksServed;
                const auto it = truth.find(packLocal(l));
                // Served something that is not what the healthy file has: the failure that must
                // never happen.
                if (it == truth.end() || !same(it->second, got)) ++servedWrong;
            }
        }
        check(opened + refused == sectors + 1, "every truncation was tried");
        check(refused > 0, "the shortest truncations are refused outright");
        check(opened > 0, "and the longest still open");
        check(servedWrong == 0,
              "AT NO TRUNCATION did the region serve a chunk that differs from the healthy file");
        AVER_INFO("   note  {} truncations: {} opened, {} refused, {} chunks served, {} wrong",
                  sectors + 1, opened, refused, chunksServed, servedWrong);
    }

    // ---- a torn header still recovers on the mutated file ---------------------------------------------
    {
        const std::string torn = (dir / "torn.avrgn").string();
        std::filesystem::copy_file(path, torn, std::filesystem::copy_options::overwrite_existing, ec);
        RegionWriter w;
        checkWhy(w.open(torn, desc, &why), "the copy opens for writing");
        checkWhy(w.writeChunk(ChunkLocal{9, 9, 9}, makePayload(3, 77), &why), "a chunk is written");
        const u64 live = w.header().serial;
        w.close();

        // Destroy whichever copy the last write went to, as a crash during that write would.
        const u64 target = (live % 2 == 1) ? 0 : u64(kRegionSectorBytes);
        {
            aver::File f;
            f.open(torn, aver::File::Mode::ReadWrite);
            std::vector<u8> rubbish(128, 0x5A);
            f.writeAt(target, rubbish.data(), rubbish.size());
            f.sync();
        }
        RegionFile r;
        checkWhy(r.open(torn, &why), "the region recovers onto the other header copy");
        check(r.header().serial == live - 1, "...which is exactly one write behind");
        ChunkPayload got;
        // The chunk from the lost write may or may not be visible -- the directory was patched before
        // the header. What must hold is that anything served is CORRECT.
        if (r.readChunk(ChunkLocal{9, 9, 9}, got, &why))
            check(same(makePayload(3, 77), got), "if the in-flight chunk is visible at all, it is intact");
        else
            check(true, "the in-flight chunk is simply absent, which is the other acceptable outcome");
    }

    std::filesystem::remove_all(dir, ec);

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return g_failures;
}
