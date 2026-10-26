#include "aver/world/WorldOrigin.hpp"

#include "aver/core/Log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace aver::world {

namespace {
using Clock = std::chrono::steady_clock;
f64 secondsNow() { return std::chrono::duration<f64>(Clock::now().time_since_epoch()).count(); }
bool onStep(i64 v) { return v % kOriginStepCm == 0; }
i64 snapToStep(i64 v) {
    const f64 k = std::round(static_cast<f64>(v) / static_cast<f64>(kOriginStepCm));
    return static_cast<i64>(k) * kOriginStepCm;
}
} // namespace

void WorldOrigin::configure(const Settings& s) {
    settings_ = s;
    const i32 clamped = std::clamp(s.marginCm, 0, kRebaseMarginMaxCm);
    if (clamped != s.marginCm && !warnedMargin_) {
        warnedMargin_ = true;
        AVER_WARN("WorldOrigin: rebase margin {} cm outside [0, {}]; using {}", s.marginCm, kRebaseMarginMaxCm, clamped);
    }
    settings_.marginCm = clamped;
}

bool WorldOrigin::checkStreamRadius(i32 radiusCm) {
    if (static_cast<i64>(settings_.marginCm) + static_cast<i64>(radiusCm) <= kRebaseMarginMaxCm) return true;
    if (!warnedRadius_) {
        warnedRadius_ = true;
        AVER_WARN("WorldOrigin: streaming radius {} cm + rebase margin {} cm exceeds {} cm; far resident objects lose sub-millimetre precision",
                  radiusCm, settings_.marginCm, kRebaseMarginMaxCm);
    }
    return false;
}

void WorldOrigin::reset(OriginCm o) {
    if (!onStep(o.x) || !onStep(o.y)) {
        AVER_WARN("WorldOrigin::reset: ({}, {}) is not on the {} cm step; snapped", o.x, o.y, kOriginStepCm);
        o.x = snapToStep(o.x);
        o.y = snapToStep(o.y);
    }
    origin_ = o;
    ++generation_;
}

void WorldOrigin::addParticipant(Participant p) {
    const auto it = std::upper_bound(parts_.begin(), parts_.end(), p.order,
                                     [](i32 order, const Participant& q) { return order < q.order; });
    parts_.insert(it, std::move(p));
}

OriginCm WorldOrigin::decide(const Vec3& cam) const {
    OriginCm d;
    if (!settings_.enabled || settings_.policy != OriginPolicy::FollowCamera) return d;
    const f64 limit = static_cast<f64>(kRegionHalfCm) + static_cast<f64>(settings_.marginCm);
    const f64 step = static_cast<f64>(kOriginStepCm);
    const f64 c[2] = {static_cast<f64>(cam.x), static_cast<f64>(cam.y)};
    i64* out[2] = {&d.x, &d.y};
    for (int a = 0; a < 2; ++a) {
        if (!std::isfinite(c[a]) || std::fabs(c[a]) <= limit) continue;
        *out[a] = static_cast<i64>(std::llround(c[a] / step)) * kOriginStepCm;
    }
    return d;
}

bool WorldOrigin::update(const Vec3& cam) {
    const OriginCm d = decide(cam);
    if (d.x == 0 && d.y == 0) return false;
    const f64 ax = d.x != 0 ? std::fabs(static_cast<f64>(cam.x)) : 0.0;
    const f64 ay = d.y != 0 ? std::fabs(static_cast<f64>(cam.y)) : 0.0;
    return shiftBy(d, std::max(ax, ay) > static_cast<f64>(kOriginUrgentCm));
}

bool WorldOrigin::shiftBy(OriginCm delta, bool urgent) {
    if (inShift_ || !settings_.enabled || settings_.policy == OriginPolicy::Fixed) return false;
    if (delta.x == 0 && delta.y == 0) return false;
    if (!onStep(delta.x) || !onStep(delta.y)) {
        AVER_WARN("WorldOrigin::shiftBy: delta ({}, {}) is not a multiple of {} cm; refused", delta.x, delta.y, kOriginStepCm);
        return false;
    }
    const auto t0 = Clock::now();

    ShiftEvent ev;
    ev.oldOrigin  = origin_;
    ev.newOrigin  = OriginCm{origin_.x + delta.x, origin_.y + delta.y};
    ev.deltaCm    = Vec3d{static_cast<f64>(delta.x), static_cast<f64>(delta.y), 0.0};
    ev.deltaF     = Vec3{static_cast<f32>(delta.x), static_cast<f32>(delta.y), 0.0f};
    ev.generation = generation_ + 1;
    ev.urgent     = urgent;

    inShift_ = true;
    for (const Participant& p : parts_) {
        if (!p.prepare) continue;
        const PrepareResult r = p.prepare(ev);
        if (r.ok) continue;
        ++stats_.deferrals;
        stats_.lastDeferReason = r.deferReason ? r.deferReason : p.name;
        const f64 now = secondsNow();
        if (now - lastDeferLogS_ >= 1.0) {
            lastDeferLogS_ = now;
            AVER_WARN("WorldOrigin: shift deferred by '{}': {}", p.name, stats_.lastDeferReason);
        }
        inShift_ = false;
        return false;
    }

    origin_ = ev.newOrigin;
    generation_ = ev.generation;
    std::fill(std::begin(stats_.lastParticipantMs), std::end(stats_.lastParticipantMs), 0.0);
    for (usize i = 0; i < parts_.size(); ++i) {
        if (!parts_[i].commit) continue;
        const auto c0 = Clock::now();
        parts_[i].commit(ev);
        if (i < kMaxTimedParticipants)
            stats_.lastParticipantMs[i] = std::chrono::duration<f64, std::milli>(Clock::now() - c0).count();
    }
    ++stats_.shifts;
    stats_.lastMs = std::chrono::duration<f64, std::milli>(Clock::now() - t0).count();
    inShift_ = false;
    return true;
}

} // namespace aver::world
