// Headless test for the glyph atlas: shelf packing never overlaps and stays inside its page, page-level
// LRU recycles the right page, a page used this frame is never recycled, and generation / dirty semantics.
#include "aver/core/Log.hpp"
#include "aver/ui/UiGlyphAtlas.hpp"

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

struct Lcg {
    u32 s = 987654321u;
    u32 next() { s = s * 1664525u + 1013904223u; return s >> 8; }
};

static UiGlyphBitmap bitmapOf(u16 w, u16 h, u8 fill) {
    UiGlyphBitmap b;
    b.w = w;
    b.h = h;
    b.offX = -1;
    b.offY = -5;
    b.pixels.assign(static_cast<usize>(w) * h, fill);
    return b;
}

struct Rect { u16 page, x, y, w, h; };

static bool overlaps(const Rect& a, const Rect& b) {
    if (a.page != b.page) return false;
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

int main() {
    AVER_INFO("=== configuration ===");
    {
        UiGlyphAtlas a;
        check(a.pageSize() == 512 && a.config().maxPages == 16, "defaults are 512 x 512 pages, at most 16");
        a.configure({100, 0});
        check(a.pageSize() == 256 && a.config().maxPages == 1, "the page size and cap are clamped (256..2048, 1..64)");
        a.configure({4096, 500});
        check(a.pageSize() == 2048 && a.config().maxPages == 64, "from above too");
    }

    AVER_INFO("=== packing without recycling ===");
    {
        UiGlyphAtlas a({256, 6});
        Lcg rng;
        std::vector<Rect> placed;
        u32 id = 0;
        bool inside = true, disjoint = true, pixelsOk = true;
        // One frame throughout: every page counts as used, so nothing can be recycled and the atlas fills.
        while (true) {
            const u16 w = static_cast<u16>(1 + rng.next() % 90), h = static_cast<u16>(1 + rng.next() % 90);
            const u8 fill = static_cast<u8>(1 + id % 250);
            const UiAtlasGlyph* g = a.insert(UiGlyphKey{1, 16, id}, bitmapOf(w, h, fill), 7);
            if (!g) break;
            ++id;
            const Rect r{g->page, g->x, g->y, g->w, g->h};
            if (r.x + r.w > 256 || r.y + r.h > 256 || r.page >= a.pageCount()) inside = false;
            for (const Rect& o : placed) if (overlaps(r, o)) disjoint = false;
            placed.push_back(r);
            if (a.pagePixels(r.page)[static_cast<usize>(r.y) * 256 + r.x] != fill) pixelsOk = false;
            if (id > 100000) break;
        }
        check(id > 100, "hundreds of random glyphs fit before the atlas is full (" + std::to_string(id) + ")");
        check(a.pageCount() == 6, "it used all six pages");
        check(inside, "every glyph stays inside its page");
        check(disjoint, "no two glyphs overlap");
        check(pixelsOk, "and each glyph's pixels landed at its rectangle");
        check(a.overflow() >= 1 && a.evictedPages() == 0, "the failed insert counted as overflow and nothing was recycled");
        check(a.glyphsResident() == id, "every successful insert is resident");
    }

    AVER_INFO("=== a million random inserts with recycling ===");
    {
        UiGlyphAtlas a({512, 8});
        Lcg rng;
        std::vector<std::vector<Rect>> perPage(8);
        std::vector<u32> gen(8, 0);
        UiGlyphBitmap bmp = bitmapOf(160, 160, 77);
        bool inside = true, disjoint = true;
        u64 inserted = 0, failed = 0;
        for (u32 i = 0; i < 1000000; ++i) {
            const u64 frame = 1 + i / 8;   // a new frame every 8 inserts: pages are recyclable between frames
            bmp.w = static_cast<u16>(1 + rng.next() % 160);
            bmp.h = static_cast<u16>(1 + rng.next() % 160);
            const UiAtlasGlyph* g = a.insert(UiGlyphKey{2, 20, i}, bmp, frame);
            if (!g) { ++failed; continue; }
            ++inserted;
            const u16 pg = g->page;
            if (pg >= 8) { inside = false; break; }
            if (a.pageGeneration(pg) != gen[pg]) { gen[pg] = a.pageGeneration(pg); perPage[pg].clear(); }
            const Rect r{pg, g->x, g->y, g->w, g->h};
            if (r.x + r.w > 512 || r.y + r.h > 512) inside = false;
            for (const Rect& o : perPage[pg]) if (overlaps(r, o)) { disjoint = false; break; }
            perPage[pg].push_back(r);
            if (!inside || !disjoint) break;
        }
        check(inside, "all glyphs stayed inside their pages");
        check(disjoint, "and none overlapped a glyph resident on the same page at that moment");
        check(inserted > 900000, "almost every insert succeeded (" + std::to_string(inserted) + ", failed " + std::to_string(failed) + ")");
        check(a.evictedPages() > 100, "pages were recycled many times (" + std::to_string(a.evictedPages()) + ")");
    }

    AVER_INFO("=== page-level LRU ===");
    {
        UiGlyphAtlas a({256, 4});
        const UiGlyphBitmap full = bitmapOf(256, 256, 9);
        a.insert(UiGlyphKey{1, 16, 1}, full, 1);   // page 0
        a.insert(UiGlyphKey{1, 16, 2}, full, 2);   // page 1
        a.insert(UiGlyphKey{1, 16, 3}, full, 3);   // page 2
        a.insert(UiGlyphKey{1, 16, 4}, full, 4);   // page 3
        check(a.pageCount() == 4, "four full pages");
        for (u32 p = 0; p < 4; ++p) check(a.pageLastUsed(p) == p + 1, "page " + std::to_string(p) + " was last used at frame " + std::to_string(p + 1));

        a.lookup(UiGlyphKey{1, 16, 2}, 5);           // touch page 1
        check(a.pageLastUsed(1) == 5, "a lookup stamps the page");
        for (u32 p = 0; p < 4; ++p) a.markUploaded(p);
        const UiAtlasGlyph* e = a.insert(UiGlyphKey{1, 16, 5}, full, 5);
        check(e != nullptr && e->page == 0, "the new glyph recycled page 0, the least recently used");
        check(a.lookup(UiGlyphKey{1, 16, 1}, 5) == nullptr, "the glyph that lived there is gone");
        check(a.lookup(UiGlyphKey{1, 16, 2}, 5) && a.lookup(UiGlyphKey{1, 16, 3}, 5) && a.lookup(UiGlyphKey{1, 16, 4}, 5),
              "the other pages' glyphs survive");
        check(a.evictedPages() == 1 && a.pageGeneration(0) == 1 && a.pageGeneration(1) == 0, "one eviction, generation bumped on that page only");
        check(a.pageDirty(0) && !a.pageDirty(1), "the recycled page is dirty, the others are not");
        a.markUploaded(0);
        check(!a.pageDirty(0), "markUploaded clears dirty");
        check(a.pageGeneration(0) == 1, "and leaves the generation alone");
    }

    AVER_INFO("=== a page used this frame is never recycled ===");
    {
        UiGlyphAtlas a({256, 2});
        const UiGlyphBitmap full = bitmapOf(256, 256, 3);
        a.insert(UiGlyphKey{1, 16, 1}, full, 10);
        a.insert(UiGlyphKey{1, 16, 2}, full, 10);
        const UiAtlasGlyph* none = a.insert(UiGlyphKey{1, 16, 3}, full, 10);
        check(none == nullptr, "every page was touched this frame: the insert fails");
        check(a.overflow() == 1 && a.evictedPages() == 0, "it counts as overflow, not as an eviction");
        check(a.lookup(UiGlyphKey{1, 16, 1}, 10) && a.lookup(UiGlyphKey{1, 16, 2}, 10), "and both glyphs are intact");
        const UiAtlasGlyph* later = a.insert(UiGlyphKey{1, 16, 3}, full, 11);
        check(later != nullptr && a.evictedPages() == 1, "next frame it recycles a page");
    }

    AVER_INFO("=== blank, oversized and repeated glyphs ===");
    {
        UiGlyphAtlas a({256, 2});
        UiGlyphBitmap blank;
        blank.offX = 3;
        blank.offY = -2;
        const UiAtlasGlyph* b = a.insert(UiGlyphKey{1, 16, 32}, blank, 1);
        check(b && b->page == kUiNoPage && b->w == 0 && b->h == 0 && b->offX == 3, "a blank glyph is remembered without a page");
        check(a.pageCount() == 0, "and takes no space");
        check(a.lookup(UiGlyphKey{1, 16, 32}, 2) == b, "lookup finds it");

        check(a.insert(UiGlyphKey{1, 16, 40}, bitmapOf(257, 10, 1), 1) == nullptr, "a glyph wider than a page is refused");
        check(a.insert(UiGlyphKey{1, 16, 41}, bitmapOf(10, 300, 1), 1) == nullptr, "and one taller");

        const UiAtlasGlyph* first = a.insert(UiGlyphKey{1, 16, 50}, bitmapOf(10, 10, 5), 3);
        const UiAtlasGlyph* again = a.insert(UiGlyphKey{1, 16, 50}, bitmapOf(10, 10, 6), 3);
        check(first && first == again && a.glyphsResident() == 2, "inserting a key that is resident returns the existing entry");
        UiGlyphBitmap shortData = bitmapOf(10, 10, 5);
        shortData.pixels.resize(5);
        check(a.insert(UiGlyphKey{1, 16, 51}, shortData, 3) == nullptr, "a bitmap with too few pixels is refused");
    }

    AVER_INFO("=== clear and configure ===");
    {
        UiGlyphAtlas a({256, 2});
        a.insert(UiGlyphKey{1, 16, 1}, bitmapOf(20, 20, 200), 1);
        a.markUploaded(0);
        a.clear();
        check(a.glyphsResident() == 0 && a.pageCount() == 1, "clear forgets the glyphs and keeps the page");
        check(a.pageDirty(0) && a.pageGeneration(0) == 1, "and marks it dirty with a new generation");
        bool zero = true;
        for (usize i = 0; i < 256u * 256u; ++i) if (a.pagePixels(0)[i] != 0) zero = false;
        check(zero, "the pixels are zeroed");
        check(a.pagePixels(5) == nullptr && !a.pageDirty(5) && a.pageGeneration(5) == 0, "a bad page index answers nothing");
        a.configure({512, 2});
        check(a.pageCount() == 0 && a.glyphsResident() == 0, "configure drops everything");
    }

    if (g_failures == 0) AVER_INFO("=== all glyph atlas tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
