// Accessibility store behind framework_access_abi.h: the option table and its dirty list, the caption engine and its
// .occaptions folder, the speech queue over the OS synthesiser, camera feel, and the input-assist hooks.
// One file-static store; single game thread. docs/ACCESSIBILITY.md.

#include "aver/framework/framework_access_abi.h"
#include "aver/framework/framework_loc_abi.h"
#include "aver/framework/framework_timers_abi.h"

#include "aver/core/Log.hpp"
#include "aver/formats/OcCaptions.hpp"
#include "aver/framework/AccessAssist.hpp"
#include "aver/framework/AccessKeys.hpp"
#include "aver/framework/CameraFx.hpp"
#include "aver/framework/CaptionEngine.hpp"
#include "aver/framework/EventBus.hpp"
#include "aver/framework/SpeechQueue.hpp"
#include "aver/platform/Speech.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

// The process event bus lives in TimersEventsAbi.cpp; the accessor is the only thing shared with this file.
namespace aver::fw { EventBus& processEventBus(); }

namespace fs = std::filesystem;
namespace acc = aver::fw::access;
namespace fmt = aver::fmt;

namespace {

struct Store {
    acc::AccessState state;
    acc::CaptionEngine captions;
    std::string captionDir;
    std::string scratch;                       // key_at / key_choices return storage
    aver::fw::CameraFx fx;
    aver::platform::SpeechSynth* synth = nullptr;
    acc::SpeechQueue* queue = nullptr;
    bool speechWasEnabled = false;
    std::chrono::steady_clock::time_point lastTick{};
    bool haveTick = false;
};

Store& g() { static Store s; return s; }

// ---- helpers ----

fs::path toPath(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}
std::string fromPath(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}
std::string lowerAscii(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
std::string normPath(std::string s) {
    for (char& c : s) if (c == '\\') c = '/';
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}
std::string safe(const char* s) { return s ? std::string(s) : std::string(); }

// Resolves a localization reference through the framework store; false when `raw` is literal text.
bool resolveRef(const std::string& raw, std::string& out) {
    const int32_t n = aver_fw_loc_resolve(raw.c_str(), nullptr, 0);
    if (n < 0) return false;
    out.assign(static_cast<size_t>(n) + 1, '\0');
    aver_fw_loc_resolve(raw.c_str(), out.data(), n + 1);
    out.resize(static_cast<size_t>(n));
    return true;
}
std::string resolved(const std::string& raw) {
    std::string o;
    return resolveRef(raw, o) ? o : raw;
}

void postChanged(const std::string& key, float value) {
    aver_fw_event_push_string(key.c_str());
    aver_fw_event_push_float(value);
    aver_fw_event_post("Access.Changed", 0, 0);
}

// Pushes the keys that other engines read into them (input assist).
void applySideEffects(const std::string& key) {
    acc::AccessState& st = g().state;
    if (key == "access.input.holdScale" || key == "access.input.windowScale")
        acc::accessAssist().setTiming(st.get("access.input.holdScale"), st.get("access.input.windowScale"));
    else if (key.size() > acc::kToggleKeyPrefix.size() && key.compare(0, acc::kToggleKeyPrefix.size(), acc::kToggleKeyPrefix) == 0)
        acc::accessAssist().setToggle(key.substr(acc::kToggleKeyPrefix.size()), st.get(key) > 0.5f);
}
void applyAllSideEffects() {
    acc::AccessState& st = g().state;
    acc::accessAssist().setTiming(st.get("access.input.holdScale"), st.get("access.input.windowScale"));
    for (const auto& kv : st.actionToggle) acc::accessAssist().setToggle(kv.first, kv.second);
}

acc::CaptionOptions captionOptions() {
    const acc::AccessState& st = g().state;
    acc::CaptionOptions o;
    o.mode = static_cast<aver::i32>(st.get("access.captions.mode") + 0.5f);
    o.maxLines = static_cast<aver::i32>(st.get("access.captions.lines") + 0.5f);
    o.speaker = st.get("access.captions.speaker") > 0.5f;
    o.colours = st.get("access.captions.colours") > 0.5f;
    o.direction = st.get("access.captions.direction") > 0.5f;
    o.reduceMotion = st.get("access.motion.reduce") > 0.5f;
    return o;
}

// ---- bus subscriptions (Caption.Show / Caption.Stop) ----

bool onCaptionShow(const aver::fw::Event& e) {
    const auto& a = e.payload.args;
    if (a.empty() || a[0].type != aver::fw::EventValueType::String) return false;
    if (a.size() >= 4) {
        const float p[3] = {a[1].asFloat(), a[2].asFloat(), a[3].asFloat()};
        g().captions.show(a[0].s, p);
    } else if (a.size() >= 2 && a[1].type == aver::fw::EventValueType::Vec3) {
        g().captions.show(a[0].s, a[1].f);
    } else {
        g().captions.show(a[0].s);
    }
    return false;   // never consumes: other subscribers still see it
}
bool onCaptionStop(const aver::fw::Event& e) {
    const auto& a = e.payload.args;
    g().captions.stop(!a.empty() && a[0].type == aver::fw::EventValueType::String ? a[0].s : std::string());
    return false;
}

// aver_fw_events_clear drops every subscription (a script reset), so the marker listener tells us to re-subscribe.
void ensureSubscribed() {
    aver::fw::EventBus& bus = aver::fw::processEventBus();
    if (bus.listenerCount("Access.Alive") > 0) return;
    bus.subscribe("Access.Alive", [](const aver::fw::Event&) { return false; });
    bus.subscribe("Caption.Show", onCaptionShow);
    bus.subscribe("Caption.Stop", onCaptionStop);
}

// ---- captions: the folder ----

int32_t loadCaptionDir(const std::string& dirIn) {
    Store& s = g();
    s.captionDir = normPath(dirIn);
    fmt::OcCaptionData merged;
    std::error_code ec;
    bool exists = !s.captionDir.empty() && fs::is_directory(toPath(s.captionDir), ec);
    std::vector<std::pair<std::string, std::string>> files;   // lowercase name, full path
    if (exists) {
        for (fs::directory_iterator it(toPath(s.captionDir), ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            if (lowerAscii(fromPath(it->path().extension())) != ".occaptions") continue;
            files.emplace_back(lowerAscii(fromPath(it->path().filename())), fromPath(it->path()));
        }
        std::sort(files.begin(), files.end());
    }
    std::set<std::string> seenCue, warned;
    for (const auto& f : files) {
        fmt::OcCaptionData d;
        std::vector<std::string> diag;
        std::string err;
        const bool ok = fmt::loadOcCaptions(f.second, d, &diag, &err);
        for (const std::string& m : diag) AVER_WARN("[Captions] {}: {}", f.second, m);
        if (!ok) { AVER_WARN("[Captions] {}: {}", f.second, err); continue; }
        for (fmt::OcCaptionSpeaker& sp : d.speakers) {
            bool replaced = false;
            for (fmt::OcCaptionSpeaker& e : merged.speakers) if (e.id == sp.id) { e = sp; replaced = true; break; }
            if (!replaced) merged.speakers.push_back(std::move(sp));
        }
        for (fmt::OcCaptionCue& c : d.cues) {
            if (!seenCue.insert(c.id).second && warned.insert(c.id).second)
                AVER_WARN("[Captions] cue '{}' is defined more than once; the later one wins ({})", c.id, f.second);
            merged.cues.push_back(std::move(c));
        }
    }
    s.captions.load(merged);
    if (!exists) return -1;
    AVER_INFO("[Captions] {} cue(s) from {} file(s) in '{}'", static_cast<int>(s.captions.cueCount()), static_cast<int>(files.size()), s.captionDir);
    return static_cast<int32_t>(s.captions.cueCount());
}

// ---- speech ----

acc::SpeechQueue* speech(bool create) {
    Store& s = g();
    if (!s.queue) {
        if (!create) return nullptr;
        s.synth = aver::platform::createSpeechSynth().release();   // lives for the process; its worker must not join in a DLL detach
        s.queue = new acc::SpeechQueue(*s.synth);
        s.queue->setResolver(resolveRef);
    }
    const acc::AccessState& st = s.state;
    acc::SpeechSettings ss;
    ss.enabled = st.get("access.speech.enabled") > 0.5f;
    ss.ui = st.get("access.speech.ui") > 0.5f;
    ss.captions = st.get("access.speech.captions") > 0.5f;
    ss.rate = static_cast<aver::i32>(std::lround(st.get("access.speech.rate")));
    ss.volume = st.get("access.speech.volume");
    s.queue->setSettings(ss, st.revision);
    return s.queue;
}

void fill(const acc::CaptionOutLine& l, AverFwCaptionLine* out) {
    out->speaker = l.speakerRef.c_str();
    out->text = l.textRef.c_str();
    out->rgba = l.rgba;
    out->kind = l.kind;
    out->dir = l.dir;
    out->alpha = l.alpha;
}

void narrateNewLines() {
    const Store& s = g();
    if (s.captions.newLines().empty()) return;
    if (s.state.get("access.speech.enabled") < 0.5f || s.state.get("access.speech.captions") < 0.5f) return;
    acc::SpeechQueue* q = speech(true);
    for (const acc::CaptionOutLine& l : s.captions.newLines()) {
        if (l.speakerRef.empty()) { q->announce(l.textRef, false, acc::SpeechSource::Captions); continue; }
        std::string text = resolved(l.speakerRef) + ": " + resolved(l.textRef);
        if (text[0] == '@') text.insert(0, "@");   // "@@x" is the literal "@x"
        q->announce(text, false, acc::SpeechSource::Captions);
    }
}

int32_t (*g_alive)(int32_t) = nullptr;

} // namespace

extern "C" {

int32_t aver_fw_access_abi_version(void) { return AVER_FW_ACCESS_ABI_VERSION; }

/* ---- options ---- */

int32_t aver_fw_access_load_ini(const char* utf8Text) {
    Store& s = g();
    const std::string text = safe(utf8Text);
    const uint32_t rev = s.state.revision;
    s.state.resetDefaults();
    s.state.dirty.clear();
    s.state.revision = rev;
    const size_t n = acc::accessParseIni(text, s.state);
    // The OS screen-reader flag is the first-run default for narration; it is not persisted unless the player touches it.
    if (text.find("access.speech.enabled") == std::string::npos && aver::platform::osScreenReaderActive())
        s.state.put("access.speech.enabled", 1.0f);
    acc::accessAssist().clearAll();
    applyAllSideEffects();
    ++s.state.revision;
    return static_cast<int32_t>(n);
}

int32_t aver_fw_access_key_count(void) { return static_cast<int32_t>(acc::kAccessKeyCount); }
const char* aver_fw_access_key_at(int32_t i) {
    return i >= 0 && static_cast<size_t>(i) < acc::kAccessKeyCount ? acc::kAccessKeys[i].key : "";
}

int32_t aver_fw_access_key_info(const char* key, float* out6) {
    const acc::AccessKey* k = acc::accessFind(safe(key));
    if (!k) return 0;
    if (out6) {
        out6[0] = static_cast<float>(static_cast<int>(k->kind));
        out6[1] = k->lo; out6[2] = k->hi; out6[3] = k->step; out6[4] = k->def;
        out6[5] = g().state.get(safe(key));
    }
    return 1;
}

const char* aver_fw_access_key_choices(const char* key) {
    const acc::AccessKey* k = acc::accessFind(safe(key));
    return k && k->choices ? k->choices : "";
}

float aver_fw_access_get(const char* key) { return g().state.get(safe(key)); }

int32_t aver_fw_access_set(const char* key, float value) {
    Store& s = g();
    const std::string k = safe(key);
    const uint32_t before = s.state.revision;
    if (!s.state.set(k, value)) return 0;
    if (s.state.revision != before) {
        applySideEffects(k);
        postChanged(k, s.state.get(k));
    }
    return 1;
}

void aver_fw_access_reset_defaults(void) {
    Store& s = g();
    std::vector<std::string> toggles;
    for (const auto& kv : s.state.actionToggle) if (kv.second) toggles.push_back(kv.first);
    for (size_t i = 0; i < acc::kAccessKeyCount; ++i) aver_fw_access_set(acc::kAccessKeys[i].key, acc::kAccessKeys[i].def);
    for (const std::string& a : toggles) aver_fw_access_set((std::string(acc::kToggleKeyPrefix) + a).c_str(), 0.0f);
}

int32_t aver_fw_access_revision(void) { return static_cast<int32_t>(g().state.revision); }

int32_t aver_fw_access_take_dirty(int32_t, char* keyOut, int32_t cap, float* valueOut) {
    Store& s = g();
    if (s.state.dirty.empty()) return 0;
    const auto d = s.state.dirty.front();
    s.state.dirty.erase(s.state.dirty.begin());
    if (keyOut && cap > 0) {
        const size_t n = std::min<size_t>(d.first.size(), static_cast<size_t>(cap) - 1);
        std::memcpy(keyOut, d.first.data(), n);
        keyOut[n] = '\0';
    }
    if (valueOut) *valueOut = d.second;
    return 1;
}

void aver_fw_access_set_toggle_allowed(const char* utf8Action, int32_t allowed) {
    if (!utf8Action || !*utf8Action) return;
    acc::accessAssist().setAllowed(utf8Action, allowed != 0);
}
int32_t aver_fw_access_toggle_allowed(const char* utf8Action) {
    return utf8Action && acc::accessAssist().allowed(utf8Action) ? 1 : 0;
}

int32_t aver_fw_access_effective(float* out8) {
    if (!out8) return 0;
    const acc::AccessEffective e = acc::accessEffective(g().state);
    out8[0] = e.colourVisionPacked; out8[1] = e.uiScale; out8[2] = e.textScale; out8[3] = e.highContrast;
    out8[4] = e.shake; out8[5] = e.bob; out8[6] = e.motionBlurAllowed; out8[7] = e.lensAllowed;
    return 1;
}

/* ---- captions ---- */

int32_t aver_fw_caption_load_dir(const char* utf8Dir) {
    ensureSubscribed();
    return loadCaptionDir(safe(utf8Dir));
}

int32_t aver_fw_caption_reload_path(const char* utf8AbsPath) {
    Store& s = g();
    const std::string p = normPath(safe(utf8AbsPath));
    if (s.captionDir.empty() || lowerAscii(fromPath(toPath(p).extension())) != ".occaptions") return 0;
    const std::string dir = lowerAscii(s.captionDir) + "/";
    if (lowerAscii(p).compare(0, dir.size(), dir) != 0) return 0;
    loadCaptionDir(s.captionDir);
    return 1;
}

void aver_fw_caption_clear(void) {
    g().captions.clear();
    g().captionDir.clear();
}
int32_t aver_fw_caption_cue_count(void) { return static_cast<int32_t>(g().captions.cueCount()); }

int32_t aver_fw_caption_show(const char* cue, int32_t positional, float x, float y, float z) {
    g().captions.setOptions(captionOptions());
    const float p[3] = {x, y, z};
    return g().captions.show(safe(cue), positional ? p : nullptr) ? 1 : 0;
}

int32_t aver_fw_caption_show_text(const char* speakerRef, const char* textRef, float seconds, uint32_t rgba, int32_t kind) {
    g().captions.setOptions(captionOptions());
    return g().captions.showText(safe(speakerRef), safe(textRef), seconds, rgba, kind) ? 1 : 0;
}

void aver_fw_caption_stop(const char* cue) { g().captions.stop(safe(cue)); }

void aver_fw_access_audio_observer(void*, const char* path, int32_t voice, int32_t bus, int32_t positional,
                                   float x, float y, float z, int32_t looping) {
    if (!path || !*path) return;
    Store& s = g();
    s.captions.setOptions(captionOptions());
    acc::CaptionSoundStart st;
    st.path = path;
    st.voice = voice;
    st.bus = bus;
    st.positional = positional != 0;
    st.looping = looping != 0;
    st.pos[0] = x; st.pos[1] = y; st.pos[2] = z;
    s.captions.onSound(st);
}

void aver_fw_caption_set_voice_probe(int32_t (*alive)(int32_t voice)) {
    g_alive = alive;
    if (alive) g().captions.setVoiceProbe([](aver::i32 v) { return g_alive && g_alive(v) != 0; });
    else g().captions.setVoiceProbe(nullptr);
}

void aver_fw_caption_update(float dt, const float* listenerPos3, const float* listenerFwd3, const float* listenerRight3) {
    Store& s = g();
    ensureSubscribed();
    s.captions.setOptions(captionOptions());
    acc::CaptionListener l;
    if (listenerPos3) for (int k = 0; k < 3; ++k) l.pos[k] = listenerPos3[k];
    if (listenerFwd3) for (int k = 0; k < 3; ++k) l.forward[k] = listenerFwd3[k];
    if (listenerRight3) for (int k = 0; k < 3; ++k) l.right[k] = listenerRight3[k];
    s.captions.update(dt, l);
    narrateNewLines();
}

int32_t aver_fw_caption_line_count(void) { return static_cast<int32_t>(g().captions.lines().size()); }
int32_t aver_fw_caption_line_at(int32_t i, AverFwCaptionLine* out) {
    const auto& v = g().captions.lines();
    if (!out || i < 0 || static_cast<size_t>(i) >= v.size()) return 0;
    fill(v[static_cast<size_t>(i)], out);
    return 1;
}
int32_t aver_fw_caption_new_line_count(void) { return static_cast<int32_t>(g().captions.newLines().size()); }
int32_t aver_fw_caption_new_line_at(int32_t i, AverFwCaptionLine* out) {
    const auto& v = g().captions.newLines();
    if (!out || i < 0 || static_cast<size_t>(i) >= v.size()) return 0;
    fill(v[static_cast<size_t>(i)], out);
    return 1;
}

/* ---- speech ---- */

int32_t aver_fw_speech_available(void) { return speech(true)->available() ? 1 : 0; }

int32_t aver_fw_speech_say(const char* utf8OrRef, int32_t interrupt) {
    if (g().state.get("access.speech.enabled") < 0.5f) return 0;
    return speech(true)->announce(safe(utf8OrRef), interrupt != 0, acc::SpeechSource::Script) ? 1 : 0;
}

int32_t aver_fw_speech_announce_ui(const char* utf8, int32_t interrupt) {
    if (g().state.get("access.speech.enabled") < 0.5f) return 0;
    return speech(true)->announce(safe(utf8), interrupt != 0, acc::SpeechSource::Ui) ? 1 : 0;
}

void aver_fw_speech_stop(void) {
    if (acc::SpeechQueue* q = speech(false)) q->stop();
}

int32_t aver_fw_speech_speaking(void) {
    acc::SpeechQueue* q = g().queue;
    return q && q->speaking() ? 1 : 0;
}

void aver_fw_speech_tick(void) {
    Store& s = g();
    const bool enabled = s.state.get("access.speech.enabled") > 0.5f;
    if (!s.queue && !enabled) return;
    acc::SpeechQueue* q = speech(true);
    if (s.speechWasEnabled && !enabled) q->stop();
    s.speechWasEnabled = enabled;
    const auto now = std::chrono::steady_clock::now();
    const float dt = s.haveTick ? std::chrono::duration<float>(now - s.lastTick).count() : 0.0f;
    s.lastTick = now;
    s.haveTick = true;
    q->update(dt);
}

int32_t aver_fw_speech_os_screen_reader(void) { return aver::platform::osScreenReaderActive() ? 1 : 0; }

/* ---- camera feel ---- */

void aver_fw_camera_shake_add(float trauma) { g().fx.addShake(trauma); }
void aver_fw_camera_bob_set(float amount01, float speedCmPerSec) { g().fx.setBob(amount01, speedCmPerSec); }

int32_t aver_fw_camera_fx_apply(float dt, float* inoutPos3, float* inoutYawPitchRad2) {
    if (!inoutPos3 || !inoutYawPitchRad2) return 0;
    const acc::AccessEffective e = acc::accessEffective(g().state);
    aver::Vec3 pos{inoutPos3[0], inoutPos3[1], inoutPos3[2]};
    float yaw = inoutYawPitchRad2[0], pitch = inoutYawPitchRad2[1];
    if (!g().fx.apply(pos, yaw, pitch, dt, e.shake, e.bob)) return 0;
    inoutPos3[0] = pos.x; inoutPos3[1] = pos.y; inoutPos3[2] = pos.z;
    inoutYawPitchRad2[0] = yaw; inoutYawPitchRad2[1] = pitch;
    return 1;
}

} // extern "C"
