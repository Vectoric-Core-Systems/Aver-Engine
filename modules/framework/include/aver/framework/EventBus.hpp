#pragma once
// Typed event bus: named events with a small typed payload, subscribe/unsubscribe, immediate
// dispatch and deferred post/flush at a frame-safe point. Header-only, single game thread only.
// Semantics and rationale: docs/GAME_TIMERS_EVENTS.md.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aver::fw {

enum class EventValueType : int32_t { None = 0, Int, Float, Bool, String, Vec3, Entity };

struct EventValue {
    EventValueType type = EventValueType::None;
    int64_t        i    = 0;          // Int, Bool, Entity
    float          f[3] = {0, 0, 0};  // Float uses f[0]; Vec3 uses all three
    std::string    s;                 // String

    static EventValue ofInt(int64_t v)   { EventValue e; e.type = EventValueType::Int;    e.i = v; return e; }
    static EventValue ofFloat(float v)   { EventValue e; e.type = EventValueType::Float;  e.f[0] = v; return e; }
    static EventValue ofBool(bool v)     { EventValue e; e.type = EventValueType::Bool;   e.i = v ? 1 : 0; return e; }
    static EventValue ofEntity(int32_t v){ EventValue e; e.type = EventValueType::Entity; e.i = v; return e; }
    static EventValue ofString(std::string v) { EventValue e; e.type = EventValueType::String; e.s = std::move(v); return e; }
    static EventValue ofVec3(float x, float y, float z) { EventValue e; e.type = EventValueType::Vec3; e.f[0] = x; e.f[1] = y; e.f[2] = z; return e; }

    // Numeric reads coerce Int/Float/Bool/Entity into each other; anything else reads as 0.
    bool numeric() const { return type == EventValueType::Int || type == EventValueType::Float || type == EventValueType::Bool || type == EventValueType::Entity; }
    int64_t asInt() const { return type == EventValueType::Float ? static_cast<int64_t>(f[0]) : (numeric() ? i : 0); }
    float   asFloat() const { return type == EventValueType::Float ? f[0] : (numeric() ? static_cast<float>(i) : 0.0f); }
    bool    asBool() const { return type == EventValueType::Float ? f[0] != 0.0f : (numeric() && i != 0); }
};

struct EventPayload {
    static constexpr size_t kMaxArgs = 16;
    std::vector<EventValue> args;
    bool push(EventValue v) { if (args.size() >= kMaxArgs) return false; args.push_back(std::move(v)); return true; }
    void clear() { args.clear(); }
};

struct Event {
    std::string  name;
    int32_t      sender = 0;   // entity that raised it; 0 = none
    int32_t      target = 0;   // 0 = broadcast; else only subscribers whose owner == target
    EventPayload payload;
};

using SubscriptionId = int32_t;   // 0 invalid

struct SubscribeOptions {
    int32_t owner        = 0;   // unsubscribeOwner() key and target filter, typically an entity
    int32_t priority     = 0;   // higher first; ties in subscription order
    int32_t senderFilter = 0;   // 0 = any sender
};

class EventBus {
public:
    // Return true to consume the event and stop later handlers.
    using Handler = std::function<bool(const Event&)>;

    static constexpr int kDefaultMaxDepth = 8;
    static constexpr int kDefaultFlushPasses = 8;

    SubscriptionId subscribe(std::string_view name, Handler h, const SubscribeOptions& o = {}) {
        if (name.empty() || !h) return 0;
        Sub s;
        s.id = nextId_++;
        if (nextId_ <= 0) nextId_ = 1;
        const SubscriptionId id = s.id;
        s.opt = o;
        s.alive = true;
        s.seq = ++seq_;
        s.h = std::make_shared<Handler>(std::move(h));
        idToName_.emplace(id, std::string(name));
        ++liveCount_;
        if (depth_ > 0) pendingAdds_.emplace_back(std::string(name), std::move(s));
        else insertSorted(channels_[std::string(name)], std::move(s));
        return id;
    }

    // Safe inside a handler: the subscriber is skipped from then on, even in the event in flight.
    bool unsubscribe(SubscriptionId id) {
        auto it = idToName_.find(id);
        if (it == idToName_.end()) return false;
        const std::string name = it->second;
        idToName_.erase(it);
        --liveCount_;
        for (size_t i = 0; i < pendingAdds_.size(); ++i)
            if (pendingAdds_[i].second.id == id) { pendingAdds_.erase(pendingAdds_.begin() + static_cast<ptrdiff_t>(i)); return true; }
        auto ch = channels_.find(name);
        if (ch != channels_.end())
            for (Sub& s : ch->second)
                if (s.id == id) { s.alive = false; dirty_ = true; break; }
        sweepIfIdle();
        return true;
    }

    int unsubscribeOwner(int32_t owner) {
        if (owner == 0) return 0;
        std::vector<SubscriptionId> ids;
        for (auto& [name, ch] : channels_)
            for (Sub& s : ch) if (s.alive && s.opt.owner == owner) ids.push_back(s.id);
        for (auto& p : pendingAdds_) if (p.second.opt.owner == owner) ids.push_back(p.second.id);
        for (SubscriptionId id : ids) unsubscribe(id);
        return static_cast<int>(ids.size());
    }

    int listenerCount(std::string_view name) const {
        auto it = channels_.find(std::string(name));
        int n = 0;
        if (it != channels_.end()) for (const Sub& s : it->second) if (s.alive) ++n;
        for (const auto& p : pendingAdds_) if (p.first == name) ++n;
        return n;
    }
    int subscriberCount() const { return liveCount_; }

    // Immediate dispatch. Returns the number of handlers that ran. Past maxDepth() nested
    // dispatches the event is queued instead of dropped and overflowed() counts it.
    int dispatch(const Event& ev) {
        if (depth_ >= maxDepth_) { queue_.push_back(ev); ++overflowed_; return 0; }
        auto it = channels_.find(ev.name);
        if (it == channels_.end()) return 0;
        std::vector<Sub>& subs = it->second;   // not resized while depth_ > 0 (adds are deferred)
        ++depth_;
        const size_t n = subs.size();
        int ran = 0;
        const Event* outer = current_;
        current_ = &ev;
        for (size_t i = 0; i < n; ++i) {
            Sub& s = subs[i];
            if (!s.alive) continue;
            if (s.opt.senderFilter != 0 && s.opt.senderFilter != ev.sender) continue;
            if (ev.target != 0 && s.opt.owner != ev.target) continue;
            std::shared_ptr<Handler> h = s.h;
            ++ran;
            bool consumed = false;
            try { consumed = (*h)(ev); } catch (...) { ++errors_; }
            if (consumed) break;
        }
        current_ = outer;
        if (--depth_ == 0) settle();
        return ran;
    }

    // Deferred dispatch: queued until flush().
    void post(Event ev) { queue_.push_back(std::move(ev)); }

    // The frame-safe point. Drains the queue in FIFO order; events posted by handlers run in the
    // next pass, up to flushPasses() passes, and the rest wait for the next flush. A flush called
    // from inside a handler is refused (returns 0). Returns the number of events dispatched.
    int flush() {
        if (flushing_ || depth_ > 0) return 0;
        flushing_ = true;
        int total = 0;
        for (int pass = 0; pass < flushPasses_ && !queue_.empty(); ++pass) {
            std::vector<Event> batch;
            batch.swap(queue_);
            const uint32_t epoch = epoch_;
            for (Event& ev : batch) {
                if (epoch != epoch_) break;   // clearPending() ran inside a handler
                dispatch(ev);
                ++total;
            }
        }
        flushing_ = false;
        return total;
    }

    size_t pending() const { return queue_.size(); }
    void   clearPending() { queue_.clear(); ++epoch_; }

    void clear() {
        clearPending();
        if (depth_ > 0) {
            for (auto& [name, ch] : channels_) for (Sub& s : ch) s.alive = false;
            dirty_ = true;
        } else {
            channels_.clear();
        }
        pendingAdds_.clear();
        idToName_.clear();
        liveCount_ = 0;
    }

    // The event whose handler is running, or null. Valid only for that handler's duration.
    const Event* current() const { return current_; }

    void setMaxDepth(int d) { if (d >= 1) maxDepth_ = d; }
    int  maxDepth() const { return maxDepth_; }
    void setFlushPasses(int p) { if (p >= 1) flushPasses_ = p; }
    int  flushPasses() const { return flushPasses_; }
    int  overflowed() const { return overflowed_; }
    int  errors() const { return errors_; }
    int  depth() const { return depth_; }

private:
    struct Sub {
        SubscriptionId id = 0;
        SubscribeOptions opt;
        bool alive = false;
        uint64_t seq = 0;
        std::shared_ptr<Handler> h;
    };

    static void insertSorted(std::vector<Sub>& v, Sub s) {
        auto pos = std::upper_bound(v.begin(), v.end(), s, [](const Sub& a, const Sub& b) {
            return a.opt.priority != b.opt.priority ? a.opt.priority > b.opt.priority : a.seq < b.seq; });
        v.insert(pos, std::move(s));
    }

    void settle() {
        auto adds = std::move(pendingAdds_);
        pendingAdds_.clear();
        for (auto& [name, s] : adds) insertSorted(channels_[name], std::move(s));
        sweepIfIdle();
    }

    void sweepIfIdle() {
        if (depth_ > 0 || !dirty_) return;
        for (auto it = channels_.begin(); it != channels_.end();) {
            auto& v = it->second;
            v.erase(std::remove_if(v.begin(), v.end(), [](const Sub& s) { return !s.alive; }), v.end());
            it = v.empty() ? channels_.erase(it) : std::next(it);
        }
        dirty_ = false;
    }

    std::unordered_map<std::string, std::vector<Sub>> channels_;
    std::vector<std::pair<std::string, Sub>>          pendingAdds_;
    std::unordered_map<SubscriptionId, std::string>   idToName_;
    std::vector<Event> queue_;
    const Event* current_ = nullptr;
    SubscriptionId nextId_ = 1;
    uint64_t seq_ = 0;
    uint32_t epoch_ = 0;
    int  depth_ = 0, maxDepth_ = kDefaultMaxDepth, flushPasses_ = kDefaultFlushPasses;
    int  liveCount_ = 0, overflowed_ = 0, errors_ = 0;
    bool dirty_ = false, flushing_ = false;
};

} // namespace aver::fw
