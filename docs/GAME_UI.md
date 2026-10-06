# Game UI: widgets, menus and the layout editor

Status: built in the `feat/game-systems` worktree. Every C++ file was syntax-checked with `cl /Zs` (the editor tab
both with and without ImGui) and every C# file compiled with `csc` into a scratch assembly, but **nothing was linked,
tested or run** (the integration step builds and runs it). Plan: [plans/GAME_SYSTEMS_PLAN.md](plans/GAME_SYSTEMS_PLAN.md), feature 6.

`Aver.UI` is the retained game UI. It is not Dear ImGui (that stays the editor's): a shipped game has no editor
toolkit, and a menu authored as data is something a designer can open, diff and hand to an artist. Everything
here sits on the existing draw list (`UiDrawList`, `UiFont`) and stays **Core-only**, so the whole system,
layout, input, focus, events and drawing, is tested with no GPU and no window.

## What exists

| Piece | Where |
|---|---|
| Widget tree, layout, input, events, drawing | `modules/ui` (`UiTree`, `UiLayout`, `UiFocus`, `UiStyle`, `UiText`) |
| Property table (one table feeds the file, the ABI and the inspector) | `modules/ui/UiProps.hpp` |
| `.ocui` layout asset: parse, write, instantiate, capture, structural edits | `modules/ui/UiLayoutAsset.hpp` |
| Settings screen, controls (rebinding) page, command menu | `modules/ui/UiScreens.hpp` |
| Keyboard and gamepad mapping with key repeat | `modules/ui/UiHostInput.hpp` |
| C ABI | `modules/ui.abi/ui_widget_abi.h` (beside the draw ABI `ui_abi.h`) |
| C# | `scripting/csharp/Aver.UI` (`Widgets.cs`, `SettingsScreen.cs`, `UiGraph.cs`), `Aver.Framework/UiGameSettings.cs`, `UiGraphInterop.cs` |
| Editor tab | `sandbox/src/UiLayoutEditor.{hpp,cpp}` |
| Tests | `tests/ui`: `UiWidgetTest`, `UiLayoutAssetTest`, `UiLayoutEditorTest` (and the existing `UiTest`) |

## The model

A `UiTree` owns widgets. A widget with no parent is a **root**; a root is one screen (a HUD, a menu, a dialog).
Roots draw back to front by `layer`, then `zOrder`, then creation order.

### Widget kinds

| Kind | Does | Events |
|---|---|---|
| `panel` | Container with a background and border (give it `style clear` for none). | none |
| `text` | A label; `wrap` word-wraps to its width. | none |
| `image` | A texture (`image` path, resolved by the host) or a tinted rectangle. | none |
| `button` | Click, or Accept when focused. | `Clicked`, then `Command` if `command` is set |
| `toggle` | Check box with a label. | `Toggled` |
| `slider` | Horizontal value; drag, or Left/Right when focused; `step` snaps. | `ValueChanged` |
| `choice` | Cycles `items` with click (left or right half) or Left/Right. | `ValueChanged` (`index`) |
| `list` | Scrolling rows of `items`; click selects and activates; Up/Down move. | `SelectionChanged`, `Clicked` |
| `scroll` | Scrolling container: wheel, scrollbar drag, scrolls to keep focus visible. | none |
| `textinput` | Single line; typed text, caret, Backspace/Delete/Home/End, `maxLength`, `masked`. | `TextChanged`, `TextSubmitted` |
| `progress` | A filled bar; `showValue` draws a percentage. | none |
| `keybind` | A button that listens for the next key, for rebinding. | `KeyCaptured` |

### Placement

Every widget has two ways to be placed, chosen by its **parent's** `layout`:

* **Parent `layout none`: anchors.** `anchors minX minY maxX maxY` are fractions of the parent's content area.
  If min equals max on an axis it is a *point anchor*: `offsets.left/top` is the position and `width/height` the
  size (auto = the content's size), with `pivot` (0..1) choosing which point of the widget sits there. If they
  differ the axis *stretches*: `offsets` are insets from the two anchor edges and the size is ignored.
* **Parent `layout vstack | hstack | grid`: flow.** `anchors` are ignored. Along the main axis children take
  their size, or share what is left by `fillW`/`fillH` weights (honouring `minWidth`..`maxHeight`); across it
  `alignH`/`alignV` is `stretch` (default), `start`, `center` or `end`. `margin` surrounds a child, `padding`
  insets the children, `spacing` separates them, `justify` places leftover space when nothing fills. A grid has
  `columns`, optional fixed `cellWidth`/`cellHeight`, and `spacingY` for rows.

Sizes are **design pixels**: authored against a reference resolution and scaled at runtime.

### DPI scaling

`SCALE <mode> <refWidth> <refHeight>` in the asset (default `height 1920 1080`). Modes: `constant` (the OS
display scale only), `width`, `height`, `shortest`, `blend` (geometric mean of the width and height ratios, so
ultrawide and portrait windows do not blow the UI up). Multiplied by the player's **user scale** (the
`graphics.uiScale` setting drives it) and clamped to 0.25..8. Fonts scale the same way: a font is baked at one
size and its glyphs are scaled. Offsets, sizes, margins, padding, spacing and borders all scale.

### Themes and styles

`dark` (default), `light`, `contrast`. A theme has one style per widget kind plus named variants a widget picks
with `style`: `title`, `heading`, `muted`, `danger`, `tab`, `clear`. Per-widget `bgColor`, `textColor`,
`accentColor` override the style (alpha-zero means "unset"). Hover, pressed and disabled looks derive from the
style. Padding in the style is *added* to the widget's own `padding`.

### Input routing (pausing game input while a menu has focus)

A root's `inputMode`:

* `passive` (default): never takes the pointer or the keyboard. A HUD.
* `blocking`: its interactive widgets take the pointer where they are; the game still gets everything else.
* `menu`: owns keyboard/gamepad navigation, and while visible **pauses game input** and wants the cursor.
  `modal` additionally dims the world and blocks every root beneath it.

After each frame the host reads `aver_ui_wants_input()` (bits: pointer, keyboard, pause, cursor). The standalone
runtime and Play mode clear their gameplay key/mouse/pad publishing while the pause bit is set. Closing the menu
hides its root (`ui.close`, Cancel, or code), which returns the input.

### Focus and navigation

Navigation is scoped to the topmost visible modal root, else the topmost visible `menu` root. Opening one focuses
its `defaultFocus` widget (by name) or the first focusable in tab order. Up/Down/Left/Right move focus spatially
(distance along the direction plus twice the sideways offset; overlapping on the cross axis counts as aligned),
wrapping at the edges. A widget's `navUp/navDown/navLeft/navRight` name overrides this. Tab/Shift+Tab (gamepad
shoulders) walk the **tab order**: positive `tabIndex` ascending, then `0` in document order, `-1` skipped.
Accept activates, Cancel raises `Cancel` and the root's `cancelCommand`. Sliders and choices consume
Left/Right; lists consume Up/Down until their ends; a listening key-bind swallows everything but Cancel.

### Events and commands

The tree queues events; a game reads them with `aver_ui_event_poll` (C), `UiSystem.Pump()` (C#) or a listener
(C++). A widget's **`command`** string raises a `Command` event when activated, so a layout can say what a
button means without code. Built-in commands run inside the tree: `ui.close` (hide the widget's root),
`ui.close:Name`, `ui.open:Name`, `ui.toggle:Name`.

## The `.ocui` asset

Text, the OC dialect: `#` comments, `KEY value` records, `"quoted strings"` with `\"` `\\` `\n`.

```
OCUI 1
NAME MainMenu
THEME dark
SCALE height 1920 1080

WIDGET Root panel -
SET Root anchors 0 0 1 1
SET Root style clear
SET Root inputMode menu
SET Root defaultFocus PlayButton

WIDGET Window panel Root
SET Window anchors 0.5 0.5
SET Window pivot 0.5 0.5
SET Window width 420
SET Window layout vstack
SET Window spacing 12

WIDGET PlayButton button Window
SET PlayButton text "Play"
SET PlayButton command play
```

* One root, parents before children, unique names. `WIDGET <name> <kind> <parent|->`.
* Only properties that differ from the kind's defaults are written, so files stay small and diffs readable.
* Unknown records and unknown `SET` keys are skipped (a newer build's file loads); a bad value, a missing
  parent, a duplicate name or a second root is an error naming the line.
* `write(parse(write(doc))) == write(doc)` is tested, floats print to the shortest text that reads back
  bit-exact.
* The module does no file I/O (like `UiFont`): the host reads the text.

### Property reference

Colours are `#RRGGBB` or `#RRGGBBAA`, or `none`. Insets/anchors/uv take 4 numbers (insets also 1 or 2, a point
anchor 2). Enum names are lower case.

* **Placement:** `anchors`, `offsets`, `pivot`, `width`, `height` (`-1` auto), `minWidth`, `minHeight`,
  `maxWidth`, `maxHeight` (`0` none), `margin`, `alignH`, `alignV`, `fillW`, `fillH`.
* **Layout:** `padding`, `layout` (`none vstack hstack grid`), `spacing`, `spacingY`, `columns`, `cellWidth`,
  `cellHeight`, `justify` (`start center end spacebetween`).
* **Appearance:** `visible`, `enabled`, `opacity`, `clip`, `hit` (`auto always never`), `style`, `bgColor`,
  `textColor`, `accentColor`.
* **Content:** `text`, `textAlign`, `fontSize`, `wrap`, `image`, `uv`, `imageMode` (`stretch fit`), `tint`,
  `placeholder`, `maxLength`, `masked`.
* **Value:** `value`, `minValue`, `maxValue`, `step`, `showValue`, `checked`, `selected`, `items`, `rowHeight`.
* **Navigation:** `focusable`, `tabIndex`, `navUp`, `navDown`, `navLeft`, `navRight`, `command`.
* **Root only:** `inputMode`, `modal`, `zOrder`, `layer`, `defaultFocus`, `cancelCommand`.

## Ready-made screens

`UiSettingsScreen` builds a tabbed settings screen from a `UiSettingsModel` (items of type toggle, slider,
choice, on named tabs) and, optionally, a `UiRebindModel` (rows: action, slot, label, current binding text) for a
**Controls** tab of key-bind buttons. `uiDefaultSettingsModel()` provides Graphics (quality, render scale, TAA,
vsync, fullscreen, UI scale) and Audio (master, music, effects, voice). The screen edits the model and raises
Command events; it never touches the renderer, the mixer or the input system:

* `setting:<key>` with `value` (and `index`) when a setting changes. `graphics.uiScale` is applied by the
  screen itself (it sets the tree's DPI user scale).
* `rebind:<action>:<slot>` with `index` = the captured key slot. The host rebinds and reports the new text with
  `aver_ui_rebind_set_binding`. `rebind.reset` asks for the defaults.
* Back and Cancel close it (`ui.close`).

`uiBuildMenu` makes a titled column of buttons (pause menu, main menu) whose `command`s the host handles.

In C#, `UiGameSettings.Create()` (Aver.Framework) wires the screen to the engine: values persist through
`Settings`, `audio.*` apply to `Audio` buses, `graphics.*` are handed to the `GraphicsChanged` event (what
"quality" means is the game's call), and the Controls tab rebinds through `EnhancedInput.RebindAction`
(`FrameworkRebindProvider.FromContext(context)` lists a context's bindings). Call `ApplyAll()` at startup to
restore saved values. Keyboard and mouse bindings are rebindable; gamepad bindings are listed greyed out.

## C ABI

`ui_widget_abi.h`. The host's per-frame protocol:

1. `aver_ui_begin_frame`, `aver_ui_set_font`, `aver_ui_set_pointer` (existing).
2. `aver_ui_input_host_frame(dt, keyPressed[256], keyHeld[256], vkToSlot[256], padConnected, padButtons[14],
   padAxes[6])`: raw keys and pad in, navigation, text editing, typed characters (US layout) and key repeat out.
   Also `aver_ui_input_wheel`, `aver_ui_input_char` (use it instead of the US table once the window forwards
   `WM_CHAR`), `aver_ui_input_pointer_valid(0)` while the game owns the mouse.
3. The game's HUD tick, then `aver_ui_widgets_frame(dt)`: layout, input, events, drawing into the draw list.
4. Submit the draw list; read `aver_ui_wants_input()` for next frame's input policy.

Widgets are 32-bit ids; create/find/destroy/reparent, typed shortcuts (`set_text`, `set_value`, `set_checked`,
`set_selected`, `set_items`, `set_visible`, `set_enabled`, `set_texture`), any property by name
(`set_prop`/`get_prop`), `layout_open(text)`/`layout_capture(root)`, `event_poll`, `menu_create`, and the
settings/rebind model calls.

## C# API

```csharp
Widget root = UiSystem.OpenLayout("Content/UI/MainMenu.ocui");
root.Find("PlayButton").OnClick(() => StartGame());
UiSystem.OnCommand("quit", _ => QuitGame());
Widget bar = Widget.Create(WidgetKind.Progress, parent: root, name: "Health");
bar.Value = 0.75f;
UiSystem.Pump();           // once a frame: delivers events, refreshes WasClicked/CommandFired
if (UiSystem.PausesGame) { /* a menu is open */ }
```

`UiSystem.ImageResolver` maps an `image` path to a texture id when a layout opens.

## The layout editor tab

Opens `.ocui` from the Content Browser. Palette (drag a kind onto the canvas or the hierarchy, or double-click to
add under the selection), hierarchy (select, drag to reparent, right-click duplicate/delete), a canvas that
previews the layout live at a chosen resolution and zoom, and a property inspector generated from the property
table (so a new property appears in the inspector with no editor change).

Canvas: click selects, drag moves (inside a stack or grid, dragging **reorders**), the eight handles resize (the
opposite edge stays put whatever the pivot), arrow keys nudge (Shift = 10), Delete removes, Ctrl+D duplicates.
The inspector has **anchor presets** that re-anchor without moving the widget, theme and scaling for the
document (root selected), and "Attach to" for reparenting. **Interact** runs the preview as a player would:
hover, click, slider drag, keyboard navigation, text entry; the last event is shown in the toolbar. Undo/redo are
whole-document snapshots; property drags are one undo entry. Renaming a widget rewrites `navUp/..` and
`defaultFocus` references to it.

## Visual-script nodes

The nodes are named `AN_*` as the plan asks (the existing catalog has no prefix; see "Wiring" for the catalog
rows). Widgets and layouts are ints. All are exec nodes (`exec` in, `then` out) and all but the two polling
nodes have a `success` out.

| Node | Attribute | Inputs | Outputs |
|---|---|---|---|
| `AN_OpenUiLayout` | `path=` | | `layout`, `success` |
| `AN_CloseUiLayout` | | `layout` | `success` |
| `AN_FindUiWidget` | `name=` | `layout` (0 = anywhere) | `widget`, `success` |
| `AN_CreateUiWidget` | `name=` | `parent` (0 = root), `kind` (0 panel .. 11 keybind) | `widget`, `success` |
| `AN_SetUiText` | `text=` | `widget` | `success` |
| `AN_SetUiValue` | | `widget`, `value` | `success` |
| `AN_SetUiChecked` | | `widget`, `checked` | `success` |
| `AN_SetUiVisible` | | `widget`, `visible` | `success` |
| `AN_SetUiEnabled` | | `widget`, `enabled` | `success` |
| `AN_GetUiValue` | | `widget` | `value`, `checked`, `selected`, `success` |
| `AN_UiWasClicked` | | `widget` | `clicked` |
| `AN_UiCommandFired` | `name=` | | `fired` |
| `AN_SetUiFocus` | | `widget` | `success` |
| `AN_OpenUiSettings` | | | `layout`, `success` |

`AN_UiWasClicked` and `AN_UiCommandFired` read what the last `UiSystem.Pump()` delivered, so Pump must run once a
frame before the graphs tick.

## Wiring the integration step must add

None of these were made: they touch files other agents own.

1. **Sandbox build**: `sandbox/CMakeLists.txt` source list: `src/UiLayoutEditor.cpp` (beside `src/BtEditor.cpp`).
2. **Editor tab**: `SandboxApp.hpp` `#include "UiLayoutEditor.hpp"` (beside `BtEditor.hpp`);
   `SandboxApp.cpp` `assetEditors_.registerFactory(&editor::makeUiLayoutEditor);` (appended after the Bt line).
3. **Content Browser**: `SandboxApp.hpp` declare `void cbCreateUiLayout();`; `SandboxContentBrowser.cpp`:
   ```cpp
   void SandboxApp::cbCreateUiLayout() {
       const std::filesystem::path target = cbFreeAssetPath("NewLayout", ".ocui");
       if (target.empty()) return;
       std::string why;
       if (!editor::writeNewFile(target.string(), editor::uiStarterLayoutText(), &why)) {
           cbStatus_ = "Could not write " + target.filename().string() + ": " + why;
           return;
       }
       cbAdoptNewAsset(target);
   }
   ```
   a menu item beside the others (`if (ImGui::MenuItem("New UI Layout")) cbCreateUiLayout(); uiReg_.track("cb.add.uiLayout");`)
   and an icon row `{".ocui", {ICON_TUNE, IM_COL32(120, 200, 255, 255), "UI Layout"}},`.
4. **Host input (both hosts)**: after publishing input each frame call `aver_ui_input_host_frame` (fill the two
   256-byte arrays from `InputState::keyPressed/keyHeld`, mouse buttons into VK 1/2/4, `vkToSlot` from
   `frameworkKeyFromVk` plus `AVER_FW_KEY_MOUSE_*`; the pad from `GamepadState`), `aver_ui_input_wheel(in.wheel())`,
   then after the HUD draw (`Runtime/src/GameApp.cpp` near `scripts_.hudDraw`; the editor's Play path in
   `SandboxApp.cpp`/`SandboxRender.cpp` beside its `aver_ui_begin_frame`) call `aver_ui_widgets_frame(dt)`.
5. **Pausing game input**: before `publishInput` (`GameApp.cpp`; `SandboxPlay.cpp`) read `wants = aver_ui_wants_input()`
   and AND `inputPolicy.keyboardToGame/mouseToGame/gamepadActive` with `!(wants & AVER_UI_WANTS_PAUSE)`; free the
   mouse-capture while `wants & AVER_UI_WANTS_CURSOR`; call `aver_ui_input_pointer_valid(captured ? 0 : 1)`.
6. **Pump**: call `Aver.UI.UiSystem.Pump()` once a frame, before graphs tick: top of `HostBridge.DispTickAll`
   when `group == 0`, and from `HudDraw`.
7. **C# projects**: `Aver.Framework.csproj` add `<ProjectReference Include="..\Aver.UI\Aver.UI.csproj" />`
   (Aver.UI references nothing, so no cycle). The Aver.UI sources are already globbed by `modules/scripting`.
8. **Graph nodes** (below).
9. **Docs**: `docs/ARCHITECTURE.md` mention `ui.abi` widget ABI and `docs/GAME_UI.md`.

### Graph node wiring

*Catalog* (`sandbox/src/GraphNodeDefs.hpp`, in `buildCatalog()`), category `"UI"`:

```cpp
// Game UI nodes: widgets and layouts are ints, all exec; one dispatcher (UiGraphInterop.UiNodeForGraph).
t.push_back({"AN_OpenUiLayout", "Open UI Layout", "UI", {pin("exec","exec",false), pin("then","exec",true),
    pin("layout","int",true), pin("success","bool",true)}, {attr("path","Layout file")}});
t.push_back({"AN_CloseUiLayout", "Close UI Layout", "UI", {pin("exec","exec",false), pin("layout","int",false),
    pin("then","exec",true), pin("success","bool",true)}});
t.push_back({"AN_FindUiWidget", "Find UI Widget", "UI", {pin("exec","exec",false), pin("layout","int",false),
    pin("then","exec",true), pin("widget","int",true), pin("success","bool",true)}, {attr("name","Widget name")}});
t.push_back({"AN_CreateUiWidget", "Create UI Widget", "UI", {pin("exec","exec",false), pin("parent","int",false),
    pin("kind","int",false), pin("then","exec",true), pin("widget","int",true), pin("success","bool",true)},
    {attr("name","Widget name")}});
t.push_back({"AN_SetUiText", "Set UI Text", "UI", {pin("exec","exec",false), pin("widget","int",false),
    pin("then","exec",true), pin("success","bool",true)}, {attr("text","Text")}});
t.push_back({"AN_SetUiValue", "Set UI Value", "UI", {pin("exec","exec",false), pin("widget","int",false),
    pin("value","float",false), pin("then","exec",true), pin("success","bool",true)}});
t.push_back({"AN_SetUiChecked", "Set UI Checked", "UI", {pin("exec","exec",false), pin("widget","int",false),
    pin("checked","bool",false), pin("then","exec",true), pin("success","bool",true)}});
t.push_back({"AN_SetUiVisible", "Set UI Visible", "UI", {pin("exec","exec",false), pin("widget","int",false),
    pin("visible","bool",false), pin("then","exec",true), pin("success","bool",true)}});
t.push_back({"AN_SetUiEnabled", "Set UI Enabled", "UI", {pin("exec","exec",false), pin("widget","int",false),
    pin("enabled","bool",false), pin("then","exec",true), pin("success","bool",true)}});
t.push_back({"AN_GetUiValue", "Get UI Value", "UI", {pin("exec","exec",false), pin("widget","int",false),
    pin("then","exec",true), pin("value","float",true), pin("checked","bool",true), pin("selected","int",true),
    pin("success","bool",true)}});
t.push_back({"AN_UiWasClicked", "UI Was Clicked", "UI", {pin("exec","exec",false), pin("widget","int",false),
    pin("then","exec",true), pin("clicked","bool",true)}});
t.push_back({"AN_UiCommandFired", "UI Command Fired", "UI", {pin("exec","exec",false), pin("then","exec",true),
    pin("fired","bool",true)}, {attr("name","Command")}});
t.push_back({"AN_SetUiFocus", "Set UI Focus", "UI", {pin("exec","exec",false), pin("widget","int",false),
    pin("then","exec",true), pin("success","bool",true)}});
t.push_back({"AN_OpenUiSettings", "Open UI Settings", "UI", {pin("exec","exec",false), pin("then","exec",true),
    pin("layout","int",true), pin("success","bool",true)}});
```

*Parser* (`OcGraphParser.AddDefaultPins`): one `case` per node, the same pins as the catalog rows (see
`playsound` for the shape). `path=` / `name=` / `text=` reuse `Node.SavePath` / `Node.NameValue` /
`Node.PrintText`, so the attribute parsing needs no change.

*Compiler* (`GraphCompiler.cs`): in the exec dispatch (next to `IsExecCapableAudioType`) add
`else if (IsExecCapableUiType(node.Type)) EmitExecUi(node);`, add the 14 types to the "side effect, refused by
`Compile()`" group beside `playsound`, and add:

```csharp
private static readonly HashSet<string> s_uiTypes = new() {
    "an_openuilayout","an_closeuilayout","an_finduiwidget","an_createuiwidget","an_setuitext","an_setuivalue",
    "an_setuichecked","an_setuivisible","an_setuienabled","an_getuivalue","an_uiwasclicked","an_uicommandfired",
    "an_setuifocus","an_openuisettings" };
private static bool IsExecCapableUiType(string type) => s_uiTypes.Contains(type.ToLowerInvariant());

private void EmitExecUi(Node node)
{
    if (_il == null) return;
    // Pushes the first of these input pins the node has, else the default.
    void Arg(Type t, params string[] names)
    {
        foreach (string n in names)
            if (node.Pins.Any(p => !p.IsOutput && p.Name == n)) { EmitPullInput(node, n); return; }
        if (t == typeof(float)) _il.Emit(OpCodes.Ldc_R4, 0f); else _il.Emit(OpCodes.Ldc_I4_0);
    }
    _il.Emit(OpCodes.Ldstr, node.Type.ToLowerInvariant());
    _il.Emit(OpCodes.Ldstr, node.SavePath ?? node.NameValue ?? node.PrintText ?? string.Empty);
    Arg(typeof(int), "layout", "widget", "parent");
    Arg(typeof(int), "kind");
    Arg(typeof(float), "value");
    Arg(typeof(bool), "checked", "visible", "enabled");
    var oi = _il.DeclareLocal(typeof(int)); var of = _il.DeclareLocal(typeof(float)); var ob = _il.DeclareLocal(typeof(bool));
    _il.Emit(OpCodes.Ldloca, oi); _il.Emit(OpCodes.Ldloca, of); _il.Emit(OpCodes.Ldloca, ob);
    _il.Emit(OpCodes.Call, UiNodeMethod);
    if (node.Pins.Any(p => p.IsOutput && p.Name == "success"))
        _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, "success", typeof(bool)));
    else _il.Emit(OpCodes.Pop);
    foreach (var (pin, local, type) in new[] {
        ("layout", oi, typeof(int)), ("widget", oi, typeof(int)), ("selected", oi, typeof(int)),
        ("value", of, typeof(float)),
        ("checked", ob, typeof(bool)), ("clicked", ob, typeof(bool)), ("fired", ob, typeof(bool)) })
        if (node.Pins.Any(p => p.IsOutput && p.Name == pin))
        { _il.Emit(OpCodes.Ldloc, local); _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, pin, type)); }
}

private static readonly MethodInfo UiNodeMethod =
    typeof(UiGraphInterop).GetMethod("UiNodeForGraph", BindingFlags.NonPublic | BindingFlags.Static)
    ?? throw new InvalidOperationException("Aver.Framework.UiGraphInterop.UiNodeForGraph was not found by reflection");
```

(`GraphCompiler.cs` needs `using Aver.Framework;` for `UiGraphInterop` as it already has for `GraphInterop`.)
Add the 14 rows to `docs/AVER_NODE_NODES.md` (a "UI" section) from the table above.

## Tests

`tests/ui` (all headless, listed in the existing `tests/ui/CMakeLists.txt`):

* `UiWidgetTest`: DPI modes and clamps, anchor resolution, axis solving (fill weights, min/max re-share,
  justify, margins), text wrapping, stack/grid/anchored layout and DPI scaling of a whole tree, hit testing
  (overlap, clipping, passive, modal), tab order and directional navigation, pointer click/toggle/choice/slider
  drag and snapping, text input editing, list selection, scrolling, keyboard/gamepad navigation and cancel,
  key-bind capture (including the same-frame guard), the settings/controls screens and the command menu,
  host input mapping with key repeat, drawing.
* `UiLayoutAssetTest`: tokenising and float round trip, the property table (every property's get/set agree for
  every kind), `.ocui` round trip, parse errors with line numbers, forward compatibility, the structural edits,
  instantiate and capture.
* `UiLayoutEditorTest`: move/resize/anchor-preset maths, the tab's load/save/dirty/undo against a real file,
  placement by drop, drag move and reorder, edge drag, duplicate/delete/reparent, rename following references.

## What is not done

* Nothing was linked, tested or run. Expect link-level fixes and, in tests, possible off-by-epsilon
  expectations; the integration step builds and reports.
* **Typed text uses a US key table** (`uiVkToChar`) because the platform window does not forward `WM_CHAR`; add
  a `Char` event to `Window`/`InputState` and feed `aver_ui_input_char` for other layouts and IME.
* **Fonts are one baked size** scaled per glyph; no kerning, bidirectional text or fallback fonts. A font atlas
  is still lent by the host (`aver_ui_set_font`).
* **Images**: a layout's `image` path is only a name; `UiSystem.ImageResolver` (C#) or `aver_ui_widget_set_texture`
  (C) must supply the texture. The editor preview shows a placeholder.
* **Graphics settings** are stored and raised as events (`GraphicsChanged`); no engine system applies them yet.
* **Gamepad rebinding** is not offered (the capture is keyboard and mouse); gamepad rows are greyed out.
* **Animation/transitions** (fades, slides) are not built; `opacity` can be driven from code.
* The nodes are exec-only; pure read variants (`AN_GetUiValue` as a data node) are a possible follow-up.
