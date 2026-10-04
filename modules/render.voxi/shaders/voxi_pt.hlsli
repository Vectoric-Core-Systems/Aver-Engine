// ---- PATH TRACING: multi-bounce paths inside the staged ray-driven frame ----
// Settings::pathTracing above Off. ReSTIR GI's candidate and the reflection ray each continue as a
// full path: every vertex is shaded from its own textures and lit by the sun and the lamps (one
// next-event ray each), the path continues through the surface's BSDF (GGX VNDF or cosine diffuse,
// chosen per vertex) and ends on a sky miss, at Settings::ptBounces, or by Russian roulette.
// ReSTIR then resamples these paths across pixels and frames, and the denoiser runs on the result.
//
// Compiled only into the AVER_PT_PATHS twins (VoxiRenderer::createPathTraceTwins), so no other
// compile pays its registers. Included from voxi_rt.hlsli, after everything it calls.
#ifndef AVER_PT_PATHS
#define AVER_PT_PATHS 0
#endif

#if AVER_PT_PATHS
// gPtBounceParams.x: path vertices after the primary hit (Settings::ptBounces, [1,8]).
uint ptBounceCount() { return (uint)clamp(gPtBounceParams.x, 1.0, 8.0); }

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

// Re-aims a surface's half vector and Fresnel at one light (they are built for whichever light came first).
void ptAim(inout AverSurface s, float3 L) {
    const float3 VL = s.V + L;
    const float  vl2 = dot(VL, VL);
    s.H = vl2 > 1e-12 ? VL * rsqrt(vl2) : s.N;
    s.F = fresnelSchlick(saturate(dot(s.H, s.V)), s.F0, s.f90);
}

// One path vertex: where a ray landed, shaded from that surface's own maps.
struct PtVertex {
    float3      pos;
    AverSurface s;
};

// Traces origin->dir against opaque geometry and builds the hit's surface. `cone` is the ray's
// footprint growth per cm (texture mip selection). False on a miss.
bool ptTrace(float3 origin, float3 dir, float tmin, float cone, out PtVertex v) {
    v = (PtVertex)0;
    RayDesc r;
    r.Origin    = origin;
    r.Direction = dir;
    r.TMin      = tmin;
    r.TMax      = 1.0e7;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gScene, RAY_FLAG_NONE | gAverRtSecondaryRayFlags, AVER_RT_MASK_OPAQUE_ALL, r);
    averRtProceedSolid(q);
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return false;

    const RtInstance inst = rtLoadInstance(rtPackCommitted(q));
    const uint tri = inst.firstIndex + q.CommittedPrimitiveIndex() * 3;
    const uint i0 = inst.firstVertex + gRtIndices[tri + 0];
    const uint i1 = inst.firstVertex + gRtIndices[tri + 1];
    const uint i2 = inst.firstVertex + gRtIndices[tri + 2];
    const float2 bary = q.CommittedTriangleBarycentrics();
    const float3 w = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    const float3 nObj = normalize(gRtVerts[i0].nrm * w.x + gRtVerts[i1].nrm * w.y + gRtVerts[i2].nrm * w.z);
    float3 N = normalize(mul(float4(nObj, 0.0), inst.objectToWorld).xyz);
    if (dot(N, dir) > 0.0) N = -N;

    v.pos = origin + dir * q.CommittedRayT();
    const RtMaterial mat = gRtMaterials[inst.materialIndex];

    float4 mapBase = float4(1, 1, 1, 1);
    float4 mapMR   = float4(1, 1, 1, 1);
    float4 mapEmis = float4(1, 1, 1, 1);
#ifdef AVER_RT_BINDLESS
    {
        const float2 meshUV = gRtVerts[i0].uv * w.x + gRtVerts[i1].uv * w.y + gRtVerts[i2].uv * w.z;
        const float2 uv     = averRtSurfaceUV(mat, inst, v.pos, N, meshUV);
        const float  rad    = max(cone, 1e-3) * q.CommittedRayT();
        float3 T, B;
        ptBasis(N, T, B);
        float2 gx, gy;
        averRtUvGrad(mat, inst, N,
                     gRtVerts[i0].pos, gRtVerts[i1].pos, gRtVerts[i2].pos,
                     gRtVerts[i0].uv,  gRtVerts[i1].uv,  gRtVerts[i2].uv,
                     T * rad, B * rad, gx, gy);
        mapBase = averRtSampleSlot(mat, 0, uv, gx, gy, float4(1, 1, 1, 1));
        mapMR   = averRtSampleSlot(mat, 1, uv, gx, gy, float4(1, 1, 1, 1));
        mapEmis = averRtSampleSlot(mat, 4, uv, gx, gy, float4(1, 1, 1, 1));
    }
#endif
    AverSurface s = (AverSurface)0;
    s.N           = N;
    s.V           = -dir;
    s.albedo      = inst.albedo * mat.baseColorFactor.rgb * mapBase.rgb;
    s.metallic    = saturate(inst.metallic * mat.metallicFactor * mapMR.b);
    s.rough       = clamp(inst.roughness * mat.roughnessFactor * mapMR.g, 0.045, 1.0);
    s.ndv         = saturate(dot(s.N, s.V));
    s.f90         = mat.f90;
    s.reflectance = mat.reflectance;
    s.sssColor    = float3(1.0, 1.0, 1.0);
#ifdef AVER_LAYERED_BSDF
    s.coatWeight = (mat.flags & AVER_MAT_COAT) ? saturate(mat.coatWeight)    : 0.0;
    s.coatRough  = (mat.flags & AVER_MAT_COAT) ? saturate(mat.coatRoughness) : 0.0;
    s.coatF0     = (mat.flags & AVER_MAT_COAT) ? saturate(mat.coatF0)        : 0.0;
#endif
    s.F0        = lerp(mat.reflectance.xxx, s.albedo, s.metallic);
    s.kdAlbedo  = (1.0 - s.metallic) * s.albedo * (1.0 - saturate(mat.transmission));
    s.model     = AVER_MODEL_STANDARD;
    s.alpha     = 1.0;
    s.occlusion = 1.0;
    s.emissive  = mat.emissiveFactor * mapEmis.rgb;
    // A lamp in the light list is reached by next-event estimation (ptLamp); its glow would count twice.
    if ((mat.flags & AVER_MAT_LIGHT) != 0u && rdLocalCarriesEmitters()) s.emissive = float3(0.0, 0.0, 0.0);
    ptAim(s, normalize(gLightDir.xyz));
    v.s = s;
    return true;
}

// One lamp, picked in proportion to its unshadowed irradiance here, with one shadow ray, divided by its
// pick probability. The lamp is shaded through the same GGX as the sun (rdLocalLightAt's sphere widening).
float3 ptLamp(AverSurface s, float3 pos, float2 pixel, inout uint rng) {
    const uint n = min(rdLocalLightCount(), 32u);
    if (n == 0u) return float3(0.0, 0.0, 0.0);
    float wsum = 0.0;
    [loop] for (uint i = 0u; i < n; ++i) wsum += averShadowLum(rdLocalIrradiance(gRdLocalLights[i], pos, s.N));
    if (!(wsum > 0.0)) return float3(0.0, 0.0, 0.0);

    const float target = ptRand(rng) * wsum;
    float acc = 0.0, wPick = 0.0;
    uint  pick = 0u;
    [loop] for (uint j = 0u; j < n; ++j) {
        const float wj = averShadowLum(rdLocalIrradiance(gRdLocalLights[j], pos, s.N));
        acc += wj;
        if (wj > 0.0) { pick = j; wPick = wj; }
        if (target < acc && wj > 0.0) break;
    }
    if (!(wPick > 0.0)) return float3(0.0, 0.0, 0.0);

    const RdLocalLight ll = gRdLocalLights[pick];
    const float3 toC   = ll.posRadius.xyz - pos;
    const float  d2    = dot(toC, toC);
    const float  range = ll.radianceRange.w;
    if (d2 >= range * range) return float3(0.0, 0.0, 0.0);
    const float r    = ll.posRadius.w;
    const float x2   = d2 / (range * range);
    const float win  = saturate(1.0 - x2 * x2);
    const float invD = rsqrt(max(d2, 1e-8));
    AverLight l;
    l.direction  = toC * invD;
    l.radiance   = ll.radianceRange.rgb * (1e4 / max(d2, r * r)) * (win * win);
    l.visibility = rdLocalShadow(pos, s.N, ll, pixel, ptRand(rng) * 6.2831853).xxx;
    AverSurface sL = s;
    sL.rough = clamp(s.rough + r * 0.5 * invD, s.rough, 1.0);
    ptAim(sL, l.direction);
    return averShadeDirect(float3(0.0, 0.0, 0.0), sL, l) * (wsum / wPick);
}

// Direct light at a vertex: the sun (one shadow ray) and one lamp.
float3 ptDirect(PtVertex v, float2 pixel, inout uint rng) {
    const float3 L = normalize(gLightDir.xyz);
    AverLight sun;
    sun.direction  = L;
    sun.radiance   = averSunRadiance();
    sun.visibility = dot(v.s.N, L) > 0.0 ? rtShadowOpaque(v.pos, v.s.N, L, pixel, ptRand(rng) * 6.2831853)
                                         : float3(0.0, 0.0, 0.0);
    return averShadeDirect(float3(0.0, 0.0, 0.0), v.s, sun) + ptLamp(v.s, v.pos, pixel, rng);
}

// Smith G1 for GGX, alpha = rough^2.
float ptSmithG1(float ndx, float a2) { return 2.0 * ndx / (ndx + sqrt(a2 + (1.0 - a2) * ndx * ndx)); }

// Samples the next direction from the surface's BSDF. weight = f * cos / pdf for the lobe chosen.
// Specular: GGX VNDF, whose estimator is F * G1(L). Diffuse: cosine-weighted, whose estimator is kd.
// The lobe is picked by its share of reflected energy, and its weight divided by that probability.
bool ptSampleBsdf(AverSurface s, inout uint rng, out float3 dir, out float3 weight) {
    dir = s.N;
    weight = float3(0.0, 0.0, 0.0);
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
        weight = fresnelSchlick(saturate(dot(s.V, H)), s.F0, s.f90) * ptSmithG1(ndl, alpha * alpha) / pSpec;
    } else {
        const float u1 = ptRand(rng), u2 = ptRand(rng);
        const float r = sqrt(u1), phi = 6.2831853 * u2;
        dir = normalize(T * (r * cos(phi)) + B * (r * sin(phi)) + s.N * sqrt(max(1.0 - u1, 0.0)));
        weight = s.kdAlbedo / (1.0 - pSpec);
    }
    return any(weight > 0.0);
}

// Radiance arriving at v along the rest of its path: up to `depth` more vertices sampled from the
// BSDF, each adding emission + direct light, a sky miss ending it. Russian roulette from the third.
float3 ptContinue(PtVertex v, float2 pixel, inout uint rng, uint depth) {
    float3 sum = float3(0.0, 0.0, 0.0);
    float3 thr = float3(1.0, 1.0, 1.0);
    [loop] for (uint b = 0u; b < depth; ++b) {
        float3 dir, w;
        if (!ptSampleBsdf(v.s, rng, dir, w)) break;
        thr *= w;
        const float bias = ptBias(v.pos);
        PtVertex nv;
        if (!ptTrace(v.pos + v.s.N * bias, dir, bias, v.s.rough * v.s.rough + 0.1, nv)) {
            sum += thr * averSkyRadianceCheap(dir) * gAmbient.r;
            break;
        }
        sum += thr * (nv.s.emissive + ptDirect(nv, pixel, rng));
        if (b >= 1u) {
            const float p = clamp(max(thr.r, max(thr.g, thr.b)), 0.05, 0.95);
            if (ptRand(rng) > p) break;
            thr /= p;
        }
        v = nv;
    }
    return sum;
}

// Outgoing radiance toward the ray from the first surface it hits: what a reflection sees.
// `skyFallback` is the caller's own sky lookup for a miss.
float3 ptRadiance(float3 origin, float3 dir, float tmin, float cone, float2 pixel, uint stream,
                  float3 skyFallback) {
    PtVertex v;
    if (!ptTrace(origin, dir, tmin, cone, v)) return skyFallback;
    uint rng = ptSeed(pixel, stream);
    return v.s.emissive + ptDirect(v, pixel, rng) + ptContinue(v, pixel, rng, ptBounceCount() - 1u);
}
#endif
