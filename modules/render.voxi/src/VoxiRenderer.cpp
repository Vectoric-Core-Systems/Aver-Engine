// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
#include "aver/voxi/VoxiRenderer.hpp"
#include "aver/core/CpuTiming.hpp"   // CpuNest(CpuSpan::VoxiSubmit) below -- a core header (Types.hpp
                                     // and the standard library only, see that header's own comment
                                     // on why), so it needs no AVER_MODULE_VOXI guard of its own: it
                                     // is exactly as unconditional here as Log.hpp and Math.hpp are.
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"   // light-frustum fit: Vec3 / Mat4::lookAtLH
#include "aver/voxi/CameraFactor.hpp"   // beginShadowHistory's NRD block: recovers worldToView/viewToClip
#include "aver/pbr/MaterialGraphRegistry.hpp"
#include "aver/pbr/MaterialSystem.hpp"
#include "aver/pbr/PbrShaders.hpp"
#include "aver/voxi/VoxiGiShaders.hpp"   // kGiSrvCount/kGiUavCount: the "typed twice" fix below
#include "aver/voxi/GiVisibility.hpp"   // givis::packAmbientW/halfDim -- the C++/HLSL bit-table's one definition

#include "VoxiShaders.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstddef>   // offsetof: buildLocalLights reads two MaterialConstants fields per draw
#include <cmath>
#include <cstring>
#include <string>

// Voxi's GPU side: resources, passes and pipelines, expressed only in terms of the generic RHI.
namespace aver::voxi {

namespace {

// Shadow map: four cascades in one 2x2 atlas texture.
constexpr u32 kShadowCascades   = 4;
constexpr u32 kShadowCascadeSize = 2048;
constexpr u32 kShadowSize        = kShadowCascadeSize * 2;   // 2x2 atlas
static_assert(kShadowCascades == 4, "the atlas below is laid out as 2x2");

// The GI-only shadow map: ONE box fitted to the GI volume, not the camera, so the camera cascades
// don't serve two masters. fitCascades() used to UNION the last cascade with the volume -- measured,
// that made cascade 3's radius 36,744cm against a camera-fitted 9,923cm (3.7x wider, ~14x the area),
// admitting every draw into the per-cascade cull (census [3, 4, 5, 27]) every frame and spending the
// cascade's texels on volume the camera can't see instead of on visible shadow.
// 1024 feeds a voxel grid 128 across at Medium (512 at Epic) -- 8 texels/voxel edge, 2 at Epic --
// 1/16 the cascade atlas's texels, rendered only on giUpdateInterval's cadence.
constexpr u32 kGiShadowSize = 1024;

// ---- RTXDI ReSTIR GI reservoir sizing -- MIRRORS rtxdi::CalculateReservoirBufferParameters ----
// (third_party/rtxdi/Source/RtxdiUtils.cpp), restated here rather than called: linking against the
// vendored Rtxdi static target (third_party/rtxdi/CMakeLists.txt) would need a CMakeLists.txt
// change, out of this change's edit list, for a four-line integer formula neither side can get out
// of step with silently -- the SHADER computes the identical thing from gGiSurfHist's own
// dimensions (voxi.hlsl's giReservoirBufferParams), so both sides derive from the same
// width/height rather than trusting two independent implementations to agree.
constexpr u32 kGiReservoirBlockSize = 16;    // RTXDI_RESERVOIR_BLOCK_SIZE (RtxdiParameters.h)
constexpr u32 kGiReservoirBufferCount = 2;   // rtxdi::c_NumReSTIRGIReservoirBuffers (GI/ReSTIRGI.h)
constexpr u32 kGiReservoirElemBytes = 32;    // sizeof(RTXDI_PackedGIReservoir) -- GI/ReSTIRGIParameters.h
u32 giReservoirElemCount(u32 width, u32 height) {
    const u32 blocksX = (width + kGiReservoirBlockSize - 1) / kGiReservoirBlockSize;
    const u32 blocksY = (height + kGiReservoirBlockSize - 1) / kGiReservoirBlockSize;
    const u32 blockRowPitch = blocksX * kGiReservoirBlockSize * kGiReservoirBlockSize;
    return blockRowPitch * blocksY * kGiReservoirBufferCount;
}

// ---- STAGED RAY-DRIVEN PASSES (milestone 1): rdVisBuf_'s element size ----
// One uint4 per pixel -- see gRdVisBuf's own declaration (voxi.hlsl) for the hit/miss encoding. NO
// block rounding, unlike kGiReservoirBlockSize above: the pitch this buffer is indexed with is the
// render target's own width, exactly, so a resize that changes the width by even one pixel changes
// the pitch by the same one pixel, and ensureRdStagedResources' own size check (rdStagedW_/H_) already
// catches that without needing a coarser "did the ROUNDED pitch change" test.
constexpr u32 kRdVisElemBytes = 16;

// ---- SUB-STAGE SPLITS (Settings::rayDrivenShadowTiles / rayDrivenGiSplit): the two candidate
// buffers CSRdShadowProbe/CSRdGiTrace write and CSRdShadow/CSRdGi read back ----
// kRdGiCandElemBytes: sizeof(RdGiCand) (voxi_restir.hlsli) -- float3 pos, uint flags, float3 nrm,
// f2LumTraced, float3 rad, f2LumSky -- 12*4 = 48 bytes, one element per pixel of the render target,
// the SAME row pitch rdVisBuf_ itself uses (see kRdVisElemBytes' own comment for why no block
// rounding applies here either).
constexpr u32 kRdGiCandElemBytes = 48;
// kRdShadowTileElemBytes: one uint mask per 8x8 tile -- sized ceil(W/8) * ceil(H/8) tiles, not per
// pixel, so this buffer is orders of magnitude smaller than rdVisBuf_/rdGiCandBuf_.
constexpr u32 kRdShadowTileElemBytes = 4;

// ---- LOCAL LIGHTS (LAMPS) in the single-pass PSRayDriven (voxi.rayDrivenStages 0) ----
// Whether that megakernel is compiled with the lamp term (AVER_RD_SINGLE_PASS_LAMPS, voxi.hlsl). A C++
// switch because the megakernel sits at the AMD driver's register limit -- it lost the device before
// ec35bb5a -- and lamps must be removable from it without touching the shader. false appends
// ";AVER_RD_SINGLE_PASS_LAMPS=0" to its four compiles (createScenePipelines) AND keeps the light count at
// 0 for the single pass (scenePass), so the blended replay and ReSTIR's emitter drop never act on lamp
// light the opaque pixels did not receive.
constexpr bool kRdSinglePassLamps = true;

// Cascade split blend: 0 is uniform slabs, 1 is logarithmic (equal ratios).
constexpr f32 kCascadeSplitLambda = 0.85f;

// How far the cascades reach, as a multiple of the camera's near plane.
constexpr f32 kShadowRangeFromNear = 4000.0f;

// How small a caster has to get, measured in this cascade's own shadow-map texels, before it stops
// being drawn into it. ONE texel is the honest floor rather than a tuned number: below it the map
// has no sample that can hold the object, so the draw cannot change the image it is drawn into.
constexpr f32 kMinShadowTexels = 1.0f;

// The draw-list cap, and therefore the instance count the TLAS is sized for.
// Was 4096, which Electric Dreams passes mid-stream (~6,370 resident entities): draws past it were
// dropped silently by submit(), so those entities still rendered (drawn elsewhere) but cast no
// cascade/GI shadow and wrote nothing to the voxel grid -- a shading bug, not a missing object.
// 16384 covers the demo with room to stream; costs a 1 MB TLAS instance array (64 B each), paid only
// when ray tracing is on.
constexpr u32 kMaxDraws = 16384;

// TLAS instance-mask lanes. A ray's own mask is ANDed with an instance's; a zero result skips it.
// Used to be 0xFF (every ray sees everything) -- correct only while the TLAS held nothing but opaque
// geometry. A shadow ray must still see a translucent pane (to attenuate through it) while a ray
// wanting only solid surfaces must exclude it. kRtMaskAll stays 0xFF, so an existing TraceRayInline
// call is unchanged until it deliberately narrows.
// Instances in the translucent lane this build; reported once (and on change) since "is the pane in
// the structure" is the first question a missing-translucent-shadow debug needs answered.
u32 tlasTranslucentThisBuild_ = 0;
u32 tlasTranslucentLogged_    = 0;
// Alpha-MASKED instances this build. Reported for the same reason and separately, because the two
// lanes answer different questions: a translucent pane is in the structure so a shadow ray can
// attenuate THROUGH it, while a cutout instance is there so any ray can see the holes in it. Both
// give up the hardware's any-hit skip, so knowing how many there are is the first thing a
// "why did the foliage get expensive" question needs.
u32 tlasAlphaMaskedThisBuild_ = 0;
u32 tlasAlphaMaskedLogged_    = 0;
constexpr u32 kRtMaskOpaque      = 0x01;
constexpr u32 kRtMaskTranslucent = 0x02;
// The viewer's own first-person body -- see AVER_RT_MASK_OWNER_HIDDEN in voxi.hlsl (must match; full
// reasoning there). Opaque geometry every ray may hit except the ray-driven primary one.
constexpr u32 kRtMaskOwnerHidden = 0x04;
constexpr u32 kRtMaskAll         = 0xFF;

// The material system's sampler register. materialShaderDefines() is told the same number.
constexpr u32 kMaterialSamplerSlot = 2;

// Fills a layout's static samplers: s0 the radiance volume, s1 the shadow comparison, s2 the
// material system's surface sampler.
void giSamplers(rhi::PipelineLayout& l) {
    l.samplers[0].filter  = rhi::Filter::Linear;
    l.samplers[0].address = rhi::AddressMode::Clamp;
    l.samplers[0].maxLod  = FLT_MAX;
    l.samplers[1].filter  = rhi::Filter::ComparisonLinear;
    l.samplers[1].address = rhi::AddressMode::Clamp;
    l.samplers[1].compare = rhi::CompareOp::LessEqual;
    l.samplers[kMaterialSamplerSlot].filter        = rhi::Filter::Anisotropic;
    l.samplers[kMaterialSamplerSlot].address       = rhi::AddressMode::Wrap;
    l.samplers[kMaterialSamplerSlot].maxAnisotropy = 8;
    l.samplerCount = kMaterialSamplerSlot + 1;
}

// TWO SLOTS WIDER THAN kGiSrvCount, DELIBERATELY. kGiSrvCount/kGiUavCount (VoxiGiShaders.hpp) are the
// union SandboxApp.cpp's GPU per-cluster path sizes its own binding set against
// (`kClusterGiSrvBase + voxi::kGiSrvCount`); that caller's register math must not move, so widening
// kGiSrvCount itself would silently rebase every register past it in a file this change never touches.
// bindGiResources() below only fills the cluster path's first two slots (volume, shadow map), so
// extra slots stay invisible to it regardless of how far this constant grows.
// t9 (material table), t10 (blended-pass backdrop: the opaque scene copied before translucency
// replays, so glass can tint per channel instead of through one blend alpha) and t11 (the
// sky-occlusion history's read side) are Voxi-only under the same reasoning, and so are the two
// newest ones: t12/t13, RTXDI ReSTIR GI's previous-frame surface history (read side, position and
// normal split across a PAIR of textures -- see giSurfPosHist_ in VoxiRenderer.hpp for why one
// alone could not hold both: rhi::Format has no four-channel 32-bit float).
//
// +8, NOT +7, AS OF U1/2.9 (the wave-2 optimisation plan): t16 is the newest slot, the
// half-resolution ReSTIR VISIBILITY history's read side (giVisHist_ in VoxiRenderer.hpp) -- see
// giVisHistWanted()'s own comment for why it is its own optional slot rather than riding t12/t13's
// giRestirWanted() condition.
//
// +9, NOT +8, AS OF THE OCCLUSION-AWARE FOG DESIGN: t17 is the AIR SKY-VISIBILITY volume's read
// side (airVisTex_ in VoxiRenderer.hpp) -- a fixed 32^3 single-channel volume, independent of
// voxelResolution, that CSAirVis writes and the shade passes sample through voxiAirVisibility()
// (voxi.hlsl) to attenuate height/aerial fog's in-scatter term by how much sky the air at that point
// actually sees. Bound to a 1x1x1 placeholder (airVisPlaceholder_) whenever voxi.fogOcclusion is off
// or the real texture has not been created yet -- the shader treats "GetDimensions() <= 1" as "no
// volume" and reads visibility 1, the identical dimension test t14/t15 above already use for their
// own optional absence.
//
// +11, NOT +9, AS OF LOCAL LIGHTS (LAMPS): t18 is this frame's local-light list (gRdLocalLights, a
// StructuredBuffer of RdLocalLight, see buildLocalLights) and t19 the local-light visibility history's
// read side (rdLocalHist_ in VoxiRenderer.hpp, u19's ping-pong twin). Both always hold a valid
// descriptor -- a placeholder whenever the real one is absent -- and the shaders touch neither unless
// gCameraMedium.z (the light count) is non-zero this frame.
constexpr u32 kVoxiSrvCount = kGiSrvCount + 11;

// The denoiser index Voxi asks NRD to run. create() is handed exactly one kind
// (ReblurDiffuseOcclusion), so this is 0 -- named rather than written as a bare literal at the
// call site, because the two must agree and nothing else would say so.
constexpr u32 kNrdAoDenoiser[] = {0u};
// Both denoisers, in the order create() is handed them: 0 occlusion, 1 diffuse radiance. Voxi runs
// this pair only when ReSTIR GI is the active estimator -- the radiance signal does not exist
// otherwise, and asking NRD for a denoiser whose input is blank is the one way to get a confident
// wrong image out of it.
constexpr u32 kNrdAoAndGiDenoisers[] = {0u, 1u};
// THE DIFFUSE RADIANCE DENOISER ALONE. The two signals are INDEPENDENTLY available and were wrongly
// treated as though the GI one implied the other: sky occlusion is Epic-tier only, ReSTIR GI is not,
// so a project can want GI denoising with no occlusion signal in the frame at all.
constexpr u32 kNrdGiDenoiser[] = {1u};

// THE SAME ARGUMENT ON THE UAV SIDE, and it needs its own constant for a reason the SRV side
// already documents. SandboxApp.cpp's cluster path sets `bsd.uavCount = voxi::kGiUavCount` and
// then fills uavKinds[0..3] BY HAND; widening kGiUavCount itself would grow that count without
// growing the kinds it declares, and Vulkan refuses a set whose slot types do not match the
// pipeline layout -- an undeclared fifth slot there would be a validation failure in a file this
// change never touches. Voxi sizes its OWN table against this instead, so u4 is invisible to it.
// +5, not +2: u4/u5 are the sky-occlusion pair as before, u6 is RTXDI's own reservoir
// StructuredBuffer (RTXDI_PackedGIReservoir, 32 bytes/element -- see giReservoirs_) and u7/u8 are
// giSurfPosHist_/giSurfNrmHist_'s write sides, the UAV twins of t12/t13 above.
//
// +7, NOT +6, AS OF U1/2.9: u10 is the newest slot, the half-resolution ReSTIR VISIBILITY history's
// write side (giVisHist_'s UAV twin of t16 above) -- same optional-slot reasoning as t16's own
// comment gives.
//
// +9, NOT +7, AS OF THE STAGED RAY-DRIVEN PASSES MILESTONE: u11/u12 are the visibility-record buffer
// and the sun-visibility texture CSRdVisibility/CSRdShadow/the AVER_RD_SPLIT branch of PSRayDriven
// pass between themselves (rdVisBuf_/rdSunVisTex_) -- bound in EVERY Voxi binding set, not only while
// voxi.rayDrivenStages is 1 or 2, for the same reason u4/u5's sky-occlusion pair is declared here rather
// than only allocated when wanted: Tier 1 hardware needs a valid descriptor of the declared KIND in
// every slot the pipeline layout reserves, staged or not, so a placeholder stands in exactly like
// voxelAccumPlaceholder_ does for u1 when the real resource does not exist.
//
// +11, NOT +9, AS OF MILESTONE 2: u13/u14 are CSRdGi's and CSRdSkyOcc's own outputs (rdGiTex_/
// rdAoTex_) -- the lighting-stage twins of u11/u12 immediately above, bound in EVERY Voxi binding set
// on the identical "declared unconditionally, placeholder when the real resource is absent" contract,
// for the identical Tier 1 reason.
//
// +12, NOT +11, AS OF MILESTONE 3: u15 is CSRdRefl's own output (rdReflTex_) -- the reflection twin of
// u13/u14 immediately above, bound in EVERY Voxi binding set on the identical contract, for the
// identical Tier 1 reason. No SRV twin, exactly like u11-u14.
//
// +13, NOT +12, AS OF THE OCCLUSION-AWARE FOG DESIGN: u16 is CSAirVis's own output (airVisTex_,
// t17's UAV twin, SAME resource -- unlike u11-u15 this one DOES have an SRV, because the shade
// passes read it through gAirVis at t17 while CSAirVis itself only ever writes it). Bound to the
// SAME 1x1x1 placeholder t17 falls back to whenever the real texture does not exist, so a Tier 1
// device always has a valid UAV descriptor here regardless of voxi.fogOcclusion.
//
// +15, NOT +13, AS OF THE SUB-STAGE SPLITS (Settings::rayDrivenShadowTiles / rayDrivenGiSplit): u17
// is CSRdGiTrace's own candidate buffer (gRdGiCand, RdGiCand, 48 bytes/element -- voxi_restir.hlsli),
// read back by the AVER_GI_SPLIT branch of CSRdGi; u18 is CSRdShadowProbe's own per-8x8-tile verdict
// buffer (gRdShadowTiles, one uint/tile), read back by the AVER_RD_SHADOW_TILES branch of CSRdShadow.
// Same "always declared, allocated unconditionally alongside u11-u16" contract as the rest of the
// staged group -- see rdGiCandBuf_/rdShadowTileBuf_'s own comment (VoxiRenderer.hpp) for why neither
// is gated on the narrower setting that consumes it.
//
// +16, NOT +15, AS OF LOCAL LIGHTS (LAMPS): u19 is the write side of the local-light history pair (t19
// above is its read side), written by whichever pass shades the opaque scene with lamps -- CSRdLocalLights
// (staged), the single-pass PSRayDriven, or PSMainVoxi's opaque RT variants (raster) -- a = the
// accumulated visibility (rgb unused). Stage B and the
// staged blended replay read it back through this same UAV register, the way they read u12.
constexpr u32 kVoxiUavCount = kGiUavCount + 16;

// What KIND of resource each of table 0's slots holds, read by both giLayout() (every pipeline) and
// createVoxelVolume()'s BindingSetDesc (the set those pipelines bind) -- Vulkan refuses a set whose
// types differ from the pipeline layout's, so the two must agree. Before this existed the pipeline
// side reflected kinds out of the shaders, which can't see a slot no shader uses (t2, the TLAS).
// See PipelineLayout's own comment on slotKindsDeclared.
void giTableKinds(rhi::SlotKind* srv, rhi::SlotKind* uav) {
    srv[0] = rhi::SlotKind::Texture3D;              // t0 volume, whole chain
    srv[1] = rhi::SlotKind::Texture2D;              // t1 shadow map
    srv[2] = rhi::SlotKind::AccelerationStructure;  // t2 TLAS, filled once one exists
    // Null-filled until the table exists. Tier 1 requires a valid descriptor of the right KIND in
    // every declared slot, so these must be declared as structured buffers even while empty.
    srv[3] = rhi::SlotKind::StructuredBuffer;       // t3 flat vertices
    srv[4] = rhi::SlotKind::StructuredBuffer;       // t4 flat indices
    srv[5] = rhi::SlotKind::StructuredBuffer;       // t5 per-instance records
    srv[6] = rhi::SlotKind::Texture2D;              // t6 ray-traced shadow history (read)
    srv[7] = rhi::SlotKind::Texture2D;              // t7 ray-traced reflection history (read)
    srv[8] = rhi::SlotKind::Texture2D;              // t8 GI-only shadow map
    // t9, kVoxiSrvCount's own reason to exist: the dense per-frame material table a ray hit indexes
    // through RtInstance::materialIndex (gRtMaterials in VoxiShaders.hpp). Same null-fill-until-built
    // story as t3..t5 above -- see buildMaterialTable().
    srv[9] = rhi::SlotKind::StructuredBuffer;       // t9 dense per-frame material constants
    // t10: the opaque scene copied just before the blended replay (IDevice::sceneColorBackdropTexture).
    // Null until the device has one -- before the first resize, under MSAA, or on a backend that does
    // not implement it -- and the shader tests for that rather than assuming.
    srv[10] = rhi::SlotKind::Texture2D;             // t10 blended backdrop
    // t11/u4: the AMBIENT OCCLUSION history pair, the third of three and the reason
    // kVoxiSrvCount/kVoxiUavCount exist. Same shape and same lockstep as the shadow and
    // reflection pairs above -- see ensureShadowHistory, which creates all six together.
    srv[11] = rhi::SlotKind::Texture2D;             // t11 sky-occlusion history (read)
    // t12/t13, u7/u8: RTXDI ReSTIR GI's previous-frame SURFACE history, split across TWO textures
    // (see giSurfPosHist_/giSurfNrmHist_ in VoxiRenderer.hpp for why) -- a FOURTH AND FIFTH
    // ping-ponged pair, created and destroyed alongside the other three in ensureShadowHistory but
    // gated on giRestirWanted() rather than rayTracingWanted()/aoHistoryWanted(), since ReSTIR GI
    // is its own switch (Settings::giMode), independent of which ray-tracing tier is active.
    srv[12] = rhi::SlotKind::Texture2D;             // t12 GI-restir surface POSITION history (read)
    srv[13] = rhi::SlotKind::Texture2D;             // t13 GI-restir surface NORMAL history (read)
    // t14: NRD's DENOISED sky occlusion, and it is the only SRV here that is routinely ABSENT.
    // The pass is optional at three independent levels -- no NRD in the build, a backend whose
    // descriptor model refuses NRD's register spaces (Vulkan), or no G-buffer to feed it -- so the
    // shader must not assume it is bound. It tests GetDimensions() rather than reading a cbuffer
    // flag, the same trick averBlendBackdropValid already uses for t10: a null-filled Texture2D
    // reports zero dimensions, which is a signal the descriptor already carries and costs no
    // fourth mirror of the constant block to express (see aver-voxi-cbuffer-three-mirrors).
    srv[14] = rhi::SlotKind::Texture2D;             // t14 NRD-denoised sky occlusion (read)
    // t15/u9: the ReSTIR GI radiance, out to NRD and back. u9 is written by giRestirIndirect
    // (rgb = indirect diffuse, a = normalised hit distance) and t15 is the same signal one frame
    // later through REBLUR_DIFFUSE. Both absent unless ReSTIR GI and NRD are BOTH running, which
    // is why the shader tests dimensions rather than trusting the slot.
    srv[15] = rhi::SlotKind::Texture2D;             // t15 NRD-denoised ReSTIR GI radiance (read)
    // t16/u10: U1/2.9's half-resolution ReSTIR VISIBILITY history pair (giVisHist_ in
    // VoxiRenderer.hpp) -- bound only while giVisHistWanted() (Settings::giRestirVisibility ==
    // HalfResolution), the identical optional-slot shape t14/t15 above already have: the shader
    // tests GetDimensions() rather than trusting the slot, because Full/Reconstructed/NoRay never
    // allocate this pair at all.
    srv[16] = rhi::SlotKind::Texture2D;             // t16 ReSTIR visibility half-res history (read)
    // t17/u16: the AIR SKY-VISIBILITY volume -- see kVoxiSrvCount's own comment above for what it is.
    // A Texture3D SRV, like t0, not a Texture2D -- it is sampled trilinear over the SAME voxel volume
    // space t0 occupies, just at its own fixed 32^3 resolution.
    srv[17] = rhi::SlotKind::Texture3D;             // t17 air sky-visibility volume (read)
    // t18/t19/u19: LOCAL LIGHTS (LAMPS) -- see kVoxiSrvCount/kVoxiUavCount's own comments above. t18
    // is a StructuredBuffer like t3-t5/t9; t19 is a Texture2D history read side like t6/t11.
    srv[18] = rhi::SlotKind::StructuredBuffer;      // t18 local-light list (gRdLocalLights)
    srv[19] = rhi::SlotKind::Texture2D;             // t19 local-light history (read)
    uav[0] = rhi::SlotKind::Texture3D;              // u0 volume mip 0
    uav[1] = rhi::SlotKind::Texture3D;              // u1 injection accumulator
    uav[2] = rhi::SlotKind::Texture2D;              // u2 ray-traced shadow history (write)
    uav[3] = rhi::SlotKind::Texture2D;              // u3 ray-traced reflection history (write)
    uav[4] = rhi::SlotKind::Texture2D;              // u4 sky-occlusion history (write)
    // u5: the sky-occlusion ray's HIT DISTANCE, written raw and never read back by Voxi -- see
    // rtAoHitDist_ in VoxiRenderer.hpp. Allocated and bound under exactly the same condition as
    // the u4/t11 pair (aoHistoryWanted()), which is why gRtDenoiseParams.w speaks for it too and
    // no fourth flag is needed.
    uav[5] = rhi::SlotKind::Texture2D;              // u5 sky-occlusion hit distance (write)
    // u6: RTXDI's OWN reservoir storage -- RWStructuredBuffer<RTXDI_PackedGIReservoir>, ONE buffer
    // holding both of rtxdi::c_NumReSTIRGIReservoirBuffers' ping-pong copies via
    // RTXDI_ReservoirPositionToPointer's array-index term, never rebound mid-session the way a
    // texture pair is -- see giReservoirs_.
    uav[6] = rhi::SlotKind::StructuredBuffer;       // u6 GI-restir reservoir buffer
    uav[7] = rhi::SlotKind::Texture2D;              // u7 GI-restir surface POSITION history (write)
    uav[8] = rhi::SlotKind::Texture2D;              // u8 GI-restir surface NORMAL history (write)
    uav[9] = rhi::SlotKind::Texture2D;              // u9 ReSTIR GI radiance + hit distance (write)
    uav[10] = rhi::SlotKind::Texture2D;             // u10 ReSTIR visibility half-res history (write)
    // u11/u12: STAGED RAY-DRIVEN PASSES (milestone 1) -- the visibility record buffer and the sun
    // visibility texture, always declared exactly like every other optional pair above regardless of
    // whether voxi.rayDrivenStages is 1 or 2 this session. u11 is a StructuredBuffer of uint4 records
    // (rdVisBuf_, one per pixel of the render target); u12 is an RW Texture2D (rdSunVisTex_, RGBA16F,
    // rgb = sun transmittance). Both are read AND written through these SAME uav registers by every
    // stage that touches them (CSRdVisibility, CSRdShadow, the AVER_RD_SPLIT branch of PSRayDriven) --
    // no SRV twin exists for either, which is why kVoxiSrvCount above did not have to move.
    uav[11] = rhi::SlotKind::StructuredBuffer;      // u11 ray-driven visibility record buffer
    uav[12] = rhi::SlotKind::Texture2D;             // u12 ray-driven sun visibility (RW)
    // u13/u14: MILESTONE 2's own lighting-stage outputs -- CSRdGi's indirect-diffuse result
    // (rdGiTex_, RGBA16F, rgb = ReSTIR GI's diffuse) and CSRdSkyOcc's transmittance (rdAoTex_,
    // RGBA16F, r = occlusion). Same "always declared, placeholder when the real resource is absent"
    // contract as u11/u12 above, and the same "no SRV twin" shape -- both are RW-only, read and
    // written through these UAV registers alone by CSRdGi/CSRdSkyOcc and the AVER_RD_SPLIT branch of
    // PSRayDriven.
    uav[13] = rhi::SlotKind::Texture2D;             // u13 ray-driven GI indirect diffuse (RW)
    uav[14] = rhi::SlotKind::Texture2D;             // u14 ray-driven sky occlusion (RW)
    // u15: MILESTONE 3's own lighting-stage output -- CSRdRefl's ray-traced reflection result
    // (rdReflTex_, RGBA16F, rgb = the traced/sky reflection radiance, a = the stage's own "I traced
    // this pixel" decision). Same "always declared, placeholder when the real resource is absent"
    // contract as u11-u14 above, and the same "no SRV twin" shape -- RW-only, read and written through
    // this UAV register alone by CSRdRefl and the AVER_RD_SPLIT branch of PSRayDriven.
    uav[15] = rhi::SlotKind::Texture2D;             // u15 ray-driven reflection (RW)
    // u16: CSAirVis's own write side, t17's UAV twin -- see kVoxiUavCount's own comment above.
    uav[16] = rhi::SlotKind::Texture3D;             // u16 air sky-visibility volume (write, CSAirVis)
    // u17/u18: THE SUB-STAGE SPLITS' OWN BUFFERS (Settings::rayDrivenShadowTiles / rayDrivenGiSplit) --
    // see kVoxiUavCount's own comment above for what each holds. Both StructuredBuffer, like u11 and
    // u6, and no SRV twin, exactly like u11-u15.
    uav[17] = rhi::SlotKind::StructuredBuffer;      // u17 GI-trace candidate buffer (gRdGiCand)
    uav[18] = rhi::SlotKind::StructuredBuffer;      // u18 shadow-probe tile verdicts (gRdShadowTiles)
    uav[19] = rhi::SlotKind::Texture2D;             // u19 local-light history (write)
    static_assert(kVoxiSrvCount == 20 && kVoxiUavCount == 20 && kGiSrvCount == 9 && kGiUavCount == 4,
                  "giTableKinds fills exactly kVoxiSrvCount SRVs and kVoxiUavCount UAVs; widen "
                  "those, never kGiSrvCount/kGiUavCount -- those two are the union SandboxApp.cpp's "
                  "cluster path reserves at its own base and fills kinds for by hand, so growing "
                  "them without widening that file in the same change is a Vulkan set/layout "
                  "type mismatch rather than a compile error");
}

// Returns the pipeline layout every Voxi raster pipeline declares. `bindlessTextures` non-zero
// appends the ray path's texture array as one more root parameter in its own register space; every
// existing caller passes 0 and gets the identical root signature as before -- the whole point of a
// defaulted argument here rather than a second function.
rhi::PipelineLayout giLayout(u32 bindlessTextures = 0) {
    rhi::PipelineLayout l{};
    // t0 volume, t1 shadow map, t2 TLAS, t3 vertices/t4 indices/t5 instances (a reflection ray's flat
    // geometry), t6/t7 ray-traced shadow/reflection history (last frame's), t8 GI-only shadow map, t9
    // dense per-frame material table. The material texture table is BASED on this count, not a fixed
    // register, so widening table 0 rebases it automatically.
    // Used to be typed twice -- a bare "9" here and again in createVoxelVolume's BindingSetDesc, with
    // nothing tying them together at compile time (a mismatch was a descriptor-table error at draw
    // time, not a build failure). kGiSrvCount/kGiUavCount type the shared part once now.
    l.srvCount = kVoxiSrvCount;
    l.uavCount = kVoxiUavCount;   // u0 volume mip 0, u1 injection accumulator, u2 shadow, u3 reflection,
                                   // u4 sky-occlusion, u5 sky-occlusion hit distance, u6 GI-restir
                                   // reservoir buffer, u7/u8 GI-restir surface position/normal
                                   // history, u9 ReSTIR GI radiance, u10 ReSTIR visibility half-res
                                   // history, u11/u12 the staged ray-driven visibility record buffer
                                   // and sun visibility texture, u13/u14 the staged ray-driven GI
                                   // indirect diffuse and sky occlusion outputs, u15 the staged
                                   // ray-driven reflection output (all this frame's), u16 the air
                                   // sky-visibility volume CSAirVis writes (t17's UAV twin), u17/u18
                                   // the sub-stage splits' own GI-trace candidate and shadow-probe
                                   // tile buffers, u19 the local-light history's write side (t19's
                                   // ping-pong twin)
    // Table 1: the material's textures, based at t(kVoxiSrvCount) -- the root-signature builder
    // accumulates srvBase across tables, so a register number derived from a comment instead of this
    // value goes wrong the moment kVoxiSrvCount grows past kGiSrvCount.
    l.srvCount1 = pbr::kMaterialSrvCount;
    l.constantDwords[rhi::kObjectConstantRegister] = rhi::kObjectConstantDwords;
    giSamplers(l);
    // Table 1's textures are ordinary Texture2D, SlotKind's default, so only table 0 needs filling
    // in. Declaring kinds at all is what stops the backend from reflecting them (which got t2 wrong).
    l.slotKindsDeclared = true;
    giTableKinds(l.srvKinds, l.uavKinds);
    l.bindlessTextureCount = bindlessTextures;
    return l;
}

// Returns the prelude Voxi's HLSL compiles on top of: the RHI's shared declarations, then the
// material system's BRDF and Aver* contract, then -- when the project has any -- the one
// averEvalMaterial its material GRAPHS compile to. Owned by a static, because the caller borrows it.
// AVER_MATERIAL_GRAPH is #defined IN THE TEXT, not passed as a -D: it lands between the two preludes,
// removing the stock averEvalMaterial so the generated one can replace it, and any shader seeing the
// graph function necessarily also saw the define. A -D added to every shader compiled against this
// prelude risks one missed shader carrying both definitions and failing to link far from the cause.
// Rebuilt when the registry's REVISION moves, not once per process -- materials load long after these
// pipelines exist, so a prelude fixed at startup would never contain a graph. Keyed on revision, not
// content, so the common case (no graphs) rebuilds nothing.
// Voxi's shader body, read from shaders/voxi.hlsl.
// THE ACCESSOR IS HERE, NOT IN VoxiShaders.hpp: tests/render.voxi/src/VoxiRtSeqTest.cpp reads the same
// source to assert its C++ mirror still matches, linking Aver.Core alone -- rhi::shaderFile in the
// header would drag in Aver.RHI. It reads modules/render.voxi/shaders/voxi.hlsl directly through
// AVER_REPO_ROOT instead.
const char* voxiHlsl() {
    // No static: the loader owns the cache and reloadShaderFiles() clears it. A static here would
    // survive a reload and hand back the shader that was read at startup for the rest of the run.
    return rhi::shaderFile("voxi.hlsl").c_str();
}

const char* voxiShaderPrelude() {
    static std::string s;
    static u64 built = ~0ull;
    // BOTH revisions, because either can move independently: a project can register a material
    // graph, and --shader-source can rewrite the prelude underneath it.
    const u64 rev = pbr::materialGraphs().revision() * 1000003ull + rhi::shaderFileRevision();
    if (built != rev) {
        s = std::string(rhi::sharedShaderPrelude());
        const std::string& graphs = pbr::materialGraphs().hlsl();
        if (!graphs.empty()) s += "\n#define AVER_MATERIAL_GRAPH 1\n";
        s += pbr::materialShaderPrelude();
        s += graphs;
        built = rev;
    }
    return s.c_str();
}

// Writes the tail of the per-draw block: shading model, then its default parameters.
void writeShadingConstants(f32* block) {
    const u32 model = 0;   // AVER_MODEL_STANDARD
    std::memcpy(block + 24, &model, sizeof(model));   // a uint in the block, not a converted float
    block[25] = 0.04f;
    block[26] = 1.0f;
    block[27] = 0.0f;
    block[28] = block[29] = block[30] = block[31] = 0.0f;   // emissive
}

// The shader model every Voxi pipeline that is not a mesh shader or a RayQuery variant asks for.
constexpr u32 kBaseSm = 51;

// Compiles Voxi shaders and destroys every one of them when it leaves scope.
struct ShaderScope {
    explicit ShaderScope(rhi::IResourceFactory& r) : res(r) {}
    ~ShaderScope() { for (rhi::ShaderHandle h : owned) res.destroyShader(h); }
    ShaderScope(const ShaderScope&) = delete;
    ShaderScope& operator=(const ShaderScope&) = delete;

    // Compiles one entry point of Voxi's HLSL. Returns 0 on failure.
    rhi::ShaderHandle operator()(const char* entry, rhi::ShaderStage stage, u32 sm,
                                 const char* defines) {
        rhi::ShaderDesc sd;
        sd.source  = voxiHlsl();
        sd.prelude = voxiShaderPrelude();
        sd.entry   = entry;
        sd.stage   = stage;
        sd.minShaderModel = sm;
        sd.defines = defines;
        const rhi::ShaderHandle h = res.createShader(sd);
        if (h) owned.push_back(h);
        return h;
    }

    rhi::IResourceFactory& res;
    std::vector<rhi::ShaderHandle> owned;
};

} // namespace

// Creates every GPU resource Voxi needs. Returns false, and names the first missing handle, if the
// device cannot support the baseline.
bool VoxiRenderer::init(rhi::IDevice& device) {
    dev_ = &device;
    res_ = device.resources();
    if (!res_) {
        AVER_WARN("[Voxi] init declined: backend exposes no resource factory (no GPU support)");
        return false;
    }
    caps_ = device.caps();

    if (!materials_.init(device, giLayout().srvCount))
        AVER_WARN("[Voxi] the material system declined to initialise; draws fall back to an unbound table 1");

    createShadowResources();
    createVoxelVolume(settings_.voxelResolution);
    createPipelines();

    // OCCLUSION-AWARE FOG: airVisPso_ has just been settled by createPipelines() above (device
    // shader model, DXC availability) -- this is the first point airVisWanted() can be answered
    // honestly, so it is where the placeholder createVoxelVolume() bound at t17/u16 gets upgraded to
    // the real kAirVisResolution^3 texture, if the setting and the device both allow it. A failure
    // here is not fatal to init() -- the placeholder stays bound and fog stays exactly as unoccluded
    // as it was before this feature existed, the same "wrong but running beats a dead renderer" shape
    // giShadowPso_'s own soft failure documents above.
    if (!ensureAirVis())
        AVER_WARN("[Voxi] air sky-visibility volume unavailable at startup; fog stays unoccluded "
                  "(voxi.fogOcclusion has no effect until it can be created)");

    const char* missing = nullptr;
    if (!shadowTex_)             missing = "shadow texture";
    else if (!voxelTex_)         missing = "voxel volume";
    else if (!voxelAccumTex_)    missing = "injection accumulator";
    else if (!bindings_)         missing = "main binding set";
    else if (!clearBindings_)    missing = "clear binding set";
    else if (!resolveBindings_)  missing = "resolve binding set";
    // OCCLUSION-AWARE FOG: airVisPlaceholder_ is created unconditionally inside createVoxelVolume(),
    // ahead of airVisPso_/airVisTex_ existing at all -- see its own comment there -- so it belongs in
    // this "always required" chain, unlike airVisPso_/airVisTex_ themselves, which are legitimately 0
    // on a device below SM 6.0 or with voxi.fogOcclusion off.
    else if (!airVisPlaceholder_) missing = "air sky-visibility placeholder";
    else if (mipBindings_.size() + 1 != voxelMips_) missing = "mip binding sets (count)";
    else if (!shadowPso_)        missing = "shadow pipeline";
    else if (!voxelPso_)         missing = "voxelise pipeline";
    else if (!clearPso_)         missing = "volume clear pipeline";
    else if (!resolvePso_)       missing = "injection resolve pipeline";
    else if (!mipPso_)           missing = "mip filter pipeline";
    else if (!debugPso_)         missing = "voxel debug pipeline";
    else if (!scenePso_)         missing = "scene pipeline";
    for (usize m = 0; !missing && m < mipBindings_.size(); ++m)
        if (!mipBindings_[m]) missing = "mip binding set";

    AVER_INFO("[Voxi] init: shadow tex={} volume={} accum={} ({}^3, {} mips) bindings={}/{}/{}/+{} "
              "pipelines shadow={}/{} voxel={} voxelMs={} clear={} resolve={} mip={} debug={} "
              "scene={}/{}/{}/{} blended={}/{}/{}/{} airVis={}/{} ({}^3)",
              shadowTex_, voxelTex_, voxelAccumTex_, voxelResBuilt_, voxelMips_,
              bindings_, clearBindings_, resolveBindings_, static_cast<u32>(mipBindings_.size()),
              // shadow is per-draw/instanced -- a zero in the second slot is shadowPass silently
              // falling back to one draw per instance, which is a 3% frame-time difference and
              // otherwise invisible. It cost an hour of misattributed measurement to learn that.
              shadowPso_, shadowInstancedPso_, voxelPso_, voxelMsPso_, clearPso_, resolvePso_, mipPso_, debugPso_,
              scenePso_, sceneMsPso_, sceneRtPso_, sceneMsRtPso_,
              // The blended family: all four legitimately zero on a device too old for AlphaBlend
              // to have failed on (it hasn't -- see createScenePipelines' own AVER_WARN if one did),
              // most commonly zero simply because vsMain/psVoxi themselves never compiled, in which
              // case scenePso_ above is already zero and `missing` below has already explained why.
              sceneBlendedPso_, sceneMsBlendedPso_, sceneRtBlendedPso_, sceneMsRtBlendedPso_,
              // airVisTex_ reads 0 here whenever voxi.fogOcclusion is off or airVisPso_ failed to
              // compile -- ensureAirVis() already ran above, so this reports the FINAL state, not the
              // placeholder createVoxelVolume() bound before it.
              airVisPso_, airVisTex_, kAirVisResolution);

    if (missing) {
        AVER_ERROR("[Voxi] init FAILED: {} has a zero handle", missing);
        shutdown();
        return false;
    }

    giReady_    = true;
    rtSupported_ = sceneRtPso_ != 0;

    // Created up front, and sized for the draw-list cap, so its t2 descriptor is never rewritten
    // while a frame that may be reading it is in flight.
    if (rtSupported_) {
        tlas_ = res_->createTlas(kMaxDraws);
        if (tlas_) res_->setSrvTlas(bindings_, 2, tlas_);   // t2, per the shader's register(t2)
        else {
            AVER_WARN("[Voxi] TLAS could not be created; ray-traced sun shadows stay off");
            rtSupported_ = false;
        }
    }

    if (materials_.ready())
        device.setDefaultDrawBinding(materials_.fallbackBindingSet(), &materials_.fallbackConstants(),
                                     sizeof(pbr::MaterialConstants));

    AVER_INFO("[Voxi] ready: conservative raster {}, mesh-shader variants {}, ray-tracing variants {}, "
              "blended (glass) variant {}, G-buffer variant {} (off by default -- dev_->gBufferEnabled() "
              "is what turns it on per frame; see pickGbuf())",
              caps_.conservativeRaster ? "on" : "off",
              (voxelMsPso_ && sceneMsPso_) ? "built" : "absent",
              rtSupported_ ? "built" : "absent",
              // Only the plain (non-mesh-shader, non-RT) blended twin gates this line -- it's the one
              // scenePipeline() falls back to from every other blended combination, so its absence
              // means "no translucent material draws through Voxi this run", not one lost twin.
              sceneBlendedPso_ ? "built" : "absent",
              // Same reasoning, one level over: sceneGbufPso_ is the one every other G-buffer
              // combination falls back to first, so it being absent is the one absence that means
              // "no pipeline in this run can ever write a G-buffer", not "one axis lost its twin".
              sceneGbufPso_ ? "built" : "absent");
    return true;
}

// Destroys every resource and returns the feature to its uninitialised state.
void VoxiRenderer::shutdown() {
    reportFrameTime("run total");
    // BEFORE the `if (!res_)` bail below, because the denoiser owns device resources of its own
    // (pipelines, two texture pools, a descriptor table per dispatch) and is the one object here
    // that would leak them on the path where this renderer never had a factory to begin with.
    nrd_.destroy();
    nrdActive_ = false;
    nrdOutput_ = 0;
    nrdGiOutput_ = 0;
    materials_.shutdown();
    if (!res_) { dev_ = nullptr; return; }
    for (rhi::BindingSetHandle s : mipBindings_) if (s) res_->destroyBindingSet(s);
    mipBindings_.clear();
    if (resolveBindings_) res_->destroyBindingSet(resolveBindings_);
    if (clearBindings_) res_->destroyBindingSet(clearBindings_);
    if (bindings_)      res_->destroyBindingSet(bindings_);
    resolveBindings_ = clearBindings_ = bindings_ = 0;

    const rhi::PipelineHandle psos[] = {shadowPso_, shadowInstancedPso_, giShadowPso_,
                                        giShadowInstancedPso_, voxelPso_, voxelMsPso_, mipPso_,
                                        clearPso_, resolvePso_, debugPso_, scenePso_, sceneMsPso_,
                                        sceneRtPso_, sceneMsRtPso_, sceneBlendedPso_,
                                        sceneMsBlendedPso_, sceneRtBlendedPso_, sceneMsRtBlendedPso_,
                                        depthPrepassPso_, scenePsoPrepassed_, sceneRtPsoPrepassed_,
                                        // rayDrivenPso_ was missing from this list before its G-buffer
                                        // twin existed, so every shutdown() leaked it; fixed alongside
                                        // adding rayDrivenGbufPso_ rather than leaving one handled.
                                        rayDrivenPso_,
                                        sceneGbufPso_, sceneMsGbufPso_, sceneRtGbufPso_, sceneMsRtGbufPso_,
                                        sceneBlendedGbufPso_, sceneMsBlendedGbufPso_,
                                        sceneRtBlendedGbufPso_, sceneMsRtBlendedGbufPso_,
                                        scenePsoPrepassedGbuf_, sceneRtPsoPrepassedGbuf_,
                                        // TWO PRE-EXISTING LEAKS CLOSED ALONGSIDE THE NEW HANDLE.
                                        // rayDrivenTexPso_ and sceneRtBlendedTexPso_ were both set
                                        // to 0 below without ever being destroyed -- the exact bug
                                        // the comment above says was fixed for rayDrivenPso_,
                                        // repeated twice in the same file and missed both times
                                        // because zeroing a handle LOOKS like releasing it.
                                        rayDrivenTexPso_, sceneRtBlendedTexPso_,
                                        rayDrivenTexGbufPso_,
                                        rayDrivenGbufPso_,
                                        // STAGED RAY-DRIVEN PASSES (milestone 1, extended by milestone
                                        // 2's rdGiCsPso_/rdSkyOccCsPso_, milestone 3's rdReflCsPso_,
                                        // and milestone 4's rdGiCbCsPso_): the six compute pipelines and
                                        // Stage B's two AVER_RD_SPLIT graphics twins -- added here, not
                                        // after, for the identical reason the comment above gives for
                                        // rayDrivenTexPso_/sceneRtBlendedTexPso_.
                                        rdVisCsPso_, rdShadowCsPso_, rdGiCsPso_, rdGiCbCsPso_,
                                        rdSkyOccCsPso_, rdReflCsPso_,
                                        rayDrivenSplitTexPso_, rayDrivenSplitTexGbufPso_,
                                        // SUB-STAGE SPLITS (Settings::rayDrivenShadowTiles /
                                        // rayDrivenGiSplit): the four extra compute pipelines, added
                                        // here for the identical reason the milestone comment
                                        // immediately above gives for its own six.
                                        rdShadowProbeCsPso_, rdShadowTiledCsPso_,
                                        rdGiTraceCsPso_, rdGiTraceCbCsPso_,
                                        rdGiSplitCsPso_, rdGiSplitCbCsPso_,
                                        // SUB-STAGE C (Settings::rayDrivenReflSplit): the same reason as
                                        // the SUB-STAGE SPLITS entry immediately above, for its own two
                                        // extra compute pipelines.
                                        rdReflSplitCsPso_, rdReflFilterCsPso_,
                                        // LOCAL LIGHTS (LAMPS): CSRdLocalLights, built beside the other
                                        // staged compute pipelines in createScenePipelines().
                                        rdLocalLightsCsPso_,
                                        // OCCLUSION-AWARE FOG: airVisPso_, created alongside mipPso_
                                        // in createPipelines() and never touched by
                                        // createScenePipelines()'s own hot-reload -- same lifetime as
                                        // mipPso_/clearPso_/resolvePso_, so it belongs in this list
                                        // rather than the stale[] one createScenePipelines() destroys.
                                        airVisPso_};
    for (rhi::PipelineHandle p : psos) if (p) res_->destroyPipeline(p);
    shadowPso_ = shadowInstancedPso_ = giShadowPso_ = giShadowInstancedPso_ = 0;
    voxelPso_ = voxelMsPso_ = mipPso_ = clearPso_ = resolvePso_ = debugPso_ = airVisPso_ = 0;
    scenePso_ = sceneMsPso_ = sceneRtPso_ = sceneMsRtPso_ = 0;
    sceneBlendedPso_ = sceneMsBlendedPso_ = sceneRtBlendedPso_ = sceneMsRtBlendedPso_ = 0;
    depthPrepassPso_ = scenePsoPrepassed_ = sceneRtPsoPrepassed_ = 0;
    rayDrivenPso_ = rayDrivenTexPso_ = 0;
    sceneRtBlendedTexPso_ = 0;
    sceneGbufPso_ = sceneMsGbufPso_ = sceneRtGbufPso_ = sceneMsRtGbufPso_ = 0;
    sceneBlendedGbufPso_ = sceneMsBlendedGbufPso_ = sceneRtBlendedGbufPso_ = sceneMsRtBlendedGbufPso_ = 0;
    scenePsoPrepassedGbuf_ = sceneRtPsoPrepassedGbuf_ = 0;
    rayDrivenGbufPso_ = rayDrivenTexGbufPso_ = 0;
    rdVisCsPso_ = rdShadowCsPso_ = rdGiCsPso_ = rdGiCbCsPso_ = rdSkyOccCsPso_ = rdReflCsPso_ = 0;
    rayDrivenSplitTexPso_ = rayDrivenSplitTexGbufPso_ = 0;
    rdShadowProbeCsPso_ = rdShadowTiledCsPso_ = 0;
    rdGiTraceCsPso_ = rdGiTraceCbCsPso_ = rdGiSplitCsPso_ = rdGiSplitCbCsPso_ = 0;
    rdReflSplitCsPso_ = rdReflFilterCsPso_ = 0;
    rdLocalLightsCsPso_ = 0;

    if (voxelAccumTex_) res_->destroyTexture(voxelAccumTex_);
    // W12: the placeholder that stands in for voxelAccumTex_ while it is freed -- see
    // manageInjectionAccumulator()'s own comment. Not owned by any binding set slot's lifetime the
    // way voxelAccumTex_ itself is thought about, so it needs its own explicit destroy here or it
    // leaks on every shutdown that ever exercised W12's free branch.
    if (voxelAccumPlaceholder_) res_->destroyTexture(voxelAccumPlaceholder_);
    voxelAccumPlaceholder_ = 0;
    // OCCLUSION-AWARE FOG: airVisTex_/airVisPlaceholder_, created alongside voxelTex_/
    // voxelAccumPlaceholder_ immediately above (see createVoxelVolume()/ensureAirVis()) and released
    // on the identical "not owned by any binding-set slot's own lifetime" reasoning as
    // voxelAccumPlaceholder_'s own comment gives.
    if (airVisTex_) res_->destroyTexture(airVisTex_);
    if (airVisPlaceholder_) res_->destroyTexture(airVisPlaceholder_);
    airVisTex_ = airVisPlaceholder_ = 0;
    airVisDirty_ = false;
    if (voxelTex_)  res_->destroyTexture(voxelTex_);
    if (shadowTex_) res_->destroyTexture(shadowTex_);
    if (giShadowTex_) res_->destroyTexture(giShadowTex_);
    voxelAccumTex_ = voxelTex_ = shadowTex_ = giShadowTex_ = 0;
    // W12's own ephemeral bookkeeping -- NOT giFreeAccumulator_/giBoundedDispatch_/giForceRebuild_
    // themselves, which are user-facing dials this function has never reset for any of their older
    // siblings (unlit_, coneTraceEnabled_, ...) and stay exactly as set across a re-init.
    giAccumWanted_ = false;
    giAccumRecreateFailedLogged_ = false;
    giQuietTicks_ = 0;
    // THE THREE HISTORY PAIRS, WHICH THIS FUNCTION HAS NEVER FREED. Six full-screen textures --
    // ensureShadowHistory's own comment prices four of them at ~225 MB at 3532x1987, and there are
    // six now -- released only when ray tracing was switched OFF at runtime, never on the way out.
    // Exactly the bug the pipeline list above records closing three times in this same function
    // ("zeroing a handle LOOKS like releasing it"), and the reason it hid here is the same: the
    // handles are cleared by the destructor's own member init, so nothing looked wrong.
    //
    // MATTERS BEYOND PROCESS EXIT: the editor can shut a render feature down and re-init it while
    // running, so this was a per-cycle leak of a third of a gigabyte, not a one-off at teardown.
    for (rhi::TextureHandle& t : rtShadowHist_) { if (t) res_->destroyTexture(t); t = 0; }
    for (rhi::TextureHandle& t : rtReflHist_)   { if (t) res_->destroyTexture(t); t = 0; }
    for (rhi::TextureHandle& t : rtAoHist_)     { if (t) res_->destroyTexture(t); t = 0; }
    if (rtAoHitDist_) { res_->destroyTexture(rtAoHitDist_); rtAoHitDist_ = 0; }
    // THE FOURTH AND FIFTH PAIRS, added alongside this same fix rather than after it -- not
    // repeating the "zeroing a handle looks like releasing it" mistake the comment above describes
    // closing three times in this file already.
    for (rhi::TextureHandle& t : giSurfPosHist_) { if (t) res_->destroyTexture(t); t = 0; }
    for (rhi::TextureHandle& t : giSurfNrmHist_) { if (t) res_->destroyTexture(t); t = 0; }
    // U1/2.11: THE SIXTH PAIR, added alongside this same fix rather than after it -- not repeating
    // the "zeroing a handle looks like releasing it" mistake the comment above the fourth/fifth pairs
    // describes closing three times in this file already.
    for (rhi::TextureHandle& t : giVisHist_)     { if (t) res_->destroyTexture(t); t = 0; }
    // LOCAL LIGHTS (LAMPS): the history pair, its placeholder, the light-list ring and ITS placeholder.
    // bindings_ is already gone (top of this function), so nothing names any of them any more.
    for (rhi::TextureHandle& t : rdLocalHist_)   { if (t) res_->destroyTexture(t); t = 0; }
    if (rdLocalHistPlaceholder_) { res_->destroyTexture(rdLocalHistPlaceholder_); rdLocalHistPlaceholder_ = 0; }
    for (rhi::BufferHandle& b : rdLocalLights_)  { if (b) res_->destroyBuffer(b); b = 0; }
    if (rdLocalLightsPlaceholder_) { res_->destroyBuffer(rdLocalLightsPlaceholder_); rdLocalLightsPlaceholder_ = 0; }
    rdLocalLightCapacity_ = rdLocalLightSlot_ = rdLocalLightCount_ = 0;
    rdLocalLightsBound_ = 0;
    rdLocalOutThisFrame_ = 0;
    rdLocalHistPrimed_ = false;
    rdLocalHistFrame_ = 0;
    if (giRadiance_) { res_->destroyTexture(giRadiance_); giRadiance_ = 0; }
    if (giReservoirs_) { res_->destroyBuffer(giReservoirs_); giReservoirs_ = 0; }
    giReservoirElemCapacity_ = 0;
    // STAGED RAY-DRIVEN PASSES (milestone 1, extended by milestone 2's rdGiTex_/rdAoTex_ pair and
    // milestone 3's rdReflTex_): rdVisBuf_/rdSunVisTex_/rdGiTex_/rdAoTex_/rdReflTex_ and their
    // placeholders. Not owned by any binding-set slot's lifetime the way voxelAccumTex_ itself is
    // thought about, same reason voxelAccumPlaceholder_'s own destroy above needs an explicit line
    // rather than being implied by anything else here.
    if (rdVisBuf_)    { res_->destroyBuffer(rdVisBuf_);  rdVisBuf_ = 0; }
    if (rdSunVisTex_) { res_->destroyTexture(rdSunVisTex_); rdSunVisTex_ = 0; }
    if (rdGiTex_)     { res_->destroyTexture(rdGiTex_);     rdGiTex_ = 0; }
    if (rdAoTex_)     { res_->destroyTexture(rdAoTex_);     rdAoTex_ = 0; }
    if (rdReflTex_)   { res_->destroyTexture(rdReflTex_);   rdReflTex_ = 0; }
    if (rdVisBufPlaceholder_)  { res_->destroyBuffer(rdVisBufPlaceholder_);  rdVisBufPlaceholder_ = 0; }
    if (rdSunVisPlaceholder_) { res_->destroyTexture(rdSunVisPlaceholder_); rdSunVisPlaceholder_ = 0; }
    if (rdGiPlaceholder_)     { res_->destroyTexture(rdGiPlaceholder_);     rdGiPlaceholder_ = 0; }
    if (rdAoPlaceholder_)     { res_->destroyTexture(rdAoPlaceholder_);     rdAoPlaceholder_ = 0; }
    if (rdReflPlaceholder_)   { res_->destroyTexture(rdReflPlaceholder_);   rdReflPlaceholder_ = 0; }
    // SUB-STAGE SPLITS' OWN BUFFERS AND PLACEHOLDERS (Settings::rayDrivenShadowTiles /
    // rayDrivenGiSplit), the identical "not owned by any binding-set slot's lifetime" reasoning as
    // the rest of this block immediately above.
    if (rdGiCandBuf_) { res_->destroyBuffer(rdGiCandBuf_); rdGiCandBuf_ = 0; }
    if (rdShadowTileBuf_) { res_->destroyBuffer(rdShadowTileBuf_); rdShadowTileBuf_ = 0; }
    if (rdGiCandBufPlaceholder_) { res_->destroyBuffer(rdGiCandBufPlaceholder_); rdGiCandBufPlaceholder_ = 0; }
    if (rdShadowTileBufPlaceholder_) { res_->destroyBuffer(rdShadowTileBufPlaceholder_); rdShadowTileBufPlaceholder_ = 0; }
    rdVisBufElemCapacity_ = 0;
    rdGiCandBufElemCapacity_ = rdShadowTileElemCapacity_ = 0;
    rdStagedW_ = rdStagedH_ = rdStagedRowPitch_ = 0;
    rdStagedFallbackLogged_ = false;
    rdStagedRunLogged_ = false;
    giHistValid_ = false;
    giHistPrimed_ = false;
    giVisHistValid_ = false;
    giVisHistPrimed_ = false;
    rtShadowHistW_ = rtShadowHistH_ = 0;
    rtHistWriteIdx_ = 0;
    rtHistValid_ = false;
    rtHistPrimed_ = false;

    // Acceleration structures are released with the factory itself: only the handles are dropped.
    blas_.clear();
    tlas_ = 0;
    // THE UNCHANGED GATE'S OWN SNAPSHOT, invalidated here for the reason blas_/tlas_ themselves are
    // cleared above: init() rebuilds both from nothing (a device reset, a resize that reaches this
    // path, or simply the editor cycling the feature), so a key matching whatever drawsPrev_ looked
    // like before this shutdown() must NOT be trusted to mean tlas_ (now zero, about to be recreated)
    // is still the answer -- see rtAccelSnapshotUnchanged()'s own force-rebuild conditions, none of
    // which would otherwise catch "the TLAS itself was thrown away without a single draw changing".
    rtAccelSnapValid_ = false;

    voxelMips_ = voxelResBuilt_ = 0;
    giReady_ = rtSupported_ = rtActive_ = rtLogged_ = false;
    draws_.clear();
    drawsPrev_.clear();
    rebuiltThisFrame_.clear();
    lastBlasRebuilds_ = 0xFFFFFFFFu;
    prevGroupCountThisBuild_.clear();
    prevGroupCountLastBuild_.clear();
    prevGroupOrdinal_.clear();
    prevTransformByKey_.clear();
    nextTransformByKey_.clear();
    rtInstancePrevWorld_.clear();
    prevTransformMemoryLogged_ = false;
    frameTimeMs_.clear();
    frameTimeLastNs_ = 0;
    frameTimeSeen_ = 0;
    res_ = nullptr;
    dev_ = nullptr;
}

void VoxiRenderer::setSettings(const Settings& s) {
    // Captured BEFORE the assignment. The ray-traced history buffers are allocated on the OFF->on
    // edge and released on the on->OFF edge, and this is the only place either edge is visible --
    // onRenderTargetsChanged sees a resize, not a settings change, so waiting for one would leave a
    // quarter of a gigabyte allocated (or missing) until the window happened to change size.
    const bool wasWanted = rayTracingWanted();
    // The ambient pair has its OWN edge: a tier moving Medium -> High starts tracing the ray without
    // changing whether ray tracing is on at all, and the pair would otherwise not appear until the
    // next resize.
    const bool wasAoWanted = aoHistoryWanted();
    // giRestirWanted()'s OWN edge, for the identical reason wasAoWanted exists: giMode can flip
    // independently of the rayTracing tier (a user turning ReSTIR GI on while ray tracing has
    // already been running for a while, or off while it stays on), and onRenderTargetsChanged only
    // ever sees a resize -- this is the one place that edge is visible at all.
    const bool wasGiRestirWanted = giRestirWanted();
    // U1/2.11: giVisHistWanted()'s OWN edge, for the identical reason wasGiRestirWanted exists --
    // giRestirVisibility_ can move to or away from HalfResolution independently of giMode/the
    // rayTracing tier, and onRenderTargetsChanged only ever sees a resize.
    const bool wasVisWanted = giVisHistWanted();
    // LOCAL LIGHTS (LAMPS): rdLocalHistWanted()'s own edge -- voxi.localLights moves it without a resize
    // (and without moving rayTracingWanted()), the same reason every edge captured here exists.
    const bool wasLocalHistWanted = rdLocalHistWanted();
    const u32 wasVis = giRestirVisibility_;
    // STAGED RAY-DRIVEN PASSES (milestone 1): rdVisBuf_/rdSunVisTex_'s OWN edge, for the identical
    // reason wasAoWanted/wasGiRestirWanted exist above -- voxi.rayDrivenStages can flip independently
    // of the rayTracing tier, and ray tracing can flip while rayDrivenStages was already 1 or 2, and
    // onRenderTargetsChanged only ever sees a resize.
    const bool wasRdStagedResourcesWanted = rdStagedResourcesWanted();
    // OCCLUSION-AWARE FOG: airVisWanted()'s OWN edge, for the identical reason wasAoWanted/
    // wasGiRestirWanted exist above -- Settings::fogOcclusion can flip independently of every other
    // setting captured here, and there is no resize event to catch it on.
    const bool wasAirVisWanted = airVisWanted();
    settings_ = s;
    // Applied here rather than only through the direct setters, so the editor's Rendering page and
    // the project manifest can drive them the same way every other setting already does; the direct
    // setters (setShadowRays, setPixelsPerRayTile) still own the actual clamping.
    setShadowRays(s.rtShadowRays);
    setPixelsPerRayTile(s.rtPixelsPerRayTile);
    rtShadowDenoise_ = s.rtShadowDenoise;
    rtRenderMode_    = s.rtRenderMode;
    giMode_          = s.giMode;
    // U1: clamped defensively even though Settings::clamp() (Voxi.cpp) already does the same thing --
    // a typo lands on Full (3), the corrected transport, never on 0/NoRay, which is what an
    // unclamped out-of-range value would silently decode as through the shader's `& 3u` mask (2.9's
    // bit table). std::min rather than a ternary, matching the idiom setGiUpdateInterval/
    // setShadowRays above already use for their own clamps.
    giRestirVisibility_ = std::min(s.giRestirVisibility, 3u);
    // Clamped defensively for the identical reason giRestirVisibility_ just above is -- Voxi.cpp's
    // setSettings already range-clamps Settings::giRestirSpatialSamples to [0,15], and std::min
    // repeats that ceiling here so this member can never disagree with givis::packAmbientW's own
    // `& 15u` mask of it even if a caller reached this field some other way than the clamped
    // setSettings.
    giRestirSpatialSamples_ = std::min(s.giRestirSpatialSamples, 15u);
    // Same defensive repeat of Voxi.cpp's own clamps, for the same reason as the three above.
    giRestirMaxHistory_     = std::min(s.giRestirMaxHistory, 63u);
    ptBounces_       = s.ptBounces;
    // LATCHED, not assigned. The pipelines this decides the shape of are built once; a later change
    // would leave the member disagreeing with the shaders actually compiled, which is worse than
    // ignoring it. Said out loud when it happens rather than dropped silently.
    if (!layeredBsdfLatched_) {
        layeredBsdf_ = (s.layeredBsdf != Quality::Off);
        layeredBsdfLatched_ = true;
    } else if (layeredBsdf_ != (s.layeredBsdf != Quality::Off) && !layeredBsdfWarned_) {
        // ONCE. The condition does not clear itself -- the setting disagrees with the shaders for the
        // whole session -- so without this the same line lands every time settings are applied.
        layeredBsdfWarned_ = true;
        AVER_WARN("[Voxi] layeredBsdf changed after the pipelines were built; it takes effect on the "
                  "next project load. The shaders compiled for this session are unchanged. "
                  "(said once)");
    }
    setGiUpdateInterval(s.giUpdateInterval);

    // U1/2.11: a genuine mode change resets the GI reservoir history, NRD's history and the
    // half-resolution visibility history together -- all three carry a stale answer to a question
    // the shader no longer asks the same way once visMode changes (2.8's precedence rule; the
    // identical reasoning setLightingLegacyBits' own R0/R2/R3 branch already gives for its bits).
    // Gated on giRestirWanted(): a change while ReSTIR GI itself is not running has nothing live to
    // invalidate. NOT resetGiHistory()/resetNrdHistory() themselves -- those are the NAMED,
    // user-facing console commands and each logs its own "console command" line; this is a
    // setSettings-driven edge, not a console action, so it logs its own line instead.
    if (giRestirVisibility_ != wasVis && giRestirWanted()) {
        giHistValid_ = false;
        giVisHistValid_ = false;
        nrd_.forceHistoryReset();
        AVER_INFO("[Voxi] ReSTIR visibility rays: {} -> {} (GI/NRD/visibility history reset)",
                  wasVis, giRestirVisibility_);
    }

    // Guarded on a real size: before the first onRenderTargetsChanged there is nothing to create at,
    // and that call will apply the current setting itself when it arrives.
    if ((rayTracingWanted() != wasWanted || aoHistoryWanted() != wasAoWanted ||
         giRestirWanted() != wasGiRestirWanted || giVisHistWanted() != wasVisWanted ||
         rdLocalHistWanted() != wasLocalHistWanted) &&
        rtHistWantW_ && rtHistWantH_)
        if (!ensureShadowHistory(rtHistWantW_, rtHistWantH_))
            AVER_ERROR("[Voxi] ray-traced history could not follow a ray-tracing setting change at {}x{}",
                       rtHistWantW_, rtHistWantH_);

    // STAGED RAY-DRIVEN PASSES (milestone 1): the identical guarded-on-a-real-size edge check as the
    // block immediately above, on rdStagedResourcesWanted()'s own edge rather than the four the block
    // above already covers.
    if (rdStagedResourcesWanted() != wasRdStagedResourcesWanted && rtHistWantW_ && rtHistWantH_)
        if (!ensureRdStagedResources(rtHistWantW_, rtHistWantH_))
            AVER_ERROR("[Voxi] staged ray-driven resources could not follow a settings change at {}x{}",
                       rtHistWantW_, rtHistWantH_);

    // OCCLUSION-AWARE FOG: airVisWanted()'s own edge -- no size to guard on, unlike the two blocks
    // above, since airVisTex_ is a fixed kAirVisResolution^3 regardless of viewport. Still guarded on
    // bindings_ existing (ensureAirVis() re-checks this itself; init() has not necessarily run yet on
    // the very first setSettings a project's loader issues before the device is ready).
    if (airVisWanted() != wasAirVisWanted)
        if (!ensureAirVis())
            AVER_ERROR("[Voxi] air sky-visibility volume could not follow a voxi.fogOcclusion change");

    // B4: REBLUR history/prepass tuning as LIVE dials. setSettings already runs every frame (this is
    // no new per-frame call site), so re-issuing setReblurTuning here -- rather than only once at NRD
    // creation, see applyReblurTuning's own comment -- is what makes a console-set
    // voxi.reblur{DiffusePrepassBlurRadius,MaxAccumulatedFrameNum,MaxStabilizedFrameNum} take effect
    // on the NEXT frame without tearing the NRD instance (and its accumulated history) down. Guarded
    // on nrd_.valid() only to skip the redundant call on every frame before NRD exists at all --
    // applyReblurTuning()/setReblurTuning() would no-op harmlessly either way.
    if (nrd_.valid()) applyReblurTuning();
}

// Places the GI volume: centre in world units, half-edge extent.
void VoxiRenderer::setVolume(const f32 center[3], f32 extent) {
    center_[0] = center[0]; center_[1] = center[1]; center_[2] = center[2];
    extent_ = extent;
}

// Sets the light axis the shadow cascades are fitted to. Colour and intensity are the device's, not
// this renderer's -- see the header.
void VoxiRenderer::setSunDirection(const f32 dirToLight[3]) {
    sunDir_[0] = dirToLight[0]; sunDir_[1] = dirToLight[1]; sunDir_[2] = dirToLight[2];
}

void VoxiRenderer::setDebugView(bool on) { debugView_ = on; }

void VoxiRenderer::setGiPoisonView(bool on) { giPoisonView_ = on; }

// See the header's own comment. NO LOG, the same shape as setGiPoisonView immediately above: this is
// a debug view reasserted from the console/--gi-vis-path-view slot every frame regardless of whether
// the user just touched it, and a log line on every one of those frames would be noise, not
// information.
void VoxiRenderer::setGiVisPathView(bool on) { giVisPathView_ = on; }

// See the header's own comment for what M5 prices. Guarded on an actual CHANGE, the REASSERT IDIOM
// setNrdLegacyCamera/setLightingLegacyBits just below already use: SandboxApp reasserts this from
// the console/--blended-gi slot every frame regardless of whether the user just touched it, and this
// switch carries no history-reset cost the way those two do -- it only changes which term a blended
// fragment's indirect diffuse reads THIS frame, so a plain reassign-and-log is enough.
void VoxiRenderer::setBlendedGiCone(bool on) {
    if (on == blendedGiCone_) return;
    blendedGiCone_ = on;
    AVER_INFO("[Voxi] blended-fragment indirect diffuse: {} (M5 pricing switch, console/--blended-gi)",
              on ? "voxel cone gather" : "ReSTIR (default)");
}

// See the header's own comment for what this switch reinstates and why. Guarded on an actual CHANGE
// (unlike setGiPoisonView above) because SandboxApp reasserts this from the console slot every frame
// regardless of whether the user just touched it (the same per-frame idiom
// editor::consoleGiPoisonViewSlot() uses) -- without the guard, EVERY frame would force an NRD
// history reset and no denoiser history would ever accumulate past one frame.
void VoxiRenderer::setNrdLegacyCamera(bool on) {
    if (on == nrdLegacyCamera_) return;
    nrdLegacyCamera_ = on;
    // The two encodings' previous cameras are shaped differently (the legacy one is last frame's
    // COMBINED viewProj; the fixed one is a factorised pair) -- reprojecting one frame's history
    // against the OTHER encoding's idea of "the previous camera" would silently blend the two rather
    // than compare them, so both the texture history and this renderer's own latch of NRD's previous
    // camera reset together, on the very frame the switch flips.
    nrd_.forceHistoryReset();
    nrdPrevCameraValid_ = false;
    nrdPrev2CameraValid_ = false;
    AVER_INFO("[NRD] camera encoding switched to {} by console command (voxi.nrdLegacyCamera); "
              "history reset on the next frame so the two encodings are never blended together.",
              on ? "the OLD, WRONG pre-fix encoding (comparison only)" : "the fixed encoding");
}

// See the header's own comment for the bit table and the contrast-fix plan's CONTRACT section
// (section 2) for exactly where each bit is read in the shaders. Guarded on an actual CHANGE, the
// same reason setNrdLegacyCamera above is: SandboxApp reasserts this from the console slot every
// frame regardless of whether the user just touched it, and resetting history on every one of those
// frames would mean neither GI nor NRD nor AO ever accumulates past one frame.
void VoxiRenderer::setLightingLegacyBits(u32 bits) {
    if (bits == lightingLegacyBits_) return;
    const u32 changed = bits ^ lightingLegacyBits_;
    lightingLegacyBits_ = bits;
    AVER_INFO("[Voxi] lighting legacy bits: ring={} doubleCount={} hitSky={} reuseVis={} cones={} "
              "blendedHistory={} nrdReadback={} (console)",
              (bits & 1u) ? 1 : 0, (bits & 2u) ? 1 : 0, (bits & 4u) ? 1 : 0, (bits & 8u) ? 1 : 0,
              (bits & 16u) ? 1 : 0, (bits & 32u) ? 1 : 0, (bits & 64u) ? 1 : 0);
    // Bit 64 (voxi.legacyNrdReadback) needs no reset: it changes only where the shader READS NRD's
    // output, never what any history accumulates.
    // R0/R2/R3 (bits 1, 4, 8): the ReSTIR estimator itself samples, adds or reuses differently under
    // these, so GI and NRD history accumulated on one side of the flip is a stale answer to a
    // question the shader no longer asks the same way -- same reasoning resetGiHistory's own
    // per-command comment gives for a console-driven reset.
    if (changed & (1u | 4u | 8u)) { resetGiHistory(); resetNrdHistory(); }
    // R0 (bit 1) also reshapes the sky-occlusion ray (voxi_rt.hlsli), whose reprojected history AO
    // tracks separately from the GI reservoir's.
    if (changed & 1u) resetAoHistory();
    // W6/M5 (4 b): bit 32 changes whether a blended (translucent) fragment writes the shadow,
    // reflection and AO histories at all this frame (PSMainVoxi's gAverHistoryWrite gate, voxi.hlsl)
    // -- flipping it makes every pixel any blended pane covers answer a different question about its
    // own cross-frame history than it did the frame before, the identical staleness resetGiHistory's
    // own per-command comment already gives for bits 1/4/8 above. GI and NRD ride along for the same
    // reason they do there; RT (shadow/reflection/AO) rides along too, unlike bits 1/4/8, because bit
    // 32 -- unlike those three -- changes what those three histories themselves hold, not only the
    // GI reservoir/NRD.
    if (changed & 32u) { resetGiHistory(); resetNrdHistory(); resetRtHistory(); }
}

// See the header's own comment for the shape every one of these five shares: flip an existing
// validity flag, touch no texture/buffer handle, log through AVER_INFO so the bisection survives even
// if the user closes the editor before reading the console's own scrollback.
void VoxiRenderer::resetGiHistory(bool quiet) {
    giHistValid_ = false;
    // U1/2.8: the half-resolution visibility pair rides along -- a legacy-bit flip (or any other
    // caller of this command) while giRestirVisibility_ is HalfResolution would otherwise leave
    // giVisReconstruct's own reprojection carrying a stale answer forward: f2Observed/f3Observed go
    // false wherever the legacy bit forces "no ray", so nothing refreshes the pair's EMA, and only
    // this flag stops reprojection from trusting it anyway.
    giVisHistValid_ = false;
    if (!quiet) AVER_INFO("[Voxi] GI ReSTIR reservoir history reset by console command (resetgihistory); "
              "next frame uses a fresh candidate only (expect one visibly noisier frame), both "
              "ping-pong slices clean within 2 frames.");
}

void VoxiRenderer::resetRtHistory(bool quiet) {
    rtHistValid_ = false;
    if (!quiet) AVER_INFO("[Voxi] RT temporal history reset by console command (resetrthistory): shadow, "
              "reflection AND sky-occlusion reprojection are ALL invalidated -- they share one "
              "validity flag today (see rtHistValid_'s own comment, VoxiRenderer.hpp, and "
              "voxi_rt.hlsli's rtReprojectAo gate).");
}

// ALIAS OF resetRtHistory, TODAY -- not a separate mechanism. rtSkyOcclusionTemporal's reprojection
// (voxi_rt.hlsli) is gated by the SAME gRtHistParams.y that mirrors rtHistValid_ (VoxiRenderer.cpp's
// beginShadowHistory), and cb_.rtDenoiseParams.w answers a different question ("is the AO pair bound
// at all this frame", not "does it hold valid history") -- repurposing it as an independent validity
// flag would also have to gate whether the pass runs at all, which would skip the WRITE into
// gAoHistOut too and leave the poisoned texel un-refreshed instead of clearing it. True independence
// needs new plumbing this task did not add; this ships as an honest alias with its own log line so
// the Output Log still says which command the user actually typed.
void VoxiRenderer::resetAoHistory() {
    rtHistValid_ = false;
    AVER_INFO("[Voxi] AO/sky-occlusion history reset by console command (resetaohistory) -- this "
              "currently ALSO resets RT shadow/reflection history, since all three share one "
              "validity flag today (see resetrthistory's own log line; true independence would need "
              "new plumbing).");
}

void VoxiRenderer::resetNrdHistory(bool quiet) {
    nrd_.forceHistoryReset();
    if (!quiet) AVER_INFO("[NRD] history reset by console command (resetnrdhistory); REBLUR restarts its own "
              "internal temporal accumulation from this frame.");
}

// Sets the occlusion rays per pixel, clamped rather than refused: a caller asking for 0 wants the
// cheapest shadow, not no shadow at all, and rtShadow divides by this.
void VoxiRenderer::setShadowRays(u32 n) {
    const u32 clamped = n < 1 ? 1 : (n > kMaxShadowRays ? kMaxShadowRays : n);
    if (clamped != n)
        AVER_WARN("[Voxi] {} shadow rays per pixel clamped to {}", n, clamped);
    if (clamped == rtShadowRays_) return;
    rtShadowRays_ = clamped;
    AVER_INFO("[Voxi] sun occlusion rays per pixel: {}", rtShadowRays_);
}

// Rounds to the nearest power of two in [1, kMaxPixelsPerRayTile] -- the shader's per-pixel trace
// schedule is a bitmask against this, not a modulo, so a non-power-of-two tile is not an option to
// silently honour the way an odd ray count is.
void VoxiRenderer::setPixelsPerRayTile(u32 n) {
    const u32 lo = n < 1 ? 1 : (n > kMaxPixelsPerRayTile ? kMaxPixelsPerRayTile : n);
    u32 rounded = 1;
    while (rounded * 2 <= lo) rounded *= 2;
    // lo sits between `rounded` and `rounded*2`; round to whichever is closer, ties toward the
    // smaller (cheaper, less amortisation) one.
    if (rounded < kMaxPixelsPerRayTile && (lo - rounded) > (rounded * 2 - lo)) rounded *= 2;
    if (rounded != n)
        AVER_WARN("[Voxi] {} pixels-per-ray tile edge rounded to the nearest power of two, {}", n, rounded);
    if (rounded == rtPixelsPerRayTile_) return;
    rtPixelsPerRayTile_ = rounded;
    AVER_INFO("[Voxi] ray-traced shadow tile: {}x{} ({} pixels per trace)",
              rounded, rounded, rounded * rounded);
}

// Clamped to [1, kMaxGiUpdateInterval]; no rounding needed, unlike the tile-edge setters above -- this
// is a plain frame count, not a bitmask operand.
void VoxiRenderer::setGiUpdateInterval(u32 n) {
    const u32 clamped = n < 1 ? 1 : (n > kMaxGiUpdateInterval ? kMaxGiUpdateInterval : n);
    if (clamped != n)
        AVER_WARN("[Voxi] GI update interval {} clamped to {}", n, clamped);
    if (clamped == giUpdateInterval_) return;
    giUpdateInterval_ = clamped;
    AVER_INFO("[Voxi] GI volume rebuilds every {} frame(s)", giUpdateInterval_);
}

// M4. See the header's own comment for the full contract; this is just the reassert-idiom-cheap
// setter every per-frame console dial in this class follows.
void VoxiRenderer::setGiForceRebuild(bool on) {
    if (on == giForceRebuild_) return;
    giForceRebuild_ = on;
    if (on) AVER_INFO("[Voxi] GI rebuild gate FORCED (--gi-force-rebuild / voxi.giForceRebuild): "
                       "every tick rebuilds; the GI cache is neither read nor written");
    else    AVER_INFO("[Voxi] GI rebuild gate no longer forced");
}

// W3. Either edge invalidates the previous rebuild's box: turning bounded dispatch ON must not trust
// a box that was never actually enforced (giBoxPrevDraws_ still describes what the LAST rebuild's
// draws touched, but nothing constrained the dispatch to it, so the volume outside it may hold
// something a bounded rebuild would never have written); turning it OFF and back on later must not
// trust a box that is now stale for the identical reason a cache restore invalidates it.
void VoxiRenderer::setGiBoundedDispatch(bool on) {
    if (on == giBoundedDispatch_) return;
    giBoundedDispatch_ = on;
    giBoxPrevValid_ = false;
    AVER_INFO("[Voxi] bounded GI dispatch {} (voxi.giBoundedDispatch); the next rebuild runs over "
              "the full grid", on ? "on" : "off");
}

// W12. See the header's own comment for the one-tick-delay contract. The recreate itself happens
// next prePass, through manageInjectionAccumulator()'s (a) branch -- turning the flag off makes that
// branch's `!giFreeAccumulator_` term true regardless of giAccumWanted_, so a freed accumulator comes
// straight back without waiting for a rebuild to ask for it.
void VoxiRenderer::setGiFreeAccumulator(bool on) {
    if (on == giFreeAccumulator_) return;
    giFreeAccumulator_ = on;
    AVER_INFO("[Voxi] injection-accumulator free-after-quiet {} (voxi.giFreeAccumulator, {} quiet "
              "tick(s) before a free); turning it off recreates the accumulator on the next tick if "
              "it was freed", on ? "on" : "off", kGiAccumulatorQuietTicks);
}

// Prints what the run's frames cost. The MEDIAN leads because a frame period is a heavy-tailed
// distribution -- one alt-tab or one shader compile drags a mean and leaves a median alone -- and
// the minimum is printed beside it because the gap between them is what says whether the machine
// was quiet enough for the reading to mean anything.
void VoxiRenderer::reportFrameTime(const char* when) {
    if (!frameTimeReport_) return;
    if (frameTimeMs_.empty()) {
        AVER_INFO("[Voxi] frame period ({}): no samples past the {}-frame warm-up", when, kFrameTimeWarmup);
        return;
    }
    std::vector<f32> s = frameTimeMs_;
    std::sort(s.begin(), s.end());
    const usize n = s.size();
    const f32 med = s[n / 2];
    const f32 p90 = s[(n * 9) / 10 < n ? (n * 9) / 10 : n - 1];
    f32 sum = 0.0f;
    for (f32 v : s) sum += v;
    AVER_INFO("[Voxi] frame period ({}): {} frames, median {:.3f} ms, mean {:.3f} ms, min {:.3f} ms, "
              "p90 {:.3f} ms -- WHOLE frame, CPU, {} sun ray(s)/pixel, rt {}",
              when, static_cast<u32>(n), med, sum / static_cast<f32>(n), s.front(), p90,
              rtShadowRays_, rtActive_ ? "active" : "off");
}

namespace {
// Row-vector transform of a point (w = 1), which is this engine's matrix convention throughout.
Vec3 xformPoint(const Vec3& p, const Mat4& m) {
    return Vec3{p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0],
                p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1],
                p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2]};
}
// Same, through a projective matrix, so the perspective divide happens.
Vec3 xformProjected(const Vec3& p, const Mat4& m) {
    const f32 x = p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0];
    const f32 y = p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1];
    const f32 z = p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2];
    const f32 w = p.x * m.m[0][3] + p.y * m.m[1][3] + p.z * m.m[2][3] + m.m[3][3];
    const f32 inv = std::fabs(w) > 1e-9f ? 1.0f / w : 0.0f;
    return Vec3{x * inv, y * inv, z * inv};
}
// The world matrix's largest axis scale, i.e. how much it can stretch a local-space radius. Row-
// vector convention: rows 0-2 are the images of the local X/Y/Z basis vectors, so each row's length
// IS that axis's scale factor. Culling needs the max rather than a per-axis figure, because the
// bounding sphere itself is not axis-aligned -- taking the largest never lets the sphere shrink
// smaller than the mesh actually reaches along any axis.
f32 maxAxisScale(const Mat4& m) {
    const f32 sx = Vec3{m.m[0][0], m.m[0][1], m.m[0][2]}.size();
    const f32 sy = Vec3{m.m[1][0], m.m[1][1], m.m[1][2]}.size();
    const f32 sz = Vec3{m.m[2][0], m.m[2][1], m.m[2][2]}.size();
    return std::max(sx, std::max(sy, sz));
}

// FNV-1a over an arbitrary byte range, same offset basis and prime giCacheKey()/prevTransformGroupKey
// already use in this file -- one mixing recipe, not a second one learned separately.
void fnvMix(u64& h, const void* p, usize n) {
    const u8* b = static_cast<const u8*>(p);
    for (usize i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
}

// The material key for a draw that is NOT authored -- a built-in SurfaceLook or the flat-gray
// fallback (see buildAccelerationStructures). Such draws share ONE binding-set handle
// (MaterialSystem::bindingSet(0)) regardless of which SurfaceLook it is -- M_Foliage and M_Rock
// resolve identically -- so the handle can't be the dedup key; the draw's own colour/metal/rough
// VALUES have to be, and this hashes exactly those.
// High bit cleared so this can never collide with an authored key (which tags the high bit over a
// plain 32-bit BindingSetHandle) -- two independent key spaces sharing one map.
u64 synthMaterialKey(const f32 color[4], f32 metallic, f32 roughness) {
    u64 h = 1469598103934665603ull;
    fnvMix(h, color, sizeof(f32) * 4);
    fnvMix(h, &metallic, sizeof(f32));
    fnvMix(h, &roughness, sizeof(f32));
    return h & ~(1ull << 63);
}

// MIXES A MESH HANDLE'S BITS before submit()'s cache (VoxiRenderer::meshSubmitCacheSlot) masks them
// into a slot index, instead of masking the raw handle directly. Same shape, same finalizer, as
// GameRender.cpp's own mixMeshId -- reused rather than reinvented -- but NOT the same justification,
// and the difference is worth stating rather than copying the old reasoning across unchecked.
// GameRender.cpp mixes an fnv1a64 hash of an ASSET PATH, whose low bits it argues are weak on this
// project's short, similar mesh names. `rhi::MeshHandle` here is a different id space entirely (see
// aver-asset-id-spaces in the engine's own notes): a small, DENSE, MONOTONICALLY INCREASING integer
// -- both D3D12Device and VulkanDevice hand a new mesh the next index into their own mesh vector and
// never recycle a destroyed one's slot (D3D12Device.cpp's own comment on destroyMesh: "the slot
// itself is KEPT"), so two live meshes are never given the same handle and handles do not arrive in
// any hash-like scattered order. A dense, monotonically increasing key already distributes evenly
// under a plain mask -- consecutive integers land in consecutive slots, which is the case direct
// mapping is best at, not worst -- so the specific failure mixing exists to avoid in GameRender.cpp
// (a hash whose low bits cluster) is not a risk this file has verified to exist. The mix is kept
// anyway, for the shape this cache was asked to match and because it costs two multiplies and three
// shifts next to a dev_->meshBounds() call, not because a load-bearing case for it was found here.
// This is MurmurHash3's 64-bit finalizer (fmix64), the same well-known avalanche mix GameRender.cpp's
// copy cites; duplicated rather than shared because render.voxi sits BELOW Runtime/game in this
// engine's dependency direction, so a shared header would have to move the wrong way for one
// nine-line function.
constexpr u64 mixMeshId(u64 x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}
} // namespace

// Starts a new draw list. Voxi runs a frame behind: the passes replay the previous one.
//
// ALSO CLEARS submit()'s PER-MESH CACHE (meshSubmitCache_) -- the invalidation point this class
// settled on for it, chosen from the three candidates the design question actually had: a change to
// depthProxyFn_/depthProxyUser_ (handled separately, at setDepthProxy() itself, since that can
// happen at any point, not only on a frame boundary); a mesh being destroyed or re-uploaded (NO
// candidate exists for this one -- nothing calls back into VoxiRenderer when either happens, so
// there is no event here to hook even if this class wanted one); or once a frame, here.
//
// HERE IS THE ONLY ONE OF THE THREE THIS CLASS CAN ACTUALLY GUARANTEE, and it is chosen for that
// reason ahead of being the cheapest option available (the cheapest would be never clearing it at
// all, and resolving each mesh once per SESSION instead of once per FRAME). A per-frame clear bounds
// the cache's own staleness to AT MOST one frame no matter what changed underneath it, INCLUDING a
// mesh re-upload this class has no way to be told about directly. That bound is not a new risk this
// cache introduces: it is the SAME one frame of staleness the swap two lines below already accepts
// for every draw in the list ("Voxi runs a frame behind"), so a mesh whose bounds changed
// mid-session was never going to be reflected sooner than the NEXT prePass() regardless of whether
// this cache exists at all. Clearing every frame keeps the memoisation strictly WITHIN that existing
// tolerance instead of opening a second, independent staleness window with no bound on it -- which
// is what letting the cache live across frames unconditionally would have done: a mesh re-uploaded
// with new geometry would keep answering with its FIRST upload's bounds for the rest of the run,
// silently, until something unrelated forced a clear. That is exactly the "entities vanish from a
// shadow cascade at the wrong moment, and it looks like a culling tuning problem for a week" bug
// meshSubmitCache_'s own header comment names as the failure mode this design has to avoid. A few
// hundred slots reset every frame -- kMeshSubmitCacheSlots default-constructed structs, no
// allocation -- is not a cost worth trading that guarantee away for; it is a rounding error next to
// the (up to) 16,000 submit() calls this cache exists to shrink.
void VoxiRenderer::beginScene() {
    drawsPrev_.swap(draws_);
    draws_.clear();
    meshSubmitCache_ = {};
}

// See the header's own comment on setDepthProxy for why this is no longer a one-line inline setter.
// Guarded on an actual change to either argument -- the same reassert idiom setBlendedGiCone/
// setShadowRays use elsewhere in this file -- so a hypothetical future caller that reasserts the
// same fn/user pair every frame (nothing in this codebase does that today) would not pay a full
// cache clear for a no-op every single time.
void VoxiRenderer::setDepthProxy(DepthProxyFn fn, void* user) {
    if (fn == depthProxyFn_ && user == depthProxyUser_) return;
    depthProxyFn_ = fn;
    depthProxyUser_ = user;
    // A cached depthProxyMesh answer is only correct for the fn/user pair that produced it, and a
    // slot has no field remembering which pair that was -- only the mesh id. Dropping the whole
    // table is the only way to guarantee the NEXT submit() for an already-cached mesh asks the NEW
    // pair instead of silently reusing the old one's verdict.
    meshSubmitCache_ = {};
}

// Finds mesh's slot in meshSubmitCache_, evicting a DIFFERENT mesh's leftover answers first -- the
// identical hazard, and the identical fix, as GameRender.cpp's findMeshLookupSlot: handing back a
// slot that still half-remembers the PREVIOUS occupant's depth-proxy or bounds answer under a NEW
// mesh id would silently misattribute one mesh's proxy substitution or bounding sphere to another.
// Aggregate-initialising `MeshSubmitCacheSlot{mesh}` on an eviction sets `.mesh` and leaves every
// other field at its own in-class default (unresolved), so a fresh occupant reads as "never touched"
// without this function having to list every field by hand.
VoxiRenderer::MeshSubmitCacheSlot& VoxiRenderer::meshSubmitCacheSlot(rhi::MeshHandle mesh) {
    MeshSubmitCacheSlot& slot =
        meshSubmitCache_[static_cast<u32>(mixMeshId(mesh) & (kMeshSubmitCacheSlots - 1))];
    if (slot.mesh != mesh) slot = MeshSubmitCacheSlot{mesh};
    return slot;
}

// Records one draw into this frame's list, copying its material block.
void VoxiRenderer::submit(rhi::MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                          f32 metallic, f32 roughness, rhi::BindingSetHandle drawBinding,
                          const void* drawConstants, u32 drawConstantBytes, bool translucent,
                          bool hiddenFromOwner) {
    if (mesh == 0) return;
    if (draws_.size() >= kMaxDraws) {
        // Says so exactly once. The old silent return is what let a 4096 cap survive a scene with
        // 6,370 entities in it without anyone noticing the shadows were incomplete.
        if (!drawCapReported_) {
            drawCapReported_ = true;
            AVER_WARN("[Voxi] draw list is full at {} -- further entities cast no shadow and do not "
                      "voxelise this frame; raise kMaxDraws", kMaxDraws);
        }
        return;
    }
    // Nested, not chained off the walk's own CpuLap: submit() is reached from all three WalkEmit*
    // branches (depth, raster and the culled/hidden direct route alike -- see this function's own
    // header comment on why culled entities arrive here too), so the call site has no single parent
    // phase to `.to()` into and CpuNest is the shape built for exactly that (see CpuNest's own
    // comment). Placed AFTER both early returns above so a mesh==0 no-op or a draw-list-full refusal
    // -- neither of which does any of the work below -- is not folded into VoxiSubmit's number,
    // which would otherwise inflate it with calls that never touch a single one of the memcpys,
    // hash lookups or bounds queries this span exists to measure.
    CpuNest voxiSubmitTiming(CpuSpan::VoxiSubmit);
    Draw d;
    d.mesh = mesh;
    // Resolved ONCE here, not per pass: six depth passes asking the same question about the same
    // handle would be six map lookups for one answer that cannot change within a frame.
    //
    // BOTH d.depthMesh AND THE meshBounds LOOKUP BELOW ARE FUNCTIONS OF mesh ALONE. a7ff716d's own
    // note here named the redundancy this used to pay in full: both re-resolved on every one of the
    // (up to) 16,000 calls this function gets a frame, one per entity, though a scene rarely has more
    // than a few hundred DISTINCT meshes -- the same shape commit 3a9dc985 found in the RT geometry
    // table (dedup by mesh: 64.2ms -> 5.4ms). THIS IS THAT FIX: meshSubmitCacheSlot() resolves each
    // of the two questions once per DISTINCT mesh id seen since the last beginScene() clear, not once
    // per entity. See meshSubmitCache_'s own header comment for the cache's shape and its 256-slot
    // deviation from 80730751's precedent, and beginScene()'s own comment (this file) for why a
    // per-frame clear is the invalidation point this class settled on. meshSubmitCacheEnabled_
    // (default true) is the runtime A/B switch: false reproduces the two calls below exactly, in the
    // original order, with none of the caching machinery touched.
    MeshSubmitCacheSlot* const cacheSlot =
        meshSubmitCacheEnabled_ ? &meshSubmitCacheSlot(mesh) : nullptr;

    if (cacheSlot) {
        if (!cacheSlot->depthProxyResolved) {
            cacheSlot->depthProxyMesh = depthProxyFn_ ? depthProxyFn_(mesh, depthProxyUser_) : 0;
            cacheSlot->depthProxyResolved = true;
        }
        d.depthMesh = cacheSlot->depthProxyMesh;
    } else {
        d.depthMesh = depthProxyFn_ ? depthProxyFn_(mesh, depthProxyUser_) : 0;
    }
    if (!d.depthMesh) d.depthMesh = mesh;
    std::memcpy(d.world, world, 16 * sizeof(f32));
    std::memcpy(d.color, baseColor, 4 * sizeof(f32));
    d.metallic = metallic;
    d.roughness = roughness;
    d.hiddenFromOwner = hiddenFromOwner;
    d.matSet = drawBinding;
    d.matBytes = drawConstantBytes < sizeof(d.mat) ? drawConstantBytes : static_cast<u32>(sizeof(d.mat));
    if (drawConstants && d.matBytes) std::memcpy(d.mat, drawConstants, d.matBytes);
    d.translucent = translucent;

    // World-space bounding sphere for shadowPass's per-cascade cull. d.boundsRadius already
    // defaults to -1 (unknown) for a backend that has no bounds to give; only overwritten below.
    //
    // ONLY THE LOCAL-SPACE HALF OF THIS IS MEMOISED -- localCentre/localRadius below, and whether
    // meshBounds found them at all -- NEVER d.boundsCentre/d.boundsRadius themselves, which are the
    // transform of that local answer through THIS ENTITY's own `world` matrix, right after. `world`
    // differs per entity even when `mesh` does not, so the transform stays computed on every single
    // call with no exception; caching its RESULT instead would hand every instance of a mesh the
    // FIRST instance's world-space bounds -- see meshSubmitCache_'s own header comment for the
    // failure that was designed away from rather than risked.
    bool haveBounds = false;
    const f32* localCentre = nullptr;
    f32 localRadius = 0.0f;
    f32 localCentreScratch[3] = {};
    f32 localRadiusScratch = 0.0f;
    if (cacheSlot) {
        if (!cacheSlot->boundsResolved) {
            cacheSlot->haveBounds =
                dev_ && dev_->meshBounds(mesh, cacheSlot->localCentre, &cacheSlot->localRadius);
            cacheSlot->boundsResolved = true;
        }
        haveBounds = cacheSlot->haveBounds;
        localCentre = cacheSlot->localCentre;
        localRadius = cacheSlot->localRadius;
    } else {
        haveBounds = dev_ && dev_->meshBounds(mesh, localCentreScratch, &localRadiusScratch);
        localCentre = localCentreScratch;
        localRadius = localRadiusScratch;
    }
    if (haveBounds) {
        Mat4 w;
        std::memcpy(&w.m[0][0], world, sizeof(w.m));
        const Vec3 c = xformPoint(Vec3{localCentre[0], localCentre[1], localCentre[2]}, w);
        d.boundsCentre[0] = c.x; d.boundsCentre[1] = c.y; d.boundsCentre[2] = c.z;
        d.boundsRadius = localRadius * maxAxisScale(w);
    }
    draws_.push_back(d);
}

// The IRenderFeature entry point every draw arrives through. `blended` is new (see RHIResources.hpp's
// IRenderFeature::submitDraw) -- before it existed every draw here was opaque, so submit() never had
// to ask.
// A blended draw is filtered here EXPLICITLY: falling into submit() unfiltered would voxelise glass as
// an opaque light-blocker, cast a solid black shadow from it, and put it in the TLAS as a surface
// every reflection ray hits as opaque. See createScenePipelines()'s "10b" comment for the full list of
// what glass gets despite this (a shadow cast ON it, GI landing ON it) versus what it doesn't (casting
// its own shadow, appearing in a reflection, injecting light).
void VoxiRenderer::submitDraw(rhi::MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                              f32 metallic, f32 roughness, rhi::BindingSetHandle drawBinding,
                              const void* drawConstants, u32 drawConstantBytes, bool blended) {
    if (blended) {
        // Takes the translucent lane: into the TLAS marked non-opaque so a shadow ray can attenuate
        // through it, still excluded from the cascade/GI shadow map/voxelisation since all three are
        // depth-only (no channel for a transmittance) -- an RT-path feature by construction, not by
        // choice; see "10b" for the full reasoning. Census kept because the exclusion is still worth
        // reporting.
        ++blendedDropped_;
        if ((blendedDropLogs_ & (blendedDropLogs_ + 1)) == 0) {
            AVER_INFO("[Voxi] {} translucent draw(s) routed to the RT lane so far -- in the TLAS as "
                      "non-opaque (attenuated, tinted shadows where ray tracing is active), still "
                      "out of the cascade, the GI shadow map and voxelisation, which are depth-only "
                      "and cannot express a transmittance",
                      blendedDropped_);
        }
        ++blendedDropLogs_;
        submit(mesh, world, baseColor, metallic, roughness, drawBinding, drawConstants,
               drawConstantBytes, /*translucent=*/true);
        return;
    }
    submit(mesh, world, baseColor, metallic, roughness, drawBinding, drawConstants, drawConstantBytes);
}

// Runs Voxi's frame: acceleration structures, shadow map, then voxelise and filter the volume.
void VoxiRenderer::prePass(rhi::IRenderContext& ctx) {
    if (!giReady_) return;
    // A MATERIAL GRAPH THAT APPEARED SINCE THESE PIPELINES WERE BUILT: materials load after
    // createScenePipelines already ran, so without this a graph would get a valid id but be shaded
    // by a pixel shader whose switch has no arm for it (silently the stock material). Checked here
    // rather than pushed from the loader (editor-side) because a pull on a revision number cannot be
    // forgotten by a future third caller.
    if (scenePipelineGraphRev_ != pbr::materialGraphs().revision()) {
        scenePipelineGraphRev_ = pbr::materialGraphs().revision();
        AVER_INFO("[Voxi] rebuilding scene pipelines for {} material graph(s)",
                  pbr::materialGraphs().count());
        if (!createScenePipelines(dev_->sampleCount(), dev_->backbufferFormat(), dev_->depthFormat()))
            AVER_ERROR("[Voxi] scene pipelines could not be rebuilt for the material graphs");
    }
    // A shader file changed on disk since these pipelines compiled. Checked here for the same reason,
    // and because it's the only SAFE point: rebuilding GPU objects from a file-watcher thread would
    // free things a command list is mid-recording -- the failure that removed the device at Present
    // when a stored render scale rebuilt targets mid-frame (D3D12Device::setRenderScale). The watcher
    // only bumps an integer; every GPU consequence happens here.
    if (scenePipelineShaderRev_ != rhi::shaderFileRevision()) {
        scenePipelineShaderRev_ = rhi::shaderFileRevision();
        AVER_INFO("[Voxi] rebuilding scene pipelines: shader files changed (revision {})",
                  scenePipelineShaderRev_);
        if (!createScenePipelines(dev_->sampleCount(), dev_->backbufferFormat(), dev_->depthFormat()))
            AVER_ERROR("[Voxi] scene pipelines could not be rebuilt from the changed shader files -- "
                       "the previous pipelines are still bound, so the last good shader keeps drawing");
    }
    ++rtFrameIndex_;   // a pure per-frame count; see the member's own comment for why
    // Sampled at the TOP of the feature's frame, so consecutive readings are one frame apart
    // whatever the passes below do. The first sample after the warm-up is discarded with the rest.
    if (frameTimeReport_) {
        const u64 now = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        if (frameTimeLastNs_ != 0 && frameTimeSeen_ >= kFrameTimeWarmup)
            frameTimeMs_.push_back(static_cast<f32>(now - frameTimeLastNs_) * 1e-6f);
        frameTimeLastNs_ = now;
        ++frameTimeSeen_;
        // Reported as the run goes as well as at shutdown, so a run that is killed or that never
        // reaches shutdown still leaves a number behind.
        if (frameTimeMs_.size() && frameTimeMs_.size() % 120 == 0) reportFrameTime("running");
    }
    // t10, the blended backdrop. Re-bound only when the handle changes -- setSrv every frame for an
    // unmoved handle is churn for nothing.
    // A 0 handle MUST clear the slot, not skip it -- getting that wrong crashed the GPU on every
    // window resize. D3D12Device::resize calls releasePostTargets() to destroy the backdrop without
    // recreating it in the same call, so `if (bd) setSrv(...)` leaves t10 pointing at a texture
    // already freed until targets rebuild; the shader's GetDimensions() guard is not a validity test
    // on a dangling descriptor, so sampling it faults the shader and removes the device. clearSrv
    // writes the null view instead, so GetDimensions() reads 0 and the shader takes its no-backdrop
    // path.
    if (dev_ && bindings_) {
        const rhi::TextureHandle bd = dev_->sceneColorBackdropTexture();
        if (bd != boundBackdrop_) {
            boundBackdrop_ = bd;
            if (bd) res_->setSrv(bindings_, 10, bd);
            else    res_->clearSrv(bindings_, 10);
        }
    }

    materials_.update();
    const f32 size = extent_ * 2.0f;
    cb_.voxelOrigin[0] = center_[0] - extent_;
    cb_.voxelOrigin[1] = center_[1] - extent_;
    cb_.voxelOrigin[2] = center_[2] - extent_;
    cb_.voxelOrigin[3] = size > 0.0f ? 1.0f / size : 0.0f;   // the shader multiplies by this
    cb_.voxelParams[0] = static_cast<f32>(voxelResBuilt_);
    cb_.voxelParams[1] = settings_.giIntensity;
    cb_.giParams[0]    = static_cast<f32>(settings_.giCones);
    // THE EDITOR VIEW MODE, COMPOSED HERE AND NOT IN beginShadowHistory, where it was first
    // written and never arrived: that function returns early unless shadow history is active,
    // so the field kept whatever it held at init and PSRayDriven read unlit as permanently off.
    // MEASURED, which is the only reason it was caught: with --unlit the image differed from lit
    // by 2.79% of pixels at an unchanged mean luminance -- the raster chrome moving, not the
    // scene. A view mode that "does something" is not evidence it did the RIGHT something.
    // prePass runs every frame before the constants are uploaded, which is what this needs.
    //
    // viewDebug_ takes priority over unlit_, not OR'd with it: they are two dropdown families the
    // editor already keeps mutually exclusive (selecting a ray-hit/triangles view clears Unlit's
    // own selection state and vice versa -- see SandboxViewport.cpp), and both pack into this one
    // float, so encoding "both at once" has no image to produce anyway. See ViewDebug's own
    // comment (VoxiRenderer.hpp) for the 2-5 values this sends and PSRayDriven (voxi.hlsl) for
    // the decode.
    cb_.viewParams[0] = viewDebug_ != ViewDebug::None ? static_cast<f32>(viewDebug_) : (unlit_ ? 1.0f : 0.0f);
    // THE LIVE GI RADIANCE CEILING (AVER_VOX_MAXRAD in voxi.hlsl/voxi_gi.hlsli) -- see
    // Settings::giRadianceCeiling's own comment for what this caps and why, and FrameConstants::
    // viewParams's comment (VoxiRenderer.hpp) for why .y is safe to repurpose. Sent every frame from
    // this already-per-frame block, same as viewParams[0] directly above.
    cb_.viewParams[1] = settings_.giRadianceCeiling;
    // .z: whether ReSTIR GI is the chosen estimator, which decides whether PSVoxel bakes the sky into
    // the volume (voxelSkyInjected, and PSVoxel's own comment). Written here, ahead of the rebuild
    // gate and voxelizePass, from the SETTING -- never from giRestirParams[0], which also reads 0 on
    // a debug-view or empty-TLAS frame and would rebake the volume on every such flip.
    // .w is 0 here; recordStagedRayDriven sets it to the staged passes' row pitch for their own
    // uploads only, and on a half-rate GI frame leaves the packed-NRD-input flag (bit 17, parity in
    // bit 16) for the rest of the frame. (Both briefly carried dials during the moving-camera ReSTIR GI fade bisection;
    // those literals are back in voxi_restir.hlsli.)
    cb_.viewParams[2] = giRestirWanted() ? 1.0f : 0.0f;
    cb_.viewParams[3] = 0.0f;
    // LOCAL LIGHTS (LAMPS): count and flags, 0 for every pass prePass itself records (shadow cascades,
    // GI shadow, voxelise, air visibility) -- none of them lights with lamps, and PSVoxel keeps a lamp's
    // emission in the volume. Raised afterwards for the scene pass only: at the end of prePass for the
    // raster scene draws, in scenePass for the single-pass/staged primary (publishLocalLights), and kept
    // from there through the blended replay.
    cb_.cameraMedium[2] = 0.0f;
    cb_.cameraMedium[3] = 0.0f;
    // y IS THE COHERENCE TILE EDGE, and it is sent whether or not the rays are on: the shader divides
    // the pixel coordinate by it unconditionally, so a 0 arriving here would be a division by zero in
    // every pixel rather than a disabled feature. max(1) is the identity, not a guard against a
    // caller mistake -- Settings clamps the authored value already.
    cb_.ambientParams[1] = static_cast<f32>(std::max(settings_.giSkyOcclusionTile, 1u));
    // THE LIGHTING-CONTRAST LEGACY BITMASK (setLightingLegacyBits' own comment has the bit table) --
    // sent every frame with NO condition, same as ambientParams[1] immediately above: a block that
    // has never been written is all zeros, which every shader that reads gAmbientParams.z takes to
    // mean every fix is live.
    cb_.ambientParams[2] = static_cast<f32>(lightingLegacyBits_);
    // Refraction rides giParams' spare .yzw -- only .x was used, so these ride a row the HLSL mirror
    // already declares at no layout cost (mirrored by hand in more than one place; see
    // MaterialConstants' note on what a silent offset mistake costs). A fourth refraction knob needs
    // its own row and a mirror update everywhere, not another borrow.
    cb_.giParams[1]    = static_cast<f32>(settings_.refractionMode);
    cb_.giParams[2]    = settings_.refractionStrength;
    cb_.giParams[3]    = settings_.refractionEdgeFade;
    cb_.voxelParams[2] = settings_.giMaxDistance;
    // Gates the cone trace -- see setConeTraceEnabled's own comment for why this is ANDed in here
    // rather than folded into giEnabled(): it lets an A/B measurement turn the shader-side read off
    // without changing whether the passes below actually run.
    cb_.voxelParams[3] = (giEnabled() && !debugView_ && coneTraceEnabled_) ? 1.0f : 0.0f;

    // THE STAGED RAY-DRIVEN BIT-FIELD TOGGLES -- see FrameConstants::giShadowParams's own comment for
    // the bit table and Voxi.hpp for what each one trades. Packed HERE, unconditionally, every frame:
    // fitGiShadow() (below, inside the GI rebuild gate) no longer touches [3] at all, precisely so a
    // frame that skips the gate -- and so never calls fitGiShadow -- still carries the bits, same
    // reasoning as ambientParams[1]/[2] a few lines up. FROM SCRATCH, NOT A MERGE, and that is what
    // gives bit 16 (Settings::blendedReuseStagedLighting) its self-clearing property: recordStagedRayDriven,
    // LATER this same frame, ORs that bit in on top of whatever this line wrote, and only on a frame
    // that actually reaches it -- see that OR's own comment.
    cb_.giShadowParams[3] = static_cast<f32>((settings_.rtSecondaryShadowOpaque ? 1u : 0u) |
                                              (settings_.rtSkyOcclusionHalfRate ? 2u : 0u) |
                                              (settings_.rtReflectionHalfRate  ? 4u : 0u) |
                                              (settings_.rtGiHitShadowMap      ? 8u : 0u));

    // Five passes below (acceleration structures, cascades, GI-only shadow box, voxelise, mip filter)
    // used to each open their own top-level GPU marker, so the timing report saw five unrelated
    // siblings instead of one feature's frame. This outer scope makes each pushMarker call a CHILD of
    // "Voxi GI update", which also gives this scope's own EXCLUSIVE time a meaning -- though M1
    // changed what that meaning is: beginShadowHistory() now opens its OWN child span ("Voxi shadow
    // history", its very first statement), and the nrd_.record() call inside it opens a grandchild
    // of THAT ("Voxi NRD denoise", wrapping only the record call and its two output-handle
    // assignments). So "Voxi GI update"'s own EXCLUSIVE time is now endShadowHistory() -- which opens
    // no marker of its own; see its definition, it is pure CPU bookkeeping with nothing to time --
    // plus whatever below still runs unmarked (W12's manageInjectionAccumulator() call, the CPU-only
    // gate logic). Opened HERE rather than at the top, because everything above is CPU-only
    // bookkeeping -- starting the scope here keeps its inclusive time real GPU work end to end.
    rhi::ScopedGpuStat voxiGpuStat(ctx, "Voxi GI update");
    // W12: recreate or free the injection accumulator for THIS frame before anything else in this
    // scope can bind bindings_. shadowPass, called a few lines below right after
    // beginShadowHistory(), is the first thing this frame that BINDS bindings_ (ctx.setBindingSet),
    // and Vulkan's ringed binding sets forbid writing a set (setUav/setSrv, which the two branches
    // inside manageInjectionAccumulator both do) after it has already been bound once this frame.
    // Everything above this line that touches bindings_ (the t10 backdrop setSrv/clearSrv, earlier in
    // this function) is a write too, but it runs before ANY binding set is bound this frame, same as
    // this call.
    manageInjectionAccumulator(ctx);
    buildAccelerationStructures(ctx);   // sets rtActive_, which beginShadowHistory reads
    // After the TLAS decision (a lamp's shadow ray needs one) and before shadowPass() first binds
    // bindings_ -- t18 is written here, and Vulkan forbids writing a set it has already bound this frame.
    buildLocalLights();
    // GATED ON rtActive_, NOT ON THE SETTING ALONE. The shader traces these against the same
    // acceleration structure the shadow ray uses, and there is not one on a frame that built no
    // TLAS -- publishing a non-zero count then would have every pixel trace into nothing and read
    // back "sky visible everywhere", which is BRIGHTER than the cone estimate it replaced and would
    // look like this feature making the bug worse. Zero here means the shader keeps the cone
    // gather's own occlusion, which is the correct fallback and the pre-existing behaviour.
    //
    // MOVED HERE BY THE BUILD/SHADER-SAFETY REVIEW (audit finding F4): this used to sit in the
    // CPU-only block above, BEFORE buildAccelerationStructures() ran, so it read LAST frame's
    // rtActive_ instead of the one this call just computed -- a frame where ray tracing turns off
    // (or the scene goes briefly empty, leaving no TLAS) still published a nonzero ray count, which
    // is exactly the "sky visible everywhere, BRIGHTER" case the comment above already warned
    // against. rtActive_ is fresh as of the line immediately above; cb_ is not uploaded
    // (setConstantBuffer) until deep inside shadowPass()/scenePass(), well below this.
    cb_.ambientParams[0] = rtActive_ ? static_cast<f32>(std::min(settings_.giSkyOcclusionRays,
                                                                 kMaxShadowRays))
                                     : 0.0f;
    beginShadowHistory(ctx);
    shadowPass(ctx);                    // fitCascades(), called from here, fills curViewProj_
    if (giEnabled()) {
        // Amortised revoxelisation: rtFrameIndex_ was incremented above, so it reads 1 on the very
        // first prePass -- (rtFrameIndex_-1) % N always lands on 0 for frame 1, guaranteeing the
        // volume builds at least once before anything samples it. Frames in between skip
        // voxelizePass/filterMips; the cone trace samples whatever the volume last held.
        // giUpdateInterval_ == 1 (default) takes the fast path every frame, as before this knob existed.
        if (giUpdateInterval_ <= 1 || ((rtFrameIndex_ - 1) % giUpdateInterval_) == 0) {
            // THE REBUILD GATE. Everything below recomputes a function of (draw list, sun, volume
            // placement); when none of those moved, the volume texture already holds the answer and
            // is still sitting there fully resolved and mip-filtered. See giSnapshotUnchanged.
            // CONVERGENCE TICKS, and the multi-bounce injection does not work without them.
            //
            // PSVoxel now reads the PREVIOUS bake and re-emits it (see its own comment), so light
            // accumulates one bounce per REBUILD rather than one bounce per frame. The gate below
            // is very good at its job -- it skips 96-98% of ticks on a still camera -- which meant
            // the volume was baked once, the feedback had nothing but an empty volume to read, and
            // the second bounce never happened at all. MEASURED: adding the feedback term alone
            // moved the isolated GI contribution from 52.1 to 37.0, i.e. it removed the false
            // unoccluded sky and put nothing back.
            //
            // So a rebuild that CHANGED something schedules a few more. Each one adds a bounce, the
            // series converges geometrically (albedo < 1), and then the gate goes quiet again --
            // the cost is a handful of extra bakes after a change, not a permanent tax. A still
            // scene still settles into skipping everything, which is what the gate was for.
            const bool converging = giConvergeTicks_ > 0;
            // giForceRebuild_ (M4, --gi-force-rebuild / voxi.giForceRebuild) is ANDed in LAST, after
            // giSnapshotUnchanged() -- that call has logging side effects (giGateWhyMask_, the
            // rejection-reason counters below it) that describe the REAL gate outcome, and forcing a
            // rebuild must not silence what the gate would otherwise have said about this tick.
            const bool gateUnchanged = giSnapshotUnchanged();
            if (gateUnchanged && !converging && !giForceRebuild_) {
                ++giSkipped_;
                // W12: only while nothing is converging -- a bake still settling into its multi-bounce
                // answer (giConvergeTicks_ > 0) is busy work the snapshot gate alone happened to skip,
                // not an idle accumulator waiting to be freed.
                if (giConvergeTicks_ == 0) ++giQuietTicks_;
                // The volume has settled: inputs unchanged and the bounces converged. The one moment
                // it is worth caching -- see giCacheSettlePending_.
                if (giCacheSettlePending_ && giCacheScheduleDump(ctx)) giCacheSettlePending_ = false;
            } else {
                giQuietTicks_ = 0;
                // W12: the accumulator may have been freed since the last rebuild. A rebuild this gate
                // just decided to run needs it, but manageInjectionAccumulator() -- called earlier this
                // same prePass, before buildAccelerationStructures() -- only learns that from
                // giAccumWanted_, which this tick is the first to set; the recreate itself lands next
                // prePass. So a tick that finds the accumulator missing asks for it back and does
                // NOTHING further: not ++giRebuilt_, not giConvergeTicks_, not takeGiSnapshot(). Leaving
                // all three untouched is what makes giSnapshotUnchanged() reject the skip again next
                // tick -- the retry this needs once the accumulator is back -- rather than the gate
                // believing this tick's change was already accounted for.
                if (!voxelAccumTex_) {
                    giAccumWanted_ = true;
                } else {
                    ++giRebuilt_;
                    if (converging) --giConvergeTicks_;
                    else            giConvergeTicks_ = kGiConvergeTicks;
                    takeGiSnapshot();
                    // Before voxelizePass, inside this gate: PSVoxel samples the GI-only map through
                    // giShadowFactor, so it must exist before injection reads it, and rebuilding it on
                    // the 3-in-4 frames injection is skipped would be pointless. (The other half of the
                    // saving is that the camera cascades no longer carry the volume at all -- fitCascades.)
                    giShadowPass(ctx);
                    // THE CACHE READ SITS EXACTLY HERE, between "gate says rebuild" and the rebuild
                    // itself: inputs are settled (takeGiSnapshot just ran) and work hasn't started. A
                    // hit fills the volume from disk and the two passes below are skipped whole; a miss
                    // falls through and bakes. The WRITE is not here: a bake only marks the volume
                    // for caching, and the skip branch above copies it out once it has settled (see
                    // giCacheSettlePending_).
                    //
                    // M4: giForceRebuild_ SHORT-CIRCUITS THE CACHE ON BOTH SIDES, not just the read. A
                    // forced tick exists to MEASURE a bake -- a --gi-force-rebuild run must pay
                    // voxelizePass+filterMips every single tick, never a cache hit -- and it never
                    // reaches the skip branch that writes, so it never caches either.
                    if (giForceRebuild_ || !giCacheRestore(ctx)) {
                        voxelizePass(ctx);
                        filterMips(ctx);
                        // Whether everything since the last settled volume was the cloud clock alone
                        // (giRebuildCloudOnly_ only describes THIS gate evaluation, and is false again
                        // by the quiet tick that writes). A pure convergence tick changes nothing.
                        if (!gateUnchanged)
                            giCacheSettleCloudOnly_ =
                                (giCacheSettlePending_ ? giCacheSettleCloudOnly_ : true) && giRebuildCloudOnly_;
                        giCacheSettlePending_ = true;
                    }
                }
            }
            giCacheTick();
            // Reported as a ratio ("GI rebuilt 3 of 170 ticks") because a run reporting 100% rebuilt
            // is a run where the gate saves nothing.
            // CORRECTED to report at widening intervals rather than once at a fixed tick count: the
            // first version logged once at 64 ticks (~frame 256 at the default interval of 4), which
            // on a streamed level is still mid-fill -- the draw list changes every tick, the gate
            // cannot match by construction, and the log printed "0 skipped of 64" regardless. A
            // measurement whose window excludes the case it measures is worse than none: it reads as
            // a result. Now it also prints the SINCE-LAST-REPORT ratio beside the lifetime one, so the
            // steady state isn't averaged away by the loading phase.
            const u64 ticks = giSkipped_ + giRebuilt_;
            if (ticks >= giGateNextReport_) {
                const u64 winTicks = ticks - giGateLastTicks_;
                const u64 winSkipped = giSkipped_ - giGateLastSkipped_;
                AVER_INFO("[Voxi] GI rebuild gate: {} rebuilt / {} skipped of {} tick(s) "
                          "-- {}% avoided overall, {}% since the last report",
                          giRebuilt_, giSkipped_, ticks, (giSkipped_ * 100) / ticks,
                          winTicks ? (winSkipped * 100) / winTicks : 0);
                giGateLastTicks_ = ticks;
                giGateLastSkipped_ = giSkipped_;
                giGateNextReport_ = ticks * 2;   // 64, 128, 256, ... -- a handful of lines, not a flood
            }
        }
    }
    // OCCLUSION-AWARE FOG: the air sky-visibility refresh, EVERY frame, decoupled from the GI rebuild
    // gate above. Sky visibility changes only when geometry does, while the voxel volume rebuilds
    // often under camera motion and a full CSAirVis measured ~10 ms (at 48^3) -- so each frame
    // refreshes ONE slab of kAirVisSlabLayers z-layers, round-robin, and the whole volume turns over
    // every kAirVisResolution / kAirVisSlabLayers frames at a flat, small cost. A dirty volume (just
    // created, or voxel GI just came back on) is filled whole in one dispatch first, so no frame ever
    // reads an unwritten texel.
    //
    // voxelTex_ rests in ShaderResource between frames (createVoxelVolume()'s initialState, and
    // filterMips()'s final transition) -- a compute read needs NonPixelShaderResource, hence the round
    // trip around the dispatch.
    const bool airVisActive = airVisTex_ && airVisPso_ && voxelTex_ && cb_.voxelParams[3] > 0.5f;
    if (airVisActive && !airVisWasActive_) airVisDirty_ = true;
    airVisWasActive_ = airVisActive;
    if (airVisActive) {
        ctx.textureBarrier(voxelTex_, rhi::ResourceState::ShaderResource,
                           rhi::ResourceState::NonPixelShaderResource);
        if (airVisDirty_) {
            dispatchAirVis(ctx, 0, kAirVisResolution);
        } else {
            constexpr u32 kSlabs = kAirVisResolution / kAirVisSlabLayers;
            const u32 zLo = (airVisSlab_ % kSlabs) * kAirVisSlabLayers;
            dispatchAirVis(ctx, zLo, zLo + kAirVisSlabLayers);
            airVisSlab_ = (airVisSlab_ + 1) % kSlabs;
        }
        ctx.textureBarrier(voxelTex_, rhi::ResourceState::NonPixelShaderResource,
                           rhi::ResourceState::ShaderResource);
    }
    endShadowHistory();

    // LOCAL LIGHTS (LAMPS) FOR THE RASTER SCENE DRAWS: the backend records every opaque PSMainVoxi draw
    // after prePass returns, reading cb_ through sceneConstants(), and Voxi gets no call once they are
    // done -- so the constants are raised HERE, after every upload prePass itself made, and u19 is marked
    // written here too, exactly as endShadowHistory just marked the sun's history those same draws write.
    // Nothing later this frame reads u19 back (the blended replay's reuse is staged-only), so no UAV
    // barrier follows them. A ray-driven frame decides in scenePass instead. A debug-view frame bound no
    // u19 (rdLocalOutThisFrame_ is 0), so it stays at 0.
    if (!rayDrivenActive() &&
        publishLocalLights(localLightsReady(), "the raster scene draws (PSMainVoxi)")) {
        rdLocalHistFrame_ = rtFrameIndex_;
        rdLocalHistHash_ = rdLocalLightHash_;
    }
}

// ---- W12: recreate or free the injection accumulator for this frame ----
//
// Called from prePass(), once, before buildAccelerationStructures() -- see that call site's own
// comment for the Vulkan binding-set ordering hazard that placement avoids.
//
// A PLACEHOLDER, NOT A NULL BIND, while the accumulator is gone: bindings_/clearBindings_/
// resolveBindings_ all declare slot 1 as a Texture3D UAV (giTableKinds), and this file's own
// aver-view-outlives-its-buffer lesson is that every binding set referencing a resource about to be
// destroyed must be rebound to something else FIRST -- see the free branch below.
void VoxiRenderer::manageInjectionAccumulator(rhi::IRenderContext& ctx) {
    (void)ctx;   // createTexture/destroyTexture go through res_, not the command list; nothing here
                 // records into ctx itself -- the parameter exists so the call site reads like every
                 // other pass this function's sibling passes take one.
    if (!voxelAccumTex_ && voxelResBuilt_ && bindings_ && (giAccumWanted_ || !giFreeAccumulator_)) {
        // (a) RECREATE. Reached either because a rebuild just asked for it (giAccumWanted_, set by
        // prePass's gate the tick it found the accumulator missing) or because giFreeAccumulator_
        // itself is off -- e.g. the accumulator was freed while the flag was on and the user then
        // turned the flag back off, which must bring it straight back rather than wait for another
        // rebuild to ask.
        if (createInjectionAccumulator(voxelResBuilt_)) {
            res_->setUav(bindings_, 1, voxelAccumTex_, 0);
            res_->setUav(clearBindings_, 1, voxelAccumTex_, 0);
            res_->setUav(resolveBindings_, 1, voxelAccumTex_, 0);
            giAccumWanted_ = false;
            giQuietTicks_ = 0;
            giAccumRecreateFailedLogged_ = false;
            const f64 mib = static_cast<f64>(static_cast<u64>(voxelResBuilt_) * voxelResBuilt_ *
                                             voxelResBuilt_ * 16ull) / (1024.0 * 1024.0);
            AVER_INFO("[Voxi] injection accumulator recreated ({:.0f} MiB); the rebuild that needed "
                      "it runs on the next tick", mib);
        } else if (!giAccumRecreateFailedLogged_) {
            giAccumRecreateFailedLogged_ = true;
            AVER_ERROR("[Voxi] injection accumulator could not be recreated at {}^3; GI stays without "
                       "a volume to inject into until it can be", voxelResBuilt_);
            // The placeholder (if one exists from an earlier free) stays exactly as bound, and
            // giAccumWanted_ stays true, so this is retried every tick rather than silently giving up
            // on GI forever.
        }
    } else if (giFreeAccumulator_ && voxelAccumTex_ && giEnabled() && !giForceRebuild_ &&
              giConvergeTicks_ == 0 && giQuietTicks_ >= kGiAccumulatorQuietTicks) {
        // (b) FREE. Every guard here is a reason NOT to free, stated as its negation: !giForceRebuild_
        // (a forced tick needs the accumulator EVERY tick -- see M4) and giConvergeTicks_ == 0 (a
        // still-converging bake is busy whatever giQuietTicks_ says -- see the gate bookkeeping in
        // prePass for why the two counters are kept separate rather than one resetting the other).
        if (!voxelAccumPlaceholder_) {
            rhi::TextureDesc pd;
            pd.dim    = rhi::TextureDim::Tex3D;
            pd.width  = 4; pd.height = 1; pd.depth = 1;
            pd.mips   = 1;
            pd.format = rhi::Format::R32Uint;
            pd.bind   = rhi::ResourceBind::UnorderedAccess;
            pd.initialState = rhi::ResourceState::UnorderedAccess;
            pd.debugName    = "Voxi injection accumulator placeholder";
            voxelAccumPlaceholder_ = res_->createTexture(pd);
        }
        if (voxelAccumPlaceholder_) {
            // REBIND BEFORE DESTROY, always -- aver-view-outlives-its-buffer.md, and this function's
            // own header comment: a binding set left pointing at voxelAccumTex_ past this point would
            // describe a destroyed resource the instant anything drew through it.
            res_->setUav(bindings_, 1, voxelAccumPlaceholder_, 0);
            res_->setUav(clearBindings_, 1, voxelAccumPlaceholder_, 0);
            res_->setUav(resolveBindings_, 1, voxelAccumPlaceholder_, 0);
            const f64 mib = static_cast<f64>(static_cast<u64>(voxelResBuilt_) * voxelResBuilt_ *
                                             voxelResBuilt_ * 16ull) / (1024.0 * 1024.0);
            res_->destroyTexture(voxelAccumTex_);   // fence-deferred on both backends
            voxelAccumTex_ = 0;
            AVER_INFO("[Voxi] injection accumulator freed after {} quiet GI tick(s): {:.0f} MiB "
                      "released", giQuietTicks_, mib);
        }
        // If the placeholder itself could not be created, nothing above ran and the accumulator
        // stays exactly as it was: "do not free" is the only safe answer when there is nothing to
        // rebind the live binding sets to first.
    }
}

// Builds a bottom-level structure for every referenced mesh, then one top-level structure over the
// replayed draw list. Publishes shadowParams.z so the lit pass knows whether it may trace. Settings::
// rtSkipUnchangedTlas gates all of that on rtAccelSnapshotUnchanged() -- see the "THE UNCHANGED GATE"
// block just below for what it skips.
void VoxiRenderer::buildAccelerationStructures(rhi::IRenderContext& ctx) {
    rtActive_ = false;
    cb_.shadowParams[2] = 0.0f;
    if (!rtSupported_ || settings_.rayTracing == Quality::Off || drawsPrev_.empty()) return;

    // ---- THE UNCHANGED GATE (Settings::rtSkipUnchangedTlas) ----
    //
    // See rtAccelSnapshotUnchanged()'s own comment for what "unchanged" checks. A MATCH means
    // tlas_, rtInstanceData_ and every SRV bound to them (bindings_ slots 2/3/4/5/9, all set the last
    // time this function actually ran its per-draw loop) are still exactly correct -- so the whole
    // body below is skipped: no BLAS/TLAS work, no instance-buffer rewrite or upload, no material
    // re-upload. What is NOT skipped is everything below that is NOT a function of drawsPrev_ and
    // that other code reads every frame regardless -- rtActive_, cb_.shadowParams[2], cb_.rtParams
    // (updateRtParamsPerFrame(), factored out for exactly this reuse) and cb_.rtParams[3], read from
    // rtGeometryReady_ rather than a fresh buildGeometryTable() call since that call is one of the
    // things being skipped and rtGeometryReady_ already says whether the UNCHANGED geometry table is
    // valid for the UNCHANGED rtInstanceData_ sitting behind it.
    if (settings_.rtSkipUnchangedTlas && rtAccelSnapshotUnchanged()) {
        ++rtAccelSkipped_;
        rtActive_ = true;
        cb_.shadowParams[2] = 1.0f;
        updateRtParamsPerFrame();
        cb_.rtParams[3] = rtGeometryReady_ ? 1.0f : 0.0f;
        reportRtAccelGate();
        return;
    }
    if (settings_.rtSkipUnchangedTlas) ++rtAccelRebuilt_;

    rhi::ScopedGpuStat gpuStat(ctx, "Voxi acceleration structures");
    tlasTranslucentThisBuild_ = 0;
    tlasAlphaMaskedThisBuild_ = 0;
    // ---- W10: two per-build scratch containers, hoisted into members (VoxiRenderer.hpp) ----
    // tlasInstScratch_ (was a local `inst`) and matConstantsScratch_ (was a local `matConstantsByKey`)
    // used to be reallocated from empty every single build. .clear() keeps the underlying storage, so
    // a steady-state scene reuses the same allocation indefinitely; only a build whose instance/
    // material count grows past the previous high-water mark pays a reallocation. Cleared HERE, right
    // after the early-return above, alongside rtInstanceData_ and the rest of this build's per-frame
    // state -- a build that returns early leaves nothing for the NEXT build to find half-populated,
    // because the next build clears both before reading either, not because this one avoided
    // touching them.
    tlasInstScratch_.clear();
    matConstantsScratch_.clear();
    tlasInstScratch_.reserve(drawsPrev_.size());
    rtInstanceData_.clear();
    rtInstanceMesh_.clear();
    rtInstanceMatKey_.clear();
    rebuiltThisFrame_.clear();
    rtInstancePrevWorld_.clear();
    u32 firstBuilds = 0;

    // M2(c): the CPU cost of this build's own per-draw loop, from just before the population pass
    // below to just after the main per-draw loop's closing brace -- NOT the BLAS/TLAS GPU recording
    // that follows it (ctx.buildBlas/ctx.buildTlas only RECORD commands onto ctx; they don't wait for
    // the GPU), which is already timed by gpuStat above. This is the CPU-only cost of walking
    // drawsPrev_ and filling rtInstanceData_/rtInstanceMesh_/rtInstanceMatKey_ -- what an instance-
    // count sweep on a streamed scene actually wants an answer to. See lastAccelBuildCpuMs().
    const auto accelBuildCpuStart = std::chrono::steady_clock::now();

    // ---- previous-transform tracking, pass 1: THIS build's population per (mesh, drawBinding) ----
    // See prevTransformGroupKey()'s header comment for the whole scheme. Counted over the FULL
    // drawsPrev_ list, not just draws with a usable BLAS, so a transient BLAS cache miss (a brand-new
    // mesh's first frame) never looks like a population change -- "has geometry to trace yet" and
    // "did the submitted SET change" are independent, and only the trust gate below needs the second.
    if constexpr (kTrackPrevTransforms) {
        prevGroupCountThisBuild_.clear();
        prevGroupOrdinal_.clear();
        nextTransformByKey_.clear();
        for (const Draw& d : drawsPrev_)
            ++prevGroupCountThisBuild_[prevTransformGroupKey(d.mesh, d.matSet)];
    }

    for (const Draw& d : drawsPrev_) {
        // ---- previous-transform tracking, pass 2: this draw's ordinal within its group ----
        // Incremented for EVERY draw, survivor or not -- ordinal numbering must match what pass 1
        // counted over, or "the Nth draw with this key" would mean two different things. Only the
        // STORE below (after the BLAS check) is conditional on survival, so rtInstancePrevWorld_
        // lines up index-for-index with rtInstanceData_, same as rtInstanceMesh_.
        u64 prevGroupKey = 0, prevInstKey = 0;
        if constexpr (kTrackPrevTransforms) {
            prevGroupKey = prevTransformGroupKey(d.mesh, d.matSet);
            const u32 prevOrdinal = prevGroupOrdinal_[prevGroupKey]++;
            prevInstKey = prevGroupKey ^ static_cast<u64>(prevOrdinal);
        }
        prevInstKey *= 1099511628211ull;

        auto it = blas_.find(d.mesh);
        // A cached structure whose mesh has been destroyed underneath us -- reachable, not
        // theoretical: the instance list is drawsPrev_, LAST frame's draws, so a mesh freed between
        // frames is still named here, and handing its structure to the TLAS would have the GPU
        // traverse freed memory. Asking the factory what the BLAS is actually for catches it without
        // every destroyMesh caller having to remember to tell this cache.
        if (it != blas_.end() && it->second && res_->blasMesh(it->second) != d.mesh) {
            blas_.erase(it);
            it = blas_.end();
        }
        if (it == blas_.end()) {
            // Returns 0 for a destroyed mesh, which is then recorded and not retried.
            const rhi::BlasHandle nb = res_->createBlas(d.mesh);
            if (nb) { ctx.buildBlas(nb); ++firstBuilds; }
            // A zero is recorded too, so a mesh that cannot produce a BLAS is not retried each frame.
            it = blas_.emplace(d.mesh, nb).first;
        } else if (it->second && dev_->meshVertexBuffer(d.mesh)) {
            // A mesh whose vertices are written by compute invalidates its own structure every frame
            // -- memoising it (right for static geometry) would give a skinned character a ray-traced
            // shadow frozen at the pose the structure was first built with, invisible in the raster
            // image. The skinning dispatch has already left the buffer in GeometryRead, since that
            // feature registers BEFORE this one.
            // Rebuilt ONCE PER MESH PER FRAME, not once per draw -- the draw list holds one entry per
            // instance, so a mesh drawn twice used to be rebuilt twice, the second a wasted full
            // PREFER_FAST_TRACE build over identical vertices. The linear scan is over the DISTINCT
            // dynamic meshes in one frame, a handful.
            if (std::find(rebuiltThisFrame_.begin(), rebuiltThisFrame_.end(), d.mesh) ==
                rebuiltThisFrame_.end()) {
                ctx.buildBlas(it->second);
                rebuiltThisFrame_.push_back(d.mesh);
            }
            if (!dynamicBlasLogged_) {
                AVER_INFO("[Voxi] mesh {} has compute-written vertices; its bottom-level structure "
                          "is rebuilt every frame rather than cached", d.mesh);
                dynamicBlasLogged_ = true;
            }
        }
        const rhi::BlasHandle b = it->second;
        if (!b) continue;
        rhi::TlasInstance i;
        // Engine convention, handed over untouched: the backend owns the transpose DXR wants.
        std::memcpy(i.world, d.world, sizeof(i.world));
        // TWO LANES IN THE MASK, so a ray can choose whether translucent geometry exists for it --
        // every instance used to be 0xFF, right for a shadow ray and wrong for anything wanting only
        // solid surfaces. Splitting it lets a reflection/AO ray opt out later via a narrower mask
        // without a structure rebuild.
        // NOW THREE LANES: translucency is a property of the material, hiddenFromOwner of WHO IS
        // LOOKING, and the two cannot co-occur -- a translucent-lane draw is diverted by submitDraw's
        // blended branch before it reaches the owner-hide walk. Tested in this order so translucency
        // keeps its existing answer unchanged.
        i.mask = d.translucent      ? kRtMaskTranslucent
               : d.hiddenFromOwner  ? kRtMaskOwnerHidden
                                    : kRtMaskOpaque;
        // FORCE_NON_OPAQUE only for the translucent lane. createBlas marks every geometry OPAQUE,
        // which permits the hardware to skip any-hit entirely -- so without this override a
        // Proceed() loop would never be offered a candidate to inspect and the pane would simply
        // stop the ray like a wall. Set per instance so opaque geometry keeps the fast path.
        i.flags = d.translucent ? rhi::TlasInstanceFlag_ForceNonOpaque : rhi::TlasInstanceFlag_None;
        if (d.translucent) ++tlasTranslucentThisBuild_;
        i.blas = b;
        // An index into rtInstanceData_, which a reflection ray reads to find the triangle it hit
        // and the surface's albedo. Assigned HERE, in the same loop that decides which instances
        // survive, so the two lists cannot drift -- an id assigned earlier would be wrong for every
        // instance after one whose acceleration structure failed to build.
        i.instanceId = static_cast<u32>(rtInstanceData_.size()) & rhi::kMaxTlasInstanceId;
        tlasInstScratch_.push_back(i);

        RtInstance ri;
        std::memcpy(ri.objectToWorld, d.world, sizeof(ri.objectToWorld));
        ri.albedo[0] = d.color[0]; ri.albedo[1] = d.color[1]; ri.albedo[2] = d.color[2];
        // From the same Draw, and available here all along.
        ri.metallic = d.metallic;
        ri.roughness = d.roughness;
        // Filled in by buildGeometryTable, which is what knows where each mesh landed.
        ri.firstIndex = 0;
        ri.firstVertex = 0;
        // materialIndex is a PLACEHOLDER -- buildMaterialTable() MUST run before buildGeometryTable(),
        // which uploads this array; that ordering is load-bearing and was wrong once (see the call
        // site's comment for what it cost). It overwrites every element once this loop (and
        // rtInstanceMatKey_, in lockstep) has run and the full material set is known -- the final
        // index depends on the sorted order of the whole set, so it can't be resolved per-draw here.
        ri.materialIndex = 0;

        // ---- resolving THIS draw's material key and, the first time it is seen, its bytes ----
        // AUTHORED means d.matSet is one of materials_'s own binding sets AND not its fallback set.
        // GameRender.cpp/SandboxApp.cpp resolve `materials->bindingSet(authored)` for every draw, so
        // an unauthored draw gets back the identical fallback handle regardless of colour (see
        // GameRender.cpp:127-136 and SandboxApp.cpp's matching lambda) -- so the handle is a useful
        // dedup key, and d.mat holds real per-material bytes, only on the AUTHORED branch; everything
        // else is keyed and built from its own colour/metal/rough instead (synthMaterialKey, above).
        const bool authored = materials_.ownsBindingSet(d.matSet) &&
                               d.matSet != materials_.fallbackBindingSet();
        const u64 matKey = authored
            ? ((1ull << 63) | static_cast<u64>(d.matSet))
            : synthMaterialKey(d.color, d.metallic, d.roughness);
        if (matConstantsScratch_.find(matKey) == matConstantsScratch_.end()) {
            pbr::MaterialConstants mc;
            if (authored) {
                // Real per-material bytes, captured at submit time -- reflectance, f90, flags,
                // emissive factor, all of it, not just the three floats RtInstance already carried.
                std::memcpy(&mc, d.mat, sizeof(mc));

                // AND ITS TEXTURES, resolved to indices into the one bindless table a ray hit can
                // index by computed value -- a ray hit has no equivalent of the raster path's per-draw
                // descriptor table. Read from the binding set rather than MaterialDesc, so the ray
                // path samples exactly what writeSlots BOUND, identity fallbacks included: an unset
                // slot samples flat white/normal on both paths with no per-slot branch in the shader.
                if (const auto* tex = materials_.textures(d.matSet)) {
                    for (u32 t = 0; t < pbr::kTextureSlotCount; ++t)
                        mc.texIndex[t] = residentTexture((*tex)[t]);
                }
            } else {
                // No authored constant block, so one is built from the SAME colour/metal/rough the
                // raster path shades this surface with, layered onto the fallback for everything
                // unspecified (reflectance 0.04, f90 1.0, no maps) -- new information a ray hit did
                // not have before, when RtInstance carried only those three floats directly.
                mc = materials_.fallbackConstants();
                mc.baseColorFactor[0] = d.color[0];
                mc.baseColorFactor[1] = d.color[1];
                mc.baseColorFactor[2] = d.color[2];
                mc.baseColorFactor[3] = d.color[3];
                mc.metallicFactor  = d.metallic;
                mc.roughnessFactor = d.roughness;
            }
            matConstantsScratch_.emplace(matKey, mc);
        }
        rtInstanceMatKey_.push_back(matKey);

        // ---- ALPHA-MASKED GEOMETRY JOINS THE NON-OPAQUE LANE ------------------------------------
        //
        // Until this, `translucent` was the ONLY thing that un-opaqued an instance, so a cutout
        // material -- foliage, grates, chain-link -- was traced as the solid sheet its triangles
        // describe. Every leaf card was its own bounding rectangle in shadows, reflections, GI and
        // (since ray-driven is the default) in primary visibility too, while the raster prepass
        // clipped the same material correctly. The two paths disagreed and the default was wrong.
        //
        // PATCHED ONTO THE INSTANCE ALREADY PUSHED, because the material cannot be resolved any
        // earlier: matKey is only known here, several statements after
        // tlasInstScratch_.push_back(i). Reordering the loop to resolve it first would work too and
        // is a bigger change to a loop whose instanceId/rtInstanceData_ lockstep is load-bearing and
        // documented as such above.
        //
        // NOT THE MASK, only the flags: this geometry still belongs to the OPAQUE lane. It occludes,
        // casts shadow and is a valid reflection hit -- it simply has holes, which is what any-hit
        // is for. Moving it to kRtMaskTranslucent would hide it from every ray that asks for solid
        // surfaces only.
        if (!d.translucent &&
            (matConstantsScratch_.at(matKey).flags & pbr::MaterialFlag_AlphaMask) != 0) {
            tlasInstScratch_.back().flags |= rhi::TlasInstanceFlag_ForceNonOpaque;
            ++tlasAlphaMaskedThisBuild_;
        }

        rtInstanceData_.push_back(ri);
        rtInstanceMesh_.push_back(d.mesh);

        // ---- previous-transform tracking, pass 2 continued: look up, or admit there is none ----
        // TRUSTED only when this group's total population matches last build's -- a population change
        // drops the WHOLE group rather than risk an ordinal pointing at a different instance's old
        // transform. "Brand new key" and "population changed" both fall to the else branch: this
        // instance's own current transform reported back as "previous", the honest zero-velocity
        // answer either way.
        if constexpr (kTrackPrevTransforms) {
            const auto lastCountIt = prevGroupCountLastBuild_.find(prevGroupKey);
            const bool trustGroup = lastCountIt != prevGroupCountLastBuild_.end() &&
                                    lastCountIt->second == prevGroupCountThisBuild_[prevGroupKey];
            std::array<f32, 16> prevWorld;
            const auto foundIt = trustGroup ? prevTransformByKey_.find(prevInstKey)
                                             : prevTransformByKey_.end();
            if (foundIt != prevTransformByKey_.end()) {
                prevWorld = foundIt->second;
            } else {
                std::memcpy(prevWorld.data(), d.world, sizeof(prevWorld));
            }
            rtInstancePrevWorld_.push_back(prevWorld);
        }

        // This build's OWN transform becomes "last build's answer" next time this key is seen --
        // staged into nextTransformByKey_, swapped into prevTransformByKey_ whole once the loop
        // finishes, never written in place: an instance dropped between builds must not leave its old
        // transform sitting under a key some unrelated later instance could reuse.
        if constexpr (kTrackPrevTransforms) {
            std::array<f32, 16> curWorld;
            std::memcpy(curWorld.data(), d.world, sizeof(curWorld));
            nextTransformByKey_[prevInstKey] = curWorld;
        }
    }

    // M2(c): stop where the CPU-only work started above ends -- the main per-draw loop just closed;
    // everything from here down either records GPU commands (ctx.buildTlas, buildGeometryTable) or is
    // itself covered by its own accounting (buildMaterialTable's own once-logged cost/re-upload
    // report). An early return above (no ray tracing wanted, an empty draw list) never reaches this
    // line, which is why lastAccelBuildCpuMs_ simply keeps its previous value on that path.
    lastAccelBuildCpuMs_ = std::chrono::duration<f64, std::milli>(
        std::chrono::steady_clock::now() - accelBuildCpuStart).count();

    // Commits this build's previous-transform bookkeeping so the NEXT build compares against it,
    // unconditionally -- even a build where nothing survived correctly clears both maps to empty
    // rather than leaving a stale generation, which is the honest state to resync from later.
    if constexpr (kTrackPrevTransforms) {
        prevGroupCountLastBuild_ = std::move(prevGroupCountThisBuild_);
        prevTransformByKey_ = std::move(nextTransformByKey_);
    }

    // Memory-cost report, said ONCE, sized from the REAL instance count this build reached rather
    // than a number that drifts with kMaxDraws or content. Reports the flat array's cost plus the
    // four maps' overhead called out separately, since std::unordered_map's per-node bookkeeping is
    // real but implementation-defined and unmeasurable from here.
    if (!prevTransformMemoryLogged_ && !rtInstancePrevWorld_.empty()) {
        prevTransformMemoryLogged_ = true;
        const usize n = rtInstancePrevWorld_.size();
        const usize payloadBytes = n * (sizeof(u64) + sizeof(std::array<f32, 16>));   // 8 + 64 = 72 B
        AVER_INFO("[Voxi] previous-instance-transform tracker: {} instances, {} bytes/instance payload "
                  "(8-byte key + 16-float transform) = {} KB of flat payload this build. The four "
                  "unordered_maps that produce it (two population-count maps, two transform maps) add "
                  "their own per-node overhead on top of that, typically on the order of the payload "
                  "itself for a map this small -- so figure roughly {}-{} KB in real bytes, briefly "
                  "doubled while both this build's and the superseded generation are both live around "
                  "the swap above, then back to one generation. Against the ~54 MB the three new "
                  "render targets themselves cost at scene resolution, and the ~144 MB the RT "
                  "histories already spend, this is noise.",
                  n, payloadBytes / n, payloadBytes / 1024, payloadBytes / 1024, (payloadBytes * 2) / 1024);
    }

    // gpuStat's destructor closes the marker here -- used to be `ctx.popMarker(); return;`, one of
    // two exits that both had to remember to pop by hand. See ScopedGpuStat's comment for the bug
    // that duplication caused, which this class exists to make impossible.
    //
    // rtAccelKey_/rtAccelSnapValid_ are deliberately left untouched on this exit rather than
    // invalidated: this function reached here because the gate above already said "rebuild" (a key
    // mismatch, a forced condition, or no snapshot yet), so whatever they held describes the LAST
    // build that actually ran ctx.buildTlas below -- which this exit does not reach, so tlas_ is
    // exactly as that last build left it, still correctly described by them.
    if (tlasInstScratch_.empty()) return;

    ctx.buildTlas(tlas_, tlasInstScratch_.data(), static_cast<u32>(tlasInstScratch_.size()));
    if (tlasTranslucentThisBuild_ && tlasTranslucentLogged_ != tlasTranslucentThisBuild_) {
        tlasTranslucentLogged_ = tlasTranslucentThisBuild_;
        AVER_INFO("[Voxi] acceleration structure: {} instance(s), {} in the translucent lane "
                  "(non-opaque, visible to shadow rays only)",
                  tlasInstScratch_.size(), tlasTranslucentThisBuild_);
    }
    if (tlasAlphaMaskedThisBuild_ && tlasAlphaMaskedLogged_ != tlasAlphaMaskedThisBuild_) {
        tlasAlphaMaskedLogged_ = tlasAlphaMaskedThisBuild_;
        AVER_INFO("[Voxi] acceleration structure: {} instance(s) alpha-masked (non-opaque, so every "
                  "ray tests the cutout instead of hitting the card)", tlasAlphaMaskedThisBuild_);
    }
    rtActive_ = true;
    cb_.shadowParams[2] = 1.0f;

    // cb_.rtParams[0..2]: NOT a function of drawsPrev_ -- see updateRtParamsPerFrame()'s own comment.
    // Factored out so the gate's skip branch above can set the identical values without duplicating
    // them.
    updateRtParamsPerFrame();
    // MATERIAL TABLE FIRST, AND THE ORDER IS THE WHOLE POINT. buildGeometryTable() below uploads
    // rtInstanceData_ to the GPU; buildMaterialTable() fills in every materialIndex, which the
    // per-draw loop leaves at a placeholder 0. Called the other way round, as this stood until now,
    // the upload carried the PLACEHOLDER and the fix-up landed on a CPU copy nobody read again: every
    // ray hit in ray-driven mode indexed gRtMaterials[0], the fallback row. Silent, because RtInstance
    // already carries albedo/metallic/roughness per-draw -- what came from the fallback instead was
    // everything else (reflectance, f90, ior, transmission, subsurface, graphId). Found by asking why
    // a coat authored at weight 1 changed nothing in the default render mode and everything in the
    // others. Nothing forces the old order -- buildMaterialTable touches no geometry and needs no cb_
    // gate, since every index it assigns is valid whether or not the GPU upload below succeeds.
    // Before the table is built: buildMaterialTable asks residentTexture() for indices, and a null
    // table would leave every one unbound.
    ensureTextureTable();
    buildMaterialTable(matConstantsScratch_);
    // w > 0.5 tells the lit pass it may trace a reflection ray. It is only true when the flat
    // geometry table is actually there, because a reflection that hits geometry it cannot look up
    // would read a neighbour's triangle rather than fail visibly.
    cb_.rtParams[3] = buildGeometryTable(ctx) ? 1.0f : 0.0f;
    if (!rtLogged_) {
        AVER_INFO("[Voxi] RayQuery active ({} instances, {} bottom-level structures)",
                  static_cast<u32>(tlasInstScratch_.size()), static_cast<u32>(blas_.size()));
        rtLogged_ = true;
    }
    // WHAT THE ACCELERATION STRUCTURES COST, as a count rather than an impression -- every build here
    // is a full PREFER_FAST_TRACE build (the RHI has no refit verb), so this number IS the bill. Also
    // the only way to see the rebuild predicate (IDevice::meshVertexBuffer non-zero) go wrong: if that
    // stops being true, a static scene silently rebuilds everything every frame and looks identical.
    // Keyed on BOTH halves rather than their sum, since two first-time builds becoming two rebuilds is
    // the same total but a very different statement about the cache.
    const u32 rebuilds = (static_cast<u32>(rebuiltThisFrame_.size()) << 16) | (firstBuilds & 0xFFFFu);
    if (rebuilds != lastBlasRebuilds_) {
        AVER_INFO("[Voxi] bottom-level builds this frame: {} ({} first-time, {} rebuilt) over {} "
                  "draws of {} distinct meshes",
                  firstBuilds + static_cast<u32>(rebuiltThisFrame_.size()), firstBuilds,
                  static_cast<u32>(rebuiltThisFrame_.size()), static_cast<u32>(drawsPrev_.size()),
                  static_cast<u32>(blas_.size()));
        lastBlasRebuilds_ = rebuilds;
    }

    // THE GATE'S OWN BOOKKEEPING, kept warm regardless of whether Settings::rtSkipUnchangedTlas is on
    // right now: a build that reaches here always leaves tlas_/rtInstanceData_ in a state
    // rtAccelDrawsKey() can describe, so recording it costs one more pass over drawsPrev_ (negligible
    // next to the TLAS build and buffer uploads this build already paid for) and means turning the
    // setting on mid-session, or back on after a frame it was off, never has to wait an extra frame
    // to prime. Only the SKIPPED/REBUILT counters and the report itself are gated on the setting --
    // see reportRtAccelGate()'s own comment -- so the ratio it prints describes ticks the gate was
    // actually consulted for, not ticks it was switched off.
    takeRtAccelSnapshot();
    if (settings_.rtSkipUnchangedTlas) reportRtAccelGate();
}

// cb_.rtParams[0..2]: the sun's angular size (as a tangent, so the shader multiplies rather than
// re-derives it), the shadow ray count and the ray bias -- none of them a function of drawsPrev_, all
// three read every frame by shadowPass()/PSRayDriven regardless of whether buildAccelerationStructures
// rebuilt anything this frame. Split out of that function's own tail so its gate's skip branch can set
// them without duplicating the derivation.
void VoxiRenderer::updateRtParamsPerFrame() {
    // How fast a ray-traced shadow edge softens is the SUN's angular size, not a tuned constant --
    // the disc subtends about half a degree, and rtShadow spreads its rays across exactly that.
    // Taken from the sky model rather than duplicated, so a scene that moves the sun or widens the
    // disc gets penumbrae that agree with its own sky.
    const f32 halfAngle = dev_->skyAtmosphere().sunAngularDiameterDeg * 0.5f * 0.01745329252f;
    cb_.rtParams[0] = std::tan(halfAngle);
    cb_.rtParams[1] = static_cast<f32>(rtShadowRays_);
    // Base ray bias in centimetres, scaled by view distance in the shader. Small enough not to
    // detach a contact shadow, large enough that a surface does not intersect its own rays.
    cb_.rtParams[2] = 0.05f;
}

// True when buildAccelerationStructures() MUST run its real per-draw loop this frame, regardless of
// what rtAccelDrawsKey() says -- the two things in that loop no key can make safe to skip, checked
// here the CHEAP way instead: an unordered_map lookup and (at most) one virtual call per draw, none
// of the BLAS creation, instance population or material resolution the real loop also does.
bool VoxiRenderer::rtAccelMustForceRebuild() const {
    for (const Draw& d : drawsPrev_) {
        // A compute-skinned mesh's BLAS is rebuilt INSIDE the per-draw loop every single call (see
        // that check's own comment, just above where the loop calls ctx.buildBlas a second time) --
        // freezing the structure here would show a shadow or reflection at whatever pose it last
        // held, silently, for as long as the rest of the draw list held still. Its mere presence in
        // this frame's draw list is reason enough; nothing about ITS key changing is required.
        if (dev_ && dev_->meshVertexBuffer(d.mesh)) return true;
        // THE SAME "destroyed-and-reused mesh handle" CHECK THE REAL LOOP MAKES (see its own comment,
        // just above where it erases the stale entry), run here instead of trusted to the key: a BLAS
        // cached under this mesh handle that the resource factory no longer attributes to it is dead,
        // and a gate that matched on the key alone would leave the TLAS pointing straight at it.
        const auto it = blas_.find(d.mesh);
        if (it != blas_.end() && it->second && res_->blasMesh(it->second) != d.mesh) return true;
    }
    return false;
}

// One draw's material identity into the running FNV chain `h`, for rtAccelDrawsKey() and
// giDrawsKey()/giDrawsSubKeys() alike (see the declaration).
//
// ---- material identity: d.matSet alone is not enough ----
// MaterialSystem::gpuMaterialRevision()'s own comment (MaterialSystem.hpp) says why plainly:
// "touch()/MaterialLibrary::update() on a material's own factors or textures never moves its
// row, only its CONTENTS at the row it already has, so an ordinary edit does not bump this".
// pbr::materialGraphs().revision() does not help either -- it moves only when a GRAPH's
// generated HLSL changes, not when a factor or a texture reference does. Neither catches an
// in-place material edit, which the per-draw loop's OWN matKey (d.matSet alone, for an
// authored draw) would silently miss too. So this hashes what that loop actually reads instead
// of trusting a handle: d.mat -- the MaterialConstants submitDraw() re-captured THIS frame
// from whatever the caller currently holds for the material, live edits included, regardless
// of whether this gate exists -- plus the resolved texture set materials_.textures() reports
// for it. d.mat ALONE is not enough for the texture half: packMaterial() always leaves
// texIndex at the unbound placeholder (MaterialGpu.cpp) since only THIS renderer's own
// residentTexture() ever fills it, downstream of where a gate would already have decided -- so
// reassigning a material's texture changes none of d.mat's bytes.
void VoxiRenderer::hashDrawMaterialInto(u64& h, const Draw& d) const {
    const bool authored = materials_.ownsBindingSet(d.matSet) &&
                           d.matSet != materials_.fallbackBindingSet();
    h ^= static_cast<u64>(d.matSet); h *= 1099511628211ull;
    h ^= authored ? 1ull : 0ull; h *= 1099511628211ull;
    if (authored) {
        for (usize i = 0; i < sizeof(d.mat); ++i) {
            h ^= static_cast<u64>(d.mat[i]);
            h *= 1099511628211ull;
        }
        if (const auto* tex = materials_.textures(d.matSet)) {
            for (u32 t = 0; t < pbr::kTextureSlotCount; ++t) {
                h ^= static_cast<u64>((*tex)[t]);
                h *= 1099511628211ull;
            }
        }
    } else {
        // UNAUTHORED: the per-draw loop builds its constants from colour/metallic/roughness alone
        // (synthMaterialKey), the same three fields both callers hash for the identical reason.
        for (u32 i = 0; i < 4; ++i) {
            u32 bits = 0;
            std::memcpy(&bits, &d.color[i], sizeof(bits));
            h ^= static_cast<u64>(bits);
            h *= 1099511628211ull;
        }
        u32 mb = 0, rb = 0;
        std::memcpy(&mb, &d.metallic, sizeof(mb));
        std::memcpy(&rb, &d.roughness, sizeof(rb));
        h ^= (static_cast<u64>(mb) << 32) ^ static_cast<u64>(rb);
        h *= 1099511628211ull;
    }
}

// ORDER-INDEPENDENT, for the identical reason giDrawsKey() is (see its own comment further down this
// file): occlusion culling reorders drawsPrev_ every frame, so this sums a per-draw hash rather than
// folding one in list order, the only way a reshuffled-but-otherwise-identical draw list still matches.
//
// COVERS EVERYTHING THE PER-DRAW LOOP AND THE TWO TABLES BELOW IT READ to decide an instance's TLAS
// entry, its RtInstance record and its row in the material table: the mesh, its world transform, the
// two flags that pick its mask/ForceNonOpaque lane (translucent, hiddenFromOwner), and its material.
u64 VoxiRenderer::rtAccelDrawsKey() const {
    u64 key = 0;
    u64 counted = 0;
    for (const Draw& d : drawsPrev_) {
        u64 h = 1469598103934665603ull;
        h ^= static_cast<u64>(d.mesh); h *= 1099511628211ull;
        for (u32 i = 0; i < 16; ++i) {
            u32 bits = 0;
            std::memcpy(&bits, &d.world[i], sizeof(bits));
            h ^= static_cast<u64>(bits);
            h *= 1099511628211ull;
        }
        // The two flags i.mask/i.flags are built from, hashed as the bools the loop actually branches
        // on rather than the mask/flag bits they produce -- cheaper, and the two can never disagree.
        h ^= (d.translucent ? 1ull : 0ull) | (d.hiddenFromOwner ? 2ull : 0ull);
        h *= 1099511628211ull;

        hashDrawMaterialInto(h, d);

        // FINALISE BEFORE ADDING -- the same avalanche giDrawsKey() uses and for the identical reason:
        // FNV's last step leaves neighbouring inputs correlated in the low bits, and plain addition of
        // correlated values collides far more readily than addition of decorrelated ones.
        h ^= h >> 33; h *= 0xff51afd7ed558ccdull;
        h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull;
        h ^= h >> 33;
        key += h;
        ++counted;
    }
    // THE COUNT, mixed in rather than added -- see giDrawsKey()'s own comment on exactly what this
    // does and does not catch; the same true here for the identical reason.
    key ^= counted * 1099511628211ull;
    return key;
}

// True when nothing buildAccelerationStructures() would read from drawsPrev_ -- or from the BLAS
// cache and dynamic-mesh state its per-draw loop also consults -- has changed since the last build
// that actually ran, so that build's tlas_, rtInstanceData_ and every SRV bound to them are still
// exactly correct as they stand. MODELLED ON giSnapshotUnchanged() (read it first): same "reject once
// per reason, log it" shape, same split between this const check and the caller taking a fresh
// snapshot (takeRtAccelSnapshot()) only once it has decided a rebuild is actually going to run.
bool VoxiRenderer::rtAccelSnapshotUnchanged() const {
    const auto reject = [this](u32 bit, const char* which) {
        if (!(rtAccelGateWhyMask_ & (1u << bit))) {
            rtAccelGateWhyMask_ |= (1u << bit);
            AVER_INFO("[Voxi] RT accel-structure gate rejected on: {}", which);
        }
        return false;
    };
    if (!rtAccelSnapValid_) return reject(0, "no snapshot yet (expected once)");
    if (rtAccelMustForceRebuild())
        return reject(1, "compute-skinned mesh present or a cached BLAS handle went stale");
    if (rtAccelDrawsKey() != rtAccelKey_) return reject(2, "draw list changed");
    return true;
}

// Records what the build about to run (or that just ran -- see buildAccelerationStructures' own call
// site) was computed from. Mirrors takeGiSnapshot(): the non-const half of the gate, called once a
// build has actually happened rather than from inside the const check above.
void VoxiRenderer::takeRtAccelSnapshot() {
    rtAccelKey_ = rtAccelDrawsKey();
    rtAccelSnapValid_ = true;
}

// The widening-interval "N rebuilt / M skipped" report, same shape as the GI rebuild gate's own (see
// prePass(), the block right after giSnapshotUnchanged()) and for the identical reason stated there:
// a window whose first report lands mid-load-in describes the phase nobody is asking about, so this
// reports both the lifetime ratio and the ratio since the last report, at doubling tick counts.
void VoxiRenderer::reportRtAccelGate() {
    const u64 ticks = rtAccelSkipped_ + rtAccelRebuilt_;
    if (ticks < rtAccelGateNextReport_) return;
    const u64 winTicks = ticks - rtAccelGateLastTicks_;
    const u64 winSkipped = rtAccelSkipped_ - rtAccelGateLastSkipped_;
    AVER_INFO("[Voxi] RT accel-structure gate: {} rebuilt / {} skipped of {} tick(s) "
              "-- {}% avoided overall, {}% since the last report",
              rtAccelRebuilt_, rtAccelSkipped_, ticks, (rtAccelSkipped_ * 100) / ticks,
              winTicks ? (winSkipped * 100) / winTicks : 0);
    rtAccelGateLastTicks_ = ticks;
    rtAccelGateLastSkipped_ = rtAccelSkipped_;
    rtAccelGateNextReport_ = ticks * 2;   // 64, 128, 256, ... -- a handful of lines, not a flood
}

// Groups a previous-transform key by (mesh, drawBinding) -- see the long comment above this method's
// declaration in VoxiRenderer.hpp for why this pair and not mesh alone, and for the ordinal that
// disambiguates instances sharing one group. FNV-1a with the same offset basis and prime giDrawsKey()
// and buildGeometryTable's own hash already use, so this reads as the same recipe, not a third one.
u64 VoxiRenderer::prevTransformGroupKey(rhi::MeshHandle mesh, rhi::BindingSetHandle matSet) const {
    u64 key = 1469598103934665603ull;
    key ^= static_cast<u64>(mesh);   key *= 1099511628211ull;
    key ^= static_cast<u64>(matSet); key *= 1099511628211ull;
    return key;
}

// Builds one orthographic light frustum per cascade, fitted to a slice of the camera's view, and
// writes the matrices and splits into cb_. Returns the usable cascade count, 0 if there is no camera.
// A hash of what voxelizePass would rasterise: every draw's mesh and full world transform, the same
// FNV-style mix buildGeometryTable uses over rtInstanceMesh_. Hashed as raw float BITS, not compared
// with a tolerance: a cache that tolerates "almost the same" transform shows the wrong lighting for
// a while and then stops.
//
// ORDER-INDEPENDENT, AND IT USED TO BE THE OPPOSITE ON PURPOSE. The old comment here argued that a
// reordered draw list "changes the injection order into the atomic accumulator, so it is NOT the
// same result". The premise is true and the conclusion did not follow, because it made the gate
// answer a question nobody asked: the gate exists to say "would rebuilding produce the volume I
// already have", and a float-rounding difference in the last bits of an atomic accumulation is not
// a different volume in any sense a viewer can see. What it DID do was reject on pure reordering.
//
// MEASURED: PTTest sets RENDER.OCCLUSIONCULL 1, so the per-entity walk visits occlusionOrder_
// (SandboxApp's cluster draw loop), which is rebuilt every frame by partitioning entities on last frame's
// hierarchical-Z result against the CURRENT viewProj. Rotating the camera reshuffles that partition,
// so a completely static world submitted a differently-ORDERED draw list every tick, and this hash
// rejected the gate on 92 of 128 ticks -- forcing a full revoxelisation of a volume whose contents
// had not changed at all. The cost of that is in [[the commit that landed this]]; the cost of the
// alternative is float noise in the low bits of a bounced-light accumulation.
//
// WRAPPING ADDITION over per-draw hashes, which is commutative, so reordering cannot change it.
// Addition alone is weak against the obvious collisions -- swapping two draws' contents, or a draw
// appearing twice -- so the COUNT is folded in at the end and each draw's hash is finalised
// (xor-shift-multiply) before it is added, which decorrelates the addends. This was the one part of
// the reviewed design marked mandatory rather than optional, and it is the reason a count exists here
// at all: without it, a list that gains a draw and loses a different one summing to the same total
// would read as unchanged.
// True when voxelizePass would actually inject this draw into the volume.
//
// THE BOUNDS TEST IS THE ONE THAT WAS MISSING FROM THE HASHES, and its absence was the exact
// failure both of them warn about: a draw beyond the volume changed giDrawsKey, the gate reported
// "draw list changed" and forced a full rebuild -- and then voxelizePass culled that same draw and
// produced a bit-identical volume. All of the gate's cost, none of its benefit, which is what
// giDrawsKey's own comment says the skinned-mesh skip exists to prevent.
//
// It matters most exactly where it is worst: a streamed level moving props kilometres away, or a
// scatter layer well outside a level-fitted GI volume, re-keyed the volume on every tick.
bool VoxiRenderer::giVoxelisedDraw(const Draw& d) const {
    if (d.translucent) return false;
    if (dev_ && dev_->meshVertexBuffer(d.mesh)) return false;
    if (d.boundsRadius < 0.0f) return true;   // no bounds stated: assume it counts, as the pass does
    const Vec3 volCentre{center_[0], center_[1], center_[2]};
    const f32 volRadius = (extent_ > 1.0f ? extent_ : 1.0f) * 1.7320508f;
    return dist(Vec3{d.boundsCentre[0], d.boundsCentre[1], d.boundsCentre[2]}, volCentre)
           <= volRadius + d.boundsRadius;
}

u64 VoxiRenderer::giDrawsKey() const {
    u64 key = 0;
    u64 counted = 0;
    for (const Draw& d : drawsPrev_) {
        // Translucent, skinned, and OUT OF THE VOLUME are all "the pass will not inject this", and
        // all three now come from one predicate rather than three hand-copied tests -- see
        // giVoxelisedDraw for what the third one was costing while it was missing here.
        if (!giVoxelisedDraw(d)) continue;
        // PER DRAW, from the same offset basis the whole-list chain used to start from. Everything
        // mixed in here is identical to before; only where the running value lives has changed.
        u64 h = 1469598103934665603ull;
        // depthMesh, NOT mesh. voxelizePass draws d.depthMesh (see its ctx.drawMesh /
        // dispatchMeshFor), and this hash's whole contract is to agree with that pass exactly -- the
        // skips below say so in as many words. Hashing d.mesh meant the gate watched a handle the
        // pass never reads: with cluster LOD on, d.mesh is a per-frame cut rebuilt whenever the
        // visible cluster set changes, so a rotating camera re-keyed a volume whose actual injected
        // geometry (the stable depth proxy) had not moved at all.
        h ^= static_cast<u64>(d.depthMesh);
        h *= 1099511628211ull;
        for (u32 i = 0; i < 16; ++i) {
            u32 bits = 0;
            std::memcpy(&bits, &d.world[i], sizeof(bits));
            h ^= static_cast<u64>(bits);
            h *= 1099511628211ull;
        }
        // The material lands in the baked radiance (voxelizePass binds d.matSet/d.mat for PSVoxel), so
        // a material edit with no movement must still rebuild. This used to hash colour/metallic/
        // roughness only, which is all an unauthored draw has, but an authored material's real
        // constants (emissiveFactor, reflectance, ...) never moved it: a lamp edited in the Material
        // Editor never reached the bounced light.
        hashDrawMaterialInto(h, d);
        // FINALISE BEFORE ADDING. FNV's last step leaves neighbouring inputs correlated in the low
        // bits, and plain addition of correlated values collides far more readily than addition of
        // decorrelated ones. This is splitmix64's finaliser, used here only as an avalanche.
        h ^= h >> 33; h *= 0xff51afd7ed558ccdull;
        h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull;
        h ^= h >> 33;
        key += h;          // wrapping, and commutative: this is the whole point
        ++counted;
    }
    // THE COUNT, mixed in rather than added. IT DISTINGUISHES N DRAWS FROM N+1 AND NOTHING ELSE --
    // the note above used to claim it also stopped "a list that gains a draw and loses a different
    // one", and that is exactly the case it cannot help with: the length is identical either side, so
    // this term contributes identical bits. What actually separates {A,B} from {A,C} is that two
    // splitmix-finalised 64-bit hashes do not happen to sum alike, i.e. a ~2^-64 coincidence rather
    // than a designed defence. Worth keeping (a pure add or remove IS caught here, cheaply) and not
    // worth believing more of.
    key ^= counted * 1099511628211ull;
    return key;
}


// Splits giDrawsKey's inputs into independent axes, so "draw list changed" can name WHICH axis.
// Same skips and same per-draw finalise as giDrawsKey, so a difference here is a real difference
// there. Order-independent for the same reason.
void VoxiRenderer::giDrawsSubKeys(u64& count, u64& mesh, u64& world, u64& mat) const {
    count = mesh = world = mat = 0;
    auto mix = [](u64 h) {
        h ^= h >> 33; h *= 0xff51afd7ed558ccdull;
        h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull;
        h ^= h >> 33; return h;
    };
    for (const Draw& d : drawsPrev_) {
        if (!giVoxelisedDraw(d)) continue;
        ++count;
        mesh += mix(1469598103934665603ull ^ static_cast<u64>(d.depthMesh));
        u64 w = 1469598103934665603ull;
        for (u32 i = 0; i < 16; ++i) {
            u32 bits = 0; std::memcpy(&bits, &d.world[i], sizeof(bits));
            w ^= static_cast<u64>(bits); w *= 1099511628211ull;
        }
        // The mesh is folded into the world axis too: without it, two draws swapping transforms
        // would read as unchanged on this axis and the report would point at the wrong thing.
        w ^= static_cast<u64>(d.depthMesh); w *= 1099511628211ull;
        world += mix(w);
        // The same material hash giDrawsKey() folds in, so the rejection log names the right axis.
        u64 m = 1469598103934665603ull;
        hashDrawMaterialInto(m, d);
        m ^= static_cast<u64>(d.depthMesh); m *= 1099511628211ull;
        mat += mix(m);
    }
}

// True when every input to voxelizePass is identical to the last rebuild's.
bool VoxiRenderer::giSnapshotUnchanged() const {
    // WHICH CHECK REJECTED, said once per reason. Two plausible causes for "never fires" were fixed
    // on reasoning alone and neither was it -- the point at which guessing stops being cheaper than
    // measuring, since the gate compares four independent things and the log said only "no", never
    // which one. The first version latched on the FIRST rejection of any kind, trivially "no snapshot
    // yet" on frame one, hiding every real cause behind it.
    giRebuildCloudOnly_ = false;
    const auto reject = [this](u32 bit, const char* which) {
        if (!(giGateWhyMask_ & (1u << bit))) {
            giGateWhyMask_ |= (1u << bit);
            AVER_INFO("[Voxi] GI rebuild gate rejected on: {}", which);
        }
        return false;
    };
    if (!giSnapValid_) return reject(0, "no snapshot yet (expected once)");
    if (giSnapExtent_ != extent_) return reject(1, "volume extent changed");
    for (u32 i = 0; i < 3; ++i) if (giSnapCenter_[i] != center_[i]) return reject(2, "volume centre changed");
    if (giSnapVoxelSky_ != voxelSkyInjected()) return reject(6, "ReSTIR GI toggled (volume sky in/out)");
    // The whole sky struct, byte for byte -- sunDirection, sunColor, sunIntensity, ground albedo,
    // sky-light intensity all live here and PSVoxel reads every one. Comparing bytes rather than a
    // chosen field list keeps this correct when a field is ADDED to SkyAtmosphere.
    if (!dev_) return false;
    // ZERO-INITIALISED, THEN ASSIGNED: the whole reason this works. skyAtmosphere() returns BY VALUE
    // and SkyAtmosphere opens with a bool followed by padding; memcmp on a raw returned copy compares
    // that unspecified padding, so this gate never once fired (0 skipped of 512 ticks on a scene
    // standing still for two thousand frames). Copy-assigning into a value-initialised object leaves
    // the padding at the zero both sides started from, so only the MEMBERS are compared.
    rhi::SkyAtmosphere now{};
    now = dev_->skyAtmosphere();
    // CLOUDTIME IS A CLOCK, AND IT IS WHY THIS GATE NEVER ONCE PASSED: it counts accumulated seconds,
    // differing every tick by construction, so a byte comparison rebuilt the entire 128^3 volume
    // every giUpdateInterval frames on a static scene forever. Measured: byte 240 of 248, 3.01e-05 on
    // the snapshot against 19.19752 live.
    // Normalised out of BOTH SIDES rather than compared field-by-field, so a field added to
    // SkyAtmosphere later still cannot silently fall outside the check -- only this one named field
    // is excused, here, where the reason is written down.
    rhi::SkyAtmosphere was{};
    was = giSky_;
    const f32 cloudTimeDelta = std::fabs(now.cloudTime - was.cloudTime);
    now.cloudTime = was.cloudTime = 0.0f;

    if (std::memcmp(&now, &was, sizeof(now)) != 0) {
        // WHICH BYTE, not just "something" -- diagnosed twice from a plain "sky/sun changed" and
        // guessed wrong both times; map the offset against rhi::SkyAtmosphere's field order.
        const auto* a = reinterpret_cast<const u8*>(&now);
        const auto* b = reinterpret_cast<const u8*>(&was);
        usize off = 0;
        while (off < sizeof(now) && a[off] == b[off]) ++off;
        f32 fa = 0, fb = 0;
        if (off + sizeof(f32) <= sizeof(now)) {
            std::memcpy(&fa, a + off, sizeof(f32));
            std::memcpy(&fb, b + off, sizeof(f32));
        }
        if (!(giGateWhyMask_ & (1u << 3)))
            AVER_INFO("[Voxi] GI rebuild gate: sky/sun differs first at byte {} of {} "
                      "(as f32: now {} vs snapshot {})", off, sizeof(now), fa, fb);
        return reject(3, "sky/sun changed");
    }

    if (giDrawsKey() != giDrawsKey_) {
        // WHICH AXIS, not just "the draw list". See giDrawsSubKeys.
        u64 c = 0, m = 0, w = 0, mt = 0;
        giDrawsSubKeys(c, m, w, mt);
        // COUNTED OVER THE RUN, NOT LATCHED ON THE FIRST REJECTION. The first version of this
        // reported once per axis and so only ever described LOAD-IN (17 draws growing to 155),
        // which is exactly the phase nobody is asking about. The steady state is the question.
        ++giDrawsRejects_;
        if (c  != giDrawsCount_)    ++giDrawsCountMoved_;
        if (m  != giDrawsMeshKey_)  ++giDrawsMeshMoved_;
        if (w  != giDrawsWorldKey_) ++giDrawsWorldMoved_;
        if (mt != giDrawsMatKey_)   ++giDrawsMatMoved_;
        if (c != giDrawsCount_ && giDrawsDiffReports_ < 3) {
            ++giDrawsDiffReports_;
            std::vector<rhi::MeshHandle> now2;
            for (const Draw& d : drawsPrev_) {
                if (d.translucent) continue;
                if (dev_ && dev_->meshVertexBuffer(d.mesh)) continue;
                now2.push_back(d.depthMesh);
            }
            std::sort(now2.begin(), now2.end());
            std::vector<rhi::MeshHandle> added, removed;
            std::set_difference(now2.begin(), now2.end(), giSnapMeshes_.begin(), giSnapMeshes_.end(),
                                std::back_inserter(added));
            std::set_difference(giSnapMeshes_.begin(), giSnapMeshes_.end(), now2.begin(), now2.end(),
                                std::back_inserter(removed));
            std::string a, r;
            for (rhi::MeshHandle h : added)   { a += std::to_string(h); a += ' '; }
            for (rhi::MeshHandle h : removed) { r += std::to_string(h); r += ' '; }
            AVER_INFO("[Voxi] GI draw-list delta: +{} mesh(es) [{}] -{} mesh(es) [{}]",
                      added.size(), a.empty() ? "-" : a.c_str(), removed.size(), r.empty() ? "-" : r.c_str());

            // WHY A HANDLE LEFT, WHICH IS THE ONE THING THIS REPORT NEVER SAID. Five hypotheses have
            // been refuted by measurement here, every one of them a guess about a mechanism upstream,
            // because naming the handles only ever established THAT the set moved. There are exactly
            // two ways a handle can leave, and they point at completely different code:
            //
            //   it is still in drawsPrev_  -> it was submitted and giVoxelisedDraw rejected it, so the
            //                                 cause is one of that predicate's three tests and is
            //                                 Voxi's own; the reason is named below.
            //   it is gone from drawsPrev_ -> it never reached submitDraw at all, so the cause is
            //                                 upstream in whoever decided not to submit, and no
            //                                 amount of looking at this file will find it.
            //
            // Every previous hypothesis was about the second case. If the log says the first, they
            // were all looking in the wrong file.
            std::string why;
            for (rhi::MeshHandle h : removed) {
                const Draw* found = nullptr;
                for (const Draw& d : drawsPrev_) if (d.depthMesh == h) { found = &d; break; }
                why += std::to_string(h);
                if (!found) { why += "=not-submitted "; continue; }
                if (found->translucent) { why += "=translucent "; continue; }
                if (dev_ && dev_->meshVertexBuffer(found->mesh)) { why += "=skinned "; continue; }
                if (found->boundsRadius >= 0.0f) {
                    const Vec3 volCentre{center_[0], center_[1], center_[2]};
                    const f32 volRadius = (extent_ > 1.0f ? extent_ : 1.0f) * 1.7320508f;
                    const f32 dd = dist(Vec3{found->boundsCentre[0], found->boundsCentre[1],
                                             found->boundsCentre[2]}, volCentre);
                    // The numbers, not just the verdict: a draw sitting a hair outside says the
                    // volume is too tight, one sitting far outside says something moved it.
                    char buf[96];
                    std::snprintf(buf, sizeof buf, "=outside(d %.0f > r %.0f+%.0f) ",
                                  static_cast<f64>(dd), static_cast<f64>(volRadius),
                                  static_cast<f64>(found->boundsRadius));
                    why += buf;
                    continue;
                }
                why += "=submitted-but-unclassified ";
            }
            if (!removed.empty())
                AVER_INFO("[Voxi] GI draw-list departures: {}", why);
        }
        if (giDrawsRejects_ >= giDrawsNextReport_) {
            AVER_INFO("[Voxi] GI draw-list axes over {} rejection(s): count moved {}x, mesh set {}x, "
                      "world transforms {}x, material values {}x  (last count {} vs {})",
                      giDrawsRejects_, giDrawsCountMoved_, giDrawsMeshMoved_, giDrawsWorldMoved_,
                      giDrawsMatMoved_, c, giDrawsCount_);
            giDrawsNextReport_ *= 2;
        }
        return reject(4, "draw list changed");
    }

    // CLOUDS LAST, AND THAT ORDER IS THE POINT. Excusing the cloud clock is not the same as ignoring
    // the clouds: a drifting layer really does change how much sky reaches the ground, so the bake is
    // allowed to go stale by a bounded amount rather than indefinitely. Only when there are clouds to
    // drift -- a clear sky holds its bake for as long as nothing else moves.
    //
    // This used to be tested FIRST, above the geometry checks, which made "clouds drifted" ambiguous:
    // it fired whether or not the draw list had also changed, so it could not be used to identify a
    // rebuild caused by nothing but the sky. Tested last, it means exactly that, which is what lets
    // giCacheScheduleDump below decline to write one to disk.
    //
    // THIRTY SECONDS, NOT TWO. Two seconds forced a full ~18 MB revoxelisation every two seconds for
    // as long as a scene had clouds in it, whether or not anything else in the world moved -- and
    // MEASURED on ElectricDreams under camera motion, that was the dominant source of 439 MB of cache
    // writes in a 900-frame run. Cloud shadow is low-frequency, indirect and diffuse; thirty seconds
    // of staleness in the bounce term is not visible, and fifteen rebuilds a minute is.
    constexpr f32 kGiCloudStaleSeconds = 30.0f;
    if (now.cloudsEnabled && cloudTimeDelta > kGiCloudStaleSeconds) {
        giRebuildCloudOnly_ = true;
        return reject(5, "clouds drifted");
    }
    return true;
}

// Records what the rebuild about to run was computed from.
// THE GI DERIVED-DATA CACHE: the rebuild gate above answers "has anything changed since the last
// bake THIS RUN"; this answers the same question ACROSS runs, so a level looked at before does not
// pay a full revoxelisation to show its first lit frame.
void VoxiRenderer::setGiCacheDir(const std::string& dir) {
    if (dir == giCacheDir_) return;
    giCacheDir_ = dir;
    // A new directory means a new project: whatever was tried against the old one says nothing.
    giCacheTried_ = false;
    giCacheTriedKey_ = fmt::GiCacheKey{};
    giCacheKnownKeys_.clear();
}

// The key describing the volume as it stands after takeGiSnapshot.
// THE SKY IS HASHED THE SAME WAY THE GATE COMPARES IT: byte for byte, minus the cloud clock. A
// cache keyed on a running clock would miss on every load by construction, which is the one failure
// mode that would make the whole thing look like it worked while never hitting.
fmt::GiCacheKey VoxiRenderer::giCacheKey() const {
    fmt::GiCacheKey k;
    k.drawsKey = giDrawsKey_;
    rhi::SkyAtmosphere sky = giSky_;
    sky.cloudTime = 0.0f;
    const u8* p = reinterpret_cast<const u8*>(&sky);
    u64 h = 1469598103934665603ull;
    for (usize i = 0; i < sizeof(sky); ++i) { h ^= p[i]; h *= 1099511628211ull; }
    // Folded into the sky's key because it IS the sky's part of the bake: with ReSTIR GI chosen,
    // PSVoxel leaves the sky out (see voxelSkyInjected), and a cached volume from the other rule would
    // otherwise be restored as if it matched. Only for the sky-less bake, so every key written before
    // this existed -- all of them sky-injected -- still hits.
    if (!giSnapVoxelSky_) { h ^= 0x5Cu; h *= 1099511628211ull; }
    k.skyKey = h;
    for (u32 i = 0; i < 3; ++i) k.centre[i] = giSnapCenter_[i];
    k.extent     = giSnapExtent_;
    k.resolution = voxelResBuilt_;
    k.mipCount   = voxelMips_;
    return k;
}

// Sizes the two staging buffers and works out where each mip sits inside them.
// THE BACKEND'S LAYOUT, NOT THE FILE'S. D3D12 pads every copy row to 256 bytes, so a 16-wide
// RGBA16F mip occupies twice the bytes it carries; Vulkan packs tight. Both are asked rather than
// assumed, and the difference is absorbed here so the FILE is always tightly packed and portable
// between the two.
bool VoxiRenderer::giCacheEnsureBuffers() {
    if (giCacheUnsupported_ || !res_ || !voxelTex_ || voxelMips_ == 0) return false;

    rhi::TextureCopyFootprint fp{};
    if (!res_->textureCopyFootprint(voxelTex_, 0, fp)) {
        // The backend has not implemented the texture<->buffer pair. Said once, then never again:
        // this is a capability gap, not a per-frame error.
        AVER_INFO("[Voxi] GI cache disabled: this backend cannot copy a texture to a buffer");
        giCacheUnsupported_ = true;
        return false;
    }

    u64 total = 0;
    giCacheMipOffsets_.assign(voxelMips_, 0);
    for (u32 m = 0; m < voxelMips_; ++m) {
        rhi::TextureCopyFootprint f{};
        if (!res_->textureCopyFootprint(voxelTex_, m, f)) { giCacheUnsupported_ = true; return false; }
        // 512-aligned per mip: D3D12 wants a placed footprint's offset on a 512-byte boundary, and
        // Vulkan wants 4. Taking the stricter of the two keeps one layout for both.
        total = (total + 511ull) & ~511ull;
        giCacheMipOffsets_[m] = total;
        total += f.totalBytes;
    }
    if (total == 0) return false;
    if (giCacheReadback_ && giCacheBufBytes_ >= total) return true;

    if (giCacheReadback_) { res_->destroyBuffer(giCacheReadback_); giCacheReadback_ = 0; }
    if (giCacheUpload_)   { res_->destroyBuffer(giCacheUpload_);   giCacheUpload_ = 0; }

    rhi::BufferDesc rb;
    rb.bytes = total;
    rb.kind  = rhi::BufferKind::Readback;
    rb.debugName = "gi cache readback";
    giCacheReadback_ = res_->createBuffer(rb);

    rhi::BufferDesc ub;
    ub.bytes = total;
    ub.kind  = rhi::BufferKind::Upload;
    ub.debugName = "gi cache upload";
    giCacheUpload_ = res_->createBuffer(ub);

    if (!giCacheReadback_ || !giCacheUpload_) {
        AVER_WARN("[Voxi] GI cache disabled: could not allocate {} KB of staging", total / 1024);
        giCacheUnsupported_ = true;
        return false;
    }
    giCacheBufBytes_ = total;
    return true;
}

// Tries to fill voxelTex_ from disk. True when the volume now holds the cached answer and the
// caller should skip voxelizePass/filterMips entirely.
bool VoxiRenderer::giCacheRestore(rhi::IRenderContext& ctx) {
    if (giCacheDir_.empty() || giCacheUnsupported_) return false;
    const fmt::GiCacheKey key = giCacheKey();
    if (key.resolution == 0 || key.mipCount == 0) return false;

    // ONCE PER KEY. A miss must not re-open the same absent file on every rebuild -- and rebuilds
    // are exactly the frames already doing the most work.
    if (giCacheTried_ && giCacheTriedKey_ == key) return false;
    giCacheTried_ = true;
    giCacheTriedKey_ = key;

    // THE BUFFER IS PART OF THE CACHE. A bake that has landed in RAM but not yet on disk must be a
    // HIT -- otherwise write-behind turns every within-session revisit into a full revoxelisation,
    // which is the exact cost this cache exists to remove, and the regression would look like the
    // cache had simply stopped working.
    fmt::GiCacheEntry entry;
    bool fromBuffer = false;
    for (const fmt::GiCacheEntry& held : giCachePendingEntries_) {
        if (held.key != key) continue;
        entry = held;
        fromBuffer = true;
        break;
    }

    const std::string path = giCacheDir_ + "\\" + fmt::giCacheFileName(key);
    std::string why;
    if (!fromBuffer && !fmt::loadGiCache(path, entry, &why)) return false;
    // The file name is a hash, so a collision is possible and cheap to rule out: compare the key
    // the entry actually carries.
    if (entry.key != key) return false;
    if (!giCacheEnsureBuffers()) return false;

    // Expand the file's TIGHTLY PACKED mips into the backend's footprint layout.
    for (u32 m = 0; m < voxelMips_; ++m) {
        rhi::TextureCopyFootprint f{};
        if (!res_->textureCopyFootprint(voxelTex_, m, f)) return false;
        const u64 srcBase = fmt::giCacheMipOffset(entry.key, m);
        const u32 tightRow = f.rowBytes;
        if (f.rowPitch == tightRow) {
            res_->writeBuffer(giCacheUpload_, entry.voxels.data() + srcBase,
                              static_cast<u64>(tightRow) * f.rows * f.depth, giCacheMipOffsets_[m]);
        } else {
            // Row by row, because the padding is real: a straight copy would slide each row's data
            // into the previous row's padding.
            for (u32 z = 0; z < f.depth; ++z)
                for (u32 y = 0; y < f.rows; ++y) {
                    const u64 src = srcBase + (static_cast<u64>(z) * f.rows + y) * tightRow;
                    const u64 dst = giCacheMipOffsets_[m] + (static_cast<u64>(z) * f.rows + y) * f.rowPitch;
                    res_->writeBuffer(giCacheUpload_, entry.voxels.data() + src, tightRow, dst);
                }
        }
    }

    ctx.textureBarrier(voxelTex_, rhi::ResourceState::ShaderResource, rhi::ResourceState::CopyDest);
    for (u32 m = 0; m < voxelMips_; ++m)
        ctx.copyBufferToTexture(voxelTex_, m, giCacheUpload_, giCacheMipOffsets_[m]);
    ctx.textureBarrier(voxelTex_, rhi::ResourceState::CopyDest, rhi::ResourceState::ShaderResource);

    AVER_INFO("[Voxi] GI cache HIT: restored a {}^3 volume from {}", key.resolution,
              fmt::giCacheFileName(key));
    // Already cached: the convergence bakes that follow a restore keep this key, and settling them
    // must not copy and rewrite the file just read.
    giCacheRememberKey(key);
    // W3: a restore writes the WHOLE volume (the copyBufferToTexture loop above runs every mip in
    // full), so this tick's box0 covers everything regardless of what any earlier rebuild's bounded
    // dispatch touched. Invalidating the previous box makes the NEXT rebuild start from a full-grid
    // box instead of unioning a stale drawsBox_prev against a volume whose contents that box no
    // longer describes.
    giBoxPrevValid_ = false;
    return true;
}

// A CEILING ON ONE ENTRY, ABSOLUTE AND NOT A FRACTION OF THE BUDGET.
//
// The RAM budget bounds how much is BUFFERED before a flush. It was never a statement that any
// single entry is worth writing, and keying this test on it gets the wrong answer twice over.
// MEASURED, both on PTTest at 512^3 where one volume is 1170 MB:
//   - at the 256 MB default, the entry went over budget the instant it was pushed and flushed
//     synchronously, every bake. 118 ms/frame of wall clock against 12.8 ms of GPU, and 9.4 GB
//     on disk in one session -- giCacheSweep keeps 8, so eight of these is the steady state.
//     Moving the camera in Play re-keys the volume often enough to do it over and over; it
//     reads as a hang, and was reported as a crash.
//   - at a 4096 MB budget it is WORSE, not better: four entries buffer to 4681 MB of RAM and
//     then write 4.8 GB in a single flush.
// A bigger budget buys a bigger stall. There is no setting of it that makes a gigabyte-per-bake
// write reasonable, which is what makes this a ceiling rather than a ratio.
//
// 256 MB, because that is the shipped default budget: an entry that cannot fit the cache as it
// ships is not one this system was built to carry. Above that line the derived data costs more
// to move than to derive -- the revoxelisation being avoided is ~8 ms of GPU, and the write is
// seconds of disk -- so the entry is dropped and the volume is simply rebuilt, which is the
// cache's own stated contract: losing it costs one rebuild.
//
// FILE SCOPE, not a local in giCacheTick, because giCacheScheduleDump has to consult it BEFORE
// issuing the readback -- see the note there on what testing it too late used to cost.
static constexpr u64 kMaxCachedGiEntryBytes = 256ull * 1024ull * 1024ull;

// SAID ONCE, and it names the lever that actually helps. Raising the budget is NOT that lever,
// which is why it is not suggested: it makes the stall larger.
void VoxiRenderer::giCacheWarnOversize(u64 bytes) {
    if (giCacheOversizeWarned_) return;
    giCacheOversizeWarned_ = true;
    AVER_WARN("[Voxi] a {}^3 GI volume is {} MB, over the {} MB per-entry ceiling, so it is "
              "NOT cached and every open re-voxelises instead -- which costs about one frame. "
              "Writing it would block for seconds per bake and keep up to 8 copies on disk. "
              "Lower RENDER.VOXELRES if you want the cache back; raising the cache budget "
              "does not help, it only makes the flush bigger.",
              voxelResBuilt_, bytes / (1024 * 1024), kMaxCachedGiEntryBytes / (1024 * 1024));
}

bool VoxiRenderer::giCacheKeyKnown(const fmt::GiCacheKey& k) const {
    for (const fmt::GiCacheKey& known : giCacheKnownKeys_)
        if (known == k) return true;
    return false;
}

void VoxiRenderer::giCacheRememberKey(const fmt::GiCacheKey& k) {
    if (giCacheKeyKnown(k)) return;
    constexpr usize kKnownKeys = 64;   // giCacheFlush's kGiCacheKeepFiles: the directory keeps no more
    if (giCacheKnownKeys_.size() >= kKnownKeys) giCacheKnownKeys_.erase(giCacheKnownKeys_.begin());
    giCacheKnownKeys_.push_back(k);
}

// Schedules a readback of the SETTLED volume (called from the gate's skip branch, see
// giCacheSettlePending_). Nothing is written yet -- see the countdown.
bool VoxiRenderer::giCacheScheduleDump(rhi::IRenderContext& ctx) {
    if (giCacheDir_.empty() || giCacheUnsupported_) return true;
    if (giCacheDumpCountdown_) return false;   // one in flight is enough; ask again next quiet tick

    // A CLOUD-ONLY REBUILD IS NOT WORTH A FILE, and this is the sharpest instance of the cache
    // writing something it can never retrieve. giCacheKey deliberately zeroes cloudTime -- a key on a
    // running clock would miss on every load by construction -- so a bake whose ONLY input change was
    // the cloud clock carries a key identical to the one already on disk while holding different
    // voxels. Writing it spends ~18 MB to make the cache non-deterministic: the same key would name
    // two different volumes depending on which run wrote last.
    if (giCacheSettleCloudOnly_) return true;

    giCachePendingKey_ = giCacheKey();
    if (giCachePendingKey_.resolution == 0) return true;
    // Restored from, or already queued to, the cache this session: nothing new to write.
    if (giCacheKeyKnown(giCachePendingKey_)) return true;

    // THE CEILING IS TESTED HERE, BEFORE ANYTHING IS COPIED, AND IT USED TO BE TESTED ONLY IN
    // giCacheTick -- after the copy had already run. That ordering made the "not cached" path the
    // MOST expensive one in the renderer rather than a free early-out: at 512^3 every rebuild issued
    // a 1170 MB GPU->CPU copy of all nine mips, allocated a 1170 MB readback buffer to hold it and a
    // 1170 MB std::vector to receive it, and then dropped the result on the size test below. The
    // volume is only written when the entry FITS, so nothing above that line was ever going to be
    // kept -- the whole readback was work done to reach a `return`.
    //
    // MEASURED, PTTest/Sponza with --cam-wobble: the gate rejects on ~72% of ticks while the camera
    // moves, so this ran about a hundred times in 151 frames. It is unmarked GPU work inside the
    // "Voxi GI update" scope, which is why that scope's EXCLUSIVE time read 10.79 ms moving against
    // 0.66 ms still -- a number the comment at the top of prePass attributed to
    // beginShadowHistory/endShadowHistory, neither of which has any camera-dependent cost at all.
    //
    // NOT A BEHAVIOUR CHANGE: an over-ceiling volume was never cached before this and still is not.
    // The only difference is that it now costs nothing to not cache it.
    if (fmt::giCacheTotalBytes(giCachePendingKey_) > kMaxCachedGiEntryBytes) {
        giCacheWarnOversize(fmt::giCacheTotalBytes(giCachePendingKey_));
        return true;
    }

    if (!giCacheEnsureBuffers()) return true;

    // filterMips left the whole resource in ShaderResource; put it back there afterwards so the
    // cone trace later this frame reads it exactly as it would have.
    ctx.textureBarrier(voxelTex_, rhi::ResourceState::ShaderResource, rhi::ResourceState::CopySource);
    for (u32 m = 0; m < voxelMips_; ++m)
        ctx.copyTextureToBuffer(giCacheReadback_, giCacheMipOffsets_[m], voxelTex_, m);
    ctx.textureBarrier(voxelTex_, rhi::ResourceState::CopySource, rhi::ResourceState::ShaderResource);

    giCacheDumpCountdown_ = kGiCacheReadbackDelay;
    giCacheRememberKey(giCachePendingKey_);
    return true;
}

// Ticks the countdown and writes the file when the GPU is provably past the copy.
void VoxiRenderer::giCacheTick() {
    if (!giCacheDumpCountdown_) return;
    if (--giCacheDumpCountdown_) return;
    if (!res_ || !giCacheReadback_) return;

    // Compact the backend's footprint layout back into the tightly packed file layout.
    fmt::GiCacheEntry entry;
    entry.key = giCachePendingKey_;
    entry.voxels.resize(static_cast<usize>(fmt::giCacheTotalBytes(entry.key)));

    for (u32 m = 0; m < entry.key.mipCount && m < giCacheMipOffsets_.size(); ++m) {
        rhi::TextureCopyFootprint f{};
        if (!res_->textureCopyFootprint(voxelTex_, m, f)) return;
        const u64 dstBase = fmt::giCacheMipOffset(entry.key, m);
        const u32 tightRow = f.rowBytes;
        if (f.rowPitch == tightRow) {
            if (!res_->readBuffer(giCacheReadback_, entry.voxels.data() + dstBase,
                                  static_cast<u64>(tightRow) * f.rows * f.depth, giCacheMipOffsets_[m]))
                return;
        } else {
            for (u32 z = 0; z < f.depth; ++z)
                for (u32 y = 0; y < f.rows; ++y) {
                    const u64 dst = dstBase + (static_cast<u64>(z) * f.rows + y) * tightRow;
                    const u64 src = giCacheMipOffsets_[m] + (static_cast<u64>(z) * f.rows + y) * f.rowPitch;
                    if (!res_->readBuffer(giCacheReadback_, entry.voxels.data() + dst, tightRow, src))
                        return;
                }
        }
    }

    // BUFFERED, NOT WRITTEN. See giCachePendingEntries_ for why this is safe: the cache is derived
    // data whose stated contract is that losing it costs one rebuild.
    //
    // REPLACES an entry with the same key rather than accumulating duplicates -- re-baking the same
    // inputs is exactly what happens when an author moves the sun back to where it was.
    const u64 bytes = static_cast<u64>(entry.voxels.size());

    // BELT AND BRACES. giCacheScheduleDump refuses to even issue the readback for an entry over
    // kMaxCachedGiEntryBytes, so reaching here with an oversize entry means the key changed between
    // scheduling the copy and reading it back -- possible in principle if the volume were resized
    // mid-flight. Keep the test: it costs one comparison, and without it that case would write a
    // gigabyte file the ceiling exists to prevent.
    if (bytes > kMaxCachedGiEntryBytes) {
        giCacheWarnOversize(bytes);
        return;
    }

    bool replaced = false;
    for (fmt::GiCacheEntry& held : giCachePendingEntries_) {
        if (held.key != entry.key) continue;
        giCachePendingBytes_ -= static_cast<u64>(held.voxels.size());
        held = std::move(entry);
        giCachePendingBytes_ += bytes;
        replaced = true;
        break;
    }
    if (!replaced) {
        giCachePendingBytes_ += bytes;
        giCachePendingEntries_.push_back(std::move(entry));
    }

    if (giCachePendingBytes_ > giCacheRamBudget_) {
        AVER_INFO("[Voxi] GI cache buffer over budget ({} MB of {} MB) -- flushing",
                  giCachePendingBytes_ / (1024 * 1024), giCacheRamBudget_ / (1024 * 1024));
        giCacheFlush();
    }
}

// Lowering the budget below what is already held flushes now, rather than leaving the buffer over
// its own limit until whenever the next bake happens to land.
void VoxiRenderer::setGiCacheRamBudget(u64 bytes) {
    giCacheRamBudget_ = bytes;
    if (giCachePendingBytes_ > giCacheRamBudget_) giCacheFlush();
}

// Writes everything held, sweeps ONCE, and empties the buffer.
//
// ONE SWEEP FOR THE WHOLE BATCH rather than one per file: the sweep stats every entry in the
// directory to order it by age, so doing it per write made a ten-bake session do ten directory
// walks to reach the same end state.
u32 VoxiRenderer::giCacheFlush() {
    if (giCachePendingEntries_.empty()) return 0;
    if (giCacheDir_.empty()) {
        // No project, nowhere to put it. Drop the buffer rather than growing it forever.
        giCachePendingEntries_.clear();
        giCachePendingBytes_ = 0;
        return 0;
    }

    u32 wrote = 0;
    u64 wroteBytes = 0;
    for (const fmt::GiCacheEntry& entry : giCachePendingEntries_) {
        const std::string path = giCacheDir_ + "\\" + fmt::giCacheFileName(entry.key);
        std::string why;
        if (!fmt::saveGiCache(path, entry, &why)) {
            AVER_WARN("[Voxi] GI cache could not be written: {}", why);
            continue;
        }
        ++wrote;
        wroteBytes += static_cast<u64>(entry.voxels.size());
    }
    giCachePendingEntries_.clear();
    giCachePendingBytes_ = 0;

    // Bounded, because nothing else bounds it: every distinct bake writes a new file and none is
    // ever overwritten.
    // RAISED FROM 8, AND NOW BOUNDED BY BYTES TOO. Eight was below the working set: MEASURED on
    // ElectricDreams under camera motion, one 900-frame run touched 17 distinct volumes, so the
    // sweep was throwing away entries that would be asked for again shortly. The byte cap is what
    // makes raising the count safe -- 64 entries is ~1.1 GB at 128^3 but would have been 16 GB at
    // the largest entry the writer accepts, and a count alone cannot tell those apart.
    constexpr u32 kGiCacheKeepFiles = 64;
    constexpr u64 kGiCacheKeepBytes = 1536ull * 1024ull * 1024ull;   // 1.5 GB of derived data
    const u32 swept = wrote ? fmt::giCacheSweep(giCacheDir_, kGiCacheKeepFiles, kGiCacheKeepBytes) : 0;
    if (wrote)
        AVER_INFO("[Voxi] GI cache FLUSHED {} entr(ies), {} KB{}", wrote, wroteBytes / 1024,
                  swept ? (", swept " + std::to_string(swept) + " older entr(ies)") : std::string());
    return wrote;
}

void VoxiRenderer::takeGiSnapshot() {
    giDrawsSubKeys(giDrawsCount_, giDrawsMeshKey_, giDrawsWorldKey_, giDrawsMatKey_);
    // The mesh multiset itself, so giSnapshotUnchanged can say WHICH draws came and went rather
    // than only that the count moved. FIVE hypotheses have now been refuted by measurement here --
    // frustum culling, submission order, occlusion bucketing, and most recently the GPU per-cluster
    // path (which does skip drawMesh(), the only route to submitDraw, so it LOOKED decisive:
    // --no-lod-mesh-shader under a six-degree wobble reproduces the axis census digit for digit,
    // 269 vs 267 and 269 vs 274, so it is not that either).
    //
    // WHAT THE CENSUS ACTUALLY SAYS, and it is worth writing down because every guess so far has
    // ignored it: all four axes move on EVERY rejection and the count moves with them. Transforms
    // and materials are not drifting -- the SET is changing, by a handful of meshes out of ~269, in
    // consecutive runs of handles that look like one source mesh's split parts. The next step is not
    // a sixth hypothesis: it is to report WHY a handle left, from the site that decided it, since
    // this end of the pipe can only ever say that it did.
    giSnapMeshes_.clear();
    for (const Draw& d : drawsPrev_) {
        if (!giVoxelisedDraw(d)) continue;
        giSnapMeshes_.push_back(d.depthMesh);
    }
    std::sort(giSnapMeshes_.begin(), giSnapMeshes_.end());
    giDrawsKey_ = giDrawsKey();
    // Same two-step on the stored side, so both sides of the memcmp have zero padding.
    if (dev_) { giSky_ = rhi::SkyAtmosphere{}; giSky_ = dev_->skyAtmosphere(); }
    for (u32 i = 0; i < 3; ++i) giSnapCenter_[i] = center_[i];
    giSnapExtent_ = extent_;
    giSnapVoxelSky_ = voxelSkyInjected();
    giSnapValid_ = true;
}

u32 VoxiRenderer::fitCascades() {
    f32 invViewProjRel[16] = {};
    f32 camPos[3] = {};
    // The forward matrix is captured too, into curViewProj_ -- not used here, but this is where the
    // camera is already being read, and endShadowHistory needs THIS frame's viewProj to become next
    // frame's reprojection source.
    if (!dev_ || !dev_->camera(curViewProj_, invViewProjRel, camPos)) return 0;

    Mat4 invVPRel;
    std::memcpy(&invVPRel.m[0][0], invViewProjRel, sizeof(invViewProjRel));
    const Vec3 eye{camPos[0], camPos[1], camPos[2]};

    // The frustum's eight corners AS OFFSETS FROM THE EYE (the camera-relative inverse, see
    // rhi::PerFrameCB::invViewProjRel), and the whole fit stays relative until the centre. Absolute
    // corners minus the eye would recover the ~2 cm near-plane offset from two |eye|-sized values --
    // about 1% error at cloud altitude (f32 ulp 0.0156 cm at 2e5 cm), changing every frame the eye
    // moves -- and every split distance below scales off camNear. D3D depth is [0,1].
    Vec3 nearR[4], farR[4];
    const f32 nx[4] = {-1, 1, 1, -1};
    const f32 ny[4] = {-1, -1, 1, 1};
    for (int i = 0; i < 4; ++i) {
        nearR[i] = xformProjected(Vec3{nx[i], ny[i], 0.0f}, invVPRel);
        farR[i]  = xformProjected(Vec3{nx[i], ny[i], 1.0f}, invVPRel);
    }

    // View depth at each plane, measured along the view axis rather than radially.
    Vec3 fwd = (farR[0] + farR[1] + farR[2] + farR[3]) * 0.25f;
    fwd = fwd.getSafeNormal();
    const f32 camNear = dot(nearR[0], fwd);
    const f32 camFar  = dot(farR[0], fwd);
    if (!(camFar > camNear + 1e-3f)) return 0;

    const f32 zNear = camNear;
    const f32 reach = camNear * kShadowRangeFromNear;
    const f32 zFar  = camFar < reach ? camFar : reach;
    if (!(zFar > zNear)) return 0;

    Vec3 dir = Vec3{sunDir_[0], sunDir_[1], sunDir_[2]}.getSafeNormal();
    if (dir.sizeSquared() < 0.5f) {
        const rhi::SkyAtmosphere def{};
        dir = Vec3{def.sunDirection[0], def.sunDirection[1], def.sunDirection[2]}.getSafeNormal();
    }
    const Vec3 up = std::fabs(dir.z) > 0.95f ? Vec3{1, 0, 0} : Vec3{0, 0, 1};

    f32 sliceNear = zNear;
    for (u32 c = 0; c < kShadowCascades; ++c) {
        const f32 p = static_cast<f32>(c + 1) / static_cast<f32>(kShadowCascades);
        const f32 logSplit = zNear * std::pow(zFar / zNear, p);
        const f32 uniSplit = zNear + (zFar - zNear) * p;
        f32 sliceFar = kCascadeSplitLambda * logSplit + (1.0f - kCascadeSplitLambda) * uniSplit;

        // The corners of this slice, interpolated along the frustum's own edges -- still relative to
        // the eye.
        const f32 tN = (sliceNear - camNear) / (camFar - camNear);
        const f32 tF = (sliceFar  - camNear) / (camFar - camNear);
        Vec3 corner[8];
        for (int i = 0; i < 4; ++i) {
            corner[i]     = nearR[i] + (farR[i] - nearR[i]) * tN;
            corner[i + 4] = nearR[i] + (farR[i] - nearR[i]) * tF;
        }

        Vec3 centreRel{0, 0, 0};
        for (const Vec3& v : corner) centreRel = centreRel + v;
        centreRel = centreRel * 0.125f;
        f32 radius = 0.0f;
        for (const Vec3& v : corner) {
            const f32 d = (v - centreRel).size();
            if (d > radius) radius = d;
        }
        radius = std::ceil(radius * 16.0f) / 16.0f;

        // Absolute once, for the light-space matrix, the texel snap and the per-draw cull below.
        const Vec3 centre = eye + centreRel;

        // NO UNION WITH THE GI VOLUME HERE ANY MORE -- see kGiShadowSize's comment for the measured
        // cost of the old union. giShadowPass/fitGiShadow answer the volume separately now; every
        // cascade here is fitted to the camera and nothing else.

        // Persisted for shadowPass's per-draw cull: the same (centre, radius) this cascade's own
        // frustum and cb_.cascadeSplit are built from, before the sub-texel nudge below.
        cascadeCentre_[c][0] = centre.x; cascadeCentre_[c][1] = centre.y; cascadeCentre_[c][2] = centre.z;
        cascadeRadius_[c] = radius;

        // Snap the centre to a whole texel of this cascade, in light space.
        const f32 texel = 2.0f * radius / static_cast<f32>(kShadowCascadeSize);
        Mat4 view = Mat4::lookAtLH(centre + dir * (radius * 2.0f), centre, up);
        Vec3 cLs = xformPoint(centre, view);
        cLs.x = std::floor(cLs.x / texel) * texel;
        cLs.y = std::floor(cLs.y / texel) * texel;
        const Vec3 snapped = xformPoint(cLs, view.inverse());
        view = Mat4::lookAtLH(snapped + dir * (radius * 2.0f), snapped, up);

        Mat4 proj;   // orthographic, row-vector, depth [0,1]
        proj.m[0][0] = 1.0f / radius;
        proj.m[1][1] = 1.0f / radius;
        proj.m[2][2] = 1.0f / (radius * 4.0f);
        proj.m[3][2] = 0.0f;
        proj.m[3][3] = 1.0f;
        const Mat4 lvp = view * proj;
        std::memcpy(cb_.cascadeViewProj[c], &lvp.m[0][0], sizeof(lvp.m));

        // Radial, because the cascades are fitted to bounding spheres.
        cb_.cascadeSplit[c][0] = centreRel.size() + radius;
        cb_.cascadeSplit[c][1] = texel * 1.5f;   // normal-offset bias, world units
        cb_.cascadeSplit[c][2] = 0.0f;
        cb_.cascadeSplit[c][3] = 0.0f;

        sliceNear = sliceFar;
    }
    return kShadowCascades;
}

// Fits the GI-only shadow map to the GI VOLUME, and writes its matrix and cull sphere.
// This is the half of the old cascade union that was actually needed, extracted so it stops
// distorting the camera cascades. Where fitCascades slices the view frustum and depends on where
// the camera looks, this depends on nothing but setVolume(): the same box every frame the volume
// does not move, which is also why it can be rebuilt on giUpdateInterval's cadence instead of
// every frame.
void VoxiRenderer::fitGiShadow() {
    // The volume's circumsphere -- the same expression the deleted union used, so the box covers
    // exactly what it always covered.
    const Vec3 centre{center_[0], center_[1], center_[2]};
    const f32 radius = (extent_ > 1.0f ? extent_ : 1.0f) * 1.7320508f;

    giShadowCentre_[0] = centre.x; giShadowCentre_[1] = centre.y; giShadowCentre_[2] = centre.z;
    giShadowRadius_ = radius;

    Vec3 dir = Vec3{sunDir_[0], sunDir_[1], sunDir_[2]}.getSafeNormal();
    if (dir.sizeSquared() < 0.5f) {
        const rhi::SkyAtmosphere def{};
        dir = Vec3{def.sunDirection[0], def.sunDirection[1], def.sunDirection[2]}.getSafeNormal();
    }
    const Vec3 up = std::fabs(dir.z) > 0.95f ? Vec3{1, 0, 0} : Vec3{0, 0, 1};

    // Snapped to a whole texel in light space, exactly as the cascades are. The volume does not
    // move with the camera, so this matters less here than it does there -- but the sun DOES move,
    // and without the snap a slow sun turn makes every injected voxel's occlusion crawl.
    const f32 texel = 2.0f * radius / static_cast<f32>(kGiShadowSize);
    Mat4 view = Mat4::lookAtLH(centre + dir * (radius * 2.0f), centre, up);
    Vec3 cLs = xformPoint(centre, view);
    cLs.x = std::floor(cLs.x / texel) * texel;
    cLs.y = std::floor(cLs.y / texel) * texel;
    const Vec3 snapped = xformPoint(cLs, view.inverse());
    view = Mat4::lookAtLH(snapped + dir * (radius * 2.0f), snapped, up);

    Mat4 proj;   // orthographic, row-vector, depth [0,1] -- same form fitCascades builds
    proj.m[0][0] = 1.0f / radius;
    proj.m[1][1] = 1.0f / radius;
    proj.m[2][2] = 1.0f / (radius * 4.0f);
    proj.m[3][2] = 0.0f;
    proj.m[3][3] = 1.0f;
    const Mat4 lvp = view * proj;
    std::memcpy(cb_.giShadowViewProj, &lvp.m[0][0], sizeof(lvp.m));

    cb_.giShadowParams[0] = 1.0f / static_cast<f32>(kGiShadowSize);
    cb_.giShadowParams[1] = 1.0f;          // usable; giShadowPass zeroes it when it cannot run
    cb_.giShadowParams[2] = texel * 1.5f;  // normal-offset bias, world units
    // [3] is NOT written here: it carries the staged ray-driven bit-field toggles, packed once per
    // frame in prePass (see that assignment's own comment) -- this function only runs on the GI
    // rebuild gate's cadence, and clobbering [3] here used to zero those bits out on every rebuild.
}

// Concatenates every referenced mesh's vertices and indices into two flat buffers, and writes the
// per-instance table that says where each mesh's data starts.
// REBUILT ONLY WHEN THE MESH SET CHANGES. Copying every mesh every frame would cost more than the
// reflections it enables, and the geometry itself does not move -- an instance's TRANSFORM changes
// per frame and lives in the instance record, which is rewritten every frame because it is small.
bool VoxiRenderer::buildGeometryTable(rhi::IRenderContext& ctx) {
    if (!res_ || !dev_ || rtInstanceData_.empty()) return false;

    // ONE ENTRY PER DISTINCT MESH, NOT PER INSTANCE -- the whole cost of this function.
    // Used to walk rtInstanceMesh_ and give every INSTANCE its own slice of the shared vertex/index
    // buffers, copying identical geometry once per instance: ~522 MB of vertex buffer for ~120
    // distinct meshes at 769 instances (16.3M vertices, 11.5M indices, two copyBuffer calls each) in
    // ElectricDreams, scaling to ~3.5 GB / 13,310 copies at 6,655 instances -- "Voxi acceleration
    // structures" cost 9.1ms at 759 instances and 64.2ms at 6,571, ~9.7us/instance at both counts, the
    // signature of a linear per-instance cost rather than a TLAS build.
    // Geometry is a property of the MESH: two instances of the same mesh index the same triangles,
    // only their transforms differ (already per-instance in RtInstance::objectToWorld), so the table
    // is built over the distinct set instead, each instance pointing at its mesh's slice.
    // SORTED, not first-appearance order: offsets from first-appearance order change whenever the
    // draw list is REORDERED -- which happens every frame here (occlusionOrder_) -- rebuilding the
    // table constantly while naming the same meshes. Sorting makes offsets a function of the SET alone.
    rtGeomMeshes_.assign(rtInstanceMesh_.begin(), rtInstanceMesh_.end());
    std::sort(rtGeomMeshes_.begin(), rtGeomMeshes_.end());
    rtGeomMeshes_.erase(std::unique(rtGeomMeshes_.begin(), rtGeomMeshes_.end()), rtGeomMeshes_.end());

    u64 key = 1469598103934665603ull;
    for (rhi::MeshHandle h : rtGeomMeshes_) {
        key ^= static_cast<u64>(h);
        key *= 1099511628211ull;
    }

    rtGeomFirstVertex_.clear();
    rtGeomFirstIndex_.clear();
    rtGeomCopiesVerts_.clear();
    rtGeomVertSlice_.clear();
    rtGeomFirstVertex_.reserve(rtGeomMeshes_.size());
    rtGeomFirstIndex_.reserve(rtGeomMeshes_.size());
    rtGeomCopiesVerts_.reserve(rtGeomMeshes_.size());
    u32 totalVerts = 0, totalIndices = 0;
    for (rhi::MeshHandle h : rtGeomMeshes_) {
        rhi::BufferHandle vb = 0, ib = 0;
        u32 vc = 0, ic = 0;
        if (!dev_->meshGeometry(h, &vb, &ib, &vc, &ic)) return false;
        // SHARED VERTICES, ONE SLICE. createMeshSharingVertices (LODs) and createPosedPartMesh (a
        // skinned mesh's per-material parts) hand out handles that name the SAME vertex buffer at
        // the SAME count with indices in its numbering, so the root's slice is byte-for-byte what a
        // copy would hold. A two-material character used to put its whole posed buffer in twice.
        const u64 sliceKey = (static_cast<u64>(vb) << 32) | vc;   // both u32: exact, no collisions
        const auto [sit, fresh] = rtGeomVertSlice_.try_emplace(sliceKey, totalVerts);
        rtGeomFirstVertex_.push_back(sit->second);
        rtGeomFirstIndex_.push_back(totalIndices);
        rtGeomCopiesVerts_.push_back(fresh ? 1u : 0u);
        if (fresh) totalVerts += vc;
        totalIndices += ic;
    }

    // Every instance points at its mesh's slice. Rewritten every frame, because the INSTANCE list
    // changes every frame even when the mesh set does not -- which is exactly why this half is
    // outside the key early-out below and the copy half is inside it.
    for (usize i = 0; i < rtInstanceMesh_.size(); ++i) {
        const auto it = std::lower_bound(rtGeomMeshes_.begin(), rtGeomMeshes_.end(), rtInstanceMesh_[i]);
        if (it == rtGeomMeshes_.end() || *it != rtInstanceMesh_[i]) return false;
        const usize slot = static_cast<usize>(it - rtGeomMeshes_.begin());
        rtInstanceData_[i].firstVertex = rtGeomFirstVertex_[slot];
        rtInstanceData_[i].firstIndex  = rtGeomFirstIndex_[slot];
    }
    if (totalVerts == 0 || totalIndices == 0) return false;

    // The instance table is rewritten every frame -- transforms move -- so it is an upload buffer.
    // Grown as a set: every slot in the ring has to hold the largest table, or rotating onto a
    // smaller one next frame would truncate it.
    if (rtInstanceCapacity_ < rtInstanceData_.size()) {
        for (u32 i = 0; i < kRtInstanceRing; ++i) {
            if (rtInstances_[i]) res_->destroyBuffer(rtInstances_[i]);
            rhi::BufferDesc d;
            d.bytes = sizeof(RtInstance) * rtInstanceData_.size();
            d.kind  = rhi::BufferKind::Upload;
            d.debugName = "rt instances";
            rtInstances_[i] = res_->createBuffer(d);
            if (!rtInstances_[i]) { rtInstanceCapacity_ = 0; return false; }
        }
        rtInstanceCapacity_ = static_cast<u32>(rtInstanceData_.size());
    }
    // Rotate BEFORE writing, so this frame never touches the buffer the previous one bound.
    rtInstanceSlot_ = (rtInstanceSlot_ + 1) % kRtInstanceRing;
    const rhi::BufferHandle inst = rtInstances_[rtInstanceSlot_];
    if (!inst) return false;
    res_->writeBuffer(inst, rtInstanceData_.data(),
                      sizeof(RtInstance) * rtInstanceData_.size(), 0);
    res_->setSrvBuffer(bindings_, 5, inst, sizeof(RtInstance),
                       static_cast<u32>(rtInstanceData_.size()), 0);

    if (key == rtGeometryKey_ && rtGeometryReady_) return true;

    // The geometry itself. Default-heap, because it is written once by copy and then read by every
    // reflection ray for as long as the mesh set holds.
    if (rtVertCapacity_ < totalVerts) {
        if (rtVerts_) res_->destroyBuffer(rtVerts_);
        rhi::BufferDesc d;
        d.bytes = static_cast<u64>(totalVerts) * sizeof(rhi::MeshVertex);
        d.kind  = rhi::BufferKind::Default;
        d.debugName = "rt vertices";
        rtVerts_ = res_->createBuffer(d);
        rtVertCapacity_ = rtVerts_ ? totalVerts : 0;
    }
    if (rtIndexCapacity_ < totalIndices) {
        if (rtIndices_) res_->destroyBuffer(rtIndices_);
        rhi::BufferDesc d;
        d.bytes = static_cast<u64>(totalIndices) * sizeof(u32);
        d.kind  = rhi::BufferKind::Default;
        d.debugName = "rt indices";
        rtIndices_ = res_->createBuffer(d);
        rtIndexCapacity_ = rtIndices_ ? totalIndices : 0;
    }
    if (!rtVerts_ || !rtIndices_) return false;

    // BOTH transitions are explicit. D3D12 would promote a Common buffer to CopyDest implicitly, but
    // the RHI tracks buffer state to catch exactly this mistake and doesn't model promotion -- an
    // implicit promotion with an explicit walk-back would claim a state the tracker never saw it enter.
    // The walk-back also matters on its own: promotion lasts the rest of the command list, so a
    // reflection ray reading this later would see a resource the runtime still considers a copy dest.
    ctx.bufferBarrier(rtVerts_,   rhi::ResourceState::Common, rhi::ResourceState::CopyDest);
    ctx.bufferBarrier(rtIndices_, rhi::ResourceState::Common, rhi::ResourceState::CopyDest);
    // TWO COPIES PER DISTINCT MESH, where this used to issue two per INSTANCE -- one, for a mesh
    // whose vertex slice another handle's copy already fills.
    u32 distinctSlices = 0;
    for (usize m = 0; m < rtGeomMeshes_.size(); ++m) {
        rhi::BufferHandle vb = 0, ib = 0;
        u32 vc = 0, ic = 0;
        if (!dev_->meshGeometry(rtGeomMeshes_[m], &vb, &ib, &vc, &ic)) return false;
        if (rtGeomCopiesVerts_[m]) {
            ctx.copyBuffer(rtVerts_, vb, static_cast<u64>(vc) * sizeof(rhi::MeshVertex),
                           static_cast<u64>(rtGeomFirstVertex_[m]) * sizeof(rhi::MeshVertex), 0);
            ++distinctSlices;
        }
        ctx.copyBuffer(rtIndices_, ib, static_cast<u64>(ic) * sizeof(u32),
                       static_cast<u64>(rtGeomFirstIndex_[m]) * sizeof(u32), 0);
    }
    ctx.bufferBarrier(rtVerts_,   rhi::ResourceState::CopyDest, rhi::ResourceState::Common);
    ctx.bufferBarrier(rtIndices_, rhi::ResourceState::CopyDest, rhi::ResourceState::Common);

    rtGeometryKey_ = key;
    rtGeometryReady_ = true;
    res_->setSrvBuffer(bindings_, 3, rtVerts_, sizeof(rhi::MeshVertex), totalVerts, 0);
    res_->setSrvBuffer(bindings_, 4, rtIndices_, sizeof(u32), totalIndices, 0);
    AVER_INFO("[Voxi] ray-traced reflection table: {} instances over {} distinct mesh(es) "
              "({} vertex slice(s)), {} vertices, {} indices",
              rtInstanceData_.size(), rtGeomMeshes_.size(), distinctSlices, totalVerts, totalIndices);
    return true;
}

// Builds this build's dense material table from rtInstanceMatKey_/matConstantsByKey (both filled by
// buildAccelerationStructures' per-draw loop, immediately before this is called), re-uploads it only
// when content actually changed, and writes the final index into every rtInstanceData_[i].materialIndex.
// SORTED BY KEY, NOT FIRST-SEEN ORDER -- same reasoning as rtGeomMeshes_: occlusionOrder_ reorders the
// draw list every frame regardless of whether the visible material SET changed, so a first-seen-order
// index would reshuffle (and re-upload, via the memcmp below) almost every frame. Sorting makes the
// table a function of the SET alone.
// MaterialSystem exposes no dirty count or generation number a caller can read, so this compares the
// actual bytes instead -- exact, and cheap at the "tens, not thousands" of resident materials
// MaterialSystem.hpp documents.
// Makes one texture resident in the ray path's bindless table and returns its index, or
// pbr::kUnboundTexture if it could not be made resident.
// APPEND-ONLY AND MEMOISED: the same texture asked for twice returns the same index, sizing the table
// by distinct IMAGES rather than materials (forty materials sharing one albedo occupy one slot).
// Nothing is ever freed -- see the table's own declaration comment for why that's safe for a session.
u32 VoxiRenderer::residentTexture(rhi::TextureHandle h) {
    if (!h || !rtTexTable_) return pbr::kUnboundTexture;
    if (const auto it = rtTexIndex_.find(h); it != rtTexIndex_.end()) return it->second;
    if (rtTexNext_ >= kRtTextureCapacity) {
        // Said ONCE. A scene past the ceiling would otherwise log per material per build, which
        // buries the one line that matters under thousands of copies of itself.
        if (!rtTexLogged_) {
            rtTexLogged_ = true;
            AVER_WARN("[Voxi] ray-traced texture table full at {} distinct textures; further "
                      "materials keep their factor colour instead of sampling. Raise "
                      "kRtTextureCapacity if a real project needs more.", kRtTextureCapacity);
        }
        return pbr::kUnboundTexture;
    }
    const u32 index = rtTexNext_;
    // THE RHI IS ASKED, NOT TOLD. setBindlessTexture refuses an out-of-range index rather than
    // writing past the table, and a refusal must leave the slot unbound rather than advancing the
    // cursor over a descriptor that was never written.
    if (!res_->setBindlessTexture(rtTexTable_, index, h)) return pbr::kUnboundTexture;
    ++rtTexNext_;
    rtTexIndex_.emplace(h, index);
    return index;
}

// Creates the bindless texture table on first need. Once: a failure is remembered so a device that
// cannot provide one is not asked again every single build.
void VoxiRenderer::ensureTextureTable() {
    if (rtTexTableTried_ || !res_) return;
    rtTexTableTried_ = true;
    if (!caps_.rtBindlessTextures) return;   // silent: the caps line already said so once
    rtTexTable_ = res_->createBindlessTextureTable(kRtTextureCapacity);
    if (!rtTexTable_)
        AVER_WARN("[Voxi] no bindless texture table; ray hits will shade from material factors "
                  "alone, as they did before textured ray hits existed");
}

bool VoxiRenderer::buildMaterialTable(const std::unordered_map<u64, pbr::MaterialConstants>& matConstantsByKey) {
    // Distinct keys, sorted -- see this function's own header comment for why sorted rather than
    // insertion order. std::map would give this for free, but the table is rebuilt from an
    // unordered_map already built by the caller for O(1) lookups; sorting the (few, "tens not
    // thousands") keys once here is cheaper than paying map-node overhead for the whole build.
    std::vector<u64> sortedKeys;
    sortedKeys.reserve(matConstantsByKey.size());
    for (const auto& [k, mc] : matConstantsByKey) { (void)mc; sortedKeys.push_back(k); }
    std::sort(sortedKeys.begin(), sortedKeys.end());

    // Index 0 is ALWAYS the material system's fallback, the sentinel for a draw whose material could
    // not be resolved. Nothing in the per-draw loop produces such a draw today (every instance
    // resolves to an authored material or a synthesized row, both of which always succeed), but the
    // slot is reserved so a future resolution path that CAN fail has somewhere to point instead of
    // silently mislabelling a real material as the sentinel.
    rtMaterialData_.clear();
    rtMaterialData_.reserve(sortedKeys.size() + 1);
    rtMaterialData_.push_back(materials_.fallbackConstants());

    std::unordered_map<u64, u32> indexByKey;
    indexByKey.reserve(sortedKeys.size());
    for (u64 k : sortedKeys) {
        indexByKey.emplace(k, static_cast<u32>(rtMaterialData_.size()));
        rtMaterialData_.push_back(matConstantsByKey.at(k));
    }

    // The lookup pass: every instance this build recorded gets its final dense index. Lockstep with
    // rtInstanceData_ by construction -- both are pushed once per surviving draw, in the same loop,
    // in buildAccelerationStructures.
    for (usize i = 0; i < rtInstanceData_.size(); ++i) {
        const auto it = indexByKey.find(rtInstanceMatKey_[i]);
        rtInstanceData_[i].materialIndex = (it != indexByKey.end()) ? it->second : 0u;
    }

    // ---- re-upload only when the content actually differs from what the GPU already holds ----
    const bool changed = rtMaterialData_.size() != rtMaterialUploaded_.size() ||
        std::memcmp(rtMaterialData_.data(), rtMaterialUploaded_.data(),
                    rtMaterialData_.size() * sizeof(pbr::MaterialConstants)) != 0;
    const bool needGrow = rtMaterialCapacity_ < rtMaterialData_.size();
    if (!changed && !needGrow && rtMaterialsReady_) return true;   // GPU content already correct

    if (needGrow) {
        // UPLOAD-heap RING, matching rtInstances_ exactly -- see this buffer's own declaration
        // comment in the header for why a RARE write needs the identical ring a per-frame write
        // does: writeBuffer is unsynchronised, and the hazard is per-WRITE, not per-frame.
        for (u32 i = 0; i < kRtInstanceRing; ++i) {
            if (rtMaterials_[i]) res_->destroyBuffer(rtMaterials_[i]);
            rhi::BufferDesc d;
            d.bytes = sizeof(pbr::MaterialConstants) * rtMaterialData_.size();
            d.kind  = rhi::BufferKind::Upload;
            d.debugName = "rt materials";
            rtMaterials_[i] = res_->createBuffer(d);
            if (!rtMaterials_[i]) {
                // Every remaining slot in the ring is destroyed already; leave the handles null
                // rather than half a ring, which setSrvBuffer below would otherwise bind
                // inconsistently slot to slot.
                rtMaterialCapacity_ = 0;
                rtMaterialsReady_ = false;
                if (!rtMaterialLogged_) {
                    AVER_ERROR("[Voxi] material table buffer could not be (re)created at {} elements; "
                               "ray hits read stale or null-filled material data until this succeeds",
                               rtMaterialData_.size());
                    rtMaterialLogged_ = true;
                }
                return false;
            }
        }
        rtMaterialCapacity_ = static_cast<u32>(rtMaterialData_.size());
    }

    // Rotate BEFORE writing, so this build never touches the slot a previous, still-in-flight
    // frame's rays might be reading -- see the ring's own declaration comment.
    rtMaterialSlot_ = (rtMaterialSlot_ + 1) % kRtInstanceRing;
    res_->writeBuffer(rtMaterials_[rtMaterialSlot_], rtMaterialData_.data(),
                      sizeof(pbr::MaterialConstants) * rtMaterialData_.size(), 0);
    res_->setSrvBuffer(bindings_, 9, rtMaterials_[rtMaterialSlot_], sizeof(pbr::MaterialConstants),
                       static_cast<u32>(rtMaterialData_.size()), 0);
    rtMaterialUploaded_ = rtMaterialData_;
    rtMaterialsReady_ = true;

    if (!rtMaterialLogged_) {
        rtMaterialLogged_ = true;
        // A realistic material count (tens) at sizeof(pbr::MaterialConstants) bytes each is a few KB,
        // times three ring slots (the ring holds the largest table any slot has needed, same as
        // rtInstances_). Re-uploads only on a rare build where the memcmp above finds a real
        // difference -- a hot-reload, or a newly-appearing material -- not every frame, and not
        // merely on reorder.
        const usize bytesPerSlot = rtMaterialData_.size() * sizeof(pbr::MaterialConstants);
        AVER_INFO("[Voxi] ray-traced material table: {} distinct material(s) (index 0 the fallback "
                  "sentinel) = {} bytes, x{} ring slots = {} bytes resident. Re-uploaded only when "
                  "its content changes, not every frame.",
                  rtMaterialData_.size(), bytesPerSlot, kRtInstanceRing, bytesPerSlot * kRtInstanceRing);
    }
    return true;
}

// LOCAL LIGHTS (LAMPS): see the header's own comment on buildLocalLights for when the list is empty.
//
// WHAT MAKES A LIGHT: an AUTHORED draw (the hashDrawMaterialInto test -- an unauthored draw's d.mat is
// not a material this project wrote) whose captured MaterialConstants carry MaterialFlag_Light with
// lightIntensity > 0, and whose mesh reported bounds (a negative radius has no sphere to light from).
// lightIntensity is a MULTIPLIER on the light the emitter's own glow and size physically cast, not a
// brightness value by itself: the glow is emissiveFactor's peak channel treated as a Lambertian
// sphere's radiance L, the size is the draw's bounding sphere radius r, and E1m = pi * L * r^2 (r in
// metres) is the irradiance that sphere casts at 1 metre, in the sun's units (SkyAtmosphere::
// sunIntensity) -- so 1 lights exactly what the material's own glow and size cast, 2 lights twice
// that, and a lamp and the sun still compose on the same scale. A brighter glow therefore lights
// more, as it would. The colour is emissiveFactor normalised to a max component of 1 (white when all
// zero, so a lamp with no emissive factor still has an L and a colour to light with -- see the L
// fallback below).
//
// RANGE: where irradiance E1m * lightIntensity / d^2 (d in metres) falls to kLocalLightRangeCutoff of
// the sun's units -- d = sqrt(E1m * lightIntensity / kLocalLightRangeCutoff) metres -- clamped to at
// least four radii (a lamp always lights its own surroundings) and at most 50 m (the per-pixel loop
// must end somewhere). The shader fades to zero at it. COST is about how many lamps a pixel has in
// range: MEASURED on NewSponza's 22 lamps under the earlier absolute brightness (intensity 2, cutoff
// 0.01, ~14 m ranges) the lamps cost 1.6 ms of a 16 ms frame, and a 32 m range put nearly every lamp
// in every pixel's two loops. E1m for a bulb a few centimetres across is around 0.1, so 0.001 keeps
// its range near that same ~14 m (chosen, not measured); 0.01 would cut it off at about 3 m.
//
// CANONICAL ORDER: drawsPrev_ is reordered every frame (occlusionOrder_), and the importance order
// shifts whenever the camera moves, but the shader's light pick is weighted per pixel and does not
// care about order. So the kept set is re-sorted by its own bytes before upload: the uploaded list,
// and so rdLocalLightHash_, is a function of the SET alone, and the accumulated history survives a
// camera move that does not change which lamps are in it.
void VoxiRenderer::buildLocalLights() {
    rdLocalLightCount_ = 0;
    rdLocalLightHash_ = 0;
    rdLocalLightsCarryAll_ = false;
    rdLocalLightData_.clear();
    rdLocalLightCand_.clear();
    if (!res_ || !bindings_) return;

    f32 vp[16], eye[3] = {};
    // Every ray-traced scene mode lights with the list (raster, single-pass, staged), so neither the
    // render mode nor CSRdLocalLights' pipeline gates it here -- publishLocalLights decides per pass.
    // No inverse needed: every lamp candidate below works from eye and the draw's own bounds.
    const bool wanted = settings_.localLights && rtActive_ && dev_ &&
                        dev_->backend() == rhi::Backend::D3D12 && dev_->camera(vp, nullptr, eye);
    // Every draw whose material ASKS to be a light, kept or not -- see rdLocalLightsCarryAll_.
    u32 flagged = 0;
    // At this cutoff, a white Lambertian surface receives kLocalLightRangeCutoff / pi ~ 3e-4 of the
    // lamp's radiance -- below display precision even under strong auto-exposure brightening. See the
    // function comment above for how this compares to the old, pre-physical-scaling cutoff.
    constexpr f32 kLocalLightRangeCutoff = 0.001f;
    if (wanted) {
        for (const Draw& d : drawsPrev_) {
            if (d.matBytes < sizeof(pbr::MaterialConstants)) continue;
            // TWO FIELDS FIRST, the whole block only for a lamp: this loop visits every draw in the
            // scene (up to kMaxDraws), and nearly none of them are lamps.
            u32 flags = 0;
            std::memcpy(&flags, d.mat + offsetof(pbr::MaterialConstants, flags), sizeof(flags));
            if (!(flags & pbr::MaterialFlag_Light)) continue;
            f32 intensity = 0.0f;
            std::memcpy(&intensity, d.mat + offsetof(pbr::MaterialConstants, lightIntensity),
                        sizeof(intensity));
            if (!(intensity > 0.0f) || !std::isfinite(intensity)) continue;   // also rejects NaN
            ++flagged;
            if (d.boundsRadius < 0.0f) continue;
            if (!materials_.ownsBindingSet(d.matSet) || d.matSet == materials_.fallbackBindingSet())
                continue;
            pbr::MaterialConstants mc;
            std::memcpy(&mc, d.mat, sizeof(mc));

            f32 col[3] = {std::max(mc.emissiveFactor[0], 0.0f), std::max(mc.emissiveFactor[1], 0.0f),
                          std::max(mc.emissiveFactor[2], 0.0f)};
            const f32 peak = std::max(col[0], std::max(col[1], col[2]));
            // L is the glow's own radiance -- the peak channel taken BEFORE colour is normalised out
            // of it below, so a brighter emissiveFactor casts more light at the same lightIntensity
            // than a dim one does. A lamp material can carry lightIntensity with no emissiveFactor at
            // all (peak <= 0): it still has to be a light, so it falls back to L = 1 with a white
            // colour rather than casting nothing.
            f32 L;
            if (peak > 0.0f && std::isfinite(peak)) {
                L = peak;
                for (f32& c : col) c /= peak;
            } else {
                L = 1.0f;
                col[0] = col[1] = col[2] = 1.0f;
            }
            const f32 maxColour = std::max(col[0], std::max(col[1], col[2]));   // always 1, post-normalise
            const f32 radius = std::max(d.boundsRadius, 1.0f);
            // E1m: the irradiance a Lambertian sphere of radiance L and this radius casts at 1 metre
            // -- pi * L * r^2 with r in metres (the two factors of 0.01 for cm -> m become 1e-4).
            // lightIntensity then MULTIPLIES this physically-consistent output rather than standing in
            // for it: 1 is exactly the light the glow and size cast, 2 is twice that.
            const f32 E1m = kPi * L * (radius * radius) * 1e-4f;
            const f32 output = E1m * intensity;
            // min(max(...)), HLSL clamp's order: a sphere wider than 12.5 m still gets 50 m, not more.
            const f32 range = std::min(std::max(100.0f * std::sqrt(output * maxColour / kLocalLightRangeCutoff),
                                                radius * 4.0f),
                                       5000.0f);

            RdLocalLightCand c{};
            c.light.posRadius[0] = d.boundsCentre[0];
            c.light.posRadius[1] = d.boundsCentre[1];
            c.light.posRadius[2] = d.boundsCentre[2];
            c.light.posRadius[3] = radius;
            c.light.radianceRange[0] = col[0] * output;
            c.light.radianceRange[1] = col[1] * output;
            c.light.radianceRange[2] = col[2] * output;
            c.light.radianceRange[3] = range;
            const f32 dx = (d.boundsCentre[0] - eye[0]) * 0.01f;   // cm -> m
            const f32 dy = (d.boundsCentre[1] - eye[1]) * 0.01f;
            const f32 dz = (d.boundsCentre[2] - eye[2]) * 0.01f;
            c.importance = output / std::max(dx * dx + dy * dy + dz * dz, 1.0f);
            rdLocalLightCand_.push_back(c);
        }
    }

    // Byte order of the light itself: a total order independent of where a draw sat in the list.
    auto canonicalLess = [](const RdLocalLight& a, const RdLocalLight& b) {
        return std::memcmp(&a, &b, sizeof(RdLocalLight)) < 0;
    };
    if (rdLocalLightCand_.size() > kMaxLocalLights) {
        // Ties broken canonically too, so which lamp is dropped at the cut does not depend on draw order.
        std::partial_sort(rdLocalLightCand_.begin(), rdLocalLightCand_.begin() + kMaxLocalLights,
                          rdLocalLightCand_.end(),
                          [&](const RdLocalLightCand& a, const RdLocalLightCand& b) {
                              if (a.importance != b.importance) return a.importance > b.importance;
                              return canonicalLess(a.light, b.light);
                          });
        rdLocalLightCand_.resize(kMaxLocalLights);
    }
    rdLocalLightData_.reserve(rdLocalLightCand_.size());
    for (const RdLocalLightCand& c : rdLocalLightCand_) rdLocalLightData_.push_back(c.light);
    std::sort(rdLocalLightData_.begin(), rdLocalLightData_.end(), canonicalLess);

    if (!rdLocalLightData_.empty()) {
        // Grown straight to the cap on first need: 32 x 32 bytes per slot, so there is never a reason
        // to reallocate again as lamps come and go.
        if (rdLocalLightCapacity_ < rdLocalLightData_.size()) {
            // Every slot or none: a half-built ring would bind a null buffer on its missing turns.
            bool ok = true;
            for (u32 i = 0; i < kRtInstanceRing; ++i) {
                if (rdLocalLights_[i]) res_->destroyBuffer(rdLocalLights_[i]);
                rhi::BufferDesc bd;
                bd.bytes = sizeof(RdLocalLight) * kMaxLocalLights;
                bd.kind  = rhi::BufferKind::Upload;
                bd.debugName = "Voxi local lights";
                rdLocalLights_[i] = res_->createBuffer(bd);
                ok = ok && rdLocalLights_[i] != 0;
            }
            rdLocalLightCapacity_ = ok ? kMaxLocalLights : 0;
        }
        if (rdLocalLightCapacity_ >= rdLocalLightData_.size()) {
            // Rotate BEFORE writing, so this frame never touches the buffer the previous one bound.
            rdLocalLightSlot_ = (rdLocalLightSlot_ + 1) % kRtInstanceRing;
            const rhi::BufferHandle buf = rdLocalLights_[rdLocalLightSlot_];
            const u32 count = static_cast<u32>(rdLocalLightData_.size());
            res_->writeBuffer(buf, rdLocalLightData_.data(), sizeof(RdLocalLight) * count, 0);
            res_->setSrvBuffer(bindings_, 18, buf, sizeof(RdLocalLight), count, 0);
            rdLocalLightsBound_ = buf;
            rdLocalLightCount_ = count;
            u64 h = 1469598103934665603ull;
            const u8* bytes = reinterpret_cast<const u8*>(rdLocalLightData_.data());
            for (usize i = 0; i < sizeof(RdLocalLight) * count; ++i) { h ^= bytes[i]; h *= 1099511628211ull; }
            h ^= count; h *= 1099511628211ull;
            rdLocalLightHash_ = h;
        } else if (!rdLocalLightsFailLogged_) {
            rdLocalLightsFailLogged_ = true;
            AVER_ERROR("[Voxi] local-light list buffer could not be created; lamps stay unlit (said once)");
        }
    }
    rdLocalLightsCarryAll_ = rdLocalLightCount_ > 0 && rdLocalLightCount_ == flagged;
    // Empty (or the ring failed): the placeholder, rebound only when t18 names something else.
    if (rdLocalLightCount_ == 0 && rdLocalLightsPlaceholder_ && rdLocalLightsBound_ != rdLocalLightsPlaceholder_) {
        res_->setSrvBuffer(bindings_, 18, rdLocalLightsPlaceholder_, sizeof(RdLocalLight), 1, 0);
        rdLocalLightsBound_ = rdLocalLightsPlaceholder_;
    }
}

// LOCAL LIGHTS (LAMPS): see the header's own comment. The history is trusted only when a scene pass
// wrote it LAST frame (rdLocalHistFrame_) under the SAME light list (rdLocalHistHash_) and the shadow
// history's own reprojection is usable this frame (rtHistParams.y > 0; its 0.5 "sun moved" state is
// still usable here -- lamps do not depend on the sun). Which mode wrote it does not matter: all three
// write the same quantity into the same pair, texel for texel.
bool VoxiRenderer::publishLocalLights(bool live, const char* pass) {
    if (!live) {
        cb_.cameraMedium[2] = 0.0f;
        cb_.cameraMedium[3] = 0.0f;
        return false;
    }
    const bool histValid = cb_.rtHistParams[1] > 0.25f && rdLocalHistFrame_ != 0 &&
                           rdLocalHistFrame_ + 1u == rtFrameIndex_ &&
                           rdLocalHistHash_ == rdLocalLightHash_;
    cb_.cameraMedium[2] = static_cast<f32>(rdLocalLightCount_);
    // Two bits: 1 = history valid; 2 = every lamp-flagged draw is a live light this frame, so the GI
    // estimators may drop an emitter's own emission (rdLocalLightsCarryAll_).
    cb_.cameraMedium[3] = (histValid ? 1.0f : 0.0f) + (rdLocalLightsCarryAll_ ? 2.0f : 0.0f);
    if (!rdLocalLightsRunLogged_) {
        rdLocalLightsRunLogged_ = true;
        AVER_INFO("[Voxi] local lights running: {} lamp(s) this frame, shaded by {}",
                  rdLocalLightCount_, pass);
    }
    return true;
}

// Renders the replayed draw list into each cascade's quadrant of the shadow atlas, depth only.
// INSTANCED BY DEFAULT when shadowInstancedPso_ built: every surviving draw in a cascade is grouped
// by mesh into shadowInstanceGroups_, one IRenderContext::drawMeshInstanced call per group instead of
// one drawMesh() per draw. Depth-only rendering with a strict Less test is order-independent except
// for two triangles landing on EXACTLY the same depth at the same pixel, so regrouping by mesh
// produces the identical shadow map at a fraction of the work -- up to ~2,114 drawMesh calls per
// cascade collapse to at most ~30 (this scene's distinct mesh count). Falls back to the untouched
// one-draw-per-instance path (shadowPso_) if the instanced pipeline failed to build.
void VoxiRenderer::shadowPass(rhi::IRenderContext& ctx) {
    const bool useInstancing = shadowInstancedPso_ != 0;
    if ((!shadowPso_ && !shadowInstancedPso_) || drawsPrev_.empty()) { cb_.shadowParams[1] = 0.0f; return; }
    u32 cascades = fitCascades();
#ifdef AVER_VOXI_SHADOW_CASCADE_LIMIT
    // TEMPORARY, for isolating this pass's own cost (see the CMake cache variable of the same name).
    // The default build sets it to kShadowCascades (4), so this is a no-op unless someone deliberately
    // reconfigures. fitCascades() still runs UNCLAMPED above -- cheap CPU work, and where curViewProj_
    // is captured, which endShadowHistory needs every frame regardless of cascade count.
    if (cascades > static_cast<u32>(AVER_VOXI_SHADOW_CASCADE_LIMIT))
        cascades = static_cast<u32>(AVER_VOXI_SHADOW_CASCADE_LIMIT);
#endif
    if (cascades == 0) { cb_.shadowParams[1] = 0.0f; return; }

    // THE ATLAS HAS NO READER WHILE RAY TRACING IS ON, so filling it is dead work -- in BOTH the
    // raster and the ray-driven mode, which is broader than the "cascade map for nobody" note that
    // stood elsewhere in this tree claimed.
    //
    // THE WHOLE CHAIN, because it is short and it is the entire justification: gShadowTex is sampled
    // at exactly one place (voxi.hlsl:1585, inside shadowSampleCascade), reached from exactly one
    // caller (shadowFactor, voxi.hlsl:1594), which the lit pass calls only on the
    // `gShadowParams.z <= 0.5` arm (voxi.hlsl:1814-1817) or in the shader variant compiled without
    // AVER_RT (:1819). And shadowParams[2] IS this same rtActive_ -- the two are written together,
    // 0.0f/false at the top of buildAccelerationStructures and 1.0f/true once the TLAS exists, so
    // they cannot disagree. PSRayDriven has no cascade arm at all (voxi.hlsl:2224 traces
    // unconditionally). Whenever there is a TLAS, nothing samples this texture.
    //
    // MEASURED on PTTest Sponza at a fixed camera: the "Voxi shadow" GPU scope is 5.57 ms at four
    // cascades and 2.07 ms at two, on a ~47 ms frame. It also explains an earlier experiment that
    // dropped four cascades to two and found a large saving with NO visual change -- there was no
    // visual change available, because there is no reader.
    //
    // fitCascades() STILL RUNS, above: it is cheap CPU work, and it is where curViewProj_ is
    // captured, which endShadowHistory copies into prevViewProj_ every frame for temporal
    // reprojection whether or not a cascade is ever drawn. Returning before it would break every
    // temporally-reprojected effect while looking like a pure optimisation.
    //
    // shadowParams[1] = 0 is what makes this SAFE rather than merely unread: shadowFactor's own
    // first line is `if (gShadowParams.y < 0.5) return 1.0`, so a pixel that somehow reaches the
    // cascade arm is unshadowed instead of sampling an atlas holding the last non-RT frame. The two
    // early-outs above set it for the same reason; this is the third case, not a new convention.
    //
    // Skipping the pass also skips BOTH halves of its barrier pair (ShaderResource -> DepthWrite
    // here, and back at the end), so the texture stays in ShaderResource -- exactly the state its
    // SRV binding expects. Skipping only one of the two would be the bug this note exists to avoid.
    if (rtActive_) { cb_.shadowParams[1] = 0.0f; return; }

    cb_.shadowParams[0] = 1.0f / static_cast<f32>(kShadowSize);
    cb_.shadowParams[1] = 1.0f;
    cb_.shadowParams[3] = static_cast<f32>(cascades);

    rhi::ScopedGpuStat gpuStat(ctx, "Voxi shadow");
    ctx.textureBarrier(shadowTex_, rhi::ResourceState::ShaderResource, rhi::ResourceState::DepthWrite);
    ctx.setPipeline(useInstancing ? shadowInstancedPso_ : shadowPso_);
    ctx.setBindingSet(bindings_);       // Tier 1: bind every declared table, read or not
    ctx.setRenderTargets(nullptr, 0, shadowTex_);
    ctx.clearDepth(shadowTex_, 1.0f);   // the whole atlas, once, before any quadrant is drawn

    // Depth-only, no pixel shader bound in either pipeline: material CONTENT is never read, so one
    // binding for the whole pass satisfies Tier 1's "table 1 must hold something" requirement for
    // every draw regardless of which draw's own material it actually names.
    ctx.setDrawBinding(materials_.fallbackBindingSet(), &materials_.fallbackConstants(), sizeof(pbr::MaterialConstants));

    // TEMPORARY measurement instrumentation: a one-time census of what this pass actually submits,
    // per cascade. Logged ONCE, not every frame -- this asks "what does a typical frame cost", not
    // "watch it every frame". Adds one INFO line to the log and nothing to the rendered image.
    static bool sShadowCensusLogged = false;
    u32 censusPerCascade[kShadowCascades] = {};

    for (u32 c = 0; c < cascades; ++c) {
        cb_.shadowDraw[0] = static_cast<f32>(c);
        ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));

        const u32 qx = (c & 1u) * kShadowCascadeSize;
        const u32 qy = (c >> 1) * kShadowCascadeSize;
        // Viewport and scissor both: the scissor confines the cascade to its own quadrant.
        ctx.setViewport(qx, qy, kShadowCascadeSize, kShadowCascadeSize);
        ctx.setScissor(qx, qy, kShadowCascadeSize, kShadowCascadeSize);

        const Vec3 cascCentre{cascadeCentre_[c][0], cascadeCentre_[c][1], cascadeCentre_[c][2]};
        const f32 cascRadius = cascadeRadius_[c];

        // WHAT THIS CASCADE CAN ACTUALLY RESOLVE: each spends its 2048 texels over its whole fitted
        // radius, so a far cascade's texel is metres wide while a near one's is centimetres -- an
        // object smaller than a texel cannot put a shadow into the map, so rasterising it is wasted.
        // Written for thousands of scattered ankle-height plants, sub-texel by cascade 2 and drawn
        // into it anyway. PER-CASCADE, not global: the same plant is real detail in cascade 0 and
        // invisible in cascade 3.
        const f32 cascTexelWorld = 2.0f * cascRadius / static_cast<f32>(kShadowCascadeSize);
        const f32 minShadowDiameter = cascTexelWorld * kMinShadowTexels;

        u32 submitted = 0;
        if (useInstancing) {
            // Reset capacity, not the group list itself: the mesh set repeats cascade to cascade and
            // frame to frame, so keeping each group's (now empty) vector alive means only the FIRST
            // frame's cascades pay for growth -- see shadowInstanceGroups_'s own comment.
            for (ShadowInstanceGroup& g : shadowInstanceGroups_) g.worlds.clear();

            for (const Draw& d : drawsPrev_) {
                // TRANSLUCENT DRAWS ARE NOT IN THIS DEPTH-ONLY PASS: it binds no pixel shader and a
                // depth map has no channel for a transmittance. They go to the TLAS instead, where a
                // shadow ray can attenuate through them. See submitDraw.
                if (d.translucent) continue;
                // Sphere-sphere overlap against this cascade's own fitted bounds: a draw whose
                // bounding sphere cannot reach the cascade's box cannot cast a shadow into it. A
                // negative boundsRadius means the backend had no bounds for this mesh -- draw it
                // regardless rather than guess.
                if (d.boundsRadius >= 0.0f) {
                    const Vec3 dc{d.boundsCentre[0], d.boundsCentre[1], d.boundsCentre[2]};
                    if (dist(dc, cascCentre) > cascRadius + d.boundsRadius) continue;
                    if (2.0f * d.boundsRadius < minShadowDiameter) continue;
                }
                ShadowInstanceGroup* group = nullptr;
                for (ShadowInstanceGroup& g : shadowInstanceGroups_)
                    if (g.mesh == d.depthMesh) { group = &g; break; }
                if (!group) { shadowInstanceGroups_.push_back({d.depthMesh, {}}); group = &shadowInstanceGroups_.back(); }
                const usize base = group->worlds.size();
                group->worlds.resize(base + 16);
                std::memcpy(group->worlds.data() + base, d.world, 16 * sizeof(f32));
                ++submitted;
            }
            for (const ShadowInstanceGroup& g : shadowInstanceGroups_) {
                if (g.worlds.empty()) continue;
                ctx.drawMeshInstanced(g.mesh, g.worlds.data(), static_cast<u32>(g.worlds.size() / 16));
            }
        } else {
            for (const Draw& d : drawsPrev_) {
                // Translucent draws excluded (depth-only pass) -- see the note above, first branch.
                if (d.translucent) continue;
                if (d.boundsRadius >= 0.0f) {
                    const Vec3 dc{d.boundsCentre[0], d.boundsCentre[1], d.boundsCentre[2]};
                    if (dist(dc, cascCentre) > cascRadius + d.boundsRadius) continue;
                    if (2.0f * d.boundsRadius < minShadowDiameter) continue;
                }
                f32 consts[rhi::kObjectConstantDwords]{};
                std::memcpy(consts, d.world, 16 * sizeof(f32));   // depth-only: nothing else is read
                ctx.setConstants(rhi::kObjectConstantRegister, consts, rhi::kObjectConstantDwords);
                ctx.drawMesh(d.depthMesh);
                ++submitted;
            }
        }
        censusPerCascade[c] = submitted;
    }

    if (!sShadowCensusLogged) {
        u32 total = 0;
        for (u32 c = 0; c < kShadowCascades; ++c) total += censusPerCascade[c];
        AVER_INFO("[Voxi] shadowPass census (once): {} draws available this frame, {} cascade(s) run, "
                  "per-cascade draws submitted = [{}, {}, {}, {}], total submitted = {}",
                  static_cast<u32>(drawsPrev_.size()), cascades,
                  censusPerCascade[0], censusPerCascade[1], censusPerCascade[2], censusPerCascade[3],
                  total);
        sShadowCensusLogged = true;
    }

    ctx.textureBarrier(shadowTex_, rhi::ResourceState::DepthWrite, rhi::ResourceState::ShaderResource);
}

// Renders the GI-only shadow map: one box over the GI volume, for light injection alone.
// Called from prePass INSIDE the giUpdateInterval gate, so it runs on exactly the frames
// voxelizePass runs -- 1 in 4 at the Medium default. That is the second half of the win: the old
// arrangement rebuilt volume-wide shadow coverage every single frame, including the 3 in 4 on which
// no voxel was re-injected and nothing read it.
void VoxiRenderer::giShadowPass(rhi::IRenderContext& ctx) {
    const bool useInstancing = giShadowInstancedPso_ != 0;
    if ((!giShadowPso_ && !giShadowInstancedPso_) || drawsPrev_.empty()) {
        cb_.giShadowParams[1] = 0.0f;   // unusable: giShadowFactor falls back to fully lit
        return;
    }
    fitGiShadow();                       // sets giShadowParams[1] = 1 and the cull sphere

    rhi::ScopedGpuStat gpuStat(ctx, "Voxi GI shadow");
    ctx.textureBarrier(giShadowTex_, rhi::ResourceState::ShaderResource, rhi::ResourceState::DepthWrite);
    ctx.setPipeline(useInstancing ? giShadowInstancedPso_ : giShadowPso_);
    ctx.setBindingSet(bindings_);        // Tier 1: bind every declared table, read or not
    ctx.setRenderTargets(nullptr, 0, giShadowTex_);
    ctx.clearDepth(giShadowTex_, 1.0f);
    ctx.setViewport(0, 0, kGiShadowSize, kGiShadowSize);
    ctx.setScissor(0, 0, kGiShadowSize, kGiShadowSize);
    // Depth-only, no pixel shader: material content is never read, so one binding satisfies Tier 1
    // for every draw -- the same reasoning shadowPass states above.
    ctx.setDrawBinding(materials_.fallbackBindingSet(), &materials_.fallbackConstants(),
                       sizeof(pbr::MaterialConstants));
    ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));

    const Vec3 giCentre{giShadowCentre_[0], giShadowCentre_[1], giShadowCentre_[2]};

    u32 submitted = 0;
    if (useInstancing) {
        for (ShadowInstanceGroup& g : giShadowInstanceGroups_) g.worlds.clear();
        for (const Draw& d : drawsPrev_) {
            // Translucent draws excluded (depth-only pass) -- see shadowPass's note on the same skip.
            if (d.translucent) continue;
            // Culled against the VOLUME's sphere, never cascadeCentre_[3]/cascadeRadius_[3] -- those
            // are camera-fitted now and have nothing to do with what the volume covers.
            if (d.boundsRadius >= 0.0f && dist(Vec3{d.boundsCentre[0], d.boundsCentre[1], d.boundsCentre[2]},
                                                giCentre) > giShadowRadius_ + d.boundsRadius) continue;
            ShadowInstanceGroup* group = nullptr;
            for (ShadowInstanceGroup& g : giShadowInstanceGroups_)
                if (g.mesh == d.depthMesh) { group = &g; break; }
            if (!group) { giShadowInstanceGroups_.push_back({d.depthMesh, {}}); group = &giShadowInstanceGroups_.back(); }
            const usize base = group->worlds.size();
            group->worlds.resize(base + 16);
            std::memcpy(group->worlds.data() + base, d.world, 16 * sizeof(f32));
            ++submitted;
        }
        for (const ShadowInstanceGroup& g : giShadowInstanceGroups_) {
            if (g.worlds.empty()) continue;
            ctx.drawMeshInstanced(g.mesh, g.worlds.data(), static_cast<u32>(g.worlds.size() / 16));
        }
    } else {
        for (const Draw& d : drawsPrev_) {
            // Translucent draws excluded (depth-only pass) -- see shadowPass's note on the same skip.
            if (d.translucent) continue;
            if (d.boundsRadius >= 0.0f && dist(Vec3{d.boundsCentre[0], d.boundsCentre[1], d.boundsCentre[2]},
                                                giCentre) > giShadowRadius_ + d.boundsRadius) continue;
            f32 consts[rhi::kObjectConstantDwords]{};
            std::memcpy(consts, d.world, 16 * sizeof(f32));
            ctx.setConstants(rhi::kObjectConstantRegister, consts, rhi::kObjectConstantDwords);
            ctx.drawMesh(d.depthMesh);
            ++submitted;
        }
    }

    static bool sGiCensusLogged = false;
    if (!sGiCensusLogged) {
        AVER_INFO("[Voxi] giShadowPass census (once): {} draws available, {} submitted into one "
                  "{}^2 box of radius {:.0f}cm, every {} frame(s)",
                  static_cast<u32>(drawsPrev_.size()), submitted, kGiShadowSize, giShadowRadius_,
                  giUpdateInterval_);
        sGiCensusLogged = true;
    }

    ctx.textureBarrier(giShadowTex_, rhi::ResourceState::DepthWrite, rhi::ResourceState::ShaderResource);
}

// Clears the accumulator, rasterises the scene into it with direct lighting applied, then resolves
// it into mip 0 of the radiance volume.
void VoxiRenderer::voxelizePass(rhi::IRenderContext& ctx) {
    // DEFENSIVE, NOT EXPECTED TO FIRE. W12's gate change (prePass's rebuild branch) requests the
    // accumulator via giAccumWanted_ and skips straight to a retry next tick rather than ever calling
    // this function while voxelAccumTex_ is 0 -- see that branch's own comment. Kept anyway because
    // "the gate change is correct" is exactly the kind of claim a cheap check like this exists to
    // catch being wrong about, rather than handing clearBindings_'s slot-1 UAV a null texture.
    if (!voxelAccumTex_) {
        static bool sNoAccumWarned = false;
        if (!sNoAccumWarned) {
            sNoAccumWarned = true;
            AVER_WARN("[Voxi] voxelizePass skipped: no injection accumulator this tick "
                      "(should be unreachable -- see the W12 gate in prePass)");
        }
        return;
    }
    const u32 res = voxelResBuilt_;
    rhi::ScopedGpuStat gpuStat(ctx, "Voxi voxelise");

    // ---- W3 pre-pass: which part of the grid THIS rebuild's injection can possibly touch ----
    //
    // Walks drawsPrev_ with EXACTLY the three skips the injection loop further down applies
    // (translucent, compute-skinned, giVoxelisedDraw's bounds/volume test) so drawsBox names
    // precisely the set of voxels a draw could write this rebuild -- a looser box would clear/
    // resolve/mip texels the injection loop never touches (safe, but not the saving W3 exists to
    // buy); a tighter one would leave a stale value outside it uncleared (not safe at all).
    //
    // RUNS EVERY REBUILD, GI-BOUNDED-DISPATCH FLAG ON OR OFF -- this doubles as the M4-style census
    // instrument the log line at the bottom of this function reports: how big the box WOULD be, so
    // the saving can be judged before opting in.
    //
    // depthMesh's AABB, NOT d.mesh's: depthMesh is what this pass actually draws a few lines below
    // (ctx.dispatchMeshFor/ctx.drawMesh(d.depthMesh)), and a coarser LOD depth proxy
    // (SandboxApp.cpp's depth-proxy resolver) can have a different footprint than the mesh submit's
    // own bounding sphere was measured against.
    VoxelBox drawsBox{};
    bool anyUnbounded = false;
    {
        const f32 origin[3] = {center_[0] - extent_, center_[1] - extent_, center_[2] - extent_};
        for (const Draw& d : drawsPrev_) {
            if (d.translucent) continue;
            if (dev_ && dev_->meshVertexBuffer(d.mesh)) continue;
            if (!giVoxelisedDraw(d)) continue;

            f32 lmin[3], lmax[3];
            const bool haveAabb = d.boundsRadius >= 0.0f && dev_ &&
                                  dev_->meshBoundsAabb(d.depthMesh, lmin, lmax);
            if (!haveAabb) { anyUnbounded = true; continue; }

            // Row-major, row-vector -- the identical convention submit() already builds a Mat4 from
            // (see submit()'s own w.m/xformPoint use) -- transforming all eight local AABB corners
            // into world space.
            Mat4 w;
            std::memcpy(&w.m[0][0], d.world, sizeof(w.m));
            f32 wmin[3] = {1e30f, 1e30f, 1e30f}, wmax[3] = {-1e30f, -1e30f, -1e30f};
            for (int c = 0; c < 8; ++c) {
                const Vec3 corner{(c & 1) ? lmax[0] : lmin[0], (c & 2) ? lmax[1] : lmin[1],
                                  (c & 4) ? lmax[2] : lmin[2]};
                const Vec3 wc = xformPoint(corner, w);
                wmin[0] = std::fmin(wmin[0], wc.x); wmax[0] = std::fmax(wmax[0], wc.x);
                wmin[1] = std::fmin(wmin[1], wc.y); wmax[1] = std::fmax(wmax[1], wc.y);
                wmin[2] = std::fmin(wmin[2], wc.z); wmax[2] = std::fmax(wmax[2], wc.z);
            }
            // Pad 2 voxels: conservative raster (vox.conservativeRaster below) can light a voxel a
            // triangle only grazes, just outside the triangle's own tight AABB.
            drawsBox = unionBox(drawsBox, voxelBoxFromWorldAabb(wmin, wmax, origin, extent_ * 2.0f,
                                                                res, 2u));
        }
    }

    // ---- W3 box selection: bounded, or full, and why ----
    //
    // FULL means clear/resolve/mip run over [0,res) on every axis -- exactly today's behaviour, and
    // the safe answer whenever the box above cannot be trusted for this rebuild. The first reason
    // below that is true wins, and it doubles as this rebuild's C-7 census reason.
    //
    // WHY UNION WITH giBoxPrevDraws_ (the LAST rebuild's own draws box), NOT drawsBox ALONE. mip 0
    // outside drawsBox_k (this rebuild) still holds whatever drawsBox_(k-1) (the last rebuild) wrote
    // there -- CSResolve zeroes a voxel with no fragments (voxi.hlsl), so a voxel a draw stopped
    // touching needs to be cleared and re-resolved to zero THIS rebuild, or it keeps showing light
    // from a draw that no longer reaches it. Anything outside drawsBox_k UNION drawsBox_(k-1) is
    // already this rebuild's correct answer (0), by induction: rebuild k-1 established that
    // invariant for itself over its own union, and this rebuild extends it by exactly drawsBox_k.
    // giBoxPrevValid_ is what says the induction's base case actually holds for drawsBox_(k-1).
    const bool boxMovedOrResized = giBoxRes_ != res ||
        giBoxCentre_[0] != center_[0] || giBoxCentre_[1] != center_[1] || giBoxCentre_[2] != center_[2] ||
        giBoxExtent_ != extent_;
    bool full; const char* giBoxReason;
    if      (!giBoundedDispatch_) { full = true;  giBoxReason = "full: bounded dispatch off"; }
    else if (!giBoxPrevValid_)    { full = true;  giBoxReason = "full: no previous box"; }
    else if (anyUnbounded)        { full = true;  giBoxReason = "full: unbounded draw"; }
    else if (boxMovedOrResized)   { full = true;  giBoxReason = "full: volume moved or resized"; }
    else                           { full = false; giBoxReason = "bounded"; }
    const VoxelBox box0 = full ? fullVoxelBox(res)
                                : alignOutward(unionBox(drawsBox, giBoxPrevDraws_), 4u, res);
    giDispatchBox0_ = box0;   // filterMips derives each mip level's own box from this

    ctx.setPipeline(clearPso_);
    ctx.setBindingSet(clearBindings_);
    {
        const GiDispatchConstants k = dispatchConstants(box0, 0);
        ctx.setConstants(3, &k, kGiDispatchConstantDwords);
        u32 g[3];
        dispatchGroups(box0, 4u, g);
        if (g[0] && g[1] && g[2]) ctx.dispatch(g[0], g[1], g[2]);
    }
    ctx.uavBarrierTexture(voxelAccumTex_);   // injection must see the cleared accumulator

    // Prefers the mesh-shader voxelise pipeline WHENEVER the device built one, independent of
    // settings_.meshShaders -- that flag is a wider device-level switch (it also moves the MAIN
    // scene's lit draws onto their own mesh-shader pipeline) that stays off by default because it's
    // a real behaviour change to opt into. Voxelisation has no such wrinkle: its pipeline state is
    // copied from the same `vox` desc either way, and PSVoxel is the same pixel shader either way,
    // with MSVoxel now running the identical dominant-axis projection VSVoxel+GSVoxel do (see
    // MSVoxel's comment for the normal-transform bug this depended on fixing first).
    // createPipelines() already validates voxelMsPso_ regardless of the setting, so
    // `voxelMsPso_ != 0` alone means the device proved it can do this; GSVoxel is the fallback for a
    // device with no mesh-shader tier.
    const bool useMs = voxelMsPso_ != 0;
    ctx.setPipeline(useMs ? voxelMsPso_ : voxelPso_);
    ctx.setBindingSet(bindings_);
    ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
    ctx.setRenderTargets(nullptr, 0, 0);   // no targets at all: the pixel shader writes only the UAV
    ctx.setViewport(0, 0, res, res);
    ctx.setScissor(0, 0, res, res);

    // CULLED AGAINST THE VOLUME, which this pass did not do at all until now -- it rasterised every
    // draw in drawsPrev_ including ones streaming had brought in kilometres away; those never
    // survived (the pixel shader's UAV write lands outside the 128^3 grid and is dropped) but paid
    // the full vertex/raster cost first. On a streamed scene the volume covers a small fraction of
    // what's resident, so this cull is most of the pass.
    // The sphere is derived HERE from center_/extent_ rather than read from giShadowCentre_/
    // giShadowRadius_ (which fitGiShadow() computes identically), because reading those would
    // silently couple voxelisation to whether the GI shadow PSO built -- if it fails, those fields
    // keep stale values and the grid under-voxelises on that device only.
    const Vec3 volCentre{center_[0], center_[1], center_[2]};
    const f32 volRadius = (extent_ > 1.0f ? extent_ : 1.0f) * 1.7320508f;
    u32 voxelSubmitted = 0, voxelCulled = 0, voxelSkinned = 0;

    for (const Draw& d : drawsPrev_) {
        // Translucent draws excluded (depth-only pass) -- see shadowPass's note on the same skip.
        if (d.translucent) continue;
        // A COMPUTE-SKINNED MESH IS EXCLUDED, DELIBERATELY -- a trade, not a fix. giDrawsKey hashes
        // mesh/transform/material, never the vertex buffer a skinning dispatch rewrites every frame,
        // so once a character's TRANSFORM settles the gate reports "unchanged" and its indirect-light
        // contribution freezes at whatever pose the last rebuild saw. Measured on a real rig: 2
        // rebuilt / 62 skipped of 64 ticks straight through a pose transition.
        // The BLAS cache above fixes the same defect by rebuilding that mesh's structure every frame
        // -- NOT AVAILABLE HERE, since voxelisation is one volume, not per-mesh, and treating a
        // skinned draw as always-changed would force a FULL revoxelisation whenever any character is
        // on screen: measured, that costs the ~96% of GI rebuilds this gate normally avoids, on top
        // of the 17.3 ms GI already costs while skipping them.
        // So the interim state is absence, not a silent freeze: a skinned character bounces no
        // indirect light, matching PtSceneView::submitDraw's own reason. THE REAL FIX is partial
        // revoxelisation -- injecting one mesh's region without rebuilding the whole volume -- which
        // voxelizePass doesn't support today.
        if (dev_ && dev_->meshVertexBuffer(d.mesh)) { ++voxelSkinned; continue; }
        // The bounds half of giVoxelisedDraw, kept inline ONLY to keep the two counters that feed the
        // census line below. The predicate is the definition; this must not drift from it.
        if (!giVoxelisedDraw(d)) { ++voxelCulled; continue; }
        ++voxelSubmitted;
        f32 consts[rhi::kObjectConstantDwords];
        std::memcpy(consts, d.world, 16 * sizeof(f32));
        std::memcpy(consts + 16, d.color, 4 * sizeof(f32));
        consts[20] = d.metallic; consts[21] = d.roughness; consts[22] = 0.0f; consts[23] = 0.0f;
        writeShadingConstants(consts);
        ctx.setConstants(rhi::kObjectConstantRegister, consts, rhi::kObjectConstantDwords);
        // The material captured at submit time, so the injection shades the same surface the lit
        // pass does.
        if (d.matSet) ctx.setDrawBinding(d.matSet, d.mat, d.matBytes);
        else          ctx.setDrawBinding(materials_.fallbackBindingSet(),
                                         &materials_.fallbackConstants(), sizeof(pbr::MaterialConstants));
        if (useMs) ctx.dispatchMeshFor(d.depthMesh);
        else       ctx.drawMesh(d.depthMesh);
    }

    // Reported on a widening interval, because the ratio is the whole point of the cull and a
    // silent one would be indistinguishable from a cull that never fires. If this ever reads
    // "culled 0", the draws are arriving with boundsRadius < 0 and the cull is a no-op.
    if ((voxelCullLogs_ & (voxelCullLogs_ + 1)) == 0) {
        const u32 considered = voxelSubmitted + voxelCulled;
        // voxelSkinned is reported separately and NOT folded into `considered`: it is not a cull,
        // it is a capability gap, and averaging it into a cull percentage would hide exactly the
        // number someone debugging missing indirect light needs to see.
        //
        // THE SUFFIX IS W3's CENSUS: what fraction of the grid the injected draws' own box covers
        // (gridFraction(drawsBox, res)) against what fraction the clear/resolve/mip dispatch above
        // actually ran over (gridFraction(box0, res)) -- the gap between the two is W3's saving, and
        // giBoxReason says why box0 was (or was not) smaller this rebuild.
        AVER_INFO("[Voxi] voxelize {} draw(s), culled {} outside the volume ({:.0f}%), "
                  "{} skinned draw(s) excluded (they contribute no GI -- see voxelizePass); "
                  "injected-draw box {:.1f}% of the {}^3 grid, clear/resolve/mip over {:.1f}% ({})",
                  voxelSubmitted, voxelCulled,
                  considered ? 100.0 * static_cast<f64>(voxelCulled) / static_cast<f64>(considered) : 0.0,
                  voxelSkinned, gridFraction(drawsBox, res) * 100.0, res,
                  gridFraction(box0, res) * 100.0, giBoxReason);
    }
    ++voxelCullLogs_;

    // Reduce the atomic sums into the filterable RGBA16F volume.
    ctx.uavBarrierTexture(voxelAccumTex_);
    ctx.textureBarrier(voxelTex_, rhi::ResourceState::ShaderResource, rhi::ResourceState::UnorderedAccess);
    ctx.setPipeline(resolvePso_);
    ctx.setBindingSet(resolveBindings_);
    {
        const GiDispatchConstants k = dispatchConstants(box0, 0);
        ctx.setConstants(3, &k, kGiDispatchConstantDwords);
        u32 g[3];
        dispatchGroups(box0, 4u, g);
        if (g[0] && g[1] && g[2]) ctx.dispatch(g[0], g[1], g[2]);
    }

    // W3: record this rebuild's OWN draws box (not box0, which also folds in the previous rebuild's)
    // for the NEXT rebuild's union -- see the box-selection comment above for why the chain must span
    // exactly one previous rebuild, not accumulate indefinitely. giBoxPrevValid_ is false whenever
    // anyUnbounded fired this rebuild: an untrustworthy drawsBox this time must not be trusted as
    // "last rebuild's box" next time either.
    giBoxPrevDraws_ = drawsBox;
    giBoxPrevValid_ = !anyUnbounded;
    giBoxRes_ = res;
    giBoxCentre_[0] = center_[0]; giBoxCentre_[1] = center_[1]; giBoxCentre_[2] = center_[2];
    giBoxExtent_ = extent_;
}

// Box-filters each level of the radiance volume into the next, then hands the whole chain back as
// a shader resource.
void VoxiRenderer::filterMips(rhi::IRenderContext& ctx) {
    rhi::ScopedGpuStat gpuStat(ctx, "Voxi mip filter");
    ctx.uavBarrierTexture(voxelTex_);
    ctx.setPipeline(mipPso_);
    const u32 res = voxelResBuilt_;
    for (u32 m = 1; m < voxelMips_; ++m) {
        // Level m-1 becomes readable while level m stays writable: a per-subresource transition.
        ctx.textureBarrier(voxelTex_, rhi::ResourceState::UnorderedAccess,
                           rhi::ResourceState::NonPixelShaderResource, m - 1);
        ctx.setBindingSet(mipBindings_[m - 1]);
        // W3: bounded to the box THIS rebuild's clear/resolve actually touched at mip 0
        // (giDispatchBox0_, set by voxelizePass just before this function is called), rounded to
        // this level via mipBox() -- see voxelizePass's own comment on box0 for the correctness
        // argument mipBox()'s outward rounding relies on: CSMip's 2x2x2 footprint means a dest voxel
        // outside mipBox(box0, m, res) has its whole footprint outside box0 at mip m-1. srcMip stays
        // 0 in the constant block regardless -- the single-mip SRV this binding set declares
        // (mipBindings_[m-1]) already rebases the Load to level m-1, so the shader never needs to
        // know which real mip that is.
        const VoxelBox mb = mipBox(giDispatchBox0_, m, res);
        const GiDispatchConstants k = dispatchConstants(mb, 0);
        ctx.setConstants(3, &k, kGiDispatchConstantDwords);
        u32 g[3];
        dispatchGroups(mb, 4u, g);
        if (g[0] && g[1] && g[2]) ctx.dispatch(g[0], g[1], g[2]);
        ctx.uavBarrierTexture(voxelTex_);
    }
    // The coarsest level is never a source, so the loop never demoted it: reconcile it here to make
    // the whole-resource transition below legal.
    ctx.textureBarrier(voxelTex_, rhi::ResourceState::UnorderedAccess,
                       rhi::ResourceState::NonPixelShaderResource, voxelMips_ - 1);
    ctx.textureBarrier(voxelTex_, rhi::ResourceState::NonPixelShaderResource,
                       rhi::ResourceState::ShaderResource);
}

// Hands the backend Voxi's per-frame constant block.
bool VoxiRenderer::sceneConstants(const void** data, u32* bytes) const {
    if (!giReady_) return false;
    *data = &cb_; *bytes = sizeof(cb_);
    return true;
}

// Whether the blended draw this material belongs to reads the backend's captured backdrop -- see the
// base class's own comment for why the backend asks at all. Only two things in a Voxi material make
// the shader sample it (voxi.hlsl's averBlendedOutputBackdrop gate, ~1997): a non-zero attenuation
// distance (the material is a volume the backdrop is seen THROUGH), or a node graph driving the
// material (graphId != 0 -- a graph can compute attenuation per pixel, so this has to assume yes
// rather than read a distance a graph never wrote). Everything else -- the common case, a decal like
// NewSponza's floor dirt -- draws a flat blended colour over the backdrop's own hardware blend and
// never touches the texture at all.
bool VoxiRenderer::blendedDrawReadsBackdrop(const void* materialConstants, u32 bytes) const {
    if (bytes < sizeof(pbr::MaterialConstants) || !materialConstants) return true;   // unknown layout: assume yes
    const auto& mat = *static_cast<const pbr::MaterialConstants*>(materialConstants);
    return mat.attenuationDistance > 0.0f || mat.graphId != 0;
}

// True once the feature is up: shadowing and the bounce are terms inside Voxi's lit pixel shader.
bool VoxiRenderer::overridesScenePipeline() const { return giReady_; }

// True while the debug view replaces the scene, including the backend's line draws.
// TWO REASONS TO REPLACE THE SCENE, and they are not interchangeable -- see shadowHistoryActive()
// in the header, which asks only about the first.
// The table the blended pipeline above expects bound. The device asks during its replay, because
// this feature is not on the stack then -- see IRenderFeature::sceneBindlessTable.
rhi::BindlessTableHandle VoxiRenderer::sceneBindlessTable() const { return rtTexTable_; }

bool VoxiRenderer::suppressesScene() const { return debugViewActive() || rayDrivenActive(); }

// The debug raymarch paints every pixel from the voxel volume and has no depth to test against, so
// anything else drawing into that frame draws over a picture of something else. Ray-driven mode is
// the opposite case: it writes real SV_DEPTH (1.0 on a miss -- exactly what the sky pass tests EQUAL
// against), so the sky lands on precisely the pixels the primary rays missed.
bool VoxiRenderer::suppressesWholeFrame() const { return debugViewActive(); }

// Draws whichever pass has replaced the scene, over the colour target and viewport the backend
// already bound. The debug raymarch wins when both are somehow asked for: it is a diagnostic, and
// a diagnostic that silently did not run because another mode outranked it would be useless.
void VoxiRenderer::scenePass(rhi::IRenderContext& ctx) {
    if (!debugViewActive() && rayDrivenActive()) {
        // STAGED RAY-DRIVEN PASSES (milestone 1): checked first, before any of the single-pass state
        // below is touched, so the default (voxi.rayDrivenStages == 0, rdStagedWanted() false)
        // reaches the single-pass code exactly as it did before this feature existed -- rdStagedActive()
        // returns false on its very first line in that case, `reason` is never written, and nothing
        // below this block runs differently. See the header's own comment on rdStagedActive() for
        // every condition it checks.
        const char* stagedFallbackReason = nullptr;
        if (rdStagedActive(&stagedFallbackReason)) {
            if (!rdStagedRunLogged_) {
                rdStagedRunLogged_ = true;
                AVER_INFO("[Voxi] staged ray-driven passes running: visibility -> lighting (shadow, GI, "
                          "sky occlusion, reflections) -> shade (GPU spans 'Voxi RD visibility', "
                          "'Voxi RD lighting stages', 'Voxi ray-driven primary')");
            }
            recordStagedRayDriven(ctx);
            return;
        }
        // LOGGED ONCE, and only when there WAS a reason to give: rdStagedActive() leaves `reason` null
        // when rdStagedWanted() or rayDrivenActive() alone said no (nothing was requested, or there is
        // nothing to stage this frame), which is not a fallback worth a line.
        if (stagedFallbackReason && !rdStagedFallbackLogged_) {
            rdStagedFallbackLogged_ = true;
            AVER_WARN("[Voxi] voxi.rayDrivenStages (1 or 2) requested the staged ray-driven passes, but "
                      "{}; falling back to the single-pass ray-driven primary (said once)",
                      stagedFallbackReason);
        }
        // ITS OWN MARKER, so the go/no-go against the rasteriser is a subtraction between two
        // named spans in the same timing tree rather than a difference of whole frames.
        rhi::ScopedGpuStat rayStat(ctx, "Voxi ray-driven primary");
        // See pickGbuf()'s own comment: ray-driven mode never goes through scenePipeline(), so this
        // is the one call site that has to ask for its G-buffer twin directly.
        // TEXTURING AND THE G-BUFFER ARE INDEPENDENT AXES NOW, four pipelines rather than three.
        // This used to read:
        //     const bool textured = rayDrivenTexPso_ != 0 && !dev_->gBufferEnabled();
        // which was true to the pipelines that existed -- there was no textured G-buffer variant --
        // but it meant --gbuffer did not merely add three targets, it turned TEXTURING OFF. An A/B
        // across that flag compared a textured image with a flat-albedo one, and the whole difference
        // was there to be attributed to the G-buffer. See the header's comment on
        // rayDrivenTexGbufPso_ for why that had to be closed before, not after, the deferred-lighting
        // work leans on this flag.
        //
        // pickGbuf() STAYS THE SINGLE AUTHORITY on whether four targets will really be bound this
        // frame -- it declines when the twin never compiled, before init(), and at MSAA > 1, where the
        // G-buffer targets are single-sample and OMSetRenderTargets refuses to bind them beside a
        // multisampled colour target. Asking dev_->gBufferEnabled() again here would duplicate that
        // MSAA rule in a second place and the two would drift.
        const bool gbufBound = pickGbuf(rayDrivenPso_, rayDrivenGbufPso_) == rayDrivenGbufPso_;
        // FALLING BACK WITHIN THE SAME TARGET COUNT, never across it: if the textured G-buffer
        // pipeline did not compile, the fallback is the FLAT G-buffer one, not the textured
        // single-target one. Binding a pipeline that declares one target while the backend has bound
        // four would leave the G-buffer unwritten while every caller believed it was on.
        const rhi::PipelineHandle rdPso =
            gbufBound ? (rayDrivenTexGbufPso_ ? rayDrivenTexGbufPso_ : rayDrivenGbufPso_)
                      : (rayDrivenTexPso_     ? rayDrivenTexPso_     : rayDrivenPso_);
        // LOCAL LIGHTS (LAMPS): raised before this draw's upload -- it shades lamps and writes u19 itself
        // when compiled with the lamp term (kRdSinglePassLamps), and gets 0 otherwise. Left raised for the
        // blended replay, which lights its panes from the same list.
        const bool lamps = publishLocalLights(kRdSinglePassLamps && localLightsReady(),
                                              "the single-pass PSRayDriven");
        ctx.setPipeline(rdPso);
        ctx.setBindingSet(bindings_);
        ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
        // A no-op on the untextured pipeline, which declares no bindless table -- so this does not
        // need to be inside the branch above.
        ctx.setBindlessTable(rtTexTable_);
        ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
        ctx.drawFullscreen();
        // No UAV barrier on u19 after it: nothing later this frame reads it (the blended replay reads
        // u19 only under giShadowParams.w bit 16, which only the staged path sets), and next frame's
        // beginShadowHistory transition orders this write before t19's reads.
        if (lamps) {
            rdLocalHistFrame_ = rtFrameIndex_;
            rdLocalHistHash_ = rdLocalLightHash_;
        }
        return;
    }
    if (!debugPso_) return;
    rhi::ScopedGpuStat gpuStat(ctx, "Voxi debug view");
    ctx.setPipeline(debugPso_);
    ctx.setBindingSet(bindings_);
    // Table 1 is bound outright: this pipeline declares it and Tier 1 populates whole tables.
    ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
    ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
    ctx.drawFullscreen();
}

// STAGED RAY-DRIVEN PASSES (milestone 1, extended by milestone 2's lighting-stage trio, milestone 3's
// own CSRdRefl, and the sub-stage splits' own S1/G1 probe/trace passes): records CSRdVisibility, then
// (when Settings::rayDrivenShadowTiles/rayDrivenGiSplit ask for them) S1/G1, then
// CSRdShadow/CSRdGi/CSRdSkyOcc/CSRdRefl back to back with no barrier between THEM, then the
// AVER_RD_SPLIT fullscreen draw -- GPU spans in place of the
// single "Voxi ray-driven primary" span scenePass() otherwise records. Called only once scenePass() has
// already confirmed rdStagedActive(), so every existence/backend check that decides whether to call
// this lives there, not here.
//
// VERIFIED READ-ONLY AGAINST D3D12Device.cpp (this backend is the only one rdStagedActive() lets
// through) rather than assumed, because every one of the four points below is a documented way to
// remove the device if it is wrong, not a wrong pixel:
//   1. A compute pipeline bound via ctx.setPipeline gets the SAME device per-frame constants
//      (gInvViewProjRel/gCamPos/gSceneViewport, kEngineFrameConstantRegister) a graphics one does:
//      D3D12ResourceFactory::rootSignature reserves a root CBV parameter for every constant slot a
//      PipelineLayout does not claim as root constants, unconditionally, for BOTH compute and
//      graphics layouts (the loop building `params[]` has no `if (mesh)`/`if (compute)` gate at
//      all), and D3D12RenderContext::setPipeline's bindDeclaredRootCbvs() fills that slot from
//      dev_->frameCBs_ on every setPipeline call, compute or graphics alike.
//   2. Switching back to the graphics PSO for Stage B is not "whatever compute left behind": setPipeline
//      reconfigures the whole root signature and calls bindDeclaredRootCbvs() again, and this function
//      re-issues setBindingSet/setBindlessTable/setConstantBuffer for Stage B exactly as the
//      single-pass code does for its one draw -- so Stage B's root bindings are freshly set, not
//      inherited from any compute dispatch.
//   3. D3D12RenderContext::dispatch is `cmdList_->Dispatch(gx, gy, gz)` with no render-target-state
//      precondition -- Dispatch does not touch OMSetRenderTargets/RSSetViewports at all, so recording
//      it between draws while the scene's colour/depth targets stay bound is legal.
//   4. uavBarrierBuffer/uavBarrierTexture emit a plain D3D12_RESOURCE_BARRIER_TYPE_UAV against the
//      resource -- the correct and sufficient barrier between a compute UAV write and a later UAV
//      read of the SAME resource in a different shader stage. No resource-state transition is needed
//      here specifically because gRdVisBuf/gRdSunVisTex/gRdGiTex/gRdAoTex/gRdReflTex stay in
//      UnorderedAccess and are read back through the SAME UAV registers in PSRayDriven's AVER_RD_SPLIT
//      branch (contract: no SRV slot exists for any of them) -- never through an SRV, which is the
//      only case that would need one.
//
// NO BARRIER, NO TIMESTAMP, BETWEEN CSRdShadow/CSRdGi/CSRdSkyOcc/CSRdRefl: shadow (u2/u12), GI
// (u6-u10/u13) and sky occlusion (u4/u5/u14) write DISJOINT resources per milestone 2's own contract.
// CSRdRefl (u3/u15, milestone 3's contract -- reflection history via rtReflectionTemporal plus its own
// gRdReflTex) IS ASSUMED DISJOINT FROM ALL THREE ON THE SAME CONTRACT, not independently reverified
// here: this file only schedules the dispatch, and the shader side of the milestone 3 contract is what
// states rtReflectionTemporal/rtReflection/rtReprojectReflection touch no other group member's UAVs.
// All four read only the visibility record (already fenced by the uavBarrierBuffer right before this
// group), last frame's own histories, and this frame's NRD outputs (produced earlier in prePass). A
// GPU timestamp dropped BETWEEN two dispatches can make some GPUs drain the pipe to take the reading,
// serialising work that would otherwise overlap -- which is exactly why all four sit inside ONE
// ScopedGpuStat ("Voxi RD lighting stages") rather than each opening its own span the way
// CSRdVisibility above and the Stage B draw below still do.
//
// CSRdLocalLights (LOCAL LIGHTS, dispatched right after CSRdShadow) JOINS THE SAME BARRIER-FREE GROUP ON
// THE SAME CONTRACT: it writes u19 alone, and reads only the fenced visibility record, the light list
// (t18), LAST frame's histories (t19, t6) and the TLAS -- never CSRdShadow's own this-frame outputs
// (u2/u12). A shader change that reads either of those needs a barrier here first.
//
// SUB-STAGE SPLITS (Settings::rayDrivenShadowTiles / rayDrivenGiSplit) ARE THE ONE EXCEPTION TO "NO
// BARRIER" ABOVE: S1 (CSRdShadowProbe) and G1 (CSRdGiTrace) run FIRST, inside the same span, and
// their own outputs (gRdShadowTiles/gRdGiCand) get an explicit uavBarrierBuffer before CSRdShadow's
// tiled branch or CSRdGi's split branch reads them back a few lines later -- these are NOT disjoint
// resources the way the rest of the group's argument above relies on, so the barrier-free reasoning
// does not extend to them, and the barrier is recorded unconditionally, whether or not
// voxi.rayDrivenStageTiming is on.
void VoxiRenderer::recordStagedRayDriven(rhi::IRenderContext& ctx) {
    const bool gbufBound = pickGbuf(rayDrivenPso_, rayDrivenGbufPso_) == rayDrivenGbufPso_;
    const rhi::PipelineHandle stageBPso = gbufBound ? rayDrivenSplitTexGbufPso_ : rayDrivenSplitTexPso_;

    // FROM THE SCENE VIEWPORT, NOT THE FULL RENDER TARGET -- curSceneViewport_ is this frame's {x, y,
    // w, h} in target pixels (IDevice::sceneViewport, read fresh every frame in beginShadowHistory,
    // which prePass() already calls before scenePass() runs). Dispatching over the whole render
    // target would trace rays for letterboxed pixels no draw ever covers; the compute shaders
    // themselves early-out any thread outside gSceneViewport.zw for the same reason. rdVisBuf_/
    // rdSunVisTex_/rdGiTex_/rdAoTex_/rdReflTex_ are still sized to the FULL render target
    // (rdStagedRowPitch_), because i.pos.xy in Stage B is a render-target-space pixel centre, not a
    // viewport-local one.
    const u32 dispatchW = curSceneViewport_[2] > 0.0f ? static_cast<u32>(curSceneViewport_[2]) : 0u;
    const u32 dispatchH = curSceneViewport_[3] > 0.0f ? static_cast<u32>(curSceneViewport_[3]) : 0u;
    const u32 gx = (dispatchW + 7u) / 8u;   // every staged compute stage declares [numthreads(8,8,1)]
    const u32 gy = (dispatchH + 7u) / 8u;

    // THE ROW PITCH RIDES viewParams.w FOR THESE UPLOADS ONLY, and goes back to 0 after them.
    // Written here rather than in prePass because this is the one place that has already decided to
    // record the staged passes: a prePass copy was computed before beginShadowHistory refreshes
    // curSceneViewport_, so the two could disagree on a frame the view appeared or went away. No
    // other shader reads gViewParams.w (it was spare), so the single pass never sees a nonzero value.
    cb_.viewParams[3] = static_cast<f32>(rdStagedRowPitch_);

    // LOCAL LIGHTS (LAMPS): decided HERE, before the first upload, because more than one stage reads
    // the count -- CSRdGi/CSRdGiTrace leave a lamp's own emission out of ReSTIR's candidate hits while
    // it is non-zero (the lamp is lit directly instead), CSRdLocalLights and Stage B act on it. Raised
    // for some of those uploads and not others, a lamp would count twice or not at all. prePass zeroed
    // both fields for every upload before this one; they stay as decided here through the blended
    // replay. Staged adds one requirement to the others: CSRdLocalLights itself compiled.
    const bool localLights = publishLocalLights(localLightsReady() && rdLocalLightsCsPso_ != 0 && gx && gy,
                                                "CSRdLocalLights (GPU span 'Voxi RD local lights' under "
                                                "voxi.rayDrivenStageTiming)");

    {
        rhi::ScopedGpuStat stat(ctx, "Voxi RD visibility");
        ctx.setPipeline(rdVisCsPso_);
        ctx.setBindingSet(bindings_);
        ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
        ctx.setBindlessTable(rtTexTable_);
        ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
        if (gx && gy) ctx.dispatch(gx, gy, 1);
    }
    ctx.uavBarrierBuffer(rdVisBuf_);   // Stage S's read of gRdVisBuf must see Stage A's writes

    // voxi.rayDrivenStageTiming (DIAGNOSTIC, Settings::rayDrivenStageTiming): each stage below gets
    // its own span and a UAV barrier on its own output before the span closes, so a span holds that
    // stage alone. Off (the default), the one shared span and the barrier-free overlap are exactly as
    // this function's own comment describes; the push/pop pairs below are then no-ops.
    const bool perStage = settings_.rayDrivenStageTiming;
    auto stageBegin = [&](const char* label) { if (perStage) ctx.pushMarker(label); };
    auto stageEnd = [&](rhi::TextureHandle written) {
        if (!perStage) return;
        ctx.uavBarrierTexture(written);
        ctx.popMarker();
    };
    // THE BUFFER OVERLOAD S1/G1 (below) NEED: identical shape, uavBarrierBuffer in place of
    // uavBarrierTexture, since gRdShadowTiles/gRdGiCand are StructuredBuffers, not textures.
    auto stageEndBuffer = [&](rhi::BufferHandle written) {
        if (!perStage) return;
        ctx.uavBarrierBuffer(written);
        ctx.popMarker();
    };

    // reflSplit: SUB-STAGE C (Settings::rayDrivenReflSplit), the same shape shadowTiles/giSplit below
    // use -- the reflection dispatch's OWN condition (mirrored a few lines below, inside the lighting-
    // stages block, where the actual R1 dispatch picks rdReflSplitCsPso_ over rdReflCsPso_) plus the
    // setting plus both new reflection pipelines. A failed compile of either one falls back to the
    // unsplit CSRdRefl, never to the single-pass primary (rdStagedActive() never inspects either member
    // -- see their own header comment). DECLARED HERE, OUTSIDE the lighting-stages block below, unlike
    // shadowTiles/giSplit: R2 (CSRdReflFilter) has to run AFTER that block's own barriers, once it has
    // closed, so reflSplit has to outlive it too -- shadowTiles/giSplit stay block-scoped because
    // nothing outside that block ever reads them.
    const bool reflSplit = (cb_.shadowParams[2] > 0.5f && cb_.rtParams[3] > 0.5f) &&
                            settings_.rayDrivenReflSplit && rdReflSplitCsPso_ != 0 && rdReflFilterCsPso_ != 0;
    if (reflSplit && !rdReflSplitRunLogged_) {
        rdReflSplitRunLogged_ = true;
        AVER_INFO("[Voxi] reflection register/filter split running: CSRdRefl (R1) traces the "
                  "reflection ray alone, CSRdReflFilter (R2) runs rtReflectionSpatial's history "
                  "gather in its own pass");
    } else if (cb_.shadowParams[2] > 0.5f && cb_.rtParams[3] > 0.5f && settings_.rayDrivenReflSplit &&
               !reflSplit && !rdReflSplitFallbackLogged_) {
        rdReflSplitFallbackLogged_ = true;
        AVER_WARN("[Voxi] voxi.rayDrivenReflSplit requested the reflection register/filter split, "
                  "but the split or filter pipeline did not compile; behaving as the unsplit "
                  "CSRdRefl (said once)");
    }

    // T4 (Settings::rtGiHitShadowMap): the GI candidate's hit samples the GI-only shadow map from the
    // COMPUTE stages below (CSRdGiTrace, or CSRdGi when unsplit), but giShadowPass leaves it in
    // ShaderResource, the pixel-shader state the voxelise pass reads it in. It visits
    // NonPixelShaderResource for this group only and goes straight back afterwards, so every pixel-
    // shader reader (next frame's voxelise, the blended replay) still finds it where it expects.
    const bool giHitShadowMap = settings_.rtGiHitShadowMap && giShadowTex_ != 0;
    if (giHitShadowMap)
        ctx.textureBarrier(giShadowTex_, rhi::ResourceState::ShaderResource,
                           rhi::ResourceState::NonPixelShaderResource);
    // THE SUN SHADOW HISTORY'S READ SIDE (t6), the same visit for the same reason: beginShadowHistory
    // leaves it in ShaderResource (D3D12 PIXEL_SHADER_RESOURCE only), but CSRdShadow's reprojection and
    // CSRdLocalLights' (rtReprojectTexel) read it from COMPUTE -- the state bug 21524cd3 fixed for the
    // post chain. Back to ShaderResource after the group, where beginShadowHistory expects it next frame.
    // rtHistWriteIdx_, NOT 1 - rtHistWriteIdx_: endShadowHistory() already flipped the index at the end of
    // prePass, so the side bound at t6 THIS frame is the one the index now names; the other one is u2,
    // sitting in UnorderedAccess (the same flip giNrmWrite's comment below describes).
    const rhi::TextureHandle shadowHistRead = rtShadowHist_[rtHistWriteIdx_];
    if (shadowHistRead)
        ctx.textureBarrier(shadowHistRead, rhi::ResourceState::ShaderResource,
                           rhi::ResourceState::NonPixelShaderResource);
    // LOCAL LIGHTS (LAMPS): t19, the same visit for the same reason -- it rests in ShaderResource because
    // the raster and single-pass scene passes read it from PIXEL shaders, while CSRdLocalLights reads it
    // from compute. The read side is simply the half of the pair u19 (rdLocalOutThisFrame_) is not.
    // Only when that dispatch runs; nothing else in this group reads t19.
    const rhi::TextureHandle localHistRead =
        localLights ? (rdLocalOutThisFrame_ == rdLocalHist_[0] ? rdLocalHist_[1] : rdLocalHist_[0]) : 0;
    if (localHistRead)
        ctx.textureBarrier(localHistRead, rhi::ResourceState::ShaderResource,
                           rhi::ResourceState::NonPixelShaderResource);
    {
        // Wraps every dispatch below -- see this function's own comment on why no barrier or
        // timestamp sits between the four milestone-2/3 stages (S1/G1, immediately below, are the one
        // exception -- see that same comment's own trailing paragraph).
        if (!perStage) ctx.pushMarker("Voxi RD lighting stages");

        // CSRdGi's OWN dispatch decision, HOISTED ABOVE THE SHADOW STAGE (it used to sit between the
        // shadow and GI dispatches): SUB-STAGE SPLIT B's own trace pass (G1, below) needs giCb before
        // the shadow stage even runs, since G1 is now recorded ahead of CSRdShadow. The CPU mirror of
        // PSRayDriven's own GI-block condition and CSRdGi's own body gate (voxi.hlsl) -- ReSTIR GI is
        // the chosen estimator AND the cone trace is gated on. Reads the SAME cb_ fields the shader
        // tests this frame, never a separately-tracked flag, so the two can never disagree.
        // rdStagedActive() already refused this frame if the condition holds but rdGiCsPso_ is 0, so
        // reaching here with the condition true means the pipeline exists.
        const bool giDispatch = cb_.voxelParams[3] > 0.5f && cb_.giRestirParams[0] > 0.5f;
        // MILESTONE 4: half-rate ReSTIR GI via NRD's checkerboard, gated on three things beyond
        // giDispatch itself -- the setting actually asking for it (rayDrivenStages == 2; rdStagedWanted()
        // now treats 1 and 2 alike, so this is the one place that still tells them apart), the
        // checkerboard variant having compiled at all (rdGiCbCsPso_ is OPTIONAL on top of an already-
        // optional pipeline, see its own header comment), and THIS frame's NRD readback actually being
        // live (nrdGiRanThisFrame_, latched by beginShadowHistory earlier this same frame) -- tracing
        // only half the pixels is only safe when REBLUR has something real to reconstruct the other
        // half from; see nrdGiRanThisFrame_'s own comment for why.
        const bool giCb = giDispatch && settings_.rayDrivenStages == 2u && rdGiCbCsPso_ != 0 &&
                          nrdGiRanThisFrame_;
        // Recorded unconditionally -- false whenever giDispatch itself is false, since giCb already
        // implies it -- and consumed at the very top of NEXT frame's beginShadowHistory to tell that
        // frame's NRD dispatch whether the GI input it is about to denoise was written checkerboarded.
        giCbWrittenThisFrame_ = giCb;
        // ONE-SHOT LOGS, so half-rate GI's on/off state is visible to whoever is testing
        // voxi.rayDrivenStages 2 without needing per-frame spam. Mutually exclusive: exactly one of
        // these two conditions can hold in a given frame (giCb requires rayDrivenStages == 2, and the
        // "why not" branch only fires when rayDrivenStages == 2 and giCb is false).
        if (giCb && !rdGiCbRunLogged_) {
            rdGiCbRunLogged_ = true;
            AVER_INFO("[Voxi] half-rate ReSTIR GI running: CSRdGi traces NRD's checkerboard half each "
                      "frame, REBLUR reconstructs the rest");
        } else if (settings_.rayDrivenStages == 2u && !giCb && !rdGiCbFallbackLogged_) {
            rdGiCbFallbackLogged_ = true;
            const char* why =
                !giDispatch ? "ReSTIR GI is not the diffuse estimator this frame"
                : !nrdGiRanThisFrame_ ? "the NRD GI denoiser did not run this frame (denoiser off, or "
                                        "NRD unavailable)"
                                      : "the checkerboard CSRdGi variant did not compile";
            AVER_INFO("[Voxi] voxi.rayDrivenStages 2 requested half-rate ReSTIR GI, but {}; behaving as "
                      "rayDrivenStages 1 for the GI stage (said once)", why);
        }

        // ---- SUB-STAGE SPLITS (Settings::rayDrivenShadowTiles / rayDrivenGiSplit) ----
        //
        // shadowTiles: the setting plus BOTH new shadow pipelines -- a failed compile of either one
        // falls back to the unsplit CSRdShadow below, never to the single-pass primary (rdStagedActive()
        // never inspects these two members -- see their own header comment).
        const bool shadowTiles = settings_.rayDrivenShadowTiles && rdShadowProbeCsPso_ && rdShadowTiledCsPso_;
        // giSplit: giDispatch (nothing to split when CSRdGi itself would not run this frame) plus the
        // setting plus whichever plain/checkerboard trace+split PAIR this frame's giCb decision needs
        // -- the checkerboard trace pass is useless without the checkerboard split pass to read it and
        // vice versa, so both members of the pair are required together.
        const bool giSplit = giDispatch && settings_.rayDrivenGiSplit &&
                              (giCb ? (rdGiTraceCbCsPso_ && rdGiSplitCbCsPso_)
                                    : (rdGiTraceCsPso_ && rdGiSplitCsPso_));
        // ONE-SHOT LOGS, the identical "said once, each half of the story" shape the half-rate GI pair
        // immediately above already uses.
        if (shadowTiles && !rdShadowTilesRunLogged_) {
            rdShadowTilesRunLogged_ = true;
            AVER_INFO("[Voxi] shadow probe/tile split running: CSRdShadowProbe traces one ray per 8x8 "
                      "tile, CSRdShadow skips its own ray wherever its tile's 3x3 neighbourhood agrees");
        } else if (settings_.rayDrivenShadowTiles && !shadowTiles && !rdShadowTilesFallbackLogged_) {
            rdShadowTilesFallbackLogged_ = true;
            AVER_WARN("[Voxi] voxi.rayDrivenShadowTiles requested the shadow probe/tile split, but the "
                      "probe or tiled shadow pipeline did not compile; behaving as the unsplit "
                      "CSRdShadow (said once)");
        }
        if (giSplit && !rdGiSplitRunLogged_) {
            rdGiSplitRunLogged_ = true;
            AVER_INFO("[Voxi] GI candidate-trace split running: CSRdGiTrace traces the fresh ReSTIR GI "
                      "candidate, CSRdGi resamples/shades from the stored result");
        } else if (giDispatch && settings_.rayDrivenGiSplit && !giSplit && !rdGiSplitFallbackLogged_) {
            rdGiSplitFallbackLogged_ = true;
            AVER_WARN("[Voxi] voxi.rayDrivenGiSplit requested the GI candidate-trace split, but the {} "
                      "GI-trace/split pipeline pair did not compile; behaving as the unsplit CSRdGi "
                      "(said once)", giCb ? "checkerboard" : "plain");
        }

        // S1: CSRdShadowProbe, one 8x8-tile group per probe, dispatched over the SAME (gx, gy) as the
        // shadow stage below -- writes gRdShadowTiles (u18).
        if (shadowTiles) {
            stageBegin("Voxi RD shadow probe stage");
            ctx.setPipeline(rdShadowProbeCsPso_);
            ctx.setBindingSet(bindings_);
            ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
            ctx.setBindlessTable(rtTexTable_);
            ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
            if (gx && gy) ctx.dispatch(gx, gy, 1);
            stageEndBuffer(rdShadowTileBuf_);
        }
        // G1: CSRdGiTrace, writing gRdGiCand (u17) -- COMPACTED to the traced half's pixels alone in
        // checkerboard mode (giCb), the identical parity bit CSRdGi's own checkerboard dispatch below
        // carries. Copied here rather than shared: this upload happens strictly before that one, and
        // both restore the plain pitch immediately after so no later upload this frame ever sees it.
        if (giSplit) {
            stageBegin("Voxi RD GI trace stage");
            ctx.setPipeline(giCb ? rdGiTraceCbCsPso_ : rdGiTraceCsPso_);
            ctx.setBindingSet(bindings_);
            ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
            ctx.setBindlessTable(rtTexTable_);
            u32 g1x = gx;
            if (giCb) {
                // HALF THE COLUMNS, ROUNDED UP, NOT HALF THE THREADS: the compacted dispatch covers
                // ceil(dispatchW/2) columns of 8x8 groups, each thread handling ONE of the traced
                // half's pixels (see CSRdGiTrace's own x = xBase + ((xBase ^ y ^ parity) & 1u) decode).
                g1x = ((dispatchW + 1u) / 2u + 7u) / 8u;
                cb_.viewParams[3] = static_cast<f32>(rdStagedRowPitch_ | ((nrdFrame_ & 1u) << 16));
                ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
                cb_.viewParams[3] = static_cast<f32>(rdStagedRowPitch_);   // restore before any later upload
            } else {
                ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
            }
            if (g1x && gy) ctx.dispatch(g1x, gy, 1);
            stageEndBuffer(rdGiCandBuf_);
        }
        // THE BARRIER BETWEEN S1/G1 AND THEIR OWN CONSUMERS, UNCONDITIONALLY -- needed whether or not
        // voxi.rayDrivenStageTiming is on, unlike stageEnd/stageEndBuffer's own barriers (which only
        // fire under perStage): CSRdShadow's tiled branch and CSRdGi's split branch, a few lines below,
        // read gRdShadowTiles/gRdGiCand back inside the SAME barrier-free group this function's own
        // header comment says the rest of the milestone-2/3 stages share -- but S1/G1's outputs are
        // NOT disjoint from their readers the way that reasoning requires, so a real barrier replaces
        // it here instead.
        if (shadowTiles) ctx.uavBarrierBuffer(rdShadowTileBuf_);
        if (giSplit) ctx.uavBarrierBuffer(rdGiCandBuf_);

        stageBegin("Voxi RD shadow stage");
        ctx.setPipeline(shadowTiles ? rdShadowTiledCsPso_ : rdShadowCsPso_);
        ctx.setBindingSet(bindings_);
        ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
        ctx.setBindlessTable(rtTexTable_);
        ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
        if (gx && gy) ctx.dispatch(gx, gy, 1);
        stageEnd(rdSunVisTex_);

        // LOCAL LIGHTS (LAMPS): CSRdLocalLights, writing u19 -- see this function's own comment for why it
        // needs no barrier against the rest of the group. Skipped outright with no lamps, so a scene
        // without any pays nothing. The history is marked written only here, where it actually is.
        if (localLights) {
            stageBegin("Voxi RD local lights");
            ctx.setPipeline(rdLocalLightsCsPso_);
            ctx.setBindingSet(bindings_);
            ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
            ctx.setBindlessTable(rtTexTable_);
            ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
            ctx.dispatch(gx, gy, 1);
            stageEnd(rdLocalOutThisFrame_);
            rdLocalHistFrame_ = rtFrameIndex_;
            rdLocalHistHash_ = rdLocalLightHash_;
        }

        if (giDispatch) {
            stageBegin("Voxi RD GI stage");
            ctx.setPipeline(giSplit ? (giCb ? rdGiSplitCbCsPso_ : rdGiSplitCsPso_)
                                     : (giCb ? rdGiCbCsPso_     : rdGiCsPso_));
            ctx.setBindingSet(bindings_);
            ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
            ctx.setBindlessTable(rtTexTable_);
            if (giCb) {
                // BIT 16 CARRIES THE CHECKERBOARD PARITY, FOR THIS ONE UPLOAD ONLY. Every shader's own
                // decode of gViewParams.w masks with & 0xFFFFu (see rdStagedRowPitch_'s own comment),
                // so CSRdVisibility/CSRdShadow/CSRdSkyOcc/CSRdRefl/Stage B below never see bit 16 --
                // setConstantBuffer copies cb_ at call time, and the plain pitch is restored
                // immediately below, before any of those later uploads.
                //
                // PARITY = nrdFrame_ & 1u: beginShadowHistory's fs.frameIndex = nrdFrame_++, earlier
                // this same frame, has already post-incremented nrdFrame_ once, so the CURRENT value is
                // exactly the frameIndex NEXT frame's NRD dispatch will use to denoise the write this
                // dispatch is about to make -- the two agree by construction, not by convention.
                //
                // The pitch always fits the low 16 bits: it is the render target's width, and D3D12
                // caps a Texture2D at 16384 texels a side (the staged path is D3D12-only).
                cb_.viewParams[3] = static_cast<f32>(rdStagedRowPitch_ | ((nrdFrame_ & 1u) << 16));
                ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
                cb_.viewParams[3] = static_cast<f32>(rdStagedRowPitch_);   // restore before any later upload
            } else {
                ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
            }
            if (gx && gy) ctx.dispatch(gx, gy, 1);
            stageEnd(rdGiTex_);
        }

        // CSRdSkyOcc: the CPU mirror of PSRayDriven's own sky-occlusion condition -- the cases where
        // rdAo would otherwise still read its initial 1.0 and rdAoGathered stays false, because ReSTIR
        // supplied the diffuse term instead of the cone gather, or there is no voxel GI running at
        // all. Cone-GI mode's own sky occlusion depends on the cone gather and stays in the shade
        // pass, so it never reaches this dispatch.
        if (cb_.ambientParams[0] > 0.5f &&
            (cb_.giRestirParams[0] > 0.5f || cb_.voxelParams[3] <= 0.5f)) {
            stageBegin("Voxi RD sky occlusion stage");
            ctx.setPipeline(rdSkyOccCsPso_);
            ctx.setBindingSet(bindings_);
            ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
            ctx.setBindlessTable(rtTexTable_);
            ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
            if (gx && gy) ctx.dispatch(gx, gy, 1);
            stageEnd(rdAoTex_);
        }

        // MILESTONE 3: CSRdRefl, the CPU mirror of PSRayDriven's own reflection-block condition's
        // reflections-enabled half -- ray tracing is active and the geometry table built this frame.
        // The per-pixel roughness half of the shader's own gate (`s.rough <= 0.75`) has no CPU-side
        // equivalent and stays entirely inside CSRdRefl, which is why gRdReflTex carries its own alpha
        // decision for Stage B to read rather than this condition trying to predict it. Reads the SAME
        // cb_ fields the shader tests this frame, never a separately-tracked flag, so the two can never
        // disagree. rdStagedActive() already refused this frame if the condition holds but rdReflCsPso_
        // is 0, so reaching here with the condition true means the pipeline exists.
        if (cb_.shadowParams[2] > 0.5f && cb_.rtParams[3] > 0.5f) {
            stageBegin("Voxi RD reflection stage");
            ctx.setPipeline(reflSplit ? rdReflSplitCsPso_ : rdReflCsPso_);
            ctx.setBindingSet(bindings_);
            ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
            ctx.setBindlessTable(rtTexTable_);
            ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
            if (gx && gy) ctx.dispatch(gx, gy, 1);
            stageEnd(rdReflTex_);
        }
        if (!perStage) ctx.popMarker();
    }
    if (giHitShadowMap)
        ctx.textureBarrier(giShadowTex_, rhi::ResourceState::NonPixelShaderResource,
                           rhi::ResourceState::ShaderResource);
    if (shadowHistRead)
        ctx.textureBarrier(shadowHistRead, rhi::ResourceState::NonPixelShaderResource,
                           rhi::ResourceState::ShaderResource);
    if (localHistRead)
        ctx.textureBarrier(localHistRead, rhi::ResourceState::NonPixelShaderResource,
                           rhi::ResourceState::ShaderResource);
    // Stage B's reads of gRdSunVisTex/gRdGiTex/gRdAoTex/gRdReflTex must see whichever of the four
    // dispatches above wrote them -- all four barriers sit here, unconditionally, rather than only
    // behind each dispatch's own `if`: a barrier against a texture nothing wrote this frame is a
    // harmless no-op (the resource is already in UnorderedAccess, see point 4 above), while
    // conditioning the barrier on the SAME test the dispatch used would duplicate that test for no
    // safety this already has.
    ctx.uavBarrierTexture(rdSunVisTex_);
    ctx.uavBarrierTexture(rdGiTex_);
    ctx.uavBarrierTexture(rdAoTex_);
    ctx.uavBarrierTexture(rdReflTex_);
    // LOCAL LIGHTS (LAMPS): Stage B (and after it the blended replay's reuse) reads gRdLocalOut through u19
    // the way it reads u12 above, so it gets the same barrier -- conditional only because u19 may be a
    // placeholder (or nothing real) on a frame without lamps, when neither reads it (the count is 0).
    if (localLights) ctx.uavBarrierTexture(rdLocalOutThisFrame_);
    // THE GI SURFACE-NORMAL HISTORY (u8) HAS TWO WRITERS NOW: CSRdGi (hit pixels, inside
    // giRestirIndirect) and Stage B's own miss branch (the sky sentinel). The texels are disjoint and
    // the barriers above already drain the GPU in practice, but the order between a dispatch and a
    // draw writing one UAV is stated here rather than left to that side effect.
    // 1 - rtHistWriteIdx_, NOT rtHistWriteIdx_: endShadowHistory() already flipped the index at the end
    // of prePass, so by scenePass the texture bound at u8 THIS frame is the other one. (Staged mode
    // requires shadowHistoryActive(), so that flip always happened.)
    const u32 giNrmWrite = 1u - rtHistWriteIdx_;
    if (cb_.voxelParams[3] > 0.5f && cb_.giRestirParams[0] > 0.5f && giSurfNrmHist_[giNrmWrite])
        ctx.uavBarrierTexture(giSurfNrmHist_[giNrmWrite]);

    // SUB-STAGE C, R2: CSRdReflFilter -- reruns rtReflectionSpatial against R1's own gRtReflHistOut
    // write from a few lines above and finishes the compose R1 deferred. Recorded OUTSIDE the
    // barrier-free "Voxi RD lighting stages" group (already popped above): R2 depends on R1's write to
    // the SAME resource, not a disjoint one, so it needs a real barrier rather than the group's
    // no-barrier reasoning, and gets its own ALWAYS-ON span -- the shared marker every other lighting
    // stage rides is already closed by the time this dispatch runs, the identical reason "Voxi RD
    // visibility" (Stage A/S, above) and "Voxi ray-driven primary" (Stage B, just below) each open their
    // own span rather than borrowing stageBegin/stageEnd's voxi.rayDrivenStageTiming-gated pair.
    if (reflSplit) {
        // 1u - rtHistWriteIdx_, NOT rtHistWriteIdx_: the SAME reasoning giNrmWrite's own comment just
        // above gives for u8 applies identically to u3 -- endShadowHistory() already flipped the index
        // at the end of prePass, so by scenePass the reflection history texture R1 wrote THIS frame
        // (bound at u3 by beginShadowHistory, see that function's own setUav(bindings_, 3, ...)) is the
        // other one. Barriers R1's write against R2's read of the identical resource through gRtReflHistOut
        // (the same u3 register, voxi.hlsl).
        ctx.uavBarrierTexture(rtReflHist_[1u - rtHistWriteIdx_]);
        {
            rhi::ScopedGpuStat stat(ctx, "Voxi RD reflection filter");
            ctx.setPipeline(rdReflFilterCsPso_);
            ctx.setBindingSet(bindings_);
            ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
            ctx.setBindlessTable(rtTexTable_);
            ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
            if (gx && gy) ctx.dispatch(gx, gy, 1);
        }
        ctx.uavBarrierTexture(rdReflTex_);   // R2's own write, before Stage B reads gRdReflTex
    }

    {
        rhi::ScopedGpuStat stat(ctx, "Voxi ray-driven primary");   // same span name the single pass
                                                                    // uses, so an A/B across the switch
                                                                    // is a like-for-like GPU-time
                                                                    // comparison of just this draw.
        ctx.setPipeline(stageBPso);
        ctx.setBindingSet(bindings_);
        ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
        ctx.setBindlessTable(rtTexTable_);
        ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
        ctx.drawFullscreen();
    }
    // setConstantBuffer copied the pitch at each call above, so the pitch goes. What stays, on a frame
    // CSRdGi wrote NRD's input checkerboarded, is bit 17 plus the parity in bit 16, for the REST of this
    // frame: the scene draws after this one (PSMainVoxi's blended replay) read cb_ through
    // sceneConstants(), and giRestirIndirect's own NRD-input write uses it to follow the packed layout
    // instead of landing on another pixel's texel. prePass zeroes the field again next frame.
    cb_.viewParams[3] = giCbWrittenThisFrame_
                      ? static_cast<f32>((1u << 17) | ((nrdFrame_ & 1u) << 16))
                      : 0.0f;
    // LOCAL LIGHTS (LAMPS): cameraMedium z/w are deliberately NOT reset here. The blended replay lights
    // its panes with the same lamps (PSMainVoxi's blended term, reading this frame's staged visibility
    // through u19 where bit 16 below proves it sits on the lit surface), so its ReSTIR GI must drop a
    // lamp's emission exactly as the opaque stages did.

    // BIT 16, SET LAST: tells the blended replay (D3D12Device::endFrame, which picks cb_ up through
    // sceneConstants() after scenePass returns) that gRdSunVisTex/gRdGiTex/gRdAoTex/gRdReflTex hold
    // THIS frame's values, so a translucent pixel sitting on the opaque surface the staged passes
    // already lit may reuse them instead of paying its own rays -- see
    // Settings::blendedReuseStagedLighting's own comment for the shape of that reuse. SELF-CLEARING:
    // this is the ONLY place that sets the bit, and prePass (top of this frame, before this function
    // even runs) already repacked giShadowParams[3] from scratch with it absent (bits 1/2/4/8 only,
    // see that assignment's own comment) -- so a frame that does not reach this line, because
    // rdStagedActive() was false, carries prePass's 0 straight through to the replay untouched. Keep
    // it that way: the bit must never be set anywhere else, or a frame that falls back from staged to
    // single-pass mid-frame could leave it on over textures nothing wrote this frame.
    if (settings_.blendedReuseStagedLighting) {
        cb_.giShadowParams[3] = static_cast<f32>(static_cast<u32>(cb_.giShadowParams[3]) | 16u);
    }
}

// Rebuilds the pipelines that bake the sample count and the target formats, and resizes the
// ray-traced shadow history to match the new resolution.
void VoxiRenderer::onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth,
                                          u32 width, u32 height) {
    if (!res_ || !giReady_) return;
    if (!createScenePipelines(sampleCount, color, depth))
        AVER_ERROR("[Voxi] scene pipelines could not be rebuilt for {} sample(s)", sampleCount);
    scenePipelineGraphRev_ = pbr::materialGraphs().revision();
    scenePipelineShaderRev_ = rhi::shaderFileRevision();   // same reason as the line above
    // Remembered even when the call below decides to allocate nothing: setSettings needs a size to
    // create at if ray tracing is switched on later, and it is never told one.
    rtHistWantW_ = width;
    rtHistWantH_ = height;
    if (!ensureShadowHistory(width, height))
        AVER_ERROR("[Voxi] ray-traced shadow history could not be (re)created at {}x{}", width, height);
    if (!ensureRdStagedResources(width, height))
        AVER_ERROR("[Voxi] staged ray-driven resources could not be (re)created at {}x{}", width, height);
}

// LOCAL LIGHTS (LAMPS): points t19/u19 back at the placeholder and THEN destroys the pair -- a
// descriptor must never outlive the texture it names (aver-view-outlives-its-buffer) -- and drops
// every flag that described the pair's state or contents: unprimed, a recreated pair is taken to rest in
// its creation state (ShaderResource, see ensureShadowHistory), and no history is trusted. Idempotent.
void VoxiRenderer::releaseLocalHistory() {
    if (res_ && bindings_ && rdLocalHistPlaceholder_ && (rdLocalHist_[0] || rdLocalHist_[1])) {
        res_->setSrv(bindings_, 19, rdLocalHistPlaceholder_);
        res_->setUav(bindings_, 19, rdLocalHistPlaceholder_, 0);
    }
    for (rhi::TextureHandle& t : rdLocalHist_) { if (t && res_) res_->destroyTexture(t); t = 0; }
    rdLocalOutThisFrame_ = 0;
    rdLocalHistPrimed_ = false;
    rdLocalHistFrame_ = 0;
}

// (Re)creates the ray-traced shadow AND reflection histories at the given resolution. All four
// textures are destroyed and rebuilt together: one at the wrong size with another right would
// corrupt reprojection, and the two pairs share rtHistWriteIdx_/rtHistValid_ so they must always
// agree on whether a previous frame's contents exist at all.
bool VoxiRenderer::ensureShadowHistory(u32 width, u32 height) {
    if (!res_ || !bindings_ || width == 0 || height == 0) return false;

    // NOTHING AT ALL WHEN RAY TRACING IS OFF, the shipped default: these four textures exist solely
    // to carry a previous frame's traced visibility/reflection colour, unused at Quality::Off. Used
    // to allocate regardless (the only gate was `giReady_`) -- ~225 MB of VRAM at this machine's
    // 3532x1987, held for a feature that never runs. Teardown rather than a bare early-out, so
    // switching ray tracing OFF at runtime gives the memory back instead of stranding it.
    if (!rayTracingWanted()) {
        const bool had = rtShadowHist_[0] || rtReflHist_[0] || rtAoHist_[0] || giSurfPosHist_[0] ||
                          giVisHist_[0] || rdLocalHist_[0];
        // LOCAL LIGHTS (LAMPS): rdLocalHistWanted() requires rayTracingWanted(), so the pair goes too.
        releaseLocalHistory();
        for (rhi::TextureHandle& t : rtShadowHist_)  { if (t) res_->destroyTexture(t); t = 0; }
        for (rhi::TextureHandle& t : rtReflHist_)    { if (t) res_->destroyTexture(t); t = 0; }
        for (rhi::TextureHandle& t : rtAoHist_)      { if (t) res_->destroyTexture(t); t = 0; }
        for (rhi::TextureHandle& t : giSurfPosHist_) { if (t) res_->destroyTexture(t); t = 0; }
        for (rhi::TextureHandle& t : giSurfNrmHist_) { if (t) res_->destroyTexture(t); t = 0; }
        // U1/2.11: giVisHistWanted() REQUIRES giRestirWanted(), which itself requires
        // rayTracingWanted() -- so this branch already implies the half-res visibility pair cannot
        // be wanted either, the identical reasoning the reservoir-buffer comment two lines below
        // gives for its own release here.
        for (rhi::TextureHandle& t : giVisHist_)     { if (t) res_->destroyTexture(t); t = 0; }
        if (giRadiance_) { res_->destroyTexture(giRadiance_); giRadiance_ = 0; }
        if (rtAoHitDist_) { res_->destroyTexture(rtAoHitDist_); rtAoHitDist_ = 0; }
        // giRestirWanted() REQUIRES rayTracingWanted(), so this branch already implies ReSTIR GI
        // cannot be running -- the reservoir buffer goes with the rest of the ray-traced state.
        if (giReservoirs_) { res_->destroyBuffer(giReservoirs_); giReservoirs_ = 0; }
        giReservoirElemCapacity_ = 0;
        giHistValid_ = false;
        giHistPrimed_ = false;
        giVisHistValid_ = false;
        giVisHistPrimed_ = false;
        rtShadowHistW_ = rtShadowHistH_ = 0;
        rtHistWriteIdx_ = 0;
        rtHistValid_ = false;
        rtHistPrimed_ = false;
        // Idempotent by construction: called from both onRenderTargetsChanged and setSettings, and
        // the handles are zeroed above, so a second call finds nothing left to destroy. Not an
        // error -- "allocated nothing because nothing needs it" is success.
        if (had) AVER_INFO("[Voxi] ray-traced history released; ray tracing is off");
        return true;
    }

    if (rtShadowHist_[0] && rtShadowHist_[1] && rtReflHist_[0] && rtReflHist_[1] &&
        // rtAoHitDist_ IS IN THIS TEST because it is what the test is FOR: this early-out says
        // "everything the current settings want already exists at the current size". The three are
        // created together, so leaving one out could only matter if that ever stopped being true --
        // and then the failure would be an early-out that skips creating it, forever, with the
        // shader told by gRtDenoiseParams.w not to write it. Silent, and permanent.
        (rtAoHist_[0] && rtAoHist_[1] && rtAoHitDist_) == aoHistoryWanted() &&
        // Same test, for the fourth/fifth pair -- see giSurfPosHist_'s own declaration for why they
        // have an independent wanted-condition (giRestirWanted()) rather than riding aoHistoryWanted()'s.
        (giSurfPosHist_[0] && giSurfPosHist_[1] && giSurfNrmHist_[0] && giSurfNrmHist_[1] &&
         giReservoirs_) == giRestirWanted() &&
        // U1/2.11: the sixth pair's own test, the identical shape -- giVisHistWanted() is a stricter
        // question than giRestirWanted() (2.9's own comment on giVisHistWanted()), so it needs its
        // own independent check here rather than riding either of the two above.
        (giVisHist_[0] && giVisHist_[1]) == giVisHistWanted() &&
        // LOCAL LIGHTS (LAMPS): the same test for rdLocalHist_, on its own wanted-condition.
        (rdLocalHist_[0] && rdLocalHist_[1]) == rdLocalHistWanted() &&
        rtShadowHistW_ == width && rtShadowHistH_ == height)
        return true;

    for (rhi::TextureHandle& t : rtShadowHist_)  { if (t) res_->destroyTexture(t); t = 0; }
    for (rhi::TextureHandle& t : rtReflHist_)    { if (t) res_->destroyTexture(t); t = 0; }
    for (rhi::TextureHandle& t : rtAoHist_)      { if (t) res_->destroyTexture(t); t = 0; }
    for (rhi::TextureHandle& t : giSurfPosHist_) { if (t) res_->destroyTexture(t); t = 0; }
    for (rhi::TextureHandle& t : giSurfNrmHist_) { if (t) res_->destroyTexture(t); t = 0; }
    // U1/2.11: the sixth pair, torn down and recreated below on the identical "resolution/wanted
    // condition changed" edge as the fourth/fifth pair immediately above -- see the early-out test's
    // own comment for why it needed a THIRD, independent test rather than riding either of theirs.
    for (rhi::TextureHandle& t : giVisHist_)     { if (t) res_->destroyTexture(t); t = 0; }
    // LOCAL LIGHTS (LAMPS): torn down with the rest and recreated below at the new size if wanted.
    releaseLocalHistory();
    if (giRadiance_) { res_->destroyTexture(giRadiance_); giRadiance_ = 0; }
    giHistValid_ = false;   // same reason rtHistValid_ two lines below is cleared: a stale resolution
    giHistPrimed_ = false;   // the pair below is about to be destroyed and recreated in ShaderResource
    giVisHistValid_ = false;   // ditto, for the sixth pair's own previous-frame contents
    giVisHistPrimed_ = false;   // ditto: recreated fresh in ShaderResource, so no read side is primed
    // THE FOURTH ONE, AND IT WAS MISSING HERE WHILE BEING PRESENT IN THE OTHER TWO TEARDOWNS --
    // exactly the shape of leak the comment at the top of shutdown() records this function having
    // had before, and for the same reason: the release paths are three separate lists and adding a
    // texture to one of them is not adding it to all three. Every viewport resize at High or Epic
    // orphaned an R16Unorm at the old resolution, unrecoverably (the handle was overwritten by the
    // create below, so not even shutdown could reach it). Dropping the tier from High to Medium at a
    // fixed size was worse than a leak: the create block is skipped when aoHistoryWanted() is false,
    // so the stale non-zero handle SURVIVED, and ambientHitDistanceTexture() went on returning it --
    // breaking its own documented "0 is the answer at a tier with no ray" contract.
    if (rtAoHitDist_) { res_->destroyTexture(rtAoHitDist_); rtAoHitDist_ = 0; }
    rtHistValid_ = false;   // the old contents, if any, belonged to a resolution that no longer exists
    rtHistPrimed_ = false;   // the pairs are about to be recreated below, fresh in ShaderResource

    rhi::TextureDesc d;
    d.dim    = rhi::TextureDim::Tex2D;
    d.width  = width;
    d.height = height;
    d.mips   = 1;
    // x = visibility, y = linear depth (view-space, centimetres) -- the depth channel is what lets
    // rtReprojectHistory tell a reprojection that lands on an already-populated texel apart from
    // one that has actually disoccluded: a silhouette edge can reproject to the SAME screen texel
    // while the surface now visible through it sits at a very different depth.
    d.format = rhi::Format::RG32Float;
    d.bind   = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::UnorderedAccess;
    d.initialState = rhi::ResourceState::ShaderResource;   // where a texture rests between frames
    d.debugName = "Voxi RT shadow history A";
    rtShadowHist_[0] = res_->createTexture(d);
    d.debugName = "Voxi RT shadow history B";
    rtShadowHist_[1] = res_->createTexture(d);
    if (!rtShadowHist_[0] || !rtShadowHist_[1]) return false;

    // rgb = the reflection's own shaded colour, a = linear depth of the hit, OR a NEGATIVE sentinel
    // meaning the ray missed -- see rtReflectionTemporal in VoxiShaders.hpp for why a miss is never
    // reprojected (sky-by-direction is cheap and view-dependent, recomputed fresh every frame).
    // RGBA16F not RGBA32F: HDR colour doesn't need full float precision, halving the bandwidth of a
    // buffer already twice the shadow one's size.
    d.format = rhi::Format::RGBA16F;
    d.debugName = "Voxi RT reflection history A";
    rtReflHist_[0] = res_->createTexture(d);
    d.debugName = "Voxi RT reflection history B";
    rtReflHist_[1] = res_->createTexture(d);
    if (!rtReflHist_[0] || !rtReflHist_[1]) return false;

    // LOCAL LIGHTS (LAMPS): the lamp visibility history, same size and ping-pong as rtShadowHist_ so
    // the shadow history's reprojection/depth test addresses it texel for texel, and resting in
    // ShaderResource like it (see rdLocalHist_'s own comment). A FAILED ALLOCATION IS NOT FATAL, the
    // giVisHist_ posture: the placeholders stay bound, the light count stays 0, and everything else in
    // this function still gets created.
    if (rdLocalHistWanted()) {
        rhi::TextureDesc ld;
        ld.dim    = rhi::TextureDim::Tex2D;
        ld.width  = width;
        ld.height = height;
        ld.mips   = 1;
        ld.format = rhi::Format::RGBA16F;
        ld.bind   = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::UnorderedAccess;
        ld.initialState = rhi::ResourceState::ShaderResource;
        ld.debugName = "Voxi RD local-light history A";
        rdLocalHist_[0] = res_->createTexture(ld);
        ld.debugName = "Voxi RD local-light history B";
        rdLocalHist_[1] = res_->createTexture(ld);
        if (!rdLocalHist_[0] || !rdLocalHist_[1]) {
            releaseLocalHistory();
            if (!rdLocalHistFailLogged_) {
                rdLocalHistFailLogged_ = true;
                AVER_WARN("[Voxi] local-light history could not be created at {}x{}; lamps stay unlit "
                          "(said once)", width, height);
            }
        } else {
            rdLocalHistFailLogged_ = false;
        }
    }

    // BACK TO RG32Float FOR THE AMBIENT PAIR -- the reflection block above left d.format on
    // RGBA16F, and inheriting it here would give the depth channel 8cm precision at the exact
    // distances the disocclusion test reads it (clip-space w in centimetres, tens of thousands
    // on terrain). Same reasoning the reflection comment gives for NOT using it for depth.
    // ONLY WHERE THE RAY RUNS -- see aoHistoryWanted(). Low and Medium never write this pair, so
    // allocating it there is 112 MB held for nothing.
    if (aoHistoryWanted()) {
        d.format = rhi::Format::RG32Float;
        d.debugName = "Voxi RT sky-occlusion history A";
        rtAoHist_[0] = res_->createTexture(d);
        d.debugName = "Voxi RT sky-occlusion history B";
        rtAoHist_[1] = res_->createTexture(d);
        if (!rtAoHist_[0] || !rtAoHist_[1]) return false;

        // THE RAW HIT DISTANCE, alongside the pair and under the same condition -- see
        // rtAoHitDist_ in VoxiRenderer.hpp for what it is and who wants it.
        //
        // IT RESTS IN UnorderedAccess AND IS NEVER BARRIERED HERE, unlike the three history pairs
        // above, and that is deliberate rather than an omission. Those pairs are read by the very
        // shader that writes them (as t6/t7/t11 the next frame), so each one has to flip state
        // twice a frame. Nothing in Voxi reads this. It is written whole, every frame, by the only
        // pass that touches it; the eventual consumer runs after that pass and outside this file,
        // and gets to make its own transition when it exists. Adding a round trip to
        // ShaderResource and back now would cost two barriers a frame to serve nobody.
        d.format = rhi::Format::R16Unorm;
        d.initialState = rhi::ResourceState::UnorderedAccess;
        d.debugName = "Voxi RT sky-occlusion hit distance";
        rtAoHitDist_ = res_->createTexture(d);
        if (!rtAoHitDist_) return false;
        d.initialState = rhi::ResourceState::ShaderResource;   // restored for anything added below

    }

    // ---- NRD IS CREATED FOR EITHER SIGNAL, NOT ONLY FOR SKY OCCLUSION ----
    //
    // This block used to sit INSIDE the aoHistoryWanted() branch above, so the NRD instance itself --
    // all twenty pipelines, the whole thing -- was never constructed unless sky-occlusion rays were
    // on. aoHistoryWanted() is `rayTracingWanted() && settings_.giSkyOcclusionRays > 0`, and
    // giSkyOcclusionRays is TIER-DERIVED: Voxi.cpp recomputes it only when the ray-tracing tier
    // CHANGES. Open a project whose recorded tier already equals the current one, the derivation never
    // fires, the field keeps its struct default of 0, and NRD is silently absent for the whole session.
    //
    // MEASURED, and it is exactly what the user saw as the denoiser working only sometimes: four
    // consecutive runs of their project denoised nothing while the log said NRD was ready, and four
    // more with --gi-sky-occlusion-rays 1 denoised every frame -- same binary, same flags otherwise.
    //
    // ReSTIR GI runs at ANY ray-tracing tier; sky occlusion is Epic-only
    // (giSkyOcclusionRaysForQuality). Tying the first to the second was never a decision, it is just
    // where this code happened to be written back when occlusion was its only consumer.
    if (aoHistoryWanted() || giRestirWanted()) {
        // ---- and the denoiser that consumes it, created ONCE beside the signal it filters ----
        // Its create() is the thing that decides whether NRD can run at all -- no NRD in the build,
        // or a backend whose descriptor model refuses NRD's register spaces -- and it says which at
        // INFO. A false here is not an error: nothing downstream is required, and Voxi keeps the
        // hand-written temporal filter it has always shipped. The pools are NOT allocated here;
        // resize() does that per frame from the hit-distance target's own dimensions, so the
        // denoiser follows a render-target change without a second place needing to know about it.
        if (!nrd_.valid()) {
            // BOTH DENOISERS, ALWAYS CREATED, SELECTED PER FRAME. Which ones RUN is chosen at
            // record time by the index list, so creating the diffuse-radiance one here costs its
            // pipelines and its share of the pool and nothing else while ReSTIR GI is off. Creating
            // it lazily instead would mean tearing the whole instance down -- and NRD's temporal
            // history with it -- every time giMode is toggled, which is precisely the moment
            // somebody is looking at the image.
            const render::nrd::DenoiserKind kinds[] = {render::nrd::DenoiserKind::ReblurDiffuseOcclusion,
                                                       render::nrd::DenoiserKind::ReblurDiffuse};
            nrdActive_ = nrd_.create(*dev_, kinds, 2);
            // NOT ONCE-WITH-BARE-DEFAULTS ANY MORE: applyReblurTuning() reads settings_ (already
            // current -- setSettings assigns it before ensureShadowHistory can ever be reached), so
            // creation-time tuning reflects whatever reblur* dials were requested before NRD existed
            // at all, not ReblurTuning{}'s own defaults. setSettings() below re-issues this same call
            // every frame thereafter, which is what makes a later console change live without
            // recreating the instance -- see applyReblurTuning's own comment. Index 1 is ReblurDiffuse
            // in the kinds array above; index 0 is ReblurDiffuseOcclusion and is tuned there too.
            // It used to be left on NRD's defaults "because the signal it filters is already
            // unit-free" -- that reasoning was wrong and is refuted in applyReblurTuning's own
            // comment, with the measurement that caught it.
            if (nrdActive_) applyReblurTuning();
            // THE ENCODING CHECK IS THE ONE SILENT FAILURE MODE LEFT. NRD's shaders were BUILT
            // against a specific normal/roughness packing (cmake/AverNRD.cmake picks it), and
            // averPackNormalRoughness (voxi.hlsl) implements one specific packing too. If those
            // disagree the denoiser still runs and still produces an image -- a plausible, wrong
            // one -- so this COMPARES the two numbers instead of describing one of them in prose.
            // A prose description is exactly what stood here before: it named the encoding NRD
            // reported correctly and then asserted, in English, what the G-buffer wrote -- a
            // statement about the code that could drift the moment the packer changed, and did.
            // That is how this bug survived a whole session: the one check built to catch it
            // described the mismatch instead of detecting it. Reported once rather than per frame,
            // because a warning nobody can act on every frame is noise.
            constexpr u32 kEngineNormalEncoding    = 2;   // NRD_NORMAL_ENCODING_R10G10B10A2_UNORM
            constexpr u32 kEngineRoughnessEncoding = 1;   // NRD_ROUGHNESS_ENCODING_LINEAR
            u32 nEnc = 0, rEnc = 0;
            if (nrdActive_ && !nrdWarnedEncoding_ && render::nrd::Denoiser::encodings(nEnc, rEnc)) {
                if (nEnc != kEngineNormalEncoding || rEnc != kEngineRoughnessEncoding) {
                    AVER_WARN("[NRD] ENCODING MISMATCH: NRD was built for normal encoding {} / "
                              "roughness encoding {}, but averPackNormalRoughness (voxi.hlsl) writes "
                              "normal encoding {} / roughness encoding {} -- REBLUR will decode a "
                              "garbage normal and a view-angle-dependent roughness. Fix "
                              "averPackNormalRoughness or cmake/AverNRD.cmake so the two agree.",
                              nEnc, rEnc, kEngineNormalEncoding, kEngineRoughnessEncoding);
                } else {
                    AVER_INFO("[NRD] denoising sky occlusion; normal/roughness encoding {}/{} "
                              "matches averPackNormalRoughness (voxi.hlsl)", nEnc, rEnc);
                }
                nrdWarnedEncoding_ = true;
            }
        }
    }

    // ---- RTXDI ReSTIR GI: the previous-surface pair(s) and the reservoir buffer, together ----
    // ONLY WHERE giMode ASKS FOR IT -- see giSurfPosHist_'s own declaration for the two-texture
    // format (rhi::Format has no four-channel 32-bit float) and the sentinel, and
    // giRestirWanted()'s own comment for why this is its own condition rather than
    // aoHistoryWanted()'s or a bare rayTracingWanted().
    if (giRestirWanted()) {
        d.format = rhi::Format::RG32Float;
        d.debugName = "Voxi GI-restir surface position history A";
        giSurfPosHist_[0] = res_->createTexture(d);
        d.debugName = "Voxi GI-restir surface position history B";
        giSurfPosHist_[1] = res_->createTexture(d);
        if (!giSurfPosHist_[0] || !giSurfPosHist_[1]) return false;
        d.debugName = "Voxi GI-restir surface normal history A";
        giSurfNrmHist_[0] = res_->createTexture(d);
        d.debugName = "Voxi GI-restir surface normal history B";
        giSurfNrmHist_[1] = res_->createTexture(d);
        if (!giSurfNrmHist_[0] || !giSurfNrmHist_[1]) return false;
        d.initialState = rhi::ResourceState::ShaderResource;   // restored for anything added below

        // The radiance NRD's REBLUR_DIFFUSE filters. RGBA16F because rgb is HDR radiance and a is
        // a normalised distance -- an 8-bit format would quantise indirect light into banding, and
        // a 32-bit one doubles the bandwidth of a texture written and read once per frame. SINGLE,
        // not a pair: it is a raw per-frame measurement handed to a filter that keeps its own
        // history, so there is nothing for a second copy to hold.
        d.format = rhi::Format::RGBA16F;
        d.initialState = rhi::ResourceState::UnorderedAccess;
        d.debugName = "Voxi ReSTIR GI radiance (to NRD)";
        giRadiance_ = res_->createTexture(d);
        if (!giRadiance_) return false;
        d.initialState = rhi::ResourceState::ShaderResource;

        // ---- U1/2.11: the sixth pair, the half-resolution ReSTIR VISIBILITY history (t16/u10) ----
        // ONLY WHILE giVisHistWanted() -- a STRICTER question than the giRestirWanted() this whole
        // block already runs under (see giVisHistWanted()'s own comment): Full/Reconstructed/NoRay
        // never read or write this pair, so allocating it for them would be VRAM held for a mode
        // that is not running, the identical VRAM-consciousness this function already applies to the
        // AO pair (aoHistoryWanted()) and this pair's own siblings above.
        //
        // HALF THE LINEAR DIMENSION, ROUNDED UP: 2.10 D/E write exactly one full-resolution pixel per
        // 2x2 block per frame, so one texel per block is all this needs to hold.
        //
        // A FAILED ALLOCATION IS NOT FATAL TO THE REST OF ReSTIR GI, unlike every `return false`
        // above in this block: it destroys both handles, warns once through giVisHistFailLogged_ and
        // falls through to the reservoir-buffer growth below -- the shader's own bit-4 decode (2.9)
        // then runs Half as Full with no extra plumbing, "wrong-but-running beats a dead renderer"
        // the same posture giShadowTex_'s own comment (VoxiRenderer.hpp) already takes.
        if (giVisHistWanted()) {
            const u32 vw = givis::halfDim(width), vh = givis::halfDim(height);
            rhi::TextureDesc vd;
            vd.dim    = rhi::TextureDim::Tex2D;
            vd.width  = vw;
            vd.height = vh;
            vd.mips   = 1;
            vd.format = rhi::Format::RGBA16F;
            vd.bind   = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::UnorderedAccess;
            vd.initialState = rhi::ResourceState::ShaderResource;
            vd.debugName = "Voxi ReSTIR visibility history A";
            giVisHist_[0] = res_->createTexture(vd);
            vd.debugName = "Voxi ReSTIR visibility history B";
            giVisHist_[1] = res_->createTexture(vd);
            if (!giVisHist_[0] || !giVisHist_[1]) {
                if (giVisHist_[0]) { res_->destroyTexture(giVisHist_[0]); giVisHist_[0] = 0; }
                if (giVisHist_[1]) { res_->destroyTexture(giVisHist_[1]); giVisHist_[1] = 0; }
                if (!giVisHistFailLogged_) {
                    giVisHistFailLogged_ = true;
                    AVER_WARN("[Voxi] ReSTIR half-resolution visibility history could not be created "
                              "at {}x{}; Half resolution runs as Full (said once)", vw, vh);
                }
            } else {
                giVisHistFailLogged_ = false;
                const f64 pairMiB = static_cast<f64>(vw) * static_cast<f64>(vh) * 8.0 * 2.0 /
                                   (1024.0 * 1024.0);
                AVER_INFO("[Voxi] ReSTIR half-resolution visibility history: 2 x {}x{} RGBA16F = "
                          "{:.1f} MiB", vw, vh, pairMiB);
            }
        } else if (giVisHist_[0]) {
            // giRestirVisibility left HalfResolution (or ReSTIR GI's own wanted edge moved) without
            // the whole-history teardown branch at the top of this function running -- e.g.
            // width/height changed while giVisHistWanted() was already false. Nothing after this
            // point needs the pair, and holding it is exactly the VRAM-for-a-mode-not-running waste
            // this whole block exists to avoid elsewhere -- the identical edge case giReservoirs_'s
            // own else-if just below handles for the reservoir buffer.
            res_->destroyTexture(giVisHist_[0]);
            res_->destroyTexture(giVisHist_[1]);
            giVisHist_[0] = giVisHist_[1] = 0;
        }

        // GROWN, NOT ALWAYS REBUILT -- the same "just enough" rule rtVerts_/rtIndices_ apply
        // (buildGeometryTable): the block-rounded pitch is a STEP function of width/height, so most
        // resizes (anything that doesn't cross a 16-pixel-block boundary) need no new buffer at all,
        // and this is the one GI-restir resource actually large enough for that to matter (32 bytes
        // times two ping-pong copies times every pixel).
        const u32 elemCount = giReservoirElemCount(width, height);
        if (giReservoirElemCapacity_ < elemCount) {
            if (giReservoirs_) res_->destroyBuffer(giReservoirs_);
            rhi::BufferDesc bd;
            bd.bytes = static_cast<u64>(elemCount) * kGiReservoirElemBytes;
            bd.kind  = rhi::BufferKind::Default;
            bd.allowUnorderedAccess = true;
            bd.debugName = "Voxi RTXDI GI reservoirs";
            giReservoirs_ = res_->createBuffer(bd);
            giReservoirElemCapacity_ = giReservoirs_ ? elemCount : 0;
            if (!giReservoirs_) return false;
            // M6: said only when the buffer is actually (re)created -- already the rare branch, per
            // the "grown, not always rebuilt" comment above.
            AVER_INFO("[Voxi] ReSTIR GI reservoir buffer: {} element(s) x {} B = {:.1f} MiB for {}x{}",
                      elemCount, kGiReservoirElemBytes,
                      static_cast<f64>(bd.bytes) / (1024.0 * 1024.0), width, height);
        }
    } else {
        // giMode was switched off (or ray tracing was) without the whole-history teardown branch at
        // the top of this function running -- e.g. width/height changed while giRestirWanted() was
        // already false. Nothing after this point needs the buffer, and holding it is exactly the
        // VRAM-for-a-feature-not-running waste this whole function exists to avoid elsewhere.
        if (giReservoirs_) {
            res_->destroyBuffer(giReservoirs_);
            giReservoirs_ = 0;
            giReservoirElemCapacity_ = 0;
        }
        // U1/2.11: the sixth pair rides the identical edge case -- giVisHistWanted() REQUIRES
        // giRestirWanted(), so this branch (giRestirWanted() false) already implies the pair cannot
        // be wanted either, whether or not it happened to survive from before this same edge.
        if (giVisHist_[0]) {
            res_->destroyTexture(giVisHist_[0]);
            res_->destroyTexture(giVisHist_[1]);
            giVisHist_[0] = giVisHist_[1] = 0;
        }
    }

    rtShadowHistW_ = width;
    rtShadowHistH_ = height;
    rtHistWriteIdx_ = 0;
    // A starting bind, not the steady state: beginShadowHistory rebinds whichever side is which
    // every frame as the roles swap.
    res_->setUav(bindings_, 2, rtShadowHist_[0], 0);
    res_->setSrv(bindings_, 6, rtShadowHist_[1]);
    res_->setUav(bindings_, 3, rtReflHist_[0], 0);
    res_->setSrv(bindings_, 7, rtReflHist_[1]);
    if (rtAoHist_[0]) {
        res_->setUav(bindings_, 4, rtAoHist_[0], 0);
        res_->setSrv(bindings_, 11, rtAoHist_[1]);
    }
    // Bound once and never rebound: it does not ping-pong, so unlike u4 above there is no per-frame
    // role for beginShadowHistory to swap.
    if (rtAoHitDist_) res_->setUav(bindings_, 5, rtAoHitDist_, 0);
    // Same starting bind as the three pairs above -- beginShadowHistory swaps u7/t12 and u8/t13
    // every frame, together.
    if (giSurfPosHist_[0]) {
        res_->setUav(bindings_, 7, giSurfPosHist_[0], 0);
        res_->setSrv(bindings_, 12, giSurfPosHist_[1]);
    }
    if (giSurfNrmHist_[0]) {
        res_->setUav(bindings_, 8, giSurfNrmHist_[0], 0);
        res_->setSrv(bindings_, 13, giSurfNrmHist_[1]);
    }
    // U1/2.11: the sixth pair's own starting bind, the identical shape -- beginShadowHistory swaps
    // u10/t16 every frame, together, exactly like u7/t12 and u8/t13 above.
    if (giVisHist_[0]) {
        res_->setUav(bindings_, 10, giVisHist_[0], 0);
        res_->setSrv(bindings_, 16, giVisHist_[1]);
    }
    // LOCAL LIGHTS (LAMPS): the same starting bind for u19/t19; beginShadowHistory swaps them with the
    // rest. Absent, the placeholders releaseLocalHistory() bound above stay.
    if (rdLocalHist_[0] && rdLocalHist_[1]) {
        res_->setUav(bindings_, 19, rdLocalHist_[0], 0);
        res_->setSrv(bindings_, 19, rdLocalHist_[1]);
    }
    // The reservoir StructuredBuffer is bound ONCE, like rtAoHitDist_ above -- it never ping-pongs as
    // a DESCRIPTOR, only the ARRAY INDEX RTXDI_ReservoirPositionToPointer computes from
    // cb_.giRestirParams.z does (see beginShadowHistory), so there is nothing for a resize to rebind
    // beyond the element count already baked into this view.
    if (giReservoirs_)
        res_->setUavBuffer(bindings_, 6, giReservoirs_, kGiReservoirElemBytes,
                           giReservoirElemCapacity_, 0);
    return true;
}

// STAGED RAY-DRIVEN PASSES (milestone 1, extended by milestone 2's rdGiTex_/rdAoTex_ pair, milestone
// 3's rdReflTex_, and the sub-stage splits' own rdGiCandBuf_/rdShadowTileBuf_): (re)creates or
// releases rdVisBuf_/rdSunVisTex_/rdGiTex_/rdAoTex_/rdReflTex_/rdGiCandBuf_/rdShadowTileBuf_ at the
// given render-target size. Shaped like ensureShadowHistory just above -- same two call sites
// (onRenderTargetsChanged and setSettings' own on/off edge), same "release everything when not
// wanted" branch first -- but simpler, since there is no ping-pong pair, every texture is recreated
// outright on any size change rather than resized in place, and every buffer is grown rather than
// always rebuilt: nothing here reprojects a previous frame's contents the way the shadow/reflection/
// ambient histories do, so there is no stale content worth preserving across a resize.
bool VoxiRenderer::ensureRdStagedResources(u32 width, u32 height) {
    if (!res_ || !bindings_) return false;

    // NOT WANTED, OR NOTHING TO SIZE AGAINST YET: release every resource and fall back to the
    // placeholders, the identical shape ensureShadowHistory's own "!rayTracingWanted()" branch
    // follows for its four textures and one buffer -- except THOSE rebind nothing before destroying
    // (t6/u2 etc. are simply left null-filled again by the SAME idiom bindings_ was created with, and
    // never re-read until the next time they are populated). u11-u15 cannot do that: setUav/
    // setUavBuffer refuse a 0 handle outright (D3D12ResourceFactory::setUav/setUavBuffer), so once a
    // slot has ever pointed at a real resource, releasing that resource without rebinding the slot
    // first leaves a descriptor naming freed memory -- exactly what voxelAccumPlaceholder_ exists to
    // avoid for u1, and the reason a placeholder exists here too.
    if (!rdStagedResourcesWanted() || width == 0 || height == 0) {
        const bool had = rdVisBuf_ != 0 || rdSunVisTex_ != 0 || rdGiTex_ != 0 || rdAoTex_ != 0 ||
                         rdReflTex_ != 0 || rdGiCandBuf_ != 0 || rdShadowTileBuf_ != 0;
        if (had) {
            if (!rdVisBufPlaceholder_) {
                rhi::BufferDesc pd;
                pd.bytes = kRdVisElemBytes;   // one element: the smallest a StructuredBuffer view of
                                               // this stride can legally describe.
                pd.kind  = rhi::BufferKind::Default;
                pd.allowUnorderedAccess = true;
                pd.debugName = "Voxi ray-driven visibility buffer placeholder";
                rdVisBufPlaceholder_ = res_->createBuffer(pd);
            }
            if (!rdSunVisPlaceholder_) {
                rhi::TextureDesc pd;
                pd.dim    = rhi::TextureDim::Tex2D;
                pd.width  = 1; pd.height = 1; pd.mips = 1;
                pd.format = rhi::Format::RGBA16F;
                pd.bind   = rhi::ResourceBind::UnorderedAccess;
                pd.initialState = rhi::ResourceState::UnorderedAccess;
                pd.debugName    = "Voxi ray-driven sun visibility placeholder";
                rdSunVisPlaceholder_ = res_->createTexture(pd);
            }
            // MILESTONE 2's OWN PAIR, the identical 1x1 RGBA16F UAV placeholder shape as
            // rdSunVisPlaceholder_ immediately above -- two independent textures, not a ping-pong
            // pair, so two independent placeholder handles rather than one shared between them.
            if (!rdGiPlaceholder_) {
                rhi::TextureDesc pd;
                pd.dim    = rhi::TextureDim::Tex2D;
                pd.width  = 1; pd.height = 1; pd.mips = 1;
                pd.format = rhi::Format::RGBA16F;
                pd.bind   = rhi::ResourceBind::UnorderedAccess;
                pd.initialState = rhi::ResourceState::UnorderedAccess;
                pd.debugName    = "Voxi ray-driven GI indirect diffuse placeholder";
                rdGiPlaceholder_ = res_->createTexture(pd);
            }
            if (!rdAoPlaceholder_) {
                rhi::TextureDesc pd;
                pd.dim    = rhi::TextureDim::Tex2D;
                pd.width  = 1; pd.height = 1; pd.mips = 1;
                pd.format = rhi::Format::RGBA16F;
                pd.bind   = rhi::ResourceBind::UnorderedAccess;
                pd.initialState = rhi::ResourceState::UnorderedAccess;
                pd.debugName    = "Voxi ray-driven sky occlusion placeholder";
                rdAoPlaceholder_ = res_->createTexture(pd);
            }
            // MILESTONE 3's OWN OUTPUT, the identical 1x1 RGBA16F UAV placeholder shape as
            // rdGiPlaceholder_/rdAoPlaceholder_ immediately above.
            if (!rdReflPlaceholder_) {
                rhi::TextureDesc pd;
                pd.dim    = rhi::TextureDim::Tex2D;
                pd.width  = 1; pd.height = 1; pd.mips = 1;
                pd.format = rhi::Format::RGBA16F;
                pd.bind   = rhi::ResourceBind::UnorderedAccess;
                pd.initialState = rhi::ResourceState::UnorderedAccess;
                pd.debugName    = "Voxi ray-driven reflection placeholder";
                rdReflPlaceholder_ = res_->createTexture(pd);
            }
            // SUB-STAGE SPLITS' OWN PAIR, the identical 1-element StructuredBuffer placeholder shape
            // rdVisBufPlaceholder_ immediately above already is.
            if (!rdGiCandBufPlaceholder_) {
                rhi::BufferDesc pd;
                pd.bytes = kRdGiCandElemBytes;
                pd.kind  = rhi::BufferKind::Default;
                pd.allowUnorderedAccess = true;
                pd.debugName = "Voxi ray-driven GI-trace candidate buffer placeholder";
                rdGiCandBufPlaceholder_ = res_->createBuffer(pd);
            }
            if (!rdShadowTileBufPlaceholder_) {
                rhi::BufferDesc pd;
                pd.bytes = kRdShadowTileElemBytes;
                pd.kind  = rhi::BufferKind::Default;
                pd.allowUnorderedAccess = true;
                pd.debugName = "Voxi ray-driven shadow-probe tile buffer placeholder";
                rdShadowTileBufPlaceholder_ = res_->createBuffer(pd);
            }
            // REBIND BEFORE DESTROY, always -- aver-view-outlives-its-buffer.md, and the identical
            // reasoning voxelAccumPlaceholder_'s own call site gives for itself.
            if (rdVisBufPlaceholder_) res_->setUavBuffer(bindings_, 11, rdVisBufPlaceholder_, kRdVisElemBytes, 1, 0);
            if (rdSunVisPlaceholder_) res_->setUav(bindings_, 12, rdSunVisPlaceholder_, 0);
            if (rdGiPlaceholder_)     res_->setUav(bindings_, 13, rdGiPlaceholder_, 0);
            if (rdAoPlaceholder_)     res_->setUav(bindings_, 14, rdAoPlaceholder_, 0);
            if (rdReflPlaceholder_)   res_->setUav(bindings_, 15, rdReflPlaceholder_, 0);
            if (rdGiCandBufPlaceholder_)     res_->setUavBuffer(bindings_, 17, rdGiCandBufPlaceholder_, kRdGiCandElemBytes, 1, 0);
            if (rdShadowTileBufPlaceholder_) res_->setUavBuffer(bindings_, 18, rdShadowTileBufPlaceholder_, kRdShadowTileElemBytes, 1, 0);
            if (rdVisBuf_)    { res_->destroyBuffer(rdVisBuf_);  rdVisBuf_ = 0; }
            if (rdSunVisTex_) { res_->destroyTexture(rdSunVisTex_); rdSunVisTex_ = 0; }
            if (rdGiTex_)     { res_->destroyTexture(rdGiTex_);     rdGiTex_ = 0; }
            if (rdAoTex_)     { res_->destroyTexture(rdAoTex_);     rdAoTex_ = 0; }
            if (rdReflTex_)   { res_->destroyTexture(rdReflTex_);   rdReflTex_ = 0; }
            if (rdGiCandBuf_)     { res_->destroyBuffer(rdGiCandBuf_);     rdGiCandBuf_ = 0; }
            if (rdShadowTileBuf_) { res_->destroyBuffer(rdShadowTileBuf_); rdShadowTileBuf_ = 0; }
            rdVisBufElemCapacity_ = 0;
            rdGiCandBufElemCapacity_ = rdShadowTileElemCapacity_ = 0;
            rdStagedW_ = rdStagedH_ = rdStagedRowPitch_ = 0;
            AVER_INFO("[Voxi] staged ray-driven resources released");
        }
        return true;
    }

    // Row pitch is the render target's own width, exactly -- see kRdVisElemBytes' own comment. Every
    // resource already matches: nothing to do, the common case on every frame between resizes.
    const u32 pitch = width;
    const u32 elemCount = pitch * height;
    // Tile count for rdShadowTileBuf_ -- one uint per 8x8 tile of the FULL render target, the SAME
    // ceil(W/8) x ceil(H/8) shape CSRdShadowProbe's own dispatch and tileIdx math use.
    const u32 tilesX = (width + 7u) / 8u;
    const u32 tilesY = (height + 7u) / 8u;
    const u32 tileElemCount = tilesX * tilesY;
    if (rdStagedW_ == width && rdStagedH_ == height && rdSunVisTex_ && rdGiTex_ && rdAoTex_ &&
        rdReflTex_ && rdVisBufElemCapacity_ >= elemCount &&
        rdGiCandBufElemCapacity_ >= elemCount && rdShadowTileElemCapacity_ >= tileElemCount)
        return true;

    // THE TEXTURE HAS NO GROWTH HEADROOM, UNLIKE THE BUFFER BELOW: a Texture2D UAV view is exactly
    // its resource's own dimensions, so any size change recreates it outright -- the same rule
    // rtShadowHist_/rtReflHist_/rtAoHist_ apply to themselves in ensureShadowHistory above, just
    // without their ping-pong pairing (nothing here reprojects, so there is no "which side is which"
    // to preserve across the recreate).
    if (rdSunVisTex_ && (rdStagedW_ != width || rdStagedH_ != height)) {
        res_->destroyTexture(rdSunVisTex_);
        rdSunVisTex_ = 0;
    }
    if (!rdSunVisTex_) {
        // UAV ONLY, NO ShaderResource: gRdSunVisTex has no SRV twin at any register (the contract's
        // own words -- see giTableKinds' u12 comment), so there is no descriptor that would ever need
        // the SRV bind flag, unlike rtAoHitDist_ above, which inherits it from the shared desc its own
        // pair uses.
        rhi::TextureDesc d;
        d.dim    = rhi::TextureDim::Tex2D;
        d.width  = width;
        d.height = height;
        d.mips   = 1;
        d.format = rhi::Format::RGBA16F;
        d.bind   = rhi::ResourceBind::UnorderedAccess;
        d.initialState = rhi::ResourceState::UnorderedAccess;
        d.debugName    = "Voxi ray-driven sun visibility";
        rdSunVisTex_ = res_->createTexture(d);
        if (!rdSunVisTex_) return false;
        res_->setUav(bindings_, 12, rdSunVisTex_, 0);
    }

    // MILESTONE 2's OWN PAIR, the identical "recreated outright on any size change" shape as
    // rdSunVisTex_ immediately above -- UAV ONLY, NO ShaderResource, for the identical "no SRV twin
    // at any register" reason (giTableKinds' u13/u14 comment).
    if (rdGiTex_ && (rdStagedW_ != width || rdStagedH_ != height)) {
        res_->destroyTexture(rdGiTex_);
        rdGiTex_ = 0;
    }
    if (!rdGiTex_) {
        rhi::TextureDesc d;
        d.dim    = rhi::TextureDim::Tex2D;
        d.width  = width;
        d.height = height;
        d.mips   = 1;
        d.format = rhi::Format::RGBA16F;
        d.bind   = rhi::ResourceBind::UnorderedAccess;
        d.initialState = rhi::ResourceState::UnorderedAccess;
        d.debugName    = "Voxi ray-driven GI indirect diffuse";
        rdGiTex_ = res_->createTexture(d);
        if (!rdGiTex_) return false;
        res_->setUav(bindings_, 13, rdGiTex_, 0);
    }
    if (rdAoTex_ && (rdStagedW_ != width || rdStagedH_ != height)) {
        res_->destroyTexture(rdAoTex_);
        rdAoTex_ = 0;
    }
    if (!rdAoTex_) {
        rhi::TextureDesc d;
        d.dim    = rhi::TextureDim::Tex2D;
        d.width  = width;
        d.height = height;
        d.mips   = 1;
        d.format = rhi::Format::RGBA16F;
        d.bind   = rhi::ResourceBind::UnorderedAccess;
        d.initialState = rhi::ResourceState::UnorderedAccess;
        d.debugName    = "Voxi ray-driven sky occlusion";
        rdAoTex_ = res_->createTexture(d);
        if (!rdAoTex_) return false;
        res_->setUav(bindings_, 14, rdAoTex_, 0);
    }

    // MILESTONE 3's OWN OUTPUT, the identical "recreated outright on any size change" shape as
    // rdGiTex_/rdAoTex_ immediately above -- UAV ONLY, NO ShaderResource, for the identical "no SRV
    // twin at any register" reason (giTableKinds' u15 comment).
    if (rdReflTex_ && (rdStagedW_ != width || rdStagedH_ != height)) {
        res_->destroyTexture(rdReflTex_);
        rdReflTex_ = 0;
    }
    if (!rdReflTex_) {
        rhi::TextureDesc d;
        d.dim    = rhi::TextureDim::Tex2D;
        d.width  = width;
        d.height = height;
        d.mips   = 1;
        d.format = rhi::Format::RGBA16F;
        d.bind   = rhi::ResourceBind::UnorderedAccess;
        d.initialState = rhi::ResourceState::UnorderedAccess;
        d.debugName    = "Voxi ray-driven reflection";
        rdReflTex_ = res_->createTexture(d);
        if (!rdReflTex_) return false;
        res_->setUav(bindings_, 15, rdReflTex_, 0);
    }

    // GROWN, NOT ALWAYS REBUILT -- the identical "just enough" rule giReservoirs_ above applies to
    // itself: most resizes that shrink the pitch or the row count keep the existing buffer, and only
    // a resize past the current capacity pays for a new allocation.
    if (rdVisBufElemCapacity_ < elemCount) {
        if (rdVisBuf_) res_->destroyBuffer(rdVisBuf_);
        rhi::BufferDesc bd;
        bd.bytes = static_cast<u64>(elemCount) * kRdVisElemBytes;
        bd.kind  = rhi::BufferKind::Default;
        bd.allowUnorderedAccess = true;
        bd.debugName = "Voxi ray-driven visibility records";
        rdVisBuf_ = res_->createBuffer(bd);
        rdVisBufElemCapacity_ = rdVisBuf_ ? elemCount : 0;
        if (!rdVisBuf_) return false;
        AVER_INFO("[Voxi] ray-driven visibility record buffer: {} element(s) x {} B = {:.1f} MiB for "
                  "{}x{} (row pitch {})", elemCount, kRdVisElemBytes,
                  static_cast<f64>(bd.bytes) / (1024.0 * 1024.0), width, height, pitch);
    }
    res_->setUavBuffer(bindings_, 11, rdVisBuf_, kRdVisElemBytes, rdVisBufElemCapacity_, 0);

    // SUB-STAGE SPLITS' OWN BUFFERS (Settings::rayDrivenShadowTiles / rayDrivenGiSplit): the identical
    // "grown, not always rebuilt" shape rdVisBuf_ immediately above already is, allocated here
    // UNCONDITIONALLY -- neither setting gates this, so a project that flips one on mid-session never
    // finds an undersized buffer (see rdGiCandBuf_'s own comment, VoxiRenderer.hpp).
    if (rdGiCandBufElemCapacity_ < elemCount) {
        if (rdGiCandBuf_) res_->destroyBuffer(rdGiCandBuf_);
        rhi::BufferDesc bd;
        bd.bytes = static_cast<u64>(elemCount) * kRdGiCandElemBytes;
        bd.kind  = rhi::BufferKind::Default;
        bd.allowUnorderedAccess = true;
        bd.debugName = "Voxi ray-driven GI-trace candidates";
        rdGiCandBuf_ = res_->createBuffer(bd);
        rdGiCandBufElemCapacity_ = rdGiCandBuf_ ? elemCount : 0;
        if (!rdGiCandBuf_) return false;
        AVER_INFO("[Voxi] ray-driven GI-trace candidate buffer: {} element(s) x {} B = {:.1f} MiB for "
                  "{}x{} (row pitch {})", elemCount, kRdGiCandElemBytes,
                  static_cast<f64>(bd.bytes) / (1024.0 * 1024.0), width, height, pitch);
    }
    res_->setUavBuffer(bindings_, 17, rdGiCandBuf_, kRdGiCandElemBytes, rdGiCandBufElemCapacity_, 0);

    if (rdShadowTileElemCapacity_ < tileElemCount) {
        if (rdShadowTileBuf_) res_->destroyBuffer(rdShadowTileBuf_);
        rhi::BufferDesc bd;
        bd.bytes = static_cast<u64>(tileElemCount) * kRdShadowTileElemBytes;
        bd.kind  = rhi::BufferKind::Default;
        bd.allowUnorderedAccess = true;
        bd.debugName = "Voxi ray-driven shadow-probe tiles";
        rdShadowTileBuf_ = res_->createBuffer(bd);
        rdShadowTileElemCapacity_ = rdShadowTileBuf_ ? tileElemCount : 0;
        if (!rdShadowTileBuf_) return false;
        AVER_INFO("[Voxi] ray-driven shadow-probe tile buffer: {} tile(s) x {} B = {:.1f} KiB for "
                  "{}x{} ({}x{} tiles)", tileElemCount, kRdShadowTileElemBytes,
                  static_cast<f64>(bd.bytes) / 1024.0, width, height, tilesX, tilesY);
    }
    res_->setUavBuffer(bindings_, 18, rdShadowTileBuf_, kRdShadowTileElemBytes, rdShadowTileElemCapacity_, 0);

    rdStagedW_ = width;
    rdStagedH_ = height;
    rdStagedRowPitch_ = pitch;
    return true;
}

// See the header's own comment on applyReblurTuning for when this runs (once at NRD creation, then
// every setSettings() call thereafter) and why that is safe (nrd::SetDenoiserSettings is documented,
// NRD.h, as legal at any cadence). hitDistA/B/C/enableAntiFirefly are left at ReblurTuning{}'s own
// defaults here -- those are the engine's fixed unit-conversion constants (see ReblurTuning's own
// comment on why metres-vs-centimetres matters), not something reblur* console dials touch.
void VoxiRenderer::applyReblurTuning() {
    render::nrd::Denoiser::ReblurTuning t{};
    t.diffusePrepassBlurRadius = settings_.reblurDiffusePrepassBlurRadius;
    t.maxAccumulatedFrameNum   = settings_.reblurMaxAccumulatedFrameNum;
    t.maxStabilizedFrameNum    = settings_.reblurMaxStabilizedFrameNum;
    t.enableAntiFirefly             = settings_.reblurAntiFirefly;
    t.fireflySuppressorMinRelativeScale = settings_.reblurFireflySuppressorScale;
    t.antilagLuminanceSigmaScale    = settings_.reblurAntilagSigmaScale;
    t.antilagLuminanceSensitivity   = settings_.reblurAntilagSensitivity;
    t.minHitDistanceWeight          = settings_.reblurMinHitDistanceWeight;
    t.fastHistoryClampingSigmaScale = settings_.reblurFastHistoryClampSigma;
    t.maxFastAccumulatedFrameNum    = settings_.reblurMaxFastAccumulatedFrameNum;
    t.historyFixFrameNum            = settings_.reblurHistoryFixFrameNum;
    // THE SUN IS MOVING (nrdSunClampApplied_): cap the history depth -- see Settings::
    // reblurSunMovingFrameNum. The stabilized and fast depths follow it down so NRD still gets
    // historyFix < fast <= main, the same ordering Settings' own clamp keeps.
    if (nrdSunClampApplied_ && settings_.reblurSunMovingFrameNum < t.maxAccumulatedFrameNum) {
        t.maxAccumulatedFrameNum     = settings_.reblurSunMovingFrameNum;
        t.maxStabilizedFrameNum      = std::min(t.maxStabilizedFrameNum, t.maxAccumulatedFrameNum);
        t.maxFastAccumulatedFrameNum = std::min(t.maxFastAccumulatedFrameNum, t.maxAccumulatedFrameNum);
        t.historyFixFrameNum = t.maxFastAccumulatedFrameNum == 0u ? 0u
            : std::min(t.historyFixFrameNum, t.maxFastAccumulatedFrameNum - 1u);
    }
    t.minBlurRadius                 = settings_.reblurMinBlurRadius;
    t.maxBlurRadius                 = settings_.reblurMaxBlurRadius;
    // ---- INDEX 0 (ReblurDiffuseOcclusion) IS TUNED TOO, AND THE COMMENT SAYING IT NEED NOT BE WAS
    // WRONG ----
    //
    // The claim was that the occlusion signal is "already unit-free", so NRD's own hit-distance
    // defaults were harmless against it. They are not. NRD de-normalises hit distance as
    // (A + |viewZ| * B) * lerp(C, 1, smc) in EVERY mode -- REBLUR_Common_SpatialFilter.hlsli calls
    // _REBLUR_GetHitDistanceNormalization unconditionally, with no NRD_MODE_OCCLUSION exemption, as
    // do the hit-distance reconstruction, temporal accumulation and history-fix passes. So this is
    // not a mistuning of an otherwise workable curve, it is the wrong SHAPE of curve: a
    // viewZ-dependent divisor applied to a signal the producer normalised by a flat constant
    // (voxi_rt.hlsli's aoTMax = gVoxelParams.z = settings_.giMaxDistance, packed a few hundred lines
    // above). The texture is R16Unorm, which can only hold [0,1] and would clip a real length --
    // the encoding is a fraction by construction.
    //
    // B = 0 and C = 1 collapse NRD's divisor to a flat A, the only form that can match, and A is
    // then the same giMaxDistance the producer divided by. This is render.nrd/README.md's own
    // prescription in its own words: "A consumer wanting NRD's convention passes
    // hitDistParams = {giMaxDistance, 0, 1}, which makes NRD's own normalisation the identity
    // against this encoding." It is read from settings_ rather than written as a literal because it
    // is a live console dial.
    //
    // ---- WHAT THIS DOES AND DOES NOT FIX, MEASURED BOTH SIDES ----
    //
    // gNrdAo rendered 0.63 in deep shadow and 0.01 in OPEN SUNLIT AREAS on a scale where 1.0 is
    // 230.8 -- "fully occluded" everywhere, including sky a surface can see all of. With this
    // tuning applied and ACCEPTED by NRD (no rejection warning), it renders 0.00 across the board,
    // and the shaded frame is unchanged to two decimal places: 3.61 mean absolute difference from a
    // converged path-traced reference either way, parity against ray-driven 0.98 either way.
    //
    // So THE UNITS WERE GENUINELY WRONG AND ARE NOW RIGHT, and that is worth having on its own --
    // the previous justification was false against NRD's own source, and a latent unit mismatch
    // left in place because it currently happens to be invisible is how the rest of this file's
    // recent bugs survived as long as they did. But it is NOT the cause of the zero output: a
    // correctly-normalised denoiser still returning zero everywhere means something else in this
    // signal's path is broken -- the input not reaching it, or the wrong output slot being read
    // (this module has a history of exactly that; see the NRD contract notes). NOT YET FOUND.
    //
    // Consequently: do not read this change as an improvement, and do not let its presence suggest
    // the AO denoiser works. Raster measures 3.77 with gNrdAo and 4.23 without, so what it is
    // currently contributing is "fully occluded", which flatters a dark interior by accident and
    // would be wrong in an open one.
    //
    // EVERY OTHER FIELD IS LEFT AT ReblurTuning's defaults, which are NRD's own, so index 0 is
    // otherwise byte-for-byte the denoiser it was. The accumulation knobs `t` carries are
    // deliberately NOT copied across: they are a separate decision with a separate measurement, and
    // folding them in here would make this change two variables instead of one.
    render::nrd::Denoiser::ReblurTuning ao{};
    ao.hitDistA = settings_.giMaxDistance;
    ao.hitDistB = 0.0f;
    ao.hitDistC = 1.0f;
    if (!nrd_.setReblurTuning(kNrdAoDenoiser[0], ao) && !nrdWarnedReblurRetune_) {
        nrdWarnedReblurRetune_ = true;
        AVER_WARN("[NRD] REBLUR_DIFFUSE_OCCLUSION hit-distance tuning was rejected by NRD; the "
                  "occlusion denoiser keeps defaults whose scale does not match this engine's "
                  "fraction-of-giMaxDistance encoding (said once)");
    }
    // MILESTONE 4: half-rate ReSTIR GI's checkerboard switch, ReblurDiffuse (index 1) ONLY -- never
    // `ao` immediately above, index 0, ReblurDiffuseOcclusion. The occlusion denoiser is never fed a
    // checkerboarded input (rtSkyOcclusionTemporal writes gRdAoTex at full rate always; only CSRdGi's
    // gRdGiTex gets the half-rate treatment), so forcing NRD to reinterpret ITS input as checkerboarded
    // packing would misread a full-resolution texture as half of one.
    //
    // ONE-FRAME LAG, BY CONSTRUCTION, NOT AN OVERSIGHT: this dispatch denoises LAST frame's CSRdGi
    // write, so the mode that has to apply here is whatever LAST frame's write actually used
    // (nrdGiCbApplied_, set by the caller from nrdGiInputCheckerboard_ -- see that latch's own comment).
    // fs.frameIndex this same call is nrdFrame_'s value from BEFORE this dispatch's own increment, i.e.
    // the exact value recordStagedRayDriven read for ITS OWN parity when it made that write -- so
    // frameIndex and checkerboardMode agree on which write they both describe.
    //
    // NO forceHistoryReset() ON A TOGGLE: REBLUR's permanent-pool history is the same full-resolution
    // signal in either mode -- checkerboardMode only changes how THIS CALL'S noisy input is READ (packed
    // 2:1 into the left half, or read at full resolution), not the shape of what REBLUR accumulates or
    // hands back. A mode flip is exactly as safe as any other frame-to-frame tuning change already
    // applied here without a reset.
    t.checkerboardMode = nrdGiCbApplied_;
    // Index 1 is ReblurDiffuse.
    if (!nrd_.setReblurTuning(1u, t) && !nrdWarnedReblurRetune_) {
        nrdWarnedReblurRetune_ = true;
        AVER_WARN("[NRD] REBLUR_DIFFUSE tuning was rejected by NRD; it keeps whatever the last "
                  "successful call set instead of the requested voxi.reblur* values (said once)");
    }
}

// See VoxiRenderer.hpp for the ping-pong rationale.
void VoxiRenderer::beginShadowHistory(rhi::IRenderContext& ctx) {
    // MILESTONE 4: latched BEFORE anything else in this function runs, including the shadowHistoryActive()
    // early return below. recordStagedRayDriven (scenePass, LATER in frame order than this function) set
    // giCbWrittenThisFrame_ for LAST frame's own dispatch -- the NRD dispatch further down in THIS
    // function denoises exactly that write, so "was the input this frame's NRD call is about to read
    // checkerboarded" has to be read here, at the top of the one frame that call runs in, before this
    // same frame's OWN (later) recordStagedRayDriven call overwrites giCbWrittenThisFrame_ with its own
    // decision for NEXT frame's NRD call to read the same way.
    nrdGiInputCheckerboard_ = giCbWrittenThisFrame_;
    giCbWrittenThisFrame_ = false;
    // M1: its own child span under "Voxi GI update" (prePass), covering every GPU-visible thing this
    // function does -- the six history-texture barriers and rebinds below, and (nested under its OWN
    // "Voxi NRD denoise" marker further down) the NRD dispatch. RAII, so the early return just below
    // (shadowHistoryActive() false) closes it exactly like every other exit.
    rhi::ScopedGpuStat historyStat(ctx, "Voxi shadow history");
    // Both default to 0: an unbound t6/u2 is Tier 1 null-filled, and the shader must not touch
    // either slot unless THIS frame actually bound them to real textures below.
    cb_.rtHistParams[0] = 0.0f;
    cb_.rtHistParams[1] = 0.0f;
    // BEFORE THE EARLY RETURN, for the same reason the two above it are: a frame that binds nothing
    // must say so, and leaving this at last frame's value would point the ambient path at a slot
    // this frame never bound.
    cb_.rtDenoiseParams[3] = 0.0f;
    // Same reasoning again: giRestirParams.x says whether t12/u6/u7 are bound to real resources this
    // frame, and must not still read 1.0 from a previous frame once this one returns early below.
    cb_.giRestirParams[0] = 0.0f;
    // LOCAL LIGHTS (LAMPS): and whether u19 is a real texture this frame -- every lamp-shading scene pass
    // (localLightsReady()) writes only to what this function actually bound.
    rdLocalOutThisFrame_ = 0;
    // ---- F5: THE POISON-VIEW FLAG, PUBLISHED HERE SO IT REACHES giMode 0 TOO ----
    // This USED TO be written only inside the giSurfPosHist_/giSurfNrmHist_ block further down,
    // which runs only when giRestirWanted() -- i.e. only in giMode 1 (RTXDI ReSTIR GI). PSMainVoxi
    // and PSRayDriven read gGiRestirParams.w for their OWN poison-colour paint (the ray-traced
    // specular ceiling clamp, voxi.hlsl -- see F5 in the audit this fixes) regardless of which giMode
    // is running, and those shaders execute on every frame the scene renders normally, not only the
    // frames shadowHistoryActive() lets reach the rest of this function. Publishing giPoisonView_
    // unconditionally, this early, means giMode 0 -- the default -- gets a live flag instead of
    // whatever giMode 1 last left behind (0 if ReSTIR has never run this session, but a STALE stuck
    // 1.0 the instant it has, since the block below would then stop updating it the moment giMode
    // flips back to 0). The write further down, inside the giRestirWanted() block, is now redundant
    // with this one and has been removed rather than kept as a silently-agreeing second source of
    // truth for the same cbuffer field.
    cb_.giRestirParams[3] = giPoisonView_ ? 1.0f : 0.0f;
    // ---- U1/2.9/2.11: gAmbientParams.w, THE SINGLE WRITER ----
    // Published here, before the early return just below, for the identical reason F5's
    // giRestirParams[3] write immediately above is: PSMainVoxi/PSRayDriven decode this every frame
    // the scene renders normally, not only the frames that reach the giSurf block further down (which
    // runs only under giRestirWanted()) -- a giMode-0 frame, or one where the fourth/fifth pair
    // failed to bind, still needs a live mode/blended-cone/replay/path-view word rather than whatever
    // a giMode-1 frame last left behind. histBound and histValid are FALSE here -- neither is known
    // true until the giSurf block below confirms the sixth pair is actually bound this frame -- and
    // that block recomputes and overwrites this same component once it knows better. See
    // givis::packAmbientW's own comment (GiVisibility.hpp) for the bit table this shares byte-for-byte
    // with voxi.hlsl/voxi_restir.hlsli/voxi_gi.hlsli's own gAmbientParams.w decode.
    const u32 ambW = givis::packAmbientW(giRestirVisibility_, /*histBound=*/false, /*histValid=*/false,
                                         blendedGiCone_, dev_ && dev_->backend() == rhi::Backend::D3D12,
                                         giVisPathView_, giRestirSpatialSamples_, giRestirMaxHistory_);
    cb_.ambientParams[3] = static_cast<f32>(ambW);
    if (!shadowHistoryActive()) {
        // ---- F3: A SKIPPED FRAME MUST NOT LEAVE THE VALIDITY FLAGS TRUSTING FROZEN STATE ----
        //
        // Before this, rtHistValid_/giHistValid_ were set false only at init/resize/teardown/console
        // resets -- NEVER on a frame that simply skipped (an empty TLAS build leaving rtActive_
        // false, or the debug raymarch taking over the scene). Such a frame leaves every history
        // texture and prevViewProj_/prevSceneViewport_ exactly as they were, while the validity flags
        // still said "usable" -- so the NEXT active frame would reproject against a FROZEN camera as
        // if it were one frame old, not however many frames were actually skipped. Setting both false
        // here, exactly once per skipped frame (this function runs once per frame; this branch is the
        // only place a skip is detected), makes the next active frame start from no history instead --
        // one extra noisy frame, the same cost a resize or a console reset already pays, rather than a
        // silently wrong reprojection.
        //
        // nrdPrevCameraValid_ (the NRD-specific previous-camera latch from CameraFactor -- see the NRD
        // block below and its own comment) is invalidated for the identical reason: it is exactly as
        // frozen as prevViewProj_ across the same gap, and NRD's own resetHistory already follows from
        // !rtHistValid_ a few lines below (fs.resetHistory = fs.resetHistory || ... || !rtHistValid_),
        // so demoting it here is the one extra flag that reasoning does not already cover for free.
        //
        // NOT resetGiHistory()/resetRtHistory(): those are the NAMED, user-facing console commands and
        // each logs an AVER_INFO line -- calling them here would log every single skipped frame (an
        // empty scene or a debug-view session could mean every frame) and would blur "the user asked
        // for a reset" with "a skip made a reset unavoidable". This sets the same two flags directly,
        // silently, because it is a structural invariant of a skipped frame, not a user action. See
        // shadowHistoryActive()'s own comment for confirmation that in this project's steady state
        // (ray tracing on, NRD on, no debug view, and therefore at least one draw building a non-empty
        // TLAS every frame) this branch is never taken at all -- rtActive_ stays true, the four history
        // textures stay allocated once ray tracing is on, and debugViewActive() stays false, so nothing
        // here resets history on a frame that was not actually skipped.
        rtHistValid_ = false;
        giHistValid_ = false;
        // U1/2.11: the sixth pair rides the identical F3 reasoning above -- a skipped frame leaves
        // giVisHist_'s contents exactly as frozen as giSurfPosHist_/giSurfNrmHist_'s, and the giSurf
        // block below (which is what would otherwise clear this) does not run on a skipped frame
        // either.
        giVisHistValid_ = false;
        nrdPrevCameraValid_ = false;
        nrdPrev2CameraValid_ = false;
        nrdAoRanLastFrame_ = false;   // this return skips the per-frame update at the function's end
        return;
    }

    const u32 writeIdx = rtHistWriteIdx_;
    const u32 readIdx  = 1 - writeIdx;

    // The write texture rests as ShaderResource between frames; make it writable. The read texture
    // is still sitting in UnorderedAccess from when IT was last frame's write target, whenever that
    // happened on some earlier ACTIVE frame since the pair was (re)created -- flip it back to
    // readable regardless of whether its contents are trusted (rtHistPrimed_ tracks the resource
    // state; rtHistValid_, below, answers the separate question of whether to feed it to the
    // shader). Both pairs share writeIdx/readIdx: they always swap together (see the member comment
    // in VoxiRenderer.hpp).
    ctx.textureBarrier(rtShadowHist_[writeIdx], rhi::ResourceState::ShaderResource, rhi::ResourceState::UnorderedAccess);
    ctx.textureBarrier(rtReflHist_[writeIdx],   rhi::ResourceState::ShaderResource, rhi::ResourceState::UnorderedAccess);
    if (rtAoHist_[writeIdx]) ctx.textureBarrier(rtAoHist_[writeIdx], rhi::ResourceState::ShaderResource, rhi::ResourceState::UnorderedAccess);
    if (rtHistPrimed_) {
        ctx.textureBarrier(rtShadowHist_[readIdx], rhi::ResourceState::UnorderedAccess, rhi::ResourceState::ShaderResource);
        ctx.textureBarrier(rtReflHist_[readIdx],   rhi::ResourceState::UnorderedAccess, rhi::ResourceState::ShaderResource);
        if (rtAoHist_[readIdx]) ctx.textureBarrier(rtAoHist_[readIdx], rhi::ResourceState::UnorderedAccess, rhi::ResourceState::ShaderResource);
    }

    res_->setUav(bindings_, 2, rtShadowHist_[writeIdx], 0);
    res_->setSrv(bindings_, 6, rtShadowHist_[readIdx]);
    res_->setUav(bindings_, 3, rtReflHist_[writeIdx], 0);
    res_->setSrv(bindings_, 7, rtReflHist_[readIdx]);
    // gRtDenoiseParams.w: 1 only while the ambient pair is genuinely bound this frame. The shader
    // must not touch a null UAV, and unlike the other two pairs this one is absent at Low and Medium
    // by design -- so it cannot ride gRtHistParams.x with the others.
    // rtAoHitDist_ IS PART OF THE CONDITION even though it is not rebound here. The flag's whole
    // job is to tell the shader which of these slots it may touch, and u5 is one of them -- a frame
    // that somehow had the pair but not the hit-distance target would otherwise be told to write a
    // null UAV, which is undefined rather than merely wasteful. They are created together, so this
    // can only ever differ if that changes; then it fails safe instead of silently.
    if (rtAoHist_[writeIdx] && rtAoHist_[readIdx] && rtAoHitDist_) {
        res_->setUav(bindings_, 4, rtAoHist_[writeIdx], 0);
        res_->setSrv(bindings_, 11, rtAoHist_[readIdx]);
        cb_.rtDenoiseParams[3] = 1.0f;
    }

    // ---- LOCAL LIGHTS (LAMPS): u19/t19, swapped on the SAME writeIdx/readIdx as the shadow pair ----
    // The same transitions as rtShadowHist_ above, SRV side included: t19 rests in ShaderResource because
    // the raster and single-pass scene passes read it from pixel shaders; CSRdLocalLights, its one compute
    // reader, visits NonPixelShaderResource around its own dispatch (recordStagedRayDriven). Own primed
    // flag, since the pair can appear on an rdLocalHistWanted() edge independently of the shadow pair's.
    if (rdLocalHist_[writeIdx] && rdLocalHist_[readIdx]) {
        ctx.textureBarrier(rdLocalHist_[writeIdx], rhi::ResourceState::ShaderResource,
                           rhi::ResourceState::UnorderedAccess);
        if (rdLocalHistPrimed_)
            ctx.textureBarrier(rdLocalHist_[readIdx], rhi::ResourceState::UnorderedAccess,
                               rhi::ResourceState::ShaderResource);
        res_->setUav(bindings_, 19, rdLocalHist_[writeIdx], 0);
        res_->setSrv(bindings_, 19, rdLocalHist_[readIdx]);
        rdLocalOutThisFrame_ = rdLocalHist_[writeIdx];
    }

    // ---- RTXDI ReSTIR GI: the fourth/fifth pair, swapped in the SAME lockstep, own condition ----
    // giRestirWanted() ⊆ rayTracingWanted() ⊆ (modulo !debugViewActive()) the condition that got
    // this far at all, so "all four handles exist" is the only extra test needed here -- unlike the
    // ao pair, this pair does not ride the shared rtHistValid_ (see giHistValid_'s own comment for
    // why that would be wrong the one frame giMode is switched on after RT has already been
    // running), so its OWN flag is what tells the shader whether the READ side holds a real frame.
    // BOTH TEXTURES SWAP TOGETHER: they describe one logical surface (position, normal) split only
    // because rhi::Format has no four-channel 32-bit float -- a reader that got position from this
    // frame's write and normal from last frame's read (or the reverse) would silently pair two
    // different surfaces.
    if (giSurfPosHist_[writeIdx] && giSurfPosHist_[readIdx] &&
        giSurfNrmHist_[writeIdx] && giSurfNrmHist_[readIdx]) {
        ctx.textureBarrier(giSurfPosHist_[writeIdx], rhi::ResourceState::ShaderResource, rhi::ResourceState::UnorderedAccess);
        ctx.textureBarrier(giSurfNrmHist_[writeIdx], rhi::ResourceState::ShaderResource, rhi::ResourceState::UnorderedAccess);
        if (giHistPrimed_) {
            ctx.textureBarrier(giSurfPosHist_[readIdx], rhi::ResourceState::UnorderedAccess, rhi::ResourceState::ShaderResource);
            ctx.textureBarrier(giSurfNrmHist_[readIdx], rhi::ResourceState::UnorderedAccess, rhi::ResourceState::ShaderResource);
        }
        res_->setUav(bindings_, 7, giSurfPosHist_[writeIdx], 0);
        res_->setSrv(bindings_, 12, giSurfPosHist_[readIdx]);
        res_->setUav(bindings_, 8, giSurfNrmHist_[writeIdx], 0);
        res_->setSrv(bindings_, 13, giSurfNrmHist_[readIdx]);
        cb_.giRestirParams[0] = 1.0f;                       // t12/t13/u6/u7/u8 are bound this frame
        // ---- A MOVED SUN VOIDS THE RESERVOIRS FOR THE FRAME IT MOVES, the same test rtHistParams[1]
        // applies to the shadow history further down this function. A reservoir keeps the radiance its
        // sample was shaded with, so after a sun move every reused sample still carries the OLD sun.
        // MEASURED on PTTest's NewSponza, fixed camera and exposure, linear radiance x1000 on the arcade
        // wall: moving the sun from 71.7 to 47.2 degrees left 12.2 three frames later against a settled
        // 1.8, and letting go took 40-150 frames -- the same with NRD off and at any REBLUR history
        // depth, so the lag was never the denoiser's. Treating this frame as having no history restarts
        // each pixel from its fresh candidate, traced under the new sun, and the next frame reuses those:
        // with this and reuse history 1 (voxi_restir.hlsli) the same move read 9.7 / 2.6 / 1.7 at
        // +3 / +17 / +42 frames against 12.2 / 8.7 / 3.8 before. NRD's own history is not reset, so a
        // drag stays denoised, but it is kept short while the sun moves (Settings::
        // reblurSunMovingFrameNum) -- at full depth it was the rest of the lag after a drag let go.
        //
        // A JUMP, NOT EVERY CHANGE (rtHistSunJumped). Voiding on ANY change made a slider DRAG a run of
        // no-history frames: every frame restarted every pixel from one fresh candidate, so the
        // indirect light was raw one-sample noise -- bright sky hits as white speckle all over the
        // arcade, which is what the owner saw while moving the light. One drag step moves the sun a
        // few degrees, so the radiance a reused reservoir carries is off by that much for about one
        // frame at reuse history 1. The measurement above was a 24.5-degree jump, which still voids.
        cb_.giRestirParams[1] = (giHistValid_ && !rtHistSunJumped()) ? 1.0f : 0.0f;  // ...and t12/t13 hold a real previous frame
        cb_.giRestirParams[2] = static_cast<f32>(writeIdx);  // this frame's reservoir array slice
        // giRestirParams[3] (the poison-view flag; was "spare", repurposed rather than a new field --
        // see setGiPoisonView's own comment and giRestirIndirect's POISON DEBUG VIEW block,
        // voxi_restir.hlsli, for what it drives) is published UNCONDITIONALLY near the top of this
        // function now, not here -- see that site's own F5 comment for why giMode 0 needed the same
        // flag this block used to be the only writer of.
        // u9, the radiance this frame hands NRD. Bound with the rest of the ReSTIR slots because
        // gGiRestirParams.x -- already set above -- is exactly the flag the shader's write is
        // guarded on, so the two can never disagree about whether the slot is real.
        if (giRadiance_) res_->setUav(bindings_, 9, giRadiance_, 0);

        // ---- U1/2.11: the sixth pair's own swap, when both giVisHist_ handles exist ----
        // giVisHistWanted() implies giRestirWanted(), so this block already runs under the same
        // condition that gated ensureShadowHistory's own creation of the pair -- the handle check is
        // what tells this frame whether that creation actually succeeded (see the failure branch
        // there: it destroys both rather than returning false, so a device out of memory leaves this
        // pair silently absent and the rest of ReSTIR GI running normally).
        if (giVisHist_[writeIdx] && giVisHist_[readIdx]) {
            ctx.textureBarrier(giVisHist_[writeIdx], rhi::ResourceState::ShaderResource,
                               rhi::ResourceState::UnorderedAccess);
            // THE READ SIDE IS GATED ON giVisHistPrimed_, NOT giHistPrimed_ -- the two pairs' primed
            // state can disagree (giVisHistWanted() is a narrower condition that can start or stop
            // wanting this pair on a frame the fourth/fifth pair's own state is untouched by), the
            // same reason giHistValid_ itself is not shared with rtHistValid_.
            if (giVisHistPrimed_)
                ctx.textureBarrier(giVisHist_[readIdx], rhi::ResourceState::UnorderedAccess,
                                   rhi::ResourceState::ShaderResource);
            res_->setUav(bindings_, 10, giVisHist_[writeIdx], 0);
            res_->setSrv(bindings_, 16, giVisHist_[readIdx]);
            // Recomputed with histBound = true and histValid = giVisHistValid_, now that both are
            // actually known -- overwrites the FALSE/FALSE word this function published near the top,
            // before shadowHistoryActive() was even known true. Every other component of ambW (mode,
            // blendedGiCone_, the D3D12-only blended-replay bit, giVisPathView_, giRestirSpatialSamples_,
            // giRestirMaxHistory_) is unchanged from that first write, so this is not a second
            // source of truth for them, only the two bits that could not be known until now.
            const u32 ambW2 = givis::packAmbientW(giRestirVisibility_, /*histBound=*/true,
                                                  giVisHistValid_, blendedGiCone_,
                                                  dev_ && dev_->backend() == rhi::Backend::D3D12,
                                                  giVisPathView_, giRestirSpatialSamples_,
                                                  giRestirMaxHistory_);
            cb_.ambientParams[3] = static_cast<f32>(ambW2);
        }
    }

    // ---- NVIDIA NRD, denoising LAST FRAME's sky-occlusion hit distance into t14 ----
    //
    // LAST FRAME'S, AND THAT IS NOT A COMPROMISE -- it is the only ordering this pass can have.
    // rtAoHitDist_ (u5) is written by the scene pixel shader, which has not run yet when
    // beginShadowHistory records; running the denoiser here filters the measurement the previous
    // frame left behind. That is exactly what every temporal filter in this file already consumes
    // (the u4/t11 pair is read one frame after it is written), so the lag is the same one the
    // hand-written path already has, not a new one -- and NRD is built to be fed a history.
    //
    // EVERY INPUT BUT THE HIT DISTANCE IS THE G-BUFFER'S, which is off by default. Nothing here
    // turns it on: a renderer that silently enabled an extra four render targets because a denoiser
    // wanted them would be spending the user's frame budget on a decision they never made. The pass
    // reports itself absent instead, and Voxi falls back to the filter it has always shipped.
    nrdOutput_ = 0;
    nrdGiOutput_ = 0;
    // MILESTONE 4: reset alongside nrdGiOutput_ immediately above, for the identical reason -- a frame
    // that skips the NRD dispatch below (gbufWritten false, neither signal present, camera factorisation
    // failed, or NRD rejects the resize) must not leave recordStagedRayDriven believing the readback is
    // still live from whenever it last actually ran. Set true only where nrdGiOutput_ itself is assigned
    // below, from a successful nrd_.record() call, never assumed from nrdGiOutput_'s own truthiness (see
    // that assignment's own comment for why the two are not the same test this frame).
    //
    // PAST the shadowHistoryActive() early return, like the two resets above it, so an inactive frame
    // leaves all three at the last active frame's values. Harmless: recordStagedRayDriven, the only
    // reader, runs only when rdStagedActive() has required shadowHistoryActive() this same frame.
    nrdGiRanThisFrame_ = false;
    // gBufferEnabled() IS NOT THE SAME QUESTION AS "the G-buffer has anything in it", and the
    // difference is a silent one. D3D12 requires every render target in one OMSetRenderTargets call
    // to share a sample count, and the G-buffer's three targets are always single-sample -- so under
    // MSAA the backend CLEARS them and does not write them, saying so once at WARN. A denoiser fed
    // cleared normals, a cleared view Z and cleared motion vectors does not fail: it produces a
    // confident, uniformly wrong image, which is the worst outcome available here. Skipping is the
    // honest answer, and it is said once rather than every frame.
    const bool gbufWritten = dev_ && dev_->gBufferEnabled() && dev_->sampleCount() == 1;
    if (dev_ && dev_->gBufferEnabled() && dev_->sampleCount() != 1 && !nrdWarnedMsaa_) {
        AVER_WARN("[NRD] denoising is OFF: the G-buffer is enabled but MSAA is {}x, so the backend "
                  "clears its targets without writing them and every NRD input would be blank. Set "
                  "MSAA to 1 (voxi.msaa 1) to denoise.", dev_->sampleCount());
        nrdWarnedMsaa_ = true;
    }
    // EITHER SIGNAL IS ENOUGH -- see the creation site above for why demanding the occlusion one
    // silently disabled GI denoising for a whole session. Only the denoisers whose input actually
    // exists are selected below; handing NRD one with a blank input is the single most expensive way
    // to be wrong here, which is what made the original coupling look defensible.
    // ---- THE OCCLUSION DENOISER IS SKIPPED WHEN NOTHING WILL READ ITS OUTPUT ----
    //
    // Its output lands in t14 as gNrdAo, and the ENTIRE tree reads gNrdAo in exactly one place:
    // rtSkyOcclusionTemporal (voxi_rt.hlsli), behind `nrdAoUsable && gAverHistoryWrite`. Under
    // ray-driven primary visibility every route to that read is closed, and each one is checkable:
    //   - PSRayDriven passes nrdAoUsable = false (voxi.hlsl's ray-driven call site; df4122cc).
    //   - No OPAQUE draw reaches PSMainVoxi at all: suppressesScene() is true whenever
    //     rayDrivenActive(), and D3D12Device/VulkanDevice::drawMesh return straight after
    //     submitDraw when any feature suppresses the scene.
    //   - Glass still reaches PSMainVoxi through the blended replay (D3D12). A replayed fragment
    //     whose MATERIAL is translucent (averDrawIsTranslucent) has gAverHistoryWrite false -- the
    //     second half of that same gate -- UNLESS legacy bit 32 (voxi.legacyBlendedHistoryWrite)
    //     turns it back on, in which case the denoiser keeps running (see nrdAoSignal below).
    //   - NOT CLOSED, AND SAID SO: a blended draw with an OPAQUE material -- GameWater's
    //     unauthored-water fallback (GameWater.cpp, blended=true on the default MaterialDesc) --
    //     keeps gAverHistoryWrite TRUE and did read t14 under ray-driven mode. It now reads its own
    //     accumulated occlusion instead of the denoised occlusion of the surface behind it, which is
    //     the mis-attribution the W6 gate exists to remove, but it IS a pixel change. It needs the
    //     simulated-fluids build and a water volume with no .ocmat. Found by adversarial review.
    // So in that mode REBLUR_DIFFUSE_OCCLUSION was being dispatched every frame to fill a texture
    // with no reader. Measured before this, ray-driven on PTTest: "Voxi NRD denoise 2.18ms" for the
    // occlusion and radiance denoisers together; this drops the occlusion one. THE SAVING IS NOT
    // MEASURED -- read the same span with --gpu-timing to get it.
    //
    // PIXEL-NEUTRAL in ray-driven mode EXCEPT the opaque-material blended draw above. In raster mode
    // nothing changes: the signal is computed exactly as before. rayDrivenActive() is THIS frame's answer
    // here -- buildAccelerationStructures has already set rtActive_ -- and it is the same predicate
    // scenePass uses later in the frame to choose the ray-driven pass, so the two cannot disagree.
    //
    // The one behavioural change is at a MODE SWITCH, and it is deliberate: see the history reset
    // below the plan, keyed on nrdAoRanLastFrame_.
    const bool nrdGiSignal = giRadiance_ && giRestirWanted();
    // Legacy bit 32 (voxi.legacyBlendedHistoryWrite) re-opens the blended-fragment read of gNrdAo
    // (voxi.hlsl: gAverHistoryWrite = !blendedFragment || bit 32), so the denoiser has to keep
    // running for that comparison to mean what it did. Toggling the bit flips this signal, and the
    // rejoin reset below already covers that edge.
    const bool nrdAoSignal = rtAoHitDist_ != 0 &&
                             (!rayDrivenActive() || (lightingLegacyBits_ & 32u) != 0u);
    if (nrd_.valid() && gbufWritten && (nrdAoSignal || nrdGiSignal)) {
        render::nrd::Recorder::Inputs in;
        in.viewZ           = dev_->gBufferViewZTexture();
        in.motionVectors   = dev_->gBufferVelocityTexture();
        in.normalRoughness = dev_->gBufferNormalRoughnessTexture();
        in.diffuseHitDist  = rtAoHitDist_;
        in.diffuseRadianceHitDist = giRadiance_;
        // SIZED FROM WHICHEVER SIGNAL IS THERE. Both are allocated at the render resolution so they
        // agree when both exist; asking the occlusion target for dimensions when only the radiance
        // one was allocated is how "GI denoising without sky occlusion" would resolve to 0x0 and skip
        // even once the gate above let it through.
        rhi::TextureDesc sizeDesc{};
        const rhi::TextureHandle sizeFrom = rtAoHitDist_ ? rtAoHitDist_ : giRadiance_;
        const bool haveSize = res_->textureInfo(sizeFrom, sizeDesc);
        const u32 tw = haveSize ? sizeDesc.width : 0u, th = haveSize ? sizeDesc.height : 0u;

        // ---- 3.4 b: THE NRD RECT VS RESOURCE GUARD ----
        //
        // CORRECT BY CONSTRUCTION ON EVERY ORDINARY FRAME (3.4 a): onRenderTargetsChanged REALLOCATES
        // every Voxi target -- the G-buffer included -- at the reduced internal size AverSR asks for,
        // so tw/th above and the G-buffer's own resource size agree without this ever running its
        // mismatch branch. What this guards against is the ORDINARY frame's assumption breaking for
        // ONE frame: a render-scale change lands between two frames (the D3D12/Vulkan "park, apply at
        // beginFrame" pattern 3.3 D/C2-13 both use), or a resize the device has not yet caught up
        // with. NRD has no story for a rect that does not match its own inputs' resource size --
        // undefined, not merely wrong -- so this is checked here, once, against all three G-buffer
        // inputs rather than trusted from tw/th alone. The same "find the line that CONSUMES it"
        // posture the hit-distance-unit and motion-vector-scale comments above this block already
        // take, and the same precedent the backdrop's own size check set (D3D12Device.cpp's
        // sceneColorBackdropTexture consumer) after ITS absence once removed the device.
        rhi::TextureDesc gzDesc{}, gmvDesc{}, gnrDesc{};
        const bool haveGbufSizes = in.viewZ && in.motionVectors && in.normalRoughness &&
                                   res_->textureInfo(in.viewZ, gzDesc) &&
                                   res_->textureInfo(in.motionVectors, gmvDesc) &&
                                   res_->textureInfo(in.normalRoughness, gnrDesc);
        const bool gbufSizeOk = haveSize && haveGbufSizes &&
                                gzDesc.width == tw && gzDesc.height == th &&
                                gmvDesc.width == tw && gmvDesc.height == th &&
                                gnrDesc.width == tw && gnrDesc.height == th;
        // WARNED ONCE PER MISMATCH EPISODE, NOT ONCE EVER -- unlike nrdWarnedMsaa_/nrdWarnedEncoding_
        // above, this condition is expected to clear itself (the next reallocation catches back up),
        // and a LATER, unrelated episode should still be reported rather than silenced by an earlier
        // one already having fired. Cleared the moment the sizes agree again.
        if (haveSize && haveGbufSizes && !gbufSizeOk) {
            nrd_.forceHistoryReset();
            if (!nrdWarnedInputSizeMismatch_) {
                nrdWarnedInputSizeMismatch_ = true;
                AVER_WARN("[NRD] input size mismatch ({}x{} signal vs {}x{} G-buffer); skipping this "
                          "frame", tw, th, gzDesc.width, gzDesc.height);
            }
        } else if (gbufSizeOk) {
            nrdWarnedInputSizeMismatch_ = false;
        }
        if (gbufSizeOk && tw && th && nrd_.resize(tw, th)) {
            render::nrd::FrameSettings fs{};
            fs.resourceWidth = fs.rectWidth  = tw;
            fs.resourceHeight = fs.rectHeight = th;
            fs.denoisingRange  = settings_.giMaxDistance > 1.0f ? settings_.giMaxDistance : 500000.0f;

            // ---- THE FOURTH SILENT NRD CONTRACT BUG IN THIS BLOCK (beside the hit-distance unit,
            // the normal/roughness packing, and the motionVectorScale unit below): worldToView and
            // viewToClip were NEVER SEPARATE, and NRD does not treat them as interchangeable with
            // their product ----
            //
            // THE OLD COMMENT HERE CLAIMED "NRD composes the two to reconstruct position, so the
            // product it actually uses is unchanged". THAT IS FALSE, verified against the vendored
            // source rather than assumed. third_party/nrd/Source/InstanceImpl.cpp:384 runs
            // DecomposeProjection on viewToClip ALONE -- never on a product -- to recover the
            // projection's frustum AND to decide whether the scene reads as left- or right-handed
            // (PROJ_LEFT_HANDED); :386-397 then conditionally negates row 2 of BOTH viewToClip and
            // worldToView (worldToView via a transpose, a row negate, and a transpose back) depending
            // on that one decision. With identity standing in for worldToView, the handedness verdict
            // -- and therefore whether that flip happens at all -- was driven entirely by whatever
            // rotation the camera happened to have baked into the COMBINED viewProj that frame, i.e.
            // by which way the camera was facing, never by anything about the scene that could
            // actually change it. REBLUR_TemporalAccumulation.cs.hlsl:88-89 reconstructs every
            // pixel's CURRENT view-space position from viewToClip's frustum alone and rotates it into
            // world space with gViewToWorld (worldToView's inverse); :144-163 reprojects the PREVIOUS
            // position the identical way through gFrustumPrev/gWorldToViewPrev/gCameraDelta. Every one
            // of those reads WORLD space when worldToView is identity -- so the reprojection, the
            // plane-distance disocclusion test and the parallax term were all evaluated against wrong
            // positions, worst exactly while the camera moves, which is when a denoiser has the most
            // work to do.
            //
            // THE FIX has two parts. First, read THIS frame's camera FRESH (dev_->camera(), not
            // curViewProj_ -- curViewProj_ is written by fitCascades(), called from shadowPass() AFTER
            // this function returns, so on every steady frame it still holds LAST frame's camera; this
            // was bug (a) in the audit that found this block, alongside the identity encoding as bug
            // (b)). Second, factorise that combined matrix with CameraFactor
            // (aver/voxi/CameraFactor.hpp) into the separate worldToView/viewToClip NRD actually reads,
            // in the SAME row-major layout the combined viewProj already used here -- so this stays a
            // straight memcpy into fs.worldToView/viewToClip, of two matrices instead of one matrix
            // asked to be both.
            //
            // voxi.nrdLegacyCamera (default OFF) reinstates the exact OLD, WRONG encoding below for an
            // A/B comparison against this fix without a rebuild -- see its own console entry.
            f32 camVp[16] = {}, camEye[3] = {};
            // No inverse needed: CameraFactor factors camVp itself, and nothing here unprojects.
            const bool haveCamera = dev_ && dev_->camera(camVp, nullptr, camEye);
            bool cameraReady = false;
            if (nrdLegacyCamera_) {
                // EXACTLY today's pre-fix encoding, both halves of it: identity worldToView, and
                // curViewProj_/prevViewProj_ (LAST frame's camera on every steady frame, not this
                // frame's fresh one) for viewToClip -- reproducing bug (a) alongside bug (b) is the
                // point of a faithful comparison, not an oversight.
                fs.worldToView[0]     = fs.worldToView[5]     = fs.worldToView[10]     = fs.worldToView[15]     = 1.0f;
                fs.worldToViewPrev[0] = fs.worldToViewPrev[5] = fs.worldToViewPrev[10] = fs.worldToViewPrev[15] = 1.0f;
                std::memcpy(fs.viewToClip,     curViewProj_,  sizeof(fs.viewToClip));
                std::memcpy(fs.viewToClipPrev, prevViewProj_, sizeof(fs.viewToClipPrev));
                cameraReady = true;
            } else if (f32 curW2V[16], curV2C[16];
                       haveCamera && CameraFactor::factor(camVp, curW2V, curV2C)) {
                // NRD keeps ITS OWN previous camera (nrdPrevWorldToView_/nrdPrevViewToClip_), latched
                // below only once this frame's camera is known good. Deliberately separate from
                // curViewProj_/prevViewProj_: those describe the COMBINED matrix every other
                // reprojection consumer in this file wants, a different shape than the factorised pair
                // NRD needs, so reusing them would still pair a factorised CURRENT camera with a
                // combined PREVIOUS one.
                //
                // ---- AND THE CURRENT CAMERA IS LAST FRAME'S, because the inputs are ----
                //
                // This dispatch runs in beginShadowHistory, BEFORE this frame's scene pass: the view
                // Z, motion vectors, normals and radiance it reads were all written by LAST frame's
                // pixel shader, and that frame's velocity is last-frame-minus-the-frame-before. The
                // camera pair that describes them is (last frame, the frame before), not (this frame,
                // last frame). Handing NRD this frame's camera -- which an earlier audit did, reading
                // "last frame's camera" as a staleness bug -- puts its position reconstruction, plane
                // distance and disocclusion tests one frame ahead of the data they judge.
                //
                // MEASURED, AND IT DID NOT MATTER: REBLUR finds history through the motion vectors,
                // which are right either way, and the matrices only feed its plane and parallax tests.
                // Consistent pairing moved motion grain on NRD's GI slightly UP (2.444 -> 2.504) with
                // no structural difference, so Settings::nrdCameraMatchesInputs defaults OFF (the
                // shipped pairing) and stays as a dial. The one-frame ghost that IS real lives in the
                // shader's readback of NRD's output, fixed there (voxi_restir.hlsli).
                const bool matchInputs = settings_.nrdCameraMatchesInputs;
                const bool hadPrevCamera  = nrdPrevCameraValid_;
                const bool hadPrev2Camera = nrdPrevCameraValid_ && nrdPrev2CameraValid_;
                if (matchInputs && hadPrevCamera) {
                    std::memcpy(fs.worldToView, nrdPrevWorldToView_, sizeof(fs.worldToView));
                    std::memcpy(fs.viewToClip,  nrdPrevViewToClip_,  sizeof(fs.viewToClip));
                } else {
                    std::memcpy(fs.worldToView, curW2V, sizeof(fs.worldToView));
                    std::memcpy(fs.viewToClip,  curV2C, sizeof(fs.viewToClip));
                }
                const bool havePrev = matchInputs ? hadPrev2Camera : hadPrevCamera;
                if (havePrev) {
                    std::memcpy(fs.worldToViewPrev, matchInputs ? nrdPrev2WorldToView_ : nrdPrevWorldToView_,
                                sizeof(fs.worldToViewPrev));
                    std::memcpy(fs.viewToClipPrev,  matchInputs ? nrdPrev2ViewToClip_  : nrdPrevViewToClip_,
                                sizeof(fs.viewToClipPrev));
                } else {
                    // NO VALID PREVIOUS CAMERA YET -- the first frame(s) NRD ever runs, or the first
                    // active frame after beginShadowHistory's own skipped-frame branch invalidated the
                    // latch above. Passing the current camera for both halves makes the reprojection a
                    // no-op (zero parallax, zero motion) instead of reprojecting against an
                    // uninitialised array, and fs.resetHistory below throws away whatever the texture
                    // history still holds from before the gap regardless.
                    std::memcpy(fs.worldToViewPrev, fs.worldToView, sizeof(fs.worldToViewPrev));
                    std::memcpy(fs.viewToClipPrev,  fs.viewToClip,  sizeof(fs.viewToClipPrev));
                }
                // SHIFTED, then latched: the frame before becomes two frames ago.
                if (hadPrevCamera) {
                    std::memcpy(nrdPrev2WorldToView_, nrdPrevWorldToView_, sizeof(nrdPrev2WorldToView_));
                    std::memcpy(nrdPrev2ViewToClip_,  nrdPrevViewToClip_,  sizeof(nrdPrev2ViewToClip_));
                }
                nrdPrev2CameraValid_ = hadPrevCamera;
                std::memcpy(nrdPrevWorldToView_, curW2V, sizeof(nrdPrevWorldToView_));
                std::memcpy(nrdPrevViewToClip_,  curV2C, sizeof(nrdPrevViewToClip_));
                nrdPrevCameraValid_ = true;
                // A MODE SWITCH restarts the history: the two pairings describe the history as one
                // frame apart, and blending across the switch would reproject by the wrong delta.
                fs.resetHistory = !havePrev || matchInputs != nrdCameraMatchedLast_;
                nrdCameraMatchedLast_ = matchInputs;
                cameraReady = true;
            }

            if (!cameraReady) {
                // FACTORISATION FAILED, OR THE DEVICE HAD NO CAMERA TO GIVE, AND THE LEGACY SWITCH IS
                // OFF. NEVER fall through to the old identity encoding silently -- that would be the
                // exact bug this rewrite removes, just reappearing on an error path instead of every
                // frame. This block already has an honest answer for "an input is missing this frame":
                // the same one gbufWritten/tw/th/nrd_.resize() failing already take a little further up
                // -- skip the dispatch entirely. nrdOutput_/nrdGiOutput_ stay at 0 (already reset above
                // this function's early return), the shader's own GetDimensions() test already reads
                // that as "not denoised this frame" through slots 14/15 (cleared just below this
                // function), and forceHistoryReset() makes the NEXT successful frame reset rather than
                // reproject through the gap. Logged once: a condition that cannot self-heal by waiting
                // would just be noise repeated every frame.
                if (!nrdWarnedCameraFactor_) {
                    AVER_WARN("[NRD] this frame's camera did not factorise into a worldToView/"
                              "viewToClip pair NRD can trust ({}); skipping the NRD dispatch this "
                              "frame rather than handing it a wrong or stale camera encoding. Neither "
                              "an orthographic nor a mirrored camera is something this engine's own "
                              "call sites (SandboxApp.cpp, GameApp.cpp) produce, so this firing in "
                              "practice means camera construction changed somewhere and this is the "
                              "first symptom of it.",
                              haveCamera ? "CameraFactor::factor rejected it" : "dev_->camera() had none to give");
                    nrdWarnedCameraFactor_ = true;
                }
                nrd_.forceHistoryReset();
                nrdPrevCameraValid_ = false;
                nrdPrev2CameraValid_ = false;
            } else {
                fs.frameIndex = nrdFrame_++;
                // ---- THE SCALE CARRIES A SIGN *AND* A UNIT CONVERSION, AND THE UNIT WAS MISSING ----
                //
                // THE SIGN, which this comment used to be entirely about: the G-buffer's velocity is
                // DESTINATION minus SOURCE (this frame's position minus last frame's, see
                // averGBufferVelocity) and NRD reprojects by ADDING the motion vector to find where a
                // pixel CAME FROM. Opposite conventions, hence negative.
                //
                // THE UNIT, which was wrong for as long as this pass has existed. NRD consumes the
                // vector as `float2 smbPixelUv = pixelUv + mv.xy` (REBLUR_TemporalAccumulation.cs.hlsl),
                // where `pixelUv = (pixelPos + 0.5) * gRectSizeInv` -- a NORMALISED [0,1] UV. So NRD
                // wants the motion in UV. averGBufferVelocity returns PIXELS (curPx - prevPx). Scaling
                // by -1 alone therefore handed NRD a one-pixel camera movement as a ONE-UV jump: the
                // entire screen.
                //
                // WHAT THAT LOOKS LIKE, and it is the bug the user reported as "the white impression of
                // the geometry is just burned into the screen": every reprojection lands nowhere near
                // the pixel it describes, so the temporal history never lines up with the frame it is
                // being blended into. Instead of decaying, stale geometry accumulates in place and
                // burns in. It was never a ReSTIR defect -- it degrades the sky-occlusion denoiser
                // identically, since both read this same CommonSettings.
                //
                // DIVIDE BY THE RECT, NOT THE RESOURCE. gRectSizeInv is the RENDERED rect; under
                // dynamic resolution that is smaller than the allocation, and rectWidth/rectHeight
                // above are already resolved to the right one for exactly this reason.
                //
                // THE THIRD UNIT BUG OF THIS KIND HERE, after the hit-distance constant that read
                // metres as centimetres and the normal/roughness packing that read one layout as
                // another. All three were silent, all three produced a confident wrong image, and none
                // of them could fail a build. When a vendored library takes a number, find the line
                // that CONSUMES it.
                fs.motionVectorScale[0] = fs.rectWidth  ? -1.0f / static_cast<f32>(fs.rectWidth)  : -1.0f;
                fs.motionVectorScale[1] = fs.rectHeight ? -1.0f / static_cast<f32>(fs.rectHeight) : -1.0f;
                fs.motionVectorScale[2] =  0.0f;   // the engine's velocity carries no Z
                fs.resetHistory = fs.resetHistory || nrd_.historyIsStale() || !rtHistValid_;
                // ReSTIR GI's radiance only exists when ReSTIR GI is the estimator, so the diffuse
                // denoiser is selected only then. Handing NRD a denoiser whose input texture is blank
                // does not fail -- it filters the blank and hands it back confidently, which is the
                // most expensive way to be wrong available here.
                //
                // ---- REBLUR_DIFFUSE IS SELECTED NOW; WHAT USED TO KILL THE ADAPTER WAS THIS SEAM ----
                //
                // It removed the device, and the cause was never anything on this side of the call.
                // The recorder resolved NRD's OUT_DIFF_HITDIST and OUT_DIFF_RADIANCE_HITDIST slots by
                // indexing its PERMANENT POOL with ResourceDesc::indexInPool. NRD writes that field
                // only for the two pool resource types; for every IN_*/OUT_* type it stays 0 and the
                // resource is the integration's own to supply, looked up by resource TYPE -- which is
                // exactly what NRD's reference integration does. So both outputs aliased permanent pool
                // texture 0, a REBLUR instance's PREV_VIEWZ history, R32_SFLOAT.
                //
                // The occlusion denoiser survived that for as long as this pass has existed, which is
                // why it read as "REBLUR_DIFFUSE is the problem": its output pass is compiled
                // RWTexture2D<float>, so a one-channel write into a one-channel view faulted on nothing
                // and merely corrupted NRD's own depth history. REBLUR_DIFFUSE's is
                // RWTexture2D<float4>, and setUav builds the descriptor from the bound texture's format
                // with no override, so that one bound four components to a one-component UAV --
                // DEVICE_REMOVED on the first recorded frame. The bisection was reporting the
                // difference between float and float4, not between 15 dispatches and 34.
                //
                // THE EARLIER GUESS IN THIS COMMENT WAS WRONG and is recorded because it cost time:
                // giRadiance_'s resource state was named as the next thing to look at. It is not
                // implicated -- it has the same shape as the sky-occlusion signal, which works -- and
                // no PIX capture was needed in the end, only NRD's own integration source.
                //
                // See Recorder's outDiffHitDistTex_/outDiffRadHitDistTex_ for the fix, and
                // Denoiser::ReblurTuning for the hit-distance constants this signal also needed before
                // REBLUR could do anything useful with it.
                // Computed once above the gate -- see the skip's own comment there.
                const bool giSignal = nrdGiSignal;
                const bool aoSignal = nrdAoSignal;
                // THE OCCLUSION DENOISER REJOINING AFTER A GAP gets a fresh history. Its permanent pool
                // still holds whatever it accumulated before ray-driven mode started skipping it, and
                // REBLUR would reproject that with only ONE frame's camera delta -- ghosting of a view
                // the user left some time ago. FrameSettings is shared by every denoiser in the call,
                // so the radiance one restarts on that frame too; that is a single frame, at a
                // user-initiated mode switch that already changes the whole image.
                if (aoSignal && !nrdAoRanLastFrame_) fs.resetHistory = true;
                const u32* which  = (giSignal && aoSignal) ? kNrdAoAndGiDenoisers
                                  : giSignal               ? kNrdGiDenoiser
                                                           : kNrdAoDenoiser;
                const u32  whichN = (giSignal && aoSignal) ? 2u : 1u;
                // M1: nested under "Voxi shadow history" above -- the recorder's own dispatches
                // (REBLUR's classify/prepass/accumulate/... passes) get their own timing bracket
                // instead of being folded into the history pair's texture-barrier cost.
                // MILESTONE 4: whether REBLUR should read gRdGiTex's checkerboard packing this call.
                // Both halves have to hold -- giSignal, because CheckerboardMode only means anything on
                // the denoiser that is actually about to run against this input, and
                // nrdGiInputCheckerboard_ (latched at the top of this function from LAST frame's
                // recordStagedRayDriven, before this frame's own overwrote it), because the input this
                // dispatch is about to read is LAST frame's CSRdGi write, not this frame's. Applied only
                // on a CHANGE (nrdGiCbApplied_ caches the last value pushed) so a steady frame does not
                // re-call setReblurTuning() -- and its WARN-once rejection path -- every single frame.
                const u8 wantCb = (giSignal && nrdGiInputCheckerboard_) ? 1u : 0u;
                // The sun-moving history clamp (Settings::reblurSunMovingFrameNum): on for the frames
                // the sun moves and one after, since this dispatch reads last frame's GI write.
                if (rtHistSunMoved()) nrdSunMovingHold_ = 2u;
                else if (nrdSunMovingHold_ > 0u) --nrdSunMovingHold_;
                const bool sunClamp = nrdSunMovingHold_ > 0u;
                if (wantCb != nrdGiCbApplied_ || sunClamp != nrdSunClampApplied_) {
                    nrdGiCbApplied_ = wantCb;
                    nrdSunClampApplied_ = sunClamp;
                    applyReblurTuning();
                }
                {
                    rhi::ScopedGpuStat nrdStat(ctx, "Voxi NRD denoise");
                    if (nrd_.record(ctx, fs, in, which, whichN)) {
                        // Only when the occlusion denoiser actually ran THIS frame: otherwise the
                        // pool texture holds a stale result, and binding it would break the
                        // "bound as nothing means not denoised this frame" contract below.
                        nrdOutput_   = aoSignal ? nrd_.outputDiffuseHitDistance() : 0;
                        nrdGiOutput_ = nrd_.outputDiffuseRadianceHitDistance();
                        // MILESTONE 4: true only when the diffuse denoiser that just ran is the one
                        // recordStagedRayDriven (later THIS SAME frame, from scenePass) can trust for
                        // half-rate GI -- giSignal, not nrdGiOutput_'s own truthiness. Unlike nrdOutput_
                        // immediately above, nrdGiOutput_ is assigned here whether or not giSignal was
                        // true this call (`which`/`whichN` may have named only the occlusion denoiser),
                        // so on such a frame the texture nrdGiOutput_ names is whatever REBLUR's own
                        // permanent pool last held for a diffuse denoiser that did not run this call --
                        // not a live readback, which is exactly what nrdGiRanThisFrame_ exists to tell
                        // recordStagedRayDriven apart from the live case.
                        nrdGiRanThisFrame_ = giSignal;
                    }
                }
            }
        }
    }
    // BOUND EVERY FRAME, INCLUDING AS NOTHING. clearSrv is what makes the shader's GetDimensions
    // test mean "not denoised THIS frame" rather than "never denoised" -- leaving last frame's
    // texture bound after the pass stops running would feed the shader a frozen image with no
    // indication anything had changed.
    // Updated on EVERY frame, including ones where the whole NRD block above was gated off, so the
    // rejoin test inside it always compares against the frame immediately before.
    nrdAoRanLastFrame_ = nrdOutput_ != 0;
    if (nrdOutput_) res_->setSrv(bindings_, 14, nrdOutput_);
    else            res_->clearSrv(bindings_, 14);
    if (nrdGiOutput_) res_->setSrv(bindings_, 15, nrdGiOutput_);
    else              res_->clearSrv(bindings_, 15);

    // Read fresh every frame rather than cached: unlike the texture resolution, the scene viewport
    // can change (an editor panel resize) without a full onRenderTargetsChanged notification. A
    // transient failure here only disables BLENDING for this one frame -- writing the fresh sample
    // out still needs nothing from it.
    const bool haveViewport = dev_ && dev_->sceneViewport(curSceneViewport_);

    std::memcpy(cb_.prevViewProj, prevViewProj_, sizeof(prevViewProj_));
    std::memcpy(cb_.sceneViewport, prevSceneViewport_, sizeof(prevSceneViewport_));
    // THIS frame's rect, for the readers that project with gViewProj rather than gPrevViewProj.
    // Zeroed when the device could not report one, which is what the shader's own w > 0 test reads.
    if (haveViewport) std::memcpy(cb_.sceneViewportCur, curSceneViewport_, sizeof(curSceneViewport_));
    else              std::memset(cb_.sceneViewportCur, 0, sizeof(cb_.sceneViewportCur));

    // ---- IS THE EYE INSIDE A TRANSLUCENT VOLUME? ----
    // Answered here, once per frame: it's a property of the CAMERA, and a pixel shader knows its own
    // surface, not whether that volume encloses the eye.
    // THE TEST IS THE DRAW'S WORLD BOUNDING SPHERE, LOOSE, AND SAFE ONLY BECAUSE OF HOW THE SHADER
    // USES IT: a sphere around a wide shallow pool bulges above the water, so standing on the deck can
    // test "inside". The shader INVERTS its back-face discard rather than switching it off, so a false
    // positive changes WHICH single face draws, never HOW MANY -- switching the discard off instead
    // would composite two coats of water. That's what lets this ship on a bounding sphere, and why the
    // shader must not be "simplified" into an early-out later.
    // Blended AND single-sided only, exactly the draws the discard applies to. M_Glass is twosided=1
    // and never a medium by this test -- correct, a pane is not something you are inside of.
    cb_.cameraMedium[0] = cb_.cameraMedium[1] = 0.0f;
    cb_.causticMin[3] = 0.0f;
    {
        f32 vp[16], eye[3] = {};
        // No inverse needed: this block only tests eye against each volume's absolute AABB.
        if (dev_ && dev_->camera(vp, nullptr, eye)) {
            // drawsPrev_, NOT draws_ -- a probe caught this read wrong once. beginScene() swaps this
            // frame's list into drawsPrev_ and clears draws_ before any pass runs, so draws_ is EMPTY
            // here; same list buildAccelerationStructures reads, for the same reason.
            for (const Draw& d : drawsPrev_) {
                if (!d.translucent || d.boundsRadius < 0.0f) continue;   // negative radius = no bounds
                const pbr::MaterialConstants* mc =
                    reinterpret_cast<const pbr::MaterialConstants*>(d.mat);
                if (d.matBytes < sizeof(pbr::MaterialConstants)) continue;
                if (mc->flags & pbr::MaterialFlag_TwoSided) continue;

                // THE BOX, NOT THE SPHERE, now that the RHI keeps the extents createMesh always
                // measured -- the sphere used here was too loose to find where the surface actually
                // is, which caustics need. Eight corners through the world matrix, then min/max:
                // exact for the axis-aligned case every volume in practice is, conservative for a
                // rotated one.
                f32 lmin[3], lmax[3];
                if (!dev_->meshBoundsAabb(d.mesh, lmin, lmax)) continue;
                f32 wmin[3] = {1e30f, 1e30f, 1e30f}, wmax[3] = {-1e30f, -1e30f, -1e30f};
                for (int c = 0; c < 8; ++c) {
                    const f32 lx = (c & 1) ? lmax[0] : lmin[0];
                    const f32 ly = (c & 2) ? lmax[1] : lmin[1];
                    const f32 lz = (c & 4) ? lmax[2] : lmin[2];
                    for (int a = 0; a < 3; ++a) {
                        const f32 v = d.world[a] * lx + d.world[4 + a] * ly + d.world[8 + a] * lz + d.world[12 + a];
                        wmin[a] = std::fmin(wmin[a], v);
                        wmax[a] = std::fmax(wmax[a], v);
                    }
                }

                // THE CAUSTIC CASTER IS PUBLISHED WHETHER OR NOT THE EYE IS IN IT -- you see caustics
                // on a pool floor from the deck, which is the common case and the one the eye test
                // would have excluded.
                if (cb_.causticMin[3] == 0.0f) {
                    for (int a = 0; a < 3; ++a) { cb_.causticMin[a] = wmin[a]; cb_.causticMax[a] = wmax[a]; }
                    cb_.causticMin[3] = 1.0f;
                    cb_.causticMax[3] = settings_.causticStrength;
                }

                if (eye[0] < wmin[0] || eye[0] > wmax[0] ||
                    eye[1] < wmin[1] || eye[1] > wmax[1] ||
                    eye[2] < wmin[2] || eye[2] > wmax[2]) continue;
                cb_.cameraMedium[0] = 1.0f;
                cb_.cameraMedium[1] = mc->ior > 1.0f ? mc->ior : 1.0f;
                break;   // one medium is enough; nesting two is not a case this models
            }
        }
    }
    // A MOVED SUN INVALIDATES THE HISTORY, because nothing else does. rtReprojectHistory's validity
    // test is purely geometric, so a still camera under a moving light reprojects perfectly and the
    // blend keeps feeding back visibility traced against the sun's OLD direction -- the shadow ray
    // is right every frame and the image is ~10 frames behind it. Dropping the history for the one
    // frame the sun changes costs a single noisy frame and lets the next accumulate from scratch,
    // which is exactly what a disocclusion already does.
    const bool sunMoved = rtHistSunMoved();
    cb_.rtHistParams[0] = 1.0f;                                        // t6/u2 are bound to real textures
    // THREE STATES, NOT TWO: 1 = history usable; 0.5 = usable, but the sun changed this frame; 0 = not
    // usable. A sun change voids only the histories whose CONTENT depends on the sun -- shadow and
    // reflection (they compare against > 0.75) -- not sky occlusion, which measures geometry against
    // the sky and is identical under any sun (it compares against > 0.25). Voiding that too, as the
    // old 0/1 flag did, restarted a one-ray AO estimate from scratch on every frame of a slider drag,
    // which is one of the things that read as white speckle while the owner moved the light.
    cb_.rtHistParams[1] = (rtHistValid_ && haveViewport) ? (sunMoved ? 0.5f : 1.0f) : 0.0f; // ...and t6 + gSceneViewport are usable
    cb_.rtHistParams[2] = static_cast<f32>(rtFrameIndex_);
    // The TILE EDGE (rtPixelsPerRayTile_) as its bit count, not the edge itself: the shader masks
    // and shifts by this rather than multiplying or taking a modulo. rtPixelsPerRayTile_ is always
    // an exact power of two -- setPixelsPerRayTile only ever rounds to one -- so this always lands
    // on an exact integer.
    u32 tileBits = 0;
    for (u32 v = rtPixelsPerRayTile_; v > 1; v >>= 1) ++tileBits;
    cb_.rtHistParams[3] = static_cast<f32>(tileBits);
    // x = the spatial filter's radius in pixels. y = HOW MUCH OF THE FILTERED VALUE TO TAKE, pinned
    // at 0 for now: the taps run and the result is discarded (lerp(v, f, 0) == v for finite f), so
    // the image is bit-identical while the work is real -- the radius reaches the shader through a
    // CONSTANT, not a #define, so the loop isn't optimised away at zero. Becomes 1 in the same change
    // that earns it, against the penumbra probe.
    // PATH TRACING IS GATED HERE, not trusted from the setting: with path tracing Off this writes 1
    // (one hit, direct lighting) whatever the stored bounce count says, so no combination of
    // manifest, CLI override and quality tier can produce a running path tracer while the setting
    // reads off.
    cb_.ptBounceParams[0] = static_cast<f32>(pathTracingWanted() ? ptBounces_ : 1u);
    cb_.rtDenoiseParams[0] = static_cast<f32>(rtShadowDenoise_);
    // MEASURED BEFORE IT WAS TRUSTED, which is why this is 1 now and was 0 for one commit. With the
    // taps running and the result discarded, 49 of them (radius 3) cost +0.02 ms on ElectricDreams at
    // 1600x900, against a control where ONE extra ray per pixel cost +1.69 ms in the same batch --
    // ~85x cheaper than the ray it replaces, so the radius is a quality knob, not a performance one.
    // (An earlier attempt used --render-scale to force a GPU bound and proved nothing: that flag
    // clamps to 1.00, so the 'measured' run rendered at the baseline's own resolution.)
    cb_.rtDenoiseParams[1] = rtShadowDenoise_ > 0 ? 1.0f : 0.0f;
    // 0 = no motion taper. Left off by default: temporal accumulation (voxi.hlsl's tileBits==0
    // branch) already removes the variance the taper was compensating for, and the two together
    // just cost denoising quality while the camera moves. Kept as a knob because it is the only
    // lever that isolates the spatial filter's motion contribution without a rebuild.
    cb_.rtDenoiseParams[2] = rtDenoiseMotionTaper_;
}

// Has the sun moved since the frame the history was last written under? Compares the three fields
// the shadow and sky-occlusion rays actually depend on -- direction, colour, intensity -- and
// deliberately nothing else; see the members' comment for why the whole sky struct is the wrong
// question. Exact float compare on purpose: the sun either was edited or it was not, and a
// tolerance here would be a second, invisible threshold to tune.
bool VoxiRenderer::rtHistSunMoved() const {
    if (!dev_) return false;
    const rhi::SkyAtmosphere sky = dev_->skyAtmosphere();
    for (int i = 0; i < 3; ++i) {
        if (sky.sunDirection[i] != rtHistSunDir_[i]) return true;
        if (sky.sunColor[i]     != rtHistSunColor_[i]) return true;
    }
    return sky.sunIntensity != rtHistSunIntensity_;
}

bool VoxiRenderer::rtHistSunJumped() const {
    // ONE FRAME'S WORTH OF SUN CHANGE, not any change. A slider drag moves the sun a few degrees per
    // frame; a typed value, a level load or --sun-set-at moves it tens of degrees at once. 10 degrees
    // separates the two with room to spare at editor frame rates, and 20% does the same for colour
    // temperature and intensity drags. The first frame (intensity sentinel -1) always counts.
    constexpr f32 kGiSunJumpDeg = 10.0f;
    constexpr f32 kGiSunJumpRel = 0.2f;
    if (!dev_) return false;
    if (rtHistSunIntensity_ < 0.0f) return true;
    const rhi::SkyAtmosphere sky = dev_->skyAtmosphere();
    f32 dotDir = 0.0f, lenA = 0.0f, lenB = 0.0f;
    for (int i = 0; i < 3; ++i) {
        dotDir += sky.sunDirection[i] * rtHistSunDir_[i];
        lenA   += sky.sunDirection[i] * sky.sunDirection[i];
        lenB   += rtHistSunDir_[i] * rtHistSunDir_[i];
    }
    const f32 lens = std::sqrt(lenA * lenB);
    if (lens < 1e-12f) return true;
    if (dotDir / lens < std::cos(kGiSunJumpDeg * 3.14159265f / 180.0f)) return true;
    auto relChange = [](f32 now, f32 was) {
        const f32 base = std::max(std::abs(was), 1e-4f);
        return std::abs(now - was) / base;
    };
    if (relChange(sky.sunIntensity, rtHistSunIntensity_) > kGiSunJumpRel) return true;
    for (int i = 0; i < 3; ++i)
        if (relChange(sky.sunColor[i], rtHistSunColor_[i]) > kGiSunJumpRel) return true;
    return false;
}

void VoxiRenderer::endShadowHistory() {
    if (!shadowHistoryActive()) return;
    // Remember the sun this frame's history was accumulated under, so the next frame can tell
    // whether it moved. Written here rather than in beginShadowHistory so it always describes what
    // the history textures actually hold.
    if (dev_) {
        const rhi::SkyAtmosphere sky = dev_->skyAtmosphere();
        for (int i = 0; i < 3; ++i) { rtHistSunDir_[i] = sky.sunDirection[i]; rtHistSunColor_[i] = sky.sunColor[i]; }
        rtHistSunIntensity_ = sky.sunIntensity;
    }
    std::memcpy(prevViewProj_, curViewProj_, sizeof(curViewProj_));
    std::memcpy(prevSceneViewport_, curSceneViewport_, sizeof(curSceneViewport_));
    rtHistWriteIdx_ = 1 - rtHistWriteIdx_;
    rtHistValid_ = true;
    rtHistPrimed_ = true;   // the write side just bound above now sits in UnorderedAccess as the read side
    // LOCAL LIGHTS (LAMPS): resource state only, same reasoning as rtHistPrimed_ -- the side bound at
    // u19 went to UnorderedAccess in beginShadowHistory whether or not a scene pass then wrote it.
    // Content trust is rdLocalHistFrame_/rdLocalHistHash_, set where that pass is recorded (the raster
    // draws: at the end of prePass, just after this call).
    if (rdLocalOutThisFrame_) rdLocalHistPrimed_ = true;
    // giHistValid_ becomes true only once beginShadowHistory actually bound and wrote the
    // giSurfPosHist_/giSurfNrmHist_ pair THIS frame (cb_.giRestirParams.x, mirrored here rather
    // than re-derived from giRestirWanted()
    // so a frame where the textures failed to bind for some other reason does not falsely claim a
    // written previous frame next time).
    if (cb_.giRestirParams[0] > 0.5f) { giHistValid_ = true; giHistPrimed_ = true; }
    // U1/2.11: the sixth pair's own validity, mirrored from the SAME word beginShadowHistory's giSurf
    // block already recomputed once it knew the pair was actually bound (bit 4 of ambientParams.w --
    // see givis::packAmbientW's own bit table). The giRestirParams[0] test is the identical defensive
    // shape giHistValid_'s own line just above already takes -- bit 4 can only be set inside the very
    // same nested block that also sets giRestirParams[0] to 1.0, so the two tests agree by
    // construction, and pairing them costs nothing while keeping this line readable next to its twin.
    if (cb_.giRestirParams[0] > 0.5f && (static_cast<u32>(cb_.ambientParams[3]) & 4u)) {
        giVisHistValid_ = true;
        giVisHistPrimed_ = true;
    }
}

// Picks between a pipeline and its G-buffer twin -- see the header's own comment on the "Gbuf"
// members for why this asks dev_ directly instead of taking a parameter. `gbuf` is 0 whenever its
// twin never compiled (an optional variant, exactly like every RT/mesh-shader twin already in this
// file), and `dev_` is null only before init() -- both fall back to `plain`, which every existing
// caller of scenePipeline()/scenePass() already trusted before this method existed.
rhi::PipelineHandle VoxiRenderer::pickGbuf(rhi::PipelineHandle plain, rhi::PipelineHandle gbuf) const {
    if (!gbuf || !dev_ || !dev_->gBufferEnabled()) return plain;

    // MSAA MUST BE 1, AND THIS CHECK IS NOT BELT-AND-BRACES -- without it this is a real PSO/OM state
    // mismatch the D3D12 debug layer flags and a release build renders undefined.
    // The three G-buffer targets are created SINGLE-SAMPLE, deliberately, for a compute pass to read
    // (FidelityFX denoiser callbacks, later FSR2/3, TAA, SSR) -- none consume a Texture2DMS. But
    // OMSetRenderTargets requires every bound RTV and the DSV to share one SampleDesc, so when the
    // scene colour target is multisampled the backend declines to bind them and binds the pre-existing
    // single RTV instead (D3D12Device::beginFrame, warns once). This function is the OTHER half of
    // that decision: a G-buffer PSO declares four targets and bakes its own sample count, so returning
    // it while the backend bound ONE target is exactly the mismatch D3D12 forbids.
    // Falling back to `plain` is the right failure: a correct image and an empty G-buffer, not a
    // corrupt one.
    if (dev_->sampleCount() > 1) return plain;

    return gbuf;
}

// STAGED RAY-DRIVEN PASSES (milestone 1, extended by milestone 2's rdGiCsPso_/rdSkyOccCsPso_ checks
// and milestone 3's rdReflCsPso_ check): whether THIS frame's ray-driven primary runs as the staged
// passes rather than the single PSRayDriven draw -- see the header's own comment on every condition
// checked here and on `reason`.
bool VoxiRenderer::rdStagedActive(const char** reason) const {
    if (!rdStagedWanted()) return false;
    if (!rayDrivenActive()) return false;   // nothing to stage: the rasteriser or the debug raymarch
                                             // has the scene this frame, not a reason to fall back.
    // RECORDING COMPUTE INSIDE THE SCENE PASS IS VULKAN-ILLEGAL: this backend's scenePass() assumes
    // whatever the single-pass ray-driven draw already assumes about command-list state, and only
    // D3D12RenderContext::dispatch/setPipeline have been read to confirm a Dispatch between draws with
    // render targets bound is legal and leaves the graphics root bindings Stage B needs intact
    // (verified read-only, D3D12Device.cpp -- see the .cpp's own comment above recordStagedRayDriven).
    if (!(dev_ && dev_->backend() == rhi::Backend::D3D12)) {
        if (reason) *reason = "the backend is not D3D12 (recording compute inside the scene pass is "
                              "Vulkan-illegal)";
        return false;
    }
    if (!rdVisCsPso_ || !rdShadowCsPso_) {
        if (reason) *reason = "the staged compute pipelines (CSRdVisibility/CSRdShadow) did not compile";
        return false;
    }
    if (!rdVisBuf_ || !rdSunVisTex_ || !rdGiTex_ || !rdAoTex_ || !rdReflTex_) {
        if (reason) *reason = "the visibility record buffer or one of the sun visibility/GI/sky "
                              "occlusion/reflection textures is absent";
        return false;
    }
    // beginShadowHistory() refreshes curSceneViewport_ (the dispatch size, and gSceneViewportCur the
    // compute stages map pixels with) only past its own shadowHistoryActive() early-out, so on any
    // other frame both would be last frame's. A zero rect means no view to cover: the dispatches
    // would be skipped while Stage B still read the records, so fall back instead.
    if (!shadowHistoryActive() || !(curSceneViewport_[2] > 0.0f && curSceneViewport_[3] > 0.0f)) {
        if (reason) *reason = "the ray-traced history or this frame's scene viewport is not available";
        return false;
    }
    // WHICHEVER TEXTURED PIPELINE SCENEPASS() WOULD ACTUALLY BIND, matching pickGbuf()'s own choice --
    // staged mode has nothing to offer a frame that would fall back to the UNTEXTURED ray-driven
    // pipeline, since CSRdVisibility/CSRdShadow are only compiled against the textured (bindless)
    // layout (giLayout(kRtTextureCapacity), see createScenePipelines).
    const bool gbufBound = pickGbuf(rayDrivenPso_, rayDrivenGbufPso_) == rayDrivenGbufPso_;
    const rhi::PipelineHandle texPso   = gbufBound ? rayDrivenTexGbufPso_      : rayDrivenTexPso_;
    const rhi::PipelineHandle splitPso = gbufBound ? rayDrivenSplitTexGbufPso_ : rayDrivenSplitTexPso_;
    if (!texPso) {
        if (reason) *reason = "the untextured ray-driven pipeline is in use, not the textured one Stage "
                              "B needs";
        return false;
    }
    if (!splitPso) {
        if (reason) *reason = "Stage B's AVER_RD_SPLIT pipeline variant did not compile";
        return false;
    }
    // MILESTONE 2's OWN, CONDITIONAL requirement -- checked only when THIS frame's cb_ values say the
    // matching dispatch would actually fire in recordStagedRayDriven() (the SAME two conditions,
    // mirroring the SAME shader-side gates PSRayDriven's AVER_RD_SPLIT branch tests before trusting
    // gRdGiTex/gRdAoTex). A project that never turns ReSTIR GI or sky occlusion on is never blocked
    // by CSRdGi/CSRdSkyOcc failing to compile; one that does and finds the matching pipeline missing
    // falls back for the whole frame, because Stage B would otherwise read a texture nothing wrote
    // this frame while believing, from the identical cb_ fields, that it had.
    if (cb_.voxelParams[3] > 0.5f && cb_.giRestirParams[0] > 0.5f && !rdGiCsPso_) {
        if (reason) *reason = "ReSTIR GI is active this frame but CSRdGi did not compile";
        return false;
    }
    if (cb_.ambientParams[0] > 0.5f && (cb_.giRestirParams[0] > 0.5f || cb_.voxelParams[3] <= 0.5f) &&
        !rdSkyOccCsPso_) {
        if (reason) *reason = "sky occlusion is active this frame but CSRdSkyOcc did not compile";
        return false;
    }
    // MILESTONE 3's OWN, CONDITIONAL requirement -- the identical shape as milestone 2's immediately
    // above: checked only when THIS frame's cb_ values say CSRdRefl's dispatch would actually fire in
    // recordStagedRayDriven() (the SAME coarse condition PSRayDriven's own AVER_RD_SPLIT reflection
    // branch tests before trusting gRdReflTex's alpha; the per-pixel roughness half of that gate lives
    // entirely inside CSRdRefl and has no CPU-side mirror). A project that never turns ray-traced
    // reflections on is never blocked by CSRdRefl failing to compile; one that does and finds the
    // pipeline missing falls back for the whole frame, for the identical reason milestone 2's own
    // paragraph gives.
    if (cb_.shadowParams[2] > 0.5f && cb_.rtParams[3] > 0.5f && !rdReflCsPso_) {
        if (reason) *reason = "ray-traced reflections are active this frame but CSRdRefl did not compile";
        return false;
    }
    return true;
}

// Returns the lit pipeline for this frame, or 0 to decline and let the backend use its own.
rhi::PipelineHandle VoxiRenderer::scenePipeline(bool meshShaders, bool wireframe, bool depthPrepassed,
                                                bool blended) const {
    if (wireframe) return 0;
    // BLENDED IS ANSWERED FIRST AND COMPLETELY, before depthPrepassed is even inspected. IDevice::
    // drawMesh never sets both at once (a blended draw writes no depth, so there's nothing for a
    // "trust the prepass" twin to trust), and glass wanting to draw outranks a depth prepass that
    // could not have targeted it anyway -- falling into depthPrepassed's WRONG (opaque,
    // depth-write-on) pipeline family would be worse than ignoring it here.
    // Falls back like the opaque family does: mesh-shader variant of the active RT state, or plain if
    // the mesh-shader compile never produced a blended twin. Unlike the opaque family, rtActive_ true
    // does NOT guarantee sceneRtBlendedPso_/sceneMsRtBlendedPso_ are non-zero -- a blended PSO is a
    // second, independent pipeline creation that can fail even though the same shader binary compiled
    // fine for the opaque twin. That failure returns 0 here and the caller drops the draw -- see
    // createScenePipelines' AVER_WARN at the blended-twins step.
    if (blended) {
        // THE TEXTURED VARIANT FIRST, when there is one. Not for the mesh-shader or G-buffer
        // permutations: neither has a textured twin, and pickGbuf must keep returning a pair that
        // agree about their root signature.
        if (rtActive_ && sceneRtBlendedTexPso_ && !meshShaders && !dev_->gBufferEnabled())
            return sceneRtBlendedTexPso_;
        if (rtActive_)
            return pickGbuf(meshShaders && sceneMsRtBlendedPso_ ? sceneMsRtBlendedPso_ : sceneRtBlendedPso_,
                            meshShaders && sceneMsRtBlendedGbufPso_ ? sceneMsRtBlendedGbufPso_
                                                                     : sceneRtBlendedGbufPso_);
        return pickGbuf(meshShaders && sceneMsBlendedPso_ ? sceneMsBlendedPso_ : sceneBlendedPso_,
                        meshShaders && sceneMsBlendedGbufPso_ ? sceneMsBlendedGbufPso_ : sceneBlendedGbufPso_);
    }
    // The LessEqual/no-write twin, for an instance a same-frame depthPrepassPipeline() draw already
    // wrote depth for -- see depthPrepassPipeline() and D3D12Device::drawMesh for who sets this.
    // MESH-SHADER SCENE DRAWS NEVER TAKE THIS BRANCH: the prepass is only offered to the plain
    // drawMesh() path, so `depthPrepassed && meshShaders` must never both be true -- and it is now
    // the BACKENDS that guarantee it (D3D12Device/VulkanDevice::drawMesh pass meshShaders=false for
    // a prepassed draw and issue it through the input assembler).
    //
    // THIS USED TO SAY "should never both be true", AND THAT FALLING THROUGH TO THE ORDINARY
    // MESH-SHADER PIPELINE "IF IT SOMEHOW HAPPENS" WAS THE SAME HARMLESS "just draws normally"
    // ANSWER GIVEN TO SKINNED MESHES. Both halves were wrong. It happened on every prepassed draw
    // whenever the device ran mesh shaders (RENDER.MESHSHADERS 1, PTTest's own setting), because the
    // backend passed msActive_ through unconditionally. And the fall-through is not harmless: the
    // ordinary pipeline is Less with depth write, run against the depth the prepass wrote for the
    // SAME triangle, and Less rejects equal -- so every eligible fragment was discarded, the frame
    // came out almost entirely unshaded (0.1% of pixels bit-identical to the prepass-off frame),
    // and --depth-prepass had to ship off. A skinned mesh differs precisely because the prepass
    // never wrote depth for it, so there is nothing equal to reject.
    if (depthPrepassed && !meshShaders) {
        if (rtActive_ && sceneRtPsoPrepassed_) return pickGbuf(sceneRtPsoPrepassed_, sceneRtPsoPrepassedGbuf_);
        if (!rtActive_ && scenePsoPrepassed_)  return pickGbuf(scenePsoPrepassed_, scenePsoPrepassedGbuf_);
    }
    if (rtActive_)
        return pickGbuf(meshShaders && sceneMsRtPso_ ? sceneMsRtPso_ : sceneRtPso_,
                        meshShaders && sceneMsRtGbufPso_ ? sceneMsRtGbufPso_ : sceneRtGbufPso_);
    return pickGbuf(meshShaders && sceneMsPso_ ? sceneMsPso_ : scenePso_,
                    meshShaders && sceneMsGbufPso_ ? sceneMsGbufPso_ : sceneGbufPso_);
}

// The depth-only prepass pipeline -- see the header's own comment. 0 (declines) whenever
// createScenePipelines() never got a compiled PSDepthPrepass, which IDevice::drawMeshDepthPrepass
// treats identically to "this feature has no prepass at all".
// OFFERED ONLY WHEN THIS FRAME'S COLOUR PASS CAN CONSUME IT. A prepass with no LessEqual/no-write
// twin behind it is not a missed optimisation, it is the original failure: scenePipeline falls back
// to the Less/write pipeline, which rejects the equal depth this pass just wrote, and the frame goes
// almost entirely unshaded. The twins are created only when depthPrepassPso_ exists, and each has
// its own WARN when it does not, so this can only bite on a partial pipeline failure -- but that is
// exactly when it should degrade to "no prepass" rather than to a black frame. Checks the PLAIN twin
// because pickGbuf falls back to it when the G-buffer twin is absent. rtActive_ is fixed for the
// frame by prePass, which runs from beginFrame before any draw. Found by adversarial review.
rhi::PipelineHandle VoxiRenderer::depthPrepassPipeline() const {
    const rhi::PipelineHandle twin = rtActive_ ? sceneRtPsoPrepassed_ : scenePsoPrepassed_;
    return twin ? depthPrepassPso_ : 0;
}

// Creates the cascaded shadow atlas.
bool VoxiRenderer::createShadowResources() {
    rhi::TextureDesc d;
    d.dim    = rhi::TextureDim::Tex2D;
    d.width  = kShadowSize;
    d.height = kShadowSize;
    // Typeless so the DSV can see D32Float while the SRV sees R32Float: one resource, two views.
    d.format = rhi::Format::R32Typeless;
    d.bind   = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::DepthStencil;
    d.initialState  = rhi::ResourceState::ShaderResource;
    d.hasClearValue = true;
    d.clearDepth    = 1.0f;
    d.debugName     = "Voxi shadow map";
    shadowTex_ = res_->createTexture(d);
    if (!shadowTex_) AVER_ERROR("[Voxi] shadow map {}^2 could not be created", kShadowSize);

    // The GI-only map: same typeless DSV/SRV trick, same clear value, one box instead of a 2x2
    // atlas -- so it needs a fraction of the texels. See kGiShadowSize.
    d.width  = kGiShadowSize;
    d.height = kGiShadowSize;
    d.debugName = "Voxi GI shadow map";
    giShadowTex_ = res_->createTexture(d);
    if (!giShadowTex_)
        AVER_WARN("[Voxi] GI-only shadow map {}^2 could not be created; indirect light will be "
                  "injected unshadowed", kGiShadowSize);

    // A WARNING, NOT A FAILURE, and only shadowTex_ gates the return. The GI map affects bounce
    // light alone; losing it should degrade the picture, not refuse to start the renderer.
    return shadowTex_ != 0;
}

// Creates the radiance volume, the injection accumulator and every binding set over them.
// W12: just voxelAccumTex_, factored out of createVoxelVolume so manageInjectionAccumulator() can
// recreate it after a free with the identical desc, debugName and error text -- one definition of
// what this texture is, not two that could drift.
bool VoxiRenderer::createInjectionAccumulator(u32 resolution) {
    // Four uints per voxel (r, g, b, fragment count) interleaved along x: R32_UINT is the only typed
    // format D3D12 guarantees UAV atomics on.
    rhi::TextureDesc ad;
    ad.dim    = rhi::TextureDim::Tex3D;
    ad.width  = resolution * 4;
    ad.height = resolution;
    ad.depth  = resolution;
    ad.mips   = 1;
    ad.format = rhi::Format::R32Uint;
    ad.bind   = rhi::ResourceBind::UnorderedAccess;
    ad.initialState = rhi::ResourceState::UnorderedAccess;   // where it stays: nothing reads it as an SRV
    ad.debugName    = "Voxi injection accumulator";
    voxelAccumTex_ = res_->createTexture(ad);
    if (!voxelAccumTex_) { AVER_ERROR("[Voxi] injection accumulator {}^3 could not be created", resolution); return false; }
    return true;
}

bool VoxiRenderer::createVoxelVolume(u32 resolution) {
    rhi::TextureDesc d;
    d.dim    = rhi::TextureDim::Tex3D;
    d.width  = resolution;
    d.height = resolution;
    d.depth  = resolution;
    d.mips   = 0;                       // full chain: mip N is the cone footprint at distance N
    d.format = rhi::Format::RGBA16F;
    d.bind   = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::UnorderedAccess;
    d.initialState = rhi::ResourceState::ShaderResource;   // where the chain rests between frames
    d.debugName    = "Voxi radiance volume";
    voxelTex_ = res_->createTexture(d);
    if (!voxelTex_) { AVER_ERROR("[Voxi] radiance volume {}^3 could not be created", resolution); return false; }

    // W12: factored into createInjectionAccumulator() so manageInjectionAccumulator() can recreate
    // voxelAccumTex_ after a free without duplicating this desc/debugName/error text a second time.
    if (!createInjectionAccumulator(resolution)) return false;

    // The resolved mip count, never a recomputed log2.
    rhi::TextureDesc got{};
    if (!res_->textureInfo(voxelTex_, got)) { AVER_ERROR("[Voxi] textureInfo failed for the radiance volume"); return false; }
    voxelMips_     = got.mips;
    voxelResBuilt_ = resolution;
    // By-hand verification aid: the settings page and the manifest both name a resolution before
    // this runs, and this is the one line that says what actually got built. Matched against
    // RENDER.VOXELRES by the tier-verification protocol.
    AVER_INFO("[Voxi] GI voxel volume {}^3", resolution);
    // M6: volume memory, computed from the texture descriptions above rather than queried from the
    // backend -- neither RHI exposes a "how many bytes did this resource actually cost" accessor, and
    // the formats/mip counts here are exactly what was just asked for, so this is exact, not an
    // estimate. Radiance: RGBA16F is 8 B/texel, summed over every mip's OWN voxel count -- mipDim()
    // floors like the backend's own mip-chain sizing does, so this is NOT a naive geometric sum.
    // Accumulator: R32_UINT at (resolution*4) x resolution x resolution is 4 B/texel, i.e. 16 B per
    // VOXEL (its four channels interleaved along x).
    {
        u64 radianceVoxels = 0;
        for (u32 m = 0; m < voxelMips_; ++m) {
            const u64 dm = mipDim(resolution, m);
            radianceVoxels += dm * dm * dm;
        }
        const f64 radianceMiB = static_cast<f64>(radianceVoxels * 8ull) / (1024.0 * 1024.0);
        const f64 accumMiB = static_cast<f64>(static_cast<u64>(resolution) * resolution * resolution *
                                              16ull) / (1024.0 * 1024.0);
        AVER_INFO("[Voxi] GI volume memory (computed from the texture descriptions, not queried): "
                  "radiance {:.0f} MiB, injection accumulator {:.0f} MiB", radianceMiB, accumMiB);
    }

    // Main table. Each slot declares its kind because Tier 1 hardware null-fills by dimension.
    rhi::BindingSetDesc bd;
    // kVoxiSrvCount/kGiUavCount, matching giLayout()'s l.srvCount/l.uavCount exactly (see
    // kVoxiSrvCount's own comment above giTableKinds for why this set is wider than the constant a
    // foreign caller's layout reserves against) -- reading the identical constants here keeps this
    // set and the pipelines that bind it from independently drifting.
    bd.srvCount = kVoxiSrvCount;
    bd.uavCount = kVoxiUavCount;
    // The SAME kinds giLayout() declares -- one function, so the set and the pipelines that bind it
    // cannot drift apart. They were two hand-kept copies until giTableKinds() existed.
    giTableKinds(bd.srvKinds, bd.uavKinds);
    bindings_ = res_->createBindingSet(bd);
    if (!bindings_) { AVER_ERROR("[Voxi] main binding set could not be created"); return false; }
    res_->setSrv(bindings_, 0, voxelTex_, rhi::kAllMips);
    if (shadowTex_) res_->setSrv(bindings_, 1, shadowTex_);
    if (giShadowTex_) res_->setSrv(bindings_, 8, giShadowTex_);   // t8, the GI-only shadow map
    res_->setUav(bindings_, 0, voxelTex_, 0);
    res_->setUav(bindings_, 1, voxelAccumTex_, 0);
    // OCCLUSION-AWARE FOG: t17/u16 ALWAYS get bound here, to a 1x1x1 placeholder -- never left
    // null-filled the way t6/t7/t11/u2-u5 below are, because createPipelines() (which decides
    // airVisPso_, and therefore airVisWanted()) has not run yet at this point in init(). ensureAirVis()
    // -- called once from init() right after createPipelines(), and again from setSettings() on
    // airVisWanted()'s own edge -- is what upgrades this bind to the real texture.
    {
        rhi::TextureDesc pd;
        pd.dim    = rhi::TextureDim::Tex3D;
        pd.width  = pd.height = pd.depth = 1;
        pd.mips   = 1;
        pd.format = rhi::Format::R16F;
        pd.bind   = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::UnorderedAccess;
        pd.initialState = rhi::ResourceState::ShaderResource;
        pd.debugName    = "Voxi air sky-visibility placeholder";
        airVisPlaceholder_ = res_->createTexture(pd);
        if (!airVisPlaceholder_) { AVER_ERROR("[Voxi] air sky-visibility placeholder could not be created"); return false; }
    }
    res_->setSrv(bindings_, 17, airVisPlaceholder_, rhi::kAllMips);
    res_->setUav(bindings_, 16, airVisPlaceholder_, 0);
    // LOCAL LIGHTS (LAMPS): t18, t19 and u19 get placeholders here for the same reason t17/u16 do --
    // buildLocalLights() and ensureShadowHistory() upgrade them to the real list/history pair, and
    // rebind these whenever those go away. Guarded so a second call here cannot orphan the first pair.
    if (!rdLocalLightsPlaceholder_) {
        rhi::BufferDesc pd;
        pd.bytes = sizeof(RdLocalLight);   // one element: the smallest view of this stride
        pd.kind  = rhi::BufferKind::Default;
        pd.debugName = "Voxi local-light list placeholder";
        rdLocalLightsPlaceholder_ = res_->createBuffer(pd);
        if (!rdLocalLightsPlaceholder_) { AVER_ERROR("[Voxi] local-light list placeholder could not be created"); return false; }
    }
    if (!rdLocalHistPlaceholder_) {
        rhi::TextureDesc pd;
        pd.dim    = rhi::TextureDim::Tex2D;
        pd.width  = 1; pd.height = 1; pd.mips = 1;
        pd.format = rhi::Format::RGBA16F;
        pd.bind   = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::UnorderedAccess;
        pd.initialState = rhi::ResourceState::ShaderResource;
        pd.debugName    = "Voxi local-light history placeholder";
        rdLocalHistPlaceholder_ = res_->createTexture(pd);
        if (!rdLocalHistPlaceholder_) { AVER_ERROR("[Voxi] local-light history placeholder could not be created"); return false; }
    }
    res_->setSrvBuffer(bindings_, 18, rdLocalLightsPlaceholder_, sizeof(RdLocalLight), 1, 0);
    rdLocalLightsBound_ = rdLocalLightsPlaceholder_;
    res_->setSrv(bindings_, 19, rdLocalHistPlaceholder_);
    res_->setUav(bindings_, 19, rdLocalHistPlaceholder_, 0);
    // t6/u2 (rtShadowHist_), t7/u3 (rtReflHist_) and t11/u4 (rtAoHist_) are populated once
    // onRenderTargetsChanged creates them -- the resolution is not known this early, and the
    // slots are declared above so Tier 1 null-fills them correctly until then. THREE pairs, not
    // two: the ambient one arrived with temporal sky occlusion and this list is the kind that
    // quietly goes one short.
    //
    // u11/u12 (rdVisBuf_/rdSunVisTex_, STAGED RAY-DRIVEN PASSES), u13/u14 (rdGiTex_/rdAoTex_,
    // MILESTONE 2) and u15 (rdReflTex_, MILESTONE 3) are populated the SAME way, by
    // ensureRdStagedResources once onRenderTargetsChanged calls it a few lines below -- except once
    // populated they are rebound to a PLACEHOLDER rather than left null-filled if staged mode is ever
    // turned back off, since a UAV slot cannot be cleared the way clearSrv clears an SRV one; see
    // ensureRdStagedResources' own comment.

    // The clear and the resolve get UAV-only sets: while they run every mip of the volume is in
    // UnorderedAccess, so no SRV descriptor over it may be live.
    rhi::BindingSetDesc cd;
    cd.uavCount = 2;
    cd.uavKinds[0] = rhi::SlotKind::Texture3D;
    cd.uavKinds[1] = rhi::SlotKind::Texture3D;
    clearBindings_ = res_->createBindingSet(cd);
    if (!clearBindings_) { AVER_ERROR("[Voxi] clear binding set could not be created"); return false; }
    res_->setUav(clearBindings_, 0, voxelTex_, 0);
    res_->setUav(clearBindings_, 1, voxelAccumTex_, 0);

    resolveBindings_ = res_->createBindingSet(cd);
    if (!resolveBindings_) { AVER_ERROR("[Voxi] resolve binding set could not be created"); return false; }
    res_->setUav(resolveBindings_, 0, voxelTex_, 0);
    res_->setUav(resolveBindings_, 1, voxelAccumTex_, 0);

    // One set per filter step. A single-mip source view is what makes reading level m-1 while
    // writing level m legal.
    mipBindings_.reserve(voxelMips_ ? voxelMips_ - 1 : 0);
    for (u32 m = 1; m < voxelMips_; ++m) {
        rhi::BindingSetDesc md;
        md.srvCount = 1;
        md.uavCount = 1;
        md.srvKinds[0] = rhi::SlotKind::Texture3D;
        md.uavKinds[0] = rhi::SlotKind::Texture3D;
        const rhi::BindingSetHandle s = res_->createBindingSet(md);
        if (!s) { AVER_ERROR("[Voxi] mip binding set {} could not be created", m); return false; }
        res_->setSrv(s, 0, voxelTex_, m - 1);
        res_->setUav(s, 0, voxelTex_, m);
        mipBindings_.push_back(s);
    }
    return true;
}

// OCCLUSION-AWARE FOG: creates or releases airVisTex_ against airVisWanted()'s own answer -- the
// identical "wanted() decides, ensure*() acts" shape ensureShadowHistory/ensureRdStagedResources
// already use, restated in miniature because this resource has neither a resize dependency (fixed
// kAirVisResolution^3) nor a ping-pong pair. Idempotent: returns true immediately once airVisTex_'s
// existence already matches what is wanted, so every call site can invoke this unconditionally on
// its own edge without a separate "did anything actually change" test of its own.
bool VoxiRenderer::ensureAirVis() {
    if (!res_ || !bindings_) return false;
    const bool wanted = airVisWanted();
    if (wanted == (airVisTex_ != 0)) return true;   // already in the state this settings/device pair wants

    if (!wanted) {
        // REBIND BEFORE DESTROY, always -- aver-view-outlives-its-buffer.md, the identical rule
        // manageInjectionAccumulator's own FREE branch and ensureRdStagedResources' own release
        // branch both state for themselves. airVisPlaceholder_ was created back in
        // createVoxelVolume(), before this function could ever run, so it already exists here.
        if (airVisPlaceholder_) {
            res_->setSrv(bindings_, 17, airVisPlaceholder_, rhi::kAllMips);
            res_->setUav(bindings_, 16, airVisPlaceholder_, 0);
        }
        res_->destroyTexture(airVisTex_);   // fence-deferred on both backends
        airVisTex_ = 0;
        airVisDirty_ = false;
        AVER_INFO("[Voxi] air sky-visibility volume released; voxi.fogOcclusion is off (placeholder "
                  "bound -- fog reads full sky visibility everywhere, exactly as before this feature "
                  "existed)");
        return true;
    }

    rhi::TextureDesc d;
    d.dim    = rhi::TextureDim::Tex3D;
    d.width  = d.height = d.depth = kAirVisResolution;
    d.mips   = 1;   // CSAirVis writes one level; the shade-pass read is a single trilinear sample, no
                     // clipmap footprint selection the way the GI radiance volume's chain needs.
    // R16F: the smallest format this RHI exposes that both backends can bind as a typed UAV store AND
    // an SRV (see D3D12Device.cpp/VulkanCommon.hpp's Format::R16F mapping) -- a [0,1] mean
    // transmittance needs no more precision than half-float gives it. Same reasoning rtAoHitDist_
    // gives for its own R16Unorm, one format family over.
    d.format = rhi::Format::R16F;
    d.bind   = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::UnorderedAccess;
    d.initialState = rhi::ResourceState::ShaderResource;   // rests here between frames, like voxelTex_
    d.debugName    = "Voxi air sky-visibility volume";
    airVisTex_ = res_->createTexture(d);
    if (!airVisTex_) {
        AVER_ERROR("[Voxi] air sky-visibility volume ({}^3) could not be created; fog stays "
                   "unoccluded (placeholder stays bound at t17/u16)", kAirVisResolution);
        return false;
    }
    res_->setSrv(bindings_, 17, airVisTex_, rhi::kAllMips);
    res_->setUav(bindings_, 16, airVisTex_, 0);
    // Nothing has written it yet -- a fresh texture holds whatever the device handed back, not a
    // neutral value. See airVisDirty_'s own comment (VoxiRenderer.hpp) for why this cannot simply
    // wait for the next GI rebuild.
    airVisDirty_ = true;
    AVER_INFO("[Voxi] air sky-visibility volume created ({}^3, {:.1f} KiB); CSAirVis fills it once, "
              "this frame or next, before any shade pass reads it", kAirVisResolution,
              static_cast<f64>(static_cast<u64>(kAirVisResolution) * kAirVisResolution *
                               kAirVisResolution * 2ull) / 1024.0);
    return true;
}

// OCCLUSION-AWARE FOG: binds airVisPso_ and dispatches CSAirVis over the whole volume. See the
// header's own comment for the contract each call site (filterMips()/prePass()) has to honour around
// voxelTex_'s state -- this function only ever touches airVisTex_'s own state and airVisDirty_.
void VoxiRenderer::dispatchAirVis(rhi::IRenderContext& ctx, u32 zLo, u32 zHi) {
    rhi::ScopedGpuStat stat(ctx, "Voxi air visibility");
    ctx.textureBarrier(airVisTex_, rhi::ResourceState::ShaderResource, rhi::ResourceState::UnorderedAccess);
    ctx.setPipeline(airVisPso_);
    ctx.setBindingSet(bindings_);
    // Table 1 is bound outright, exactly like every other full-layout Voxi pass (scenePass()'s debug
    // view, recordStagedRayDriven()'s stages): the layout declares kMaterialSrvCount SRVs there and
    // Tier 1 populates whole tables, even though CSAirVis's own body never reads it.
    ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
    ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
    // The cell box rides MipCB (b3), the block the voxel kernels use -- set before EVERY dispatch of
    // this PSO, since a declared-but-unset root constant block is this project's recorded TDR class.
    VoxelBox box;
    box.lo[0] = 0; box.lo[1] = 0; box.lo[2] = zLo;
    box.hi[0] = kAirVisResolution; box.hi[1] = kAirVisResolution; box.hi[2] = zHi;
    const GiDispatchConstants k = dispatchConstants(box, 0);
    ctx.setConstants(3, &k, kGiDispatchConstantDwords);
    constexpr u32 kGroupsXY = (kAirVisResolution + 7u) / 8u;   // CSAirVis: [numthreads(8,8,1)]
    if (zHi > zLo) ctx.dispatch(kGroupsXY, kGroupsXY, zHi - zLo);
    ctx.uavBarrierTexture(airVisTex_);
    ctx.textureBarrier(airVisTex_, rhi::ResourceState::UnorderedAccess, rhi::ResourceState::ShaderResource);
    if (zLo == 0 && zHi >= kAirVisResolution) airVisDirty_ = false;
}

// THE MODULAR SEAM (Stage 3, GPU per-cluster shading parity): a caller that has merged Voxi's
// table-0 union into a binding set IT owns (see VoxiGiShaders.hpp, and SandboxApp.cpp's
// ensureLodMeshPipeline for the one real consumer) asks Voxi to populate the slots it reserved
// rather than reach into voxelTex_/shadowTex_ itself -- Voxi stays the only code that knows those
// handles' kind or readiness.
// ONLY THE TWO SLOTS giShaderPrelude() DECLARES A SYMBOL FOR: the GI volume at srvBase, the shadow
// map at srvBase+1. The rest of the table-0 union (a caller's layout still reserves all of it, so its
// register numbers past this pair land where giLayout()'s own do) is Voxi's own ray-traced/
// GI-only-shadow state, which giShaderPrelude() never declares a register for -- nothing to bind.
// `res` is passed in, not read from res_, so this is callable from a caller with only an
// rhi::IResourceFactory&, not a VoxiRenderer's own device handle.
void VoxiRenderer::bindGiResources(rhi::IResourceFactory& res, rhi::BindingSetHandle set, u32 srvBase) const {
    // Guarded like bindings_'s own population above: init() may not have finished, or the resolution
    // may be mid-rebuild, and Tier 1's null-fill defines "reads as empty" for whichever isn't ready --
    // not this function's problem to report. The caller learns nothing shadowed/bounced this frame
    // the same way it would from Voxi's own pipeline: gShadowParams.y and gVoxelParams.w in the shader.
    if (voxelTex_)  res.setSrv(set, srvBase + 0, voxelTex_, rhi::kAllMips);
    if (shadowTex_) res.setSrv(set, srvBase + 1, shadowTex_);
}

// Creates every pipeline the feature runs. Mesh-shader and RayQuery variants are optional.
bool VoxiRenderer::createPipelines() {
    const bool msOk = caps_.meshShaderTier > 0 && caps_.shaderModel >= 65 && caps_.dxcAvailable;

    ShaderScope compile(*res_);

    const rhi::PipelineLayout gi = giLayout();
    // Raster pipelines declare the material table, so their shaders are told which registers it
    // landed at. The compute ones must not be told: they declare no second table.
    const std::string matDefs = pbr::materialShaderDefines(gi.srvCount, kMaterialSamplerSlot,
                                                          layeredBsdf_);
    auto rasterDefs = [&](const char* extra) { return extra ? matDefs + ";" + extra : matDefs; };

    // Which SHADING VARIANT this session's pipelines were compiled for, said once, at the only place
    // that decides it. The layered BSDF cannot be toggled after this point (see setSettings' latch),
    // so a run that renders no coat has exactly two possible causes -- the variant is Off, or no
    // material authored one -- and this line settles the first without a debugger.
    AVER_INFO("[Voxi] scene pipelines compiling with the {} shading model",
              layeredBsdf_ ? "LAYERED (base BRDF + coat lobe)" : "standard BRDF");

    // --- 1. shadow map: depth only, from the sun ---
    if (const rhi::ShaderHandle vs = compile("VSShadow", rhi::ShaderStage::Vertex, kBaseSm, rasterDefs(nullptr).c_str())) {
        rhi::GraphicsPipelineDesc p;
        p.vs = vs;                                   // no pixel shader: depth is the only output
        p.layout = gi;
        p.cull = rhi::CullMode::None;
        p.depth = {true, true, rhi::CompareOp::Less};
        p.renderTargetCount = 0;
        p.depthFormat = rhi::Format::D32Float;
        p.sampleCount = 1;                           // the shadow map is never multisampled
        p.slopeScaledDepthBias = 1.5f;
        shadowPso_ = res_->createGraphicsPipeline(p);
    }
    if (!shadowPso_) AVER_ERROR("[Voxi] shadow pipeline unavailable");

    // --- 1b. the same depth-only pass, but instanced: one DrawIndexedInstanced per mesh per cascade
    // instead of one drawMesh() per surviving draw per cascade. See VSShadowInstanced (VoxiShaders.hpp)
    // and IRenderContext::drawMeshInstanced (RHIResources.hpp). AVER_INSTANCE_SRV must be the SAME
    // number GraphicsPipelineDesc::instanced makes the backend reserve for this layout --
    // declaredSrvCount(gi), the register past every t-register `gi` declares (see the comment above
    // `instanced` in RHIResources.hpp for why this can't be shared any other way). Optional:
    // shadowPass() falls back to shadowPso_'s one-draw-per-instance path if this failed to build.
    const std::string instDefs = rasterDefs(("AVER_INSTANCE_SRV=" + std::to_string(rhi::declaredSrvCount(gi))).c_str());
    // SM 6.0 AND DXC, OR NO INSTANCED SHADOWS AT ALL -- a bug fix, not caution. Under the FXC/SM 5.1
    // fallback the VSShadowInstanced entry point still COMPILED, so shadowPass took the instanced
    // branch, but the per-instance transforms (fed through a StructuredBuffer indexed by
    // SV_InstanceID) never arrived: every caster rasterised with a garbage world matrix, the shadow
    // not where the geometry was. Failed silently, as a picture: the gate oracle caught it as `shadow`
    // reading 90,85,80 against a recorded 23,27,32 -- a shadowed floor pixel exactly as bright as the
    // sunlit one -- via the "shadow/sunlit must bracket the lighting" invariant. shadowPass already
    // falls back to shadowPso_ whenever this handle is 0, so refusing to build it here is the whole fix.
    const bool instancedShadowsOk = caps_.shaderModel >= 60 && caps_.dxcAvailable;
    if (!instancedShadowsOk)
        AVER_INFO("[Voxi] instanced shadows off (SM {}, DXC {}); using one draw per instance",
                  caps_.shaderModel, caps_.dxcAvailable ? "yes" : "no");
    if (instancedShadowsOk)
    if (const rhi::ShaderHandle vsInst = compile("VSShadowInstanced", rhi::ShaderStage::Vertex, kBaseSm, instDefs.c_str())) {
        rhi::GraphicsPipelineDesc p;
        p.vs = vsInst;
        p.layout = gi;
        p.instanced = true;
        p.cull = rhi::CullMode::None;
        p.depth = {true, true, rhi::CompareOp::Less};
        p.renderTargetCount = 0;
        p.depthFormat = rhi::Format::D32Float;
        p.sampleCount = 1;
        p.slopeScaledDepthBias = 1.5f;
        shadowInstancedPso_ = res_->createGraphicsPipeline(p);
    }
    if (!shadowInstancedPso_ && instancedShadowsOk)
        AVER_WARN("[Voxi] instanced shadow pipeline unavailable; shadowPass falls back to one draw per instance");

    // --- 1c. the GI-only depth pass, plain and instanced. Same pipeline state as the cascade pair
    // above; only the entry point differs, since the matrix it transforms into (gGiShadowViewProj vs
    // gCascadeViewProj[gShadowDraw.x]) is baked per pipeline rather than chosen per draw. Both
    // OPTIONAL: giShadowPass skips entirely without the plain one, falls back to one draw per
    // instance without the instanced one.
    if (const rhi::ShaderHandle vsGi = compile("VSGiShadow", rhi::ShaderStage::Vertex, kBaseSm,
                                               rasterDefs(nullptr).c_str())) {
        rhi::GraphicsPipelineDesc p;
        p.vs = vsGi;
        p.layout = gi;
        p.cull = rhi::CullMode::None;
        p.depth = {true, true, rhi::CompareOp::Less};
        p.renderTargetCount = 0;
        p.depthFormat = rhi::Format::D32Float;
        p.sampleCount = 1;
        p.slopeScaledDepthBias = 1.5f;
        giShadowPso_ = res_->createGraphicsPipeline(p);
    }
    if (instancedShadowsOk)
    if (const rhi::ShaderHandle vsGiInst = compile("VSGiShadowInstanced", rhi::ShaderStage::Vertex,
                                                   kBaseSm, instDefs.c_str())) {
        rhi::GraphicsPipelineDesc p;
        p.vs = vsGiInst;
        p.layout = gi;
        p.instanced = true;
        p.cull = rhi::CullMode::None;
        p.depth = {true, true, rhi::CompareOp::Less};
        p.renderTargetCount = 0;
        p.depthFormat = rhi::Format::D32Float;
        p.sampleCount = 1;
        p.slopeScaledDepthBias = 1.5f;
        giShadowInstancedPso_ = res_->createGraphicsPipeline(p);
    }
    if (!giShadowPso_)
        AVER_WARN("[Voxi] GI-only shadow pipeline unavailable; indirect light is injected unshadowed");

    // --- 2/3. voxelisation + light injection: rasterise with NO render target ---
    const rhi::ShaderHandle psVoxel = compile("PSVoxel", rhi::ShaderStage::Pixel, kBaseSm, rasterDefs(nullptr).c_str());
    rhi::GraphicsPipelineDesc vox;
    vox.layout = gi;
    vox.cull = rhi::CullMode::None;
    vox.depthClip = false;                           // a triangle outside the slab still voxelises
    // Widens rasterisation so thin geometry still covers a voxel.
    vox.conservativeRaster = caps_.conservativeRaster;
    vox.renderTargetCount = 0;                       // the pixel shader's only output is the UAV
    vox.depthFormat = rhi::Format::Unknown;
    vox.sampleCount = 1;

    const rhi::ShaderHandle vsVoxel = compile("VSVoxel", rhi::ShaderStage::Vertex, kBaseSm, rasterDefs(nullptr).c_str());
    const rhi::ShaderHandle gsVoxel = compile("GSVoxel", rhi::ShaderStage::Geometry, kBaseSm, rasterDefs(nullptr).c_str());
    if (vsVoxel && gsVoxel && psVoxel) {
        rhi::GraphicsPipelineDesc p = vox;
        p.vs = vsVoxel; p.gs = gsVoxel; p.ps = psVoxel;
        voxelPso_ = res_->createGraphicsPipeline(p);
    }
    if (!voxelPso_) AVER_ERROR("[Voxi] voxelise pipeline unavailable");

    if (msOk && psVoxel) {
        // The geometry registers sit just past whatever SRV table this pipeline declared, so they
        // are derived from the same layout the pipeline is created with.
        const std::string msDefs = rasterDefs("AVER_MS=1") + ";" + rhi::meshGeometryDefines(vox.layout);
        if (const rhi::ShaderHandle ms = compile("MSVoxel", rhi::ShaderStage::Mesh, 65, msDefs.c_str())) {
            rhi::GraphicsPipelineDesc p = vox;
            p.ms = ms; p.ps = psVoxel;
            voxelMsPso_ = res_->createGraphicsPipeline(p);
        }
        if (!voxelMsPso_) AVER_WARN("[Voxi] mesh-shader voxelise variant unavailable; the GS path stands in");
    }

    // --- 4. clear the accumulator, and reduce it into mip 0. UAV-only layouts, matching the
    //        UAV-only binding sets. ---
    if (const rhi::ShaderHandle cs = compile("CSClear", rhi::ShaderStage::Compute, kBaseSm, nullptr)) {
        rhi::ComputePipelineDesc p;
        p.cs = cs;
        p.layout.uavCount = 2;
        // W3/C-4: the bounded-dispatch constant block at b3 -- the SAME 8 dwords all three of
        // clear/resolve/mip declare (MipCB in voxi.hlsl). EVERY dispatch of this PSO is preceded by
        // ctx.setConstants(3, ...) unconditionally in voxelizePass, bounded dispatch on or off: a
        // declared-but-unset root constant block is the recorded TDR class on this project.
        p.layout.constantDwords[3] = kGiDispatchConstantDwords;
        clearPso_ = res_->createComputePipeline(p);
    }
    if (!clearPso_) AVER_ERROR("[Voxi] volume clear pipeline unavailable");

    if (const rhi::ShaderHandle cs = compile("CSResolve", rhi::ShaderStage::Compute, kBaseSm, nullptr)) {
        rhi::ComputePipelineDesc p;
        p.cs = cs;
        p.layout.uavCount = 2;
        p.layout.constantDwords[3] = kGiDispatchConstantDwords;   // see CSClear's own comment above
        resolvePso_ = res_->createComputePipeline(p);
    }
    if (!resolvePso_) AVER_ERROR("[Voxi] injection resolve pipeline unavailable");

    // --- 5. mip filter: one source mip in, one destination mip out. ---
    if (const rhi::ShaderHandle cs = compile("CSMip", rhi::ShaderStage::Compute, kBaseSm, nullptr)) {
        rhi::ComputePipelineDesc p;
        p.cs = cs;
        p.layout.srvCount = 1;
        p.layout.uavCount = 1;
        // b3: WAS 4 dwords (source mip index alone); NOW the full W3 bounded-dispatch block -- the
        // source mip plus the box filterMips derived for this level via mipBox(). Widened rather than
        // kept separate, since voxi.hlsl declares one MipCB at b3 shared by all three of
        // clear/resolve/mip (C-4), so the three PSOs must agree on its size.
        p.layout.constantDwords[3] = kGiDispatchConstantDwords;   // b3: source mip + dispatch box (was 4: mip alone)
        mipPso_ = res_->createComputePipeline(p);
    }
    if (!mipPso_) AVER_ERROR("[Voxi] mip filter pipeline unavailable");

    // --- 5b. OCCLUSION-AWARE FOG: CSAirVis, the air sky-visibility volume's own march. UNLIKE
    // CSClear/CSResolve/CSMip immediately above, this is compiled against the FULL Voxi layout (`gi`,
    // the same PipelineLayout shadowPso_/voxelPso_/etc. use a few dozen lines up) rather than a tiny
    // srv/uav-only one: CSAirVis reads t0's whole mip chain through gVoxelSamp (s0, declared by `gi`)
    // and writes u16 -- both live in table 0's declared range -- and it needs VoxiFrame's b4 block
    // (gVoxelOrigin/gVoxelParams) the way every other full-layout Voxi compute stage does
    // (recordStagedRayDriven's own header comment states why that costs nothing extra to declare: b4
    // is a root CBV on every PipelineLayout unconditionally, D3D12Device.cpp's root-signature builder
    // loop has no `if` gating it). rasterDefs(nullptr), not csDefs/bindlessDefs: CSAirVis needs no
    // bindless texture table (it touches no material), the identical shape VSShadow/VSVoxel/etc. above
    // already have with the same `gi` layout.
    //
    // SM 6.0, NOT kBaseSm (5.1): no RT, no wave intrinsics, no derivatives -- SM 6.0 is enough, and
    // gating on instancedShadowsOk's own `caps_.shaderModel >= 60 && caps_.dxcAvailable` test (rather
    // than compiling unconditionally at kBaseSm the way CSClear/CSResolve/CSMip do) is what makes a
    // device below that line fall back to the placeholder cleanly instead of failing this compile.
    if (instancedShadowsOk)
    if (const rhi::ShaderHandle csAirVis = compile("CSAirVis", rhi::ShaderStage::Compute, 60,
                                                    rasterDefs(nullptr).c_str())) {
        rhi::ComputePipelineDesc p;
        p.cs = csAirVis;
        p.layout = gi;
        // b3: the slab box (MipCB, the block the voxel kernels share) -- set before every dispatch
        // in dispatchAirVis.
        p.layout.constantDwords[3] = kGiDispatchConstantDwords;
        airVisPso_ = res_->createComputePipeline(p);
    }
    if (!airVisPso_)
        AVER_WARN("[Voxi] air sky-visibility pipeline unavailable (SM {}, DXC {}); "
                  "voxi.fogOcclusion has no effect on this device (placeholder stays bound)",
                  caps_.shaderModel, caps_.dxcAvailable ? "yes" : "no");

    // --- 6-10. everything that bakes the sample count and the target formats. ---
    const bool sceneOk = createScenePipelines(dev_->sampleCount(), dev_->backbufferFormat(),
                                              dev_->depthFormat());
    // Recorded HERE and not inside createScenePipelines, because onRenderTargetsChanged also calls
    // that and a resize is not a graph change; what this remembers is "the pipelines have seen this
    // revision", and both callers leave that true.
    scenePipelineGraphRev_ = pbr::materialGraphs().revision();
    scenePipelineShaderRev_ = rhi::shaderFileRevision();   // same reason as the line above

    return shadowPso_ && voxelPso_ && clearPso_ && mipPso_ && sceneOk;
}

// Creates the five pipelines that bake the sample count and the render-target formats: the debug
// view and the four scene lit variants. Replaces whatever was there.
bool VoxiRenderer::createScenePipelines(u32 sampleCount, rhi::Format color, rhi::Format depth) {
    const bool msOk = caps_.meshShaderTier > 0 && caps_.shaderModel >= 65 && caps_.dxcAvailable;
    const bool rtOk = caps_.rayTracingTier >= 11 && caps_.shaderModel >= 65 && caps_.dxcAvailable;

    const rhi::PipelineHandle stale[] = {debugPso_, scenePso_, sceneMsPso_, sceneRtPso_, sceneMsRtPso_,
                                         sceneBlendedPso_, sceneMsBlendedPso_, sceneRtBlendedPso_,
                                         sceneMsRtBlendedPso_,
                                         depthPrepassPso_, scenePsoPrepassed_, sceneRtPsoPrepassed_,
                                         // rayDrivenPso_ is built below but was missing from this
                                         // destroy list -- every call leaked the old handle by simply
                                         // overwriting the member. Fixed here so its new "Gbuf" twin
                                         // doesn't inherit the identical leak from day one.
                                         rayDrivenPso_,
                                         sceneGbufPso_, sceneMsGbufPso_, sceneRtGbufPso_, sceneMsRtGbufPso_,
                                         sceneBlendedGbufPso_, sceneMsBlendedGbufPso_,
                                         sceneRtBlendedGbufPso_, sceneMsRtBlendedGbufPso_,
                                         scenePsoPrepassedGbuf_, sceneRtPsoPrepassedGbuf_,
                                         // Same two leaks as shutdown()'s list, and the same reason
                                         // they survived: this function OVERWRITES every member a
                                         // few lines down, so a handle missing from here leaks on
                                         // every pipeline rebuild -- once per resize, not once
                                         // per run.
                                         rayDrivenTexPso_, sceneRtBlendedTexPso_,
                                         rayDrivenTexGbufPso_,
                                         rayDrivenGbufPso_,
                                         // STAGED RAY-DRIVEN PASSES (milestone 1, extended by
                                         // milestone 2's rdGiCsPso_/rdSkyOccCsPso_, milestone 3's
                                         // rdReflCsPso_, and milestone 4's rdGiCbCsPso_): the
                                         // identical "this function overwrites every member a few
                                         // lines down" reasoning the comment above already gives for
                                         // rayDrivenTexPso_/sceneRtBlendedTexPso_.
                                         rdVisCsPso_, rdShadowCsPso_, rdGiCsPso_, rdGiCbCsPso_,
                                         rdSkyOccCsPso_, rdReflCsPso_,
                                         rayDrivenSplitTexPso_, rayDrivenSplitTexGbufPso_,
                                         // SUB-STAGE SPLITS (Settings::rayDrivenShadowTiles /
                                         // rayDrivenGiSplit): the identical "this function overwrites
                                         // every member a few lines down" reasoning, for the four extra
                                         // compute pipelines.
                                         rdShadowProbeCsPso_, rdShadowTiledCsPso_,
                                         rdGiTraceCsPso_, rdGiTraceCbCsPso_,
                                         rdGiSplitCsPso_, rdGiSplitCbCsPso_,
                                         // SUB-STAGE C (Settings::rayDrivenReflSplit): the identical
                                         // "this function overwrites every member a few lines down"
                                         // reasoning, for its own two extra compute pipelines.
                                         rdReflSplitCsPso_, rdReflFilterCsPso_,
                                         // LOCAL LIGHTS (LAMPS): the same, for CSRdLocalLights.
                                         rdLocalLightsCsPso_};
    for (rhi::PipelineHandle p : stale) if (p) res_->destroyPipeline(p);
    debugPso_ = scenePso_ = sceneMsPso_ = sceneRtPso_ = sceneMsRtPso_ = 0;
    sceneBlendedPso_ = sceneMsBlendedPso_ = sceneRtBlendedPso_ = sceneMsRtBlendedPso_ = 0;
    depthPrepassPso_ = scenePsoPrepassed_ = sceneRtPsoPrepassed_ = 0;
    rayDrivenPso_ = rayDrivenTexPso_ = 0;
    sceneRtBlendedTexPso_ = 0;
    sceneGbufPso_ = sceneMsGbufPso_ = sceneRtGbufPso_ = sceneMsRtGbufPso_ = 0;
    sceneBlendedGbufPso_ = sceneMsBlendedGbufPso_ = sceneRtBlendedGbufPso_ = sceneMsRtBlendedGbufPso_ = 0;
    scenePsoPrepassedGbuf_ = sceneRtPsoPrepassedGbuf_ = 0;
    rayDrivenGbufPso_ = rayDrivenTexGbufPso_ = 0;
    rdVisCsPso_ = rdShadowCsPso_ = rdGiCsPso_ = rdGiCbCsPso_ = rdSkyOccCsPso_ = rdReflCsPso_ = 0;
    rayDrivenSplitTexPso_ = rayDrivenSplitTexGbufPso_ = 0;
    rdShadowProbeCsPso_ = rdShadowTiledCsPso_ = 0;
    rdGiTraceCsPso_ = rdGiTraceCbCsPso_ = rdGiSplitCsPso_ = rdGiSplitCbCsPso_ = 0;
    rdReflSplitCsPso_ = rdReflFilterCsPso_ = 0;
    rdLocalLightsCsPso_ = 0;

    ShaderScope compile(*res_);
    const rhi::PipelineLayout gi = giLayout();
    // Every pipeline here is a raster one, so every shader is told the material registers.
    const std::string matDefs = pbr::materialShaderDefines(gi.srvCount, kMaterialSamplerSlot,
                                                          layeredBsdf_);
    auto rasterDefs = [&](const char* extra) { return extra ? matDefs + ";" + extra : matDefs; };

    // --- 6. debug: raymarch the volume to screen (shares the prelude's fullscreen triangle). ---
    const rhi::ShaderHandle vsky = compile("VSky", rhi::ShaderStage::Vertex, kBaseSm, rasterDefs(nullptr).c_str());
    if (const rhi::ShaderHandle ps = compile("PSVoxelDebug", rhi::ShaderStage::Pixel, kBaseSm, rasterDefs(nullptr).c_str()); ps && vsky) {
        rhi::GraphicsPipelineDesc p;
        p.vs = vsky; p.ps = ps;
        p.layout = gi;
        p.cull = rhi::CullMode::None;
        p.renderTargetCount = 1;
        p.renderTargets[0] = color;
        p.sampleCount = sampleCount;
        debugPso_ = res_->createGraphicsPipeline(p);
    }
    if (!debugPso_) AVER_ERROR("[Voxi] voxel debug pipeline unavailable");

    // --- 7-10. scene lit variants: the cone trace and RayQuery live inside the pixel shader. ---
    rhi::GraphicsPipelineDesc scene;
    scene.layout = gi;
    scene.cull = rhi::CullMode::None;
    scene.depth = {true, true, rhi::CompareOp::Less};
    scene.renderTargetCount = 1;
    scene.renderTargets[0] = color;
    scene.depthFormat = depth;
    scene.sampleCount = sampleCount;

    // `scene`'s G-buffer twin: IDENTICAL layout/cull/depth/format/sampleCount, four render targets
    // instead of one. The three extra formats are fixed constants, not parameters -- see the
    // "Gbuf" pipeline members' own comment in VoxiRenderer.hpp for why they cannot be anything else
    // and must match IDevice::gBufferVelocityTexture()/gBufferViewZTexture()/
    // gBufferNormalRoughnessTexture()'s own contract (RHI.hpp) one-for-one.
    rhi::GraphicsPipelineDesc sceneGbuf = scene;
    sceneGbuf.renderTargetCount = 4;
    sceneGbuf.renderTargets[1] = rhi::Format::RG16F;          // velocity
    sceneGbuf.renderTargets[2] = rhi::Format::R32Float;       // view-space linear depth
    sceneGbuf.renderTargets[3] = rhi::Format::RGB10A2Unorm;   // world normal + roughness

    const rhi::ShaderHandle vsMain = compile("VSMain", rhi::ShaderStage::Vertex, kBaseSm, rasterDefs(nullptr).c_str());
    const rhi::ShaderHandle psVoxi = compile("PSMainVoxi", rhi::ShaderStage::Pixel, kBaseSm, rasterDefs(nullptr).c_str());
    if (vsMain && psVoxi) {
        rhi::GraphicsPipelineDesc p = scene;
        p.vs = vsMain; p.ps = psVoxi;
        scenePso_ = res_->createGraphicsPipeline(p);
    }
    if (!scenePso_) AVER_ERROR("[Voxi] scene pipeline unavailable");

    // PSMainVoxi recompiled with AVER_GBUFFER=1: VoxiShaders.hpp's #if AVER_GBUFFER makes this
    // change the function's RETURN TYPE (GBufferOut, four SV_TARGETs, instead of a bare float4), so
    // it needs its own ShaderHandle -- reusing psVoxi's binary against a 4-target PSO would be a
    // pipeline/shader mismatch, not merely a wasted target. vsMain is reused unchanged: the vertex
    // stage's signature does not depend on this define at all.
    const rhi::ShaderHandle psVoxiGbuf =
        compile("PSMainVoxi", rhi::ShaderStage::Pixel, kBaseSm, rasterDefs("AVER_GBUFFER=1").c_str());
    if (vsMain && psVoxiGbuf) {
        rhi::GraphicsPipelineDesc p = sceneGbuf;
        p.vs = vsMain; p.ps = psVoxiGbuf;
        sceneGbufPso_ = res_->createGraphicsPipeline(p);
    }
    // OPTIONAL, matching every other axis this family degrades on: pickGbuf() falls back to
    // scenePso_ whenever this is 0, so a device that can't build the G-buffer twin simply never gets
    // one, the same as a mesh-shader or ray-tracing twin failing already does.
    if (!sceneGbufPso_)
        AVER_WARN("[Voxi] G-buffer scene pipeline unavailable; the G-buffer stays off even if requested");

    // MSMain comes from the prelude and needs AVER_MS, plus this layout's geometry registers.
    const std::string msDefs = rasterDefs("AVER_MS=1") + ";" + rhi::meshGeometryDefines(scene.layout);
    const rhi::ShaderHandle msMain = msOk ? compile("MSMain", rhi::ShaderStage::Mesh, 65, msDefs.c_str()) : 0;
    if (msMain && psVoxi) {
        rhi::GraphicsPipelineDesc p = scene;
        p.ms = msMain; p.ps = psVoxi;
        sceneMsPso_ = res_->createGraphicsPipeline(p);
    }
    if (msOk && !sceneMsPso_) AVER_WARN("[Voxi] mesh-shader scene variant unavailable");

    if (msMain && psVoxiGbuf) {
        rhi::GraphicsPipelineDesc p = sceneGbuf;
        p.ms = msMain; p.ps = psVoxiGbuf;
        sceneMsGbufPso_ = res_->createGraphicsPipeline(p);
    }
    if (msOk && !sceneMsGbufPso_) AVER_WARN("[Voxi] mesh-shader G-buffer scene variant unavailable");

    // RayQuery replaces the shadow-map lookup with an exact occlusion ray: a second PS at SM 6.5.
    // Appended to the ray-driven pixel shader's defines only. Empty in every normal build, so the
    // shader text and therefore the DXIL cache key are untouched unless a measurement asks.
    const auto rdAblateDefs = [&]() -> std::string {
        return rdAblate_ ? (";AVER_RD_ABLATE=" + std::to_string(rdAblate_)) : std::string();
    };
    // The single-pass PSRayDriven variants only (rayDrivenStages 0): the voxi.rt* cost toggles become
    // compile-time constants there, because with all four live the megakernel lost the device on AMD.
    // See rtGiShadowBits() in voxi_rt.hlsli. The staged compute stages and PSMainVoxi do not take it.
    // kRdSinglePassLamps (file scope) rides the same string, so all four single-pass compiles take it.
    static constexpr const char* kRdSinglePassDefs =
        kRdSinglePassLamps ? ";AVER_RD_SINGLE_PASS=1" : ";AVER_RD_SINGLE_PASS=1;AVER_RD_SINGLE_PASS_LAMPS=0";
    const rhi::ShaderHandle psRt = rtOk ? compile("PSMainVoxi", rhi::ShaderStage::Pixel, 65, rasterDefs("AVER_RT=1").c_str()) : 0;
    if (vsMain && psRt) {
        rhi::GraphicsPipelineDesc p = scene;
        p.vs = vsMain; p.ps = psRt;
        sceneRtPso_ = res_->createGraphicsPipeline(p);
    }
    if (rtOk && !sceneRtPso_) AVER_WARN("[Voxi] ray-tracing scene variant unavailable");

    // The SAME shader, both extra defines at once: AVER_RT for the ray-query occlusion path,
    // AVER_GBUFFER for the four-target return -- VoxiShaders.hpp's PSMainVoxi nests the two #if
    // blocks rather than treating them as alternatives, so one compile with both strings covers it.
    const rhi::ShaderHandle psRtGbuf =
        rtOk ? compile("PSMainVoxi", rhi::ShaderStage::Pixel, 65,
                       rasterDefs("AVER_RT=1;AVER_GBUFFER=1").c_str())
             : 0;
    if (vsMain && psRtGbuf) {
        rhi::GraphicsPipelineDesc p = sceneGbuf;
        p.vs = vsMain; p.ps = psRtGbuf;
        sceneRtGbufPso_ = res_->createGraphicsPipeline(p);
    }
    if (rtOk && !sceneRtGbufPso_) AVER_WARN("[Voxi] ray-tracing G-buffer scene variant unavailable");

    // RAY-DRIVEN PRIMARY VISIBILITY: the fullscreen triangle VSky already feeds the debug
    // raymarch, paired here with a pixel shader that traces the camera ray instead of marching
    // the voxel volume. Depth is WRITTEN by the shader (SV_DEPTH), so the state has depth-write on
    // and the test Always -- there is no prior depth for a fullscreen pass to test against, and
    // Less would reject every pixel against a cleared far-plane buffer.
    const rhi::ShaderHandle psRayDriven =
        rtOk ? compile("PSRayDriven", rhi::ShaderStage::Pixel, 65,
                       rasterDefs((std::string("AVER_RT=1") + kRdSinglePassDefs + rdAblateDefs()).c_str()).c_str()) : 0;
    if (vsky && psRayDriven) {
        rhi::GraphicsPipelineDesc p;
        p.vs = vsky; p.ps = psRayDriven;
        p.layout = gi;
        p.cull = rhi::CullMode::None;
        p.depth = {true, true, rhi::CompareOp::Always};
        p.renderTargetCount = 1;
        p.renderTargets[0] = color;
        p.depthFormat = depth;
        p.sampleCount = sampleCount;
        rayDrivenPso_ = res_->createGraphicsPipeline(p);
    }
    // A WARNING, NOT AN ERROR, and the mode simply does not engage: rayDrivenActive() requires the
    // pipeline, so a device that cannot compile this keeps rasterising rather than going black.
    if (rtOk && !rayDrivenPso_) AVER_WARN("[Voxi] ray-driven primary-visibility pass unavailable");

    // THE TEXTURED VARIANT, a second PSO rather than a branch inside the first: the bindless table is
    // a root-signature difference, so it CANNOT be a runtime toggle -- declaring the range on every
    // device would put an unbounded-ish descriptor range in front of hardware that doesn't need one.
    // Built only when the device reports it AND the table actually exists (caps says the hardware
    // could, ensureTextureTable() says the descriptors were really reserved); a scene failing the
    // second keeps the flat-albedo pipeline above, itself falling back to the rasteriser -- three
    // levels of fallback, none of them a black screen.
    ensureTextureTable();
    if (rtOk && rtTexTable_) {
        const rhi::PipelineLayout giTex = giLayout(kRtTextureCapacity);
        // Emitted, not written twice: the shader's array length and this layout's declared range
        // are the same number and must stay that way. giLayout() above and this string are the only
        // two places kRtTextureCapacity is read.
        const std::string bindlessDefs = "AVER_RT=1;AVER_RT_BINDLESS=1;AVER_RT_TEX_CAPACITY=" +
                                         std::to_string(kRtTextureCapacity);
        // The SAME material defines: giTex differs from gi only in bindlessTextureCount, which is a
        // root-signature fact and not a register the material prelude cares about.
        const rhi::ShaderHandle vskyTex = compile("VSky", rhi::ShaderStage::Vertex, kBaseSm,
                                                  rasterDefs(bindlessDefs.c_str()).c_str());
        const rhi::ShaderHandle psTex = compile("PSRayDriven", rhi::ShaderStage::Pixel, 65,
                                                rasterDefs((bindlessDefs + kRdSinglePassDefs + rdAblateDefs()).c_str()).c_str());
        if (vskyTex && psTex) {
            rhi::GraphicsPipelineDesc p;
            p.vs = vskyTex; p.ps = psTex;
            p.layout = giTex;
            p.cull = rhi::CullMode::None;
            p.depth = {true, true, rhi::CompareOp::Always};
            p.renderTargetCount = 1;
            p.renderTargets[0] = color;
            p.depthFormat = depth;
            p.sampleCount = sampleCount;
            rayDrivenTexPso_ = res_->createGraphicsPipeline(p);
        }
        if (!rayDrivenTexPso_)
            AVER_WARN("[Voxi] textured ray-driven pass unavailable; hits will shade from material "
                      "factors alone");
        else
            AVER_INFO("[Voxi] textured ray-driven pass ready ({} texture slots)", kRtTextureCapacity);

        // THE TEXTURED G-BUFFER TWIN. Same shader, same bindless layout, plus AVER_GBUFFER=1 so
        // PSRayDriven returns RayDrivenGBufferOut's four targets instead of one. Built here rather
        // than beside rayDrivenGbufPso_ below because `giTex`, `bindlessDefs` and `vskyTex` are
        // scoped to this block -- the bindless range is a root-signature fact, so a G-buffer PSO that
        // wants texturing has to be created where the textured layout exists.
        //
        // WITHOUT THIS, --gbuffer SILENTLY UNTEXTURED THE RENDERER: see the header's comment on this
        // member for why that made every A/B across the flag meaningless.
        const rhi::ShaderHandle psTexGbuf =
            compile("PSRayDriven", rhi::ShaderStage::Pixel, 65,
                    rasterDefs((bindlessDefs + ";AVER_GBUFFER=1" + kRdSinglePassDefs + rdAblateDefs()).c_str()).c_str());
        if (vskyTex && psTexGbuf) {
            rhi::GraphicsPipelineDesc p;
            p.vs = vskyTex; p.ps = psTexGbuf;
            p.layout = giTex;
            p.cull = rhi::CullMode::None;
            // Always/write-on for the same reason rayDrivenPso_ gives: a fullscreen pass has no prior
            // depth to test against, and Less would reject every pixel against a cleared far plane.
            p.depth = {true, true, rhi::CompareOp::Always};
            p.renderTargetCount = 4;
            p.renderTargets[0] = color;
            p.renderTargets[1] = rhi::Format::RG16F;
            p.renderTargets[2] = rhi::Format::R32Float;
            p.renderTargets[3] = rhi::Format::RGB10A2Unorm;
            p.depthFormat = depth;
            p.sampleCount = sampleCount;
            rayDrivenTexGbufPso_ = res_->createGraphicsPipeline(p);
        }
        if (!rayDrivenTexGbufPso_)
            AVER_WARN("[Voxi] textured G-buffer ray-driven pass unavailable; --gbuffer will fall back "
                      "to the flat-albedo G-buffer pipeline");

        // ---- STAGED RAY-DRIVEN PASSES (milestone 1, extended by milestone 2's CSRdGi/CSRdSkyOcc):
        // Stage A/S compute, the milestone 2 lighting stages, Stage B's split twin ----
        //
        // Same layout (giTex) and the SAME defines as psTex/psTexGbuf just above -- CSRdVisibility/
        // CSRdShadow (voxi.hlsl) read gRtInstances/gRtVerts/gRtMaterials/the bindless texture table
        // exactly like PSRayDriven's own trace does, so nothing here can compile against a narrower
        // layout than the textured graphics pipelines did. Building the identical PipelineLayout
        // object means D3D12ResourceFactory::rootSignature dedupes every staged pipeline's root
        // signature with rayDrivenTexPso_'s own (rootSignature keys on layout content, not on
        // compute-vs-graphics -- verified read-only, D3D12Device.cpp).
        const std::string csDefs = bindlessDefs + rdAblateDefs();
        const rhi::ShaderHandle csVis = compile("CSRdVisibility", rhi::ShaderStage::Compute, 65,
                                                rasterDefs(csDefs.c_str()).c_str());
        if (csVis) {
            rhi::ComputePipelineDesc p;
            p.cs = csVis;
            p.layout = giTex;
            rdVisCsPso_ = res_->createComputePipeline(p);
        }
        // SM 6.6, NOT 6.5: rtShadowTemporal's reprojection and spatial filter take ddx/ddy of depth
        // (voxi_rt.hlsli), and derivatives in a compute shader only exist from 6.6. [numthreads(8,8,1)]
        // makes 6.6 form its quads as 2x2 thread blocks, the same neighbourhood a pixel shader's quad
        // is, so the shadow filter sees the derivatives the single pass sees. A device below 6.6 fails
        // this compile, and rdStagedActive() then keeps the single pass.
        const rhi::ShaderHandle csShadow = compile("CSRdShadow", rhi::ShaderStage::Compute, 66,
                                                   rasterDefs(csDefs.c_str()).c_str());
        if (csShadow) {
            rhi::ComputePipelineDesc p;
            p.cs = csShadow;
            p.layout = giTex;
            rdShadowCsPso_ = res_->createComputePipeline(p);
        }
        // SUB-STAGE SPLIT A (Settings::rayDrivenShadowTiles): CSRdShadowProbe, dispatched over the
        // SAME (gx, gy) as csShadow -- one group per 8x8 tile -- writing its verdict to
        // gRdShadowTiles (u18). Same layout/csDefs/SM 6.6 as csShadow immediately above, since it
        // reconstructs the identical surface and traces through the identical rtShadow footprint.
        const rhi::ShaderHandle csShadowProbe = compile("CSRdShadowProbe", rhi::ShaderStage::Compute, 66,
                                                        rasterDefs(csDefs.c_str()).c_str());
        if (csShadowProbe) {
            rhi::ComputePipelineDesc p;
            p.cs = csShadowProbe;
            p.layout = giTex;
            rdShadowProbeCsPso_ = res_->createComputePipeline(p);
        }
        // CSRdShadow recompiled with AVER_RD_SHADOW_TILES=1 -- ORs gRdShadowTiles' own 3x3
        // neighbourhood around its tile and skips the per-pixel ray wherever every probe in it
        // agrees. Same layout/csDefs/SM 6.6 as csShadow; the default compile (csShadow itself, no
        // define) stays byte-for-byte unchanged.
        const rhi::ShaderHandle csShadowTiled =
            compile("CSRdShadow", rhi::ShaderStage::Compute, 66,
                    rasterDefs((csDefs + ";AVER_RD_SHADOW_TILES=1").c_str()).c_str());
        if (csShadowTiled) {
            rhi::ComputePipelineDesc p;
            p.cs = csShadowTiled;
            p.layout = giTex;
            rdShadowTiledCsPso_ = res_->createComputePipeline(p);
        }
        // MILESTONE 2: CSRdGi and CSRdSkyOcc, the two lighting-stage twins dispatched alongside
        // csShadow out of recordStagedRayDriven()'s single "Voxi RD lighting stages" span. Same SM
        // 6.6 requirement as csShadow immediately above for the identical reason -- giRestirIndirect/
        // rtSkyOcclusionTemporal (voxi_restir.hlsli/voxi_rt.hlsli) take ddx/ddy of depth exactly like
        // rtShadowTemporal does, and [numthreads(8,8,1)] forms the same 2x2 quads csShadow relies on
        // for that -- and the same layout/defines, so a device that compiled csShadow has everything
        // these two need too, and one that can't reach 6.6 fails all three identically.
        const rhi::ShaderHandle csGi = compile("CSRdGi", rhi::ShaderStage::Compute, 66,
                                               rasterDefs(csDefs.c_str()).c_str());
        if (csGi) {
            rhi::ComputePipelineDesc p;
            p.cs = csGi;
            p.layout = giTex;
            rdGiCsPso_ = res_->createComputePipeline(p);
        }
        // MILESTONE 4: CSRdGi compiled a second time with AVER_GI_CHECKERBOARD=1 appended -- same
        // csDefs, same SM 6.6, same giTex layout as csGi immediately above, so a device that compiled
        // the plain variant has everything this one needs too. Genuinely optional on top of an
        // already-optional pipeline: rdStagedActive() never inspects this member, so a build (or a
        // single permutation) that fails to produce it leaves rayDrivenStages == 2 behaving exactly
        // like 1 -- see recordStagedRayDriven's own giCb decision and rdGiCbFallbackLogged_.
        const rhi::ShaderHandle csGiCb = compile("CSRdGi", rhi::ShaderStage::Compute, 66,
                                                 rasterDefs((csDefs + ";AVER_GI_CHECKERBOARD=1").c_str()).c_str());
        if (csGiCb) {
            rhi::ComputePipelineDesc p;
            p.cs = csGiCb;
            p.layout = giTex;
            rdGiCbCsPso_ = res_->createComputePipeline(p);
        }
        // SUB-STAGE SPLIT B (Settings::rayDrivenGiSplit): CSRdGiTrace, the candidate trace CSRdGi
        // itself used to run inline -- writes gRdGiCand (u17). Same layout/csDefs/SM 6.6 as csGi
        // above. The checkerboard twin (rdGiTraceCbCsPso_) adds AVER_GI_CHECKERBOARD=1, pairing with
        // csGiCb's own checkerboard dispatch, and is COMPACTED to the traced half's pixels alone
        // (recordStagedRayDriven's own dispatch shape, not a shader-side early return).
        const rhi::ShaderHandle csGiTrace = compile("CSRdGiTrace", rhi::ShaderStage::Compute, 66,
                                                    rasterDefs(csDefs.c_str()).c_str());
        if (csGiTrace) {
            rhi::ComputePipelineDesc p;
            p.cs = csGiTrace;
            p.layout = giTex;
            rdGiTraceCsPso_ = res_->createComputePipeline(p);
        }
        const rhi::ShaderHandle csGiTraceCb =
            compile("CSRdGiTrace", rhi::ShaderStage::Compute, 66,
                    rasterDefs((csDefs + ";AVER_GI_CHECKERBOARD=1").c_str()).c_str());
        if (csGiTraceCb) {
            rhi::ComputePipelineDesc p;
            p.cs = csGiTraceCb;
            p.layout = giTex;
            rdGiTraceCbCsPso_ = res_->createComputePipeline(p);
        }
        // CSRdGi recompiled with AVER_GI_SPLIT=1 -- loads gRdGiCand instead of tracing its own
        // candidate; the plain/checkerboard pair mirrors csGi/csGiCb's own pair exactly, so a project
        // that compiled the checkerboard variant of one has everything the other needs too. The
        // default compiles (csGi/csGiCb themselves, no define) stay byte-for-byte unchanged.
        const rhi::ShaderHandle csGiSplit =
            compile("CSRdGi", rhi::ShaderStage::Compute, 66,
                    rasterDefs((csDefs + ";AVER_GI_SPLIT=1").c_str()).c_str());
        if (csGiSplit) {
            rhi::ComputePipelineDesc p;
            p.cs = csGiSplit;
            p.layout = giTex;
            rdGiSplitCsPso_ = res_->createComputePipeline(p);
        }
        const rhi::ShaderHandle csGiSplitCb =
            compile("CSRdGi", rhi::ShaderStage::Compute, 66,
                    rasterDefs((csDefs + ";AVER_GI_SPLIT=1;AVER_GI_CHECKERBOARD=1").c_str()).c_str());
        if (csGiSplitCb) {
            rhi::ComputePipelineDesc p;
            p.cs = csGiSplitCb;
            p.layout = giTex;
            rdGiSplitCbCsPso_ = res_->createComputePipeline(p);
        }
        const rhi::ShaderHandle csSkyOcc = compile("CSRdSkyOcc", rhi::ShaderStage::Compute, 66,
                                                   rasterDefs(csDefs.c_str()).c_str());
        if (csSkyOcc) {
            rhi::ComputePipelineDesc p;
            p.cs = csSkyOcc;
            p.layout = giTex;
            rdSkyOccCsPso_ = res_->createComputePipeline(p);
        }
        // MILESTONE 3: CSRdRefl, the ray-traced reflection lighting stage, dispatched alongside
        // csShadow/csGi/csSkyOcc out of the SAME "Voxi RD lighting stages" span. Same SM 6.6
        // requirement as csGi/csSkyOcc immediately above for the identical reason -- it calls the same
        // rtReflectionTemporal (voxi_rt.hlsli) PSRayDriven's own un-split reflection block calls today,
        // which takes ddx/ddy of depth exactly like rtShadowTemporal does -- and the same layout/
        // defines, so a device that compiled csShadow has everything this needs too.
        const rhi::ShaderHandle csRefl = compile("CSRdRefl", rhi::ShaderStage::Compute, 66,
                                                 rasterDefs(csDefs.c_str()).c_str());
        if (csRefl) {
            rhi::ComputePipelineDesc p;
            p.cs = csRefl;
            p.layout = giTex;
            rdReflCsPso_ = res_->createComputePipeline(p);
        }
        // SUB-STAGE C (Settings::rayDrivenReflSplit): rdReflSplitCsPso_ (R1) is CSRdRefl recompiled
        // with AVER_RD_REFL_SPLIT=1 -- traces the ray exactly as csRefl above and writes a PENDING
        // marker in place of composing wherever a history is bound to gather against (see gRdReflTex's
        // own header comment, voxi.hlsl, for the fourth alpha this adds). rdReflFilterCsPso_ (R2) is the
        // new CSRdReflFilter entry point, reading R1's own gRtReflHistOut write back this same frame and
        // finishing the compose with rtReflectionSpatial. Same layout/csDefs/SM 6.6 as csRefl -- see
        // CSRdRefl's and CSRdReflFilter's own header comments (voxi.hlsl) for the full split contract.
        const rhi::ShaderHandle csReflSplit =
            compile("CSRdRefl", rhi::ShaderStage::Compute, 66,
                    rasterDefs((csDefs + ";AVER_RD_REFL_SPLIT=1").c_str()).c_str());
        if (csReflSplit) {
            rhi::ComputePipelineDesc p;
            p.cs = csReflSplit;
            p.layout = giTex;
            rdReflSplitCsPso_ = res_->createComputePipeline(p);
        }
        const rhi::ShaderHandle csReflFilter = compile("CSRdReflFilter", rhi::ShaderStage::Compute, 66,
                                                        rasterDefs(csDefs.c_str()).c_str());
        if (csReflFilter) {
            rhi::ComputePipelineDesc p;
            p.cs = csReflFilter;
            p.layout = giTex;
            rdReflFilterCsPso_ = res_->createComputePipeline(p);
        }
        // LOCAL LIGHTS (LAMPS): CSRdLocalLights, compiled exactly like csShadow -- same layout (so the
        // staged root signature is shared), csDefs and SM 6.6 -- since it reprojects into its history
        // through the shadow history's own reprojection and depth test. Optional: a failure leaves the
        // staged path's lamps unlit (its light count stays 0; raster and single-pass do not use it) and
        // never takes the staged path down with it -- rdStagedActive() does not inspect this member.
        const rhi::ShaderHandle csLocalLights = compile("CSRdLocalLights", rhi::ShaderStage::Compute, 66,
                                                        rasterDefs(csDefs.c_str()).c_str());
        if (csLocalLights) {
            rhi::ComputePipelineDesc p;
            p.cs = csLocalLights;
            p.layout = giTex;
            rdLocalLightsCsPso_ = res_->createComputePipeline(p);
        }
        // Stage B: the SAME textured pixel shader psTex/psTexGbuf were compiled from, with
        // ";AVER_RD_SPLIT=1" appended -- it reads gRdVisBuf/gRdSunVisTex instead of tracing and
        // calling rtShadowTemporal itself; everything after that point is unchanged (voxi.hlsl's own
        // comment on AVER_RD_SPLIT). vskyTex is reused: the vertex stage does not depend on this
        // define at all, same as every other twin in this block.
        const rhi::ShaderHandle psSplit =
            compile("PSRayDriven", rhi::ShaderStage::Pixel, 65,
                    rasterDefs((csDefs + ";AVER_RD_SPLIT=1").c_str()).c_str());
        if (vskyTex && psSplit) {
            rhi::GraphicsPipelineDesc p;
            p.vs = vskyTex; p.ps = psSplit;
            p.layout = giTex;
            p.cull = rhi::CullMode::None;
            p.depth = {true, true, rhi::CompareOp::Always};
            p.renderTargetCount = 1;
            p.renderTargets[0] = color;
            p.depthFormat = depth;
            p.sampleCount = sampleCount;
            rayDrivenSplitTexPso_ = res_->createGraphicsPipeline(p);
        }
        const rhi::ShaderHandle psSplitGbuf =
            compile("PSRayDriven", rhi::ShaderStage::Pixel, 65,
                    rasterDefs((csDefs + ";AVER_GBUFFER=1;AVER_RD_SPLIT=1").c_str()).c_str());
        if (vskyTex && psSplitGbuf) {
            rhi::GraphicsPipelineDesc p;
            p.vs = vskyTex; p.ps = psSplitGbuf;
            p.layout = giTex;
            p.cull = rhi::CullMode::None;
            p.depth = {true, true, rhi::CompareOp::Always};
            p.renderTargetCount = 4;
            p.renderTargets[0] = color;
            p.renderTargets[1] = rhi::Format::RG16F;
            p.renderTargets[2] = rhi::Format::R32Float;
            p.renderTargets[3] = rhi::Format::RGB10A2Unorm;
            p.depthFormat = depth;
            p.sampleCount = sampleCount;
            rayDrivenSplitTexGbufPso_ = res_->createGraphicsPipeline(p);
        }
        // ONE LINE FOR THE WHOLE STAGED SET, not one warning per PSO: rdStagedActive() already checks
        // every one of these together every frame staged mode is requested, and its own `reason`
        // output says which requirement was not met at the one point that matters -- when a project
        // actually asks for voxi.rayDrivenStages 1 or 2. Unconditional, like every other optional
        // pipeline in this function: createScenePipelines() only reruns on a real rebuild, never per
        // frame. rdGiCsPso_/rdSkyOccCsPso_/rdReflCsPso_/rdGiCbCsPso_ are reported alongside the rest,
        // even though rdStagedActive() only requires any one of them the frames its own matching cb_
        // condition holds -- a project that never turns those on would otherwise never see whether the
        // milestone 2/3/4 shaders compiled at all.
        if (rdVisCsPso_ && rdShadowCsPso_ && rayDrivenSplitTexPso_)
            AVER_INFO("[Voxi] staged ray-driven passes ready for voxi.rayDrivenStages ({} texture slots, "
                      "G-buffer twin {}, GI stage {}, sky occlusion stage {}, reflection stage {}, "
                      "half-rate GI checkerboard stage {}, shadow-tile sub-stage {}, GI-trace "
                      "sub-stage {}, reflection register/filter sub-stage {}, local lights stage {})",
                      kRtTextureCapacity,
                      rayDrivenSplitTexGbufPso_ ? "ready" : "unavailable",
                      rdGiCsPso_ ? "ready" : "unavailable", rdSkyOccCsPso_ ? "ready" : "unavailable",
                      rdReflCsPso_ ? "ready" : "unavailable", rdGiCbCsPso_ ? "ready" : "unavailable",
                      (rdShadowProbeCsPso_ && rdShadowTiledCsPso_) ? "ready" : "unavailable",
                      (rdGiTraceCsPso_ && rdGiSplitCsPso_) ? "ready" : "unavailable",
                      (rdReflSplitCsPso_ && rdReflFilterCsPso_) ? "ready" : "unavailable",
                      rdLocalLightsCsPso_ ? "ready" : "unavailable");
        else
            AVER_WARN("[Voxi] staged ray-driven passes unavailable (visibility cs {}, shadow cs {}, "
                      "split pixel shader {}); voxi.rayDrivenStages 1 or 2 falls back to the single pass",
                      rdVisCsPso_ ? "ready" : "missing", rdShadowCsPso_ ? "ready" : "missing",
                      rayDrivenSplitTexPso_ ? "ready" : "missing");

        // AND THE BLENDED VARIANT, which is the one that reaches GLASS. A blended draw never goes
        // through PSRayDriven -- the device captures it and replays it after the deferred sky through
        // scenePipeline(..., blended=true), which is PSMainVoxi. So the bindless table on the
        // ray-driven pipeline did nothing for a windowpane: gRtTextures didn't exist in the pipeline
        // painting it, so rtReflection's #ifdef compiled to the flat path, and reflections in glass
        // stayed Lambertian paint against a textured world. Same layout/defines/PSMainVoxi as
        // sceneRtBlendedPso_ below, differing only in blend state and declared bindless range.
        const rhi::ShaderHandle vsMainTex = compile("VSMain", rhi::ShaderStage::Vertex, kBaseSm,
                                                    rasterDefs(bindlessDefs.c_str()).c_str());
        const rhi::ShaderHandle psMainTex = compile("PSMainVoxi", rhi::ShaderStage::Pixel, 65,
                                                    rasterDefs(bindlessDefs.c_str()).c_str());
        if (vsMainTex && psMainTex) {
            rhi::GraphicsPipelineDesc p = scene;
            p.vs = vsMainTex; p.ps = psMainTex;
            p.layout = giTex;
            p.blend = rhi::BlendMode::PremultipliedAlpha;   // see sceneBlendedPso_ for why
            p.depth.test = true;
            p.depth.write = false;
            sceneRtBlendedTexPso_ = res_->createGraphicsPipeline(p);
        }
        if (!sceneRtBlendedTexPso_)
            AVER_WARN("[Voxi] textured blended (glass) variant unavailable; reflections in glass "
                      "will stay flat");
        else
            AVER_INFO("[Voxi] textured blended (glass) variant ready");
    }

    // PSRayDriven's own G-buffer twin: RayDrivenGBufferOut adds SV_DEPTH to the same four targets
    // sceneGbuf already describes, so the desc is built the same way rayDrivenPso_'s own desc was
    // (not derived from `scene`/`sceneGbuf`, which carry a depth STATE this fullscreen pass does not
    // share -- see rayDrivenPso_'s own block for why Always/write-on replaces Less/write-on here).
    const rhi::ShaderHandle psRayDrivenGbuf =
        rtOk ? compile("PSRayDriven", rhi::ShaderStage::Pixel, 65,
                       rasterDefs((std::string("AVER_RT=1;AVER_GBUFFER=1") + kRdSinglePassDefs + rdAblateDefs()).c_str()).c_str())
             : 0;
    if (vsky && psRayDrivenGbuf) {
        rhi::GraphicsPipelineDesc p;
        p.vs = vsky; p.ps = psRayDrivenGbuf;
        p.layout = gi;
        p.cull = rhi::CullMode::None;
        p.depth = {true, true, rhi::CompareOp::Always};
        p.renderTargetCount = 4;
        p.renderTargets[0] = color;
        p.renderTargets[1] = rhi::Format::RG16F;
        p.renderTargets[2] = rhi::Format::R32Float;
        p.renderTargets[3] = rhi::Format::RGB10A2Unorm;
        p.depthFormat = depth;
        p.sampleCount = sampleCount;
        rayDrivenGbufPso_ = res_->createGraphicsPipeline(p);
    }
    if (rtOk && !rayDrivenGbufPso_)
        AVER_WARN("[Voxi] ray-driven G-buffer primary-visibility pass unavailable");

    if (msMain && psRt) {
        rhi::GraphicsPipelineDesc p = scene;
        p.ms = msMain; p.ps = psRt;
        sceneMsRtPso_ = res_->createGraphicsPipeline(p);
    }
    if (msOk && rtOk && !sceneMsRtPso_) AVER_WARN("[Voxi] mesh-shader + ray-tracing scene variant unavailable");

    if (msMain && psRtGbuf) {
        rhi::GraphicsPipelineDesc p = sceneGbuf;
        p.ms = msMain; p.ps = psRtGbuf;
        sceneMsRtGbufPso_ = res_->createGraphicsPipeline(p);
    }
    if (msOk && rtOk && !sceneMsRtGbufPso_)
        AVER_WARN("[Voxi] mesh-shader + ray-tracing G-buffer scene variant unavailable");

    // --- 10b. blended twins of 7-10: SAME shader binaries, premultiplied-alpha + depth-write off. ---
    // A blended draw DOES reach submitDraw() (IDevice::setDrawBlended/drawBlended mark it via
    // IRenderFeature::submitDraw's `blended` param); THIS RENDERER's submitDraw() routes it to the
    // translucent lane rather than adding it to draws_/drawsPrev_. CORRECTED from an earlier version
    // of this comment that said a blended draw is absent from the TLAS entirely -- that stopped being
    // true once submitDraw grew the translucent lane (submit(..., translucent=true), above). The
    // RASTER side is unchanged: IDevice still sorts every blended draw back-to-front and replays it
    // in endFrame through scenePipeline(..., blended=true) below.
    // WHAT IS ACTUALLY EXCLUDED, now TWO things, not three:
    //   IN the TLAS   -- masked kRtMaskTranslucent, flagged FORCE_NON_OPAQUE, letting rtShadow's
    //                    Proceed() walk attenuate a shadow through the pane. Glass DOES cast a
    //                    shadow now.
    //   OUT of voxelisation, the shadow cascade and the GI shadow map -- all depth-only, no channel
    //                    for a transmittance, by construction rather than choice.
    //   OUT of reflection and primary-visibility RAYS -- by MASK, not absence: both trace
    //                    AVER_RT_MASK_OPAQUE. Widening that mask is all it would take to let glass
    //                    reflect glass; the geometry is already there.
    // So the standing approximations are: no GI bounce CONTRIBUTED by it, no reflection hit ON it.
    // Being dropped IN submitDraw costs glass exactly the three items above and nothing else.
    // What glass DOES still get, running the SAME pixel shader as the opaque pass: a shadow cast BY
    // something opaque (shadowFactor()/rtShadowTemporal() test glass's own pixel like any surface),
    // and indirect light landing ON it (coneTracedIndirect() samples the volume).
    // The GraphicsPipelineDesc is `scene` UNCHANGED except BlendMode::PremultipliedAlpha instead of
    // Opaque (see the first blended pipeline below for why, not straight AlphaBlend) and depth.write
    // forced false while depth.test stays true -- glass still occludes without writing depth.
    // Back-to-front ordering across MULTIPLE blended draws is the CALLER's job (IDevice sorts the
    // captured list by camera distance before replaying); this pipeline only handles one.
    if (vsMain && psVoxi) {
        rhi::GraphicsPipelineDesc p = scene;
        p.vs = vsMain; p.ps = psVoxi;
        // PREMULTIPLIED, NOT STRAIGHT ALPHA -- this used to be BlendMode::AlphaBlend, whose equation
        // dst = src.rgb*a + dst.rgb*(1-a) attenuates EVERY term in src.rgb by the same a, specular
        // included: a pane at a usefully transparent alpha of 0.2 showed its own sky/GI reflection at
        // 20% strength, the single biggest reason glass read as flat cartoon tint -- a real dielectric
        // transmits the background and reflects at FULL strength, two different weights.
        // PremultipliedAlpha's dst = src.rgb + dst.rgb*(1-a) lands whatever PSMainVoxi's blended
        // branch put into src.rgb unattenuated; PSMainVoxi's job (averBlendedOutput's contract in
        // VoxiShaders.hpp) is to pack rgb = specular + diffuse*alpha itself, so only the diffuse lobe
        // gets multiplied by a here. depth.test/write are UNCHANGED: this is only about src.rgb's
        // content, not how the two surfaces occlude each other.
        p.blend = rhi::BlendMode::PremultipliedAlpha;
        p.depth.test = true;    // inherited from `scene` already; restated for this block's own sake
        p.depth.write = false;
        sceneBlendedPso_ = res_->createGraphicsPipeline(p);
    }
    if (!sceneBlendedPso_)
        AVER_WARN("[Voxi] blended scene pipeline unavailable; translucent materials will not draw");

    // The blended twin's OWN G-buffer twin: same PremultipliedAlpha/depth-write-off state as
    // sceneBlendedPso_, `sceneGbuf`'s four targets instead of `scene`'s one, psVoxiGbuf instead of
    // psVoxi. PSMainVoxi's AVER_GBUF_RETURN macro expands at EVERY return site including the
    // translucent branch, so glass's own velocity/depth/normal are written exactly like an opaque
    // surface's -- nothing glass-specific to gate.
    if (vsMain && psVoxiGbuf) {
        rhi::GraphicsPipelineDesc p = sceneGbuf;
        p.vs = vsMain; p.ps = psVoxiGbuf;
        p.blend = rhi::BlendMode::PremultipliedAlpha;
        p.depth.test = true;
        p.depth.write = false;
        sceneBlendedGbufPso_ = res_->createGraphicsPipeline(p);
    }
    if (!sceneBlendedGbufPso_)
        AVER_WARN("[Voxi] blended G-buffer scene pipeline unavailable; a translucent draw's "
                  "G-buffer output drops out while the plain blended pipeline still draws it");

    if (msMain && psVoxi) {
        rhi::GraphicsPipelineDesc p = scene;
        p.ms = msMain; p.ps = psVoxi;
        p.blend = rhi::BlendMode::PremultipliedAlpha;   // see sceneBlendedPso_ above for why
        p.depth.test = true;    // same test-on/write-off state as sceneBlendedPso_ above
        p.depth.write = false;
        sceneMsBlendedPso_ = res_->createGraphicsPipeline(p);
    }
    if (msOk && !sceneMsBlendedPso_)
        AVER_WARN("[Voxi] mesh-shader blended scene variant unavailable");

    if (msMain && psVoxiGbuf) {
        rhi::GraphicsPipelineDesc p = sceneGbuf;
        p.ms = msMain; p.ps = psVoxiGbuf;
        p.blend = rhi::BlendMode::PremultipliedAlpha;
        p.depth.test = true;
        p.depth.write = false;
        sceneMsBlendedGbufPso_ = res_->createGraphicsPipeline(p);
    }
    if (msOk && !sceneMsBlendedGbufPso_)
        AVER_WARN("[Voxi] mesh-shader G-buffer blended scene variant unavailable");

    if (vsMain && psRt) {
        rhi::GraphicsPipelineDesc p = scene;
        p.vs = vsMain; p.ps = psRt;
        p.blend = rhi::BlendMode::PremultipliedAlpha;   // see sceneBlendedPso_ above for why
        p.depth.test = true;    // same test-on/write-off state as sceneBlendedPso_ above
        p.depth.write = false;
        sceneRtBlendedPso_ = res_->createGraphicsPipeline(p);
    }
    if (rtOk && !sceneRtBlendedPso_)
        AVER_WARN("[Voxi] ray-traced blended scene variant unavailable");

    if (vsMain && psRtGbuf) {
        rhi::GraphicsPipelineDesc p = sceneGbuf;
        p.vs = vsMain; p.ps = psRtGbuf;
        p.blend = rhi::BlendMode::PremultipliedAlpha;
        p.depth.test = true;
        p.depth.write = false;
        sceneRtBlendedGbufPso_ = res_->createGraphicsPipeline(p);
    }
    if (rtOk && !sceneRtBlendedGbufPso_)
        AVER_WARN("[Voxi] ray-traced G-buffer blended scene variant unavailable");

    if (msMain && psRt) {
        rhi::GraphicsPipelineDesc p = scene;
        p.ms = msMain; p.ps = psRt;
        p.blend = rhi::BlendMode::PremultipliedAlpha;   // see sceneBlendedPso_ above for why
        p.depth.test = true;    // same test-on/write-off state as sceneBlendedPso_ above
        p.depth.write = false;
        sceneMsRtBlendedPso_ = res_->createGraphicsPipeline(p);
    }
    if (msOk && rtOk && !sceneMsRtBlendedPso_)
        AVER_WARN("[Voxi] mesh-shader + ray-traced blended scene variant unavailable");

    if (msMain && psRtGbuf) {
        rhi::GraphicsPipelineDesc p = sceneGbuf;
        p.ms = msMain; p.ps = psRtGbuf;
        p.blend = rhi::BlendMode::PremultipliedAlpha;
        p.depth.test = true;
        p.depth.write = false;
        sceneMsRtBlendedGbufPso_ = res_->createGraphicsPipeline(p);
    }
    if (msOk && rtOk && !sceneMsRtBlendedGbufPso_)
        AVER_WARN("[Voxi] mesh-shader + ray-traced G-buffer blended scene variant unavailable");

    // --- 11-13. the depth prepass, and the two scene variants that trust it. ---
    // PSDepthPrepass (VoxiShaders.hpp) alpha-tests and clips but writes no colour: renderTargetCount
    // stays 0. PAIRED WITH vsMain, the SAME compiled vertex shader handle scenePso_/sceneRtPso_ use, so
    // this pass's depth and the real colour pass's depth are bit-identical for the same instance --
    // same MVP, clip, rasteriser rounding, shader binary. depth = {true, true, Less} is the ORDINARY
    // scene depth state: it writes depth like a normal opaque draw, just never runs the expensive PS.
    // COSTS ONE TEXTURE FETCH PER COVERED PIXEL ON EVERY MATERIAL, ALPHA-TESTED OR NOT: PSDepthPrepass
    // must read gMaterialFlags before it knows whether this material needs the clip, so opaque
    // materials pay a wasted branch+cbuffer read and alpha-tested ones pay that plus one Sample() --
    // both far cheaper than what they replace: PSMainVoxi's shadow lookup, cone trace and fog on every
    // hidden fragment behind them. See PSDepthPrepass's own comment for the full accounting.
    const rhi::ShaderHandle psDepthPrepass =
        compile("PSDepthPrepass", rhi::ShaderStage::Pixel, kBaseSm, rasterDefs(nullptr).c_str());
    if (vsMain && psDepthPrepass) {
        rhi::GraphicsPipelineDesc p;
        p.vs = vsMain;
        p.ps = psDepthPrepass;
        p.layout = gi;                 // SAME table-0 shape as the colour pass -- see the .hpp comment
        p.cull = rhi::CullMode::None;   // matches `scene.cull`: foliage two-sided, culled the same way
        p.depth = {true, true, rhi::CompareOp::Less};
        p.renderTargetCount = 0;        // depth-only: no colour output at all, not even an unused one
        p.depthFormat = depth;
        p.sampleCount = sampleCount;
        depthPrepassPso_ = res_->createGraphicsPipeline(p);
    }
    if (!depthPrepassPso_)
        AVER_WARN("[Voxi] depth prepass pipeline unavailable; --depth-prepass will have no effect");

    // The colour-pass twins that TRUST the prepass: same shaders as scenePso_/sceneRtPso_, LessEqual
    // depth test with writes OFF instead of Less/write. LessEqual (not Equal) is deliberate -- see
    // IDevice::drawMeshDepthPrepass's comment: with bit-identical geometry between the two passes, the
    // prepass already found the true per-pixel minimum depth for this instance, so nothing from the
    // SAME geometry can be strictly less than what's there, and LessEqual behaves exactly like Equal
    // without a new CompareOp neither backend declares today.
    if (vsMain && psVoxi && depthPrepassPso_) {
        rhi::GraphicsPipelineDesc p = scene;
        p.vs = vsMain; p.ps = psVoxi;
        p.depth = {true, false, rhi::CompareOp::LessEqual};
        scenePsoPrepassed_ = res_->createGraphicsPipeline(p);
    }
    if (depthPrepassPso_ && !scenePsoPrepassed_)
        AVER_WARN("[Voxi] prepassed scene pipeline unavailable; --depth-prepass will have no effect");

    if (vsMain && psVoxiGbuf && depthPrepassPso_) {
        rhi::GraphicsPipelineDesc p = sceneGbuf;
        p.vs = vsMain; p.ps = psVoxiGbuf;
        p.depth = {true, false, rhi::CompareOp::LessEqual};
        scenePsoPrepassedGbuf_ = res_->createGraphicsPipeline(p);
    }
    if (depthPrepassPso_ && !scenePsoPrepassedGbuf_)
        AVER_WARN("[Voxi] prepassed G-buffer scene pipeline unavailable; --depth-prepass and the "
                  "G-buffer will not combine even though each works alone");

    if (rtOk && vsMain && psRt && depthPrepassPso_) {
        rhi::GraphicsPipelineDesc p = scene;
        p.vs = vsMain; p.ps = psRt;
        p.depth = {true, false, rhi::CompareOp::LessEqual};
        sceneRtPsoPrepassed_ = res_->createGraphicsPipeline(p);
    }
    if (rtOk && depthPrepassPso_ && !sceneRtPsoPrepassed_)
        AVER_WARN("[Voxi] prepassed ray-traced scene pipeline unavailable; ray tracing keeps its "
                  "normal depth state under --depth-prepass");

    if (rtOk && vsMain && psRtGbuf && depthPrepassPso_) {
        rhi::GraphicsPipelineDesc p = sceneGbuf;
        p.vs = vsMain; p.ps = psRtGbuf;
        p.depth = {true, false, rhi::CompareOp::LessEqual};
        sceneRtPsoPrepassedGbuf_ = res_->createGraphicsPipeline(p);
    }
    if (rtOk && depthPrepassPso_ && !sceneRtPsoPrepassedGbuf_)
        AVER_WARN("[Voxi] prepassed ray-traced G-buffer scene pipeline unavailable; ray tracing keeps "
                  "its normal depth state under --depth-prepass with the G-buffer on, same as without it");

    // Only the two mandatory ones: scenePipeline() falls back when the optional variants are absent,
    // covering depthPrepassPso_/scenePsoPrepassed_/sceneRtPsoPrepassed_ and every blended variant
    // being absent (a device with none simply never draws translucent geometry; init() doesn't fail
    // for it, same as a device with no ray-tracing tier). Every "Gbuf" twin is optional on identical
    // terms -- pickGbuf() falls back to the plain pipeline whenever its twin is 0, so a device that
    // can't build one simply never has a G-buffer to offer.
    return debugPso_ && scenePso_;
}

} // namespace aver::voxi
