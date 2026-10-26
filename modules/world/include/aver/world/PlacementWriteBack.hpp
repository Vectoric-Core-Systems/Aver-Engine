#pragma once
// The untouched-record rule (docs/plans/wave11/LARGE_WORLD.md 2.6.2): narrowing an f64 record to f32
// and widening it back is not an identity, so a save or a stream-out write-back must not re-derive a
// placement the author did not move. The host keeps a PlacementBaseline per placement: the authored
// f64 and the f32 local the entity was created with (rebased by every origin shift with the very
// same f32 subtraction the scene does, so an untouched entity still compares equal bit for bit).
//
// Per axis: an axis whose local still equals its baseline is written from the original f64; a moved
// axis is f64(O) + f64(local), quantised to 1e-4 cm (1 micrometre, below the f32 grid wherever it
// matters) so the shortest-round-trip writer emits a short number.
//
// Frozen: the other areas code against this header. Header-only, Core-only.
#include "aver/world/WorldPos.hpp"

#include <cmath>

namespace aver::world {

inline constexpr f64 kWriteBackQuantumCm = 1e-4;

inline f64 quantiseCm(f64 v) { return std::round(v * 1e4) / 1e4; }   // divide by an integer power of ten: the nearest double to the 4-decimal text

struct PlacementBaseline {
    Vec3d recordCm;      // the loaded f64 absolute, as authored
    Vec3  localAtLoad;   // the f32 local the entity was created at; a host that adjusts Z (ground snap) overwrites .z
};

// The baseline of a record loaded against origin `o`: the loader's own narrowing (f64 subtract, then f32).
inline PlacementBaseline makeBaseline(const Vec3d& recordCm, OriginCm o) {
    return PlacementBaseline{recordCm,
        Vec3{static_cast<f32>(recordCm.x - static_cast<f64>(o.x)),
             static_cast<f32>(recordCm.y - static_cast<f64>(o.y)),
             static_cast<f32>(recordCm.z)}};
}

// After an origin shift: the same f32 subtraction World::shiftRoots applies to the entity (ShiftEvent::deltaF).
inline void rebaseBaseline(PlacementBaseline& b, const Vec3& deltaF) {
    b.localAtLoad.x -= deltaF.x;
    b.localAtLoad.y -= deltaF.y;
    b.localAtLoad.z -= deltaF.z;
}

inline bool untouched(const PlacementBaseline& b, const Vec3& currentLocal) {
    return currentLocal.x == b.localAtLoad.x && currentLocal.y == b.localAtLoad.y
        && currentLocal.z == b.localAtLoad.z;
}

// The absolute f64 centimetres to write for an entity whose current scene-frame local is `currentLocal`
// under origin `originNow`.
inline Vec3d writeBackCm(const PlacementBaseline& b, const Vec3& currentLocal, OriginCm originNow) {
    Vec3d out = b.recordCm;
    if (currentLocal.x != b.localAtLoad.x) out.x = quantiseCm(static_cast<f64>(originNow.x) + static_cast<f64>(currentLocal.x));
    if (currentLocal.y != b.localAtLoad.y) out.y = quantiseCm(static_cast<f64>(originNow.y) + static_cast<f64>(currentLocal.y));
    if (currentLocal.z != b.localAtLoad.z) out.z = quantiseCm(static_cast<f64>(currentLocal.z));
    return out;
}

} // namespace aver::world
