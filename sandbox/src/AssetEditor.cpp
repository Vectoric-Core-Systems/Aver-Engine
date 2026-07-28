#include "AssetEditor.hpp"

#include "aver/core/Log.hpp"
#include "aver/formats/OcMesh.hpp"

#include <filesystem>

#if AVER_WITH_IMGUI
#  include "imgui.h"
#endif

namespace aver::editor {

bool AssetEditorHost::open(const std::string& path) {
    // Already open: focus rather than open a second copy. Two editors on one file would each hold
    // their own dirty state and the last save would silently win.
    for (const auto& ed : editors_) {
        if (ed->path() == path) { focusRequest_ = path; return true; }
    }
    for (const AssetEditorFactory f : factories_) {
        if (std::unique_ptr<AssetEditor> ed = f(path)) {
            AVER_INFO("[Editor] opened '{}'", std::filesystem::path(path).filename().string());
            focusRequest_ = path;
            editors_.push_back(std::move(ed));
            return true;
        }
    }
    return false;   // the caller falls back to the shell
}

bool AssetEditorHost::anyDirty() const {
    for (const auto& ed : editors_) if (ed->dirty()) return true;
    return false;
}

bool AssetEditorHost::draw(Engine& e, unsigned dockInto, float dpi) {
#if AVER_WITH_IMGUI
    if (editors_.empty()) return false;
    closing_.clear();

    for (usize i = 0; i < editors_.size(); ++i) {
        AssetEditor& ed = *editors_[i];
        bool open = true;

        // The path is the window ID, not the title: a title carries the dirty marker and changes as
        // soon as something is edited, and an ImGui window whose ID changes loses its size, its
        // position and its docking.
        const std::string label = ed.title() + "###assetEditor:" + ed.path();
        if (ed.path() == focusRequest_) { ImGui::SetNextWindowFocus(); focusRequest_.clear(); }
        ImGui::SetNextWindowSize(ImVec2(760.0f * dpi, 560.0f * dpi), ImGuiCond_FirstUseEver);
        // Into the editor's central region on first appearance, so an asset editor arrives where the
        // work is rather than as a small window over the menu bar. ImGui then supplies the tab bar,
        // the drag-to-undock and the drag-back-to-dock -- all of which are its docking behaviour
        // rather than anything this host implements.
        if (dockInto) ImGui::SetNextWindowDockID(static_cast<ImGuiID>(dockInto), ImGuiCond_FirstUseEver);

        if (ImGui::Begin(label.c_str(), &open, ed.dirty() ? ImGuiWindowFlags_UnsavedDocument : 0)) {
            ed.draw(e);
        }
        ImGui::End();

        if (!open) closing_.push_back(i);
    }

    // Destroyed AFTER the loop. Erasing mid-iteration would free an editor whose ImGui window is
    // still on the current frame's draw list, and the next widget would write through a dead this.
    for (usize k = closing_.size(); k-- > 0;) {
        AssetEditor& ed = *editors_[closing_[k]];
        if (ed.dirty())
            AVER_WARN("[Editor] closed '{}' with unsaved changes", std::filesystem::path(ed.path()).filename().string());
        editors_.erase(editors_.begin() + static_cast<isize>(closing_[k]));
    }
    return !editors_.empty();
#else
    (void)e; (void)dockInto; (void)dpi;
    return false;
#endif
}

// ---------------------------------------------------------------- the mesh editor

namespace {

class MeshEditor final : public AssetEditor {
public:
    explicit MeshEditor(std::string path) : path_(std::move(path)) {
        if (!fmt::loadOcMesh(path_, mesh_, &error_)) loaded_ = false;
        else                                        loaded_ = true;
    }

    const std::string& path() const override { return path_; }
    std::string title() const override {
        return std::filesystem::path(path_).filename().string() + "  [Mesh]";
    }

    void draw(Engine&) override {
#if AVER_WITH_IMGUI
        if (!loaded_) {
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "This file could not be read.");
            ImGui::Separator();
            ImGui::TextWrapped("%s", error_.c_str());
            return;
        }

        ImGui::TextDisabled("%s", path_.c_str());
        ImGui::Separator();

        if (ImGui::CollapsingHeader("Geometry", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Vertices   %u", mesh_.vertexCount());
            ImGui::Text("Triangles  %zu", mesh_.indices.size() / 3);
            ImGui::Text("Indices    %zu (%s)", mesh_.indices.size(),
                        (mesh_.flags & fmt::kOcMeshIndex32) ? "32-bit" : "16-bit");
        }

        if (ImGui::CollapsingHeader("Bounds", ImGuiTreeNodeFlags_DefaultOpen)) {
            const Vec3 lo = mesh_.boundsMin, hi = mesh_.boundsMax;
            ImGui::Text("Min   %8.1f  %8.1f  %8.1f", lo.x, lo.y, lo.z);
            ImGui::Text("Max   %8.1f  %8.1f  %8.1f", hi.x, hi.y, hi.z);
            ImGui::Text("Size  %8.1f  %8.1f  %8.1f  cm", hi.x - lo.x, hi.y - lo.y, hi.z - lo.z);
        }

        if (ImGui::CollapsingHeader("Submeshes", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::BeginTable("submeshes", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Material slot");
                ImGui::TableSetupColumn("First index");
                ImGui::TableSetupColumn("Indices");
                ImGui::TableHeadersRow();
                for (const fmt::OcMeshSubmesh& s : mesh_.submeshes) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(s.name.c_str());
                    ImGui::TableNextColumn();
                    if (s.materialSlot < mesh_.materialSlots.size())
                        ImGui::Text("%u  (%s)", s.materialSlot, mesh_.materialSlots[s.materialSlot].c_str());
                    else
                        ImGui::Text("%u", s.materialSlot);
                    ImGui::TableNextColumn(); ImGui::Text("%u", s.indexStart);
                    ImGui::TableNextColumn(); ImGui::Text("%u", s.indexCount);
                }
                ImGui::EndTable();
            }
        }

        ImGui::Separator();
        // Said plainly rather than left to be discovered. A viewport here needs a render target and
        // a preview camera, and claiming one is coming is the kind of thing this tree has too much of.
        ImGui::TextDisabled("Read-only. A 3D preview needs an offscreen render target and a preview");
        ImGui::TextDisabled("camera, neither of which exists yet.");
#endif
    }

private:
    std::string path_;
    fmt::OcMeshData mesh_;
    std::string error_;
    bool loaded_ = false;
};

} // namespace

std::unique_ptr<AssetEditor> makeMeshEditor(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    for (char& c : ext) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (ext != ".ocmesh") return nullptr;
    return std::make_unique<MeshEditor>(path);
}

} // namespace aver::editor
