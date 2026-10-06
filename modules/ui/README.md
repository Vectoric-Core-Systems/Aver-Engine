# Aver.UI

The retained game UI. Core-only: a widget tree produces a draw list, something else turns that into draw
calls (`Aver.Render.UI`), and the whole thing is testable with no GPU and no window.

| File | What |
|---|---|
| `UiDrawList`, `UiFont` | The draw primitives: rectangles, textured quads, clip stack, baked-font text, hit rectangles. |
| `UiTypes`, `UiWidget` | Geometry, colours and the authored properties of a widget (`UiWidgetProps`). |
| `UiProps` | The property table: name, type, get/set as text. Feeds the file, the C ABI and the editor inspector. |
| `UiLayout` | Layout maths: DPI scale, anchors, one-axis stack solving, alignment, scroll clamping. |
| `UiTree` (`UiTreeLayout/Input/Draw.cpp`) | The tree: layout, pointer/keyboard/gamepad input, focus, events, drawing through a `UiPainter`. |
| `UiStyle` | Themes (`dark`, `light`, `contrast`) and named styles. |
| `UiFocus` | Tab order and directional navigation as pure functions. |
| `UiText` | Text metrics, word wrap, and the painter interface (a draw-list painter is included). |
| `UiLayoutAsset` | The `.ocui` layout asset: parse, write, instantiate, capture, the editor's structural edits. |
| `UiScreens` | A settings screen (graphics, audio, controls with key rebinding) and a command menu. |
| `UiHostInput` | Raw key/gamepad state to navigation, text editing and typed characters, with key repeat. |

Deliberately not Dear ImGui, which stays the editor's. See [docs/GAME_UI.md](../../docs/GAME_UI.md) for the model,
the asset format, the C ABI, the C# API, the editor tab and the graph nodes.
