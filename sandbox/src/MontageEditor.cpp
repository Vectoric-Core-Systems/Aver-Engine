// The .ocmontage editor tab. See the header for the SandboxApp hook and for why the edits are free
// functions rather than members.

#include "MontageEditor.hpp"
#include "EditorChrome.hpp"
#include "EditorKeybinds.hpp"

#include "aver/anim/AnimGraphAsset.hpp"
#include "aver/anim/AnimSampler.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>

#if AVER_WITH_IMGUI
#  include "imgui.h"
#endif

namespace aver::editor {

using anim::MontageAsset;
using anim::MontageNotify;
using anim::MontageSection;
using anim::MontageSegment;

namespace {

namespace fs = std::filesystem;

bool inRange(usize n, i32 i) { return i >= 0 && static_cast<usize>(i) < n; }

constexpr f32 kMinSectionStart = 1.0f / 30.0f;

i32 sectionByName(const MontageAsset& a, const std::string& n) {
    for (usize i = 0; i < a.sections.size(); ++i)
        if (a.sections[i].name == n) return static_cast<i32>(i);
    return -1;
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

} // namespace

// ------------------------------------------------------------------------------- edits ----------

MontageAsset montageStarter() {
    MontageAsset a;
    a.sections = {{"Default", 0.0f, ""}};
    return a;
}

i32 mtgAddSegment(MontageAsset& a, const std::string& clip, i32 at) {
    if (clip.empty()) return -1;
    MontageSegment g;
    g.clip = clip;
    const i32 idx = inRange(a.segments.size(), at) ? at : static_cast<i32>(a.segments.size());
    a.segments.insert(a.segments.begin() + idx, std::move(g));
    return idx;
}

bool mtgRemoveSegment(MontageAsset& a, i32 index) {
    if (!inRange(a.segments.size(), index)) return false;
    a.segments.erase(a.segments.begin() + index);
    return true;
}

i32 mtgMoveSegment(MontageAsset& a, i32 index, i32 delta) {
    if (!inRange(a.segments.size(), index)) return -1;
    const i32 to = std::clamp(index + delta, 0, static_cast<i32>(a.segments.size()) - 1);
    if (to == index) return index;
    MontageSegment g = std::move(a.segments[static_cast<usize>(index)]);
    a.segments.erase(a.segments.begin() + index);
    a.segments.insert(a.segments.begin() + to, std::move(g));
    return to;
}

std::string mtgUniqueSectionName(const MontageAsset& a, const std::string& base) {
    if (sectionByName(a, base) < 0) return base;
    for (int n = 2;; ++n) {
        const std::string cand = base + " " + std::to_string(n);
        if (sectionByName(a, cand) < 0) return cand;
    }
}

i32 mtgAddSection(MontageAsset& a, const std::string& name, f32 start) {
    MontageSection s;
    s.name = mtgUniqueSectionName(a, name.empty() ? std::string("Section") : name);
    // The first section is pinned at 0, so a later one always starts after it.
    s.start = a.sections.empty() ? 0.0f : (std::isfinite(start) ? std::max(start, kMinSectionStart) : kMinSectionStart);
    usize at = 0;
    while (at < a.sections.size() && a.sections[at].start <= s.start) ++at;
    a.sections.insert(a.sections.begin() + static_cast<std::ptrdiff_t>(at), std::move(s));
    return static_cast<i32>(at);
}

bool mtgRemoveSection(MontageAsset& a, i32 index) {
    if (!inRange(a.sections.size(), index) || a.sections.size() < 2) return false;
    const MontageSection gone = a.sections[static_cast<usize>(index)];
    // Whatever led here now leads where the removed section led; a loop on itself falls back to "following".
    const std::string takeover = gone.next == gone.name ? std::string() : gone.next;
    a.sections.erase(a.sections.begin() + index);
    for (MontageSection& s : a.sections)
        if (s.next == gone.name) s.next = takeover;
    if (index == 0) a.sections[0].start = 0.0f;
    return true;
}

bool mtgRenameSection(MontageAsset& a, i32 index, const std::string& name) {
    if (!inRange(a.sections.size(), index) || name.empty() || name == anim::kMontageEnd) return false;
    MontageSection& s = a.sections[static_cast<usize>(index)];
    if (s.name == name) return true;
    if (sectionByName(a, name) >= 0) return false;
    const std::string old = s.name;
    s.name = name;
    for (MontageSection& o : a.sections)
        if (o.next == old) o.next = name;
    return true;
}

bool mtgSetNext(MontageAsset& a, i32 index, const std::string& next) {
    if (!inRange(a.sections.size(), index)) return false;
    if (!next.empty() && next != anim::kMontageEnd && sectionByName(a, next) < 0) return false;
    a.sections[static_cast<usize>(index)].next = next;
    return true;
}

i32 mtgMoveSection(MontageAsset& a, i32 index, f32 start) {
    if (index <= 0 || !inRange(a.sections.size(), index) || !std::isfinite(start)) return -1;
    const std::string name = a.sections[static_cast<usize>(index)].name;
    a.sections[static_cast<usize>(index)].start = std::max(start, kMinSectionStart);
    std::stable_sort(a.sections.begin(), a.sections.end(),
                     [](const MontageSection& x, const MontageSection& y) { return x.start < y.start; });
    return sectionByName(a, name);
}

i32 mtgAddNotify(MontageAsset& a, f32 time, const std::string& name, f32 duration) {
    if (!std::isfinite(time) || !std::isfinite(duration) || duration < 0.0f) return -1;
    MontageNotify n;
    n.time = std::max(time, 0.0f);
    n.name = name.empty() ? std::string("Notify") : name;
    n.duration = duration;
    usize at = 0;
    while (at < a.notifies.size() && a.notifies[at].time <= n.time) ++at;
    a.notifies.insert(a.notifies.begin() + static_cast<std::ptrdiff_t>(at), std::move(n));
    return static_cast<i32>(at);
}

bool mtgRemoveNotify(MontageAsset& a, i32 index) {
    if (!inRange(a.notifies.size(), index)) return false;
    a.notifies.erase(a.notifies.begin() + index);
    return true;
}

i32 mtgMoveNotify(MontageAsset& a, i32 index, f32 time) {
    if (!inRange(a.notifies.size(), index) || !std::isfinite(time)) return -1;
    MontageNotify n = std::move(a.notifies[static_cast<usize>(index)]);
    a.notifies.erase(a.notifies.begin() + index);
    n.time = std::max(time, 0.0f);
    usize at = 0;
    while (at < a.notifies.size() && a.notifies[at].time <= n.time) ++at;
    a.notifies.insert(a.notifies.begin() + static_cast<std::ptrdiff_t>(at), std::move(n));
    return static_cast<i32>(at);
}

f32 mtgSnap(f32 t, f32 step) {
    if (!(step > 0.0f)) return t;
    return std::round(t / step) * step;
}

MtgTimeline mtgTimeline(const MontageAsset& a, const MtgDurationFn& clipDuration) {
    MtgTimeline t;
    for (const MontageSegment& g : a.segments) {
        const f32 dur = clipDuration ? clipDuration(g.clip) : 0.0f;
        // The same rule MontagePlayer::bind applies to a resolved clip.
        f32 end, start;
        if (dur > 0.0f) {
            end = g.end < 0.0f ? dur : std::min(g.end, dur);
            start = std::min(std::max(g.start, 0.0f), end);
        } else {
            start = std::max(g.start, 0.0f);
            end = g.end < 0.0f ? start : g.end;
        }
        const f32 len = (g.rate > 0.0f) ? std::max(0.0f, (end - start) / g.rate) : 0.0f;
        t.starts.push_back(t.total);
        t.lengths.push_back(len);
        t.total += len;
    }
    return t;
}

std::string mtgContentRootOf(const std::string& montagePath) {
    const fs::path p(montagePath);
    for (fs::path d = p.parent_path(); !d.empty(); d = d.parent_path()) {
        if (iequals(d.filename().string(), "Content")) return d.string();
        if (d == d.parent_path()) break;
    }
    return p.parent_path().string();
}

std::string mtgClipRefFromPath(const std::string& montagePath, const std::string& clipPath) {
    const fs::path root(mtgContentRootOf(montagePath));
    const fs::path clip(clipPath);
    fs::path rel = clip.lexically_relative(root);
    const std::string first = rel.empty() ? std::string() : rel.begin()->string();
    if (rel.empty() || first == "..") rel = clip.filename();
    return rel.generic_string();
}

std::string mtgResolveClipPath(const std::string& montagePath, const std::string& clipRef) {
    if (clipRef.empty()) return {};
    const fs::path ref(clipRef);
    if (ref.is_absolute()) return isFile(ref) ? ref.string() : std::string();
    const fs::path root(mtgContentRootOf(montagePath));
    const fs::path candidates[] = {root / ref, fs::path(montagePath).parent_path() / ref, root.parent_path() / ref};
    for (const fs::path& c : candidates)
        if (isFile(c)) return c.string();
    return {};
}

// ------------------------------------------------------------------------------- the tab ----------

MontageEditor::MontageEditor(std::string path) : path_(std::move(path)) { loadFromDisk(); }

void MontageEditor::loadFromDisk() {
    std::string why;
    MontageAsset a;
    if (!anim::loadMontage(path_, a, &why)) {
        loaded_ = false;
        loadError_ = why;
        return;
    }
    asset_ = std::move(a);
    loaded_ = true;
    loadError_.clear();
    dirty_ = false;
    selSection_ = selNotify_ = selSegment_ = -1;
    simStale_ = true;
    simBound_ = false;
    history_.clear();
    clips_.clear();
}

std::string MontageEditor::title() const {
    return fs::path(path_).filename().string() + "###mtg:" + path_;
}

bool MontageEditor::save(std::string* why) {
    if (!loaded_) {
        if (why) *why = "cannot save: file failed to load (" + loadError_ + ")";
        return false;
    }
    if (!anim::saveMontage(path_, asset_, why)) return false;
    dirty_ = false;
    return true;
}

void MontageEditor::onFileChanged() {
    if (dirty_) {
        AVER_WARN("[MontageEditor] '{}' changed on disk, but this tab has unsaved edits -- keeping them", path_);
        return;
    }
    loadFromDisk();
}

void MontageEditor::undo() {
    if (!history_.undo(asset_)) return;
    dirty_ = true;
    simStale_ = true;
    selSection_ = selNotify_ = selSegment_ = -1;
}

void MontageEditor::redo() {
    if (!history_.redo(asset_)) return;
    dirty_ = true;
    simStale_ = true;
    selSection_ = selNotify_ = selSegment_ = -1;
}

i32 MontageEditor::addSegment(const std::string& clipRef) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 idx = mtgAddSegment(asset_, clipRef);
    if (idx < 0) { history_.cancelPush(); return -1; }
    selectSegment(idx);
    markDirty();
    return idx;
}

bool MontageEditor::removeSegment(i32 index) {
    if (!loaded_) return false;
    pushUndo();
    if (!mtgRemoveSegment(asset_, index)) { history_.cancelPush(); return false; }
    selSegment_ = -1;
    markDirty();
    return true;
}

i32 MontageEditor::moveSegment(i32 index, i32 delta) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 to = mtgMoveSegment(asset_, index, delta);
    if (to < 0 || to == index) { history_.cancelPush(); return to; }
    selectSegment(to);
    markDirty();
    return to;
}

i32 MontageEditor::addSection(const std::string& name, f32 start) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 idx = mtgAddSection(asset_, name, start);
    selectSection(idx);
    markDirty();
    return idx;
}

bool MontageEditor::removeSection(i32 index) {
    if (!loaded_) return false;
    pushUndo();
    if (!mtgRemoveSection(asset_, index)) { history_.cancelPush(); return false; }
    selSection_ = -1;
    markDirty();
    return true;
}

bool MontageEditor::renameSection(i32 index, const std::string& name) {
    if (!loaded_) return false;
    pushUndo();
    if (!mtgRenameSection(asset_, index, name)) { history_.cancelPush(); return false; }
    markDirty();
    return true;
}

bool MontageEditor::setNext(i32 index, const std::string& next) {
    if (!loaded_) return false;
    pushUndo();
    if (!mtgSetNext(asset_, index, next)) { history_.cancelPush(); return false; }
    markDirty();
    return true;
}

i32 MontageEditor::addNotify(f32 time, const std::string& name, f32 duration) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 idx = mtgAddNotify(asset_, time, name, duration);
    if (idx < 0) { history_.cancelPush(); return -1; }
    selectNotify(idx);
    markDirty();
    return idx;
}

bool MontageEditor::removeNotify(i32 index) {
    if (!loaded_) return false;
    pushUndo();
    if (!mtgRemoveNotify(asset_, index)) { history_.cancelPush(); return false; }
    selNotify_ = -1;
    markDirty();
    return true;
}

i32 MontageEditor::moveNotify(i32 index, f32 time) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 to = mtgMoveNotify(asset_, index, time);
    if (to < 0) { history_.cancelPush(); return -1; }
    selectNotify(to);
    markDirty();
    return to;
}

const fmt::OcAnimation* MontageEditor::clipFor(const std::string& clipRef) {
    const auto it = clips_.find(clipRef);
    if (it != clips_.end()) return it->second.get();
    auto clip = std::make_unique<fmt::OcAnimation>();
    const std::string file = mtgResolveClipPath(path_, clipRef);
    std::string why;
    if (file.empty() || !fmt::loadOcAnim(file, *clip, &why)) clip.reset();
    return (clips_[clipRef] = std::move(clip)).get();
}

f32 MontageEditor::clipDuration(const std::string& clipRef) {
    const fmt::OcAnimation* c = clipFor(clipRef);
    return c ? c->duration : 0.0f;
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

bool isClipFile(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".ocanim";
}

const char* eventLabel(anim::MontageEvent::Kind k) {
    switch (k) {
        case anim::MontageEvent::Kind::Started:        return "started";
        case anim::MontageEvent::Kind::Notify:         return "notify";
        case anim::MontageEvent::Kind::NotifyBegin:    return "notify begin";
        case anim::MontageEvent::Kind::NotifyEnd:      return "notify end";
        case anim::MontageEvent::Kind::SectionStarted: return "section";
        case anim::MontageEvent::Kind::BlendOut:       return "blend out";
        case anim::MontageEvent::Kind::Interrupted:    return "interrupted";
        case anim::MontageEvent::Kind::Ended:          return "ended";
    }
    return "";
}

} // namespace

void MontageEditor::touch(bool changed) {
    if (changed) {
        if (!gesture_) { history_.push(before_); gesture_ = true; }
        markDirty();
    }
    if (ImGui::IsItemDeactivated()) endGesture_ = true;
}

void MontageEditor::drawHeader() {
    const f32 dpi = fontScale();
    char buf[256];
    auto text = [&](const char* label, std::string& v) {
        std::snprintf(buf, sizeof buf, "%s", v.c_str());
        const bool c = ImGui::InputText(label, buf, sizeof buf);
        if (c) v = buf;
        touch(c);
    };
    text("Name", asset_.name);
    text("Slot", asset_.slot);
    ImGui::SetNextItemWidth(90.0f * dpi);
    touch(ImGui::DragFloat("Blend in", &asset_.blendIn, 0.01f, 0.0f, 10.0f, "%.2f s"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f * dpi);
    int c = static_cast<int>(asset_.inCurve);
    if (ImGui::Combo("##incurve", &c, "linear\0smooth\0")) { pushUndo(); asset_.inCurve = static_cast<anim::BlendCurve>(c); markDirty(); }
    ImGui::SetNextItemWidth(90.0f * dpi);
    touch(ImGui::DragFloat("Blend out", &asset_.blendOut, 0.01f, 0.0f, 10.0f, "%.2f s"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f * dpi);
    c = static_cast<int>(asset_.outCurve);
    if (ImGui::Combo("##outcurve", &c, "linear\0smooth\0")) { pushUndo(); asset_.outCurve = static_cast<anim::BlendCurve>(c); markDirty(); }
    ImGui::SetNextItemWidth(90.0f * dpi);
    touch(ImGui::DragFloat("Blend-out trigger", &asset_.blendOutTrigger, 0.01f, -1.0f, 10.0f, "%.2f s"));
    ImGui::SetItemTooltip("Seconds before the end at which the blend-out starts. -1 = the blend-out time.");
    ImGui::SetNextItemWidth(90.0f * dpi);
    touch(ImGui::DragFloat("Rate", &asset_.rate, 0.01f, 0.0f, 8.0f, "%.2f"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f * dpi);
    int ad = asset_.additive < 0 ? 0 : asset_.additive ? 1 : 2;
    if (ImGui::Combo("Additive", &ad, "auto\0yes\0no\0")) { pushUndo(); asset_.additive = ad == 0 ? -1 : ad == 1 ? 1 : 0; markDirty(); }
    ImGui::SetItemTooltip("auto = additive when the first clip is flagged Additive in the Anim tab.");
}

void MontageEditor::drawTimeline() {
    const f32 dpi = fontScale();
    const MtgTimeline tl = timeline();
    const f32 total = std::max(tl.total, 0.1f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const f32 w = std::max(ImGui::GetContentRegionAvail().x - 4.0f * dpi, 60.0f * dpi);
    const f32 h = 84.0f * dpi;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto xOf = [&](f32 t) { return p0.x + std::clamp(t / total, 0.0f, 1.0f) * w; };
    auto tOf = [&](f32 x) { return std::clamp((x - p0.x) / w, 0.0f, 1.0f) * total; };

    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(28, 28, 32, 255));
    const f32 segY0 = p0.y + 22.0f * dpi, segY1 = p0.y + 50.0f * dpi;
    for (usize i = 0; i < tl.starts.size(); ++i) {
        const ImVec2 a(xOf(tl.starts[i]), segY0), b(xOf(tl.starts[i] + tl.lengths[i]), segY1);
        const bool sel = selSegment_ == static_cast<i32>(i);
        dl->AddRectFilled(a, b, (i % 2) ? IM_COL32(70, 110, 160, 255) : IM_COL32(60, 140, 130, 255));
        dl->AddRect(a, b, sel ? IM_COL32(255, 220, 90, 255) : IM_COL32(15, 15, 18, 255), 0.0f, 0, sel ? 2.0f : 1.0f);
        const std::string name = fs::path(asset_.segments[i].clip).stem().string();
        dl->PushClipRect(a, b, true);
        dl->AddText(ImVec2(a.x + 3.0f * dpi, a.y + 5.0f * dpi), IM_COL32(240, 240, 240, 255), name.c_str());
        dl->PopClipRect();
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(static_cast<int>(i) + 1000);
        if (b.x - a.x > 2.0f && ImGui::InvisibleButton("##seg", ImVec2(b.x - a.x, segY1 - segY0))) selectSegment(static_cast<i32>(i));
        ImGui::PopID();
    }

    // Section markers: the line spans the lane; the label sits on top.
    for (usize i = 0; i < asset_.sections.size(); ++i) {
        const MontageSection& s = asset_.sections[i];
        const f32 x = xOf(s.start);
        const bool sel = selSection_ == static_cast<i32>(i);
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + h), sel ? IM_COL32(255, 220, 90, 255) : IM_COL32(230, 170, 60, 255), sel ? 2.5f : 1.5f);
        dl->AddText(ImVec2(x + 3.0f * dpi, p0.y + 2.0f * dpi), IM_COL32(240, 200, 120, 255), s.name.c_str());
        ImGui::SetCursorScreenPos(ImVec2(x - 5.0f * dpi, p0.y));
        ImGui::PushID(static_cast<int>(i) + 2000);
        ImGui::InvisibleButton("##sec", ImVec2(10.0f * dpi, 20.0f * dpi));
        if (ImGui::IsItemActivated()) { selectSection(static_cast<i32>(i)); if (i > 0) dragSection_ = static_cast<i32>(i); }
        ImGui::PopID();
    }

    // Notify markers: a diamond on the bottom lane, a bar for a state's duration.
    const f32 noteY = p0.y + 66.0f * dpi;
    for (usize i = 0; i < asset_.notifies.size(); ++i) {
        const MontageNotify& n = asset_.notifies[i];
        const f32 x = xOf(n.time);
        const bool sel = selNotify_ == static_cast<i32>(i);
        const ImU32 col = sel ? IM_COL32(255, 220, 90, 255) : IM_COL32(120, 200, 255, 255);
        if (n.duration > 0.0f) dl->AddRectFilled(ImVec2(x, noteY - 2.0f * dpi), ImVec2(xOf(n.time + n.duration), noteY + 2.0f * dpi), col);
        const f32 r = 5.0f * dpi;
        dl->AddQuadFilled(ImVec2(x, noteY - r), ImVec2(x + r, noteY), ImVec2(x, noteY + r), ImVec2(x - r, noteY), col);
        ImGui::SetCursorScreenPos(ImVec2(x - r, noteY - r));
        ImGui::PushID(static_cast<int>(i) + 3000);
        ImGui::InvisibleButton("##note", ImVec2(2.0f * r, 2.0f * r));
        if (ImGui::IsItemActivated()) { selectNotify(static_cast<i32>(i)); dragNotify_ = static_cast<i32>(i); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s  %.2f s", n.name.c_str(), n.time);
        ImGui::PopID();
    }

    // Dragging, snapped to the time grid. A re-sort can change the index, so it is followed.
    if ((dragSection_ >= 0 || dragNotify_ >= 0) && !ImGui::IsMouseDown(0)) {
        dragSection_ = dragNotify_ = -1;
        endGesture_ = true;
    } else if (dragSection_ >= 0 || dragNotify_ >= 0) {
        const f32 t = mtgSnap(tOf(ImGui::GetIO().MousePos.x));
        if (!gesture_) { history_.push(before_); gesture_ = true; }
        if (dragSection_ >= 0) {
            const i32 to = mtgMoveSection(asset_, dragSection_, t);
            if (to >= 0) { dragSection_ = to; selectSection(to); markDirty(); }
        } else {
            const i32 to = mtgMoveNotify(asset_, dragNotify_, t);
            if (to >= 0) { dragNotify_ = to; selectNotify(to); markDirty(); }
        }
    }

    // The time ruler and the playhead; a click on the empty lane parks the playhead there.
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + 52.0f * dpi));
    ImGui::InvisibleButton("##lane", ImVec2(w, 8.0f * dpi));
    if (ImGui::IsItemActive()) {
        playhead_ = mtgSnap(tOf(ImGui::GetIO().MousePos.x));
        if (simOn_) simSeek(playhead_);
    }
    const f32 ph = (simOn_ && simBound_) ? sim_.position() : playhead_;
    dl->AddLine(ImVec2(xOf(ph), p0.y), ImVec2(xOf(ph), p0.y + h), IM_COL32(255, 80, 80, 255), 1.5f);
    char end[48];
    std::snprintf(end, sizeof end, "%.2f s", tl.total);
    dl->AddText(ImVec2(p0.x + w - 60.0f * dpi, p0.y + h - 16.0f * dpi), IM_COL32(150, 150, 160, 255), end);

    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h + 6.0f * dpi));
    ImGui::Dummy(ImVec2(1, 1));
}

void MontageEditor::drawSegments() {
    const f32 dpi = fontScale();
    char buf[256];
    int removeAt = -1, moveAt = -1, moveBy = 0;
    for (usize i = 0; i < asset_.segments.size(); ++i) {
        MontageSegment& g = asset_.segments[i];
        ImGui::PushID(static_cast<int>(i));
        std::snprintf(buf, sizeof buf, "%s", g.clip.c_str());
        ImGui::SetNextItemWidth(220.0f * dpi);
        const bool c = ImGui::InputText("##clip", buf, sizeof buf);
        if (c) { g.clip = buf; clips_.clear(); }
        touch(c);
        if (ImGui::IsItemActivated()) selectSegment(static_cast<i32>(i));
        if (clipDuration(g.clip) <= 0.0f) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "?");
            ImGui::SetItemTooltip("This clip could not be found next to the project's content.");
        }
        ImGui::SetNextItemWidth(60.0f * dpi);
        touch(ImGui::DragFloat("start", &g.start, 0.01f, 0.0f, 600.0f, "%.2f"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0f * dpi);
        touch(ImGui::DragFloat("end", &g.end, 0.01f, -1.0f, 600.0f, "%.2f"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0f * dpi);
        touch(ImGui::DragFloat("rate", &g.rate, 0.01f, 0.05f, 8.0f, "%.2f"));
        ImGui::SameLine();
        if (ImGui::SmallButton("^")) { moveAt = static_cast<int>(i); moveBy = -1; }
        ImGui::SameLine();
        if (ImGui::SmallButton("v")) { moveAt = static_cast<int>(i); moveBy = 1; }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeAt = static_cast<int>(i);
        ImGui::PopID();
    }
    if (moveAt >= 0) moveSegment(moveAt, moveBy);
    if (removeAt >= 0) removeSegment(removeAt);

    ImGui::Selectable("Drop .ocanim clips here to append", false, 0, ImVec2(0, 26.0f * dpi));
    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kPayloadSet);
        if (!payload) payload = ImGui::AcceptDragDropPayload(kPayloadOne);
        for (const std::string& one : payloadPaths(payload))
            if (isClipFile(one)) addSegment(mtgClipRefFromPath(path_, one));
        ImGui::EndDragDropTarget();
    }
}

void MontageEditor::drawDetails() {
    const f32 dpi = fontScale();
    char buf[256];
    if (ImGui::Button("Add section at playhead")) addSection("Section", mtgSnap(playhead_));
    ImGui::SameLine();
    if (ImGui::Button("Add notify at playhead")) addNotify(mtgSnap(playhead_), "Notify", 0.0f);

    if (inRange(asset_.sections.size(), selSection_)) {
        MontageSection& s = asset_.sections[static_cast<usize>(selSection_)];
        ImGui::SeparatorText("Section");
        std::snprintf(buf, sizeof buf, "%s", s.name.c_str());
        if (ImGui::InputText("Name##sec", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) renameSection(selSection_, buf);
        ImGui::BeginDisabled(selSection_ == 0);
        f32 start = s.start;
        ImGui::SetNextItemWidth(90.0f * dpi);
        if (ImGui::DragFloat("Start##sec", &start, 0.01f, 0.0f, 600.0f, "%.2f s")) {
            if (!gesture_) { history_.push(before_); gesture_ = true; }
            const i32 to = mtgMoveSection(asset_, selSection_, start);
            if (to >= 0) { selectSection(to); markDirty(); }
        }
        if (ImGui::IsItemDeactivated()) endGesture_ = true;
        ImGui::EndDisabled();
        // [following] | @end | every section (its own name = loop)
        const MontageSection& cur = asset_.sections[static_cast<usize>(selSection_)];
        const std::string label = cur.next.empty() ? "[following]" : cur.next;
        if (ImGui::BeginCombo("Next", label.c_str())) {
            if (ImGui::Selectable("[following]", cur.next.empty())) setNext(selSection_, "");
            if (ImGui::Selectable("@end", cur.next == anim::kMontageEnd)) setNext(selSection_, anim::kMontageEnd);
            for (usize i = 0; i < asset_.sections.size(); ++i) {
                const std::string name = asset_.sections[i].name;
                if (ImGui::Selectable(name.c_str(), cur.next == name)) setNext(selSection_, name);
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button("Delete section")) removeSection(selSection_);
    } else if (inRange(asset_.notifies.size(), selNotify_)) {
        MontageNotify& n = asset_.notifies[static_cast<usize>(selNotify_)];
        ImGui::SeparatorText("Notify");
        std::snprintf(buf, sizeof buf, "%s", n.name.c_str());
        const bool c = ImGui::InputText("Name##note", buf, sizeof buf);
        if (c) n.name = buf;
        touch(c);
        f32 time = n.time;
        ImGui::SetNextItemWidth(90.0f * dpi);
        if (ImGui::DragFloat("Time##note", &time, 0.01f, 0.0f, 600.0f, "%.2f s")) {
            if (!gesture_) { history_.push(before_); gesture_ = true; }
            const i32 to = mtgMoveNotify(asset_, selNotify_, time);
            if (to >= 0) { selectNotify(to); markDirty(); }
        }
        if (ImGui::IsItemDeactivated()) endGesture_ = true;
        if (inRange(asset_.notifies.size(), selNotify_)) {
            ImGui::SetNextItemWidth(90.0f * dpi);
            touch(ImGui::DragFloat("Duration##note", &asset_.notifies[static_cast<usize>(selNotify_)].duration, 0.01f, 0.0f, 600.0f, "%.2f s"));
            ImGui::SetItemTooltip("0 = an instant notify. Above 0 it is a state: <Name>_Begin ... <Name>_End.");
        }
        if (ImGui::Button("Delete notify")) removeNotify(selNotify_);
    } else {
        ImGui::TextDisabled("Click a marker on the timeline to edit it. Drag section and notify markers to move them.");
    }

    std::string why;
    if (!asset_.valid(&why)) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "Invalid: %s", why.c_str());
}

void MontageEditor::simRebind() {
    simBound_ = false;
    simStale_ = false;
    simLog_.clear();
    std::string why;
    if (!asset_.valid(&why)) return;
    simBound_ = sim_.bind(&asset_, [this](const std::string& c) { return clipFor(c); });
    if (simBound_) sim_.start(-1, 1.0f, -1.0f);
}

void MontageEditor::simSeek(f32 t) {
    simRebind();
    if (!simBound_) return;
    constexpr f32 kStep = 1.0f / 60.0f;
    for (f32 done = 0.0f; done < t; done += kStep) sim_.advance(std::min(kStep, t - done));
}

void MontageEditor::drawSimulator() {
    ImGui::SeparatorText("Simulator");
    const bool wasOn = simOn_;
    ImGui::Checkbox("Run", &simOn_);
    if (!simOn_) return;
    if (!wasOn || simStale_) simRebind();
    if (!simBound_) {
        std::string why;
        asset_.valid(&why);
        ImGui::TextDisabled("Nothing to play: %s", why.empty() ? "no clip could be loaded" : why.c_str());
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button(simPlaying_ ? "Pause" : "Play")) simPlaying_ = !simPlaying_;
    ImGui::SameLine();
    if (ImGui::Button("Restart")) { simRebind(); simPlaying_ = true; }
    ImGui::SameLine();
    if (ImGui::Button("Stop (blend out)")) sim_.stop(-1.0f);

    if (simPlaying_ && !sim_.finished()) sim_.advance(ImGui::GetIO().DeltaTime);
    std::vector<anim::MontageEvent> evs;
    sim_.drainEvents(evs);
    for (const anim::MontageEvent& e : evs) simLog_.push_back(std::string(eventLabel(e.kind)) + (e.name.empty() ? "" : "  " + e.name));
    if (simLog_.size() > 10) simLog_.erase(simLog_.begin(), simLog_.end() - 10);

    f32 pos = sim_.position();
    if (ImGui::SliderFloat("Position", &pos, 0.0f, std::max(sim_.length(), 0.01f), "%.2f s")) { simSeek(pos); simPlaying_ = false; }
    playhead_ = sim_.position();
    ImGui::Text("Section: %s    Weight: %.2f%s", sim_.sectionName(sim_.section()).c_str(), sim_.weight(),
                sim_.finished() ? "    (ended)" : "");
    ImGui::ProgressBar(sim_.weight(), ImVec2(-1, 0));
    for (i32 i = 0; i < sim_.sectionCount(); ++i) {
        if (i) ImGui::SameLine();
        const std::string label = "Jump: " + sim_.sectionName(i) + "##jmp" + std::to_string(i);
        if (ImGui::SmallButton(label.c_str())) sim_.jumpToSection(i);
    }
    for (const std::string& line : simLog_) ImGui::TextDisabled("%s", line.c_str());
}

void MontageEditor::draw(Engine& e) {
    (void)e;
    if (!loaded_) {
        ImGui::TextWrapped("This file could not be read: %s", loadError_.c_str());
        return;
    }
    before_ = asset_;
    if (ImGui::Button("Save") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                                  keybinds().pressed(CommandId::AssetSave, ImGui::GetIO()))) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[MontageEditor] save failed for '{}': {}", path_, why);
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
    ImGui::Separator();
    drawTimeline();
    ImGui::SeparatorText("Segments");
    drawSegments();
    drawDetails();
    drawSimulator();

    if (endGesture_ && !ImGui::IsMouseDown(0)) { gesture_ = false; endGesture_ = false; }
}

#else   // AVER_WITH_IMGUI

void MontageEditor::draw(Engine& e) { (void)e; }

#endif  // AVER_WITH_IMGUI

std::unique_ptr<AssetEditor> makeMontageEditor(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".ocmontage") return nullptr;
    return std::make_unique<MontageEditor>(path);
}

} // namespace aver::editor
