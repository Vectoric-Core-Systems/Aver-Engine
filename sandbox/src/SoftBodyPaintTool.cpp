// The soft-body paint tool. See the header for the model; the ImGui panel is at the end.
#include "SoftBodyPaintTool.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#if AVER_WITH_IMGUI
#  include "imgui.h"
#endif

namespace aver::editor {
namespace {

Vec3 toWorld(const Vec3& p, const Mat4& m) {
    return {p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0],
            p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1],
            p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2]};
}

// Moller-Trumbore, double sided. t > 0 on a hit.
bool rayTriangle(const Vec3& o, const Vec3& d, const Vec3& a, const Vec3& b, const Vec3& c, f32& t) {
    const Vec3 e1 = b - a, e2 = c - a;
    const Vec3 p = cross(d, e2);
    const f32 det = dot(e1, p);
    if (std::fabs(det) < 1e-9f) return false;
    const f32 inv = 1.0f / det;
    const Vec3 s = o - a;
    const f32 u = dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    const Vec3 q = cross(s, e1);
    const f32 v = dot(d, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    t = dot(e2, q) * inv;
    return t > 1e-6f;
}

void cross3(std::vector<PaintLine>& out, const Mat4& w, const Vec3& p, f32 size, f32 r, f32 g, f32 b) {
    const Vec3 axes[3] = {{size, 0, 0}, {0, size, 0}, {0, 0, size}};
    for (const Vec3& ax : axes) out.push_back({toWorld(p - ax, w), toWorld(p + ax, w), r, g, b});
}

}  // namespace

void SoftBodyPaintTool::reachColour(u8 byte, f32& r, f32& g, f32& b) {
    if (byte == 255) { r = g = b = 0.5f; return; }
    const f32 t = std::clamp(static_cast<f32>(byte) / 254.0f, 0.0f, 1.0f);
    r = t; g = 1.0f - t; b = 0.0f;
}

bool SoftBodyPaintTool::load(const fmt::OcMeshData& mesh, const softbody::SoftAsset& asset, const std::string& assetPath) {
    close();
    mesh_ = mesh;
    asset_ = asset;
    path_ = assetPath;
    softbody::SimMeshInput in;
    in.positions = mesh_.positions.data();
    in.uvs = mesh_.uvs.empty() ? nullptr : mesh_.uvs.data();
    in.indices = mesh_.indices.data();
    in.vertexCount = mesh_.vertexCount();
    in.indexCount = static_cast<u32>(mesh_.indices.size());
    if (mesh_.hasSkin()) { in.joints = mesh_.joints.data(); in.weights = mesh_.weights.data(); }
    softbody::SimBuildSpec spec;
    spec.material = asset_.material;
    spec.weldCm = asset_.sim.weldCm;
    spec.maxParticles = 65536;     // editing must work on a mesh the runtime would refuse
    spec.destructible = true;      // no default pin: this tool is how pins are chosen
    if (mesh_.positions.empty() || !softbody::buildSimMesh(in, spec, sim_)) return false;

    const u32 n = particleCount();
    pin_.assign(n, 0);
    reach_.assign(n, 255);
    staleOnLoad_ = asset_.paint.stale(sim_.meshKey);
    if (!staleOnLoad_) {
        if (asset_.paint.pin.size() == n) pin_ = asset_.paint.pin;
        if (asset_.paint.reach.size() == n) reach_ = asset_.paint.reach;
    }
    undo_.clear();
    dirty_ = false;
    active_ = true;
    return true;
}

void SoftBodyPaintTool::close() {
    active_ = false;
    dirty_ = false;
    staleOnLoad_ = false;
    inStroke_ = false;
    pin_.clear();
    reach_.clear();
    sim_ = softbody::SimMesh{};
    mesh_ = fmt::OcMeshData{};
    asset_ = softbody::SoftAsset{};
    path_.clear();
    undo_.clear();
}

bool SoftBodyPaintTool::pick(const Vec3& origin, const Vec3& dir, Vec3& hit, u32* triangle) const {
    if (!active_) return false;
    const softbody::Cage& c = sim_.cage;
    f32 best = 1e30f;
    u32 bestTri = softbody::kNone;
    for (u32 t = 0; t < c.triangles.size(); ++t) {
        const softbody::Triangle& tri = c.triangles[t];
        f32 hitT = 0.0f;
        if (rayTriangle(origin, dir, c.particles[tri.v[0]].pos, c.particles[tri.v[1]].pos, c.particles[tri.v[2]].pos, hitT) && hitT < best) {
            best = hitT;
            bestTri = t;
        }
    }
    if (bestTri == softbody::kNone) return false;
    hit = origin + dir * best;
    if (triangle) *triangle = bestTri;
    return true;
}

std::vector<u32> SoftBodyPaintTool::select(const Vec3& centre, f32 radius) const {
    std::vector<u32> out;
    if (!active_) return out;
    const f32 rSq = radius * radius;
    for (u32 i = 0; i < sim_.cage.particles.size(); ++i)
        if (distSquared(sim_.cage.particles[i].pos, centre) <= rSq) out.push_back(i);
    return out;
}

SoftBodyPaintTool::State SoftBodyPaintTool::snapshot() const { return State{pin_, reach_, asset_.paint.rules}; }

void SoftBodyPaintTool::restore(const State& s) {
    pin_ = s.pin;
    reach_ = s.reach;
    asset_.paint.rules = s.rules;
}

void SoftBodyPaintTool::beginStroke() {
    if (!active_ || inStroke_) return;
    inStroke_ = true;
    strokeChanged_ = false;
    strokeBefore_ = snapshot();
}

u32 SoftBodyPaintTool::paint(const std::vector<u32>& particles) {
    if (!active_) return 0;
    const bool autoStroke = !inStroke_;
    if (autoStroke) beginStroke();
    u32 changed = 0;
    for (const u32 p : particles) {
        if (p >= pin_.size()) continue;
        u8 pin = pin_[p], reach = reach_[p];
        switch (brush.mode) {
            case Mode::Pin:
                pin = 255;
                break;
            case Mode::Reach:
                pin = 0;
                reach = static_cast<u8>(std::lround(std::clamp(brush.reach01, 0.0f, 1.0f) * 254.0f));
                break;
            case Mode::Erase:
                pin = 0;
                reach = 255;
                break;
        }
        if (pin != pin_[p] || reach != reach_[p]) {
            pin_[p] = pin;
            reach_[p] = reach;
            ++changed;
        }
    }
    if (changed) {
        strokeChanged_ = true;
        staleOnLoad_ = false;   // what is painted now belongs to this mesh
    }
    if (autoStroke) endStroke();
    return changed;
}

void SoftBodyPaintTool::endStroke() {
    if (!inStroke_) return;
    inStroke_ = false;
    if (strokeChanged_) {
        undo_.push(strokeBefore_);
        markDirty();
    }
    strokeChanged_ = false;
}

bool SoftBodyPaintTool::undo() {
    State cur = snapshot();
    if (!undo_.undo(cur)) return false;
    restore(cur);
    markDirty();
    return true;
}

bool SoftBodyPaintTool::redo() {
    State cur = snapshot();
    if (!undo_.redo(cur)) return false;
    restore(cur);
    markDirty();
    return true;
}

u32 SoftBodyPaintTool::pinnedCount() const {
    u32 n = 0;
    for (const u8 v : pin_) n += v == 255 ? 1u : 0u;
    return n;
}

u32 SoftBodyPaintTool::reachCount() const {
    u32 n = 0;
    for (u32 i = 0; i < reach_.size(); ++i) n += (reach_[i] != 255 && pin_[i] != 255) ? 1u : 0u;
    return n;
}

void SoftBodyPaintTool::addRule(const softbody::PaintRule& r) {
    if (!active_) return;
    undo_.push(snapshot());
    asset_.paint.rules.push_back(r);
    markDirty();
}

void SoftBodyPaintTool::removeRule(usize index) {
    if (!active_ || index >= asset_.paint.rules.size()) return;
    undo_.push(snapshot());
    asset_.paint.rules.erase(asset_.paint.rules.begin() + static_cast<std::ptrdiff_t>(index));
    markDirty();
}

u32 SoftBodyPaintTool::ruleCount(const softbody::PaintRule& r) const {
    if (!active_) return 0;
    softbody::Cage c = sim_.cage;
    softbody::PaintData pd;
    pd.rules.push_back(r);
    softbody::SimMeshInput in;
    in.positions = mesh_.positions.data();
    in.uvs = mesh_.uvs.empty() ? nullptr : mesh_.uvs.data();
    in.indices = mesh_.indices.data();
    in.vertexCount = mesh_.vertexCount();
    in.indexCount = static_cast<u32>(mesh_.indices.size());
    softbody::PaintEnv env;
    env.mesh = &in;
    env.binding = &sim_.binding;
    env.mask = mask_;
    env.boneWeight = bone_;
    env.meshKey = sim_.meshKey;
    env.defaultReachCm = asset_.cloth.defaultReachCm;
    return softbody::applyPaint(c, pd, env);
}

softbody::SoftAsset SoftBodyPaintTool::assetToSave() const {
    softbody::SoftAsset a = asset_;
    a.paint.meshKey = sim_.meshKey;
    bool anyPin = false, anyReach = false;
    for (const u8 v : pin_) anyPin = anyPin || v != 0;
    for (const u8 v : reach_) anyReach = anyReach || v != 255;
    // Reach is only read together with the pin array, so painting either writes both.
    if (anyPin || anyReach) { a.paint.pin = pin_; a.paint.reach = anyReach ? reach_ : std::vector<u8>{}; }
    else { a.paint.pin.clear(); a.paint.reach.clear(); }
    return a;
}

bool SoftBodyPaintTool::save(std::string* why) {
    if (!active_) { if (why) *why = "nothing is open"; return false; }
    if (path_.empty()) { if (why) *why = "the asset has no path yet; use Create .ocsoft first"; return false; }
    const softbody::SoftAsset a = assetToSave();
    if (!softbody::saveSoftAsset(path_, a, why)) return false;
    asset_ = a;
    dirty_ = false;
    return true;
}

bool SoftBodyPaintTool::viewportPaint(const Vec3& ro, const Vec3& rd, bool down) {
    hasHit_ = false;
    if (!active_) return false;
    // World -> mesh-local (row vectors): points take the translation, directions do not.
    const Mat4 inv = entityWorld.inverse();
    const Vec3 o = toWorld(ro, inv);
    const Vec3 d{rd.x * inv.m[0][0] + rd.y * inv.m[1][0] + rd.z * inv.m[2][0],
                 rd.x * inv.m[0][1] + rd.y * inv.m[1][1] + rd.z * inv.m[2][1],
                 rd.x * inv.m[0][2] + rd.y * inv.m[1][2] + rd.z * inv.m[2][2]};
    Vec3 hit;
    if (pick(o, d.getSafeNormal(), hit)) {
        hasHit_ = true;
        hitLocal_ = hit;
    }
    if (down && hasHit_) {
        if (!inStroke_) beginStroke();
        paint(select(hitLocal_, brush.radiusCm));
    } else {
        endStroke();
    }
    return hasHit_;
}

void SoftBodyPaintTool::collectLines(const Mat4& entityWorld, const Vec3* brushHitLocal, std::vector<PaintLine>& out) const {
    if (!active_) return;
    const f32 size = std::max(0.6f, brush.radiusCm * 0.08f);
    for (u32 i = 0; i < sim_.cage.particles.size(); ++i) {
        const Vec3& p = sim_.cage.particles[i].pos;
        if (pin_[i] == 255) {
            cross3(out, entityWorld, p, size, 1.0f, 0.9f, 0.1f);
        } else if (reach_[i] != 255) {
            f32 r, g, b;
            reachColour(reach_[i], r, g, b);
            cross3(out, entityWorld, p, size * 0.7f, r, g, b);
        }
    }
    if (brushHitLocal) {
        const Vec3 c = *brushHitLocal;
        const f32 rad = brush.radiusCm;
        constexpr int kSeg = 24;
        for (int plane = 0; plane < 3; ++plane)
            for (int k = 0; k < kSeg; ++k) {
                const f32 a0 = kTwoPi * static_cast<f32>(k) / kSeg, a1 = kTwoPi * static_cast<f32>(k + 1) / kSeg;
                const auto at = [&](f32 a) {
                    const f32 x = std::cos(a) * rad, y = std::sin(a) * rad;
                    return plane == 0 ? Vec3{c.x + x, c.y + y, c.z} : (plane == 1 ? Vec3{c.x + x, c.y, c.z + y} : Vec3{c.x, c.y + x, c.z + y});
                };
                out.push_back({toWorld(at(a0), entityWorld), toWorld(at(a1), entityWorld), 0.3f, 0.8f, 1.0f});
            }
    }
}

SoftBodyPaintTool& softBodyPaintTool() {
    static SoftBodyPaintTool tool;
    return tool;
}

#if AVER_WITH_IMGUI
bool SoftBodyPaintTool::drawPanel() {
    bool changed = false;
    if (!active_) {
        ImGui::TextDisabled("Press Paint on a Cloth or Destructible to edit its pins.");
        return false;
    }
    ImGui::Text("%u particles   %u pinned   %u with a reach limit", particleCount(), pinnedCount(), reachCount());
    if (staleOnLoad_)
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.2f, 1.0f), "The mesh changed since this was painted; the rules apply instead until you paint again.");
    int mode = static_cast<int>(brush.mode);
    ImGui::RadioButton("Pin", &mode, 0); ImGui::SameLine();
    ImGui::RadioButton("Reach", &mode, 1); ImGui::SameLine();
    ImGui::RadioButton("Erase", &mode, 2);
    brush.mode = static_cast<Mode>(mode);
    ImGui::SliderFloat("Radius (cm)", &brush.radiusCm, 0.5f, 100.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
    if (brush.mode == Mode::Reach) ImGui::SliderFloat("Reach (0 = hard skin)", &brush.reach01, 0.0f, 1.0f, "%.2f");
    ImGui::TextDisabled("Drag on the mesh in the viewport to paint. Yellow = pinned, green to red = reach.");

    ImGui::BeginDisabled(!undo_.canUndo());
    if (ImGui::SmallButton("Undo")) changed = undo() || changed;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!undo_.canRedo());
    if (ImGui::SmallButton("Redo")) changed = redo() || changed;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!dirty_ || path_.empty());
    if (ImGui::SmallButton("Save .ocsoft")) {
        std::string why;
        if (!save(&why)) ImGui::SetTooltip("Could not save: %s", why.c_str());
        changed = true;
    }
    ImGui::EndDisabled();
    if (path_.empty()) ImGui::TextDisabled("No .ocsoft yet: use Create .ocsoft above.");

    ImGui::Separator();
    ImGui::TextDisabled("Rules (mesh-local; they apply when nothing is painted or the paint is stale)");
    usize remove = rules().size();
    for (usize i = 0; i < asset_.paint.rules.size(); ++i) {
        softbody::PaintRule& r = asset_.paint.rules[i];
        ImGui::PushID(static_cast<int>(i));
        static const char* const kNames[] = {"Above", "Near bone", "Sphere", "Mask texture"};
        ImGui::Text("%s", kNames[static_cast<int>(r.type)]);
        ImGui::SameLine();
        switch (r.type) {
            case softbody::PaintRule::Type::Above: {
                static const char* const kAxes[] = {"X", "Y", "Z"};
                int ax = r.axis < 3 ? r.axis : 2;
                ImGui::SetNextItemWidth(48);
                if (ImGui::Combo("##axis", &ax, kAxes, 3)) { undo_.push(snapshot()); r.axis = static_cast<u8>(ax); markDirty(); changed = true; }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90);
                if (ImGui::DragFloat("##value", &r.valueCm, 0.5f, -1e5f, 1e5f, ">= %.1f cm")) { markDirty(); changed = true; }
                break;
            }
            case softbody::PaintRule::Type::Bone: {
                char buf[64];
                std::snprintf(buf, sizeof buf, "%s", r.bone.c_str());
                ImGui::SetNextItemWidth(120);
                if (ImGui::InputText("##bone", buf, sizeof buf)) { r.bone = buf; markDirty(); changed = true; }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70);
                if (ImGui::DragFloat("##w", &r.minWeight, 0.01f, 0.0f, 1.0f, "w >= %.2f")) { markDirty(); changed = true; }
                break;
            }
            case softbody::PaintRule::Type::Sphere:
                ImGui::SetNextItemWidth(160);
                if (ImGui::DragFloat3("##c", r.centreCm, 0.5f)) { markDirty(); changed = true; }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70);
                if (ImGui::DragFloat("##r", &r.radiusCm, 0.5f, 0.1f, 1e4f, "r %.1f")) { markDirty(); changed = true; }
                break;
            case softbody::PaintRule::Type::Mask: {
                char buf[128];
                std::snprintf(buf, sizeof buf, "%s", r.texture.c_str());
                ImGui::SetNextItemWidth(200);
                if (ImGui::InputText("##tex", buf, sizeof buf)) { r.texture = buf; markDirty(); changed = true; }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70);
                if (ImGui::DragFloat("##pb", &r.pinBelow, 0.01f, 0.0f, 1.0f, "R < %.2f")) { markDirty(); changed = true; }
                break;
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("pins %u", ruleCount(r));
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) remove = i;
        ImGui::PopID();
    }
    if (remove < asset_.paint.rules.size()) { removeRule(remove); changed = true; }

    if (ImGui::SmallButton("+ Above")) { softbody::PaintRule r; r.type = softbody::PaintRule::Type::Above; addRule(r); changed = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Bone")) { softbody::PaintRule r; r.type = softbody::PaintRule::Type::Bone; r.bone = "spine_03"; addRule(r); changed = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Sphere")) { softbody::PaintRule r; r.type = softbody::PaintRule::Type::Sphere; r.radiusCm = 8.0f; addRule(r); changed = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Mask")) { softbody::PaintRule r; r.type = softbody::PaintRule::Type::Mask; r.texture = "Textures/mask.png"; addRule(r); changed = true; }
    return changed;
}
#endif

}  // namespace aver::editor
