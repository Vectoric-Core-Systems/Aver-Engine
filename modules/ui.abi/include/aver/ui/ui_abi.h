#pragma once
// Aver.UI — the plain-C ABI a game's HUD is authored against.
// Screen pixels, top-left origin; setters return 1/0; 0 is always an invalid handle.
#include <stdint.h>

#if defined(_WIN32)
#  if defined(AVER_UI_ABI_BUILD)
#    define AVER_UI_API __declspec(dllexport)
#  else
#    define AVER_UI_API __declspec(dllimport)
#  endif
#else
#  define AVER_UI_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Layer bands. Mirror aver::ui::UiLayer one for one.
#define AVER_UI_LAYER_BACKGROUND 0
#define AVER_UI_LAYER_CONTENT    1
#define AVER_UI_LAYER_OVERLAY    2
#define AVER_UI_LAYER_TOOLTIP    3
#define AVER_UI_LAYER_DEBUG      4

// Text alignment (aver::ui::UiTextAlign): Left / Center / Right are physical, Start / End follow the
// paragraph direction. Mirror Aver.UI.TextAlignment.
#define AVER_UI_TEXT_ALIGN_LEFT   0
#define AVER_UI_TEXT_ALIGN_CENTER 1
#define AVER_UI_TEXT_ALIGN_RIGHT  2
#define AVER_UI_TEXT_ALIGN_START  3
#define AVER_UI_TEXT_ALIGN_END    4
// Paragraph direction (aver::ui::UiTextDir). Mirror Aver.UI.TextDirection.
#define AVER_UI_TEXT_DIR_AUTO 0
#define AVER_UI_TEXT_DIR_LTR  1
#define AVER_UI_TEXT_DIR_RTL  2
// Flags of aver_ui_text_measure / block / prewarm. Mirror Aver.UI.TextFlags.
#define AVER_UI_TEXT_FLAG_BOLD     1
#define AVER_UI_TEXT_FLAG_ITALIC   2
#define AVER_UI_TEXT_FLAG_RICH     4
#define AVER_UI_TEXT_FLAG_ELLIPSIS 8
#define AVER_UI_TEXT_FLAG_WRAP     16
// Where a font file is looked for (the `kind` the host's open callback gets).
#define AVER_UI_FONT_CONTENT 0   /* a path under the project's Content */
#define AVER_UI_FONT_SYSTEM  1   /* a file name in the OS fonts directory (Windows only in v1) */
#define AVER_UI_FONT_ENGINE  2   /* a file name next to the executable (".ocfonts" paths written @exe/name) */

// ---- the frame ------------------------------------------------------------------------------------

// Clears the list and declares the rectangle this frame's UI is laid out against, in backbuffer
// pixels. Called by the host, once per frame, before anything ticks.
AVER_UI_API void aver_ui_begin_frame(float x, float y, float w, float h);

// Writes the rectangle begin_frame was given: x, y, width, height. A HUD anchors to this, not to
// the window.
AVER_UI_API void aver_ui_viewport(float* outXYWH);

// ---- state ----------------------------------------------------------------------------------------

// Sets the layer subsequent draws go to. Out-of-range is ignored, not clamped.
AVER_UI_API void    aver_ui_set_layer(int32_t layer);
// Returns the current layer.
AVER_UI_API int32_t aver_ui_layer(void);

// Pushes a clip rect, intersected with the one in force. Coordinates are edges, not an extent.
AVER_UI_API void aver_ui_push_clip(int32_t left, int32_t top, int32_t right, int32_t bottom);
// Pops the innermost clip rect.
AVER_UI_API void aver_ui_pop_clip(void);

// ---- geometry ---------------------------------------------------------------------------------------

// Draws a solid rectangle. `rgba` is straight (non-premultiplied) 0xAABBGGRR.
AVER_UI_API void aver_ui_rect(float x, float y, float w, float h, uint32_t rgba);

// Draws a textured rectangle. `texture` is 0 for the built-in white texel and must be
// premultiplied, like the colour.
AVER_UI_API void aver_ui_textured_rect(float x, float y, float w, float h, uint64_t texture,
                                       float u0, float v0, float u1, float v1, uint32_t rgba);

// ---- text -------------------------------------------------------------------------------------------
//
// WHY THIS IS AN OPAQUE POINTER AND NOT A PATH. modules/ui is Core-only on purpose -- UiFont.hpp's
// own header says it "does no file I/O and knows nothing about the filesystem", and this DLL links
// no RHI, so it can neither read the .ocfont nor upload its atlas. The HOST does both (it already
// decodes the atlas and fills UiFont::atlasTexture) and lends the result here. This is
// aver_ui_draw_list's convention run the other way: the host hands over an `aver::ui::UiFont*` it
// keeps alive, and this module only ever reads it.
//
// BORROWED, NOT OWNED. Passing NULL clears it. A font that outlives its atlas texture would draw
// glyphs sampling a dead descriptor, so a host that tears its device down clears this first.
AVER_UI_API void aver_ui_set_font(const void* font);

// 1 when a font is set and has glyphs. A HUD checks this rather than measuring an empty string.
AVER_UI_API int32_t aver_ui_has_font(void);

// Writes ascent, descent, lineHeight (pixels) for the set font, or three zeroes. Descent is
// negative, matching UiFont.
AVER_UI_API void aver_ui_font_metrics(float* outAscentDescentLine);

// Draws `utf8` with the pen at (x, y) -- the LEFT END OF THE BASELINE, not a box corner, because
// that is the convention every glyph's offY is expressed against. Returns the pen's x afterwards so
// a caller can chain a label and a value without measuring twice. 0 and nothing drawn with no font.
AVER_UI_API float aver_ui_text(float x, float y, const char* utf8, uint32_t rgba);

// The width `utf8` would occupy. Needed BEFORE drawing to centre or right-align, which is why it is
// an export of its own and not something a caller derives from aver_ui_text's return.
AVER_UI_API float aver_ui_text_width(const char* utf8);

// ---- the text engine (docs/GAME_UI_TEXT.md) ---------------------------------------------------------
//
// Runtime TrueType with fallback chains, kerning, Arabic shaping, bidi, line breaking and styling. It
// lives in this DLL (like the draw list); the host feeds it font bytes through a callback, lends the
// atlas to the renderer by an opaque pointer, and everything below is additive. When the engine has a
// usable TrueType face, aver_ui_text / aver_ui_text_width / aver_ui_font_metrics use the default family
// at 16 px; otherwise they use the lent bitmap font exactly as before.

// Opens a font file. Returns 1 and fills outData / outSize (read only, valid until the matching close) and
// an opaque outToken passed back to the close callback; 0 when it cannot be opened. The host maps files
// read only; OS fonts (kind SYSTEM) must NOT be reported to the package file trace, project fonts must.
typedef int32_t (*AverUiFontOpenFn)(int32_t kind, const char* utf8Name, const uint8_t** outData, uint64_t* outSize,
                                    void** outToken);
typedef void (*AverUiFontCloseFn)(void* token);
// The current UI language (BCP-47), e.g. aver_fw_loc_culture. Re-read every aver_ui_begin_frame.
typedef const char* (*AverUiLanguageFn)(void);

// Installs the font loader (host). NULLs clear it AND close every font opened through it.
AVER_UI_API void aver_ui_text_set_loader(AverUiFontOpenFn open, AverUiFontCloseFn close);
// Parses and loads an .ocfonts document, replacing the font set. 1 ok, 0 failed (aver_ui_text_error).
AVER_UI_API int32_t aver_ui_text_load_fontset(const char* ocfontsUtf8);
AVER_UI_API const char* aver_ui_text_error(void);
// The UI language; "" = none. Chains reorder (lang=) and OVERRIDE rows apply.
AVER_UI_API void aver_ui_text_set_language(const char* bcp47);
AVER_UI_API void aver_ui_text_set_language_fn(AverUiLanguageFn fn);
// Atlas page size, page cap and per-frame glyph budget. A value <= 0 keeps the current one.
AVER_UI_API void aver_ui_text_configure(int32_t pageSize, int32_t maxPages, int32_t glyphBudget);
// The glyph atlas as an opaque aver::ui::UiGlyphAtlas*, for the renderer (UiRenderer::setTextAtlas).
AVER_UI_API const void* aver_ui_text_atlas(void);
// pages, glyphsResident, rasterThisFrame, rasterTotal, evictedPages, overflow, layoutHits, layoutMisses,
// cacheEntries, incompleteLayouts, faces, uploadBytesLastFrame.
AVER_UI_API void aver_ui_text_stats(float* out12);
// 1 when some face of the family carries the code point.
AVER_UI_API int32_t aver_ui_text_has_glyph(const char* family, uint32_t codepoint);
// Measures `utf8` (localization references resolve): width, height, line count into outWHL (3 floats).
// `family` NULL or "" = the default; maxWidth > 0 wraps (with FLAG_WRAP) or ellipsizes (FLAG_ELLIPSIS).
// 1 when the engine measured it, 0 when no font is available (outWHL is then zeroes).
AVER_UI_API int32_t aver_ui_text_measure(const char* utf8, const char* family, float px, int32_t flags, float maxWidth,
                                         float* outWHL);
// Draws a block with its top-left at (x, y), wrapped in maxWidth (> 0), aligned within it by AVER_UI_TEXT_ALIGN_*.
// Returns the block height; outWHL (may be NULL) receives width, height, line count.
AVER_UI_API float aver_ui_text_block(float x, float y, const char* utf8, uint32_t rgba, const char* family, float px,
                                     int32_t flags, float maxWidth, int32_t align, float* outWHL);
// Rasterises the glyphs of a string ahead of use, ignoring the per-frame budget (loading screens).
AVER_UI_API void aver_ui_text_prewarm(const char* utf8, const char* family, float px, int32_t flags);

// ---- hit testing ------------------------------------------------------------------------------------
//
// NOT A WIDGET TREE, deliberately -- see UiDrawList::addHitRect. This is the smallest thing that
// turns "the UI drew a button" into "the pointer is over that button": the draw list already knows
// the clip stack and the layer, which are exactly what decide whether a point reaches a widget.

// Registers a rectangle under a caller-chosen id, clipped and layered like a draw would be.
AVER_UI_API void aver_ui_hit_rect(uint64_t id, float x, float y, float w, float h);

// The id under (x, y), or 0. Topmost wins.
AVER_UI_API uint64_t aver_ui_hit_test(float x, float y);

// ---- the pointer (the host) --------------------------------------------------------------------------
//
// THE POINTER LIVES HERE, NOT ON THE INPUT ABI, and that is the whole point: Aver.Framework's input
// surface reports mouse DELTAS, and a click on a HUD needs an ABSOLUTE position IN THE SAME SPACE AS
// aver_ui_begin_frame's rect. In the editor that rect is the viewport, not the window, so a
// window-relative cursor would be wrong by the dockspace's offset on every click -- the same
// viewport-relative trap NDC already has here. The host converts once, per frame, beside
// begin_frame; a game reads what the host converted.

// Sets this frame's pointer position (in begin_frame's space) and its held buttons as a bitmask:
// bit 0 left, bit 1 right, bit 2 middle. Called by the HOST, never by a game.
AVER_UI_API void aver_ui_set_pointer(float x, float y, uint32_t buttons);

// Writes the pointer as x, y.
AVER_UI_API void aver_ui_pointer(float* outXY);

// 1 when `button` (0 left, 1 right, 2 middle) is held this frame.
AVER_UI_API int32_t aver_ui_pointer_down(int32_t button);

// ---- readback (the host) ----------------------------------------------------------------------------

// Returns the number of vertices submitted this frame.
AVER_UI_API int32_t aver_ui_vertex_count(void);
// Returns the number of draw commands submitted this frame, across all layers.
AVER_UI_API int32_t aver_ui_command_count(void);

// Returns the aver::ui::UiDrawList this module writes into, as an opaque pointer the host casts
// back and only ever reads. NULL before the first begin_frame; valid until the next one.
AVER_UI_API const void* aver_ui_draw_list(void);

#ifdef __cplusplus
} // extern "C"
#endif
