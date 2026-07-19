#include "aver/formats/OcMap.hpp"
#include "aver/formats/detail/TextScan.hpp"
#include "aver/platform/FileSystem.hpp"

namespace aver::fmt {
using namespace aver::fmt::detail;

namespace {

// ROOT hex -> bytes (up to 32), matching OcMap.cs ParseHash.
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

f64 tokF(const std::vector<std::string_view>& t, usize i) {
    return i < t.size() ? parseF64(t[i]) : 0.0;
}
std::string tokS(const std::vector<std::string_view>& t, usize i) {
    return i < t.size() ? std::string(t[i]) : std::string();
}

} // namespace

bool parseOcmap(std::string_view text, OcMapData& out, std::string* err) {
    out = OcMapData{};

    usize pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view rawLine = text.substr(pos, nl - pos);
        pos = nl + 1;

        // OcMap.cs StripComment: cut at first '#', trim, drop one trailing ';', trim.
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
            // PLACE <asset> <x y z> <yaw pitch roll> <scale> [surfaceId]
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
            // DEFORM <cageAsset> <x y z> <yaw pitch roll> <material>
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
        // Unknown keywords ignored (forward-compatible), matching the tolerant reader.
    }

    if (err) {
        std::string why;
        if (!ocmapIsServerValid(out, &why)) *err = why;
    }
    return true;
}

bool ocmapIsServerValid(const OcMapData& m, std::string* why) {
    if (m.name.empty()) { if (why) *why = "missing NAME"; return false; }
    if (m.contentId == 0) { if (why) *why = "missing or zero ID"; return false; }
    bool rootZero = true;
    for (u8 b : m.root) if (b != 0) { rootZero = false; break; }
    if (rootZero) { if (why) *why = "missing or zero ROOT"; return false; }
    if (!m.hasGround && m.placements.empty()) { if (why) *why = "no collision source (need GROUND or >=1 PLACE)"; return false; }
    return true;
}

bool loadOcmap(const std::string& path, OcMapData& out, std::string* err) {
    std::string text;
    if (!readFileText(path, text)) {
        if (err) *err = "cannot read file: " + path;
        return false;
    }
    return parseOcmap(text, out, err);
}

} // namespace aver::fmt
