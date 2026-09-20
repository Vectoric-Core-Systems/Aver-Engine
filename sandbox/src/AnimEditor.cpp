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
#include "AnimEdit.hpp"
#include "EditorIcons.hpp"
#include "EditorKeybinds.hpp"
#include "EditorWidgets.hpp"
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

// THE PREVIEW'S OWN MATERIAL RESOLUTION (buildPreview's mesh draw, below): a mesh's slot-0 surface
// name -> its .ocmat -> both the domain graph it names (if any) and the pbr::MaterialHandle for its
// own factors and textures, which is what ActorPreview actually shades the mesh with -- see
// resolvePreviewMaterial's own top comment for why a graph id alone used to leave the mesh white.
//
// GATED ON AVER_MODULE_PBR ALONE, deliberately NOT also nested inside AVER_WITH_IMGUI the way
// imgui.h above is -- even though GraphEditor.cpp nests this exact pair of guards for its own,
// similarly-shaped material preview. The two files differ in one load-bearing way: THIS file already
// includes ActorPreview.hpp/PreviewMeshCache.hpp unconditionally (the block above), so
// render::preview::PreviewDraw is a complete type here regardless of ImGui, and nothing the
// resolution code below touches is an ImGui type either -- its only real dependency is the material
// system. Nesting these includes inside AVER_WITH_IMGUI as well would build resolvePreviewMaterial()
// (guarded only by AVER_MODULE_PBR, since that is genuinely all IT depends on) in any UI-less
// configuration that leaves AVER_MODULE_PBR at its default of ON -- module-matrix.ps1's "no-ui" row
// is exactly that -- with these declarations missing: an undeclared-identifier error the matrix
// exists to catch, in a function that itself has nothing to do with ImGui. Aver.Formats.Material
// (OcMat.hpp) and Aver.Render.PBR (MaterialGraphRegistry.hpp) are still only on Sandbox's include
// path when AVER_MODULE_PBR is on (sandbox/CMakeLists.txt's own `if(TARGET Aver.Render.PBR)` block),
// which is why the #include needs a guard at all -- just this one, matching what the code actually
// needs rather than the file's other, unrelated guard.
#if AVER_MODULE_PBR
// MaterialResolve.hpp is the sandbox-local header that already carries the runtime's own
// candidate-path order (see its own top comment).
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

// The view/tracks split (draw()'s "view" preview column against "tracks", its notify/curve/track
// column to the right) -- see EditorWidgets.hpp's own top comment for why this is a FRACTION
// (SplitPane) rather than ActorEditor's pixel-width convention. 0.62f is this tab's own
// PRE-EXISTING default (it used to be `avail.x * 0.62f` of whatever remained right of the bones
// column, recomputed fresh every frame with no persistence at all) -- kept exactly, so adopting the
// shared helper changes draggability and persistence only. The bones/sockets/animations column to
// its left stays a fixed width, untouched -- see draw()'s own comment where it is sized.
constexpr f32 kDefaultViewFraction = 0.62f;
constexpr const char* kPrefViewSplit = "animEditor.viewSplit";

// THE SEQUENCER STRIP'S OWN SPLIT, along the BOTTOM of the tab rather than side-by-side with
// anything -- see draw()'s own layout comment for why the transport and the timeline lanes moved
// down here out of the vertical flow they used to sit in, between the header and the mesh/bones
// checkboxes. A SEPARATE PREF KEY AND DEFAULT FROM kPrefViewSplit/kDefaultViewFraction just above:
// this is a different divider, between a different pair of panes, and giving it its own key means
// dragging one split can never be read back as a resize of the other. 0.32f leaves a bit under a
// third of the tab's vertical room to the strip by default -- enough for the transport row, the
// master scrub bar, the always-drawn notify lane and a handful of track lanes before drawTimeline()'s
// own internal scrollbar has to take over, without starving the preview above it on an
// ordinary-sized dock.
constexpr f32 kDefaultSeqFraction = 0.32f;
constexpr const char* kPrefSeqSplit = "animEditor.seqSplit";
// Whether the strip is retracted to its own thin header. A SEPARATE KEY FROM THE SPLIT FRACTION
// above, not folded into it as "fraction == 0 means collapsed": a collapsed strip still remembers
// the fraction it will spring back open to, rather than forgetting it the moment it closes.
constexpr const char* kPrefSeqCollapsed = "animEditor.seqCollapsed";

#if AVER_WITH_IMGUI
// The vertical counterpart to EditorWidgets.hpp's own splitterHandle (see that header's own top
// comment for why the view/tracks split just above already shares ONE horizontal-divider convention,
// through SplitPane, across every asset tab that has one). That helper drags a WIDTH between two
// side-by-side children: it reads MouseDelta.x, sets the East-West resize cursor, and calls
// ImGui::SameLine() so the second pane lands on the same row as the first. None of that is right for
// a pane stacked BELOW another one, which is the shape this editor's own drawer already uses at the
// app level and the shape the sequencer strip needs: this reads MouseDelta.y, sets the North-South
// resize cursor, and touches no cursor position at all, because ImGui's ordinary top-to-bottom flow
// already puts the next child exactly where a vertical split wants it.
//
// KEPT LOCAL TO THIS FILE rather than folded into EditorWidgets.hpp as a second orientation for
// splitterHandle: this task's brief is this file alone, and a second orientation for a helper other
// tabs already share is exactly the kind of change that wants its own review rather than one folded
// in behind an unrelated fix.
//
// `*heightPx` IS THE BOTTOM PANE'S OWN HEIGHT (the strip), so dragging the handle UP hands the strip
// MORE room: `-= MouseDelta.y`, the opposite sign from splitterHandle's `+= MouseDelta.x`, because
// here the pane whose size this changes sits on the far side of the handle from where that
// convention's `+=` already reads correctly.
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
    // The mesh's OWN slot-0 surface name (OcMeshData::materialSlots[0]), or empty when the mesh
    // named none. This is the one fact buildPreview's material resolution needs and the only reason
    // it still has the decoded OcMeshData around by the time bind() returns -- see bind()'s own
    // comment on why that struct is built here rather than borrowed from PreviewMeshCache.
    const std::string& slot0Material() const { return slot0Material_; }

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
        // CLEARED HERE, not only set on success: a rebind that fails partway (no skin streams, a
        // truncated file) must not leave a stale name from whatever the PREVIOUS mesh was, which
        // buildPreview's caller-side cache would otherwise happily keep resolving a material for.
        slot0Material_.clear();
        if (meshPath.empty() || boneCount == 0) return false;

        fmt::OcMeshData md;
        std::string why;
        if (!fmt::loadOcMesh(meshPath, md, &why)) {
            AVER_WARN("[AnimEditor] {}", why);
            return false;
        }
        // TAKEN REGARDLESS OF hasSkin() BELOW: a mesh with no skin streams still falls back to bone
        // boxes and never reaches buildPreview's mesh-draw branch, so an empty name here is harmless --
        // but reading it NOW, while `md` is already in hand, is simpler than re-opening the file later
        // just for this one field.
        if (!md.materialSlots.empty()) slot0Material_ = md.materialSlots[0];
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
    std::string             slot0Material_;   // OcMeshData::materialSlots[0] of the bound mesh, or ""
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

// THE BONE PALETTE, minus one hue. computeBonePalette (AnimEdit.hpp) spreads chain hues across the
// whole wheel by the golden angle precisely so no hue is reserved -- which means, left alone, a chain
// could perfectly well land on the same cyan the socket markers use a few lines below in buildPreview.
// Cyan there means one specific thing across this whole editor -- "an author placed this" -- and a
// bone that happened to hash to the same colour would say that about a joint that is not a socket.
// So this reimplements boneColorForChain's own arithmetic (not computeBonePalette itself: rejecting
// its answer after the fact would still need this same golden-angle formula to compute a
// replacement, so there is nothing saved by calling it first) with one change: a hue landing inside
// the excluded band is pushed to the band's far edge instead. Widened well past the socket's own hue
// (worked out by hand from its {0.18, 0.72, 0.95} baseColor: max=B, L=0.565, hue≈198°) because the
// band only has to dodge the HUE -- the palette's lightness cycle (kBoneLightLow..kBoneLightHigh)
// still crosses the socket's own lightness at some depth in any chain that lands near that hue, so a
// narrow band would just trade an exact collision for a near one.
constexpr f32 kSocketHueDeg = 198.0f;
constexpr f32 kSocketHueHalfWidthDeg = 28.0f;

BoneColor boneColorAvoidingSocketCyan(u32 chainRootBoneIndex, u32 depthInChain) {
    f32 hue = std::fmod(static_cast<f32>(chainRootBoneIndex) * kBoneHueGoldenTurns, 1.0f) * 360.0f;
    const f32 lo = kSocketHueDeg - kSocketHueHalfWidthDeg;
    const f32 hi = kSocketHueDeg + kSocketHueHalfWidthDeg;
    if (hue >= lo && hue <= hi) hue = std::fmod(hue + (hi - lo), 360.0f);

    // IDENTICAL TO boneColorForChain BELOW THIS LINE -- see AnimEdit.hpp's own comment on the
    // triangle-wave lightness cycle and why it does not simply ramp with depth.
    const u32 phase = depthInChain % (2 * kBoneLightPeriod);
    const u32 folded = phase <= kBoneLightPeriod ? phase : (2 * kBoneLightPeriod - phase);
    const f32 frac = static_cast<f32>(folded) / static_cast<f32>(kBoneLightPeriod);
    const f32 lightness = kBoneLightLow + (kBoneLightHigh - kBoneLightLow) * frac;
    return hslToBoneColor(hue, kBoneSaturation, lightness);
}

// The .cpp-side equivalent of computeBonePalette: one entry per bone, childCounts built once and
// shared across the walk -- see that function's own comment for why this shape (rather than calling
// boneColorFor per bone) is the one to use in a loop. The only difference from the header's own
// version is the substitution above.
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
// Everything one call to resolvePreviewMaterial (below) hands buildPreview: the compiled graph id
// PreviewDraw::materialGraphId wants, the REAL pbr::MaterialHandle PreviewDraw::materialHandle wants
// (see that field's own comment for why the preview shades white without one), and a single sentence
// -- always set, never empty -- for what actually happened. That sentence is cheap enough to both
// log once (buildPreview's own materialTried_ latch is what makes "once" true) and paint straight
// into the tab, which is the whole point of it existing as a field rather than living only in a log
// line nobody thought to go looking for.
struct PreviewMaterialResolution {
    u32 graphId = 0;                 // pbr::materialGraphs() id, or 0 = no graph runs on top
    pbr::MaterialHandle handle = 0;  // the surface's OWN textures/factors, or 0 = the preview's stock identity set
    std::string status;              // one sentence, safe to print verbatim in the tab
    bool failed = false;             // a genuine problem -- see the no-GRAPHREF branch for the one case this stays false despite graphId being 0
};

// Turns a mesh's slot-0 surface NAME into everything the preview seam wants to shade with. This
// REPLACES the narrower resolvePreviewMaterialGraph this used to be: that function answered only
// "which graph", discarding the pbr::MaterialDesc loadOcmat() had already parsed on the way there --
// which is exactly the factors and textures PreviewDraw::materialHandle exists to carry. Throwing
// them away is why a material with no graph at all (most of them; see the no-GRAPHREF branch below)
// still drew ActorPreview's white identity textures no matter how correctly the graph half resolved.
// This answers both questions from the one .ocmat parse instead of discarding half of it.
//
// `binariesDir` may be "" (see this function's one call site's own comment on how it is derived) --
// resolveMaterialPath already tolerates that, it just never matches the Binaries candidate.
//
// NOT GameContent::resolveMaterialGraph/materialForSurface (Runtime/src/GameContent.cpp): those
// methods are PRIVATE to a class this editor has no instance of and no business reaching into (see
// this function's own reuse-by-name paragraph below for what that costs), and the two hosts resolve
// a NAME to a FILE differently besides -- the runtime only ever tries Binaries\Materials, by a name a
// level already recorded; the editor also accepts a hand-authored .ocmat straight under
// Content\Materials (MaterialResolve.hpp's three-candidate order, the same one the Content Browser
// and SandboxAssets.cpp already share), which is the common case for a rig an artist just imported
// and has not yet run through avermatc.
//
// FIVE WAYS THIS CAN COME BACK UNABLE TO SHADE WITH ANYTHING REAL, and before this function existed
// only the last one said why: an empty surface name, an empty content root, no .ocmat found by that
// name, an .ocmat that failed to parse, and a GRAPHREF naming a graph that will not load or compile.
// The four genuine failures among them (all but the next paragraph's) each fill `status` with what
// they were looking for -- the name, the paths tried, the graph it could not read -- and log it once
// through AVER_WARN.
//
// AN .ocmat WITH NO GRAPHREF IS NOT ONE OF THE FOUR FAILURES, on purpose: most materials that exist
// are exactly this -- factors and textures, no graph at all -- so `failed` stays false, `graphId`
// stays 0, and `handle` is still the real material's, which is what makes the mesh shade with its
// authored look instead of the preview's own identity textures. Reporting the ordinary case as a
// problem would be exactly the kind of cried-wolf log line that made the ACTUAL failure (a mesh that
// stayed white no matter what this function returned) impossible to tell apart from business as usual.
//
// pbr::MaterialLibrary IS A PROCESS-GLOBAL SINGLETON, so a handle for this exact surface may already
// exist -- created by SandboxApp's own GameContent when the open level placed something wearing it
// (GameContent::materialForSurface), or by another tab that resolved this same name first. This
// editor cannot reach GameContent's own name -> handle cache to ask it directly (content_ is a
// private SandboxApp field, and an AssetEditor tab holds no SandboxApp reference), so it scans the
// library's LIVE materials by NAME instead -- count()/at()/desc() are the only names MaterialLibrary
// itself exposes, and an editor session rarely has more than a few dozen materials live, so the scan
// is cheap, especially run only once per attempt rather than per frame. A NAME MATCH IS A HEURISTIC,
// not a guarantee: two different .ocmat files that both leave NAME unauthored take it from their own
// file stem (loadOcmat's own documented default), so a collision would need two different stems to
// somehow share one authored NAME record -- the same trust MaterialLibrary itself already places in
// `name` being distinctive enough to show in a tooltip. Nothing found means this really can be the
// first thing in the process to touch this surface -- a rig opened in a project whose level never
// placed anything wearing its mesh's material -- so this loads and creates it itself: the same
// three-candidate-path parse GameContent::materialForSurface does, minus that class's own per-
// instance cache, which is the thing this whole reuse-by-name scan stands in for.
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

    // REUSE FIRST -- see this function's own comment above on why a name scan stands in for
    // GameContent's own cache, which this editor cannot reach.
    for (u32 i = 0, n = pbr::MaterialLibrary::get().count(); i < n && !r.handle; ++i) {
        const pbr::MaterialHandle h = pbr::MaterialLibrary::get().at(i);
        if (const pbr::MaterialDesc* live = pbr::MaterialLibrary::get().desc(h))
            if (live->name == desc.name) r.handle = h;
    }

    if (extras.graphRef.empty()) {
        // AN ORDINARY MATERIAL -- see this function's own top comment for why this is not one of
        // the four failures: most materials that exist are exactly this, factors and textures with
        // no graph at all.
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

    // CONTENT-RELATIVE, exactly as GameContent::resolveMaterialGraph reads it -- see
    // OcMatExtras::graphRef's own comment for why the stored path never carries the content
    // directory on the front.
    std::string graphPath = contentDir + "\\" + extras.graphRef;
    for (char& c : graphPath) if (c == '/') c = '\\';

    // idOf() FIRST, so a graph another material or another editor tab already compiled is reused
    // rather than recompiled (ids are stable for the process -- see MaterialGraphRegistry's own top
    // comment).
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
            // add() ITSELF LOGS AND RETURNS 0 when the graph reads fine but does not compile
            // (MaterialGraphRegistry::add's own comment) -- nothing here needs to repeat that.
            graphId = pbr::materialGraphs().add(graphPath, g.name, g);
            if (!graphId) {
                graphBroken = true;
                r.status = "material '" + name + "' names graph '" + extras.graphRef +
                           "' but it did not compile; see the log above";
            }
        }
    }

    // A BROKEN GRAPH DOES NOT TAKE THE WHOLE MATERIAL DOWN WITH IT -- GameContent::materialForSurface's
    // own comment states the identical rule for the runtime's copy of this same load. graphId stays 0
    // (ActorPreview's `default: break` arm), but the material's own factors and textures still reach
    // the mesh through `handle` below, exactly as an ordinary graph-less material would.
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

    // Restores the view/tracks split to its default proportion and persists that -- see
    // EditorWidgets.hpp's own comment for why this tab's split is a FRACTION (SplitPane) rather than
    // ActorEditor's pixel-width convention. Only the view/tracks boundary: the bones/sockets/
    // animations column to its left is a fixed width, not the fraction-of-content-region pattern this
    // helper was generalised from, and its own internal vertical splits are untouched -- see this
    // file's own draw() for both.
    void resetLayout() override;

    // Snapshot undo, through the shared SnapshotUndo<State> template (SnapshotUndo.hpp) -- see
    // pushUndo()'s own .cpp comment for what each of the two stacks holds and why there are two.
    void pushUndo();
    void undo();
    void redo();
    bool canUndo() const { return isClip_ ? clipHistory_.canUndo() : socketHistory_.canUndo(); }
    bool canRedo() const { return isClip_ ? clipHistory_.canRedo() : socketHistory_.canRedo(); }

private:
    // Snapshot undo state. TWO STACKS, not one: this tab edits two different assets depending on
    // isClip_ -- a clip's notifies/notifyDurations/curves/flags on a .ocanim, or a rig's sockets on a
    // .ocskel -- and they save() to two different files. A single shared stack would let an Undo
    // pressed while looking at one asset pop an entry that was really a snapshot of the OTHER one,
    // overwriting whichever is on screen with a value that was never part of it -- see pushUndo()'s
    // own comment for the full reasoning.
    //
    // ONLY THE FIELDS THIS EDITOR CAN WRITE are snapshotted, not a whole clip or a whole skeleton.
    // skel_.bones is STILL read-only here, for the reason ParticleEditor.hpp's own comment on
    // AnimEditor originally gave for the whole struct -- selectedBone_ picks one; nothing on this tab
    // moves one -- so it stays out of both stacks.
    //
    // clip_.tracks AND clip_.duration ARE NOW IN HERE, which that same original comment said they
    // never needed to be: "copying that array on a key drag this editor never touches would be a cost
    // with nothing behind it" was true right up until this editor grew the ability to touch it. A key
    // move, insert or delete, and a clip-wide retime/scale/trim, all mutate `tracks` (and the last two
    // also mutate `duration`), so an Undo that left them out would put back every notify, curve and
    // flag an edit touched while leaving the actual key or timing change on screen -- exactly the
    // "restores something the author was not editing" failure pushUndo()'s own comment warns about,
    // just inverted: here it would be an edit that DOES touch tracks/duration but pushes a stack that
    // doesn't carry them. The cost this reintroduces -- a full copy of every sampled key on every drag
    // start -- is the same trade GraphEditor's own whole-graph snapshot already makes; there is no
    // cheaper correct answer once the thing being dragged is IN the snapshot's blast radius.
    //
    // sampleRate IS ALSO HERE for the same reason as duration: scaleClipDuration (AnimEdit.hpp) writes
    // it for a BakedUniform clip, and an Undo that restored `duration` and every track's times but left
    // sampleRate at its post-scale value would hand a later save() a clip whose baked rate no longer
    // matches its own duration -- a field silently wrong in a way nothing on screen shows, until
    // something re-derives key times from it.
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
    // Copies an undo/redo result back onto clip_ or skel_.sockets, clamps whichever selection index
    // that mode owns, and re-syncs its text-edit buffer -- ParticleEditor's own syncEditBuffers()
    // reason: a buffer left holding the pre-undo name would show text that disagrees with the row
    // underneath it until the author happened to retype it.
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
    // THE THREE WHOLE-CLIP RETIME OPERATIONS (AnimEdit.hpp's scaleClipDuration/shiftClipTime/
    // trimClip), as one small toolbar rather than three. Split out of drawTracks() -- which is where
    // it is drawn, at the top of that panel -- because it edits the CLIP, not a track, and giving it
    // its own function keeps that distinction visible in the diff rather than folding a third kind of
    // edit into a function whose name still says "tracks".
    void drawClipRetime();
    // A track's sampled value at the playhead, formatted per enabled channel -- see its own comment
    // for why this reads pose_ instead of calling trackSampleAt() a second time.
    std::string trackValueLabel(const fmt::OcTrack& t) const;
    // The selected key's time and value-component editors, plus Insert/Delete for the selected
    // track -- see its own comment for why this is not folded into drawTracks() itself.
    void drawTrackKeyEditor();
    // Bounds-clamps selectedTrack_/selectedTrackKey_ against clip_.tracks as it stands right now --
    // see its own comment for the one case (a trim) that needs this outside of undo/redo.
    void clampTrackKeySelection();
    // ITEM 7.2: the 2D curve canvas -- draggable keys and draggable tangent handles. Split out of
    // drawCurves() because it is the one part of that panel with real geometry to get right (see its
    // own header comment for the ImGui-free math it calls into and what is and is not tested).
    void drawCurveWidget(fmt::OcCurve& c);
    void buildPreview(Engine& e);
    void reloadIfNeeded();
    // THE VIEWPORT'S "F" GESTURE WITH A BONE SELECTED: frames that one bone rather than
    // ActorPreview::frameAll()'s whole-draw-list bounds -- see its own .cpp comment for why this
    // needs a little geometry of its own rather than reusing frameAll(), which has no notion of
    // "just this one draw" and nothing else in PreviewCamera (addOrbit/addZoom/panPixels, see its
    // own declaration) that could stand in for it.
    void frameSelectedBone(render::preview::ActorPreview& preview, usize boneIndex);

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
    // CURVE WIDGET DRAG STATE. Set once, at the moment the mouse goes down on the canvas (see
    // drawCurveWidget), and held for the rest of the drag so a fast mouse movement that strays out of
    // a key's hit radius mid-drag does not drop it -- the same press-time-hit-test-then-hold shape
    // GraphEditor.cpp's own DragMode state machine uses for moving nodes.
    CurveHitKind curveDragKind_ = CurveHitKind::None;
    usize curveDragKeyIndex_ = 0;

    // TRACK/KEY SELECTION, shared between drawTimeline()'s per-track lanes (where a key is clicked
    // and dragged) and drawTracks()'s detail panel (where the same key's exact time and value
    // components are typed) -- one selection, read and written from both places, so clicking a key in
    // the timeline and then typing its time in the table edit the SAME key rather than two the author
    // has to keep in sync by hand.
    int   selectedTrack_ = -1;                      // index into clip_.tracks, or -1 for none
    usize selectedTrackKey_ = kInvalidKeyIndex;      // index into that track's times/values, or none
    // Set at the moment the mouse goes down on a key in a lane, held for the rest of the drag --
    // the same press-time-hit-test-then-hold shape curveDragKind_ above already uses, collapsed to a
    // single bool because a track lane has exactly one draggable thing (the key itself), never a
    // tangent handle: see AnimEdit.hpp's own note that this file does not build a handle UI for
    // track tangents, only for curves.
    bool  trackKeyDragging_ = false;

    // RETIME TOOLBAR STATE (drawClipRetime()). Plain session-only fields, never saved and never
    // undone themselves -- they hold whatever the author last TYPED, not a fact about the clip, so an
    // Undo after clicking Apply should put the clip back, not these boxes.
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
    // The bone boxes are an OVERLAY now, not the picture -- and, since buildPreview's first bind,
    // shown BY DEFAULT alongside a mesh rather than only in its absence. See buildPreview's own
    // comment on why the earlier "mesh present -> bones off" default was wrong; both checkboxes
    // below (draw(), the "Mesh"/"Bones" pair) still let an author turn either one off by hand.
    bool showBones_ = false;
    bool showMesh_ = true;
    bool skinBound_ = false;
    std::string meshPath_;         // <rig>.ocmesh beside the skeleton, if there is one
    bool framed_ = false;
    u32 pendingW_ = 0, pendingH_ = 0;
    f64 resizeDue_ = 0.0;
    // ONE COLOUR PER BONE (AnimEdit.hpp's palette, minus the socket-cyan band -- see
    // boneColorAvoidingSocketCyan above), rebuilt only when the skeleton changes size. Sized 0 by
    // default, which buildPreview's own size check treats as "not built yet" the same way it treats
    // a genuine skeleton reload -- see reloadIfNeeded(), which clears this outright on one.
    std::vector<BoneColor> bonePalette_;
    // THE PREVIEW MESH'S MATERIAL, resolved from its slot-0 surface name and cached rather than
    // re-resolved every frame -- see buildPreview's own comment for what re-running this per frame
    // would cost. Plain fields, not gated on AVER_MODULE_PBR, matching GraphEditor's own
    // materialPreviewGraphId_/materialPreviewDirtyMark_ (GraphEditor.hpp): only the CODE that fills
    // them needs the guard, not their existence, so this tab's fields keep compiling either way.
    u32 materialGraphId_ = 0;        // pbr::materialGraphs() id, or 0 = draw the stock pale colour
    // THE MATERIAL'S OWN TEXTURES AND FACTORS, orthogonal to materialGraphId_ just above -- see
    // PreviewDraw::materialHandle's own comment for why a real handle is what actually fixes a mesh
    // that samples white: materialGraphId_ alone selects a GRAPH, but the fixed key-light-plus-fill
    // and stock shaders both still read from ActorPreview's own identity textures (white base
    // colour, flat normal, full roughness) until a real pbr::MaterialHandle says otherwise.
    u32 materialHandle_ = 0;         // a pbr::MaterialHandle, or 0 = the preview's stock identity textures
    // Set while no content root has been seen yet, so the FIRST resolution that can actually
    // succeed re-arms the latch above rather than inheriting a verdict reached without a root.
    bool materialRootPending_ = true;
    std::string materialTriedFor_;   // the slot-0 name materialGraphId_/materialHandle_ were resolved for
    bool materialTried_ = false;     // has ANY attempt (success or failure) been made yet
    // WHAT THE LAST ATTEMPT ACTUALLY FOUND, IN ONE SENTENCE -- always non-empty once materialTried_
    // is true, and safe to print verbatim: draw() paints this straight into the preview panel (see
    // its own comment there) so a white or grey mesh is never a silent mystery with nothing but an
    // Output Log line -- which used to not even exist for four of resolvePreviewMaterial's five ways
    // of coming back with nothing to shade with -- to explain it.
    std::string materialStatus_;
    // Whether materialStatus_ describes a genuine problem (drawn as a warning) rather than an
    // ordinary outcome (an authored material with no graph is NOT a problem -- see
    // resolvePreviewMaterial's own comment on why that case leaves this false).
    bool materialFailed_ = false;

    // The view/tracks divider. A plain SplitPane (EditorWidgets.hpp), not gated on AVER_WITH_IMGUI,
    // matching every plain-POD field above it: this tab's fields must keep compiling with no ImGui
    // even though only draw() and resetLayout() actually touch it.
    SplitPane split_;

    // THE SEQUENCER STRIP (drawTransport() + drawTimeline(), now a pane of its own along the BOTTOM
    // of the tab rather than inline partway down it -- see draw()'s own comment for the reason).
    // A SEPARATE SplitPane FROM split_ ABOVE, not a second use of it: split_ divides the bones/view/
    // tracks ROW left-to-right, this divides that whole row from the strip top-to-bottom, and the two
    // dividers must never read or write each other's persisted fraction. resetLayout() resets both.
    SplitPane seqSplit_;
    // Read ONCE, at construction, rather than through SplitPane's lazy `fraction < 0` sentinel: a
    // bool has no unused value to overload as "not loaded yet" the way a negative fraction already
    // does for a float, and nothing after construction ever needs to re-read the preference -- only
    // to write it back, which the toggle below does directly.
    bool seqCollapsed_ = prefBool(kPrefSeqCollapsed, false);
};

// Pushes the current state of whichever asset isClip_ says is being edited -- see the header's own
// ClipUndoState comment for the two shapes and why there are two stacks rather than one. Every edit
// site below calls this immediately before the mutation it guards, the same "push, then apply" order
// every other asset editor's own pushUndo() call sites use.
//
// DISPATCHES ON isClip_ RATHER THAN TAKING A MODE ARGUMENT, because every call site already only runs
// while its own mode is active: drawNotifies(), drawCurves() and the flag checkboxes all early-return
// unless isClip_, and drawSockets()' own editable half runs only when it is not -- so the stack this
// picks always agrees with the edit it is about to record.
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
    // SAME RULE FOR THE TRACK/KEY SELECTION undo just added a stake in: `tracks` can be a different
    // length after an Undo/Redo than it was before it, and clampTrackKeySelection() is the one place
    // (shared with drawClipRetime()'s trim, the only live EDIT that can also shrink a track's key
    // count) that re-derives whether selectedTrack_/selectedTrackKey_ still point at something real.
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
        // A RELOAD REPLACES clip_ WHOLE, so any undo entry recorded against the old content would
        // restore data that no longer belongs to what is on screen -- cleared for the same reason
        // openClip() below clears it when the author switches to a different clip file entirely.
        if (fmt::loadOcAnim(path_, c, &why)) { clip_ = std::move(c); clipHistory_.clear(); }
        else AVER_WARN("[AnimEditor] {}", why);
    } else {
        fmt::OcSkeleton s;
        // bonePalette_ IS CLEARED HERE, not left for buildPreview's size check to catch: a reloaded
        // rig that happens to keep the same bone count would otherwise pass that check and go on
        // showing colours computed against whatever the PREVIOUS hierarchy's forks were.
        if (fmt::loadOcSkel(path_, s, &why)) { skel_ = std::move(s); socketHistory_.clear(); bonePalette_.clear(); }
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
    // A DIFFERENT CLIP, not an edit to this one -- clipHistory_'s entries described the clip just
    // replaced, and an Undo reaching past this point would restore ITS notifies/curves/flags onto
    // the one now on screen. See pushUndo()'s own comment for why the two assets this tab can open
    // never share a stack; a clip swapped for another of the same kind gets the same treatment.
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
        // USED TO BE `showBones_ = !showMesh_` -- bones went off the instant a mesh bound, on the
        // reasoning that the skinned surface already shows what the rig is doing and a redundant
        // box-per-bone overlay was clutter by default. That reasoning does not survive what the
        // overlay is actually FOR: it is how an author tells which bone a selected track belongs to,
        // spots a broken chain, and -- now that every chain has its own colour (see the palette
        // rebuild below) -- reads the rig's structure at a glance, none of which the skinned surface
        // shows by itself. A rig with a mesh now defaults to showing both, same as a rig with none
        // always has; the "Mesh"/"Bones" checkboxes in draw() still let an author turn either off.
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
        // THE STOCK PALE FALLBACK. Left set unconditionally -- not only when no material resolves --
        // because a mesh with no slot-0 name, or one whose graph will not compile, must still draw
        // rather than vanish (materialGraphId simply stays 0 below and ActorPreview falls back to
        // its own simple key-light-plus-fill shader, which is exactly this colour). Refusing to draw
        // a mesh over a broken material would turn a shading problem into what looks like a missing
        // asset -- the same reasoning MaterialGraphRegistry::add's own comment gives for returning 0
        // on a graph that fails to compile rather than failing the whole material.
        d.baseColor[0] = 0.78f; d.baseColor[1] = 0.76f; d.baseColor[2] = 0.72f;
        const Mat4 id = Mat4::identity();
        std::memcpy(d.world, &id.m[0][0], sizeof d.world);
#if AVER_MODULE_PBR
        // RESOLVED ONCE PER SURFACE NAME, NOT ONCE PER FRAME. resolvePreviewMaterial does at least
        // one filesystem probe (resolveMaterialPath) and, on a cold name, a parse of the .ocmat and
        // its graph, plus a scan of every live pbr::MaterialLibrary entry -- paying any of that every
        // frame would be the "compile a graph per frame" cost the task that added this explicitly
        // ruled out. materialTried_ latches even a FAILED lookup (no .ocmat, a parse error, a graph
        // that will not compile) for the same reason: a name that never resolves must not be retried
        // sixty times a second either. Re-armed only when the mesh's own slot-0 name actually
        // changes -- which, since skin->bind() above only ever runs once per tab (skinBound_
        // latches), in practice means "resolved exactly once".
        //
        // A MISSING CONTENT ROOT IS NOT A FAILED LOOKUP, and latching it as one was a real bug that
        // survived a build and a run. setAnimEditorContentRoot is called from applyProject
        // (SandboxProject.cpp), so a tab opened by `--open-asset` draws its first frames BEFORE any
        // root is known -- and the latch above then recorded "this material does not resolve" for
        // the rest of the session, from a lookup that never had anywhere to look. The mesh stayed
        // white with the content root sitting right there one frame later.
        //
        // So the latch distinguishes TERMINAL from TRANSIENT: no .ocmat, a parse error, a graph that
        // will not compile are all answers about the material and are latched, because a name that
        // never resolves must not be retried sixty times a second. An empty g_contentRoot is an
        // answer about the EDITOR's state instead, it changes without the material changing, and it
        // is the one case worth asking again about.
        const std::string& slot0 = skin->slot0Material();
        const bool haveRoot = !g_contentRoot.empty();
        if (haveRoot && (!materialTried_ || materialTriedFor_ != slot0 || materialRootPending_)) {
            materialTriedFor_ = slot0;
            materialTried_ = true;
            materialRootPending_ = false;
            // BINARIES DIR, DERIVED RATHER THAN PLUMBED IN: this tab only ever receives a content
            // root (setAnimEditorContentRoot, called from SandboxProject.cpp with project_.
            // contentDir()), and adding a second setter for the binaries root is a change to a file
            // outside this one. OcProject::contentDir() is `dir + "\" + contentRoot` and
            // ::binariesDir() is `dir + "\Binaries"` (OcProject.hpp) -- both built from the SAME
            // `dir`, one path segment apart -- so taking the parent of the content root recovers
            // `dir` exactly whenever contentRoot (default "Content") is a single path segment, which
            // it is for every project this tree ships. A contentRoot nested in a subdirectory would
            // make this guess wrong; the failure mode is only that the Binaries\Materials candidate
            // never matches, which resolveMaterialPath already tolerates by falling through to its
            // next candidate, not a crash or a wrong material.
            const std::string binariesDir =
                std::filesystem::path(g_contentRoot).parent_path().string() + "\\Binaries";
            const PreviewMaterialResolution res = resolvePreviewMaterial(binariesDir, g_contentRoot, slot0);
            materialGraphId_ = res.graphId;
            materialHandle_ = res.handle;
            materialStatus_ = res.status;
            materialFailed_ = res.failed;
        }
        d.materialGraphId = materialGraphId_;
        // THE FIX ITSELF: a non-zero handle makes averStockAuthored (ActorPreview.cpp's material
        // pipeline) sample this material's OWN base-colour/normal/roughness textures and factors
        // instead of the preview's white/flat/full-rough identity set -- see
        // PreviewDraw::materialHandle's own comment for the two fields' orthogonality and
        // ActorPreview.cpp's prePass for exactly how a non-zero handle changes which binding set and
        // constants get bound.
        d.materialHandle = materialHandle_;
#endif
        draws.push_back(d);
    }

    // REBUILT ONLY WHEN THE SKELETON CHANGED SIZE -- a genuine reload (a different bone count, or
    // the same count after reloadIfNeeded() clears this outright, see its own comment) -- not every
    // frame. computeAnimEditorBonePalette is O(bone count) TOTAL (childCounts built once, shared
    // across the walk), so this guard is about not repeating that walk sixty times a second for a
    // rig that never changes, not about the walk itself being expensive.
    if (bonePalette_.size() != skel_.bones.size()) bonePalette_ = computeAnimEditorBonePalette(skel_);

    for (usize i = 0; showBones_ && i < model_.size(); ++i) {
        const Vec3 here{model_[i].m[3][0], model_[i].m[3][1], model_[i].m[3][2]};
        const i32 parent = skel_.bones[i].parent;
        render::preview::PreviewDraw d;
        d.mesh = cube;
        d.boundsRadius = radius;
        d.roughness = 0.55f;
        d.selected = static_cast<int>(i) == selectedBone_;
        // EVERY CHAIN GETS ITS OWN HUE (computeAnimEditorBonePalette above), so an author can tell a
        // limb apart from its neighbour -- left arm from right, a finger from the chain it forks off
        // of -- without reading a single bone name. This REPLACES the old "root is pale" special
        // case: a skeleton's true root is chain-root-of-itself at depth 0, which the palette already
        // renders at its own darkest step (kBoneLightLow), so nothing distinct is lost by dropping
        // the special case, and a rig with several disconnected roots no longer paints them all the
        // same washed-out grey.
        if (i < bonePalette_.size()) {
            const BoneColor& c = bonePalette_[i];
            d.baseColor[0] = c.r; d.baseColor[1] = c.g; d.baseColor[2] = c.b;
        }
        // SELECTION STILL HAS TO WIN against a rig that is now colourful everywhere, not just against
        // the old flat neutral grey -- so dropping the amber highlight in favour of "the palette
        // colour, brightened" was not good enough: the golden-angle spread reserves no hue, so SOME
        // chain can legitimately land close to whatever hue "brightened" would produce, and picking a
        // fixed accent hue has the identical problem (see the socket-cyan comment above
        // boneColorAvoidingSocketCyan for the same reasoning applied to a different colour). What the
        // palette CANNOT produce, by construction, is a lightness above kBoneLightHigh (0.64) at any
        // hue or any depth -- so a highlight paler than that reads as "brighter than any rig colour"
        // regardless of which bone, or which chain, is selected. Kept warm (amber-ish) rather than
        // pure white to match the "selected" language this same file already uses for a socket.
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
// ITEM 7.2: A REAL 2D CURVE WIDGET, not numbers with a shape drawn over the timeline bar. That used
// to be the honest limit here -- the format stored no tangents, so a tangent handle would have had
// nothing to write to (see OcCurve::inTangents/outTangents' own comment for the CTAN chunk that
// closed that gap). The canvas itself is drawCurveWidget, below; this panel still keeps the numeric
// key rows underneath it for precise entry, the same way Unreal's own Curve Editor keeps a details
// panel beside its graph.
void AnimEditor::drawCurves() {
#if AVER_WITH_IMGUI
    if (!isClip_) { ImGui::TextDisabled("a skeleton has no curves"); return; }

    if (ImGui::SmallButton("Add curve")) {
        pushUndo();
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
                } else if (!next.empty() && next != c.name) { pushUndo(); c.name = next; dirty_ = true; }
            }

            // CUBICSPLINE IS OFFERED NOW that a curve has somewhere to keep tangents (CTAN). Picking
            // it for the first time gives every existing key a FLAT (zero) tangent pair to drag from
            // -- see OcCurve::inTangents' own comment on why a zero-filled array and an absent one are
            // indistinguishable on disk until a handle actually moves, which is exactly what makes
            // this safe to do unconditionally rather than only when the arrays are still empty.
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

            // WHETHER THIS CURVE CARRIES TANGENTS AT ALL, decided once per frame here so both the
            // "add key" and "delete key" edits below and the canvas can agree on it without each
            // re-deriving the same size comparison.
            const bool hasTangents = c.inTangents.size() == c.times.size() && c.outTangents.size() == c.times.size();

            if (ImGui::SmallButton("Add key at playhead")) {
                pushUndo();
                // INSERTED IN TIME ORDER. The sampler binary-searches `times`, so an out-of-order key
                // does not merely look odd in the list -- it makes every lookup past it wrong.
                usize at = 0;
                while (at < c.times.size() && c.times[at] < time_) ++at;
                c.times.insert(c.times.begin() + static_cast<isize>(at), time_);
                c.values.insert(c.values.begin() + static_cast<isize>(at), anim::sampleCurve(c, time_));
                // KEPT PARALLEL: a new key starts flat (zero tangent both sides) if this curve already
                // carries tangent data at all -- see OcCurve::inTangents' own comment on why the two
                // arrays must stay exactly times.size() long or not exist at all.
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

            // THE CANVAS. See drawCurveWidget's own header comment for what it draws, what it lets an
            // author drag, and -- the honest limit stated plainly rather than glossed -- that the drag
            // interaction itself is visual-only and reachable by no test in this tree.
            drawCurveWidget(c);

            for (usize k = 0; k < c.times.size() && k < c.values.size(); ++k) {
                ImGui::PushID(static_cast<int>(k));
                f32 kv[2] = {c.times[k], c.values[k]};
                ImGui::SetNextItemWidth(w * 1.4f);
                const bool keyChanged = ImGui::DragFloat2("##key", kv, 0.01f);
                // ONE DRAG IS ONE UNDO ENTRY, not one per frame: pushed at the moment the drag
                // ACTIVATES, before this frame's delta (if any) has been applied below, rather than
                // once per frame while it is held.
                if (ImGui::IsItemActivated()) pushUndo();
                if (keyChanged) {
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
                    pushUndo();
                    // KEPT PARALLEL on delete too, same reason as the insert above.
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

// THE 2D CURVE WIDGET: a canvas with draggable keys and draggable tangent handles, in the SAME
// hand-rolled ImGui style GraphEditor.cpp already uses for its own canvas (one big InvisibleButton
// for input capture, hit-testing done by hand against a plain draw-list, no vendored curve-editor
// widget anywhere in this tree). ALL LAYOUT, MAPPING AND HIT-TESTING GO THROUGH
// AnimCurveGeometry.hpp's free functions -- see that file's own header for why: they are ImGui-free
// and Engine-free on purpose, so AnimCurveGeometryTest can reach them with no window and no GPU.
//
// THE DRAG ITSELF IS VISUAL-ONLY, and is NOT covered by any test in this tree. Reading ImGui's
// per-frame mouse position and delta, deciding (once, at press time) what a click landed on, and
// writing the result into clip_ through screenToCurve/tangentSlopeFromHandle is all real editor
// behaviour, but it can only be exercised by a real mouse over a real window -- there is no ImGui
// context to drive headlessly the way GraphEditorLoadSaveTest drives GraphEditor.cpp's non-drawing
// half. What IS tested, in AnimCurveGeometryTest, is everything this function CALLS: the screen<->
// curve mapping, where a key or handle's geometry places it, and which one a given point resolves
// to -- which is every part of "does this widget do the right thing" that arithmetic can answer.
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

    // THE VIEW: time spans the WHOLE CLIP, so a key's position here reads against the same timeline
    // the notify/track bar above already shows. Value spans this curve's OWN authored min/max, padded
    // 10% each way so a point sitting exactly on the top or bottom edge stays fully visible and
    // grabbable -- the same normalise-to-own-range choice drawTimeline's older curve overlay makes,
    // now with the padding a real hit target needs that a one-pixel-tall line never did.
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
        // ONE UNDO ENTRY PER GESTURE, pushed at this press rather than per frame of the drag below --
        // and only when the press actually landed on something draggable, so a click on empty canvas
        // (which moves nothing) does not leave a no-op entry on the stack.
        if (curveDragKind_ != CurveHitKind::None) pushUndo();
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) curveDragKind_ = CurveHitKind::None;

    // DRAG: apply this frame's mouse delta through the SAME curve-space math the geometry file
    // exposes for testing -- a key MOVES; a handle's new screen position implies a new tangent SLOPE.
    if (canvasActive && curveDragKind_ != CurveHitKind::None &&
        curveDragKeyIndex_ < c.times.size()) {
        const ImVec2 md = ImGui::GetIO().MouseDelta;
        if (md.x != 0.0f || md.y != 0.0f) {
            const usize idx = curveDragKeyIndex_;
            const Vec2 keyScreen = curveToScreen(view, c.times[idx], c.values[idx]);
            if (curveDragKind_ == CurveHitKind::Key) {
                f32 newTime = 0.0f, newValue = 0.0f;
                screenToCurve(view, Vec2{keyScreen.x + md.x, keyScreen.y + md.y}, newTime, newValue);
                // TIME CLAMPED BETWEEN ITS NEIGHBOURS, the identical rule the numeric row below
                // applies and for the identical reason: the sampler binary-searches `times`.
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

    // DRAW: keys and, where this curve carries them, their two tangent handles joined by a line
    // through the key -- the usual "broken tangent" presentation, matching the fact that OcCurve
    // stores an INDEPENDENT in- and out-tangent per key rather than one shared slope.
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
        pushUndo();
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
                    pushUndo();
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
                pushUndo();
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
            const bool offsetChanged = ImGui::DragFloat3("Offset (cm)", t, 0.25f);
            // ONE DRAG IS ONE UNDO ENTRY -- pushed at activation, before any of this drag's own delta
            // is applied below, not once per frame while it is held.
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
                // RENORMALISED ON EDIT. Dragging four components independently leaves a quaternion
                // that is not a rotation, and the composition downstream would scale the attachment
                // rather than turn it. A zero-length drag falls back to identity instead of NaN.
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
        pushUndo();
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
                if (!next.empty() && next != n.name) { pushUndo(); n.name = next; dirty_ = true; }
            }
            f32 t = n.time;
            const f32 dur = clip_.duration > 0.0f ? clip_.duration : 1.0f;
            ImGui::SetNextItemWidth(180.0f * (ImGui::GetFontSize() / 16.0f));
            const bool timeChanged = ImGui::SliderFloat("Time", &t, 0.0f, dur, "%.3f s");
            // ONE DRAG IS ONE UNDO ENTRY -- pushed at activation, before this widget's own apply below,
            // not once per frame while the slider is held.
            if (ImGui::IsItemActivated()) pushUndo();
            if (timeChanged) {
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

// A stable, cheap-to-call time-to-pixel mapper shared by every lane this function draws, so the
// master slider, the notify lane, the curve lane and every per-track lane all agree on where a given
// second lands -- one bar of ticks and forty scattered rows would still be useless if they each did
// their own rounding. Already inside this file's own top-level anonymous namespace (opened above
// AnimSkinFeature), so this needs no linkage of its own -- same as boneBox and computeAnimEditorBonePalette
// beside it.
f32 laneX(f32 x0, f32 x1, f32 t, f32 dur) { return x0 + (x1 - x0) * (dur > 0.0f ? t / dur : 0.0f); }
#if AVER_WITH_IMGUI
// A lane row's screen rect, named rather than std::pair<ImVec2, ImVec2> so a call site reads
// `.min`/`.max` instead of `.first`/`.second`.
struct LaneRect { ImVec2 min, max; };
#endif

// ITEM: PER-TRACK LANES. This used to be ONE shared bar -- the master slider itself -- with every
// track's key times painted onto it as identical yellow ticks. That told an author THAT a key existed
// somewhere near a given time and nothing about WHOSE key it was; a forty-bone rig gave one bar of
// forty overlapping ticks and no way to click any single one of them. Below, each track gets its own
// row, named by its bone, with only ITS OWN keys drawn in it -- which is also what makes a key
// something the mouse can land on: drawTracks()'s selectedTrack_/selectedTrackKey_ are set from a
// click IN A SPECIFIC LANE, not from a click on a bar that never said which track it belonged to.
//
// NOTIFIES AND THE SELECTED CURVE GET THEIR OWN LANES TOO, ABOVE THE TRACK LIST, rather than staying
// overlaid on the master slider. The old comment on the notify loop said Unreal gives notifies their
// own lane and that doing the same here would be right "once there are lanes to give" -- there are
// now, so notifies take the first one and the selected curve (when there is one) takes the next,
// both drawn UNSCROLLED and always visible: a notify or a curve shape is a small, bounded amount of
// content an author wants to see continuously while scrolling past track 80 of 200, not something
// that should scroll away with the tracks underneath it.
void AnimEditor::drawTimeline() {
#if AVER_WITH_IMGUI
    const f32 dur = clip_.duration > 0.0f ? clip_.duration : 1.0f;
    ImGui::SetNextItemWidth(-1);
    // Scrubbing PAUSES, because a slider that fights the clock cannot be placed. This is now the
    // ONLY thing drawn on the slider itself -- every per-track tick, the curve overlay and the
    // notify markers that used to be painted over it moved into their own lanes below, so this bar
    // goes back to being what an ImGui slider already looks like.
    if (ImGui::SliderFloat("##time", &time_, 0.0f, dur, "%.3f s")) playing_ = false;

    const f32 uiScale = ImGui::GetFontSize() / 16.0f;
    // THE NAME COLUMN'S WIDTH is fixed across every lane -- the master slider has none, so this
    // constant exists only from here down -- so that a key's x in one row lands under the same
    // instant as a key's x in every other row, and under the same instant as the playhead line
    // drawn across all of them.
    const f32 nameW = 130.0f * uiScale;
    const f32 rowH = ImGui::GetTextLineHeightWithSpacing();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // A lane row's canvas: a name label of fixed width, then an InvisibleButton spanning whatever is
    // left, exactly as drawCurveWidget's own canvas captures input -- one big invisible button, hit-
    // tested by hand, no vendored timeline widget anywhere in this tree. Returns the canvas rect so
    // the caller can draw into it and hit-test against it.
    auto beginLaneRow = [&](const char* label) -> LaneRect {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(nameW);
        ImGui::InvisibleButton("##lane", ImVec2(ImGui::GetContentRegionAvail().x, rowH));
        return {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
    };

    // NOTIFY LANE, ALWAYS DRAWN even with zero notifies -- an author scanning forty tracks still
    // needs to see "there are no notifies here" rather than wonder whether the row silently vanished.
    {
        ImGui::PushID("##notifyLane");
        const auto [p0, p1] = beginLaneRow("Notifies");
        dl->AddRectFilled(p0, p1, IM_COL32(26, 26, 30, 255));
        for (usize i = 0; i < clip_.notifies.size(); ++i) {
            const fmt::OcNotify& n = clip_.notifies[i];
            const f32 x = laneX(p0.x, p1.x, n.time, dur);
            const bool sel = (static_cast<int>(i) == selectedNotify_);
            const ImU32 col = sel ? IM_COL32(255, 220, 90, 255) : IM_COL32(120, 200, 255, 230);
            // A STATE'S WINDOW, drawn as a translucent band UNDER the marker -- visual-only, matching
            // the clamp AnimSystem itself applies at the loop seam, so an author dragging a window
            // past the end of the clip sees it stop exactly where the runtime will actually close it.
            const f32 stateDur = notifyDurationAt(i);
            if (stateDur > 0.0f) {
                const f32 xEnd = laneX(p0.x, p1.x, std::min(n.time + stateDur, dur), dur);
                dl->AddRectFilled(ImVec2(x, p0.y), ImVec2(xEnd, p1.y),
                                  sel ? IM_COL32(255, 220, 90, 70) : IM_COL32(120, 200, 255, 55));
            }
            // A downward triangle rather than another vertical tick, so a notify is never mistaken
            // for the key ticks the track lanes below draw in the same style of row.
            const ImVec2 tri[3] = {ImVec2(x - 5.0f, p0.y), ImVec2(x + 5.0f, p0.y), ImVec2(x, p0.y + 9.0f)};
            dl->AddConvexPolyFilled(tri, 3, col);
            dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), col, sel ? 2.0f : 1.0f);
        }
        dl->AddLine(ImVec2(laneX(p0.x, p1.x, time_, dur), p0.y), ImVec2(laneX(p0.x, p1.x, time_, dur), p1.y),
                    IM_COL32(255, 210, 90, 150), 1.0f);
        ImGui::PopID();
    }

    // THE SELECTED CURVE'S OWN LANE, drawn only when a curve is actually selected -- an unselected
    // one has nothing an author is looking at, and reserving the row anyway would waste vertical
    // space on every clip that has no curves at all. Normalised to the curve's own min/max, exactly
    // as the old shared-bar overlay was: a curve's range is whatever the author chose, and a fixed
    // 0..1 mapping would flatten most of them into a line.
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
            // SAMPLED ACROSS THE LANE rather than drawn key-to-key, so a STEP curve reads as steps
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
        dl->AddLine(ImVec2(laneX(p0.x, p1.x, time_, dur), p0.y), ImVec2(laneX(p0.x, p1.x, time_, dur), p1.y),
                    IM_COL32(255, 210, 90, 150), 1.0f);
        ImGui::PopID();
    }

    if (clip_.tracks.empty()) { ImGui::TextDisabled("No tracks."); return; }

    // THE TRACK LANES. A rig can carry far more tracks than fit on screen -- FirstPerson's own 7-bone
    // rig is small, but nothing here assumes that stays true -- so this is a fixed-height, scrolling
    // child (ImGui's own scrollbar does the rest) with ImGuiListClipper choosing which rows to
    // actually submit, the identical clipper shape SandboxContentBrowser.cpp already uses for its own
    // asset grid. WITHOUT the clipper, a 200-bone rig would mean 200 InvisibleButtons and 200 hit-test
    // sweeps over every key on every track EVERY FRAME whether or not a single one of them is visible
    // -- the clipper keeps that cost proportional to what is on screen instead of to the rig.
    constexpr int kVisibleLanes = 8;
    const f32 lanesHeight = std::min(static_cast<f32>(clip_.tracks.size()), static_cast<f32>(kVisibleLanes)) * rowH
                          + ImGui::GetStyle().ItemSpacing.y;
    if (ImGui::BeginChild("##trackLanes", ImVec2(0, lanesHeight), true)) {
        // RE-FETCHED, not the outer `dl` captured above: a child window carries its OWN ImDrawList
        // and its own scissor/clip rect (which is what makes rows scrolled past its top or bottom
        // disappear correctly), and drawing through the PARENT window's list here would paint into
        // the wrong clip rect and the wrong place in this frame's draw order.
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

                // KEYS TINTED BY THE SAME BONE PALETTE the 3D preview paints its boxes with
                // (computeAnimEditorBonePalette, rebuilt in buildPreview) -- so a key in this lane and
                // the bone it moves read as the SAME colour, not just the same row label. Falls back
                // to a neutral grey on the one frame before the palette exists yet (bonePalette_
                // starts empty; buildPreview fills it before this ever runs a second time) or for a
                // track whose boneIndex is out of range, matching buildPreview's own fallback.
                BoneColor bc{0.6f, 0.6f, 0.6f};
                if (named && t.boneIndex < bonePalette_.size()) bc = bonePalette_[t.boneIndex];
                // CLAMPED BEFORE THE *255 CONVERSION: IM_COL32 just shifts and ORs its four bytes
                // together with no range check of its own, so a component that drifted a hair past
                // 1.0 (float error accumulated through the golden-angle hue math) would bleed a bit
                // into the NEXT channel instead of merely clipping to white -- a wrong colour rather
                // than a saturated one.
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
                // A ROW OUTLINE ON THE SELECTED TRACK, so "which track is Insert/Delete Key below
                // going to act on" is visible even when it currently has no key selected inside it.
                if (thisTrackSelected) dl->AddRect(p0, p1, IM_COL32(255, 220, 140, 200));
                dl->AddLine(ImVec2(laneX(p0.x, p1.x, time_, dur), p0.y),
                            ImVec2(laneX(p0.x, p1.x, time_, dur), p1.y), IM_COL32(255, 210, 90, 120), 1.0f);

                // PRESS: decide once, at the instant the mouse goes down on this lane, what it landed
                // on -- the same press-time-hit-test-then-hold shape drawCurveWidget already uses, so
                // a fast drag that strays past a key's own hit radius mid-gesture does not drop it.
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
                    // SELECTING A TRACK SELECTS ITS BONE, the identical cross-highlight drawSockets'
                    // own selection already gives a socket, so the tree, the preview highlight and
                    // this lane all agree about which bone a click just picked.
                    if (named) selectedBone_ = static_cast<int>(t.boneIndex);
                    trackKeyDragging_ = (hit != kInvalidKeyIndex);
                    // ONE UNDO ENTRY PER GESTURE, pushed at this press rather than per frame of the
                    // drag below -- and only when the press actually landed on a key, so a click on
                    // empty lane space (which moves nothing) does not leave a no-op entry on the
                    // stack. Mirrors drawCurveWidget's own identical bracket exactly.
                    if (trackKeyDragging_) pushUndo();
                }
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) trackKeyDragging_ = false;

                // DRAG: re-derive the key's time from the mouse's CURRENT x every frame (not a
                // delta), which is simpler than the curve widget's delta-from-screen-space approach
                // and correct here because a lane has exactly one axis that means anything -- there
                // is no y-position for a track key to preserve the way a curve's value is. trackMoveKey
                // RE-SORTS rather than clamps (see its own header comment for why), so the index this
                // key lives at can change mid-drag; the return value is what selectedTrackKey_ tracks
                // afterwards so the NEXT frame's drag still finds the same key.
                //
                // GATED ON laneActive, NOT ONLY ON trackKeyDragging_ -- the same gate drawCurveWidget's
                // own drag block uses (canvasActive there) -- because ImGui's ButtonBehavior clears
                // this widget's active id INSIDE the very InvisibleButton() call above the instant the
                // mouse comes up, so laneActive already reads false on the release frame with no
                // ordering trick needed here to stop this block from firing on it.
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

// Clamps selectedTrack_/selectedTrackKey_ against clip_.tracks as it stands RIGHT NOW -- the one
// place this file re-derives after any edit that can shrink either array out from under them: an
// Undo/Redo (applyClipUndoState), and drawClipRetime()'s trim (the only one of the three retime ops
// that can remove keys; scale and shift only move times, never the count of them). Bounds-only, not
// identity-preserving -- a trim that removes the keys BEFORE the selected one shifts every later
// index down, so the selection can end up pointing at a DIFFERENT surviving key rather than at
// nothing. That is the same trade applyClipUndoState already made for selectedNotify_/selectedCurve_
// before this file could touch tracks at all; nothing here raises the bar past what this tab already
// accepted for its other two selections.
void AnimEditor::clampTrackKeySelection() {
    if (selectedTrack_ < 0 || static_cast<usize>(selectedTrack_) >= clip_.tracks.size()) {
        selectedTrack_ = -1;
        selectedTrackKey_ = kInvalidKeyIndex;
        return;
    }
    const usize keyCount = clip_.tracks[static_cast<usize>(selectedTrack_)].times.size();
    if (selectedTrackKey_ != kInvalidKeyIndex && selectedTrackKey_ >= keyCount) selectedTrackKey_ = kInvalidKeyIndex;
}

// THE THREE WHOLE-CLIP RETIME OPERATIONS, wired straight to AnimEdit.hpp's pure functions. Each is a
// ONE-SHOT button press rather than a drag, so the "one undo entry per gesture, not per frame" rule
// this file otherwise enforces with an IsItemActivated/IsItemDeactivatedAfterEdit bracket (the
// bracket SandboxPanels.cpp's own materialPanel comment describes) is automatic here: a button click
// is already a single event, never a stream of per-frame deltas, so there is nothing to bracket.
//
// EVERY APPLY IS PUSH-THEN-ATTEMPT-THEN-CANCEL-ON-REFUSAL, using SnapshotUndo::cancelPush() exactly
// the way its own header comment says it exists to be used. scaleClipDuration/shiftClipTime/trimClip
// can all refuse (a non-finite or non-positive factor, a non-finite shift, a trim range that clamps
// to nothing) -- and an undo entry that restores a clip to a state IDENTICAL to the one already on
// screen is not a safety net, it is a Ctrl+Z that visibly does nothing once, the exact failure mode
// materialPanel's own "PUSHED ON RELEASE" comment calls out for a no-op edit.
void AnimEditor::drawClipRetime() {
#if AVER_WITH_IMGUI
    if (!ImGui::CollapsingHeader("Retime")) return;
    // LAZILY DEFAULTED TO THE FULL CLIP, not left at its zero-initialised default: an End of 0.0
    // always fails trimClip's own `end > start` check, so an author's very first look at this panel
    // would find Trim refusing to do anything until they pressed Full Range once by hand. Re-applied
    // only while End is still exactly its unset default, so a deliberate End of 0.0 (which can never
    // succeed anyway -- see the same check) does not fight an author who typed it on purpose.
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

// THE TRACK'S SAMPLED VALUE AT THE CURRENT PLAYHEAD, in the units OcTrack itself stores: engine
// centimetres for translation (OcBone::translation's own unit), the raw stored quaternion x/y/z/w for
// rotation -- NOT Euler degrees, which would be a DIFFERENT and lossy representation of a value the
// format never keeps that way -- and a unitless multiplier for scale.
//
// READS pose_ WHEN IT CAN, rather than calling AnimEdit.hpp's trackSampleAt() a second time.
// buildPreview() already ran anim::sampleAnimation(clip_, t, pose_) this exact frame (draw() calls it
// before drawTracks()), and sampleAnimation writes each bone's translation/rotation/scale ONLY for
// the channels a track for that bone enables -- an untouched channel keeps whatever restPose or an
// earlier track already put there -- so pose_.local[t.boneIndex] already holds exactly what THIS
// track contributed, at THIS frame's playhead, with the SAME loop-wrap/clamp buildPreview applied to
// time_. Re-sampling here would cost a second walk of this track's keys for an answer that can only
// ever agree with what is already sitting in pose_, or silently stop agreeing with it the day the two
// call sites' clamping rules drift apart.
//
// FALLS BACK TO trackSampleAt() ONLY WHEN THERE IS NO POSE TO READ -- t.boneIndex is past
// pose_.local's size, which happens when this clip opened with no matching skeleton (see draw()'s own
// "No skeleton found for this clip" branch) and buildPreview() therefore had no bones to seed a rest
// pose from at all. A raw sample off the track is still better than showing nothing, and loop-
// wrapping time_ for it would be wrapping against a duration that, with no skeleton to check tracks
// against, this function has no better reason to trust than the raw playhead.
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
        // THE BONE CELL IS NOW A SELECTABLE, not plain text. Clicking it sets selectedTrack_ -- the
        // SAME field drawTimeline()'s lanes set when a key is clicked there instead, and the field
        // drawTrackKeyEditor() below reads to know which track Insert/Delete Key act on. One
        // selection, reachable from either the table or the lane.
        const bool rowSelected = (selectedTrack_ == static_cast<int>(i));
        if (ImGui::Selectable(named ? skel_.bones[t.boneIndex].name.c_str() : "<out of range>",
                              rowSelected, ImGuiSelectableFlags_SpanAllColumns)) {
            selectedTrack_ = rowSelected ? -1 : static_cast<int>(i);
            selectedTrackKey_ = kInvalidKeyIndex;
            // SELECTING A TRACK SELECTS ITS BONE, the identical cross-highlight drawSockets' own
            // selection already gives a socket (see its own comment) -- so the bone tree, the preview
            // highlight and this table all agree about which bone a click just picked.
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

// THE SELECTED KEY'S DETAILS: its exact time (typed or dragged) and its value components (one drag
// per enabled channel), plus Insert/Delete for the selected track -- the numeric counterpart to
// dragging a key in drawTimeline()'s lanes, for the case where the number IS what an author has.
// Drawn below the table rather than inside it: a table cell is a poor home for a handful of
// DragFloatN controls whose count and width vary with which channels a track enables.
//
// TANGENTS ARE NOT EXPOSED HERE, only the Value slot -- unlike drawCurveWidget, which gives a curve's
// tangent handles their own draggable geometry, there is no 3D or 2D canvas here to drag a track's
// tangent ON, and the ask this panel exists to answer ("move/edit/add/delete keys") names values, not
// tangents. A CubicSpline track's tangents are left exactly as they were; editing only the value slot
// is still a well-formed edit -- OcTrack::valid() has no rule tying a value to its neighbours' slopes.
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

    // TIME. ImGui's DragFloat already doubles as "type an exact value" -- double-clicking (or
    // Ctrl+clicking) it opens a text box, the same way every other numeric field in this file already
    // lets an exact number in alongside the drag, so no separate InputFloat is needed for that.
    f32 tk = t.times[selectedTrackKey_];
    ImGui::SetNextItemWidth(w);
    const bool timeChanged = ImGui::DragFloat("Time", &tk, 0.01f, 0.0f, dur, "%.3f s");
    // ONE DRAG IS ONE UNDO ENTRY -- pushed at activation, before this widget's own apply below, not
    // once per frame while it is held. Same bracket as drawNotifies' own "Time" slider.
    if (ImGui::IsItemActivated()) pushUndo();
    if (timeChanged) {
        // trackMoveKey RE-SORTS rather than clamps (see its own header comment for why), so the
        // index this key lives at can change the instant it crosses a neighbour; the return value is
        // what selectedTrackKey_ becomes so the NEXT frame of the same drag still finds the same key.
        const usize moved = trackMoveKey(t, selectedTrackKey_, tk);
        if (moved != kInvalidKeyIndex) { selectedTrackKey_ = moved; dirty_ = true; }
    }

    // VALUE, ONE DRAG PER ENABLED CHANNEL, read and written through trackReadComponent/
    // trackWriteComponent so the per-key stride arithmetic (AnimEdit.hpp's own top-comment trap) is
    // never re-derived here -- there is exactly one place that does that math, and this is not it.
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
            // RENORMALISED ON EDIT, the identical reasoning drawSockets' own rotation drag already
            // gives: four components dragged independently stop being a rotation. AnimSampler.cpp
            // reads this exact key back through Quat::normalized() regardless (see its own
            // sampleChannel), so this is hygiene rather than a correctness requirement -- it keeps
            // what is ON DISK a genuine rotation, not only what gets sampled from it.
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

// Points the preview's pivot at one bone and picks a distance that frames it, rather than the whole
// rig -- see the header's own comment on why this is not simply a call into ActorPreview::frameAll().
//
// THE FRAMING DISTANCE COMES FROM HOW FAR THIS BONE ACTUALLY REACHES, not from boneBox's own
// geometry: a bone's box is deliberately THIN (kBoneThicknessCm = 2.2cm, sized to read as a limb, not
// to fill a viewport), so sizing the frame off it the way buildPreview's initial autoframe sizes off
// the whole draw list's bounds would land the camera far too close, exactly the failure buildPreview
// already backs off from with its own `addZoom(2.6f)` -- see that call's own comment. What actually
// answers "how much of the rig around this joint" is the distance to whatever else is ATTACHED to it:
// its parent, and any bone that names it as parent in turn. Longest of those wins, so framing a hip
// with a whole leg hanging off it does not crop the thigh the way averaging every neighbour would.
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
    // AN ISOLATED BONE (no parent, no children -- a one-bone rig, or a disconnected extra root) has
    // nothing to measure a reach against. kRootCubeCm is the box buildPreview draws for exactly this
    // case, so framing at a small multiple of it keeps that box comfortably inside view instead of
    // filling the whole frame (reach too small) or shrinking to a speck (reach left at its 0 default).
    if (reach < 1e-4f) reach = kRootCubeCm * 3.0f;

    render::preview::PreviewCamera& cam = preview.camera();
    cam.pivot[0] = at.x; cam.pivot[1] = at.y; cam.pivot[2] = at.z;
    // THE SAME FORMULA ActorPreview::frameAll() uses -- span * 1.8, clamped to its own [2, 500000]
    // range -- not reinvented here, so a single bone frames with the identical sense of "comfortably
    // inside view" the whole-rig frame already has. `reach` is a radius from the joint outward in ONE
    // direction; frameAll's `span` is a full diameter, hence the *2 before the same multiplier.
    cam.distance = std::clamp(reach * 2.0f * 1.8f, 2.0f, 500000.0f);
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

    // Ctrl+Z / Ctrl+Y reach the same undo()/redo() the buttons below call: guarded by canUndo()/
    // canRedo() the way the buttons are, and skipped while an InputText has focus -- it has its own
    // Ctrl+Z, and WantTextInput is how SoundEditor.cpp's own block already tells the two apart. Which
    // stack either one reaches follows isClip_, exactly as pushUndo() does -- see its own comment.
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
    // THE TRANSPORT AND THE TIMELINE LANES USED TO BE DRAWN RIGHT HERE, inline between the header
    // and the mesh/bones checkboxes below -- which meant the lanes fought the preview and the side
    // panels for height on every frame, at a proportion nothing in this tab let an author change.
    // They now live in their own retractable strip along the BOTTOM of the tab, drawn after the
    // bones/view/tracks row further down -- see that row's own comment, right before it, for the
    // full layout and why the strip's height has to be decided before this row is.

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
    // SCALED, not 240 raw pixels. Every size in this file predates the editor running at 300% DPI,
    // where a 240px column is about 80 logical pixels -- narrow enough that the bone names fit only
    // because they are short. GetFontSize() is the DPI proxy rather than a plumbed-through scale,
    // because it is already correct here and needs nothing threading through four call sites.
    const f32 uiScale = ImGui::GetFontSize() / 16.0f;

    // THE SEQUENCER STRIP'S HEIGHT HAS TO BE DECIDED HERE, BEFORE THE ROW BELOW IS LAID OUT --
    // ImGui is immediate-mode, so the bones/view/tracks row's own BeginChild calls need a height
    // THIS frame, and that height is "whatever total room is left, minus whatever the strip along
    // the bottom takes" -- so the strip's share has to come first even though the strip itself is
    // drawn AFTER the row, at the bottom, where it visually belongs. See kDefaultSeqFraction's own
    // comment for why this is a SplitPane of its own (seqSplit_) rather than a second use of split_
    // above: split_ divides this row LEFT-TO-RIGHT; seqSplit_ divides the row from the strip
    // TOP-TO-BOTTOM, and the two must never read or write each other's persisted fraction.
    const f32 hTotal = ImGui::GetContentRegionAvail().y;
    const f32 seqHandleH = 6.0f * uiScale;
    // ONE LINE PLUS FRAME PADDING -- exactly enough for the retract toggle and its label. This IS
    // "a thin header", the brief's own words: a header plus a summary row plus a mini-transport
    // would be a strip that never actually collapsed.
    const f32 seqHeaderH = ImGui::GetFrameHeightWithSpacing();
    const f32 minSeqH  = std::max(seqHeaderH + 90.0f * uiScale, 160.0f * uiScale);
    const f32 minMainH = 200.0f * uiScale;

    // A SKELETON TAB (isClip_ == false) HAS NO TRANSPORT OR TIMELINE TO SHOW AT ALL -- both read
    // clip_, which a .ocskel opened directly has none of (drawTransport/drawTimeline are already
    // gated the same way drawNotifies/drawCurves are) -- so it gets no strip and no split: the row
    // below simply takes every pixel hTotal has, exactly the layout this tab already had before the
    // strip existed.
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

    // WRAPPED IN A CHILD OF ITS OWN, EXACTLY `h` TALL -- this row did not need one before the strip
    // existed, and adding it fixes a bug that was ALWAYS latent in drawSplitHandle (EditorWidgets.hpp)
    // and only became visible here.
    //
    // THE SYMPTOM THIS FIXES: the three panels below drew at their correct size, but everything under
    // them was empty tab background all the way to the status bar, the sequencer strip was not on
    // screen anywhere, and the tab had grown a vertical scrollbar with a thumb sized as if the tab's
    // content were roughly double its own height.
    //
    // THE ACTUAL CAUSE: drawSplitHandle's own splitterHandle (EditorWidgets.hpp) sizes its invisible
    // button -- and the hover-highlight line it draws through the same height -- off
    // `ImGui::GetContentRegionAvail().y` at the moment it runs. That call is only ever correct when
    // the row calling it already consumes every pixel of vertical room the WINDOW has left, because
    // splitterHandle is placed via `ImGui::SameLine()`, which leaves the cursor sitting at the ROW's
    // own top -- so `GetContentRegionAvail().y` there measures all the way down to the WINDOW's own
    // bottom, not down to wherever the row itself is supposed to end. ActorEditor's own copy of this
    // pattern and BtEditor/SoundEditor/ParticleEditor's shared calls through drawSplitHandle all still
    // give their row the FULL remaining height (nothing of theirs is drawn below it in the same
    // window), so for every one of them "the window's bottom" and "the row's own bottom" are the same
    // pixel and this was never visible. This tab is the first to draw anything -- the sequencer strip
    // -- BELOW that same row within the SAME window, which is exactly why `h` above is deliberately
    // LESS than `hTotal`. drawSplitHandle has no way to know that: it sized its invisible button to
    // reach the window's actual bottom regardless, and ImGui grows a SameLine()'d row's line height to
    // its TALLEST item even though every other item on it (the three bordered children) stayed exactly
    // as tall as their own declared `h` -- so the row's cursor, once it moved to the next line, had
    // already advanced by very nearly `hTotal`, not `h`. That swallowed almost the entire budget this
    // function had just set aside for the strip and the seq-split handle before either of them ever
    // got to draw: the strip's own BeginChild still ran, at its correct `stripH`, but starting from a
    // cursor position already close to the window's bottom -- which is why it rendered far below the
    // visible tab instead of merely too short, and why the tab needed a scrollbar roughly as tall as
    // the strip's own share of the window to ever reach it.
    //
    // THE FIX STAYS IN THIS FILE, not in EditorWidgets.hpp: that header is shared by four other tabs
    // whose own rows are not wrong, and widening drawSplitHandle's contract for the one caller that
    // now needs a bounded height is exactly the kind of change that wants its own review rather than
    // one folded in here. Giving this row its OWN child, exactly `h` tall, means
    // `ImGui::GetContentRegionAvail().y` inside splitterHandle is now measured against THIS child's
    // own remaining height rather than the tab's -- which caps the invisible button, and with it the
    // row's own line height and cursor advance, at `h` regardless of what drawSplitHandle assumes.
    // `ImGuiStyleVar_WindowPadding` IS PUSHED TO ZERO FOR panelsRow'S OWN CREATION ONLY, popped again
    // the instant BeginChild returns and BEFORE any of the three panels below are drawn -- a
    // PushStyleVar this early would otherwise stay in effect for everything drawn while it is on the
    // stack, zeroing the SAME padding back out of "left"/"view"/"tracks" themselves (each its own
    // bordered child, each still expecting the editor's ordinary interior padding) and quietly
    // trimming a few DPI-scaled pixels off "tracks"'s own fill-remaining width in the process. Zeroing
    // it for JUST this one BeginChild call is what makes panelsRow an invisible wrapper rather than a
    // visible change: with no padding of its own, panelsRow's interior origin lands on the exact same
    // pixel "left" already started at without it, so nothing already on screen moves -- only
    // `ImGui::GetContentRegionAvail()` calls made INSIDE panelsRow (splitterHandle's, specifically)
    // now measure against ITS bottom instead of the tab's.
    //
    // THE SAME BUG, AND THE SAME FIX, APPLY WHEN THE STRIP IS COLLAPSED: `seqCollapsed_` only changes
    // how `stripH` (and therefore `h`) was computed above -- this row is drawn exactly the same way,
    // through the exact same drawSplitHandle call, whether the strip is a full pane or its own thin
    // header, so a wrapper that bounds `h` correctly needs no separate case for either state.
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
    // Draggable, persisted, through the shared SplitPane helper (EditorWidgets.hpp) -- see this
    // file's own kDefaultViewFraction comment for why 0.62f is not a new number. `midAvail` is the
    // width remaining after the fixed-width bones/sockets/animations column just drawn -- the SAME
    // base the original `avail.x * 0.62f` measured, so the default view width at a given window size
    // is unchanged.
    const f32 midAvail = ImGui::GetContentRegionAvail().x;
    const f32 minView = 200.0f * uiScale, minTracks = 200.0f * uiScale;
    const f32 midW = splitPaneWidth(split_, kPrefViewSplit, kDefaultViewFraction, midAvail,
                                     minView, minTracks);
    if (ImGui::BeginChild("view", ImVec2(midW, h), true)) {
        // WHAT THE MESH IS SHADING WITH, IN ONE LINE, ABOVE THE PICTURE -- the same "message above
        // the render" GraphEditor::drawMaterialViewport already uses for a graph that does not
        // compile. The point in both places is that a preview with nothing said about itself just
        // looks broken, and an author who never thinks to open the Output Log (or does, and finds
        // nothing there for four of resolvePreviewMaterial's five ways of coming up empty) needs the
        // answer right here instead. Guarded on materialStatus_ being non-empty rather than on
        // AVER_MODULE_PBR: without that module materialTried_ never latches and the string stays
        // empty forever, so the line simply never draws -- no second guard needed for a plain
        // std::string with nothing PBR-specific in it.
        if (!materialStatus_.empty()) {
            // WRAPPED, NOT A SINGLE LONG LINE -- unlike GraphEditor's own short compile-error
            // sentence, a couple of this message's branches spell out every candidate .ocmat path
            // tried, which is easily wider than the view column ever is (minView is 200 logical
            // pixels). TextDisabled/TextColored neither wrap on their own, so the colour is pushed
            // by hand around an ordinary TextWrapped instead of switching widgets per branch.
            if (materialFailed_) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.72f, 0.35f, 1.0f));
            else                 ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", materialStatus_.c_str());
            ImGui::PopStyleColor();
        }
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
            // NAVIGATION, MADE TO MATCH THE MAIN VIEWPORT -- the second half of this task's brief.
            // GATED ON HOVER ALONE, exactly like the orbit this replaces: every gesture below fires
            // only while the mouse sits over the preview IMAGE itself, never merely somewhere inside
            // this child window, so a drag that ends with the mouse over the sequencer strip below
            // (dragging a timeline key, say) can never also spin, pan or nudge this camera.
            if (ImGui::IsItemHovered()) {
                const ImGuiIO& io = ImGui::GetIO();
                render::preview::PreviewCamera& cam = preview->camera();

                // LEFT-DRAG ORBITS, X INVERTED. This used to read `+io.MouseDelta.x`, which is what
                // AssetEditor.cpp's own copy of this exact block still does -- but ActorEditor.cpp and
                // GraphEditor.cpp both negate it, and negating is the one that matches a real
                // viewport's feel: the drag moves the WORLD under the cursor, so pushing the mouse
                // right swings what is on screen to the LEFT, not to the right.
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                    cam.addOrbit(-io.MouseDelta.x * 0.4f, io.MouseDelta.y * 0.4f);
                if (io.MouseWheel != 0.0f)
                    cam.addZoom(io.MouseWheel > 0.0f ? 0.9f : 1.1f);

                // MIDDLE- AND RIGHT-DRAG PAN, THROUGH camera().panPixels() -- the one gesture every
                // OTHER preview tab in this editor already has (AssetEditor.cpp, ActorEditor.cpp,
                // GraphEditor.cpp) and this one alone was missing. An orbit camera can spin around its
                // pivot and dolly to it, but cannot bring an off-centre joint TO the middle of frame,
                // which is the thing an author actually needs before zooming in on it.
                for (const ImGuiMouseButton b : {ImGuiMouseButton_Middle, ImGuiMouseButton_Right}) {
                    if (!ImGui::IsMouseDragging(b)) continue;
                    const ImVec2 d = ImGui::GetMouseDragDelta(b);
                    ImGui::ResetMouseDragDelta(b);
                    cam.panPixels(d.x, d.y, static_cast<f32>(preview->height()));
                }

                // WASD/QE and F, DECIDED DELIBERATELY RATHER THAN COPIED FROM THE LEVEL VIEWPORT
                // AS-IS. SandboxViewport's own WASD/QE (SandboxApp.cpp's `flying_` block) fly a free
                // CAMERA through space: W/A/S/D step camPos_ along the camera's forward/right, Q/E
                // along world up. PreviewCamera has no camPos_ to step at all -- it is an ORBIT,
                // yaw/pitch/distance ABOUT A PIVOT (see its own declaration) -- so stepping an eye
                // position the way a fly camera does would be discarded the instant the next
                // addOrbit/addZoom recomputed the eye from pivot+distance+yaw+pitch anyway. Moving the
                // PIVOT along those same forward/right/world-up axes is what keeps every other
                // control's meaning intact (orbit still turns around whatever is centred, zoom still
                // dollies to it) while giving WASD/QE the one job an orbit camera can actually do with
                // them: recentre, along the view's own axis and not only across its screen-parallel
                // plane the way panPixels above is limited to. Scaled by the camera's OWN distance
                // rather than a fixed cm/s, so a preview framed close (one small bone) moves in fine
                // steps and one framed wide (the whole rig) covers ground fast enough to be worth
                // pressing at all -- the same reasoning frameSelectedBone's own distance formula
                // leans on.
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

                    // F FRAMES THE SELECTED BONE IF ONE IS SELECTED, THE WHOLE RIG OTHERWISE --
                    // the brief's own words exactly. `false` (no repeat) so holding F down does not
                    // refire every frame the way a held movement key above is supposed to.
                    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
                        if (selectedBone_ >= 0 && static_cast<usize>(selectedBone_) < model_.size()) {
                            frameSelectedBone(*preview, static_cast<usize>(selectedBone_));
                        } else {
                            preview->frameAll();
                            // THE SAME BACK-OFF buildPreview's OWN INITIAL AUTOFRAME APPLIES, and for
                            // the identical reason (see that call's own comment): frameAll sizes on a
                            // draw's bounds radius times its world scale, and a bone box is thin
                            // across and long along, so the radius it picks lands the camera inside
                            // the rig without this.
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
    ImGui::EndChild();   // "panelsRow" -- see its own BeginChild comment above. Its WindowPadding
                         // PushStyleVar was already popped right after panelsRow's own BeginChild,
                         // so EndChild here needs nothing further undone.

    // THE SEQUENCER STRIP, drawn AFTER the row above so ImGui's ordinary top-to-bottom flow places
    // it exactly where the height budget computed before that row already reserved for it. Nothing
    // to draw for a skeleton tab -- see hTotal's own comment above for why isClip_ == false takes
    // none of this and h already claimed all of hTotal in that case.
    if (isClip_) {
        if (seqExpanded) {
            // THE DRAG HANDLE, only while expanded: a strip pinned to its collapsed header height
            // has nothing to resize, and a handle drawn anyway would invite a drag that silently
            // does nothing.
            //
            // WRITES STRAIGHT BACK INTO `stripH`, UNLIKE split_'s OWN drawSplitHandle (which updates
            // its SplitPane only for the FOLLOWING frame's splitPaneWidth call, because the pane it
            // sizes -- "view" -- was already drawn above it by the time drawSplitHandle runs). Here
            // the order is the other way around: the handle runs BEFORE the "sequencer" child below
            // it, so the very child a drag is resizing can pick up this SAME frame's delta instead of
            // trailing it by one -- a strictly better answer to "the layout actually reflows" for the
            // one pane in this tab where the handle and the pane it sizes are drawn in that order.
            bool seqReleased = false;
            verticalSplitterHandle("##seqsplit", seqHandleH, &stripH, seqSplitAvail, minSeqH, minMainH,
                                    &seqReleased);
            seqSplit_.fraction = splitFractionOf(stripH, seqSplitAvail);
            if (seqReleased) storeSplitFraction(kPrefSeqSplit, seqSplit_.fraction);
        }
        if (ImGui::BeginChild("sequencer", ImVec2(0, stripH), true)) {
            // THE RETRACT TOGGLE. ICON_EXPAND ("expand_more", a downward chevron) reads as "open,
            // click to close" the same way ImGui's own tree-node arrow does when expanded;
            // ICON_CHEVRON ("chevron_right") reads as "closed, click to open" for the same reason.
            if (ImGui::SmallButton(seqCollapsed_ ? ICON_CHEVRON " Sequencer" : ICON_EXPAND " Sequencer")) {
                seqCollapsed_ = !seqCollapsed_;
                // PERSISTED IMMEDIATELY, matching storeSplitFraction's own "the instant a drag ends"
                // rule (EditorWidgets.hpp) -- a crash or an alt-tab right after this click should not
                // silently un-collapse the strip on the next launch.
                setPrefBool(kPrefSeqCollapsed, seqCollapsed_);
                flushEditorPrefs();
            }
            if (seqExpanded) {
                ImGui::SameLine();
                ImGui::TextDisabled("%.3f s   %zu track(s)", clip_.duration, clip_.tracks.size());
                // THE STRIP'S OWN CONTENT, filling whatever room is left in it once the header row
                // just drawn is accounted for. drawTimeline()'s own track-lane child clips itself
                // further still (its own ImGuiListClipper region), so a strip too short to show every
                // lane grows an INNER scrollbar there rather than clipping the transport or the
                // master scrub bar above it.
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

// Restores the view/tracks split AND the sequencer strip to their default proportions, persisting
// both immediately -- see AssetEditor.hpp's own resetLayout() comment for why "Reset Tab Layout"
// needs every tab to implement this rather than just ActorEditor. A no-op with AVER_WITH_IMGUI off: a
// headless build never lays the panels out at all, so there is nothing for a reset to restore.
void AnimEditor::resetLayout() {
#if AVER_WITH_IMGUI
    resetSplitPane(split_, kPrefViewSplit, kDefaultViewFraction);
    resetSplitPane(seqSplit_, kPrefSeqSplit, kDefaultSeqFraction);
    // THE COLLAPSE STATE RESETS TOO -- it is layout state exactly as much as either split's fraction
    // is, and a "Reset Tab Layout" that put the strip back to its default HEIGHT while leaving it
    // retracted (or the reverse: expanded at whatever fraction it last had before being collapsed)
    // would still not be the tab's default layout.
    seqCollapsed_ = false;
    setPrefBool(kPrefSeqCollapsed, false);
    flushEditorPrefs();
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
