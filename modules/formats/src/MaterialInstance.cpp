// Material instances and material functions (MaterialInstance.hpp).
#include "aver/formats/MaterialInstance.hpp"

#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>

namespace aver::fmt {
namespace {

bool ci(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

std::string_view trimView(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> words(std::string_view line) {
    std::vector<std::string_view> w;
    usize i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        const usize b = i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i > b) w.push_back(line.substr(b, i - b));
    }
    return w;
}

bool parseNumber(std::string_view s, f64& v) {
    const std::string t(s);
    char* end = nullptr;
    v = std::strtod(t.c_str(), &end);
    return end && end != t.c_str() && *end == 0;
}

// "a,b,c" -> true when it is 1..maxN numbers; *count gets how many.
bool numberList(std::string_view s, usize maxN, usize* count = nullptr) {
    usize n = 0, b = 0;
    while (b <= s.size()) {
        const usize e = std::min(s.find(',', b), s.size());
        f64 v;
        if (!parseNumber(s.substr(b, e - b), v)) return false;
        ++n;
        if (e >= s.size()) break;
        b = e + 1;
    }
    if (count) *count = n;
    return n >= 1 && n <= maxN;
}

std::string canonPath(std::string p) {
    for (char& c : p) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return p;
}

std::string_view nodeAttr(const OcGraphNode& n, std::string_view key) {
    for (const std::string& tok : n.extraTokens) {
        const usize eq = tok.find('=');
        if (eq != std::string::npos && ci(std::string_view(tok).substr(0, eq), key))
            return std::string_view(tok).substr(eq + 1);
    }
    return {};
}

} // namespace

// ------------------------------------------------------------------------------------- .ocmati

bool parseOcmati(std::string_view text, OcMatInstance& out, std::string* err) {
    out = OcMatInstance{};
    bool header = false;
    std::istringstream in{std::string(text)};
    std::string raw;
    while (std::getline(in, raw)) {
        const std::string_view line = trimView(raw);
        if (line.empty() || line.front() == '#') continue;
        const std::vector<std::string_view> w = words(line);
        const std::string_view key = w[0];
        if (ci(key, "OCMATI")) {
            if (w.size() > 1 && std::atoi(std::string(w[1]).c_str()) != kOcMatiVersion) {
                if (err) *err = "unsupported OCMATI version " + std::string(w[1]);
                return false;
            }
            header = true;
        } else if (!header) {
            break;
        } else if (ci(key, "NAME")) {
            out.name = std::string(trimView(line.substr(key.size())));
        } else if (ci(key, "PARENT")) {
            out.parent = std::string(trimView(line.substr(key.size())));
        } else if (ci(key, "SCALAR") || ci(key, "VECTOR")) {
            const bool vec = ci(key, "VECTOR");
            const usize want = vec ? 4 : 1;
            if (w.size() < 3 || w.size() - 2 > want) {
                if (err) *err = std::string(key) + " needs a name and " + (vec ? "1 to 4 numbers" : "one number");
                return false;
            }
            std::string value;
            for (usize i = 2; i < w.size(); ++i) {
                f64 v;
                if (!parseNumber(w[i], v)) {
                    if (err) *err = std::string(key) + " '" + std::string(w[1]) + "': '" + std::string(w[i]) + "' is not a number";
                    return false;
                }
                if (i > 2) value += ',';
                value += w[i];
            }
            out.params.emplace_back(std::string(w[1]), std::move(value));
        } else {
            out.records.emplace_back(line);   // any other record is an .ocmat record applied over the parent
        }
    }
    if (!header) {
        if (err) *err = "not an .ocmati file: no OCMATI version line";
        return false;
    }
    return true;
}

std::string writeOcmati(const OcMatInstance& m) {
    std::string s = "OCMATI 1\n";
    if (!m.name.empty()) s += "NAME " + m.name + "\n";
    if (!m.parent.empty()) s += "PARENT " + m.parent + "\n";
    for (const auto& [name, value] : m.params) {
        const bool vec = value.find(',') != std::string::npos;
        std::string spaced = value;
        std::replace(spaced.begin(), spaced.end(), ',', ' ');
        s += (vec ? "VECTOR " : "SCALAR ") + name + " " + spaced + "\n";
    }
    for (const std::string& r : m.records) s += r + "\n";
    return s;
}

bool loadOcmati(const std::string& path, OcMatInstance& out, std::string* err) {
    std::string text;
    if (!readFileText(path, text)) {
        if (err) *err = "could not read " + path;
        return false;
    }
    return parseOcmati(text, out, err);
}

bool saveOcmati(const std::string& path, const OcMatInstance& m, std::string* err) {
    if (!writeFileText(path, writeOcmati(m))) {
        if (err) *err = "could not write " + path;
        return false;
    }
    return true;
}

bool isMaterialInstancePath(std::string_view path) {
    constexpr std::string_view ext = ".ocmati";
    return path.size() >= ext.size() && ci(path.substr(path.size() - ext.size()), ext);
}

bool resolveMaterialInstance(const std::string& path, const MaterialTextReader& read,
                             ResolvedMaterialInstance& out, std::string* err) {
    out = ResolvedMaterialInstance{};
    std::set<std::string> seen;
    std::vector<OcMatInstance> levels;   // leaf first
    std::string cur = path;
    while (isMaterialInstancePath(cur)) {
        out.chain.push_back(cur);
        if (!seen.insert(canonPath(cur)).second) {
            if (err) {
                *err = "material instance cycle:";
                for (usize i = 0; i < out.chain.size(); ++i) *err += (i ? " -> " : " ") + out.chain[i];
            }
            return false;
        }
        if (out.chain.size() > kMaxInstanceChain) {
            if (err) *err = "material instance chain longer than " + std::to_string(kMaxInstanceChain) + " at " + cur;
            return false;
        }
        std::string text, perr;
        OcMatInstance inst;
        if (!read(cur, text)) { if (err) *err = "could not read material instance " + cur; return false; }
        if (!parseOcmati(text, inst, &perr)) { if (err) *err = cur + ": " + perr; return false; }
        if (inst.parent.empty()) { if (err) *err = cur + ": no PARENT record"; return false; }
        cur = inst.parent;
        levels.push_back(std::move(inst));
    }
    out.chain.push_back(cur);
    out.basePath = cur;
    if (!read(cur, out.baseText)) { if (err) *err = "could not read parent material " + cur; return false; }

    std::map<std::string, std::string> merged;
    for (auto it = levels.rbegin(); it != levels.rend(); ++it) {   // root-first: the child overwrites
        for (const auto& [n, v] : it->params) merged[n] = v;
        out.records.insert(out.records.end(), it->records.begin(), it->records.end());
    }
    out.params.assign(merged.begin(), merged.end());
    return true;
}

std::string composeInstanceOcmat(const ResolvedMaterialInstance& r) {
    std::string s = r.baseText;
    if (!s.empty() && s.back() != '\n') s += '\n';
    for (const std::string& rec : r.records) s += rec + "\n";
    return s;
}

// ------------------------------------------------------------------------------------ parameters

std::vector<MaterialGraphParameter> listMaterialGraphParameters(const OcGraphData& g) {
    std::vector<MaterialGraphParameter> out;
    for (const OcGraphNode& n : g.nodes) {
        const bool sc = ci(n.type, "ScalarParameter"), ve = ci(n.type, "VectorParameter");
        if (!sc && !ve) continue;
        const std::string_view name = nodeAttr(n, "name");
        if (name.empty()) continue;
        MaterialGraphParameter p;
        p.name = std::string(name);
        p.isVector = ve;
        for (const OcGraphPin& pin : n.pins)
            if (pin.isOutput) { p.defaultValue = pin.defaultValue; break; }
        out.push_back(std::move(p));
    }
    return out;
}

bool applyMaterialGraphParameters(OcGraphData& g, const std::vector<std::pair<std::string, std::string>>& params,
                                  std::vector<std::string>* unknown, std::string* err) {
    for (const auto& [name, value] : params) {
        bool found = false;
        for (OcGraphNode& n : g.nodes) {
            const bool sc = ci(n.type, "ScalarParameter"), ve = ci(n.type, "VectorParameter");
            if ((!sc && !ve) || !ci(nodeAttr(n, "name"), name)) continue;
            usize count = 0;
            if (!numberList(value, ve ? 4 : 1, &count) || (sc && count != 1)) {
                if (err) *err = "parameter '" + name + "': '" + value + "' is not " + (ve ? "1 to 4 numbers" : "one number");
                return false;
            }
            found = true;
            for (OcGraphPin& pin : n.pins) if (pin.isOutput) pin.defaultValue = value;
        }
        if (!found && unknown) unknown->push_back(name);
    }
    return true;
}

std::string materialParameterKey(std::vector<std::pair<std::string, std::string>> params) {
    std::sort(params.begin(), params.end());
    std::string s;
    for (const auto& [n, v] : params) s += n + "=" + v + ";";
    return s;
}

// ------------------------------------------------------------------------------------ functions

namespace {

const char* constFor(const std::string& pinType) {
    if (ci(pinType, "float4")) return "ConstFloat4";
    if (ci(pinType, "float3")) return "ConstFloat3";
    if (ci(pinType, "float2")) return "ConstFloat2";
    return "ConstFloat";
}

struct Src { std::string node, pin; };

bool inlineAll(OcGraphData& g, const MaterialGraphReader& read, std::vector<std::string>& stack, std::string* err) {
    for (;;) {
        usize idx = g.nodes.size();
        for (usize i = 0; i < g.nodes.size(); ++i)
            if (ci(g.nodes[i].type, "CallFunction")) { idx = i; break; }
        if (idx == g.nodes.size()) return true;

        const OcGraphNode call = g.nodes[idx];
        const std::string fpath(nodeAttr(call, "function"));
        const auto fail = [&](const std::string& m) {
            if (err) *err = "CallFunction '" + call.id + "': " + m;
            return false;
        };
        if (fpath.empty()) return fail("no function= path");
        const std::string key = canonPath(fpath);
        if (std::find(stack.begin(), stack.end(), key) != stack.end()) {
            std::string chain;
            for (const std::string& s : stack) chain += s + " -> ";
            return fail("material function cycle: " + chain + key);
        }
        if (stack.size() >= kMaxFunctionDepth) return fail("functions nest deeper than " + std::to_string(kMaxFunctionDepth));

        OcGraphData f;
        std::string rerr;
        if (!read(fpath, f, &rerr)) return fail("could not load " + fpath + ": " + rerr);
        if (!ci(f.domain, "materialfunction")) return fail(fpath + " is not a DOMAIN materialfunction graph");
        stack.push_back(key);
        const bool inner = inlineAll(f, read, stack, err);
        stack.pop_back();
        if (!inner) return false;

        const std::string prefix = call.id + "~";
        std::map<std::string, Src> inputs;              // function input name -> where its value comes from
        std::map<std::string, std::string> inputName;   // FunctionInput node id -> name
        std::map<std::string, Src> outputs;             // function output name -> source inside the inlined body
        std::map<std::string, std::string> outputName;  // FunctionOutput node id -> name
        std::vector<OcGraphNode> added;

        for (const OcGraphNode& n : f.nodes) {
            const bool isIn = ci(n.type, "FunctionInput"), isOut = ci(n.type, "FunctionOutput");
            if (!isIn && !isOut) {
                OcGraphNode c = n;
                c.id = prefix + n.id;
                added.push_back(std::move(c));
                continue;
            }
            const std::string name(nodeAttr(n, "name"));
            if (name.empty()) return fail(fpath + ": " + n.type + " '" + n.id + "' has no name=");
            if (isOut) { outputName[n.id] = name; continue; }
            inputName[n.id] = name;
            // A caller link wins; else the call's own typed literal; else the function input's default.
            const OcGraphLink* feed = nullptr;
            for (const OcGraphLink& l : g.links)
                if (ci(l.destNode, call.id) && ci(l.destPin, name)) { feed = &l; break; }
            if (feed) { inputs[name] = Src{feed->sourceNode, feed->sourcePin}; continue; }
            std::string type = "float", def;
            for (const OcGraphPin& p : n.pins) if (p.isOutput) { type = p.type; def = p.defaultValue; }
            for (const OcGraphPin& p : call.pins)
                if (!p.isOutput && ci(p.name, name) && !p.defaultValue.empty()) def = p.defaultValue;
            OcGraphNode c;
            c.id = prefix + n.id;
            c.type = constFor(type);
            c.hasPosition = false;
            c.pins.push_back(OcGraphPin{"value", type, true, def});
            added.push_back(std::move(c));
            inputs[name] = Src{prefix + n.id, "value"};
        }

        const auto mapSrc = [&](const OcGraphLink& l) {
            const auto it = inputName.find(l.sourceNode);
            if (it != inputName.end()) return inputs[it->second];
            return Src{prefix + l.sourceNode, l.sourcePin};
        };
        std::vector<OcGraphLink> bodyLinks;
        for (const OcGraphLink& l : f.links) {
            if (inputName.count(l.destNode)) continue;
            if (const auto o = outputName.find(l.destNode); o != outputName.end()) { outputs[o->second] = mapSrc(l); continue; }
            const Src s = mapSrc(l);
            bodyLinks.push_back(OcGraphLink{s.node, s.pin, prefix + l.destNode, l.destPin});
        }

        std::vector<OcGraphLink> links;
        for (OcGraphLink l : g.links) {
            if (ci(l.destNode, call.id)) continue;
            if (ci(l.sourceNode, call.id)) {
                const auto o = outputs.find(l.sourcePin);
                if (o == outputs.end()) return fail("function " + fpath + " has no connected output '" + l.sourcePin + "'");
                l.sourceNode = o->second.node;
                l.sourcePin = o->second.pin;
            }
            links.push_back(std::move(l));
        }
        links.insert(links.end(), bodyLinks.begin(), bodyLinks.end());

        g.nodes.erase(g.nodes.begin() + static_cast<std::ptrdiff_t>(idx));
        g.nodes.insert(g.nodes.end(), added.begin(), added.end());
        g.links = std::move(links);
    }
}

} // namespace

bool inlineMaterialFunctions(OcGraphData& g, const MaterialGraphReader& read, std::string* err) {
    std::vector<std::string> stack;
    return inlineAll(g, read, stack, err);
}

} // namespace aver::fmt
