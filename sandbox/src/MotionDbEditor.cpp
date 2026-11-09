// The .ocmotion editor tab. See the header for the SandboxApp hook and for why the edits are free
// functions rather than members.

#include "MotionDbEditor.hpp"
#include "EditorChrome.hpp"
#include "EditorKeybinds.hpp"

#include "aver/anim/MotionMatcher.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcAnim.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#if AVER_WITH_IMGUI
#  include "imgui.h"
#endif

namespace aver::editor {

using anim::MotionAsset;
using anim::MotionClipEntry;
using anim::MotionTagRange;

namespace {

namespace fs = std::filesystem;

bool inRange(usize n, i32 i) { return i >= 0 && static_cast<usize>(i) < n; }

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

std::vector<u8> slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void replaceName(std::vector<std::string>& v, const std::string& from, const std::string& to) {
    for (std::string& s : v)
        if (s == from) s = to;
}

void eraseName(std::vector<std::string>& v, const std::string& name) {
    v.erase(std::remove(v.begin(), v.end(), name), v.end());
}

constexpr f32 kMinRangeLength = 1.0f / 30.0f;

} // namespace

// ------------------------------------------------------------------------------- edits ----------

MotionAsset motionStarter() {
    MotionAsset a;
    a.name = "Motion Database";
    return a;
}

i32 mdbAddClip(MotionAsset& a, const std::string& clipRef, i32 at) {
    if (clipRef.empty()) return -1;
    MotionClipEntry e;
    e.clip = clipRef;
    const i32 idx = inRange(a.clips.size(), at) ? at : static_cast<i32>(a.clips.size());
    a.clips.insert(a.clips.begin() + idx, std::move(e));
    return idx;
}

bool mdbRemoveClip(MotionAsset& a, i32 index) {
    if (!inRange(a.clips.size(), index)) return false;
    a.clips.erase(a.clips.begin() + index);
    return true;
}

i32 mdbMoveClip(MotionAsset& a, i32 index, i32 delta) {
    if (!inRange(a.clips.size(), index)) return -1;
    const i32 to = std::clamp(index + delta, 0, static_cast<i32>(a.clips.size()) - 1);
    if (to == index) return index;
    MotionClipEntry e = std::move(a.clips[static_cast<usize>(index)]);
    a.clips.erase(a.clips.begin() + index);
    a.clips.insert(a.clips.begin() + to, std::move(e));
    return to;
}

i32 mdbAddTag(MotionAsset& a, const std::string& name) {
    if (name.empty() || a.tags.size() >= anim::kMaxMotionTags || a.tagIndex(name) >= 0) return -1;
    a.tags.push_back(name);
    return static_cast<i32>(a.tags.size()) - 1;
}

bool mdbRemoveTag(MotionAsset& a, i32 index) {
    if (!inRange(a.tags.size(), index)) return false;
    const std::string gone = a.tags[static_cast<usize>(index)];
    a.tags.erase(a.tags.begin() + index);
    for (MotionClipEntry& e : a.clips) {
        eraseName(e.tags, gone);
        for (MotionTagRange& r : e.ranges) eraseName(r.tags, gone);
    }
    return true;
}

bool mdbRenameTag(MotionAsset& a, i32 index, const std::string& name) {
    if (!inRange(a.tags.size(), index) || name.empty()) return false;
    const std::string old = a.tags[static_cast<usize>(index)];
    if (old == name) return true;
    if (a.tagIndex(name) >= 0) return false;
    a.tags[static_cast<usize>(index)] = name;
    for (MotionClipEntry& e : a.clips) {
        replaceName(e.tags, old, name);
        for (MotionTagRange& r : e.ranges) replaceName(r.tags, old, name);
    }
    return true;
}

i32 mdbAddRange(MotionAsset& a, i32 clip, f32 start, f32 end) {
    if (!inRange(a.clips.size(), clip) || !std::isfinite(start) || !std::isfinite(end)) return -1;
    start = std::max(start, 0.0f);
    if (end >= 0.0f && end <= start) return -1;
    MotionTagRange r;
    r.start = start;
    r.end = end;
    auto& ranges = a.clips[static_cast<usize>(clip)].ranges;
    ranges.push_back(std::move(r));
    return static_cast<i32>(ranges.size()) - 1;
}

bool mdbRemoveRange(MotionAsset& a, i32 clip, i32 range) {
    if (!inRange(a.clips.size(), clip)) return false;
    auto& ranges = a.clips[static_cast<usize>(clip)].ranges;
    if (!inRange(ranges.size(), range)) return false;
    ranges.erase(ranges.begin() + range);
    return true;
}

bool mdbMoveRange(MotionAsset& a, i32 clip, i32 range, f32 start, f32 end) {
    if (!inRange(a.clips.size(), clip) || !std::isfinite(start) || !std::isfinite(end)) return false;
    auto& ranges = a.clips[static_cast<usize>(clip)].ranges;
    if (!inRange(ranges.size(), range)) return false;
    MotionTagRange& r = ranges[static_cast<usize>(range)];
    r.start = std::max(start, 0.0f);
    r.end = end < 0.0f ? -1.0f : std::max(end, r.start + kMinRangeLength);
    return true;
}

i32 mdbAddBone(MotionAsset& a, const std::string& bone, f32 pos, f32 vel) {
    if (bone.empty()) return -1;
    for (const anim::MotionBoneFeature& b : a.bones)
        if (b.bone == bone) return -1;
    a.bones.push_back({bone, pos, vel});
    return static_cast<i32>(a.bones.size()) - 1;
}

bool mdbRemoveBone(MotionAsset& a, i32 index) {
    if (!inRange(a.bones.size(), index)) return false;
    a.bones.erase(a.bones.begin() + index);
    return true;
}

u32 mdbSetTrajectoryTimes(MotionAsset& a, bool future, const std::vector<f32>& times) {
    std::vector<f32> keep;
    for (const f32 t : times)
        if (std::isfinite(t) && (future ? t > 0.0f : t < 0.0f)) keep.push_back(t);
    std::sort(keep.begin(), keep.end(), [](f32 x, f32 y) { return std::fabs(x) < std::fabs(y); });
    keep.erase(std::unique(keep.begin(), keep.end(), [](f32 x, f32 y) { return std::fabs(x - y) < 1e-4f; }), keep.end());
    if (keep.size() > anim::kMaxTrajectorySamples) keep.resize(anim::kMaxTrajectorySamples);
    (future ? a.futureTimes : a.pastTimes) = keep;
    return static_cast<u32>(keep.size());
}

f32 mdbSnap(f32 t, f32 step) {
    if (!(step > 0.0f)) return t;
    return std::round(t / step) * step;
}

std::string mdbContentRootOf(const std::string& assetPath) {
    const fs::path p(assetPath);
    for (fs::path d = p.parent_path(); !d.empty(); d = d.parent_path()) {
        if (iequals(d.filename().string(), "Content")) return d.string();
        if (d == d.parent_path()) break;
    }
    return p.parent_path().string();
}

std::string mdbClipRefFromPath(const std::string& assetPath, const std::string& clipPath) {
    const fs::path root(mdbContentRootOf(assetPath));
    const fs::path clip(clipPath);
    fs::path rel = clip.lexically_relative(root);
    const std::string first = rel.empty() ? std::string() : rel.begin()->string();
    if (rel.empty() || first == "..") rel = clip.filename();
    return rel.generic_string();
}

std::string mdbResolveClipPath(const std::string& assetPath, const std::string& ref) {
    if (ref.empty()) return {};
    const fs::path r(ref);
    if (r.is_absolute()) return isFile(r) ? r.string() : std::string();
    const fs::path root(mdbContentRootOf(assetPath));
    const fs::path candidates[] = {root / r, fs::path(assetPath).parent_path() / r, root.parent_path() / r};
    for (const fs::path& c : candidates)
        if (isFile(c)) return c.string();
    return {};
}

std::string mdbSidecarPath(const std::string& assetPath) {
    return fs::path(assetPath).replace_extension(".ocmbake").string();
}

MdbPreview mdbBuildPreview(const MotionAsset& a, const std::string& assetPath) {
    MdbPreview r;
    std::string why;
    if (!a.valid(&why)) { r.why = "invalid asset: " + why; return r; }

    const std::string skelPath = mdbResolveClipPath(assetPath, a.skeleton);
    fmt::OcSkeleton skel;
    if (skelPath.empty() || !fmt::loadOcSkel(skelPath, skel, &why)) {
        r.why = "skeleton '" + a.skeleton + "' could not be loaded" + (why.empty() ? std::string() : ": " + why);
        return r;
    }
    std::map<std::string, fmt::OcAnimation> clips;
    std::vector<std::string> refs, paths;
    for (const MotionClipEntry& e : a.clips) {
        if (std::find(refs.begin(), refs.end(), e.clip) != refs.end()) continue;
        refs.push_back(e.clip);
        paths.push_back(mdbResolveClipPath(assetPath, e.clip));
        fmt::OcAnimation c;
        if (!paths.back().empty() && fmt::loadOcAnim(paths.back(), c, nullptr)) clips[e.clip] = std::move(c);
    }
    auto resolve = [&clips](const std::string& ref) -> const fmt::OcAnimation* {
        const auto it = clips.find(ref);
        return it == clips.end() ? nullptr : &it->second;
    };

    r.db = std::make_unique<anim::PoseDatabase>();
    if (!anim::buildPoseDatabase(a, skel, resolve, *r.db, &r.report, &r.why)) {
        r.db.reset();
        return r;
    }
    std::vector<std::vector<u8>> clipBytes;
    for (const std::string& p : paths) clipBytes.push_back(p.empty() ? std::vector<u8>{} : slurp(p));
    r.sourceKey = anim::motionSourceKey(a, slurp(skelPath), clipBytes);
    r.ok = true;
    return r;
}

bool mdbBake(const MotionAsset& a, const std::string& assetPath, MdbPreview& out) {
    out = mdbBuildPreview(a, assetPath);
    if (!out.ok) return false;
    std::string why;
    if (!anim::savePoseDatabase(mdbSidecarPath(assetPath), *out.db, out.sourceKey, &why))
        out.report.warnings.push_back("could not write " + mdbSidecarPath(assetPath) + ": " + why);
    return true;
}

MdbLabResult mdbRunQuery(const anim::PoseDatabase& db, const MdbLabInput& in, u32 topK) {
    MdbLabResult res;
    const MotionAsset& d = db.defaults();

    // A second of history at the current speed along +X, then the wanted velocity as the intent.
    anim::TrajectoryPredictor tp;
    constexpr f32 dt = 1.0f / 60.0f;
    for (int i = 0; i <= 60; ++i)
        tp.observe(Vec3{in.currentSpeed * dt * static_cast<f32>(i), 0.0f, 0.0f}, 0.0f, i ? dt : 0.0f, 1.0f);
    const f32 h = radians(in.wantedHeadingDeg);
    tp.setIntent(in.wantedSpeed * std::cos(h), in.wantedSpeed * std::sin(h), false, 0.0f, d.spring.faceSpeedMin);
    tp.fill({d.spring.velocityHalfLife, d.spring.turnHalfLife}, db.futureTimes(), db.pastTimes(), res.query);
    res.query.require = in.require;
    res.query.exclude = in.exclude;

    std::vector<f32> q;
    anim::motionQueryVector(db, res.query, nullptr, q);
    const anim::SearchFilter filter{in.require, in.exclude};

    std::vector<anim::SearchHit> top;
    db.searchTopK(q.data(), filter, topK, top);
    for (const anim::SearchHit& hit : top) {
        MdbLabHit lh;
        lh.row = hit.row;
        lh.clip = db.ranges()[db.meta(hit.row).range].clip;
        lh.time = db.timeOf(hit.row);
        lh.cost = hit.cost;
        for (u32 g = 0; g < db.groups().size(); ++g) lh.groupCosts.push_back(db.groupCost(q.data(), hit.row, g));
        res.hits.push_back(std::move(lh));
    }

    auto micros = [&](bool kd) {
        constexpr int reps = 20;
        const auto t0 = std::chrono::steady_clock::now();
        u32 sink = 0;
        for (int i = 0; i < reps; ++i) sink += kd ? db.searchKd(q.data(), filter).row : db.searchLinear(q.data(), filter).row;
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / reps;
        return sink == 0xFFFFFFFFu ? us + 1e-9 : us;   // keeps the loop observable
    };
    res.linearMicros = micros(false);
    if (db.hasKd()) {
        res.kdMicros = micros(true);
        res.sameAnswer = db.searchKd(q.data(), filter).row == db.searchLinear(q.data(), filter).row;
    }
    return res;
}

// ------------------------------------------------------------------------------- the tab ----------

MotionDbEditor::MotionDbEditor(std::string path) : path_(std::move(path)) { loadFromDisk(); }

void MotionDbEditor::loadFromDisk() {
    std::string why;
    MotionAsset a;
    if (!anim::loadMotionAsset(path_, a, &why)) {
        loaded_ = false;
        loadError_ = why;
        return;
    }
    asset_ = std::move(a);
    loaded_ = true;
    loadError_.clear();
    dirty_ = false;
    selClip_ = -1;
    previewStale_ = true;
    history_.clear();
    durations_.clear();
    bake_ = MdbPreview{};
    labResult_ = MdbLabResult{};
}

std::string MotionDbEditor::title() const {
    return fs::path(path_).filename().string() + "###mdb:" + path_;
}

bool MotionDbEditor::save(std::string* why) {
    if (!loaded_) {
        if (why) *why = "cannot save: file failed to load (" + loadError_ + ")";
        return false;
    }
    if (!anim::saveMotionAsset(path_, asset_, why)) return false;
    dirty_ = false;
    return true;
}

void MotionDbEditor::onFileChanged() {
    if (dirty_) {
        AVER_WARN("[MotionDbEditor] '{}' changed on disk, but this tab has unsaved edits -- keeping them", path_);
        return;
    }
    loadFromDisk();
}

void MotionDbEditor::undo() {
    if (!history_.undo(asset_)) return;
    dirty_ = true;
    previewStale_ = true;
    selClip_ = -1;
}

void MotionDbEditor::redo() {
    if (!history_.redo(asset_)) return;
    dirty_ = true;
    previewStale_ = true;
    selClip_ = -1;
}

i32 MotionDbEditor::addClip(const std::string& clipRef) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 idx = mdbAddClip(asset_, clipRef);
    if (idx < 0) { history_.cancelPush(); return -1; }
    selClip_ = idx;
    markDirty();
    return idx;
}

bool MotionDbEditor::removeClip(i32 index) {
    if (!loaded_) return false;
    pushUndo();
    if (!mdbRemoveClip(asset_, index)) { history_.cancelPush(); return false; }
    selClip_ = -1;
    markDirty();
    return true;
}

i32 MotionDbEditor::moveClip(i32 index, i32 delta) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 to = mdbMoveClip(asset_, index, delta);
    if (to < 0 || to == index) { history_.cancelPush(); return to; }
    selClip_ = to;
    markDirty();
    return to;
}

i32 MotionDbEditor::addTag(const std::string& name) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 idx = mdbAddTag(asset_, name);
    if (idx < 0) { history_.cancelPush(); return -1; }
    markDirty();
    return idx;
}

bool MotionDbEditor::removeTag(i32 index) {
    if (!loaded_) return false;
    pushUndo();
    if (!mdbRemoveTag(asset_, index)) { history_.cancelPush(); return false; }
    markDirty();
    return true;
}

bool MotionDbEditor::renameTag(i32 index, const std::string& name) {
    if (!loaded_) return false;
    pushUndo();
    if (!mdbRenameTag(asset_, index, name)) { history_.cancelPush(); return false; }
    markDirty();
    return true;
}

i32 MotionDbEditor::addRange(i32 clip, f32 start, f32 end) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 idx = mdbAddRange(asset_, clip, start, end);
    if (idx < 0) { history_.cancelPush(); return -1; }
    markDirty();
    return idx;
}

bool MotionDbEditor::removeRange(i32 clip, i32 range) {
    if (!loaded_) return false;
    pushUndo();
    if (!mdbRemoveRange(asset_, clip, range)) { history_.cancelPush(); return false; }
    markDirty();
    return true;
}

i32 MotionDbEditor::addBone(const std::string& bone) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 idx = mdbAddBone(asset_, bone);
    if (idx < 0) { history_.cancelPush(); return -1; }
    markDirty();
    return idx;
}

bool MotionDbEditor::removeBone(i32 index) {
    if (!loaded_) return false;
    pushUndo();
    if (!mdbRemoveBone(asset_, index)) { history_.cancelPush(); return false; }
    markDirty();
    return true;
}

bool MotionDbEditor::setTrajectoryTimes(bool future, const std::vector<f32>& times) {
    if (!loaded_) return false;
    pushUndo();
    mdbSetTrajectoryTimes(asset_, future, times);
    markDirty();
    return true;
}

bool MotionDbEditor::bake(std::string* why) {
    if (!loaded_) {
        if (why) *why = "the file failed to load";
        return false;
    }
    if (dirty_ && !save(why)) return false;   // the sidecar's key hashes the saved text
    if (!mdbBake(asset_, path_, bake_)) {
        if (why) *why = bake_.why;
        return false;
    }
    previewStale_ = false;
    labResult_ = MdbLabResult{};
    return true;
}

f32 MotionDbEditor::clipDuration(const std::string& clipRef) {
    const auto it = durations_.find(clipRef);
    if (it != durations_.end()) return it->second;
    f32 d = 0.0f;
    fmt::OcAnimation c;
    const std::string file = mdbResolveClipPath(path_, clipRef);
    if (!file.empty() && fmt::loadOcAnim(file, c, nullptr)) d = c.duration;
    return durations_[clipRef] = d;
}

#if AVER_WITH_IMGUI

namespace {

// The Content Browser sends its selection as newline-separated paths under one of these.
constexpr const char* kPayloadSet = "AVER_CB_MOVE_SET";
constexpr const char* kPayloadOne = "AVER_ASSET_PATH";

std::vector<std::string> payloadPaths(const ImGuiPayload* p) {
    std::vector<std::string> out;
    if (!p || !p->Data) return out;
    const std::string blob(static_cast<const char*>(p->Data), p->DataSize > 0 ? static_cast<usize>(p->DataSize - 1) : usize(0));
    usize from = 0;
    while (from <= blob.size()) {
        const usize nl = blob.find('\n', from);
        const std::string one = blob.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
        if (!one.empty()) out.push_back(one);
        if (nl == std::string::npos) break;
        from = nl + 1;
    }
    return out;
}

bool hasExt(const std::string& path, const char* want) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == want;
}

std::string timesToText(const std::vector<f32>& v) {
    std::string s;
    for (usize i = 0; i < v.size(); ++i) {
        char b[32];
        std::snprintf(b, sizeof b, "%s%g", i ? ", " : "", static_cast<double>(v[i]));
        s += b;
    }
    return s;
}

std::vector<f32> textToTimes(const std::string& text) {
    std::vector<f32> out;
    std::stringstream ss(text);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        try { out.push_back(std::stof(tok)); } catch (...) {}
    }
    return out;
}

bool hasTag(const std::vector<std::string>& v, const std::string& t) {
    return std::find(v.begin(), v.end(), t) != v.end();
}

} // namespace

void MotionDbEditor::touch(bool changed) {
    if (changed) {
        if (!gesture_) { history_.push(before_); gesture_ = true; }
        markDirty();
    }
    if (ImGui::IsItemDeactivated()) endGesture_ = true;
}

void MotionDbEditor::drawHeader() {
    const f32 dpi = fontScale();
    char buf[256];
    auto text = [&](const char* label, std::string& v) {
        std::snprintf(buf, sizeof buf, "%s", v.c_str());
        const bool c = ImGui::InputText(label, buf, sizeof buf);
        if (c) v = buf;
        touch(c);
        return c;
    };
    text("Name", asset_.name);
    if (text("Skeleton", asset_.skeleton)) durations_.clear();
    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kPayloadSet);
        if (!payload) payload = ImGui::AcceptDragDropPayload(kPayloadOne);
        for (const std::string& one : payloadPaths(payload))
            if (hasExt(one, ".ocskel")) { pushUndo(); asset_.skeleton = mdbClipRefFromPath(path_, one); markDirty(); }
        ImGui::EndDragDropTarget();
    }
    ImGui::SetItemTooltip("Content path of the .ocskel every clip was authored against. Drop one from the Content Browser.");
    text("Root bone", asset_.rootBone);
    ImGui::SetItemTooltip("Empty = the skeleton's root bone. The bone whose travel and heading define the trajectory.");

    int rate = static_cast<int>(asset_.sampleRate);
    ImGui::SetNextItemWidth(90.0f * dpi);
    if (ImGui::DragInt("Sample rate", &rate, 1.0f, 5, 120, "%d Hz")) { asset_.sampleRate = static_cast<u32>(rate); }
    touch(ImGui::IsItemEdited());

    if (::aver::editor::sectionHeader("Search, trajectory, inertialization")) {
        anim::MotionSearchDefaults& s = asset_.search;
        ImGui::SetNextItemWidth(90.0f * dpi);
        touch(ImGui::DragFloat("Interval", &s.interval, 0.005f, 0.01f, 5.0f, "%.3f s"));
        ImGui::SetItemTooltip("Seconds between searches.");
        ImGui::SetNextItemWidth(90.0f * dpi);
        touch(ImGui::DragFloat("Switch margin", &s.switchMargin, 0.01f, 0.0f, 100.0f, "%.2f"));
        ImGui::SetItemTooltip("A candidate must beat the playing frame by this much (normalised cost) to be taken.");
        ImGui::SetNextItemWidth(90.0f * dpi);
        touch(ImGui::DragFloat("Accept cost", &s.acceptCost, 0.05f, -1.0f, 1000.0f, "%.2f"));
        ImGui::SetItemTooltip("Above this a valid playing frame is kept whatever the best hit. -1 = off.");
        ImGui::SetNextItemWidth(90.0f * dpi);
        touch(ImGui::DragFloat("Continuity window", &s.continuityWindow, 0.01f, 0.0f, 5.0f, "%.2f s"));
        int accel = static_cast<int>(s.accel);
        ImGui::SetNextItemWidth(110.0f * dpi);
        if (ImGui::Combo("Accel", &accel, "auto\0linear\0kd tree\0")) { pushUndo(); s.accel = static_cast<anim::SearchAccel>(accel); markDirty(); }
        int kd = static_cast<int>(s.kdMinRows);
        ImGui::SetNextItemWidth(110.0f * dpi);
        if (ImGui::DragInt("KD min rows", &kd, 100.0f, 1024, 10000000)) s.kdMinRows = static_cast<u32>(kd);
        touch(ImGui::IsItemEdited());

        anim::MotionSpringDefaults& sp = asset_.spring;
        ImGui::SetNextItemWidth(90.0f * dpi);
        touch(ImGui::DragFloat("Velocity half-life", &sp.velocityHalfLife, 0.005f, 0.01f, 5.0f, "%.3f s"));
        ImGui::SetNextItemWidth(90.0f * dpi);
        touch(ImGui::DragFloat("Turn half-life", &sp.turnHalfLife, 0.005f, 0.01f, 5.0f, "%.3f s"));
        ImGui::SetNextItemWidth(90.0f * dpi);
        touch(ImGui::DragFloat("History", &sp.historySeconds, 0.05f, 0.1f, 10.0f, "%.2f s"));
        ImGui::SetNextItemWidth(90.0f * dpi);
        touch(ImGui::DragFloat("Face speed min", &sp.faceSpeedMin, 0.5f, 0.0f, 500.0f, "%.1f cm/s"));
        ImGui::SetNextItemWidth(90.0f * dpi);
        touch(ImGui::DragFloat("Inertialization half-life", &asset_.inertHalfLife, 0.005f, 0.01f, 2.0f, "%.3f s"));
    }
}

void MotionDbEditor::drawTags() {
    const f32 dpi = fontScale();
    ImGui::SeparatorText("Tags");
    char buf[128];
    int removeAt = -1;
    for (usize i = 0; i < asset_.tags.size(); ++i) {
        ImGui::PushID(static_cast<int>(i) + 100);
        std::snprintf(buf, sizeof buf, "%s", asset_.tags[i].c_str());
        ImGui::SetNextItemWidth(150.0f * dpi);
        if (ImGui::InputText("##tag", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) renameTag(static_cast<i32>(i), buf);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeAt = static_cast<int>(i);
        ImGui::PopID();
    }
    if (removeAt >= 0) removeTag(removeAt);
    static char newTag[128] = "";
    ImGui::SetNextItemWidth(150.0f * dpi);
    const bool enter = ImGui::InputText("##newtag", newTag, sizeof newTag, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Add tag") || enter) && newTag[0]) {
        addTag(newTag);
        newTag[0] = 0;
    }
    ImGui::SetItemTooltip("Tags are declared once (at most 32) and used by clips and tag ranges; gameplay requires or excludes them.");
}

void MotionDbEditor::drawFeatures() {
    const f32 dpi = fontScale();
    ImGui::SeparatorText("Features");
    char buf[128];
    int removeAt = -1;
    for (usize i = 0; i < asset_.bones.size(); ++i) {
        anim::MotionBoneFeature& b = asset_.bones[i];
        ImGui::PushID(static_cast<int>(i) + 200);
        std::snprintf(buf, sizeof buf, "%s", b.bone.c_str());
        ImGui::SetNextItemWidth(130.0f * dpi);
        const bool c = ImGui::InputText("##bone", buf, sizeof buf);
        if (c) b.bone = buf;
        touch(c);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0f * dpi);
        touch(ImGui::DragFloat("pos", &b.pos, 0.01f, 0.0f, 10.0f, "%.2f"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0f * dpi);
        touch(ImGui::DragFloat("vel", &b.vel, 0.01f, 0.0f, 10.0f, "%.2f"));
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeAt = static_cast<int>(i);
        ImGui::PopID();
    }
    if (removeAt >= 0) removeBone(removeAt);
    static char newBone[128] = "";
    ImGui::SetNextItemWidth(130.0f * dpi);
    const bool enter = ImGui::InputText("##newbone", newBone, sizeof newBone, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Add bone") || enter) && newBone[0]) {
        addBone(newBone);
        newBone[0] = 0;
    }
    ImGui::SetItemTooltip("Feet and hips are the usual choices. A weight of 0 leaves that part out of the match.");

    ImGui::SetNextItemWidth(70.0f * dpi);
    touch(ImGui::DragFloat("Root velocity", &asset_.rootVelocity, 0.01f, 0.0f, 10.0f, "%.2f"));

    if (!timesInit_) {
        timesFuture_ = timesToText(asset_.futureTimes);
        timesPast_ = timesToText(asset_.pastTimes);
        timesInit_ = true;
    }
    char tb[160];
    std::snprintf(tb, sizeof tb, "%s", timesFuture_.c_str());
    ImGui::SetNextItemWidth(170.0f * dpi);
    if (ImGui::InputText("Future times", tb, sizeof tb, ImGuiInputTextFlags_EnterReturnsTrue)) {
        setTrajectoryTimes(true, textToTimes(tb));
        timesFuture_ = timesToText(asset_.futureTimes);
    } else if (ImGui::IsItemActive()) {
        timesFuture_ = tb;
    }
    ImGui::SetItemTooltip("Seconds ahead of the frame, comma separated, at most 8 (Enter applies).");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f * dpi);
    touch(ImGui::DragFloat("pos##f", &asset_.futurePos, 0.01f, 0.0f, 10.0f, "%.2f"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f * dpi);
    touch(ImGui::DragFloat("dir##f", &asset_.futureDir, 0.01f, 0.0f, 10.0f, "%.2f"));

    std::snprintf(tb, sizeof tb, "%s", timesPast_.c_str());
    ImGui::SetNextItemWidth(170.0f * dpi);
    if (ImGui::InputText("Past times", tb, sizeof tb, ImGuiInputTextFlags_EnterReturnsTrue)) {
        setTrajectoryTimes(false, textToTimes(tb));
        timesPast_ = timesToText(asset_.pastTimes);
    } else if (ImGui::IsItemActive()) {
        timesPast_ = tb;
    }
    ImGui::SetItemTooltip("Negative seconds behind the frame, further back first or last, at most 8.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f * dpi);
    touch(ImGui::DragFloat("pos##p", &asset_.pastPos, 0.01f, 0.0f, 10.0f, "%.2f"));
}

// The entry's window and its tag ranges on the clip's duration. Handles drag in 1/30 s steps.
void MotionDbEditor::drawLane(i32 ci) {
    const f32 dpi = fontScale();
    MotionClipEntry& e = asset_.clips[static_cast<usize>(ci)];
    f32 total = clipDuration(e.clip);
    if (total <= 0.0f) {
        total = std::max(e.end, e.start + 1.0f);
        for (const MotionTagRange& r : e.ranges) total = std::max(total, std::max(r.end, r.start + 0.1f));
    }
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const f32 w = std::max(ImGui::GetContentRegionAvail().x - 8.0f * dpi, 80.0f * dpi);
    const f32 h = 30.0f * dpi;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto xOf = [&](f32 t) { return p0.x + std::clamp(t / total, 0.0f, 1.0f) * w; };
    auto tOf = [&](f32 x) { return std::clamp((x - p0.x) / w, 0.0f, 1.0f) * total; };

    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(28, 28, 32, 255));
    const f32 winEnd = e.end < 0.0f ? total : std::min(e.end, total);
    dl->AddRectFilled(ImVec2(xOf(e.start), p0.y + 12.0f * dpi), ImVec2(xOf(winEnd), p0.y + h - 2.0f * dpi), IM_COL32(55, 100, 95, 255));
    for (usize i = 0; i < e.ranges.size(); ++i) {
        const MotionTagRange& r = e.ranges[i];
        const f32 rEnd = r.end < 0.0f ? winEnd : r.end;
        const ImU32 col = r.noSearch ? IM_COL32(200, 80, 70, 200) : IM_COL32(90, 140, 210, 200);
        dl->AddRectFilled(ImVec2(xOf(r.start), p0.y + 2.0f * dpi), ImVec2(xOf(rEnd), p0.y + h - 2.0f * dpi), col);
        for (int edge = 0; edge < 2; ++edge) {
            const f32 x = xOf(edge == 0 ? r.start : rEnd);
            ImGui::SetCursorScreenPos(ImVec2(x - 4.0f * dpi, p0.y));
            ImGui::PushID(static_cast<int>(i) * 2 + edge + 5000);
            ImGui::InvisibleButton("##edge", ImVec2(8.0f * dpi, h));
            if (ImGui::IsItemActivated()) { dragRange_ = static_cast<i32>(i); dragEdge_ = edge; }
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            ImGui::PopID();
            dl->AddLine(ImVec2(x, p0.y + 2.0f * dpi), ImVec2(x, p0.y + h - 2.0f * dpi), IM_COL32(240, 240, 240, 255), 2.0f);
        }
    }
    if (dragRange_ >= 0 && dragRange_ < static_cast<i32>(e.ranges.size())) {
        if (!ImGui::IsMouseDown(0)) {
            dragRange_ = -1;
            endGesture_ = true;
        } else {
            if (!gesture_) { history_.push(before_); gesture_ = true; }
            const MotionTagRange& r = e.ranges[static_cast<usize>(dragRange_)];
            const f32 t = mdbSnap(tOf(ImGui::GetIO().MousePos.x));
            const f32 rEnd = r.end < 0.0f ? winEnd : r.end;
            if (dragEdge_ == 0) mdbMoveRange(asset_, ci, dragRange_, std::min(t, rEnd - kMinRangeLength), r.end);
            else mdbMoveRange(asset_, ci, dragRange_, r.start, t);
            markDirty();
        }
    } else {
        dragRange_ = -1;
    }
    char label[48];
    std::snprintf(label, sizeof label, "%.2f s", total);
    dl->AddText(ImVec2(p0.x + w - 56.0f * dpi, p0.y + h - 16.0f * dpi), IM_COL32(150, 150, 160, 255), label);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h + 4.0f * dpi));
    ImGui::Dummy(ImVec2(1, 1));
}

void MotionDbEditor::drawClips() {
    const f32 dpi = fontScale();
    ImGui::SeparatorText("Clips");
    char buf[256];
    int removeAt = -1, moveAt = -1, moveBy = 0;
    for (usize i = 0; i < asset_.clips.size(); ++i) {
        MotionClipEntry& e = asset_.clips[i];
        ImGui::PushID(static_cast<int>(i) + 300);
        const f32 dur = clipDuration(e.clip);
        std::string head = fs::path(e.clip).stem().string();
        if (head.empty()) head = "(clip)";
        head += dur > 0.0f ? "  " + std::to_string(dur).substr(0, 4) + " s" : "  (not found)";
        const bool open = ::aver::editor::sectionHeader((head + "###entry").c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        if (ImGui::IsItemClicked()) selClip_ = static_cast<i32>(i);
        if (open) {
            std::snprintf(buf, sizeof buf, "%s", e.clip.c_str());
            ImGui::SetNextItemWidth(260.0f * dpi);
            const bool c = ImGui::InputText("Clip", buf, sizeof buf);
            if (c) { e.clip = buf; durations_.clear(); }
            touch(c);
            ImGui::SetNextItemWidth(70.0f * dpi);
            touch(ImGui::DragFloat("start", &e.start, 0.01f, 0.0f, 600.0f, "%.2f"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70.0f * dpi);
            touch(ImGui::DragFloat("end", &e.end, 0.01f, -1.0f, 600.0f, "%.2f"));
            ImGui::SetItemTooltip("-1 = to the end of the clip.");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70.0f * dpi);
            int loop = e.loop < 0 ? 0 : e.loop ? 1 : 2;
            if (ImGui::Combo("loop", &loop, "auto\0yes\0no\0")) { pushUndo(); e.loop = loop == 0 ? -1 : loop == 1 ? 1 : 0; markDirty(); }
            ImGui::SetNextItemWidth(70.0f * dpi);
            touch(ImGui::DragFloat("bias", &e.bias, 0.01f, -100.0f, 100.0f, "%.2f"));
            ImGui::SetItemTooltip("Added to the cost of every frame of this entry; negative prefers it.");
            ImGui::SameLine();
            bool ns = e.noSearch;
            if (ImGui::Checkbox("no search", &ns)) { pushUndo(); e.noSearch = ns; markDirty(); }
            ImGui::SetItemTooltip("Playable (continuation) but never a candidate.");
            ImGui::SameLine();
            if (ImGui::SmallButton("^")) { moveAt = static_cast<int>(i); moveBy = -1; }
            ImGui::SameLine();
            if (ImGui::SmallButton("v")) { moveAt = static_cast<int>(i); moveBy = 1; }
            ImGui::SameLine();
            if (ImGui::SmallButton("remove")) removeAt = static_cast<int>(i);

            for (usize t = 0; t < asset_.tags.size(); ++t) {
                const std::string& tag = asset_.tags[t];
                bool on = hasTag(e.tags, tag);
                if (t) ImGui::SameLine();
                if (ImGui::Checkbox((tag + "##t").c_str(), &on)) {
                    pushUndo();
                    if (on) e.tags.push_back(tag); else e.tags.erase(std::remove(e.tags.begin(), e.tags.end(), tag), e.tags.end());
                    markDirty();
                }
            }

            drawLane(static_cast<i32>(i));
            int removeRange2 = -1;
            for (usize r = 0; r < e.ranges.size(); ++r) {
                MotionTagRange& rg = e.ranges[r];
                ImGui::PushID(static_cast<int>(r) + 1000);
                ImGui::SetNextItemWidth(60.0f * dpi);
                touch(ImGui::DragFloat("##rs", &rg.start, 0.01f, 0.0f, 600.0f, "%.2f"));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(60.0f * dpi);
                touch(ImGui::DragFloat("##re", &rg.end, 0.01f, -1.0f, 600.0f, "%.2f"));
                ImGui::SameLine();
                bool rns = rg.noSearch;
                if (ImGui::Checkbox("no search##r", &rns)) { pushUndo(); rg.noSearch = rns; markDirty(); }
                for (usize t = 0; t < asset_.tags.size(); ++t) {
                    const std::string& tag = asset_.tags[t];
                    bool on = hasTag(rg.tags, tag);
                    ImGui::SameLine();
                    if (ImGui::Checkbox((tag + "##rt").c_str(), &on)) {
                        pushUndo();
                        if (on) rg.tags.push_back(tag); else rg.tags.erase(std::remove(rg.tags.begin(), rg.tags.end(), tag), rg.tags.end());
                        markDirty();
                    }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) removeRange2 = static_cast<int>(r);
                ImGui::PopID();
            }
            if (removeRange2 >= 0) removeRange(static_cast<i32>(i), removeRange2);
            if (ImGui::SmallButton("Add range")) {
                const f32 total = dur > 0.0f ? dur : 1.0f;
                addRange(static_cast<i32>(i), 0.0f, std::min(total, 0.25f));
            }
            ImGui::SetItemTooltip("A sub-range of the entry: extra tags and/or removed from the search (e.g. the first frames of a stop).");
        }
        ImGui::PopID();
    }
    if (moveAt >= 0) moveClip(moveAt, moveBy);
    if (removeAt >= 0) removeClip(removeAt);

    ImGui::Selectable("Drop .ocanim clips here to append", false, 0, ImVec2(0, 26.0f * dpi));
    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kPayloadSet);
        if (!payload) payload = ImGui::AcceptDragDropPayload(kPayloadOne);
        for (const std::string& one : payloadPaths(payload))
            if (hasExt(one, ".ocanim")) addClip(mdbClipRefFromPath(path_, one));
        ImGui::EndDragDropTarget();
    }
}

void MotionDbEditor::drawBake() {
    ImGui::SeparatorText("Bake");
    std::string why;
    const bool valid = asset_.valid(&why);
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button(dirty_ ? "Save and bake" : "Bake")) {
        std::string err;
        if (!bake(&err)) AVER_ERROR("[MotionDbEditor] bake failed for '{}': {}", path_, err);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", mdbSidecarPath(path_).c_str());
    if (!valid) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "Invalid: %s", why.c_str());
    if (!bake_.ok) {
        if (!bake_.why.empty()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "Last bake failed: %s", bake_.why.c_str());
        return;
    }
    const anim::MotionBuildReport& r = bake_.report;
    ImGui::Text("%u rows, %u ranges, %.2f MB, built in %.0f ms%s", r.rows, r.ranges, static_cast<double>(r.bytes) / 1048576.0,
                r.seconds * 1000.0, previewStale_ ? "   (the asset changed since)" : "");
    if (bake_.db) ImGui::Text("%u dimensions (%u padded)%s", bake_.db->dim(), bake_.db->dimPadded(), bake_.db->hasKd() ? ", KD tree built" : "");
    for (const std::string& w : r.warnings) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", w.c_str());
}

void MotionDbEditor::drawLab() {
    const f32 dpi = fontScale();
    ImGui::SeparatorText("Query lab");
    const anim::PoseDatabase* db = database();
    if (!db) {
        ImGui::TextDisabled("Bake first: the lab searches the baked rows.");
        return;
    }
    ImGui::SetNextItemWidth(100.0f * dpi);
    ImGui::DragFloat("Moving at", &lab_.currentSpeed, 1.0f, 0.0f, 1500.0f, "%.0f cm/s");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f * dpi);
    ImGui::DragFloat("Wants", &lab_.wantedSpeed, 1.0f, 0.0f, 1500.0f, "%.0f cm/s");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f * dpi);
    ImGui::DragFloat("Heading", &lab_.wantedHeadingDeg, 0.5f, -180.0f, 180.0f, "%.0f deg");
    for (usize t = 0; t < asset_.tags.size() && t < anim::kMaxMotionTags; ++t) {
        const u32 bit = 1u << t;
        bool req = (lab_.require & bit) != 0, exc = (lab_.exclude & bit) != 0;
        if (t) ImGui::SameLine();
        ImGui::PushID(static_cast<int>(t) + 7000);
        if (ImGui::Checkbox("req", &req)) lab_.require = req ? lab_.require | bit : lab_.require & ~bit;
        ImGui::SameLine();
        if (ImGui::Checkbox("not", &exc)) lab_.exclude = exc ? lab_.exclude | bit : lab_.exclude & ~bit;
        ImGui::SameLine();
        ImGui::TextUnformatted(asset_.tags[t].c_str());
        ImGui::PopID();
    }
    if (ImGui::Button("Search")) labResult_ = mdbRunQuery(*db, lab_, 8);

    if (labResult_.hits.empty()) return;
    ImGui::Text("linear %.0f us%s", labResult_.linearMicros, db->hasKd() ? "" : " (no KD tree)");
    if (db->hasKd()) {
        ImGui::SameLine();
        ImGui::Text(" | kd tree %.0f us%s", labResult_.kdMicros, labResult_.sameAnswer ? "" : "  MISMATCH");
    }
    for (const MdbLabHit& h : labResult_.hits) {
        ImGui::Text("%s @ %.2f s   cost %.3f", fs::path(h.clip).stem().string().c_str(), h.time, h.cost);
        f32 total = 0.0f;
        for (const f32 c : h.groupCosts) total += c;
        for (usize g = 0; g < h.groupCosts.size(); ++g) {
            ImGui::SameLine();
            ImGui::PushID(static_cast<int>(g) + static_cast<int>(h.row) * 16);
            ImGui::ProgressBar(total > 0.0f ? h.groupCosts[g] / total : 0.0f, ImVec2(18.0f * dpi, 10.0f * dpi), "");
            ImGui::PopID();
        }
    }
}

void MotionDbEditor::draw(Engine& e) {
    (void)e;
    if (!loaded_) {
        ImGui::TextWrapped("This file could not be read: %s", loadError_.c_str());
        return;
    }
    before_ = asset_;
    if (ImGui::Button("Save") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                                  keybinds().pressed(CommandId::AssetSave, ImGui::GetIO()))) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[MotionDbEditor] save failed for '{}': {}", path_, why);
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
    ImGui::Separator();

    drawHeader();
    drawTags();
    drawFeatures();
    drawClips();
    drawBake();
    drawLab();

    if (endGesture_ && !ImGui::IsMouseDown(0)) { gesture_ = false; endGesture_ = false; }
}

#else   // AVER_WITH_IMGUI

void MotionDbEditor::draw(Engine& e) { (void)e; }

#endif  // AVER_WITH_IMGUI

std::unique_ptr<AssetEditor> makeMotionDbEditor(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".ocmotion") return nullptr;
    return std::make_unique<MotionDbEditor>(path);
}

} // namespace aver::editor
