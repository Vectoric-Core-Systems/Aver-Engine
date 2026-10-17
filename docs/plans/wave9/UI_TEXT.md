# Game UI text upgrade (runtime TrueType, fallback fonts, kerning, Arabic shaping, bidi, line breaking, styling)

Design contract for the implementing agent. Approved by the owner 2026-10-17 (approval relayed in the task text; this document is the contract, nothing in it is built). Rules in force: upgrade the existing systems (no parallel text path), D3D12 and Vulkan parity, no new third-party code (Dear ImGui's vendored `imstb_truetype.h` is the one allowed rasteriser; nothing else is added), lean comments, every runtime surface gets C# bindings and Aver Node nodes.

Every "exists today" claim below was verified by reading the file at the cited line on 2026-10-17. Nothing was built or run. Where a claim rests on a grep and not on running something, it says so.

## 0. Decisions in one screen

1. **Per-size alpha bitmaps, not SDF.** Glyphs are rasterised with `stbtt_MakeGlyphBitmapSubpixel` at an integer pixel size (4..160) and drawn 1:1 with integer-snapped origins. Crisp at the 12 to 48 px that game UI uses, needs no shader change (so D3D12 and Vulkan are identical by construction), and keeps the existing premultiplied `col * tex` pixel shader (`modules/render.ui/shaders/ui.hlsl:44-45`). SDF is rejected for v1 (section 2.2 has the reasons); large animated display text is the only place it would win and is a later option.
2. **All logic lives in `Aver.UI` (Core-only, no GPU, no file I/O)** so everything but the pixels is a ctest. Fonts reach it as byte blobs from host callbacks. The GPU half is `Aver.Render.UI`, which uploads dirty atlas pages from the existing `IRenderFeature::prePass` hook (a legal place for copies on both backends, proven by Voxi's GI-cache restore).
3. **One engine behind the existing seam.** `UiTextMetrics` and `UiPainter` (`UiText.hpp:15,61`) gain defaulted virtuals (`layoutText`, `drawText`); every existing implementer (test painters, the editor preview, `UiEstimatedMetrics`) keeps compiling and behaving. The baked `UiFont` stays as the last real fallback face and as the whole path when no TTF is available.
4. **Dynamic paged atlas, page-level LRU.** 512x512 pages, A8 on the CPU, RGBA8 premultiplied on the GPU, at most 16 pages (16 MB), shelf-packed, the least-recently-used page not used this frame is recycled whole. At most 256 glyph rasterisations per frame; a layout that could not finish is not cached and completes next frame (`prewarm` exists for loading screens).
5. **Kerning is stb's** (`kern` table, or GPOS PairPos format 1 and 2 X-advance only), cached per face. GSUB, GPOS marks and extension lookups are not read (limits stated in 2.3).
6. **Fonts are described by `.ocfonts`** (families, each an ordered chain): project faces, then optional Windows OS fonts (`SYSTEM auto` or named files, loaded lazily and memory-mapped), then the legacy baked font, then a procedural missing-glyph box. Per-language ordering (`lang=`) and per-language family overrides handle Han unification.
7. **Arabic shaping in-house**: joining types, isolated/initial/medial/final forms mapped to Unicode Presentation Forms (the font needs FE70..FEFC, which Arial, Segoe UI and Tahoma have), lam-alef ligatures, ZWJ/ZWNJ/tatweel, tashkeel placed from glyph bounding boxes. No GSUB.
8. **Bidi is a UAX #9 subset** (P2-P3, X1-X8 as plain embeddings/overrides without isolating run sequences, W1-W7, N1-N2, I1-I2, L1, L2, L4 mirroring; no N0 bracket pairing). **Line breaking is a UAX #14 subset** plus CJK break-anywhere and kinsoku. Both are table-driven, hand-authored ranges, tested headless.
9. **Styling**: `fontFamily`, `bold`, `italic`, `fontSize` (exists), `textAlign` (+ `start`/`end`), `textDir`, `lineSpacing`, `letterSpacing`, `ellipsis`, `maxLines`, `rich`, `wrap` (exists), `textColor` (exists). **Rich-text tags do not exist today** (grep of `modules/ui`, `docs/GAME_UI.md`, `docs/LOCALIZATION.md` finds none); v1 adds a deliberately small set (`<b> <i> <color=> <size=> <font=> <br>`, `<<` escape), off unless the widget sets `rich`.
10. **Layout is one cached object** (`UiTextLayout`) that both `measure` and `draw` read, so a widget is never measured one way and drawn another. It carries per-line caret stops for `TextInput`.
11. **Host glue is header-only per host** (like `GameUiInput.hpp`), shared by the editor and the packaged game. OS fonts are opened through a new untraced `platform::mapFileReadOnly` so `scripts/verify-game.ps1`'s isolation assertion (`:203-231`) does not flag `C:\Windows\Fonts`. Project fonts are traced.
12. **Existing defect fixed in phase 1**: the bitmap text path never decodes UTF-8 (`UiDrawList.cpp:111-113`, `UiFont.cpp:106-113` look glyphs up per byte), so `docs/LOCALIZATION.md:63`'s "covers Latin with accents, Greek and Cyrillic" is true of the atlas and false of the drawing. Found by reading; no test exercises a non-ASCII byte (grep of `tests/ui/src` finds none).
13. **Default look becomes scalable Roboto**: a packaged game appears to ship no HUD font at all today (no `Roboto-Regular.ocfont/.png` entry in `scripts/game.allowlist`, none in `stage-game.ps1`; `GameApp.cpp:788-791` treats that as legal; grep-verified, no package was run). The default font set stages `Roboto-Regular.ttf` and `Roboto-Medium.ttf` (Apache-2.0) plus the baked pair with the game and the licence notice.
14. **Not in v1**: Indic/Thai/Khmer/Myanmar shaping, vertical text, colour emoji, variable-font axes, synthetic bold, hinting, sub-pixel positioning, justify, GSUB features, OpenType mark positioning, N0 bracket pairing, full isolates, IME composition, the ImGui editor font (the editor's own chrome is unchanged).

## 1. What exists today

| Fact | Where (file:line) | Consequence |
|---|---|---|
| Game text is a baked bitmap: `UiGlyph` (uv, w, h, offX, offY, advance), `UiFont` (one `pixelSize`, ascent, descent, lineHeight, `atlasTexture`, glyph map) | `modules/ui/include/aver/ui/UiFont.hpp:34-60` | The legacy face. Kept. |
| `.ocfont` parse; "a unknown codepoint contributes nothing" | `UiFont.cpp:125-184`, `UiFont.hpp:55` | No missing-glyph box today. |
| Width and draw look glyphs up **per byte**, not per code point | `UiFont.cpp:106-113` (cast at `:110`), `UiDrawList.cpp:106-124` (cast at `:113`) | Multi-byte UTF-8 draws mojibake. Fixed in phase 1. |
| One font size, scaled per glyph | `UiDrawList.hpp:81` (`addTextScaled`), `UiText.cpp:176-180` (`UiDrawListPainter::text`) | Soft at any size but 16. |
| Metrics/painter seam | `UiTextMetrics` (`UiText.hpp:15`), `UiFontMetrics` (`:32`), `UiPainter` (`:61`), `UiDrawListPainter` (`:76`), `uiWrapText` (`:45`) | The seam the engine plugs into. |
| Wrapping splits on `' '` only; a word wider than the line breaks between code points | `UiText.cpp:25-60` | CJK/Arabic/URLs wrap badly. Replaced by the engine; kept for the legacy path. |
| Only three implementers outside `modules/ui` | `sandbox/src/UiLayoutEditor.cpp:497` (`PreviewMetrics`), `:508` (`PreviewPainter`), `tests/ui/src/UiLocalizeTest.cpp:50` (`RecordingPainter`) | New virtuals must default so these compile unchanged. |
| Text sizing: `fontPx = dp * scale_` | `UiTreeLayout.cpp:45-48` | The one choke point; the engine rasterises at `round(px)`. |
| Measure for Text/Button/Toggle/Choice/List/TextInput | `UiTreeLayout.cpp:84-136` | Switch to `layoutText`. |
| Draw: `drawBlock(lines, px, align)` and per-kind `p.text` calls | `UiTreeDraw.cpp:37-50,120-132,158-164,177-181,202-203,213-216,234,246-262,274-275` | Switch to `drawText(layout, ...)`. |
| TextInput caret/hit-test by prefix width | `UiTreeInput.cpp:316-341` (`displayText`, `textShift`, `caretFromX`) | Switch to layout caret stops. |
| Widget text props: `text`, `textAlign`, `fontSize`, `wrap` | `UiWidget.hpp:51-54`; table `UiProps.cpp:262-265` | Add props beside them. |
| `UiTextAlign { Left, Center, Right }` | `UiTypes.hpp:105` | Append `Start`, `End`. |
| Theme: `baseFontSize 18`, `UiStyle::fontSize` | `UiStyle.hpp:33,27` | Add `fontFamily`, `bold` to `UiStyle`. |
| Draw list vertex = x,y,u,v,rgba (20 bytes), `UiDrawCmd::texture` u64, merging by texture+clip | `UiDrawList.hpp:13,28-31`; merge `UiDrawList.cpp:append` | Atlas pages become texture tokens; consecutive same-page glyphs already merge. |
| Renderer: premultiplied blend, linear+clamp sampler, texture u64 cast to `TextureHandle` | `UiRenderer.cpp:97-98,104,183,216`; shader `ui.hlsl:44-45` | RGBA8 premultiplied atlas works with no shader change. |
| Renderer hooks only `overlayPass`; `submit` copies the list | `UiRenderer.hpp:22,29,51`; `UiRenderer.cpp:170,189` | Add `prePass` + token translation. |
| `prePass` runs from `beginFrame`, before the scene targets are bound, on both backends | `RHIResources.hpp:728`; `D3D12Device.cpp:4265`; `VulkanDevice.cpp:3142` (Vulkan opens its own scope for it: `VulkanCommon.hpp:1747`) | The upload point. |
| Buffer-to-texture copy exists; **no sub-region copy, no texture write** | `RHIResources.hpp:633` (`copyBufferToTexture(dst, mip, src, offset)`), footprint `:122-133,554` | Dirty pages upload whole (hence small pages). |
| Proven copy pattern on both backends: footprint, `writeBuffer` row by row, barrier SR->CopyDest, copy, barrier back, destroy staging | `VoxiRenderer.cpp:2839-2864` | Copy this pattern. |
| UI frame is built in `onUpdate` before `beginFrame`; submit is in `onRender` | `SandboxApp.cpp:1510` (`onUpdate`), `:2162-2204` (begin frame, set font, `aver_ui_widgets_frame`); `SandboxRender.cpp:2060` (`submitGameUi`); `GameApp.cpp:1975,2067,2085,2219-2225` | All rasterisation precedes that frame's `prePass`. |
| Hosts load the baked font and lend it every frame | `SandboxApp.cpp:285-330,2165`; `GameApp.cpp:781-828,2070,2341-2350` | Replace by shared glue; keep the lend. |
| Both hosts create `UiRenderer` and register it | `SandboxApp.cpp:1185-1186`; `GameApp.cpp:771-773` | Bind the atlas there. |
| `Aver.UI.Abi` links only Core + UI; the font is lent as an opaque pointer and read only | `ui_abi.h:70`; `UiAbi.cpp:113,130,139`; `ui.abi/CMakeLists.txt` | Engine singleton lives in the DLL; hosts get an opaque pointer (the draw-list precedent `ui_abi.h:128`). |
| `aver_ui_widgets_frame` draws with `UiDrawListPainter(drawList, font)` | `UiWidgetAbi.cpp:78` | Gets the engine. |
| Runtime library "links NO UI" in Core; the UI link is on `Aver.Runtime.Game` only; `GameUiInput.hpp` is header-only per host | `Runtime/CMakeLists.txt:15-20,233-239`; `GameUiInput.hpp:1-8` | Host glue is header-only too. |
| File API: `readFileBytes/Text` call the open trace; `traceFileOpen` | `modules/platform/include/aver/platform/FileSystem.hpp:64-78` | OS fonts must bypass it (below). |
| Packaged-game isolation assertion fails on any traced open outside the package | `scripts/verify-game.ps1:203-231` | OS-font opens must not be traced. |
| Roboto `.ttf` excluded from games because only ImGui loaded them | `scripts/game.allowlist:124`; `docs/PACKAGING.md:220`; licence/notice rows `stage-payload.ps1:375` | Default fonts get allowlisted. |
| Font licence policy excludes OFL | `third_party/fonts/README.md:28-29` | No Noto bundle (open question 1). |
| `imstb_truetype.h` v1.26 is vendored; ImGui compiles it `STBTT_STATIC` only under `IMGUI_ENABLE_STB_TRUETYPE` (off in `imconfig.h:104`) | `third_party/imgui/imstb_truetype.h:1-5,11` (security warning); `imgui_draw.cpp:140-164` | Aver.UI needs its own implementation TU; `STBTT_STATIC` avoids duplicate symbols with Sandbox's ImGui. |
| stb: TTC (`GetFontOffsetForIndex`), CFF/CID-keyed OTF (`fdselect`), composite glyphs, subpixel bitmaps, `stbtt_GetGlyphSDF` | `imstb_truetype.h:709,735,1414,922-925,953` | OTF CJK fonts load. No variable-axis instancing (`third_party/fonts/README.md` says the same). |
| stb kerning: `GetGlyphKernAdvance` uses GPOS **if present**, else `kern`; GPOS = lookup type 2 only, formats 1 and 2, `valueFormat1 == 4 && valueFormat2 == 0` (X advance), no extension lookups, no feature/script selection | `imstb_truetype.h:2501-2625` | Defines exactly what "kerning" means in v1. |
| Node pins are only float/int/bool/exec; strings are NODE attributes | `GraphGameSystemNodes.cs` (`Args` doc: `p:` pin, `@attr`, `@attr?`, `>out`); `docs/plans/wave2/DATA_TABLES_LOCALIZATION.md:22` | Family name rides an attribute. |
| UI node rows go through one dispatcher with a fixed 5-input shape | `GraphGameSystemNodeTable.cs:50-63`; `UiGraphInterop.cs:23-48` | New nodes get their own owner class and rows (the per-feature pattern, `GraphMotionMatchNodeTable.cs`). |
| Per-feature node tables concatenated on one shared line; palette mirror included from one shared header | `GraphGameSystemNodeTable.cs:121`; `GraphNodeDefs.hpp:115`; pattern file `GraphNodeDefsMotionMatch.hpp`; tests `MotionMatchNodeTests.cs`, `Program.cs:280` | Pattern for 2.13. |
| `Hud.Text`, `MeasureText`, `TextCentred` exist; **`docs/SCRIPTING_API.md:638-640,878-881` still say "There is no text"** | `scripting/csharp/Aver.UI/Hud.cs:127,139,144,149` | Stale docs corrected in phase 6. |
| `Widget.Set(property, value)` goes through the property table, `Text` is typed | `Widgets.cs` (`Widget.Set`, `Text`) | New props are reachable from C# the moment they are in the table; typed wrappers are added on top. |
| Typed text uses a US key table; no `WM_CHAR` plumbing | `docs/GAME_UI.md:408-409` | Typing Arabic/CJK is out of scope; display of such text in a TextInput is in scope. |
| Another wave-9 feature (accessibility) multiplies `fontPx`, adds `UiProps` row `accessName`, edits `UiTree.hpp`, `ui_widget_abi.h`, `GameApp.cpp`, `SandboxApp.cpp`, `GraphGameSystemNodeTable.cs:121` | `docs/plans/wave9/ACCESSIBILITY.md:174-179,332-361` | Merge-both hunks; see section 3 shared rules. |

Not present, verified by grep: any glyph-atlas upload path in `Aver.Render.UI` (only the 1x1 white texel, `UiRenderer.cpp:49-62`), any rich-text parser, any bidi/shaping/line-break code in `modules/ui`.

## 2. Design

### 2.1 Architecture and frame flow

```
host (once)   aver_ui_text_set_loader -> aver_ui_text_load_fontset(<Content/UI/Fonts.ocfonts or built-in default>)
                                       -> aver_ui_text_set_language_fn(&aver_fw_loc_culture)
                                       -> UiRenderer::setTextAtlas(aver_ui_text_atlas())
per frame     onUpdate : aver_ui_begin_frame -> HUD scripts / widgets -> UiTree::layout+draw
                           UiTextEngine::layout()  (cache; rasterises <= glyphBudget new glyphs into CPU pages)
                           UiTextEngine::emit()    (glyph quads -> UiDrawList, texture = atlas page token)
              beginFrame -> UiRenderer::prePass : upload dirty pages (barrier, copyBufferToTexture, barrier)
              onRender  : UiRenderer::submit(list)  ; overlayPass : tokens -> page textures, draw as today
```

Modules: pure logic in `modules/ui` (files in section 3); GPU in `modules/render.ui`; C ABI in `modules/ui.abi`; host glue in `Runtime/include/aver/game/GameUiText.hpp` (header-only per host); file mapping in `modules/platform`. Single-threaded: the engine is called from the game thread only.

The engine singleton lives inside `Aver.UI.Abi.dll` (like the draw list, `UiAbi.cpp:10`). Native hosts reach it through `aver_ui_text_atlas()` (opaque pointer, read like `aver_ui_draw_list()`), they never construct one.

### 2.2 Rasterisation and atlas (SDF versus bitmaps)

**Decision: per-size alpha bitmaps.** Reasons, each checkable:
- *Sharpness where UI lives.* UI body text is 12 to 28 device px. Bitmaps at the exact pixel size with integer origins are as sharp as an unhinted rasteriser gets; single-channel SDF at those sizes rounds corners and blurs stems. (MSDF would fix corners but is a new algorithm and a multi-channel atlas.)
- *No pipeline change.* SDF needs a smoothstep pixel shader and a per-draw mode; the UI vertex is 20 bytes with no spare field (`UiRenderer.cpp:91` static_asserts it), so it would mean a second pipeline and a second draw path in `UiRenderer` on both backends. Bitmaps ride the shipped pipeline.
- *CJK cost.* stb's SDF is computed per pixel against every edge; my estimate (not measured) is several hundred microseconds per 32 px glyph versus tens for a coverage bitmap, which is the difference between a CJK dialog appearing instantly and over a second. Measure in phase 3 with `--gpu-timing`-free CPU timers if the owner doubts it.
- *Costs accepted*: one raster per (face, glyph, size). Mitigated by integer size quantisation, the LRU, and that UI sizes are few. Very large glyphs (>160 px) are drawn from a 160 px raster scaled by the sampler.

**Raster size** `rasterPx = clamp(round(px), 4, 160)` where `px = dp * scale` (the tree's `fontPx`). Layout, caret and draw all use `rasterPx`, so measured width equals drawn width; widgets whose real `px` is fractional differ from their ideal by under half a pixel of font size. Quad scale = `px / rasterPx` only when `px > 160`.

**Em scale** `s = stbtt_ScaleForMappingEmToPixels(font, rasterPx)` (the UI's `fontSize` is an em size, as the baked Roboto 16 was). Pen advances are fractional floats (`hmtx * s + kern * s + letterSpacing`); each glyph's bitmap origin is `round(pen + bitmapLeft)` and `round(baseline + bitmapTop)`; ascent, descent and lineGap are rounded to whole pixels so baselines stay on the grid.

**Bitmap**: `stbtt_GetGlyphBitmapBoxSubpixel` + `stbtt_MakeGlyphBitmapSubpixel` with shift 0, then a 1 px transparent gutter on every side (linear sampling stays exact at 1:1; 2 px when `px > 160`). Output is A8 coverage. An optional `coverageGamma` (default 1.0, in `UiTextConfig`) is applied by a 256-entry LUT during the A8 to RGBA8 expansion at upload; the headed pass may tune it (open question 6).

**Pages** (`UiGlyphAtlas`): `pageSize` (default 512, 256..2048), `maxPages` (default 16). CPU page = A8 plus shelf list; shelf packing: first shelf with `height >= h && height <= h + 4` and room, else a new shelf if the page has room, else next page. GPU texture = `RGBA8Unorm`, `(c,c,c,c)` per texel (premultiplied white), created lazily on first upload.

**Key** `UiGlyphKey { u16 face; u16 rasterPx; u32 glyph; }` (hash map to `UiAtlasGlyph { u16 page, x, y, w, h; i16 offX, offY; }`; offY is down from the baseline, `UiGlyph`'s convention).

**LRU is page-level.** `lookup` stamps `page.lastUsedFrame = frame`. When `insert` finds no room and `pageCount == maxPages`, it recycles the page with the smallest `lastUsedFrame` **strictly older than the current frame**: all its resident keys are erased, shelves reset, `generation++`, whole page marked dirty. If every page was used this frame, `insert` returns null; the engine counts `overflow`, draws nothing for that glyph, logs once per 5 s, and does **not** cache the layout (so it retries next frame). A recycled page may still be read by an earlier frame's draw already queued on the GPU; that is safe because the new content is a later copy on the same queue ordered by the barriers (the same argument the Voxi restore relies on).

**Budget**: `glyphBudget` (default 256) new rasterisations per frame. When spent, remaining misses are skipped, the layout is flagged `incomplete` and not cached. `UiTextEngine::prewarm(text, style)` ignores the budget (loading screens, `aver_ui_text_prewarm`).

**Draw list tokens**: atlas page `i` appears in `UiDrawCmd::texture` as `kUiAtlasTokenBit | i` where `kUiAtlasTokenBit = 1ull << 63`. `rhi::TextureHandle` is `u32` (`RHIResources.hpp:15`) so tokens cannot collide. Legacy bitmap fonts keep their real handle in `UiFont::atlasTexture`.

### 2.3 Fonts, faces, validation, metrics, kerning

`UiFontFace` (one `stbtt_fontinfo`, holds a `UiFontBlob { const u8* data; usize size; std::shared_ptr<void> owner; }`):
- `open(blob, index, err)`; `index` selects a TTC member via `stbtt_GetFontOffsetForIndex`.
- `glyphFor(cp)` (`stbtt_FindGlyphIndex`, 0 = missing), `advance(glyph, px)`, `kern(g1, g2, px)`, `glyphBox(glyph)` (unscaled, for mark placement), `rasterize(glyph, rasterPx, out)`, `unitsPerEm`, `ascent/descent/lineGap` in px from `hhea` (`stbtt_GetFontVMetrics`); if `hhea` ascent and descent are both 0, use OS/2 typo (`stbtt_GetFontVMetricsOS2`).
- **Kerning** = `stbtt_GetGlyphKernAdvance` (GPOS PairPos 1 and 2 X-advance if the font has GPOS, else `kern` format 0). Results are memoised per face in a pair map. **Limits, stated**: a font that has a GPOS table but only expresses kerning through extension lookups (type 9), other value formats, or non-`kern` features gets no kerning; a font with both GPOS and `kern` uses GPOS only (`imstb_truetype.h:2619-2622`). No GSUB (no ligatures except the Arabic ones in 2.5), no GPOS mark/cursive.
- **Skipped for**: Arabic-script clusters (cursive), and nothing else; `letterSpacing` adds to advance after kerning.
- No synthetic bold. `bold` picks a face with `weight=bold`; if none exists the regular face is used. `italic` picks an italic face, else shears the quad by 0.2 (a small `UiDrawList::addGlyphQuad(..., skew)` addition; the atlas is shared).
- No hinting, no sub-pixel positioning, no variable-axis instancing.

**Safety.** The vendored header says `NO SECURITY GUARANTEE -- DO NOT USE THIS ON UNTRUSTED FONT FILES` (`imstb_truetype.h:11`). Policy: faces load only from the project's Content, the engine directory, or the OS fonts directory; never from saves, network, or mod archives. As defence in depth `uiValidateSfnt(data, size, index, err)` runs before `stbtt_InitFont`: magic (`0x00010000`, `OTTO`, `true`, `ttcf`), table directory entries inside the buffer and non-overlapping count <= 64, required tables present (`cmap head hhea hmtx maxp` plus `glyf+loca` or `CFF `), every table `offset + length <= size`, `head.unitsPerEm` in 16..16384, `numGlyphs > 0`, `hhea.numberOfHMetrics <= numGlyphs`, `loca` length consistent with `numGlyphs`, `cmap` subtable offsets inside `cmap`. It cannot make glyph-outline parsing safe and the document says so; the validator test mutates a synthetic font and runs the validator only (never rasterises a mutated file).

**Blobs and memory.** `UiFontLoader` is a struct of callbacks (the C ABI form is 2.10): `open(kind, name, UiFontBlob&)` with kind `Content`, `System`, `Engine`. The host maps files read-only (`platform::mapFileReadOnly`, 2.11), so a 20 to 30 MB CJK `.ttc` costs address space and only the touched pages. OS faces beyond the first matching one are opened lazily the first time a missing glyph needs them and stay open.

### 2.4 Font sets, fallback chain, missing glyph

`.ocfonts` (OC text dialect: `#` comments, `KEY value`, unknown keys skipped, a bad value is an error naming the line, `write(parse(write(x))) == write(x)`):

```
OCFONTS 1
DEFAULT Body
FAMILY Body
FACE   Fonts/Inter-Regular.ttf
FACE   Fonts/Inter-Bold.ttf weight=bold
FACE   Fonts/Inter-Italic.ttf italic
FACE   Fonts/Mono.ttf index=0 scale=0.95 dy=0.5     # TTC member, size multiplier, baseline shift (px at 16)
FACE   @exe/Roboto-Regular.ttf                       # next to the executable
SYSTEM auto                                          # built-in Windows candidates per script, all optional
SYSTEM msyh.ttc index=0 lang=zh-Hans
BITMAP legacy                                        # the font lent with aver_ui_set_font
LINEGAP 1.0                                          # multiplier on the primary face's line gap
FAMILY Heading
FACE   Fonts/Display.ttf
OVERRIDE ja Body BodyJa                              # UI language ja (or ja-*): "Body" means "BodyJa"
```
Rules:
- Face order in a family is priority order. `weight=regular|bold`, `italic`, `lang=<tag>` (prefix match on the UI language), `scale`, `dy`, `index` are the only attributes.
- **Selection for a code point under (bold, italic)**: pass 1 walks the family's entries whose bold/italic flags equal the request (project, then SYSTEM, then BITMAP); pass 2 walks the rest. Among SYSTEM entries, those whose `lang=` matches the current language are tried first (Han unification: ja prefers a Japanese face, zh-Hans a Simplified one). The first face with `glyphFor(cp) != 0` wins. The decision is memoised per (family, bold, italic, cp) including negative results; the memo is dropped on font-set load or language change.
- `SYSTEM auto` expands, in code (`UiFontSet.cpp`, a static table), to optional Windows file names: Latin/Greek/Cyrillic/Arabic/Hebrew `segoeui.ttf tahoma.ttf arial.ttf`; symbols `seguisym.ttf`; ja `YuGothM.ttc meiryo.ttc msgothic.ttc`; ko `malgun.ttf`; zh-Hans `msyh.ttc simsun.ttc`; zh-Hant `msjh.ttc`; Devanagari `Nirmala.ttf`; Thai `LeelawUI.ttf`. A name the host cannot open is skipped silently. Non-Windows hosts return "cannot open" for kind System in v1 (so macOS/Linux builds use project fonts only).
- **Zero-config default** (built into the host glue as text, used when `Content/UI/Fonts.ocfonts` is absent): `DEFAULT Default`, family `Default` = `FACE @exe/Roboto-Regular.ttf`, `FACE @exe/Roboto-Medium.ttf weight=bold`, `SYSTEM auto`, `BITMAP legacy`.
- If no face can be opened at all, only the bitmap face (or nothing, as today) remains and the engine reports `usable() == false` for TTF so callers keep today's behaviour.
- **Missing glyph**: when no chain face has the code point and it is not a space, control, format (ZWJ/ZWNJ/bidi marks/variation selectors) or tag character, the layout emits a *box glyph*: a hollow rectangle `0.55 em x 0.7 em`, stroke `max(1, round(px/16))`, advance `0.6 em`, drawn with `addRect` on the white texel (no atlas, no face). It is the visible answer to "your font set cannot show this".
- Family name lookup is case-insensitive; an unknown family name uses `DEFAULT`; `fontFamily` empty uses the style's family, then `DEFAULT`.
- A reload (`.ocfonts` or a font file changed) calls `loadFontSet` again: faces closed, memo and layout cache dropped, atlas contents cleared (pages kept, all marked dirty).

### 2.5 The text pipeline (contract for `UiTextEngine::layout`)

Input: UTF-8 `shown` text, `UiTextStyle`, `UiTextBox`. Steps, in order, each a separately tested pure function where noted:

1. **Decode** (`uiDecodeUtf8`): strict; each invalid byte becomes U+FFFD (overlongs, surrogates, > U+10FFFF, truncated sequences). Keep `cpToByte`. Caps: 16,384 code points per layout (excess dropped, logged once), 512 lines, 32,768 glyphs.
2. **Rich text** (if `box.rich`): strip tags into spans (2.8); caret/offset math never sees tags (rich is not used on `TextInput`).
3. **Paragraphs**: split at U+000A, U+000D (CRLF as one), U+0085, U+2029; U+2028 is a line break inside the paragraph. Tab = 4 space advances.
4. **Bidi** (`uiBidiResolve`, pure): paragraph level from `dir` (LTR 0, RTL 1) or, for Auto, the first strong L/R/AL (UAX #9 P2/P3; none -> 0). Implemented: X1-X8 with embeddings/overrides/isolates all treated as embeddings (depth cap 125, overflow ignored), W1-W7, N1-N2, I1-I2, L1 (trailing whitespace and separators reset to the paragraph level) at line time, L2 reordering at line time, L4 mirroring by the table in `uiMirror` (parentheses, brackets, braces, `< >`, guillemets, and the other common pairs). **Not implemented**: N0 (bracket pair resolution), true isolating run sequences. `LRM RLM ALM` are strong, zero width. Output: `levels[cp]`.
5. **Mirror and Arabic shaping** (`uiShapeArabic`, pure): applied to maximal ranges of Arabic-script code points, Arabic marks, ZWJ/ZWNJ and tatweel, in logical order, ignoring face boundaries.
   - Joining types from a hand-written range table (UCD ArabicShaping facts; no UCD file is vendored): D dual, R right, L left, U non, C causing (tatweel U+0640, ZWJ), T transparent (Mn marks 064B-065F, 0670, 06D6-06DC, 06DF-06E4, 06E7-06E8, 06EA-06ED, 08D3-08E1 and similar). ZWNJ breaks joining; ZWJ forces it.
   - Form = f(previous joining side, next joining side) skipping T: isolated / final / initial / medial.
   - Forms map to Presentation Forms-B `FE70..FEFC` and the Persian/Urdu letters (U+067E 0686 0698 06A9 06AF 06CC and the other common extended letters) to their `FB50..FDxx` forms, from a table of ~90 rows.
   - **Lam-alef**: U+0644 followed (skipping T) by U+0622/0623/0625/0627 becomes the single ligature code point `FEF5/FEF7/FEF9/FEFB` (isolated) or `FEF6/FEF8/FEFA/FEFC` (final, when the lam joins from the left neighbour). The cluster spans both source code points (`srcCount = 2`).
   - Output `UiShapedCp { u32 cp; u32 src; u8 srcCount; u8 flags }`. A code point with no Presentation Form row keeps its own value.
   - **Font dependency, stated**: lookup of the shaped code point goes through the face's `cmap`. If a face lacks a presentation form but has the base letter, the base letter's glyph is used and the cluster is marked `unjoined` (readable, not connected). Arial, Segoe UI and Tahoma carry `FE70..FEFC`; fonts that only ship GSUB-driven forms (many modern OTFs) will show unjoined letters. The font-set editor's coverage tab shows this per face. In-house GSUB reading is the v2 path.
6. **Itemise**: walk clusters (base + following marks `uiIsCombining`/Arabic T). For each cluster choose a face by the cluster's first code point (2.4); marks follow their base's face when it has the mark glyph, else choose independently. A run ends where face, bidi level, rich span, or script class (Arabic versus other) changes.
7. **Glyphs**: per run: glyph ids, advances (`rasterPx` em scale), kerning between adjacent same-face glyphs of a non-Arabic run, `letterSpacing` added (ignored in Arabic clusters), `scale`/`dy` of the face entry applied.
8. **Marks**: zero advance. Placement uses `glyphBox`: horizontally the mark's box centre goes to the base's box centre; vertically, for marks above (fatha, damma, shadda, sukun, madda, dammatan, fathatan, Hebrew points above) the mark's box bottom is lifted to `base.top + 0.06 em` if it would overlap, stacking the next above-mark on the previous mark's top; below marks (kasra, kasratan, subscript alef, Hebrew points below) mirrored downwards. Latin combining marks 0300..036F use the same rule when the base face has no precomposed glyph. This is "basics": no per-glyph anchors.
9. **Line breaking** (`uiLineBreaks`, pure): `breaks[i]` = boundary before code point `i`: `No`, `Allowed`, `Mandatory`.
   - UAX #14 subset by class: BK CR LF NL (mandatory), SP, ZW, WJ, GL, CM (attach), OP CL CP QU EX IS SY NS BA BB HY B2 ID CJ AL NU PR PO IN HL, everything else AL. Rules implemented: LB4-LB8, LB9-LB10 (combining marks), LB11-LB13, LB14-LB17, LB18, LB19, LB21-LB25, LB28-LB31. Not implemented: LB20 (CB), LB26-LB27 (Hangul jamo; Hangul syllables are AL, so Korean wraps at spaces only), LB30 beyond the OP/CP East Asian carve-out, tailoring. Soft hyphen is a break opportunity that renders nothing.
   - **CJK**: ideographs, kana, CJK symbols and fullwidth forms are ID: a break is allowed between any two of them unless a kinsoku rule forbids it. `uiCannotStartLine`: closing brackets and quotes `)]}、。，．：；！？」』）】〕〉》”’`, long vowel mark `ー`, small kana (`ぁぃぅぇぉっゃゅょゎ`, katakana equivalents), iteration marks `々〻`, `…` `‥` and fullwidth `！？` family. `uiCannotEndLine`: opening brackets and quotes `([{「『（【〔〈《“‘`.
   - Thai, Lao, Khmer, Myanmar are AL (break at spaces only), documented.
10. **Fill lines** (greedy): accumulate cluster widths; trailing spaces hang (not counted in the fit test, excluded from the line width); at an `Allowed` boundary where the next segment would overflow `box.maxWidth`, break there. A single segment wider than the box breaks between clusters (never between a base and its marks or inside a ZWJ/lam-alef cluster), at least one cluster per line. **Kinsoku push-down**: if the chosen break would put a `cannotStartLine` code point first on the next line, or a `cannotEndLine` last on this one, move the break one cluster earlier (twice at most, never leaving a line empty). `Mandatory` breaks always break. `box.wrap == false` keeps one line per paragraph (hard breaks still break).
11. **Ellipsis / maxLines**: with `maxLines > 0` and text remaining after that line, or with `ellipsis` and a single unwrapped line wider than `maxWidth`, the last kept line is truncated at a cluster boundary in *logical* order until `…` fits (U+2026; if no face has it, three U+002E); the ellipsis therefore lands at the visual end for the paragraph's direction.
12. **Per line**: apply L1, reorder runs by levels (L2, `uiBidiReorderLine`), reverse glyph order inside odd-level runs, accumulate x. Line metrics: `ascent = max(ascent of faces used, primary ascent)`, `descent` likewise, `height = (ascent - descent + lineGap) * lineSpacing`, rounded to integers. Records `rtl` (paragraph level odd).
13. **Caret stops** (`UiTextLayout::caretX`, `indexAt`): per line, one stop per cluster edge in logical order; a code point inside a multi-code-point cluster (ligature) snaps to the cluster start; index == end of line -> trailing edge of the last logical cluster. `indexAt(x)` picks the nearest stop. TextInput moves the caret in **logical** order in v1 (arrow keys do not follow visual order in bidi text; documented).
14. **Cache**: key = `fnv1a64` over (text bytes, family hash, rasterPx, flags, dir, lineSpacing, letterSpacing, scale, maxWidth quantised to 1/4 px, wrap/ellipsis/maxLines/rich, `fontSetEpoch`, `languageEpoch`). 1024 entries, LRU by frame. A layout flagged `incomplete` (glyph budget or atlas overflow) is returned but not stored.

**Atlas coupling**: `layout` resolves glyphs and ensures rasterisation (counting against the budget); `emit` only looks glyphs up and writes quads (a lookup that misses -> skip the glyph and flag `incompleteDraw`, never rasterise in `emit`).

### 2.6 The layout object and the seam

```cpp
// UiTextLayout.hpp
struct UiGlyphRef { u32 glyph; u16 face; u16 rasterPx; f32 x, y; u16 color; u8 flags; }; // x from line origin, y from baseline; color indexes layout.palette (0 = caller colour)
struct UiTextLine { u32 glyphBegin, glyphEnd, srcBegin, srcEnd; f32 width, ascent, descent, top; bool rtl; };
struct UiTextLayout {
    std::string source;                       // the shown text
    std::vector<UiGlyphRef> glyphs;
    std::vector<UiTextLine> lines;
    std::vector<u32> palette;                 // rich colours
    f32 width = 0, height = 0, px = 0;        // widest line, total height
    bool incomplete = false, legacy = false;  // legacy: built from the 3 old virtuals
    std::string_view lineText(usize i) const;
    f32 caretX(usize cpIndex) const;  usize indexAt(f32 x, f32 y) const;
};
struct UiTextStyle { std::string_view family; f32 px; f32 scale = 1; bool bold = false, italic = false;
                     UiTextDir dir = UiTextDir::Auto; f32 lineSpacing = 1; f32 letterSpacing = 0; };
struct UiTextBox   { f32 maxWidth = 0; bool wrap = false, ellipsis = false, rich = false, richColors = true; u32 maxLines = 0; };
```
`UiTextDir { Auto, LTR, RTL }` lives in `UiTypes.hpp`.

`UiText.hpp` additions (defaulted, so nothing existing changes):
```cpp
// UiTextMetrics
virtual std::shared_ptr<const UiTextLayout> layoutText(std::string_view text, const UiTextStyle&, const UiTextBox&) const;
// default: lines from uiWrapText + the three legacy virtuals; legacy = true.
// UiPainter
virtual void drawText(const UiTextLayout&, const UiRect& area, UiTextAlign align, bool centreVertically, u32 rgba);
// default: per line, p.text(x, baseline, lineText, layout.px, rgba) with the old drawBlock maths.
```
`drawText` owns alignment: per line `x` from `area`, `line.width` and `align` (`Left/Center/Right` physical; `Start/End` logical, flipped when `line.rtl`). `UiEngineMetrics : UiTextMetrics` (engine-backed) implements the three legacy virtuals from the default family's primary face at the requested size, plus `layoutText`. `UiDrawListPainter` gains a constructor `(list, UiTextEngine*, const UiFont* bitmap)` and overrides `drawText` to call `engine->emit`. Existing `UiDrawListPainter(list, font)` is unchanged.

`UiTree` changes (all in files `UiTree.hpp`, `UiTreeLayout.cpp`, `UiTreeDraw.cpp`, `UiTreeInput.cpp`):
- New private `UiTextStyle textStyleOf(const UiWidget&, const UiStyle&) const` and `UiTextBox textBoxOf(const UiWidget&, f32 wrapWidth) const` (family: widget -> style -> default; bold/italic: widget OR style; `letterSpacing * scale_`; `richColors` follows the accessibility flag when that feature lands).
- `measure` (Text, Button, KeyBind, Toggle, Choice, List, TextInput): `layoutText` once per string; `cw = layout->width`, `ch = layout->height`. Wrap uses the existing `wrapW` rule (`w.wrap && w.width >= 0`).
- `drawBlock` becomes `drawLayout(p, layout, rect, align, centreV, colour)` calling `p.drawText`. The arrows in Choice (`"<"`, `">"`) stay as `p.text`.
- TextInput: `textShift` and `caretFromX` use the layout's `caretX`/`indexAt`; `displayText` (masked `*`) unchanged.
- `UiTree::lastTextSize(UiWidgetId, UiVec2& size, i32& lines)` returns the layout cached by the last `measure` (feeds `aver_ui_widget_text_size`).

### 2.7 Widget, style and property changes

`UiWidgetProps` (content block, after `wrap`): `std::string fontFamily; bool bold=false, italic=false; UiTextDir textDir=Auto; f32 lineSpacing=1, letterSpacing=0; bool ellipsis=false; i32 maxLines=0; bool rich=false;`. `UiStyle`: `std::string fontFamily; i8 bold=-1` (-1 inherit). `UiTextAlign`: append `Start, End` (values 3 and 4; existing numbers unchanged).

`UiProps.cpp` rows (group `kCont`, kind mask `kTextual` unless noted), after the `wrap` row: `fontFamily` (string), `bold`, `italic` (bool), `textDir` (enum `auto ltr rtl`), `lineSpacing` (float 0.5..3), `letterSpacing` (float -10..50), `ellipsis` (bool), `maxLines` (int 0..100), `rich` (bool; Text, Button, Toggle, Choice, List kinds, not TextInput). `textAlign` enum names become `left center right start end`. Because the table feeds the `.ocui` reader/writer, the inspector and `aver_ui_widget_set_prop`, all three get the properties; the writer only emits non-default values and older builds skip unknown `SET` keys (`GAME_UI.md:139`), so new files load in old builds minus styling. No `UiLayoutAsset` change and no `OCUI` version bump.

### 2.8 Rich text (new, small)

`UiRichText.hpp`: `parseRich(std::string_view in, std::u32string& plain, std::vector<UiRichSpan>& spans)` and `std::string uiStripRichText(std::string_view)` (pure; the accessibility feature's speech path should call the latter). Tags (case-insensitive, nesting by a stack, unclosed tags close at the end): `<b></b> <i></i> <color=#RRGGBB[AA]></color> <size=N></size>` (design pixels; multiplied by `style.scale`) `<font=Family></font> <br>`; `<<` is a literal `<`. Anything that is not exactly one of those tags (including `a < b`, `<3`) is literal text. Spans carry (bold, italic, colour index, px, family). With `richColors = false` colour spans are ignored (the accessibility high-contrast mode). `rich` defaults to false so existing strings, including localized ones containing `<`, are unaffected.

### 2.9 GPU: `Aver.Render.UI`

- `UiRenderer::setTextAtlas(aver::ui::UiGlyphAtlas*)` (non-owning; hosts call it after `create` and pass `nullptr` before teardown).
- `UiRenderer` also overrides `prePass(IRenderContext&)`. For each atlas page with `dirty`: create the `RGBA8Unorm` `ShaderResource` texture on first use (`initialState = ShaderResource`, name `Aver.UI glyph page N`); query `textureCopyFootprint(tex, 0)`; allocate one transient `Upload` buffer sized to the dirty pages' footprints; expand A8 to premultiplied RGBA8 through the gamma LUT **row by row at `rowPitch`** (the D3D12 256-byte padding trap the Voxi code documents at `VoxiRenderer.cpp:2849`); then per page `textureBarrier(SR -> CopyDest)`, `copyBufferToTexture(tex, 0, staging, off)`, `textureBarrier(CopyDest -> SR)`; destroy the staging buffer immediately (the backends retire it behind the fence, `VoxiRenderer.cpp:2864`); `atlas->markUploaded(page)`.
- `submit()` keeps copying the list; `overlayPass` maps `texture & kUiAtlasTokenBit` to `pageTexture(texture & 0xFFFF)` (skip the draw if the page has no texture yet) and otherwise casts as today (`UiRenderer.cpp:183,216`). The binding-set cache (`bindingFor`) works unchanged for page textures.
- Parity: no shader, pipeline or blend change; only `createTexture`, `createBuffer`, `writeBuffer`, `textureCopyFootprint`, `textureBarrier`, `copyBufferToTexture`, which Voxi already runs on both backends. D3D11 (`UiRenderer::create` declines without a resource factory, `UiRenderer.cpp:44`) is unaffected.
- Ordering invariant (documented in the header): every glyph a frame's list references is rasterised before that frame's `beginFrame`. Both hosts satisfy it (`SandboxApp.cpp:1510-2204`, `GameApp.cpp:1975-2085` run the UI frame in `onUpdate`). A glyph rasterised later in the frame (the ImGui layout-editor preview builds during `buildUI`, after `prePass`) appears one frame late; `UiTextEngine::pendingUploads()` lets that preview request a redraw.
- Cost note for the headed pass: a page upload is one 1 MB copy plus a ~1 MB expansion; steady state uploads nothing.

### 2.10 C ABI (additive; `ui_abi.h` and `ui_widget_abi.h`, `Aver.UI.Abi`)

`ui_abi.h` (draw side):
```c
#define AVER_UI_TEXT_ALIGN_LEFT 0 / CENTER 1 / RIGHT 2 / START 3 / END 4
#define AVER_UI_TEXT_DIR_AUTO 0 / LTR 1 / RTL 2
#define AVER_UI_TEXT_FLAG_BOLD 1 / ITALIC 2 / RICH 4 / ELLIPSIS 8 / WRAP 16
#define AVER_UI_FONT_CONTENT 0 / SYSTEM 1 / ENGINE 2
typedef int32_t (*AverUiFontOpenFn)(int32_t kind, const char* utf8Name, const uint8_t** outData, uint64_t* outSize, void** outToken);
typedef void    (*AverUiFontCloseFn)(void* token);
typedef const char* (*AverUiLanguageFn)(void);

void        aver_ui_text_set_loader(AverUiFontOpenFn open, AverUiFontCloseFn close);   /* host; NULLs clear */
int32_t     aver_ui_text_load_fontset(const char* ocfontsUtf8);                         /* 1 ok; replaces the set */
const char* aver_ui_text_error(void);
void        aver_ui_text_set_language(const char* bcp47);                               /* "" = none */
void        aver_ui_text_set_language_fn(AverUiLanguageFn fn);                          /* host passes aver_fw_loc_culture; re-read each frame */
void        aver_ui_text_configure(int32_t pageSize, int32_t maxPages, int32_t glyphBudget);   /* <= 0 keeps */
const void* aver_ui_text_atlas(void);                                                   /* opaque UiGlyphAtlas*, host/renderer only */
void        aver_ui_text_stats(float* out12);   /* pages, glyphsResident, rasterThisFrame, rasterTotal, evictedPages, overflow, layoutHits, layoutMisses, cacheEntries, incompleteLayouts, faces, uploadBytesLastFrame */
int32_t     aver_ui_text_has_glyph(const char* family, uint32_t codepoint);
int32_t     aver_ui_text_measure(const char* utf8, const char* family, float px, int32_t flags, float maxWidth, float* outWHL);  /* width, height, lines */
float       aver_ui_text_block(float x, float y, const char* utf8, uint32_t rgba, const char* family, float px,
                               int32_t flags, float maxWidth, int32_t align, float* outWHL);   /* x,y = top-left of the block; returns height */
void        aver_ui_text_prewarm(const char* utf8, const char* family, float px, int32_t flags);
```
`aver_ui_text` / `aver_ui_text_width` / `aver_ui_font_metrics` / `aver_ui_has_font` keep their signatures and meaning; when the engine has a usable face they use the default family at the bake size 16 px (`UiTextConfig::legacyPx`), else the lent bitmap font exactly as today. `aver_ui_set_font` stays and is how `BITMAP legacy` is supplied. Localization references resolve as before (`UiAbi.cpp:130-139`).

`ui_widget_abi.h`: `int32_t aver_ui_widget_text_size(uint32_t id, float* outWHL)` (1 if the widget has a laid-out text from the last frame). No new widget property entry points: the property table is the interface (`aver_ui_widget_set_prop(id, "fontFamily", ...)`).

Mirrored enums get rows in `tests/abi/src/AbiEnumTest.cpp` (next to the `AVER_UI_LAYER_` row at `:664`), against the C# enums in 2.12. `docs/ABI.md` section 10's export count ("21 exports") is updated.

### 2.11 Host wiring, platform, packaging

**`platform::mapFileReadOnly`** (shared file, additive): `struct MappedFile { const u8* data; usize size; ...; }`, `bool mapFileReadOnly(const std::string& path, MappedFile& out)` (Win32 `CreateFileMappingW`/`MapViewOfFile`; POSIX `mmap`; both in `FileSystem.cpp`), `void unmap(MappedFile&)`. It deliberately does **not** call `traceFileOpen`.

**`Runtime/include/aver/game/GameUiText.hpp`** (new, header-only, `#if AVER_WITH_UI_ABI`, evaluated per host like `GameUiInput.hpp`): 
- `uiTextInit(const std::string& contentDir)`: installs the loader (kind Content: `contentDir/name`, mapped, then `traceFileOpen(path)`; kind Engine: `executableDir()/name` (the `@exe/` prefix), traced; kind System: Windows `%WINDIR%\Fonts\name` then `%LOCALAPPDATA%\Microsoft\Windows\Fonts\name`, **untraced by design**, logging one `[UiText] system font <name>` line per opened face); reads `contentDir/UI/Fonts.ocfonts` with `readFileText` when it exists, else the built-in default text of 2.4; calls `aver_ui_text_load_fontset`; installs `aver_ui_text_set_language_fn(&aver_fw_loc_culture)` when the framework is built in (the same condition as `aver_ui_set_text_resolver`, `GameApp.cpp:1174-1175`, `SandboxProject.cpp:145`).
- `uiTextBindRenderer(UiRenderer*)`: `renderer->setTextAtlas(static_cast<UiGlyphAtlas*>(const_cast<void*>(aver_ui_text_atlas())))`.
- `uiTextReloadIfChanged(const std::vector<FileEvent>&)`: for the editor's content watcher, reloads on `.ocfonts`, `.ttf`, `.otf`, `.ttc` under Content.
- `uiTextShutdown()`: unbinds the renderer (before the device is torn down, mirroring `GameApp.cpp:2341-2350`) and clears the loader.
Call sites (small, additive): `GameApp.cpp` after `loadGameUiFont(e)` (`:1832`) and `attachGameUi` (`:771`), teardown beside `:2341`; `SandboxApp.cpp` after `loadGameUiFont` (`:721`) and `UiRenderer::create` (`:1185`), content watcher, teardown. `loadGameUiFont` stays (it supplies `BITMAP legacy`).

**Packaging**: `scripts/game.allowlist` gains `Roboto-Regular.ttf`, `Roboto-Medium.ttf`, `Roboto-Regular.ocfont`, `Roboto-Regular.png` in `[bin]` with the licence file in the notices table (`stage-payload.ps1:375` already lists Roboto/Apache-2.0; `stage-game.ps1`'s third-party-licence refusal is satisfied by that row); the "DELIBERATELY ABSENT" comment at `game.allowlist:124` is rewritten; `docs/PACKAGING.md:220`'s row changes from "excluded" to "ships (default UI font)". Project fonts and `Content/UI/Fonts.ocfonts` are ordinary Content and ship with it. `scripts/verify-game.ps1` needs no change: project/engine opens are traced inside the package, OS-font opens are intentionally untraced (a comment there records why). The stb notice already exists (stb row in the same table).

### 2.12 C# API (`scripting/csharp/Aver.UI`)

New `TextStyle.cs`/`UiText.cs`; `Native.cs` gets the new `DllImport`s (UTF-8 marshalling like the existing ones); `Widgets.cs` and `Hud.cs` get thin additions.

```csharp
namespace Aver.UI;
public enum TextAlignment { Left = 0, Center = 1, Right = 2, Start = 3, End = 4 }      // mirrors AVER_UI_TEXT_ALIGN_*
public enum TextDirection { Auto = 0, LeftToRight = 1, RightToLeft = 2 }               // AVER_UI_TEXT_DIR_*
[Flags] public enum TextFlags { None = 0, Bold = 1, Italic = 2, Rich = 4, Ellipsis = 8, Wrap = 16 }
public readonly record struct TextStyle(string? Family = null, float Size = 16f, bool Bold = false, bool Italic = false,
                                        bool Rich = false, bool Ellipsis = false);
public readonly record struct TextMetrics(float Width, float Height, int Lines);
public readonly record struct TextStats(int Pages, int GlyphsResident, int RasterThisFrame, long RasterTotal,
                                        int EvictedPages, int Overflow, int LayoutHits, int LayoutMisses, int Faces);

public static class UiText {
    public static bool LoadFontSet(string ocfontsText);          // normally the host does this
    public static void Configure(int pageSize = 0, int maxPages = 0, int glyphBudget = 0);
    public static string Language { set; }                       // BCP-47; the host normally follows Loc
    public static bool HasGlyph(string family, int codePoint);
    public static TextMetrics Measure(string text, TextStyle style, float maxWidth = 0f);
    public static void Prewarm(string text, TextStyle style);
    public static TextStats Stats { get; }
}
// Hud additions (existing Text/MeasureText/TextCentred unchanged):
public static float TextBlock(Rect r, string text, Colour colour, TextStyle style, TextAlignment align = TextAlignment.Left);  // wraps in r.Width, returns height
public static TextMetrics MeasureText(string text, TextStyle style, float maxWidth = 0f);
// Widget additions (typed wrappers over Set/Get):
public string FontFamily { get; set; }  public float FontSize { get; set; }  public bool Bold { get; set; }  public bool Italic { get; set; }
public TextAlignment TextAlign { get; set; }  public TextDirection TextDir { get; set; }  public bool Wrap { get; set; }
public bool Ellipsis { get; set; }  public int MaxLines { get; set; }  public float LineSpacing { get; set; }  public float LetterSpacing { get; set; }
public bool RichText { get; set; }  public Colour TextColour { set; }  public TextMetrics TextSize { get; }   // last laid-out size
```
`Aver.Framework` (graph owner) gets `UiTextGraphInterop` (2.13). The rest of `Aver.UI` is untouched; `Aver.UI.csproj` still references nothing.

### 2.13 Aver Node nodes

Category `"UI"`. Owner class `UiTextGraphInterop` in new `scripting/csharp/Aver.Framework/UiTextGraph.cs` (internal static, like `UiGraphInterop`; methods call `Aver.UI.Widget`/`UiText`, return false for a dead widget id, catch `DllNotFoundException` and return false). Rows in new `scripting/csharp/Aver.Graph/GraphUiTextNodeTable.cs` (`UiTextTable()`, appended with one `.Concat(UiTextTable())` at `GraphGameSystemNodeTable.cs:121`). Palette mirror: new `sandbox/src/GraphNodeDefsUiText.hpp` with `appendUiTextNodeDefs(t)`, one `#include` at `GraphNodeDefs.hpp:115` and one call before `buildCatalog` returns. Type names use the `AN_` prefix like the other UI nodes.

| Node | Pins (besides `exec`, `then`) | Attrs | Wraps (`UiTextGraphInterop`) |
|---|---|---|---|
| `AN_SetUiFont` | in `widget:int`, `size:float=0` (0 = theme size), `bold:bool`, `italic:bool`; out `success:bool` | `family` (optional; empty keeps) | `SetUiFontForGraph(int widget, float size, bool bold, bool italic, string family)` |
| `AN_SetUiTextStyle` | in `widget:int`, `align:int` (0 left, 1 centre, 2 right, 3 start, 4 end), `dir:int` (0 auto, 1 LTR, 2 RTL), `wrap:bool`, `ellipsis:bool`, `maxLines:int` (0 = unlimited), `lineSpacing:float=1`, `letterSpacing:float`, `rich:bool`; out `success:bool` | | `SetUiTextStyleForGraph(...)` (sets all nine) |
| `AN_SetUiTextColor` | in `widget:int`, `r:float`, `g:float`, `b:float`, `a:float=1`; out `success:bool` | | `SetUiTextColorForGraph` (writes `textColor`) |
| `AN_GetUiTextSize` (pure) | in `widget:int`; out `found:bool`, `width:float`, `height:float`, `lines:int` | | `GetUiTextSizeForGraph(int widget, out float width, out float height, out int lines)` -> found |

Row strings (exact shapes, in the style of `GraphGameSystemNodeTable.cs`):
```csharp
Reg("an_setuifont", GenericKind.Exec, typeof(UiTextGraphInterop), "SetUiFontForGraph", "widget:int,size:float,bold:bool,italic:bool", "success:bool", "p:widget,p:size,p:bold,p:italic,@family?", "success"),
Reg("an_setuitextstyle", GenericKind.Exec, typeof(UiTextGraphInterop), "SetUiTextStyleForGraph", "widget:int,align:int,dir:int,wrap:bool,ellipsis:bool,maxLines:int,lineSpacing:float=1,letterSpacing:float,rich:bool", "success:bool", "p:widget,p:align,p:dir,p:wrap,p:ellipsis,p:maxLines,p:lineSpacing,p:letterSpacing,p:rich", "success"),
Reg("an_setuitextcolor", GenericKind.Exec, typeof(UiTextGraphInterop), "SetUiTextColorForGraph", "widget:int,r:float,g:float,b:float,a:float=1", "success:bool", "p:widget,p:r,p:g,p:b,p:a", "success"),
Reg("an_getuitextsize", GenericKind.Pure, typeof(UiTextGraphInterop), "GetUiTextSizeForGraph", "widget:int", "found:bool,width:float,height:float,lines:int", "p:widget,>width,>height,>lines", "found"),
```
Not given nodes (stated so nobody wonders): font-set loading, atlas config, prewarm, `HasGlyph` (C# only: no string pins), UI language (follows the existing culture nodes `AN_SetCulture`, `docs/LOCALIZATION.md:57`; the host re-reads the culture each frame). `docs/AVER_NODE_NODES.md` gets a "Game UI text" block under the UI table (after `:737`'s group) with the four rows and the semantics above (including: `AN_SetUiTextStyle` sets all nine; `align` start/end follow the paragraph direction).

### 2.14 Editor

1. **Font Set editor** (`.ocfonts`), `sandbox/src/FontSetEditor.{hpp,cpp}`, an `AssetEditor` registered next to `UiLayoutEditor` (`SandboxApp.cpp:463`, factory `editor::makeFontSetEditor`), same shape as `UiLayoutEditor`: a pure core (free functions `fsAddFamily/fsRemoveFamily/fsAddFace/fsMoveFace/fsSetAttr/fsAddOverride/...`, whole-document snapshot undo, load/save/dirty, `.ocfonts` text through `UiFontSet::parse/write`) compilable with `AVER_WITH_IMGUI` undefined, plus the ImGui half. Panes: **Families** (tree of families and entries; Add Face opens the Content picker filtered to `.ttf .otf .ttc`; SYSTEM and BITMAP rows; reorder; DEFAULT), **Preview** (sample strings for Latin, Greek, Cyrillic, Arabic, Hebrew, CJK ja/zh/ko, a mixed bidi line, a wrap box; size slider, bold/italic, language box, "tint by face" which colours glyphs by `UiGlyphRef::face`, and shows box glyphs), **Coverage** (per face: Unicode blocks present, `FE70..FEFC` check for Arabic, kerning source `kern`/`GPOS`/none), **Atlas** (page thumbnails via `IDevice::uiTextureId`, occupancy, the `aver_ui_text_stats` counters). Content Browser: "New Font Set" writes a starter (`editor::fontSetStarterText()`), type label/icon for `.ocfonts`.
2. **UI Layout editor** (`UiLayoutEditor.cpp`, owned): `PreviewMetrics`/`PreviewPainter` (`:497-540`) use `UiEngineMetrics` and the engine's glyph runs when the engine has a usable face (glyph quads via `ImDrawList::AddImage` with the page's `uiTextureId`), else the current ImGui-font path. Known, accepted difference: ImGui blends straight alpha and the atlas is premultiplied, so preview glyph edges look slightly thinner than in Play; the Interact/Play view is the exact one. `drawProperty` shows `fontFamily` as a combo of the loaded family names (free text still allowed) and `textDir`/`textAlign` as enums via the table; the new properties otherwise appear automatically from the property table.
3. **Content watcher**: `uiTextReloadIfChanged` on `.ocfonts/.ttf/.otf/.ttc`.

### 2.15 Limits (documented in `docs/GAME_UI_TEXT.md`)
Section 0 item 14, plus: Arabic quality depends on presentation-form coverage (2.5); no mark anchoring beyond bounding boxes; variable fonts render their default instance; OS fonts exist only on Windows in v1; fonts are trusted-source only; logical-order caret movement in bidi text; no IME; Korean wraps at spaces.

## 3. FILE OWNERSHIP

### 3.1 Created (only this feature touches them)

`modules/ui/include/aver/ui/`: `UiUnicode.hpp`, `UiBidi.hpp`, `UiShaping.hpp`, `UiLineBreak.hpp`, `UiRichText.hpp`, `UiFontFace.hpp`, `UiFontSet.hpp`, `UiGlyphAtlas.hpp`, `UiTextLayout.hpp`, `UiTextEngine.hpp`.
`modules/ui/src/`: `UiUnicode.cpp`, `UiUnicodeData.cpp` (range tables: bidi class, mirror, script, line-break class, combining), `UiBidi.cpp`, `UiShaping.cpp`, `UiArabicData.cpp` (joining + forms tables), `UiLineBreak.cpp`, `UiRichText.cpp`, `UiTrueType.cpp` (the only TU that defines `STB_TRUETYPE_IMPLEMENTATION`), `UiFontFace.cpp`, `UiFontSet.cpp`, `UiGlyphAtlas.cpp`, `UiTextLayout.cpp`, `UiTextEngine.cpp`.
`Runtime/include/aver/game/GameUiText.hpp`.
`sandbox/src/FontSetEditor.hpp`, `sandbox/src/FontSetEditor.cpp`, `sandbox/src/GraphNodeDefsUiText.hpp`.
`scripting/csharp/Aver.UI/TextStyle.cs`, `scripting/csharp/Aver.UI/UiText.cs`, `scripting/csharp/Aver.Framework/UiTextGraph.cs`, `scripting/csharp/Aver.Graph/GraphUiTextNodeTable.cs`.
Tests: `tests/ui/src/{UiUnicodeTest,UiBidiTest,UiShapingTest,UiLineBreakTest,UiFontTest,UiGlyphAtlasTest,UiTextLayoutTest,UiTextTreeTest,UiTextAbiTest}.cpp`, `tests/ui/src/TestFont.hpp` (synthetic TrueType builder), `tests/editor/src/FontSetEditorTest.cpp`, `scripting/csharp/Aver.Graph.Tests/UiTextNodeTests.cs`.
Docs: `docs/GAME_UI_TEXT.md` (user-facing summary of 2.2-2.8, 2.12, 2.13).

### 3.2 Modified, owned by this feature (the feature may edit freely)

- `modules/ui/CMakeLists.txt` (sources, `target_include_directories(Aver.UI PRIVATE ${CMAKE_SOURCE_DIR}/third_party/imgui)` for `imstb_truetype.h` only; **no link to ImGui**), `modules/ui/README.md`.
- `modules/ui/src/UiFont.cpp`, `UiDrawList.cpp`, `include/aver/ui/UiDrawList.hpp` (UTF-8 decode in `uiTextWidth`/`addTextScaled`, `addGlyphQuad` with skew), `UiText.cpp`, `include/aver/ui/UiText.hpp`.
- `modules/render.ui/include/aver/render/ui/UiRenderer.hpp`, `src/UiRenderer.cpp`, `README.md`.
- `modules/ui.abi/src/UiAbi.cpp`, `include/aver/ui/ui_abi.h`, `src/UiAbiState.hpp` (engine singleton accessors).
- `sandbox/src/UiLayoutEditor.cpp` (+ `.hpp` only for the starter/factory if needed).
- `scripting/csharp/Aver.UI/Native.cs`, `Hud.cs` (additions), `Widgets.cs` (additions).
- `tests/ui/CMakeLists.txt`, `tests/render.ui/src/UiRenderTest.cpp` (mock context records copies/barriers), `tests/editor/CMakeLists.txt` (one block).
- Docs: `docs/GAME_UI.md` (`:69` fonts sentence, `:410` limits, new property list rows, a "Text" section), `docs/LOCALIZATION.md:61-63`, `docs/SCRIPTING_API.md:638-640,878-881` (stale "no text" corrected) and a new subsection under section 16, `docs/PACKAGING.md:220`, `docs/ABI.md` section 10.

### 3.3 SHARED files (other features may touch; keep edits small and additive, whoever lands second keeps both hunks)

| File | This feature's edit | Also touched by |
|---|---|---|
| `modules/ui/include/aver/ui/UiTree.hpp` | `textStyleOf`, `textBoxOf`, `lastTextSize` + a small cache member | accessibility (`setAccess`, two members) |
| `modules/ui/src/UiTreeLayout.cpp`, `UiTreeDraw.cpp`, `UiTreeInput.cpp` | `measure`/`draw`/caret via layouts (hunks at `:84-136`, `:37-50,120-275`, `:316-341`) | accessibility (`fontPx :45-48` text scale; colour overrides `:101-115`) |
| `modules/ui/include/aver/ui/UiWidget.hpp`, `src/UiProps.cpp` | the props of 2.7 (after `wrap`) | accessibility (`accessName`) |
| `modules/ui/include/aver/ui/UiTypes.hpp`, `UiStyle.hpp/.cpp` | `UiTextAlign` +2, `UiTextDir`, style fields | none known |
| `modules/ui.abi/include/aver/ui/ui_widget_abi.h`, `src/UiWidgetAbi.cpp` | `aver_ui_widget_text_size`; painter construction `:78` | accessibility (`aver_ui_set_access`, captions, announcer) |
| `Runtime/src/GameApp.cpp`, `Runtime/include/aver/game/GameApp.hpp` | 4 call-site insertions (2.11) | accessibility, others |
| `sandbox/src/SandboxApp.cpp/.hpp`, `SandboxContentBrowser.cpp`, `sandbox/CMakeLists.txt` | init/teardown calls, editor factory registration (`:463` neighbourhood), Content Browser entry, `FontSetEditor.cpp` source, reload hook | many |
| `modules/platform/include/aver/platform/FileSystem.hpp`, `src/FileSystem.cpp` | `MappedFile`, `mapFileReadOnly`, `unmap` (append at the end of the file API) | none known |
| `scripts/game.allowlist`, `scripts/verify-game.ps1` (comment only) | Roboto files in `[bin]`; one explanatory comment | packaging work |
| `tests/abi/src/AbiEnumTest.cpp` | rows for `AVER_UI_TEXT_ALIGN_/DIR_/FLAG_` | any ABI feature |
| `scripting/csharp/Aver.Graph/GraphGameSystemNodeTable.cs` | `.Concat(UiTextTable())` on `:121` | accessibility and every node feature |
| `sandbox/src/GraphNodeDefs.hpp` | one `#include` (`:115` area) + one `appendUiTextNodeDefs(t)` call | accessibility and every node feature |
| `scripting/csharp/Aver.Graph.Tests/Program.cs` | register `UiTextNodeTests.RunAll()` (`:280` area) | every node feature |
| `docs/AVER_NODE_NODES.md` | four rows (UI group after `:737`) | every node feature |

`Native.cs` of `Aver.Framework`, `framework_abi.h`, `modules/formats/OcProject.*` and `UiLayoutAsset.*` are **not** touched (project fonts are found by convention, not by a new project key; the `.ocui` format needs no change).

## 4. Phased implementation

Each phase ends with its tests green and is committed on its own; later phases never depend on a GPU.

**Phase 1: foundation and the fix.** `UiUnicode` (UTF-8, class tables), UTF-8 decode in the bitmap path (`uiTextWidth`, `addTextScaled`, `UiFontMetrics`), `UiTextLayout` types, `UiTextMetrics::layoutText` + `UiPainter::drawText` defaults, `UiTextAlign Start/End`, `UiTextDir`, tree switched to `layoutText/drawText` (legacy layout builder reproduces today's output byte for byte), all existing UI tests unchanged and green. New: `UiUnicodeTest`, `UiTextTreeTest` legacy half.
**Phase 2: fonts.** `UiTrueType.cpp`, `UiFontFace` + `uiValidateSfnt`, `UiFontSet` (`.ocfonts` parse/write, chain, `SYSTEM auto` table, memo), `TestFont.hpp`. Tests: `UiFontTest`.
**Phase 3: atlas, rasterisation, LTR engine, GPU, ABI, hosts.** `UiGlyphAtlas`, `UiTextEngine` steps 1, 3, 6, 7, 10 (LTR only, spaces break), `emit`, `UiEngineMetrics`, engine-backed `UiDrawListPainter`, ABI of 2.10, `UiRenderer::prePass` + tokens, `platform::mapFileReadOnly`, `GameUiText.hpp`, host call sites, game.allowlist. Tests: `UiGlyphAtlasTest`, `UiTextLayoutTest` (LTR), `UiTextAbiTest`, `UiRenderTest` additions. First headed run: Latin/Greek/Cyrillic/CJK at several sizes, both backends.
**Phase 4: line breaking, wrap, ellipsis, CJK.** `UiLineBreak`, steps 9-11, kinsoku, `maxLines`. Tests: `UiLineBreakTest`, wrap cases in `UiTextLayoutTest`.
**Phase 5: bidi, Arabic, marks, caret.** `UiBidi`, `UiShaping` + data, steps 4, 5, 8, 12, 13, mirroring, TextInput caret via layouts. Tests: `UiBidiTest`, `UiShapingTest`, bidi cases in `UiTextLayoutTest`.
**Phase 6: styling, rich text, C#, nodes, editor, docs.** Props/table/`UiStyle`, `UiRichText`, `Hud`/`Widget`/`UiText` C#, `UiTextGraphInterop` + rows + palette + node tests, `FontSetEditor`, `UiLayoutEditor` preview, docs corrections. Tests: `FontSetEditorTest`, `UiTextNodeTests`, `AbiEnumTest` rows, property round trips in `UiTextTreeTest`.

## 5. Tests (all ctest-registered by the `*Test` walk, `CMakeLists.txt:602+`; none needs a GPU; fonts come from `TestFont.hpp`, Roboto from `AVER_REPO_ROOT`, OS fonts skip with exit 77 when absent)

Link line for new `tests/ui` executables: `Aver.UI Aver.Core Aver.Platform` (+ `AVER_REPO_ROOT` define as `tests/formats/CMakeLists.txt:178`); `UiTextAbiTest` links `Aver.UI.Abi` like `UiLocalizeAbiTest`.

**`TestFont.hpp`**: builds a valid in-memory TrueType (tables `head hhea maxp hmtx cmap(fmt 4 and fmt 12) loca glyf name post`, optional `kern` format 0 and GPOS PairPos format 1, optional TTC wrapper of two fonts) from a spec `{cp -> (advance, box)}` where every glyph is a filled rectangle. Distinct advances make order, kerning, shaping and fallback observable by arithmetic, and a rectangle's rasterised coverage is exactly computable.

- **UiUnicodeTest**: round trip of every valid scalar value; each invalid class (overlong, surrogate, > U+10FFFF, truncated, stray continuation) yields one U+FFFD per bad byte and never loses sync; `cpToByte` monotone; bidi class/mirror/script spot tables (~60 code points incl. boundaries of every range); 20,000 random byte strings never crash and re-encode stably.
- **UiBidiTest**: paragraph level by first strong, explicit `RTL/LTR`; ~60 hand-typed cases covering W1-W7 (e.g. AL then digits -> AN, ES/ET/CS between numbers), N1-N2 (neutrals between same direction), I1-I2, L1 trailing whitespace, L2 visual order for mixed Latin/Hebrew/Arabic with numbers, mirroring of `()[]<>` in RTL runs, explicit embeddings/overrides with depth cap; `reorder` is a permutation of the line; invariants over 50,000 random strings (levels length, level <= 126, permutation).
- **UiShapingTest**: joining type of every letter in the table; forms of "العربية", "سلام", "محمد", "بسم الله" compared with hand-checked presentation-form sequences; lam-alef in all four variants, initial/medial/final context, after a non-joining letter, with an intervening mark; ZWNJ breaks, ZWJ joins, tatweel is dual; Persian letters (پ چ ژ گ ک ی); shaping an already shaped string is a fixed point; marks keep their cluster; `srcCount` sums to the input length.
- **UiLineBreakTest**: classes and the implemented LB rules each with positive and negative cases; numbers (`$10.00`, `1,000`, `3.14%`), URLs and paths, NBSP/WJ/GL, hard breaks, CJK break-anywhere, every kinsoku list member as a line start/end candidate, mixed Latin+CJK, ellipsis (`…`) and quotes; `breaks[0]`/`[n]` semantics; 20,000 random strings produce monotone legal output.
- **UiFontTest**: validator accepts the synthetic fonts and Roboto, rejects 40 targeted mutations (truncation at every table boundary, offset past the end, zero `unitsPerEm`, `numGlyphs` 0, overlapping tables, bad `ttcf` index); metrics/advance/`cmap` against the builder's spec; kern from a `kern` table and from GPOS PairPos format 1 and 2 equal the spec, and GPOS-wins-over-kern is asserted; TTC index selection; rasterisation of a rectangle glyph equals the exact coverage with the 1 px gutter; `.ocfonts` parse/write round trip, line-numbered errors, unknown keys skipped, a newer `OCFONTS` version note; fallback chain order for (bold,italic) and `lang=` with fake `UiFontLoader` (memory blobs), negative memo, `OVERRIDE`; missing-glyph box emitted only for non-format code points.
- **UiGlyphAtlasTest**: shelf packing never overlaps and stays inside pages (1,000,000 random rect inserts across sizes 1..160, check disjointness incl. gutters); page-LRU: fill all pages, touch some, insert, the right page recycles and its keys vanish; a page used this frame is never recycled (insert returns null, `overflow` counts); `generation` and `dirty` semantics; `markUploaded`; budget accounting; reset on font-set reload.
- **UiTextLayoutTest**: widths equal sum of advances + kerning exactly on the synthetic font; `letterSpacing`; line height rules (`max` of used faces, `lineSpacing`); alignment through a recording painter (`Left/Center/Right/Start/End` x LTR/RTL); wrap at spaces, in CJK, inside a long word, with hanging spaces; kinsoku push-down; `maxLines` + ellipsis position for LTR and RTL; mixed bidi lines give the visual order the table predicts; Arabic glyph ids are the presentation forms; marks have zero advance and lie within their base's box horizontally; rich text (tags, nesting, unclosed, `<<`, `a < b`, `richColors=false`); caret round trip `indexAt(caretX(i)) == i` for LTR and monotone stops for RTL; cache: hit returns the same pointer, an `incomplete` layout is not stored, the epoch drops entries; budget: with `glyphBudget = 3` a 20-glyph string completes over 7 frames; raster size quantisation (px 15.4 and 15.6 differ, 15.4 and 15.2 share a key); a Roboto smoke test (Latin/Greek/Cyrillic width positive and kerning pair "AV" narrower than "A"+"V").
- **UiTextTreeTest**: the legacy path (`UiEstimatedMetrics`) produces identical layout/draw calls as before (compare against the existing recording painters); a tree with the engine metrics: Text wraps, Button/Choice/List widths follow measured text, TextInput caret and click-to-caret on LTR and RTL text, `textAlign start/end`; every new property round-trips `.ocui` write->parse->write and `aver_ui_widget_set_prop` get/set; unknown property in an old file is skipped.
- **UiTextAbiTest**: load a font set through the C ABI with an in-memory loader; `aver_ui_text_measure/block` widths; draw-list commands reference atlas tokens; stats move; the legacy `aver_ui_set_font` + `aver_ui_text` path still draws (and draws "é" correctly after the phase 1 fix); language function changes the chain.
- **`tests/render.ui` `UiRenderTest`** (mock device records): first dirty page creates one `RGBA8Unorm` texture, one staging buffer sized `footprint.totalBytes`, barrier/copy/barrier order, rows written at `rowPitch` when padded (mock footprint with a 256 pitch), a clean frame uploads nothing, a recycled page re-uploads, tokens map to page textures and non-tokens still cast, `setTextAtlas(nullptr)` is safe, staging destroyed after recording.
- **`tests/editor` `FontSetEditorTest`** (compiles `sandbox/src/FontSetEditor.cpp` with `AVER_WITH_IMGUI` undefined): the `fs*` edits, ordering, `OVERRIDE` repair on family rename/delete, load/save/dirty/undo, coverage numbers for a synthetic font, a corrupt file reports `loadError`.
- **`tests/abi` `AbiEnumTest`**: the three new constant groups equal the C# enums.
- **C# `UiTextNodeTests`** (`NodeRowKit`, in the `MotionMatchNodeTests` mould): the four rows exist exactly (`Owner.Name == "UiTextGraphInterop"`), pins/defaults match, every row validates against the real method (`GameSystemNodes.Validate()`), IL valid, exec/pure compiler split (`an_getuitextsize` is pure), the optional `family=` attribute reaches the node and a missing one is not an error, an unknown widget id returns false.
- Existing suites that must stay green untouched: `UiTest`, `UiWidgetTest`, `UiLayoutAssetTest`, `UiLayoutEditorTest`, `UiLocalizeTest`, `UiLocalizeAbiTest`, `UiRenderTest`.

## 6. Headed manual verification (main loop; D3D12 first, then `--backend vulkan`; GPU idle for timing; screenshots at 1440p)

1. **Default look and scale**: open the demo level with a HUD; Roboto text is crisp at `graphics.uiScale` 0.75, 1.0, 1.25, 1.5 and with the window resized (no soft scaled bitmaps). Compare against a 0.7.0 screenshot: same face, same layout within a pixel.
2. **Packaged game**: stage a game (`stage-game.ps1`), run `verify-game.ps1`; HUD text present; `[UiText]` log lines for any OS font; isolation assertion passes (this also confirms the earlier finding that games shipped no HUD font).
3. **Scripts**: a settings screen with `ja`, `zh-Hans`, `ar`, `he`, `ru`, `de` string tables; switch culture at runtime (`AN_SetCulture`): text re-lays out, Han glyphs follow the language's face, Arabic is joined and right-to-left, Hebrew numbers keep their order.
4. **Arabic**: a multi-line wrapped paragraph with tashkeel and Latin/number runs, right-aligned with `textAlign=start`; lam-alef visible; marks above/below do not collide with the letters; a font without `FE70..FEFC` (a project font) shows unjoined letters and the coverage tab says why.
5. **CJK**: a 600-character Chinese paragraph in a fixed-width wrapped box: no line starts with `，。」`, none ends with `「`; a 3,000-character stress string: first frame spreads over frames (budget), then steady state rasterises 0 glyphs/frame (`aver_ui_text_stats`); atlas page count stays <= 16; scroll a long list of distinct CJK names for a minute: no corruption, evictions counted, no wrong-glyph flashes.
6. **Missing glyphs**: a symbol no installed font has draws the box, not nothing.
7. **Styling and rich text**: bold/italic faces, `fontFamily` per widget, `letterSpacing`, `lineSpacing`, `ellipsis` + `maxLines` in a 3-line box, `rich` with `<color>` `<size>` `<b>`; a localized string containing `<` literal text when `rich` is off.
8. **TextInput**: type, click to place the caret in Arabic and mixed text, caret stays inside the field, selection of the box glyph case; masked field.
9. **Editor**: Font Set editor on a project set (add a face, reorder, preview each script, tint by face, atlas tab counters); UI Layout editor preview shows shaped Arabic and CJK; New Font Set from the Content Browser; replacing a `.ttf` on disk hot-reloads.
10. **Vulkan parity**: repeat 1, 4, 5 with `--backend vulkan`; compare screenshots with D3D12 (identical up to rasteriser-independent output: both use the same CPU bitmaps, so they should be pixel-identical).
11. **Performance** (targets to measure, not claims): steady-state UI with ~40 text widgets: text CPU under 0.3 ms, zero rasterisations; a 300-glyph relayout on a width change under 1 ms; atlas upload frames show one or two 1 MB copies and no hitch at 144 Hz. Run with the owner's real editor.ini.
12. **ImGui and byte scan**: `stage-game.ps1`'s ImGui marker scan (`8d`) still passes (the stb header is compiled into `Aver.UI` with `STBTT_STATIC` and no ImGui symbols).

## 7. Open questions (defaults chosen; say if you disagree)

1. **Bundled Arabic/CJK fonts.** The licence policy excludes OFL (`third_party/fonts/README.md:28-29`), which rules out Noto. Default: ship none; OS fonts on Windows; projects add their own `.ttf`. Say if you want an OFL exception or have an Apache/commercial CJK font to bundle (CJK is 10 to 20 MB).
2. **Rich-text tags are new** (none existed). Default: include the small set, off unless `rich`. Drop phase 6's `UiRichText` if you want strictly existing behaviour.
3. **Korean** wraps at spaces only (Hangul syllables are AL). Default kept; a `break-anywhere` project switch is a one-line later add.
4. **Typing non-ASCII** needs the `WM_CHAR` plumbing listed in `docs/GAME_UI.md:408-409`; out of scope here (display works).
5. **Hand-authored Unicode range tables** (bidi, joining, line break, script) instead of vendoring UCD data files, so no new third-party data. They are checked by the tests above; say if you would rather vendor derived tables.
6. **Text weight.** stb has no hinting and our blend is in display space, so light-on-dark text can look thin. `coverageGamma` (default 1.0) is the knob; the headed pass picks the shipped value.

## 8. Risks

- **stb limits** (no GSUB, partial GPOS) mean some OTF fonts kern less or join Arabic less than a HarfBuzz stack would; the coverage tab and 2.3/2.5 state exactly where.
- **Whole-page uploads** are the cost of an RHI with no sub-region copy; small pages cap it at ~1 MB per dirty page. A region copy in `IRenderContext` (both backends) would remove it and is the first optimisation if the headed pass shows hitches.
- **Shared-file churn** with accessibility in `UiTree*`, `UiProps.cpp`, `ui_widget_abi.h`, both host apps and the node-table lines; the rule in 3.3 is "both hunks, whoever lands second".
- **`Aver.UI.Abi` owns the engine**; native hosts read the atlas through an opaque pointer across the DLL boundary (the draw-list precedent). Only POD-style accessors are used there, and the renderer never frees atlas memory.
