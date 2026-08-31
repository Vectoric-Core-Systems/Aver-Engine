
// Feature-owned frame constants, at the register RHIResources.hpp reserves for a render feature.
#define AVER_SHADOW_CASCADES 4

// register(AVER_CB_JOIN(b, AVER_FEATURE_FRAME_CB)), not a literal b4. rhi::
// kFeatureFrameConstantRegister is the one definition and shaderConstantsHlsl() emits it as
// that #define; ClusterFrameCB in shared_prelude.hlsl already binds this way. A literal here
// keeps compiling against the OLD slot the day that constant is renumbered, while the C++
// binds the new one -- and its static_assert exists precisely to make renumbering deliberate.
cbuffer VoxiFrame : register(AVER_CB_JOIN(b, AVER_FEATURE_FRAME_CB)) {
    float4   gVoxelOrigin; // xyz = volume min corner, w = 1/volumeWorldSize
    float4   gVoxelParams; // x = resolution, y = intensity, z = maxDistance, w = enabled|debug<<1
    // One per cascade, tightest first: world space into that cascade's own [-1,1] clip box.
    float4x4 gCascadeViewProj[AVER_SHADOW_CASCADES];
    // x = the RADIUS at which this cascade stops applying, y = its normal-offset bias in world units
    float4   gCascadeSplit[AVER_SHADOW_CASCADES];
    float4   gShadowParams; // x = 1/atlasSize, y = enabled, z = accel structure built, w = cascades
    float4   gShadowDraw;   // x = the cascade the depth-only pass is filling right now
    // x = tan of the sun's ANGULAR RADIUS, which is what sets how fast a shadow edge softens;
    // y = occlusion rays per pixel; z = ray bias in world units at the near plane;
    // w = 1 once the flat geometry table is ready for a reflection ray's hit lookup.
    float4   gRtParams;
    // x = 1 while t6/u2 (gRtShadowHist / gRtShadowHistOut) are bound to real textures this frame;
    // y = 1 once gRtShadowHist ALSO holds a real previous frame (0 right after creation/resize);
    // z = the current frame index, a pure per-frame count, never wall-clock;
    // w = the pixels-per-ray tile edge as its BIT COUNT (0 = no tiling, every pixel traces).
    float4   gRtHistParams;
    // LAST frame's camera view-projection, for reprojecting a pixel's world position into
    // gRtShadowHist. Only meaningful while gRtHistParams.y is set.
    float4x4 gPrevViewProj;
    // LAST frame's scene viewport rect (x, y, w, h in target pixels): the reprojected NDC lands
    // here, not at [0,1] of the whole history texture -- the editor docks the 3D view in a sub-rect
    // of the backbuffer. Same validity as gPrevViewProj.
    float4   gSceneViewport;
    // THIS frame's scene viewport rect, same (x, y, w, h) in target pixels. Paired with gViewProj,
    // where gSceneViewport above is paired with gPrevViewProj. w == 0 means the device had none.
    float4   gSceneViewportCur;
    // x = 1 when the eye is inside a blended single-sided volume, y = that medium's ior.
    // Computed once per frame on the CPU -- a pixel cannot know whether its own volume encloses the
    // camera. See VoxiRenderer's own comment for why a loose bounding-sphere test is safe here.
    float4   gCameraMedium;
    // The GI-ONLY shadow map's light view-projection: one box fitted to the GI VOLUME, not to the
    // camera. Read only by giShadowFactor (PSVoxel); the cascades above stay camera-fitted and are
    // what PSMainVoxi samples.
    float4x4 gGiShadowViewProj;
    // x = 1/kGiShadowSize, y = 1 once the GI-only map is usable (0 = fall back to unshadowed
    // indirect), z = normal-offset bias in world units, w unused.
    float4   gGiShadowParams;
    // The SPATIAL shadow denoiser. x = filter radius in pixels (0 = off); y = how much of the
    // filtered value to take (0 = none, so the taps still run and the result is discarded --
    // that is the cost-measurement configuration, and lerp(v, f, 0) is v exactly for any finite
    // f); z and w unused. RADIUS LIVES IN A CONSTANT, not a #define, so the tap loop is dynamic
    // and cannot be unrolled away when the host asks for zero taps -- a compile-time 0 would
    // measure nothing and report it as free.
    float4   gRtDenoiseParams;
    float4   gPtBounceParams;
    // x = total cones the diffuse gather traces, including the axial one. y/z/w unused.
    float4   gGiParams;
};

// ---- Voxi: voxel cone traced GI ----
RWTexture3D<float4> gVoxelUAV : register(u0);
Texture3D<float4>   gVoxelTex : register(t0);
SamplerState        gVoxelSamp : register(s0);

// Injection accumulator: channel k of voxel c lives at (c.x * 4 + k, c.y, c.z), k == 3 being the
// fragment count. R32_UINT is the only typed format D3D12 guarantees UAV atomics on.
RWTexture3D<uint> gVoxelAccum : register(u1);

// Fixed-point scale radiance is multiplied by before accumulation and divided by in CSResolve.
#define AVER_VOX_FIXED 16384.0
#define AVER_VOX_MAXRAD 16.0

// The roughness below which a reflective surface is treated as a MIRROR: no cone, no temporal
// history, no spatial filter. See rtReflectionTemporal's own comment for why all three must be
// derived from this one number rather than each choosing its own threshold.
#define AVER_REFL_MIRROR_ROUGH 0.1

// TLAS instance-mask lanes. MUST MATCH kRtMaskOpaque/kRtMaskTranslucent in VoxiRenderer.cpp -- there
// is no shared-source mechanism between C++ and HLSL anywhere in this engine, so a value changed on
// one side and not the other is an image-only bug with no build error, exactly like the cbuffer
// mirrors this file already warns about.
//
// WHY EVERY RAY BUT THE SHADOW RAY ASKS FOR OPAQUE ONLY. Translucent panes are now in the structure,
// marked FORCE_NON_OPAQUE. A RayQuery that meets a non-opaque candidate does NOT commit it; Proceed()
// returns true and hands it to the shader to decide. Every traversal below except rtShadow calls
// Proceed() exactly ONCE and reads CommittedStatus -- correct while the structure held only opaque
// geometry, and quietly wrong now: traversal would stop AT the pane with an opaque wall behind it
// still unvisited, so the wall would vanish from that ray's answer. Narrowing the mask means those
// rays never traverse the pane at all, so their single-Proceed stays exactly as valid as it was.
#define AVER_RT_MASK_OPAQUE      0x01
#define AVER_RT_MASK_TRANSLUCENT 0x02
#define AVER_RT_MASK_ALL         0xFF

// Directional shadow map. Core feature level 11_0, so it works on every DX12 GPU.
Texture2D<float>          gShadowTex  : register(t1);
SamplerComparisonState    gShadowSamp : register(s1);

// The GI-only shadow map. OUTSIDE the AVER_RT guard below on purpose: its only reader is PSVoxel,
// which is compiled without ray tracing, so a declaration inside the guard would vanish exactly
// where it is needed. Shares gShadowSamp -- same comparison state, different texture.
Texture2D<float>          gGiShadowTex : register(t8);
// THE OPAQUE SCENE, COPIED BEFORE TRANSLUCENCY REPLAYS -- see IDevice::sceneColorBackdropTexture.
// It exists so a blended surface can tint what is behind it PER CHANNEL. Hardware blending gives one
// scalar (1 - src.a) for the destination, and volume absorption is per-channel by definition, so
// without this a pane of glass can only get darker with depth, never greener.
//
// MAY BE NULL-FILLED: before the first resize, under MSAA (the copy is invalid from a multisampled
// target), or on a backend that does not implement it. averBlendBackdropValid() below is how a
// caller asks, and every use falls back to the scalar composite when it says no.
Texture2D<float4>         gBlendBackdrop : register(t10);

#if AVER_RT
// DXR 1.1 inline ray tracing: traced from the pixel shader, no state objects or binding tables.
RaytracingAccelerationStructure gScene : register(t2);

// The flat geometry a reflection ray reads after it hits something. Four descriptors for the whole
// scene rather than one per mesh or per material, because this RHI uses explicit descriptor tables
// and not bindless: dynamic indexing INTO one bound resource (a StructuredBuffer read at a runtime
// index, below) is a Tier 1 feature everywhere -- indexing into a descriptor HEAP, which none of
// these are, is the only thing "bindless" actually means in this codebase.
struct RtVertex   { float3 pos; float3 nrm; float2 uv; };
struct RtInstance { float4x4 objectToWorld; uint firstIndex; uint firstVertex; float3 albedo;
                    float metallic; float roughness; uint materialIndex; };
StructuredBuffer<RtVertex>   gRtVerts     : register(t3);
StructuredBuffer<uint>       gRtIndices   : register(t4);
StructuredBuffer<RtInstance> gRtInstances : register(t5);

// ---- per-material data for a ray hit, keyed by RtInstance::materialIndex ----
//
// WHAT materialIndex REPLACES: `pad`, a u32 nothing on either side of this ABI ever read (grep
// confirmed zero references, in this file or its C++ mirror, before this change). A ray hit
// therefore had no per-material data reachable at all -- see the comment this one replaces, just
// above PSRayDriven's AverSurface construction, for what that cost. Repurposing an already-unread
// field is why this costs zero bytes: RtInstance is still 96 bytes, the C++-side static_assert on
// that size (VoxiRenderer.hpp) is unchanged, and every existing ray-driven pixel that never once
// read `pad` is unaffected by it meaning something now.
//
// RtMaterial MIRRORS pbr::MaterialConstants (MaterialGpu.hpp) FIELD FOR FIELD, in the SAME ORDER,
// the identical discipline cbuffer AverMaterial above (PbrShaders.cpp) already follows for the
// raster path's per-draw constant buffer -- see that struct's own comment for why grouping every
// four floats needs no explicit HLSL padding to land the C++ struct's 96 bytes exactly, and for why
// a mismatched order is what makes every field after the mismatch read a neighbour's bytes rather
// than fail to compile. Fields this pass never reads -- now only graphId and ior; emissive, the
// second material layer and transmission ARE read (see PSRayDriven's bindless block, s.emissive and
// the layer-1 blend, and the transmission read further down) and this list said otherwise for
// longer than it was true -- are still declared, in order, for exactly that reason: HLSL has no
// partial StructuredBuffer element, so leaving any of them out would silently misalign every field
// that follows it, not just drop the one that was skipped.
#ifdef AVER_RT_BINDLESS
// THE RAY PATH'S TEXTURE ARRAY, in register space 1 so it cannot collide with any t-register the
// two ordinary descriptor tables, the mesh geometry SRVs or the instanced world matrices already
// claim in space 0. The size must match PipelineLayout::bindlessTextureCount exactly -- the root
// signature declares a range of that length and a shader declaring more would read past it.
//
// A fixed size rather than an unbounded array: a fixed range serialises under root signature
// version 1.0, which is what this backend builds.
//
// THE LENGTH COMES FROM C++, not from a literal here. It must equal the root signature's declared
// range exactly -- a shader declaring more would index past what the table actually reserved, which
// is the out-of-bounds descriptor read this whole design is careful about. Same reason the register
// numbers in the shared prelude are emitted rather than written: two copies of a number drift.
#ifndef AVER_RT_TEX_CAPACITY
#error "AVER_RT_TEX_CAPACITY must be defined by the pipeline that declares the bindless table"
#endif
Texture2D gRtTextures[AVER_RT_TEX_CAPACITY] : register(t0, space1);
#define AVER_TEX_UNBOUND 0xFFFFFFFFu

// Ray-differential mip selection. 1 = SampleGrad from a footprint this pass derives; 0 = the mip-0
// SampleLevel the textured path shipped with. See averRtUvGrad for where the footprint comes from.
#ifndef AVER_RT_SAMPLEGRAD
#define AVER_RT_SAMPLEGRAD 1
#endif

#endif

struct RtMaterial {
    float4 baseColorFactor;   // rgb LINEAR already -- see MaterialConstants::baseColorFactor's own
                               // comment; no srgbToLin needed on the read side, unlike the raster
                               // path's separate per-draw gBaseColor (PbrShaders.cpp), which is not
                               // this and is not read here.
    float3 emissiveFactor;
    float  metallicFactor;
    float  roughnessFactor;
    float  normalScale;
    float  occlusionStrength;
    float  alphaCutoff;
    uint   flags;
    float  reflectance;
    float  f90;
    float  uvTilesPerCm;
    float  slopeBlendLo;
    float  slopeBlendHi;
    float  layer1UvScale;
    uint   graphId;
    float  ior;
    float  transmission;
    // These two mirror MaterialConstants::subsurfaceWeight/subsurfaceRadius, which spent the
    // _pad0/_pad1 this used to declare. Same order, same offsets: the struct still ends at 96 bytes.
    float  subsurfaceWeight;
    float  subsurfaceRadius;
    // The coat row, same order as MaterialConstants and as the cbuffer in material_prelude.hlsl.
    // THREE hand-maintained copies of one struct, with only the byte count checked -- see
    // tests/formats/src/MaterialTest.cpp for the field-layout check that now guards the order.
    float  coatWeight;
    float  coatRoughness;
    float  coatF0;
    float  _coatPad;

    // MIRRORS MaterialConstants::texIndex, in TextureSlot order: BaseColor, MetalRough, Normal,
    // Occlusion, Emissive, Layer1BaseColor, Layer1MetalRough, Layer1Normal. 0xFFFFFFFF is "no
    // texture in this slot" -- not 0, which is a real index. Unread until the bindless table lands.
    uint   texIndex[8];

    // Volume absorption, mirroring MaterialConstants::attenuationColor/attenuationDistance -- the row
    // that took the block from 144 to 160. Same order and same offsets as the C++ struct and as
    // `cbuffer AverMaterial` in material_prelude.hlsl; tests/formats/src/MaterialTest.cpp asserts the
    // offsets so a field moved in one copy and not the other two fails the suite rather than shading
    // a material with its neighbour's bytes.
    float3 attenuationColor;
    float  attenuationDistance;
};

#ifdef AVER_RT_BINDLESS
// One material map at a ray hit, or `fallback` where the material bound nothing.
//
// SampleLevel, NEVER Sample, and this is not a preference. A pixel shader's implicit derivatives
// describe how the SCREEN coordinate changes between neighbouring pixels; in a fullscreen ray pass
// the neighbour may have hit a different triangle, a different object, or nothing. The derivative is
// meaningless there and the mip it picks is garbage at every silhouette. Mip 0 aliases in the
// distance -- wrong, but wrong PREDICTABLY -- and ray-differential SampleGrad is a later stage.
//
// NonUniformResourceIndex because neighbouring pixels genuinely hit different materials. Without it
// the hardware may broadcast one lane's index across the wave, and every pixel in it samples one
// material's texture.
// ---- AVER_RD_ABLATE: A MEASUREMENT SWITCH, NOT A FEATURE ----------------------------------------
//
// WHY THIS EXISTS. The ray-driven primary path costs about 6.7 ms of a 14.55 ms frame on PTTest --
// measured, by taking the same camera through the raster path (7.82 ms) with ray tracing left fully
// on in both. It is strongly pixel-bound: quarter the pixels and it drops to 9.00 ms, while raster
// barely moves. But NOTHING said which term inside this shader spends it, and every RT quality dial
// is inert against it (--rt-rays 4/2/1 gives 14.49/14.46/14.44, and --rt 2 is SLOWER than --rt 4).
//
// YOU CANNOT TIMESTAMP INSIDE A PIXEL SHADER. GPU spans bracket draws, not terms, so the only way to
// attribute cost within one shader is to remove a term and re-measure. Each value below neutralises
// exactly one, keeping the shader compiling and the surrounding code identical, so the difference
// between two runs is that term and nothing else.
//
// THE OUTPUT IS WRONG ON PURPOSE for every non-zero value. This is not a quality ladder and must
// never be wired to one: an ablated frame is a broken frame that happens to be timeable. 0 is the
// only value that renders correctly, and it is the default in every build that does not ask.
//
// HOW TO READ THE RESULT: the terms should roughly ADD UP to the gap between the normal frame and
// the raster floor. If they do not -- if ablating everything does not approach 7.82 ms -- then the
// ablation is not measuring what it claims and the numbers are void. Check that before believing
// any single line of the table.
#ifndef AVER_RD_ABLATE
#define AVER_RD_ABLATE 0
#endif
#define AVER_RD_ABL_NONE    0
#define AVER_RD_ABL_SHADOW  1   // the sun-visibility ray (rtShadowTemporal)
#define AVER_RD_ABL_GI      2   // the diffuse cone gather (coneTracedIndirect)
#define AVER_RD_ABL_REFL    3   // the mirror ray (rtReflectionTemporal)
#define AVER_RD_ABL_SKY     4   // the atmosphere march (skyColor) in the reflection branch
#define AVER_RD_ABL_TEX     5   // material texture sampling
// ALL = shadow + GI + reflection + sky. NOT textures, and that exclusion is the fix to a method
// check this harness previously FAILED. Ablating textures makes averRtSampleSlot return its
// fallback, so metalRough reads 1.0 and the surface's roughness jumps to the material factor --
// past the `s.rough <= 0.75` gate on the reflection branch. Mode 5 therefore does not merely remove
// texture sampling, it REROUTES the shader onto the voxel-cone fallback, which is why combining it
// with the others made the frame SLOWER than removing the sky alone and why the deltas refused to
// add up. A term that moves a branch condition cannot be summed with terms that do not.
#define AVER_RD_ABL_ALL     6   // shadow + GI + reflection + sky -- the method check
// 7 IS NOT LIKE THE OTHERS: it does not remove a term, it restores ACCEPT_FIRST_HIT_AND_END_SEARCH
// on the sun-shadow ray. That flag was deliberately dropped so a pane of glass could ATTENUATE a
// shadow instead of stopping it, and the shadow ray's own comment asks for the cost of that decision
// to be measured before anyone assumes it is small. This is that stopwatch. It KNOWINGLY BREAKS
// tinted shadows through glass -- a pane becomes a wall again -- so like every other non-zero value
// here it renders a deliberately wrong frame and must never be wired to a quality tier.
#define AVER_RD_ABL_SHADOW_FIRSTHIT 7

float4 averRtSampleSlot(RtMaterial mat, uint slot, float2 uv, float2 gx, float2 gy, float4 fallback) {
    const uint idx = mat.texIndex[slot];
    if (idx == AVER_TEX_UNBOUND) return fallback;
#if AVER_RD_ABLATE == AVER_RD_ABL_TEX
    return fallback;   // ablated: the slot is bound, but nothing is sampled
#endif
#if AVER_RT_SAMPLEGRAD
    // SampleGrad with a footprint this pass DERIVED, rather than SampleLevel(0). Zero gradients
    // degrade to mip 0 exactly, so a degenerate triangle or a material whose gradient could not be
    // solved lands on the old behaviour rather than on something undefined.
    return gRtTextures[NonUniformResourceIndex(idx)].SampleGrad(gMaterialSampler, uv, gx, gy);
#else
    return gRtTextures[NonUniformResourceIndex(idx)].SampleLevel(gMaterialSampler, uv, 0);
#endif
}

// The UV-space footprint of one pixel's primary ray, for SampleGrad.
//
// WHY THIS IS NOT ddx(uv)/ddy(uv). A pixel shader's implicit derivatives describe the SCREEN
// coordinate; in a fullscreen ray pass the neighbouring lane may have hit a different triangle, a
// different object, or nothing, so those derivatives are meaningless here. What IS available is
// rdRayDx/rdRayDy -- the neighbouring pixels' own primary rays, reconstructed analytically from
// gInvViewProj and scaled by this ray's hitT, already built in this shader for the shadow disc.
// That is a real world-space footprint; this turns it into a UV-space one.
void averRtUvGrad(RtMaterial mat, RtInstance inst, float3 N,
                  float3 p0, float3 p1, float3 p2,
                  float2 t0, float2 t1, float2 t2,
                  float3 dx, float3 dy,
                  out float2 gx, out float2 gy) {
    gx = 0.0;
    gy = 0.0;

    if (mat.flags & AVER_MAT_WORLD_UV) {
        // A WORLD-ALIGNED MATERIAL'S GRADIENT LIVES IN A DIFFERENT SPACE, and using the mesh-UV
        // Jacobian for it would compute a perfectly precise gradient of the wrong function. Its UV
        // is a planar projection of world position, so the differential is just that same projection
        // of the world delta -- no Jacobian to invert at all. Same axes and dominant-axis choice as
        // averRtSurfaceUV, or the gradient would describe a different plane from the sample.
        const float3 ax = normalize(inst.objectToWorld[0].xyz);
        const float3 ay = normalize(inst.objectToWorld[1].xyz);
        const float3 az = normalize(inst.objectToWorld[2].xyz);
        const float3 a  = abs(float3(dot(N, ax), dot(N, ay), dot(N, az)));
        if (a.z >= a.x && a.z >= a.y) {
            gx = float2(dot(dx, ax), dot(dx, ay));
            gy = float2(dot(dy, ax), dot(dy, ay));
        } else if (a.x >= a.y) {
            gx = float2(dot(dx, ay), dot(dx, az));
            gy = float2(dot(dy, ay), dot(dy, az));
        } else {
            gx = float2(dot(dx, ax), dot(dx, az));
            gy = float2(dot(dy, ax), dot(dy, az));
        }
        gx *= mat.uvTilesPerCm;
        gy *= mat.uvTilesPerCm;
        return;
    }

    // Mesh UV: invert the triangle's own position-to-UV map. T and B here are dP/du and dP/dv,
    // deliberately UNNORMALISED -- their lengths are centimetres per UV unit, which is exactly the
    // scale being inverted.
    const float3 e1 = p1 - p0, e2 = p2 - p0;
    const float2 d1 = t1 - t0, d2 = t2 - t0;
    const float  r  = d1.x * d2.y - d2.x * d1.y;
    if (abs(r) < 1e-12) return;   // degenerate UVs: no footprint, so mip 0, as before

    const float3 T = mul(float4((e1 * d2.y - e2 * d1.y) / r, 0.0), inst.objectToWorld).xyz;
    const float3 B = mul(float4((e2 * d1.x - e1 * d2.x) / r, 0.0), inst.objectToWorld).xyz;

    // Dual basis of (T, B, N): the rows of the inverse. A world delta in the tangent plane projects
    // onto these to give its (du, dv).
    const float3 cbn = cross(B, N);
    const float3 cnt = cross(N, T);
    const float  det = dot(T, cbn);
    if (abs(det) < 1e-12) return;   // T parallel to B: no usable frame

    gx = float2(dot(cbn, dx), dot(cnt, dx)) / det;
    gy = float2(dot(cbn, dy), dot(cnt, dy)) / det;
}

// The texture coordinate a ray hit is sampled at: the mesh's own UV, or a planar projection when
// the material asks for world-aligned UV.
//
// A RAY HIT NEEDS THIS AS MUCH AS A FRAGMENT DOES, and the first version of this path did not have
// it -- it sampled every material at the mesh UV. On a mesh whose UVs are unrelated to its intended
// projection (PTTest's floor and concrete both set worlduv=1 and rely entirely on it) that samples
// the texture at essentially arbitrary coordinates, and the surface reads as noise rather than as
// the material. It went unnoticed because the scene it was first measured on, ElectricDreams
// terrain, does not set the flag.
//
// Mirrors averSurfaceUV in material_prelude.hlsl exactly, with inst.objectToWorld standing in for
// gWorld -- the same rows, the same dominant-axis choice, the same tiles-per-centimetre scale.
float2 averRtSurfaceUV(RtMaterial mat, RtInstance inst, float3 wpos, float3 N, float2 meshUV) {
    if (!(mat.flags & AVER_MAT_WORLD_UV)) return meshUV;
    const float3 ax = normalize(inst.objectToWorld[0].xyz);
    const float3 ay = normalize(inst.objectToWorld[1].xyz);
    const float3 az = normalize(inst.objectToWorld[2].xyz);
    const float3 d  = wpos - inst.objectToWorld[3].xyz;
    const float3 op = float3(dot(d, ax), dot(d, ay), dot(d, az));
    const float3 on = float3(dot(N, ax), dot(N, ay), dot(N, az));
    const float3 a  = abs(on);
    const float2 pp = (a.z >= a.x && a.z >= a.y) ? op.xy
                    : ((a.x >= a.y) ? op.yz : op.xz);
    return pp * mat.uvTilesPerCm;
}

// A tangent frame for a ray hit, solved from the triangle's own positions and UVs.
//
// THE RASTER PATH CANNOT BE REUSED HERE. averPerturbNormal solves its frame from ddx/ddy of world
// position and UV, which is exactly the screen-derivative trick that is invalid at a ray hit. And
// there is no vertex tangent to fall back on: RtVertex is pos+normal+uv, and no tangent stream
// exists anywhere in this engine -- docs/formats/FORMAT_SPECS.md claims .ocmesh carries a MikkTSpace
// QTangent, but normalToQTangent() takes only a normal and computes a shortest-arc rotation, so the
// stream has never held a real tangent.
//
// So it is derived per triangle, which is the textbook solve and is CONSTANT across the face --
// slightly flatter than a per-vertex interpolated frame on curved surfaces, and exact on the flat
// ones normal maps are mostly used on.
float3 averRtPerturbNormal(RtMaterial mat, RtInstance inst, float3 N, float3 nTS,
                           float3 p0, float3 p1, float3 p2,
                           float2 t0, float2 t1, float2 t2) {
    float3 T, B;

    if (mat.flags & AVER_MAT_WORLD_UV) {
        // THE FRAME MUST MATCH THE PARAMETRISATION IT SAMPLES. A world-aligned material's UV comes
        // from a planar projection onto two of the object's own axes, and has nothing to do with the
        // mesh's UVs -- so solving a tangent from those UVs would give a frame rotated arbitrarily
        // against the texture it is about to apply, which is a normal map lighting from the wrong
        // direction rather than an obvious failure.
        //
        // Same axes and the same dominant-axis choice averRtSurfaceUV makes, so the two cannot
        // disagree about which plane this surface is projected onto.
        const float3 ax = normalize(inst.objectToWorld[0].xyz);
        const float3 ay = normalize(inst.objectToWorld[1].xyz);
        const float3 az = normalize(inst.objectToWorld[2].xyz);
        const float3 a  = abs(float3(dot(N, ax), dot(N, ay), dot(N, az)));
        if (a.z >= a.x && a.z >= a.y) { T = ax; B = ay; }        // uv = op.xy
        else if (a.x >= a.y)          { T = ay; B = az; }        // uv = op.yz
        else                          { T = ax; B = az; }        // uv = op.xz
    } else {
        const float3 e1 = p1 - p0, e2 = p2 - p0;
        const float2 d1 = t1 - t0, d2 = t2 - t0;
        const float  r  = d1.x * d2.y - d2.x * d1.y;

        // DEGENERATE UVs ARE COMMON, not exotic: a face with all three UVs equal (an untextured or
        // collapsed shell) gives r == 0, and dividing by it produces NaN that propagates through the
        // whole shade and paints a pixel that looks like a lighting bug. Mirrors averPerturbNormal's
        // own `if (m <= 0.0) return N;`.
        if (abs(r) < 1e-12) return N;

        const float3 tObj = (e1 * d2.y - e2 * d1.y) / r;
        const float3 bObj = (e2 * d1.x - e1 * d2.x) / r;
        // Row-vector multiply, the same convention the shading normal uses.
        T = mul(float4(tObj, 0.0), inst.objectToWorld).xyz;
        B = mul(float4(bObj, 0.0), inst.objectToWorld).xyz;
    }

    if (dot(T, T) <= 0.0) return N;

    // Gram-Schmidt against the SHADING normal, so the frame stays orthonormal where the interpolated
    // normal has been bent away from the triangle's own plane.
    T = normalize(T - N * dot(N, T));
    if (!(dot(T, T) > 0.0)) return N;

    // Handedness taken from the solved bitangent rather than assumed: a mirrored UV shell has the
    // opposite one, and guessing lights half of a symmetrical model from the wrong side.
    const float3 Bo = cross(N, T);
    B = Bo * (dot(Bo, B) < 0.0 ? -1.0 : 1.0);

    return normalize(T * nTS.x + B * nTS.y + N * nTS.z);
}
#endif
// SLOT t9 IS A GUESS, LOUDLY. This is table 0's next free SRV slot after t8 -- kGiSrvCount
// (VoxiGiShaders.hpp), currently 9, would need to become 10 to declare it here, and table 1 (the
// material TEXTURE table) is BASED on that count and auto-rebases from t9 to t10 the moment it does
// (see giLayout()'s own comment on exactly that mechanism). The buffer itself follows the t3/t4/t5
// recipe exactly: one more setSrvBuffer call, dense over whatever a frame's draws actually used,
// built the same way gRtInstances already is. THE C++ SIDE OF THIS -- widening kGiSrvCount,
// giTableKinds, and building the per-frame buffer this reads -- is owned by a different agent
// editing VoxiRenderer.hpp/.cpp concurrently with this file, per this task's own brief; if their
// actual slot differs from t9, THIS is the line to move, not the pattern around it.
StructuredBuffer<RtMaterial> gRtMaterials : register(t9);

// The ray-traced sun-shadow history: LAST frame's resolved (visibility, depth) (t6, read this
// frame) and THIS frame's (u2, written this frame, becomes t6 next frame). Ping-ponged on the C++
// side -- VoxiRenderer.hpp rtShadowHist_ -- never the same texture in the same frame. x =
// visibility in [0,1], y = linear (view-space) depth in centimetres, for rtReprojectHistory's
// disocclusion test.
Texture2D<float2>   gRtShadowHist    : register(t6);
RWTexture2D<float2> gRtShadowHistOut : register(u2);

// The ray-traced reflection history: LAST frame's resolved (colour, depth) (t7, read this frame)
// and THIS frame's (u3, written this frame, becomes t7 next frame). Same ping-pong as the shadow
// history above, its own pair of textures. rgb = the reflection's own shaded colour, a = linear
// depth of the hit, OR NEGATIVE meaning the ray missed -- see rtReflectionTemporal.
Texture2D<float4>   gRtReflHist    : register(t7);
RWTexture2D<float4> gRtReflHistOut : register(u3);

// ---- LAST FRAME's normal-roughness G-buffer, read-only, for the spatial denoisers' crease term ----
//
// NOT THE SAME RESOURCE AS SV_TARGET3 below (GBufferOut::normalRoughness / RayDrivenGBufferOut::
// normalRoughness). That target is THIS frame's, written by the very same pass that would want to
// read it back for a filter: D3D12Device binds it as an RTV for the identical draw whose pixel
// shader would need it as an SRV -- one resource cannot be bound RTV and SRV in the same draw, and
// even if a backend allowed it, a pixel shader invocation cannot see a neighbouring pixel's
// not-yet-written output within its own rasterisation call. Same ordering hazard rtShadowHist_/
// rtReflHist_ solve by ping-ponging two textures instead of reading and writing one; this is meant
// to be that same solution applied to the normal-roughness channel, not a second write path -- the
// EXISTING SV_TARGET3 write is the "write" half already, this is only the "read last frame's copy"
// half.
//
// GATED BEHIND ITS OWN DEFINE, SEPARATELY FROM AVER_GBUFFER, and that is deliberate. AVER_GBUFFER on
// its own only proves the SINGLE, non-ping-ponged target exists (see that feature's own module
// comment, below: "THIS SLICE ONLY WRITES THE TARGETS"), not that a second, double-buffered copy of
// it does. Reading a register nothing bound is a silent null-descriptor read on a real GPU and a
// validation-layer failure on Vulkan's -- so this stays off (hence unreferenced, hence not a
// register the root signature has to account for) until the ping-ponged pair and its swap
// (VoxiRenderer.hpp/.cpp, D3D12Device.cpp, VulkanDevice.cpp -- none of them this file) actually
// exist, exactly the discipline AVER_GBUFFER itself already established one layer up in this file:
// undefined (hence 0 under HLSL's #if) everywhere except a variant the host explicitly compiles with
// "AVER_GBUFFER_HISTORY=1" appended to its define string, on top of "AVER_GBUFFER=1" -- this is a
// history OF that G-buffer channel, and asking for one without the other is a host-side contract
// this file does not itself enforce.
//
// SLOT t10 IS A GUESS, LOUDLY, for the identical reason gRtMaterials' t9 above is: table 0's next
// free SRV after this file's other new addition, with table 1 auto-rebasing from t10 to t11 the
// same way it already rebases from t9. Nothing here checks that guess against whatever slot the
// ping-pong actually lands at once it is built -- move this line to match when it does.
//
// ASSUMED THE SAME RESOLUTION AS gRtShadowHist (both are sized against the scene render target),
// which is why the tap loops below reuse gRtShadowHist's own GetDimensions()/bounds check rather
// than querying this texture's dimensions separately -- unverified from this file, since the actual
// allocation is C++-side.
#if AVER_GBUFFER_HISTORY
Texture2D<float4> gGBufNormalHist : register(t10);
#endif

// A hash of the pixel, for rotating each pixel's sample pattern.
//
// SPATIAL ONLY, and that is a hard requirement rather than a simplification: the gate oracle
// compares 8-bit probe codes BIT-EXACTLY, and two of its gates deliberately sample a penumbra. A
// seed that varied per frame would make those a coin flip run to run, and the engine's whole
// verification story rests on flat-neighbourhood probes being reproducible. THIS STAYS TRUE of
// rtHash itself even now that rtShadow accepts a separate, explicit `frameJitter` on top of it --
// see rtShadow's own comment. Nothing IN rtHash depends on the frame; a caller that wants frame
// variance adds it outside, deliberately, where it is a choice made once per call site rather than
// a property of the hash every caller inherits.
float rtHash(float2 p) {
    float3 q = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
    q += dot(q, q.yzx + 33.33);
    return frac((q.x + q.y) * q.z);
}

// The radical inverse of `i` in base 2 -- its bits reflected about the binary point -- in [0,1).
//
// THIS IS WHAT MAKES THE SAMPLE SEQUENCE NESTED, and the radius it replaced was not. `sqrt((k+0.5)/n)`
// puts sample k at a radius that depends on the TOTAL ray count, so asking for more rays MOVES every
// sample rather than adding to them: n=2 and n=4 are then two unrelated estimators of the same
// integral, each its own oracle, and no comparison between two ray counts is a refinement of the
// first. phi(k) depends on k alone, so the first m samples of an n-sample set ARE the m-sample set --
// raising the count keeps every ray already traced and fills in between them. That is what lets a
// ray-count sweep be read as convergence, and what lets one baseline serve every count.
//
// EXACT ON EVERY ADAPTER, which the sun disc's other option -- a hash -- is not. reversebits is
// integer bit manipulation, the uint-to-float conversion is IEEE round-to-nearest, and 2^-32 is a
// power of two so the multiply is exact. The gate oracle compares nine configurations bit-exactly,
// WARP among them, and a divide by a per-call ray count was one more thing that did not have to be.
float rtRadicalInverse2(uint i) {
    return (float)reversebits(i) * 2.3283064365386963e-10;   // 1 / 2^32
}

// Sample `k` of the disc sequence the shadow loop walks, as a point in the unit disc. `ang0` turns
// the whole pattern by a per-pixel angle so neighbouring pixels do not share one set of directions.
//
// THE SIGNATURE IS THE PROPERTY. There is no ray count in it, and there cannot be one: a sample is a
// function of its index alone, which is what "nested" means and what the version this replaced --
// sqrt((k + 0.5) / n) -- could not say. Kept as its own function rather than inlined into the loop
// so that the claim is checkable in one place rather than argued about in a loop body.
//
// The golden angle around and the radical inverse outward, with sqrt to map the radial coordinate
// onto AREA rather than radius -- without it the samples crowd the centre and every estimate is
// biased toward the middle of the sun.
float2 rtDiscSample(uint k, float ang0) {
    float rad = sqrt(rtRadicalInverse2(k + 1));
    float a   = ang0 + (float)k * 2.39996323;
    return float2(cos(a), sin(a)) * rad;
}

// Traces occlusion rays toward the sun's DISC and returns the fraction that reached it: 0 fully
// shadowed, 1 fully lit, and everything between is a real penumbra.
//
// The single ray this replaced returned exactly 0.0 or 1.0, so a ray-traced shadow had a hard
// aliased edge while the cascade path beside it did a 3x3 comparison filter -- turning ray tracing
// ON made shadows look worse, which is the wrong way round. The sun is not a point: it subtends
// about half a degree, and that angle is what sets how fast an edge softens. gRtParams.x carries
// its tangent so the softening is the SUN's property rather than a tuned constant.
//
// The bias scales with distance from the camera. A fixed 0.02 cm offset is roughly 300 float ulp
// at 1000 cm from the origin and under 3 at 100000 cm, so distant geometry self-intersects and
// speckles -- acne that looks like flickering rather than like a bias problem.
// `dpx`/`dpy` are the receiver's screen-space footprint, PASSED IN rather than taken here.
//
// They used to be ddx/ddy(wpos) computed inside. That is correct for a primary surface, where wpos
// varies smoothly across the quad, and WRONG for a reflected hit: neighbouring pixels' rays land on
// different triangles metres apart, so the "footprint" becomes a wild vector and the shadow rays
// scatter across the scene. A derivative taken inside divergent flow is undefined in HLSL as well.
// A reflected caller passes zero and gets a point sample, which is what it wants.
// `rays` is explicit rather than read from the constant buffer, so a SECONDARY ray can ask for
// fewer than a primary one. A reflection is already an approximation -- one bounce, no roughness
// lobe -- and spending a full disc sweep on the shadow of something seen IN a reflection buys
// detail nobody can resolve. Primary shading still passes the full count.
//
// `frameJitter` is ADDED to the per-pixel rotation, and is NOT part of rtHash: rtHash itself stays
// exactly what its own comment says it must -- a pure function of the pixel, for the gate oracle's
// bit-exact single-frame probes. Every existing caller passes 0.0 here and is completely unaffected.
// rtShadowTemporal is the one caller that passes something else, and only when pixel tiling is
// actually on -- see its own comment for why a NONZERO, per-frame value is what makes tiling
// converge at all instead of repeating one sample forever.
float3 rtShadow(float3 wpos, float3 N, float3 L, float2 pixel, float3 dpx, float3 dpy, uint rays,
               float frameJitter) {
    const uint  n    = max(rays, 1u);
    const float tanR = max(gRtParams.x, 0.0);
    const float bias = max(gRtParams.z, 1e-4) * (1.0 + length(wpos - gCamPos.xyz) * 5e-4);

    // A frame around the light direction, to spread samples across the disc.
    float3 up = abs(L.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T  = normalize(cross(up, L));
    float3 B  = cross(L, T);

    // THE PIXEL'S OWN FOOTPRINT ON THE SURFACE, from the screen-space derivatives of the world
    // position. This is what actually fixes the jagged edge, and the sun's disc is not:
    //
    // the sun's angular RADIUS is about a quarter of a degree, so at contact distances the true
    // penumbra is far narrower than a pixel -- spreading rays across the disc alone gives the same
    // binary answer as one ray did, everywhere except a single edge pixel. Meanwhile the shadow
    // term is computed ONCE PER PIXEL while the geometry beside it is resolved at 8x MSAA, so the
    // shadow boundary stair-steps against smooth silhouettes. That mismatch is the visible defect.
    //
    // Jittering the ray ORIGIN across the footprint turns the per-pixel test into an area estimate,
    // which is antialiasing the shadow rather than blurring it: the result converges to the exact
    // fraction of the pixel that is occluded.
    const float ang0 = rtHash(pixel) * 6.2831853 + frameJitter;
    float3 vis = float3(0.0, 0.0, 0.0);

    [loop] for (uint k = 0; k < n; ++k) {
        // The sample the loop is at. NOTHING HERE DEPENDS ON n, which is the whole design: sample k
        // sits in the same place whatever the ray count, so raising the count refines the estimate
        // instead of replacing it with an unrelated one. The same rotated pattern serves both the
        // sun disc and the pixel footprint, so one sample covers both.
        float2 disc = rtDiscSample(k, ang0);

        float3 dir = normalize(L + (T * disc.x + B * disc.y) * tanR);
        // Half the footprint, so samples stay inside the pixel they are estimating.
        float3 org = wpos + (dpx * disc.x + dpy * disc.y) * 0.5;

        RayDesc r;
        // Offset along the NORMAL and along the ray. The normal alone leaves acne at grazing
        // angles, where the surface is nearly parallel to the ray and the offset barely separates
        // them.
        r.Origin    = org + N * bias + dir * bias;
        r.Direction = dir;
        r.TMin      = bias;
        r.TMax      = 100000.0;
        // NO ACCEPT_FIRST_HIT, and that removal is the feature.
        //
        // "Stop at the first thing you touch" is exactly right for a binary shadow and exactly wrong
        // for a transmissive one: a pane of glass IS the first thing touched, and ending there makes
        // it a wall. The ray now runs its own traversal, multiplying a running transmittance by each
        // translucent surface it crosses and stopping only at something genuinely solid.
        //
        // THE COST IS REAL AND WORTH STATING: without ACCEPT_FIRST_HIT the hardware can no longer
        // abandon traversal at the first intersection, so every shadow ray in the scene now walks
        // until it finds an opaque hit or leaves the structure -- including the overwhelming majority
        // that never meet a pane at all. Measure it before assuming it is small.
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
        // BOTH LANES: this is the one ray that wants to see translucent geometry.
#if AVER_RD_ABLATE == AVER_RD_ABL_SHADOW_FIRSTHIT
        // Measurement only -- see AVER_RD_ABL_SHADOW_FIRSTHIT. Opaque lane only and stop at the first
        // thing touched, which is what this ray did before transmissive shadows existed.
        q.TraceRayInline(gScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE,
                         AVER_RT_MASK_OPAQUE, r);
#else
        q.TraceRayInline(gScene, RAY_FLAG_NONE, AVER_RT_MASK_ALL, r);
#endif

        // ---- TWO MODELS, AND THE MATERIAL PICKS WHICH ----------------------------------------
        //
        // A material that authors a VOLUME (attenuationDistance > 0) is attenuated by Beer-Lambert
        // over the distance the light actually travelled inside it. One that does not keeps the
        // per-crossing surface rule, which is the correct answer for a thin sheet with no authored
        // medium rather than a compromise.
        //
        // WHY THE LOOP GATHERS INSTEAD OF MULTIPLYING AS IT GOES. A path length needs both ends of
        // the segment, and the exit candidate can arrive at any point in the walk, so there is
        // nowhere to apply a volume's contribution until the walk is over. Spans are collected here
        // and resolved below.
        //
        // PAIRED BY MIN/MAX t, NEVER BY ARRIVAL ORDER OR BY FACING. DXR does not specify that
        // non-opaque candidates arrive nearest-first, so "first is entry, second is exit" is unsound.
        // And CandidateTriangleFrontFace() would be worse than unsound here: winding is exactly what
        // broke volume absorption on the pool this morning -- the fluid box winds opposite to the
        // cube -- so a facing test is the one thing this must not be built on. min/max needs neither.
        //
        //   one hit   -> the ray STARTED INSIDE the medium, and t IS the distance out. This is the
        //                pool floor looking up at the sun through its own water.
        //   two hits  -> entered and exited; the span between them is the thickness. A pane.
        //   more      -> concave or self-overlapping geometry; the outer span is the honest estimate.
        // TWO SLOTS AS PLAIN SCALARS, NOT AN ARRAY, AND THAT IS A MEASURED DECISION.
        //
        // The first version of this held medIid/medTMin/medTMax/medHits as local arrays indexed by a
        // loop variable. HLSL cannot always keep a dynamically-indexed local array in registers, and
        // when it spills to scratch the cost lands on the pass that can least afford it: the sun
        // shadow is wave-bound, so losing occupancy costs far more than the arithmetic saves.
        // MEASURED: 7.96 ms -> 9.95 ms, a 25% frame regression for a water shadow. Unrolled into
        // named scalars it stays in registers.
        //
        // Two is what the scene actually needs -- glass over water -- and a third overlapping medium
        // falls through to the surface rule rather than being dropped, which is approximate but never
        // silently absent.
        uint  med0Iid = 0xffffffffu, med1Iid = 0xffffffffu;
        float med0Min = 0.0, med0Max = 0.0, med1Min = 0.0, med1Max = 0.0;
        uint  med0Hits = 0u, med1Hits = 0u;

        float3 through = float3(1, 1, 1);   // running transmittance along this ray
        // Bounded: a ray that somehow finds a great many translucent surfaces must not spin. Eight
        // crossings is far past the point where transmittance is visually zero anyway.
        [loop] for (uint step = 0; step < 8u && q.Proceed(); ++step) {
            if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) break;

            // The material of the thing we just passed through. This is what RtInstance::materialIndex
            // and the t9 material table were built for and, until now, never used by a shadow ray.
            const uint iid = q.CandidateInstanceID();
            const RtMaterial m = gRtMaterials[gRtInstances[iid].materialIndex];

            if ((m.flags & AVER_MAT_CAST_SHADOW) == 0) continue;   // casts nothing at all

            bool handled = false;
            if (m.attenuationDistance > 0.0) {
                const float ct = q.CandidateTriangleRayT();
                if (med0Iid == iid) {
                    med0Min = min(med0Min, ct); med0Max = max(med0Max, ct); ++med0Hits; handled = true;
                } else if (med1Iid == iid) {
                    med1Min = min(med1Min, ct); med1Max = max(med1Max, ct); ++med1Hits; handled = true;
                } else if (med0Hits == 0u) {
                    med0Iid = iid; med0Min = ct; med0Max = ct; med0Hits = 1u; handled = true;
                } else if (med1Hits == 0u) {
                    med1Iid = iid; med1Min = ct; med1Max = ct; med1Hits = 1u; handled = true;
                }
            }

            if (!handled) {
                // NO VOLUME (or no slot left): the per-crossing surface rule, the HLSL twin of
                // pbr::shadowTransmittance (Material.cpp). See that function's comment for why the
                // two are no longer identical in shape -- it has no ray and cannot know a distance.
                const float k = max(1.0 - m.baseColorFactor.a, saturate(m.transmission));
                through *= saturate(m.baseColorFactor.rgb) * k;

                // Once effectively nothing gets through, the surface is opaque for shadow purposes
                // and there is no point walking further. Only the surface path may take this
                // early-out: a volume's contribution has not been applied yet, so a walk carrying
                // one must run to the end or its medium is silently dropped.
                if (max(through.r, max(through.g, through.b)) < 0.01 && med0Hits == 0u) {
                    through = float3(0, 0, 0);
                    break;
                }
            }

            // NOT committed. Committing would end the traversal at this pane; leaving the candidate
            // uncommitted is what lets Proceed() carry on to whatever is behind it.
        }

        // Resolve the gathered spans, per channel, by the same Beer-Lambert the VIEW path uses.
        // averVolumeTransmittance is the one implementation both paths call and it reads no globals,
        // precisely so a ray hit can use it -- see its own comment. Calling it here is what makes the
        // light going DOWN through a medium agree with the light coming back UP through it, which is
        // the disagreement this whole change exists to remove.
        //
        //   one hit  -> the ray STARTED INSIDE the medium and t IS the distance out: the pool floor
        //               looking up at the sun through its own water.
        //   two+     -> entered and exited; the span between them is the thickness. A pane.
        if (med0Hits > 0u) {
            const RtMaterial m0 = gRtMaterials[gRtInstances[med0Iid].materialIndex];
            const float th0 = (med0Hits == 1u) ? med0Max : (med0Max - med0Min);
            through *= averVolumeTransmittance(m0.attenuationColor, m0.attenuationDistance, th0);
        }
        if (med1Hits > 0u) {
            const RtMaterial m1 = gRtMaterials[gRtInstances[med1Iid].materialIndex];
            const float th1 = (med1Hits == 1u) ? med1Max : (med1Max - med1Min);
            through *= averVolumeTransmittance(m1.attenuationColor, m1.attenuationDistance, th1);
        }

        // An opaque hit anywhere along the way blocks everything, whatever the panes in front said.
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) through = float3(0, 0, 0);

        // PER CHANNEL NOW. This used to reduce to luminance here because rtShadow's return type,
        // its call sites and the history were all single-channel -- so a shadow could be correctly
        // DARKENED by a medium but never take its COLOUR. Water lit a pool floor grey instead of
        // cyan. The reduction moved to rtShadowTemporal, which needs a scalar only for the history
        // it filters; see there for why the history did NOT have to grow to carry this.
        vis += through;
    }
    return vis / (float)n;
}

// Reprojects wpos through LAST frame's camera to sample the ray-traced shadow history there. False
// when the reprojection is not usable at all: off-screen, behind last frame's near plane, or a
// DISOCCLUSION -- the stored depth at the reprojected texel does not match what this world point
// should have looked like last frame (see the depth-comparison block below). In every false case
// `hist`/`velocityPx` are left untouched.
//
// `pixel` is this frame's own screen position; the reprojection's screen-space displacement from
// it is handed back as `velocityPx` so the caller can discount an otherwise-valid sample that has
// moved -- camera motion (or the receiver surface itself moving) pushes a static world point
// across texels between frames, which the depth test alone does not catch: it only tells apart
// "still the same surface" from "now looking at something else", not "the same surface, but I have
// slid along it since last frame."
//
// NDC -> LAST frame's VIEWPORT rect, not [0,1] of the whole texture: the editor docks the 3D view
// in a sub-rect of the backbuffer (gSceneViewport), and a plain ndc*0.5+0.5 implicitly assumes the
// viewport covers the entire render target, which lands every reprojection on the wrong texel
// whenever it does not. Caught by the shadow-rt / penumbra-rt gates -- moved by a full shade, not a
// rounding difference -- not shipped.
//
// NEAREST, not bilinear -- and this is the OPPOSITE of what an earlier version of this function
// concluded, for a reason specific to who calls it now. That version was read every frame by every
// pixel, all of them always in sync (freshly traced that same frame), so a bilinear tap blending in
// a neighbour was blending in something almost identical -- cheap insurance against a reprojected
// coordinate landing a hair off a texel centre. rtShadowTemporal's tiled path is the opposite
// situation on purpose: neighbouring pixels are DELIBERATELY out of sync, each mid-way through its
// own turn cycle, so a bilinear tap mixes in a neighbour that can be many frames stale and on the
// other side of a penumbra -- measured to converge to a stable but WRONG value (a 4x4 tile settled
// at 46,46,47 against a real 23,27,32, unmoved between 300 and 1500 frames, so this was not slow
// convergence). Nearest guarantees this pixel reads its OWN last write, which is what the schedule
// in rtShadowTemporal actually assumes.
bool rtReprojectHistory(float3 wpos, float2 pixel, out float hist, out float2 velocityPx) {
    hist = 0.0;
    velocityPx = 0.0;
    float4 clip = mul(float4(wpos, 1.0), gPrevViewProj);
    if (clip.w <= 1e-4) return false;
    float3 ndc = clip.xyz / clip.w;
    if (ndc.z < 0.0 || ndc.z > 1.0) return false;
    float texW, texH;
    gRtShadowHist.GetDimensions(texW, texH);
    float2 px = gSceneViewport.xy +
                float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * gSceneViewport.zw;
    // FLOOR, not round: `px` for a pixel that has not moved lands almost exactly on THAT pixel's own
    // centre (integer index + 0.5), which is precisely the .5 tie `round()` breaks inconsistently
    // (round-half-to-even) depending on whether the index is odd or even -- silently sending roughly
    // half of all pixels to their wrong neighbour instead of themselves. floor() of a centre at
    // index+0.5 is exactly `index`, matching how the WRITE side already indexes this same texture
    // (gRtShadowHistOut[uint2(pixel)] truncates SV_Position the same way). This is what a tiled
    // pixel schedule's "read my own last write" actually needs -- round() was measured to converge
    // to a stable but wrong value (penumbra-rt settled at 78,75,70 against a true ~23,27,32).
    int2 texel = int2(floor(px));
    if (any(texel < 0) || texel.x >= (int)texW || texel.y >= (int)texH) return false;

    const float2 stored = gRtShadowHist.Load(int3(texel, 0));   // x = visibility, y = linear depth
    // clip.w IS the expected depth at this reprojected point: the same value VSMain's o.pos.w would
    // carry for a vertex sitting at wpos (RHIShaders.cpp's VSMain computes o.pos = mul(wp,
    // gViewProj) the identical way), just through LAST frame's camera instead of this one. Comparing
    // it to what the history actually stored there catches a disocclusion a screen-position check
    // alone cannot: a silhouette edge can reproject to an already-populated texel while the surface
    // now visible through it sits at a completely different depth.
    const float tol = max(clip.w, stored.y) * 0.03 + 1.0;   // 3% relative, +1cm floor at grazing distances
    if (abs(clip.w - stored.y) > tol) return false;

    hist = stored.x;
    velocityPx = px - pixel;
    return true;
}

// The SPATIAL denoiser: average this pixel's shadow with its neighbours' from the history texture,
// weighted by how well each neighbour's surface agrees with this one's.
//
// WHY IT READS LAST FRAME'S TEXTURE AND WHY THAT IS FINE. t6 and u2 are different textures, ping-
// ponged per frame, and t6 rests in ShaderResource for the whole colour pass -- so an arbitrary
// neighbourhood read is a plain load of an immutable texture, needing no barrier and no reordering
// of anything. The neighbours are one frame old. That is a completely different proposition from
// the TEMPORAL path above, which reuses a value up to 2^(2*tileBits) frames old AS THE ANSWER:
// here the centre pixel always contributes its own freshly traced value, and a neighbour only ever
// adjusts the weighting. A neighbour that fails the plane test is DROPPED from the kernel, never
// substituted, so the worst case is that every neighbour is rejected and this returns the centre
// unchanged -- today's noisy-but-correct behaviour. The guide weights the blur; it never supplies
// the value.
//
// WHY AVERAGING NEIGHBOURS IS AN ESTIMATE RATHER THAN A BLUR. rtShadow jitters the ray ORIGIN
// across the pixel's own footprint (see its `org` line), so neighbouring pixels on one flat
// receiver are already sampling different points of the same surface. Their mean is a genuine area
// estimate of the same integral one pixel would need many rays to reach.
float rtShadowSpatial(float centre, float3 wpos, float3 N, float2 pixel, float curDepth) {
    const int radius = (int)gRtDenoiseParams.x;
    if (radius <= 0 || gRtHistParams.y < 0.5) return centre;

    float texW, texH;
    gRtShadowHist.GetDimensions(texW, texH);

    // GATHER AROUND WHERE THIS PIXEL WAS LAST FRAME, NOT AROUND WHERE IT IS NOW. gRtShadowHist is
    // last frame's texture; while the camera moves, the image has shifted inside it, so a
    // neighbourhood centred on THIS frame's coordinate samples a patch of a different part of the
    // scene. Measured before this was added: still, the filter landed within one code of the
    // sixteen-ray answer -- and under a six-degree wobble it drifted 12 to 32 codes darker,
    // increasing with radius, which is the signature of a kernel walking off its own surface.
    //
    // Same arithmetic as rtReprojectHistory, and deliberately the same in every detail: last
    // frame's viewProj, mapped into last frame's VIEWPORT RECT rather than [0,1] of the whole
    // texture (the editor docks the 3D view in a sub-rect), and FLOOR rather than round -- a pixel
    // that has not moved lands on its own centre at index+0.5, which is exactly the tie round()
    // breaks half the time in the wrong direction. Both of those are documented landmines in that
    // function; this one inherits them rather than re-deriving them.
    float2 centrePx = pixel;
    if (gRtHistParams.y > 0.5) {
        const float4 pclip = mul(float4(wpos, 1.0), gPrevViewProj);
        if (pclip.w > 1e-4) {
            const float3 pndc = pclip.xyz / pclip.w;
            if (pndc.z >= 0.0 && pndc.z <= 1.0)
                centrePx = gSceneViewport.xy +
                           float2(pndc.x * 0.5 + 0.5, 0.5 - pndc.y * 0.5) * gSceneViewport.zw;
        }
    }
    const int2 base = int2(floor(centrePx));

    // Plane-distance rejection, not a raw depth delta. Centre depth plus its screen-space gradient
    // defines the receiver's plane; a neighbour on that same plane is kept however far its depth
    // has slid, while one at the same depth on a DIFFERENT surface is dropped. On a grazing floor a
    // plain |dz| test rejects almost everything and the filter quietly does nothing.
    const float dzdx = ddx(curDepth);
    const float dzdy = ddy(curDepth);

    // A GAUSSIAN falloff, where this used to give every accepted neighbour weight 1.0. A flat kernel
    // is a box filter, and a box filter's frequency response rings: the visible form of that is the
    // faint square-edged plateau a box blur leaves around a bright feature, which on a shadow reads
    // as a rectangular halo around a contact point. sigma = radius/2 puts the useful support at
    // about the radius asked for, so raising the radius widens the blur smoothly rather than in
    // steps. Matched to rtReflectionSpatial's kernel deliberately -- two filters over the same
    // geometry with different shapes is a difference someone will eventually have to explain.
    //
    // THIS IS LIVE AT EVERY REAL TIER, AND THE COMMENT THAT USED TO SIT HERE SAID THE OPPOSITE.
    //
    // It claimed the change shipped inert, on the grounds that rtShadowDenoiseForQuality returns 0 at
    // every tier so the early return above fires everywhere. That was simply false, and checkable in
    // one file: Voxi.cpp returns 2 for Low, Medium and High and 1 for Epic -- only Off and an unknown
    // tier return 0 -- and Voxi.hpp defaults the struct field to 2 because the DEFAULT TIER IS Medium.
    // So the radius is 2 out of the box and this kernel runs on every frame that traces a shadow.
    //
    // MEASURED, because it was caught by the oracle rather than by reading: re-recording the gates
    // moved 15 values, and these three in all five ray-tracing-capable configurations, WARP included:
    //
    //   penumbra-rt   33,39,48 -> 62,64,66      partially occluded, much brighter
    //   rt-penumbra   91,88,85 -> 96,93,89      partially occluded, brighter
    //   ms-rt-gi      52,19,13 -> 38,15,11      the same sunVis feeding the GI-composited path
    //
    // and moved shadow-rt/shadow-ms-rt NOT AT ALL. That split is the signature of a reweighting and
    // is what makes the mechanism legible: in full umbra every accepted tap already reads 0, so any
    // weighting averages to 0 and the value is pinned. In a penumbra the old flat kernel dragged the
    // estimate toward far, more-occluded neighbours; the Gaussian discounts them (0.135 at distance
    // 2, 0.018 at the corner, sigma = 1), so partial shadow lightens. Bit-identical across hardware
    // and the WARP software rasteriser, over a record and an independent verify pass -- deterministic,
    // not flaky.
    //
    // THE LESSON IS THE FALSE CLAIM, NOT THE FILTER. A change asserted to be inert is a change nobody
    // reviews as a visual one. If this kernel is ever the wrong choice, that is a judgement to make on
    // a capture -- but it must be made, and the sentence that used to be here prevented it from being
    // asked at all.
    const float sigma  = max((float)radius * 0.5, 0.5);
    const float inv2s2 = 1.0 / (2.0 * sigma * sigma);

    float acc = centre;
    float wsum = 1.0;
    [loop] for (int oy = -radius; oy <= radius; ++oy) {
        [loop] for (int ox = -radius; ox <= radius; ++ox) {
            if (ox == 0 && oy == 0) continue;
            const int2 t = base + int2(ox, oy);
            if (any(t < 0) || t.x >= (int)texW || t.y >= (int)texH) continue;
            const float2 st = gRtShadowHist.Load(int3(t, 0));
            // What this neighbour's depth WOULD be if it sat on the centre's plane.
            const float predicted = curDepth + dzdx * (float)ox + dzdy * (float)oy;
            const float tol = max(abs(predicted), 1.0) * 0.02 + 1.0;
            if (abs(st.y - predicted) > tol) continue;
#if AVER_GBUFFER_HISTORY
            // THE CREASE TERM. A depth-plane test alone cannot see a normal DISCONTINUITY: two
            // surfaces meeting at a similar depth -- a wall meeting a floor, two faces of a box seen
            // edge-on -- can pass the test just above and still be the WRONG surface to blend with,
            // which is what bleeds shadow across a crease that ought to stay sharp (see this
            // function's own header for the grazing-floor failure the plane test already documents;
            // a crease is the other side of the same coin -- two surfaces, not one, where depth
            // alone cannot tell them apart). Sampled from the SAME reprojected tap (`t`) the depth
            // history above just read, at LAST frame's position of LAST frame's normal -- reading
            // THIS frame's own normal here instead would compare two different moments and the
            // comparison would mean nothing, the identical reasoning gRtShadowHist itself is read
            // from last frame rather than this one.
            const float3 nb = gGBufNormalHist.Load(int3(t, 0)).xyz * 2.0 - 1.0;
            // A HARD REJECT, matching the depth-plane test immediately above rather than a soft
            // weight: a neighbour whose normal disagrees is the wrong surface, not a noisier sample
            // of the right one, and blending it in is exactly the crease-bleed this term exists to
            // stop -- "dropped from the kernel, never substituted", the same rule this function's
            // own header states for a plane-test failure. cos(60 deg), not something tighter:
            // averPackNormalRoughness (below) quantises the normal to RGB10A2 -- 10 bits per
            // channel -- and a threshold much tighter than this starts rejecting a FLAT surface's
            // own quantisation noise as if every neighbour were a crease.
            if (dot(N, nb) < 0.5) continue;
#endif
            const float w = exp(-(float)(ox * ox + oy * oy) * inv2s2);
            acc  += st.x * w;
            wsum += w;
        }
    }

    // gRtDenoiseParams.y is how much of the filtered value to take. At 0 the taps above still run --
    // radius comes from a constant, so the loop is dynamic and survives optimisation -- and this
    // returns `centre` EXACTLY, because lerp(v, f, 0) is v + 0*(f-v) for any finite f. That is the
    // configuration the tap cost is measured in, before the filter itself is trusted.
    return lerp(centre, acc / wsum, saturate(gRtDenoiseParams.y));
}

// The PRIMARY sun-shadow call only -- rtReflection's own inner rtShadow() call stays exactly as it
// always has, one ray with no footprint and frameJitter = 0.0, and never touches the history:
// blending in a reflected surface's shadow would overwrite this pixel's history with a value that
// has nothing to do with what a later frame's PRIMARY ray at this same pixel is estimating.
//
// gRtHistParams.w is the pixels-per-ray TILE EDGE, as its bit count (0 = off, every pixel traces
// every frame). At 0 this is BIT-FOR-BIT what rtShadow() alone gives: no jitter, no history blend,
// only a plain write so the buffer stays live for whenever a caller turns tiling on. Tiling is what
// actually cuts ray count -- not blending on its own, which only smooths flicker on something that
// is genuinely moving. Turning tiling on is what makes the (now adaptive, see below) blend cost
// worth paying: most pixels do not trace at all most frames.
// Splits a tinted visibility into the scalar the denoiser filters and the colour it does not.
//
// WHY THE HISTORY DID NOT HAVE TO GROW, which is the whole design of this: the two things rtShadow
// now returns have completely different character. OCCLUSION is binary and noisy -- one ray either
// met a wall or did not -- and that is exactly what the temporal and spatial filters exist to
// smooth. A MEDIUM'S TINT is smooth and almost noise-free: it is a continuous function of a path
// length through a volume, and neighbouring pixels agree closely. Filtering it buys nothing.
//
// So the scalar keeps going through the RG32Float history and its filters, bit for bit as before,
// and the tint rides on top unfiltered. Widening gRtShadowHist to RGBA32Float would have doubled two
// buffers already costing ~56 MB each at scene resolution, and RGBA16Float would have put the
// history's linear DEPTH channel -- clip-space w in centimetres, which reaches tens of thousands out
// on terrain -- into a format with 8 cm of precision up there, which the disocclusion test reads.
// Neither price is worth paying for a quantity that does not need filtering.
float averShadowLum(float3 v) { return dot(v, float3(0.2126, 0.7152, 0.0722)); }

// The normalised colour of a tinted visibility. White when there is effectively nothing to tint --
// a fully occluded pixel has no medium colour to speak of, and dividing by its luminance would be a
// 0/0 that spreads NaN through the filter.
float3 averShadowTint(float3 v, float lum) {
    return (lum > 1e-4) ? (v / lum) : float3(1.0, 1.0, 1.0);
}

float3 rtShadowTemporal(float3 wpos, float3 N, float3 L, float2 pixel, float3 dpx, float3 dpy, uint rays) {
    // gRtHistParams.x is 0 whenever t6/u2 are not bound to real textures this frame (see
    // VoxiRenderer::beginShadowHistory) -- an unbound slot is Tier 1 null-filled, and touching
    // either one here would read or write a null descriptor rather than skip cleanly.
    if (gRtHistParams.x < 0.5) return rtShadow(wpos, N, L, pixel, dpx, dpy, rays, 0.0);

    // THIS frame's own linear depth at wpos, computed the same way VSMain would (mul(wp, gViewProj)
    // .w) rather than read back from a depth buffer -- the shadow pass has none of its own. Written
    // into the history alongside visibility every single write below, regardless of which branch is
    // taken, so next frame's rtReprojectHistory always has a fresh depth to disocclusion-test
    // against, not one that is itself stale by however many frames since this pixel's last turn.
    const float curDepth = mul(float4(wpos, 1.0), gViewProj).w;

    const uint tileBits = (uint)gRtHistParams.w;
    if (tileBits == 0u) {
        const float3 fresh3 = rtShadow(wpos, N, L, pixel, dpx, dpy, rays, 0.0);
        const float  fresh   = averShadowLum(fresh3);
        const float3 tint    = averShadowTint(fresh3, fresh);
        gRtShadowHistOut[uint2(pixel)] = float2(fresh, curDepth);
        // FILTERED HERE TOO, and this branch is the one that matters most. tileBits == 0 is
        // rtPixelsPerRayTile == 1, which is what Medium -- the DEFAULT tier -- runs, and it is the
        // only configuration in which the shadow term is a hard 0 or 1 with nothing whatsoever
        // smoothing it. Returning `fresh` straight from here, as this did, wired the spatial filter
        // into the amortised path alone and left the default one completely untouched: the penumbra
        // probe read an unchanged 61,59,59 at every radius, which is what caught it. Two returns,
        // two call sites -- an early return is exactly how a later edit loses one of them again.
        return rtShadowSpatial(fresh, wpos, N, pixel, curDepth) * tint;
    }

    // Which pixel in its tileBits x tileBits tile gets to trace THIS frame -- a bitmask against the
    // pixel coordinate and the frame index, not a modulo, since tileBits is always a power of two
    // (VoxiRenderer::setPixelsPerRayTile only ever rounds to one). Over 2^(2*tileBits) consecutive
    // frames every pixel in the tile gets exactly one turn, staggered so a whole tile is never
    // skipped or traced together -- a block pattern would show as visible tiles rather than noise.
    const uint frameIdx = (uint)gRtHistParams.z;
    const uint tileMask = (1u << tileBits) - 1u;
    const uint turnMask  = (1u << (2u * tileBits)) - 1u;
    const uint localIdx  = (((uint)pixel.y & tileMask) << tileBits) | ((uint)pixel.x & tileMask);
    const bool myTurn    = localIdx == (frameIdx & turnMask);

    float hist = 0.0;
    float2 velocityPx = 0.0;
    const bool haveHist = gRtHistParams.y > 0.5 && rtReprojectHistory(wpos, pixel, hist, velocityPx);

    float vis;
    // WHITE WHEN THIS PIXEL DID NOT TRACE. On a reused-history frame there is no fresh sample to
    // take a colour from, and the history deliberately does not store one -- so the tint falls back
    // to untinted rather than to a stale colour from a different surface. Only reachable on the
    // amortised path, which is dead at every shipped tier (rtPixelsPerRayTile is 1 everywhere).
    float3 tint = float3(1.0, 1.0, 1.0);
    if (myTurn || !haveHist) {
        // The golden-angle frame offset spends a DIFFERENT sample of the same low-discrepancy
        // sequence each turn, so 2^(2*tileBits) turns converge toward the same estimate that many
        // SPATIAL rays would give in one frame -- see rtDiscSample for why that composition works.
        // frameJitter = 0.0 (rtHash alone) would repeat the identical ray every turn, which never
        // converges past one sample; that is the whole reason tiling needs this and the tileBits==0
        // path above does not.
        const float frameJitter = (float)frameIdx * 2.39996323;
        const float3 vis3 = rtShadow(wpos, N, L, pixel, dpx, dpy, rays, frameJitter);
        vis  = averShadowLum(vis3);
        tint = averShadowTint(vis3, vis);
        if (haveHist) {
            // ADAPTIVE blend weight, not a flat constant: a reprojected sample that has barely
            // moved on screen is close to a repeated measurement of the same point and earns a high
            // weight; one that has moved several pixels is increasingly likely to be sampling
            // slightly the wrong part of the surface even though it passed the depth test above
            // (depth alone tells "same surface" from "different surface", not "same surface, but
            // I've slid along it"), so it is trusted less the faster it is moving. The budget the
            // falloff runs over SHRINKS as the tile grows: a bigger tile's history is on average
            // staler even before any motion is considered (up to 2^(2*tileBits) frames old), so the
            // same screen velocity should discount it over a shorter distance.
            const float budget = max(6.0 - 1.5 * (float)tileBits, 1.0);
            const float t = saturate(length(velocityPx) / budget);
            const float weight = lerp(0.9, 0.1, t);
            vis = lerp(vis, hist, weight);
        }
    } else {
        // Not this pixel's turn, and reprojection is valid: reuse it outright. No ray at all this
        // frame -- this is the actual saving tiling exists for.
        vis = hist;
    }

    // WRITE THE RAW VALUE, NEVER THE FILTERED ONE, and this is the single most important line in
    // the whole denoiser. gRtShadowHistOut is what next frame reprojects from; feeding a filtered
    // value back into it makes this an IIR filter with a spatial kernel -- a TEMPORAL filter by
    // another name -- and every artefact the spatial path exists to avoid comes straight back,
    // compounding a little more each frame. The filter is applied on READ, below, and the history
    // never learns it happened. A later reader will be tempted to "save work" by writing the
    // filtered value here; that is the bug, not the optimisation.
    gRtShadowHistOut[uint2(pixel)] = float2(vis, curDepth);
    return rtShadowSpatial(vis, wpos, N, pixel, curDepth) * tint;
}

// Traces one reflection ray and shades what it hits.
//
// THIS IS WHAT MAKES REFLECTIONS GLOBAL. The cone tracer it replaces walks the voxel volume and
// stops dead at its boundary -- `if (!insideVolume(uvw)) break;` -- so anything outside simply was
// not reflected and the result fell back to sky. Objects popped in and out of reflections as they
// crossed a boundary that has nothing to do with the scene. A ray has no such bound: it reaches
// whatever the acceleration structure holds, at any distance.
//
// The shading is one bounce of Lambertian direct light plus sky ambient, with the hit surface's own
// albedo from the instance table. No textures and no second bounce -- a reflected surface is
// slightly flatter than the same surface seen directly, which is a stated approximation rather than
// an accident.
//
// ---- THE LOBE: why `rough` is a parameter now, and what it fixed ----
//
// This function used to trace R exactly, and its caller's comment said so plainly: "A MIRROR RAY IS
// ONLY RIGHT FOR A SMOOTH SURFACE ... a single ray has no aperture at all, so applying it to a rough
// surface hands back a sharp reflection where a blurred one belongs". The caller's answer was to
// REFUSE the reflection above roughness 0.5 and to fade what remained toward flat sky at twice the
// roughness -- so a half-rough surface got no ray, and a quarter-rough surface got a mirror
// reflection at half strength mixed with half a flat sky colour. Neither of those is a glossy
// reflection; they are two ways of not having one.
//
// The fix is the aperture the comment says is missing. `rough` widens the ray into a cone, exactly
// the way `gRtParams.x` (the tangent of the sun's angular radius) already widens the shadow ray into
// the sun's disc -- SAME rtDiscSample sequence, SAME per-pixel rotation, SAME frameJitter
// decorrelation, so this is one more user of machinery that was already here and already proven
// bit-exact across the nine gate configurations, not a second sampling scheme to keep in step.
//
// tan(cone) = rough*rough is the GGX alpha, not a tuned number: alpha = rough^2 is the standard
// remapping from perceptual roughness to the microfacet distribution's width, and for small angles
// the tangent of the lobe's half-angle IS alpha. It falls out where it must -- a glass pane at
// roughness 0.05 gets tan = 0.0025, which over a 10 m reflection distance is a 2.5 cm spread, i.e.
// still a mirror; a half-rough floor at 0.5 gets tan = 0.25, about 14 degrees, which is a real blur.
//
// ONE RAY, NOT A SWEEP. Widening the cone does not cost more rays; it costs VARIANCE, which is paid
// down over frames by rtReflectionTemporal's history and across pixels by rtReflectionSpatial's
// roughness-scaled kernel. That is the whole trade, and it is the right one here: a second ray would
// halve the noise and double the cost of the single most expensive thing in the ray path, while the
// filter pair costs texture loads.
//
// `frameJitter` is ADDED to the per-pixel rotation and is zero for every caller that has no history
// to converge into -- the identical contract, for the identical reason, as rtShadow's own parameter
// of the same name. rtHash stays a pure function of the pixel so a single-frame capture is still
// bit-exact; see rtHash's own comment, which the gate oracle depends on.
float3 rtReflection(float3 wpos, float3 N, float3 R, float3 L, float2 pixel, float rough,
                    float frameJitter, out bool hit) {
    hit = false;

    // The cone. At rough = 0 this is exactly zero and the arithmetic below reduces to `dir = R`
    // bit-for-bit (normalize(R + 0) with R already unit), so a mirror surface is untouched by this
    // change -- which is what keeps a chrome ball and a pane of glass looking the way they did.
    const float tanCone = rough * rough;
    float3 dir = R;
    if (tanCone > 0.0) {
        float3 up = abs(R.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0);
        float3 T  = normalize(cross(up, R));
        float3 B  = cross(R, T);
        // Sample index 0 of the nested disc sequence, turned by this pixel's own angle plus the
        // frame's. Index 0 rather than a per-pixel index because the sequence's RADIUS depends on the
        // index and a per-pixel index would give neighbouring pixels systematically different cone
        // widths -- a spatial filter would then be averaging samples drawn from different lobes. One
        // fixed radius, rotated per pixel and per frame, keeps every sample on the same ring of the
        // same lobe, which is what makes both filters' means unbiased.
        const float2 d = rtDiscSample(0, rtHash(pixel) * 6.2831853 + frameJitter);
        dir = normalize(R + (T * d.x + B * d.y) * tanCone);
        // A cone wide enough to swing the ray below the surface would reflect the receiver into
        // itself and shade from a hit that is geometrically behind it. Clamp back into the upper
        // hemisphere rather than dropping the sample: dropping it biases the estimate dark exactly
        // where the lobe is widest, which is where the estimate matters most.
        if (dot(dir, N) <= 0.0) dir = normalize(dir - N * (dot(dir, N) - 1e-3));
    }

    RayDesc r;
    const float bias = max(gRtParams.z, 1e-4) * (1.0 + length(wpos - gCamPos.xyz) * 5e-4);
    r.Origin    = wpos + N * bias;
    r.Direction = dir;
    r.TMin      = bias;
    r.TMax      = 100000.0;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    // Opaque lane only -- see AVER_RT_MASK_OPAQUE. A single Proceed() cannot correctly traverse
    // past a non-opaque candidate, and this ray has no reason to want one.
    // FORCE_OPAQUE IS FREE HERE, AND PROVABLY A NO-OP. createBlas marks every geometry it builds
    // D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE (D3D12Device.cpp), and the only thing that can un-opaque an
    // instance is TlasInstanceFlag_ForceNonOpaque, which VoxiRenderer sets ONLY on the translucent
    // lane -- the lane this ray's AVER_RT_MASK_OPAQUE excludes. So no candidate this ray can ever see
    // is non-opaque, and saying so lets the hardware skip any-hit bookkeeping entirely.
    //
    // NOT on the shadow ray (this file, the rtShadow query): that one masks AVER_RT_MASK_ALL on
    // purpose so a pane of glass can attenuate it, and forcing opaque there would make every pane a
    // wall -- which is the exact behaviour its own comment says was removed.
    q.TraceRayInline(gScene, RAY_FLAG_FORCE_OPAQUE, AVER_RT_MASK_OPAQUE, r);
    q.Proceed();
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return 0.0;

    RtInstance inst = gRtInstances[q.CommittedInstanceID()];
    uint tri = inst.firstIndex + q.CommittedPrimitiveIndex() * 3;
    uint i0 = inst.firstVertex + gRtIndices[tri + 0];
    uint i1 = inst.firstVertex + gRtIndices[tri + 1];
    uint i2 = inst.firstVertex + gRtIndices[tri + 2];

    float2 bary = q.CommittedTriangleBarycentrics();
    float3 w = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    float3 nObj = normalize(gRtVerts[i0].nrm * w.x + gRtVerts[i1].nrm * w.y + gRtVerts[i2].nrm * w.z);
    // Rotation only. The engine is row-vector, so a direction is the vector times the upper 3x3 --
    // and a non-uniform scale would need the inverse transpose, which this deliberately does not
    // carry: reflections here are of rigid instances.
    float3 nWS = normalize(mul(float4(nObj, 0.0), inst.objectToWorld).xyz);
    // `dir`, NOT `R`. These two used to be the same vector and are not any more: once the lobe
    // widens, testing the facing against R and marching along R would place the hit somewhere the
    // ray never went, which shows up as a reflected surface lit from the wrong side and as shading
    // sampled at the wrong distance -- worst exactly where the cone is widest.
    if (dot(nWS, dir) > 0.0) nWS = -nWS;   // face the ray, so a back-facing hit is not lit from behind

    float3 hitPos = wpos + dir * q.CommittedRayT();
    // Whether the SUN reaches the reflected surface. Without this every reflection is lit as if
    // nothing could shadow it, which is what makes cheap reflections look like they glow.
    // Seeded from the PIXEL, and with NO footprint. Seeding from hitPos.xy -- a world float derived
    // from CommittedRayT -- makes the sample pattern depend on a ray distance, which is exactly the
    // value most likely to differ between a hardware adapter and WARP, and the gate oracle compares
    // nine configurations bit-exactly.
    // ONE ray, not the full disc. This is the single largest saving available in the ray path:
    // a reflective pixel was firing one reflection ray plus a four-ray disc from its hit, so five
    // rays where two do. The penumbra of a reflected shadow is not resolvable in a one-bounce
    // mirror image.
    // float3: a reflected surface seen through a tinted medium is lit through that tint too.
    float3 shadow = rtShadow(hitPos, nWS, L, pixel, float3(0,0,0), float3(0,0,0), 1u, 0.0);

    // LAMBERTIAN EXITANT RADIANCE, and the /PI is the whole point. averGroundRadiance is the
    // engine's own reference for this and reads:
    //
    //     E = sunIrradiance*ndl + PI*skyRadiance*ambient;   return albedo * E / PI;
    //
    // so the sun's contribution to outgoing RADIANCE is albedo*sunIrradiance*ndl/PI, while the
    // sky's is albedo*skyRadiance*ambient with the PI cancelling. The first version of this
    // function omitted the divide on the direct term only, which made every SUNLIT reflection
    // 3.14x too bright while leaving shaded ones correct -- brightness that looks like an exposure
    // problem rather than a units one, and which pushed the lit ray-traced gates from 94,27,14 to
    // 131,58,40.
    //
    // THE WHITE FURNACE DOES NOT CATCH THIS, and that is worth knowing about the oracle: it turns
    // the sun OFF, so the direct term is zero and only the ambient half -- which was already right
    // -- is under test. A furnace with a sun is a second mode worth having.
    float3 direct = averSunRadiance() * saturate(dot(nWS, L)) * shadow / PI;
    float3 ambient = averSkyIrradiance(nWS) * gAmbient.r;
    hit = true;

    // WHAT THE REFLECTED SURFACE'S ALBEDO ACTUALLY IS.
    //
    // `inst.albedo` is the per-draw FLAT colour. Until the primary path started sampling textures
    // that was at least consistent -- everything was flat. It is not consistent any more: a scene
    // whose directly-viewed surfaces carry real base-colour maps, reflected as untextured Lambertian
    // paint, is exactly the mismatch that reads as "the reflection is fake".
    //
    // Everything needed is already here: the hit resolved inst.materialIndex, gRtMaterials is bound,
    // the barycentrics and the three vertices are in hand, and averRtSurfaceUV/averRtSampleSlot are
    // the same helpers the primary hit uses -- so a reflected surface and a directly-viewed one
    // resolve their UV the same way, world-aligned materials included.
    float3 reflAlbedo = inst.albedo;
#ifdef AVER_RT_BINDLESS
    {
        const RtMaterial rmat = gRtMaterials[inst.materialIndex];
        const float2 meshUV = gRtVerts[i0].uv * w.x + gRtVerts[i1].uv * w.y + gRtVerts[i2].uv * w.z;
        const float2 ruv    = averRtSurfaceUV(rmat, inst, hitPos, nWS, meshUV);

        // A CONE FOOTPRINT, NOT MIP 0. Zero gradients degrade to SampleLevel(0), and mip-0 sampling
        // in this pass was measured as a THROUGHPUT problem, not merely a quality one -- proper mip
        // selection took the whole frame 11.96 -> 8.14 ms on ElectricDreams. A reflection ray has no
        // screen derivatives at all, but it does have a cone: the lobe half-angle is tanCone and the
        // ray travelled CommittedRayT, so the footprint on the hit surface is their product. Rougher
        // reflections therefore read wider mips, which is free denoising rather than a compromise.
        const float  rad = max(tanCone, 1e-3) * q.CommittedRayT();
        // The same up-vector trick this function already uses to build its cone basis a few dozen
        // lines up, applied to the hit normal instead of R. Two orthogonal in-plane directions are
        // all averRtUvGrad needs -- it projects them through the surface's own position-to-UV map.
        const float3 rup = abs(nWS.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0);
        const float3 rt  = normalize(cross(rup, nWS));
        const float3 rb  = cross(nWS, rt);
        float2 rgx, rgy;
        averRtUvGrad(rmat, inst, nWS,
                     gRtVerts[i0].pos, gRtVerts[i1].pos, gRtVerts[i2].pos,
                     gRtVerts[i0].uv,  gRtVerts[i1].uv,  gRtVerts[i2].uv,
                     rt * rad, rb * rad, rgx, rgy);
        reflAlbedo *= averRtSampleSlot(rmat, 0, ruv, rgx, rgy, float4(1, 1, 1, 1)).rgb
                    * rmat.baseColorFactor.rgb;
    }
#endif
    return reflAlbedo * (direct + ambient);
}

// How far a view ray travels INSIDE a volume before something stops it, in centimetres.
//
// WHAT THIS IS FOR. averVolumeTransmittance needs a path length, and a blended surface has no idea
// how thick it is -- it is one fragment on one face. The old fluid shader guessed, with
// `depthCm / max(abs(V.z), 0.15)` measured from a CONSTANT floor height (FluidShaders.hpp), which is
// a depth below the surface only on the TOP face of a box. On a side face it is height up the wall,
// and at pitch 0 the clamp turns it into an ~8.7 m path, so the side of a pool came out fully opaque
// and bright. That is the whole reason the PTTest pit read as a blown-out white slab. A ray does not
// have to guess: it measures.
//
// FORCE_OPAQUE, DELIBERATELY, and this is the one flag that makes a single Proceed() correct here.
// Translucent instances are in the structure as FORCE_NON_OPAQUE (see the mask comment at the top of
// this file), so an ordinary traversal would stop AT a candidate without committing it and this
// function would answer with whatever CommittedStatus happened to hold. FORCE_OPAQUE overrides that
// per-instance flag for this ray only: traversal commits the nearest hit in either lane and returns
// false from Proceed(), which is exactly the question being asked -- "what is the first thing along
// this ray, of any kind".
//
// BOTH LANES, because both can end the path. The volume's own BACK FACE ends it (that is the exit
// point of a convex body), and so does any opaque object sitting INSIDE the volume -- a rock in a
// pool, the pool's own floor. Taking the nearest of the two is what makes absorption respond to real
// geometry instead of to an authored box height, which is the property the old formula could not
// have at any amount of tuning.
//
// Returns 0 when nothing is hit, which averVolumeTransmittance reads as "no path through the
// medium" and answers with full transmission. That is the right failure: an unbounded volume should
// not absorb infinitely, it should absorb nothing until someone gives it a boundary.
// Is a real backdrop bound? A null-filled Texture2D reports zero dimensions, and that is the only
// signal available from inside the shader -- the alternative is threading a validity flag through a
// constant buffer, which would mean finding or inventing a spare component for something the
// descriptor already tells us. Tested rather than assumed: forcing the null case (MSAA on, where the
// device cannot take the copy) and confirming glass falls back instead of going black is part of
// this feature's verification.
bool averBlendBackdropValid(out float2 invSize) {
    uint w = 0, h = 0;
    gBlendBackdrop.GetDimensions(w, h);
    const bool ok = (w > 0u && h > 0u);
    invSize = ok ? (1.0 / float2((float)w, (float)h)) : float2(0.0, 0.0);
    return ok;
}

// THE VOLUME COMPOSITE, WITH THE BACKGROUND AS A CORRECTION RATHER THAN A REPLACEMENT.
//
// What we want, physically, is the surface's own light plus what survived the medium:
//
//     final = specular + diffuse*alpha + dst * T * (1 - alpha)
//
// and the hardware, in premultiplied alpha, gives `final = src.rgb + dst * (1 - src.a)`. Setting
// src.a = alpha and solving for src.rgb:
//
//     src.rgb = specular + diffuse*alpha + bg * (1 - alpha) * (T - 1)
//
// THE LAST TERM IS THE WHOLE TRICK, and it is why this reads as a subtraction. (T - 1) is negative,
// so it removes exactly the light the medium absorbed, per channel -- the thing one blend alpha
// cannot express. The hardware still adds the REAL destination afterwards.
//
// TWO PROPERTIES THIS BUYS, both deliberate:
//
//   T == 1 MAKES THE CORRECTION EXACTLY ZERO, so a material with no volume composites
//   bit-for-bit as it did before this existed. That is what the no-absorption regression check
//   leans on: it is an identity, not an approximation that happens to be close.
//
//   STACKED TRANSLUCENCY DEGRADES GENTLY. `bg` is the scene copied BEFORE any translucent draw, so
//   for a second layer it is stale -- but it is used only in the correction, while the base
//   composite still blends against the true `dst`. Replacing the background outright (alpha = 1)
//   would have made the nearer pane erase the farther one entirely. This map puts glass over water
//   on purpose, so that is not a hypothetical.
//
// Falls back to the scalar composite when no backdrop is bound, which is the honest answer rather
// than sampling black and calling it absorption.
// WHERE THE BACKGROUND IS READ FROM, once the surface is allowed to bend it.
//
// Absorption decides what COLOUR survives the medium; refraction decides where it comes FROM. This
// is the second half, and gIor is its first reader -- that uniform has been uploaded and unread
// since it was added, which its own declaration comment says out loud.
//
// Returns the UV to sample the backdrop at. gGiParams.y is the mode, .z the strength, .w the edge
// fade (see Settings::refractionMode and its neighbours for the ladder these come off).
float2 averRefractedBackdropUV(AverSurface s, float3 wpos, float thicknessCm,
                               float2 invSize, float2 screenPos, out bool tir) {
    tir = false;
    const float2 uv0  = screenPos * invSize;
    const uint   mode = (uint)(gGiParams.y + 0.5);
    if (mode == 0u || gGiParams.z <= 0.0) return uv0;

    // WHICH WAY THE LIGHT IS CROSSING, which decides everything below.
    //
    // eta is the ratio of the index the ray is LEAVING to the one it is ENTERING. Looking at a pane
    // from the outside that is air->medium, 1/n. Looking at the underside of the pool's surface while
    // swimming in it, it is medium->air, n -- and getting that backwards does not merely bend the
    // image the wrong way, it makes total internal reflection unreachable, because TIR only exists
    // for eta > 1.
    //
    // GATED ON gCameraMedium, NOT ON s.backFace, AND THAT DISTINCTION HAS ALREADY COST THIS ENGINE
    // ONCE. material_prelude.hlsl carries the post-mortem: a TIR override gated on backFace "turned
    // every pane of glass in the engine into a dark slab at 41 degrees off normal", because backFace
    // is also true for the FAR surface of a two-sided pane seen from outside -- an ordinary
    // air-to-glass view with the normal flipped -- and Snell forbids TIR there outright. That
    // comment concludes the two cases are "indistinguishable from a pixel shader". They were. They
    // are not any more: gCameraMedium.x is computed on the CPU from the camera against the volume's
    // own bounds, which is exactly the fact a pixel could never recover for itself. Glass is
    // twosided=1 and is never a medium by that test, so the pane case cannot reach this branch at
    // all.
    const float  ior      = max(gIor, 1.0001);
    const bool   eyeInside = gCameraMedium.x > 0.5;
    const float  eta      = eyeInside ? ior : (1.0 / ior);
    float3 R = refract(-s.V, s.N, eta);
    if (dot(R, R) < 1e-6) {
        // refract() returns 0 to say "no transmitted ray exists". Normalising it would be a NaN.
        if (!eyeInside) return uv0;   // from outside this is unreachable; sampling straight through
                                      // stays the honest fallback rather than a fabricated bend.
        // GENUINE TIR, and this is the half of Snell's window nobody sees from above: past the
        // critical angle -- 48.75 degrees at n = 1.33 -- the underside of the water stops being a
        // window and becomes a MIRROR, showing the pool floor and walls instead of the sky. Reflect
        // about the same eye-facing normal and let the machinery below project it exactly as it
        // projects a refracted target.
        tir = true;
        R = reflect(-s.V, s.N);
    }

    // Where the bent ray leaves the medium, one thickness along the bent path rather than the
    // straight one -- which is the whole point, and is why this needs the ray-measured thickness
    // rather than an authored constant.
    float3 target = wpos + R * max(thicknessCm, 0.0);

#if AVER_RT
    // RAY-TRACED: follow the bent ray to what it ACTUALLY reaches and project THAT. The screen-space
    // mode below can only ever offset within the image the camera already captured; this finds real
    // geometry, so the distortion follows surfaces instead of sliding pixels around. It still reads
    // colour from the backdrop -- shading the hit properly would mean a second full material
    // evaluation on the frame's most expensive pass -- so a hit that was off-screen or occluded
    // still resolves to whatever the backdrop holds there. Honest limit, much better geometry.
    if (mode >= 2u) {
        RayDesc rr;
        rr.Origin = target;
        rr.Direction = R;
        rr.TMin = 0.0;
        rr.TMax = 100000.0;
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
        rq.TraceRayInline(gScene, RAY_FLAG_FORCE_OPAQUE, AVER_RT_MASK_OPAQUE, rr);
        rq.Proceed();
        if (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
            target = target + R * rq.CommittedRayT();
    }
#endif

    const float4 clip = mul(float4(target, 1.0), gViewProj);
    if (clip.w <= 1e-4) return uv0;               // behind the eye: nothing sensible to sample
    const float2 ndc = clip.xy / clip.w;

    // NDC -> THE VIEWPORT RECT, NOT [0,1] OF THE WHOLE TARGET, and this line is the whole bug that
    // made refraction look like a wrecked image rather than a bent one.
    //
    // The editor docks the 3D view in a SUB-RECT of the backbuffer -- a toolbar above it, panels to
    // the right -- so NDC maps to gSceneViewportCur, while uv0 (screenPos * invSize) is a full-target
    // UV because SV_Position is in target pixels. A plain ndc*0.5+0.5 mixes those two spaces. The
    // error is affine, so it does not look like noise: it looks like the pane is showing a shifted,
    // scaled copy of the scene, and near the right-hand edge it walks clean off the image and
    // samples nothing, which is where the black block in the glass rail came from.
    //
    // MEASURED, before the fix, by forcing target = wpos so the answer HAD to be uv0 exactly: 100%
    // of the rail's pixels still landed more than 60 px away. That is what proved this was the
    // projection and not the refraction offset -- an 8 cm pane cannot bend anything by 60 px, and I
    // had been about to go looking at the offset.
    //
    // The file already knew: rtReprojectHistory (voxi.hlsl, "NDC -> LAST frame's VIEWPORT rect")
    // and three other sites all do exactly this conversion, and one of them says in its own comment
    // that the plain form "lands every reprojection on the wrong texel". Refraction was written
    // later and did not pick it up.
    //
    // A ZERO-WIDTH RECT means the device reported no viewport this frame. Sampling straight through
    // is the honest answer there -- the same fallback this function already takes for total internal
    // reflection and for a target behind the eye.
    if (gSceneViewportCur.z <= 0.0 || gSceneViewportCur.w <= 0.0) return uv0;
    const float2 pxR = gSceneViewportCur.xy +
                       float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * gSceneViewportCur.zw;
    float2 uvR = pxR * invSize;

    // THE EDGE FADE, and it is not cosmetic. The backdrop only holds what the camera saw, so an
    // offset that walks off the screen samples nothing meaningful and one that walks onto a
    // FOREGROUND object shows that object through the glass. Fading the offset back to zero near
    // the border turns both into a soft loss of refraction rather than a hard wrong pixel.
    // FADED AGAINST THE VIEWPORT RECT, NOT THE TARGET. Outside the 3D view the backdrop holds
    // whatever the rest of the frame is, so "still on the texture" is not the test that matters --
    // "still on the part of the texture the camera drew" is. Measuring to the target border let a
    // pane keep sampling right up to the edge of the docked view and then past it.
    const float fadePx = max(gGiParams.w, 0.0);
    float edge = 1.0;
    if (fadePx > 0.0) {
        const float2 vpMin = gSceneViewportCur.xy * invSize;
        const float2 vpMax = (gSceneViewportCur.xy + gSceneViewportCur.zw) * invSize;
        const float2 d = min(uvR - vpMin, vpMax - uvR);   // to the nearest viewport border, in UV
        edge = saturate(min(d.x, d.y) / fadePx);
    }
    return lerp(uv0, uvR, saturate(gGiParams.z) * edge);
}

float4 averBlendedOutputBackdrop(AverSurface s, float3 diffuse, float3 specular, float3 T,
                                 float2 screenPos, float3 wpos, float thicknessCm) {
    float2 invSize;
    if (!averBlendBackdropValid(invSize))
        return averBlendedOutputVolume(s, diffuse, specular, T);

    // TWO SAMPLES, AND THE SECOND ONE IS NOT AN EXTRA -- IT IS WHAT MAKES THE CORRECTION CANCEL.
    //
    // This function does not write the background; the HARDWARE does, adding dst * (1 - alpha) after
    // this returns. Everything here is a correction ON TOP of that, and a correction can only work if
    // it subtracts EXACTLY WHAT THE HARDWARE WILL ADD -- the pixel straight behind this one, at uv0.
    //
    // The single-sample version subtracted the REFRACTED sample instead. Those are different pixels
    // the moment refraction is on, so nothing cancelled and the residue was (dst - bgRefracted) per
    // channel: wherever the bent ray landed on something BRIGHTER than what is really behind -- the
    // sunlit deck sampled against the dark pool floor -- the sum went NEGATIVE, and a negative
    // channel through the tonemap is not dark, it is a hue. That is what the magenta blocks in the
    // pool were. MEASURED: 41773 magenta pixels at the 45-degree pool camera with refraction on,
    // exactly 0 with --refraction 0, and 0 with refraction on after this change.
    //
    // The algebra, with a == alpha:
    //     final = specular + diffuse*a + (bgRefr*T - bgStraight)*(1-a) + dst*(1-a)
    //   and dst IS bgStraight, so the last two collapse and leave
    //     final = specular + diffuse*a + bgRefr*T*(1-a)
    //   which is the answer: the bent background, absorbed over the path, behind a Fresnel-weighted
    //   surface. Two properties worth keeping:
    //     - refraction OFF makes bgRefr == bgStraight and this becomes bg*(T-1)*(1-a) again, the
    //       exact expression this replaced, so the no-refraction path is bit-identical.
    //     - T == 1 leaves (bgRefr - bgStraight)*(1-a): pure bending, no absorption. Also correct,
    //       and it means a material with refraction but no volume is no longer silently wrong.
    //
    // dst == bgStraight holds for the FIRST translucent surface over a pixel. A second one behind
    // glass is compositing against a backdrop that does not yet contain the first, which is the
    // known cost of capturing the backdrop once per frame rather than once per draw. It is an
    // approximation in the overlap only, and it is bounded -- unlike the misregistration above,
    // which was unbounded and could invert a channel.
    bool tir = false;
    const float2 uvR   = averRefractedBackdropUV(s, wpos, thicknessCm, invSize, screenPos, tir);
    const float2 uv0   = screenPos * invSize;
    const float3 bgR   = gBlendBackdrop.SampleLevel(gMaterialSampler, uvR, 0).rgb;
    const float3 bg0   = gBlendBackdrop.SampleLevel(gMaterialSampler, uv0, 0).rgb;
    const float  alpha = saturate(s.alpha);

    // TOTAL INTERNAL REFLECTION IS NOT A WINDOW WITH A DIFFERENT UV, AND TREATING IT AS ONE MADE IT
    // BLACK. Past the critical angle no light is transmitted at all: everything the eye receives
    // arrived from the REFLECTED direction. The window form below weights its background term by
    // (1 - alpha), and alpha is driven toward 1 by the view Fresnel at exactly the grazing angles
    // where TIR occurs -- so the mirror was multiplied by roughly zero. Measured underwater at 25
    // degrees: the mirrored region read 0.59 mean against the 18.8 of the pit wall it was supposed
    // to be showing. Not dark because the pool is dark; dark because the term was cancelled.
    //
    // alpha = 1 is correct HERE and was wrong in the case material_prelude.hlsl's post-mortem
    // describes. That override slammed alpha to 1 on a PANE seen from outside -- where Snell forbids
    // TIR outright -- and supplied no reflected image, so it left a dark slab. This fires only where
    // gCameraMedium says the eye is genuinely inside a single-sided volume, and it hands back the
    // reflected scene as the surface's own radiance. The background is blocked because a mirror
    // blocks it, and replaced because a mirror replaces it.
    if (tir) return float4(specular + bgR * T, 1.0);

    return float4(specular + diffuse * alpha + (bgR * T - bg0) * (1.0 - alpha), alpha);
}

float averVolumeThickness(float3 wpos, float3 N, float3 viewDir) {
    RayDesc r;
    // The SAME bias the reflection ray uses, and for the same reason -- a ray starting exactly on the
    // surface it just shaded re-hits it at t≈0 and reports a thickness of zero. Pushed along the view
    // direction rather than along N: this ray is deliberately heading INTO the surface, so offsetting
    // along the normal would push it out of the very volume it is trying to measure.
    const float bias = max(gRtParams.z, 1e-4) * (1.0 + length(wpos - gCamPos.xyz) * 5e-4);
    r.Origin    = wpos + viewDir * bias;
    r.Direction = viewDir;
    r.TMin      = 0.0;
    r.TMax      = 100000.0;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gScene, RAY_FLAG_FORCE_OPAQUE,
                     AVER_RT_MASK_OPAQUE | AVER_RT_MASK_TRANSLUCENT, r);
    q.Proceed();
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return 0.0;
    return q.CommittedRayT() + bias;
}

// Reprojects wpos through LAST frame's camera to sample the reflection history there. False when
// not usable: off-screen, behind last frame's near plane, no hit recorded there (stored.a <= 0 --
// see gRtReflHist's own comment for the miss sentinel), or a depth mismatch (disocclusion) -- the
// exact same test rtReprojectHistory uses for the shadow, against the reflection's own depth
// channel instead.
//
// A MISS IS NEVER REPROJECTED, on purpose, and that is the one real difference from the shadow
// case: sky-by-direction is cheap to recompute (no ray, just an analytic model) and highly VIEW
// dependent, so reusing a stale miss sample as the camera rotates would show the wrong patch of
// sky through a still surface. A real hit's shading has no such problem -- it depends on the
// reflected surface and a mostly-static light, not on the viewing angle -- so only hits are worth
// the reprojection at all.
bool rtReprojectReflection(float3 wpos, float2 pixel, out float3 hist, out float2 velocityPx) {
    hist = 0.0;
    velocityPx = 0.0;
    float4 clip = mul(float4(wpos, 1.0), gPrevViewProj);
    if (clip.w <= 1e-4) return false;
    float3 ndc = clip.xyz / clip.w;
    if (ndc.z < 0.0 || ndc.z > 1.0) return false;
    float texW, texH;
    gRtReflHist.GetDimensions(texW, texH);
    float2 px = gSceneViewport.xy +
                float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * gSceneViewport.zw;
    int2 texel = int2(floor(px));   // see rtReprojectHistory for why floor, not round
    if (any(texel < 0) || texel.x >= (int)texW || texel.y >= (int)texH) return false;

    const float4 stored = gRtReflHist.Load(int3(texel, 0));
    if (stored.a <= 0.0) return false;   // a miss was recorded there -- nothing to reuse
    const float tol = max(clip.w, stored.a) * 0.03 + 1.0;
    if (abs(clip.w - stored.a) > tol) return false;

    hist = stored.rgb;
    velocityPx = px - pixel;
    return true;
}

// The SPATIAL denoiser for REFLECTIONS. There was none: the reflection history was reprojected and
// blended in time and never filtered in space at all, while the shadow beside it had a spatial
// filter from the start. That asymmetry was harmless while the reflection ray was a mirror -- a
// mirror ray is deterministic, so there was no variance to remove -- and stops being harmless the
// moment rtReflection widens into a lobe. The lobe and this filter are one change; shipping the
// lobe without this would trade a wrong-but-clean reflection for a right-but-noisy one.
//
// STRUCTURE IS DELIBERATELY rtShadowSpatial's, down to the reprojected gather centre and the
// plane-distance rejection, because the reasoning is identical and is argued at length there --
// read that function's comments first. Three things genuinely differ:
//
//  1. THE RADIUS COMES FROM ROUGHNESS, not from a host constant. It has to: the width of the thing
//     being estimated is the width of the lobe, so a filter of fixed width is either blurring a
//     mirror or under-filtering a rough surface. At roughness 0 this returns the centre untouched
//     without loading a single texel, which is what keeps glass and chrome sharp. It is also why
//     this needs no new dial, no tier-ladder entry and no C++ change -- see Settings::
//     rtShadowDenoise, whose *ForQuality returns 0 at every tier including Epic, for what a dial
//     that nobody sets is worth.
//
//  2. A NEIGHBOUR THAT MISSED IS SKIPPED, not counted as black. gRtReflHist stores a NEGATIVE alpha
//     as its miss sentinel (see the texture's own declaration); averaging those in would drag the
//     edge of every reflected object toward black, which is the exact artefact the sentinel exists
//     to prevent in the temporal path.
//
//  3. THERE IS NO LUMINANCE WEIGHT, and that is a decision rather than an omission. SVGF's
//     colour-similarity term is driven by a per-pixel VARIANCE estimate, which tells it whether a
//     luminance difference is an edge or is noise. This engine tracks no variance -- the history is
//     (rgb, depth) with no second moment and no room for one -- and a luminance weight without
//     variance cannot tell those apart, so it preserves precisely the noise it was added to remove.
//     Geometry weights only, until something stores moments.
//
// A FOURTH THING NO LONGER DIFFERS, as of the AVER_GBUFFER_HISTORY crease term inside the tap loop
// below: it is copied from rtShadowSpatial's OWN crease term verbatim, same threshold, same
// reasoning (a reflected surface creases exactly the way a shadowed one does, and this filter's own
// item 1 above already argues the two share a design whenever the underlying problem is the same
// one) -- see that function's own comment for why cos(60 deg) and not something tighter.
//
// `N` IS A NEW PARAMETER, added only for the crease term. It needs no derivative and no divergent-
// flow caveat of its own -- unlike `dzdx`/`dzdy`, it is simply the surface normal the caller already
// built (rtReflectionTemporal's own `N` parameter, unchanged, passed straight through at both call
// sites below) rather than a value this function would otherwise have to recompute or approximate.
float3 rtReflectionSpatial(float3 centre, float3 wpos, float3 N, float2 pixel, float curDepth,
                           float rough, float dzdx, float dzdy) {
    // Radius tracks the lobe: tan(cone) = rough*rough, so the reflected image's blur grows with the
    // square of roughness while this grows linearly -- deliberately conservative, because a kernel
    // wider than the lobe smears real detail and a kernel narrower than it merely leaves some noise
    // for the temporal history to finish. Capped at 3 (a 7x7 gather) because the cost is quadratic
    // in the radius and the caller stops asking for reflections not far above where this saturates.
    const int radius = (int)clamp(floor(rough * 6.0), 0.0, 3.0);
    if (radius <= 0 || gRtHistParams.y < 0.5) return centre;

    float texW, texH;
    gRtReflHist.GetDimensions(texW, texH);

    // Gather around where this pixel WAS last frame -- gRtReflHist is last frame's texture. Same
    // arithmetic as rtReprojectHistory and rtShadowSpatial, including the two landmines documented
    // there: last frame's VIEWPORT rect rather than [0,1] of the whole texture, and floor rather
    // than round.
    float2 centrePx = pixel;
    const float4 pclip = mul(float4(wpos, 1.0), gPrevViewProj);
    if (pclip.w > 1e-4) {
        const float3 pndc = pclip.xyz / pclip.w;
        if (pndc.z >= 0.0 && pndc.z <= 1.0)
            centrePx = gSceneViewport.xy +
                       float2(pndc.x * 0.5 + 0.5, 0.5 - pndc.y * 0.5) * gSceneViewport.zw;
    }
    const int2 base = int2(floor(centrePx));

    // A GAUSSIAN falloff, where rtShadowSpatial uses a flat box. Every accepted neighbour counting
    // equally makes the kernel a box filter, whose frequency response rings -- visible as the faint
    // square-edged plateaus a box blur leaves around a bright highlight. sigma = radius/2 puts the
    // kernel's useful support at about the radius asked for, so widening the radius widens the blur
    // smoothly instead of stepping it.
    const float sigma2 = max((float)radius * 0.5, 0.5);
    const float inv2s2 = 1.0 / (2.0 * sigma2 * sigma2);

    float3 acc  = centre;
    float  wsum = 1.0;
    [loop] for (int oy = -radius; oy <= radius; ++oy) {
        [loop] for (int ox = -radius; ox <= radius; ++ox) {
            if (ox == 0 && oy == 0) continue;
            const int2 t = base + int2(ox, oy);
            if (any(t < 0) || t.x >= (int)texW || t.y >= (int)texH) continue;
            const float4 st = gRtReflHist.Load(int3(t, 0));
            if (st.a <= 0.0) continue;   // that neighbour's ray missed; see (2) above
            const float predicted = curDepth + dzdx * (float)ox + dzdy * (float)oy;
            const float tol = max(abs(predicted), 1.0) * 0.02 + 1.0;
            if (abs(st.a - predicted) > tol) continue;
#if AVER_GBUFFER_HISTORY
            // THE CREASE TERM -- copied from rtShadowSpatial's identical block; see that function's
            // own comment for the full reasoning (a depth-plane test alone cannot see a normal
            // discontinuity) and for why cos(60 deg). ASSUMES gGBufNormalHist is the same resolution
            // as gRtReflHist, the identical assumption made against gRtShadowHist where the texture
            // itself is declared -- both histories are sized against the same scene render target.
            const float3 nb = gGBufNormalHist.Load(int3(t, 0)).xyz * 2.0 - 1.0;
            if (dot(N, nb) < 0.5) continue;
#endif
            const float w = exp(-(float)(ox * ox + oy * oy) * inv2s2);
            acc  += st.rgb * w;
            wsum += w;
        }
    }
    return acc / wsum;
}

// Tiled/temporal wrapper around rtReflection(), mirroring rtShadowTemporal's structure exactly and
// sharing its tile schedule -- same tileBits, same frameIdx, same per-pixel turn -- so a pixel's
// shadow ray and reflection ray amortise on the same cadence instead of needing two separate
// knobs. Called ONLY when the caller has already gated on roughness (s.rough <= 0.5 in
// PSMainVoxi): a pixel that never qualifies for a reflection ray at all has nothing here to tile
// or reproject, and this function does not re-check that gate.
//
// `hit` means the same thing it does for rtReflection() -- true when there is a real reflection
// colour to use, false when the caller should fall back to the sky. It is true both for a fresh
// hit this frame AND for a reused hit from history (haveHist is already conditioned on the stored
// sample being a real hit, never a miss -- see rtReprojectReflection).
float3 rtReflectionTemporal(float3 wpos, float3 N, float3 R, float3 L, float2 pixel, float rough,
                            float dzdx, float dzdy, out bool hit) {
    // ---- THE MIRROR CUTOFF: ONE predicate, three consumers ----
    //
    // Below AVER_REFL_MIRROR_ROUGH a surface is a mirror and is treated as one throughout: no
    // jitter, no temporal history, no spatial filter. tan(cone) = rough^2, so at 0.1 the ray is
    // displaced by one part in a hundred of its own length -- under a pixel for anything close
    // enough to read detail in. There is no variance there for a filter to remove.
    //
    // THIS IS NOT A TUNING KNOB, IT IS A CORRECTNESS FIX, and it is worth saying what it fixes
    // because the first version of this function had the bug. That version gated the temporal blend
    // on `rough > 0.0`, while its own comment claimed a smooth surface "still takes the fresh value
    // outright". Those disagree for every near-mirror: glass authored at roughness 0.05 and the
    // --refl-test mirror at 0.03 are both greater than zero, so both got an 85%-history blend --
    // which cannot reduce a variance that is already zero, and can only add lag. A pane of glass
    // would have smeared its reflection behind a moving camera for no benefit whatsoever.
    //
    // Deriving all three behaviours from ONE value is the point: a lobe that is jittered but not
    // filtered is noise, and a filter applied to an unjittered ray is blur. They must agree, and the
    // only way to guarantee that is for them to read the same number.
    const float lobeRough = rough < AVER_REFL_MIRROR_ROUGH ? 0.0 : rough;

    // NO HISTORY TEXTURE AT ALL: no accumulation is possible, so the lobe is not opened either.
    // Widening the cone with nothing to converge it trades a biased-but-stable reflection for an
    // unbiased one that flickers every frame, which is strictly worse to look at. Passing rough = 0
    // here reduces rtReflection to the exact mirror ray it traced before this change, so this
    // configuration is bit-for-bit what it always was.
    if (gRtHistParams.x < 0.5) return rtReflection(wpos, N, R, L, pixel, 0.0, 0.0, hit);

    const float4 curClip = mul(float4(wpos, 1.0), gViewProj);
    const uint frameIdx  = (uint)gRtHistParams.z;
    // The per-frame rotation that turns one ray per frame into a converging estimate of the lobe.
    // A pure per-frame count, never wall-clock (gRtHistParams.z's own contract), so a capture at
    // frame N is reproducible; the golden-angle multiplier keeps successive frames' rotations from
    // landing near each other the way a fixed increment would.
    const float frameJitter = (float)frameIdx * 2.39996323;
    const uint tileBits = (uint)gRtHistParams.w;

    if (tileBits == 0u) {
        // THE SHIPPED PATH: rtPixelsPerRayTileForQuality returns 1 at every tier, so tileBits is 0
        // everywhere and this is the branch that actually runs. It used to trace, write and return
        // with NO temporal blend of any kind -- correct while the ray was a deterministic mirror,
        // because there was no variance for a blend to reduce, and wrong the moment the lobe opened.
        //
        // So the blend is added HERE, and only where variance was actually introduced: a smooth
        // surface (rough = 0, hence tanCone = 0, hence one deterministic ray) still takes the fresh
        // value outright and this branch stays exactly what it was for glass, chrome and water.
        bool curHit;
        float3 fresh = rtReflection(wpos, N, R, L, pixel, lobeRough, frameJitter, curHit);
        float3 col = fresh;

        if (lobeRough > 0.0 && curHit) {
            float3 hist = 0.0;
            float2 velocityPx = 0.0;
            if (gRtHistParams.y > 0.5 && rtReprojectReflection(wpos, pixel, hist, velocityPx)) {
                // Same velocity-discounted shape rtShadowTemporal and the tiled path below use, and
                // for the same reason: a reprojected sample that has slid a long way across the
                // screen is on the same surface but not at the same point of it, and trusting it
                // fully is what smears a reflection into a comet tail behind a moving camera. Still
                // camera, full weight and the lobe converges in a handful of frames; fast pan, no
                // weight and it falls back to this frame's single sample plus the spatial filter.
                const float t = saturate(length(velocityPx) / 6.0);
                col = lerp(hist, fresh, lerp(0.15, 1.0, t));
            }
        }

        // WRITE THE RAW TEMPORAL VALUE, NEVER THE SPATIALLY FILTERED ONE -- the identical rule
        // rtShadowTemporal states at its own history write. Feeding a filtered value back in makes
        // the spatial pass a compounding IIR filter that widens without bound over frames, which
        // looks like the reflection slowly dissolving.
        gRtReflHistOut[uint2(pixel)] = curHit ? float4(col, curClip.w) : float4(0.0, 0.0, 0.0, -1.0);
        hit = curHit;
        return curHit ? rtReflectionSpatial(col, wpos, N, pixel, curClip.w, lobeRough, dzdx, dzdy) : col;
    }

    const uint tileMask = (1u << tileBits) - 1u;
    const uint turnMask  = (1u << (2u * tileBits)) - 1u;
    const uint localIdx  = (((uint)pixel.y & tileMask) << tileBits) | ((uint)pixel.x & tileMask);
    const bool myTurn    = localIdx == (frameIdx & turnMask);

    float3 hist = 0.0;
    float2 velocityPx = 0.0;
    const bool haveHist = gRtHistParams.y > 0.5 && rtReprojectReflection(wpos, pixel, hist, velocityPx);

    float3 col;
    bool curHit;
    if (myTurn || !haveHist) {
        col = rtReflection(wpos, N, R, L, pixel, lobeRough, frameJitter, curHit);
        // Same adaptive-weight shape as rtShadowTemporal -- see its comment for the reasoning.
        // Only blends a HIT with history: a fresh miss stays a miss (the caller's own sky fallback
        // already handles that correctly) rather than being dragged toward a stale hit colour.
        if (curHit && haveHist) {
            const float budget = max(6.0 - 1.5 * (float)tileBits, 1.0);
            const float t = saturate(length(velocityPx) / budget);
            const float weight = lerp(0.9, 0.1, t);
            col = lerp(col, hist, weight);
        }
    } else {
        // Not this pixel's turn, and reprojection found a real hit: reuse it outright. No ray at
        // all this frame -- this is the actual saving tiling exists for.
        col = hist;
        curHit = true;
    }

    hit = curHit;
    // Raw, not filtered -- see the untiled branch's own note on why feeding the spatial result back
    // into the history makes it a compounding filter.
    gRtReflHistOut[uint2(pixel)] = curHit ? float4(col, curClip.w) : float4(0.0, 0.0, 0.0, -1.0);
    return curHit ? rtReflectionSpatial(col, wpos, N, pixel, curClip.w, lobeRough, dzdx, dzdy) : col;
}
#endif

// 3x3 PCF inside ONE cascade's quadrant of the atlas. Returns 1 = lit, 0 = shadowed, -1 = outside
// this cascade so the caller can try the next one.
float shadowSampleCascade(float3 wpos, uint c) {
    float4 lp = mul(float4(wpos, 1.0), gCascadeViewProj[c]);
    float3 p = lp.xyz / lp.w;
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || p.z > 1.0 || p.z < 0.0) return -1.0;

    // The 2x2 atlas layout: quadrant x = c&1, y = c>>1. Must match the C++ side.
    float2 quad = float2(c & 1u, c >> 1u) * 0.5;
    float inset = gShadowParams.x;
    uv = quad + clamp(uv * 0.5, float2(inset, inset), float2(0.5 - inset, 0.5 - inset));

    float s = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
        s += gShadowTex.SampleCmpLevelZero(gShadowSamp, uv + float2(x, y) * gShadowParams.x, p.z);
    return s / 9.0;
}

// Where shadowing starts fading to unshadowed, as a fraction of the last cascade's reach.
#define AVER_SHADOW_FADE_START 0.84

// Picks a cascade by distance and samples it, offsetting along N by that cascade's bias. Returns
// sun visibility, 1 = fully lit.
float shadowFactor(float3 wpos, float3 N, float ndl) {
    if (gShadowParams.y < 0.5) return 1.0;
    uint count = (uint)gShadowParams.w;
    if (count == 0) return 1.0;
    count = min(count, (uint)AVER_SHADOW_CASCADES);

    float slope = saturate(1.0 - ndl);
    float dist  = distance(wpos, gCamPos.xyz);

    float fadeSpan = max(gCascadeSplit[count - 1].x * (1.0 - AVER_SHADOW_FADE_START), 1e-3);
    float fade = saturate((dist - gCascadeSplit[count - 1].x * AVER_SHADOW_FADE_START) / fadeSpan);

    [loop] for (uint c = 0; c < count; ++c) {
        if (dist > gCascadeSplit[c].x) continue;
        float bias = gCascadeSplit[c].y * (1.0 + slope);
        float s = shadowSampleCascade(wpos + N * bias, c);
        if (s >= 0.0) return lerp(s, 1.0, fade);
    }
    return 1.0;
}

// Sun visibility for LIGHT INJECTION, from the GI-only shadow map.
//
// A SEPARATE FUNCTION FROM shadowFactor BECAUSE IT ANSWERS A DIFFERENT QUESTION. shadowFactor asks
// "is this PIXEL in shadow", picks a cascade by distance from the CAMERA, and fades out past the
// last one -- all correct for something being drawn on screen. A voxel is not on screen. It exists
// wherever the GI volume is, the volume does not move with the camera, and a voxel the camera is
// not looking at must still be shadowed correctly or the bounce light it contributes is wrong.
//
// Feeding voxels through the cascades is what forced fitCascades to union its last cascade with the
// whole GI volume, which cost ~14x the area and handed that cascade every draw in the scene. One box
// over the volume answers it directly: no cascade selection, no camera distance, no fade.
float giShadowFactor(float3 wpos, float3 N, float ndl) {
    // Unusable map: fully lit. The volume is still injected, just without sun occlusion -- the same
    // degradation shadowFactor performs when the atlas is missing, and the reason giShadowTex_ is a
    // soft dependency rather than an init() failure.
    if (gGiShadowParams.y < 0.5) return 1.0;

    float slope = saturate(1.0 - ndl);
    float3 p0 = wpos + N * (gGiShadowParams.z * (1.0 + slope));

    float4 lp = mul(float4(p0, 1.0), gGiShadowViewProj);
    float3 p = lp.xyz / lp.w;
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    // OUTSIDE THE BOX IS LIT, NOT SHADOWED. The box covers the whole GI volume by construction, so
    // landing outside it means the voxel is outside the volume too and its radiance is never read.
    if (any(uv < 0.0) || any(uv > 1.0) || p.z > 1.0 || p.z < 0.0) return 1.0;

    // No atlas quadrant to inset into: this map is one box filling the whole texture.
    float t = gGiShadowParams.x;
    float s = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
        s += gGiShadowTex.SampleCmpLevelZero(gShadowSamp, uv + float2(x, y) * t, p.z);
    return s / 9.0;
}

// Mip level being read by CSMip (b3: b0/b1 are taken by the graphics root signature).
cbuffer MipCB : register(b3) { uint gSrcMip; uint3 _mipPad; };

// world -> [0,1] volume coords
float3 voxelUVW(float3 wp) { return (wp - gVoxelOrigin.xyz) * gVoxelOrigin.w; }
bool insideVolume(float3 uvw) { return all(uvw >= 0.0) && all(uvw <= 1.0); }

// ---- cone tracing ----
// Marches a cone through the volume, widening with distance and reading a coarser mip each step.
// Returns front-to-back composited, premultiplied radiance; alpha is coverage.
float4 traceCone(float3 originWS, float3 dir, float aperture) {
    float voxelWorld = 1.0 / (gVoxelOrigin.w * gVoxelParams.x); // one voxel, world units
    float dist = voxelWorld * 2.0;
    float4 acc = 0;
    [loop] for (int step = 0; step < 24; ++step) {
        if (acc.a >= 0.95 || dist > gVoxelParams.z) break;
        float diameter = max(voxelWorld, 2.0 * aperture * dist);
        float mip = log2(diameter / voxelWorld);
        float3 uvw = voxelUVW(originWS + dir * dist);
        if (!insideVolume(uvw)) break;
        float4 s = gVoxelTex.SampleLevel(gVoxelSamp, uvw, mip);
        acc += (1.0 - acc.a) * s;
        dist += diameter;
    }
    return acc;
}

// Cosine-weighted gather of six cones over the hemisphere: one along the normal, five in a ring.
// Returns the indirect diffuse radiance and, through `ao`, the ambient occlusion.
float3 coneTracedIndirect(float3 wpos, float3 N, out float ao) {
    float3 up = abs(N.z) < 0.9 ? float3(0,0,1) : float3(1,0,0);
    float3 T = normalize(cross(up, N)), B = cross(N, T);
    const float aperture = 0.577;              // ~60 degree cone

    float4 sum = traceCone(wpos, N, aperture);
    float occ = sum.a;
    float wsum = 1.0;
    // THE RING COMES FROM THE QUALITY TIER NOW, not from a hardcoded 5. The axial cone along N is
    // always traced, so gGiParams.x is the TOTAL and the ring is one fewer.
    //
    // A DYNAMIC [loop], not the [unroll] this was: an unrolled loop with a dynamic bound is
    // predicated rather than skipped, so every tier would still pay for six cones and the ladder
    // would be a lie. That is the whole reason this is worth the loop overhead.
    //
    // The angle is 2*pi/ring computed rather than the 1.2566 literal that stood here. That literal
    // was a rounded 2*pi/5, so the six-cone case shifts by about 3e-5 radians and is no longer
    // bit-identical to before -- in the more correct direction.
    const uint  ring = (uint)max(gGiParams.x, 1.0) - 1u;
    const float dphi = ring > 0u ? 6.2831853 / (float)ring : 0.0;
    [loop] for (uint k = 0; k < ring; ++k) {
        float ang = dphi * (float)k;
        float3 d = normalize(N * 0.5 + (T * cos(ang) + B * sin(ang)) * 0.866);
        float w = saturate(dot(N, d));
        float4 c = traceCone(wpos, d, aperture);
        sum += c * w; occ += c.a * w; wsum += w;
    }
    sum /= wsum; occ /= wsum;
    ao = saturate(1.0 - occ);
    // THE SAME CEILING THE INJECTION ALREADY HAS, applied AFTER the intensity multiply. Every
    // voxel this cone gathered was clamped to AVER_VOX_MAXRAD on the way in (see PSVoxel), so
    // `sum` cannot exceed it either -- and then gVoxelParams.y, the authored giIntensity, is let
    // as high as 8 (Voxi.cpp clamps it there), which multiplies a bounded quantity straight back
    // out of its bound: 16 x 8 = 128 units of radiance, out of a volume in which nothing emits
    // more than 16.
    //
    // That is the runaway Voxi.hpp documents -- enough large, saturated, brightly-lit geometry and
    // the bounce floods the frame with that geometry's colour. The bound is a PHYSICAL statement
    // rather than a tuned number (a gather cannot hand back more radiance than the brightest thing
    // it gathered from emits), which is why it reuses the injection's own constant instead of
    // introducing a second one that would then have to be kept in step with it.
    //
    // A CLAMP IS NOT A LIGHTING MODEL. This stops a divergence; it does not make the answer right
    // at the ceiling, and a scene that reaches it is still asking for more light than the volume
    // holds. It fires in nothing shipped with this engine -- ElectricDreams, the gate scene and
    // the furnace were all measured bit-identical across this change.
    return min(sum.rgb * gVoxelParams.y, AVER_VOX_MAXRAD);
}

// ================= additive G-buffer: velocity, view-space depth, normal+roughness =================
// Gated on AVER_GBUFFER, following this file's AVER_RT convention exactly: a compile-time define,
// undefined (hence 0 under HLSL's #if) everywhere except a variant the host explicitly compiles with
// "AVER_GBUFFER=1" appended to its define string -- never a runtime branch -- so PSMainVoxi's and
// PSRayDriven's ORIGINAL variants (SV_TARGET alone, or SV_TARGET+SV_DEPTH) fall through the #else
// branches below UNCHANGED and compile to the identical shader they always did. THIS IS WHAT MAKES
// THE FEATURE ADDITIVE: with the define off, the frame this file produces must be bit-identical to
// today's, and the render gate oracle (18 gates x 9 configurations) is what would notice if it were
// not.
//
// WHY THIS EXISTS AT ALL: nothing in this engine produces motion vectors, and there is no G-buffer --
// PSMainVoxi returns one SV_TARGET, and normal/roughness/albedo live only in this shader's own
// registers, never in a texture a compute pass could read. That single gap blocks the vendored
// FidelityFX denoiser (third_party/fidelityfx-denoiser/README.md lists the exact host callbacks it is
// missing), FSR 2/3, TAA and screen-space reflections all at once, and it is also WHY temporal
// reprojection is wrong for moving geometry today: rtReprojectHistory and its siblings transform THIS
// frame's wpos through LAST frame's camera, which only holds for a surface that did not move.
//
// THIS SLICE ONLY WRITES THE TARGETS -- nothing in this file reads them back. Wiring a consumer (the
// FFX denoiser, FSR3, TAA) is later work, stated as out of scope here on purpose: a target that is
// written and never read is precisely the shape this codebase keeps producing by accident (declared
// but unread, entry after entry), and the point of saying so here is that this one is unread
// DELIBERATELY, for now, not by omission.
//
// ---- HOW TO DECODE EACH CHANNEL -- stated here because the encode (this file) and whatever decodes
// it later (a denoiser or upscaler module, on the other side of a module boundary) will drift apart
// the moment only one side remembers the contract ----
//   SV_TARGET1 velocity:        RG16F. Texels/frame, DESTINATION (this frame's) texel minus SOURCE
//                                (last frame's) texel for the same surface point -- read back as-is,
//                                no scale or bias: `prevPixel = thisPixel - velocity`. Matches
//                                rhi::UpscalerNeeds::MotionVectors' own documented convention exactly
//                                (RHIResources.hpp).
//   SV_TARGET2 viewZ:           R32F. VIEW-SPACE LINEAR depth -- clip.w from
//                                mul(float4(wpos,1), gViewProj) -- NOT the post-projective [0,1]
//                                SV_Position.z/SV_DEPTH value a hardware depth buffer stores. Read
//                                back as-is; it is already linear, in the same world units as wpos.
//   SV_TARGET3 normalRoughness: RGB10A2. xyz = world-space shading normal * 0.5 + 0.5 (decode as
//                                N = xyz*2-1, unit length up to 10-bit-per-channel quantisation);
//                                w = roughness, unscaled. THE ALPHA CHANNEL IS TWO BITS -- four
//                                representable levels -- which is the format this feature was
//                                specified against, not a choice made in this file; a consumer that
//                                needs finer roughness than four steps has to read it from the
//                                material's own SRV instead of this channel. Stated here rather than
//                                left for someone to discover as unexplained banding.
#if AVER_GBUFFER
struct GBufferOut {
    float4 col              : SV_TARGET0;   // exactly PSMainVoxi's own colour -- unchanged by this define
    float2 velocity         : SV_TARGET1;
    float  viewZ            : SV_TARGET2;
    float4 normalRoughness  : SV_TARGET3;
};

// Packs a world-space unit normal and a roughness into the RGB10A2 normalRoughness convention above.
// Shared by PSMainVoxi and PSRayDriven so the encode is written once instead of risking the two
// diverging on which remap they used.
float4 averPackNormalRoughness(float3 N, float roughness) {
    return float4(N * 0.5 + 0.5, saturate(roughness));
}

// Screen-space motion for the velocity channel: `wpos` reprojected through THIS frame's camera minus
// the SAME wpos reprojected through LAST frame's, mapped into the SCENE VIEWPORT RECT the identical
// way rtReprojectHistory/rtShadowSpatial/rtReprojectReflection already do -- see rtReprojectHistory's
// own comment for the landmine this inherits rather than re-derives: the editor docks the 3D view in
// a sub-rect of the backbuffer (gSceneViewport), and a plain ndc*0.5+0.5 implicitly assumes the
// viewport covers the entire render target.
//
// STATIC-GEOMETRY ONLY, AND THIS IS A STATED, DELIBERATE GAP RATHER THAN AN OVERSIGHT. `wpos` stands
// in for the SAME world point in both projections, which is only correct if the underlying surface
// did not move between frames -- precisely the assumption this feature's own module comment (above)
// says is broken for temporal reprojection today. A genuinely moving instance needs its OWN
// previous-frame object-to-world transform: reproject the vertex's OBJECT-space position through a
// previous `gWorld` (raster) or a previous `RtInstance.objectToWorld` (ray-driven) instead of reusing
// this frame's wpos for both terms. NEITHER EXISTS YET as this is written: VSOut/the per-draw cbuffer
// carry no previous gWorld, and RtInstance (this file, above -- `struct RtInstance { float4x4
// objectToWorld; ... }`) carries exactly one transform and no previous one, so there is nothing
// reachable from here to consume. The consequence: this function reports real, correct motion for a
// STATIONARY object under a MOVING camera (the same case rtReprojectHistory/rtShadowSpatial/
// rtReprojectReflection already need fixed for their own disocclusion tests), and reports ZERO motion
// for an object that is itself animating -- SILENTLY, which is exactly the failure mode worth naming
// rather than leaving implicit. A consumer that trusts this channel for a moving character's own
// reprojection will reuse stale history under it precisely the way today's shadow/reflection temporal
// filters already do for the same reason, until a previous-transform is threaded through RtInstance
// (or the per-draw cbuffer) and this function is updated to reproject through it instead of through
// `wpos` twice.
float2 averGBufferVelocity(float3 wpos) {
    const float4 curClip  = mul(float4(wpos, 1.0), gViewProj);
    const float4 prevClip = mul(float4(wpos, 1.0), gPrevViewProj);
    // Either transform can put this point behind its own near plane -- prevClip routinely does (the
    // very first frame gPrevViewProj is whatever it was last cleared to, and anything that just
    // entered the frustum has no meaningful "last frame" position at all). Zero is "no motion known",
    // the same fallback rtReprojectHistory leaves its own velocityPx at when it returns false: a
    // consumer already has to treat this channel as advisory, and zero is the least wrong single
    // answer when the maths behind it is not defined, rather than a divide that hands back Inf/NaN.
    if (curClip.w <= 1e-4 || prevClip.w <= 1e-4) return float2(0.0, 0.0);

    const float2 curNdc  = curClip.xy  / curClip.w;
    const float2 prevNdc = prevClip.xy / prevClip.w;
    const float2 curPx  = gSceneViewport.xy +
                          float2(curNdc.x * 0.5 + 0.5, 0.5 - curNdc.y * 0.5) * gSceneViewport.zw;
    const float2 prevPx = gSceneViewport.xy +
                          float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5) * gSceneViewport.zw;
    // DESTINATION (this frame, curPx) minus SOURCE (last frame, prevPx) -- see this function's own
    // header and UpscalerNeeds::MotionVectors' declaration (RHIResources.hpp) for why that order, not
    // the reverse, is the contract a consumer is entitled to assume.
    return curPx - prevPx;
}

// One expansion point for every one of PSMainVoxi's several `return` statements, so the three extra
// channels are populated identically at each of them instead of by hand at each site, where a future
// edit could update the colour at one site and forget one of the other three. `gbufVelocity`/
// `gbufViewZ`/`gbufNormalRough` are computed exactly once, further down, right after `s` (the
// AverSurface every return site below already has in scope) is built -- this macro only ever
// assembles values that already exist, never recomputes them per call site.
#define AVER_GBUF_RETURN(colorExpr) \
    { GBufferOut aver_gbuf_o; aver_gbuf_o.col = (colorExpr); aver_gbuf_o.velocity = gbufVelocity; \
      aver_gbuf_o.viewZ = gbufViewZ; aver_gbuf_o.normalRoughness = gbufNormalRough; return aver_gbuf_o; }
#else
// Disabled: PSMainVoxi's own `return` sites expand to a plain return, exactly what stood at each site
// before this feature existed -- see the #if branch above for what they become when it is on.
#define AVER_GBUF_RETURN(colorExpr) return (colorExpr)
#endif

// The Voxi lit pixel shader. Voxi supplies light transport only — sun visibility, sky, bounce —
// and the material shades it. Returns linear radiance; the post chain tonemaps.
#if AVER_GBUFFER
GBufferOut PSMainVoxi(VSOut i) {
#else
float4 PSMainVoxi(VSOut i) : SV_TARGET {
#endif
    float3 N = normalize(i.nrmWS);
    float3 L = normalize(gLightDir.xyz);
    float ndl = saturate(dot(N, L));
#if AVER_RT
    // View-space depth and its screen-space gradient, taken HERE -- at the top of the shader, where
    // control flow is still uniform -- and carried down to the reflection block far below. That
    // block is gated on this pixel's own roughness, and a derivative taken inside divergent flow is
    // undefined in HLSL: the same landmine rtShadow's `dpx`/`dpy` parameters exist to step around,
    // documented at length there. rtShadowSpatial can afford its own ddx because its caller is gated
    // on uniform state only; the reflection path cannot, and the failure would present as
    // adapter-specific corruption rather than as anything resembling its cause.
    const float rtViewZ = mul(float4(i.wpos, 1.0), gViewProj).w;
    const float rtDzdx  = ddx(rtViewZ);
    const float rtDzdy  = ddy(rtViewZ);

    // float3 NOW: rtShadowTemporal carries the medium's colour. shadowFactor returns a scalar and
    // promotes, so the non-RT path is unchanged.
    float3 sunVis;
    if (gShadowParams.z > 0.5)
        sunVis = rtShadowTemporal(i.wpos, N, L, i.pos.xy, ddx(i.wpos), ddy(i.wpos),
                                  (uint)max(gRtParams.y, 1.0));
    else                       sunVis = shadowFactor(i.wpos, N, ndl);
#else
    const float3 sunVis = shadowFactor(i.wpos, N, ndl);
#endif
#if AVER_GBUFFER
    // View-space linear depth for this pixel's G-buffer entry. REUSED, not recomputed, when AVER_RT
    // already produced the identical value a few lines up (rtViewZ) for the reflection block's own
    // screen-space derivatives -- the two branches above already agree on how to compute it
    // (mul(wpos,1,gViewProj).w), so there is nothing to gain by asking twice.
#if AVER_RT
    const float gbufViewZ = rtViewZ;
#else
    const float gbufViewZ = mul(float4(i.wpos, 1.0), gViewProj).w;
#endif
#endif
    float ao = 1.0;
    float3 ind = 0;
    if (gVoxelParams.w > 0.5) ind = coneTracedIndirect(i.wpos, N, ao);

    AverVertex vtx = averVertexOf(i);

    // ---- a single-sided BLENDED surface draws ONE layer, not every face it owns ----
    //
    // THE SCENE PIPELINE DOES NOT CULL. `scene.cull = rhi::CullMode::None` (VoxiRenderer.cpp:3133)
    // and every blended twin is built from that desc without reassigning it, so a closed volume
    // rasterises ALL of its faces. For an OPAQUE mesh that is invisible -- the depth test discards
    // the far ones -- but a blended draw writes no depth, so each surviving face composites its own
    // alpha and the layers multiply. A box of water therefore came out roughly four coats thick:
    // measured, a surface authored at alpha 0.02 still DARKENED the pit behind it, from (50.7, 51.4,
    // 49.2) with no water at all to (36.6, 41.9, 46.1). A single 2% layer cannot do that; four can.
    //
    // GATED ON THE AUTHORED FLAG, WHICH IS THE WHOLE POINT. `twosided` has been parseable since the
    // format existed and no shader has ever read it (grep AVER_MAT_TWO_SIDED: this is its first
    // consumer). M_Glass sets twosided=1 deliberately -- "glass is routinely CULL none, because you
    // walk round it", as the TIR comment further down this file puts it -- so a pane is untouched by
    // this and keeps compositing both of its faces exactly as it does today. A water volume sets
    // twosided=0 and gets one layer. The author decides; the renderer stops overriding them.
    //
    // BLENDED ONLY, deliberately narrow. Opaque geometry already gets correct single-layer results
    // from the depth test, and single-sided opaque content drawn with culling off (a floor seen from
    // below, a foliage sheet) has relied on CullMode::None for as long as it has existed. Restricting
    // the discard to the blended lane fixes exactly what is broken and can regress nothing else.
    //
    // dot(N, V), not SV_IsFrontFace: averVertexOf already computes it that way and states why, and
    // this reuses that answer rather than adding a second, differently-derived notion of "backwards"
    // that could disagree with the normal flip sitting immediately beside it.
    // INVERTED WHEN THE EYE IS INSIDE THE VOLUME, NOT SWITCHED OFF, and the difference is the whole
    // reason this is safe.
    //
    // Seen from within a closed volume every face points away from the eye, so every face is
    // backFace and the discard above deleted the surface outright -- which is why swimming under the
    // pool showed no water surface at all, only the concrete beyond it. What is wanted from below is
    // still exactly ONE layer: a ray leaving the eye inside a convex volume crosses the boundary
    // once, so keeping the back faces and dropping the front ones is the same "one coat" rule read
    // from the other side.
    //
    // gCameraMedium.x comes from a bounding-SPHERE test and is therefore loose -- a wide shallow
    // pool's sphere bulges above its own surface. Inverting rather than disabling is what makes that
    // acceptable: a false positive standing on the deck drops the near face and keeps the far one,
    // so the pixel still gets ONE layer of water, not two. Turning the discard off instead would
    // composite both and resurrect the four-coats bug this whole block exists to prevent. Do not
    // "simplify" this into an early-out.
    const bool eyeInside = gCameraMedium.x > 0.5;
    if ((gMaterialFlags & AVER_MAT_ALPHA_BLEND) && !(gMaterialFlags & AVER_MAT_TWO_SIDED) &&
        (eyeInside ? !vtx.backFace : vtx.backFace))
        clip(-1);

    AverLight sun;
    sun.direction  = L;
    sun.radiance   = averSunRadiance();
    sun.visibility = sunVis;

    AverSurface s = averEvalMaterial(vtx, sun);
#if AVER_GBUFFER
    // Velocity and the packed normal/roughness are computed once here -- after `s` exists but before
    // this function's first `return` -- so every AVER_GBUF_RETURN site below shares identical values
    // instead of each recomputing its own copy under whatever branch happened to be taken.
    // averGBufferVelocity's own comment states plainly what this DOES NOT yet handle: a moving
    // instance's own motion, versus camera motion over static geometry.
    const float2 gbufVelocity    = averGBufferVelocity(i.wpos);
    const float4 gbufNormalRough = averPackNormalRoughness(averShadingNormal(s), s.rough);
#endif
    float4 display;
    if (averDisplayColour(s, display)) AVER_GBUF_RETURN(display);

    float3 V = normalize(gCamPos.xyz - i.wpos);
    float3 R = reflect(-V, averShadingNormal(s));
    AverIndirect ind4;
    ind4.ambient      = averSkyIrradiance(averShadingNormal(s));
    ind4.ambientScale = gAmbient.r;
    ind4.diffuse      = ind;
    ind4.occlusion    = ao;
#if AVER_RT
    // Ray traced when the acceleration structure and the geometry table are both there. Preferred
    // over the cone trace unconditionally: the cone is bounded by the voxel volume and this is not.
    //
    // ---- THE APERTURE THE OLD COMMENT SAID WAS MISSING ----
    //
    // What stood here said: "A MIRROR RAY IS ONLY RIGHT FOR A SMOOTH SURFACE. The cone path it
    // replaced widened its aperture with roughness; a single ray has no aperture at all, so applying
    // it to a rough surface hands back a sharp reflection where a blurred one belongs." That was an
    // accurate description of a real defect, and the two things it did about it were both
    // concealments rather than fixes: refuse the reflection outright above roughness 0.5, and fade
    // whatever survived toward flat sky at TWICE the roughness. Between them, a surface at roughness
    // 0.25 -- ordinary painted metal, damp stone, a pool coping -- received a mirror reflection at
    // half strength blended with half a flat sky colour, and a surface at 0.5 received no ray at all
    // while paying the roughness test to find that out.
    //
    // rtReflection now takes the roughness and opens a real cone (tan = rough^2, the GGX alpha),
    // rtReflectionTemporal converges it over frames, and rtReflectionSpatial filters it across
    // pixels with a kernel that widens with the same roughness. So the reflection can simply be
    // USED, across the range where a one-ray-per-pixel estimate plus those two filters actually
    // resolves something.
    //
    // WHY 0.75 AND NOT 1.0. Past roughly three-quarters rough, the lobe is wide enough that a single
    // ray per pixel is estimating a near-hemispherical integral, and neither filter can close that:
    // the spatial kernel saturates at radius 3 and the temporal history rejects itself as soon as
    // the camera moves. What a surface that rough reflects is, to a very good approximation, the
    // average of its surroundings -- which is what the cone trace below returns from the voxel
    // volume, and what the sky term returns outdoors. Handing those cases to the path that already
    // answers them well is the right division, and 0.75 is where the ray stops being the better
    // answer rather than where it stops being affordable.
    //
    // The fade is kept, but it is now only a seam-hider across the last quarter of the range instead
    // of the mechanism: it goes from 0 at 0.5 rough to 1 at 0.75, so nothing pops as a surface
    // crosses the cutoff into the cone path, and everything below 0.5 rough is the traced answer at
    // full strength rather than a blend with a flat colour.
    if (gShadowParams.z > 0.5 && gRtParams.w > 0.5 && s.rough <= 0.75) {
        bool specHit = false;
        float3 refl = rtReflectionTemporal(i.wpos, N, R, L, i.pos.xy, s.rough,
                                           rtDzdx, rtDzdy, specHit);
        // ONE skyColor(R), NOT TWO, AND NOW NOT ALWAYS ONE. Both operands of this lerp ask for the
        // same value with the same argument, and skyColor is a 32-step atmosphere march under a
        // physical sky -- each step evaluating a Chapman-function sun transmittance. Naming it once
        // rather than trusting DXC to common-subexpression a term priced that high was the first
        // half of this; the guard is the second.
        //
        // WHEN THE MARCH IS PURE WASTE: smoothstep(0.5, 0.75, rough) is EXACTLY 0 for any surface at
        // or below 0.5 roughness, so for a ray that HIT something the lerp reduces to `refl` and the
        // sky it just marched is multiplied by zero. Water, glass, chrome and wet stone are all in
        // that band, which is exactly the population that grew when water became a blended material:
        // measured on the PTTest pool, the blended replay went 18.9 -> 15.9 ms and the whole frame
        // 44.86 -> 41.78. Nothing on screen changes, because the term removed was multiplied by 0.
        //
        // STILL ONE CALL, AND THAT IS NOT A STYLE POINT. An earlier attempt at this guard split the
        // lerp into two branches that each called skyColor(R), which cost 13% (8.15 -> 9.26 ms): a
        // divergent wave executes both sides, so "avoiding" the march by branching around it ran it
        // twice instead. The shape below has exactly one call site, guarded, and the guard is false
        // only where the result is provably unused.
        const float skyW = smoothstep(0.5, 0.75, s.rough);
        float3 skyR = float3(0.0, 0.0, 0.0);
        if (!specHit || skyW > 0.0) skyR = skyColor(R);
        ind4.specular = lerp(specHit ? refl : skyR, skyR, skyW);
    } else
#endif
    if (gVoxelParams.w > 0.5) {
        float  specAperture = clamp(s.rough * 0.5 + 0.02, 0.02, 0.4);
        float4 sceneSpec    = traceCone(i.wpos, R, specAperture);
        // Bounded for the reason coneTracedIndirect is. The sky term is left alone: skyColor is
        // not a gather out of the volume and carries no runaway of its own.
        // THE SKY TERM IS SKIPPED WHERE THE CONE ALREADY SAW A WALL. HLSL does not short-circuit a
        // multiply: skyColor(R) * (1 - sceneSpec.a) ran the full 32-step atmosphere march even when
        // the cone came back fully occluded and the result was scaled to nothing. This is the
        // rough > 0.75 branch -- concrete, cloth, unpolished stone, which is most of a real scene --
        // so that was the common case paying for a value it then multiplied away.
        //
        // THE SAME SHAPE, AND THE SAME FIX, AS averFogInscatter'S OWN THRESHOLD, whose comment
        // records what it was worth there: "scene draw 8.9ms -> 1.3ms ... 85% of the scene pass and
        // 41% of the entire frame". That one gates on w > 0.01 and argues the skipped contribution is
        // under one 8-bit step, so a pixel crossing the threshold cannot band. The argument holds
        // identically here because the weight is the same kind of quantity -- a [0,1] coverage -- and
        // the term it scales is bounded radiance.
        //
        // 0.004, NOT 0.01, and the difference is deliberate: this weight multiplies a sky that can be
        // far brighter than the fog reference, so the same visual error needs a tighter cutoff. At
        // 0.004 the dropped term is at most 0.4% of a sky sample, which stays under a code at 8 bits
        // for any sky this engine can produce short of the furnace.
        const float skyWeight = 1.0 - sceneSpec.a;
        ind4.specular       = min(sceneSpec.rgb * gVoxelParams.y, AVER_VOX_MAXRAD);
        if (skyWeight > 0.004) ind4.specular += skyColor(R) * skyWeight;
    } else {
        ind4.specular       = skyColor(R);
    }

    // ---- TRANSLUCENT MATERIALS TAKE A SEPARATE, EARLY-RETURNING PATH ----
    //
    // Gated on the flag rather than on averOpacity(s) < 1: alpha is an authored number a shader could
    // legally set to 1 on a pipeline that is still the blended twin (or vice versa on a mostly-clear
    // pane), and this branch's whole reason to exist is which PIPELINE/blend-state the draw is
    // running under, not what value came out of the material graph. AVER_MAT_ALPHA_BLEND is set
    // exactly when the draw was routed to the blended PSOs in VoxiRenderer.cpp -- see
    // scenePipeline()'s own comment -- which is the one thing that actually decides whether this
    // pixel's rgb is about to be composited with PremultipliedAlpha or written straight over an
    // opaque background. Reading the wrong signal here would pack a premultiplied output for a draw
    // the pipeline blends as if it were straight, or vice versa, and the failure is a silent one:
    // every value stays a plausible colour, just wrong by however translucent the surface happens to
    // be that frame.
    if (gMaterialFlags & AVER_MAT_ALPHA_BLEND) {
        // averShadeSplit is averShadeDirect + averShadeIndirect's IDENTICAL arithmetic (see its own
        // contract in PbrShaders.cpp), just kept as two registers instead of summed into one -- so an
        // opaque draw through the branch below is unaffected by this existing at all, and a blended
        // draw gets the same energy an opaque one would, only apportioned between "coverage-weighted"
        // and "always full strength" before averBlendedOutput folds alpha in.
        float3 dif, spc;
        averShadeSplit(s, sun, ind4, dif, spc);
        // rgb = specular + diffuse*alpha, a = alpha -- see averBlendedOutput's own contract. This is
        // the fix the shading contract exists for: straight alpha would have multiplied `spc` by
        // alpha too, so a pane authored at 0.2 opacity showed its own reflection at a fifth strength
        // instead of full. sceneBlendedPso_'s PremultipliedAlpha blend state is what makes packing it
        // this way the correct thing to hand the ROP rather than a number that needs a different
        // blend equation to come out right.
        // THE VOLUME, where one is authored. gAttenuationDistance <= 0 is the off state every
        // material carried before that row existed, so this whole block compiles to a compare and a
        // branch nobody takes for ordinary glass -- and averBlendedOutputVolume reduces to
        // averBlendedOutput exactly at T = 1 regardless, so the two are the same function for a
        // surface with no interior.
        //
        // MEASURED WITH A RAY, NOT DERIVED FROM AN AUTHORED HEIGHT. See averVolumeThickness for why:
        // a fragment on a face has no idea how thick its own body is, and the formula that guessed
        // is what made the PTTest pool a bright opaque slab from every angle but straight down.
        // Behind AVER_RT because the ray is: without ray tracing there is no structure to measure
        // against, so the surface keeps exactly today's volumeless composite rather than a fabricated
        // thickness.
        float4 outc;
#if AVER_RT
        // FRONT FACES ONLY, and this is a correctness gate rather than an optimisation.
        //
        // averVolumeThickness traces from the shaded point ALONG THE VIEW DIRECTION and takes the
        // nearest hit, which is the medium's exit point -- correct when the shaded point is where
        // the ray ENTERS the volume. On a BACK face that same trace starts where the ray LEAVES,
        // so it heads out into the room and measures the distance to whatever is behind the glass:
        // the floor, metres away, instead of the pane's own few centimetres.
        //
        // M_Glass authors twosided=1 on purpose (you walk around the rail), so both faces shade and
        // the back one was applying absorption for a path length taken from the scene's depth. That
        // is why an 8 cm pane read like a metre of bottle glass face-on.
        //
        // s.backFace, NOT SV_IsFrontFace, AND THE DIFFERENCE IS A BUG I SHIPPED. SV_IsFrontFace is
        // winding-dependent; s.backFace is `dot(N, V) < 0` computed in averVertexOf, which is the
        // geometric question actually being asked -- is the ray entering this medium. The fluid box
        // winds the other way from the cube, so the winding test called the pool's visible top
        // surface a BACK face and silently switched water absorption off entirely. This file already
        // carried the rule ("dot(N, V), not SV_IsFrontFace: averVertexOf already computes it that
        // way and states why") a few hundred lines up, and I used the other one anyway.
        //
        // Absorbing once, on entry, is also the physically right count: light crossing a pane is
        // attenuated by its thickness once, not once per surface it passes through.
        // FROM THE SURFACE, NOT FROM THE CBUFFER, and that one-word change is what makes the volume
        // authorable. A material GRAPH can now drive attenuationColor/attenuationDistance per pixel;
        // reading gAttenuationColor here would fetch the authored constant straight back and discard
        // whatever the graph decided, silently and with the graph looking perfectly correct.
        if (s.attenuationDistance > 0.0 && !s.backFace) {
            // Measured ONCE and used twice: the absorption needs it for Beer-Lambert, and the
            // refraction needs it to know how far along the bent path the ray travels before it
            // leaves. Tracing it a second time would be the same ray for the same answer.
            const float volThick = averVolumeThickness(i.wpos, N, -s.V);
            const float3 volT = averVolumeTransmittance(
                s.attenuationColor, s.attenuationDistance, volThick);
            // THE BACKDROP PATH, which is what lets attenuationColor's HUE reach the picture at all.
        // averBlendedOutputVolume (the fallback inside this call) can only make the surface go
        // opaque faster in the channels it absorbs; it cannot tint the background, because one
        // blend alpha is one number. i.pos.xy is SV_Position in pixels, which is the screen
        // coordinate the copy is indexed by.
        outc = averBlendedOutputBackdrop(s, dif, spc, volT, i.pos.xy, i.wpos, volThick);
        } else
#endif
        outc = averBlendedOutput(s, dif, spc);

        // ---- THE FOG DECISION ----
        //
        // Fogged HERE is deliberate, not the WaterShaders.hpp precedent applied blindly -- that file's
        // PSWater (see its own comment, right where it returns) skips averApplyFog because water's
        // colour is built straight from a Fresnel-blended deep/shallow palette that is ALREADY
        // standing in for the atmosphere's own reflective boundary; fogging it again would
        // double-apply the same air to the one surface that is, itself, meant to read as that air.
        // Glass is not that: `dif`/`spc`
        // above are ordinary PBR terms -- sun, sky, bounce -- that have not been fogged by anything
        // yet, exactly like an opaque draw's `radiance` before its own averApplyFog call three lines
        // below.
        //
        // WHAT IS ALREADY FOGGED AND MUST NOT BE TOUCHED AGAIN: the scene behind this pane. Those
        // pixels went through their own opaque draw's averApplyFog(radiance, theirWpos) using THEIR
        // OWN distance to camera, and PremultipliedAlpha's blend equation --
        // dst_new = outc.rgb + dst.rgb*(1-alpha) -- carries dst (the background) through untouched
        // except for the coverage weight. This code never reads dst, so it cannot re-fog it; the only
        // thing being fogged below is `outc.rgb`, which is entirely this pane's OWN new light.
        //
        // WHY THAT NEW LIGHT STILL NEEDS FOGGING, AND WHY ON THE PACKED SUM RATHER THAN ON dif/spc
        // SEPARATELY: the atmosphere between the camera and i.wpos sits in front of the glass, which
        // puts it in front of BOTH lobes -- the sky/GI reflection leaving the front surface and the
        // pane's own tint are the same distance from the camera, so both want the identical
        // attenuation and inscatter. averApplyFog is affine in its `color` argument
        // (color*T + inscatter, then a lerp toward a second inscatter term -- see its own definition
        // in RHIShaders.cpp) but that affine map is NOT distributive over the alpha-weighted split:
        // fogging `dif` and `spc` as two separate calls and then combining them as
        // spc_fogged + dif_fogged*alpha would add the inscatter term TWICE (once from each call,
        // the second copy additionally scaled by alpha) for one physical slab of air, which is a
        // second, smaller instance of exactly the double-counting the water comment warns about.
        // Fogging the already-packed `outc.rgb` applies that one slab of air exactly once, to exactly
        // the new radiance this draw is contributing -- symmetric with the opaque branch below, which
        // also fogs its combined radiance once, after summing the direct and indirect lobes together.
        // Alpha itself is coverage, not radiance, and is left untouched by fog either way.
        outc.rgb = averApplyFog(outc.rgb, i.wpos);
        AVER_GBUF_RETURN(outc);
    }

    float3 radiance = 0.0;
    radiance = averShadeDirect(radiance, s, sun);
    radiance = averShadeIndirect(radiance, s, ind4);
    radiance = averApplyFog(radiance, i.wpos);
    AVER_GBUF_RETURN(float4(radiance, averOpacity(s)));
}

// ================= ray-driven primary visibility (experimental) =================
// THE ONLY THING THIS REPLACES IS "WHAT DID THIS PIXEL SEE". Everything after the first hit is
// the same work PSMainVoxi does -- a sun shadow ray, sky ambient, fog -- because the rasteriser
// was never doing any of that. It answered the visibility question and nothing else, and this
// answers the same question with a ray.
//
// WHAT IT GIVES UP, stated here because no amount of tuning recovers it: hardware early-Z. A
// rasterised fragment that turns out to be hidden is discarded before its shader ever runs; a ray
// pays the whole traversal to discover the same thing. That is the trade this mode exists to
// measure, and the number to beat is in Settings::rtRenderMode.
//
// TEXTURED, and this comment claimed the opposite long after it stopped being true. Under
// AVER_RT_BINDLESS a hit samples base colour, metal-rough, normal, occlusion and emissive -- plus
// the slope-blended second layer -- through RtInstance::materialIndex and gRtMaterials, with UVs
// interpolated at the hit and a real gradient footprint. The Texture2DArray this used to say "does
// not exist yet" is bound in register space 1 a few lines below the material struct.
//
// WHAT STILL DIFFERS from the raster image is narrower than it was: no material GRAPH is dispatched
// on any ray path (graphId is declared and never read), and normal-map PERTURBATION is compiled out
// -- AVER_RT_NORMAL_MAPPING is 0, so the tangent-space normal is built and then not applied.
// GEOMETRY MUST NOT DIFFER, and that is what the
// side-by-side capture is checking.
//
// ---- WHAT A TRANSLUCENT SURFACE SHARES WITH THIS PASS, AND WHAT IT STILL DOES NOT ----
//
// A blended (glass) draw never reaches PSRayDriven at all, but NOT because it is missing from the
// TLAS -- it is in there. submitDraw used to drop it outright and this comment used to say so;
// it now routes it into the TRANSLUCENT LANE instead (submit(..., translucent=true)), which puts it
// in the acceleration structure masked kRtMaskTranslucent and flagged FORCE_NON_OPAQUE so a shadow
// ray's Proceed() loop can attenuate through it. What keeps it out of THIS pass is the MASK: the
// primary visibility ray here traces AVER_RT_MASK_OPAQUE only, so the pane is excluded by the ray's
// own mask rather than by its absence. The distinction matters to anyone changing either: widening
// the mask here would start hitting glass immediately, with no change to how the TLAS is built.
// The ONLY place glass is drawn in this mode is
// the same place it is drawn in raster mode: D3D12Device::endFrame's blended-mesh flush, through
// VSMain+PSMainVoxi's premultiplied-alpha PSO, after this pass's own scenePass() call and the
// deferred sky. Depth test (Less, no write), the blend equation, VSMain's vertex math, and
// PSMainVoxi's Fresnel-lifted alpha and split shading (averBuildSurface/averShadeSplit/
// averBlendedOutput in PbrShaders.cpp) are IDENTICAL code paths whether this pass or the
// rasteriser answered primary visibility for what is behind the pane -- none of them read
// rtRenderMode, suppressesScene(), or anything this pass sets. Traced by hand through
// D3D12Device::drawMesh's capture, storeDrawBinding's byte copy, and the flush's own per-draw
// setDrawBinding call: none of it is elided or skipped in this mode.
//
// WHAT DOES DIVERGE is what a translucent surface reveals: the picture already painted at that
// pixel by whichever pass drew primary visibility. This pass's own hit shading is still a simpler
// material response than PSMainVoxi's -- though much less so than this said for a long time. The
// maps ARE sampled (base colour, metal-rough, normal, occlusion, emissive, and the second layer);
// what remains simpler is that no material GRAPH runs, normal-map perturbation is compiled out
// (AVER_RT_NORMAL_MAPPING 0), and a stochastic path-traced bounce stands in for the voxel cone trace
// on diffuse GI -- all stated and accepted above and below as
// approximations for an OPAQUE surface. They stopped being invisible the moment glass could put
// that surface behind a window a viewer looks through instead of at directly, which is what a
// translucency bug report reads as if nobody has separated it from an actual compositing defect
// first -- see this pass's own environment-specular block, a few hundred lines down, for the one
// piece of that gap this change closes (voxel-cone GI instead of flat sky, matching PSMainVoxi's
// own fallback) and for why the rest is not touched here.
//
// BEHIND AVER_RT because RayQuery is: this entry point only compiles into the SM 6.5 variant, and
// VoxiRenderer refuses the mode outright when the device has no ray-query support.
#if AVER_RT
struct RayDrivenOut {
    float4 col   : SV_TARGET;
    float  depth : SV_DEPTH;
};

// PSRayDriven's own G-buffer bundle: same contract as GBufferOut above (that struct's own field
// comments, up by PSMainVoxi, state the decode for each channel) plus SV_DEPTH, which this pass
// writes itself. Unlike PSMainVoxi -- whose depth comes from ordinary hardware rasterisation -- this
// pass IS the thing answering primary visibility with a ray, so it has no rasteriser depth to inherit
// and must produce its own, exactly as RayDrivenOut already does without this define.
#if AVER_GBUFFER
struct RayDrivenGBufferOut {
    float4 col              : SV_TARGET0;
    float2 velocity         : SV_TARGET1;
    float  viewZ            : SV_TARGET2;
    float4 normalRoughness  : SV_TARGET3;
    float  depth            : SV_DEPTH;
};
#endif

// THIS ENTRY POINT MUST DO THE SAME AS PSMainVoxi, AND THAT IS NOT OPTIONAL: ray-driven primary
// visibility is now the DEFAULT render path (Settings::rtRenderMode = 1), so a G-buffer written only
// by the rasteriser would sit empty in the default configuration -- the exact "built through every
// layer and nothing fills it" failure this codebase keeps producing (see aver-declared-but-unread in
// project memory). Both of this function's `return` sites below fill every AVER_GBUFFER channel.
#if AVER_GBUFFER
RayDrivenGBufferOut PSRayDriven(SkyOut i) {
    RayDrivenGBufferOut o;
#else
RayDrivenOut PSRayDriven(SkyOut i) {
    RayDrivenOut o;
#endif

    // The same NDC-to-world-ray reconstruction PSVoxelDebug does, through the same gInvViewProj,
    // so the primary ray and the debug raymarch cannot disagree about where a pixel looks.
    float4 far = mul(float4(i.ndc, 1.0, 1.0), gInvViewProj);
    float3 dir = normalize(far.xyz / far.w - gCamPos.xyz);

    RayDesc r;
    r.Origin    = gCamPos.xyz;
    r.Direction = dir;
    r.TMin      = 0.0;
    r.TMax      = 1.0e7;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    // Opaque lane only -- see AVER_RT_MASK_OPAQUE. A single Proceed() cannot correctly traverse
    // past a non-opaque candidate, and this ray has no reason to want one.
    // FORCE_OPAQUE IS FREE HERE, AND PROVABLY A NO-OP. createBlas marks every geometry it builds
    // D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE (D3D12Device.cpp), and the only thing that can un-opaque an
    // instance is TlasInstanceFlag_ForceNonOpaque, which VoxiRenderer sets ONLY on the translucent
    // lane -- the lane this ray's AVER_RT_MASK_OPAQUE excludes. So no candidate this ray can ever see
    // is non-opaque, and saying so lets the hardware skip any-hit bookkeeping entirely.
    //
    // NOT on the shadow ray (this file, the rtShadow query): that one masks AVER_RT_MASK_ALL on
    // purpose so a pane of glass can attenuate it, and forcing opaque there would make every pane a
    // wall -- which is the exact behaviour its own comment says was removed.
    q.TraceRayInline(gScene, RAY_FLAG_FORCE_OPAQUE, AVER_RT_MASK_OPAQUE, r);
    q.Proceed();

    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
        // A miss is the sky at the far plane. Depth 1, not 0 -- this engine's projection is not
        // reversed, and writing 0 here would put the sky in front of everything drawn after it.
        //
        // skyColorFull, NOT skyColor, AND THE DIFFERENCE IS A WHOLE-SCREEN ATMOSPHERE MARCH.
        //
        // This colour is thrown away in every frame that draws a sky. The deferred sky dome runs
        // right after this pass (D3D12Device.cpp, gated on skyEnabled_ && !frameSuppressed_), it is
        // a fullscreen triangle with DepthFunc EQUAL against exactly the 1.0 written on the line
        // below, and it is OPAQUE -- so it overwrites every one of these pixels and none of the
        // others. That is not incidental, it is the mechanism the dome was deliberately switched to
        // frameSuppressed_ to get: see its own comment, which records that testing the wrong flag
        // there "left that mode with no clouds, no atmosphere and no sun".
        //
        // So the dome is the RICHER sky -- clouds, atmosphere, sun disc -- and this write is a
        // placeholder for the case where there is no dome at all. Paying for a per-pixel atmosphere
        // march to produce a value that is unconditionally overwritten is the most expensive way to
        // compute nothing, and it got that expensive the moment skyColor started honouring the
        // physical model: the march was measured at up to 41% of a frame in this engine's own notes.
        //
        // WHAT CHANGES, precisely, so this is not a silent behaviour edit: with a sky enabled --
        // every normal frame -- nothing changes at all, because the pixel is overwritten either way.
        // With the sky DISABLED, the ray-driven background is the authored gradient rather than a
        // marched atmosphere, which is what this line produced before skyColor was changed today.
        o.col   = float4(skyColorFull(dir), 1.0);
        o.depth = 1.0;
#if AVER_GBUFFER
        // A miss is the sky at the far plane -- there is no real surface here, so there is no true
        // velocity or shading normal to report. Velocity 0 matches this file's other "no data"
        // fallback (averGBufferVelocity's own near-plane guard, above); viewZ a large sentinel past
        // anything real geometry could report, consistent with this ray's own TMax (1.0e7, just
        // above); and the normal points back at the camera (-dir) rather than being left zero, so a
        // consumer that blindly renormalises this channel gets a valid unit vector instead of a NaN
        // out of normalizing (0,0,0). NONE OF THIS IS A SUBSTITUTE FOR A REAL SKY MASK, which this
        // engine does not have: a consumer that needs to exclude sky pixels from reprojection should
        // do it from viewZ's own far-plane sentinel until one exists, not from anything claimed here
        // about the normal.
        o.velocity        = float2(0.0, 0.0);
        o.viewZ            = 1.0e7;
        o.normalRoughness  = averPackNormalRoughness(-dir, 1.0);
#endif
        return o;
    }

    // Surface reconstruction: the same barycentric interpolation and the same ROTATION-ONLY normal
    // transform rtReflection performs. Its comment explains why the inverse transpose is
    // deliberately not carried (these are rigid instances); the same holds here, and the two must
    // agree or a surface would shade differently depending on whether it was seen directly or in a
    // mirror.
    RtInstance inst = gRtInstances[q.CommittedInstanceID()];
    uint tri = inst.firstIndex + q.CommittedPrimitiveIndex() * 3;
    uint i0  = inst.firstVertex + gRtIndices[tri + 0];
    uint i1  = inst.firstVertex + gRtIndices[tri + 1];
    uint i2  = inst.firstVertex + gRtIndices[tri + 2];

    float2 bary = q.CommittedTriangleBarycentrics();
    float3 w    = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    float3 nObj = normalize(gRtVerts[i0].nrm * w.x + gRtVerts[i1].nrm * w.y + gRtVerts[i2].nrm * w.z);
    float3 N    = normalize(mul(float4(nObj, 0.0), inst.objectToWorld).xyz);
    if (dot(N, dir) > 0.0) N = -N;   // face the ray, so a back-facing hit is not lit from behind

    // UV, the IDENTICAL barycentric pattern `nObj` just used one line up -- RtVertex has always
    // carried it (see its declaration near the top of this file), nothing until now read it. Ready
    // for a base-colour Texture2DArray the moment one exists (indexed by a material's own texture
    // slice; RtMaterial above has no such index yet, so that array and this UV's first real
    // consumer arrive together, not separately) -- NOT sampled here, because there is nothing yet to
    // sample. WHOEVER ADDS THAT SAMPLE: this is a RAY HIT, not a rasterised fragment with SV_Position
    // quad neighbours -- ddx/ddy of a value that comes from a ray query is exactly the landmine
    // rtShadow's `dpx`/`dpy` parameters exist to step around (see that function's own comment): a
    // ray-traced hit position is not screen-space-smooth across a silhouette, so an implicit
    // derivative of anything downstream of it is undefined in HLSL, primary ray or not. The first
    // real sample here MUST be SampleLevel or SampleGrad, never plain Sample(). Mip 0 via
    // SampleLevel is an acceptable first cut for a primary-visibility ray -- it costs minification
    // aliasing (a shimmering high-frequency texture at glancing angles or far distance, where a
    // rasterised fragment would have picked a coarser mip from its own screen-space footprint) and
    // nothing worse; that cost is exactly what a future SampleGrad using this ray's own footprint
    // would buy back, and is explicitly not paid for in this change. THAT FOOTPRINT NOW EXISTS a
    // little further down (`rdRayDx`/`rdRayDy`, built for the shadow-ray call's own `dpx`/`dpy`) --
    // a SampleGrad here would want it too, though it would need recomputing before this point in the
    // function rather than reused, since UV is read before wpos/hitT are.
    float2 hitUV = gRtVerts[i0].uv * w.x + gRtVerts[i1].uv * w.y + gRtVerts[i2].uv * w.z;

    // The hit's own material, keyed by RtInstance::materialIndex (see that field's own comment for
    // what it used to be and why repurposing it cost nothing). This is what makes the constants
    // below real instead of guessed -- see the comment just above where they are used.
    RtMaterial mat = gRtMaterials[inst.materialIndex];

    const float hitT = q.CommittedRayT();
    float3 wpos = gCamPos.xyz + dir * hitT;
    float3 L    = normalize(gLightDir.xyz);

    // ---- THE SHADOW-RAY FOOTPRINT: A RAY DIFFERENTIAL, NOT A SCREEN-SPACE DERIVATIVE ----
    //
    // What stood here passed float3(0,0,0) for both dpx and dpy -- a point sample of the shadow
    // disc, no pixel-footprint jitter at all. Its comment gave rtReflection's own inner shadow call
    // as precedent, and for THAT call zero remains exactly right: a reflected hit's screen
    // neighbours can land on triangles metres apart, so ddx/ddy of the hit position is undefined in
    // HLSL besides being meaningless if it weren't (see rtShadow's own comment on `dpx`/`dpy`, and
    // the UV comment forty lines up, for the length this file already goes to about that landmine).
    //
    // Measured (speckle audit, this task's own briefing): a dense fine speckle on the PTTest pit's
    // far wall, ray-driven only, surviving GI on/off, shadow ray count, denoise radius and the
    // path-traced view toggle -- i.e. tied to nothing but this call. rtShadow's own header explains
    // what a zero footprint costs: every sample in the pixel fires from the exact same origin, so a
    // shadow boundary finer than one pixel -- a grazing self-shadow the pit's own rim casts onto its
    // far wall is exactly that -- aliases into per-pixel speckle instead of resolving as a true area
    // fraction. PSMainVoxi never shows it because its ddx(i.wpos)/ddy(i.wpos) footprint is real.
    //
    // THIS PRIMARY RAY IS NOT THE REFLECTED CASE, though it shares half the problem: this pass has
    // ALREADY returned early for a miss above, so by this point in the shader neighbouring lanes are
    // provably not all at the same point in the program -- the divergent-flow case HLSL's derivative
    // intrinsics do not define, on top of a ray hit's position never being screen-space-smooth in
    // the first place. What it does NOT share is being undirected: every primary ray's DIRECTION is
    // a smooth, analytic function of its own pixel (`dir`, above, built from `i.ndc` and
    // `gInvViewProj`, nothing a ray went on to hit), so that same function can be evaluated for the
    // NEIGHBOUR pixel directly -- no ddx/ddy, no quad communication, well-defined in any control
    // flow, uniform or not.
    //
    // This is a RAY DIFFERENTIAL (Igehy 1999; the same idea a ray-cone texture-LOD scheme uses to
    // size a footprint from distance): reconstruct the neighbouring pixel's own primary-ray
    // direction the identical way `dir` was built, and see how far it has diverged by the time THIS
    // ray travelled `hitT`. A silhouette a few pixels wide still gives a small, bounded footprint --
    // it is a function of the CAMERA and the pixel grid, never of what either ray happened to hit --
    // exactly where ddx(wpos) across that same silhouette would return the actual metres-wide gap
    // between two unrelated surfaces. This is the footprint derived from the ray differential --
    // pixel angular size times hit distance -- that the speckle audit recommended, chosen over its
    // other candidate (a screen-space derivative) for the reason just given.
    //
    // AND NOTE THE PUNCTUATION: this whole shader lives inside a C++ raw string literal, so the
    // two-character sequence close-paren-double-quote ENDS IT. The first draft of this comment
    // quoted the phrase above and closed the quote right after a bracket, which terminated the
    // shader source mid-file and produced forty lines of C++ syntax errors about HLSL identifiers.
    // Keep brackets and quotes apart in here.
    const float2 ndcPixelStep = float2(2.0 / max(gSceneViewport.z, 1.0),
                                       2.0 / max(gSceneViewport.w, 1.0));
    float4 farDx = mul(float4(i.ndc + float2(ndcPixelStep.x, 0.0), 1.0, 1.0), gInvViewProj);
    float3 dirDx = normalize(farDx.xyz / farDx.w - gCamPos.xyz);
    float4 farDy = mul(float4(i.ndc + float2(0.0, ndcPixelStep.y), 1.0, 1.0), gInvViewProj);
    float3 dirDy = normalize(farDy.xyz / farDy.w - gCamPos.xyz);
    // The world-space displacement between this ray and its neighbour, carried out to the SAME
    // distance this ray actually travelled -- "pixel angular size times hit distance", which is what
    // makes the footprint widen with range the way a real receiver footprint does, and shrink to
    // nothing on a surface hit close to the camera the way a point sample should.
    const float3 rdRayDx = (dirDx - dir) * hitT;
    const float3 rdRayDy = (dirDy - dir) * hitT;
    // Flattened onto the hit's own tangent plane before use as dpx/dpy. rtShadow jitters the ray
    // ORIGIN by these (`org = wpos + (dpx*disc.x + dpy*disc.y)*0.5`) and then offsets it along N; an
    // un-flattened footprint could carry that origin off the local surface in the normal direction
    // by however non-tangent the raw camera-space differential happens to be, re-opening exactly the
    // grazing-angle acne rtShadow's own `N*bias` term exists to close. PSMainVoxi's
    // ddx(wpos)/ddy(wpos) never need this step because they are real neighbouring points ON the
    // surface already; this reconstruction is not, so it is projected onto N to behave like one.
    const float3 dpx = rdRayDx - N * dot(rdRayDx, N);
    const float3 dpy = rdRayDy - N * dot(rdRayDy, N);

    // THE SHADOW RAY IS THE SAME CALL THE RASTER PATH MAKES, temporal wrapper and all, so the two
    // modes pay identical shadow cost -- the timing difference between them is primary visibility
    // plus the handful of extra matrix multiplies the footprint above costs, not a difference in
    // what the shadow itself does. It is keyed by pixel, and this pass covers the same pixel grid,
    // so the history buffer means the same thing here as it does there.
#if AVER_RD_ABLATE == AVER_RD_ABL_SHADOW || AVER_RD_ABLATE == AVER_RD_ABL_ALL
    float sunVis = 1.0;   // ablated: fully lit, no ray
#else
    float3 sunVis = rtShadowTemporal(wpos, N, L, i.pos.xy, dpx, dpy, (uint)max(gRtParams.y, 1.0));
#endif

    // Lambertian exitant radiance, with the /PI on the direct term -- see rtReflection's own
    // comment for what omitting it cost last time (every sunlit surface 3.14x too bright, which
    // reads as an exposure bug rather than a units one, and which the white furnace does not catch
    // because it turns the sun off).
    // ---- the bounce loop ----------------------------------------------------------------
    // PATH TRACING HERE IS EXTRA RAYS ON THE LOOP ABOVE, not a second renderer. The first hit
    // has already been found and shaded the way rtReflection shades its own hit; every further
    // bounce repeats exactly that, carrying a throughput and adding what each surface emits
    // toward the previous one.
    //
    // COSINE-WEIGHTED, so the 1/PI of the Lambertian BRDF and the cosine of the rendering
    // equation cancel against the pdf and the throughput is a plain albedo multiply. Getting
    // this wrong is the /PI mistake rtReflection already paid for once, in the other direction.
    //
    // SCREEN-PINNED HASH, no per-frame jitter, matching rtShadow's own seeding: the gate oracle
    // compares nine configurations bit-exactly, and a frame counter in the seed makes every one
    // of them a different image. That means the noise is a fixed dither rather than something
    // that converges over time -- honest for a first cut, and the thing a temporal accumulator
    // would fix.
    // THE ENGINE'S OWN BRDF, not a second one written here. averShadeDirect is the same
    // Cook-Torrance GGX PSMainVoxi shades through; building an AverSurface by hand and handing it
    // over is what stops the ray image and the raster image disagreeing about what a material
    // looks like for reasons that are nobody's intent. The /PI lives inside it (kdAlbedo / PI),
    // which is the divide rtReflection had to learn the hard way.
    //
    // THREE CONSTANTS USED TO BE DEFAULTED, and this comment used to explain why: reflectance, f90
    // and albedo are per-MATERIAL, and a ray hit had nothing but RtInstance to read them from --
    // `inst.albedo` is the raster path's flat per-DRAW colour, not a real baseColorFactor (and, for
    // an AUTHORED material, is neutralised to white by the raster path's own multiply-in-the-shader
    // convention before it ever reaches this table -- a separate, known bug tracked outside this
    // file, not fixed by this change), while reflectance/f90 had no per-material source at all and
    // sat at the textbook dielectric defaults (0.04/1.0) unconditionally. `mat`, fetched above
    // through the new materialIndex/gRtMaterials pair, is real per-material data; using it here is
    // the entire reason that pair exists.
    //
    // METALLIC AND ROUGHNESS ARE DELIBERATELY LEFT ON inst.metallic/inst.roughness, NOT mat.
    // metallicFactor/mat.roughnessFactor, even though the latter sit right there and this file's own
    // task brief describes the former as "already real, per-instance". THEY ARE NOT ALWAYS: for an
    // AUTHORED material inst.metallic/inst.roughness carry the same raster-path neutralised-to-1.0
    // placeholder inst.albedo used to (see buildAccelerationStructures in VoxiRenderer.cpp, which
    // this file does not own and does not edit), so this pass can still render an authored metal
    // fully rough and fully metallic regardless of what it was actually authored as. That bug is a
    // C++-side fix -- source RtInstance.metallic/roughness from the same undistorted MaterialConstants
    // mat.metallicFactor/mat.roughnessFactor already carry, at the point RtInstance is built, not
    // here -- and is explicitly out of THIS change's scope, which only names reflectance/f90/
    // baseColorFactor. Left here as a stated, known gap rather than silently believed fixed.
    //
    // WHAT THIS DOES NOT VERIFY: whether gRtMaterials is actually populated with each instance's
    // real material constants for every draw path (authored .ocmat AND the built-in SurfaceLook
    // table alike), rather than a fallback/default entry. That population is C++-side
    // (VoxiRenderer.hpp/.cpp), owned and edited concurrently by a different agent per this task's
    // own brief, and is not something this file can confirm on its own.
    AverSurface s = (AverSurface)0;
    s.N        = N;
    s.V        = -dir;
    s.H        = normalize(s.V + L);
    // MULTIPLY THE PER-DRAW VALUE BY THE MATERIAL FACTOR. DO NOT REPLACE IT. This is the model the
    // raster path has always used -- PbrShaders.cpp: `s.metallic = saturate(gMaterial.x * a.metallic)`
    // -- and departing from it is what turned this whole render white.
    //
    // WHY BOTH TERMS ARE LOAD-BEARING, because it is not obvious and getting it wrong is silent:
    //   - An AUTHORED (.ocmat) draw has its per-draw colour/metal/rough deliberately NEUTRALISED to
    //     1.0 by the caller (SandboxApp/GameRender both do it), precisely so the material's own
    //     factors can carry the value through this multiply. Per-draw alone would be white.
    //   - An UNAUTHORED draw has no material of its own, so it gets the fallback constants, whose
    //     factors are 1.0 -- and its real colour lives in the per-draw value. The factor alone would
    //     be white.
    // Each source is the identity where the other carries the data. Take one and you lose half the
    // scene; take the product and both cases are right, which is exactly why the raster path
    // multiplies.
    //
    // THE BUG THIS REPLACES: this read `mat.baseColorFactor.rgb` alone. Every unauthored draw --
    // which in the PTTest range is the floor, the walls, the crates and the red targets -- shaded
    // as the fallback's white, while the rasteriser drew them correctly. Reported as "everything is
    // white", and correctly attributed by the user to the ray path having a hardcoded dependency.
#ifdef AVER_RT_BINDLESS
    // THE FULL STOCK MATERIAL AT A RAY HIT: all eight maps, the slope-blended second layer, and a
    // normal-mapped shading normal. Composed exactly as the raster path composes it -- every factor
    // MULTIPLIES its sampled texel rather than replacing it -- so a material with no textures at all
    // reduces to the untextured branch below and the two paths agree by construction.
    //
    // The fallbacks are the identity values averSampleMaps uses for a material with no table, so an
    // unbound slot costs one compare and changes nothing.
    // THE EFFECTIVE UV, which for a world-aligned material is nothing like the mesh's own.
    const float2 uvS = averRtSurfaceUV(mat, inst, wpos, N, hitUV);
    // ...and the footprint one pixel covers in that same UV space, from the ray differentials this
    // shader already built for the shadow disc.
    float2 uvGx, uvGy;
    averRtUvGrad(mat, inst, N,
                 gRtVerts[i0].pos, gRtVerts[i1].pos, gRtVerts[i2].pos,
                 gRtVerts[i0].uv,  gRtVerts[i1].uv,  gRtVerts[i2].uv,
                 rdRayDx, rdRayDy, uvGx, uvGy);

    float4 mapBase  = averRtSampleSlot(mat, 0, uvS, uvGx, uvGy, float4(1, 1, 1, 1));
    float4 mapMR    = averRtSampleSlot(mat, 1, uvS, uvGx, uvGy, float4(1, 1, 1, 1));
    float3 mapNrm   = averRtSampleSlot(mat, 2, uvS, uvGx, uvGy, float4(0.5, 0.5, 1, 1)).xyz * 2.0 - 1.0;
    float  mapOcc   = averRtSampleSlot(mat, 3, uvS, uvGx, uvGy, float4(1, 1, 1, 1)).r;
    float3 mapEmis  = averRtSampleSlot(mat, 4, uvS, uvGx, uvGy, float4(0, 0, 0, 1)).rgb;
    // glTF packs occlusion in R, roughness in G, metallic in B -- the same unpack averSampleMaps does.
    float2 metalRough = float2(mapMR.g, mapMR.b);
    float3 normalTS   = float3(mapNrm.xy * mat.normalScale, mapNrm.z);

    // THE SECOND LAYER, blended by SLOPE off the GEOMETRIC normal -- not the normal-mapped one.
    // averBlendLayers' own comment gives the reason and it is worth repeating rather than diverging
    // from: the question is "is this part of the terrain a cliff", which is a property of the
    // surface, and feeding a normal map into it makes the layer choice flicker with every bump.
    // geoN here is N BEFORE any perturbation, which is exactly what the raster path passes.
    if (mat.flags & AVER_MAT_SLOPE_BLEND) {
        const float flat01 = saturate(abs(N.z));
        const float lw = 1.0 - smoothstep(mat.slopeBlendLo, mat.slopeBlendHi, flat01);
        if (lw > 0.001) {
            const float2 uv1 = uvS * mat.layer1UvScale;
            if (mat.texIndex[5] != AVER_TEX_UNBOUND)
                mapBase = lerp(mapBase, averRtSampleSlot(mat, 5, uv1, uvGx * mat.layer1UvScale, uvGy * mat.layer1UvScale, mapBase), lw);
            if (mat.texIndex[6] != AVER_TEX_UNBOUND) {
                const float4 mr1 = averRtSampleSlot(mat, 6, uv1, uvGx * mat.layer1UvScale, uvGy * mat.layer1UvScale, mapMR);
                metalRough = lerp(metalRough, float2(mr1.g, mr1.b), lw);
            }
            if (mat.texIndex[7] != AVER_TEX_UNBOUND) {
                const float3 n1 = averRtSampleSlot(mat, 7, uv1, uvGx * mat.layer1UvScale, uvGy * mat.layer1UvScale, float4(0.5, 0.5, 1, 1)).xyz * 2.0 - 1.0;
                normalTS = normalize(lerp(normalTS, float3(n1.xy * mat.normalScale, n1.z), lw));
            }
        }
    }

    // NORMAL MAPPING LAST, and only when a map was actually bound: with no map, normalTS is the
    // identity (0,0,1) and perturbing by it is a no-op that still costs a tangent solve.
    // NORMAL MAPPING IS OFF, AND THAT IS A MEASURED DECISION RATHER THAN AN OMISSION.
    //
    // The frame below is written, correct as far as it has been tested, and demonstrably not ready.
    // Mean absolute difference against the raster path over the whole viewport:
    //
    //                              base colour only   + these slots, no normals   + normal mapping
    //   PTTest (authored flats)          32.08                17.23                    18.21
    //   ElectricDreams (terrain)          6.06                 7.48                    17.19
    //
    // So it costs a little on authored surfaces and is catastrophic on terrain -- worse there than
    // having no textures at all. What is already ruled out by measurement, so nobody repeats it:
    // the frame IS orthonormal (feeding nTS = (0,0,1) reproduces the unperturbed normal exactly);
    // the map decodes correctly (its Z reads saturated positive); normalScale is 1.0; and it is not
    // the layer-1 blend, which changes nothing when disabled on its own.
    //
    // The leading suspect is that the frame is ROTATED WITHIN THE TANGENT PLANE -- nTS = (0,0,1)
    // returning N proves only that T and B are perpendicular to N, not that T points along +U. The
    // other live possibility is that the landscape does not perturb in the raster path either, in
    // which case raster is the wrong reference for terrain and this needs a flat authored surface
    // with a known-good normal map to judge against.
    //
    // Flip to 1 to measure it; do not ship it at 1 until terrain is explained.
#define AVER_RT_NORMAL_MAPPING 0
#if AVER_RT_NORMAL_MAPPING
    if (mat.texIndex[2] != AVER_TEX_UNBOUND || mat.texIndex[7] != AVER_TEX_UNBOUND) {
        s.N = averRtPerturbNormal(mat, inst, N, normalTS,
                                  gRtVerts[i0].pos, gRtVerts[i1].pos, gRtVerts[i2].pos,
                                  gRtVerts[i0].uv,  gRtVerts[i1].uv,  gRtVerts[i2].uv);
        // The perturbed normal must still face the ray, for the same reason the geometric one is
        // flipped above: a normal map can tip a grazing normal past the horizon, and shading from
        // behind produces a black rim that looks like a shadow bug.
        if (dot(s.N, dir) > 0.0) s.N = -s.N;
    }
#endif

    s.albedo    = inst.albedo * mat.baseColorFactor.rgb * mapBase.rgb;
    s.emissive  = mat.emissiveFactor * mapEmis;
    // THROUGH occlusionStrength, exactly as averBuildSurface does it:
    //   s.occlusion = lerp(1.0, a.occlusion, gOcclusionStrength)
    // Applying the map at full strength instead -- which this line did at first -- darkened the
    // ElectricDreams terrain from 111,101,96 to 73,72,76 against a raster reference of 104,102,100,
    // i.e. it moved the ray path FURTHER from raster while adding a feature meant to close the gap.
    s.occlusion = lerp(1.0, mapOcc, mat.occlusionStrength);
#else
    s.albedo   = inst.albedo * mat.baseColorFactor.rgb;
#endif
#ifdef AVER_RT_BINDLESS
    // metalRough carries the SAMPLED pair, unpacked glTF-style above: .x is roughness (green),
    // .y is metallic (blue). Multiplied onto the factors exactly as averStockAuthored does, so an
    // unbound map contributes its identity 1.0 and the result is the untextured line below.
    s.metallic = saturate(inst.metallic  * mat.metallicFactor  * metalRough.y);
    s.rough    = clamp(inst.roughness * mat.roughnessFactor * metalRough.x, 0.045, 1.0);
#else
    s.metallic = saturate(inst.metallic * mat.metallicFactor);
    s.rough    = clamp(inst.roughness * mat.roughnessFactor, 0.045, 1.0);   // averEvalMaterial's own floor
#endif
    s.ndv      = saturate(dot(s.N, s.V));
    s.f90      = mat.f90;
    s.reflectance = mat.reflectance;
    // HAND-SET, BECAUSE THIS SURFACE IS HAND-BUILT. averBuildSurface reads these off the material
    // cbuffer, but a ray hit has no material cbuffer bound -- the same reason reflectance is carried
    // here rather than read back off gMatReflectance, two lines up. HLSL does not zero-initialise a
    // struct, so omitting them would feed averDirectTerms whatever was on the stack.
    // A PRIMARY RAY LEAVES THE EYE, so its first hit is always a front face as far as this test is
    // concerned. The ray-driven path does not carry a refracted ray into the medium and back out,
    // so there is no exit interface for it to shade and TIR cannot arise -- stated here rather than
    // left as an uninitialised bool that happens to read false.
    s.backFace  = false;
    s.sssWeight = (mat.flags & AVER_MAT_SUBSURFACE) ? saturate(mat.subsurfaceWeight) : 0.0;
    s.sssRadius = (mat.flags & AVER_MAT_SUBSURFACE) ? saturate(mat.subsurfaceRadius) : 0.0;
#ifdef AVER_LAYERED_BSDF
    // THE COAT NEEDS EXPLICIT LINES HERE. This pass zero-inits the surface and then hand-sets every
    // field, so a new one defaults to 0 for free -- which is the right OFF state for a weight, and
    // would therefore have looked correct while silently meaning "no material in this scene has a
    // coat in ray-driven mode". Read from the ray hit's own material, exactly as the two lines above
    // do, because this pass has no material cbuffer to read.
    s.coatWeight = (mat.flags & AVER_MAT_COAT) ? saturate(mat.coatWeight)    : 0.0;
    s.coatRough  = (mat.flags & AVER_MAT_COAT) ? saturate(mat.coatRoughness) : 0.0;
    s.coatF0     = (mat.flags & AVER_MAT_COAT) ? saturate(mat.coatF0)        : 0.0;
#endif
    // Identical shape to averBuildSurface's own F0 (PbrShaders.cpp: `lerp(gMatReflectance.xxx,
    // s.albedo, s.metallic)`), with `mat.reflectance` standing in for `gMatReflectance` -- the same
    // per-material value, read from this pass's own material buffer instead of the raster path's
    // per-draw constant buffer, which a ray hit has no binding for.
    s.F0       = lerp(mat.reflectance.xxx, s.albedo, s.metallic);
    s.F        = fresnelSchlick(saturate(dot(s.H, s.V)), s.F0, s.f90);
    // (1 - transmission) exactly as averBuildSurface does it (PbrShaders.cpp, the kdAlbedo line):
    // light that passed THROUGH the substrate cannot also scatter back out of it, and a material
    // that does both is inventing energy. Kept identical on both paths deliberately -- this is the
    // ray-hit twin of that line, and a rule honoured by the rasteriser but not by primary rays is
    // the exact shape of defect this tree keeps rediscovering.
    //
    // A BLENDED pane never arrives here (see this pass's own contract comment above: glass is drawn
    // by PSMainVoxi in both render modes), so in practice this fires only for an OPAQUE material
    // that authors transmission -- which is a legitimate thing to author, and the reason the rule is
    // written against the material field rather than against the blend mode.
    s.kdAlbedo = (1.0 - s.metallic) * s.albedo * (1.0 - saturate(mat.transmission));
    s.model    = AVER_MODEL_STANDARD;
    s.alpha    = 1.0;
#ifndef AVER_RT_BINDLESS
    // The textured variant sampled a real occlusion map above; this default would overwrite it.
    s.occlusion = 1.0;
#endif

    AverLight sun;
    sun.direction  = L;
    sun.radiance   = averSunRadiance();
    sun.visibility = sunVis;

    const uint bounces = (uint)max(gPtBounceParams.x, 1.0);

    float3 radiance = averShadeDirect(0.0, s, sun);

    // THE ENVIRONMENT THROUGH THE ENGINE'S OWN INDIRECT TERM, not a diffuse-only line of this
    // shader's own. What stood here was `radiance += s.kdAlbedo * averSkyIrradiance(N) * gAmbient.r`,
    // and the white furnace measured two separate faults in it:
    //
    //   - A WHITE METAL RENDERED BLACK, 0.003 against a correct 1.000. averShadeIndirect was never
    //     called, so the environment SPECULAR term (FssEss * ind.specular) did not exist on this
    //     path at all. A metal has kdAlbedo = 0, so a diffuse-only ambient line hands it nothing,
    //     and roughness could not matter either -- every roughness column read identically.
    //   - TURNING PATH TRACING ON DOUBLED THE ENERGY, 1.000 -> 1.977. This line added the sky once,
    //     and then the bounce loop below added it AGAIN every time a ray escaped to it. In an open
    //     scene every path escapes on its first bounce, so the environment was counted exactly
    //     twice -- and because every path was already gone by bounce 1, a sweep over bounce depth
    //     came back perfectly flat and hid it.
    //
    // ONE OWNER FOR THE ENVIRONMENT, and it is this call. The bounce loop below no longer adds the
    // sky when a ray escapes; it carries surface-to-surface light only. Zeroing ind.ambient instead
    // and letting the escaping ray supply the sky was tried first and measured worse: the
    // multiple-scattering term FmsEms multiplies the IRRADIANCE, so zeroing it threw the
    // compensation away and a white metal fell straight back down the single-scatter curve,
    // 0.971 at roughness 0.05 to 0.450 at 1.0. FmsEms is specular energy, not diffuse, and a
    // path-traced bounce does not carry it.
    //
    // This also makes the ray path structurally the same as the raster one, which adds a full
    // unoccluded sky ambient and then adds bounced light on top of it without subtracting the sky
    // that bounce occludes. Same approximation, same place, one thing to fix if it is ever wrong.
    float3 R = reflect(dir, N);
    AverIndirect ind;
    ind.ambient      = averSkyIrradiance(N);
    ind.ambientScale = gAmbient.r;

    // ---- DIFFUSE INDIRECT: THE CONE TRACE, exactly as PSMainVoxi does it ----
    //
    // THIS REPLACES A ONE-SAMPLE STOCHASTIC BOUNCE THAT COULD NOT CONVERGE, and the distinction
    // matters because the old code was not merely noisy, it was FROZEN noisy. The bounce loop below
    // draws its direction from `rtHash(i.pos.xy + ...)`, and rtHash is deliberately a PURE FUNCTION
    // OF THE PIXEL with no frame term -- its own comment says so, because the gate oracle depends on
    // it. So every pixel picked one direction and kept it forever: one sample, never averaged, never
    // decorrelated. That is not a noisy estimate that settles down, it is a fixed wrong answer per
    // pixel, and no denoiser can help because there is nothing to average over.
    //
    // WHAT IT LOOKED LIKE, since the shape of the artefact is the evidence: dense static
    // salt-and-pepper on enclosed surfaces and none at all in the open. In the open the bounce ray
    // escapes, the loop adds nothing (see its own escape comment), and the variance is zero. Inside
    // a room every bounce ray lands on a different wall at a different angle, so the variance is
    // enormous -- which is exactly the concrete pit reported as "still broken" while the sky and the
    // sphere beside it were clean.
    //
    // coneTracedIndirect is what PSMainVoxi has always used for this term (see its own call site).
    // It is DETERMINISTIC -- a cone march through the voxel clipmap, prefiltered by construction --
    // so it has no variance to remove, and it hands back a real ambient-occlusion factor as well,
    // which the bounce loop never provided (`ind.occlusion = 1.0` below was a stated gap). Using it
    // here makes the two paths answer this question the same way, which is the whole point of a
    // ray-driven mode that is supposed to differ only in HOW THE FIRST SURFACE IS FOUND.
    //
    // COST, MEASURED RATHER THAN ASSUMED, and it went the other way from the guess. The first
    // version of this comment claimed the cone trace was "cheaper as well as cleaner". It is not:
    // on PTTest (voxelResolution 512, GI Epic) "Voxi ray-driven primary" went from 1.3-1.4 ms to
    // 3.0-4.0 ms, and the whole frame from a 4.13 ms median to 8.86 ms. A 6-cone gather marching a
    // 512^3 clipmap costs more than three ray-query traversals on hardware that has ray-query units
    // sitting idle. Stated here because a wrong performance claim in a comment outlives the person
    // who wrote it.
    //
    // IT IS STILL THE RIGHT TRADE, for a reason that is about parity rather than speed: this is
    // exactly the cost PSMainVoxi has always paid for the same term, from the same function, on the
    // same volume. Ray-driven was not cheaper before, it was doing something much worse and
    // charging less for it. The two paths now cost what each other cost, which is the only footing
    // on which "is ray-driven faster than the rasteriser" is a question worth asking at all.
    //
    // WHERE THE COST GOES IF IT NEEDS TO, AND WHERE IT DOES NOT ANY MORE: this paragraph used to say
    // giCones was six, hardcoded, with no quality tier able to reduce it. That was already false
    // when it was written -- Renderer::giConesForQuality (Voxi.cpp) has covered Off/Medium=6, Low=3,
    // High=9, Epic=13 since commit fbb3aad, wired live every frame through gGiParams.x
    // (VoxiRenderer.cpp) into the dynamic `[loop]` coneTracedIndirect actually runs, and that commit
    // predates this very comment in git history. The ladder IS the lever, it already exists, and it
    // already belongs to the GI settings rather than to this pass: nothing here needs redesigning to
    // reach it, only a quality tier below Epic. PTTest itself pins RENDER.GI 4, so it runs at Epic's
    // 13 cones, and any cost number taken on that project is taken at this pass's worst rung.
    //
    // BUT THE CONE COUNT IS *NOT* WHERE THE TIME GOES, and this is measured, not reasoned. A cost
    // audit predicted from arithmetic (14 traceCone calls at ~0.22ms) that dropping Epic to a lower
    // rung should recover 1.5-2ms of the ~3.7ms. It does not. Measured on PTTest, same camera, only
    // the tier differing:
    //
    //     Epic, 13 cones -- ray-driven primary 3.7-3.8ms, whole frame 6.498ms
    //     High,  9 cones -- ray-driven primary 3.5-3.8ms, whole frame 6.066ms
    //
    // Four fewer cones bought about 0.4ms of a 6.5ms frame. So the gather is real but it is not the
    // dominant term, the arithmetic that said otherwise was wrong, and **the 3x gap against the
    // rasteriser remains unexplained**. Do not spend effort shaving cones on the strength of that
    // prediction; it has already been tested and it did not pay.
    //
    // WHAT TO DO INSTEAD, and it is a measurement problem before it is a rendering one: there is NO
    // GPU-timed marker anywhere in this tree for the raster path's own pixel shading. The 0.1ms
    // figure the 3x is computed against is not a like-for-like GPU span the way "Voxi ray-driven
    // primary" is -- it may be counting a different category of work entirely. Put a symmetric
    // ScopedGpuStat on the raster colour draw and re-take both numbers before trusting any ratio
    // between them, let alone optimising against it.
    float rdAo  = 1.0;
    ind.diffuse = 0.0;
#if AVER_RD_ABLATE == AVER_RD_ABL_GI || AVER_RD_ABLATE == AVER_RD_ABL_ALL
    // ablated: no cone gather
#else
    if (gVoxelParams.w > 0.5) ind.diffuse = coneTracedIndirect(wpos, N, rdAo);
#endif

    // ---- ENVIRONMENT SPECULAR: A REAL MIRROR RAY NOW, GATED THE SAME WAY PSMainVoxi GATES ONE ----
    //
    // This used to always fall through to PSMainVoxi's OWN voxel-cone fallback -- the flat-sky-or-
    // cone answer that function itself only reaches once a surface has already failed to qualify for
    // a sharp `rtReflectionTemporal` mirror ray. Found while chasing a translucency bug, not a
    // reflection one: a glass pane's blended replay (D3D12Device::endFrame) composites straight over
    // whatever this pass already painted at that pixel, so every opaque hit under a pane of glass in
    // ray-driven mode is something a viewer can see PAST a real material response instead of past a
    // raster fragment -- and this pass's own hit shading was handing back a cone-traced-or-flat-sky
    // answer for the term PSMainVoxi answers with a sharp ray whenever the surface qualifies. That is
    // not a compositing defect (the blend equation, the depth test and PSMainVoxi's own Fresnel-
    // lifted alpha are unchanged and unaffected by which pass drew what is behind the glass -- see
    // the ray-driven/raster parity note above PSRayDriven's definition), but it does mean glass and
    // chrome read visibly flatter in ray-driven mode than the identical surface in raster, which is
    // exactly the "broad, flat sheet" look the glass/reflection audit traced to this branch.
    //
    // THE OLD REASON NOT TO PORT IT WAS COST, STATED HONESTLY, AND IS STILL TRUE OF AN UNGATED RAY:
    // "stacking a fourth ray onto every pixel is a real cost change to the very thing ray-driven mode
    // exists to measure, not a bug fix." That argument does not survive gating the ray the identical
    // way PSMainVoxi already gates its own -- `s.rough <= 0.75` -- because at that point this pass is
    // not paying for a ray PSMainVoxi's own budget never accounted for either; it is paying the SAME
    // ray, on the SAME subset of surfaces, that the rasteriser already prices into the comparison
    // this mode exists to make. A rough surface (most of a scene: concrete, cloth, unpolished stone)
    // still takes the cheap cone/sky path below, unmodified.
    //
    // THE GATE IS THE SAME PREDICATE PSMainVoxi USES, verbatim: `gShadowParams.z > 0.5 &&
    // gRtParams.w > 0.5 && s.rough <= 0.75`, read from the same VoxiFrame cbuffer fields PSMainVoxi
    // reads (see that struct's own field comments, near the top of this file), not a looser or
    // tighter threshold of this pass's own invention -- the two passes are meant to agree about which
    // surfaces earn a mirror ray, or "ray-driven vs raster" measures two different feature sets
    // instead of the same one priced two ways. Both terms are, in practice, already true whenever
    // this pass itself runs at all: `rayDrivenActive()` (VoxiRenderer.hpp) requires `rtActive_`, which
    // is exactly what sets gShadowParams.z, and this pass already reads gRtInstances/gRtVerts/
    // gRtIndices/gRtMaterials UNCONDITIONALLY for its own primary hit, above -- a geometry-table
    // failure would already have broken this pass before reaching this line, gate or no gate. The
    // gate is kept anyway, for the same reason PSMainVoxi keeps it: it is the one place either pass
    // is told "no" if the geometry table ever legitimately fails to build while the TLAS still
    // exists, and assuming that can never happen is how a future VoxiRenderer.cpp change turns into a
    // wrong-but-plausible reflection here instead of a clean fallback to the cone/sky branch below.
    //
    // rtReflectionTemporal's OWN dzdx/dzdy PARAMETER gets the ray-differential treatment the shadow
    // footprint above already worked out, reused rather than rederived: dzdx/dzdy exist only to
    // predict a REPROJECTED NEIGHBOUR's depth for `rtReflectionSpatial`'s plane-rejection test (see
    // that function's own comment) -- a quality term that shapes how much of the spatial filter's
    // kernel gets accepted, not one that can misplace a ray the way the shadow's dpx/dpy could, so it
    // does not need the tangent-plane projection dpx/dpy got. `rdRayDx`/`rdRayDy`, already built above
    // for the shadow call, are the exact world-space displacement wanted; `mul(float4(rdRayDx, 0.0),
    // gViewProj).w` reads off the DIRECTIONAL part of the same clip.w computation PSMainVoxi's own
    // ddx(viewZ) approximates for a smooth rasterised surface -- exact given how `rdRayDx` was built,
    // not an approximation on top of one -- at the cost of one more directional matrix multiply, far
    // cheaper than reconstructing a second ray differential from nothing. Computed only inside this
    // branch, since nothing outside it uses a reflection dzdx/dzdy at all.
    //
    // EXPECTED COST, STATED RATHER THAN MEASURED -- this task's own hard rule is no build, no launch,
    // so what follows is an estimate to be confirmed against a real capture, not a claim that one was
    // taken. `rtReflectionTemporal` shares its tile schedule with the shadow ray this pass already
    // pays for every pixel (see that function's own comment), so a smooth pixel's reflection is
    // amortised the identical way its shadow already is, not a second uncapped ray-per-frame cost.
    // It is paid ONLY where `s.rough <= 0.75` -- glass, chrome, wet or polished surfaces, a minority
    // of most scenes' pixels -- while everything above that roughness is completely unaffected. The
    // nearest measured reference is the wave-bound-shadow finding's own number for one tile-amortised
    // full-screen ray-query pass, ~2.0ms on a larger scene (ElectricDreams) at full coverage; gated to
    // a roughness minority here, the real cost should land well under that, concentrated on reflective
    // materials rather than spread over every pixel the way the diffuse GI gather already is.
    if (gShadowParams.z > 0.5 && gRtParams.w > 0.5 && s.rough <= 0.75) {
        const float rdReflDzdx = mul(float4(rdRayDx, 0.0), gViewProj).w;
        const float rdReflDzdy = mul(float4(rdRayDy, 0.0), gViewProj).w;
        bool specHit = false;
#if AVER_RD_ABLATE == AVER_RD_ABL_REFL || AVER_RD_ABLATE == AVER_RD_ABL_ALL
        float3 refl = float3(0.0, 0.0, 0.0);   // ablated: no mirror ray
#else
        float3 refl = rtReflectionTemporal(wpos, N, R, L, i.pos.xy, s.rough,
                                           rdReflDzdx, rdReflDzdy, specHit);
#endif
        // THE SAME GUARD PSMainVoxi's twin carries, and for the same reason -- see that block, a few
        // hundred lines up, for the full account including the 13% regression an earlier two-call
        // version of it cost. smoothstep(0.5, 0.75, rough) is exactly 0 at or below 0.5 roughness, so
        // for a ray that HIT something the lerp reduces to `refl` and the marched sky is multiplied
        // by zero.
        //
        // THIS ONE IS THE WHOLE SCREEN, not just the blended surfaces: PSRayDriven answers primary
        // visibility for every pixel in the default render mode, so every smooth surface in the frame
        // was paying a 32-step atmosphere march it then discarded.
        const float skyW = smoothstep(0.5, 0.75, s.rough);
        float3 skyR = float3(0.0, 0.0, 0.0);
#if AVER_RD_ABLATE == AVER_RD_ABL_SKY || AVER_RD_ABLATE == AVER_RD_ABL_ALL
        // ablated: no atmosphere march
#else
        if (!specHit || skyW > 0.0) skyR = skyColor(R);
#endif
        ind.specular = lerp(specHit ? refl : skyR, skyR, skyW);
    } else if (gVoxelParams.w > 0.5) {
        // PSMainVoxi's OWN voxel-cone fallback, for exactly the surfaces PSMainVoxi itself would also
        // fall back for (rough > 0.75, or ray tracing unavailable) -- see that function's identical
        // branch, two hundred lines up, for why this remains the right answer there rather than a
        // gap: past that roughness a one-ray-per-pixel estimate cannot resolve a near-hemispherical
        // lobe regardless of which pass is asking.
        float  specAperture = clamp(s.rough * 0.5 + 0.02, 0.02, 0.4);
        float4 sceneSpec    = traceCone(wpos, R, specAperture);
        // THE SKY TERM IS SKIPPED WHERE THE CONE ALREADY SAW A WALL. HLSL does not short-circuit a
        // multiply: skyColor(R) * (1 - sceneSpec.a) ran the full 32-step atmosphere march even when
        // the cone came back fully occluded and the result was scaled to nothing. This is the
        // rough > 0.75 branch -- concrete, cloth, unpolished stone, which is most of a real scene --
        // so that was the common case paying for a value it then multiplied away.
        //
        // THE SAME SHAPE, AND THE SAME FIX, AS averFogInscatter'S OWN THRESHOLD, whose comment
        // records what it was worth there: "scene draw 8.9ms -> 1.3ms ... 85% of the scene pass and
        // 41% of the entire frame". That one gates on w > 0.01 and argues the skipped contribution is
        // under one 8-bit step, so a pixel crossing the threshold cannot band. The argument holds
        // identically here because the weight is the same kind of quantity -- a [0,1] coverage -- and
        // the term it scales is bounded radiance.
        //
        // 0.004, NOT 0.01, and the difference is deliberate: this weight multiplies a sky that can be
        // far brighter than the fog reference, so the same visual error needs a tighter cutoff. At
        // 0.004 the dropped term is at most 0.4% of a sky sample, which stays under a code at 8 bits
        // for any sky this engine can produce short of the furnace.
        const float skyWeight = 1.0 - sceneSpec.a;
        ind.specular        = min(sceneSpec.rgb * gVoxelParams.y, AVER_VOX_MAXRAD);
        if (skyWeight > 0.004) ind.specular += skyColor(R) * skyWeight;
    } else {
        ind.specular        = skyColor(R);
    }
    // REAL AMBIENT OCCLUSION NOW, from the same cone march that produced the diffuse term. This
    // used to be a hardcoded 1.0 whose comment said "a traced bounce is its own occlusion" -- true
    // of a converged path tracer, and not true of one fixed sample per pixel, which is what this
    // actually had. With the cone trace supplying both, the ray path gets the occlusion the raster
    // path has always had, and the two agree.
    ind.occlusion    = rdAo;
    radiance = averShadeIndirect(radiance, s, ind);

    // THE BOUNCE CARRIES THE DIFFUSE RESPONSE, not the raw albedo. A metal reflects almost
    // nothing diffusely, so multiplying a path's throughput by base colour would light an
    // interior with bounced light off surfaces that do not bounce it.
    float3 throughput = s.kdAlbedo;
    float3 bp = wpos;
    float3 bn = N;
    // SKIPPED ENTIRELY WHENEVER THE CONE TRACE ALREADY ANSWERED THIS, and that is a correctness
    // requirement, not a saving. The two compute the SAME QUANTITY -- surface-to-surface bounced
    // light arriving at this point -- so running both adds the room's bounce twice and lights every
    // interior about twice as brightly as the rasteriser lights the identical scene.
    //
    // The loop is left reachable for the GI-off case, where nothing else supplies a bounce at all,
    // and for anyone deliberately measuring the path-traced estimator. But be clear about what it is
    // in this pass: ONE cosine sample per pixel per bounce, drawn from a hash that cannot vary by
    // frame, with no history to accumulate into. It is a correct estimator sampled far too few times
    // to be an image. Making it usable needs per-frame decorrelation AND somewhere to accumulate --
    // a history buffer this pass does not have -- and until it has one, the cone trace above is the
    // better answer at a fraction of the cost.
    const bool rdConeSuppliedDiffuse = gVoxelParams.w > 0.5;
    [loop] for (uint b = 1; b < bounces && !rdConeSuppliedDiffuse; ++b) {
        // A cosine-weighted direction about the surface normal, from the same rtHash the shadow
        // disc uses. Two hashes for the two dimensions, decorrelated by offsetting the pixel.
        float u1 = rtHash(i.pos.xy + float2(b * 17.0, 0.0));
        float u2 = rtHash(i.pos.xy + float2(0.0, b * 23.0));
        float r   = sqrt(u1);
        float phi = 2.0 * PI * u2;
        float3 t  = normalize(abs(bn.z) < 0.999 ? cross(float3(0,0,1), bn) : cross(float3(1,0,0), bn));
        float3 bt = cross(bn, t);
        float3 dirB = normalize(t * (r * cos(phi)) + bt * (r * sin(phi)) + bn * sqrt(max(0.0, 1.0 - u1)));

        RayDesc rb;
        const float bbias = max(gRtParams.z, 1e-4) * (1.0 + length(bp - gCamPos.xyz) * 5e-4);
        rb.Origin = bp + bn * bbias;
        rb.Direction = dirB;
        rb.TMin = bbias;
        rb.TMax = 1.0e7;

        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> qb;
        // Opaque lane only -- see AVER_RT_MASK_OPAQUE.
        // Same proof as the primary ray above: this mask cannot see a non-opaque candidate.
        qb.TraceRayInline(gScene, RAY_FLAG_FORCE_OPAQUE, AVER_RT_MASK_OPAQUE, rb);
        qb.Proceed();

        if (qb.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
            // The ray escaped, and the path simply ends. It deliberately does NOT add the sky:
            // averShadeIndirect above already gave this surface the whole unoccluded environment,
            // and adding it again here is what made turning path tracing on read 1.977 in a
            // furnace where the answer is 1.000. The loop carries bounced light off SURFACES.
            break;
        }

        RtInstance bi = gRtInstances[qb.CommittedInstanceID()];
        uint btri = bi.firstIndex + qb.CommittedPrimitiveIndex() * 3;
        uint b0 = bi.firstVertex + gRtIndices[btri + 0];
        uint b1 = bi.firstVertex + gRtIndices[btri + 1];
        uint b2 = bi.firstVertex + gRtIndices[btri + 2];
        float2 bbary = qb.CommittedTriangleBarycentrics();
        float3 bw = float3(1.0 - bbary.x - bbary.y, bbary.x, bbary.y);
        float3 bnObj = normalize(gRtVerts[b0].nrm * bw.x + gRtVerts[b1].nrm * bw.y + gRtVerts[b2].nrm * bw.z);
        float3 bnWS = normalize(mul(float4(bnObj, 0.0), bi.objectToWorld).xyz);
        if (dot(bnWS, dirB) > 0.0) bnWS = -bnWS;

        bp = bp + dirB * qb.CommittedRayT();
        bn = bnWS;
        // Diffuse response again, for the reason stated at the primary hit: a cosine-weighted
        // bounce is sampling the DIFFUSE lobe, so the throughput is the diffuse albedo and a
        // metal correctly contributes almost nothing to it.
        throughput *= (1.0 - saturate(bi.metallic)) * bi.albedo;

        // ONE shadow ray per bounce, no disc -- the penumbra of a surface seen only through two
        // diffuse bounces is not resolvable, and this is the single largest cost in the loop.
        float bshadow = rtShadow(bp, bn, L, i.pos.xy, float3(0,0,0), float3(0,0,0), 1u, 0.0);
        float3 bdirect = averSunRadiance() * saturate(dot(bn, L)) * bshadow / PI;
        radiance += throughput * bdirect;
    }

    radiance = averApplyFog(radiance, wpos);

    // Depth for everything that draws AFTER the scene -- the deferred sky, transparentPass, the
    // particle pass. Without it they have nothing to test against and sort against a cleared
    // buffer, which puts smoke in front of walls.
    float4 clip = mul(float4(wpos, 1.0), gViewProj);
    o.depth = clip.w > 1e-6 ? saturate(clip.z / clip.w) : 1.0;
    o.col   = float4(radiance, 1.0);
#if AVER_GBUFFER
    // clip.w IS the view-space linear depth this pass's own viewZ channel wants -- computed two
    // lines up for o.depth's own post-projective divide, reused rather than a second
    // mul(float4(wpos,1), gViewProj). Velocity uses the SAME static-geometry function PSMainVoxi
    // does (see averGBufferVelocity's own comment for what that does and does not yet handle for a
    // moving hit), and the normal is this pass's own ray-hit shading normal N -- not the interpolated
    // vertex normal a rasterised fragment would carry, which is exactly what the task this feature
    // was scoped from asked for.
    o.velocity        = averGBufferVelocity(wpos);
    o.viewZ            = clip.w;
    o.normalRoughness  = averPackNormalRoughness(N, s.rough);
#endif
    return o;
}
#endif  // AVER_RT

// ================= depth prepass =================
// Same-frame depth-only pass -- see VoxiRenderer.hpp's depthPrepassPipeline() and
// D3D12Device::drawMesh for the whole mechanism. Paired with VSMain (the SAME compiled vertex shader
// PSMainVoxi's own pipelines use, not a second copy of it), so this writes EXACTLY the depth the
// colour pass's own rasterisation would have produced for the identical triangle.
//
// WRITES NO COLOUR -- the pipeline this compiles into declares renderTargetCount = 0 -- and reads
// only enough of the material to answer one question: does this fragment survive alpha test. That
// is deliberately far short of averEvalMaterial(), which this does NOT call: averEvalMaterial also
// samples the metal-rough, normal, occlusion and emissive maps and does the Fresnel/GGX setup around
// them, none of which a depth-only fragment has any use for. Calling it here to reach one field
// (s.alpha) would make the "cheap prepass" pay four texture fetches instead of at most one.
//
// STILL NOT FREE, though, and the task this pass exists for says to be honest about the cost: EVERY
// covered pixel pays a branch on gMaterialFlags (that flag lives in the SAME AverMaterial cbuffer as
// everything else a material declares, so there is no way to know "is this alpha-tested" without at
// least reading it), and an alpha-tested material additionally pays one Sample() against
// gBaseColorMap plus the multiply/compare below. What it buys back is skipping PSMainVoxi entirely --
// a shadow-cascade lookup, up to eight cone traces, ray-traced-history blending and fog -- on every
// fragment this pass determines is hidden, which is the entire point: one cheap sample now instead
// of one expensive shader later, repeated for whatever overdraw sits behind it.
//
// DOES NOT EVALUATE AVER_MAT_SLOPE_BLEND's second layer (see averBlendLayers in PbrShaders.cpp): that
// flag is landscape-only in this codebase, and the landscape is drawn through a wholly separate call
// site (LandscapeRenderer::draw(), never through IDevice::drawMesh/drawMeshDepthPrepass) that this
// prepass never reaches in the first place -- see SandboxApp.cpp's own comment on why the landscape
// is one of this feature's three excluded paths. If a non-landscape material is ever authored with
// both AVER_MAT_SLOPE_BLEND and AVER_MAT_ALPHA_MASK set, this function's alpha would be the FIRST
// layer's alone; that combination does not exist in this tree today.
void PSDepthPrepass(VSOut i) {
#ifdef AVER_MATERIAL_SRV
    if (gMaterialFlags & AVER_MAT_ALPHA_MASK) {
        AverVertex v = averVertexOf(i);
        float2 uv = averSurfaceUV(v);
        float alpha = gBaseColor.a * gBaseColorFactor.a * gBaseColorMap.Sample(gMaterialSampler, uv).a;
        clip(alpha - gAlphaCutoff);
    }
#endif
}

// Depth-only vertex shader for one shadow cascade; which one is in gShadowDraw.x.
float4 VSShadow(VSIn i) : SV_POSITION {
    return mul(mul(float4(i.pos, 1.0), gWorld), gCascadeViewProj[(uint)gShadowDraw.x]);
}

#ifdef AVER_INSTANCE_SRV
// AVER_INSTANCE_SRV is the t-register VoxiRenderer.cpp computed for THIS pipeline's layout when it
// compiled this entry point -- see GraphicsPipelineDesc::instanced in RHIResources.hpp. Two macro
// layers so the register NUMBER (not the literal text "AVER_INSTANCE_SRV") gets pasted after "t".
#define AVER_INST_JOIN2(a, b) a##b
#define AVER_INST_JOIN(a, b) AVER_INST_JOIN2(a, b)
StructuredBuffer<float4x4> gInstanceWorlds : register(AVER_INST_JOIN(t, AVER_INSTANCE_SRV));

// VSShadow's instanced twin: one DrawIndexedInstanced call submits every surviving instance of one
// mesh in one cascade, instead of shadowPass calling drawMesh() once per instance. The world
// transform comes from gInstanceWorlds[instanceID] -- written by VoxiRenderer::shadowPass via
// IRenderContext::drawMeshInstanced -- rather than from PerObject's gWorld, which this entry point
// never reads. Everything else (the cascade's view-projection, the depth-only output) is identical
// to VSShadow.
float4 VSShadowInstanced(VSIn i, uint instanceID : SV_InstanceID) : SV_POSITION {
    float4x4 world = gInstanceWorlds[instanceID];
    return mul(mul(float4(i.pos, 1.0), world), gCascadeViewProj[(uint)gShadowDraw.x]);
}
#endif

// The same depth-only pair again, for the GI-ONLY shadow map. Identical to VSShadow/
// VSShadowInstanced except that they transform into gGiShadowViewProj -- one box over the GI
// volume -- rather than into a cascade selected by gShadowDraw.x. Separate entry points rather than
// a branch because the matrix is picked at pipeline level, not per draw, and a depth-only vertex
// shader is far too hot to spend a dynamic index on.
float4 VSGiShadow(VSIn i) : SV_POSITION {
    return mul(mul(float4(i.pos, 1.0), gWorld), gGiShadowViewProj);
}

#ifdef AVER_INSTANCE_SRV
float4 VSGiShadowInstanced(VSIn i, uint instanceID : SV_InstanceID) : SV_POSITION {
    float4x4 world = gInstanceWorlds[instanceID];
    return mul(mul(float4(i.pos, 1.0), world), gGiShadowViewProj);
}
#endif

// ================= Voxi: voxelisation =================
// The scene is rasterised once per frame with no render target; the pixel shader writes lit
// radiance straight into the volume.
struct VoxOut { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nrm : NORMAL; float2 uv : TEXCOORD1; };

// Adapts a VoxOut to AverVertex. V is exactly zero: there is no camera in a voxelisation pass.
// A distinct name, NOT an overload of averVertexOf: FXC converts between compatible structs and
// makes every call ambiguous (error X3067).
AverVertex voxelVertexOf(VoxOut i) {
    AverVertex v;
    v.wpos = i.wpos;
    v.N    = normalize(i.nrm);
    v.V    = float3(0, 0, 0);
    // FALSE, AND NOT MERELY UNSET. There is no camera in a voxelisation pass -- V is exactly zero
    // above -- so "is the eye inside this surface" has no answer here. False is the answer that
    // makes the TIR test below inert, which is correct: a voxel bake records how a surface emits
    // and reflects, not how it looks from a viewpoint that does not exist. HLSL leaves a struct
    // member uninitialised, so this must be written rather than assumed.
    v.backFace = false;
    v.uv   = i.uv;
    return v;
}

// Voxelisation vertex shader: outputs world space for the geometry shader to project.
VoxOut VSVoxel(VSIn i) {
    VoxOut o;
    float4 wp = mul(float4(i.pos, 1.0), gWorld);
    o.wpos = wp.xyz;
    o.nrm  = averTransformNormal(i.nrm, gWorld);
    o.uv   = i.uv;
    o.pos  = wp;
    return o;
}

// Projects each triangle along its dominant axis so it covers the most voxels.
[maxvertexcount(3)]
void GSVoxel(triangle VoxOut inp[3], inout TriangleStream<VoxOut> os) {
    float3 n = abs(cross(inp[1].wpos - inp[0].wpos, inp[2].wpos - inp[0].wpos));
    int axis = (n.x > n.y && n.x > n.z) ? 0 : ((n.y > n.z) ? 1 : 2);
    [unroll] for (int k = 0; k < 3; ++k) {
        VoxOut o = inp[k];
        float3 v = voxelUVW(o.wpos);
        float2 p = (axis == 0) ? v.yz : ((axis == 1) ? v.xz : v.xy);
        o.pos = float4(p * 2.0 - 1.0, 0.5, 1.0);
        os.Append(o);
    }
}

#if AVER_MS
// Voxelisation without a geometry shader: the same dominant-axis projection, per primitive.
//
// nr[k] went through averTransformNormal(v.nrm, gWorld) rather than a plain mul(float4(nrm,0),
// gWorld) so that this path actually matches VSVoxel below (and VSMain, ActorPreview's vertex
// shader and MSClusterMain) -- see 1856da1 "Render: a normal transform, two GPU races, and a seam
// along every terrain section", which fixed all four OTHER call sites of this exact bug and missed
// this fifth one because MSVoxel is compiled only behind AVER_MS, which nothing had turned on yet.
// A plain mul is correct only under rotation and uniform scale; every normal with components on
// more than one axis reads wrong the moment an entity's scale goes non-uniform, and this pass
// backs indirect lighting -- a mis-shaded normal here does not flicker on one triangle, it tints an
// entire surface's bounce light for as long as the volume holds it.
[numthreads(AVER_MS_TRIS, 1, 1)]
[outputtopology("triangle")]
void MSVoxel(uint gid : SV_GroupID, uint gtid : SV_GroupThreadID,
             out vertices VoxOut verts[AVER_MS_TRIS * 3],
             out indices uint3 tris[AVER_MS_TRIS]) {
    uint count = msTriCount(gid);
    SetMeshOutputCounts(count * 3, count);
    if (gtid >= count) return;

    uint3 idx = gIndices.Load3((gid * AVER_MS_TRIS + gtid) * 12);
    float3 wp[3], nr[3];
    float2 uv[3];
    [unroll] for (uint k = 0; k < 3; ++k) {
        MeshVtx v = gVerts[idx[k]];
        wp[k] = mul(float4(v.pos, 1.0), gWorld).xyz;
        nr[k] = averTransformNormal(v.nrm, gWorld);
        uv[k] = v.uv;
    }
    float3 n = abs(cross(wp[1] - wp[0], wp[2] - wp[0]));
    int axis = (n.x > n.y && n.x > n.z) ? 0 : ((n.y > n.z) ? 1 : 2);
    uint o = gtid * 3;
    [unroll] for (uint k = 0; k < 3; ++k) {
        float3 v = voxelUVW(wp[k]);
        float2 p = (axis == 0) ? v.yz : ((axis == 1) ? v.xz : v.xy);
        VoxOut ov;
        ov.wpos = wp[k];
        ov.nrm  = nr[k];
        ov.uv   = uv[k];
        ov.pos  = float4(p * 2.0 - 1.0, 0.5, 1.0);
        verts[o + k] = ov;
    }
    tris[gtid] = uint3(o, o + 1, o + 2);
}
#endif // AVER_MS

// Shades the fragment with shadowed sun plus sky and adds its exitant radiance to the accumulator.
void PSVoxel(VoxOut i) {
    float3 uvw = voxelUVW(i.wpos);
    if (!insideVolume(uvw)) return;
    float3 N = normalize(i.nrm);
    float3 L = normalize(gLightDir.xyz);
    float ndl = saturate(dot(N, L));

    AverLight sun;
    sun.direction  = L;
    sun.radiance   = averSunRadiance();
    // THE GI-ONLY MAP, not the cascades -- see giShadowFactor for why a voxel cannot use them.
    sun.visibility = giShadowFactor(i.wpos, N, ndl);
    AverSurface s = averEvalMaterial(voxelVertexOf(i), sun);
    float3 albedo = averDiffuseAlbedo(s);
    // Exitant radiance, not radiosity: the sun term is an irradiance so it takes the 1/PI, the sky
    // term is already a radiance so it does not.
    float3 radiance = albedo * (sun.radiance * ndl * sun.visibility / PI
                                + averSkyIrradiance(N) * gAmbient.r);
    radiance = clamp(radiance, 0.0, AVER_VOX_MAXRAD);

    // insideVolume() is inclusive of 1.0, and conservative raster does produce uvw == 1.0 exactly.
    uint3 c = min(uint3(uvw * gVoxelParams.x), (uint)gVoxelParams.x - 1);
    uint3 a = uint3(c.x * 4, c.y, c.z);
    uint prev;
    InterlockedAdd(gVoxelAccum[a],                (uint)(radiance.r * AVER_VOX_FIXED), prev);
    InterlockedAdd(gVoxelAccum[a + uint3(1,0,0)], (uint)(radiance.g * AVER_VOX_FIXED), prev);
    InterlockedAdd(gVoxelAccum[a + uint3(2,0,0)], (uint)(radiance.b * AVER_VOX_FIXED), prev);
    InterlockedAdd(gVoxelAccum[a + uint3(3,0,0)], 1u, prev);   // fragments covering this voxel
}

// Zeroes the accumulator before injection.
[numthreads(4,4,4)]
void CSClear(uint3 id : SV_DispatchThreadID) {
    uint3 a = uint3(id.x * 4, id.y, id.z);
    [unroll] for (uint k = 0; k < 4; ++k) gVoxelAccum[a + uint3(k,0,0)] = 0;
}

// Turns the fixed-point sums into mip 0 of the filterable RGBA16F volume: the mean radiance of the
// fragments that covered each voxel.
[numthreads(4,4,4)]
void CSResolve(uint3 id : SV_DispatchThreadID) {
    uint3 a = uint3(id.x * 4, id.y, id.z);
    uint n = gVoxelAccum[a + uint3(3,0,0)];
    if (n == 0) { gVoxelUAV[id] = 0.0; return; }
    float3 s = float3(gVoxelAccum[a], gVoxelAccum[a + uint3(1,0,0)], gVoxelAccum[a + uint3(2,0,0)]);
    gVoxelUAV[id] = float4(s / (AVER_VOX_FIXED * (float)n), 1.0);   // alpha = occupancy
}

// ================= Voxi: mip filtering =================
// Box-filters radiance and occupancy from one mip into the next. The mip binding set puts a
// SINGLE-MIP view of the source at t0 and the destination level at u0.
[numthreads(4,4,4)]
void CSMip(uint3 id : SV_DispatchThreadID) {
    int3 s = int3(id) * 2;
    float4 a = 0;
    [unroll] for (int x=0;x<2;++x)
    [unroll] for (int y=0;y<2;++y)
    [unroll] for (int z=0;z<2;++z)
        a += gVoxelTex.Load(int4(s + int3(x,y,z), gSrcMip));
    gVoxelUAV[id] = a * 0.125;
}

// Debug view: raymarches the volume straight to screen over the sky. Returns linear radiance.
float4 PSVoxelDebug(SkyOut i) : SV_TARGET {
    float4 far = mul(float4(i.ndc, 1.0, 1.0), gInvViewProj);
    float3 ray = normalize(far.xyz / far.w - gCamPos.xyz);
    float voxelWorld = 1.0 / (gVoxelOrigin.w * gVoxelParams.x);
    float4 acc = 0;
    float t = 0;
    [loop] for (int s = 0; s < 256; ++s) {
        if (acc.a >= 0.98) break;
        float3 uvw = voxelUVW(gCamPos.xyz + ray * t);
        t += voxelWorld;
        if (t > gVoxelParams.z * 2.0) break;
        if (!insideVolume(uvw)) continue;
        float4 v = gVoxelTex.SampleLevel(gVoxelSamp, uvw, 0);
        acc += (1.0 - acc.a) * v;
    }
    float3 bg = skyColor(ray);
    float3 col = acc.rgb + bg * (1.0 - acc.a);
    return float4(col, 1.0);
}
