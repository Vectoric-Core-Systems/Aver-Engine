#pragma once
// SnapshotUndo<State> -- the whole-state snapshot undo/redo triad that GraphEditor, SoundEditor and
// BtEditor each hand-rolled as a near-identical ~25-40 line copy (SoundEditor's and BtEditor's own
// comments cite GraphEditor's pushUndo() as the source they copied): a 200-entry cap, the redo stack
// cleared on every push, and the inverse pushed back on undo/redo so the OTHER direction always
// works. This header is that copy, made once.
//
// NO BEHAVIOUR CHANGE FROM ANY OF THE THREE ORIGINALS -- this is a mechanical lift, not a redesign:
//   * push()       == pushUndo(): append, evict the OLDEST entry (index 0) once past the cap, then
//                     clear the redo stack. Same order as the originals.
//   * cancelPush() == the `undoStack_.pop_back()` every structural edit calls when the edit it just
//                     pushed for turns out to have refused (add/link/reparent all do this) -- pops
//                     the speculative entry WITHOUT touching the redo stack, exactly like the
//                     originals' direct vector call. Like theirs, calling this without an
//                     immediately preceding push() is a caller bug (undefined here exactly as
//                     `pop_back()` on an empty vector is undefined there).
//   * undo()/redo() == the originals' own undo()/redo(): push the CURRENT value onto the other
//                     stack, pop this stack's back into `value`, report whether there was anything
//                     to pop. Mutating `value` in place is what makes this a near-drop-in
//                     replacement for both shapes the three originals actually use it at: Sound/Bt
//                     hand their one live record straight in (State IS the record), and Graph
//                     assembles a temporary {graph, displayPos} pair around its two live members
//                     first (see GraphEditor.hpp's own UndoState).
//
// WHAT THIS TYPE DELIBERATELY DOES NOT OWN: dirty flags, selection indices, validation state, or any
// other per-editor bookkeeping an undo/redo must also touch. All three originals do MORE than swap
// state in their own undo()/redo() (BtEditor and SoundEditor re-clamp a selection INDEX that the
// swap may have invalidated; GraphEditor additionally clears its node/link selection SETS). That
// work stays in each editor's own undo()/redo(), immediately after the call into this type -- folding
// it in here would mean this template quietly grows an editor-specific selection model the day a
// fourth caller's "current edit position" does not happen to be a single integer.
//
// State is stored BY VALUE in both stacks and must be copyable (GraphEditor::UndoState,
// fmt::OcSoundData and fmt::OcBtData all already are, which is what made the original triads
// possible in the first place) -- a whole-state snapshot IS a copy of the whole state, by
// construction. That copy is why AnimEditor snapshots only what it edits (notifies, curves, flags, or
// its skeleton's sockets) rather than its whole clip_, whose full sample arrays would be copied on
// every edit -- this header does nothing to hide that cost, so the State a caller picks is the lever.
#include <cstddef>
#include <utility>
#include <vector>

namespace aver::editor {

template <typename State>
class SnapshotUndo {
public:
    // The cap every one of the three original copies used, unchanged.
    static constexpr std::size_t kCap = 200;

    // Appends `state` as a new undo entry, evicts the oldest entry once past kCap, and clears the
    // redo stack -- pushUndo()'s own three steps, in the same order.
    void push(State state) {
        undo_.push_back(std::move(state));
        if (undo_.size() > kCap) undo_.erase(undo_.begin());
        redo_.clear();
    }

    // Pops the entry a push() just added, without touching the redo stack -- for the "push, attempt
    // the edit, cancel if it refused" pattern every structural add/link/reparent call site in all
    // three originals repeats. Caller's responsibility to have just pushed; see the header comment.
    void cancelPush() { undo_.pop_back(); }

    // Undoes: pushes a copy of `value` onto the redo stack, then overwrites `value` with the most
    // recent undo entry and pops it. Returns false (leaving `value` untouched) when there is
    // nothing to undo -- the originals' own `if (undoStack_.empty()) return;` guard, hoisted here.
    bool undo(State& value) {
        if (undo_.empty()) return false;
        redo_.push_back(value);
        value = std::move(undo_.back());
        undo_.pop_back();
        return true;
    }

    // Redoes: the exact mirror of undo() against the other stack.
    bool redo(State& value) {
        if (redo_.empty()) return false;
        undo_.push_back(value);
        value = std::move(redo_.back());
        redo_.pop_back();
        return true;
    }

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }

    // Raw depths, for a caller that derives its own edit counter from them (GraphEditor's material
    // preview does exactly this: undoCount() - redoCount() moves once per pushUndo() and nowhere
    // else, which is what makes it a better "has this graph changed" signal than a plain dirty_ flag
    // that latches true on the first edit and never resets). Nothing here interprets the difference;
    // that reading belongs to the caller, not to this template.
    std::size_t undoCount() const { return undo_.size(); }
    std::size_t redoCount() const { return redo_.size(); }

    // Both stacks empty -- loadFromDisk()'s own reset in all three originals.
    void clear() { undo_.clear(); redo_.clear(); }

private:
    std::vector<State> undo_;
    std::vector<State> redo_;
};

} // namespace aver::editor
