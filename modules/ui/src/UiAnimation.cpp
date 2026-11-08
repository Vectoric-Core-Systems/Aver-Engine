// Widget animation data: easing, record parsing and writing, sampling. The player is UiTree (UiTreeRuntime.cpp).
#include "aver/ui/UiAnimation.hpp"

#include "aver/ui/UiLayoutAsset.hpp"
#include "aver/ui/UiProps.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace aver::ui {

namespace {

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

struct EaseName { const char* name; UiEase e; };
constexpr EaseName kEases[] = {
    {"linear", UiEase::Linear},     {"step", UiEase::Step},         {"in", UiEase::QuadIn},
    {"out", UiEase::QuadOut},       {"inout", UiEase::QuadInOut},   {"cubicin", UiEase::CubicIn},
    {"cubicout", UiEase::CubicOut}, {"cubicinout", UiEase::CubicInOut},
};

struct PropName { const char* name; UiAnimProp p; };
constexpr PropName kProps[] = {
    {"opacity", UiAnimProp::Opacity},
    {"bgColor", UiAnimProp::BgColor},       {"color", UiAnimProp::BgColor},      {"background", UiAnimProp::BgColor},
    {"textColor", UiAnimProp::TextColor},
    {"accentColor", UiAnimProp::AccentColor}, {"accent", UiAnimProp::AccentColor},
    {"tint", UiAnimProp::Tint},
    {"renderTranslate", UiAnimProp::Translate}, {"offset", UiAnimProp::Translate}, {"translate", UiAnimProp::Translate},
    {"renderRotation", UiAnimProp::Rotation},   {"rotation", UiAnimProp::Rotation}, {"angle", UiAnimProp::Rotation},
    {"renderScale", UiAnimProp::Scale},         {"scale", UiAnimProp::Scale},
};

bool parseHex(std::string_view s, f32 out[4]) {
    if ((s.size() != 7 && s.size() != 9) || s[0] != '#') return false;
    out[3] = 255;
    const usize n = (s.size() - 1) / 2;
    for (usize i = 0; i < n; ++i) {
        const std::string pair(s.substr(1 + i * 2, 2));
        char* end = nullptr;
        const unsigned long v = std::strtoul(pair.c_str(), &end, 16);
        if (end != pair.c_str() + 2) return false;
        out[i] = static_cast<f32>(v);
    }
    return true;
}

} // namespace

const char* uiEaseName(UiEase e) {
    for (const EaseName& n : kEases) if (n.e == e) return n.name;
    return "linear";
}

bool uiEaseFromName(std::string_view s, UiEase& out) {
    for (const EaseName& n : kEases) if (ieq(s, n.name)) { out = n.e; return true; }
    return false;
}

f32 uiEaseApply(UiEase e, f32 x) {
    x = std::clamp(x, 0.0f, 1.0f);
    switch (e) {
        case UiEase::Linear: return x;
        case UiEase::Step: return x >= 1.0f ? 1.0f : 0.0f;
        case UiEase::QuadIn: return x * x;
        case UiEase::QuadOut: return 1.0f - (1.0f - x) * (1.0f - x);
        case UiEase::QuadInOut: return x < 0.5f ? 2.0f * x * x : 1.0f - 2.0f * (1.0f - x) * (1.0f - x);
        case UiEase::CubicIn: return x * x * x;
        case UiEase::CubicOut: { const f32 y = 1.0f - x; return 1.0f - y * y * y; }
        case UiEase::CubicInOut: { const f32 y = 1.0f - x; return x < 0.5f ? 4.0f * x * x * x : 1.0f - 4.0f * y * y * y; }
    }
    return x;
}

UiAnimKind uiAnimPropKind(UiAnimProp p) {
    switch (p) {
        case UiAnimProp::Opacity: case UiAnimProp::Rotation: return UiAnimKind::Float;
        case UiAnimProp::Translate: case UiAnimProp::Scale: return UiAnimKind::Vec2;
        default: return UiAnimKind::Colour;
    }
}

const char* uiAnimPropName(UiAnimProp p) {
    for (const PropName& n : kProps) if (n.p == p) return n.name;
    return "opacity";
}

bool uiAnimPropFromName(std::string_view s, UiAnimProp& out) {
    for (const PropName& n : kProps) if (ieq(s, n.name)) { out = n.p; return true; }
    return false;
}

f32 UiAnimation::length() const {
    f32 len = 0;
    for (const UiAnimTrack& t : tracks) if (!t.keys.empty()) len = std::max(len, t.keys.back().time);
    return len;
}

bool uiAnimSample(const UiAnimTrack& track, f32 t, UiAnimValue& out) {
    const std::vector<UiAnimKey>& K = track.keys;
    if (K.empty()) return false;
    const usize n = uiAnimPropKind(track.prop) == UiAnimKind::Float ? 1 : uiAnimPropKind(track.prop) == UiAnimKind::Vec2 ? 2 : 4;
    const auto copy = [&](const UiAnimKey& k) { for (usize i = 0; i < 4; ++i) out.v[i] = i < n ? k.v[i] : 0.0f; };
    if (t <= K.front().time) { copy(K.front()); return true; }
    if (t >= K.back().time) { copy(K.back()); return true; }
    usize i = 0;
    while (i + 1 < K.size() && K[i + 1].time <= t) ++i;
    const UiAnimKey& a = K[i];
    const UiAnimKey& b = K[i + 1];
    const f32 span = b.time - a.time;
    const f32 u = span > 0.0f ? uiEaseApply(a.ease, (t - a.time) / span) : 1.0f;
    for (usize c = 0; c < 4; ++c) out.v[c] = c < n ? a.v[c] + (b.v[c] - a.v[c]) * u : 0.0f;
    return true;
}

std::string uiAnimKeyToken(UiAnimProp p, const UiAnimKey& k) {
    std::string s;
    switch (uiAnimPropKind(p)) {
        case UiAnimKind::Float: s = uiFormatFloat(k.v[0]); break;
        case UiAnimKind::Vec2: s = uiFormatFloat(k.v[0]) + "," + uiFormatFloat(k.v[1]); break;
        case UiAnimKind::Colour: {
            char buf[16];
            const auto q = [](f32 c) { return static_cast<unsigned>(std::clamp(std::lround(c), 0l, 255l)); };
            std::snprintf(buf, sizeof buf, "#%02X%02X%02X%02X", q(k.v[0]), q(k.v[1]), q(k.v[2]), q(k.v[3]));
            s = buf;
            break;
        }
    }
    if (k.ease != UiEase::Linear) s += std::string("@") + uiEaseName(k.ease);
    return s;
}

bool uiAnimKeyFromToken(UiAnimProp p, std::string_view token, UiAnimKey& out) {
    UiAnimKey k = out;
    const usize at = token.find('@');
    k.ease = UiEase::Linear;
    if (at != std::string_view::npos) {
        if (!uiEaseFromName(token.substr(at + 1), k.ease)) return false;
        token = token.substr(0, at);
    }
    switch (uiAnimPropKind(p)) {
        case UiAnimKind::Float:
            if (!uiParseFloat(token, k.v[0])) return false;
            break;
        case UiAnimKind::Vec2: {
            const usize comma = token.find(',');
            if (comma == std::string_view::npos) {
                if (!uiParseFloat(token, k.v[0])) return false;
                k.v[1] = k.v[0];
            } else if (!uiParseFloat(token.substr(0, comma), k.v[0]) || !uiParseFloat(token.substr(comma + 1), k.v[1])) {
                return false;
            }
            break;
        }
        case UiAnimKind::Colour:
            if (!parseHex(token, k.v)) return false;
            break;
    }
    out = k;
    return true;
}

bool uiAnimTrackFromRecord(const UiLayoutAnim& rec, UiAnimTrack& out, std::string* err) {
    const auto fail = [&](const std::string& why) { if (err) *err = why; return false; };
    UiAnimTrack t;
    t.widget = rec.widget;
    if (!uiAnimPropFromName(rec.property, t.prop)) return fail("unknown animation property '" + rec.property + "'");
    if (rec.keys.size() % 2 != 0) return fail("ANIM keys come as <time> <value> pairs");
    for (usize i = 0; i < rec.keys.size(); i += 2) {
        UiAnimKey k;
        if (!uiParseFloat(rec.keys[i], k.time) || k.time < 0.0f) return fail("bad key time '" + rec.keys[i] + "'");
        if (!uiAnimKeyFromToken(t.prop, rec.keys[i + 1], k)) return fail("bad key value '" + rec.keys[i + 1] + "'");
        if (!t.keys.empty() && k.time < t.keys.back().time) return fail("key times must not go backwards");
        t.keys.push_back(k);
    }
    out = std::move(t);
    return true;
}

UiLayoutAnim uiAnimTrackToRecord(std::string_view animName, const UiAnimTrack& track) {
    UiLayoutAnim r;
    r.name = std::string(animName);
    r.widget = track.widget;
    r.property = uiAnimPropName(track.prop);
    for (const UiAnimKey& k : track.keys) {
        r.keys.push_back(uiFormatFloat(k.time));
        r.keys.push_back(uiAnimKeyToken(track.prop, k));
    }
    return r;
}

std::vector<UiAnimation> uiAnimationsFromDoc(const UiLayoutDoc& doc, std::vector<std::string>* errors) {
    std::vector<UiAnimation> out;
    for (const UiLayoutAnim& rec : doc.anims) {
        UiAnimTrack t;
        std::string why;
        if (!uiAnimTrackFromRecord(rec, t, &why)) {
            if (errors) errors->push_back("ANIM " + rec.name + " " + rec.widget + " " + rec.property + ": " + why);
            continue;
        }
        auto it = std::find_if(out.begin(), out.end(), [&](const UiAnimation& a) { return a.name == rec.name; });
        if (it == out.end()) { out.push_back({rec.name, {}}); it = out.end() - 1; }
        it->tracks.push_back(std::move(t));
    }
    return out;
}

void uiAnimApply(UiWidget& w, UiAnimProp p, const UiAnimValue& v) {
    UiWidgetFx& fx = w.fx;
    const auto pack = [&](u32 UiWidgetFx::*m, u32 bit) {
        const auto q = [](f32 c) { return static_cast<u32>(std::clamp(std::lround(c), 0l, 255l)); };
        fx.*m = uiRgba(q(v.v[0]), q(v.v[1]), q(v.v[2]), q(v.v[3]));
        fx.mask |= bit;
    };
    switch (p) {
        case UiAnimProp::Opacity: fx.opacity = std::clamp(v.v[0], 0.0f, 1.0f); fx.mask |= kFxOpacity; break;
        case UiAnimProp::BgColor: pack(&UiWidgetFx::bg, kFxBg); break;
        case UiAnimProp::TextColor: pack(&UiWidgetFx::text, kFxText); break;
        case UiAnimProp::AccentColor: pack(&UiWidgetFx::accent, kFxAccent); break;
        case UiAnimProp::Tint: pack(&UiWidgetFx::tint, kFxTint); break;
        case UiAnimProp::Translate: fx.translate = {v.v[0], v.v[1]}; fx.mask |= kFxTranslate; break;
        case UiAnimProp::Rotation: fx.rotation = v.v[0]; fx.mask |= kFxRotation; break;
        case UiAnimProp::Scale: fx.scale = {v.v[0], v.v[1]}; fx.mask |= kFxScale; break;
        case UiAnimProp::Count: break;
    }
}

void uiAnimRelease(UiWidget& w, UiAnimProp p) {
    static constexpr u32 kBits[] = {kFxOpacity, kFxBg, kFxText, kFxAccent, kFxTint, kFxTranslate, kFxRotation, kFxScale};
    if (p < UiAnimProp::Count) w.fx.mask &= ~kBits[static_cast<usize>(p)];
}

} // namespace aver::ui
