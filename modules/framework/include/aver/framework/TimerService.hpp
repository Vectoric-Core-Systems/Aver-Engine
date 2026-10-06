#pragma once
// Frame-safe timer service: one-shot and repeating timers on a pausable, dilatable game clock.
// Header-only and dependency-free so it unit-tests without the DLL. Single game thread only.
// Semantics and rationale: docs/GAME_TIMERS_EVENTS.md.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <utility>
#include <vector>

namespace aver::fw {

// 0 is invalid. (generation << 16) | (slot + 1): a stale handle never aliases a reused slot.
using TimerHandle = int32_t;

enum TimerFlags : uint32_t {
    kTimerRepeat         = 1,   // re-arm after each fire
    kTimerIgnorePause    = 2,   // keeps running while the service is paused
    kTimerIgnoreDilation = 4,   // runs on real time, not dilated game time
};

struct TimerOptions {
    bool     repeat       = false;
    float    firstDelay   = -1.0f;  // < 0: same as the interval
    int32_t  maxFires     = 0;      // repeat only; 0 = unlimited
    uint32_t flags        = 0;      // kTimerIgnorePause | kTimerIgnoreDilation
    int32_t  owner        = 0;      // cancelOwner() key, typically an entity; 0 = none
};

class TimerService {
public:
    using Callback = std::function<void(TimerHandle)>;

    static constexpr int kMaxTimers   = 0xFFFF;
    static constexpr int kMaxCatchUp  = 8;     // fires of one timer per update before surplus is dropped
    static constexpr double kEpsilon  = 1e-6;  // tolerance so 3 x 0.1f reaches 0.3f

    // Delay 0 fires on the next update. Returns 0 on a NaN delay or when the table is full.
    TimerHandle set(float delay, Callback cb, const TimerOptions& o = {}) {
        if (!cb || std::isnan(delay)) return 0;
        if (delay < 0.0f) delay = 0.0f;
        if (std::isnan(o.firstDelay)) return 0;
        uint32_t idx;
        if (!free_.empty()) { idx = free_.back(); free_.pop_back(); }
        else if (slots_.size() < static_cast<size_t>(kMaxTimers)) { idx = static_cast<uint32_t>(slots_.size()); slots_.emplace_back(); }
        else return 0;
        Slot& s         = slots_[idx];
        s.live          = true;
        s.paused        = false;
        s.repeat        = o.repeat;
        s.ignorePause   = (o.flags & kTimerIgnorePause) != 0;
        s.ignoreDilation = (o.flags & kTimerIgnoreDilation) != 0;
        s.interval      = delay;
        s.remaining     = (o.repeat && o.firstDelay >= 0.0f) ? o.firstDelay : delay;
        s.avail         = 0.0;   // a timer born inside update() starts counting next update
        s.dtFrame       = 0.0;
        s.maxFires      = o.repeat ? std::max(0, o.maxFires) : 0;
        s.fired         = 0;
        s.owner         = o.owner;
        s.seq           = ++seq_;
        s.cb            = std::make_shared<Callback>(std::move(cb));
        ++count_;
        return encode(idx, s.gen);
    }

    // Safe from inside any callback, including the timer's own.
    bool cancel(TimerHandle h) {
        Slot* s = find(h);
        if (!s) return false;
        release(decode(h));
        return true;
    }

    int cancelOwner(int32_t owner) {
        if (owner == 0) return 0;
        int n = 0;
        for (uint32_t i = 0; i < slots_.size(); ++i)
            if (slots_[i].live && slots_[i].owner == owner) { release(i); ++n; }
        return n;
    }

    void clear() {
        for (uint32_t i = 0; i < slots_.size(); ++i)
            if (slots_[i].live) release(i);
        ++epoch_;
    }

    bool  isActive(TimerHandle h) const { return find(h) != nullptr; }
    float remaining(TimerHandle h) const { const Slot* s = find(h); return s ? static_cast<float>(std::max(0.0, s->remaining - s->avail)) : -1.0f; }
    int   fireCount(TimerHandle h) const { const Slot* s = find(h); return s ? s->fired : -1; }

    bool setPaused(TimerHandle h, bool paused) { Slot* s = find(h); if (!s) return false; s->paused = paused; return true; }
    bool isPaused(TimerHandle h) const { const Slot* s = find(h); return s && s->paused; }

    // Restarts the countdown; delay < 0 keeps the current interval.
    bool reset(TimerHandle h, float delay = -1.0f) {
        Slot* s = find(h);
        if (!s || std::isnan(delay)) return false;
        if (delay >= 0.0f) s->interval = delay;
        s->remaining = s->interval;
        s->avail     = 0.0;
        return true;
    }

    void   setPaused(bool p) { paused_ = p; }
    bool   paused() const { return paused_; }
    void   setDilation(float d) { if (!std::isnan(d)) dilation_ = std::clamp(d, 0.0f, 100.0f); }
    float  dilation() const { return dilation_; }
    void   setMaxStep(float s) { if (s > 0.0f) maxStep_ = s; }
    double time() const { return gameTime_; }       // dilated, pause-respecting
    double realTime() const { return realTime_; }   // raw
    int    count() const { return count_; }
    uint32_t epoch() const { return epoch_; }       // bumps on clear(); managed side drops stale records
    int    errors() const { return errors_; }

    // One frame. Timers fire in the order their due moment falls within the frame (ties: creation
    // order), repeating timers catch up inside the frame, and a timer added or cancelled by a
    // callback is honoured immediately. A pause or dilation change made by a callback applies from
    // the next update. rawDt is clamped to setMaxStep() so a debugger break does not fire a storm.
    void update(float rawDtIn) {
        if (!(rawDtIn > 0.0f) || std::isinf(rawDtIn)) return;
        const double raw    = std::min<double>(rawDtIn, maxStep_);
        const double scaled = raw * dilation_;
        realTime_ += raw;
        if (!paused_) gameTime_ += scaled;

        struct Cand { double f; uint64_t seq; uint32_t idx; uint32_t gen; };
        struct Later { bool operator()(const Cand& a, const Cand& b) const { return a.f != b.f ? a.f > b.f : a.seq > b.seq; } };
        std::priority_queue<Cand, std::vector<Cand>, Later> due;

        const uint32_t n = static_cast<uint32_t>(slots_.size());
        for (uint32_t i = 0; i < n; ++i) {
            Slot& s = slots_[i];
            if (!s.live) continue;
            s.firesThisUpdate = 0;
            double d = s.ignoreDilation ? raw : scaled;
            if (s.paused || (paused_ && !s.ignorePause)) d = 0.0;
            s.dtFrame = d;
            s.avail   = d;
            if (d <= 0.0) continue;
            if (s.remaining <= d + kEpsilon) due.push({s.remaining / d, s.seq, i, s.gen});
            else { s.remaining -= d; s.avail = 0.0; }
        }

        while (!due.empty()) {
            const Cand c = due.top();
            due.pop();
            Slot& s = slots_[c.idx];
            if (!s.live || s.gen != c.gen) continue;
            if (s.paused) { s.avail = 0.0; continue; }
            if (s.remaining > s.avail + kEpsilon) { s.remaining -= s.avail; s.avail = 0.0; continue; }  // reset() by a callback

            s.avail = std::max(0.0, s.avail - s.remaining);
            ++s.fired;
            ++s.firesThisUpdate;
            const TimerHandle h = encode(c.idx, s.gen);
            const bool last = !s.repeat || (s.maxFires > 0 && s.fired >= s.maxFires);
            std::shared_ptr<Callback> cb = s.cb;
            if (last) {
                release(c.idx);
            } else {
                s.remaining = s.interval;
                if (s.interval <= 0.0f || s.firesThisUpdate >= kMaxCatchUp) {
                    s.avail = 0.0;   // zero interval = once per update; cap = drop the surplus
                } else if (s.remaining <= s.avail + kEpsilon) {
                    due.push({(s.dtFrame - s.avail + s.remaining) / s.dtFrame, s.seq, c.idx, s.gen});
                } else {
                    s.remaining -= s.avail;
                    s.avail = 0.0;
                }
            }
            try { (*cb)(h); } catch (...) { ++errors_; }
        }
    }

private:
    struct Slot {
        uint32_t gen = 1;
        bool     live = false, paused = false, repeat = false, ignorePause = false, ignoreDilation = false;
        float    interval = 0.0f;
        double   remaining = 0.0, avail = 0.0, dtFrame = 0.0;
        int32_t  maxFires = 0, fired = 0, firesThisUpdate = 0, owner = 0;
        uint64_t seq = 0;
        std::shared_ptr<Callback> cb;
    };

    static TimerHandle encode(uint32_t idx, uint32_t gen) { return static_cast<TimerHandle>(((gen & 0x7FFFu) << 16) | (idx + 1u)); }
    static uint32_t decode(TimerHandle h) { return (static_cast<uint32_t>(h) & 0xFFFFu) - 1u; }

    Slot* find(TimerHandle h) { return const_cast<Slot*>(std::as_const(*this).find(h)); }
    const Slot* find(TimerHandle h) const {
        if (h <= 0) return nullptr;
        const uint32_t idx = decode(h);
        if (idx >= slots_.size()) return nullptr;
        const Slot& s = slots_[idx];
        if (!s.live || (s.gen & 0x7FFFu) != ((static_cast<uint32_t>(h) >> 16) & 0x7FFFu)) return nullptr;
        return &s;
    }

    void release(uint32_t idx) {
        Slot& s = slots_[idx];
        s.live = false;
        s.cb.reset();
        s.gen = (s.gen % 0x7FFFu) + 1u;
        free_.push_back(idx);
        --count_;
    }

    std::vector<Slot>     slots_;
    std::vector<uint32_t> free_;
    uint64_t seq_       = 0;
    int      count_     = 0;
    int      errors_    = 0;
    uint32_t epoch_     = 0;
    bool     paused_    = false;
    float    dilation_  = 1.0f;
    float    maxStep_   = 1.0f;
    double   gameTime_  = 0.0;
    double   realTime_  = 0.0;
};

} // namespace aver::fw
