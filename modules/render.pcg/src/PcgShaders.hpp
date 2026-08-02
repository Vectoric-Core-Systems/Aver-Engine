// The density-volume compute shader.
//
// MIRRORED FUNCTION FOR FUNCTION by PcgVolume.cpp, and by Aver.Pcg's sampleDensity in F#. Three
// implementations of one function is two too many to keep in step by hope, which is why
// PcgVolumeTest compares this against the C++ one voxel by voxel and fails on a single differing
// bit. The atmosphere model in this repo is kept honest the same way.
//
// EVERY INTEGER OPERATION HERE IS 32-BIT UNSIGNED AND WRAPS. That is true in HLSL, in C++ and in F#
// for uint32, which is what makes a bit-exact comparison between them possible at all -- and it is
// why the hash is splitmix32 written out rather than anything from a library.
#pragma once

namespace aver::pcg {

inline constexpr const char* kPcgVolumeHLSL = R"HLSL(
cbuffer PcgVolumeCB : register(b0) {
    uint4  gRes;         // xyz resolution, w layer count
    int4   gSeedFloor;   // x seed, y unused, zw unused
    float4 gCoverage;    // x floor, y bias
    // One float4 per layer: x frequency, y amplitude, z lacunarity, w gain.
    float4 gLayerA[4];
    // x octaves, y seed offset.
    int4   gLayerB[4];
};

RWTexture3D<float> gVolume : register(u0);

// splitmix32. The literals are the contract: change one and the F#, C++ and HLSL sides diverge
// silently, producing three different worlds from one seed.
uint pcgHash(uint x) {
    uint z = x + 0x9E3779B9u;
    z = (z ^ (z >> 16)) * 0x21F0AAADu;
    z = (z ^ (z >> 15)) * 0x735A2D97u;
    return z ^ (z >> 15);
}

uint pcgHash3(int seed, int x, int y, int z) {
    return pcgHash(uint(seed) ^ pcgHash(uint(x) ^ pcgHash(uint(y) ^ pcgHash(uint(z)))));
}

// The TOP 24 bits over 2^24. float32 has a 24-bit mantissa, so every value is exactly
// representable; using the low bits of a multiply-based hash would sample its weakest ones.
float pcgFloat01(uint h) { return float(h >> 8) / 16777216.0; }

float pcgDensity(uint3 p) {
    float total = 0.0, norm = 0.0;
    uint layers = min(gRes.w, 4u);
    for (uint l = 0; l < layers; ++l) {
        float amp  = gLayerA[l].y;
        float freq = gLayerA[l].x;
        int octaves = max(gLayerB[l].x, 1);
        for (int o = 0; o < octaves; ++o) {
            // TRUNCATION toward zero, matching (int) in C++ and `int` in F#. A floor() here would
            // differ from all three for negative operands, which this cannot produce today but
            // would the moment a volume is centred on the origin.
            int cx = (int)(float(p.x) * freq / float(gRes.x));
            int cy = (int)(float(p.y) * freq / float(gRes.y));
            int cz = (int)(float(p.z) * freq / float(gRes.z));
            float v = pcgFloat01(pcgHash3(gSeedFloor.x + gLayerB[l].y, cx, cy, cz));
            total += v * amp;
            norm  += amp;
            amp  *= gLayerA[l].w;
            freq *= gLayerA[l].z;
        }
    }
    if (norm <= 0.0) return 0.0;
    float d = total / norm;
    if (d < gCoverage.x) return 0.0;
    float remapped = (d - gCoverage.x) / max(1e-6, 1.0 - gCoverage.x);
    return pow(remapped, gCoverage.y);
}

[numthreads(4, 4, 4)]
void CSVolume(uint3 tid : SV_DispatchThreadID) {
    // Guarded: a resolution that is not a multiple of the group size dispatches extra threads, and
    // an unguarded write past the texture is undefined rather than merely wasted.
    if (tid.x >= gRes.x || tid.y >= gRes.y || tid.z >= gRes.z) return;
    gVolume[tid] = pcgDensity(tid);
}
)HLSL";

} // namespace aver::pcg
