// Headless test for the style ABI (ui_style_abi.h): the seam Aver.Fabric calls for style boxes, stretch and the default theme.
#include "aver/core/Log.hpp"
#include "aver/ui/UiStyleBox.hpp"
#include "aver/ui/ui_style_abi.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::ui;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }

int main() {
    check(aver_ui_style_abi_version() == AVER_UI_STYLE_ABI_VERSION, "the ABI version");
    check(aver_ui_stylebox_struct_size() == 200, "the style box struct is 200 bytes (the C# mirror asserts the same)");

    AVER_INFO("=== defaults ===");
    {
        UiStyleBox sb;
        aver_ui_stylebox_defaults(AVER_UI_SB_FLAT, &sb);
        check(sb.kind == AVER_UI_SB_FLAT && (sb.flags & kSbDrawCenter) && (sb.flags & kSbAntiAlias) && sb.cornerDetail == 8 && sb.bgColor == 0xFF999999 && sb.borderColorL == 0xFFCCCCCC,
              "flat: the defaults (draw_center and anti_aliasing on, detail 8, background grey 0.6, border grey 0.8)");
        aver_ui_stylebox_defaults(AVER_UI_SB_LINE, &sb);
        check(sb.kind == AVER_UI_SB_LINE && sb.thickness == 1 && sb.growBegin == 1, "line: thickness 1, grow 1");
        aver_ui_stylebox_defaults(99, &sb);
        check(sb.kind == AVER_UI_SB_FLAT, "an unknown kind is a flat box");
        aver_ui_stylebox_defaults(AVER_UI_SB_FLAT, nullptr);
    }

    AVER_INFO("=== mesh and draw ===");
    {
        UiStyleBox sb;
        aver_ui_stylebox_defaults(AVER_UI_SB_FLAT, &sb);
        sb.bgColor = 0xFF0000FF;
        sb.radiusTL = sb.radiusTR = sb.radiusBR = sb.radiusBL = 6;
        sb.borderL = sb.borderT = sb.borderR = sb.borderB = 2;
        std::vector<float> xyuv(4 * 512);
        std::vector<uint32_t> rgba(512), idx(2048);
        int32_t counts[4] = {};
        const int ok = aver_ui_stylebox_mesh(&sb, 5, 6, 100, 40, xyuv.data(), rgba.data(), 512, idx.data(), 2048, counts);
        UiMesh ref;
        uiStyleBoxMesh(sb, {5, 6, 100, 40}, ref);
        check(ok == 1 && counts[0] == static_cast<int>(ref.verts.size()) && counts[1] == static_cast<int>(ref.idx.size()), "the ABI mesh is the library's mesh");
        bool same = true;
        for (usize i = 0; i < ref.verts.size(); ++i)
            if (xyuv[i * 4] != ref.verts[i].x || xyuv[i * 4 + 1] != ref.verts[i].y || rgba[i] != ref.verts[i].rgba) same = false;
        for (usize i = 0; i < ref.idx.size(); ++i) if (idx[i] != ref.idx[i]) same = false;
        check(same, "vertex for vertex");
        int32_t counts2[4] = {};
        check(aver_ui_stylebox_mesh(&sb, 5, 6, 100, 40, xyuv.data(), rgba.data(), 4, idx.data(), 4, counts2) == 0 && counts2[0] == counts[0], "a buffer that is too small answers 0 and still reports the sizes");
        check(aver_ui_stylebox_mesh(nullptr, 0, 0, 1, 1, nullptr, nullptr, 0, nullptr, 0, counts2) == 0 && counts2[0] == 0, "a null box answers 0");
        aver_ui_begin_frame(0, 0, 200, 100);
        aver_ui_stylebox_draw(&sb, 5, 6, 100, 40);
        check(aver_ui_vertex_count() == static_cast<int>(ref.verts.size()) && aver_ui_command_count() == 1, "draw adds the same triangles to the HUD draw list");
        aver_ui_stylebox_draw(nullptr, 0, 0, 10, 10);
        check(aver_ui_command_count() == 1, "a null box draws nothing");
    }

    AVER_INFO("=== metrics and 9-patch ===");
    {
        UiStyleBox sb;
        aver_ui_stylebox_defaults(AVER_UI_SB_TEXTURE, &sb);
        sb.texW = 30; sb.texH = 30;
        sb.patchL = sb.patchT = sb.patchR = sb.patchB = 10;
        float m[6];
        aver_ui_stylebox_metrics(&sb, m);
        check(m[0] == 20 && m[1] == 20 && m[2] == 10 && m[5] == 10, "minimum size and margins");
        std::vector<float> cells(8 * 12);
        const int n = aver_ui_ninepatch_slices(&sb, 0, 0, 80, 60, cells.data(), 12);
        check(n == 9 && near(cells[0 * 8 + 2], 10) && near(cells[4 * 8 + 2], 60), "nine cells, the corner 10 wide and the middle 60");
        check(aver_ui_ninepatch_slices(&sb, 0, 0, 80, 60, nullptr, 0) == 9, "the count without a buffer");
    }

    AVER_INFO("=== stretch ===");
    {
        const float in[6] = {AVER_UI_STRETCH_CANVAS_ITEMS, AVER_UI_ASPECT_KEEP, AVER_UI_SCALEMODE_FRACTIONAL, 1280, 720, 1};
        float out[11];
        aver_ui_stretch_compute(in, 2560, 1080, out);
        check(near(out[0], 320) && near(out[2], 1920) && near(out[4], 1280) && near(out[8], 1.5f) && out[10] == 1.0f, "letterboxed 16:9 content in a wide window");
        aver_ui_stretch_compute(nullptr, 100, 100, out);
        check(out[2] == 0, "no input: zeros");
    }

    AVER_INFO("=== the default theme ===");
    {
        const int len = aver_ui_theme_default_text(nullptr, 0);
        std::vector<char> buf(static_cast<usize>(len) + 1);
        check(len > 100 && aver_ui_theme_default_text(buf.data(), len + 1) == len && std::strncmp(buf.data(), "OCTHEME 1", 9) == 0, "the text, with its length");
        char small[8];
        aver_ui_theme_default_text(small, 8);
        check(std::strlen(small) == 7, "a short buffer is cut and terminated");
        char err[64];
        check(aver_ui_theme_validate(buf.data(), err, 64) == 1, "the default text validates");
        check(aver_ui_theme_validate("nope", err, 64) == 0 && std::strstr(err, "line 1") != nullptr, "a bad document reports its line");
    }

    if (g_failures == 0) AVER_INFO("UiStyleAbiTest: all checks passed");
    else AVER_ERROR("UiStyleAbiTest: {} check(s) failed", g_failures);
    return g_failures == 0 ? 0 : 1;
}
