// The .ocblend blend-space editor tab. See the header for the SandboxApp hook and for why the edits
// are free functions rather than members.

#include "BlendSpaceEditor.hpp"
#include "EditorKeybinds.hpp"

#include "aver/anim/AnimGraphAsset.hpp"
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
namespace {

f32 rangeOf(const anim::BlendAxis& a) {
    const f32 r = a.max - a.min;
    return r > 1e-6f ? r : 1.0f;
}

f32 snapTo(f32 v, f32 lo, f32 step) {
    if (step <= 0.0f) return v;
    return lo + std::round((v - lo) / step) * step;
}

} // namespace

// ---------------------------------------------------------------------------- free edits ----------

anim::BlendSpaceAsset bsStarterSpace() {
    anim::BlendSpaceAsset a;
    a.name = "NewBlendSpace";
    a.dims = 1;
    a.axisX = {"Speed", 0.0f, 600.0f, 0.0f};
    a.axisY = {"Direction", -180.0f, 180.0f, 0.0f};
    auto s = [](const char* clip, f32 x) {
        anim::BlendSample m;
        m.clip = clip;
        m.x = x;
        return m;
    };
    a.samples = {s("Content/Anim/Idle.ocanim", 0.0f), s("Content/Anim/Walk.ocanim", 200.0f),
                 s("Content/Anim/Run.ocanim", 600.0f)};
    return a;
}

i32 bsAddSample(anim::BlendSpaceAsset& a, const std::string& clip, f32 x, f32 y) {
    anim::BlendSample s;
    s.clip = clip;
    s.x = std::clamp(x, a.axisX.min, a.axisX.max);
    s.y = a.dims >= 2 ? std::clamp(y, a.axisY.min, a.axisY.max) : 0.0f;
    a.samples.push_back(std::move(s));
    return static_cast<i32>(a.samples.size()) - 1;
}

bool bsRemoveSample(anim::BlendSpaceAsset& a, i32 index) {
    if (index < 0 || static_cast<usize>(index) >= a.samples.size() || a.samples.size() <= 1) return false;
    a.samples.erase(a.samples.begin() + index);
    return true;
}

void bsMoveSample(anim::BlendSpaceAsset& a, i32 index, f32 x, f32 y, f32 snap) {
    if (index < 0 || static_cast<usize>(index) >= a.samples.size()) return;
    anim::BlendSample& s = a.samples[static_cast<usize>(index)];
    s.x = std::clamp(snapTo(x, a.axisX.min, snap), a.axisX.min, a.axisX.max);
    s.y = a.dims >= 2 ? std::clamp(snapTo(y, a.axisY.min, snap), a.axisY.min, a.axisY.max) : 0.0f;
}

void bsSetDims(anim::BlendSpaceAsset& a, u8 dims) {
    if (dims != 1 && dims != 2) return;
    a.dims = dims;
    if (dims == 1)
        for (anim::BlendSample& s : a.samples) s.y = 0.0f;
}

i32 bsAddMarker(anim::BlendSpaceAsset& a, i32 sample, const std::string& name, f32 time) {
    if (sample < 0 || static_cast<usize>(sample) >= a.samples.size()) return -1;
    auto& m = a.samples[static_cast<usize>(sample)].markers;
    const auto at = std::upper_bound(m.begin(), m.end(), time,
                                     [](f32 t, const anim::SyncMarker& k) { return t < k.time; });
    const i32 idx = static_cast<i32>(at - m.begin());
    m.insert(at, anim::SyncMarker{name, time});
    return idx;
}

bool bsRemoveMarker(anim::BlendSpaceAsset& a, i32 sample, i32 marker) {
    if (sample < 0 || static_cast<usize>(sample) >= a.samples.size()) return false;
    auto& m = a.samples[static_cast<usize>(sample)].markers;
    if (marker < 0 || static_cast<usize>(marker) >= m.size()) return false;
    m.erase(m.begin() + marker);
    return true;
}

void bsToScreen(const anim::BlendSpaceAsset& a, const BsView& v, f32 x, f32 y, f32& sx, f32& sy) {
    sx = v.ox + (x - a.axisX.min) / rangeOf(a.axisX) * v.w;
    sy = a.dims >= 2 ? v.oy + v.h - (y - a.axisY.min) / rangeOf(a.axisY) * v.h : v.oy + v.h * 0.5f;
}

void bsFromScreen(const anim::BlendSpaceAsset& a, const BsView& v, f32 sx, f32 sy, f32& x, f32& y) {
    x = a.axisX.min + (sx - v.ox) / (v.w > 1e-3f ? v.w : 1.0f) * rangeOf(a.axisX);
    y = a.dims >= 2 ? a.axisY.min + (v.oy + v.h - sy) / (v.h > 1e-3f ? v.h : 1.0f) * rangeOf(a.axisY) : 0.0f;
}

i32 bsPick(const anim::BlendSpaceAsset& a, const BsView& v, f32 sx, f32 sy, f32 radius) {
    i32 best = -1;
    f32 bestD = radius * radius;
    for (usize i = 0; i < a.samples.size(); ++i) {
        f32 px, py;
        bsToScreen(a, v, a.samples[i].x, a.samples[i].y, px, py);
        const f32 d = (px - sx) * (px - sx) + (py - sy) * (py - sy);
        if (d <= bestD) { bestD = d; best = static_cast<i32>(i); }
    }
    return best;
}

// ------------------------------------------------------------------------------- the tab ----------

BlendSpaceEditor::BlendSpaceEditor(std::string path) : path_(std::move(path)) { loadFromDisk(); }

void BlendSpaceEditor::loadFromDisk() {
    std::string why;
    anim::BlendSpaceAsset a;
    if (!anim::loadBlendSpace(path_, a, &why)) {
        loaded_ = false;
        loadError_ = why;
        return;
    }
    asset_ = std::move(a);
    loaded_ = true;
    loadError_.clear();
    dirty_ = false;
    selected_ = asset_.samples.empty() ? -1 : 0;
    probeX_ = asset_.axisX.min;
    probeY_ = asset_.axisY.min;
    history_.clear();
    refreshTopology();
}

void BlendSpaceEditor::refreshTopology() {
    topo_ = anim::buildBlendTopology(asset_);
    anim::computeBlendWeights(asset_, topo_, probeX_, probeY_, weights_);
}

std::string BlendSpaceEditor::title() const {
    return std::filesystem::path(path_).filename().string() + "###blend:" + path_;
}

bool BlendSpaceEditor::save(std::string* why) {
    if (!loaded_) {
        if (why) *why = "cannot save: file failed to load (" + loadError_ + ")";
        return false;
    }
    if (!anim::saveBlendSpace(path_, asset_, why)) return false;
    dirty_ = false;
    return true;
}

void BlendSpaceEditor::onFileChanged() {
    if (dirty_) {
        AVER_WARN("[BlendSpaceEditor] '{}' changed on disk, but this tab has unsaved edits -- keeping them", path_);
        return;
    }
    loadFromDisk();
}

void BlendSpaceEditor::undo() {
    if (!history_.undo(asset_)) return;
    dirty_ = true;
    if (selected_ >= static_cast<i32>(asset_.samples.size())) selected_ = static_cast<i32>(asset_.samples.size()) - 1;
    refreshTopology();
}

void BlendSpaceEditor::redo() {
    if (!history_.redo(asset_)) return;
    dirty_ = true;
    if (selected_ >= static_cast<i32>(asset_.samples.size())) selected_ = static_cast<i32>(asset_.samples.size()) - 1;
    refreshTopology();
}

i32 BlendSpaceEditor::addSample(const std::string& clip, f32 x, f32 y) {
    if (!loaded_) return -1;
    pushUndo();
    selected_ = bsAddSample(asset_, clip, x, y);
    dirty_ = true;
    refreshTopology();
    return selected_;
}

void BlendSpaceEditor::deleteSelected() {
    if (!loaded_ || selected_ < 0) return;
    pushUndo();
    if (!bsRemoveSample(asset_, selected_)) { history_.cancelPush(); return; }
    selected_ = std::min(selected_, static_cast<i32>(asset_.samples.size()) - 1);
    dirty_ = true;
    refreshTopology();
}

void BlendSpaceEditor::moveSample(i32 index, f32 x, f32 y) {
    if (!loaded_) return;
    bsMoveSample(asset_, index, x, y, snap_);
    dirty_ = true;
    refreshTopology();
}

void BlendSpaceEditor::setDims(u8 dims) {
    if (!loaded_ || dims == asset_.dims) return;
    pushUndo();
    bsSetDims(asset_, dims);
    dirty_ = true;
    refreshTopology();
}

void BlendSpaceEditor::setProbe(f32 x, f32 y) {
    probeX_ = std::clamp(x, asset_.axisX.min, asset_.axisX.max);
    probeY_ = asset_.dims >= 2 ? std::clamp(y, asset_.axisY.min, asset_.axisY.max) : 0.0f;
    anim::computeBlendWeights(asset_, topo_, probeX_, probeY_, weights_);
}

#if AVER_WITH_IMGUI

namespace {
std::string stem(const std::string& p) { return std::filesystem::path(p).stem().string(); }
} // namespace

void BlendSpaceEditor::drawCanvas() {
    const f32 dpi = ImGui::GetFontSize() / 16.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const f32 pad = 28.0f * dpi;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size(std::max(avail.x, 120.0f * dpi), std::max(avail.y, 120.0f * dpi));
    ImGui::InvisibleButton("##bscanvas", size, ImGuiButtonFlags_MouseButtonLeft);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    BsView view;
    view.ox = p0.x + pad;
    view.oy = p0.y + pad * 0.5f;
    view.w = size.x - pad * 1.5f;
    view.h = asset_.dims >= 2 ? size.y - pad * 1.5f : 40.0f * dpi;

    const ImU32 colGrid = IM_COL32(120, 120, 130, 70), colText = IM_COL32(200, 200, 205, 255);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(24, 25, 29, 255));
    for (int i = 0; i <= 4; ++i) {
        const f32 t = static_cast<f32>(i) / 4.0f;
        dl->AddLine(ImVec2(view.ox + t * view.w, view.oy), ImVec2(view.ox + t * view.w, view.oy + view.h), colGrid);
        if (asset_.dims >= 2) dl->AddLine(ImVec2(view.ox, view.oy + t * view.h), ImVec2(view.ox + view.w, view.oy + t * view.h), colGrid);
        char b[32];
        std::snprintf(b, sizeof b, "%g", asset_.axisX.min + t * (asset_.axisX.max - asset_.axisX.min));
        dl->AddText(ImVec2(view.ox + t * view.w - 8.0f * dpi, view.oy + view.h + 4.0f * dpi), colText, b);
        if (asset_.dims >= 2) {
            std::snprintf(b, sizeof b, "%g", asset_.axisY.max - t * (asset_.axisY.max - asset_.axisY.min));
            dl->AddText(ImVec2(p0.x + 2.0f * dpi, view.oy + t * view.h - 6.0f * dpi), colText, b);
        }
    }
    dl->AddText(ImVec2(view.ox + view.w * 0.5f - 20.0f * dpi, view.oy + view.h + 16.0f * dpi), colText, asset_.axisX.name.c_str());
    if (asset_.dims >= 2) dl->AddText(ImVec2(p0.x + 2.0f * dpi, p0.y + 2.0f * dpi), colText, asset_.axisY.name.c_str());

    // The mesh the weights are taken from, so a hole or a sliver is visible rather than inferred.
    if (asset_.dims >= 2) {
        for (const auto& t : topo_.tris) {
            ImVec2 q[3];
            for (int k = 0; k < 3; ++k) {
                const anim::BlendSample& s = asset_.samples[t[k]];
                f32 sx, sy;
                bsToScreen(asset_, view, s.x, s.y, sx, sy);
                q[k] = ImVec2(sx, sy);
            }
            bool hot = false;
            for (const anim::SampleWeight& w : weights_)
                if (w.weight > 0.0f && (w.index == t[0] || w.index == t[1] || w.index == t[2])) hot = true;
            if (hot && weights_.size() == 3) dl->AddTriangleFilled(q[0], q[1], q[2], IM_COL32(70, 120, 200, 60));
            dl->AddTriangle(q[0], q[1], q[2], IM_COL32(110, 130, 170, 140));
        }
    } else {
        const f32 y = view.oy + view.h * 0.5f;
        dl->AddLine(ImVec2(view.ox, y), ImVec2(view.ox + view.w, y), IM_COL32(110, 130, 170, 160), 2.0f);
    }

    // Samples.
    const f32 r = 6.0f * dpi;
    for (usize i = 0; i < asset_.samples.size(); ++i) {
        const anim::BlendSample& s = asset_.samples[i];
        f32 sx, sy;
        bsToScreen(asset_, view, s.x, s.y, sx, sy);
        f32 w = 0.0f;
        for (const anim::SampleWeight& e : weights_)
            if (e.index == i) w = e.weight;
        const bool sel = static_cast<i32>(i) == selected_;
        dl->AddCircleFilled(ImVec2(sx, sy), r + w * 6.0f * dpi, sel ? IM_COL32(255, 190, 70, 255) : IM_COL32(120, 190, 255, 255));
        dl->AddCircle(ImVec2(sx, sy), r + w * 6.0f * dpi, IM_COL32(0, 0, 0, 200));
        char label[96];
        std::snprintf(label, sizeof label, "%s %.0f%%", stem(s.clip).c_str(), w * 100.0f);
        dl->AddText(ImVec2(sx + r + 4.0f * dpi, sy - 7.0f * dpi), colText, label);
    }

    // The probe.
    f32 px, py;
    bsToScreen(asset_, view, probeX_, probeY_, px, py);
    const f32 d = 7.0f * dpi;
    dl->AddQuadFilled(ImVec2(px, py - d), ImVec2(px + d, py), ImVec2(px, py + d), ImVec2(px - d, py), IM_COL32(255, 80, 80, 255));

    // Interaction. A drag that starts on a sample moves it; one that starts on the probe moves the
    // probe; a click on empty space puts the probe there. Double-click empty space adds a sample.
    const ImVec2 m = ImGui::GetIO().MousePos;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const i32 hit = bsPick(asset_, view, m.x, m.y, 10.0f * dpi);
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hit < 0) {
            f32 x, y;
            bsFromScreen(asset_, view, m.x, m.y, x, y);
            const std::string clip = selected_ >= 0 ? asset_.samples[static_cast<usize>(selected_)].clip : std::string("Content/Anim/Clip.ocanim");
            addSample(clip, snapTo(x, asset_.axisX.min, snap_), snapTo(y, asset_.axisY.min, snap_));
        } else if (hit >= 0) {
            selected_ = hit;
            pushUndo();
            dragging_ = true;
        } else {
            draggingProbe_ = true;
            f32 x, y;
            bsFromScreen(asset_, view, m.x, m.y, x, y);
            setProbe(x, y);
        }
    }
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        f32 x, y;
        bsFromScreen(asset_, view, m.x, m.y, x, y);
        if (dragging_ && selected_ >= 0) moveSample(selected_, x, y);
        else if (draggingProbe_) setProbe(x, y);
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { dragging_ = false; draggingProbe_ = false; }
    if (hovered && selected_ >= 0 && ImGui::IsKeyPressed(ImGuiKey_Delete)) deleteSelected();
}

void BlendSpaceEditor::drawDetails() {
    anim::BlendSpaceAsset before = asset_;
    bool gestureEnded = false;
    auto touch = [&](bool changed) {
        if (changed) {
            if (!gesture_) { history_.push(before); gesture_ = true; }
            dirty_ = true;
            refreshTopology();
        }
        if (ImGui::IsItemDeactivated()) gestureEnded = true;
    };

    char buf[256];
    std::snprintf(buf, sizeof buf, "%s", asset_.name.c_str());
    if (ImGui::InputText("Name", buf, sizeof buf)) { asset_.name = buf; touch(true); } else touch(false);

    int dims = asset_.dims;
    if (ImGui::RadioButton("1D", dims == 1)) setDims(1);
    ImGui::SameLine();
    if (ImGui::RadioButton("2D", dims == 2)) setDims(2);
    ImGui::SameLine();
    touch(ImGui::Checkbox("Sync markers", &asset_.syncMarkers));
    ImGui::DragFloat("Snap", &snap_, 0.5f, 0.0f, 1000.0f, "%.1f");

    auto axisUi = [&](const char* label, anim::BlendAxis& ax) {
        ImGui::PushID(label);
        ImGui::SeparatorText(label);
        std::snprintf(buf, sizeof buf, "%s", ax.name.c_str());
        if (ImGui::InputText("Parameter", buf, sizeof buf)) { ax.name = buf; touch(true); } else touch(false);
        touch(ImGui::DragFloat("Min", &ax.min, 1.0f));
        touch(ImGui::DragFloat("Max", &ax.max, 1.0f));
        if (ax.max <= ax.min) ax.max = ax.min + 1.0f;
        touch(ImGui::DragFloat("Smoothing (s)", &ax.smoothing, 0.01f, 0.0f, 5.0f));
        ImGui::PopID();
    };
    axisUi("Horizontal axis", asset_.axisX);
    if (asset_.dims >= 2) axisUi("Vertical axis", asset_.axisY);

    ImGui::SeparatorText("Samples");
    for (usize i = 0; i < asset_.samples.size(); ++i) {
        const std::string label = std::to_string(i) + "  " + stem(asset_.samples[i].clip);
        if (ImGui::Selectable(label.c_str(), static_cast<i32>(i) == selected_)) selected_ = static_cast<i32>(i);
    }
    if (ImGui::Button("Add sample")) {
        const anim::BlendSample& ref = asset_.samples.empty() ? anim::BlendSample{} : asset_.samples.back();
        addSample(ref.clip.empty() ? "Content/Anim/Clip.ocanim" : ref.clip, probeX_, probeY_);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(selected_ < 0 || asset_.samples.size() <= 1);
    if (ImGui::Button("Delete sample")) deleteSelected();
    ImGui::EndDisabled();

    if (selected_ >= 0 && static_cast<usize>(selected_) < asset_.samples.size()) {
        anim::BlendSample& s = asset_.samples[static_cast<usize>(selected_)];
        ImGui::PushID(selected_);
        ImGui::SeparatorText("Selected sample");
        std::snprintf(buf, sizeof buf, "%s", s.clip.c_str());
        if (ImGui::InputText("Clip", buf, sizeof buf)) { s.clip = buf; touch(true); } else touch(false);
        touch(ImGui::DragFloat("X", &s.x, 0.5f, asset_.axisX.min, asset_.axisX.max));
        if (asset_.dims >= 2) touch(ImGui::DragFloat("Y", &s.y, 0.5f, asset_.axisY.min, asset_.axisY.max));
        touch(ImGui::DragFloat("Rate", &s.rate, 0.01f, 0.1f, 4.0f));
        ImGui::TextDisabled("Sync markers: same names, same order in every clip (a cycle: L R L R ...)");
        int removeAt = -1;
        for (usize k = 0; k < s.markers.size(); ++k) {
            ImGui::PushID(static_cast<int>(k));
            std::snprintf(buf, sizeof buf, "%s", s.markers[k].name.c_str());
            ImGui::SetNextItemWidth(90.0f * (ImGui::GetFontSize() / 16.0f));
            if (ImGui::InputText("##mn", buf, sizeof buf)) { s.markers[k].name = buf; touch(true); } else touch(false);
            ImGui::SameLine();
            touch(ImGui::DragFloat("##mt", &s.markers[k].time, 0.005f, 0.0f, 600.0f, "%.3f s"));
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) removeAt = static_cast<int>(k);
            ImGui::PopID();
        }
        if (removeAt >= 0) {
            pushUndo();
            bsRemoveMarker(asset_, selected_, removeAt);
            dirty_ = true;
            refreshTopology();
        }
        if (ImGui::Button("Add marker")) {
            pushUndo();
            bsAddMarker(asset_, selected_, s.markers.empty() ? "L" : (s.markers.size() % 2 ? "R" : "L"),
                        s.markers.empty() ? 0.0f : s.markers.back().time + 0.25f);
            dirty_ = true;
        }
        ImGui::PopID();
    }

    ImGui::SeparatorText("Probe");
    ImGui::Text("%s = %.2f", asset_.axisX.name.c_str(), probeX_);
    if (asset_.dims >= 2) ImGui::Text("%s = %.2f", asset_.axisY.name.c_str(), probeY_);
    for (const anim::SampleWeight& w : weights_) {
        if (w.index >= asset_.samples.size()) continue;
        ImGui::ProgressBar(w.weight, ImVec2(-1, 0), (stem(asset_.samples[w.index].clip) + "  " + std::to_string(static_cast<int>(w.weight * 100.0f + 0.5f)) + "%").c_str());
    }

    std::string why;
    if (!asset_.valid(&why)) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "Invalid: %s", why.c_str());
    if (gestureEnded) gesture_ = false;
}

void BlendSpaceEditor::draw(Engine& e) {
    (void)e;
    if (!loaded_) {
        ImGui::TextWrapped("This file could not be read: %s", loadError_.c_str());
        return;
    }
    if (ImGui::Button("Save") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                                  keybinds().pressed(CommandId::AssetSave, ImGui::GetIO()))) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[BlendSpaceEditor] save failed for '{}': {}", path_, why);
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
    ImGui::TextDisabled("click: probe   drag a point: move   double-click: add   Del: remove");
    ImGui::Separator();

    const f32 dpi = ImGui::GetFontSize() / 16.0f;
    const f32 detailsW = std::min(ImGui::GetContentRegionAvail().x * 0.4f, 340.0f * dpi);
    if (ImGui::BeginChild("##bscanvaspane", ImVec2(ImGui::GetContentRegionAvail().x - detailsW - 8.0f * dpi, 0), true)) drawCanvas();
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##bsdetails", ImVec2(0, 0), true)) drawDetails();
    ImGui::EndChild();
}

#else   // AVER_WITH_IMGUI

void BlendSpaceEditor::draw(Engine& e) { (void)e; }

#endif  // AVER_WITH_IMGUI

std::unique_ptr<AssetEditor> makeBlendSpaceEditor(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".ocblend") return nullptr;
    return std::make_unique<BlendSpaceEditor>(path);
}

} // namespace aver::editor
