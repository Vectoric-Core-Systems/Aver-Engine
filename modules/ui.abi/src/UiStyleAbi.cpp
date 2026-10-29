// The Aver.UI style ABI (ui_style_abi.h): style boxes, stretch and the default theme text over the C++ in modules/ui.
#include "aver/ui/ui_style_abi.h"
#include "UiAbiState.hpp"
#include "aver/ui/UiStretch.hpp"
#include "aver/ui/UiStyleBox.hpp"
#include "aver/ui/UiThemeSheet.hpp"

#include <algorithm>
#include <cstring>
#include <type_traits>

using namespace aver;
using namespace aver::ui;

static_assert(AVER_UI_SB_FLAT == static_cast<int>(UiStyleBoxKind::Flat) && AVER_UI_SB_TEXTURE == static_cast<int>(UiStyleBoxKind::Texture) &&
              AVER_UI_SB_LINE == static_cast<int>(UiStyleBoxKind::Line) && AVER_UI_SB_EMPTY == static_cast<int>(UiStyleBoxKind::Empty), "style box kinds");
static_assert(AVER_UI_SBF_DRAW_CENTER == kSbDrawCenter && AVER_UI_SBF_ANTI_ALIAS == kSbAntiAlias && AVER_UI_SBF_BORDER_BLEND == kSbBorderBlend &&
              AVER_UI_SBF_VERTICAL == kSbVertical, "style box flags");
static_assert(std::is_trivially_copyable_v<UiStyleBox> && std::is_standard_layout_v<UiStyleBox>, "UiStyleBox crosses the C seam by pointer");

extern "C" {

int32_t aver_ui_style_abi_version(void) { return AVER_UI_STYLE_ABI_VERSION; }

int32_t aver_ui_stylebox_struct_size(void) { return static_cast<int32_t>(sizeof(UiStyleBox)); }

void aver_ui_stylebox_defaults(int32_t kind, void* outStyleBox) {
    if (!outStyleBox) return;
    UiStyleBox sb;
    sb.kind = kind;
    switch (static_cast<UiStyleBoxKind>(kind)) {
        case UiStyleBoxKind::Texture: sb.flags = kSbDrawCenter; break;
        case UiStyleBoxKind::Line: sb.flags = 0; break;
        case UiStyleBoxKind::Empty: sb.flags = 0; break;
        default: sb.kind = static_cast<i32>(UiStyleBoxKind::Flat); sb.flags = kSbDrawCenter | kSbAntiAlias; break;   // Godot's StyleBoxFlat: anti_aliasing is on
    }
    std::memcpy(outStyleBox, &sb, sizeof sb);
}

void aver_ui_stylebox_draw(const void* styleBox, float x, float y, float w, float h) {
    if (!styleBox) return;
    UiStyleBox sb;
    std::memcpy(&sb, styleBox, sizeof sb);
    uiStyleBoxDraw(abi::drawList(), sb, UiRect{x, y, w, h});
}

int32_t aver_ui_stylebox_mesh(const void* styleBox, float x, float y, float w, float h, float* outXYUV, uint32_t* outRgba, int32_t maxVerts,
                              uint32_t* outIdx, int32_t maxIdx, int32_t* outCounts) {
    if (!styleBox) {
        if (outCounts) outCounts[0] = outCounts[1] = outCounts[2] = outCounts[3] = 0;
        return 0;
    }
    UiStyleBox sb;
    std::memcpy(&sb, styleBox, sizeof sb);
    UiMesh m;
    uiStyleBoxMesh(sb, UiRect{x, y, w, h}, m);
    if (outCounts) {
        outCounts[0] = static_cast<int32_t>(m.verts.size());
        outCounts[1] = static_cast<int32_t>(m.idx.size());
        outCounts[2] = static_cast<int32_t>(m.texture & 0xFFFFFFFFull);
        outCounts[3] = static_cast<int32_t>(m.texture >> 32);
    }
    if (static_cast<int32_t>(m.verts.size()) > maxVerts || static_cast<int32_t>(m.idx.size()) > maxIdx) return 0;
    for (usize i = 0; i < m.verts.size(); ++i) {
        if (outXYUV) { outXYUV[i * 4 + 0] = m.verts[i].x; outXYUV[i * 4 + 1] = m.verts[i].y; outXYUV[i * 4 + 2] = m.verts[i].u; outXYUV[i * 4 + 3] = m.verts[i].v; }
        if (outRgba) outRgba[i] = m.verts[i].rgba;
    }
    if (outIdx) for (usize i = 0; i < m.idx.size(); ++i) outIdx[i] = m.idx[i];
    return 1;
}

void aver_ui_stylebox_metrics(const void* styleBox, float* out6) {
    if (!out6) return;
    std::fill(out6, out6 + 6, 0.0f);
    if (!styleBox) return;
    UiStyleBox sb;
    std::memcpy(&sb, styleBox, sizeof sb);
    const UiInsets m = uiStyleBoxMargins(sb);
    out6[0] = m.horizontal(); out6[1] = m.vertical();
    out6[2] = m.left; out6[3] = m.top; out6[4] = m.right; out6[5] = m.bottom;
}

int32_t aver_ui_ninepatch_slices(const void* styleBox, float x, float y, float w, float h, float* outCells, int32_t maxCells) {
    if (!styleBox) return 0;
    UiStyleBox sb;
    std::memcpy(&sb, styleBox, sizeof sb);
    const std::vector<UiPatchSlice> cells = uiNinePatchSlices(sb, UiRect{x, y, w, h});
    if (outCells)
        for (int32_t i = 0; i < maxCells && i < static_cast<int32_t>(cells.size()); ++i) {
            const UiPatchSlice& c = cells[static_cast<usize>(i)];
            const float v[8] = {c.dst.x, c.dst.y, c.dst.w, c.dst.h, c.u0, c.v0, c.u1, c.v1};
            std::memcpy(outCells + i * 8, v, sizeof v);
        }
    return static_cast<int32_t>(cells.size());
}

void aver_ui_stretch_compute(const float* in6, float windowW, float windowH, float* out11) {
    if (!out11) return;
    std::fill(out11, out11 + 11, 0.0f);
    if (!in6) return;
    UiStretchSettings s;
    s.mode = static_cast<UiStretchMode>(static_cast<int>(in6[0]));
    s.aspect = static_cast<UiStretchAspect>(static_cast<int>(in6[1]));
    s.scaleMode = static_cast<UiStretchScaleMode>(static_cast<int>(in6[2]));
    s.baseW = in6[3]; s.baseH = in6[4]; s.scale = in6[5];
    const UiStretchResult r = uiComputeStretch(s, windowW, windowH);
    const float v[11] = {r.screen.x, r.screen.y, r.screen.w, r.screen.h, r.contentSize.x, r.contentSize.y, r.renderSize.x, r.renderSize.y,
                         r.scaleX, r.scaleY, r.letterboxed ? 1.0f : 0.0f};
    std::memcpy(out11, v, sizeof v);
}

int32_t aver_ui_theme_default_text(void* outText, int32_t cap) {
    const std::string& t = uiGodotDefaultThemeText();
    if (outText && cap > 0) {
        char* dst = static_cast<char*>(outText);
        const usize n = std::min(t.size(), static_cast<usize>(cap - 1));
        std::memcpy(dst, t.data(), n);
        dst[n] = '\0';
    }
    return static_cast<int32_t>(t.size());
}

int32_t aver_ui_theme_validate(const char* text, void* errOut, int32_t cap) {
    UiThemeSheet sheet;
    std::string err;
    const bool ok = uiThemeParse(text ? text : "", sheet, &err);
    if (!ok && errOut && cap > 0) {
        char* dst = static_cast<char*>(errOut);
        const usize n = std::min(err.size(), static_cast<usize>(cap - 1));
        std::memcpy(dst, err.data(), n);
        dst[n] = '\0';
    }
    return ok ? 1 : 0;
}

} // extern "C"
