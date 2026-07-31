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
