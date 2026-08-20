// The animation editor tab: clip header, transport, timeline, bone tree, an asset browser of every
// clip the rig can play, a track list, and a 3D preview of the SKINNED MESH.
//
// IT USED TO SAY IT COULD NOT DRAW A SKINNED MESH, "because skinning needs the RHI to be able to
// bind a buffer as an SRV or a UAV, which it cannot". That was true for SEVENTEEN MINUTES. This
// file's box-per-bone preview landed at 15:00:28 on 2026-08-01 (commit 1735709); commit 9fbe761 at
// 15:17:25 the same afternoon is titled "RHI: a buffer can be a shader resource, which is what
// blocked GPU skinning". setSrvBuffer/setUavBuffer have been pure virtual on IResourceFactory ever
// since, both backends implement them, and modules/render.skin has been a complete GPU linear-blend
// skinning module with its own self-tests for just as long. The comment outlived its truth by the
// entire life of the file, and a scatter of cubes is what an animator got for it.
//
// THE BONE BOXES ARE STILL HERE, as an overlay toggle rather than as the only thing on screen. They
// are the right view for a rig problem -- which joint is rotating, where a chain is broken -- and
// the mesh is the right view for everything else. Deleting them to celebrate the mesh would have
// traded one incomplete answer for another.
#include "AnimEditor.hpp"

#include "ActorEditor.hpp"

#include "aver/anim/AnimSampler.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/render/preview/ActorPreview.hpp"
#include "aver/render/preview/PreviewMeshCache.hpp"
#include "aver/render/SkinningPass.hpp"
#include "aver/formats/OcMesh.hpp"
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

// The one render feature this editor owns: it turns the pose the UI just sampled into posed
// vertices, once per frame, before the preview draws them.
//
// A FEATURE RATHER THAN A CALL FROM draw(), because a compute dispatch needs an IRenderContext and
// the UI has none -- the editor runs inside ImGui, the GPU work runs inside the frame. This is the
// same shape SkinnedScene uses for the scene path, down to the prePass/overlayPass pair: dispatch
// leaves the output buffer in GeometryRead so the draw can read it, and EVERY frame must hand it
// back to Common before submit, because D3D12 decays buffer state at the end of a command list and
// next frame's barrier would otherwise claim a state the hardware no longer holds.
//
// REGISTERED BEFORE THE PREVIEW, which is what makes the pose on screen this frame's and not last
// frame's: features get prePass in registration order, and ActorPreview renders the preview image
// in its own prePass.
class AnimSkinFeature final : public rhi::IRenderFeature {
public:
    const char* name() const override { return "Aver.AnimEditor.Skin"; }

    bool init(rhi::IDevice& dev) {
        dev_ = &dev;
        ready_ = pass_.init(dev);
        if (!ready_) AVER_WARN("[AnimEditor] GPU skinning unavailable; the preview stays on bone boxes");
        return ready_;
    }

    void shutdown() {
        if (gpu_.valid()) pass_.destroyMesh(gpu_);
        gpu_ = {};
        pass_.shutdown();
        ready_ = false;
        drawMesh_ = 0;
    }

    bool ready() const { return ready_; }
    rhi::MeshHandle drawMesh() const { return drawMesh_; }
    f32 boundsRadius() const { return radius_; }

    // Binds a rig + mesh pair. Idempotent for the same pair, so the editor can call it every frame
    // without rebuilding GPU resources -- which it does, because the clip (and therefore possibly the
    // rig) can change under it when someone clicks the asset browser.
    bool bind(rhi::IDevice& dev, const std::string& meshPath, u32 boneCount) {
        if (!ready_) return false;
        if (meshPath == meshPath_ && gpu_.valid() && boneCount == boneCount_) return true;

        if (gpu_.valid()) pass_.destroyMesh(gpu_);
        gpu_ = {};
        drawMesh_ = 0;
        meshPath_ = meshPath;
        boneCount_ = boneCount;
        if (meshPath.empty() || boneCount == 0) return false;

        fmt::OcMeshData md;
        std::string why;
        if (!fmt::loadOcMesh(meshPath, md, &why)) {
            AVER_WARN("[AnimEditor] {}", why);
            return false;
        }
        if (!md.hasSkin()) {
            // Not an error and not silent: a mesh beside the rig with no skin streams is an ordinary
            // thing to find, and the editor falls back to bone boxes rather than drawing a T-pose that
            // never moves and looks like a broken clip.
            AVER_INFO("[AnimEditor] {} has no skin streams; the preview stays on bone boxes", meshPath);
            return false;
        }

        // OcMeshData keeps positions, normals and uvs as three FLAT float arrays; the RHI wants one
        // interleaved MeshVertex stream. Built here rather than borrowed from PreviewMeshCache
        // because the cache resolves CONTENT-RELATIVE paths and this mesh is found by walking the
        // skeleton's own directory -- and because the skinning pass needs the decoded OcMeshData
        // anyway for its rest and bind streams, so the file is already open.
        const usize vcount = md.positions.size() / 3;
        if (vcount == 0 || md.normals.size() < vcount * 3 || md.uvs.size() < vcount * 2) return false;
        std::vector<rhi::MeshVertex> verts0(vcount);
        for (usize i = 0; i < vcount; ++i) {
            verts0[i].px = md.positions[i * 3 + 0];
            verts0[i].py = md.positions[i * 3 + 1];
            verts0[i].pz = md.positions[i * 3 + 2];
            verts0[i].nx = md.normals[i * 3 + 0];
            verts0[i].ny = md.normals[i * 3 + 1];
            verts0[i].nz = md.normals[i * 3 + 2];
            verts0[i].u  = md.uvs[i * 2 + 0];
            verts0[i].v  = md.uvs[i * 2 + 1];
        }
        const rhi::MeshHandle source = dev.createMesh(verts0.data(), static_cast<u32>(vcount),
                                                       md.indices.data(),
                                                       static_cast<u32>(md.indices.size()));
        if (!source) return false;
        rhi::BufferHandle verts = 0;
        // The DRAW handle whose vertex buffer IS the skin target -- so the posed vertices are what the
        // preview rasterises, with no second copy and nothing taught about a new stream.
        drawMesh_ = dev.createSkinTargetMesh(source, &verts);
        if (!drawMesh_ || !verts) { drawMesh_ = 0; return false; }
        if (!pass_.createMesh(md, boneCount, gpu_, verts)) { drawMesh_ = 0; return false; }

        // A bounds radius for the preview's frameAll, from the rest mesh: the posed mesh moves, but
        // not far enough to matter for framing, and computing it per frame off GPU data we never read
        // back is not possible anyway.
        radius_ = 1.0f;
        for (usize i = 0; i < vcount; ++i) {
            const f32 x = md.positions[i * 3 + 0], y = md.positions[i * 3 + 1], z = md.positions[i * 3 + 2];
            radius_ = std::max(radius_, std::sqrt(x * x + y * y + z * z));
        }
        return true;
    }

    // This frame's skinning matrices, copied because prePass runs later than the UI that produced them.
    void stage(const std::vector<Mat4>& skin) { staged_ = skin; }

    void prePass(rhi::IRenderContext& ctx) override {
        if (!ready_ || !gpu_.valid() || staged_.empty()) return;
        pass_.dispatch(ctx, gpu_, staged_.data(), static_cast<u32>(staged_.size()));
    }

    void overlayPass(rhi::IRenderContext& ctx, u32 width, u32 height) override {
        (void)width; (void)height;
        if (!ready_ || !gpu_.valid()) return;
        // Unconditional, not "only if dispatched": see SkinnedScene::overlayPass, which states the
        // same rule -- skinTransition is a no-op when the state already matches, and a buffer left in
        // GeometryRead at submit is a validation error next frame, not a visible bug this one.
        render::skinTransition(ctx, gpu_, rhi::ResourceState::Common);
    }

private:
    rhi::IDevice*           dev_ = nullptr;
    render::SkinningPass    pass_;
    render::SkinnedMeshGpu  gpu_;
    rhi::MeshHandle         drawMesh_ = 0;
    std::string             meshPath_;
    u32                     boneCount_ = 0;
    f32                     radius_ = 1.0f;
    bool                    ready_ = false;
    std::vector<Mat4>       staged_;
};

AnimSkinFeature g_skin;
bool g_skinTried = false;

// Created and registered ONCE, and BEFORE the shared preview for the ordering reason in
// AnimSkinFeature's comment. Shared rather than per-tab, matching sharedPreview() beside it: two
// animation tabs open at once share one preview already, so a second skinning target would have
// nothing to draw into.
// Keeps the device the feature was registered with, so teardown can unregister from the SAME one.
rhi::IDevice* g_skinDevice = nullptr;

AnimSkinFeature* sharedSkin(Engine& e) {
    if (!g_skinTried && e.device()) {
        g_skinTried = true;
        if (g_skin.init(*e.device())) {
            g_skinDevice = e.device();
            g_skinDevice->addRenderFeature(&g_skin);
        }
    }
    return g_skin.ready() ? &g_skin : nullptr;
}

// One .ocanim or .ocskel, open.
class AnimEditor final : public AssetEditor {
public:
    AnimEditor(std::string path, fmt::OcAnimation clip, fmt::OcSkeleton skel, bool isClip,
                std::string skelPath = {})
        : path_(std::move(path)), skelPath_(std::move(skelPath)), clip_(std::move(clip)),
          skel_(std::move(skel)), isClip_(isClip) {}

    const std::string& path() const override { return path_; }
    std::string title() const override { return std::filesystem::path(path_).filename().string(); }
    void draw(Engine& e) override;
    void onFileChanged() override { reload_ = true; }

private:
    void drawTransport();
    void drawTimeline();
    void drawTracks();
    void drawBones();
    void drawAssetBrowser();
    void buildPreview(Engine& e);
    void reloadIfNeeded();

    // THE ASSET BROWSER, which is Persona's name for it and its shape too: every clip this
    // skeleton can play, listed beside the preview, one click to watch it.
    //
    // WHY IT IS THE FIRST THING THIS EDITOR NEEDED after the preview itself. The FirstPerson
    // project ships 32 clips against one 7-bone skeleton, and until now the only way to see the
    // second one was to close the tab and open another file. An animator compares clips -- walk
    // against run, the two melee swings against each other -- and comparing meant a round trip
    // through the content browser every time.
    struct ClipEntry {
        std::string path;
        std::string name;      // the file stem, which is what the tab shows
        std::string display;   // the same with the rig prefix removed -- see scanClips
        f32  duration = 0.0f;
        u32  tracks = 0;
        // A clip whose highest bone index is past this skeleton's bone count cannot be played
        // against it -- sampling would read off the end of the pose. Computed once at scan time
        // and shown as a disabled row rather than hidden, so a mismatched clip in the folder
        // reads as "not for this rig" instead of as a file that mysteriously is not there.
        bool compatible = true;
    };
    std::vector<ClipEntry> clips_;
    bool clipsScanned_ = false;
    void scanClips();
    void openClip(const std::string& path);

    std::string path_;
    // Where the skeleton came from. findSkeleton() already worked this out and the result was
    // discarded once the bones were loaded; the asset browser needs it to know what the rig is
    // CALLED, which is how it strips the shared <skeleton>_ prefix off 32 clip names.
    std::string skelPath_;
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
    std::vector<Mat4> skin_;      // poseToSkinning output, handed to the GPU each frame
    // The bone boxes are an OVERLAY now, not the picture. On by default only when there is no
    // skinned mesh to show, so a rig with no mesh looks exactly as it always did.
    bool showBones_ = false;
    bool showMesh_ = true;
    bool skinBound_ = false;
    std::string meshPath_;         // <rig>.ocmesh beside the skeleton, if there is one
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

// Every .ocanim beside this asset, with the two facts the list shows and the one that decides
// whether a row is clickable.
//
// SCANNED ONCE, not per frame. Thirty-two clips is thirty-two file reads; doing that every frame
// would be the kind of cost that only shows up on someone else's slower disk.
void AnimEditor::scanClips() {
    clipsScanned_ = true;
    clips_.clear();
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::path(path_).parent_path();
    if (dir.empty()) return;

    const u32 boneCount = static_cast<u32>(skel_.bones.size());
    // The rig's own stem, for the prefix strip below. Taken from the skeleton file when one was
    // resolved, so a project that does NOT follow the <skeleton>_<action> convention simply keeps
    // its full names rather than getting something chopped off the front.
    const std::string rigStem = skelPath_.empty()
        ? std::string()
        : std::filesystem::path(skelPath_).stem().string();
    for (fs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".ocanim") continue;
        fmt::OcAnimation c;
        std::string why;
        if (!fmt::loadOcAnim(it->path().string(), c, &why)) continue;   // unreadable: not a row
        ClipEntry entry;
        entry.path = it->path().string();
        entry.name = it->path().stem().string();
        // THE DISPLAY NAME DROPS THE RIG PREFIX. Clips here are named <skeleton>_<action> by
        // convention -- the same stem convention findSkeleton() already relies on -- so every one
        // of the 32 rows began "character_" and the part that told them apart was the part that
        // got truncated. The full name is still what the tooltip and the tab title show.
        entry.display = entry.name;
        if (!rigStem.empty() && entry.display.size() > rigStem.size() + 1 &&
            entry.display.compare(0, rigStem.size(), rigStem) == 0 &&
            entry.display[rigStem.size()] == '_') {
            entry.display = entry.display.substr(rigStem.size() + 1);
        }
        entry.duration = c.duration;
        entry.tracks = static_cast<u32>(c.tracks.size());
        // COMPATIBILITY BY BONE INDEX, not by skeletonRef. The ref is a filename hint the importer
        // wrote and nothing at runtime reads (see findSkeleton above); the bone indices are what
        // sampling actually uses, so they are what decides whether this clip can drive this rig.
        for (const fmt::OcTrack& t : c.tracks) {
            if (t.boneIndex >= boneCount) { entry.compatible = false; break; }
        }
        clips_.push_back(std::move(entry));
    }
    std::sort(clips_.begin(), clips_.end(),
              [](const ClipEntry& a, const ClipEntry& b) { return a.name < b.name; });
}

// Swaps which clip this tab is previewing, in place.
//
// IN PLACE, rather than opening a second tab, because that is what Persona does and because the
// point of the browser is comparison: a new tab per clip would put the thing being compared behind
// the thing it is compared to. The path changes with it, so the tab title and a later reload both
// name the clip actually on screen.
void AnimEditor::openClip(const std::string& path) {
    fmt::OcAnimation c;
    std::string why;
    if (!fmt::loadOcAnim(path, c, &why)) { AVER_WARN("[AnimEditor] {}", why); return; }
    clip_ = std::move(c);
    path_ = path;
    isClip_ = true;
    time_ = 0.0f;
    playing_ = true;
    selectedBone_ = -1;
}

void AnimEditor::drawAssetBrowser() {
#if AVER_WITH_IMGUI
    if (!clipsScanned_) scanClips();

    if (ImGui::SmallButton("Refresh")) scanClips();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu clip(s)", clips_.size());

    if (clips_.empty()) {
        ImGui::TextDisabled("No .ocanim beside this asset.");
        return;
    }

    const std::string current = std::filesystem::path(path_).stem().string();
    for (const ClipEntry& c : clips_) {
        const bool isCurrent = (c.name == current);
        if (!c.compatible) {
            // Shown and refused, rather than hidden. See ClipEntry::compatible.
            ImGui::BeginDisabled();
            ImGui::Selectable(c.display.c_str(), false);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Not for this skeleton: a track addresses a bone past %zu.",
                                   skel_.bones.size());
            continue;
        }
        if (ImGui::Selectable(c.display.c_str(), isCurrent) && !isCurrent) openClip(c.path);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n%.2f s, %u track(s)", c.name.c_str(), c.duration, c.tracks);
    }
#endif
}

void AnimEditor::buildPreview(Engine& e) {
#if AVER_WITH_IMGUI
    // BEFORE sharedPreview, so the skinning dispatch is registered ahead of the preview render and
    // the pose on screen is this frame's. See AnimSkinFeature's own comment.
    AnimSkinFeature* skin = sharedSkin(e);
    render::preview::ActorPreview* preview = sharedPreview(e);
    if (!preview || !e.device()) return;

    // The mesh is found the way the skeleton already is: by convention, beside it. A rig called
    // character.ocskel is skinned by character.ocmesh -- the same <stem> the clip names use.
    if (meshPath_.empty() && !skelPath_.empty()) {
        std::filesystem::path guess = std::filesystem::path(skelPath_);
        guess.replace_extension(".ocmesh");
        std::error_code ec;
        if (std::filesystem::exists(guess, ec)) meshPath_ = guess.string();
    }
    if (skin && !skinBound_) {
        skinBound_ = true;   // tried once; a failure falls back to bones and does not retry per frame
        showMesh_ = skin->bind(*e.device(), meshPath_, static_cast<u32>(skel_.bones.size()));
        showBones_ = !showMesh_;
    }

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
    // The SAME pose, in the other space the renderer needs: poseToModel gives joint transforms for
    // drawing bones, poseToSkinning gives bind-relative matrices for moving vertices. Both come from
    // one sampled pose, so the boxes and the mesh can never disagree about what frame it is.
    if (skin && showMesh_) {
        anim::poseToSkinning(skel_, pose_, skin_);
        skin->stage(skin_);
    }

    render::preview::PreviewMeshCache& meshes = sharedPreviewMeshes();
    meshes.setContentRoot(*e.device(), g_contentRoot);
    f32 radius = 1.0f;
    const rhi::MeshHandle cube = meshes.resolve(*e.device(), "Meshes/cube.ocmesh", &radius);
    if (!cube) return;

    std::vector<render::preview::PreviewDraw> draws;
    draws.reserve(model_.size() + 1);

    // THE MESH FIRST, at identity: the skinning pass has already put every vertex where the pose
    // says it goes, so a world transform here would move it a second time.
    if (skin && showMesh_ && skin->drawMesh()) {
        render::preview::PreviewDraw d;
        d.mesh = skin->drawMesh();
        d.boundsRadius = skin->boundsRadius();
        d.roughness = 0.62f;
        d.baseColor[0] = 0.78f; d.baseColor[1] = 0.76f; d.baseColor[2] = 0.72f;
        const Mat4 id = Mat4::identity();
        std::memcpy(d.world, &id.m[0][0], sizeof d.world);
        draws.push_back(d);
    }

    for (usize i = 0; showBones_ && i < model_.size(); ++i) {
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

    // What the preview shows. Both can be on at once, which is the useful state while checking
    // whether a joint is where the silhouette says it is.
    {
        AnimSkinFeature* sk = sharedSkin(e);
        ImGui::BeginDisabled(!sk || !sk->drawMesh());
        ImGui::Checkbox("Mesh", &showMesh_);
        ImGui::EndDisabled();
        if ((!sk || !sk->drawMesh()) && ImGui::IsItemHovered()) {
            ImGui::SetTooltip(meshPath_.empty()
                               ? "No <skeleton>.ocmesh beside this rig."
                               : "That mesh has no skin streams, or the device has no compute.");
        }
        ImGui::SameLine();
        ImGui::Checkbox("Bones", &showBones_);
    }

    buildPreview(e);

    ImGui::Separator();
    const f32 h = ImGui::GetContentRegionAvail().y;
    // SCALED, not 240 raw pixels. Every size in this file predates the editor running at 300% DPI,
    // where a 240px column is about 80 logical pixels -- narrow enough that the bone names fit only
    // because they are short. GetFontSize() is the DPI proxy rather than a plumbed-through scale,
    // because it is already correct here and needs nothing threading through four call sites.
    const f32 uiScale = ImGui::GetFontSize() / 16.0f;
    if (ImGui::BeginChild("left", ImVec2(260.0f * uiScale, h), true)) {
        // The bone tree and the clip list share the left column, split so neither starves: the
        // rig is a fixed small thing (7 bones here) and the clip list is the one that grows.
        if (ImGui::BeginChild("bones", ImVec2(0, h * 0.42f), false)) {
            ImGui::TextDisabled("BONES");
            drawBones();
        }
        ImGui::EndChild();
        ImGui::Separator();
        ImGui::TextDisabled("ANIMATIONS");
        if (ImGui::BeginChild("clips", ImVec2(0, 0), false)) {
            drawAssetBrowser();
        }
        ImGui::EndChild();
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

// Unregisters the skinning feature and frees its GPU resources, BEFORE the device goes.
//
// THIS WAS MISSING AND IT CRASHED ON EXIT -- an access violation at shutdown, after the last frame
// had already been captured, which is the most misleading shape a lifetime bug has: everything on
// screen was correct and the process died anyway. A feature registered with addRenderFeature is
// held NON-OWNING by the device, exactly as ActorEditor's own shared preview is, so something has
// to take it back out. sharedPreview had this from the start; the new feature simply did not copy
// it.
void shutdownAnimEditors() {
    if (g_skinDevice) g_skinDevice->removeRenderFeature(&g_skin);
    g_skinDevice = nullptr;
    g_skin.shutdown();
    g_skinTried = false;
}

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
        return std::make_unique<AnimEditor>(path, std::move(clip), std::move(skel), true, rig);
    }
    if (ext == ".ocskel") {
        fmt::OcSkeleton skel;
        if (!fmt::loadOcSkel(path, skel, &why)) {
            AVER_WARN("[AnimEditor] {}", why);
            return nullptr;
        }
        // A .ocskel opened directly IS its own rig, so the browser lists every clip beside it --
        // which is Persona's Skeleton editor, arrived at by the same panel rather than a second one.
        return std::make_unique<AnimEditor>(path, fmt::OcAnimation{}, std::move(skel), false, path);
    }
    return nullptr;
}

} // namespace aver::editor
