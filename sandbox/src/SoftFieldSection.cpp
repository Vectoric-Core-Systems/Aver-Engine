// The widget half of the soft-body depth Details: the Soft Field section and the Caps / Effects / Proxy / Debris subsections
// of the Cloth and Destructible sections. The rules are in SoftFieldDetailsModel.hpp and SoftBodyDetailsModel.hpp (ImGui-free,
// tested); this file only draws them. In its own translation unit because SandboxComponentDetails.cpp is near the Debug COFF
// section cap. docs/SOFTBODY.md "Authoring".
#include "SoftBodyDetailsModel.hpp"
#include "SoftFieldDetailsModel.hpp"
#include "SoftProxyTool.hpp"

#if AVER_WITH_IMGUI && AVER_MODULE_SOFTBODY_SCENE

#include "aver/scene/World.hpp"
#include "aver/softbody/SoftBodySystem.hpp"

#include "imgui.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace aver::editor {
namespace {

namespace fm = fieldmodel;
namespace sm = softmodel;

const ImVec4 kWarn{1.0f, 0.55f, 0.35f, 1.0f};

// An InputText over a std::string through a fixed buffer (no imgui_stdlib).
bool inputString(const char* label, std::string& s, usize cap = 256) {
    std::vector<char> buf(cap, '\0');
    std::memcpy(buf.data(), s.data(), std::min(s.size(), cap - 1));
    if (!ImGui::InputText(label, buf.data(), cap)) return false;
    s = buf.data();
    return true;
}

}  // namespace

void drawSoftFieldSection(const SoftSectionEnv& env, void* worldPtr, u32 entity) {
    auto& w = *static_cast<scene::World*>(worldPtr);
    const scene::Entity e = static_cast<scene::Entity>(entity);
    const u32 type = softbody::SoftBodySystem::softFieldType();
    auto* c = type ? w.component<softbody::CSoftField>(e, type) : nullptr;
    if (!c) return;
    if (!ImGui::CollapsingHeader("Soft Field", ImGuiTreeNodeFlags_DefaultOpen)) return;
    using softbody::FieldKind;
    using softbody::FieldShape;

    ImGui::BeginDisabled(env.playing);
    const FieldKind kind = c->kind < softbody::kFieldKindCount ? static_cast<FieldKind>(c->kind) : FieldKind::RadialForce;
    const FieldShape shape = c->shape < softbody::kFieldShapeCount ? static_cast<FieldShape>(c->shape) : FieldShape::Sphere;
    if (ImGui::BeginCombo("Kind", fm::kindRow(kind).label)) {
        for (const fm::KindRow& row : fm::kKindRows) {
            const bool picked = ImGui::Selectable(row.label, row.kind == kind);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", row.tip);
            if (picked) env.instantEdit(row.kind != kind, [&] { c->kind = static_cast<u32>(row.kind); });
        }
        ImGui::EndCombo();
    }
    if (ImGui::BeginCombo("Shape", fm::shapeRow(shape).label)) {
        for (const fm::ShapeRow& row : fm::kShapeRows)
            if (ImGui::Selectable(row.label, row.shape == shape))
                env.instantEdit(row.shape != shape, [&] { c->shape = static_cast<u32>(row.shape); });
        ImGui::EndCombo();
    }

    if (shape != FieldShape::Everywhere) {
        float ext[3] = {c->extentX > 0.0f ? c->extentX : 100.0f, c->extentY > 0.0f ? c->extentY : 100.0f, c->extentZ > 0.0f ? c->extentZ : 100.0f};
        env.gestureEdit(ImGui::DragFloat3(fm::extentLabel(shape), ext, 1.0f, 1.0f, 100000.0f, "%.0f cm"),
                        [&] { c->extentX = ext[0]; c->extentY = ext[1]; c->extentZ = ext[2]; });
        if (ImGui::SmallButton("Match primitive")) {
            fm::FieldValues v;
            v.shape = shape;
            v.extent[0] = ext[0]; v.extent[1] = ext[1]; v.extent[2] = ext[2];
            fm::matchPrimitive(v);
            env.instantEdit(true, [&] { c->extentX = v.extent[0]; c->extentY = v.extent[1]; c->extentZ = v.extent[2]; });
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sizes the shape to the primitive it sits on (a Cube is 100 cm); the entity's scale then scales the volume.");
    }

    const bool forceKind = kind == FieldKind::RadialForce || kind == FieldKind::DirectionalForce;
    if (forceKind) {
        float strength = c->strength;
        env.gestureEdit(ImGui::DragFloat("Strength (cm/s per s)", &strength, 5.0f, -20000.0f, 20000.0f, "%.0f"), [&] { c->strength = strength; });
    }
    if (kind == FieldKind::Damage) {
        float energy = c->energyJPerS;
        env.gestureEdit(ImGui::DragFloat("Energy (J per s)", &energy, 1.0f, 0.0f, 100000.0f, "%.1f"), [&] { c->energyJPerS = energy; });
    }
    if (kind == FieldKind::DirectionalForce || kind == FieldKind::Damage) {
        float dir[3] = {c->dirX, c->dirY, c->dirZ};
        env.gestureEdit(ImGui::DragFloat3("Direction (local)", dir, 0.01f, -1.0f, 1.0f, "%.2f"), [&] { c->dirX = dir[0]; c->dirY = dir[1]; c->dirZ = dir[2]; });
    }
    if (forceKind || kind == FieldKind::Damage) {
        float falloff = c->falloff > 0.0f ? c->falloff : 1.0f;
        env.gestureEdit(ImGui::SliderFloat("Falloff", &falloff, 0.0f, 8.0f, "%.2f"), [&] { c->falloff = falloff; });
        float turb = c->turbulence;
        env.gestureEdit(ImGui::SliderFloat("Turbulence", &turb, 0.0f, 1.0f, "%.2f"), [&] { c->turbulence = turb; });
    }

    // Targets: a zero mask reads as the kind's default.
    u32 mask = c->targets ? c->targets : softbody::fieldDefaultTargets(kind);
    bool tCloth = (mask & softbody::kFieldTargetCloth) != 0, tDest = (mask & softbody::kFieldTargetDestructible) != 0, tDebris = (mask & softbody::kFieldTargetDebris) != 0;
    bool tChanged = ImGui::Checkbox("Cloth", &tCloth);
    ImGui::SameLine();
    tChanged = ImGui::Checkbox("Destructible", &tDest) || tChanged;
    ImGui::SameLine();
    tChanged = ImGui::Checkbox("Debris", &tDebris) || tChanged;
    env.instantEdit(tChanged, [&] {
        const u32 m = (tCloth ? softbody::kFieldTargetCloth : 0u) | (tDest ? softbody::kFieldTargetDestructible : 0u) | (tDebris ? softbody::kFieldTargetDebris : 0u);
        c->targets = m == softbody::fieldDefaultTargets(kind) ? 0u : m;
    });
    if (kind == FieldKind::Kill && (mask & softbody::kFieldTargetDestructible))
        ImGui::TextColored(kWarn, "Destructibles are targeted: a killed body is hidden (Repair restores it).");

    bool once = (c->flags & softbody::kFieldOnce) != 0;
    bool enabled = (c->flags & softbody::kFieldDisabled) == 0;
    bool fChanged = ImGui::Checkbox("Once", &once);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Apply as a single pulse (strength and energy are totals), then disable.");
    ImGui::SameLine();
    fChanged = ImGui::Checkbox("Enabled", &enabled) || fChanged;
    env.instantEdit(fChanged, [&] {
        c->flags = (once ? softbody::kFieldOnce : 0u) | (enabled ? 0u : softbody::kFieldDisabled);
    });
    ImGui::EndDisabled();

    if (env.makeInvisibleVolume) {
        const bool already = env.isInvisibleVolume && env.isInvisibleVolume();
        ImGui::BeginDisabled(env.playing || already);
        if (ImGui::SmallButton("Make invisible volume")) env.makeInvisibleVolume();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Hides the primitive this field sits on and turns its collision off; the volume still works.");
    }
    if (env.playing)
        ImGui::TextDisabled("%s", fm::outReadout(c->outTouched, (c->outState & softbody::kFieldStateActive) != 0, (c->outState & softbody::kFieldStateFired) != 0,
                                                 (c->flags & softbody::kFieldDisabled) != 0).c_str());
    else
        ImGui::TextDisabled("Moving, rotating or scaling the entity moves the field. Show > Soft Bodies > Fields draws it.");
}

bool drawSoftBodyDepthSections(const SoftSectionEnv& env, softbody::SoftAsset& asset, const std::string& absAssetPath, bool editable,
                               bool destructible, const std::string& meshRel, bool overCap, bool proxyRunning) {
    bool saved = false;
    softbody::SoftAsset edit = asset;
    bool changed = false, finished = false;
    const auto done = [&] { finished = ImGui::IsItemDeactivatedAfterEdit() || finished; };
    ImGui::BeginDisabled(!editable);

    if (ImGui::TreeNode("Caps")) {
        if (ImGui::BeginCombo("Mode", sm::capModeLabel(edit.caps.mode))) {
            for (const sm::CapModeRow& row : sm::kCapModeRows)
                if (ImGui::Selectable(row.label, row.mode == edit.caps.mode)) { edit.caps.mode = row.mode; changed = finished = true; }
            ImGui::EndCombo();
        }
        bool usePreset = edit.caps.thicknessCm < 0.0f;
        if (ImGui::Checkbox("Preset thickness", &usePreset)) { edit.caps.thicknessCm = usePreset ? -1.0f : softbody::capThicknessCm(edit); changed = finished = true; }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", sm::capThicknessText(edit).c_str());
        if (!usePreset) {
            changed = ImGui::SliderFloat("Thickness (cm)", &edit.caps.thicknessCm, 0.0f, 10.0f, "%.2f") || changed;
            done();
        }
        if (ImGui::BeginCombo("Back skin", sm::backSkinRow(edit.caps.backSkin).label)) {
            for (const sm::BackSkinRow& row : sm::kBackSkinRows) {
                if (ImGui::Selectable(row.label, row.mode == edit.caps.backSkin)) { edit.caps.backSkin = row.mode; changed = finished = true; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", row.tip);
            }
            ImGui::EndCombo();
        }
        changed = ImGui::SliderFloat("UV scale (cm per tile)", &edit.caps.uvCmPerTile, 1.0f, 400.0f, "%.0f", ImGuiSliderFlags_Logarithmic) || changed;
        done();
        changed = ImGui::SliderFloat("Cap LOD distance (cm)", &edit.caps.lodCm, 0.0f, 20000.0f, "%.0f") || changed;
        done();
        if (inputString("Interior material", edit.caps.interiorMaterial)) changed = true;
        done();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A material NAME for the fracture faces; empty uses the surface material.");
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Effects")) {
        for (const sm::FxSlotRow& row : sm::kFxSlotRows) {
            softbody::FxSlotParams& s = edit.effects.slot[static_cast<u32>(row.slot)];
            const std::string header = std::string(row.label) + (s.bound() ? " *" : "");
            if (!ImGui::TreeNode(header.c_str())) continue;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", row.tip);
            std::string path = s.particle;
            if (env.pickAsset && env.pickAsset("Particle effect", path, ".ocparticle")) { s.particle = path; changed = finished = true; }
            for (usize i = 0; i < s.sounds.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                ImGui::TextUnformatted(s.sounds[i].c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) { sm::removeSound(s, i); changed = finished = true; ImGui::PopID(); break; }
                ImGui::PopID();
            }
            if (s.sounds.size() < softbody::kMaxFxSounds) {
                std::string pick;
                if (env.pickAsset && env.pickAsset("Add sound", pick, ".wav") && sm::addSound(s, pick)) changed = finished = true;
            } else {
                ImGui::TextDisabled("Eight sounds is the most a slot takes.");
            }
            if (row.slot == softbody::FxSlot::Impact) { changed = ImGui::DragFloat("Min speed (cm/s)", &s.minSpeedCmS, 5.0f, 0.0f, 10000.0f, "%.0f") || changed; done(); }
            if (row.slot == softbody::FxSlot::Dust) {
                changed = ImGui::DragFloat("Particles per fragment particle", &s.countPerParticle, 0.01f, 0.0f, 8.0f, "%.2f") || changed; done();
            } else {
                changed = ImGui::DragFloat("Count", &s.count, 0.1f, 0.0f, 64.0f, "%.1f") || changed; done();
                changed = ImGui::DragFloat("Count per m/s", &s.countPerMs, 0.05f, 0.0f, 16.0f, "%.2f") || changed; done();
            }
            changed = ImGui::SliderFloat("Volume", &s.volume, 0.0f, 2.0f, "%.2f") || changed; done();
            changed = ImGui::SliderFloat("Pitch jitter", &s.pitchJitter, 0.0f, 1.0f, "%.2f") || changed; done();
            changed = ImGui::DragFloat("Cooldown (s)", &s.cooldownS, 0.01f, 0.0f, 10.0f, "%.2f") || changed; done();
            changed = ImGui::DragFloat("Inner radius (cm)", &s.innerCm, 5.0f, 0.0f, 100000.0f, "%.0f") || changed; done();
            changed = ImGui::DragFloat("Outer radius (cm)", &s.outerCm, 10.0f, 0.0f, 100000.0f, "%.0f") || changed; done();
            s.outerCm = std::max(s.outerCm, s.innerCm);
            ImGui::TreePop();
        }
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Proxy")) {
        const std::string advice = sm::proxyAdvice(overCap, proxyRunning, edit.proxy.mesh, proxyToolAvailable());
        if (!advice.empty()) ImGui::TextColored(overCap && !proxyRunning ? kWarn : ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", advice.c_str());
        std::string path = edit.proxy.mesh;
        if (env.pickAsset && env.pickAsset("Proxy mesh", path, ".ocmesh")) { edit.proxy.mesh = path; changed = finished = true; }
        int maxP = static_cast<int>(edit.proxy.maxParticles);
        if (ImGui::DragInt("Proxy max particles", &maxP, 8.0f, 0, 65536, maxP > 0 ? "%d" : "sim.maxParticles")) { edit.proxy.maxParticles = static_cast<u32>(std::max(0, maxP)); changed = true; }
        done();
        SoftProxyTool& tool = softProxyTool();
        int target = static_cast<int>(tool.request.targetParticles);
        if (ImGui::DragInt("Target particles", &target, 8.0f, 64, 65536)) tool.request.targetParticles = static_cast<u32>(std::max(64, target));
        ImGui::BeginDisabled(meshRel.empty() || !proxyToolAvailable());
        if (ImGui::SmallButton("Create proxy cage")) {
            const std::string proxyRel = sm::proxyPathForMesh(meshRel);
            if (tool.cook(env.contentDir, meshRel, proxyRel)) {
                edit.proxy.mesh = proxyRel;
                changed = finished = true;
            } else if (env.setStatus) {
                env.setStatus("Proxy cage: " + tool.last.why);
            }
        }
        ImGui::EndDisabled();
        if (!tool.last.why.empty()) ImGui::TextDisabled("%s", tool.last.why.c_str());
        ImGui::TreePop();
    }

    if (destructible && ImGui::TreeNode("Debris")) {
        changed = ImGui::Checkbox("Bake settled pieces", &edit.destruct.bake) || changed;
        finished = changed || finished;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A piece asleep for a few seconds becomes a plain static mesh: cheaper, but no longer a collider or damageable.");
        changed = ImGui::SliderFloat("Impact radius scale", &edit.destruct.impactRadiusScale, 0.1f, 4.0f, "%.2f") || changed;
        done();
        ImGui::TreePop();
    }
    ImGui::EndDisabled();

    if (changed && editable) asset = edit;
    if (finished && editable) {
        std::string why;
        if (softbody::saveSoftAsset(absAssetPath, asset, &why)) saved = true;
        else if (env.setStatus) env.setStatus("Could not save the asset: " + why);
    }
    return saved;
}

}  // namespace aver::editor

#else

namespace aver::editor {}

#endif
