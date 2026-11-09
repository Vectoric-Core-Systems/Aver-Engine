#include "PhysAnimEditor.hpp"
#include "EditorChrome.hpp"

#include "aver/core/Log.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <map>

#if AVER_WITH_IMGUI
#include "EditorIcons.hpp"
#include "imgui.h"
#endif

namespace aver::editor {

namespace fs = std::filesystem;

namespace {

// The skeleton each profile was last previewed against, kept for the session (the file itself names none).
std::map<std::string, std::string>& chosenSkeletons() {
    static std::map<std::string, std::string> m;
    return m;
}

// The nearest ancestor folder named "Content", else the profile's own folder.
fs::path searchRoot(const std::string& profilePath) {
    fs::path p = fs::path(profilePath).parent_path();
    for (fs::path at = p; !at.empty() && at.has_parent_path() && at != at.parent_path(); at = at.parent_path())
        if (at.filename() == "Content") return at;
    return p;
}

}  // namespace

ragdoll::PhysAnimProfile physAnimStarter(const std::string& name) {
    ragdoll::PhysAnimProfile p;
    p.name = name;
    return p;
}

PhysAnimEditor::PhysAnimEditor(std::string path) : path_(std::move(path)) {
    loadFromDisk();
    if (const auto it = chosenSkeletons().find(path_); it != chosenSkeletons().end()) setSkeleton(it->second);
}

std::string PhysAnimEditor::title() const { return fs::path(path_).filename().string() + (dirty_ ? " *" : ""); }

void PhysAnimEditor::loadFromDisk() {
    ragdoll::PhysAnimProfile p;
    std::string why;
    if (!ragdoll::loadPhysAnimProfile(path_, p, &why)) {
        loaded_ = false;
        loadError_ = why;
        return;
    }
    profile_ = std::move(p);
    loaded_ = true;
    loadError_.clear();
    dirty_ = false;
    history_.clear();
    selRegion_ = -1;
    previewStale_ = true;
}

void PhysAnimEditor::onFileChanged() {
    if (!dirty_) loadFromDisk();   // an unsaved edit wins over the disk until the user decides
}

bool PhysAnimEditor::save(std::string* why) {
    physanim::clampProfile(profile_);
    if (!ragdoll::savePhysAnimProfile(path_, profile_, why)) return false;
    dirty_ = false;
    return true;
}

void PhysAnimEditor::edit(const std::function<void(ragdoll::PhysAnimProfile&)>& fn) {
    history_.push(profile_);
    fn(profile_);
    dirty_ = true;
    previewStale_ = true;
}

void PhysAnimEditor::undo() {
    if (history_.undo(profile_)) { dirty_ = true; previewStale_ = true; selRegion_ = std::min(selRegion_, static_cast<int>(profile_.regions.size()) - 1); }
}

void PhysAnimEditor::redo() {
    if (history_.redo(profile_)) { dirty_ = true; previewStale_ = true; selRegion_ = std::min(selRegion_, static_cast<int>(profile_.regions.size()) - 1); }
}

bool PhysAnimEditor::setSkeleton(const std::string& skelPath) {
    fmt::OcSkeleton s;
    std::string why;
    if (!fmt::loadOcSkel(skelPath, s, &why)) return false;
    skel_ = std::move(s);
    skelPath_ = skelPath;
    skelOk_ = true;
    chosenSkeletons()[path_] = skelPath;
    selBone_ = -1;
    previewStale_ = true;
    return true;
}

std::vector<std::string> PhysAnimEditor::problems() const {
    std::vector<std::string> out;
    if (skelOk_) ragdoll::validatePhysAnimProfile(profile_, skel_, &out);
    return out;
}

void PhysAnimEditor::rescanSkeletons() {
    skelChoices_.clear();
    skelScanned_ = true;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(searchRoot(path_), fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (skelChoices_.size() >= 300) break;
        if (!it->is_regular_file(ec)) continue;
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".ocskel") skelChoices_.push_back(it->path().string());
    }
    std::sort(skelChoices_.begin(), skelChoices_.end());
}

void PhysAnimEditor::refreshPreview() {
    previewStale_ = false;
    preview_ = physanim::HitPreview{};
    if (!skelOk_ || selBone_ < 0) return;
    preview_ = physanim::previewHit(profile_, skel_, selBone_, impulse_ > 0.0f ? impulse_ : profile_.fullImpulse);
}

#if AVER_WITH_IMGUI

namespace {

ImU32 regionColor(int region) {
    if (region < 0) return IM_COL32(120, 120, 130, 255);
    static const ImU32 palette[] = {IM_COL32(255, 140, 70, 255), IM_COL32(90, 180, 255, 255), IM_COL32(130, 220, 120, 255), IM_COL32(220, 120, 230, 255),
                                    IM_COL32(250, 210, 80, 255), IM_COL32(100, 220, 210, 255), IM_COL32(240, 100, 110, 255), IM_COL32(170, 150, 255, 255)};
    return palette[region % 8];
}

}  // namespace

void PhysAnimEditor::draw(Engine&) {
    if (!loaded_) {
        ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.3f, 1.0f), "Could not read %s", path_.c_str());
        ImGui::TextWrapped("%s", loadError_.c_str());
        return;
    }
    // The "before" of a drag is the profile as of the last frame nothing was being edited.
    if (!ImGui::IsAnyItemActive()) snap_ = profile_;
    const auto beginDrag = [&]() { if (ImGui::IsItemActivated()) { history_.push(snap_); } };
    const auto touched = [&]() { dirty_ = true; previewStale_ = true; };

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::GetIO().KeyCtrl && !ImGui::IsAnyItemActive()) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (ImGui::GetIO().KeyShift) redo(); else undo(); }
        else if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) redo();
    }

    // Sizes are logical px scaled by the UI font, so the swatches, indents and columns keep their proportions at 300% DPI.
    const f32 dpi = fontScale();
    const f32 lineH = ImGui::GetTextLineHeight();
    const auto swatch = [&](ImU32 col) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + lineH * 0.15f), ImVec2(p.x + 6.0f * dpi, p.y + lineH * 0.9f), col);
        ImGui::Dummy(ImVec2(10.0f * dpi, lineH));
        ImGui::SameLine();
    };

    // ---- toolbar ----
    ImGui::BeginDisabled(!history_.canUndo());
    if (ImGui::Button("Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!history_.canRedo());
    if (ImGui::Button("Redo")) redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Skeleton");
    ImGui::SameLine();
    if (!skelScanned_) rescanSkeletons();
    ImGui::SetNextItemWidth(320.0f * dpi);
    if (ImGui::BeginCombo("##skel", skelOk_ ? fs::path(skelPath_).filename().string().c_str() : "(pick a skeleton to preview)")) {
        for (const std::string& c : skelChoices_)
            if (ImGui::Selectable(fs::path(c).filename().string().c_str(), c == skelPath_)) setSkeleton(c);
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The profile names bones but not a skeleton. Pick the one to list and preview against.\nNothing here is saved with the profile.");
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_REFRESH)) rescanSkeletons();
    ImGui::Separator();

    if (previewStale_) refreshPreview();
    const f32 rightW = std::max(360.0f * dpi, ImGui::GetContentRegionAvail().x * 0.5f);

    // ---- left: the skeleton ----
    ImGui::BeginChild("##bones", ImVec2(ImGui::GetContentRegionAvail().x - rightW - 8.0f * dpi, 0), true);
    ImGui::TextDisabled("Bones  (click one to aim the preview hit at it)");
    if (!skelOk_) {
        ImGui::TextWrapped("Pick a skeleton above to list its bones.");
    } else {
        const auto rows = physanim::boneRows(profile_, skel_);
        f32 nameColX = 0.0f;
        for (const physanim::BoneRow& row : rows)
            nameColX = std::max(nameColX, ImGui::GetCursorPosX() + static_cast<f32>(row.depth) * 14.0f * dpi + 10.0f * dpi +
                                          ImGui::GetStyle().ItemSpacing.x + ImGui::CalcTextSize(skel_.bones[static_cast<usize>(row.bone)].name.c_str()).x);
        nameColX += 16.0f * dpi;
        for (const physanim::BoneRow& row : rows) {
            ImGui::PushID(row.bone);
            // Indent(0) means "the default indent" in ImGui, which pushed every root bone in past its children.
            if (row.depth > 0) ImGui::Indent(static_cast<f32>(row.depth) * 14.0f * dpi);
            swatch(regionColor(row.region));
            const f32 heat = preview_.valid && row.bone < static_cast<int>(preview_.peak.size()) ? preview_.peak[static_cast<usize>(row.bone)] : 0.0f;
            if (heat > 0.001f) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f + 0.45f * (1.0f - heat), 0.35f * (1.0f - heat), 1.0f));
            if (ImGui::Selectable(skel_.bones[static_cast<usize>(row.bone)].name.c_str(), selBone_ == row.bone, ImGuiSelectableFlags_AllowOverlap)) { selBone_ = row.bone; previewStale_ = true; }
            if (heat > 0.001f) ImGui::PopStyleColor();
            if (preview_.valid) {
                // A column after the longest bone name, not a fixed 220 px that long names ran into.
                ImGui::SameLine(nameColX);   // the Selectable spans the row (AllowOverlap), so this is an absolute column
                ImGui::TextDisabled("%s", row.region >= 0 ? profile_.regions[static_cast<usize>(row.region)].name.c_str() : "default");
                if (heat > 0.001f) { ImGui::SameLine(); ImGui::TextDisabled("%.0f%%", heat * 100.0f); }
            }
            if (row.depth > 0) ImGui::Unindent(static_cast<f32>(row.depth) * 14.0f * dpi);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ---- right: regions, drive, reach, preview, problems ----
    ImGui::BeginChild("##profile", ImVec2(0, 0), false);
    ImGui::TextDisabled("Regions  (when two regions name the same bone, the later one wins)");
    if (ImGui::Selectable("(defaults: bones in no region)", selRegion_ < 0)) selRegion_ = -1;
    for (int i = 0; i < static_cast<int>(profile_.regions.size()); ++i) {
        ImGui::PushID(i);
        const ragdoll::PhysAnimRegion& r = profile_.regions[static_cast<usize>(i)];
        swatch(regionColor(i));
        if (ImGui::Selectable((r.name + "  (" + std::to_string(r.bones.size()) + " bones, " + std::to_string(r.subtrees.size()) + " subtrees)").c_str(), selRegion_ == i)) selRegion_ = i;
        ImGui::PopID();
    }
    if (ImGui::Button(ICON_ADD " Region")) {
        const std::string name = physanim::freshRegionName(profile_);
        history_.push(profile_);
        selRegion_ = physanim::addRegion(profile_, name);
        touched();
    }
    const bool haveRegion = selRegion_ >= 0 && selRegion_ < static_cast<int>(profile_.regions.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(!haveRegion);
    if (ImGui::Button(ICON_DELETE)) { history_.push(profile_); physanim::removeRegion(profile_, selRegion_); selRegion_ = std::min(selRegion_, static_cast<int>(profile_.regions.size()) - 1); touched(); }
    ImGui::SameLine();
    if (ImGui::Button("Up")) { history_.push(profile_); const int to = physanim::moveRegion(profile_, selRegion_, -1); if (to >= 0) selRegion_ = to; touched(); }
    ImGui::SameLine();
    if (ImGui::Button("Down")) { history_.push(profile_); const int to = physanim::moveRegion(profile_, selRegion_, 1); if (to >= 0) selRegion_ = to; touched(); }
    ImGui::SameLine();
    if (ImGui::Button("Rename")) {
        renaming_ = true;
        std::snprintf(renameBuf_, sizeof renameBuf_, "%s", profile_.regions[static_cast<usize>(selRegion_)].name.c_str());
    }
    ImGui::EndDisabled();
    if (renaming_ && haveRegion) {
        ImGui::SetNextItemWidth(200.0f * dpi);
        if (ImGui::InputText("##rn", renameBuf_, sizeof renameBuf_, ImGuiInputTextFlags_EnterReturnsTrue)) {
            history_.push(profile_);
            if (!physanim::renameRegion(profile_, selRegion_, renameBuf_)) history_.cancelPush();
            else touched();
            renaming_ = false;
        } else if (ImGui::IsItemDeactivated()) renaming_ = false;
    }

    ImGui::BeginDisabled(!haveRegion || selBone_ < 0 || !skelOk_);
    const std::string boneName = selBone_ >= 0 && skelOk_ ? skel_.bones[static_cast<usize>(selBone_)].name : std::string();
    if (ImGui::Button("Add bone and below")) { history_.push(profile_); if (physanim::assignBone(profile_, selRegion_, boneName, true)) touched(); else history_.cancelPush(); }
    ImGui::SameLine();
    if (ImGui::Button("Add bone only")) { history_.push(profile_); if (physanim::assignBone(profile_, selRegion_, boneName, false)) touched(); else history_.cancelPush(); }
    ImGui::SameLine();
    if (ImGui::Button("Remove bone")) { history_.push(profile_); if (physanim::unassignBone(profile_, selRegion_, boneName)) touched(); else history_.cancelPush(); }
    ImGui::EndDisabled();

    ragdoll::PhysAnimDrive& d = haveRegion ? profile_.regions[static_cast<usize>(selRegion_)].drive : profile_.defaults;
    ImGui::Separator();
    ImGui::TextDisabled("%s", haveRegion ? "Drive and envelope of the selected region" : "Drive and envelope of a bone in no region");
    const auto slider = [&](const char* label, f32& v, f32 lo, f32 hi, const char* fmtText = "%.2f") {
        if (ImGui::SliderFloat(label, &v, lo, hi, fmtText)) touched();
        beginDrag();
    };
    slider("Strength", d.strength, 0.0f, 1.0f);
    slider("Damping", d.damping, 0.0f, 4.0f);
    slider("Torque scale", d.torque, 0.0f, 4.0f);
    slider("Blend", d.blend, 0.0f, 1.0f);
    slider("Boost at full reaction", d.boost, 0.0f, 1.0f);
    slider("Impulse multiplier", d.impulse, 0.0f, 4.0f);
    slider("Attack (s)", d.attack, 0.0f, 1.0f, "%.3f");
    slider("Hold (s)", d.hold, 0.0f, 1.0f, "%.3f");
    slider("Decay (s)", d.decay, 0.001f, 2.0f, "%.3f");

    ImGui::Separator();
    ImGui::TextDisabled("Reach and life");
    slider("Falloff per hop", profile_.falloffPerHop, 0.0f, 1.0f);
    if (ImGui::SliderInt("Hops", &profile_.maxHops, 0, 12)) touched();
    beginDrag();
    slider("Impulse for a full reaction", profile_.fullImpulse, 100.0f, 20000.0f, "%.0f");
    slider("Death ramp (s)", profile_.deathSeconds, 0.0f, 5.0f);
    slider("Get-up blend (s)", profile_.getUpBlend, 0.0f, 3.0f);
    physanim::clampProfile(profile_);

    ImGui::Separator();
    ImGui::TextDisabled("Hit preview (the real envelope, no physics)");
    if (!skelOk_ || selBone_ < 0) {
        ImGui::TextWrapped("Pick a skeleton and click a bone to preview a hit on it.");
    } else {
        f32 imp = impulse_ > 0.0f ? impulse_ : profile_.fullImpulse;
        if (ImGui::SliderFloat("Hit impulse (kg*cm/s)", &imp, 50.0f, 20000.0f, "%.0f")) { impulse_ = imp; previewStale_ = true; }
        if (preview_.valid) {
            // Plotted to just past the moment it comes to rest: the full 4 s preview squeezed a 0.6 s reaction into the plot's left edge.
            const int total = static_cast<int>(preview_.curve.size());
            const int shown = preview_.dt > 0.0f ? std::min(total, std::max(8, static_cast<int>(preview_.settleSeconds / preview_.dt * 1.2f) + 4)) : total;
            ImGui::PlotLines("##curve", preview_.curve.data(), shown, 0, "Envelope on the hit bone", 0.0f, 1.0f, ImVec2(-1.0f, 80.0f * dpi));
            ImGui::TextDisabled("%u bone(s) react; all at rest after %.2f s", preview_.bonesReached, preview_.settleSeconds);
        }
    }

    const std::vector<std::string> probs = problems();
    if (!probs.empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%zu problem(s)", probs.size());
        for (const std::string& pr : probs) ImGui::BulletText("%s", pr.c_str());
    }
    ImGui::EndChild();
}

#else

void PhysAnimEditor::draw(Engine&) {}

#endif

std::unique_ptr<AssetEditor> makePhysAnimEditor(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".ocphysanim") return nullptr;
    return std::make_unique<PhysAnimEditor>(path);
}

}  // namespace aver::editor
