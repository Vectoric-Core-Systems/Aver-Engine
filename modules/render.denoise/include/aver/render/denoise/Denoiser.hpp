// Denoiser -- the engine's spatio-temporal denoiser for Voxi's two noisy ray-traced signals: the
// ReSTIR GI radiance and the sky-occlusion hit distance.
//
// THE FILTER IS AMD's, THE PLUMBING IS OURS. The shader passes are AMD FidelityFX Denoiser's
// reflection pipeline (third_party/fidelityfx-denoiser, MIT), driven at roughness 1 as a diffuse
// denoiser -- see shaders/aver_denoise.hlsl for why that pipeline and what the host callbacks
// pin. This class owns everything around it: the compute pipelines (three passes in a colour and a
// one-channel variant, plus the colour pre-exposure pass), the per-signal history textures, the G-buffer history copy, and every resource-state transition.
//
// RUNTIME-COMPILED HLSL through the engine's own shader compiler, like every other Voxi pass: no
// offline shader build, no precompiled bytecode, no register spaces. FidelityFX's headers are
// deployed under bin/shaders/FidelityFX by this module's CMakeLists.
//
// WHAT IT READS is LAST frame's G-buffer (view Z, motion vectors, normal) and LAST frame's noisy
// signal, because it records before this frame's scene pass -- the same one-frame lag every other
// temporal filter in Voxi has. The caller passes the states those textures rest in; record()
// transitions them for its own reads and puts them back.
//
// HISTORY IS PER SIGNAL. A signal that did not run last frame restarts its own history and leaves
// the other's alone, unlike a denoiser instance shared between the two.
#pragma once

#include <aver/core/Types.hpp>
#include <aver/rhi/RHI.hpp>
#include <aver/rhi/RHIResources.hpp>


namespace aver::render::denoise {

enum class Signal : u32 {
    Occlusion = 0,   // one channel: the sky-occlusion normalised hit distance
    Radiance  = 1,   // rgb: the ReSTIR GI indirect diffuse radiance
    Reflection = 2,  // rgb: Voxi's ray-traced reflection, a: its hit distance (cm)
};
inline constexpr u32 kSignalCount = 3;

class Denoiser {
public:
    Denoiser() = default;
    ~Denoiser();
    Denoiser(const Denoiser&)            = delete;
    Denoiser& operator=(const Denoiser&) = delete;

    // The engine textures it reads, with the states they rest in. G-buffer: render targets.
    // Signals: the textures Voxi's passes write. A null signal simply means that signal is not
    // denoised this frame; a null G-buffer input refuses the whole record().
    struct Inputs {
        rhi::TextureHandle viewZ           = 0;   // R32Float view-space linear depth
        rhi::TextureHandle motionVectors   = 0;   // RG16F pixels, destination minus source
        rhi::TextureHandle normalRoughness = 0;   // RGB10A2, averPackNormalRoughness (voxi.hlsl)
        rhi::ResourceState gbufferState    = rhi::ResourceState::RenderTarget;

        rhi::TextureHandle occlusion = 0;         // R16Unorm normalised hit distance
        rhi::TextureHandle radiance  = 0;         // RGBA16F, rgb linear radiance (a: hit distance, NRD)
        rhi::TextureHandle reflection = 0;        // RGBA16F, rgb linear radiance, a hit distance (cm)
        rhi::ResourceState signalState = rhi::ResourceState::UnorderedAccess;
        // Reflection only: the G-buffer frame's camera-relative inverse view-projection and position
        // (IDevice::camera), and the frame before it's view-projection (voxi.hlsl's gPrevViewProj).
        f32 invViewProjRel[16] = {};
        f32 prevViewProj[16] = {};
        f32 camPos[3]        = {};
        f32 viewport[4]      = {};   // scene viewport x y w h, pixels
        f32 prevViewport[4]  = {};
    };

    // Per-frame choices that are not textures.
    struct Frame {
        bool runOcclusion = false;
        bool runRadiance  = false;
        bool runReflection = false;
        // Every signal's history is discarded this frame (a camera cut, a mode switch, a resize).
        bool resetHistory = false;
        // The radiance input was traced at half rate: only pixels with ((x ^ y ^ parity) & 1) == 0
        // hold this frame's value, the rest are reconstructed from their four neighbours.
        bool radianceHalfRate       = false;
        u32  radianceHalfRateParity = 0;
    };

    // FidelityFX's two dials. Safe to change any frame.
    struct Tuning {
        u32 maxSamples        = 32;     // cap on the accumulated sample count (history length)
        f32 historyClipWeight = 4.0f;   // width of the neighbourhood clip applied to the history
    };

    // Compiles the pipelines. False -- said once at WARN -- when a FidelityFX shader will not compile
    // or a pipeline will not build; the caller then runs undenoised.
    bool create(rhi::IDevice& dev);
    void destroy();
    [[nodiscard]] bool valid() const { return pipelines_[0] != 0; }

    // Allocates the history and scratch textures for this resolution. Idempotent at an unchanged
    // size; a real change discards every history.
    bool resize(u32 width, u32 height);
    [[nodiscard]] u32 width() const { return width_; }
    [[nodiscard]] u32 height() const { return height_; }
    // Discards every signal's history on the next record().
    void forceHistoryReset() { for (bool& s : stale_) s = true; }

    void setTuning(const Tuning& t) { tuning_ = t; }

    // Records the passes for the signals `frame` selects. False when nothing ran (invalid, not
    // sized, a null G-buffer input, or no signal selected and present).
    bool record(rhi::IRenderContext& ctx, const Frame& frame, const Inputs& in);

    // The denoised result, resting in ShaderResource. Zero when that signal did not run in the
    // last record() -- a caller must read zero as "not denoised this frame".
    [[nodiscard]] rhi::TextureHandle output(Signal s) const { return output_[static_cast<u32>(s)]; }

private:
    // Scale (colour only) records first; each number matches the shader's AVER_DNSR_PASS.
    enum Pass : u32 { Reproject = 0, Prefilter = 1, Resolve = 2, Scale = 3, kPassCount = 4 };

    // Everything one signal keeps. Pairs ping-pong by `parity`: [parity] is written this frame,
    // [1 - parity] holds last frame's.
    struct SignalTargets {
        rhi::TextureHandle history[2]     = {};   // the denoised result = next frame's history
        rhi::TextureHandle varHistory[2]  = {};   // resolve's variance = next frame's
        rhi::TextureHandle sampleCount[2] = {};   // reproject's accumulated sample count
        rhi::TextureHandle reprojected    = 0;    // last frame's result at this frame's pixels
        rhi::TextureHandle average        = 0;    // 1/8-resolution mean of the noisy input
        rhi::TextureHandle variance       = 0;    // reproject's temporal variance
        rhi::TextureHandle prefiltered    = 0;    // the spatially filtered input
        rhi::TextureHandle prefilteredVar = 0;
        rhi::TextureHandle scale          = 0;    // 1x1 pre-exposure scale (colour only)
        rhi::BindingSetHandle sets[kPassCount] = {};
        // history[] alone changes resting state: the one written this frame is left pixel-readable
        // for Voxi (ShaderResource); every other target rests in NonPixelShaderResource.
        rhi::ResourceState historyState[2] = {rhi::ResourceState::NonPixelShaderResource,
                                              rhi::ResourceState::NonPixelShaderResource};
        u32  parity = 0;
    };

    bool recordSignal(rhi::IRenderContext& ctx, u32 signal, rhi::TextureHandle input,
                      const Inputs& in, u32 flags);
    void releaseTargets();

    rhi::IDevice*          dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    // [pass * kVariants + v]. Per pass: colour (GI), one channel (sky occlusion), reflection (FidelityFX's own reflection setup).
    static constexpr u32 kVariants = 3;
    rhi::PipelineHandle pipelines_[kPassCount * kVariants] = {};

    SignalTargets      sig_[kSignalCount];
    rhi::TextureHandle depthHistory_  = 0;   // last frame's G-buffer view Z, for disocclusion
    rhi::TextureHandle normalHistory_ = 0;   // last frame's G-buffer normal
    rhi::TextureHandle output_[kSignalCount] = {};
    bool ranLast_[kSignalCount] = {};
    bool stale_[kSignalCount]   = {true, true, true};

    Tuning tuning_{};
    u32  width_ = 0, height_ = 0;
    u32  failedWidth_ = 0, failedHeight_ = 0;   // the size whose targets would not allocate
};

}  // namespace aver::render::denoise
