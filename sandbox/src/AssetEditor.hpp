#pragma once
// Asset editors, the way Unreal separates them: the Level Editor is the main window, and every other
// asset opens its OWN editor with its own tabs, its own selection and its own dirty state.
//
// WHAT THIS REPLACES. Double-clicking an asset in the Content Browser used to hand it to the shell --
// a .ocmesh went to whatever Windows associates with the extension, which is nothing. There was no
// notion that an asset HAS an editor, so every panel the editor grew had to live in the one main
// dockspace and be about whatever happened to be selected in the level. That does not scale past two
// asset types, and it is why Unreal separates them.
//
// TABS RATHER THAN OS WINDOWS, for now. Unreal docks Blueprint editors as tabs by default and lets
// you tear them off; tearing off needs ImGuiConfigFlags_ViewportsEnable, which is NOT set here and
// which would need this engine's own platform layer to grow multi-window support. The architecture
// below does not change when that lands -- an editor draws into a window it does not own, so making
// that window a real OS one is a flag and a backend, not a redesign.
//
// WHAT AN EDITOR OWNS: its asset, its dirty flag, its own panels. What it does NOT own: the device,
// the project, or anything about the level. Those are passed in, so an editor cannot quietly become
// a second place that knows how to load a world.
#include "aver/core/Types.hpp"

#include <memory>
#include <string>
#include <vector>

namespace aver {

class Engine;

namespace editor {

// One open asset editor. Implementations live beside this file, one per asset family.
class AssetEditor {
public:
    virtual ~AssetEditor() = default;

    // The absolute path of the asset being edited. Identity: the host refuses to open the same path
    // twice and focuses the existing editor instead, which is what every editor with tabs does.
    virtual const std::string& path() const = 0;

    // What the tab says. Includes the dirty marker; the host does not decorate it, because only the
    // editor knows whether an asterisk is meaningful for its asset type.
    virtual std::string title() const = 0;

    // Unsaved changes. The host asks before closing, and asks again before the application exits.
    virtual bool dirty() const { return false; }

    // Draw the editor's contents. Called with a window already begun by the host, so an editor is a
    // set of panels rather than a window -- which is what lets the host decide tabs versus windows
    // without every editor caring.
    virtual void draw(Engine& e) = 0;

    // Write the asset back. Returns false and leaves `why` set if it could not.
    virtual bool save(std::string* why) { (void)why; return true; }
};

// Creates an editor for a path, or nullptr if this factory does not handle it. Registered by the
// host; the FIRST factory that accepts a path wins, so ordering is registration order.
using AssetEditorFactory = std::unique_ptr<AssetEditor> (*)(const std::string& path);

// Owns every open editor and decides what a double-click does.
class AssetEditorHost {
public:
    void registerFactory(AssetEditorFactory f) { factories_.push_back(f); }

    // Open (or focus) an editor for `path`. Returns false when no registered factory handles it,
    // which is the caller's cue to fall back to the shell -- opening a .txt in a mesh editor would
    // be worse than opening it in Notepad.
    bool open(const std::string& path);

    // Draw every open editor. Returns true if any is open, so the caller can tell whether the level
    // viewport should still be taking input.
    // `dockInto` is the dock node a NEWLY opened editor becomes a tab in -- the editor's central
    // region, the same area the level viewport occupies. FirstUseEver, so it is a starting position
    // and not a cage: drag the tab out and it stays out.
    //
    // Passed per frame rather than stored because editor windows do not exist at layout time (their
    // ImGui names are built from their paths), so DockBuilderDockWindow cannot name them and the
    // dock has to happen as each one first appears.
    //
    // `dpi` scales the fallback size for an UNDOCKED window. Without it the default is raw pixels:
    // 720x520 on a 300% display is a window barely a fifth of the screen with its own toolbar
    // clipped, which is exactly how this first shipped.
    bool draw(Engine& e, unsigned dockInto = 0, float dpi = 1.0f);

    // Whether anything is open at all. The level's viewport overlay -- the Perspective/Lit/Show
    // bar and the gizmo toolbar -- is hidden while an editor covers the central region, because it
    // is drawn later and would otherwise sit on top of the tab's own toolbar. Which it did.
    bool anyOpen() const { return !editors_.empty(); }

    bool anyDirty() const;
    usize count() const { return editors_.size(); }

private:
    std::vector<AssetEditorFactory> factories_;
    std::vector<std::unique_ptr<AssetEditor>> editors_;
    std::string focusRequest_;          // path to bring forward on the next draw
    std::vector<usize> closing_;        // deferred: an editor must not be destroyed mid-draw
};

// The first concrete editor: a .ocmesh inspector.
//
// Chosen deliberately as the one that proves the host. It is small, it needs nothing that does not
// already exist, and it is immediately useful -- until now a .ocmesh was a file you could import and
// never look at. A 3D preview needs a render target and a second camera, which is real work and is
// not what the host needs proving.
std::unique_ptr<AssetEditor> makeMeshEditor(const std::string& path);

} // namespace editor
} // namespace aver
