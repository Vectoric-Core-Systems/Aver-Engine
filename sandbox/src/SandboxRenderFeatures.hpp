// The editor-owned render features (G-buffer debug view, NeuraFI visualisation overlay) that SandboxApp registers.
// Included from SandboxApp.hpp (inside namespace aver) after the engine headers; not a standalone header.
#pragma once

// G-buffer debug view: visualise unsampled buffers via overlayPass after tonemap.
static const char* gbufferDebugShaderSource();

class GBufferDebugFeature final : public rhi::IRenderFeature {
public:
    enum class Mode : u32 { Off = 0, Velocity = 1, ViewZ = 2, NormalRoughness = 3 };

    // Velocity debug saturation (texels/frame).
    static constexpr f32 kVelocityFullScaleTexels = 8.0f;
    // ViewZ debug white distance (cm).
    static constexpr f32 kViewZDebugFarUnits = 5000.0f;

    const char* name() const override { return "GBufferDebug"; }

    // Device borrowed (outlives feature for run).
    void setDevice(rhi::IDevice* dev) { device_ = dev; }
    void setMode(Mode m) { mode_ = m; }
    // 3D viewport rect (not whole window, to avoid chrome).
    void setViewportRect(u32 x, u32 y, u32 w, u32 h) { vpX_ = x; vpY_ = y; vpW_ = w; vpH_ = h; }

    ~GBufferDebugFeature() override { releaseGpu(); }

    void overlayPass(rhi::IRenderContext& ctx, u32 width, u32 height) override {
        if (mode_ == Mode::Off || !device_ || vpW_ == 0 || vpH_ == 0) return;
        const rhi::TextureHandle tex = textureForMode(mode_);
        if (!tex) {
            // 0 is the documented "nothing here" answer (RHI.hpp) when gBufferEnabled() is false or
            // this backend hasn't implemented the accessor -- an honest no-op, so warn once only.
            if (!warnedMissing_) {
                AVER_WARN("[GBufferDebug] mode {} selected but the device returned no texture for it "
                          "-- is the G-buffer actually enabled, and does this backend implement it yet?",
                          static_cast<u32>(mode_));
                warnedMissing_ = true;
            }
            return;
        }
        if (!ensurePipeline()) return;
        if (tex != boundTex_) { res_->setSrv(binding_, 0, tex); boundTex_ = tex; }

        // Constant buffer: mode, velocity scale, viewZ far, unused.
        const f32 cb[4] = { static_cast<f32>(static_cast<u32>(mode_)), kVelocityFullScaleTexels,
                            kViewZDebugFarUnits, 0.0f };
        // Narrow to 3D viewport only.
        ctx.setViewport(vpX_, vpY_, vpW_, vpH_);
        ctx.setScissor(vpX_, vpY_, vpW_, vpH_);
        ctx.setPipeline(pipeline_);
        ctx.setBindingSet(binding_);
        ctx.setConstantBuffer(kGBufferDebugConstantRegister, cb, sizeof(cb));
        ctx.drawFullscreen();
        // Restore to full backbuffer.
        ctx.setViewport(0, 0, width, height);
        ctx.setScissor(0, 0, width, height);
    }

    // Release GPU resources (idempotent).
    void shutdown() { releaseGpu(); }

private:
    rhi::TextureHandle textureForMode(Mode m) const {
        switch (m) {
            case Mode::Velocity:        return device_->gBufferVelocityTexture();
            case Mode::ViewZ:           return device_->gBufferViewZTexture();
            case Mode::NormalRoughness: return device_->gBufferNormalRoughnessTexture();
            default:                    return 0;
        }
    }

    // Lazy pipeline creation (fullscreen-triangle, G-buffer SRVs).
    bool ensurePipeline() {
        if (pipeline_) return true;
        if (!device_) return false;
        rhi::IResourceFactory* res = device_->resources();
        if (!res) return false;
        res_ = res;

        const char* src = gbufferDebugShaderSource();
        rhi::ShaderDesc vsd; vsd.source = src; vsd.entry = "VSGBufferDebug";
        vsd.stage = rhi::ShaderStage::Vertex; vsd.minShaderModel = 51;
        rhi::ShaderDesc psd; psd.source = src; psd.entry = "PSGBufferDebug";
        psd.stage = rhi::ShaderStage::Pixel; psd.minShaderModel = 51;
        const rhi::ShaderHandle vs = res->createShader(vsd);
        const rhi::ShaderHandle ps = res->createShader(psd);

        bool ok = false;
        if (vs && ps) {
            rhi::GraphicsPipelineDesc pd;
            pd.vs = vs; pd.ps = ps;
            pd.layout.srvCount = 1;
            pd.layout.samplers[0] = rhi::SamplerDesc{rhi::Filter::Linear, rhi::AddressMode::Clamp};
            pd.layout.samplerCount = 1;
            pd.cull = rhi::CullMode::None;
            pd.depthClip = false;
            pd.renderTargetCount = 1;
            // The display target (tonemapped composite), NOT device_->backbufferFormat() (the HDR scene).
            pd.renderTargets[0] = device_ ? device_->displayFormat() : rhi::Format::RGBA8Unorm;
            pd.sampleCount = 1;   // Already-resolved composite.
            pipeline_ = res->createGraphicsPipeline(pd);
            ok = pipeline_ != 0;
        }
        if (vs) res->destroyShader(vs);
        if (ps) res->destroyShader(ps);
        if (!ok) { AVER_ERROR("[GBufferDebug] pipeline unavailable"); return false; }

        rhi::BindingSetDesc bd;
        bd.srvCount = 1;
        bd.srvKinds[0] = rhi::SlotKind::Texture2D;
        binding_ = res->createBindingSet(bd);
        if (!binding_) {
            AVER_ERROR("[GBufferDebug] binding set unavailable");
            res->destroyPipeline(pipeline_);
            pipeline_ = 0;
            return false;
        }
        return true;
    }

    void releaseGpu() {
        if (res_) {
            if (pipeline_) res_->destroyPipeline(pipeline_);
            if (binding_)  res_->destroyBindingSet(binding_);
        }
        pipeline_ = 0; binding_ = 0; boundTex_ = 0;
    }

    // Own CBV register (no shared prelude).
    static constexpr u32 kGBufferDebugConstantRegister = 1;

    rhi::IDevice*          device_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    Mode mode_ = Mode::Off;
    u32 vpX_ = 0, vpY_ = 0, vpW_ = 0, vpH_ = 0;
    rhi::PipelineHandle   pipeline_ = 0;
    rhi::BindingSetHandle binding_  = 0;
    rhi::TextureHandle    boundTex_ = 0;
    bool warnedMissing_ = false;
};

// GBuffer debug shader (fullscreen-triangle, own constants).
static inline const char* gbufferDebugShaderSource() {
    // Keyed on shaderFileRevision(), not a plain static -- the loader owns the cache.
    static std::string s;
    static u64 built = ~0ull;
    if (built != rhi::shaderFileRevision()) {
        s = rhi::shaderFile("gbuffer_debug.hlsl");
        built = rhi::shaderFileRevision();
    }
    return s.c_str();
}

// NeuraFI visualisation overlay (over 3D viewport, post-tonemap, alpha-blended).
class NeuraFiVizFeature final : public rhi::IRenderFeature {
public:
    const char* name() const override { return "NeuraFiViz"; }
    ~NeuraFiVizFeature() override { releaseGpu(); }

    void setDevice(rhi::IDevice* dev) { device_ = dev; }
    // Set source, vizCount baseline, opacity (called pre-render).
    void setSource(const aver::neurafi::NeuraFI* src, f32 opacity) {
        src_ = src;
        baseline_ = src ? src->vizCount() : 0;
        opacity_ = opacity;
    }
    void setViewportRect(u32 x, u32 y, u32 w, u32 h) { vpX_ = x; vpY_ = y; vpW_ = w; vpH_ = h; }

    void overlayPass(rhi::IRenderContext& ctx, u32 width, u32 height) override {
        if (!src_ || !device_ || vpW_ == 0 || vpH_ == 0 || src_->vizCount() == baseline_) return;
        const rhi::TextureHandle tex = src_->visualisation();
        if (!tex || !ensurePipeline()) return;
        if (tex != boundTex_) { res_->setSrv(binding_, 0, tex); boundTex_ = tex; }
        const f32 cb[4] = {opacity_, 0.0f, 0.0f, 0.0f};
        ctx.setViewport(vpX_, vpY_, vpW_, vpH_);
        ctx.setScissor(vpX_, vpY_, vpW_, vpH_);
        ctx.setPipeline(pipeline_);
        ctx.setBindingSet(binding_);
        ctx.setConstantBuffer(kConstantRegister, cb, sizeof(cb));
        ctx.drawFullscreen();
        ctx.setViewport(0, 0, width, height);
        ctx.setScissor(0, 0, width, height);
    }

    void shutdown() { releaseGpu(); }

private:
    bool ensurePipeline() {
        if (pipeline_) return true;
        if (failed_ || !device_) return false;
        rhi::IResourceFactory* res = device_->resources();
        if (!res) return false;
        res_ = res;
        const std::string& src = rhi::shaderFile("neural_visualiser.hlsl");
        if (src.empty()) {
            AVER_ERROR("[NeuralViz] neural_visualiser.hlsl is not deployed beside the executable");
            failed_ = true;
            return false;
        }
        rhi::ShaderDesc vsd; vsd.source = src.c_str(); vsd.entry = "VSNeuralViz";
        vsd.stage = rhi::ShaderStage::Vertex; vsd.minShaderModel = 51;
        rhi::ShaderDesc psd; psd.source = src.c_str(); psd.entry = "PSNeuralViz";
        psd.stage = rhi::ShaderStage::Pixel; psd.minShaderModel = 51;
        const rhi::ShaderHandle vs = res->createShader(vsd);
        const rhi::ShaderHandle ps = res->createShader(psd);
        if (vs && ps) {
            rhi::GraphicsPipelineDesc pd;
            pd.vs = vs; pd.ps = ps;
            pd.layout.srvCount = 1;
            pd.layout.samplers[0] = rhi::SamplerDesc{rhi::Filter::Linear, rhi::AddressMode::Clamp};
            pd.layout.samplerCount = 1;
            pd.cull = rhi::CullMode::None;
            pd.depthClip = false;
            pd.renderTargetCount = 1;
            pd.renderTargets[0] = device_ ? device_->displayFormat() : rhi::Format::RGBA8Unorm;   // the display target
            pd.sampleCount = 1;
            pd.blend = rhi::BlendMode::AlphaBlend;
            pipeline_ = res->createGraphicsPipeline(pd);
        }
        if (vs) res->destroyShader(vs);
        if (ps) res->destroyShader(ps);
        if (pipeline_) {
            rhi::BindingSetDesc bd;
            bd.srvCount = 1;
            bd.srvKinds[0] = rhi::SlotKind::Texture2D;
            binding_ = res->createBindingSet(bd);
        }
        if (!pipeline_ || !binding_) {
            AVER_ERROR("[NeuralViz] pipeline unavailable");
            releaseGpu();
            failed_ = true;
            return false;
        }
        return true;
    }

    void releaseGpu() {
        if (res_) {
            if (pipeline_) res_->destroyPipeline(pipeline_);
            if (binding_)  res_->destroyBindingSet(binding_);
        }
        pipeline_ = 0; binding_ = 0; boundTex_ = 0;
    }

    static constexpr u32 kConstantRegister = 1;   // Own constant register.

    rhi::IDevice*          device_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    const aver::neurafi::NeuraFI* src_ = nullptr;
    u64 baseline_ = 0;
    f32 opacity_ = 1.0f;
    u32 vpX_ = 0, vpY_ = 0, vpW_ = 0, vpH_ = 0;
    rhi::PipelineHandle   pipeline_ = 0;
    rhi::BindingSetHandle binding_ = 0;
    rhi::TextureHandle    boundTex_ = 0;
    bool failed_ = false;
};
