// The bus graph, effects and Godot-style players through the plain-C seam Aver.Fabric calls (modules/audio.abi), in OFFLINE mode: the real mixer
// with no device behind it, rendered into buffers here. Also the .ocbus text format (parse, write, the layout applied through the ABI).
#include "aver/audio/audio_abi.h"
#include "aver/audio/BusLayout.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcBusLayout.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}
static bool near(float a, float b, float eps = 1.0e-3f) { return std::fabs(a - b) <= eps; }

static void renderBlocks(std::vector<float>& out, int blocks, int frames = 480) {
    for (int i = 0; i < blocks; ++i) aver_audio_offline_mix(out.data(), frames);
}

static void testFormat() {
    AVER_INFO("=== .ocbus ===");
    audio::BusLayout lay;
    lay.buses.resize(2);
    lay.buses[0].name = "Master"; lay.buses[0].send = ""; lay.buses[0].volumeDb = -2.5f;
    lay.buses[1].name = "Sound Effects"; lay.buses[1].mute = true;
    lay.buses[1].effects.push_back({"AudioEffectDelay", true, "", {{"tap1_delay_ms", 125.0f}, {"voice/1/delay_ms", 3.0f}}});
    lay.buses[1].effects.push_back({"AudioEffectCompressor", false, "Master", {{"threshold", -9.5f}}});
    const std::string text = fmt::writeOcBusLayout(lay);
    audio::BusLayout back;
    std::string why;
    check(fmt::parseOcBusLayout(text, back, &why) && back.buses.size() == 2, "a written layout parses (" + why + ")");
    check(back.buses[0].name == "Master" && near(back.buses[0].volumeDb, -2.5f, 1.0e-6f), "volumes survive");
    check(back.buses[1].name == "Sound Effects" && back.buses[1].mute && back.buses[1].send == "Master", "names with spaces, mute and send survive");
    check(back.buses[1].effects.size() == 2 && back.buses[1].effects[0].params.size() == 2 && back.buses[1].effects[0].params[1].first == "voice/1/delay_ms", "effects keep their order and Godot's parameter names");
    check(!back.buses[1].effects[1].enabled && back.buses[1].effects[1].sidechain == "Master" && near(back.buses[1].effects[1].params[0].second, -9.5f, 1.0e-6f), "enabled, sidechain and values survive");
    check(fmt::writeOcBusLayout(back) == text, "write(parse(write(x))) is stable");
    audio::BusLayout bad;
    check(!fmt::parseOcBusLayout("BUS \"x\"\n", bad, &why) && why.find("line 1") == 0, "a file without the header is refused with a line number");
    check(!fmt::parseOcBusLayout("OCBUS 1\nEFFECT \"AudioEffectReverb\"\n", bad, &why), "an EFFECT before any BUS is refused");
    check(!fmt::parseOcBusLayout("OCBUS 1\nBUS \"a\" volume_db=loud\n", bad, &why), "a bad number is refused");
    check(fmt::parseOcBusLayout("OCBUS 1\r\n# comment\r\nBUS \"Master\" volume_db=1\r\n", bad, &why) && bad.buses.size() == 1, "CRLF line endings and comments are accepted");
}

int main() {
    testFormat();

    AVER_INFO("=== offline mixer through the ABI ===");
    check(aver_audio_offline_init(48000, 2) == 1 && aver_audio_ready() && aver_audio_is_offline(), "offline init gives a ready mixer with no device");
    check(aver_audio_sample_rate() == 48000 && aver_audio_channels() == 2, "at the requested rate and channels");

    std::vector<float> pcm(48000, 0.5f);
    const int snd = aver_audio_load_pcm(pcm.data(), 48000, 1, 48000);
    check(snd > 0 && near(static_cast<float>(aver_audio_sound_length(snd)), 1.0f, 1.0e-4f), "load_pcm works offline and reports its length");

    aver_audio_graph_set_active(1);
    check(aver_audio_graph_active() == 1 && aver_audio_bus_count() == 1, "the graph starts as Master");
    uint8_t nameBuf[64];
    const char* name = reinterpret_cast<const char*>(nameBuf);
    check(aver_audio_bus_name(0, nameBuf, 64) == 6 && std::string(name) == "Master", "bus 0 is named Master");
    const int sfx = aver_audio_bus_add(-1);
    aver_audio_bus_set_name(sfx, "SFX");
    check(sfx == 1 && aver_audio_bus_find("SFX") == 1 && aver_audio_bus_find("none") == -1, "buses are added, named and found");
    check(aver_audio_bus_name(sfx, nameBuf, 3) == 3 && std::string(name) == "SF", "a name is truncated to the caller's buffer, terminated, and the full length returned");
    aver_audio_bus_set_send(sfx, "Master");

    std::vector<float> out(480 * 2);
    const int v = aver_audio_play_ex(snd, 1.0f, 1.0f, 1, 0, sfx, 0.0f, 0, 0, 0, 0, nullptr);
    renderBlocks(out, 3);
    check(v != 0 && near(out[200], 0.5f), "play_ex on a graph bus reaches the output");
    aver_audio_bus_set_volume_db(sfx, -6.0206f);
    renderBlocks(out, 3);
    check(near(out[200], 0.25f), "bus volume");
    check(near(aver_audio_bus_peak_db(sfx, 0), -12.0412f, 0.05f), "peak in dB after the gain (0.25)");
    aver_audio_bus_set_flag(sfx, AVER_AUDIO_BUSFLAG_MUTE, 1);
    renderBlocks(out, 3);
    check(near(out[200], 0.0f, 1.0e-4f) && aver_audio_bus_get_flag(sfx, AVER_AUDIO_BUSFLAG_MUTE) == 1, "mute");
    aver_audio_bus_set_flag(sfx, AVER_AUDIO_BUSFLAG_MUTE, 0);
    aver_audio_bus_set_volume_db(sfx, 0.0f);

    const int amp = aver_audio_effect_add(sfx, "AudioEffectAmplify", -1);
    check(amp > 0 && aver_audio_effect_add(sfx, "AudioEffectNope", -1) == 0, "an effect is added by Godot class name; an unknown one is refused");
    check(aver_audio_effect_set_param(amp, "volume_db", 6.0206f) == 1 && aver_audio_effect_set_param(amp, "bogus", 1.0f) == 0, "parameters are set by Godot name");
    renderBlocks(out, 3);
    check(near(out[200], 1.0f, 2.0e-3f), "the effect runs on the bus");
    int b = -1, e = -1;
    check(aver_audio_effect_find(amp, &b, &e) == 1 && b == sfx && e == 0, "an effect id finds its bus and index");
    uint8_t pn[64];
    float def = -1, lo = -1, hi = -1;
    check(aver_audio_effect_param_info(amp, 0, pn, 64, &def, &lo, &hi) == 9 && std::string(reinterpret_cast<const char*>(pn)) == "volume_db" && near(lo, -80.0f) && near(hi, 24.0f), "parameter info: name, default and range");
    aver_audio_effect_set_enabled(sfx, 0, 0);
    renderBlocks(out, 3);
    check(near(out[200], 0.5f, 2.0e-3f), "a disabled effect is skipped");
    aver_audio_effect_remove(sfx, 0);

    // The layout as text, applied through the ABI.
    const char* layoutText =
        "OCBUS 1\nBUS \"Master\" volume_db=0\nBUS \"Music\" send=\"Master\" volume_db=-3\nEFFECT \"AudioEffectLowPassFilter\" enabled=1 cutoff_hz=800\n"
        "EFFECT \"AudioEffectUnknown\" enabled=1\n";
    check(aver_audio_bus_apply_layout_text(layoutText) == 1, "a layout applies and counts the effects it could not make");
    check(aver_audio_bus_count() == 2 && aver_audio_bus_find("Music") == 1 && near(aver_audio_bus_get_volume_db(1), -3.0f), "buses come from the layout");
    const int lp = aver_audio_effect_id(1, 0);
    check(lp > 0 && near(aver_audio_effect_get_param(lp, "cutoff_hz"), 800.0f), "effect parameters come from the layout");
    std::vector<uint8_t> txt(4096);
    const int len = aver_audio_bus_layout_text(txt.data(), 4096);
    check(len > 20 && std::string(reinterpret_cast<const char*>(txt.data())).find("AudioEffectLowPassFilter") != std::string::npos, "the layout can be read back as text");

    // Capture through the ABI.
    aver_audio_stop_all();
    renderBlocks(out, 4);
    aver_audio_bus_set_count(1);
    aver_audio_bus_add(-1);
    aver_audio_bus_set_name(1, "Rec");
    const int cap = aver_audio_effect_add(1, "AudioEffectCapture", -1);
    const int v2 = aver_audio_play_ex(snd, 1.0f, 1.0f, 1, 0, 1, 0.0f, 0, 0, 0, 0, nullptr);
    renderBlocks(out, 2);
    check(v2 != 0 && aver_audio_capture_available(cap) > 0, "Capture queues what plays through its bus");
    std::vector<float> got(960);
    const int n = aver_audio_capture_read(cap, got.data(), 480);
    check(n == 480 && near(got[2 * 400], 0.5f, 5.0e-3f), "and hands it back");

    // Live voice.
    const int live = aver_audio_live_play(1.0f, 1.0f, 0, 0, 0, 0, 0, nullptr, 48000, 2, 4800);
    std::vector<float> push(960, 0.2f);
    check(live != 0 && aver_audio_live_push(live, push.data(), 480) == 480 && aver_audio_live_free(live) == 4800 - 480, "a live voice takes pushed frames");
    aver_audio_voice_underruns(live);
    aver_audio_stop_all();
    renderBlocks(out, 8);

    aver_audio_shutdown();
    check(!aver_audio_ready() && !aver_audio_is_offline(), "shutdown ends offline mode");
    if (g_failures) AVER_ERROR("AudioAbiTest: {} check(s) FAILED", g_failures);
    else AVER_INFO("AudioAbiTest: all checks passed");
    return g_failures ? 1 : 0;
}
