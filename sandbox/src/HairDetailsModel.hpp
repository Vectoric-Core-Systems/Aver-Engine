#pragma once
// The pure half of the Hair Details section (docs/HAIR.md "The editor"): what the COMPONENT Hair record keys mean and when a key is at its default (so
// the canonical record omits it), the advice and state lines, and the arithmetic of the numbers the section shows. Header-only and ImGui-free, so a
// test can drive every rule; HairGroomTab.cpp is the widget half, in its own translation unit because SandboxComponentDetails.cpp is at the Debug
// COFF section cap.
//
// The component state itself is read and written through game::applyPlacedComponent / capturePlacedComponent (LevelComponents.cpp), so what the
// section edits is exactly what the level saves and reloads; this header only knows the record spelling.
//
//   COMPONENT Hair  asset=<path of the .ochair> width=<scale> density=<scale> lodbias=<steps> wind=<scale> stiffness=<scale> teleport=<cm>
//                   collision=0|1 shadow=0|1 windon=0|1 hidden=0|1
//
// A key at its default is omitted: width, density, wind, stiffness 1; lodbias, teleport 0; collision, shadow, windon 1; hidden 0.
//
// THE GROOM TAB (an .ochair opened as an asset, phase I) adds a second half to this header, below "the Groom tab": the rules for editing the
// records of a groom (clamps that keep OcHairData::valid() true, the draw budget, finding the scalp mesh), the 2D projection the root preview
// uses and, behind AVER_MODULE_HAIR, the preview itself (Aver.Hair's generator over the scalp). The widgets are HairGroomTab.cpp's.
#include "aver/core/Types.hpp"
#include "aver/formats/OcHair.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/formats/OcWorld.hpp"

#if AVER_MODULE_HAIR
#  include "aver/hair/HairGenerate.hpp"
#  include "aver/hair/HairLod.hpp"
#  include "aver/hair/HairProxy.hpp"
#  include "aver/hair/HairSettingsBuild.hpp"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace aver::editor::hairmodel {

// ---- COMPONENT record keys ----------------------------------------------------------------------------------------

inline void setKey(fmt::OcPlacedComponent& r, const std::string& key, const std::string& value) {
    const std::string hexKey = key + "#";
    r.kv.erase(std::remove_if(r.kv.begin(), r.kv.end(), [&](const auto& kv) { return kv.first == key || kv.first == hexKey; }), r.kv.end());
    if (!value.empty()) r.kv.emplace_back(key, value);
}

inline double numberOf(const fmt::OcPlacedComponent& r, std::string_view key, double def) {
    const std::string* v = r.find(key);
    if (!v || v->empty()) return def;
    char* end = nullptr;
    const double d = std::strtod(v->c_str(), &end);
    return end && *end == '\0' ? d : def;
}

struct KeyDefault { const char* key; double def; };
inline constexpr KeyDefault kHairKeys[] = {{"width", 1}, {"density", 1}, {"lodbias", 0}, {"wind", 1}, {"stiffness", 1}, {"teleport", 0},
                                           {"collision", 1}, {"shadow", 1}, {"windon", 1}, {"hidden", 0}};

// Drops every key that is at its default. Idempotent.
inline void canonicalize(fmt::OcPlacedComponent& r) {
    if (r.type != "Hair") return;
    for (const KeyDefault& k : kHairKeys) {
        const std::string* v = r.find(k.key);
        if (v && numberOf(r, k.key, k.def + 1.0) == k.def) setKey(r, k.key, std::string());
    }
}

// A fresh record for Add Component: the asset path (may be empty; the section then asks for one) and nothing else.
inline fmt::OcPlacedComponent newRecord(const std::string& assetPath = std::string()) {
    fmt::OcPlacedComponent r;
    r.type = "Hair";
    if (!assetPath.empty()) r.kv.emplace_back("asset", assetPath);
    return r;
}

// The CHair flag bits a record's switches mean (hair::kHair*), so the applier and the capturer agree with the section.
inline u32 flagsOf(const fmt::OcPlacedComponent& r) {
    u32 f = 0;
    if (numberOf(r, "collision", 1) == 0) f |= 0x1;   // kHairNoCollision
    if (numberOf(r, "shadow", 1) == 0) f |= 0x2;      // kHairNoShadow
    if (numberOf(r, "windon", 1) == 0) f |= 0x8;      // kHairNoWind
    if (numberOf(r, "hidden", 0) != 0) f |= 0x10;     // kHairHidden
    return f;
}

// ---- level of detail --------------------------------------------------------------------------------------------

// The strand-count levels of HairLod.hpp (kHairLodLevels); repeated here so this header needs no Aver.Hair include in a build without the module.
inline constexpr u32 kLodLevels = 4;

inline const char* lodName(u32 level) {
    static const char* names[kLodLevels] = {"every strand", "half the strands", "a quarter of the strands", "an eighth of the strands (fat ribbons)"};
    return names[level < kLodLevels ? level : kLodLevels - 1];
}

// ---- advice and state lines (CHair::outState: bit0 simulating, bit1 frozen, bit2 inert, bit3 building, level from bit 8) ----------------------

struct HairFacts {
    bool hasAsset = false;
    bool hasSkeleton = false;      // the entity carries a skeletal mesh (a groom on a rigid head works, but the scalp then follows one bone)
    bool playing = false;
    u32 outState = 0;
    u32 renderStrands = 0, simStrands = 0;
    bool computeHost = true;       // a GPU compute host is installed; false = Jolt's CPU backend (small grooms only)
};

inline std::string advice(const HairFacts& f) {
    if (!f.hasAsset) return "Assign an .ochair groom. Make one with AverAssetC hair, or write a GENERATE record (docs/HAIR.md).";
    if (!f.hasSkeleton) return "No skeletal mesh on this entity: the scalp follows a single bone, and the head, neck and shoulder collision proxies are not made.";
    if (f.outState & 0x4u) return "The groom was not built (see the log): a missing scalp joint, roots too far from the scalp, or no compute device.";
    if (!f.computeHost && f.simStrands > 64) return "No GPU compute host: Jolt's CPU backend is for tests and takes grooms of at most 64 simulated strands.";
    return std::string();
}

inline std::string stateText(const HairFacts& f) {
    if (!f.playing && !(f.outState & 0xFu)) return std::string();
    const u32 lod = f.outState >> 8;
    const char* what = (f.outState & 0x4u) ? "inert" : (f.outState & 0x8u) ? "building" : (f.outState & 0x2u) ? "frozen (far away or unseen)"
                     : (f.outState & 0x1u) ? "simulating" : "idle";
    char b[200];
    std::snprintf(b, sizeof b, "%s; level %u: %s; %u strands drawn, %u simulated", what, lod, lodName(lod), f.renderStrands, f.simStrands);
    return b;
}

// ---- the Add Component row ---------------------------------------------------------------------------------------------

inline constexpr const char* kMenuLabel = "Hair";
inline constexpr const char* kMenuTip = "Strand hair on this character: a groom (.ochair) simulated by Jolt, drawn as ribbons. Needs a skeletal mesh for the collision proxies.";

// ======================================================= the Groom tab: editing an .ochair =======================================================

// What the groom as it stands will cost to draw (the same arithmetic as HairSystem's build, so the tab warns before the build refuses).
struct GroomBudget {
    u64 renderStrands = 0;
    u64 renderVertices = 0;
    u64 simStrands = 0;
    u64 drawBytes = 0;
    bool overStrandCap = false;     // more strands than MAXSTRANDS
    bool overBudget = false;        // the draw buffers would pass the hair VRAM budget
};

// `records` is the groom without its stored strands; the stored strand and point counts come beside it (the tab keeps the strands out of its undo
// snapshots, so it never has the whole groom in one piece).
inline GroomBudget groomBudgetOf(const fmt::OcHairData& g, u64 storedStrands, u64 storedPoints, u32 budgetMB = 96) {
    GroomBudget b;
    if (storedStrands > 0) {
        b.renderStrands = storedStrands;
        b.renderVertices = storedPoints;
    } else if (g.generate.present) {
        b.renderStrands = g.generate.strands;
        b.renderVertices = static_cast<u64>(g.generate.strands) * (static_cast<u64>(g.generate.segments) + 1u);
    }
    b.overStrandCap = b.renderStrands > g.maxRenderStrands;
    const f32 frac = std::clamp(g.sim.simFraction, 0.0f, 1.0f);
    b.simStrands = b.renderStrands == 0 ? 0 : std::max<u64>(1, static_cast<u64>(static_cast<f64>(b.renderStrands) * frac + 0.5));
    const u64 segs = b.renderVertices > b.renderStrands ? b.renderVertices - b.renderStrands : 0;
    b.drawBytes = b.renderVertices * 2 * 32 + b.renderVertices * 28 + segs * 6 * 4 * 2 + b.renderVertices * 12;
    b.overBudget = b.drawBytes > static_cast<u64>(budgetMB) * 1024ull * 1024ull;
    return b;
}

inline GroomBudget groomBudget(const fmt::OcHairData& g, u32 budgetMB = 96) {
    return groomBudgetOf(g, g.strands.strandCount(), g.strands.pointCount(), budgetMB);
}

// Gradient fractions may not coincide (valid() refuses it); a nudge keeps the sliders from walking into that.
inline void fixGradient(fmt::OcHairGradient& g) {
    auto fin = [](f32& v, f32 def) { if (!std::isfinite(v)) v = def; };
    fin(g.min, 0.0f); fin(g.max, 0.0f); fin(g.minFraction, 0.0f); fin(g.maxFraction, 1.0f);
    g.minFraction = std::clamp(g.minFraction, 0.0f, 1.0f);
    g.maxFraction = std::clamp(g.maxFraction, 0.0f, 1.0f);
    if (g.minFraction == g.maxFraction) {
        if (g.maxFraction <= 0.9f) g.maxFraction = g.minFraction + 0.1f;
        else g.minFraction = g.maxFraction - 0.1f;
    }
}

// Clamps every number the tab can reach into what OcHairData::valid() accepts, and makes sure a groom has a material and a look for index 0.
// Idempotent. It never touches the strands, the scalp or the names; a record that is wrong for another reason stays wrong and valid() says why.
inline void sanitize(fmt::OcHairData& g) {
    auto fin = [](f32& v, f32 def) { if (!std::isfinite(v)) v = def; };
    // SIM
    fmt::OcHairSim& s = g.sim;
    fin(s.iterationsPerSecond, 360.0f); fin(s.maxDeltaTime, 0.0333f); fin(s.boundsPaddingCm, 10.0f); fin(s.simFraction, 0.1f);
    s.iterationsPerSecond = std::clamp(s.iterationsPerSecond, 1.0f, 4096.0f);
    s.maxDeltaTime = std::clamp(s.maxDeltaTime, 1.0e-4f, 1.0f);
    for (f32& c : s.grid) { fin(c, 32.0f); c = std::clamp(std::round(c), 4.0f, 128.0f); }
    s.boundsPaddingCm = std::max(s.boundsPaddingCm, 0.0f);
    s.simFraction = std::clamp(s.simFraction, 0.001f, 1.0f);
    if (s.resample != 0) s.resample = std::clamp<u32>(s.resample, fmt::kOcHairMinStrandVertices, fmt::kOcHairMaxStrandVertices);
    for (f32& c : s.initialGravity) fin(c, 0.0f);
    // MATERIAL
    for (fmt::OcHairMaterial& m : g.materials) {
        for (f32* v : {&m.linearDamping, &m.angularDamping, &m.friction, &m.bendCompliance, &m.stretchCompliance, &m.gridDensityForce, &m.gravityPreload}) {
            fin(*v, 0.0f);
            *v = std::max(*v, 0.0f);
        }
        for (f32* v : {&m.maxLinearCmS, &m.maxAngularRadS, &m.inertiaMul}) {
            fin(*v, 1.0f);
            *v = std::max(*v, 1.0e-3f);
        }
        for (f32& v : m.bendMul) { fin(v, 1.0f); v = std::max(v, 0.0f); }
        fin(m.simFraction, -1.0f);
        if (m.simFraction >= 0.0f) m.simFraction = std::clamp(m.simFraction, 0.001f, 1.0f);
        for (fmt::OcHairGradient* gr : {&m.gravity, &m.radiusCm, &m.worldInfluence, &m.gridVelocity, &m.globalPose, &m.skinGlobalPose}) fixGradient(*gr);
        m.radiusCm.min = std::max(m.radiusCm.min, 0.0f);
        m.radiusCm.max = std::max(m.radiusCm.max, 0.0f);
    }
    // LOOK
    for (fmt::OcHairLook& l : g.looks) {
        for (f32* c : {&l.baseColor[0], &l.baseColor[1], &l.baseColor[2], &l.tipColor[0], &l.tipColor[1], &l.tipColor[2]}) { fin(*c, 0.0f); *c = std::max(*c, 0.0f); }
        for (f32& c : l.tint) fin(c, 1.0f);
        fin(l.colorVariation, 0.0f); fin(l.roughness, 0.35f); fin(l.shift, 0.07f); fin(l.tt, 1.0f); fin(l.trt, 1.0f); fin(l.azimuthRough, 0.4f); fin(l.scatter, 0.5f);
        fin(l.widthRootCm, 0.1f); fin(l.widthTipCm, 0.04f);
        l.widthRootCm = std::max(l.widthRootCm, 0.001f);
        l.widthTipCm = std::max(l.widthTipCm, 0.0f);
    }
    // GENERATE
    if (g.generate.present) {
        fmt::OcHairGenerate& n = g.generate;
        n.strands = std::clamp<u32>(n.strands, 1u, std::max<u32>(g.maxRenderStrands, 1u));
        n.segments = std::clamp<u32>(n.segments, 2u, fmt::kOcHairMaxStrandVertices - 1u);
        for (f32& v : n.lengthCm) fin(v, 30.0f);
        n.lengthCm[0] = std::max(n.lengthCm[0], 0.1f);
        n.lengthCm[1] = std::max(n.lengthCm[1], n.lengthCm[0]);
        fin(n.curlCm, 0.0f); fin(n.curlFrequency, 3.0f); fin(n.clumpRadiusCm, 1.5f); fin(n.clumpTightness, 0.6f); fin(n.partWidthCm, 0.0f); fin(n.styleWeight, 0.0f);
        n.curlCm = std::max(n.curlCm, 0.0f);
        n.curlFrequency = std::max(n.curlFrequency, 0.0f);
        n.clumpRadiusCm = std::max(n.clumpRadiusCm, 0.0f);
        n.clumpTightness = std::clamp(n.clumpTightness, 0.0f, 1.0f);
        n.partWidthCm = std::max(n.partWidthCm, 0.0f);
        n.styleWeight = std::clamp(n.styleWeight, 0.0f, 1.0f);
        for (f32& v : n.partDirection) fin(v, 0.0f);
        for (f32& v : n.style) fin(v, 0.0f);
        if (n.styleWeight > 0.0f && n.style[0] == 0.0f && n.style[1] == 0.0f && n.style[2] == 0.0f) n.style[2] = -1.0f;
        n.material = std::min<u32>(n.material, fmt::kOcHairMaxMaterials);
    }
    // LOD: three increasing distances, four non-increasing fractions in (0, 1]
    {
        f32 last = 0.0f;
        for (f32& d : g.lod.distances) {
            fin(d, last + 100.0f);
            d = std::max(d, last + 1.0f);
            last = d;
        }
        f32 prev = 1.0f;
        for (f32& f : g.lod.strandFractions) {
            fin(f, prev);
            f = std::clamp(f, 0.01f, prev);
            prev = f;
        }
    }
    // PROXY
    for (fmt::OcHairProxy& p : g.proxies) {
        if (p.name.empty()) p.name = "proxy";
        for (f32& v : p.sizeCm) fin(v, 0.0f);
        for (f32& v : p.offsetCm) fin(v, 0.0f);
        p.sizeCm[0] = std::max(p.sizeCm[0], 0.1f);
        if (p.shape == fmt::OcHairProxyShape::Capsule) p.sizeCm[1] = std::max(p.sizeCm[1], 0.1f);
        if (p.shape == fmt::OcHairProxyShape::Box) { p.sizeCm[1] = std::max(p.sizeCm[1], 0.1f); p.sizeCm[2] = std::max(p.sizeCm[2], 0.1f); }
    }
    if (g.maxRenderStrands == 0) g.maxRenderStrands = 1;
    if (g.materials.empty()) g.materials.push_back(fmt::OcHairMaterial{});
    if (g.looks.empty()) g.looks.push_back(fmt::OcHairLook{});
}

// What "New Hair Groom" writes: a generated groom (no strands stored) with one material and one look, valid as it stands, so the tab opens it. It
// names no scalp: the tab asks for one, and the preview waits for it.
inline fmt::OcHairData starterGroom() {
    fmt::OcHairData g;
    g.generate.present = true;
    g.generate.seed = 1;
    g.generate.strands = 4000;
    g.generate.lengthCm[0] = 18.0f;
    g.generate.lengthCm[1] = 26.0f;
    g.generate.segments = 10;
    g.generate.clumps = 60;
    g.materials.push_back(fmt::OcHairMaterial{});
    g.looks.push_back(fmt::OcHairLook{});
    return g;
}

// The advice lines for a groom open in the tab (empty when there is none): what would stop the build or what costs more than it should.
inline std::vector<std::string> groomAdvice(const fmt::OcHairData& g, const GroomBudget& b, bool previewHasScalp) {
    std::vector<std::string> out;
    std::string why;
    if (!g.valid(&why)) out.push_back("Not valid yet: " + why);
    if (g.scalpMesh.empty() && !g.hasScalp) out.push_back("No scalp mesh: the groom cannot be built or previewed until it names one (Scalp section).");
    else if (!previewHasScalp) out.push_back("The scalp mesh could not be read, so there is no preview; the path is relative to the project's Content folder.");
    if (b.overStrandCap) out.push_back("More strands than MAXSTRANDS: raise it or lower the count.");
    if (b.overBudget) out.push_back("The draw buffers would pass the hair VRAM budget (96 MB by default); the build refuses such a groom. Lower the strand count or the points per strand.");
    if (g.sim.simFraction > 0.25f) out.push_back("Simulating more than a quarter of the strands is costly; the render strands between them are interpolated.");
    if (g.scalpJoints.empty()) out.push_back("No scalp joints: the scalp follows one bone. List them (Scalp section) for a skinned head.");
    return out;
}

// ---- finding the scalp mesh ---------------------------------------------------------------------------------------------------------------------

// A groom names its scalp relative to the project's Content folder; the editor knows only the groom's absolute path, so it looks for the mesh beside
// the groom and in each parent folder up to eight levels, which finds it for any layout that keeps the groom under Content. Empty when it is not found.
inline std::string resolveContentPath(const std::string& assetAbsPath, const std::string& relative) {
    if (relative.empty()) return std::string();
    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::path(relative).lexically_normal();
    if (rel.is_absolute()) return std::filesystem::exists(rel, ec) ? rel.string() : std::string();
    std::filesystem::path dir = std::filesystem::path(assetAbsPath).parent_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        const std::filesystem::path cand = dir / rel;
        if (std::filesystem::exists(cand, ec) && !ec) return cand.string();
        const std::filesystem::path up = dir.parent_path();
        if (up == dir) break;
        dir = up;
    }
    return std::string();
}

// ---- the 2D preview: engine axes, +Z up --------------------------------------------------------------------------------------------------------

enum class PreviewView : int { Front = 0, Side = 1, Top = 2 };   // XZ, YZ, XY

inline const char* previewViewName(PreviewView v) {
    switch (v) {
        case PreviewView::Front: return "Front (X right, Z up)";
        case PreviewView::Side: return "Side (Y right, Z up)";
        default: return "Top (X right, Y up)";
    }
}

// A point (x y z, engine) onto the view's plane: u to the right, v up.
inline void projectPoint(PreviewView view, const f32 p[3], f32& u, f32& v) {
    switch (view) {
        case PreviewView::Front: u = p[0]; v = p[2]; break;
        case PreviewView::Side: u = p[1]; v = p[2]; break;
        default: u = p[0]; v = p[1]; break;
    }
}

// Scale and centre that fit the preview's bounds into a width x height box with a margin; pixels per centimetre and the (u, v) the box's centre shows.
struct PreviewFit {
    f32 pixelsPerCm = 1.0f;
    f32 centreU = 0.0f, centreV = 0.0f;
};

inline PreviewFit fitPreview(PreviewView view, const f32 bmin[3], const f32 bmax[3], f32 width, f32 height, f32 margin = 12.0f) {
    f32 u0, v0, u1, v1;
    projectPoint(view, bmin, u0, v0);
    projectPoint(view, bmax, u1, v1);
    if (u0 > u1) std::swap(u0, u1);
    if (v0 > v1) std::swap(v0, v1);
    const f32 w = std::max(u1 - u0, 1.0e-3f), h = std::max(v1 - v0, 1.0e-3f);
    PreviewFit f;
    f.pixelsPerCm = std::max(std::min((width - 2.0f * margin) / w, (height - 2.0f * margin) / h), 1.0e-3f);
    f.centreU = 0.5f * (u0 + u1);
    f.centreV = 0.5f * (v0 + v1);
    return f;
}

// Pixels (x right, y down) inside a box of width x height.
inline void toPixels(const PreviewFit& fit, f32 u, f32 v, f32 width, f32 height, f32& px, f32& py) {
    px = 0.5f * width + (u - fit.centreU) * fit.pixelsPerCm;
    py = 0.5f * height - (v - fit.centreV) * fit.pixelsPerCm;
}

// What the preview shows: the scalp's edges (thinned), the roots and a few guide strands as segments, all in the groom's scalp space.
struct GroomPreview {
    std::vector<f32> scalpEdges;     // 6 per edge
    std::vector<f32> roots;          // 3 per root
    std::vector<f32> segments;       // 6 per segment (a guide strand is up to 12 of them)
    f32 boundsMin[3] = {0, 0, 0};
    f32 boundsMax[3] = {0, 0, 0};
    u32 strandsShown = 0;
    u32 strandsInGroom = 0;
    f32 spacingCm = 0.0f;
    bool ok = false;
    std::string error;
};

inline void previewGrowBounds(GroomPreview& p, const f32 pt[3], bool& first) {
    for (int k = 0; k < 3; ++k) {
        if (first) { p.boundsMin[k] = p.boundsMax[k] = pt[k]; }
        else { p.boundsMin[k] = std::min(p.boundsMin[k], pt[k]); p.boundsMax[k] = std::max(p.boundsMax[k], pt[k]); }
    }
    first = false;
}

// The strands of `strands` as at most `maxStrands` guide polylines (every nth), each cut to at most 12 segments, plus the roots.
inline void previewAddStrands(GroomPreview& p, const fmt::OcHairStrands& strands, u32 maxStrands, bool& first) {
    const usize n = strands.strandCount();
    p.strandsInGroom = static_cast<u32>(n);
    if (n == 0 || strands.start.size() < n + 1) return;
    const usize step = std::max<usize>(1, n / std::max<u32>(maxStrands, 1u));
    for (usize s = 0; s < n; s += step) {
        const u32 first0 = strands.start[s];
        const u32 count = strands.start[s + 1] - first0;
        if (count < 2 || (static_cast<usize>(first0) + count) * 3 > strands.points.size()) continue;
        const f32* root = &strands.points[static_cast<usize>(first0) * 3];
        p.roots.insert(p.roots.end(), {root[0], root[1], root[2]});
        previewGrowBounds(p, root, first);
        const u32 segs = std::min<u32>(count - 1, 12u);
        u32 prev = 0;
        for (u32 k = 1; k <= segs; ++k) {
            const u32 cur = (k * (count - 1) + segs / 2) / segs;
            const f32* a = &strands.points[(static_cast<usize>(first0) + prev) * 3];
            const f32* b = &strands.points[(static_cast<usize>(first0) + cur) * 3];
            p.segments.insert(p.segments.end(), {a[0], a[1], a[2], b[0], b[1], b[2]});
            previewGrowBounds(p, b, first);
            prev = cur;
        }
        ++p.strandsShown;
    }
}

#if AVER_MODULE_HAIR
// The scalp a groom grows on, as the generator takes it: the embedded one, else the .ocmesh it names (found beside the groom or in a parent folder).
// False with `why` when there is none or it cannot be read.
// `embedded` is the groom's SCLP when it has one (null otherwise); `g` carries the SCALP name and submesh.
inline bool loadPreviewScalp(const fmt::OcHairData& g, const fmt::OcHairScalp* embedded, const std::string& assetAbsPath, hair::ScalpMesh& out,
                             std::string* why = nullptr) {
    auto fail = [&](const std::string& m) { if (why) *why = m; return false; };
    hair::BuildScalp bs;
    if (embedded) {
        fmt::OcHairData tmp;
        tmp.hasScalp = true;
        tmp.scalp = *embedded;
        tmp.scalpJoints = g.scalpJoints;
        if (!hair::scalpFromGroom(tmp, bs, why)) return false;
    } else {
        if (g.scalpMesh.empty()) return fail("the groom names no scalp mesh and embeds none");
        const std::string path = resolveContentPath(assetAbsPath, g.scalpMesh);
        if (path.empty()) return fail("the scalp mesh '" + g.scalpMesh + "' was not found beside the groom or in a parent folder");
        fmt::OcMeshData md;
        if (!fmt::loadOcMesh(path, md, why)) return false;
        if (!hair::scalpFromMesh(md, g.scalpSubmesh, {}, {}, bs, why)) return false;
    }
    out = hair::generatorScalp(bs);
    return true;
}

// Builds the preview: the scalp's edges, and either the stored strands (every nth) or the GENERATE record grown at most `maxStrands` strong over
// the scalp (the same seed, so the roots are a thinner version of the real thing, not the real thing). Does not touch the groom.
// `stored` is the groom's STRN (empty for a generated groom), `g` the records.
inline bool buildPreview(const fmt::OcHairData& g, const fmt::OcHairStrands& stored, const hair::ScalpMesh& scalp, u32 maxStrands, GroomPreview& out) {
    out = GroomPreview{};
    bool first = true;
    // The scalp edges, from at most about 800 triangles so a dense head stays cheap to draw.
    const usize tris = scalp.triangleCount();
    const usize triStep = std::max<usize>(1, tris / 800);
    for (usize t = 0; t < tris; t += triStep) {
        const u32 i0 = scalp.triangles[t * 3], i1 = scalp.triangles[t * 3 + 1], i2 = scalp.triangles[t * 3 + 2];
        if (std::max({i0, i1, i2}) >= scalp.vertexCount()) continue;
        const f32* a = &scalp.positions[static_cast<usize>(i0) * 3];
        const f32* b = &scalp.positions[static_cast<usize>(i1) * 3];
        const f32* c = &scalp.positions[static_cast<usize>(i2) * 3];
        out.scalpEdges.insert(out.scalpEdges.end(), {a[0], a[1], a[2], b[0], b[1], b[2], b[0], b[1], b[2], c[0], c[1], c[2], c[0], c[1], c[2], a[0], a[1], a[2]});
        previewGrowBounds(out, a, first);
        previewGrowBounds(out, b, first);
        previewGrowBounds(out, c, first);
    }
    if (!stored.empty()) {
        previewAddStrands(out, stored, maxStrands, first);
        out.ok = out.strandsShown > 0;
        if (!out.ok) out.error = "the stored strands have nothing to draw";
        return out.ok;
    }
    if (!g.generate.present) {
        out.error = "the groom has neither strands nor a GENERATE record";
        return false;
    }
    fmt::OcHairGenerate gen = g.generate;
    gen.strands = std::min<u32>(gen.strands, std::max<u32>(maxStrands, 1u));
    gen.clumps = std::min<u32>(gen.clumps, gen.strands / 4u);
    fmt::OcHairStrands made;
    hair::GenerateStats stats;
    if (!hair::generateHair(gen, scalp, made, &stats, &out.error)) return false;
    out.spacingCm = stats.spacingCm;
    previewAddStrands(out, made, maxStrands, first);
    out.strandsInGroom = g.generate.strands;
    out.ok = out.strandsShown > 0;
    if (!out.ok) out.error = "the generator made no strands";
    return out.ok;
}

static_assert(kLodLevels == hair::kHairLodLevels, "the tab and HairLod.hpp must agree on the number of strand-count levels");

// The humanoid proxy set for the bone names the tab offers (the library's rules; a rig that names its bones otherwise renames them in the list).
inline std::vector<fmt::OcHairProxy> starterProxies() {
    return hair::defaultHumanoidProxies({"head", "neck", "spine_03", "clavicle_l", "clavicle_r"});
}
#endif

// ---- proxies, LOD and looks ----------------------------------------------------------------------------------------------------------------------

inline const char* proxyShapeName(fmt::OcHairProxyShape s) {
    switch (s) {
        case fmt::OcHairProxyShape::Sphere: return "sphere";
        case fmt::OcHairProxyShape::Capsule: return "capsule";
        default: return "box";
    }
}

// A proxy with a name not yet used in the list, valid as it stands.
inline fmt::OcHairProxy newProxy(const std::vector<fmt::OcHairProxy>& list, const std::string& bone = "head") {
    fmt::OcHairProxy p;
    p.bone = bone;
    p.shape = fmt::OcHairProxyShape::Sphere;
    p.sizeCm[0] = 10.0f;
    for (u32 i = 0;; ++i) {
        p.name = i == 0 ? "proxy" : "proxy" + std::to_string(i);
        const bool used = std::any_of(list.begin(), list.end(), [&](const fmt::OcHairProxy& q) { return q.name == p.name; });
        if (!used) break;
    }
    return p;
}

// How many strands each LOD level draws for a groom of `strands` render strands, and at what distance each level starts.
struct LodRow { f32 fromCm; f32 fraction; u64 strandsDrawn; };
inline std::array<LodRow, kLodLevels> lodRows(const fmt::OcHairLod& lod, u64 strands) {
    std::array<LodRow, kLodLevels> rows{};
    for (u32 i = 0; i < kLodLevels; ++i) {
        rows[i].fromCm = i == 0 ? 0.0f : lod.distances[i - 1];
        rows[i].fraction = lod.strandFractions[i];
        rows[i].strandsDrawn = static_cast<u64>(static_cast<f64>(strands) * lod.strandFractions[i] + 0.5);
    }
    return rows;
}

// The colour a LOOK shows in a swatch: root to tip in `steps` steps (linear, as stored; the tab converts for the display).
inline void lookGradient(const fmt::OcHairLook& l, int steps, std::vector<std::array<f32, 3>>& out) {
    out.clear();
    for (int i = 0; i < steps; ++i) {
        const f32 t = steps > 1 ? static_cast<f32>(i) / static_cast<f32>(steps - 1) : 0.0f;
        out.push_back({l.baseColor[0] + (l.tipColor[0] - l.baseColor[0]) * t, l.baseColor[1] + (l.tipColor[1] - l.baseColor[1]) * t,
                       l.baseColor[2] + (l.tipColor[2] - l.baseColor[2]) * t});
    }
}

} // namespace aver::editor::hairmodel
