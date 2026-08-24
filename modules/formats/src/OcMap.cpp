// .ocmap reader: the text scanner behind parseOcmap, plus the server-side validity check.
#include "aver/formats/OcMap.hpp"
#include "aver/formats/detail/TextScan.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace aver::fmt {
using namespace aver::fmt::detail;

namespace {

// Decodes a ROOT hex string into up to 32 bytes, zero-filling the rest.
void parseRoot(std::string_view hex, std::array<u8, 32>& out) {
    hex = trim(hex);
    out.fill(0);
    const usize n = (hex.size() / 2 < 32) ? hex.size() / 2 : 32;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (usize i = 0; i < n; ++i) {
        const int hi = nib(hex[i * 2]), lo = nib(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) break;
        out[i] = static_cast<u8>((hi << 4) | lo);
    }
}

// The inverse of parseRoot just above: always the full 64 lowercase hex characters (32 bytes),
// zero bytes included. parseRoot never fails partway and leave a short tail -- it zero-fills
// whatever a short hex string didn't cover -- so this has nothing to omit and always writes the
// same length parseRoot always reads.
std::string writeRoot(const std::array<u8, 32>& root) {
    static const char kHex[] = "0123456789abcdef";
    std::string s;
    s.reserve(64);
    for (u8 b : root) { s += kHex[b >> 4]; s += kHex[b & 0xF]; }
    return s;
}

// Token `i` as a double, or 0 when the line is shorter than that.
f64 tokF(const std::vector<std::string_view>& t, usize i) {
    return i < t.size() ? parseF64(t[i]) : 0.0;
}
// Token `i` as a string, or empty when the line is shorter than that.
std::string tokS(const std::vector<std::string_view>& t, usize i) {
    return i < t.size() ? std::string(t[i]) : std::string();
}

// Formats a number for the text form: enough digits to round-trip, no trailing noise. The exact
// duplicate of OcWorld.cpp's own `num` -- kept separate rather than shared because it is three
// lines and pulling the two writers together for it would cost more coupling than it saves.
std::string num(f64 v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

} // namespace

// Parses an .ocmap from memory. Always succeeds; invariant failures are reported through `err`.
bool parseOcmap(std::string_view text, OcMapData& out, std::string* err) {
    out = OcMapData{};

    usize pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view rawLine = text.substr(pos, nl - pos);
        pos = nl + 1;

        std::string_view line = stripTrailingSemicolon(truncateHash(rawLine));
        if (line.empty()) continue;

        std::vector<std::string_view> t = splitWhitespace(line);
        if (t.empty()) continue;
        std::string_view key = t[0];

        if (equalsCI(key, "OCMAP")) {
            if (t.size() > 1) out.version = parseI32(t[1], 1);
        } else if (equalsCI(key, "ID")) {
            out.contentId = t.size() > 1 ? parseU64(t[1]) : 0;
        } else if (equalsCI(key, "NAME")) {
            out.name = tokS(t, 1);
        } else if (equalsCI(key, "BUILD")) {
            out.build = static_cast<u32>(t.size() > 1 ? parseU64(t[1]) : 0);
        } else if (equalsCI(key, "ALGO")) {
            out.algo = static_cast<u32>(t.size() > 1 ? parseU64(t[1]) : 3);
        } else if (equalsCI(key, "ROOT")) {
            if (t.size() > 1) parseRoot(t[1], out.root);
        } else if (equalsCI(key, "CLIENT")) {
            out.clientUmap = (t.size() >= 3 && equalsCI(t[1], "umap")) ? std::string(t[2]) : std::string();
        } else if (equalsCI(key, "KILLZ")) {
            out.killZ = tokF(t, 1);
        } else if (equalsCI(key, "GROUND")) {
            out.hasGround = true;
            out.groundZ = tokF(t, 1);
            out.groundSurface = t.size() > 2 ? static_cast<i32>(tokF(t, 2)) : 0;
        } else if (equalsCI(key, "SPAWN")) {
            out.hasSpawn = true;
            out.spawnX = tokF(t, 1); out.spawnY = tokF(t, 2); out.spawnZ = tokF(t, 3); out.spawnYaw = tokF(t, 4);
        } else if (equalsCI(key, "SURFACE")) {
            OcSurface s;
            s.id = static_cast<i32>(tokF(t, 1));
            s.name = tokS(t, 2);
            s.grip = tokF(t, 3);
            s.roll = tokF(t, 4);
            s.restitution = t.size() > 5 ? tokF(t, 5) : 0.0;
            out.surfaces.push_back(std::move(s));
        } else if (equalsCI(key, "PLACE")) {
            OcPlacement p;
            p.asset = tokS(t, 1);
            p.x = tokF(t, 2); p.y = tokF(t, 3); p.z = tokF(t, 4);
            p.yaw = tokF(t, 5); p.pitch = tokF(t, 6); p.roll = tokF(t, 7);
            p.scale = t.size() > 8 ? tokF(t, 8) : 1.0;
            p.surface = t.size() > 9 ? static_cast<i32>(tokF(t, 9)) : -1;
            p.deform = false;
            p.objectId = makeObjectId(p.asset);
            out.placements.push_back(std::move(p));
        } else if (equalsCI(key, "DEFORM")) {
            OcPlacement p;
            p.asset = tokS(t, 1);
            p.x = tokF(t, 2); p.y = tokF(t, 3); p.z = tokF(t, 4);
            p.yaw = tokF(t, 5); p.pitch = tokF(t, 6); p.roll = tokF(t, 7);
            p.scale = 1.0;
            p.surface = -1;
            p.deform = true;
            p.material = t.size() > 8 ? std::string(t[8]) : std::string("default");
            p.objectId = makeObjectId(p.asset);
            out.placements.push_back(std::move(p));
        }
    }

    if (err) {
        std::string why;
        if (!ocmapIsServerValid(out, &why)) *err = why;
    }
    return true;
}

// True when the map has NAME, a non-zero ID and ROOT, and a collision source.
bool ocmapIsServerValid(const OcMapData& m, std::string* why) {
    if (m.name.empty()) { if (why) *why = "missing NAME"; return false; }
    if (m.contentId == 0) { if (why) *why = "missing or zero ID"; return false; }
    bool rootZero = true;
    for (u8 b : m.root) if (b != 0) { rootZero = false; break; }
    if (rootZero) { if (why) *why = "missing or zero ROOT"; return false; }
    if (!m.hasGround && m.placements.empty()) { if (why) *why = "no collision source (need GROUND or >=1 PLACE)"; return false; }
    return true;
}

// Loads an .ocmap from disk.
bool loadOcmap(const std::string& path, OcMapData& out, std::string* err) {
    std::string text;
    if (!readFileText(path, text)) {
        if (err) *err = "cannot read file: " + path;
        return false;
    }
    return parseOcmap(text, out, err);
}

// Serialises a map to the text form. Record order matches §11 of docs/formats/FORMAT_SPECS.md's
// own worked example (identity, then ROOT/CLIENT, then the env block SURFACE/GROUND/KILLZ/SPAWN,
// then placements) rather than parseOcmap's read order, which is free to accept records in any
// order a hand-authored file puts them in but still has to write them out in ONE order.
std::string writeOcmap(const OcMapData& m) {
    std::string s;
    s.reserve(256 + m.placements.size() * 96);
    s += "OCMAP "; s += std::to_string(m.version); s += "\n";
    s += "# Written by the Aver Engine editor.\n";

    char idbuf[32];
    // Same "derive from the name if the caller never set one" fallback writeOcworld uses, for the
    // identical reason: a level created in this editor's legacy path (there is currently none, but
    // nothing prevents one existing later) would otherwise write ID 0x0000000000000000, which
    // ocmapIsServerValid rejects outright ("missing or zero ID"). A file that already carried a
    // real ID -- every file this writer actually round-trips today -- keeps exactly that ID.
    const u64 id = m.contentId ? m.contentId : makeObjectId(m.name);
    std::snprintf(idbuf, sizeof idbuf, "0x%016llX", static_cast<unsigned long long>(id));
    s += "ID "; s += idbuf; s += "\n";
    s += "NAME "; s += (m.name.empty() ? std::string("untitled") : m.name); s += "\n";
    s += "BUILD "; s += std::to_string(m.build); s += "\n";
    s += "ALGO "; s += std::to_string(m.algo); s += "\n";
    s += "ROOT "; s += writeRoot(m.root); s += "\n";
    // "" means build-from-assets, written as the bare `CLIENT scene` token the format spec's own
    // §11 example shows (`CLIENT scene  # or: CLIENT umap <path>`) -- not an empty `CLIENT umap `
    // that would read back as a umap path of "".
    s += "CLIENT ";
    if (m.clientUmap.empty()) s += "scene";
    else { s += "umap "; s += m.clientUmap; }
    s += "\n";

    if (!m.surfaces.empty()) {
        s += "#\n";
        for (const OcSurface& sf : m.surfaces) {
            s += "SURFACE " + std::to_string(sf.id) + " " + sf.name + " " +
                 num(sf.grip) + " " + num(sf.roll) + " " + num(sf.restitution) + "\n";
        }
    }
    if (m.hasGround) {
        s += "GROUND " + num(m.groundZ) + " " + std::to_string(m.groundSurface) + "\n";
    }
    // Unconditional, unlike GROUND/SPAWN just above and below: OcMapData carries no "was KILLZ
    // authored" flag the way hasGround/hasSpawn do, because every real .ocmap this format was
    // built to read has one (docs/formats/FORMAT_SPECS.md §4.3's own record catalog lists it
    // alongside GROUND and SPAWN as ordinary env-block fields) -- killZ's -5000.0 default is a
    // sensible fallback for a level that somehow has none, not a sentinel meaning "omit this line".
    s += "KILLZ " + num(m.killZ) + "\n";
    if (m.hasSpawn) {
        s += "SPAWN " + num(m.spawnX) + " " + num(m.spawnY) + " " + num(m.spawnZ) + " " +
             num(m.spawnYaw) + "\n";
    }

    s += "#\n";
    for (const OcPlacement& p : m.placements) {
        if (p.deform) {
            // No `default` fallback needed here the way the reader's own DEFORM branch has one:
            // parseOcmap already applied it at load time (`t.size() > 8 ? ... : "default"`), so
            // p.material is never empty for a record that reached this writer through a round trip.
            // A freshly-authored DEFORM placement with no material set would write an empty token
            // and fail to round-trip -- exactly why every other writer in this codebase folds its
            // "what does an unauthored field become" decision into the STRUCT default rather than
            // into the writer, and OcPlacement does too (there is no separate default here to keep
            // in sync with the reader's).
            s += "DEFORM " + p.asset + " " +
                 num(p.x) + " " + num(p.y) + " " + num(p.z) + " " +
                 num(p.yaw) + " " + num(p.pitch) + " " + num(p.roll) + " " + p.material + "\n";
        } else {
            s += "PLACE " + p.asset + " " +
                 num(p.x) + " " + num(p.y) + " " + num(p.z) + " " +
                 num(p.yaw) + " " + num(p.pitch) + " " + num(p.roll) + " " + num(p.scale);
            // OMITTED WHEN -1, matching OcPlacement's own "-1 = use the asset's own surface"
            // contract: a placement that never named one must not come back from a save claiming
            // surface 0, which IS a real, distinct surface (see the SURFACE table above it).
            if (p.surface >= 0) s += " " + std::to_string(p.surface);
            s += "\n";
        }
    }
    return s;
}

// Writes a map to disk, creating parent directories.
bool saveOcmap(const std::string& path, const OcMapData& m, std::string* err) {
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { if (err) *err = "could not open " + path + " for writing"; return false; }
    const std::string text = writeOcmap(m);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) { if (err) *err = "write failed for " + path; return false; }
    return true;
}

} // namespace aver::fmt
