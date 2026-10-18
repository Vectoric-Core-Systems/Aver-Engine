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

// MANY LIGHTS (docs/plans/wave4/MANY_LIGHTS.md): the compiles that have the full direct-light evaluator and so the
// light-tree estimator (the lite arm of averDirectLights, glass and the single pass never do).
#if AVER_RD_LAMPS && defined(AVER_RT_BINDLESS) && !AVER_BLENDED_PASS && !AVER_RD_SINGLE_PASS
#define AVER_MANY_LIGHTS 1
#else
#define AVER_MANY_LIGHTS 0
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

// A scattering volume (scatterColor > 0) whose thickness ray finds nothing is open water, as deep as it
// gets, not empty: 40 attenuation distances leave nothing behind it and its in-scatter reaches its deep
// value. Any other volume keeps the measured thickness. Callers feed this to the transmittance and the
// in-scatter, not to the refraction offset.
float averVolumeThicknessOrDeep(float t, float attD, float3 scatterColor) {
    return (t <= 0.0 && any(scatterColor > 0.0)) ? 40.0 * attD : t;
}

// The light a scattering volume adds along the view path through it (averVolumeInscatter, material_prelude),
// for a front-face crossing of the surface `s`. Sun: its radiance through the visibility, less the share the
// surface reflects at the sun's angle (the caller's Fresnel, so the volume is lit by what enters it). Sky:
// the ambient the surface is shaded with. Zero, and skipped, without a volume or a scatter colour.
float3 averSurfaceInscatter(AverSurface s, float thicknessCm, float3 viewTravelDir, AverLight sun,
                            AverIndirect ind) {
    if (s.attenuationDistance <= 0.0 || !any(s.scatterColor > 0.0)) return float3(0.0, 0.0, 0.0);
    const float3 sunIn = sun.radiance * sun.visibility *
                         (1.0 - fresnelSchlick(saturate(dot(s.N, sun.direction)), s.F0, s.f90));
    // Leaving the water the glow loses what the surface reflects back at the view angle (alpha is the view
    // Fresnel for a fully transmissive surface); without it grazing water adds sky AND full glow.
    return averVolumeInscatter(s.attenuationColor, s.attenuationDistance, s.scatterColor, s.scatterAnisotropy,
                               thicknessCm, viewTravelDir, s.N, sun.direction, sunIn,
                               ind.ambient * ind.ambientScale * ind.occlusion) * (1.0 - saturate(s.alpha));
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
    bool        behind;   // the ray met a scattering material's back face; false for every other material
};

// rtHitFrom flips h.N to face the ray; this recovers which side the ray came from.
bool rtHitFromBehind(RtHit h, float3 dir) {
    const float3 nObj = rtVertex(h.inst, h.i0).nrm * h.w.x + rtVertex(h.inst, h.i1).nrm * h.w.y + rtVertex(h.inst, h.i2).nrm * h.w.z;
    return dot(mul(float4(nObj, 0.0), h.inst.objectToWorld).xyz, dir) > 0.0;
}

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
    v.behind = any(h.mat.scatterColor > 0.0) && rtHitFromBehind(h, dir);
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

// ---- MANY LIGHTS (docs/plans/wave4/MANY_LIGHTS.md 3.3): the stochastic tail estimator ----
// The compile that has the full evaluator (the lite arm of averDirectLights below and the single pass never reach it).
// Per shading point: M candidates drawn from the light tree by importance (voxi_lighttree.hlsli), resampled by a
// BRDF-aware target into S slots (RIS), each slot traced with one shadow ray. Nothing is carried from another pixel or
// another frame: every candidate comes from this point's own descent over this frame's light table (patent rule,
// MANY_LIGHTS.md section 2). The caller's exact light e0 (or none) is excluded and shaded by its own path.
#if AVER_MANY_LIGHTS

// Cranley-Patterson rotation per 4x4 tile (interleaved gradient noise), so neighbouring tiles draw different candidates.
float rdTreeRotation(float2 pixel) {
    const float2 tile = floor(pixel * 0.25);
    return frac(52.9829189 * frac(dot(tile, float2(0.06711056, 0.00583715))));
}

// The selection density: the light's cheap irradiance luminance times a cheap BRDF at its centre (diffuse plus a GGX
// lobe widened by the emitter size as ptLampLight does). Only a density: any positive function is unbiased, so the
// surface terms come from the factor-level inputs and need no texture fetch.
float rdTreeTarget(RdLocalLight l, AverSurface s, float3 pos, float kdLum, float f0Lum) {
    const float e = averShadowLum(rdLightIrradianceCheap(l, pos, s.N));
    if (!(e > 0.0)) return 0.0;
    float3 Lc;
    float  widen = 0.0;
    if (aversLightKind(l) == AVER_LIGHT_DIRECTIONAL) {
        Lc = normalize(l.axisKind.xyz);
    } else {
        const float3 toC = l.posRadius.xyz - pos;
        const float  invD = rsqrt(max(dot(toC, toC), 1e-8));
        Lc = toC * invD;
        widen = l.posRadius.w * 0.5 * invD;
    }
    const float3 Hv = s.V + Lc;
    const float NoL = saturate(dot(s.N, Lc));
    const float NoV = max(s.ndv, 0.05);
    const float NoH = saturate(dot(s.N, Hv) * rsqrt(max(dot(Hv, Hv), 1e-8)));
    const float rough = clamp(s.rough + widen, s.rough, 1.0);
    return e * (kdLum * (1.0 / PI) + f0Lum * distGGX(NoH, rough * rough) / (4.0 * NoV * max(NoL, 0.05)));
}

// Lights `pos` with the tree. e0 is the exact light the caller shades itself (0xFFFFFFFF = none); M candidates, S slots
// (1 to 4); `exhaustive` walks every tree light with one ray each instead (voxi.manyLights 3, the ground truth).
// `rayN` is the normal the shadow rays are offset along (the visible surface passes its geometric one, hits their shading one).
// Diffuse+subsurface and specular radiance accumulate apart (NRD2's D and S buckets); sumC / sumCV are the unshadowed
// and shadowed luminance, whose ratio is the tail fraction the filter and the glass replay read. The directional
// lights other than e0 are shaded exactly with one ray each. ONE shading call in the loop (compile time).
void averManyLights(AverSurface s, float3 pos, float3 rayN, float2 pixel, inout uint rng, uint e0, uint M, uint S,
                    bool exhaustive, bool allowMap, RdTreeHdr th,
                    inout float3 diffuse, inout float3 specular, inout float sumC, inout float sumCV) {
    if (!th.valid || s.model == AVER_MODEL_UNLIT) return;
    uint g0, g1, g2;
    const uint nG = rdTreeGlobals(g0, g1, g2);
    uint ex0 = 0xFFFFFFFFu, ex1 = 0xFFFFFFFFu, ex2 = 0xFFFFFFFFu;
    if (nG > 0u && g0 != e0) ex0 = g0;
    if (nG > 1u && g1 != e0) ex1 = g1;
    if (nG > 2u && g2 != e0) ex2 = g2;

    const float kdLum = averShadowLum(s.kdAlbedo);
    const float f0Lum = averShadowLum(s.F0);
    uint  sel0 = 0xFFFFFFFFu, sel1 = 0xFFFFFFFFu, sel2 = 0xFFFFFFFFu, sel3 = 0xFFFFFFFFu;
    float ph0 = 0.0, ph1 = 0.0, ph2 = 0.0, ph3 = 0.0;
    float wsum = 0.0;
    float tau = 1.0e-4;
    if (!exhaustive) {
        // A draw that lands on e0 is redrawn (three tries): the density of any other light is q * (1 + q0 + q0^2), the
        // mass q0^3 a candidate of weight 0, so the estimate stays unbiased without the spike a dominant e0 would cause.
        const bool e0Local = e0 != 0xFFFFFFFFu && aversLightKind(gRdLocalLights[e0]) != AVER_LIGHT_DIRECTIONAL;
        float mult = 1.0;
        if (e0Local) {
            const float q0 = rdTreePdfOf(th, e0, pos, s.N);
            mult = 1.0 + q0 + q0 * q0;
        }
        if (e0 != 0xFFFFFFFFu) tau = rdTreeTarget(gRdLocalLights[e0], s, pos, kdLum, f0Lum) * (1.0 / 256.0);
        const uint  tries = e0Local ? 3u : 1u;
        const float rot = rdTreeRotation(pixel);
        [loop] for (uint j = 0u; j < M; ++j) {
            uint  y = 0xFFFFFFFFu;
            float q = 0.0;
            [loop] for (uint t = 0u; t < tries; ++t) {
                uint yy;
                float qq;
                if (!rdTreeSample(th, pos, s.N, frac(ptRand(rng) + rot), yy, qq)) break;
                if (yy != e0) { y = yy; q = qq; break; }
            }
            if (y == 0xFFFFFFFFu) continue;
            const float ph = rdTreeTarget(gRdLocalLights[y], s, pos, kdLum, f0Lum);
            if (!(ph > 0.0)) continue;
            float w = ph / (q * mult);
            // voxi.lightWeightClamp: a candidate is capped at clamp times the running mean (0 = off), a bias knob.
            if (th.weightClamp > 0.0 && j > 0u) w = min(w, th.weightClamp * wsum / (float)j);
            wsum += w;
            // Each slot streams its own pick: accepted with probability w / wsum, no runtime-indexed array.
            if (ptRand(rng) * wsum < w) { sel0 = y; ph0 = ph; }
            if (S > 1u && ptRand(rng) * wsum < w) { sel1 = y; ph1 = ph; }
            if (S > 2u && ptRand(rng) * wsum < w) { sel2 = y; ph2 = ph; }
            if (S > 3u && ptRand(rng) * wsum < w) { sel3 = y; ph3 = ph; }
        }
    }

    const uint nMain = exhaustive ? rdTreeLightCount(th) : S;
    [loop] for (uint it = 0u; it < nMain + 3u; ++it) {
        uint  y = 0xFFFFFFFFu;
        float W = 1.0;
        if (it < nMain) {
            if (exhaustive) {
                y = it;
                if (!(averShadowLum(rdLightIrradianceCheap(gRdLocalLights[y], pos, s.N)) > 0.0)) continue;
            } else {
                y = it == 0u ? sel0 : it == 1u ? sel1 : it == 2u ? sel2 : sel3;
                const float ph = it == 0u ? ph0 : it == 1u ? ph1 : it == 2u ? ph2 : ph3;
                if (y == 0xFFFFFFFFu) continue;
                // Contribution weight of the slot: wsum / (M * ph). ph * W = wsum / M is the slot's unshadowed
                // contribution estimate, which russian roulette tests against tau.
                const float c = wsum / (float)M;
                W = c / ph;
                if (c < tau) {
                    const float p = c / tau;
                    if (ptRand(rng) >= p) continue;
                    W /= p;
                }
                W /= (float)S;
            }
        } else {
            const uint k = it - nMain;
            y = k == 0u ? ex0 : k == 1u ? ex1 : ex2;
            if (y == 0xFFFFFFFFu) continue;
        }
        const RdLocalLight ll = gRdLocalLights[y];
        AverLight   l;
        AverSurface sL;
        if (!ptLampLight(ll, s, pos, l, sL)) continue;
        if (dot(s.N, l.direction) <= 0.0 && s.sssWeight <= 0.0) continue;
        const float3 vis = averLightVisibility(ll, pos, rayN, pixel, rng, allowMap);   // rays leave along the caller's offset normal
        float3 dD, dS, dSss;
        float  ndl;
        averDirectTerms(sL, l, dD, dS, dSss, ndl);
        const float3 lt = l.radiance * (ndl * W) * vis;
        diffuse  += dD * lt + dSss * l.radiance * vis * W;
        specular += dS * lt;
        const float cu = averShadowLum(l.radiance) * ndl * W;
        sumC  += cu;
        sumCV += cu * saturate(averShadowLum(vis));
    }
}
#endif

float3 averDirectLights(AverSurface s, float3 pos, float2 pixel, inout uint rng, bool allowMap) {
#if AVER_RD_LAMPS && !(defined(AVER_RT_BINDLESS) && !AVER_BLENDED_PASS && !AVER_RD_SINGLE_PASS)
    // LITE, for the ray-traced raster, glass and single-pass shaders: at their register limit, they hung the
    // RX 7800 XT whenever their code grew (rect lights, decals, stratified picks, and this evaluator's full form in
    // the glass replay's reflections under FidelityFX, 2026-10-07). One cheap loop sums every light's diffuse
    // irradiance and finds the strongest; it alone is shaded exactly with its own ray, and the rest take that ray's
    // visibility as diffuse light. Still nothing random.
    const RdLightRange lr = rdLightsAt(pos);
    float3 eAll = float3(0.0, 0.0, 0.0);
    float  wBest = 0.0;
    uint   best = 0xFFFFFFFFu;
    [loop] for (uint k = 0u; k < lr.count; ++k) {
        const uint j = rdLightIndex(lr, k);
        const float3 e = rdLightIrradianceCheap(gRdLocalLights[j], pos, s.N);
        const float  w = averShadowLum(e);
        eAll += e;
        if (w > wBest) { wBest = w; best = j; }
    }
    if (best == 0xFFFFFFFFu) return float3(0.0, 0.0, 0.0);
    const RdLocalLight ll = gRdLocalLights[best];
    AverLight   l;
    AverSurface sL;
    float3 lit = float3(0.0, 0.0, 0.0);
    float  v = 1.0;
    if (ptLampLight(ll, s, pos, l, sL) && dot(s.N, l.direction) > 0.0) {
        l.visibility = averLightVisibility(ll, pos, s.N, pixel, rng, allowMap);
        v = saturate(averShadowLum(l.visibility));
        lit = averShadeDirect(float3(0.0, 0.0, 0.0), sL, l);
    }
    const float3 kd = s.model == AVER_MODEL_UNLIT ? float3(0.0, 0.0, 0.0) : s.kdAlbedo * (1.0 / PI);
    return lit + kd * max(eAll - rdLightIrradianceCheap(ll, pos, s.N), 0.0) * v;
#elif AVER_RD_LAMPS
    // MANY LIGHTS (MANY_LIGHTS.md 3.3, hits): the tree's estimator in place of the top-4 loop below, when the frame runs
    // it (rtManyLights), or every light with one ray (voxi.manyLights 3). Still one shading call, inside the loop.
    if (rtManyLights() || rtManyLightsExhaustive()) {
        const RdTreeHdr th = rdTreeHeader();
        if (th.valid) {
            float3 dif = float3(0.0, 0.0, 0.0), spc = float3(0.0, 0.0, 0.0);
            float  sc = 0.0, scv = 0.0;
            averManyLights(s, pos, s.N, pixel, rng, 0xFFFFFFFFu, max(th.hitCandidates, 1u),
                           clamp(min(rdLightRaysPerHit(), 4u), 1u, 4u), rtManyLightsExhaustive(), allowMap, th,
                           dif, spc, sc, scv);
            return dif + spc;
        }
    }
    // EVERY LIGHT THE SUN'S WAY, within a ray budget, nothing picked at random (UNIFIED_LIGHTS.md "Ray budget"):
    // the strongest lightRaysPerHit lights (at most 4) are shaded exactly with their own shadow ray; every other light
    // is summed as diffuse irradiance in ONE cheap loop and takes those rays' irradiance-weighted visibility. Hits are
    // indirect (their light is attenuated by the hit's albedo), so the rest's specular is left out. A light that is not
    // a plain sphere (rect, spot, IES, cookie) outside the traced set is shaded exactly instead.
    const RdLightRange lr = rdLightsAt(pos);
    RdTop4 top = rdTop4Init();
    float3 eShared = float3(0.0, 0.0, 0.0);   // simple shadowable lights: cheap diffuse irradiance
    float3 eFree   = float3(0.0, 0.0, 0.0);   // simple no-shadow lights
    bool   anyComplex = false;                  // a rect/spot/IES/cookie light reaches here: the exact walk below
    [loop] for (uint k = 0u; k < lr.count; ++k) {
        const uint j = rdLightIndex(lr, k);
        const RdLocalLight ll = gRdLocalLights[j];
        const float3 e = rdLightIrradianceCheap(ll, pos, s.N);
        const float  w = averShadowLum(e);
        if (!(w > 0.0)) continue;
        anyComplex = anyComplex || !rdLightIsSimple(ll);
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
    [loop] for (uint k = 0u; k < K + (anyComplex ? lr.count : 0u); ++k) {
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

// The light a scattering volume adds at a front-face path vertex, once per visit (not per branch the path takes
// next: the composite adds it whatever passes through). The sun's visibility is one opaque shadow ray, drawn
// only for such a volume, so no other material's random stream moves.
float3 ptVolumeInscatter(PtVertex v, float2 pixel, inout uint rng) {
    if (v.s.attenuationDistance <= 0.0 || !any(v.s.scatterColor > 0.0) || v.behind) return float3(0.0, 0.0, 0.0);
    const float3 dir = -v.s.V;
    const float  th  = averVolumeThicknessOrDeep(averVolumeThickness(v.pos, v.s.N, dir),
                                                 v.s.attenuationDistance, v.s.scatterColor);
    AverLight sun;
    sun.direction  = normalize(gLightDir.xyz);
    sun.radiance   = averSunRadiance();
    sun.visibility = rdSunLit() ? rtShadowOpaque(v.pos, v.s.N, sun.direction, pixel, ptRand(rng) * 6.2831853)
                                : float3(0.0, 0.0, 0.0);
    AverIndirect ind;
    ind.ambient        = averSkyIrradiance(v.s.N);
    ind.ambientScale   = gAmbient.r;
    ind.diffuse        = float3(0.0, 0.0, 0.0);
    ind.occlusion      = 1.0;
    ind.specular       = float3(0.0, 0.0, 0.0);
    ind.specularTraced = 1.0;
    return averSurfaceInscatter(v.s, th, dir, sun, ind);
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
                                        averVolumeThicknessOrDeep(averVolumeThickness(v.pos, v.s.N, dir),
                                                                  v.s.attenuationDistance, v.s.scatterColor))
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
        // A scattering volume's glow is added whole, outside the coverage (it is not the surface's light).
        here += ptVolumeInscatter(nv, pixel, rng);
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
