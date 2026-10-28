// The sub-viewport C ABI (framework_subview_abi.h) end to end through the Aver.Framework DLL: creation by key and the budget (RENDER.SUBVIEWPORTS), every
// setter and what info reads back, the update-mode scheduling through aver_fw_subview_plan, consumers, the on-screen rectangle, the texture the host
// publishes, and the zeros for bad arguments. The store is process-wide, so each section ends with aver_fw_subview_reset. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/framework/framework_subview_abi.h"

#include <cmath>
#include <limits>
#include <string>

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static void resetAll() {
    aver_fw_subview_reset();
    aver_fw_subview_configure(AVER_FW_SUBVIEW_DEFAULT_MAX_VIEWS, AVER_FW_SUBVIEW_DEFAULT_RENDERS, 8.0f);
}

static void testVersionAndNumbers() {
    AVER_INFO("=== version and numbers ===");
    check(aver_fw_subview_abi_version() == 1 && AVER_FW_SUBVIEW_ABI_VERSION == 1, "the sub-viewport ABI is version 1");
    check(AVER_FW_SUBVIEW_UPDATE_DISABLED == 0 && AVER_FW_SUBVIEW_UPDATE_ONCE == 1 && AVER_FW_SUBVIEW_UPDATE_WHEN_VISIBLE == 2 &&
              AVER_FW_SUBVIEW_UPDATE_WHEN_PARENT_VISIBLE == 3 && AVER_FW_SUBVIEW_UPDATE_ALWAYS == 4,
          "the update modes are Godot's");
    check(AVER_FW_SUBVIEW_CLEAR_ALWAYS == 0 && AVER_FW_SUBVIEW_CLEAR_NEVER == 1 && AVER_FW_SUBVIEW_CLEAR_ONCE == 2, "the clear modes are Godot's");
    check(AVER_FW_SUBVIEW_DEFAULT_MAX_VIEWS == 4 && AVER_FW_SUBVIEW_DEFAULT_RENDERS == 2 && AVER_FW_SUBVIEW_ABSOLUTE_MAX == 16, "the budget numbers are pinned");
}

static void testCreateAndFind() {
    AVER_INFO("=== create, find, destroy ===");
    resetAll();
    check(aver_fw_subview_create(nullptr, 64, 64) == 0, "a null key is refused");
    check(aver_fw_subview_create("", 64, 64) == 0, "an empty key is refused");
    const int32_t a = aver_fw_subview_create("Main/Screen", 640, 360);
    check(a > 0 && aver_fw_subview_count() == 1, "a view is made");
    check(aver_fw_subview_find("Main/Screen") == a && aver_fw_subview_find("Nope") == 0 && aver_fw_subview_find(nullptr) == 0, "find answers by key");
    check(aver_fw_subview_create("Main/Screen", 320, 200) == a && aver_fw_subview_count() == 1, "the same key returns the same view");
    int32_t info[8] = {};
    char keyBuf[64] = {};
    check(aver_fw_subview_key(a, keyBuf, sizeof keyBuf) == 11 && std::string(keyBuf) == "Main/Screen", "the key reads back by id");
    char small[5] = {};
    check(aver_fw_subview_key(a, small, sizeof small) == 11 && std::string(small) == "Main", "a short buffer is cut and terminated; the full length is returned");
    check(aver_fw_subview_key(a, nullptr, 0) == 11 && aver_fw_subview_key(999, keyBuf, sizeof keyBuf) == 0, "no buffer asks for the length; an unknown id answers 0");
    check(aver_fw_subview_info(a, info) == 1 && info[4] == 320 && info[5] == 200, "and takes the new size");
    check(info[0] == AVER_FW_SUBVIEW_UPDATE_WHEN_VISIBLE && info[1] == AVER_FW_SUBVIEW_CLEAR_ALWAYS && info[2] == 0 && info[3] == 0 && info[6] == 0,
          "a new view is Godot's default: when visible, clear always, opaque, no camera, never drawn");
    check(aver_fw_subview_id_at(0) == a && aver_fw_subview_id_at(1) == 0 && aver_fw_subview_id_at(-1) == 0, "id_at lists in creation order");
    check(aver_fw_subview_destroy(a) == 1 && aver_fw_subview_destroy(a) == 0 && aver_fw_subview_count() == 0, "destroy removes once");
    check(aver_fw_subview_info(a, info) == 0 && aver_fw_subview_texture(a) == 0, "a destroyed id answers nothing");
    resetAll();
}

static void testBudget() {
    AVER_INFO("=== the view budget ===");
    resetAll();
    int32_t ids[6] = {};
    for (int i = 0; i < 4; ++i) ids[i] = aver_fw_subview_create(("V" + std::to_string(i)).c_str(), 64, 64);
    check(ids[0] > 0 && ids[3] > 0 && aver_fw_subview_count() == 4, "four views fit by default");
    const int64_t refused = aver_fw_subview_refused();
    check(aver_fw_subview_create("V4", 64, 64) == 0 && aver_fw_subview_refused() == refused + 1, "the fifth is refused and counted");
    aver_fw_subview_configure(6, 3, 8.0f);
    check((ids[4] = aver_fw_subview_create("V4", 64, 64)) > 0, "the project can raise the cap");
    aver_fw_subview_configure(0, 2, 8.0f);
    check(aver_fw_subview_create("V5", 64, 64) == 0 && aver_fw_subview_count() == 5, "a cap of zero refuses new views and keeps the old");
    aver_fw_subview_configure(500, 500, 0.0f);
    check(aver_fw_subview_create("V5", 64, 64) > 0 && aver_fw_subview_count() == 6, "absurd values are clamped (16 views at most), and zero pixels keeps the default");
    resetAll();
}

static void testSetters() {
    AVER_INFO("=== setters and info ===");
    resetAll();
    const int32_t id = aver_fw_subview_create("S", 512, 512);
    int32_t info[8] = {};
    check(aver_fw_subview_set_size(id, 800, 450) == 1 && aver_fw_subview_set_size(id, 0, 450) == 0 && aver_fw_subview_set_size(id, 800, -1) == 0, "size: positive only");
    check(aver_fw_subview_set_update(id, AVER_FW_SUBVIEW_UPDATE_ALWAYS) == 1 && aver_fw_subview_set_update(id, 5) == 0 && aver_fw_subview_set_update(id, -1) == 0, "update mode: 0..4");
    check(aver_fw_subview_set_clear(id, AVER_FW_SUBVIEW_CLEAR_NEVER) == 1 && aver_fw_subview_set_clear(id, 3) == 0, "clear mode: 0..2");
    check(aver_fw_subview_set_transparent(id, 1) == 1, "transparent");
    check(aver_fw_subview_set_own_world(id, 1) == 1, "own world");
    check(aver_fw_subview_set_camera(id, 42) == 1, "camera entity");
    check(aver_fw_subview_set_scale(id, 0.5f, 4) == 1 && aver_fw_subview_set_scale(id, 0.0f, 0) == 0 && aver_fw_subview_set_scale(id, std::nanf(""), 0) == 0, "scale: finite and positive");
    check(aver_fw_subview_info(id, info) == 1, "info");
    check(info[0] == AVER_FW_SUBVIEW_UPDATE_ALWAYS && info[1] == AVER_FW_SUBVIEW_CLEAR_NEVER && info[2] == 1 && info[3] == 42, "update, clear, transparent and camera read back");
    // 800x450 at scale 0.5, but the 8 MP cap does not bind: 400x225.
    check(info[4] == 400 && info[5] == 225, "the effective size is the size times the scale");
    int32_t host[8] = {};
    float color[4] = {};
    check(aver_fw_subview_set_clear_color(id, 0.1f, 0.2f, 0.3f, 0.9f) == 1 && aver_fw_subview_set_clear_color(id, std::nanf(""), 0, 0, 0) == 0, "clear colour: finite");
    check(aver_fw_subview_host_info(id, host, color) == 1, "host info");
    check(host[0] == 400 && host[1] == 225 && (host[2] & 1) == 1 && (host[2] & 8) == 8 && host[3] == 42 && host[5] == 4, "the host reads the size, flags, camera and msaa");
    check(std::fabs(color[0] - 0.1f) < 1e-6f && std::fabs(color[2] - 0.3f) < 1e-6f && color[3] == 0.0f, "a transparent view clears with alpha 0");
    aver_fw_subview_set_transparent(id, 0);
    aver_fw_subview_host_info(id, host, color);
    check(std::fabs(color[3] - 0.9f) < 1e-6f, "an opaque view keeps the alpha it was given");
    check(aver_fw_subview_set_size(999, 1, 1) == 0 && aver_fw_subview_set_update(0, 1) == 0 && aver_fw_subview_set_camera(-5, 1) == 0, "bad ids answer 0");
    check(aver_fw_subview_set_camera(id, -3) == 1, "a negative entity clears the camera");
    aver_fw_subview_info(id, info);
    check(info[3] == 0, "the camera is cleared");
    resetAll();
}

static void testScheduling() {
    AVER_INFO("=== scheduling through the ABI ===");
    resetAll();
    const int32_t a = aver_fw_subview_create("A", 128, 128);
    const int32_t b = aver_fw_subview_create("B", 128, 128);
    const int32_t c = aver_fw_subview_create("C", 128, 128);
    int32_t ids[8] = {}, clears[8] = {};
    check(aver_fw_subview_plan(1, ids, clears, 8) == 0, "views without a camera are not drawn");
    for (int32_t id : {a, b, c}) aver_fw_subview_set_camera(id, 10);
    check(aver_fw_subview_plan(2, ids, clears, 8) == 0, "WhenVisible views nothing shows are not drawn");

    check(aver_fw_subview_consumer(a, AVER_FW_SUBVIEW_CONSUMER_MATERIAL, +1) == 1 && aver_fw_subview_consumer(a, 7, +1) == 0, "a material consumer");
    int32_t n = aver_fw_subview_plan(3, ids, clears, 8);
    check(n == 1 && ids[0] == a && clears[0] == 1, "now A is drawn, clearing (its first picture)");
    n = aver_fw_subview_plan(4, ids, clears, 8);
    check(n == 1 && ids[0] == a && clears[0] == 1, "and again next frame (Clear Always)");
    int32_t host[8] = {};
    aver_fw_subview_host_info(a, host, nullptr);
    check((host[2] & 2) == 2, "a view only a material reads is told to store linear values");
    aver_fw_subview_consumer(a, AVER_FW_SUBVIEW_CONSUMER_UI, +1);
    aver_fw_subview_host_info(a, host, nullptr);
    check((host[2] & 2) == 0, "one the UI also reads stores display values");

    aver_fw_subview_set_update(b, AVER_FW_SUBVIEW_UPDATE_ONCE);
    aver_fw_subview_set_update(c, AVER_FW_SUBVIEW_UPDATE_ALWAYS);
    // A (drawn twice), B (a waiting Once), C (never drawn): B and C wait for their first picture and go before A; the budget is 2.
    n = aver_fw_subview_plan(5, ids, clears, 8);
    check(n == 2 && ((ids[0] == b && ids[1] == c) || (ids[0] == c && ids[1] == b)), "views waiting for a first picture go first; two a frame");
    int32_t info[8] = {};
    aver_fw_subview_info(b, info);
    check(info[0] == AVER_FW_SUBVIEW_UPDATE_DISABLED && info[6] == 1, "the Once view reads Disabled afterwards, drawn once");
    check(aver_fw_subview_deferred() > 0, "the postponed render is counted");

    check(aver_fw_subview_request_render(b) == 1 && aver_fw_subview_request_render(123) == 0, "a script can ask for one more render");
    n = aver_fw_subview_plan(6, ids, clears, 8);
    bool sawB = false;
    for (int32_t i = 0; i < n; ++i) sawB = sawB || ids[i] == b;
    check(sawB, "and gets it");
    check(aver_fw_subview_plan(7, nullptr, nullptr, 8) == 0 && aver_fw_subview_plan(7, ids, clears, 0) == 0, "no output array, or none wanted: nothing planned");

    aver_fw_subview_set_update(a, AVER_FW_SUBVIEW_UPDATE_WHEN_PARENT_VISIBLE);
    aver_fw_subview_set_parent_visible(a, 0);
    aver_fw_subview_set_update(c, AVER_FW_SUBVIEW_UPDATE_DISABLED);
    check(aver_fw_subview_plan(8, ids, clears, 8) == 0, "a hidden parent stops WhenParentVisible, Disabled never draws");
    aver_fw_subview_set_parent_visible(a, 1);
    check(aver_fw_subview_plan(9, ids, clears, 8) == 1 && ids[0] == a, "and a visible one resumes it");
    resetAll();
}

static void testScreenAndTexture() {
    AVER_INFO("=== the on-screen rectangle and the texture ===");
    resetAll();
    const int32_t id = aver_fw_subview_create("W", 256, 256);
    float rect[4] = {-1, -1, -1, -1};
    check(aver_fw_subview_screen(id, rect) == 0 && rect[0] == -1, "a view is not on screen at first");
    check(aver_fw_subview_set_screen_rect(id, 10, 20, 640, 360, 1) == 1, "put on screen");
    check(aver_fw_subview_screen(id, rect) == 1 && rect[0] == 10 && rect[1] == 20 && rect[2] == 640 && rect[3] == 360, "the rectangle reads back");
    check(aver_fw_subview_set_screen_rect(id, 0, 0, 0, 10, 1) == 0 && aver_fw_subview_set_screen_rect(id, std::nanf(""), 0, 1, 1, 1) == 0, "an empty or non-finite rectangle is refused");
    int32_t host[8] = {};
    aver_fw_subview_host_info(id, host, nullptr);
    check((host[2] & 4) == 4, "an on-screen view is flagged for the host");
    check(aver_fw_subview_set_screen_rect(id, 0, 0, 0, 0, 0) == 1 && aver_fw_subview_screen(id, rect) == 0, "hidden again");

    int32_t info[8] = {};
    check(aver_fw_subview_texture(id) == 0, "no texture before the host draws");
    check(aver_fw_subview_host_set_texture(id, 0x1234, 256, 256) == 1, "the host publishes its texture");
    check(aver_fw_subview_texture(id) == 0x1234, "scripts read it back");
    aver_fw_subview_info(id, info);
    check(info[7] == 1, "with generation 1");
    aver_fw_subview_host_set_texture(id, 0x1234, 256, 256);
    aver_fw_subview_info(id, info);
    check(info[7] == 1, "the same texture keeps the generation");
    aver_fw_subview_host_set_texture(id, 0x2345, 256, 256);
    aver_fw_subview_info(id, info);
    check(info[7] == 2 && aver_fw_subview_texture(id) == 0x2345, "a rebuilt one bumps it");
    check(aver_fw_subview_host_set_texture(0, 1, 1, 1) == 0 && aver_fw_subview_host_set_texture(id, 1, -1, 1) == 0, "bad arguments are refused");
    resetAll();
}

static void testReset() {
    AVER_INFO("=== session boundary ===");
    resetAll();
    const int32_t a = aver_fw_subview_create("R", 64, 64);
    aver_fw_subview_tick();   // the play state is Editor throughout: no reset
    check(aver_fw_subview_find("R") == a, "a tick in the editor state keeps the views");
    aver_fw_subview_reset();
    check(aver_fw_subview_count() == 0 && aver_fw_subview_find("R") == 0, "reset forgets every view");
    check(aver_fw_subview_create("R", 64, 64) > a, "and ids keep counting");
    resetAll();
}

int main() {
    AVER_INFO("FrameworkSubViewAbiTest");
    testVersionAndNumbers();
    testCreateAndFind();
    testBudget();
    testSetters();
    testScheduling();
    testScreenAndTexture();
    testReset();
    if (g_failures) {
        AVER_ERROR("FrameworkSubViewAbiTest: {} failure(s)", g_failures);
        return 1;
    }
    AVER_INFO("FrameworkSubViewAbiTest: ok");
    return 0;
}
