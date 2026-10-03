#include "aver/rhi/EditorLines.hpp"

#include "aver/core/Log.hpp"
#include "aver/rhi/ShaderFiles.hpp"   // editor_lines.hlsl is a deployed file, not a literal

#include <cmath>
#include <cstddef>
#include <cstring>

namespace aver::rhi {
namespace {

// corner.x: 0 at endpoint a, 1 at endpoint b. corner.y: -1/+1 side. Matches EditorLineVertex's own
// comment and VSEditorLine's read of i.corner.
constexpr f32 kCorner[4][2] = {{0.0f, -1.0f}, {0.0f, 1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}};

// A display colour as RGBA8Unorm, R in the low byte (0xAABBGGRR), opaque.
u32 packDisplay(f32 r, f32 g, f32 b) {
    auto q = [](f32 v) -> u32 {
        const f32 c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<u32>(c * 255.0f + 0.5f);
    };
    return q(r) | (q(g) << 8) | (q(b) << 16) | (255u << 24);
}

// Unreal's wireframe colours (UStaticMeshComponent::GetWireframeColor), display sRGB: static meshes
// cyan, movable ones magenta.
constexpr f32 kWireStatic[3]  = {0.0f, 1.0f, 1.0f};
constexpr f32 kWireMovable[3] = {1.0f, 0.0f, 1.0f};

// The CPU twin of the prelude's srgbToLin, for a colour written to a target that encodes sRGB itself.
f32 srgbToLinear(f32 c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

bool formatIsSrgb(Format f) {
    switch (f) {
        case Format::RGBA8UnormSrgb:
        case Format::BC1UnormSrgb:
        case Format::BC3UnormSrgb:
        case Format::BC7UnormSrgb:
            return true;
        default:
            return false;
    }
}

} // namespace

bool EditorLines::init(IResourceFactory& res) {
    res_ = &res;

    ShaderDesc sd;
    sd.source = shaderFile("editor_lines.hlsl").c_str();
    sd.prelude = sharedShaderPrelude();
    sd.entry = "VSEditorLine";
    sd.stage = ShaderStage::Vertex;
    vs_ = res_->createShader(sd);

    sd.entry = "PSEditorLine";
    sd.stage = ShaderStage::Pixel;
    ps_ = res_->createShader(sd);

    sd.defines = "AVER_EDITOR_LINE_MS=1";
    psMs_ = res_->createShader(sd);

    if (!vs_ || !ps_ || !psMs_) {
        AVER_ERROR("[EditorLines] editor_lines.hlsl failed to compile -- lines will not be drawn");
        shutdown();
        return false;
    }

    sd.defines = nullptr;
    sd.entry = "VSEditorWire";
    sd.stage = ShaderStage::Vertex;
    wireVs_ = res_->createShader(sd);
    sd.entry = "PSEditorWire";
    sd.stage = ShaderStage::Pixel;
    wirePs_ = res_->createShader(sd);
    if (!wireVs_ || !wirePs_)
        AVER_WARN("[EditorLines] the wireframe view's shaders failed to compile -- Wireframe will show "
                  "only the editor's own lines");

    // One binding set per frame in flight, per depth-sample flavour, so a rewritten descriptor
    // (setSrv in replay()) never reaches a draw from an earlier frame still executing -- the same
    // reasoning ViewportIconRenderer documents for its own per-icon sets.
    for (u32 i = 0; i < kFramesInFlight; ++i) {
        BindingSetDesc bsd;
        bsd.srvCount = 1;
        bsd.srvKinds[0] = SlotKind::Texture2D;
        bsd.srvBaseRegister = 0;
        depthSet_[i] = res_->createBindingSet(bsd);

        BindingSetDesc bsdMs;
        bsdMs.srvCount = 1;
        bsdMs.srvKinds[0] = SlotKind::Texture2DMS;
        bsdMs.srvBaseRegister = 0;
        depthSetMs_[i] = res_->createBindingSet(bsdMs);

        if (!depthSet_[i] || !depthSetMs_[i]) {
            AVER_ERROR("[EditorLines] binding set creation failed");
            shutdown();
            return false;
        }
    }

    AVER_INFO("[EditorLines] ready");
    return true;
}

void EditorLines::shutdown() {
    if (res_) {
        for (Mesh& m : meshes_) {
            res_->destroyBuffer(m.vb);
            res_->destroyBuffer(m.ib);
        }
        for (Retired& r : retired_) {
            res_->destroyBuffer(r.vb);
            res_->destroyBuffer(r.ib);
        }
        for (Spare& s : spare_) res_->destroyBuffer(s.h);
        for (u32 i = 0; i < kFramesInFlight; ++i) {
            res_->destroyBindingSet(depthSet_[i]);
            res_->destroyBindingSet(depthSetMs_[i]);
        }
        res_->destroyPipeline(pso_);
        res_->destroyPipeline(psoMs_);
        res_->destroyPipeline(wirePso_);
        res_->destroyShader(vs_);
        res_->destroyShader(ps_);
        res_->destroyShader(psMs_);
        res_->destroyShader(wireVs_);
        res_->destroyShader(wirePs_);
    }
    meshes_.assign(1, Mesh{});
    queue_.clear();
    wireQueue_.clear();
    retired_.clear();
    spare_.clear();
    for (u32 i = 0; i < kFramesInFlight; ++i) { depthSet_[i] = 0; depthSetMs_[i] = 0; }
    pso_ = 0; psoMs_ = 0; wirePso_ = 0;
    vs_ = 0; ps_ = 0; psMs_ = 0; wireVs_ = 0; wirePs_ = 0;
    builtSamples_ = 0;
    builtFormat_ = Format::Unknown;
    frame_ = 0;
    res_ = nullptr;
}

LineHandle EditorLines::create(const LineVertex* verts, u32 count) {
    if (!res_) return 0;
    if (!verts || count < 2 || (count % 2) != 0) {
        AVER_WARN("[EditorLines] create() refused: {} vertices (need an even count of at least 2)", count);
        return 0;
    }

    const u32 segCount = count / 2;
    std::vector<EditorLineVertex> verts4(static_cast<usize>(segCount) * 4);
    std::vector<u32> indices(static_cast<usize>(segCount) * 6);

    for (u32 s = 0; s < segCount; ++s) {
        const LineVertex& va = verts[s * 2 + 0];
        const LineVertex& vb = verts[s * 2 + 1];
        const u32 base = s * 4;
        for (u32 c = 0; c < 4; ++c) {
            EditorLineVertex& v = verts4[base + c];
            v.a[0] = va.px; v.a[1] = va.py; v.a[2] = va.pz;
            v.b[0] = vb.px; v.b[1] = vb.py; v.b[2] = vb.pz;
            const bool atB = kCorner[c][0] > 0.5f;
            const LineVertex& ep = atB ? vb : va;
            v.color = packDisplay(ep.r, ep.g, ep.b);
            v.corner[0] = kCorner[c][0];
            v.corner[1] = kCorner[c][1];
        }
        // Two triangles over corners {a-, a+, b-, b+}; CullMode::None so winding doesn't matter.
        const u32 quad[6] = {base + 0, base + 1, base + 2, base + 1, base + 3, base + 2};
        std::memcpy(&indices[static_cast<usize>(s) * 6], quad, sizeof(quad));
    }

    Mesh m;
    const u64 vbBytes = verts4.size() * sizeof(EditorLineVertex);
    const u64 ibBytes = indices.size() * sizeof(u32);
    m.vb = acquire(vbBytes, "Aver.EditorLines vertices", m.vbCap);
    m.ib = acquire(ibBytes, "Aver.EditorLines indices", m.ibCap);

    if (!m.vb || !m.ib || !res_->writeBuffer(m.vb, verts4.data(), vbBytes) ||
        !res_->writeBuffer(m.ib, indices.data(), ibBytes)) {
        AVER_WARN("[EditorLines] create() could not build GPU buffers for {} vertices", count);
        res_->destroyBuffer(m.vb);
        res_->destroyBuffer(m.ib);
        return 0;
    }

    m.indexCount = static_cast<u32>(indices.size());
    m.alive = true;
    meshes_.push_back(m);
    return static_cast<LineHandle>(meshes_.size() - 1);
}

bool EditorLines::destroy(LineHandle mesh) {
    if (mesh == 0 || mesh >= meshes_.size()) return false;
    Mesh& m = meshes_[mesh];
    if (!m.alive) return false;
    retired_.push_back(Retired{m.vb, m.ib, m.vbCap, m.ibCap, kFramesInFlight});
    m.vb = 0;
    m.ib = 0;
    m.vbCap = 0;
    m.ibCap = 0;
    m.indexCount = 0;
    m.alive = false;
    return true;
}

void EditorLines::queue(LineHandle mesh, const f32 world[16]) {
    if (mesh == 0 || mesh >= meshes_.size() || !meshes_[mesh].alive) return;
    Draw d;
    d.mesh = mesh;
    std::memcpy(d.world, world, sizeof(d.world));
    d.width = width_;
    d.depthTest = depthTest_;
    queue_.push_back(d);
}

void EditorLines::queueWire(MeshHandle mesh, const f32 world[16], bool movable) {
    if (mesh == 0 || !res_) return;
    WireDraw d;
    d.mesh = mesh;
    std::memcpy(d.world, world, sizeof(d.world));
    d.movable = movable;
    wireQueue_.push_back(d);
}

void EditorLines::retireTick() {
    if (!res_) { retired_.clear(); spare_.clear(); return; }
    for (usize i = 0; i < retired_.size();) {
        Retired& r = retired_[i];
        if (r.framesLeft > 0) --r.framesLeft;
        if (r.framesLeft == 0) {
            shelve(r.vb, r.vbCap);
            shelve(r.ib, r.ibCap);
            r = retired_.back();
            retired_.pop_back();
        } else {
            ++i;
        }
    }
    for (usize i = 0; i < spare_.size();) {
        Spare& s = spare_[i];
        if (s.idle < kSpareLifeTicks) ++s.idle;
        if (s.idle >= kSpareLifeTicks) {
            res_->destroyBuffer(s.h);
            s = spare_.back();
            spare_.pop_back();
        } else {
            ++i;
        }
    }
}

void EditorLines::shelve(BufferHandle h, u32 cap) {
    if (h == 0) return;
    if (cap == 0 || spare_.size() >= kMaxSpare) { res_->destroyBuffer(h); return; }
    spare_.push_back(Spare{h, cap, 0});
}

BufferHandle EditorLines::acquire(u64 bytes, const char* name, u32& cap) {
    cap = 0;
    if (bytes <= kMaxSpareBytes) {
        const u32 want = static_cast<u32>((bytes + kBufferGranule - 1) / kBufferGranule * kBufferGranule);
        // The smallest idle-enough buffer that fits without wasting more than a quarter of itself.
        usize best = spare_.size();
        for (usize i = 0; i < spare_.size(); ++i) {
            const Spare& s = spare_[i];
            if (s.idle < kFramesInFlight || s.cap < want || s.cap - want > want / 4) continue;
            if (best == spare_.size() || s.cap < spare_[best].cap) best = i;
        }
        if (best != spare_.size()) {
            const BufferHandle h = spare_[best].h;
            cap = spare_[best].cap;
            spare_[best] = spare_.back();
            spare_.pop_back();
            return h;
        }
        cap = want;
        bytes = want;
    }
    BufferDesc d;
    d.bytes = bytes;
    d.kind = BufferKind::Upload;
    d.debugName = name;
    return res_->createBuffer(d);
}

bool EditorLines::buildPipelines(u32 depthSamples, Format targetFormat) {
    if (!res_ || !vs_ || !ps_ || !psMs_) return false;
    res_->destroyPipeline(pso_);
    res_->destroyPipeline(psoMs_);
    res_->destroyPipeline(wirePso_);
    pso_ = 0;
    psoMs_ = 0;
    wirePso_ = 0;

    // The wireframe view: the engine's own MeshVertex (the default vertex layout), edges only, no
    // depth and no blending. Its failure is logged and leaves the lines untouched.
    if (wireVs_ && wirePs_) {
        GraphicsPipelineDesc wd;
        wd.vs = wireVs_;
        wd.ps = wirePs_;
        wd.fill = FillMode::Wireframe;
        wd.cull = CullMode::None;
        wd.depth.test = false;
        wd.depth.write = false;
        wd.depthFormat = Format::Unknown;
        wd.blend = BlendMode::Opaque;
        wd.renderTargetCount = 1;
        wd.renderTargets[0] = targetFormat;
        wd.sampleCount = 1;
        wd.layout.constantDwords[kObjectConstantRegister] = kObjectConstantDwords;   // see below
        wirePso_ = res_->createGraphicsPipeline(wd);
        if (!wirePso_)
            AVER_WARN("[EditorLines] the wireframe view's pipeline failed to build (target format {})",
                      static_cast<u32>(targetFormat));
    }

    GraphicsPipelineDesc gd;
    gd.vs = vs_;

    // Positions split xy + z (no three-component float Format), in editor_lines.hlsl's ELIn order,
    // which is also the Vulkan attribute-location order.
    gd.vertexLayout.stride = sizeof(EditorLineVertex);
    gd.vertexLayout.attribCount = 6;
    gd.vertexLayout.attribs[0] = {VertexSemantic::Position, 0, Format::RG32Float,
                                  static_cast<u32>(offsetof(EditorLineVertex, a))};
    gd.vertexLayout.attribs[1] = {VertexSemantic::Position, 1, Format::R32Float,
                                  static_cast<u32>(offsetof(EditorLineVertex, a) + 8)};
    gd.vertexLayout.attribs[2] = {VertexSemantic::Position, 2, Format::RG32Float,
                                  static_cast<u32>(offsetof(EditorLineVertex, b))};
    gd.vertexLayout.attribs[3] = {VertexSemantic::Position, 3, Format::R32Float,
                                  static_cast<u32>(offsetof(EditorLineVertex, b) + 8)};
    gd.vertexLayout.attribs[4] = {VertexSemantic::Color, 0, Format::RGBA8Unorm,
                                  static_cast<u32>(offsetof(EditorLineVertex, color))};
    gd.vertexLayout.attribs[5] = {VertexSemantic::TexCoord, 0, Format::RG32Float,
                                  static_cast<u32>(offsetof(EditorLineVertex, corner))};
    static_assert(offsetof(EditorLineVertex, b) == 12, "editor_lines.hlsl's ELIn names this offset");
    static_assert(offsetof(EditorLineVertex, color) == 24, "editor_lines.hlsl's ELIn names this offset");
    static_assert(offsetof(EditorLineVertex, corner) == 28, "editor_lines.hlsl's ELIn names this offset");

    gd.depth.test = false;
    gd.depth.write = false;
    gd.depthFormat = Format::Unknown;
    gd.cull = CullMode::None;
    gd.blend = BlendMode::PremultipliedAlpha;
    gd.renderTargetCount = 1;
    gd.renderTargets[0] = targetFormat;
    gd.sampleCount = 1;   // the overlay target itself is never multisampled; depthSamples is the SRV's
    gd.layout.srvCount = 1;
    // b1 as root constants, DECLARED: an undeclared slot is a root CBV on D3D12, and replay()'s
    // setConstants was refused on every draw ("slot 1 declares constantDwords 0") -- ActorPreview
    // declares its object block the same way.
    gd.layout.constantDwords[kObjectConstantRegister] = kObjectConstantDwords;
    // Declared, not reflected: Vulkan types every binding in the layout, and the two pixel shaders
    // read t0 as different kinds (PipelineLayout's own comment on why reflection is not enough).
    gd.layout.slotKindsDeclared = true;

    gd.ps = ps_;
    gd.layout.srvKinds[0] = SlotKind::Texture2D;
    pso_ = res_->createGraphicsPipeline(gd);
    gd.ps = psMs_;
    gd.layout.srvKinds[0] = SlotKind::Texture2DMS;
    psoMs_ = res_->createGraphicsPipeline(gd);

    builtSamples_ = depthSamples;
    builtFormat_ = targetFormat;

    if (!pso_ || !psoMs_) {
        AVER_ERROR("[EditorLines] pipeline build failed (target format {}, depth samples {})",
                   static_cast<u32>(targetFormat), depthSamples);
        return false;
    }
    return true;
}

void EditorLines::replay(IRenderContext& ctx, u32 targetW, u32 targetH, const f32 sceneRect[4],
                          TextureHandle sceneDepth, u32 depthSamples, Format targetFormat,
                          bool firstOfFrame, bool lastOfFrame) {
    if (firstOfFrame) retireTick();

    // Cleared on every path out of this function, including every early return below, so a frame
    // that never reaches this call (suppressed, device lost) doesn't carry stale draws forward. Not
    // after a frame's first of two replays (frame interpolation), which leaves the queue for the second.
    struct ClearOnExit {
        std::vector<Draw>* q;
        std::vector<WireDraw>* w;
        bool on;
        ~ClearOnExit() { if (on) { q->clear(); w->clear(); } }
    } clearer{&queue_, &wireQueue_, lastOfFrame};

    if ((queue_.empty() && wireQueue_.empty()) || !res_) return;

    if (targetFormat != builtFormat_ || depthSamples != builtSamples_) {
        if (!buildPipelines(depthSamples, targetFormat)) return;
    }

    f32 rect[4] = {sceneRect[0], sceneRect[1], sceneRect[2], sceneRect[3]};
    if (rect[0] < 0.0f) rect[0] = 0.0f;
    if (rect[1] < 0.0f) rect[1] = 0.0f;
    if (rect[0] > static_cast<f32>(targetW)) rect[0] = static_cast<f32>(targetW);
    if (rect[1] > static_cast<f32>(targetH)) rect[1] = static_cast<f32>(targetH);
    if (rect[2] < 0.0f) rect[2] = 0.0f;
    if (rect[3] < 0.0f) rect[3] = 0.0f;
    if (rect[0] + rect[2] > static_cast<f32>(targetW)) rect[2] = static_cast<f32>(targetW) - rect[0];
    if (rect[1] + rect[3] > static_cast<f32>(targetH)) rect[3] = static_cast<f32>(targetH) - rect[1];

    const u32 rx = static_cast<u32>(rect[0]), ry = static_cast<u32>(rect[1]);
    const u32 rw = static_cast<u32>(rect[2]), rh = static_cast<u32>(rect[3]);

    const bool srgbTarget = formatIsSrgb(targetFormat);
    ctx.pushMarker("Aver.EditorLines");

    // The wireframe view first, so the grid, gizmo and outline draw over it.
    if (!wireQueue_.empty() && wirePso_) {
        ctx.setPipeline(wirePso_);
        ctx.setViewport(rx, ry, rw, rh);
        ctx.setScissor(rx, ry, rw, rh);
        f32 colors[2][3];
        for (int k = 0; k < 3; ++k) {
            colors[0][k] = srgbTarget ? srgbToLinear(kWireStatic[k])  : kWireStatic[k];
            colors[1][k] = srgbTarget ? srgbToLinear(kWireMovable[k]) : kWireMovable[k];
        }
        for (const WireDraw& d : wireQueue_) {
            // PerObject: gWorld[0..15], gBaseColor[16..19] = the wire colour in the target's encoding.
            f32 consts[kObjectConstantDwords] = {};
            std::memcpy(consts, d.world, sizeof(f32) * 16);
            std::memcpy(consts + 16, colors[d.movable ? 1 : 0], sizeof(f32) * 3);
            consts[19] = 1.0f;
            ctx.setConstants(kObjectConstantRegister, consts, kObjectConstantDwords);
            ctx.drawMesh(d.mesh);
        }
    }

    const bool useMs = depthSamples > 1;
    const PipelineHandle pso = useMs ? psoMs_ : pso_;
    if (queue_.empty() || !pso) { ctx.popMarker(); return; }

    // Advance to the next frame-in-flight slot BEFORE writing its descriptor, so this replay's
    // setSrv can't race a draw from an earlier frame still reading the previous contents of the
    // slot it is about to reuse (three replays back, by construction).
    frame_ = (frame_ + 1) % kFramesInFlight;
    const BindingSetHandle set = useMs ? depthSetMs_[frame_] : depthSet_[frame_];
    if (!set) { ctx.popMarker(); return; }

    if (sceneDepth) res_->setSrv(set, 0, sceneDepth);
    else res_->clearSrv(set, 0);
    const bool depthAvailable = sceneDepth != 0;

    ctx.setPipeline(pso);
    ctx.setBindingSet(set, 0);
    ctx.setViewport(rx, ry, rw, rh);
    ctx.setScissor(rx, ry, rw, rh);

    for (const Draw& d : queue_) {
        if (d.mesh == 0 || d.mesh >= meshes_.size()) continue;
        const Mesh& m = meshes_[d.mesh];
        if (!m.alive || !m.vb || !m.ib) continue;

        // PerObject (shared_prelude.hlsl), 32 dwords: gWorld[0..15], gBaseColor[16..19],
        // gMaterial[20..23], gShadingModel/gReflectance/gF90/_objPad[24..27] (unused here, left
        // zero), gEmissive[28..31].
        f32 consts[kObjectConstantDwords] = {};
        std::memcpy(consts, d.world, sizeof(f32) * 16);
        consts[16] = d.width;
        consts[17] = (d.depthTest && depthAvailable) ? 1.0f : 0.0f;
        consts[18] = rect[2];
        consts[19] = rect[3];
        consts[20] = srgbTarget ? 1.0f : 0.0f;
        consts[22] = rect[0];
        consts[23] = rect[1];
        consts[28] = static_cast<f32>(targetW);
        consts[29] = static_cast<f32>(targetH);

        ctx.setConstants(kObjectConstantRegister, consts, kObjectConstantDwords);
        ctx.setVertexBuffer(m.vb, sizeof(EditorLineVertex));
        ctx.setIndexBuffer(m.ib, Format::R32Uint);
        ctx.drawIndexed(m.indexCount);
    }

    ctx.popMarker();
}

} // namespace aver::rhi
