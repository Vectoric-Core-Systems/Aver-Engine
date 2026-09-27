// The animation editor tab: clip header, transport, timeline, bone tree, an asset browser of every
// clip the rig can play, a track list, and a 3D preview of the skinned mesh.
//
// Bone boxes remain as an overlay toggle rather than the only view: they are still the right way to
// see a rig problem (which joint is rotating, where a chain is broken), while the mesh covers
// everything else. GPU skinning itself lives in modules/render.skin -- this file used to claim GPU
// skinning was impossible ("the RHI...cannot"), a claim commit 9fbe761 disproved 17 minutes after
// commit 1735709 landed this box-per-bone preview.
#include "AnimEditor.hpp"
#include "AnimEdit.hpp"
#include "EditorIcons.hpp"
#include "EditorKeybinds.hpp"
#include "EditorWidgets.hpp"
#include "PreviewChrome.hpp"
#include "SnapshotUndo.hpp"
#include "AnimCurveGeometry.hpp"

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

// Preview material resolution (buildPreview's mesh draw, below): a mesh's slot-0 surface name -> its
// .ocmat -> the domain graph it names (if any) and the pbr::MaterialHandle for its own factors and
// textures -- see resolvePreviewMaterial's own top comment for why a graph id alone used to leave
// the mesh white.
//
// Gated on AVER_MODULE_PBR alone, NOT nested inside AVER_WITH_IMGUI (unlike GraphEditor.cpp's
// similarly-shaped block): this file already includes ActorPreview.hpp/PreviewMeshCache.hpp
// unconditionally, so nothing below depends on ImGui. Nesting under AVER_WITH_IMGUI too would break
// module-matrix.ps1's no-ui row (AVER_MODULE_PBR still on) with an undeclared-identifier error, in
// code that has nothing to do with ImGui. Aver.Formats.Material/Aver.Render.PBR are only on
// Sandbox's include path when AVER_MODULE_PBR is on (sandbox/CMakeLists.txt), hence the guard.
#if AVER_MODULE_PBR
// MaterialResolve.hpp carries the runtime's own candidate-path order (see its own top comment).
#include "MaterialResolve.hpp"
#include "aver/formats/OcMat.hpp"
#include "aver/formats/OcGraph.hpp"
#include "aver/pbr/MaterialGraphRegistry.hpp"
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

// view/tracks split fraction (SplitPane, not ActorEditor's pixel-width convention -- see
// EditorWidgets.hpp). 0.62f matches the tab's pre-existing unpersisted default (was
// `avail.x * 0.62f`, recomputed every frame); only draggability and persistence are new.
// Bones/sockets/animations column to the left stays fixed width (see draw()).
constexpr f32 kDefaultViewFraction = 0.62f;
constexpr const char* kPrefViewSplit = "animEditor.viewSplit";

// Sequencer strip split, along the bottom of the tab -- moved down out of the vertical flow it used
// to sit in, between the header and the mesh/bones checkboxes (own pref key/default, separate from the
// view/tracks split above so dragging one is never read back as resizing the other). 0.32f leaves
// room for transport + scrub bar + notify lane + a few track lanes before drawTimeline()'s own
// scrollbar takes over.
constexpr f32 kDefaultSeqFraction = 0.32f;
constexpr const char* kPrefSeqSplit = "animEditor.seqSplit";
// Whether the strip is collapsed to its thin header; separate key so collapsing doesn't lose the
// fraction it reopens to.
constexpr const char* kPrefSeqCollapsed = "animEditor.seqCollapsed";

#if AVER_WITH_IMGUI
// Vertical counterpart to EditorWidgets.hpp's splitterHandle, for a pane stacked below another (the
// sequencer strip) rather than side-by-side: reads MouseDelta.y, sets the N-S resize cursor, no
// SameLine(). Kept local to this file rather than folded into the shared helper.
//
// `*heightPx` is the BOTTOM pane's height, so dragging up must give it MORE room: `-= MouseDelta.y`,
// the opposite sign from splitterHandle's `+=`.
bool verticalSplitterHandle(const char* id, f32 thickness, f32* heightPx, f32 avail, f32 minSelf,
                             f32 minOther, bool* released) {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const f32 w = ImGui::GetContentRegionAvail().x;
    ImGui::InvisibleButton(id, ImVec2(w > 8.0f ? w : 8.0f, thickness));

    const bool hot = ImGui::IsItemActive() || ImGui::IsItemHovered();
    if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

    bool moved = false;
    if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.y != 0.0f) {
        *heightPx -= ImGui::GetIO().MouseDelta.y;
        moved = true;
    }
    if (released) *released = ImGui::IsItemDeactivated();
    *heightPx = clampSplitWidth(*heightPx, avail, minSelf, minOther);

    if (hot) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const f32 y = at.y + thickness * 0.5f;
        dl->AddLine(ImVec2(at.x, y), ImVec2(at.x + (w > 8.0f ? w : 8.0f), y),
                    ImGui::GetColorU32(ImGuiCol_SeparatorActive), 2.0f);
    }
    return moved;
}
#endif

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

// The one render feature this editor owns: turns the UI-sampled pose into posed vertices once per
// frame, before the preview draws them.
//
// A feature rather than a draw()-side call, because dispatch needs an IRenderContext the UI has none
// of -- same shape as SkinnedScene's prePass/overlayPass pair: dispatch leaves the buffer in
// GeometryRead for the draw, and every frame must hand it back to Common before submit (D3D12 decays
// buffer state at end-of-command-list; next frame's barrier would otherwise claim a stale state).
//
// Registered BEFORE the preview so this frame's pose (not last frame's) is what's on screen: features
// get prePass in registration order, and ActorPreview renders the preview image in its own prePass too.
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
    // For the preview's stats overlay.
    u32 vertexCount() const { return vertexCount_; }
    u32 triangleCount() const { return triangleCount_; }
    // Mesh's slot-0 surface name (OcMeshData::materialSlots[0]), or empty. The reason bind() keeps
    // the decoded OcMeshData around -- see bind()'s own comment.
    const std::string& slot0Material() const { return slot0Material_; }

    // Binds a rig + mesh pair. Idempotent for the same pair, so the editor can call it every frame
    // without rebuilding GPU resources -- the clip (and rig) can change when the asset browser is
    // clicked.
    bool bind(rhi::IDevice& dev, const std::string& meshPath, u32 boneCount) {
        if (!ready_) return false;
        if (meshPath == meshPath_ && gpu_.valid() && boneCount == boneCount_) return true;

        if (gpu_.valid()) pass_.destroyMesh(gpu_);
        gpu_ = {};
        drawMesh_ = 0;
        vertexCount_ = 0;
        triangleCount_ = 0;
        meshPath_ = meshPath;
        boneCount_ = boneCount;
        // Cleared here, not only on success: a partial rebind failure (no skin streams, a truncated
        // file) must not leave a stale name from the previous mesh for buildPreview's cache to keep
        // resolving.
        slot0Material_.clear();
        if (meshPath.empty() || boneCount == 0) return false;

        fmt::OcMeshData md;
        std::string why;
        if (!fmt::loadOcMesh(meshPath, md, &why)) {
            AVER_WARN("[AnimEditor] {}", why);
            return false;
        }
        // Read regardless of hasSkin() below: harmless if unused (falls back to bone boxes), and
        // simpler than re-opening the file later just for this field.
        if (!md.materialSlots.empty()) slot0Material_ = md.materialSlots[0];
        if (!md.hasSkin()) {
            // Not an error and not silent: a mesh with no skin streams is ordinary; falls back to bone
            // boxes rather than a static T-pose that would look like a broken clip.
            AVER_INFO("[AnimEditor] {} has no skin streams; the preview stays on bone boxes", meshPath);
            return false;
        }

        // OcMeshData keeps positions/normals/uvs as flat arrays; the RHI wants one interleaved
        // MeshVertex stream. Built here (not via PreviewMeshCache, which resolves content-relative
        // paths -- this mesh is found by walking the skeleton's own directory) since the skinning pass
        // needs the decoded OcMeshData anyway, for its rest and bind streams.
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
        // Draw handle whose vertex buffer IS the skin target: the preview rasterises posed vertices
        // directly, no second copy.
        drawMesh_ = dev.createSkinTargetMesh(source, &verts);
        if (!drawMesh_ || !verts) { drawMesh_ = 0; return false; }
        if (!pass_.createMesh(md, boneCount, gpu_, verts)) { drawMesh_ = 0; return false; }

        // Bounds radius for frameAll, from the rest mesh -- posed mesh doesn't move enough to matter,
        // and per-frame GPU readback isn't available anyway.
        vertexCount_ = static_cast<u32>(vcount);
        triangleCount_ = static_cast<u32>(md.indices.size() / 3);
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
        // Unconditional, not "only if dispatched" (see SkinnedScene::overlayPass): skinTransition is
        // a no-op if already in state; a buffer left in GeometryRead at submit is a validation error
        // next frame.
        render::skinTransition(ctx, gpu_, rhi::ResourceState::Common);
    }

private:
    rhi::IDevice*           dev_ = nullptr;
    render::SkinningPass    pass_;
    render::SkinnedMeshGpu  gpu_;
    rhi::MeshHandle         drawMesh_ = 0;
    std::string             meshPath_;
    u32                     boneCount_ = 0;
    u32                     vertexCount_ = 0, triangleCount_ = 0;
    f32                     radius_ = 1.0f;
    bool                    ready_ = false;
    std::vector<Mat4>       staged_;
    std::string             slot0Material_;   // OcMeshData::materialSlots[0] of the bound mesh, or ""
};

AnimSkinFeature g_skin;
bool g_skinTried = false;

// Created and registered once, before the shared preview (ordering reason in AnimSkinFeature's
// comment). Shared rather than per-tab, matching sharedPreview(): two anim tabs already share one
// preview, so a second skinning target would have nothing to draw into.
// Keeps the device the feature was registered with, so teardown unregisters from the same one.
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

// Bone palette, minus one hue. computeBonePalette's golden-angle spread could land a chain on the
// same cyan buildPreview's socket markers use, which would misread as "an author placed this". So
// this reimplements boneColorForChain's arithmetic (calling it first would save nothing -- rejecting
// its answer still needs the same formula to pick a replacement) and pushes any hue landing inside the
// excluded band to the band's far edge. Band is widened past the socket hue itself (baseColor
// {0.18, 0.72, 0.95}: max=B, L=0.565 -> hue ~198 deg) because only the hue needs dodging -- the
// palette's lightness cycle (kBoneLightLow..kBoneLightHigh) still crosses the socket's lightness at
// nearby hues.
constexpr f32 kSocketHueDeg = 198.0f;
constexpr f32 kSocketHueHalfWidthDeg = 28.0f;

BoneColor boneColorAvoidingSocketCyan(u32 chainRootBoneIndex, u32 depthInChain) {
    f32 hue = std::fmod(static_cast<f32>(chainRootBoneIndex) * kBoneHueGoldenTurns, 1.0f) * 360.0f;
    const f32 lo = kSocketHueDeg - kSocketHueHalfWidthDeg;
    const f32 hi = kSocketHueDeg + kSocketHueHalfWidthDeg;
    if (hue >= lo && hue <= hi) hue = std::fmod(hue + (hi - lo), 360.0f);

    // Identical to boneColorForChain from here down -- see AnimEdit.hpp on the triangle-wave
    // lightness cycle.
    const u32 phase = depthInChain % (2 * kBoneLightPeriod);
    const u32 folded = phase <= kBoneLightPeriod ? phase : (2 * kBoneLightPeriod - phase);
    const f32 frac = static_cast<f32>(folded) / static_cast<f32>(kBoneLightPeriod);
    const f32 lightness = kBoneLightLow + (kBoneLightHigh - kBoneLightLow) * frac;
    return hslToBoneColor(hue, kBoneSaturation, lightness);
}

// .cpp-side equivalent of computeBonePalette: childCounts built once, shared across the walk. Only
// difference from the header version is the hue substitution above.
std::vector<BoneColor> computeAnimEditorBonePalette(const fmt::OcSkeleton& skeleton) {
    std::vector<u32> counts;
    boneChildCounts(skeleton, counts);
    std::vector<BoneColor> out(skeleton.bones.size());
    for (usize i = 0; i < skeleton.bones.size(); ++i) {
        u32 root = static_cast<u32>(i), depth = 0;
        boneChainRootAndDepth(skeleton, static_cast<u32>(i), counts, root, depth);
        out[i] = boneColorAvoidingSocketCyan(root, depth);
    }
    return out;
}

#if AVER_MODULE_PBR
// Everything resolvePreviewMaterial hands buildPreview: the graph id PreviewDraw::materialGraphId
// wants, the real pbr::MaterialHandle PreviewDraw::materialHandle wants (else the preview shades
// white), and a status sentence -- always set -- logged once (buildPreview's materialTried_ latch
// makes "once" true) and painted into the tab.
struct PreviewMaterialResolution {
    u32 graphId = 0;                 // pbr::materialGraphs() id, or 0 = no graph runs on top
    pbr::MaterialHandle handle = 0;  // the surface's own textures/factors, or 0 = stock identity set
    std::string status;              // one sentence, safe to print verbatim in the tab
    bool failed = false;             // a genuine problem -- see the no-GRAPHREF branch: false there despite graphId==0
};

// Turns a mesh's slot-0 surface NAME into everything the preview needs to shade with. Replaces the
// narrower resolvePreviewMaterialGraph, which answered only "which graph" and discarded the
// pbr::MaterialDesc loadOcmat() had already parsed -- which is why a material with no graph (most of
// them) still drew ActorPreview's white identity textures.
//
// `binariesDir` may be "" (see call site) -- resolveMaterialPath tolerates that, it just never
// matches the Binaries candidate.
//
// NOT GameContent::resolveMaterialGraph/materialForSurface (Runtime/src/GameContent.cpp): those are
// private to a class this editor has no instance of, and resolve differently besides -- the runtime
// only tries Binaries\Materials by a recorded name; the editor also accepts a hand-authored .ocmat
// under Content\Materials (MaterialResolve.hpp's three-candidate order), the common case for a rig
// just imported and not yet run through avermatc.
//
// Five ways this can come back unable to shade with anything real (before this function existed, only
// the last one said why): empty surface name, empty content root, no .ocmat found, an .ocmat that
// failed to parse, and a GRAPHREF naming a graph that won't load/compile. Each of these four failures
// fills `status` and logs once via AVER_WARN.
//
// An .ocmat with no GRAPHREF is NOT one of the four failures: most materials are exactly this
// (factors/textures, no graph), so `failed` stays false and `handle` is still the real material's --
// reporting the ordinary case as a problem would cry-wolf-mask the actual white-mesh bug.
//
// pbr::MaterialLibrary is a process-global singleton, so a handle for this surface may already exist
// (GameContent::materialForSurface, or another tab). This editor can't reach GameContent's own
// name->handle cache (content_ is a private SandboxApp field; no AssetEditor tab holds a SandboxApp
// reference), so it scans the library's live materials BY NAME instead (cheap: count()/at()/desc()
// only, rarely more than a few dozen live, and run once per attempt rather than per frame). A name
// match is a heuristic -- two unauthored .ocmat files take NAME from their file stem, so a collision
// needs two stems sharing one authored NAME. Nothing found means this loads and creates it itself, the
// same three-candidate parse GameContent::materialForSurface does, minus that class's per-instance
// cache.
PreviewMaterialResolution resolvePreviewMaterial(const std::string& binariesDir,
                                                  const std::string& contentDir,
                                                  const std::string& name) {
    PreviewMaterialResolution r;
    if (name.empty()) {
        r.status = "no material: the bound mesh's slot-0 surface has no name";
        r.failed = true;
        AVER_WARN("[AnimEditor] {}", r.status);
        return r;
    }
    if (contentDir.empty()) {
        r.status = "no material: no project content root is open to look '" + name + "' up in";
        r.failed = true;
        AVER_WARN("[AnimEditor] {}", r.status);
        return r;
    }
    const std::string matPath = resolveMaterialPath(binariesDir, contentDir, name);
    if (matPath.empty()) {
        r.status = "surface '" + name + "' has no .ocmat -- tried " +
                    (binariesDir.empty() ? std::string("<no Binaries dir; project not fully opened>")
                                          : binariesDir + "\\Materials\\" + name + ".ocmat") +
                    ", " + contentDir + "\\Materials\\" + name + ".ocmat, and " + contentDir + "\\" + name;
        r.failed = true;
        AVER_WARN("[AnimEditor] {}", r.status);
        return r;
    }

    fmt::OcMatExtras extras;
    pbr::MaterialDesc desc;
    std::string err;
    if (!fmt::loadOcmat(matPath, desc, &extras, &err)) {
        r.status = "material '" + name + "' at " + matPath + " would not parse: " + err;
        r.failed = true;
        AVER_WARN("[AnimEditor] {}", r.status);
        return r;
    }

    // Reuse first -- see top comment on why a name scan stands in for GameContent's cache.
    for (u32 i = 0, n = pbr::MaterialLibrary::get().count(); i < n && !r.handle; ++i) {
        const pbr::MaterialHandle h = pbr::MaterialLibrary::get().at(i);
        if (const pbr::MaterialDesc* live = pbr::MaterialLibrary::get().desc(h))
            if (live->name == desc.name) r.handle = h;
    }

    if (extras.graphRef.empty()) {
        // Ordinary material -- not one of the four failures (see top comment).
        if (!r.handle) r.handle = pbr::MaterialLibrary::get().create(desc);
        if (!r.handle) {
            r.failed = true;
            r.status = "material '" + name + "' parsed but MaterialLibrary is full";
            AVER_WARN("[AnimEditor] {}", r.status);
        } else {
            r.status = "shading with '" + desc.name + "' (factors and textures, no graph)";
        }
        return r;
    }

    // Content-relative, as GameContent::resolveMaterialGraph reads it -- see OcMatExtras::graphRef.
    std::string graphPath = contentDir + "\\" + extras.graphRef;
    for (char& c : graphPath) if (c == '/') c = '\\';

    // idOf() first, so an already-compiled graph is reused (ids are stable for the process -- see
    // MaterialGraphRegistry).
    u32 graphId = pbr::materialGraphs().idOf(graphPath);
    bool graphBroken = false;
    if (!graphId) {
        fmt::OcGraphData g;
        if (!fmt::loadOcgraph(graphPath, g, &err)) {
            graphBroken = true;
            r.status = "material '" + name + "' names graph '" + extras.graphRef +
                       "' but it did not load: " + err;
            AVER_WARN("[AnimEditor] {}", r.status);
        } else {
            // add() itself logs and returns 0 if the graph reads but doesn't compile (see its own
            // comment).
            graphId = pbr::materialGraphs().add(graphPath, g.name, g);
            if (!graphId) {
                graphBroken = true;
                r.status = "material '" + name + "' names graph '" + extras.graphRef +
                           "' but it did not compile; see the log above";
            }
        }
    }

    // A broken graph doesn't take the whole material down: graphId stays 0 (ActorPreview's
    // `default: break` arm) but factors/textures still reach the mesh through `handle` -- same rule as
    // GameContent::materialForSurface.
    desc.graphId = graphId;
    if (!r.handle) r.handle = pbr::MaterialLibrary::get().create(desc);
    r.graphId = graphId;
    r.failed = graphBroken || !r.handle;
    if (!r.handle) {
        r.status = "material '" + name + "' parsed but MaterialLibrary is full";
        AVER_WARN("[AnimEditor] {}", r.status);
    } else if (!graphBroken) {
        r.status = "shading with '" + desc.name + "' and graph '" + extras.graphRef + "'";
    }
    return r;
}
#endif

// One .ocanim or .ocskel, open.
class AnimEditor final : public AssetEditor {
public:
    AnimEditor(std::string path, fmt::OcAnimation clip, fmt::OcSkeleton skel, bool isClip,
                std::string skelPath = {})
        : path_(std::move(path)), skelPath_(std::move(skelPath)), clip_(std::move(clip)),
          skel_(std::move(skel)), isClip_(isClip) {}

    const std::string& path() const override { return path_; }
    std::string title() const override { return std::filesystem::path(path_).filename().string(); }

    // This editor could not write anything until now (it opened a clip, drew it -- that was the whole
    // contract). Notifies are the first thing an author can create here, so the tab needs the
    // dirty/save pair every other asset editor already has.
    bool dirty() const override { return dirty_; }
    bool save(std::string* why) override {
        // A skeleton has something to save now -- it did not when this override was written, when the
        // rig tab showed a read-only bone tree. Sockets are authored here, so a skeleton now has
        // something to save too.
        const bool ok = isClip_ ? fmt::saveOcAnim(path_, clip_, why)
                                : fmt::saveOcSkel(path_, skel_, why);
        if (!ok) return false;
        dirty_ = false;
        return true;
    }
    void draw(Engine& e) override;
    void onFileChanged() override { reload_ = true; }

    // Restores the view/tracks split to its default fraction and persists it (SplitPane, see
    // EditorWidgets.hpp). Only that boundary -- the fixed-width bones column and the internal
    // vertical splits are untouched (see draw()).
    void resetLayout() override;

    // Snapshot undo via SnapshotUndo<State> (SnapshotUndo.hpp) -- see pushUndo() for what each stack
    // holds and why there are two.
    void pushUndo();
    void undo();
    void redo();
    bool canUndo() const { return isClip_ ? clipHistory_.canUndo() : socketHistory_.canUndo(); }
    bool canRedo() const { return isClip_ ? clipHistory_.canRedo() : socketHistory_.canRedo(); }

private:
    // Snapshot undo state. Two stacks, not one: this tab edits a clip's notifies/notifyDurations/
    // curves/flags (.ocanim) or a rig's sockets (.ocskel) depending on isClip_, and they save() to
    // different files -- one shared stack could pop the wrong asset's snapshot. See pushUndo().
    //
    // Only fields this editor can write are snapshotted; skel_.bones stays out (read-only here, per
    // ParticleEditor.hpp's original reason: selectedBone_ picks one, nothing on this tab moves one).
    //
    // clip_.tracks/duration are included because key move/insert/delete and retime/scale/trim mutate
    // them -- otherwise an undo would restore notifies/curves/flags while leaving the edit on screen.
    // The original comment called copying tracks on every drag "a cost with nothing behind it"; true
    // until this editor grew the ability to touch tracks. The cost this reintroduces -- a full copy of
    // every sampled key on every drag start -- is the same trade GraphEditor's whole-graph snapshot
    // already makes.
    //
    // sampleRate too: scaleClipDuration (AnimEdit.hpp) writes it for a BakedUniform clip; without it an
    // undo could restore duration/track times with a baked rate that disagrees.
    struct ClipUndoState {
        std::vector<fmt::OcTrack> tracks;
        f32 duration = 0.0f;
        u16 sampleRate = 0;
        std::vector<fmt::OcNotify> notifies;
        std::vector<f32> notifyDurations;
        std::vector<fmt::OcCurve> curves;
        u8 flags = 0;
    };
    SnapshotUndo<ClipUndoState> clipHistory_;
    SnapshotUndo<std::vector<fmt::OcSocket>> socketHistory_;
    // Copies an undo/redo result onto clip_ or skel_.sockets, clamps the mode's selection index, and
    // re-syncs its text-edit buffer -- ParticleEditor's own syncEditBuffers() reason: else stale text
    // disagrees with the row until retyped.
    void applyClipUndoState(ClipUndoState&& s);
    void applySocketUndoState();

    void drawTransport();
    void drawTimeline();
    void drawTracks();
    void drawBones();
    void drawAssetBrowser();
    void drawNotifies();
    void drawSockets();
    void drawCurves();
    // The three whole-clip retime operations (AnimEdit.hpp's scaleClipDuration/shiftClipTime/
    // trimClip) as one toolbar. Split out of drawTracks() (drawn at that panel's top) since it edits
    // the clip, not a track.
    void drawClipRetime();
    // Track's sampled value at the playhead, per enabled channel -- reads pose_ rather than calling
    // trackSampleAt() again (see own comment).
    std::string trackValueLabel(const fmt::OcTrack& t) const;
    // Selected key's time/value editors plus Insert/Delete for the selected track (own comment on why
    // not folded into drawTracks()).
    void drawTrackKeyEditor();
    // Clamps selectedTrack_/selectedTrackKey_ against clip_.tracks -- needed outside undo/redo for a
    // trim (see own comment).
    void clampTrackKeySelection();
    // Item 7.2: the 2D curve canvas -- draggable keys and tangent handles. Split out of drawCurves()
    // as the one part with real geometry (see own header comment for the ImGui-free math and test
    // coverage).
    void drawCurveWidget(fmt::OcCurve& c);
    void buildPreview(Engine& e);
    void reloadIfNeeded();
    // Viewport's "F" gesture with a bone selected: frames that one bone rather than
    // ActorPreview::frameAll()'s whole-draw-list bounds -- PreviewCamera's addOrbit/addZoom/panPixels
    // don't cover this either (own .cpp comment on why frameAll() can't be reused here).
    void frameSelectedBone(render::preview::ActorPreview& preview, usize boneIndex);

    // Item 1.3: loop/additive-base/root-motion already persist through parseOcAnim/writeOcAnim
    // (clip_.flags is a plain u8); this editor just had nowhere to change one. One checkbox per bit,
    // toggling `bit` in clip_.flags and marking the tab dirty -- the existing Save path rewrites clip_
    // whole, so that's enough to pick it up.
    void flagCheckbox(u8 bit, const char* label);

    // Notify state durations. clip_.notifyDurations stays empty until the first is authored (see
    // OcAnimation::notifyDurations); these two are the only places touching it, kept either empty or
    // exactly parallel to clip_.notifies -- never a third shape writeOcAnim would refuse.
    f32 notifyDurationAt(usize i) const {
        return i < clip_.notifyDurations.size() ? clip_.notifyDurations[i] : 0.0f;
    }
    void setNotifyDuration(usize i, f32 seconds) {
        if (clip_.notifyDurations.size() != clip_.notifies.size())
            clip_.notifyDurations.assign(clip_.notifies.size(), 0.0f);
        if (i < clip_.notifyDurations.size()) clip_.notifyDurations[i] = seconds;
    }

    // The asset browser (Persona's name and shape for it too): every clip this skeleton can play,
    // listed beside the preview, one click to watch. The FirstPerson project ships 32 clips against
    // one 7-bone skeleton; needed because comparing clips (walk vs run, two melee swings) used to mean
    // closing the tab and reopening another file each time.
    struct ClipEntry {
        std::string path;
        std::string name;      // the file stem, which is what the tab shows
        std::string display;   // the same with the rig prefix removed -- see scanClips
        f32  duration = 0.0f;
        u32  tracks = 0;
        // A clip whose highest bone index exceeds this skeleton's bone count can't be sampled --
        // sampling would read off the end of the pose. Computed once at scan time, shown as a disabled
        // row rather than hidden.
        bool compatible = true;
    };
    std::vector<ClipEntry> clips_;
    bool clipsScanned_ = false;
    void scanClips();
    void openClip(const std::string& path);

    std::string path_;
    // Where the skeleton came from -- findSkeleton()'s result, previously discarded once bones were
    // loaded; kept so the asset browser can strip the shared <skeleton>_ prefix off clip names.
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
    // Curve widget drag state, set on mouse-down and held for the drag (press-time-hit-test-then-hold,
    // same shape as GraphEditor.cpp's DragMode) so a fast movement mid-drag doesn't drop it.
    CurveHitKind curveDragKind_ = CurveHitKind::None;
    usize curveDragKeyIndex_ = 0;

    // Track/key selection, shared between drawTimeline()'s lanes and drawTracks()'s detail panel: one
    // selection read/written from both, so clicking a key and typing its time in the table edit the
    // same key.
    int   selectedTrack_ = -1;                      // index into clip_.tracks, or -1 for none
    usize selectedTrackKey_ = kInvalidKeyIndex;      // index into that track's times/values, or none
    // Same press-time-hit-test-then-hold shape as curveDragKind_, collapsed to a bool: a track lane
    // has only one draggable thing (the key), never a tangent handle (see AnimEdit.hpp).
    bool  trackKeyDragging_ = false;

    // Retime toolbar state (drawClipRetime()): session-only, never saved/undone -- holds what the
    // author typed, not a clip fact, so Undo after Apply reverts the clip, not these boxes.
    f32 retimeScaleFactor_ = 1.0f;
    f32 retimeShiftSeconds_ = 0.0f;
    f32 retimeTrimStart_ = 0.0f;
    f32 retimeTrimEnd_ = 0.0f;

    f32 time_ = 0.0f;
    f32 speed_ = 1.0f;
    bool playing_ = true;
    bool loop_ = true;
    int selectedBone_ = -1;
    f64 lastClock_ = 0.0;

    anim::Pose pose_;
    std::vector<Mat4> model_;
    std::vector<Mat4> skin_;      // poseToSkinning output, handed to the GPU each frame
    // Bone boxes are an overlay now, shown by default alongside a mesh (see buildPreview on why
    // "mesh present -> bones off" was wrong). Bones/Sockets are Show-dropdown toggles (PreviewChrome.hpp).
    bool showBones_ = true;
    bool showSockets_ = true;
    bool showMesh_ = true;
    // Preview toolbar state, reasserted onto the shared ActorPreview every frame (see draw()).
    render::preview::PreviewViewMode viewMode_ = render::preview::PreviewViewMode::Lit;
    render::preview::PreviewShowFlags showFlags_{};
    bool skinBound_ = false;
    std::string meshPath_;         // <rig>.ocmesh beside the skeleton, if there is one
    bool framed_ = false;
    u32 pendingW_ = 0, pendingH_ = 0;
    f64 resizeDue_ = 0.0;
    // One colour per bone (AnimEdit.hpp's palette minus the socket-cyan band, see
    // boneColorAvoidingSocketCyan), rebuilt only on skeleton size change. Size 0 means "not built yet"
    // (buildPreview's check); reloadIfNeeded() clears it on a reload.
    std::vector<BoneColor> bonePalette_;
    // Preview mesh's material, resolved from its slot-0 surface name and cached (see buildPreview for
    // the per-frame cost this avoids). Plain fields, not gated on AVER_MODULE_PBR -- matching
    // GraphEditor's materialPreviewGraphId_/materialPreviewDirtyMark_ (GraphEditor.hpp), only the
    // filling code needs the guard.
    u32 materialGraphId_ = 0;        // pbr::materialGraphs() id, or 0 = draw the stock pale colour
    // Material's own textures/factors, orthogonal to materialGraphId_: that field alone selects a
    // graph, but the key-light-plus-fill and stock shaders both still read ActorPreview's white
    // identity textures until a real pbr::MaterialHandle says otherwise (see PreviewDraw::materialHandle).
    u32 materialHandle_ = 0;         // a pbr::MaterialHandle, or 0 = the preview's stock identity textures
    // True until a content root has been seen, so the first resolution that can succeed re-arms the
    // latch rather than inheriting a rootless verdict.
    bool materialRootPending_ = true;
    std::string materialTriedFor_;   // the slot-0 name materialGraphId_/materialHandle_ were resolved for
    bool materialTried_ = false;     // has ANY attempt (success or failure) been made yet
    // What the last attempt found, in one sentence -- always non-empty once materialTried_ is true,
    // safe to print verbatim; draw() paints it into the preview panel so a white/grey mesh is never a
    // silent mystery (4 of resolvePreviewMaterial's 5 come-back-with-nothing paths used to have no
    // Output Log line at all).
    std::string materialStatus_;
    // Whether materialStatus_ is a genuine problem (drawn as a warning) vs an ordinary outcome (a
    // no-graph material is not a problem -- see resolvePreviewMaterial).
    bool materialFailed_ = false;

    // View/tracks divider, a plain SplitPane (EditorWidgets.hpp), not gated on AVER_WITH_IMGUI so the
    // fields keep compiling with no ImGui even though only draw()/resetLayout() touch it.
    SplitPane split_;

    // Sequencer strip (drawTransport() + drawTimeline()), a pane along the bottom (see draw()).
    // Separate SplitPane from split_: that one divides bones/view/tracks left-to-right, this divides
    // the whole row from the strip top-to-bottom; the two must never share a persisted fraction.
    // resetLayout() resets both.
    SplitPane seqSplit_;
    // Read once at construction (a bool has no unused value to overload as "not loaded yet" the way
    // SplitPane's negative-fraction sentinel does); the toggle writes the pref back directly.
    bool seqCollapsed_ = prefBool(kPrefSeqCollapsed, false);
};

// Pushes the current state of whichever asset isClip_ says is being edited (see ClipUndoState in the
// header for the two shapes and why two stacks, not one). Every edit site calls this immediately
// before the mutation it guards ("push, then apply").
//
// Dispatches on isClip_ rather than taking a mode argument: every call site already only runs while
// its own mode is active (drawNotifies()/drawCurves()/flag checkboxes need isClip_; drawSockets()'
// editable half needs !isClip_), so the stack picked always agrees with the edit being recorded.
void AnimEditor::pushUndo() {
    if (isClip_) clipHistory_.push(ClipUndoState{clip_.tracks, clip_.duration, clip_.sampleRate,
                                                  clip_.notifies, clip_.notifyDurations, clip_.curves,
                                                  clip_.flags});
    else         socketHistory_.push(skel_.sockets);
}

void AnimEditor::applyClipUndoState(ClipUndoState&& s) {
    clip_.tracks = std::move(s.tracks);
    clip_.duration = s.duration;
    clip_.sampleRate = s.sampleRate;
    clip_.notifies = std::move(s.notifies);
    clip_.notifyDurations = std::move(s.notifyDurations);
    clip_.curves = std::move(s.curves);
    clip_.flags = s.flags;
    // The selections are INDICES, and undo/redo can shrink either array under them.
    if (selectedNotify_ >= static_cast<int>(clip_.notifies.size())) selectedNotify_ = -1;
    else if (selectedNotify_ >= 0)
        std::snprintf(notifyNameBuf_, sizeof notifyNameBuf_, "%s",
                      clip_.notifies[static_cast<usize>(selectedNotify_)].name.c_str());
    if (selectedCurve_ >= static_cast<int>(clip_.curves.size())) selectedCurve_ = -1;
    else if (selectedCurve_ >= 0)
        std::snprintf(curveNameBuf_, sizeof curveNameBuf_, "%s",
                      clip_.curves[static_cast<usize>(selectedCurve_)].name.c_str());
    // Same rule for track/key selection: `tracks` can change length across Undo/Redo;
    // clampTrackKeySelection() is the one place (shared with drawClipRetime()'s trim, the only live
    // edit that can also shrink a track's key count) that re-derives whether
    // selectedTrack_/selectedTrackKey_ still point at something real.
    clampTrackKeySelection();
}

void AnimEditor::applySocketUndoState() {
    if (selectedSocket_ >= static_cast<int>(skel_.sockets.size())) selectedSocket_ = -1;
    else if (selectedSocket_ >= 0)
        std::snprintf(socketNameBuf_, sizeof socketNameBuf_, "%s",
                      skel_.sockets[static_cast<usize>(selectedSocket_)].name.c_str());
}

void AnimEditor::undo() {
    if (isClip_) {
        ClipUndoState s{clip_.tracks, clip_.duration, clip_.sampleRate,
                        clip_.notifies, clip_.notifyDurations, clip_.curves, clip_.flags};
        if (!clipHistory_.undo(s)) return;
        applyClipUndoState(std::move(s));
    } else {
        if (!socketHistory_.undo(skel_.sockets)) return;
        applySocketUndoState();
    }
    dirty_ = true;
}

void AnimEditor::redo() {
    if (isClip_) {
        ClipUndoState s{clip_.tracks, clip_.duration, clip_.sampleRate,
                        clip_.notifies, clip_.notifyDurations, clip_.curves, clip_.flags};
        if (!clipHistory_.redo(s)) return;
        applyClipUndoState(std::move(s));
    } else {
        if (!socketHistory_.redo(skel_.sockets)) return;
        applySocketUndoState();
    }
    dirty_ = true;
}

void AnimEditor::reloadIfNeeded() {
    if (!reload_) return;
    reload_ = false;
    std::string why;
    if (isClip_) {
        fmt::OcAnimation c;
        // A reload replaces clip_ whole, so old undo entries would restore data that no longer
        // belongs on screen -- cleared for the same reason openClip() clears it.
        if (fmt::loadOcAnim(path_, c, &why)) { clip_ = std::move(c); clipHistory_.clear(); }
        else AVER_WARN("[AnimEditor] {}", why);
    } else {
        fmt::OcSkeleton s;
        // bonePalette_ cleared here, not left for buildPreview's size check: a reload keeping the same
        // bone count would otherwise pass that check and keep colours from the previous hierarchy.
        if (fmt::loadOcSkel(path_, s, &why)) { skel_ = std::move(s); socketHistory_.clear(); bonePalette_.clear(); }
        else AVER_WARN("[AnimEditor] {}", why);
    }
}

// Every .ocanim beside this asset, with the two facts the list shows and the one that decides
// whether a row is clickable. Scanned once, not per frame -- 32 clips is 32 file reads.
void AnimEditor::scanClips() {
    clipsScanned_ = true;
    clips_.clear();
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::path(path_).parent_path();
    if (dir.empty()) return;

    const u32 boneCount = static_cast<u32>(skel_.bones.size());
    // Rig's own stem, for the prefix strip below; a project not following <skeleton>_<action> just
    // keeps its full names.
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
        // Display name drops the rig prefix (clips named <skeleton>_<action> by convention, same as
        // findSkeleton() relies on) -- every row began "character_" and the truncated part was the
        // one that told them apart. Full name still shows in the tooltip and tab title.
        entry.display = entry.name;
        if (!rigStem.empty() && entry.display.size() > rigStem.size() + 1 &&
            entry.display.compare(0, rigStem.size(), rigStem) == 0 &&
            entry.display[rigStem.size()] == '_') {
            entry.display = entry.display.substr(rigStem.size() + 1);
        }
        entry.duration = c.duration;
        entry.tracks = static_cast<u32>(c.tracks.size());
        // Compatibility by bone index, not skeletonRef: the ref is just a filename hint the importer
        // wrote and nothing at runtime reads (see findSkeleton); bone indices are what sampling
        // actually uses.
        for (const fmt::OcTrack& t : c.tracks) {
            if (t.boneIndex >= boneCount) { entry.compatible = false; break; }
        }
        clips_.push_back(std::move(entry));
    }
    std::sort(clips_.begin(), clips_.end(),
              [](const ClipEntry& a, const ClipEntry& b) { return a.name < b.name; });
}

// Swaps which clip this tab is previewing, in place (not a second tab, matching what Persona does)
// -- the browser's point is comparison, and a new tab per clip would put the thing compared behind
// the thing it's compared to. The path changes with it, so the tab title and a later reload name the
// right clip.
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
    // A different clip, not an edit: old clipHistory_ entries describe the replaced clip, and an Undo
    // past this point would restore its notifies/curves/flags onto the new one. Same reason pushUndo()
    // never shares a stack between the two assets this tab can open; a same-kind swap gets the same
    // treatment.
    clipHistory_.clear();
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
    // Before sharedPreview, so the skinning dispatch registers ahead of the preview render and the
    // pose on screen is this frame's (see AnimSkinFeature).
    AnimSkinFeature* skin = sharedSkin(e);
    render::preview::ActorPreview* preview = sharedPreview(e);
    if (!preview || !e.device()) return;

    // Mesh found by convention beside the skeleton: character.ocskel -> character.ocmesh, the same
    // <stem> the clip names use.
    if (meshPath_.empty() && !skelPath_.empty()) {
        std::filesystem::path guess = std::filesystem::path(skelPath_);
        guess.replace_extension(".ocmesh");
        std::error_code ec;
        if (std::filesystem::exists(guess, ec)) meshPath_ = guess.string();
    }
    if (skin && !skinBound_) {
        skinBound_ = true;   // tried once; a failure falls back to bones and does not retry per frame
        showMesh_ = skin->bind(*e.device(), meshPath_, static_cast<u32>(skel_.bones.size()));
        // Was `showBones_ = !showMesh_`; kept on now because the overlay is how an author tells which
        // bone a track belongs to, spots a broken chain, and -- now every chain has its own colour --
        // reads the rig's structure at a glance, none of which the skinned surface shows alone. A rig
        // with a mesh now defaults to showing both, same as a rig with none always has. "Mesh"/"Bones"
        // toggles in draw() still let either be turned off.
        showBones_ = true;
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
    // Same pose, two spaces: poseToModel gives joint transforms for bones, poseToSkinning gives
    // bind-relative matrices for vertices -- both from one sample, so boxes and mesh never disagree.
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

    // Mesh first, at identity: skinning already placed every vertex, so a world transform here would
    // move it twice.
    if (skin && showMesh_ && skin->drawMesh()) {
        render::preview::PreviewDraw d;
        d.mesh = skin->drawMesh();
        d.boundsRadius = skin->boundsRadius();
        d.roughness = 0.62f;
        // Stock pale fallback, set unconditionally: a mesh with no slot-0 name or an uncompilable
        // graph must still draw (materialGraphId stays 0, ActorPreview's key-light-plus-fill shader
        // uses this colour) rather than vanish and look like a missing asset -- same reasoning
        // MaterialGraphRegistry::add uses in returning 0 for a graph that fails to compile, rather
        // than failing the whole material.
        d.baseColor[0] = 0.78f; d.baseColor[1] = 0.76f; d.baseColor[2] = 0.72f;
        const Mat4 id = Mat4::identity();
        std::memcpy(d.world, &id.m[0][0], sizeof d.world);
#if AVER_MODULE_PBR
        // Resolved once per surface name, not once per frame: resolvePreviewMaterial does at least one
        // filesystem probe (resolveMaterialPath) and, on a cold name, parses the .ocmat/graph and scans
        // MaterialLibrary -- materialTried_ latches even a failed lookup so a name that never resolves
        // isn't retried 60x/sec. Re-armed only when slot0 actually changes (in practice: once per tab,
        // since skinBound_ latches).
        //
        // A missing content root is NOT a failed lookup -- latching it as one was a real bug that
        // survived a build and a run: a tab opened by `--open-asset` draws before
        // setAnimEditorContentRoot (applyProject, SandboxProject.cpp) sets a root, so the latch would
        // record "does not resolve" from a lookup that had nowhere to look, and the mesh stayed white
        // with the root sitting right there a frame later.
        //
        // So the latch distinguishes terminal (no .ocmat, parse error, uncompilable graph -- about the
        // material, never retried) from transient (empty g_contentRoot -- about editor state, worth
        // asking again).
        const std::string& slot0 = skin->slot0Material();
        const bool haveRoot = !g_contentRoot.empty();
        if (haveRoot && (!materialTried_ || materialTriedFor_ != slot0 || materialRootPending_)) {
            materialTriedFor_ = slot0;
            materialTried_ = true;
            materialRootPending_ = false;
            // Binaries dir, derived rather than plumbed in: this tab only ever receives a content root
            // (setAnimEditorContentRoot, called from applyProject in SandboxProject.cpp).
            // OcProject::contentDir()/::binariesDir() are both `dir + "\" + ...` one segment apart
            // (OcProject.hpp), so the content root's parent recovers `dir` when contentRoot (default
            // "Content") is a single segment (true for every project here). A nested contentRoot
            // would just make the Binaries\Materials candidate never match -- resolveMaterialPath
            // already tolerates that by falling through, not a crash or a wrong material.
            const std::string binariesDir =
                std::filesystem::path(g_contentRoot).parent_path().string() + "\\Binaries";
            const PreviewMaterialResolution res = resolvePreviewMaterial(binariesDir, g_contentRoot, slot0);
            materialGraphId_ = res.graphId;
            materialHandle_ = res.handle;
            materialStatus_ = res.status;
            materialFailed_ = res.failed;
        }
        d.materialGraphId = materialGraphId_;
        // A non-zero handle makes averStockAuthored (ActorPreview.cpp) sample this material's own
        // base-colour/normal/roughness textures and factors instead of the preview's white/flat/
        // full-rough identity set -- see PreviewDraw::materialHandle and ActorPreview.cpp's prePass
        // for how.
        d.materialHandle = materialHandle_;
#endif
        draws.push_back(d);
    }

    // Rebuilt only on a skeleton size change (a genuine reload, see reloadIfNeeded()), not every
    // frame -- computeAnimEditorBonePalette is O(bone count) total (childCounts built once, shared
    // across the walk), so this guard is about not repeating the walk 60x/sec, not about the walk
    // being expensive.
    if (bonePalette_.size() != skel_.bones.size()) bonePalette_ = computeAnimEditorBonePalette(skel_);

    for (usize i = 0; showBones_ && i < model_.size(); ++i) {
        const Vec3 here{model_[i].m[3][0], model_[i].m[3][1], model_[i].m[3][2]};
        const i32 parent = skel_.bones[i].parent;
        render::preview::PreviewDraw d;
        d.mesh = cube;
        d.boundsRadius = radius;
        d.roughness = 0.55f;
        d.selected = static_cast<int>(i) == selectedBone_;
        // Every chain gets its own hue (computeAnimEditorBonePalette above), so an author can tell a
        // limb apart from its neighbour without reading a bone name. Replaces the old "root is pale"
        // special case: a true root is chain-root-of-itself at depth 0, which already renders at the
        // palette's own darkest step (kBoneLightLow), so several disconnected roots no longer all
        // paint the same washed-out grey.
        if (i < bonePalette_.size()) {
            const BoneColor& c = bonePalette_[i];
            d.baseColor[0] = c.r; d.baseColor[1] = c.g; d.baseColor[2] = c.b;
        }
        // Selection must still win against a now-colourful rig: "palette colour, brightened" isn't
        // enough since the golden-angle spread reserves no hue and some chain could land near it (same
        // issue as boneColorAvoidingSocketCyan's cyan dodge). What the palette can't produce, by
        // construction, is a lightness above kBoneLightHigh (0.64) at any hue or depth -- so a paler
        // highlight always reads as brighter than any rig colour, regardless of which bone or chain is
        // selected. Kept amber-ish rather than pure white, matching the "selected" colour this file
        // uses for a socket.
        if (d.selected) { d.baseColor[0] = 0.98f; d.baseColor[1] = 0.92f; d.baseColor[2] = 0.55f; }

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

    // Sockets draw whether or not bones do: a socket is placed by eye, and hiding it behind "Bones"
    // would hide it exactly when the mesh is on and placement matters. showSockets_ is independent of
    // showBones_.
    for (usize i = 0; showSockets_ && i < skel_.sockets.size(); ++i) {
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

    // Framed once via the preview's own frameAll (scales bounds radius by world scale,
    // ActorPreview.cpp:267) rather than by hand over joint positions, which would ignore box size and
    // put the camera inside the rig.
    if (!framed_) {
        framed_ = true;
        preview->frameAll();
        // Back off: a bone box is non-uniform (thin across, long along), so frameAll's radius pick is
        // the thin one and lands the camera inside the rig. Constant margin since the orbit is under
        // the mouse anyway.
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
// Added at the playhead, not a typed time: placing an event is something an author does by
// scrubbing to the frame and saying "here". Still editable afterwards.
// The socket list: add on the selected bone, rename, retarget, nudge the offset, delete.
//
// Added on the selected bone, not a second bone picker: the bone tree is already open on the left
// with one highlighted, and choosing where a socket goes IS choosing a bone -- a socket created
// against nothing would need a bone chosen before it meant anything, a second decision for no gain.
// The curve list: what each one reads at the playhead, and its keys.
//
// The value at the playhead is the headline, not the key list: a curve is a number that varies, and
// the question is "what does it read here" -- answered by scrubbing and reading, not by
// interpolating three keyframes in your head.
//
// Item 7.2: a real 2D curve widget, not numbers with a shape drawn over the timeline bar (the format
// stored no tangents until CTAN, see OcCurve::inTangents/outTangents). Canvas is drawCurveWidget,
// below; numeric key rows stay underneath for precise entry, same as Unreal's Curve Editor.
void AnimEditor::drawCurves() {
#if AVER_WITH_IMGUI
    if (!isClip_) { ImGui::TextDisabled("a skeleton has no curves"); return; }

    if (ImGui::SmallButton("Add curve")) {
        pushUndo();
        fmt::OcCurve c;
        // Made unique on creation, same reason as a socket: curve() returns the FIRST match, so a
        // duplicate name leaves the loser unreadable by name.
        std::string name = "NewCurve";
        for (int n = 1; clip_.curve(name) != nullptr; ++n) name = "NewCurve" + std::to_string(n);
        c.name = name;
        // One key at the playhead, not none: an empty curve reads the caller's fallback everywhere,
        // indistinguishable from a curve that isn't there.
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
                } else if (!next.empty() && next != c.name) { pushUndo(); c.name = next; dirty_ = true; }
            }

            // CubicSpline offered now that a curve has somewhere to keep tangents (CTAN). Picking it
            // the first time gives every key a flat (zero) tangent pair -- safe unconditionally since
            // a zero-filled array and an absent one are indistinguishable on disk until a handle moves
            // (see OcCurve::inTangents).
            int mode = c.interp == fmt::OcInterp::Step ? 1 : (c.interp == fmt::OcInterp::CubicSpline ? 2 : 0);
            ImGui::SetNextItemWidth(w);
            if (ImGui::Combo("Interp", &mode, "Linear\0Step\0CubicSpline\0")) {
                pushUndo();
                c.interp = mode == 1 ? fmt::OcInterp::Step
                         : mode == 2 ? fmt::OcInterp::CubicSpline
                                     : fmt::OcInterp::Linear;
                if (c.interp == fmt::OcInterp::CubicSpline &&
                    (c.inTangents.size() != c.times.size() || c.outTangents.size() != c.times.size())) {
                    c.inTangents.assign(c.times.size(), 0.0f);
                    c.outTangents.assign(c.times.size(), 0.0f);
                }
                dirty_ = true;
            }

            // Whether this curve carries tangents at all, decided once per frame so "add key"/
            // "delete key" and the canvas all agree without re-deriving the same comparison.
            const bool hasTangents = c.inTangents.size() == c.times.size() && c.outTangents.size() == c.times.size();

            if (ImGui::SmallButton("Add key at playhead")) {
                pushUndo();
                // Inserted in time order: the sampler binary-searches `times`, so an out-of-order key
                // makes every lookup past it wrong, not just looking odd.
                usize at = 0;
                while (at < c.times.size() && c.times[at] < time_) ++at;
                c.times.insert(c.times.begin() + static_cast<isize>(at), time_);
                c.values.insert(c.values.begin() + static_cast<isize>(at), anim::sampleCurve(c, time_));
                // Kept parallel: a new key starts flat if tangent data exists at all (arrays must stay
                // exactly times.size() long or not exist -- see OcCurve::inTangents).
                if (hasTangents) {
                    c.inTangents.insert(c.inTangents.begin() + static_cast<isize>(at), 0.0f);
                    c.outTangents.insert(c.outTangents.begin() + static_cast<isize>(at), 0.0f);
                }
                dirty_ = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete curve")) {
                pushUndo();
                clip_.curves.erase(clip_.curves.begin() + static_cast<isize>(i));
                selectedCurve_ = -1;
                dirty_ = true;
                ImGui::Unindent();
                ImGui::PopID();
                break;
            }

            // The canvas -- see drawCurveWidget's header comment for what it draws/drags, and that the
            // drag itself is visual-only, untested.
            drawCurveWidget(c);

            for (usize k = 0; k < c.times.size() && k < c.values.size(); ++k) {
                ImGui::PushID(static_cast<int>(k));
                f32 kv[2] = {c.times[k], c.values[k]};
                ImGui::SetNextItemWidth(w * 1.4f);
                const bool keyChanged = ImGui::DragFloat2("##key", kv, 0.01f);
                // One drag is one undo entry: pushed when the drag activates, before this frame's
                // delta is applied, not once per frame while held.
                if (ImGui::IsItemActivated()) pushUndo();
                if (keyChanged) {
                    // Time clamped between neighbours rather than sorted after: dragging can never
                    // reorder the array under the binary search. Past a neighbour it holds, not swaps
                    // -- delete and re-add to move a key past another.
                    const f32 lo = k > 0 ? c.times[k - 1] : -1e9f;
                    const f32 hi = (k + 1) < c.times.size() ? c.times[k + 1] : 1e9f;
                    c.times[k] = kv[0] < lo ? lo : (kv[0] > hi ? hi : kv[0]);
                    c.values[k] = kv[1];
                    dirty_ = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    pushUndo();
                    // Kept parallel on delete too, same reason as the insert above.
                    if (hasTangents) {
                        c.inTangents.erase(c.inTangents.begin() + static_cast<isize>(k));
                        c.outTangents.erase(c.outTangents.begin() + static_cast<isize>(k));
                    }
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

// The 2D curve widget: a canvas with draggable keys and tangent handles, same hand-rolled ImGui style
// as GraphEditor.cpp's canvas (one InvisibleButton, hand hit-testing, no vendored widget). All
// layout/mapping/hit-testing go through AnimCurveGeometry.hpp's free functions, which are ImGui-free
// and Engine-free so AnimCurveGeometryTest can reach them with no window and no GPU.
//
// The drag itself is visual-only and untested: reading ImGui's mouse state and writing into clip_ via
// screenToCurve/tangentSlopeFromHandle needs a real mouse over a real window, unlike
// GraphEditorLoadSaveTest's headless drive of GraphEditor.cpp. AnimCurveGeometryTest covers everything
// this function calls into (the screen<->curve mapping and hit resolution) -- every part arithmetic
// can answer.
void AnimEditor::drawCurveWidget(fmt::OcCurve& c) {
#if AVER_WITH_IMGUI
    const f32 uiScale = ImGui::GetFontSize() / 16.0f;
    const f32 height = 160.0f * uiScale;
    const f32 width = std::max(ImGui::GetContentRegionAvail().x, 40.0f);

    ImGui::InvisibleButton("##curveCanvas", ImVec2(width, height));
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    const bool canvasActive = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(24, 24, 28, 255));
    dl->AddRect(p0, p1, IM_COL32(80, 80, 90, 255));

    if (c.times.empty() || c.values.size() != c.times.size()) {
        ImGui::TextDisabled("No keys to show. Add one at the playhead above.");
        curveDragKind_ = CurveHitKind::None;
        return;
    }

    // View: time spans the whole clip, matching the notify/track bar above. Value spans this curve's
    // own authored min/max, padded 10% each way so an edge point stays fully visible and grabbable
    // (same normalise-to-own-range choice as drawTimeline's older curve overlay, now with the padding
    // a real hit target needs that its one-pixel-tall line never did).
    f32 vLo = c.values[0], vHi = c.values[0];
    for (const f32 v : c.values) { vLo = std::min(vLo, v); vHi = std::max(vHi, v); }
    if (vHi - vLo < 1e-4f) { vHi += 0.5f; vLo -= 0.5f; }   // a flat curve still gets a visible band
    const f32 pad = (vHi - vLo) * 0.1f;
    vLo -= pad; vHi += pad;
    const f32 dur = clip_.duration > 0.0f ? clip_.duration : 1.0f;

    CurveView view;
    view.rectMin = Vec2{p0.x, p0.y};
    view.rectMax = Vec2{p1.x, p1.y};
    view.tMin = 0.0f; view.tMax = dur;
    view.vMin = vLo;  view.vMax = vHi;

    // GRID: a zero line, when zero sits inside the visible band, so a value's sign reads at a glance.
    if (vLo < 0.0f && vHi > 0.0f) {
        const Vec2 z = curveToScreen(view, 0.0f, 0.0f);
        dl->AddLine(ImVec2(p0.x, z.y), ImVec2(p1.x, z.y), IM_COL32(70, 70, 78, 255));
    }

    // THE SHAPE, sampled across the whole width through the REAL sampler (anim::sampleCurve) --
    // exactly what plays back, tangents and all, not a reconstruction of it.
    {
        const int steps = 128;
        ImVec2 prev{}; bool have = false;
        for (int s = 0; s <= steps; ++s) {
            const f32 u = static_cast<f32>(s) / static_cast<f32>(steps);
            const Vec2 sp = curveToScreen(view, u * dur, anim::sampleCurve(c, u * dur));
            const ImVec2 pt(sp.x, sp.y);
            if (have) dl->AddLine(prev, pt, IM_COL32(150, 230, 160, 230), 1.5f);
            prev = pt;
            have = true;
        }
    }

    // THE PLAYHEAD, so scrubbing the transport above visibly moves against this canvas too.
    {
        const Vec2 ph = curveToScreen(view, time_, vHi);
        dl->AddLine(ImVec2(ph.x, p0.y), ImVec2(ph.x, p1.y), IM_COL32(255, 210, 90, 150), 1.0f);
    }

    // KEYS AND TANGENT HANDLES. handleSeconds is a FIXED FRACTION of the clip's own duration rather
    // than a constant pixel length, so a one-second clip and a sixty-second clip both get a handle
    // long enough to grab and short enough not to overlap its neighbours -- see CurveKeyLayout's own
    // comment on why the offset is in TIME, not pixels.
    const f32 handleSeconds = dur * 0.04f;
    const auto layout = computeCurveLayout(c, view, handleSeconds);
    const f32 hitRadius = 8.0f * uiScale;
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    // PRESS: decide once, at the instant the mouse goes down over this canvas, what it landed on.
    // Held for the rest of the drag (see curveDragKind_'s own comment) so a fast movement that
    // strays outside a key's hit radius mid-drag does not drop it.
    if (canvasActive && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const CurveHitResult hit = curveHitTest(layout, Vec2{mouse.x, mouse.y}, hitRadius);
        curveDragKind_ = hit.kind;
        curveDragKeyIndex_ = hit.keyIndex;
        // One undo entry per gesture, pushed at this press (not per frame), only when the press
        // landed on something draggable -- a click on empty canvas leaves no no-op entry.
        if (curveDragKind_ != CurveHitKind::None) pushUndo();
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) curveDragKind_ = CurveHitKind::None;

    // Drag: apply this frame's mouse delta through the same curve-space math the geometry file
    // exposes for testing -- a key moves; a handle's new screen position implies a new tangent slope.
    if (canvasActive && curveDragKind_ != CurveHitKind::None &&
        curveDragKeyIndex_ < c.times.size()) {
        const ImVec2 md = ImGui::GetIO().MouseDelta;
        if (md.x != 0.0f || md.y != 0.0f) {
            const usize idx = curveDragKeyIndex_;
            const Vec2 keyScreen = curveToScreen(view, c.times[idx], c.values[idx]);
            if (curveDragKind_ == CurveHitKind::Key) {
                f32 newTime = 0.0f, newValue = 0.0f;
                screenToCurve(view, Vec2{keyScreen.x + md.x, keyScreen.y + md.y}, newTime, newValue);
                // Time clamped between neighbours, same rule as the numeric row below: the sampler
                // binary-searches `times`.
                const f32 lo = idx > 0 ? c.times[idx - 1] : -1e9f;
                const f32 hi = (idx + 1) < c.times.size() ? c.times[idx + 1] : 1e9f;
                c.times[idx] = std::min(std::max(newTime, lo), hi);
                c.values[idx] = newValue;
                dirty_ = true;
            } else if (curveDragKind_ == CurveHitKind::InHandle && idx < c.inTangents.size()) {
                const Vec2 handle = curveToScreen(view, c.times[idx] - handleSeconds,
                                                            c.values[idx] - c.inTangents[idx] * handleSeconds);
                c.inTangents[idx] = tangentSlopeFromHandle(
                    view, keyScreen, Vec2{handle.x + md.x, handle.y + md.y}, /*isOutHandle=*/false);
                dirty_ = true;
            } else if (curveDragKind_ == CurveHitKind::OutHandle && idx < c.outTangents.size()) {
                const Vec2 handle = curveToScreen(view, c.times[idx] + handleSeconds,
                                                            c.values[idx] + c.outTangents[idx] * handleSeconds);
                c.outTangents[idx] = tangentSlopeFromHandle(
                    view, keyScreen, Vec2{handle.x + md.x, handle.y + md.y}, /*isOutHandle=*/true);
                dirty_ = true;
            }
        }
    }

    // Draw: keys and, where present, their tangent handles joined by a line through the key -- the
    // usual "broken tangent" presentation (OcCurve stores independent in-/out-tangents per key).
    for (const CurveKeyLayout& k : layout) {
        const bool isDragTarget = curveDragKeyIndex_ == k.index && curveDragKind_ != CurveHitKind::None;
        if (k.hasTangents) {
            const ImVec2 inPt(k.inHandleScreen.x, k.inHandleScreen.y);
            const ImVec2 outPt(k.outHandleScreen.x, k.outHandleScreen.y);
            const ImVec2 keyPt(k.keyScreen.x, k.keyScreen.y);
            dl->AddLine(inPt, keyPt, IM_COL32(230, 190, 90, 200), 1.0f);
            dl->AddLine(keyPt, outPt, IM_COL32(230, 190, 90, 200), 1.0f);
            const bool inDrag = isDragTarget && curveDragKind_ == CurveHitKind::InHandle;
            const bool outDrag = isDragTarget && curveDragKind_ == CurveHitKind::OutHandle;
            dl->AddCircleFilled(inPt, hitRadius * 0.45f,
                                 inDrag ? IM_COL32(255, 235, 140, 255) : IM_COL32(230, 190, 90, 220));
            dl->AddCircleFilled(outPt, hitRadius * 0.45f,
                                 outDrag ? IM_COL32(255, 235, 140, 255) : IM_COL32(230, 190, 90, 220));
        }
        const bool keyDrag = isDragTarget && curveDragKind_ == CurveHitKind::Key;
        dl->AddCircleFilled(ImVec2(k.keyScreen.x, k.keyScreen.y), hitRadius * 0.5f,
                             keyDrag ? IM_COL32(255, 255, 255, 255) : IM_COL32(150, 230, 160, 255));
    }
#endif
}

void AnimEditor::drawSockets() {
#if AVER_WITH_IMGUI
    if (skel_.bones.empty()) { ImGui::TextDisabled("no skeleton"); return; }
    // Skeleton tab only -- a correctness rule, not layout: a socket lives on the .ocskel, but save()
    // on a clip tab writes the .ocanim, so editing here from a clip would set dirty_, write the clip,
    // clear the flag, and lose the socket without a word. Bone tree shows on both tabs (reading is
    // useful); writing only works on the skeleton tab.
    if (isClip_) {
        if (skel_.sockets.empty()) ImGui::TextDisabled("None on this rig.");
        else for (const fmt::OcSocket& k : skel_.sockets) {
            const char* bn = k.bone < skel_.bones.size() ? skel_.bones[k.bone].name.c_str() : "?";
            ImGui::BulletText("%s  on %s", k.name.c_str(), bn);
        }
        ImGui::TextDisabled("Open the .ocskel to edit these.");
        return;
    }

    // Save sits here (as on the notify panel): the host only calls saveAllDirty() from the quit
    // prompt, so without a button closing the editor would be the only way to persist a socket.
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
        pushUndo();
        fmt::OcSocket k;
        k.bone = static_cast<u32>(selectedBone_);
        // Named after the bone and made unique: the format doesn't enforce unique names, and
        // OcSkeleton::socket returns the first match, so a duplicate would be unreachable by name.
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
                // A rename onto an existing name is refused, same reason the add path disambiguates.
                const fmt::OcSocket* clash = next.empty() ? nullptr : skel_.socket(next);
                if (clash && clash != &k) {
                    AVER_WARN("[AnimEditor] a socket called '{}' already exists on this rig", next);
                    std::snprintf(socketNameBuf_, sizeof socketNameBuf_, "%s", k.name.c_str());
                } else if (!next.empty() && next != k.name) {
                    pushUndo();
                    k.name = next;
                    dirty_ = true;
                }
            }

            ImGui::SetNextItemWidth(w);
            // Gated like "Add on selected bone" above -- was enabled with no bone selected and
            // silently did nothing when clicked.
            ImGui::BeginDisabled(!haveBone);
            if (ImGui::SmallButton("Move to selected bone") && k.bone != static_cast<u32>(selectedBone_)) {
                pushUndo();
                k.bone = static_cast<u32>(selectedBone_);
                dirty_ = true;
            }
            ImGui::EndDisabled();
            if (!haveBone && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Select a bone in the skeleton list first.");

            // Centimetres, dragged rather than typed: placing a grip is done by watching the preview
            // marker, not the number -- still editable when the number is what you have.
            f32 t[3] = {k.translation.x, k.translation.y, k.translation.z};
            ImGui::SetNextItemWidth(w * 1.6f);
            const bool offsetChanged = ImGui::DragFloat3("Offset (cm)", t, 0.25f);
            // One drag is one undo entry: pushed at activation, before this drag's delta is applied.
            if (ImGui::IsItemActivated()) pushUndo();
            if (offsetChanged) {
                k.translation = Vec3{t[0], t[1], t[2]};
                dirty_ = true;
            }
            f32 q[4] = {k.rotation.x, k.rotation.y, k.rotation.z, k.rotation.w};
            ImGui::SetNextItemWidth(w * 1.6f);
            const bool rotChanged = ImGui::DragFloat4("Rotation (xyzw)", q, 0.01f);
            if (ImGui::IsItemActivated()) pushUndo();
            if (rotChanged) {
                // Renormalised on edit: dragging four components independently breaks unit length,
                // which would scale the attachment rather than rotate it. Zero-length falls back to
                // identity instead of NaN.
                const f32 len = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
                k.rotation = len > 1e-6f ? Quat{q[0]/len, q[1]/len, q[2]/len, q[3]/len}
                                         : Quat{0, 0, 0, 1};
                dirty_ = true;
            }

            if (ImGui::SmallButton("Delete")) {
                pushUndo();
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

    // Save lives here, next to the only editable thing in this editor. The host only calls
    // saveAllDirty() from the quit prompt, so without this closing the editor would be the only way
    // to persist a notify. Ctrl+S alongside it, matching GraphEditor's toolbar.
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
        pushUndo();
        fmt::OcNotify n;
        n.time = time_;
        n.name = "OnNotify";
        clip_.notifies.push_back(n);
        // Only when durations are already in use for this clip -- keeps a clip with no states at all
        // writing no NTFD chunk.
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

        // Time and name go inside the Selectable, not beside it: a zero-width Selectable spans the
        // whole line, so a SameLine after it would draw off the end of the panel. PushID above already
        // makes the row unique, so two notifies sharing a name are still two rows.
        char row[160];
        const f32 rowDur = notifyDurationAt(i);
        // A state's window shows in the collapsed row too, else the one fact distinguishing it from
        // an instant notify stays invisible until clicked.
        if (rowDur > 0.0f)
            std::snprintf(row, sizeof row, "%7.3f s + %5.3f s   %s", n.time, rowDur, n.name.c_str());
        else
            std::snprintf(row, sizeof row, "%7.3f s   %s", n.time, n.name.c_str());
        if (ImGui::Selectable(row, sel)) {
            selectedNotify_ = static_cast<int>(i);
            std::snprintf(notifyNameBuf_, sizeof notifyNameBuf_, "%s", n.name.c_str());
            // Selecting a notify moves the playhead to it (the only way to see what it marks) and
            // pauses, same reason the scrubber does.
            time_ = n.time;
            playing_ = false;
        }

        if (sel) {
            ImGui::Indent();
            ImGui::SetNextItemWidth(180.0f * (ImGui::GetFontSize() / 16.0f));
            ImGui::InputText("Event", notifyNameBuf_, sizeof notifyNameBuf_);
            // Committed on deactivate (same boundary as the graph editor's text fields), so one typed
            // name is one edit, not one per keystroke.
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                std::string next = notifyNameBuf_;
                // Fired as a graph event landing in a node-adjacent record, so stray whitespace is
                // silently trimmed rather than refused.
                while (!next.empty() && next.front() == ' ') next.erase(next.begin());
                while (!next.empty() && next.back() == ' ') next.pop_back();
                if (!next.empty() && next != n.name) { pushUndo(); n.name = next; dirty_ = true; }
            }
            f32 t = n.time;
            const f32 dur = clip_.duration > 0.0f ? clip_.duration : 1.0f;
            ImGui::SetNextItemWidth(180.0f * (ImGui::GetFontSize() / 16.0f));
            const bool timeChanged = ImGui::SliderFloat("Time", &t, 0.0f, dur, "%.3f s");
            // One drag is one undo entry: pushed at activation, before this widget's apply below.
            if (ImGui::IsItemActivated()) pushUndo();
            if (timeChanged) {
                n.time = t;
                time_ = t;
                playing_ = false;
                dirty_ = true;
            }
            // Duration turns an instant notify into a state open for this long (item 7.1); 0 stays
            // instant, as before. See AnimSystem.hpp's own banner for what the runtime does with a
            // non-zero value.
            f32 stateDur = notifyDurationAt(i);
            ImGui::SetNextItemWidth(180.0f * (ImGui::GetFontSize() / 16.0f));
            const bool durChanged = ImGui::SliderFloat("Duration", &stateDur, 0.0f, dur, "%.3f s");
            if (ImGui::IsItemActivated()) pushUndo();
            if (durChanged) {
                setNotifyDuration(i, stateDur);
                dirty_ = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled(stateDur > 0.0f ? "(state)" : "(instant)");
            if (ImGui::SmallButton("Delete")) {
                pushUndo();
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

// Stable time-to-pixel mapper shared by every lane (master slider, notify/curve/track lanes) so they
// all agree on where a second lands. In the file's anonymous namespace already (opened above
// AnimSkinFeature), needing no linkage of its own -- same as boneBox and computeAnimEditorBonePalette.
f32 laneX(f32 x0, f32 x1, f32 t, f32 dur) { return x0 + (x1 - x0) * (dur > 0.0f ? t / dur : 0.0f); }
#if AVER_WITH_IMGUI
// A lane row's screen rect, named rather than std::pair<ImVec2, ImVec2> so a call site reads
// `.min`/`.max` instead of `.first`/`.second`.
struct LaneRect { ImVec2 min, max; };
#endif

// Per-track lanes: each track gets its own row, named by its bone, with only its own keys -- replacing
// one shared bar where forty overlapping ticks said a key existed somewhere but not whose. This also
// makes a key clickable: drawTracks()'s selectedTrack_/selectedTrackKey_ are set from a click in a
// specific lane.
//
// Notifies and the selected curve get their own lanes too, above the track list: the old notify-loop
// comment said Unreal gives notifies their own lane and doing the same here would be right "once there
// are lanes to give" -- there are now, so notifies take the first and the selected curve takes the
// next, both drawn unscrolled and always visible -- small bounded content an author wants to see while
// scrolling past track 80 of 200, not something that should scroll away with the tracks.
void AnimEditor::drawTimeline() {
#if AVER_WITH_IMGUI
    const f32 dur = clip_.duration > 0.0f ? clip_.duration : 1.0f;
    ImGui::SetNextItemWidth(-1);
    // Scrubbing pauses -- a slider fighting the clock can't be placed. Now the only thing drawn on
    // the slider itself; per-track ticks, curve overlay and notify markers moved to their own lanes.
    if (ImGui::SliderFloat("##time", &time_, 0.0f, dur, "%.3f s")) playing_ = false;

    const f32 uiScale = ImGui::GetFontSize() / 16.0f;
    // Name column width fixed across every lane (the master slider has none, so this constant exists
    // only from here down), so a key's x in one row lands under the same instant as in every other row
    // and under the playhead line.
    const f32 nameW = 130.0f * uiScale;
    const f32 rowH = ImGui::GetTextLineHeightWithSpacing();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // A lane row's canvas: fixed-width name label, then an InvisibleButton spanning the rest (same
    // input-capture shape as drawCurveWidget, no vendored timeline widget anywhere in this tree).
    // Returns the canvas rect to draw/hit-test against.
    auto beginLaneRow = [&](const char* label) -> LaneRect {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(nameW);
        ImGui::InvisibleButton("##lane", ImVec2(ImGui::GetContentRegionAvail().x, rowH));
        return {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
    };

    // Notify lane, always drawn even with zero notifies -- an author scanning forty tracks needs to
    // see "no notifies here", not wonder if the row vanished.
    {
        ImGui::PushID("##notifyLane");
        const auto [p0, p1] = beginLaneRow("Notifies");
        dl->AddRectFilled(p0, p1, IM_COL32(26, 26, 30, 255));
        for (usize i = 0; i < clip_.notifies.size(); ++i) {
            const fmt::OcNotify& n = clip_.notifies[i];
            const f32 x = laneX(p0.x, p1.x, n.time, dur);
            const bool sel = (static_cast<int>(i) == selectedNotify_);
            const ImU32 col = sel ? IM_COL32(255, 220, 90, 255) : IM_COL32(120, 200, 255, 230);
            // A state's window, drawn as a translucent band under the marker -- visual-only, matching
            // the clamp AnimSystem applies at the loop seam, so dragging past the clip end stops where
            // the runtime will actually close it.
            const f32 stateDur = notifyDurationAt(i);
            if (stateDur > 0.0f) {
                const f32 xEnd = laneX(p0.x, p1.x, std::min(n.time + stateDur, dur), dur);
                dl->AddRectFilled(ImVec2(x, p0.y), ImVec2(xEnd, p1.y),
                                  sel ? IM_COL32(255, 220, 90, 70) : IM_COL32(120, 200, 255, 55));
            }
            // Downward triangle, not another vertical tick, so a notify isn't mistaken for a track
            // lane's key ticks.
            const ImVec2 tri[3] = {ImVec2(x - 5.0f, p0.y), ImVec2(x + 5.0f, p0.y), ImVec2(x, p0.y + 9.0f)};
            dl->AddConvexPolyFilled(tri, 3, col);
            dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), col, sel ? 2.0f : 1.0f);
        }
        dl->AddLine(ImVec2(laneX(p0.x, p1.x, time_, dur), p0.y), ImVec2(laneX(p0.x, p1.x, time_, dur), p1.y),
                    IM_COL32(255, 210, 90, 150), 1.0f);
        ImGui::PopID();
    }

    // Selected curve's own lane, drawn only when one is selected -- else it would waste vertical
    // space on every clip without curves. Normalised to the curve's own min/max (a fixed 0..1 mapping
    // would flatten most curves into a line).
    if (selectedCurve_ >= 0 && static_cast<usize>(selectedCurve_) < clip_.curves.size()) {
        const fmt::OcCurve& c = clip_.curves[static_cast<usize>(selectedCurve_)];
        ImGui::PushID("##curveLane");
        char label[64];
        std::snprintf(label, sizeof label, "%.16s", c.name.c_str());
        const auto [p0, p1] = beginLaneRow(label);
        dl->AddRectFilled(p0, p1, IM_COL32(22, 30, 24, 255));
        if (c.times.size() >= 2 && c.values.size() == c.times.size()) {
            f32 lo = c.values[0], hi = c.values[0];
            for (const f32 v : c.values) { lo = v < lo ? v : lo; hi = v > hi ? v : hi; }
            const f32 span = (hi - lo) > 1e-6f ? (hi - lo) : 1.0f;
            const f32 top = p0.y + 2.0f, bot = p1.y - 2.0f;
            ImVec2 prev{};
            bool have = false;
            // Sampled across the lane, not drawn key-to-key, so a Step curve reads as steps rather
            // than a straight line it isn't.
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
        dl->AddLine(ImVec2(laneX(p0.x, p1.x, time_, dur), p0.y), ImVec2(laneX(p0.x, p1.x, time_, dur), p1.y),
                    IM_COL32(255, 210, 90, 150), 1.0f);
        ImGui::PopID();
    }

    if (clip_.tracks.empty()) { ImGui::TextDisabled("No tracks."); return; }

    // Track lanes: FirstPerson's own 7-bone rig is small, but nothing here assumes that stays true, so
    // this is a fixed-height scrolling child with ImGuiListClipper choosing which rows to submit (same
    // shape as SandboxContentBrowser.cpp's asset grid) -- without it, a 200-bone rig would mean 200
    // InvisibleButtons and hit-test sweeps every frame regardless of visibility.
    constexpr int kVisibleLanes = 8;
    const f32 lanesHeight = std::min(static_cast<f32>(clip_.tracks.size()), static_cast<f32>(kVisibleLanes)) * rowH
                          + ImGui::GetStyle().ItemSpacing.y;
    if (ImGui::BeginChild("##trackLanes", ImVec2(0, lanesHeight), true)) {
        // Re-fetched, not the outer `dl`: a child window has its own ImDrawList and clip rect (what
        // makes scrolled-past rows disappear correctly); the parent's list would paint the wrong place.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const f32 hitRadius = 7.0f * uiScale;
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(clip_.tracks.size()), rowH);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const usize i = static_cast<usize>(row);
                fmt::OcTrack& t = clip_.tracks[i];
                ImGui::PushID(row);
                const bool named = t.boneIndex < skel_.bones.size();
                const auto [p0, p1] = beginLaneRow(named ? skel_.bones[t.boneIndex].name.c_str()
                                                          : "<out of range>");
                const bool laneHovered = ImGui::IsItemHovered();
                const bool laneActive = ImGui::IsItemActive();

                dl->AddRectFilled(p0, p1, IM_COL32(30, 30, 34, 255));

                // Keys tinted by the same bone palette the 3D preview paints its boxes with
                // (computeAnimEditorBonePalette, rebuilt in buildPreview), so a key and the bone it
                // moves read as the same colour. Falls back to neutral grey on the one frame before the
                // palette exists yet (bonePalette_ starts empty; buildPreview fills it before this ever
                // runs a second time) or for an out-of-range boneIndex, matching buildPreview's fallback.
                BoneColor bc{0.6f, 0.6f, 0.6f};
                if (named && t.boneIndex < bonePalette_.size()) bc = bonePalette_[t.boneIndex];
                // Clamped before the *255 conversion: IM_COL32 has no range check, so a component
                // drifting past 1.0 (float error from the golden-angle hue math) would bleed into the
                // next channel instead of clipping to white.
                const auto toByte = [](f32 v) { return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
                const ImU32 keyCol = IM_COL32(toByte(bc.r), toByte(bc.g), toByte(bc.b), 235);

                const bool thisTrackSelected = (selectedTrack_ == static_cast<int>(i));
                for (usize k = 0; k < t.times.size(); ++k) {
                    const f32 x = laneX(p0.x, p1.x, t.times[k], dur);
                    const bool keySelected = thisTrackSelected && selectedTrackKey_ == k;
                    const f32 r = keySelected ? 5.0f * uiScale : 3.0f * uiScale;
                    dl->AddCircleFilled(ImVec2(x, (p0.y + p1.y) * 0.5f), r,
                                        keySelected ? IM_COL32(255, 255, 255, 255) : keyCol);
                }
                // Row outline on the selected track, so "which track will Insert/Delete Key act on"
                // is visible even with no key selected inside it.
                if (thisTrackSelected) dl->AddRect(p0, p1, IM_COL32(255, 220, 140, 200));
                dl->AddLine(ImVec2(laneX(p0.x, p1.x, time_, dur), p0.y),
                            ImVec2(laneX(p0.x, p1.x, time_, dur), p1.y), IM_COL32(255, 210, 90, 120), 1.0f);

                // Press: decide once, at mouse-down, what it landed on (same press-time-hit-test-
                // then-hold shape as drawCurveWidget) so a fast drag mid-gesture doesn't drop it.
                if (laneHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    usize hit = kInvalidKeyIndex;
                    f32 bestDist = hitRadius;
                    const f32 mx = ImGui::GetIO().MousePos.x;
                    for (usize k = 0; k < t.times.size(); ++k) {
                        const f32 dist = std::fabs(mx - laneX(p0.x, p1.x, t.times[k], dur));
                        if (dist <= bestDist) { bestDist = dist; hit = k; }
                    }
                    selectedTrack_ = static_cast<int>(i);
                    selectedTrackKey_ = hit;
                    // Selecting a track selects its bone too (same cross-highlight as drawSockets'
                    // socket selection), so tree/preview/lane all agree.
                    if (named) selectedBone_ = static_cast<int>(t.boneIndex);
                    trackKeyDragging_ = (hit != kInvalidKeyIndex);
                    // One undo entry per gesture, pushed at this press, only when it landed on a key --
                    // a click on empty lane space moves nothing and leaves no no-op entry on the stack
                    // (mirrors drawCurveWidget's identical bracket).
                    if (trackKeyDragging_) pushUndo();
                }
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) trackKeyDragging_ = false;

                // Drag: re-derive the key's time from the mouse's current x every frame (simpler than
                // the curve widget's delta approach; a lane has only one meaningful axis). trackMoveKey
                // re-sorts rather than clamps (see its own header comment), so the key's index can
                // change mid-drag -- the return value is what selectedTrackKey_ tracks afterward.
                //
                // Gated on laneActive, not just trackKeyDragging_ (same gate as drawCurveWidget's
                // canvasActive): ButtonBehavior clears the active id inside InvisibleButton() itself on
                // mouse-up, so laneActive already reads false on the release frame.
                if (trackKeyDragging_ && laneActive && thisTrackSelected &&
                    selectedTrackKey_ != kInvalidKeyIndex && selectedTrackKey_ < t.times.size()) {
                    const f32 mx = ImGui::GetIO().MousePos.x;
                    const f32 span = std::max(1.0f, p1.x - p0.x);
                    const f32 newTime = std::clamp((mx - p0.x) / span * dur, 0.0f, dur);
                    const usize moved = trackMoveKey(t, selectedTrackKey_, newTime);
                    if (moved != kInvalidKeyIndex) { selectedTrackKey_ = moved; dirty_ = true; }
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
#endif
}

// Clamps selectedTrack_/selectedTrackKey_ against clip_.tracks as it stands now -- the one place this
// file re-derives after any edit that can shrink either array: Undo/Redo (applyClipUndoState), and
// drawClipRetime()'s trim (the only retime op that removes keys; scale/shift only move times).
// Bounds-only, not identity-preserving: a trim
// removing keys before the selected one shifts later indices down, so selection can land on a
// different surviving key rather than nothing -- the same trade applyClipUndoState already makes for
// selectedNotify_/selectedCurve_.
void AnimEditor::clampTrackKeySelection() {
    if (selectedTrack_ < 0 || static_cast<usize>(selectedTrack_) >= clip_.tracks.size()) {
        selectedTrack_ = -1;
        selectedTrackKey_ = kInvalidKeyIndex;
        return;
    }
    const usize keyCount = clip_.tracks[static_cast<usize>(selectedTrack_)].times.size();
    if (selectedTrackKey_ != kInvalidKeyIndex && selectedTrackKey_ >= keyCount) selectedTrackKey_ = kInvalidKeyIndex;
}

// The three whole-clip retime operations, wired to AnimEdit.hpp's pure functions. Each is a one-shot
// button press, so the "one undo entry per gesture" rule (the bracket SandboxPanels.cpp's own
// materialPanel comment describes) is automatic here -- a click is already a single event.
//
// Every apply is push-then-attempt-then-cancel-on-refusal, via SnapshotUndo::cancelPush()
// (scaleClipDuration/shiftClipTime/trimClip can all refuse: non-finite/non-positive factor, a trim
// range clamping to nothing) -- an undo entry restoring an identical state would be a Ctrl+Z that
// visibly does nothing (see materialPanel's "PUSHED ON RELEASE" comment).
void AnimEditor::drawClipRetime() {
#if AVER_WITH_IMGUI
    if (!ImGui::CollapsingHeader("Retime")) return;
    // Lazily defaulted to the full clip: an End of 0.0 always fails trimClip's `end > start` check, so
    // the first look at this panel would find Trim refusing until Full Range is pressed. Re-applied
    // only while End is still its unset default, so a deliberate 0.0 isn't fought.
    if (retimeTrimEnd_ <= 0.0f && clip_.duration > 0.0f) retimeTrimEnd_ = clip_.duration;
    const f32 w = 130.0f * (ImGui::GetFontSize() / 16.0f);

    ImGui::TextWrapped("These act on the WHOLE clip: every track's keys, every notify's time and "
                       "state window, and every curve's keys move together, so nothing drifts out "
                       "of sync with the pose.");

    ImGui::SetNextItemWidth(w);
    ImGui::DragFloat("##scaleFactor", &retimeScaleFactor_, 0.01f, 0.01f, 100.0f, "%.3fx");
    ImGui::SameLine();
    if (ImGui::SmallButton("Scale duration")) {
        pushUndo();
        if (scaleClipDuration(clip_, retimeScaleFactor_)) {
            dirty_ = true;
            time_ = std::min(time_, clip_.duration);
        } else {
            clipHistory_.cancelPush();
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("< 1 speeds the clip up, > 1 slows it down. CubicSpline tangents rescale "
                          "with it so the curve's SHAPE does not change, only its timing.");

    ImGui::SetNextItemWidth(w);
    ImGui::DragFloat("##shiftSeconds", &retimeShiftSeconds_, 0.01f, -3600.0f, 3600.0f, "%.3f s");
    ImGui::SameLine();
    if (ImGui::SmallButton("Shift all keys")) {
        pushUndo();
        if (shiftClipTime(clip_, retimeShiftSeconds_)) dirty_ = true;
        else clipHistory_.cancelPush();
    }

    ImGui::SetNextItemWidth(w);
    ImGui::DragFloat("##trimStart", &retimeTrimStart_, 0.01f, 0.0f, clip_.duration, "Start %.3f s");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(w);
    ImGui::DragFloat("##trimEnd", &retimeTrimEnd_, 0.01f, 0.0f, clip_.duration, "End %.3f s");
    ImGui::SameLine();
    if (ImGui::SmallButton("Full range")) { retimeTrimStart_ = 0.0f; retimeTrimEnd_ = clip_.duration; }
    if (ImGui::SmallButton("Trim to [Start, End]")) {
        pushUndo();
        if (trimClip(clip_, retimeTrimStart_, retimeTrimEnd_)) {
            dirty_ = true;
            time_ = std::min(std::max(time_ - retimeTrimStart_, 0.0f), clip_.duration);
            retimeTrimStart_ = 0.0f;
            retimeTrimEnd_ = clip_.duration;
            clampTrackKeySelection();
        } else {
            clipHistory_.cancelPush();
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Cuts everything outside [Start, End] and slides what's left back to "
                          "start at zero. A boundary key is inserted at each cut first, so the "
                          "surviving span keeps the exact pose it already had at its own edges.");
#endif
}

// Track's sampled value at the playhead, in OcTrack's own units: engine centimetres for translation
// (OcBone::translation's own unit), raw quaternion x/y/z/w for rotation (not Euler degrees, a different
// lossy representation), unitless multiplier for scale.
//
// Reads pose_ when it can, rather than calling trackSampleAt() again: buildPreview() already ran
// sampleAnimation(clip_, t, pose_) this frame (draw() calls it before drawTracks()), and that call
// writes a bone's translation/rotation/scale only for channels a track enables -- an untouched channel
// keeps whatever restPose or an earlier track put there -- so pose_.local[t.boneIndex] already holds
// exactly what this track contributed with the same loop-wrap/clamp -- re-sampling would just risk
// drifting from it.
//
// Falls back to trackSampleAt() only when there's no pose to read (boneIndex past pose_.local's size,
// e.g. no matching skeleton, see draw()'s own "No skeleton found for this clip" branch) -- a raw sample
// is still better than nothing; this path does not loop-wrap time_, since with no skeleton to check
// tracks against there is no duration worth trusting over the raw playhead.
std::string AnimEditor::trackValueLabel(const fmt::OcTrack& t) const {
    std::string s;
    auto appendVec3 = [&](const char* tag, f32 x, f32 y, f32 z) {
        char buf[80];
        std::snprintf(buf, sizeof buf, "%s%s(%.2f, %.2f, %.2f)", s.empty() ? "" : "  ", tag, x, y, z);
        s += buf;
    };
    auto appendQuat = [&](f32 x, f32 y, f32 z, f32 w) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%sR(%.3f, %.3f, %.3f, %.3f)", s.empty() ? "" : "  ", x, y, z, w);
        s += buf;
    };

    if (t.boneIndex < pose_.local.size()) {
        const Transform& xf = pose_.local[t.boneIndex];
        if (t.channels & fmt::kOcChannelTranslation) appendVec3("T", xf.position.x, xf.position.y, xf.position.z);
        if (t.channels & fmt::kOcChannelRotation)    appendQuat(xf.rotation.x, xf.rotation.y, xf.rotation.z, xf.rotation.w);
        if (t.channels & fmt::kOcChannelScale)       appendVec3("S", xf.scale.x, xf.scale.y, xf.scale.z);
        return s.empty() ? std::string("-") : s;
    }

    std::vector<f32> values;
    if (!trackSampleAt(t, time_, values)) return "-";
    usize idx = 0;
    if ((t.channels & fmt::kOcChannelTranslation) && idx + 3 <= values.size()) {
        appendVec3("T", values[idx], values[idx + 1], values[idx + 2]);
        idx += 3;
    }
    if ((t.channels & fmt::kOcChannelRotation) && idx + 4 <= values.size()) {
        appendQuat(values[idx], values[idx + 1], values[idx + 2], values[idx + 3]);
        idx += 4;
    }
    if ((t.channels & fmt::kOcChannelScale) && idx + 3 <= values.size()) {
        appendVec3("S", values[idx], values[idx + 1], values[idx + 2]);
        idx += 3;
    }
    return s.empty() ? std::string("-") : s;
}

void AnimEditor::drawTracks() {
#if AVER_WITH_IMGUI
    drawClipRetime();
    ImGui::Separator();

    if (clip_.tracks.empty()) { ImGui::TextDisabled("no tracks"); return; }
    if (!ImGui::BeginTable("tracks", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) return;
    ImGui::TableSetupColumn("Bone");
    ImGui::TableSetupColumn("Channels");
    ImGui::TableSetupColumn("Interp");
    ImGui::TableSetupColumn("Value @ playhead");
    ImGui::TableSetupColumn("Keys");
    ImGui::TableSetupColumn("Span");
    ImGui::TableHeadersRow();
    for (usize i = 0; i < clip_.tracks.size(); ++i) {
        const fmt::OcTrack& t = clip_.tracks[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const bool named = t.boneIndex < skel_.bones.size();
        // Bone cell is a Selectable: clicking it sets selectedTrack_, the same field drawTimeline()'s
        // lanes set, which drawTrackKeyEditor() reads for Insert/Delete Key. One selection, reachable
        // from either the table or the lane.
        const bool rowSelected = (selectedTrack_ == static_cast<int>(i));
        if (ImGui::Selectable(named ? skel_.bones[t.boneIndex].name.c_str() : "<out of range>",
                              rowSelected, ImGuiSelectableFlags_SpanAllColumns)) {
            selectedTrack_ = rowSelected ? -1 : static_cast<int>(i);
            selectedTrackKey_ = kInvalidKeyIndex;
            // Selecting a track selects its bone too (same cross-highlight as drawSockets), so the
            // bone tree/preview/table all agree.
            if (!rowSelected && named) selectedBone_ = static_cast<int>(t.boneIndex);
        }
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
        ImGui::TextUnformatted(trackValueLabel(t).c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%zu", t.times.size());
        ImGui::TableNextColumn();
        if (t.times.empty()) ImGui::TextUnformatted("-");
        else ImGui::Text("%.2f - %.2f s", t.times.front(), t.times.back());
        ImGui::PopID();
    }
    ImGui::EndTable();

    drawTrackKeyEditor();
#endif
}

// Selected key's details: exact time (typed/dragged) and value components (one drag per enabled
// channel), plus Insert/Delete for the selected track -- numeric counterpart to dragging a key in
// drawTimeline()'s lanes. Drawn below the table, not inside it: a cell is a poor home for
// DragFloatN controls whose count varies per track.
//
// Tangents aren't exposed here, only Value: unlike drawCurveWidget, which gives a curve's tangent
// handles their own draggable geometry, there's no canvas to drag a track's tangent on, and this
// panel's job is move/edit/add/delete keys, not tangents. A CubicSpline track's tangents stay as they
// were; OcTrack::valid() has no rule tying a value to its neighbours' slopes.
void AnimEditor::drawTrackKeyEditor() {
#if AVER_WITH_IMGUI
    if (selectedTrack_ < 0 || static_cast<usize>(selectedTrack_) >= clip_.tracks.size()) return;
    fmt::OcTrack& t = clip_.tracks[static_cast<usize>(selectedTrack_)];
    const bool named = t.boneIndex < skel_.bones.size();

    ImGui::Separator();
    ImGui::Text("Selected track: %s", named ? skel_.bones[t.boneIndex].name.c_str() : "<out of range>");

    if (ImGui::SmallButton("Insert key at playhead")) {
        pushUndo();
        const usize idx = trackInsertKey(t, time_);
        if (idx != kInvalidKeyIndex) { selectedTrackKey_ = idx; dirty_ = true; }
        else clipHistory_.cancelPush();
    }

    if (selectedTrackKey_ == kInvalidKeyIndex || selectedTrackKey_ >= t.times.size()) {
        ImGui::TextDisabled("No key selected. Click one in a lane above, or Insert one here.");
        return;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Delete key")) {
        pushUndo();
        if (trackDeleteKey(t, selectedTrackKey_)) dirty_ = true;
        else clipHistory_.cancelPush();
        selectedTrackKey_ = kInvalidKeyIndex;
        return;
    }

    const f32 w = 160.0f * (ImGui::GetFontSize() / 16.0f);
    const f32 dur = clip_.duration > 0.0f ? clip_.duration : 1.0f;

    // Time: DragFloat already doubles as "type an exact value" via double-click/Ctrl+click, so no
    // separate InputFloat is needed.
    f32 tk = t.times[selectedTrackKey_];
    ImGui::SetNextItemWidth(w);
    const bool timeChanged = ImGui::DragFloat("Time", &tk, 0.01f, 0.0f, dur, "%.3f s");
    // One drag is one undo entry: pushed at activation, before this widget's apply below (same
    // bracket as drawNotifies' "Time" slider).
    if (ImGui::IsItemActivated()) pushUndo();
    if (timeChanged) {
        // trackMoveKey re-sorts rather than clamps (see its own header comment), so the key's index
        // can change on crossing a neighbour -- the return value is what selectedTrackKey_ becomes.
        const usize moved = trackMoveKey(t, selectedTrackKey_, tk);
        if (moved != kInvalidKeyIndex) { selectedTrackKey_ = moved; dirty_ = true; }
    }

    // Value, one drag per enabled channel, via trackReadComponent/trackWriteComponent so the per-key
    // stride arithmetic (AnimEdit.hpp's top-comment trap) is never re-derived here.
    auto editVec3 = [&](const char* label, u8 channel) {
        if (!(t.channels & channel)) return;
        f32 v[3] = {0.0f, 0.0f, 0.0f};
        for (u32 c = 0; c < 3; ++c) trackReadComponent(t, selectedTrackKey_, channel, c, TrackKeySlot::Value, v[c]);
        ImGui::SetNextItemWidth(w * 1.6f);
        const bool changed = ImGui::DragFloat3(label, v, 0.05f);
        if (ImGui::IsItemActivated()) pushUndo();
        if (changed) {
            for (u32 c = 0; c < 3; ++c) trackWriteComponent(t, selectedTrackKey_, channel, c, TrackKeySlot::Value, v[c]);
            dirty_ = true;
        }
    };
    editVec3("Translation (cm)", fmt::kOcChannelTranslation);
    if (t.channels & fmt::kOcChannelRotation) {
        f32 q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        for (u32 c = 0; c < 4; ++c)
            trackReadComponent(t, selectedTrackKey_, fmt::kOcChannelRotation, c, TrackKeySlot::Value, q[c]);
        ImGui::SetNextItemWidth(w * 1.8f);
        const bool changed = ImGui::DragFloat4("Rotation (xyzw)", q, 0.01f);
        if (ImGui::IsItemActivated()) pushUndo();
        if (changed) {
            // Renormalised on edit, same reasoning as drawSockets' rotation drag: four independent
            // components stop being a rotation. AnimSampler.cpp reads this key back through
            // Quat::normalized() regardless (sampleChannel), so this is hygiene -- keeps what's on disk
            // a genuine rotation too.
            const f32 len = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
            if (len > 1e-6f) { q[0] /= len; q[1] /= len; q[2] /= len; q[3] /= len; }
            else { q[0] = 0.0f; q[1] = 0.0f; q[2] = 0.0f; q[3] = 1.0f; }
            for (u32 c = 0; c < 4; ++c)
                trackWriteComponent(t, selectedTrackKey_, fmt::kOcChannelRotation, c, TrackKeySlot::Value, q[c]);
            dirty_ = true;
        }
    }
    editVec3("Scale", fmt::kOcChannelScale);
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
        pushUndo();
        if (set) clip_.flags = static_cast<u8>(clip_.flags | bit);
        else     clip_.flags = static_cast<u8>(clip_.flags & ~bit);
        dirty_ = true;
    }
#else
    (void)bit; (void)label;
#endif
}

// Points the preview's pivot at one bone and picks a distance that frames it, not the whole rig (see
// header comment for why frameAll() doesn't fit).
//
// Framing distance comes from how far this bone actually reaches, not boneBox's geometry: a bone's
// box is deliberately thin (kBoneThicknessCm = 2.2cm, sized to read as a limb, not to fill a viewport),
// so sizing off it would land the camera too close (the same failure buildPreview's `addZoom(2.6f)`
// backs off from). Uses the distance to
// whatever is attached (parent, or any child bone) -- longest wins, so a hip with a leg hanging off it
// isn't cropped by averaging.
void AnimEditor::frameSelectedBone(render::preview::ActorPreview& preview, usize boneIndex) {
    if (boneIndex >= model_.size() || boneIndex >= skel_.bones.size()) return;
    const Vec3 at{model_[boneIndex].m[3][0], model_[boneIndex].m[3][1], model_[boneIndex].m[3][2]};

    f32 reach = 0.0f;
    const i32 parent = skel_.bones[boneIndex].parent;
    if (parent >= 0 && static_cast<usize>(parent) < model_.size()) {
        const Vec3 p{model_[usize(parent)].m[3][0], model_[usize(parent)].m[3][1],
                     model_[usize(parent)].m[3][2]};
        reach = std::max(reach, (at - p).size());
    }
    for (usize i = 0; i < skel_.bones.size() && i < model_.size(); ++i) {
        if (skel_.bones[i].parent != static_cast<i32>(boneIndex)) continue;
        const Vec3 c{model_[i].m[3][0], model_[i].m[3][1], model_[i].m[3][2]};
        reach = std::max(reach, (at - c).size());
    }
    // An isolated bone (no parent/children) has nothing to measure reach against. kRootCubeCm is the
    // box buildPreview draws for this case, so framing at a small multiple keeps it comfortably in view.
    if (reach < 1e-4f) reach = kRootCubeCm * 3.0f;

    render::preview::PreviewCamera& cam = preview.camera();
    cam.pivot[0] = at.x; cam.pivot[1] = at.y; cam.pivot[2] = at.z;
    // Same formula as ActorPreview::frameAll() (span * 1.8, clamped to [2, 500000]), not reinvented:
    // `reach` is a one-direction radius, frameAll's `span` a full diameter, hence the *2 first.
    cam.distance = std::clamp(reach * 2.0f * 1.8f, 2.0f, 500000.0f);
}

void AnimEditor::draw(Engine& e) {
#if AVER_WITH_IMGUI
    reloadIfNeeded();

    // Clock runs off ImGui's time, not engine dt: an asset tab only draws while active, so an
    // engine-dt clock would accumulate while hidden and the clip would jump on return.
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

    // Ctrl+Z/Ctrl+Y reach the same undo()/redo() as the buttons, guarded the same way, and skipped
    // while an InputText has focus (it has its own Ctrl+Z; WantTextInput is the same check
    // SoundEditor.cpp uses to tell the two apart). Follows isClip_, same as pushUndo().
    {
        const ImGuiIO& io = ImGui::GetIO();
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (focused && !io.WantTextInput) {
            if (canUndo() && keybinds().pressed(CommandId::EditUndo, io)) undo();
            if (canRedo() && keybinds().pressed(CommandId::EditRedo, io)) redo();
        }
    }
    ImGui::BeginDisabled(!canUndo());
    if (ImGui::Button(ICON_UNDO " Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canRedo());
    if (ImGui::Button(ICON_REDO " Redo")) redo();
    ImGui::EndDisabled();
    ImGui::SameLine();

    ImGui::Text("%s", isClip_ ? "Animation clip" : "Skeleton");
    ImGui::SameLine();
    if (isClip_)
        ImGui::TextDisabled("%.3f s   %zu track(s)   %s", clip_.duration, clip_.tracks.size(),
                            clip_.storage == fmt::OcAnimStorage::BakedUniform ? "baked" : "keyframed");
    else
        ImGui::TextDisabled("%zu bone(s)", skel_.bones.size());

    // Was static text reading only one of the three flags (loop), with no way to change it or the
    // other two; all three round-trip through the format already, so this is what was missing to
    // author them. Not labelled "Loop": drawTransport() already has an unrelated Checkbox("Loop",
    // &loop_) for this tab's own preview playback (session-only, never saved) -- same label here would
    // collide IDs and read as the same setting. "Clip loops" names the authored, persisted bit
    // (clip_.flags).
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
    // Transport and timeline lanes now live in their own retractable strip along the bottom of the
    // tab (were drawn inline here before, fighting the preview/side panels for height). See the
    // layout comment below for why the strip's height must be decided before this row is.

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
        // Bones/Sockets: now in the preview toolbar's Show dropdown, below.
    }

    buildPreview(e);

    ImGui::Separator();
    // Scaled, not raw pixels: sizes here predate 300% DPI, where a 240px column becomes ~80 logical
    // pixels, narrow enough that bone names fit only because they're short. GetFontSize() stands in as
    // the DPI proxy since it's already correct here and needs nothing threading through four call sites.
    const f32 uiScale = ImGui::GetFontSize() / 16.0f;

    // The strip's height must be decided here, before the row below is laid out: ImGui is immediate-
    // mode, so the row's BeginChild calls need a height this frame ("total room minus the strip"),
    // even though the strip itself draws after the row, at the bottom. seqSplit_ is a separate
    // SplitPane from split_ (see kDefaultSeqFraction): split_ divides left-to-right, seqSplit_
    // top-to-bottom, and the two must never share a persisted fraction.
    const f32 hTotal = ImGui::GetContentRegionAvail().y;
    const f32 seqHandleH = 6.0f * uiScale;
    // One line plus frame padding, enough for the retract toggle and its label -- this IS "a thin
    // header", the brief's own words: a header-plus-summary-plus-mini-transport would never actually
    // collapse.
    const f32 seqHeaderH = ImGui::GetFrameHeightWithSpacing();
    const f32 minSeqH  = std::max(seqHeaderH + 90.0f * uiScale, 160.0f * uiScale);
    const f32 minMainH = 200.0f * uiScale;

    // A skeleton tab (isClip_ == false) has no transport/timeline to show (both read clip_, which a
    // .ocskel opened directly has none of, gated like drawNotifies/drawCurves), so it gets no strip:
    // the row below takes all of hTotal.
    f32 stripH = 0.0f, seqSplitAvail = 0.0f;
    const bool seqExpanded = isClip_ && !seqCollapsed_;
    if (isClip_) {
        if (seqCollapsed_) {
            stripH = seqHeaderH;
        } else {
            if (seqSplit_.fraction < 0.0f)
                seqSplit_.fraction = loadSplitFraction(kPrefSeqSplit, kDefaultSeqFraction);
            seqSplitAvail = std::max(0.0f, hTotal - seqHandleH);
            stripH = clampSplitWidth(splitWidthOf(seqSplit_.fraction, seqSplitAvail), seqSplitAvail,
                                      minSeqH, minMainH);
        }
    }
    const f32 handleH = seqExpanded ? seqHandleH : 0.0f;
    const f32 h = std::max(0.0f, hTotal - stripH - handleH);

    // Wrapped in a child of its own, exactly `h` tall -- fixes a bug latent in drawSplitHandle
    // (EditorWidgets.hpp) that only surfaced here: that helper's splitterHandle sizes its invisible
    // button off GetContentRegionAvail().y, correct only when the row calling it already consumes all
    // remaining window height. This tab is the first to draw something (the sequencer strip) BELOW
    // that same row in the same window, so `h` is deliberately less than `hTotal` -- but
    // splitterHandle doesn't know that, sized itself to the window's real bottom, and grew the
    // SameLine()'d row's line height to match, swallowing the budget set aside for the strip. ActorEditor's
    // own copy of this pattern and BtEditor/SoundEditor/ParticleEditor's shared calls through
    // drawSplitHandle all still give their row the full remaining height (nothing of theirs draws below
    // it), so for them "window bottom" and "row bottom" are the same pixel and this was never visible.
    // Symptom: the three panels drew at correct size, but everything below them was empty background to
    // the status bar, the strip rendered far below the visible tab, and the tab grew a scrollbar with a
    // thumb sized as if its content were roughly double its own height.
    //
    // Fixed here, not in EditorWidgets.hpp (shared by four other tabs whose rows aren't wrong): a
    // dedicated child bounds GetContentRegionAvail().y to `h` for everything inside it, including
    // splitterHandle's call. WindowPadding is zeroed only for this child's own creation (popped before
    // the three panels draw) so panelsRow is an invisible wrapper -- nothing on screen shifts, only
    // GetContentRegionAvail() calls inside it now measure against its bottom instead of the tab's.
    // Same fix applies whether the strip is expanded or collapsed; only `h`'s computation differs.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("panelsRow", ImVec2(0.0f, h), false);
    ImGui::PopStyleVar();
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
    // Draggable, persisted, via SplitPane (see kDefaultViewFraction). `midAvail` is the width
    // remaining after the fixed-width bones/sockets/animations column just drawn -- the same base the
    // original `avail.x * 0.62f` measured, so the default view width is unchanged.
    const f32 midAvail = ImGui::GetContentRegionAvail().x;
    const f32 minView = 200.0f * uiScale, minTracks = 200.0f * uiScale;
    const f32 midW = splitPaneWidth(split_, kPrefViewSplit, kDefaultViewFraction, midAvail,
                                     minView, minTracks);
    if (ImGui::BeginChild("view", ImVec2(midW, h), true)) {
        // What the mesh is shading with, in one line above the picture (same "message above the
        // render" as GraphEditor::drawMaterialViewport) -- a preview that says nothing about itself
        // just looks broken, and an author who never opens the Output Log (or does, and finds nothing
        // there for four of resolvePreviewMaterial's five ways of coming up empty) needs the answer
        // right here. Guarded on materialStatus_ non-empty rather than AVER_MODULE_PBR: without that
        // module materialTried_ never latches, so the string simply stays empty and the line never draws.
        if (!materialStatus_.empty()) {
            // Wrapped, not a single long line: unlike GraphEditor's own short compile-error sentence,
            // some branches spell out every .ocmat path tried, wider than the view column (minView
            // 200px). TextDisabled/TextColored don't wrap on their own, so colour is pushed by hand
            // around TextWrapped instead.
            if (materialFailed_) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.72f, 0.35f, 1.0f));
            else                 ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", materialStatus_.c_str());
            ImGui::PopStyleColor();
        }
        if (preview && preview->uiTextureId()) {
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            // Debounced: resize() waits for the GPU to idle and destroys a texture ImGui may still be
            // sampling, so resizing every frame of a drag would stall the editor.
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

            // Preview chrome (PreviewChrome.hpp): toolbar, stats, axes gizmo. Reasserted onto the
            // preview every frame, since it is shared across every asset tab that has one.
            const ImVec2 imageMin = ImGui::GetItemRectMin(), imageMax = ImGui::GetItemRectMax();
            const PreviewShowItem extraShow[] = {
                {"Bones", &showBones_},
                {"Sockets", &showSockets_},
            };
            drawPreviewToolbar("animPreviewToolbar", imageMin, imageMax, uiScale, viewMode_, showFlags_,
                               extraShow, static_cast<int>(sizeof extraShow / sizeof extraShow[0]));
            preview->setViewMode(viewMode_);
            preview->setShowFlags(showFlags_);

            std::vector<std::string> stats;
            stats.push_back("Bones: " + formatCount(skel_.bones.size()));
            AnimSkinFeature* skin = sharedSkin(e);
            if (skin && showMesh_ && skin->drawMesh()) {
                stats.push_back("Vertices: " + formatCount(skin->vertexCount()));
                stats.push_back("Triangles: " + formatCount(skin->triangleCount()));
            }
            if (isClip_) {
                char lenBuf[32];
                std::snprintf(lenBuf, sizeof lenBuf, "Length: %.2f s", clip_.duration);
                stats.push_back(lenBuf);
                // sampleRate/Frames only mean anything for a baked clip (OcAnim.hpp).
                if (clip_.storage == fmt::OcAnimStorage::BakedUniform && clip_.sampleRate > 0) {
                    stats.push_back("FPS: " + std::to_string(clip_.sampleRate));
                    const u32 frames = static_cast<u32>(std::lround(clip_.duration * clip_.sampleRate));
                    stats.push_back("Frames: " + formatCount(frames));
                }
                stats.push_back("Notifies: " + formatCount(clip_.notifies.size()));
                stats.push_back("Curves: " + formatCount(clip_.curves.size()));
            }
            drawPreviewStats(imageMin, uiScale, stats);
            drawPreviewAxes(imageMin, imageMax, uiScale, preview->camera());

            // Navigation, matched to the main viewport. Gated on hover alone, like the orbit it
            // replaces: fires only while the mouse sits over the preview image, so a drag ending over
            // the sequencer strip below can never also spin/pan/nudge this camera.
            if (ImGui::IsItemHovered()) {
                const ImGuiIO& io = ImGui::GetIO();
                render::preview::PreviewCamera& cam = preview->camera();

                // Left-drag orbits, x inverted: matches ActorEditor.cpp/GraphEditor.cpp (AssetEditor.cpp
                // still uses +) -- the drag moves the world under the cursor, so pushing right swings
                // the view left.
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                    cam.addOrbit(-io.MouseDelta.x * 0.4f, io.MouseDelta.y * 0.4f);
                if (io.MouseWheel != 0.0f)
                    cam.addZoom(io.MouseWheel > 0.0f ? 0.9f : 1.1f);

                // Middle/right-drag pan via camera().panPixels() -- every other preview tab already
                // has this (AssetEditor.cpp, ActorEditor.cpp, GraphEditor.cpp); an orbit camera alone
                // can't bring an off-centre joint to the middle of frame before zooming in.
                for (const ImGuiMouseButton b : {ImGuiMouseButton_Middle, ImGuiMouseButton_Right}) {
                    if (!ImGui::IsMouseDragging(b)) continue;
                    const ImVec2 d = ImGui::GetMouseDragDelta(b);
                    ImGui::ResetMouseDragDelta(b);
                    cam.panPixels(d.x, d.y, static_cast<f32>(preview->height()));
                }

                // WASD/QE move the PIVOT, not an eye position: PreviewCamera is an orbit (yaw/pitch/
                // distance about a pivot, see its declaration), so stepping a camPos_ the way
                // SandboxViewport's fly camera does (SandboxApp.cpp's `flying_` block) would be
                // discarded by the next addOrbit/addZoom recomputing the eye from pivot+distance+yaw+
                // pitch. Moving the pivot along forward/right/world-up keeps orbit/zoom meaning intact
                // while giving WASD/QE recentring along the view axis (panPixels above is limited to
                // the screen-parallel plane). Scaled by the camera's own distance -- fine steps when
                // framed close, fast enough to be worth pressing when framed wide -- same reasoning as
                // frameSelectedBone's formula.
                if (!io.WantCaptureKeyboard) {
                    const f32 yawRad = radians(cam.yawDeg), pitchRad = radians(cam.pitchDeg);
                    const Vec3 fwd = Vec3{std::cos(pitchRad) * std::cos(yawRad),
                                          std::cos(pitchRad) * std::sin(yawRad),
                                          -std::sin(pitchRad)}.getSafeNormal();
                    const Vec3 worldUp{0.0f, 0.0f, 1.0f};
                    const Vec3 right = cross(worldUp, fwd).getSafeNormal();
                    const f32 sp = cam.distance * dt;
                    Vec3 step{0.0f, 0.0f, 0.0f};
                    if (ImGui::IsKeyDown(ImGuiKey_W)) step += fwd * sp;
                    if (ImGui::IsKeyDown(ImGuiKey_S)) step -= fwd * sp;
                    if (ImGui::IsKeyDown(ImGuiKey_D)) step += right * sp;
                    if (ImGui::IsKeyDown(ImGuiKey_A)) step -= right * sp;
                    if (ImGui::IsKeyDown(ImGuiKey_E)) step += worldUp * sp;
                    if (ImGui::IsKeyDown(ImGuiKey_Q)) step -= worldUp * sp;
                    cam.pivot[0] += step.x; cam.pivot[1] += step.y; cam.pivot[2] += step.z;

                    // F frames the selected bone if one is selected, the whole rig otherwise. `false`
                    // (no repeat) so holding F doesn't refire every frame.
                    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
                        if (selectedBone_ >= 0 && static_cast<usize>(selectedBone_) < model_.size()) {
                            frameSelectedBone(*preview, static_cast<usize>(selectedBone_));
                        } else {
                            preview->frameAll();
                            // Same back-off as buildPreview's initial autoframe (see its comment):
                            // frameAll's radius pick, off a thin bone box, would land the camera
                            // inside the rig without this.
                            cam.addZoom(2.6f);
                        }
                    }
                }
            }
        } else {
            ImGui::TextDisabled("No preview on this backend.");
        }
    }
    ImGui::EndChild();
    drawSplitHandle(split_, "##animsplit", kPrefViewSplit, midAvail, minView, minTracks,
                     6.0f * uiScale);

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
    ImGui::EndChild();   // "panelsRow" (see its BeginChild comment); WindowPadding was already
                         // popped right after that BeginChild, nothing further to undo here.

    // Sequencer strip, drawn after the row above so top-to-bottom flow places it where the height
    // budget already reserved. Nothing to draw for a skeleton tab (h already claimed all of hTotal).
    if (isClip_) {
        if (seqExpanded) {
            // Drag handle, only while expanded: a collapsed strip has nothing to resize, and a handle
            // drawn anyway would invite a drag that silently does nothing.
            //
            // Writes straight back into `stripH`, unlike split_'s drawSplitHandle (which only updates
            // its SplitPane for the NEXT frame, since "view" is drawn before it runs). Here the handle
            // runs before the "sequencer" child, so a drag's resize applies this same frame -- the
            // better answer for the one pane in this tab where handle and pane draw in that order.
            bool seqReleased = false;
            verticalSplitterHandle("##seqsplit", seqHandleH, &stripH, seqSplitAvail, minSeqH, minMainH,
                                    &seqReleased);
            seqSplit_.fraction = splitFractionOf(stripH, seqSplitAvail);
            if (seqReleased) storeSplitFraction(kPrefSeqSplit, seqSplit_.fraction);
        }
        if (ImGui::BeginChild("sequencer", ImVec2(0, stripH), true)) {
            // Retract toggle: ICON_EXPAND ("expand_more", a downward chevron) reads as "open, click to
            // close" (like a tree-node arrow), ICON_CHEVRON ("chevron_right") as "closed, click to open".
            if (ImGui::SmallButton(seqCollapsed_ ? ICON_CHEVRON " Sequencer" : ICON_EXPAND " Sequencer")) {
                seqCollapsed_ = !seqCollapsed_;
                // Persisted immediately (storeSplitFraction's "instant a drag ends" rule,
                // EditorWidgets.hpp) so a crash or alt-tab right after this click doesn't silently
                // un-collapse the strip next launch.
                setPrefBool(kPrefSeqCollapsed, seqCollapsed_);
                flushEditorPrefs();
            }
            if (seqExpanded) {
                ImGui::SameLine();
                ImGui::TextDisabled("%.3f s   %zu track(s)", clip_.duration, clip_.tracks.size());
                // Strip's own content, filling what's left after the header row. drawTimeline()'s
                // track-lane child clips further still (its own ImGuiListClipper region), so a short
                // strip grows an inner scrollbar there rather than clipping the transport/scrub bar above.
                if (ImGui::BeginChild("sequencerBody", ImVec2(0, 0), false)) {
                    drawTransport();
                    drawTimeline();
                }
                ImGui::EndChild();
            }
        }
        ImGui::EndChild();
    }
#else
    (void)e;
#endif
}

// Restores the view/tracks split and the sequencer strip to their default proportions, persisting
// both immediately (see AssetEditor.hpp's resetLayout() for why every tab, not just ActorEditor, must
// implement "Reset Tab Layout"). No-op with AVER_WITH_IMGUI off: a headless build never lays the
// panels out, so there's nothing for a reset to restore.
void AnimEditor::resetLayout() {
#if AVER_WITH_IMGUI
    resetSplitPane(split_, kPrefViewSplit, kDefaultViewFraction);
    resetSplitPane(seqSplit_, kPrefSeqSplit, kDefaultSeqFraction);
    // Collapse state resets too -- it's layout state same as either split's fraction; leaving it
    // retracted or half-restored wouldn't be the tab's true default.
    seqCollapsed_ = false;
    setPrefBool(kPrefSeqCollapsed, false);
    flushEditorPrefs();
#endif
}

} // namespace

void setAnimEditorContentRoot(std::string root) { g_contentRoot = std::move(root); }

// Unregisters the skinning feature and frees its GPU resources, before the device goes.
//
// Missing this crashed on exit (access violation after the last frame, screen correct but process
// dead): addRenderFeature holds the feature non-owning, same as ActorEditor's shared preview, so
// something must remove it -- sharedPreview always did; this feature simply hadn't.
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
        // A .ocskel opened directly is its own rig, so the browser lists every clip beside it --
        // Persona's Skeleton editor, via the same panel rather than a second one.
        return std::make_unique<AnimEditor>(path, fmt::OcAnimation{}, std::move(skel), false, path);
    }
    return nullptr;
}

} // namespace aver::editor
