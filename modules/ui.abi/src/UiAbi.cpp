#include "aver/ui/ui_abi.h"
#include "aver/ui/UiDrawList.hpp"

// The C ABI's state, and there is deliberately only one of it.
//
// A per-caller draw list would let two systems each build a HUD and neither see the other's, which
// is not composition -- it is two UIs racing for the same screen with the last submitter winning.
// One list, layers to separate what goes where, and submission order within a layer, is the model
// Aver.UI already states; this is that model reaching C.
namespace {

aver::ui::UiDrawList g_list;
float g_viewport[4] = {0.0f, 0.0f, 0.0f, 0.0f};
bool  g_started = false;

} // namespace

extern "C" {

void aver_ui_begin_frame(float x, float y, float w, float h) {
    g_list.clear();
    g_viewport[0] = x; g_viewport[1] = y; g_viewport[2] = w; g_viewport[3] = h;
    g_started = true;
}

void aver_ui_viewport(float* outXYWH) {
    if (!outXYWH) return;
    for (int i = 0; i < 4; ++i) outXYWH[i] = g_viewport[i];
}

void aver_ui_set_layer(int32_t layer) {
    // Ignored rather than clamped when out of range. Clamping would silently move a widget to a
    // band its author did not choose -- Debug becoming Tooltip is a shipped debug overlay -- and the
    // caller has no way to notice. Leaving the layer alone at least keeps it where it was.
    if (layer < 0 || layer >= static_cast<int32_t>(aver::ui::UiLayer::Count)) return;
    g_list.setLayer(static_cast<aver::ui::UiLayer>(layer));
}

int32_t aver_ui_layer(void) { return static_cast<int32_t>(g_list.layer()); }

void aver_ui_push_clip(int32_t left, int32_t top, int32_t right, int32_t bottom) {
    g_list.pushClip(aver::ui::UiClip{left, top, right, bottom});
}

void aver_ui_pop_clip(void) { g_list.popClip(); }

void aver_ui_rect(float x, float y, float w, float h, uint32_t rgba) {
    g_list.addRect(x, y, w, h, rgba);
}

void aver_ui_textured_rect(float x, float y, float w, float h, uint64_t texture,
                           float u0, float v0, float u1, float v1, uint32_t rgba) {
    g_list.addTexturedRect(x, y, w, h, texture, u0, v0, u1, v1, rgba);
}

int32_t aver_ui_vertex_count(void)  { return static_cast<int32_t>(g_list.vertices().size()); }
int32_t aver_ui_command_count(void) { return static_cast<int32_t>(g_list.totalCommands()); }

const void* aver_ui_draw_list(void) { return g_started ? &g_list : nullptr; }

} // extern "C"
