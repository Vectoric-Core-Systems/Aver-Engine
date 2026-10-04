// voxi_reservoir.hlsli -- Aver's own ReSTIR GI reservoir: the data type, its 32-byte packing, the
// streaming RIS update, the reconnection Jacobian, the finalised contribution weight, a PCG random
// stream and the spatial-tap disc offsets. Written in-house from the published papers:
//   Talbot et al. 2005, "Importance Resampling for Global Illumination" (RIS);
//   Bitterli et al. 2020, "Spatiotemporal Reservoir Resampling for Real-Time Ray Tracing with
//     Dynamic Direct Lighting" (streaming reservoirs, M-capped reuse, the 1/Z and MIS
//     normalisations for unbiased combining);
//   Ouyang et al. 2021, "ReSTIR GI: Path Resampling for Real-Time Path Tracing" (a reservoir holds a
//     secondary-hit POSITION, reused across receivers through a solid-angle Jacobian);
//   Lin et al. 2022, "Generalized Resampled Importance Sampling" (contribution weights, confidence M,
//     shift-mapped weights W * |J|);
//   Jarzynski & Olano 2020, "Hash Functions for GPU Rendering" (PCG hash);
//   Cigolle et al. 2014, "A Survey of Efficient Representations for Independent Unit Vectors"
//     (octahedral normal encoding).
//
// ENGINE-FREE ON PURPOSE: nothing here names a Voxi resource, cbuffer or RT helper. Loading,
// storing, surface lookups, the target function and the neighbour walk itself live in
// voxi_restir.hlsli, which #includes this file; this file is the maths those call.
//
// NO PER-CELL LIGHT RESERVOIRS: a reservoir here is always per PIXEL and holds one secondary-hit
// sample. Nothing in this file builds a world-space grid of stochastic light reservoirs, and nothing
// may be added that does.

#ifndef VOXI_RESERVOIR_HLSLI
#define VOXI_RESERVOIR_HLSLI

// ---- the reservoir ----
// position/normal: the secondary hit the receiver reconnects to (a sky miss stores a point at the
//   ray's far end with the normal facing back along it, so the Jacobian still sees real geometry).
// radiance: what leaves that hit toward the receiver that traced it.
// W: before giFinalizeResampling, the running RIS weight sum; after it, the sample's unbiased
//   contribution weight (an estimate of 1/pdf, in solid angle at the receiver that owns it).
// M: confidence -- how many candidates this reservoir stands for. 0 means empty.
// age: frames since this sample was traced; reused samples older than the caller's cap are dropped.
struct GiReservoir {
    float3 position;
    float3 normal;
    float3 radiance;
    float  W;
    uint   M;
    uint   age;
};

// 32 bytes, two uint4 lanes, matching VoxiRenderer.cpp's kGiReservoirElemBytes:
//   a.xyz  position (fp32)                a.w  normal, octahedral snorm16x2
//   b.x    radiance r | g << 16 (fp16)    b.y  radiance b (fp16) | M << 16 (8 bits) | age << 24
//   b.z    W (fp32)                       b.w  reserved, written 0
// A zero-filled buffer unpacks with M == 0, i.e. empty -- a freshly allocated buffer needs no clear.
struct GiPackedReservoir {
    uint4 a;
    uint4 b;
};

// The packed M and age fields are 8 bits each; anything larger saturates rather than wraps.
#define GI_RESERVOIR_MAX_M   255u
#define GI_RESERVOIR_MAX_AGE 255u
// Largest finite fp16. Radiance is clamped to the engine's own ceiling long before this, so this
// only keeps a pathological value from packing as fp16 infinity.
#define GI_RESERVOIR_MAX_HALF 65504.0

GiReservoir giEmptyReservoir() {
    GiReservoir r;
    r.position = 0.0;
    r.normal   = float3(0.0, 0.0, 1.0);
    r.radiance = 0.0;
    r.W        = 0.0;
    r.M        = 0u;
    r.age      = 0u;
    return r;
}

bool giIsValidReservoir(GiReservoir r) { return r.M != 0u; }

// A fresh candidate: one sample drawn with density `samplePdf` (solid angle at the receiver), so
// its contribution weight is 1/pdf and it stands for exactly one candidate. A non-positive pdf
// gives W = 0, which the caller's store-time guard turns into an empty reservoir.
GiReservoir giMakeReservoir(float3 position, float3 normal, float3 radiance, float samplePdf) {
    GiReservoir r;
    r.position = position;
    r.normal   = normal;
    r.radiance = radiance;
    r.W        = samplePdf > 0.0 ? 1.0 / samplePdf : 0.0;
    r.M        = 1u;
    r.age      = 0u;
    return r;
}

// ---- octahedral unit-vector encoding (Cigolle et al. 2014), snorm16 per axis ----
float2 giOctWrap(float2 v) {
    const float2 signNotZero = float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
    return (1.0 - abs(v.yx)) * signNotZero;
}

uint giOctEncode(float3 n) {
    const float l1 = abs(n.x) + abs(n.y) + abs(n.z);
    float2 p = l1 > 0.0 ? n.xy / l1 : float2(0.0, 0.0);
    if (n.z < 0.0) p = giOctWrap(p);
    const int2 q = int2(round(clamp(p, -1.0, 1.0) * 32767.0));
    return (uint(q.x) & 0xFFFFu) | ((uint(q.y) & 0xFFFFu) << 16);
}

float3 giOctDecode(uint packed) {
    // Sign-extend each 16-bit half through an arithmetic right shift.
    const int2 q = int2(int(packed << 16) >> 16, int(packed) >> 16);
    const float2 p = max(float2(q) / 32767.0, -1.0);
    float3 n = float3(p, 1.0 - abs(p.x) - abs(p.y));
    if (n.z < 0.0) n.xy = giOctWrap(n.xy);
    return normalize(n);
}

// ---- packing ----
GiPackedReservoir giPackReservoir(GiReservoir r) {
    const float3 rad = min(max(r.radiance, 0.0), GI_RESERVOIR_MAX_HALF);
    GiPackedReservoir p;
    p.a = uint4(asuint(r.position), giOctEncode(r.normal));
    p.b.x = f32tof16(rad.r) | (f32tof16(rad.g) << 16);
    p.b.y = f32tof16(rad.b) | (min(r.M, GI_RESERVOIR_MAX_M) << 16) | (min(r.age, GI_RESERVOIR_MAX_AGE) << 24);
    p.b.z = asuint(r.W);
    p.b.w = 0u;
    return p;
}

GiReservoir giUnpackReservoir(GiPackedReservoir p) {
    GiReservoir r;
    r.position = asfloat(p.a.xyz);
    r.normal   = giOctDecode(p.a.w);
    r.radiance = float3(f16tof32(p.b.x & 0xFFFFu), f16tof32(p.b.x >> 16), f16tof32(p.b.y & 0xFFFFu));
    r.M        = (p.b.y >> 16) & 0xFFu;
    r.age      = p.b.y >> 24;
    r.W        = asfloat(p.b.z);
    return r;
}

// ---- PCG random stream (Jarzynski & Olano 2020) ----
uint giPcgHash(uint v) {
    const uint state = v * 747796405u + 2891336453u;
    const uint word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

struct GiRng {
    uint state;
};

// One independent stream per (pixel, frame, salt): each input is folded through the hash in turn
// so neighbouring pixels and consecutive frames decorrelate. `salt` separates streams that share a
// pixel and frame.
GiRng giInitRng(uint2 pixel, uint frame, uint salt) {
    GiRng g;
    g.state = giPcgHash(pixel.x ^ giPcgHash(pixel.y ^ giPcgHash(frame ^ giPcgHash(salt))));
    return g;
}

// Uniform in [0, 1): advance the LCG state, then permute it through the same output function.
float giRandom(inout GiRng g) {
    g.state = g.state * 747796405u + 2891336453u;
    const uint word = ((g.state >> ((g.state >> 28u) + 4u)) ^ g.state) * 277803737u;
    return float(((word >> 22u) ^ word) >> 8) * (1.0 / 16777216.0);
}

// A uniformly distributed offset inside a disc of `radius` pixels (polar sampling, area-uniform
// through the square root). Used for spatial taps: fresh per pixel and frame, so no neighbourhood
// pattern repeats and no offset table needs storing.
float2 giDiscOffset(inout GiRng g, float radius) {
    const float r   = sqrt(giRandom(g)) * radius;
    const float phi = giRandom(g) * 6.2831853;
    return float2(cos(phi), sin(phi)) * r;
}

// ---- streaming RIS (Talbot 2005; Bitterli 2020 weighted reservoir sampling) ----
// Adds one candidate of resampling weight `w` to a running sum and says whether it replaces the
// current selection: with probability w / (sum including w). The caller keeps the selected
// reservoir itself; this only decides.
bool giStreamAccept(inout float weightSum, float w, float u) {
    if (!(w > 0.0)) return false;   // also rejects NaN
    weightSum += w;
    return u * weightSum < w;
}

// ---- the reconnection Jacobian (Ouyang 2021, GRIS shift by position reuse) ----
// A sample at `samplePos` (normal `sampleNormal`) found from receiver `fromReceiver` is reused at
// `toReceiver`. Its contribution weight is a reciprocal density in solid angle at the receiver that
// found it; re-expressed in solid angle at the new receiver it scales by
//     |J| = (cos at new / cos at old) * (dist_old^2 / dist_new^2),
// the cosines taken at the sample between its normal and the direction back to each receiver.
// Returns 0 when either geometry degenerates (receiver on the sample, or behind it); the caller
// rejects non-positive and non-finite values.
float giReconnectionJacobian(float3 toReceiver, float3 fromReceiver, float3 samplePos, float3 sampleNormal) {
    const float3 dNew  = toReceiver - samplePos;
    const float3 dOld  = fromReceiver - samplePos;
    const float  d2New = dot(dNew, dNew);
    const float  d2Old = dot(dOld, dOld);
    if (d2New <= 1e-8 || d2Old <= 1e-8) return 0.0;
    const float cosNew = dot(sampleNormal, dNew) * rsqrt(d2New);
    const float cosOld = dot(sampleNormal, dOld) * rsqrt(d2Old);
    if (cosNew <= 0.0 || cosOld <= 0.0) return 0.0;
    return (cosNew / cosOld) * (d2Old / d2New);
}

// ---- the finalised contribution weight (Bitterli 2020, MIS normalisation over the streams) ----
// After every stream has been offered, the selected sample y gets
//     W = weightSum * pSelected / (pHere * pSum),
// where pHere is the target function at the receiver doing the resampling, pSelected is it at the
// receiver of the stream y was taken from (pHere itself when y is this frame's own candidate), and
// pSum is the sum over every stream i of M_i * target_i(y). With one stream of M = 1 this reduces
// to the candidate's own 1/pdf, so a pixel that reuses nothing keeps exactly its fresh estimate.
// Any non-positive or non-finite factor gives 0, never a division by zero.
float giFinalizeWeight(float weightSum, float pSelected, float pHere, float pSum) {
    const float denom = pHere * pSum;
    if (!(denom > 0.0) || !(pSelected > 0.0)) return 0.0;
    return weightSum * pSelected / denom;
}

#endif // VOXI_RESERVOIR_HLSLI
