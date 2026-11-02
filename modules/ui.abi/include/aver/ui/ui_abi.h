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

// ---- transform and draw state (parity P40, P41) ------------------------------------------------------
//
// A 2D affine transform applied to every draw added after it: x' = a*x + c*y + tx, y' = b*x + d*y + ty, composed on
// the one in force (the new matrix is applied first). Rectangles, glyph quads, style boxes and meshes are transformed
// vertex by vertex; a clip rect and a hit rect become the axis-aligned box of their transformed corners. A matrix with a
// NaN or infinity is ignored, and its pop still pairs. The stack is emptied by aver_ui_begin_frame.
AVER_UI_API void aver_ui_push_transform(float a, float b, float c, float d, float tx, float ty);
AVER_UI_API void aver_ui_pop_transform(void);

// How draws added after this sample their texture. filter: 0 linear (default), 1 nearest. address: 0 clamp (default),
// 1 repeat, 2 mirror. Out-of-range is ignored. Reset to the defaults by aver_ui_begin_frame.
AVER_UI_API void aver_ui_set_sampler(int32_t filter, int32_t address);
// How draws added after this combine with the backbuffer: 0 mix (default), 1 add, 2 subtract, 3 multiply, 4 disabled
// (the colour is written as it is). Out-of-range is ignored. Reset by aver_ui_begin_frame.
AVER_UI_API void aver_ui_set_blend(int32_t mode);
#define AVER_UI_FILTER_LINEAR  0
#define AVER_UI_FILTER_NEAREST 1
#define AVER_UI_ADDRESS_CLAMP  0
#define AVER_UI_ADDRESS_REPEAT 1
#define AVER_UI_ADDRESS_MIRROR 2
#define AVER_UI_BLEND_MIX      0
#define AVER_UI_BLEND_ADD      1
#define AVER_UI_BLEND_SUB      2
#define AVER_UI_BLEND_MUL      3
#define AVER_UI_BLEND_DISABLED 4

// ---- geometry ---------------------------------------------------------------------------------------

// Draws triangles built by the caller. xyuv is x, y, u, v per vertex (4 floats), rgba is straight 0xAABBGGRR per vertex
// (premultiplied on the way in), indices are relative to the first vertex. `texture` as aver_ui_textured_rect. Nothing is
// drawn for a count that is not positive, an index past the vertices, or an empty clip. At most 65536 vertices and 262144
// indices per call.
AVER_UI_API void aver_ui_mesh(const float* xyuv, const uint32_t* rgba, int32_t vertexCount, const uint32_t* indices,
                              int32_t indexCount, uint64_t texture);

// Draws a solid rectangle. `rgba` is straight (non-premultiplied) 0xAABBGGRR.
AVER_UI_API void aver_ui_rect(float x, float y, float w, float h, uint32_t rgba);

// Draws a textured rectangle. `texture` is 0 for the built-in white texel; the texels must be
// premultiplied (the colour is premultiplied on the way in). aver_ui_image_acquire below makes a
// texture of a content image, premultiplied.
AVER_UI_API void aver_ui_textured_rect(float x, float y, float w, float h, uint64_t texture,
                                       float u0, float v0, float u1, float v1, uint32_t rgba);

// ---- images (a content path as a UI texture) ----------------------------------------------------------
//
// The id aver_ui_textured_rect and a style box take is a GPU texture, and this DLL links no RHI, so the HOST
// loads the file, uploads it and lends the result through these callbacks (the same arrangement as the font
// loader). The host keeps one table: a path is decoded and uploaded once, premultiplied, and shared by every
// caller. `path` is relative to the project's Content folder, with forward or back slashes ("Textures/ui/a.png");
// an absolute path, a drive or scheme, or ".." is refused. Any image the engine decodes (PNG, JPEG, TGA, BMP,
// WebP, ...). Main thread only, like the rest of this header.
//
// An id is valid until it is released and a few frames have passed (the host never destroys a texture a frame in
// flight still names), and until the host ends the session (Play stop), which drops every reference: a game that
// outlives that re-acquires.
typedef uint64_t (*AverUiImageAcquireFn)(const char* utf8Path);          /* id with one more reference, 0 on failure */
typedef void     (*AverUiImageReleaseFn)(uint64_t id);
typedef int32_t  (*AverUiImageSizeFn)(uint64_t id, int32_t* outWH);      /* 1 and the source size in pixels, 0 unknown id */
typedef uint64_t (*AverUiImageUseFn)(const char* utf8Path);              /* id WITHOUT a reference, kept alive while asked for */

// Installs the host's image table (host only). NULL acquire clears it; the other callbacks may be NULL.
AVER_UI_API void aver_ui_set_image_host(AverUiImageAcquireFn acquire, AverUiImageReleaseFn release,
                                        AverUiImageSizeFn size, AverUiImageUseFn use);
// 1 when a host installed an image table.
AVER_UI_API int32_t aver_ui_image_available(void);
// The texture id for a content path with one more reference, loading it on first use. 0 when there is no host table,
// the path is refused or the image does not load (the host logs why once). Pair with aver_ui_image_release.
AVER_UI_API uint64_t aver_ui_image_acquire(const char* utf8Path);
// Drops one reference. An unknown id, or one with none left, is ignored.
AVER_UI_API void aver_ui_image_release(uint64_t id);
// Writes the image's width and height in pixels to outWH (2 ints). 1 on success, 0 (outWH zeroed) for an unknown id.
AVER_UI_API int32_t aver_ui_image_size(uint64_t id, int32_t* outWH);
// The id for a path without taking a reference, for something that draws the same path every frame and never
// lets go. The texture lives while it keeps being asked for. 0 as acquire.
AVER_UI_API uint64_t aver_ui_image_use(const char* utf8Path);

// ---- the clipboard (parity E16) ---------------------------------------------------------------------------
//
// Like the image table, the DLL links no platform layer, so the HOST lends the operating system's text clipboard through two callbacks and the editing
// controls (a LineEdit's copy and paste) call these. Without a host the calls fail and a control keeps its own in-process buffer.
//  get: writes the clipboard's text as UTF-8 to `out` (at most cap - 1 bytes and a NUL; `out` may be NULL with cap 0 to ask the size) and returns the
//       text's full length in bytes without the NUL, or -1 when the clipboard holds no text.
//  set: replaces the clipboard's text; 1 on success, 0 on failure.
typedef int32_t (*AverUiClipboardGetFn)(char* out, int32_t cap);
typedef int32_t (*AverUiClipboardSetFn)(const char* utf8);

// Installs the host's clipboard (host only). NULL clears it.
AVER_UI_API void aver_ui_set_clipboard_host(AverUiClipboardGetFn get, AverUiClipboardSetFn set);
// 1 when a host installed a clipboard.
AVER_UI_API int32_t aver_ui_clipboard_available(void);
// See `get` above. -1 with no host or no text.
AVER_UI_API int32_t aver_ui_clipboard_get(char* out, int32_t cap);
// See `set` above. 0 with no host, a NULL string, or a failure.
AVER_UI_API int32_t aver_ui_clipboard_set(const char* utf8);

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
// A raw deflate decoder (RFC 1951, no zlib header) the host lends, so WOFF 1.0 web fonts (".woff", wrapping zlib-compressed sfnt
// tables) load: decompresses `in` into `out`, which the stream must fill with exactly outSize bytes; 1 on success, 0 otherwise.
// Without one a WOFF is refused (aver_ui_text_error says so). WOFF2 needs Brotli and is refused either way. NULL clears it.
typedef int32_t (*AverUiInflateFn)(const uint8_t* in, uint64_t inSize, uint8_t* out, uint64_t outSize);
AVER_UI_API void aver_ui_text_set_inflate(AverUiInflateFn inflate);
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
// aver_ui_text_block with an outline and a shadow (outline size / outline colour / shadow colour / shadow offset /
// shadow outline size). The text is drawn as several passes through the same path: the shadow's outline, the shadow, the outline ring,
// then the fill in `rgba`. outlineSize 0 and a shadowRgba with alpha 0 draw exactly aver_ui_text_block. Colours are straight 0xAABBGGRR.
// Returns the block height; outWHL (may be NULL) receives the block's width, height and line count (the outline is not counted).
AVER_UI_API float aver_ui_text_block_fx(float x, float y, const char* utf8, uint32_t rgba, const char* family, float px,
                                        int32_t flags, float maxWidth, int32_t align, float outlineSize, uint32_t outlineRgba,
                                        uint32_t shadowRgba, float shadowOffX, float shadowOffY, float shadowOutlineSize,
                                        float* outWHL);
// P38: the same three calls with a spacing block, `spacing4` = {line height multiplier (<= 0 keeps 1), pixels added after
// every glyph, pixels added after every space or tab (on top of the glyph spacing), pixels added to every line advance}.
// NULL is all defaults, and the calls above are these with NULL. Values are clamped, NaN reads as 0.
AVER_UI_API int32_t aver_ui_text_measure_ex(const char* utf8, const char* family, float px, int32_t flags, float maxWidth,
                                            const float* spacing4, float* outWHL);
AVER_UI_API float aver_ui_text_block_ex(float x, float y, const char* utf8, uint32_t rgba, const char* family, float px,
                                        int32_t flags, float maxWidth, int32_t align, const float* spacing4, float* outWHL);
AVER_UI_API float aver_ui_text_block_fx_ex(float x, float y, const char* utf8, uint32_t rgba, const char* family, float px,
                                           int32_t flags, float maxWidth, int32_t align, float outlineSize, uint32_t outlineRgba,
                                           uint32_t shadowRgba, float shadowOffX, float shadowOffY, float shadowOutlineSize,
                                           const float* spacing4, float* outWHL);
// Rasterises the glyphs of a string ahead of use, ignoring the per-frame budget (loading screens).
AVER_UI_API void aver_ui_text_prewarm(const char* utf8, const char* family, float px, int32_t flags);

// ---- text in the world (parity E6, Label3D's engine half) ---------------------------------------------
//
// A label is text laid out by the same engine as the UI, drawn as glyph quads in a plane that a matrix places in the
// world. Immediate mode like the rest of this header: call aver_text3d_draw every frame a label should show; the list is
// emptied by aver_ui_begin_frame. The quads come from a WORLD glyph atlas of their own (aver_text3d_atlas), so a HUD that
// churns the UI atlas never recycles a page a label is using, and the pages are plain textures a material may read.
//
// DRAWING. The renderer draws labels in the scene's transparent pass with its own plain raster shader: no lighting, no ray
// query, no loop. It is NOT drawn in ray-driven mode unless the host opts in (UiRenderer::setWorldTextInRayDriven), because
// that mode replaces the pass labels would draw in.
#define AVER_TEXT3D_NO_DEPTH    1   /* drawn over the scene instead of depth-tested against it */
#define AVER_TEXT3D_ADDITIVE    2   /* added to the scene instead of premultiplied alpha */
#define AVER_TEXT3D_BILLBOARD   4   /* the plane turns to face the camera */
#define AVER_TEXT3D_BILLBOARD_Y 8   /* the plane turns about the world's up axis only */
#define AVER_TEXT3D_ONE_SIDED   16  /* the back of the plane is culled */
#define AVER_TEXT3D_H_LEFT   0
#define AVER_TEXT3D_H_CENTER 1
#define AVER_TEXT3D_H_RIGHT  2
#define AVER_TEXT3D_V_TOP    0
#define AVER_TEXT3D_V_CENTER 1
#define AVER_TEXT3D_V_BOTTOM 2

typedef struct AverText3dDesc {
    int32_t size;          /* sizeof(AverText3dDesc), so the struct can grow; a smaller size reads only what it holds */
    float world[16];       /* row-major, row vectors: plane (x, y, 0, 1) * world is the position in the scene frame */
    char family[64];       /* UTF-8 font family, "" = the default */
    float px;              /* em size in layout pixels (0 = 32); glyphs are rasterised at this size unless the world atlas is SDF */
    float pixelSize;       /* plane units per layout pixel (0 = 0.01) */
    float width;           /* wrap / ellipsis width in layout pixels, 0 = none */
    int32_t flags;         /* AVER_UI_TEXT_FLAG_* */
    int32_t hAlign;        /* AVER_TEXT3D_H_* of the text about the plane's origin */
    int32_t vAlign;        /* AVER_TEXT3D_V_* */
    float spacing[4];      /* as aver_ui_text_block_ex: line multiplier (0 = 1), per glyph, per space, per line (layout pixels) */
    uint32_t rgba;         /* straight 0xAABBGGRR */
    uint32_t outlineRgba;
    float outlineSize;     /* layout pixels, 0 = none */
    float offsetX, offsetY;/* layout pixels, +y up, from the anchor */
    int32_t drawFlags;     /* AVER_TEXT3D_* */
    float alphaCut;        /* pixels with less alpha are dropped; 0 = none */
    float intensity;       /* colour multiplier in scene units, 0 = 1 */
    float fixedPxPerUnit;  /* > 0: constant on-screen size, one plane unit drawn as this many pixels */
    float sdfSoftness;     /* edge softness of distance-field glyphs, 0 = the renderer's default */
    int32_t maxLines;      /* 0 = unlimited */
} AverText3dDesc;

/* 1 when the label was added to this frame's list; 0 with no usable font, empty text, a bad descriptor or a full list.
   outWHL (may be NULL) receives the text box's width and height in plane units and the line count. */
AVER_UI_API int32_t aver_text3d_draw(const char* utf8, const AverText3dDesc* desc, float* outWHL);
/* The same layout without adding anything: width, height (plane units), line count. 1 when measured. */
AVER_UI_API int32_t aver_text3d_measure(const char* utf8, const AverText3dDesc* desc, float* outWHL);
/* The world glyph atlas as an opaque aver::ui::UiGlyphAtlas*, for the renderer (UiRenderer::setWorldAtlas). */
AVER_UI_API const void* aver_text3d_atlas(void);
/* World glyphs as distance fields (sdf != 0): rasterised once at sdfPx with sdfSpread pixels of padding, scaling to any size.
   A value < 0 keeps the current one. Takes effect at once and drops the world atlas. */
AVER_UI_API void aver_text3d_configure(int32_t sdf, int32_t sdfPx, int32_t sdfSpread, int32_t pageSize, int32_t maxPages);
/* labels this frame, quads this frame, pages, glyphs resident, evicted pages, overflow, mesh builds, mesh rebuilds after a
   page was recycled. */
AVER_UI_API void aver_text3d_stats(float* out8);

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
