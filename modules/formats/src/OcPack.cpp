// .ocpack reader/writer and the instance-row matrix helper. See OcPack.hpp.
#include "aver/formats/OcPack.hpp"
#include "aver/formats/OcLevelInst.hpp"
#include "aver/formats/detail/TextEscape.hpp"
#include "aver/formats/detail/TextScan.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace aver::fmt {
using namespace aver::fmt::detail;

namespace {

std::string normaliseIdPath(std::string_view p) {
    std::vector<u64> ids;
    if (!ocLevelInstSplitIds(p, ids)) return {};
    return ocLevelInstJoinIds(ids);
}

} // namespace

bool OcPack::keeps(std::string_view idPath) const {
    const std::string n = normaliseIdPath(idPath);
    if (n.empty()) return false;
    return std::find(keep.begin(), keep.end(), n) != keep.end();
}

bool parseOcPack(std::string_view text, OcPack& out, std::string* why) {
    out = OcPack{};
    const auto fail = [&](const char* m) { if (why) *why = m; return false; };
    bool sawHeader = false, sawSource = false;
    usize pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        const std::string_view line = stripTrailingSemicolon(truncateHash(text.substr(pos, nl - pos)));
        pos = nl + 1;
        if (line.empty()) continue;
        const std::vector<std::string_view> t = splitWhitespace(line);
        if (t.empty()) continue;
        if (equalsCI(t[0], "OCPACK")) {
            out.version = t.size() > 1 ? parseI32(t[1], 1) : 1;
            sawHeader = true;
        } else if (!sawHeader) {
            return fail("not an .ocpack file (no OCPACK header line)");
        } else if (equalsCI(t[0], "SOURCE")) {
            if (t.size() < 2) return fail("SOURCE without a hash");
            out.source = ocLevelInstIdParse(t[1]);
            sawSource = true;
        } else if (equalsCI(t[0], "TABLE")) {
            if (t.size() < 2) return fail("TABLE without a path");
            out.table = pctDecode(t[1]);
        } else if (equalsCI(t[0], "GROUP")) {
            // GROUP <index> collide <0|1> material <name|->; indices must run 0,1,2...
            if (t.size() < 2 || parseU64(t[1], ~0ull) != out.groups.size()) return fail("GROUP index out of order");
            OcPackGroup g;
            for (usize i = 2; i + 1 < t.size(); i += 2) {
                if (equalsCI(t[i], "collide")) g.collide = parseI32(t[i + 1], 0) != 0;
                else if (equalsCI(t[i], "material")) g.material = t[i + 1] == "-" ? std::string() : pctDecode(t[i + 1]);
            }
            out.groups.push_back(std::move(g));
        } else if (equalsCI(t[0], "KEEP")) {
            if (t.size() < 2) continue;
            std::string n = normaliseIdPath(t[1]);
            if (!n.empty()) out.keep.push_back(std::move(n));
        }
        // Unknown records are skipped, like every other text format here.
    }
    if (!sawHeader) return fail("not an .ocpack file (no OCPACK header line)");
    if (!sawSource) return fail("an .ocpack needs a SOURCE record");
    if (out.table.empty()) return fail("an .ocpack needs a TABLE record");
    return true;
}

bool loadOcPack(const std::string& path, OcPack& out, std::string* why) {
    std::string text;
    if (!readFileText(path, text)) {
        if (why) *why = "could not read " + path;
        return false;
    }
    return parseOcPack(text, out, why);
}

std::string writeOcPack(const OcPack& p) {
    std::string s;
    s.reserve(128 + p.groups.size() * 48 + p.keep.size() * 36);
    s += "OCPACK 1\n";
    s += "SOURCE " + ocLevelInstIdText(p.source) + "\n";
    s += "TABLE " + pctEncode(p.table) + "\n";
    for (usize i = 0; i < p.groups.size(); ++i) {
        s += "GROUP " + std::to_string(i) + " collide " + (p.groups[i].collide ? "1" : "0") + " material ";
        s += p.groups[i].material.empty() ? std::string("-") : pctEncode(p.groups[i].material);
        s += '\n';
    }
    for (const std::string& k : p.keep) s += "KEEP " + k + "\n";
    return s;
}

bool saveOcPack(const std::string& path, const OcPack& p, std::string* why) {
    createParentDirectories(path);
    if (!writeFileTextAtomic(path, writeOcPack(p))) {
        if (why) *why = "could not write " + path;
        return false;
    }
    return true;
}

bool ocPackMatchesTable(const OcPack& pack, const OcInstanceData& table, std::string* why) {
    if (pack.groups.size() != table.groups.size()) {
        if (why) *why = "the pack lists " + std::to_string(pack.groups.size()) + " group(s) but its table has " +
                        std::to_string(table.groups.size());
        return false;
    }
    return true;
}

void transformOcInstanceRows(const f32* rows, usize count, const Mat4& m, f32* out) {
    for (usize i = 0; i < count; ++i) {
        const f32* r = rows + i * 12;
        f32 o[12];
        for (int row = 0; row < 4; ++row) {
            // Row `row` of the 4x4: the stored 3 floats, then the implied last column (0,0,0,1).
            const f32 a0 = r[row * 3 + 0], a1 = r[row * 3 + 1], a2 = r[row * 3 + 2];
            const f32 a3 = row == 3 ? 1.0f : 0.0f;
            for (int c = 0; c < 3; ++c)
                o[row * 3 + c] = a0 * m.m[0][c] + a1 * m.m[1][c] + a2 * m.m[2][c] + a3 * m.m[3][c];
        }
        for (int k = 0; k < 12; ++k) out[i * 12 + k] = o[k];
    }
}

} // namespace aver::fmt
