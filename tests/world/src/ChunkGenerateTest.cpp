// Generate-as-you-go: determinism, delta-from-baseline, and a CROSS-PROCESS round trip.
// Exit code = failure count.
//
// THE CROSS-PROCESS HALF IS THE POINT, and docs/CHUNKS.md section 10 says why: the five
// process-local values a chunk must never carry -- the material token, a String field's pool index,
// CName offsets, component ids, field ids -- all round-trip PERFECTLY inside the process that minted
// them. A same-process test therefore passes on a file that is unloadable anywhere else. Everything
// before this slice argued the codec cannot carry them; this measures it.
//
// So the test re-executes itself. The child generates the same chunks and COOKS A REGION from them;
// the parent reads that region back and compares it against what it generates itself. Nothing is
// shared but the file.
#include "aver/core/Log.hpp"
#include "aver/formats/Avr1.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/world/ChunkCodec.hpp"
#include "aver/world/ChunkGenerator.hpp"
#include "aver/world/RegionFile.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
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

// The settings both processes must agree on, hardcoded rather than passed, so the child cannot be
// handed something the parent did not use.
static GeneratorSettings fixture() {
    GeneratorSettings s;
    s.worldSeed = 0x5EED'1234'ABCD'0001ull;
    s.chunkSizeCm = kDefaultChunkSizeCm;
    s.samplesPerAxis = 5;
    s.threshold = 0.55f;
    s.featureSizeCm = 1600.0f;
    s.octaves = 3;
    s.material = "M_Foliage";
    return s;
}

// The chunks both processes generate. Negative and positive, several chunks apart.
static std::vector<ChunkCoord> fixtureChunks() {
    return {ChunkCoord{0, 0, 0},   ChunkCoord{1, 0, 0},   ChunkCoord{-1, 0, 0},
            ChunkCoord{-3, -4, 0}, ChunkCoord{7, 11, 0},  ChunkCoord{-9, 2, 0}};
}

// A digest over the ENCODED bytes -- what would actually go to disk, not the in-memory struct.
static u64 digestOf(GeneratedChunkSource& g) {
    std::vector<u8> all;
    for (const ChunkCoord& c : fixtureChunks()) {
        ChunkPayload p;
        g.load(c, p, nullptr);
        const std::vector<u8> b = encodeChunk(p);
        all.insert(all.end(), b.begin(), b.end());
    }
    return all.empty() ? 0 : fmt::avrHash64(all.data(), all.size());
}

// The child: generate, cook a region, write the digest. No assertions -- the parent judges.
static int runChild(const std::string& dir) {
    GeneratedChunkSource g(fixture());
    const u64 d = digestOf(g);

    std::vector<std::pair<ChunkLocal, ChunkPayload>> chunks;
    for (const ChunkCoord& c : fixtureChunks()) {
        ChunkPayload p;
        g.load(c, p, nullptr);
        if (!p.entities.empty()) chunks.emplace_back(localOf(c), p);
    }
    RegionWriteDesc desc;
    desc.coord = regionOf(fixtureChunks().front());
    desc.chunkSizeCm = fixture().chunkSizeCm;
    desc.worldSeed = fixture().worldSeed;
    desc.generatorVersion = fixture().generatorVersion;
    if (!writeRegion(dir + "/child.avrgn", desc, chunks, nullptr)) return 2;

    const std::string text = std::to_string(d) + "\n" + std::to_string(chunks.size()) + "\n";
    return writeFileText(dir + "/child.digest", text) ? 0 : 3;
}

// Is this process the child?
//
// AN ENVIRONMENT VARIABLE, NOT argv, AND THAT IS THE WHOLE SAFETY ARGUMENT. A test that launches
// itself is one mangled command line away from a fork bomb: if the child fails to see its marker it
// becomes a parent, spawns another, and the machine is gone. argv is exactly what a quoting mistake
// eats -- and the first version of this test DID hang, with a path cmd.exe would not take. An
// inherited variable survives every quoting layer between here and the child, so a process that has
// it can never spawn another.
static bool isChild() {
    const char* v = std::getenv("AVER_CHUNKGEN_CHILD");
    return v && v[0] == '1';
}

int main(int argc, char** argv) {
    (void)argc;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aver-chunkgen-test";
    std::error_code ec;

    if (isChild()) return runChild(dir.string());

    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    const GeneratorSettings gs = fixture();
    GeneratedChunkSource gen(gs);

    // ---- generation is a pure function ------------------------------------------------------------
    {
        u32 nonEmpty = 0, totalEntities = 0, partial = 0;
        const u32 perChunk = gs.samplesPerAxis * gs.samplesPerAxis;
        bool localsInRange = true, stable = true;
        for (const ChunkCoord& c : fixtureChunks()) {
            const ChunkPayload a = gen.generate(c);
            const ChunkPayload b = gen.generate(c);
            if (encodeChunk(a) != encodeChunk(b)) stable = false;
            if (!a.entities.empty()) ++nonEmpty;
            if (!a.entities.empty() && a.entities.size() < perChunk) ++partial;
            totalEntities += static_cast<u32>(a.entities.size());
            for (const PayloadEntity& e : a.entities) {
                const f32 lim = static_cast<f32>(gs.chunkSizeCm);
                if (e.local.position.x < 0 || e.local.position.x >= lim ||
                    e.local.position.y < 0 || e.local.position.y >= lim) localsInRange = false;
                if (e.parent != -1) localsInRange = false;   // generated entities are all roots
            }
        }
        check(stable, "generating the same chunk twice gives identical bytes");
        // PARTIAL FILL, which is what catches a feature size so large that every candidate in a
        // chunk shares one noise cell. The first version of this generator produced 25 entities or
        // 0, never anything between, and every other assertion here passed anyway.
        check(partial > 0,
              "at least one chunk is PARTIALLY filled -- the field varies WITHIN a chunk, not just between");
        check(nonEmpty >= 3, "most of the fixture chunks produced content");
        check(totalEntities > 0, "and some entities in total");
        check(localsInRange, "every generated position is chunk-local, in [0, chunkSize)");
        AVER_INFO("   note  {} of {} chunks non-empty, {} partially filled, {} entities ({} candidates/chunk)",
                  nonEmpty, fixtureChunks().size(), partial, totalEntities, perChunk);
    }

    // A different seed must give a different world, or the seed is decorative.
    {
        GeneratorSettings other = gs;
        other.worldSeed ^= 0xFFFFull;
        GeneratedChunkSource g2(other);
        u32 differing = 0;
        for (const ChunkCoord& c : fixtureChunks())
            if (encodeChunk(gen.generate(c)) != encodeChunk(g2.generate(c))) ++differing;
        check(differing >= 3, "a different world seed generates different chunks");
    }

    // Off the surface layer there is nothing, which is what makes has() cheap for most of a world.
    {
        check(gen.generate(ChunkCoord{0, 0, 1}).entities.empty(), "a chunk above the surface is empty");
        check(gen.generate(ChunkCoord{0, 0, -1}).entities.empty(), "and one below it");
        check(!gen.has(ChunkCoord{0, 0, 5}), "has() answers no for them without generating");
    }

    // ---- CROSS-PROCESS -------------------------------------------------------------------------------
    {
        const u64 mine = digestOf(gen);

        // The child is handed NOTHING on its command line: the marker and the directory are both
        // inherited, so there is no quoting for a shell to get wrong. The path is made absolute and
        // native-separated because cmd.exe will not run a "./build/..." forward-slash path.
        std::error_code pec;
        const std::string exe = std::filesystem::absolute(argv[0], pec).make_preferred().string();
#if defined(_WIN32)
        _putenv_s("AVER_CHUNKGEN_CHILD", "1");
#else
        setenv("AVER_CHUNKGEN_CHILD", "1", 1);
#endif
        const int rc = std::system(("\"" + exe + "\"").c_str());
#if defined(_WIN32)
        _putenv_s("AVER_CHUNKGEN_CHILD", "");
#else
        unsetenv("AVER_CHUNKGEN_CHILD");
#endif
        check(rc == 0, "the child process ran and generated its own copy");

        std::string text;
        const bool got = readFileText((dir / "child.digest").string(), text);
        check(got, "the child wrote its digest");
        if (got) {
            const u64 theirs = std::strtoull(text.c_str(), nullptr, 10);
            check(theirs == mine,
                  "SAME SEED AND COORD GENERATE IDENTICALLY IN A SEPARATE PROCESS");
            AVER_INFO("   note  digest {} in both processes", mine);
        }

        // ...and the region the CHILD cooked reads correctly HERE. This is the check docs/CHUNKS.md
        // owed from slice 5: the five process-local values all round-trip perfectly inside the
        // process that minted them, so only a second process can prove the file does not carry one.
        RegionFile r;
        checkWhy(r.open((dir / "child.avrgn").string(), &why), "the region the CHILD wrote opens here");
        u32 matched = 0, checked = 0;
        for (const ChunkCoord& c : fixtureChunks()) {
            ChunkPayload wanted;
            gen.load(c, wanted, nullptr);
            if (wanted.entities.empty()) continue;
            ++checked;
            ChunkPayload got2;
            if (!r.readChunk(localOf(c), got2, &why)) continue;
            if (encodeChunk(wanted) == encodeChunk(got2)) ++matched;
        }
        check(checked > 0, "there were chunks to compare");
        check(matched == checked,
              "EVERY CHUNK THE CHILD WROTE DECODES IDENTICALLY HERE -- no process-local value crossed");

        // Named explicitly, because it is the value most likely to have been the token.
        ChunkPayload one;
        if (r.readChunk(localOf(fixtureChunks().front()), one, &why) && !one.entities.empty())
            check(one.entities.front().material == gs.material,
                  "the surface came back as its NAME across the process boundary, not as a token");
    }

    // ---- delta from baseline --------------------------------------------------------------------------
    {
        const std::string path = (dir / "save.avrgn").string();
        RegionWriteDesc desc;
        desc.coord = regionOf(ChunkCoord{0, 0, 0});
        desc.chunkSizeCm = gs.chunkSizeCm;
        desc.worldSeed = gs.worldSeed;
        desc.generatorVersion = gs.generatorVersion;

        RegionWriter wr;
        checkWhy(wr.open(path, desc, &why), "a save region is created");

        // A chunk exactly as generated must cost NOTHING on disk.
        //
        // CHOSEN, not hardcoded. An earlier version named chunk (1,0,0) and started failing the
        // moment the generator's feature size changed and that chunk came out empty -- a test that
        // breaks when the content it never cared about moves is a test that will be silenced rather
        // than read.
        ChunkCoord c{0, 0, 0};
        ChunkPayload asGenerated;
        for (const ChunkCoord& cand : fixtureChunks()) {
            ChunkPayload p;
            gen.load(cand, p, nullptr);
            if (!p.entities.empty()) { c = cand; asGenerated = p; break; }
        }
        check(!asGenerated.entities.empty(), "a generated chunk with content was found to edit");

        const PersistOutcome o1 = persistChunk(wr, gen, c, asGenerated, &why);
        check(o1 == PersistOutcome::Unchanged, "an UNMODIFIED chunk persists to NOTHING");
        check(wr.header().chunkCount == 0, "...and the save file stays empty");

        // Change it, and it must be stored.
        ChunkPayload edited = asGenerated;
        edited.entities.front().local.position.x += 3.0f;
        const PersistOutcome o2 = persistChunk(wr, gen, c, edited, &why);
        check(o2 == PersistOutcome::Stored, "a MODIFIED chunk is stored");
        check(wr.header().chunkCount == 1, "...as exactly one override");

        // Put it back, and the override must GO -- otherwise a save grows monotonically as a player
        // edits and un-edits, and never shrinks again.
        const PersistOutcome o3 = persistChunk(wr, gen, c, asGenerated, &why);
        check(o3 == PersistOutcome::OverrideRemoved, "reverting an edit REMOVES the override");
        check(wr.header().chunkCount == 0, "...leaving the save empty again");

        // Store it once more, so the layered read below has something to find.
        persistChunk(wr, gen, c, edited, &why);
        wr.close();

        RegionIndex ix;
        ix.levelId = 1;
        ix.chunkSizeCm = gs.chunkSizeCm;
        ix.worldSeed = gs.worldSeed;
        ix.generatorVersion = gs.generatorVersion;
        {
            RegionFile rf;
            rf.open(path, &why);
            RegionEntry e;
            e.coord = desc.coord;
            e.contentHash = rf.header().contentHash;
            e.chunkCount = rf.header().chunkCount;
            e.relativePath = "save.avrgn";
            ix.add(e);
        }
        // contentHash is 0 after a mutation by design -- it only describes a whole region after a
        // compaction -- so the index records 0 and the staleness check has nothing to disagree with.
        check(ix.regions.front().contentHash == 0,
              "a mutated region reports no content hash, rather than a stale one");

        RegionChunkSource overrides;
        writeIndex((dir / "save.ocindex").string(), ix, &why);
        checkWhy(overrides.open((dir / "save.ocindex").string(), dir.string(), &why),
                 "the save opens as an override source");

        LayeredChunkSource layered(&gen, &overrides);
        check(layered.isOverridden(c), "the edited chunk is served by the override");
        check(!layered.isOverridden(ChunkCoord{-3, -4, 0}), "an untouched chunk is not");

        ChunkPayload got;
        checkWhy(layered.load(c, got, &why), "the layered source loads the edited chunk");
        check(encodeChunk(got) == encodeChunk(edited), "...and serves the EDIT, not the generated original");

        ChunkPayload untouched;
        checkWhy(layered.load(ChunkCoord{-3, -4, 0}, untouched, &why), "it loads an untouched chunk");
        ChunkPayload fresh;
        gen.load(ChunkCoord{-3, -4, 0}, fresh, nullptr);
        check(encodeChunk(untouched) == encodeChunk(fresh),
              "...straight from the generator, with nothing on disk behind it");
    }

    // ---- scatter palette --------------------------------------------------------------------------
    {
        // A three-species palette spanning the whole eligible density range between them, so every
        // candidate that clears `threshold` places SOMETHING -- lets the weight-distribution check
        // below reason about "which species" without also reasoning about "did anything place".
        //   rock:  weak end of the field, no clearance check (collisionRadiusCm = 0)
        //   tree:  strong end of the field, LARGE clearance -- this is the interpenetration case
        //   grass: the whole range, tiny weight, so it only wins where rock/tree did not
        GeneratorSettings ps = fixture();
        ps.samplesPerAxis = 6;   // more candidates per chunk than the 5x5 fixture, for better weight stats
        ScatterSpecies rock;
        rock.meshPath = "Meshes/rock.ocmesh";
        rock.material = "M_Rock";
        rock.weight = 3.0f;
        rock.scaleMin = 0.6f;
        rock.scaleMax = 0.9f;
        rock.densityMin = ps.threshold;
        rock.densityMax = 0.75f;
        ScatterSpecies tree;
        tree.meshPath = "Meshes/pine_tree_01.ocmesh";
        tree.material = "M_Bark";
        tree.weight = 2.0f;
        tree.scaleMin = 1.0f;
        tree.scaleMax = 2.0f;
        tree.densityMin = 0.75f;
        tree.densityMax = 1.0f;
        tree.collisionRadiusCm = 300.0f;   // a real footprint -- the interpenetration case
        ScatterSpecies grass;
        grass.meshPath = "Meshes/grass_medium_01.ocmesh";
        grass.material = "M_Foliage";
        grass.weight = 1.0f;
        grass.scaleMin = 0.8f;
        grass.scaleMax = 1.3f;
        grass.densityMin = ps.threshold;
        grass.densityMax = 1.0f;
        ps.palette = {rock, tree, grass};

        // A wide spread of chunks -- many more than the 6-chunk fixture -- so weight ratios and scale
        // ranges have enough samples to be meaningful rather than noise from a handful of entities.
        std::vector<ChunkCoord> many;
        for (i32 x = -12; x <= 12; ++x)
            for (i32 y = -12; y <= 12; ++y) many.push_back(ChunkCoord{x, y, 0});

        GeneratedChunkSource pg(ps);

        u32 rockCount = 0, treeCount = 0, grassCount = 0, otherCount = 0;
        bool scalesInRange = true;
        bool anyTreeOverlap = false;
        u64 totalEntities2 = 0;
        for (const ChunkCoord& c : many) {
            const ChunkPayload p = pg.generate(c);
            totalEntities2 += p.entities.size();
            std::vector<std::pair<f32, f32>> treePositions;   // local x,y of trees in this chunk
            for (const PayloadEntity& e : p.entities) {
                const f32 s = e.local.scale.x;
                if (e.material == "M_Rock") {
                    ++rockCount;
                    if (s < rock.scaleMin - 1e-4f || s > rock.scaleMax + 1e-4f) scalesInRange = false;
                } else if (e.material == "M_Bark") {
                    ++treeCount;
                    if (s < tree.scaleMin - 1e-4f || s > tree.scaleMax + 1e-4f) scalesInRange = false;
                    treePositions.emplace_back(e.local.position.x, e.local.position.y);
                } else if (e.material == "M_Foliage") {
                    ++grassCount;
                    if (s < grass.scaleMin - 1e-4f || s > grass.scaleMax + 1e-4f) scalesInRange = false;
                } else {
                    ++otherCount;
                }
            }
            // Within THIS chunk, no two trees may be closer than the sum of their (scaled) radii --
            // the in-chunk half of requirement 4. Cross-chunk seams are the documented exception.
            for (usize a = 0; a < treePositions.size(); ++a) {
                for (usize b = a + 1; b < treePositions.size(); ++b) {
                    const f32 dx = treePositions[a].first - treePositions[b].first;
                    const f32 dy = treePositions[a].second - treePositions[b].second;
                    // Radii vary with scale (0.6x-1.8x of 300cm); 2*minScale*collisionRadius is a
                    // sound lower bound the true rejection distance can only exceed, so this is a
                    // conservative (never-false-positive) proximity check.
                    const f32 minPossible = 2.0f * tree.collisionRadiusCm * tree.scaleMin;
                    if (std::sqrt(dx * dx + dy * dy) < minPossible - 1.0f) anyTreeOverlap = true;
                }
            }
        }
        check(otherCount == 0, "every placed entity's material matches a palette species");
        check(rockCount > 0 && treeCount > 0 && grassCount > 0, "all three species were placed somewhere");
        check(scalesInRange, "every entity's scale stayed inside its own species' [scaleMin, scaleMax]");
        check(!anyTreeOverlap, "no two trees in the same chunk overlap their (scaled) collision radii");
        // Weight ratio: rock:tree = 3:2 over their SHARED band [threshold, 0.75] is not directly
        // comparable (tree cannot appear there), so compare rock:grass instead -- both are eligible
        // across their bands with grass's weight (1) much lower than rock's (3), so rock should
        // outnumber grass by roughly 3x wherever both could have appeared. This is a loose bound
        // (grass's band is wider than rock's, so grass gets some candidates rock cannot compete for
        // at all), so the check only asks for the right DIRECTION with real margin, not an exact ratio.
        check(rockCount > grassCount,
              "the heavier-weighted species (rock, weight 3) was placed more often than the lighter one "
              "(grass, weight 1) despite grass's band being at least as wide");
        AVER_INFO("   note  palette: {} rock, {} tree, {} grass, {} total entities over {} chunks",
                  rockCount, treeCount, grassCount, totalEntities2, many.size());

        // A palette entry with weight <= 0 must never be chosen.
        {
            GeneratorSettings zw = ps;
            zw.palette[2].weight = 0.0f;   // grass disabled
            GeneratedChunkSource zg(zw);
            bool anyGrass = false;
            for (const ChunkCoord& c : many) {
                for (const PayloadEntity& e : zg.generate(c).entities)
                    if (e.material == "M_Foliage") anyGrass = true;
                if (anyGrass) break;
            }
            check(!anyGrass, "a species with weight <= 0 is never selected");
        }

        // An empty density band (a candidate whose d falls between rock's and tree's bands, in this
        // palette there is none -- rock ends at 0.75, tree starts at 0.75) is covered implicitly by
        // otherCount == 0 above: nothing placed a material outside {M_Rock, M_Bark, M_Foliage}.

        // ---- determinism under a DIFFERENT visitation order ---------------------------------------
        //
        // Requirement 3, proven directly rather than argued: generate the SAME set of chunks twice,
        // once in ascending coordinate order and once reversed, and require every chunk's ENCODED
        // BYTES to match regardless of which order the calls happened in. generate() takes no state
        // from GeneratedChunkSource across calls (resolvedPalette_/spec_ are fixed at setSettings()),
        // so this also stands in for "chunk A generated before or after chunk B" -- the property the
        // scouts flagged as the trap a running counter would fall into.
        {
            std::vector<ChunkCoord> order1 = many;
            std::vector<ChunkCoord> order2 = many;
            std::reverse(order2.begin(), order2.end());

            std::vector<std::vector<u8>> encoded1(order1.size());
            for (usize idx = 0; idx < order1.size(); ++idx)
                encoded1[idx] = encodeChunk(pg.generate(order1[idx]));

            // Reversed pass through a SEPARATE source instance built from the same settings, so any
            // hidden cross-call state in GeneratedChunkSource itself (not just chunk-generation order)
            // would also be caught -- a fresh source generating in reverse order must still agree with
            // the first source's forward-order results, chunk for chunk.
            GeneratedChunkSource pg2(ps);
            std::vector<std::vector<u8>> encoded2(order2.size());
            for (usize idx = 0; idx < order2.size(); ++idx)
                encoded2[idx] = encodeChunk(pg2.generate(order2[idx]));

            bool allMatch = true;
            for (usize idx = 0; idx < order1.size(); ++idx) {
                // order2 is order1 reversed, so order1[idx] == order2[order2.size()-1-idx].
                const usize mirror = order2.size() - 1 - idx;
                if (encoded1[idx] != encoded2[mirror]) { allMatch = false; break; }
            }
            check(allMatch,
                  "every chunk's encoded bytes are identical whether the whole set was generated "
                  "forward or reversed -- species/scale/rejection depend on (seed, coord, salt) only");
        }
    }

    std::filesystem::remove_all(dir, ec);

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return g_failures;
}
