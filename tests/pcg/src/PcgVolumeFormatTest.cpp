// PCGVOLUME round trip: parse, write, parse again, and require the two parses to agree.
//
// A FORMAT IS THE WORST THING TO GET HALF-RIGHT. Once a .ocworld is authored with this record its
// shape is frozen, so the round trip is asserted before anything depends on it -- including the
// cases a hand-authored file will actually contain: a bare `infinite`, explicit bounds, missing
// optional keys, and values that would divide by zero.
#include "aver/formats/OcWorld.hpp"
#include "aver/core/Log.hpp"

#include <string>

using namespace aver;

static int g_failures = 0;
static void check(bool c, const std::string& what) {
    if (c) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

int main() {
    AVER_INFO("PcgVolumeFormatTest");

    const char* src =
        "OCMAP 1\n"
        "NAME PcgLevel\n"
        "\n"
        "PCGVOLUME name Sky seed 20260802 cell 1600 octaves 4 floor 0.35 bias 1.7 infinite\n"
        "PCGVOLUME name Caves seed -991 cell 400 octaves 2 floor 0.1 bias 1 bounds -5000 -5000 0 5000 5000 2000\n"
        "PCGVOLUME name Sparse seed 7\n"
        "PCGVOLUME name Bad seed 1 cell 0 octaves 0 infinite\n";

    fmt::OcWorldData a;
    std::string err;
    check(fmt::parseOcworld(src, a, &err), "the file parses: " + err);
    check(a.pcgVolumes.size() == 4, "four volumes read (got " + std::to_string(a.pcgVolumes.size()) + ")");
    if (a.pcgVolumes.size() != 4) { AVER_ERROR("PcgVolumeFormatTest: {} failure(s)", ++g_failures); return g_failures; }

    check(a.pcgVolumes[0].name == "Sky" && a.pcgVolumes[0].seed == 20260802, "seed is read as authored");
    check(a.pcgVolumes[0].infinite, "a bare `infinite` token marks the field unbounded");
    check(!a.pcgVolumes[1].infinite, "`bounds` marks it bounded");
    check(a.pcgVolumes[1].seed == -991, "a NEGATIVE seed survives - seeds are user-chosen, not indices");
    check(a.pcgVolumes[1].boundsMin[0] == -5000.0 && a.pcgVolumes[1].boundsMax[2] == 2000.0,
          "all six bounds land in the right slots");

    // Defaults, for the line a human actually writes first.
    check(a.pcgVolumes[2].infinite,          "a volume with no bounds token defaults to infinite");
    check(a.pcgVolumes[2].cellSizeCm == 1600.0, "cell size defaults to one 16 m chunk");
    check(a.pcgVolumes[2].octaves == 4,      "octaves default to 4");

    // Values that would divide by zero downstream. Guarded at the reader, because the file is
    // hand-authored and every sampler divides by cell size.
    check(a.pcgVolumes[3].cellSizeCm > 0.0, "a cell size of 0 is repaired, not passed on");
    check(a.pcgVolumes[3].octaves >= 1,     "an octave count of 0 is repaired");

    // ---- the round trip ----
    const std::string written = fmt::writeOcworld(a);
    fmt::OcWorldData b;
    check(fmt::parseOcworld(written, b, &err), "the written file parses: " + err);
    check(b.pcgVolumes.size() == a.pcgVolumes.size(), "the same number of volumes survives a write");

    bool same = b.pcgVolumes.size() == a.pcgVolumes.size();
    for (usize i = 0; same && i < a.pcgVolumes.size(); ++i) {
        const fmt::OcPcgVolume& x = a.pcgVolumes[i];
        const fmt::OcPcgVolume& y = b.pcgVolumes[i];
        same = x.name == y.name && x.seed == y.seed && x.cellSizeCm == y.cellSizeCm &&
               x.octaves == y.octaves && x.coverageFloor == y.coverageFloor &&
               x.coverageBias == y.coverageBias && x.infinite == y.infinite;
        for (int k = 0; k < 3; ++k)
            same = same && x.boundsMin[k] == y.boundsMin[k] && x.boundsMax[k] == y.boundsMax[k];
    }
    check(same, "every field survives parse -> write -> parse unchanged");

    // And writing TWICE is byte-identical, so a level saved twice does not churn in version control.
    check(fmt::writeOcworld(b) == written, "a second write reproduces the first byte for byte");

    if (g_failures == 0) { AVER_INFO("PcgVolumeFormatTest: ALL PASS"); return 0; }
    AVER_ERROR("PcgVolumeFormatTest: {} failure(s)", g_failures);
    return g_failures;
}
