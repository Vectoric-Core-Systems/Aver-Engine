// The C++ half of the PCG density mirror: printed for comparison against the F# half, AND asserted
// against locked values so C++ drift fails on its own.
//
// THREE IMPLEMENTATIONS OF ONE FUNCTION exist: Aver.Pcg's sampleDensity in F#, PcgVolume.cpp's in
// C++, and CSVolume in HLSL. That is two too many to keep in step by hope.
//
// WHAT THIS PROVES, AND WHAT IT DOES NOT. It proves the C++ implementation still produces the values
// it produced when they were locked: any edit to the hash, the fBm walk, the layer weights or the
// coverage curve fails here immediately. It does NOT prove the F# and HLSL halves still agree --
// that needs all three run together.
//
// The previous version of this file said "scripts compare it against the F# side". No such script
// exists in scripts/, and it never ran. So this was a printer that always returned 0: the mirror
// could drift arbitrarily and nothing would fail. Locking the C++ side is the half that can be done
// in a unit test with no .NET and no GPU; the cross-language comparison remains a manual step, and
// saying so is better than implying otherwise.
#include "aver/pcg/PcgVolume.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <cstdio>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

int main() {
    pcg::VolumeSpec spec;
    spec.resX = spec.resY = spec.resZ = 32;
    spec.seed = 20260802;
    spec.layerCount = 2;
    spec.layers[0] = { 4.0f,  1.0f,  4, 2.0f, 0.5f, 7919 };
    spec.layers[1] = { 11.0f, 0.5f,  3, 2.0f, 0.5f, 104729 };
    spec.coverageFloor = 0.35f;
    spec.coverageBias  = 1.7f;

    // A deterministic scatter of voxels rather than a contiguous block: a block would agree even if
    // one axis were transposed, which is exactly the kind of mirror bug worth catching.
    struct Probe { unsigned x, y, z; f32 expected; };
    // LOCKED from this implementation on 2026-08-03. Regenerate deliberately, never to make a red
    // test go green: a changed value here means the density field moved, and every level built on
    // the old field moved with it.
    static const Probe kProbes[] = {
        { 0,  0,  0, 0.656379461f},
        { 1,  2,  3, 0.198327333f},
        {31, 31, 31, 0.133532733f},
        { 7, 19,  2, 0.161684304f},
        {16, 16, 16, 0.118005879f},
        {30,  1, 29, 0.317053735f},
        { 5,  5,  5, 0.283110708f},
        {12,  0, 27, 0.0381172039f},
    };

    // Printed as well as asserted, so the manual comparison against the F# and HLSL halves still has
    // something to read. %.9g round-trips a float exactly, so what is compared is the value and not
    // a printed approximation of it.
    std::printf("PcgMirrorTest cpp\n");
    for (const Probe& p : kProbes) {
        const f32 d = pcg::sampleDensity(spec, p.x, p.y, p.z);
        std::printf("  %2u %2u %2u  %.9g\n", p.x, p.y, p.z, static_cast<double>(d));
    }

    AVER_INFO("=== density is stable against the locked values ===");
    for (const Probe& p : kProbes) {
        const f32 d = pcg::sampleDensity(spec, p.x, p.y, p.z);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "(%u,%u,%u) = %.9g, locked %.9g",
                      p.x, p.y, p.z, static_cast<double>(d), static_cast<double>(p.expected));
        // EXACT equality, not a tolerance. This is one deterministic integer-hash function evaluated
        // on the same inputs by the same binary; anything but the same bits means the function
        // changed, and a tolerance would hide precisely the drift being watched for.
        check(d == p.expected, buf);
    }

    AVER_INFO("=== the sampler's own invariants ===");
    {
        bool inRange = true;
        for (unsigned z = 0; z < 32; z += 7)
            for (unsigned y = 0; y < 32; y += 5)
                for (unsigned x = 0; x < 32; x += 3) {
                    const f32 d = pcg::sampleDensity(spec, x, y, z);
                    if (!(d >= 0.0f && d <= 1.0f) || std::isnan(d)) inRange = false;
                }
        check(inRange, "every sampled density is within [0,1] and none is NaN");

        // A floor that removes everything must give exactly zero, not a small negative that later
        // scales into a stripe.
        pcg::VolumeSpec solid = spec;
        solid.coverageFloor = 1.0f;
        bool allZero = true;
        for (unsigned i = 0; i < 32; ++i)
            if (pcg::sampleDensity(solid, i, i, i) != 0.0f) allZero = false;
        check(allZero, "a coverage floor of 1 empties the field exactly, with no negative residue");

        // The seed must actually reach the field, or PCGVOLUME's seed parameter is decorative.
        pcg::VolumeSpec other = spec;
        other.seed = spec.seed + 1;
        bool differs = false;
        for (unsigned i = 0; i < 32 && !differs; ++i)
            if (pcg::sampleDensity(other, i, i, i) != pcg::sampleDensity(spec, i, i, i)) differs = true;
        check(differs, "changing the seed changes the field");
    }

    if (g_failures == 0)
        AVER_INFO("=== PCG mirror: the C++ side is stable over {} probes ===",
                  sizeof(kProbes) / sizeof(kProbes[0]));
    else
        AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
