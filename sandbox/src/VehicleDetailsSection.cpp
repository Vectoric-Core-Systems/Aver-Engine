// The widget half of the Vehicle Details section. The rules (preset rows, record keys, the live-tune table, the Create path, the
// formatting) are in VehicleDetailsModel.hpp (ImGui-free, tested); this file only draws them. In its own translation unit because
// SandboxComponentDetails.cpp is near the Debug COFF section cap. docs/VEHICLES.md "Editor".
#include "VehicleDetailsModel.hpp"

#if AVER_WITH_IMGUI && AVER_MODULE_VEHICLE_SCENE

#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"
#include "aver/vehicle/OcVehicle.hpp"
#include "aver/vehicle/VehiclePhysicsSystem.hpp"
#include "aver/vehicle/VehiclePresets.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

namespace aver::editor {
namespace {

namespace vm = vehmodel;

const ImVec4 kWarn{1.0f, 0.55f, 0.35f, 1.0f};

// The ghost driver Drive uses when the host has no pawn: an empty entity, removed again on exit. Keyed by vehicle.
std::map<u32, scene::Entity>& ghosts() {
    static std::map<u32, scene::Entity> g;
    return g;
}

void dropGhost(scene::World& w, u32 vehicleEntity) {
    auto it = ghosts().find(vehicleEntity);
    if (it == ghosts().end()) return;
    if (w.valid(it->second)) w.destroy(it->second);
    ghosts().erase(it);
}

// Live-tune slider positions for the selected entity (the system keeps the applied value; the slider remembers what was set).
struct TuneMemory {
    u32 entity = 0;
    f32 value[vm::kTuneRows.size()] = {};
    bool known[vm::kTuneRows.size()] = {};
};
TuneMemory& tuneMemory(u32 entity) {
    static TuneMemory m;
    if (m.entity != entity) {
        m = TuneMemory{};
        m.entity = entity;
    }
    return m;
}

vm::LiveReading readingOf(const vehicle::Telemetry& t, u32 wheelCount) {
    vm::LiveReading r;
    r.speedCmS = t.speedCmS;
    r.forwardCmS = t.forwardSpeedCmS;
    r.rpm = t.rpm;
    r.gear = t.gear;
    r.gearCount = t.gearCount;
    r.manual = t.manual;
    r.shifting = t.shifting;
    r.throttle = t.throttle;
    r.brake = t.brake;
    r.steer = t.steer;
    r.handbrake = t.handbrake;
    r.wheelsOnGround = t.wheelsOnGround;
    r.wheelCount = static_cast<i32>(wheelCount);
    r.health01 = t.health01;
    r.abs = t.absActive;
    r.tcs = t.tcsActive;
    r.flipped = t.flipped;
    r.destroyed = t.destroyed;
    return r;
}

vm::WheelReading readingOf(const vehicle::WheelTelemetry& w) {
    vm::WheelReading r;
    r.contact = w.contact;
    r.skidding = w.skidding;
    r.slipLong = w.slipLongitudinal;
    r.slipLatDeg = w.slipLateralDeg;
    r.loadN = w.loadN;
    r.compressionCm = w.compressionCm;
    r.friction = w.surfaceFriction;
    return r;
}

// The starter spec Create .ocvehicle writes: the preset, sized from the mesh the entity carries (the same derivation a preset-only
// vehicle is built from, so assigning the new file changes nothing about how the car drives until it is edited).
bool starterText(scene::World& w, scene::Entity e, u32 preset, std::string& text, std::string& why) {
    const auto* mr = w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
    if (!mr) { why = "the entity has no mesh"; return false; }
    const Vec3 ext{std::fabs(mr->aabbMax[0] - mr->aabbMin[0]), std::fabs(mr->aabbMax[1] - mr->aabbMin[1]), std::fabs(mr->aabbMax[2] - mr->aabbMin[2])};
    vehicle::VehicleSpec spec;
    if (!vehicle::specFromBounds(vm::presetRow(preset).key, ext, spec)) { why = "the mesh has no size yet"; return false; }
    spec.name = vm::presetRow(preset).label;
    std::string problem;
    if (!vehicle::validate(spec, problem)) { why = problem; return false; }
    text = vehicle::writeOcVehicle(spec);
    return true;
}

}  // namespace

void drawVehicleSection(const VehicleSectionEnv& env, void* worldPtr, u32 entity) {
    auto& w = *static_cast<scene::World*>(worldPtr);
    const scene::Entity e = static_cast<scene::Entity>(entity);
    const u32 type = vehicle::VehiclePhysicsSystem::typeId();
    auto* c = type ? w.component<vehicle::CVehicle>(e, type) : nullptr;
    if (!c || !env.instantEdit || !env.gestureEdit) return;
    vehicle::VehiclePhysicsSystem& sys = vehicle::vehiclePhysicsSystem();

    // ---- source: the asset, or a preset ------------------------------------------------------------------------------
    const std::string assetRel = c->asset != 0 && env.pathOfId ? env.pathOfId(c->asset) : std::string();
    if (env.assetRow) env.assetRow("asset", "Vehicle (.ocvehicle)", c->asset);
    if (c->asset != 0 && assetRel.empty()) ImGui::TextColored(kWarn, "The vehicle asset is not under Content.");
    if (!assetRel.empty() && env.openEditor) {
        if (ImGui::SmallButton("Open in editor")) env.openEditor(assetRel);
        ImGui::SameLine();
    }
    ImGui::TextDisabled(c->asset != 0 ? "Edits to the asset apply to vehicles built after the save." : "No asset: the preset below, sized from the mesh.");

    ImGui::BeginDisabled(env.playing);
    const u32 preset = c->preset < vm::kPresetRows.size() ? c->preset : 0u;
    if (ImGui::BeginCombo("Preset", vm::presetRow(preset).label)) {
        for (const vm::PresetRow& row : vm::kPresetRows) {
            const bool picked = ImGui::Selectable(row.label, row.preset == preset && c->asset == 0);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", row.tip);
            // Choosing a preset makes it the source: an assigned asset is dropped, the same edit vehmodel::applyPreset makes to a record.
            if (picked) env.instantEdit(row.preset != preset || c->asset != 0, [&] { c->preset = row.preset; c->asset = 0; });
        }
        ImGui::EndCombo();
    }
    if (c->asset != 0 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Ignored while an asset is set. Choosing one drops the asset.");

    // Create .ocvehicle: the preset's derived spec written beside the mesh and assigned.
    if (c->asset == 0 && env.createAsset && !env.meshRel.empty()) {
        const std::string rel = vm::createAssetPath(env.meshRel, env.fileExists);
        if (ImGui::SmallButton("Create .ocvehicle")) {
            std::string text, why;
            if (rel.empty()) { if (env.setStatus) env.setStatus("Could not choose a name for the vehicle asset"); }
            else if (!starterText(w, e, preset, text, why)) { if (env.setStatus) env.setStatus("Create .ocvehicle: " + why); }
            else env.createAsset(rel, text);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Writes %s from the preset, then edit it in the Vehicle editor.", rel.empty() ? "an asset" : rel.c_str());
    }

    // ---- scales and flags --------------------------------------------------------------------------------------------
    const auto scale = [&](const char* label, f32& field, const char* tip) {
        f32 v = field > 0.0f ? field : 1.0f;
        env.gestureEdit(ImGui::SliderFloat(label, &v, vm::kScaleMin, vm::kScaleMax, "%.2f", ImGuiSliderFlags_Logarithmic), [&] { field = v; });
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    scale("Power", c->powerScale, "Engine torque scale.");
    scale("Grip", c->gripScale, "Tyre friction scale.");
    scale("Brakes", c->brakeScale, "Brake torque scale.");
    scale("Damage taken", c->damageScale, "How much of each hit becomes damage.");
    const auto flag = [&](const char* label, u32 bit, bool negative, const char* tip) {
        bool on = negative ? (c->flags & bit) == 0 : (c->flags & bit) != 0;
        env.instantEdit(ImGui::Checkbox(label, &on), [&] {
            const bool set = negative ? !on : on;
            if (set) c->flags |= bit; else c->flags &= ~bit;
        });
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    flag("Driver controls", vehicle::kVehicleNoActions, true, "A driver steers it with the Drive.* input actions. Off: only a script sets the pedals.");
    flag("Engine sound", vehicle::kVehicleNoAudio, true, "The engine, skid and impact sounds from the spec.");
    flag("Smoke and sparks", vehicle::kVehicleNoEffects, true, "Skid smoke and impact sparks from the spec.");
    flag("Wheel visuals", vehicle::kVehicleNoWheelVisuals, true, "Moves the wheel child entities (or bones) named in the spec.");
    flag("Indestructible", vehicle::kVehicleIndestructible, false, "Damage never accumulates.");
    ImGui::EndDisabled();

    // ---- the Play block ----------------------------------------------------------------------------------------------
    if (!env.playing) {
        ImGui::TextDisabled("Speed, gear, the wheel table and the live tuning show during Play.");
        return;
    }
    vehicle::Telemetry tel;
    if (!sys.telemetry(e, tel)) {
        const vehicle::Stats st = sys.stats();
        ImGui::TextColored(kWarn, "Not built (%u vehicle%s waiting or refused; see the log).", st.pending, st.pending == 1 ? "" : "s");
        return;
    }
    const u32 wheelCount = sys.wheelCount(e);
    const vm::LiveReading live = readingOf(tel, wheelCount);
    ImGui::Separator();
    ImGui::TextUnformatted(vm::outLine(live).c_str());
    const std::string lamps = vm::lampText(live);
    if (!lamps.empty()) { ImGui::SameLine(); ImGui::TextColored(kWarn, "%s", lamps.c_str()); }

    // RPM between idle and the limit (the spec's, when known).
    f32 minRpm = 1000.0f, maxRpm = 6000.0f;
    if (const vehicle::VehicleSpec* spec = sys.specOf(e)) { minRpm = spec->engine.minRpm; maxRpm = spec->engine.maxRpm; }
    char rpmText[48];
    std::snprintf(rpmText, sizeof rpmText, "%.0f rpm", static_cast<double>(live.rpm));
    ImGui::ProgressBar(vm::rpmFraction(live.rpm, minRpm, maxRpm), ImVec2(-1.0f, 0.0f), rpmText);
    ImGui::ProgressBar(std::min(std::max(live.throttle, 0.0f), 1.0f), ImVec2(ImGui::GetContentRegionAvail().x * 0.5f - 4.0f, 0.0f), "throttle");
    ImGui::SameLine();
    ImGui::ProgressBar(std::min(std::max(live.brake, 0.0f), 1.0f), ImVec2(-1.0f, 0.0f), "brake");
    ImGui::TextDisabled("steer %.2f   handbrake %.0f%%   on ground %d / %d   %s", static_cast<double>(live.steer), static_cast<double>(live.handbrake) * 100.0,
                        live.wheelsOnGround, live.wheelCount, live.shifting ? "shifting" : "");

    if (live.manual) {
        if (ImGui::SmallButton("Shift down")) sys.shift(e, -1);
        ImGui::SameLine();
        if (ImGui::SmallButton("Shift up")) sys.shift(e, 1);
    }

    // Buttons.
    const scene::Entity driver = sys.driverOf(e);
    const vm::ButtonState bs = vm::buttonsFor(true, driver != scene::kInvalidEntity, live);
    ImGui::BeginDisabled(!bs.drive);
    if (ImGui::Button("Drive")) {
        scene::Entity who = env.driverEntity ? static_cast<scene::Entity>(env.driverEntity()) : scene::kInvalidEntity;
        if (who == scene::kInvalidEntity || !w.valid(who)) {
            who = w.create("Ghost driver");
            ghosts()[entity] = who;
        }
        if (!sys.enter(e, who)) dropGhost(w, entity);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!bs.exit);
    if (ImGui::Button("Exit")) { sys.exit(e); dropGhost(w, entity); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Flip")) sys.flip(e, 50.0f);
    ImGui::SameLine();
    ImGui::BeginDisabled(!bs.repair);
    if (ImGui::Button("Repair")) sys.repair(e);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Stop")) { c->inThrottle = 0.0f; c->inBrake = 1.0f; c->inHandbrake = 1.0f; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Zero throttle, full brake and handbrake. A driver's own input replaces it.");
    if (driver != scene::kInvalidEntity && ghosts().count(entity))
        ImGui::TextDisabled("Driven by a ghost driver: the Drive.* actions of the project's input scheme steer it.");

    // Live tuning: applied now, not saved (the Power / Grip / Brakes sliders above are the saved ones).
    if (ImGui::TreeNode("Live tuning")) {
        TuneMemory& mem = tuneMemory(entity);
        for (usize i = 0; i < vm::kTuneRows.size(); ++i) {
            const vm::TuneRow& row = vm::kTuneRows[i];
            if (!mem.known[i]) mem.value[i] = row.def;
            f32 v = mem.value[i];
            ImGui::PushID(static_cast<int>(i));
            bool changed = false;
            if (row.toggle) {
                bool on = v >= 0.5f;
                changed = ImGui::Checkbox(row.label, &on);
                v = on ? 1.0f : 0.0f;
            } else if (row.integer) {
                int n = static_cast<int>(std::lround(v));
                changed = ImGui::SliderInt(row.label, &n, static_cast<int>(row.min), static_cast<int>(row.max));
                v = static_cast<f32>(n);
            } else {
                changed = ImGui::SliderFloat(row.label, &v, row.min, row.max, "%.2f");
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", row.tip);
            if (changed) {
                v = vm::clampTune(row, v);
                mem.value[i] = v;
                mem.known[i] = true;
                sys.tune(e, row.key, v);
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }

    // The per-wheel table.
    if (wheelCount > 0 && ImGui::TreeNodeEx("Wheels", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("##wheels", static_cast<int>(vm::kWheelColumns.size()), ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            for (const char* col : vm::kWheelColumns) ImGui::TableSetupColumn(col);
            ImGui::TableHeadersRow();
            for (u32 i = 0; i < wheelCount; ++i) {
                vehicle::WheelTelemetry wt;
                if (!sys.wheel(e, i, wt)) continue;
                const auto cells = vm::wheelCells(i, readingOf(wt));
                ImGui::TableNextRow();
                for (usize col = 0; col < cells.size(); ++col) {
                    ImGui::TableSetColumnIndex(static_cast<int>(col));
                    if (col == 7 && wt.skidding) ImGui::TextColored(kWarn, "%s", cells[col].c_str());
                    else ImGui::TextUnformatted(cells[col].c_str());
                }
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
}

}  // namespace aver::editor

#else

namespace aver::editor {}

#endif
