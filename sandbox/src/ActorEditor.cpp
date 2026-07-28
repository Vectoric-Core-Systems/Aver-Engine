#include "ActorEditor.hpp"

#include "aver/core/Log.hpp"
#include "aver/formats/ActorScript.hpp"
#include "aver/render/preview/ActorPreview.hpp"
#include "aver/render/preview/PreviewMeshCache.hpp"
#include "aver/runtime/Engine.hpp"

#if AVER_WITH_IMGUI
#  include <imgui.h>
#endif

#include <cstdio>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace aver::editor {
namespace {

// ONE preview, shared by every open actor tab, and only the ACTIVE tab drives it.
//
// Not one per tab, and the reason is a hard limit rather than thrift: the UI descriptor heap holds
// sixteen slots and the editor already spends five, so a target per tab exhausts it at about eleven
// and the failure is a black image rather than an assert. A second tab shows its model list and its
// numbers; it just does not get the 3D until it is focused.
render::preview::ActorPreview* g_preview = nullptr;
render::preview::PreviewMeshCache g_meshes;
std::string g_contentRoot;
bool g_previewTried = false;
// Held so shutdown can unregister before the feature goes. A device that still holds a pointer to
// a deleted feature calls prePass on freed memory, which is not an error anything reports.
rhi::IDevice* g_device = nullptr;

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Degrees (yaw, pitch, roll) about +Z, +Y, +X and a scale, into the engine's row-vector matrix with
// the translation in the LAST ROW. Written out rather than borrowed from the scene, because this
// module must not depend on the world to draw something that is not in it.
void composeTransform(const f32 pos[3], const f32 rotDeg[3], const f32 scale[3], f32 out[16]) {
    constexpr f32 kPi = 3.14159265358979f;
    const f32 y = rotDeg[0] * kPi / 180.0f, p = rotDeg[1] * kPi / 180.0f, r = rotDeg[2] * kPi / 180.0f;
    const f32 cy = std::cos(y), sy = std::sin(y);
    const f32 cp = std::cos(p), sp = std::sin(p);
    const f32 cr = std::cos(r), sr = std::sin(r);

    // Z (yaw) then Y (pitch) then X (roll), which is the order the framework applies them.
    const f32 m00 = cy * cp,  m01 = sy * cp,  m02 = -sp;
    const f32 m10 = cy * sp * sr - sy * cr, m11 = sy * sp * sr + cy * cr, m12 = cp * sr;
    const f32 m20 = cy * sp * cr + sy * sr, m21 = sy * sp * cr - cy * sr, m22 = cp * cr;

    out[0]  = m00 * scale[0]; out[1]  = m01 * scale[0]; out[2]  = m02 * scale[0]; out[3]  = 0.0f;
    out[4]  = m10 * scale[1]; out[5]  = m11 * scale[1]; out[6]  = m12 * scale[1]; out[7]  = 0.0f;
    out[8]  = m20 * scale[2]; out[9]  = m21 * scale[2]; out[10] = m22 * scale[2]; out[11] = 0.0f;
    out[12] = pos[0];         out[13] = pos[1];         out[14] = pos[2];         out[15] = 1.0f;
}

class ActorEditor final : public AssetEditor {
public:
    explicit ActorEditor(std::string path, fmt::ActorScript parsed, std::string source)
        : path_(std::move(path)), script_(std::move(parsed)), source_(std::move(source)) {
        title_ = std::filesystem::path(path_).filename().string();
    }

    const std::string& path() const override { return path_; }
    std::string title() const override { return dirty_ ? title_ + " *" : title_; }
    bool dirty() const override { return dirty_; }

    void draw(Engine& e) override;

    bool save(std::string* why) override {
        if (!dirty_) return true;
        std::string out;
        if (!fmt::rewriteActorScript(source_, script_.models, out, why)) return false;
        std::ofstream os(path_, std::ios::binary | std::ios::trunc);
        if (!os) { if (why) *why = "could not open " + path_ + " for writing"; return false; }
        os.write(out.data(), static_cast<std::streamsize>(out.size()));
        if (!os) { if (why) *why = "the write failed part way through"; return false; }
        // The in-memory source becomes what is now on disk, so a second save rewrites from the file
        // as it stands rather than from the text this tab was opened with.
        source_ = std::move(out);
        dirty_ = false;
        AVER_INFO("[ActorEditor] wrote {} placement(s) back to {}", script_.models.size(), path_);
        return true;
    }

private:
    void buildDrawList(Engine& e);

    std::string path_, title_, source_;
    fmt::ActorScript script_;
    bool dirty_ = false;
    int selected_ = -1;
    bool framed_ = false;
    std::string status_;
};

void ActorEditor::buildDrawList(Engine& e) {
    if (!g_preview || !e.device()) return;
    g_meshes.setContentRoot(*e.device(), g_contentRoot);

    std::vector<render::preview::PreviewDraw> draws;
    draws.reserve(script_.models.size());
    for (int i = 0; i < static_cast<int>(script_.models.size()); ++i) {
        const fmt::ActorModel& m = script_.models[static_cast<usize>(i)];
        render::preview::PreviewDraw d;
        d.mesh = g_meshes.resolve(*e.device(), m.meshPath);
        composeTransform(m.pos, m.rot, m.scale, d.world);
        d.selected = (i == selected_);
        draws.push_back(d);
    }
    g_preview->setDrawList(std::move(draws));
    if (!framed_) { g_preview->frameAll(); framed_ = true; }
}

void ActorEditor::draw(Engine& e) {
#if AVER_WITH_IMGUI
    // The preview is created on FIRST DRAW rather than at registration, because the factory is a
    // bare function pointer with no device to hand it and the device is what this needs.
    if (!g_previewTried && e.device()) {
        g_previewTried = true;
        g_preview = render::preview::ActorPreview::create(*e.device(), 1024);
        if (g_preview) {
            // Registered so its prePass runs. NON-OWNING on the device's side, which is why
            // shutdownActorEditors removes it before deleting -- see there.
            g_device = e.device();
            g_device->addRenderFeature(g_preview);
        } else {
            AVER_WARN("[ActorEditor] no preview on this backend; the tab shows numbers only");
        }
    }

    buildDrawList(e);

    // ---- the view ----
    const f32 avail = ImGui::GetContentRegionAvail().x;
    const f32 side = 320.0f;
    const f32 viewW = avail > side * 2.0f ? avail - side : avail;

    if (g_preview && g_preview->uiTextureId()) {
        const f32 s = viewW < 64.0f ? 64.0f : viewW;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::Image(static_cast<ImTextureID>(g_preview->uiTextureId()), ImVec2(s, s));

        // Orbit and zoom, on the image itself. Handled here rather than by the preview because input
        // is the editor's business and the feature must stay drivable with no ImGui at all.
        if (ImGui::IsItemHovered()) {
            const ImGuiIO& io = ImGui::GetIO();
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                g_preview->camera().addOrbit(-d.x * 0.4f, d.y * 0.4f);
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
            }
            if (io.MouseWheel != 0.0f)
                g_preview->camera().addZoom(io.MouseWheel > 0.0f ? 0.88f : 1.0f / 0.88f);
        }
        (void)at;
    } else {
        ImGui::TextDisabled("No 3D preview on this backend.");
    }

    // Meshes the source names and the project does not have. Said out loud, because an actor whose
    // meshes are all missing renders an EMPTY view, and an empty view with no explanation is
    // indistinguishable from a broken preview.
    if (!g_meshes.missing().empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.25f, 1.0f), "%zu mesh(es) not found:",
                           g_meshes.missing().size());
        for (const std::string& m : g_meshes.missing()) ImGui::BulletText("%s", m.c_str());
    }

    ImGui::SameLine();
    ImGui::BeginGroup();

    // ---- the models ----
    ImGui::Text("%zu placement(s)", script_.models.size());
    ImGui::TextDisabled("Preview lighting is fixed and does not match the level viewport.");
    ImGui::Separator();

    for (int i = 0; i < static_cast<int>(script_.models.size()); ++i) {
        const fmt::ActorModel& m = script_.models[static_cast<usize>(i)];
        char label[192];
        std::snprintf(label, sizeof label, "%s##model%d", m.property.c_str(), i);
        if (ImGui::Selectable(label, selected_ == i)) selected_ = i;
    }

    ImGui::Separator();
    if (selected_ >= 0 && selected_ < static_cast<int>(script_.models.size())) {
        fmt::ActorModel& m = script_.models[static_cast<usize>(selected_)];
        ImGui::TextDisabled("%s", m.meshPath.c_str());
        ImGui::TextDisabled("material: %s", m.material.c_str());
        // The match key, shown because it is what a rewrite finds the line by -- when a save goes to
        // the wrong row this is the first thing anyone will want to see.
        ImGui::TextDisabled("id: 0x%016llX", static_cast<unsigned long long>(m.objectId));

        bool changed = false;
        changed |= ImGui::DragFloat3("Position (cm)", m.pos, 1.0f);
        changed |= ImGui::DragFloat3("Rotation (deg)", m.rot, 0.5f);
        changed |= ImGui::DragFloat3("Scale", m.scale, 0.01f, 0.001f, 1000.0f);
        // The preview is rebuilt from these every frame, so it follows the drag with no extra
        // plumbing -- which is the whole point of drawing the source rather than a spawned actor.
        if (changed) dirty_ = true;
    } else {
        ImGui::TextDisabled("Select a placement to edit it.");
    }

    ImGui::Separator();
    ImGui::BeginDisabled(!dirty_);
    if (ImGui::Button("Save to C#")) {
        std::string why;
        status_ = save(&why) ? "Saved." : ("Save failed: " + why);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Frame All")) { g_preview ? g_preview->frameAll() : void(); }
    if (!status_.empty()) ImGui::TextDisabled("%s", status_.c_str());

    ImGui::EndGroup();
#else
    (void)e;
#endif
}

} // namespace

void setActorEditorContentRoot(std::string root) { g_contentRoot = std::move(root); }

void shutdownActorEditors() {
    if (g_device && g_preview) g_device->removeRenderFeature(g_preview);
    g_device = nullptr;
    delete g_preview;
    g_preview = nullptr;
    g_previewTried = false;
}

std::unique_ptr<AssetEditor> makeActorEditor(const std::string& path) {
    // Only a `.cs`, and only one that actually carries a generated region. A hand-written actor with
    // no designer file is not an editing surface this can offer anything for, and opening it here
    // instead of in the IDE would be taking something away.
    const std::filesystem::path p(path);
    if (p.extension() != ".cs") return nullptr;

    std::string source;
    if (!readFile(path, source)) return nullptr;

    fmt::ActorScript parsed = fmt::parseActorScript(source);
    if (parsed.status == fmt::ActorParseStatus::NoRegion) return nullptr;   // not an actor; let the IDE have it
    if (parsed.status != fmt::ActorParseStatus::Ok) {
        // Declined LOUDLY. Malformed is the signal that the text has left the locked grammar, which
        // is the case a Roslyn backend would pick up; until that exists, saying so beats opening a
        // tab that shows nothing and cannot save.
        AVER_WARN("[ActorEditor] {} has a generated region this build cannot read: {}",
                  path, parsed.error);
        return nullptr;
    }
    return std::make_unique<ActorEditor>(path, std::move(parsed), std::move(source));
}

} // namespace aver::editor
