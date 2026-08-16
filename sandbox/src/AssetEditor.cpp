// Asset editor host: opens per-file editor windows, routes file-change notifications, and the
// .ocmesh viewer.

#include "AssetEditor.hpp"

#include "aver/core/Log.hpp"
#include "aver/formats/OcMesh.hpp"

#include <cctype>
#include <filesystem>

#if AVER_WITH_IMGUI
#  include "imgui.h"
#endif

namespace aver::editor {

// Opens an editor for a path, or focuses the existing one. Returns false if no factory handles it.
bool AssetEditorHost::open(const std::string& path) {
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
    return false;
}

// The already-open editor for `path`, or nullptr.
AssetEditor* AssetEditorHost::find(const std::string& path) {
    for (const auto& ed : editors_) if (ed->path() == path) return ed.get();
    return nullptr;
}

// True if any open editor has unsaved changes.
bool AssetEditorHost::anyDirty() const {
    for (const auto& ed : editors_) if (ed->dirty()) return true;
    return false;
}

// The titles of every editor with unsaved changes.
std::vector<std::string> AssetEditorHost::dirtyTitles() const {
    std::vector<std::string> out;
    for (const auto& ed : editors_) if (ed->dirty()) out.push_back(ed->title());
    return out;
}

// Saves every dirty editor, counting the failures.
usize AssetEditorHost::saveAllDirty(std::string* why) {
    usize failed = 0;
    for (const auto& ed : editors_) {
        if (!ed->dirty()) continue;
        std::string one;
        if (ed->save(&one)) continue;
        ++failed;
        if (why) {
            if (!why->empty()) *why += "\n";
            *why += ed->title() + ": " + (one.empty() ? "save failed" : one);
        }
    }
    return failed;
}

namespace {
// True if two paths name the same file. Falls back to normalised string compare when either side
// does not exist.
bool samePath(const std::string& a, const std::string& b) {
    std::error_code ec;
    if (std::filesystem::equivalent(a, b, ec) && !ec) return true;
    const std::filesystem::path na = std::filesystem::weakly_canonical(std::filesystem::path(a), ec);
    if (ec) return false;
    const std::filesystem::path nb = std::filesystem::weakly_canonical(std::filesystem::path(b), ec);
    if (ec) return false;
#ifdef _WIN32
    std::string sa = na.string(), sb = nb.string();
    for (char& c : sa) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    for (char& c : sb) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return sa == sb;
#else
    return na == nb;
#endif
}
} // namespace

// Tells the editor holding this path that the file changed on disk. False if none does.
bool AssetEditorHost::notifyFileChanged(const std::string& path) {
    for (const auto& ed : editors_) {
        if (samePath(ed->path(), path)) { ed->onFileChanged(); return true; }
    }
    return false;
}

// Tells every editor the file watcher died.
void AssetEditorHost::notifyWatchLost() {
    for (const auto& ed : editors_) ed->onWatchLost();
}

// Draws every open editor window and destroys the ones the user closed. True if any remain.
bool AssetEditorHost::draw(Engine& e, unsigned dockInto, float dpi) {
#if AVER_WITH_IMGUI
    if (editors_.empty()) return false;
    closing_.clear();

    for (usize i = 0; i < editors_.size(); ++i) {
        AssetEditor& ed = *editors_[i];
        bool open = true;

        // Window ID is the path, not the title: a changing ImGui ID loses size, position and docking.
        const std::string label = ed.title() + "###assetEditor:" + ed.path();
        if (ed.path() == focusRequest_) { ImGui::SetNextWindowFocus(); focusRequest_.clear(); }
        ImGui::SetNextWindowSize(ImVec2(760.0f * dpi, 560.0f * dpi), ImGuiCond_FirstUseEver);
        if (dockInto) ImGui::SetNextWindowDockID(static_cast<ImGuiID>(dockInto), ImGuiCond_FirstUseEver);

        if (ImGui::Begin(label.c_str(), &open, ed.dirty() ? ImGuiWindowFlags_UnsavedDocument : 0)) {
            ed.draw(e);
        }
        ImGui::End();

        if (!open) closing_.push_back(i);
    }

    // Erase after the loop: an editor freed mid-iteration is still on this frame's ImGui draw list.
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

// Read-only viewer for one .ocmesh file: counts, bounds and submesh table.
class MeshEditor final : public AssetEditor {
public:
    // Loads the mesh; a failed load is shown as an error page.
    explicit MeshEditor(std::string path) : path_(std::move(path)) {
        if (!fmt::loadOcMesh(path_, mesh_, &error_)) loaded_ = false;
        else                                        loaded_ = true;
    }

    const std::string& path() const override { return path_; }

    // Window title: file name plus a kind tag.
    std::string title() const override {
        return std::filesystem::path(path_).filename().string() + "  [Mesh]";
    }

    // Draws the mesh summary, or the load error.
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

// Editor factory for .ocmesh. Returns null for any other extension.
std::unique_ptr<AssetEditor> makeMeshEditor(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    for (char& c : ext) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (ext != ".ocmesh") return nullptr;
    return std::make_unique<MeshEditor>(path);
}

} // namespace aver::editor
