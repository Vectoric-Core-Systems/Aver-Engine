# Game UI text

How the game UI draws text: runtime TrueType fonts with fallback chains, kerning, Arabic shaping, right-to-left
text, line breaking for CJK, styling and a small rich-text markup. It sits behind the existing widget tree
([GAME_UI.md](GAME_UI.md)), so every widget that shows text uses it and a script sees nothing new to learn beyond
the properties below.

The design contract is `docs/plans/wave9/UI_TEXT.md`; this page is what the code does.

## What you get

* **Scalable fonts.** Glyphs are rasterised at the pixel size the widget is drawn at (4 to 160 px), so text is
  sharp at any UI scale. The baked bitmap font is the last fallback and the whole path when no TrueType font can
  be opened, in which case nothing changes from before.
* **Fallback fonts.** A *font set* (`Content/UI/Fonts.ocfonts`) names families, each an ordered chain. A glyph
  comes from the first face in the chain that has it: your fonts, then the Windows fonts, then the baked font. A
  code point nobody has draws a visible hollow box, so a missing font is noticed rather than silent.
* **Kerning** from the font (`kern` table, or GPOS pair adjustment).
* **Arabic** is shaped (initial, medial, final and isolated forms, lam-alef ligatures, marks placed on their
  letters). **Hebrew and Arabic run right to left**, mixed text is reordered, brackets mirror.
* **Line breaking**: spaces and hyphens for Latin, between any two characters for Chinese and Japanese with the
  usual rules about which characters may not start or end a line.
* **Styling**: family, bold, italic, size, alignment (including `start`/`end` that follow the text direction),
  direction, line spacing, letter spacing, ellipsis, line limit, and `<b> <i> <color> <size> <font> <br>` markup.
* **UTF-8 everywhere.** A multi-byte character is one glyph (the old per-byte drawing of non-ASCII text was a bug
  and is fixed on the bitmap path too).

## Zero configuration

A game with no `Content/UI/Fonts.ocfonts` uses a built-in set:

```
OCFONTS 1
DEFAULT Default
FAMILY Default
FACE @exe/Roboto-Regular.ttf
FACE @exe/Roboto-Medium.ttf weight=bold
SYSTEM auto
BITMAP legacy
```

`Roboto-Regular.ttf` and `Roboto-Medium.ttf` (Apache-2.0) ship next to the executable. `SYSTEM auto` adds the
Windows fonts for the scripts Roboto lacks. `BITMAP legacy` is the baked font the host lends with
`aver_ui_set_font`.

## Font sets: `.ocfonts`

```
OCFONTS 1
DEFAULT Body
FAMILY Body
FACE   Fonts/Inter-Regular.ttf
FACE   Fonts/Inter-Bold.ttf weight=bold
FACE   Fonts/Inter-Italic.ttf italic
FACE   Fonts/Mono.ttc index=1 scale=0.95 dy=0.5
FACE   @exe/Roboto-Regular.ttf
SYSTEM auto
SYSTEM msyh.ttc index=0 lang=zh-Hans
BITMAP legacy
LINEGAP 1.0
FAMILY Heading
FACE   Fonts/Display.ttf
OVERRIDE ja Body BodyJa
```

`#` starts a comment. Keys are case-insensitive, unknown keys are skipped (a file from a newer build still
loads), and a bad value is an error naming the line.

| Line | Meaning |
|---|---|
| `OCFONTS <n>` | Header. A higher version than this build reads loads with a note. |
| `DEFAULT <family>` | The family used when a widget names none, or names one that does not exist. The first family if omitted. |
| `FAMILY <name>` | Starts a family. Names are matched ignoring case. |
| `FACE <path> [attrs]` | A project font: a path under the project's Content folder, or `@exe/Name.ttf` for a file next to the executable. `.ttf`, `.otf` and `.ttc` work. |
| `SYSTEM <file\|auto> [attrs]` | An OS font by file name (looked up in the Windows fonts folders), or `auto` for the built-in list below. Opened the first time a glyph needs it. Windows only for now. |
| `BITMAP legacy` | The baked bitmap font lent by the host. |
| `LINEGAP <x>` | Multiplier on the primary face's line gap. |
| `OVERRIDE <lang> <from> <to>` | When the UI language is `lang`, the family `from` means `to`. |

Attributes on `FACE` and `SYSTEM`: `weight=regular|bold`, `italic`, `index=<n>` (a collection member), `lang=<tag>`
(use only for that UI language, prefix match, tried first), `scale=<x>` (size multiplier for this face, to even
out fonts of different size), `dy=<px>` (baseline shift at 16 px, positive down).

Within a family the order is **project faces, then OS fonts, then the bitmap font**, each group in the order
written. Selection for a code point under (bold, italic) first looks at the entries whose flags equal the
request, then at the rest, so asking for bold text still shows a glyph that only the regular face has. Among OS
fonts, those whose `lang=` matches the UI language come first: that is how Han characters get Japanese or
Chinese shapes. The decision is remembered per code point.

`SYSTEM auto` tries, in this order and skipping any that is not installed: `segoeui.ttf tahoma.ttf arial.ttf`
(Latin, Greek, Cyrillic, Arabic, Hebrew), `seguisym.ttf`, `YuGothM.ttc meiryo.ttc msgothic.ttc` (ja),
`malgun.ttf` (ko), `msyh.ttc simsun.ttc` (zh-Hans), `msjh.ttc` (zh-Hant), `Nirmala.ttf` (Devanagari),
`LeelawUI.ttf` (Thai).

The UI language comes from the active culture (the host reads it every frame; `Loc.SetCulture` or
`AN_SetCulture` switch it) or from `UiText.Language`. A change reorders the chains and applies the `OVERRIDE`
rows. A reload of the font set (the file or a font changed) drops every cached layout and refills the atlas.

## Widget properties

All of these are in the property table, so they work in `.ocui`, the layout editor inspector, `Widget.Set` and
`aver_ui_widget_set_prop`. Only non-default values are written to a file.

| Property | Values | Notes |
|---|---|---|
| `fontFamily` | family name | Empty: the style's family, then the font set's default. |
| `fontSize` | design pixels | Existing. Multiplied by the UI scale. |
| `bold`, `italic` | bool | Picks a face with that flag. There is no synthetic bold; a missing italic face is sheared by 0.2. |
| `textAlign` | `left center right start end` | `start`/`end` follow the paragraph direction (right-to-left text starts on the right). |
| `textDir` | `auto ltr rtl` | `auto` takes the direction of the first strong character. |
| `wrap` | bool | Existing. Needs a width. |
| `lineSpacing` | 0.5 to 3 | Multiplier on the line height. |
| `letterSpacing` | -10 to 50 | Design pixels added after each glyph (not inside joined Arabic). |
| `ellipsis` | bool | A line that does not fit ends in `...` (U+2026, or three dots if no face has it). |
| `maxLines` | 0 to 100 | 0 is unlimited. The last kept line ends in an ellipsis if text was cut. |
| `rich` | bool | Reads the markup below. Off by default so existing strings are unchanged. Not for text inputs. |

A style (`UiStyle`) can set `fontFamily` and `bold` for every widget that uses it; a widget's own value wins.

## Rich text

With `rich` on, these tags are read (case-insensitive, nesting by a stack, unclosed tags close at the end):

| Tag | Effect |
|---|---|
| `<b>...</b>`, `<i>...</i>` | Bold, italic. |
| `<color=#RRGGBB>...</color>`, `<color=#RRGGBBAA>` | Text colour (the caller's alpha is multiplied in). |
| `<size=N>...</size>` | Size in design pixels. |
| `<font=Family>...</font>` | Another family. |
| `<br>` | Line break. |
| `<<` | A literal `<`. |

Anything that is not exactly one of those (`a < b`, `<3`, `<unknown>`) is shown as text. `UiRichText.hpp` has
`uiStripRichText` for code that needs the plain text (speech, search).

## How a string becomes glyphs

1. UTF-8 is decoded strictly; each invalid byte becomes U+FFFD. Limits per string: 16,384 code points, 512 lines,
   32,768 glyphs (the excess is dropped and logged once).
2. Rich markup is stripped into spans.
3. Paragraphs split at line feed, carriage return, NEL and U+2029.
4. Bidirectional levels are resolved (a subset of UAX #9: explicit embeddings and overrides, weak and neutral
   rules, implicit levels, mirroring; no bracket pairing, isolates act as embeddings).
5. Arabic is shaped to presentation forms. The face needs the forms `FE70` to `FEFC`; Arial, Segoe UI and Tahoma
   have them. A face with the base letters but not the forms shows the letters unjoined (readable, not
   connected); the font set editor's Coverage tab shows this per face.
6. Each cluster (a base and its marks) gets a face from the chain; kerning applies between neighbours of one face.
   Marks are placed from glyph bounding boxes (centred on the base, lifted clear of it).
7. Lines are broken with a UAX #14 subset plus CJK break-anywhere and the kinsoku rules (characters that may not
   start a line, such as closing brackets, and may not end one, such as opening brackets, push the break one
   cluster earlier). Trailing spaces hang. A word wider than the line breaks between clusters.
8. Each line is reordered visually and measured; its ascent, descent and height are whole pixels.
9. The result is one cached object (`UiTextLayout`) that both measuring and drawing read, with a caret stop per
   cluster edge for text inputs. The cache holds 1024 layouts keyed by text, style, width and the font set.

## The glyph atlas

Glyphs are kept in pages (512 by 512 by default, up to 16) packed in shelves; a page used by no glyph this frame is
recycled whole when the cap is reached. At most 256 new glyphs are rasterised per frame; a layout that could not
finish is drawn with what exists and completed on a later frame (a loading screen can call `Prewarm`). On the GPU
each page is a premultiplied RGBA8 texture, uploaded once from the render system's pre-pass, so the UI shader and
pipeline are unchanged and D3D12 and Vulkan behave identically. Steady state uploads nothing.

Tune with `UiText.Configure(pageSize, maxPages, glyphBudget)` or `aver_ui_text_configure`. `UiText.Stats` shows
pages, resident glyphs, rasterisations, recycled pages, overflows and cache hits. Overflow (every page used in
one frame) means the page cap is too small for the text on screen: raise `maxPages`.

## C ABI

In `ui_abi.h`, additive. Constants: `AVER_UI_TEXT_ALIGN_LEFT/CENTER/RIGHT/START/END` (0 to 4),
`AVER_UI_TEXT_DIR_AUTO/LTR/RTL` (0 to 2), `AVER_UI_TEXT_FLAG_BOLD/ITALIC/RICH/ELLIPSIS/WRAP` (1, 2, 4, 8, 16),
`AVER_UI_FONT_CONTENT/SYSTEM/ENGINE` (0, 1, 2).

| Export | Purpose |
|---|---|
| `aver_ui_text_set_loader(open, close)` | The host's font file callbacks (it maps files and hands back pointer, size and a token). |
| `aver_ui_text_load_fontset(text)` / `aver_ui_text_error()` | Loads an `.ocfonts`, replacing the set. 1 on success. |
| `aver_ui_text_set_language(tag)` / `aver_ui_text_set_language_fn(fn)` | The UI language, or a function re-read every frame (hosts pass `aver_fw_loc_culture`). |
| `aver_ui_text_configure(pageSize, maxPages, glyphBudget)` | Atlas settings; 0 or less keeps. |
| `aver_ui_text_atlas()` | Opaque atlas pointer for the renderer (`UiRenderer::setTextAtlas`). Hosts never dereference it. |
| `aver_ui_text_stats(float out[12])` | pages, glyphsResident, rasterThisFrame, rasterTotal, evictedPages, overflow, layoutHits, layoutMisses, cacheEntries, incompleteLayouts, faces, uploadBytesLastFrame. |
| `aver_ui_text_has_glyph(family, codePoint)` | Whether the chain has a glyph for the code point. |
| `aver_ui_text_measure(text, family, px, flags, maxWidth, out[3])` | Width, height, line count. |
| `aver_ui_text_block(x, y, text, rgba, family, px, flags, maxWidth, align, out[3])` | Draws a block with its top-left at (x, y); returns its height. |
| `aver_ui_text_block_fx(x, y, text, rgba, family, px, flags, maxWidth, align, outlineSize, outlineRgba, shadowRgba, shadowOffX, shadowOffY, shadowOutlineSize, out[3])` | `aver_ui_text_block` with an outline and a shadow (Godot's `outline_size`, `font_outline_color`, `font_shadow_color`, `shadow_offset`, `shadow_outline_size`; used by the Fabric Control layer, `G5_PARITY.md` U2). The block is drawn as several passes through the same path: the shadow's ring, the shadow copy, the outline ring (8 to 32 copies round the origin, a second ring at half the radius from 3 px up), then the fill in `rgba`. Outline size 0 and a shadow of alpha 0 draw exactly `aver_ui_text_block`. Colours are straight 0xAABBGGRR; copies of a translucent outline overlap and darken where they do. The pass list is `aver/ui/UiTextEffect.hpp` (CPU, tested by `UiTextEffectTest`). |
| `aver_ui_text_prewarm(text, family, px, flags)` | Rasterises ahead of time, ignoring the budget. |
| `aver_ui_widget_text_size(id, out[3])` | The size of a widget's text from the last frame (width, height, lines). |

`aver_ui_text`, `aver_ui_text_width`, `aver_ui_font_metrics` and `aver_ui_has_font` keep their signatures; with a
usable TrueType face they use the default family at 16 px, otherwise the lent bitmap font as before.

## C#

```csharp
UiText.Language = "ja";                                    // normally the host follows Loc
bool ok = UiText.HasGlyph("Body", 0x3042);
TextMetrics m = UiText.Measure("Hello", new TextStyle(Family: "Heading", Size: 28f, Bold: true), maxWidth: 300f);
UiText.Prewarm("Loading...", new TextStyle(Size: 24f));
float h = Hud.TextBlock(new Rect(40, 40, 400, 0), "A long caption that wraps", Colour.White,
                        new TextStyle(Size: 20f), TextAlignment.Center);

Widget label = Widget.Create(WidgetKind.Text, parent: root, name: "Title");
label.Text = "<b>Wave</b> 3";
label.RichText = true;
label.FontFamily = "Heading";
label.TextAlign = TextAlignment.Start;
label.TextColour = Colour.Rgb(255, 220, 120);
TextMetrics size = label.TextSize;                         // from the last laid-out frame
```

`TextAlignment`, `TextDirection` and `TextFlags` mirror the C constants. `UiText.Stats`, `UiText.Configure` and
`UiText.LoadFontSet` are there for tools; a game normally does none of them.

## Aver Node nodes (category UI)

| Node | Pins | Attribute | Does |
|---|---|---|---|
| `AN_SetUiFont` | `widget`, `size` (0 = theme), `bold`, `italic`; out `success` | `family` (empty keeps) | Sets the font of a widget. |
| `AN_SetUiTextStyle` | `widget`, `align` (0 to 4), `dir` (0 to 2), `wrap`, `ellipsis`, `maxLines`, `lineSpacing`, `letterSpacing`, `rich`; out `success` | | Sets all nine style properties together. |
| `AN_SetUiTextColor` | `widget`, `r`, `g`, `b`, `a` (0 to 1); out `success` | | Sets `textColor`. |
| `AN_GetUiTextSize` (pure) | `widget`; out `found`, `width`, `height`, `lines` | | Reads the last laid-out text size. |

Font-set loading, atlas settings, prewarm and glyph queries are C# only (they take strings), and the UI language
follows the culture nodes (`AN_SetCulture` and the others in [LOCALIZATION.md](LOCALIZATION.md)).

## Hosts

The engine lives inside `Aver.UI.Abi`. What only a host can do is open font files, name the font set and lend the
renderer the atlas, and `Runtime/include/aver/game/GameUiText.hpp` does that for both the editor's Play mode and
the packaged game:

* `uiTextInit(contentDir)` once, after the UI ABI is up: installs the loader, loads `Content/UI/Fonts.ocfonts`
  (or the built-in set) and follows the culture.
* `uiTextBindRenderer(renderer)` once the UI renderer exists.
* `uiTextReloadIfChanged(events)` from the editor's content watcher: `.ocfonts`, `.ttf`, `.otf` and `.ttc` reload.
* `uiTextShutdown()` before the device is torn down.

Project fonts (Content and `@exe/`) are mapped read-only and recorded in the file trace, so inside a package they
prove the package is self-contained. OS fonts are mapped read-only and deliberately **not** traced
(`aver::mapFileReadOnly`): a packaged game legitimately reads `C:\Windows\Fonts`, and `scripts/verify-game.ps1`
flags any traced open outside the package.

## Editors

* **Font Set editor** (`.ocfonts`, New Font Set in the Content Browser). Left: families, their chains (file,
  bold, italic, `lang`, scale, `dy`, index; reorder, remove), the default family, line gap and the per-language
  replacements. Right: **Preview** (Latin, kerning pairs, Greek, Cyrillic, Arabic, Hebrew, Japanese, Chinese, a
  mixed-direction line, combining marks, a missing glyph and a wrapped paragraph; size, bold, italic, direction,
  wrap width, a language box, and "tint by face" which colours each glyph by the face that drew it),
  **Coverage** (per open face: glyph count, kerning source, Unicode blocks present, and how many of the 141 Arabic
  presentation forms it has) and **Atlas** (the counters and the page images). Edits apply to the preview at
  once, before saving. Undo and redo are whole-document.
* **UI Layout editor**: when the project's font set has a TrueType face the canvas draws text with the game's
  engine (same fallback, shaping, wrapping), else with the editor's font. `fontFamily` is a combo of the family
  names. Glyph edges in the editor use straight alpha and can look marginally different from Play; Interact/Play
  is the reference.

The editor's own chrome (menus, inspector) still uses the editor font and is unchanged.

## Safety and licences

The font rasteriser is the `stb_truetype` header that ships with Dear ImGui, which states that it is not safe for
untrusted font files. Fonts therefore load only from the project's Content, the executable's folder and the OS
fonts folder, never from saves, the network or mod archives. A structural check (`uiValidateSfnt`: table
directory, required tables, sizes) runs first as defence in depth; it does not make outline parsing safe. Roboto
is Apache-2.0 and its notice ships with the game. The OFL is excluded by the project's font licence policy, so no
Noto bundle is shipped: CJK and Arabic on Windows come from the OS, and elsewhere from a font you add.

## Limits

* No shaping for Indic scripts, Thai, Lao, Khmer or Myanmar (they draw as separate glyphs and wrap at spaces).
* Arabic needs presentation forms in the face; fonts that rely on OpenType substitution only show unjoined
  letters. No GSUB features, no mark-to-base anchoring (marks use bounding boxes), no cursive attachment.
* Kerning is pair adjustment from `kern` or GPOS lookup type 2 (formats 1 and 2, X advance); extension lookups and
  other value formats are not read. A font with both uses GPOS only.
* The bidi implementation has no bracket-pair resolution and treats isolates as embeddings. Korean wraps at
  spaces. Vertical text, colour emoji, variable-font axes, synthetic bold, hinting, sub-pixel positioning and
  justified text are not supported.
* Text inputs move the caret in logical order (arrow keys do not follow visual order in mixed-direction text),
  typed characters still come from the host's key table, and there is no IME.
* OS fonts exist on Windows only for now; other hosts use project fonts.
* Sizes are rounded to whole pixels (4 to 160); larger text is drawn from a 160 px raster, scaled.

## Tests

All headless, none needs a GPU or a system font (a synthetic TrueType builder, `tests/ui/src/TestFont.hpp`, makes
fonts whose glyphs are rectangles, so order, kerning, shaping and fallback are checked by arithmetic):
`UiUnicodeTest` (decoding, tables), `UiBidiTest`, `UiShapingTest`, `UiLineBreakTest`, `UiFontTest` (validation,
metrics, kerning, font sets, fallback), `UiGlyphAtlasTest` (packing, recycling, overflow), `UiTextLayoutTest` (the
pipeline end to end), `UiTextTreeTest` (widgets measure and draw from one layout), `UiTextAbiTest` (the C ABI),
`UiRenderTest` (page upload and tokens), `FontSetEditorTest`, and the Aver Node tests in `UiTextNodeTests.cs`.
