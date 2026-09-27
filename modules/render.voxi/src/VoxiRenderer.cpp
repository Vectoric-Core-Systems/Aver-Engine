// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
#include "aver/voxi/VoxiRenderer.hpp"
#include "aver/core/CpuTiming.hpp"   // CpuNest(CpuSpan::VoxiSubmit) below -- Types.hpp + stdlib only,
                                     // so no AVER_MODULE_VOXI guard needed, same as Log.hpp/Math.hpp.
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

// GI-only shadow map: one box fit to the GI volume, not the camera, so camera cascades stay
// single-purpose. Old union-with-last-cascade approach measured cascade 3 at 36,744cm radius vs a
// camera-fitted 9,923cm (3.7x wider, ~14x area), admitting every draw into the per-cascade cull
// (census [3, 4, 5, 27]) every frame and wasting texels on volume the camera can't see.
// 1024 feeds a 128-voxel grid at Medium (512 at Epic), 8 texels/edge (2 at Epic); updates only on
// giUpdateInterval's cadence.
constexpr u32 kGiShadowSize = 1024;

// RTXDI ReSTIR GI reservoir sizing -- MIRRORS rtxdi::CalculateReservoirBufferParameters
// (third_party/rtxdi/Source/RtxdiUtils.cpp) rather than linking it (would need a
// third_party/rtxdi/CMakeLists.txt change for a four-line formula). The shader computes the
// identical thing from gGiSurfHist's own dimensions (voxi.hlsl's giReservoirBufferParams), so both
// sides derive from the same width/height rather than trusting two implementations to agree.
constexpr u32 kGiReservoirBlockSize = 16;    // RTXDI_RESERVOIR_BLOCK_SIZE (RtxdiParameters.h)
constexpr u32 kGiReservoirBufferCount = 2;   // rtxdi::c_NumReSTIRGIReservoirBuffers (GI/ReSTIRGI.h)
constexpr u32 kGiReservoirElemBytes = 32;    // sizeof(RTXDI_PackedGIReservoir) -- GI/ReSTIRGIParameters.h
u32 giReservoirElemCount(u32 width, u32 height) {
    const u32 blocksX = (width + kGiReservoirBlockSize - 1) / kGiReservoirBlockSize;
    const u32 blocksY = (height + kGiReservoirBlockSize - 1) / kGiReservoirBlockSize;
    const u32 blockRowPitch = blocksX * kGiReservoirBlockSize * kGiReservoirBlockSize;
    return blockRowPitch * blocksY * kGiReservoirBufferCount;
}

// Staged ray-driven passes: rdVisBuf_'s element size. One uint4/pixel -- see gRdVisBuf (voxi.hlsl)
// for the hit/miss encoding. No block rounding (unlike kGiReservoirBlockSize): pitch = render
// target's exact width, so ensureRdStagedResources' size check (rdStagedW_/H_) catches any resize.
constexpr u32 kRdVisElemBytes = 16;

// Sub-stage splits (Settings::rayDrivenShadowTiles / rayDrivenGiSplit): candidate buffers
// CSRdShadowProbe/CSRdGiTrace write and CSRdShadow/CSRdGi read back.
// kRdGiCandElemBytes: sizeof(RdGiCand) (voxi_restir.hlsli) -- float3 pos, uint flags, float3 nrm,
// f2LumTraced, float3 rad, f2LumSky = 48 bytes, one per pixel, same row pitch as rdVisBuf_.
constexpr u32 kRdGiCandElemBytes = 48;
// kRdShadowTileElemBytes: one uint mask per 8x8 tile (ceil(W/8)*ceil(H/8) tiles, not per pixel) --
// orders of magnitude smaller than rdVisBuf_/rdGiCandBuf_.
constexpr u32 kRdShadowTileElemBytes = 4;

// Local lights (lamps) in the single-pass PSRayDriven (voxi.rayDrivenStages 0): whether that
// megakernel compiles with the lamp term (AVER_RD_SINGLE_PASS_LAMPS, voxi.hlsl). A C++ switch
// because the megakernel sits at the AMD driver's register limit (lost the device before ec35bb5a),
// so lamps must be removable without touching the shader. false appends
// ";AVER_RD_SINGLE_PASS_LAMPS=0" to its four compiles (createScenePipelines) AND keeps the light
// count at 0 for the single pass (scenePass), so the blended replay/ReSTIR emitter drop never act
// on lamp light the opaque pixels didn't receive.
constexpr bool kRdSinglePassLamps = true;

// Cascade split blend: 0 is uniform slabs, 1 is logarithmic (equal ratios).
constexpr f32 kCascadeSplitLambda = 0.85f;

// How far the cascades reach, as a multiple of the camera's near plane.
constexpr f32 kShadowRangeFromNear = 4000.0f;

// How small a caster has to get, measured in this cascade's own shadow-map texels, before it stops
// being drawn into it. ONE texel is the honest floor rather than a tuned number: below it the map
// has no sample that can hold the object, so the draw cannot change the image it is drawn into.
constexpr f32 kMinShadowTexels = 1.0f;

// Draw-list cap, and therefore the TLAS instance count. Was 4096: Electric Dreams passes mid-stream
// (~6,370 resident entities), and draws past the cap were dropped silently by submit(), so those
// entities rendered elsewhere but cast no cascade/GI shadow and wrote nothing to the voxel grid -- a
// shading bug, not a missing object. 16384 covers the demo with room to stream; costs a 1 MB TLAS
// instance array (64 B each), paid only when ray tracing is on.
constexpr u32 kMaxDraws = 16384;

// TLAS instance-mask lanes: a ray's mask is ANDed with an instance's, zero result skips it. Needed
// because a shadow ray must still see a translucent pane (to attenuate through it) while a
// solid-only ray must exclude it; kRtMaskAll stays 0xFF so existing TraceRayInline calls are
// unchanged until they deliberately narrow.
// Translucent-lane instance count this build, logged once (and on change) to answer "is the pane in
// the structure" for missing-translucent-shadow debugging.
u32 tlasTranslucentThisBuild_ = 0;
u32 tlasTranslucentLogged_    = 0;
// Alpha-masked instance count, logged separately: a translucent pane lets a shadow ray attenuate
// through it, while a cutout instance lets any ray see the holes -- both give up the hardware's
// any-hit skip, so the count is the first thing a "why did foliage get expensive" question needs.
u32 tlasAlphaMaskedThisBuild_ = 0;
u32 tlasAlphaMaskedLogged_    = 0;
constexpr u32 kRtMaskOpaque      = 0x01;
constexpr u32 kRtMaskTranslucent = 0x02;
// The viewer's own first-person body -- see AVER_RT_MASK_OWNER_HIDDEN in voxi.hlsl (must match).
// Opaque geometry every ray may hit except the ray-driven primary one.
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

// Wider than kGiSrvCount/kGiUavCount, DELIBERATELY: those two (VoxiGiShaders.hpp) are the union
// SandboxApp.cpp's GPU per-cluster path sizes its own binding set against
// (`kClusterGiSrvBase + voxi::kGiSrvCount`, and `bsd.uavCount = voxi::kGiUavCount` with uavKinds[0..3]
// filled by hand there) -- widening either directly would silently rebase every register past it in
// a file this change never touches, and on the UAV side would grow the count without growing the
// kinds Vulkan validates against. Voxi sizes its own table against these wider constants instead, so
// bindGiResources() (which only fills the cluster path's first two slots) never sees the extra ones.
//
// Extra SRV slots (kind and reason for each in giTableKinds() below): t9 material table; t10
// blended-pass backdrop (the opaque scene copied before translucency replays, so glass can tint per
// channel instead of through one blend alpha); t11 sky-occlusion history; t12/t13 RTXDI ReSTIR GI
// surface history split across two textures (rhi::Format has no 4-channel 32-bit float); t14
// NRD-denoised AO; t15 NRD-denoised ReSTIR GI radiance; t16 half-res ReSTIR visibility history (see
// giVisHistWanted()); t17 air sky-visibility volume, fixed 32^3 independent of voxelResolution,
// sampled by voxiAirVisibility() (voxi.hlsl) to attenuate fog in-scatter, 1x1x1 placeholder when
// voxi.fogOcclusion is off or the real texture isn't created yet (shader treats GetDimensions()<=1
// as "no volume"); t18 local-light list (gRdLocalLights); t19 local-light visibility history
// (rdLocalHist_, u19's read twin) -- shaders skip both unless gCameraMedium.z's light count is
// nonzero this frame.
constexpr u32 kVoxiSrvCount = kGiSrvCount + 11;

// The denoiser index Voxi asks NRD to run. create() is handed exactly one kind
// (ReblurDiffuseOcclusion), so this is 0 -- named rather than a bare literal at the call site,
// since the two must agree and nothing else would say so.
constexpr u32 kNrdAoDenoiser[] = {0u};
// Both denoisers, in create()'s own order: 0 occlusion, 1 diffuse radiance. Run as a pair only when
// ReSTIR GI is active -- the radiance signal doesn't exist otherwise, and denoising a blank input
// is the one way to get a confident wrong image.
constexpr u32 kNrdAoAndGiDenoisers[] = {0u, 1u};
// Diffuse radiance denoiser alone: sky occlusion isn't traced at Low, but ReSTIR GI can still run,
// so a project can want GI denoising with no occlusion signal.
constexpr u32 kNrdGiDenoiser[] = {1u};

// Extra UAV slots, same widening reasoning as kVoxiSrvCount above (kind/reason per slot in
// giTableKinds() below): u4/u5 sky-occlusion history + hit distance; u6 RTXDI reservoir buffer
// (RTXDI_PackedGIReservoir, 32 B/element -- giReservoirs_); u7/u8 GI-restir surface history write
// side (t12/t13's twin); u10 half-res ReSTIR visibility history write side (t16's twin); u11/u12
// ray-driven visibility-record buffer + sun-visibility texture (rdVisBuf_/rdSunVisTex_); u13/u14
// ray-driven GI/sky-occlusion outputs (rdGiTex_/rdAoTex_); u15 ray-driven reflection (rdReflTex_);
// u16 air sky-visibility volume write side (t17's twin, same resource -- unlike u11-u15 this one has
// an SRV since the shade passes also read it); u17/u18 sub-stage split candidate buffers (gRdGiCand,
// 48 B/element, and gRdShadowTiles, one uint/8x8 tile -- see rdGiCandBuf_/rdShadowTileBuf_ in
// VoxiRenderer.hpp for why neither is gated on the setting that consumes it); u19 local-light
// history write side (t19's twin). u11 onward are ALWAYS declared, staged or not, and bound to a
// placeholder when the real resource is absent -- Tier 1 hardware needs a valid descriptor of the
// declared kind in every reserved slot, the same reason u1's voxelAccumPlaceholder_ exists.
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
    // t14: NRD-denoised sky occlusion -- the only SRV here routinely absent (optional at 3 levels:
    // no NRD in the build, a backend refusing NRD's register spaces (Vulkan), or no G-buffer to
    // feed it). Shader tests GetDimensions() rather than a cbuffer flag, same trick
    // averBlendBackdropValid uses for t10 (see aver-voxi-cbuffer-three-mirrors): a null-filled
    // Texture2D reports zero dimensions, a signal the descriptor already carries for free, costing
    // no fourth mirror of the constant block.
    srv[14] = rhi::SlotKind::Texture2D;             // t14 NRD-denoised sky occlusion (read)
    // t15/u9: ReSTIR GI radiance, out to NRD and back. u9 written by giRestirIndirect (rgb =
    // indirect diffuse, a = normalised hit distance); t15 is the same signal one frame later
    // through REBLUR_DIFFUSE. Absent unless ReSTIR GI and NRD are both running.
    srv[15] = rhi::SlotKind::Texture2D;             // t15 NRD-denoised ReSTIR GI radiance (read)
    // t16/u10: half-res ReSTIR VISIBILITY history pair (giVisHist_) -- bound only while
    // giVisHistWanted() (giRestirVisibility == HalfResolution); same GetDimensions() test as t14/t15.
    srv[16] = rhi::SlotKind::Texture2D;             // t16 ReSTIR visibility half-res history (read)
    // t17/u16: air sky-visibility volume -- see kVoxiSrvCount's comment above. Texture3D SRV like
    // t0, sampled trilinear over the same voxel space, at its own fixed 32^3 resolution.
    srv[17] = rhi::SlotKind::Texture3D;             // t17 air sky-visibility volume (read)
    // t18/t19/u19: local lights (lamps) -- see kVoxiSrvCount/kVoxiUavCount above. t18 is a
    // StructuredBuffer like t3-t5/t9; t19 a Texture2D history read side like t6/t11.
    srv[18] = rhi::SlotKind::StructuredBuffer;      // t18 local-light list (gRdLocalLights)
    srv[19] = rhi::SlotKind::Texture2D;             // t19 local-light history (read)
    uav[0] = rhi::SlotKind::Texture3D;              // u0 volume mip 0
    uav[1] = rhi::SlotKind::Texture3D;              // u1 injection accumulator
    uav[2] = rhi::SlotKind::Texture2D;              // u2 ray-traced shadow history (write)
    uav[3] = rhi::SlotKind::Texture2D;              // u3 ray-traced reflection history (write)
    uav[4] = rhi::SlotKind::Texture2D;              // u4 sky-occlusion history (write)
    // u5: sky-occlusion ray's hit distance, written raw, never read back (rtAoHitDist_). Same
    // condition as the u4/t11 pair (aoHistoryWanted()) -- gRtDenoiseParams.w covers both.
    uav[5] = rhi::SlotKind::Texture2D;              // u5 sky-occlusion hit distance (write)
    // u6: RTXDI's reservoir storage -- RWStructuredBuffer<RTXDI_PackedGIReservoir>, one buffer
    // holding both ping-pong copies via RTXDI_ReservoirPositionToPointer's array-index term
    // (giReservoirs_), never rebound mid-session the way a texture pair is.
    uav[6] = rhi::SlotKind::StructuredBuffer;       // u6 GI-restir reservoir buffer
    uav[7] = rhi::SlotKind::Texture2D;              // u7 GI-restir surface POSITION history (write)
    uav[8] = rhi::SlotKind::Texture2D;              // u8 GI-restir surface NORMAL history (write)
    uav[9] = rhi::SlotKind::Texture2D;              // u9 ReSTIR GI radiance + hit distance (write)
    uav[10] = rhi::SlotKind::Texture2D;             // u10 ReSTIR visibility half-res history (write)
    // u11/u12: staged ray-driven visibility record buffer + sun-visibility texture, always declared
    // regardless of rayDrivenStages. u11 StructuredBuffer of uint4 (rdVisBuf_, one/pixel); u12 RW
    // Texture2D (rdSunVisTex_, RGBA16F, rgb = sun transmittance). No SRV twin -- read+written
    // through this UAV alone by CSRdVisibility/CSRdShadow/PSRayDriven's AVER_RD_SPLIT branch, which
    // is why kVoxiSrvCount above didn't have to move.
    uav[11] = rhi::SlotKind::StructuredBuffer;      // u11 ray-driven visibility record buffer
    uav[12] = rhi::SlotKind::Texture2D;             // u12 ray-driven sun visibility (RW)
    // u13/u14: CSRdGi's/CSRdSkyOcc's outputs (rdGiTex_ RGBA16F rgb=ReSTIR diffuse, rdAoTex_
    // RGBA16F r=occlusion). Same always-declared/placeholder/no-SRV-twin contract as u11/u12.
    uav[13] = rhi::SlotKind::Texture2D;             // u13 ray-driven GI indirect diffuse (RW)
    uav[14] = rhi::SlotKind::Texture2D;             // u14 ray-driven sky occlusion (RW)
    // u15: CSRdRefl's output (rdReflTex_, RGBA16F, rgb = traced/sky reflection radiance, a = "traced
    // this pixel" flag). Same contract as u11-u14.
    uav[15] = rhi::SlotKind::Texture2D;             // u15 ray-driven reflection (RW)
    // u16: CSAirVis's write side, t17's UAV twin (same resource) -- see kVoxiUavCount above.
    uav[16] = rhi::SlotKind::Texture3D;             // u16 air sky-visibility volume (write, CSAirVis)
    // u17/u18: sub-stage splits' own buffers (Settings::rayDrivenShadowTiles/rayDrivenGiSplit) --
    // see kVoxiUavCount above. Both StructuredBuffer, no SRV twin.
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
    // Full slot list is in giTableKinds() above. srvCount/uavCount are BASED on kVoxiSrvCount/
    // kVoxiUavCount, not fixed registers, so widening table 0 rebases everything automatically --
    // used to be typed twice (a bare "9" here and again in createVoxelVolume's BindingSetDesc, with
    // no compile-time link, so a mismatch was a descriptor-table error at draw time).
    l.srvCount = kVoxiSrvCount;
    l.uavCount = kVoxiUavCount;
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

// Voxi's shader body, read from shaders/voxi.hlsl. THE ACCESSOR IS HERE, NOT IN VoxiShaders.hpp:
// tests/render.voxi/src/VoxiRtSeqTest.cpp reads the same source to assert its C++ mirror still
// matches, linking Aver.Core alone -- rhi::shaderFile in the header would drag in Aver.RHI. It reads
// modules/render.voxi/shaders/voxi.hlsl directly through AVER_REPO_ROOT instead.
const char* voxiHlsl() {
    // No static: the loader owns the cache and reloadShaderFiles() clears it. A static here would
    // survive a reload and hand back the shader that was read at startup for the rest of the run.
    return rhi::shaderFile("voxi.hlsl").c_str();
}

// The prelude Voxi's HLSL compiles on top of: RHI shared declarations, then the material system's
// BRDF/Aver* contract, then -- when the project has any -- the averEvalMaterial its material
// GRAPHS compile to. AVER_MATERIAL_GRAPH is #defined IN THE TEXT, not passed as a -D: it lands
// between the two preludes so any shader seeing the graph function necessarily also saw the define
// (a -D on every shader risks one missed shader carrying both definitions and failing far from the
// cause). Rebuilt when the registry's revision moves, not once per process -- materials load after
// these pipelines exist, so a prelude fixed at startup would never contain a graph. Keyed on
// revision, not content, so the common no-graphs case rebuilds nothing.
const char* voxiShaderPrelude() {
    static std::string s;   // owned by a static, because the caller borrows it
    static u64 built = ~0ull;
    // Both revisions, because either can move independently: a project can register a material
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

    // Occlusion-aware fog: airVisPso_ is settled by createPipelines() above (device shader model,
    // DXC availability), so this is the first point airVisWanted() can be answered honestly --
    // upgrades the t17/u16 placeholder to the real
    // kAirVisResolution^3 texture if setting and device allow it. Failure here isn't fatal: the
    // placeholder stays bound and fog stays unoccluded ("wrong but running" beats a dead renderer).
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
    // airVisPlaceholder_ is created unconditionally in createVoxelVolume(), so it belongs in this
    // "always required" chain -- unlike airVisPso_/airVisTex_, legitimately 0 below SM 6.0 or with
    // voxi.fogOcclusion off.
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
              // shadow is per-draw/instanced -- a zero in the second slot silently falls back to one
              // draw per instance, a 3% frame-time difference that cost an hour of misattributed
              // measurement to learn about.
              shadowPso_, shadowInstancedPso_, voxelPso_, voxelMsPso_, clearPso_, resolvePso_, mipPso_, debugPso_,
              scenePso_, sceneMsPso_, sceneRtPso_, sceneMsRtPso_,
              // Blended family: all four legitimately zero on a device too old for AlphaBlend, most
              // commonly zero because vsMain/psVoxi never compiled (scenePso_ already zero then).
              sceneBlendedPso_, sceneMsBlendedPso_, sceneRtBlendedPso_, sceneMsRtBlendedPso_,
              // airVisTex_ reads 0 whenever fogOcclusion is off or airVisPso_ failed to compile --
              // this is the FINAL state, after ensureAirVis() ran above.
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
              // sceneBlendedPso_ is the fallback for every other blended combination, so its absence
              // means "no translucent material draws through Voxi this run", not one lost twin.
              sceneBlendedPso_ ? "built" : "absent",
              // Same reasoning: sceneGbufPso_ is the fallback for every G-buffer combination, so its
              // absence means "no pipeline can write a G-buffer this run".
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
                                        rayDrivenPso_,
                                        sceneGbufPso_, sceneMsGbufPso_, sceneRtGbufPso_, sceneMsRtGbufPso_,
                                        sceneBlendedGbufPso_, sceneMsBlendedGbufPso_,
                                        sceneRtBlendedGbufPso_, sceneMsRtBlendedGbufPso_,
                                        scenePsoPrepassedGbuf_, sceneRtPsoPrepassedGbuf_,
                                        // Every handle in this array must be destroyed here, not just
                                        // zeroed below -- rayDrivenPso_, rayDrivenTexPso_ and
                                        // sceneRtBlendedTexPso_ were each missed at some point and
                                        // leaked, because zeroing a handle LOOKS like releasing it.
                                        rayDrivenTexPso_, sceneRtBlendedTexPso_,
                                        rayDrivenTexGbufPso_,
                                        rayDrivenGbufPso_,
                                        // Staged ray-driven passes: the compute pipelines plus Stage
                                        // B's two AVER_RD_SPLIT graphics twins.
                                        rdVisCsPso_, rdShadowCsPso_, rdGiCsPso_, rdGiCbCsPso_,
                                        rdSkyOccCsPso_, rdReflCsPso_,
                                        rayDrivenSplitTexPso_, rayDrivenSplitTexGbufPso_,
                                        // Sub-stage splits (Settings::rayDrivenShadowTiles/rayDrivenGiSplit).
                                        rdShadowProbeCsPso_, rdShadowTiledCsPso_,
                                        rdGiTraceCsPso_, rdGiTraceCbCsPso_,
                                        rdGiSplitCsPso_, rdGiSplitCbCsPso_,
                                        // Sub-stage C (Settings::rayDrivenReflSplit).
                                        rdReflSplitCsPso_, rdReflFilterCsPso_,
                                        // Local lights (lamps): CSRdLocalLights.
                                        rdLocalLightsCsPso_,
                                        // airVisPso_: created alongside mipPso_ in createPipelines(),
                                        // not touched by createScenePipelines()'s hot-reload, so it
                                        // belongs in this list rather than that function's stale[].
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
    // airVisTex_/airVisPlaceholder_: not owned by any binding-set slot's lifetime, same reason
    // voxelAccumPlaceholder_ above needs its own explicit destroy.
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
    // Six history-pair textures this function used to never free (~225 MB at 3532x1987 for four of
    // them, per ensureShadowHistory) -- only released when ray tracing switched OFF at runtime, not
    // on the way out, since member-init clears the handles so nothing looked wrong. Matters beyond
    // process exit: the editor can shut this feature down and re-init it while running, so this was
    // a per-cycle leak of a third of a gigabyte.
    for (rhi::TextureHandle& t : rtShadowHist_) { if (t) res_->destroyTexture(t); t = 0; }
    for (rhi::TextureHandle& t : rtReflHist_)   { if (t) res_->destroyTexture(t); t = 0; }
    for (rhi::TextureHandle& t : rtAoHist_)     { if (t) res_->destroyTexture(t); t = 0; }
    if (rtAoHitDist_) { res_->destroyTexture(rtAoHitDist_); rtAoHitDist_ = 0; }
    for (rhi::TextureHandle& t : giSurfPosHist_) { if (t) res_->destroyTexture(t); t = 0; }
    for (rhi::TextureHandle& t : giSurfNrmHist_) { if (t) res_->destroyTexture(t); t = 0; }
    for (rhi::TextureHandle& t : giVisHist_)     { if (t) res_->destroyTexture(t); t = 0; }
    // Local lights (lamps): the history pair, its placeholder, the light-list ring and its placeholder.
    // bindings_ is already gone (top of this function), so nothing else names any of them.
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
    // Staged ray-driven passes: rdVisBuf_/rdSunVisTex_/rdGiTex_/rdAoTex_/rdReflTex_ and their
    // placeholders, same "needs its own explicit destroy" reasoning as voxelAccumPlaceholder_ above.
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
    // Sub-stage splits' buffers and placeholders (Settings::rayDrivenShadowTiles/rayDrivenGiSplit),
    // same reasoning as the rest of this block.
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
    // Invalidated for the same reason blas_/tlas_ are cleared above: init() rebuilds both from
    // nothing (a device reset, a resize, or the editor cycling the feature), so a key matching
    // pre-shutdown drawsPrev_ must not be trusted post-shutdown -- none
    // of rtAccelSnapshotUnchanged()'s own force-rebuild conditions would otherwise catch "the TLAS
    // was thrown away without a single draw changing".
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
    // All captured BEFORE the assignment below: each resource is allocated on its OFF->on edge and
    // released on the on->OFF edge, and setSettings is the only place any of these edges is visible
    // -- onRenderTargetsChanged only ever sees a resize, not a settings change, so waiting for one
    // would leave the resource allocated (or missing) -- a quarter of a gigabyte in the ray-traced
    // history buffers' case -- until the window happened to resize. Each flag
    // below can flip independently of the others (e.g. a tier moving Medium->High traces AO without
    // changing rayTracingWanted(); giMode/fogOcclusion/rayDrivenStages/localLights each have their
    // own on/off unrelated to the ray-tracing tier), so each needs its own edge capture here.
    const bool wasWanted = rayTracingWanted();
    const bool wasAoWanted = aoHistoryWanted();
    const bool wasGiRestirWanted = giRestirWanted();
    const bool wasVisWanted = giVisHistWanted();
    const bool wasLocalHistWanted = rdLocalHistWanted();
    const u32 wasVis = giRestirVisibility_;
    const bool wasRdStagedResourcesWanted = rdStagedResourcesWanted();
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
    // Clamped defensively even though Settings::clamp() (Voxi.cpp) already does the same thing --
    // std::min lands a typo on Full(3), the corrected value, never on 0/NoRay, which is what an
    // unclamped out-of-range value would silently decode as through the shader's `& 3u` mask (2.9's
    // bit table) if a caller ever reached this field some other way.
    giRestirVisibility_ = std::min(s.giRestirVisibility, 3u);
    // Same defensive clamp repeat, for givis::packAmbientW's `& 15u` mask.
    giRestirSpatialSamples_ = std::min(s.giRestirSpatialSamples, 15u);
    giRestirMaxHistory_     = std::min(s.giRestirMaxHistory, 63u);
    ptBounces_       = s.ptBounces;
    // LATCHED, not assigned: the pipelines this shapes are built once, so a later change disagreeing
    // with the shaders already compiled is worse silently ignored than surfaced once.
    if (!layeredBsdfLatched_) {
        layeredBsdf_ = (s.layeredBsdf != Quality::Off);
        layeredBsdfLatched_ = true;
    } else if (layeredBsdf_ != (s.layeredBsdf != Quality::Off) && !layeredBsdfWarned_) {
        // Once -- the condition doesn't clear itself, so without this guard the warning would fire on
        // every setSettings for the rest of the session.
        layeredBsdfWarned_ = true;
        AVER_WARN("[Voxi] layeredBsdf changed after the pipelines were built; it takes effect on the "
                  "next project load. The shaders compiled for this session are unchanged. "
                  "(said once)");
    }
    setGiUpdateInterval(s.giUpdateInterval);

    // A genuine visibility-mode change resets GI reservoir history, NRD history and the half-res
    // visibility history together -- all three carry a stale answer once visMode changes (2.8's
    // precedence rule; same reasoning as setLightingLegacyBits' R0/R2/R3 branch below). Gated on
    // giRestirWanted(): nothing live to invalidate otherwise. Not resetGiHistory()/resetNrdHistory()
    // themselves, which are named console commands with their own log line -- this is a
    // setSettings-driven edge, so it logs its own.
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

    // Same guarded-on-a-real-size edge check as above, on rdStagedResourcesWanted()'s own edge.
    if (rdStagedResourcesWanted() != wasRdStagedResourcesWanted && rtHistWantW_ && rtHistWantH_)
        if (!ensureRdStagedResources(rtHistWantW_, rtHistWantH_))
            AVER_ERROR("[Voxi] staged ray-driven resources could not follow a settings change at {}x{}",
                       rtHistWantW_, rtHistWantH_);

    // airVisWanted()'s own edge -- no size to guard on since airVisTex_ is a fixed
    // kAirVisResolution^3 regardless of viewport; still guarded on bindings_ existing
    // (ensureAirVis() re-checks, since init() may not have run yet on the first setSettings).
    if (airVisWanted() != wasAirVisWanted)
        if (!ensureAirVis())
            AVER_ERROR("[Voxi] air sky-visibility volume could not follow a voxi.fogOcclusion change");

    // REBLUR history/prepass tuning as LIVE dials: re-issuing setReblurTuning every setSettings call
    // (not just once at NRD creation) is what makes a console-set
    // voxi.reblur{DiffusePrepassBlurRadius,MaxAccumulatedFrameNum,MaxStabilizedFrameNum} take effect
    // next frame without tearing NRD's accumulated history down. Guarded on nrd_.valid() only to
    // skip the redundant call before NRD exists -- applyReblurTuning()/setReblurTuning() would no-op
    // harmlessly either way.
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

// See the header's own comment. No log: reasserted from the console/--gi-vis-path-view slot every
// frame regardless of whether the user touched it, so a log line here would be noise.
void VoxiRenderer::setGiVisPathView(bool on) { giVisPathView_ = on; }

// See the header for what M5 prices. Guarded on an actual change (SandboxApp reasserts this from the
// console/--blended-gi slot every frame) -- unlike setNrdLegacyCamera/setLightingLegacyBits below,
// this carries no history-reset cost, so a plain reassign-and-log is enough.
void VoxiRenderer::setBlendedGiCone(bool on) {
    if (on == blendedGiCone_) return;
    blendedGiCone_ = on;
    AVER_INFO("[Voxi] blended-fragment indirect diffuse: {} (M5 pricing switch, console/--blended-gi)",
              on ? "voxel cone gather" : "ReSTIR (default)");
}

// See the header for what this switch reinstates and why. Guarded on an actual change because
// SandboxApp reasserts this from the console slot every frame -- without the guard, every frame
// would force an NRD history reset and no denoiser history would ever accumulate.
void VoxiRenderer::setNrdLegacyCamera(bool on) {
    if (on == nrdLegacyCamera_) return;
    nrdLegacyCamera_ = on;
    // The two encodings' previous cameras differ in shape (legacy = last frame's combined viewProj;
    // fixed = a factorised pair) -- reprojecting against the OTHER encoding's camera would silently
    // blend the two rather than compare them, so history and the previous-camera latch reset together.
    nrd_.forceHistoryReset();
    nrdPrevCameraValid_ = false;
    nrdPrev2CameraValid_ = false;
    AVER_INFO("[NRD] camera encoding switched to {} by console command (voxi.nrdLegacyCamera); "
              "history reset on the next frame so the two encodings are never blended together.",
              on ? "the OLD, WRONG pre-fix encoding (comparison only)" : "the fixed encoding");
}

// See the header for the bit table and the contrast-fix plan's CONTRACT section 2 for where each bit
// is read in the shaders. Guarded on an actual change, same reason as setNrdLegacyCamera above.
void VoxiRenderer::setLightingLegacyBits(u32 bits) {
    if (bits == lightingLegacyBits_) return;
    const u32 changed = bits ^ lightingLegacyBits_;
    lightingLegacyBits_ = bits;
    AVER_INFO("[Voxi] lighting legacy bits: ring={} doubleCount={} hitSky={} reuseVis={} cones={} "
              "blendedHistory={} nrdReadback={} (console)",
              (bits & 1u) ? 1 : 0, (bits & 2u) ? 1 : 0, (bits & 4u) ? 1 : 0, (bits & 8u) ? 1 : 0,
              (bits & 16u) ? 1 : 0, (bits & 32u) ? 1 : 0, (bits & 64u) ? 1 : 0);
    // Bit 64 (legacyNrdReadback) needs no reset: changes only where the shader reads NRD's output.
    // R0/R2/R3 (bits 1,4,8): ReSTIR samples/adds/reuses differently under these, so history
    // accumulated on one side of the flip is stale (same reasoning as resetGiHistory's own comment).
    if (changed & (1u | 4u | 8u)) { resetGiHistory(); resetNrdHistory(); }
    // R0 (bit 1) also reshapes the sky-occlusion ray (voxi_rt.hlsli); its AO history tracks
    // separately from the GI reservoir's.
    if (changed & 1u) resetAoHistory();
    // Bit 32 (W6/M5): whether a blended fragment writes shadow/reflection/AO histories at all
    // (PSMainVoxi's gAverHistoryWrite gate) -- unlike bits 1/4/8 this changes what those histories
    // THEMSELVES hold, not just the GI reservoir/NRD, so RT history rides along too.
    if (changed & 32u) { resetGiHistory(); resetNrdHistory(); resetRtHistory(); }
}

// See the header's own comment for the shape every one of these five shares: flip an existing
// validity flag, touch no texture/buffer handle, log through AVER_INFO so the bisection survives even
// if the user closes the editor before reading the console's own scrollback.
void VoxiRenderer::resetGiHistory(bool quiet) {
    giHistValid_ = false;
    // The half-res visibility pair rides along: while giRestirVisibility_ is HalfResolution, a
    // legacy-bit flip (or any other caller of this command) forcing "no ray" leaves
    // f2Observed/f3Observed false and nothing refreshes the pair's EMA, so only this flag stops
    // giVisReconstruct's reprojection from trusting stale data.
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

// ALIAS OF resetRtHistory, TODAY: rtSkyOcclusionTemporal's reprojection (voxi_rt.hlsli) is gated by
// the same gRtHistParams.y that mirrors rtHistValid_ (set in beginShadowHistory); cb_.rtDenoiseParams.w
// answers a different question ("is the AO pair bound", not "is its history valid") -- gating the
// pass on it would also skip the gAoHistOut write, leaving the poisoned texel un-refreshed instead of
// cleared, so making it independent would need new plumbing this task didn't add. Ships as an honest
// alias with its own log line so the Output Log still shows which command was typed.
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
              "p90 {:.3f} ms -- WHOLE frame, CPU, {} of max {} sun ray(s)/pixel, rt {}",
              when, static_cast<u32>(n), med, sum / static_cast<f32>(n), s.front(), p90,
              rtShadowRaysUsed_, rtShadowRays_, rtActive_ ? "active" : "off");
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

// Mixes a mesh handle's bits before submit()'s cache (meshSubmitCacheSlot) masks them into a slot
// index. Same shape/finalizer as GameRender.cpp's own mixMeshId (MurmurHash3's fmix64), duplicated
// rather than shared since render.voxi sits below Runtime/game in this engine's dependency direction.
// NOT the same justification though: GameRender.cpp mixes because its key is an fnv1a64 hash of an
// asset path, whose low bits it argues are weak; `rhi::MeshHandle` here is a small, dense,
// monotonically increasing id (both D3D12Device and VulkanDevice hand out the next index and never
// recycle a destroyed one's slot -- D3D12Device.cpp's destroyMesh comment: "the slot itself is KEPT")
// that a plain mask already distributes evenly (see aver-asset-id-spaces), so no load-bearing case
// for mixing has been found here -- kept only for shape-parity, at the cost of two multiplies/three
// shifts.
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
// Also clears submit()'s per-mesh cache (meshSubmitCache_), the one invalidation point this class
// can actually guarantee (a depthProxyFn_/depthProxyUser_ change is handled separately at
// setDepthProxy() itself; a mesh destroy/re-upload has no callback to hook at all). Clearing every
// frame bounds the cache's staleness to at most one frame -- the SAME one frame of staleness the
// swap two lines below already accepts for the whole draw list -- rather than opening an unbounded
// second staleness window where a re-uploaded mesh keeps answering with its first upload's bounds
// until something else forces a clear (meshSubmitCache_'s header names this as the failure mode to
// avoid: entities vanishing from a shadow cascade, misdiagnosed as a culling bug). A few hundred
// default-constructed slots reset per frame is a rounding error next to the up to 16,000 submit()
// calls this cache exists to shrink.
void VoxiRenderer::beginScene() {
    drawsPrev_.swap(draws_);
    draws_.clear();
    meshSubmitCache_ = {};
}

// See the header's setDepthProxy comment for why this is no longer a one-line inline setter. Guarded
// on an actual change (the reassert idiom setBlendedGiCone/setShadowRays use elsewhere) so a future
// caller reasserting the same fn/user every frame wouldn't pay a full cache clear for a no-op.
void VoxiRenderer::setDepthProxy(DepthProxyFn fn, void* user) {
    if (fn == depthProxyFn_ && user == depthProxyUser_) return;
    depthProxyFn_ = fn;
    depthProxyUser_ = user;
    // A cached depthProxyMesh answer is only correct for the fn/user pair that produced it, and a
    // slot has no field remembering which pair that was -- dropping the whole table is the only way
    // to guarantee the next submit() asks the new pair instead of reusing the old verdict.
    meshSubmitCache_ = {};
}

// Finds mesh's slot in meshSubmitCache_, evicting a different mesh's leftover answers first -- same
// hazard and fix as GameRender.cpp's findMeshLookupSlot (a stale slot could misattribute one mesh's
// depth-proxy/bounds answer to another). Aggregate-init `MeshSubmitCacheSlot{mesh}` sets `.mesh` and
// leaves every other field at its in-class default, so a fresh occupant reads as "never touched".
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
    // branches (depth, raster, and the culled/hidden direct route -- see this function's own header
    // comment on why culled entities arrive here too), so the call site has no single parent phase to
    // `.to()` into; CpuNest is built for exactly that (see CpuNest's own comment). Placed after both
    // early returns above so a no-op/refusal doesn't inflate VoxiSubmit's number with calls that touch
    // none of the work below.
    CpuNest voxiSubmitTiming(CpuSpan::VoxiSubmit);
    Draw d;
    d.mesh = mesh;
    // d.depthMesh and the meshBounds lookup below are both functions of mesh alone, resolved ONCE
    // here rather than per pass/per entity (six depth passes would otherwise repeat the same lookup)
    // -- previously re-resolved in full on every one of up to 16,000 calls/frame though a scene rarely
    // has more than a few hundred distinct meshes, the redundancy a7ff716d named and the same shape
    // commit 3a9dc985 found in the RT geometry table's mesh dedup (64.2ms -> 5.4ms). meshSubmitCacheSlot()
    // resolves each question once per distinct mesh id seen since the last beginScene() clear; see
    // meshSubmitCache_'s header for its 256-slot deviation from 80730751's precedent. meshSubmitCacheEnabled_
    // (default true) is the runtime A/B switch: false reproduces the two calls below unchanged.
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

    // World-space bounding sphere for shadowPass's per-cascade cull. d.boundsRadius defaults to -1
    // (unknown) for a backend with no bounds to give.
    //
    // Only the LOCAL-space half is memoised (localCentre/localRadius, and whether meshBounds found
    // them) -- never d.boundsCentre/d.boundsRadius, which transform that local answer through THIS
    // entity's own `world` matrix right after. `world` differs per entity even when `mesh` doesn't,
    // so the transform stays computed on every call with no exception; caching the transformed result
    // would hand every instance of a mesh the first instance's world-space bounds -- see
    // meshSubmitCache_'s header for the failure this design avoids.
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

// The IRenderFeature entry point every draw arrives through (`blended` per RHIResources.hpp's
// IRenderFeature::submitDraw). A blended draw is filtered here explicitly: unfiltered, it would
// voxelise glass as an opaque light-blocker, cast a solid black shadow, and read as opaque to
// reflection rays in the TLAS. See createScenePipelines()'s "10b" comment for what glass gets
// despite this (shadow cast ON it, GI landing on it) vs. what it doesn't (casting its own shadow,
// appearing in a reflection, injecting light).
void VoxiRenderer::submitDraw(rhi::MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                              f32 metallic, f32 roughness, rhi::BindingSetHandle drawBinding,
                              const void* drawConstants, u32 drawConstantBytes, bool blended) {
    if (blended) {
        // Translucent lane: TLAS marks it non-opaque so a shadow ray can attenuate through it, still
        // excluded from cascade/GI shadow map/voxelisation since all three are depth-only (no
        // transmittance channel) -- an RT-path feature by construction, not by choice. See "10b" for
        // the full reasoning; the census below is kept because the exclusion is still worth reporting.
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
    // A material graph appeared since these pipelines were built: without this, a graph would get a
    // valid id but no pixel-shader arm to shade it (silently the stock material). Checked here via a
    // revision pull, not pushed from the loader, so a future third caller can't forget to push it.
    if (scenePipelineGraphRev_ != pbr::materialGraphs().revision()) {
        scenePipelineGraphRev_ = pbr::materialGraphs().revision();
        AVER_INFO("[Voxi] rebuilding scene pipelines for {} material graph(s)",
                  pbr::materialGraphs().count());
        if (!createScenePipelines(dev_->sampleCount(), dev_->backbufferFormat(), dev_->depthFormat()))
            AVER_ERROR("[Voxi] scene pipelines could not be rebuilt for the material graphs");
    }
    // A shader file changed on disk. Checked here because it's the only safe point: rebuilding GPU
    // objects from a file-watcher thread would free things a command list is mid-recording (the
    // failure that removed the device at Present when a stored render scale rebuilt targets
    // mid-frame, D3D12Device::setRenderScale). The watcher only bumps an integer; every GPU
    // consequence happens here.
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
    // unmoved handle is churn for nothing. A 0 handle MUST clear the slot, not skip it: getting that
    // wrong crashed the GPU on every window resize, since D3D12Device::resize destroys the backdrop
    // without recreating it in the same call, leaving t10 pointing at a freed texture until targets
    // rebuild (GetDimensions() is not a validity test on a dangling descriptor). clearSrv writes the
    // null view so GetDimensions() reads 0 and the shader takes its no-backdrop path.
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
    // The editor view mode, composed here (not in beginShadowHistory, which returns early unless
    // shadow history is active, so the field never got written and PSRayDriven read unlit as
    // permanently off) -- MEASURED via --unlit differing from lit by 2.79% of pixels at unchanged
    // mean luminance (raster chrome moving, not the scene), which is what caught the bug. prePass
    // runs every frame before cb_ is uploaded, which is what composing it here needs.
    //
    // viewDebug_ takes priority over unlit_, not OR'd: the editor keeps the two dropdown families
    // mutually exclusive (SandboxViewport.cpp) and both pack into this one float. See ViewDebug's
    // comment (VoxiRenderer.hpp) for the 2-5 values and PSRayDriven (voxi.hlsl) for the decode.
    cb_.viewParams[0] = viewDebug_ != ViewDebug::None ? static_cast<f32>(viewDebug_) : (unlit_ ? 1.0f : 0.0f);
    // Live GI radiance ceiling (AVER_VOX_MAXRAD, voxi.hlsl/voxi_gi.hlsli) -- see
    // Settings::giRadianceCeiling for what this caps, and FrameConstants::viewParams's comment for
    // why .y is safe to repurpose.
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
    // Lighting-contrast legacy bitmask (setLightingLegacyBits has the bit table) -- sent unconditionally
    // every frame, same as ambientParams[1] above: an all-zero (never-written) block reads as "every
    // fix is live" to any shader reading gAmbientParams.z.
    cb_.ambientParams[2] = static_cast<f32>(lightingLegacyBits_);
    // Refraction rides giParams' spare .yzw (only .x was used) at no layout cost -- but this row is
    // mirrored by hand in more than one place; see MaterialConstants' note on what a silent offset
    // mistake costs. A fourth refraction knob needs its own row and a mirror update everywhere, not
    // another borrow.
    cb_.giParams[1]    = static_cast<f32>(settings_.refractionMode);
    cb_.giParams[2]    = settings_.refractionStrength;
    cb_.giParams[3]    = settings_.refractionEdgeFade;
    cb_.voxelParams[2] = settings_.giMaxDistance;
    // Gates the cone trace -- see setConeTraceEnabled's own comment for why this is ANDed in here
    // rather than folded into giEnabled(): it lets an A/B measurement turn the shader-side read off
    // without changing whether the passes below actually run.
    cb_.voxelParams[3] = (giEnabled() && !debugView_ && coneTraceEnabled_) ? 1.0f : 0.0f;

    // Staged ray-driven bit-field toggles -- see FrameConstants::giShadowParams for the bit table.
    // Packed here unconditionally every frame (fitGiShadow() no longer touches [3]), so a frame that
    // skips the GI rebuild gate still carries the bits, same reasoning as ambientParams[1]/[2] above.
    // Written FROM SCRATCH, not merged -- gives bit 16 (blendedReuseStagedLighting) its self-clearing
    // property, since recordStagedRayDriven ORs it in later this same frame, only when reached.
    cb_.giShadowParams[3] = static_cast<f32>((settings_.rtSecondaryShadowOpaque ? 1u : 0u) |
                                              (settings_.rtSkyOcclusionHalfRate ? 2u : 0u) |
                                              (settings_.rtReflectionHalfRate  ? 4u : 0u) |
                                              (settings_.rtGiHitShadowMap      ? 8u : 0u));

    // Makes every pushMarker below a CHILD of "Voxi GI update" instead of five unrelated top-level
    // siblings (acceleration structures, cascades, GI-only shadow box, voxelise, mip filter). This
    // scope's own exclusive time is endShadowHistory() (no marker of its own, pure CPU bookkeeping)
    // plus whatever else runs unmarked (manageInjectionAccumulator, gate logic) -- beginShadowHistory()
    // opens its own child span ("Voxi shadow history"), and the nrd_.record() call inside it opens a
    // grandchild of that ("Voxi NRD denoise"). Opened here, not at the top, because everything above
    // is CPU-only bookkeeping.
    rhi::ScopedGpuStat voxiGpuStat(ctx, "Voxi GI update");
    // W12: recreate/free the injection accumulator before anything else in this scope binds
    // bindings_. shadowPass (below) is the first thing that binds it, and Vulkan's ringed binding
    // sets forbid writing a set after it's already bound this frame -- the earlier t10 backdrop
    // setSrv/clearSrv above is a write too, but it runs before any binding set is bound this frame,
    // same as this call.
    manageInjectionAccumulator(ctx);
    buildAccelerationStructures(ctx);   // sets rtActive_, which beginShadowHistory reads
    // After the TLAS decision (a lamp's shadow ray needs one), before shadowPass() first binds
    // bindings_ -- t18 is written here, and Vulkan forbids writing a set it has already bound this
    // frame.
    buildLocalLights();
    // Gated on rtActive_, not the setting alone: the shader traces against the same acceleration
    // structure the shadow ray uses, and there is none on a frame with no TLAS -- publishing a
    // non-zero count then would have every pixel trace into nothing and read "sky visible
    // everywhere", brighter than the cone estimate it replaced. Zero keeps the cone gather's own
    // occlusion as fallback. Must read THIS frame's rtActive_ (set by buildAccelerationStructures
    // just above), not last frame's -- a frame where ray tracing turns off or the TLAS goes briefly
    // empty must not publish a stale nonzero count. Moved here by the build/shader-safety review
    // (audit finding F4), which had this reading last frame's rtActive_; cb_ itself isn't uploaded
    // (setConstantBuffer) until deep inside shadowPass()/scenePass(), well below this.
    cb_.ambientParams[0] = rtActive_ ? static_cast<f32>(std::min(settings_.giSkyOcclusionRays,
                                                                 kMaxShadowRays))
                                     : 0.0f;
    beginShadowHistory(ctx);
    shadowPass(ctx);                    // fitCascades(), called from here, fills curViewProj_
    if (giEnabled()) {
        // Amortised revoxelisation: rtFrameIndex_ was incremented above, so (rtFrameIndex_-1) % N
        // lands on 0 for frame 1, guaranteeing the volume builds before anything samples it. Frames
        // in between skip voxelizePass/filterMips; the cone trace samples the last-built volume.
        // giUpdateInterval_ == 1 (default) takes this every frame, as before this knob existed.
        if (giUpdateInterval_ <= 1 || ((rtFrameIndex_ - 1) % giUpdateInterval_) == 0) {
            // The rebuild gate: below recomputes a function of (draw list, sun, volume placement); if
            // none moved, the volume texture already holds the answer (see giSnapshotUnchanged).
            //
            // Convergence ticks exist because PSVoxel re-emits the previous bake, so light
            // accumulates one bounce per REBUILD, not per frame -- the gate alone skips 96-98% of
            // ticks on a still camera, which starved the feedback term of a second bounce. MEASURED:
            // adding the feedback term alone moved the isolated GI contribution from 52.1 to 37.0 --
            // it removed the false unoccluded sky but put nothing back. So a rebuild that changed
            // something schedules a few more ticks; the series converges geometrically (albedo < 1)
            // and the gate goes quiet again.
            const bool converging = giConvergeTicks_ > 0;
            // giForceRebuild_ (M4, --gi-force-rebuild / voxi.giForceRebuild) is ANDed in LAST, after
            // giSnapshotUnchanged(): that call has logging side effects (giGateWhyMask_, the
            // rejection-reason counters below it) describing the real gate outcome, which a forced
            // rebuild must not silence.
            const bool gateUnchanged = giSnapshotUnchanged();
            if (gateUnchanged && !converging && !giForceRebuild_) {
                ++giSkipped_;
                // Only while nothing is converging -- a bake still settling (giConvergeTicks_ > 0) is
                // busy work, not an idle accumulator waiting to be freed.
                if (giConvergeTicks_ == 0) ++giQuietTicks_;
                // Settled for kGiCacheDwellTicks: the one moment worth caching.
                if (giCacheSettlePending_ && giQuietTicks_ >= kGiCacheDwellTicks && giCacheScheduleDump(ctx))
                    giCacheSettlePending_ = false;
            } else {
                giQuietTicks_ = 0;
                // The accumulator may have been freed since the last rebuild. manageInjectionAccumulator()
                // (earlier this prePass) only learns a rebuild needs it from giAccumWanted_, which this
                // tick is the first to set -- recreate lands next prePass. So a tick finding it missing
                // asks for it back and touches nothing else (not giRebuilt_, giConvergeTicks_,
                // takeGiSnapshot()), so giSnapshotUnchanged() retries the rebuild next tick instead of
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
                    // the 3-in-4 frames injection is skipped would be pointless (the other half of the
                    // saving is that the camera cascades no longer carry the volume at all -- fitCascades).
                    giShadowPass(ctx);
                    // Cache read sits exactly here, between "gate says rebuild" and the rebuild
                    // itself: inputs are settled, work hasn't started. A hit fills the volume from
                    // disk and skips the two passes below; a miss falls through and bakes. The write
                    // isn't here -- a bake only marks the volume for caching; the skip branch above
                    // copies it out once settled (giCacheSettlePending_).
                    //
                    // giForceRebuild_ short-circuits the cache on BOTH sides: a forced tick exists to
                    // measure a bake, so it must pay voxelizePass+filterMips every tick and never hit
                    // or write the cache.
                    if (giForceRebuild_ || !giCacheRestore(ctx)) {
                        voxelizePass(ctx);
                        filterMips(ctx);
                        // Whether everything since the last settled volume was the cloud clock alone
                        // (giRebuildCloudOnly_ describes only THIS evaluation, and is false again by
                        // the quiet tick that writes). A pure convergence tick changes nothing.
                        if (!gateUnchanged)
                            giCacheSettleCloudOnly_ =
                                (giCacheSettlePending_ ? giCacheSettleCloudOnly_ : true) && giRebuildCloudOnly_;
                        giCacheSettlePending_ = true;
                    }
                }
            }
            giCacheTick();
            // Reported as a ratio ("GI rebuilt 3 of 170 ticks") since 100% rebuilt means the gate
            // saves nothing. Widening report intervals, not a fixed tick count: a fixed 64-tick
            // report lands mid-stream on a streamed level (draw list changes every tick, so the gate
            // can't match by construction) and always prints "0 skipped of 64" -- a measurement whose
            // window excludes the case it measures (the first version logged once at a fixed 64 ticks,
            // ~frame 256 at the default interval of 4). Also prints the since-last-report ratio beside
            // the lifetime one, so steady state isn't averaged away by the loading phase.
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
    // Air sky-visibility refresh, every frame, decoupled from the GI rebuild gate above: sky
    // visibility changes only when geometry does, while a full CSAirVis measured ~10ms (at 48^3) --
    // so each frame refreshes one slab of kAirVisSlabLayers z-layers round-robin, turning the whole
    // volume over every kAirVisResolution/kAirVisSlabLayers frames at flat, small cost. A dirty
    // volume (just created, or GI just came back on) is filled whole first so no frame reads an
    // unwritten texel.
    //
    // voxelTex_ rests in ShaderResource between frames; a compute read needs NonPixelShaderResource,
    // hence the round trip around the dispatch.
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
            // The placeholder stays bound and giAccumWanted_ stays true, so this retries every tick
            // rather than silently giving up on GI.
        }
    } else if (giFreeAccumulator_ && voxelAccumTex_ && giEnabled() && !giForceRebuild_ &&
              giConvergeTicks_ == 0 && giQuietTicks_ >= kGiAccumulatorQuietTicks) {
        // (b) FREE. Each guard is a reason not to free: !giForceRebuild_ (a forced tick needs the
        // accumulator every tick) and giConvergeTicks_ == 0 (a still-converging bake is busy
        // regardless of giQuietTicks_ -- the two counters are kept separate, not one resetting the other).
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
            // Rebind before destroy, always (aver-view-outlives-its-buffer.md): a binding set left
            // pointing at voxelAccumTex_ past this point would describe a destroyed resource.
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

    // ---- The unchanged gate (Settings::rtSkipUnchangedTlas) ----
    // See rtAccelSnapshotUnchanged() for what "unchanged" checks. A match means tlas_,
    // rtInstanceData_ and every SRV bound to them (slots 2/3/4/5/9, set the last time the per-draw
    // loop actually ran) are still correct, so the whole
    // body below is skipped -- except what's NOT a function of drawsPrev_ and is read every frame
    // regardless: rtActive_, cb_.shadowParams[2], cb_.rtParams (updateRtParamsPerFrame(), factored
    // out for this reuse), and cb_.rtParams[3] from rtGeometryReady_ rather than a fresh
    // buildGeometryTable() call (one of the things being skipped).
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
    // tlasInstScratch_/matConstantsScratch_: hoisted into members (VoxiRenderer.hpp) rather than
    // locals reallocated from empty every build. .clear() keeps the underlying storage, so a
    // steady-state scene reuses the same allocation; only a build growing past the previous
    // high-water mark reallocates. Cleared here, alongside the rest of this build's per-frame state,
    // so an early-returning build leaves nothing half-populated -- the next build clears both before
    // reading either, not because this one avoided touching them.
    tlasInstScratch_.clear();
    matConstantsScratch_.clear();
    tlasInstScratch_.reserve(drawsPrev_.size());
    rtInstanceData_.clear();
    rtInstanceMesh_.clear();
    rtInstanceMatKey_.clear();
    rebuiltThisFrame_.clear();
    rtInstancePrevWorld_.clear();
    u32 firstBuilds = 0;

    // CPU-only cost of walking drawsPrev_ and filling rtInstanceData_/rtInstanceMesh_/
    // rtInstanceMatKey_, measured from just before this population pass to the loop's closing brace --
    // NOT the BLAS/TLAS GPU recording after it (already timed by gpuStat above,
    // and ctx.buildBlas/buildTlas only record commands, they don't wait on the GPU). See
    // lastAccelBuildCpuMs().
    const auto accelBuildCpuStart = std::chrono::steady_clock::now();

    // ---- previous-transform tracking, pass 1: THIS build's population per (mesh, drawBinding) ----
    // See prevTransformGroupKey() for the whole scheme. Counted over the full drawsPrev_ list, not
    // just draws with a usable BLAS, so a transient BLAS cache miss (a brand-new mesh's first frame)
    // never looks like a population change -- "has geometry to trace yet" and "did the submitted set
    // change" are independent.
    if constexpr (kTrackPrevTransforms) {
        prevGroupCountThisBuild_.clear();
        prevGroupOrdinal_.clear();
        nextTransformByKey_.clear();
        for (const Draw& d : drawsPrev_)
            ++prevGroupCountThisBuild_[prevTransformGroupKey(d.mesh, d.matSet)];
    }

    for (const Draw& d : drawsPrev_) {
        // ---- previous-transform tracking, pass 2: this draw's ordinal within its group ----
        // Incremented for every draw, survivor or not, so ordinal numbering matches what pass 1
        // counted over. Only the STORE below (after the BLAS check) is conditional on survival, so
        // rtInstancePrevWorld_ lines up index-for-index with rtInstanceData_.
        u64 prevGroupKey = 0, prevInstKey = 0;
        if constexpr (kTrackPrevTransforms) {
            prevGroupKey = prevTransformGroupKey(d.mesh, d.matSet);
            const u32 prevOrdinal = prevGroupOrdinal_[prevGroupKey]++;
            prevInstKey = prevGroupKey ^ static_cast<u64>(prevOrdinal);
        }
        prevInstKey *= 1099511628211ull;

        auto it = blas_.find(d.mesh);
        // A cached structure whose mesh was destroyed underneath us -- reachable, not theoretical:
        // the instance list is LAST frame's draws, so a mesh freed between frames is still named
        // here, and handing its structure to the TLAS would have the GPU traverse freed memory.
        // Asking the factory what the BLAS is actually for catches it without every destroyMesh
        // caller having to remember to tell this cache.
        if (it != blas_.end() && it->second && res_->blasMesh(it->second) != d.mesh) {
            blas_.erase(it);
            it = blas_.end();
        }
        if (it == blas_.end()) {
            // createBlas returns 0 for a destroyed mesh; recorded so it isn't retried each frame.
            const rhi::BlasHandle nb = res_->createBlas(d.mesh);
            if (nb) { ctx.buildBlas(nb); ++firstBuilds; }
            it = blas_.emplace(d.mesh, nb).first;
        } else if (it->second && dev_->meshVertexBuffer(d.mesh)) {
            // A mesh whose vertices are written by compute invalidates its own structure every
            // frame -- memoising it (right for static geometry) would give a skinned character a
            // ray-traced shadow frozen at its first pose, invisible in the raster image. The
            // skinning dispatch already left the buffer in GeometryRead, since that feature
            // registers before this one.
            // Rebuilt once per mesh per frame, not once per draw -- previously a mesh drawn twice
            // was rebuilt twice, the second a wasted full PREFER_FAST_TRACE build over identical
            // vertices. The linear scan is over the distinct dynamic meshes in one frame, a handful.
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
        // Three mask lanes: translucency is a material property, hiddenFromOwner is about WHO IS
        // LOOKING, and the two can't co-occur (a translucent draw is diverted by submitDraw's
        // blended branch before the owner-hide walk). Lets a reflection/AO ray opt out via a
        // narrower mask without a structure rebuild. Tested in this order so translucency's answer
        // stays unchanged.
        i.mask = d.translucent      ? kRtMaskTranslucent
               : d.hiddenFromOwner  ? kRtMaskOwnerHidden
                                    : kRtMaskOpaque;
        // ForceNonOpaque only for the translucent lane: createBlas marks every geometry OPAQUE
        // (lets hardware skip any-hit), so without this override a Proceed() loop never sees a
        // candidate and the pane stops the ray like a wall. Set per instance so opaque geometry
        // keeps the fast path.
        i.flags = d.translucent ? rhi::TlasInstanceFlag_ForceNonOpaque : rhi::TlasInstanceFlag_None;
        if (d.translucent) ++tlasTranslucentThisBuild_;
        i.blas = b;
        // Index into rtInstanceData_, which a reflection ray reads for the hit triangle/albedo.
        // Assigned in the same loop that decides which instances survive, so the two lists can't
        // drift -- an id assigned earlier would be wrong for every instance after one whose BLAS
        // failed to build.
        i.instanceId = static_cast<u32>(rtInstanceData_.size()) & rhi::kMaxTlasInstanceId;
        tlasInstScratch_.push_back(i);

        RtInstance ri;
        std::memcpy(ri.objectToWorld, d.world, sizeof(ri.objectToWorld));
        ri.albedo[0] = d.color[0]; ri.albedo[1] = d.color[1]; ri.albedo[2] = d.color[2];
        // From the same Draw, and available here all along.
        ri.metallic = d.metallic;
        ri.roughness = d.roughness;
        // Filled in by buildGeometryTable, which knows where each mesh landed.
        ri.firstIndex = 0;
        ri.firstVertex = 0;
        // Placeholder -- buildMaterialTable() MUST run before buildGeometryTable() (load-bearing
        // ordering, was wrong once -- see the call site below for what it cost). Overwritten once
        // this loop and rtInstanceMatKey_ have run and
        // the full material set is known, since the final index depends on the whole set's sorted order.
        ri.materialIndex = 0;

        // ---- resolving THIS draw's material key and, the first time it is seen, its bytes ----
        // AUTHORED means d.matSet is one of materials_'s own binding sets and not its fallback set.
        // An unauthored draw gets back the identical fallback handle regardless of colour
        // (GameRender.cpp:127-136 and SandboxApp.cpp's matching lambda), so the handle is a useful
        // dedup key -- and d.mat holds real
        // per-material bytes -- only on the authored branch; everything else keys off its own
        // colour/metal/rough (synthMaterialKey, above).
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

        // ---- alpha-masked geometry joins the non-opaque lane ----
        // Previously `translucent` was the only thing that un-opaqued an instance, so a cutout
        // material (foliage, grates, chain-link) traced as the solid sheet its triangles describe --
        // every leaf card its own bounding rectangle in shadows/reflections/GI, and (ray-driven is
        // the default) in primary visibility too, while the raster prepass clipped it correctly.
        // The two paths disagreed and the default was wrong.
        //
        // Patched onto the instance already pushed, because matKey is only known here, several
        // statements after tlasInstScratch_.push_back(i).
        //
        // Not the mask, only the flags: this geometry stays in the OPAQUE lane -- it occludes, casts
        // shadow, is a valid reflection hit, it simply has holes (what any-hit is for). Moving it to
        // kRtMaskTranslucent would hide it from rays that ask for solid surfaces only.
        if (!d.translucent &&
            (matConstantsScratch_.at(matKey).flags & pbr::MaterialFlag_AlphaMask) != 0) {
            tlasInstScratch_.back().flags |= rhi::TlasInstanceFlag_ForceNonOpaque;
            ++tlasAlphaMaskedThisBuild_;
        }

        rtInstanceData_.push_back(ri);
        rtInstanceMesh_.push_back(d.mesh);

        // ---- previous-transform tracking, pass 2 continued: look up, or admit there is none ----
        // Trusted only when this group's population matches last build's -- a change drops the
        // WHOLE group rather than risk an ordinal pointing at a different instance's old transform.
        // "Brand new key" and "population changed" both fall to the else branch: this instance's
        // current transform reported back as "previous" (the honest zero-velocity answer).
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

        // This build's own transform becomes "last build's answer" next time this key is seen --
        // staged into nextTransformByKey_, swapped into prevTransformByKey_ whole once the loop
        // finishes (never written in place), so a dropped instance can't leave a stale transform
        // under a key some later instance could reuse.
        if constexpr (kTrackPrevTransforms) {
            std::array<f32, 16> curWorld;
            std::memcpy(curWorld.data(), d.world, sizeof(curWorld));
            nextTransformByKey_[prevInstKey] = curWorld;
        }
    }

    // Stops where the CPU-only work above ends -- everything below either records GPU commands or is
    // covered by its own accounting. An early return (no ray tracing wanted, empty draw list) never
    // reaches this line, so lastAccelBuildCpuMs_ just keeps its previous value on that path.
    lastAccelBuildCpuMs_ = std::chrono::duration<f64, std::milli>(
        std::chrono::steady_clock::now() - accelBuildCpuStart).count();

    // Commits this build's previous-transform bookkeeping unconditionally, so a build where nothing
    // survived clears both maps to empty rather than leaving a stale generation.
    if constexpr (kTrackPrevTransforms) {
        prevGroupCountLastBuild_ = std::move(prevGroupCountThisBuild_);
        prevTransformByKey_ = std::move(nextTransformByKey_);
    }

    // Memory-cost report, said once, sized from the real instance count this build reached rather
    // than a number that drifts with kMaxDraws or scene content.
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

    // gpuStat's destructor closes the marker here (see ScopedGpuStat for the two-hand-popped-exits
    // bug this replaces). rtAccelKey_/rtAccelSnapValid_ are deliberately left untouched on this
    // exit: the gate above already said "rebuild" (a key mismatch, a forced condition, or no
    // snapshot yet), so whatever they hold still correctly describes the LAST build that actually
    // ran ctx.buildTlas, which this exit (empty tlasInstScratch_) doesn't reach.
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

    // Not a function of drawsPrev_ -- see updateRtParamsPerFrame(). Factored out so the gate's skip
    // branch above can set identical values without duplicating them.
    updateRtParamsPerFrame();
    // MATERIAL TABLE FIRST, and the order is load-bearing: buildGeometryTable() below uploads
    // rtInstanceData_; buildMaterialTable() fills in every materialIndex, which the per-draw loop
    // left at placeholder 0. Called the other way round (as this stood until now), the upload
    // carried the placeholder and the fix-up landed on a CPU copy nobody read again -- every ray hit
    // in ray-driven mode silently indexed gRtMaterials[0], the fallback row. Silent, because
    // RtInstance already carries albedo/metallic/roughness per-draw; what came from the fallback
    // instead was everything else (reflectance, f90, ior, transmission, subsurface, graphId).
    // Found by asking why a coat authored at weight 1 changed nothing in the default render mode
    // and everything in the others. buildMaterialTable touches no geometry and needs no cb_ gate.
    // Texture table before material table: buildMaterialTable asks residentTexture() for indices,
    // and a null table would leave every one unbound.
    ensureTextureTable();
    buildMaterialTable(matConstantsScratch_);
    // w > 0.5 tells the lit pass it may trace a reflection ray, only true when the flat geometry
    // table actually exists -- a reflection hitting geometry it can't look up would read a
    // neighbour's triangle rather than fail visibly.
    cb_.rtParams[3] = buildGeometryTable(ctx) ? 1.0f : 0.0f;
    if (!rtLogged_) {
        AVER_INFO("[Voxi] RayQuery active ({} instances, {} bottom-level structures)",
                  static_cast<u32>(tlasInstScratch_.size()), static_cast<u32>(blas_.size()));
        rtLogged_ = true;
    }
    // Every build here is a full PREFER_FAST_TRACE build (the RHI has no refit verb), so this count
    // IS the bill -- also the only way to see the rebuild predicate (meshVertexBuffer non-zero) go
    // wrong: if it stops being true, a static scene silently rebuilds everything every frame and
    // looks identical. Keyed on BOTH halves, not their sum: two first-time builds becoming two
    // rebuilds is the same total but a different statement about the cache.
    const u32 rebuilds = (static_cast<u32>(rebuiltThisFrame_.size()) << 16) | (firstBuilds & 0xFFFFu);
    if (rebuilds != lastBlasRebuilds_) {
        AVER_INFO("[Voxi] bottom-level builds this frame: {} ({} first-time, {} rebuilt) over {} "
                  "draws of {} distinct meshes",
                  firstBuilds + static_cast<u32>(rebuiltThisFrame_.size()), firstBuilds,
                  static_cast<u32>(rebuiltThisFrame_.size()), static_cast<u32>(drawsPrev_.size()),
                  static_cast<u32>(blas_.size()));
        lastBlasRebuilds_ = rebuilds;
    }

    // The gate's own bookkeeping, kept warm regardless of whether rtSkipUnchangedTlas is on right
    // now: costs one more pass over drawsPrev_ (negligible next to the TLAS build and buffer
    // uploads this build already paid for), so turning it on mid-session never has to wait an
    // extra frame to prime. Only the skipped/rebuilt counters and the report itself are gated on
    // the setting (see reportRtAccelGate()), so its ratio describes ticks the gate was actually
    // consulted for, not ticks it was switched off.
    takeRtAccelSnapshot();
    if (settings_.rtSkipUnchangedTlas) reportRtAccelGate();
}

// cb_.rtParams[0..2]: the sun's angular size (as a tangent, so the shader multiplies rather than
// re-derives it), the shadow ray count and the ray bias -- none of them a function of drawsPrev_, all
// three read every frame by shadowPass()/PSRayDriven regardless of whether buildAccelerationStructures
// rebuilt anything this frame. Split out of that function's own tail so its gate's skip branch can set
// them without duplicating the derivation.
void VoxiRenderer::updateRtParamsPerFrame() {
    // How fast a ray-traced shadow edge softens is the sun's angular size, not a tuned constant --
    // the disc subtends about half a degree and rtShadow spreads its rays across exactly that.
    // Taken from the sky model so a scene that moves the sun or widens the disc gets agreeing penumbrae.
    const f32 discDeg = dev_->skyAtmosphere().sunAngularDiameterDeg;
    const f32 halfAngle = discDeg * 0.5f * 0.01745329252f;
    cb_.rtParams[0] = std::tan(halfAngle);
    // The disc decides how many rays a penumbra needs; the tier's count is only the ceiling. MEASURED
    // on Sponza: 1/2/4/8 rays gave bit-identical frames, still and moving, at ~0.5deg, 8 cost 0.66ms over 2.
    // Rays = ceil(disc / 0.3 deg), at least 2: 0.55 deg -> 2, 1.2 deg -> 4, 2.4 deg -> 8.
    rtShadowRaysUsed_ = std::min(rtShadowRays_, std::max(2u, static_cast<u32>(std::ceil(discDeg / 0.3f))));
    cb_.rtParams[1] = static_cast<f32>(rtShadowRaysUsed_);
    // Base ray bias in centimetres, scaled by view distance in the shader. Small enough not to
    // detach a contact shadow, large enough that a surface does not intersect its own rays.
    cb_.rtParams[2] = 0.05f;
}

// True when buildAccelerationStructures() must run its real per-draw loop this frame regardless of
// what rtAccelDrawsKey() says -- checked here the cheap way: an unordered_map lookup and at most one
// virtual call per draw, none of the real loop's BLAS creation/instance population/material resolution.
bool VoxiRenderer::rtAccelMustForceRebuild() const {
    for (const Draw& d : drawsPrev_) {
        // A compute-skinned mesh's BLAS is rebuilt inside the per-draw loop every call -- freezing it
        // here would silently show a shadow/reflection at a stale pose. Presence in the draw list is
        // reason enough; nothing about its key changing is required.
        if (dev_ && dev_->meshVertexBuffer(d.mesh)) return true;
        // Same "destroyed-and-reused mesh handle" check the real loop makes: a BLAS cached under a
        // mesh handle the factory no longer attributes to it is dead, and a key-only gate would leave
        // the TLAS pointing straight at it.
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

// Order-independent, same reason as giDrawsKey() (see further down): occlusion culling reorders
// drawsPrev_ every frame, so this sums a per-draw hash rather than folding one in list order --
// the only way a reshuffled-but-otherwise-identical draw list still matches.
//
// Covers everything the per-draw loop and the two tables below it read to decide an instance's TLAS
// entry, RtInstance record and material-table row: mesh, world transform, the two mask/flag-picking
// flags (translucent, hiddenFromOwner), and material.
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

        // Finalise before adding -- same avalanche giDrawsKey() uses: FNV's last step leaves
        // neighbouring inputs correlated in the low bits, and plain addition of correlated values
        // collides more readily than decorrelated ones.
        h ^= h >> 33; h *= 0xff51afd7ed558ccdull;
        h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull;
        h ^= h >> 33;
        key += h;
        ++counted;
    }
    // The count, mixed in rather than added -- see giDrawsKey()'s comment for exactly what this does
    // and doesn't catch.
    key ^= counted * 1099511628211ull;
    return key;
}

// True when nothing buildAccelerationStructures() would read (drawsPrev_, or the BLAS cache/dynamic-
// mesh state its loop consults) has changed since the last build, so tlas_/rtInstanceData_/their SRVs
// are still correct. Modelled on giSnapshotUnchanged() (read it first): same "reject once per reason,
// log it" shape, same split from takeRtAccelSnapshot() (only called once a rebuild is decided).
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

// Records what the build about to run was computed from. Mirrors takeGiSnapshot(): the non-const
// half of the gate, called once a build has actually happened.
void VoxiRenderer::takeRtAccelSnapshot() {
    rtAccelKey_ = rtAccelDrawsKey();
    rtAccelSnapValid_ = true;
}

// Widening-interval "N rebuilt / M skipped" report, same shape and reason as the GI rebuild gate's
// (prePass(), right after giSnapshotUnchanged()): a fixed-window report lands mid-load-in and
// describes a phase nobody asked about, so this reports both the lifetime and since-last-report ratio.
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

// Groups a previous-transform key by (mesh, drawBinding) -- see VoxiRenderer.hpp's declaration
// comment for why this pair, not mesh alone, and for the disambiguating ordinal. Same FNV-1a
// offset/prime as giDrawsKey()/buildGeometryTable's hash.
u64 VoxiRenderer::prevTransformGroupKey(rhi::MeshHandle mesh, rhi::BindingSetHandle matSet) const {
    u64 key = 1469598103934665603ull;
    key ^= static_cast<u64>(mesh);   key *= 1099511628211ull;
    key ^= static_cast<u64>(matSet); key *= 1099511628211ull;
    return key;
}

// Builds one orthographic light frustum per cascade, fitted to a slice of the camera's view, and
// writes the matrices and splits into cb_. Returns the usable cascade count, 0 if there is no camera.
// A hash of what voxelizePass would rasterise: every draw's mesh and full world transform, same
// FNV-style mix as buildGeometryTable's over rtInstanceMesh_. Hashed as raw float bits, not compared
// with a tolerance -- a cache tolerating "almost the same" transform shows wrong lighting for a while.
//
// Order-independent, and it used to be the opposite on purpose: the old reasoning was that a
// reordered draw list changes the injection order into the atomic accumulator, so it's "not the same
// result" -- true, but the gate only needs to know "would rebuilding produce the volume I already
// have", and float-rounding noise in an atomic accumulation isn't a visibly different volume:
// what the old check actually did was reject on pure reordering.
// MEASURED: PTTest sets RENDER.OCCLUSIONCULL 1, so the per-entity walk visits occlusionOrder_
// (SandboxApp's cluster draw loop), rebuilt every frame from last frame's hierarchical-Z partition
// against the current viewProj, so rotating the camera reshuffled a completely static world's draw
// order and rejected the gate on 92 of 128 ticks -- forcing full revoxelisation of an unchanged
// volume; the cost of the alternative is float noise in the low bits of a bounced-light accumulation.
//
// Wrapping addition over per-draw hashes (commutative, so reordering can't change it; the one part
// of this design marked mandatory rather than optional). The count is
// folded in at the end and each draw's hash is finalised (xor-shift-multiply) before adding, to
// decorrelate the addends -- without the count, a list that gains one draw and loses a different one
// summing to the same total would read as unchanged.
// True when voxelizePass would actually inject this draw into the volume.
//
// The bounds test was missing from the hashes, and its absence was the exact failure both warn
// about: a draw beyond the volume changed giDrawsKey, the gate forced a full rebuild, and
// voxelizePass then culled that same draw anyway -- all of the gate's cost, none of its benefit.
// Matters most where it's worst: a streamed level moving props kilometres away, or a scatter layer
// outside a level-fitted GI volume, re-keyed the volume every tick.
// The sphere comes from center_/extent_, not giShadowCentre_/giShadowRadius_, so voxelisation doesn't
// depend on the GI shadow PSO having built -- if it fails, those fields keep stale values instead.
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
        // Translucent, skinned, and out-of-volume are all "the pass won't inject this", now one
        // predicate rather than three hand-copied tests -- see giVoxelisedDraw for what the third
        // one was costing while missing here.
        if (!giVoxelisedDraw(d)) continue;
        u64 h = 1469598103934665603ull;
        // depthMesh, NOT mesh: voxelizePass draws d.depthMesh, so this hash must agree with that
        // exactly. Hashing d.mesh watched a handle the pass never reads -- with cluster LOD on,
        // d.mesh is a per-frame cut rebuilt on visible-cluster changes, so a rotating camera re-keyed
        // a volume whose actual injected geometry (the stable depth proxy) hadn't moved.
        h ^= static_cast<u64>(d.depthMesh);
        h *= 1099511628211ull;
        for (u32 i = 0; i < 16; ++i) {
            u32 bits = 0;
            std::memcpy(&bits, &d.world[i], sizeof(bits));
            h ^= static_cast<u64>(bits);
            h *= 1099511628211ull;
        }
        // Material lands in the baked radiance (voxelizePass binds d.matSet/d.mat for PSVoxel), so an
        // edit with no movement must still rebuild. Used to hash colour/metallic/roughness only (all
        // an unauthored draw has), so an authored material's real constants never moved it -- a lamp
        // edited in the Material Editor never reached the bounced light.
        hashDrawMaterialInto(h, d);
        // Finalise before adding (splitmix64's finaliser, used as an avalanche): FNV's last step
        // leaves neighbouring inputs correlated in the low bits, and plain addition of correlated
        // values collides more readily than decorrelated ones.
        h ^= h >> 33; h *= 0xff51afd7ed558ccdull;
        h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull;
        h ^= h >> 33;
        key += h;          // wrapping, and commutative: the whole point
        ++counted;
    }
    // The count, mixed in rather than added, distinguishes N draws from N+1 and nothing else -- it
    // does NOT stop "a list that gains a draw and loses a different one" (identical length either
    // side, so this term contributes identical bits); what separates {A,B} from {A,C} is that two
    // splitmix-finalised hashes don't happen to sum alike, a ~2^-64 coincidence rather than a
    // designed defence. Worth keeping (a pure add/remove IS caught cheaply), not worth believing more of.
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
    // Which check rejected, said once per reason -- two plausible causes were fixed on reasoning
    // alone and neither was it, the point at which guessing stops being cheaper than measuring
    // across the gate's four independent checks. The first version latched on the first rejection
    // of any kind, trivially "no snapshot yet" on frame one, hiding every real cause behind it.
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
    // Zero-initialised then assigned, the whole reason this works: skyAtmosphere() returns BY VALUE
    // and SkyAtmosphere opens with a bool then padding, so memcmp on a raw returned copy compared
    // that unspecified padding and this gate never once fired (0 skipped of 512 ticks on a static
    // scene, 2000 frames). Copy-assigning into a value-initialised object keeps padding at zero on
    // both sides, so only members are compared.
    rhi::SkyAtmosphere now{};
    now = dev_->skyAtmosphere();
    // cloudTime is a clock -- differs every tick by construction, so a byte comparison rebuilt the
    // entire 128^3 volume every giUpdateInterval frames on a static scene forever. MEASURED: byte 240
    // of 248, 3.01e-05 (snapshot) vs 19.19752 (live). Normalised out of BOTH sides rather than
    // compared field-by-field, so a field added to SkyAtmosphere later can't silently escape the check.
    rhi::SkyAtmosphere was{};
    was = giSky_;
    const f32 cloudTimeDelta = std::fabs(now.cloudTime - was.cloudTime);
    now.cloudTime = was.cloudTime = 0.0f;

    if (std::memcmp(&now, &was, sizeof(now)) != 0) {
        // Which byte, not just "something" -- diagnosed twice from a plain "sky/sun changed" and
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
        // Which axis, not just "the draw list". See giDrawsSubKeys.
        u64 c = 0, m = 0, w = 0, mt = 0;
        giDrawsSubKeys(c, m, w, mt);
        // Counted over the run, not latched on the first rejection: reporting once per axis only
        // ever described load-in (17 draws growing to 155), not the steady state that's the question.
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

            // Why a handle left, which naming the handles alone never says (five upstream hypotheses
            // were refuted by measurement here). Exactly two ways one can leave, pointing at
            // different code: still in drawsPrev_ -> submitted, giVoxelisedDraw rejected it (Voxi's
            // own, reason named below); gone from drawsPrev_ -> never reached submitDraw, cause is
            // upstream in whoever decided not to submit. Every earlier hypothesis assumed the
            // second case -- if the log says the first, they were all looking in the wrong file.
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

    // Clouds tested LAST, deliberately: excusing the cloud clock isn't ignoring clouds (a drifting
    // layer changes how much sky reaches the ground, so the bake goes stale by a bounded amount, not
    // indefinitely), but testing it above the geometry checks made "clouds drifted" ambiguous --
    // fired whether or not the draw list also changed. Tested last, it means exactly "nothing but the
    // sky moved", which is what lets giCacheScheduleDump below decline to write to disk. Only fires
    // with clouds enabled -- a clear sky holds its bake for as long as nothing else moves.
    //
    // Thirty seconds, not two: two seconds forced a full ~18 MB revoxelisation every two seconds
    // whenever a scene had clouds, and MEASURED on ElectricDreams under camera motion was the
    // dominant source of 439 MB of cache writes in a 900-frame run. Cloud shadow is low-frequency,
    // indirect and diffuse -- thirty seconds of staleness there isn't visible; fifteen rebuilds/minute is.
    constexpr f32 kGiCloudStaleSeconds = 30.0f;
    if (now.cloudsEnabled && cloudTimeDelta > kGiCloudStaleSeconds) {
        giRebuildCloudOnly_ = true;
        return reject(5, "clouds drifted");
    }
    return true;
}

// Records what the rebuild about to run was computed from.
// The GI derived-data cache: the rebuild gate above answers "changed since the last bake this run";
// this answers the same question ACROSS runs, so a previously-seen level skips a full revoxelisation
// on its first lit frame.
void VoxiRenderer::setGiCacheDir(const std::string& dir) {
    if (dir == giCacheDir_) return;
    giCacheDir_ = dir;
    // A new directory means a new project: whatever was tried against the old one says nothing.
    giCacheTried_ = false;
    giCacheTriedKey_ = fmt::GiCacheKey{};
    giCacheKnownKeys_.clear();
}

// The key describing the volume as it stands after takeGiSnapshot.
// Sky hashed the same way the gate compares it: byte for byte, minus the cloud clock -- a cache
// keyed on a running clock would miss on every load by construction, the one failure mode that
// would look like it worked while never hitting.
fmt::GiCacheKey VoxiRenderer::giCacheKey() const {
    fmt::GiCacheKey k;
    k.drawsKey = giDrawsKey_;
    rhi::SkyAtmosphere sky = giSky_;
    sky.cloudTime = 0.0f;
    const u8* p = reinterpret_cast<const u8*>(&sky);
    u64 h = 1469598103934665603ull;
    for (usize i = 0; i < sizeof(sky); ++i) { h ^= p[i]; h *= 1099511628211ull; }
    // Folded into the sky's key because it IS the sky's part of the bake: with ReSTIR GI chosen,
    // PSVoxel leaves the sky out (voxelSkyInjected), and a cached volume from the other rule would
    // otherwise be restored as a match. Only for the sky-less bake, so keys written before this
    // existed (all sky-injected) still hit.
    if (!giSnapVoxelSky_) { h ^= 0x5Cu; h *= 1099511628211ull; }
    // Bake rule's version: a volume is a function of PSVoxel's injection as much as its inputs, so a
    // change to what a settled volume holds must miss every older entry. 2 = per-channel bounce gain
    // capped; volumes baked before it could hold a red runaway (NewSponza_Night after a sun drag),
    // which a matching key would reload on every open.
    constexpr u64 kGiBakeRuleVersion = 2;
    h ^= kGiBakeRuleVersion; h *= 1099511628211ull;
    k.skyKey = h;
    for (u32 i = 0; i < 3; ++i) k.centre[i] = giSnapCenter_[i];
    k.extent     = giSnapExtent_;
    k.resolution = voxelResBuilt_;
    k.mipCount   = voxelMips_;
    return k;
}

// Sizes the two staging buffers and works out where each mip sits inside them.
// The backend's layout, not the file's: D3D12 pads every copy row to 256 bytes (a 16-wide RGBA16F
// mip occupies 2x its bytes), Vulkan packs tight. Both asked, not assumed, so the file itself stays
// tightly packed and portable between the two.
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

    // Once per key: a miss must not re-open the same absent file on every rebuild -- rebuilds are
    // exactly the frames already doing the most work.
    if (giCacheTried_ && giCacheTriedKey_ == key) return false;
    giCacheTried_ = true;
    giCacheTriedKey_ = key;

    // The buffer is part of the cache: a bake landed in RAM but not yet on disk must be a HIT, or
    // write-behind turns every within-session revisit into a full revoxelisation -- the regression
    // would look like the cache had simply stopped working.
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
    // A restore writes the WHOLE volume (copyBufferToTexture loop above runs every mip in full), so
    // invalidating the previous box makes the next rebuild start from a full-grid box instead of
    // unioning a stale drawsBox_prev against contents that box no longer describes.
    giBoxPrevValid_ = false;
    return true;
}

// A ceiling on ONE entry, absolute, not a fraction of the RAM budget -- the budget bounds how much
// is buffered before a flush, it never said any single entry is worth writing.
// MEASURED, PTTest at 512^3 (one volume = 1170 MB): at the 256 MB default, an entry went over budget
// instantly and flushed synchronously every bake -- 118 ms/frame wall clock against 12.8 ms GPU, 9.4
// GB on disk in one session (giCacheSweep keeps 8); moving the camera in Play re-keys the volume
// often enough to do it over and over, which reads as a hang and was reported as a crash. At a
// 4096 MB budget it's WORSE: four entries buffer to 4681 MB and write 4.8 GB in one flush. A bigger
// budget buys a bigger stall -- no setting of it makes a gigabyte-per-bake write reasonable, which
// is why this is a ceiling, not a ratio.
//
// 256 MB, the shipped default budget: above that line the derived data costs more to move than to
// derive (~8 ms GPU to revoxelise vs. seconds of disk to write), so the entry is dropped and rebuilt
// instead -- the cache's own contract: losing it costs one rebuild.
//
// File scope, not a local in giCacheTick, because giCacheScheduleDump must consult it BEFORE issuing
// the readback (see the note there on what testing it too late used to cost).
static constexpr u64 kMaxCachedGiEntryBytes = 256ull * 1024ull * 1024ull;

// Said once, names the lever that actually helps -- raising the budget is NOT it; that only makes
// the stall larger.
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

    // A cloud-only rebuild isn't worth a file: giCacheKey deliberately zeroes cloudTime, so a bake
    // whose only input change was the cloud clock carries a key identical to the one on disk while
    // holding different voxels -- writing it would make the cache non-deterministic (same key naming
    // two different volumes depending on which run wrote last).
    if (giCacheSettleCloudOnly_) return true;

    giCachePendingKey_ = giCacheKey();
    if (giCachePendingKey_.resolution == 0) return true;
    // Restored from, or already queued to, the cache this session: nothing new to write.
    if (giCacheKeyKnown(giCachePendingKey_)) return true;

    // The ceiling is tested HERE, before anything is copied -- it used to be tested only in
    // giCacheTick, after the copy had already run, making "not cached" the MOST expensive path: at
    // 512^3 every rebuild issued a 1170 MB GPU->CPU copy of all nine mips, allocated a 1170 MB
    // readback buffer and a 1170 MB vector, then dropped the result on the size test below.
    //
    // MEASURED, PTTest/Sponza with --cam-wobble: the gate rejects ~72% of ticks moving, so this ran
    // about a hundred times in 151 frames -- unmarked GPU work inside "Voxi GI update", the reason
    // that scope's exclusive time read 10.79 ms moving vs. 0.66 ms still, a number the comment at the
    // top of prePass attributed to beginShadowHistory/endShadowHistory, neither of which has any
    // camera-dependent cost at all.
    //
    // Not a behaviour change: an over-ceiling volume was never cached before and still isn't. The
    // only difference is it now costs nothing to not cache it.
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

    // Buffered, not written -- see giCachePendingEntries_ for why (the cache's contract: losing an
    // entry costs one rebuild). Replaces an entry with the same key rather than accumulating
    // duplicates -- re-baking the same inputs happens when an author moves the sun back.
    const u64 bytes = static_cast<u64>(entry.voxels.size());

    // Belt and braces: giCacheScheduleDump refuses to issue the readback for an entry over
    // kMaxCachedGiEntryBytes, so reaching here oversize means the key changed between scheduling and
    // readback (e.g. a mid-flight resize). Cheap to keep, and without it that case writes a gigabyte
    // file the ceiling exists to prevent.
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

    // Bounded, because nothing else bounds it: every distinct bake writes a new file, none overwritten.
    // Raised from 8, now bounded by bytes too: MEASURED on ElectricDreams under camera motion, one
    // 900-frame run touched 17 distinct volumes, so 8 was throwing away entries asked for again
    // shortly. The byte cap makes raising the count safe -- 64 entries is ~1.1 GB at 128^3 but would
    // be 16 GB at the largest entry the writer accepts, and a count alone can't tell those apart.
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
    // The mesh multiset itself, so giSnapshotUnchanged can say WHICH draws came and went, not just
    // that the count moved. Five hypotheses refuted by measurement so far (frustum culling,
    // submission order, occlusion bucketing, GPU per-cluster path -- it does skip drawMesh(), the
    // only route to submitDraw, so it LOOKED decisive: --no-lod-mesh-shader under a six-degree wobble
    // reproduces the same axis census, 269 vs 267 and 269 vs 274, so not that either).
    //
    // What the census says: all four axes move on every rejection, and the count moves with them --
    // transforms and materials are not drifting, the SET is changing, by a handful of meshes out of
    // ~269, in consecutive runs of handles that look like one source mesh's split parts. Next step:
    // report WHY a handle left, from the site that decided it, since this end of the pipe can only
    // ever say that it did.
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

    // Frustum's eight corners as offsets FROM THE EYE (camera-relative inverse, see
    // rhi::PerFrameCB::invViewProjRel); the fit stays relative until the centre. Absolute corners
    // minus the eye would recover a ~2 cm near-plane offset from two |eye|-sized values -- ~1% error
    // at cloud altitude (f32 ulp 0.0156 cm at 2e5 cm), changing every frame the eye moves -- and every
    // split distance below scales off camNear. D3D depth is [0,1].
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

        // No union with the GI volume here any more -- see kGiShadowSize for the measured cost of the
        // old union. giShadowPass/fitGiShadow answer the volume separately; every cascade here is
        // fitted to the camera alone.

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
// Rebuilt only when the mesh set changes -- copying every mesh every frame would cost more than the
// reflections it enables; an instance's transform lives in the small, every-frame instance record instead.
bool VoxiRenderer::buildGeometryTable(rhi::IRenderContext& ctx) {
    if (!res_ || !dev_ || rtInstanceData_.empty()) return false;

    // One entry per distinct mesh, not per instance -- the whole cost of this function. Used to give
    // every INSTANCE its own slice, copying identical geometry per instance: ~522 MB of vertex buffer
    // for ~120 distinct meshes at 769 instances (16.3M vertices, 11.5M indices, two copyBuffer calls
    // each) in ElectricDreams, scaling to ~3.5 GB/13,310 copies at 6,655 instances ("Voxi acceleration
    // structures" cost 9.1ms at 759 instances, 64.2ms at 6,571, ~9.7us/instance both times -- linear
    // per-instance cost, not a TLAS build). Geometry is a property of the MESH (only transforms differ
    // per instance, already per-instance in RtInstance::objectToWorld), so the table is built over the
    // distinct set, each instance pointing at its mesh's slice.
    // Sorted, not first-appearance order: first-appearance offsets change whenever the draw list is
    // reordered (every frame here, via occlusionOrder_), rebuilding the table while naming the same
    // meshes. Sorting makes offsets a function of the set alone.
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
        // Shared vertices, one slice: createMeshSharingVertices (LODs) and createPosedPartMesh (a
        // skinned mesh's per-material parts) hand out handles naming the same vertex buffer at the
        // same count with indices in its numbering, so the root's slice is byte-for-byte what a copy
        // would hold. A two-material character used to put its whole posed buffer in twice.
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

    // Both transitions explicit: D3D12 would promote Common to CopyDest implicitly, but the RHI
    // tracks buffer state to catch exactly this mistake and doesn't model promotion -- an implicit
    // promotion with an explicit walk-back would claim a state the tracker never saw it enter. The
    // walk-back matters on its own too, since promotion lasts the rest of the command list (a later
    // reflection ray would see a copy dest).
    ctx.bufferBarrier(rtVerts_,   rhi::ResourceState::Common, rhi::ResourceState::CopyDest);
    ctx.bufferBarrier(rtIndices_, rhi::ResourceState::Common, rhi::ResourceState::CopyDest);
    // Two copies per distinct mesh -- previously two per INSTANCE, one wasted for a mesh whose
    // vertex slice another handle's copy already fills.
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

// Builds this build's dense material table from rtInstanceMatKey_/matConstantsByKey (filled by
// buildAccelerationStructures' per-draw loop just before this), re-uploads only when content
// changed, and writes the final index into every rtInstanceData_[i].materialIndex.
// Sorted by key, not first-seen order -- same reasoning as rtGeomMeshes_: occlusionOrder_ reorders
// the draw list every frame regardless of whether the material SET changed, so a first-seen-order
// index would reshuffle (and re-upload, via the memcmp below) almost every frame. Sorting makes the
// table a function of the set alone.
// MaterialSystem exposes no dirty count/generation number, so this compares actual bytes instead --
// exact, and cheap at the "tens, not thousands" of resident materials MaterialSystem.hpp documents.
// Makes one texture resident in the ray path's bindless table and returns its index, or
// pbr::kUnboundTexture if it could not be made resident.
// Append-only and memoised: the same texture asked twice returns the same index, sizing the table by
// distinct IMAGES rather than materials (forty materials sharing one albedo occupy one slot).
// Nothing is ever freed -- see the table's declaration comment.
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
    // The RHI is asked, not told: setBindlessTexture refuses an out-of-range index rather than
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
    // Distinct keys, sorted -- see this function's header comment for why. std::map would give this
    // for free, but the table is rebuilt from an unordered_map already built by the caller for O(1)
    // lookups; sorting the few ("tens not thousands") keys once here is cheaper than paying map-node
    // overhead for the whole build.
    std::vector<u64> sortedKeys;
    sortedKeys.reserve(matConstantsByKey.size());
    for (const auto& [k, mc] : matConstantsByKey) { (void)mc; sortedKeys.push_back(k); }
    std::sort(sortedKeys.begin(), sortedKeys.end());

    // Index 0 is always the material system's fallback, the sentinel for a draw whose material
    // couldn't be resolved. Nothing in the per-draw loop produces such a draw today (every instance
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
        // Upload-heap ring, matching rtInstances_ exactly -- see the header's declaration comment
        // for why a rare write needs the same ring a per-frame write does: writeBuffer is
        // unsynchronised, and the hazard is per-write, not per-frame.
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

// Local lights (lamps): see the header's buildLocalLights comment for when the list is empty.
//
// What makes a light: an AUTHORED draw (the hashDrawMaterialInto test -- an unauthored draw's d.mat
// is not a material this project wrote) whose captured MaterialConstants carry MaterialFlag_Light
// with lightIntensity > 0, and whose mesh reported bounds (a negative radius has no sphere to light
// from). lightIntensity is a MULTIPLIER on what the emitter's own glow and size physically cast, not an
// absolute brightness: glow is emissiveFactor's peak channel as a Lambertian sphere's radiance L,
// size is the bounding-sphere radius r, and E1m = pi * L * r^2 (r in metres) is the irradiance that
// sphere casts at 1m, in sun units (SkyAtmosphere::sunIntensity) -- so 1 lights exactly what the
// glow/size cast, 2 lights twice that, and a lamp and the sun compose on the same scale. Colour is
// emissiveFactor normalised to max component 1 (white when all zero, so a lamp with no emissive
// factor still has an L and a colour to light with -- see the L fallback below).
//
// Range: where irradiance E1m * lightIntensity / d^2 falls to kLocalLightRangeCutoff of sun units --
// d = sqrt(E1m * lightIntensity / kLocalLightRangeCutoff) metres -- clamped to at least four radii (a
// lamp always lights its own surroundings) and at most 50m (the per-pixel loop must end somewhere);
// the shader fades to zero at it. Cost is how many lamps a pixel has in range: MEASURED on NewSponza's
// 22 lamps, the earlier absolute brightness (intensity 2, cutoff 0.01, ~14m ranges) cost 1.6ms of a
// 16ms frame, and a 32m range put nearly every lamp in every pixel's two loops. E1m for a small bulb
// is ~0.1, so 0.001 keeps range near that same ~14m (chosen, not measured); 0.01 would cut off at ~3m.
//
// Canonical order: drawsPrev_ reorders every frame (occlusionOrder_) but the shader's light pick
// doesn't care about order, so the kept set is re-sorted by its own bytes before upload -- the
// uploaded list (and rdLocalLightHash_) is a function of the SET alone, surviving a camera move that
// doesn't change which lamps are in it.
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
            // Two fields first, whole block only for a lamp: this loop visits every draw (up to
            // kMaxDraws), and nearly none are lamps.
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
            // L is the glow's own radiance -- peak channel before colour is normalised out below, so
            // a brighter emissiveFactor casts more light at the same lightIntensity. peak <= 0 (no
            // emissiveFactor) still falls back to L = 1, white, rather than casting nothing.
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

// Local lights (lamps): see the header comment. History trusted only when a scene pass wrote it LAST
// frame (rdLocalHistFrame_) under the SAME light list (rdLocalHistHash_) and the shadow history's
// reprojection is usable this frame (rtHistParams.y > 0; its 0.5 "sun moved" state is still usable
// here since lamps don't depend on the sun). Which mode wrote it doesn't matter: all three write the
// same quantity into the same pair.
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
// Instanced by default when shadowInstancedPso_ built: every surviving draw is grouped by mesh into
// shadowInstanceGroups_, one drawMeshInstanced call per group instead of one drawMesh per draw.
// Depth-only with a strict Less test is order-independent (except two triangles at exactly the same
// depth), so regrouping by mesh gives the identical shadow map for a fraction of the work -- up to
// ~2,114 drawMesh calls per cascade collapse to at most ~30 (this scene's distinct mesh count). Falls
// back to one-draw-per-instance (shadowPso_) if the instanced pipeline failed to build.
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

    // The atlas has no reader while ray tracing is on, in both raster and ray-driven mode -- broader
    // than the "cascade map for nobody" note that stood elsewhere in this tree claimed. The whole
    // chain: gShadowTex is sampled at exactly one place (shadowSampleCascade, voxi.hlsl:1585) via one
    // caller (shadowFactor:1594), reached only on `gShadowParams.z <= 0.5` (voxi.hlsl:1814-1817) or
    // the non-AVER_RT variant (:1819); PSRayDriven has no cascade arm at all (:2224 traces
    // unconditionally). shadowParams[2] IS this same rtActive_, written together in
    // buildAccelerationStructures (0.0f/false at the top, 1.0f/true once the TLAS exists), so they
    // cannot disagree. Whenever there's a TLAS, nothing samples this texture.
    //
    // MEASURED on PTTest Sponza, fixed camera: "Voxi shadow" GPU scope is 5.57ms at four cascades,
    // 2.07ms at two, on a ~47ms frame -- also why an earlier four->two cascade experiment found a
    // large saving with no visual change (there was no reader to notice).
    //
    // fitCascades() still runs above: cheap CPU work, and where curViewProj_ is captured, which
    // endShadowHistory needs every frame for temporal reprojection regardless of whether a cascade
    // is drawn. Returning before it would break every temporally-reprojected effect while looking
    // like a pure optimisation.
    //
    // shadowParams[1] = 0 is what makes this safe, not merely unread: shadowFactor's first line is
    // `if (gShadowParams.y < 0.5) return 1.0`, so a pixel reaching the cascade arm is unshadowed
    // instead of sampling a stale atlas. The two early-outs above set it for the same reason; this
    // is the third case, not a new convention.
    //
    // Skipping the pass also skips BOTH halves of its barrier pair, so the texture stays in
    // ShaderResource -- exactly the state its SRV binding expects. Skipping only one of the two
    // would be the bug this note exists to avoid.
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

        // What this cascade can actually resolve: each spends its 2048 texels over its whole fitted
        // radius, so a far cascade's texel is metres wide while a near one's is centimetres -- an
        // object smaller than a texel can't put a shadow into the map, so rasterising it is wasted.
        // Written for thousands of scattered ankle-height plants, sub-texel by cascade 2 and drawn
        // into it anyway. Per-cascade, not global: the same plant is real detail in cascade 0,
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
// Called from prePass INSIDE the giUpdateInterval gate, so it runs only on the frames voxelizePass
// runs -- 1 in 4 at the Medium default, vs. the old arrangement's every frame including the 3 in 4
// nothing read it.
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
    // Defensive, not expected to fire: the prePass rebuild branch requests the accumulator via
    // giAccumWanted_ and retries next tick rather than ever calling this function while
    // voxelAccumTex_ is 0. Kept anyway because "the gate change is correct" is exactly the kind of
    // claim a cheap check like this exists to catch being wrong about, rather than handing
    // clearBindings_'s slot-1 UAV a null texture.
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
    // Walks drawsPrev_ with EXACTLY the three skips the injection loop below applies (translucent,
    // compute-skinned, giVoxelisedDraw's bounds/volume test), so drawsBox names precisely the voxels
    // a draw could write -- looser would waste the saving, tighter would leave stale values uncleared.
    //
    // Runs every rebuild regardless of the bounded-dispatch flag, doubling as the M4-style census
    // instrument the log line at the bottom reports: how big the box WOULD be, so the saving can be
    // judged before opting in.
    //
    // depthMesh's AABB, NOT d.mesh's: depthMesh is what this pass actually draws a few lines below
    // (ctx.dispatchMeshFor/ctx.drawMesh(d.depthMesh)), and a coarser LOD depth proxy (SandboxApp.cpp's
    // depth-proxy resolver) can have a different footprint than the submit's own bounding sphere.
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
    // Full means clear/resolve/mip run over [0,res) on every axis -- the safe answer whenever the
    // box above can't be trusted. First true reason below wins, and doubles as this rebuild's C-7
    // census reason.
    //
    // Union with giBoxPrevDraws_ (the LAST rebuild's box), not drawsBox alone: mip 0 outside
    // drawsBox_k still holds whatever drawsBox_(k-1) wrote there (CSResolve zeroes a voxel with no
    // fragments), so a voxel a draw stopped touching needs re-clearing THIS rebuild or it keeps
    // showing stale light. Anything outside drawsBox_k UNION drawsBox_(k-1) is already correct (0) by
    // induction: rebuild k-1 established the invariant over its own union, this one extends it by
    // exactly drawsBox_k. giBoxPrevValid_ says the induction's base case holds.
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

    // Prefers the mesh-shader voxelise pipeline whenever the device built one, independent of
    // settings_.meshShaders (a wider switch that also moves the main scene's lit draws onto their
    // own mesh-shader pipeline, off by default as a real behaviour change). Voxelisation has no such
    // wrinkle: same `vox` desc, same PSVoxel pixel shader either way, MSVoxel running the identical
    // dominant-axis projection VSVoxel+GSVoxel do (see MSVoxel's comment for the normal-transform bug
    // this depended on fixing first). createPipelines() already validates voxelMsPso_ regardless of
    // the setting, so `voxelMsPso_ != 0` alone means the device proved it can do this; GSVoxel is the
    // fallback for no mesh-shader tier.
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
    // what's resident, so this cull is most of the pass. The test is giVoxelisedDraw's.
    u32 voxelSubmitted = 0, voxelCulled = 0, voxelSkinned = 0;

    for (const Draw& d : drawsPrev_) {
        // Translucent draws excluded (depth-only pass) -- see shadowPass's note on the same skip.
        if (d.translucent) continue;
        // A COMPUTE-SKINNED MESH IS EXCLUDED, DELIBERATELY -- a trade, not a fix. giDrawsKey hashes
        // mesh/transform/material, never the vertex buffer a skinning dispatch rewrites every frame,
        // so once a character's TRANSFORM settles the gate reports "unchanged" and its indirect-light
        // contribution freezes at whatever pose the last rebuild saw. Measured on a real rig: 2
        // rebuilt / 62 skipped of 64 ticks straight through a pose transition.
        // The BLAS cache fixes the same defect per-mesh by rebuilding every frame -- NOT available
        // here, since voxelisation is one volume: treating a skinned draw as always-changed would
        // force a full revoxelisation whenever any character is on screen (measured: the ~96% of GI
        // rebuilds this gate normally avoids, on top of the 17.3 ms GI already costs while skipping
        // them). So the interim state is absence, not a silent freeze:
        // a skinned character bounces no indirect light (matches PtSceneView::submitDraw). Real fix
        // is partial revoxelisation, which voxelizePass doesn't support today.
        if (dev_ && dev_->meshVertexBuffer(d.mesh)) { ++voxelSkinned; continue; }
        // The bounds half of giVoxelisedDraw, kept inline only for the census counters below -- the
        // predicate is the definition and must not drift from it.
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
        // voxelSkinned reported separately, NOT folded into `considered`: it's a capability gap, not
        // a cull, and averaging it in would hide the number a "missing indirect light" debug needs.
        //
        // The suffix is W3's census: injected draws' own box coverage (gridFraction(drawsBox, res))
        // vs. what the clear/resolve/mip dispatch actually ran over (gridFraction(box0, res)) -- the
        // gap is W3's saving; giBoxReason says why box0 was (or wasn't) smaller this rebuild.
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

    // Records this rebuild's OWN draws box (not box0, which also folds in the previous one) for the
    // next union -- the chain must span exactly one previous rebuild, not accumulate indefinitely.
    // giBoxPrevValid_ false whenever anyUnbounded fired: an untrustworthy box now must not be trusted
    // as "last rebuild's box" next time either.
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
        // Bounded to the box THIS rebuild's clear/resolve touched at mip 0 (giDispatchBox0_),
        // rounded to this level via mipBox(): CSMip's 2x2x2 footprint means a dest voxel outside
        // mipBox(box0, m, res) has its whole footprint outside box0 at mip m-1. srcMip stays 0
        // regardless -- the single-mip SRV this binding set declares already rebases the Load to
        // level m-1, so the shader never needs to know which real mip that is.
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
// base class's comment for why the backend asks at all. Only two things make the shader sample it
// (voxi.hlsl's averBlendedOutputBackdrop gate, ~1997): a non-zero attenuation distance (the material is a
// volume seen THROUGH), or a node graph (graphId != 0, since a graph can compute attenuation per
// pixel and this can't read a distance it never wrote). Everything else -- a decal like NewSponza's
// floor dirt -- draws flat blended colour over hardware blend and never touches the texture.
bool VoxiRenderer::blendedDrawReadsBackdrop(const void* materialConstants, u32 bytes) const {
    if (bytes < sizeof(pbr::MaterialConstants) || !materialConstants) return true;   // unknown layout: assume yes
    const auto& mat = *static_cast<const pbr::MaterialConstants*>(materialConstants);
    return mat.attenuationDistance > 0.0f || mat.graphId != 0;
}

// True once the feature is up: shadowing and the bounce are terms inside Voxi's lit pixel shader.
bool VoxiRenderer::overridesScenePipeline() const { return giReady_; }

// True while the debug view replaces the scene, including the backend's line draws.
// Two reasons to replace the scene, not interchangeable -- see shadowHistoryActive() in the header,
// which asks only about the first.
// The table the blended pipeline expects bound. The device asks during its replay, since this
// feature is not on the stack then -- see IRenderFeature::sceneBindlessTable.
rhi::BindlessTableHandle VoxiRenderer::sceneBindlessTable() const { return rtTexTable_; }

bool VoxiRenderer::suppressesScene() const { return debugViewActive() || rayDrivenActive(); }

// The debug raymarch paints every pixel from the voxel volume with no depth to test against, so
// anything else drawing into that frame draws over a picture of something else. Ray-driven mode is
// the opposite: it writes real SV_DEPTH (1.0 on a miss, which the sky pass tests EQUAL against), so
// the sky lands on precisely the pixels the primary rays missed.
bool VoxiRenderer::suppressesWholeFrame() const { return debugViewActive(); }

// Draws whichever pass has replaced the scene, over the colour target and viewport the backend
// already bound. The debug raymarch wins when both are somehow asked for: it is a diagnostic, and
// a diagnostic that silently did not run because another mode outranked it would be useless.
void VoxiRenderer::scenePass(rhi::IRenderContext& ctx) {
    if (!debugViewActive() && rayDrivenActive()) {
        // Staged ray-driven passes checked first, before any single-pass state is touched, so the
        // default (rayDrivenStages == 0) reaches the single-pass code exactly as before this feature
        // existed -- rdStagedActive() returns false on its first line then, `reason` stays null, and
        // nothing below runs differently. See the header's rdStagedActive() comment for every
        // condition it checks.
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
        // Logged once, only when there WAS a reason to give: rdStagedActive() leaves `reason` null
        // when nothing was requested or nothing needs staging, which isn't a fallback worth a line.
        if (stagedFallbackReason && !rdStagedFallbackLogged_) {
            rdStagedFallbackLogged_ = true;
            AVER_WARN("[Voxi] voxi.rayDrivenStages (1 or 2) requested the staged ray-driven passes, but "
                      "{}; falling back to the single-pass ray-driven primary (said once)",
                      stagedFallbackReason);
        }
        // Its own marker, so the go/no-go against the rasteriser is a subtraction between two named
        // spans rather than a difference of whole frames.
        rhi::ScopedGpuStat rayStat(ctx, "Voxi ray-driven primary");
        // See pickGbuf(): ray-driven mode never goes through scenePipeline(), so this is the one call
        // site that has to ask for its G-buffer twin directly.
        // Texturing and the G-buffer are independent axes now, four pipelines not three -- used to
        // read `rayDrivenTexPso_ != 0 && !dev_->gBufferEnabled()`, true to the pipelines that existed
        // (no textured G-buffer variant) but meant --gbuffer also turned texturing off, so any A/B
        // across that flag attributed a texturing difference to the G-buffer. See rayDrivenTexGbufPso_'s
        // header comment for why that had to close before the deferred-lighting work leans on this flag.
        //
        // pickGbuf() stays the single authority on whether four targets will really be bound (it
        // declines when the twin never compiled, before init(), and at MSAA > 1 where single-sample
        // G-buffer targets can't bind beside a multisampled colour target) -- asking
        // dev_->gBufferEnabled() again here would duplicate that MSAA rule and the two could drift.
        const bool gbufBound = pickGbuf(rayDrivenPso_, rayDrivenGbufPso_) == rayDrivenGbufPso_;
        // Falls back within the same target count, never across it: if the textured G-buffer
        // pipeline didn't compile, fall back to the FLAT G-buffer one, not the textured single-target
        // one -- binding a one-target pipeline while the backend has bound four leaves the G-buffer
        // unwritten while every caller believes it's on.
        const rhi::PipelineHandle rdPso =
            gbufBound ? (rayDrivenTexGbufPso_ ? rayDrivenTexGbufPso_ : rayDrivenGbufPso_)
                      : (rayDrivenTexPso_     ? rayDrivenTexPso_     : rayDrivenPso_);
        // Local lights (lamps): raised before this draw shades lamps and writes u19 itself when
        // compiled with the lamp term (kRdSinglePassLamps), 0 otherwise. Left raised for the blended
        // replay, which lights its panes from the same list.
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

// Staged ray-driven passes: records CSRdVisibility, then (when the sub-stage split settings ask for
// them) S1/G1, then CSRdShadow/CSRdGi/CSRdSkyOcc/CSRdRefl back to back with no barrier between THEM,
// then the AVER_RD_SPLIT fullscreen draw -- GPU spans in place of scenePass()'s single "Voxi
// ray-driven primary" span. Called only once scenePass() has confirmed rdStagedActive(), so every
// existence/backend check lives there, not here.
//
// VERIFIED read-only against D3D12Device.cpp (the only backend rdStagedActive() lets through), not
// assumed: each point below is a documented way to remove the device if wrong, not just a wrong pixel.
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
// No barrier, no timestamp, between CSRdShadow/CSRdGi/CSRdSkyOcc/CSRdRefl: shadow (u2/u12), GI
// (u6-u10/u13) and sky occlusion (u4/u5/u14) write DISJOINT resources by contract. Reflection (u3/u15,
// via rtReflectionTemporal/gRdReflTex) is ASSUMED disjoint from the other three on that same contract,
// not independently reverified here: this file only schedules the dispatch, and the shader side states
// rtReflectionTemporal/rtReflection/rtReprojectReflection touch no other group member's UAVs. All four
// read only the fenced visibility record, last frame's own histories, and this frame's NRD outputs
// (from prePass). A GPU timestamp between two dispatches can drain the pipe to take the
// reading, serialising work that would otherwise overlap -- why all four sit inside ONE ScopedGpuStat
// ("Voxi RD lighting stages") instead of each opening its own span the way CSRdVisibility and Stage B
// still do.
//
// CSRdLocalLights (dispatched right after CSRdShadow) joins the same barrier-free group on the same
// contract: writes u19 alone, reads only the fenced visibility record, light list (t18), LAST frame's
// histories (t19, t6) and the TLAS -- never CSRdShadow's this-frame outputs (u2/u12). A shader change
// that reads either of those needs a barrier here first.
//
// Sub-stage splits (rayDrivenShadowTiles/rayDrivenGiSplit) are the one exception: S1 (CSRdShadowProbe)
// and G1 (CSRdGiTrace) run first, inside the same span, and their outputs (gRdShadowTiles/gRdGiCand)
// get an explicit uavBarrierBuffer before CSRdShadow's tiled branch or CSRdGi's split branch reads
// them back -- these are NOT disjoint, so the barrier is recorded unconditionally, whether or not
// voxi.rayDrivenStageTiming is on.
void VoxiRenderer::recordStagedRayDriven(rhi::IRenderContext& ctx) {
    const bool gbufBound = pickGbuf(rayDrivenPso_, rayDrivenGbufPso_) == rayDrivenGbufPso_;
    const rhi::PipelineHandle stageBPso = gbufBound ? rayDrivenSplitTexGbufPso_ : rayDrivenSplitTexPso_;

    // From the scene VIEWPORT, not the full render target -- curSceneViewport_ is this frame's {x, y,
    // w, h} in target pixels (IDevice::sceneViewport, read fresh every frame in beginShadowHistory,
    // which prePass() calls before scenePass() runs). Dispatching over the
    // whole render target would trace rays for letterboxed pixels no draw ever covers; the compute
    // shaders also early-out any thread outside gSceneViewport.zw. The staged buffers/textures are
    // still sized to the FULL render target (rdStagedRowPitch_), because Stage B's i.pos.xy is a
    // render-target-space pixel centre, not a viewport-local one.
    const u32 dispatchW = curSceneViewport_[2] > 0.0f ? static_cast<u32>(curSceneViewport_[2]) : 0u;
    const u32 dispatchH = curSceneViewport_[3] > 0.0f ? static_cast<u32>(curSceneViewport_[3]) : 0u;
    const u32 gx = (dispatchW + 7u) / 8u;   // every staged compute stage declares [numthreads(8,8,1)]
    const u32 gy = (dispatchH + 7u) / 8u;

    // Row pitch rides viewParams.w for these uploads only, back to 0 after. Written here, not
    // prePass, because a prePass copy would predate beginShadowHistory refreshing
    // curSceneViewport_, so the two could disagree on a frame the view appeared or went away. No
    // other shader reads gViewParams.w (it was spare), so the single pass never sees a nonzero value.
    cb_.viewParams[3] = static_cast<f32>(rdStagedRowPitch_);

    // Local lights (lamps): decided here, before the first upload, since more than one stage reads
    // the count (CSRdGi/CSRdGiTrace leave a lamp's own emission out of ReSTIR's candidate hits while
    // non-zero, CSRdLocalLights and Stage B act on it) -- raised inconsistently, a lamp would count
    // twice or not at all. prePass zeroed both fields for every upload before this one; they stay as
    // decided here through the blended replay. Staged adds one requirement beyond the others:
    // CSRdLocalLights compiled.
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
    // The buffer overload S1/G1 need: same shape, uavBarrierBuffer instead of uavBarrierTexture,
    // since gRdShadowTiles/gRdGiCand are StructuredBuffers, not textures.
    auto stageEndBuffer = [&](rhi::BufferHandle written) {
        if (!perStage) return;
        ctx.uavBarrierBuffer(written);
        ctx.popMarker();
    };

    // reflSplit: sub-stage C (rayDrivenReflSplit), same shape as shadowTiles/giSplit below -- the
    // reflection dispatch's own condition plus the setting plus both new reflection pipelines. A
    // failed compile falls back to the unsplit CSRdRefl, never to the single-pass primary
    // (rdStagedActive() never inspects either member -- see their own header comment). Declared
    // here, outside the lighting-stages block below (unlike shadowTiles/giSplit, which stay
    // block-scoped since nothing outside it reads them), because R2 (CSRdReflFilter) has to run
    // after that block's barriers close, so reflSplit must outlive it.
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

    // T4 (rtGiHitShadowMap): the GI candidate's hit samples the GI-only shadow map from the compute
    // stages below (CSRdGiTrace, or CSRdGi when unsplit), but giShadowPass leaves it in ShaderResource
    // (the pixel-shader state voxelise reads it in). Visits NonPixelShaderResource for this group only,
    // straight back afterwards, so every pixel-shader reader (next frame's voxelise, the blended
    // replay) still finds it where it expects.
    const bool giHitShadowMap = settings_.rtGiHitShadowMap && giShadowTex_ != 0;
    if (giHitShadowMap)
        ctx.textureBarrier(giShadowTex_, rhi::ResourceState::ShaderResource,
                           rhi::ResourceState::NonPixelShaderResource);
    // Sun shadow history's read side (t6), same visit for the same reason: beginShadowHistory leaves
    // it in ShaderResource (PIXEL_SHADER_RESOURCE only), but CSRdShadow's reprojection and
    // CSRdLocalLights read it from COMPUTE (the state bug 21524cd3 fixed for the post chain). Back to
    // ShaderResource after the group. rtHistWriteIdx_, not the flip: endShadowHistory() already
    // flipped the index, so the side bound at t6 THIS frame is the one the index now names; the other
    // one is u2, in UnorderedAccess (same flip giNrmWrite below describes).
    const rhi::TextureHandle shadowHistRead = rtShadowHist_[rtHistWriteIdx_];
    if (shadowHistRead)
        ctx.textureBarrier(shadowHistRead, rhi::ResourceState::ShaderResource,
                           rhi::ResourceState::NonPixelShaderResource);
    // Local lights: t19, same visit/reason -- rests in ShaderResource for pixel-shader readers while
    // CSRdLocalLights reads it from compute. The read side is the half of the pair u19
    // (rdLocalOutThisFrame_) is not. Only when that dispatch runs; nothing else in this group reads t19.
    const rhi::TextureHandle localHistRead =
        localLights ? (rdLocalOutThisFrame_ == rdLocalHist_[0] ? rdLocalHist_[1] : rdLocalHist_[0]) : 0;
    if (localHistRead)
        ctx.textureBarrier(localHistRead, rhi::ResourceState::ShaderResource,
                           rhi::ResourceState::NonPixelShaderResource);
    {
        // Wraps every dispatch below -- see this function's comment on why no barrier or timestamp
        // sits between the four lighting stages (S1/G1 just below are the one exception).
        if (!perStage) ctx.pushMarker("Voxi RD lighting stages");

        // CSRdGi's own dispatch decision, hoisted above the shadow stage (it used to sit between the
        // shadow and GI dispatches): sub-stage split B's trace
        // pass (G1, below) needs giCb before the shadow stage runs, since G1 is recorded ahead of
        // CSRdShadow. CPU mirror of PSRayDriven's own GI-block condition and CSRdGi's own body gate --
        // ReSTIR GI is the chosen estimator AND the cone trace is gated on. Reads the SAME cb_ fields
        // the shader tests, never a separately-tracked flag, so the two
        // can't disagree. rdStagedActive() already refused this frame if the condition holds but
        // rdGiCsPso_ is 0, so reaching here true means the pipeline exists.
        const bool giDispatch = cb_.voxelParams[3] > 0.5f && cb_.giRestirParams[0] > 0.5f;
        // Half-rate ReSTIR GI via NRD's checkerboard, gated on three things beyond giDispatch: the
        // setting actually asking for it (rayDrivenStages == 2; rdStagedWanted() treats 1 and 2 alike,
        // so this is the one place that still tells them apart), the checkerboard variant having
        // compiled (optional on top of an already-optional pipeline), and this frame's NRD readback
        // being live (nrdGiRanThisFrame_) -- tracing only half the pixels is safe only when REBLUR has
        // something real to reconstruct the other half from.
        const bool giCb = giDispatch && settings_.rayDrivenStages == 2u && rdGiCbCsPso_ != 0 &&
                          nrdGiRanThisFrame_;
        // Recorded unconditionally -- false whenever giDispatch itself is false, since giCb already
        // implies it -- and consumed at the top of NEXT frame's beginShadowHistory to tell
        // that frame's NRD dispatch whether the GI input it's about to denoise was checkerboarded.
        giCbWrittenThisFrame_ = giCb;
        // One-shot logs, mutually exclusive (giCb requires rayDrivenStages == 2, the "why not" branch
        // only fires when rayDrivenStages == 2 and giCb is false).
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

        // ---- Sub-stage splits (rayDrivenShadowTiles / rayDrivenGiSplit) ----
        // shadowTiles: the setting plus both new shadow pipelines -- a failed compile falls back to
        // the unsplit CSRdShadow, never the single-pass primary (rdStagedActive() never inspects
        // these two members -- see their own header comment).
        const bool shadowTiles = settings_.rayDrivenShadowTiles && rdShadowProbeCsPso_ && rdShadowTiledCsPso_;
        // giSplit: giDispatch plus the setting plus whichever plain/checkerboard trace+split pair
        // this frame's giCb needs -- the checkerboard trace pass is useless without the checkerboard
        // split pass and vice versa, so both are required together.
        const bool giSplit = giDispatch && settings_.rayDrivenGiSplit &&
                              (giCb ? (rdGiTraceCbCsPso_ && rdGiSplitCbCsPso_)
                                    : (rdGiTraceCsPso_ && rdGiSplitCsPso_));
        // One-shot logs, same shape as the half-rate GI pair above.
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
        // G1: CSRdGiTrace, writing gRdGiCand (u17) -- compacted to the traced half's pixels alone in
        // checkerboard mode (giCb), the identical parity bit CSRdGi's own checkerboard dispatch below
        // carries. Copied here rather than shared: this upload happens strictly before that one, and
        // both restore the plain pitch immediately after so no later upload sees it.
        if (giSplit) {
            stageBegin("Voxi RD GI trace stage");
            ctx.setPipeline(giCb ? rdGiTraceCbCsPso_ : rdGiTraceCsPso_);
            ctx.setBindingSet(bindings_);
            ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
            ctx.setBindlessTable(rtTexTable_);
            u32 g1x = gx;
            if (giCb) {
                // Half the COLUMNS, rounded up, not half the threads: covers ceil(dispatchW/2)
                // columns of 8x8 groups, each thread handling one traced-half pixel (see
                // CSRdGiTrace's x = xBase + ((xBase ^ y ^ parity) & 1u) decode).
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
                // Bit 16 carries the checkerboard parity, for this one upload only. Every shader's
                // decode of gViewParams.w masks with & 0xFFFFu, so CSRdVisibility/CSRdShadow/
                // CSRdSkyOcc/CSRdRefl/Stage B below never see bit 16 -- the plain pitch is restored
                // immediately below.
                //
                // parity = nrdFrame_ & 1u: beginShadowHistory's fs.frameIndex = nrdFrame_++ earlier
                // this frame already post-incremented it once, so the current value IS the frameIndex
                // next frame's NRD dispatch will use to denoise this write -- by construction.
                //
                // Pitch always fits the low 16 bits: it's the render target's width, and D3D12 caps a
                // Texture2D at 16384 texels a side (the staged path is D3D12-only).
                cb_.viewParams[3] = static_cast<f32>(rdStagedRowPitch_ | ((nrdFrame_ & 1u) << 16));
                ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
                cb_.viewParams[3] = static_cast<f32>(rdStagedRowPitch_);   // restore before any later upload
            } else {
                ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
            }
            if (gx && gy) ctx.dispatch(gx, gy, 1);
            stageEnd(rdGiTex_);
        }

        // CSRdSkyOcc: CPU mirror of PSRayDriven's own sky-occlusion condition -- covers the cases
        // where rdAo would otherwise still read its initial 1.0 and rdAoGathered stays false (ReSTIR
        // supplied the diffuse term instead of the cone gather, or no voxel GI at all). Cone-GI mode's
        // own sky occlusion depends on the cone gather and stays in the shade pass.
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

        // CSRdRefl: CPU mirror of PSRayDriven's reflection-block condition's reflections-enabled half
        // (ray tracing active, geometry table built). The per-pixel roughness half of the shader's
        // gate (`s.rough <= 0.75`) has no CPU-side equivalent and stays inside CSRdRefl, which is why
        // gRdReflTex carries its own alpha decision for Stage B to read. Reads the SAME cb_ fields the
        // shader tests, never a separately-tracked flag; rdStagedActive() already refused this frame
        // if the condition holds but rdReflCsPso_ is 0, so reaching here means the pipeline exists.
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
    // Local lights: Stage B (and after it the blended replay's reuse) reads gRdLocalOut through u19
    // the way it reads u12 above, so it gets the same barrier -- conditional since u19 may be a
    // placeholder (or nothing real) on a frame without lamps, when neither reads it (the count is 0).
    if (localLights) ctx.uavBarrierTexture(rdLocalOutThisFrame_);
    // The GI surface-normal history (u8) has two writers: CSRdGi (hit pixels, inside
    // giRestirIndirect) and Stage B's own miss branch (sky sentinel). Texels are disjoint and the
    // barriers above already drain the GPU, but the order is stated here rather than left implicit.
    // 1 - rtHistWriteIdx_, not rtHistWriteIdx_: endShadowHistory() already flipped the index at the
    // end of prePass, so by scenePass the texture bound at u8 THIS frame is the other one. (Staged
    // mode requires shadowHistoryActive(), so that flip always happened.)
    const u32 giNrmWrite = 1u - rtHistWriteIdx_;
    if (cb_.voxelParams[3] > 0.5f && cb_.giRestirParams[0] > 0.5f && giSurfNrmHist_[giNrmWrite])
        ctx.uavBarrierTexture(giSurfNrmHist_[giNrmWrite]);

    // Sub-stage C, R2: CSRdReflFilter -- reruns rtReflectionSpatial against R1's gRtReflHistOut write
    // above and finishes the compose R1 deferred. Recorded outside the barrier-free "Voxi RD lighting
    // stages" group: R2 depends on R1's write to the SAME resource, not a disjoint one, so it needs a
    // real barrier and gets its own always-on span -- the shared marker is already closed by then,
    // the same reason "Voxi RD visibility" and "Voxi ray-driven primary" each open their own span too.
    if (reflSplit) {
        // 1u - rtHistWriteIdx_, not rtHistWriteIdx_: same reasoning as giNrmWrite above, for u3 --
        // endShadowHistory() already flipped the index, so by scenePass the reflection history
        // texture R1 wrote THIS frame is the other one. Barriers R1's write against R2's read of the
        // same resource through gRtReflHistOut (u3, voxi.hlsl).
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
    // setConstantBuffer copied the pitch at each call above, so it's spent. What stays, on a frame
    // CSRdGi wrote NRD's input checkerboarded, is bit 17 plus the parity in bit 16, for the REST of
    // this frame: scene draws after this one (PSMainVoxi's blended replay) read cb_ through
    // sceneConstants(), and giRestirIndirect's NRD-input write uses it to follow the packed layout
    // instead of landing on another pixel's texel. prePass zeroes it next frame.
    cb_.viewParams[3] = giCbWrittenThisFrame_
                      ? static_cast<f32>((1u << 17) | ((nrdFrame_ & 1u) << 16))
                      : 0.0f;
    // Local lights: cameraMedium z/w deliberately NOT reset here -- the blended replay lights its
    // panes with the same lamps, so its ReSTIR GI must drop a lamp's emission exactly as the opaque
    // stages did.

    // Bit 16, set last: tells the blended replay that gRdSunVisTex/gRdGiTex/gRdAoTex/gRdReflTex hold
    // THIS frame's values, so a translucent pixel on the opaque surface the staged passes already lit
    // may reuse them (Settings::blendedReuseStagedLighting). Self-clearing: this is the ONLY place
    // that sets the bit, and prePass already repacked giShadowParams[3] from scratch with it absent
    // (bits 1/2/4/8 only), so a frame that doesn't reach this line, because rdStagedActive() was
    // false, carries prePass's 0 straight through. Keep it that way -- the bit must never be set
    // anywhere else, or a frame that falls back from staged to single-pass mid-frame could leave it
    // on over textures nothing wrote this frame.
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

// Local lights: points t19/u19 back at the placeholder, THEN destroys the pair -- a descriptor must
// never outlive the texture it names (aver-view-outlives-its-buffer) -- and drops every flag
// describing its state: unprimed, a recreated pair rests in its creation state (ShaderResource, see
// ensureShadowHistory), no history trusted. Idempotent.
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
    // ReSTIR GI runs at ANY ray-tracing tier; sky occlusion is Medium and up
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
