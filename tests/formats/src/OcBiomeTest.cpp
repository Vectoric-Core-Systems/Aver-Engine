// .ocbiome (text biome asset) and the .ocinst per-group cull chunk (ICUL) it feeds. Exit code = failure count.
#include "aver/formats/OcBiome.hpp"
#include "aver/formats/OcInstances.hpp"

#include "aver/core/Log.hpp"

#include <filesystem>
#include <string>

using namespace aver;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("  FAIL  {}", what);
    ++g_failures;
}

static fmt::OcBiomeData fixture() {
    fmt::OcBiomeData b;
    b.name = "Pine Forest";
    b.priority = 2;
    b.mask.kind = fmt::OcBiomeMask::Kind::Layer;
    b.mask.name = "Forest";
    b.mask.min = 0.4f;
    b.mask.edge = 0.3f;
    fmt::OcBiomeEntry tree;
    tree.foliage = "Foliage/Pine.ocfoliage";
    tree.densityPer100m2 = 4.5f;
    tree.slopeMaxDeg = 35.0f;
    tree.heightMinCm = 0.0f; tree.heightMaxCm = 9000.0f;
    tree.layer = "Forest"; tree.layerMin = 0.55f;
    tree.clumpRadiusCm = 600.0f; tree.clumpMin = 3; tree.clumpMax = 6;
    tree.priority = 3;
    tree.pruneRadiusCm = 250.0f;
    fmt::OcBiomeEntry fern;
    fern.foliage = "Foliage/Fern.ocfoliage";
    fern.densityPer100m2 = 30.0f;
    fern.noiseScaleCm = 3000.0f; fern.noiseMin = 0.4f; fern.noiseMax = 0.9f; fern.noiseSeed = 7;
    b.entries = {tree, fern};
    return b;
}

static bool same(const fmt::OcBiomeData& a, const fmt::OcBiomeData& b) {
    if (a.name != b.name || a.priority != b.priority || a.entries.size() != b.entries.size()) return false;
    if (a.mask.kind != b.mask.kind || a.mask.name != b.mask.name || a.mask.min != b.mask.min || a.mask.edge != b.mask.edge ||
        a.mask.invert != b.mask.invert || a.mask.halfWidthCm != b.mask.halfWidthCm || a.mask.noiseSeed != b.mask.noiseSeed)
        return false;
    for (usize i = 0; i < a.entries.size(); ++i) {
        const auto &x = a.entries[i], &y = b.entries[i];
        if (x.foliage != y.foliage || x.densityPer100m2 != y.densityPer100m2 || x.slopeMinDeg != y.slopeMinDeg ||
            x.slopeMaxDeg != y.slopeMaxDeg || x.heightMinCm != y.heightMinCm || x.heightMaxCm != y.heightMaxCm ||
            x.layer != y.layer || x.layerMin != y.layerMin || x.noiseScaleCm != y.noiseScaleCm || x.noiseMin != y.noiseMin ||
            x.noiseMax != y.noiseMax || x.noiseSeed != y.noiseSeed || x.clumpRadiusCm != y.clumpRadiusCm ||
            x.clumpMin != y.clumpMin || x.clumpMax != y.clumpMax || x.priority != y.priority || x.pruneRadiusCm != y.pruneRadiusCm)
            return false;
    }
    return true;
}

int main() {
    AVER_INFO("=== .ocbiome round trip ===");
    {
        const fmt::OcBiomeData src = fixture();
        std::string text, why;
        check(src.valid(&why), "the fixture is valid (" + why + ")");
        check(fmt::writeOcBiome(src, text, &why), "writes");
        fmt::OcBiomeData back;
        check(fmt::parseOcBiome(text, back, &why), "parses back (" + why + ")");
        check(same(src, back), "every field survives");
        std::string text2;
        check(fmt::writeOcBiome(back, text2, &why) && text == text2, "re-writing the parsed biome reproduces the same text");
        check(back.entries[1].clumpRadiusCm == 0.0f && back.entries[1].layer.empty(), "an entry that left the keys out keeps the defaults");

        // Every mask kind, with invert.
        for (auto kind : {fmt::OcBiomeMask::Kind::Noise, fmt::OcBiomeMask::Kind::Spline, fmt::OcBiomeMask::Kind::Volume}) {
            fmt::OcBiomeData m = fixture();
            m.mask = fmt::OcBiomeMask{};
            m.mask.kind = kind;
            m.mask.name = "thing";
            m.mask.edge = 250.0f;
            m.mask.invert = true;
            m.mask.halfWidthCm = 640.0f;
            m.mask.noiseSeed = 5;
            std::string t;
            fmt::OcBiomeData r;
            check(fmt::writeOcBiome(m, t, &why) && fmt::parseOcBiome(t, r, &why) && r.mask.kind == kind && r.mask.invert &&
                      r.mask.edge == 250.0f,
                  "mask kind " + std::to_string(static_cast<int>(kind)) + " round trips (" + why + ")");
        }
    }

    AVER_INFO("=== hand-written text, comments and forward compatibility ===");
    {
        const std::string text =
            "# a hand-written biome\n"
            "OCBIOME 1\r\n"
            "NAME  Meadow  \n"
            "FUTURE_RECORD x=1\n"
            "MASK none\n"
            "ENTRY foliage=Foliage/Grass.ocfoliage density=120 futureKey=3  # trailing comment\n";
        fmt::OcBiomeData b;
        std::string why;
        check(fmt::parseOcBiome(text, b, &why), "parses (" + why + ")");
        check(b.name == "Meadow", "the name is trimmed");
        check(b.entries.size() == 1 && b.entries[0].densityPer100m2 == 120.0f, "the entry is read, the unknown key skipped");
        check(b.mask.kind == fmt::OcBiomeMask::Kind::None, "MASK none");
    }

    AVER_INFO("=== refusals ===");
    {
        fmt::OcBiomeData b;
        std::string why;
        check(!fmt::parseOcBiome("NAME x\n", b, &why), "no header is refused (" + why + ")");
        check(!fmt::parseOcBiome("OCBIOME 9\n", b, &why), "a newer version is refused");
        check(!fmt::parseOcBiome("OCBIOME 1\nENTRY density=3\n", b, &why), "an entry with no foliage is refused (" + why + ")");
        check(!fmt::parseOcBiome("OCBIOME 1\nENTRY foliage=a density=0\n", b, &why), "zero density is refused");
        check(!fmt::parseOcBiome("OCBIOME 1\nENTRY foliage=a density=2 slope=50,10\n", b, &why), "an inverted slope range is refused");
        check(!fmt::parseOcBiome("OCBIOME 1\nENTRY foliage=a density=2 clump=100,5,2\n", b, &why), "clump max below min is refused");
        check(!fmt::parseOcBiome("OCBIOME 1\nMASK layer min=0.5\nENTRY foliage=a density=2\n", b, &why), "a layer mask with no name is refused");
        check(!fmt::parseOcBiome("OCBIOME 1\nENTRY foliage=a density=nan\n", b, &why), "a non-number is refused");
        fmt::OcBiomeData bad = fixture();
        bad.entries[0].densityPer100m2 = -1.0f;
        std::string out;
        check(!fmt::writeOcBiome(bad, out, &why), "writeOcBiome refuses an invalid biome (" + why + ")");
    }

    AVER_INFO("=== file round trip ===");
    {
        const std::string dir = (std::filesystem::temp_directory_path() / "aver-ocbiome-test").string();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::string path = dir + "/Forest.ocbiome";
        std::string why;
        const fmt::OcBiomeData src = fixture();
        check(fmt::saveOcBiome(path, src, &why), "saves (" + why + ")");
        fmt::OcBiomeData back;
        check(fmt::loadOcBiome(path, back, &why) && same(src, back), "loads back equal");
        check(!fmt::loadOcBiome(dir + "/none.ocbiome", back, &why), "a missing file is refused");
        std::filesystem::remove_all(dir, ec);
    }

    AVER_INFO("=== .ocinst ICUL: per-group cull range ===");
    {
        fmt::OcInstanceData d;
        d.transforms.assign(12 * 3, 0.0f);
        for (u32 i = 0; i < 3; ++i) { d.transforms[i * 12 + 0] = d.transforms[i * 12 + 4] = d.transforms[i * 12 + 8] = 1.0f; }
        fmt::OcInstanceGroup a;
        a.asset = "Meshes/a.ocmesh"; a.first = 0; a.count = 2; a.cullStartCm = 1000.0f; a.cullEndCm = 4000.0f; a.lodScaleBias = 2.0f;
        fmt::OcInstanceGroup b;
        b.asset = "Meshes/b.ocmesh"; b.first = 2; b.count = 1;
        d.groups = {a, b};
        std::vector<u8> bytes;
        std::string why;
        check(fmt::writeOcInstances(d, bytes, &why), "writes (" + why + ")");
        fmt::OcInstanceData back;
        check(fmt::parseOcInstances(bytes.data(), bytes.size(), back, &why), "parses (" + why + ")");
        check(back.groups[0].cullStartCm == 1000.0f && back.groups[0].cullEndCm == 4000.0f && back.groups[0].lodScaleBias == 2.0f,
              "the cull range survives");
        check(back.groups[1].cullEndCm == 0.0f && back.groups[1].lodScaleBias == 1.0f, "an uncullable group keeps the defaults");

        fmt::OcInstanceData plain = d;
        for (auto& g : plain.groups) { g.cullStartCm = g.cullEndCm = 0.0f; g.lodScaleBias = 1.0f; }
        std::vector<u8> pbytes;
        check(fmt::writeOcInstances(plain, pbytes, &why) && pbytes.size() < bytes.size(), "a table with no culling writes no ICUL chunk");
        fmt::OcInstanceData pback;
        check(fmt::parseOcInstances(pbytes.data(), pbytes.size(), pback, &why) && pback.groups[0].lodScaleBias == 1.0f,
              "and reads the defaults back (the file as it was before ICUL)");

        // The keep rule.
        const fmt::OcInstanceGroup& g = back.groups[0];
        check(fmt::ocInstanceKeepQuarters(g, 500.0f) == 4, "inside cullStart: everything");
        check(fmt::ocInstanceKeepQuarters(g, 4000.0f) == 0 && fmt::ocInstanceKeepQuarters(g, 9000.0f) == 0, "at and past cullEnd: nothing");
        const u32 mid = fmt::ocInstanceKeepQuarters(g, 2500.0f);
        check(mid >= 1 && mid <= 3, "between: a share");
        check(fmt::ocInstanceKeepQuarters(back.groups[1], 1.0e6f) == 4, "no cullEnd: never culled");
        u32 kept = 0;
        for (u32 i = 0; i < 4000; ++i) kept += fmt::ocInstanceRowKept(i, 2) ? 1u : 0u;
        check(kept > 1700 && kept < 2300, "two quarters keeps about half the rows (" + std::to_string(kept) + " of 4000)");
        bool nested = true;
        for (u32 i = 0; i < 4000; ++i) if (fmt::ocInstanceRowKept(i, 1) && !fmt::ocInstanceRowKept(i, 2)) nested = false;
        check(nested, "a row kept at fewer quarters is kept at more (stable as the viewer moves)");
    }

    AVER_INFO("OcBiomeTest: {} of {} checks passed", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
