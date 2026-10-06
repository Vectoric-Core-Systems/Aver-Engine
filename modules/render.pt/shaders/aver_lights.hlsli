// Scene lights shared by Voxi's staged ray-driven lamps and the reference path tracer: point, spot and
// rectangular area lights, with IES profiles and cookies. One implementation so both agree.
// See docs/rendering/LIGHTS.md. Includers define, before this file, whichever of
//   AVER_RT_BINDLESS (Voxi: gRtTextures, gMaterialSampler) or AVER_PT_BINDLESS (PT: gPtTextures, gPtSamp)
// they have; with neither, IES and cookies read as 1.
// AVER_LIGHTS_SIMPLE 1 (Voxi's single-pass megakernel, which sits at the register limit) leaves out
// rectangles, IES and cookies: point and spot lights only, a rectangle contributes nothing.
#ifndef AVER_LIGHTS_HLSLI
#define AVER_LIGHTS_HLSLI

#ifndef AVER_LIGHTS_SIMPLE
#define AVER_LIGHTS_SIMPLE 0
#endif

#if AVER_LIGHTS_SIMPLE
#define AVER_LIGHT_HAS_TEX 0
#elif defined(AVER_RT_BINDLESS)
#define AVER_LIGHT_HAS_TEX 1
#define AVER_LIGHT_TEX(idx, uv) gRtTextures[NonUniformResourceIndex(idx)].SampleLevel(gMaterialSampler, uv, 0)
#elif defined(AVER_PT_BINDLESS)
#define AVER_LIGHT_HAS_TEX 1
#define AVER_LIGHT_TEX(idx, uv) gPtTextures[NonUniformResourceIndex(idx)].SampleLevel(gPtSamp, uv, 0)
#else
#define AVER_LIGHT_HAS_TEX 0
#endif

#define AVER_LIGHT_POINT 0u
#define AVER_LIGHT_SPOT  1u
#define AVER_LIGHT_RECT  2u

// Mirrors kIesTableV / kIesTableH (IesProfile.hpp).
#define AVER_IES_V 64.0
#define AVER_IES_H 32.0

// 80 bytes; mirrors voxi::VoxiRenderer::RdLocalLight and pt::PtLight field for field.
//   posRadius     xyz centre (cm); w emitter radius (point/spot), 0 for rect
//   radianceRange rgb: point/spot colour * irradiance at 1 m; rect colour * radiance. w range (cm)
//   axisKind      xyz emission axis; w kind + 8 * (1 when the light casts no shadow)
//   shape         spot: x cosInner, y cosOuter. rect: x half width, y half height (cm).
//                 z IES texture index, w cookie texture index (-1 = none)
//   right         xyz unit tangent (the rect's width axis, IES 0 degrees); w cookie tan(half angle)
struct AverLightRec {
    float4 posRadius;
    float4 radianceRange;
    float4 axisKind;
    float4 shape;
    float4 right;
};

uint  aversLightKind(AverLightRec l)     { return (uint)(l.axisKind.w + 0.5) & 7u; }
bool  aversLightNoShadow(AverLightRec l) { return (((uint)(l.axisKind.w + 0.5)) >> 3) != 0u; }

// Relative candela toward `d` (unit, light -> receiver) from the baked IES table; 1 without a profile.
float aversIesFactor(float idx, float3 axis, float3 right, float3 d) {
#if AVER_LIGHT_HAS_TEX
    if (idx < 0.0) return 1.0;
    const float3 up = cross(axis, right);
    const float v = acos(clamp(dot(d, axis), -1.0, 1.0)) * (1.0 / 3.14159265);
    const float h = atan2(dot(d, up), dot(d, right)) * (0.5 / 3.14159265);
    const float2 uv = float2(h + 0.5 / AVER_IES_H, lerp(0.5 / AVER_IES_V, 1.0 - 0.5 / AVER_IES_V, v));
    return max(AVER_LIGHT_TEX((uint)(idx + 0.5), uv).r, 0.0);
#else
    return 1.0;
#endif
}

// Cookie colour projected through the light's frustum (tanHalf at unit forward distance); 1 without one.
float3 aversCookieFactor(float idx, float tanHalf, float3 axis, float3 right, float3 d) {
#if AVER_LIGHT_HAS_TEX
    if (idx < 0.0) return float3(1.0, 1.0, 1.0);
    const float fw = dot(d, axis);
    if (fw <= 1e-3) return float3(0.0, 0.0, 0.0);
    const float3 up = cross(axis, right);
    const float2 st = float2(dot(d, right), dot(d, up)) / (fw * max(tanHalf, 1e-3));
    if (any(abs(st) > 1.0)) return float3(0.0, 0.0, 0.0);
    return AVER_LIGHT_TEX((uint)(idx + 0.5), float2(st.x, -st.y) * 0.5 + 0.5).rgb;
#else
    return float3(1.0, 1.0, 1.0);
#endif
}

#if !AVER_LIGHTS_SIMPLE
// Sutherland-Hodgman clip of a quad (points relative to the receiver) to the hemisphere above N.
// At most 5 vertices come back.
uint aversClipQuad(float3 q[4], float3 N, out float3 o[5]) {
    o[0] = o[1] = o[2] = o[3] = o[4] = float3(0.0, 0.0, 0.0);
    uint n = 0u;
    [unroll] for (uint i = 0u; i < 4u; ++i) {
        const float3 a = q[i];
        const float3 b = q[(i + 1u) & 3u];
        const float  da = dot(a, N), db = dot(b, N);
        if (da >= 0.0) { o[n] = a; ++n; }
        if ((da >= 0.0) != (db >= 0.0)) { o[n] = lerp(a, b, da / (da - db)); ++n; }
    }
    return n;
}

// Vector irradiance per unit radiance of a Lambertian polygon (Lambert/Arvo edge formula): its dot with N
// is the diffuse irradiance, its direction the flux-weighted direction to the light. Sign follows the winding.
// Works on the unnormalised vertices: a x b is taken as a x (b - a), which keeps its digits for a small
// distant panel (normalising first leaves a 1e-2 relative error in the sum at 50 m / 20 cm).
float3 aversPolygonVector(float3 v[5], uint n) {
    float3 sum = float3(0.0, 0.0, 0.0);
    [loop] for (uint i = 0u; i < n; ++i) {
        uint j = i + 1u;
        if (j == n) j = 0u;
        const float3 a  = v[i];
        const float3 b  = v[j];
        const float3 cr = cross(a, b - a);
        const float  s2 = dot(cr, cr);
        // atan2 of the edge's sine and cosine (both scaled by |a||b|): acos loses every digit when small.
        if (s2 > 1e-14 * dot(a, a) * dot(b, b)) sum += cr * (atan2(sqrt(s2), dot(a, b)) * rsqrt(s2));
    }
    return 0.5 * sum;
}
#endif

// One light at p for a surface with normal N. `radiance` is the irradiance at normal incidence (the
// caller multiplies by N.dir, as for the sun); `dir` is the unit direction to the light's flux centroid;
// `srcRadius` the emitter's size for widening a specular lobe. False past range or off the lit side.
bool aversLightEval(AverLightRec ll, float3 p, float3 N, out float3 dir, out float3 radiance,
                    out float srcRadius) {
    dir = float3(0.0, 0.0, 1.0);
    radiance = float3(0.0, 0.0, 0.0);
    srcRadius = 0.0;

    const float3 toC   = ll.posRadius.xyz - p;
    const float  d2    = dot(toC, toC);
    const float  range = ll.radianceRange.w;
    if (d2 >= range * range) return false;
    const float  x2   = d2 / (range * range);
    float        win  = saturate(1.0 - x2 * x2);
    win *= win;
    const float  invD = rsqrt(max(d2, 1e-8));
    const float3 toL  = toC * invD;
    const float3 fromL = -toL;
    const float3 axis = ll.axisKind.xyz;
    const uint   kind = aversLightKind(ll);

    float3 ang = float3(1.0, 1.0, 1.0);
    if (kind == AVER_LIGHT_SPOT) {
        const float c = dot(fromL, axis);
        if (c <= ll.shape.y) return false;
        const float t = saturate((c - ll.shape.y) / max(ll.shape.x - ll.shape.y, 1e-3));
        ang *= t * t * (3.0 - 2.0 * t);
    }
    if (ll.shape.z >= 0.0) ang *= aversIesFactor(ll.shape.z, axis, ll.right.xyz, fromL);
    if (ll.shape.w >= 0.0) ang *= aversCookieFactor(ll.shape.w, ll.right.w, axis, ll.right.xyz, fromL);

    if (kind != AVER_LIGHT_RECT) {
        const float r = ll.posRadius.w;
        dir = toL;
        radiance = ll.radianceRange.rgb * (1e4 / max(d2, r * r)) * win * ang;
        srcRadius = r;
        return true;
    }

#if AVER_LIGHTS_SIMPLE
    return false;
#else
    // Rect: one-sided Lambertian panel. The receiver must be in front of its plane.
    if (dot(-toC, axis) <= 0.0) return false;
    const float3 right = ll.right.xyz;
    const float3 up    = cross(axis, right);
    const float3 ex = right * ll.shape.x, ey = up * ll.shape.y;
    float3 q[4];
    q[0] = toC + ex + ey;
    q[1] = toC - ex + ey;
    q[2] = toC - ex - ey;
    q[3] = toC + ex - ey;
    float3 poly[5];
    const uint n = aversClipQuad(q, N, poly);
    if (n < 3u) return false;
    float3 vec = aversPolygonVector(poly, n);
    // Light sits behind the panel's front plane, so the vector toward it has a negative axis component.
    if (dot(vec, axis) > 0.0) vec = -vec;
    const float len = length(vec);
    if (!(len > 1e-7)) return false;
    dir = vec / len;
    radiance = ll.radianceRange.rgb * (len * win) * ang;
    srcRadius = 1.13 * sqrt(ll.shape.x * ll.shape.y);
    return true;
#endif
}

// Diffuse irradiance (N.L included) from one light; zero when it does not reach p.
float3 aversLightIrradiance(AverLightRec ll, float3 p, float3 N) {
    float3 dir, rad;
    float  r;
    if (!aversLightEval(ll, p, N, dir, rad, r)) return float3(0.0, 0.0, 0.0);
    return rad * saturate(dot(N, dir));
}

// A point on the rectangle for a shadow ray, u in [0,1)^2.
float3 aversRectSamplePoint(AverLightRec ll, float2 u) {
    const float3 axis  = ll.axisKind.xyz;
    const float3 right = ll.right.xyz;
    const float3 up    = cross(axis, right);
    return ll.posRadius.xyz + right * (ll.shape.x * (2.0 * u.x - 1.0)) + up * (ll.shape.y * (2.0 * u.y - 1.0));
}

// The shadow-ray target for receiver p with normal N: a rectangle sample the receiver can see over its
// horizon (one below it contributes no light, so is mirrored through the centre, else replaced by it).
float3 aversRectShadowTarget(AverLightRec ll, float2 u, float3 p, float3 N) {
    float3 t = aversRectSamplePoint(ll, u);
    if (dot(t - p, N) <= 0.0) t = 2.0 * ll.posRadius.xyz - t;
    if (dot(t - p, N) <= 0.0) t = ll.posRadius.xyz;
    return t;
}

#endif
