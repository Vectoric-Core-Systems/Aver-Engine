#pragma once
// The physical animation profile tab's rules (WAVE2_UE W6 step 8): edits of a ragdoll::PhysAnimProfile (regions, their bones, the drive and
// envelope of each), the skeleton as a flat tree with each bone's region, and an offline hit preview that runs the real HitReaction envelope
// over the resolved profile so the heat on each bone and the envelope curve are exactly what a hit would do. Header-only, no ImGui, no world,
// no physics: every function is unit tested. Nothing is keyed on a bone name; names are whatever the skeleton and the profile hold.
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/ragdoll/HitReaction.hpp"
#include "aver/ragdoll/PhysAnimProfile.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace aver::editor::physanim {

using ragdoll::PhysAnimDrive;
using ragdoll::PhysAnimProfile;
using ragdoll::PhysAnimRegion;

// ---- regions ----
inline int regionIndex(const PhysAnimProfile& p, const std::string& name) {
    for (usize i = 0; i < p.regions.size(); ++i) if (p.regions[i].name == name) return static_cast<int>(i);
    return -1;
}
// A region name has no spaces (the file is one directive per line, names are single tokens) and is not taken.
inline bool regionNameOk(const PhysAnimProfile& p, const std::string& name) {
    if (name.empty() || name.find_first_of(" \t\r\n#") != std::string::npos) return false;
    return regionIndex(p, name) < 0;
}
inline std::string freshRegionName(const PhysAnimProfile& p, const std::string& base = "region") {
    if (regionNameOk(p, base)) return base;
    for (int i = 2;; ++i) {
        std::string n = base + std::to_string(i);
        if (regionNameOk(p, n)) return n;
    }
}
// Appends a region with the profile's defaults as its drive; returns its index or -1.
inline int addRegion(PhysAnimProfile& p, const std::string& name) {
    if (!regionNameOk(p, name)) return -1;
    PhysAnimRegion r;
    r.name = name;
    r.drive = p.defaults;
    p.regions.push_back(std::move(r));
    return static_cast<int>(p.regions.size()) - 1;
}
inline bool removeRegion(PhysAnimProfile& p, int index) {
    if (index < 0 || index >= static_cast<int>(p.regions.size())) return false;
    p.regions.erase(p.regions.begin() + index);
    return true;
}
inline bool renameRegion(PhysAnimProfile& p, int index, const std::string& name) {
    if (index < 0 || index >= static_cast<int>(p.regions.size()) || p.regions[static_cast<usize>(index)].name == name || !regionNameOk(p, name)) return false;
    p.regions[static_cast<usize>(index)].name = name;
    return true;
}
// When two regions name the same bone the later one wins, so order is meaning. Returns the new index or -1.
inline int moveRegion(PhysAnimProfile& p, int index, int delta) {
    const int to = index + delta;
    if (index < 0 || to < 0 || index >= static_cast<int>(p.regions.size()) || to >= static_cast<int>(p.regions.size()) || delta == 0) return -1;
    std::swap(p.regions[static_cast<usize>(index)], p.regions[static_cast<usize>(to)]);
    return to;
}

// ---- membership ----
// Puts a bone (or, with `subtree`, the bone and all below it) in the region, taking the same entry out of every other region so the file has one
// owner for it. False for a bad region or an empty name, or when it already was there.
inline bool assignBone(PhysAnimProfile& p, int region, const std::string& bone, bool subtree) {
    if (region < 0 || region >= static_cast<int>(p.regions.size()) || bone.empty()) return false;
    bool had = false;
    for (usize i = 0; i < p.regions.size(); ++i) {
        std::vector<std::string>& list = subtree ? p.regions[i].subtrees : p.regions[i].bones;
        const auto it = std::find(list.begin(), list.end(), bone);
        if (it == list.end()) continue;
        if (static_cast<int>(i) == region) had = true;
        else list.erase(it);
    }
    if (had) return false;
    std::vector<std::string>& mine = subtree ? p.regions[static_cast<usize>(region)].subtrees : p.regions[static_cast<usize>(region)].bones;
    mine.push_back(bone);
    return true;
}
// Removes the bone entry (both kinds) from the region.
inline bool unassignBone(PhysAnimProfile& p, int region, const std::string& bone) {
    if (region < 0 || region >= static_cast<int>(p.regions.size())) return false;
    PhysAnimRegion& r = p.regions[static_cast<usize>(region)];
    const usize before = r.bones.size() + r.subtrees.size();
    r.bones.erase(std::remove(r.bones.begin(), r.bones.end(), bone), r.bones.end());
    r.subtrees.erase(std::remove(r.subtrees.begin(), r.subtrees.end(), bone), r.subtrees.end());
    return r.bones.size() + r.subtrees.size() != before;
}

// ---- values ----
inline f32 clamp01(f32 v) { return std::min(1.0f, std::max(0.0f, v)); }
// The ranges the file validates against (validatePhysAnimProfile), applied while editing so the slider never writes a refused value.
inline void clampDrive(PhysAnimDrive& d) {
    d.strength = clamp01(d.strength);
    d.blend = clamp01(d.blend);
    d.boost = clamp01(d.boost);
    d.damping = std::max(0.0f, d.damping);
    d.torque = std::max(0.0f, d.torque);
    d.impulse = std::max(0.0f, d.impulse);
    d.attack = std::max(0.0f, d.attack);
    d.hold = std::max(0.0f, d.hold);
    d.decay = std::max(0.001f, d.decay);
}
inline void clampProfile(PhysAnimProfile& p) {
    clampDrive(p.defaults);
    for (PhysAnimRegion& r : p.regions) clampDrive(r.drive);
    p.falloffPerHop = clamp01(p.falloffPerHop);
    p.maxHops = std::max(0, std::min(32, p.maxHops));
    p.epsilon = std::max(1e-4f, std::min(0.5f, p.epsilon));
    p.fullImpulse = std::max(1.0f, p.fullImpulse);
    p.deathSeconds = std::max(0.0f, p.deathSeconds);
    p.getUpBlend = std::max(0.0f, p.getUpBlend);
}

// ---- the skeleton as the tab lists it ----
struct BoneRow {
    int bone = 0;
    int depth = 0;
    int region = -1;   // index into profile.regions, -1 = the defaults
};
// Depth-first, roots in skeleton order, each bone with the region the resolved profile gives it.
inline std::vector<BoneRow> boneRows(const PhysAnimProfile& p, const fmt::OcSkeleton& skel) {
    const ragdoll::ResolvedPhysAnim r = ragdoll::resolvePhysAnim(p, skel);
    const int n = static_cast<int>(skel.bones.size());
    std::vector<std::vector<int>> kids(static_cast<usize>(n));
    std::vector<int> roots;
    for (int i = 0; i < n; ++i) {
        const int par = skel.bones[static_cast<usize>(i)].parent;
        if (par >= 0 && par < n && par != i) kids[static_cast<usize>(par)].push_back(i);
        else roots.push_back(i);
    }
    std::vector<BoneRow> out;
    std::vector<char> seen(static_cast<usize>(n), 0);
    const auto walk = [&](auto&& self, int b, int depth) -> void {
        if (seen[static_cast<usize>(b)]) return;
        seen[static_cast<usize>(b)] = 1;
        out.push_back({b, depth, b < static_cast<int>(r.region.size()) ? r.region[static_cast<usize>(b)] : -1});
        for (const int k : kids[static_cast<usize>(b)]) self(self, k, depth + 1);
    };
    for (const int root : roots) walk(walk, root, 0);
    return out;
}
inline int boneIndexByName(const fmt::OcSkeleton& skel, const std::string& name) {
    for (usize i = 0; i < skel.bones.size(); ++i) if (skel.bones[i].name == name) return static_cast<int>(i);
    return -1;
}

// ---- the offline hit preview ----
struct HitPreview {
    bool valid = false;
    std::vector<f32> peak;        // per bone: the highest envelope reached
    std::vector<f32> peakBlend;   // per bone: the highest simulated-pose weight
    std::vector<f32> curve;       // the hit bone's envelope, one sample per `dt`
    f32 dt = 0.0f;
    f32 settleSeconds = 0.0f;     // when every envelope had run out (capped at the preview length)
    u32 bonesReached = 0;         // bones whose peak is above the profile's epsilon
};
// Hits `bone` with `impulse` (kg*cm/s) and runs the envelopes to rest. Reads exactly what RagdollSystem::applyHit reads: the resolved profile
// and HitReaction, so the preview and the game cannot disagree.
inline HitPreview previewHit(const PhysAnimProfile& p, const fmt::OcSkeleton& skel, int bone, f32 impulse, f32 maxSeconds = 4.0f, f32 dt = 1.0f / 60.0f) {
    HitPreview out;
    const int n = static_cast<int>(skel.bones.size());
    if (bone < 0 || bone >= n || dt <= 0.0f) return out;
    const ragdoll::ResolvedPhysAnim r = ragdoll::resolvePhysAnim(p, skel);
    ragdoll::HitReaction hr;
    hr.configure(r);
    hr.trigger(bone, Vec3{impulse, 0.0f, 0.0f});
    out.peak.assign(static_cast<usize>(n), 0.0f);
    out.peakBlend.assign(static_cast<usize>(n), 0.0f);
    out.dt = dt;
    const int steps = static_cast<int>(maxSeconds / dt);
    int lastActive = 0;
    for (int s = 0; s < steps; ++s) {
        hr.step(dt);
        out.curve.push_back(hr.envelope(bone));
        for (int b = 0; b < n; ++b) {
            out.peak[static_cast<usize>(b)] = std::max(out.peak[static_cast<usize>(b)], hr.envelope(b));
            out.peakBlend[static_cast<usize>(b)] = std::max(out.peakBlend[static_cast<usize>(b)], hr.blend(b));
        }
        if (hr.active()) lastActive = s + 1;
    }
    out.settleSeconds = static_cast<f32>(lastActive) * dt;
    for (int b = 0; b < n; ++b) if (out.peak[static_cast<usize>(b)] > p.epsilon) ++out.bonesReached;
    out.valid = true;
    return out;
}

}  // namespace aver::editor::physanim
