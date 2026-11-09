// The .ocikrig editor tab. See the header for the SandboxApp hook and for why the edits are free functions.
#include "IkRigEditor.hpp"
#include "EditorChrome.hpp"
#include "EditorKeybinds.hpp"

#include "aver/anim/AnimSampler.hpp"
#include "aver/anim/RetargetAsset.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"

#if AVER_MODULE_RAGDOLL
#  include "aver/ragdoll/RagdollBuilder.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>

#if AVER_WITH_IMGUI
#  include "imgui.h"
#endif

namespace aver::editor {

using anim::IkRigAsset;
using anim::IkRigChainDef;
using anim::IkRigFootDef;
using anim::IkRigLimitDef;
using anim::IkSolverKind;

namespace {

namespace fs = std::filesystem;

bool inRange(usize n, i32 i) { return i >= 0 && static_cast<usize>(i) < n; }

i32 chainIndex(const IkRigAsset& a, const std::string& name) {
    for (usize i = 0; i < a.chains.size(); ++i)
        if (a.chains[i].name == name) return static_cast<i32>(i);
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

bool isFile(const fs::path& p) {
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(p, ec);
}

} // namespace

// ------------------------------------------------------------------------------- edits ----------

IkRigAsset ikrigStarter(const std::string& skeletonRef) {
    IkRigAsset a;
    a.name = fs::path(skeletonRef).stem().string() + "Ik";
    a.skeleton = skeletonRef;
    return a;
}

IkRigAsset ikrigSuggest(const fmt::OcSkeleton& skel, const std::string& skeletonRef) {
    IkRigAsset a = ikrigStarter(skeletonRef);
    const anim::AutoMapResult m = anim::autoMapChains(skel, skel);
    a.body.bone = m.asset.root.srcBone;
    a.body.goal = a.body.bone.empty() ? std::string() : "Hips";
    std::map<std::string, std::string> toes;
    for (const anim::RetargetChainDef& c : m.asset.chains)
        if (c.name.size() > 3 && c.name.compare(c.name.size() - 3, 3, "Toe") == 0) toes[c.name.substr(0, c.name.size() - 3)] = c.srcStart;
    for (const anim::RetargetChainDef& rc : m.asset.chains) {
        const bool leg = rc.name.find("Leg") != std::string::npos;
        const bool arm = rc.name.find("Arm") != std::string::npos;
        const bool spine = rc.name == "Spine";
        if (!leg && !arm && !spine) continue;
        anim::IkChain chain;
        if (!anim::buildChain(skel, static_cast<u32>(boneIndex(skel, rc.srcStart)), static_cast<u32>(boneIndex(skel, rc.srcEnd)), chain)) continue;
        IkRigChainDef c;
        c.name = rc.name;
        c.start = rc.srcStart;
        c.end = rc.srcEnd;
        const std::string side = rc.name.compare(0, 4, "Left") == 0 ? "Left" : rc.name.compare(0, 5, "Right") == 0 ? "Right" : "";
        if (spine) {
            c.solver = IkSolverKind::Ccd;
            c.priority = 0;
            c.goal = "Chest";
            c.iterations = 12;
            c.toleranceCm = 0.5f;
            c.limits = true;
        } else {
            c.solver = chain.bones.size() == 3 ? IkSolverKind::TwoBone : IkSolverKind::Fabrik;
            c.priority = arm ? 1 : 2;
            c.goal = side + (arm ? "Hand" : "Foot");
            c.tipRotation = arm;
        }
        a.chains.push_back(c);
        if (leg) {
            IkRigFootDef f;
            f.chain = c.name;
            const auto toe = toes.find(side);
            if (toe != toes.end()) f.toe = toe->second;
            a.feet.push_back(f);
        }
    }
    return a;
}

std::string ikrigUniqueChainName(const IkRigAsset& a, const std::string& base) {
    const std::string b = base.empty() ? std::string("Chain") : base;
    if (chainIndex(a, b) < 0) return b;
    for (int n = 2;; ++n) {
        const std::string cand = b + " " + std::to_string(n);
        if (chainIndex(a, cand) < 0) return cand;
    }
}

i32 ikrigAddChain(IkRigAsset& a, const std::string& name, const std::string& start, const std::string& end, IkSolverKind solver) {
    if (start.empty() || end.empty()) return -1;
    IkRigChainDef c;
    c.name = ikrigUniqueChainName(a, name);
    c.start = start;
    c.end = end;
    c.solver = solver;
    c.iterations = solver == IkSolverKind::Ccd ? 12u : 10u;
    c.toleranceCm = solver == IkSolverKind::Ccd ? 0.5f : 0.1f;
    i32 worst = -1;
    for (const IkRigChainDef& o : a.chains) worst = std::max(worst, o.priority);
    c.priority = worst + 1;
    a.chains.push_back(std::move(c));
    return static_cast<i32>(a.chains.size() - 1);
}

bool ikrigRemoveChain(IkRigAsset& a, i32 index) {
    if (!inRange(a.chains.size(), index)) return false;
    const std::string name = a.chains[static_cast<usize>(index)].name;
    a.chains.erase(a.chains.begin() + index);
    a.feet.erase(std::remove_if(a.feet.begin(), a.feet.end(), [&](const IkRigFootDef& f) { return f.chain == name; }), a.feet.end());
    return true;
}

i32 ikrigMoveChain(IkRigAsset& a, i32 index, i32 delta) {
    if (!inRange(a.chains.size(), index)) return -1;
    const i32 to = std::clamp(index + delta, 0, static_cast<i32>(a.chains.size()) - 1);
    if (to == index) return index;
    IkRigChainDef c = std::move(a.chains[static_cast<usize>(index)]);
    a.chains.erase(a.chains.begin() + index);
    a.chains.insert(a.chains.begin() + to, std::move(c));
    return to;
}

bool ikrigSetPriority(IkRigAsset& a, i32 index, i32 priority) {
    if (!inRange(a.chains.size(), index) || priority < 0) return false;
    a.chains[static_cast<usize>(index)].priority = priority;
    return true;
}

bool ikrigRenameChain(IkRigAsset& a, i32 index, const std::string& name) {
    if (!inRange(a.chains.size(), index) || name.empty()) return false;
    IkRigChainDef& c = a.chains[static_cast<usize>(index)];
    if (c.name == name) return true;
    if (chainIndex(a, name) >= 0) return false;
    const std::string old = c.name;
    c.name = name;
    for (IkRigFootDef& f : a.feet)
        if (f.chain == old) f.chain = name;
    return true;
}

i32 ikrigAddLimit(IkRigAsset& a, const std::string& bone, f32 swing1, f32 swing2, f32 twist) {
    if (bone.empty()) return -1;
    for (const f32 v : {swing1, swing2, twist})
        if (!std::isfinite(v) || v < 0.0f || v > 180.0f) return -1;
    for (usize i = 0; i < a.limits.size(); ++i) {
        if (a.limits[i].bone != bone) continue;
        a.limits[i].swing1 = swing1; a.limits[i].swing2 = swing2; a.limits[i].twist = twist;
        return static_cast<i32>(i);
    }
    IkRigLimitDef l;
    l.bone = bone; l.swing1 = swing1; l.swing2 = swing2; l.twist = twist;
    a.limits.push_back(std::move(l));
    return static_cast<i32>(a.limits.size() - 1);
}

bool ikrigRemoveLimit(IkRigAsset& a, const std::string& bone) {
    const auto it = std::remove_if(a.limits.begin(), a.limits.end(), [&](const IkRigLimitDef& l) { return l.bone == bone; });
    const bool any = it != a.limits.end();
    a.limits.erase(it, a.limits.end());
    return any;
}

namespace {
// The bones of every chain, tips excluded (a tip's limit is ignored by the solver).
std::vector<u32> limitedBones(const IkRigAsset& a, const fmt::OcSkeleton& skel) {
    std::set<u32> out;
    for (const IkRigChainDef& c : a.chains) {
        anim::IkChain chain;
        const i32 s = boneIndex(skel, c.start), e = boneIndex(skel, c.end);
        if (s < 0 || e < 0 || !anim::buildChain(skel, static_cast<u32>(s), static_cast<u32>(e), chain)) continue;
        for (usize i = 0; i + 1 < chain.bones.size(); ++i) out.insert(chain.bones[i]);
    }
    return std::vector<u32>(out.begin(), out.end());
}
} // namespace

u32 ikrigImportLimits(IkRigAsset& a, const fmt::OcSkeleton& skel, const f32* limitsDeg3PerBone, usize boneCount) {
    if (!limitsDeg3PerBone) return 0;
    u32 n = 0;
    for (const u32 b : limitedBones(a, skel)) {
        if (b >= boneCount) continue;
        const f32* l = limitsDeg3PerBone + static_cast<usize>(b) * 3;
        if (ikrigAddLimit(a, skel.bones[b].name, std::clamp(l[0], 0.0f, 180.0f), std::clamp(l[1], 0.0f, 180.0f), std::clamp(l[2], 0.0f, 180.0f)) >= 0) ++n;
    }
    return n;
}

u32 ikrigResetLimits(IkRigAsset& a, const fmt::OcSkeleton& skel) {
    u32 n = 0;
    for (const u32 b : limitedBones(a, skel))
        if (ikrigAddLimit(a, skel.bones[b].name, 45.0f, 45.0f, 30.0f) >= 0) ++n;
    return n;
}

i32 ikrigAddFoot(IkRigAsset& a, const std::string& chain, const std::string& toe) {
    if (chainIndex(a, chain) < 0) return -1;
    for (const IkRigFootDef& f : a.feet)
        if (f.chain == chain) return -1;
    IkRigFootDef f;
    f.chain = chain;
    f.toe = toe;
    a.feet.push_back(std::move(f));
    return static_cast<i32>(a.feet.size() - 1);
}

bool ikrigRemoveFoot(IkRigAsset& a, i32 index) {
    if (!inRange(a.feet.size(), index)) return false;
    a.feet.erase(a.feet.begin() + index);
    return true;
}

std::vector<std::string> ikrigProblems(const IkRigAsset& a, const fmt::OcSkeleton& skel) {
    std::vector<std::string> out;
    std::string why;
    if (!a.valid(&why)) out.push_back("asset: " + why);
    if (!a.body.bone.empty() && boneIndex(skel, a.body.bone) < 0) out.push_back("body bone '" + a.body.bone + "' is not in the skeleton");
    std::vector<anim::IkChain> built(a.chains.size());
    std::vector<u8> ok(a.chains.size(), 0);
    for (usize i = 0; i < a.chains.size(); ++i) {
        const IkRigChainDef& c = a.chains[i];
        const i32 s = boneIndex(skel, c.start), e = boneIndex(skel, c.end);
        if (s < 0) { out.push_back("chain '" + c.name + "': start bone '" + c.start + "' is not in the skeleton"); continue; }
        if (e < 0) { out.push_back("chain '" + c.name + "': end bone '" + c.end + "' is not in the skeleton"); continue; }
        if (!anim::buildChain(skel, static_cast<u32>(s), static_cast<u32>(e), built[i])) {
            out.push_back("chain '" + c.name + "': '" + c.start + "' is not an ancestor of '" + c.end + "'");
            continue;
        }
        ok[i] = 1;
        if (c.solver == IkSolverKind::TwoBone && built[i].bones.size() != 3)
            out.push_back("chain '" + c.name + "': twobone needs exactly 3 bones (it has " + std::to_string(built[i].bones.size()) + "); FABRIK will be used");
        if (c.pole.mode == anim::IkPoleMode::Bone && boneIndex(skel, c.pole.bone) < 0)
            out.push_back("chain '" + c.name + "': pole bone '" + c.pole.bone + "' is not in the skeleton");
        bool hasFoot = false;
        for (const IkRigFootDef& f : a.feet) hasFoot = hasFoot || f.chain == c.name;
        if (c.goal.empty() && !hasFoot) out.push_back("chain '" + c.name + "' has no goal and no foot entry: nothing drives it");
    }
    for (usize i = 0; i < a.chains.size(); ++i) {
        if (!ok[i]) continue;
        for (usize j = i + 1; j < a.chains.size(); ++j) {
            if (!ok[j]) continue;
            for (const u32 b : built[i].bones) {
                if (std::find(built[j].bones.begin(), built[j].bones.end(), b) == built[j].bones.end()) continue;
                if (a.chains[i].priority == a.chains[j].priority)
                    out.push_back("chains '" + a.chains[i].name + "' and '" + a.chains[j].name + "' share bone '" + skel.bones[b].name +
                                  "' at the same priority; the file order decides");
                break;
            }
        }
    }
    for (const IkRigLimitDef& l : a.limits)
        if (boneIndex(skel, l.bone) < 0) out.push_back("limit on '" + l.bone + "': not a bone of the skeleton");
    for (const IkRigFootDef& f : a.feet) {
        if (chainIndex(a, f.chain) < 0) out.push_back("foot on '" + f.chain + "' has no leg chain");
        if (!f.toe.empty() && boneIndex(skel, f.toe) < 0) out.push_back("foot '" + f.chain + "': toe bone '" + f.toe + "' is not in the skeleton");
    }
    return out;
}

std::string ikrigResolveSkeleton(const std::string& rigPath, const std::string& skeletonRef) {
    if (skeletonRef.empty()) return {};
    const fs::path ref(skeletonRef);
    if (ref.is_absolute()) return isFile(ref) ? ref.string() : std::string();
    const fs::path root(contentRootOf(rigPath));
    const fs::path candidates[] = {root / ref, fs::path(rigPath).parent_path() / ref, root.parent_path() / ref};
    for (const fs::path& c : candidates)
        if (isFile(c)) return c.string();
    return {};
}

// ------------------------------------------------------------------------------- the tab ----------

IkRigEditor::IkRigEditor(std::string path) : path_(std::move(path)) { loadFromDisk(); }

void IkRigEditor::loadFromDisk() {
    std::string why;
    IkRigAsset a;
    if (!anim::loadIkRig(path_, a, &why)) {
        loaded_ = false;
        loadError_ = why;
        return;
    }
    asset_ = std::move(a);
    loaded_ = true;
    loadError_.clear();
    dirty_ = false;
    selChain_ = -1;
    bindStale_ = true;
    history_.clear();
    loadSkeleton();
}

void IkRigEditor::loadSkeleton() {
    skelOk_ = false;
    skel_ = fmt::OcSkeleton{};
    skelPath_ = ikrigResolveSkeleton(path_, asset_.skeleton);
    std::string why;
    if (!skelPath_.empty() && fmt::loadOcSkel(skelPath_, skel_, &why)) {
        skelOk_ = true;
        anim::restPose(skel_, restPose_);
    }
}

void IkRigEditor::rebind() {
    bindStale_ = false;
    state_ = anim::IkRigState{};
    if (skelOk_) anim::bindIkRig(asset_, skel_, bound_);
    else bound_ = anim::IkRigBound{};
}

std::string IkRigEditor::title() const {
    return fs::path(path_).filename().string() + "###ikrig:" + path_;
}

bool IkRigEditor::save(std::string* why) {
    if (!loaded_) {
        if (why) *why = "cannot save: file failed to load (" + loadError_ + ")";
        return false;
    }
    if (!anim::saveIkRig(path_, asset_, why)) return false;
    dirty_ = false;
    return true;
}

void IkRigEditor::onFileChanged() {
    if (dirty_) {
        AVER_WARN("[IkRigEditor] '{}' changed on disk, but this tab has unsaved edits -- keeping them", path_);
        return;
    }
    loadFromDisk();
}

std::vector<std::string> IkRigEditor::problems() const {
    if (!skelOk_) {
        std::vector<std::string> out;
        std::string why;
        if (!asset_.valid(&why)) out.push_back("asset: " + why);
        out.push_back(asset_.skeleton.empty() ? "no skeleton is named" : "the skeleton '" + asset_.skeleton + "' was not found next to the project content");
        return out;
    }
    return ikrigProblems(asset_, skel_);
}

void IkRigEditor::undo() {
    if (!history_.undo(asset_)) return;
    dirty_ = true;
    bindStale_ = true;
    if (!inRange(asset_.chains.size(), selChain_)) selChain_ = -1;
    if (asset_.skeleton != skelPath_) loadSkeleton();
}

void IkRigEditor::redo() {
    if (!history_.redo(asset_)) return;
    dirty_ = true;
    bindStale_ = true;
    if (!inRange(asset_.chains.size(), selChain_)) selChain_ = -1;
    if (asset_.skeleton != skelPath_) loadSkeleton();
}

i32 IkRigEditor::addChain(const std::string& name, const std::string& start, const std::string& end, IkSolverKind solver) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 idx = ikrigAddChain(asset_, name, start, end, solver);
    if (idx < 0) { history_.cancelPush(); return -1; }
    selChain_ = idx;
    markDirty();
    return idx;
}

bool IkRigEditor::removeChain(i32 index) {
    if (!loaded_) return false;
    pushUndo();
    if (!ikrigRemoveChain(asset_, index)) { history_.cancelPush(); return false; }
    selChain_ = -1;
    markDirty();
    return true;
}

i32 IkRigEditor::moveChain(i32 index, i32 delta) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 to = ikrigMoveChain(asset_, index, delta);
    if (to < 0 || to == index) { history_.cancelPush(); return to; }
    selChain_ = to;
    markDirty();
    return to;
}

bool IkRigEditor::setPriority(i32 index, i32 priority) {
    if (!loaded_) return false;
    pushUndo();
    if (!ikrigSetPriority(asset_, index, priority)) { history_.cancelPush(); return false; }
    markDirty();
    return true;
}

bool IkRigEditor::renameChain(i32 index, const std::string& name) {
    if (!loaded_) return false;
    pushUndo();
    if (!ikrigRenameChain(asset_, index, name)) { history_.cancelPush(); return false; }
    markDirty();
    return true;
}

i32 IkRigEditor::addLimit(const std::string& bone, f32 s1, f32 s2, f32 tw) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 idx = ikrigAddLimit(asset_, bone, s1, s2, tw);
    if (idx < 0) { history_.cancelPush(); return -1; }
    markDirty();
    return idx;
}

bool IkRigEditor::removeLimit(const std::string& bone) {
    if (!loaded_) return false;
    pushUndo();
    if (!ikrigRemoveLimit(asset_, bone)) { history_.cancelPush(); return false; }
    markDirty();
    return true;
}

u32 IkRigEditor::importLimits(const f32* limitsDeg3PerBone, usize boneCount) {
    if (!loaded_ || !skelOk_) return 0;
    pushUndo();
    const u32 n = ikrigImportLimits(asset_, skel_, limitsDeg3PerBone, boneCount);
    if (n == 0) { history_.cancelPush(); return 0; }
    markDirty();
    return n;
}

i32 IkRigEditor::addFoot(const std::string& chain, const std::string& toe) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 idx = ikrigAddFoot(asset_, chain, toe);
    if (idx < 0) { history_.cancelPush(); return -1; }
    markDirty();
    return idx;
}

bool IkRigEditor::removeFoot(i32 index) {
    if (!loaded_) return false;
    pushUndo();
    if (!ikrigRemoveFoot(asset_, index)) { history_.cancelPush(); return false; }
    markDirty();
    return true;
}

#if AVER_WITH_IMGUI

namespace {

// A stable colour per chain index, as ABGR.
unsigned chainColor(usize i) {
    static const unsigned palette[] = {IM_COL32(255, 140, 90, 255), IM_COL32(110, 200, 255, 255), IM_COL32(150, 230, 120, 255),
                                       IM_COL32(230, 130, 230, 255), IM_COL32(255, 215, 90, 255), IM_COL32(120, 240, 215, 255)};
    return palette[i % (sizeof palette / sizeof palette[0])];
}

} // namespace

void IkRigEditor::touch(bool changed) {
    if (changed) {
        if (!gesture_) { history_.push(before_); gesture_ = true; }
        markDirty();
    }
    if (ImGui::IsItemDeactivated()) endGesture_ = true;
}

bool IkRigEditor::boneCombo(const char* label, std::string& bone) {
    bool changed = false;
    if (ImGui::BeginCombo(label, bone.empty() ? "<none>" : bone.c_str())) {
        if (ImGui::Selectable("<none>", bone.empty())) { pushUndo(); bone.clear(); changed = true; }
        for (const fmt::OcBone& b : skel_.bones)
            if (ImGui::Selectable(b.name.c_str(), b.name == bone)) { pushUndo(); bone = b.name; changed = true; }
        ImGui::EndCombo();
    }
    if (changed) markDirty();
    return changed;
}

void IkRigEditor::scanClips() {
    clipsScanned_ = true;
    clipPaths_.clear();
    clipNames_.clear();
    if (skelPath_.empty()) return;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::path(skelPath_).parent_path(), ec)) {
        if (!e.is_regular_file() || e.path().extension() != ".ocanim") continue;
        clipPaths_.push_back(e.path().string());
    }
    std::sort(clipPaths_.begin(), clipPaths_.end());
    for (const std::string& p : clipPaths_) clipNames_.push_back(fs::path(p).stem().string());
}

void IkRigEditor::loadClip(i32 index) {
    clipIndex_ = index;
    clipOk_ = false;
    if (!inRange(clipPaths_.size(), index)) return;
    std::string why;
    clipOk_ = fmt::loadOcAnim(clipPaths_[static_cast<usize>(index)], clip_, &why) && clip_.valid();
    clipTime_ = 0.0f;
}

void IkRigEditor::drawLeft() {
    if (!skelOk_) {
        ImGui::TextDisabled("no skeleton");
        return;
    }
    ImGui::TextDisabled("BONES");
    std::set<std::string> inChain;
    std::vector<std::pair<std::string, unsigned>> tag;
    for (usize ci = 0; ci < asset_.chains.size(); ++ci) {
        anim::IkChain chain;
        const i32 s = boneIndex(skel_, asset_.chains[ci].start), e = boneIndex(skel_, asset_.chains[ci].end);
        if (s < 0 || e < 0 || !anim::buildChain(skel_, static_cast<u32>(s), static_cast<u32>(e), chain)) continue;
        for (const u32 b : chain.bones) tag.emplace_back(skel_.bones[b].name, chainColor(ci));
    }
    for (usize i = 0; i < skel_.bones.size(); ++i) {
        const fmt::OcBone& b = skel_.bones[i];
        int depth = 0;
        for (i32 p = b.parent; p >= 0 && depth < 32; p = skel_.bones[static_cast<usize>(p)].parent) ++depth;
        ImGui::PushID(static_cast<int>(i));
        if (depth * 10.0f > 0.0f) ImGui::Indent(depth * 10.0f);   // Indent(0) would apply the default indent
        unsigned col = 0;
        for (const auto& t : tag) if (t.first == b.name) col = t.second;
        if (col) ImGui::PushStyleColor(ImGuiCol_Text, col);
        if (ImGui::Selectable(b.name.empty() ? "<unnamed>" : b.name.c_str(), static_cast<i32>(i) == selBone_)) selBone_ = static_cast<i32>(i);
        if (col) ImGui::PopStyleColor();
        if (depth * 10.0f > 0.0f) ImGui::Unindent(depth * 10.0f);
        ImGui::PopID();
    }
}

void IkRigEditor::drawPreview(float width, float height) {
    if (!skelOk_) {
        ImGui::TextWrapped("The skeleton '%s' was not found. Set the Skeleton field (Body tab) to a path under the project content.", asset_.skeleton.c_str());
        return;
    }
    if (bindStale_) rebind();
    if (!clipsScanned_) scanClips();

    // The pose: the rest pose, or a sample clip.
    anim::Pose pose = restPose_;
    if (clipOk_) {
        anim::sampleAnimation(clip_, clip_.duration > 0.0f ? std::fmod(clipTime_, clip_.duration) : 0.0f, pose);
    }

    // Goals: handles at the current tip of each goal's chain until dragged.
    std::vector<anim::IkGoalValue> goals(bound_.goalHashes.size());
    std::vector<RigHandle> handles;
    std::vector<std::string> handleGoal;
    std::vector<Mat4> model;
    anim::poseToModel(skel_, pose, model);
    const f32 dt = 1.0f / 60.0f;
    if (reachTest_) reachPhase_ += dt * 2.0f;
    for (usize g = 0; g < bound_.goalNames.size(); ++g) {
        const std::string& name = bound_.goalNames[g];
        bool on = goalOn_[name];
        const anim::IkBoundChain* chainOf = nullptr;
        for (const anim::IkBoundChain& c : bound_.chains) if (c.goalSlot == static_cast<i32>(g)) chainOf = &c;
        if (!goalPos_.count(name)) {
            Vec3 start{0, 0, 100};
            if (chainOf) { const Mat4& m = model[chainOf->chain.bones.back()]; start = Vec3{m.m[3][0], m.m[3][1], m.m[3][2]}; }
            else if (bound_.bodyBone != anim::kIkNone) { const Mat4& m = model[bound_.bodyBone]; start = Vec3{m.m[3][0], m.m[3][1], m.m[3][2]}; }
            goalPos_[name] = start;
        }
        Vec3 p = goalPos_[name];
        if (on && reachTest_ && chainOf) {
            const Mat4& r = model[chainOf->chain.bones.front()];
            p = Vec3{r.m[3][0], r.m[3][1], r.m[3][2]} + Vec3{std::cos(reachPhase_), std::sin(reachPhase_), 0.35f} * reachRadius_;
        }
        goals[g].active = on;
        goals[g].pos = p;
        goals[g].alpha = on ? 1.0f : 0.0f;
        RigHandle h;
        h.pos = p;
        h.label = name.c_str();
        h.enabled = on && !reachTest_;
        h.color = on ? IM_COL32(90, 220, 255, 255) : IM_COL32(120, 120, 120, 255);
        handles.push_back(h);
        handleGoal.push_back(name);
    }

    // Solve with the real stack over the synthetic ground.
    struct Plane { f32 h, slope; } plane{groundHeight_, std::tan(radians(groundSlopeDeg_))};
    anim::IkRigInputs in;
    in.dt = dt;
    in.goals = goals.data();
    in.goalCount = goals.size();
    if (groundOn_) {
        in.ground = [](const Vec3& o, f32 dist, u32, Vec3& p, Vec3& n, void* user) {
            const Plane* pl = static_cast<const Plane*>(user);
            const f32 z = pl->h - pl->slope * o.x;
            if (o.z - z < 0.0f || o.z - z > dist) return false;
            p = Vec3{o.x, o.y, z};
            n = Vec3{pl->slope, 0.0f, 1.0f}.getSafeNormal();
            return true;
        };
        in.groundUser = &plane;
    }
    anim::solveIkRig(skel_, bound_, state_, in, pose, report_, scratch_);

    // Colours: each chain its own, the selected chain brighter.
    RigPreviewIn pv;
    pv.skel = &skel_;
    pv.pose = &pose;
    pv.boneColors.assign(skel_.bones.size(), 0u);
    for (usize ci = 0; ci < asset_.chains.size(); ++ci) {
        anim::IkChain chain;
        const i32 s = boneIndex(skel_, asset_.chains[ci].start), e = boneIndex(skel_, asset_.chains[ci].end);
        if (s < 0 || e < 0 || !anim::buildChain(skel_, static_cast<u32>(s), static_cast<u32>(e), chain)) continue;
        for (const u32 b : chain.bones) pv.boneColors[b] = (static_cast<i32>(ci) == selChain_) ? IM_COL32(255, 255, 255, 255) : chainColor(ci);
    }
    pv.handles = handles;
    pv.selectedBone = selBone_;
    pv.drawGround = groundOn_;
    pv.groundZ = groundHeight_;
    pv.groundNormal = Vec3{plane.slope, 0.0f, 1.0f};
    pv.caption = "orbit: left drag   pan: right drag   zoom: wheel";
    if (!framed_) { rigFramePose(view_, model); framed_ = true; }
    const RigPreviewOut out = drawRigPreview("##rigview", width, height, view_, pv);
    if (out.draggingHandle >= 0) goalPos_[handleGoal[static_cast<usize>(out.draggingHandle)]] = out.handlePos;
    if (out.clickedBone >= 0) selBone_ = out.clickedBone;
}

void IkRigEditor::drawChains() {
    const f32 dpi = fontScale();
    if (ImGui::Button("Add chain") && skelOk_) {
        std::string s = selBone_ >= 0 ? skel_.bones[static_cast<usize>(selBone_)].name : (skel_.bones.empty() ? std::string() : skel_.bones[0].name);
        std::string e = s;
        if (selBone_ >= 0) for (usize i = skel_.bones.size(); i-- > 0;) if (skel_.bones[i].parent == selBone_) { e = skel_.bones[i].name; break; }
        addChain("Chain", s, e, IkSolverKind::Fabrik);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!inRange(asset_.chains.size(), selChain_));
    if (ImGui::Button("Remove")) removeChain(selChain_);
    ImGui::SameLine();
    if (ImGui::Button("Up")) moveChain(selChain_, -1);
    ImGui::SameLine();
    if (ImGui::Button("Down")) moveChain(selChain_, 1);
    ImGui::EndDisabled();
    ImGui::Separator();
    char buf[256];
    for (usize i = 0; i < asset_.chains.size(); ++i) {
        IkRigChainDef& c = asset_.chains[i];
        ImGui::PushID(static_cast<int>(i));
        const bool sel = selChain_ == static_cast<i32>(i);
        ImGui::PushStyleColor(ImGuiCol_Text, chainColor(i));
        if (ImGui::Selectable(c.name.c_str(), sel)) selChain_ = static_cast<i32>(i);
        ImGui::PopStyleColor();
        if (sel) {
            ImGui::Indent(8.0f * dpi);
            std::snprintf(buf, sizeof buf, "%s", c.name.c_str());
            if (ImGui::InputText("name", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) renameChain(static_cast<i32>(i), buf);
            if (skelOk_) { boneCombo("start", c.start); boneCombo("end", c.end); }
            int solver = static_cast<int>(c.solver);
            if (ImGui::Combo("solver", &solver, "twobone\0fabrik\0ccd\0")) {
                pushUndo();
                c.solver = static_cast<IkSolverKind>(solver);
                if (c.solver == IkSolverKind::Ccd && c.iterations == 10) { c.iterations = 12; c.toleranceCm = 0.5f; }
                markDirty();
            }
            int pri = c.priority;
            ImGui::SetNextItemWidth(80.0f * dpi);
            if (ImGui::InputInt("priority", &pri)) setPriority(static_cast<i32>(i), pri);
            ImGui::SetItemTooltip("Ascending is more important. A bone a chain rotated is claimed; later chains start below it.");
            std::snprintf(buf, sizeof buf, "%s", c.goal.c_str());
            const bool gc = ImGui::InputText("goal", buf, sizeof buf);
            if (gc) c.goal = buf;
            touch(gc);
            if (c.solver != IkSolverKind::TwoBone) {
                int it = static_cast<int>(c.iterations);
                ImGui::SetNextItemWidth(80.0f * dpi);
                if (ImGui::DragInt("iterations", &it, 0.2f, 1, 64)) { if (!gesture_) { history_.push(before_); gesture_ = true; } c.iterations = static_cast<u32>(it); markDirty(); }
                if (ImGui::IsItemDeactivated()) endGesture_ = true;
                ImGui::SetNextItemWidth(80.0f * dpi);
                touch(ImGui::DragFloat("tolerance cm", &c.toleranceCm, 0.01f, 0.0f, 10.0f));
                if (c.solver == IkSolverKind::Ccd) {
                    ImGui::SetNextItemWidth(80.0f * dpi);
                    touch(ImGui::DragFloat("max step deg", &c.maxStepDeg, 0.5f, 1.0f, 180.0f));
                }
            }
            bool b = c.limits;
            if (ImGui::Checkbox("limits", &b)) { pushUndo(); c.limits = b; markDirty(); }
            ImGui::SameLine();
            b = c.tipRotation;
            if (ImGui::Checkbox("tip rotation", &b)) { pushUndo(); c.tipRotation = b; markDirty(); }
            int pole = static_cast<int>(c.pole.mode);
            if (ImGui::Combo("pole", &pole, "animated\0bone\0vector\0")) { pushUndo(); c.pole.mode = static_cast<anim::IkPoleMode>(pole); markDirty(); }
            if (c.pole.mode == anim::IkPoleMode::Bone && skelOk_) boneCombo("pole bone", c.pole.bone);
            if (c.pole.mode == anim::IkPoleMode::Vector) {
                float d[3] = {c.pole.dirModel.x, c.pole.dirModel.y, c.pole.dirModel.z};
                if (ImGui::DragFloat3("direction", d, 0.01f)) { if (!gesture_) { history_.push(before_); gesture_ = true; } c.pole.dirModel = Vec3{d[0], d[1], d[2]}; markDirty(); }
                if (ImGui::IsItemDeactivated()) endGesture_ = true;
            }
            ImGui::Unindent(8.0f * dpi);
        }
        ImGui::PopID();
    }
}

void IkRigEditor::drawLimits() {
    const f32 dpi = fontScale();
    if (skelOk_ && selBone_ >= 0) {
        const std::string& bn = skel_.bones[static_cast<usize>(selBone_)].name;
        if (ImGui::Button(("Add limit for " + bn).c_str())) addLimit(bn, 45.0f, 45.0f, 30.0f);
    } else {
        ImGui::TextDisabled("Pick a bone in the view or the bone list to add a limit.");
    }
#if AVER_MODULE_RAGDOLL
    if (skelOk_) {
        if (ImGui::Button("Import from ragdoll")) {
            ragdoll::RagdollOverrides ov;
            const std::string ovPath = ragdoll::overridePathFor(skelPath_);
            const bool haveOv = isFile(ovPath) && ragdoll::loadRagdollOverrides(ovPath, ov);
            const ragdoll::RagdollDef def = ragdoll::buildRagdoll(skel_, ragdoll::BuildOptions{}, haveOv ? &ov : nullptr);
            const u32 n = importLimits(def.limitsDeg.data(), def.boneCount);
            status_ = "Imported " + std::to_string(n) + " limit(s) from the ragdoll" + (haveOv ? " (with the .ocragdoll override)." : ".");
        }
        ImGui::SameLine();
    }
#endif
    if (skelOk_ && ImGui::Button("Reset to 45/45/30")) {
        pushUndo();
        const u32 n = ikrigResetLimits(asset_, skel_);
        if (n == 0) history_.cancelPush(); else markDirty();
    }
    if (!status_.empty()) ImGui::TextDisabled("%s", status_.c_str());
    ImGui::Separator();
    int removeAt = -1;
    for (usize i = 0; i < asset_.limits.size(); ++i) {
        IkRigLimitDef& l = asset_.limits[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TextUnformatted(l.bone.c_str());
        ImGui::SetNextItemWidth(64.0f * dpi);
        touch(ImGui::DragFloat("swing1", &l.swing1, 0.5f, 0.0f, 180.0f, "%.0f"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(64.0f * dpi);
        touch(ImGui::DragFloat("swing2", &l.swing2, 0.5f, 0.0f, 180.0f, "%.0f"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(64.0f * dpi);
        touch(ImGui::DragFloat("twist", &l.twist, 0.5f, 0.0f, 180.0f, "%.0f"));
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeAt = static_cast<int>(i);
        bool ax = l.hasAxis;
        if (ImGui::Checkbox("axis", &ax)) { pushUndo(); l.hasAxis = ax; markDirty(); }
        if (l.hasAxis) {
            ImGui::SameLine();
            float a[3] = {l.axis.x, l.axis.y, l.axis.z};
            if (ImGui::DragFloat3("##axis", a, 0.01f)) { if (!gesture_) { history_.push(before_); gesture_ = true; } l.axis = Vec3{a[0], a[1], a[2]}; markDirty(); }
            if (ImGui::IsItemDeactivated()) endGesture_ = true;
        }
        ImGui::PopID();
    }
    if (removeAt >= 0) removeLimit(asset_.limits[static_cast<usize>(removeAt)].bone);
}

void IkRigEditor::drawFeet() {
    const f32 dpi = fontScale();
    if (ImGui::BeginCombo("Add foot", "chain...")) {
        for (const IkRigChainDef& c : asset_.chains) {
            bool has = false;
            for (const IkRigFootDef& f : asset_.feet) has = has || f.chain == c.name;
            if (has) continue;
            if (ImGui::Selectable(c.name.c_str())) addFoot(c.name, "");
        }
        ImGui::EndCombo();
    }
    int removeAt = -1;
    char buf[128];
    for (usize i = 0; i < asset_.feet.size(); ++i) {
        IkRigFootDef& f = asset_.feet[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::SeparatorText(f.chain.c_str());
        if (skelOk_) boneCombo("toe", f.toe);
        ImGui::SetNextItemWidth(80.0f * dpi);
        touch(ImGui::DragFloat("sole offset cm", &f.soleOffsetCm, 0.1f, -1.0f, 50.0f));
        ImGui::SetItemTooltip("Ankle height above the sole. -1 derives it from the rest pose.");
        std::snprintf(buf, sizeof buf, "%s", f.plantCurve.c_str());
        if (ImGui::InputText("plant curve", buf, sizeof buf)) { f.plantCurve = buf; markDirty(); }
        touch(ImGui::IsItemEdited());
        ImGui::SetNextItemWidth(70.0f * dpi);
        touch(ImGui::DragFloat("plant low", &f.plantLowCm, 0.1f, 0.0f, 100.0f));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f * dpi);
        touch(ImGui::DragFloat("plant high", &f.plantHighCm, 0.1f, 0.0f, 100.0f));
        ImGui::SetNextItemWidth(70.0f * dpi);
        touch(ImGui::DragFloat("probe up", &f.probeUpCm, 0.5f, 1.0f, 500.0f));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f * dpi);
        touch(ImGui::DragFloat("probe down", &f.probeDownCm, 0.5f, 1.0f, 500.0f));
        bool al = f.alignToNormal;
        if (ImGui::Checkbox("align to ground normal", &al)) { pushUndo(); f.alignToNormal = al; markDirty(); }
        ImGui::SetNextItemWidth(70.0f * dpi);
        touch(ImGui::DragFloat("max align deg", &f.maxAlignDeg, 0.5f, 0.0f, 90.0f));
        if (ImGui::SmallButton("Remove foot")) removeAt = static_cast<int>(i);
        ImGui::PopID();
    }
    if (removeAt >= 0) removeFoot(removeAt);
    ImGui::Separator();
    ImGui::TextDisabled("FOOT SETTINGS");
    int pm = static_cast<int>(asset_.foot.pelvisMode);
    if (ImGui::Combo("pelvis", &pm, "lowest\0none\0")) { pushUndo(); asset_.foot.pelvisMode = static_cast<anim::IkPelvisMode>(pm); markDirty(); }
    ImGui::SetNextItemWidth(80.0f * dpi);
    touch(ImGui::DragFloat("smoothing 1/s", &asset_.foot.smoothing, 0.1f, 0.0f, 100.0f));
    ImGui::SetNextItemWidth(80.0f * dpi);
    touch(ImGui::DragFloat("max pelvis drop cm", &asset_.foot.maxPelvisDropCm, 0.5f, 0.0f, 200.0f));
    ImGui::SetNextItemWidth(80.0f * dpi);
    touch(ImGui::DragFloat("max foot offset cm", &asset_.foot.maxFootOffsetCm, 0.5f, 0.0f, 200.0f));
    int mask = static_cast<int>(asset_.foot.layerMask);
    ImGui::SetNextItemWidth(120.0f * dpi);
    if (ImGui::InputInt("layer mask", &mask, 1, 100, ImGuiInputTextFlags_CharsHexadecimal)) { pushUndo(); asset_.foot.layerMask = static_cast<u32>(mask); markDirty(); }
}

void IkRigEditor::drawBody() {
    const f32 dpi = fontScale();
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s", asset_.skeleton.c_str());
    if (ImGui::InputText("Skeleton", buf, sizeof buf)) { asset_.skeleton = buf; markDirty(); }
    touch(ImGui::IsItemEdited());
    if (ImGui::IsItemDeactivatedAfterEdit()) { loadSkeleton(); bindStale_ = true; }
    ImGui::TextDisabled("%s", skelOk_ ? skelPath_.c_str() : "not found");
    ImGui::Separator();
    if (skelOk_) boneCombo("Body bone", asset_.body.bone);
    std::snprintf(buf, sizeof buf, "%s", asset_.body.goal.c_str());
    const bool gc = ImGui::InputText("Body goal", buf, sizeof buf);
    if (gc) asset_.body.goal = buf;
    touch(gc);
    ImGui::SetNextItemWidth(80.0f * dpi);
    touch(ImGui::DragFloat("max offset cm", &asset_.body.maxOffsetCm, 0.5f, 0.0f, 200.0f));
}

void IkRigEditor::drawProblems() {
    const std::vector<std::string> p = problems();
    if (p.empty()) { ImGui::TextDisabled("No problems."); return; }
    for (const std::string& s : p) ImGui::BulletText("%s", s.c_str());
}

void IkRigEditor::drawRight() {
    if (ImGui::BeginTabBar("##ikrigtabs")) {
        if (ImGui::BeginTabItem("Chains")) { drawChains(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Limits")) { drawLimits(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Feet")) { drawFeet(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Body")) { drawBody(); ImGui::EndTabItem(); }
        const usize np = problems().size();
        const std::string pt = "Problems (" + std::to_string(np) + ")###problems";
        if (ImGui::BeginTabItem(pt.c_str())) { drawProblems(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
}

void IkRigEditor::draw(Engine& e) {
    (void)e;
    if (!loaded_) {
        ImGui::TextWrapped("This file could not be read: %s", loadError_.c_str());
        return;
    }
    before_ = asset_;
    if (ImGui::Button("Save") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                                  keybinds().pressed(CommandId::AssetSave, ImGui::GetIO()))) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[IkRigEditor] save failed for '{}': {}", path_, why);
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
    char nm[128];
    std::snprintf(nm, sizeof nm, "%s", asset_.name.c_str());
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::InputText("##name", nm, sizeof nm)) { asset_.name = nm; markDirty(); }
    touch(ImGui::IsItemEdited());
    ImGui::Separator();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const f32 leftW = std::max(140.0f, avail.x * 0.18f), rightW = std::max(300.0f, avail.x * 0.34f);
    const f32 midW = std::max(160.0f, avail.x - leftW - rightW - 16.0f);
    if (ImGui::BeginChild("##left", ImVec2(leftW, avail.y), true)) drawLeft();
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##mid", ImVec2(midW, avail.y), true)) {
        // Controls above the view.
        if (skelOk_) {
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::BeginCombo("##clip", clipIndex_ >= 0 ? clipNames_[static_cast<usize>(clipIndex_)].c_str() : "rest pose")) {
                if (ImGui::Selectable("rest pose", clipIndex_ < 0)) { clipIndex_ = -1; clipOk_ = false; }
                for (usize i = 0; i < clipNames_.size(); ++i)
                    if (ImGui::Selectable(clipNames_[i].c_str(), static_cast<i32>(i) == clipIndex_)) loadClip(static_cast<i32>(i));
                ImGui::EndCombo();
            }
            if (clipOk_) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120.0f);
                ImGui::SliderFloat("##t", &clipTime_, 0.0f, std::max(clip_.duration, 0.01f), "%.2f s");
            }
            ImGui::Checkbox("ground", &groundOn_);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100.0f);
            ImGui::DragFloat("height", &groundHeight_, 0.2f, -60.0f, 60.0f, "%.1f cm");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100.0f);
            ImGui::DragFloat("slope", &groundSlopeDeg_, 0.2f, -45.0f, 45.0f, "%.1f deg");
            ImGui::Checkbox("reach test", &reachTest_);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            ImGui::DragFloat("radius", &reachRadius_, 0.5f, 5.0f, 150.0f, "%.0f");
            for (usize g = 0; g < bound_.goalNames.size(); ++g) {
                ImGui::SameLine();
                bool on = goalOn_[bound_.goalNames[g]];
                if (ImGui::Checkbox(bound_.goalNames[g].c_str(), &on)) goalOn_[bound_.goalNames[g]] = on;
            }
        }
        const ImVec2 room = ImGui::GetContentRegionAvail();
        drawPreview(room.x, std::max(120.0f, room.y));
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##right", ImVec2(rightW, avail.y), true)) drawRight();
    ImGui::EndChild();

    if (endGesture_ && !ImGui::IsMouseDown(0)) { gesture_ = false; endGesture_ = false; }
}

#else   // AVER_WITH_IMGUI

void IkRigEditor::draw(Engine& e) { (void)e; }

#endif  // AVER_WITH_IMGUI

std::unique_ptr<AssetEditor> makeIkRigEditor(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".ocikrig") return nullptr;
    return std::make_unique<IkRigEditor>(path);
}

} // namespace aver::editor
