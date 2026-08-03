#pragma once
// Asset editors: the interface one open asset implements, the host that owns them all, and the
// factory for the first concrete one.
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

    // The absolute path of the asset being edited. This is the editor's identity to the host.
    virtual const std::string& path() const = 0;

    // What the tab says, including any dirty marker.
    virtual std::string title() const = 0;

    // True when there are unsaved changes.
    virtual bool dirty() const { return false; }

    // Draws the editor's contents into a window the host has already begun.
    virtual void draw(Engine& e) = 0;

    // Writes the asset back. Returns false and sets `why` if it could not.
    virtual bool save(std::string* why) { (void)why; return true; }

    // The host saw this editor's asset change on disk. Called on the main thread between frames.
    virtual void onFileChanged() {}

    // The watcher overflowed and cannot say what changed; assume the asset is stale.
    virtual void onWatchLost() { onFileChanged(); }
};

// Creates an editor for a path, or nullptr if this factory does not handle it. First match wins.
using AssetEditorFactory = std::unique_ptr<AssetEditor> (*)(const std::string& path);

// Owns every open editor and decides what a double-click does.
class AssetEditorHost {
public:
    void registerFactory(AssetEditorFactory f) { factories_.push_back(f); }

    // Opens or focuses an editor for `path`. False when no factory handles it, so the caller can
    // fall back to the shell.
    bool open(const std::string& path);

    // Draws every open editor, docking a newly opened one into `dockInto`. Returns true if any is
    // open. `dpi` scales the fallback size of an undocked window.
    bool draw(Engine& e, unsigned dockInto = 0, float dpi = 1.0f);

    // True when any editor is open.
    bool anyOpen() const { return !editors_.empty(); }

    bool anyDirty() const;
    usize count() const { return editors_.size(); }

    // The titles of every editor with unsaved changes, so a prompt can NAME them. "You have unsaved
    // changes" without saying which file is a dialog the user cannot act on.
    std::vector<std::string> dirtyTitles() const;

    // Saves every dirty editor. Returns how many FAILED, appending each reason to `why`. Carries on
    // after a failure rather than stopping at the first: a user who asked to save everything wants
    // the nine that can be saved written, not eight of them abandoned because the tenth is
    // read-only.
    usize saveAllDirty(std::string* why);

    // Routes a disk change to whichever editor owns that path. Returns true if one did.
    bool notifyFileChanged(const std::string& path);

    // Tells every open editor the watcher overflowed.
    void notifyWatchLost();

private:
    std::vector<AssetEditorFactory> factories_;
    std::vector<std::unique_ptr<AssetEditor>> editors_;
    std::string focusRequest_;          // path to bring forward on the next draw
    std::vector<usize> closing_;        // deferred: an editor must not be destroyed mid-draw
};

// Creates the .ocmesh inspector.
std::unique_ptr<AssetEditor> makeMeshEditor(const std::string& path);

} // namespace editor
} // namespace aver
