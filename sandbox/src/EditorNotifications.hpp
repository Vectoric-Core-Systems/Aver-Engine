#pragma once
// Transient editor notifications: the "what just happened" channel.
//
// WHY THIS EXISTS. Until now the editor reported outcomes into three places, none of which a user
// reliably sees: the Output Log (a drawer that is closed by default), cbStatus_ (a footer visible
// only while the Content Browser drawer is open), and upgradeStatus_ (a 12-second string wedged
// into the status bar). Import failures literally read "... see the Output Log" -- an instruction
// naming a panel the message had no way to open. An autosave that failed disabled itself for the
// rest of the session and said so once, to the log.
//
// NOT A SECOND LOG. The Output Log already exists and scrolls; this is a small stack of recent
// outcomes that fades on its own, and the moment it becomes something to scroll it has failed. The
// graph-print overlay in SandboxApp.cpp makes the same argument and this borrows its shape.
//
// DELIBERATELY ImGui-FREE. This header must never include imgui.h and must never be wrapped in
// `#if AVER_WITH_IMGUI`. EditorKeybinds.hpp is whole-file-guarded and that is exactly why testing it
// drags ImGui in; InputOwnership.hpp exists as its own file for precisely this reason. Everything
// here is arithmetic over plain data with time injected as a parameter, so EditorNotificationsTest
// links Aver.Core and nothing else. If this file ever needs a live ImGui context, the split has been
// undone -- the drawing half lives in SandboxApp.cpp and stays there.
#include "aver/core/Log.hpp"
#include "aver/core/Types.hpp"

#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace aver::editor {

enum class NotifySeverity : u8 { Info, Success, Warning, Error, Critical };

// What a notification's button does when pressed.
//
// AN ENUM, NOT A std::function. Notifications are pushed from the log sink, which runs on whatever
// thread logged and holds two mutexes already; a callback capturing the editor would be a lifetime
// hazard stored inside a lock. The draw layer drains an activation and acts on it, which is the same
// "record the request, act on it where it is safe" shape cbWantRename_/cbWantDelete_ already use.
enum class NotifyAction : u8 {
    None,
    Dismiss,
    ShowOutputLog,      // the OUTPUT LOG, not the Console -- the Console keeps its own scrollback
    PostponeAutosave,
    RetryAutosave,
};

struct Notification {
    u64            id = 0;
    NotifySeverity severity = NotifySeverity::Info;
    std::string    title;
    std::string    body;
    // Empty means "never collapse". Otherwise a repeat with the same key bumps `count` instead of
    // adding a row.
    std::string    dedupKey;
    u32            count = 1;

    // -1.0 MEANS UNSTAMPED, AND IT IS THE LOAD-BEARING FIELD HERE.
    //
    // The producer cannot take a timestamp: the log sink has no business calling into the UI clock,
    // and it may run on any thread. So an entry is stamped by the first tick() that actually
    // RETURNS it, which buys three things at once:
    //   - a notification pushed while the editor was minimised, or during a blocking level load,
    //     gets its full lifetime when a frame finally draws it rather than having expired unseen;
    //   - entries queued behind kMaxVisible do not age while they are invisible, so the sixth toast
    //     of a burst still gets its full time when it reaches the front;
    //   - a repeat of a still-firing error resets to unstamped, so it cannot fade out from under
    //     its own rising count.
    f64  createdAt = -1.0;
    f64  ttlSec = 6.0;
    bool sticky = false;          // ignores ttlSec entirely; leaves only by dismiss/close

    // While true, ttlSec is not counted. This is how a long operation's toast survives a slow step
    // without every caller having to think about timing. finish() clears it and re-arms createdAt.
    bool hasProgress = false;
    f32  progress = 0.0f;         // 0..1, or negative for indeterminate
    std::string progressNote;

    NotifyAction actions[2] = {NotifyAction::None, NotifyAction::None};
    std::string  actionLabels[2];
};

// A bounded, thread-safe, self-expiring stack of recent outcomes.
//
// THREADING. push/setProgress/update/finish/close/pushFromLog may be called from any thread and take
// this class's own mutex. tick/activate/drainActivation/dismiss are the draw thread's half; they
// take the same mutex, so the split is a statement about who calls what, not about locking.
class NotificationQueue {
public:
    // Hard cap. The log sink can push, so this must be bounded by construction rather than by
    // whoever happens to be logging: a shader recompile emitting 300 identical errors in one frame
    // must cost one entry with count == 300, not 300 entries.
    static constexpr usize kMaxLive = 32;
    // How many are drawn at once. The rest wait, unstamped and therefore unaged.
    static constexpr usize kMaxVisible = 5;

    // ---- producer side: any thread ----------------------------------------------------------

    // Adds one, or collapses into a live entry with the same non-empty dedupKey. Returns the id of
    // whichever entry now represents it. Never returns 0.
    u64 push(Notification n);

    // Updates a live entry in place. If the id has already expired the entry is RE-INSERTED with the
    // same id, so a caller holding a handle across a slow operation never has to check whether its
    // toast is still alive. Ids are monotonic and never reused, so a stale handle cannot address
    // somebody else's notification.
    void setProgress(u64 id, f32 progress, std::string note);
    void update(u64 id, NotifySeverity sev, std::string title, std::string body);
    // Replaces both action slots at once. Pass None to clear one. Setting them is how a countdown
    // withdraws its own Postpone button the moment postponing stops being possible.
    void setActions(u64 id, NotifyAction a0, std::string label0,
                    NotifyAction a1, std::string label1);
    void setSticky(u64 id, bool sticky);
    // Ends a progress notification: clears hasProgress and re-arms the stamp, so the "Done" state
    // gets a fresh full ttl starting from the frame that draws it.
    void finish(u64 id, NotifySeverity sev, std::string title, std::string body, f64 ttlSec);
    void close(u64 id);

    // The log sink's one entry point. See the contract comment on its definition -- it runs with the
    // core log mutex held and MUST NOT log, touch ImGui, touch the filesystem, or block.
    void pushFromLog(LogLevel level, std::string_view message);

    // ---- consumer side: the drawing thread ---------------------------------------------------

    // Stamps unstamped entries with `now`, reaps expired ones, and fills `out` with at most
    // kMaxVisible, oldest first. `hidden` receives how many more are queued.
    //
    // TIME IS A PARAMETER, not ImGui::GetTime(). That single choice is what makes every rule in this
    // class testable without a graphics context.
    void tick(f64 now, std::vector<Notification>& out, usize& hidden);

    void activate(u64 id, NotifyAction action);
    // Pops one pending activation. False when there is none.
    bool drainActivation(u64& outId, NotifyAction& outAction);
    void dismiss(u64 id);

    usize size() const;
    void  clear();

private:
    Notification* find(u64 id);           // caller holds mutex_

    mutable std::mutex       mutex_;
    std::deque<Notification> live_;
    u64                      nextId_ = 1;
    std::deque<std::pair<u64, NotifyAction>> pending_;
};

// The process-wide queue.
//
// A SEAM, NOT AN OWNER. The asset editors (ActorEditor, AnimEditor, GraphEditor, BtEditor) are in
// editor:: and cannot reach SandboxApp, but they have outcomes worth reporting. This accessor is how
// they post without anything depending on the editor application object; SandboxApp still owns the
// drawing. Mirrors scene::World::instance() and voxi::Renderer::get(), both already used this way.
NotificationQueue& notifications();

} // namespace aver::editor
