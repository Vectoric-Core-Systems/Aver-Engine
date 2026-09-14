#pragma once
// The start screen's list model: the recent projects, the cards drawn for them, and the selection.
//
// WHY THIS IS ITS OWN UNIT. The browser used to keep two parallel containers -- `recents_` (paths,
// most-recent-first) and `cards_` (what the screen draws) -- with an implicit index correspondence
// that nothing enforced. The selection indexed one and "Open Selected" indexed the other, and the
// two stayed aligned only for the prefix where they happened to agree. That produced three separate
// defects from one cause: a selection past the recents could not be opened at all, a failed open
// dropped an entry from one container and not the other so later indices addressed the WRONG
// project, and an upgrade rebuilt the cards while the selection kept pointing at a slot that now
// meant something else.
//
// Fixing the arithmetic at each site would have fixed each instance. Moving the correspondence into
// one place makes the whole class of bug unrepresentable: THE SELECTION IS A PATH, resolved through
// `selectedPath()`, and there is no supported way to turn it back into an index into a container
// the caller happens to be holding.
//
// NO IMGUI, NO FILESYSTEM. `tests/editor/` compiles sandbox sources directly with AVER_WITH_IMGUI
// left undefined and asserts on the non-drawing half -- the same argument `InputOwnership.hpp` and
// `ProjectCopyTest` already make. The caller does the directory walk and the manifest reads and
// hands the results in; this decides order, identity and selection. That is the part with the bugs
// in it, and it is now the part a test can reach.
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver::editor {

// One row on the start screen. Carries what the card DRAWS, so the list is built once from disk
// rather than each frame re-reading a manifest to find out what version to print in a corner.
struct ProjectCard {
    std::string path;       // the .ocproject
    std::string name;       // its file stem
    std::string version;    // CREATEDWITH, or empty when it records none
    bool recent = false;    // came from the recent list rather than the folder scan
};

// The recent list, the cards built from it, and which one is selected.
class RecentProjects {
public:
    // The recent list is capped. Older entries fall off the end when a new project is opened.
    static constexpr usize kMax = 10;

    // ---- the recent list itself ----

    // Parses recent.txt: one path per line, most-recent-first, CR and trailing spaces tolerated.
    // `keep` decides which lines survive, so a test can supply its own answer instead of touching
    // the disk -- the browser passes `fileExists`, which is what drops projects that have gone.
    template <typename KeepFn>
    void parse(const std::string& text, KeepFn keep) {
        recents_.clear();
        usize pos = 0;
        while (pos <= text.size() && recents_.size() < kMax) {
            usize nl = text.find('\n', pos);
            if (nl == std::string::npos) nl = text.size();
            std::string line = text.substr(pos, nl - pos);
            pos = nl + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty() && keep(line)) recents_.push_back(line);
        }
        dirty_ = true;
    }

    // The list as it should be written back, one path per line.
    std::string serialise() const;

    const std::vector<std::string>& recents() const { return recents_; }

    // Moves `path` to the head, capping the list. This is what opening a project does.
    //
    // IT MARKS THE CARDS DIRTY, and that is not defensive. Reordering the recents while leaving the
    // cards in their old order is exactly the desync this class exists to prevent -- it was
    // invisible only because the browser could never be re-entered once a project opened.
    void remember(const std::string& path);

    // Drops `path` from the recent list, marking the cards dirty for the same reason.
    void forget(const std::string& path);

    // ---- the cards ----

    // True when the cards no longer reflect the recent list and must be rebuilt.
    bool dirty() const { return dirty_; }
    void markDirty() { dirty_ = true; }

    // Rebuilds the card list from `found` -- every project the caller located, in any order, each
    // already carrying its name and version.
    //
    // Order is the recent list first, in recent order, then everything else in the order given.
    // Duplicates are dropped by path, recents winning. A recent whose path is not in `found` still
    // gets a card, because a project can be on this machine's recent list while sitting outside the
    // scanned folder entirely.
    //
    // THE SELECTION SURVIVES BY PATH, not by index. Rebuilding used to leave the selection pointing
    // at whatever now occupied that slot, so an upgrade could silently retarget it at a different
    // project. If the selected path is gone from the new list, the selection clears rather than
    // sliding onto a neighbour.
    void rebuild(const std::vector<ProjectCard>& found);

    const std::vector<ProjectCard>& cards() const { return cards_; }

    // ---- the selection ----

    int  selection() const { return sel_; }
    void select(int index);
    void clearSelection() { sel_ = -1; }

    // The selected card's path, or empty when nothing is selected.
    //
    // THE ONLY WAY TO TURN A SELECTION INTO A PROJECT. Every caller that used to index a container
    // itself goes through here, which is what makes the two-array desync unrepresentable rather
    // than merely fixed.
    std::string selectedPath() const;

private:
    std::vector<std::string> recents_;
    std::vector<ProjectCard> cards_;
    bool dirty_ = true;
    int  sel_ = -1;
};

} // namespace aver::editor
