// The accessibility C ABI (framework_access_abi.h) end to end through the Aver.Framework DLL: options (set, clamp,
// revision, dirty drain, the Access.Changed event, load_ini, effective), captions from a folder (merge, show, lines,
// new lines, the audio observer path with a fake voice probe, the Caption.Show bus event and its re-subscription,
// hot reload), speech with whatever synthesiser the platform has (volume 0), camera feel, and the input-assist hooks
// in the action layer. Also scans AudioAbi.cpp to confirm every play function notifies the observer. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/framework/AccessKeys.hpp"
#include "aver/framework/framework_abi.h"
#include "aver/framework/framework_access_abi.h"
#include "aver/framework/framework_timers_abi.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace aver;
namespace fs = std::filesystem;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// ---- the bus: a native dispatcher that records Access.Changed ----------------------------------------------

static std::vector<std::pair<std::string, float>> g_changed;
static int32_t AVER_FW_CALL onEvent(int32_t, int64_t token, void*) {
    if (token == 1) g_changed.emplace_back(aver_fw_event_current_arg_string(0), aver_fw_event_current_arg_float(1));
    return 0;
}

static std::string drainKey(float* value) {
    char buf[160] = {};
    return aver_fw_access_take_dirty(0, buf, sizeof buf, value) ? std::string(buf) : std::string("(none)");
}

static void testOptions() {
    AVER_INFO("=== options ===");
    check(aver_fw_access_abi_version() == 1 && AVER_FW_ACCESS_ABI_VERSION == 1, "the access ABI is version 1");
    check(aver_fw_access_key_count() == static_cast<int32_t>(fw::access::kAccessKeyCount), "the key count is the table's");
    check(std::string(aver_fw_access_key_at(0)) == "graphics.uiScale" && std::string(aver_fw_access_key_at(9999)).empty(), "keys are listed in table order; out of range is empty");

    aver_fw_access_load_ini("");
    float info[6] = {};
    check(aver_fw_access_key_info("access.ui.textScale", info) == 1 && info[0] == 1.0f && near(info[1], 0.75f) && near(info[2], 2.0f) && near(info[3], 0.05f) && near(info[4], 1.0f) && near(info[5], 1.0f),
          "key_info: slider, 0.75..2.0, step 0.05, default 1, current 1");
    check(aver_fw_access_key_info("access.colour.mode", info) == 1 && info[0] == 2.0f, "a choice reports kind 2");
    check(aver_fw_access_key_info("nope", info) == 0, "an unknown key is 0");
    check(std::string(aver_fw_access_key_choices("access.colour.mode")).find("Protanopia") != std::string::npos, "choices are listed as refs with English defaults");
    check(std::string(aver_fw_access_key_choices("access.ui.textScale")).empty(), "a slider has none");

    // set / clamp / unknown / revision / event
    aver_fw_set_event_dispatch(&onEvent, nullptr);
    aver_fw_event_subscribe("Access.Changed", 0, 0, 0, 1);
    g_changed.clear();
    float v = 0;
    while (aver_fw_access_take_dirty(0, nullptr, 0, &v)) {}   // drain whatever the earlier calls left
    const int32_t rev0 = aver_fw_access_revision();
    check(aver_fw_access_set("access.ui.textScale", 5.0f) == 1 && near(aver_fw_access_get("access.ui.textScale"), 2.0f), "set clamps to the range");
    check(aver_fw_access_revision() == rev0 + 1, "an accepted change bumps the revision");
    check(aver_fw_access_set("access.ui.textScale", 2.0f) == 1 && aver_fw_access_revision() == rev0 + 1, "the same value changes nothing");
    check(aver_fw_access_set("nope", 1.0f) == 0 && aver_fw_access_get("nope") == 0.0f, "an unknown key is refused and reads 0");
    aver_fw_access_set("access.motion.shake", 0.5f);
    aver_fw_events_flush();
    check(g_changed.size() == 2 && g_changed[0].first == "access.ui.textScale" && near(g_changed[0].second, 2.0f) && g_changed[1].first == "access.motion.shake",
          "Access.Changed is posted once per accepted change, in order, with the key and value");

    // dirty drain in order
    check(drainKey(&v) == "access.ui.textScale" && near(v, 2.0f), "the dirty list drains oldest first");
    check(drainKey(&v) == "access.motion.shake" && near(v, 0.5f), "then the next");
    check(drainKey(&v) == "(none)", "then nothing");

    // load_ini
    check(aver_fw_access_load_ini("graphics.uiScale=1.500000\naccess.captions.size=3\nbogus=1\n") == 2, "load_ini counts the keys it read");
    check(near(aver_fw_access_get("graphics.uiScale"), 1.5f) && aver_fw_access_get("access.captions.size") == 3.0f, "and applies them");
    check(near(aver_fw_access_get("access.motion.shake"), 1.0f), "the rest take their defaults");
    check(drainKey(&v) == "(none)", "loading marks nothing dirty");

    // effective: reduce motion is a gate
    aver_fw_access_set("access.motion.shake", 0.4f);
    float e[8] = {};
    check(aver_fw_access_effective(e) == 1 && near(e[1], 1.5f) && near(e[4], 0.4f) && e[6] == 1.0f && e[7] == 1.0f, "effective: scale and shake come through, blur and lens allowed");
    aver_fw_access_set("access.motion.reduce", 1.0f);
    aver_fw_access_effective(e);
    check(e[4] == 0.0f && e[5] == 0.0f && e[6] == 0.0f && e[7] == 0.0f, "reduce motion zeroes shake, bob, blur and lens");
    aver_fw_access_set("access.motion.reduce", 0.0f);
    aver_fw_access_effective(e);
    check(near(e[4], 0.4f), "and un-ticking restores the slider");
    aver_fw_access_set("access.colour.mode", 3.0f);
    aver_fw_access_set("access.colour.strength", 0.5f);
    aver_fw_access_effective(e);
    check(e[0] == 3.0f + 8.0f * 50.0f, "the packed colour filter is in effective[0]");

    aver_fw_access_reset_defaults();
    check(near(aver_fw_access_get("graphics.uiScale"), 1.0f) && aver_fw_access_get("access.colour.mode") == 0.0f, "reset_defaults restores everything");
    while (aver_fw_access_take_dirty(0, nullptr, 0, &v)) {}
    aver_fw_events_clear();
    aver_fw_set_event_dispatch(nullptr, nullptr);
}

// ---- captions ------------------------------------------------------------------------------------------

static bool g_voiceAlive = true;
static int32_t aliveProbe(int32_t voice) { return voice == 11 && g_voiceAlive ? 1 : 0; }

static void writeFile(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

static int lineCount() { return aver_fw_caption_line_count(); }

static std::string lineText(int i) {
    AverFwCaptionLine l{};
    return aver_fw_caption_line_at(i, &l) ? std::string(l.text) : std::string("(none)");
}

static void testCaptions() {
    AVER_INFO("=== captions ===");
    aver_fw_access_load_ini("access.captions.mode=2\n");
    const fs::path dir = fs::temp_directory_path() / "aver_access_abi_test_captions";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    writeFile(dir / "a.occaptions",
              "OCCAPTIONS 1\n"
              "SPEAKER guard name \"Guard\" colour #7DD3FC\n"
              "CUE intro_01 sound \"Audio/VO/intro_01.ocaudio\" kind dialogue speaker guard priority 8\n"
              "  LINE 0.0 2.0 \"Welcome.\"\n"
              "  LINE 2.5 0 \"Keep quiet.\"\n"
              "CUE boom sound \"Audio/SFX/explosion_*.ocaudio\" kind sound tag \"[explosion]\"\n");
    writeFile(dir / "b.occaptions",
              "OCCAPTIONS 1\n"
              "CUE boom sound \"Audio/SFX/explosion_*.ocaudio\" kind sound tag \"[boom2]\"\n");
    writeFile(dir / "notes.txt", "not a captions file");

    check(aver_fw_caption_load_dir(dir.string().c_str()) == 2 && aver_fw_caption_cue_count() == 2, "two cues loaded; a later file's duplicate replaces the earlier");
    check(aver_fw_caption_load_dir((dir / "missing").string().c_str()) == -1 && aver_fw_caption_cue_count() == 0, "a missing folder is -1 and leaves the set empty");
    check(aver_fw_caption_load_dir(dir.string().c_str()) == 2, "reloading the folder works");

    check(aver_fw_caption_show("nope", 0, 0, 0, 0) == 0, "an unknown cue is refused");
    check(aver_fw_caption_show("intro_01", 0, 0, 0, 0) == 1, "a known cue shows");
    aver_fw_caption_update(0.1f, nullptr, nullptr, nullptr);
    AverFwCaptionLine l{};
    check(lineCount() == 1 && aver_fw_caption_line_at(0, &l) == 1 && std::string(l.text) == "Welcome." && std::string(l.speaker) == "Guard" && l.kind == 0 && l.rgba == 0x7DD3FCFFu && l.alpha == 1.0f,
          "the line comes back with its raw text, speaker, kind and colour");
    check(aver_fw_caption_new_line_count() == 1 && aver_fw_caption_new_line_at(0, &l) == 1, "and is new once");
    aver_fw_caption_update(0.1f, nullptr, nullptr, nullptr);
    check(aver_fw_caption_new_line_count() == 0, "not on the next update");
    check(aver_fw_caption_line_at(5, &l) == 0, "line_at out of range is 0");

    // The audio observer path with a fake voice probe.
    aver_fw_caption_set_voice_probe(&aliveProbe);
    g_voiceAlive = true;
    aver_fw_access_audio_observer(nullptr, "C:/proj/Content/Audio/SFX/explosion_03.ocaudio", 11, 0, 1, 100.0f, 0.0f, 0.0f, 0);
    aver_fw_caption_update(0.1f, nullptr, nullptr, nullptr);
    bool boom = false;
    for (int i = 0; i < lineCount(); ++i) if (lineText(i) == "[boom2]") boom = true;
    check(boom, "a played sound raises its cue through the observer");
    g_voiceAlive = false;
    aver_fw_caption_update(0.5f, nullptr, nullptr, nullptr);   // notices the voice ended
    aver_fw_caption_update(3.0f, nullptr, nullptr, nullptr);
    check(lineCount() == 0, "the voice ends, the caption goes (min hold and tail long past)");
    aver_fw_access_audio_observer(nullptr, nullptr, 12, 0, 0, 0, 0, 0, 0);   // generated PCM has no path: ignored
    aver_fw_caption_update(0.1f, nullptr, nullptr, nullptr);
    check(lineCount() == 0, "a sound with no path raises nothing");
    aver_fw_caption_set_voice_probe(nullptr);

    // The bus event, and re-subscription after the bus was cleared.
    aver_fw_event_push_string("intro_01");
    aver_fw_event_dispatch("Caption.Show", 0, 0);
    aver_fw_caption_update(0.1f, nullptr, nullptr, nullptr);
    check(lineCount() >= 1 && lineText(0) == "Welcome.", "the Caption.Show bus event shows a cue");
    aver_fw_caption_stop(nullptr);
    aver_fw_caption_update(1.5f, nullptr, nullptr, nullptr);
    check(lineCount() == 0, "stop(null) ends every caption");
    aver_fw_events_clear();                                  // a script reset drops every subscription
    aver_fw_caption_update(0.0f, nullptr, nullptr, nullptr);   // the next update re-subscribes
    aver_fw_event_push_string("intro_01");
    aver_fw_event_dispatch("Caption.Show", 0, 0);
    aver_fw_caption_update(0.1f, nullptr, nullptr, nullptr);
    check(lineCount() >= 1, "and the event works again after the bus was cleared");
    aver_fw_caption_stop(nullptr);

    // Ad-hoc text and the mode filter.
    aver_fw_access_set("access.captions.mode", 1.0f);
    check(aver_fw_caption_show_text("Narrator", "@Cap:x=Hello", 2.0f, 0xFFFFFFFFu, 1) == 0 && aver_fw_caption_show_text("Narrator", "Hello", 2.0f, 0xFFFFFFFFu, 0) == 1,
          "show_text obeys the mode: subtitles keep dialogue only");
    aver_fw_caption_update(0.1f, nullptr, nullptr, nullptr);
    check(lineCount() == 1 && lineText(0) == "Hello", "and the line carries the raw text");
    aver_fw_access_set("access.captions.mode", 0.0f);
    aver_fw_caption_update(0.1f, nullptr, nullptr, nullptr);
    check(lineCount() == 0, "turning captions off hides everything at once");
    aver_fw_access_set("access.captions.mode", 2.0f);
    aver_fw_caption_stop(nullptr);

    // Hot reload.
    writeFile(dir / "a.occaptions", "OCCAPTIONS 1\nCUE only sound \"x.ocaudio\" tag \"[only]\"\n");
    check(aver_fw_caption_reload_path((dir / "a.occaptions").string().c_str()) == 1 && aver_fw_caption_cue_count() == 2, "a changed caption file reloads the folder (only + boom)");
    check(aver_fw_caption_reload_path((dir / "notes.txt").string().c_str()) == 0 && aver_fw_caption_reload_path("C:/elsewhere/a.occaptions") == 0, "other files and other folders are not ours");
    aver_fw_caption_clear();
    check(aver_fw_caption_cue_count() == 0, "clear drops the set");
    fs::remove_all(dir, ec);
    aver_fw_events_clear();
}

static void testSpeech() {
    AVER_INFO("=== speech ===");
    aver_fw_access_load_ini("access.speech.enabled=0\naccess.speech.volume=0\n");
    check(aver_fw_speech_say("hello", 1) == 0 && aver_fw_speech_announce_ui("menu", 1) == 0, "disabled: nothing is said");
    const int32_t available = aver_fw_speech_available();
    aver_fw_access_set("access.speech.enabled", 1.0f);
    aver_fw_speech_tick();
    const int32_t said = aver_fw_speech_say("hello", 1);
    check(said == available, "enabled: said when a voice exists, silently refused when not (volume 0 either way)");
    aver_fw_access_set("access.speech.ui", 0.0f);
    check(aver_fw_speech_announce_ui("another menu", 1) == 0, "ui narration can be off on its own");
    aver_fw_speech_stop();
    aver_fw_speech_tick();
    check(aver_fw_speech_speaking() == 0 || available != 0, "stop is safe");
    aver_fw_access_set("access.speech.enabled", 0.0f);
    aver_fw_speech_tick();
    aver_fw_access_reset_defaults();
}

static void testCamera() {
    AVER_INFO("=== camera feel ===");
    aver_fw_access_load_ini("");
    float pos[3] = {100, 200, 300}, yp[2] = {0.5f, -0.1f};
    check(aver_fw_camera_fx_apply(0.016f, pos, yp) == 0 && pos[0] == 100 && yp[0] == 0.5f, "nothing active: the camera is untouched");
    aver_fw_camera_shake_add(1.0f);
    check(aver_fw_camera_fx_apply(0.016f, pos, yp) == 1 && (pos[0] != 100 || pos[1] != 200 || pos[2] != 300 || yp[0] != 0.5f), "a shake moves it");
    aver_fw_camera_shake_add(1.0f);
    aver_fw_access_set("access.motion.reduce", 1.0f);
    float p2[3] = {1, 2, 3}, y2[2] = {0.25f, 0.0f};
    check(aver_fw_camera_fx_apply(0.016f, p2, y2) == 0 && p2[0] == 1 && y2[0] == 0.25f, "reduce motion: untouched, even with trauma pending");
    aver_fw_access_set("access.motion.reduce", 0.0f);
    aver_fw_camera_bob_set(1.0f, 640.0f);
    float p3[3] = {0, 0, 0}, y3[2] = {0, 0};
    aver_fw_access_set("access.motion.shake", 0.0f);
    check(aver_fw_camera_fx_apply(0.125f, p3, y3) == 1 && near(p3[2], 2.2f, 1e-3f), "bob moves it up and down (and shake 0 leaves the angles alone)");
    check(y3[0] == 0.0f && y3[1] == 0.0f, "with the shake slider at 0 the angles do not move");
    aver_fw_camera_bob_set(0.0f, 0.0f);
    aver_fw_access_reset_defaults();
}

static void testInputAssist() {
    AVER_INFO("=== input assist in the action layer ===");
    aver_fw_access_load_ini("");
    const int32_t sprint = aver_fw_action_register("AccessSprint", AVER_FW_ACTION_DIGITAL);
    aver_fw_action_bind(sprint, AVER_FW_ACTION_SRC_KEY, AVER_FW_KEY_W, 1.0f, 0, 0);
    const auto frame = [&](int down) {
        aver_fw_input_new_frame();
        aver_fw_input_set_key(AVER_FW_KEY_W, down);
        aver_fw_action_update(0.016f);
    };
    aver_fw_access_set("access.input.toggle.AccessSprint", 1.0f);
    frame(1);
    check(aver_fw_action_held(sprint) == 1 && aver_fw_action_pressed(sprint) == 1, "toggle: a press latches the action on");
    frame(1);
    check(aver_fw_action_held(sprint) == 1 && aver_fw_action_pressed(sprint) == 0, "holding does not press again");
    frame(0);
    check(aver_fw_action_held(sprint) == 1 && aver_fw_action_released(sprint) == 0, "releasing the key keeps it held: held/pressed/released come from the cached values");
    frame(1);
    check(aver_fw_action_held(sprint) == 0 && aver_fw_action_released(sprint) == 1, "the next press releases it");
    frame(0);
    check(aver_fw_action_held(sprint) == 0, "and it stays off");

    aver_fw_access_set_toggle_allowed("AccessSprint", 0);
    check(aver_fw_access_toggle_allowed("AccessSprint") == 0, "an exempt action reports it");
    frame(1);
    frame(0);
    check(aver_fw_action_held(sprint) == 0, "exempt: the saved toggle is ignored and the key behaves as a key");
    aver_fw_access_set_toggle_allowed("AccessSprint", 1);
    aver_fw_access_set("access.input.toggle.AccessSprint", 0.0f);
    aver_fw_action_reset_state();

    // Hold scale: a 0.5 s hold fires after 0.3 s only with the scale.
    const int32_t interact = aver_fw_action_register("AccessInteract", AVER_FW_ACTION_DIGITAL);
    aver_fw_action_bind(interact, AVER_FW_ACTION_SRC_KEY, AVER_FW_KEY_E, 1.0f, 0, 0);
    check(aver_fw_action_add_trigger(interact, -1, "hold:0.5") == 1, "a hold trigger attaches");
    for (int pass = 0; pass < 2; ++pass) {
        aver_fw_action_reset_state();
        aver_fw_access_set("access.input.holdScale", pass ? 0.5f : 1.0f);
        for (int i = 0; i < 3; ++i) {
            aver_fw_input_new_frame();
            aver_fw_input_set_key(AVER_FW_KEY_E, 1);
            aver_fw_action_update(0.1f);
        }
        const bool fired = aver_fw_action_state(interact) == AVER_FW_TRIGGER_STATE_TRIGGERED;
        check(fired == (pass == 1), pass ? "holdScale 0.5: the hold fires at 0.25 s" : "unassisted: 0.3 s is not enough for a 0.5 s hold");
    }
    aver_fw_access_reset_defaults();
    aver_fw_action_clear_bindings();
    aver_fw_action_reset_state();
}

// Every play function in AudioAbi.cpp must notify the observer (the caption path would silently miss one otherwise).
static std::string functionBody(const std::string& src, const std::string& signature) {
    const size_t at = src.find(signature);
    if (at == std::string::npos) return {};
    const size_t end = src.find("\n}\n", at);
    return end == std::string::npos ? src.substr(at) : src.substr(at, end - at);
}

static void testAudioObserverSites() {
    AVER_INFO("=== the audio ABI notifies the observer from every play function ===");
    const fs::path src = fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path() / "modules" / "audio.abi" / "src" / "AudioAbi.cpp";
    std::ifstream f(src, std::ios::binary);
    if (!f) { AVER_INFO("   (AudioAbi.cpp not found beside this source; skipped)"); return; }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());   // the working copy may be CRLF
    const char* sigs[] = {"int32_t aver_audio_play(", "int32_t aver_audio_play_at(", "int32_t aver_audio_stream_play(",
                          "int32_t aver_audio_stream_play_at(", "int32_t aver_audio_music_play("};
    for (const char* s : sigs)
        check(functionBody(text, s).find("notifyPlay(") != std::string::npos, std::string(s) + " calls notifyPlay");
    check(text.find("aver_audio_set_play_observer") != std::string::npos, "the setter exists");
}

int main() {
    AVER_INFO("FrameworkAccessAbiTest");
    testOptions();
    testCaptions();
    testSpeech();
    testCamera();
    testInputAssist();
    testAudioObserverSites();
    AVER_INFO(g_failures == 0 ? "FrameworkAccessAbiTest: PASS" : "FrameworkAccessAbiTest: FAIL");
    return g_failures == 0 ? 0 : 1;
}
