#pragma once

// The path tracer's HLSL, compiled as the TAIL of rhi::sharedShaderPrelude(), which already
// declares PerFrame at b0, PI, srgbToLin, skyColor and the averFurnace* contract. Declaration
// order is load-bearing: HLSL has no forward declarations, so a helper used before it is written
// compiles in C++ and fails in DXC, at RUNTIME, on a build that reported success.
namespace aver::pt {

inline constexpr const char* kPathTracerHLSL = R"(
// ---- the scene a ray reads ---------------------------------------------------------------------
// t0 the acceleration structure, then the flat geometry a hit is resolved against: t1 vertices,
// t2 indices, t3 instances. Three descriptors for the whole scene rather than one per mesh, because
// this RHI uses explicit descriptor tables and not bindless.
RaytracingAccelerationStructure gPtScene : register(t0);

// MIRRORS rhi::MeshVertex byte for byte; the stride is handed to setSrvBuffer and nothing checks it.
struct PtVertex   { float3 pos; float3 nrm; float2 uv; };
// MIRRORS pt::PtInstance: 64 + 4 + 4 + 12 + 4 = 88 bytes, packed tightly with natural alignment.
struct PtInstance { float4x4 objectToWorld; uint firstIndex; uint firstVertex; float3 albedo; uint pad; };

StructuredBuffer<PtVertex>   gPtVerts     : register(t1);
StructuredBuffer<uint>       gPtIndices   : register(t2);
StructuredBuffer<PtInstance> gPtInstances : register(t3);

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
// shown failing proves nothing -- and the two failures below are the exact arithmetic mistakes a
// cosine-weighted Lambertian estimator is prone to, each with a known wrong answer:
//
//   TIMES_PI  the estimator scaled by PI, which is also what dropping the 1/PI out of the
//             Lambertian BRDF does. An albedo-1 furnace then reads PI*L instead of L.
//   NO_COSINE the cosine of the rendering equation dropped from the numerator while the
//             cosine-weighted pdf stays in the denominator. The estimator becomes albedo/cos,
//             whose mean over that pdf is 2*albedo, so the furnace reads 2L.
#define PT_DEFECT_NONE      0
#define PT_DEFECT_TIMES_PI  1
#define PT_DEFECT_NO_COSINE 2

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

// The environment: whatever radiance arrives from a direction that hit nothing.
//
// skyColor is the ENGINE's own environment, and it already returns averFurnaceL() when the furnace
// is on. Asking it rather than carrying a private constant is what makes the furnace an oracle over
// the shipped path rather than over a test rig: the integrator has no idea it is being measured.
float3 ptEnvironment(float3 dir) { return skyColor(dir); }

// Traces one ray and resolves the surface it hit. False means the ray left the scene.
bool ptTrace(float3 org, float3 dir, out float3 hitPos, out float3 nWS, out float3 albedo) {
    hitPos = org;
    nWS    = float3(0, 0, 1);
    albedo = float3(0, 0, 0);

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
    nWS = normalize(mul(float4(nObj, 0.0), inst.objectToWorld).xyz);
    // TWO-SIDED, on purpose. The furnace's claim is about geometry, and a surface that is dark from
    // behind would make the answer depend on which way a triangle was wound -- turning an authoring
    // mistake into an energy failure and hiding a real one.
    if (dot(nWS, dir) > 0.0) nWS = -nWS;

    hitPos = org + dir * q.CommittedRayT();
    albedo = inst.albedo;
    return true;
}

// One scatter event: the throughput multiplier for bouncing into `dir` off a surface with `albedo`.
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
            if (!ptTrace(org, dir, hitPos, nWS, albedo)) {
                radiance += throughput * ptEnvironment(dir);
                escaped += 1.0;
                break;
            }
            // Out of bounces. The path is TRUNCATED and contributes nothing, which is why the
            // escaped count above is read back: at a finite bounce count the furnace's exact
            // answer is L times the escaped fraction, and pretending otherwise would need a
            // terminal environment lookup that ignores occlusion -- a bias that would hide
            // exactly the sort of error this exists to find.
            if (b == bounce) break;

            float3 d = ptCosineHemisphere(nWS, ptRand(rng), ptRand(rng));
            float cosTheta = dot(d, nWS);
            if (!(cosTheta > 0.0)) break;

            throughput *= ptScatter(albedo, cosTheta, defect);
            org = hitPos + nWS * bias;
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
)";

} // namespace aver::pt
