// voxi_rt.hlsli -- RT lighting estimators; scene/material resources, RayQuery helpers, sampling
// primitives, and estimators. Compiled into voxi.hlsl under AVER_RT guard.

// The single-pass ray-driven shader (AVER_RD_SINGLE_PASS=1) works at its register limit; see its users.
#ifndef AVER_RD_SINGLE_PASS
#define AVER_RD_SINGLE_PASS 0
#endif

// DXR 1.1 inline ray tracing.
RaytracingAccelerationStructure gScene : register(t2);

// Flat geometry for ray hits: 4 descriptors for whole scene.
struct RtVertex   { float3 pos; float3 nrm; float2 uv; };
struct RtInstance { float4x4 objectToWorld; uint firstIndex; uint firstVertex; float3 albedo;
                    float metallic; float roughness; uint materialIndex;
                    float4x4 prevObjectToWorld; };
// prevObjectToWorld: last frame's transform. Feeds ray-driven G-buffer velocity.
StructuredBuffer<RtVertex>   gRtVerts     : register(t3);
StructuredBuffer<uint>       gRtIndices   : register(t4);
StructuredBuffer<RtInstance> gRtInstances : register(t5);

// ---- INSTANCED FOLIAGE: TLAS static prefix instances ----
// Not in gRtInstances; InstanceID is AVER_RT_FOLIAGE_ID_BIT | first row of prototype parts.
// Transform read from gRtFoliageDescs, committed instance desc (static, no motion).
#define AVER_RT_FOLIAGE_ID_BIT 0x800000u
// Packed instance reference: draw's InstanceID (bit 31 clear) or foliage bit 31 | part (27..30) | TLAS index (0..26).
#define AVER_RT_REF_FOLIAGE     0x80000000u
#define AVER_RT_REF_GEOM_SHIFT  27u
#define AVER_RT_REF_INDEX_MASK  0x07FFFFFFu
struct RtFoliageDesc { float4 row0; float4 row1; float4 row2; uint4 tail; };
StructuredBuffer<RtInstance>    gRtFoliageParts : register(t20);
StructuredBuffer<RtFoliageDesc> gRtFoliageDescs : register(t21);

uint rtPackRef(uint instanceId, uint instanceIndex, uint geometryIndex) {
    return (instanceId & AVER_RT_FOLIAGE_ID_BIT) != 0u
        ? (AVER_RT_REF_FOLIAGE | (geometryIndex << AVER_RT_REF_GEOM_SHIFT) | instanceIndex)
        : instanceId;
}
uint rtPackCommitted(inout RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q) {
    return rtPackRef(q.CommittedInstanceID(), q.CommittedInstanceIndex(), q.CommittedGeometryIndex());
}
uint rtPackCandidate(inout RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q) {
    return rtPackRef(q.CandidateInstanceID(), q.CandidateInstanceIndex(), q.CandidateGeometryIndex());
}
// THE ONE INSTANCE LOOKUP: packed reference -> RtInstance. Foliage reconstructs objectToWorld from desc.
RtInstance rtLoadInstance(uint ref) {
    if ((ref & AVER_RT_REF_FOLIAGE) == 0u) return gRtInstances[ref];
    const RtFoliageDesc d = gRtFoliageDescs[ref & AVER_RT_REF_INDEX_MASK];
    RtInstance inst = gRtFoliageParts[(d.tail.x & (AVER_RT_FOLIAGE_ID_BIT - 1u)) +
                                      ((ref >> AVER_RT_REF_GEOM_SHIFT) & 15u)];
    inst.objectToWorld = transpose(float4x4(d.row0, d.row1, d.row2, float4(0.0, 0.0, 0.0, 1.0)));
    // Foliage static, so last frame = this frame.
    inst.prevObjectToWorld = inst.objectToWorld;
    return inst;
}

// ---- Per-material data, keyed by RtInstance::materialIndex ----
#ifdef AVER_RT_BINDLESS
// Ray path's texture array, space1.
#ifndef AVER_RT_TEX_CAPACITY
#error "AVER_RT_TEX_CAPACITY must be defined by the pipeline that declares the bindless table"
#endif
Texture2D gRtTextures[AVER_RT_TEX_CAPACITY] : register(t0, space1);
#define AVER_TEX_UNBOUND 0xFFFFFFFFu

// Ray-differential mip selection: 1 = SampleGrad, 0 = SampleLevel(0).
#ifndef AVER_RT_SAMPLEGRAD
#define AVER_RT_SAMPLEGRAD 1
#endif

#endif

// The material table's row IS the material (material_prelude.hlsl's AverMaterialData, mirroring
// pbr::MaterialConstants): one type, so raster and every ray hit compose a surface from the same value.
typedef AverMaterialData RtMaterial;

#ifdef AVER_RT_BINDLESS
// Measurement ablation switches (0=default; every non-zero value renders deliberately wrong).
#ifndef AVER_RD_ABLATE
#define AVER_RD_ABLATE 0
#endif
#define AVER_RD_ABL_NONE         0
#define AVER_RD_ABL_SHADOW       1
#define AVER_RD_ABL_GI           2
#define AVER_RD_ABL_REFL         3
#define AVER_RD_ABL_SKY          4
#define AVER_RD_ABL_TEX          5
#define AVER_RD_ABL_ALL          6
#define AVER_RD_ABL_SHADOW_FIRSTHIT 7
#define AVER_RD_ABL_SKYOCC       8
#define AVER_RD_ABL_SPECCONE     9
#define AVER_RD_ABL_ROUGHSKY     10
#define AVER_RD_ABL_FOG          11
#define AVER_RD_ABL_AERIAL       12

// Sample one material texture, or fallback if unbound. SampleLevel (fullscreen ray pass,
// implicit derivatives are garbage at silhouettes).
float4 averRtSampleSlot(RtMaterial mat, uint slot, float2 uv, float2 gx, float2 gy, float4 fallback) {
    const uint idx = mat.texIndex[slot];
    if (idx == AVER_TEX_UNBOUND) return fallback;
#if AVER_RD_ABLATE == AVER_RD_ABL_TEX
    return fallback;   // ablated
#endif
#if AVER_RT_SAMPLEGRAD
    return gRtTextures[NonUniformResourceIndex(idx)].SampleGrad(gMaterialSampler, uv, gx, gy);
#else
    return gRtTextures[NonUniformResourceIndex(idx)].SampleLevel(gMaterialSampler, uv, 0);
#endif
}

// Material-graph adapter: the generated ray twin (MaterialGraphHlsl.cpp) samples through this, and is
// declared ahead of it there. The hit's material and footprint travel in statics the caller sets.
static RtMaterial gAverGraphMat;
static float2     gAverGraphGx;
static float2     gAverGraphGy;

float4 averRtSampleSlotGraph(uint slot, float2 uv) {
    return averRtSampleSlot(gAverGraphMat, slot, uv, gAverGraphGx, gAverGraphGy,
                            float4(1.0, 1.0, 1.0, 1.0));
}

// UV-space footprint of one pixel's primary ray for SampleGrad (not implicit ddx/ddy).
// Uses rdRayDx/rdRayDy, neighbour pixels' rays at ray hit's hitT scale.
void averRtUvGrad(RtMaterial mat, RtInstance inst, float3 N,
                  float3 p0, float3 p1, float3 p2,
                  float2 t0, float2 t1, float2 t2,
                  float3 dx, float3 dy,
                  out float2 gx, out float2 gy) {
    gx = 0.0;
    gy = 0.0;

    if (mat.flags & AVER_MAT_WORLD_UV) {
        // World-aligned UV: planar projection.
        const float3 ax = normalize(inst.objectToWorld[0].xyz);
        const float3 ay = normalize(inst.objectToWorld[1].xyz);
        const float3 az = normalize(inst.objectToWorld[2].xyz);
        const float3 a  = abs(float3(dot(N, ax), dot(N, ay), dot(N, az)));
        if (a.z >= a.x && a.z >= a.y) {
            gx = float2(dot(dx, ax), dot(dx, ay));
            gy = float2(dot(dy, ax), dot(dy, ay));
        } else if (a.x >= a.y) {
            gx = float2(dot(dx, ay), dot(dx, az));
            gy = float2(dot(dy, ay), dot(dy, az));
        } else {
            gx = float2(dot(dx, ax), dot(dx, az));
            gy = float2(dot(dy, ax), dot(dy, az));
        }
        gx *= mat.uvTilesPerCm;
        gy *= mat.uvTilesPerCm;
        return;
    }

    // Mesh UV: invert triangle's position-to-UV map. T/B are dP/du, dP/dv, unnormalised.
    const float3 e1 = p1 - p0, e2 = p2 - p0;
    const float2 d1 = t1 - t0, d2 = t2 - t0;
    const float  r  = d1.x * d2.y - d2.x * d1.y;
    if (abs(r) < 1e-12) return;   // Degenerate UVs: mip 0.

    const float3 T = mul(float4((e1 * d2.y - e2 * d1.y) / r, 0.0), inst.objectToWorld).xyz;
    const float3 B = mul(float4((e2 * d1.x - e1 * d2.x) / r, 0.0), inst.objectToWorld).xyz;

    // Dual basis of (T, B, N): rows of the inverse.
    const float3 cbn = cross(B, N);
    const float3 cnt = cross(N, T);
    const float  det = dot(T, cbn);
    if (abs(det) < 1e-12) return;   // T parallel to B.

    gx = float2(dot(cbn, dx), dot(cnt, dx)) / det;
    gy = float2(dot(cbn, dy), dot(cnt, dy)) / det;
}

// Texture coordinate at ray hit: mesh UV or world-aligned planar projection.
float2 averRtSurfaceUV(RtMaterial mat, RtInstance inst, float3 wpos, float3 N, float2 meshUV) {
    if (!(mat.flags & AVER_MAT_WORLD_UV)) return meshUV;
    return averWorldUV(inst.objectToWorld, wpos, N, mat.uvTilesPerCm);   // the prelude's one projection
}

// Tangent frame from triangle positions/UVs (ddx/ddy invalid at ray hit; RtVertex has no tangent).
// Constant across face (no interpolation on curved surfaces).
float3 averRtPerturbNormal(RtMaterial mat, RtInstance inst, float3 N, float3 nTS,
                           float3 p0, float3 p1, float3 p2,
                           float2 t0, float2 t1, float2 t2) {
    float3 T, B;

    if (mat.flags & AVER_MAT_WORLD_UV) {
        // Frame matches world-aligned parametrisation.
        const float3 ax = normalize(inst.objectToWorld[0].xyz);
        const float3 ay = normalize(inst.objectToWorld[1].xyz);
        const float3 az = normalize(inst.objectToWorld[2].xyz);
        const float3 a  = abs(float3(dot(N, ax), dot(N, ay), dot(N, az)));
        if (a.z >= a.x && a.z >= a.y) { T = ax; B = ay; }
        else if (a.x >= a.y)          { T = ay; B = az; }
        else                          { T = ax; B = az; }
    } else {
        const float3 e1 = p1 - p0, e2 = p2 - p0;
        const float2 d1 = t1 - t0, d2 = t2 - t0;
        const float  r  = d1.x * d2.y - d2.x * d1.y;

        // Degenerate UVs are common; avoid NaN.
        if (abs(r) < 1e-12) return N;

        const float3 tObj = (e1 * d2.y - e2 * d1.y) / r;
        const float3 bObj = (e2 * d1.x - e1 * d2.x) / r;
        T = mul(float4(tObj, 0.0), inst.objectToWorld).xyz;
        B = mul(float4(bObj, 0.0), inst.objectToWorld).xyz;
    }

    if (dot(T, T) <= 0.0) return N;

    // Gram-Schmidt orthonormalization against shading normal.
    T = normalize(T - N * dot(N, T));
    if (!(dot(T, T) > 0.0)) return N;

    // Handedness from solved bitangent (mirrored UV shells have opposite).
    const float3 Bo = cross(N, T);
    B = Bo * (dot(Bo, B) < 0.0 ? -1.0 : 1.0);

    return normalize(T * nTS.x + B * nTS.y + N * nTS.z);
}
#endif

// Material buffer slot t9.
StructuredBuffer<RtMaterial> gRtMaterials : register(t9);

// Projected decals (t24). Before rtHitSurface, which applies them to every ray hit it builds.
#include "voxi_decal.hlsli"

// ================= ONE SURFACE FOR EVERY RAY HIT =================
// Where a ray landed, as the surface its material describes. The primary ray (PSRayDriven / Stage B),
// ReSTIR GI candidates, reflections and Path Tracing vertices all build it here: the material's maps
// through the bindless table, then material_prelude.hlsl's averAuthoredFrom/averComposeSurface -- the
// composition raster runs too. What differs per caller is only the UV footprint and how many maps.
struct RtHit {
    RtInstance inst;
    RtMaterial mat;
    uint   i0, i1, i2;
    float3 w;        // barycentrics
    float3 pos;      // world position
    float3 N;        // interpolated normal, world space, facing back along the ray
    float2 meshUV;
    float  t;        // ray distance
};

RtHit rtHitFrom(uint ref, uint prim, float2 bary, float3 origin, float3 dir, float t) {
    RtHit h;
    h.inst = rtLoadInstance(ref);
    h.mat  = gRtMaterials[h.inst.materialIndex];
    const uint tri = h.inst.firstIndex + prim * 3;
    h.i0 = h.inst.firstVertex + gRtIndices[tri + 0];
    h.i1 = h.inst.firstVertex + gRtIndices[tri + 1];
    h.i2 = h.inst.firstVertex + gRtIndices[tri + 2];
    h.w  = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    const float3 nObj = normalize(gRtVerts[h.i0].nrm * h.w.x + gRtVerts[h.i1].nrm * h.w.y + gRtVerts[h.i2].nrm * h.w.z);
    h.N = normalize(mul(float4(nObj, 0.0), h.inst.objectToWorld).xyz);
    if (dot(h.N, dir) > 0.0) h.N = -h.N;
    h.pos    = origin + dir * t;
    h.meshUV = gRtVerts[h.i0].uv * h.w.x + gRtVerts[h.i1].uv * h.w.y + gRtVerts[h.i2].uv * h.w.z;
    h.t      = t;
    return h;
}

RtHit rtHitCommitted(inout RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q, float3 origin, float3 dir) {
    return rtHitFrom(rtPackCommitted(q), q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics(),
                     origin, dir, q.CommittedRayT());
}

// The per-draw half of a hit, as the composition takes it. The tint is LINEARISED like raster's
// averDrawTerms: RtInstance carries the draw's colour as authored (sRGB).
AverDrawTerms rtDrawTerms(RtInstance inst) {
    AverDrawTerms d;
    d.tint      = srgbToLin(inst.albedo);
    d.alpha     = 1.0;
    d.metallic  = inst.metallic;
    d.roughness = inst.roughness;
    d.emissive  = float3(0.0, 0.0, 0.0);
    d.model     = AVER_MODEL_STANDARD;
    return d;
}

// How many of the material's maps a hit samples. FULL: all eight. LITE: base colour and metal/rough
// (plus emissive outside the single pass) -- the single-pass shader's secondary hits, at its register limit.
// ROUGHNORMAL: only what s.rough and s.N read (metal/rough, normal, their layer-1 maps), bit-identical to
// FULL for those two; every other field is unspecified. A material graph can rewrite them from any map, so
// it takes FULL.
#define AVER_RT_HIT_FULL        0u
#define AVER_RT_HIT_LITE        1u
#define AVER_RT_HIT_ROUGHNORMAL 2u

// Normal maps on ray hits, as on raster. Every user of the hit's shading normal (direct light, the
// reflection ray, sky ambient, the G-buffer) takes the same perturbed s.N.
#ifndef AVER_RT_NORMAL_MAPPING
#define AVER_RT_NORMAL_MAPPING 1
#endif

// UV footprint for SampleGrad from world-space footprint vectors at the hit: ray differentials for
// the primary, a cone for everything secondary (rtHitConeGrad).
void rtHitGrad(RtHit h, float3 dx, float3 dy, out float2 gx, out float2 gy) {
#ifdef AVER_RT_BINDLESS
    averRtUvGrad(h.mat, h.inst, h.N,
                 gRtVerts[h.i0].pos, gRtVerts[h.i1].pos, gRtVerts[h.i2].pos,
                 gRtVerts[h.i0].uv,  gRtVerts[h.i1].uv,  gRtVerts[h.i2].uv,
                 dx, dy, gx, gy);
#else
    gx = float2(0.0, 0.0);
    gy = float2(0.0, 0.0);
#endif
}

// A cone of half-angle tangent `tanCone` from the ray origin, as a footprint at the hit.
void rtHitConeGrad(RtHit h, float tanCone, out float2 gx, out float2 gy) {
    const float  rad = max(tanCone, 1e-3) * h.t;
    const float3 up  = abs(h.N.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0);
    const float3 T   = normalize(cross(up, h.N));
    const float3 B   = cross(h.N, T);
    rtHitGrad(h, T * rad, B * rad, gx, gy);
}

// THE HIT'S SURFACE. V points back along the ray; L is the light the half vector is first aimed at.
AverSurface rtHitSurface(RtHit h, float3 V, float3 L, float2 gx, float2 gy, uint detail) {
    const RtMaterial m = h.mat;
#if defined(AVER_RT_BINDLESS) && defined(AVER_MATERIAL_GRAPH)
    if (detail == AVER_RT_HIT_ROUGHNORMAL && m.graphId != 0u) detail = AVER_RT_HIT_FULL;
#endif
    const bool rn = detail == AVER_RT_HIT_ROUGHNORMAL;
    AverMaps map;
    map.baseColor  = float4(1.0, 1.0, 1.0, 1.0);
    map.metalRough = float2(1.0, 1.0);
    map.normalTS   = float3(0.0, 0.0, 1.0);
    map.normalLen  = 1.0;
    map.occlusion  = 1.0;
    map.emissive   = float3(1.0, 1.0, 1.0);
    float2 uv = h.meshUV;
#ifdef AVER_RT_BINDLESS
    uv = averRtSurfaceUV(m, h.inst, h.pos, h.N, h.meshUV);
    if (!rn) map.baseColor = averRtSampleSlot(m, 0, uv, gx, gy, float4(1.0, 1.0, 1.0, 1.0));
    const float4 mr = averRtSampleSlot(m, 1, uv, gx, gy, float4(1.0, 1.0, 1.0, 1.0));
    map.metalRough = float2(mr.g, mr.b);   // glTF: roughness G, metallic B
#if !AVER_RD_SINGLE_PASS
    if (!rn) map.emissive = averRtSampleSlot(m, 4, uv, gx, gy, float4(1.0, 1.0, 1.0, 1.0)).rgb;
#endif
    if (detail != AVER_RT_HIT_LITE) {
#if AVER_RD_SINGLE_PASS
        if (!rn) map.emissive = averRtSampleSlot(m, 4, uv, gx, gy, float4(1.0, 1.0, 1.0, 1.0)).rgb;
#endif
        const float3 n = averRtSampleSlot(m, 2, uv, gx, gy, float4(0.5, 0.5, 1.0, 1.0)).xyz * 2.0 - 1.0;
        map.normalTS  = float3(n.xy * m.normalScale, n.z);
        map.normalLen = (m.flags & AVER_MAT_NORMAL_MAP) ? length(n) : 1.0;
        if (!rn) map.occlusion = averRtSampleSlot(m, 3, uv, gx, gy, float4(1.0, 1.0, 1.0, 1.0)).r;
        // The second layer by slope (material_prelude.hlsl's averLayerWeight), as raster blends it.
        const float lw = averLayerWeight(m, h.N);
        if (lw > 0.001) {
            const float  s1  = m.layer1UvScale;
            const float2 uv1 = uv * s1;
            if (!rn && (m.flags & AVER_MAT_L1_BASECOLOR))
                map.baseColor = lerp(map.baseColor, averRtSampleSlot(m, 5, uv1, gx * s1, gy * s1, map.baseColor), lw);
            if (m.flags & AVER_MAT_L1_METALROUGH) {
                const float4 mr1 = averRtSampleSlot(m, 6, uv1, gx * s1, gy * s1, float4(1.0, mr.g, mr.b, 1.0));
                map.metalRough = lerp(map.metalRough, float2(mr1.g, mr1.b), lw);
            }
            if (m.flags & AVER_MAT_L1_NORMAL) {
                const float3 n1 = averRtSampleSlot(m, 7, uv1, gx * s1, gy * s1, float4(0.5, 0.5, 1.0, 1.0)).xyz * 2.0 - 1.0;
                map.normalTS = normalize(lerp(map.normalTS, float3(n1.xy * m.normalScale, n1.z), lw));
                map.normalLen = lerp(map.normalLen, length(n1), lw);
            }
        }
    }
#endif
    AverAuthored a = averAuthoredFrom(m, map);

    AverVertex v;
    v.wpos     = h.pos;
    v.N        = h.N;
    v.V        = V;
    v.uv       = uv;
    v.backFace = false;   // h.N already faces the ray
#if defined(AVER_RT_BINDLESS) && defined(AVER_MATERIAL_GRAPH)
    // THE MATERIAL'S GRAPH, as raster runs it: it edits the same AverAuthored before the composition.
    if (m.graphId != 0u) {
        gAverGraphMat       = m;
        gAverGraphGx        = gx;
        gAverGraphGy        = gy;
        gAverGraphObjectPos = h.inst.objectToWorld[3].xyz;
        a = averApplyMaterialGraphRt(m.graphId, a, v, uv);
    }
#endif

    float3 N = h.N;
#if defined(AVER_RT_BINDLESS) && AVER_RT_NORMAL_MAPPING
    if (detail != AVER_RT_HIT_LITE && (m.flags & (AVER_MAT_NORMAL_MAP | AVER_MAT_L1_NORMAL))) {
        N = averRtPerturbNormal(m, h.inst, h.N, a.normalTS,
                                gRtVerts[h.i0].pos, gRtVerts[h.i1].pos, gRtVerts[h.i2].pos,
                                gRtVerts[h.i0].uv,  gRtVerts[h.i1].uv,  gRtVerts[h.i2].uv);
        if (dot(N, V) < 0.0) N = normalize(N - V * dot(N, V) * 1.01);   // keep it facing the viewer
    }
#endif
    AverLight l;
    l.direction  = L;
    l.radiance   = float3(0.0, 0.0, 0.0);
    l.visibility = float3(1.0, 1.0, 1.0);
    AverSurface s = averComposeSurface(v, l, a, m, rtDrawTerms(h.inst), N);
#if defined(AVER_RT_BINDLESS) && !AVER_RD_SINGLE_PASS && !AVER_BLENDED_PASS
    // Decals repaint opaque hits (voxi_decal.hlsli): the staged ray-driven passes and their compute stages
    // (bindless). Not in the single-pass kernel, the glass variant or the non-bindless ray-traced raster
    // variants, all at their register limit (the last two hung the RX 7800 XT), nor on LITE secondary hits.
    // h.N is already the geometric normal facing the ray.
    if (gDecalParams.x > 0.5 && detail != AVER_RT_HIT_LITE &&
        (m.flags & AVER_MAT_ALPHA_BLEND) == 0 && !(m.transmission > 0.0))
        averApplyDecals(s, h.pos, h.N, detail == AVER_RT_HIT_FULL);
#endif
    return s;
}
// ---- ALPHA-TESTED GEOMETRY ----
// Opaque by default; BLENDED instances un-opaqued. Alpha-masked geometry (foliage, grates) is
// FORCE_NON_OPAQUE: each candidate pays index/vertex fetches, texture sample.
static uint gAverRtCutoutBudget      = 0xFFFFFFFFu;
static uint gAverRtSecondaryRayFlags = 0u;

void averRtCutoutPolicy(uint budget, bool solidCutouts) {
    gAverRtCutoutBudget      = budget;
    gAverRtSecondaryRayFlags = solidCutouts ? RAY_FLAG_FORCE_OPAQUE : 0u;
}

bool averRtCandidateOpaque(inout RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q) {
#ifndef AVER_RT_BINDLESS
    return true;
#else
    const RtInstance inst = rtLoadInstance(rtPackCandidate(q));
    const RtMaterial mat  = gRtMaterials[inst.materialIndex];
    // NOT ALPHA-MASKED MEANS OPAQUE.
    if ((mat.flags & AVER_MAT_ALPHA_MASK) == 0) return true;

    const uint tri = inst.firstIndex + q.CandidatePrimitiveIndex() * 3;
    const uint i0  = inst.firstVertex + gRtIndices[tri + 0];
    const uint i1  = inst.firstVertex + gRtIndices[tri + 1];
    const uint i2  = inst.firstVertex + gRtIndices[tri + 2];

    const float2 bary = q.CandidateTriangleBarycentrics();
    const float3 w    = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    float2 uv = gRtVerts[i0].uv * w.x + gRtVerts[i1].uv * w.y + gRtVerts[i2].uv * w.z;

    // World-aligned UV is resolved.
    if (mat.flags & AVER_MAT_WORLD_UV) {
        const float3 p0 = gRtVerts[i0].pos, p1 = gRtVerts[i1].pos, p2 = gRtVerts[i2].pos;
        const float3 nObj = cross(p1 - p0, p2 - p0);
        const float3 N    = normalize(mul(float4(nObj, 0.0), inst.objectToWorld).xyz);
        const float3 wpos = q.WorldRayOrigin() + q.WorldRayDirection() * q.CandidateTriangleRayT();
        uv = averRtSurfaceUV(mat, inst, wpos, N, uv);
    }

    // Mip 0; unbound slot returns opaque white. Slot 0 is BaseColor.
    const float4 baseMap = averRtSampleSlot(mat, 0, uv, float2(0, 0), float2(0, 0), float4(1, 1, 1, 1));
#ifdef AVER_MATERIAL_GRAPH
    // A graph may drive opacity or the cutoff; the cut-out must be the one raster clip()s.
    if (mat.graphId != 0u) {
        AverMaps map;
        map.baseColor  = baseMap;
        map.metalRough = float2(1.0, 1.0);
        map.normalTS   = float3(0.0, 0.0, 1.0);
        map.normalLen  = 1.0;
        map.occlusion  = 1.0;
        map.emissive   = float3(1.0, 1.0, 1.0);
        AverAuthored a = averAuthoredFrom(mat, map);
        const float3 p0 = gRtVerts[i0].pos, p1 = gRtVerts[i1].pos, p2 = gRtVerts[i2].pos;
        AverVertex v;
        v.wpos     = q.WorldRayOrigin() + q.WorldRayDirection() * q.CandidateTriangleRayT();
        v.N        = normalize(mul(float4(cross(p1 - p0, p2 - p0), 0.0), inst.objectToWorld).xyz);
        v.V        = -q.WorldRayDirection();
        v.uv       = uv;
        v.backFace = false;
        gAverGraphMat       = mat;
        gAverGraphGx        = float2(0.0, 0.0);
        gAverGraphGy        = float2(0.0, 0.0);
        gAverGraphObjectPos = inst.objectToWorld[3].xyz;
        a = averApplyMaterialGraphRt(mat.graphId, a, v, uv);
        return a.opacity >= a.alphaCutoff;
    }
#endif
    return baseMap.a * mat.baseColorFactor.a >= mat.alphaCutoff;
#endif
}

// Run to nearest solid hit, honouring cutouts. Only alpha-masked geometry produces candidates.
void averRtProceedSolid(inout RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q) {
    uint tests = 0u;
    while (q.Proceed()) {
        if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        if (tests >= gAverRtCutoutBudget || averRtCandidateOpaque(q)) q.CommitNonOpaqueTriangleHit();
        ++tests;
    }
}

// Ray-traced sun-shadow history: (visibility, depth) ping-ponged (t6/u2).
Texture2D<float2>   gRtShadowHist    : register(t6);
RWTexture2D<float2> gRtShadowHistOut : register(u2);
// Sky-occlusion history pair (x = openness, y = linear depth).
Texture2D<float2>   gAoHist    : register(t11);
RWTexture2D<float2> gAoHistOut : register(u4);
// Hit distance [0,1] as fraction of TMax; 1 = sky, 0 = at surface. Signed distance signal
// for denoiser's spread radius (occlusion alone can't distinguish wide opening from tight crevice).
RWTexture2D<float>  gAoHitDistOut : register(u5);

// Denoised AO, one frame later. Declared here for rtSkyOcclusionTemporal. Routinely absent
// (readers test dimensions==0); needs denoiser and G-buffer on (D3D12 only).
Texture2D<float>    gDenoisedAo   : register(t14);

// ReSTIR GI radiance (u9/t15): indirect diffuse, denoised. Separate from AO history.
RWTexture2D<float4> gGiRadianceOut : register(u9);
Texture2D<float4>   gDenoisedGi    : register(t15);
// Reflection denoising (Denoiser Signal::Reflection): last frame's result (zero size = not denoised),
// and this frame's input -- one fresh sample per pixel, a = its hit distance (cm).
Texture2D<float4>   gDenoisedRefl  : register(t23);
RWTexture2D<float4> gRdReflDnIn    : register(u23);

// Ray-traced reflection history: (colour, depth) ping-ponged (t7/u3).
Texture2D<float4>   gRtReflHist    : register(t7);
RWTexture2D<float4> gRtReflHistOut : register(u3);

// Last frame's normal-roughness G-buffer (crease term for spatial denoisers). Ping-ponged.
#if AVER_GBUFFER_HISTORY
Texture2D<float4> gGBufNormalHist : register(t10);
#endif

// Pixel hash for sample pattern rotation (spatial only, gate-deterministic).
float rtHash(float2 p) {
    float3 q = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
    q += dot(q, q.yzx + 33.33);
    return frac((q.x + q.y) * q.z);
}

// Radical inverse of i in base 2 [0,1). Sample sequence nested: phi(k) depends on k alone.
// Exact on every adapter (reversebits + IEEE round-to-nearest + power-of-two divisor).
float rtRadicalInverse2(uint i) {
    return (float)reversebits(i) * 2.3283064365386963e-10;   // 1 / 2^32
}

// Sample k of disc sequence as point in unit disc. Angle rotated per-pixel. Nested sequence:
// golden angle around, radical inverse outward, sqrt maps to AREA.
float2 rtDiscSample(uint k, float ang0) {
    float rad = sqrt(rtRadicalInverse2(k + 1));
    float a   = ang0 + (float)k * 2.39996323;
    return float2(cos(a), sin(a)) * rad;
}

// Cosine-weighted hemisphere sample (not fixed 45-degree ring). Malley's method: uniform disc
// lifted via sqrt(u) gives correct cosine law P(cosTheta < c) = c^2.
// idx = frameIdx*n+k keeps sequence NESTED; streamSalt prevents collisions between callers.
float2 rtHemiDiscSample(uint k, uint n, uint frameIdx, float2 pixelKey, float streamSalt) {
    const uint  idx = frameIdx * max(n, 1u) + k;
    const float u   = frac(rtRadicalInverse2(idx + 1u) + rtHash(pixelKey + float2(streamSalt, 17.0 + streamSalt)));
    const float a   = rtHash(pixelKey) * 6.2831853 + averGoldenTurns(idx) + streamSalt;
    return float2(cos(a), sin(a)) * sqrt(u);
}

// Decoder for gGiShadowParams.w bitfield (rtGiShadowBits usage in staging passes).
#if AVER_RD_SINGLE_PASS
uint rtGiShadowBits() { return 1u; }
#else
uint rtGiShadowBits() { return (uint)gGiShadowParams.w; }
#endif

// NRD2 frame (bit 512): u9 and u23's rgb belong to Stage B's targets, so the stages' writes there are dead.
bool rtNrd2Frame() { return (rtGiShadowBits() & 512u) != 0u; }

// False where the sun's radiance is exactly 0 (a night scene): every consumer multiplies visibility by it.
bool rdSunLit() { return any(averSunRadiance() != 0.0); }

// Sun-shadow rays: 0..1 visibility over disc. Penumbra from pixel footprint (dpx/dpy), not just disc.
// Bias scales with distance. Secondary rays can request fewer samples than primary.
// frameJitter added to rotation (gate-deterministic per-pixel). Bit 32 enables ACCEPT_FIRST_HIT.
float3 rtShadowEx(float3 wpos, float3 N, float3 L, float2 pixel, float3 dpx, float3 dpy, uint rays,
                  float frameJitter, uint kFirst) {
    const uint  n    = max(rays, 1u);
    const float tanR = gAverShadowTanR >= 0.0 ? gAverShadowTanR : max(gRtParams.x, 0.0);
    const float bias = max(gRtParams.z, 1e-4) * (1.0 + length(wpos - gCamPos.xyz) * 5e-4);
#if AVER_RD_ABLATE != AVER_RD_ABL_SHADOW_FIRSTHIT
    // A lamp's ray (rdSetShadowLight gave it a disc: gAverShadowTanR >= 0) takes the opaque first-hit path too, as
    // lamp rays always did: the transmittance walk through glass cost ~2x and lamps were never tinted by it.
    const bool firstHitOnly = (rtGiShadowBits() & 32u) != 0u || gAverShadowTanR >= 0.0;
#endif

    // Frame around light direction.
    float3 up = abs(L.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T  = normalize(cross(up, L));
    float3 B  = cross(L, T);

    // Jitter origin across pixel footprint for area estimate.
    const float ang0 = rtHash(pixel) * 6.2831853 + frameJitter;
    float3 vis = float3(0.0, 0.0, 0.0);

    [loop] for (uint k = 0; k < n; ++k) {
        // Sample independent of n: same location, refining estimate rather than replacing it.
        float2 disc = rtDiscSample(k + kFirst, ang0);

        float3 dir = normalize(L + (T * disc.x + B * disc.y) * tanR);
        // Half footprint: samples stay inside pixel.
        float3 org = wpos + (dpx * disc.x + dpy * disc.y) * 0.5;

        RayDesc r;
        // Bias along normal AND ray (normal alone fails at grazing angles).
        r.Origin    = org + N * bias + dir * bias + gAverShadowOriginPush;
        r.Direction = dir;
        r.TMin      = bias;
        r.TMax      = max(gAverShadowTMax, bias);
#if AVER_RD_ABLATE != AVER_RD_ABL_SHADOW_FIRSTHIT
        // FIRST-HIT FAST PATH for opaque geometry (bit 32).
        if (firstHitOnly) {
            RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> qf;
            qf.TraceRayInline(gScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, AVER_RT_MASK_OPAQUE_ALL, r);
            [loop] for (uint step = 0; step < 8u && qf.Proceed(); ++step) {
                // Only cutouts are non-opaque here.
                const RtMaterial m = gRtMaterials[rtLoadInstance(rtPackCandidate(qf)).materialIndex];
                if ((m.flags & AVER_MAT_CAST_SHADOW) && averRtCandidateOpaque(qf)) {
                    qf.CommitNonOpaqueTriangleHit();
                    break;
                }
            }
            if (qf.CommittedStatus() != COMMITTED_TRIANGLE_HIT) vis += float3(1.0, 1.0, 1.0);
            continue;
        }
#endif
        // Transmittance walk for glass/water; walks to opaque or end. Multiplies transmittance per surface.
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
#if AVER_RD_ABLATE == AVER_RD_ABL_SHADOW_FIRSTHIT
        // Ablation measurement: opaque lane only.
        q.TraceRayInline(gScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE,
                         AVER_RT_MASK_OPAQUE_ALL, r);
#else
        q.TraceRayInline(gScene, RAY_FLAG_NONE, AVER_RT_MASK_ALL, r);
#endif

        // ---- Two transmittance models ----
        // VOLUME (attenuationDistance > 0): Beer-Lambert over span; otherwise per-crossing surface rule.
        // Paired by min/max t (order, not arrival).
        uint  med0Iid = 0xffffffffu, med1Iid = 0xffffffffu;
        float med0Min = 0.0, med0Max = 0.0, med1Min = 0.0, med1Max = 0.0;
        uint  med0Hits = 0u, med1Hits = 0u;

        float3 through = float3(1, 1, 1);   // Running transmittance.
        // Bounded at 8 crossings (transmittance visually zero).
        [loop] for (uint step = 0; step < 8u && q.Proceed(); ++step) {
            if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) break;

            const uint iid = rtPackCandidate(q);
            const RtMaterial m = gRtMaterials[rtLoadInstance(iid).materialIndex];

            // CUTOUT IS NOT A MEDIUM: alpha-masked (leaf, grate) binary rule.
            if (m.flags & AVER_MAT_ALPHA_MASK) {
                if ((m.flags & AVER_MAT_CAST_SHADOW) && averRtCandidateOpaque(q)) {
                    q.CommitNonOpaqueTriangleHit();
                    break;                       // Fully blocked.
                }
                continue;
            }

            if ((m.flags & AVER_MAT_CAST_SHADOW) == 0) continue;

            bool handled = false;
            if (m.attenuationDistance > 0.0) {
                const float ct = q.CandidateTriangleRayT();
                if (med0Iid == iid) {
                    med0Min = min(med0Min, ct); med0Max = max(med0Max, ct); ++med0Hits; handled = true;
                } else if (med1Iid == iid) {
                    med1Min = min(med1Min, ct); med1Max = max(med1Max, ct); ++med1Hits; handled = true;
                } else if (med0Hits == 0u) {
                    med0Iid = iid; med0Min = ct; med0Max = ct; med0Hits = 1u; handled = true;
                } else if (med1Hits == 0u) {
                    med1Iid = iid; med1Min = ct; med1Max = ct; med1Hits = 1u; handled = true;
                }
            }

            if (!handled) {
                // Per-crossing surface rule (HLSL twin of pbr::shadowTransmittance).
                const float k = max(1.0 - m.baseColorFactor.a, saturate(m.transmission));
                through *= saturate(m.baseColorFactor.rgb) * k;

                // Early-out once nothing gets through (surface path only).
                if (max(through.r, max(through.g, through.b)) < 0.01 && med0Hits == 0u) {
                    through = float3(0, 0, 0);
                    break;
                }
            }
        }

        // Resolve gathered spans via Beer-Lambert (same as VIEW path).
        if (med0Hits > 0u) {
            const RtMaterial m0 = gRtMaterials[rtLoadInstance(med0Iid).materialIndex];
            const float th0 = (med0Hits == 1u) ? med0Max : (med0Max - med0Min);
            through *= averVolumeTransmittance(m0.attenuationColor, m0.attenuationDistance, th0);
        }
        if (med1Hits > 0u) {
            const RtMaterial m1 = gRtMaterials[rtLoadInstance(med1Iid).materialIndex];
            const float th1 = (med1Hits == 1u) ? med1Max : (med1Max - med1Min);
            through *= averVolumeTransmittance(m1.attenuationColor, m1.attenuationDistance, th1);
        }

        // Opaque hit anywhere blocks everything.
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) through = float3(0, 0, 0);

        vis += through;
    }
    return vis / (float)n;
}

float3 rtShadow(float3 wpos, float3 N, float3 L, float2 pixel, float3 dpx, float3 dpy, uint rays,
               float frameJitter) {
    return rtShadowEx(wpos, N, L, pixel, dpx, dpy, rays, frameJitter, 0u);
}

// Shadow ray start: distance-scaled bias, along normal AND ray. Shared with rdLocalShadow.
void rtShadowRayStart(float3 wpos, float3 N, float3 dir, out float3 origin, out float bias) {
    bias   = max(gRtParams.z, 1e-4) * (1.0 + length(wpos - gCamPos.xyz) * 5e-4);
    origin = wpos + N * bias + dir * bias;
}

// Single rtShadowEx sample (rays=1, kFirst=0, zero footprint). Factored for rtShadowOpaque.
void rtShadowRay0(float3 wpos, float3 N, float3 L, float2 pixel, float frameJitter,
                  out float3 dir, out float3 origin, out float bias) {
    float3 up = abs(L.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T  = normalize(cross(up, L));
    float3 B  = cross(L, T);
    const float tanR  = gAverShadowTanR >= 0.0 ? gAverShadowTanR : max(gRtParams.x, 0.0);
    const float ang0  = rtHash(pixel) * 6.2831853 + frameJitter;
    const float2 disc = rtDiscSample(0, ang0);
    dir    = normalize(L + (T * disc.x + B * disc.y) * tanR);
    rtShadowRayStart(wpos, N, dir, origin, bias);
}

// SECONDARY HIT sun shadow: one ray, ACCEPT_FIRST_HIT, AVER_RT_MASK_OPAQUE_ALL.
// Leaf still casts alpha-tested SHAPE (via averRtProceedSolid), not bounding rectangle.
// Excludes translucent by mask (no tint). Primary (rtShadowTemporalEx) keeps tint.
float3 rtShadowOpaque(float3 wpos, float3 N, float3 L, float2 pixel, float frameJitter) {
    float3 dir, origin;
    float  bias;
    rtShadowRay0(wpos, N, L, pixel, frameJitter, dir, origin, bias);

    RayDesc r;
    r.Origin    = origin;
    r.Direction = dir;
    r.TMin      = bias;
    r.TMax      = max(gAverShadowTMax, bias);

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | gAverRtSecondaryRayFlags, AVER_RT_MASK_OPAQUE_ALL, r);
    averRtProceedSolid(q);

    return (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? float3(0, 0, 0) : float3(1, 1, 1);
}

// ---- LOCAL LIGHTS: lamps lit like the sun ----
// Material lightIntensity > 0 becomes a SPHERE light in gRdLocalLights; scene CLights (point, spot, rect,
// with IES and cookies) join the same list. Lit pixels get one stochastic shadow ray toward one light,
// accumulated via the sun history. Each light shaded through the sun's BRDF.
// Declared here (before voxi_restir.hlsli) because that file needs rdLocalCarriesEmitters().
// MIRRORS the C++ RdLocalLight (80 bytes) field for field; the record is documented in aver_lights.hlsli.
#if AVER_RD_SINGLE_PASS
#define AVER_LIGHTS_SIMPLE 1
#endif
#include "aver_lights.hlsli"
// The light list (t18) holds every emitter, the sun as its directional entry, then the light grid
// (docs/rendering/UNIFIED_LIGHTS.md). AVER_LIGHT_LIST_MAX bounds the raster path's working set (gCameraMedium.z):
// the first, most important lamps. The ray-driven passes and every hit read the grid instead.
#define AVER_LIGHT_LIST_MAX 32u
typedef AverLightRec RdLocalLight;
// The light list (at most 32). Declared here, not in voxi.hlsl, so voxi_pt.hlsli below can read it.
StructuredBuffer<RdLocalLight> gRdLocalLights : register(t18);

// AVER_RD_SINGLE_PASS_LAMPS (default 1) keeps lamps in single-pass, at register limit. ";AVER_RD_SINGLE_PASS_LAMPS=0" strips them.
#ifndef AVER_RD_SINGLE_PASS_LAMPS
#define AVER_RD_SINGLE_PASS_LAMPS 1
#endif
#define AVER_RD_LAMPS (!AVER_RD_SINGLE_PASS || AVER_RD_SINGLE_PASS_LAMPS)

#if AVER_RD_LAMPS
// Count 0 means lamps off. gRdLocalHist (t19) holds valid history if bit 1 set.
uint rdLocalLightCount() { return (uint)(gCameraMedium.z + 0.5); }
// True when this frame's light set is valid for reprojection.
bool rdLocalHistValid()  { return ((uint)(gCameraMedium.w + 0.5) & 1u) != 0u; }
// True when all lamp-flagged draws made the 32-cap list (bit 2), so GI hits can skip their emission.
bool rdLocalCarriesEmitters() { return ((uint)(gCameraMedium.w + 0.5) & 2u) != 0u; }
// ---- THE LIGHT GRID (UNIFIED_LIGHTS.md phase 3), stored after the lights in t18 as floats ----
// gDecalParams.y: the header record (0 = no grid). See VoxiRenderer::buildLocalLights for the layout.
float rdListFloat(uint f) {
    const uint i = f / 20u, c = f % 20u, q = c >> 2u;
    // One float4 per read: selecting among a loaded record's five made the whole 80 bytes load.
    if (q == 0u) return gRdLocalLights[i].posRadius[c & 3u];
    if (q == 1u) return gRdLocalLights[i].radianceRange[c & 3u];
    if (q == 2u) return gRdLocalLights[i].axisKind[c & 3u];
    if (q == 3u) return gRdLocalLights[i].shape[c & 3u];
    return gRdLocalLights[i].right[c & 3u];
}
struct RdLightRange {
    uint globals;            // directional lights, reaching every point
    uint g0, g1, g2;
    uint cellStart;          // float index of the cell's first pool entry
    uint count;              // globals + the cell's lights
};
// The lights that can reach `pos`: the directional ones, then those of its grid cell. Without a grid, the raster
// working set [0, gCameraMedium.z).
RdLightRange rdLightsAt(float3 pos) {
    RdLightRange r;
    r.globals = 0u; r.g0 = r.g1 = r.g2 = 0u; r.cellStart = 0u; r.count = 0u;
    const uint hdr = (uint)(gDecalParams.y + 0.5);
    if (hdr == 0u) {
        r.count = min((uint)(gCameraMedium.z + 0.5), AVER_LIGHT_LIST_MAX);
        r.cellStart = 0xFFFFFFFFu;   // marks the identity mapping
        return r;
    }
    const AverLightRec h = gRdLocalLights[hdr];
    if (h.shape.y > 0.5) {   // a short list, walked whole: the same light in every lane at once (a uniform load)
        r.count = (uint)(h.shape.y + 0.5);
        r.cellStart = 0xFFFFFFFFu;
        return r;
    }
    r.globals = (uint)(h.axisKind.w + 0.5);
    r.g0 = (uint)max(h.axisKind.x, 0.0); r.g1 = (uint)max(h.axisKind.y, 0.0); r.g2 = (uint)max(h.axisKind.z, 0.0);
    r.count = r.globals;
    const uint3 dim = (uint3)(h.radianceRange.xyz + 0.5);
    if (all(dim > 0u)) {
        const float3 c = (pos - h.posRadius.xyz) / max(h.posRadius.w, 1.0);
        if (all(c >= 0.0) && all(c < (float3)dim)) {
            const uint3 ci = (uint3)c;
            const uint cell = (ci.z * dim.y + ci.y) * dim.x + ci.x;
            const uint tf = (uint)(h.radianceRange.w + 0.5) * 20u + cell * 2u;
            r.cellStart = (uint)(h.shape.x + 0.5) * 20u + (uint)(rdListFloat(tf) + 0.5);
            r.count += (uint)(rdListFloat(tf + 1u) + 0.5);
        }
    }
    return r;
}
uint rdLightIndex(RdLightRange r, uint k) {
    if (r.cellStart == 0xFFFFFFFFu) return k;
    if (k < r.globals) return k == 0u ? r.g0 : k == 1u ? r.g1 : r.g2;
    return (uint)(rdListFloat(r.cellStart + k - r.globals) + 0.5);
}

// THE RAY BUDGET (UNIFIED_LIGHTS.md): the n-th largest of up to 8 weights, kept sorted in registers (fully unrolled
// compare-swaps, no indexed array). Lights at or above it get their own shadow ray.
struct RdTopWeights { float t0, t1, t2, t3, t4, t5, t6, t7; };
RdTopWeights rdTopInit() { RdTopWeights r; r.t0 = r.t1 = r.t2 = r.t3 = r.t4 = r.t5 = r.t6 = r.t7 = 0.0; return r; }
void rdTopAdd(inout RdTopWeights r, float w) {
    float x = w, h;
    h = max(r.t0, x); x = min(r.t0, x); r.t0 = h;
    h = max(r.t1, x); x = min(r.t1, x); r.t1 = h;
    h = max(r.t2, x); x = min(r.t2, x); r.t2 = h;
    h = max(r.t3, x); x = min(r.t3, x); r.t3 = h;
    h = max(r.t4, x); x = min(r.t4, x); r.t4 = h;
    h = max(r.t5, x); x = min(r.t5, x); r.t5 = h;
    h = max(r.t6, x); x = min(r.t6, x); r.t6 = h;
    r.t7 = max(r.t7, x);
}
// How many of the 8 kept weights are above x.
uint rdTopCountAbove(RdTopWeights r, float x) {
    return (uint)(r.t0 > x) + (uint)(r.t1 > x) + (uint)(r.t2 > x) + (uint)(r.t3 > x)
         + (uint)(r.t4 > x) + (uint)(r.t5 > x) + (uint)(r.t6 > x) + (uint)(r.t7 > x);
}
uint rdLightRaysPerBlock() { return clamp((uint)(gDecalParams.z + 0.5), 1u, 8u); }
uint rdLightRaysPerHit()   { return clamp((uint)(gDecalParams.w + 0.5), 1u, 8u); }

// True when this pass's visible-surface lamp visibility exists (bit 4). The list itself is always published.
bool rdLocalLampsLive() { return ((uint)(gCameraMedium.w + 0.5) & 4u) != 0u; }
// True when an emitter a ray hit at `hitPos` is one of the listed lamps (inside its bounding sphere): its light is
// already in the direct lamp term, so the hit drops its glow. Every listed lamp, not only when all are listed:
// a lamp past kMaxListLights, or outside the grid, keeps its glow at hits; it is its only path. Looked up through
// the light grid, the same lights the hit's direct term sees.
// Only reached on hits with a light-flagged material, so the loop is rare.
bool rdLocalEmitterCarried(float3 hitPos) {
    if (rdLocalCarriesEmitters()) return true;
    const RdLightRange lr = rdLightsAt(hitPos);
    [loop] for (uint k = 0u; k < lr.count; ++k) {
        const uint i = rdLightIndex(lr, k);
        if (aversLightKind(gRdLocalLights[i]) == AVER_LIGHT_DIRECTIONAL) continue;
        const float4 pr = gRdLocalLights[i].posRadius;
        const float3 d  = hitPos - pr.xyz;
        if (dot(d, d) <= pr.w * pr.w * 1.0404) return true;   // 2% margin on the bound
    }
    return false;
}

// Diffuse irradiance from one light (N.L included): inverse square clamped at the emitter radius, faded by
// (1-(d/range)^4)^2, shaped by cone / IES / cookie; a rectangle through its polygonal form factor.
float3 rdLocalIrradiance(RdLocalLight l, float3 wpos, float3 N) {
    return aversLightIrradiance(l, wpos, N);
}

// One opaque shadow ray from wpos toward a point on the light: its sphere (TMax stops short of it), or a
// uniform point u2 on a rectangle. A light flagged no-shadow returns 1.
float rdLocalShadow(float3 wpos, float3 N, RdLocalLight l, float2 pixel, float frameJitter, float2 u2) {
    if (aversLightNoShadow(l)) return 1.0;
    const float3 toC  = l.posRadius.xyz - wpos;
    const float  dist = length(toC);
    float3 target;
    float  tMax;
    if (aversLightKind(l) == AVER_LIGHT_RECT) {
        target = aversRectShadowTarget(l, u2, wpos, N);
        tMax   = length(target - wpos) - 0.5;
    } else {
        tMax = dist - l.posRadius.w * 1.25;
        if (tMax <= 0.0) return 1.0;
        const float3 Lc = toC / dist;
        float3 up = abs(Lc.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0);
        float3 T  = normalize(cross(up, Lc));
        float3 B  = cross(Lc, T);
        const float  ang0 = rtHash(pixel) * 6.2831853 + frameJitter;
        const float2 disc = rtDiscSample(0, ang0);
        target = l.posRadius.xyz + (T * disc.x + B * disc.y) * l.posRadius.w;
    }
    if (tMax <= 0.0) return 1.0;
    const float3 dir = normalize(target - wpos);

    float3 origin;
    float  bias;
    rtShadowRayStart(wpos, N, dir, origin, bias);

    RayDesc r;
    r.Origin    = origin;
    r.Direction = dir;
    r.TMin      = bias;
    r.TMax      = max(tMax, bias);

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, AVER_RT_MASK_OPAQUE_ALL, r);
    averRtProceedSolid(q);

    return (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0 : 1.0;
}
#endif

// Fraction of hemisphere above N from which the SKY is reachable (0=enclosed, 1=open).
float3 voxelUVW(float3 wp);
bool   insideVolume(float3 uvw);

// Forward declare traceCone (defined in voxi_cone.hlsli, needed by voxi_restir.hlsli).
float4 traceCone(float3 originWS, float3 dir, float aperture);

// Result of one hemisphere gather: open fraction and mean hit distance.
struct AverAmbientTraced {
    float  open;     // fraction reaching sky
    float  hitDist;  // mean distance / TMax (external denoiser input)
};

AverAmbientTraced rtAmbientTraced(float3 wpos, float3 N, float2 pixel, uint rays) {
    const uint n = clamp(rays, 1u, 32u);
    const float bias = max(gRtParams.z, 1e-4) * (1.0 + length(wpos - gCamPos.xyz) * 5e-4);
    // Rotation shared across tile, not per-pixel (measured as the best lever for cache coherence).
    const float aoTileEdge = max(gAmbientParams.y, AVER_AO_COHERENCE_TILE);
    const float2 aoTile = floor(pixel / aoTileEdge);
    // Different direction every frame via golden angle, else accumulation does nothing.
    const float ang0 = rtHash(aoTile) * 6.2831853 + averGoldenTurns((uint)gRtHistParams.z);
    float3 T, B;
    const float3 up = abs(N.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0);
    T = normalize(cross(up, N));
    B = cross(N, T);

    AverAmbientTraced res;
    res.open = 0.0; res.hitDist = 0.0;
    const float aoTMax = max(gVoxelParams.z, 1.0);
    [loop] for (uint k = 0; k < n; ++k) {
        // F1 (R0): gAmbientParams.z bit 1 keeps legacy 45-degree ring; default uses cosine hemisphere.
        const float2 d = (((uint)gAmbientParams.z & 1u) != 0u) ? rtDiscSample(k, ang0) : rtHemiDiscSample(k, n, (uint)gRtHistParams.z, aoTile, 0.37);
        // Malley: lift disc onto hemisphere.
        const float3 dir = T * d.x + B * d.y + N * sqrt(saturate(1.0 - dot(d, d)));

        RayDesc r;
        r.Origin    = wpos + N * bias + dir * bias;
        r.Direction = dir;
        r.TMin      = bias;
        r.TMax      = max(gVoxelParams.z, 1.0);

        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
        q.TraceRayInline(gScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | gAverRtSecondaryRayFlags, AVER_RT_MASK_OPAQUE_ALL, r);
        averRtProceedSolid(q);

        if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
            res.open    += 1.0;
            res.hitDist += aoTMax;
        } else {
            res.hitDist += min(q.CommittedRayT(), aoTMax);
        }
    }

    const float inv = 1.0 / (float)n;
    res.open    *= inv;
    res.hitDist = saturate(res.hitDist * inv / aoTMax);
    return res;
}

// Sky occlusion fraction (wrapper, hit distance unused).
float rtSkyOcclusion(float3 wpos, float3 N, float2 pixel, uint rays) {
    return rtAmbientTraced(wpos, N, pixel, rays).open;
}

// Reprojects wpos through last frame's camera to sample shadow history. False when unusable
// (off-screen, behind near plane, or disocclusion).
// Shadow history .x holds the light's key with its visibility: key * 2 + vis (vis in [0, 1]; the key is below 4096,
// so RG32F keeps vis to about 1e-3). Unpack fails when the texel belongs to another light.
float rtShadowHistPack(float vis) { return gAverShadowLightId * 2.0 + saturate(vis); }
bool  rtShadowHistUnpack(float x, out float vis) {
    const float id = floor(max(x, 0.0) * 0.5);
    vis = saturate(x - 2.0 * id);
    return id == gAverShadowLightId;
}

bool rtReprojectHistory(float3 wpos, float2 pixel, out float hist, out float2 velocityPx) {
    hist = 0.0;
    velocityPx = 0.0;
    float4 clip = mul(float4(wpos + gAverReprojDelta, 1.0), gPrevViewProj);
    const float dzdx = ddx(clip.w);
    const float dzdy = ddy(clip.w);
    if (clip.w <= 1e-4) return false;
    float3 ndc = clip.xyz / clip.w;
    if (ndc.z < 0.0 || ndc.z > 1.0) return false;
    float texW, texH;
    gRtShadowHist.GetDimensions(texW, texH);
    float2 px = gSceneViewport.xy +
                float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * gSceneViewport.zw;
    // Floor not round: matches write side indexing.
    int2 texel = int2(floor(px));
    if (any(texel < 0) || texel.x >= (int)texW || texel.y >= (int)texH) return false;

    const float2 stored = gRtShadowHist.Load(int3(texel, 0));
    // Tolerance follows depth gradient: 3% relative + 1cm floor + surface slope.
    const float tol = max(clip.w, stored.y) * 0.03 + 1.0 + (abs(dzdx) + abs(dzdy)) * 2.0;
    if (abs(clip.w - stored.y) > tol) return false;
    // Only this light's history (rtShadowHistPack): a texel that last held another light starts fresh.
    if (!rtShadowHistUnpack(stored.x, hist)) return false;
    velocityPx = px - pixel;
    return true;
}

// Local-light twin of rtReprojectHistory: returns texel instead of visibility. Arithmetic identical.
bool rtReprojectTexel(float3 wpos, float2 pixel, out int2 texel, out float2 velocityPx) {
    texel = int2(0, 0);
    velocityPx = 0.0;
    float4 clip = mul(float4(wpos + gAverReprojDelta, 1.0), gPrevViewProj);
    const float dzdx = ddx(clip.w);
    const float dzdy = ddy(clip.w);
    if (clip.w <= 1e-4) return false;
    float3 ndc = clip.xyz / clip.w;
    if (ndc.z < 0.0 || ndc.z > 1.0) return false;
    float texW, texH;
    gRtShadowHist.GetDimensions(texW, texH);
    float2 px = gSceneViewport.xy +
                float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * gSceneViewport.zw;
    const int2 t = int2(floor(px));
    if (any(t < 0) || t.x >= (int)texW || t.y >= (int)texH) return false;
    const float storedDepth = gRtShadowHist.Load(int3(t, 0)).y;
    const float tol = max(clip.w, storedDepth) * 0.03 + 1.0 + (abs(dzdx) + abs(dzdy)) * 2.0;
    if (abs(clip.w - storedDepth) > tol) return false;
    texel = t;
    velocityPx = px - pixel;
    return true;
}

// Ambient (AO) twin of rtReprojectHistory: reads gAoHist. Arithmetic identical.
bool rtReprojectAo(float3 wpos, float2 pixel, out float hist, out float2 velocityPx) {
    hist = 0.0;
    velocityPx = 0.0;
    float4 clip = mul(float4(wpos + gAverReprojDelta, 1.0), gPrevViewProj);
    const float dzdx = ddx(clip.w);
    const float dzdy = ddy(clip.w);
    if (clip.w <= 1e-4) return false;
    float3 ndc = clip.xyz / clip.w;
    if (ndc.z < 0.0 || ndc.z > 1.0) return false;
    float texW, texH;
    gAoHist.GetDimensions(texW, texH);
    float2 px = gSceneViewport.xy +
                float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * gSceneViewport.zw;
    int2 texel = int2(floor(px));
    if (any(texel < 0) || texel.x >= (int)texW || texel.y >= (int)texH) return false;
    const float2 stored = gAoHist.Load(int3(texel, 0));
    const float tol = max(clip.w, stored.y) * 0.03 + 1.0 + (abs(dzdx) + abs(dzdy)) * 2.0;
    if (abs(clip.w - stored.y) > tol) return false;
    hist = stored.x;
    velocityPx = px - pixel;
    return true;
}

// AO accumulated over time, denoised spatially, then optionally replaced by external denoiser result.
float rtAoSpatial(float centre, float3 wpos, float3 N, float2 pixel, float curDepth) {
    const int shadowRadius = (int)gRtDenoiseParams.x;
    if (shadowRadius <= 0 || gRtHistParams.y < 0.25 || gRtDenoiseParams.w < 0.5) return centre;
    const int radius = max(shadowRadius, 2);

    float texW, texH;
    gAoHist.GetDimensions(texW, texH);

    const float4 pclip  = mul(float4(wpos + gAverReprojDelta, 1.0), gPrevViewProj);
    const float  pdepth = pclip.w;
    const float  dpdx   = ddx(pdepth);
    const float  dpdy   = ddy(pdepth);
    const float  cdx    = ddx(curDepth);
    const float  cdy    = ddy(curDepth);

    float2 centrePx = pixel;
    bool   reproj   = false;
    if (pdepth > 1e-4) {
        const float3 pndc = pclip.xyz / pdepth;
        if (pndc.z >= 0.0 && pndc.z <= 1.0) {
            centrePx = gSceneViewport.xy +
                       float2(pndc.x * 0.5 + 0.5, 0.5 - pndc.y * 0.5) * gSceneViewport.zw;
            reproj = true;
        }
    }
    const int2 base = int2(floor(centrePx));

    const float planeDepth = reproj ? pdepth : curDepth;
    const float dzdx       = reproj ? dpdx   : cdx;
    const float dzdy       = reproj ? dpdy   : cdy;

    const float sigma  = max((float)radius * 0.5, 0.5);
    const float inv2s2 = 1.0 / (2.0 * sigma * sigma);

    float acc = centre;
    float wsum = 1.0;
    float nSum = 0.0, nSum2 = 0.0, nCount = 0.0;
    [loop] for (int oy = -radius; oy <= radius; ++oy) {
        [loop] for (int ox = -radius; ox <= radius; ++ox) {
            if (ox == 0 && oy == 0) continue;
            const int2 t = base + int2(ox, oy);
            if (any(t < 0) || t.x >= (int)texW || t.y >= (int)texH) continue;
            const float2 st = gAoHist.Load(int3(t, 0));
            const float predicted = planeDepth + dzdx * (float)ox + dzdy * (float)oy;
            const float tol = max(abs(predicted), 1.0) * 0.02 + 1.0;
            if (abs(st.y - predicted) > tol) continue;
#if AVER_GBUFFER_HISTORY
            const float3 nb = gGBufNormalHist.Load(int3(t, 0)).xyz * 2.0 - 1.0;
            if (dot(N, nb) < 0.5) continue;
#endif
            const float w = exp(-(float)(ox * ox + oy * oy) * inv2s2);
            acc  += st.x * w;
            wsum += w;
            nSum += st.x; nSum2 += st.x * st.x; nCount += 1.0;
        }
    }

    // Clamp to mean +/- 2*sigma removes salt-and-pepper impulses without blurring real features.
    float clamped = centre;
    if (nCount >= 3.0) {
        const float mean  = nSum / nCount;
        const float sigma = sqrt(max(nSum2 / nCount - mean * mean, 0.0));
        const float k = 2.0 * max(sigma, 0.02);
        clamped = clamp(centre, mean - k, mean + k);
        acc += clamped - centre;
    }

    const float velPx = reproj ? length(centrePx - pixel) : 0.0;
    const float trust = gRtDenoiseParams.z > 0.0 ? saturate((3.0 - velPx) * gRtDenoiseParams.z) : 1.0;
    return saturate(lerp(clamped, acc / wsum, saturate(gRtDenoiseParams.y) * trust));
}

// Temporal AO: accumulates over frames via history, optionally replaces with external denoiser result.
float rtSkyOcclusionTemporal(float3 wpos, float3 N, float2 pixel, uint rays, float coneAo,
                             bool coneAoIsGather, bool denoisedAoUsable) {
    // AO history pair allocated only when sky occlusion ray wanted (VoxiRenderer::aoHistoryWanted).
    if (gRtDenoiseParams.w < 0.5) {
        const AverAmbientTraced amb = rtAmbientTraced(wpos, N, pixel, rays);
        return coneAoIsGather ? coneAo : amb.open;
    }

    const float curDepth = mul(float4(wpos, 1.0), gViewProj).w;

    // T2 (Settings::rtSkyOcclusionHalfRate): skip trace on alternating 8x8 tiles to reduce ray cost.
    float histV = 0.0;
    float2 velocityPx = 0.0;
    const bool haveHist = gRtHistParams.y > 0.25 && rtReprojectAo(wpos, pixel, histV, velocityPx);

    bool skipTrace = false;
    if ((rtGiShadowBits() & 2u) != 0u && haveHist) {
        const uint2 tile = (uint2(pixel) - (uint2)gSceneViewportCur.xy) / 8u;
        skipTrace = ((tile.x ^ tile.y ^ (uint)gRtHistParams.z) & 1u) != 0u;
    }

    float fresh;
    float hitDist = 0.0;
    bool  tracedNow;
    if (skipTrace) {
        fresh = histV;
        tracedNow = false;
    } else {
        const AverAmbientTraced amb = rtAmbientTraced(wpos, N, pixel, rays);
        fresh = amb.open;
        hitDist = amb.hitDist;
        tracedNow = true;
    }

    // Seed reprojection from this pixel's own trace, not coneAo (which is constant 1.0 under ReSTIR GI).
    float vis = fresh;
    if (haveHist) {
        const float t = saturate(length(velocityPx) / 32.0);
        float weight  = lerp(0.97, 0.5, t);
        // A change beyond the trace's noise (std of an n-ray mean <= 0.5/sqrt(n)) is an occluder
        // arriving or leaving: drop toward 0.75 so it does not trail for 30 frames.
        if (tracedNow) {
            const float thr = 0.75 / sqrt((float)max(rays, 1u));
            weight = lerp(weight, min(weight, 0.75), saturate((abs(fresh - histV) - thr) / thr));
        }
        vis = lerp(fresh, histV, weight);
    }

    // Replace with external denoiser result if available (gDenoisedAo, from last frame's filtered hit distance).
    uint dnW = 0, dnH = 0;
    gDenoisedAo.GetDimensions(dnW, dnH);
    if (denoisedAoUsable && gAverHistoryWrite && dnW > 0u && dnH > 0u) {
        vis = saturate(gDenoisedAo.Load(int3(pixel, 0)).r);
    }

    if (gAverHistoryWrite) gAoHistOut[uint2(pixel)] = float2(vis, curDepth);
    if (tracedNow && gAverHistoryWrite) gAoHitDistOut[uint2(pixel)] = hitDist;
    return rtAoSpatial(vis, wpos, N, pixel, curDepth);
}

// Spatial filter for sun shadows: average neighbours weighted by surface agreement (plane-distance rejection).
float rtShadowSpatial(float centre, float3 wpos, float3 N, float2 pixel, float curDepth) {
    const int radius = (int)gRtDenoiseParams.x;
    if (radius <= 0 || gRtHistParams.y < 0.75) return centre;

    float texW, texH;
    gRtShadowHist.GetDimensions(texW, texH);

    // Gather around where this pixel was last frame (last frame's depth texture).
    const float4 pclip  = mul(float4(wpos + gAverReprojDelta, 1.0), gPrevViewProj);
    const float  pdepth = pclip.w;
    const float  dpdx   = ddx(pdepth);
    const float  dpdy   = ddy(pdepth);
    const float  cdx    = ddx(curDepth);
    const float  cdy    = ddy(curDepth);

    float2 centrePx = pixel;
    bool   reproj   = false;
    if (gRtHistParams.y > 0.75 && pdepth > 1e-4) {
        const float3 pndc = pclip.xyz / pdepth;
        if (pndc.z >= 0.0 && pndc.z <= 1.0) {
            centrePx = gSceneViewport.xy +
                       float2(pndc.x * 0.5 + 0.5, 0.5 - pndc.y * 0.5) * gSceneViewport.zw;
            reproj = true;
        }
    }
    const int2 base = int2(floor(centrePx));

    // Plane-distance rejection: centre depth + gradient defines receiver plane.
    const float planeDepth = reproj ? pdepth : curDepth;
    const float dzdx       = reproj ? dpdx   : cdx;
    const float dzdy       = reproj ? dpdy   : cdy;

    const float sigma  = max((float)radius * 0.5, 0.5);
    const float inv2s2 = 1.0 / (2.0 * sigma * sigma);

    float acc = centre;
    float wsum = 1.0;
    float nSum = 0.0, nSum2 = 0.0, nCount = 0.0;
    [loop] for (int oy = -radius; oy <= radius; ++oy) {
        [loop] for (int ox = -radius; ox <= radius; ++ox) {
            if (ox == 0 && oy == 0) continue;
            const int2 t = base + int2(ox, oy);
            if (any(t < 0) || t.x >= (int)texW || t.y >= (int)texH) continue;
            const float2 st = gRtShadowHist.Load(int3(t, 0));
            const float predicted = planeDepth + dzdx * (float)ox + dzdy * (float)oy;
            const float tol = max(abs(predicted), 1.0) * 0.02 + 1.0;
            if (abs(st.y - predicted) > tol) continue;
#if AVER_GBUFFER_HISTORY
            // Crease test: normal discontinuity (wall meeting floor).
            const float3 nb = gGBufNormalHist.Load(int3(t, 0)).xyz * 2.0 - 1.0;
            if (dot(N, nb) < 0.5) continue;
#endif
            const float w = exp(-(float)(ox * ox + oy * oy) * inv2s2);
            acc  += st.x * w;
            wsum += w;
            nSum += st.x; nSum2 += st.x * st.x; nCount += 1.0;
        }
    }

    // Taper by reprojection velocity: stops shadows flickering under motion.
    // Outlier clamp: mean +/- 2*sigma, sigma floor, min 3 taps.
    float clamped = centre;
    if (nCount >= 3.0) {
        const float mean  = nSum / nCount;
        const float nsig  = sqrt(max(nSum2 / nCount - mean * mean, 0.0));
        const float k     = 2.0 * max(nsig, 0.02);
        clamped = clamp(centre, mean - k, mean + k);
        acc += clamped - centre;
    }

    const float velPx = reproj ? length(centrePx - pixel) : 0.0;
    const float trust = gRtDenoiseParams.z > 0.0 ? saturate((3.0 - velPx) * gRtDenoiseParams.z) : 1.0;
    return lerp(clamped, acc / wsum, saturate(gRtDenoiseParams.y) * trust);
}

// Luminance of shadow tint: scales colour to greyscale for denoiser input.
float averShadowLum(float3 v) { return dot(v, float3(0.2126, 0.7152, 0.0722)); }

#if AVER_RD_LAMPS
// A light's stable key from its position (cm), 1..4095; 0 is a directional light. Unlike its list index, it survives
// the list reordering as the camera moves.
float rdLightKey(RdLocalLight l) {
    if (aversLightKind(l) == AVER_LIGHT_DIRECTIONAL) return 0.0;
    const uint3 q = (uint3)(int3(round(l.posRadius.xyz)) + 1048576);
    return (float)(1u + (((q.x * 73856093u) ^ (q.y * 19349663u) ^ (q.z * 83492791u)) % 4095u));
}

// The direction toward list entry `l` from `wpos`, and the shadow kernels' disc, ray length and history key for it
// (gAverShadow*). A rectangle is traced toward its centre with the disc of its equal-area circle.
float3 rdSetShadowLight(RdLocalLight l, float3 wpos) {
    if (aversLightKind(l) == AVER_LIGHT_DIRECTIONAL) {
        gAverShadowTanR    = -1.0;
        gAverShadowTMax    = 100000.0;
        gAverShadowLightId = 0.0;
        return normalize(l.axisKind.xyz);
    }
    const float3 toC  = l.posRadius.xyz - wpos;
    const float  dist = max(length(toC), 1e-3);
    const float  rad  = aversLightKind(l) == AVER_LIGHT_RECT ? 1.13 * sqrt(max(l.shape.x * l.shape.y, 0.0))
                                                             : l.posRadius.w;
    gAverShadowTanR = rad / dist;
    gAverShadowTMax = max(dist - rad * 1.25, 0.0);
    gAverShadowLightId = rdLightKey(l);
    return toC / dist;
}
void rdResetShadowLight() {
    gAverShadowTanR    = -1.0;
    gAverShadowTMax    = 100000.0;
    gAverShadowLightId = 0.0;
}

// Weight for picking which light gets this pixel's shadow ray, 0 for a light that casts no shadow (it never
// needs a ray and is added unshadowed). Only chooses the ray, the shading evaluates every light exactly, so
// it is the cheap sphere estimate (range window, inverse square, N.L to the centre, spot cone) without IES,
// cookies or the rectangle's form factor: two loops over 32 lights per pixel. A rectangle keeps N.L >= 0.1
// while the receiver faces it, since part of it can be above the horizon when the centre is not.
float rdLightWeight(RdLocalLight l, float3 wpos, float3 N);
float rdLocalPickWeight(RdLocalLight l, float3 wpos, float3 N) {
    // The raster lamp loops leave the directional entry to the sun's own path (gRdSunVisTex).
    if (aversLightNoShadow(l) || aversLightKind(l) == AVER_LIGHT_DIRECTIONAL) return 0.0;
    return rdLightWeight(l, wpos, N);
}
// A light's ranking weight at a point (UNIFIED_LIGHTS.md "Ray budget"): the cheap sphere estimate of its irradiance
// (range window, inverse square, N.L to the centre, spot cone), without IES, cookies or the rectangle's form factor.
// Every loop that only RANKS lights uses it; the shading evaluates each light exactly.
float3 rdLightIrradianceCheap(RdLocalLight l, float3 wpos, float3 N);
float rdLightWeight(RdLocalLight l, float3 wpos, float3 N) {
    return averShadowLum(rdLightIrradianceCheap(l, wpos, N));
}
// rdLightWeight's estimate in colour: irradiance (N.L included). Exact for a sphere or point light without IES or
// cookie (rdLightIsSimple); a ranking estimate for the rest.
bool rdLightIsSimple(RdLocalLight l) {
    const uint kind = aversLightKind(l);
    return (kind == AVER_LIGHT_POINT || kind == AVER_LIGHT_DIRECTIONAL) && l.shape.z < 0.0 && l.shape.w < 0.0;
}
float3 rdLightIrradianceCheap(RdLocalLight l, float3 wpos, float3 N) {
    if (aversLightKind(l) == AVER_LIGHT_DIRECTIONAL)
        return l.radianceRange.rgb * saturate(dot(N, normalize(l.axisKind.xyz)));
    const float3 toC   = l.posRadius.xyz - wpos;
    const float  d2    = dot(toC, toC);
    const float  range = l.radianceRange.w;
    if (d2 >= range * range) return 0.0;
    const float  x2   = d2 / (range * range);
    const float  win  = saturate(1.0 - x2 * x2);
    const float  invD = rsqrt(max(d2, 1e-8));
    const uint   kind = aversLightKind(l);
    float ndl = saturate(dot(N, toC) * invD);
    if (kind == AVER_LIGHT_RECT) {
        if (dot(-toC, l.axisKind.xyz) <= 0.0) return 0.0;
        ndl = max(ndl, 0.1);
    }
    if (kind == AVER_LIGHT_SPOT) {
        const float c = dot(-toC * invD, l.axisKind.xyz);
        if (c <= l.shape.y) return 0.0;
    }
    const float r = l.posRadius.w;
    return l.radianceRange.rgb * ((1e4 / max(d2, r * r)) * (win * win) * ndl);
}

// The strongest 4 lights at a point with their list indices, sorted in registers (fully unrolled compare-swaps).
struct RdTop4 { float w0, w1, w2, w3; uint i0, i1, i2, i3; };
RdTop4 rdTop4Init() {
    RdTop4 r;
    r.w0 = r.w1 = r.w2 = r.w3 = 0.0;
    r.i0 = r.i1 = r.i2 = r.i3 = 0xFFFFFFFFu;
    return r;
}
void rdTop4Add(inout RdTop4 r, float w, uint i) {
    float x = w; uint xi = i; bool sw;
    sw = x > r.w0; { const float tw = sw ? r.w0 : x; const uint ti = sw ? r.i0 : xi; r.w0 = sw ? x : r.w0; r.i0 = sw ? xi : r.i0; x = tw; xi = ti; }
    sw = x > r.w1; { const float tw = sw ? r.w1 : x; const uint ti = sw ? r.i1 : xi; r.w1 = sw ? x : r.w1; r.i1 = sw ? xi : r.i1; x = tw; xi = ti; }
    sw = x > r.w2; { const float tw = sw ? r.w2 : x; const uint ti = sw ? r.i2 : xi; r.w2 = sw ? x : r.w2; r.i2 = sw ? xi : r.i2; x = tw; xi = ti; }
    sw = x > r.w3; if (sw) { r.w3 = x; r.i3 = xi; }
}
// Two quasi-random numbers in [0,1) for a rectangle sample: radical inverse and golden Weyl of the
// pixel's turn, rotated per pixel.
float2 rdLocalRectSample(float2 pixel, uint turn) {
    const float2 rot = float2(rtHash(pixel + float2(5.1, 2.3)), rtHash(pixel + float2(7.7, 1.9)));
    return frac(rot + float2(rtRadicalInverse2(turn + 1u), (float)(turn & 0xFFFFu) * 0.7548776662));
}
#endif

// Normalised colour of tinted visibility: white when no tint.
float3 averShadowTint(float3 v, float lum) {
    return (lum > 1e-4) ? (v / lum) : float3(1.0, 1.0, 1.0);
}

// An occluder arrived or left (often a moving object): the fresh answer differs from history by more
// than penumbra noise can. Follow it instead of trailing it for ~10 frames.
static const float kAverShadowChangeHistory = 0.35;
bool rtShadowChanged(float fresh, float hist) { return abs(fresh - hist) > 0.75; }

// Primary sun-shadow with temporal accumulation and optional tiling (ray amortisation).
float3 rtShadowTemporalEx(float3 wpos, float3 N, float3 L, float2 pixel, float3 dpx, float3 dpy, uint rays,
                          bool haveFresh, float3 freshIn) {
    if (gRtHistParams.x < 0.5) {
        if (haveFresh) return freshIn;
        return rtShadow(wpos, N, L, pixel, dpx, dpy, rays, 0.0);
    }

    const float curDepth = mul(float4(wpos, 1.0), gViewProj).w;
    const uint tileBits = (uint)gRtHistParams.w;

    if (tileBits == 0u) {
        // Non-tiled: every pixel traces every frame, accumulate vs history.
        const float frameJitter = averGoldenTurns((uint)gRtHistParams.z);
        float3 fresh3 = freshIn;
        if (!haveFresh) fresh3 = rtShadow(wpos, N, L, pixel, dpx, dpy, rays, frameJitter);
        const float  fresh   = averShadowLum(fresh3);
        const float3 tint    = averShadowTint(fresh3, fresh);

        float vis = fresh;
        float histV = 0.0;
        float2 velocityPx = 0.0;
        if (gRtHistParams.y > 0.75 && rtReprojectHistory(wpos, pixel, histV, velocityPx)) {
            const float t      = saturate(length(velocityPx) / 32.0);
            const float weight = rtShadowChanged(fresh, histV) ? kAverShadowChangeHistory : lerp(0.9, 0.5, t);
            vis = lerp(fresh, histV, weight);
        }
        if (gAverHistoryWrite) gRtShadowHistOut[uint2(pixel)] = float2(rtShadowHistPack(vis), curDepth);
        return saturate(rtShadowSpatial(vis, wpos, N, pixel, curDepth) * tint);
    }

    // Tiled path: pixels take turns tracing across 2^(2*tileBits) frames.
    const uint frameIdx = (uint)gRtHistParams.z;
    const uint tileMask = (1u << tileBits) - 1u;
    const uint turnMask  = (1u << (2u * tileBits)) - 1u;
    const uint localIdx  = (((uint)pixel.y & tileMask) << tileBits) | ((uint)pixel.x & tileMask);
    const bool myTurn    = localIdx == (frameIdx & turnMask);

    float hist = 0.0;
    float2 velocityPx = 0.0;
    const bool haveHist = gRtHistParams.y > 0.75 && rtReprojectHistory(wpos, pixel, hist, velocityPx);

    float vis;
    float3 tint = float3(1.0, 1.0, 1.0);
    if (myTurn || !haveHist) {
        const float frameJitter = averGoldenTurns(frameIdx);
        float3 vis3 = freshIn;
        if (!haveFresh) vis3 = rtShadow(wpos, N, L, pixel, dpx, dpy, rays, frameJitter);
        vis  = averShadowLum(vis3);
        tint = averShadowTint(vis3, vis);
        if (haveHist) {
            // Adaptive blend: high weight for near-still reprojection, lower as velocity increases.
            const float budget = max(6.0 - 1.5 * (float)tileBits, 1.0);
            const float t = saturate(length(velocityPx) / budget);
            const float weight = rtShadowChanged(vis, hist) ? kAverShadowChangeHistory : lerp(0.9, 0.1, t);
            vis = lerp(vis, hist, weight);
        }
    } else {
        vis = hist;
    }

    if (gAverHistoryWrite) gRtShadowHistOut[uint2(pixel)] = float2(rtShadowHistPack(vis), curDepth);
    return saturate(rtShadowSpatial(vis, wpos, N, pixel, curDepth) * tint);
}

// Entry point: most callers use this (haveFresh=false, so every rtShadow(...) branch runs).
float3 rtShadowTemporal(float3 wpos, float3 N, float3 L, float2 pixel, float3 dpx, float3 dpy, uint rays) {
    return rtShadowTemporalEx(wpos, N, L, pixel, dpx, dpy, rays, false, float3(0.0, 0.0, 0.0));
}

// GGX VNDF sampling: Heitz 2018, JCGT 7(4).
float3 rtSampleGgxVndf(float3 Ve, float alpha, float2 u) {
    const float3 Vh = normalize(float3(alpha * Ve.x, alpha * Ve.y, Ve.z));
    const float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    const float3 T1 = lensq > 0.0 ? float3(-Vh.y, Vh.x, 0.0) * rsqrt(lensq) : float3(1.0, 0.0, 0.0);
    const float3 T2 = cross(Vh, T1);
    const float r = sqrt(u.x);
    const float phi = 6.2831853 * u.y;
    const float t1 = r * cos(phi);
    const float s = 0.5 * (1.0 + Vh.z);
    const float t2 = (1.0 - s) * sqrt(max(0.0, 1.0 - t1 * t1)) + s * r * sin(phi);
    const float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
    return normalize(float3(alpha * Nh.x, alpha * Nh.y, max(1e-6, Nh.z)));
}

#include "voxi_pt.hlsli"

// Trace one reflection ray: shaded hit or sky. One ray/pixel/frame; variance paid by history and spatial filter.
// `hit` true for traced hits (even misses, which sample the lobe like any ray). GGX lobe, sampled by VNDF (alpha=rough^2).
// The last rtReflection call's hit distance (cm), for the reflection denoiser.
static float gAverReflHitT = kAverReflMissT;

float3 rtReflection(float3 wpos, float3 N, float3 Ng, float3 R, float3 L, float2 pixel, float rough,
                    uint frameIdx, out bool hit) {
    hit = false;

    const float alpha = rough * rough;
    const float tanCone = alpha;
    float3 dir = R;
    if (alpha > 0.0) {
        const float3 V  = -reflect(R, N);
        const float3 up = abs(N.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0);
        const float3 T  = normalize(cross(up, N));
        const float3 B  = cross(N, T);
        const float3 Ve = float3(dot(V, T), dot(V, B), max(dot(V, N), 1e-4));
        const float2 u  = frac(float2(rtHash(pixel), rtHash(pixel + float2(17.31, 91.7))) +
                               (float)frameIdx * float2(0.7548776662, 0.5698402910));
        const float3 m  = rtSampleGgxVndf(Ve, alpha, u);
        const float3 H  = T * m.x + B * m.y + N * m.z;
        dir = reflect(-V, H);
    }
    // Clamp to both shading and geometric planes.
    if (dot(dir, N)  <= 1e-3) dir = normalize(dir - N  * (dot(dir, N)  - 1e-3));
    if (dot(dir, Ng) <= 1e-3) dir = normalize(dir - Ng * (dot(dir, Ng) - 1e-3));

    RayDesc r;
    const float bias = max(gRtParams.z, 1e-4) * (1.0 + length(wpos - gCamPos.xyz) * 5e-4);
    r.Origin    = wpos + Ng * bias;
    r.Direction = dir;
    r.TMin      = bias;
    r.TMax      = 1.0e7;

#if AVER_PT_PATHS
    // Path Tracing: the reflected surface is shaded as a full path vertex (voxi_pt.hlsli).
    hit = true;
    const float3 ptRefl = ptRadiance(r.Origin, dir, r.TMin, max(tanCone, 1e-3), pixel, 0x51f3u);
    gAverReflHitT = gAverPtFirstT;
    return ptRefl;
#endif
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gScene, RAY_FLAG_NONE | gAverRtSecondaryRayFlags, AVER_RT_MASK_OPAQUE_ALL, r);
    averRtProceedSolid(q);
    gAverReflHitT = kAverReflMissT;
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
        hit = true;
#if AVER_RD_ABLATE == AVER_RD_ABL_SKY || AVER_RD_ABLATE == AVER_RD_ABL_ALL
        return 0.0;
#else
        return skyColor(dir);
#endif
    }
    const float frameJitter = averGoldenTurns(frameIdx);
    hit = true;

    // The reflected surface, built and lit like every other ray hit: its own maps through the shared
    // composition, the sun through the full BRDF (one shadow ray from this pixel), the lamps, and the
    // sky for its diffuse ambient.
    const RtHit h = rtHitCommitted(q, wpos, dir);
    gAverReflHitT = min(h.t, kAverReflMissT);
    float2 rgx, rgy;
    rtHitConeGrad(h, tanCone, rgx, rgy);
    AverSurface s = rtHitSurface(h, -dir, L, rgx, rgy, AVER_RD_SINGLE_PASS ? AVER_RT_HIT_LITE : AVER_RT_HIT_FULL);
#if AVER_RD_LAMPS
    // A listed lamp's glow is already its sphere light's specular on the surface this ray left.
    if ((h.mat.flags & AVER_MAT_LIGHT) != 0u && rdLocalEmitterCarried(h.pos)) s.emissive = float3(0.0, 0.0, 0.0);
#endif

    // Every emitter, the sun included, through the one evaluator (voxi_pt.hlsli averDirectLights).
    uint lrng = ptSeed(pixel, 0x5e11u);
    const float3 radiance = s.emissive + averDirectLights(s, h.pos, pixel, lrng, false);
    return radiance + s.kdAlbedo * averSkyIrradiance(s.N) * gAmbient.r;
}
