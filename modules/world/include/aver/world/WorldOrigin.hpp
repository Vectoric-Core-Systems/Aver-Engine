#pragma once
// The floating-origin service: owns the origin O, the shift generation and an ordered participant list
// (docs/plans/wave11/LARGE_WORLD.md 2.2). No Scene dependency: the hosts register what they own.
//
// One shift is two phases. Every participant first answers `prepare` (no mutation; it may refuse, and
// the shift is retried next frame), then the origin and generation change and every `commit` runs
// (cannot fail). A shift is frame-thread only, at the top of onUpdate, before anything reads a position.
//
// FROZEN by phase 0 of LARGE_WORLD: the other areas code against this header.
#include "aver/world/WorldPos.hpp"

#include <functional>
#include <vector>

namespace aver::world {

// Fixed: lockstep/replay/net -- the service never shifts (update and shiftBy both refuse); reset still works.
// Manual: tests and the console -- update never shifts, shiftBy does.
enum class OriginPolicy : u8 { FollowCamera, Fixed, Manual };

struct ShiftEvent {
    OriginCm oldOrigin, newOrigin;
    Vec3d    deltaCm;          // newOrigin - oldOrigin; every local position changes by -deltaCm
    Vec3     deltaF;           // the same, f32 (exact by I4)
    u32      generation;       // the value AFTER the shift; starts at 0, +1 per commit
    bool     urgent;           // |camLocal| > 1'000'000 cm on a shifted axis: participants must not defer
};
struct PrepareResult { bool ok = true; const char* deferReason = nullptr; };
struct Participant {
    const char* name = "";
    i32 order = 0;
    std::function<PrepareResult(const ShiftEvent&)> prepare;   // no mutation; may refuse. Empty = always ok.
    std::function<void(const ShiftEvent&)>          commit;    // must not fail. Empty = nothing to do.
};

inline constexpr i32 kOriginUrgentCm = 1'000'000;   // beyond this on a shifted axis a shift is `urgent`

class WorldOrigin {
public:
    struct Settings { bool enabled = true; i32 marginCm = kRebaseMarginDefaultCm; OriginPolicy policy = OriginPolicy::FollowCamera; };
    static constexpr u32 kMaxTimedParticipants = 32;

    void  configure(const Settings&);                     // clamps the margin to [0, kRebaseMarginMaxCm], logs a clamp once
    const Settings& settings() const { return settings_; }
    // I3: the sub-millimetre cliff holds while margin + radius <= kRebaseMarginMaxCm. Streaming areas call
    // this with their largest radius; false (and one warning per service) when it is broken.
    bool  checkStreamRadius(i32 radiusCm);
    void  reset(OriginCm o);                              // level load: set the origin when nothing exists yet; generation++ ; no participants run
    OriginCm origin() const { return origin_; }
    u32   generation() const { return generation_; }
    bool  enabled() const { return settings_.enabled; }
    void  addParticipant(Participant p);                  // ordered by `order`, then registration
    OriginCm decide(const Vec3& camLocalCm) const;        // the delta 2.2.2 would apply (zero = none); pure. Zero too when disabled or not FollowCamera
    bool  update(const Vec3& camLocalCm);                 // decide + shiftBy; false = nothing to do or deferred
    // prepare all, then commit all; false = refused (disabled, Fixed, zero or off-step delta, a prepare
    // deferred, or called from inside a commit). OriginCm carries no Z, so "Z never shifts" holds by type.
    bool  shiftBy(OriginCm delta, bool urgent = false);
    WorldPos absolute(const Vec3& localCm) const { return worldPosOfLocal(localCm, origin_); }
    Vec3     local(const WorldPos& p) const { return localOf(p, origin_); }

    struct Stats {
        u32 shifts = 0, deferrals = 0;
        f64 lastMs = 0;                                    // the whole last shiftBy that committed
        f64 lastParticipantMs[kMaxTimedParticipants] = {}; // commit time by position in the ordered list
        const char* lastDeferReason = nullptr;
    };
    const Stats& stats() const { return stats_; }
    usize participantCount() const { return parts_.size(); }

private:
    Settings settings_;
    OriginCm origin_;
    u32      generation_ = 0;
    std::vector<Participant> parts_;
    Stats    stats_;
    bool     inShift_ = false;
    bool     warnedRadius_ = false, warnedMargin_ = false;
    f64      lastDeferLogS_ = -1e9;
};

} // namespace aver::world
