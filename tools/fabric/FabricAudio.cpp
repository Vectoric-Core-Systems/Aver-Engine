// Audio: sound files, the audio stream resources, the audio nodes' properties and the bus layout (docs/plans/fabric/G5_PARITY.md, S1).
//
// Files are copied under Content/Audio/<their res:// path> (Aver.Fabric's ResourceLoader looks there for a bare res:// path, so animation audio tracks and scripts
// that load("res://sfx/hit.wav") find them too). Stream resources a scene holds become `Object(Class, "prop": value, ...)` values the runtime builds, with Godot's
// property names. A Godot AudioBusLayout becomes an .ocbus (aver/formats/OcBusLayout.hpp) that the project names with AUDIO.BUSLAYOUT. The engine's own effect
// tables decide which effect parameters are carried, so the converter cannot drift from what the mixer implements.
#include "FabricInternal.hpp"

#include "aver/audio/Effects.hpp"
#include "aver/formats/OcBusLayout.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace aver::fabric {

namespace {

std::string extLower(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    return dot == std::string::npos ? std::string() : lower(path.substr(dot));
}

bool matchesKey(const std::string& key, const char* pattern) {
    const size_t n = std::strlen(pattern);
    if (n > 0 && pattern[n - 1] == '*') return key.compare(0, n - 1, pattern, n - 1) == 0;
    return key == pattern;
}

struct StreamFamily {
    const char* type;
    std::vector<const char*> known;
    std::vector<std::pair<const char*, const char*>> approx;
};

const std::vector<StreamFamily>& streamFamilies() {
    static const std::vector<StreamFamily> f = {
        {"AudioStreamRandomizer", {"playback_mode", "random_pitch", "random_volume_offset_db", "streams_count", "stream_*"}, {}},
        {"AudioStreamPlaylist", {"shuffle", "loop", "stream_count", "stream_*", "bpm", "beat_count"},
         {{"fade_time", "stored; the entries play back to back, the engine does not cross-fade them"}}},
        {"AudioStreamSynchronized", {"stream_count", "stream_*"}, {}},
        {"AudioStreamInteractive", {"clip_count", "initial_clip", "clip_*"},
         {{"_transitions", "the transition rules (beat or bar timing, fades, filler clips) are not carried: the engine switches clips at once"},
          {"transitions", "the transition rules (beat or bar timing, fades, filler clips) are not carried: the engine switches clips at once"}}},
        {"AudioStreamPolyphonic", {"polyphony"}, {}},
        {"AudioStreamGenerator", {"mix_rate", "mix_rate_mode", "buffer_length"}, {}},
        {"AudioStreamWAV", {"data", "format", "mix_rate", "stereo", "loop_mode", "loop_begin", "loop_end"}, {}},
    };
    return f;
}

const StreamFamily* familyOf(const std::string& type) {
    for (const StreamFamily& f : streamFamilies()) if (type == f.type) return &f;
    return nullptr;
}

bool ignorable(const std::string& n) {
    return n == "resource_local_to_scene" || n == "resource_name" || n == "resource_path" || n == "script" || n == "metadata/_custom_type_script";
}

// Where each effect departs from Godot's algorithm (the parameters keep Godot's names and ranges either way).
const char* effectApproximation(aver::audio::EffectKind k) {
    using K = aver::audio::EffectKind;
    switch (k) {
        case K::Delay: return "Godot's taps and feedback; the tap pan law is constant power with unity at the centre";
        case K::Chorus: return "a modulated-delay chorus with Godot's voice parameters; the voices are summed from the mono mix, so a very wide input narrows";
        case K::Compressor: return "a peak-envelope compressor on the static curve (threshold, ratio, makeup, attack, release, mix, sidechain); transients differ from Godot's detector";
        case K::Limiter: return "a soft-clip limiter (knee at ceiling minus soft_clip_db, tanh above it); Godot's differs in the knee";
        case K::HardLimiter: return "a look-ahead brick-wall limiter (1.5 ms): the output never exceeds the ceiling; Godot's release curve differs";
        case K::EQ6: case K::EQ10: case K::EQ21: return "peaking filters at Godot's band centres; the band overlap differs from Godot's equaliser";
        case K::Distortion: return "Godot's five modes with the same parameters; the shaping curves are the engine's own";
        case K::Phaser: return "a six-stage all-pass phaser with Godot's parameters; the depth scale differs";
        case K::PitchShift: return "a time-domain two-head shifter, not Godot's FFT one: the pitch is right, some modulation remains on sustained tones";
        case K::Reverb: return "a Freeverb-style reverb with Godot's parameters; the tail and colour differ";
        case K::Panner: return "a linear balance control: -1 silences the right channel, +1 the left";
        case K::StereoEnhance: return "mid/side widening with the right channel delayed by time_pullout_ms; the surround term is simplified";
        default: return nullptr;
    }
}

}  // namespace

bool isAudioExt(const std::string& ext) { return ext == ".wav" || ext == ".ogg" || ext == ".mp3"; }

bool isAudioStreamResourceType(const std::string& t) {
    return familyOf(t) != nullptr;
}

std::string Converter::audioFileAsset(const std::string& resPath, const Where& w) {
    if (const auto it = audioAssets_.find(resPath); it != audioAssets_.end()) return it->second;
    const std::string ext = extLower(resPath);
    std::string asset;
    std::vector<u8> bytes;
    const std::string disk = fs_.toDiskPath(resPath);
    if (disk.empty() || !readFileBytes(disk, bytes)) {
        report_.add("resource", resPath, w.node, "AudioStream", "", Rating::Unsupported, "the audio file is missing", 0);
    } else {
        const std::string rel = resPath.rfind("res://", 0) == 0 ? resPath.substr(6) : resPath;
        asset = "Audio/" + rel;
        const std::string dst = contentDir_ + "/" + asset;
        createDirectories(std::filesystem::path(dst).parent_path().string());
        if (!writeFileBytes(dst, bytes.data(), bytes.size())) {
            report_.add("resource", resPath, w.node, "AudioStream", "", Rating::Unsupported, "could not write " + dst, 0);
            asset.clear();
        } else if (ext == ".wav") {
            report_.add("resource", resPath, "", "AudioStreamWAV", "", Rating::Converted, "copied to " + asset + "; the engine reads WAV itself", 0);
        } else if (ext == ".mp3") {
            report_.add("resource", resPath, "", "AudioStreamMP3", "", Rating::Approximated,
                        "copied to " + asset + "; decoded by the platform's media stack (Windows Media Foundation), so it plays on Windows only: the engine has no MP3 decoder of its own", 0);
        } else {
            report_.add("resource", resPath, "", "AudioStreamOggVorbis", "", Rating::Approximated,
                        "copied to " + asset + "; the engine has no Ogg Vorbis decoder: Windows Media Foundation decodes it only when the Web Media Extensions codec is installed, and other platforms cannot "
                        "play it (convert the file to WAV, or see docs/AUDIO.md for what a decoder dependency would add)", 0);
        }
    }
    audioAssets_[resPath] = asset;
    return asset;
}

Var Converter::audioFileValue(const std::string& resPath, const std::string& godotType, const Where& w, const std::string& prop, int line) {
    const std::string ext = extLower(resPath);
    const std::string asset = audioFileAsset(resPath, w);
    if (asset.empty()) return makeNil();
    (void)godotType;
    (void)prop;
    (void)line;

    // The importer's settings: loop points and the options the converter does not apply.
    gd::GodotImport imp;
    gd::GodotDiagnostics d;
    std::vector<std::pair<std::string, Var>> extra;
    if (fs_.loadImport(resPath, imp, d)) {
        if (ext == ".wav") {
            const int mode = static_cast<int>(std::lround(numOf(imp.param("edit/loop_mode"), 0)));
            if (mode >= 2 && mode <= 4) {
                extra.emplace_back("loop_mode", makeInt(mode - 1));
                extra.emplace_back("loop_begin", makeInt(static_cast<long long>(numOf(imp.param("edit/loop_begin"), 0))));
                extra.emplace_back("loop_end", makeInt(std::max(0LL, static_cast<long long>(numOf(imp.param("edit/loop_end"), 0)))));   // -1: to the end
                if (mode >= 3)
                    report_.add("resource", resPath, "", "AudioStreamWAV", "edit/loop_mode", Rating::Approximated, "ping-pong and backward loops play forward: the mixer loops in one direction", 0);
            } else if (mode == 0) {
                report_.add("resource", resPath, "", "AudioStreamWAV", "edit/loop_mode", Rating::Approximated,
                            "'Detect From WAV' reads the loop points of the file's sampler chunk in Godot; the engine plays the file without a loop", 0);
            }
            for (const char* k : {"edit/trim", "edit/normalize", "force/8_bit", "force/mono", "force/max_rate"})
                if (const Var* v = imp.param(k); v && boolOf(v, false))
                    report_.add("resource", resPath, "", "AudioStreamWAV", k, Rating::Approximated, "an import-time edit the converter does not apply to the copied file", 0);
        } else {
            if (boolOf(imp.param("loop"), false)) {
                extra.emplace_back("loop", makeBool(true));
                if (const Var* off = imp.param("loop_offset"); off && numOf(off, 0) != 0)
                    report_.add("resource", resPath, "", ext == ".mp3" ? "AudioStreamMP3" : "AudioStreamOggVorbis", "loop_offset", Rating::Approximated,
                                "stored; a loop restarts at the beginning of the file", 0);
            }
            if (numOf(imp.param("bpm"), 0) != 0) extra.emplace_back("bpm", makeFloat(numOf(imp.param("bpm"), 0)));
            if (numOf(imp.param("beat_count"), 0) != 0) extra.emplace_back("beat_count", makeInt(static_cast<long long>(numOf(imp.param("beat_count"), 0))));
        }
    }
    if (extra.empty()) return makeResource(asset);
    extra.insert(extra.begin(), {"source_path", makeString(asset)});
    return makeObject(ext == ".wav" ? "AudioStreamWAV" : ext == ".mp3" ? "AudioStreamMP3" : "AudioStreamOggVorbis", std::move(extra));
}

Var Converter::audioStreamObject(const std::string& type, const std::vector<gd::GodotProperty>& props, const gd::GodotDocBase& doc, const Where& w, int line) {
    const StreamFamily* fam = familyOf(type);
    std::vector<std::pair<std::string, Var>> entries;
    for (const gd::GodotProperty& p : props) {
        if (ignorable(p.name)) continue;
        bool known = false, reportedOnly = false;
        if (fam) {
            for (const char* k : fam->known) if (matchesKey(p.name, k)) { known = true; break; }
            for (const auto& a : fam->approx)
                if (p.name == a.first) {
                    known = reportedOnly = true;   // stored in the report only: the runtime has nothing to apply it to
                    report_.add("property", doc.file, w.node, type, p.name, Rating::Approximated, a.second, p.line);
                    break;
                }
        }
        if (!known) {
            report_.add("property", doc.file, w.node, type, p.name, Rating::Unsupported, "the runtime class has no such property; the value is dropped", p.line);
            continue;
        }
        if (reportedOnly) continue;
        Var cv = convertValue(p.value, doc, w, p.name);
        if (cv.isNil() && !p.value.isNil()) continue;   // a resource that could not be converted; already reported
        entries.emplace_back(p.name, std::move(cv));
    }
    if (type == "AudioStreamWAV") {
        const Var* fmtv = nullptr;
        for (const gd::GodotProperty& p : props) if (p.name == "format") fmtv = &p.value;
        const int format = static_cast<int>(numOf(fmtv, 0));
        if (format >= 2)
            report_.add("resource", doc.file, w.node, type, "format", Rating::Unsupported, format == 2 ? "IMA ADPCM data is not decoded; only 8 and 16 bit PCM plays" : "QOA data is not decoded; only 8 and 16 bit PCM plays", line);
    }
    report_.add("resource", doc.file, w.node, type, "", Rating::Converted, "kept as Object(" + type + ") with Godot's property names; Aver.Fabric builds it", line);
    return makeObject(type, std::move(entries));
}

void Converter::noteAudioNode(const FNode& fn) {
    const auto note = [&](const Prop& pr, Rating r, const char* why) { report_.add("property", fn.reportFile, fn.reportPath, fn.type, pr.key, r, why, pr.line); };
    const bool threeD = fn.type == "AudioStreamPlayer3D";
    for (const Prop& pr : fn.props) {
        const std::string& k = pr.key;
        if (k == "stream" || k == "volume_db" || k == "pitch_scale" || k == "playing" || k == "autoplay" || k == "stream_paused" || k == "bus" || k == "max_polyphony")
            note(pr, Rating::Converted, "AudioStreamPlayer (Aver.Fabric) reads it");
        else if (threeD && (k == "attenuation_model" || k == "unit_size" || k == "max_db" || k == "max_distance" || k == "panning_strength" || k == "emission_angle_enabled" ||
                            k == "emission_angle_degrees" || k == "emission_angle_filter_attenuation_db" || k == "attenuation_filter_cutoff_hz" || k == "attenuation_filter_db" ||
                            k == "doppler_tracking" || k == "unit_db"))
            note(pr, Rating::Converted, "the mixer applies Godot's AudioStreamPlayer3D model with this value");
        else if (threeD && k == "area_mask")
            note(pr, Rating::Approximated, "stored; reverb buses of areas are not routed");
        else if (k == "mix_target")
            note(pr, Rating::Approximated, "the mixer outputs stereo only; surround and centre targets play as stereo");
        else if (k == "playback_type")
            note(pr, Rating::Approximated, "stored; every sound plays as a stream (there are no sample playbacks)");
    }
    if (fn.type == "AudioListener3D") report_.add("node", fn.reportFile, fn.reportPath, fn.type, "", Rating::Converted, "the first listener in the tree is current, as in Godot", 0);
}

// ---- the bus layout --------------------------------------------------------------------------------------------------------------

bool Converter::convertBusLayout(const std::string& resPath, const gd::GodotResource& r) {
    using aver::audio::Effect;
    using aver::audio::EffectKind;
    aver::audio::BusLayout layout;
    int count = 1;
    for (const gd::GodotProperty& p : r.props) {
        if (p.name.rfind("bus/", 0) != 0) continue;
        const size_t slash = p.name.find('/', 4);
        if (slash == std::string::npos) continue;
        count = std::max(count, std::atoi(p.name.substr(4, slash - 4).c_str()) + 1);
    }
    count = std::min(count, 255);
    layout.buses.resize(static_cast<size_t>(count));
    layout.buses[0].name = "Master";
    layout.buses[0].send.clear();
    for (int i = 0; i < count; ++i) {
        aver::audio::BusLayoutBus& b = layout.buses[static_cast<size_t>(i)];
        const std::string pre = "bus/" + std::to_string(i) + "/";
        if (i > 0) b.name = "Bus " + std::to_string(i);
        std::map<int, aver::audio::BusLayoutEffect> effects;
        for (const gd::GodotProperty& p : r.props) {
            if (p.name.rfind(pre, 0) != 0) continue;
            const std::string key = p.name.substr(pre.size());
            if (key == "name") b.name = stringOf(&p.value, b.name);
            else if (key == "solo") b.solo = boolOf(&p.value, false);
            else if (key == "mute") b.mute = boolOf(&p.value, false);
            else if (key == "bypass_fx") b.bypass = boolOf(&p.value, false);
            else if (key == "volume_db") b.volumeDb = static_cast<f32>(numOf(&p.value, 0.0));
            else if (key == "send") b.send = stringOf(&p.value, "Master");
            else if (key.rfind("effect/", 0) == 0) {
                const size_t s2 = key.find('/', 7);
                if (s2 == std::string::npos) continue;
                const int k = std::atoi(key.substr(7, s2 - 7).c_str());
                const std::string what = key.substr(s2 + 1);
                if (what == "enabled") effects[k].enabled = boolOf(&p.value, true);
                else if (what == "effect") {
                    const gd::GodotSubResource* sr = p.value.type == VType::SubResource ? r.findSub(p.value.s) : nullptr;
                    if (!sr) {
                        report_.add("resource", resPath, "", "AudioBusLayout", p.name, Rating::Unsupported, "the effect resource is not declared", p.line);
                        continue;
                    }
                    effects[k].className = sr->type;
                    const EffectKind kind = Effect::kindFromClassName(sr->type.c_str());
                    std::unique_ptr<Effect> probe = kind == EffectKind::Count ? nullptr : Effect::create(kind, 48000);
                    if (!probe) {
                        report_.add("resource", resPath, "", sr->type, "", Rating::Unsupported,
                                    "the mixer has no " + sr->type + "; the effect is left out of bus '" + b.name + "'", sr->line);
                        effects[k].className.clear();
                        continue;
                    }
                    for (const gd::GodotProperty& sp : sr->props) {
                        if (ignorable(sp.name)) continue;
                        if (sp.name == "sidechain") { effects[k].sidechain = stringOf(&sp.value, ""); continue; }
                        if (probe->findParam(sp.name.c_str()) < 0) {
                            report_.add("property", resPath, "", sr->type, sp.name, Rating::Unsupported, "the mixer's effect has no such parameter; the value is dropped", sp.line);
                            continue;
                        }
                        const double v = sp.value.type == VType::Bool ? (boolOf(&sp.value, false) ? 1.0 : 0.0) : numOf(&sp.value, 0.0);
                        effects[k].params.emplace_back(sp.name, static_cast<f32>(v));
                    }
                    if (const char* why = effectApproximation(kind))
                        report_.add("resource", resPath, "", sr->type, "", Rating::Approximated, why, sr->line);
                    else
                        report_.add("resource", resPath, "", sr->type, "", Rating::Converted, "implemented in the mixer with Godot's coefficients and parameters", sr->line);
                }
            }
        }
        for (auto& [k, e] : effects)
            if (!e.className.empty()) b.effects.push_back(std::move(e));
        report_.add("resource", resPath, "", "AudioBusLayout", pre + "name", Rating::Converted, "bus '" + b.name + "' (volume, send, mute, solo, bypass, " + std::to_string(b.effects.size()) + " effect(s))", 0);
    }

    const std::string asset = "Audio/" + safeName(stemOf(resPath), "bus_layout") + ".ocbus";
    const std::string dst = contentDir_ + "/" + asset;
    createDirectories(std::filesystem::path(dst).parent_path().string());
    const std::string text = fmt::writeOcBusLayout(layout);
    if (!writeFileText(dst, text)) {
        report_.add("resource", resPath, "", "AudioBusLayout", "", Rating::Unsupported, "could not write " + dst, 0);
        return false;
    }
    const std::string defaultLayout = settingString("audio/buses/default_bus_layout", "res://default_bus_layout.tres");
    if (resPath == defaultLayout) {
        audioBusLayout_ = asset;
        report_.add("setting", resPath, "", "", "audio/buses/default_bus_layout", Rating::Converted, "AUDIO.BUSLAYOUT " + asset, 0);
    }
    return true;
}

}  // namespace aver::fabric
