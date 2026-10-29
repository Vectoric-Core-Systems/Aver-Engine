// Headless test for the style box tessellator (G5 U1): corner tessellation, border quads, shadow, anti-aliasing fringe, skew,
// the 9-patch slice rectangles, line boxes, content margins, and UiDrawList::addMesh. All arithmetic on the CPU.
#include "aver/core/Log.hpp"
#include "aver/ui/UiDrawList.hpp"
#include "aver/ui/UiStyleBox.hpp"

#include <algorithm>
#include <cmath>
#include <string>

using namespace aver;
using namespace aver::ui;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }

static f32 triArea(const UiVertex& a, const UiVertex& b, const UiVertex& c) {
    return 0.5f * std::fabs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y));
}

// Sum of the triangle areas whose first vertex has `pred(colour)`; with no predicate, all of them.
template <class Pred>
static f32 areaWhere(const UiMesh& m, Pred pred) {
    f32 s = 0;
    for (usize i = 0; i + 2 < m.idx.size(); i += 3) {
        const UiVertex& a = m.verts[m.idx[i]];
        if (pred(a.rgba)) s += triArea(a, m.verts[m.idx[i + 1]], m.verts[m.idx[i + 2]]);
    }
    return s;
}
static f32 area(const UiMesh& m) { return areaWhere(m, [](u32) { return true; }); }

struct Bounds { f32 x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f; };
static Bounds boundsOf(const UiMesh& m) {
    Bounds b;
    for (const UiVertex& v : m.verts) { b.x0 = std::min(b.x0, v.x); b.y0 = std::min(b.y0, v.y); b.x1 = std::max(b.x1, v.x); b.y1 = std::max(b.y1, v.y); }
    return b;
}

// The colour at (x, y): the last triangle that contains it, interpolated (the one drawn on top).
static bool colourAt(const UiMesh& m, f32 x, f32 y, u32& out) {
    bool found = false;
    for (usize i = 0; i + 2 < m.idx.size(); i += 3) {
        const UiVertex &a = m.verts[m.idx[i]], &b = m.verts[m.idx[i + 1]], &c = m.verts[m.idx[i + 2]];
        const f32 d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
        if (std::fabs(d) < 1e-9f) continue;
        const f32 l1 = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / d;
        const f32 l2 = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / d;
        const f32 l3 = 1.0f - l1 - l2;
        if (l1 < -1e-4f || l2 < -1e-4f || l3 < -1e-4f) continue;
        u32 col = 0;
        for (int s = 0; s <= 24; s += 8) {
            const f32 v = l1 * static_cast<f32>((a.rgba >> s) & 0xFF) + l2 * static_cast<f32>((b.rgba >> s) & 0xFF) + l3 * static_cast<f32>((c.rgba >> s) & 0xFF);
            col |= static_cast<u32>(std::lround(std::max(0.0f, std::min(255.0f, v)))) << s;
        }
        out = col;
        found = true;
    }
    return found;
}

static bool sane(const UiMesh& m) {
    for (u32 i : m.idx) if (i >= m.verts.size()) return false;
    for (const UiVertex& v : m.verts) if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.u) || !std::isfinite(v.v)) return false;
    return m.idx.size() % 3 == 0;
}

static UiStyleBox flat(u32 bg) {
    UiStyleBox sb;
    sb.kind = static_cast<i32>(UiStyleBoxKind::Flat);
    sb.bgColor = bg;
    sb.flags = kSbDrawCenter;   // no anti-aliasing unless a test asks
    return sb;
}

static UiMesh mesh(const UiStyleBox& sb, UiRect r) {
    UiMesh m;
    uiStyleBoxMesh(sb, r, m);
    return m;
}

int main() {
    const u32 RED = 0xFF0000FF, GREEN = 0xFF00FF00, BLUE = 0xFFFF0000, WHITE = 0xFFFFFFFF, BLACK = 0xFF000000;

    AVER_INFO("=== flat: a plain rectangle ===");
    {
        const UiMesh m = mesh(flat(RED), {10, 20, 100, 50});
        const Bounds b = boundsOf(m);
        check(sane(m), "indices in range, numbers finite");
        check(near(b.x0, 10) && near(b.y0, 20) && near(b.x1, 110) && near(b.y1, 70), "covers exactly the rectangle");
        check(near(area(m), 5000), "area is w * h");
        check(m.verts.size() == 5, "four corners and the fan centre");
        check(m.texture == 0, "no texture");
    }

    AVER_INFO("=== flat: corner tessellation ===");
    {
        UiStyleBox sb = flat(RED);
        sb.radiusTL = sb.radiusTR = sb.radiusBR = sb.radiusBL = 10;
        sb.cornerDetail = 8;
        UiMesh m = mesh(sb, {0, 0, 100, 50});
        check(sane(m), "sane");
        const f32 quarter = 8.0f / 2.0f * 100.0f * std::sin(3.14159265f / 16.0f);
        const f32 expect = 100.0f * 50.0f - 4.0f * (100.0f - quarter);
        check(near(area(m), expect, 0.05f), "area is the rectangle minus four polygonal corner cuts");
        check(m.verts.size() == 1 + 4 * 9, "centre + 9 points per rounded corner");
        const Bounds b = boundsOf(m);
        check(near(b.x0, 0) && near(b.x1, 100) && near(b.y0, 0) && near(b.y1, 50), "still spans the rectangle");
        u32 c;
        check(!colourAt(m, 0.2f, 0.2f, c), "the very corner is cut away");
        check(colourAt(m, 50, 25, c) && c == RED, "the middle is filled");

        UiStyleBox fine = sb;
        fine.cornerDetail = 24;
        check(mesh(fine, {0, 0, 100, 50}).verts.size() == 1 + 4 * 25, "corner_detail 24 gives 25 points per corner");
        check(area(mesh(fine, {0, 0, 100, 50})) > area(m), "more segments fill more of the circle");

        UiStyleBox one = sb;
        one.radiusTL = 0;
        check(mesh(one, {0, 0, 100, 50}).verts.size() == 1 + 3 * 9 + 1, "a square corner is one point");

        UiStyleBox huge = flat(RED);
        huge.radiusTL = huge.radiusTR = huge.radiusBR = huge.radiusBL = 100;
        const UiMesh hm = mesh(huge, {0, 0, 40, 40});
        const Bounds hb = boundsOf(hm);
        check(sane(hm) && near(hb.x0, 0) && near(hb.x1, 40), "radii larger than the box shrink to fit (a pill / circle)");
        check(colourAt(hm, 20, 20, c) && !colourAt(hm, 1, 1, c), "and the box is a circle");
    }

    AVER_INFO("=== flat: borders ===");
    {
        UiStyleBox sb = flat(BLUE);
        sb.borderL = sb.borderT = sb.borderR = sb.borderB = 4;
        sb.borderColorL = sb.borderColorT = sb.borderColorR = sb.borderColorB = RED;
        const UiMesh m = mesh(sb, {0, 0, 100, 60});
        check(sane(m), "sane");
        check(near(areaWhere(m, [&](u32 c) { return c == RED; }), 100 * 60 - 92 * 52, 0.01f), "the border ring is w*h minus the inside");
        check(near(areaWhere(m, [&](u32 c) { return c == BLUE; }), 92 * 52, 0.01f), "the fill is the inside");
        u32 c;
        check(colourAt(m, 2, 30, c) && c == RED, "a point in the left border is the border colour");
        check(colourAt(m, 50, 30, c) && c == BLUE, "a point inside is the fill colour");

        UiStyleBox sides = sb;
        sides.borderL = 2; sides.borderT = 6; sides.borderR = 10; sides.borderB = 0;
        sides.borderColorL = RED; sides.borderColorT = GREEN; sides.borderColorR = WHITE; sides.borderColorB = BLACK;
        const UiMesh sm = mesh(sides, {0, 0, 100, 60});
        check(near(areaWhere(sm, [&](u32 c2) { return c2 == RED; }), (60 + 54) / 2.0f * 2, 0.01f), "left border: a 2 wide mitred strip in its own colour");
        check(near(areaWhere(sm, [&](u32 c2) { return c2 == GREEN; }), (100 + 88) / 2.0f * 6, 0.01f), "top border: a 6 high mitred strip in its own colour");
        check(near(areaWhere(sm, [&](u32 c2) { return c2 == BLACK; }), 0, 0.01f), "no bottom border, no bottom area");
        check(colourAt(sm, 95, 30, c) && c == WHITE, "right border colour on the right edge");

        UiStyleBox ring = sb;
        ring.flags &= ~kSbDrawCenter;
        const UiMesh rm = mesh(ring, {0, 0, 100, 60});
        check(!colourAt(rm, 50, 30, c), "draw_center off leaves the middle empty");
        check(colourAt(rm, 2, 30, c), "and keeps the border");

        UiStyleBox clear = flat(0x00000000);
        clear.borderL = clear.borderT = clear.borderR = clear.borderB = 3;
        check(mesh(clear, {0, 0, 50, 50}).idx.size() > 0, "a transparent fill with an opaque border still draws the border");
        UiStyleBox none = flat(0x00000000);
        check(mesh(none, {0, 0, 50, 50}).idx.empty(), "a transparent fill and no border draws nothing");
    }

    AVER_INFO("=== flat: rounded borders ===");
    {
        UiStyleBox sb = flat(BLUE);
        sb.radiusTL = sb.radiusTR = sb.radiusBR = sb.radiusBL = 12;
        sb.borderL = sb.borderT = sb.borderR = sb.borderB = 3;
        sb.borderColorL = RED; sb.borderColorT = GREEN; sb.borderColorR = RED; sb.borderColorB = GREEN;
        const UiMesh m = mesh(sb, {0, 0, 80, 40});
        check(sane(m), "sane");
        const UiStyleBox plain = [&] { UiStyleBox p = sb; p.borderL = p.borderT = p.borderR = p.borderB = 0; return p; }();
        check(near(area(m), area(mesh(plain, {0, 0, 80, 40})), 0.05f), "border + fill together cover the same shape as the box without a border");
        u32 c;
        check(colourAt(m, 40, 1.5f, c) && c == GREEN, "top edge is the top colour");
        check(colourAt(m, 1.5f, 20, c) && c == RED, "left edge is the left colour");
        // Split at the corner's middle: just past the start of the arc, the left side's colour; just before the end, the top's.
        check(colourAt(m, 0.4f, 11, c) && c == RED, "near the start of the top-left arc: the left colour");
        check(colourAt(m, 11, 0.4f, c) && c == GREEN, "near the end of the top-left arc: the top colour");
        UiStyleBox blend = sb;
        blend.flags |= kSbBorderBlend;
        const UiMesh bm = mesh(blend, {0, 0, 80, 40});
        // The arc's middle (45 degrees) blends red and green evenly.
        const f32 mx = 12 - 12 * std::cos(3.14159265f / 4), my = mx;
        check(colourAt(bm, mx + 0.5f, my + 0.5f, c) && ((c & 0xFF) > 60 && (c & 0xFF) < 200) && (((c >> 8) & 0xFF) > 60 && ((c >> 8) & 0xFF) < 200), "border_blend mixes the two sides across the corner");
    }

    AVER_INFO("=== flat: shadow, anti-aliasing, skew, expand ===");
    {
        UiStyleBox sb = flat(WHITE);
        sb.shadowColor = 0xFF000000;
        sb.shadowSize = 8;
        sb.shadowOffX = 4; sb.shadowOffY = 6;
        const UiMesh m = mesh(sb, {0, 0, 100, 50});
        const Bounds b = boundsOf(m);
        check(sane(m), "sane");
        check(near(b.x0, -4) && near(b.x1, 112) && near(b.y0, -2) && near(b.y1, 64), "the shadow extends by offset + size");
        u32 c;
        check(colourAt(m, 50, 25, c) && c == WHITE, "the box is drawn over its shadow");
        check(colourAt(m, 108, 30, c) && (c >> 24) > 0 && (c >> 24) < 255, "the feather fades out");
        bool zero = false;
        for (const UiVertex& v : m.verts) if ((v.rgba >> 24) == 0) zero = true;
        check(zero, "the feather's outer edge is transparent");
        UiStyleBox noShadow = sb;
        noShadow.shadowSize = 0;
        check(mesh(noShadow, {0, 0, 100, 50}).verts.size() == 5, "shadow size 0 draws no shadow");

        UiStyleBox aa = flat(WHITE);
        aa.flags |= kSbAntiAlias;
        aa.aaSize = 2;
        aa.radiusTL = aa.radiusTR = aa.radiusBR = aa.radiusBL = 8;
        const UiMesh am = mesh(aa, {0, 0, 100, 50});
        const Bounds ab = boundsOf(am);
        check(sane(am), "sane");
        check(near(ab.x0, -2, 0.01f) && near(ab.x1, 102, 0.01f) && near(ab.y0, -2, 0.01f) && near(ab.y1, 52, 0.01f), "the fringe sits aa_size outside the shape");
        int fringe = 0;
        for (const UiVertex& v : am.verts) if ((v.rgba >> 24) == 0) ++fringe;
        check(fringe == 2 * 4 * 9, "two transparent fringe vertices per outline segment");
        UiStyleBox aaOff = aa;
        aaOff.flags &= ~kSbAntiAlias;
        check(boundsOf(mesh(aaOff, {0, 0, 100, 50})).x1 <= 100.001f, "anti_aliasing off: no fringe");

        UiStyleBox sk = flat(WHITE);
        sk.skewX = 10;
        const UiMesh skm = mesh(sk, {0, 0, 100, 50});
        f32 topMin = 1e9f, botMin = 1e9f;
        for (const UiVertex& v : skm.verts) { if (near(v.y, 0)) topMin = std::min(topMin, v.x); if (near(v.y, 50)) botMin = std::min(botMin, v.x); }
        check(near(topMin, 5) && near(botMin, -5), "skew.x leans the top right and the bottom left by half the skew each");
        check(near(area(skm), 5000, 0.01f), "a skew keeps the area");

        UiStyleBox ex = flat(WHITE);
        ex.expandL = 3; ex.expandT = 4; ex.expandR = 5; ex.expandB = 6;
        const Bounds eb = boundsOf(mesh(ex, {10, 10, 100, 50}));
        check(near(eb.x0, 7) && near(eb.y0, 6) && near(eb.x1, 115) && near(eb.y1, 66), "expand margins grow the drawn rectangle");
        check(near(uiStyleBoxDrawRect(ex, {10, 10, 100, 50}).w, 108), "uiStyleBoxDrawRect agrees");
    }

    AVER_INFO("=== content margins and minimum size ===");
    {
        UiStyleBox sb = flat(WHITE);
        sb.borderL = 2; sb.borderT = 3; sb.borderR = 4; sb.borderB = 5;
        UiInsets m = uiStyleBoxMargins(sb);
        check(m.left == 2 && m.top == 3 && m.right == 4 && m.bottom == 5, "the default content margin of a flat box is its border");
        sb.contentL = 8; sb.contentR = 9;
        m = uiStyleBoxMargins(sb);
        check(m.left == 8 && m.top == 3 && m.right == 9 && m.bottom == 5, "an explicit margin wins, per side");
        const UiVec2 mn = uiStyleBoxMinimumSize(sb);
        check(mn.x == 17 && mn.y == 8, "minimum size is the margins summed");
        const UiRect cr = uiStyleBoxContentRect(sb, {0, 0, 100, 50});
        check(cr.x == 8 && cr.y == 3 && cr.w == 83 && cr.h == 42, "the content rectangle is the box inside the margins");
        UiStyleBox empty;
        empty.kind = static_cast<i32>(UiStyleBoxKind::Empty);
        empty.contentL = 6; empty.contentR = 6;
        check(uiStyleBoxMinimumSize(empty).x == 12 && mesh(empty, {0, 0, 10, 10}).idx.empty(), "an empty box has margins and draws nothing");
        UiStyleBox tex;
        tex.kind = static_cast<i32>(UiStyleBoxKind::Texture);
        tex.patchL = 5; tex.patchR = 7; tex.patchT = 1; tex.patchB = 2;
        check(uiStyleBoxMinimumSize(tex).x == 12 && uiStyleBoxMinimumSize(tex).y == 3, "a texture box's default margin is its texture margin");
    }

    AVER_INFO("=== 9-patch slices ===");
    {
        UiStyleBox sb;
        sb.kind = static_cast<i32>(UiStyleBoxKind::Texture);
        sb.texture = 77;
        sb.texW = 30; sb.texH = 30;
        sb.patchL = sb.patchT = sb.patchR = sb.patchB = 10;
        sb.flags = kSbDrawCenter;
        auto cells = uiNinePatchSlices(sb, {100, 200, 80, 60});
        check(cells.size() == 9, "nine cells");
        check(near(cells[0].dst.x, 100) && near(cells[0].dst.w, 10) && near(cells[0].dst.h, 10) && near(cells[0].u1, 1.0f / 3) && near(cells[0].v1, 1.0f / 3), "top-left: a 10 x 10 corner showing the first third");
        check(near(cells[1].dst.x, 110) && near(cells[1].dst.w, 60) && near(cells[1].u0, 1.0f / 3) && near(cells[1].u1, 2.0f / 3), "top-middle stretches over the width");
        check(near(cells[4].dst.x, 110) && near(cells[4].dst.y, 210) && near(cells[4].dst.w, 60) && near(cells[4].dst.h, 40), "the centre takes the rest");
        check(near(cells[8].dst.x, 170) && near(cells[8].dst.y, 250) && near(cells[8].u1, 1.0f) && near(cells[8].v1, 1.0f), "bottom-right ends at the far corner");
        f32 total = 0;
        for (const auto& c : cells) total += c.dst.w * c.dst.h;
        check(near(total, 80 * 60), "the cells tile the rectangle exactly");

        UiStyleBox hollow = sb;
        hollow.flags = 0;
        check(uiNinePatchSlices(hollow, {0, 0, 80, 60}).size() == 8, "draw_center off drops the middle cell");

        const auto small = uiNinePatchSlices(sb, {0, 0, 10, 60});
        check(!small.empty() && near(small[0].dst.w, 5), "a destination narrower than both margins shrinks them together");

        UiStyleBox tile = sb;
        tile.axisH = static_cast<i32>(UiAxisStretch::Tile);
        const auto tiled = uiNinePatchSlices(tile, {0, 0, 85, 30});
        int topMiddle = 0;
        f32 lastW = 0, lastU1 = 0;
        for (const auto& c : tiled) if (near(c.dst.y, 0) && c.dst.x >= 10 - 1e-3f && c.dst.x < 75 - 1e-3f) { ++topMiddle; lastW = c.dst.w; lastU1 = c.u1; }
        check(topMiddle == 7, "tile: the 65 pixel middle holds six whole 10 pixel tiles and one cut one");
        check(near(lastW, 5) && near(lastU1, 1.0f / 3 + 0.5f / 3), "the last tile is cut to the remaining 5 pixels (half the source)");

        UiStyleBox fit = sb;
        fit.axisH = static_cast<i32>(UiAxisStretch::TileFit);
        const auto fitted = uiNinePatchSlices(fit, {0, 0, 85, 30});
        int fitCount = 0;
        for (const auto& c : fitted) if (near(c.dst.y, 0) && c.dst.x >= 10 - 1e-3f && c.dst.x < 75 - 1e-3f) { ++fitCount; check(near(c.dst.w, 65.0f / 7.0f, 0.01f), "tile_fit: equal tiles"); }
        check(fitCount == 7, "tile_fit: the whole number of tiles that comes closest (65 / 10 -> 7)");

        UiStyleBox region = sb;
        region.texW = 100; region.texH = 100;
        region.regionX = 20; region.regionY = 40; region.regionW = 30; region.regionH = 30;
        const auto rc = uiNinePatchSlices(region, {0, 0, 60, 60});
        check(near(rc[0].u0, 0.20f) && near(rc[0].u1, 0.30f) && near(rc[0].v0, 0.40f) && near(rc[0].v1, 0.50f), "a region offsets and scales the uv");
        check(near(rc[8].u1, 0.50f) && near(rc[8].v1, 0.70f), "and ends at the region's far corner");

        UiMesh m;
        uiStyleBoxMesh(sb, {0, 0, 80, 60}, m);
        check(m.texture == 77 && m.verts.size() == 36 && m.idx.size() == 54, "the mesh is nine textured quads on the box's texture");
        check(m.verts[0].rgba == 0xFFFFFFFF, "modulate defaults to white");
        sb.modulate = 0x80FFFFFF;
        UiMesh m2;
        uiStyleBoxMesh(sb, {0, 0, 80, 60}, m2);
        check(m2.verts[0].rgba == 0x80FFFFFF, "modulate colours the quads");
        sb.texture = 0;
        UiMesh m3;
        uiStyleBoxMesh(sb, {0, 0, 80, 60}, m3);
        check(m3.idx.empty(), "no texture handle: nothing is drawn");
    }

    AVER_INFO("=== line ===");
    {
        UiStyleBox ln;
        ln.kind = static_cast<i32>(UiStyleBoxKind::Line);
        ln.flags = 0;
        ln.lineColor = RED;
        ln.thickness = 4;
        ln.growBegin = 2; ln.growEnd = 3;
        UiMesh m = mesh(ln, {10, 10, 100, 30});
        Bounds b = boundsOf(m);
        check(near(b.x0, 8) && near(b.x1, 113) && near(b.y0, 23) && near(b.y1, 27), "a horizontal line is centred in the height and grows at both ends");
        ln.flags = kSbVertical;
        m = mesh(ln, {10, 10, 100, 30});
        b = boundsOf(m);
        check(near(b.x0, 58) && near(b.x1, 62) && near(b.y0, 8) && near(b.y1, 43), "a vertical line is centred in the width");
        ln.thickness = 0;
        check(mesh(ln, {0, 0, 10, 10}).idx.empty(), "zero thickness draws nothing");
    }

    AVER_INFO("=== UiDrawList::addMesh ===");
    {
        UiDrawList dl;
        UiStyleBox sb = flat(0x80FF8040);
        UiStyleBox sb2 = sb;
        uiStyleBoxDraw(dl, sb, {0, 0, 10, 10});
        uiStyleBoxDraw(dl, sb2, {20, 0, 10, 10});
        check(dl.totalCommands() == 1, "two untextured boxes merge into one command");
        check(dl.vertices().size() == 10 && dl.indices().size() == 24, "both are in the list");
        check(dl.vertices()[0].rgba == uiPremultiply(0x80FF8040), "colours are premultiplied on the way in");
        check(dl.indices()[12] >= 5, "the second mesh's indices are offset");
        dl.pushClip({0, 0, 0, 0});
        const usize before = dl.vertices().size();
        uiStyleBoxDraw(dl, sb, {0, 0, 10, 10});
        check(dl.vertices().size() == before, "an empty clip draws nothing");
        dl.popClip();
        const UiVertex v[3] = {{0, 0, 0, 0, WHITE}, {1, 0, 0, 0, WHITE}, {0, 1, 0, 0, WHITE}};
        const u32 bad[3] = {0, 1, 7};
        const usize n = dl.vertices().size();
        dl.addMesh(v, 3, bad, 3, 0);
        check(dl.vertices().size() == n, "an index past the vertices is rejected whole");
        const u32 good[3] = {0, 1, 2};
        dl.addMesh(v, 3, good, 3, 9);
        check(dl.vertices().size() == n + 3 && dl.totalCommands() == 2, "a different texture starts a new command");
    }

    if (g_failures == 0) AVER_INFO("UiStyleBoxTest: all checks passed");
    else AVER_ERROR("UiStyleBoxTest: {} check(s) failed", g_failures);
    return g_failures == 0 ? 0 : 1;
}
