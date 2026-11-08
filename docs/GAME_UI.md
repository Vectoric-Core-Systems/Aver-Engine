# Game UI: widgets, menus and the layout editor

Status: merged and built. The headless suites (`UiTest`, `UiWidgetTest`, `UiLayoutAssetTest`, `UiLayoutEditorTest` and the
text, style and ABI tests under `tests/ui`) are in ctest and pass (526 of 526 on 2026-11-08). The designer's ImGui half
(palette, canvas, inspector) is not covered by ctest; it is checked by hand in the editor. Plan:
[plans/GAME_SYSTEMS_PLAN.md](plans/GAME_SYSTEMS_PLAN.md), feature 6; editor roadmap:
[plans/EDITOR_UE_PARITY.md](plans/EDITOR_UE_PARITY.md) track B.

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
| Animation, bindings, event routes | `modules/ui` (`UiAnimation.hpp`, `UiBindings.hpp`, `UiTreeRuntime.cpp`); C ABI `aver_ui_anim_*` |
| Tests | `tests/ui`: `UiWidgetTest`, `UiPanelsTest`, `UiAnimationTest`, `UiLayoutAssetTest`, `UiLayoutAbiTest`, `UiLayoutEditorTest` (and the existing `UiTest`) |

## The model

A `UiTree` owns widgets. A widget with no parent is a **root**; a root is one screen (a HUD, a menu, a dialog).
Roots draw back to front by `layer`, then `zOrder`, then creation order.

### Widget kinds

| Kind | Does | Events |
|---|---|---|
| `panel` | Container with a background and border (give it `style clear` for none). | none |
| `text` | A label; `wrap` word-wraps to its width. | none |
| `image` | A texture (`image` path, a file under Content, loaded by the host's image table: `aver_ui_image_use`, see ABI.md "Images by content path") or a tinted rectangle. A path that does not load keeps a grey placeholder. | none |
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

### Panels (UMG-style containers)

There is no widget kind per container. Each is a layout mode or a property on a `panel`, and the designer's
palette lists them by UMG's names (**Panels** section; each is a `panel` with those properties set):

| Palette name | Is |
|---|---|
| Canvas Panel, Vertical Box, Horizontal Box, Grid Panel | `layout none / vstack / hstack / grid` |
| Uniform Grid Panel | `layout grid` with `uniform true`: every cell as big as the largest, spare space shared equally |
| Overlay | `layout overlay`: every child shares the content box, placed by its `alignH`/`alignV`, `margin`, and size |
| Wrap Box | `layout wrap`: children flow in rows and break at the width (`width`, else the width the parent gives); `spacing` between children, `spacingY` between rows, `justify` per row. A wrap box with no width learns it on the second layout pass, like a wrapped label |
| Size Box | an overlay with `width`/`height` (override) and `minWidth`..`maxHeight` (clamps); any widget takes these |
| Scale Box | an overlay with `scaleFit` (`none contain cover down width height`) or a fixed `contentScale`: the children's whole subtree is laid out at the scaled design-pixel scale, so text, padding and borders scale too |
| Border | a padded overlay panel; `borderWidth` (design px, `-1` = the style's) and `borderColor` on any widget |
| Spacer | an empty panel with `hit never` and a size |
| Widget Switcher | `layout switcher`: only the child at `activeIndex` is laid out, drawn, hit-tested and focusable |

Enum values are append-only (`layout` numbers 4, 5 and 6 are overlay, wrap and switcher).

### DPI scaling

`SCALE <mode> <refWidth> <refHeight>` in the asset (default `height 1920 1080`). Modes: `constant` (the OS
display scale only), `width`, `height`, `shortest`, `blend` (geometric mean of the width and height ratios, so
ultrawide and portrait windows do not blow the UI up). Multiplied by the player's **user scale** (the
`graphics.uiScale` setting drives it) and clamped to 0.25..8. Fonts scale the same way: text is rasterised at the
scaled pixel size (see [GAME_UI_TEXT.md](GAME_UI_TEXT.md)); the baked bitmap font, used when no TrueType font can
be opened, is scaled per glyph. Offsets, sizes, margins, padding, spacing and borders all scale.

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

* Version 2 adds `USE` (a nested layout), `OVERRIDE`, `EXPOSE`, `SLOT`, `ANIM`, `BIND` and `EVENT`; see "Nested
  layouts" below and the format in [formats/FORMAT_SPECS.md](formats/FORMAT_SPECS.md) section 11n. A layout that
  uses none of them is still written as `OCUI 1`.
* One root, parents before children, unique names. `WIDGET <name> <kind> <parent|->`.
* Only properties that differ from the kind's defaults are written, so files stay small and diffs readable.
* Unknown records and unknown `SET` keys are skipped (a newer build's file loads); a bad value, a missing
  parent, a duplicate name or a second root is an error naming the line.
* `write(parse(write(doc))) == write(doc)` is tested, floats print to the shortest text that reads back
  bit-exact.
* The module does no file I/O (like `UiFont`): the host reads the text.

### Nested layouts (user widgets)

A `USE` node instantiates another `.ocui` as a subtree. The used layout says what it lets others change
(`EXPOSE <name> <widget> <key>`) and where children go (`SLOT <name> <widget>`); the user sets the exposed values
with `OVERRIDE` and puts its own children into slots with their `slot` property. The used widgets are named
`<instance>/<name>`. `UiSystem.OpenLayout` (C#) finds the used files beside the opened file, in its parent folders
(so a content-relative `UI/Row.ocui` works) and in `UiSystem.LayoutRoots`; a C host installs
`aver_ui_set_layout_reader`. Missing files become placeholders (`UiSystem.LastWarnings`, `aver_ui_layout_warnings`);
a cycle or nesting past 16 levels fails the open (`UiSystem.LastError`). In the designer a used layout is one item in
the hierarchy; the palette's **Layouts** section lists the project's other `.ocui` files (double-click or drag),
and the inspector shows the layout path and its exposed properties. A layout that would make a cycle is refused
with the chain in the toolbar.

### Animation

`ANIM <name> <widget> <property> (<time> <value>)*` records make animations: tracks of keys over a widget's `opacity`,
`bgColor`, `textColor`, `accentColor`, `tint` and render transform (`renderTranslate`, `renderRotation`,
`renderScale`; aliases `offset`, `rotation`, `scale`, `color`). A value is a float, `x,y` or `#RRGGBB[AA]`, with an
optional `@ease` that shapes the segment leaving that key (`linear step in out inout cubicin cubicout cubicinout`).
Records sharing a name make one animation; a layout opened with `uiInstantiateLayoutEx` / `aver_ui_layout_open` keeps
them with its root, and a used layout's are `<instance>/<name>`.

The values sit over the widget's properties and are never written to them (a saved capture has the authored look).
`UiTree::animPlay(root, name, {speed, loops, reverse, pingPong})`, `animStop(release)`, `animSeek`, `animReverse`,
`animPause`; a finished play raises `AnimationFinished` (widget = the root, text = the name) and keeps its last values
until released. `aver_ui_anim_*` (C), `UiAnimation.Play/Stop/Seek/Reverse/Pause/IsPlaying/Time` and
`UiSystem.AnimationFinished` (C#), and the `AN_PlayUiAnimation` family of nodes expose it.

The render transform (`renderTranslate` design px, `renderRotation` degrees, `renderScale`, `renderPivot` as a fraction
of the widget's rectangle, default the centre) is a 2D affine pushed on the draw list around the widget and its
children (`UiPainter::pushTransform`). It is draw-only: layout, hit testing and focus ignore it, so a widget that moves
a long way should not be clicked while it does.

### Property reference

Colours are `#RRGGBB` or `#RRGGBBAA`, or `none`. Insets/anchors/uv take 4 numbers (insets also 1 or 2, a point
anchor 2). Enum names are lower case.

* **Placement:** `anchors`, `offsets`, `pivot`, `width`, `height` (`-1` auto), `minWidth`, `minHeight`,
  `maxWidth`, `maxHeight` (`0` none), `margin`, `alignH`, `alignV`, `fillW`, `fillH`, `slot` (a child of a `USE`:
  the used layout's slot it fills).
* **Layout:** `padding`, `layout` (`none vstack hstack grid overlay wrap switcher`), `spacing`, `spacingY`,
  `columns`, `cellWidth`, `cellHeight`, `justify` (`start center end spacebetween`), `uniform`, `activeIndex`,
  `scaleFit` (`none contain cover down width height`), `contentScale`.
* **Appearance:** `visible`, `enabled`, `opacity`, `clip`, `hit` (`auto always never`), `style`, `bgColor`,
  `textColor`, `accentColor`, `borderWidth`, `borderColor`.
* **Content:** `text`, `textAlign` (`left center right start end`), `fontSize`, `wrap`, `fontFamily`, `bold`,
  `italic`, `textDir` (`auto ltr rtl`), `lineSpacing`, `letterSpacing`, `ellipsis`, `maxLines`, `rich`, `image`,
  `uv`, `imageMode` (`stretch fit`), `tint`, `placeholder`, `maxLength`, `masked`. The text properties are
  explained in [GAME_UI_TEXT.md](GAME_UI_TEXT.md); a style (`UiStyle`) can also set `fontFamily` and `bold`.
* **Value:** `value`, `minValue`, `maxValue`, `step`, `showValue`, `checked`, `selected`, `items`, `rowHeight`.
* **Transform:** `renderTranslate`, `renderRotation`, `renderScale`, `renderPivot` (draw-only, see Animation).
* **Navigation:** `focusable`, `tabIndex`, `navUp`, `navDown`, `navLeft`, `navRight`, `command`.
* **Accessibility:** `accessName` (the label a narrator speaks; empty = derived from the widget's text or its label sibling).
* **Root only:** `inputMode`, `modal`, `zOrder`, `layer`, `defaultFocus`, `cancelCommand`.

## Text

Text is drawn by the text engine (runtime TrueType, fallback fonts, kerning, Arabic shaping, bidirectional text,
line breaking, rich text); see [GAME_UI_TEXT.md](GAME_UI_TEXT.md) for the model, the `.ocfonts` font sets, the
properties, the C and C# API, the nodes and the limits. A widget measures and draws from one cached layout, so
the size a script reads back (`Widget.TextSize`, `AN_GetUiTextSize`) is the size that was drawn. With no usable
TrueType font the tree behaves exactly as before and draws with the lent baked font.

## Ready-made screens

`UiSettingsScreen` builds a tabbed settings screen from a `UiSettingsModel` (items of type toggle, slider,
choice, on named tabs) and, optionally, a `UiRebindModel` (rows: action, slot, label, current binding text) for a
**Controls** tab of key-bind buttons. `uiDefaultSettingsModel()` provides Graphics (quality, render scale, TAA,
vsync, fullscreen), Audio (master, music, effects, voice), **Accessibility** (UI scale 0.75 to 2.0, text size, high
contrast, colour vision filter and strength, reduce motion, shake, head bob, motion blur, lens effects), **Captions and
Speech** (mode, size, background, position, lines, speaker names and colours, direction hints, text to speech and its
sources, rate, volume) and **Input Assist** (hold time, tap and combo window; the C# side adds one toggle per action).
The window fills 90 percent of the viewport up to 980 x 720 design pixels, centred, and row labels wrap, so a 2.0 UI
scale on a small screen keeps Back reachable. See [ACCESSIBILITY.md](ACCESSIBILITY.md). The screen edits the model and raises
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

## Accessibility

`UiTree::setAccess({textScale, highContrast})` (C: `aver_ui_set_access`) is a property of the tree, not of widgets. `textScale`
multiplies font sizes only; row heights, padding and widths stay on the UI scale. `highContrast` resolves styles against the
built-in `contrast` theme (sized like the game's theme), ignores `bgColor`/`textColor`/`accentColor` and rich-text colours, doubles
the focus ring and draws the scrollbar white; the game's theme returns when it is off. Both default to today's behaviour exactly.

`uiAccessibleText(tree, id)` (C: `aver_ui_widget_accessible_text`) is what a narrator says: `name, role, state or value`, with the role
and state words as `@Access:...` references that localize. `aver_ui_set_announcer(fn)` installs a callback (the framework's
`aver_fw_speech_announce_ui`) that is called with that text, interrupting, on every focus change and every slider, toggle or choice
change.

`UiCaptionView` is the caption overlay: one passive Overlay root named `Aver.Captions` with a row per line, recycled between frames,
hidden when the list is empty. The host pushes the lines before the frame with `aver_ui_captions_set(lines, count, sizeScale,
bgOpacity, top)`. It never takes the pointer or focus, and its text stays a raw reference so the tree localizes it.

A `Text` that wraps and takes its width from a fill weight (`fillW > 0`, no `width`) now wraps at the width its slot gave it: the
tree runs a second layout pass when such a widget's width changes.

## Localized text

Any text field (`text`, `items`, `placeholder`, menu and settings labels) may hold a reference `@Table:Key=Default` instead of literal text; `UiTree::shown(raw)` is what layout and drawing read. A host installs the resolver with `aver_ui_set_text_resolver(aver_fw_loc_resolve, aver_fw_loc_epoch)`; without one a reference shows its inline default. Typed text and event payloads stay raw. The built-in screens use `@UI:...` references with English defaults. The shipped font (Roboto) covers Latin with accents, Greek and Cyrillic; CJK, Arabic and Hebrew come from the Windows fonts or a project font through a font set ([GAME_UI_TEXT.md](GAME_UI_TEXT.md)). See [LOCALIZATION.md](LOCALIZATION.md).

## C ABI

`ui_widget_abi.h`. The host's per-frame protocol:

1. `aver_ui_begin_frame`, `aver_ui_set_font`, `aver_ui_set_pointer` (existing). The text engine's host calls
   (`aver_ui_text_set_loader`, `aver_ui_text_load_fontset`, `aver_ui_text_atlas`, ...) are listed in
   [GAME_UI_TEXT.md](GAME_UI_TEXT.md); `aver_ui_widget_text_size` reads a widget's laid-out text size.
2. `aver_ui_input_host_frame(dt, keyPressed[256], keyHeld[256], vkToSlot[256], padConnected, padButtons[14],
   padAxes[6])`: raw keys and pad in, navigation, text editing, typed characters (US layout) and key repeat out.
   Also `aver_ui_input_wheel`, `aver_ui_input_char` (use it instead of the US table once the window forwards
   `WM_CHAR`), `aver_ui_input_pointer_valid(0)` while the game owns the mouse.
3. The game's HUD tick, then `aver_ui_widgets_frame(dt)`: layout, input, events, drawing into the draw list.
4. Submit the draw list; read `aver_ui_wants_input()` for next frame's input policy.

Accessibility calls (host): `aver_ui_set_access`, `aver_ui_captions_set`, `aver_ui_set_announcer`, `aver_ui_widget_accessible_text`.

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

Text: `Widget` has typed `FontFamily`, `FontSize`, `Bold`, `Italic`, `TextAlign`, `TextDir`, `Wrap`, `Ellipsis`,
`MaxLines`, `LineSpacing`, `LetterSpacing`, `RichText`, `TextColour` and `TextSize`; `UiText` loads font sets,
measures, prewarms and reads the atlas counters; `Hud.TextBlock` / `Hud.MeasureText(text, style)` draw and measure
styled blocks. See [GAME_UI_TEXT.md](GAME_UI_TEXT.md).

## The layout editor tab

Opens `.ocui` from the Content Browser. Palette (drag a kind onto the canvas or the hierarchy, or double-click to
add under the selection), hierarchy (select, drag to reparent, right-click duplicate/delete), a canvas that
previews the layout live at a chosen resolution and zoom, and a property inspector generated from the property
table (so a new property appears in the inspector with no editor change). When the project has a font set with a
TrueType face, the canvas draws text with the game's own engine (same fallback fonts, kerning, shaping and
wrapping as Play); otherwise it uses the editor's font. `fontFamily` is a combo of the family names in
`Content/UI/Fonts.ocfonts`. `.ocfonts` files open in their own tab (a font set editor with a live preview, per-face
coverage and the atlas pages); see [GAME_UI_TEXT.md](GAME_UI_TEXT.md).

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
| `AN_PlayUiAnimation` | `name=` | `layout`, `loops` (0 = until stopped), `speed`, `reverse` | `success` |
| `AN_StopUiAnimation` | `name=` | `layout`, `release` | `success` |
| `AN_SeekUiAnimation` | `name=` | `layout`, `time` | `success` |
| `AN_ReverseUiAnimation` | `name=` | `layout` | `success` |
| `AN_UiAnimationFinished` | `name=` | `layout` | `finished` |
| `AN_OpenUiSettings` | | | `layout`, `success` |

`AN_UiWasClicked` and `AN_UiCommandFired` read what the last `UiSystem.Pump()` delivered, so Pump must run once a
frame before the graphs tick.

## Where it is wired

| Piece | Where |
|---|---|
| Editor tab, "New UI Layout" in the Content Browser | `SandboxInit.cpp` (`makeUiLayoutEditor`), `SandboxContentBrowser.cpp` (`cbCreateUiLayout`) |
| Per-frame UI update and draw | `SandboxUpdate.cpp` (Play), `Runtime/src/GameApp.cpp` (shipped game): `aver_ui_widgets_frame` |
| Host input mapping, pausing game input | `Runtime/include/aver/game/GameUiInput.hpp`, `GameAccessibility.hpp` |
| `UiSystem.Pump()` | `HostBridge.cs` (once a frame, before graphs tick) |
| Visual-script nodes (`AN_*Ui*`) | catalog in `sandbox/src/GraphNodeDefsGameSystems.hpp`, compiled through `Aver.Framework/UiGraphInterop.cs` |

## Tests

`tests/ui` (all headless, listed in the existing `tests/ui/CMakeLists.txt`):

* `UiWidgetTest`: DPI modes and clamps, anchor resolution, axis solving (fill weights, min/max re-share,
  justify, margins), text wrapping, stack/grid/anchored layout and DPI scaling of a whole tree, hit testing
  (overlap, clipping, passive, modal), tab order and directional navigation, pointer click/toggle/choice/slider
  drag and snapping, text input editing, list selection, scrolling, keyboard/gamepad navigation and cancel,
  key-bind capture (including the same-frame guard), the settings/controls screens and the command menu,
  host input mapping with key repeat, drawing.
* `UiPanelsTest`: the panel containers: overlay alignment, uniform and plain grids, wrap breaking and justify, size
  box clamps, scale box fits and scaled text, border drawing, the switcher's hit/draw/focus filtering, the
  palette presets and the file round trip of the new modes.
* `UiAnimationTest`, `UiAnimationAbiTest`: easing, ANIM parse/write, sampling, the player (play, reverse, loop,
  ping-pong, pause, seek, stop, the finish event, nested layouts), the render transform in the draw list, and the
  same through the C ABI.
* `UiLayoutAbiTest`: nested layouts through the C ABI (reader, overrides, slots, warnings, missing file, cycle, a
  layout larger than the first buffer).
* `UiLayoutAssetTest`: the v2 records (byte-identical round trip, v1 unchanged, errors), instantiate with overrides,
  slots, a missing file, cycles and depth; tokenising and float round trip, the property table (every property's get/set agree for
  every kind), `.ocui` round trip, parse errors with line numbers, forward compatibility, the structural edits,
  instantiate and capture.
* Text engine tests (`tests/ui/src`, all headless; fonts come from the synthetic builder in `TestFont.hpp`, so no
  system font is needed): `UiUnicodeTest`, `UiBidiTest`, `UiShapingTest`, `UiLineBreakTest`, `UiFontTest`,
  `UiGlyphAtlasTest`, `UiTextLayoutTest`, `UiTextTreeTest` and `UiTextAbiTest`; `tests/render.ui` checks the atlas
  upload and the page tokens; `tests/editor` has `FontSetEditorTest`.
* `UiLayoutEditorTest`: move/resize/anchor-preset maths, the tab's load/save/dirty/undo against a real file,
  placement by drop, drag move and reorder, edge drag, duplicate/delete/reparent, rename following references.

## What is not done

* **Typed text**: a host that feeds the window's characters through `aver_ui_input_char` gets the real layout,
  dead keys and committed IME text; without that the tree falls back to a US key table (`uiVkToChar`).
* **Text limits** are listed in [GAME_UI_TEXT.md](GAME_UI_TEXT.md) (no Indic or Thai shaping, no vertical text, no
  IME, OS fonts on Windows only). A baked bitmap font is still lent by the host (`aver_ui_set_font`); it is the
  last fallback face and the whole path when no TrueType font can be opened.
* **Images**: a layout's `image` path resolves through the host's image table (`aver_ui_image_use`), or
  `UiSystem.ImageResolver` (C#) / `aver_ui_widget_set_texture` (C). The editor preview shows a placeholder.
* **Graphics settings** are stored and raised as events (`GraphicsChanged`); no engine system applies them yet.
* **Gamepad rebinding** is not offered (the capture is keyboard and mouse); gamepad rows are greyed out.
* **Animation**: the runtime exists (see Animation). Not done: animating layout properties (size, margin), hit
  testing that follows a render transform, curves with custom tangents (the eases are fixed).
* The nodes are exec-only; pure read variants (`AN_GetUiValue` as a data node) are a possible follow-up.
