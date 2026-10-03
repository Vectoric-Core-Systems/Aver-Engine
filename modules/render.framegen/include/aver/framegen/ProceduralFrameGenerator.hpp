// ProceduralFrameGenerator -- frame interpolation milestone 1 (docs/rendering/FRAME_INTERPOLATION.md):
// the rhi::IFrameGenerator the hosts install on the device. Procedural only; no network.
//
// WHAT IT OWNS: the previous real frame (colour, motion, view depth -- copied after every generate()),
// the generated image and a scratch image for the fill, and two compute pipelines built from
// shaders/framegen.hlsl (CSFgGather, CSFgFill). Everything is scene resolution and recreated on a size
// change, which also forgets the previous frame.
//
// Backend-agnostic: only the generic RHI (createTexture/copyTexture/dispatch). The D3D12 device is the
// first caller; Vulkan uses the same object when its present loop learns two presents per frame.
//
// NOT THREAD-SAFE; generate() records into the context the device is recording the frame with.
#pragma once

#include "aver/core/Types.hpp"
#include "aver/rhi/RHIResources.hpp"

namespace aver::framegen {

class ProceduralFrameGenerator final : public rhi::IFrameGenerator {
public:
    explicit ProceduralFrameGenerator(rhi::IResourceFactory& res) : res_(res) {}
    ~ProceduralFrameGenerator() override;
    ProceduralFrameGenerator(const ProceduralFrameGenerator&)            = delete;
    ProceduralFrameGenerator& operator=(const ProceduralFrameGenerator&) = delete;

    const char* name() const override { return "Aver procedural interpolation"; }
    rhi::TextureHandle generate(rhi::IRenderContext& ctx, const rhi::FrameGenInput& in) override;
    void reset() override { historyValid_ = false; }

private:
    bool ensurePipelines();
    bool ensureTargets(u32 w, u32 h);   // true = usable; recreating forgets the previous frame
    void releaseTargets();
    void bindInputs(const rhi::FrameGenInput& in);

    rhi::IResourceFactory& res_;
    bool pipelinesFailed_ = false;   // said once; generate() then returns 0 for good
    rhi::PipelineHandle gatherPso_ = 0, fillPso_ = 0;

    u32 w_ = 0, h_ = 0;
    // The previous real frame. Rest in NonPixelShaderResource (read by CSFgGather).
    rhi::TextureHandle histColor_ = 0, histVel_ = 0, histZ_ = 0;
    // The generated image and the fill's ping-pong partner. Rest in ShaderResource.
    rhi::TextureHandle out_ = 0, tmp_ = 0;
    // gather: t0-t2 frame N, t3-t5 the previous frame, u0 out_. fillA: out_ -> tmp_; fillB: tmp_ -> out_.
    rhi::BindingSetHandle gatherSet_ = 0, fillSetA_ = 0, fillSetB_ = 0;
    rhi::TextureHandle boundColor_ = 0, boundVel_ = 0, boundZ_ = 0;   // what gatherSet_'s t0-t2 hold
    bool historyValid_ = false;
};

}  // namespace aver::framegen
