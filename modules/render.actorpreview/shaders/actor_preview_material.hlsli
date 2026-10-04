// The preview's MATERIAL entry point (PreviewMaterialPS), appended to actor_preview.hlsl.
//
// STILL TWO FILES, CONCATENATED, NOT ONE WITH AN #ifdef -- and the split is load-bearing. The
// material text must reach the material pipeline and must NEVER reach the simple one, which is what
// lets ActorPreviewTest assert "no shader source mentions AVER_MATERIAL_GRAPH" for the no-graph case
// and mean it literally: the macro name appears in no .source string the simple pipeline is given.
// Merging these would make that assertion untrue while every pixel still looked right.

// ---- the MATERIAL path: the same AverVertex/AverSurface contract PbrShaders.cpp declares, so the
// graph editor can put a .ocgraph's own averEvalMaterial on this sphere ----
//
// Shaded with the EXACT SAME key light and hemisphere fill PreviewPS uses above -- not the material
// system's own BRDF (averShadeDirect/averShadeIndirect), which PbrShaders.cpp's own tests already
// exercise end to end. The only thing this entry point changes relative to PreviewPS is WHERE the
// surface colour comes from: gBaseColor there, the graph's own averEvalMaterial here. Sharing the
// lighting model is what keeps the two previews COMPARABLE side by side, rather than one reading
// brighter or flatter because it took a different shading path.
float4 PreviewMaterialPS(PreviewOut i) : SV_TARGET {
    AverVertex v;
    v.wpos = i.wpos;
    v.N    = normalize(i.nrmWS);
    v.V    = normalize(gPreviewEye.xyz - i.wpos);
    // Two-sided, exactly like averVertexOf's own comment in PbrShaders.cpp: a closed preview sphere
    // never needs this, but a future flat preview mesh (a plane, say) should not shade black on the
    // half of it facing away from the light.
    v.backFace = dot(v.N, v.V) < 0.0;
    if (v.backFace) v.N = -v.N;
    v.uv = i.uv;

    // averBuildSurface reads l.direction alone, to build the half vector H. radiance and visibility
    // exist for averShadeDirect/averShadeIndirect, neither of which this entry point calls, so there
    // is nothing honest to compute for them here -- they are left at the identity rather than wired
    // to a light this shader shades with its own formula, not the BRDF's.
    AverLight l;
    l.direction  = normalize(gPreviewKey.xyz);
    l.radiance   = float3(0.0, 0.0, 0.0);
    l.visibility = 1.0;

    AverSurface s = averEvalMaterial(v, l);

    // The same view modes as PreviewPS; Normals shows the material's shading normal, maps included.
    uint mode = (uint)gPreviewMode.x;
    if (mode == kPreviewModeNormals) return float4(s.N * 0.5 + 0.5, 1.0);
    if (mode == kPreviewModeWire) return float4(previewWireColor(s.albedo), 1.0);

    float3 lit = s.albedo;
    if (mode != kPreviewModeUnlit) {
        float ndl = saturate(dot(s.N, l.direction));
        float up = s.N.z * 0.5 + 0.5;
        float3 fill = lerp(gPreviewAmbient.rgb * 0.35, gPreviewAmbient.rgb, up);
        lit = s.albedo * (fill + ndl * gPreviewKey.w);
    }

    float rim = pow(1.0 - saturate(dot(s.N, v.V)), 3.0);
    lit += gPreviewAmbient.w * rim * float3(1.0, 0.62, 0.2);

    return float4(toGamma(acesTonemap(lit)), 1.0);
}
