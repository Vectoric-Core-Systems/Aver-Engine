#pragma once
// Aver.UI — the plain-C ABI, following the engine's idiom: int32/float/const char* cross, UTF-8
// strings, setters return 1/0, and 0 is ALWAYS an invalid handle.
//
// This is the SEVENTH seam (see docs/ABI.md). It exists so a game's HUD can be authored in the
// game's own language rather than in the engine. A HUD is content -- it knows about ammunition and
// objectives and the shape of one particular game -- and content authored in engine C++ is content
// that ships with the engine, which is the wrong side of the line for every project but the one it
// was written for.
//
// SCREEN PIXELS, TOP-LEFT ORIGIN, exactly as Aver.UI states. Not normalised coordinates: a UI is
// authored against a resolution and every layout number a designer types is a pixel. Ask for the
// viewport rather than assuming one -- see aver_ui_viewport.
//
// THE HOST OWNS THE FRAME. aver_ui_begin_frame is called by whatever runs the game loop, once, before
// gameplay ticks; the game only ever draws. A game that called it would clear whatever another
// system had already contributed, and the last caller would win with nothing to say so.
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

// Layer bands, matching aver::ui::UiLayer one for one. Coarse and named on purpose: a widget picks
// the band it belongs in, not a number it has to reason about relative to every other widget. Order
// WITHIN a band is submission order.
#define AVER_UI_LAYER_BACKGROUND 0
#define AVER_UI_LAYER_CONTENT    1   /* the HUD and ordinary widgets; the default */
#define AVER_UI_LAYER_OVERLAY    2   /* menus and modal panels */
#define AVER_UI_LAYER_TOOLTIP    3
#define AVER_UI_LAYER_DEBUG      4

// ---- the frame ------------------------------------------------------------------------------------

// Clear the list and declare the rectangle this frame's UI is laid out against, in backbuffer pixels.
// Called by the HOST, once per frame, before anything ticks.
//
// The rectangle is passed rather than queried because this module has no device and no window: it is
// Core-only for the same reason Aver.UI is, and asking it for a screen size would be the first thing
// that made it need one.
AVER_UI_API void aver_ui_begin_frame(float x, float y, float w, float h);

// The rectangle begin_frame was given: x, y, width, height. A HUD anchors to this, NOT to the
// window -- in the editor they differ, because the game is drawn into a dockspace panel, and a HUD
// laid out against the window would sit partly under the editor's own chrome.
AVER_UI_API void aver_ui_viewport(float* outXYWH);

// ---- state ----------------------------------------------------------------------------------------

AVER_UI_API void    aver_ui_set_layer(int32_t layer);   // out-of-range is ignored, not clamped
AVER_UI_API int32_t aver_ui_layer(void);

// Nested clips INTERSECT rather than replace, so a child can never escape its parent by pushing a
// larger rectangle. Coordinates are edges, not an extent: left, top, right, bottom.
AVER_UI_API void aver_ui_push_clip(int32_t left, int32_t top, int32_t right, int32_t bottom);
AVER_UI_API void aver_ui_pop_clip(void);

// ---- geometry ---------------------------------------------------------------------------------------
//
// `rgba` is STRAIGHT (non-premultiplied) 0xAABBGGRR -- the byte order a R8G8B8A8_UNORM vertex
// attribute reads on a little-endian machine. The premultiplication happens on the way in, so a
// caller never has to think about it. The managed binding offers the familiar 0xAARRGGBB spelling
// and swizzles; this level keeps the one packing the vertex actually holds.

AVER_UI_API void aver_ui_rect(float x, float y, float w, float h, uint32_t rgba);

// `texture` is 0 for the built-in white texel, or a value the renderer recognises. The texture is
// required to be PREMULTIPLIED, like the colour: a straight-alpha texture multiplied by a
// premultiplied colour produces a halo that reads as a filtering artefact and is not one.
AVER_UI_API void aver_ui_textured_rect(float x, float y, float w, float h, uint64_t texture,
                                       float u0, float v0, float u1, float v1, uint32_t rgba);

// ---- readback (the host) ----------------------------------------------------------------------------

AVER_UI_API int32_t aver_ui_vertex_count(void);
AVER_UI_API int32_t aver_ui_command_count(void);

// The aver::ui::UiDrawList this module writes into, as an opaque pointer the host casts back.
//
// A C++ TYPE ACROSS A DLL BOUNDARY, which the engine's other DLLs deliberately avoid -- so the
// reason this one is safe is worth stating rather than assuming. The rule those modules follow is
// that only the DLL may ALLOCATE or FREE; the hazard is a std::vector grown by one heap and released
// by another. This pointer is const and the host only ever reads through it: every allocation the
// list makes happens inside this DLL, on this side of the boundary, and the host copies the bytes it
// wants into its own storage. Both sides compile the same header from the same tree with the same
// toolchain, so the layout is not a matter of hope.
//
// Returns NULL before the first begin_frame. Valid until the next one.
AVER_UI_API const void* aver_ui_draw_list(void);

#ifdef __cplusplus
} // extern "C"
#endif
