#pragma once

// The fluid surface's HLSL, following WaterShaders.hpp's exact convention: a raw string literal
// compiled as the TAIL of rhi::sharedShaderPrelude(), which already declares PerFrame at b0
// (gViewProj, gCamPos, gLightDir/gLightColor among its fields) plus srgbToLin/toGamma/averSunRadiance
// and the rest of kColorHlsl. Declaration order is load-bearing: HLSL has no forward declarations, so
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
    float4 gFluidTime;   // x = elapsedSeconds (Timestep::total, threaded through update()); yzw unused
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

    // WaterRenderer.hpp's own DEFAULT shallowColor_/deepColor_ (that header's own comment calls them
    // "linear; unvalidated placeholders, see README.md") -- reused VERBATIM here rather than
    // re-derived, on the reasoning that a swimming pool and the ocean grid are the SAME SUBSTANCE and
    // ought to read as one, and that the tuning pass after this change should have ONE pair of
    // numbers to move, not two that can silently drift apart from each other.
    const float3 kDeepColor    = float3(0.02, 0.10, 0.14);
    const float3 kShallowColor = float3(0.10, 0.30, 0.36);
    float3 colorLinear = lerp(kDeepColor, kShallowColor, fresnel);

    // Blinn-Phong sun glitter, the identical shape PSWater uses -- gLightDir/gLightColor are already
    // in the shared PerFrame block at b0, so no new light data is needed. The shininess exponent is
    // not a physically derived quantity; it is the value WaterShaders.hpp settled on for "reads as a
    // highlight, not a mirror," ported unchanged rather than re-tuned for a pool this change never
    // measured against a reference image.
    const float kSpecularShininess = 128.0;
    float3 L = normalize(gLightDir.xyz);
    float3 H = normalize(L + V);
    float specular = pow(saturate(dot(N, H)), kSpecularShininess) * fresnel;
    colorLinear += averSunRadiance() * specular;

    // Straight (NOT premultiplied) alpha, driven by the same Fresnel term -- ported unchanged from
    // PSWater as a starting point for the tuning phase, not re-derived for this pool's own depth or
    // material. kAlphaBase is the floor at NdotV=1 (looking straight down, most transparent);
    // kAlphaFresnelRange is how much a grazing view adds on top of that floor.
    const float kAlphaBase = 0.08;
    const float kAlphaFresnelRange = 0.40;
    float alpha = saturate(kAlphaBase + kAlphaFresnelRange * fresnel);

    // Deliberately NOT calling averApplyFog -- see PSWater's identical comment in WaterShaders.hpp:
    // this is a blended surface sitting in front of pixels the opaque forward pass already fogged,
    // and fogging it a second time would double-apply the same atmosphere to the one surface that is,
    // itself, meant to read as the atmosphere's reflective boundary.
    return float4(colorLinear, alpha);
}
)";

} // namespace aver::fluids
