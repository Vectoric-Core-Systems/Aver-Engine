// The inline pipeline batch: every request runs on the calling thread, through the factory's own
// synchronous create calls. Backends without worker-thread support and --sync-shaders use it, so the
// code that records a batch is one path whether the work is asynchronous or not.
#include "aver/rhi/RHIResources.hpp"

#include "aver/core/Log.hpp"

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace aver::rhi {
namespace {

std::atomic<bool> g_asyncAllowed{true};

// A ShaderDesc that owns its strings and bytecode. The big texts (source, prelude) are shared: a renderer
// hands the same few hundred KB to dozens of shaders.
struct OwnedShader {
    std::shared_ptr<const std::string> source, prelude;
    std::string entry, defines;
    bool hasEntry = false, hasDefines = false;
    std::vector<u8> bytes;
    ShaderStage stage = ShaderStage::Vertex;
    u32 minShaderModel = 60;
    ShaderHandle real = 0;
    bool failed = false;
};

class InlinePipelineBatch final : public IPipelineBatch {
public:
    explicit InlinePipelineBatch(IResourceFactory& f) : f_(f) {}

    ShaderHandle createShader(const ShaderDesc& d) override {
        OwnedShader s;
        s.source = intern(d.source);
        s.prelude = intern(d.prelude);
        if (d.entry)   { s.entry = d.entry;     s.hasEntry = true; }
        if (d.defines) { s.defines = d.defines; s.hasDefines = true; }
        if (d.precompiled()) s.bytes.assign(static_cast<const u8*>(d.bytecode),
                                            static_cast<const u8*>(d.bytecode) + d.bytecodeSize);
        s.stage = d.stage;
        s.minShaderModel = d.minShaderModel;
        shaders_.push_back(std::move(s));
        ++total_;
        return static_cast<ShaderHandle>(shaders_.size());
    }
    PipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& d) override {
        Pipe p;
        p.graphics = d;
        pipes_.push_back(p);
        ++total_;
        return static_cast<PipelineHandle>(pipes_.size());
    }
    PipelineHandle createComputePipeline(const ComputePipelineDesc& d) override {
        Pipe p;
        p.compute = true;
        p.cs = d;
        pipes_.push_back(p);
        ++total_;
        return static_cast<PipelineHandle>(pipes_.size());
    }

    void start() override {
        if (started_) return;
        started_ = true;
        // Pipelines in request order; a shader is made when its first pipeline needs it, and every shader
        // is freed at the end, as a scope of create/destroy calls would have.
        auto shaderOf = [&](ShaderHandle local) -> ShaderHandle {
            if (local == 0 || local > shaders_.size()) return 0;
            OwnedShader& s = shaders_[local - 1];
            if (s.real == 0 && !s.failed) {
                ShaderDesc d;
                d.stage = s.stage;
                d.minShaderModel = s.minShaderModel;
                if (s.source)  d.source = s.source->c_str();
                if (s.prelude) d.prelude = s.prelude->c_str();
                if (s.hasEntry)   d.entry = s.entry.c_str();
                if (s.hasDefines) d.defines = s.defines.c_str();
                if (!s.bytes.empty()) { d.bytecode = s.bytes.data(); d.bytecodeSize = s.bytes.size(); }
                s.real = f_.createShader(d);
                s.failed = (s.real == 0);
                ++done_;
            }
            return s.real;
        };
        for (Pipe& p : pipes_) {
            if (p.compute) {
                ComputePipelineDesc d = p.cs;
                d.cs = shaderOf(p.cs.cs);
                p.real = d.cs ? f_.createComputePipeline(d) : 0;
            } else {
                GraphicsPipelineDesc d = p.graphics;
                bool ok = true;
                for (ShaderHandle* h : {&d.vs, &d.gs, &d.ms, &d.ps, &d.as}) {
                    if (*h == 0) continue;
                    *h = shaderOf(*h);
                    ok = ok && *h != 0;
                }
                p.real = ok ? f_.createGraphicsPipeline(d) : 0;
            }
            ++done_;
        }
        for (OwnedShader& s : shaders_) {
            if (s.real) f_.destroyShader(s.real);
            s.real = 0;
        }
        // A shader no pipeline named still counts as done.
        done_ = total_;
        finished_.store(true, std::memory_order_release);
    }
    bool finished() const override { return finished_.load(std::memory_order_acquire); }
    void progress(u32& done, u32& total) const override {
        done = finished() ? total_ : done_;
        total = total_;
    }
    void waitFinished() override { if (!started_) start(); }
    void adopt() override { adopted_ = true; }
    PipelineHandle resolve(PipelineHandle local) const override {
        if (cancelled_ || local == 0 || local > pipes_.size()) return 0;
        return pipes_[local - 1].real;
    }
    void cancel() override {
        // Work already ran on the owner's thread; the pipelines are the factory's now. Give them back.
        if (cancelled_ || adopted_) return;
        cancelled_ = true;
        for (Pipe& p : pipes_) if (p.real) { f_.destroyPipeline(p.real); p.real = 0; }
    }
    ~InlinePipelineBatch() override { cancel(); }

private:
    std::shared_ptr<const std::string> intern(const char* text) {
        if (!text) return nullptr;
        const usize n = std::strlen(text);
        for (const std::shared_ptr<const std::string>& e : interned_)
            if (e->size() == n && std::memcmp(e->data(), text, n) == 0) return e;
        interned_.push_back(std::make_shared<const std::string>(text, n));
        return interned_.back();
    }
    struct Pipe {
        bool compute = false;
        GraphicsPipelineDesc graphics{};
        ComputePipelineDesc cs{};
        PipelineHandle real = 0;
    };
    IResourceFactory& f_;
    std::vector<OwnedShader> shaders_;
    std::vector<std::shared_ptr<const std::string>> interned_;
    std::vector<Pipe> pipes_;
    u32 total_ = 0, done_ = 0;
    bool started_ = false, cancelled_ = false, adopted_ = false;
    std::atomic<bool> finished_{false};
};

}  // namespace

std::unique_ptr<IPipelineBatch> createPipelineBatch(IResourceFactory& res, bool async) {
    if (async && asyncShaderBuildsAllowed())
        if (std::unique_ptr<IPipelineBatch> b = res.createPipelineBatchAsync()) return b;
    return std::make_unique<InlinePipelineBatch>(res);
}

void setAsyncShaderBuilds(bool allowed) {
    g_asyncAllowed.store(allowed, std::memory_order_relaxed);
    if (!allowed) AVER_INFO("[RHI] --sync-shaders: shader and pipeline creation stays on the calling thread");
}
bool asyncShaderBuildsAllowed() { return g_asyncAllowed.load(std::memory_order_relaxed); }

}  // namespace aver::rhi
