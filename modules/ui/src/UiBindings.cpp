// Binding paths, value formatting and event-route names. The evaluation loop lives in UiTreeRuntime.cpp.
#include "aver/ui/UiBindings.hpp"

#include "aver/ui/UiProps.hpp"
#include "aver/ui/UiTree.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace aver::ui {

namespace {

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

std::string fixed(double x, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", std::clamp(decimals, 0, 12), x);
    return buf;
}

std::string shortest(double x) {
    if (std::isnan(x)) return "nan";
    if (std::fabs(x) < 1e15 && x == std::floor(x)) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(x));
        return buf;
    }
    return uiFormatFloat(static_cast<f32>(x));
}

std::string grouped(double x, int decimals) {
    std::string s = fixed(std::fabs(x), decimals);
    const usize dot = s.find('.');
    const usize intLen = dot == std::string::npos ? s.size() : dot;
    std::string out;
    for (usize i = 0; i < intLen; ++i) {
        if (i && (intLen - i) % 3 == 0) out.push_back(',');
        out.push_back(s[i]);
    }
    out += s.substr(intLen);
    return (x < 0 ? "-" : "") + out;
}

int digitsAfter(std::string_view s, usize from) {
    int n = 0;
    for (usize i = from; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return -1;
        n = n * 10 + (s[i] - '0');
    }
    return n;
}

std::string numberSpec(double x, std::string_view spec) {
    if (spec.empty()) return shortest(x);
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(spec[0])));
    const int n = spec.size() > 1 ? digitsAfter(spec, 1) : 0;
    if (n >= 0 && (c == 'f' || c == 'd' || c == 'p' || c == 'n')) {
        if (c == 'f') return fixed(x, spec.size() > 1 ? n : 2);
        if (c == 'p') return fixed(x * 100.0, spec.size() > 1 ? n : 0) + "%";
        if (c == 'n') return grouped(x, spec.size() > 1 ? n : 0);
        std::string s = fixed(std::fabs(std::round(x)), 0);
        while (static_cast<int>(s.size()) < n) s.insert(s.begin(), '0');
        return (x < 0 ? "-" : "") + s;
    }
    // A custom pattern: 0s before the dot pad, 0s after it are fixed decimals, #s after it are optional ones.
    const usize dot = spec.find('.');
    const std::string_view head = spec.substr(0, dot);
    const std::string_view tail = dot == std::string_view::npos ? std::string_view{} : spec.substr(dot + 1);
    if (spec.find_first_not_of("0#.") != std::string_view::npos) return shortest(x);
    const int zeros = static_cast<int>(std::count(tail.begin(), tail.end(), '0'));
    const int optional = static_cast<int>(std::count(tail.begin(), tail.end(), '#'));
    std::string s = fixed(std::fabs(x), zeros + optional);
    if (optional > 0) {
        usize end = s.size();
        const usize d = s.find('.');
        while (end > d + 1 + static_cast<usize>(zeros) && s[end - 1] == '0') --end;
        if (end == d + 1) --end;
        s.resize(end);
    }
    const int pad = static_cast<int>(std::count(head.begin(), head.end(), '0'));
    const usize d2 = s.find('.');
    usize intLen = d2 == std::string::npos ? s.size() : d2;
    while (static_cast<int>(intLen) < pad) { s.insert(s.begin(), '0'); ++intLen; }
    return (x < 0 ? "-" : "") + s;
}

std::string applySpec(const UiBindValue& v, std::string_view spec) {
    switch (v.kind) {
        case UiBindValue::Kind::None: return {};
        case UiBindValue::Kind::Text: {
            std::string s = v.text;
            if (spec == "U") for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            else if (spec == "L") for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return s;
        }
        case UiBindValue::Kind::Bool: {
            const usize bar = spec.find('|');
            if (bar != std::string_view::npos) return std::string(v.number != 0 ? spec.substr(0, bar) : spec.substr(bar + 1));
            return spec.empty() ? (v.number != 0 ? "true" : "false") : numberSpec(v.number, spec);
        }
        case UiBindValue::Kind::Number: return numberSpec(v.number, spec);
    }
    return {};
}

} // namespace

bool uiParseBindPath(std::string_view text, UiBindSource& source, std::string& path) {
    source = UiBindSource::ViewModel;
    if (text.size() > 3 && ieq(text.substr(0, 3), "vm:")) text.remove_prefix(3);
    else if (text.size() > 4 && ieq(text.substr(0, 4), "var:")) { source = UiBindSource::Variable; text.remove_prefix(4); }
    path = std::string(text);
    return !path.empty();
}

std::string uiBindPathText(UiBindSource source, std::string_view path) {
    return source == UiBindSource::Variable ? "var:" + std::string(path) : "vm:" + std::string(path);
}

std::string uiFormatBound(const UiBindValue& v, std::string_view format) {
    if (format.empty()) return applySpec(v, {});
    std::string out;
    bool used = false;
    for (usize i = 0; i < format.size(); ++i) {
        const char c = format[i];
        if (c == '{' && i + 1 < format.size() && format[i + 1] == '{') { out.push_back('{'); ++i; continue; }
        if (c == '}' && i + 1 < format.size() && format[i + 1] == '}') { out.push_back('}'); ++i; continue; }
        if (c == '{' && !used) {
            const usize close = format.find('}', i);
            if (close != std::string_view::npos) {
                std::string_view inner = format.substr(i + 1, close - i - 1);
                const usize colon = inner.find(':');
                const std::string_view spec = colon == std::string_view::npos ? std::string_view{} : inner.substr(colon + 1);
                out += applySpec(v, spec);
                used = true;
                i = close;
                continue;
            }
        }
        out.push_back(c);
    }
    if (!used) out += applySpec(v, {});
    return out;
}

const char* uiRouteActionName(UiRouteAction a) {
    switch (a) {
        case UiRouteAction::Command: return "command";
        case UiRouteAction::Graph: return "graph";
        case UiRouteAction::Method: return "method";
    }
    return "command";
}

bool uiRouteActionFromName(std::string_view s, UiRouteAction& out) {
    if (ieq(s, "command")) { out = UiRouteAction::Command; return true; }
    if (ieq(s, "graph")) { out = UiRouteAction::Graph; return true; }
    if (ieq(s, "method")) { out = UiRouteAction::Method; return true; }
    return false;
}

namespace {
struct EvName { const char* name; UiEventType type; bool enter; };
constexpr EvName kEvents[] = {
    {"click", UiEventType::Clicked, true},          {"toggled", UiEventType::Toggled, true},
    {"valuechanged", UiEventType::ValueChanged, true}, {"selectionchanged", UiEventType::SelectionChanged, true},
    {"textchanged", UiEventType::TextChanged, true}, {"submitted", UiEventType::TextSubmitted, true},
    {"focus", UiEventType::FocusGained, true},      {"blur", UiEventType::FocusLost, true},
    {"hover", UiEventType::Hovered, true},          {"unhover", UiEventType::Hovered, false},
    {"cancel", UiEventType::Cancel, true},          {"command", UiEventType::Command, true},
    {"finished", UiEventType::AnimationFinished, true},
};
} // namespace

bool uiRouteEventFromName(std::string_view name, u8& type, bool& hoverEnter) {
    for (const EvName& e : kEvents)
        if (ieq(name, e.name)) { type = static_cast<u8>(e.type); hoverEnter = e.enter; return true; }
    return false;
}

const char* uiRouteEventName(u8 type, bool hoverEnter) {
    for (const EvName& e : kEvents)
        if (static_cast<u8>(e.type) == type && e.enter == hoverEnter) return e.name;
    return "click";
}

} // namespace aver::ui
