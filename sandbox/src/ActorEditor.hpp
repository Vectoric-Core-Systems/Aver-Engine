#pragma once
// The actor editor tab: a `.Designer.cs` opened as an asset rather than as text.
//
// It shows what the source DECLARES -- the placements, in local space, drawn by
// Aver.Render.ActorPreview -- and writes edits back to that source through
// fmt::rewriteActorScript. The picture and the file are the same data, so there is no third state
// to keep in step.
#include "AssetEditor.hpp"

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

// Releases the shared preview and its meshes. Called before the device goes.
void shutdownActorEditors();

} // namespace aver::editor
