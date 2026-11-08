#pragma once
// The UI layout asset (.ocui): a widget hierarchy as data. Text, the same OC dialect as the other
// formats; parse from memory and write to a string (this module does no file I/O, like UiFont).
//
//   OCUI 1
//   NAME MainMenu
//   THEME dark
//   SCALE height 1920 1080 1
//   WIDGET Root panel -
//   SET Root anchors 0 0 1 1
//   WIDGET PlayButton button Root
//   SET PlayButton text "Play"
//
// Only properties that differ from the widget kind's defaults are written. Unknown records and
// unknown SET keys are skipped, so a file written by a newer build still loads.
//
// Version 2 (docs/formats/FORMAT_SPECS.md) adds records for nested layouts and the later UI runtime. A
// document that uses none of them is written as version 1, byte for byte as before; a v1 file is read
// exactly as before (its v2 records, if any, stay unknown records).
//
//   USE Row1 "UI/Row.ocui" Root            a nested layout (a Panel node with `use`)
//   SET Row1 anchors 0 0 1 0                 ... placement of the instance, like any widget
//   OVERRIDE Row1 Title "Hello"              an exposed property of the used layout
//   EXPOSE Title Label text                  (in the used layout) name -> widget key
//   SLOT Content Body                        (in the used layout) name -> widget that takes the fills
//   ANIM / BIND / EVENT                      animations, bindings and event routes (UiAnimation.hpp, UiBindings.hpp); attached to the root by uiInstantiateLayoutEx
#include "aver/ui/UiLayout.hpp"
#include "aver/ui/UiTree.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aver::ui {

inline constexpr i32 kUiLayoutVersion = 1;      // what a document with no v2 records carries
inline constexpr i32 kUiLayoutVersionMax = 2;   // the newest this build reads

// A nested layout reference. The node's own props are the instance's placement (anchors, size, margin, ...) and are
// laid over the used layout's root; its children are slot fills (child `slot` names a SLOT of the used layout; no
// slot = appended to the instance root).
struct UiLayoutUse {
    struct Override {
        std::string name;                  // an EXPOSE name of the used layout
        std::vector<std::string> values;   // the property's value tokens
    };
    std::string path;                      // as the host resolves it (the editor: relative to the content root)
    std::vector<Override> overrides;
};

struct UiLayoutNode {
    UiWidgetProps props;
    i32 parent = -1;   // index of the parent node; -1 for the root
    std::optional<UiLayoutUse> use;   // set: a USE record, a Panel standing for the whole used layout
};

// EXPOSE <name> <widget> <key>: lets a layout that USEs this one set `key` of `widget` by `name`.
struct UiLayoutExpose { std::string name, widget, key; };
// SLOT <name> <widget>: where the children of a USE of this layout go.
struct UiLayoutSlot { std::string name, widget; };
// ANIM <name> <widget> <property> (<time> <value>)*: one track; tracks of one name make an animation.
struct UiLayoutAnim { std::string name, widget, property; std::vector<std::string> keys; };
// BIND <widget> <property> <path> [mode...]
struct UiLayoutBind { std::string widget, property, path; std::vector<std::string> rest; };
// EVENT <widget> <event> <action> <target> [args...]
struct UiLayoutEvent { std::string widget, event, action, target; std::vector<std::string> rest; };

struct UiLayoutDoc {
    i32 version = kUiLayoutVersion;
    std::string name;
    std::string theme = "dark";
    UiDpi dpi{};
    std::vector<UiLayoutNode> nodes;   // nodes[0] is the single root; a parent precedes its children
    std::vector<UiLayoutExpose> exposed;
    std::vector<UiLayoutSlot> slots;
    std::vector<UiLayoutAnim> anims;
    std::vector<UiLayoutBind> binds;
    std::vector<UiLayoutEvent> events;

    // One root, parents before children, unique non-empty names; EXPOSE and SLOT name widgets that exist; a USE has a path.
    bool valid(std::string* why = nullptr) const;
    // True when the document needs the v2 format (a USE, EXPOSE, SLOT, ANIM, BIND or EVENT).
    bool usesV2() const;
};

// Parses a layout. False with `err` set (including the line number) on a malformed document; never
// a half-loaded one.
bool uiParseLayout(std::string_view text, UiLayoutDoc& out, std::string* err = nullptr);

// Serialises. parse(write(doc)) reproduces doc, and write(parse(write(doc))) == write(doc).
std::string uiWriteLayout(const UiLayoutDoc& doc);

// Creates the widgets under `parent` (0 = as a root) and returns the root widget id, 0 for an
// invalid document. The tree's theme and DPI are not touched. A USE in the document becomes a placeholder
// (no loader here); use uiInstantiateLayoutEx to load them.
UiWidgetId uiInstantiateLayout(const UiLayoutDoc& doc, UiTree& tree, UiWidgetId parent = 0);

// Reads the text of a layout a USE names. False when it cannot be found. The module does no file I/O: the host decides.
using UiLayoutLoader = std::function<bool(const std::string& path, std::string& text)>;

struct UiInstantiateResult {
    UiWidgetId root = 0;                // 0: invalid document, a cycle, or too deep
    std::vector<UiWidgetId> nodeIds;    // per document node: its widget (the instance root for a USE)
    std::vector<std::string> errors;    // everything that went wrong, including what did not stop the build
};
// Like uiInstantiateLayout, with nested layouts: each USE loads its file, applies the OVERRIDEs through the used
// layout's EXPOSEs, puts the node's children in the used layout's SLOTs, and names the used widgets "<instance>/<name>".
// A missing or unreadable file leaves a visible placeholder and an error, the rest of the layout still builds. A cycle
// (A uses B uses A) or more than 16 levels fails the whole instantiate with an error naming the chain.
// `selfPath` is this document's own path, so a layout that uses itself is caught at once.
UiInstantiateResult uiInstantiateLayoutEx(const UiLayoutDoc& doc, UiTree& tree, UiWidgetId parent,
                                          const UiLayoutLoader& loader, std::string_view selfPath = {});

// Walks the USE graph without building anything. False with the cycle chain in `err` when one exists; a file that
// cannot be loaded is not a cycle (and is skipped).
bool uiLayoutCheckUses(const UiLayoutDoc& doc, std::string_view selfPath, const UiLayoutLoader& loader, std::string* err = nullptr);

// An exposed property of a used layout, with the value the instance has now.
struct UiExposedProp {
    std::string name, widget, key;
    std::vector<std::string> value;
    bool overridden = false;
};
// The EXPOSEs of the layout the USE node `index` names. False (and `err`) when the node is not a USE or its file fails.
bool uiLayoutExposedProps(const UiLayoutDoc& doc, i32 index, const UiLayoutLoader& loader,
                          std::vector<UiExposedProp>& out, std::string* err = nullptr);

// Captures the subtree at `root` as a document (names made unique). False for an unknown root.
bool uiCaptureLayout(const UiTree& tree, UiWidgetId root, UiLayoutDoc& out);

// A small valid layout: a centred panel with a title and three buttons.
UiLayoutDoc uiStarterLayout();

// The properties a newly added widget of `kind` starts with (sensible size and sample text).
UiWidgetProps uiNewWidgetProps(UiWidgetKind kind);

// Palette presets named like UMG's panels. Each is a plain Panel with a layout mode and properties set: there
// is no kind per container (Overlay = layout overlay, Size Box = overlay with width/height, Scale Box =
// overlay with scaleFit, Border = a padded panel with a border, Spacer = an empty panel that takes no input,
// Widget Switcher = layout switcher).
struct UiWidgetPreset {
    const char* label;      // as the palette shows it
    const char* name;       // the widget name it starts with
    UiWidgetProps props;
};
const std::vector<UiWidgetPreset>& uiWidgetPresets();
// Adds a preset under `parent`, last among its siblings. Returns the new index, -1 for a bad parent or preset.
i32 uiLayoutAddPreset(UiLayoutDoc& doc, i32 parent, usize preset, std::string_view name = {});

// ---- structural edits (the editor's operations; all keep valid() true) -----------------------------
// Indices move across these calls; each returns the new index of what it acted on, or -1.

std::vector<i32> uiLayoutChildren(const UiLayoutDoc& doc, i32 parent);
i32 uiLayoutFind(const UiLayoutDoc& doc, std::string_view name);
std::string uiUniqueWidgetName(const UiLayoutDoc& doc, std::string_view base);
// Adds a child of `kind` under `parent`, last among its siblings.
i32 uiLayoutAdd(UiLayoutDoc& doc, i32 parent, UiWidgetKind kind, std::string_view name = {});
// Removes the node and its subtree. Returns the parent's new index; -1 for the root or a bad index.
i32 uiLayoutDelete(UiLayoutDoc& doc, i32 index);
// Moves under `newParent` at `position` among its children (-1 = last). -1 on a cycle or the root.
i32 uiLayoutReparent(UiLayoutDoc& doc, i32 index, i32 newParent, i32 position = -1);
// Moves one place earlier (delta < 0) or later among its siblings. -1 at the end or for the root.
i32 uiLayoutMoveSibling(UiLayoutDoc& doc, i32 index, i32 delta);
// Deep-copies the subtree right after the original, with unique names. Returns the copy's root.
i32 uiLayoutDuplicate(UiLayoutDoc& doc, i32 index);
// Renames a node; false when the name is empty or taken. EXPOSE, SLOT, ANIM, BIND and EVENT records follow the rename.
bool uiLayoutRename(UiLayoutDoc& doc, i32 index, std::string_view name);
// Adds a USE node of `path` under `parent` (named after the file when `name` is empty). -1 for a bad parent or empty path.
i32 uiLayoutAddUse(UiLayoutDoc& doc, i32 parent, std::string_view path, std::string_view name = {});
// Sets (or replaces) the OVERRIDE `name` of a USE node. False when the node is not a USE.
bool uiLayoutSetOverride(UiLayoutDoc& doc, i32 index, std::string_view name, std::vector<std::string> values);
bool uiLayoutClearOverride(UiLayoutDoc& doc, i32 index, std::string_view name);

const char* uiScaleModeName(UiScaleMode m);
bool uiScaleModeFromName(std::string_view s, UiScaleMode& out);

} // namespace aver::ui
