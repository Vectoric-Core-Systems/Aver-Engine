// voxi_neurac.hlsli: radiance cache constants, layouts, addressing, SH basis, and packing.
// Includes NO resources; see voxi_neurac_io.hlsli for scatter/lookup over t22/u20/u21.

#ifndef AVER_NEURAC_PURE_HLSLI
#define AVER_NEURAC_PURE_HLSLI

#define AVER_RC_CASCADES        3
// A lookup whose contributing corners hold less trilinear weight than this fades out (rcLookupCascade).
#define AVER_RC_MIN_COVER       0.25
#define AVER_RC_RES             64
#define AVER_RC_CELLS_PER_CASC  262144      // 64^3
#define AVER_RC_CELLS           786432      // 3 * 64^3
#define AVER_RC_PAYLOAD_BASE    1572864     // 2 * AVER_RC_CELLS: counts and arrivals come first
#define AVER_RC_SQ_BASE         13369344    // 17 * AVER_RC_CELLS: one sum-of-squares int per cell, after the payload

// Fixed-point scales: SH terms * 2^15, summed normals * 2^16.
#define AVER_RC_SH_SCALE        32768.0
#define AVER_RC_N_SCALE         65536.0
#define AVER_RC_SQ_SCALE        2048.0      // sum of squares of the per-sample scalar (<= 80), 2^11
#define AVER_RC_DC_TO_E         0.0795774715 // 1 / (4 PI) = 0.282095^2: DC coefficient -> irradiance / PI
#define AVER_RC_MAX_CAP         64          // per-cell per-frame sample cap
#define AVER_RC_LMAX            32.0        // radiance clamped before projection
#define AVER_RC_MIN_COS         0.1         // pdf floor: weight = PI / max(cos, MIN_COS)
#define AVER_RC_NEFF_MAX        15          // n_eff and age are 4 bits each
#define AVER_RC_AGE_MAX         15
#define AVER_RC_NORMAL_K        2.0         // corner weight = saturate(dot(cellNormal, N) * K)
#define AVER_RC_MARGIN_CELLS    1           // a cascade is skipped if a corner is within this of its edge
#define AVER_RC_CROSSFADE_CELLS 4           // edge fade width in cells
#define AVER_RC_MIN_ACCEPT_RATE 0.03125     // 1/32: floor on the load-adaptive accept rate
#define AVER_RC_TRUST_MAX       63          // 6-bit trust; 63 = 1.0
#define AVER_RC_LEN_MAX         63          // 6-bit planarity

// ---- RESOURCE ELEMENT LAYOUTS (shared with resolve shader and C++ side) ----
// RcInfo, stride 80, ONE element, CPU-written Upload ring.
struct RcInfo { uint4 hdr0; float4 hdr1; int4 cas[3]; };
// hdr1 = (alphaMin, relaxAlpha, asfloat(coarseCaps = cap1 | cap2 << 8), netStrength). Cascade 0's cap is hdr0.w.
// RcCell, stride 32, cascade-major: a.xyzw + b.xy = 12 SH values as fp16 pairs,
// b.z = normal word (octU 10 | octV 10 | len 6 | trust 6), b.w = meta (tag bits 0-23 | n_eff bits 24-27 | age bits 28-31).
// Valid iff n_eff > 0 && tag == rcTagPack(expected world cell) && age < AVER_RC_AGE_MAX.
struct RcCell { uint4 a; uint4 b; };

// Accumulator (ints): counts [0, K) = accepted | prevArrivals << 16; arrivals [K, 2K); payload at
// AVER_RC_PAYLOAD_BASE + cell*15 + k, K = AVER_RC_CELLS. k = 0..11 SH sums, k = 12..14 summed surface normal;
// then K ints at AVER_RC_SQ_BASE + cell: the sum over accepted samples of (lum(L) * weight / 4 PI)^2, scale SQ_SCALE.
#define AVER_RC_PAYLOAD_INTS    15

// 3 x 8 bits of (worldCell >> 6). Toroidal address (worldCell & 63) cannot distinguish cells 64 apart; tag can.
uint rcTagPack(int3 worldCell) {
    const uint3 t = (uint3)((worldCell >> 6) & 255);
    return t.x | (t.y << 8) | (t.z << 16);
}

// Cascade-major toroidal index.
uint rcCellIndex(uint cascade, int3 worldCell) {
    const uint3 t = (uint3)(worldCell & 63);
    return cascade * AVER_RC_CELLS_PER_CASC + ((t.z * 64u) + t.y) * 64u + t.x;
}

// Per-cell per-frame sample cap of a cascade: the finest from hdr0.w, the coarse two packed in hdr1.z.
uint rcCapFor(RcInfo info, uint cascade) {
    const uint caps = asuint(info.hdr1.z);
    const uint cap  = cascade == 0u ? info.hdr0.w : (cascade == 1u ? (caps & 255u) : ((caps >> 8) & 255u));
    return min(cap, (uint)AVER_RC_MAX_CAP);
}

// Real L1 SH basis, order [Y0, Y1(y), Y1(z), Y1(x)].
void rcShBasis(float3 dir, out float y[4]) {
    y[0] = 0.282095;
    y[1] = 0.488603 * dir.y;
    y[2] = 0.488603 * dir.z;
    y[3] = 0.488603 * dir.x;
}

// Cosine-convolved irradiance / PI (Ramamoorthi-Hanrahan). Constant L over the hemisphere returns L.
float3 rcShIrradianceOverPi(float3 c[4], float3 N) {
    const float3 e = 0.282095 * c[0]
                   + (2.0 / 3.0) * 0.488603 * (N.y * c[1] + N.z * c[2] + N.x * c[3]);
    return max(e, 0.0);
}

// Octahedral encode/decode, 10 bits per axis. Normal word: octU 10 | octV 10 | len 6 | trust 6.
// len = planarity (1 = flat); trust 63 = 1.0 (stage 2 lowers it).
float2 rcOctEncode(float3 n) {
    float2 p = n.xy / max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6);
    if (n.z < 0.0) p = (1.0 - abs(p.yx)) * float2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    return p;
}
float3 rcOctDecode(float2 p) {
    float3 n = float3(p.x, p.y, 1.0 - abs(p.x) - abs(p.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}
uint rcPackNormal(float3 meanVec, uint trust6) {
    const float len = length(meanVec);
    const float3 dir = len > 1e-6 ? meanVec / len : float3(0.0, 0.0, 1.0);
    const float2 o = saturate(rcOctEncode(dir) * 0.5 + 0.5);
    const uint u = (uint)(o.x * 1023.0 + 0.5);
    const uint v = (uint)(o.y * 1023.0 + 0.5);
    const uint l = (uint)(saturate(len) * (float)AVER_RC_LEN_MAX + 0.5);
    return u | (v << 10) | (l << 20) | (min(trust6, (uint)AVER_RC_TRUST_MAX) << 26);
}
uint rcNormalTrust6(uint w) { return w >> 26; }
float3 rcUnpackNormal(uint w, out float len, out float trust) {
    const float2 o = float2((float)(w & 1023u), (float)((w >> 10) & 1023u)) * (1.0 / 1023.0);
    len   = (float)((w >> 20) & 63u) * (1.0 / (float)AVER_RC_LEN_MAX);
    trust = (float)(w >> 26) * (1.0 / (float)AVER_RC_TRUST_MAX);
    return rcOctDecode(o * 2.0 - 1.0);
}
float3 rcUnpackNormal(uint w, out float len) {
    float trust;
    return rcUnpackNormal(w, len, trust);
}

// 12 SH values (order [c0.rgb, cY.rgb, cZ.rgb, cX.rgb]) as 6 words of two fp16.
void rcPackSh(float3 c[4], out uint w[6]) {
    const float v[12] = { c[0].x, c[0].y, c[0].z, c[1].x, c[1].y, c[1].z,
                          c[2].x, c[2].y, c[2].z, c[3].x, c[3].y, c[3].z };
    [unroll] for (int i = 0; i < 6; ++i) w[i] = f32tof16(v[2 * i]) | (f32tof16(v[2 * i + 1]) << 16);
}
void rcUnpackSh(uint4 a, uint2 b, out float3 c[4]) {
    const uint w[6] = { a.x, a.y, a.z, a.w, b.x, b.y };
    float v[12];
    [unroll] for (int i = 0; i < 6; ++i) {
        v[2 * i]     = f16tof32(w[i] & 0xFFFFu);
        v[2 * i + 1] = f16tof32(w[i] >> 16);
    }
    c[0] = float3(v[0], v[1], v[2]);   c[1] = float3(v[3], v[4], v[5]);
    c[2] = float3(v[6], v[7], v[8]);   c[3] = float3(v[9], v[10], v[11]);
}

#endif  // AVER_NEURAC_PURE_HLSLI
