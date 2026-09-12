// SnapshotUndo<State> -- the whole-state undo/redo triad shared by GraphEditor/SoundEditor/BtEditor
// (and, from tonight, ParticleEditor). Pure and header-only: no ImGui, no Engine, no scene::, so
// every rule the three original hand-rolled copies relied on is checked directly against the
// template rather than through one editor's own load/save plumbing.
#include "../../../sandbox/src/SnapshotUndo.hpp"

#include "aver/core/Log.hpp"

#include <string>

using namespace aver;
using editor::SnapshotUndo;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

int main() {
    AVER_INFO("=== SnapshotUndo ===");

    // ---- push / undo / redo, the basic round trip -----------------------------------------------
    {
        SnapshotUndo<int> u;
        check(!u.canUndo() && !u.canRedo(), "a fresh stack starts empty both ways");

        int value = 0;
        u.push(value);      // snapshot "0" before the edit that follows
        value = 1;
        check(u.canUndo() && !u.canRedo(), "a push makes undo available and leaves redo empty");

        check(u.undo(value), "undo reports it did something");
        check(value == 0, "and restores the value the snapshot was taken from");
        check(!u.canUndo() && u.canRedo(), "undo drains the undo stack into the redo stack");

        check(u.redo(value), "redo reports it did something");
        check(value == 1, "and restores what undo just replaced");
        check(u.canUndo() && !u.canRedo(), "redo drains the redo stack back into the undo stack");
    }

    // ---- redo is cleared on the NEXT push, not just consumed by undo -----------------------------
    {
        SnapshotUndo<int> u;
        int value = 0;
        u.push(value); value = 1;
        u.push(value); value = 2;
        check(u.undo(value) && value == 1, "first undo steps back one edit");
        check(u.canRedo(), "leaving a redo entry available");

        u.push(value); value = 3;   // a NEW edit after an undo -- must clear the redo branch
        check(!u.canRedo(), "pushing a new edit after an undo clears the redo stack");
        check(!u.redo(value), "so redo() now correctly reports nothing to do");
        check(value == 3, "and leaves the value untouched when it does");
    }

    // ---- undo at empty / redo at top: both report false and touch nothing ------------------------
    {
        SnapshotUndo<int> u;
        int value = 42;
        check(!u.undo(value), "undo on a fresh stack reports false");
        check(value == 42, "and does not touch the value");
        check(!u.redo(value), "redo on a fresh stack reports false");
        check(value == 42, "and does not touch the value either");

        u.push(value);
        check(u.undo(value), "one undo after one push succeeds");
        check(!u.undo(value), "a second undo, past the bottom of the stack, reports false");
        check(u.canRedo(), "the one redo entry from the successful undo is still there");
    }

    // ---- the 200-entry cap evicts the OLDEST entry, not the newest --------------------------------
    {
        SnapshotUndo<int> u;
        // Push 0..200 (201 pushes): the 201st push (value 200) crosses the cap, so push(0) is the
        // one entry that must fall off the FRONT, leaving exactly 200 entries: 1..200.
        for (int i = 0; i <= 200; ++i) u.push(i);
        int value = 999;
        int lastPopped = -1;
        // undo() pops from the BACK (most recent first), so 200 undos drain 200, 199, ..., down to
        // whatever survived eviction at the front -- the LAST of these 200 calls is the one that
        // proves what that oldest surviving entry actually is.
        for (int i = 0; i < 200; ++i) {
            check(u.undo(value), "the cap keeps exactly 200 entries, so 200 undos all succeed");
            lastPopped = value;
        }
        check(!u.canUndo(), "and the 201st undo has nothing left");
        check(lastPopped == 1, "the oldest surviving entry is 1, not 0 -- "
                                "push(0) was the one evicted when push(200) crossed the cap");
    }

    // ---- cancelPush(): the "push, attempt, cancel on refusal" pattern every structural add/link/
    // reparent call site in all three originals repeats (e.g. BtEditor::addChild: pushUndo(); ...
    // if (added < 0) { undoStack_.pop_back(); return; }) --------------------------------------------
    {
        SnapshotUndo<int> u;
        int value = 0;
        u.push(value); value = 1;
        check(u.undo(value) && value == 0, "set up one redo entry ahead of the speculative push");
        check(u.canRedo(), "redo entry present before the speculative push");

        u.push(value);        // a structural edit about to be attempted -- push() ALREADY clears
                               // redo here, matching pushUndo()'s own unconditional redoStack_.clear()
        check(!u.canRedo(), "push() clears redo up front, same as every original -- even one about "
                             "to be cancelled");
        u.cancelPush();       // ...and the edit refused, so the caller cancels the speculative entry
        check(!u.canUndo(), "cancelPush removes only the entry push() just added");
        check(!u.canRedo(), "and does not (cannot) bring back what push() already cleared -- a "
                             "refused structural edit still costs the redo stack, exactly like the "
                             "three originals' own `pushUndo(); ...; undoStack_.pop_back();` shape");
    }

    // ---- a struct State, matching GraphEditor's own {graph, displayPos} shape ---------------------
    {
        struct Pair { int a; int b; };
        SnapshotUndo<Pair> u;
        Pair p{1, 10};
        u.push(p);
        p = Pair{2, 20};
        check(u.undo(p), "a struct State round-trips through undo like a scalar one");
        check(p.a == 1 && p.b == 10, "restoring every field, not just the first");
    }

    // ---- clear(): both stacks go empty, matching loadFromDisk()'s own reset -----------------------
    {
        SnapshotUndo<int> u;
        int value = 0;
        u.push(value); value = 1;
        u.undo(value);
        check(u.canRedo(), "sanity: a redo entry exists before clear()");
        u.clear();
        check(!u.canUndo() && !u.canRedo(), "clear() empties both stacks");
    }

    if (g_failures == 0) AVER_INFO("=== all {} SnapshotUndo checks passed ===", g_checks);
    else                 AVER_ERROR("=== {} of {} SnapshotUndo check(s) FAILED ===", g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
