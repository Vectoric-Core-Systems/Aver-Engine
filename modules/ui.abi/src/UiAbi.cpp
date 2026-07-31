// The Aver.UI C ABI: one process-wide draw list, and the exports that write into it.
#include "aver/ui/ui_abi.h"
#include "aver/ui/UiDrawList.hpp"

namespace {

aver::ui::UiDrawList g_list;
float g_viewport[4] = {0.0f, 0.0f, 0.0f, 0.0f};
bool  g_started = false;

} // namespace

extern "C" {

// Clears the list and records the frame's viewport rectangle.
void aver_ui_begin_frame(float x, float y, float w, float h) {
    g_list.clear();
    g_viewport[0] = x; g_viewport[1] = y; g_viewport[2] = w; g_viewport[3] = h;
    g_started = true;
}

// Writes the frame's viewport rectangle as x, y, width, height.
void aver_ui_viewport(float* outXYWH) {
    if (!outXYWH) return;
    for (int i = 0; i < 4; ++i) outXYWH[i] = g_viewport[i];
}

// Sets the layer subsequent draws go to. Out-of-range is ignored, not clamped.
void aver_ui_set_layer(int32_t layer) {
    if (layer < 0 || layer >= static_cast<int32_t>(aver::ui::UiLayer::Count)) return;
    g_list.setLayer(static_cast<aver::ui::UiLayer>(layer));
}

// Returns the current layer.
int32_t aver_ui_layer(void) { return static_cast<int32_t>(g_list.layer()); }

// Pushes a clip rect, intersected with the one in force.
void aver_ui_push_clip(int32_t left, int32_t top, int32_t right, int32_t bottom) {
    g_list.pushClip(aver::ui::UiClip{left, top, right, bottom});
}

// Pops the innermost clip rect.
void aver_ui_pop_clip(void) { g_list.popClip(); }

// Draws a solid rectangle. `rgba` is straight (non-premultiplied) 0xAABBGGRR.
void aver_ui_rect(float x, float y, float w, float h, uint32_t rgba) {
    g_list.addRect(x, y, w, h, rgba);
}

// Draws a textured rectangle. `texture` is 0 for the built-in white texel.
void aver_ui_textured_rect(float x, float y, float w, float h, uint64_t texture,
                           float u0, float v0, float u1, float v1, uint32_t rgba) {
    g_list.addTexturedRect(x, y, w, h, texture, u0, v0, u1, v1, rgba);
}

// Returns the number of vertices submitted this frame.
int32_t aver_ui_vertex_count(void)  { return static_cast<int32_t>(g_list.vertices().size()); }
// Returns the number of draw commands submitted this frame, across all layers.
int32_t aver_ui_command_count(void) { return static_cast<int32_t>(g_list.totalCommands()); }

// Returns the draw list as an opaque pointer. NULL before the first begin_frame.
const void* aver_ui_draw_list(void) { return g_started ? &g_list : nullptr; }

} // extern "C"
