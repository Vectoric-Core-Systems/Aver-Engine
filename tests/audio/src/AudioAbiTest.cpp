// The bus graph, effects and spatial-model players through the plain-C seam (modules/audio.abi), in OFFLINE mode: the real mixer
// with no device behind it, rendered into buffers here. Also the .ocbus text format (parse, write, the layout applied through the ABI).
#include "aver/audio/audio_abi.h"
#include "aver/audio/BusLayout.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcAudio.hpp"
#include "aver/formats/OcBusLayout.hpp"
#include "aver/formats/OcSound.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
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
    check(back.buses[1].effects.size() == 2 && back.buses[1].effects[0].params.size() == 2 && back.buses[1].effects[0].params[1].first == "voice/1/delay_ms", "effects keep their order and parameter names");
    check(!back.buses[1].effects[1].enabled && back.buses[1].effects[1].sidechain == "Master" && near(back.buses[1].effects[1].params[0].second, -9.5f, 1.0e-6f), "enabled, sidechain and values survive");
    check(fmt::writeOcBusLayout(back) == text, "write(parse(write(x))) is stable");
    audio::BusLayout bad;
    check(!fmt::parseOcBusLayout("BUS \"x\"\n", bad, &why) && why.find("line 1") == 0, "a file without the header is refused with a line number");
    check(!fmt::parseOcBusLayout("OCBUS 1\nEFFECT \"AudioEffectReverb\"\n", bad, &why), "an EFFECT before any BUS is refused");
    check(!fmt::parseOcBusLayout("OCBUS 1\nBUS \"a\" volume_db=loud\n", bad, &why), "a bad number is refused");
    check(fmt::parseOcBusLayout("OCBUS 1\r\n# comment\r\nBUS \"Master\" volume_db=1\r\n", bad, &why) && bad.buses.size() == 1, "CRLF line endings and comments are accepted");
}

// Real-time sound graphs through the C seam (docs/AUDIO.md section 13), in offline mode: a .ocsnd on disk, an .ocaudio for its WavePlayer.
static void testSoundGraphs() {
    AVER_INFO("=== sound graphs through the ABI ===");
    namespace fs = std::filesystem;
    using K = fmt::OcSoundNodeKind;
    const fs::path dir = fs::temp_directory_path() / "aver_soundgraph_abi_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    audio::SoundData clip;
    clip.channels = 1; clip.sampleRate = 48000;
    clip.samples.assign(4800, 0.25f);
    const std::string clipPath = (dir / "clip.ocaudio").string();
    std::string why;
    check(fmt::saveOcAudio(clipPath, clip) , "the test asset is written");

    auto node = [](K k, float p0 = 0, float p1 = 0, float p2 = 0, float p3 = 0, std::string name = {}) {
        fmt::OcSoundNode n; n.kind = k; n.params[0] = p0; n.params[1] = p1; n.params[2] = p2; n.params[3] = p3; n.name = std::move(name); return n;
    };
    fmt::OcSoundData g;
    g.nodes = {node(K::Sine, 437), node(K::Parameter, 0, 0, 0, 0, "level"), node(K::Multiply), node(K::WavePlayer, 1, 1, 0, 0, clipPath), node(K::Mix)};
    g.links = {{0, 2, 0}, {1, 2, 1}, {2, 4, 0}, {3, 4, 1}};
    g.outputNode = 4;
    g.durationSec = 1.0f;
    const std::string path = (dir / "engine.ocsnd").string();
    check(fmt::saveOcSound(path, g, &why), "the graph is written: " + why);

    const int gh = aver_audio_soundgraph_load(path.c_str());
    check(gh > 0 && aver_audio_soundgraph_load(path.c_str()) == gh, "a graph loads, and the same path is the same handle");
    check(aver_audio_soundgraph_load("") == 0 && aver_audio_soundgraph_load((dir / "missing.ocsnd").string().c_str()) == 0, "an empty or missing path loads nothing");
    {
        std::ofstream junk(dir / "junk.ocsnd", std::ios::binary);
        junk << "this is not a sound graph";
    }
    check(aver_audio_soundgraph_load((dir / "junk.ocsnd").string().c_str()) == 0, "and so does a file that is not one");
    uint8_t nb[32];
    check(aver_audio_soundgraph_param_count(gh) == 1 && aver_audio_soundgraph_param_name(gh, 0, nb, 32) == 5 && std::string(reinterpret_cast<const char*>(nb)) == "level" &&
          aver_audio_soundgraph_param_find(gh, "level") == 0 && aver_audio_soundgraph_param_find(gh, "x") == -1 && near(aver_audio_soundgraph_param_default(gh, 0), 0.0f) &&
          near(static_cast<float>(aver_audio_soundgraph_duration(gh)), 1.0f, 1.0e-6f),
          "its inputs and duration can be listed");
    check(aver_audio_soundgraph_param_count(9999) == 0 && aver_audio_soundgraph_param_find(9999, "level") == -1 && aver_audio_soundgraph_duration(9999) == 0.0,
          "an unknown graph handle answers empty");

    std::vector<float> out(480 * 2);
    int v = aver_audio_soundgraph_play(gh, 1.0f, 1, AVER_AUDIO_BUS_SFX, 1, nullptr, nullptr, 0);
    check(v != 0 && aver_audio_soundgraph_is_voice(v) == 1 && aver_audio_playing(v) == 1, "a graph plays as a voice");
    renderBlocks(out, 3);
    check(near(out[400], 0.25f, 1.0e-4f), "at its default input (0) only the WavePlayer's 0.25 is left");
    check(aver_audio_soundgraph_set_param(v, "level", 0.5f) == 1 && aver_audio_soundgraph_set_param_id(v, 0, 0.5f) == 1 &&
          aver_audio_soundgraph_set_param(v, "nope", 1.0f) == 0 && aver_audio_soundgraph_set_param_id(v, 7, 1.0f) == 0, "inputs are set by name and by id; unknown ones are refused");
    renderBlocks(out, 20);
    float top = -2, bottom = 2;
    for (int i = 0; i < 480; ++i) { top = std::max(top, out[i * 2]); bottom = std::min(bottom, out[i * 2]); }
    check(near(top, 0.75f, 0.03f) && near(bottom, -0.25f, 0.03f), "the live input scales the sine: 0.25 +- 0.5");
    check(aver_audio_soundgraph_set_param(v + 1000, "level", 1.0f) == 0 && aver_audio_soundgraph_is_voice(v + 1000) == 0 && aver_audio_soundgraph_is_voice(0) == 0, "a stale or null voice is refused");
    aver_audio_soundgraph_note_off(v);
    renderBlocks(out, 3);
    check(aver_audio_playing(v) == 0, "the note-off ends a held graph with no ADSR within a few ms");

    // Started with its inputs set, a finite play lasts the graph's duration, and a positional one pans.
    const char* names[] = {"level", "ignored"};
    const float values[] = {0.5f, 3.0f};
    v = aver_audio_soundgraph_play(gh, 1.0f, 0, AVER_AUDIO_BUS_SFX, 0, names, values, 2);
    renderBlocks(out, 1);
    float first = 0;
    for (int i = 0; i < 480; ++i) first = std::max(first, out[i * 2]);
    check(v != 0 && first > 0.7f, "inputs passed at play time are there from the first block (peak " + std::to_string(first) + ")");
    renderBlocks(out, 90);
    check(aver_audio_playing(v) == 1, "a finite graph is still playing at 0.9 s");
    renderBlocks(out, 20);
    check(aver_audio_playing(v) == 0, "and has ended by 1.1 s");

    const int vp = aver_audio_soundgraph_play_at(gh, 0.0f, 500.0f, 0.0f, 1.0f, 1, AVER_AUDIO_BUS_SFX, 2000.0f, 4000.0f, 1, nullptr, nullptr, 0);
    aver_audio_set_listener(0, 0, 0, 1, 0, 0, 0, 1, 0);
    renderBlocks(out, 2);
    float l = 0, r = 0;
    for (int i = 0; i < 480; ++i) { l = std::max(l, std::fabs(out[i * 2])); r = std::max(r, std::fabs(out[i * 2 + 1])); }
    check(vp != 0 && r > 2.0f * l, "a positional graph pans with the listener (L " + std::to_string(l) + ", R " + std::to_string(r) + ")");
    aver_audio_stop(vp);
    renderBlocks(out, 3);

    aver_audio_soundgraph_unload(gh);
    check(aver_audio_soundgraph_play(gh, 1.0f, 0, AVER_AUDIO_BUS_SFX, 0, nullptr, nullptr, 0) == 0 && aver_audio_soundgraph_param_count(gh) == 0, "an unloaded graph cannot be played");
    check(aver_audio_soundgraph_load(path.c_str()) > 0, "but it loads again");
    aver_audio_stop_all();
    renderBlocks(out, 3);
    fs::remove_all(dir, ec);
}

// Surround output, LFE and binaural through the C seam (docs/AUDIO.md section 14). Offline: a 5.1 mixer made with aver_audio_offline_init(48000, 6).
static void testSurround() {
    check(aver_audio_speakers() == 2 && aver_audio_binaural() == 0 && aver_audio_lfe_send() == 0.0f && aver_audio_stereo_upmix() == AVER_AUDIO_UPMIX_WRAP,
          "the defaults: stereo requested, binaural off, no LFE send, Wrap upmix");
    check(aver_audio_set_speakers(6) == 6 && aver_audio_speakers() == 6 && aver_audio_set_speakers(5) == 2 && aver_audio_speakers() == 2 &&
              aver_audio_set_speakers(8) == 8 && aver_audio_set_speakers(0) == 2,
          "set_speakers keeps 6 and 8 and turns anything else into the stereo default");
    aver_audio_set_binaural(1); aver_audio_set_lfe_send(0.5f); aver_audio_set_stereo_upmix(AVER_AUDIO_UPMIX_FRONT);
    check(aver_audio_binaural() == 1 && aver_audio_lfe_send() == 0.5f && aver_audio_stereo_upmix() == AVER_AUDIO_UPMIX_FRONT, "the other settings read back");
    aver_audio_set_binaural(0); aver_audio_set_lfe_send(2.0f);
    check(aver_audio_binaural() == 0 && aver_audio_lfe_send() == 1.0f, "binaural off again; the LFE send is clamped to 1");
    aver_audio_set_lfe_send(0.0f); aver_audio_set_stereo_upmix(AVER_AUDIO_UPMIX_WRAP);

    check(aver_audio_offline_init(48000, 3) == 0 && aver_audio_offline_init(48000, 4) == 0, "an offline mixer refuses 3 and 4 channels");
    check(aver_audio_offline_init(48000, 6) == 1 && aver_audio_channels() == 6, "an offline mixer takes 6 channels");
    std::vector<float> pcm(48000, 0.5f);
    const int snd = aver_audio_load_pcm(pcm.data(), 48000, 1, 48000);
    aver_audio_set_listener(0, 0, 0, 1, 0, 0, 0, 1, 0);
    const int v = aver_audio_play_at(snd, 100.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1, AVER_AUDIO_BUS_SFX, 100.0f, 2000.0f);
    std::vector<float> out(480 * 6 * 4);
    check(v != 0 && aver_audio_offline_mix(out.data(), 480 * 4) == 480 * 4, "a positional voice plays on the 6-channel mixer");
    const float* last = out.data() + static_cast<size_t>(480 * 4 - 1) * 6;
    check(last[2] > 0.6f && near(last[0], 0.0f, 1.0e-3f) && near(last[1], 0.0f, 1.0e-3f) && near(last[3], 0.0f, 1.0e-6f), "straight ahead lands on the centre channel, the LFE silent");
    aver_audio_set_lfe_send(0.5f);
    aver_audio_offline_mix(out.data(), 480 * 4);
    aver_audio_offline_mix(out.data(), 480 * 4);
    check(std::fabs(out[(480 * 4 - 1) * 6 + 3]) > 0.05f, "the LFE send reaches the LFE channel (a constant is below the 120 Hz corner)");
    aver_audio_set_lfe_send(0.0f);
    aver_audio_shutdown();
}

// Environment audio through the C seam (docs/AUDIO.md section 15): a zone send reverb convolving with a loaded sound, and the convolution reverb effect.
static void testEnvironment() {
    AVER_INFO("=== zone reverbs and the convolution effect through the ABI ===");
    check(aver_audio_offline_init(48000, 2) == 1, "an offline stereo mixer");
    std::vector<float> irPcm(1000, 0.0f);
    irPcm[300] = 0.5f;
    const int ir = aver_audio_load_pcm(irPcm.data(), 1000, 1, 48000);
    std::vector<float> blip(16, 0.0f);
    blip[0] = 1.0f;
    const int snd = aver_audio_load_pcm(blip.data(), 16, 1, 48000);
    check(ir > 0 && snd > 0, "an impulse response and a blip load");

    check(aver_audio_set_zone_reverb(0, 1.0f, 1.5f, 0.4f, ir, 0.0f) == 1, "a zone reverb takes a loaded sound as its impulse");
    check(aver_audio_set_zone_reverb(AVER_AUDIO_ZONE_REVERBS, 1.0f, 1.5f, 0.4f, 0, 0.0f) == 0 && aver_audio_set_zone_reverb(-1, 1.0f, 1.5f, 0.4f, 0, 0.0f) == 0,
          "a slot out of range is refused");
    check(aver_audio_set_zone_reverb(1, 1.0f, 1.5f, 0.4f, 9999, 0.0f) == 0 && aver_audio_set_zone_reverb(1, 1.0f, 1.5f, 0.4f, -3, 0.0f) == 0,
          "an unknown or negative impulse sound is refused");

    const int v = aver_audio_play(snd, 1.0f, 1.0f, 0, AVER_AUDIO_BUS_SFX);
    aver_audio_set_voice_zone_send(v, 0, 1.0f);
    aver_audio_set_voice_zone_send(v, 7, 1.0f);            // out of range: ignored
    std::vector<float> out(480 * 2 * 4);
    // The slot's return ramps up over the first call, so the first 480 frames are one call and the impulse arrives in the next.
    check(aver_audio_offline_mix(out.data(), 480) == 480 && aver_audio_offline_mix(out.data() + 480 * 2, 480 * 3) == 480 * 3, "the mixer renders");
    size_t at = 0;
    for (size_t n = 1; n < 480 * 4; ++n) if (std::fabs(out[2 * n]) > 0.05f) { at = n; break; }
    check(at == 300 + 256 && near(out[2 * at], 0.5f, 1.0e-3f) && near(out[2 * at + 1], 0.5f, 1.0e-3f), "the blip is heard again through the impulse, 300 + 256 frames later, in both ears");

    aver_audio_graph_set_active(1);
    const int fx = aver_audio_effect_add(0, "AudioEffectConvolutionReverb", -1);
    uint8_t cls[64];
    check(fx > 0 && aver_audio_effect_class(fx, cls, 64) > 0 && std::string(reinterpret_cast<const char*>(cls)) == "AudioEffectConvolutionReverb", "the convolution reverb is a bus effect by its class name");
    check(aver_audio_effect_set_impulse(fx, ir) == 1, "it takes a loaded sound as its impulse");
    check(aver_audio_effect_set_impulse(fx, 0) == 0 && aver_audio_effect_set_impulse(fx, 9999) == 0 && aver_audio_effect_set_impulse(9999, ir) == 0 && aver_audio_effect_set_impulse(0, ir) == 0,
          "no sound, an unknown sound and an unknown effect are refused");
    const int plain = aver_audio_effect_add(0, "AudioEffectReverb", -1);
    check(plain > 0 && aver_audio_effect_set_impulse(plain, ir) == 0, "an effect that takes no impulse says so");
    check(aver_audio_effect_set_param(fx, "wet", 1.0f) == 1 && aver_audio_effect_set_param(fx, "predelay_msec", 20.0f) == 1 &&
              near(aver_audio_effect_get_param(fx, "predelay_msec"), 20.0f), "its parameters are reachable by name");
    aver_audio_unload(ir);
    aver_audio_offline_mix(out.data(), 480 * 2);
    check(aver_audio_effect_set_impulse(fx, snd) == 1, "and unloading the impulse sound later does not disturb the mixer");
    aver_audio_shutdown();
}

int main() {
    testFormat();

    AVER_INFO("=== offline mixer through the ABI ===");
    check(aver_audio_offline_init(48000, 2) == 1 && aver_audio_ready() && aver_audio_is_offline(), "offline init gives a ready mixer with no device");
    check(aver_audio_sample_rate() == 48000 && aver_audio_channels() == 2, "at the requested rate and channels");
    {
        uint8_t devName[8] = {'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'};
        check(aver_audio_device_state() == AVER_AUDIO_DEVICE_NONE && aver_audio_device_channels() == 0 &&
              aver_audio_device_buffer_frames() == 0 && aver_audio_device_changes() == 0 && aver_audio_device_latency_ms() == 0.0f,
              "offline there is no device: state NONE and every device figure zero");
        check(aver_audio_device_name(devName, 8) == 0 && devName[0] == 0 && aver_audio_underruns() == 0, "and an empty device name, written terminated");
    }

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
    check(amp > 0 && aver_audio_effect_add(sfx, "AudioEffectNope", -1) == 0, "an effect is added by class name; an unknown one is refused");
    check(aver_audio_effect_set_param(amp, "volume_db", 6.0206f) == 1 && aver_audio_effect_set_param(amp, "bogus", 1.0f) == 0, "parameters are set by name");
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

    testSoundGraphs();

    aver_audio_shutdown();
    check(!aver_audio_ready() && !aver_audio_is_offline(), "shutdown ends offline mode");

    testSurround();
    testEnvironment();
    if (g_failures) AVER_ERROR("AudioAbiTest: {} check(s) FAILED", g_failures);
    else AVER_INFO("AudioAbiTest: all checks passed");
    return g_failures ? 1 : 0;
}
