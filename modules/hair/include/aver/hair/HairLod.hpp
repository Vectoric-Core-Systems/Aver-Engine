#pragma once
// The distance rules of strand hair, as pure functions (docs/plans/wave11/SOFTBODY_HAIR.md 2.15 item 4 and 2.16): how often a groom is simulated and
// which of its strand-count LOD levels is drawn. Header-only and free of any engine type so tests/hair decides them with no world and no device, and
// HairSystem (simulation cadence) and HairScene (draw level) both call the one copy.
//
// UNVERIFIED STARTING VALUES: every distance and time below is a design number to be tuned in the headed pass.
#include "aver/core/Types.hpp"

namespace aver::hair {

inline constexpr u32 kHairLodLevels = 4;

// ---- simulation cadence ----

struct HairCadenceOptions {
    f32 nearCm = 800.0f;              // closer than this: simulated every frame
    f32 farCm = 2500.0f;              // beyond this: not simulated at all
    f32 invisibleSeconds = 2.0f;      // not visible for this long: not simulated
    f32 resumeTeleportSeconds = 1.0f; // a groom that was not simulated for longer than this restarts from its default pose when it resumes
};

enum class HairCadence : u8 { Every = 0, Half = 1, Frozen = 2 };

struct HairCadenceState {
    bool phase = false;               // flips every frame while in the Half band
    f32 accumulated = 0.0f;           // frame time owed to the next Half update
    f32 invisibleFor = 0.0f;
    f32 frozenFor = 0.0f;
};

struct HairCadenceResult {
    bool update = false;              // call aver_phys_hair_update this frame
    f32 dt = 0.0f;                    // the time to hand it (the owed time in the Half band)
    bool teleport = false;            // call aver_phys_hair_teleport first: it is resuming after a long freeze
    HairCadence level = HairCadence::Every;
};

// One frame of the rule. distanceCm is the distance from the view to the groom's head; frameDt is this frame's time.
inline HairCadenceResult stepHairCadence(const HairCadenceOptions& o, HairCadenceState& st, f32 distanceCm, bool visible, f32 frameDt) {
    HairCadenceResult r;
    if (visible) st.invisibleFor = 0.0f;
    else st.invisibleFor += frameDt;
    const bool frozen = distanceCm >= o.farCm || st.invisibleFor >= o.invisibleSeconds;
    if (frozen) {
        st.frozenFor += frameDt;
        st.accumulated = 0.0f;
        st.phase = false;
        r.level = HairCadence::Frozen;
        return r;
    }
    r.teleport = st.frozenFor > o.resumeTeleportSeconds;
    st.frozenFor = 0.0f;
    if (distanceCm < o.nearCm) {
        st.accumulated = 0.0f;
        st.phase = false;
        r.update = true;
        r.dt = frameDt;
        r.level = HairCadence::Every;
        return r;
    }
    // The middle band: every second frame, with the time of both. Jolt clamps one update to its maximum delta time, so a slow frame rate simply slows
    // the far hair down, which is acceptable at that distance.
    st.accumulated += frameDt;
    st.phase = !st.phase;
    r.level = HairCadence::Half;
    if (st.phase) return r;
    r.update = true;
    r.dt = st.accumulated;
    st.accumulated = 0.0f;
    return r;
}

// ---- the drawn level ----

// The strand-count LOD level (0 = every strand, 3 = the thinnest) for a distance. distances are the three thresholds, ascending. `current` is the
// level drawn last frame: moving away needs the distance to pass a threshold by `hysteresis` (a fraction), moving closer to fall short of it by the
// same, so a head hovering on a boundary does not flicker. lodBias (in steps, may be negative) is added last. Clamped to 0..3.
inline u32 chooseHairLod(f32 distanceCm, const f32 distances[3], f32 lodBias, u32 current, f32 hysteresis = 0.15f) {
    u32 level = current > kHairLodLevels - 1 ? kHairLodLevels - 1 : current;
    while (level < kHairLodLevels - 1 && distanceCm >= distances[level] * (1.0f + hysteresis)) ++level;
    while (level > 0 && distanceCm < distances[level - 1] * (1.0f - hysteresis)) --level;
    // The level the hysteresis settled on is the STATE; the bias only moves what is drawn.
    return level;
}

// The drawn level for a state level: lodBias rounded to whole steps and clamped.
inline u32 biasedHairLod(u32 stateLevel, f32 lodBias) {
    const f32 shifted = static_cast<f32>(stateLevel) + lodBias;
    const i32 r = static_cast<i32>(shifted < 0.0f ? shifted - 0.5f : shifted + 0.5f);
    if (r < 0) return 0;
    if (r > static_cast<i32>(kHairLodLevels) - 1) return kHairLodLevels - 1;
    return static_cast<u32>(r);
}

// The strand fraction drawn at a level, scaled by the groom's densityScale and clamped to (0, 1]. 0 or negative densityScale reads as 1
// (a zero-filled component means "the default").
inline f32 hairDrawFraction(const f32 fractions[4], u32 level, f32 densityScale) {
    const f32 base = fractions[level > kHairLodLevels - 1 ? kHairLodLevels - 1 : level];
    const f32 d = densityScale > 0.0f ? densityScale : 1.0f;
    f32 f = base * d;
    if (f > 1.0f) f = 1.0f;
    if (f < 0.01f) f = 0.01f;
    return f;
}

} // namespace aver::hair
