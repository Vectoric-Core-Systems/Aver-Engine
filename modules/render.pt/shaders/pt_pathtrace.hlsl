// Aver.Render.PathTracer: the brute-force path-tracing compute kernel (CSPathTrace).
//
// Moved out of a C++ raw-string literal; composed and -D'd exactly as before.

// ---- the scene a ray reads ---------------------------------------------------------------------
// t0 the acceleration structure, then the flat geometry a hit is resolved against: t1 vertices,
// t2 indices, t3 instances. Three descriptors for the whole scene rather than one per mesh, because
// this RHI uses explicit descriptor tables and not bindless.
RaytracingAccelerationStructure gPtScene : register(t0);

// MIRRORS rhi::MeshVertex byte for byte; the stride is handed to setSrvBuffer and nothing checks it.
struct PtVertex   { float3 pos; float3 nrm; float2 uv; };
// MIRRORS pt::PtInstance: 64 + 4 + 4 + 12 + 4 + 4 + 4 + 4 + 4 + 4 + 4 = 112 bytes, packed tightly
// with natural alignment (every field lands on a 4-byte boundary, so there is no padding to
// disagree about).
// `ior` used to be the last field and used to be a spare `pad`; it is read as 0.0 for an ordinary
// Lambertian surface and as a real index of refraction (>0) for a smooth dielectric -- see
// PtSurface::ior (PathTracer.hpp) for why that single float carries both the kind and the value with
// no separate flag and no bit-packing.
//
// `baseColorTex` is declared in BOTH shader variants even though only the textured one samples it:
// the instance BUFFER is the same bytes either way, so a struct that omitted the field in the
// untextured build would read every following instance at the wrong offset. Only the SAMPLING is
// conditional, never the layout.
struct PtInstance { float4x4 objectToWorld; uint firstIndex; uint firstVertex; float3 albedo; float ior; uint baseColorTex; float roughness; float metallic; uint metalRoughTex; uint normalTex; float normalScale; };

StructuredBuffer<PtVertex>   gPtVerts     : register(t1);
StructuredBuffer<uint>       gPtIndices   : register(t2);
StructuredBuffer<PtInstance> gPtInstances : register(t3);

// E(cos(theta), roughness): how much of the light a SINGLE-SCATTER GGX lobe with F = 1 actually
// returns. Row-major, roughness outer. Built on the CPU by buildEnergyLut() with this file's own
// estimator -- see its comment for why a published analytic fit would compensate the wrong lobe.
// AVER_PT_ENERGY_DIM must equal kEnergyLutDim; both pipelines are compiled with it defined.
#ifndef AVER_PT_ENERGY_DIM
#error "AVER_PT_ENERGY_DIM must be defined by the pipeline that binds the energy table"
#endif
StructuredBuffer<float>      gPtEnergy    : register(t4);

#ifdef AVER_PT_BINDLESS
// The base-colour table, in space1 so it cannot collide with space0's explicit descriptor table.
// AVER_PT_TEX_CAPACITY must equal PipelineLayout::bindlessTextureCount exactly -- declaring more
// here reads past the root signature's range. Both come from kBindlessTexCapacity in PathTracer.cpp.
#ifndef AVER_PT_TEX_CAPACITY
#error "AVER_PT_TEX_CAPACITY must be defined by the pipeline that declares the bindless table"
#endif
Texture2D    gPtTextures[AVER_PT_TEX_CAPACITY] : register(t0, space1);
SamplerState gPtSamp                           : register(s0);
#define AVER_PT_TEX_UNBOUND 0xFFFFFFFFu
#endif

// The progressive accumulator: TWO float4 elements per pixel, and the second one is not decoration.
//
//   [2p+0] .rgb  summed radiance, in LINEAR units, never tonemapped
//   [2p+1] .x    paths that escaped the scene, .y bounce events, .z paths traced
//
// A missing 1/PI is a GLOBAL SCALE. No ratio between two pixels and no equality between two
// configurations can see one, so the furnace has to be read as an absolute number -- which means
// linear floats out of a buffer, never a pixel that a tonemap and an exposure have been through.
//
// The statistics exist because the furnace's exact value depends on how many paths ESCAPED: a path
// that runs out of bounces contributes nothing, so the honest claim at a finite bounce count is
// "L times the escaped fraction", and that fraction has to be measured by the same paths rather
// than assumed. It is also what proves the single-bounce configuration really is single-bounce.
RWStructuredBuffer<float4>   gPtAccum     : register(u0);

// Feature-owned frame constants, at the register RHIResources.hpp reserves for a render feature.
cbuffer PtFrame : register(b4) {
    float4 gPtOrigin;    // xyz camera origin, w tan(half vertical fov)
    float4 gPtForward;   // xyz view axis, w aspect (width / height)
    float4 gPtRight;     // xyz
    float4 gPtUp;        // xyz
    uint4  gPtImage;     // x width, y height, z max bounces, w defect mode
    uint4  gPtSample;    // x first sample index, y samples this dispatch, z reset the accumulator
    float4 gPtTrace;     // x ray bias in cm, y tMax in cm
};

// THE DELIBERATE DEFECTS. They are shipped, not commented out, because a check that has never been
// shown failing proves nothing -- and the three failures below are the exact arithmetic mistakes
// each estimator is prone to, each with a known wrong answer:
//
//   TIMES_PI    the LAMBERTIAN estimator scaled by PI, which is also what dropping the 1/PI out of
//               the BRDF does. An albedo-1 furnace then reads PI*L instead of L.
//   NO_COSINE   the cosine of the rendering equation dropped from the LAMBERTIAN numerator while the
//               cosine-weighted pdf stays in the denominator. The estimator becomes albedo/cos,
//               whose mean over that pdf is 2*albedo, so the furnace reads 2L.
//   DIELECTRIC_NO_PDF_CANCEL   ptScatterDielectric's Fresnel term applied a SECOND time, as a
//               multiplicative weight on top of already having been the reflect/refract branch
//               PROBABILITY. The two are the same number and must cancel to exactly 1 -- this
//               defect leaves them uncancelled, so a non-absorbing dielectric furnace reads
//               L*(F0^2 + (1-F0)^2) instead of L. See PtFurnaceTest.cpp for the derivation.
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

// The path's whole random stream, seeded from (pixel, sampleIndex) AND NOTHING ELSE.
//
// Not the frame index, not a clock, not the dispatch that happens to be carrying this sample. That
// is what makes progressive accumulation reproducible: re-running sample k of pixel p a hundred
// frames later must trace the identical path, and PtFurnaceTest checks exactly that by replaying
// the first block of samples at the end of the run and requiring the two accumulators to be
// BIT-IDENTICAL. A seed that reached for SV_GroupIndex, a frame counter or a time uniform would
// still look correct in every image and would fail that comparison.
uint ptSeed(uint pixel, uint sampleIndex) {
    return ptHashU32(pixel * 0x9E3779B9u ^ ptHashU32(sampleIndex + 0x9E3779B9u));
}

// The next uniform in [0,1). 24 bits, which is also what bounds 1/cos below -- see ptScatter.
float ptRand(inout uint s) {
    s = s * 1664525u + 1013904223u;
    return float((s >> 8) & 0x00FFFFFFu) * (1.0 / 16777216.0);
}

// An orthonormal basis about a unit normal, branchless (Duff et al.). Building one from a fixed
// "up" vector degenerates when the normal approaches it, and a degenerate tangent frame sends every
// sample into a plane rather than a hemisphere -- which in a furnace still integrates to L and so
// would never be caught by this test.
void ptBasis(float3 n, out float3 t, out float3 b) {
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float c = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

// Cosine-weighted hemisphere sampling by Malley's method: a uniform point on the disc lifted onto
// the hemisphere. The result is unit length by construction (r*r + z*z == u1 + (1 - u1) == 1), so
// it is deliberately NOT renormalised -- a normalize() here would perturb the very cosine the
// estimator divides by.
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
//
// skyColor is the ENGINE's own environment, and it already returns averFurnaceL() when the furnace
// is on. Asking it rather than carrying a private constant is what makes the furnace an oracle over
// the shipped path rather than over a test rig: the integrator has no idea it is being measured.
//
// NO SUN IN HERE, ON PURPOSE. skyColor()/skyColorFull() (RHIShaders.cpp) draw an authored
// horizon-to-zenith gradient and a ground term -- there is no visible sun disc anywhere in that
// function, by design (a disc would need an angular radius and a hard step no BRDF-sampled ray
// could ever land inside, see ptDirectSun below for the general version of that problem). So a path
// that MISSES and reads this function is correctly picking up sky and ground light only; the sun's
// own contribution is added separately, at every HIT, by ptDirectSun -- not here, and the two do
// not overlap.
float3 ptEnvironment(float3 dir) { return skyColor(dir); }

// ---- the metal/rough lobe -----------------------------------------------------------------------
//
// GATED ON roughness >= 0 AT EVERY CALL SITE. A surface that never set it is the pure Lambertian
// this tracer shipped with and takes none of the code below -- which is what keeps PtFurnaceTest's
// existing configurations measuring exactly what they always measured. See PtSurface::roughness.

// Is this hit a PBR surface at all, or the plain Lambertian default?
bool ptHasSpecular(float rough) { return rough >= 0.0; }

// Normal incidence reflectance. A dielectric reflects ~4% and keeps its colour in the diffuse lobe;
// a conductor has no diffuse lobe at all and takes its colour from F0 instead.
float3 ptF0(float3 albedo, float metal) { return lerp(float3(0.04, 0.04, 0.04), albedo, metal); }

// The diffuse albedo left after the specular lobe has taken its share. Metals keep none.
float3 ptDiffuseAlbedo(float3 albedo, float metal) { return albedo * (1.0 - metal); }

// Bilinear fetch from the single-scatter energy table. ENDPOINT-INCLUSIVE, matching how
// buildEnergyLut() placed its samples: cell 0 is exactly 0 and cell D-1 is exactly 1, so a query at
// roughness 1 reads a row integrated at roughness 1. A half-texel disagreement between these two
// mappings is not cosmetic -- it under-compensated a fully rough conductor by 5.4%, which is ten
// times the furnace's tolerance, while every interior point still looked correct.
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

// MULTIPLE-SCATTERING COMPENSATION, the term that makes a rough conductor stop reading dark.
//
// A single-scattering microfacet model lets a facet shadow light and then forgets about it. Real
// microsurfaces scatter that light again off neighbouring facets until it escapes, so a lobe that
// only ever bounces ONCE returns E of what arrived and silently drops (1 - E). At roughness 1 with
// F0 = 1 this tracer measured E ~= 0.31, i.e. it was losing 69% of the light off a white metal --
// the same defect, and worse, than the 45% white metal the RASTER furnace caught.
//
// The correction scales the lobe by 1 + F0*(1/E - 1). At F0 = 1 that is exactly 1/E, so the lobe
// returns E * (1/E) = 1: every photon, which is what a white conductor must do. At F0 = 0.04 it is a
// slight lift, because a dielectric only sent 4% down this lobe in the first place. It is the
// Kulla-Conty form, with F0 standing in for the directional Fresnel average -- exact at both ends of
// the metallic range, and within the furnace's tolerance between them.
float3 ptSpecCompensation(float3 F0, float E) { return 1.0 + F0 * (1.0 / E - 1.0); }

// What the COMPENSATED specular lobe returns over the hemisphere: F0*E scaled by the compensation
// above, which simplifies to F0*E + F0^2*(1 - E). This is the quantity the diffuse lobe must give
// way to, and using it rather than a bare (1 - F0) is what lets a rough DIELECTRIC also read exactly
// L: the specular lobe takes F0*E, not F0, so scaling diffuse by (1 - F0) left the difference
// unaccounted for and the furnace read 0.973 instead of 1.
float3 ptSpecAlbedo(float3 F0, float E) { return F0 * E + F0 * F0 * (1.0 - E); }

// Samples a GGX half-vector about `n` and returns the REFLECTED direction for view vector `v`.
// Reuses ptBasis above rather than building a second tangent frame: the cosine-weighted sampler and
// this one must agree on what "around the normal" means, or two lobes on the same surface would be
// oriented differently and only their SUM would look wrong.
// The half-vector is drawn from D(h)*cos(h), the standard distribution-of-normals sample, so the
// weight below cancels D and the cosine analytically rather than evaluating them and dividing.
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
// D NEVER APPEARS, which is the whole reason to importance-sample the distribution: at low roughness
// D is enormous and the pdf is equally enormous, and evaluating both and dividing loses the precision
// that cancelling them keeps. G is Smith, one Schlick term per direction, sharing the shared
// prelude's plainGeomSchlick so this file and the rasteriser cannot drift on the geometry term.
float3 ptScatterSpecular(float3 F0, float rough, float3 n, float3 v, float3 l) {
    const float ndv = dot(n, v), ndl = dot(n, l);
    if (!(ndv > 0.0) || !(ndl > 0.0)) return float3(0, 0, 0);   // below the horizon: no energy
    const float3 h = normalize(v + l);
    const float ndh = saturate(dot(n, h)), vdh = saturate(dot(v, h));
    if (!(ndh > 0.0) || !(vdh > 0.0)) return float3(0, 0, 0);
    // k = a/2, the Smith-Schlick pairing this engine's IBL form already uses. The furnace is what
    // decides whether that choice conserves energy, and it is the reason this is one named constant
    // rather than an inline literal.
    const float a = max(rough * rough, 1e-3);
    const float k = a * 0.5;
    const float G = plainGeomSchlick(ndv, k) * plainGeomSchlick(ndl, k);
    const float3 F = plainFresnelSchlick(vdh, F0);
    // ptEnergyE is keyed on n.v, the direction the table was integrated over -- not n.l, and not the
    // half-vector. E answers "how much does this lobe return to an observer at this elevation".
    return F * G * vdh / max(ndv * ndh, 1e-6) * ptSpecCompensation(F0, ptEnergyE(ndv, rough));
}

// How often to send a bounce down the specular lobe rather than the diffuse one. ANY value in (0,1)
// is unbiased -- the weight divides by whichever probability was used -- so this only decides
// VARIANCE, never the answer. Tracking F0 puts the samples where the energy is: a conductor is
// almost all specular, a dielectric almost all diffuse. The clamp keeps both lobes reachable, so
// neither is estimated from zero samples on a surface where it still contributes.
float ptSpecularProbability(float3 F0, float metal) {
    const float lum = dot(F0, float3(0.2126, 0.7152, 0.0722));
    return clamp(max(lum, metal), 0.1, 0.9);
}

// Direct light from the sun, by NEXT-EVENT ESTIMATION rather than by hoping a bounce finds it.
//
// THE PROBLEM THIS EXISTS TO FIX. averSunRadiance() (RHIShaders.cpp) is a single direction with no
// angular size at all -- there is no disc, no cone, nothing a BRDF-sampled ray drawn from a
// CONTINUOUS density (ptCosineHemisphere below) could ever land inside; the probability is exactly
// zero. A path tracer that only reaches light by bouncing into the environment therefore renders
// every sun-lit-but-sky-occluded surface -- an overhang, a wall facing away from open sky, the
// underside of anything -- as honestly, perfectly BLACK, no matter how bright the sun is. That used
// to be this integrator's whole story for direct light: none.
//
// THE FIX: at every hit, fire ONE shadow ray straight at the light instead of waiting for a
// scattered ray to find it by chance. This is what "next-event estimation" means -- the next
// lighting EVENT (the sun) is sampled explicitly rather than left to the BRDF's own sampling to
// stumble onto, and because the light has a KNOWN direction (not a drawn one), there is no pdf to
// divide by: sampled with probability one, not weighted by one.
//
// WHY THIS DOES NOT DOUBLE-COUNT WITH BRDF-SAMPLED BOUNCES. Exactly because the sun has zero
// angular size (see above): a bounce can never independently rediscover it, so there is nothing
// here for indirect sampling to count twice and no MIS weight is needed -- unlike a light with real
// solid angle, where both strategies can find it and naively adding both would be wrong.
//
// HARD SHADOWS ONLY. One ray, not an area sample of a disc -- correct for a delta light (there is
// no disc to sample, see above), and simpler than VoxiShaders.hpp's own shadow routine, which
// exists to soften a shadow from a light that DOES have an authored angular radius. If the sun ever
// grows one here too, this is the function that would need the disc-sampling loop that file already
// has.
//
// COSTS ONE EXTRA RayQuery PER DIFFUSE HIT, ALWAYS AGAINST THE SAME gPtScene ALREADY BOUND AT t0 --
// no new SRV, no new register, no new dependency: this module still links only Aver.RHI and
// Aver.Core.
//
// NOT CALLED AT ALL FOR A DIELECTRIC HIT -- gated at the call site in CSPathTrace, not in here, so a
// diffuse hit's cost and result are completely unchanged by the dielectric's existence. The formula
// below is albedo/PI * N.L * sunRadiance, which is the irradiance a LAMBERTIAN surface reflects
// toward the camera; a smooth dielectric has no diffuse lobe for that formula to be estimating, and
// a delta BSDF has exactly zero probability of a BRDF-sampled bounce landing on the sun regardless
// (same reasoning as the "why this does not double-count" note above) -- so there is no compensating
// term missing, only a term that would not apply.
// `V` / `rough` / `metal` DESCRIBE THE SURFACE, and are only read when rough >= 0. A surface that
// never set a roughness takes the identical Lambertian line this function has always ended with --
// not an equivalent one, the same one -- which is what keeps every pre-specular caller, PtFurnaceTest
// above all, measuring exactly what it measured before.
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

    // ACCEPT_FIRST_HIT_AND_END_SEARCH: this is an OCCLUSION test, not a closest-hit query -- the
    // first candidate that commits is already a reason to call the sun blocked, so there is nothing
    // to gain by letting the query keep looking for a closer one.
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(gPtScene, RAY_FLAG_NONE, 0xFF, r);
    q.Proceed();
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) return float3(0, 0, 0);   // occluded

    // Lambertian BRDF (albedo/PI) times the rendering equation's cosine, times the light's own
    // radiance. NO PDF DIVISION -- unlike ptScatter, which divides by the density of a DRAWN
    // direction, this direction was not drawn, it was CHOSEN (the one direction that reaches the
    // sun), so there is no density over directions here to divide out.
    if (!ptHasSpecular(rough)) return albedo * (1.0 / PI) * ndl * averSunRadiance();

    // THE PBR SURFACE, evaluated rather than sampled. D APPEARS IN FULL HERE, unlike
    // ptScatterSpecular where importance sampling cancels it: this direction was chosen, not drawn,
    // so there is no matching density to cancel against and the distribution has to be evaluated.
    const float3 F0  = ptF0(albedo, metal);
    const float  ndv = dot(nWS, V);
    // The SAME energy bookkeeping the bounce path uses, so a surface lit directly and the same
    // surface lit by a bounce do not disagree about how much light it reflects.
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
// `entering` IS THE ONE BIT THIS USED TO THROW AWAY. The two-sided flip below always turns nWS to
// face against the incoming ray -- correct for a Lambertian hit, which only ever cares which side is
// visible -- but it collapses "hit the front" and "hit the back" into the same output, and a
// refraction cannot pick n1/n2 versus n2/n1 (entering the medium vs leaving it) without that
// distinction. Recovered here, from the SAME comparison the flip already made, so a diffuse caller
// that never reads it gets the identical nWS it always did.
bool ptTrace(float3 org, float3 dir, out float3 hitPos, out float3 nWS, out float3 albedo,
            out float ior, out bool entering, out float rough, out float metal) {
    hitPos   = org;
    nWS      = float3(0, 0, 1);
    albedo   = float3(0, 0, 0);
    ior      = 0.0;
    entering = true;
    // NEGATIVE IS THE DEFAULT ON A MISS TOO, so a caller that ignores the return value still sees
    // "no specular lobe" rather than a mirror. See PtSurface::roughness.
    rough    = -1.0;
    metal    = 0.0;

    RayDesc r;
    r.Origin    = org;
    r.Direction = dir;
    r.TMin      = gPtTrace.x;
    r.TMax      = gPtTrace.y;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gPtScene, RAY_FLAG_NONE, 0xFF, r);
    q.Proceed();
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return false;

    // CommittedInstanceID, never CommittedInstanceIndex: buildTlas skips instances whose
    // acceleration structure failed, so the index shifts and every later lookup reads its
    // neighbour's geometry. See TlasInstance::instanceId.
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
    // TWO-SIDED, on purpose. The furnace's claim is about geometry, and a surface that is dark from
    // behind would make the answer depend on which way a triangle was wound -- turning an authoring
    // mistake into an energy failure and hiding a real one.
    //
    // `entering` is recorded from EXACTLY the condition the flip below used to apply unconditionally
    // (dot(nGeom, dir) > 0 means the ray struck the side the mesh's own winding calls the back, so
    // the flip fires and entering is false); nWS ends up bit-identical to the old single-line version
    // either way, since `entering ? nGeom : -nGeom` IS that flip, just with its condition named.
    entering = !(dot(nGeom, dir) > 0.0);
    nWS = entering ? nGeom : -nGeom;

    hitPos = org + dir * q.CommittedRayT();
    albedo = inst.albedo;
    ior    = inst.ior;
    rough  = inst.roughness;
    metal  = inst.metallic;
#ifdef AVER_PT_BINDLESS
    // ONE UV FOR THE WHOLE MATERIAL, computed once whether one map is bound or three.
    const float2 uv = gPtVerts[i0].uv * w.x + gPtVerts[i1].uv * w.y + gPtVerts[i2].uv * w.z;

    // THE FACTOR TIMES THE TEXEL, which is why PtSurface::albedo must carry baseColorFactor and not
    // the texture's mean when a texture is bound -- mean x texel applies the texture twice.
    if (inst.baseColorTex != AVER_PT_TEX_UNBOUND) {
        // SampleLevel AT MIP 0, not SampleGrad. A compute kernel has no pixel quad and therefore no
        // implicit derivatives, and after the first cosine-weighted bounce there is no ray footprint
        // to derive one from either -- the differential a primary hit could carry says nothing about
        // where a diffusely scattered ray lands. Mip 0 aliases, but this integrator averages 1600
        // jittered samples per pixel, which integrates that aliasing away rather than freezing it;
        // a wrong mip would instead bias every one of those samples the same way.
        albedo *= gPtTextures[NonUniformResourceIndex(inst.baseColorTex)]
                      .SampleLevel(gPtSamp, uv, 0).rgb;
    }

    // glTF packs occlusion in R, ROUGHNESS IN G AND METALLIC IN B. The factors multiply the sampled
    // channels rather than replacing them -- which is what stops a material authoring
    // `metallicFactor 1` beside a metal map (every Sponza material does) from rendering as a mirror.
    if (rough >= 0.0 && inst.metalRoughTex != AVER_PT_TEX_UNBOUND) {
        const float4 mr = gPtTextures[NonUniformResourceIndex(inst.metalRoughTex)]
                              .SampleLevel(gPtSamp, uv, 0);
        rough = saturate(rough * mr.g);
        metal = saturate(metal * mr.b);
    }

    // TANGENT-SPACE NORMAL MAPPING, with the frame derived from the triangle rather than a vertex
    // stream: PtVertex carries no tangent, and inverting the position-to-UV map of the three
    // vertices recovers dP/du and dP/dv exactly, which IS the frame a tangent-space map is authored
    // against. No extra geometry, no extra upload.
    if (inst.normalTex != AVER_PT_TEX_UNBOUND) {
        const float3 p0 = gPtVerts[i0].pos, p1 = gPtVerts[i1].pos, p2 = gPtVerts[i2].pos;
        const float2 t0 = gPtVerts[i0].uv,  t1 = gPtVerts[i1].uv,  t2 = gPtVerts[i2].uv;
        const float3 e1 = p1 - p0, e2 = p2 - p0;
        const float2 d1 = t1 - t0, d2 = t2 - t0;
        const float  det = d1.x * d2.y - d2.x * d1.y;
        // DEGENERATE UVs ARE LEFT ALONE, not approximated. A triangle whose UVs are collinear has no
        // tangent frame at all, and inventing one puts a normal map on it in an arbitrary rotation --
        // worse than the geometric normal it already had.
        if (abs(det) > 1e-12) {
            const float3 T = mul(float4((e1 * d2.y - e2 * d1.y) / det, 0.0), inst.objectToWorld).xyz;
            float3 nrm = gPtTextures[NonUniformResourceIndex(inst.normalTex)]
                             .SampleLevel(gPtSamp, uv, 0).xyz * 2.0 - 1.0;
            nrm.xy *= inst.normalScale;
            // Gram-Schmidt against the SHADING normal, so the frame is orthonormal about the normal
            // this hit actually uses. B from a cross product rather than the second UV derivative:
            // that keeps the frame right-handed even on a mesh with mirrored UVs, where dP/dv points
            // the opposite way and a raw B would flip the map's green channel.
            // LENGTH TESTED BEFORE normalize(), NOT AFTER. A tangent parallel to the normal leaves
            // nothing after the projection, and normalize() of that is NaN -- but checking the
            // normalised result relies on NaN comparisons being false, which DXC is entitled to
            // assume away under its default fast-math. Testing the vector that is about to be
            // normalised needs no such assumption.
            const float3 Tperp = T - nWS * dot(nWS, T);
            if (dot(Tperp, Tperp) > 1e-12) {
                const float3 Tn = normalize(Tperp);
                const float3 Bn = cross(nWS, Tn);
                const float3 pert = normalize(Tn * nrm.x + Bn * nrm.y + nWS * nrm.z);
                // A normal map can tip a grazing normal past the horizon, which would shade the
                // surface from behind and read as a false shadow. Keep it facing the ray.
                if (dot(pert, dir) < 0.0) nWS = pert;
            }
        }
    }
#endif
    return true;
}

// One scatter event: the throughput multiplier for bouncing into `dir` off a LAMBERTIAN surface with
// `albedo`. See ptScatterDielectric below for the other material kind this file supports.
//
// WRITTEN OUT IN FULL, AND THAT IS THE POINT. For cosine-weighted sampling the 1/PI of the
// Lambertian BRDF, the cosine of the rendering equation and the cos/PI of the pdf cancel exactly,
// so the whole thing could be spelled `return albedo;`. It is not, because then there would be no
// cosine and no PI left in the code for a defect to remove, and the furnace could never be shown
// FAILING -- a check that has never failed proves nothing. Keeping the cancellation NUMERICAL
// costs about 1e-7 relative and buys an oracle with teeth.
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

// One scatter event off a SMOOTH DIELECTRIC: Fresnel decides, STOCHASTICALLY rather than by
// blending, between specular reflection and refraction; Snell's law gives the refracted direction;
// total internal reflection falls out of the SAME formula rather than needing its own branch, and
// the weight is exactly 1 on whichever branch is taken -- a delta lobe has no continuous pdf to
// write out the way ptScatter's cosine/PI does, only the discrete probability of the choice, and
// that probability IS the Fresnel weight, so the two cancel. See PT_DEFECT_DIELECTRIC_NO_PDF_CANCEL
// for what leaving them uncancelled looks like.
//
// SCHLICK, NOT THE FULL FRESNEL DIELECTRIC EQUATIONS. Every angle Schlick needs (cosI, and cosT
// where it applies) is already in hand from the refraction test below; unpolarised light is exactly
// Schlick's own assumption; and it is within about 1% of the full equations everywhere except very
// close to the critical angle on the low-index side, where it overshoots (Schlick 1994). The choice
// costs nothing for the energy-conservation claim this file checks -- that claim holds for ANY
// reflectance function returning a value in [0,1], because it rests on the stochastic choice
// cancelling its own probability, not on which curve computed that probability. A caller that later
// wants %-accurate grazing highlights on rough glass can swap this formula without touching anything
// else here.
//
// `entering` is exactly what ptTrace recovered: true when the ray arrives from the side the mesh's
// winding calls the front (n1 = 1 air, n2 = ior), false on the reverse (n1 = ior, n2 = 1 air). One
// IOR describes the WHOLE interface; which side counts as air depends only on which way the ray is
// travelling through it.
//
// WEIGHT IS 1, NOT `albedo`. This models a CLEAR, non-absorbing dielectric -- ordinary window glass,
// not smoked or tinted -- so nothing here scatters a fraction of the light diffusely as well as
// specularly. A future tinted-glass material would need to fold an absorption term into the
// transmitted branch's weight and would need to say so as plainly as this comment does; it is not
// modelled today, and pretending otherwise would be exactly the kind of approximation this codebase
// has been burned by leaving undocumented.
void ptScatterDielectric(float3 dir, float3 nWS, bool entering, float ior, uint defect,
                         inout uint rng, out float3 outDir, out float3 weight) {
    const float n1 = entering ? 1.0 : ior;
    const float n2 = entering ? ior : 1.0;
    const float eta = n1 / n2;

    // nWS already faces against dir (ptTrace's own invariant, unconditional on entering/exiting), so
    // -dot(dir, nWS) is cosI directly. Clamped only against float error introduced by the normalize()
    // calls upstream at exact grazing incidence -- a real input never drives this outside [0,1].
    const float cosI  = clamp(-dot(dir, nWS), 0.0, 1.0);
    const float sin2T = eta * eta * (1.0 - cosI * cosI);
    // TOTAL INTERNAL REFLECTION, HANDLED BY THE SAME LINE THAT WOULD OTHERWISE TAKE sqrt() OF A
    // NEGATIVE NUMBER. There is no separate TIR code path to remember to write: forcing reflectance
    // to 1 here means the stochastic choice below always lands in the reflect branch, which is
    // exactly what a real dielectric does past the critical angle.
    const bool tir = sin2T > 1.0;

    float reflectance;
    float3 refrDir = float3(0, 0, 0);
    if (tir) {
        reflectance = 1.0;
    } else {
        const float cosT = sqrt(1.0 - sin2T);
        const float r0raw = (n1 - n2) / (n1 + n2);
        const float r0 = r0raw * r0raw;
        // Schlick's grazing-angle term is measured on the LOW-INDEX side of the interface: cosI when
        // entering (air, the low-index side, is where the incident ray already is), cosT when
        // exiting (air is now on the FAR side, and cosT is the angle the ray makes there). Using cosI
        // unconditionally is the common shortcut and is wrong on the way out -- it under-states
        // reflectance at exactly the shallow exit angle where real glass goes bright.
        const float grazing = entering ? cosI : cosT;
        const float x  = 1.0 - grazing;
        const float x2 = x * x;
        reflectance = r0 + (1.0 - r0) * x2 * x2 * x;
        // Standard vector refraction (Snell's law in vector form): with nWS facing against dir,
        // t = eta*dir + (eta*cosI - cosT)*nWS. Explicitly normalized as insurance against the float
        // error sqrt() can introduce, the same reason nObj/nGeom above are.
        refrDir = normalize(eta * dir + (eta * cosI - cosT) * nWS);
    }

    // ONE RAY PER HIT, chosen with probability `reflectance`, not both terms summed. Summing would
    // need two rays (and normally two recursive calls) to stay unbiased; the stochastic choice gets
    // the same expected value from one, which is what a path tracer that only ever carries one
    // `dir`/`throughput` per path needs.
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

    float3 sum = float3(0, 0, 0);
    float  escaped = 0.0, bounceEvents = 0.0;

    [loop] for (uint s = 0; s < spp; ++s) {
        uint rng = ptSeed(pixel, gPtSample.x + s);

        // Jittered inside the pixel, from the SAME stream, so the sample index alone still
        // determines the whole path.
        float2 ndc = ((float2(tid.xy) + float2(ptRand(rng), ptRand(rng))) / float2(W, H)) * 2.0 - 1.0;
        float3 dir = normalize(gPtForward.xyz
                             + gPtRight.xyz * ( ndc.x * gPtOrigin.w * gPtForward.w)
                             + gPtUp.xyz    * (-ndc.y * gPtOrigin.w));
        float3 org = gPtOrigin.xyz;

        float3 throughput = float3(1, 1, 1);
        float3 radiance   = float3(0, 0, 0);
        uint   depth = 0;

        // <= bounce, not < : the last iteration is allowed to MISS and collect the environment,
        // it is only forbidden to scatter again. Off by one here would make a "1 bounce" path
        // tracer collect nothing at all and read 0 rather than L.
        [loop] for (uint b = 0; b <= bounce; ++b) {
            float3 hitPos, nWS, albedo;
            float ior; bool entering; float rough, metal;
            if (!ptTrace(org, dir, hitPos, nWS, albedo, ior, entering, rough, metal)) {
                radiance += throughput * ptEnvironment(dir);
                escaped += 1.0;
                break;
            }

            // KIND IS ior > 0, NOT A SEPARATE FIELD -- see PtSurface::ior (PathTracer.hpp) for why
            // that sentinel needs no bit-packing and loses no precision versus a quantised kind+ior
            // pair squeezed into the same word.
            const bool dielectric = ior > 0.0;

            // DIRECT LIGHT, AT EVERY DIFFUSE HIT, INCLUDING THE LAST ONE THE BOUNCE BUDGET ALLOWS --
            // BUT NEVER AT A DIELECTRIC ONE. See ptDirectSun's own comment for why this is the fix
            // for sky-only lighting on a Lambertian surface, and for why a smooth dielectric has no
            // diffuse lobe for that same next-event estimate to be approximating: gated here, not
            // inside ptDirectSun, so a diffuse hit's cost and result are unchanged. It is a next-event
            // shadow ray, not a bounce, so it does not compete with the escaped-fraction identity
            // PtFurnaceTest checks below -- that identity is about what `radiance` collects on a MISS,
            // and this line only ever runs on a HIT. It also costs the furnace test nothing to check:
            // averSunRadiance() (RHIShaders.cpp) returns exactly 0 whenever the furnace is on and the
            // sun is off, which is every configuration PtFurnaceTest actually asserts a number for, so
            // this term is provably zero there, not just empirically small.
            //
            // A KNOWN, STATED APPROXIMATION THIS DOES NOT FIX: ptDirectSun's own shadow ray (fired
            // from a DIFFUSE hit behind a pane of glass) still treats the glass as a fully opaque
            // occluder -- RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH stops at the first triangle hit at
            // all, dielectric or not. A surface seen only through glass renders fully sun-shadowed
            // rather than dimmed by however much the pane actually transmits. Fixing that needs the
            // shadow ray itself to reason about transmission (or ignore dielectric hits outright),
            // which is a real, separable piece of follow-up work, not a consequence of anything this
            // change gets to skip quietly.
            if (!dielectric) {
                // -dir is the direction back toward where this ray came from, which is the view
                // vector this hit's BRDF is evaluated against.
                radiance += throughput * ptDirectSun(hitPos, nWS, albedo, bias, gPtTrace.y,
                                                     -dir, rough, metal);
            }

            // Out of bounces. The path is TRUNCATED and contributes no further INDIRECT light,
            // which is why the escaped count above is read back: at a finite bounce count the
            // furnace's exact answer is L times the escaped fraction, and pretending otherwise
            // would need a terminal environment lookup that ignores occlusion -- a bias that would
            // hide exactly the sort of error this exists to find. (The direct-light term just above
            // is unaffected by that truncation: it is evaluated at every hit, not carried forward by
            // a bounce that might never happen.)
            if (b == bounce) break;

            float3 d, weight;
            if (dielectric) {
                // Always produces a valid direction -- reflection or refraction, with total internal
                // reflection folded into the reflection branch -- so there is no failure case here
                // to break out of the loop for, unlike the Lambertian branch's cosTheta guard below.
                ptScatterDielectric(dir, nWS, entering, ior, defect, rng, d, weight);
            } else if (ptHasSpecular(rough)) {
                // TWO LOBES, ONE SAMPLE. Drawing from both and adding would double the ray count for
                // an estimator that is already unbiased with one, so a single uniform picks which
                // lobe this bounce follows and the weight divides by that probability. The choice is
                // a draw from the SAME (pixel, sampleIndex) stream as everything else, so the path
                // stays a pure function of its seed and the bit-identical replay still holds.
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
                    // The cosine-weighted weight is the diffuse albedo (see ptScatter), scaled by
                    // (1 - F) so the two lobes together cannot return more energy than arrived.
                    // Fresnel at normal incidence rather than per-direction: the diffuse lobe has no
                    // single direction to evaluate F against, and this is the standard pairing with
                    // ptScatterSpecular's own F.
                    // (1 - what the specular lobe actually returns), NOT (1 - F0). The compensated
                    // lobe hands back F0*E + F0^2*(1-E), which is less than F0 at any real roughness,
                    // so scaling by (1 - F0) would leave the difference unaccounted for -- measured as
                    // a rough dielectric reading 0.973 L instead of 1.
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
            // THE BIAS OFFSET FOLLOWS THE NEW RAY, NOT UNCONDITIONALLY THE FACING NORMAL. A diffuse
            // bounce and a specular REFLECTION both continue on the nWS side of the surface (dot(d,
            // nWS) > 0 by construction) -- exactly where the old `nWS * bias` always put them, so this
            // is bit-identical for both of those. A REFRACTED ray is the one direction that does not:
            // it continues THROUGH the interface onto the -nWS side, and offsetting it along +nWS
            // would push the new origin back into the surface it just crossed instead of away from it.
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
