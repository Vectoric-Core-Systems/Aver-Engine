// Aver.Render.Skin: the compute skinning dispatch.
//
// Moved out of a C++ raw-string literal. It is compiled the same way it always was -- see the call
// site for which prelude it is composed with -- but it is now a file the shader watcher can see
// and a payload can ship.

struct SkinVertex { float3 pos; float3 nrm; float2 uv; };
struct SkinBind   { uint4  joints; float4 weights; };
struct SkinBone   { float4 r0; float4 r1; float4 r2; float4 r3; };

StructuredBuffer<SkinVertex>   gRest  : register(t0);
StructuredBuffer<SkinBind>     gBind  : register(t1);
StructuredBuffer<SkinBone>     gBones : register(t2);
RWStructuredBuffer<SkinVertex> gOut   : register(u0);

cbuffer SkinParams : register(b3) {
    uint gVertexCount;
    uint gBoneCount;
    uint gSkinPad0;
    uint gSkinPad1;
};

[numthreads(64, 1, 1)]
void CSSkin(uint3 tid : SV_DispatchThreadID) {
    const uint v = tid.x;
    if (v >= gVertexCount) return;

    const SkinVertex r = gRest[v];
    const SkinBind   b = gBind[v];

    float3 p = float3(0, 0, 0);
    float3 n = float3(0, 0, 0);
    float  used = 0.0f;

    [unroll] for (uint i = 0; i < 4; ++i) {
        const float w = b.weights[i];
        const uint  j = b.joints[i];
        // Zero weight and an out-of-range bone are the same case: the influence does not exist.
        if (w == 0.0f || j >= gBoneCount) continue;

        const SkinBone m = gBones[j];
        p += w * (r.pos.x * m.r0 + r.pos.y * m.r1 + r.pos.z * m.r2 + m.r3).xyz;
        n += w * (r.nrm.x * m.r0 + r.nrm.y * m.r1 + r.nrm.z * m.r2).xyz;
        used += w;
    }

    // No surviving influence means the vertex is unrigged, not at the origin.
    if (used == 0.0f) { p = r.pos; n = r.nrm; }

    const float len = length(n);
    if (len > 1e-8f) n /= len;

    SkinVertex o;
    o.pos = p;
    o.nrm = n;
    // Carried through UNTOUCHED, and deliberately so: the uv is the one field skinning must not
    // change, which makes it the instrument that proves C++ and DXC agree about this struct's
    // stride. A disagreement shifts every field after the first and corrupts it visibly.
    o.uv  = r.uv;
    gOut[v] = o;
}
