#include "GameAppInternal.hpp"

namespace aver::game {

#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK

namespace app_detail {

// ---- Save seams a composition root installs -----------------------------------------------
// Aver.Save takes function pointers rather than linking Aver.Framework; this is where they meet --
// same shape as the animation notify sink and the animation-curve provider above.

// Spawns without BeginPlay -- the whole reason aver_fw_dispatch_begin_play exists: aver_fw_spawn runs
// bind -> build_models -> beginPlay inline, so an actor spawned then patched would begin play
// against class defaults. Preview, patch, then begin.
aver::scene::Entity saveSpawnClass(const char* className, void*) {
    const i32 c = aver_fw_class_find(className);
    if (c == 0) return aver::scene::kInvalidEntity;
    const i32 e = aver_fw_spawn_preview(c, className, nullptr, nullptr, nullptr);
    return e == 0 ? aver::scene::kInvalidEntity : static_cast<aver::scene::Entity>(e);
}

const char* saveClassOf(aver::scene::Entity e, void*) {
    const i32 c = aver_fw_class_of(static_cast<i32>(e));
    return c == 0 ? nullptr : aver_fw_class_name(c);
}

void saveBeginPlay(aver::scene::Entity e, void*) {
    aver_fw_dispatch_begin_play(static_cast<i32>(e), AVER_FW_BEGIN_SPAWN);
}

// Through the framework, so OnEndPlay runs and the managed instance is released. World::destroy
// would take the entity out from under a live C# object.
void saveDestroyActor(aver::scene::Entity e, void*) { aver_fw_destroy(static_cast<i32>(e)); }

// Graph-local variables: thin forwards to the aver_fw_graph_var_* relay (framework_abi.h), field
// for field with SandboxApp.cpp's own saveHost(), since both build a save::Host from the same
// framework ABI.
i32 saveGraphVarCount(aver::scene::Entity e, void*) {
    return aver_fw_graph_var_count(static_cast<i32>(e));
}
i32 saveGraphVarAt(aver::scene::Entity e, i32 index, char* nameBuf, i32 nameBufLen,
                   u32* outKind, f32* outF, i32* outI, void*) {
    return aver_fw_graph_var_at(static_cast<i32>(e), index, nameBuf, nameBufLen,
                                reinterpret_cast<int32_t*>(outKind), outF, outI);
}
i32 saveGraphVarSet(aver::scene::Entity e, const char* name, u32 kind, f32 f, i32 i, void*) {
    return aver_fw_graph_var_set(static_cast<i32>(e), name, static_cast<int32_t>(kind), f, i);
}

// Managed entity state (framework_state_abi.h): relays, like the graph VARs above.
i32 saveStateCount(aver::scene::Entity e, void*) {
    return std::max(0, static_cast<i32>(aver_fw_state_count(static_cast<i32>(e)))) + aver::game::softBodyStateCount(e);
}
i32 saveStateAt(aver::scene::Entity e, i32 index, char* nameBuf, i32 nameCap, char* dataBuf,
                i32 dataCap, void*) {
    // The managed providers first, then the soft-body damage entry (docs/SOFTBODY.md, saved damage).
    const i32 managed = std::max(0, static_cast<i32>(aver_fw_state_count(static_cast<i32>(e))));
    if (index < managed) return aver_fw_state_at(static_cast<i32>(e), index, nameBuf, nameCap, dataBuf, dataCap);
    return aver::game::softBodyStateAt(e, index - managed, nameBuf, nameCap, dataBuf, dataCap);
}
i32 saveStateSet(aver::scene::Entity e, const char* name, const char* data, i32 dataLen, void*) {
    if (aver_fw_state_set(static_cast<i32>(e), name, data, dataLen)) return 1;
    return aver::game::softBodyStateSet(e, name, data, dataLen);
}

aver::save::Host saveHost() {
    aver::save::Host h;
    h.spawnClass    = &saveSpawnClass;
    h.classOf       = &saveClassOf;
    h.beginPlay     = &saveBeginPlay;
    h.destroyActor  = &saveDestroyActor;
    h.graphVarCount = &saveGraphVarCount;
    h.graphVarAt    = &saveGraphVarAt;
    h.graphVarSet   = &saveGraphVarSet;
    h.stateCount    = &saveStateCount;
    h.stateAt       = &saveStateAt;
    h.stateSet      = &saveStateSet;
    return h;
}

// Not saved: transient soft-body field entities, and whatever a streamer made (PCG scatter, a cooked level's chunks), which
// has its own persistence and would otherwise be restored as a second, permanent copy. `user` is the host's OriginParticipants.
bool gameSaveSkip(aver::scene::Entity e, void* user) {
    return aver::game::softBodySaveSkip(e, nullptr) || aver::game::OriginParticipants::skipStreamed(e, user);
}

i32 saveWriteProvider(const char* path, void* user) {
    auto* origin = static_cast<aver::game::OriginParticipants*>(user);
    aver::save::CaptureOptions co;
    co.host = saveHost();
    co.host.user = user;
    co.skip = &gameSaveSkip;
    if (origin) { co.originX = origin->originX(); co.originY = origin->originY(); }   // the ORIG chunk: a restore under another origin re-expresses every root
    aver::fmt::OcSaveData snap;
    std::string why;
    if (!aver::save::capture(aver::scene::World::instance(), snap, co, &why)) {
        AVER_ERROR("[Save] capture failed: {}", why);
        return 0;
    }
    if (!aver::fmt::saveOcSave(path, snap, &why)) {
        AVER_ERROR("[Save] write failed: {}", why);
        return 0;
    }
    AVER_INFO("[Save] wrote {} ({} entities)", path, snap.entities.size());
    return 1;
}

i32 saveLoadProvider(const char* path, void* user) {
    auto* origin = static_cast<aver::game::OriginParticipants*>(user);
    aver::fmt::OcSaveData snap;
    std::string why;
    if (!aver::fmt::loadOcSave(path, snap, &why)) {
        AVER_ERROR("[Save] load failed: {}", why);
        return 0;
    }
    aver::save::RestoreOptions ro;
    ro.host = saveHost();
    ro.host.user = user;
    ro.skip = &aver::game::OriginParticipants::skipStreamed;   // the teardown leaves resident chunks alone, as capture left them out
    if (origin) { ro.originX = origin->originX(); ro.originY = origin->originY(); }
    aver::game::softBodyResetForRestore();   // the restore pass replays saved damage onto rebuilt bodies
    if (!aver::save::restore(snap, aver::scene::World::instance(), ro, &why)) {
        AVER_ERROR("[Save] restore failed: {}", why);
        return 0;
    }
    return 1;
}
} // namespace app_detail

#endif // AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK

} // namespace aver::game
