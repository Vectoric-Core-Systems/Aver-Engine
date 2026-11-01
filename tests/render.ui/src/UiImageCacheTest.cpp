// UI images by content path: the cache's path rules, reference counting and delayed destruction, and the
// premultiplied mip chain, with a fake loader instead of a GPU. Exit code = failure count.
#include "aver/render/ui/UiImageCache.hpp"
#include "aver/render/ui/UiRenderer.hpp"
#include "aver/core/Log.hpp"

#include <map>
#include <string>
#include <vector>

using namespace aver;
using aver::render::ui::UiImageCache;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

namespace {

// Stands in for the host: files that "exist" load into the next handle, everything else fails.
struct Fake {
    std::map<std::string, std::pair<u32, u32>> files;
    std::vector<std::string> loads;
    std::vector<rhi::TextureHandle> destroyed;
    rhi::TextureHandle next = 100;

    UiImageCache make() {
        return UiImageCache(
            [this](const std::string& p, u32& w, u32& h, std::string& err) -> rhi::TextureHandle {
                loads.push_back(p);
                const auto it = files.find(p);
                if (it == files.end()) { err = "no such file"; return 0; }
                w = it->second.first;
                h = it->second.second;
                return next++;
            },
            [this](rhi::TextureHandle t) { destroyed.push_back(t); });
    }
};

void tickN(UiImageCache& c, u32 n) { for (u32 i = 0; i < n; ++i) c.tick(); }

} // namespace

int main() {
    AVER_INFO("=== path rules ===");
    {
        check(UiImageCache::normalizePath("Textures/ui/a.png") == "Textures/ui/a.png", "a content path is kept");
        check(UiImageCache::normalizePath("Textures\\ui\\a.png") == "Textures/ui/a.png", "back slashes become forward slashes");
        check(UiImageCache::normalizePath("./Textures//ui/./a.png") == "Textures/ui/a.png", "dot and empty segments are dropped");
        check(UiImageCache::normalizePath("Textures/ui/") == "Textures/ui", "a trailing slash is dropped");
        check(UiImageCache::normalizePath("").empty(), "empty is refused");
        check(UiImageCache::normalizePath("../secret.png").empty(), "a leading .. is refused");
        check(UiImageCache::normalizePath("Textures/../../secret.png").empty(), "a .. in the middle is refused");
        check(UiImageCache::normalizePath("/etc/passwd").empty(), "an absolute path is refused");
        check(UiImageCache::normalizePath("\\\\server\\share\\a.png").empty(), "a UNC path is refused");
        check(UiImageCache::normalizePath("C:/Windows/a.png").empty(), "a drive path is refused");
        check(UiImageCache::normalizePath("res://a.png").empty(), "a scheme is refused");
        check(UiImageCache::normalizePath(std::string(2000, 'a')).empty(), "an absurdly long path is refused");
    }

    AVER_INFO("=== acquire and release ===");
    {
        Fake f;
        f.files["Textures/a.png"] = {64, 32};
        f.files["Textures/b.png"] = {8, 8};
        UiImageCache c = f.make();

        const u64 a = c.acquire("Textures/a.png");
        check(a != 0 && a == render::ui::UiRenderer::textureId(100), "the id is the texture handle the UI draws with");
        check(c.acquire("textures/missing.png") == 0, "a missing file gives 0");
        u32 w = 0, h = 0;
        check(c.size(a, w, h) && w == 64 && h == 32, "the size is the source image's");
        check(!c.size(12345, w, h), "an unknown id has no size");

        const u64 a2 = c.acquire("Textures\\a.png");
        check(a2 == a && f.loads.size() == 2, "a second acquire of the same path (any slashes) shares the texture: one load, plus the failed one");
        check(c.references(a) == 2, "two references");

        const u64 b = c.acquire("Textures/b.png");
        check(b != a && c.textureCount() == 2, "another path is another texture");

        check(c.release(a), "release succeeds");
        check(c.references(a) == 1, "one reference left");
        check(c.release(a), "the last release succeeds");
        check(!c.release(a), "a release with none left is refused");
        check(!c.release(999999), "an unknown id is refused");
        check(f.destroyed.empty(), "nothing is destroyed on release");
        (void)b;
    }

    AVER_INFO("=== failures are remembered ===");
    {
        Fake f;
        UiImageCache c = f.make();
        c.acquire("Textures/nope.png");
        c.acquire("Textures/nope.png");
        c.use("Textures/nope.png");
        check(f.loads.size() == 1, "a path that did not load is tried once");
        f.files["Textures/nope.png"] = {4, 4};
        check(c.acquire("Textures/nope.png") == 0, "and stays failed while the table is not reset");
        c.releaseAll();
        check(c.acquire("Textures/nope.png") != 0, "releaseAll forgets failures, so a file added later loads");
    }

    AVER_INFO("=== delayed destruction ===");
    {
        Fake f;
        f.files["a.png"] = {2, 2};
        UiImageCache c = f.make();
        const u64 id = c.acquire("a.png");
        tickN(c, 20);
        check(f.destroyed.empty(), "a referenced texture is never destroyed");
        c.release(id);
        tickN(c, UiImageCache::kGraceFrames - 1);
        check(f.destroyed.empty() && c.textureCount() == 1, "an unreferenced texture waits out the grace frames");
        const u64 again = c.acquire("a.png");
        check(again == id && f.loads.size() == 1, "acquiring it inside the grace resurrects it without a reload");
        c.release(again);
        tickN(c, UiImageCache::kGraceFrames);
        check(f.destroyed.size() == 1 && f.destroyed[0] == 100 && c.textureCount() == 0, "after the grace it is destroyed once");
        u32 w = 0, h = 0;
        check(!c.size(id, w, h), "and its id is gone");
        check(c.acquire("a.png") != 0 && f.loads.size() == 2, "a later acquire loads it afresh");
    }

    AVER_INFO("=== use keeps a texture alive without a reference ===");
    {
        Fake f;
        f.files["w.png"] = {2, 2};
        UiImageCache c = f.make();
        const u64 id = c.use("w.png");
        check(id != 0 && c.references(id) == 0, "use gives an id and takes no reference");
        for (u32 i = 0; i < 40; ++i) { c.tick(); c.use("w.png"); }
        check(f.destroyed.empty() && f.loads.size() == 1, "asked for every frame, it is never destroyed or reloaded");
        tickN(c, UiImageCache::kGraceFrames);
        check(f.destroyed.size() == 1, "stop asking and it goes after the grace");
    }

    AVER_INFO("=== releaseAll and shutdown ===");
    {
        Fake f;
        f.files["a.png"] = {2, 2};
        f.files["b.png"] = {2, 2};
        UiImageCache c = f.make();
        const u64 a = c.acquire("a.png");
        c.acquire("a.png");
        const u64 b = c.acquire("b.png");
        c.releaseAll();
        check(c.references(a) == 0 && c.references(b) == 0, "releaseAll drops every reference");
        check(f.destroyed.empty(), "and destroys nothing yet: the frame in flight may still name them");
        check(!c.release(a), "a late release from the game is ignored");
        tickN(c, UiImageCache::kGraceFrames);
        check(f.destroyed.size() == 2, "they go at the grace");

        c.acquire("a.png");
        c.shutdown();
        check(f.destroyed.size() == 3 && c.textureCount() == 0, "shutdown destroys what is left at once");
    }
    {
        Fake f;
        f.files["a.png"] = {2, 2};
        {
            UiImageCache c = f.make();
            c.acquire("a.png");
        }
        check(f.destroyed.size() == 1, "the destructor destroys the textures");
    }

    AVER_INFO("=== premultiplied mip chain ===");
    {
        ImageData img;
        img.width = 4;
        img.height = 2;
        img.channels = 4;
        img.pixels.assign(4 * 2 * 4, 0);
        // Left half opaque white, texel (2,0) half-transparent pure red (straight), texel (3,*) fully transparent with a stray colour.
        const auto put = [&](u32 x, u32 y, u8 r, u8 g, u8 b, u8 a) {
            u8* p = &img.pixels[(static_cast<usize>(y) * 4 + x) * 4];
            p[0] = r; p[1] = g; p[2] = b; p[3] = a;
        };
        put(0, 0, 255, 255, 255, 255); put(1, 0, 255, 255, 255, 255);
        put(0, 1, 255, 255, 255, 255); put(1, 1, 255, 255, 255, 255);
        put(2, 0, 255, 0, 0, 128);     put(2, 1, 255, 0, 0, 128);
        put(3, 0, 200, 200, 200, 0);   put(3, 1, 200, 200, 200, 0);

        render::ui::UiImageLevels lv;
        std::string err;
        check(render::ui::buildUiImageLevels(img, lv, &err), "a valid image builds");
        check(lv.width == 4 && lv.height == 2, "the size is kept");
        check(lv.levels.size() == 3, "4x2 gives 4x2, 2x1, 1x1");
        const std::vector<u8>& l0 = lv.levels[0];
        check(l0[0] == 255 && l0[3] == 255, "an opaque texel is unchanged");
        const u8* half = &l0[(2) * 4];
        check(half[0] == 128 && half[1] == 0 && half[3] == 128, "a half-transparent red texel has its colour multiplied by alpha");
        const u8* clear = &l0[(3) * 4];
        check(clear[0] == 0 && clear[1] == 0 && clear[2] == 0 && clear[3] == 0, "a fully transparent texel is black, so it cannot fringe");
        check(lv.levels[1].size() == 2 * 1 * 4, "level 1 is 2x1");
        check(lv.levels[1][0] == 255 && lv.levels[1][3] == 255, "level 1 left texel is opaque white");
        check(lv.levels[1][4 + 0] == 64 && lv.levels[1][4 + 3] == 64, "level 1 right texel averages the premultiplied texels (64, 0, 0, 64)");
        check(lv.levels[2].size() == 4, "the chain ends at 1x1");

        ImageData bad;
        check(!render::ui::buildUiImageLevels(bad, lv, &err) && !err.empty(), "an empty image is refused with a reason");
        ImageData huge;
        huge.width = render::ui::kMaxUiImageDimension + 1;
        huge.height = 1;
        huge.pixels.assign(static_cast<usize>(huge.width) * 4, 255);
        check(!render::ui::buildUiImageLevels(huge, lv, &err), "an oversized image is refused");
    }

    if (g_failures) AVER_ERROR("=== {} FAILED ===", g_failures);
    else AVER_INFO("=== UiImageCacheTest: all passed ===");
    return g_failures;
}
