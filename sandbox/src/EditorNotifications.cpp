// The notification model. No ImGui, no filesystem, no clock -- see the header for why.
#include "EditorNotifications.hpp"

#include <algorithm>
#include <utility>

namespace aver::editor {

Notification* NotificationQueue::find(u64 id) {
    for (Notification& n : live_)
        if (n.id == id) return &n;
    return nullptr;
}

u64 NotificationQueue::push(Notification n) {
    std::lock_guard<std::mutex> lk(mutex_);

    // DEDUP ACROSS EVERY LIVE ENTRY, not just the newest -- and that is a deliberate divergence from
    // the graph-print feed, which collapses only CONSECUTIVE duplicates because two alternating
    // prints are themselves the signal there. This is a notice board of at most 32 keyed entries: a
    // shader error repeating behind an unrelated import toast must still collapse, or the board
    // fills with one message.
    if (!n.dedupKey.empty()) {
        for (Notification& e : live_) {
            if (e.dedupKey != n.dedupKey) continue;
            ++e.count;
            e.severity = n.severity;
            e.title    = std::move(n.title);
            e.body     = std::move(n.body);
            e.ttlSec   = n.ttlSec;
            e.sticky   = n.sticky;
            // BACK TO UNSTAMPED so a still-firing error restarts its life rather than fading out
            // underneath its own rising count. This half IS copied from the graph-print overlay.
            e.createdAt = -1.0;
            return e.id;
        }
    }

    n.id = nextId_++;
    n.count = 1;
    n.createdAt = -1.0;   // stamped by the first tick that returns it
    live_.push_back(std::move(n));

    // OVERFLOW DROPS THE OLDEST NON-STICKY. Dropping the newest would be worse than useless -- the
    // entry most worth seeing is the one that just happened -- and a sticky entry is by definition
    // one the user has not acknowledged, so it outranks age.
    while (live_.size() > kMaxLive) {
        auto victim = std::find_if(live_.begin(), live_.end(),
                                   [](const Notification& e) { return !e.sticky; });
        // Every live entry is sticky: drop the oldest anyway rather than exceed the cap, because an
        // unbounded queue fed by the log sink is the failure this cap exists to prevent.
        if (victim == live_.end()) victim = live_.begin();
        live_.erase(victim);
    }
    return live_.back().id;
}

// Re-inserts a notification whose toast has already expired, so a caller holding a handle across a
// long operation can update unconditionally. Ids are never reused, so this can only ever revive the
// caller's own entry. Caller holds mutex_.
static Notification& reviveOrFind(std::deque<Notification>& live, u64 id, Notification* found) {
    if (found) return *found;
    Notification n;
    n.id = id;
    n.createdAt = -1.0;
    live.push_back(std::move(n));
    return live.back();
}

void NotificationQueue::setProgress(u64 id, f32 progress, std::string note) {
    if (!id) return;
    std::lock_guard<std::mutex> lk(mutex_);
    Notification& n = reviveOrFind(live_, id, find(id));
    n.hasProgress = true;
    // Negative is the indeterminate sentinel and is preserved; anything else is clamped, so a caller
    // computing a fraction from a zero total cannot produce a bar longer than the widget.
    n.progress = progress < 0.0f ? -1.0f : std::min(progress, 1.0f);
    n.progressNote = std::move(note);
}

void NotificationQueue::update(u64 id, NotifySeverity sev, std::string title, std::string body) {
    if (!id) return;
    std::lock_guard<std::mutex> lk(mutex_);
    Notification& n = reviveOrFind(live_, id, find(id));
    n.severity = sev;
    n.title = std::move(title);
    n.body  = std::move(body);
}

void NotificationQueue::finish(u64 id, NotifySeverity sev, std::string title, std::string body,
                               f64 ttlSec) {
    if (!id) return;
    std::lock_guard<std::mutex> lk(mutex_);
    Notification& n = reviveOrFind(live_, id, find(id));
    n.severity = sev;
    n.title = std::move(title);
    n.body  = std::move(body);
    n.hasProgress = false;
    n.progressNote.clear();
    n.ttlSec = ttlSec;
    // FRESH LIFE FROM THE FRAME THAT DRAWS THE RESULT. A bake that ran for four minutes must not
    // have its "Done" flash for the remainder of a ttl that started when the bake did.
    n.createdAt = -1.0;
}

void NotificationQueue::close(u64 id) {
    if (!id) return;
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto it = live_.begin(); it != live_.end(); ++it)
        if (it->id == id) { live_.erase(it); return; }
}

void NotificationQueue::dismiss(u64 id) { close(id); }

// THE CONTRACT FOR THIS FUNCTION, because getting it wrong deadlocks the editor rather than failing
// a test. It is called from SandboxApp's log sink, which Log.cpp invokes WITH THE CORE LOG MUTEX
// HELD, and which itself holds the editor's own logMutex_. This call therefore sits three locks
// deep.
//
//   MAY: take mutex_, build at most two strings, push/pop a deque bounded at kMaxLive, bump a count.
//   MUST NOT: call any AVER_* macro -- Log.hpp states it outright and std::mutex is not recursive,
//             so one log line here deadlocks against the outermost lock. Must not touch ImGui (wrong
//             thread, no live frame -- hence createdAt = -1.0 and hence severity->colour living
//             entirely on the draw side). Must not touch the filesystem, block, or launch anything:
//             crash::noteCritical is deliberately called OUTSIDE the log lock because CreateProcess
//             is too slow to hold a mutex across, and this must not undo that by being slow itself.
void NotificationQueue::pushFromLog(LogLevel level, std::string_view message) {
    // Fatal is excluded because AVER_FATAL does not return -- the process is gone before another
    // frame could draw. Warn is excluded because this editor logs Warn for ordinary outcomes
    // ("already exists", "could not write ... autosave is off"); those are pushed explicitly at
    // their call sites with wording chosen for a person, not sniffed out of a log line.
    if (level != LogLevel::Error && level != LogLevel::Critical) return;

    Notification n;
    n.severity = level == LogLevel::Critical ? NotifySeverity::Critical : NotifySeverity::Error;
    // The message text IS the identity of "that error again", which is what makes a storm collapse.
    n.dedupKey.assign(message);

    // Every AVER_* call in this tree carries a "[Tag] " prefix. Lifting it into the title keeps the
    // toast scannable; the rest becomes the body.
    std::string_view tag, rest = message;
    if (!message.empty() && message.front() == '[') {
        const usize close = message.find(']');
        if (close != std::string_view::npos) {
            tag = message.substr(1, close - 1);
            rest = message.substr(close + 1);
            while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
        }
    }
    const bool critical = n.severity == NotifySeverity::Critical;
    n.title.assign(critical ? "Critical" : "Error");
    if (!tag.empty()) { n.title += " - "; n.title.append(tag); }
    n.body.assign(rest);

    if (critical) {
        // STICKY, because this is the class of thing that must not scroll away. A Critical has
        // already woken the crash reporter; the user should not learn that only from a log they
        // never opened. This is additive to crash::noteCritical, not a replacement for it.
        n.sticky = true;
        n.actions[0] = NotifyAction::ShowOutputLog;
        n.actionLabels[0] = "Show in Output Log";
        n.actions[1] = NotifyAction::Dismiss;
        n.actionLabels[1] = "Dismiss";
    } else {
        n.ttlSec = 8.0;
        n.actions[0] = NotifyAction::ShowOutputLog;
        n.actionLabels[0] = "Show in Output Log";
    }
    push(std::move(n));
}

void NotificationQueue::tick(f64 now, std::vector<Notification>& out, usize& hidden) {
    out.clear();
    hidden = 0;
    std::lock_guard<std::mutex> lk(mutex_);

    // REAPED AT DRAW TIME, not on push. Nothing ages while no frame is being drawn, which is what
    // makes the unstamped convention work.
    for (auto it = live_.begin(); it != live_.end();) {
        const bool immortal = it->sticky || it->hasProgress;
        const bool stamped  = it->createdAt >= 0.0;
        if (!immortal && stamped && (now - it->createdAt) >= it->ttlSec) it = live_.erase(it);
        else ++it;
    }

    if (live_.size() > kMaxVisible) hidden = live_.size() - kMaxVisible;

    // The visible window is the NEWEST kMaxVisible; older ones wait. Only what is returned gets
    // stamped, so a queued entry starts its life when it first becomes visible.
    usize first = live_.size() > kMaxVisible ? live_.size() - kMaxVisible : 0;
    for (usize i = first; i < live_.size(); ++i) {
        if (live_[i].createdAt < 0.0) live_[i].createdAt = now;
        out.push_back(live_[i]);
    }
}

void NotificationQueue::activate(u64 id, NotifyAction action) {
    if (!id || action == NotifyAction::None) return;
    std::lock_guard<std::mutex> lk(mutex_);
    // Activating an id that is already gone is a no-op rather than an error: the toast can expire
    // between the click and the drain.
    for (const Notification& n : live_)
        if (n.id == id) { pending_.emplace_back(id, action); return; }
}

bool NotificationQueue::drainActivation(u64& outId, NotifyAction& outAction) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (pending_.empty()) return false;
    outId = pending_.front().first;
    outAction = pending_.front().second;
    pending_.pop_front();
    return true;
}

usize NotificationQueue::size() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return live_.size();
}

void NotificationQueue::clear() {
    std::lock_guard<std::mutex> lk(mutex_);
    live_.clear();
    pending_.clear();
}

NotificationQueue& notifications() {
    static NotificationQueue q;
    return q;
}

} // namespace aver::editor
