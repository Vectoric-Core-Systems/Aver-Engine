// UiRenderer's world-text half: the labels the draw list carries, placed for the camera and drawn in the scene's transparent
// pass with world_text.hlsl. See UiRenderer.hpp.
//
// SAFETY, because this pass draws inside the scene. It uses its own shaders and pipelines (plain raster: one texture sample,
// no ray query, no loop, no derivative-dependent branch); it never touches the scene's material or voxel pipelines. Every
// index is checked against the vertices it names and every page against the world atlas before it reaches a buffer; the
// buffers are capped by the draw list's own caps (kUiMax*), so no frame can ask for more than a few megabytes.
#include "aver/render/ui/UiRenderer.hpp"

#include "aver/core/Log.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <algorithm>
#include <cstring>

namespace aver::render::ui {
namespace {

constexpr u32 kWorldConstantSlot = 3;     // the one slot a feature may claim
constexpr u32 kWorldConstantDwords = 20;  // gPVP (16) + gParams (4)
constexpr u32 kInitialWorldVertices = 2048;
constexpr u32 kInitialWorldIndices = 3072;

u32 pipelineSlot(u32 flags) {
    return ((flags & aver::ui::kWorldTextNoDepth) ? 1u : 0u) | ((flags & aver::ui::kWorldTextAdditive) ? 2u : 0u) |
           ((flags & aver::ui::kWorldTextOneSided) ? 4u : 0u);
}

} // namespace

void UiRenderer::destroyWorldPipelines() {
    if (!res_) return;
    for (u32 i = 0; i < 8; ++i) {
        res_->destroyPipeline(worldPipes_[i]);
        worldPipes_[i] = 0;
        worldPipeTried_[i] = false;
    }
}

void UiRenderer::onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth, u32 width, u32 height) {
    (void)width;
    sceneHeight_ = height;
    if (sampleCount == worldSampleCount_ && color == worldColor_ && depth == worldDepth_) return;
    worldSampleCount_ = sampleCount ? sampleCount : 1;
    worldColor_ = color;
    worldDepth_ = depth;
    destroyWorldPipelines();   // the target's formats and sample count are baked into every pipeline
}

// The pipeline for a label's draw flags, built on first use. 0 when the shaders or the pipeline could not be built.
rhi::PipelineHandle UiRenderer::worldPipelineFor(u32 flags) {
    const u32 slot = pipelineSlot(flags);
    if (worldPipeTried_[slot]) return worldPipes_[slot];
    worldPipeTried_[slot] = true;

    if (!worldShadersTried_) {
        worldShadersTried_ = true;
        rhi::ShaderDesc sd;
        sd.source = rhi::shaderFile("world_text.hlsl").c_str();
        sd.entry = "WorldTextVS";
        sd.stage = rhi::ShaderStage::Vertex;
        worldVs_ = res_->createShader(sd);
        sd.entry = "WorldTextPS";
        sd.stage = rhi::ShaderStage::Pixel;
        worldPs_ = res_->createShader(sd);
        if (!worldVs_ || !worldPs_) AVER_ERROR("[Render.UI] the world text shaders failed to compile; labels are not drawn");
    }
    if (!worldVs_ || !worldPs_) return 0;

    if (worldColor_ == rhi::Format::Unknown && device_) {
        worldColor_ = device_->backbufferFormat();
        worldDepth_ = device_->depthFormat();
        worldSampleCount_ = std::max<u32>(1, device_->sampleCount());
    }

    rhi::GraphicsPipelineDesc gd;
    gd.vs = worldVs_;
    gd.ps = worldPs_;
    // position(3) as 2D + 1D, the RHI having no three-component float vertex format (as the particle mesh path), uv, colour.
    gd.vertexLayout.stride = sizeof(WorldTextVertex);
    gd.vertexLayout.attribCount = 4;
    gd.vertexLayout.attribs[0] = {rhi::VertexSemantic::Position, 0, rhi::Format::RG32Float, 0};
    gd.vertexLayout.attribs[1] = {rhi::VertexSemantic::Position, 1, rhi::Format::R32Float, 8};
    gd.vertexLayout.attribs[2] = {rhi::VertexSemantic::TexCoord, 0, rhi::Format::RG32Float, 12};
    gd.vertexLayout.attribs[3] = {rhi::VertexSemantic::Color, 0, rhi::Format::RGBA8Unorm, 20};
    static_assert(offsetof(WorldTextVertex, u) == 12 && offsetof(WorldTextVertex, rgba) == 20, "the world text input layout names these offsets");

    gd.layout.srvCount = 1;
    gd.layout.samplerCount = 1;
    gd.layout.samplers[0].filter = rhi::Filter::Linear;
    gd.layout.samplers[0].address = rhi::AddressMode::Clamp;
    gd.layout.constantDwords[kWorldConstantSlot] = kWorldConstantDwords;

    // Depth-tested with depth write off, the contract IRenderFeature::transparentPass documents.
    gd.depth.test = (flags & aver::ui::kWorldTextNoDepth) == 0;
    gd.depth.write = false;
    gd.depth.op = rhi::CompareOp::LessEqual;
    gd.cull = (flags & aver::ui::kWorldTextOneSided) ? rhi::CullMode::Back : rhi::CullMode::None;
    gd.blend = (flags & aver::ui::kWorldTextAdditive) ? rhi::BlendMode::Additive : rhi::BlendMode::PremultipliedAlpha;
    gd.renderTargetCount = 1;
    gd.renderTargets[0] = worldColor_;
    gd.depthFormat = worldDepth_;
    gd.sampleCount = worldSampleCount_;
    worldPipes_[slot] = res_->createGraphicsPipeline(gd);
    if (!worldPipes_[slot]) AVER_WARN("[Render.UI] a world text pipeline (flags {}) failed to build", flags);
    return worldPipes_[slot];
}

bool UiRenderer::ensureWorldCapacity(u32 vertexCount, u32 indexCount) {
    const bool growV = vertexCount > wvbCapacity_ || !wvb_[0];
    const bool growI = indexCount > wibCapacity_ || !wib_[0];
    if (!growV && !growI) return true;
    const u32 newV = growV ? std::max({vertexCount, wvbCapacity_ * 2, kInitialWorldVertices}) : wvbCapacity_;
    const u32 newI = growI ? std::max({indexCount, wibCapacity_ * 2, kInitialWorldIndices}) : wibCapacity_;
    for (u32 i = 0; i < kFramesInFlight; ++i) {
        if (growV) {
            res_->destroyBuffer(wvb_[i]);
            rhi::BufferDesc bd;
            bd.bytes = static_cast<u64>(newV) * sizeof(WorldTextVertex);
            bd.kind = rhi::BufferKind::Upload;
            bd.debugName = "Aver.UI world text vertices";
            wvb_[i] = res_->createBuffer(bd);
            if (!wvb_[i]) { AVER_ERROR("[Render.UI] world text vertex buffer allocation failed"); return false; }
        }
        if (growI) {
            res_->destroyBuffer(wib_[i]);
            rhi::BufferDesc bd;
            bd.bytes = static_cast<u64>(newI) * sizeof(u32);
            bd.kind = rhi::BufferKind::Upload;
            bd.debugName = "Aver.UI world text indices";
            wib_[i] = res_->createBuffer(bd);
            if (!wib_[i]) { AVER_ERROR("[Render.UI] world text index buffer allocation failed"); return false; }
        }
    }
    wvbCapacity_ = newV;
    wibCapacity_ = newI;
    return true;
}

void UiRenderer::transparentPass(rhi::IRenderContext& ctx) {
    worldDrawn_ = worldSkipped_ = 0;
    if (!worldEnabled_ || !res_ || !device_ || worldTexts_.empty()) return;
    if (rayDriven_ && !worldInRayDriven_) {
        worldSkipped_ = static_cast<u32>(worldTexts_.size());
        return;
    }
    WorldTextCamera cam;
    if (!device_->camera(cam.viewProj, cam.invViewProjRel, cam.eye)) return;
    cam.viewportHeight = sceneHeight_;
    const bool sdf = worldAtlas_ && worldAtlas_->sdf();

    // ---- place every label for this camera, far to near ----
    struct Item {
        u32 text;
        f32 depth;
        f32 matrix[16];
    };
    std::vector<Item> items;
    items.reserve(worldTexts_.size());
    for (u32 t = 0; t < worldTexts_.size() && t < aver::ui::kUiMaxWorldTexts; ++t) {
        const aver::ui::UiWorldText& wt = worldTexts_[t];
        if (static_cast<u64>(wt.vertexOffset) + wt.vertexCount > worldVerts_.size() ||
            static_cast<u64>(wt.runBegin) + wt.runCount > worldRuns_.size()) {
            ++worldSkipped_;
            continue;
        }
        Item it;
        it.text = t;
        if (!worldTextMatrix(wt, cam, it.matrix)) { ++worldSkipped_; continue; }
        it.depth = worldTextDepth(it.matrix, cam);
        items.push_back(it);
    }
    if (items.empty()) return;
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.depth > b.depth; });

    // ---- the CPU-side buffers and the draw records ----
    struct Record {
        u32 flags;
        u32 page;
        u32 indexOffset, indexCount;
        f32 alphaCut, intensity, softness;
    };
    std::vector<WorldTextVertex> verts;
    std::vector<u32> idx;
    std::vector<Record> records;
    for (const Item& it : items) {
        const aver::ui::UiWorldText& wt = worldTexts_[it.text];
        const u32 base = static_cast<u32>(verts.size());
        verts.resize(verts.size() + wt.vertexCount);
        worldTextPlace(worldVerts_.data() + wt.vertexOffset, wt.vertexCount, it.matrix, verts.data() + base);
        bool any = false;
        for (u32 r = 0; r < wt.runCount; ++r) {
            const aver::ui::UiWorldTextRun& run = worldRuns_[wt.runBegin + r];
            if (run.indexCount == 0 || run.indexCount % 3 != 0 || static_cast<u64>(run.indexOffset) + run.indexCount > worldIdx_.size()) continue;
            if (!worldAtlas_ || run.page >= worldAtlas_->pageCount() || run.page >= worldPages_.size() || !worldPages_[run.page].tex) continue;
            bool ok = true;
            for (u32 i = 0; i < run.indexCount && ok; ++i) ok = worldIdx_[run.indexOffset + i] < wt.vertexCount;
            if (!ok) continue;
            const u32 first = static_cast<u32>(idx.size());
            for (u32 i = 0; i < run.indexCount; ++i) idx.push_back(base + worldIdx_[run.indexOffset + i]);
            records.push_back(Record{wt.flags, run.page, first, run.indexCount, wt.alphaCut, wt.intensity, wt.sdfSoftness > 0.0f ? wt.sdfSoftness : worldSoftness_});
            any = true;
        }
        if (any) ++worldDrawn_; else ++worldSkipped_;
    }
    if (records.empty() || verts.size() > aver::ui::kUiMaxWorldVertices || idx.size() > aver::ui::kUiMaxWorldIndices) return;
    if (!ensureWorldCapacity(static_cast<u32>(verts.size()), static_cast<u32>(idx.size()))) return;

    worldFrame_ = (worldFrame_ + 1) % kFramesInFlight;
    const rhi::BufferHandle vb = wvb_[worldFrame_], ib = wib_[worldFrame_];
    if (!vb || !ib) return;
    if (!res_->writeBuffer(vb, verts.data(), verts.size() * sizeof(WorldTextVertex))) return;
    if (!res_->writeBuffer(ib, idx.data(), idx.size() * sizeof(u32))) return;

    ctx.pushMarker("Aver.UI world text");
    ctx.setVertexBuffer(vb, sizeof(WorldTextVertex));
    ctx.setIndexBuffer(ib, rhi::Format::R32Uint);
    rhi::PipelineHandle boundPipe = 0;
    rhi::TextureHandle boundTex = 0;
    bool haveTex = false;
    f32 consts[kWorldConstantDwords];
    std::memcpy(consts, cam.viewProj, sizeof(f32) * 16);
    for (const Record& rec : records) {
        const rhi::PipelineHandle pso = worldPipelineFor(rec.flags);
        if (!pso) continue;
        if (pso != boundPipe) {
            ctx.setPipeline(pso);
            boundPipe = pso;
            ctx.setVertexBuffer(vb, sizeof(WorldTextVertex));
            ctx.setIndexBuffer(ib, rhi::Format::R32Uint);
            haveTex = false;
        }
        consts[16] = rec.alphaCut;
        consts[17] = rec.intensity;
        consts[18] = sdf ? 1.0f : 0.0f;
        consts[19] = rec.softness;
        ctx.setConstants(kWorldConstantSlot, consts, kWorldConstantDwords);
        const rhi::TextureHandle tex = worldPages_[rec.page].tex;
        if (!haveTex || tex != boundTex) {
            const rhi::BindingSetHandle set = bindingFor(tex);
            if (!set) continue;
            ctx.setBindingSet(set, 0);
            boundTex = tex;
            haveTex = true;
        }
        ctx.drawIndexed(rec.indexCount, rec.indexOffset, 0);
    }
    ctx.popMarker();
}

} // namespace aver::render::ui
