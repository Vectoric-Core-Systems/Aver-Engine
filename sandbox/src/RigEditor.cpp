// The .ocrig editor tab. See the header for the SandboxApp hook and for why the edits are free functions.
#include "RigEditor.hpp"
#include "EditorKeybinds.hpp"

#include "aver/anim/AnimSampler.hpp"
#include "aver/anim/IkSolvers.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcAnim.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>

#if AVER_WITH_IMGUI
#  include "aver/anim/ControlRig.hpp"
#  include "imgui.h"
#endif

namespace aver::editor {

using fmt::OcRigData;
using fmt::OcRigInput;
using fmt::OcRigInputType;
using fmt::OcRigOp;
using fmt::OcRigOpKind;
using fmt::OcRigRef;

namespace {

namespace fs = std::filesystem;

bool inRange(usize n, i32 i) { return i >= 0 && static_cast<usize>(i) < n; }

i32 inputIndex(const OcRigData& r, const std::string& name) {
    for (usize i = 0; i < r.inputs.size(); ++i)
        if (r.inputs[i].name == name) return static_cast<i32>(i);
    return -1;
}

i32 boneIndex(const fmt::OcSkeleton& s, const std::string& name) {
    if (name.empty()) return -1;
    for (usize i = 0; i < s.bones.size(); ++i)
        if (s.bones[i].name == name) return static_cast<i32>(i);
    return -1;
}

bool iequals(const std::string& a, const char* b) {
    const usize n = std::char_traits<char>::length(b);
    if (a.size() != n) return false;
    for (usize i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

std::string contentRootOf(const std::string& assetPath) {
    const fs::path p(assetPath);
    for (fs::path d = p.parent_path(); !d.empty(); d = d.parent_path()) {
        if (iequals(d.filename().string(), "Content")) return d.string();
        if (d == d.parent_path()) break;
    }
    return p.parent_path().string();
}

// Re-points every reference to `from` (an input) at `to`; an empty `to` turns them back into literals.
void repairRefs(OcRigData& rig, const std::string& from, const std::string& to) {
    for (OcRigOp& op : rig.ops) {
        for (OcRigRef* r : {&op.targetRef, &op.hintRef, &op.rotRef}) {
            if (r->kind != OcRigRef::Kind::Input || r->name != from) continue;
            if (to.empty()) *r = OcRigRef{};
            else r->name = to;
        }
        if (op.weightInput == from) op.weightInput = to;
    }
}

} // namespace

// ------------------------------------------------------------------------------- edits ----------

OcRigData rigStarter(const std::string& name) {
    OcRigData r;
    r.name = name.empty() ? std::string("NewRig") : name;
    return r;
}

i32 rigAddOp(OcRigData& rig, OcRigOpKind kind, const std::vector<std::string>& hints) {
    auto hint = [&](usize i, const char* fallback) { return i < hints.size() && !hints[i].empty() ? hints[i] : std::string(fallback); };
    OcRigOp op;
    op.kind = kind;
    switch (kind) {
        case OcRigOpKind::TwoBoneIk:
            op.root = hint(0, "root"); op.mid = hint(1, "mid"); op.tip = hint(2, "tip");
            op.target = Vec3{50, 0, 50};
            op.hint = Vec3{0, -100, 0};
            break;
        case OcRigOpKind::AimAt:
            op.root = hint(0, "bone");
            op.target = Vec3{0, 0, 200};
            op.hint = Vec3{0, 0, 1};
            break;
        case OcRigOpKind::Fabrik:
        case OcRigOpKind::Ccd:
            op.root = hint(0, "root"); op.tip = hint(1, "tip");
            op.target = Vec3{50, 0, 50};
            op.iterations = kind == OcRigOpKind::Ccd ? 12u : 10u;
            op.tolerance = kind == OcRigOpKind::Ccd ? 0.5f : 0.1f;
            op.maxStepDeg = 30.0f;
            break;
        case OcRigOpKind::CopyBone:
            op.dst = hint(0, "dst"); op.root = hint(1, "src");
            break;
        case OcRigOpKind::SetBone:
            op.root = hint(0, "bone");
            op.copyPos = true;
            op.copyRot = false;
            break;
    }
    rig.ops.push_back(std::move(op));
    return static_cast<i32>(rig.ops.size() - 1);
}

bool rigRemoveOp(OcRigData& rig, i32 index) {
    if (!inRange(rig.ops.size(), index)) return false;
    rig.ops.erase(rig.ops.begin() + index);
    return true;
}

i32 rigMoveOp(OcRigData& rig, i32 index, i32 delta) {
    if (!inRange(rig.ops.size(), index)) return -1;
    const i32 to = std::clamp(index + delta, 0, static_cast<i32>(rig.ops.size()) - 1);
    if (to == index) return index;
    OcRigOp op = std::move(rig.ops[static_cast<usize>(index)]);
    rig.ops.erase(rig.ops.begin() + index);
    rig.ops.insert(rig.ops.begin() + to, std::move(op));
    return to;
}

bool rigSetValueRef(OcRigData& rig, i32 opIndex, RigSlot slot, OcRigRef::Kind kind, const std::string& name, const Vec3& offset) {
    if (!inRange(rig.ops.size(), opIndex)) return false;
    OcRigOp& op = rig.ops[static_cast<usize>(opIndex)];
    using K = OcRigRef::Kind;
    if (slot == RigSlot::Weight) {
        if (kind == K::Literal) { op.weightInput.clear(); return true; }
        if (kind != K::Input) return false;
        const i32 at = inputIndex(rig, name);
        if (at < 0 || rig.inputs[static_cast<usize>(at)].type != OcRigInputType::Float) return false;
        op.weightInput = name;
        return true;
    }
    OcRigRef& ref = slot == RigSlot::Target ? op.targetRef : slot == RigSlot::Hint ? op.hintRef : op.rotRef;
    if (kind == K::Literal) { ref = OcRigRef{}; return true; }
    if (kind == K::Input) {
        const i32 at = inputIndex(rig, name);
        const OcRigInputType need = slot == RigSlot::Rotation ? OcRigInputType::Quat : OcRigInputType::Vec3;
        if (at < 0 || rig.inputs[static_cast<usize>(at)].type != need) return false;
        ref.kind = K::Input;
        ref.name = name;
        ref.offset = Vec3{0, 0, 0};
        return true;
    }
    if (slot == RigSlot::Rotation || name.empty()) return false;
    ref.kind = K::Bone;
    ref.name = name;
    ref.offset = offset;
    return true;
}

i32 rigAddInput(OcRigData& rig, const std::string& name, OcRigInputType type) {
    std::string n = name.empty() ? std::string("Input") : name;
    if (inputIndex(rig, n) >= 0) {
        const std::string base = n;
        for (int k = 2; inputIndex(rig, n) >= 0; ++k) n = base + std::to_string(k);
    }
    OcRigInput in;
    in.name = n;
    in.type = type;
    rig.inputs.push_back(std::move(in));
    return static_cast<i32>(rig.inputs.size() - 1);
}

bool rigRemoveInput(OcRigData& rig, i32 index) {
    if (!inRange(rig.inputs.size(), index)) return false;
    const std::string name = rig.inputs[static_cast<usize>(index)].name;
    rig.inputs.erase(rig.inputs.begin() + index);
    repairRefs(rig, name, {});
    return true;
}

bool rigRenameInput(OcRigData& rig, i32 index, const std::string& name) {
    if (!inRange(rig.inputs.size(), index) || name.empty()) return false;
    OcRigInput& in = rig.inputs[static_cast<usize>(index)];
    if (in.name == name) return true;
    if (inputIndex(rig, name) >= 0) return false;
    for (const char c : name)
        if (std::isspace(static_cast<unsigned char>(c)) || c == '=' || c == '#') return false;     // the text format splits on these
    const std::string old = in.name;
    in.name = name;
    repairRefs(rig, old, name);
    return true;
}

std::vector<std::string> rigProblems(const OcRigData& rig, const fmt::OcSkeleton* skel) {
    std::vector<std::string> out;
    std::string why;
    if (!rig.valid(&why)) out.push_back("rig: " + why);
    std::set<std::string> used;
    for (usize i = 0; i < rig.ops.size(); ++i) {
        const OcRigOp& op = rig.ops[i];
        const std::string what = "op " + std::to_string(i + 1);
        for (const OcRigRef* r : {&op.targetRef, &op.hintRef, &op.rotRef})
            if (r->kind == OcRigRef::Kind::Input) used.insert(r->name);
        if (!op.weightInput.empty()) used.insert(op.weightInput);
        if (!skel) continue;
        auto need = [&](const std::string& bone, const char* role) {
            if (bone.empty() || boneIndex(*skel, bone) >= 0) return true;
            out.push_back(what + ": " + role + " bone '" + bone + "' is not in the skeleton");
            return false;
        };
        const bool okRoot = need(op.root, "root");
        bool okMid = true, okTip = true;
        if (op.kind == OcRigOpKind::TwoBoneIk) okMid = need(op.mid, "mid");
        if (op.kind == OcRigOpKind::TwoBoneIk || op.kind == OcRigOpKind::Fabrik || op.kind == OcRigOpKind::Ccd) okTip = need(op.tip, "tip");
        if (op.kind == OcRigOpKind::CopyBone) need(op.dst, "dst");
        for (const OcRigRef* r : {&op.targetRef, &op.hintRef})
            if (r->kind == OcRigRef::Kind::Bone) need(r->name, "referenced");
        if (okRoot && okTip && (op.kind == OcRigOpKind::Fabrik || op.kind == OcRigOpKind::Ccd)) {
            anim::IkChain chain;
            if (!anim::buildChain(*skel, static_cast<u32>(boneIndex(*skel, op.root)), static_cast<u32>(boneIndex(*skel, op.tip)), chain))
                out.push_back(what + ": '" + op.root + "' is not an ancestor of '" + op.tip + "'");
        }
        if (okRoot && okMid && okTip && op.kind == OcRigOpKind::TwoBoneIk) {
            const i32 r = boneIndex(*skel, op.root), m = boneIndex(*skel, op.mid), t = boneIndex(*skel, op.tip);
            if (skel->bones[static_cast<usize>(m)].parent != r || skel->bones[static_cast<usize>(t)].parent != m)
                out.push_back(what + ": root, mid and tip are not a parent-linked chain");
        }
    }
    for (const OcRigInput& in : rig.inputs)
        if (!used.count(in.name)) out.push_back("input '" + in.name + "' is not read by any op");
    return out;
}

// ------------------------------------------------------------------------------- the tab ----------

RigEditor::RigEditor(std::string path) : path_(std::move(path)) { loadFromDisk(); }

void RigEditor::loadFromDisk() {
    std::string why;
    OcRigData r;
    if (!fmt::loadOcRig(path_, r, &why)) {
        loaded_ = false;
        loadError_ = why;
        return;
    }
    rig_ = std::move(r);
    loaded_ = true;
    loadError_.clear();
    dirty_ = false;
    selOp_ = -1;
    history_.clear();
}

std::string RigEditor::title() const { return fs::path(path_).filename().string() + "###rig:" + path_; }

bool RigEditor::save(std::string* why) {
    if (!loaded_) {
        if (why) *why = "cannot save: file failed to load (" + loadError_ + ")";
        return false;
    }
    if (!fmt::saveOcRig(path_, rig_, why)) return false;
    dirty_ = false;
    return true;
}

void RigEditor::onFileChanged() {
    if (dirty_) {
        AVER_WARN("[RigEditor] '{}' changed on disk, but this tab has unsaved edits -- keeping them", path_);
        return;
    }
    loadFromDisk();
}

void RigEditor::undo() {
    if (!history_.undo(rig_)) return;
    dirty_ = true;
    if (!inRange(rig_.ops.size(), selOp_)) selOp_ = -1;
}

void RigEditor::redo() {
    if (!history_.redo(rig_)) return;
    dirty_ = true;
    if (!inRange(rig_.ops.size(), selOp_)) selOp_ = -1;
}

bool RigEditor::setPreviewSkeleton(const std::string& path) {
    skelOk_ = false;
    skel_ = fmt::OcSkeleton{};
    skelPath_ = path;
    std::string why;
    skelOk_ = !path.empty() && fmt::loadOcSkel(path, skel_, &why);
    return skelOk_;
}

i32 RigEditor::addOp(OcRigOpKind kind) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 idx = rigAddOp(rig_, kind);
    selOp_ = idx;
    markDirty();
    return idx;
}

bool RigEditor::removeOp(i32 index) {
    if (!loaded_) return false;
    pushUndo();
    if (!rigRemoveOp(rig_, index)) { history_.cancelPush(); return false; }
    selOp_ = -1;
    markDirty();
    return true;
}

i32 RigEditor::moveOp(i32 index, i32 delta) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 to = rigMoveOp(rig_, index, delta);
    if (to < 0 || to == index) { history_.cancelPush(); return to; }
    selOp_ = to;
    markDirty();
    return to;
}

bool RigEditor::setValueRef(i32 op, RigSlot slot, OcRigRef::Kind kind, const std::string& name, const Vec3& offset) {
    if (!loaded_) return false;
    pushUndo();
    if (!rigSetValueRef(rig_, op, slot, kind, name, offset)) { history_.cancelPush(); return false; }
    markDirty();
    return true;
}

i32 RigEditor::addInput(const std::string& name, OcRigInputType type) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 idx = rigAddInput(rig_, name, type);
    markDirty();
    return idx;
}

bool RigEditor::removeInput(i32 index) {
    if (!loaded_) return false;
    pushUndo();
    if (!rigRemoveInput(rig_, index)) { history_.cancelPush(); return false; }
    markDirty();
    return true;
}

bool RigEditor::renameInput(i32 index, const std::string& name) {
    if (!loaded_) return false;
    pushUndo();
    if (!rigRenameInput(rig_, index, name)) { history_.cancelPush(); return false; }
    markDirty();
    return true;
}

#if AVER_WITH_IMGUI

namespace {

const char* kindName(OcRigOpKind k) {
    switch (k) {
        case OcRigOpKind::TwoBoneIk: return "twobone";
        case OcRigOpKind::AimAt: return "aim";
        case OcRigOpKind::Fabrik: return "fabrik";
        case OcRigOpKind::Ccd: return "ccd";
        case OcRigOpKind::CopyBone: return "copybone";
        case OcRigOpKind::SetBone: return "setbone";
    }
    return "";
}

struct InputValues {
    struct Slot { u64 hash; f32 v[4]; };
    std::vector<Slot> slots;
};

bool inputLookup(u64 hash, f32* out4, void* user) {
    const auto* iv = static_cast<const InputValues*>(user);
    for (const InputValues::Slot& s : iv->slots)
        if (s.hash == hash) { for (int i = 0; i < 4; ++i) out4[i] = s.v[i]; return true; }
    return false;
}

} // namespace

void RigEditor::touch(bool changed) {
    if (changed) {
        if (!gesture_) { history_.push(before_); gesture_ = true; }
        markDirty();
    }
    if (ImGui::IsItemDeactivated()) endGesture_ = true;
}

bool RigEditor::boneCombo(const char* label, std::string& bone) {
    if (!skelOk_) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "%s", bone.c_str());
        const bool c = ImGui::InputText(label, buf, sizeof buf);
        if (c) { bone = buf; }
        touch(c);
        return c;
    }
    bool changed = false;
    if (ImGui::BeginCombo(label, bone.empty() ? "<none>" : bone.c_str())) {
        for (const fmt::OcBone& b : skel_.bones)
            if (ImGui::Selectable(b.name.c_str(), b.name == bone)) { pushUndo(); bone = b.name; changed = true; }
        ImGui::EndCombo();
    }
    if (changed) markDirty();
    return changed;
}

void RigEditor::scanSkeletons() {
    skelScanned_ = true;
    skelChoices_.clear();
    std::error_code ec;
    usize seen = 0;
    for (auto it = fs::recursive_directory_iterator(contentRootOf(path_), fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator() && seen < 4000; it.increment(ec), ++seen) {
        if (it->is_regular_file(ec) && it->path().extension() == ".ocskel") skelChoices_.push_back(it->path().string());
    }
    std::sort(skelChoices_.begin(), skelChoices_.end());
}

void RigEditor::valueRow(i32 index, OcRigOp& op, RigSlot slot, const char* label, Vec3& literal, bool isVec) {
    ImGui::PushID(label);
    OcRigRef& ref = slot == RigSlot::Target ? op.targetRef : slot == RigSlot::Hint ? op.hintRef : op.rotRef;
    int src = ref.kind == OcRigRef::Kind::Literal ? 0 : ref.kind == OcRigRef::Kind::Input ? 1 : 2;
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::Combo("##src", &src, isVec ? "literal\0@input\0bone:\0" : "literal\0@input\0")) {
        if (src == 0) setValueRef(index, slot, OcRigRef::Kind::Literal);
        else if (src == 1) {
            // The first input of the right type, or a new one.
            const OcRigInputType need = slot == RigSlot::Rotation ? OcRigInputType::Quat : OcRigInputType::Vec3;
            std::string pick;
            for (const OcRigInput& in : rig_.inputs) if (in.type == need && pick.empty()) pick = in.name;
            if (pick.empty()) { addInput(slot == RigSlot::Rotation ? "Rotation" : "Target", need); pick = rig_.inputs.back().name; }
            setValueRef(index, slot, OcRigRef::Kind::Input, pick);
        } else {
            setValueRef(index, slot, OcRigRef::Kind::Bone, skelOk_ && !skel_.bones.empty() ? skel_.bones[0].name : std::string("pelvis"));
        }
    }
    ImGui::SameLine();
    if (ref.kind == OcRigRef::Kind::Literal) {
        if (isVec) {
            float v[3] = {literal.x, literal.y, literal.z};
            ImGui::SetNextItemWidth(200.0f);
            const bool c = ImGui::DragFloat3(label, v, 0.5f);
            if (c) literal = Vec3{v[0], v[1], v[2]};
            touch(c);
        } else {
            float v[4] = {op.rotation.x, op.rotation.y, op.rotation.z, op.rotation.w};
            ImGui::SetNextItemWidth(240.0f);
            const bool c = ImGui::DragFloat4(label, v, 0.01f);
            if (c) op.rotation = Quat{v[0], v[1], v[2], v[3]}.normalized();
            touch(c);
        }
    } else if (ref.kind == OcRigRef::Kind::Input) {
        const OcRigInputType need = slot == RigSlot::Rotation ? OcRigInputType::Quat : OcRigInputType::Vec3;
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo(label, ("@" + ref.name).c_str())) {
            for (const OcRigInput& in : rig_.inputs) {
                if (in.type != need) continue;
                if (ImGui::Selectable(("@" + in.name).c_str(), in.name == ref.name)) setValueRef(index, slot, OcRigRef::Kind::Input, in.name);
            }
            ImGui::EndCombo();
        }
    } else {
        std::string bone = ref.name;
        ImGui::SetNextItemWidth(160.0f);
        if (boneCombo(label, bone)) { ref.name = bone; }
        float o[3] = {ref.offset.x, ref.offset.y, ref.offset.z};
        ImGui::SetNextItemWidth(200.0f);
        const bool c = ImGui::DragFloat3("offset", o, 0.5f);
        if (c) ref.offset = Vec3{o[0], o[1], o[2]};
        touch(c);
    }
    ImGui::PopID();
}

void RigEditor::drawOpForm(OcRigOp& op, i32 index) {
    ImGui::PushID(index);
    switch (op.kind) {
        case OcRigOpKind::TwoBoneIk:
            boneCombo("root", op.root); boneCombo("mid", op.mid); boneCombo("tip", op.tip);
            valueRow(index, op, RigSlot::Target, "goal", op.target, true);
            valueRow(index, op, RigSlot::Hint, "pole", op.hint, true);
            break;
        case OcRigOpKind::AimAt:
            boneCombo("bone", op.root);
            valueRow(index, op, RigSlot::Target, "look at", op.target, true);
            valueRow(index, op, RigSlot::Hint, "axis", op.hint, true);
            break;
        case OcRigOpKind::Fabrik:
        case OcRigOpKind::Ccd: {
            boneCombo("root", op.root); boneCombo("tip", op.tip);
            valueRow(index, op, RigSlot::Target, "goal", op.target, true);
            bool hp = op.hasPole;
            if (ImGui::Checkbox("pole", &hp)) { pushUndo(); op.hasPole = hp; markDirty(); }
            if (op.hasPole) valueRow(index, op, RigSlot::Hint, "pole at", op.hint, true);
            int it = static_cast<int>(op.iterations);
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::DragInt("iterations", &it, 0.2f, 1, 64)) { if (!gesture_) { history_.push(before_); gesture_ = true; } op.iterations = static_cast<u32>(it); markDirty(); }
            if (ImGui::IsItemDeactivated()) endGesture_ = true;
            ImGui::SetNextItemWidth(80.0f);
            touch(ImGui::DragFloat("tolerance cm", &op.tolerance, 0.01f, 0.0f, 10.0f));
            if (op.kind == OcRigOpKind::Ccd) {
                ImGui::SetNextItemWidth(80.0f);
                touch(ImGui::DragFloat("max step deg", &op.maxStepDeg, 0.5f, 1.0f, 180.0f));
            }
            bool lim = op.limits;
            if (ImGui::Checkbox("limits", &lim)) { pushUndo(); op.limits = lim; markDirty(); }
            break;
        }
        case OcRigOpKind::CopyBone: {
            boneCombo("dst", op.dst); boneCombo("src", op.root);
            int sp = op.modelSpace ? 0 : 1;
            if (ImGui::Combo("space", &sp, "model\0local\0")) { pushUndo(); op.modelSpace = sp == 0; markDirty(); }
            bool p = op.copyPos, r = op.copyRot;
            if (ImGui::Checkbox("position", &p)) { pushUndo(); op.copyPos = p; markDirty(); }
            ImGui::SameLine();
            if (ImGui::Checkbox("rotation", &r)) { pushUndo(); op.copyRot = r; markDirty(); }
            break;
        }
        case OcRigOpKind::SetBone: {
            boneCombo("bone", op.root);
            int sp = op.modelSpace ? 0 : 1;
            if (ImGui::Combo("space", &sp, "model\0local\0")) { pushUndo(); op.modelSpace = sp == 0; markDirty(); }
            bool p = op.copyPos;
            if (ImGui::Checkbox("set position", &p)) { pushUndo(); op.copyPos = p; markDirty(); }
            if (op.copyPos) valueRow(index, op, RigSlot::Target, "position", op.target, true);
            bool r = op.copyRot;
            if (ImGui::Checkbox("set rotation", &r)) { pushUndo(); op.copyRot = r; markDirty(); }
            if (op.copyRot) { Vec3 unused; valueRow(index, op, RigSlot::Rotation, "rotation", unused, false); }
            break;
        }
    }
    if (op.kind == OcRigOpKind::TwoBoneIk || op.kind == OcRigOpKind::Fabrik || op.kind == OcRigOpKind::Ccd) {
        bool tr = op.hasTipRot;
        if (ImGui::Checkbox("tip rotation", &tr)) { pushUndo(); op.hasTipRot = tr; markDirty(); }
        if (op.hasTipRot) { Vec3 unused; valueRow(index, op, RigSlot::Rotation, "tip rot", unused, false); }
    }
    // Weight: a slider, or an input.
    {
        bool fromInput = !op.weightInput.empty();
        if (ImGui::Checkbox("weight from input", &fromInput)) {
            if (fromInput) {
                std::string pick;
                for (const OcRigInput& in : rig_.inputs) if (in.type == OcRigInputType::Float && pick.empty()) pick = in.name;
                if (pick.empty()) { addInput("Weight", OcRigInputType::Float); pick = rig_.inputs.back().name; }
                setValueRef(index, RigSlot::Weight, OcRigRef::Kind::Input, pick);
            } else {
                setValueRef(index, RigSlot::Weight, OcRigRef::Kind::Literal);
            }
        }
        ImGui::SameLine();
        if (fromInput) {
            if (ImGui::BeginCombo("##winput", ("@" + op.weightInput).c_str())) {
                for (const OcRigInput& in : rig_.inputs)
                    if (in.type == OcRigInputType::Float && ImGui::Selectable(("@" + in.name).c_str(), in.name == op.weightInput))
                        setValueRef(index, RigSlot::Weight, OcRigRef::Kind::Input, in.name);
                ImGui::EndCombo();
            }
        } else {
            ImGui::SetNextItemWidth(160.0f);
            touch(ImGui::SliderFloat("##weight", &op.weight, 0.0f, 1.0f));
        }
    }
    ImGui::PopID();
}

void RigEditor::drawOps() {
    const char* names[] = {"twobone", "aim", "fabrik", "ccd", "copybone", "setbone"};
    const OcRigOpKind kinds[] = {OcRigOpKind::TwoBoneIk, OcRigOpKind::AimAt, OcRigOpKind::Fabrik, OcRigOpKind::Ccd, OcRigOpKind::CopyBone, OcRigOpKind::SetBone};
    if (ImGui::BeginCombo("Add op", "kind...")) {
        for (int i = 0; i < 6; ++i) if (ImGui::Selectable(names[i])) addOp(kinds[i]);
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!inRange(rig_.ops.size(), selOp_));
    if (ImGui::Button("Delete")) removeOp(selOp_);
    ImGui::SameLine();
    if (ImGui::Button("Up")) moveOp(selOp_, -1);
    ImGui::SameLine();
    if (ImGui::Button("Down")) moveOp(selOp_, 1);
    ImGui::EndDisabled();
    ImGui::Separator();
    for (usize i = 0; i < rig_.ops.size(); ++i) {
        OcRigOp& op = rig_.ops[i];
        ImGui::PushID(static_cast<int>(i));
        const bool sel = selOp_ == static_cast<i32>(i);
        std::string label = std::to_string(i + 1) + "  " + kindName(op.kind) + "  " + op.root;
        if (op.kind == OcRigOpKind::CopyBone) label = std::to_string(i + 1) + "  copybone  " + op.dst + " <- " + op.root;
        if (ImGui::Selectable(label.c_str(), sel)) selOp_ = static_cast<i32>(i);
        if (sel) {
            ImGui::Indent(10.0f);
            drawOpForm(op, static_cast<i32>(i));
            ImGui::Unindent(10.0f);
        }
        ImGui::PopID();
    }
}

void RigEditor::drawInputs() {
    if (ImGui::Button("Add vec3")) addInput("Target", OcRigInputType::Vec3);
    ImGui::SameLine();
    if (ImGui::Button("Add float")) addInput("Amount", OcRigInputType::Float);
    ImGui::SameLine();
    if (ImGui::Button("Add quat")) addInput("Rotation", OcRigInputType::Quat);
    ImGui::TextDisabled("A script sets these with SetRigInput; the default applies until it does.");
    int removeAt = -1;
    char buf[128];
    for (usize i = 0; i < rig_.inputs.size(); ++i) {
        OcRigInput& in = rig_.inputs[i];
        ImGui::PushID(static_cast<int>(i));
        std::snprintf(buf, sizeof buf, "%s", in.name.c_str());
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::InputText("##name", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) renameInput(static_cast<i32>(i), buf);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", in.type == OcRigInputType::Float ? "float" : in.type == OcRigInputType::Vec3 ? "vec3" : "quat");
        ImGui::SameLine();
        if (in.type == OcRigInputType::Float) {
            ImGui::SetNextItemWidth(100.0f);
            touch(ImGui::DragFloat("default", &in.defFloat, 0.01f));
        } else if (in.type == OcRigInputType::Vec3) {
            float v[3] = {in.defVec.x, in.defVec.y, in.defVec.z};
            ImGui::SetNextItemWidth(200.0f);
            const bool c = ImGui::DragFloat3("default", v, 0.5f);
            if (c) in.defVec = Vec3{v[0], v[1], v[2]};
            touch(c);
        } else {
            float v[4] = {in.defQuat.x, in.defQuat.y, in.defQuat.z, in.defQuat.w};
            ImGui::SetNextItemWidth(240.0f);
            const bool c = ImGui::DragFloat4("default", v, 0.01f);
            if (c) in.defQuat = Quat{v[0], v[1], v[2], v[3]}.normalized();
            touch(c);
        }
        if (in.type != OcRigInputType::Float) {
            ImGui::SameLine();
            bool w = in.world;
            if (ImGui::Checkbox("world", &w)) { pushUndo(); in.world = w; markDirty(); }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeAt = static_cast<int>(i);
        ImGui::PopID();
    }
    if (removeAt >= 0) removeInput(removeAt);
}

void RigEditor::drawProblemsTab() {
    const std::vector<std::string> p = problems();
    if (p.empty()) ImGui::TextDisabled("No problems.");
    for (const std::string& s : p) ImGui::BulletText("%s", s.c_str());
    ImGui::Separator();
    ImGui::TextDisabled(rig_.needsV2() ? "Saves as OCRIG 2 (inputs, references or new ops are in use)." : "Saves as OCRIG 1.");
}

void RigEditor::drawPreview(float width, float height) {
    if (!skelScanned_) scanSkeletons();
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::BeginCombo("preview skeleton", skelPath_.empty() ? "pick one..." : fs::path(skelPath_).filename().string().c_str())) {
        for (const std::string& s : skelChoices_)
            if (ImGui::Selectable(fs::path(s).filename().string().c_str(), s == skelPath_)) { setPreviewSkeleton(s); framed_ = false; }
        ImGui::EndCombo();
    }
    if (!skelOk_) { ImGui::TextDisabled("Pick a skeleton to see the ops work on it."); return; }

    // Test values for the inputs: vec3 inputs are handles in the view, floats are sliders.
    InputValues iv;
    std::vector<RigHandle> handles;
    std::vector<std::string> handleInput;
    anim::Pose pose;
    anim::restPose(skel_, pose);
    std::vector<Mat4> model;
    anim::poseToModel(skel_, pose, model);
    for (const OcRigInput& in : rig_.inputs) {
        InputValues::Slot s{fnv1a64(std::string_view(in.name)), {0, 0, 0, 0}};
        if (in.type == OcRigInputType::Vec3) {
            if (!testVec_.count(in.name)) testVec_[in.name] = in.defVec;
            const Vec3 p = testVec_[in.name];
            s.v[0] = p.x; s.v[1] = p.y; s.v[2] = p.z;
            RigHandle h;
            h.pos = p;
            h.label = in.name.c_str();
            handles.push_back(h);
            handleInput.push_back(in.name);
        } else if (in.type == OcRigInputType::Float) {
            if (!testFloat_.count(in.name)) testFloat_[in.name] = in.defFloat;
            ImGui::SetNextItemWidth(140.0f);
            ImGui::SliderFloat(in.name.c_str(), &testFloat_[in.name], 0.0f, 1.0f);
            s.v[0] = testFloat_[in.name];
        } else {
            s.v[0] = in.defQuat.x; s.v[1] = in.defQuat.y; s.v[2] = in.defQuat.z; s.v[3] = in.defQuat.w;
        }
        iv.slots.push_back(s);
    }
    anim::RigEval ev;
    ev.rig = &rig_;
    ev.input = &inputLookup;
    ev.inputUser = &iv;
    for (const OcRigOp& op : rig_.ops) anim::applyRigOp(skel_, pose, op, 1.0f, ev);

    RigPreviewIn pv;
    pv.skel = &skel_;
    pv.pose = &pose;
    pv.boneColors.assign(skel_.bones.size(), 0u);
    if (inRange(rig_.ops.size(), selOp_)) {
        const OcRigOp& op = rig_.ops[static_cast<usize>(selOp_)];
        anim::IkChain chain;
        const i32 r = boneIndex(skel_, op.root);
        i32 t = boneIndex(skel_, op.tip);
        if (op.kind == OcRigOpKind::TwoBoneIk) t = boneIndex(skel_, op.tip);
        if (r >= 0 && t >= 0 && anim::buildChain(skel_, static_cast<u32>(r), static_cast<u32>(t), chain))
            for (const u32 b : chain.bones) pv.boneColors[b] = IM_COL32(255, 180, 90, 255);
        else if (r >= 0) pv.boneColors[static_cast<usize>(r)] = IM_COL32(255, 180, 90, 255);
    }
    pv.handles = handles;
    pv.selectedBone = selBone_;
    pv.caption = "ops applied to the rest pose";
    if (!framed_) { rigFramePose(view_, model); framed_ = true; }
    const RigPreviewOut out = drawRigPreview("##rigview", width, height, view_, pv);
    if (out.draggingHandle >= 0) testVec_[handleInput[static_cast<usize>(out.draggingHandle)]] = out.handlePos;
    if (out.clickedBone >= 0) selBone_ = out.clickedBone;
}

void RigEditor::draw(Engine& e) {
    (void)e;
    if (!loaded_) {
        ImGui::TextWrapped("This file could not be read: %s", loadError_.c_str());
        return;
    }
    before_ = rig_;
    if (assetSaveButton()) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[RigEditor] save failed for '{}': {}", path_, why);
    }
    assetUndoRedoShortcuts(canUndo(), canRedo(), [&] { undo(); }, [&] { redo(); });
    ImGui::SameLine();
    ImGui::BeginDisabled(!canUndo());
    if (ImGui::Button("Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canRedo());
    if (ImGui::Button("Redo")) redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    char nm[128];
    std::snprintf(nm, sizeof nm, "%s", rig_.name.c_str());
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::InputText("##name", nm, sizeof nm)) { rig_.name = nm; markDirty(); }
    touch(ImGui::IsItemEdited());
    ImGui::Separator();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const f32 leftW = std::max(340.0f, avail.x * 0.45f);
    if (ImGui::BeginChild("##left", ImVec2(leftW, avail.y), true)) {
        if (ImGui::BeginTabBar("##rigtabs")) {
            if (ImGui::BeginTabItem("Ops")) { drawOps(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Inputs")) { drawInputs(); ImGui::EndTabItem(); }
            const std::string pt = "Problems (" + std::to_string(problems().size()) + ")###problems";
            if (ImGui::BeginTabItem(pt.c_str())) { drawProblemsTab(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##right", ImVec2(0, avail.y), true)) {
        const ImVec2 room = ImGui::GetContentRegionAvail();
        drawPreview(room.x, std::max(160.0f, room.y - 60.0f));
    }
    ImGui::EndChild();
    if (endGesture_ && !ImGui::IsMouseDown(0)) { gesture_ = false; endGesture_ = false; }
}

#else   // AVER_WITH_IMGUI

void RigEditor::draw(Engine& e) { (void)e; }

#endif  // AVER_WITH_IMGUI

std::unique_ptr<AssetEditor> makeRigEditor(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".ocrig") return nullptr;
    return std::make_unique<RigEditor>(path);
}

} // namespace aver::editor
