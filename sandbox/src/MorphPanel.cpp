// The blend-shape sliders. See MorphPanel.hpp.
#include "MorphPanel.hpp"

#if AVER_WITH_IMGUI
#include "imgui.h"
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace aver::editor {

void MorphPanel::setTargets(const std::vector<std::string>& names, const std::vector<f32>& defaults) {
    if (names == names_) { defaults_ = defaults; defaults_.resize(names_.size(), 0.0f); return; }
    names_ = names;
    defaults_ = defaults;
    defaults_.resize(names_.size(), 0.0f);
    live_.assign(names_.size(), kNone);
    override_.assign(names_.size(), kNone);
}

void MorphPanel::setPoses(const std::vector<std::string>& names) {
    if (names == poseNames_) return;
    poseNames_ = names;
    livePose_.assign(poseNames_.size(), kNone);
    poseOverride_.assign(poseNames_.size(), kNone);
}

bool MorphPanel::anyOverride() const {
    for (const f32 v : override_) if (!isNone(v)) return true;
    for (const f32 v : poseOverride_) if (!isNone(v)) return true;
    return false;
}

void MorphPanel::release() {
    std::fill(override_.begin(), override_.end(), kNone);
    std::fill(poseOverride_.begin(), poseOverride_.end(), kNone);
}

bool MorphPanel::takeKeyRequest(MorphKeyRequest& out) {
    if (!keyPending_) return false;
    keyPending_ = false;
    out = key_;
    return true;
}

bool MorphPanel::matches(const std::string& name) const {
    if (filter_[0] == '\0') return true;
    const usize n = std::strlen(filter_);
    if (n > name.size()) return false;
    for (usize i = 0; i + n <= name.size(); ++i) {
        usize k = 0;
        while (k < n && std::tolower(static_cast<unsigned char>(name[i + k])) == std::tolower(static_cast<unsigned char>(filter_[k]))) ++k;
        if (k == n) return true;
    }
    return false;
}

bool MorphPanel::draw(const char* id) {
#if AVER_WITH_IMGUI
    bool changed = false;
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##filter", "Search blend shapes", filter_, sizeof filter_);
    if (ImGui::SmallButton("Zero all")) {
        for (f32& v : override_) v = 0.0f;
        for (f32& v : poseOverride_) v = 0.0f;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset to defaults")) {
        // Every target at its authored default: an override equal to the default, so the animation is out of the way.
        for (usize i = 0; i < override_.size(); ++i) override_[i] = defaults_[i];
        for (f32& v : poseOverride_) v = 0.0f;
        changed = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!anyOverride());
    if (ImGui::SmallButton("Release")) { release(); changed = true; }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Hand the sliders back to the animation.");
    if (!stats_.empty()) ImGui::TextDisabled("%s", stats_.c_str());

    if (ImGui::BeginTabBar("##morphTabs")) {
        if (ImGui::BeginTabItem("Targets")) { drawTargets(changed); ImGui::EndTabItem(); }
        if (!poseNames_.empty() && ImGui::BeginTabItem("Poses")) { drawPoses(changed); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::PopID();
    return changed;
#else
    (void)id;
    return false;
#endif
}

void MorphPanel::drawTargets(bool& changed) {
#if AVER_WITH_IMGUI
    for (usize i = 0; i < names_.size(); ++i) {
        if (!matches(names_[i])) continue;
        ImGui::PushID(static_cast<int>(i));
        const f32 live = i < live_.size() ? live_[i] : kNone;
        const bool overridden = !isNone(override_[i]);
        f32 shown = overridden ? override_[i] : (isNone(live) ? defaults_[i] : live);
        ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x * 0.5f));
        if (ImGui::SliderFloat("##w", &shown, -1.0f, 2.0f, "%.3f")) { override_[i] = shown; changed = true; }
        // The animation's own value as a tick on the slider, so a slider the author moved can still be compared with it.
        if (!isNone(live)) {
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const f32 t = std::min(1.0f, std::max(0.0f, (live + 1.0f) / 3.0f));
            const f32 x = a.x + (b.x - a.x) * t;
            ImGui::GetWindowDrawList()->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), IM_COL32(255, 200, 60, 255), 2.0f);
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { override_[i] = kNone; changed = true; }
        ImGui::SameLine();
        if (overridden) ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "%s", names_[i].c_str());
        else            ImGui::TextUnformatted(names_[i].c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("S")) {
            for (usize k = 0; k < override_.size(); ++k) override_[k] = 0.0f;
            override_[i] = 1.0f;
            changed = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Solo: only this shape, at 1.");
        if (keying_) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Key")) { key_ = MorphKeyRequest{names_[i], false, shown}; keyPending_ = true; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a key at the playhead to this shape's morph curve.");
        }
        ImGui::PopID();
    }
#else
    (void)changed;
#endif
}

void MorphPanel::drawPoses(bool& changed) {
#if AVER_WITH_IMGUI
    for (usize i = 0; i < poseNames_.size(); ++i) {
        if (!matches(poseNames_[i])) continue;
        ImGui::PushID(static_cast<int>(i));
        const f32 live = i < livePose_.size() ? livePose_[i] : kNone;
        const bool overridden = !isNone(poseOverride_[i]);
        f32 shown = overridden ? poseOverride_[i] : (isNone(live) ? 0.0f : live);
        ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x * 0.5f));
        if (ImGui::SliderFloat("##p", &shown, 0.0f, 1.0f, "%.3f")) { poseOverride_[i] = shown; changed = true; }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { poseOverride_[i] = kNone; changed = true; }
        ImGui::SameLine();
        if (overridden) ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "%s", poseNames_[i].c_str());
        else            ImGui::TextUnformatted(poseNames_[i].c_str());
        if (keying_) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Key")) { key_ = MorphKeyRequest{poseNames_[i], true, shown}; keyPending_ = true; }
        }
        ImGui::PopID();
    }
#else
    (void)changed;
#endif
}

} // namespace aver::editor
