#pragma once
// A recording resource factory and device for the material residency tests: no GPU, handles are sequential, and
// every texture, binding-set write and destroy is kept for the test to read back.
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace aver::test {

struct MockFactory final : public rhi::IResourceFactory {
    std::vector<rhi::TextureDesc> textures;     // by handle - 1
    std::vector<std::string> names;             // debugName by handle - 1 (the desc's pointer may not outlive the call)
    std::set<rhi::TextureHandle> destroyed;
    std::map<std::pair<rhi::BindingSetHandle, u32>, rhi::TextureHandle> bound;   // (set, slot) -> texture last written
    u32 sets = 0;

    rhi::TextureHandle createTexture(const rhi::TextureDesc& d) override {
        textures.push_back(d);
        names.push_back(d.debugName ? d.debugName : "");
        textures.back().debugName = nullptr;
        return static_cast<rhi::TextureHandle>(textures.size());
    }
    const std::string& nameOf(rhi::TextureHandle h) const { static const std::string none; return h && h <= names.size() ? names[h - 1] : none; }
    const rhi::TextureDesc* descOf(rhi::TextureHandle h) const { return h && h <= textures.size() ? &textures[h - 1] : nullptr; }
    rhi::TextureHandle at(rhi::BindingSetHandle s, u32 slot) const {
        const auto it = bound.find({s, slot});
        return it == bound.end() ? 0 : it->second;
    }

    rhi::BufferHandle createBuffer(const rhi::BufferDesc&) override { return 1; }
    rhi::ShaderHandle createShader(const rhi::ShaderDesc&) override { return 1; }
    rhi::PipelineHandle createGraphicsPipeline(const rhi::GraphicsPipelineDesc&) override { return 1; }
    rhi::PipelineHandle createComputePipeline(const rhi::ComputePipelineDesc&) override { return 0; }
    rhi::BindlessTableHandle createBindlessTextureTable(u32) override { return 0; }
    void destroyBindlessTextureTable(rhi::BindlessTableHandle) override {}
    bool setBindlessTexture(rhi::BindlessTableHandle, u32, rhi::TextureHandle) override { return false; }
    u32  bindlessTableCapacity(rhi::BindlessTableHandle) const override { return 0; }
    rhi::BindingSetHandle createBindingSet(const rhi::BindingSetDesc&) override { return ++sets; }
    rhi::BlasHandle createBlas(rhi::MeshHandle) override { return 0; }
    rhi::TlasHandle createTlas(u32) override { return 0; }
    void destroyTexture(rhi::TextureHandle h) override { destroyed.insert(h); }
    void destroyBuffer(rhi::BufferHandle) override {}
    void destroyShader(rhi::ShaderHandle) override {}
    void destroyPipeline(rhi::PipelineHandle) override {}
    void destroyBindingSet(rhi::BindingSetHandle) override {}
    void setSrv(rhi::BindingSetHandle s, u32 slot, rhi::TextureHandle t, u32) override { bound[{s, slot}] = t; }
    void setUav(rhi::BindingSetHandle, u32, rhi::TextureHandle, u32) override {}
    void clearSrv(rhi::BindingSetHandle, u32) override {}
    void setSrvTlas(rhi::BindingSetHandle, u32, rhi::TlasHandle) override {}
    void setSrvBuffer(rhi::BindingSetHandle, u32, rhi::BufferHandle, u32, u32, u32) override {}
    void setUavBuffer(rhi::BindingSetHandle, u32, rhi::BufferHandle, u32, u32, u32) override {}
    bool writeBuffer(rhi::BufferHandle, const void*, u64, u64) override { return true; }
    bool readBuffer(rhi::BufferHandle, void*, u64, u64) override { return true; }
    bool textureInfo(rhi::TextureHandle, rhi::TextureDesc&) const override { return false; }
    void waitIdle() override {}
};

struct MockDevice final : public rhi::IDevice {
    MockFactory factory;
    rhi::Backend backend() const override { return rhi::Backend::Null; }
    const char* adapterName() const override { return "recording device"; }
    rhi::IResourceFactory* resources() override { return &factory; }
    rhi::ISwapchain* createSwapchain(const rhi::SwapchainDesc&) override { return nullptr; }
    void beginFrame() override {}
    void endFrame() override {}
};

} // namespace aver::test
