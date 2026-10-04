// Aver.Occlusion: the per-instance occlusion test.
//
// Moved out of a C++ raw-string literal; composed and -D'd exactly as before -- see the call site.

struct AabbGpu { float3 lo; float pad0; float3 hi; float pad1; };
StructuredBuffer<AabbGpu> gBoxes  : register(t0);
Texture2D<float>          gPyramid : register(t1);
RWStructuredBuffer<uint>  gVisible : register(u0);

cbuffer TestCB : register(b1) {
    float4x4 gViewProj;   // row-major storage, row-vector multiply -- see OcclusionMath.hpp's own note
    uint gCount;
    uint gPyramidW;
    uint gPyramidH;
    uint gMipCount;
    // F7: the scene's own sub-rect of this target, IN TARGET PIXELS -- xy = origin, zw = size --
    // mirroring OcclusionMath.hpp's ViewportRect{x,y,w,h} exactly (OcclusionCuller.cpp packs the
    // same four floats straight into this field). z <= 0.5 stands in for "w <= 0" (a whole-pixel
    // width can never be exactly 0.5, so this is an exact test, not a tolerance) and means "the
    // whole target": no rect arithmetic runs below at all. 16 (gViewProj) + 4 (the uints above) +
    // 4 (this) = 24 dwords total -- OcclusionCuller.cpp's TestCB struct, its setConstants call and
    // its constantDwords[1] must all agree on that count, or this cbuffer reads past what the CPU
    // actually uploaded.
    float4 gViewport;
};

// One corner's contribution to the running screen-space bounds -- a plain function instead of a
// loop-body-with-continue over an indexed array, so nothing here is a local array or a
// dynamically-indexed load: every one of the 8 calls below is fully resolved at compile time to a
// fixed pair of scalar min/max updates, which is the least a driver's own DXIL-to-ISA backend could
// ever have to work out.
void accumulateCorner(float3 pos, float4x4 vp, inout float minX, inout float minY,
                       inout float maxX, inout float maxY, inout float nearestZ, inout bool anyInFront) {
    float4 clip = mul(float4(pos, 1.0), vp);
    if (clip.w <= 1e-5) return;
    anyInFront = true;
    float invW = 1.0 / clip.w;
    float ndcX = clip.x * invW, ndcY = clip.y * invW, ndcZ = clip.z * invW;
    float u = ndcX * 0.5 + 0.5;
    float v = 0.5 - ndcY * 0.5;
    minX = min(minX, u); maxX = max(maxX, u);
    minY = min(minY, v); maxY = max(maxY, v);
    nearestZ = min(nearestZ, ndcZ);
}

[numthreads(64, 1, 1)]
void CSTest(uint3 tid : SV_DispatchThreadID) {
    uint i = tid.x;
    if (i >= gCount) return;
    AabbGpu b = gBoxes[i];

    // ---- projectAabbScreenBounds: the box's 8 corners, spelled out rather than indexed by a bit
    // pattern over a loop variable -- see accumulateCorner's own comment. ----
    float minX = 1e30, minY = 1e30, maxX = -1e30, maxY = -1e30, nearestZ = 1e30;
    bool anyInFront = false;
    accumulateCorner(float3(b.lo.x, b.lo.y, b.lo.z), gViewProj, minX, minY, maxX, maxY, nearestZ, anyInFront);
    accumulateCorner(float3(b.hi.x, b.lo.y, b.lo.z), gViewProj, minX, minY, maxX, maxY, nearestZ, anyInFront);
    accumulateCorner(float3(b.lo.x, b.hi.y, b.lo.z), gViewProj, minX, minY, maxX, maxY, nearestZ, anyInFront);
    accumulateCorner(float3(b.hi.x, b.hi.y, b.lo.z), gViewProj, minX, minY, maxX, maxY, nearestZ, anyInFront);
    accumulateCorner(float3(b.lo.x, b.lo.y, b.hi.z), gViewProj, minX, minY, maxX, maxY, nearestZ, anyInFront);
    accumulateCorner(float3(b.hi.x, b.lo.y, b.hi.z), gViewProj, minX, minY, maxX, maxY, nearestZ, anyInFront);
    accumulateCorner(float3(b.lo.x, b.hi.y, b.hi.z), gViewProj, minX, minY, maxX, maxY, nearestZ, anyInFront);
    accumulateCorner(float3(b.hi.x, b.hi.y, b.hi.z), gViewProj, minX, minY, maxX, maxY, nearestZ, anyInFront);

    if (!anyInFront) { gVisible[i] = 1; return; }
    // Clamped in VIEWPORT space -- a box can only ever be visible somewhere inside the viewport
    // itself, never in whatever the rest of the target texture happens to extend to. This must
    // happen BEFORE the rect mapping below, or a box straddling the viewport's own edge would
    // clamp at the TARGET's edge instead (see OcclusionMath.hpp's matching overload comment).
    minX = max(0.0, minX); minY = max(0.0, minY);
    maxX = min(1.0, maxX); maxY = min(1.0, maxY);
    if (maxX <= minX || maxY <= minY) { gVisible[i] = 1; return; }
    nearestZ = max(0.0, nearestZ);

    // F7: map the viewport-relative [0,1] bounds into this SCENE's own sub-rect of the target,
    // in EXACTLY the same operation order as OcclusionMath.hpp's viewport-relative
    // projectAabbScreenBounds overload (multiply, then add, then divide) -- see that function's
    // own comment for why a divergence here would be a divergence between what SandboxApp.cpp's
    // CPU-side trust gate checked and what this shader actually samples. gViewport.z <= 0.5 means
    // "the whole target" (ViewportRect's own "w <= 0" convention): minX/minY/maxX/maxY are left
    // exactly as the viewport-space clamp above produced them, so a caller with no sub-rect pays
    // no extra arithmetic here and reads the identical texels the pre-F7 shader always did.
    if (gViewport.z > 0.5) {
        minX = (gViewport.x + minX * gViewport.z) / (float)gPyramidW;
        maxX = (gViewport.x + maxX * gViewport.z) / (float)gPyramidW;
        minY = (gViewport.y + minY * gViewport.w) / (float)gPyramidH;
        maxY = (gViewport.y + maxY * gViewport.w) / (float)gPyramidH;
    }

    // ---- selectConservativeMip -- MUST stay bit-for-bit the same decision as OcclusionMath.hpp's
    // CPU copy of this function (its own comment has the full "why 1.0 texel" derivation): mip 0 is
    // only safe when the footprint is at most 1 texel wide, since only then are the two sampled
    // corners guaranteed to be the only texel indices the footprint can touch. ----
    float texelW = (maxX - minX) * (float)gPyramidW;
    float texelH = (maxY - minY) * (float)gPyramidH;
    float largest = max(texelW, texelH);
    uint maxMip = gMipCount - 1;
    uint mip = 0;
    if (largest > 1.0) {
        float mipF = ceil(log2(largest));
        mip = mipF <= 0.0 ? 0u : (uint)mipF;
        if (mip > maxMip) mip = maxMip;
    }

    // ---- conservativelyHidden: sample the 4 corners at `mip`, one plain Load each -- no local
    // array, no dynamic index, no early-out break; the fourth compare's result is simply ANDed in
    // whether or not an earlier one already proved the box visible. ----
    uint mw = max(gPyramidW >> mip, 1u);
    uint mh = max(gPyramidH >> mip, 1u);
    int2 t0 = int2(clamp(minX * (float)mw, 0.0, (float)mw - 1.0), clamp(minY * (float)mh, 0.0, (float)mh - 1.0));
    int2 t1 = int2(clamp(maxX * (float)mw, 0.0, (float)mw - 1.0), clamp(minY * (float)mh, 0.0, (float)mh - 1.0));
    int2 t2 = int2(clamp(minX * (float)mw, 0.0, (float)mw - 1.0), clamp(maxY * (float)mh, 0.0, (float)mh - 1.0));
    int2 t3 = int2(clamp(maxX * (float)mw, 0.0, (float)mw - 1.0), clamp(maxY * (float)mh, 0.0, (float)mh - 1.0));
    bool hidden = (nearestZ > gPyramid.Load(int3(t0, (int)mip)))
               && (nearestZ > gPyramid.Load(int3(t1, (int)mip)))
               && (nearestZ > gPyramid.Load(int3(t2, (int)mip)))
               && (nearestZ > gPyramid.Load(int3(t3, (int)mip)));
    gVisible[i] = hidden ? 0u : 1u;
}
