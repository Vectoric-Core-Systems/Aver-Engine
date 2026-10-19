// Details > Add Component for Skeletal Mesh, Animator, Root Motion, Motion Matching, Control Rig, IK Rig, AI Brain
// and Smart Object: the menu entries, the inspector sections, add / remove / edit and their undo.
//
// The rules (what may be attached where, creation defaults, asset listing) are PlacedComponentModel.hpp. The
// component state is read and written through game::applyPlacedComponent / capturePlacedComponent
// (aver/game/LevelComponents.hpp), the code that also loads a COMPONENT record in the packaged game, so what
// the editor edits is exactly what the level saves and reloads. Plain numeric fields are edited on the live
// component in place; a gesture is bracketed by a "before" capture and one undo entry.
// docs/plans/parity_demo/ADD_COMPONENT.md sections 4 and 7.
#include "SandboxApp.hpp"

#if AVER_MODULE_SCENE
#include "PlacedComponentModel.hpp"
#include "SoftBodyDetailsModel.hpp"
#include "SoftBodyPaintTool.hpp"
#include "SoftFieldDetailsModel.hpp"
#include "aver/anim/AnimSystem.hpp"
#include "aver/anim/ControlRig.hpp"
#include "aver/anim/MotionMatchSystem.hpp"
#include "aver/game/LevelComponents.hpp"

#if AVER_MODULE_CAMERA
#  include "CameraRigDetailsModel.hpp"
#  include "aver/camera/CameraRigSystem.hpp"
#  if AVER_MODULE_FRAMEWORK
#    include "aver/framework/framework_access_abi.h"
#  endif
#endif
#if AVER_MODULE_SOFTBODY_SCENE
#  include "aver/softbody/SoftBodySystem.hpp"
#endif
#if AVER_MODULE_SYNAPSE_SCENE
#  include "aver/synapse/SynapseBt.hpp"
#  include "aver/synapse/SynapseSmartObject.hpp"
#  if AVER_WITH_SYNAPSE_AI
#    include "aver/synapse/SynapseAi.hpp"
#    include "aver/synapse/synapse_ai_abi.h"
#  endif
#endif

namespace aver {
namespace {

u32 kindComponentId(const scene::World& w, const editor::PlacedKind& k) {
    return k.builtinId != 0 ? k.builtinId : w.componentId(k.componentName);
}

// Sets `key` (dropping its unresolved `key#` hex twin), or removes it when `value` is empty.
void setKey(fmt::OcPlacedComponent& r, const std::string& key, const std::string& value) {
    const std::string hexKey = key + "#";
    r.kv.erase(std::remove_if(r.kv.begin(), r.kv.end(),
                              [&](const auto& kv) { return kv.first == key || kv.first == hexKey; }),
               r.kv.end());
    if (!value.empty()) r.kv.emplace_back(key, value);
}

} // namespace

// The content-relative path an asset id names, "" when the content index does not hold it.
std::string SandboxApp::pathOfContentId(u64 id) const {
    if (id == 0) return {};
    const std::string abs = content_.pathFor(id);
    return abs.empty() ? std::string() : cbRelativeToContent(abs);
}

editor::AttachContext SandboxApp::placedAttachContext(scene::Entity e) const {
    const scene::World& w = scene::World::instance();
    editor::AttachContext c;
    c.levelOwned = isLevelOwned(e);
    c.hasMesh = w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer) != nullptr;
    const auto* sm = w.component<scene::CSkeletalMesh>(e, scene::kComponentSkeletalMesh);
    c.hasSkeleton = sm && sm->skeleton != 0;
    const EntityAnim* oa = entityAnim(e);
    c.hasObjectAnim = oa && !oa->clip.empty();
    c.legacyOcmap = levelIsLegacyOcmap_;
    c.prefabLinked = prefabSys_.isLinked(e);
    c.instanceMember = level_.instances().isMember(e);
    const u32 clothId = w.componentId("CCloth"), destructId = w.componentId("CDestructible");
    c.hasCloth = clothId != 0 && w.hasComponent(e, clothId);
    c.hasDestructible = destructId != 0 && w.hasComponent(e, destructId);

    const u32 vehicleId = w.componentId("CVehicle");
    c.hasVehicle = vehicleId != 0 && w.hasComponent(e, vehicleId);
    c.hasRigidBody = w.hasComponent(e, scene::kComponentRigidBody);
    c.hasVehicleToken = level_.vehiclePresetOf(e) != nullptr;
    return c;
}

void SandboxApp::beginPlacedGesture(const editor::PlacedKind& kind) {
    placedBefore_ = game::capturePlacedComponent(scene::World::instance(), selEntity_, kind.type,
                                                 [this](u64 id) { return pathOfContentId(id); });
    placedGesture_ = &kind;
}

// Pushes one undo entry when the component differs from `before` (nullopt = it was absent). A no-op entry would
// make Ctrl+Z visibly do nothing once, so an unchanged gesture pushes nothing.
void SandboxApp::commitPlacedEdit(const editor::PlacedKind& kind, std::optional<fmt::OcPlacedComponent> before) {
    placedBefore_.reset();
    placedGesture_ = nullptr;
    scene::World& w = scene::World::instance();
    const scene::Entity e = selEntity_;
    if (!w.valid(e)) return;
    auto after = game::capturePlacedComponent(w, e, kind.type, [this](u64 id) { return pathOfContentId(id); });
    if (before == after) return;
    EditCmd c;
    c.kind = EditCmd::Kind::PlacedComponent;
    c.id = editIdFor(e);
    c.pcType = kind.type;
    c.pcBefore = std::move(before);
    c.pcAfter = std::move(after);
    pushEdit(std::move(c));
    markLevelUnsaved();
}

bool SandboxApp::addPlacedComponent(const editor::PlacedKind& kind) {
    scene::World& w = scene::World::instance();
    const scene::Entity e = selEntity_;
    if (!w.valid(e)) return false;
    if (anyPlayActive()) { cbStatus_ = "Stop Play to add components."; return false; }
    const u32 cid = kindComponentId(w, kind);
    if (cid == 0) return false;
    editor::AttachContext ctx = placedAttachContext(e);
    ctx.alreadyPresent = w.hasComponent(e, cid);
    std::string why;
    if (!editor::canAttach(ctx, kind, &why)) { cbStatus_ = why; return false; }

    fmt::OcPlacedComponent rec = editor::makeNewRecord(kind);
    if (std::string_view(kind.type) == "SkeletalMesh") {
        // The skeleton next to the mesh with the same stem, when there is exactly that.
        std::vector<std::string> listing;
        for (const auto& kv : content_.index())
            if (lowerExt(std::filesystem::path(kv.second)) == ".ocskel") listing.push_back(cbRelativeToContent(kv.second));
        const std::string preset = editor::presetSkeletonFor(w.name(e), listing);
        if (!preset.empty()) setKey(rec, "skeleton", preset);
    }
    if (!game::applyPlacedComponent(w, e, rec)) {
        cbStatus_ = std::string("Could not add ") + kind.menuLabel + " (see the log)";
        return false;
    }
    EditCmd c;
    c.kind = EditCmd::Kind::PlacedComponent;
    c.id = editIdFor(e);
    c.pcType = kind.type;
    c.pcBefore = std::nullopt;
    c.pcAfter = game::capturePlacedComponent(w, e, kind.type, [this](u64 id) { return pathOfContentId(id); });
    pushEdit(std::move(c));
    markLevelUnsaved();
    cbStatus_ = std::string("Added ") + kind.menuLabel;
    return true;
}

bool SandboxApp::removePlacedComponentFromSelection(const editor::PlacedKind& kind) {
    scene::World& w = scene::World::instance();
    const scene::Entity e = selEntity_;
    if (!w.valid(e)) return false;
    if (anyPlayActive()) { cbStatus_ = "Stop Play to remove components."; return false; }
    const auto has = [&](const char* name) {
        const u32 id = w.componentId(name);
        return id != 0 && w.hasComponent(e, id);
    };
    std::string why;
    if (!editor::canRemove(w.hasComponent(e, scene::kComponentAnimator), has("CRootMotion"), has("CMotionMatcher"),
                           has("CControlRig"), has("CIkRig"), kind, &why)) {
        cbStatus_ = why;
        return false;
    }
    auto before = game::capturePlacedComponent(w, e, kind.type, [this](u64 id) { return pathOfContentId(id); });
    if (!before) return false;
    if (!game::removePlacedComponent(w, e, kind.type)) return false;
    EditCmd c;
    c.kind = EditCmd::Kind::PlacedComponent;
    c.id = editIdFor(e);
    c.pcType = kind.type;
    c.pcBefore = std::move(before);
    c.pcAfter = std::nullopt;
    pushEdit(std::move(c));
    markLevelUnsaved();
    cbStatus_ = std::string("Removed ") + kind.menuLabel;
    return true;
}

// Undo / redo of one PlacedComponent command: put the component into state `st` (nullopt = absent).
void SandboxApp::applyPlacedComponentState(EditId id, const std::string& type,
                                           const std::optional<fmt::OcPlacedComponent>& st) {
    scene::World& w = scene::World::instance();
    const scene::Entity e = entityForEdit(id);
    if (e == scene::kInvalidEntity || !w.valid(e)) return;
    if (st) game::applyPlacedComponent(w, e, *st);
    else    game::removePlacedComponent(w, e, type);
    markLevelUnsaved();
}

#if AVER_WITH_IMGUI
// A path row: the current path, a drop target and a Change... picker. Candidates are built only while the popup is
// open (scanning the whole index every frame once cost real time, see the Particle Emitter picker).
bool SandboxApp::drawAssetField(const char* id, const char* label, std::string& path,
                                std::span<const char* const> exts,
                                const std::function<bool(u64, const std::string&)>& accept) {
    bool changed = false;
    ImGui::PushID(id);
    const auto extWanted = [&](const std::string& ext) {
        for (const char* e : exts)
            if (ext == e) return true;
        return false;
    };
    ImGui::TextDisabled("%s  %s", label, path.empty() ? "(none)" : path.c_str());
    if (ImGui::BeginDragDropTarget()) {
        // The Content Browser sends its selection under kCbMoveDragDropType (one payload per drag), so take the
        // first entry with a wanted extension; kAssetDragDropType is still accepted from a single-asset source.
        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kCbMoveDragDropType);
        if (!payload) payload = ImGui::AcceptDragDropPayload(kAssetDragDropType);
        if (payload && payload->Data) {
            const std::string blob(static_cast<const char*>(payload->Data),
                                   payload->DataSize > 0 ? static_cast<usize>(payload->DataSize - 1) : usize(0));
            bool taken = false;
            for (const std::string& one : editor::splitDropPayload(blob)) {
                if (!extWanted(lowerExt(std::filesystem::path(one)))) continue;
                const std::string rel = cbRelativeToContent(one);
                if (rel.empty() || std::filesystem::path(rel).is_absolute() || rel.rfind("..", 0) == 0) {
                    cbStatus_ = "That asset is not under the project's Content folder";
                    taken = true;
                    break;
                }
                path = rel;
                changed = taken = true;
                break;
            }
            if (!taken) {
                std::string list;
                for (const char* e : exts) list += (list.empty() ? "" : "/") + std::string(e);
                cbStatus_ = "Only a " + list + " asset can be dropped here";
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Change...")) ImGui::OpenPopup("##pick");

    static ImGuiID s_owner = 0;   // which row's candidates pickPlacedCands_ holds; one popup is open at a time
    const ImGuiID me = ImGui::GetID("##pick");
    if (ImGui::IsPopupOpen("##pick")) {
        PickerCands& pc = pickPlacedCands_;
        if (s_owner != me) { pc.drop(); s_owner = me; }
        const int frame = ImGui::GetFrameCount();
        if (pc.lastFrame < frame - 1 || pc.stamp != content_.index().size()) {
            pc.list.clear();
            pc.list.push_back({"(None)", 0});
            for (const editor::PlacedAssetChoice& a : editor::listAssets(
                     content_.index(), exts, [this](const std::string& abs) { return cbRelativeToContent(abs); }, accept))
                pc.list.push_back({a.label, a.id});
            pc.stamp = content_.index().size();
        }
        pc.lastFrame = frame;
        u64 picked = 0;
        const u64 current = path.empty() ? 0 : fnv1a64(std::string_view(path));
        if (assetPicker("##pick", pc.list, current, &picked)) {
            path.clear();
            for (const AssetChoice& c : pc.list)
                if (c.id == picked) { path = picked == 0 ? std::string() : c.label; break; }
            changed = true;
        }
    } else if (s_owner == me) {
        pickPlacedCands_.drop();
        s_owner = 0;
    }
    ImGui::PopID();
    return changed;
}

void SandboxApp::drawPlacedAddMenu() {
    scene::World& w = scene::World::instance();
    const bool playing = anyPlayActive();
    const editor::AttachContext base = placedAttachContext(selEntity_);   // the popup body only runs while open
    bool headed = false;
    for (const editor::PlacedKind& k : editor::kPlacedKinds) {
        const u32 id = kindComponentId(w, k);
        if (id == 0) continue;   // not registered in this build
        if (!headed) { ImGui::Separator(); ImGui::TextDisabled("Animation and AI"); headed = true; }
        editor::AttachContext ctx = base;
        ctx.alreadyPresent = w.hasComponent(selEntity_, id);
        std::string why;
        bool ok = editor::canAttach(ctx, k, &why);
        if (ok && playing) { ok = false; why = "Stop Play to add components."; }
        ImGui::BeginDisabled(!ok);
        if (ImGui::MenuItem(k.menuLabel)) addPlacedComponent(k);
        ImGui::EndDisabled();
        if (!ok && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", why.c_str());
        else if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", k.tip);
    }
}

bool SandboxApp::drawPlacedRemoveMenu() {
    scene::World& w = scene::World::instance();
    const scene::Entity e = selEntity_;
    const bool playing = anyPlayActive();
    const auto has = [&](const char* name) {
        const u32 id = w.componentId(name);
        return id != 0 && w.hasComponent(e, id);
    };
    const bool hasAnimator = w.hasComponent(e, scene::kComponentAnimator), hasRoot = has("CRootMotion"),
               hasMatcher = has("CMotionMatcher"), hasRig = has("CControlRig"), hasIk = has("CIkRig");
    bool anyPresent = false, headed = false;
    for (const editor::PlacedKind& k : editor::kPlacedKinds) {
        const u32 id = kindComponentId(w, k);
        if (id == 0) continue;
        if (!headed) { ImGui::Separator(); ImGui::TextDisabled("Animation and AI"); headed = true; }
        const bool present = w.hasComponent(e, id);
        anyPresent = anyPresent || present;
        std::string why;
        bool ok = present && editor::canRemove(hasAnimator, hasRoot, hasMatcher, hasRig, hasIk, k, &why);
        if (!present) why = "This entity does not have one.";
        else if (ok && playing) { ok = false; why = "Stop Play to remove components."; }
        ImGui::BeginDisabled(!ok);
        if (ImGui::MenuItem(k.menuLabel)) removePlacedComponentFromSelection(k);
        ImGui::EndDisabled();
        if (!ok && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", why.c_str());
    }
    return anyPresent;
}

// The Details sections of the components this entity carries, in canonical order.
void SandboxApp::drawPlacedComponentSections() {
    scene::World& w = scene::World::instance();
    const scene::Entity e = selEntity_;
    if (!w.valid(e)) return;
    const bool playing = anyPlayActive();
    const ImVec4 kRed(0.95f, 0.35f, 0.25f, 1.0f);
    const auto pathFn = [this](u64 id) { return pathOfContentId(id); };
    const auto cap = [&](const editor::PlacedKind& k) { return game::capturePlacedComponent(w, e, k.type, pathFn); };

    // A drag / slider / text edit changes the live value in place; the whole gesture is one undo entry, bracketed by
    // the item's activation and its deactivation. `changed` is the widget's return, passed in so the "before" is
    // captured on the activating frame, ahead of the first write.
    const auto gestureEdit = [&](const editor::PlacedKind& k, bool changed, const auto& write) {
        if (ImGui::IsItemActivated()) beginPlacedGesture(k);
        if (changed) {
            const bool open = placedGesture_ == &k;
            std::optional<fmt::OcPlacedComponent> before;
            if (!open) before = cap(k);
            write();
            if (!open) commitPlacedEdit(k, std::move(before));   // a one-shot change (typed value, keyboard step)
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && placedGesture_ == &k) commitPlacedEdit(k, placedBefore_);
        else if (ImGui::IsItemDeactivated() && placedGesture_ == &k) { placedGesture_ = nullptr; placedBefore_.reset(); }   // clicked, changed nothing
    };
    // A checkbox, combo or button finishes in the frame it changes: one capture either side of the write.
    const auto instantEdit = [&](const editor::PlacedKind& k, bool changed, const auto& write) {
        if (!changed) return;
        auto before = cap(k);
        write();
        commitPlacedEdit(k, std::move(before));
    };
    // An asset change goes through the applier, so the component, the saved record and undo agree.
    const auto changeAsset = [&](const editor::PlacedKind& k, const char* key, const std::string& path) {
        auto before = cap(k);
        fmt::OcPlacedComponent rec = before ? *before : editor::makeNewRecord(k);
        setKey(rec, key, path);
        if (!game::applyPlacedComponent(w, e, rec)) {
            cbStatus_ = std::string("Could not use '") + path + "' for " + k.menuLabel + " (see the log)";
            return;
        }
        commitPlacedEdit(k, std::move(before));
    };
    const auto assetRow = [&](const editor::PlacedKind& k, const char* key, const char* label, u64 curId,
                              const std::function<bool(u64, const std::string&)>& accept = {}) {
        std::string path = pathOfContentId(curId);
        if (curId != 0 && path.empty())
            ImGui::TextColored(kRed, "%s asset 0x%016llx is not under Content", label, static_cast<unsigned long long>(curId));
        ImGui::BeginDisabled(playing);
        const bool changed = drawAssetField(key, label, path, editor::placedAssetExts(k), accept);
        ImGui::EndDisabled();
        if (changed) changeAsset(k, key, path);
    };

#if AVER_MODULE_SOFTBODY_SCENE
    const auto makeSoftEnv = [&](const editor::PlacedKind& k) {
        editor::SoftSectionEnv env;
        env.playing = playing;
        env.contentDir = project_.contentDir();
        env.instantEdit = [&, kp = &k](bool changed, const std::function<void()>& mutate) { instantEdit(*kp, changed, mutate); };
        env.gestureEdit = [&, kp = &k](bool changed, const std::function<void()>& mutate) { gestureEdit(*kp, changed, mutate); };
        env.pickAsset = [this](const char* label, std::string& path, const char* ext) {
            const char* exts[] = {ext};
            return drawAssetField(label, label, path, std::span<const char* const>(exts, 1), {});
        };
        env.setStatus = [this](const std::string& msg) { cbStatus_ = msg; };
        return env;
    };
    // Cloth and Destructible share one section body: preset, asset (create / paint), tuning, and the Play controls.
    const auto softBodySection = [&](const editor::PlacedKind& k, bool destructible, u32 cid) {
        namespace sm = editor::softmodel;
        softbody::SoftBodySystem& sys = softbody::softBodySystem();
        auto* cloth = destructible ? nullptr : w.component<softbody::CCloth>(e, cid);
        auto* dest = destructible ? w.component<softbody::CDestructible>(e, cid) : nullptr;
        if (!cloth && !dest) return;
        u32& presetRef = destructible ? dest->preset : cloth->preset;
        const u64 assetId = destructible ? dest->asset : cloth->asset;
        u32& flagsRef = destructible ? dest->flags : cloth->flags;
        const softbody::SoftPreset cur = presetRef < softbody::kSoftPresetCount
            ? static_cast<softbody::SoftPreset>(presetRef)
            : (destructible ? softbody::SoftPreset::Metal : softbody::SoftPreset::ClothLight);

        ImGui::BeginDisabled(playing);
        if (ImGui::BeginCombo("Preset", sm::presetRow(cur).label)) {
            for (const sm::PresetRow& row : sm::kPresetRows)
                if (ImGui::Selectable(row.label, row.preset == cur))
                    instantEdit(k, row.preset != cur, [&] { presetRef = static_cast<u32>(row.preset); });
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        assetRow(k, "asset", "Asset (.ocsoft)", assetId);

        // The asset behind the row, re-read on a change of path and about twice a second when no widget is busy.
        const std::string assetRel = pathOfContentId(assetId);
        const std::string absAsset = assetRel.empty() ? std::string() : project_.contentDir() + "/" + assetRel;
        static std::string s_loadedFor;
        static softbody::SoftAsset s_asset;
        static bool s_ok = false;
        const int frame = ImGui::GetFrameCount();
        static int s_lastLoad = 0;
        if (s_loadedFor != absAsset || (!ImGui::IsAnyItemActive() && frame - s_lastLoad > 30)) {
            s_loadedFor = absAsset;
            s_lastLoad = frame;
            s_ok = !absAsset.empty() && softbody::loadSoftAsset(absAsset, s_asset, nullptr);
        }
        const std::string problem = sm::assetProblem(assetId, assetRel, s_ok);
        if (!problem.empty()) ImGui::TextColored(kRed, "%s", problem.c_str());

        const auto* mr = w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
        const std::string meshRel = mr ? pathOfContentId(mr->mesh) : std::string();
        if (assetId == 0) {
            ImGui::BeginDisabled(playing || meshRel.empty());
            if (ImGui::SmallButton("Create .ocsoft")) {
                const std::string rel = sm::assetPathForMesh(meshRel);
                const std::filesystem::path target = std::filesystem::path(project_.contentDir()) / rel;
                std::error_code ec;
                if (std::filesystem::exists(target, ec)) {
                    cbStatus_ = rel + " already exists; assign it instead.";
                } else {
                    std::string why;
                    if (!softbody::saveSoftAsset(target.string(), sm::starterAsset(destructible, cur), &why)) {
                        cbStatus_ = "Could not write " + rel + ": " + why;
                    } else {
                        cbAdoptNewAsset(target, false);
                        changeAsset(k, "asset", rel);
                    }
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("Writes %s beside the mesh. Without one the preset is used.", meshRel.empty() ? "an asset" : sm::assetPathForMesh(meshRel).c_str());
        }

        // The numbers: editable on the loaded .ocsoft, read-only for a preset alone.
        const softbody::SoftAsset shown = s_ok && assetId != 0 ? s_asset : sm::starterAsset(destructible, cur);
        const bool editable = s_ok && assetId != 0 && !playing;
        if (ImGui::TreeNode("Material")) {
            softbody::SoftAsset edit = shown;
            bool changed = false, finished = false;
            ImGui::BeginDisabled(!editable);
            const auto slider = [&](const char* label, f32& v, f32 lo, f32 hi, const char* format, bool log) {
                changed = ImGui::SliderFloat(label, &v, lo, hi, format, log ? ImGuiSliderFlags_Logarithmic : 0) || changed;
                finished = ImGui::IsItemDeactivatedAfterEdit() || finished;
            };
            slider("Area density (kg/m2)", edit.material.areaDensityKgM2, 0.05f, 20.0f, "%.2f", true);
            slider("Edge compliance", edit.material.compliance, 0.0f, 1e-3f, "%.2e", false);
            slider("Bend compliance", edit.material.bendCompliance, 0.0f, 1.0f, "%.2e", false);
            slider("Yield strain", edit.material.yieldStrain, 0.0f, 0.5f, "%.4f", false);
            slider("Break strain", edit.material.breakStrain, 0.0f, 1.0f, "%.4f", false);
            slider("Toughness (J / edge)", edit.material.toughnessJ, 0.0f, 200.0f, "%.2f", false);
            slider("Damping", edit.sim.damping, 0.0f, 5.0f, "%.2f", false);
            slider("Vertex radius (cm)", edit.sim.vertexRadiusCm, 0.0f, 5.0f, "%.2f", false);
            int iterations = static_cast<int>(edit.sim.iterations);
            if (ImGui::SliderInt("Iterations", &iterations, 1, 24)) { edit.sim.iterations = static_cast<u32>(iterations); changed = true; }
            finished = ImGui::IsItemDeactivatedAfterEdit() || finished;
            if (!destructible) {
                slider("Drag", edit.cloth.dragCoeff, 0.0f, 2.0f, "%.2f", false);
                slider("Lift", edit.cloth.liftCoeff, 0.0f, 2.0f, "%.2f", false);
                slider("Default reach (cm)", edit.cloth.defaultReachCm, 0.0f, 50.0f, "%.1f", false);
            } else {
                slider("Min impact (cm/s)", edit.destruct.minImpactCmS, 0.0f, 2000.0f, "%.0f", false);
                slider("Impulse efficiency", edit.destruct.impulseEfficiency, 0.01f, 1.0f, "%.2f", false);
                slider("Max kick (cm/s)", edit.destruct.maxKickCmS, 0.0f, 5000.0f, "%.0f", false);
            }
            ImGui::EndDisabled();
            if (changed && editable) s_asset = edit;
            if (finished && editable) {
                std::string why;
                if (!softbody::saveSoftAsset(absAsset, s_asset, &why)) cbStatus_ = "Could not save the asset: " + why;
            }
            if (!editable) ImGui::TextDisabled("%s", assetId == 0 ? "Create an .ocsoft to override the preset." : "Stop Play to edit.");
            ImGui::TreePop();
        }
        // Caps, effects, proxy cage and debris (wave 10, SoftFieldSection.cpp): they edit the loaded .ocsoft.
        if (s_ok && assetId != 0) {
            const u32 st = destructible ? dest->outState : cloth->outState;
            editor::drawSoftBodyDepthSections(makeSoftEnv(k), s_asset, absAsset, editable, destructible, meshRel,
                                              (st & softbody::kStateOverCap) != 0, (st & softbody::kStateProxy) != 0);
        }

        // Per-component tuning (scales: 0 reads as 1, and 1 is shown).
        ImGui::BeginDisabled(playing);
        if (!destructible) {
            f32 wind = cloth->windScale > 0.0f ? cloth->windScale : 1.0f;
            gestureEdit(k, ImGui::SliderFloat("Wind scale", &wind, 0.0f, 4.0f, "%.2f"), [&] { cloth->windScale = wind; });
            f32 stiff = cloth->stiffnessScale > 0.0f ? cloth->stiffnessScale : 1.0f;
            gestureEdit(k, ImGui::SliderFloat("Stiffness scale", &stiff, 0.1f, 10.0f, "%.2f", ImGuiSliderFlags_Logarithmic), [&] { cloth->stiffnessScale = stiff; });
            f32 reach = cloth->reachScale > 0.0f ? cloth->reachScale : 1.0f;
            gestureEdit(k, ImGui::SliderFloat("Reach scale", &reach, 0.0f, 4.0f, "%.2f"), [&] { cloth->reachScale = reach; });
            f32 freeze = cloth->freezeDistanceCm;
            gestureEdit(k, ImGui::DragFloat("Freeze distance (cm)", &freeze, 10.0f, 0.0f, 100000.0f, freeze > 0.0f ? "%.0f" : "system default"),
                        [&] { cloth->freezeDistanceCm = freeze; });
        } else {
            f32 tough = dest->toughnessScale > 0.0f ? dest->toughnessScale : 1.0f;
            gestureEdit(k, ImGui::SliderFloat("Toughness scale", &tough, 0.05f, 20.0f, "%.2f", ImGuiSliderFlags_Logarithmic), [&] { dest->toughnessScale = tough; });
            f32 impact = dest->impactScale > 0.0f ? dest->impactScale : 1.0f;
            gestureEdit(k, ImGui::SliderFloat("Impact scale", &impact, 0.05f, 20.0f, "%.2f", ImGuiSliderFlags_Logarithmic), [&] { dest->impactScale = impact; });
            f32 life = dest->debrisLifetimeS;
            gestureEdit(k, ImGui::DragFloat("Debris lifetime (s)", &life, 0.1f, 0.0f, 600.0f, life > 0.0f ? "%.1f" : "asset default"), [&] { dest->debrisLifetimeS = life; });
            int maxPieces = static_cast<int>(dest->maxPieces);
            gestureEdit(k, ImGui::DragInt("Max pieces", &maxPieces, 0.2f, 0, 256, maxPieces > 0 ? "%d" : "asset default"),
                        [&] { dest->maxPieces = static_cast<u32>(std::max(0, maxPieces)); });
        }
        const auto flagBox = [&](const char* label, u32 bit, bool negative) {
            bool on = negative ? (flagsRef & bit) == 0 : (flagsRef & bit) != 0;
            instantEdit(k, ImGui::Checkbox(label, &on), [&] {
                const bool set = negative ? !on : on;
                if (set) flagsRef |= bit; else flagsRef &= ~bit;
            });
        };
        if (!destructible) {
            flagBox("Tearable", softbody::kClothTearable, false);
            flagBox("Collide with the world", softbody::kClothNoWorld, true);
            flagBox("Collide with the character", softbody::kClothNoCharacterProxy, true);
            flagBox("Wind", softbody::kClothNoWind, true);
        } else {
            flagBox("Indestructible", softbody::kDestructIndestructible, false);
            flagBox("Debris pieces", softbody::kDestructNoDebris, true);
        }
        ImGui::EndDisabled();
        if (!destructible) {
            if (ImGui::SmallButton("Copy sky wind")) sys.setWind(Vec3{sky_.cloudWind[0], sky_.cloudWind[1], 0.0f}, 0.0f, 0.3f, 0.0f);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sets the soft-body wind to the sky cloud drift. They are independent otherwise.");
        }

        // Paint: pins and reach on the sim particles.
        editor::SoftBodyPaintTool& paint = editor::softBodyPaintTool();
        const bool painting = paint.active() && paint.entity == static_cast<u32>(e);
        ImGui::BeginDisabled(playing || meshRel.empty());
        if (!painting) {
            if (ImGui::SmallButton("Paint pins...")) {
                fmt::OcMeshData md;
                std::string why;
                if (!fmt::loadOcMesh(project_.contentDir() + "/" + meshRel, md, &why)) cbStatus_ = "Could not read the mesh: " + why;
                else if (!paint.load(md, shown, absAsset)) cbStatus_ = "The mesh could not be welded for painting";
                else paint.entity = static_cast<u32>(e);
            }
        } else if (ImGui::SmallButton("Close paint")) {
            paint.close();
        }
        ImGui::EndDisabled();
        if (painting) {
            paint.entityWorld = w.worldMatrix(e);
            paint.drawPanel();
            ImGui::TextDisabled("Shift + drag on the mesh in the viewport paints with the brush.");
        }

        // What it is doing.
        const softbody::SoftBodyState st = sys.state(e);
        if (!playing || !st.found) {
            ImGui::TextDisabled("Hit, Explode, Tear, Repair and Release work during Play.");
        } else {
            if (destructible) ImGui::TextDisabled("Damage %.0f%%   broken %u   pieces %u   %s", static_cast<double>(st.damage01) * 100.0,
                                                  st.brokenEdges, st.pieces, st.frozen ? "frozen" : (st.awake ? "awake" : "asleep"));
            else              ImGui::TextDisabled("Max strain %.3f   pieces %u   %s", static_cast<double>(st.maxStrain), st.pieces,
                                                  st.frozen ? "frozen" : (st.awake ? "awake" : "asleep"));
            Vec3 centre{0, 0, 0};
            if (const softbody::Cage* c = sys.cageOf(e); c && !c->particles.empty()) {
                for (const softbody::Particle& p : c->particles) centre += p.pos;
                centre = centre / static_cast<f32>(c->particles.size());
            }
            if (ImGui::SmallButton("Hit")) sys.damage(e, centre, 30.0f, 300.0f, Vec3{0, 0, 0}, 2);
            ImGui::SameLine();
            if (ImGui::SmallButton("Explode")) sys.explode(centre + Vec3{0, -60.0f, 0}, 300.0f, 600.0f, 500.0f);
            ImGui::SameLine();
            if (ImGui::SmallButton("Tear")) sys.tearAt(e, centre, 20.0f);
            ImGui::SameLine();
            if (ImGui::SmallButton("Repair")) sys.repair(e);
            if (!destructible) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Release")) sys.release(e);
            }
        }
    };
#endif

    for (const editor::PlacedKind& k : editor::kPlacedKinds) {
        const u32 cid = kindComponentId(w, k);
        if (cid == 0 || !w.hasComponent(e, cid)) continue;
        if (!ImGui::CollapsingHeader(k.header, ImGuiTreeNodeFlags_DefaultOpen)) continue;
        ImGui::PushID(k.type);
        if (playing) ImGui::TextDisabled("Stop Play to edit.");
        const std::string_view t = k.type;

        // ---- Skeletal Mesh ----
        if (t == "SkeletalMesh") {
            auto* sm = w.component<scene::CSkeletalMesh>(e, scene::kComponentSkeletalMesh);
            assetRow(k, "skeleton", "Skeleton", sm->skeleton);
            if (sm->skeleton == 0) {
                ImGui::TextDisabled("Pick a skeleton (.ocskel); nothing is posed until one is chosen.");
            } else if (sm->boneCount > 0) {
                ImGui::TextDisabled("Bones  %u", sm->boneCount);
            } else {
                // AnimSystem fills boneCount on its first tick with the skeleton; still 0 after a couple of frames
                // means it did not load.
                static u32 s_entity = 0;
                static u64 s_skeleton = 0;
                static int s_first = 0, s_last = 0;
                const int frame = ImGui::GetFrameCount();
                if (s_entity != static_cast<u32>(e) || s_skeleton != sm->skeleton || frame - s_last > 1) {
                    s_entity = static_cast<u32>(e);
                    s_skeleton = sm->skeleton;
                    s_first = frame;
                }
                s_last = frame;
                if (frame - s_first > 2) ImGui::TextColored(kRed, "Bones  0: the skeleton did not load");
                else                     ImGui::TextDisabled("Bones  resolving...");
            }
            const auto collideIt = entityCollide_.find(static_cast<u32>(e));
            if (collideIt == entityCollide_.end() || collideIt->second)
                ImGui::TextDisabled(ICON_WARNING " A character should not have a static collision body: turn Collides off.");
        }
        // ---- Animator ----
        else if (t == "Animator") {
            auto* an = w.component<scene::CAnimator>(e, scene::kComponentAnimator);
            // Object clips move the entity instead of posing bones, so they are not offered here.
            assetRow(k, "clip", "Clip", an->clip, [](u64 clipId, const std::string&) {
                const fmt::OcAnimation* c = anim::animSystem().clip(clipId);
                return c && !(c->flags & fmt::kOcAnimObject);
            });
            ImGui::BeginDisabled(playing);
            f32 speed = an->speed;
            gestureEdit(k, ImGui::DragFloat("Speed", &speed, 0.01f, -4.0f, 4.0f, "%.2f"), [&] { an->speed = speed; });
            f32 weight = an->blendWeight;
            gestureEdit(k, ImGui::SliderFloat("Blend weight", &weight, 0.0f, 1.0f, "%.2f"), [&] { an->blendWeight = weight; });
            bool loop = (an->flags & scene::kAnimatorOnce) == 0;
            instantEdit(k, ImGui::Checkbox("Loop", &loop), [&] {
                if (loop) an->flags &= ~scene::kAnimatorOnce; else an->flags |= scene::kAnimatorOnce;
            });
            bool playingClip = (an->flags & scene::kAnimatorPaused) == 0;
            instantEdit(k, ImGui::Checkbox("Playing", &playingClip), [&] {
                if (playingClip) an->flags &= ~scene::kAnimatorPaused; else an->flags |= scene::kAnimatorPaused;
            });
            ImGui::EndDisabled();
            ImGui::TextDisabled("Time  %.2f s", an->time);
            const u32 mmId = w.componentId("CMotionMatcher");
            if (const auto* mm = mmId ? w.component<anim::CMotionMatcher>(e, mmId) : nullptr; mm && mm->database != 0)
                ImGui::TextDisabled("Pose comes from Motion Matching; this clip is ignored.");
        }
        // ---- Root Motion ----
        else if (t == "RootMotion") {
            auto* rm = w.component<anim::CRootMotion>(e, cid);
            ImGui::BeginDisabled(playing);
            static const char* const kModes[] = {"Ignore", "Apply", "Accumulate"};   // kRootMotionIgnore..Accumulate = 1..3
            const char* preview = rm->mode >= anim::kRootMotionIgnore && rm->mode <= anim::kRootMotionAccumulate
                                      ? kModes[rm->mode - 1] : "None";
            if (ImGui::BeginCombo("Mode", preview)) {
                for (int i = 0; i < 3; ++i)
                    if (ImGui::Selectable(kModes[i], rm->mode == i + 1)) instantEdit(k, rm->mode != i + 1, [&] { rm->mode = i + 1; });
                ImGui::EndCombo();
            }
            // 0 reads as X | Y | Yaw (and both sources): show the effective bits, never let the last one clear to 0.
            u32 axes = rm->axes ? rm->axes : anim::kRootAxesDefault;
            const struct { const char* label; u32 bit; } kAxes[] = {
                {"X", anim::kRootAxisX}, {"Y", anim::kRootAxisY}, {"Z", anim::kRootAxisZ}, {"Yaw", anim::kRootAxisYaw}};
            ImGui::TextDisabled("Axes");
            for (const auto& a : kAxes) {
                ImGui::SameLine();
                bool on = (axes & a.bit) != 0;
                instantEdit(k, ImGui::Checkbox(a.label, &on) && (on || (axes & ~a.bit) != 0), [&] {
                    rm->axes = on ? (axes | a.bit) : (axes & ~a.bit);
                });
            }
            u32 sources = rm->sources ? rm->sources : (anim::kRootSrcGraph | anim::kRootSrcMontage);
            ImGui::TextDisabled("Sources");
            ImGui::SameLine();
            bool fromGraph = (sources & anim::kRootSrcGraph) != 0;
            instantEdit(k, ImGui::Checkbox("State machine", &fromGraph) && (fromGraph || (sources & anim::kRootSrcMontage) != 0), [&] {
                rm->sources = fromGraph ? (sources | anim::kRootSrcGraph) : (sources & ~anim::kRootSrcGraph);
            });
            ImGui::SameLine();
            bool fromMontage = (sources & anim::kRootSrcMontage) != 0;
            instantEdit(k, ImGui::Checkbox("Montage", &fromMontage) && (fromMontage || (sources & anim::kRootSrcGraph) != 0), [&] {
                rm->sources = fromMontage ? (sources | anim::kRootSrcMontage) : (sources & ~anim::kRootSrcMontage);
            });
            f32 scale = rm->scale > 0.0f ? rm->scale : 1.0f;
            gestureEdit(k, ImGui::DragFloat("Scale", &scale, 0.01f, 0.0f, 100.0f, "%.2f"), [&] { rm->scale = scale; });
            bool external = (rm->flags & anim::kRootMotionExternal) != 0;
            instantEdit(k, ImGui::Checkbox("External consumer", &external), [&] {
                if (external) rm->flags |= anim::kRootMotionExternal; else rm->flags &= ~anim::kRootMotionExternal;
            });
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("A physics character reads the accumulator; Apply leaves the transform alone.");
            ImGui::EndDisabled();
            if (playing)
                ImGui::TextDisabled("Accumulated  %.1f  %.1f  %.1f cm   yaw %.2f rad", rm->accumX, rm->accumY, rm->accumZ, rm->accumYaw);
            ImGui::TextDisabled("Apply moves the entity during Play only.");
            const auto* an = w.component<scene::CAnimator>(e, scene::kComponentAnimator);
            const u32 mmId = w.componentId("CMotionMatcher"), graphId = w.componentId("CAnimGraph");
            const auto* mm = mmId ? w.component<anim::CMotionMatcher>(e, mmId) : nullptr;
            if (!(an && an->clip != 0) && !(mm && mm->database != 0) && !(graphId && w.hasComponent(e, graphId)))
                ImGui::TextDisabled("Nothing produces root motion on this entity.");
        }
        // ---- Motion Matching ----
        else if (t == "MotionMatcher") {
            auto* mm = w.component<anim::CMotionMatcher>(e, cid);
            assetRow(k, "database", "Database", mm->database);
            ImGui::BeginDisabled(playing);
            int input = mm->inputSource == anim::kMotionInputEntityVelocity ? 1 : 0;
            static const char* const kInputs[] = {"Script", "Entity velocity"};
            if (ImGui::Combo("Input", &input, kInputs, 2)) instantEdit(k, true, [&] { mm->inputSource = input == 1 ? anim::kMotionInputEntityVelocity : anim::kMotionInputScript; });
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Script: a script or graph writes the desired velocity.\nEntity velocity: follows how the entity actually moves.");
            int facing = (mm->flags & anim::kMotionFaceYaw) ? 1 : 0;
            static const char* const kFacing[] = {"Velocity", "Entity heading"};
            if (ImGui::Combo("Facing", &facing, kFacing, 2)) instantEdit(k, true, [&] {
                if (facing == 1) mm->flags |= anim::kMotionFaceYaw; else mm->flags &= ~anim::kMotionFaceYaw;
            });
            f32 rate = mm->playRate > 0.0f ? mm->playRate : 1.0f;
            gestureEdit(k, ImGui::DragFloat("Play rate", &rate, 0.01f, 0.1f, 4.0f, "%.2f"), [&] { mm->playRate = rate; });
            f32 interval = mm->searchInterval;
            gestureEdit(k, ImGui::DragFloat("Search interval (s)", &interval, 0.005f, 0.0f, 2.0f, interval > 0.0f ? "%.3f" : "database default"),
                        [&] { mm->searchInterval = interval; });
            f32 velHL = mm->velHalfLife;
            gestureEdit(k, ImGui::DragFloat("Velocity half-life (s)", &velHL, 0.005f, 0.0f, 2.0f, velHL > 0.0f ? "%.3f" : "database default"),
                        [&] { mm->velHalfLife = velHL; });
            f32 turnHL = mm->turnHalfLife;
            gestureEdit(k, ImGui::DragFloat("Turn half-life (s)", &turnHL, 0.005f, 0.0f, 2.0f, turnHL > 0.0f ? "%.3f" : "database default"),
                        [&] { mm->turnHalfLife = turnHL; });
            bool paused = (mm->flags & anim::kMotionPaused) != 0;
            instantEdit(k, ImGui::Checkbox("Paused", &paused), [&] {
                if (paused) mm->flags |= anim::kMotionPaused; else mm->flags &= ~anim::kMotionPaused;
            });
            ImGui::EndDisabled();
            // The component's OUT fields and the database's load state.
            anim::MotionMatchSystem& mms = anim::motionMatchSystem();
            if (mm->database == 0)            ImGui::TextDisabled("Database state  none");
            else if (mms.database(mm->database)) ImGui::TextDisabled("Database state  Ready");
            else if (mms.pendingBuilds() > 0) ImGui::TextDisabled("Database state  Loading");
            else ImGui::TextColored(kRed, "Database state  Failed, or it does not fit the skeleton (see the log)");
            const std::string playingClipPath = pathOfContentId(mm->clip);
            ImGui::TextDisabled("Playing  %s", playingClipPath.empty() ? "-" : playingClipPath.c_str());
            ImGui::TextDisabled("cost %.3f   switches %u   row %u", mm->cost, mm->switches, mm->row);
        }
        // ---- Control Rig ----
        else if (t == "ControlRig") {
            auto* cr = w.component<anim::CControlRig>(e, cid);
            assetRow(k, "rig", "Rig", cr->rig);
            ImGui::BeginDisabled(playing);
            f32 weight = cr->weight;
            gestureEdit(k, ImGui::SliderFloat("Weight", &weight, 0.0f, 1.0f, "%.2f"), [&] { cr->weight = weight; });
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 turns the whole rig off without detaching it.");
            ImGui::TextDisabled("applied to %u entities last tick", anim::controlRigSystem().appliedLastTick());
            ImGui::TextDisabled("An @input a script never sets uses its default.");
        }
        // ---- IK Rig ----
        else if (t == "IkRig") {
            auto* ik = w.component<anim::CIkRig>(e, cid);
            assetRow(k, "asset", "IK Rig", ik->asset);
            ImGui::BeginDisabled(playing);
            f32 weight = ik->weight;
            gestureEdit(k, ImGui::SliderFloat("IK weight", &weight, 0.0f, 1.0f, "%.2f"), [&] { ik->weight = weight; });
            const struct { const char* label; u32 off; } kParts[] = {
                {"Foot placement", anim::kIkRigFeetOff}, {"Goals", anim::kIkRigGoalsOff}, {"Body", anim::kIkRigBodyOff}};
            for (const auto& p : kParts) {
                bool on = (ik->flags & p.off) == 0;
                instantEdit(k, ImGui::Checkbox(p.label, &on), [&] {
                    if (on) ik->flags &= ~p.off; else ik->flags |= p.off;
                });
            }
            // "Foot IK weight" is IK weight while goals and body are off: the one authored weight, no extra field.
            if (ImGui::Button("Feet only"))
                instantEdit(k, true, [&] { ik->flags = (ik->flags & ~anim::kIkRigFeetOff) | anim::kIkRigGoalsOff | anim::kIkRigBodyOff; });
            ImGui::EndDisabled();
            anim::IkRigRuntime& rt = anim::controlRigSystem().ik();
            ImGui::TextDisabled("Ground probe  %s   probes last tick  %u", rt.hasGroundProbe() ? "installed" : "none", rt.probesLastTick());
            if (playing && rt.hasGroundProbe() && rt.probesLastTick() == 0)
                ImGui::TextDisabled("No foot rays ran: feet need collision under them.");
            if (!playing)
                ImGui::TextDisabled("Foot placement casts through physics, which is fully live in Play.");
        }
        // ---- Morph Targets ----
        else if (t == "MorphTargets") {
            auto* mt = w.component<anim::CMorphTargets>(e, cid);
            assetRow(k, "rig", "Face rig", mt->faceRig);
            if (mt->faceRig == 0) ImGui::TextDisabled("No rig: the 52 ARKit shape names and the 15 visemes work on any mesh that has them.");
            ImGui::BeginDisabled(playing);
            f32 weight = mt->weight == 0.0f ? 1.0f : mt->weight;
            gestureEdit(k, ImGui::SliderFloat("Weight", &weight, 0.01f, 2.0f, "%.2f"), [&] { mt->weight = weight; });
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scales every blend shape. Use Enabled to turn them off: a weight of 0 reads as 1.");
            const struct { const char* label; u32 bit; bool inverted; } kFlags[] = {
                {"Enabled", anim::kMorphOff, true}, {"Animation drives it", anim::kMorphIgnoreAnim, true},
                {"Inherit from parent", anim::kMorphInheritParent, false}};
            for (const auto& f : kFlags) {
                bool on = f.inverted ? (mt->flags & f.bit) == 0 : (mt->flags & f.bit) != 0;
                instantEdit(k, ImGui::Checkbox(f.label, &on), [&] {
                    if (on == f.inverted) mt->flags &= ~f.bit; else mt->flags |= f.bit;
                });
            }
            ImGui::EndDisabled();
            anim::MorphSystem& ms = anim::animSystem().morphs();
            if (const anim::MorphFrame* fr = ms.frame(e)) {
                if (ms.declaredCount(e) == 0) ImGui::TextDisabled("This mesh has no blend shapes (or has not been drawn yet).");
                else ImGui::TextDisabled("Blend shapes  %u   active  %u", ms.declaredCount(e), static_cast<u32>(fr->active.size()));
            }
        }
#if AVER_MODULE_CAMERA
        // ---- Camera Rig ----
        else if (t == "CameraRig") {
            namespace cm = editor::cammodel;
            camera::CameraRigSystem& cams = camera::cameraRigSystem();
            auto* rig = w.component<camera::CCameraRig>(e, cid);
            const auto presetNow = static_cast<camera::CameraPreset>(rig->preset < camera::kCameraPresetCount ? rig->preset : 0u);
            const auto modeNow = static_cast<camera::CameraMode>(rig->mode < camera::kCameraModeCount ? rig->mode : 0u);
            using Rig = camera::CCameraRig;
            const auto dragF = [&](const char* label, f32 Rig::* m, f32 speed, f32 lo, f32 hi, const char* fmtText, const char* tip) {
                f32 v = rig->*m;
                const bool ch = ImGui::DragFloat(label, &v, speed, lo, hi, fmtText);
                if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
                gestureEdit(k, ch, [&] { rig->*m = v; });
            };
            const auto dragV3 = [&](const char* label, f32 Rig::* mx, f32 Rig::* my, f32 Rig::* mz, const char* tip) {
                f32 v[3] = {rig->*mx, rig->*my, rig->*mz};
                const bool ch = ImGui::DragFloat3(label, v, 1.0f, -5000.0f, 5000.0f, "%.0f");
                if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
                gestureEdit(k, ch, [&] { rig->*mx = v[0]; rig->*my = v[1]; rig->*mz = v[2]; });
            };
            const auto flag = [&](const char* label, u32 bit, bool negative, const char* tip) {
                bool on = negative ? (rig->flags & bit) == 0 : (rig->flags & bit) != 0;
                const bool ch = ImGui::Checkbox(label, &on);
                if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
                const bool setBit = negative ? !on : on;
                instantEdit(k, ch, [&] { rig->flags = setBit ? (rig->flags | bit) : (rig->flags & ~bit); });
            };
            ImGui::BeginDisabled(playing);
            if (ImGui::BeginCombo("Preset", cm::presetRow(presetNow).label)) {
                for (const cm::PresetRow& row : cm::kPresetRows) {
                    if (!ImGui::Selectable(row.label, row.preset == presetNow)) continue;
                    auto before = cap(k);   // one undo step: the preset replaces the whole tuning
                    const fmt::OcPlacedComponent rec = cm::applyPreset(before ? *before : editor::makeNewRecord(k), row.preset);
                    if (game::applyPlacedComponent(w, e, rec)) commitPlacedEdit(k, std::move(before));
                }
                ImGui::EndCombo();
            }
            if (ImGui::BeginCombo("Mode", cm::modeRow(modeNow).label)) {
                for (const cm::ModeRow& row : cm::kModeRows)
                    if (ImGui::Selectable(row.label, row.mode == modeNow)) instantEdit(k, true, [&] { rig->mode = static_cast<u32>(row.mode); });
                ImGui::EndCombo();
            }
            const scene::Entity parent = w.parent(e);
            const bool followsParent = parent != scene::kInvalidEntity && rig->target == parent && rig->target != e;
            if (ImGui::BeginCombo("Follows", followsParent ? "Parent" : "This entity")) {
                if (ImGui::Selectable("This entity", !followsParent)) instantEdit(k, true, [&] { rig->target = e; });
                ImGui::BeginDisabled(parent == scene::kInvalidEntity);
                if (ImGui::Selectable("Parent", followsParent)) instantEdit(k, true, [&] { rig->target = parent; });
                ImGui::EndDisabled();
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("Arm");
            dragF("Length (cm)", &Rig::armLengthCm, 2.0f, 0.0f, 5000.0f, "%.0f", "0 reads 450");
            dragF("Closest (cm)", &Rig::minArmCm, 0.5f, 0.0f, 500.0f, "%.0f", "The closest the camera may come to the pivot. 0 reads 20");
            dragF("Probe radius (cm)", &Rig::probeRadiusCm, 0.25f, 0.0f, 100.0f, "%.1f", "0 reads 12");
            dragF("Recover half-life (s)", &Rig::recoverHalfLifeS, 0.01f, 0.0f, 5.0f, "%.2f", "How fast the arm returns outward after a hit. Inward is instant. 0 reads 0.25");
            flag("Collide with the world", camera::kRigNoCollision, true, "A swept sphere keeps the camera out of walls. Triggers do not stop it.");
            if (ImGui::TreeNode("Collision layers")) {
                const u32 mask = cm::effectiveMask(rig->collisionMask, cams.options().defaultProbeMask);
                for (u32 layer = 0; layer < 32; ++layer) {
                    bool on = cm::layerOn(mask, layer);
                    char lbl[24];
                    std::snprintf(lbl, sizeof lbl, "%u##lyr%u", layer, layer);
                    if (ImGui::Checkbox(lbl, &on)) instantEdit(k, true, [&] { rig->collisionMask = cm::toggleLayer(mask, layer); });
                    if (layer % 8 != 7) ImGui::SameLine();
                }
                if (ImGui::SmallButton("Use the default (all but cloth and proxy)")) instantEdit(k, true, [&] { rig->collisionMask = 0; });
                ImGui::TreePop();
            }
            ImGui::TextDisabled("Offsets");
            dragV3("Pivot (world)", &Rig::pivotX, &Rig::pivotY, &Rig::pivotZ, "Offset of the arm origin in world axes: eye height");
            dragV3("Socket (fwd, right, up)", &Rig::socketX, &Rig::socketY, &Rig::socketZ, "Offset at the arm's end in the camera frame");
            ImGui::TextDisabled("Lag");
            dragF("Position lag (1/s)", &Rig::posLagSpeed, 0.1f, 0.0f, 100.0f, "%.1f", "0 = none. Reduce motion raises it to 40");
            dragF("Max lag (cm)", &Rig::lagMaxDistCm, 1.0f, 0.0f, 2000.0f, "%.0f", "0 = unlimited");
            dragF("Rotation lag (1/s)", &Rig::rotLagSpeed, 0.1f, 0.0f, 100.0f, "%.1f", "0 = none");
            dragF("Max rotation lag (deg)", &Rig::rotLagMaxDeg, 0.5f, 0.0f, 180.0f, "%.0f", "0 = unlimited");
            dragF("Look-ahead (s)", &Rig::lookAheadS, 0.01f, 0.0f, 2.0f, "%.2f", "The pivot leads by planar velocity times this. 0 = off");
            dragF("Look-ahead max (cm)", &Rig::lookAheadMaxCm, 1.0f, 0.0f, 1000.0f, "%.0f", "0 reads 150");
            ImGui::TextDisabled("Rotation");
            dragF("Pitch min (deg)", &Rig::pitchMinDeg, 0.5f, -89.0f, 0.0f, "%.0f", "Both limits 0 reads -80 / +80");
            dragF("Pitch max (deg)", &Rig::pitchMaxDeg, 0.5f, 0.0f, 89.0f, "%.0f", nullptr);
            dragF("Own yaw (deg)", &Rig::yawDeg, 0.5f, -360.0f, 360.0f, "%.0f", "Orbit, TopDown, a no-inherit axis, and the Seat head look");
            dragF("Own pitch (deg)", &Rig::pitchDeg, 0.5f, -89.0f, 89.0f, "%.0f", nullptr);
            dragF("Recentre after (s)", &Rig::recenterDelayS, 0.1f, 0.0f, 20.0f, "%.1f", "Orbit or own-yaw Chase eases behind the target after this long without look input. 0 = off");
            flag("Inherit yaw", camera::kRigNoInheritYaw, true, nullptr);
            flag("Inherit pitch", camera::kRigNoInheritPitch, true, nullptr);
            flag("Inherit roll", camera::kRigNoInheritRoll, true, nullptr);
            flag("Look at the target (Fixed)", camera::kRigLookAtTarget, false, nullptr);
            ImGui::TextDisabled("Blend");
            dragF("Blend time (s)", &Rig::blendTimeS, 0.02f, 0.0f, 10.0f, "%.2f", "Seconds to blend INTO this rig or mode. 0 = cut");
            {
                static const char* const kCurves[] = {"Ease in-out", "Linear", "Ease in", "Ease out"};
                int curve = static_cast<int>(rig->blendCurve < 4u ? rig->blendCurve : 0u);
                if (ImGui::Combo("Blend curve", &curve, kCurves, 4)) instantEdit(k, true, [&] { rig->blendCurve = static_cast<u32>(curve); });
            }
            ImGui::TextDisabled("Feel");
            dragF("FOV kick (deg)", &Rig::fovKickDeg, 0.1f, 0.0f, 40.0f, "%.1f", "Added FOV at the speed below. 0 = off");
            dragF("FOV kick speed (cm/s)", &Rig::fovKickSpeedCmS, 10.0f, 0.0f, 10000.0f, "%.0f", "0 reads 1500");
            dragF("Fade below (cm)", &Rig::fadeEndCm, 1.0f, 0.0f, 500.0f, "%.0f", "The camera nearer than this to the pivot hides the target's meshes. Both fade values 0 read 140 / 70");
            dragF("Fade start (cm)", &Rig::fadeStartCm, 1.0f, 0.0f, 500.0f, "%.0f", nullptr);
            flag("Collision fade", camera::kRigNoFade, true, nullptr);
            dragF("Head bob", &Rig::bobAmount, 0.01f, 0.0f, 1.0f, "%.2f", "First person and Seat only. 0 = off");
            dragF("Seat yaw limit (deg)", &Rig::seatYawLimitDeg, 1.0f, 0.0f, 180.0f, "%.0f", "0 reads 110");
            dragF("Seat pitch limit (deg)", &Rig::seatPitchLimitDeg, 1.0f, 0.0f, 89.0f, "%.0f", "0 reads 50");
            flag("Disabled", camera::kRigDisabled, false, "Never selected as the view");
            ImGui::EndDisabled();
            if (const auto* cam = w.component<scene::CCamera>(e, scene::kComponentCamera);
                cam && cm::probeTooSmall(*rig, cam->nearCm, cam->fovYRad, viewAspect()))
                ImGui::TextColored(kRed, "The probe radius is below the near-plane corner (%.1f cm): the camera can see through the surface it stops on.",
                                   static_cast<double>(cm::nearPlaneWarningRadius(*rig, cam->nearCm, cam->fovYRad, viewAspect())));
            if (playing) ImGui::TextDisabled("%s", cm::outLine(*rig).c_str());
            ImGui::BeginDisabled(!playing);
            if (ImGui::SmallButton("Make live")) cams.setViewTarget(e, camera::ViewBlend{});
            ImGui::SameLine();
            if (ImGui::SmallButton("Cut")) cams.cut(e);
#  if AVER_MODULE_FRAMEWORK
            ImGui::SameLine();
            if (ImGui::SmallButton("Test shake")) aver_fw_camera_shake_add(0.6f);
#  endif
            ImGui::SameLine();
            if (ImGui::SmallButton("Test FOV kick")) {
                camera::CameraModifier kick;
                kick.kind = camera::ModKind::FovKick; kick.a = 8.0f; kick.inS = 0.05f; kick.holdS = 0.1f; kick.outS = 0.4f;
                cams.pushModifier(kick);
            }
            ImGui::EndDisabled();
            if (cameraLookRig_ == e) {
                if (ImGui::SmallButton("Stop looking through (Esc)") || ImGui::IsKeyPressed(ImGuiKey_Escape)) cameraLookRig_ = scene::kInvalidEntity;
            } else if (ImGui::SmallButton("Look through")) {
                cameraLookRig_ = e;
            }
        }
#endif
#if AVER_MODULE_PARTICLES
        // ---- Particle Emitter ----
        else if (t == "ParticleEmitter") {
            auto* pe = w.component<scene::CParticleEmitter>(e, cid);
            assetRow(k, "effect", "Effect", pe->effect);
            ImGui::BeginDisabled(playing);
            static const char* kSim[] = {"Auto", "CPU", "GPU"};
            int sim = (pe->flags & scene::kParticleEmitterForceGpu) ? 2 : ((pe->flags & scene::kParticleEmitterForceCpu) ? 1 : 0);
            instantEdit(k, ImGui::Combo("Simulate on", &sim, kSim, 3), [&] {
                pe->flags &= ~(scene::kParticleEmitterForceCpu | scene::kParticleEmitterForceGpu);
                if (sim == 1) pe->flags |= scene::kParticleEmitterForceCpu;
                else if (sim == 2) pe->flags |= scene::kParticleEmitterForceGpu;
            });
            bool isPlaying = (pe->flags & scene::kParticleEmitterStopped) == 0;
            instantEdit(k, ImGui::Checkbox("Playing", &isPlaying), [&] {
                if (isPlaying) pe->flags &= ~scene::kParticleEmitterStopped; else pe->flags |= scene::kParticleEmitterStopped;
            });
            bool autoDestroy = (pe->flags & scene::kParticleEmitterAutoDestroy) != 0;
            instantEdit(k, ImGui::Checkbox("Destroy when finished", &autoDestroy), [&] {
                if (autoDestroy) pe->flags |= scene::kParticleEmitterAutoDestroy; else pe->flags &= ~scene::kParticleEmitterAutoDestroy;
            });
            ImGui::EndDisabled();
            u32 aliveNow = 0, tierNow = 0;
            if (particles::particleSystem().emitterState(e, aliveNow, tierNow))
                ImGui::TextDisabled("Tier  %s   alive  %u%s", tierNow ? "GPU" : "CPU", aliveNow, tierNow ? "  (a few frames old)" : "");
            else
                ImGui::TextDisabled("Not simulated yet.");
            if (playing) {
                if (ImGui::Button("Burst 20")) particles::particleSystem().burst(e, 20);
                ImGui::SameLine();
                if (ImGui::Button("Stop")) particles::particleSystem().stop(e, false);
            }
        }
#endif
#if AVER_MODULE_SOFTBODY_SCENE
        // ---- Cloth / Destructible ----
        else if (t == "Cloth" || t == "Destructible") {
            softBodySection(k, t == "Destructible", cid);
        }
        // ---- Soft Field ----  (the body is SoftFieldSection.cpp)
        else if (t == "SoftField") {
            editor::drawSoftFieldSection(makeSoftEnv(k), &w, static_cast<u32>(e));
        }
#endif

#if AVER_MODULE_VEHICLE_SCENE
        // ---- Vehicle ----  (the body is VehicleDetailsSection.cpp; this block only hands it the undo helpers, the asset row and the Content Browser)
        else if (t == "Vehicle") {
            editor::VehicleSectionEnv env;
            env.playing = playing;
            env.instantEdit = [&](bool changed, const std::function<void()>& mutate) { instantEdit(k, changed, mutate); };
            env.gestureEdit = [&](bool changed, const std::function<void()>& mutate) { gestureEdit(k, changed, mutate); };
            env.assetRow = [&](const char* key, const char* label, u64 id) { assetRow(k, key, label, id); };
            env.pathOfId = [this](u64 id) { return pathOfContentId(id); };
            env.fileExists = [this](const std::string& rel) {
                std::error_code ec;
                return std::filesystem::exists(std::filesystem::path(project_.contentDir()) / rel, ec);
            };
            env.createAsset = [&](const std::string& rel, const std::string& text) {
                const std::filesystem::path target = std::filesystem::path(project_.contentDir()) / rel;
                std::string why;
                if (!editor::writeNewFile(target.string(), text, &why)) { cbStatus_ = "Could not write " + rel + ": " + why; return false; }
                cbAdoptNewAsset(target, false);
                changeAsset(k, "asset", rel);
                return true;
            };
            env.openEditor = [this](const std::string& rel) { assetEditors_.open((std::filesystem::path(project_.contentDir()) / rel).string()); };
            env.setStatus = [this](const std::string& s) { cbStatus_ = s; };
            if (const auto* mr = w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer)) env.meshRel = pathOfContentId(mr->mesh);
            editor::drawVehicleSection(env, &w, static_cast<u32>(e));
        }
#endif
#if AVER_MODULE_SYNAPSE_SCENE
        // ---- AI Brain ----
        else if (t == "Brain") {
            synapse::BtSystem& bt = synapse::btSystem();
            auto* sb = w.component<synapse::CSynapseBehavior>(e, cid);
            std::string brainPath;
            if (const auto* tree = bt.findTree(sb->treeAssetId)) brainPath = tree->path;
            else if (const auto* st = bt.findStateTree(sb->treeAssetId)) brainPath = st->path;
            ImGui::BeginDisabled(playing);
            {
                std::string path = brainPath;
                if (sb->treeAssetId != 0 && path.empty())
                    ImGui::TextColored(kRed, "Brain asset 0x%016llx is not loaded", static_cast<unsigned long long>(sb->treeAssetId));
                // Clearing the path leaves an empty brain (the applier stops the tree first).
                if (drawAssetField("asset", "Brain", path, editor::placedAssetExts(k))) changeAsset(k, "asset", path);
            }
            static char s_team[64] = {};
            static bool s_teamActive = false;
            if (!s_teamActive) {
                const std::string* tn = bt.teamName(sb->teamId);
                std::snprintf(s_team, sizeof s_team, "%s", tn ? tn->c_str() : "");
            }
            ImGui::InputText("Team", s_team, sizeof s_team);
            s_teamActive = ImGui::IsItemActive();
            if (ImGui::IsItemActivated()) beginPlacedGesture(k);
            if (ImGui::IsItemDeactivatedAfterEdit() && placedGesture_ == &k) {
                bt.setTeam(w, e, s_team);
                commitPlacedEdit(k, placedBefore_);
            } else if (ImGui::IsItemDeactivated() && placedGesture_ == &k) {
                placedGesture_ = nullptr;
                placedBefore_.reset();
            }
            ImGui::EndDisabled();
            static const char* const kKinds[] = {"none", "behaviour tree", "state tree"};
            static const char* const kStatus[] = {"idle", "running", "succeeded", "failed"};
            const i32 kind = bt.brainKind(e), status = bt.brainStatus(e);
            ImGui::TextDisabled("Kind  %s   status  %s", kKinds[kind >= 0 && kind <= 2 ? kind : 0],
                                kStatus[status >= 0 && status <= 3 ? status : 0]);
            if (kind == 2) {
                const std::string active = bt.activeStatePath(e);
                ImGui::TextDisabled("Active state  %s", active.empty() ? "-" : active.c_str());
            }
            ImGui::BeginDisabled(!playing);
            if (ImGui::Button("Restart")) bt.brainRestart(e);
            ImGui::SameLine();
            if (ImGui::Button("Stop")) bt.brainStop(e);
            ImGui::EndDisabled();
            if (!playing) ImGui::TextDisabled("A brain runs during Play.");
        }
        // ---- Smart Object ----
        else if (t == "SmartObject") {
            auto* so = w.component<synapse::CSmartObject>(e, cid);
            const u32 assetField = w.fieldId("CSmartObject.asset");
            const char* cur = assetField ? aver_scene_get_str(static_cast<i32>(e), static_cast<i32>(assetField)) : "";
            std::string path = cur ? cur : "";
            ImGui::BeginDisabled(playing);
            if (drawAssetField("asset", "Smart Object", path, editor::placedAssetExts(k))) changeAsset(k, "asset", path);
            bool enabled = so->enabled != 0;
            instantEdit(k, ImGui::Checkbox("Enabled", &enabled), [&] { so->enabled = enabled ? 1 : 0; });
            ImGui::EndDisabled();
#if AVER_WITH_SYNAPSE_AI
            // The slot count lives in the Synapse DLL's registry, which registers the object on first ask: Play only.
            if (playing) {
                auto* ai = static_cast<synapse::SynapseAi*>(aver_syn_ai_instance());
                const i32 slots = ai ? ai->smartObjects().slotCount(w, e) : -1;
                if (slots >= 0) ImGui::TextDisabled("Slots  %d   free  %d", slots, so->slotsFree);
                else            ImGui::TextDisabled("Slots  not registered yet");
            } else {
                ImGui::TextDisabled("Slots and Free are shown during Play.");
            }
#else
            ImGui::TextDisabled("Free  %d", so->slotsFree);
#endif
        }
#endif
        ImGui::PopID();
    }
}
#endif // AVER_WITH_IMGUI

} // namespace aver
#endif // AVER_MODULE_SCENE
