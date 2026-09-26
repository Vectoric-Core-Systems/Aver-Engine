// ParticleRenderer implementation: the pipelines, the per-frame vertex build, and the draw.
#include "aver/particles/ParticleRenderer.hpp"
#include "ParticleShaders.hpp"

#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include "aver/rhi/ShaderFiles.hpp"   // this pass's HLSL is a deployed file, not a literal

namespace aver::particles {
namespace {

constexpr u32 kInitialVertices = 4 * 256;   // 256 particles' worth, across every emitter this frame
constexpr u32 kInitialIndices  = 6 * 256;

// Applies the row-vector camera-relative inverse view-projection (rhi::PerFrameCB::invViewProjRel)
// to an NDC point and dehomogenises it -- the same manual expansion ParticleSystem.cpp's
// transformPoint uses, just against that matrix instead of a world matrix. The result is an offset
// FROM THE EYE; an absolute point needs camPos added.
Vec3 unprojectPoint(const f32 ivpRel[16], f32 ndcX, f32 ndcY, f32 ndcZ) {
    const f32 v[4] = {ndcX, ndcY, ndcZ, 1.0f};
    f32 r[4] = {0, 0, 0, 0};
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i)
            r[j] += v[i] * ivpRel[i * 4 + j];
    const f32 invW = r[3] != 0.0f ? 1.0f / r[3] : 1.0f;
    return {r[0] * invW, r[1] * invW, r[2] * invW};
}

// Recovers the camera's world-space RIGHT and UP axes from invViewProjRel ALONE -- no separate view
// matrix exists on IDevice::camera() to read them from directly.
//
// WHY THIS IS EXACT, NOT APPROXIMATE. Fix an NDC depth d and vary only ndc.x: for this engine's
// perspective matrix (Math.hpp's Mat4::perspectiveLH), clip.w after ViewProj equals the view-space
// Z, and ndc.z = f(view-space Z) alone -- so holding ndc.z fixed holds view-space Z fixed. With
// view-space Z fixed, ndc.x = viewX * xScale / viewZ is LINEAR in viewX, so a finite step in ndc.x
// maps to a finite step PURELY along the camera's view-space X axis, at ANY step size -- not just in
// the limit. Unprojecting two such points back through invViewProjRel and differencing therefore
// recovers a vector that is exactly parallel to the camera's world-space right axis (view space's
// X, carried through the view matrix's own rotation), not a first-order approximation of it. The
// same argument holds for ndc.y and the up axis. Two calls, not four: `center` is shared by both.
//
// CAMERA-RELATIVE, so the three points sit a few centimetres from the eye rather than at its world
// magnitude, and their differences lose nothing however far the camera is from the origin.
void cameraBasis(const f32 invViewProjRel[16], Vec3& outRight, Vec3& outUp) {
    const Vec3 center = unprojectPoint(invViewProjRel, 0.0f, 0.0f, 0.5f);
    const Vec3 rightP = unprojectPoint(invViewProjRel, 0.5f, 0.0f, 0.5f);
    const Vec3 upP    = unprojectPoint(invViewProjRel, 0.0f, 0.5f, 0.5f);
    outRight = (rightP - center).getSafeNormal();
    outUp    = (upP - center).getSafeNormal();
}

f32 lerpf(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

// Packs a straight rgba[4] (0..1) into premultiplied RGBA8Unorm -- byte order R,G,B,A from the low
// byte up, the SAME convention modules/ui/src/UiDrawList.cpp's uiPremultiply uses (0xAABBGGRR), so
// this and the UI renderer agree on what an RGBA8Unorm vertex colour means. Alpha itself is NOT
// multiplied away -- BlendMode::PremultipliedAlpha's `dst.rgb * (1 - src.a)` still needs it.
u32 packPremultiplied(const f32 rgba[4]) {
    auto byteOf = [](f32 v) {
        v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<u32>(v * 255.0f + 0.5f);
    };
    const f32 a = rgba[3];
    const u32 r = byteOf(rgba[0] * a), g = byteOf(rgba[1] * a), b = byteOf(rgba[2] * a), ac = byteOf(a);
    return r | (g << 8) | (b << 16) | (ac << 24);
}

} // namespace

bool ParticleRenderer::init(rhi::IDevice& device) {
    dev_ = &device;
    res_ = device.resources();
    if (!res_) {
        AVER_WARN("[Particles] init declined: backend exposes no resource factory");
        return false;
    }

    rhi::ShaderDesc sd;
    sd.source = rhi::shaderFile("particles.hlsl").c_str();
    sd.prelude = rhi::sharedShaderPrelude();   // gViewProj, and nothing else from it, is used
    sd.entry = "ParticleVS";
    sd.stage = rhi::ShaderStage::Vertex;
    vs_ = res_->createShader(sd);
    sd.entry = "ParticlePS";
    sd.stage = rhi::ShaderStage::Pixel;
    ps_ = res_->createShader(sd);
    if (!vs_ || !ps_) {
        AVER_ERROR("[Particles] the particle shaders failed to compile");
        return false;
    }

    if (!ensureCapacity(kInitialVertices, kInitialIndices)) return false;

    // Best-effort initial build (see VoxiRenderer::init's identical two-step pattern): the real scene
    // target's sample count and formats arrive through onRenderTargetsChanged, which the device calls
    // before the first real frame; this just means the very first registration is never pipeline-less.
    buildPipelines(device.sampleCount(), device.backbufferFormat(), device.depthFormat());

    AVER_INFO("[Particles] ready");
    return true;
}

void ParticleRenderer::shutdown() {
    if (res_) {
        for (u32 i = 0; i < kFramesInFlight; ++i) { res_->destroyBuffer(vb_[i]); res_->destroyBuffer(ib_[i]); }
        res_->destroyPipeline(premultPso_);
        res_->destroyPipeline(additivePso_);
        res_->destroyPipeline(giPremultPso_);
        res_->destroyPipeline(giAdditivePso_);
        res_->destroyBindingSet(giBindingSet_);
        res_->destroyShader(vs_);
        res_->destroyShader(ps_);
        res_->destroyShader(giPs_);
    }
    vb_[0] = vb_[1] = vb_[2] = 0;
    ib_[0] = ib_[1] = ib_[2] = 0;
    premultPso_ = additivePso_ = 0;
    giPremultPso_ = giAdditivePso_ = 0;
    giBindingSet_ = 0;
    vs_ = ps_ = giPs_ = 0;
    vbCapacity_ = ibCapacity_ = 0;
    res_ = nullptr;
    dev_ = nullptr;
}

void ParticleRenderer::setGiSeam(const GiSeam& seam) {
    giSeam_ = seam;
    // Rebuild NOW, not on the next resize -- Voxi attaches before Particles in both composition
    // roots (see GiSeam's own comment), so a seam installed right after init() must take effect on
    // this same run's very first transparentPass, not wait for a window resize that may never come
    // during a --frames capture.
    if (dev_) buildPipelines(pipelineSampleCount_, pipelineColor_, pipelineDepth_);
}

bool ParticleRenderer::ensureCapacity(usize vertexCount, usize indexCount) {
    const bool growV = vertexCount > vbCapacity_;
    const bool growI = indexCount > ibCapacity_;
    if (!growV && !growI) return true;

    const usize newV = growV ? vertexCount : vbCapacity_;
    const usize newI = growI ? indexCount : ibCapacity_;

    for (u32 i = 0; i < kFramesInFlight; ++i) {
        if (growV) {
            res_->destroyBuffer(vb_[i]);
            rhi::BufferDesc bd;
            bd.bytes = static_cast<u64>(newV) * sizeof(ParticleVertex);
            bd.kind = rhi::BufferKind::Upload;
            bd.debugName = "Aver.Particles vertices";
            vb_[i] = res_->createBuffer(bd);
            if (!vb_[i]) { AVER_ERROR("[Particles] vertex buffer allocation failed"); return false; }
        }
        if (growI) {
            res_->destroyBuffer(ib_[i]);
            rhi::BufferDesc bd;
            bd.bytes = static_cast<u64>(newI) * sizeof(u32);
            bd.kind = rhi::BufferKind::Upload;
            bd.debugName = "Aver.Particles indices";
            ib_[i] = res_->createBuffer(bd);
            if (!ib_[i]) { AVER_ERROR("[Particles] index buffer allocation failed"); return false; }
        }
    }
    vbCapacity_ = newV;
    ibCapacity_ = newI;
    return true;
}

bool ParticleRenderer::buildPipelines(u32 sampleCount, rhi::Format color, rhi::Format depth) {
    if (!res_ || !vs_ || !ps_) return false;

    res_->destroyPipeline(premultPso_);
    res_->destroyPipeline(additivePso_);
    premultPso_ = additivePso_ = 0;
    // Torn down unconditionally too, even though only some callers end up rebuilding them below --
    // stale handles from a PREVIOUS sample count/format must never survive into this frame's set,
    // the same reasoning the two lines above already follow.
    res_->destroyPipeline(giPremultPso_);
    res_->destroyPipeline(giAdditivePso_);
    res_->destroyBindingSet(giBindingSet_);
    res_->destroyShader(giPs_);
    giPremultPso_ = giAdditivePso_ = 0;
    giBindingSet_ = 0;
    giPs_ = 0;

    rhi::GraphicsPipelineDesc gd;
    gd.vs = vs_;
    gd.ps = ps_;

    // Split across POSITION0 (xy) + POSITION1 (z): see ParticleVertex's own comment and
    // ParticleShaders.hpp's VSIn for why there is no single three-float attribute to declare here.
    gd.vertexLayout.stride = sizeof(ParticleVertex);
    gd.vertexLayout.attribCount = 4;
    gd.vertexLayout.attribs[0] = {rhi::VertexSemantic::Position, 0, rhi::Format::RG32Float, 0};
    gd.vertexLayout.attribs[1] = {rhi::VertexSemantic::Position, 1, rhi::Format::R32Float, 8};
    gd.vertexLayout.attribs[2] = {rhi::VertexSemantic::TexCoord, 0, rhi::Format::RG32Float, 12};
    gd.vertexLayout.attribs[3] = {rhi::VertexSemantic::Color, 0, rhi::Format::RGBA8Unorm, 20};
    static_assert(offsetof(ParticleVertex, uv) == 12, "TEXCOORD0 is declared at byte 12 above");
    static_assert(offsetof(ParticleVertex, color) == 20, "COLOR0 is declared at byte 20 above");

    // DEPTH-TESTED, DEPTH-WRITE OFF: DECIDED 1's whole point, and the exact contract
    // IRenderFeature::transparentPass documents -- see D3D12Device::endFrame's own comment on why
    // write-off is load-bearing for back-to-front blended geometry.
    gd.depth.test = true;
    gd.depth.write = false;
    // A billboard always faces the camera; there is no back face to discard.
    gd.cull = rhi::CullMode::None;

    gd.renderTargetCount = 1;
    gd.renderTargets[0] = color;
    gd.depthFormat = depth;
    // MSAA IS BAKED INTO THE PIPELINE AT CREATION (DECIDED 1): the scene target's sample count, not a
    // hardcoded 1, and rebuilt here whenever onRenderTargetsChanged reports it changed.
    gd.sampleCount = sampleCount;

    gd.blend = rhi::BlendMode::PremultipliedAlpha;
    premultPso_ = res_->createGraphicsPipeline(gd);
    gd.blend = rhi::BlendMode::Additive;
    additivePso_ = res_->createGraphicsPipeline(gd);

    // ---- the GI-sampling twins (DECIDED 4) ----
    // Only attempted while a seam is installed (see GiSeam's own comment) -- with none, giSeam_.prepare
    // is null and every particle draws through premultPso_/additivePso_ exactly as it always has.
    if (giSeam_.prepare) {
        std::string giPrelude, giDefines;
        if (giSeam_.prepare(kGiSrvBase, kGiSamplerBase, rhi::kFeatureFrameConstantRegister,
                            &giPrelude, &giDefines, giSeam_.user)) {
            rhi::ShaderDesc gsd;
            gsd.source = rhi::shaderFile("particles.hlsl").c_str();
            const std::string prelude = std::string(rhi::sharedShaderPrelude()) + giPrelude;
            gsd.prelude = prelude.c_str();
            gsd.entry = "ParticlePS";
            gsd.stage = rhi::ShaderStage::Pixel;
            // AVER_PARTICLES_GI=1 is what the #if in ParticlePS (ParticleShaders.hpp) branches on --
            // everything past it is whatever giDefines asked for (the register placement giShaderDefines()
            // itself computed against the SAME srvBase/samplerBase/cbRegister passed to prepare() above).
            const std::string defines = "AVER_PARTICLES_GI=1;" + giDefines;
            gsd.defines = defines.c_str();
            giPs_ = res_->createShader(gsd);

            if (giPs_) {
                rhi::BindingSetDesc bsd;
                bsd.srvCount = 2;
                bsd.srvKinds[kGiSrvBase + 0] = rhi::SlotKind::Texture3D;   // the GI volume
                bsd.srvKinds[kGiSrvBase + 1] = rhi::SlotKind::Texture2D;   // the shadow map (unread by
                                                                            // coneTracedIndirect, reserved
                                                                            // to match giShaderDefines()'s
                                                                            // own 2-slot shape)
                bsd.srvBaseRegister = kGiSrvBase;
                giBindingSet_ = res_->createBindingSet(bsd);
            }

            if (giPs_ && giBindingSet_) {
                rhi::GraphicsPipelineDesc gid = gd;   // same vertex layout, depth state, cull, targets
                gid.vs = vs_;                          // the GI variant never recompiles the vertex shader
                gid.ps = giPs_;
                gid.layout.srvCount = 2;
                gid.layout.samplerCount = kGiSamplerBase + 2;
                // The SAME filter/address/compare values voxi::VoxiRenderer's own giSamplers() gives its
                // s0/s1 -- see sandbox/src/SandboxApp.cpp's ensureLodMeshPipeline, the other caller of
                // this exact shape, for where that precedent was checked.
                gid.layout.samplers[kGiSamplerBase + 0].filter  = rhi::Filter::Linear;
                gid.layout.samplers[kGiSamplerBase + 0].address = rhi::AddressMode::Clamp;
                gid.layout.samplers[kGiSamplerBase + 1].filter  = rhi::Filter::ComparisonLinear;
                gid.layout.samplers[kGiSamplerBase + 1].address = rhi::AddressMode::Clamp;
                gid.layout.samplers[kGiSamplerBase + 1].compare = rhi::CompareOp::LessEqual;
                // A root CBV (zero dwords), not root constants -- see PipelineLayout::constantDwords'
                // own comment. Bound every GI draw via ctx.setConstantBuffer, never setConstants.
                gid.layout.constantDwords[rhi::kFeatureFrameConstantRegister] = 0;

                gid.blend = rhi::BlendMode::PremultipliedAlpha;
                giPremultPso_ = res_->createGraphicsPipeline(gid);
                gid.blend = rhi::BlendMode::Additive;
                giAdditivePso_ = res_->createGraphicsPipeline(gid);
            }

            if (!giPs_ || !giBindingSet_ || !giPremultPso_ || !giAdditivePso_) {
                AVER_WARN("[Particles] GI pipeline build failed; effects with receivesGI=true draw unlit this run");
                res_->destroyPipeline(giPremultPso_); res_->destroyPipeline(giAdditivePso_);
                res_->destroyBindingSet(giBindingSet_); res_->destroyShader(giPs_);
                giPremultPso_ = giAdditivePso_ = 0; giBindingSet_ = 0; giPs_ = 0;
            } else {
                AVER_INFO("[Particles] GI sampling pipelines ready");
            }
        }
        // prepare() returning false ("nothing available right now") is not an error: it is exactly
        // what a Voxi that has not finished init() yet, or hasn't attached at all, reports. Every
        // particle draws unlit until a later rebuild finds it ready.
    }

    pipelineSampleCount_ = sampleCount;
    pipelineColor_ = color;
    pipelineDepth_ = depth;

    const bool ok = premultPso_ != 0 && additivePso_ != 0;
    if (ok) {
        AVER_INFO("[Particles] pipelines (re)built: {}x MSAA", sampleCount);
    } else {
        AVER_ERROR("[Particles] pipeline build failed at {}x MSAA", sampleCount);
    }
    return ok;
}

void ParticleRenderer::onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth,
                                              u32 width, u32 height) {
    (void)width; (void)height;
    if (sampleCount == pipelineSampleCount_ && color == pipelineColor_ && depth == pipelineDepth_) return;
    buildPipelines(sampleCount, color, depth);
}

rhi::PipelineHandle ParticleRenderer::pipelineFor(rhi::BlendMode blend, bool wantsGi) const {
    // Two pipelines per GI state: ParticlePS always premultiplies rgb by alpha (ParticleShaders.hpp),
    // so of rhi::BlendMode's four values only PremultipliedAlpha and Additive compose correctly
    // against that output. Opaque/AlphaBlend are still valid DATA on an effect -- rhi::BlendMode is
    // reused wholesale, nothing here rejects them -- they simply draw through the premultiplied
    // pipeline rather than double-applying alpha against an already-premultiplied source.
    //
    // wantsGi FALLS BACK TO THE ORDINARY PIPELINE, silently, whenever the GI variant does not exist
    // -- no seam installed, prepare() said "not ready", or the build failed (buildPipelines already
    // logged why). This is the enforcement GiSeam's own header comment promises: an effect asking for
    // GI it cannot get still draws, unlit, rather than not drawing at all.
    if (wantsGi) {
        if (blend == rhi::BlendMode::Additive && giAdditivePso_) return giAdditivePso_;
        if (blend != rhi::BlendMode::Additive && giPremultPso_)  return giPremultPso_;
    }
    return blend == rhi::BlendMode::Additive ? additivePso_ : premultPso_;
}

void ParticleRenderer::transparentPass(rhi::IRenderContext& ctx) {
    if (!system_ || !res_ || !dev_) return;
    if (!premultPso_ && !additivePso_) return;

    f32 viewProj[16], invViewProjRel[16], camPosArr[3];
    if (!dev_->camera(viewProj, invViewProjRel, camPosArr)) return;
    const Vec3 camPos{camPosArr[0], camPosArr[1], camPosArr[2]};

    Vec3 right, up;
    cameraBasis(invViewProjRel, right, up);
    if (right.sizeSquared() < 0.5f || up.sizeSquared() < 0.5f) return;   // a degenerate camera this frame

    verts_.clear();
    idx_.clear();
    draws_.clear();

    system_->forEachEmitter([&](const EmitterView& ev) {
        if (!ev.effect || !ev.particles || ev.particles->empty()) return;
        const ParticleEffect& fx = *ev.effect;

        // Back-to-front WITHIN this emitter (DECIDED). See ParticleSystem::forEachEmitter's own
        // comment for what is NOT sorted: the order emitters themselves draw in.
        sortScratch_.clear();
        sortScratch_.reserve(ev.particles->size());
        for (u32 i = 0; i < ev.particles->size(); ++i) sortScratch_.push_back(i);
        std::sort(sortScratch_.begin(), sortScratch_.end(), [&](u32 a, u32 b) {
            return distSquared((*ev.particles)[a].position, camPos) >
                   distSquared((*ev.particles)[b].position, camPos);
        });

        const u32 indexOffset = static_cast<u32>(idx_.size());
        for (u32 pi : sortScratch_) {
            const Particle& p = (*ev.particles)[pi];
            const f32 t = p.lifetime > 0.0f
                ? std::clamp(p.age / p.lifetime, 0.0f, 1.0f) : 1.0f;
            const f32 halfSize = lerpf(fx.sizeStart, fx.sizeEnd, t) * 0.5f;
            f32 col[4];
            for (int k = 0; k < 4; ++k) col[k] = lerpf(fx.colorStart[k], fx.colorEnd[k], t);
            const u32 packed = packPremultiplied(col);

            const Vec3 rW = right * halfSize, uW = up * halfSize;
            const Vec3 corners[4] = {
                p.position - rW - uW, p.position + rW - uW,
                p.position + rW + uW, p.position - rW + uW,
            };
            const f32 uvs[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            const u32 base = static_cast<u32>(verts_.size());
            for (int c = 0; c < 4; ++c) {
                ParticleVertex v;
                v.pos[0] = corners[c].x; v.pos[1] = corners[c].y; v.pos[2] = corners[c].z;
                v.uv[0] = uvs[c][0]; v.uv[1] = uvs[c][1];
                v.color = packed;
                verts_.push_back(v);
            }
            const u32 quad[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
            idx_.insert(idx_.end(), quad, quad + 6);
        }

        draws_.push_back({indexOffset, static_cast<u32>(idx_.size()) - indexOffset,
                          pipelineFor(fx.blend, fx.receivesGI)});
    });

    if (draws_.empty()) return;
    if (!ensureCapacity(verts_.size(), idx_.size())) return;

    // Whether ANY draw this frame actually landed on a GI pipeline -- gates the one seam call below.
    // Checked against the pipeline handles themselves, not ParticleEffect::receivesGI again: an
    // effect can ASK for GI and still end up on the ordinary pipeline (see pipelineFor's own
    // fallback), and only the pipeline that was actually picked says whether the GI binding set and
    // frame constants are about to be needed this frame.
    bool anyGi = false;
    for (const DrawCmd& d : draws_)
        if (d.pipeline == giPremultPso_ || d.pipeline == giAdditivePso_) { anyGi = true; break; }
    const void* giCbData = nullptr;
    u32 giCbBytes = 0;
    if (anyGi && giSeam_.bind)
        giSeam_.bind(*res_, giBindingSet_, kGiSrvBase, &giCbData, &giCbBytes, giSeam_.user);

    // Rotated here, not per-emitter, so several emitters in one frame share one ring slot -- ONE draw
    // per emitter (DECIDED), not one buffer generation per emitter.
    frame_ = (frame_ + 1) % kFramesInFlight;
    const rhi::BufferHandle vb = vb_[frame_], ib = ib_[frame_];
    if (!vb || !ib) return;
    if (!res_->writeBuffer(vb, verts_.data(), verts_.size() * sizeof(ParticleVertex))) return;
    if (!res_->writeBuffer(ib, idx_.data(), idx_.size() * sizeof(u32))) return;

    // Viewport, scissor and the scene colour+depth targets are ALL already set -- the transparentPass
    // contract (RHIResources.hpp) -- so nothing here touches any of them; only the pipeline is ours.
    ctx.pushMarker("Aver.Particles");
    ctx.setVertexBuffer(vb, sizeof(ParticleVertex));
    ctx.setIndexBuffer(ib, rhi::Format::R32Uint);
    rhi::PipelineHandle bound = 0;
    for (const DrawCmd& d : draws_) {
        if (!d.pipeline) continue;
        if (d.pipeline != bound) {
            ctx.setPipeline(d.pipeline);
            bound = d.pipeline;
            // A GI pipeline's root signature declares table 0 and the b(kFeatureFrameConstantRegister)
            // CBV that the ordinary pipelines do not -- D3D12 requires every root parameter a NEWLY
            // bound root signature declares to be set again, so this fires every time `bound` becomes
            // a GI pipeline, not once per frame. Cheap (a root CBV pointer set, per
            // VoxiRenderer::bindGiResources' own precedent) and only reached at all when anyGi (above)
            // already found at least one GI draw this frame.
            if (d.pipeline == giPremultPso_ || d.pipeline == giAdditivePso_) {
                ctx.setBindingSet(giBindingSet_, 0);
                if (giCbData) ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, giCbData, giCbBytes);
            }
        }
        ctx.drawIndexed(d.indexCount, d.indexOffset, 0);
    }
    ctx.popMarker();
}

} // namespace aver::particles
