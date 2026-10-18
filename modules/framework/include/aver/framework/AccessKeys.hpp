#pragma once
// Accessibility options: one key table (kind, range, step, default), the flat state behind it, and the INI-text
// reader the native hosts run before the first frame. Header-only, no I/O. docs/ACCESSIBILITY.md.
//
// Every option is a float under a stable key because the settings screen stores everything as floats. The table is
// mirrored by uiDefaultSettingsModel() (Aver.UI cannot see this header); GameAccessibilityTest pins the two together.

#include "aver/core/Types.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aver::fw::access {

enum class AccessKind : u8 { Toggle, Slider, Choice };

struct AccessKey {
    const char* key;
    AccessKind kind;
    f32 lo, hi, step, def;
    const char* label;     // loc ref with the English default; null = not shown in the settings screen
    const char* tab;       // loc ref of the page; null = not shown
    const char* choices;   // newline-separated loc refs for a Choice
};

#define AVER_ACC_TAB_A "@UI:settings.tab.access=Accessibility"
#define AVER_ACC_TAB_C "@UI:settings.tab.captions=Captions and Speech"
#define AVER_ACC_TAB_I "@UI:settings.tab.input=Input Assist"

inline constexpr AccessKey kAccessKeys[] = {
    {"graphics.uiScale",          AccessKind::Slider, 0.75f, 2.0f, 0.05f, 1.0f, "@UI:settings.uiScale=UI scale", AVER_ACC_TAB_A, ""},
    {"access.ui.textScale",       AccessKind::Slider, 0.75f, 2.0f, 0.05f, 1.0f, "@UI:settings.access.textScale=Text size", AVER_ACC_TAB_A, ""},
    {"access.ui.highContrast",    AccessKind::Toggle, 0, 1, 1, 0, "@UI:settings.access.highContrast=High contrast UI", AVER_ACC_TAB_A, ""},
    {"access.colour.mode",        AccessKind::Choice, 0, 3, 1, 0, "@UI:settings.access.colourMode=Colour vision filter", AVER_ACC_TAB_A,
     "@UI:settings.access.cv.off=Off\n@UI:settings.access.cv.prot=Protanopia\n@UI:settings.access.cv.deut=Deuteranopia\n@UI:settings.access.cv.trit=Tritanopia"},
    {"access.colour.strength",    AccessKind::Slider, 0, 1, 0.05f, 1.0f, "@UI:settings.access.colourStrength=Filter strength", AVER_ACC_TAB_A, ""},
    {"access.colour.simulate",    AccessKind::Toggle, 0, 1, 1, 0, nullptr, nullptr, ""},   // developer preview: editor and C# only
    {"access.motion.reduce",      AccessKind::Toggle, 0, 1, 1, 0, "@UI:settings.access.reduceMotion=Reduce motion", AVER_ACC_TAB_A, ""},
    {"access.motion.shake",       AccessKind::Slider, 0, 1, 0.05f, 1.0f, "@UI:settings.access.shake=Camera shake", AVER_ACC_TAB_A, ""},
    {"access.motion.bob",         AccessKind::Slider, 0, 1, 0.05f, 1.0f, "@UI:settings.access.bob=Head bob", AVER_ACC_TAB_A, ""},
    {"access.motion.blur",        AccessKind::Toggle, 0, 1, 1, 1, "@UI:settings.access.blur=Motion blur", AVER_ACC_TAB_A, ""},
    {"access.motion.lens",        AccessKind::Toggle, 0, 1, 1, 1, "@UI:settings.access.lens=Chromatic aberration, grain and lens flare", AVER_ACC_TAB_A, ""},
    {"access.captions.mode",      AccessKind::Choice, 0, 2, 1, 1, "@UI:settings.access.captionMode=Captions", AVER_ACC_TAB_C,
     "@UI:settings.access.cap.off=Off\n@UI:settings.access.cap.subtitles=Subtitles\n@UI:settings.access.cap.captions=Captions"},
    {"access.captions.size",      AccessKind::Choice, 0, 3, 1, 1, "@UI:settings.access.captionSize=Caption size", AVER_ACC_TAB_C,
     "@UI:settings.access.size.small=Small\n@UI:settings.access.size.medium=Medium\n@UI:settings.access.size.large=Large\n@UI:settings.access.size.xl=Extra large"},
    {"access.captions.bgOpacity", AccessKind::Slider, 0, 1, 0.05f, 0.6f, "@UI:settings.access.captionBg=Caption background", AVER_ACC_TAB_C, ""},
    {"access.captions.position",  AccessKind::Choice, 0, 1, 1, 0, "@UI:settings.access.captionPos=Caption position", AVER_ACC_TAB_C,
     "@UI:settings.access.pos.bottom=Bottom\n@UI:settings.access.pos.top=Top"},
    {"access.captions.lines",     AccessKind::Slider, 1, 4, 1, 3, "@UI:settings.access.captionLines=Caption lines", AVER_ACC_TAB_C, ""},
    {"access.captions.speaker",   AccessKind::Toggle, 0, 1, 1, 1, "@UI:settings.access.captionSpeaker=Speaker names", AVER_ACC_TAB_C, ""},
    {"access.captions.colours",   AccessKind::Toggle, 0, 1, 1, 1, "@UI:settings.access.captionColours=Speaker colours", AVER_ACC_TAB_C, ""},
    {"access.captions.direction", AccessKind::Toggle, 0, 1, 1, 0, "@UI:settings.access.captionDir=Direction hints", AVER_ACC_TAB_C, ""},
    {"access.speech.enabled",     AccessKind::Toggle, 0, 1, 1, 0, "@UI:settings.access.speech=Text to speech", AVER_ACC_TAB_C, ""},
    {"access.speech.ui",          AccessKind::Toggle, 0, 1, 1, 1, "@UI:settings.access.speechUi=Read menus aloud", AVER_ACC_TAB_C, ""},
    {"access.speech.captions",    AccessKind::Toggle, 0, 1, 1, 0, "@UI:settings.access.speechCaptions=Read captions aloud", AVER_ACC_TAB_C, ""},
    {"access.speech.rate",        AccessKind::Slider, -10, 10, 1, 0, "@UI:settings.access.speechRate=Speech rate", AVER_ACC_TAB_C, ""},
    {"access.speech.volume",      AccessKind::Slider, 0, 1, 0.05f, 1.0f, "@UI:settings.access.speechVolume=Speech volume", AVER_ACC_TAB_C, ""},
    {"access.input.holdScale",    AccessKind::Slider, 0.25f, 1, 0.05f, 1.0f, "@UI:settings.access.holdScale=Hold time", AVER_ACC_TAB_I, ""},
    {"access.input.windowScale",  AccessKind::Slider, 1, 4, 0.25f, 1.0f, "@UI:settings.access.windowScale=Tap and combo window", AVER_ACC_TAB_I, ""},
};

#undef AVER_ACC_TAB_A
#undef AVER_ACC_TAB_C
#undef AVER_ACC_TAB_I

inline constexpr usize kAccessKeyCount = sizeof(kAccessKeys) / sizeof(kAccessKeys[0]);
inline usize accessKeyCount() { return kAccessKeyCount; }

inline constexpr std::string_view kToggleKeyPrefix = "access.input.toggle.";

// access.input.toggle.<Action>: a dynamic Toggle per registered Digital action, not in the static table.
inline const AccessKey& accessDynamicToggle() {
    static const AccessKey k{"access.input.toggle.", AccessKind::Toggle, 0, 1, 1, 0, nullptr, nullptr, ""};
    return k;
}

// Null for an unknown key. The dynamic toggle prefix answers the shared dynamic row (a non-empty action name required).
inline const AccessKey* accessFind(std::string_view key) {
    for (const AccessKey& k : kAccessKeys) if (key == k.key) return &k;
    if (key.size() > kToggleKeyPrefix.size() && key.substr(0, kToggleKeyPrefix.size()) == kToggleKeyPrefix)
        return &accessDynamicToggle();
    return nullptr;
}

// Index into AccessState::v; kAccessKeyCount for the dynamic toggle or null.
inline usize accessIndex(const AccessKey* k) {
    if (!k || k == &accessDynamicToggle()) return kAccessKeyCount;
    return static_cast<usize>(k - kAccessKeys);
}

// Clamp, snap to step from lo, Toggle -> 0/1, Choice -> integer. NaN becomes the default.
inline f32 accessClamp(const AccessKey& k, f32 v) {
    if (!(v == v)) return k.def;
    if (k.kind == AccessKind::Toggle) return v > 0.5f ? 1.0f : 0.0f;
    if (v < k.lo) v = k.lo;
    if (v > k.hi) v = k.hi;
    if (k.kind == AccessKind::Choice) return std::floor(v + 0.5f);
    if (k.step > 0) {
        v = k.lo + std::floor((v - k.lo) / k.step + 0.5f) * k.step;
        if (v < k.lo) v = k.lo;
        if (v > k.hi) v = k.hi;
    }
    return v;
}

// Caption size preset -> text scale (Small, Medium, Large, Extra large).
inline f32 captionSizeScale(f32 preset) {
    static constexpr f32 s[4] = {0.8f, 1.0f, 1.4f, 1.9f};
    const int i = static_cast<int>(preset + 0.5f);
    return s[i < 0 ? 0 : (i > 3 ? 3 : i)];
}

// Twin of aver::rhi::packColourVision: mode + 4 * simulate + 8 * round(strength * 100). 0 when the mode is Off.
inline f32 accessPackColourVision(int mode, bool simulate, f32 strength) {
    if (mode <= 0 || mode > 3) return 0.0f;
    const f32 s = strength < 0 ? 0 : (strength > 1 ? 1 : strength);
    return static_cast<f32>(mode + (simulate ? 4 : 0) + 8 * static_cast<int>(std::floor(s * 100.0f + 0.5f)));
}

struct AccessState {
    std::array<f32, kAccessKeyCount> v{};
    std::unordered_map<std::string, bool> actionToggle;
    u32 revision = 0;
    std::vector<std::pair<std::string, f32>> dirty;   // accepted changes not yet drained, oldest first

    AccessState() { resetDefaults(); }

    void resetDefaults() {
        for (usize i = 0; i < kAccessKeyCount; ++i) v[i] = kAccessKeys[i].def;
        actionToggle.clear();
    }

    f32 get(std::string_view key) const {
        const AccessKey* k = accessFind(key);
        if (!k) return 0;
        const usize i = accessIndex(k);
        if (i < kAccessKeyCount) return v[i];
        const auto it = actionToggle.find(std::string(key.substr(kToggleKeyPrefix.size())));
        return it != actionToggle.end() && it->second ? 1.0f : 0.0f;
    }
    bool toggleOn(std::string_view action) const {
        const auto it = actionToggle.find(std::string(action));
        return it != actionToggle.end() && it->second;
    }

    // Stores without touching dirty or revision (loading). False for an unknown key.
    bool put(std::string_view key, f32 value) {
        const AccessKey* k = accessFind(key);
        if (!k) return false;
        const f32 c = accessClamp(*k, value);
        const usize i = accessIndex(k);
        if (i < kAccessKeyCount) v[i] = c;
        else actionToggle[std::string(key.substr(kToggleKeyPrefix.size()))] = c > 0.5f;
        return true;
    }

    // Accepted set: clamps, and when the value changed marks the key dirty and bumps the revision. False for an unknown key.
    bool set(std::string_view key, f32 value) {
        const AccessKey* k = accessFind(key);
        if (!k) return false;
        const f32 before = get(key);
        put(key, value);
        const f32 after = get(key);
        if (after != before) {
            ++revision;
            for (auto& d : dirty) if (d.first == key) { d.second = after; return true; }
            dirty.emplace_back(std::string(key), after);
        }
        return true;
    }
};

namespace detail {
inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
inline std::string lower(std::string_view s) {
    std::string o(s);
    for (char& c : o) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}
// "1.000000", "3", "true"/"false"/"yes"/"no"/"on"/"off". False when it is neither.
inline bool parseValue(std::string_view s, f32& out) {
    s = trim(s);
    if (s.empty()) return false;
    const std::string w = lower(s);
    if (w == "true" || w == "yes" || w == "on") { out = 1.0f; return true; }
    if (w == "false" || w == "no" || w == "off") { out = 0.0f; return true; }
    const std::string z(s);
    char* end = nullptr;
    const f32 f = std::strtof(z.c_str(), &end);
    if (end == z.c_str() || *end != '\0') return false;
    out = f;
    return true;
}
} // namespace detail

// key=value lines ('#' comments). Applies every known key (loading: not dirty), ignores the rest. Returns the number read.
inline usize accessParseIni(std::string_view text, AccessState& st) {
    usize n = 0, i = 0;
    while (i <= text.size()) {
        const usize e = text.find('\n', i);
        const std::string_view line = detail::trim(text.substr(i, (e == std::string_view::npos ? text.size() : e) - i));
        i = e == std::string_view::npos ? text.size() + 1 : e + 1;
        if (line.empty() || line[0] == '#') continue;
        const usize eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        f32 v = 0;
        if (!detail::parseValue(line.substr(eq + 1), v)) continue;
        if (st.put(detail::trim(line.substr(0, eq)), v)) ++n;
    }
    return n;
}

// What hosts consume each frame; "reduce motion" is already applied (a gate, not a preset: the sliders keep their values).
struct AccessEffective {
    f32 colourVisionPacked = 0, uiScale = 1, textScale = 1, highContrast = 0;
    f32 shake = 1, bob = 1, motionBlurAllowed = 1, lensAllowed = 1;
};

inline AccessEffective accessEffective(const AccessState& s) {
    const bool reduce = s.get("access.motion.reduce") > 0.5f;
    AccessEffective e;
    e.colourVisionPacked = accessPackColourVision(static_cast<int>(s.get("access.colour.mode")), s.get("access.colour.simulate") > 0.5f,
                                                  s.get("access.colour.strength"));
    e.uiScale = s.get("graphics.uiScale");
    e.textScale = s.get("access.ui.textScale");
    e.highContrast = s.get("access.ui.highContrast");
    e.shake = reduce ? 0.0f : s.get("access.motion.shake");
    e.bob = reduce ? 0.0f : s.get("access.motion.bob");
    e.motionBlurAllowed = reduce ? 0.0f : s.get("access.motion.blur");
    e.lensAllowed = reduce ? 0.0f : s.get("access.motion.lens");
    return e;
}

} // namespace aver::fw::access
