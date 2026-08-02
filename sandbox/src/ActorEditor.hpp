#pragma once
// The actor editor tab: a `.Designer.cs` opened as an asset, showing the placements it declares and
// writing edits back to that source.
#include "AssetEditor.hpp"

#include <functional>
#include <memory>
#include <string>

namespace aver { class Engine; namespace render::preview { class ActorPreview; class PreviewMeshCache; } }

namespace aver::editor {

// THE ONE PREVIEW FEATURE EVERY ASSET TAB SHARES, created lazily by whichever tab draws first.
//
// One, not one each: a second feature would be registered alongside this one in the device's list
// and both would draw every frame, into each other's target. Only the ACTIVE dock tab draws, so
// whichever tab is in front owns the draw list for that frame and there is nothing to arbitrate.
// Null when the backend has no GPU support, which is how a tab degrades to numbers only.
render::preview::ActorPreview* sharedPreview(Engine& e);
// The mesh registry that preview shares, for the same reason.
render::preview::PreviewMeshCache& sharedPreviewMeshes();

// Creates an actor editor for a `.Designer.cs` that carries a generated region, else nullptr.
std::unique_ptr<AssetEditor> makeActorEditor(const std::string& path);

// Sets the project's content root, which mesh paths in a designer file are relative to.
void setActorEditorContentRoot(std::string root);

// What the tab's toolbar does. Any hook may be left empty; its button is then disabled.
struct ActorEditorHooks {
    std::function<void()> compileScripts;              // Tools > Compile .NET
    std::function<void(const std::string&)> openInIde; // the project's chosen IDE
    std::function<bool()> compileBusy;                 // true while a build is running
    std::string ideName;                               // for the button's label

    // Draws the app's own Compile .NET split button at the current cursor. Empty falls back to a
    // plain button.
    std::function<void()> drawCompileButton;
};
// Installs the toolbar hooks every actor tab uses.
void setActorEditorHooks(ActorEditorHooks hooks);

// Bumps the reload generation, telling every actor tab its Live view was built by dead code.
void notifyActorEditorsScriptsReloaded();

// Opens every new actor tab with the Live view already on.
void setActorEditorLiveByDefault(bool on);

// Restores the tab's columns to their defaults and persists that.
void resetActorEditorLayout();

// Releases the shared preview and its meshes. Called before the device goes.
void shutdownActorEditors();

} // namespace aver::editor
