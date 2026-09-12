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
#include "EditorKeybinds.hpp"

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

    // THIS EDITOR COULD NOT WRITE ANYTHING UNTIL NOW. It opened a clip, drew it, and that was the
    // whole contract -- which was honest while everything on screen was derived from the file. A
    // notify is the first thing an author can CREATE here, so the tab needs the dirty/save pair
    // every other asset editor already has.
    bool dirty() const override { return dirty_; }
    bool save(std::string* why) override {
        // A SKELETON HAS SOMETHING TO SAVE NOW. It did not when this override was written -- the
        // rig tab showed a bone tree derived entirely from the file and offered nothing to change.
        // Sockets are authored here, so refusing to write a .ocskel would make them unsavable.
        const bool ok = isClip_ ? fmt::saveOcAnim(path_, clip_, why)
                                : fmt::saveOcSkel(path_, skel_, why);
        if (!ok) return false;
        dirty_ = false;
        return true;
    }
    void draw(Engine& e) override;
    void onFileChanged() override { reload_ = true; }

private:
    void drawTransport();
    void drawTimeline();
    void drawTracks();
    void drawBones();
    void drawAssetBrowser();
    void drawNotifies();
    void drawSockets();
    void drawCurves();
    void buildPreview(Engine& e);
    void reloadIfNeeded();

    // ITEM 1.3: loop, additive-base and root-motion all persist through parseOcAnim/writeOcAnim
    // already (clip_.flags is a plain u8 the parser fills and the writer emits verbatim) -- this
    // editor just never offered anywhere to CHANGE one. One checkbox per bit, each toggling `bit` in
    // clip_.flags and marking the tab dirty so the existing Save path (fmt::saveOcAnim(path_, clip_,
    // why), which rewrites clip_ WHOLE) picks it up along with everything else already in clip_.
    void flagCheckbox(u8 bit, const char* label);

    // NOTIFY STATE DURATIONS. clip_.notifyDurations is left EMPTY until the first one is authored --
    // see OcAnimation::notifyDurations in OcAnim.hpp -- so these two are the only places this editor
    // touches it, and both keep it either empty or exactly parallel to clip_.notifies, never a third
    // shape writeOcAnim would refuse.
    f32 notifyDurationAt(usize i) const {
        return i < clip_.notifyDurations.size() ? clip_.notifyDurations[i] : 0.0f;
    }
    void setNotifyDuration(usize i, f32 seconds) {
        if (clip_.notifyDurations.size() != clip_.notifies.size())
            clip_.notifyDurations.assign(clip_.notifies.size(), 0.0f);
        if (i < clip_.notifyDurations.size()) clip_.notifyDurations[i] = seconds;
    }

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
    bool dirty_ = false;
    int  selectedNotify_ = -1;
    char notifyNameBuf_[96] = {};
    int  selectedSocket_ = -1;
    char socketNameBuf_[96] = {};
    int  selectedCurve_ = -1;
    char curveNameBuf_[96] = {};

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

    // SOCKETS, drawn whether or not the bones are. A socket is a thing an author is placing by
    // eye, so hiding it behind the "Bones" toggle would hide it exactly when the mesh is on and
    // the placement actually matters -- "is the grip inside the hand" is a question you ask with
    // the hand visible.
    for (usize i = 0; i < skel_.sockets.size(); ++i) {
        Mat4 sm;
        if (!anim::socketModelMatrix(model_, skel_.sockets[i], sm)) continue;
        const Vec3 at{sm.m[3][0], sm.m[3][1], sm.m[3][2]};
        render::preview::PreviewDraw d;
        d.mesh = cube;
        d.boundsRadius = radius;
        d.roughness = 0.35f;
        d.selected = static_cast<int>(i) == selectedSocket_;
        // Cyan, and the same cyan the notify markers use on the timeline: one colour meaning
        // "a thing an author added" across the whole editor.
        d.baseColor[0] = 0.18f; d.baseColor[1] = 0.72f; d.baseColor[2] = 0.95f;
        if (d.selected) { d.baseColor[0] = 0.98f; d.baseColor[1] = 0.82f; d.baseColor[2] = 0.30f; }
        // Deliberately SMALLER than a root cube: a socket marks a point, and a marker as big as
        // the joint it sits on would swallow the joint.
        const f32 sz = kRootCubeCm * 0.55f;
        const Mat4 mm = Mat4::scale(Vec3{sz, sz, sz}) * Mat4::translation(at);
        std::memcpy(d.world, &mm.m[0][0], sizeof d.world);
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

// The notify list: add at the playhead, rename, retime, delete.
//
// ADDED AT THE PLAYHEAD rather than at a typed time, because placing an event in an animation is
// something an author does by SCRUBBING to the frame and saying "here" -- the number is the result,
// not the input. It is editable afterwards for the case where the number is what you actually have.
// The socket list: add on the selected bone, rename, retarget, nudge the offset, delete.
//
// ADDED ON THE SELECTED BONE, not on a bone picked from a second list, because choosing where a
// socket goes is choosing a bone -- and the bone tree is already open on the left with one
// highlighted. A socket created against nothing would need a bone chosen before it meant anything,
// which is a second decision for no gain.
// The curve list: what each one reads AT THE PLAYHEAD, and its keys.
//
// THE VALUE AT THE PLAYHEAD IS THE HEADLINE, not the key list, and that is the whole reason this
// panel is worth having over editing the file by hand. A curve is a number that varies, and the
// question an author has is "what does it read HERE" -- which is answered by scrubbing and reading,
// not by looking at three keyframes and interpolating in your head.
//
// NOT A GRAPH EDITOR. Unreal draws curves on a 2D graph with draggable tangents; this draws the
// shape over the timeline and edits keys as numbers. That is a real difference in authoring comfort
// and it is stated rather than glossed: the format stores no tangents (see OcCurve), so a tangent
// handle would have nothing to write to.
void AnimEditor::drawCurves() {
#if AVER_WITH_IMGUI
    if (!isClip_) { ImGui::TextDisabled("a skeleton has no curves"); return; }

    if (ImGui::SmallButton("Add curve")) {
        fmt::OcCurve c;
        // Made unique on creation, for the reason a socket is: curve() returns the FIRST match, so a
        // duplicate name leaves the loser permanently unreadable by name.
        std::string name = "NewCurve";
        for (int n = 1; clip_.curve(name) != nullptr; ++n) name = "NewCurve" + std::to_string(n);
        c.name = name;
        // ONE KEY AT THE PLAYHEAD rather than none. An empty curve reads the caller's fallback
        // everywhere, which looks identical to a curve that is not there -- so a curve created with
        // no keys would appear broken the moment anything read it.
        c.times = {time_};
        c.values = {0.0f};
        clip_.curves.push_back(c);
        selectedCurve_ = static_cast<int>(clip_.curves.size()) - 1;
        std::snprintf(curveNameBuf_, sizeof curveNameBuf_, "%s", c.name.c_str());
        dirty_ = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu curve(s)", clip_.curves.size());

    if (clip_.curves.empty()) {
        ImGui::TextDisabled("None. A curve is a named float that varies over the clip.");
        return;
    }

    for (usize i = 0; i < clip_.curves.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        fmt::OcCurve& c = clip_.curves[i];
        const bool sel = (static_cast<int>(i) == selectedCurve_);

        char row[192];
        std::snprintf(row, sizeof row, "%s   = %.4f", c.name.c_str(), anim::sampleCurve(c, time_));
        if (ImGui::Selectable(row, sel)) {
            selectedCurve_ = static_cast<int>(i);
            std::snprintf(curveNameBuf_, sizeof curveNameBuf_, "%s", c.name.c_str());
        }

        if (sel) {
            ImGui::Indent();
            const f32 w = 170.0f * (ImGui::GetFontSize() / 16.0f);
            ImGui::SetNextItemWidth(w);
            ImGui::InputText("Name", curveNameBuf_, sizeof curveNameBuf_);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                std::string next = curveNameBuf_;
                while (!next.empty() && next.front() == ' ') next.erase(next.begin());
                while (!next.empty() && next.back() == ' ') next.pop_back();
                const fmt::OcCurve* clash = next.empty() ? nullptr : clip_.curve(next);
                if (clash && clash != &c) {
                    AVER_WARN("[AnimEditor] a curve called '{}' already exists on this clip", next);
                    std::snprintf(curveNameBuf_, sizeof curveNameBuf_, "%s", c.name.c_str());
                } else if (!next.empty() && next != c.name) { c.name = next; dirty_ = true; }
            }

            // CUBICSPLINE IS NOT OFFERED. The format can carry it and the sampler reads it as linear,
            // because a curve stores one value per key with no tangents -- so putting it in this combo
            // would let an author choose a mode that does nothing.
            int mode = c.interp == fmt::OcInterp::Step ? 1 : 0;
            ImGui::SetNextItemWidth(w);
            if (ImGui::Combo("Interp", &mode, "Linear\0Step\0")) {
                c.interp = mode == 1 ? fmt::OcInterp::Step : fmt::OcInterp::Linear;
                dirty_ = true;
            }

            if (ImGui::SmallButton("Add key at playhead")) {
                // INSERTED IN TIME ORDER. The sampler binary-searches `times`, so an out-of-order key
                // does not merely look odd in the list -- it makes every lookup past it wrong.
                usize at = 0;
                while (at < c.times.size() && c.times[at] < time_) ++at;
                c.times.insert(c.times.begin() + static_cast<isize>(at), time_);
                c.values.insert(c.values.begin() + static_cast<isize>(at), anim::sampleCurve(c, time_));
                dirty_ = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete curve")) {
                clip_.curves.erase(clip_.curves.begin() + static_cast<isize>(i));
                selectedCurve_ = -1;
                dirty_ = true;
                ImGui::Unindent();
                ImGui::PopID();
                break;
            }

            for (usize k = 0; k < c.times.size() && k < c.values.size(); ++k) {
                ImGui::PushID(static_cast<int>(k));
                f32 kv[2] = {c.times[k], c.values[k]};
                ImGui::SetNextItemWidth(w * 1.4f);
                if (ImGui::DragFloat2("##key", kv, 0.01f)) {
                    // The time is CLAMPED BETWEEN ITS NEIGHBOURS rather than sorted after the fact, so
                    // dragging a key can never reorder the array under the binary search. Dragging past
                    // a neighbour holds instead of swapping -- delete and re-add to move a key past
                    // another, which is rare and unambiguous.
                    const f32 lo = k > 0 ? c.times[k - 1] : -1e9f;
                    const f32 hi = (k + 1) < c.times.size() ? c.times[k + 1] : 1e9f;
                    c.times[k] = kv[0] < lo ? lo : (kv[0] > hi ? hi : kv[0]);
                    c.values[k] = kv[1];
                    dirty_ = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    c.times.erase(c.times.begin() + static_cast<isize>(k));
                    c.values.erase(c.values.begin() + static_cast<isize>(k));
                    dirty_ = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
#endif
}

void AnimEditor::drawSockets() {
#if AVER_WITH_IMGUI
    if (skel_.bones.empty()) { ImGui::TextDisabled("no skeleton"); return; }
    // SKELETON TAB ONLY, and this is a correctness rule rather than a layout preference. A socket
    // lives on the .ocskel; save() on a CLIP tab writes the .ocanim. Editing a socket from a clip
    // would set dirty_, write the clip, clear the flag, and lose the socket without a word. The bone
    // tree is shown on both tabs because reading a rig while looking at a clip is useful; WRITING one
    // from there is not, and there is a tab where it works.
    if (isClip_) {
        if (skel_.sockets.empty()) ImGui::TextDisabled("None on this rig.");
        else for (const fmt::OcSocket& k : skel_.sockets) {
            const char* bn = k.bone < skel_.bones.size() ? skel_.bones[k.bone].name.c_str() : "?";
            ImGui::BulletText("%s  on %s", k.name.c_str(), bn);
        }
        ImGui::TextDisabled("Open the .ocskel to edit these.");
        return;
    }

    // Save sits here for the same reason it sits on the notify panel: the host only calls
    // saveAllDirty() from the quit prompt, so without a button the only way to persist an authored
    // socket would be to close the editor.
    ImGui::BeginDisabled(!dirty_);
    const bool saveClicked = ImGui::SmallButton("Save##sock");
    ImGui::EndDisabled();
    const bool saveKey = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                         keybinds().pressed(CommandId::AssetSave, ImGui::GetIO());
    if (dirty_ && (saveClicked || saveKey)) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[AnimEditor] save failed for '{}': {}", path_, why);
    }
    ImGui::SameLine();

    const bool haveBone = selectedBone_ >= 0 && static_cast<usize>(selectedBone_) < skel_.bones.size();
    ImGui::BeginDisabled(!haveBone);
    if (ImGui::SmallButton("Add on selected bone")) {
        fmt::OcSocket k;
        k.bone = static_cast<u32>(selectedBone_);
        // NAMED AFTER THE BONE AND MADE UNIQUE, because the format does not enforce unique names and
        // OcSkeleton::socket returns the FIRST match -- two sockets called "Socket" would leave the
        // second one permanently unreachable by name, which is a trap the editor should not set.
        const std::string base = skel_.bones[static_cast<usize>(selectedBone_)].name + "_Socket";
        std::string name = base;
        for (int n = 1; skel_.socket(name) != nullptr; ++n) name = base + std::to_string(n);
        k.name = name;
        skel_.sockets.push_back(k);
        selectedSocket_ = static_cast<int>(skel_.sockets.size()) - 1;
        std::snprintf(socketNameBuf_, sizeof socketNameBuf_, "%s", k.name.c_str());
        dirty_ = true;
    }
    ImGui::EndDisabled();
    if (!haveBone && ImGui::IsItemHovered()) ImGui::SetTooltip("Select a bone in the tree first.");

    if (skel_.sockets.empty()) {
        ImGui::TextDisabled("None. Select a bone and press Add.");
        return;
    }

    for (usize i = 0; i < skel_.sockets.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        fmt::OcSocket& k = skel_.sockets[i];
        const bool sel = (static_cast<int>(i) == selectedSocket_);
        const char* boneName = k.bone < skel_.bones.size()
                             ? skel_.bones[k.bone].name.c_str() : "<out of range>";

        char row[192];
        std::snprintf(row, sizeof row, "%s   on %s", k.name.c_str(), boneName);
        if (ImGui::Selectable(row, sel)) {
            selectedSocket_ = static_cast<int>(i);
            std::snprintf(socketNameBuf_, sizeof socketNameBuf_, "%s", k.name.c_str());
            // Selecting a socket selects its bone, so the tree, the preview highlight and this list
            // all agree about what is being looked at.
            if (k.bone < skel_.bones.size()) selectedBone_ = static_cast<int>(k.bone);
        }

        if (sel) {
            ImGui::Indent();
            const f32 w = 170.0f * (ImGui::GetFontSize() / 16.0f);
            ImGui::SetNextItemWidth(w);
            ImGui::InputText("Name", socketNameBuf_, sizeof socketNameBuf_);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                std::string next = socketNameBuf_;
                while (!next.empty() && next.front() == ' ') next.erase(next.begin());
                while (!next.empty() && next.back() == ' ') next.pop_back();
                // A rename onto a name that already exists is REFUSED rather than accepted, for the
                // same reason the add path disambiguates: the loser would be unreachable by name.
                const fmt::OcSocket* clash = next.empty() ? nullptr : skel_.socket(next);
                if (clash && clash != &k) {
                    AVER_WARN("[AnimEditor] a socket called '{}' already exists on this rig", next);
                    std::snprintf(socketNameBuf_, sizeof socketNameBuf_, "%s", k.name.c_str());
                } else if (!next.empty() && next != k.name) {
                    k.name = next;
                    dirty_ = true;
                }
            }

            ImGui::SetNextItemWidth(w);
            // Gated like its sibling "Add on selected bone" above, which has always been
            // BeginDisabled(!haveBone) with a reason. This one was enabled with no bone selected and
            // silently did nothing when clicked.
            ImGui::BeginDisabled(!haveBone);
            if (ImGui::SmallButton("Move to selected bone") && k.bone != static_cast<u32>(selectedBone_)) {
                k.bone = static_cast<u32>(selectedBone_);
                dirty_ = true;
            }
            ImGui::EndDisabled();
            if (!haveBone && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Select a bone in the skeleton list first.");

            // CENTIMETRES, and dragged rather than typed: placing a grip is done by watching the
            // marker in the preview, not by knowing the number. The number is still editable for the
            // case where it IS what you have.
            f32 t[3] = {k.translation.x, k.translation.y, k.translation.z};
            ImGui::SetNextItemWidth(w * 1.6f);
            if (ImGui::DragFloat3("Offset (cm)", t, 0.25f)) {
                k.translation = Vec3{t[0], t[1], t[2]};
                dirty_ = true;
            }
            f32 q[4] = {k.rotation.x, k.rotation.y, k.rotation.z, k.rotation.w};
            ImGui::SetNextItemWidth(w * 1.6f);
            if (ImGui::DragFloat4("Rotation (xyzw)", q, 0.01f)) {
                // RENORMALISED ON EDIT. Dragging four components independently leaves a quaternion
                // that is not a rotation, and the composition downstream would scale the attachment
                // rather than turn it. A zero-length drag falls back to identity instead of NaN.
                const f32 len = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
                k.rotation = len > 1e-6f ? Quat{q[0]/len, q[1]/len, q[2]/len, q[3]/len}
                                         : Quat{0, 0, 0, 1};
                dirty_ = true;
            }

            if (ImGui::SmallButton("Delete")) {
                skel_.sockets.erase(skel_.sockets.begin() + static_cast<isize>(i));
                selectedSocket_ = -1;
                dirty_ = true;
                ImGui::Unindent();
                ImGui::PopID();
                break;   // the vector moved under this loop; next frame redraws it
            }
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
#endif
}

void AnimEditor::drawNotifies() {
#if AVER_WITH_IMGUI
    if (!isClip_) { ImGui::TextDisabled("a skeleton has no notifies"); return; }

    // SAVE LIVES HERE, next to the only thing in this editor that can be edited. The host only
    // ever calls saveAllDirty() from the quit prompt, so without this the sole way to write an
    // authored notify to disk would be to close the editor -- and Ctrl+S alongside it, matching
    // GraphEditor's own toolbar exactly, because that is where a hand already goes.
    ImGui::BeginDisabled(!dirty_);
    const bool saveClicked = ImGui::SmallButton("Save");
    ImGui::EndDisabled();
    const bool saveKey = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                         keybinds().pressed(CommandId::AssetSave, ImGui::GetIO());
    if (dirty_ && (saveClicked || saveKey)) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[AnimEditor] save failed for '{}': {}", path_, why);
    }
    ImGui::SameLine();

    if (ImGui::SmallButton("Add at playhead")) {
        fmt::OcNotify n;
        n.time = time_;
        n.name = "OnNotify";
        clip_.notifies.push_back(n);
        // Only when durations are ALREADY in use for this clip -- keeps a clip with no states at all
        // leaving notifyDurations empty, which is what keeps it writing no NTFD chunk.
        if (!clip_.notifyDurations.empty()) clip_.notifyDurations.push_back(0.0f);
        selectedNotify_ = static_cast<int>(clip_.notifies.size()) - 1;
        std::snprintf(notifyNameBuf_, sizeof notifyNameBuf_, "%s", n.name.c_str());
        dirty_ = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu notify(s)", clip_.notifies.size());

    if (clip_.notifies.empty()) {
        ImGui::TextDisabled("Scrub to a frame and press Add.");
        return;
    }

    for (usize i = 0; i < clip_.notifies.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        fmt::OcNotify& n = clip_.notifies[i];
        const bool sel = (static_cast<int>(i) == selectedNotify_);

        // THE TIME AND THE NAME GO INSIDE THE SELECTABLE, not beside it. A Selectable given a zero
        // width spans the whole line, so a SameLine after one starts at the right-hand edge and
        // everything drawn there is off the end of the panel. PushID above already makes the row
        // unique, so two notifies with identical text are still two rows.
        char row[160];
        const f32 rowDur = notifyDurationAt(i);
        // A STATE'S WINDOW SHOWS IN THE COLLAPSED ROW, not only once expanded -- otherwise the one
        // fact that makes a notify a hit window rather than an instant is invisible until clicked.
        if (rowDur > 0.0f)
            std::snprintf(row, sizeof row, "%7.3f s + %5.3f s   %s", n.time, rowDur, n.name.c_str());
        else
            std::snprintf(row, sizeof row, "%7.3f s   %s", n.time, n.name.c_str());
        if (ImGui::Selectable(row, sel)) {
            selectedNotify_ = static_cast<int>(i);
            std::snprintf(notifyNameBuf_, sizeof notifyNameBuf_, "%s", n.name.c_str());
            // Selecting a notify moves the playhead to it, which is the only way to SEE what it is
            // marking. Pauses, for the reason the scrubber does: a playhead that keeps running has
            // not been placed anywhere.
            time_ = n.time;
            playing_ = false;
        }

        if (sel) {
            ImGui::Indent();
            ImGui::SetNextItemWidth(180.0f * (ImGui::GetFontSize() / 16.0f));
            ImGui::InputText("Event", notifyNameBuf_, sizeof notifyNameBuf_);
            // Committed on deactivate, the same activate/apply/deactivate boundary the graph editor's
            // own text fields use, so one typed name is one edit and not one per keystroke.
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                std::string next = notifyNameBuf_;
                // A name is fired as a graph event and lands in a NODE-adjacent record, so leading and
                // trailing space is silently unhelpful rather than an error worth refusing.
                while (!next.empty() && next.front() == ' ') next.erase(next.begin());
                while (!next.empty() && next.back() == ' ') next.pop_back();
                if (!next.empty() && next != n.name) { n.name = next; dirty_ = true; }
            }
            f32 t = n.time;
            const f32 dur = clip_.duration > 0.0f ? clip_.duration : 1.0f;
            ImGui::SetNextItemWidth(180.0f * (ImGui::GetFontSize() / 16.0f));
            if (ImGui::SliderFloat("Time", &t, 0.0f, dur, "%.3f s")) {
                n.time = t;
                time_ = t;
                playing_ = false;
                dirty_ = true;
            }
            // DURATION: the ONE thing this editor could not author before item 7.1 -- turning a
            // plain instant notify into a notify STATE that opens at Time and stays open this long.
            // 0 is instant, exactly as an untouched notify always was; see AnimSystem.hpp's own
            // banner for what the runtime does with a non-zero value.
            f32 stateDur = notifyDurationAt(i);
            ImGui::SetNextItemWidth(180.0f * (ImGui::GetFontSize() / 16.0f));
            if (ImGui::SliderFloat("Duration", &stateDur, 0.0f, dur, "%.3f s")) {
                setNotifyDuration(i, stateDur);
                dirty_ = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled(stateDur > 0.0f ? "(state)" : "(instant)");
            if (ImGui::SmallButton("Delete")) {
                clip_.notifies.erase(clip_.notifies.begin() + static_cast<isize>(i));
                if (!clip_.notifyDurations.empty())
                    clip_.notifyDurations.erase(clip_.notifyDurations.begin() + static_cast<isize>(i));
                selectedNotify_ = -1;
                dirty_ = true;
                ImGui::Unindent();
                ImGui::PopID();
                break;   // the vector moved under this loop; next frame redraws it
            }
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
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

    // THE SELECTED CURVE, drawn over the same bar. Only the selected one: overlaying every curve
    // on a 20-pixel-high slider gives a scribble, and the one being edited is the one whose shape
    // an author is trying to see. Normalised to its own min and max, because a curve's range is
    // whatever the author chose and a fixed 0..1 would flatten most of them into a line.
    if (selectedCurve_ >= 0 && static_cast<usize>(selectedCurve_) < clip_.curves.size()) {
        const fmt::OcCurve& c = clip_.curves[static_cast<usize>(selectedCurve_)];
        if (c.times.size() >= 2 && c.values.size() == c.times.size()) {
            f32 lo = c.values[0], hi = c.values[0];
            for (const f32 v : c.values) { lo = v < lo ? v : lo; hi = v > hi ? v : hi; }
            const f32 span = (hi - lo) > 1e-6f ? (hi - lo) : 1.0f;
            const f32 top = p0.y + 2.0f, bot = p1.y - 2.0f;
            ImVec2 prev{};
            bool have = false;
            // SAMPLED ACROSS THE BAR rather than drawn key-to-key, so a STEP curve reads as steps
            // instead of as a straight line between its keys -- which is what it is not.
            const int steps = 96;
            for (int k = 0; k <= steps; ++k) {
                const f32 u = static_cast<f32>(k) / static_cast<f32>(steps);
                const f32 v = anim::sampleCurve(c, u * dur);
                const ImVec2 pt(p0.x + (p1.x - p0.x) * u, bot - (bot - top) * ((v - lo) / span));
                if (have) dl->AddLine(prev, pt, IM_COL32(150, 230, 160, 210), 1.5f);
                prev = pt;
                have = true;
            }
        }
    }

    // NOTIFIES, on the SAME bar as the keys and above them. Unreal gives them their own lane, and
    // that is the right answer once there are lanes to give; with one clip and no curves the thing
    // an author needs is the notify's position against the KEYS it is being placed relative to --
    // a footstep belongs on the frame the foot plants, and that frame is one of those yellow ticks.
    for (usize i = 0; i < clip_.notifies.size(); ++i) {
        const fmt::OcNotify& n = clip_.notifies[i];
        const f32 x = p0.x + (p1.x - p0.x) * (dur > 0.0f ? n.time / dur : 0.0f);
        const bool sel = (static_cast<int>(i) == selectedNotify_);
        const ImU32 col = sel ? IM_COL32(255, 220, 90, 255) : IM_COL32(120, 200, 255, 230);

        // A STATE'S WINDOW, drawn as a translucent band UNDER the marker -- visual-only, matching
        // the clamp AnimSystem itself applies at the loop seam, so an author dragging a window past
        // the end of the clip sees it stop exactly where the runtime will actually close it rather
        // than being told nothing until the surprise shows up in play.
        const f32 stateDur = notifyDurationAt(i);
        if (stateDur > 0.0f) {
            const f32 endT = dur > 0.0f ? std::min(n.time + stateDur, dur) : 0.0f;
            const f32 xEnd = p0.x + (p1.x - p0.x) * (dur > 0.0f ? endT / dur : 0.0f);
            dl->AddRectFilled(ImVec2(x, p0.y), ImVec2(xEnd, p1.y),
                              sel ? IM_COL32(255, 220, 90, 70) : IM_COL32(120, 200, 255, 55));
        }

        // A downward triangle sitting on the bar: a shape rather than another vertical line, so a
        // notify is never mistaken for the key ticks it sits among.
        const f32 top = p0.y;
        const ImVec2 tri[3] = {ImVec2(x - 5.0f, top), ImVec2(x + 5.0f, top), ImVec2(x, top + 9.0f)};
        dl->AddConvexPolyFilled(tri, 3, col);
        dl->AddLine(ImVec2(x, top), ImVec2(x, p1.y), col, sel ? 2.0f : 1.0f);
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

// One clip-flag bit as a checkbox, bound straight to clip_.flags. Not `#if AVER_WITH_IMGUI`-only
// itself since it is called only from draw(), which already is -- but it lives in its own function
// (rather than inline three times in draw()) so the bit-toggle logic is written once.
void AnimEditor::flagCheckbox(u8 bit, const char* label) {
#if AVER_WITH_IMGUI
    bool set = (clip_.flags & bit) != 0;
    if (ImGui::Checkbox(label, &set)) {
        if (set) clip_.flags = static_cast<u8>(clip_.flags | bit);
        else     clip_.flags = static_cast<u8>(clip_.flags & ~bit);
        dirty_ = true;
    }
#else
    (void)bit; (void)label;
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
        ImGui::TextDisabled("%.3f s   %zu track(s)   %s", clip_.duration, clip_.tracks.size(),
                            clip_.storage == fmt::OcAnimStorage::BakedUniform ? "baked" : "keyframed");
    else
        ImGui::TextDisabled("%zu bone(s)", skel_.bones.size());

    // Was static text reading ONE of the three flags (loop) with no way to change it or the other
    // two. All three round-trip through the format already; this is what was missing to author them.
    //
    // NOT LABELLED "Loop": drawTransport() already has an unrelated ImGui::Checkbox("Loop", &loop_)
    // for this TAB's own PREVIEW playback (loop_, a session-only bool, never saved). Reusing that
    // label here, in the same window's ID scope, would collide two different checkboxes onto one
    // ImGui id -- and would read as the same setting to an author even if it did not. "Clip loops"
    // names the AUTHORED, persisted bit this one actually writes (clip_.flags).
    if (isClip_) {
        flagCheckbox(fmt::kOcAnimLoop, "Clip loops");
        ImGui::SameLine();
        flagCheckbox(fmt::kOcAnimAdditiveBase, "Additive base");
        ImGui::SameLine();
        flagCheckbox(fmt::kOcAnimRootMotion, "Root motion");
    }

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
        ImGui::TextDisabled("SOCKETS");
        if (ImGui::BeginChild("sockets", ImVec2(0, h * 0.30f), false)) drawSockets();
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
        ImGui::TextDisabled("NOTIFIES");
        if (ImGui::BeginChild("notifies", ImVec2(0, h * 0.38f), false)) drawNotifies();
        ImGui::EndChild();
        ImGui::Separator();
        ImGui::TextDisabled("CURVES");
        if (ImGui::BeginChild("curves", ImVec2(0, h * 0.30f), false)) drawCurves();
        ImGui::EndChild();
        ImGui::Separator();
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
