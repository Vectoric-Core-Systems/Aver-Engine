#pragma once
// The start screen: recent projects, New Project, Open Project. An ImGui screen inside the editor's
// own window, not a launcher process.
#include "aver/formats/OcProject.hpp"

#include <string>
#include <vector>

struct ImFont;

namespace aver::editor {

// What the browser wants to happen after this frame.
enum class BrowserAction {
    Stay,    // still choosing
    Open,    // project() is loaded — go to the editor
    Skip,    // continue with no project (the editor works fine without one)
    Quit,
};

// The start screen: the recent list, its modals, and the project it ends up opening.
class ProjectBrowser {
public:
    // Reads the recent list from disk.
    void init();

    // Draws one frame of the start screen. `medium` may be null; `logoTex` is the engine mark's UI
    // texture id, or 0 when the caller loaded none.
    BrowserAction draw(f32 dpi, ImFont* medium, u64 logoTex, f32 logoAspect);

    const fmt::ProjectDesc& project() const { return project_; }

    // Loads a manifest and, on success, moves it to the head of the recent list.
    bool open(const std::string& manifestPath, std::string* err);

    // Opens `path` unless it was made by an older SERIES, in which case it raises the upgrade modal
    // and opens nothing. Public because a project named on the command line has to pass the same
    // gate a double-clicked card does. False with upgradePending() true means "a question is
    // waiting on the start screen", which is not a failure.
    bool openOrOfferUpgrade(const std::string& path, std::string* err = nullptr);

    // True while the upgrade modal is waiting to be shown or answered.
    bool upgradePending() const { return upgradeModal_ || !upgradePath_.empty(); }

    // One row on the start screen. Carries what the card DRAWS, so the list is built once from disk
    // rather than each frame re-reading a manifest to find out what version to print in a corner.
    struct Card {
        std::string path;       // the .ocproject
        std::string name;       // its file stem
        std::string version;    // CREATEDWITH, or empty when it records none
        bool recent = false;    // in the recent list, so it sorts above the rest
    };

private:
    // Writes the recent list back to disk.
    void saveRecents() const;
    // Drops one manifest from the recent list.
    void forget(const std::string& manifestPath);

    // Rebuilds `cards_` from the recent list AND every .ocproject under the projects folder, so a
    // project the author has never opened on this machine still appears. Reads each manifest once,
    // for its version stamp.
    void rescan();

    // Copies a whole project tree beside itself and returns the new manifest path, or empty.
    std::string copyProjectTree(const std::string& manifestPath, std::string* err) const;

    fmt::ProjectDesc project_;
    std::vector<std::string> recents_;
    std::vector<Card> cards_;
    bool cardsDirty_ = true;
    int recentSel_ = -1;

    // ---- the upgrade prompt ----
    // A project made by an older series is not opened until the author chooses what happens to it.
    // Converting in place is destructive and irreversible -- there is no rollback inside a migration
    // chain -- so it is gated behind typing the project's own name, the same way a repository host
    // gates deleting one. Copy is the default because it cannot lose anything.
    bool  upgradeModal_ = false;
    std::string upgradePath_;     // the manifest being offered
    std::string upgradeName_;     // its stem, which is what must be typed to convert in place
    std::string upgradeFrom_;     // the version it records
    std::string upgradeError_;
    char  upgradeConfirmBuf_[96] = {};
    std::string error_;          // shown in red under the columns
    std::string newError_;       // ...and inside the New Project modal
    bool openNewModal_ = false;
    char nameBuf_[96] = {};
    char locBuf_[512] = {};
    char pathBuf_[512] = {};
};

} // namespace aver::editor
