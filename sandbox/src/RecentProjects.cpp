// RecentProjects -- see the header for why the selection is a path rather than an index.
#include "RecentProjects.hpp"

namespace aver::editor {

std::string RecentProjects::serialise() const {
    std::string text;
    for (const std::string& p : recents_) text += p + "\n";
    return text;
}

void RecentProjects::remember(const std::string& path) {
    if (path.empty()) return;
    forget(path);                                    // also marks dirty
    recents_.insert(recents_.begin(), path);
    if (recents_.size() > kMax) recents_.resize(kMax);
    dirty_ = true;
}

void RecentProjects::forget(const std::string& path) {
    for (usize i = 0; i < recents_.size(); ++i) {
        if (recents_[i] != path) continue;
        recents_.erase(recents_.begin() + static_cast<isize>(i));
        dirty_ = true;
        return;
    }
}

void RecentProjects::rebuild(const std::vector<ProjectCard>& found) {
    // Remembered BEFORE the list is rebuilt, so the selection can be restored by identity below.
    const std::string wasSelected = selectedPath();

    cards_.clear();
    dirty_ = false;

    const auto has = [this](const std::string& path) {
        for (const ProjectCard& c : cards_) if (c.path == path) return true;
        return false;
    };
    const auto findFound = [&found](const std::string& path) -> const ProjectCard* {
        for (const ProjectCard& c : found) if (c.path == path) return &c;
        return nullptr;
    };

    // The recent list first, in its own order.
    for (const std::string& r : recents_) {
        if (has(r)) continue;
        if (const ProjectCard* src = findFound(r)) {
            ProjectCard c = *src;
            c.recent = true;
            cards_.push_back(std::move(c));
        } else {
            // On the recent list but not among what the caller found -- a project outside the
            // scanned folder. It still gets a row; only its details are unknown.
            ProjectCard c;
            c.path = r;
            c.recent = true;
            cards_.push_back(std::move(c));
        }
    }

    // Then everything else, in the order given.
    for (const ProjectCard& c : found) {
        if (has(c.path)) continue;
        ProjectCard copy = c;
        copy.recent = false;
        cards_.push_back(std::move(copy));
    }

    // Restore by path. A selection whose project has left the list clears rather than sliding onto
    // whichever project now occupies that index.
    sel_ = -1;
    if (wasSelected.empty()) return;
    for (usize i = 0; i < cards_.size(); ++i) {
        if (cards_[i].path == wasSelected) { sel_ = static_cast<int>(i); return; }
    }
}

void RecentProjects::select(int index) {
    sel_ = (index >= 0 && index < static_cast<int>(cards_.size())) ? index : -1;
}

std::string RecentProjects::selectedPath() const {
    if (sel_ < 0 || sel_ >= static_cast<int>(cards_.size())) return {};
    return cards_[static_cast<usize>(sel_)].path;
}

} // namespace aver::editor
