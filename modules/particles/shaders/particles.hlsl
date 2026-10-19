// Aver.Particles: the draw shaders. One file, three vertex entries and one pixel entry:
//   ParticleSpriteVS   camera-facing, velocity-stretched or axis-aligned quads (4-vertex template, one instance per particle)
//   ParticleRibbonVS   trail ribbons (one instance per particle segment) and linked ribbons (one instance per ring slot)
//   ParticleMeshVS     a small mesh per particle (the mesh's own vertex / index buffers, one instance per particle)
//   ParticlePS         texture / SubUV, the procedural dot, lighting and fog
// Every vertex entry reads particles from a StructuredBuffer by SV_InstanceID through an order list, so the CPU tier's
// uploaded array and the GPU tier's pool buffer draw through the same code. Compiled as rhi::sharedShaderPrelude() +
// (with a GI seam installed) Voxi's GI prelude + this file; AVER_PARTICLES_GI=1 switches on the GI term and the shadowed sun.
//
// Struct names are prefixed (PVSIn, PVSOut): sharedShaderPrelude() already declares VSIn / VSOut.
#include "particles_common.hlsli"

struct AverParticleLight {
    float4 posRange;     // xyz position (cm), w range (cm; 0 unbounded)
    float4 radiance;     // rgb colour * irradiance at 1 m (engine radiance units), w source radius (cm)
    float4 axisKind;     // xyz axis, w kind (0 none, 1 point, 2 spot, 3 rect)
    float4 cone;         // x innerCos, y outerCos
};

// ParticleDrawCB (ParticleRenderer.cpp mirrors this field for field).
cbuffer ParticleDraw : register(b3) {
    float4x4 gPVP;        // the view-projection and eye of THIS pass (the editor preview draws with its own camera, not the engine's b0)
    float4 gPEye;
    float4 gCamRight;     // xyz camera right, w alignment (0 camera, 1 velocity, 2 axis)
    float4 gCamUp;        // xyz camera up, w velocity stretch
    float4 gAxis;         // xyz alignment axis (unit), w 1 when the instance list is an order list
    float4 gSize;         // x sizeStart, y sizeEnd, z size curve stops (0 none), w mesh scale (1 / mesh diameter)
    float4 gSizeT;        // size curve times
    float4 gSizeV;        // size curve values
    float4 gColA;         // colorStart (straight rgba)
    float4 gColB;         // colorEnd
    float4 gColT;         // colour curve times
    float4 gColV[4];      // colour curve stops (straight rgba)
    float4 gSubuv;        // x cols, y rows, z frames, w fps (0: once over the particle life)
    float4 gFlags;        // x has texture, y SubUV blend, z fog, w lighting (0 unlit, 1 ambient, 2 lit)
    float4 gTrailInfo;    // x ring points stored per particle, y trail fade, z linked ring capacity (0: not linked)
    float4 gMisc;         // x additive, y mesh orientation, z colour curve stops (0 none), w kind (0 sprite, 1 ribbon, 2 mesh)
    AverParticleLight gLights[4];
};

StructuredBuffer<PParticle> gParticles : register(t0);
StructuredBuffer<uint>      gOrder     : register(t1);
StructuredBuffer<float4>    gTrailPts  : register(t2);
Texture2D                   gTex       : register(t3);
SamplerState                gSamp      : register(s0);

struct PQuadIn { float2 corner : POSITION0; };
struct PMeshIn {
    float2 posXY : POSITION0; float posZ : POSITION1;
    float2 nrmXY : NORMAL0;   float nrmZ : NORMAL1;
    float2 uv : TEXCOORD0;
};
struct PVSOut {
    float4 pos    : SV_Position;
    float4 uvAB   : TEXCOORD0;   // frame A (xy) and frame B (zw) texture coordinates
    float4 color  : COLOR0;      // straight alpha: curves * tint (* trail fade)
    float3 wpos   : TEXCOORD1;
    float3 normal : TEXCOORD2;
    float3 fogExt : TEXCOORD3;
    float3 fogIns : TEXCOORD4;
    float  subBlend : TEXCOORD5;
    float2 uvLocal : TEXCOORD6;  // 0..1 across the particle, for the procedural dot / ribbon edge
};

float4 pUnpackColor(uint c) {
    return float4((float)(c & 255u), (float)((c >> 8) & 255u), (float)((c >> 16) & 255u), (float)(c >> 24)) * (1.0 / 255.0);
}

float pEvalSize(float t) {
    int n = (int)gSize.z;
    if (n < 2) return lerp(gSize.x, gSize.y, t);
    float ts[4] = { gSizeT.x, gSizeT.y, gSizeT.z, gSizeT.w };
    float vs[4] = { gSizeV.x, gSizeV.y, gSizeV.z, gSizeV.w };
    if (t <= ts[0]) return vs[0];
    for (int i = 0; i < 3; ++i) {
        if (i + 1 < n && t <= ts[i + 1]) {
            float span = ts[i + 1] - ts[i];
            float u = span > 1e-8 ? (t - ts[i]) / span : 1.0;
            return lerp(vs[i], vs[i + 1], u);
        }
    }
    return vs[max(n - 1, 0)];
}

float4 pEvalColor(float t) {
    int n = (int)gMisc.z;
    if (n < 2) return lerp(gColA, gColB, t);
    float ts[4] = { gColT.x, gColT.y, gColT.z, gColT.w };
    if (t <= ts[0]) return gColV[0];
    for (int i = 0; i < 3; ++i) {
        if (i + 1 < n && t <= ts[i + 1]) {
            float span = ts[i + 1] - ts[i];
            float u = span > 1e-8 ? (t - ts[i]) / span : 1.0;
            return lerp(gColV[i], gColV[i + 1], u);
        }
    }
    return gColV[max(n - 1, 0)];
}

float pLifeFraction(PParticle p) { return p.lifetime > 0.0 ? saturate(p.age / p.lifetime) : 1.0; }

// SubUV: the two frames' coordinates and the blend between them.
void pSubUv(PParticle p, float t, float2 uvl, out float4 uvAB, out float blend) {
    float cols = max(gSubuv.x, 1.0), rows = max(gSubuv.y, 1.0), frames = max(gSubuv.z, 1.0);
    float f = 0.0;
    if (frames > 1.0) {
        f = (gSubuv.w > 0.0 ? p.age * gSubuv.w : t * frames) + (float)(p.spawnInfo & 0xFFFFu);
    }
    float fl = floor(f);
    float k0 = gSubuv.w > 0.0 ? fmod(fl, frames) : min(fl, frames - 1.0);
    float k1 = gSubuv.w > 0.0 ? fmod(fl + 1.0, frames) : min(fl + 1.0, frames - 1.0);
    float2 cell = float2(cols, rows);
    uvAB = float4((uvl + float2(fmod(k0, cols), floor(k0 / cols))) / cell,
                  (uvl + float2(fmod(k1, cols), floor(k1 / cols))) / cell);
    blend = frac(f);
}

void pFog(float3 world, out float3 ext, out float3 ins) {
    ext = float3(1, 1, 1);
    ins = float3(0, 0, 0);
    if (gFlags.z > 0.5) averFogTermsAirVis(world, true, 1.0, ext, ins);
}

PVSOut pFinish(float3 world, float3 normal, float4 color, float2 uvl, PParticle p, float t) {
    PVSOut o;
    o.pos = mul(float4(world, 1.0), gPVP);
    o.wpos = world;
    o.normal = normal;
    o.color = color;
    o.uvLocal = uvl;
    pSubUv(p, t, uvl, o.uvAB, o.subBlend);
    pFog(world, o.fogExt, o.fogIns);
    return o;
}

// ---------------------------------------------------------------------------------------------------- sprites
PVSOut ParticleSpriteVS(PQuadIn vin, uint inst : SV_InstanceID) {
    uint slot = (gAxis.w > 0.5) ? gOrder[inst] : inst;
    PParticle p = gParticles[slot];
    float t = pLifeFraction(p);
    float size = pEvalSize(t) * p.sizeScale;
    float4 col = pEvalColor(t) * pUnpackColor(p.tint);
    float2 c = vin.corner;
    uint mode = (uint)gCamRight.w;
    float3 right = gCamRight.xyz, up = gCamUp.xyz;
    float3 world, N;
    if (mode == 1u) {            // velocity: stretch along the projected velocity
        float vx = dot(p.velocity, right), vy = dot(p.velocity, up);
        float sp2 = sqrt(vx * vx + vy * vy);
        float2 d = sp2 > 1e-4 ? float2(vx, vy) / sp2 : float2(1.0, 0.0);
        float speed = length(p.velocity);
        float halfLen = size * 0.5 * (1.0 + gCamUp.w * speed * 0.01);
        float halfW = size * 0.5;
        float2 o2 = d * (c.x * halfLen) + float2(-d.y, d.x) * (c.y * halfW);
        world = p.position + right * o2.x + up * o2.y;
        N = normalize(gPEye.xyz - p.position);
    } else if (mode == 2u) {     // axis: the quad lies in the plane perpendicular to the axis
        float3 ax = gAxis.xyz;
        float3 h = abs(ax.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
        float3 u = normalize(cross(ax, h));
        float3 v = cross(ax, u);
        float s = sin(p.rotation), co = cos(p.rotation);
        float2 r = float2(co * c.x - s * c.y, s * c.x + co * c.y) * (size * 0.5);
        world = p.position + u * r.x + v * r.y;
        N = ax;
    } else {                     // camera facing, rotated in the quad plane
        float s = sin(p.rotation), co = cos(p.rotation);
        float2 r = float2(co * c.x - s * c.y, s * c.x + co * c.y) * (size * 0.5);
        world = p.position + right * r.x + up * r.y;
        N = normalize(gPEye.xyz - p.position);
    }
    float2 uvl = float2(c.x, -c.y) * 0.5 + 0.5;
    return pFinish(world, N, col, uvl, p, t);
}

// ---------------------------------------------------------------------------------------------------- ribbons
float3 pTrailPoint(uint slot, PParticle p, uint j, uint ring) {
    if (j == 0u) return p.position;
    uint head = p.trail & 255u;
    uint idx = (head + ring - (j - 1u)) % ring;
    return gTrailPts[slot * ring + idx].xyz;
}

PVSOut pDegenerate() {
    PVSOut o = (PVSOut)0;
    o.pos = float4(0, 0, 0, 0);
    o.fogExt = float3(1, 1, 1);
    return o;
}

PVSOut pTrailRibbon(PQuadIn vin, uint inst) {
    uint ring = max((uint)gTrailInfo.x, 1u);
    uint pi = inst / ring, s = inst % ring;
    uint slot = (gAxis.w > 0.5) ? gOrder[pi] : pi;
    PParticle p = gParticles[slot];
    uint count = (p.trail >> 8) & 255u;
    if (s >= count) return pDegenerate();
    uint j = s + (vin.corner.x > 0.0 ? 1u : 0u);
    float3 P = pTrailPoint(slot, p, j, ring);
    float3 Pp = pTrailPoint(slot, p, j > 0u ? j - 1u : 0u, ring);
    float3 Pn = pTrailPoint(slot, p, min(j + 1u, count), ring);
    float3 tang = Pn - Pp;
    float3 view = normalize(P - gPEye.xyz);
    float3 side = cross(tang, view);
    float sl = length(side);
    side = sl > 1e-4 ? side / sl : gCamRight.xyz;
    float t = pLifeFraction(p);
    float size = pEvalSize(t) * p.sizeScale;
    float along = (float)j / (float)ring;
    float fade = saturate(1.0 - gTrailInfo.y * along);
    float4 col = pEvalColor(t) * pUnpackColor(p.tint);
    col.a *= fade;
    float3 world = P + side * (vin.corner.y * size * 0.5);
    float2 uvl = float2(along, -vin.corner.y * 0.5 + 0.5);
    PVSOut o = pFinish(world, -view, col, uvl, p, t);
    return o;
}

bool pLinkAlive(PParticle p) { return p.age < p.lifetime; }

PVSOut pLinkedRibbon(PQuadIn vin, uint inst) {
    uint cap = (uint)gTrailInfo.z;
    uint k = inst;
    uint prev = (k + cap - 1u) % cap;
    PParticle p = gParticles[k];
    PParticle q = gParticles[prev];
    if (!pLinkAlive(p) || !pLinkAlive(q) || p.id != q.id + 1u) return pDegenerate();
    bool head = vin.corner.x > 0.0;
    PParticle e = head ? p : q;
    float3 A = q.position, B = p.position;
    float3 tang = B - A;
    if (head) {
        PParticle nx = gParticles[(k + 1u) % cap];
        if (pLinkAlive(nx) && nx.id == p.id + 1u) tang = nx.position - A;
    } else {
        PParticle pp = gParticles[(k + cap - 2u) % cap];
        if (pLinkAlive(pp) && pp.id + 1u == q.id) tang = B - pp.position;
    }
    float3 P = head ? B : A;
    float3 view = normalize(P - gPEye.xyz);
    float3 side = cross(tang, view);
    float sl = length(side);
    side = sl > 1e-4 ? side / sl : gCamRight.xyz;
    float t = pLifeFraction(e);
    float size = pEvalSize(t) * e.sizeScale;
    float4 col = pEvalColor(t) * pUnpackColor(e.tint);
    float3 world = P + side * (vin.corner.y * size * 0.5);
    float2 uvl = float2(head ? 1.0 : 0.0, -vin.corner.y * 0.5 + 0.5);
    return pFinish(world, -view, col, uvl, e, t);
}

PVSOut ParticleRibbonVS(PQuadIn vin, uint inst : SV_InstanceID) {
    if (gTrailInfo.z > 0.5) return pLinkedRibbon(vin, inst);
    return pTrailRibbon(vin, inst);
}

// ---------------------------------------------------------------------------------------------------- mesh particles
PVSOut ParticleMeshVS(PMeshIn vin, uint inst : SV_InstanceID) {
    uint slot = (gAxis.w > 0.5) ? gOrder[inst] : inst;
    PParticle p = gParticles[slot];
    float t = pLifeFraction(p);
    float scale = pEvalSize(t) * p.sizeScale * gSize.w;
    float4 col = pEvalColor(t) * pUnpackColor(p.tint);

    uint orient = (uint)gMisc.y;
    float3 X, Y, Z;
    if (orient == 2u) {          // fixed: spin about +Z
        float s = sin(p.rotation), co = cos(p.rotation);
        X = float3(co, s, 0); Y = float3(-s, co, 0); Z = float3(0, 0, 1);
    } else {                     // velocity, or velocity then a spin about the travel axis
        float sp = length(p.velocity);
        float3 fwd = sp > 1e-3 ? p.velocity / sp : float3(1, 0, 0);
        float3 helper = abs(fwd.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
        float3 y0 = normalize(cross(helper, fwd));
        float3 z0 = cross(fwd, y0);
        X = fwd; Y = y0; Z = z0;
        if (orient == 1u) {
            float s = sin(p.rotation), co = cos(p.rotation);
            Y = y0 * co + z0 * s;
            Z = -y0 * s + z0 * co;
        }
    }
    float3 lp = float3(vin.posXY, vin.posZ) * scale;
    float3 ln = float3(vin.nrmXY, vin.nrmZ);
    float3 world = p.position + X * lp.x + Y * lp.y + Z * lp.z;
    float3 N = normalize(X * ln.x + Y * ln.y + Z * ln.z);
    return pFinish(world, N, col, vin.uv, p, t);
}

// ---------------------------------------------------------------------------------------------------- pixel
float3 pLocalLights(float3 wpos, float3 N) {
    float3 sum = float3(0, 0, 0);
    [unroll] for (int k = 0; k < 4; ++k) {
        AverParticleLight l = gLights[k];
        float kind = l.axisKind.w;
        if (kind < 0.5) continue;
        float3 d = l.posRange.xyz - wpos;
        float dist = length(d);
        float3 Ld = d / max(dist, 1e-4);
        float dm = dist * 0.01;
        float src = max(l.radiance.w, 1.0) * 0.01;
        float atten = 1.0 / (dm * dm + src * src);
        float range = l.posRange.w;
        if (range > 0.0) atten *= saturate(1.0 - pow(saturate(dist / range), 4.0));
        float cone = 1.0;
        if (kind > 1.5 && kind < 2.5) {
            float cd = dot(-Ld, l.axisKind.xyz);
            cone = saturate((cd - l.cone.y) / max(l.cone.x - l.cone.y, 1e-4));
        }
        float wrapN = saturate(dot(N, Ld) * 0.5 + 0.5);
        sum += l.radiance.rgb * (atten * cone * wrapN);
    }
    return sum;
}

float4 ParticlePS(PVSOut i) : SV_Target {
    float4 tex = float4(1, 1, 1, 1);
    float mask = 1.0;
    if (gFlags.x > 0.5) {
        tex = gTex.Sample(gSamp, i.uvAB.xy);
        if (gFlags.y > 0.5) tex = lerp(tex, gTex.Sample(gSamp, i.uvAB.zw), i.subBlend);
    } else if (gMisc.w < 0.5) {
        // The procedural soft dot: a falloff over the quad, applied to alpha so premultiplying carries it into rgb.
        float2 c = i.uvLocal * 2.0 - 1.0;
        mask = saturate(1.0 - dot(c, c));
        mask *= mask;
    } else if (gMisc.w < 1.5) {
        float cy = i.uvLocal.y * 2.0 - 1.0;
        mask = saturate(1.0 - cy * cy);
    }
    float4 o = i.color;
    o.rgb *= tex.rgb;
    o.a *= tex.a * mask;

    int lighting = (int)gFlags.w;
    if (lighting >= 1) {
        float3 N = normalize(i.normal);
        float ao = 1.0;
        float3 irradiance = float3(1, 1, 1);
#if AVER_PARTICLES_GI
        // coneTracedIndirect is declared by whatever prelude ParticleRenderer::GiSeam::prepare returned (Voxi's, but this
        // file never learns the name). A billboard has no stable normal, so N is toward the camera and the term is a
        // modulation of the particle colour, exactly the shape the scene's own diffuse call uses.
        float3 gi = coneTracedIndirect(i.wpos, N, ao);
        if (lighting == 1) {
            irradiance = gi * ao;
        } else {
            float3 L = normalize(gLightDir.xyz);
            float ndl = dot(N, L);
            float vis = shadowFactor(i.wpos, N, saturate(ndl));
            float3 sun = averSunRadiance() * (vis * saturate(ndl * 0.5 + 0.5) / PI);
            irradiance = (sun + gi + pLocalLights(i.wpos, N) / PI) * ao;
        }
#else
        if (lighting == 2) {
            // No GI seam: an unshadowed sun and the sky.
            float3 L = normalize(gLightDir.xyz);
            float ndl = dot(N, L);
            float3 sun = averSunRadiance() * (saturate(ndl * 0.5 + 0.5) / PI);
            irradiance = sun + averSkyIrradiance(N) + pLocalLights(i.wpos, N) / PI;
        }
#endif
        o.rgb *= irradiance;
    }

    float a = o.a;
    float3 rgb = o.rgb * a;
    if (gMisc.x > 0.5) rgb *= i.fogExt;
    else rgb = rgb * i.fogExt + i.fogIns * a;
    return float4(rgb, a);
}
