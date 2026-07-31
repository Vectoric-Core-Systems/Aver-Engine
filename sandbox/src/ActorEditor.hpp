#pragma once
// The actor editor tab: a `.Designer.cs` opened as an asset, showing the placements it declares and
// writing edits back to that source.
#include "AssetEditor.hpp"

#include <functional>
#include <memory>
#include <string>

namespace aver::editor {

// Creates an actor editor for a `.Designer.cs` that carries a generated region, else nullptr.
std::unique_ptr<AssetEditor> makeActorEditor(const std::string& path);

// Sets the project's content root, which mesh paths in a designer file are relative to.
void setActorEditorContentRoot(std::string root);

// What the tab's toolbar does. Any hook may be left empty; its button is then disabled.
struct ActorEditorHooks {
    std::function<void()> compileScripts;              // Tools > Compile C#
    std::function<void(const std::string&)> openInIde; // the project's chosen IDE
    std::function<bool()> compileBusy;                 // true while a build is running
    std::string ideName;                               // for the button's label

    // Draws the app's own Compile C# split button at the current cursor. Empty falls back to a
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
