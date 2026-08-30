#pragma once

// The fluid surface's HLSL, following WaterShaders.hpp's exact convention: a raw string literal
// compiled as the TAIL of rhi::sharedShaderPrelude(), which already declares PerFrame at b0
// (gViewProj, gCamPos, gLightDir/gLightColor among its fields) plus srgbToLin/toGamma/averSunRadiance
// and the rest of kColorHlsl -- AND, since sharedShaderPrelude() is one shared string, skyColor/
// averSkyIrradiance and plainFresnelSchlick/plainDistGGX/plainGeomSchlick (RHIShaders.cpp, everything
// it defines before its own #if AVER_MS block). PSFluid below reaches for skyColor and the GGX
// helpers the same way PSWater does -- see WaterShaders.hpp's own comment on envReflection for why
// that needs no new binding. Declaration order is load-bearing: HLSL has no forward declarations, so
// a helper used before it is written compiles in C++ and fails in DXC, AT RUNTIME, on a build that
// reported success (WaterShaders.hpp's own comment makes the identical point, quoting PtShaders.hpp
// before it).
namespace aver::fluids {

inline constexpr const char* kFluidHLSL = R"(
// ---- the per-feature frame block --------------------------------------------------------------
// Feature-owned frame constants at b4 (kFeatureFrameConstantRegister -- RHIResources.hpp's reserved-
// register table), a root CBV like WaterFrame (WaterShaders.hpp) and every other feature-frame block
// in this engine -- but the SMALLEST one: this shader has exactly one per-frame need, an elapsed-
// seconds clock for PSFluid's ripple perturbation below, so gFluidTime carries only that, in x, with
// y/z/w padding the block out to a legal 16-byte constant buffer. MIRRORS FluidScene::transparentPass's
// own fluidFrame[4] array field for field -- that four-float array is the SOURCE OF TRUTH for this
// layout, the same discipline WaterFrameCB/kWaterHLSL's cbuffer keep (see that file's own comment for
// why a field renamed or reordered on one side of this boundary is a silent, image-only bug and not a
// build error: there is no shared-source mechanism between C++ and HLSL anywhere in this engine).
cbuffer FluidFrame : register(b4) {
    float4 gFluidTime;      // x = elapsedSeconds (Timestep::total, threaded through update()); y = the
                            // volume's FLOOR in world Z, cm (desc.centreCm.z - desc.halfExtentCm.z),
                            // which is how deep the column below a surface pixel is; zw unused
    float4 gFluidExtinct;   // rgb = extinction coefficient per CENTIMETRE; a unused
    float4 gFluidBody;      // rgb = in-scattered body colour; a unused
};

// ---- the vertex stage ---------------------------------------------------------------------------
//
// VSFluidIn matches rhi::MeshVertex byte for byte (POSITION0 xyz, NORMAL0 xyz, TEXCOORD0 uv) because
// this pipeline leaves GraphicsPipelineDesc::vertexLayout at its documented default -- "left empty,
// the engine's own MeshVertex" (RHIResources.hpp's own comment on that field) -- rather than
// declaring a custom VertexLayout the way WaterShaders.hpp's VSWaterIn does. That is not a style
// choice: rhi::Format has no three-component float entry at all (only R32Float, RG32Float, RGBA16F,
// ...), so a custom VertexLayout literally cannot express a float3 POSITION0/NORMAL0 attribute in
// this RHI -- only the backend's own hardcoded kMeshInputLayout (D3D12Device.cpp) can, and that is
// exactly what an empty VertexLayout falls back to. TEXCOORD0 is declared to match the buffer's real
// layout and is never read below: FluidScene's packFluidVerts (FluidScene.cpp) writes it as a
// constant (0,0), because this procedural shell has no source UVs to carry.
struct VSFluidIn {
    float3 pos : POSITION0;
    float3 nrm : NORMAL0;
    float2 uv  : TEXCOORD0;
};
struct VSFluidOut {
    float4 clipPos  : SV_Position;
    float3 worldPos : TEXCOORD0;   // for the pixel shader's Fresnel/specular
    float3 worldNrm : TEXCOORD1;
};

VSFluidOut VSFluid(VSFluidIn i) {
    VSFluidOut o;
    // A PASSTHROUGH, not a displacement -- the one place this shader is structurally simpler than
    // WaterShaders.hpp's VSWater, which has to SYNTHESISE both a world position and a normal for an
    // analytic surface that has no other source of either (averGerstnerDisplace). This mesh's
    // position and normal are ALREADY the answer: FluidScene stages ABSOLUTE WORLD-SPACE positions
    // and per-vertex normals RECOMPUTED THIS FRAME from the deformed Jolt soft-body shape
    // (fluids::FluidVolume::updateFromSimulation, called once per frame by FluidScene::update, before
    // this draw ever runs -- see FluidScene.hpp's own header comment for why this buffer is drawn
    // with an IDENTITY world matrix rather than one built here). Deriving a second normal from the
    // geometry, the way an analytic wave surface must, would throw away the one signal the physics
    // solver already computed correctly this frame.
    o.worldPos = i.pos;
    o.worldNrm = i.nrm;
    o.clipPos = mul(float4(i.pos, 1.0), gViewProj);
    return o;
}

// ---- the pixel stage ------------------------------------------------------------------------------

float4 PSFluid(VSFluidOut i) : SV_Target {
    float3 N = normalize(i.worldNrm);

    // A DECORATIVE ripple, not a simulated one -- named that plainly because it is the one place
    // this shader draws something the physics solver never computed, unlike everything else in this
    // file (which either passes the solver's own geometry through unchanged or shades it with real
    // Fresnel optics). Its whole job is surface DETAIL the coarse 8x8x4 shell structurally cannot
    // carry: FluidVolumeDesc's own comment says the horizontal footprint is already the finer of the
    // two subdivided axes, and even so a 6m pool at 8 segments is 75cm per cell -- far too coarse to
    // show anything at the scale real water ripples occur, no matter how the mesh normals are shaded.
    // Two sine waves, not one: a single sine reads as corduroy (parallel stripes, visibly periodic
    // from most angles); a second wave at a different direction, frequency and speed breaks that
    // regularity into something that reads as texture instead of a pattern, the cheapest version of
    // the "sum of a few waves beats looping one" idea Gerstner's own multi-wave sum already leans on
    // for the open ocean (GerstnerWave.hpp), scaled down from "waves" to "ripples". Analytic slope
    // (a cosine derivative), not a second normal map texture or a displaced position: this is ALU
    // only, no new SRV, no new sampler, and no vertex-stage cost at all, because it perturbs the
    // interpolated shading normal PSFluid already has rather than moving any geometry.
    //
    // +Z-up, world XY the horizontal plane (physics_abi.h's own top-of-file convention, restated in
    // WaterShaders.hpp's VSWater axis note) -- so the ripple's height field is a function of
    // (worldPos.x, worldPos.y), and its gradient perturbs N.xy, exactly mirroring how averGerstnerDisplace
    // derives dHdx/dHdz from the SAME kind of cosine term for the ocean's own normal, just without
    // that function's dispersion physics, wave list, or steepness clamp -- there is no simulated
    // wave here to be physically faithful TO, only a target of "does not look flat".
    //
    // kRippleAmp is a SLOPE, not a length -- how far N.xy is nudged per unit of cosine -- picked by
    // screenshot comparison (water_02_ripple.png against water_01_fresnel_exp2.png) as the largest
    // value that reads as fine ripple texture rather than a second, faster slosh riding on top of the
    // real one; kRippleFreq1/2 are 1/cm, chosen so one full cycle is a few hundred cm -- shorter than
    // the pool's own ~200-400cm sloshing wavelength (so the two are visually distinct), longer than a
    // texel (so it never aliases at this mesh's vertex density, since it perturbs a per-pixel-
    // interpolated normal, not per-vertex geometry). kRippleSpeed1/2 differ in both sign and
    // magnitude so the two crests visibly drift apart over time instead of staying in lockstep.
    const float kRippleAmp    = 0.12;
    const float kRippleFreq1  = 0.035;
    const float kRippleFreq2  = 0.052;
    const float kRippleSpeed1 =  1.1;
    const float kRippleSpeed2 = -0.7;
    float t = gFluidTime.x;
    // kRippleAmp is applied DIRECTLY here, with no extra multiply by either frequency: an earlier
    // version of this line multiplied by kRippleFreq{1,2} on the theory that the slope of A*sin(f*x)
    // is A*f*cos(f*x), which is correct calculus but wrong for what kRippleAmp is DEFINED to be
    // above -- that extra factor (~0.035-0.05) silently shrank every slope to about 1/20th of
    // kRippleAmp, which is why the first screenshot taken against this code (before this fix) was
    // byte-identical to the pre-ripple one everywhere except the HUD's own FPS counter: the ripple
    // was there, just about 0.1 degrees of tilt, invisible at any exposure. Caught by diffing
    // water_02_ripple.png against water_01_fresnel_exp2.png pixel-for-pixel instead of trusting the
    // shader compiled and ran without error.
    float2 slope1 = kRippleAmp
                  * cos(dot(i.worldPos.xy, float2(kRippleFreq1, 0.0)) + t * kRippleSpeed1)
                  * float2(1.0, 0.0);
    float2 slope2 = kRippleAmp
                  * cos(dot(i.worldPos.xy, float2(kRippleFreq2 * 0.6, kRippleFreq2)) + t * kRippleSpeed2)
                  * float2(0.6, 1.0);
    N = normalize(N + float3(slope1 + slope2, 0.0));

    float3 V = normalize(gCamPos.xyz - i.worldPos);
    float NdotV = saturate(dot(N, V));

    // Schlick's approximation for the air/water interface. F0 = 0.02 is water's refractive index
    // (~1.33) run through the standard Schlick formula ((1-1.33)/(1+1.33))^2 -- a PHYSICAL constant
    // of the air/water boundary, kept as PSWater's, so the pool and the open-ocean surface still
    // agree on it. The exponent is NOT PSWater's 5.0 any more -- this is the one deliberate
    // divergence from that shader, and it is a divergence in MESH DENSITY, not physics. Schlick's 5.0
    // is the correct fit for a Fresnel term sampled on a surface whose normal is smooth from one
    // shaded point to the next (WaterShaders.hpp's analytic 129x129 Gerstner grid, one normal per
    // pixel by construction). FluidVolume's soft-body shell is an 8x8x4 subdivision -- see
    // FluidVolumeDesc's own comment -- so PSFluid's per-vertex normal is effectively FLAT-shaded
    // across a handful of large facets, and two adjacent facets can differ in tilt by tens of
    // degrees as the sim sloshes. Measured directly (water_00_asis_transparent.png at this file's
    // original 5.0): pow(x,5) is nearly flat for any x below ~0.7 and then rockets to 1 right at the
    // top of its domain, so a moderately-tilted facet contributes almost NOTHING to fresnel while its
    // neighbour one tilt-step further over floods to nearly white -- a hard binary edge between
    // near-black "deep" and near-white "shallow" facets, exactly the blotchy diamond patches that
    // screenshot shows. 2.0 is the cheapest available fix: it widens pow's transition band so a
    // moderate tilt now lands at a genuinely intermediate fresnel value instead of snapping to one
    // extreme, which turns a facet boundary into a gradient instead of an edge -- with NO added
    // runtime cost (still one pow() per pixel) and no touch to F0, which stays the real constant.
    const float kFresnelF0 = 0.02;
    const float kFresnelExponent = 2.0;
    float fresnel = kFresnelF0 + (1.0 - kFresnelF0) * pow(1.0 - NdotV, kFresnelExponent);

    float3 L = normalize(gLightDir.xyz);
    float3 R = reflect(-V, N);

    // ---- THE MIRROR TERM: an ACTUAL reflection, exactly PSWater's fix (WaterShaders.hpp), applied
    // to the same substance. This pool's pipeline compiles against rhi::sharedShaderPrelude() too
    // (FluidScene::init), so skyColor(R) -- the identical dome function PbrShaders/VoxiShaders/PSWater
    // all reflect off -- is reachable here for free, the same discovery PSWater's own comment makes
    // at length: nothing needed binding, the function was already sitting in the prelude this file
    // already compiled against.
    //
    // kShallowColor tints that reflection rather than replacing it -- WaterRenderer.hpp's own default
    // shallowColor_ value, reused VERBATIM (not re-derived) on the same reasoning this file already
    // gave for kDeepColor below: a pool and the open ocean are the SAME SUBSTANCE, so the tuning pass
    // this change leaves open should have ONE pair of numbers to move for both, not two that can
    // silently drift apart.
    // UNTINTED, and that is a correction. What stood here multiplied the reflection by
    // kShallowColor = (0.05, 0.35, 0.45), i.e. it threw away 95% of the red and over half the blue
    // of whatever the surface was reflecting. A dielectric's Fresnel reflection is ACHROMATIC -- F0
    // for water is 0.02 in all three channels, which is exactly what `fresnel` below already is --
    // so the sky arrives at the eye with the sky's own colour, not the water's. Tinting it made the
    // one term that should have lit the surface at grazing angles the darkest thing in the frame,
    // which is a large part of why a pool read as a dark sheet from every angle but straight down.
    // The body colour below is where the water's own colour belongs, and now lives.
    float3 envReflection = skyColor(R) * fresnel;

    // GGX sun glitter, PSWater's identical Cook-Torrance formula (plainDistGGX/plainGeomSchlick,
    // RHIShaders.cpp -- this engine's one GGX, reachable here for the same reason skyColor is) in
    // place of a fixed Blinn-Phong exponent -- see PSWater's own comment for why a fixed exponent
    // cannot express slope-dependent glitter at all.
    //
    // ROUGHNESS IS WIDER HERE THAN PSWater's 0.045, and DELIBERATELY, for the SAME underlying reason
    // kFresnelExponent above is 2.0 instead of PSWater's 5.0: this mesh is the 8x8x4 soft-body shell
    // (FluidVolumeDesc's own comment), flat-shaded across a handful of large facets whose normals can
    // differ by tens of degrees between neighbours as the sim sloshes. A tight GGX lobe reads that
    // facet-to-facet jump as a sparkly, swimming highlight -- the specular-side version of the exact
    // blotchy-Fresnel artifact kFresnelExponent's own comment measured and fixed. Widening the lobe is
    // the equivalent fix for the highlight. UNLIKE kFresnelExponent, this number is NOT measured
    // against a reference screenshot -- it is reasoned by the same analogy, not verified the same way
    // -- so treat it as a starting point for the open tuning pass, not a settled constant.
    const float kFluidRoughness = 0.15;
    float3 H = normalize(L + V);
    float ndl = saturate(dot(N, L));
    float ndh = saturate(dot(N, H));
    float ggxA = kFluidRoughness * kFluidRoughness;
    float ggxK = kFluidRoughness + 1.0; ggxK = ggxK * ggxK / 8.0;
    float D = plainDistGGX(ndh, ggxA);
    float G = plainGeomSchlick(NdotV, ggxK) * plainGeomSchlick(ndl, ggxK);
    float specBRDF = (D * G * fresnel) / max(4.0 * NdotV * ndl, 1e-4);
    float3 sunGlitter = specBRDF * averSunRadiance() * ndl;

    float3 specular = envReflection + sunGlitter;

    // BEER-LAMBERT THROUGH THE WATER COLUMN, replacing a flat constant that could not describe a pool.
    //
    // What stood here was `kDeepColor * (1 - fresnel)` against `alpha = 0.08 + 0.40 * fresnel`: a
    // single hardcoded navy weighted by VIEW ANGLE and nothing else. It has no term for how much
    // water the eye actually looked through, so a pool could not get bluer toward its deep end, could
    // not show its floor through the shallows, and read as one flat dark sheet from every angle that
    // was not grazing -- which is exactly how it looked. The colour was also unreachable: it was a
    // compiled literal here, a duplicate of WaterRenderer.hpp's own default, and WaterRenderer's
    // setColors has zero callers in the tree, so no level could ever have changed either copy.
    //
    // Real water is not a coloured sheet, it is a VOLUME that absorbs. Absorption is exponential in
    // path length and strongly per-channel -- red dies within a metre or so, blue survives tens of
    // metres, and that difference is the whole reason water reads cyan and reads DEEPER cyan the
    // further you look through it. One exp() gets all of that; no amount of tuning a constant does.
    float floorZ  = gFluidTime.y;
    float depthCm = max(i.worldPos.z - floorZ, 0.0);
    // The SLANT path, not the vertical depth: light reaching the eye from the floor crossed
    // depth/|V.z| of water, so a shallow view angle looks through far more of it than a top-down one.
    // Clamped at 0.15 because the true grazing limit is an infinite path -- and it does not matter,
    // since fresnel has taken the surface to a mirror by then and the body term is being multiplied
    // by (1 - fresnel) anyway.
    float pathCm  = depthCm / max(abs(V.z), 0.15);
    float3 T      = exp(-gFluidExtinct.rgb * pathCm);   // transmittance of the column, per channel
    float  Tavg   = dot(T, float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0));

    // The composite this is building, through the PremultipliedAlpha blend state
    // (out = src + dst * (1 - a)), is:
    //
    //     out = F * reflection  +  (1 - F) * [ body * (1 - T)  +  background * T ]
    //
    // so the coverage the blend needs is a = 1 - (1 - F) * T, and everything else rides in src.
    // `specular` above is ALREADY Fresnel-weighted (envReflection multiplies by fresnel, and the sun
    // glitter's BRDF carries it too), so it is the F * reflection term as-is.
    //
    // ONE APPROXIMATION, stated rather than hidden: the blend has a single scalar alpha, so the
    // BACKGROUND can only be attenuated by the average transmittance, while the body colour added on
    // top keeps its full per-channel (1 - T). The chromatic part of the absorption therefore lands in
    // what the water ADDS rather than in what it removes. Getting the removal per-channel as well
    // needs a scene-colour SRV this pass does not have -- IRenderContext::copyTexture exists and is
    // proven (ThumbnailCache is its one caller), so that is a real follow-up, not a dead end.
    // THE COLUMN IS LIT, and treating gFluidBody as a finished radiance rather than a scattering
    // albedo was the other half of the darkness. In-scattered light is albedo TIMES the light that
    // actually reached the water; a constant cannot track the sun going down, the sky changing, or
    // the scene's exposure, so a pool lit by a bright noon sky came out the same dim navy as one at
    // dusk. Zenith sky plus a sun term weighted by its own elevation is the cheap, correct shape:
    // it is what illuminates a horizontal body of water, and it costs one extra skyColor call.
    // averSkyIrradiance, NOT skyColor, AND IT IS BOTH CHEAPER AND MORE CORRECT.
    //
    // skyColor(0,0,1) is a single ray straight up, and under the physical sky that is a 32-step
    // atmosphere march -- run per fragment, for a direction that cannot vary per fragment or per
    // pool within a frame. PSFluid was therefore marching the atmosphere TWICE for every water
    // pixel: once for the reflection direction R, which genuinely varies, and once for this, which
    // does not. The engine had already hit this exact shape and fixed it elsewhere; this call was
    // simply never revisited.
    //
    // MORE CORRECT because the light entering a water column comes from the whole hemisphere above
    // it, not from the zenith alone. averSkyIrradiance is that hemisphere integral, carried in the
    // nine sky SH coefficients baked once per frame on the CPU -- about twenty ALU, no march, and
    // azimuth-aware under the physical atmosphere where a single upward ray is not. It returns a
    // MEAN RADIANCE (the PI is already divided out -- see its own comment), so the units here are
    // unchanged and the sun term below still adds as it did.
    //
    // It also inherits two behaviours this line used to get from skyColor for free: the furnace
    // returns its uniform L, and an AUTHORED sky falls back to the cheap dome rather than the SH.
    float3 inLight = averSkyIrradiance(float3(0.0, 0.0, 1.0)) + averSunRadiance() * saturate(L.z) * 0.25;
    float3 diffuse = (1.0 - fresnel) * gFluidBody.rgb * inLight * (1.0 - T);
    float  alpha   = saturate(1.0 - (1.0 - fresnel) * Tavg);

    // Deliberately NOT calling averApplyFog -- see PSWater's identical comment in WaterShaders.hpp:
    // this is a blended surface sitting in front of pixels the opaque forward pass already fogged,
    // and fogging it a second time would double-apply the same atmosphere to the one surface that is,
    // itself, meant to read as the atmosphere's reflective boundary.

    // PREMULTIPLIED, not straight -- see FluidScene.cpp's blend-state comment, and PSWater's identical
    // fix in WaterShaders.hpp, for the full argument: `specular` must reach the framebuffer at full
    // strength however transparent this pool is authored to be.
    //
    // `diffuse` is NO LONGER multiplied by alpha here, and that is the change, not an oversight. It
    // used to be a body colour needing to be weighted by coverage; it is now the light actually
    // scattered back out of the column, (1 - F) * body * (1 - T), which is already an absolute
    // radiance. Multiplying it by alpha as well would attenuate it twice -- once in its own (1 - T),
    // once in the coverage derived from that same T.
    return float4(specular + diffuse, alpha);
}
)";

} // namespace aver::fluids
