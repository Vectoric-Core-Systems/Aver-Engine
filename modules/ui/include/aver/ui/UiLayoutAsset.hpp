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
#include "aver/ui/UiLayout.hpp"
#include "aver/ui/UiTree.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::ui {

inline constexpr i32 kUiLayoutVersion = 1;

struct UiLayoutNode {
    UiWidgetProps props;
    i32 parent = -1;   // index of the parent node; -1 for the root
};

struct UiLayoutDoc {
    i32 version = kUiLayoutVersion;
    std::string name;
    std::string theme = "dark";
    UiDpi dpi{};
    std::vector<UiLayoutNode> nodes;   // nodes[0] is the single root; a parent precedes its children

    // One root, parents before children, unique non-empty names.
    bool valid(std::string* why = nullptr) const;
};

// Parses a layout. False with `err` set (including the line number) on a malformed document; never
// a half-loaded one.
bool uiParseLayout(std::string_view text, UiLayoutDoc& out, std::string* err = nullptr);

// Serialises. parse(write(doc)) reproduces doc, and write(parse(write(doc))) == write(doc).
std::string uiWriteLayout(const UiLayoutDoc& doc);

// Creates the widgets under `parent` (0 = as a root) and returns the root widget id, 0 for an
// invalid document. The tree's theme and DPI are not touched.
UiWidgetId uiInstantiateLayout(const UiLayoutDoc& doc, UiTree& tree, UiWidgetId parent = 0);

// Captures the subtree at `root` as a document (names made unique). False for an unknown root.
bool uiCaptureLayout(const UiTree& tree, UiWidgetId root, UiLayoutDoc& out);

// A small valid layout: a centred panel with a title and three buttons.
UiLayoutDoc uiStarterLayout();

// The properties a newly added widget of `kind` starts with (sensible size and sample text).
UiWidgetProps uiNewWidgetProps(UiWidgetKind kind);

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
// Renames a node; false when the name is empty or taken.
bool uiLayoutRename(UiLayoutDoc& doc, i32 index, std::string_view name);

const char* uiScaleModeName(UiScaleMode m);
bool uiScaleModeFromName(std::string_view s, UiScaleMode& out);

} // namespace aver::ui
