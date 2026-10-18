// Accessibility host wiring (Runtime/src/GameAccessibility.cpp): applyAccessibilityToPost is a no-op at defaults and
// wins over blended values, reduce motion zeroes the motion effects and nothing else, the camera wrapper is the
// identity until something is active, initAccessibility reads Settings.ini text and the Captions folder, the per-frame
// call reports changes once and lists the visible captions, and the key table in Aver.Framework matches the default
// settings model in Aver.UI key for key (the two cannot see each other, so this test is the pin). A temp project; no
// window, no device.
#include "aver/core/Log.hpp"
#include "aver/framework/AccessKeys.hpp"
#include "aver/framework/framework_access_abi.h"
#include "aver/game/GameAccessibility.hpp"
#include "aver/game/GameCamera.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/rhi/ColourVision.hpp"
#include "aver/ui/UiScreens.hpp"

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

static std::vector<std::string> splitLines(const char* s) {
    std::vector<std::string> out;
    if (!s || !*s) return out;
    std::string cur;
    for (const char* p = s; *p; ++p) {
        if (*p == '\n') { out.push_back(cur); cur.clear(); }
        else cur.push_back(*p);
    }
    out.push_back(cur);
    return out;
}

static void testPost() {
    AVER_INFO("=== applyAccessibilityToPost ===");
    rhi::PostSettings level;
    level.motionBlur = 0.5f;
    level.chromaticAberration = 0.3f;
    level.grain = 0.2f;
    level.lensFlare = 0.4f;
    level.vignette = 0.25f;
    level.bloomIntensity = 0.1f;

    rhi::PostSettings p = level;
    game::applyAccessibilityToPost(p, game::AccessSnapshot{});
    check(rhi::postSettingsEqual(p, level), "at defaults the blended settings come out unchanged");

    game::AccessSnapshot cv;
    cv.colourVisionPacked = rhi::packColourVision(2, true, 0.5f);
    p = level;
    p.colourVision = 1;   // whatever a volume or the editor left there: the player's choice wins
    game::applyAccessibilityToPost(p, cv);
    check(p.colourVision == (2u | 4u) && near(p.colourVisionStrength, 0.5f), "a colour filter sets mode, simulate and strength, over anything blended");
    check(p.motionBlur == 0.5f && p.grain == 0.2f && p.vignette == 0.25f, "and touches nothing else");

    game::AccessSnapshot off;
    p = level;
    p.colourVision = 3;
    game::applyAccessibilityToPost(p, off);
    check(p.colourVision == 0, "no filter chosen: a stray one is cleared");

    game::AccessSnapshot reduce;
    reduce.motionBlurAllowed = 0.0f;
    reduce.lensAllowed = 0.0f;
    p = level;
    game::applyAccessibilityToPost(p, reduce);
    check(p.motionBlur == 0.0f && p.chromaticAberration == 0.0f && p.grain == 0.0f && p.lensFlare == 0.0f, "reduce motion zeroes blur, chromatic aberration, grain and lens flare");
    check(p.vignette == 0.25f && p.bloomIntensity == 0.1f && p.exposure == level.exposure, "and nothing else");

    game::AccessSnapshot blurOnly;
    blurOnly.motionBlurAllowed = 0.0f;
    p = level;
    game::applyAccessibilityToPost(p, blurOnly);
    check(p.motionBlur == 0.0f && p.chromaticAberration == 0.3f && p.grain == 0.2f, "the blur switch and the lens switch are independent");
}

static void testTablePin() {
    AVER_INFO("=== the key table and the default settings model agree ===");
    const ui::UiSettingsModel model = ui::uiDefaultSettingsModel();
    int checked = 0;
    bool all = true;
    for (usize i = 0; i < fw::access::accessKeyCount(); ++i) {
        const fw::access::AccessKey& k = fw::access::kAccessKeys[i];
        const ui::UiSettingItem* item = model.find(k.key);
        if (!k.tab) {
            if (item) { all = false; AVER_ERROR("   {} is a developer key but the screen has it", k.key); }
            continue;
        }
        ++checked;
        if (!item) { all = false; AVER_ERROR("   {} is missing from the default settings model", k.key); continue; }
        const ui::UiSettingItem::Type want = k.kind == fw::access::AccessKind::Toggle ? ui::UiSettingItem::Type::Toggle
                                           : k.kind == fw::access::AccessKind::Choice ? ui::UiSettingItem::Type::Choice
                                                                                      : ui::UiSettingItem::Type::Slider;
        bool ok = item->type == want && item->label == k.label && item->tab == k.tab && near(item->value, k.def);
        if (k.kind == fw::access::AccessKind::Slider)
            ok = ok && near(item->minValue, k.lo) && near(item->maxValue, k.hi) && near(item->step, k.step);
        if (k.kind == fw::access::AccessKind::Choice) ok = ok && item->choices == splitLines(k.choices);
        if (!ok) { all = false; AVER_ERROR("   {} differs between AccessKeys.hpp and UiScreens.cpp", k.key); }
    }
    check(all && checked > 20, "every screen key matches its model item (kind, range, step, default, label, tab, choices)");
    bool reverse = true;
    for (const ui::UiSettingItem& item : model.items) {
        const bool ours = item.key == "graphics.uiScale" || item.key.rfind("access.", 0) == 0;
        if (ours && !fw::access::accessFind(item.key)) { reverse = false; AVER_ERROR("   {} is in the model but not in the key table", item.key); }
    }
    check(reverse, "and every accessibility item in the model is in the key table");
}

static void testInitAndFrame() {
    AVER_INFO("=== initAccessibility and the frame ===");
    const fs::path dir = fs::temp_directory_path() / "aver_game_access_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "Content" / "Captions");
    const std::string settings = (dir / "Settings.ini").generic_string();
    writeFileText(settings, "graphics.uiScale=1.500000\naccess.captions.mode=2\naccess.captions.lines=2\naccess.colour.mode=1\naccess.speech.enabled=0\n");
    writeFileText((dir / "Content" / "Captions" / "t.occaptions").generic_string(),
                  "OCCAPTIONS 1\nCUE c1 tag \"Hello\"\nCUE c2 sound \"x.ocaudio\" tag \"[x]\"\n");

    game::AccessibilityInit in;
    in.settingsPath = settings;
    in.contentDir = (dir / "Content").generic_string();
    game::initAccessibility(in);

    game::AccessSnapshot s = game::accessibilitySnapshot();
    check(near(s.uiScale, 1.5f) && s.colourVisionPacked == rhi::packColourVision(1, false, 1.0f), "options come from the INI text before the first frame");
    check(aver_fw_caption_cue_count() == 2, "and the Captions folder is loaded");

    aver_fw_caption_show("c1", 0, 0, 0, 0);
    game::AccessFrame f;
    const float pos[3] = {0, 0, 0}, fwd[3] = {1, 0, 0}, right[3] = {0, 1, 0};
    game::accessibilityFrame(0.1f, pos, fwd, right, f);
    check(f.changed, "the first frame reports the options as changed");
    check(f.captions.size() == 1 && f.captions[0].text == "Hello" && f.captionSizeScale == 1.0f && !f.captionsTop, "and lists the visible caption with the style");
    check(near(f.snap.uiScale, 1.5f), "with the snapshot");
    game::accessibilityFrame(0.1f, pos, fwd, right, f);
    check(!f.changed, "the next frame reports no change");
    aver_fw_access_set("access.ui.textScale", 1.5f);
    game::accessibilityFrame(0.1f, pos, fwd, right, f);
    check(f.changed && near(f.snap.textScale, 1.5f), "a changed option is reported once");
    game::accessibilityFrame(0.1f, pos, fwd, right, f);
    check(!f.changed, "and not again");
    aver_fw_access_set("access.captions.position", 1.0f);
    aver_fw_access_set("access.captions.size", 3.0f);
    game::accessibilityFrame(0.1f, Vec3{0, 0, 0}, 0.0f, 0.0f, f);   // the camera-pose overload
    check(f.captionsTop && f.captionSizeScale == 1.9f, "top-of-screen and the size preset reach the frame");

    // Camera: identity until something is active.
    Vec3 p{10, 20, 30};
    f32 yaw = 0.5f, pitch = -0.2f;
    game::applyAccessibilityCamera(p, yaw, pitch, 0.016f);
    check(p.x == 10 && p.y == 20 && p.z == 30 && yaw == 0.5f && pitch == -0.2f, "the camera wrapper is the identity at defaults");
    game::applyCameraFx(p, yaw, pitch, 0.016f);
    check(p.x == 10 && yaw == 0.5f, "(GameCamera.hpp's applyCameraFx too)");
    aver_fw_camera_shake_add(1.0f);
    game::applyAccessibilityCamera(p, yaw, pitch, 0.016f);
    check(p.x != 10 || p.y != 20 || p.z != 30 || yaw != 0.5f, "a shake moves it");
    aver_fw_camera_shake_add(1.0f);
    aver_fw_access_set("access.motion.reduce", 1.0f);
    Vec3 q{1, 2, 3};
    f32 qy = 0.1f, qp = 0.2f;
    game::applyAccessibilityCamera(q, qy, qp, 0.016f);
    check(q.x == 1 && qy == 0.1f, "reduce motion leaves it alone");

    aver_fw_caption_clear();
    aver_fw_access_load_ini("");
    fs::remove_all(dir, ec);
}

int main() {
    AVER_INFO("GameAccessibilityTest");
    testPost();
    testTablePin();
    testInitAndFrame();
    AVER_INFO(g_failures == 0 ? "GameAccessibilityTest: PASS" : "GameAccessibilityTest: FAIL");
    return g_failures == 0 ? 0 : 1;
}
