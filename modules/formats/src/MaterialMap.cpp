#include "aver/formats/MaterialMap.hpp"

#include "aver/core/Log.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include "aver/platform/FileSystem.hpp"

namespace aver::fmt {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

u32 countTokens(const std::string& s) {
    u32 n = 0;
    bool in = false;
    for (const char c : s) {
        if (c == '_') in = false;
        else if (!in) { in = true; ++n; }
    }
    return n;
}

// Whole '_'-separated tokens only, trailing digits on the last one allowed: `clip` matches
// clip_player_ai and clip2 but not clipboard, and `stain` wall_stain_01 but not metal_stainless_steel.
bool tokenMatch(const std::string& name, const std::string& pat) {
    if (pat.empty()) return false;
    for (usize at = name.find(pat); at != std::string::npos; at = name.find(pat, at + 1)) {
        const usize end = at + pat.size();
        const bool startOk = at == 0 || name[at - 1] == '_';
        const bool endOk = end == name.size() || name[end] == '_' || std::isdigit(static_cast<unsigned char>(name[end]));
        if (startOk && endOk) return true;
    }
    return false;
}

bool ruleMatches(const MaterialRule& r, const std::string& leaf) {
    switch (r.kind) {
        case MaterialRuleKind::Any:       return true;
        case MaterialRuleKind::Tokens:    return tokenMatch(leaf, r.text);
        case MaterialRuleKind::Substring: return leaf.find(r.text) != std::string::npos;
        case MaterialRuleKind::Prefix:    return leaf.compare(0, r.text.size(), r.text) == 0;
        case MaterialRuleKind::Suffix:    return leaf.size() >= r.text.size() &&
                                                 leaf.compare(leaf.size() - r.text.size(), r.text.size(), r.text) == 0;
    }
    return false;
}

} // namespace

void parseMaterialMap(const std::string& text, bool legacy, MaterialMap& out) {
    out.legacy = legacy;
    std::istringstream in(text);
    std::string line;
    u32 lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        std::istringstream ls(line);
        MaterialRule r;
        if (!(ls >> r.pattern) || r.pattern[0] == '#') continue;
        if (!(ls >> r.stem)) {
            out.warnings.push_back("line " + std::to_string(lineNo) + ": '" + r.pattern + "' has no material stem; skipped");
            continue;
        }
        r.pattern = lower(r.pattern);
        r.line = lineNo;
        const bool front = r.pattern.front() == '*', back = r.pattern.size() > 1 && r.pattern.back() == '*';
        r.text = r.pattern.substr(front ? 1 : 0, r.pattern.size() - (front ? 1 : 0) - (back ? 1 : 0));
        if (r.text.empty()) r.kind = MaterialRuleKind::Any;   // "*" and "**"
        else if (front && back) r.kind = MaterialRuleKind::Substring;
        else if (front) r.kind = MaterialRuleKind::Suffix;
        else if (back) r.kind = MaterialRuleKind::Prefix;
        else r.kind = MaterialRuleKind::Tokens;
        r.tokenCount = r.kind == MaterialRuleKind::Any ? 0 : countTokens(r.text);
        if (!legacy && r.text.find('*') != std::string::npos)
            out.warnings.push_back("line " + std::to_string(lineNo) + ": '" + r.pattern +
                                   "' has a '*' inside; only leading/trailing wildcards are supported, so it never matches");
        out.rules.push_back(std::move(r));
    }
}

bool loadMaterialMap(const std::string& path, bool legacy, MaterialMap& out) {
    std::ifstream f(aver::nativePath(path), std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    parseMaterialMap(ss.str(), legacy, out);
    return true;
}

int matchMaterialRule(const MaterialMap& map, const std::string& leaf) {
    if (map.legacy) {
        for (usize i = 0; i < map.rules.size(); ++i) {
            const MaterialRule& r = map.rules[i];
            const bool match = r.pattern == "*" || (r.drop() ? tokenMatch(leaf, r.pattern)
                                                             : leaf.find(r.pattern) != std::string::npos);
            if (match) return static_cast<int>(i);
        }
        return -1;
    }
    int best = -1;
    for (usize i = 0; i < map.rules.size(); ++i) {
        const MaterialRule& r = map.rules[i];
        if (r.text.find('*') != std::string::npos || !ruleMatches(r, leaf)) continue;
        // More tokens replaces; the same count keeps the earlier line. Not pattern length: 'asphalt'
        // outranking a drop rule 'rvl' kept every asphalt_crack_rvl overlay as solid road.
        if (best < 0 || r.tokenCount > map.rules[usize(best)].tokenCount) best = static_cast<int>(i);
    }
    return best;
}

MaterialMapStats applyMaterialMap(std::vector<OcMeshData>& meshes, const std::vector<std::string>& names,
                                  const MaterialMap& map, MaterialMapReport* report) {
    MaterialMapStats st;
    std::map<std::string, MaterialMapReportEntry> seen;
    if (report) report->ruleWins.assign(map.rules.size(), 0);
    for (usize mi = 0; mi < meshes.size(); ++mi) {
        OcMeshData& m = meshes[mi];
        bool touched = false;
        std::vector<OcMeshSubmesh> keep;
        std::vector<std::string> droppedHere;
        for (OcMeshSubmesh& sm : m.submeshes) {
            if (sm.materialSlot >= m.materialSlots.size() || !m.materialSlots[sm.materialSlot].empty()) {
                ++st.alreadyBound;
                keep.push_back(sm);
                continue;
            }
            const std::string leaf = lower(sm.name.substr(sm.name.find_last_of('/') + 1));
            const int hit = matchMaterialRule(map, leaf);
            if (report) {
                MaterialMapReportEntry& e = seen[leaf];
                if (e.count++ == 0) {
                    e.leaf = leaf;
                    e.rule = hit;
                    e.firstMesh = mi < names.size() ? names[mi] : std::string("?");
                }
                if (hit >= 0) ++report->ruleWins[usize(hit)];
            }
            if (hit < 0) { ++st.unmatched; keep.push_back(sm); continue; }
            const MaterialRule& rule = map.rules[usize(hit)];
            touched = true;
            if (rule.drop()) {
                ++st.dropped;
                if (droppedHere.size() < 6) droppedHere.push_back(leaf + " (" + rule.pattern + ")");
                continue;
            }
            // Each subset has its own slot (buildMesh), so writing it in place touches no other submesh.
            m.materialSlots[sm.materialSlot] = rule.stem;
            ++st.mapped;
            keep.push_back(sm);
        }
        if (!touched && m.submeshes.size() <= 255) continue;
        if (keep.empty() && !m.submeshes.empty()) {
            ++st.emptied;
            std::string why;
            for (const std::string& d : droppedHere) why += (why.empty() ? "" : ", ") + d;
            AVER_INFO("--material-map: '{}' dropped whole: {}", mi < names.size() ? names[mi] : std::string("?"), why);
        }
        m.submeshes = std::move(keep);
        groupSubmeshesByMaterial(m);
    }
    if (report) for (auto& kv : seen) report->entries.push_back(std::move(kv.second));
    AVER_INFO("--material-map: {} submesh(es) mapped, {} dropped, {} matched no rule, {} mesh(es) emptied",
              st.mapped, st.dropped, st.unmatched, st.emptied);
    return st;
}

std::string materialMapReportText(const MaterialMap& map, const MaterialMapReport& report,
                                  const MaterialMapStats& stats, const std::string& source) {
    std::ostringstream o;
    const auto ruleText = [&](int i) {
        const MaterialRule& r = map.rules[usize(i)];
        return "line " + std::to_string(r.line) + " \"" + r.pattern + " " + r.stem + "\"";
    };
    o << "# material map report for " << source << "\n"
      << "# matching: " << (map.legacy ? "legacy (first rule in file order, plain substring)"
                                        : "most specific rule wins (more tokens, then file order)") << "\n"
      << "# " << map.rules.size() << " rule(s); " << stats.mapped << " submesh(es) mapped, " << stats.dropped
      << " dropped, " << stats.unmatched << " unmatched, " << stats.alreadyBound
      << " already had a material (not looked up), " << stats.emptied << " mesh(es) emptied\n";
    for (const std::string& w : map.warnings) o << "# warning: " << w << "\n";

    o << "\n## mapped (name -> winning rule -> material, submeshes)\n";
    for (const MaterialMapReportEntry& e : report.entries)
        if (e.rule >= 0 && !map.rules[usize(e.rule)].drop())
            o << e.leaf << " -> " << ruleText(e.rule) << " -> " << map.rules[usize(e.rule)].stem << "  x" << e.count << "\n";
    o << "\n## dropped (name -> winning rule, submeshes)\n";
    for (const MaterialMapReportEntry& e : report.entries)
        if (e.rule >= 0 && map.rules[usize(e.rule)].drop())
            o << e.leaf << " -> " << ruleText(e.rule) << "  x" << e.count << "  (first in " << e.firstMesh << ")\n";
    o << "\n## unmatched (left without a material)\n";
    for (const MaterialMapReportEntry& e : report.entries)
        if (e.rule < 0) o << e.leaf << "  x" << e.count << "  (first in " << e.firstMesh << ")\n";
    o << "\n## unused rules (won no submesh)\n";
    for (usize i = 0; i < map.rules.size(); ++i)
        if (i >= report.ruleWins.size() || report.ruleWins[i] == 0) o << ruleText(int(i)) << "\n";
    return o.str();
}

} // namespace aver::fmt
