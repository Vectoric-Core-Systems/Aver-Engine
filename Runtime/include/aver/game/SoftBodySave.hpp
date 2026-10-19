#pragma once
// Saved soft-body damage (docs/SOFTBODY.md "Saved damage"): the `softbody` channel of Aver.Save's `$State` component, chained
// behind the framework's managed-state channels in each host's save relay. The `$State` seam has ONE managed provider
// (aver_fw_state_*, gameplay abilities install it), so the soft-body system is not a second provider: a host adds its
// count to the framework's, forwards the indices at or above the framework's count to softBodyStateAt, and tries the
// framework first in the setter, falling back to softBodyStateSet.
//
//   saveStateCount(e)  = aver_fw_state_count(e) + softBodyStateCount(e)
//   saveStateAt(e, i)  = i < frameworkCount ? aver_fw_state_at(e, i, ...) : softBodyStateAt(e, i - frameworkCount, ...)
//   saveStateSet(e, n) = aver_fw_state_set(e, ...) ? 1 : softBodyStateSet(e, ...)
//
// and set CaptureOptions::skip = &softBodySaveSkip (pieces, cap entities and spawned field entities are not captured: a
// restore recreates the pieces from the replayed tear log), and call softBodyResetForRestore() before save::restore.
// HEADER-ONLY, the SequenceHost.hpp arrangement: a build without the soft-body module gets no-ops, so a host calls these
// unconditionally.
#include "aver/core/Types.hpp"
#include "aver/scene/Entity.hpp"

#include <cstring>
#include <string>
#include <string_view>

#if AVER_MODULE_SCENE && AVER_MODULE_SOFTBODY_SCENE

#include "aver/scene/World.hpp"
#include "aver/softbody/SoftBodySystem.hpp"

namespace aver::game {

// 1 when the entity's soft body has damage worth saving, else 0.
inline i32 softBodyStateCount(scene::Entity e) {
    std::string payload;
    return softbody::softBodySystem().saveState(e, payload) ? 1 : 0;
}

// The same shape as aver_fw_state_at: copies as much as fits and returns the payload's FULL length, or -1 for a bad index.
inline i32 softBodyStateAt(scene::Entity e, i32 index, char* nameBuf, i32 nameCap, char* dataBuf, i32 dataCap) {
    if (index != 0) return -1;
    std::string payload;
    if (!softbody::softBodySystem().saveState(e, payload)) return -1;
    if (nameBuf && nameCap > 0) {
        const char* name = "softbody";
        const i32 n = static_cast<i32>(std::strlen(name));
        const i32 copy = n < nameCap - 1 ? n : nameCap - 1;
        std::memcpy(nameBuf, name, static_cast<usize>(copy));
        nameBuf[copy] = '\0';
    }
    if (dataBuf && dataCap > 0) {
        const i32 copy = static_cast<i32>(payload.size()) < dataCap ? static_cast<i32>(payload.size()) : dataCap;
        std::memcpy(dataBuf, payload.data(), static_cast<usize>(copy));
    }
    return static_cast<i32>(payload.size());
}

// 1 if the channel is ours and the payload is well formed (it is applied when the entity's entry is built).
inline i32 softBodyStateSet(scene::Entity e, const char* name, const char* data, i32 dataLen) {
    if (!name || std::strcmp(name, "softbody") != 0 || !data || dataLen <= 0) return 0;
    return softbody::softBodySystem().restoreState(e, std::string_view(data, static_cast<usize>(dataLen))) ? 1 : 0;
}

// For save::CaptureOptions::skip: debris and cap entities (kSoftBodyPiece) and live spawned field entities.
inline bool softBodySaveSkip(scene::Entity e, void*) {
    scene::World& w = scene::World::instance();
    if (const auto* sb = w.component<scene::CSoftBody>(e, scene::kComponentSoftBody))
        if (sb->flags & scene::kSoftBodyPiece) return true;
    return softbody::softBodySystem().isTransientFieldEntity(e);
}

// Before save::restore: drops bodies, debris, queued pulses and stale blobs so the restored entities start clean.
inline void softBodyResetForRestore() { softbody::softBodySystem().clear(); }

}  // namespace aver::game

#else

namespace aver::game {
inline i32 softBodyStateCount(scene::Entity) { return 0; }
inline i32 softBodyStateAt(scene::Entity, i32, char*, i32, char*, i32) { return -1; }
inline i32 softBodyStateSet(scene::Entity, const char*, const char*, i32) { return 0; }
inline bool softBodySaveSkip(scene::Entity, void*) { return false; }
inline void softBodyResetForRestore() {}
}  // namespace aver::game

#endif
