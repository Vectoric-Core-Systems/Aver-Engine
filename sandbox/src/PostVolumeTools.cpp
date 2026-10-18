// Editor-side post-process volumes. See PostVolumeTools.hpp.
#include "PostVolumeTools.hpp"

#include "aver/assets/LevelSky.hpp"
#include "aver/core/Log.hpp"
#if AVER_MODULE_FRAMEWORK
#  include "aver/framework/framework_abi.h"   // aver_fw_play_state
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#if AVER_WITH_IMGUI
#include "imgui.h"
#endif

namespace aver::editor {

int PostVolumeTools::addAtCamera(std::vector<fmt::OcPostVolume>& vols, const Vec3& cam) {
    // A distinct name per volume, so the list reads: PostProcessVolume, PostProcessVolume2, ...
    std::string name = "PostProcessVolume";
    for (int n = 2;; ++n) {
        const auto taken = std::any_of(vols.begin(), vols.end(), [&](const fmt::OcPostVolume& v) { return v.name == name; });
        if (!taken) break;
        name = "PostProcessVolume" + std::to_string(n);
    }
    vols.push_back(makeNewPostVolume(cam, name));
    selected_ = static_cast<int>(vols.size()) - 1;
    AVER_INFO("[Editor] added Post Process Volume '{}' at ({:.0f}, {:.0f}, {:.0f})", name, cam.x, cam.y, cam.z);
    return selected_;
}

void PostVolumeTools::drawOverlay(rhi::IDevice& dev, const std::vector<fmt::OcPostVolume>& vols, bool showAll) {
    if (mesh_) { dev.destroyLineMesh(mesh_); mesh_ = 0; }   // last frame's, replayed by now
    verts_.clear();
    for (usize i = 0; i < vols.size(); ++i) {
        const fmt::OcPostVolume& v = vols[i];
        const bool sel = static_cast<int>(i) == selected_;
        if (v.infinite || (!sel && !showAll)) continue;
        const f32 r = sel ? 0.35f : 0.25f, g = sel ? 0.85f : 0.3f, b = sel ? 1.0f : 0.5f;
        // The box, and for the selected one the blend shell (the box grown by blendCm) where the weight ramps out.
        for (int shell = 0; shell < (sel && v.blendCm > 0.0 ? 2 : 1); ++shell) {
            const f32 grow = shell ? static_cast<f32>(v.blendCm) : 0.0f;
            const f32 k = shell ? 0.5f : 1.0f;
            f32 lo[3], hi[3];
            for (int a = 0; a < 3; ++a) {
                lo[a] = static_cast<f32>(std::min(v.bounds[a], v.bounds[a + 3])) - grow;
                hi[a] = static_cast<f32>(std::max(v.bounds[a], v.bounds[a + 3])) + grow;
            }
            const auto corner = [&](int c, rhi::LineVertex& o) {
                o = {(c & 1) ? hi[0] : lo[0], (c & 2) ? hi[1] : lo[1], (c & 4) ? hi[2] : lo[2], r * k, g * k, b * k};
            };
            for (int c = 0; c < 8; ++c)
                for (int bit = 1; bit <= 4; bit <<= 1) {
                    if (c & bit) continue;   // each edge once, from its low end
                    rhi::LineVertex a, bb;
                    corner(c, a); corner(c | bit, bb);
                    verts_.push_back(a); verts_.push_back(bb);
                }
        }
    }
    if (verts_.empty()) return;
    mesh_ = dev.createLineMesh(verts_.data(), static_cast<u32>(verts_.size()));
    if (!mesh_) return;
    const Mat4 identity = Mat4::identity();   // vertices are already in world space
    dev.setLineDepth(false);                  // a volume box usually encloses the geometry it affects
    dev.drawLines(mesh_, &identity.m[0][0]);
    dev.setLineDepth(true);
}

rhi::PostSettings PostVolumeTools::blend(rhi::IDevice& dev, const std::string& contentDir,
                                         const std::vector<fmt::OcPostVolume>& vols, const rhi::PostSettings& base,
                                         const Vec3& eye) {
#if AVER_MODULE_FRAMEWORK
    // A Play session only (authoring shows the level as saved): the same volume list the game builds. The level's volumes
    // with their script scales, then the script's post layers as infinite volumes; blended below, and the accessibility
    // override the caller applies afterwards still wins.
    static std::vector<fmt::OcPostVolume> scripted;
    if (aver_fw_play_state() != AVER_FW_PLAY_EDITOR &&
        assets::buildScriptPostVolumes<assets::FrameworkWorldFxApi>(vols, scripted))
        return game::blendPostVolumes(scripted, base, eye, luts_.resolver(dev, contentDir));
#endif
    if (vols.empty()) return base;
    return game::blendPostVolumes(vols, base, eye, luts_.resolver(dev, contentDir));
}

void PostVolumeTools::reset(rhi::IDevice* dev) {
    selected_ = -1;
    cubeScanned_ = false;
    cubeFiles_.clear();
    if (dev && mesh_) { dev->destroyLineMesh(mesh_); mesh_ = 0; }
    luts_.clear(dev);
}

void PostVolumeTools::refreshLuts(const std::string& contentDir) {
    cubeFiles_.clear();
    cubeScanned_ = true;
    std::error_code ec;
    const std::filesystem::path root(contentDir);
    if (!std::filesystem::is_directory(root, ec)) return;
    for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".cube") continue;
        cubeFiles_.push_back(std::filesystem::relative(it->path(), root, ec).generic_string());
    }
    std::sort(cubeFiles_.begin(), cubeFiles_.end());
}

#if AVER_WITH_IMGUI

namespace {

// One override row: a checkbox that adds/removes the field, then its value widget. True when anything changed.
bool overrideRow(fmt::OcPostVolume& v, const fmt::OcPostKeyInfo& ki, const rhi::PostSettings& base) {
    bool changed = false;
    ImGui::PushID(ki.key);
    fmt::OcPostValue* cur = nullptr;
    for (fmt::OcPostValue& e : v.values)
        if (e.key == ki.key) { cur = &e; break; }
    bool on = cur != nullptr;
    if (ImGui::Checkbox("##on", &on)) {
        if (on) {
            f64 seed[3];
            game::readPostField(base, ki.key, seed);
            v.set(ki.key, seed, ki.count);
        } else {
            v.unset(ki.key);
        }
        changed = true;
        cur = nullptr;
        for (fmt::OcPostValue& e : v.values)
            if (e.key == ki.key) { cur = &e; break; }
    }
    ImGui::SameLine();
    if (!cur) {
        ImGui::TextDisabled("%s", ki.label);
    } else {
        ImGui::SetNextItemWidth(-ImGui::CalcTextSize(ki.label).x - 24.0f);
        if (ki.kind == 1) {
            bool b = cur->v[0] >= 0.5;
            if (ImGui::Checkbox(ki.label, &b)) { cur->v[0] = b ? 1.0 : 0.0; changed = true; }
        } else if (ki.kind == 2) {
            int n = static_cast<int>(std::lround(cur->v[0]));
            if (ImGui::SliderInt(ki.label, &n, static_cast<int>(ki.lo), static_cast<int>(ki.hi))) { cur->v[0] = n; changed = true; }
        } else {
            f32 x[3] = {static_cast<f32>(cur->v[0]), static_cast<f32>(cur->v[1]), static_cast<f32>(cur->v[2])};
            const ImGuiSliderFlags fl = (ki.lo > 0.0f && ki.hi / ki.lo > 100.0f) ? ImGuiSliderFlags_Logarithmic : 0;
            const bool edited = ki.count == 3 ? ImGui::SliderFloat3(ki.label, x, ki.lo, ki.hi, "%.3f", fl)
                                              : ImGui::SliderFloat(ki.label, x, ki.lo, ki.hi, "%.3f", fl);
            if (edited) {
                for (int c = 0; c < ki.count; ++c) cur->v[c] = x[c];
                changed = true;
            }
        }
    }
    ImGui::PopID();
    return changed;
}

} // namespace

bool PostVolumeTools::drawDetails(rhi::IDevice* dev, const std::string& contentDir, std::vector<fmt::OcPostVolume>& vols,
                                  const rhi::PostSettings& base, const Vec3& cam) {
    bool changed = false;
    ImGui::TextUnformatted("Post Process Volumes");
    ImGui::TextDisabled("Override post settings inside a box, or everywhere. Blended by the camera's position.");
    ImGui::Separator();

    if (ImGui::Button("Add Volume At Camera")) { addAtCamera(vols, cam); changed = true; }
    if (selected_ >= 0 && selected_ < static_cast<int>(vols.size())) {
        ImGui::SameLine();
        if (ImGui::Button("Delete")) {
            vols.erase(vols.begin() + selected_);
            selected_ = std::min(selected_, static_cast<int>(vols.size()) - 1);
            changed = true;
        }
    }
    if (vols.empty()) { ImGui::TextDisabled("This level has no post-process volumes."); return changed; }

    for (usize i = 0; i < vols.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        char label[160];
        std::snprintf(label, sizeof label, "%s%s  (priority %d)", vols[i].name.empty() ? "(unnamed)" : vols[i].name.c_str(),
                      vols[i].infinite ? "  [infinite]" : "", vols[i].priority);
        if (ImGui::Selectable(label, static_cast<int>(i) == selected_)) selected_ = static_cast<int>(i);
        ImGui::PopID();
    }
    if (selected_ < 0 || selected_ >= static_cast<int>(vols.size())) return changed;

    fmt::OcPostVolume& v = vols[static_cast<usize>(selected_)];
    ImGui::Separator();
    ImGui::PushID(selected_);

    char nameBuf[96];
    std::snprintf(nameBuf, sizeof nameBuf, "%s", v.name.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##name", nameBuf, sizeof nameBuf)) { v.name = nameBuf; changed = true; }

    if (ImGui::Checkbox("Infinite (whole level)", &v.infinite)) changed = true;
    if (!v.infinite) {
        f32 lo[3], hi[3];
        for (int a = 0; a < 3; ++a) { lo[a] = static_cast<f32>(v.bounds[a]); hi[a] = static_cast<f32>(v.bounds[a + 3]); }
        bool edited = ImGui::DragFloat3("Min (cm)", lo, 10.0f);
        edited |= ImGui::DragFloat3("Max (cm)", hi, 10.0f);
        if (edited) {
            for (int a = 0; a < 3; ++a) { v.bounds[a] = lo[a]; v.bounds[a + 3] = hi[a]; }
            changed = true;
        }
        f32 blend = static_cast<f32>(v.blendCm);
        if (ImGui::DragFloat("Blend Distance (cm)", &blend, 5.0f, 0.0f, 100000.0f)) { v.blendCm = blend; changed = true; }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The weight ramps from the full value at the box's edge to nothing this far outside it.\n"
                              "Zero is a hard edge.");
    }
    int prio = v.priority;
    if (ImGui::DragInt("Priority", &prio)) { v.priority = prio; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Volumes apply in ascending priority, so the highest wins where they overlap.");
    f32 weight = static_cast<f32>(v.weight);
    if (ImGui::SliderFloat("Weight", &weight, 0.0f, 1.0f)) { v.weight = weight; changed = true; }

    ImGui::Separator();
    ImGui::TextUnformatted("Grading LUT (.cube)");
    if (!cubeScanned_) refreshLuts(contentDir);
    if (ImGui::BeginCombo("##lut", v.lut.empty() ? "(none)" : v.lut.c_str())) {
        if (ImGui::Selectable("(none)", v.lut.empty())) { v.lut.clear(); changed = true; }
        for (const std::string& f : cubeFiles_)
            if (ImGui::Selectable(f.c_str(), f == v.lut)) { v.lut = f; changed = true; }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) {
        refreshLuts(contentDir);
        if (!v.lut.empty()) luts_.forget(dev, v.lut);   // re-read the file too: an edited grade shows up
    }
    if (cubeFiles_.empty()) ImGui::TextDisabled("No .cube files under the project's content folder.");

    ImGui::Separator();
    ImGui::TextUnformatted("Overrides");
    ImGui::TextDisabled("Tick a field to override it inside this volume; unticked fields keep the level's value.");
    for (const fmt::OcPostKeyInfo& ki : fmt::kOcPostKeys) {
        if (std::strcmp(ki.key, "lutintensity") == 0 && v.lut.empty()) continue;   // meaningless without a LUT
        changed |= overrideRow(v, ki, base);
    }
    ImGui::PopID();
    return changed;
}

#else

bool PostVolumeTools::drawDetails(rhi::IDevice*, const std::string&, std::vector<fmt::OcPostVolume>&,
                                  const rhi::PostSettings&, const Vec3&) {
    return false;
}

#endif

} // namespace aver::editor
