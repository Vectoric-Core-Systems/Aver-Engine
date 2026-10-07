// ---- PATH VERTICES: light sampling and path continuation for every ray hit ----
// Included from voxi_rt.hlsli after everything it calls. The helpers here (random numbers, one lamp by
// next-event estimation, BSDF sampling) serve any ray path that wants them; reflections light their hit
// with ptLamp in every mode.
//
// Path Tracing (Settings::pathTracing above Off) uses the rest: ReSTIR GI's candidate and the
// reflection ray continue as full paths. Every vertex is the material's own surface (rtHitSurface),
// lit by the sun and one lamp; the path continues through the BSDF and ends on a sky miss, at
// Settings::ptBounces, or by Russian roulette. Those call sites exist only in the AVER_PT_PATHS twins
// (VoxiRenderer::createPathTraceTwins), so no other compile pays for them.
#ifndef AVER_PT_PATHS
#define AVER_PT_PATHS 0
#endif

// gPtBounceParams.x: path vertices after the primary hit (Settings::ptBounces, [1,8]) in the low four
// bits; bit 4 is Reference mode (Settings::ptMode 1).
uint ptBounceCount() { return clamp((uint)gPtBounceParams.x & 15u, 1u, 8u); }
bool ptReferenceMode() { return ((uint)gPtBounceParams.x & 16u) != 0u; }

// PCG hash (Jarzynski & Olano 2020): per pixel, per frame, per stream.
uint ptPcg(uint v) {
    const uint s = v * 747796405u + 2891336453u;
    const uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}
uint ptSeed(float2 pixel, uint stream) {
    return ptPcg((uint)pixel.x ^ ptPcg((uint)pixel.y ^ ptPcg((uint)gRtHistParams.z * 9781u + stream)));
}
float ptRand(inout uint st) { st = ptPcg(st); return (float)(st >> 8) * (1.0 / 16777216.0); }

void ptBasis(float3 N, out float3 T, out float3 B) {
    const float3 up = abs(N.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0);
    T = normalize(cross(up, N));
    B = cross(N, T);
}

// Ray offset off a surface, growing with distance from the camera like every other secondary ray here.
float ptBias(float3 p) { return max(gRtParams.z, 1e-4) * (1.0 + length(p - gCamPos.xyz) * 5e-4); }

// Distance through the medium a surface bounds, along `viewDir`: to the next surface of any kind
// (panes are real boundaries). 0 when nothing is found. Read by every translucent composite.
float averVolumeThickness(float3 wpos, float3 N, float3 viewDir) {
    RayDesc r;
    // Bias same as reflection ray, pushed along view direction (ray heads into the surface).
    const float bias = max(gRtParams.z, 1e-4) * (1.0 + length(wpos - gCamPos.xyz) * 5e-4);
    r.Origin    = wpos + viewDir * bias;
    r.Direction = viewDir;
    r.TMin      = 0.0;
    r.TMax      = 100000.0;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gScene, RAY_FLAG_NONE, AVER_RT_MASK_OPAQUE_ALL | AVER_RT_MASK_TRANSLUCENT, r);
    averRtProceedSolid(q);
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return 0.0;
    return q.CommittedRayT() + bias;
}
// Re-aims a surface's half vector and Fresnel at one light (they are built for whichever light came first).
void ptAim(inout AverSurface s, float3 L) {
    const float3 VL = s.V + L;
    const float  vl2 = dot(VL, VL);
    s.H = vl2 > 1e-12 ? VL * rsqrt(vl2) : s.N;
    s.F = fresnelSchlick(saturate(dot(s.H, s.V)), s.F0, s.f90);
}

// One path vertex: where a ray landed, as its material's surface. `cover` is how much of the light it
// stops: its alpha for a blended material, 1 for everything else (a masked cut-out that was hit is solid).
struct PtVertex {
    float3      pos;
    AverSurface s;
    float       cover;
};

// Traces origin->dir against opaque geometry and builds the hit's surface through rtHitSurface.
// `cone` is the ray's footprint growth per cm (texture mip selection). False on a miss.
bool ptTrace(float3 origin, float3 dir, float tmin, float cone, out PtVertex v,
             uint mask = AVER_RT_MASK_OPAQUE_ALL) {
    v = (PtVertex)0;
    RayDesc r;
    r.Origin    = origin;
    r.Direction = dir;
    r.TMin      = tmin;
    r.TMax      = 1.0e7;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gScene, RAY_FLAG_NONE | gAverRtSecondaryRayFlags, mask, r);
    averRtProceedSolid(q);
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return false;

    const RtHit h = rtHitCommitted(q, origin, dir);
    float2 gx, gy;
    rtHitConeGrad(h, cone, gx, gy);
    v.pos   = h.pos;
    v.s     = rtHitSurface(h, -dir, normalize(gLightDir.xyz), gx, gy, AVER_RT_HIT_FULL);
    v.cover = (h.mat.flags & AVER_MAT_ALPHA_MASK) != 0u ? 1.0 : saturate(v.s.alpha);
#if AVER_RD_LAMPS
    // A listed lamp is reached by next-event estimation (ptLamp); its glow would count twice.
    if ((h.mat.flags & AVER_MAT_LIGHT) != 0u && rdLocalEmitterCarried(h.pos)) v.s.emissive = float3(0.0, 0.0, 0.0);
#endif
    return true;
}

#if AVER_RD_LAMPS
// A lamp as a light at `pos`, unshadowed: its emitter size widens the surface's roughness (rdLocalLightAt).
bool ptLampLight(RdLocalLight ll, AverSurface s, float3 pos, out AverLight l, out AverSurface sL) {
    sL = s;
    l.direction  = float3(0.0, 0.0, 1.0);
    l.radiance   = float3(0.0, 0.0, 0.0);
    l.visibility = float3(1.0, 1.0, 1.0);
    float r;
    if (!aversLightEval(ll, pos, s.N, l.direction, l.radiance, r)) return false;
    const float3 toC = ll.posRadius.xyz - pos;
    const float  invD = rsqrt(max(dot(toC, toC), 1e-8));
    sL.rough = clamp(s.rough + r * 0.5 * invD, s.rough, 1.0);
    ptAim(sL, l.direction);
    return true;
}
#endif

// ---- THE DIRECT-LIGHT EVALUATOR (docs/rendering/UNIFIED_LIGHTS.md) ----
// Every emitter is one entry of the light list (t18), the sun its directional entry; this is the one way they light
// a point off the visible surface (GI candidates, reflection hits, path vertices). See averDirectLights.

#if AVER_RD_LAMPS
float giHitShadowMapVisibility(float3 wpos, float3 N, float3 L);   // voxi_restir.hlsli

// A light's visibility from `pos`, tinted. Directional: the shadow map where the GI hit allows it (allowMap,
// Settings::rtGiHitShadowMap) and it can answer, else one opaque ray (rtSecondaryShadowOpaque) or a transmittance
// ray; caustic focus on top. Others: one ray to a point on the emitter (rdLocalShadow).
float3 averLightVisibility(RdLocalLight ll, float3 pos, float3 N, float2 pixel, inout uint rng, bool allowMap) {
    const float jitter = ptRand(rng) * 6.2831853;
    if (aversLightKind(ll) == AVER_LIGHT_DIRECTIONAL) {
        if (aversLightNoShadow(ll)) return 1.0;
        const float3 L = normalize(ll.axisKind.xyz);
        float3 v;
        const float mapVis = (allowMap && (rtGiShadowBits() & 8u) != 0u) ? giHitShadowMapVisibility(pos, N, L) : -1.0;
        if (mapVis >= 0.0)                        v = mapVis;
        else if ((rtGiShadowBits() & 1u) != 0u)   v = rtShadowOpaque(pos, N, L, pixel, jitter);
        else                                      v = rtShadow(pos, N, L, pixel, float3(0, 0, 0), float3(0, 0, 0), 1u, jitter);
        return v * (1.0 + averCausticFocus(pos));
    }
    float2 u2 = float2(0.5, 0.5);
    if (aversLightKind(ll) == AVER_LIGHT_RECT) u2 = float2(ptRand(rng), ptRand(rng));
    return rdLocalShadow(pos, N, ll, pixel, jitter, u2).xxx;
}

#endif

float3 averDirectLights(AverSurface s, float3 pos, float2 pixel, inout uint rng, bool allowMap) {
#if AVER_RD_LAMPS
    // EVERY LIGHT THE SUN'S WAY, within a ray budget, nothing picked at random (UNIFIED_LIGHTS.md "Ray budget"):
    // the strongest lightRaysPerHit lights (at most 4) are shaded exactly with their own shadow ray; every other light
    // is summed as diffuse irradiance in ONE cheap loop and takes those rays' irradiance-weighted visibility. Hits are
    // indirect (their light is attenuated by the hit's albedo), so the rest's specular is left out. A light that is not
    // a plain sphere (rect, spot, IES, cookie) outside the traced set is shaded exactly instead.
    const RdLightRange lr = rdLightsAt(pos);
    RdTop4 top = rdTop4Init();
    float3 eShared = float3(0.0, 0.0, 0.0);   // simple shadowable lights: cheap diffuse irradiance
    float3 eFree   = float3(0.0, 0.0, 0.0);   // simple no-shadow lights
    [loop] for (uint k = 0u; k < lr.count; ++k) {
        const uint j = rdLightIndex(lr, k);
        const RdLocalLight ll = gRdLocalLights[j];
        const float3 e = rdLightIrradianceCheap(ll, pos, s.N);
        const float  w = averShadowLum(e);
        if (!(w > 0.0)) continue;
        if (aversLightNoShadow(ll)) { if (rdLightIsSimple(ll)) eFree += e; continue; }
        rdTop4Add(top, w, j);
        if (rdLightIsSimple(ll)) eShared += e;
    }
    const uint  K = min(rdLightRaysPerHit(), 4u);
    float3 traced = float3(0.0, 0.0, 0.0);
    float3 exactShared = float3(0.0, 0.0, 0.0);   // non-simple lights outside the traced set, shaded exactly
    float  sumW = 0.0, sumV = 0.0;
    // Traced lights, then (k >= K) every non-simple light: one shading and one visibility call site in the loop, as
    // each visibility call inlines the whole shadow kernel.
    [loop] for (uint k = 0u; k < K + lr.count; ++k) {
        uint j;
        if (k < K) {
            j = k == 0u ? top.i0 : k == 1u ? top.i1 : k == 2u ? top.i2 : top.i3;
            if (j == 0xFFFFFFFFu) continue;
        } else {
            j = rdLightIndex(lr, k - K);
            const RdLocalLight lc = gRdLocalLights[j];
            if (rdLightIsSimple(lc) || j == top.i0 || (K > 1u && j == top.i1) || (K > 2u && j == top.i2) ||
                (K > 3u && j == top.i3)) continue;
        }
        const RdLocalLight ll = gRdLocalLights[j];
        AverLight   l;
        AverSurface sL;
        if (k < K && rdLightIsSimple(ll)) eShared -= rdLightIrradianceCheap(ll, pos, s.N);   // traced, not shared
        if (!ptLampLight(ll, s, pos, l, sL) || dot(s.N, l.direction) <= 0.0) continue;
        const float3 c = averShadeDirect(float3(0.0, 0.0, 0.0), sL, l);   // l.visibility is 1 here
        if (k >= K) { if (aversLightNoShadow(ll)) traced += c; else exactShared += c; continue; }
        const float3 v = averLightVisibility(ll, pos, s.N, pixel, rng, allowMap);
        const float  w = averShadowLum(rdLightIrradianceCheap(ll, pos, s.N));
        traced += c * v;
        sumW += w;
        sumV += w * saturate(averShadowLum(v));
    }
    const float  frac = sumW > 0.0 ? sumV / sumW : 1.0;
    const float3 kd   = s.model == AVER_MODEL_UNLIT ? float3(0.0, 0.0, 0.0) : s.kdAlbedo * (1.0 / PI);
    return traced + exactShared * frac + kd * (max(eShared, 0.0) * frac + eFree);
#else
    // No light list in this compile (single pass without lamps): nothing to light with.
    return float3(0.0, 0.0, 0.0);
#endif
}

// Direct light at a path vertex: every emitter through the evaluator.
float3 ptDirect(PtVertex v, float2 pixel, inout uint rng) {
    return averDirectLights(v.s, v.pos, pixel, rng, false);
}

// Smith G1 for GGX, alpha = rough^2.
float ptSmithG1(float ndx, float a2) { return 2.0 * ndx / (ndx + sqrt(a2 + (1.0 - a2) * ndx * ndx)); }

// Samples the next direction from the surface's BSDF. weight = f * cos / pdf for the lobe chosen.
// Specular: GGX VNDF, whose estimator is F * G1(L). Diffuse: cosine-weighted, whose estimator is kd.
// The lobe is picked by its share of reflected energy, and its weight divided by that probability.
// coverage < 1 (a translucent surface, reached on the branch that reflects with probability
// coverage): its specular is full strength and its diffuse coverage-weighted, so the specular
// estimate is divided by the branch probability and the diffuse one not. `diffuse` says which lobe.
bool ptSampleBsdf(AverSurface s, inout uint rng, out float3 dir, out float3 weight, float coverage,
                  out bool diffuse) {
    dir = s.N;
    weight = float3(0.0, 0.0, 0.0);
    diffuse = false;
    const float specLum = averShadowLum(fresnelSchlick(s.ndv, s.F0, s.f90));
    const float diffLum = averShadowLum(s.kdAlbedo);
    const float pSpec   = diffLum > 0.0 ? clamp(specLum / max(specLum + diffLum, 1e-4), 0.1, 0.9) : 1.0;
    float3 T, B;
    ptBasis(s.N, T, B);
    if (ptRand(rng) < pSpec) {
        const float  alpha = max(s.rough * s.rough, 1e-3);
        const float3 Ve = float3(dot(s.V, T), dot(s.V, B), max(dot(s.V, s.N), 1e-4));
        const float3 m  = rtSampleGgxVndf(Ve, alpha, float2(ptRand(rng), ptRand(rng)));
        const float3 H  = T * m.x + B * m.y + s.N * m.z;
        dir = reflect(-s.V, H);
        const float ndl = dot(s.N, dir);
        if (ndl <= 1e-4) return false;
        weight = fresnelSchlick(saturate(dot(s.V, H)), s.F0, s.f90) * ptSmithG1(ndl, alpha * alpha) /
                 (pSpec * max(coverage, 1e-3));
    } else {
        const float u1 = ptRand(rng), u2 = ptRand(rng);
        const float r = sqrt(u1), phi = 6.2831853 * u2;
        dir = normalize(T * (r * cos(phi)) + B * (r * sin(phi)) + s.N * sqrt(max(1.0 - u1, 0.0)));
        weight = s.kdAlbedo / (1.0 - pSpec);
        diffuse = true;
    }
    return any(weight > 0.0);
}

#if AVER_NEURAC
// The radiance cache (voxi_neurac_io.hlsli, included after this file in the NeuRaC twins).
bool   rcCacheOn();
float3 rcLookup(float3 p, float3 N, out float remaining);
#endif
// How much of a vertex's light the cache must cover before a path ends there (1 - its fallback share).
#define AVER_PT_RC_CONFIDENCE 0.5
// The cache holds diffuse light only, so a path may end in it only where the diffuse bounce dominates:
// rough, or more diffuse than specular. A glossy or metal vertex keeps tracing (its reflections live there).
bool ptCacheable(AverSurface s) {
    return s.rough >= 0.5 || averShadowLum(s.kdAlbedo) >= 2.0 * averShadowLum(s.F0);
}

// The first segment of a continued path: its direction, the radiance that came back along it (before
// the BSDF weight), and whether the diffuse lobe chose it, which makes it a cosine-distributed sample
// of incident light: what the radiance cache trains on.
struct PtFirst {
    float3 dir;
    float3 li;
    bool   diffuse;
};

// Radiance arriving at v along the rest of its path: up to `depth` more vertices sampled from the
// BSDF, each adding emission + direct light, a sky miss ending it. Russian roulette from the third.
// In the NeuRaC twins a vertex the cache covers ends the path with the cached diffuse bounce there
// (NVIDIA's NRC termination): the path is two segments long and the cache carries the rest.
float3 ptContinueEx(PtVertex v, float2 pixel, inout uint rng, uint depth, out PtFirst first) {
    first.dir = v.s.N;
    first.li = float3(0.0, 0.0, 0.0);
    first.diffuse = false;
    float3 sum = float3(0.0, 0.0, 0.0);
    float3 thr = float3(1.0, 1.0, 1.0);
    float3 rel = float3(1.0, 1.0, 1.0);   // thr without the first segment's weight (first.li)
    [loop] for (uint b = 0u; b < depth; ++b) {
        float3 dir, w;
        float3 origin;
        bool diffuse = false;
        const float bias = ptBias(v.pos);
        const float a    = v.cover;
        if (a < 0.999 && ptRand(rng) >= a) {
            // THROUGH a translucent surface, with probability (1 - coverage): the light the composite
            // passes, absorbed by the volume over its measured thickness.
            dir = -v.s.V;
            w = (v.s.attenuationDistance > 0.0)
              ? averVolumeTransmittance(v.s.attenuationColor, v.s.attenuationDistance,
                                        averVolumeThickness(v.pos, v.s.N, dir))
              : float3(1.0, 1.0, 1.0);
            origin = v.pos + dir * bias;
        } else {
            if (!ptSampleBsdf(v.s, rng, dir, w, a, diffuse)) break;
            origin = v.pos + v.s.N * bias;
        }
        thr *= w;
        if (b == 0u) { first.dir = dir; first.diffuse = diffuse; }
        else rel *= w;
        PtVertex nv;
        if (!ptTrace(origin, dir, bias, v.s.rough * v.s.rough + 0.1, nv,
                     AVER_RT_MASK_OPAQUE_ALL | AVER_RT_MASK_TRANSLUCENT)) {
            const float3 sky = averSkyRadianceCheap(dir) * gAmbient.r;
            sum += thr * sky;
            first.li += rel * sky;
            break;
        }
        // A translucent vertex reflects its direct light by its coverage (the composite's diffuse share).
        float3 here = (nv.s.emissive + ptDirect(nv, pixel, rng)) * nv.cover;
        bool cached = false;
#if AVER_NEURAC
        if (rcCacheOn() && ptCacheable(nv.s)) {
            float rem;
            const float3 e = rcLookup(nv.pos, nv.s.N, rem);
            if (1.0 - rem >= AVER_PT_RC_CONFIDENCE) {
                here += nv.cover * nv.s.kdAlbedo * e / (1.0 - rem);
                cached = true;
            }
        }
#endif
        sum += thr * here;
        first.li += rel * here;
        if (cached) break;
        if (b >= 1u) {
            const float p = clamp(max(thr.r, max(thr.g, thr.b)), 0.05, 0.95);
            if (ptRand(rng) > p) break;
            thr /= p;
            rel /= p;
        }
        v = nv;
    }
    return sum;
}

float3 ptContinue(PtVertex v, float2 pixel, inout uint rng, uint depth) {
    PtFirst first;
    return ptContinueEx(v, pixel, rng, depth, first);
}
// Outgoing radiance toward the ray from the first surface it hits, path traced: what a reflection
// sees in Path Tracing. A miss returns skyColor(dir), evaluated only then.
// The last ptRadiance call's first hit distance (kAverReflMissT on a miss): the reflection denoiser's
// parallax reprojection needs it, and the path itself does not return it.
static const float kAverReflMissT = 6.0e4;   // cm; fits RGBA16F, far enough to read as infinity
static float gAverPtFirstT = kAverReflMissT;
float3 ptRadiance(float3 origin, float3 dir, float tmin, float cone, float2 pixel, uint stream) {
    PtVertex v;
    gAverPtFirstT = kAverReflMissT;
    if (!ptTrace(origin, dir, tmin, cone, v)) return skyColor(dir);
    gAverPtFirstT = min(length(v.pos - origin), kAverReflMissT);
    uint rng = ptSeed(pixel, stream);
    const float3 here = v.s.emissive + ptDirect(v, pixel, rng);
#if AVER_NEURAC
    // The cache already holds the light arriving here (other paths trained it): its diffuse bounce
    // replaces the continuation's rays.
    if (rcCacheOn() && ptCacheable(v.s)) {
        float rem;
        const float3 e = rcLookup(v.pos, v.s.N, rem);
        if (1.0 - rem >= AVER_PT_RC_CONFIDENCE) return here + v.cover * v.s.kdAlbedo * e / (1.0 - rem);
    }
#endif
    return here + ptContinue(v, pixel, rng, ptBounceCount() - 1u);
}

// REFERENCE MODE (Settings::ptMode 1): one independent path from the primary surface, every lobe, fresh
// shadow rays, no reuse across pixels or frames. Stage B averages it while the view holds still.
float3 ptReferencePixel(AverSurface s, float3 wpos, float2 pixel) {
    uint rng = ptSeed(pixel, 0x7e1fu);
    PtVertex v;
    v.pos   = wpos;
    v.s     = s;
    v.cover = 1.0;   // the primary ray sees the opaque lane; glass is composited over it afterwards
    const float3 l = s.emissive + ptDirect(v, pixel, rng) + ptContinue(v, pixel, rng, ptBounceCount());
    return all(isfinite(l)) ? min(l, 64.0 * AVER_VOX_MAXRAD) : float3(0.0, 0.0, 0.0);
}
