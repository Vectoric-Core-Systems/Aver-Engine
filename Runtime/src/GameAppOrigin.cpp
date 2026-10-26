// GameApp's side of the floating origin (docs/plans/wave11/LARGE_WORLD.md 2.2.4): the bindings the participants run against,
// and slot S0. The participants themselves are in OriginParticipants.cpp (Aver.Runtime.Game.Core, shared with the editor).
#include "aver/game/GameApp.hpp"

#if AVER_MODULE_SCENE

#include "aver/core/Log.hpp"
#include "aver/runtime/Engine.hpp"

#if AVER_WITH_AUDIO_ABI
#  include "aver/audio/audio_abi.h"
#endif
#if AVER_MODULE_AUDIO_SCENE
#  include "aver/audio/AudioScene.hpp"
#endif
#if AVER_WITH_SYNAPSE_AI
#  include "aver/synapse/synapse_ai_abi.h"
#  include "aver/synapse/SynapseAi.hpp"
#endif

namespace aver::game {

void GameApp::initOrigin(Engine& e) {
    if (origin_.installed()) return;
    OriginBindings& b = origin_.bindings();
    b.device = e.device();
    b.mobility = &playMobility_;
    b.sequences = &sequenceDirector_;
    b.streaming = &streaming_;
    b.level = &level_;
#if AVER_MODULE_SYNAPSE_SCENE
    b.nav = &gameNav_;
#endif
#if AVER_WITH_SYNAPSE_AI
    b.synapseAi = static_cast<synapse::SynapseAi*>(aver_syn_ai_instance());
#endif
#if AVER_MODULE_LANDSCAPE
    b.landscape = &landscape_;
#endif
#if AVER_MODULE_VOXI
    b.foliage = &levelFoliage_;
#endif
#if AVER_MODULE_FLUIDS
    b.water = &water_;
#endif
    // The camera, the eye the last frame rendered from, and the GI volume centre: origin-relative, held across frames.
    b.positions = {&camPos_, &eye_, &giCenter_};
#if AVER_WITH_AUDIO_ABI
    b.audioShift = [](f32 dx, f32 dy, f32 dz) {
        aver_audio_origin_shift(dx, dy, dz);
#  if AVER_MODULE_AUDIO_SCENE
        audio::audioSceneSystem().rebase(dx, dy, dz);
#  endif
    };
#endif
    // The shipped game takes the floating origin by default; WORLD.FLOATINGORIGIN 0 keeps the old single frame.
    origin_.configure(project_.worldFloatingOrigin, project_.worldRebaseMarginCm, true, project_.worldAllowShiftRequest);
    origin_.install();
}

void GameApp::tickOrigin(Engine& e) {
    if (!origin_.installed()) return;
    OriginBindings& b = origin_.bindings();
    b.device = e.device();
#if AVER_MODULE_VOXI
    b.voxi = voxiAttached_ ? &voxiRenderer_ : nullptr;
#endif
    origin_.update(camPos_);   // S0: prepare all, commit all, before anything reads a position this frame
}

void GameApp::loadStartMapAtOrigin(const std::string& overridePath) {
    OriginParticipants::Quiesce quiet(origin_, "a level is loading");
    origin_.resetForLevel();
    level_.loadStartMap(project_, content_, overridePath);
    origin_.levelLoaded();
}

} // namespace aver::game

#endif // AVER_MODULE_SCENE
