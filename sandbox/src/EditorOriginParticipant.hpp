#pragma once
// The editor's part of a floating-origin shift (LARGE_WORLD 2.7), plugged into game::OriginParticipants as the order-80
// participant (OriginBindings::editorPrepare / editorCommit).
//
//   prepare  = the gesture gate. A gizmo drag, a nudge or a Details drag, a marquee, a sculpt, paint or foliage stroke and a
//              water edit all hold positions or screen state across frames, so a shift is REFUSED (and retried next frame)
//              while any is open. When the event is `urgent` (the camera is past 1,000,000 cm and precision is going) the
//              gate cannot wait: it calls each open gesture's cancel, which must END it, and accepts.
//   commit   = the rebasers, in registration order: undo and redo stacks, clipboard, Play-start snapshots, the foliage brush's
//              placed list, ... Each is a closure over the editor's own state, so this class owns no editor type and a test
//              can drive it headless.
//
// A cancel runs inside prepare, BEFORE the shift, so it works in the old frame and the stored "before" values are still valid:
// a transform gesture restores them, a stroke is ended through its normal path (its undo entry is pushed), nothing is reverted
// that cannot be put back exactly.
#include "aver/world/WorldOrigin.hpp"

#include <functional>
#include <vector>

namespace aver::editor {

class EditorOriginParticipant {
public:
    // `reason` is a string literal: it outlives the call and is what Stats::lastDeferReason shows.
    void addGesture(const char* reason, std::function<bool()> active, std::function<void()> cancel) {
        gestures_.push_back({reason, std::move(active), std::move(cancel)});
    }
    // `what` names the state for the readout and the test.
    void addRebaser(const char* what, std::function<void(const world::ShiftEvent&)> fn) {
        rebasers_.push_back({what, std::move(fn)});
    }

    world::PrepareResult prepare(const world::ShiftEvent& ev) {
        const char* firstOpen = nullptr;
        for (const Gesture& g : gestures_) {
            if (!g.active || !g.active()) continue;
            if (!firstOpen) firstOpen = g.reason;
            if (!ev.urgent) continue;
            if (g.cancel) g.cancel();
            ++cancelled_;
        }
        if (!firstOpen) return {};
        if (!ev.urgent) { ++refused_; lastRefusal_ = firstOpen; return {false, firstOpen}; }
        lastCancelled_ = firstOpen;
        return {};   // every open gesture was ended above
    }

    void commit(const world::ShiftEvent& ev) {
        lastGeneration_ = ev.generation;
        ++commits_;
        for (const Rebaser& r : rebasers_) if (r.fn) r.fn(ev);
    }

    // An overlay cache keyed on world positions folds this into its signature, so it rebuilds after a shift (the collider
    // overlay's colliderOverlaySig_): a generation counter in FNV-1a's step, so equal inputs give equal outputs.
    static u64 overlaySignature(u64 sig, u32 originGeneration) {
        return (sig ^ static_cast<u64>(originGeneration)) * 1099511628211ull;
    }

    u32 commits() const { return commits_; }
    u32 refused() const { return refused_; }
    u32 cancelled() const { return cancelled_; }
    u32 lastGeneration() const { return lastGeneration_; }
    const char* lastRefusal() const { return lastRefusal_; }
    const char* lastCancelled() const { return lastCancelled_; }

private:
    struct Gesture { const char* reason; std::function<bool()> active; std::function<void()> cancel; };
    struct Rebaser { const char* what; std::function<void(const world::ShiftEvent&)> fn; };
    std::vector<Gesture> gestures_;
    std::vector<Rebaser> rebasers_;
    u32 commits_ = 0, refused_ = 0, cancelled_ = 0, lastGeneration_ = 0;
    const char* lastRefusal_ = nullptr;
    const char* lastCancelled_ = nullptr;
};

} // namespace aver::editor
