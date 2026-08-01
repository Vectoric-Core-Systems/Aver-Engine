// The animation editor tab: clip header, transport, timeline, bone tree, track list, and a 3D
// preview that draws one box per bone.
//
// IT DOES NOT DRAW A SKINNED MESH, and that is the whole reason it exists this early. Skinning needs
// the RHI to be able to bind a buffer as an SRV or a UAV, which it cannot -- see docs/STATUS.md. A
// box per bone is honest about what is being shown, needs nothing new from the renderer, and is what
// makes a clip scrubbable today rather than after an RHI change.
#include "AnimEditor.hpp"

#include "ActorEditor.hpp"

#include "aver/anim/AnimSampler.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/render/preview/ActorPreview.hpp"
#include "aver/render/preview/PreviewMeshCache.hpp"
#include "aver/runtime/Engine.hpp"

#if AVER_WITH_IMGUI
#include "imgui.h"
#endif

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace aver::editor {
namespace {

std::string g_contentRoot;

// A bone's box is drawn along the vector to its parent, so a chain reads as a limb rather than as a
// scatter of cubes. A root, having no parent, gets a small cube at its own joint.
constexpr f32 kBoneThicknessCm = 2.2f;
constexpr f32 kRootCubeCm      = 4.0f;

// The path a clip's skeletonRef names, looked for beside the clip and then under the content root.
// OcAnimation::skeletonRef was written by the importer and read by NOTHING before this.
std::string findSkeleton(const std::string& clipPath, const std::string& ref) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::path(clipPath).parent_path();

    if (!ref.empty()) {
        const fs::path beside = dir / (ref + ".ocskel");
        if (fs::exists(beside, ec)) return beside.string();
        if (!g_contentRoot.empty()) {
            for (fs::recursive_directory_iterator it(g_contentRoot, ec), end; it != end; it.increment(ec)) {
                if (ec) break;
                if (it->is_regular_file(ec) && it->path().stem() == ref &&
                    it->path().extension() == ".ocskel")
                    return it->path().string();
            }
        }
    }
    // No ref, or it named nothing: the only .ocskel beside the clip is a better guess than none.
    std::string only;
    u32 found = 0;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec) && it->path().extension() == ".ocskel") { only = it->path().string(); ++found; }
    }
    return found == 1 ? only : std::string();
}

// Builds the world matrix for a box spanning `from` to `to`, thickened to `thick`.
Mat4 boneBox(const Vec3& from, const Vec3& to, f32 thick) {
    const Vec3 d = to - from;
    const f32 len = d.size();
    if (len < 1e-4f) {
        Mat4 m = Mat4::scale(Vec3{thick, thick, thick});
        return m * Mat4::translation(from);
    }
    // The unit cube spans -1..1, so a half-extent of len/2 along the bone and `thick` across it.
    const Vec3 z = d / len;
    Vec3 up = std::fabs(z.z) > 0.9f ? Vec3{1, 0, 0} : Vec3{0, 0, 1};
    const Vec3 x = cross(up, z).getSafeNormal();
    const Vec3 y = cross(z, x);

    Mat4 basis = Mat4::identity();
    basis.m[0][0] = x.x * thick; basis.m[0][1] = x.y * thick; basis.m[0][2] = x.z * thick;
    basis.m[1][0] = y.x * thick; basis.m[1][1] = y.y * thick; basis.m[1][2] = y.z * thick;
    basis.m[2][0] = z.x * len * 0.5f; basis.m[2][1] = z.y * len * 0.5f; basis.m[2][2] = z.z * len * 0.5f;
    const Vec3 mid = from + d * 0.5f;
    basis.m[3][0] = mid.x; basis.m[3][1] = mid.y; basis.m[3][2] = mid.z;
    return basis;
}

// One .ocanim or .ocskel, open.
class AnimEditor final : public AssetEditor {
public:
    AnimEditor(std::string path, fmt::OcAnimation clip, fmt::OcSkeleton skel, bool isClip)
        : path_(std::move(path)), clip_(std::move(clip)), skel_(std::move(skel)), isClip_(isClip) {}

    const std::string& path() const override { return path_; }
    std::string title() const override { return std::filesystem::path(path_).filename().string(); }
    void draw(Engine& e) override;
    void onFileChanged() override { reload_ = true; }

private:
    void drawTransport();
    void drawTimeline();
    void drawTracks();
    void drawBones();
    void buildPreview(Engine& e);
    void reloadIfNeeded();

    std::string path_;
    fmt::OcAnimation clip_;
    fmt::OcSkeleton skel_;
    bool isClip_ = true;
    bool reload_ = false;

    f32 time_ = 0.0f;
    f32 speed_ = 1.0f;
    bool playing_ = true;
    bool loop_ = true;
    int selectedBone_ = -1;
    f64 lastClock_ = 0.0;

    anim::Pose pose_;
    std::vector<Mat4> model_;
    bool framed_ = false;
    u32 pendingW_ = 0, pendingH_ = 0;
    f64 resizeDue_ = 0.0;
};

void AnimEditor::reloadIfNeeded() {
    if (!reload_) return;
    reload_ = false;
    std::string why;
    if (isClip_) {
        fmt::OcAnimation c;
        if (fmt::loadOcAnim(path_, c, &why)) clip_ = std::move(c);
        else AVER_WARN("[AnimEditor] {}", why);
    } else {
        fmt::OcSkeleton s;
        if (fmt::loadOcSkel(path_, s, &why)) skel_ = std::move(s);
        else AVER_WARN("[AnimEditor] {}", why);
    }
}

void AnimEditor::buildPreview(Engine& e) {
#if AVER_WITH_IMGUI
    render::preview::ActorPreview* preview = sharedPreview(e);
    if (!preview || !e.device()) return;

    anim::restPose(skel_, pose_);
    if (isClip_ && !clip_.tracks.empty()) {
        f32 t = time_;
        if (clip_.duration > 0.0f) {
            if (loop_) { t = std::fmod(t, clip_.duration); if (t < 0.0f) t += clip_.duration; }
            else       { t = t < 0.0f ? 0.0f : (t > clip_.duration ? clip_.duration : t); }
        }
        anim::sampleAnimation(clip_, t, pose_);
    }
    anim::poseToModel(skel_, pose_, model_);

    render::preview::PreviewMeshCache& meshes = sharedPreviewMeshes();
    meshes.setContentRoot(*e.device(), g_contentRoot);
    f32 radius = 1.0f;
    const rhi::MeshHandle cube = meshes.resolve(*e.device(), "Meshes/cube.ocmesh", &radius);
    if (!cube) return;

    std::vector<render::preview::PreviewDraw> draws;
    draws.reserve(model_.size());
    for (usize i = 0; i < model_.size(); ++i) {
        const Vec3 here{model_[i].m[3][0], model_[i].m[3][1], model_[i].m[3][2]};
        const i32 parent = skel_.bones[i].parent;
        render::preview::PreviewDraw d;
        d.mesh = cube;
        d.boundsRadius = radius;
        d.roughness = 0.55f;
        d.selected = static_cast<int>(i) == selectedBone_;
        // The selected bone is amber, a root is pale, everything else is the neutral the actor tab
        // already uses -- so the hierarchy reads without a legend.
        if (d.selected) { d.baseColor[0] = 0.95f; d.baseColor[1] = 0.62f; d.baseColor[2] = 0.18f; }
        else if (parent < 0) { d.baseColor[0] = 0.85f; d.baseColor[1] = 0.86f; d.baseColor[2] = 0.90f; }

        Mat4 m;
        if (parent >= 0 && static_cast<usize>(parent) < model_.size()) {
            const Vec3 from{model_[usize(parent)].m[3][0], model_[usize(parent)].m[3][1],
                            model_[usize(parent)].m[3][2]};
            m = boneBox(from, here, kBoneThicknessCm);
        } else {
            m = Mat4::scale(Vec3{kRootCubeCm, kRootCubeCm, kRootCubeCm}) * Mat4::translation(here);
        }
        std::memcpy(d.world, &m.m[0][0], sizeof d.world);
        draws.push_back(d);
    }
    preview->setDrawList(std::move(draws));

    // FRAMED ONCE, by the preview's own frameAll rather than by hand. It scales each draw's bounds
    // radius by that draw's world scale (ActorPreview.cpp:267), which a hand-rolled pass over the
    // joint positions does not -- and a rig is a few big boxes, so ignoring their size put the
    // camera inside them.
    if (!framed_) {
        framed_ = true;
        preview->frameAll();
        // ...then back off. frameAll sizes on a draw's bounds radius times its world scale, and a
        // bone box is deliberately non-uniform -- thin across, long along -- so the radius it picks
        // is the thin one and the camera lands inside the rig. The margin is a constant rather than
        // a cleverer fit because the orbit is under the mouse anyway.
        preview->camera().addZoom(2.6f);
    }
#else
    (void)e;
#endif
}

void AnimEditor::drawTransport() {
#if AVER_WITH_IMGUI
    if (ImGui::Button(playing_ ? "Pause" : "Play")) playing_ = !playing_;
    ImGui::SameLine();
    if (ImGui::Button("Stop")) { playing_ = false; time_ = 0.0f; }
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &loop_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f * ImGui::GetFontSize() / 13.0f);
    ImGui::SliderFloat("Speed", &speed_, -2.0f, 4.0f, "%.2fx");
    ImGui::SameLine();
    if (ImGui::Button("1x")) speed_ = 1.0f;
#endif
}

void AnimEditor::drawTimeline() {
#if AVER_WITH_IMGUI
    const f32 dur = clip_.duration > 0.0f ? clip_.duration : 1.0f;
    ImGui::SetNextItemWidth(-1);
    // Scrubbing PAUSES, because a slider that fights the clock cannot be placed.
    if (ImGui::SliderFloat("##time", &time_, 0.0f, dur, "%.3f s")) playing_ = false;

    // The key times of every track, so a scrub can be landed on a key rather than near one.
    const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const fmt::OcTrack& t : clip_.tracks) {
        for (const f32 k : t.times) {
            const f32 x = p0.x + (p1.x - p0.x) * (dur > 0.0f ? k / dur : 0.0f);
            dl->AddLine(ImVec2(x, p1.y - 4.0f), ImVec2(x, p1.y), IM_COL32(240, 190, 90, 200), 1.0f);
        }
    }
#endif
}

void AnimEditor::drawTracks() {
#if AVER_WITH_IMGUI
    if (clip_.tracks.empty()) { ImGui::TextDisabled("no tracks"); return; }
    if (!ImGui::BeginTable("tracks", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                        ImGuiTableFlags_ScrollY)) return;
    ImGui::TableSetupColumn("Bone");
    ImGui::TableSetupColumn("Channels");
    ImGui::TableSetupColumn("Interp");
    ImGui::TableSetupColumn("Keys");
    ImGui::TableSetupColumn("Span");
    ImGui::TableHeadersRow();
    for (const fmt::OcTrack& t : clip_.tracks) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const bool named = t.boneIndex < skel_.bones.size();
        ImGui::TextUnformatted(named ? skel_.bones[t.boneIndex].name.c_str() : "<out of range>");
        ImGui::TableNextColumn();
        std::string ch;
        if (t.channels & fmt::kOcChannelTranslation) ch += "T";
        if (t.channels & fmt::kOcChannelRotation)    ch += "R";
        if (t.channels & fmt::kOcChannelScale)       ch += "S";
        ImGui::TextUnformatted(ch.empty() ? "-" : ch.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(t.interp == fmt::OcInterp::Step ? "Step"
                             : t.interp == fmt::OcInterp::CubicSpline ? "Cubic" : "Linear");
        ImGui::TableNextColumn();
        ImGui::Text("%zu", t.times.size());
        ImGui::TableNextColumn();
        if (t.times.empty()) ImGui::TextUnformatted("-");
        else ImGui::Text("%.2f - %.2f s", t.times.front(), t.times.back());
    }
    ImGui::EndTable();
#endif
}

void AnimEditor::drawBones() {
#if AVER_WITH_IMGUI
    if (skel_.bones.empty()) { ImGui::TextDisabled("no skeleton"); return; }
    for (usize i = 0; i < skel_.bones.size(); ++i) {
        const fmt::OcBone& b = skel_.bones[i];
        ImGui::PushID(static_cast<int>(i));
        // Indented by depth rather than nested, because a tree of one-child chains is all twisties.
        int depth = 0;
        for (i32 p = b.parent; p >= 0 && depth < 32; p = skel_.bones[usize(p)].parent) ++depth;
        ImGui::Indent(depth * 12.0f);
        const bool sel = static_cast<int>(i) == selectedBone_;
        if (ImGui::Selectable(b.name.empty() ? "<unnamed>" : b.name.c_str(), sel))
            selectedBone_ = sel ? -1 : static_cast<int>(i);
        if (ImGui::IsItemHovered() && i < model_.size())
            ImGui::SetTooltip("bone %zu  model (%.1f, %.1f, %.1f) cm", i,
                              model_[i].m[3][0], model_[i].m[3][1], model_[i].m[3][2]);
        ImGui::Unindent(depth * 12.0f);
        ImGui::PopID();
    }
#endif
}

void AnimEditor::draw(Engine& e) {
#if AVER_WITH_IMGUI
    reloadIfNeeded();

    // THE CLOCK RUNS OFF ImGui's, not the engine's frame delta. An asset tab only draws while it is
    // the active dock tab, so an engine-dt clock would keep accumulating while the tab was hidden
    // and the clip would jump on the way back.
    const f64 now = ImGui::GetTime();
    const f32 dt = lastClock_ > 0.0 ? static_cast<f32>(now - lastClock_) : 0.0f;
    lastClock_ = now;
    if (playing_ && isClip_) {
        time_ += dt * speed_;
        if (clip_.duration > 0.0f) {
            if (loop_) { time_ = std::fmod(time_, clip_.duration); if (time_ < 0.0f) time_ += clip_.duration; }
            else if (time_ >= clip_.duration) { time_ = clip_.duration; playing_ = false; }
        }
    }

    ImGui::Text("%s", isClip_ ? "Animation clip" : "Skeleton");
    ImGui::SameLine();
    if (isClip_)
        ImGui::TextDisabled("%.3f s   %zu track(s)   %s   %s", clip_.duration, clip_.tracks.size(),
                            clip_.storage == fmt::OcAnimStorage::BakedUniform ? "baked" : "keyframed",
                            (clip_.flags & fmt::kOcAnimLoop) ? "loop" : "one-shot");
    else
        ImGui::TextDisabled("%zu bone(s)", skel_.bones.size());

    if (skel_.bones.empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
                           "No skeleton found for this clip.");
        ImGui::TextWrapped("A clip stores the NAME of the skeleton it was authored against "
                           "(skeletonRef = '%s'). Put the matching .ocskel beside it, or anywhere "
                           "under Content.", clip_.skeletonRef.c_str());
    }

    ImGui::Separator();
    if (isClip_) { drawTransport(); drawTimeline(); }

    buildPreview(e);

    ImGui::Separator();
    const f32 h = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginChild("left", ImVec2(240, h), true)) {
        ImGui::TextDisabled("BONES");
        drawBones();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    render::preview::ActorPreview* preview = sharedPreview(e);
    const f32 midW = ImGui::GetContentRegionAvail().x * 0.62f;
    if (ImGui::BeginChild("view", ImVec2(midW, h), true)) {
        if (preview && preview->uiTextureId()) {
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            // DEBOUNCED. resize() waits for the GPU to go idle and destroys a texture ImGui is
            // still sampling, so resizing on every frame of a drag stalls the editor.
            const u32 w = static_cast<u32>(std::max(64.0f, avail.x));
            const u32 hh = static_cast<u32>(std::max(64.0f, avail.y));
            if (w != pendingW_ || hh != pendingH_) {
                pendingW_ = w; pendingH_ = hh;
                resizeDue_ = ImGui::GetTime() + 0.25;
            } else if (resizeDue_ > 0.0 && ImGui::GetTime() >= resizeDue_) {
                resizeDue_ = 0.0;
                preview->resize(pendingW_, pendingH_);
            }
            const f32 iw = avail.x, ih = avail.y;
            ImGui::Image(static_cast<ImTextureID>(preview->uiTextureId()), ImVec2(iw, ih));
            if (ImGui::IsItemHovered()) {
                const ImGuiIO& io = ImGui::GetIO();
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                    preview->camera().addOrbit(io.MouseDelta.x * 0.4f, io.MouseDelta.y * 0.4f);
                if (io.MouseWheel != 0.0f)
                    preview->camera().addZoom(io.MouseWheel > 0.0f ? 0.9f : 1.1f);
            }
        } else {
            ImGui::TextDisabled("No preview on this backend.");
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    if (ImGui::BeginChild("tracks", ImVec2(0, h), true)) {
        ImGui::TextDisabled("TRACKS");
        drawTracks();
    }
    ImGui::EndChild();
#else
    (void)e;
#endif
}

} // namespace

void setAnimEditorContentRoot(std::string root) { g_contentRoot = std::move(root); }

std::unique_ptr<AssetEditor> makeAnimEditor(const std::string& path) {
    const std::string ext = std::filesystem::path(path).extension().string();
    std::string why;

    if (ext == ".ocanim") {
        fmt::OcAnimation clip;
        if (!fmt::loadOcAnim(path, clip, &why)) {
            AVER_WARN("[AnimEditor] {}", why);
            return nullptr;
        }
        fmt::OcSkeleton skel;
        const std::string rig = findSkeleton(path, clip.skeletonRef);
        if (!rig.empty() && !fmt::loadOcSkel(rig, skel, &why))
            AVER_WARN("[AnimEditor] {} names skeleton '{}' but it did not load: {}",
                      path, clip.skeletonRef, why);
        return std::make_unique<AnimEditor>(path, std::move(clip), std::move(skel), true);
    }
    if (ext == ".ocskel") {
        fmt::OcSkeleton skel;
        if (!fmt::loadOcSkel(path, skel, &why)) {
            AVER_WARN("[AnimEditor] {}", why);
            return nullptr;
        }
        return std::make_unique<AnimEditor>(path, fmt::OcAnimation{}, std::move(skel), false);
    }
    return nullptr;
}

} // namespace aver::editor
