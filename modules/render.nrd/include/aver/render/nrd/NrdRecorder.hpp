// NrdRecorder -- the half of the NRD integration that touches the GPU.
//
// NrdDenoiser.hpp is the translation layer: it asks NRD what to do and hands back a plan -- a list
// of compute dispatches, each naming a pipeline, a set of slots and a blob of constants. Its own
// header closes with "nothing records these dispatches", and this is that thing. Given a device it
// creates the pipelines from NRD's DXIL, allocates the texture pools NRD asks for, and turns a
// frame's plan into recorded work.
//
// D3D12 ONLY, AND THAT IS A DESIGN DECISION RATHER THAN A GAP LEFT OPEN. An HLSL register space is
// a Vulkan DESCRIPTOR SET, and NRD wants its constant buffer and samplers in space 1 while its
// resources stay in space 0. VulkanResourceFactory::descriptorLayout deliberately REFUSES a
// non-zero space and says why: honouring one means building a different set layout AND binding it
// at a different index in VulkanRenderContext, which hard-codes kVkSetConstants/kVkSetSamplers at
// every bind site -- and wiring the first half without the second produces correctly-built
// descriptors bound at the wrong set, which is not a validation error, not a crash, just a shader
// reading the wrong memory. create() therefore reports failure on any other backend, by name, and
// the caller is expected to carry on undenoised rather than to treat that as fatal.
//
// WHY THE POOLS ARE OURS AND NOT NRD'S. NRD allocates nothing; it describes what it needs
// (InstanceLayout's permanentPool/transientPool, each entry a format and a downsample factor) and
// then refers to those textures by index for the rest of its life. Permanent textures carry NRD's
// own temporal history between frames and must survive; transient ones are scratch within a frame.
// Both are recreated on resize, which is also when NRD's history becomes meaningless -- see
// FrameSettings::resetHistory, which the caller must set on the frame after a resize.
#pragma once

#include "aver/render/nrd/NrdDenoiser.hpp"

#include <aver/rhi/RHI.hpp>
#include <aver/rhi/RHIResources.hpp>

#include <vector>

namespace aver::render::nrd {

class Recorder {
public:
    Recorder() = default;
    ~Recorder();
    Recorder(const Recorder&)            = delete;
    Recorder& operator=(const Recorder&) = delete;

    // The engine textures NRD reads. Every one is required by REBLUR; a null handle here is a
    // caller bug rather than an optional feature, and record() refuses rather than binding null.
    //
    // NORMAL/ROUGHNESS PACKING IS NOT FREE-FORM: NRD's shaders were BUILT with a specific encoding
    // (cmake/AverNRD.cmake chooses it, Denoiser::encodings reports it), and a mismatch produces a
    // plausible-looking wrong image rather than an error. Check encodings() against what the
    // G-buffer actually writes before trusting the output of this pass.
    struct Inputs {
        rhi::TextureHandle viewZ            = 0;   // IN_VIEWZ, view-space linear depth
        rhi::TextureHandle motionVectors    = 0;   // IN_MV
        rhi::TextureHandle normalRoughness  = 0;   // IN_NORMAL_ROUGHNESS, packed to NRD's encoding
        rhi::TextureHandle diffuseHitDist   = 0;   // IN_DIFF_HITDIST
        // IN_DIFF_RADIANCE_HITDIST, for ReblurDiffuse. rgb = diffuse radiance, a = NORMALISED hit
        // distance. OPTIONAL in a way the four above are not: a caller running only the occlusion
        // denoiser never produces one, and record() only refuses a null here when the plan it was
        // handed actually asks for the slot -- which is exactly what selecting denoisers by index
        // is for.
        rhi::TextureHandle diffuseRadianceHitDist = 0;
    };

    // Creates the NRD instance, its pipelines and nothing size-dependent. Returns false -- and says
    // why at WARN -- when NRD is absent from the build, when the backend is not D3D12, or when a
    // pipeline will not build. False is a degrade, not a fatal: the caller keeps its own filter.
    bool create(rhi::IDevice& dev, const DenoiserKind* kinds, u32 count);
    void destroy();
    [[nodiscard]] bool valid() const { return !pipelines_.empty(); }

    // Allocates (or reallocates) the texture pools and the per-dispatch binding sets for this
    // resolution. Idempotent: calling it with an unchanged size does nothing and keeps NRD's
    // history. A real change throws the history away, so the next record() must pass
    // FrameSettings::resetHistory = true -- historyIsStale() says when.
    bool resize(u32 width, u32 height);
    [[nodiscard]] bool historyIsStale() const { return historyStale_; }
    // Forces the NEXT record() to pass FrameSettings::resetHistory = true, without a resize --
    // exactly what resize() and destroy() already do to this same flag when they actually have a
    // reason to (a genuine size change or teardown); this is the on-demand equivalent for
    // VoxiRenderer::resetNrdHistory's own console command. Cleared automatically once that record()
    // succeeds (see record()'s own historyStale_ = false).
    void forceHistoryReset() { historyStale_ = true; }

    // Per-denoiser REBLUR tuning -- see Denoiser::ReblurTuning, which documents why the caller has to
    // supply the hit-distance constants rather than accept NRD's. NRD keeps these until they are set
    // again, so once after create() is enough; `denoiserIndex` indexes the kinds create() was given.
    bool setReblurTuning(u32 denoiserIndex, const Denoiser::ReblurTuning& s) {
        return denoiser_.setReblurTuning(denoiserIndex, s);
    }

    // Plans and records one frame. `denoiserIndices` selects which of the denoisers passed to
    // create() run this frame, by the same index. Clears historyIsStale() on success.
    bool record(rhi::IRenderContext& ctx, const FrameSettings& settings, const Inputs& in,
                const u32* denoiserIndices, u32 indexCount);

    // The filtered result, for the shader that consumes it. Valid after a successful record().
    // Zero until then -- a caller must treat zero as "not denoised this frame" rather than assuming
    // the texture merely holds stale data, because on the first frame it holds nothing at all.
    [[nodiscard]] rhi::TextureHandle outputDiffuseHitDistance() const { return outDiffHitDist_; }

    // The denoised diffuse RADIANCE (rgb) and hit distance (a), from ReblurDiffuse. Zero on any
    // frame that denoiser did not run, for the same reason and with the same contract as the
    // occlusion output above.
    [[nodiscard]] rhi::TextureHandle outputDiffuseRadianceHitDistance() const { return outDiffRadHitDist_; }

private:
    // One dispatch's descriptor table. Created once per plan slot and rewritten before ANY dispatch
    // is recorded, never between them -- see record() for why that ordering is load-bearing.
    struct Slot {
        rhi::BindingSetHandle set = 0;
        u32 srvCount = 0;
        u32 uavCount = 0;
    };

    rhi::TextureHandle poolTexture(SlotRole role, u32 index, const Inputs& in) const;
    void               releasePools();

    rhi::IDevice*          dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    Denoiser               denoiser_;

    std::vector<rhi::PipelineHandle> pipelines_;      // one per InstanceLayout::pipelines
    std::vector<rhi::TextureHandle>  permanent_;
    std::vector<rhi::TextureHandle>  transient_;
    std::vector<Slot>                slots_;          // one per dispatch in the plan

    // THE OUT_* SLOTS ARE OURS TO ALLOCATE, NOT NRD'S TO HAND BACK, and getting that backwards is
    // what removed the device. NRD's own reference integration resolves a dispatch's resources three
    // ways (Integration/NRDIntegration.hpp, _Dispatch): TRANSIENT_POOL and PERMANENT_POOL index its
    // pools by ResourceDesc::indexInPool, and EVERY OTHER ResourceType -- every IN_* and every
    // OUT_* -- is looked up by the resource TYPE in a snapshot the application filled in. For those
    // types indexInPool is never written and stays 0, so reading it as a pool index silently aliases
    // the output onto permanent pool texture 0, which for a REBLUR instance is its PREV_VIEWZ
    // history (R32_SFLOAT, Source/Reblur.cpp).
    //
    // Two separate consequences followed, and only the second one was visible. The occlusion
    // denoiser's output pass is compiled RWTexture2D<float> (REBLUR_Config.hlsli's REBLUR_TYPE under
    // NRD_MODE_OCCLUSION), so a single-channel write into a single-channel R32_SFLOAT view did not
    // fault -- it quietly corrupted NRD's own depth history and ran that way for as long as this
    // pass has existed. REBLUR_DIFFUSE's output pass is compiled RWTexture2D<float4>, and
    // D3D12ResourceFactory::setUav builds the descriptor from the bound texture's OWN format with no
    // override, so that one bound a four-component shader to a one-component UAV: DEVICE_REMOVED on
    // the first recorded frame, which is exactly where the bisection landed.
    //
    // FORMATS ARE NRD'S, NOT A CHOICE MADE HERE. Source/Reblur.cpp names REBLUR_FORMAT_OCCLUSION
    // (R16_UNORM) for the occlusion result and REBLUR_FORMAT (RGBA16_SFLOAT, ".xyz - color, .w -
    // normalized hit distance") for the radiance result, and those are the element types its shaders
    // were compiled against. Allocated only for the denoisers create() was actually given, because
    // the radiance target is 8 bytes a pixel at full resolution and an occlusion-only caller has no
    // use for it.
    rhi::TextureHandle outDiffHitDistTex_    = 0;   // OUT_DIFF_HITDIST, R16Unorm
    rhi::TextureHandle outDiffRadHitDistTex_ = 0;   // OUT_DIFF_RADIANCE_HITDIST, RGBA16F

    // ---- IN_MV IS AN OUTPUT TOO, WHICH NRD DOES NOT DOCUMENT ANYWHERE ----
    //
    // REBLUR_DIFFUSE's "Temporal stabilization" pass ends with PushOutput(AsUint(ResourceType::IN_MV))
    // (Source/Denoisers/Reblur_Diffuse.hpp). NRD reuses the motion-vector texture as scratch and
    // writes it through a UAV, and nothing in NRDDescs.h, NRDSettings.h or the README says so -- IN_MV
    // is listed under "NON-NOISY INPUTS" with no hint that it is written. REBLUR_DIFFUSE_OCCLUSION has
    // no stabilization pass and so never does this, which is exactly why the occlusion denoiser ran for
    // as long as it did while adding the diffuse one removed the adapter on its first recorded frame.
    //
    // TWO REASONS THIS IS A COPY RATHER THAN JUST A UAV FLAG ON THE ENGINE'S TEXTURE. The first is
    // mechanical: the G-buffer's velocity target is a RENDER TARGET, created without
    // D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, and CreateUnorderedAccessView on such a resource is
    // not a call that fails -- D3D12 calls RemoveDevice with DXGI_ERROR_INVALID_CALL and the whole
    // adapter goes. The second is the one that matters more: the engine's motion vectors have other
    // readers (the upscaler among them), and letting a denoiser scribble on a buffer the rest of the
    // frame depends on would be a coupling no caller could see. NRD gets its own, and the engine's
    // stays exactly what the G-buffer wrote.
    rhi::TextureHandle mvScratch_ = 0;              // IN_MV, ours: same format, plus UnorderedAccess
    bool wantDiffHitDist_    = false;               // a ReblurDiffuseOcclusion was requested
    bool wantDiffRadHitDist_ = false;               // a ReblurDiffuse was requested

    rhi::TextureHandle outDiffHitDist_ = 0;
    rhi::TextureHandle outDiffRadHitDist_ = 0;
    u32  width_ = 0, height_ = 0;
    u32  failedWidth_ = 0, failedHeight_ = 0;   // the size whose pools would not allocate
    bool historyStale_ = true;
    u32  loggedPlanSize_ = 0;   // so the plan is described once, not every frame
};

}  // namespace aver::render::nrd
