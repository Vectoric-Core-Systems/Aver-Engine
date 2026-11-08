// PhysAnimProfile: see PhysAnimProfile.hpp.
#include "aver/ragdoll/PhysAnimProfile.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <queue>
#include <sstream>
#include <unordered_map>

namespace aver::ragdoll {

namespace {

bool toF32(const std::string& s, f32& v) {
    char* end = nullptr;
    v = std::strtof(s.c_str(), &end);
    return end && *end == 0 && !s.empty() && std::isfinite(v);
}

bool toI32(const std::string& s, i32& v) {
    char* end = nullptr;
    const long l = std::strtol(s.c_str(), &end, 10);
    v = static_cast<i32>(l);
    return end && *end == 0 && !s.empty();
}

bool toU64(const std::string& s, u64& v) {
    char* end = nullptr;
    v = std::strtoull(s.c_str(), &end, 0);   // base 0: decimal, 0x hex
    return end && *end == 0 && !s.empty();
}

// One "key value" drive pair; false when the key is not a drive key.
bool driveKey(PhysAnimDrive& d, const std::string& k, const std::string& v, bool& bad) {
    f32* dst = k == "strength" ? &d.strength : k == "damping" ? &d.damping : k == "torque" ? &d.torque
             : k == "blend" ? &d.blend : k == "boost" ? &d.boost : k == "impulse" ? &d.impulse
             : k == "attack" ? &d.attack : k == "hold" ? &d.hold : k == "decay" ? &d.decay : nullptr;
    if (!dst) return false;
    bad = !toF32(v, *dst);
    return true;
}

std::string num(f32 v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

void writeDrive(std::ostringstream& o, const PhysAnimDrive& d) {
    o << " strength " << num(d.strength) << " damping " << num(d.damping) << " torque " << num(d.torque)
      << " blend " << num(d.blend) << " boost " << num(d.boost) << " impulse " << num(d.impulse)
      << " attack " << num(d.attack) << " hold " << num(d.hold) << " decay " << num(d.decay);
}

bool fail(std::string* why, int line, const std::string& msg) {
    if (why) *why = "line " + std::to_string(line) + ": " + msg;
    return false;
}

} // namespace

bool parsePhysAnimProfile(std::string_view text, PhysAnimProfile& out, std::string* why) {
    std::istringstream in{std::string(text)};
    std::string raw;
    int line = 0;
    while (std::getline(in, raw)) {
        ++line;
        if (const auto h = raw.find('#'); h != std::string::npos) raw.erase(h);
        std::istringstream ls(raw);
        std::vector<std::string> t;
        for (std::string w; ls >> w;) t.push_back(w);
        if (t.empty()) continue;
        const std::string& cmd = t[0];

        if (cmd == "physanim") {
            if (t.size() != 2) return fail(why, line, "physanim <name>");
            out.name = t[1];
        } else if (cmd == "reach" || cmd == "life") {
            if (t.size() % 2 == 0) return fail(why, line, cmd + " takes key value pairs");
            for (usize i = 1; i + 1 < t.size(); i += 2) {
                const std::string &k = t[i], &v = t[i + 1];
                bool ok = true;
                if (cmd == "reach") {
                    if (k == "falloff") ok = toF32(v, out.falloffPerHop);
                    else if (k == "hops") ok = toI32(v, out.maxHops);
                    else if (k == "epsilon") ok = toF32(v, out.epsilon);
                    else if (k == "impulse") ok = toF32(v, out.fullImpulse);
                    else return fail(why, line, "unknown reach key '" + k + "'");
                } else {
                    if (k == "death") ok = toF32(v, out.deathSeconds);
                    else if (k == "getupblend") ok = toF32(v, out.getUpBlend);
                    else if (k == "supine") ok = toU64(v, out.getUpSupine);
                    else if (k == "prone") ok = toU64(v, out.getUpProne);
                    else if (k == "side") ok = toU64(v, out.getUpSide);
                    else return fail(why, line, "unknown life key '" + k + "'");
                }
                if (!ok) return fail(why, line, "bad value '" + v + "' for " + k);
            }
        } else if (cmd == "default" || cmd == "region") {
            usize i = 1;
            PhysAnimRegion r;
            PhysAnimDrive* d = &out.defaults;
            if (cmd == "region") {
                if (t.size() < 2) return fail(why, line, "region <name> ...");
                r.name = t[1];
                r.drive = out.defaults;
                d = &r.drive;
                i = 2;
            }
            if ((t.size() - i) % 2 != 0) return fail(why, line, "keys take a value");
            for (; i + 1 < t.size(); i += 2) {
                const std::string &k = t[i], &v = t[i + 1];
                bool bad = false;
                if (driveKey(*d, k, v, bad)) {
                    if (bad) return fail(why, line, "bad value '" + v + "' for " + k);
                } else if (cmd == "region" && k == "bone") {
                    r.bones.push_back(v);
                } else if (cmd == "region" && k == "subtree") {
                    r.subtrees.push_back(v);
                } else {
                    return fail(why, line, "unknown key '" + k + "'");
                }
            }
            if (cmd == "region") out.regions.push_back(std::move(r));
        } else {
            return fail(why, line, "unknown directive '" + cmd + "'");
        }
    }
    return true;
}

bool loadPhysAnimProfile(const std::string& path, PhysAnimProfile& out, std::string* why) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (why) *why = "cannot open " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    return parsePhysAnimProfile(ss.str(), out, why);
}

std::string serializePhysAnimProfile(const PhysAnimProfile& p) {
    std::ostringstream o;
    o << "# Aver physical animation profile\n";
    o << "physanim " << (p.name.empty() ? "unnamed" : p.name) << "\n";
    o << "reach falloff " << num(p.falloffPerHop) << " hops " << p.maxHops << " epsilon " << num(p.epsilon)
      << " impulse " << num(p.fullImpulse) << "\n";
    o << "life death " << num(p.deathSeconds) << " getupblend " << num(p.getUpBlend) << " supine "
      << p.getUpSupine << " prone " << p.getUpProne << " side " << p.getUpSide << "\n";
    o << "default";
    writeDrive(o, p.defaults);
    o << "\n";
    for (const PhysAnimRegion& r : p.regions) {
        o << "region " << r.name;
        for (const std::string& b : r.bones) o << " bone " << b;
        for (const std::string& b : r.subtrees) o << " subtree " << b;
        writeDrive(o, r.drive);
        o << "\n";
    }
    return o.str();
}

bool savePhysAnimProfile(const std::string& path, const PhysAnimProfile& p, std::string* why) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { if (why) *why = "cannot write " + path; return false; }
    f << serializePhysAnimProfile(p);
    return static_cast<bool>(f);
}

bool validatePhysAnimProfile(const PhysAnimProfile& p, const fmt::OcSkeleton& skel, std::vector<std::string>* problems) {
    std::vector<std::string> local;
    auto& out = problems ? *problems : local;
    const usize before = out.size();

    std::unordered_map<std::string, int> names;
    for (const fmt::OcBone& b : skel.bones) names.emplace(b.name, 1);

    auto drive = [&](const std::string& who, const PhysAnimDrive& d) {
        auto in01 = [&](f32 v, const char* what) {
            if (!(v >= 0.0f && v <= 1.0f)) out.push_back(who + ": " + what + " " + num(v) + " is outside 0..1");
        };
        auto nonNeg = [&](f32 v, const char* what) {
            if (!(v >= 0.0f)) out.push_back(who + ": " + what + " " + num(v) + " is negative");
        };
        in01(d.strength, "strength");
        in01(d.blend, "blend");
        in01(d.boost, "boost");
        nonNeg(d.damping, "damping");
        nonNeg(d.torque, "torque");
        nonNeg(d.impulse, "impulse");
        nonNeg(d.attack, "attack");
        nonNeg(d.hold, "hold");
        nonNeg(d.decay, "decay");
    };
    drive("default", p.defaults);

    std::unordered_map<std::string, int> seen;
    for (const PhysAnimRegion& r : p.regions) {
        if (r.name.empty()) out.push_back("a region has no name");
        if (++seen[r.name] == 2) out.push_back("region '" + r.name + "' is defined twice");
        if (r.bones.empty() && r.subtrees.empty()) out.push_back("region '" + r.name + "' names no bone");
        for (const std::string& b : r.bones)
            if (!names.count(b)) out.push_back("region '" + r.name + "': unknown bone '" + b + "'");
        for (const std::string& b : r.subtrees)
            if (!names.count(b)) out.push_back("region '" + r.name + "': unknown subtree root '" + b + "'");
        drive("region '" + r.name + "'", r.drive);
    }
    if (!(p.falloffPerHop >= 0.0f && p.falloffPerHop <= 1.0f)) out.push_back("reach falloff is outside 0..1");
    if (p.maxHops < 0) out.push_back("reach hops is negative");
    if (!(p.epsilon > 0.0f)) out.push_back("reach epsilon must be positive");
    if (!(p.fullImpulse > 0.0f)) out.push_back("reach impulse must be positive");
    if (!(p.deathSeconds >= 0.0f)) out.push_back("life death is negative");
    if (!(p.getUpBlend >= 0.0f)) out.push_back("life getupblend is negative");
    return out.size() == before;
}

ResolvedPhysAnim resolvePhysAnim(const PhysAnimProfile& p, const fmt::OcSkeleton& skel) {
    ResolvedPhysAnim r;
    const usize n = skel.bones.size();
    r.boneCount = static_cast<u32>(n);
    r.parents.resize(n);
    r.region.assign(n, -1);
    r.drive.assign(n, p.defaults);
    r.falloffPerHop = p.falloffPerHop;
    r.epsilon = p.epsilon;
    r.fullImpulse = p.fullImpulse;
    r.maxHops = p.maxHops;
    r.deathSeconds = p.deathSeconds;
    r.getUpBlend = p.getUpBlend;
    r.getUpSupine = p.getUpSupine;
    r.getUpProne = p.getUpProne;
    r.getUpSide = p.getUpSide;

    std::unordered_map<std::string, i32> byName;
    for (usize i = 0; i < n; ++i) {
        r.parents[i] = skel.bones[i].parent;
        byName.emplace(skel.bones[i].name, static_cast<i32>(i));
    }
    std::vector<i32> sub;
    for (usize ri = 0; ri < p.regions.size(); ++ri) {
        const PhysAnimRegion& reg = p.regions[ri];
        auto set = [&](i32 b) { r.region[static_cast<usize>(b)] = static_cast<i32>(ri); r.drive[static_cast<usize>(b)] = reg.drive; };
        for (const std::string& name : reg.bones)
            if (const auto it = byName.find(name); it != byName.end()) set(it->second);
        for (const std::string& name : reg.subtrees)
            if (const auto it = byName.find(name); it != byName.end()) {
                r.subtreeOf(it->second, sub);
                for (i32 b : sub) set(b);
            }
    }
    return r;
}

void ResolvedPhysAnim::subtreeOf(i32 bone, std::vector<i32>& out) const {
    out.clear();
    if (bone < 0 || bone >= static_cast<i32>(boneCount)) return;
    // Parents precede children, so one forward pass collects the descendants.
    std::vector<char> in(boneCount, 0);
    in[static_cast<usize>(bone)] = 1;
    for (u32 b = static_cast<u32>(bone); b < boneCount; ++b) {
        if (b != static_cast<u32>(bone) && parents[b] >= 0 && in[static_cast<usize>(parents[b])]) in[b] = 1;
        if (in[b]) out.push_back(static_cast<i32>(b));
    }
}

void ResolvedPhysAnim::withinHops(i32 bone, i32 hops, std::vector<std::pair<i32, i32>>& out) const {
    out.clear();
    if (bone < 0 || bone >= static_cast<i32>(boneCount)) return;
    std::vector<i32> dist(boneCount, -1);
    std::queue<i32> q;
    dist[static_cast<usize>(bone)] = 0;
    q.push(bone);
    // Neighbours are the parent and the children (a scan: skeletons are small and this runs per hit).
    while (!q.empty()) {
        const i32 b = q.front();
        q.pop();
        out.emplace_back(b, dist[static_cast<usize>(b)]);
        if (dist[static_cast<usize>(b)] >= hops) continue;
        const i32 p = parents[static_cast<usize>(b)];
        if (p >= 0 && dist[static_cast<usize>(p)] < 0) { dist[static_cast<usize>(p)] = dist[static_cast<usize>(b)] + 1; q.push(p); }
        for (u32 c = static_cast<u32>(b) + 1; c < boneCount; ++c)
            if (parents[c] == b && dist[c] < 0) { dist[c] = dist[static_cast<usize>(b)] + 1; q.push(static_cast<i32>(c)); }
    }
}

} // namespace aver::ragdoll
