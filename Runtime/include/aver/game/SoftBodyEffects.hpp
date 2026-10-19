#pragma once
// The host's half of soft-body break effects (docs/SOFTBODY.md "Break effects"): the sound sink. The soft-body system
// queues a request per break event that an .ocsoft binds a sound to and hands the survivors of its budgets to ONE sink at the end
// of the tick; this header is that sink for a host with the audio ABI. Particles need nothing from here: they go through
// aver::spawnEffectAt (aver/core/EffectSpawn.hpp), which the particle module installs.
//
// HEADER-ONLY, the SequenceHost.hpp arrangement: each host translation unit supplies its own AVER_WITH_AUDIO_ABI /
// AVER_MODULE_SOFTBODY_SCENE, so a build without the audio ABI installs nothing and the sounds are silent. Call
// installSoftBodyEffects(pathFn, user) once at startup beside installSoftBodyHost, with the same path resolver (an asset id,
// the fnv1a64 of a content-relative path, to a file path). A request's sound path is a project asset, never an OS path.
#include "aver/core/Types.hpp"

#include <string>

#if AVER_MODULE_SCENE && AVER_MODULE_SOFTBODY_SCENE

#include "aver/softbody/SoftBodySystem.hpp"

#if AVER_WITH_AUDIO_ABI
#  include "aver/audio/audio_abi.h"
#endif

namespace aver::game {
namespace detail {

struct SoftBodyEffectsState {
    std::string (*pathFn)(u64, void*) = nullptr;
    void* pathUser = nullptr;
};

inline SoftBodyEffectsState& softBodyEffectsState() {
    static SoftBodyEffectsState s;
    return s;
}

#if AVER_WITH_AUDIO_ABI
// Resolves the sound id to a path, loads it (cached by path in Aver.Audio) and plays it where the break happened.
inline void playSoftBodySound(const softbody::FxRequest& r, void*) {
    const SoftBodyEffectsState& st = softBodyEffectsState();
    if (!st.pathFn || r.sound == 0) return;
    const std::string path = st.pathFn(r.sound, st.pathUser);
    if (path.empty()) return;
    const i32 sound = aver_audio_load(path.c_str());
    if (!sound) return;
    aver_audio_play_at(sound, r.pos.x, r.pos.y, r.pos.z, r.volume, r.pitch, 0, AVER_AUDIO_BUS_SFX, r.innerCm, r.outerCm);
}
#endif

}  // namespace detail

inline void installSoftBodyEffects(std::string (*pathFn)(u64, void*), void* pathUser) {
    detail::softBodyEffectsState() = detail::SoftBodyEffectsState{pathFn, pathUser};
#if AVER_WITH_AUDIO_ABI
    softbody::softBodySystem().setSoundSink(pathFn ? &detail::playSoftBodySound : nullptr, nullptr);
#endif
}

// Stop Play / shutdown: no sink, no path function.
inline void uninstallSoftBodyEffects() {
    detail::softBodyEffectsState() = detail::SoftBodyEffectsState{};
    softbody::softBodySystem().setSoundSink(nullptr, nullptr);
}

}  // namespace aver::game

#else

namespace aver::game {
inline void installSoftBodyEffects(std::string (*)(u64, void*), void*) {}
inline void uninstallSoftBodyEffects() {}
}  // namespace aver::game

#endif
