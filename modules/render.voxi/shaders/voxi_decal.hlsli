// Projected decals (docs/rendering/DECALS.md): box projectors that repaint base colour, normal and
// roughness on the surface at a point, BEFORE that surface is lit.
//
// ONE FUNCTION, TWO CALLERS: PSMainVoxi (raster) calls averApplyDecals after averEvalMaterial, and
// rtHitSurface (voxi_rt.hlsli) after averComposeSurface -- the staged ray-driven primary surface, its
// reflection and GI hits and Path Tracing vertices all build their surface there, so a decal shows up
// in all of them from this one edit. Nothing here samples with implicit derivatives (the footprint is
// the pixel cone at the point's distance), so it is legal inside a divergent loop in a compute pass.
//
// With no decals gDecalParams.x is 0 and both call sites skip the call on a uniform branch.
//
// MUST FOLLOW: the VoxiFrame cbuffer (gDecalParams, gViewProj, gCamPos), the material prelude
// (AverSurface, gMaterialSampler, fresnelSchlick) and, in bindless variants, gRtTextures.
#ifndef AVER_VOXI_DECAL_HLSLI
#define AVER_VOXI_DECAL_HLSLI

// PackedDecal::ext.w bits (SceneDecal.hpp's kDecalPack*).
#define AVER_DECAL_COLOUR     0x01u
#define AVER_DECAL_NORMAL     0x02u
#define AVER_DECAL_ROUGH      0x04u
#define AVER_DECAL_HAS_BASE   0x08u
#define AVER_DECAL_HAS_NORMAL 0x10u
#define AVER_DECAL_HAS_ORM    0x20u
#define AVER_DECAL_TEXTURED   0x40u

// SceneDecal.hpp's PackedDecal, field for field (192 bytes).
struct AverDecalRec {
    float4 r0, r1, r2;     // local = prel.x*r0.xyz + prel.y*r1.xyz + prel.z*r2.xyz + float3(r0.w, r1.w, r2.w)
    float4 ext;            // xyz half extents, w flag bits
    float4 tint;           // rgb tint (linear), a opacity
    float4 shade;          // normal strength, roughness, metallic, edge fade
    float4 ang;            // x cos(full angle), y cos(zero angle), zw uv scale
    float4 uvo;            // xy uv offset
    uint4  tex;            // bindless indices: base, normal, orm
    float4 ax, ay, az;     // unit world axes of box-local X (projection), Y (u), Z (v up)
};

StructuredBuffer<AverDecalRec> gDecals : register(t24);

// Radians one pixel subtends, from the projection's own focal length.
float averDecalPixelAngle() {
    const float h = gSceneViewportCur.w > 0.0 ? gSceneViewportCur.w : gSceneViewport.w;
    return 2.0 / max(abs(gViewProj[1][1]) * h, 1.0);
}

#ifdef AVER_RT_BINDLESS
// Mip for a decal image: texels one pixel covers, from the footprint in cm and the box's size in uv.
float averDecalMip(uint idx, float footCm, float2 halfExtYZ, float2 uvScale) {
    uint w, h;
    gRtTextures[NonUniformResourceIndex(idx)].GetDimensions(w, h);
    const float2 texPerPx = footCm * uvScale / (2.0 * halfExtYZ) * float2((float)w, (float)h);
    return max(log2(max(max(texPerPx.x, texPerPx.y), 1e-6)), 0.0);
}

float4 averDecalSample(uint idx, float2 uv, float mip, float4 fallback) {
    if (idx == AVER_TEX_UNBOUND) return fallback;
    return gRtTextures[NonUniformResourceIndex(idx)].SampleLevel(gMaterialSampler, uv, mip);
}
#endif

// Paints every decal whose box contains `wpos` onto `s`, in list order (the CPU sorts by paint order).
// geoN is the unit GEOMETRIC normal facing the viewer. `full` false rewrites only roughness and the
// normal (the reflection gate's ROUGHNORMAL surfaces read nothing else); the result for those two is
// the same either way.
void averApplyDecals(inout AverSurface s, float3 wpos, float3 geoN, bool full) {
    if (s.model == AVER_MODEL_UNLIT) return;
    const uint count = (uint)gDecalParams.x;
    const float3 prel = wpos - gCamPos.xyz;
    const float footCm = max(length(prel) * averDecalPixelAngle(), 1e-3);

    float3 alb = s.albedo;
    float  rgh = s.rough;
    float  met = s.metallic;
    float3 nrm = s.N;
    bool touched = false;

    [loop] for (uint k = 0; k < count; ++k) {
        const float4 r0 = gDecals[k].r0, r1 = gDecals[k].r1, r2 = gDecals[k].r2;
        const float3 lp = prel.x * r0.xyz + prel.y * r1.xyz + prel.z * r2.xyz + float3(r0.w, r1.w, r2.w);
        const float4 ext = gDecals[k].ext;
        const float3 q = abs(lp) / ext.xyz;
        if (max(q.x, max(q.y, q.z)) >= 1.0) continue;

        const uint flags = asuint(ext.w);
#ifndef AVER_RT_BINDLESS
        if (flags & AVER_DECAL_TEXTURED) continue;   // no table to read the images from: draw nothing, not a flat box
#endif
        const float4 shade = gDecals[k].shade;
        const float4 ang = gDecals[k].ang;
        const float3 axisX = gDecals[k].ax.xyz;

        // Soft box edge, then how squarely the surface faces the projector.
        const float3 inner = saturate((1.0 - q) / max(shade.w, 1e-4));
        float w = min(inner.x, min(inner.y, inner.z));
        w *= saturate((dot(geoN, -axisX) - ang.y) / max(ang.x - ang.y, 1e-4));
        const float4 tint = gDecals[k].tint;
        float cover = tint.a * w;
        if (cover <= 0.0) continue;

        const float2 uv = float2(lp.y / ext.y * 0.5 + 0.5, 0.5 - lp.z / ext.z * 0.5) * ang.zw + gDecals[k].uvo.xy;
        float3 colour = tint.rgb;
        float4 orm = float4(1.0, 1.0, 1.0, 1.0);
        float3 nTS = float3(0.0, 0.0, 1.0);
#ifdef AVER_RT_BINDLESS
        const uint3 ti = gDecals[k].tex.xyz;
        if (flags & AVER_DECAL_HAS_BASE) {
            const float4 b = averDecalSample(ti.x, uv, averDecalMip(ti.x, footCm, ext.yz, ang.zw), float4(1.0, 1.0, 1.0, 1.0));
            colour *= b.rgb;
            cover *= b.a;
            if (cover <= 0.0) continue;
        }
        if ((flags & AVER_DECAL_ROUGH) && (flags & AVER_DECAL_HAS_ORM))
            orm = averDecalSample(ti.z, uv, averDecalMip(ti.z, footCm, ext.yz, ang.zw), orm);
        if ((flags & AVER_DECAL_NORMAL) && (flags & AVER_DECAL_HAS_NORMAL))
            nTS = averDecalSample(ti.y, uv, averDecalMip(ti.y, footCm, ext.yz, ang.zw), float4(0.5, 0.5, 1.0, 1.0)).xyz * 2.0 - 1.0;
#endif

        if ((flags & AVER_DECAL_COLOUR) && full) alb = lerp(alb, colour, cover);
        if (flags & AVER_DECAL_ROUGH) {
            rgh = lerp(rgh, saturate(orm.g * shade.y), cover);
            met = lerp(met, saturate(orm.b * shade.z), cover);
        }
        if (flags & AVER_DECAL_NORMAL) {
            // The map's frame lies on the SURFACE: tangent along the decal's u, bitangent toward its v-up.
            const float3 axisY = gDecals[k].ay.xyz;
            const float3 axisZ = gDecals[k].az.xyz;
            float3 T = axisY - geoN * dot(geoN, axisY);
            if (dot(T, T) < 1e-8) T = axisZ - geoN * dot(geoN, axisZ);
            T = normalize(T);
            float3 B = cross(geoN, T);
            if (dot(B, axisZ) < 0.0) B = -B;
            nTS.xy *= shade.x;
            const float3 dn = normalize(T * nTS.x + B * nTS.y + geoN * max(nTS.z, 1e-3));
            nrm = normalize(lerp(nrm, dn, cover));
        }
        touched = true;
    }
    if (!touched) return;

    s.rough = clamp(rgh, 0.045, 1.0);
    if (dot(nrm, s.V) < 0.0) nrm = normalize(nrm - s.V * dot(nrm, s.V) * 1.01);   // keep it facing the viewer
    s.N = nrm;
    s.ndv = saturate(dot(nrm, s.V));
    if (full) {
        s.albedo = alb;
        s.metallic = saturate(met);
        s.F0 = lerp(s.reflectance.xxx, s.albedo, s.metallic);
        s.kdAlbedo = (1.0 - s.metallic) * s.albedo;
        s.F = fresnelSchlick(saturate(dot(s.H, s.V)), s.F0, s.f90);
    }
}

#endif
