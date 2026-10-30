// Render layers, cull masks and visibility ranges (G4 R4, docs/plans/fabric/G4_RENDER.md). Pure logic, no engine state, so the walk, the
// converter's runtime and the tests all use one definition.
//
// ZERO-SAFE ENCODING. World::addComponent hands back zero-filled storage, so every mask is stored INVERTED (the layers it excludes):
// 0 means "every layer", which is what an entity, a camera or a light that never heard of masks must mean. A real mask of 0 (no
// layer at all) is representable: it is 0xFFFFF inverted.
#pragma once
#include "aver/core/Types.hpp"

#include <cmath>

namespace aver::scene {

// Render layers and camera cull masks are 20 bits.
inline constexpr u32 kAllLayers = 0xFFFFFu;

inline constexpr u32 storeMask(u32 mask) { return ~mask & kAllLayers; }
inline constexpr u32 loadMask(u32 stored) { return ~stored & kAllLayers; }

// Does the instance (layers, stored) share a layer with the mask (camera cull mask / light cull mask / shadow caster mask, stored)?
inline constexpr bool masksOverlap(u32 storedLayers, u32 storedMask) {
    return (loadMask(storedLayers) & loadMask(storedMask)) != 0;
}

// ---- visibility ranges ------------------------------------------------------------------------------------------------------------

inline constexpr u32 kRangeHidden = 0x1;   // the last verdict was "hidden"
inline constexpr u32 kRangeKnown = 0x2;    // there has been a verdict (the first one uses the plain thresholds)

// A geometry instance's visibility range with the fade mode disabled: visible from `begin` to `end` (0 turns a bound off). A margin is
// a hysteresis: a visible instance stays so until it has crossed a threshold by the margin, a hidden one reappears only once it has
// crossed back by the margin. `state` is the caller's remembered verdict (kRange* bits). Fade modes self / dependencies are a hard cut.
inline bool rangeShown(f32 distCm, f32 beginCm, f32 endCm, f32 beginMarginCm, f32 endMarginCm, u32& state) {
    if (!(beginCm > 0.0f) && !(endCm > 0.0f)) { state = 0; return true; }
    const f32 bm = beginMarginCm > 0.0f ? beginMarginCm : 0.0f;
    const f32 em = endMarginCm > 0.0f ? endMarginCm : 0.0f;
    bool shown;
    if (!(state & kRangeKnown)) {
        shown = (!(beginCm > 0.0f) || distCm >= beginCm) && (!(endCm > 0.0f) || distCm < endCm);
    } else {
        const bool wasShown = !(state & kRangeHidden);
        const bool beginOk = !(beginCm > 0.0f) || (wasShown ? distCm >= beginCm - bm : distCm >= beginCm + bm);
        const bool endOk = !(endCm > 0.0f) || (wasShown ? distCm < endCm + em : distCm < endCm - em);
        shown = beginOk && endOk;
    }
    state = kRangeKnown | (shown ? 0u : kRangeHidden);
    return shown;
}

} // namespace aver::scene
