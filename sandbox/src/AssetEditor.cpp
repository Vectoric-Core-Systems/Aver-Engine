// Asset editor host: opens per-file editor windows, routes file-change notifications, and the
// .ocmesh viewer.

#include "AssetEditor.hpp"

#include "aver/core/Log.hpp"
#include "aver/formats/OcMesh.hpp"
#if AVER_WITH_IMGUI
#include "ActorEditor.hpp"                                  // sharedPreview
#include "AnimEditor.hpp"                                   // releaseAnimEditorGpu
#include "EditorWidgets.hpp"                                 // SplitPane
#include "PreviewChrome.hpp"                                 // drawPreviewToolbar/Stats/Axes
#include "aver/render/preview/ActorPreview.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/runtime/Engine.hpp"
#include <algorithm>
#include <cmath>
#include <vector>
#endif

#include <cctype>
#include <cstdio>
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

// Frees the shared preview, mesh cache and skinning meshes once no open tab draws into them; the
// next tab that needs them recreates them lazily.
void AssetEditorHost::releaseSharedGpuIfIdle() {
#if AVER_WITH_IMGUI
    for (const auto& ed : editors_)
        if (ed->usesSharedPreview()) return;
    releaseActorEditorGpu();
    releaseAnimEditorGpu();
#endif
}

// Draws every open editor window and destroys the ones the user closed. True if any remain.
bool AssetEditorHost::draw(Engine& e, unsigned dockInto, float dpi) {
#if AVER_WITH_IMGUI
    if (releasePending_) { releasePending_ = false; releaseSharedGpuIfIdle(); }
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
        releasePending_ = true;
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
            releasePending_ = true;
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

#if AVER_WITH_IMGUI
// The viewport/details split fraction -- see EditorWidgets.hpp's own SplitPane for why this is a
// persisted FRACTION rather than a raw pixel width. 0.75: the viewport dominates, as in Unreal's
// Static Mesh Editor. ".v2" drops fractions stored before drawSplitHandle stopped ratcheting the
// viewport smaller every time the tab was briefly narrow (EditorWidgets.hpp).
constexpr f32 kDefaultViewFraction = 0.75f;
constexpr const char* kPrefViewSplit = "meshEditor.viewSplit.v2";

// One UE-style property grid: a dim label column and a value column, alternating row shade.
bool beginPropertyGrid(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return false;
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.42f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.58f);
    return true;
}
void propertyRow(const char* name, const char* value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn(); ImGui::TextDisabled("%s", name);
    ImGui::TableNextColumn(); ImGui::TextUnformatted(value);
}
#endif

// Viewer AND (as of ITEM 1.2) editor for one .ocmesh file: counts, bounds, an editable material-slot
// list, a read-only UV-shell wireframe, and the submesh table.
//
// MeshEditor had never written an .ocmesh before this. save() reuses fmt::saveOcMesh on `mesh_` --
// the SAME struct parseOcMesh (via loadOcMesh) populated, edited in place -- rather than rebuilding a
// fresh OcMeshData from scratch, which is what keeps every chunk the parser captured (skin joints/
// weights, multi-LOD Trifactor cluster data, ...) but this editor never surfaces intact through a
// save, instead of silently dropping it the way "rebuild from what the UI shows" would. Proven, not
// assumed: TrifactorTest.cpp's "ITEM 1.2 GATE" combines skin + multi-LOD cluster data + multiple
// material slots in one mesh, round-trips it through real files with zero edits, and asserts the
// result is BYTE-IDENTICAL to the original -- see that test for the one chunk this format does NOT
// actually carry (a second UV set) and why "extra UV sets" could not be included in that fixture.
class MeshEditor final : public AssetEditor {
public:
    // Loads the mesh; a failed load is shown as an error page.
    explicit MeshEditor(std::string path) : path_(std::move(path)) {
        if (!fmt::loadOcMesh(path_, mesh_, &error_)) loaded_ = false;
        else                                        loaded_ = true;
    }

#if AVER_WITH_IMGUI
    // Destroys the preview upload. sharedEditorDevice() is null once the app is shutting down, when
    // the device reclaims it anyway.
    ~MeshEditor() override {
        if (!gpuMesh_) return;
        rhi::IDevice* dev = editor::sharedEditorDevice();
        if (!dev) return;
        if (render::preview::ActorPreview* preview = editor::sharedPreviewIfCreated())
            preview->dropDrawsUsing(gpuMesh_);
        dev->destroyMesh(gpuMesh_);
    }
#endif

    bool usesSharedPreview() const override { return true; }

    const std::string& path() const override { return path_; }

    // Window title: file name plus a kind tag.
    //
    // No manual dirty marker: the host passes ImGuiWindowFlags_UnsavedDocument for every editor whose
    // dirty() is true (AssetEditorHost::draw, above), so adding one here would double it up -- the
    // same convention BtEditor/SoundEditor's own title() comments already document.
    std::string title() const override {
        return std::filesystem::path(path_).filename().string() + "  [Mesh]";
    }

    // True once a material slot has been renamed, added or removed. Geometry/bounds/submesh ranges
    // are still read-only (no UI mutates them), so this can only ever be set from drawMaterialSlots().
    bool dirty() const override { return dirty_; }

#if AVER_WITH_IMGUI
    // Restores the viewport/details split to its default proportion -- see AssetEditor.hpp's own
    // resetLayout() comment for why "Reset Tab Layout" needs every tab to implement this.
    void resetLayout() override { resetSplitPane(split_, kPrefViewSplit, kDefaultViewFraction); }
#endif

    // Writes `mesh_` back via the SAME fmt::saveOcMesh every other .ocmesh writer in this tree uses --
    // see this class's own header comment for why editing `mesh_` in place (rather than reconstructing
    // an OcMeshData from what the UI shows) is what keeps every chunk this editor never surfaces
    // (skin, multi-LOD cluster data, ...) intact through a save.
    bool save(std::string* why) override {
        if (!loaded_) {
            if (why) *why = "cannot save: this file failed to load in the first place";
            return false;
        }
        if (!fmt::saveOcMesh(path_, mesh_, why)) return false;
        dirty_ = false;
        return true;
    }

    // Draws the load error, or the UE-style split: a large preview viewport on the left and a
    // scrollable Details panel on the right.
    void draw(Engine& e) override {
#if AVER_WITH_IMGUI
        if (!loaded_) {
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "This file could not be read.");
            ImGui::Separator();
            ImGui::TextWrapped("%s", error_.c_str());
            return;
        }

        const f32 dpi = ImGui::GetFontSize() / 16.0f;
        const f32 avail = ImGui::GetContentRegionAvail().x;
        const f32 minView = 240.0f * dpi, minDetails = 220.0f * dpi;
        const f32 viewW = splitPaneWidth(split_, kPrefViewSplit, kDefaultViewFraction, avail,
                                          minView, minDetails);

        // Viewport edge to edge, as in Unreal: no padding, border or scrollbar in its pane -- padding
        // is dead space round the render, and a scrollbar appearing would steal width from the image.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        const bool viewOpen = ImGui::BeginChild("##meshView", ImVec2(viewW, 0.0f), ImGuiChildFlags_None,
                                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        if (viewOpen) drawPreview(e);
        ImGui::EndChild();
        drawSplitHandle(split_, "##meshsplit", kPrefViewSplit, avail, minView, minDetails, 6.0f * dpi);

        if (ImGui::BeginChild("##meshDetails", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) {
            ImGui::TextUnformatted("Details");
            ImGui::SameLine();
            ImGui::TextDisabled("  %s", std::filesystem::path(path_).filename().string().c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path_.c_str());
            ImGui::Separator();

            char buf[96];
            if (ImGui::CollapsingHeader("Geometry", ImGuiTreeNodeFlags_DefaultOpen) &&
                beginPropertyGrid("##geometry")) {
                std::snprintf(buf, sizeof buf, "%u", mesh_.vertexCount());
                propertyRow("Vertices", buf);
                std::snprintf(buf, sizeof buf, "%zu", mesh_.indices.size() / 3);
                propertyRow("Triangles", buf);
                std::snprintf(buf, sizeof buf, "%zu (%s)", mesh_.indices.size(),
                              (mesh_.flags & fmt::kOcMeshIndex32) ? "32-bit" : "16-bit");
                propertyRow("Indices", buf);
                ImGui::EndTable();
            }

            if (ImGui::CollapsingHeader("LODs", ImGuiTreeNodeFlags_DefaultOpen)) {
                drawLods();
            }

            if (ImGui::CollapsingHeader("Bounds", ImGuiTreeNodeFlags_DefaultOpen) &&
                beginPropertyGrid("##bounds")) {
                const Vec3 lo = mesh_.boundsMin, hi = mesh_.boundsMax;
                std::snprintf(buf, sizeof buf, "%.1f  %.1f  %.1f", lo.x, lo.y, lo.z);
                propertyRow("Min", buf);
                std::snprintf(buf, sizeof buf, "%.1f  %.1f  %.1f", hi.x, hi.y, hi.z);
                propertyRow("Max", buf);
                std::snprintf(buf, sizeof buf, "%.1f x %.1f x %.1f cm", hi.x - lo.x, hi.y - lo.y, hi.z - lo.z);
                propertyRow("Size", buf);
                ImGui::EndTable();
            }

            if (ImGui::CollapsingHeader("Material Slots", ImGuiTreeNodeFlags_DefaultOpen)) {
                drawMaterialSlots();
            }

            if (ImGui::CollapsingHeader("UV Layout")) {
                drawUvWireframe();
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
        }
        ImGui::EndChild();
#else
        (void)e;   // the headless (no-ImGui) test build draws nothing
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

        const f32 dpi = ImGui::GetFontSize() / 16.0f;

        // ONLY WHEN FOCUSED. ActorPreview is shared by every tab that draws into it, and the draw
        // list is whatever the last writer set this frame -- so an actor editor and a mesh viewer
        // both open would otherwise take turns showing each other's contents. Claiming it only on
        // focus keeps a background tab from stealing the image out of the one being looked at.
        const bool mine = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (mine) {
            preview->setViewMode(viewMode_);
            preview->setShowFlags(show_);

            std::vector<render::preview::PreviewDraw> draws(1);
            draws[0].mesh = gpuMesh_;
            // Shaded with the mesh's own first material, as Unreal's mesh editor shows it.
            if (!mesh_.submeshes.empty() && mesh_.submeshes[0].materialSlot < mesh_.materialSlots.size())
                draws[0].materialHandle = previewMaterialFor(mesh_.materialSlots[mesh_.submeshes[0].materialSlot]);
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

        // FOLLOWS THE VIEWPORT'S OWN SIZE NOW, debounced exactly like AnimEditor's own resize of
        // this same SHARED preview (AnimEditor.cpp) -- resize() costs a waitIdle, so doing it every
        // frame of a drag would stall the editor; a quarter-second of stillness first is what that
        // debounce buys. Gated on `mine` so a background tab never fights the focused one over the
        // shared target's size the way two unrelated frame-by-frame resizes would.
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const f32 availW = std::max(64.0f, avail.x), availH = std::max(64.0f, avail.y);
        if (mine) {
            const u32 iw = static_cast<u32>(availW), ih = static_cast<u32>(availH);
            if (iw != pendingW_ || ih != pendingH_) {
                pendingW_ = iw; pendingH_ = ih;
                resizeDue_ = ImGui::GetTime() + 0.25;
            } else if (resizeDue_ > 0.0 && ImGui::GetTime() >= resizeDue_) {
                resizeDue_ = 0.0;
                preview->resize(pendingW_, pendingH_);
            } else if (resizeDue_ <= 0.0 && (preview->width() != iw || preview->height() != ih)) {
                // SHARED target: another tab (Actor/Anim editor) resized it to its own viewport since.
                // Comparing only against this tab's own last request kept drawing that size here,
                // letterboxed small; take it back now this tab is the focused one.
                resizeDue_ = ImGui::GetTime() + 0.1;
            }
        }

        // FIT, NEVER STRETCH, still: resize() above is debounced and can lag a drag by a quarter
        // second, so the target's aspect and the viewport's can disagree for a few frames even
        // though they are now converging rather than permanently apart.
        const f32 texW = static_cast<f32>(preview->width());
        const f32 texH = static_cast<f32>(preview->height());
        const f32 fit  = std::min(availW / std::max(texW, 1.0f), availH / std::max(texH, 1.0f));
        const ImVec2 imgSize(std::max(texW * fit, 16.0f), std::max(texH * fit, 16.0f));
        // Centred while letterboxed (only until the debounced resize lands), not pinned top-left.
        const ImVec2 origin = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(origin.x + std::max(0.0f, (availW - imgSize.x) * 0.5f),
                                   origin.y + std::max(0.0f, (availH - imgSize.y) * 0.5f)));
        const ImVec2 imageMin = ImGui::GetCursorScreenPos();
        ImGui::Image(static_cast<ImTextureID>(preview->uiTextureId()), imgSize);
        const ImVec2 imageMax(imageMin.x + imgSize.x, imageMin.y + imgSize.y);

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
            // F FRAMES THE MESH -- mirrors AnimEditor.cpp's own inline ImGuiKey_F handling: no
            // repeat, and gated on WantCaptureKeyboard so a text field elsewhere in this tab does
            // not lose an 'f' keystroke to the viewport underneath it.
            if (!io.WantCaptureKeyboard && ImGui::IsKeyPressed(ImGuiKey_F, false)) preview->frameAll();
        }

        if (mine) {
            PreviewShowItem extra[] = {{"UV Layout overlay", &showUvOverlay_}};
            drawPreviewToolbar("##meshToolbar", imageMin, imageMax, dpi, viewMode_, show_, extra, 1);

            std::vector<std::string> lines;
            lines.push_back("LOD: 0");
            lines.push_back("Triangles: " + formatCount(mesh_.indices.size() / 3));
            lines.push_back("Vertices: " + formatCount(mesh_.vertexCount()));
            lines.push_back("UV Channels: " + std::to_string(mesh_.uvs.empty() ? 0 : 1));
            {
                const Vec3 lo = mesh_.boundsMin, hi = mesh_.boundsMax;
                char buf[64];
                std::snprintf(buf, sizeof buf, "Approx Size: %dx%dx%d",
                              static_cast<int>(std::lround(hi.x - lo.x)),
                              static_cast<int>(std::lround(hi.y - lo.y)),
                              static_cast<int>(std::lround(hi.z - lo.z)));
                lines.push_back(buf);
            }
            lines.push_back("Materials: " + std::to_string(mesh_.materialSlots.size()));
            drawPreviewStats(imageMin, dpi, lines);
            drawPreviewAxes(imageMin, imageMax, dpi, preview->camera());
            if (showUvOverlay_) drawUvOverlay(imageMin, imageMax, dpi);
        } else {
            // Over the image, not below it: a line under the image would overflow the pane.
            const char* msg = "Click this tab to take the preview";
            const ImVec2 ts = ImGui::CalcTextSize(msg);
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(imageMin.x + (imgSize.x - ts.x) * 0.5f, imageMin.y + (imgSize.y - ts.y) * 0.5f),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), msg);
        }
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

    // ITEM 1.2 (i): the material-slot list, editable. OcMeshSubmesh::materialSlot is a POSITIONAL
    // index into OcMeshData::materialSlots (§5.6), not a token, so removing a slot has to fix up every
    // submesh that pointed at it or past it -- leaving a dangling index would read the wrong name (or
    // past the end) the moment the mesh reloads.
    void drawMaterialSlots() {
        int removeIdx = -1;
        for (usize i = 0; i < mesh_.materialSlots.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            char buf[128];
            std::snprintf(buf, sizeof buf, "%s", mesh_.materialSlots[i].c_str());
            ImGui::SetNextItemWidth(-96.0f);
            if (ImGui::InputText("##slotname", buf, sizeof buf)) {
                mesh_.materialSlots[i] = buf;
                dirty_ = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("slot %zu", i);
            ImGui::SameLine();
            // A mesh needs at least one slot for its submeshes to resolve against -- see
            // OcMeshSubmesh::materialSlot's own comment above -- so the last one cannot be removed
            // from here; renaming or adding are still available.
            ImGui::BeginDisabled(mesh_.materialSlots.size() <= 1);
            if (ImGui::SmallButton("Remove")) removeIdx = static_cast<int>(i);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        if (removeIdx >= 0) {
            mesh_.materialSlots.erase(mesh_.materialSlots.begin() + removeIdx);
            for (fmt::OcMeshSubmesh& s : mesh_.submeshes) {
                if (s.materialSlot == static_cast<u32>(removeIdx)) s.materialSlot = 0;
                else if (s.materialSlot > static_cast<u32>(removeIdx)) --s.materialSlot;
            }
            dirty_ = true;
        }
        if (ImGui::SmallButton("Add Slot")) {
            mesh_.materialSlots.push_back("M_Slot" + std::to_string(mesh_.materialSlots.size()));
            dirty_ = true;
        }
    }

    // The new LODs section: how many levels this mesh carries, and each one's own triangle count --
    // LOD 0 from indices/meshlets as always, coarserLods[i] (i.e. LOD i+1) from its own index buffer.
    // See OcMeshData::coarserLods' own comment for why a coarser level owns triangles but not
    // vertices. Read-only: nothing here generates or strips a LOD, only reports what the file has.
    void drawLods() {
        ImGui::Text("LOD Count  %u", mesh_.lodCount());
        if (!ImGui::BeginTable("lods", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) return;
        ImGui::TableSetupColumn("LOD");
        ImGui::TableSetupColumn("Triangles");
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("0 (base)");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(formatCount(mesh_.indices.size() / 3).c_str());
        for (usize i = 0; i < mesh_.coarserLods.size(); ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%zu", i + 1);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(formatCount(mesh_.coarserLods[i].indices.size() / 3).c_str());
        }
        ImGui::EndTable();
    }

    // ITEM 1.2 (ii): a READ-ONLY wireframe of every LOD-0 triangle's UV shell. mesh_.uvs is already
    // flat, per-vertex data (§5.2 UV0) -- no format change needed, only a new view onto data the
    // parser already captures. Judged by eye only: there is no oracle for "does this UV layout look
    // right", so this is never compared against a rendered image or a recorded baseline anywhere.
    //
    // SPLIT INTO A CORE (drawUvWireframeCore) PLUS TWO CALL SITES: the Details panel's own canvas
    // below, and drawUvOverlay's on-demand copy over a corner of the 3D viewport -- same triangles,
    // a different rect, and the overlay draws with no InvisibleButton of its own so it never steals
    // the image's hover (orbit/zoom/pan/F all still read the image underneath it).
    void drawUvWireframeCore(ImDrawList* dl, ImVec2 p0, ImVec2 p1) const {
        dl->AddRectFilled(p0, p1, IM_COL32(24, 24, 28, 235));
        dl->AddRect(p0, p1, IM_COL32(90, 90, 100, 255));
        const f32 sx = p1.x - p0.x, sy = p1.y - p0.y;
        // Quarter gridlines at 0/.25/.5/.75/1 on both axes, purely for orientation.
        for (int i = 1; i < 4; ++i) {
            const f32 t = static_cast<f32>(i) / 4.0f;
            const f32 x = p0.x + t * sx, y = p0.y + t * sy;
            dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(60, 60, 68, 255));
            dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), IM_COL32(60, 60, 68, 255));
        }

        // UV -> rect, V flipped to the usual texture-space convention (V=0 at the top). Clipped: a
        // tiled UV set legitimately runs outside [0,1], and without a clip its edges would be drawn
        // over whatever sits next to (or, for the overlay, underneath) this rect.
        const u32 vcount = mesh_.vertexCount();
        const auto toPt = [p0, sx, sy, this](u32 vi) {
            const f32 u = mesh_.uvs[usize(vi) * 2 + 0], v = mesh_.uvs[usize(vi) * 2 + 1];
            return ImVec2(p0.x + u * sx, p0.y + (1.0f - v) * sy);
        };

        // A dense LOD-0 could be hundreds of thousands of edges; this is a debug view, not a
        // renderer, so triangles beyond the cap are simply not drawn rather than stalling the frame.
        const usize triCount = mesh_.indices.size() / 3;
        const usize shown = std::min(triCount, kMaxUvTris);
        dl->PushClipRect(p0, p1, true);
        for (usize t = 0; t < shown; ++t) {
            const u32 ia = mesh_.indices[t * 3 + 0], ib = mesh_.indices[t * 3 + 1], ic = mesh_.indices[t * 3 + 2];
            if (ia >= vcount || ib >= vcount || ic >= vcount) continue;
            const ImVec2 a = toPt(ia), b = toPt(ib), c = toPt(ic);
            const ImU32 col = IM_COL32(120, 190, 255, 160);
            dl->AddLine(a, b, col);
            dl->AddLine(b, c, col);
            dl->AddLine(c, a, col);
        }
        dl->PopClipRect();
    }

    // The Details panel's own UV canvas: a square sized to the column, laid out as an ordinary item.
    void drawUvWireframe() {
        if (mesh_.uvs.size() < 2 || mesh_.indices.size() < 3) {
            ImGui::TextDisabled("No UVs to show.");
            return;
        }
        const f32 avail = std::max(64.0f, ImGui::GetContentRegionAvail().x);
        const f32 side = std::min(avail, 420.0f);
        ImGui::InvisibleButton("##uvcanvas", ImVec2(side, side));
        drawUvWireframeCore(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        const usize triCount = mesh_.indices.size() / 3;
        if (triCount > kMaxUvTris)
            ImGui::TextDisabled("Showing %zu of %zu triangles (capped).", kMaxUvTris, triCount);
    }

    // The viewport's on-demand "UV Layout overlay" toggle (toolbar Show dropdown): the same wireframe,
    // in the image's top-right corner, drawn straight onto the window's draw list with no layout item
    // of its own -- exactly how drawPreviewStats/drawPreviewAxes sit over the image already.
    void drawUvOverlay(ImVec2 imageMin, ImVec2 imageMax, f32 dpi) const {
        if (mesh_.uvs.size() < 2 || mesh_.indices.size() < 3) return;
        const f32 inset = 8.0f * dpi;
        const f32 side = std::min(180.0f * dpi,
                                   std::min(imageMax.x - imageMin.x, imageMax.y - imageMin.y) * 0.4f);
        if (side < 48.0f * dpi) return;
        const ImVec2 p0(imageMax.x - inset - side, imageMin.y + inset);
        const ImVec2 p1(imageMax.x - inset, imageMin.y + inset + side);
        drawUvWireframeCore(ImGui::GetWindowDrawList(), p0, p1);
    }

    static constexpr usize kMaxUvTris = 20000;

    rhi::MeshHandle gpuMesh_ = 0;
    bool uploadTried_ = false;
    bool framed_ = false;

    SplitPane split_;
    render::preview::PreviewViewMode viewMode_ = render::preview::PreviewViewMode::Lit;
    render::preview::PreviewShowFlags show_{};
    bool showUvOverlay_ = false;
    u32 pendingW_ = 0, pendingH_ = 0;
    double resizeDue_ = 0.0;   // ImGui::GetTime() deadline for the next preview->resize(); 0 = none due
#endif

    std::string path_;
    fmt::OcMeshData mesh_;
    std::string error_;
    bool loaded_ = false;
    bool dirty_ = false;   // set only by drawMaterialSlots(); see dirty()'s own comment
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
