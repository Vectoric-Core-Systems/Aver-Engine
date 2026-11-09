// Software occlusion culling: toggles, per-frame buffers and diagnostics counters.
// Included from SandboxApp.hpp (inside namespace aver) after the engine headers; not a standalone header.
#pragma once

struct OcclusionState {
    // --occlusion-cull: hierarchical-Z two-pass box culling (modules/occlusion). OFF (default) never calls occluder_.
    // UNGUARDED on this side of the #if, a MANIFEST MIRROR (RENDER.OCCLUSIONCULL round-trips through it).
    bool occlusionCullEnabled_ = false;
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
    bool occlusionCullForceOff_ = false;   // --no-occlusion-cull: see setOcclusionCullForceOff's own comment
    // --occlusion-waitidle / --no-occlusion-waitidle: see setOcclusionDebugForceWaitIdle. DEFAULTS TRUE.
    bool occlusionDebugForceWaitIdleArg_ = true;
    // NON-owning: SandboxApp owns the one instance for the run and destroys it in onShutdown.
    aver::occlusion::IOcclusionCuller* occluder_ = nullptr;
    // Per-entity "the pyramid could not prove this hidden, as of last test" bit, carried across frames.
    std::unordered_map<scene::Entity, bool> occlusionVisible_;
    // This frame's reordering of [0, w.count()) so every PASS-1 (assumed-visible) index precedes PASS-2.
    std::vector<u32> occlusionOrder_;
    // This frame's world AABBs, collected in a pre-walk: the culler needs ALL of them in one call.
    std::vector<aver::occlusion::Aabb> occlusionBoxes_;
    std::vector<scene::Entity> occlusionBoxEntities_;
    std::vector<u8> occlusionResults_;
    // Diagnostics: accumulated since the process started, and the frame count they cover.
    u64  occlusionCulledAccum_ = 0, occlusionTestedAccum_ = 0;
    u32  occlusionReportFrames_ = 0;
    // MOTION-SAFE CULLING: the camera basis testBatch()'s answer was computed from one-frame-stale camera (see Occlusion.hpp's TWO-PASS).
    Vec3 occlusionBasisCamPos_{0.0f, 0.0f, 0.0f};
    Vec3 occlusionBasisForward_{1.0f, 0.0f, 0.0f};
    bool occlusionBasisValid_ = false;
    // F7: the scene's sub-rect (target pixels) that produced THIS basis's pyramid. A dock-layout drag is a discontinuity like a teleport.
    f32 occlusionBasisRect_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // F7's once-per-change diagnostic: last rect/pyramid size the "[Occlusion] testing..." line printed for.
    f32 occlusionLoggedRect_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    u32 occlusionLoggedPyramidW_ = 0xFFFFFFFFu, occlusionLoggedPyramidH_ = 0xFFFFFFFFu;
    // F8: has the "[Occlusion] idle: ..." line already fired for the CURRENT idle streak.
    bool occlusionIdleLogged_ = false;
    // 3B: this frame's copy of the two EditorConsole.hpp slots, reasserted every frame.
    bool occlusionShowCulled_ = false;
    bool occlusionCullUnderSuppression_ = false;
    // Diagnostics for the staleness detector: frames it fired on, and whether the one-time warning already fired.
    u64  occlusionStaleReadbacks_ = 0;
    bool occlusionStaleWarnedOnce_ = false;
#endif
};
