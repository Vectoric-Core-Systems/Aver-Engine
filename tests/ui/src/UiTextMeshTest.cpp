// Parity E6 / E14 on the CPU: the text mesh a Label3D draws (layout, alignment, scale, outline, caps), the world atlas page
// lifetime (a mesh whose page was recycled is stale and is rebuilt), signed-distance glyphs, and the draw list's world text
// validation. Fonts are synthetic rectangles (TestFont.hpp) so every number is arithmetic. No GPU, no window.
#include "TestFont.hpp"

#include "aver/core/Log.hpp"
#include "aver/ui/UiDrawList.hpp"
#include "aver/ui/UiSdf.hpp"
#include "aver/ui/UiTextEngine.hpp"
#include "aver/ui/UiTextMesh.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
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

static bool approx(f32 a, f32 b, f32 eps = 0.01f) { return std::fabs(a - b) <= eps; }

namespace {

struct Env {
    UiTextEngine eng;
    std::map<std::string, std::vector<u8>> files;
    UiFontLoader loader;

    Env() {
        testfont::Spec s = testfont::simpleSpec();
        s.chars['V'] = {620, 40, 0, 580, 700};
        files["latin.ttf"] = testfont::buildFont(s);
        loader.open = [this](UiFontKind, const std::string& name, UiFontBlob& out) {
            const auto it = files.find(name);
            if (it == files.end()) return false;
            auto keep = std::make_shared<std::vector<u8>>(it->second);
            out.data = keep->data();
            out.size = keep->size();
            out.owner = keep;
            return true;
        };
        std::string err;
        if (!eng.loadFontSet("OCFONTS 1\nDEFAULT Body\nFAMILY Body\nFACE latin.ttf\n", loader, &err)) AVER_ERROR("font set: {}", err);
    }
};

UiGlyphBitmap solid(u16 w, u16 h, u8 v) {
    UiGlyphBitmap b;
    b.w = w; b.h = h; b.offX = -1; b.offY = -h;
    b.pixels.assign(static_cast<usize>(w) * h, v);
    return b;
}

} // namespace

int main() {
    AVER_INFO("=== the mesh: size, scale, alignment ===");
    {
        Env env;
        UiTextMeshStyle st;
        st.px = 20.0f;
        st.pixelSize = 0.01f;
        st.hAlign = UiTextHAlign::Center;
        st.vAlign = UiTextVAlign::Center;
        UiTextMesh m;
        check(uiBuildTextMesh(env.eng, "AB", st, m), "a mesh builds");
        check(m.verts.size() == 8 && m.indices.size() == 12, "two glyphs are two quads");
        check(approx(m.width, 0.242f, 0.002f) && approx(m.height, 0.2f, 0.002f), "the box is the layout's 24.2 x 20 px at 0.01 per px");
        check(m.lines == 1 && !m.incomplete && !m.truncated, "one line, complete");
        check(!m.runs.empty() && m.runs[0].indexCount == 12, "one run on one atlas page");
        check(m.verts[0].u >= 0.0f && m.verts[0].u <= 1.0f && m.verts[0].v >= 0.0f && m.verts[0].v <= 1.0f, "UVs are inside the page");
        check(m.verts[0].rgba == 0xFFFFFFFFu, "the colour is premultiplied white");
        // y is up: the glyph tops are higher than their bottoms.
        check(m.verts[0].y > m.verts[2].y, "+y is up (the top vertex is above the bottom one)");
        check(approx((m.minX + m.maxX) * 0.5f, 0.0f, 0.02f), "centred horizontally about the origin");
        check(approx((m.minY + m.maxY) * 0.5f, 0.0f, 0.06f), "centred vertically about the origin (within the glyph shape)");

        UiTextMeshStyle left = st; left.hAlign = UiTextHAlign::Left; left.vAlign = UiTextVAlign::Top;
        UiTextMesh ml;
        uiBuildTextMesh(env.eng, "AB", left, ml);
        check(ml.minX >= -0.02f && ml.maxX <= 0.27f, "left aligned: the text starts at the origin and runs right");
        check(ml.maxY <= 0.03f && ml.minY >= -0.21f, "top aligned: the text hangs below the origin");

        UiTextMeshStyle right = st; right.hAlign = UiTextHAlign::Right; right.vAlign = UiTextVAlign::Bottom;
        UiTextMesh mr;
        uiBuildTextMesh(env.eng, "AB", right, mr);
        check(mr.maxX <= 0.03f && mr.minX >= -0.27f, "right aligned: the text ends at the origin");
        check(mr.minY >= -0.03f && mr.maxY <= 0.21f, "bottom aligned: the text stands on the origin");

        UiTextMeshStyle big = st; big.pixelSize = 0.02f;
        UiTextMesh mb;
        uiBuildTextMesh(env.eng, "AB", big, mb);
        check(approx(mb.width, m.width * 2.0f, 0.002f), "doubling the pixel size doubles the plane size");

        UiTextMeshStyle off = left; off.offsetX = 10.0f; off.offsetY = 5.0f;
        UiTextMesh mo;
        uiBuildTextMesh(env.eng, "AB", off, mo);
        check(approx(mo.minX, ml.minX + 0.10f, 0.002f) && approx(mo.minY, ml.minY + 0.05f, 0.002f), "the offset moves the quads (10 px right, 5 px up)");

        UiTextMeshStyle wrap = left; wrap.width = 90.0f; wrap.wrap = true;
        UiTextMesh mw;
        uiBuildTextMesh(env.eng, "AAA BBB CCC", wrap, mw);
        check(mw.lines == 2, "a wrap width of 90 px breaks the line in two (\"AAA BBB\" is 78.6)");
        check(approx(mw.height, 0.4f, 0.002f) && approx(mw.width, 0.9f, 0.002f), "the box is the wrap width wide and two lines tall");
        UiTextMeshStyle spaced = left; spaced.letterSpacing = 5.0f;
        UiTextMesh msp;
        uiBuildTextMesh(env.eng, "AB", spaced, msp);
        check(approx(msp.width, m.width + 0.10f, 0.002f), "letter spacing adds 5 px after each glyph (P38 reaches the mesh)");

        UiTextMesh none;
        check(!uiBuildTextMesh(env.eng, "", st, none) && none.empty(), "empty text builds nothing");
        UiTextMeshStyle nan = st; nan.pixelSize = std::numeric_limits<f32>::quiet_NaN();
        check(!uiBuildTextMesh(env.eng, "AB", nan, none), "a NaN size is refused");
        UiTextMesh blank;
        check(uiBuildTextMesh(env.eng, "   ", st, blank) && blank.empty(), "spaces build an empty mesh, not a failure");
    }

    AVER_INFO("=== outline and caps ===");
    {
        Env env;
        UiTextMeshStyle st;
        st.px = 20.0f;
        st.rgba = 0xFF0000FFu;          // red
        st.outlineRgba = 0xFFFF0000u;   // blue
        UiTextMesh plain, outlined;
        uiBuildTextMesh(env.eng, "AB", st, plain);
        st.outlineSize = 2.0f;
        uiBuildTextMesh(env.eng, "AB", st, outlined);
        check(outlined.verts.size() > plain.verts.size() * 4, "an outline is many copies of the glyphs");
        check(outlined.verts.front().rgba == uiPremultiply(0xFFFF0000u) && outlined.verts.back().rgba == uiPremultiply(0xFF0000FFu),
              "the outline copies come first and the fill (the caller's colour) is last");
        check(approx(outlined.width, plain.width, 0.0001f), "the outline is not counted in the box");
        check(outlined.maxX >= plain.maxX - 0.0001f && outlined.minX == plain.minX, "bounds are the fill's");
        check(outlined.runs.back().indexOffset + outlined.runs.back().indexCount == outlined.indices.size(), "runs cover every index");

        std::string longText(900, 'A');
        UiTextMeshStyle many = st;
        many.outlineSize = 4.0f;
        many.px = 8.0f;
        UiTextMesh capped;
        uiBuildTextMesh(env.eng, longText, many, capped);
        check(capped.truncated && capped.verts.size() <= static_cast<usize>(kUiTextMeshMaxQuads) * 4,
              "too many outline copies drops the outline, never the text, and stays within the quad cap");
        check(capped.verts.back().rgba == uiPremultiply(0xFF0000FFu), "and what is left is the fill");
        std::string huge(5000, 'B');
        UiTextMesh hm;
        uiBuildTextMesh(env.eng, huge, st, hm);
        check(hm.verts.size() <= static_cast<usize>(kUiTextMeshMaxQuads) * 4 && hm.verts.size() / 4 <= kUiTextMeshMaxChars, "text beyond the read cap is cut");
    }

    AVER_INFO("=== atlas page lifetime: a mesh outlives its page ===");
    {
        Env env;
        UiTextConfig cfg = env.eng.config();
        cfg.worldPageSize = 256;
        cfg.worldMaxPages = 1;
        env.eng.configure(cfg);
        UiGlyphAtlas& atlas = env.eng.worldAtlas();
        check(atlas.pageSize() == 256 && atlas.config().maxPages == 1, "a one-page world atlas");

        UiTextMeshStyle st;
        st.px = 20.0f;
        UiTextMeshCache cache;
        const UiTextMesh* first = cache.get(env.eng, "AB", st);
        check(first && !first->runs.empty() && !first->stale(atlas), "a mesh on page 0 is current");
        const UiTextMesh held = *first;   // a caller that kept the mesh
        const u32 gen0 = atlas.pageGeneration(0);
        check(held.runs[0].page == 0 && held.runs[0].generation == gen0, "it remembers the page and its generation");

        // Three fillers use up the page (16 rows of text, then 3 x 80). A fourth in the same frame cannot recycle the page:
        // it is in use this frame, so the insert overflows instead.
        for (u32 k = 1; k <= 3; ++k)
            check(atlas.insert(UiGlyphKey{9, 9, k}, solid(240, 80, 255), env.eng.frame()) != nullptr, "filler fits");
        const UiAtlasGlyph* big = atlas.insert(UiGlyphKey{9, 9, 4}, solid(240, 80, 255), env.eng.frame());
        check(big == nullptr && atlas.overflow() == 1 && !held.stale(atlas), "a page used this frame is never recycled (the insert overflows instead)");

        env.eng.beginFrame();
        // Not used this frame: the same insert recycles the page whole.
        big = atlas.insert(UiGlyphKey{9, 9, 4}, solid(240, 80, 255), env.eng.frame());
        check(big != nullptr && atlas.evictedPages() == 1, "a later frame recycles the page");
        check(atlas.pageGeneration(0) == gen0 + 1, "the page's generation moved");
        check(held.stale(atlas), "so the mesh built before is STALE: its UVs would show another glyph");
        check(atlas.glyphsResident() == 1, "and the glyphs it named are forgotten");

        const UiTextMesh* second = cache.get(env.eng, "AB", st);
        check(second && !second->stale(atlas), "the cache hands back a rebuilt mesh");
        check(cache.builds() == 2 && cache.rebuildsStale() == 1, "counted as a rebuild after a recycle");
        check(second->runs[0].generation == atlas.pageGeneration(0), "on the page's new generation");
        // Its UVs are where the glyph is NOW.
        const f32 ps = static_cast<f32>(atlas.pageSize());
        bool uvOk = true;
        for (usize q = 0; q < second->verts.size() / 4; ++q) {
            const f32 u0 = second->verts[q * 4].u * ps, v0 = second->verts[q * 4].v * ps;
            const f32 u1 = second->verts[q * 4 + 2].u * ps, v1 = second->verts[q * 4 + 2].v * ps;
            uvOk = uvOk && u1 > u0 && v1 > v0 && u1 <= ps + 0.01f && v1 <= ps + 0.01f;
        }
        check(uvOk, "with UVs inside the page");
        check(cache.get(env.eng, "AB", st) == second && cache.builds() == 2, "a current mesh is reused, not rebuilt");

        // configure() forgets pages altogether: the page index and generation come back from zero, but the epoch tells.
        const UiTextMesh beforeCfg = *cache.get(env.eng, "AB", st);
        const u64 epoch = atlas.epoch();
        UiTextConfig c2 = env.eng.config();
        c2.worldPageSize = 512;
        env.eng.configure(c2);
        check(atlas.epoch() > epoch && beforeCfg.stale(atlas), "reconfiguring the atlas stales every mesh, even where page and generation match");
        env.eng.beginFrame();
        const UiTextMesh* third = cache.get(env.eng, "AB", st);
        check(third && !third->stale(atlas), "and the cache rebuilds on the new atlas");
        const UiTextMesh beforeLoad = *third;
        std::string err;
        env.eng.loadFontSet("OCFONTS 1\nDEFAULT Body\nFAMILY Body\nFACE latin.ttf\n", env.loader, &err);
        check(beforeLoad.stale(env.eng.worldAtlas()), "loading a font set clears the atlas and stales the meshes");

        // touch(): a label that is drawn keeps its page from being recycled by a later insert in the same frame.
        env.eng.beginFrame();
        const UiTextMesh* fourth = cache.get(env.eng, "AB", st);
        check(fourth != nullptr, "rebuilt after the font set load");
        const UiTextMesh* again = cache.get(env.eng, "AB", st);
        (void)again;
        UiGlyphAtlas& wa = env.eng.worldAtlas();
        const auto* r = wa.insert(UiGlyphKey{9, 9, 9}, solid(500, 500, 255), env.eng.frame());
        check(r == nullptr || wa.pageLastUsed(0) >= env.eng.frame(), "a drawn label's page is stamped as used this frame");
    }

    AVER_INFO("=== the cache is bounded ===");
    {
        Env env;
        UiTextMeshCache cache;
        cache.maxEntries = 4;
        UiTextMeshStyle st;
        for (int i = 0; i < 12; ++i) {
            env.eng.beginFrame();
            const std::string t(1, static_cast<char>('A' + i));
            cache.get(env.eng, t, st);
        }
        check(cache.size() <= 4, "at most maxEntries meshes are kept (the least recently used go)");
        env.eng.beginFrame();
        cache.keepFrames = 3;
        for (int i = 0; i < 10; ++i) env.eng.beginFrame();
        cache.trim(env.eng.frame());
        check(cache.size() == 0, "meshes unused for keepFrames frames are dropped");
    }

    AVER_INFO("=== E14: signed-distance glyphs ===");
    {
        // A solid 8 x 8 box with the engine's 1 px gutter: 10 x 10 coverage.
        UiGlyphBitmap box;
        box.w = box.h = 10; box.offX = -1; box.offY = -9;
        box.pixels.assign(100, 0);
        for (int y = 1; y < 9; ++y) for (int x = 1; x < 9; ++x) box.pixels[static_cast<usize>(y) * 10 + x] = 255;
        UiGlyphBitmap sdf;
        check(uiBitmapToSdf(box, 4, sdf), "a bitmap converts");
        check(sdf.w == 18 && sdf.h == 18 && sdf.offX == -5 && sdf.offY == -13, "padded by the spread on each side, the glyph kept where it was");
        const auto at = [&](int x, int y) { return sdf.pixels[static_cast<usize>(y) * sdf.w + static_cast<usize>(x)]; };
        check(at(9, 9) > 200, "the middle of the box is well inside");
        check(at(0, 0) == 0 && at(17, 17) == 0, "the corners of the padding are far outside");
        check(at(5, 9) >= 138 && at(5, 9) <= 150 && at(4, 9) >= 105 && at(4, 9) <= 118,
              "the texel just inside the left edge reads a little over 128 and the one just outside a little under");
        bool monotone = true;
        for (int x = 0; x < 8; ++x) monotone = monotone && at(x, 9) <= at(x + 1, 9);
        check(monotone, "going in from the left edge the field only rises");
        check(at(3, 9) < at(4, 9), "and further out it falls");
        check(uiSdfCoverage(255, 0.05f) == 1.0f && uiSdfCoverage(0, 0.05f) == 0.0f && approx(uiSdfCoverage(128, 0.05f), 0.5f, 0.05f),
              "the shader's smoothstep: 1 well inside, 0 outside, a half on the edge");

        UiGlyphBitmap out;
        UiGlyphBitmap blank; blank.offX = 2; blank.offY = -3;
        check(uiBitmapToSdf(blank, 4, out) && out.w == 0 && out.offX == 2, "a blank glyph stays blank");
        check(!uiBitmapToSdf(box, 0, out) && !uiBitmapToSdf(box, kUiSdfMaxSpread + 1, out), "a spread of 0 or above the cap is refused");
        UiGlyphBitmap shortPixels = box; shortPixels.pixels.resize(10);
        check(!uiBitmapToSdf(shortPixels, 4, out), "a short pixel array is refused");
        UiGlyphBitmap wide; wide.w = 400; wide.h = 4; wide.pixels.assign(1600, 255);
        check(!uiBitmapToSdf(wide, 4, out), "a bitmap over the edge cap is refused");
        UiGlyphBitmap full; full.w = full.h = 6; full.pixels.assign(36, 255);
        check(uiBitmapToSdf(full, 3, out), "an all-covered bitmap converts");
        UiGlyphBitmap empty8; empty8.w = empty8.h = 6; empty8.pixels.assign(36, 0);
        check(uiBitmapToSdf(empty8, 3, out) && out.pixels[out.pixels.size() / 2] == 0, "an all-empty bitmap is all outside");

        // Through the engine: one raster size, any display size.
        Env env;
        UiTextConfig cfg = env.eng.config();
        cfg.worldSdf = true;
        cfg.worldSdfPx = 48;
        cfg.worldSdfSpread = 6;
        env.eng.configure(cfg);
        check(env.eng.worldAtlas().sdf() && env.eng.worldLayoutPx(12.0f) == 48.0f && env.eng.worldLayoutPx(200.0f) == 48.0f,
              "an SDF world atlas lays out at the SDF size whatever is asked for");
        UiTextMeshStyle st;
        st.px = 48.0f;
        UiTextMesh m48, m24;
        uiBuildTextMesh(env.eng, "A", st, m48);
        st.px = 24.0f;
        uiBuildTextMesh(env.eng, "A", st, m24);
        const f32 w48 = m48.verts[2].x - m48.verts[0].x, w24 = m24.verts[2].x - m24.verts[0].x;
        check(w48 > 0 && approx(w24 / w48, 0.5f, 0.01f), "the same glyph, half the size: its quad is half as wide");
        check(approx(m24.width, m48.width * 0.5f, 0.002f), "and so is the layout box");
        check(env.eng.stats().rasterTotal == 1, "rasterised once for both sizes");
        UiTextMesh mrich;
        st.px = 24.0f;
        st.rich = true;
        uiBuildTextMesh(env.eng, "A<size=48>A</size>", st, mrich);
        check(mrich.verts.size() == 8, "rich text builds in an SDF atlas too");
    }

    AVER_INFO("=== the draw list carries labels, with caps and checks ===");
    {
        UiDrawList dl;
        const UiVertex v[4] = {{0, 0, 0, 0, 0xFFFFFFFF}, {1, 0, 1, 0, 0xFFFFFFFF}, {1, 1, 1, 1, 0xFFFFFFFF}, {0, 1, 0, 1, 0xFFFFFFFF}};
        const u32 idx[6] = {0, 1, 2, 0, 2, 3};
        const UiWorldTextRun run{0, 6, 0};
        UiWorldText h;
        check(dl.addWorldText(h, v, 4, idx, 6, &run, 1), "a label is added");
        check(dl.worldTexts().size() == 1 && dl.worldTexts()[0].vertexCount == 4 && dl.worldTexts()[0].runCount == 1, "with its offsets filled in");
        check(dl.addWorldText(h, v, 4, idx, 6, &run, 1) && dl.worldTexts()[1].vertexOffset == 4 && dl.worldTextRuns()[1].indexOffset == 6,
              "a second label rebases its run onto the shared index buffer");
        check(dl.empty(), "labels do not make the UI geometry non-empty");

        const u32 bad[6] = {0, 1, 4, 0, 2, 3};
        check(!dl.addWorldText(h, v, 4, bad, 6, &run, 1), "an index past the vertices is refused");
        const UiWorldTextRun past{3, 6, 0};
        check(!dl.addWorldText(h, v, 4, idx, 6, &past, 1), "a run that leaves the index array is refused");
        const UiWorldTextRun odd{0, 4, 0};
        check(!dl.addWorldText(h, v, 4, idx, 6, &odd, 1), "a run that is not whole triangles is refused");
        UiWorldText nan = h; nan.world[3] = std::numeric_limits<f32>::quiet_NaN();
        check(!dl.addWorldText(nan, v, 4, idx, 6, &run, 1), "a NaN matrix is refused");
        UiWorldText inf = h; inf.intensity = std::numeric_limits<f32>::infinity();
        check(!dl.addWorldText(inf, v, 4, idx, 6, &run, 1), "and an infinite intensity");
        check(!dl.addWorldText(h, nullptr, 4, idx, 6, &run, 1) && !dl.addWorldText(h, v, 0, idx, 6, &run, 1) && !dl.addWorldText(h, v, 4, idx, 0, &run, 1),
              "nulls and zero counts are refused");
        check(dl.worldTexts().size() == 2 && dl.worldVertices().size() == 8, "a refused label leaves the list as it was");

        UiDrawList full;
        u32 added = 0;
        for (u32 i = 0; i < kUiMaxWorldTexts + 10; ++i) if (full.addWorldText(h, v, 4, idx, 6, &run, 1)) ++added;
        check(added == kUiMaxWorldTexts, "the label count is capped");
        std::vector<UiVertex> many(kUiMaxWorldVertices + 1);
        std::vector<u32> mi = {0, 1, 2};
        UiDrawList cap2;
        const UiWorldTextRun r3{0, 3, 0};
        check(!cap2.addWorldText(h, many.data(), many.size(), mi.data(), mi.size(), &r3, 1), "a vertex count over the cap is refused");
        dl.clear();
        check(dl.worldTexts().empty() && dl.worldVertices().empty() && dl.worldIndices().empty() && dl.worldTextRuns().empty(), "clear() empties the labels");
    }

    if (g_failures == 0) AVER_INFO("=== all text mesh tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
