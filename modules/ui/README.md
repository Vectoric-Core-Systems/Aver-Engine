# Aver.UI

The retained game UI. Core-only: a widget tree produces a draw list, something else turns that into draw
calls (`Aver.Render.UI`), and the whole thing is testable with no GPU and no window.

| File | What |
|---|---|
| `UiDrawList`, `UiFont` | The draw primitives: rectangles, textured quads, clip stack, baked-font text, hit rectangles. |
| `UiTypes`, `UiWidget` | Geometry, colours and the authored properties of a widget (`UiWidgetProps`). |
| `UiProps` | The property table: name, type, get/set as text. Feeds the file, the C ABI and the editor inspector. |
| `UiLayout` | Layout maths: DPI scale, anchors, one-axis stack solving, alignment, scroll clamping. |
| `UiTree` (`UiTree.cpp`, `UiTreeLayout.cpp`, `UiTreeInput.cpp`, `UiTreeDraw.cpp`) | The tree: layout, pointer/keyboard/gamepad input, focus, events, drawing through a `UiPainter`. |
| `UiStyle` | Themes (`dark`, `light`, `contrast`) and named styles. |
| `UiFocus` | Tab order and directional navigation as pure functions. |
| `UiText` | Text metrics, the painter interface (a draw-list painter is included) and `layoutText`/`drawText`, the seam the text engine plugs into. |
| `UiTextEngine`, `UiTextLayout` | The text engine: layout of a string into positioned glyphs (cached, one object for measuring and drawing), glyph quads into a draw list. |
| `UiFontFace`, `UiTrueType`, `UiFontSet` | One TrueType/OpenType face (via Dear ImGui's vendored `imstb_truetype.h`, compiled only here), and the `.ocfonts` font sets: families, fallback chains, per-language order and overrides. |
| `UiGlyphAtlas` | Paged, shelf-packed glyph atlas on the CPU with page-level LRU; `Aver.Render.UI` uploads the dirty pages. |
| `UiUnicode`, `UiBidi`, `UiShaping`, `UiLineBreak` | UTF-8, the Unicode range tables, bidirectional reordering, Arabic shaping and line breaking, each a pure function with its own test. |
| `UiRichText` | The small `<b> <i> <color> <size> <font> <br>` markup. |
| `UiLayoutAsset` | The `.ocui` layout asset: parse, write, instantiate, capture, the editor's structural edits. |
| `UiScreens` | A settings screen (graphics, audio, controls with key rebinding) and a command menu. |
| `UiHostInput` | Raw key/gamepad state to navigation, text editing and typed characters, with key repeat. |
| `UiTextEffect` | The draw passes of a text outline and shadow (a ring of copies, the shadow's ring, the fill last) and the bounds they grow to; `aver_ui_text_block_fx` walks them (G5 U2). |

Deliberately not Dear ImGui, which stays the editor's (the one exception is the vendored `imstb_truetype.h`,
a private include of this module used by `UiTrueType.cpp` only). No file I/O either: fonts arrive as byte blobs
from a host callback (`UiFontLoader`). See [docs/GAME_UI.md](../../docs/GAME_UI.md) for the model, the asset
format, the C ABI, the C# API, the editor tab and the graph nodes, and [docs/GAME_UI_TEXT.md](../../docs/GAME_UI_TEXT.md)
for text.
