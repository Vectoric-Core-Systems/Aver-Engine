// The .ocface editor tab. See the header for the SandboxApp hook and for why the pose edits are free functions.
#include "FaceRigEditor.hpp"

#include "ActorEditor.hpp"
#include "EditorKeybinds.hpp"
#include "SkinPreview.hpp"

#include "aver/anim/Morph.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/render/preview/ActorPreview.hpp"
#include "aver/runtime/Engine.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>

#if AVER_WITH_IMGUI
#  include "imgui.h"
#endif

namespace aver::editor {

namespace fs = std::filesystem;

namespace {

bool inRange(usize n, i32 i) { return i >= 0 && static_cast<usize>(i) < n; }

bool nameTaken(const anim::FaceRigAsset& a, const std::string& n, i32 except = -1) {
    for (usize i = 0; i < a.poses.size(); ++i)
        if (static_cast<i32>(i) != except && a.poses[i].name == n) return true;
    return false;
}

} // namespace

// ------------------------------------------------------------------------------- edits ----------

std::string faceRigUniquePoseName(const anim::FaceRigAsset& a, const std::string& base) {
    const std::string b = base.empty() ? "pose" : base;
    if (!nameTaken(a, b)) return b;
    for (int n = 2;; ++n) {
        const std::string cand = b + " " + std::to_string(n);
        if (!nameTaken(a, cand)) return cand;
    }
}

i32 faceRigAddPose(anim::FaceRigAsset& a, const std::string& name) {
    if (a.poses.size() >= anim::kFaceRigMaxPoses) return -1;
    anim::FacePose p;
    p.name = faceRigUniquePoseName(a, name);
    a.poses.push_back(std::move(p));
    return static_cast<i32>(a.poses.size()) - 1;
}

i32 faceRigDuplicatePose(anim::FaceRigAsset& a, i32 index) {
    if (!inRange(a.poses.size(), index) || a.poses.size() >= anim::kFaceRigMaxPoses) return -1;
    anim::FacePose p = a.poses[static_cast<usize>(index)];
    p.name = faceRigUniquePoseName(a, p.name + " copy");
    a.poses.insert(a.poses.begin() + index + 1, std::move(p));
    return index + 1;
}

bool faceRigRemovePose(anim::FaceRigAsset& a, i32 index) {
    if (!inRange(a.poses.size(), index)) return false;
    a.poses.erase(a.poses.begin() + index);
    return true;
}

bool faceRigRenamePose(anim::FaceRigAsset& a, i32 index, const std::string& name) {
    if (!inRange(a.poses.size(), index) || name.empty() || name.size() > anim::kFaceNameMax || nameTaken(a, name, index)) return false;
    a.poses[static_cast<usize>(index)].name = name;
    return true;
}

u32 faceRigImportArkit(anim::FaceRigAsset& a) {
    u32 added = 0;
    for (const anim::FacePose& p : anim::defaultFaceRig().poses) {
        if (a.poses.size() >= anim::kFaceRigMaxPoses) break;
        if (a.find(p.name)) continue;
        a.poses.push_back(p);
        ++added;
    }
    return added;
}

std::vector<std::string> faceRigMissingTargets(const anim::FaceRigAsset& a, const std::vector<std::string>& meshTargets) {
    std::set<std::string> have(meshTargets.begin(), meshTargets.end());
    std::set<std::string> seen;
    std::vector<std::string> out;
    for (const anim::FacePose& p : a.poses)
        for (const anim::FaceTargetRef& t : p.targets)
            if (!have.count(t.name) && seen.insert(t.name).second) out.push_back(t.name);
    return out;
}

// ------------------------------------------------------------------------------- the tab ----------

FaceRigEditor::FaceRigEditor(std::string path) : path_(std::move(path)) { loadFromDisk(); }

FaceRigEditor::~FaceRigEditor() = default;

void FaceRigEditor::loadFromDisk() {
    std::string why;
    anim::FaceRigAsset a;
    if (!anim::loadFaceRig(path_, a, &why)) {
        loaded_ = false;
        loadError_ = why;
        return;
    }
    asset_ = std::move(a);
    loaded_ = true;
    loadError_.clear();
    dirty_ = false;
    ++rev_;
    sel_ = asset_.poses.empty() ? -1 : 0;
    history_.clear();
#if AVER_WITH_IMGUI
    cachedMeshRel_ = cachedSkelRel_ = "\x01";   // force the hints to be resolved again
    bindTried_ = false;
#endif
}

std::string FaceRigEditor::title() const { return fs::path(path_).filename().string() + "###ocface:" + path_; }

bool FaceRigEditor::save(std::string* why) {
    if (!loaded_) {
        if (why) *why = "cannot save: file failed to load (" + loadError_ + ")";
        return false;
    }
    if (!anim::saveFaceRig(path_, asset_, why)) return false;
    dirty_ = false;
    return true;
}

void FaceRigEditor::onFileChanged() {
    if (dirty_) {
        AVER_WARN("[FaceRigEditor] '{}' changed on disk, but this tab has unsaved edits -- keeping them", path_);
        return;
    }
    loadFromDisk();
}

void FaceRigEditor::undo() {
    if (!history_.undo(asset_)) return;
    dirty_ = true;
    ++rev_;
    if (!inRange(asset_.poses.size(), sel_)) sel_ = asset_.poses.empty() ? -1 : static_cast<i32>(asset_.poses.size()) - 1;
}

void FaceRigEditor::redo() {
    if (!history_.redo(asset_)) return;
    dirty_ = true;
    ++rev_;
    if (!inRange(asset_.poses.size(), sel_)) sel_ = asset_.poses.empty() ? -1 : static_cast<i32>(asset_.poses.size()) - 1;
}

i32 FaceRigEditor::addPose(const std::string& name) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 i = faceRigAddPose(asset_, name);
    if (i < 0) { history_.cancelPush(); return -1; }
    sel_ = i;
    markDirty();
    return i;
}

i32 FaceRigEditor::duplicatePose(i32 index) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 i = faceRigDuplicatePose(asset_, index);
    if (i < 0) { history_.cancelPush(); return -1; }
    sel_ = i;
    markDirty();
    return i;
}

bool FaceRigEditor::removePose(i32 index) {
    if (!loaded_ || !inRange(asset_.poses.size(), index)) return false;
    pushUndo();
    faceRigRemovePose(asset_, index);
    if (!inRange(asset_.poses.size(), sel_)) sel_ = asset_.poses.empty() ? -1 : static_cast<i32>(asset_.poses.size()) - 1;
    markDirty();
    return true;
}

bool FaceRigEditor::renamePose(i32 index, const std::string& name) {
    if (!loaded_) return false;
    pushUndo();
    if (!faceRigRenamePose(asset_, index, name)) { history_.cancelPush(); return false; }
    markDirty();
    return true;
}

u32 FaceRigEditor::importArkit() {
    if (!loaded_) return 0;
    pushUndo();
    const u32 n = faceRigImportArkit(asset_);
    if (n == 0) { history_.cancelPush(); return 0; }
    markDirty();
    return n;
}

std::string FaceRigEditor::resolveContent(const std::string& rel) const {
    if (rel.empty()) return {};
    const fs::path p(rel);
    std::error_code ec;
    if (p.is_absolute()) return fs::exists(p, ec) ? p.string() : std::string();
    for (fs::path d = fs::path(path_).parent_path(); !d.empty(); d = d.parent_path()) {
        const fs::path cand = d / p;
        if (fs::exists(cand, ec)) return cand.string();
        if (d == d.parent_path()) break;
    }
    return {};
}

#if AVER_WITH_IMGUI

namespace {

// Euler angles in degrees (yaw about z, pitch about y, roll about x) for the joint rotation fields, ZYX order:
// q = qz(yaw) * qy(pitch) * qx(roll), the order Quat::operator* composes.
Vec3 toEulerDeg(const Quat& q) {
    const f32 roll = std::atan2(2.0f * (q.w * q.x + q.y * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const f32 s = std::fmax(-1.0f, std::fmin(1.0f, 2.0f * (q.w * q.y - q.z * q.x)));
    const f32 pitch = std::asin(s);
    const f32 yaw = std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.y * q.y + q.z * q.z));
    return Vec3{roll * kRadToDeg, pitch * kRadToDeg, yaw * kRadToDeg};
}

Quat fromEulerDeg(const Vec3& e) {
    const Quat qx = Quat::fromAxisAngle(Vec3{1, 0, 0}, e.x * kDegToRad);
    const Quat qy = Quat::fromAxisAngle(Vec3{0, 1, 0}, e.y * kDegToRad);
    const Quat qz = Quat::fromAxisAngle(Vec3{0, 0, 1}, e.z * kDegToRad);
    return (qz * qy * qx).normalized();
}

} // namespace

void FaceRigEditor::touch(bool changed) {
    if (changed) {
        if (!gesture_) { history_.push(before_); gesture_ = true; }
        markDirty();
    }
    if (ImGui::IsItemDeactivated()) endGesture_ = true;
}

void FaceRigEditor::drawLeft() {
    ImGui::TextDisabled("POSES  %zu", asset_.poses.size());
    if (ImGui::SmallButton("Add")) addPose("pose");
    ImGui::SameLine();
    ImGui::BeginDisabled(!inRange(asset_.poses.size(), sel_));
    if (ImGui::SmallButton("Duplicate")) duplicatePose(sel_);
    ImGui::SameLine();
    if (ImGui::SmallButton("Delete")) removePose(sel_);
    ImGui::EndDisabled();
    if (ImGui::SmallButton("Import ARKit defaults")) {
        const u32 n = importArkit();
        status_ = n ? "Added " + std::to_string(n) + " built-in pose(s)." : "Every built-in pose is already here.";
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Adds the 52 ARKit shapes and the 15 visemes the rig does not have yet.");
    ImGui::Separator();
    if (ImGui::BeginChild("##poselist", ImVec2(0, 0), false)) {
        for (usize i = 0; i < asset_.poses.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(asset_.poses[i].name.c_str(), static_cast<i32>(i) == sel_)) sel_ = static_cast<i32>(i);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void FaceRigEditor::bindPreview(Engine& e) {
    SkinPreview* skin = sharedSkinPreview(e);
    if (!skin || !e.device()) return;
    if (cachedMeshRel_ != asset_.mesh) {
        cachedMeshRel_ = asset_.mesh;
        meshPath_ = resolveContent(asset_.mesh);
        bindTried_ = false;
    }
    if (cachedSkelRel_ != asset_.skeleton) {
        cachedSkelRel_ = asset_.skeleton;
        skelPath_ = resolveContent(asset_.skeleton);
        skelOk_ = false;
        skel_ = fmt::OcSkeleton{};
        std::string why;
        if (!skelPath_.empty() && fmt::loadOcSkel(skelPath_, skel_, &why)) skelOk_ = true;
        bindTried_ = false;
    }
    const u32 bones = skelOk_ ? static_cast<u32>(skel_.bones.size()) : 0u;
    // Another tab shares the feature: bind again when what it holds is no longer ours.
    if (bound_ && (skin->boundPath() != meshPath_ || skin->boundBones() != bones)) bindTried_ = false;
    if (bindTried_ || meshPath_.empty()) return;
    bindTried_ = true;
    bound_ = skin->bind(*e.device(), meshPath_, bones);
    rtRev_ = ~0u;   // the declared target ids changed with the mesh
    if (bound_) {
        panel_.setTargets(skin->targetNames(), skin->targetDefaults());
        panel_.allowKeying(false);
        targetIds_.clear();
        targetIndex_.clear();
        for (const std::string& n : skin->targetNames()) {
            const u64 id = fnv1a64(std::string_view(n));
            targetIndex_[id] = static_cast<u32>(targetIds_.size());
            targetIds_.push_back(id);
        }
    } else {
        panel_.setTargets({}, {});
    }
}

// This frame's weights: the selected pose at its preview weight, a held viseme, the spoken phonemes, then the sliders'
// overrides on top. Joint deltas land in the rest pose and are skinned like any clip.
void FaceRigEditor::stagePreview(Engine& e) {
    SkinPreview* skin = sharedSkinPreview(e);
    if (!skin || !bound_) return;
    // Rebound when the rig changed: the compiled poses point into asset_, which an edit moves.
    if (rtRev_ != rev_) {
        rtRev_ = rev_;
        rt_.bind(&asset_);
        rt_.setDeclared(targetIds_, true);
    }

    const usize n = targetIds_.size();
    std::vector<f32> value(n, 0.0f);
    std::vector<u8> driven(n, 0);
    if (skelOk_) anim::restPose(skel_, pose_);
    const auto addPose = [&](u64 poseId, f32 w) {
        if (w == 0.0f) return;
        expand_.clear();
        rt_.expand(poseId, w, expand_);
        for (const anim::FaceTargetOut& t : expand_) {
            const auto it = targetIndex_.find(t.target);
            if (it == targetIndex_.end()) continue;
            value[it->second] += t.weight;
            driven[it->second] = 1;
        }
        if (skelOk_) rt_.applyJoints(poseId, w, skel_, pose_);
    };
    if (inRange(asset_.poses.size(), sel_)) addPose(fnv1a64(std::string_view(asset_.poses[static_cast<usize>(sel_)].name)), poseWeight_);
    if (heldViseme_ >= 0 && heldViseme_ < static_cast<i32>(anim::kVisemeCount))
        addPose(fnv1a64(std::string_view(anim::kVisemeNames[heldViseme_])), 1.0f);
    if (speaking_) {
        std::vector<anim::VisemeSample> samples;
        anim::evalVisemeWeights(speech_, asset_.lipsync, speechTime_, samples);
        for (const anim::VisemeSample& s : samples) addPose(s.pose, s.weight);
    }

    std::vector<f32> live(n, MorphPanel::kNone);
    for (usize i = 0; i < n; ++i) if (driven[i]) live[i] = value[i];
    panel_.setLiveTargets(live);

    std::vector<render::MorphTargetWeight> out;
    for (usize i = 0; i < n; ++i) {
        const f32 ov = panel_.targetOverride(static_cast<u32>(i));
        const f32 w = !MorphPanel::isNone(ov) ? ov : (driven[i] ? value[i] : panel_.defaultOf(static_cast<u32>(i)));
        if (std::fabs(w) >= anim::kMorphActiveEps) out.push_back(render::MorphTargetWeight{static_cast<u32>(i), anim::clampMorphWeight(w)});
    }
    skin->stageMorph(std::move(out));
    if (skelOk_ && !skin->morphOnly()) {
        anim::poseToSkinning(skel_, pose_, skin_);
        skin->stage(skin_);
    }
    char line[160];
    std::snprintf(line, sizeof line, "%zu shape(s)   %u applied   %.1f KiB on the GPU", n, skin->morphActive(),
                  double(skin->morphGpuBytes()) / 1024.0);
    panel_.setStats(line);
}

void FaceRigEditor::drawPreview(Engine& e, float width, float height) {
    render::preview::ActorPreview* preview = sharedPreview(e);
    SkinPreview* skin = sharedSkinPreview(e);
    if (!preview || !skin || !e.device()) { ImGui::TextDisabled("No preview on this backend."); return; }

    const float panelH = panel_.hasTargets() ? std::max(120.0f, height * 0.38f) : 0.0f;
    const float imageH = std::max(80.0f, height - panelH - (panel_.hasTargets() ? 8.0f : 0.0f));

    if (!bound_ || !skin->drawMesh()) {
        ImGui::TextWrapped("%s", meshPath_.empty()
            ? "Set the preview mesh below (a content-relative .ocmesh with blend shapes) to see the face."
            : "That mesh could not be previewed (no blend shapes, no skin streams, or no compute on this device).");
    } else {
        std::vector<render::preview::PreviewDraw> draws;
        render::preview::PreviewDraw d;
        d.mesh = skin->drawMesh();
        d.boundsRadius = skin->boundsRadius();
        d.roughness = 0.62f;
        d.baseColor[0] = 0.78f; d.baseColor[1] = 0.76f; d.baseColor[2] = 0.72f;
        const Mat4 id = Mat4::identity();
        std::memcpy(d.world, &id.m[0][0], sizeof d.world);
        draws.push_back(d);
        preview->setDrawList(std::move(draws));
        if (!framed_) { framed_ = true; preview->frameAll(); }
    }

    if (preview->uiTextureId()) {
        const u32 w = static_cast<u32>(std::max(64.0f, width));
        const u32 hh = static_cast<u32>(std::max(64.0f, imageH));
        // Debounced, as in the Anim editor: resize() waits for the GPU to idle.
        if (w != pendingW_ || hh != pendingH_) {
            pendingW_ = w; pendingH_ = hh;
            resizeDue_ = ImGui::GetTime() + 0.25;
        } else if (resizeDue_ > 0.0 && ImGui::GetTime() >= resizeDue_) {
            resizeDue_ = 0.0;
            preview->resize(pendingW_, pendingH_);
        } else if (resizeDue_ <= 0.0 && (preview->width() != w || preview->height() != hh) &&
                   ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            resizeDue_ = ImGui::GetTime() + 0.1;
        }
        ImGui::Image(static_cast<ImTextureID>(preview->uiTextureId()), ImVec2(width, imageH));
        if (ImGui::IsItemHovered()) {
            const ImGuiIO& io = ImGui::GetIO();
            render::preview::PreviewCamera& cam = preview->camera();
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) cam.addOrbit(-io.MouseDelta.x * 0.4f, io.MouseDelta.y * 0.4f);
            if (io.MouseWheel != 0.0f) cam.addZoom(io.MouseWheel > 0.0f ? 0.9f : 1.1f);
            for (const ImGuiMouseButton b : {ImGuiMouseButton_Middle, ImGuiMouseButton_Right}) {
                if (!ImGui::IsMouseDragging(b)) continue;
                const ImVec2 dd = ImGui::GetMouseDragDelta(b);
                ImGui::ResetMouseDragDelta(b);
                cam.panPixels(dd.x, dd.y, static_cast<f32>(preview->height()));
            }
            if (ImGui::IsKeyPressed(ImGuiKey_F, false)) preview->frameAll();
        }
    }
    if (panel_.hasTargets()) {
        if (ImGui::BeginChild("##facemorph", ImVec2(0, panelH), true)) panel_.draw("faceMorph");
        ImGui::EndChild();
    }
}

void FaceRigEditor::drawPoseTab() {
    if (!inRange(asset_.poses.size(), sel_)) {
        ImGui::TextDisabled("Select or add a pose.");
        return;
    }
    anim::FacePose& p = asset_.poses[static_cast<usize>(sel_)];
    char nm[160];
    std::snprintf(nm, sizeof nm, "%s", p.name.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##posename", nm, sizeof nm);
    if (ImGui::IsItemDeactivatedAfterEdit() && std::string(nm) != p.name) {
        if (!renamePose(sel_, nm)) status_ = "That name is empty or already used.";
    }
    ImGui::SetNextItemWidth(-90.0f);
    ImGui::SliderFloat("Preview weight", &poseWeight_, 0.0f, 1.0f, "%.2f");

    // ---- targets ----
    ImGui::Separator();
    ImGui::TextDisabled("BLEND SHAPES  %zu / %u", p.targets.size(), anim::kFaceRigMaxTargets);
    const std::vector<std::string>* meshTargets = bound_ ? &skinPreviewInstance().targetNames() : nullptr;
    for (usize i = 0; i < p.targets.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        anim::FaceTargetRef& t = p.targets[i];
        ImGui::SetNextItemWidth(160.0f);
        if (meshTargets && !meshTargets->empty()) {
            if (ImGui::BeginCombo("##t", t.name.c_str())) {
                for (const std::string& mt : *meshTargets)
                    if (ImGui::Selectable(mt.c_str(), mt == t.name) && mt != t.name) { pushUndo(); t.name = mt; markDirty(); }
                ImGui::EndCombo();
            }
        } else {
            char tb[160];
            std::snprintf(tb, sizeof tb, "%s", t.name.c_str());
            if (ImGui::InputText("##t", tb, sizeof tb)) { t.name = tb; touch(true); } else touch(false);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        const bool changed = ImGui::DragFloat("##w", &t.weight, 0.01f, -4.0f, 4.0f, "%.3f");
        touch(changed);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            pushUndo();
            p.targets.erase(p.targets.begin() + static_cast<isize>(i));
            markDirty();
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    ImGui::BeginDisabled(p.targets.size() >= anim::kFaceRigMaxTargets);
    if (meshTargets && !meshTargets->empty()) {
        if (ImGui::BeginCombo("##addt", "Add blend shape...")) {
            for (const std::string& mt : *meshTargets) {
                bool used = false;
                for (const anim::FaceTargetRef& x : p.targets) if (x.name == mt) used = true;
                ImGui::BeginDisabled(used);
                if (ImGui::Selectable(mt.c_str())) { pushUndo(); p.targets.push_back({mt, 1.0f}); markDirty(); }
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
    } else if (ImGui::SmallButton("Add blend shape")) {
        pushUndo();
        p.targets.push_back({"target", 1.0f});
        markDirty();
    }
    ImGui::EndDisabled();

    // ---- joints ----
    ImGui::Separator();
    ImGui::TextDisabled("BONE DELTAS  %zu / %u", p.joints.size(), anim::kFaceRigMaxJoints);
    if (!skelOk_) ImGui::TextDisabled("Set the skeleton below to pick bones by name.");
    for (usize i = 0; i < p.joints.size(); ++i) {
        ImGui::PushID(1000 + static_cast<int>(i));
        anim::FaceJoint& j = p.joints[i];
        ImGui::SetNextItemWidth(160.0f);
        if (skelOk_) {
            if (ImGui::BeginCombo("##b", j.bone.empty() ? "<bone>" : j.bone.c_str())) {
                for (const fmt::OcBone& b : skel_.bones)
                    if (ImGui::Selectable(b.name.c_str(), b.name == j.bone) && b.name != j.bone) { pushUndo(); j.bone = b.name; markDirty(); }
                ImGui::EndCombo();
            }
        } else {
            char bb[160];
            std::snprintf(bb, sizeof bb, "%s", j.bone.c_str());
            if (ImGui::InputText("##b", bb, sizeof bb)) { j.bone = bb; touch(true); } else touch(false);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            pushUndo();
            p.joints.erase(p.joints.begin() + static_cast<isize>(i));
            markDirty();
            ImGui::PopID();
            break;
        }
        f32 pos[3] = {j.position.x, j.position.y, j.position.z};
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::DragFloat3("position (cm)", pos, 0.05f)) { j.position = Vec3{pos[0], pos[1], pos[2]}; touch(true); } else touch(false);
        const Vec3 eu = toEulerDeg(j.rotation);
        f32 rot[3] = {eu.x, eu.y, eu.z};
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::DragFloat3("rotation (deg)", rot, 0.2f)) { j.rotation = fromEulerDeg(Vec3{rot[0], rot[1], rot[2]}); touch(true); } else touch(false);
        f32 scl[3] = {j.scale.x, j.scale.y, j.scale.z};
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::DragFloat3("scale", scl, 0.005f)) { j.scale = Vec3{scl[0], scl[1], scl[2]}; touch(true); } else touch(false);
        ImGui::PopID();
    }
    ImGui::BeginDisabled(p.joints.size() >= anim::kFaceRigMaxJoints);
    if (ImGui::SmallButton("Add bone delta")) {
        pushUndo();
        anim::FaceJoint j;
        j.bone = skelOk_ && !skel_.bones.empty() ? skel_.bones[0].name : std::string("bone");
        p.joints.push_back(j);
        markDirty();
    }
    ImGui::EndDisabled();
}

void FaceRigEditor::playPhonemes(const std::string& text) {
    speech_.clear();
    f32 at = 0.0f;
    usize i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        const usize b = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i == b) break;
        anim::VisemeEvent ev;
        ev.symbol = text.substr(b, i - b);
        ev.start = at;
        ev.end = at + 0.08f;
        at = ev.end;
        speech_.push_back(std::move(ev));
    }
    std::vector<std::string> unknown;
    anim::resolveVisemeEvents(speech_, &asset_, false, &unknown);
    status_ = unknown.empty() ? std::string() : "Not a phoneme or viseme: " + unknown.front();
    speechTime_ = 0.0f;
    speaking_ = !speech_.empty();
}

void FaceRigEditor::drawVisemeTab() {
    ImGui::TextDisabled("HOLD A BUTTON TO PREVIEW THE MOUTH SHAPE");
    for (u32 i = 0; i < anim::kVisemeCount; ++i) {
        if (i % 3 != 0) ImGui::SameLine();
        ImGui::Button(anim::kVisemeNames[i]);
        if (ImGui::IsItemActive()) heldNext_ = static_cast<i32>(i);
    }
    ImGui::Separator();
    ImGui::TextDisabled("SPEAK");
    ImGui::SetNextItemWidth(-70.0f);
    ImGui::InputText("##phonemes", phonemeBuf_, sizeof phonemeBuf_);
    ImGui::SameLine();
    if (ImGui::Button("Play")) playPhonemes(phonemeBuf_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("ARPAbet symbols or viseme names, 80 ms each.");

    ImGui::Separator();
    ImGui::TextDisabled("LIP-SYNC ENVELOPE");
    anim::LipSyncParams& ls = asset_.lipsync;
    touch(ImGui::DragFloat("Lead (s)", &ls.lead, 0.005f, 0.0f, 0.5f, "%.3f"));
    touch(ImGui::DragFloat("Attack (s)", &ls.attack, 0.005f, 0.0f, 0.5f, "%.3f"));
    touch(ImGui::DragFloat("Release (s)", &ls.release, 0.005f, 0.0f, 0.5f, "%.3f"));
    touch(ImGui::DragFloat("Min hold (s)", &ls.minHold, 0.005f, 0.0f, 0.5f, "%.3f"));
    touch(ImGui::DragFloat("Max total", &ls.maxTotal, 0.01f, 0.1f, 4.0f, "%.2f"));

    ImGui::Separator();
    ImGui::TextDisabled("PHONEME OVERRIDES  %zu", asset_.phonemes.size());
    for (usize i = 0; i < asset_.phonemes.size(); ++i) {
        ImGui::PushID(2000 + static_cast<int>(i));
        char a[64], b[128];
        std::snprintf(a, sizeof a, "%s", asset_.phonemes[i].first.c_str());
        std::snprintf(b, sizeof b, "%s", asset_.phonemes[i].second.c_str());
        ImGui::SetNextItemWidth(70.0f);
        if (ImGui::InputText("##sym", a, sizeof a)) { asset_.phonemes[i].first = a; touch(true); } else touch(false);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::InputText("##vis", b, sizeof b)) { asset_.phonemes[i].second = b; touch(true); } else touch(false);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            pushUndo();
            asset_.phonemes.erase(asset_.phonemes.begin() + static_cast<isize>(i));
            markDirty();
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton("Add override")) {
        pushUndo();
        asset_.phonemes.push_back({"XX", "viseme_aa"});
        markDirty();
    }

    ImGui::Separator();
    ImGui::TextDisabled("CHECK AGAINST THE MESH");
    if (ImGui::Button("Validate against mesh")) {
        SkinPreview& skin = skinPreviewInstance();
        missing_ = faceRigMissingTargets(asset_, skin.targetNames());
        validated_ = true;
    }
    if (validated_) {
        if (!bound_) ImGui::TextDisabled("No mesh is bound, so every name would read as missing.");
        else if (missing_.empty()) ImGui::TextDisabled("Every blend shape the poses name is on the mesh.");
        else {
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1.0f), "%zu name(s) the mesh lacks:", missing_.size());
            for (const std::string& m : missing_) ImGui::BulletText("%s", m.c_str());
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("BAKE A LIP-SYNC TRACK");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##track", "Speech/Hello.ocvis (content-relative)", trackBuf_, sizeof trackBuf_);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##out", "output .ocanim (blank = beside the track)", outBuf_, sizeof outBuf_);
    if (ImGui::Button("Bake .ocvis to clip...") && trackBuf_[0] != '\0') {
        const std::string trackPath = resolveContent(trackBuf_);
        anim::VisemeTrack track;
        std::string why;
        if (trackPath.empty()) {
            status_ = "Cannot find the track '" + std::string(trackBuf_) + "'.";
        } else if (!anim::loadVisemeTrack(trackPath, track, nullptr, &why)) {
            status_ = "Cannot read the track: " + why;
        } else {
            const fmt::OcAnimation clip = anim::bakeVisemeTrack(track, &asset_, 30.0f);
            fs::path out = outBuf_[0] != '\0' ? fs::path(outBuf_) : fs::path(trackPath).replace_extension(".ocanim");
            if (out.is_relative() && outBuf_[0] != '\0') out = fs::path(trackPath).parent_path() / out;
            if (clip.curves.empty()) status_ = "No event in the track resolved to a viseme.";
            else if (!fmt::saveOcAnim(out.string(), clip, &why)) status_ = "Cannot write the clip: " + why;
            else status_ = "Baked " + std::to_string(clip.curves.size()) + " curve(s) to " + out.filename().string();
        }
    }
}

void FaceRigEditor::draw(Engine& e) {
    if (!loaded_) {
        ImGui::TextWrapped("This file could not be read: %s", loadError_.c_str());
        return;
    }
    before_ = asset_;
    heldViseme_ = heldNext_;   // what the Visemes tab saw last frame; the preview (drawn before the tab) uses it
    heldNext_ = -1;
    if (ImGui::Button("Save") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                                  keybinds().pressed(CommandId::AssetSave, ImGui::GetIO()))) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[FaceRigEditor] save failed for '{}': {}", path_, why);
    }
    {
        const ImGuiIO& io = ImGui::GetIO();
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (focused && !io.WantTextInput) {
            if (canUndo() && keybinds().pressed(CommandId::EditUndo, io)) undo();
            if (canRedo() && keybinds().pressed(CommandId::EditRedo, io)) redo();
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!canUndo());
    if (ImGui::Button("Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canRedo());
    if (ImGui::Button("Redo")) redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    {
        char mb[260];
        std::snprintf(mb, sizeof mb, "%s", asset_.mesh.c_str());
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::InputTextWithHint("##mesh", "preview mesh (.ocmesh)", mb, sizeof mb)) { asset_.mesh = mb; touch(true); } else touch(false);
        ImGui::SameLine();
        char sb[260];
        std::snprintf(sb, sizeof sb, "%s", asset_.skeleton.c_str());
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::InputTextWithHint("##skel", "skeleton (.ocskel)", sb, sizeof sb)) { asset_.skeleton = sb; touch(true); } else touch(false);
    }
    if (!status_.empty()) ImGui::TextDisabled("%s", status_.c_str());
    ImGui::Separator();

    // The speech clock runs off ImGui's time; an asset tab only draws while active.
    const f64 now = ImGui::GetTime();
    const f32 dt = lastClock_ > 0.0 ? static_cast<f32>(now - lastClock_) : 0.0f;
    lastClock_ = now;
    if (speaking_) {
        speechTime_ += dt;
        f32 endT = 0.0f;
        for (const anim::VisemeEvent& v : speech_) endT = std::max(endT, v.end);
        if (speechTime_ > endT + asset_.lipsync.release + asset_.lipsync.lead) speaking_ = false;
    }

    bindPreview(e);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const f32 leftW = std::max(150.0f, avail.x * 0.18f), rightW = std::max(320.0f, avail.x * 0.34f);
    const f32 midW = std::max(160.0f, avail.x - leftW - rightW - 16.0f);
    if (ImGui::BeginChild("##left", ImVec2(leftW, avail.y), true)) drawLeft();
    ImGui::EndChild();
    ImGui::SameLine();

    // The right-hand tabs first (they set heldViseme_), then the preview from what they said.
    if (ImGui::BeginChild("##mid", ImVec2(midW, avail.y), true)) {
        stagePreview(e);
        drawPreview(e, ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##right", ImVec2(rightW, avail.y), true)) {
        if (ImGui::BeginTabBar("##facetabs")) {
            if (ImGui::BeginTabItem("Pose")) { drawPoseTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Visemes")) { drawVisemeTab(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }
    ImGui::EndChild();

    if (endGesture_ && !ImGui::IsMouseDown(0)) { gesture_ = false; endGesture_ = false; }
}

#else   // AVER_WITH_IMGUI

void FaceRigEditor::draw(Engine& e) { (void)e; }

#endif  // AVER_WITH_IMGUI

std::unique_ptr<AssetEditor> makeFaceRigEditor(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".ocface") return nullptr;
    return std::make_unique<FaceRigEditor>(path);
}

} // namespace aver::editor
