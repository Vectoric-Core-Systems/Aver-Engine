#pragma once
// The Input Scheme editor tab: a .ocinput opened as an asset -- its declared actions, their key/
// mouse/gamepad bindings, and the CONTEXT name + priority EnhancedInput.AddContext takes. See
// aver/formats/OcInput.hpp for the format itself.
//
// THE HOOK INTO SandboxApp.cpp IS THREE LINES, matching FoliageTypeEditor.hpp before it:
//   #include "InputSchemeEditor.hpp"
//   assetEditors_.registerFactory(&editor::makeInputSchemeEditor);   // APPENDED -- order is precedence
// Unconditional, like FoliageTypeEditor's own registration: OcInput.hpp needs neither Aver.Scene nor
// Aver.Landscape (its own header comment: "NO ENGINE DEPENDENCY, DELIBERATELY"), so this tab is not
// gated behind any optional module either.
//
// MODELLED ON FoliageTypeEditor: SnapshotUndo<State> from the start, State IS the whole record
// (fmt::OcInputData already carries actions/bindings/context -- nothing is held alongside it, the
// same reason BtEditor/SoundEditor's own State is their format's POD directly rather than a
// {record, extras} pair). Save/dirty/Ctrl+Z/Ctrl+Y follow the identical shape.
//
// UNLIKE FoliageTypeEditor, THIS FORMAT IS TEXT-MERGED ON SAVE (fmt::writeOcinput(data_,
// originalText_), GraphEditor's own convention) rather than round-tripped through a plain
// save*() POD writer: .ocinput is meant to be hand-editable (OcInput.hpp's own worked example),
// so a comment an author left in the file must survive a save made entirely from this UI.
#include "AssetEditor.hpp"
#include "SnapshotUndo.hpp"

#include "aver/formats/OcInput.hpp"

#include <functional>
#include <memory>
#include <string>

namespace aver::editor {

// What the tab needs from SandboxApp to answer "is this the project's Input Scheme?" and to make it
// one. No AssetEditorHost/host-context hook already reaches an open tab (AssetEditorFactory is a
// bare `unique_ptr<AssetEditor>(*)(const std::string&)`, and AssetEditor itself declares nothing
// project-aware) -- so this follows ActorEditorHooks' own precedent (ActorEditor.hpp) instead: a
// small struct of std::function callbacks, installed once by SandboxApp and read by every open tab
// through a file-local global (InputSchemeEditor.cpp), the narrowest hook that does not require
// AssetEditor's interface itself to grow a project pointer every other tab would have to ignore.
struct InputSchemeEditorHooks {
    // project_.contentDir(): the absolute folder every content-relative path resolves against, or
    // empty when no project is open (draw() then shows nothing about project scheme status at all).
    std::function<std::string()> contentDir;
    // project_.inputScheme -- the project's current INPUT.SCHEME, content-relative. Empty means unset.
    std::function<std::string()> projectInputScheme;
    // Sets project_.inputScheme to `contentRelativePath` and marks the manifest dirty -- the SAME
    // projectDirty_ flag Project Settings > Description's own fields set (SandboxSettings.cpp); the
    // actual write happens on the existing autosave timer or the Save button there, not here. False
    // when there is no hook installed or no project open.
    std::function<bool(const std::string& contentRelativePath)> useAsProjectInputScheme;
};
// Installs the hooks every Input Scheme tab uses. Call once, the way setActorEditorHooks is.
void setInputSchemeEditorHooks(InputSchemeEditorHooks hooks);

class InputSchemeEditor final : public AssetEditor {
public:
    explicit InputSchemeEditor(std::string path);

    const std::string& path() const override { return path_; }
    std::string title() const override;
    bool dirty() const override { return dirty_; }
    void draw(Engine& e) override;
    bool save(std::string* why) override;
    void onFileChanged() override;

    // Reachable for a headless test, matching FoliageTypeEditor/BtEditor's own reason.
    bool loaded() const { return loaded_; }
    const std::string& loadError() const { return loadError_; }
    const fmt::OcInputData& data() const { return data_; }
    void markDirty() { dirty_ = true; }

    // Snapshot undo, through the shared SnapshotUndo<State> template -- see the header comment above.
    void pushUndo();
    void undo();
    void redo();
    bool canUndo() const { return history_.canUndo(); }
    bool canRedo() const { return history_.canRedo(); }

    // ---- actions ----
    // Appends a new Digital action named "NewAction" (or "NewAction1", "NewAction2", ... the first
    // free one) -- a duplicate name is not itself invalid (OcInput.hpp: "TWO ACTIONS SHARING A NAME
    // IS NOT CHECKED HERE"), but a freshly added row showing a name already used two rows up reads
    // like a bug even where the format does not mind it.
    void addAction();
    // Removes the action at `index`, AND every binding that names it: a binding left naming a
    // now-gone action is exactly the "BIND names action ..., which this file never declares" error
    // parseOcinput refuses to load (OcInput.cpp) -- this tab must never be able to save a file its
    // own Save button could not then reopen.
    void removeAction(usize index);
    // Renames the action at `index` and repoints every binding that named its OLD name -- a binding
    // matches an action by name, not by index (OcInputBinding::action's own comment), so a rename
    // that did not cascade would silently orphan every binding under the old spelling.
    void renameAction(usize index, std::string newName);
    void setActionType(usize index, fmt::OcInputValueType type);

    // ---- bindings ----
    // Appends a binding on the FIRST declared action (a no-op with none declared -- there is nothing
    // for a fresh binding to feed).
    void addBinding();
    void removeBinding(usize index);
    void setBindingAction(usize index, std::string actionName);
    // Switching source also seeds `key` from that source's own vocabulary the moment it becomes
    // Key/GamepadButton/GamepadAxis-shaped and `key` is empty -- see the .cpp for why an empty key
    // on one of those three sources is a file parseOcinput would refuse to reload.
    void setBindingSource(usize index, fmt::OcInputSource source);
    void setBindingKey(usize index, std::string key);
    void setBindingScale(usize index, f32 scale);
    void setBindingComponent(usize index, i32 component);

    // ---- context ----
    void setContextName(std::string name);
    void setContextPriority(i32 priority);

private:
    void loadFromDisk();

    std::string path_;
    fmt::OcInputData data_;
    // The file's own text as last loaded/saved -- fmt::writeOcinput's `existing` merge argument, so
    // a comment or a record this format does not model survives a save made entirely from this UI
    // (see this header's own top comment). Empty for a file that does not exist yet, which is fine:
    // writeOcinput(data_, "") is its documented fresh-file path.
    std::string originalText_;
    bool loaded_ = false;
    std::string loadError_;
    bool dirty_ = false;

    // Through the shared SnapshotUndo<State> template (SnapshotUndo.hpp). State IS the whole record,
    // matching FoliageTypeEditor/BtEditor/SoundEditor's own shape.
    SnapshotUndo<fmt::OcInputData> history_;

#if AVER_WITH_IMGUI
    void drawContext();
    void drawActions();
    void drawBindings();
    // The "this is not the project's Input Scheme" line and its "Use as Project Input Scheme"
    // button, through InputSchemeEditorHooks above. A no-op when no hooks are installed or no
    // project is open.
    void drawProjectSchemeLine();
#endif
};

// Creates an Input Scheme editor for a .ocinput, else nullptr.
std::unique_ptr<AssetEditor> makeInputSchemeEditor(const std::string& path);

// The starter scheme the Content Browser's "New Input Scheme" writes: one digital Jump action bound
// to the Space key (Input.cs's own Key.Space -- InputKeyNames.hpp's kFwKeyNames), in a context
// named "NewContext" at priority 0 -- immediately meaningful rather than an empty file with nothing
// to demonstrate the grammar. Declared here so a test can check exactly what it writes, matching
// foliageStarterType's own precedent (FoliageTypeEditor.hpp).
inline fmt::OcInputData inputSchemeStarterData() {
    fmt::OcInputData d;
    fmt::OcInputAction jump;
    jump.name = "Jump";
    jump.type = fmt::OcInputValueType::Digital;
    d.actions.push_back(jump);

    fmt::OcInputBinding bind;
    bind.action = "Jump";
    bind.source = fmt::OcInputSource::Key;
    bind.key = "Space";
    d.bindings.push_back(bind);

    d.contextName = "NewContext";
    d.contextPriority = 0;
    return d;
}

} // namespace aver::editor
