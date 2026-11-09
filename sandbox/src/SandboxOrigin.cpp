// The editor's side of the floating origin (LARGE_WORLD 2.2.4, 2.7): the bindings the participants run against, and slot S0.
// The participants are shared with the game (Runtime/src/OriginParticipants.cpp). The editor's own part (undo and redo stacks,
// clipboard, play snapshots, the gesture gate) is order 80, SandboxEditorOrigin.cpp, and plugs in through OriginBindings::editorPrepare /
// editorCommit. The editor still keeps its single frame unless a project asks for the origin (WORLD.FLOATINGORIGIN 1): it cannot yet
// WRITE a level from a shifted scene frame, so saves are refused away from the first region (originBlocksSave).
//
// New file, not new members of a big translation unit: the Debug COFF section cap rules out growing SandboxApp*.cpp.
#include "SandboxApp.hpp"

#if AVER_MODULE_SCENE

#  if AVER_WITH_AUDIO_ABI
#    include "aver/audio/audio_abi.h"
#  endif
#  if AVER_MODULE_AUDIO_SCENE
#    include "aver/audio/AudioScene.hpp"
#  endif
#  if AVER_WITH_SYNAPSE_AI
#    include "aver/synapse/SynapseAi.hpp"
#    include "aver/synapse/synapse_ai_abi.h"
#  endif

namespace aver {

void SandboxApp::initOrigin(Engine& e) {
    game::OriginBindings& b = origin_.bindings();
    b.device = e.device();
    b.mobility = &play_.playMobility_;
    b.streaming = &streaming_;
    b.level = &level_;
#  if AVER_MODULE_SYNAPSE_SCENE
    b.nav = &dbgOv_.nav_;
#  endif
#  if AVER_WITH_SYNAPSE_AI
    b.synapseAi = static_cast<synapse::SynapseAi*>(aver_syn_ai_instance());
#  endif
#  if AVER_MODULE_LANDSCAPE
    b.landscape = &landscape_;
#  endif
#  if AVER_MODULE_VOXI
    b.foliage = &levelFoliage_;
#  endif
#  if AVER_MODULE_FLUIDS
    b.water = &water_;
#  endif
    // Every origin-relative position the editor holds across frames (2.2.5, host members).
    b.positions = {&viewState_.camPos_, &viewState_.viewPosOv_, &harness_.camWanderBasePos_, &camPosOverride_, &rset_.giCenter_, &drone_.dronePos_, &drone_.droneLastPos_, &viewState_.eye_};
#  if AVER_MODULE_LANDSCAPE
    b.positions.push_back(&landTool_.sculptCursor_);
#  endif
#  if AVER_MODULE_OCCLUSION
    b.positions.push_back(&occl_.occlusionBasisCamPos_);
#  endif
#  if AVER_MODULE_FRAMEWORK
    b.positions.push_back(&harness_.pieCamRef_);
    b.positions.push_back(&harness_.vmRef_);
#  endif
#  if AVER_MODULE_OCCLUSION
    // The culler's answer was computed from the old basis: it reports "not exact" once, so nothing is culled for a frame.
    b.onRenderCommit = [this](const world::ShiftEvent&) { if (occl_.occluder_) occl_.occluder_->invalidateBasis(); };
#  endif
    // A project that is still loading, or Play on its way out (the reload that restores the level), is not a moment to move the world.
    b.busy = [this](bool) -> const char* {
        if (projectLoading_) return "a project is loading";
        if (play_.pieReloadPending_) return "play is stopping";
        return nullptr;
    };
#  if AVER_WITH_AUDIO_ABI
    b.audioShift = [](f32 dx, f32 dy, f32 dz) {
        aver_audio_origin_shift(dx, dy, dz);
#    if AVER_MODULE_AUDIO_SCENE
        audio::audioSceneSystem().rebase(dx, dy, dz);
#    endif
    };
#  endif
    initEditorOrigin();   // the order-80 participant: gesture gate and rebasers (SandboxEditorOrigin.cpp)
    configureOrigin();
    origin_.install();
}

void SandboxApp::configureOrigin() {
    // Off unless the project states WORLD.FLOATINGORIGIN 1: see the header comment.
    origin_.configure(projSess_.project_.worldFloatingOrigin, projSess_.project_.worldRebaseMarginCm, false, projSess_.project_.worldAllowShiftRequest);
}

void SandboxApp::tickOrigin(Engine& e) {
    if (!origin_.installed()) return;
    game::OriginBindings& b = origin_.bindings();
    b.device = e.device();
#  if AVER_MODULE_VOXI
    b.voxi = rset_.voxiAttached_ ? &voxiRenderer_ : nullptr;
#  endif
    const u32 generation = origin_.generation();
    // A world.rebase or world.tp typed in the console runs here, ahead of the camera-driven update, never in the UI pass.
    bool ok = true;
    const std::string report = editor::applyWorldRequest(worldHost_, ok);
    if (!report.empty()) {
        if (ok) AVER_INFO("[World] {}", report);
        else AVER_WARN("[World] {}", report);
    }
    origin_.update(viewState_.camPos_);   // S0: prepare all, commit all, before anything reads a position this frame
    if (origin_.generation() != generation) afterOriginShift(e);
}

} // namespace aver

#endif // AVER_MODULE_SCENE
