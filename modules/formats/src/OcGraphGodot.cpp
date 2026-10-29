// The GodotShader node of a DOMAIN material .ocgraph (OcGraphGodot.hpp).
#include "aver/formats/OcGraphGodot.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace aver::fmt {
namespace {

constexpr usize kChunkRaw = 1500;   // raw characters per text chunk

bool isPlain(unsigned char c) { return c > 0x20 && c < 0x7F && c != '#' && c != '%'; }

std::string attrOf(const OcGraphNode& n, std::string_view key) {
    for (const std::string& t : n.extraTokens) {
        if (t.size() > key.size() && t.compare(0, key.size(), key) == 0 && t[key.size()] == '=') return t.substr(key.size() + 1);
    }
    return {};
}

bool hasAttr(const OcGraphNode& n, std::string_view key) {
    for (const std::string& t : n.extraTokens)
        if (t.size() > key.size() && t.compare(0, key.size(), key) == 0 && t[key.size()] == '=') return true;
    return false;
}

u32 uintAttr(const OcGraphNode& n, std::string_view key) {
    const std::string v = attrOf(n, key);
    return v.empty() ? 0u : static_cast<u32>(std::strtoul(v.c_str(), nullptr, 10));
}

void addChunks(OcGraphNode& n, const char* prefix, const std::string& text) {
    for (usize i = 0, k = 0; i < text.size(); i += kChunkRaw, ++k)
        n.extraTokens.push_back(std::string(prefix) + std::to_string(k) + "=" + gdGraphEscape(std::string_view(text).substr(i, kChunkRaw)));
}

bool readChunks(const OcGraphNode& n, const char* prefix, std::string& out, std::string* err) {
    out.clear();
    for (usize k = 0;; ++k) {
        const std::string key = std::string(prefix) + std::to_string(k);
        if (!hasAttr(n, key)) break;
        std::string piece;
        if (!gdGraphUnescape(attrOf(n, key), piece)) {
            if (err) *err = "the GodotShader node's " + key + " is not valid percent-escaped text";
            return false;
        }
        out += piece;
    }
    return true;
}

}  // namespace

std::string gdGraphEscape(std::string_view s) {
    std::string o;
    o.reserve(s.size() + s.size() / 4);
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (isPlain(c)) { o += ch; continue; }
        char buf[4];
        std::snprintf(buf, sizeof buf, "%%%02X", c);
        o += buf;
    }
    return o;
}

bool gdGraphUnescape(std::string_view s, std::string& out) {
    out.clear();
    out.reserve(s.size());
    for (usize i = 0; i < s.size(); ++i) {
        if (s[i] != '%') { out += s[i]; continue; }
        if (i + 2 >= s.size()) return false;
        const auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        const int h = hex(s[i + 1]), l = hex(s[i + 2]);
        if (h < 0 || l < 0) return false;
        out += static_cast<char>(h * 16 + l);
        i += 2;
    }
    return true;
}

void packGodotShader(const GodotShaderPart& part, OcGraphData& g) {
    g.domain = "material";
    if (g.name.empty()) g.name = part.name;
    OcGraphNode n;
    n.id = "gd";
    n.type = "GodotShader";
    n.hasPosition = false;
    n.extraTokens.push_back("name=" + gdGraphEscape(part.name));
    n.extraTokens.push_back("ns=" + part.ns);
    n.extraTokens.push_back("nu=" + std::to_string(part.regsU));
    n.extraTokens.push_back("ni=" + std::to_string(part.regsI));
    n.extraTokens.push_back("ng=" + std::to_string(part.regsG));
    n.extraTokens.push_back("fx=" + std::to_string(part.fx));
    n.extraTokens.push_back(std::string("hasfs=") + (part.hasFragment ? "1" : "0"));
    n.extraTokens.push_back(std::string("hasvs=") + (part.hasVertex ? "1" : "0"));
    if (!part.varyings.empty()) {
        std::string v;
        for (usize i = 0; i < part.varyings.size(); ++i) {
            if (i) v += ';';
            v += std::string(part.varyings[i].flat ? "flat" : "smooth") + ":" + std::to_string(part.varyings[i].rows) + ":" + gdGraphEscape(part.varyings[i].decl);
        }
        n.extraTokens.push_back("vary=" + v);
    }
    if (part.hasFragment) addChunks(n, "fs", part.fragmentSource);
    if (part.hasVertex) addChunks(n, "vs", part.vertexSource);
    for (usize i = 0; i < g.nodes.size(); ++i)
        if (g.nodes[i].type == "GodotShader") { g.nodes.erase(g.nodes.begin() + static_cast<std::ptrdiff_t>(i)); break; }
    g.nodes.push_back(std::move(n));
}

bool ocGraphHasGodotShader(const OcGraphData& g) {
    for (const OcGraphNode& n : g.nodes)
        if (n.type == "GodotShader") return true;
    return false;
}

bool unpackGodotShader(const OcGraphData& g, GodotShaderPart& out, std::string* err) {
    out = GodotShaderPart{};
    const OcGraphNode* node = nullptr;
    for (const OcGraphNode& n : g.nodes) {
        if (n.type != "GodotShader") continue;
        if (node) { if (err) *err = "two GodotShader nodes: a graph carries one shader"; return false; }
        node = &n;
    }
    if (!node) { if (err) *err = "no GodotShader node"; return false; }
    std::string nm;
    if (!gdGraphUnescape(attrOf(*node, "name"), nm)) { if (err) *err = "the GodotShader node's name is malformed"; return false; }
    out.name = nm.empty() ? g.name : nm;
    out.ns = attrOf(*node, "ns");
    for (const char c : out.ns) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) { if (err) *err = "the GodotShader node's ns must be an identifier prefix"; return false; }
    }
    out.regsU = uintAttr(*node, "nu");
    out.regsI = uintAttr(*node, "ni");
    out.regsG = uintAttr(*node, "ng");
    out.fx = uintAttr(*node, "fx");
    out.hasFragment = uintAttr(*node, "hasfs") != 0;
    out.hasVertex = uintAttr(*node, "hasvs") != 0;
    if (!readChunks(*node, "fs", out.fragmentSource, err)) return false;
    if (!readChunks(*node, "vs", out.vertexSource, err)) return false;
    if (out.hasFragment && out.fragmentSource.empty()) { if (err) *err = "the GodotShader node says it has a fragment() but carries no fragment source"; return false; }
    if (out.hasVertex && out.vertexSource.empty()) { if (err) *err = "the GodotShader node says it has a vertex() but carries no vertex source"; return false; }
    const std::string vary = attrOf(*node, "vary");
    for (usize b = 0; b < vary.size();) {
        const usize e = std::min(vary.find(';', b), vary.size());
        const std::string item = vary.substr(b, e - b);
        b = e + 1;
        if (item.empty()) continue;
        const usize c1 = item.find(':'), c2 = c1 == std::string::npos ? c1 : item.find(':', c1 + 1);
        if (c2 == std::string::npos) { if (err) *err = "a varying item of the GodotShader node is malformed"; return false; }
        GodotShaderVarying v;
        v.flat = item.compare(0, c1, "flat") == 0;
        v.rows = static_cast<u32>(std::strtoul(item.substr(c1 + 1, c2 - c1 - 1).c_str(), nullptr, 10));
        if (v.rows == 0) v.rows = 1;
        if (!gdGraphUnescape(item.substr(c2 + 1), v.decl)) { if (err) *err = "a varying declaration of the GodotShader node is malformed"; return false; }
        out.varyings.push_back(std::move(v));
    }
    return true;
}

// ---- G6 SH3: GodotEnvShader ----

void packGodotEnvShader(const GodotEnvPart& part, OcGraphData& g) {
    g.domain = "godotenv";
    if (g.name.empty()) g.name = part.name;
    OcGraphNode n;
    n.id = "gd";
    n.type = "GodotEnvShader";
    n.hasPosition = false;
    n.extraTokens.push_back("kind=" + part.kind);
    n.extraTokens.push_back("name=" + gdGraphEscape(part.name));
    n.extraTokens.push_back("ns=" + part.ns);
    n.extraTokens.push_back("regs=" + std::to_string(part.regs));
    n.extraTokens.push_back("gregs=" + std::to_string(part.globalRegs));
    n.extraTokens.push_back(std::string("start=") + (part.hasStart ? "1" : "0"));
    n.extraTokens.push_back(std::string("process=") + (part.hasProcess ? "1" : "0"));
    std::string words;
    for (usize i = 0; i < part.defaults.size(); ++i) {
        u32 bits = 0;
        std::memcpy(&bits, &part.defaults[i], sizeof bits);
        char buf[16];
        std::snprintf(buf, sizeof buf, "%s%08X", i ? "," : "", static_cast<unsigned>(bits));
        words += buf;
    }
    if (!words.empty()) n.extraTokens.push_back("reg=" + words);
    addChunks(n, "src", part.source);
    for (usize i = 0; i < g.nodes.size(); ++i)
        if (g.nodes[i].type == "GodotEnvShader") { g.nodes.erase(g.nodes.begin() + static_cast<std::ptrdiff_t>(i)); break; }
    g.nodes.push_back(std::move(n));
}

bool ocGraphHasGodotEnvShader(const OcGraphData& g) {
    for (const OcGraphNode& n : g.nodes)
        if (n.type == "GodotEnvShader") return true;
    return false;
}

bool unpackGodotEnvShader(const OcGraphData& g, GodotEnvPart& out, std::string* err) {
    out = GodotEnvPart{};
    const OcGraphNode* node = nullptr;
    for (const OcGraphNode& n : g.nodes) {
        if (n.type != "GodotEnvShader") continue;
        if (node) { if (err) *err = "two GodotEnvShader nodes: a graph carries one shader"; return false; }
        node = &n;
    }
    if (!node) { if (err) *err = "no GodotEnvShader node"; return false; }
    out.kind = attrOf(*node, "kind");
    if (out.kind != "sky" && out.kind != "fog" && out.kind != "particles") { if (err) *err = "the GodotEnvShader node's kind must be sky, fog or particles"; return false; }
    std::string nm;
    if (!gdGraphUnescape(attrOf(*node, "name"), nm)) { if (err) *err = "the GodotEnvShader node's name is malformed"; return false; }
    out.name = nm.empty() ? g.name : nm;
    out.ns = attrOf(*node, "ns");
    for (const char c : out.ns) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) { if (err) *err = "the GodotEnvShader node's ns must be an identifier prefix"; return false; }
    }
    out.regs = uintAttr(*node, "regs");
    out.globalRegs = uintAttr(*node, "gregs");
    if (out.regs + out.globalRegs > 16) { if (err) *err = "the GodotEnvShader node declares more than 16 uniform registers"; return false; }
    out.hasStart = uintAttr(*node, "start") != 0;
    out.hasProcess = uintAttr(*node, "process") != 0;
    const std::string words = attrOf(*node, "reg");
    for (usize b = 0; b < words.size();) {
        const usize e = std::min(words.find(',', b), words.size());
        const std::string w = words.substr(b, e - b);
        b = e + 1;
        if (w.empty()) continue;
        const u32 bits = static_cast<u32>(std::strtoul(w.c_str(), nullptr, 16));
        float f = 0.0f;
        std::memcpy(&f, &bits, sizeof f);
        out.defaults.push_back(f);
    }
    out.defaults.resize(static_cast<usize>(out.regs + out.globalRegs) * 4, 0.0f);
    if (!readChunks(*node, "src", out.source, err)) return false;
    if (out.source.empty()) { if (err) *err = "the GodotEnvShader node carries no source"; return false; }
    return true;
}

}  // namespace aver::fmt
