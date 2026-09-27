// Aver.Render.PathTracer: the brute-force path-tracing compute kernel (CSPathTrace).
//
// Moved out of a C++ raw-string literal; composed and -D'd exactly as before.

// ---- the scene a ray reads ---------------------------------------------------------------------
// t0 the acceleration structure; t1 vertices, t2 indices, t3 instances -- three descriptors for the
// whole scene, since this RHI uses explicit descriptor tables, not bindless.
RaytracingAccelerationStructure gPtScene : register(t0);

// MIRRORS rhi::MeshVertex byte for byte; the stride is handed to setSrvBuffer and nothing checks it.
struct PtVertex   { float3 pos; float3 nrm; float2 uv; };
// MIRRORS pt::PtInstance: 64+4+4+12+4+4+4+4+4+4+4+12 = 124 bytes, naturally aligned -- every field
// lands on a 4-byte boundary, so there is no padding to disagree about.
// `ior` is 0.0 for Lambertian, >0 (a real IOR) for a smooth dielectric -- see PtSurface::ior
// (PathTracer.hpp) for why one float carries both the kind and the value. `baseColorTex` is declared
// in BOTH shader variants (only the textured one samples it) so the instance BUFFER layout matches
// either way -- omitting it in the untextured build would read every later instance at the wrong
// offset; `emissive` is a flat emissiveFactor read in both variants like `albedo` (no emissive texture).
struct PtInstance { float4x4 objectToWorld; uint firstIndex; uint firstVertex; float3 albedo; float ior; uint baseColorTex; float roughness; float metallic; uint metalRoughTex; uint normalTex; float normalScale; float3 emissive; };

StructuredBuffer<PtVertex>   gPtVerts     : register(t1);
StructuredBuffer<uint>       gPtIndices   : register(t2);
StructuredBuffer<PtInstance> gPtInstances : register(t3);

// E(cos(theta), roughness): fraction of light a SINGLE-SCATTER GGX lobe (F=1) returns. Row-major,
// roughness outer; built on the CPU by buildEnergyLut() (see its comment: a published analytic fit
// would compensate the wrong lobe). AVER_PT_ENERGY_DIM must equal kEnergyLutDim in both pipelines.
#ifndef AVER_PT_ENERGY_DIM
#error "AVER_PT_ENERGY_DIM must be defined by the pipeline that binds the energy table"
#endif
StructuredBuffer<float>      gPtEnergy    : register(t4);

#ifdef AVER_PT_BINDLESS
// Base-colour table, in space1 so it cannot collide with space0's explicit descriptor table.
// AVER_PT_TEX_CAPACITY must equal PipelineLayout::bindlessTextureCount (kBindlessTexCapacity in
// PathTracer.cpp) -- declaring more here reads past the root signature's range.
#ifndef AVER_PT_TEX_CAPACITY
#error "AVER_PT_TEX_CAPACITY must be defined by the pipeline that declares the bindless table"
#endif
Texture2D    gPtTextures[AVER_PT_TEX_CAPACITY] : register(t0, space1);
SamplerState gPtSamp                           : register(s0);
#define AVER_PT_TEX_UNBOUND 0xFFFFFFFFu
#endif

// The progressive accumulator: TWO float4 elements per pixel; the second is not decoration.
//
//   [2p+0] .rgb  summed radiance, LINEAR units, never tonemapped
//   [2p+1] .x    paths escaped, .y bounce events, .z paths traced
//
// Must stay linear, untonemapped: a missing 1/PI is a global scale no pixel ratio or cross-config
// equality could reveal, so the furnace reads an absolute number -- linear floats out of the buffer,
// never a pixel that a tonemap and exposure have been through. The stats exist because the exact
// answer depends on the ESCAPED fraction (a path out of bounces contributes nothing) -- measured
// per-run, not assumed, and what proves a "single bounce" config really only takes one.
RWStructuredBuffer<float4>   gPtAccum     : register(u0);

// Feature-owned frame constants, at the register RHIResources.hpp reserves for a render feature.
cbuffer PtFrame : register(b4) {
    float4 gPtOrigin;    // xyz camera origin, w tan(half vertical fov)
    float4 gPtForward;   // xyz view axis, w aspect (width / height)
    float4 gPtRight;     // xyz
    float4 gPtUp;        // xyz
    uint4  gPtImage;     // x width, y height, z max bounces, w defect mode
    uint4  gPtSample;    // x first sample index, y samples this dispatch, z reset the accumulator,
                         // w first bounce eligible for Russian roulette (0 = never)
    float4 gPtTrace;     // x ray bias in cm, y tMax in cm, z legacy-environment bit (R5, see
                         // ptEnvironment below; >= 0.5 = old unmatched, < 0.5 = default), w spare
};

// THE DELIBERATE DEFECTS, shipped rather than commented out: a check that has never been shown
// failing proves nothing. Each is the exact arithmetic mistake its estimator is prone to:
//
//   TIMES_PI    Lambertian estimator scaled by PI (= dropping the BRDF's 1/PI). Albedo-1 furnace
//               reads PI*L instead of L.
//   NO_COSINE   rendering-equation cosine dropped from the numerator while the cosine-weighted pdf
//               stays in the denominator -> albedo/cos, mean 2*albedo -> furnace reads 2L.
//   DIELECTRIC_NO_PDF_CANCEL   ptScatterDielectric's Fresnel applied twice -- once as the
//               reflect/refract branch PROBABILITY, once again as a weight. They must cancel to 1;
//               left uncancelled, a non-absorbing dielectric furnace reads L*(F0^2+(1-F0)^2) instead
//               of L. Derivation: PtFurnaceTest.cpp.
#define PT_DEFECT_NONE                     0
#define PT_DEFECT_TIMES_PI                 1
#define PT_DEFECT_NO_COSINE                2
#define PT_DEFECT_DIELECTRIC_NO_PDF_CANCEL 3

// ---- sampling ----------------------------------------------------------------------------------

// A 32-bit integer hash (Wang / "lowbias32"). Used only to decorrelate the seed, never to sample.
uint ptHashU32(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// The path's whole random stream, seeded from (pixel, sampleIndex) and NOTHING ELSE (not the frame
// index, a clock, or the dispatch carrying this sample). PtFurnaceTest replays the first block of
// samples at the end of the run and requires the two accumulators to be BIT-IDENTICAL; a seed pulling
// in SV_GroupIndex, a frame counter or time would still look correct yet fail that check.
uint ptSeed(uint pixel, uint sampleIndex) {
    return ptHashU32(pixel * 0x9E3779B9u ^ ptHashU32(sampleIndex + 0x9E3779B9u));
}

// The next uniform in [0,1). 24 bits, which is also what bounds 1/cos below -- see ptScatter.
float ptRand(inout uint s) {
    s = s * 1664525u + 1013904223u;
    return float((s >> 8) & 0x00FFFFFFu) * (1.0 / 16777216.0);
}

// An orthonormal basis, branchless (Duff et al.), not from a fixed "up" vector: that degenerates near
// the normal, collapsing samples into a plane -- still integrates to L in a furnace, so it would go uncaught.
void ptBasis(float3 n, out float3 t, out float3 b) {
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float c = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

// Cosine-weighted hemisphere sampling, Malley's method: unit length by construction (r*r + z*z ==
// u1 + (1-u1) == 1) -- deliberately NOT renormalised, since normalize() would perturb the cosine the estimator divides by.
float3 ptCosineHemisphere(float3 n, float u1, float u2) {
    float r   = sqrt(u1);
    float phi = 6.28318530718 * u2;
    float z   = sqrt(max(1.0 - u1, 0.0));
    float3 t, b;
    ptBasis(n, t, b);
    return t * (r * cos(phi)) + b * (r * sin(phi)) + n * z;
}

// ---- the scene ---------------------------------------------------------------------------------

// The environment: whatever INDIRECT/ambient radiance arrives from a direction that hit nothing.
// Asks skyColor() (returns averFurnaceL() under the furnace) rather than a private constant, so the
// furnace measures the shipped path. NO SUN IN HERE: skyColor()/skyColorFull() (RHIShaders.cpp) draw
// only a gradient + ground term, no disc (see ptDirectSun below); the sun's contribution is added
// separately at every HIT by ptDirectSun, and the two never overlap.
//
// UNCALIBRATED (R5, contrast-fix plan): skyColor() under the physical atmosphere is averSkyPhysical()
// (shared_prelude.hlsl); the raster's ambient/ReSTIR sky uses the SH fit scaled by
// kSkyIrradianceCalibration (D3D12Device.cpp/VulkanDevice.cpp; 1 since 2026-09-24, 8 before -- the "8x
// dimmer" below dates from then). Only a CAMERA-RAY miss (b == 0) wants this uncalibrated value,
// matching the raster's primary sky and its specular reflections (voxi.hlsl:1146/1148). Every
// INDIRECT miss now reads the calibrated sky instead (fixed at the call site below); it used to fall
// back here for a specular/dielectric bounce while only diffuse was routed to the calibrated sky,
// reading skies 8x apart. This uncalibrated path is otherwise reached only by the legacy-environment
// bit restoring the old unmatched behaviour outright.
float3 ptEnvironment(float3 dir) { return skyColor(dir); }

// ---- the metal/rough lobe -----------------------------------------------------------------------
// GATED ON roughness >= 0 AT EVERY CALL SITE: a surface that never set it stays pure Lambertian, so
// PtFurnaceTest's existing configs keep measuring what they always did. See PtSurface::roughness.

// Is this hit a PBR surface at all, or the plain Lambertian default?
bool ptHasSpecular(float rough) { return rough >= 0.0; }

// Normal incidence reflectance: dielectrics reflect ~4% (colour stays in the diffuse lobe); conductors have no diffuse lobe, colour comes from F0 instead.
float3 ptF0(float3 albedo, float metal) { return lerp(float3(0.04, 0.04, 0.04), albedo, metal); }

// The diffuse albedo left after the specular lobe has taken its share. Metals keep none.
float3 ptDiffuseAlbedo(float3 albedo, float metal) { return albedo * (1.0 - metal); }

// Bilinear fetch from the single-scatter energy table. ENDPOINT-INCLUSIVE, matching buildEnergyLut()
// (cell 0 = 0, cell D-1 = 1) -- a half-texel disagreement here is not cosmetic: it under-compensated
// a fully rough conductor by 5.4%, ten times the furnace's tolerance, while interior points looked correct.
float ptEnergyE(float ndv, float rough) {
    const float D = float(AVER_PT_ENERGY_DIM);
    const float fx = clamp(saturate(ndv)   * (D - 1.0), 0.0, D - 1.0);
    const float fy = clamp(saturate(rough) * (D - 1.0), 0.0, D - 1.0);
    const uint x0 = (uint)fx, y0 = (uint)fy;
    const uint x1 = min(x0 + 1u, (uint)D - 1u), y1 = min(y0 + 1u, (uint)D - 1u);
    const float tx = fx - float(x0), ty = fy - float(y0);
    const float e00 = gPtEnergy[y0 * (uint)D + x0], e10 = gPtEnergy[y0 * (uint)D + x1];
    const float e01 = gPtEnergy[y1 * (uint)D + x0], e11 = gPtEnergy[y1 * (uint)D + x1];
    return max(lerp(lerp(e00, e10, tx), lerp(e01, e11, tx), ty), 1e-2);
}

// MULTIPLE-SCATTERING COMPENSATION, the term that stops a rough conductor reading dark: single-
// scattering models drop the light neighbouring facets would re-scatter, so a lobe that only bounces
// once returns E and loses (1-E). At roughness 1, F0=1, measured E ~= 0.31 -- losing 69% off a white
// metal, worse than the 45% the RASTER furnace caught.
//
// Scales the lobe by 1 + F0*(1/E-1): at F0=1 that's 1/E (every photon back); at F0=0.04, a slight
// lift (only 4% went down this lobe). Kulla-Conty form, F0 standing in for the directional Fresnel
// average -- exact at both metallic endpoints, within the furnace's tolerance between them.
float3 ptSpecCompensation(float3 F0, float E) { return 1.0 + F0 * (1.0 / E - 1.0); }

// What the COMPENSATED specular lobe returns over the hemisphere: F0*E+F0^2*(1-E). Diffuse must give
// way to exactly this, not a bare (1-F0) -- that left the gap unaccounted for; a rough dielectric furnace read 0.973 instead of 1.
float3 ptSpecAlbedo(float3 F0, float E) { return F0 * E + F0 * F0 * (1.0 - E); }

// Samples a GGX half-vector about `n`, returns the REFLECTED direction for `v`. Reuses ptBasis above
// so this lobe and the cosine-weighted one agree on "around the normal" (else two lobes on one
// surface would be misoriented). Drawn from D(h)*cos(h), so the weight below cancels D and cosine analytically.
float3 ptSampleGGX(float3 n, float3 v, float rough, float u1, float u2) {
    const float a = max(rough * rough, 1e-3);   // a=0 is a delta lobe; clamp keeps the maths finite
    const float phi = 2.0 * PI * u1;
    const float ct = sqrt(saturate((1.0 - u2) / (1.0 + (a * a - 1.0) * u2)));
    const float st = sqrt(saturate(1.0 - ct * ct));
    float3 t, b;
    ptBasis(n, t, b);
    const float3 h = normalize(t * (st * cos(phi)) + b * (st * sin(phi)) + n * ct);
    return reflect(-v, h);
}

// The throughput multiplier for a GGX-sampled direction, with D and the sampling density cancelled:
//
//   weight = BRDF * cos(l) / pdf(l)
//          = [D*G*F / (4*ndv*ndl)] * ndl / [D*ndh / (4*vdh)]
//          = F * G * vdh / (ndv * ndh)
//
// D never appears -- the reason to importance-sample: at low roughness both D and the pdf are huge,
// evaluating and dividing loses precision that cancelling keeps. G is Smith (one Schlick term per
// direction) via the shared prelude's plainGeomSchlick, so this file and the rasteriser cannot drift.
float3 ptScatterSpecular(float3 F0, float rough, float3 n, float3 v, float3 l) {
    const float ndv = dot(n, v), ndl = dot(n, l);
    if (!(ndv > 0.0) || !(ndl > 0.0)) return float3(0, 0, 0);   // below the horizon: no energy
    const float3 h = normalize(v + l);
    const float ndh = saturate(dot(n, h)), vdh = saturate(dot(v, h));
    if (!(ndh > 0.0) || !(vdh > 0.0)) return float3(0, 0, 0);
    // k = a/2, the Smith-Schlick pairing this engine's IBL form uses; the furnace decides if that conserves energy, hence a named constant.
    const float a = max(rough * rough, 1e-3);
    const float k = a * 0.5;
    const float G = plainGeomSchlick(ndv, k) * plainGeomSchlick(ndl, k);
    const float3 F = plainFresnelSchlick(vdh, F0);
    // ptEnergyE is keyed on n.v (what the table was integrated over), not n.l or the half-vector --
    // E answers "how much does this lobe return to an observer at this elevation".
    return F * G * vdh / max(ndv * ndh, 1e-6) * ptSpecCompensation(F0, ptEnergyE(ndv, rough));
}

// How often to send a bounce down the specular lobe. ANY value in (0,1) is unbiased (the weight
// divides by it), so this only affects VARIANCE. Tracks F0 to put samples where the energy is
// (conductor: mostly specular, dielectric: mostly diffuse); clamped so neither lobe is ever estimated from zero samples.
float ptSpecularProbability(float3 F0, float metal) {
    const float lum = dot(F0, float3(0.2126, 0.7152, 0.0722));
    return clamp(max(lum, metal), 0.1, 0.9);
}

// Direct light from the sun, by NEXT-EVENT ESTIMATION rather than hoping a bounce finds it.
//
// THE PROBLEM: averSunRadiance() (RHIShaders.cpp) has zero angular size, so no BRDF-sampled ray
// drawn from a continuous density (ptCosineHemisphere) can ever land on it -- a bounce-only tracer
// renders every sun-lit-but-sky-occluded surface (an overhang, a wall facing away from open sky)
// perfectly BLACK -- that used to be this integrator's whole story for direct light: none. THE FIX:
// fire ONE shadow ray straight at the light at every hit ("next-event estimation"). The direction is
// KNOWN, not drawn, so there's no pdf to divide by, and no double-counting with BRDF-sampled bounces
// -- zero angular size means a bounce can never independently rediscover it.
//
// HARD SHADOWS ONLY (one ray, correct for a delta light; VoxiShaders.hpp's soft-shadow routine is for
// a light with an authored angular radius). Costs one extra RayQuery per diffuse hit, against the
// SAME gPtScene already bound at t0 -- no new SRV/register/dependency: this module still links only
// Aver.RHI and Aver.Core.
//
// NOT CALLED FOR A DIELECTRIC HIT (gated in CSPathTrace): the formula below is the irradiance a
// LAMBERTIAN surface reflects; a dielectric has no diffuse lobe for it. `V`/`rough`/`metal` are read
// only when rough >= 0 -- without one, this is the identical Lambertian line the function always
// ended with, so PtFurnaceTest still measures exactly what it measured before.
float3 ptDirectSun(float3 hitPos, float3 nWS, float3 albedo, float bias, float tMax,
                   float3 V, float rough, float metal) {
    float3 L = normalize(gLightDir.xyz);   // "direction TO light" -- see PerFrame in the prelude
    float ndl = dot(nWS, L);
    if (!(ndl > 0.0)) return float3(0, 0, 0);   // the light is behind this surface; no ray to fire

    RayDesc r;
    r.Origin    = hitPos + nWS * bias;   // same normal-offset ptTrace's own callers already use
    r.Direction = L;
    r.TMin      = bias;
    r.TMax      = tMax;

    // ACCEPT_FIRST_HIT_AND_END_SEARCH: an OCCLUSION test, not closest-hit -- the first committed
    // candidate already means the sun is blocked, so there is nothing to gain by looking further.
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(gPtScene, RAY_FLAG_NONE, 0xFF, r);
    q.Proceed();
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) return float3(0, 0, 0);   // occluded

    // Lambertian BRDF (albedo/PI) times cosine times light radiance. NO PDF DIVISION: this direction
    // was CHOSEN (the sun's), not drawn, so there's no density here to divide out.
    if (!ptHasSpecular(rough)) return albedo * (1.0 / PI) * ndl * averSunRadiance();

    // THE PBR SURFACE, evaluated not sampled: D appears in full here (unlike ptScatterSpecular, where
    // importance sampling cancels it), since this direction was chosen, not drawn.
    const float3 F0  = ptF0(albedo, metal);
    const float  ndv = dot(nWS, V);
    // The SAME energy bookkeeping the bounce path uses, so direct and bounce lighting agree.
    const float  E   = ptEnergyE(max(ndv, 1e-3), rough);
    const float3 kd  = ptDiffuseAlbedo(albedo, metal) * (1.0 - ptSpecAlbedo(F0, E));
    float3 brdf = kd * (1.0 / PI);
    if (ndv > 0.0) {
        const float3 H = normalize(V + L);
        const float ndh = saturate(dot(nWS, H)), vdh = saturate(dot(V, H));
        const float a = max(rough * rough, 1e-3);
        const float k = a * 0.5;   // the same pairing ptScatterSpecular uses; they must not drift
        const float  D = plainDistGGX(ndh, a);
        const float  G = plainGeomSchlick(ndv, k) * plainGeomSchlick(ndl, k);
        const float3 F = plainFresnelSchlick(vdh, F0);
        brdf += D * G * F / max(4.0 * ndv * ndl, 1e-6) * ptSpecCompensation(F0, E);
    }
    return brdf * ndl * averSunRadiance();
}

// Traces one ray and resolves the surface it hit. False means the ray left the scene.
//
// `entering` recovers a bit the two-sided flip below would otherwise discard: the flip always turns
// nWS to face against the incoming ray (fine for Lambertian), collapsing front/back into one output --
// but refraction needs that distinction to pick n1/n2 vs n2/n1. Read from the SAME comparison, so a
// diffuse caller that ignores it still gets the identical nWS.
bool ptTrace(float3 org, float3 dir, out float3 hitPos, out float3 nWS, out float3 albedo,
            out float ior, out bool entering, out float rough, out float metal,
            out float3 emissive) {
    hitPos   = org;
    nWS      = float3(0, 0, 1);
    albedo   = float3(0, 0, 0);
    ior      = 0.0;
    entering = true;
    // NEGATIVE IS THE DEFAULT ON A MISS TOO, so a caller that ignores the return value still sees
    // "no specular lobe" rather than a mirror. See PtSurface::roughness.
    rough    = -1.0;
    metal    = 0.0;
    emissive = float3(0, 0, 0);

    RayDesc r;
    r.Origin    = org;
    r.Direction = dir;
    r.TMin      = gPtTrace.x;
    r.TMax      = gPtTrace.y;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gPtScene, RAY_FLAG_NONE, 0xFF, r);
    q.Proceed();
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return false;

    // CommittedInstanceID, never CommittedInstanceIndex: buildTlas skips failed-BLAS instances, so
    // the index shifts and lookups would read the wrong neighbour. See TlasInstance::instanceId.
    PtInstance inst = gPtInstances[q.CommittedInstanceID()];
    uint tri = inst.firstIndex + q.CommittedPrimitiveIndex() * 3;
    uint i0 = inst.firstVertex + gPtIndices[tri + 0];
    uint i1 = inst.firstVertex + gPtIndices[tri + 1];
    uint i2 = inst.firstVertex + gPtIndices[tri + 2];

    float2 bary = q.CommittedTriangleBarycentrics();
    float3 w = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    float3 nObj = normalize(gPtVerts[i0].nrm * w.x + gPtVerts[i1].nrm * w.y + gPtVerts[i2].nrm * w.z);
    // Rotation only. The engine is row-vector, so a direction is the vector times the upper 3x3.
    float3 nGeom = normalize(mul(float4(nObj, 0.0), inst.objectToWorld).xyz);
    // TWO-SIDED, on purpose: the furnace's claim is about geometry, and a surface dark from behind
    // would make the answer depend on triangle winding -- turning an authoring mistake into an energy
    // failure and hiding a real one.
    //
    // `entering` is recorded from the SAME condition the flip already applies (dot(nGeom, dir) > 0 =
    // back side, flip fires, entering false); nWS is bit-identical either way, since
    // `entering ? nGeom : -nGeom` IS that flip, just named.
    entering = !(dot(nGeom, dir) > 0.0);
    nWS = entering ? nGeom : -nGeom;

    hitPos = org + dir * q.CommittedRayT();
    albedo = inst.albedo;
    ior    = inst.ior;
    rough  = inst.roughness;
    metal  = inst.metallic;
    emissive = inst.emissive;   // a factor, not a texture -- read the same way in both shader variants
#ifdef AVER_PT_BINDLESS
    // ONE UV FOR THE WHOLE MATERIAL, computed once whether one map is bound or three.
    const float2 uv = gPtVerts[i0].uv * w.x + gPtVerts[i1].uv * w.y + gPtVerts[i2].uv * w.z;

    // THE FACTOR TIMES THE TEXEL: PtSurface::albedo carries baseColorFactor, not the texture's mean,
    // when a texture is bound (mean x texel would double-apply it).
    if (inst.baseColorTex != AVER_PT_TEX_UNBOUND) {
        // SampleLevel AT MIP 0, not SampleGrad: a compute kernel has no pixel quad or ray footprint to
        // derive a gradient from. Mip 0 aliases, but 1600 jittered samples/pixel integrate that away
        // rather than freezing it; a wrong mip would bias every sample the same way.
        albedo *= gPtTextures[NonUniformResourceIndex(inst.baseColorTex)]
                      .SampleLevel(gPtSamp, uv, 0).rgb;
    }

    // glTF packs occlusion/R, roughness/G, metallic/B. Factors MULTIPLY the sampled channels (not
    // replace them), so `metallicFactor 1` beside a metal map (every Sponza material) doesn't render as a mirror.
    if (rough >= 0.0 && inst.metalRoughTex != AVER_PT_TEX_UNBOUND) {
        const float4 mr = gPtTextures[NonUniformResourceIndex(inst.metalRoughTex)]
                              .SampleLevel(gPtSamp, uv, 0);
        rough = saturate(rough * mr.g);
        metal = saturate(metal * mr.b);
    }

    // TANGENT-SPACE NORMAL MAPPING, frame from the triangle, not a vertex stream: PtVertex has no
    // tangent, so inverting the UV map recovers dP/du, dP/dv exactly -- no extra geometry or upload.
    if (inst.normalTex != AVER_PT_TEX_UNBOUND) {
        const float3 p0 = gPtVerts[i0].pos, p1 = gPtVerts[i1].pos, p2 = gPtVerts[i2].pos;
        const float2 t0 = gPtVerts[i0].uv,  t1 = gPtVerts[i1].uv,  t2 = gPtVerts[i2].uv;
        const float3 e1 = p1 - p0, e2 = p2 - p0;
        const float2 d1 = t1 - t0, d2 = t2 - t0;
        const float  det = d1.x * d2.y - d2.x * d1.y;
        // DEGENERATE UVs ARE LEFT ALONE, not approximated: collinear UVs have no tangent frame, and
        // inventing one would rotate the normal map arbitrarily -- worse than the geometric normal.
        if (abs(det) > 1e-12) {
            const float3 T = mul(float4((e1 * d2.y - e2 * d1.y) / det, 0.0), inst.objectToWorld).xyz;
            float3 nrm = gPtTextures[NonUniformResourceIndex(inst.normalTex)]
                             .SampleLevel(gPtSamp, uv, 0).xyz * 2.0 - 1.0;
            nrm.xy *= inst.normalScale;
            // Gram-Schmidt against the SHADING normal. B from a cross product, not the 2nd UV
            // derivative, so it stays right-handed with mirrored UVs (dP/dv reversed, a raw B would
            // flip the green channel). LENGTH TESTED BEFORE normalize(), NOT AFTER: a tangent parallel
            // to the normal gives NaN post-normalize, and testing the normalised result relies on NaN
            // comparisons being false -- which DXC's default fast-math can assume away.
            const float3 Tperp = T - nWS * dot(nWS, T);
            if (dot(Tperp, Tperp) > 1e-12) {
                const float3 Tn = normalize(Tperp);
                const float3 Bn = cross(nWS, Tn);
                const float3 pert = normalize(Tn * nrm.x + Bn * nrm.y + nWS * nrm.z);
                // A normal map can tip a grazing normal past the horizon (a false shadow); keep it facing the ray.
                if (dot(pert, dir) < 0.0) nWS = pert;
            }
        }
    }
#endif
    return true;
}

// One scatter event: the throughput multiplier for bouncing into `dir` off a LAMBERTIAN surface with
// `albedo`. See ptScatterDielectric below for the other material kind.
//
// WRITTEN OUT IN FULL, ON PURPOSE: the BRDF's 1/PI, the cosine, and the pdf's cos/PI cancel exactly,
// so this could be `return albedo;`. It isn't, because then no defect could remove the cosine or PI,
// and the furnace could never be shown FAILING. Keeping the cancellation numerical costs ~1e-7 relative.
float3 ptScatter(float3 albedo, float cosTheta, uint defect) {
    float3 brdf = albedo * (1.0 / PI);      // Lambertian
    float  pdf  = cosTheta * (1.0 / PI);    // cosine-weighted density of the direction we just drew

    // cosTheta is sqrt(1 - u1) with u1 a 24-bit uniform, so it never drops below 2^-12 and pdf
    // never below ~7.8e-5. The guard is against a caller, not against the sampler.
    float3 weight = brdf * cosTheta / max(pdf, 1e-9);

    if (defect == PT_DEFECT_TIMES_PI)  weight *= PI;
    if (defect == PT_DEFECT_NO_COSINE) weight = brdf / max(pdf, 1e-9);
    return weight;
}

// One scatter event off a SMOOTH DIELECTRIC: Fresnel decides, STOCHASTICALLY, between reflection and
// refraction; Snell's law gives the refracted direction; TIR falls out of the same formula, no
// separate branch needed. Weight is exactly 1 either way -- a delta lobe's only "pdf" is the discrete
// choice probability, which IS the Fresnel weight, so the two cancel. See
// PT_DEFECT_DIELECTRIC_NO_PDF_CANCEL for what leaving them uncancelled looks like.
//
// SCHLICK, NOT THE FULL FRESNEL EQUATIONS: unpolarised light is exactly Schlick's own assumption, and
// every angle needed is already in hand below; within ~1% of the full equations except near the
// critical angle on the low-index side, where it overshoots (Schlick 1994). Costs nothing for the
// energy-conservation claim here, since it only needs a reflectance function in [0,1] whose
// stochastic choice cancels its own probability -- swappable later for %-accurate grazing highlights.
//
// `entering` is what ptTrace recovered: front of the winding (n1=1 air, n2=ior) or the reverse. One
// IOR describes the WHOLE interface. WEIGHT IS 1, NOT `albedo`: models a CLEAR, non-absorbing
// dielectric (window glass, not tinted); a tinted-glass material would need an absorption term folded
// into the transmitted branch's weight -- not modelled today, and pretending otherwise is the kind of
// undocumented approximation this codebase has been burned by before.
void ptScatterDielectric(float3 dir, float3 nWS, bool entering, float ior, uint defect,
                         inout uint rng, out float3 outDir, out float3 weight) {
    const float n1 = entering ? 1.0 : ior;
    const float n2 = entering ? ior : 1.0;
    const float eta = n1 / n2;

    // nWS already faces against dir (ptTrace's invariant), so -dot(dir, nWS) is cosI directly.
    // Clamped only against upstream normalize() float error at grazing incidence.
    const float cosI  = clamp(-dot(dir, nWS), 0.0, 1.0);
    const float sin2T = eta * eta * (1.0 - cosI * cosI);
    // TOTAL INTERNAL REFLECTION falls out of the same line that would otherwise sqrt() a negative
    // number: forcing reflectance to 1 sends the choice below into the reflect branch always -- no separate TIR path needed.
    const bool tir = sin2T > 1.0;

    float reflectance;
    float3 refrDir = float3(0, 0, 0);
    if (tir) {
        reflectance = 1.0;
    } else {
        const float cosT = sqrt(1.0 - sin2T);
        const float r0raw = (n1 - n2) / (n1 + n2);
        const float r0 = r0raw * r0raw;
        // Schlick's grazing-angle term uses the LOW-INDEX side: cosI entering (air), cosT exiting
        // (air is now the far side). cosI unconditionally -- the common shortcut -- under-states
        // reflectance at the shallow exit angle where real glass goes bright.
        const float grazing = entering ? cosI : cosT;
        const float x  = 1.0 - grazing;
        const float x2 = x * x;
        reflectance = r0 + (1.0 - r0) * x2 * x2 * x;
        // Standard vector refraction (Snell's law): t = eta*dir + (eta*cosI-cosT)*nWS, nWS facing
        // against dir. Normalized explicitly against sqrt() float error, same reason nObj/nGeom above are.
        refrDir = normalize(eta * dir + (eta * cosI - cosT) * nWS);
    }

    // ONE RAY PER HIT, chosen with probability `reflectance`, not both terms summed: summing needs
    // two rays to stay unbiased; the stochastic choice gets the same expected value from one.
    const float xi = ptRand(rng);
    if (xi < reflectance) {
        outDir = reflect(dir, nWS);
        weight = (defect == PT_DEFECT_DIELECTRIC_NO_PDF_CANCEL)
                     ? float3(reflectance, reflectance, reflectance)
                     : float3(1, 1, 1);
    } else {
        outDir = refrDir;
        weight = (defect == PT_DEFECT_DIELECTRIC_NO_PDF_CANCEL)
                     ? float3(1.0 - reflectance, 1.0 - reflectance, 1.0 - reflectance)
                     : float3(1, 1, 1);
    }
}

// ---- the integrator ----------------------------------------------------------------------------

[numthreads(8, 8, 1)]
void CSPathTrace(uint3 tid : SV_DispatchThreadID) {
    const uint W = gPtImage.x, H = gPtImage.y;
    if (tid.x >= W || tid.y >= H) return;

    const uint pixel  = tid.y * W + tid.x;
    const uint spp    = max(gPtSample.y, 1u);
    const uint bounce = gPtImage.z;
    const uint defect = gPtImage.w;
    const float bias  = gPtTrace.x;
    // 0 disables it entirely; see PtDispatch::rouletteDepth for why that is the default.
    const uint rrFrom = gPtSample.w;

    float3 sum = float3(0, 0, 0);
    float  escaped = 0.0, bounceEvents = 0.0;

    [loop] for (uint s = 0; s < spp; ++s) {
        uint rng = ptSeed(pixel, gPtSample.x + s);

        // Jittered inside the pixel from the SAME stream, so the sample index alone determines the whole path.
        float2 ndc = ((float2(tid.xy) + float2(ptRand(rng), ptRand(rng))) / float2(W, H)) * 2.0 - 1.0;
        float3 dir = normalize(gPtForward.xyz
                             + gPtRight.xyz * ( ndc.x * gPtOrigin.w * gPtForward.w)
                             + gPtUp.xyz    * (-ndc.y * gPtOrigin.w));
        float3 org = gPtOrigin.xyz;

        float3 throughput = float3(1, 1, 1);
        float3 radiance   = float3(0, 0, 0);
        uint   depth = 0;

        // <= bounce, not <: the last iteration may still MISS and collect the environment, just not
        // scatter again -- off by one here would make a "1 bounce" tracer read 0 instead of L.
        [loop] for (uint b = 0; b <= bounce; ++b) {
            float3 hitPos, nWS, albedo, emissive;
            float ior; bool entering; float rough, metal;
            if (!ptTrace(org, dir, hitPos, nWS, albedo, ior, entering, rough, metal, emissive)) {
                // gPtTrace.z (R5/F6): >= 0.5 = legacy bit, restoring skyColor() on every miss
                // unconditionally (byte-identical to the pre-fix line); < 0.5 (default) matches the
                // raster: b==0 keeps skyColor()'s uncalibrated dome; b>0 is an INDIRECT miss, off ANY
                // lobe, reading the SAME calibrated sky ReSTIR uses (voxi_restir.hlsli:
                // averSkyRadianceCheap(dir)*gAmbient.r). TWO bugs fixed here: specular/dielectric
                // bounces used to be lobe-dependent (via a lastDiffuse flag) and fell back to the
                // uncalibrated sky, reading skies 8x apart; separately, *gAmbient.r was omitted even
                // for diffuse's own correct path. gAmbient.r is exactly 1.0 under every furnace config
                // this ships (SandboxApp::setPtFurnaceTest forces skyLightIntensity to 1), so the
                // multiply is a no-op there, leaving PtFurnaceTest's escaped-fraction identity and
                // every energy check untouched.
                const bool indirectMiss = b > 0;
                radiance += throughput * ((gPtTrace.z < 0.5 && indirectMiss)
                                               ? averSkyRadianceCheap(dir) * gAmbient.r
                                               : ptEnvironment(dir));
                escaped += 1.0;
                break;
            }

            // EMISSION (Le), at every hit including the camera's: a bulb is seen directly and a
            // bounce landing on it carries its light. Weighted by the path so far ONLY (this hit's
            // albedo, folded into throughput below, must not scale its own glow), before the
            // dielectric branch so glowing glass counts too. Not next-event estimated (nothing aims
            // a ray at an emitter) -- noisy for a small emitter, not biased.
            radiance += throughput * emissive;

            // KIND IS ior > 0, NOT A SEPARATE FIELD -- see PtSurface::ior (PathTracer.hpp) for why
            // that sentinel needs no bit-packing and loses no precision versus a quantised kind+ior
            // pair in the same word.
            const bool dielectric = ior > 0.0;

            // DIRECT LIGHT, at every DIFFUSE hit (including the last bounce), NEVER at a dielectric
            // one -- gated here, not inside ptDirectSun, so cost/result are unchanged (see
            // ptDirectSun's comment for why this fixes sky-only lighting, and for why a smooth
            // dielectric has no diffuse lobe for that estimate to approximate). A next-event shadow
            // ray, not a bounce, so it doesn't compete with PtFurnaceTest's escaped-fraction identity
            // below (that's about a MISS; this runs only on a HIT), and costs it provably (not just
            // empirically) nothing: averSunRadiance() (RHIShaders.cpp) is exactly 0 whenever the
            // furnace is on and the sun is off.
            //
            // KNOWN, UNFIXED (OPEN): ptDirectSun's shadow ray treats glass as fully opaque --
            // RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH stops at the first triangle hit, dielectric or
            // not -- so a surface seen only through glass renders fully sun-shadowed instead of
            // dimmed by however much the pane transmits -- separable follow-up work.
            if (!dielectric) {
                // -dir is the view vector this hit's BRDF is evaluated against.
                radiance += throughput * ptDirectSun(hitPos, nWS, albedo, bias, gPtTrace.y,
                                                     -dir, rough, metal);
            }

            // Out of bounces: the path is TRUNCATED, contributing no further INDIRECT light -- why
            // the escaped count is read back (the furnace's exact answer at finite depth is L times
            // the escaped fraction; a terminal environment lookup ignoring occlusion would hide the
            // very bias this test exists to find). (Direct light above is unaffected: evaluated at
            // every hit, not carried forward by a bounce that might never happen.)
            if (b == bounce) break;

            float3 d, weight;
            if (dielectric) {
                // Always produces a valid direction (reflection or refraction, TIR folded in), so
                // there's no failure case here to break for, unlike the Lambertian cosTheta guard below.
                ptScatterDielectric(dir, nWS, entering, ior, defect, rng, d, weight);
            } else if (ptHasSpecular(rough)) {
                // TWO LOBES, ONE SAMPLE: drawing both would double the ray count for an estimator
                // already unbiased with one, so a single uniform picks the lobe and the weight
                // divides by that probability -- from the SAME (pixel, sampleIndex) stream, so
                // replay stays bit-identical.
                const float3 V = -dir;
                const float3 F0 = ptF0(albedo, metal);
                const float pSpec = ptSpecularProbability(F0, metal);
                if (ptRand(rng) < pSpec) {
                    d = ptSampleGGX(nWS, V, rough, ptRand(rng), ptRand(rng));
                    if (!(dot(d, nWS) > 0.0)) break;   // sampled below the horizon; this path ends
                    weight = ptScatterSpecular(F0, rough, nWS, V, d) / pSpec;
                } else {
                    d = ptCosineHemisphere(nWS, ptRand(rng), ptRand(rng));
                    const float cosTheta = dot(d, nWS);
                    if (!(cosTheta > 0.0)) break;
                    // Cosine-weighted weight is the diffuse albedo (ptScatter) scaled so the two
                    // lobes together can't return more energy than arrived: by (1 - what the
                    // specular lobe actually returns), not (1 - F0). The compensated lobe returns
                    // F0*E + F0^2*(1-E), less than F0, so (1-F0) left the gap unaccounted for --
                    // measured as a rough dielectric reading 0.973 L instead of 1. (Fresnel at normal
                    // incidence, the standard pairing with ptScatterSpecular's own F: the diffuse
                    // lobe has no single direction to evaluate F against.)
                    const float3 kd = ptDiffuseAlbedo(albedo, metal)
                                    * (1.0 - ptSpecAlbedo(F0, ptEnergyE(max(dot(nWS, V), 1e-3), rough)));
                    weight = ptScatter(kd, cosTheta, defect) / (1.0 - pSpec);
                }
            } else {
                d = ptCosineHemisphere(nWS, ptRand(rng), ptRand(rng));
                float cosTheta = dot(d, nWS);
                if (!(cosTheta > 0.0)) break;
                weight = ptScatter(albedo, cosTheta, defect);
            }

            throughput *= weight;

            // ---- RUSSIAN ROULETTE ---------------------------------------------------------------
            //
            // Kills a path with probability (1-p), dividing survivors by p. UNBIASED for any p in
            // (0,1]: a survivor carries exactly the expected contribution of the ones that didn't, so
            // only variance moves -- a legitimate optimisation, not an approximation with an error
            // budget. p IS THE THROUGHPUT'S largest channel: a near-full-energy path almost certainly
            // survives, a few-percent one almost certainly stops (it would otherwise pay a
            // closest-hit traversal AND a shadow ray per bounce for little contribution).
            //
            // NOT BEFORE `rrFrom`: rouletting early bounces would kill primary paths (nearly all the
            // image's energy) for little saved traversal -- a path killed at b=0 saves at most
            // `bounce` iterations the early exits above often skip anyway. DRAWN FROM THE SAME `rng`
            // STREAM as every other decision -- a hard requirement for PtFurnaceTest's bit-identical
            // replay; a separate stream, or anything keyed on a frame counter, would break that
            // quietly.
            //
            // CLAMPED AT 1: a specular weight can exceed one channel-wise; p above 1 would DIVIDE
            // survivors' throughput down, silently darkening the bright paths this must not touch.
            if (rrFrom != 0u && b >= rrFrom) {
                const float p = saturate(max(throughput.x, max(throughput.y, throughput.z)));
                // A path with no energy left contributes nothing either way, so it stops
                // unconditionally (and keeps the division below off zero).
                if (!(p > 0.0)) break;
                if (ptRand(rng) >= p) break;
                throughput /= p;
            }

            // THE BIAS OFFSET FOLLOWS THE NEW RAY, not unconditionally the facing normal. Diffuse and
            // specular-REFLECTION bounces both continue on the nWS side (dot(d,nWS)>0), bit-identical
            // to the old `nWS * bias`. A REFRACTED ray continues onto the -nWS side instead, so
            // offsetting along +nWS would push it back into the surface it just crossed.
            float3 offsetN = dot(d, nWS) > 0.0 ? nWS : -nWS;
            org = hitPos + offsetN * bias;
            dir = d;
            depth = b + 1;
        }

        sum += radiance;
        bounceEvents += float(depth);
    }

    const uint a = pixel * 2;
    const float4 stats = float4(escaped, bounceEvents, float(spp), 0.0);
    if (gPtSample.z != 0) {
        gPtAccum[a + 0] = float4(sum, 0.0);
        gPtAccum[a + 1] = stats;
    } else {
        gPtAccum[a + 0] += float4(sum, 0.0);
        gPtAccum[a + 1] += stats;
    }
}
