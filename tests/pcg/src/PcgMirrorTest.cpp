// The C++ half of the PCG density mirror, printed so it can be compared against the F# half.
//
// THREE IMPLEMENTATIONS OF ONE FUNCTION exist: Aver.Pcg's sampleDensity in F#, PcgVolume.cpp's in
// C++, and CSVolume in HLSL. That is two too many to keep in step by hope. This prints the C++
// side for a fixed spec; scripts compare it against the F# side, and the HLSL is compared against
// C++ once the dispatch lands.
#include "aver/pcg/PcgVolume.hpp"
#include "aver/core/Log.hpp"

#include <cstdio>

using namespace aver;

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
    const unsigned probes[][3] = {
        {0,0,0}, {1,2,3}, {31,31,31}, {7,19,2}, {16,16,16}, {30,1,29}, {5,5,5}, {12,0,27},
    };
    std::printf("PcgMirrorTest cpp\n");
    for (const auto& p : probes) {
        const f32 d = pcg::sampleDensity(spec, p[0], p[1], p[2]);
        // %.9g round-trips a float exactly, so the comparison is on the value and not on a printed
        // approximation of it.
        std::printf("  %2u %2u %2u  %.9g\n", p[0], p[1], p[2], static_cast<double>(d));
    }
    return 0;
}
