// UI images on the C ABI (ui_abi.h "images"): the table a host installs, what the exports do without one, and an
// Image widget's content path reaching the draw list as the host's texture. A fake host stands in for the renderer's
// cache. No GPU, no window.
#include "aver/core/Log.hpp"
#include "aver/ui/UiDrawList.hpp"
#include "aver/ui/ui_abi.h"
#include "aver/ui/ui_widget_abi.h"

#include <map>
#include <string>
#include <vector>

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

namespace {

std::map<std::string, uint64_t> g_ids = {{"Textures/a.png", 0x1001}, {"Textures/b.png", 0x1002}};
std::vector<uint64_t> g_released;
int g_uses = 0;

uint64_t fakeAcquire(const char* p) { const auto it = g_ids.find(p); return it == g_ids.end() ? 0 : it->second; }
void fakeRelease(uint64_t id) { g_released.push_back(id); }
int32_t fakeSize(uint64_t id, int32_t* wh) {
    if (id != 0x1001) return 0;
    wh[0] = 64; wh[1] = 32;
    return 1;
}
uint64_t fakeUse(const char* p) { ++g_uses; return fakeAcquire(p); }

// The texture the widget frame drew with, or 0 when no command carries one.
uint64_t drawnTexture() {
    const auto* dl = static_cast<const aver::ui::UiDrawList*>(aver_ui_draw_list());
    if (!dl) return 0;
    for (uint8_t l = 0; l < static_cast<uint8_t>(aver::ui::UiLayer::Count); ++l)
        for (const aver::ui::UiDrawCmd& c : dl->commands(static_cast<aver::ui::UiLayer>(l)))
            if (c.texture != 0) return c.texture;
    return 0;
}

} // namespace

int main() {
    AVER_INFO("=== no host ===");
    {
        int32_t wh[2] = {7, 7};
        check(aver_ui_image_available() == 0, "no image table is installed at first");
        check(aver_ui_image_acquire("Textures/a.png") == 0, "acquire is 0");
        check(aver_ui_image_use("Textures/a.png") == 0, "use is 0");
        check(aver_ui_image_size(0x1001, wh) == 0 && wh[0] == 0 && wh[1] == 0, "size is 0 and zeroes the output");
        aver_ui_image_release(0x1001);   // must not crash
    }

    AVER_INFO("=== a host ===");
    {
        aver_ui_set_image_host(&fakeAcquire, &fakeRelease, &fakeSize, &fakeUse);
        check(aver_ui_image_available() == 1, "available once installed");
        check(aver_ui_image_acquire("Textures/a.png") == 0x1001, "acquire returns the host's id");
        check(aver_ui_image_acquire("Textures/missing.png") == 0, "an image the host refuses is 0");
        check(aver_ui_image_acquire(nullptr) == 0 && aver_ui_image_acquire("") == 0, "a null or empty path is 0 and never reaches the host");
        int32_t wh[2] = {0, 0};
        check(aver_ui_image_size(0x1001, wh) == 1 && wh[0] == 64 && wh[1] == 32, "size comes from the host");
        check(aver_ui_image_size(0x1002, wh) == 0 && wh[0] == 0, "an unknown id is 0");
        check(aver_ui_image_size(0x1001, nullptr) == 0, "a null output is refused");
        aver_ui_image_release(0x1001);
        aver_ui_image_release(0);
        check(g_released.size() == 1 && g_released[0] == 0x1001, "release reaches the host, and id 0 does not");
    }

    AVER_INFO("=== an Image widget resolves its path ===");
    {
        const uint32_t root = aver_ui_widget_create(AVER_UI_KIND_PANEL, 0, "Root");
        const uint32_t img = aver_ui_widget_create(AVER_UI_KIND_IMAGE, root, "Pic");
        aver_ui_widget_set_prop(img, "width", "100");
        aver_ui_widget_set_prop(img, "height", "50");
        aver_ui_widget_set_prop(img, "image", "Textures/a.png");

        g_uses = 0;
        aver_ui_begin_frame(0, 0, 1280, 720);
        aver_ui_widgets_frame(0.016f);
        check(drawnTexture() == 0x1001, "the widget draws the host's texture for its path");
        check(g_uses >= 1, "it asked with the borrowing call, not by acquiring");

        aver_ui_widget_set_prop(img, "image", "Textures/missing.png");
        aver_ui_begin_frame(0, 0, 1280, 720);
        aver_ui_widgets_frame(0.016f);
        check(drawnTexture() == 0, "a path the host cannot load keeps the placeholder");

        aver_ui_widget_set_prop(img, "image", "Textures/b.png");
        aver_ui_widget_set_texture(img, 0x7777);
        aver_ui_begin_frame(0, 0, 1280, 720);
        aver_ui_widgets_frame(0.016f);
        check(drawnTexture() == 0x7777, "a texture the host set on the widget wins over the path");

        aver_ui_widget_destroy(img);
    }

    AVER_INFO("=== clearing ===");
    {
        aver_ui_set_image_host(nullptr, &fakeRelease, &fakeSize, &fakeUse);
        check(aver_ui_image_available() == 0 && aver_ui_image_use("Textures/a.png") == 0, "a NULL acquire clears the whole table");
    }

    AVER_INFO("UiImageAbi tests done: {} failure(s)", g_failures);
    return g_failures ? 1 : 0;
}
