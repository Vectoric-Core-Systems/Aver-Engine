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

private:
    // Writes the recent list back to disk.
    void saveRecents() const;
    // Drops one manifest from the recent list.
    void forget(const std::string& manifestPath);

    fmt::ProjectDesc project_;
    std::vector<std::string> recents_;
    int recentSel_ = -1;
    std::string error_;          // shown in red under the columns
    std::string newError_;       // ...and inside the New Project modal
    bool openNewModal_ = false;
    char nameBuf_[96] = {};
    char locBuf_[512] = {};
    char pathBuf_[512] = {};
};

} // namespace aver::editor
