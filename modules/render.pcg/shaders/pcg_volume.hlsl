// Aver.Render.Pcg: the volume-evaluation compute shader.
//
// Moved out of a C++ raw-string literal. It is compiled the same way it always was -- see the call
// site for which prelude it is composed with -- but it is now a file the shader watcher can see,
// scripts/check-code-unchanged.py can normalise, and a payload can ship.

cbuffer PcgVolumeCB : register(b0) {
    uint4  gRes;         // xyz resolution, w layer count
    int4   gSeedFloor;   // x seed, y unused, zw unused
    float4 gCoverage;    // x floor, y bias
    // One float4 per layer: x frequency, y amplitude, z lacunarity, w gain.
    float4 gLayerA[4];
    // x octaves, y seed offset.
    int4   gLayerB[4];
};

// A STRUCTURED BUFFER and not an RWTexture3D, and the reason is testability rather than taste: the
// RHI has copyBuffer but no copyTexture, so a Tex3D result cannot be read back and compared against
// the CPU mirror at all. A field nobody can check against its reference is the exact thing this
// module exists to avoid. A Tex3D variant, for hardware-filtered sampling in a raymarch, wants a
// texture copy path the RHI does not have yet.
RWStructuredBuffer<float> gVolume : register(u0);

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
    // x-major, matching the CPU mirror's index arithmetic exactly. Transposing this is the classic
    // way to get a field that is "obviously noise" and disagrees everywhere.
    uint idx = tid.z * gRes.y * gRes.x + tid.y * gRes.x + tid.x;
    gVolume[idx] = pcgDensity(tid);
}
