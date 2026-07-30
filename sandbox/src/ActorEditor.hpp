#pragma once
// The actor editor tab: a `.Designer.cs` opened as an asset rather than as text.
//
// It shows what the source DECLARES -- the placements, in local space, drawn by
// Aver.Render.ActorPreview -- and writes edits back to that source through
// fmt::rewriteActorScript. The picture and the file are the same data, so there is no third state
// to keep in step.
#include "AssetEditor.hpp"

#include <functional>
#include <memory>
#include <string>

namespace aver::editor {

// The factory, registered alongside the mesh editor's. Accepts a `.Designer.cs` that actually
// carries a generated region; a `.cs` without one is not an actor and belongs in the IDE.
std::unique_ptr<AssetEditor> makeActorEditor(const std::string& path);

// The project's content root, which mesh paths in a designer file are relative to.
//
// Pushed rather than pulled because AssetEditorFactory is a bare function pointer with nowhere to
// carry context, and widening it for one editor would change every other. Set when a project opens;
// clearing it leaves the tab drawing an empty preview and saying why, which is better than an editor
// that cannot open at all.
void setActorEditorContentRoot(std::string root);

// What the tab's toolbar does. Installed by the app, because the tab must not reach into the editor
// to find a compile job or an IDE choice -- those belong to the app, and an asset editor that knew
// about them could not be tested or reused.
//
// Either may be left empty: the corresponding button is then disabled and says why, which is better
// than a button that silently does nothing.
struct ActorEditorHooks {
    std::function<void()> compileScripts;              // Tools > Compile C#
    std::function<void(const std::string&)> openInIde; // the project's chosen IDE
    std::function<bool()> compileBusy;                 // true while a build is running
    std::string ideName;                               // for the button's label

    // Draws the app's OWN Compile C# split button -- status icon, label, and the dropdown carrying
    // Reload / Auto-compile on Save / Open Scripts In -- at the current cursor.
    //
    // A hook rather than this tab drawing its own, because the tab previously had a plain
    // `ImGui::Button("Compile C#")`: same words, no status icon, no dropdown, and a separate idea of
    // when it should be disabled. Two buttons that claim to do the same thing and look different is
    // worse than either. The tab still knows nothing about ToolsMenu -- it calls a std::function and
    // the app decides what that means, which is the same arrangement as `compileScripts`.
    //
    // If empty, the tab falls back to the plain button, so a build or a test that installs no hooks
    // still gets something that works.
    std::function<void()> drawCompileButton;
};
void setActorEditorHooks(ActorEditorHooks hooks);

// The scripting host has swapped the script assembly: every loaded class is a NEW type, and every
// Live view now holds a snapshot built by code that is no longer running.
//
// A GENERATION rather than a flag, and rather than a callback per tab. The app does not know which
// tabs exist -- deliberately, the asset-editor host owns that -- and a bare flag would be consumed
// by whichever tab drew first, leaving every other tab stale with nothing to tell it. A counter
// each tab compares against its own last-seen value is read-only for the tabs, correct for any
// number of them, and costs one integer compare per frame.
void notifyActorEditorsScriptsReloaded();

// Opens every new actor tab with the LIVE view already on.
//
// Exists for the headless run, and that is the honest reason: Live is a checkbox nobody can tick
// without a window, so without this the one path that spawns a real class could only ever be checked
// by a human looking at a screen. `--actor-live` sets it, `--open-asset` opens the tab, and the log
// line says how many models the class actually built.
void setActorEditorLiveByDefault(bool on);

// Restore the tab's columns to their defaults, and persist that. Called by Window > Reset Layout
// when an actor tab is the active one.
void resetActorEditorLayout();

// Releases the shared preview and its meshes. Called before the device goes.
void shutdownActorEditors();

} // namespace aver::editor
