#pragma once
// 2D ground-plane vector for steering, crowds and cover. Centimetres, +X forward, +Y right.
// Named dot2/det2/... so they never compete with aver::dot(Vec3) in a TU that sees both.
#include "aver/core/Types.hpp"

#include <cmath>

namespace aver::synapse {

struct V2 {
    f32 x = 0.0f, y = 0.0f;
};

inline V2  operator+(V2 a, V2 b) { return {a.x + b.x, a.y + b.y}; }
inline V2  operator-(V2 a, V2 b) { return {a.x - b.x, a.y - b.y}; }
inline V2  operator*(V2 a, f32 s) { return {a.x * s, a.y * s}; }
inline V2  operator*(f32 s, V2 a) { return {a.x * s, a.y * s}; }
inline V2  operator-(V2 a) { return {-a.x, -a.y}; }
inline V2& operator+=(V2& a, V2 b) { a.x += b.x; a.y += b.y; return a; }
inline V2& operator-=(V2& a, V2 b) { a.x -= b.x; a.y -= b.y; return a; }

inline f32 dot2(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
// z of the 3D cross product: positive when b is counter-clockwise of a.
inline f32 det2(V2 a, V2 b) { return a.x * b.y - a.y * b.x; }
inline f32 lenSq2(V2 a) { return a.x * a.x + a.y * a.y; }
inline f32 len2(V2 a) { return std::sqrt(lenSq2(a)); }
inline f32 distSq2(V2 a, V2 b) { return lenSq2(a - b); }
inline f32 dist2(V2 a, V2 b) { return len2(a - b); }

// Unit vector, or `fallback` when shorter than 1e-6.
inline V2 norm2(V2 a, V2 fallback = {0.0f, 0.0f}) {
    const f32 l = len2(a);
    return l > 1e-6f ? V2{a.x / l, a.y / l} : fallback;
}
// Scales down to `maxLen`; never scales up.
inline V2 clampLen2(V2 a, f32 maxLen) {
    const f32 l2 = lenSq2(a);
    if (l2 <= maxLen * maxLen) return a;
    const f32 l = std::sqrt(l2);
    return l > 1e-9f ? a * (maxLen / l) : V2{};
}
// Rotates 90 degrees counter-clockwise.
inline V2 perp2(V2 a) { return {-a.y, a.x}; }

// Small deterministic hash for tie-breaks and jitter: same input, same output, every platform.
inline u32 hashU32(u32 x) {
    x += 0x9E3779B9u;
    x = (x ^ (x >> 16)) * 0x21F0AAADu;
    x = (x ^ (x >> 15)) * 0x735A2D97u;
    return x ^ (x >> 15);
}
// [0,1) from a hash.
inline f32 hashToUnit(u32 h) { return static_cast<f32>(h >> 8) / 16777216.0f; }

} // namespace aver::synapse
