// NrdLinkTest -- proves NVIDIA NRD is actually LINKED and actually answers, with no GPU.
//
// WHAT A "LINK TEST" IS FOR HERE, because the name undersells it. NRD is a planner, not a renderer
// (see modules/render.nrd/include/aver/render/nrd/NrdDenoiser.hpp): everything it does on the CPU
// -- deciding which of its 159 shader permutations run this frame, in what order, reading and
// writing which slots, with what constants -- happens without a device. So the interesting half of
// this integration is testable here, and only the recording of the dispatches is not. A green run
// means the vendored library builds, the shaders were compiled and embedded, the instance can be
// created, and it returns a coherent plan. A red one names which of those broke.
//
// It also holds two things to account that are otherwise silent:
//
//   * THE HEADER AND THE LIBRARY AGREE ON A VERSION. A stale NRD.lib against a newer header does
//     not fail to link -- the symbols are the same -- it just reads structures with the wrong
//     shapes, and the first symptom is a wrong number deep in a constant buffer.
//   * EVERY POOL FORMAT NRD ASKS FOR HAS AN RHI EQUIVALENT. NRD's format list is wider than
//     Aver.RHI's, and a format with no mapping means a texture the engine cannot allocate -- so the
//     denoiser could not run even with every other piece in place. This test names the gap rather
//     than letting it surface as a null texture handle later.
#include "aver/render/nrd/NrdDenoiser.hpp"
#include "aver/core/Log.hpp"

#include <string>
#include <vector>

using namespace aver;
using namespace aver::render::nrd;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// Column-major identity, the convention FrameSettings documents.
static const f32 kIdentity[16] = {
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1,
};

static const char* roleName(SlotRole r) {
    switch (r) {
        case SlotRole::InViewZ:               return "IN_VIEWZ";
        case SlotRole::InMotionVectors:       return "IN_MV";
        case SlotRole::InNormalRoughness:     return "IN_NORMAL_ROUGHNESS";
        case SlotRole::InDiffuseHitDistance:  return "IN_DIFF_HITDIST";
        case SlotRole::InPenumbra:            return "IN_PENUMBRA";
        case SlotRole::InTranslucency:        return "IN_TRANSLUCENCY";
        case SlotRole::OutDiffuseHitDistance: return "OUT_DIFF_HITDIST";
        case SlotRole::OutShadowTranslucency: return "OUT_SHADOW_TRANSLUCENCY";
        case SlotRole::PermanentPool:         return "PERMANENT_POOL";
        case SlotRole::TransientPool:         return "TRANSIENT_POOL";
        case SlotRole::Other:                 return "other";
    }
    return "?";
}

int main() {
    AVER_INFO("=== NrdLinkTest ===");

    if (!Denoiser::available()) {
        // NOT A FAILURE, and the distinction is the whole reason cmake/AverNRD.cmake is allowed to
        // turn NRD off by itself: a machine with no offline shader compiler builds an engine that
        // renders with the hand-written filter, and this suite must stay green there. The line
        // below is what tells a reader which of the two engines they just built.
        AVER_INFO("  SKIP  NRD is not in this build (AVER_WITH_NRD=OFF) -- the engine falls back "
                  "to the hand-written denoiser, and nothing below applies");
        AVER_INFO("=== NrdLinkTest: skipped ===");
        return 0;
    }

    // ---- the library is the one the header describes -------------------------------------------
    AVER_INFO("compiled against: {}", Denoiser::versionString());
    u32 major = 0, minor = 0, build = 0;
    check(Denoiser::libraryVersion(major, minor, build), "GetLibraryDesc() answers");
    AVER_INFO("linked library:   NRD {}.{}.{}", major, minor, build);
    {
        const std::string expect = "NRD " + std::to_string(major) + "." + std::to_string(minor)
                                   + "." + std::to_string(build);
        check(expect == Denoiser::versionString(),
              "the linked library's version matches the header this module compiled against");
    }

    u32 normalEnc = 99, roughEnc = 99;
    check(Denoiser::encodings(normalEnc, roughEnc), "the shader encodings are reported");
    AVER_INFO("shader encodings: normal={} roughness={}  (the G-buffer must be packed to match)",
              normalEnc, roughEnc);

    // ---- an instance can be created -------------------------------------------------------------
    // REBLUR_DIFFUSE_OCCLUSION specifically: it is the denoiser NRD was vendored for, filtering the
    // sky-occlusion ray that measurement put at 78% of the engine's remaining speckle.
    const DenoiserKind kinds[] = {DenoiserKind::ReblurDiffuseOcclusion};
    Denoiser den;
    check(den.create(kinds, 1), "CreateInstance(REBLUR_DIFFUSE_OCCLUSION)");
    if (!den.valid()) {
        AVER_ERROR("=== NrdLinkTest FAILED === ({} failure(s))", g_failures + 1);
        return 1;
    }

    // ---- the shaders were built and embedded ----------------------------------------------------
    const InstanceLayout lay = den.layout();
    check(lay.pipelineCount > 0, "the instance describes at least one pipeline");
    AVER_INFO("pipelines: {}   permanent pool: {}   transient pool: {}",
              lay.pipelineCount, lay.permanentPoolSize, lay.transientPoolSize);

    u32 withDxil = 0, withSpirv = 0, withNeither = 0, withRanges = 0;
    u64 bytecodeBytes = 0;
    for (u32 i = 0; i < lay.pipelineCount; ++i) {
        const PipelineInfo& p = lay.pipelines[i];
        if (p.dxil.valid())  { ++withDxil;  bytecodeBytes += p.dxil.size; }
        if (p.spirv.valid()) { ++withSpirv; bytecodeBytes += p.spirv.size; }
        if (!p.dxil.valid() && !p.spirv.valid()) {
            ++withNeither;
            AVER_ERROR("  pipeline {} '{}' has no bytecode at all", i,
                       p.debugName ? p.debugName : "?");
        }
        if (p.rangeCount > 0 && p.ranges != nullptr) { ++withRanges; }
    }
    AVER_INFO("bytecode: {} DXIL, {} SPIR-V, {} KiB total", withDxil, withSpirv,
              bytecodeBytes / 1024);
    // THE CENTRAL CLAIM OF THIS SUITE. An NRD that builds but embeds no bytecode is exactly what
    // "vendored but cannot run" looked like before this wiring existed -- the C++ compiled, the
    // library linked, and every pipeline was empty.
    check(withNeither == 0, "every pipeline carries bytecode for at least one backend");
    check(withRanges == lay.pipelineCount, "every pipeline declares its resource ranges");

    // ---- the binding model is what a root signature has to be built to ---------------------------
    const BindingModel& b = lay.binding;
    AVER_INFO("bindings: cbv/samplers in space {}, resources in space {}; b{}, s{}, t/u{}",
              b.constantBufferAndSamplersSpace, b.resourcesSpace, b.constantBufferRegister,
              b.samplersBaseRegister, b.resourcesBaseRegister);
    AVER_INFO("constant buffer: up to {} bytes; {} immutable sampler(s); entry point '{}'",
              b.constantBufferMaxDataSize, b.samplerCount, b.entryPoint ? b.entryPoint : "?");
    check(b.entryPoint != nullptr && b.entryPoint[0] != '\0', "an entry point name is reported");
    check(b.constantBufferMaxDataSize > 0, "a constant buffer size is reported");
    check(b.samplerCount > 0, "at least one immutable sampler is reported");

    // ---- every pool texture is a format this engine can allocate ---------------------------------
    u32 unmapped = 0;
    auto checkPool = [&unmapped](const char* which, const PoolTexture* pool, u32 n) {
        for (u32 i = 0; i < n; ++i) {
            if (pool[i].format == rhi::Format::Unknown) {
                ++unmapped;
                AVER_ERROR("  {} pool [{}] wants {}, which Aver.RHI has no equivalent for",
                           which, i, pool[i].nrdFormatName ? pool[i].nrdFormatName : "?");
            }
        }
    };
    checkPool("permanent", lay.permanentPool, lay.permanentPoolSize);
    checkPool("transient", lay.transientPool, lay.transientPoolSize);
    check(unmapped == 0, "every pool texture format maps onto an rhi::Format");

    // ---- it plans a frame ------------------------------------------------------------------------
    FrameSettings fs{};
    for (u32 i = 0; i < 16; ++i) {
        fs.viewToClip[i] = fs.viewToClipPrev[i] = kIdentity[i];
        fs.worldToView[i] = fs.worldToViewPrev[i] = kIdentity[i];
    }
    // A perspective-shaped clip matrix rather than identity: NRD derives its unproject constants
    // from this, and an identity projection makes every derived frustum quantity degenerate. These
    // are the terms of a standard 90-degree, 16:9, reverse-Z-free projection.
    fs.viewToClip[0]  = 1.0f / 1.7777778f;
    fs.viewToClip[5]  = 1.0f;
    fs.viewToClip[10] = 1.0f;
    fs.viewToClip[11] = 1.0f;
    fs.viewToClip[14] = -0.1f;
    fs.viewToClip[15] = 0.0f;
    for (u32 i = 0; i < 16; ++i) { fs.viewToClipPrev[i] = fs.viewToClip[i]; }

    fs.resourceWidth  = 1920;
    fs.resourceHeight = 1080;
    fs.denoisingRange = 10000.0f;
    fs.timeDeltaSeconds = 1.0f / 60.0f;
    fs.frameIndex = 0;
    fs.resetHistory = true;
    check(den.setFrameSettings(fs), "SetCommonSettings accepts a 1920x1080 frame");

    const u32 ids[] = {0};
    const Dispatch* disp = nullptr;
    u32 dispCount = 0;
    check(den.dispatches(ids, 1, disp, dispCount), "GetComputeDispatches answers");
    check(dispCount > 0, "the first frame plans at least one dispatch");
    AVER_INFO("frame 0: {} dispatch(es)", dispCount);

    // Every dispatch has to be recordable: a pipeline that exists, a grid that covers something,
    // and slots to bind. A zero grid is the specific shape of "NRD was told a resolution it did
    // not believe" and would silently do nothing on the GPU.
    u32 badPipeline = 0, zeroGrid = 0, noBindings = 0, overlongConstants = 0;
    bool sawViewZ = false, sawMv = false, sawNormal = false, sawInHitDist = false,
         sawOutHitDist = false;
    for (u32 i = 0; i < dispCount; ++i) {
        const Dispatch& d = disp[i];
        if (d.pipelineIndex >= lay.pipelineCount)              { ++badPipeline; }
        if (d.groupsX == 0 || d.groupsY == 0)                  { ++zeroGrid; }
        if (d.bindingCount == 0 || d.bindings == nullptr)      { ++noBindings; }
        if (d.constantsSize > b.constantBufferMaxDataSize)     { ++overlongConstants; }
        for (u32 s = 0; s < d.bindingCount; ++s) {
            switch (d.bindings[s].role) {
                case SlotRole::InViewZ:               sawViewZ = true; break;
                case SlotRole::InMotionVectors:       sawMv = true; break;
                case SlotRole::InNormalRoughness:     sawNormal = true; break;
                case SlotRole::InDiffuseHitDistance:  sawInHitDist = true; break;
                case SlotRole::OutDiffuseHitDistance: sawOutHitDist = true; break;
                default: break;
            }
        }
    }
    check(badPipeline == 0, "every dispatch names a pipeline the layout declared");
    check(zeroGrid == 0, "every dispatch has a non-empty thread-group grid");
    check(noBindings == 0, "every dispatch binds at least one slot");
    check(overlongConstants == 0, "no dispatch exceeds the reported constant buffer size");

    // The four inputs and one output third_party/nrd/AVER_README.md names as the prerequisites.
    // Asserting them here means the list in that document is checked against the library rather
    // than remembered.
    check(sawViewZ,      "the plan reads IN_VIEWZ");
    check(sawMv,         "the plan reads IN_MV");
    check(sawNormal,     "the plan reads IN_NORMAL_ROUGHNESS");
    check(sawInHitDist,  "the plan reads IN_DIFF_HITDIST");
    check(sawOutHitDist, "the plan writes OUT_DIFF_HITDIST");

    // ---- a second frame, so the temporal state is exercised --------------------------------------
    // Frame 0 was a history reset, which takes a different path through NRD's tile classification.
    // A denoiser that only works on its first frame is a denoiser that does nothing.
    fs.frameIndex   = 1;
    fs.resetHistory = false;
    check(den.setFrameSettings(fs), "SetCommonSettings accepts a second, non-reset frame");
    const Dispatch* disp1 = nullptr;
    u32 dispCount1 = 0;
    check(den.dispatches(ids, 1, disp1, dispCount1), "the second frame plans");
    check(dispCount1 > 0, "the second frame plans at least one dispatch");
    AVER_INFO("frame 1: {} dispatch(es)", dispCount1);

    // A tiny window of what the plan actually looks like. Not a check -- it is here because the
    // first question anyone integrating this asks is "what does it want me to do", and the answer
    // being in the test output is cheaper than a doc that goes stale.
    const u32 show = dispCount1 < 4 ? dispCount1 : 4;
    for (u32 i = 0; i < show; ++i) {
        const Dispatch& d = disp1[i];
        std::string slots;
        for (u32 s = 0; s < d.bindingCount && s < 8; ++s) {
            slots += (s ? ", " : "");
            slots += (d.bindings[s].cls == ResourceClass::StorageTexture ? "uav:" : "srv:");
            slots += roleName(d.bindings[s].role);
        }
        AVER_INFO("  [{}] {} -> pipeline {}, {}x{} groups, {} B constants | {}", i,
                  d.name ? d.name : "?", d.pipelineIndex, d.groupsX, d.groupsY, d.constantsSize,
                  slots);
    }

    den.destroy();
    check(!den.valid(), "destroy() releases the instance");

    if (g_failures != 0) {
        AVER_ERROR("=== NrdLinkTest FAILED === ({} failure(s))", g_failures);
        return 1;
    }
    AVER_INFO("=== NrdLinkTest passed ===");
    return 0;
}
