// GiDrawsSettle: when the GI volume rebuilds for a change to the static draw list alone.
//
// Every rebuild re-voxelises the whole volume (then kGiConvergeTicks more for the bounce feedback), so rebuilding on every
// tick the list moves costs a full bake per frame while a level streams in (Caldera: 636 rebuilds in a row, 17-25 ms a
// frame) or while an object is dragged. A change to the list alone waits until the list has held still for `quietTicks`
// ticks, or `maxWaitTicks` at the latest, then rebuilds once. The volume it ends on is the same: only the interim ticks show
// the previous bake. Sun, sky, volume and forced rebuilds never go through here.
// Depends on aver/core/Types.hpp only (tests/render.voxi GiDrawsSettleTest).
#pragma once

#include "aver/core/Types.hpp"

namespace aver::voxi {

class GiDrawsSettle {
public:
    GiDrawsSettle(u32 quietTicks, u32 maxWaitTicks) : quiet_(quietTicks), maxWait_(maxWaitTicks) {}

    // The gate rejected on the draw list this tick, which now hashes to `key`. True: hold the rebuild this tick.
    bool hold(u64 key) {
        if (!pending_) {
            pending_ = true;
            key_ = key;
            still_ = 0;
            waited_ = 0;
        } else {
            ++waited_;
            if (key != key_) { key_ = key; still_ = 0; }
            else ++still_;
        }
        if (still_ >= quiet_ || waited_ >= maxWait_) { pending_ = false; return false; }
        return true;
    }
    // New thresholds; a change drops whatever was pending. quietTicks 0 turns the rule off.
    void configure(u32 quietTicks, u32 maxWaitTicks) {
        if (quietTicks == quiet_ && maxWaitTicks == maxWait_) return;
        quiet_ = quietTicks;
        maxWait_ = maxWaitTicks;
        pending_ = false;
    }
    u32 quietTicks() const { return quiet_; }
    // Anything else decided the tick (unchanged, or a rebuild for another reason): nothing is pending any more.
    void reset() { pending_ = false; }
    bool pending() const { return pending_; }

private:
    u32  quiet_, maxWait_;
    u64  key_ = 0;
    u32  still_ = 0, waited_ = 0;
    bool pending_ = false;
};

} // namespace aver::voxi
