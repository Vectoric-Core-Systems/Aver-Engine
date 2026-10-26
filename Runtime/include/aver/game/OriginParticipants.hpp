#pragma once
// OriginParticipants: the hosts' side of the floating origin (docs/plans/wave11/LARGE_WORLD.md 2.2.4, 2.2.5, 4 phase 2).
//
// One object per host. It owns the world::WorldOrigin service and registers every participant of the order table in it, in
// order: gate (0), physics (10), scene roots and PlayMobility (20), host members (30), scene systems (40), streaming (50),
// renderer (60), audio and particles (70), editor (80), script event (90). It is the ONLY caller of the other areas' shift
// entry points; each area is tested alone, this file is where they meet.
//
// A shift happens in exactly one place, slot S0 at the top of onUpdate (update()), before input, gameplay, the physics step,
// streaming and the scene flush. Every participant first answers `prepare` (it may refuse, the shift is retried next frame),
// then all `commit`. A refused shift calls nothing: the renderer's and physics' entry points have no undo.
//
// What a host hands over (Bindings) is pointers and hooks, never ownership. Types are forward-declared and every pointer is
// optional, so this header has no #if on a module and its layout is the same in every translation unit; the .cpp (in
// Aver.Runtime.Game.Core, which sees the module macros) calls what its build has. Whatever Core cannot link (audio, the
// Synapse AI instance, the occlusion culler, the editor) arrives as a hook the host fills.
#include "aver/core/Math.hpp"

#if AVER_MODULE_SCENE
#include "aver/scene/Entity.hpp"
#include "aver/world/WorldOrigin.hpp"

#include <functional>
#include <utility>
#include <vector>

namespace aver::scene { class World; }
namespace aver::rhi { class IDevice; }
namespace aver::voxi { class VoxiRenderer; }
namespace aver::fmt { struct OcNavData; }
namespace aver::synapse { class SynapseAi; }

namespace aver::game {

class PlayMobility;
class SequenceDirector;
class GameStreaming;
class GameLevel;
class GameLandscape;
class LevelFoliage;
class GameWater;

struct OriginBindings {
    // ---- scene and gameplay systems (the systems that are process singletons need no entry) ----
    PlayMobility*      mobility = nullptr;     // re-baselined, so statics stay Static (the reclassification trap)
    SequenceDirector*  sequences = nullptr;    // its base store follows the shift
    fmt::OcNavData*    nav = nullptr;          // the host's baked grid
    synapse::SynapseAi* synapseAi = nullptr;   // hearing, tactics, crowd, smart objects, EQS
    // ---- streaming ----
    GameStreaming*     streaming = nullptr;    // PCG scatter chunk worlds
    GameLevel*         level = nullptr;        // the cooked-chunk streamer, and what the gate reads about LevelStreaming / instances
    // ---- renderer ----
    rhi::IDevice*      device = nullptr;
    voxi::VoxiRenderer* voxi = nullptr;        // null until attached
    GameLandscape*     landscape = nullptr;
    LevelFoliage*      foliage = nullptr;
    GameWater*         water = nullptr;
    // ---- host members: every origin-relative position the host holds across frames (camera, eye, GI centre, drone...). ----
    // Each becomes `p - deltaF`, plain f32 per axis, like World::shiftRoots.
    std::vector<Vec3*> positions;

    // ---- hooks (all optional) ----
    // Host state that is not a bare Vec3 (order 30), after `positions`.
    std::function<void(const world::ShiftEvent&)> onHostCommit;
    // Renderer-side host state (order 60), after the renderer calls: the sandbox's occlusion basis.
    std::function<void(const world::ShiftEvent&)> onRenderCommit;
    // Audio voices and listener move by -(dx, dy, dz) cm (order 70). Core does not link the audio ABI.
    std::function<void(f32 dxCm, f32 dyCm, f32 dzCm)> audioShift;
    // Why the host cannot take a shift right now (a play transition, a gesture), or null. `urgent` is ShiftEvent::urgent.
    // A string literal: it outlives the call and is what Stats::lastDeferReason shows.
    std::function<const char*(bool urgent)> busy;
    // The editor's part (order 80): undo and redo stacks, clipboard, play snapshots, overlay signatures; its prepare is the
    // gesture gate. Empty in the game.
    std::function<world::PrepareResult(const world::ShiftEvent&)> editorPrepare;
    std::function<void(const world::ShiftEvent&)>                 editorCommit;
    // The script event (order 90). Empty = post "world.origin_changed" through the framework's event bus when it is linked
    // (sender 0, target 0, payload ints dxCm, dyCm, generation, originX, originY: framework_origin_abi.h).
    std::function<void(const world::ShiftEvent&)> postEvent;
};

class OriginParticipants {
public:
    OriginParticipants();
    ~OriginParticipants();
    OriginParticipants(const OriginParticipants&) = delete;
    OriginParticipants& operator=(const OriginParticipants&) = delete;

    OriginBindings& bindings() { return b_; }
    const OriginBindings& bindings() const { return b_; }

    // The project keys. floatingOrigin and marginCm are ProjectDesc::worldFloatingOrigin / worldRebaseMarginCm (-1 = unstated);
    // `unstatedDefault` is what an unstated WORLD.FLOATINGORIGIN means in this host (the game: on; the editor: off until its
    // own participant is complete). The service clamps the margin. allowShiftRequest is WORLD.ALLOWSHIFTREQUEST (-1 = unstated =
    // refused): whether a script's aver_fw_origin_request_shift is honoured.
    void configure(int floatingOrigin, int marginCm, bool unstatedDefault, int allowShiftRequest = -1);

    // Registers every participant, and (with the framework linked) makes this the provider the origin ABI answers from. Once;
    // later calls do nothing. Call after the bindings that never change are filled (the participants read the bindings when
    // they run, so a pointer set later is seen).
    void install();
    bool installed() const { return installed_; }

    // Slot S0. The camera is the origin-relative position the host will render from. True when a shift committed. A shift a script
    // asked for (requestShift) runs first, under the same deferral rules; a refused one is dropped, not retried.
    bool update(const Vec3& camLocalCm);

    // aver_fw_origin_request_shift: queue a shift of whole regions for the next update(). False (nothing queued) when the project
    // did not allow it (WORLD.ALLOWSHIFTREQUEST), the origin is off, or both numbers are zero. A second request before the slot
    // replaces the first. Under the follow-camera policy the camera, which moved with the world, pulls the origin straight back at
    // the same update: this is for the Manual policy and for callers that move the camera too.
    bool requestShift(i32 dxRegions, i32 dyRegions);
    bool shiftRequestsAllowed() const { return allowShiftRequest_; }

    // A level load or unload: the service moves to `o` without running a participant (nothing exists in the new scene yet),
    // and every piece that remembers an origin is told. A host calls it with the default (zero) until levels choose a region.
    void resetForLevel(world::OriginCm o = {});
    // After the level has loaded: checks its streaming radius against the rebase margin (the service warns once if they break I3).
    void levelLoaded();

    // Quiesce: while one of these lives, every shift is refused with `why` (a string literal). Level load, unload, a play
    // start or stop. Nests; the innermost reason shows.
    class Quiesce {
    public:
        Quiesce(OriginParticipants& p, const char* why) : p_(p), prev_(p.busyReason_) { p_.busyReason_ = why; }
        ~Quiesce() { p_.busyReason_ = prev_; }
        Quiesce(const Quiesce&) = delete;
        Quiesce& operator=(const Quiesce&) = delete;
    private:
        OriginParticipants& p_;
        const char* prev_;
    };

    world::WorldOrigin& service() { return origin_; }
    const world::WorldOrigin& service() const { return origin_; }
    world::OriginCm origin() const { return origin_.origin(); }
    u32 generation() const { return origin_.generation(); }

    // The participants in commit order, for the per-participant readout (Stats::lastParticipantMs is indexed the same way).
    usize participantCount() const { return order_.size(); }
    const char* participantName(usize i) const { return i < order_.size() ? order_[i].second : ""; }

    // ---- saves ----
    // True when `e` was made by a streamer (PCG scatter or a cooked chunk): it has its own persistence, so a save must not
    // capture it (and a restore must not tear it down). `self` is an OriginParticipants*.
    bool ownsStreamed(scene::Entity e) const;
    static bool skipStreamed(scene::Entity e, void* self);   // save::SkipFn-shaped
    // The origin a save is written under and restored into (CaptureOptions / RestoreOptions originX, originY).
    i64 originX() const { return origin_.origin().x; }
    i64 originY() const { return origin_.origin().y; }

private:
    void reg(const char* name, i32 order, std::function<world::PrepareResult(const world::ShiftEvent&)> prepare,
             std::function<void(const world::ShiftEvent&)> commit);

    OriginBindings b_;
    world::WorldOrigin origin_;
    bool installed_ = false;
    bool allowShiftRequest_ = false;
    bool providerSet_ = false;           // this object is the origin ABI's provider; the destructor clears it
    bool requestPending_ = false;
    i32  requestDx_ = 0, requestDy_ = 0;
    const char* busyReason_ = nullptr;
    bool physicsLive_ = false;   // prepare found a physics world; commit shifts it
    std::vector<std::pair<i32, const char*>> order_;   // (order, name), kept in the service's order
};

} // namespace aver::game

#endif // AVER_MODULE_SCENE
