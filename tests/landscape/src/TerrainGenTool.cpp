// TerrainGenTool -- not a test. Writes an .ocland heightfield from fBm noise, so a project can have
// terrain to stand on without one being authored by hand first.
//
// WHY THIS EXISTS. Aver.Landscape has been a complete runtime for a long time -- quadtree LOD,
// skirts, hysteresis, a tested Jolt bridge -- and there was not a single .ocland file anywhere in
// this repository to feed it. The sculpt tools can only reshape a section that already exists, so
// "make some terrain" had no starting point at all. This is that starting point: a base a level can
// declare and an artist can then sculpt.
//
// THE NOISE IS SELF-CONTAINED, deliberately. modules/render.pcg has sampleInfinite, and using it
// here would tie a terrain tool to the render layer for a hash and a lerp. It would also invite the
// assumption that terrain height and the scatter's density field are the same signal, which they are
// not -- density decides WHETHER something is placed, height decides WHERE it sits. Keeping them
// separate is what lets a level put a dense thicket in a valley or leave a bare ridge.
//
//     TerrainGenTool.exe <out.ocland> [samples] [spacingCm] [amplitudeCm] [seed] [featureCm]
//
// Defaults cover a 30000cm square -- the size of the ElectricDreams ground plane it replaces.
#include "aver/formats/OcLand.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

using namespace aver;

namespace {

// A 2D integer hash -> [-1, 1]. Deterministic across runs, platforms and build types: integer ops
// and one divide, no floating-point accumulation whose rounding could differ.
f32 hash2(i32 x, i32 y, u32 seed) {
    u32 h = static_cast<u32>(x) * 374761393u + static_cast<u32>(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<f32>(h & 0xFFFFu) / 32767.5f - 1.0f;
}

// Value noise with a smoothstep fade, which is what keeps the surface C1-continuous -- linear
// interpolation would leave a visible crease along every lattice line once it is lit.
f32 valueNoise(f32 x, f32 y, u32 seed) {
    const f32 fx = std::floor(x), fy = std::floor(y);
    const i32 ix = static_cast<i32>(fx), iy = static_cast<i32>(fy);
    f32 tx = x - fx, ty = y - fy;
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    const f32 a = hash2(ix, iy, seed),     b = hash2(ix + 1, iy, seed);
    const f32 c = hash2(ix, iy + 1, seed), d = hash2(ix + 1, iy + 1, seed);
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

// Ridged fBm. Plain fBm gives rolling dunes; folding each octave through 1-|n| turns the zero
// crossings into creases, which is what reads as eroded ground rather than a bedsheet.
f32 fbm(f32 x, f32 y, u32 seed, int octaves) {
    f32 sum = 0.0f, amp = 0.5f, freq = 1.0f, norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        const f32 n = valueNoise(x * freq, y * freq, seed + static_cast<u32>(o) * 7919u);
        sum  += amp * (1.0f - std::fabs(n));
        norm += amp;
        freq *= 2.0f;
        amp  *= 0.5f;
    }
    return norm > 0.0f ? (sum / norm) * 2.0f - 1.0f : 0.0f;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        AVER_ERROR("usage: TerrainGenTool <out.ocland> [samples] [spacingCm] [amplitudeCm] [seed] [featureCm]");
        return 2;
    }
    const std::string out = argv[1];
    const u32 samples   = argc > 2 ? static_cast<u32>(std::atoi(argv[2])) : 257u;
    const f32 spacing   = argc > 3 ? static_cast<f32>(std::atof(argv[3])) : 117.1875f;
    const f32 amplitude = argc > 4 ? static_cast<f32>(std::atof(argv[4])) : 900.0f;
    const u32 seed      = argc > 5 ? static_cast<u32>(std::atoi(argv[5])) : 20260809u;
    const f32 featureCm = argc > 6 ? static_cast<f32>(std::atof(argv[6])) : 9000.0f;

    if (samples < fmt::kOcLandMinSamples || spacing <= 0.0f || featureCm <= 0.0f) {
        AVER_ERROR("samples must be >= {}, spacing and feature size > 0", fmt::kOcLandMinSamples);
        return 2;
    }

    fmt::OcLandData d;
    d.sampleCount = samples;
    d.spacingCm = spacing;
    // Centred on the world origin, because that is where a level's camera and its PCGVOLUME already
    // are -- terrain that started at the origin and ran +X/+Y would leave three quadrants bare.
    const f32 half = 0.5f * static_cast<f32>(samples - 1) * spacing;
    d.originCm[0] = -half;
    d.originCm[1] = -half;
    d.originCm[2] = 0.0f;
    d.heights.resize(static_cast<usize>(samples) * samples);

    f32 lo = 1e30f, hi = -1e30f;
    for (u32 iy = 0; iy < samples; ++iy) {
        for (u32 ix = 0; ix < samples; ++ix) {
            const f32 wx = d.originCm[0] + static_cast<f32>(ix) * spacing;
            const f32 wy = d.originCm[1] + static_cast<f32>(iy) * spacing;
            const f32 h = fbm(wx / featureCm, wy / featureCm, seed, 5) * amplitude;
            d.heights[static_cast<usize>(iy) * samples + ix] = h;
            if (h < lo) lo = h;
            if (h > hi) hi = h;
        }
    }

    std::string why;
    if (!fmt::saveOcLand(out, d, &why)) {
        AVER_ERROR("could not write '{}': {}", out, why);
        return 1;
    }
    AVER_INFO("wrote {}: {}x{} samples, {:.2f}cm spacing, {:.0f}cm across, height {:.0f}..{:.0f}cm",
              out, samples, samples, spacing,
              static_cast<f32>(samples - 1) * spacing, lo, hi);
    return 0;
}
