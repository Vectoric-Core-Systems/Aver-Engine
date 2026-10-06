// .ocblend / .ocasm read and write. See AnimGraphAsset.hpp.
#include "aver/anim/AnimGraphAsset.hpp"
#include "aver/formats/Json.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace aver::anim {

namespace {

std::string esc(std::string_view s) {
    std::string o = "\"";
    for (const char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += c;
                }
        }
    }
    return o + "\"";
}

std::string num(f32 v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.9g", static_cast<double>(v));
    return b;
}

const char* opName(AsmOp o) {
    static const char* n[] = {"gt", "ge", "lt", "le", "eq", "ne", "true", "false", "trigger"};
    return n[static_cast<int>(o)];
}

bool opFrom(std::string_view s, AsmOp& o) {
    for (int i = 0; i <= static_cast<int>(AsmOp::Trigger); ++i)
        if (s == opName(static_cast<AsmOp>(i))) { o = static_cast<AsmOp>(i); return true; }
    return false;
}

const char* paramTypeName(AsmParamType t) {
    static const char* n[] = {"float", "int", "bool", "trigger"};
    return n[static_cast<int>(t)];
}

const char* kindName(AsmStateKind k) {
    static const char* n[] = {"clip", "blendspace", "machine"};
    return n[static_cast<int>(k)];
}

std::string str(const fmt::JsonValue& v, std::string_view key) { return std::string(v[key].asString()); }

bool readFile(const std::string& path, std::string& out, std::string* why) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (why) *why = "cannot open " + path; return false; }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool writeFile(const std::string& path, const std::string& text, std::string* why) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { if (why) *why = "cannot write " + path; return false; }
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(f);
}

bool header(const fmt::JsonValue& root, const char* tag, int version, std::string* why) {
    if (!root.isObject() || root["format"].asString() != tag) {
        if (why) *why = std::string("not a ") + tag + " file";
        return false;
    }
    if (root["version"].asInt() > version) {
        if (why) *why = "file version is newer than this build";
        return false;
    }
    return true;
}

} // namespace

std::string writeBlendSpace(const BlendSpaceAsset& a) {
    std::string s = "{\n  \"format\": \"ocblend\",\n  \"version\": " + std::to_string(kBlendSpaceVersion) + ",\n";
    s += "  \"name\": " + esc(a.name) + ",\n  \"dims\": " + std::to_string(a.dims) + ",\n";
    s += std::string("  \"syncMarkers\": ") + (a.syncMarkers ? "true" : "false") + ",\n";
    auto axis = [&](const char* key, const BlendAxis& x) {
        return std::string("  \"") + key + "\": {\"name\": " + esc(x.name) + ", \"min\": " + num(x.min) +
               ", \"max\": " + num(x.max) + ", \"smoothing\": " + num(x.smoothing) + "},\n";
    };
    s += axis("axisX", a.axisX) + axis("axisY", a.axisY) + "  \"samples\": [";
    for (usize i = 0; i < a.samples.size(); ++i) {
        const BlendSample& m = a.samples[i];
        s += i ? ",\n    " : "\n    ";
        s += "{\"clip\": " + esc(m.clip) + ", \"x\": " + num(m.x) + ", \"y\": " + num(m.y) +
             ", \"rate\": " + num(m.rate) + ", \"markers\": [";
        for (usize k = 0; k < m.markers.size(); ++k) {
            if (k) s += ", ";
            s += "{\"name\": " + esc(m.markers[k].name) + ", \"time\": " + num(m.markers[k].time) + "}";
        }
        s += "]}";
    }
    s += a.samples.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return s;
}

bool parseBlendSpace(std::string_view json, BlendSpaceAsset& out, std::string* why) {
    fmt::JsonValue root;
    if (!fmt::parseJson(json, root, why)) return false;
    if (!header(root, "ocblend", kBlendSpaceVersion, why)) return false;
    BlendSpaceAsset a;
    a.name = str(root, "name");
    a.dims = static_cast<u8>(root["dims"].asInt(1));
    a.syncMarkers = root["syncMarkers"].asBool(true);
    auto axis = [&](const fmt::JsonValue& v, BlendAxis& x) {
        x.name = str(v, "name");
        x.min = v["min"].asFloat(0.0f);
        x.max = v["max"].asFloat(1.0f);
        x.smoothing = v["smoothing"].asFloat(0.0f);
    };
    axis(root["axisX"], a.axisX);
    axis(root["axisY"], a.axisY);
    const fmt::JsonValue& samples = root["samples"];
    for (usize i = 0; i < samples.size(); ++i) {
        const fmt::JsonValue& v = samples[i];
        BlendSample m;
        m.clip = str(v, "clip");
        m.x = v["x"].asFloat();
        m.y = v["y"].asFloat();
        m.rate = v["rate"].asFloat(1.0f);
        const fmt::JsonValue& mk = v["markers"];
        for (usize k = 0; k < mk.size(); ++k) m.markers.push_back({str(mk[k], "name"), mk[k]["time"].asFloat()});
        a.samples.push_back(std::move(m));
    }
    out = std::move(a);
    return true;
}

bool saveBlendSpace(const std::string& path, const BlendSpaceAsset& a, std::string* why) {
    return writeFile(path, writeBlendSpace(a), why);
}

bool loadBlendSpace(const std::string& path, BlendSpaceAsset& out, std::string* why) {
    std::string text;
    return readFile(path, text, why) && parseBlendSpace(text, out, why);
}

std::string writeStateMachine(const AnimStateMachineAsset& a) {
    std::string s = "{\n  \"format\": \"ocasm\",\n  \"version\": " + std::to_string(kStateMachineVersion) + ",\n";
    s += "  \"name\": " + esc(a.name) + ",\n  \"params\": [";
    for (usize i = 0; i < a.params.size(); ++i) {
        const AsmParam& p = a.params[i];
        s += i ? ",\n    " : "\n    ";
        s += "{\"name\": " + esc(p.name) + ", \"type\": \"" + paramTypeName(p.type) + "\", \"default\": " + num(p.def) + "}";
    }
    s += a.params.empty() ? "],\n" : "\n  ],\n";
    s += "  \"machines\": [";
    for (usize mi = 0; mi < a.machines.size(); ++mi) {
        const AsmMachine& m = a.machines[mi];
        s += mi ? ",\n    {" : "\n    {";
        s += "\"name\": " + esc(m.name) + ", \"entry\": " + std::to_string(m.entry) + ",\n      \"states\": [";
        for (usize i = 0; i < m.states.size(); ++i) {
            const AsmState& st = m.states[i];
            s += i ? ",\n        " : "\n        ";
            s += "{\"name\": " + esc(st.name) + ", \"kind\": \"" + kindName(st.kind) + "\", \"asset\": " + esc(st.asset) +
                 ", \"machine\": " + std::to_string(st.subMachine) + ", \"speed\": " + num(st.speed) +
                 ", \"loop\": " + (st.loop ? "true" : "false") + ", \"speedParam\": " + esc(st.speedParam) +
                 ", \"xParam\": " + esc(st.xParam) + ", \"yParam\": " + esc(st.yParam) +
                 ", \"onEnter\": " + esc(st.onEnter) + ", \"onExit\": " + esc(st.onExit) +
                 ", \"pos\": [" + num(st.posX) + ", " + num(st.posY) + "]}";
        }
        s += m.states.empty() ? "],\n      \"transitions\": [" : "\n      ],\n      \"transitions\": [";
        for (usize i = 0; i < m.transitions.size(); ++i) {
            const AsmTransition& t = m.transitions[i];
            s += i ? ",\n        " : "\n        ";
            s += "{\"from\": " + std::to_string(t.from) + ", \"to\": " + std::to_string(t.to) +
                 ", \"blend\": " + num(t.blendTime) + ", \"exitTime\": " + (t.hasExitTime ? num(t.exitTime) : "-1") +
                 ", \"interruptible\": " + (t.interruptible ? "true" : "false") +
                 ", \"allowSelf\": " + (t.allowSelf ? "true" : "false") + ", \"when\": [";
            for (usize k = 0; k < t.conditions.size(); ++k) {
                const AsmCondition& c = t.conditions[k];
                if (k) s += ", ";
                s += "{\"param\": " + esc(c.param) + ", \"op\": \"" + opName(c.op) + "\", \"value\": " + num(c.value) + "}";
            }
            s += "]}";
        }
        s += m.transitions.empty() ? "]}" : "\n      ]}";
    }
    s += a.machines.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return s;
}

bool parseStateMachine(std::string_view json, AnimStateMachineAsset& out, std::string* why) {
    fmt::JsonValue root;
    if (!fmt::parseJson(json, root, why)) return false;
    if (!header(root, "ocasm", kStateMachineVersion, why)) return false;
    AnimStateMachineAsset a;
    a.name = str(root, "name");
    const fmt::JsonValue& ps = root["params"];
    for (usize i = 0; i < ps.size(); ++i) {
        AsmParam p;
        p.name = str(ps[i], "name");
        const std::string_view t = ps[i]["type"].asString("float");
        p.type = t == "int" ? AsmParamType::Int : t == "bool" ? AsmParamType::Bool
               : t == "trigger" ? AsmParamType::Trigger : AsmParamType::Float;
        p.def = ps[i]["default"].asFloat();
        a.params.push_back(std::move(p));
    }
    const fmt::JsonValue& ms = root["machines"];
    for (usize mi = 0; mi < ms.size(); ++mi) {
        const fmt::JsonValue& mv = ms[mi];
        AsmMachine m;
        m.name = str(mv, "name");
        m.entry = static_cast<i32>(mv["entry"].asInt());
        const fmt::JsonValue& sts = mv["states"];
        for (usize i = 0; i < sts.size(); ++i) {
            const fmt::JsonValue& v = sts[i];
            AsmState s;
            s.name = str(v, "name");
            const std::string_view k = v["kind"].asString("clip");
            s.kind = k == "blendspace" ? AsmStateKind::BlendSpace : k == "machine" ? AsmStateKind::SubMachine : AsmStateKind::Clip;
            s.asset = str(v, "asset");
            s.subMachine = static_cast<i32>(v["machine"].asInt(-1));
            s.speed = v["speed"].asFloat(1.0f);
            s.loop = v["loop"].asBool(true);
            s.speedParam = str(v, "speedParam");
            s.xParam = str(v, "xParam");
            s.yParam = str(v, "yParam");
            s.onEnter = str(v, "onEnter");
            s.onExit = str(v, "onExit");
            s.posX = v["pos"][static_cast<usize>(0)].asFloat();
            s.posY = v["pos"][static_cast<usize>(1)].asFloat();
            m.states.push_back(std::move(s));
        }
        const fmt::JsonValue& trs = mv["transitions"];
        for (usize i = 0; i < trs.size(); ++i) {
            const fmt::JsonValue& v = trs[i];
            AsmTransition t;
            t.from = static_cast<i32>(v["from"].asInt());
            t.to = static_cast<i32>(v["to"].asInt());
            t.blendTime = v["blend"].asFloat(0.2f);
            const f32 et = v["exitTime"].asFloat(-1.0f);
            t.hasExitTime = et >= 0.0f;
            t.exitTime = t.hasExitTime ? et : 0.9f;
            t.interruptible = v["interruptible"].asBool(true);
            t.allowSelf = v["allowSelf"].asBool(false);
            const fmt::JsonValue& cs = v["when"];
            for (usize k = 0; k < cs.size(); ++k) {
                AsmCondition c;
                c.param = str(cs[k], "param");
                if (!opFrom(cs[k]["op"].asString(), c.op)) {
                    if (why) *why = "unknown condition op";
                    return false;
                }
                c.value = cs[k]["value"].asFloat();
                t.conditions.push_back(std::move(c));
            }
            m.transitions.push_back(std::move(t));
        }
        a.machines.push_back(std::move(m));
    }
    out = std::move(a);
    return true;
}

bool saveStateMachine(const std::string& path, const AnimStateMachineAsset& a, std::string* why) {
    return writeFile(path, writeStateMachine(a), why);
}

bool loadStateMachine(const std::string& path, AnimStateMachineAsset& out, std::string* why) {
    std::string text;
    return readFile(path, text, why) && parseStateMachine(text, out, why);
}

} // namespace aver::anim
