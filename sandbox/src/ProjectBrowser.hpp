#pragma once
// The start screen: recent projects, New Project, Open Project.
//
// It is an ImGui screen inside the editor's own window and ImGui host, not a launcher process.
// The engine already owns a window, a swapchain and an ImGui host by the time this draws, so a
// separate exe would duplicate all of it to show one list.
//
// It is deliberately NOT reachable from automation: the verification harness runs the editor with
// --frames and reads one probe pixel, and a start screen in front of the viewport would break
// every gate. SandboxApp decides whether to arm this; see createApplication().
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

class ProjectBrowser {
public:
    void init();  // read the recent list from disk

    // One frame of the start screen. `medium` may be null when only the fallback font loaded.
    //
    // `logoTex` is the UI texture identifier of the engine mark (rhi::IDevice::uiTextureId), or 0
    // when the caller could not load it. The image is passed in rather than loaded here so this
    // stays a UI screen: the app owns asset paths, the device and the lifetime, and knows whether
    // the screen is armed at all -- automation must not pay to upload a mark it never shows.
    BrowserAction draw(f32 dpi, ImFont* medium, u64 logoTex, f32 logoAspect);

    const fmt::ProjectDesc& project() const { return project_; }

    // Load a manifest and, on success, move it to the head of the recent list. Used by the
    // browser's own buttons and by the command line, so a project opened either way is recorded.
    bool open(const std::string& manifestPath, std::string* err);

private:
    void saveRecents() const;
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
