// The Aver.UI C ABI: one process-wide draw list, and the exports that write into it.
#include "aver/ui/ui_abi.h"
#include "UiAbiState.hpp"
#include "aver/ui/UiDrawList.hpp"
#include "aver/ui/UiFont.hpp"
#include "aver/ui/UiText.hpp"
#include "aver/ui/UiTextEngine.hpp"

#include <cstring>
#include <memory>

namespace {

aver::ui::UiDrawList g_list;
aver::ui::UiTextEngine g_engine;
AverUiFontOpenFn g_fontOpen = nullptr;
AverUiFontCloseFn g_fontClose = nullptr;
AverUiLanguageFn g_languageFn = nullptr;
std::string g_textError;
float g_viewport[4] = {0.0f, 0.0f, 0.0f, 0.0f};
bool  g_started = false;

// BORROWED. The host owns the font and its atlas texture; this only ever reads it. See ui_abi.h for
// why it arrives as an opaque pointer rather than a path.
const aver::ui::UiFont* g_font = nullptr;

// THE POINTER SURVIVES begin_frame, unlike the draw list. A host sets it once per frame beside
// begin_frame, and the ORDER of those two calls should not decide whether a game sees a click.
float    g_pointer[2] = {0.0f, 0.0f};
uint32_t g_buttons    = 0;

// The localization resolver the host installed (framework aver_fw_loc_resolve); null = text drawn as given.
aver::ui::abi::TextResolveFn g_resolve = nullptr;

// The font only if it is worth drawing with. `valid()` is glyphs plus a line height, so a font that
// parsed but carries nothing reads as absent rather than as a font that draws nothing.
const aver::ui::UiFont* usableFont() {
    return (g_font && g_font->valid()) ? g_font : nullptr;
}

// What closes a font the host opened: handed back to its close callback when the blob's last owner lets go.
struct FontToken {
    void* token;
    AverUiFontCloseFn close;
};

// The engine's loader over the host's two callbacks (empty when the host installed none).
aver::ui::UiFontLoader makeLoader() {
    aver::ui::UiFontLoader l;
    if (!g_fontOpen) return l;
    const AverUiFontOpenFn open = g_fontOpen;
    const AverUiFontCloseFn close = g_fontClose;
    l.open = [open, close](aver::ui::UiFontKind kind, const std::string& name, aver::ui::UiFontBlob& out) {
        const uint8_t* data = nullptr;
        uint64_t size = 0;
        void* token = nullptr;
        if (open(static_cast<int32_t>(kind), name.c_str(), &data, &size, &token) == 0) return false;
        if (!data || size == 0) {
            if (close) close(token);
            return false;
        }
        out.data = data;
        out.size = static_cast<std::size_t>(size);
        out.owner = std::shared_ptr<void>(new FontToken{token, close}, [](void* p) {
            FontToken* t = static_cast<FontToken*>(p);
            if (t->close) t->close(t->token);
            delete t;
        });
        return true;
    };
    return l;
}

// A call's flags and size as the engine's style and box.
void styleFrom(const char* family, float px, int32_t flags, float maxWidth, aver::ui::UiTextStyle& st,
               aver::ui::UiTextBox& box) {
    st.family = family ? family : "";
    st.px = px > 0.0f ? px : 16.0f;
    st.bold = (flags & AVER_UI_TEXT_FLAG_BOLD) != 0;
    st.italic = (flags & AVER_UI_TEXT_FLAG_ITALIC) != 0;
    box.maxWidth = maxWidth > 0.0f ? maxWidth : 0.0f;
    box.wrap = (flags & AVER_UI_TEXT_FLAG_WRAP) != 0 && maxWidth > 0.0f;
    box.ellipsis = (flags & AVER_UI_TEXT_FLAG_ELLIPSIS) != 0;
    box.rich = (flags & AVER_UI_TEXT_FLAG_RICH) != 0;
}

// The text after localization-reference resolution.
std::string shownText(const char* utf8) {
    std::string resolved;
    if (aver::ui::abi::resolveUiText(utf8, resolved)) return resolved;
    return utf8;
}

} // namespace

namespace aver::ui::abi {

UiDrawList& drawList() { return g_list; }
const UiFont* font() { return usableFont(); }
UiTextEngine& textEngine() { return g_engine; }
void viewport(float out[4]) { for (int i = 0; i < 4; ++i) out[i] = g_viewport[i]; }
void pointer(float& x, float& y, std::uint32_t& buttons) { x = g_pointer[0]; y = g_pointer[1]; buttons = g_buttons; }
bool frameStarted() { return g_started; }

void setTextResolver(TextResolveFn fn) { g_resolve = fn; }

bool resolveUiText(const char* raw, std::string& out) {
    if (!raw || raw[0] != '@') return false;
    if (raw[1] == '@') { out = raw + 1; return true; }   // "@@x" shows "@x"
    UiTextRef ref;
    if (!uiParseTextRef(raw, ref)) return false;
    // No resolver (or it declined): the inline default, as UiTree::shown does.
    const auto fallback = [&] { out = ref.hasDefault ? ref.defaultText : "[" + ref.table + ":" + ref.key + "]"; return true; };
    if (!g_resolve) return fallback();
    std::string buf(256, '\0');
    std::int32_t n = g_resolve(raw, buf.data(), static_cast<std::int32_t>(buf.size()));
    if (n < 0) return fallback();
    if (static_cast<std::size_t>(n) >= buf.size()) {
        buf.assign(static_cast<std::size_t>(n) + 1, '\0');
        n = g_resolve(raw, buf.data(), static_cast<std::int32_t>(buf.size()));
        if (n < 0 || static_cast<std::size_t>(n) >= buf.size()) return fallback();
    }
    buf.resize(static_cast<std::size_t>(n));
    out = std::move(buf);
    return true;
}

} // namespace aver::ui::abi

extern "C" {

// Clears the list and records the frame's viewport rectangle.
void aver_ui_begin_frame(float x, float y, float w, float h) {
    g_list.clear();
    g_viewport[0] = x; g_viewport[1] = y; g_viewport[2] = w; g_viewport[3] = h;
    g_started = true;
    // The language follows the host's culture every frame (a no-op when it did not change); the glyph
    // budget resets here, once per frame, so a layout that ran out of it completes on a later one.
    if (g_languageFn) {
        if (const char* lang = g_languageFn()) g_engine.setLanguage(lang);
    }
    g_engine.beginFrame();
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
    g_engine.setBitmapFont(usableFont());   // the last real fallback face of every chain
}

// 1 when the engine has a TrueType face or a bitmap font is set and has glyphs.
int32_t aver_ui_has_font(void) { return (g_engine.usable() || usableFont()) ? 1 : 0; }

// Writes ascent, descent, lineHeight, or three zeroes.
void aver_ui_font_metrics(float* outAscentDescentLine) {
    if (!outAscentDescentLine) return;
    if (g_engine.usable()) {
        const float px = g_engine.config().legacyPx;
        aver::ui::UiTextStyle st;
        st.px = px;
        const std::shared_ptr<const aver::ui::UiTextLayout> l = g_engine.layout(std::string_view(), st, aver::ui::UiTextBox{});
        outAscentDescentLine[0] = l->lines.empty() ? 0.0f : l->lines[0].ascent;
        outAscentDescentLine[1] = l->lines.empty() ? 0.0f : l->lines[0].descent;
        outAscentDescentLine[2] = l->lines.empty() ? 0.0f : l->lines[0].height;
        return;
    }
    const aver::ui::UiFont* f = usableFont();
    outAscentDescentLine[0] = f ? f->ascent : 0.0f;
    outAscentDescentLine[1] = f ? f->descent : 0.0f;
    outAscentDescentLine[2] = f ? f->lineHeight : 0.0f;
}

// Draws `utf8` with the pen at the left end of the baseline. Returns the pen's x afterwards.
float aver_ui_text(float x, float y, const char* utf8, uint32_t rgba) {
    if (!utf8) return x;
    if (g_engine.usable()) {
        const std::string shown = shownText(utf8);
        aver::ui::UiTextStyle st;
        st.px = g_engine.config().legacyPx;
        const std::shared_ptr<const aver::ui::UiTextLayout> l = g_engine.layout(shown, st, aver::ui::UiTextBox{});
        if (l->lines.empty()) return x;
        const float base0 = l->lines[0].top + l->lines[0].baseline;
        for (std::size_t i = 0; i < l->lines.size(); ++i)
            g_engine.emitLine(g_list, *l, i, x, y + (l->lines[i].top + l->lines[i].baseline - base0), rgba);
        return x + l->lines.back().width;
    }
    const aver::ui::UiFont* f = usableFont();
    if (!f) return x;
    std::string resolved;
    if (aver::ui::abi::resolveUiText(utf8, resolved)) return g_list.addText(x, y, resolved.c_str(), *f, rgba);
    return g_list.addText(x, y, utf8, *f, rgba);
}

// The width `utf8` would occupy with the set font.
float aver_ui_text_width(const char* utf8) {
    if (!utf8) return 0.0f;
    if (g_engine.usable()) return g_engine.textWidth(shownText(utf8), g_engine.config().legacyPx);
    const aver::ui::UiFont* f = usableFont();
    if (!f) return 0.0f;
    std::string resolved;
    if (aver::ui::abi::resolveUiText(utf8, resolved)) return aver::ui::uiTextWidth(*f, resolved.c_str());
    return aver::ui::uiTextWidth(*f, utf8);
}

// ---- the text engine ---------------------------------------------------------------------------

void aver_ui_text_set_loader(AverUiFontOpenFn open, AverUiFontCloseFn close) {
    g_fontOpen = open;
    g_fontClose = close;
    if (!open) {
        // Clearing the loader closes every font opened through it, so no blob outlives the host's callbacks.
        g_engine.fontSet().clear();
        g_engine.atlas().clear();
        g_engine.clearLayouts();
    }
}

int32_t aver_ui_text_load_fontset(const char* ocfontsUtf8) {
    g_textError.clear();
    if (!ocfontsUtf8) { g_textError = "no font set text"; return 0; }
    return g_engine.loadFontSet(ocfontsUtf8, makeLoader(), &g_textError) ? 1 : 0;
}

const char* aver_ui_text_error(void) { return g_textError.c_str(); }

void aver_ui_text_set_language(const char* bcp47) { g_engine.setLanguage(bcp47 ? bcp47 : ""); }

void aver_ui_text_set_language_fn(AverUiLanguageFn fn) {
    g_languageFn = fn;
    if (fn) {
        if (const char* lang = fn()) g_engine.setLanguage(lang);
    }
}

void aver_ui_text_configure(int32_t pageSize, int32_t maxPages, int32_t glyphBudget) {
    aver::ui::UiTextConfig c = g_engine.config();
    if (pageSize > 0) c.pageSize = static_cast<std::uint32_t>(pageSize);
    if (maxPages > 0) c.maxPages = static_cast<std::uint32_t>(maxPages);
    if (glyphBudget > 0) c.glyphBudget = static_cast<std::uint32_t>(glyphBudget);
    g_engine.configure(c);
}

const void* aver_ui_text_atlas(void) { return &g_engine.atlas(); }

void aver_ui_text_stats(float* out12) {
    if (!out12) return;
    const aver::ui::UiTextStats s = g_engine.stats();
    out12[0] = static_cast<float>(s.pages);
    out12[1] = static_cast<float>(s.glyphsResident);
    out12[2] = static_cast<float>(s.rasterThisFrame);
    out12[3] = static_cast<float>(s.rasterTotal);
    out12[4] = static_cast<float>(s.evictedPages);
    out12[5] = static_cast<float>(s.overflow);
    out12[6] = static_cast<float>(s.layoutHits);
    out12[7] = static_cast<float>(s.layoutMisses);
    out12[8] = static_cast<float>(s.cacheEntries);
    out12[9] = static_cast<float>(s.incompleteLayouts);
    out12[10] = static_cast<float>(s.faces);
    out12[11] = static_cast<float>(s.uploadBytesLastFrame);
}

int32_t aver_ui_text_has_glyph(const char* family, uint32_t codepoint) {
    return g_engine.hasGlyph(family ? family : "", codepoint) ? 1 : 0;
}

int32_t aver_ui_text_measure(const char* utf8, const char* family, float px, int32_t flags, float maxWidth, float* outWHL) {
    if (outWHL) outWHL[0] = outWHL[1] = outWHL[2] = 0.0f;
    if (!utf8) return 0;
    const std::string shown = shownText(utf8);
    aver::ui::UiTextStyle st;
    aver::ui::UiTextBox box;
    styleFrom(family, px, flags, maxWidth, st, box);
    aver::ui::UiDrawListPainter painter(g_list, &g_engine, usableFont());
    if (!g_engine.usable() && !usableFont()) return 0;
    const std::shared_ptr<const aver::ui::UiTextLayout> l = painter.layoutText(shown, st, box);
    if (outWHL) {
        outWHL[0] = l->width;
        outWHL[1] = l->height;
        outWHL[2] = static_cast<float>(l->lines.size());
    }
    return 1;
}

float aver_ui_text_block(float x, float y, const char* utf8, uint32_t rgba, const char* family, float px, int32_t flags,
                         float maxWidth, int32_t align, float* outWHL) {
    if (outWHL) outWHL[0] = outWHL[1] = outWHL[2] = 0.0f;
    if (!utf8) return 0.0f;
    if (!g_engine.usable() && !usableFont()) return 0.0f;
    const std::string shown = shownText(utf8);
    aver::ui::UiTextStyle st;
    aver::ui::UiTextBox box;
    styleFrom(family, px, flags, maxWidth, st, box);
    aver::ui::UiDrawListPainter painter(g_list, &g_engine, usableFont());
    const std::shared_ptr<const aver::ui::UiTextLayout> l = painter.layoutText(shown, st, box);
    const float areaW = maxWidth > 0.0f ? maxWidth : l->width;
    const aver::ui::UiTextAlign a = (align >= 0 && align <= 4) ? static_cast<aver::ui::UiTextAlign>(align) : aver::ui::UiTextAlign::Left;
    painter.drawText(*l, aver::ui::UiRect{x, y, areaW, l->height}, a, false, rgba);
    if (outWHL) {
        outWHL[0] = l->width;
        outWHL[1] = l->height;
        outWHL[2] = static_cast<float>(l->lines.size());
    }
    return l->height;
}

void aver_ui_text_prewarm(const char* utf8, const char* family, float px, int32_t flags) {
    if (!utf8 || !g_engine.usable()) return;
    const std::string shown = shownText(utf8);
    aver::ui::UiTextStyle st;
    aver::ui::UiTextBox box;
    styleFrom(family, px, flags, 0.0f, st, box);
    g_engine.prewarm(shown, st, box);
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
