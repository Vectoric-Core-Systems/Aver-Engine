// The .ocretarget editor tab. See the header for the SandboxApp hook and for why the edits are free functions.
#include "RetargetEditor.hpp"
#include "EditorChrome.hpp"
#include "EditorKeybinds.hpp"

#include "aver/anim/AnimSampler.hpp"
#include "aver/core/Log.hpp"

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

using anim::RetargetAsset;
using anim::RetargetChainDef;

namespace {

namespace fs = std::filesystem;

bool inRange(usize n, i32 i) { return i >= 0 && static_cast<usize>(i) < n; }

i32 chainIndex(const RetargetAsset& a, const std::string& name) {
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

bool isAncestorOrSelf(const fmt::OcSkeleton& s, i32 anc, i32 b) {
    for (usize guard = 0; b >= 0 && guard <= s.bones.size(); ++guard) {
        if (b == anc) return true;
        b = s.bones[static_cast<usize>(b)].parent;
    }
    return false;
}

bool iequals(const std::string& a, const char* b) {
    const usize n = std::char_traits<char>::length(b);
    if (a.size() != n) return false;
    for (usize i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

bool isFile(const fs::path& p) {
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(p, ec);
}

// Content-relative skeleton reference -> a file: the nearest "Content" ancestor of the asset, its own folder, or the
// parent of the root.
std::string resolveSkeleton(const std::string& assetPath, const std::string& ref) {
    if (ref.empty()) return {};
    const fs::path r(ref);
    if (r.is_absolute()) return isFile(r) ? r.string() : std::string();
    fs::path root = fs::path(assetPath).parent_path();
    for (fs::path d = fs::path(assetPath).parent_path(); !d.empty(); d = d.parent_path()) {
        if (iequals(d.filename().string(), "Content")) { root = d; break; }
        if (d == d.parent_path()) break;
    }
    const fs::path candidates[] = {root / r, fs::path(assetPath).parent_path() / r, root.parent_path() / r};
    for (const fs::path& c : candidates)
        if (isFile(c)) return c.string();
    return {};
}

// The bones of a parent path start..end (root first); empty when it is not one.
std::vector<u32> pathOf(const fmt::OcSkeleton& s, i32 start, i32 end) {
    std::vector<u32> out;
    if (start < 0 || end < 0) return out;
    for (i32 k = end; k >= 0; k = s.bones[static_cast<usize>(k)].parent) {
        out.push_back(static_cast<u32>(k));
        if (k == start) { std::reverse(out.begin(), out.end()); return out; }
    }
    out.clear();
    return out;
}

} // namespace

// ------------------------------------------------------------------------------- edits ----------

RetargetAsset rtgStarter(const std::string& srcRef, const std::string& dstRef) {
    RetargetAsset a;
    a.name = fs::path(srcRef).stem().string() + "_to_" + fs::path(dstRef).stem().string();
    a.source = srcRef;
    a.target = dstRef;
    return a;
}

std::string rtgUniqueChainName(const RetargetAsset& a, const std::string& base) {
    const std::string b = base.empty() ? std::string("Chain") : base;
    if (chainIndex(a, b) < 0) return b;
    for (int n = 2;; ++n) {
        const std::string cand = b + " " + std::to_string(n);
        if (chainIndex(a, cand) < 0) return cand;
    }
}

i32 rtgAddChain(RetargetAsset& a, const std::string& name, const std::string& srcStart, const std::string& srcEnd,
                const std::string& dstStart, const std::string& dstEnd, anim::RtgMode mode) {
    if (srcStart.empty() || srcEnd.empty() || dstStart.empty() || dstEnd.empty()) return -1;
    RetargetChainDef c;
    c.name = rtgUniqueChainName(a, name);
    c.srcStart = srcStart; c.srcEnd = srcEnd;
    c.dstStart = dstStart; c.dstEnd = dstEnd;
    c.mode = mode;
    a.chains.push_back(std::move(c));
    return static_cast<i32>(a.chains.size() - 1);
}

bool rtgRemoveChain(RetargetAsset& a, i32 index) {
    if (!inRange(a.chains.size(), index)) return false;
    a.chains.erase(a.chains.begin() + index);
    return true;
}

bool rtgSetBones(RetargetAsset& a, i32 index, const std::string& srcStart, const std::string& srcEnd,
                 const std::string& dstStart, const std::string& dstEnd) {
    if (!inRange(a.chains.size(), index) || srcStart.empty() || srcEnd.empty() || dstStart.empty() || dstEnd.empty()) return false;
    RetargetChainDef& c = a.chains[static_cast<usize>(index)];
    c.srcStart = srcStart; c.srcEnd = srcEnd;
    c.dstStart = dstStart; c.dstEnd = dstEnd;
    return true;
}

anim::AutoMapResult rtgAutoMap(const fmt::OcSkeleton& src, const fmt::OcSkeleton& dst) { return anim::autoMapChains(src, dst); }

u32 rtgApplyMap(RetargetAsset& a, const anim::AutoMapResult& draft, const std::vector<bool>& acceptMask, bool applyRoot) {
    u32 taken = 0;
    for (usize i = 0; i < draft.asset.chains.size(); ++i) {
        if (i >= acceptMask.size() || !acceptMask[i]) continue;
        const i32 at = chainIndex(a, draft.asset.chains[i].name);
        if (at >= 0) a.chains[static_cast<usize>(at)] = draft.asset.chains[i];
        else a.chains.push_back(draft.asset.chains[i]);
        ++taken;
    }
    if (applyRoot && taken > 0) {
        a.root.srcBone = draft.asset.root.srcBone;
        a.root.dstBone = draft.asset.root.dstBone;
        a.rootMotion.srcBone = draft.asset.rootMotion.srcBone;
        a.rootMotion.dstBone = draft.asset.rootMotion.dstBone;
    }
    return taken;
}

std::vector<std::string> rtgProblems(const RetargetAsset& a, const fmt::OcSkeleton& src, const fmt::OcSkeleton& dst) {
    std::vector<std::string> out;
    std::string why;
    if (!a.valid(&why)) out.push_back("asset: " + why);
    auto need = [&](const fmt::OcSkeleton& s, const std::string& bone, const char* side, const std::string& what) {
        if (bone.empty()) return true;
        if (boneIndex(s, bone) >= 0) return true;
        out.push_back(what + ": " + side + " bone '" + bone + "' is not in the skeleton");
        return false;
    };
    need(src, a.root.srcBone, "source", "root");
    need(dst, a.root.dstBone, "target", "root");
    if (a.rootMotion.srcBone.empty() != a.rootMotion.dstBone.empty()) out.push_back("rootMotion needs both bones or neither");
    need(src, a.rootMotion.srcBone, "source", "rootMotion");
    need(dst, a.rootMotion.dstBone, "target", "rootMotion");
    const i32 dstRoot = boneIndex(dst, a.root.dstBone);

    std::vector<std::string> usedS(src.bones.size()), usedD(dst.bones.size());
    for (const RetargetChainDef& c : a.chains) {
        const std::string what = "chain '" + c.name + "'";
        const bool okSS = need(src, c.srcStart, "source", what), okSE = need(src, c.srcEnd, "source", what);
        const bool okDS = need(dst, c.dstStart, "target", what), okDE = need(dst, c.dstEnd, "target", what);
        std::vector<u32> sp, dp;
        if (okSS && okSE) {
            sp = pathOf(src, boneIndex(src, c.srcStart), boneIndex(src, c.srcEnd));
            if (sp.empty()) out.push_back(what + ": '" + c.srcStart + "' is not an ancestor of '" + c.srcEnd + "' on the source");
        }
        if (okDS && okDE) {
            dp = pathOf(dst, boneIndex(dst, c.dstStart), boneIndex(dst, c.dstEnd));
            if (dp.empty()) out.push_back(what + ": '" + c.dstStart + "' is not an ancestor of '" + c.dstEnd + "' on the target");
        }
        for (const u32 b : sp) {
            if (!usedS[b].empty()) out.push_back("source bone '" + src.bones[b].name + "' is in both '" + usedS[b] + "' and '" + c.name + "'");
            else usedS[b] = c.name;
        }
        for (const u32 b : dp) {
            if (!usedD[b].empty()) out.push_back("target bone '" + dst.bones[b].name + "' is in both '" + usedD[b] + "' and '" + c.name + "'");
            else usedD[b] = c.name;
        }
        if (!sp.empty() && !dp.empty()) {
            if (c.rotation == anim::RtgRotation::OneToOne && sp.size() != dp.size())
                out.push_back(what + " is oneToOne but maps " + std::to_string(sp.size()) + " source bones onto " + std::to_string(dp.size()));
            if (c.mode == anim::RtgMode::Ik && (sp.size() < 2 || dp.size() < 2))
                out.push_back(what + ": an ik chain needs at least two bones on each side (it will be fk)");
        }
        if (!dp.empty() && dstRoot >= 0 && !isAncestorOrSelf(dst, dstRoot, static_cast<i32>(dp.front())))
            out.push_back(what + " is not under the target root bone '" + a.root.dstBone + "'");
    }
    if (a.chains.empty()) out.push_back("no chains: run Auto-map, or add chains by hand");
    return out;
}

RtgScales rtgScalePreview(const RetargetAsset& a, const fmt::OcSkeleton& src, const fmt::OcSkeleton& dst) {
    RtgScales s;
    anim::Retargeter rt;
    if (!rt.bind(a, src, dst, nullptr)) return s;
    s.scaleH = rt.scaleH();
    s.scaleV = rt.scaleV();
    s.bound = true;
    return s;
}

// ------------------------------------------------------------------------------- the tab ----------

RetargetEditor::RetargetEditor(std::string path) : path_(std::move(path)) { loadFromDisk(); }

void RetargetEditor::loadFromDisk() {
    std::string why;
    RetargetAsset a;
    if (!anim::loadRetarget(path_, a, &why)) {
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
    draft_ = anim::AutoMapResult{};
    accept_.clear();
    history_.clear();
    loadSkeletons();
}

void RetargetEditor::loadSkeletons() {
    srcOk_ = dstOk_ = false;
    src_ = fmt::OcSkeleton{};
    dst_ = fmt::OcSkeleton{};
    std::string why;
    srcPath_ = resolveSkeleton(path_, asset_.source);
    dstPath_ = resolveSkeleton(path_, asset_.target);
    srcOk_ = !srcPath_.empty() && fmt::loadOcSkel(srcPath_, src_, &why);
    dstOk_ = !dstPath_.empty() && fmt::loadOcSkel(dstPath_, dst_, &why);
    bindStale_ = true;
}

void RetargetEditor::rebind() {
    bindStale_ = false;
    bindIssues_.clear();
    rtOk_ = srcOk_ && dstOk_ && rt_.bind(asset_, src_, dst_, &bindIssues_);
}

std::string RetargetEditor::title() const { return fs::path(path_).filename().string() + "###rtg:" + path_; }

bool RetargetEditor::save(std::string* why) {
    if (!loaded_) {
        if (why) *why = "cannot save: file failed to load (" + loadError_ + ")";
        return false;
    }
    if (!anim::saveRetarget(path_, asset_, why)) return false;
    dirty_ = false;
    return true;
}

void RetargetEditor::onFileChanged() {
    if (dirty_) {
        AVER_WARN("[RetargetEditor] '{}' changed on disk, but this tab has unsaved edits -- keeping them", path_);
        return;
    }
    loadFromDisk();
}

std::vector<std::string> RetargetEditor::problems() const {
    if (!srcOk_ || !dstOk_) {
        std::vector<std::string> out;
        std::string why;
        if (!asset_.valid(&why)) out.push_back("asset: " + why);
        if (!srcOk_) out.push_back("the source skeleton '" + asset_.source + "' was not found next to the project content");
        if (!dstOk_) out.push_back("the target skeleton '" + asset_.target + "' was not found next to the project content");
        return out;
    }
    return rtgProblems(asset_, src_, dst_);
}

RtgScales RetargetEditor::scales() const { return (srcOk_ && dstOk_) ? rtgScalePreview(asset_, src_, dst_) : RtgScales{}; }

void RetargetEditor::undo() {
    if (!history_.undo(asset_)) return;
    dirty_ = true;
    bindStale_ = true;
    if (!inRange(asset_.chains.size(), selChain_)) selChain_ = -1;
}

void RetargetEditor::redo() {
    if (!history_.redo(asset_)) return;
    dirty_ = true;
    bindStale_ = true;
    if (!inRange(asset_.chains.size(), selChain_)) selChain_ = -1;
}

i32 RetargetEditor::addChain(const std::string& name, const std::string& srcStart, const std::string& srcEnd,
                             const std::string& dstStart, const std::string& dstEnd, anim::RtgMode mode) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 idx = rtgAddChain(asset_, name, srcStart, srcEnd, dstStart, dstEnd, mode);
    if (idx < 0) { history_.cancelPush(); return -1; }
    selChain_ = idx;
    markDirty();
    return idx;
}

bool RetargetEditor::removeChain(i32 index) {
    if (!loaded_) return false;
    pushUndo();
    if (!rtgRemoveChain(asset_, index)) { history_.cancelPush(); return false; }
    selChain_ = -1;
    markDirty();
    return true;
}

void RetargetEditor::autoMap() {
    if (!srcOk_ || !dstOk_) return;
    draft_ = rtgAutoMap(src_, dst_);
    accept_.assign(draft_.asset.chains.size(), true);
}

u32 RetargetEditor::applyDraft(bool applyRoot) {
    if (!loaded_) return 0;
    pushUndo();
    const u32 n = rtgApplyMap(asset_, draft_, accept_, applyRoot);
    if (n == 0) { history_.cancelPush(); return 0; }
    markDirty();
    return n;
}

anim::RetargetBatchResult RetargetEditor::runBatch(const std::vector<std::string>& clips, const std::string& outDir,
                                                   const std::string& suffix, const anim::RetargetOptions& opt, bool overwrite) {
    anim::RetargetBatchResult r;
    if (!srcOk_ || !dstOk_) {
        r.failed = static_cast<u32>(clips.size());
        r.issues.push_back({anim::RetargetIssue::Kind::Error, "the source or target skeleton was not found"});
        return r;
    }
    return anim::retargetFolder(asset_, srcPath_, dstPath_, clips, outDir, suffix, opt, overwrite);
}

#if AVER_WITH_IMGUI

namespace {
unsigned chainColor(usize i) {
    static const unsigned palette[] = {IM_COL32(255, 140, 90, 255), IM_COL32(110, 200, 255, 255), IM_COL32(150, 230, 120, 255),
                                       IM_COL32(230, 130, 230, 255), IM_COL32(255, 215, 90, 255), IM_COL32(120, 240, 215, 255)};
    return palette[i % (sizeof palette / sizeof palette[0])];
}
const char* confidenceLabel(anim::RtgConfidence c) {
    switch (c) {
        case anim::RtgConfidence::Exact: return "exact";
        case anim::RtgConfidence::Synonym: return "synonym";
        case anim::RtgConfidence::Structural: return "structural";
    }
    return "";
}
} // namespace

void RetargetEditor::touch(bool changed) {
    if (changed) {
        if (!gesture_) { history_.push(before_); gesture_ = true; }
        markDirty();
    }
    if (ImGui::IsItemDeactivated()) endGesture_ = true;
}

bool RetargetEditor::boneCombo(const char* label, const fmt::OcSkeleton& skel, std::string& bone) {
    bool changed = false;
    if (ImGui::BeginCombo(label, bone.empty() ? "<none>" : bone.c_str())) {
        for (const fmt::OcBone& b : skel.bones)
            if (ImGui::Selectable(b.name.c_str(), b.name == bone)) { pushUndo(); bone = b.name; changed = true; }
        ImGui::EndCombo();
    }
    if (changed) markDirty();
    return changed;
}

void RetargetEditor::scanClips() {
    clipsScanned_ = true;
    clipPaths_.clear();
    clipNames_.clear();
    if (srcPath_.empty()) return;
    const std::string stem = fs::path(srcPath_).stem().string();
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::path(srcPath_).parent_path(), ec)) {
        if (!e.is_regular_file() || e.path().extension() != ".ocanim") continue;
        fmt::OcAnimation c;
        if (!fmt::loadOcAnim(e.path().string(), c, nullptr)) continue;
        if (c.flags & (fmt::kOcAnimObject | fmt::kOcAnimAdditiveBase)) continue;
        if (anim::pathStem(c.skeletonRef) != stem) continue;
        clipPaths_.push_back(e.path().string());
    }
    std::sort(clipPaths_.begin(), clipPaths_.end());
    for (const std::string& p : clipPaths_) clipNames_.push_back(fs::path(p).stem().string());
    batchSel_.assign(clipPaths_.size(), false);
}

void RetargetEditor::loadClip(i32 index) {
    clipIndex_ = index;
    clipOk_ = false;
    if (!inRange(clipPaths_.size(), index)) return;
    std::string why;
    clipOk_ = fmt::loadOcAnim(clipPaths_[static_cast<usize>(index)], clip_, &why) && clip_.valid();
    clipTime_ = 0.0f;
}

void RetargetEditor::drawTop() {
    if (ImGui::Button("Save") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                                  keybinds().pressed(CommandId::AssetSave, ImGui::GetIO()))) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[RetargetEditor] save failed for '{}': {}", path_, why);
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
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s", asset_.source.c_str());
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::InputText("source", buf, sizeof buf)) { asset_.source = buf; markDirty(); }
    touch(ImGui::IsItemEdited());
    if (ImGui::IsItemDeactivatedAfterEdit()) loadSkeletons();
    ImGui::SameLine();
    std::snprintf(buf, sizeof buf, "%s", asset_.target.c_str());
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::InputText("target", buf, sizeof buf)) { asset_.target = buf; markDirty(); }
    touch(ImGui::IsItemEdited());
    if (ImGui::IsItemDeactivatedAfterEdit()) loadSkeletons();
    if (!srcOk_ || !dstOk_) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s%s%s", !srcOk_ ? "source skeleton not found  " : "", !dstOk_ ? "target skeleton not found" : "", "");
}

void RetargetEditor::drawChains() {
    if (ImGui::Button("Add chain") && srcOk_ && dstOk_) addChain("Chain", src_.bones[0].name, src_.bones[0].name, dst_.bones[0].name, dst_.bones[0].name, anim::RtgMode::Fk);
    ImGui::SameLine();
    ImGui::BeginDisabled(!inRange(asset_.chains.size(), selChain_));
    if (ImGui::Button("Remove")) removeChain(selChain_);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Auto-map")) autoMap();
    ImGui::Separator();
    for (usize i = 0; i < asset_.chains.size(); ++i) {
        const RetargetChainDef& c = asset_.chains[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::PushStyleColor(ImGuiCol_Text, chainColor(i));
        const std::string label = c.name + "  (" + (c.mode == anim::RtgMode::Ik ? "ik" : c.mode == anim::RtgMode::None ? "none" : "fk") + ")";
        if (ImGui::Selectable(label.c_str(), selChain_ == static_cast<i32>(i))) selChain_ = static_cast<i32>(i);
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s..%s  ->  %s..%s", c.srcStart.c_str(), c.srcEnd.c_str(), c.dstStart.c_str(), c.dstEnd.c_str());
        ImGui::PopID();
    }
}

void RetargetEditor::drawChainDetails() {
    if (!inRange(asset_.chains.size(), selChain_)) { ImGui::TextDisabled("Select a chain."); return; }
    const f32 dpi = fontScale();
    RetargetChainDef& c = asset_.chains[static_cast<usize>(selChain_)];
    char buf[128];
    std::snprintf(buf, sizeof buf, "%s", c.name.c_str());
    if (ImGui::InputText("name", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue) && chainIndex(asset_, buf) < 0 && buf[0]) {
        pushUndo(); c.name = buf; markDirty();
    }
    if (srcOk_) { boneCombo("source start", src_, c.srcStart); boneCombo("source end", src_, c.srcEnd); }
    if (dstOk_) { boneCombo("target start", dst_, c.dstStart); boneCombo("target end", dst_, c.dstEnd); }
    int mode = static_cast<int>(c.mode);
    if (ImGui::Combo("mode", &mode, "fk\0ik\0none\0")) { pushUndo(); c.mode = static_cast<anim::RtgMode>(mode); markDirty(); }
    int rot = static_cast<int>(c.rotation);
    if (ImGui::Combo("rotation", &rot, "interpolate\0oneToOne\0")) { pushUndo(); c.rotation = static_cast<anim::RtgRotation>(rot); markDirty(); }
    int tr = static_cast<int>(c.translation);
    if (ImGui::Combo("translation", &tr, "none\0scaled\0")) { pushUndo(); c.translation = static_cast<anim::RtgTranslation>(tr); markDirty(); }
    if (c.mode == anim::RtgMode::Ik) {
        int an = static_cast<int>(c.anchor);
        if (ImGui::Combo("anchor", &an, "auto\0root\0start\0")) { pushUndo(); c.anchor = static_cast<anim::RtgAnchor>(an); markDirty(); }
        bool autoScale = c.scale.autoScale;
        if (ImGui::Checkbox("auto scale", &autoScale)) { pushUndo(); c.scale.autoScale = autoScale; markDirty(); }
        if (!c.scale.autoScale) {
            ImGui::SetNextItemWidth(80.0f * dpi);
            touch(ImGui::DragFloat("scale", &c.scale.value, 0.01f, 0.05f, 10.0f));
        }
        int pole = static_cast<int>(c.pole);
        if (ImGui::Combo("pole", &pole, "fk\0none\0")) { pushUndo(); c.pole = static_cast<anim::RtgPole>(pole); markDirty(); }
        bool tipRot = c.tipRotation;
        if (ImGui::Checkbox("tip rotation", &tipRot)) { pushUndo(); c.tipRotation = tipRot; markDirty(); }
    }
}

void RetargetEditor::drawRoot() {
    const f32 dpi = fontScale();
    if (srcOk_) { boneCombo("source pelvis", src_, asset_.root.srcBone); boneCombo("source root", src_, asset_.rootMotion.srcBone); }
    if (dstOk_) { boneCombo("target pelvis", dst_, asset_.root.dstBone); boneCombo("target root", dst_, asset_.rootMotion.dstBone); }
    const RtgScales sc = scales();
    ImGui::TextDisabled("computed scales: horizontal %.3f, vertical %.3f", sc.scaleH, sc.scaleV);
    auto scaleRow = [&](const char* label, anim::RtgScale& s) {
        ImGui::PushID(label);
        bool autoScale = s.autoScale;
        if (ImGui::Checkbox(label, &autoScale)) { pushUndo(); s.autoScale = autoScale; markDirty(); }
        if (!s.autoScale) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f * dpi);
            touch(ImGui::DragFloat("##v", &s.value, 0.01f, 0.05f, 10.0f));
        }
        ImGui::PopID();
    };
    scaleRow("auto horizontal scale", asset_.root.scaleH);
    scaleRow("auto vertical scale", asset_.root.scaleV);
    scaleRow("auto root-motion scale", asset_.rootMotion.scale);
    float off[3] = {asset_.root.offsetCm.x, asset_.root.offsetCm.y, asset_.root.offsetCm.z};
    if (ImGui::DragFloat3("pelvis offset cm", off, 0.1f)) { if (!gesture_) { history_.push(before_); gesture_ = true; } asset_.root.offsetCm = Vec3{off[0], off[1], off[2]}; markDirty(); }
    if (ImGui::IsItemDeactivated()) endGesture_ = true;
    int um = static_cast<int>(asset_.unmapped);
    if (ImGui::Combo("unmapped bones", &um, "rest\0inherit\0")) { pushUndo(); asset_.unmapped = static_cast<anim::RtgUnmapped>(um); markDirty(); }
}

void RetargetEditor::drawDraft() {
    if (ImGui::Button("Run Auto-map")) autoMap();
    if (draft_.asset.chains.empty()) {
        ImGui::TextDisabled("Auto-map proposes chains from bone names. Nothing is applied until you accept it.");
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Accept all")) accept_.assign(draft_.asset.chains.size(), true);
    ImGui::SameLine();
    if (ImGui::Button("Accept none")) accept_.assign(draft_.asset.chains.size(), false);
    ImGui::SameLine();
    if (ImGui::Button("Apply accepted")) applyDraft(true);
    ImGui::Separator();
    accept_.resize(draft_.asset.chains.size(), false);
    for (usize i = 0; i < draft_.asset.chains.size(); ++i) {
        const RetargetChainDef& c = draft_.asset.chains[i];
        ImGui::PushID(static_cast<int>(i));
        bool on = accept_[i];
        if (ImGui::Checkbox("##acc", &on)) accept_[i] = on;
        ImGui::SameLine();
        const anim::RtgConfidence conf = draft_.confidence[i];
        ImVec4 col = conf == anim::RtgConfidence::Exact ? ImVec4(0.5f, 0.9f, 0.5f, 1) : conf == anim::RtgConfidence::Synonym ? ImVec4(0.95f, 0.85f, 0.4f, 1) : ImVec4(0.95f, 0.55f, 0.4f, 1);
        ImGui::TextColored(col, "%-10s", confidenceLabel(conf));
        ImGui::SameLine();
        ImGui::Text("%s: %s..%s -> %s..%s", c.name.c_str(), c.srcStart.c_str(), c.srcEnd.c_str(), c.dstStart.c_str(), c.dstEnd.c_str());
        ImGui::PopID();
    }
}

void RetargetEditor::drawBatch() {
    if (!clipsScanned_) scanClips();
    if (!suffixSet_) {
        std::snprintf(suffix_, sizeof suffix_, "_%s", fs::path(asset_.target).stem().string().c_str());
        if (!dstPath_.empty()) std::snprintf(outDir_, sizeof outDir_, "%s", (fs::path(dstPath_).parent_path() / "Retargeted").string().c_str());
        suffixSet_ = true;
    }
    if (ImGui::SmallButton("Refresh")) scanClips();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu clip(s) for this source skeleton", clipPaths_.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("All")) batchSel_.assign(clipPaths_.size(), true);
    ImGui::SameLine();
    if (ImGui::SmallButton("None")) batchSel_.assign(clipPaths_.size(), false);
    if (ImGui::BeginChild("##batchclips", ImVec2(0, 140.0f), true)) {
        batchSel_.resize(clipPaths_.size(), false);
        for (usize i = 0; i < clipPaths_.size(); ++i) {
            bool on = batchSel_[i];
            if (ImGui::Checkbox(clipNames_[i].c_str(), &on)) batchSel_[i] = on;
        }
    }
    ImGui::EndChild();
    ImGui::InputText("output folder", outDir_, sizeof outDir_);
    ImGui::InputText("suffix", suffix_, sizeof suffix_);
    ImGui::SetNextItemWidth(80.0f);
    ImGui::DragFloat("fps", &opt_.fps, 0.5f, 1.0f, 120.0f, "%.0f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::DragFloat("rot tol deg", &opt_.rotToleranceDeg, 0.005f, 0.0f, 5.0f, "%.3f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::DragFloat("pos tol cm", &opt_.posToleranceCm, 0.005f, 0.0f, 5.0f, "%.3f");
    ImGui::Checkbox("reduce keys", &opt_.reduceKeys);
    ImGui::SameLine();
    ImGui::Checkbox("overwrite", &overwrite_);
    usize n = 0;
    for (const bool b : batchSel_) n += b ? 1u : 0u;
    ImGui::BeginDisabled(n == 0 || !srcOk_ || !dstOk_);
    const std::string label = "Retarget " + std::to_string(n) + " clips";
    if (ImGui::Button(label.c_str())) {
        std::vector<std::string> chosen;
        for (usize i = 0; i < clipPaths_.size() && i < batchSel_.size(); ++i)
            if (batchSel_[i]) chosen.push_back(clipPaths_[i]);
        batch_ = runBatch(chosen, outDir_, suffix_, opt_, overwrite_);
        batchRan_ = true;
    }
    ImGui::EndDisabled();
    if (batchRan_) {
        ImGui::Text("written %u, skipped %u, failed %u", batch_.written, batch_.skipped, batch_.failed);
        for (const std::string& f : batch_.files) ImGui::BulletText("%s", f.c_str());
        for (const anim::RetargetIssue& i : batch_.issues) {
            const ImVec4 col = i.kind == anim::RetargetIssue::Kind::Error ? ImVec4(1, 0.45f, 0.4f, 1) : i.kind == anim::RetargetIssue::Kind::Warn ? ImVec4(1, 0.8f, 0.4f, 1) : ImVec4(0.7f, 0.7f, 0.75f, 1);
            ImGui::TextColored(col, "%s", i.text.c_str());
        }
    }
}

void RetargetEditor::drawProblems() {
    const std::vector<std::string> p = problems();
    if (p.empty()) ImGui::TextDisabled("No problems.");
    for (const std::string& s : p) ImGui::BulletText("%s", s.c_str());
    for (const anim::RetargetIssue& i : bindIssues_)
        if (i.kind != anim::RetargetIssue::Kind::Info) ImGui::BulletText("bind: %s", i.text.c_str());
}

void RetargetEditor::drawViews(float width, float height) {
    if (!srcOk_ || !dstOk_) { ImGui::TextDisabled("Both skeletons must load to preview."); return; }
    if (bindStale_) rebind();
    if (!clipsScanned_) scanClips();

    anim::Pose srcPose;
    anim::restPose(src_, srcPose);
    if (clipOk_) {
        if (clipPlay_) clipTime_ += ImGui::GetIO().DeltaTime;
        anim::sampleAnimation(clip_, clip_.duration > 0.0f ? std::fmod(clipTime_, clip_.duration) : 0.0f, srcPose);
    }
    anim::Pose dstPose;
    anim::restPose(dst_, dstPose);
    if (rtOk_) rt_.retarget(srcPose, dstPose);

    RigPreviewIn a, b;
    a.skel = &src_; a.pose = &srcPose;
    b.skel = &dst_; b.pose = &dstPose;
    a.boneColors.assign(src_.bones.size(), IM_COL32(120, 122, 132, 255));
    b.boneColors.assign(dst_.bones.size(), IM_COL32(120, 122, 132, 255));
    for (usize ci = 0; ci < asset_.chains.size(); ++ci) {
        const RetargetChainDef& c = asset_.chains[ci];
        const unsigned col = static_cast<i32>(ci) == selChain_ ? IM_COL32(255, 255, 255, 255) : chainColor(ci);
        const std::vector<u32> sp = pathOf(src_, boneIndex(src_, c.srcStart), boneIndex(src_, c.srcEnd));
        const std::vector<u32> dp = pathOf(dst_, boneIndex(dst_, c.dstStart), boneIndex(dst_, c.dstEnd));
        for (const u32 i : sp) a.boneColors[i] = col;
        for (const u32 i : dp) b.boneColors[i] = col;
    }
    a.caption = "source";
    b.caption = "target (retargeted)";
    if (!framed_) {
        std::vector<Mat4> m;
        anim::poseToModel(src_, srcPose, m);
        rigFramePose(srcView_, m);
        anim::poseToModel(dst_, dstPose, m);
        rigFramePose(dstView_, m);
        framed_ = true;
    }
    const f32 half = std::max(80.0f, (width - 8.0f) * 0.5f);
    drawRigPreview("##srcview", half, height, srcView_, a);
    ImGui::SameLine();
    drawRigPreview("##dstview", half, height, dstView_, b);
}

void RetargetEditor::draw(Engine& e) {
    (void)e;
    if (!loaded_) {
        ImGui::TextWrapped("This file could not be read: %s", loadError_.c_str());
        return;
    }
    before_ = asset_;
    drawTop();
    ImGui::Separator();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const f32 leftW = std::max(200.0f, avail.x * 0.22f), rightW = std::max(320.0f, avail.x * 0.34f);
    const f32 midW = std::max(200.0f, avail.x - leftW - rightW - 16.0f);
    if (ImGui::BeginChild("##left", ImVec2(leftW, avail.y), true)) drawChains();
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##mid", ImVec2(midW, avail.y), true)) {
        if (srcOk_) {
            if (!clipsScanned_) scanClips();
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::BeginCombo("##clip", clipIndex_ >= 0 ? clipNames_[static_cast<usize>(clipIndex_)].c_str() : "rest pose")) {
                if (ImGui::Selectable("rest pose", clipIndex_ < 0)) { clipIndex_ = -1; clipOk_ = false; }
                for (usize i = 0; i < clipNames_.size(); ++i)
                    if (ImGui::Selectable(clipNames_[i].c_str(), static_cast<i32>(i) == clipIndex_)) loadClip(static_cast<i32>(i));
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::Checkbox("play", &clipPlay_);
            const RtgScales sc = scales();
            ImGui::SameLine();
            ImGui::TextDisabled("scale %.3f / %.3f", sc.scaleH, sc.scaleV);
        }
        const ImVec2 room = ImGui::GetContentRegionAvail();
        drawViews(room.x, std::max(120.0f, room.y));
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##right", ImVec2(rightW, avail.y), true)) {
        if (ImGui::BeginTabBar("##rtgtabs")) {
            if (ImGui::BeginTabItem("Chain")) { drawChainDetails(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Root")) { drawRoot(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Auto-map")) { drawDraft(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Batch")) { drawBatch(); ImGui::EndTabItem(); }
            const std::string pt = "Problems (" + std::to_string(problems().size()) + ")###problems";
            if (ImGui::BeginTabItem(pt.c_str())) { drawProblems(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }
    ImGui::EndChild();
    if (endGesture_ && !ImGui::IsMouseDown(0)) { gesture_ = false; endGesture_ = false; }
}

#else   // AVER_WITH_IMGUI

void RetargetEditor::draw(Engine& e) { (void)e; }

#endif  // AVER_WITH_IMGUI

std::unique_ptr<AssetEditor> makeRetargetEditor(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".ocretarget") return nullptr;
    return std::make_unique<RetargetEditor>(path);
}

} // namespace aver::editor
