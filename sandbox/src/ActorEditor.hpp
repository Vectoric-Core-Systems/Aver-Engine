#pragma once
// The actor editor tab: a `.Designer.cs` opened as an asset, showing the placements it declares and
// writing edits back to that source.
#include "AssetEditor.hpp"

#include <functional>
#include <memory>
#include <string>

namespace aver { class Engine; namespace render::preview { class ActorPreview; class PreviewMeshCache; } }

namespace aver::editor {

// The bare tab title, with no manual dirty marker appended: the host (AssetEditorHost::draw, in
// AssetEditor.cpp) already applies ImGuiWindowFlags_UnsavedDocument whenever an editor's dirty() is
// true, so a title that appends its own "*" on top doubles the marker. SoundEditor::title() and
// BtEditor::title() document this exact rule with a comment instead of a shared function; this is
// the same rule for ActorEditor, pulled out as a free function for one reason: ActorEditor itself is
// a file-local class (defined inside ActorEditor.cpp, reachable only through the AssetEditor
// interface returned by makeActorEditor), and its dirty_ flag is private with no public setter --
// it is only ever set true from mouse-driven ImGui gizmo dragging inside draw(), so no headless
// caller can force it true to prove "regardless of dirty()" against the class directly. `dirty` is
// accepted and deliberately ignored (not removed) so the signature states the invariant it enforces,
// and ActorEditorTitleTest asserts exactly that against this function, which is the one
// ActorEditor::title() actually calls.
inline std::string actorTabTitle(const std::string& baseTitle, bool /*dirty*/) { return baseTitle; }

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

// The content root last set, so another tab resolving mesh paths through sharedPreviewMeshes()
// uses the SAME root rather than deriving its own from an asset path -- a graph in
// Content/Scripts and a designer file elsewhere would otherwise disagree about where Content is.
const std::string& actorEditorContentRoot();

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
