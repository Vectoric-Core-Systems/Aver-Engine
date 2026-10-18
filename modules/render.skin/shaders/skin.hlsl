// Aver.Render.Skin: the compute skinning dispatch, with blend shapes (MORPH_TARGETS.md 2.7).
//
// Moved out of a C++ raw-string literal. It is compiled the same way it always was -- see the call
// site for which prelude it is composed with -- but it is now a file the shader watcher can see
// and a payload can ship.
//
// ONE FILE FOR D3D12 AND VULKAN: no vendor extensions, no atomics, no wave intrinsics, no 64-bit types.
// Blend shapes are applied to the REST vertex, before skinning, so a morphed face deforms with its bones.
// Per mesh the CPU uploads each target's sparse delta list and a rank bitmap over the vertices (gMorphRank:
// per 32 vertices a {membership bits, bits set in earlier words} pair); per dispatch it uploads the active
// list (gMorphActive, at most 64 targets). A vertex finds its delta in O(1): no inverse index, no scatter.
// render.skin's MorphLayout.cpp is the C++ twin of this lookup.

struct SkinVertex { float3 pos; float3 nrm; float2 uv; };
struct SkinBind   { uint4  joints; float4 weights; };
struct SkinBone   { float4 r0; float4 r1; float4 r2; float4 r3; };
struct MorphDelta  { float3 dp; float3 dn; };
struct MorphActive { uint rankBase; uint deltaBase; float weight; uint pad; };

StructuredBuffer<SkinVertex>   gRest  : register(t0);
StructuredBuffer<SkinBind>     gBind  : register(t1);
StructuredBuffer<SkinBone>     gBones : register(t2);
StructuredBuffer<uint2>        gMorphRank   : register(t3);
StructuredBuffer<MorphDelta>   gMorphDelta  : register(t4);
StructuredBuffer<MorphActive>  gMorphActive : register(t5);
RWStructuredBuffer<SkinVertex> gOut   : register(u0);

// gFlags bit0 = apply the active morph list, bit1 = no skin (a morph-only instance: bind and bones are
// never read, and sit on dummy buffers).
cbuffer SkinParams : register(b3) {
    uint gVertexCount;
    uint gBoneCount;
    uint gFlags;
    uint gActiveCount;
};

[numthreads(64, 1, 1)]
void CSSkin(uint3 tid : SV_DispatchThreadID) {
    const uint v = tid.x;
    if (v >= gVertexCount) return;

    const SkinVertex r = gRest[v];

    // The rest vertex, morphed. The loop count is uniform across the wave.
    float3 p0 = r.pos;
    float3 n0 = r.nrm;
    if ((gFlags & 1u) != 0u) {
        const uint word = v >> 5;
        const uint bit  = 1u << (v & 31u);
        for (uint k = 0; k < gActiveCount; ++k) {
            const MorphActive a = gMorphActive[k];
            const uint2 rw = gMorphRank[a.rankBase + word];
            if ((rw.x & bit) == 0u) continue;
            const MorphDelta d = gMorphDelta[a.deltaBase + rw.y + countbits(rw.x & (bit - 1u))];
            p0 += a.weight * d.dp;
            n0 += a.weight * d.dn;
        }
        const float ml = length(n0);
        if (ml > 1e-8f) n0 /= ml;
    }

    float3 p = p0;
    float3 n = n0;
    if ((gFlags & 2u) == 0u) {
        const SkinBind b = gBind[v];

        p = float3(0, 0, 0);
        n = float3(0, 0, 0);
        float used = 0.0f;

        [unroll] for (uint i = 0; i < 4; ++i) {
            const float w = b.weights[i];
            const uint  j = b.joints[i];
            // Zero weight and an out-of-range bone are the same case: the influence does not exist.
            if (w == 0.0f || j >= gBoneCount) continue;

            const SkinBone m = gBones[j];
            p += w * (p0.x * m.r0 + p0.y * m.r1 + p0.z * m.r2 + m.r3).xyz;
            n += w * (n0.x * m.r0 + n0.y * m.r1 + n0.z * m.r2).xyz;
            used += w;
        }

        // No surviving influence means the vertex is unrigged, not at the origin.
        if (used == 0.0f) { p = p0; n = n0; }

        const float len = length(n);
        if (len > 1e-8f) n /= len;
    }

    SkinVertex o;
    o.pos = p;
    o.nrm = n;
    // Carried through UNTOUCHED, and deliberately so: the uv is the one field skinning must not
    // change, which makes it the instrument that proves C++ and DXC agree about this struct's
    // stride. A disagreement shifts every field after the first and corrupts it visibly.
    o.uv  = r.uv;
    gOut[v] = o;
}
