// Aver.Particles: the billboard vertex/pixel pair.
//
// Moved out of a C++ raw-string literal. It is compiled the same way it always was -- see the call
// site for which prelude it is composed with -- but it is now a file the shader watcher can see,
// scripts/check-code-unchanged.py can normalise, and a payload can ship.

struct ParticleVSIn {
    // Split across two POSITION slots rather than one float3: this RHI's Format enum has no
    // three-component float format for a caller-owned vertex layout (RG32Float is its widest float
    // vertex format) -- see ParticleRenderer.cpp's vertex layout for the byte offsets this pairs
    // with. Reconstructed into one float3 immediately below; nothing past that line knows the split
    // ever happened.
    float2 posXY : POSITION0;
    float  posZ  : POSITION1;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;   // premultiplied by ParticleRenderer -- see its packPremultiplied()
};

struct ParticleVSOut {
    float4 pos   : SV_Position;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
    // World position, forwarded ONLY so the GI variant's pixel shader can sample the volume at this
    // fragment -- see ParticlePS's AVER_PARTICLES_GI block. Always written (cheap: one interpolant,
    // no branch), even by a build with no GI seam at all, so ParticleVS itself never needs its own
    // #ifdef ladder; the branch that matters is entirely in ParticlePS below.
    float3 wpos  : TEXCOORD1;
};

ParticleVSOut ParticleVS(ParticleVSIn i) {
    ParticleVSOut o;
    float3 worldPos = float3(i.posXY, i.posZ);
    o.pos = mul(float4(worldPos, 1.0), gViewProj);
    o.uv = i.uv;
    o.color = i.color;
    o.wpos = worldPos;
    return o;
}

// A soft circular falloff, so an unlit quad reads as a point sprite rather than a hard-edged
// square. Every effect draws THIS shape until DECIDED 3's asset can name an actual texture -- see
// ParticleEffect::textureId. Applied to both rgb and alpha so the premultiplied invariant the vertex
// colour already carries (rgb <= alpha, componentwise) survives the falloff instead of the edge
// picking up a colour fringe from an alpha that faded faster than its rgb.
float4 ParticlePS(ParticleVSOut i) : SV_Target {
    float2 c = i.uv * 2.0 - 1.0;
    float mask = saturate(1.0 - dot(c, c));
    mask *= mask;
    float4 o = i.color;
#if AVER_PARTICLES_GI
    // THE GI TERM. coneTracedIndirect (declared by whatever prelude ParticleRenderer::GiSeam::prepare
    // returned -- see that struct's own "name contract" comment; this file never learns it is Voxi's)
    // wants a surface normal. A camera-facing billboard has no real one, so N is the vector FROM the
    // particle TOWARD the camera -- the same convention a flat card catching light from the viewer's
    // side would use, and exact rather than approximate for what it is: this quad's own plane really
    // is perpendicular to that vector, by construction (see ParticleRenderer.cpp's cameraBasis()).
    // A puff of smoke is not a flat card, so this is a deliberate, honest simplification, not a bug --
    // an omnidirectional multi-sample average would suit a volumetric puff better and is future work,
    // not this slice's.
    //
    // MODULATED, not added: coneTracedIndirect already returns irradiance*intensity in roughly the
    // same units PSMainVoxi multiplies a surface's own albedo by (VoxiShaders.hpp's own ind4.diffuse
    // use) -- so `albedo * (indirect * ao)` is the same shape that call site uses, not an invented
    // one. This is the WHOLE lighting model particles get: no direct sun term (no stable N to light
    // against it with), no ambient sky term -- see this comment's own opening paragraph for why a
    // bounce-lit but sun-unlit particle is still an honest, if partial, answer rather than a fake one.
    float3 N = normalize(gCamPos.xyz - i.wpos);
    float ao = 1.0;
    float3 gi = coneTracedIndirect(i.wpos, N, ao);
    o.rgb *= gi * ao;
#endif
    o.rgb *= mask;
    o.a *= mask;
    return o;
}
