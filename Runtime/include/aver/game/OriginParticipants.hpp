#pragma once
// Host side of the floating origin. One object per host: it owns the world::WorldOrigin service and registers every participant
// in order: gate (0), physics (10), roots and PlayMobility (20), host members (30), scene systems (40), streaming (50),
// renderer (60), audio and particles (70), editor (80), script event (90). The only caller of the areas' shift entry points.
// A shift runs at slot S0 in update(): every participant answers `prepare` (a refusal retries next frame), then all `commit`; a refusal calls nothing.
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

// Pointers and hooks, never ownership; every pointer is optional.
struct OriginBindings {
    PlayMobility*      mobility = nullptr;     // re-baselined so statics stay Static
    SequenceDirector*  sequences = nullptr;    // its base store follows the shift
    fmt::OcNavData*    nav = nullptr;
    synapse::SynapseAi* synapseAi = nullptr;
    GameStreaming*     streaming = nullptr;
    GameLevel*         level = nullptr;        // the cooked-chunk streamer; the gate reads it
    rhi::IDevice*      device = nullptr;
    voxi::VoxiRenderer* voxi = nullptr;        // null until attached
    GameLandscape*     landscape = nullptr;
    LevelFoliage*      foliage = nullptr;
    GameWater*         water = nullptr;
    // Origin-relative positions: each becomes `p - deltaF`, plain f32 per axis, like World::shiftRoots.
    std::vector<Vec3*> positions;

    // Host state that is not a bare Vec3 (order 30), after `positions`.
    std::function<void(const world::ShiftEvent&)> onHostCommit;
    // Renderer-side host state (order 60), after the renderer calls.
    std::function<void(const world::ShiftEvent&)> onRenderCommit;
    // Core does not link the audio ABI.
    std::function<void(f32 dxCm, f32 dyCm, f32 dzCm)> audioShift;
    // Why the host cannot take a shift now, or null. A string literal; Stats::lastDeferReason shows it.
    std::function<const char*(bool urgent)> busy;
    // The editor's part (order 80); its prepare is the gesture gate. Empty in the game.
    std::function<world::PrepareResult(const world::ShiftEvent&)> editorPrepare;
    std::function<void(const world::ShiftEvent&)>                 editorCommit;
    // Empty = post "world.origin_changed" on the framework bus (payload ints dxCm, dyCm, generation, originX, originY).
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

    // floatingOrigin, marginCm: ProjectDesc::worldFloatingOrigin / worldRebaseMarginCm (-1 = unstated). unstatedDefault: the game
    // uses on, the editor off until its participant is complete. allowShiftRequest: -1 = refused.
    void configure(int floatingOrigin, int marginCm, bool unstatedDefault, int allowShiftRequest = -1);

    // Idempotent. With the framework linked, makes this the origin ABI's provider. Call after the bindings are filled;
    // participants read them when they run.
    void install();
    bool installed() const { return installed_; }

    // Slot S0. The camera is the origin-relative position the host renders from. True when a shift committed. A script's
    // requested shift runs first under the same deferral rules; a refused one is dropped, not retried.
    bool update(const Vec3& camLocalCm);

    // Queues a shift of whole regions for the next update(). False (nothing queued) when not allowed (WORLD.ALLOWSHIFTREQUEST),
    // the origin is off, or both numbers are zero. A second request replaces the first. Under follow-camera the camera pulls
    // the origin back in the same update, so this suits the Manual policy.
    bool requestShift(i32 dxRegions, i32 dyRegions);
    bool shiftRequestsAllowed() const { return allowShiftRequest_; }

    // Level load or unload: moves the service without running a participant, and tells every piece that remembers an origin.
    void resetForLevel(world::OriginCm o = {});
    // Checks the streaming radius against the rebase margin (warns once if they break I3).
    void levelLoaded();

    // While it lives, every shift is refused with `why`. Nests; the innermost reason shows.
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

    // Commit order; Stats::lastParticipantMs is indexed the same way.
    usize participantCount() const { return order_.size(); }
    const char* participantName(usize i) const { return i < order_.size() ? order_[i].second : ""; }

    // True when a streamer (PCG scatter or a cooked chunk) made `e`: saves must not capture it, and restores must not tear it down.
    bool ownsStreamed(scene::Entity e) const;
    static bool skipStreamed(scene::Entity e, void* self);
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
    std::vector<std::pair<i32, const char*>> order_;
};

} // namespace aver::game

#endif // AVER_MODULE_SCENE
