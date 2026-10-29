// FabricAudioTest -- G5 S1 through the Godot converter: `AverAssetC fabric` on tests/fabric/data/AudioGame (the sound files are written here: the fixture holds text only),
// then the files it wrote read back with the engine's own loaders. The sounds must be copied under Content/Audio/, a stream resource must become an Object(...) value with
// Godot's property names, the audio nodes must be placed as their classes, the default AudioBusLayout must become an .ocbus that the project names with AUDIO.BUSLAYOUT, and
// every item (the Ogg and MP3 files, the effects, the VideoStreamPlayer) must be rated in the report with its reason.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcBusLayout.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

using namespace aver;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }

static int runCapture(const std::string& cmd, std::string& out) {
    out.clear();
#if defined(_WIN32)
    const std::string wrapped = "\"" + cmd + " 2>&1\"";
    FILE* pipe = _popen(wrapped.c_str(), "r");
#else
    const std::string wrapped = cmd + " 2>&1";
    FILE* pipe = popen(wrapped.c_str(), "r");
#endif
    if (!pipe) return -1;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, pipe)) out += buf;
#if defined(_WIN32)
    return _pclose(pipe);
#else
    const int status = pclose(pipe);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

static std::string slurp(const std::string& path) {
    std::string s;
    readFileText(path, s);
    return s;
}

static std::vector<u8> wav16(float value, double seconds, u32 rate = 48000) {
    const u32 frames = static_cast<u32>(seconds * rate);
    std::vector<u8> b;
    auto u32le = [&](u32 v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>((v >> (8 * i)) & 0xFF)); };
    auto u16le = [&](u16 v) { b.push_back(static_cast<u8>(v & 0xFF)); b.push_back(static_cast<u8>(v >> 8)); };
    auto tag = [&](const char* t) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(t[i])); };
    tag("RIFF"); u32le(36 + frames * 2); tag("WAVE"); tag("fmt "); u32le(16); u16le(1); u16le(1); u32le(rate); u32le(rate * 2); u16le(2); u16le(16);
    tag("data"); u32le(frames * 2);
    const i16 s = static_cast<i16>(std::lround(value * 32768.0f));
    for (u32 i = 0; i < frames; ++i) u16le(static_cast<u16>(s));
    return b;
}

static bool put(const std::string& path, const std::vector<u8>& bytes) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    return writeFileBytes(path, bytes.data(), bytes.size());
}

static int find(const fmt::OcWorldData& w, std::initializer_list<const char*> path) {
    int parent = -1;
    for (const char* name : path) {
        int found = -1;
        for (usize i = 0; i < w.placements.size(); ++i)
            if (w.placements[i].parent == parent && w.placements[i].name == name) { found = static_cast<int>(i); break; }
        if (found < 0) return -1;
        parent = found;
    }
    return parent;
}

static const std::string* propOf(const fmt::OcWorldData& w, int idx, const char* key) {
    if (idx < 0) return nullptr;
    for (const fmt::OcPlacedComponent& c : w.placements[static_cast<usize>(idx)].components)
        if (c.type == "Props") return c.find(key);
    return nullptr;
}

static std::string classOf(const fmt::OcWorldData& w, int idx) {
    return idx < 0 ? std::string() : w.placements[static_cast<usize>(idx)].className;
}

int main() {
    AVER_INFO("FabricAudioTest");
    const std::filesystem::path exe = AVER_ASSETC_EXE;
    const std::string fixture = std::string(AVER_FABRIC_DATA) + "/AudioGame";
    std::error_code ec;
    check(std::filesystem::exists(exe, ec), "AverAssetC was built: " + exe.string());
    check(std::filesystem::exists(fixture + "/project.godot", ec), "the Godot fixture exists: " + fixture);
    if (g_failures) return exitCode(ExitCode::Failed);

    const std::filesystem::path outRoot = std::filesystem::temp_directory_path() / "aver-fabric-audio-test";
    std::filesystem::remove_all(outRoot, ec);
    std::filesystem::create_directories(outRoot, ec);
    // The fixture is text; the sound files are made beside a copy of it.
    const std::string src = (outRoot / "AudioGame").string();
    std::filesystem::copy(fixture, src, std::filesystem::copy_options::recursive, ec);
    check(!ec, "the fixture was copied");
    check(put(src + "/sfx/loop.wav", wav16(0.3f, 0.5)) && put(src + "/sfx/step_a.wav", wav16(0.2f, 0.1)) && put(src + "/sfx/step_b.wav", wav16(0.4f, 0.1)), "the WAV files were written");
    check(put(src + "/sfx/song.ogg", std::vector<u8>(16, 0x4F)) && put(src + "/sfx/jingle.mp3", std::vector<u8>(16, 0x49)), "stand-in Ogg and MP3 files were written");
    const std::string out1 = (outRoot / "out").string();

    std::string log;
    const int code = runCapture("\"" + exe.string() + "\" fabric \"" + src + "\" \"" + out1 + "\" --scenes-only", log);
    check(code == 0, "the converter exits 0 (got " + std::to_string(code) + ")");
    if (code != 0) AVER_INFO("{}", log);

    const std::string project = out1 + "/AudioGame";
    const std::string content = project + "/Content";

    // ---- the sound files -------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- files --");
    std::vector<u8> orig, copy;
    readFileBytes(src + "/sfx/step_a.wav", orig);
    readFileBytes(content + "/Audio/sfx/step_a.wav", copy);
    check(!orig.empty() && orig == copy, "a WAV is copied byte for byte to Content/Audio/sfx/step_a.wav");
    check(std::filesystem::exists(content + "/Audio/sfx/song.ogg", ec) && std::filesystem::exists(content + "/Audio/sfx/jingle.mp3", ec), "the Ogg and MP3 files are copied too");

    // ---- the bus layout -------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- bus layout --");
    audio::BusLayout lay;
    std::string err;
    check(fmt::loadOcBusLayout(content + "/Audio/default_bus_layout.ocbus", lay, &err), "default_bus_layout.ocbus loads: " + err);
    check(lay.buses.size() == 3 && lay.buses[0].name == "Master" && lay.buses[1].name == "Music" && lay.buses[2].name == "SFX", "Master, Music, SFX");
    if (lay.buses.size() == 3) {
        check(near(lay.buses[1].volumeDb, -6.0) && lay.buses[1].send == "Master" && !lay.buses[1].mute, "Music: -6 dB, sends to Master");
        check(lay.buses[2].mute && !lay.buses[2].solo && !lay.buses[2].bypass, "SFX is muted");
        check(lay.buses[1].effects.size() == 1 && lay.buses[1].effects[0].className == "AudioEffectReverb" && lay.buses[1].effects[0].params.size() == 2, "Music has its reverb with two parameters");
        bool room = false, wet = false;
        for (const auto& p : lay.buses[1].effects[0].params) { room |= p.first == "room_size" && near(p.second, 0.6); wet |= p.first == "wet" && near(p.second, 0.3); }
        check(room && wet, "room_size 0.6 and wet 0.3");
        check(lay.buses[2].effects.size() == 1 && lay.buses[2].effects[0].className == "AudioEffectCompressor" && lay.buses[2].effects[0].sidechain == "Music", "SFX has the compressor with its sidechain; the AudioEffectRecord is left out");
        bool thr = false, ratio = false;
        for (const auto& p : lay.buses[2].effects[0].params) { thr |= p.first == "threshold" && near(p.second, -18.0); ratio |= p.first == "ratio" && near(p.second, 6.0); }
        check(thr && ratio, "threshold -18 and ratio 6");
    }
    fmt::ProjectDesc proj;
    check(fmt::loadOcproject(project + "/AudioGame.ocproject", proj, &err) && proj.audioBusLayout == "Audio/default_bus_layout.ocbus", "the project names the layout: AUDIO.BUSLAYOUT Audio/default_bus_layout.ocbus");
    const std::string projText = slurp(project + "/AudioGame.ocproject");
    check(projText.find("AUDIO.BUSLAYOUT Audio/default_bus_layout.ocbus") != std::string::npos, "and writes the key");
    fmt::ProjectDesc again;
    fmt::parseOcproject(fmt::writeOcproject(proj, projText), again, &err);
    check(again.audioBusLayout == proj.audioBusLayout, "the key survives a save of the manifest");

    // ---- the level ---------------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- level --");
    fmt::OcWorldData w;
    check(fmt::loadOcworld(content + "/Maps/main.ocworld", w, &err), "the level loads: " + err);
    const int music = find(w, {"Main", "Music"}), steps = find(w, {"Main", "Steps"}), emitter = find(w, {"Main", "Emitter"}), listener = find(w, {"Main", "Listener"}), video = find(w, {"Main", "Cutscene"});
    check(music >= 0 && steps >= 0 && emitter >= 0 && listener >= 0 && video >= 0, "every node is placed");
    check(classOf(w, music) == "AudioStreamPlayer" && classOf(w, emitter) == "AudioStreamPlayer3D" && classOf(w, listener) == "AudioListener3D", "as AudioStreamPlayer, AudioStreamPlayer3D and AudioListener3D");
    const std::string* stream = propOf(w, music, "stream");
    check(stream && stream->find("AudioStreamWAV") != std::string::npos && stream->find("Audio/sfx/loop.wav") != std::string::npos && stream->find("loop_mode") != std::string::npos,
          "the looping WAV import becomes Object(AudioStreamWAV) with its source and loop mode: " + (stream ? *stream : std::string("(none)")));
    const std::string* bus = propOf(w, music, "bus");
    check(bus && bus->find("Music") != std::string::npos, "the bus name is kept");
    const std::string* vol = propOf(w, music, "volume_db");
    check(vol && near(std::atof(vol->c_str()), -3.0), "volume_db -3");
    const std::string* rnd = propOf(w, steps, "stream");
    check(rnd && rnd->find("AudioStreamRandomizer") != std::string::npos && rnd->find("stream_1/weight") != std::string::npos && rnd->find("Audio/sfx/step_b.wav") != std::string::npos &&
              rnd->find("random_pitch") != std::string::npos,
          "the randomizer keeps its streams (as Audio/ paths), weights and pitch range: " + (rnd ? *rnd : std::string("(none)")));
    const std::string* plain = propOf(w, emitter, "stream");
    check(plain && plain->find("Audio/sfx/step_a.wav") != std::string::npos, "a plain WAV is Resource(\"Audio/sfx/step_a.wav\")");
    const std::string* unit = propOf(w, emitter, "unit_size");
    const std::string* model = propOf(w, emitter, "attenuation_model");
    const std::string* maxd = propOf(w, emitter, "max_distance");
    check(unit && near(std::atof(unit->c_str()), 2.0) && model && std::atoi(model->c_str()) == 1 && maxd && near(std::atof(maxd->c_str()), 40.0), "the 3D player keeps unit_size, attenuation_model and max_distance");
    check(propOf(w, emitter, "emission_angle_degrees") && propOf(w, emitter, "doppler_tracking"), "and the cone and Doppler properties");
    const std::string* cls = propOf(w, video, "_class");
    check(cls && cls->find("VideoStreamPlayer") != std::string::npos, "the VideoStreamPlayer is a carrier that names its class");

    // ---- the report -------------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- the report --");
    {
        const std::string jsonl = slurp(out1 + "/FabricReport.jsonl");
        const auto has = [&](const std::string& a, const std::string& b, const std::string& c) {
            std::istringstream is(jsonl);
            std::string line;
            while (std::getline(is, line))
                if (line.find(a) != std::string::npos && line.find(b) != std::string::npos && line.find(c) != std::string::npos) return true;
            return false;
        };
        check(has("loop.wav", "\"rating\":\"converted\"", "copied to Audio/sfx/loop.wav"), "the WAV is rated converted");
        check(has("song.ogg", "\"rating\":\"converted\"", "decodes Ogg Vorbis itself"), "the Ogg file is rated converted: the engine decodes it on every platform");
        check(has("jingle.mp3", "\"rating\":\"converted\"", "decodes MP3 itself"), "the MP3 file is rated converted: the engine decodes it on every platform");
        check(has("loop.wav", "\"property\":\"edit/trim\"", "\"rating\":\"approximated\""), "an import-time trim is reported as not applied");
        check(has("AudioEffectReverb", "\"rating\":\"approximated\"", "Freeverb"), "the reverb is rated approximated with its difference");
        check(has("AudioEffectCompressor", "\"rating\":\"approximated\"", "compressor"), "the compressor is rated approximated with its difference");
        check(has("AudioEffectRecord", "\"rating\":\"unsupported\"", "no AudioEffectRecord"), "AudioEffectRecord is unsupported: the mixer has none");
        check(has("\"type\":\"AudioStreamPlayer\"", "\"property\":\"volume_db\"", "\"rating\":\"converted\""), "volume_db is rated converted");
        check(has("\"type\":\"AudioStreamPlayer\"", "\"property\":\"mix_target\"", "\"rating\":\"approximated\""), "mix_target is approximated (stereo only)");
        check(has("\"type\":\"AudioStreamPlayer3D\"", "\"property\":\"area_mask\"", "\"rating\":\"approximated\""), "area_mask is approximated (no area reverb buses)");
        check(has("\"type\":\"AudioStreamPlayer3D\"", "\"property\":\"attenuation_model\"", "\"rating\":\"converted\""), "attenuation_model is converted");
        check(has("Cutscene", "\"rating\":\"unsupported\"", "video decoder"), "the VideoStreamPlayer is unsupported, with the decoder reason");
        check(has("AudioStreamRandomizer", "\"rating\":\"converted\"", "Object("), "the randomizer resource is rated converted");
        check(has("audio/buses/default_bus_layout", "\"rating\":\"converted\"", "AUDIO.BUSLAYOUT"), "the project setting is rated converted");
    }

    AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures);
    if (!g_failures) std::filesystem::remove_all(outRoot, ec);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
