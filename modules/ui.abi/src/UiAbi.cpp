// The Aver.UI C ABI: one process-wide draw list, and the exports that write into it.
#include "aver/ui/ui_abi.h"
#include "UiAbiState.hpp"
#include "aver/ui/UiDrawList.hpp"
#include "aver/ui/UiFont.hpp"

namespace {

aver::ui::UiDrawList g_list;
float g_viewport[4] = {0.0f, 0.0f, 0.0f, 0.0f};
bool  g_started = false;

// BORROWED. The host owns the font and its atlas texture; this only ever reads it. See ui_abi.h for
// why it arrives as an opaque pointer rather than a path.
const aver::ui::UiFont* g_font = nullptr;

// THE POINTER SURVIVES begin_frame, unlike the draw list. A host sets it once per frame beside
// begin_frame, and the ORDER of those two calls should not decide whether a game sees a click.
float    g_pointer[2] = {0.0f, 0.0f};
uint32_t g_buttons    = 0;

// The font only if it is worth drawing with. `valid()` is glyphs plus a line height, so a font that
// parsed but carries nothing reads as absent rather than as a font that draws nothing.
const aver::ui::UiFont* usableFont() {
    return (g_font && g_font->valid()) ? g_font : nullptr;
}

} // namespace

namespace aver::ui::abi {

UiDrawList& drawList() { return g_list; }
const UiFont* font() { return usableFont(); }
void viewport(float out[4]) { for (int i = 0; i < 4; ++i) out[i] = g_viewport[i]; }
void pointer(float& x, float& y, std::uint32_t& buttons) { x = g_pointer[0]; y = g_pointer[1]; buttons = g_buttons; }
bool frameStarted() { return g_started; }

} // namespace aver::ui::abi

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

// ---- text -------------------------------------------------------------------------------------

// Lends this module the host's font. NULL clears it.
void aver_ui_set_font(const void* font) {
    g_font = static_cast<const aver::ui::UiFont*>(font);
}

// 1 when a font is set and has glyphs.
int32_t aver_ui_has_font(void) { return usableFont() ? 1 : 0; }

// Writes ascent, descent, lineHeight, or three zeroes.
void aver_ui_font_metrics(float* outAscentDescentLine) {
    if (!outAscentDescentLine) return;
    const aver::ui::UiFont* f = usableFont();
    outAscentDescentLine[0] = f ? f->ascent : 0.0f;
    outAscentDescentLine[1] = f ? f->descent : 0.0f;
    outAscentDescentLine[2] = f ? f->lineHeight : 0.0f;
}

// Draws `utf8` with the pen at the left end of the baseline. Returns the pen's x afterwards.
float aver_ui_text(float x, float y, const char* utf8, uint32_t rgba) {
    const aver::ui::UiFont* f = usableFont();
    if (!f || !utf8) return x;
    return g_list.addText(x, y, utf8, *f, rgba);
}

// The width `utf8` would occupy with the set font.
float aver_ui_text_width(const char* utf8) {
    const aver::ui::UiFont* f = usableFont();
    if (!f || !utf8) return 0.0f;
    return aver::ui::uiTextWidth(*f, utf8);
}

// ---- hit testing --------------------------------------------------------------------------------

// Registers a rectangle under `id`, clipped and layered like a draw would be.
void aver_ui_hit_rect(uint64_t id, float x, float y, float w, float h) {
    g_list.addHitRect(id, x, y, w, h);
}

// The id under (x, y), or 0. Topmost wins.
uint64_t aver_ui_hit_test(float x, float y) { return g_list.hitTest(x, y); }

// ---- the pointer --------------------------------------------------------------------------------

// Sets this frame's pointer, in begin_frame's space. Called by the host.
void aver_ui_set_pointer(float x, float y, uint32_t buttons) {
    g_pointer[0] = x; g_pointer[1] = y;
    g_buttons = buttons;
}

// Writes the pointer as x, y.
void aver_ui_pointer(float* outXY) {
    if (!outXY) return;
    outXY[0] = g_pointer[0]; outXY[1] = g_pointer[1];
}

// 1 when `button` is held. Out-of-range is "not held", not an error: a game asking about a fourth
// button should get a quiet no rather than a crash.
int32_t aver_ui_pointer_down(int32_t button) {
    if (button < 0 || button > 31) return 0;
    return (g_buttons & (1u << static_cast<uint32_t>(button))) ? 1 : 0;
}

// Returns the number of vertices submitted this frame.
int32_t aver_ui_vertex_count(void)  { return static_cast<int32_t>(g_list.vertices().size()); }
// Returns the number of draw commands submitted this frame, across all layers.
int32_t aver_ui_command_count(void) { return static_cast<int32_t>(g_list.totalCommands()); }

// Returns the draw list as an opaque pointer. NULL before the first begin_frame.
const void* aver_ui_draw_list(void) { return g_started ? &g_list : nullptr; }

} // extern "C"
