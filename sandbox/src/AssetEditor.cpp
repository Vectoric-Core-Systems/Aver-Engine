// Asset editor host: opens per-file editor windows, routes file-change notifications, and the
// .ocmesh viewer.

#include "AssetEditor.hpp"

#include "aver/core/Log.hpp"
#include "aver/formats/OcMesh.hpp"
#if AVER_WITH_IMGUI
#include "ActorEditor.hpp"                                  // sharedPreview
#include "aver/render/preview/ActorPreview.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/runtime/Engine.hpp"
#include <algorithm>
#include <vector>
#endif

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

// Routes "Reset Tab Layout" to whichever editor is actually FOCUSED.
//
// THE BUG THIS REPLACES. SandboxApp.cpp used to gate the menu item on "some asset tab is open" (one
// bool, no per-editor-type branch) and then unconditionally call the Actor editor's own layout reset
// -- so resetting a Sound or Graph tab's layout reset the Actor editor's instead, silently, because
// nothing checked WHICH tab was in front. find(focusedPath_) is that check.
void AssetEditorHost::resetFocusedLayout() {
    if (AssetEditor* ed = find(focusedPath_)) ed->resetLayout();
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
        // Checked whether or not Begin returned true (a collapsed tab can still be focused), and
        // before End() -- IsWindowFocused() answers for whichever window Begin/End currently bracket.
        // This is what resetFocusedLayout() below routes "Reset Tab Layout" through.
        if (ImGui::IsWindowFocused()) focusedPath_ = ed.path();
        ImGui::End();

        if (!open) closing_.push_back(i);
    }

    // Erase after the loop: an editor freed mid-iteration is still on this frame's ImGui draw list.
    //
    // A DIRTY TAB IS NOT CLOSED, IT IS ASKED ABOUT. Closing one used to destroy the editor and log
    // a warning into a drawer the user is probably not looking at -- an hour of graph or sound
    // editing gone to a click on the wrong X, with the only record in the Output Log. The whole
    // application already refuses to exit with unsaved editors and raises a proper prompt naming
    // them; this is the same question for one tab.
    for (usize k = closing_.size(); k-- > 0;) {
        AssetEditor& ed = *editors_[closing_[k]];
        if (ed.dirty()) {
            closeAskPath_ = ed.path();     // held open until the prompt is answered
            closeAskError_.clear();
            continue;
        }
        editors_.erase(editors_.begin() + static_cast<isize>(closing_[k]));
    }

    drawClosePrompt(dpi);
    return !editors_.empty();
#else
    (void)e; (void)dockInto; (void)dpi;
    return false;
#endif
}

// Asks before throwing away one tab's unsaved edits. Modelled on SandboxApp's own exit prompt: same
// centred auto-resizing modal, same Save / Discard / Cancel order, same rule that a FAILED save
// keeps the dialog up and says why rather than closing and losing the work anyway.
void AssetEditorHost::drawClosePrompt(float dpi) {
#if AVER_WITH_IMGUI
    if (closeAskPath_.empty()) return;

    AssetEditor* ed = find(closeAskPath_);
    if (!ed) { closeAskPath_.clear(); return; }   // saved or closed some other way meanwhile

    constexpr const char* kTitle = "Unsaved changes";
    if (!ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460.0f * dpi, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::TextWrapped("'%s' has unsaved changes.", ed->title().c_str());
    ImGui::TextDisabled("%s", closeAskPath_.c_str());
    if (!closeAskError_.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.93f, 0.42f, 0.38f, 1.0f), "%s", closeAskError_.c_str());
    }
    ImGui::Spacing();
    ImGui::Separator();

    const auto drop = [this]() {
        for (usize i = 0; i < editors_.size(); ++i) {
            if (editors_[i]->path() != closeAskPath_) continue;
            editors_.erase(editors_.begin() + static_cast<isize>(i));
            break;
        }
        closeAskPath_.clear();
        closeAskError_.clear();
        ImGui::CloseCurrentPopup();
    };

    if (ImGui::Button("Save and close", ImVec2(150.0f * dpi, 0.0f))) {
        std::string why;
        if (ed->save(&why)) drop();
        else closeAskError_ = why.empty() ? std::string("Could not save.") : why;
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard", ImVec2(110.0f * dpi, 0.0f))) {
        AVER_WARN("[Editor] discarded unsaved changes in '{}'",
                  std::filesystem::path(closeAskPath_).filename().string());
        drop();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110.0f * dpi, 0.0f))) {
        closeAskPath_.clear();       // the tab simply stays open
        closeAskError_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
#else
    (void)dpi;
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
    void draw(Engine& e) override {
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
        drawPreview(e);
#endif
    }

private:
#if AVER_WITH_IMGUI
    // A 3D preview of the mesh.
    //
    // THE COMMENT THAT USED TO BE HERE SAID THIS "needs an offscreen render target and a preview
    // camera, neither of which exists yet". Both existed: ActorPreview provides the target, the
    // orbit camera, frameAll() and uiTextureId(), and ThumbnailCache already renders .ocmesh files
    // through it for the Content Browser's tiles. The mesh viewer was the only place that still
    // said it could not be done -- the third stale "cannot" comment found in this tree.
    //
    // UPLOADED FROM THE ALREADY-PARSED MESH, not through PreviewMeshCache::resolve. resolve joins
    // its path against the project content root, and this tab opens whatever file it was given,
    // which may be outside any project entirely. The bytes are already in mesh_; the only thing
    // missing was a GPU copy of them.
    void drawPreview(Engine& e) {
        rhi::IDevice* dev = e.device();
        render::preview::ActorPreview* preview = editor::sharedPreview(e);
        if (!dev || !preview || !preview->uiTextureId()) {
            ImGui::TextDisabled("No preview on this backend.");
            return;
        }
        if (!uploadTried_) {
            uploadTried_ = true;
            gpuMesh_ = uploadMesh(*dev);
            if (!gpuMesh_) AVER_WARN("[Editor] mesh preview: could not upload '{}'", path_);
        }
        if (!gpuMesh_) { ImGui::TextDisabled("This mesh could not be uploaded for preview."); return; }

        // ONLY WHEN FOCUSED. ActorPreview is shared by every tab that draws into it, and the draw
        // list is whatever the last writer set this frame -- so an actor editor and a mesh viewer
        // both open would otherwise take turns showing each other's contents. Claiming it only on
        // focus keeps a background tab from stealing the image out of the one being looked at.
        const bool mine = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (mine) {
            std::vector<render::preview::PreviewDraw> draws(1);
            draws[0].mesh = gpuMesh_;
            // Recentre on the mesh's own middle and give frameAll a real extent: at boundsRadius 0
            // it computes a zero span and falls back to a fixed distance, framing every mesh
            // identically regardless of size. Same reasoning ThumbnailCache writes out at length.
            f32 centre[3] = {};
            f32 radius = 0.0f;
            if (dev->meshBounds(gpuMesh_, centre, &radius)) {
                draws[0].world[12] = -centre[0];
                draws[0].world[13] = -centre[1];
                draws[0].world[14] = -centre[2];
                draws[0].boundsRadius = radius;
            }
            preview->setDrawList(std::move(draws));
            if (!framed_) { preview->frameAll(); framed_ = true; }
        }

        // FIT, NEVER STRETCH. This drew the preview at ImVec2(avail.x, h) -- the panel's shape, not
        // the target's -- so the mesh was squashed or elongated by however far the two aspects
        // disagreed. Docking the tab tall or wide visibly deformed the model, which on the one view
        // whose whole job is judging a mesh's proportions is the worst place for it.
        //
        // LETTERBOXED RATHER THAN RESIZING THE TARGET, which is the other way to fix this and is
        // what ActorEditor and AnimEditor do. Not here: this preview is SHARED (sharedPreview), so a
        // resize from this tab is a resize for every other one, and two docked tabs of different
        // shapes would take turns resizing it every frame. Fitting costs nothing and cannot fight.
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const f32 h = std::max(160.0f, avail.y);
        const f32 texW = static_cast<f32>(preview->width());
        const f32 texH = static_cast<f32>(preview->height());
        const f32 fit  = std::min(avail.x / std::max(texW, 1.0f), h / std::max(texH, 1.0f));
        ImGui::Image(static_cast<ImTextureID>(preview->uiTextureId()),
                     ImVec2(std::max(texW * fit, 16.0f), std::max(texH * fit, 16.0f)));
        if (ImGui::IsItemHovered()) {
            const ImGuiIO& io = ImGui::GetIO();
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                preview->camera().addOrbit(io.MouseDelta.x * 0.4f, io.MouseDelta.y * 0.4f);
            if (io.MouseWheel != 0.0f)
                preview->camera().addZoom(io.MouseWheel > 0.0f ? 0.9f : 1.1f);
            // PAN, which orbit and zoom alone cannot substitute for: off-centre detail on a large
            // mesh is unreachable when the camera can only swing about a fixed point. Middle and
            // right both, matching the level viewport's own MMB pan so the gesture transfers.
            for (const ImGuiMouseButton b : {ImGuiMouseButton_Middle, ImGuiMouseButton_Right}) {
                if (!ImGui::IsMouseDragging(b)) continue;
                const ImVec2 d = ImGui::GetMouseDragDelta(b);
                ImGui::ResetMouseDragDelta(b);
                preview->camera().panPixels(d.x, d.y, static_cast<f32>(preview->height()));
            }
        }
        if (!mine) ImGui::TextDisabled("Click this tab to take the preview.");
    }

    // The file's stream layout into the engine's interleaved vertex, then one createMesh.
    rhi::MeshHandle uploadMesh(rhi::IDevice& dev) const {
        const u32 n = mesh_.vertexCount();
        if (n == 0 || mesh_.indices.empty()) return 0;
        std::vector<rhi::MeshVertex> verts(n);
        for (u32 i = 0; i < n; ++i) {
            rhi::MeshVertex& v = verts[i];
            v.px = mesh_.positions[usize(i)*3+0];
            v.py = mesh_.positions[usize(i)*3+1];
            v.pz = mesh_.positions[usize(i)*3+2];
            if (mesh_.normals.size() >= usize(n)*3) {
                v.nx = mesh_.normals[usize(i)*3+0];
                v.ny = mesh_.normals[usize(i)*3+1];
                v.nz = mesh_.normals[usize(i)*3+2];
            } else { v.nx = 0.0f; v.ny = 0.0f; v.nz = 1.0f; }
            if (mesh_.uvs.size() >= usize(n)*2) {
                v.u = mesh_.uvs[usize(i)*2+0];
                v.v = mesh_.uvs[usize(i)*2+1];
            } else { v.u = 0.0f; v.v = 0.0f; }
        }
        return dev.createMesh(verts.data(), n, mesh_.indices.data(),
                              static_cast<u32>(mesh_.indices.size()));
    }

    rhi::MeshHandle gpuMesh_ = 0;
    bool uploadTried_ = false;
    bool framed_ = false;
#endif

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
