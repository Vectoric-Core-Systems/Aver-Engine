#pragma once
// AI debug draw feed: a plain list of world-space lines and text labels that AI systems push into and
// the editor viewport draws. Header-only and engine-free so any AI system (sight, hearing, steering,
// cover, crowds) can feed it, and tests can check the geometry. Z is up; yaw rotates around Z.
//
// Systems either push during their own tick when `wants(category)` is true, or register a provider
// that the overlay runs once per frame after clear(). Overlay: sandbox/src/AiDebugOverlay.*.
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace aver::synapse {

enum AiDebugCategory : u32 {
    kAiDebugSight    = 1u << 0,
    kAiDebugHearing  = 1u << 1,
    kAiDebugPath     = 1u << 2,
    kAiDebugSteering = 1u << 3,
    kAiDebugBtState  = 1u << 4,
    kAiDebugCover    = 1u << 5,
    kAiDebugAll      = 0x3Fu,
};

// 0xRRGGBBAA.
constexpr u32 aiRgba(u32 r, u32 g, u32 b, u32 a = 255) { return (r << 24) | (g << 16) | (b << 8) | a; }

struct AiDebugLine { Vec3 a, b; u32 rgba = 0xFFFFFFFFu; u32 category = 0; };
struct AiDebugText { Vec3 pos; std::string text; u32 rgba = 0xFFFFFFFFu; u32 category = 0; };

class AiDebugSink {
public:
    using ProviderFn = void (*)(AiDebugSink& sink, void* user);

    void setEnabled(u32 mask) { enabled_ = mask; }
    u32  enabled() const { return enabled_; }
    bool wants(u32 category) const { return (enabled_ & category) != 0; }

    void clear() { lines_.clear(); texts_.clear(); }
    const std::vector<AiDebugLine>& lines() const { return lines_; }
    const std::vector<AiDebugText>& texts() const { return texts_; }

    void addProvider(ProviderFn fn, void* user = nullptr) {
        if (fn) providers_.push_back({fn, user});
    }
    void removeProvider(ProviderFn fn) {
        providers_.erase(std::remove_if(providers_.begin(), providers_.end(),
                                        [fn](const Provider& p) { return p.fn == fn; }),
                         providers_.end());
    }
    void runProviders() {
        for (const Provider& p : providers_) p.fn(*this, p.user);
    }

    // Everything below is dropped when the category is disabled.
    void line(const Vec3& a, const Vec3& b, u32 rgba, u32 category) {
        if (wants(category)) lines_.push_back({a, b, rgba, category});
    }
    void text(const Vec3& pos, std::string s, u32 rgba, u32 category) {
        if (wants(category)) texts_.push_back({pos, std::move(s), rgba, category});
    }
    // Line with a two-barb head at `b`.
    void arrow(const Vec3& a, const Vec3& b, u32 rgba, u32 category, f32 headLen = 20.0f) {
        if (!wants(category)) return;
        lines_.push_back({a, b, rgba, category});
        const Vec3 d = b - a;
        const f32 len = d.size();
        if (len < 1e-3f) return;
        const Vec3 dir = d / len;
        const f32 h = std::min(headLen, len * 0.5f);
        Vec3 side = cross(dir, Vec3{0.0f, 0.0f, 1.0f});
        if (side.sizeSquared() < 1e-6f) side = cross(dir, Vec3{1.0f, 0.0f, 0.0f});
        side = side.getSafeNormal();
        const Vec3 base = b - dir * h;
        lines_.push_back({b, base + side * (h * 0.5f), rgba, category});
        lines_.push_back({b, base - side * (h * 0.5f), rgba, category});
    }
    // Horizontal circle (XY plane) around `center`.
    void circle(const Vec3& center, f32 radius, u32 rgba, u32 category, int segments = 32) {
        if (!wants(category) || radius <= 0.0f || segments < 3) return;
        ring(center, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, radius, rgba, category, segments);
    }
    // Three orthogonal circles.
    void sphere(const Vec3& center, f32 radius, u32 rgba, u32 category, int segments = 24) {
        if (!wants(category) || radius <= 0.0f || segments < 3) return;
        ring(center, Vec3{1, 0, 0}, Vec3{0, 1, 0}, radius, rgba, category, segments);
        ring(center, Vec3{1, 0, 0}, Vec3{0, 0, 1}, radius, rgba, category, segments);
        ring(center, Vec3{0, 1, 0}, Vec3{0, 0, 1}, radius, rgba, category, segments);
    }
    // A view cone: four edge lines from the apex and a rim circle at the far end.
    void cone(const Vec3& apex, const Vec3& forward, f32 range, f32 halfAngleRad, u32 rgba,
              u32 category, int segments = 24) {
        if (!wants(category) || range <= 0.0f || halfAngleRad <= 0.0f) return;
        const Vec3 f = forward.getSafeNormal();
        if (f.sizeSquared() < 0.5f) return;
        Vec3 right = cross(f, Vec3{0.0f, 0.0f, 1.0f});
        if (right.sizeSquared() < 1e-6f) right = cross(f, Vec3{1.0f, 0.0f, 0.0f});
        right = right.getSafeNormal();
        const Vec3 up = cross(right, f);
        const f32 axial = range * std::cos(halfAngleRad);
        const f32 radial = range * std::sin(halfAngleRad);
        const Vec3 rimCenter = apex + f * axial;
        ring(rimCenter, right, up, radial, rgba, category, segments);
        lines_.push_back({apex, rimCenter + right * radial, rgba, category});
        lines_.push_back({apex, rimCenter - right * radial, rgba, category});
        lines_.push_back({apex, rimCenter + up * radial, rgba, category});
        lines_.push_back({apex, rimCenter - up * radial, rgba, category});
    }
    // Connected polyline (e.g. a path).
    void polyline(const std::vector<Vec3>& points, u32 rgba, u32 category) {
        if (!wants(category)) return;
        for (usize i = 1; i < points.size(); ++i) lines_.push_back({points[i - 1], points[i], rgba, category});
    }

private:
    struct Provider { ProviderFn fn; void* user; };

    void ring(const Vec3& c, const Vec3& u, const Vec3& v, f32 radius, u32 rgba, u32 category, int segments) {
        constexpr f32 kTwoPi = 6.28318530718f;
        Vec3 prev = c + u * radius;
        for (int i = 1; i <= segments; ++i) {
            const f32 t = kTwoPi * static_cast<f32>(i) / static_cast<f32>(segments);
            const Vec3 p = c + u * (radius * std::cos(t)) + v * (radius * std::sin(t));
            lines_.push_back({prev, p, rgba, category});
            prev = p;
        }
    }

    u32 enabled_ = 0;
    std::vector<AiDebugLine> lines_;
    std::vector<AiDebugText> texts_;
    std::vector<Provider> providers_;
};

inline AiDebugSink& aiDebug() {
    static AiDebugSink sink;
    return sink;
}

} // namespace aver::synapse
