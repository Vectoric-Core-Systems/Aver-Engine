// Editor: viewport -- gizmos/manipulation, picking, spawning/drops, sculpt & foliage brushes, PlayerStart authoring, overlays, editor modes.
// Split out of the 29,952-line SandboxApp.cpp (2026-09-16), method bodies moved verbatim; class declared in SandboxApp.hpp.

#include "SandboxApp.hpp"
#include "ViewportMarquee.hpp"

namespace aver {
// Physics-only guard (matches the declaration): draws every body's world AABB. Previously nested
// under AVER_MODULE_SYNAPSE by mistake -- that guard belongs to the nav overlay below.
#if AVER_MODULE_PHYSICS
namespace {
// Green = static box, blue = static triangle mesh, amber = moving (separates "why isn't this falling"
// from "why isn't this stopping anything" at a glance). Blue matters: a mesh body's bound can span a
// whole courtyard the mesh only rings, and drawn green that reads as solid floor.
constexpr f32 kColliderBoxRgb[3]   = {0.35f, 0.95f, 0.45f};
constexpr f32 kColliderMeshRgb[3]  = {0.30f, 0.62f, 1.00f};
constexpr f32 kColliderMoverRgb[3] = {1.00f, 0.72f, 0.25f};

// One box's twelve edges, appended to `out` as a line list.
void appendBoxEdges(std::vector<rhi::LineVertex>& out, const f32* lo, const f32* hi, const f32* rgb) {
    const f32 xs[2] = {lo[0], hi[0]};
    const f32 ys[2] = {lo[1], hi[1]};
    const f32 zs[2] = {lo[2], hi[2]};
    const auto edge = [&](int x0, int y0, int z0, int x1, int y1, int z1) {
        rhi::LineVertex a{}, c{};
        a.px = xs[x0]; a.py = ys[y0]; a.pz = zs[z0]; a.r = rgb[0]; a.g = rgb[1]; a.b = rgb[2];
        c.px = xs[x1]; c.py = ys[y1]; c.pz = zs[z1]; c.r = rgb[0]; c.g = rgb[1]; c.b = rgb[2];
        out.push_back(a);
        out.push_back(c);
    };
    // Twelve box edges, written out (not looped) so a wrong corner is visible in the source.
    edge(0,0,0, 1,0,0); edge(0,1,0, 1,1,0); edge(0,0,1, 1,0,1); edge(0,1,1, 1,1,1);
    edge(0,0,0, 0,1,0); edge(1,0,0, 1,1,0); edge(0,0,1, 0,1,1); edge(1,0,1, 1,1,1);
    edge(0,0,0, 0,0,1); edge(1,0,0, 1,0,1); edge(0,1,0, 0,1,1); edge(1,1,0, 1,1,1);
}
}  // namespace

// Twelve edges per body's world AABB. Tooltip clarifies this is a BOUND, not the shape (see
// aver_phys_body_aabb for why the ABI reports a bound, not a shape tree).
//
// TWO MESHES, EACH REMADE ONLY WHEN WHAT IT DRAWS COULD HAVE CHANGED.
//
// colliderMesh_ is the STATIC bodies: the 20,000-odd boxes and triangle meshes of a big level, and the
// picture that costs. It used to be destroyed and recreated on EVERY frame the toggle was on, after an
// O(n^2) walk of the physics world (aver_phys_body_at is a linear advance over a hash map): seconds a
// frame on a level with tens of thousands of colliders, to redraw a picture that had not moved. A static
// body does not move, so its picture depends only on which bodies exist and where they were placed --
// in Play as in the editor. Every editor path that makes, remakes or drops one of its bodies bumps
// colliderRev_ (rebuildEntityBody, destroyEntity), and a level load, unload or instantiate changes the
// body counts and the first/last handle levelBodies_ holds -- all of that is in the stamp below. Left
// OUT of it on purpose: the undo stacks, since a rename or a visibility toggle touches no body and must
// not cost a rebuild of the whole overlay; and whether a play session runs, which WAS in it, so Play
// rebuilt every static box every frame (12.7 MB of vertices, two fresh upload buffers) to animate the
// few hundred bodies that actually move. What the stamp cannot see: a script repositioning a static
// body, or flipping a body's motion type, with no body added or dropped. In Play that is caught by an
// audit instead: colliderStatics_ keeps the handles and AABBs the static mesh drew, and each frame a
// rotating slice of them (kAuditSlice, about 0.1 ms) is re-read; one that is no longer static or has
// moved forces a full rebuild that frame. A full cycle is n / kAuditSlice frames, so a change shows
// within about 43 frames at 22,000 bodies. Outside Play nothing runs scripts, so there is no audit.
//
// colliderMoverMesh_ is the bodies that MOVE (kinematic or dynamic, amber), whose handles
// colliderMovers_ captures whenever the static mesh is rebuilt. It is refreshed then, every frame a
// play session runs, once more on the frame it ends (the level put back), and every frame while the
// editor does not track one of them (a rigid body Play spawned could move at any time). Even then the
// line mesh is only remade if a box actually moved, so a paused Play or a level of sleeping bodies
// costs a few hundred AABB queries and no GPU work.
//
// AND THE STATIC MESH ONLY WHEN THE EDITOR KNOWS EVERY STATIC BODY, because that is what the stamp can
// vouch for: a static body made by something else (a streamed chunk, the landscape heightfield, a
// script) can appear with nothing here to see it, so while any exists the static mesh is rebuilt every
// frame, as before. "Knows every static body" is proven, not assumed: each live static handle is looked
// up among the handles the editor tracks (levelBodies_, entityBodies_). Either way the walk is O(n):
// the world's handles come from one aver_phys_body_handles pass instead of asking for the i-th body n
// times.
void SandboxApp::rebuildColliderOverlay(Engine& e) {
    const int32_t n = aver_phys_body_count();

#if AVER_MODULE_FRAMEWORK
    const bool moving = anyPlayActive();
#else
    const bool moving = false;
#endif
    u64 sig = kFnv1a64OffsetBasis;
    const auto mix = [&sig](u64 v) { sig = (sig ^ v) * kFnv1a64Prime; };
    mix(static_cast<u64>(static_cast<u32>(n)));
    mix(colliderRev_);
#if AVER_MODULE_SCENE
    mix(static_cast<u64>(levelBodies_.size()));
    mix(levelBodies_.empty() ? 0u : static_cast<u64>(static_cast<u32>(levelBodies_.front())));
    mix(levelBodies_.empty() ? 0u : static_cast<u64>(static_cast<u32>(levelBodies_.back())));
    mix(static_cast<u64>(entityBodies_.size()));
#endif
    bool staticStale =
        !(colliderOverlayBuilt_ && colliderOverlayAllKnown_ && sig == colliderOverlaySig_);

    // The audit (see the comment above): in Play, re-read a slice of the static handles the mesh drew.
    // Before the rebuild below, so a change found here is redrawn this frame, not the next.
    if (!staticStale && moving && !colliderStatics_.empty()) {
        constexpr usize kAuditSlice = 512;
        const usize count = colliderStatics_.size();
        usize i = colliderAuditCursor_ < count ? colliderAuditCursor_ : 0;
        const usize end = std::min(count, i + kAuditSlice);
        for (; i < end; ++i) {
            const int32_t body = colliderStatics_[i];
            f32 lo[3], hi[3];
            const f32* drawn = &colliderStaticBoxes_[i * 6];
            if (aver_phys_body_motion_type(body) != 0 || !aver_phys_body_aabb(body, lo, hi) ||
                std::memcmp(lo, drawn, sizeof lo) != 0 || std::memcmp(hi, drawn + 3, sizeof hi) != 0) {
                staticStale = true;   // the rebuild resets the cursor
                break;
            }
        }
        colliderAuditCursor_ = i >= count ? 0 : i;
    }

    if (staticStale) {
        colliderOverlaySig_ = sig;
        colliderOverlayBuilt_ = true;
        colliderOverlayAllKnown_ = false;
        colliderMoversTracked_ = false;
        colliderMovers_.clear();
        colliderStatics_.clear();
        colliderStaticBoxes_.clear();
        colliderAuditCursor_ = 0;
        colliderMoverBoxes_.clear();   // so the moving mesh below is remade even if nothing moved
        if (colliderMesh_) { e.device()->destroyLineMesh(colliderMesh_); colliderMesh_ = 0; }
        if (colliderMoverMesh_) { e.device()->destroyLineMesh(colliderMoverMesh_); colliderMoverMesh_ = 0; }

        if (n <= 0) {
            colliderOverlayAllKnown_ = true;
            colliderMoversTracked_ = true;
        } else {
            // ABI doesn't report body shape; editor built each level body by one rule (rebuildEntityBody /
            // world::instantiate): triangles when content_.collisionMeshFor has a mesh for the entity's
            // CMeshRenderer, else a fitted box. Re-querying that cache avoids a second table that could drift.
            std::unordered_set<int32_t> meshBodies;
            // The handles the editor tracks, sorted, to tell which live bodies it knows of.
            std::vector<int32_t> tracked;
#if AVER_MODULE_SCENE
            {
                scene::World& w = scene::World::instance();
                for (const auto& [ent, body] : entityBodies_) {
                    const auto* mr = w.component<scene::CMeshRenderer>(static_cast<scene::Entity>(ent),
                                                                       scene::kComponentMeshRenderer);
                    if (mr && content_.collisionMeshFor(mr->mesh)) meshBodies.insert(body);
                }
                tracked.reserve(levelBodies_.size() + entityBodies_.size());
                tracked.insert(tracked.end(), levelBodies_.begin(), levelBodies_.end());
                for (const auto& kv : entityBodies_) tracked.push_back(kv.second);
                std::sort(tracked.begin(), tracked.end());
            }
#endif

            // Every live body, enumerated in ONE pass (aver_phys_body_at(i) over [0, n) was O(n^2)), and
            // drawn -- tracked or not, so a landscape heightfield or a streamed box costs no extra pass.
            // The static ones go into this mesh; the rest are only listed, for the moving mesh.
            std::vector<int32_t> handles(static_cast<usize>(n));
            handles.resize(static_cast<usize>(aver_phys_body_handles(handles.data(), n)));
            std::vector<rhi::LineVertex> lines;
            lines.reserve(handles.size() * 24);
            bool staticKnown = true, moversKnown = true;
            for (const int32_t body : handles) {
                if (!body) continue;
                const int32_t motion = aver_phys_body_motion_type(body);
                if (motion < 0) continue;   // not a live body (-1; 0 is STATIC, a real answer)
                const bool known = std::binary_search(tracked.begin(), tracked.end(), body);
                if (motion != 0) {
                    colliderMovers_.push_back(body);
                    if (!known) moversKnown = false;
                    continue;
                }
                if (!known) staticKnown = false;
                f32 lo[3], hi[3];
                if (!aver_phys_body_aabb(body, lo, hi)) continue;
                appendBoxEdges(lines, lo, hi, meshBodies.count(body) != 0 ? kColliderMeshRgb : kColliderBoxRgb);
                colliderStatics_.push_back(body);   // what the Play audit re-reads
                colliderStaticBoxes_.insert(colliderStaticBoxes_.end(), lo, lo + 3);
                colliderStaticBoxes_.insert(colliderStaticBoxes_.end(), hi, hi + 3);
            }
            // Whether the editor tracks every static body, which is what lets the stamp above skip the
            // next frames; and every moving one, which is what lets the moving mesh skip them outside Play.
            colliderOverlayAllKnown_ = staticKnown;
            colliderMoversTracked_ = moversKnown;

            if (!lines.empty())
                colliderMesh_ = e.device()->createLineMesh(lines.data(), static_cast<u32>(lines.size()));
        }
    }

    const bool moversMayHaveMoved =
        staticStale || moving || colliderMoversWereLive_ || !colliderMoversTracked_;
    colliderMoversWereLive_ = moving;
    if (!moversMayHaveMoved || colliderMovers_.empty()) return;

    // Where every mover is now, compared with where the mesh drew it: nothing moved, nothing to remake.
    std::vector<f32>& boxes = colliderMoverBoxesNext_;
    boxes.clear();
    for (const int32_t body : colliderMovers_) {
        f32 lo[3], hi[3];
        if (!aver_phys_body_aabb(body, lo, hi)) continue;   // dead since the list was taken: draws nothing
        // Made static again since the list was taken (a script's SetBodyMotionType): redrawn green by the
        // next frame's full rebuild, which is also what drops it from this list.
        if (moving && aver_phys_body_motion_type(body) == 0) colliderOverlayBuilt_ = false;
        boxes.insert(boxes.end(), lo, lo + 3);
        boxes.insert(boxes.end(), hi, hi + 3);
    }
    if (boxes == colliderMoverBoxes_) return;
    colliderMoverBoxes_.swap(boxes);

    if (colliderMoverMesh_) { e.device()->destroyLineMesh(colliderMoverMesh_); colliderMoverMesh_ = 0; }
    colliderMoverLines_.clear();
    for (usize i = 0; i + 6 <= colliderMoverBoxes_.size(); i += 6)
        appendBoxEdges(colliderMoverLines_, &colliderMoverBoxes_[i], &colliderMoverBoxes_[i + 3], kColliderMoverRgb);
    if (!colliderMoverLines_.empty())
        colliderMoverMesh_ = e.device()->createLineMesh(colliderMoverLines_.data(),
                                                        static_cast<u32>(colliderMoverLines_.size()));
}

#endif  // AVER_MODULE_PHYSICS

#if AVER_MODULE_SYNAPSE
void SandboxApp::rebuildNavOverlay(Engine& e) {
    if (navMesh_) { e.device()->destroyLineMesh(navMesh_); navMesh_ = 0; }
    if (nav_.cells.empty()) return;
    const std::vector<rhi::LineVertex> lines = editor::buildNavOverlay(nav_, navRegionColours_);
    if (lines.empty()) return;
    navMesh_ = e.device()->createLineMesh(lines.data(), (u32)lines.size());
}

#endif

// ---- selection outline, as LINES (not a mesh) --------------------------------------------------
// Used to be the mesh redrawn via drawMesh+setWireframe(true), but drawMesh is gated on
// sceneSuppressed(), which VoxiRenderer returns true whenever ray-driven (this engine's standing
// default) -- so the outline drew into nothing: measured with the interactive gate lifted, ONE
// orange pixel appeared anywhere in the viewport (a leaf vein); the device log says so outright,
// "The rasteriser draws NOTHING while this holds". drawLines uses the narrower suppressesWholeFrame(),
// which VoxiRenderer keeps false for ray-driven -- D3D12Device::drawLines: "Gizmos and wireframes
// belong in a ray-driven viewport as much as in a rastered one, and they depth-test against the
// real depth the ray pass writes." The grid, gizmo, nav mesh and collider overlay already use that path.
//
// Draws boundary and crease edges only, not every edge (a wireframe of a 31k-triangle plant is an
// orange thicket): an edge is drawn when it belongs to exactly one triangle, or its two triangles'
// normals disagree by more than kCreaseCos. Camera-independent, so built once per mesh and cached.
//
// Re-reads the .ocmesh because loadProjectMeshes frees the CPU-side OcMeshData after GPU upload;
// cached by mesh id, so this costs one file read per asset, not per selection.
rhi::LineHandle SandboxApp::selectionOutlineLines(Engine& e, u64 meshId) {
// Guards the whole body (not just selOutlineLines_'s find/insert): selOutlineLines_ lives behind
// AVER_MODULE_PBR in SandboxApp.hpp, so guarding only the cache accesses would leave the
// .ocmesh read / edge-weld / createLineMesh in between running with nowhere to store the handle --
// a leaked line mesh every call, not a degraded feature. Call site in SandboxRender.cpp stays
// unconditional (same shape as the rest of this file's AVER_MODULE_LANDSCAPE guards).
//
// Also needs AVER_MODULE_SCENE: PBR does not imply SCENE (root CMakeLists' cascade lists neither),
// and meshPathById_ -- the scene's mesh-id -> .ocmesh map -- is the only way to turn the id into a
// file to read.
#if AVER_MODULE_PBR && AVER_MODULE_SCENE
    if (const auto it = selOutlineLines_.find(meshId); it != selOutlineLines_.end()) return it->second;

    const auto pit = meshPathById_.find(meshId);
    const std::string content = project_.contentDir();
    if (pit == meshPathById_.end() || content.empty()) return 0;

    fmt::OcMeshData md;
    std::string why;
    if (!fmt::loadOcMesh(content + "/" + pit->second, md, &why)) {
        AVER_WARN("[Editor] selection outline: {}", why);
        selOutlineLines_[meshId] = 0;   // cached as "no outline", so this is not retried per frame
        return 0;
    }

    // Adjacency by POSITION, not index: meshes split a vertex at UV/normal seams, so keyed by index
    // almost no edge finds its neighbour and the "outline" becomes a full wireframe (measured: one
    // Anthurium mesh gave 31,113 edges from 15,544 triangles, more than the ~23k a closed mesh that
    // size should have). Welding by position finds the neighbours the indices hide.
    //
    // Quantised to 1/100 cm before hashing (engine unit is cm): welds anything within 10 microns,
    // not what a person would call two places.
    const auto weld = [&md](u32 v) {
        const auto q = [](f32 x) { return static_cast<i64>(std::llround(static_cast<f64>(x) * 100.0)); };
        const i64 x = q(md.positions[usize(v)*3+0]);
        const i64 y = q(md.positions[usize(v)*3+1]);
        const i64 z = q(md.positions[usize(v)*3+2]);
        // fnv1a over the three quantised coordinates: a 64-bit id for a POSITION.
        u64 h = 0xcbf29ce484222325ull;
        for (const i64 c : {x, y, z}) {
            const u64 u = static_cast<u64>(c);
            for (int b = 0; b < 8; ++b) { h ^= (u >> (b * 8)) & 0xFF; h *= 0x100000001b3ull; }
        }
        return h;
    };

    struct EdgeFaces { u32 a = 0xFFFFFFFFu, b = 0xFFFFFFFFu; };
    // Keyed by the pair of welded position ids, order-independent, so either triangle's walk lands in the same bucket.
    std::unordered_map<u64, EdgeFaces> edges;
    std::unordered_map<u64, std::pair<u32, u32>> edgeVerts;   // key -> one representative index pair
    const usize triCount = md.indices.size() / 3;
    edges.reserve(triCount * 3);
    edgeVerts.reserve(triCount * 3);

    std::vector<u64> welded(md.vertexCount());
    for (u32 v = 0; v < md.vertexCount(); ++v) welded[v] = weld(v);

    const auto faceNormal = [&md](usize t, Vec3& n) {
        const u32 i0 = md.indices[t*3+0], i1 = md.indices[t*3+1], i2 = md.indices[t*3+2];
        const Vec3 p0{md.positions[usize(i0)*3+0], md.positions[usize(i0)*3+1], md.positions[usize(i0)*3+2]};
        const Vec3 p1{md.positions[usize(i1)*3+0], md.positions[usize(i1)*3+1], md.positions[usize(i1)*3+2]};
        const Vec3 p2{md.positions[usize(i2)*3+0], md.positions[usize(i2)*3+1], md.positions[usize(i2)*3+2]};
        n = cross(p1 - p0, p2 - p0).getSafeNormal();
    };

    for (usize t = 0; t < triCount; ++t) {
        const u32 idx[3] = {md.indices[t*3+0], md.indices[t*3+1], md.indices[t*3+2]};
        for (int k = 0; k < 3; ++k) {
            const u32 vi = idx[k], vj = idx[(k+1)%3];
            u64 wa = welded[vi], wb = welded[vj];
            if (wa == wb) continue;              // a degenerate edge: both ends weld to one place
            if (wa > wb) { const u64 t2 = wa; wa = wb; wb = t2; }
            // 64 bits mixed from two 64-bit ids. A collision would merge two unrelated edges into
            // one bucket and at worst drop one line from a highlight -- not worth a wider key.
            const u64 key = wa ^ (wb * 0x9E3779B97F4A7C15ull);
            edgeVerts.emplace(key, std::pair<u32, u32>{vi, vj});
            EdgeFaces& ef = edges[key];
            if (ef.a == 0xFFFFFFFFu)      ef.a = static_cast<u32>(t);
            else if (ef.b == 0xFFFFFFFFu) ef.b = static_cast<u32>(t);
            // A third face on one edge is non-manifold geometry; it is drawn as a crease rather
            // than dropped, which is the conservative answer for a selection highlight.
        }
    }

    // cos(40 degrees). Chosen so a smooth cylinder's facets do not each become an edge while a
    // box's corners still do.
    constexpr f32 kCreaseCos = 0.766f;
    std::vector<rhi::LineVertex> lines;
    lines.reserve(edges.size() / 2);
    for (const auto& [key, ef] : edges) {
        bool draw = ef.b == 0xFFFFFFFFu;   // a boundary edge: exactly one face
        if (!draw) {
            Vec3 na, nb;
            faceNormal(ef.a, na);
            faceNormal(ef.b, nb);
            draw = dot(na, nb) < kCreaseCos;
        }
        if (!draw) continue;
        const auto vit = edgeVerts.find(key);
        if (vit == edgeVerts.end()) continue;
        const u32 x = vit->second.first, y = vit->second.second;
        lines.push_back({md.positions[usize(x)*3+0], md.positions[usize(x)*3+1], md.positions[usize(x)*3+2],
                         kSelectionColor.x, kSelectionColor.y, kSelectionColor.z});
        lines.push_back({md.positions[usize(y)*3+0], md.positions[usize(y)*3+1], md.positions[usize(y)*3+2],
                         kSelectionColor.x, kSelectionColor.y, kSelectionColor.z});
    }

    const rhi::LineHandle h = lines.empty() ? 0
                            : e.device()->createLineMesh(lines.data(), static_cast<u32>(lines.size()));
    AVER_INFO("[Editor] selection outline for '{}': {} edge(s) of {} triangle(s)",
              pit->second, lines.size() / 2, triCount);
    selOutlineLines_[meshId] = h;
    return h;
#else
    // No material system, no cache: 0 already means "no outline" to every caller (SandboxRender.cpp
    // treats a zero handle as nothing to draw), so a PBR-less build reports exactly that.
    (void)e; (void)meshId;
    return 0;
#endif
}

// Where the terrain block starts -- it used to start above selectionOutlineLines(), which sat in
// the guard only because it's defined next to the sculpt code. That function names no landscape_
// member and is unrelated to whether terrain is built in; it isn't guarded because an
// AVER_MODULE_LANDSCAPE=OFF build died on a mismatch with its unguarded call site. Everything from
// here down reads landscape_.
#if AVER_MODULE_LANDSCAPE
// Replaces every height in the section with the noise params the panel is showing, as ONE undo
// entry (same LandscapeStroke machinery a brush stroke uses). Heights are computed into a LOCAL
// vector first; applyHeightRect only runs (section write, bounds/tree rebuild, GPU invalidation)
// once the comparison against landBefore confirms something actually changed.
void SandboxApp::generateLandscapeNoise(Engine& e) {
    (void)e;
    if (!landscape_.loaded() || landscape_.data().sampleCount == 0) return;
    const u32 n = landscape_.data().sampleCount;

    EditCmd c;
    c.kind = EditCmd::Kind::LandscapeStroke;
    c.label = "Generate terrain";
    c.landX0 = 0; c.landY0 = 0; c.landX1 = n - 1; c.landY1 = n - 1;
    c.landBefore = landscape_.data().heights;

    c.landAfter.resize(static_cast<size_t>(n) * n);
    for (u32 y = 0; y < n; ++y) {
        for (u32 x = 0; x < n; ++x) {
            const f32 wx = landscape_.data().originCm[0] + static_cast<f32>(x) * landscape_.data().spacingCm;
            const f32 wy = landscape_.data().originCm[1] + static_cast<f32>(y) * landscape_.data().spacingCm;
            c.landAfter[static_cast<size_t>(y) * n + x] =
                landscape::terrainHeightAt(wx, wy, landscape_.noiseParams());
        }
    }
    if (c.landBefore == c.landAfter) return;

    landscape_.applyHeightRect(0, 0, n - 1, n - 1, c.landAfter);
    pushEdit(std::move(c));
    AVER_INFO("[Landscape] generated {}x{} samples from noise (seed {}, {} octaves)",
              n, n, landscape_.noiseParams().seed, landscape_.noiseParams().octaves);
}

// Starts a stroke: remembers that nothing has been touched yet. GameLandscape captures the BEFORE
// samples lazily as each sculpt tick grows the rect (GameLandscape::sculpt) rather than up front,
// because at stroke start the rect is not known -- a drag can wander anywhere.
void SandboxApp::beginSculptStroke() {
    sculpting_ = true;
    landscape_.beginStroke();
}

// Ends a stroke and pushes ONE undo entry for the whole thing.
void SandboxApp::endSculptStroke() {
    sculpting_ = false;
    if (!landscape_.strokeActive()) return;
    EditCmd c;
    c.kind = EditCmd::Kind::LandscapeStroke;
    c.label = "Sculpt";
    // A no-op stroke (terrain already at the flatten target, a Smooth pass over a plane) pushes no
    // entry, else every stray click would cost a Ctrl+Z that appears to do nothing; endStroke()
    // returns false for that case too.
    if (!landscape_.endStroke(c.landBefore, c.landAfter, c.landX0, c.landY0, c.landX1, c.landY1)) return;
    pushEdit(std::move(c));
}

// Fills the foliage palette from every .ocfoliage TYPE ASSET under the project's content folder,
// one entry per type (see FoliageSpecies), not per loaded mesh. A type whose meshPath doesn't
// resolve in content_'s meshes is skipped with a warning (mirrors loadProjectMeshes' own reason
// for keying off already-resolved meshes, one layer up): offering it would be a palette entry
// that silently does nothing when picked.
void SandboxApp::refreshFoliagePalette() {
    foliagePalette_.clear();
    const std::string dir = project_.contentDir();
    if (dir.empty()) return;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return;

    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".ocfoliage") continue;

        const std::string full = it->path().string();
        fmt::OcFoliageData type;
        std::string why;
        if (!fmt::loadOcFoliage(full, type, &why)) {
            AVER_WARN("[Foliage] {}", why);
            continue;
        }
        const u64 meshId = fnv1a64(std::string_view(type.meshPath));
        if (content_.meshFor(meshId) == 0) {
            AVER_WARN("[Foliage] '{}' names mesh '{}', which is not loaded -- skipping",
                      full, type.meshPath);
            continue;
        }

        FoliageSpecies sp;
        sp.assetPath = full;
        sp.name = it->path().stem().string();
        sp.type = std::move(type);
        foliagePalette_.push_back(std::move(sp));
    }
    std::sort(foliagePalette_.begin(), foliagePalette_.end(),
              [](const FoliageSpecies& a, const FoliageSpecies& b) { return a.name < b.name; });
    // Only one species ticked, else a project with many types opens with a brush painting a random
    // mix of every asset -- never what anyone wants and many clicks to undo.
    for (size_t i = 0; i < foliagePalette_.size(); ++i) foliagePalette_[i].enabled = (i == 0);
    AVER_INFO("[Foliage] palette: {} type(s) available to scatter", foliagePalette_.size());
}

// A cheap deterministic hash, so a brush stroke is repeatable for the same seed and cursor.
 f32 SandboxApp::foliageRand(u32& state) {
    state = state * 1664525u + 1013904223u;
    return static_cast<f32>((state >> 8) & 0xFFFFFFu) / 16777216.0f;
}

#endif

#if AVER_MODULE_LANDSCAPE
#if AVER_MODULE_SCENE && AVER_WITH_IMGUI
// The foliage brush: scatter meshes across the terrain under the cursor, or erase them. NOT the
// world module's procedural scatter (world::ScatterPalette / GeneratedChunkSource, density-field
// driven, authored as SCATTER records -- "cover this region by rule") -- this is "put some here,
// by hand"; the two are complementary. Placements are ordinary scene entities: they save, select,
// move and undo.
void SandboxApp::handleFoliage(Engine& e, const ImGuiIO& io, bool overScene, f32 mx, f32 my) {
    sculptCursorValid_ = false;
    if (!landscape_.loaded() || foliagePalette_.empty()) { foliageStroking_ = false; return; }

    Vec3 ro, rd;
    viewportRay(mx, my, ro, rd);
    const f32 roA[3] = {ro.x, ro.y, ro.z}, rdA[3] = {rd.x, rd.y, rd.z};
    landscape::HeightfieldHit hit;
    const bool haveHit = overScene && landscape::raycastHeightfield(landscape_.data(), roA, rdA, hit);
    if (haveHit) {
        sculptCursorValid_ = true;   // the same cursor ring the sculpt brush draws
        sculptCursor_ = Vec3{hit.posCm[0], hit.posCm[1], hit.posCm[2]};
    }

    const bool erasing = io.KeyShift || foliageErase_;
    if (ImGui::IsMouseClicked(0) && overScene && haveHit) {
        foliageStroking_ = true;
        foliageBatch_ = EditCmd{};
        foliageBatch_.kind = EditCmd::Kind::FoliageStroke;
        foliageBatch_.label = erasing ? "Erase foliage" : "Paint foliage";
        foliageBatch_.batchWasErase = erasing;
    }
    if (!io.MouseDown[0] && foliageStroking_) {
        foliageStroking_ = false;
        if (!foliageBatch_.batchIds.empty()) pushEdit(std::move(foliageBatch_));
        foliageBatch_ = EditCmd{};
    }
    if (!foliageStroking_ || !haveHit) return;

    // Rate-limited by time, not by frame: at 250 fps an unlimited brush would place hundreds of
    // instances in the time it takes to notice, and every one of them is a real entity.
    const f32 dt = std::fmin(e.time().dt, 0.05f);
    foliageAccum_ += dt * foliageDensity_;
    int attempts = static_cast<int>(foliageAccum_);
    if (attempts <= 0) return;
    foliageAccum_ -= static_cast<f32>(attempts);
    attempts = std::min(attempts, 16);   // a stall must not become a burst

    if (erasing) { foliageErase(hit.posCm[0], hit.posCm[1]); return; }
    for (int i = 0; i < attempts; ++i) foliagePlaceOne(e, hit.posCm[0], hit.posCm[1]);
}

// One placement attempt inside the brush disc. Rejected if too close to something already there
// (per the picked species' own collisionRadiusCm) -- stops a held brush from stacking meshes.
//
// Not split into a "pick a species" helper: a member function's signature is resolved at its
// declaration point, not deferred to complete-class context like a body is, so a helper returning
// `FoliageSpecies*` declared before FoliageSpecies itself (defined later in this member block)
// wouldn't compile -- everything referencing the type stays inside a function body instead, as the
// rest of this class already does for the same reason.
void SandboxApp::foliagePlaceOne(Engine& e, f32 cx, f32 cy) {
    std::vector<const FoliageSpecies*> live;
    for (const auto& sp : foliagePalette_) if (sp.enabled) live.push_back(&sp);
    if (live.empty()) return;

    // Uniform over the disc, not the square: sqrt on the radius avoids clumping toward the centre.
    const f32 ang = foliageRand(foliageSeed_) * 6.2831853f;
    const f32 rad = std::sqrt(foliageRand(foliageSeed_)) * foliageRadiusCm_;
    const f32 x = cx + std::cos(ang) * rad;
    const f32 y = cy + std::sin(ang) * rad;

    f32 z = 0.0f;
    if (!landscape::surfaceHeightAt(landscape_.data(), x, y, z)) return;   // off the section

    // Weighted by type.weight, mirroring ChunkGenerator.cpp's pickSpecies minus the density-band
    // test (meaningless here, see OcFoliage.hpp). Weight <= 0 is never picked (matches
    // ScatterSpecies); if EVERY live species has weight <= 0, falls back to a uniform pick rather
    // than placing nothing -- ticking a species should never silently stop it being chosen. Decided
    // before the collision test below because collisionRadiusCm/scale are now per-type, replacing
    // the single global spacing value this had before.
    f32 totalWeight = 0.0f;
    for (const FoliageSpecies* s : live) if (s->type.weight > 0.0f) totalWeight += s->type.weight;
    const FoliageSpecies* picked = nullptr;
    if (totalWeight > 0.0f) {
        f32 pick = foliageRand(foliageSeed_) * totalWeight;
        for (const FoliageSpecies* s : live) {
            if (s->type.weight <= 0.0f) continue;
            if (pick < s->type.weight) { picked = s; break; }
            pick -= s->type.weight;
        }
        if (!picked) picked = live.back();   // float rounding at the very top of the range
    } else {
        picked = live[static_cast<size_t>(foliageRand(foliageSeed_) * live.size()) % live.size()];
    }
    const FoliageSpecies& sp = *picked;

    const u64 meshId = fnv1a64(std::string_view(sp.type.meshPath));
    if (content_.meshFor(meshId) == 0) return;

    const f32 sc = sp.type.scaleMin +
                   foliageRand(foliageSeed_) * (sp.type.scaleMax - sp.type.scaleMin);

    // Interpenetration check, mirroring ChunkGenerator.cpp's placedSolid: SUMMED radii (this
    // instance's scaled radius plus the neighbour's). A neighbour placed with collisionRadiusCm ==
    // 0 (grass, other overlap-tolerant fill) never entered the check, so it neither blocks nor is blocked.
    if (sp.type.collisionRadiusCm > 0.0f) {
        const f32 r = sp.type.collisionRadiusCm * sc;
        for (const FoliagePlaced& p : foliagePlaced_) {
            if (!(p.solidRadiusCm > 0.0f)) continue;
            const f32 dx = p.pos.x - x, dy = p.pos.y - y;
            const f32 minDist = p.solidRadiusCm + r;
            if (dx*dx + dy*dy < minDist*minDist) return;
        }
    }

    Transform xf;
    xf.position = Vec3{x, y, z};
    // "Align to slope" is per-type (sp.type.alignToNormal), not a palette-wide checkbox --
    // foliagePlacementRotation (FoliageAlign.hpp) is unchanged, only where alignToNormal comes from.
    // Yaw is a random azimuth unless randomizeYaw is false (then every instance faces the same way).
    // eps=10cm sits inside a section's sample spacing (OcLandData::spacingCm), keeping the four
    // probes local to this instance's patch rather than blurring across samples.
    const f32 yaw = sp.type.randomizeYaw ? foliageRand(foliageSeed_) * 6.2831853f : 0.0f;
    xf.rotation = editor::foliagePlacementRotation(landscape_.data(), x, y, /*epsCm=*/10.0f, yaw,
                                                    sp.type.alignToNormal);
    xf.scale = Vec3{sc, sc, sc};

    scene::World& world = scene::World::instance();
    const scene::Entity ent = world.create(sp.type.meshPath, scene::kInvalidEntity, xf);
    if (ent == scene::kInvalidEntity) return;
    if (auto* mr = static_cast<scene::CMeshRenderer*>(
            world.addComponent(ent, scene::kComponentMeshRenderer))) {
        mr->mesh = meshId;
        mr->flags |= scene::kMeshRendererVisible;
        mr->aabbMin[0] = mr->aabbMin[1] = mr->aabbMin[2] = -1.0f;
        mr->aabbMax[0] = mr->aabbMax[1] = mr->aabbMax[2] =  1.0f;
        // Material override; empty means the mesh's own cooked material (OcFoliageData's documented
        // default) -- previously not honoured at all, since the old ad hoc FoliageSpecies had no
        // material field. Same resolve-and-bind pair LevelInstance.cpp uses for an .ocworld PLACE's `material`.
        if (!sp.type.material.empty()) {
            mr->material = aver_scene_material(0, sp.type.material.c_str());
#if AVER_MODULE_PBR && AVER_MODULE_SCENE
            if (mr->material) {
                const pbr::MaterialHandle h = content_.materialForSurface(sp.type.material);
                if (h) content_.bindSurfaceMaterial(mr->material, h);
            }
#endif
        }
    }
    levelEntities_.push_back(ent);
    entityLabels_[static_cast<u32>(ent)] = makeEntityLabel(sp.type.material, sp.type.meshPath);
    foliagePlaced_.push_back({xf.position,
                               sp.type.collisionRadiusCm > 0.0f ? sp.type.collisionRadiusCm * sc : 0.0f});

    EditCmd one = describeEntity(ent);
    foliageBatch_.batchSnaps.push_back(one.snap);
    foliageBatch_.batchIds.push_back(one.id);
    (void)e;
}

// Removes painted instances within the brush; only entities tracked in foliagePlaced_, so an erase
// pass cannot delete level geometry that merely happens to be under the cursor.
void SandboxApp::foliageErase(f32 cx, f32 cy) {
    const f32 rSq = foliageRadiusCm_ * foliageRadiusCm_;
    scene::World& world = scene::World::instance();
    for (size_t i = levelEntities_.size(); i-- > 0;) {
        const scene::Entity ent = levelEntities_[i];
        if (!world.valid(ent)) continue;
        const Transform& xf = world.localTransform(ent);
        const f32 dx = xf.position.x - cx, dy = xf.position.y - cy;
        if (dx*dx + dy*dy > rSq) continue;
        bool mine = false;
        for (size_t k = 0; k < foliagePlaced_.size(); ++k) {
            const Vec3& p = foliagePlaced_[k].pos;
            if (std::fabs(p.x - xf.position.x) < 1.0f && std::fabs(p.y - xf.position.y) < 1.0f) {
                mine = true;
                foliagePlaced_.erase(foliagePlaced_.begin() + static_cast<long>(k));
                break;
            }
        }
        if (!mine) continue;
        EditCmd one = describeEntity(ent);
        foliageBatch_.batchSnaps.push_back(one.snap);
        foliageBatch_.batchIds.push_back(one.id);
        destroyEntity(ent);
    }
}

#endif
#endif

#if AVER_MODULE_SCENE

#endif

// Re-finds the Player Start by NAME (see makePlayerStart). Re-derived rather than trusted, because
// destroying the marker, undoing that destroy (new handle) or redoing it invalidates playerStart_
// without touching this cache -- a stale handle points at nothing (Add > Player Start then refuses
// a second one, thinking it exists) or at a reused id the viewport draws a spawn icon on. Cheap:
// one pass over the level's entities, only on undo/redo/delete.
//
// FIXED (was a real bug, not cosmetic): both comparisons used to be `w.name(x) == "PlayerStart"` --
// World::name returns `const char*`, so `==` against a literal compared POINTERS, never characters;
// playerStart_ was silently reset to kInvalidEntity on every undo/redo/delete. Now std::string_view,
// which compares content.
//
// Not world::find("PlayerStart"): that scans every live entity in the process-global World, not
// just this level's levelEntities_ (the level-ownership boundary isLevelOwned() is keyed on) -- a
// same-named entity outside this level would make find() return the wrong handle silently.
void SandboxApp::refreshPlayerStart() {
#if AVER_MODULE_SCENE
    // Logic lives in editor::refreshPlayerStart (PlayerStartRefresh.hpp), a free function over
    // plain scene::World + std::vector<Entity> so a headless test can exercise it directly (see
    // that header for the pointer-comparison bug this used to hide).
    playerStart_ = editor::refreshPlayerStart(scene::World::instance(), playerStart_, levelEntities_);
#endif
}

// The spawn transform a level should use: the marker if one is live, else the loaded SPAWN
// record, else nothing. Returns false when the level declares no spawn at all.
bool SandboxApp::playerStartTransform(Vec3& outPos, f32& outYawDeg) const {
#if AVER_MODULE_SCENE
    const scene::World& w = scene::World::instance();
    if (playerStart_ != scene::kInvalidEntity && w.valid(playerStart_)) {
        outPos = w.localTransform(playerStart_).position;
        outYawDeg = playerStartYaw_;
        return true;
    }
#endif
    if (!levelHeader_.hasSpawn) return false;
    outPos = Vec3{static_cast<f32>(levelHeader_.spawnX), static_cast<f32>(levelHeader_.spawnY),
                  static_cast<f32>(levelHeader_.spawnZ)};
    outYawDeg = static_cast<f32>(levelHeader_.spawnYaw);
    return true;
}

#if AVER_MODULE_SCENE
// Creates the marker entity at a world position. Shared by "Add > Player Start" and by the
// level loader when a file already carries a SPAWN record.
scene::Entity SandboxApp::makePlayerStart(const Vec3& at, f32 yawDeg) {
    scene::World& world = scene::World::instance();
    Transform xf;
    xf.position = at;
    xf.rotation = quatFromEulerDeg(Vec3{0.0f, 0.0f, yawDeg});
    xf.scale = Vec3{kEditorCubeHalf, kEditorCubeHalf, kEditorCubeHalf};

    // Drawn as the unit cube because that's the only primitive the editor is guaranteed to have;
    // the NAME is what makes it a PlayerStart. Deliberately not an asset path: the marker is never
    // written as a PLACE record, so nothing tries to resolve it as a mesh file.
    static const std::string kPlayerStartName = "PlayerStart";
    const scene::Entity e = world.create(kPlayerStartName, scene::kInvalidEntity, xf);
    if (e == scene::kInvalidEntity) {
        AVER_WARN("[Editor] Player Start: the world refused a new entity");
        return scene::kInvalidEntity;
    }
    static const std::string kCubeAsset = "Meshes/cube.ocmesh";
    if (auto* mr = static_cast<scene::CMeshRenderer*>(
            world.addComponent(e, scene::kComponentMeshRenderer))) {
        mr->mesh = fnv1a64(std::string_view(kCubeAsset));
        mr->flags |= scene::kMeshRendererVisible;
        mr->aabbMin[0] = mr->aabbMin[1] = -1.0f;
        mr->aabbMax[0] = mr->aabbMax[1] =  1.0f;
        // Taller than the cube it bounds, only in +Z: the marker draws as the capsule/arrow/sprite
        // built in SandboxApp.cpp (drawn in SandboxRender.cpp), standing on the origin, and this box
        // is what framing (F) and the other bounds readers size it by. Clicks test the drawn capsule
        // itself (pick()). Reaches up to the capsule's own top (kPlayerStartCapsuleHalfHeight * 2,
        // in units of this box's own scale, kEditorCubeHalf) and down to -1 (not 0) to also contain the
        // fallback CUBE (icon renderer/PNG unavailable); one box serves both looks.
        mr->aabbMin[2] = -1.0f;
        mr->aabbMax[2] = (kPlayerStartCapsuleHalfHeight * 2.0f) / kEditorCubeHalf;
    }
    entityLabels_[static_cast<u32>(e)] = "Player Start";
    playerStartYaw_ = yawDeg;
    return e;
}

#endif

// "Add > Player Start". One per level: a second call selects the one that exists rather than
// creating a rival the save path would have to choose between.
void SandboxApp::addPlayerStart(Engine&) {
#if AVER_MODULE_SCENE
    scene::World& world = scene::World::instance();
    if (playerStart_ != scene::kInvalidEntity && world.valid(playerStart_)) {
        sel_ = kSelScene; selEntity_ = playerStart_;
        cbStatus_ = "This level already has a Player Start -- selected it";
        AVER_INFO("[Editor] Player Start already exists; selected it rather than adding a second");
        return;
    }
    Vec3 at = camPos_ + camForward() * kAddDistance;
    if (snapMove_) for (int k = 0; k < 3; ++k) (&at.x)[k] = snapf((&at.x)[k], moveSnap_);
    // Faces the camera's direction: atan2 of forward, in the +X-forward/+Y-right frame the level
    // format's yaw is authored in.
    const Vec3 f = camForward();
    const f32 yaw = degrees(std::atan2(f.y, f.x));
    playerStart_ = makePlayerStart(at, yaw);
    if (playerStart_ == scene::kInvalidEntity) return;
    sel_ = kSelScene; selEntity_ = playerStart_;
    // An undo entry -- this was the only Add item that didn't push one (spawnPrimitive and
    // spawnFromAssetDrop both end with describeEntity()+pushEdit(); this didn't), so Ctrl+Z after
    // adding a Player Start silently undid whatever edit came before instead (worse than doing nothing).
    {
        EditCmd c = describeEntity(playerStart_);
        c.kind = EditCmd::Kind::Create;
        pushEdit(std::move(c));
    }
    AVER_INFO("[Editor] Player Start at ({:.0f}, {:.0f}, {:.0f}) yaw {:.0f}",
              at.x, at.y, at.z, yaw);
#else
    (void)0;
#endif
}

// Adds a built-in primitive in front of the camera and selects it. Cube and sphere are both
// synthesised at startup (appendBox/appendSphere) and registered in content_'s meshes/bounds and
// meshTris_, so "Add > Sphere" needed no new asset/loader/bounds, only this function to stop
// hardcoding the cube -- it had stayed disabled in the menu as long as sphere.ocmesh was already a
// registered built-in.
void SandboxApp::spawnPrimitive(Engine& engine, const char* assetPath, const char* label) {
    (void)engine;
    const Vec3 at = camPos_ + camForward() * kAddDistance;
#if AVER_MODULE_SCENE
    if (hideEditorScene_ || !levelPath_.empty()) {
        scene::World& world = scene::World::instance();
        Transform xf;
        xf.position = at;
        if (snapMove_) for (int k=0;k<3;++k) (&xf.position.x)[k] = snapf((&xf.position.x)[k], moveSnap_);
        xf.rotation = Quat{0,0,0,1};
        // FROZEN: scale is the half-extent in cm, matching .ocworld PLACEG against the unit cube.
        xf.scale = Vec3{kEditorCubeHalf, kEditorCubeHalf, kEditorCubeHalf};

        // FROZEN: the entity name is the asset path saveLevel writes and loadOcworld hashes back.
        const std::string kCubeAsset = assetPath;
        const scene::Entity e = world.create(kCubeAsset, scene::kInvalidEntity, xf);
        if (e == scene::kInvalidEntity) { AVER_WARN("[Editor] Add: the world refused a new entity"); return; }
        if (auto* mr = static_cast<scene::CMeshRenderer*>(
                world.addComponent(e, scene::kComponentMeshRenderer))) {
            mr->mesh = fnv1a64(std::string_view(kCubeAsset));
            mr->flags |= scene::kMeshRendererVisible;
            mr->aabbMin[0] = mr->aabbMin[1] = mr->aabbMin[2] = -1.0f;
            mr->aabbMax[0] = mr->aabbMax[1] = mr->aabbMax[2] =  1.0f;
        }
        levelEntities_.push_back(e);
        entityLabels_[static_cast<u32>(e)] = makeEntityLabel(std::string(), kCubeAsset);
#if AVER_MODULE_PHYSICS
        // Collides like a loaded placement would: a fresh entity has no entityCollide_ entry, which
        // reads as the default (true) everywhere else consulted, so it gets the same body a level
        // file's own placement gets. Before describeEntity() below so the pushed Create's hadBody matches what was built.
        rebuildEntityBody(e);
#endif
        sel_ = kSelScene; selEntity_ = e;
        {
            EditCmd c = describeEntity(e);
            c.kind = EditCmd::Kind::Create;
            pushEdit(std::move(c));
        }
        AVER_INFO("[Editor] added {} entity #{} at ({:.0f}, {:.0f}, {:.0f})",
                  label, (u32)e, xf.position.x, xf.position.y, xf.position.z);
        return;
    }
#endif
    // The placeholder world (no level loaded) keeps its OWN cube mesh at kEditorCubeHalf, not the
    // unit one content_ registers. The two cubes are different sizes -- cubeMesh_ is appendBox at
    // half-extent 50 drawn at scale 1; content_'s "Meshes/cube.ocmesh" is the UNIT cube (.ocworld
    // PLACEG scales are half-extents in cm applied to it). Looking it up in content_ here would
    // silently shrink it 50x, so the cube keeps its own handle; anything else comes from the
    // registry scaled up to match it.
    rhi::MeshHandle mesh = cubeMesh_;
    u32 tris = cubeTris_;
    Vec3 scale{1, 1, 1};
    const u64 assetId = fnv1a64(std::string_view(assetPath));
    if (assetId != fnv1a64(std::string_view("Meshes/cube.ocmesh"))) {
        const rhi::MeshHandle found = content_.meshFor(assetId);
        if (!found) {
            AVER_WARN("[Editor] Add: no built-in mesh registered for '{}'", assetPath);
            return;
        }
        mesh = found;
#if AVER_MODULE_SCENE
        const auto trisIt = meshTris_.find(assetId);
        tris = trisIt != meshTris_.end() ? trisIt->second : 0;
#else
        // meshTris_ is filled by onMeshLoaded (SandboxAssets.cpp), which only runs under
        // AVER_MODULE_SCENE, so a scene-less build has no per-mesh count; 0 here is honest, vs.
        // reporting the CUBE's count for a sphere.
        tris = 0;
#endif
        scale = Vec3{kEditorCubeHalf, kEditorCubeHalf, kEditorCubeHalf};   // unit mesh -> cube's size
    }
    if (!mesh) return;
    MeshObj c; c.mesh = mesh; c.tris = tris; c.scale = scale;
    c.name = std::string(label) + " " + std::to_string(++spawnCount_);
    c.pos = at;
    if (snapMove_) for (int k=0;k<3;++k) (&c.pos.x)[k] = snapf((&c.pos.x)[k], moveSnap_);
    c.color[0]=0.72f; c.color[1]=0.72f; c.color[2]=0.74f; c.metallic=0.0f; c.roughness=0.6f;
    objects_.push_back(c);
    // Unguarded: the placeholder-world fallback, reached with SCENE off too.
    sel_ = (int)objects_.size() - 1; selEntity_ = kInvalidId;
    // Pushes a CreateObj undo entry (this branch used to push none, so a placeholder cube add was
    // silently non-undoable). Mirrors the scene branch's describeEntity()+pushEdit() tail, minus
    // the EditId/World indirection only the scene entity needs.
    EditCmd edit;
    edit.kind = EditCmd::Kind::CreateObj;
    edit.objIndex = sel_;
    edit.objSnapshot = c;
    pushEdit(std::move(edit));
}

// The original name, kept so every existing caller (menus, --undo-test, --spawn-test)
// is untouched by the parameterisation above.
void SandboxApp::spawnCube(Engine& engine) { spawnPrimitive(engine, "Meshes/cube.ocmesh", "Cube"); }

#if AVER_WITH_IMGUI
#if AVER_MODULE_SCENE
// Finds a finite world point to drop an asset at, from a screen mouse position. Order: nearest
// scene-entity hit, else the ground plane, else a fixed distance along the ray. Z is up in this
// engine (averAtmoCamAlt()), so the ground plane is Z = 0, not Y = 0.
//
// `onSurface` tells the caller to rest the new object on what it hit rather than leave its origin
// buried (see restOnSurface). Ground plane reports true (Z=0 IS a surface); the in-front-of-camera
// "ray points at the sky" fallback reports false -- nothing there to sit on.
Vec3 SandboxApp::dropWorldPoint(f32 screenX, f32 screenY, bool* onSurface) const {
    if (onSurface) *onSurface = true;
    Vec3 ro, rd;
    viewportRay(screenX, screenY, ro, rd);

    f32 bestT = 1e30f; bool hit = false;

    // No test against the placeholder Floor/Cube, deliberately: they're on screen only while
    // hideEditorScene_ is false ("no level open"), and spawnFromAssetDrop refuses a drop then -- a
    // loop over them here could never run. Written down because the obvious next edit is to add one.

    // FIXED: this loop used to sit inside `if (!hideEditorScene_)`, which is true exactly WHEN THE
    // LEVEL HAS ENTITIES -- so it tested the level's objects only while there were none; the moment
    // there was anything to land on, the drop fell through to the ground plane instead (aim at a
    // crate, get the floor). Now unconditional, mirroring pick()'s placeholders-guarded/scene-unconditional split.
    {
        scene::World& w = scene::World::instance();
        const u32 n = w.count();
        for (u32 i = 0; i < n; ++i) {
            const scene::Entity ent = w.at(i);
            if (!w.valid(ent) || w.destroyPending(ent)) continue;
            const auto* mr = w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
            if (!mr || !(mr->flags & scene::kMeshRendererVisible) || mr->mesh == 0) continue;
            if (content_.meshFor(mr->mesh) == 0) continue;
            Vec3 lmin{mr->aabbMin[0], mr->aabbMin[1], mr->aabbMin[2]};
            Vec3 lmax{mr->aabbMax[0], mr->aabbMax[1], mr->aabbMax[2]};
            if (!(lmax.x > lmin.x && lmax.y > lmin.y && lmax.z > lmin.z)) { lmin = Vec3{-1,-1,-1}; lmax = Vec3{1,1,1}; }
            const Mat4 iw = w.worldMatrix(ent).inverse();
            const Vec3 lo = xformPoint(iw, ro), ld = xformVec(iw, rd);
            f32 t; if (rayAabb(lo, ld, lmin, lmax, t) && t > 0.0f && t < bestT) { bestT = t; hit = true; }
        }
    }
    if (hit) {
        const Vec3 p = ro + rd * bestT;
        if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) return p;
    }

    // Ground plane: Z = 0. Reached only when the ray met nothing at all, now that it actually
    // tests what is in front of it.
    if (std::fabs(rd.z) > 1e-6f) {
        const f32 t = -ro.z / rd.z;
        if (t > 0.0f) {
            const Vec3 p = ro + rd * t;
            if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) return p;
        }
    }

    // The ray is parallel to (or points away from) the ground: fall back to a fixed distance
    // in front of the camera, matching spawnCube()'s placement. Nothing was hit, so nothing is
    // underneath to rest on -- the object goes exactly where the camera is pointing.
    if (onSurface) *onSurface = false;
    return camPos_ + camForward() * kAddDistance;
}

// The first SURFACE under a screen point, for Play From Here: whichever the ray meets first of the
// level's triangles (pick()'s own broadphase + rayPickGeometry), the landscape's heightfield and the
// placeholder boxes. False on a miss (sky, or nothing under the cursor).
// NOT dropWorldPoint: that tests bounding BOXES, so aiming at the floor of a room reports the top of the
// building's box, and its Z = 0 plane and in-front-of-camera fallbacks are places to put an asset, not
// somewhere to stand.
// Keeps pick()'s start-inside rule (skipBackFaces = insideBox), so the walls of a room the camera is
// standing in never block the floor it is aiming at. Streamed-chunk entities ARE tested, unlike pick():
// selection avoids them because the streamer may evict one mid-edit, but a pawn can stand on one.
// Skinned and posed meshes are skipped: their rest triangles are not what is drawn, and a bounding box
// around a character is not ground.
bool SandboxApp::pickSurfacePoint(f32 screenX, f32 screenY, Vec3& out) {
    Vec3 ro, rd;
    viewportRay(screenX, screenY, ro, rd);
    f32 bestT = 1e30f;   // along ro + rd*t: the one parameter every test below reports in
    bool hit = false;

#if AVER_MODULE_LANDSCAPE
    if (landscape_.loaded()) {
        const f32 roA[3] = {ro.x, ro.y, ro.z}, rdA[3] = {rd.x, rd.y, rd.z};
        landscape::HeightfieldHit lh;
        const f32 rr = dot(rd, rd);
        if (rr > 0.0f && landscape::raycastHeightfield(landscape_.data(), roA, rdA, lh)) {
            // The heightfield reports a point (and a distance along the NORMALISED ray); the tests below
            // compare in rd's own parameter, so the point is converted back.
            const f32 t = dot(Vec3{lh.posCm[0], lh.posCm[1], lh.posCm[2]} - ro, rd) / rr;
            if (t > 0.0f) { bestT = t; hit = true; }
        }
    }
#endif

    // Placeholder Floor/Cube boxes, bounds-only -- pick()'s own placeholder loop.
    if (!hideEditorScene_)
        for (const MeshObj& o : objects_) {
            if (!o.visible) continue;
            Transform tr; tr.position = o.pos; tr.rotation = quatFromEulerDeg(o.rotDeg); tr.scale = o.scale;
            const Mat4 iw = tr.toMatrix().inverse();
            const Vec3 lo = xformPoint(iw, ro), ld = xformVec(iw, rd);
            f32 t;
            if (rayAabb(lo, ld, o.aabbMin, o.aabbMax, t) && t > 0.0f && t < bestT) { bestT = t; hit = true; }
        }

    {
        struct SurfaceCand { scene::Entity ent; u64 meshId; f32 tBox; bool insideBox; Vec3 lo, ld; };
        std::vector<SurfaceCand> cands;
        scene::World& w = scene::World::instance();
        const u32 n = w.count();
        for (u32 i = 0; i < n; ++i) {
            const scene::Entity ent = w.at(i);
            if (!w.valid(ent) || w.destroyPending(ent)) continue;
            if (ent == playerStart_) continue;   // a stand-in cube around its feet, not what is drawn
            const auto* mr = w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
            if (!mr || !(mr->flags & scene::kMeshRendererVisible) || mr->mesh == 0) continue;
            if (content_.meshFor(mr->mesh) == 0) continue;
            if (skinnedMeshIds_.count(mr->mesh) != 0 || posedHandle(ent) != 0) continue;
            Vec3 lmin{mr->aabbMin[0], mr->aabbMin[1], mr->aabbMin[2]};
            Vec3 lmax{mr->aabbMax[0], mr->aabbMax[1], mr->aabbMax[2]};
            if (!(lmax.x > lmin.x && lmax.y > lmin.y && lmax.z > lmin.z)) { lmin = Vec3{-1,-1,-1}; lmax = Vec3{1,1,1}; }
            const Mat4 iw = w.worldMatrix(ent).inverse();
            const Vec3 lo = xformPoint(iw, ro), ld = xformVec(iw, rd);
            f32 tBox;
            if (!rayAabb(lo, ld, lmin, lmax, tBox)) continue;
            cands.push_back({ent, mr->mesh, tBox, tBox <= 0.0f, lo, ld});
        }
        // Nearest box first, stopping once a box can no longer beat the best hit (pick()'s early-out).
        std::sort(cands.begin(), cands.end(),
                  [](const SurfaceCand& a, const SurfaceCand& b) { return a.tBox < b.tBox; });
        for (const SurfaceCand& c : cands) {
            if (c.tBox >= bestT) break;
            const aver::editor::PickGeometry& geo = pickGeometryFor(c.meshId);
            if (geo.empty()) {
                // No triangles resident for this mesh: its box is the best surface known.
                if (c.tBox > 0.0f) { bestT = c.tBox; hit = true; }
                continue;
            }
            f32 tTri;
            if (aver::editor::rayPickGeometry(geo, c.lo, c.ld, /*skipBackFaces=*/c.insideBox, bestT, tTri))
                { bestT = tTri; hit = true; }
        }
    }

    if (!hit) return false;
    const Vec3 p = ro + rd * bestT;
    if (!(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z))) return false;
    out = p;
    return true;
}

// Places a content-browser asset dropped on the viewport at a screen-space position, following
// spawnCube()'s create -> CMeshRenderer -> levelEntities_ -> undo recipe exactly. Only .ocmesh
// assets are placeable in this slice; anything else is reported, not silently ignored.
void SandboxApp::spawnFromAssetDrop(Engine& e, const std::string& full, f32 screenX, f32 screenY) {
    const std::string ext = lowerExt(std::filesystem::path(full));
    const std::string fileName = std::filesystem::path(full).filename().string();
    const bool isMesh = ext == ".ocmesh";
    bool isParticle = false;
#if AVER_MODULE_PARTICLES
    isParticle = ext == ".ocparticle";
#endif
    if (!isMesh && !isParticle) {
#if AVER_MODULE_PARTICLES
        cbStatus_ = "'" + fileName + "' can't be placed in the level (only .ocmesh/.ocparticle assets can)";
        AVER_WARN("[Editor] drop: '{}' is not a placeable asset (need .ocmesh or .ocparticle)", full);
#else
        cbStatus_ = "'" + fileName + "' can't be placed in the level (only .ocmesh assets can)";
        AVER_WARN("[Editor] drop: '{}' is not a placeable asset (need .ocmesh)", full);
#endif
        return;
    }
    if (!(hideEditorScene_ || !levelPath_.empty())) {
        cbStatus_ = "Load a level (or hide the editor scene) before dropping assets";
        AVER_WARN("[Editor] drop: no scene world is active to place '{}' into", full);
        return;
    }

    const std::string content = project_.contentDir();
    std::error_code ec;
    std::string rel = content.empty() ? std::string()
                                      : std::filesystem::relative(full, content, ec).string();
    if (content.empty() || ec || rel.empty()) {
        cbStatus_ = "Could not resolve '" + fileName + "' to a project-relative path";
        AVER_WARN("[Editor] drop: relative() failed for '{}' against content root '{}'", full, content);
        return;
    }
    for (char& c : rel) if (c == '\\') c = '/';

#if AVER_MODULE_PARTICLES
    if (isParticle) {
        // Drop-to-place for particle effects, mirroring the .ocmesh path's drop-target/level-active
        // checks. Loaded directly (not via content_.loadProjectParticleEffects()'s whole-tree
        // walk) so an effect just authored resolves immediately -- same reasoning the mesh path
        // gives for reloading synchronously.
        particles::ParticleEffect fx;
        std::string err;
        if (!fmt::loadOcparticle(full, fx, nullptr, &err)) {
            cbStatus_ = "Could not load '" + fileName + "': " + err;
            AVER_WARN("[Editor] drop: {}", err);
            return;
        }
        const u64 effectId = fnv1a64(std::string_view(rel));
        particles::particleEffects().set(effectId, fx);

        const Vec3 at = dropWorldPoint(screenX, screenY);
        if (!(std::isfinite(at.x) && std::isfinite(at.y) && std::isfinite(at.z))) {
            cbStatus_ = "Could not find a valid drop position";
            AVER_WARN("[Editor] drop: computed a non-finite world position for '{}'", rel);
            return;
        }

        scene::World& world = scene::World::instance();
        Transform xf;
        xf.position = at;
        if (snapMove_) for (int k=0;k<3;++k) (&xf.position.x)[k] = snapf((&xf.position.x)[k], moveSnap_);

        const scene::Entity ent = world.create(rel, scene::kInvalidEntity, xf);
        if (ent == scene::kInvalidEntity) {
            cbStatus_ = "The world refused to place '" + rel + "'";
            AVER_WARN("[Editor] drop: the world refused a new entity for '{}'", rel);
            return;
        }
        if (auto* pe = static_cast<scene::CParticleEmitter*>(
                world.addComponent(ent, scene::kComponentParticleEmitter))) {
            pe->effect = effectId;
        }
        levelEntities_.push_back(ent);
        entityLabels_[static_cast<u32>(ent)] = makeEntityLabel(std::string(), rel);
        sel_ = kSelScene; selEntity_ = ent;
        {
            EditCmd c = describeEntity(ent);
            c.kind = EditCmd::Kind::Create;
            pushEdit(std::move(c));
        }
        cbStatus_ = "Placed " + fileName;
        AVER_INFO("[Editor] placed particle emitter entity #{} from '{}' around effect 0x{:016X} "
                  "at ({:.0f}, {:.0f}, {:.0f})",
                  (u32)ent, rel, effectId, xf.position.x, xf.position.y, xf.position.z);
        return;
    }
#endif

    const u64 meshId = fnv1a64(std::string_view(rel));
    if (content_.meshFor(meshId) == 0) {
        // Not loaded yet -- likely imported moments ago. Reload synchronously (same release+load
        // pair buildUI() runs for wantMeshReload_) rather than wait a frame, so this drop lands.
        releaseProjectMeshes(e);
        loadProjectMeshes(e);
    }
    if (content_.meshFor(meshId) == 0) {
        cbStatus_ = "Could not resolve '" + rel + "' to a loaded mesh";
        AVER_WARN("[Editor] drop: '{}' (id {}) is not a loaded scene mesh", rel, meshId);
        return;
    }

    bool onSurface = false;
    const Vec3 at = dropWorldPoint(screenX, screenY, &onSurface);
    if (!(std::isfinite(at.x) && std::isfinite(at.y) && std::isfinite(at.z))) {
        cbStatus_ = "Could not find a valid drop position";
        AVER_WARN("[Editor] drop: computed a non-finite world position for '{}'", rel);
        return;
    }

    scene::World& world = scene::World::instance();
    Transform xf;
    xf.position = at;
    xf.rotation = Quat{0,0,0,1};
    xf.scale = Vec3{1,1,1};

    // Rests on top of what it landed on, applied BEFORE the snap so the snap quantises the final
    // position, not a value the lift then knocks off grid. A mesh with no known bounds (content_'s
    // bounds fill at load from the .ocmesh header; a miss means nothing measured it) is left where
    // it landed -- inventing a lift would move it for a reason nobody could see.
    f32 lift = 0.0f;
    if (onSurface) {
        if (const auto* b = content_.boundsFor(meshId))
            lift = editor::dropRestLift(b->first.z, xf.scale.z);
    }
    xf.position.z += lift;
    if (snapMove_) for (int k=0;k<3;++k) (&xf.position.x)[k] = snapf((&xf.position.x)[k], moveSnap_);

    const scene::Entity ent = world.create(rel, scene::kInvalidEntity, xf);
    if (ent == scene::kInvalidEntity) {
        cbStatus_ = "The world refused to place '" + rel + "'";
        AVER_WARN("[Editor] drop: the world refused a new entity for '{}'", rel);
        return;
    }
    if (auto* mr = static_cast<scene::CMeshRenderer*>(
            world.addComponent(ent, scene::kComponentMeshRenderer))) {
        mr->mesh = meshId;
        mr->flags |= scene::kMeshRendererVisible;
        mr->aabbMin[0] = mr->aabbMin[1] = mr->aabbMin[2] = -1.0f;
        mr->aabbMax[0] = mr->aabbMax[1] = mr->aabbMax[2] =  1.0f;
    }
    levelEntities_.push_back(ent);
    entityLabels_[static_cast<u32>(ent)] = makeEntityLabel(std::string(), rel);
#if AVER_MODULE_PHYSICS
    // Collides like a loaded placement would (see spawnPrimitive's identical call); mesh-drop
    // branch only, the .ocparticle branch above has no CMeshRenderer. Before describeEntity() so hadBody matches what was built.
    rebuildEntityBody(ent);
#endif
    sel_ = kSelScene; selEntity_ = ent;
    {
        EditCmd c = describeEntity(ent);
        c.kind = EditCmd::Kind::Create;
        pushEdit(std::move(c));
    }
    cbStatus_ = "Placed " + fileName;
    AVER_INFO("[Editor] placed entity #{} from '{}' at ({:.0f}, {:.0f}, {:.0f}){}",
              (u32)ent, rel, xf.position.x, xf.position.y, xf.position.z,
              lift > 0.0f ? " -- lifted " + std::to_string((int)lift) + "cm to rest on the surface"
                          : std::string());
}

#endif
#endif

f32 SandboxApp::gizmoLen(const Vec3& origin) const { f32 L = dist(eye_, origin) * 0.17f; return L < 50.0f ? 50.0f : L; }

// Projects a world point to viewport pixels (row-vector clip = p * viewProj). False when behind.
bool SandboxApp::project(const Vec3& wp, f32& sx, f32& sy) const {
    const Mat4& m = viewProj_;
    const f32 x = wp.x*m.m[0][0]+wp.y*m.m[1][0]+wp.z*m.m[2][0]+m.m[3][0];
    const f32 y = wp.x*m.m[0][1]+wp.y*m.m[1][1]+wp.z*m.m[2][1]+m.m[3][1];
    const f32 w = wp.x*m.m[0][3]+wp.y*m.m[1][3]+wp.z*m.m[2][3]+m.m[3][3];
    if (w <= 1e-4f) return false;
    sx = vpX_ + (x / w * 0.5f + 0.5f) * vpW_;
    sy = vpY_ + (1.0f - (y / w * 0.5f + 0.5f)) * vpH_;
    return true;
}

// Returns the distance from a point to a 2D line segment.
 f32 SandboxApp::distToSeg(f32 px, f32 py, f32 ax, f32 ay, f32 bx, f32 by) {
    const f32 vx=bx-ax, vy=by-ay, wx=px-ax, wy=py-ay;
    const f32 len2=vx*vx+vy*vy; f32 t = len2>1e-6f ? (wx*vx+wy*vy)/len2 : 0.0f;
    t = t<0?0:(t>1?1:t); const f32 cx=ax+vx*t, cy=ay+vy*t;
    return std::sqrt((px-cx)*(px-cx)+(py-cy)*(py-cy));
}

// Gizmo's three axes in world space, for the current tool and World/Local button. Scale is ALWAYS
// local regardless: applyScale writes o.scale.x/y/z (the object's own axes), so a world-axis handle
// on a rotated object would drag one way while the number changed another. UE hides World here too.
void SandboxApp::gizmoBasis(const EditXform& o, Vec3 ax[3]) const {
    if (worldSpace_ && tool_ != Tool::Scale) {
        for (int a = 0; a < 3; ++a) ax[a] = kAxisDir[a];
        return;
    }
    const Quat q = quatFromEulerDeg(o.rotDeg);
    for (int a = 0; a < 3; ++a) ax[a] = q.rotate(kAxisDir[a]).getSafeNormal();
}

// Returns which gizmo handle is under the cursor: 0..2 axis, 3 = centre, -1 = none.
int SandboxApp::pickAxis(const Vec3& origin, const Vec3 ax[3], f32 L, f32 mx, f32 my) const {
    f32 ox, oy; if (!project(origin, ox, oy)) return -1;
    const f32 thr = 16.0f * dpi_;
    if (tool_ == Tool::Rotate) {
        int best=-1; f32 bestD=thr;
        for (int a=0;a<3;++a) {
            const Vec3 P=ax[(a+1)%3], Q=ax[(a+2)%3];
            f32 pxx=0, pyy=0; bool havePrev=false, first=true; f32 dmin=1e9f;
            const int N=48;
            for (int k=0;k<=N;++k) {
                const f32 t=k*(kTwoPi/N);
                Vec3 wp = origin + (P*std::cos(t) + Q*std::sin(t)) * L; f32 sx, sy;
                if (project(wp, sx, sy)) { if (havePrev && !first) { f32 d=distToSeg(mx,my,pxx,pyy,sx,sy); if (d<dmin) dmin=d; } pxx=sx; pyy=sy; havePrev=true; first=false; }
                else havePrev=false;
            }
            if (dmin<bestD) { bestD=dmin; best=a; }
        }
        return best;
    }
    if (std::sqrt((mx-ox)*(mx-ox)+(my-oy)*(my-oy)) < 13.0f*dpi_) return 3;
    int best=-1; f32 bestD=thr;
    for (int a=0;a<3;++a) {
        f32 tx, ty; if (!project(origin + ax[a]*L, tx, ty)) continue;
        const f32 d=distToSeg(mx,my,ox,oy,tx,ty);
        if (d<bestD) { bestD=d; best=a; }
    }
    return best;
}

// Moves the transform by a mouse delta in pixels, along the active axis or the screen plane.
void SandboxApp::applyMove(EditXform& o, f32 dx, f32 dy) {
    if (activeAxis_ == 3) {
        const Vec3 fwd = camForward();
        const Vec3 s = cross(Vec3{0,0,1}, fwd).getSafeNormal();
        const Vec3 u = cross(fwd, s);
        const f32 wpp = 2.0f * std::tan(radians(30.0f)) * dist(eye_, o.pos) / (vpH_ > 1 ? vpH_ : 900.0f);
        o.pos += s * (dx * wpp) + u * (-dy * wpp);
    } else {
        Vec3 ax[3]; gizmoBasis(o, ax);
        const Vec3 A = ax[activeAxis_];
        f32 s0x,s0y,s1x,s1y;
        if (project(o.pos, s0x, s0y) && project(o.pos + A, s1x, s1y)) {
            const f32 px=s1x-s0x, py=s1y-s0y, pl2=px*px+py*py;
            if (pl2 > 1e-4f) o.pos += A * ((dx*px + dy*py) / pl2);
        }
    }
    // Grid snap stays in WORLD space even for a local-axis drag: a grid the object isn't aligned to
    // is still the grid the level is built on; snapping to the object's own rotated lattice would put nothing on round numbers.
    if (snapMove_) for (int k=0;k<3;++k) (&o.pos.x)[k] = snapf((&o.pos.x)[k], moveSnap_);
}

// Scales the transform by a mouse delta in pixels, along the active axis or uniformly.
void SandboxApp::applyScale(EditXform& o, f32 dx, f32 dy) {
    auto bump = [&](int a, f32 amt){ f32& c=(&o.scale.x)[a]; c += amt; if (c<0.02f) c=0.02f; };
    if (activeAxis_ == 3) { const f32 amt=(dx - dy)/80.0f; for (int a=0;a<3;++a) bump(a, amt); }
    else {
        Vec3 ax[3]; gizmoBasis(o, ax);
        const Vec3 A = ax[activeAxis_];
        f32 s0x,s0y,s1x,s1y;
        if (project(o.pos, s0x, s0y) && project(o.pos + A, s1x, s1y)) {
            const f32 px=s1x-s0x, py=s1y-s0y, pl=std::sqrt(px*px+py*py);
            if (pl > 1e-3f) bump(activeAxis_, ((dx*px + dy*py)/pl) / 60.0f);
        }
    }
    if (snapScale_) for (int k=0;k<3;++k) (&o.scale.x)[k] = std::fmax(0.02f, snapf((&o.scale.x)[k], scaleSnap_));
}

// Rotates the transform about the active gizmo axis by the swept cursor angle, by QUATERNION
// COMPOSITION, not by adding to an Euler component: `rotDeg[axis] += angle` is only correct when
// the other two components are zero, so on an already-rotated object it rotated about the wrong axis.
// Order is `dq * q` (this Quat's operator* is the Hamilton product); "q first, then dq" would put
// dq on the LEFT, rotating about the object's axes instead of the world's (agreeing only at identity).
void SandboxApp::applyRotate(EditXform& o, f32 px, f32 py, f32 mx, f32 my) {
    f32 ox, oy; if (!project(o.pos, ox, oy)) return;
    const f32 a0=std::atan2(py-oy, px-ox), a1=std::atan2(my-oy, mx-ox);
    f32 da=a1-a0; while (da> kPi) da-=kTwoPi; while (da< -kPi) da+=kTwoPi;

    Vec3 ax[3]; gizmoBasis(o, ax);
    const Vec3 A = ax[activeAxis_];
    // Which way the ring turns on screen depends on which side of it the camera is.
    const f32 sgn = dot(A, camForward()) >= 0.0f ? -1.0f : 1.0f;

    rotDragDeg_ += degrees(da) * sgn;
    // Snaps the ACCUMULATED angle, not the per-frame delta -- snapping each delta would round most to zero and the object would never turn.
    const f32 target = snapRot_ ? snapf(rotDragDeg_, rotSnap_) : rotDragDeg_;
    const f32 step = target - rotAppliedDeg_;
    if (std::fabs(step) < 1e-5f) return;
    rotAppliedDeg_ = target;

    const Quat dq = Quat::fromAxisAngle(A, radians(step));
    o.rotDeg = eulerDegFromQuat((dq * quatFromEulerDeg(o.rotDeg)).normalized());
}

// Runs the tool keys, picking, and the gizmo drag for one frame.
void SandboxApp::handleManip(Engine& e) {
#if AVER_WITH_IMGUI
    if (!e.device()->uiActive() || browserActive_) return;
    if (gameHasInput()) return;
    const ImGuiIO& io = ImGui::GetIO();

    if (levelFocused_ && !io.WantCaptureKeyboard) {
        // 1..4 select a tool WITHIN the active mode (not a flat list that grows with every mode);
        // Tab switches mode. Both halves of the 1..4 dispatch are 4 commands, not 8 -- their scopes
        // never overlap since mode_ is single-valued.
        if (keybinds_.pressed(editor::CommandId::ModeToggleLandscape, io)) toggleEditorMode();
#if AVER_MODULE_LANDSCAPE
        if (mode_ == EditorMode::Landscape) {
            if (keybinds_.pressed(editor::CommandId::SculptRaise, io))   sculptTool_=SculptTool::Raise;
            if (keybinds_.pressed(editor::CommandId::SculptLower, io))   sculptTool_=SculptTool::Lower;
            if (keybinds_.pressed(editor::CommandId::SculptSmooth, io))  sculptTool_=SculptTool::Smooth;
            if (keybinds_.pressed(editor::CommandId::SculptFlatten, io)) sculptTool_=SculptTool::Flatten;
        } else
#endif
        {
            if (keybinds_.pressed(editor::CommandId::ToolSelect, io)) tool_=Tool::Select;
            if (keybinds_.pressed(editor::CommandId::ToolMove, io))   tool_=Tool::Move;
            if (keybinds_.pressed(editor::CommandId::ToolRotate, io)) tool_=Tool::Rotate;
            if (keybinds_.pressed(editor::CommandId::ToolScale, io))  tool_=Tool::Scale;
        }
#if AVER_MODULE_LANDSCAPE
        if (landscape_.loaded()) {
        }
#endif
    }
    const f32 mx=io.MousePos.x, my=io.MousePos.y;
    const bool overScene = levelHovered_ && inViewport(mx, my);

#if AVER_MODULE_LANDSCAPE
    const bool isSculptTool = editorModeIsLandscape();
#else
    // Unused with the module off (the isSculptTool branch below compiles out too), but declared
    // anyway so isXformTool's "not a sculpt tool" phrasing needs no second definition per configuration.
    [[maybe_unused]] const bool isSculptTool = false;
#endif
    // Only Move/Rotate/Scale ever show or drive the transform gizmo: a sculpt tool has its own
    // brush-ring cursor and click/drag handling below, not this one. Object transforms are a
    // Select-mode action; in Landscape mode a drag is a brush stroke and must not also nudge whatever is selected.
    const bool isXformTool = !editorModeIsLandscape() &&
                             (tool_==Tool::Move || tool_==Tool::Rotate || tool_==Tool::Scale);

    hoverAxis_ = -1;
    EditXform gx;
    const bool haveGizmo = isXformTool && anySelected() && selectedXform(gx);
    Vec3 gaxis[3];
    if (haveGizmo) gizmoBasis(gx, gaxis);
    if (haveGizmo && !dragging_ && overScene)
        hoverAxis_ = pickAxis(gx.pos, gaxis, gizmoLen(gx.pos), mx, my);

#if AVER_MODULE_LANDSCAPE
    if (isSculptTool) {
        handleSculpt(e, io, overScene, mx, my);
    } else
#if AVER_MODULE_SCENE
    // Foliage owns the click like Landscape does: no picking, no gizmo, just the brush. Tested here
    // rather than falling through to Select, or painting would also select whatever it painted on.
    if (editorModeIsFoliage()) {
        handleFoliage(e, io, overScene, mx, my);
    } else
#endif
#endif
    {
        if (ImGui::IsMouseClicked(0) && overScene) {
            int ax = -1;
            if (haveGizmo)
                ax = pickAxis(gx.pos, gaxis, gizmoLen(gx.pos), mx, my);
            if (ax >= 0) {
                dragging_=true; activeAxis_=ax; prevMouseX_=mx; prevMouseY_=my;
                rotDragDeg_=0.0f; rotAppliedDeg_=0.0f;   // see applyRotate
                beginTransformEdit();
            }
            else {
                // Dragging the object's own body moves it too, not just the three hairline axis
                // handles (a gizmo handle means landing within 16px of a two-pixel line, and until
                // now that was the ONLY way to move anything -- a click on the body fell through to
                // a re-pick, so "grab it and drag" did nothing).
                //
                // Test is ALREADY-SELECTED: pick() still decides what was hit; landing on what was
                // already selected makes the click a grab rather than a selection change, so the
                // first click still only selects and a click elsewhere keeps its old meaning.
                //
                // Uses axis 3, applyMove's screen-plane branch (previously reachable only via a
                // 13px hotspot at the pivot, now the whole silhouette). No drag threshold needed:
                // a non-moving click produces no transform change, and endTransformEdit already
                // drops a no-op edit (nearlySameXform).
                const int  prevSel = sel_;
#if AVER_MODULE_SCENE
                const scene::Entity prevEnt = selEntity_;
#endif
                const bool hadSel = anySelected();
                const bool pickHit = pick(e, io);
                // pickHit gates the drag, not just sel_/selEntity_ matching prevSel/prevEnt: a
                // Ctrl-click MISS leaves the selection unchanged (pick()'s own tail), so that match
                // would be trivially true on a miss too, letting a Ctrl-click on empty space next
                // to a selection grab and drag it. Requiring pickHit means only a click that landed
                // back on the selected thing counts.
                const bool sameTarget = pickHit && hadSel && anySelected() && sel_ == prevSel
#if AVER_MODULE_SCENE
                                        && selEntity_ == prevEnt
#endif
                    ;
                if (haveGizmo && sameTarget && tool_ == Tool::Move) {
                    dragging_=true; activeAxis_=3; prevMouseX_=mx; prevMouseY_=my;
                    rotDragDeg_=0.0f; rotAppliedDeg_=0.0f;
                    beginTransformEdit();
                }
#if AVER_MODULE_SCENE
                // Marquee arms on a miss, Select tool only (Move/Rotate/Scale keep "click empty
                // space, deselect"). Two misses arm it: a plain click that cleared the selection
                // (costs nothing extra), and a Ctrl-click miss (pick() left selection alone for it)
                // -- arming that makes Ctrl+drag from empty space ADD to an existing selection. A
                // release with no further movement reads as that same click (marqueeActive_ stays false).
                else if (tool_ == Tool::Select && !pickHit && (!anySelected() || io.KeyCtrl)) {
                    marqueeArmed_ = true; marqueeActive_ = false;
                    marqueeX0_ = marqueeX1_ = mx; marqueeY0_ = marqueeY1_ = my;
                }
#endif
            }
        }
        if (!io.MouseDown[0]) {
            if (dragging_) endTransformEdit();
            dragging_=false; activeAxis_=-1;
        }
#if AVER_MODULE_SCENE
        // Marquee continues while armed, becomes ACTIVE once the drag clears the threshold, commits
        // on release -- independent of `overScene`, since a drag that started inside the viewport
        // must keep tracking past its edge.
        if (marqueeArmed_) {
            marqueeX1_ = mx; marqueeY1_ = my;
            if (!marqueeActive_ &&
                editor::marqueeExceedsThreshold(marqueeX0_, marqueeY0_, marqueeX1_, marqueeY1_))
                marqueeActive_ = true;

            if (marqueeActive_) {
                f32 loX, loY, hiX, hiY;
                editor::normalizeMarqueeRect(marqueeX0_, marqueeY0_, marqueeX1_, marqueeY1_, loX, loY, hiX, hiY);
                // The selection outline's own orange (kSelectionColor, SandboxApp.hpp), diluted for
                // the fill so the scene underneath a drag stays readable.
                ImDrawList* dl = ImGui::GetForegroundDrawList();
                dl->AddRectFilled(ImVec2(loX, loY), ImVec2(hiX, hiY), IM_COL32(235, 163, 10, 40));
                dl->AddRect(ImVec2(loX, loY), ImVec2(hiX, hiY), IM_COL32(235, 163, 10, 220), 0.0f, 0, 1.5f*dpi_);
            }

            if (!io.MouseDown[0]) {
                if (marqueeActive_) {
                    f32 loX, loY, hiX, hiY;
                    editor::normalizeMarqueeRect(marqueeX0_, marqueeY0_, marqueeX1_, marqueeY1_, loX, loY, hiX, hiY);
                    // Every entity eligible per pick()'s broadphase, whose projected world AABB intersects the rectangle.
                    std::vector<scene::Entity> hitEnts;
                    scene::World& w = scene::World::instance();
                    const u32 n = w.count();
                    for (u32 i = 0; i < n; ++i) {
                        const scene::Entity ent = w.at(i);
                        if (!w.valid(ent) || w.destroyPending(ent)) continue;
                        if (anyChunkWorldOwns(ent)) continue;
                        const auto* mr = w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
                        if (!mr || !(mr->flags & scene::kMeshRendererVisible) || mr->mesh == 0) continue;
                        if (content_.meshFor(mr->mesh) == 0) continue;
                        Vec3 lmin{mr->aabbMin[0], mr->aabbMin[1], mr->aabbMin[2]};
                        Vec3 lmax{mr->aabbMax[0], mr->aabbMax[1], mr->aabbMax[2]};
                        if (!(lmax.x > lmin.x && lmax.y > lmin.y && lmax.z > lmin.z)) { lmin = Vec3{-1,-1,-1}; lmax = Vec3{1,1,1}; }
                        const Mat4 wm = w.worldMatrix(ent);
                        f32 cx[8], cy[8]; int cn = 0;
                        for (u32 c = 0; c < 8; ++c) {
                            const Vec3 p{(c & 1) ? lmax.x : lmin.x, (c & 2) ? lmax.y : lmin.y, (c & 4) ? lmax.z : lmin.z};
                            f32 sx, sy;
                            if (project(xformPoint(wm, p), sx, sy)) { cx[cn] = sx; cy[cn] = sy; ++cn; }
                        }
                        if (editor::projectedAabbIntersectsRect(cx, cy, cn, loX, loY, hiX, hiY))
                            hitEnts.push_back(ent);
                    }
                    // Ctrl ADDS the catch to the existing selection; a plain drag REPLACES it --
                    // including replacing it with nothing, when the rectangle caught nothing at all.
                    if (hitEnts.empty()) {
                        if (!io.KeyCtrl) { multiClear(); sel_ = -1; selEntity_ = scene::kInvalidEntity; }
                    } else if (io.KeyCtrl) {
                        for (const scene::Entity ent : hitEnts) if (!multiIsSelected(ent)) multiToggle(ent);
                    } else {
                        multiSetSingle(hitEnts.front());
                        for (usize k = 1; k < hitEnts.size(); ++k) multiToggle(hitEnts[k]);
                    }
                }
                marqueeArmed_ = false;
                marqueeActive_ = false;
            }
        }
#endif
    }

    // The Outliner and Details panels count as "the level" for the edit verbs too, not just
    // levelFocused_ (true only while the 3D viewport holds keyboard focus): clicking an Outliner
    // row -- the ordinary way to select something -- moved focus there, so Delete/Copy/Paste/
    // Duplicate/Undo silently stopped working (reported as "deleting is a bit broken"). Not a
    // blanket "any window": WantTextInput excludes text fields, and the Content Browser keeps its
    // own Delete (deletes a FILE) rather than sharing this key's meaning.
    //
    // Also gated on !dragging_, a correctness fix, not a nicety: beginTransformEdit captures
    // editBefore_ at grab time but nothing pins the gesture to that entity, so Ctrl+A/Delete/
    // Ctrl+V while the mouse is still held could change the selection under the drag and
    // endTransformEdit would push an undo record for an entity that was never dragged -- corruption
    // an anchor-equality assert wouldn't catch. (Repro: drag a non-first Outliner object's gizmo,
    // press Ctrl+A mid-drag -- the gizmo jumped to the other object.) Gating the verbs is narrower
    // than pinning a dragEntity_ through the drag (that risks a second source of truth for "what is
    // being dragged"): the mouse button already blocks a click from changing selection, so the keyboard verbs were the whole hole.
    if (!dragging_ && (levelFocused_ || outlinerFocused_ || detailsFocused_) && !io.WantTextInput) {
        if (keybinds_.pressed(editor::CommandId::EditDelete, io))    deleteSelection();
        if (keybinds_.pressed(editor::CommandId::EditCopy, io))      copySelection();
        if (keybinds_.pressed(editor::CommandId::EditPaste, io))     pasteClipboard();
        if (keybinds_.pressed(editor::CommandId::EditDuplicate, io)) duplicateSelection();
        // Guarded on the same emptiness the menu item greys itself on (key and menu agree), and
        // also on AVER_MODULE_SCENE (not one of the panel-focus checks above): outlinerOrder_ and
        // selectAllInOutliner belong to the ECS world's multi-selection (multiSel_ and friends, in
        // SandboxApp.hpp), which doesn't exist without SCENE.
#if AVER_MODULE_SCENE
        if (!outlinerOrder_.empty() && keybinds_.pressed(editor::CommandId::EditSelectAll, io))
            selectAllInOutliner();
#endif
        if (keybinds_.pressed(editor::CommandId::EditUndo, io)) undo();
        // Ctrl+Shift+Z: intentionally NOT-rebindable alternate spelling of Redo (same command),
        // kept hardcoded next to the registry-driven checks as it always was.
        if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)) redo();
        if (keybinds_.pressed(editor::CommandId::EditRedo, io)) redo();

#if AVER_MODULE_SCENE
        if (keybinds_.pressed(editor::CommandId::SnapToFloor, io))     snapSelectionToFloor();
        if (keybinds_.pressed(editor::CommandId::HideSelected, io))    hideSelection();
        if (keybinds_.pressed(editor::CommandId::IsolateSelected, io)) isolateSelection();
        if (keybinds_.pressed(editor::CommandId::UnhideAll, io))       unhideAll();

        // Nudge is camera-relative, snapped to whichever single world axis each direction most
        // agrees with -- else "left" on a non-axis-aligned camera would nudge diagonally.
        {
            const Vec3 rightVec = cross(Vec3{0,0,1}, camForward());
            Vec3 fwdFlat = camForward(); fwdFlat.z = 0.0f;
            const Vec3 rightAxis = std::fabs(rightVec.x) >= std::fabs(rightVec.y)
                ? Vec3{rightVec.x >= 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f}
                : Vec3{0.0f, rightVec.y >= 0.0f ? 1.0f : -1.0f, 0.0f};
            const Vec3 fwdAxis = std::fabs(fwdFlat.x) >= std::fabs(fwdFlat.y)
                ? Vec3{fwdFlat.x >= 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f}
                : Vec3{0.0f, fwdFlat.y >= 0.0f ? 1.0f : -1.0f, 0.0f};
            const f32 nudgeStep = snapMove_ ? moveSnap_ : 10.0f;

            using CI = editor::CommandId;
            const CI kNudgeIds[6] = {CI::NudgeLeft, CI::NudgeRight, CI::NudgeForward,
                                      CI::NudgeBack, CI::NudgeUp, CI::NudgeDown};
            bool anyNudgeDown = false;
            for (CI id : kNudgeIds) {
                const auto& ch = keybinds_.chordFor(id);
                if (ch.isBound() && ImGui::IsKeyDown(ch.key)) { anyNudgeDown = true; break; }
            }
            // One undo entry per held run, not per repeat: opens on the first nudge key down,
            // closes once all are up. Tracks its own flag (not editBeforeValid_, which a Details
            // panel field drag also holds open -- this would otherwise close that entry every frame).
            if (anyNudgeDown && !nudgeEditOpen_) nudgeEditOpen_ = beginTransformEdit();

            if (keybinds_.pressed(CI::NudgeLeft, io))    nudgeSelection(rightAxis * -nudgeStep);
            if (keybinds_.pressed(CI::NudgeRight, io))   nudgeSelection(rightAxis *  nudgeStep);
            if (keybinds_.pressed(CI::NudgeForward, io)) nudgeSelection(fwdAxis   *  nudgeStep);
            if (keybinds_.pressed(CI::NudgeBack, io))    nudgeSelection(fwdAxis   * -nudgeStep);
            if (keybinds_.pressed(CI::NudgeUp, io))      nudgeSelection(Vec3{0.0f, 0.0f,  nudgeStep});
            if (keybinds_.pressed(CI::NudgeDown, io))    nudgeSelection(Vec3{0.0f, 0.0f, -nudgeStep});

            if (!anyNudgeDown && nudgeEditOpen_) { endTransformEdit(); nudgeEditOpen_ = false; }
        }
#endif
    }

    if (dragging_ && anySelected()) {
        EditXform o;
        if (selectedXform(o)) {
            const f32 dx=mx-prevMouseX_, dy=my-prevMouseY_;
            const Vec3 before = o.pos;
            if (tool_==Tool::Move)        applyMove(o, dx, dy);
            else if (tool_==Tool::Rotate) applyRotate(o, prevMouseX_, prevMouseY_, mx, my);
            else if (tool_==Tool::Scale)  applyScale(o, dx, dy);
            setSelectedXform(o);
#if AVER_MODULE_SCENE
            // The rest of the selection follows by the same world-space DELTA the anchor just
            // moved (reads `before` rather than recomputing: snap/axis-constraint/projection are
            // already applied to the anchor, so re-deriving per entity could snap each separately
            // and change their spacing).
            //
            // Move only: rotate/scale about a shared pivot need a pivot CHOSEN (anchor? centroid?
            // each object's own?), and picking one silently would be worse than the honest gap --
            // they still act on the anchor alone.
            if (tool_ == Tool::Move) {
                const Vec3 delta = o.pos - before;
                if (delta.x != 0.0f || delta.y != 0.0f || delta.z != 0.0f) {
                    // Through the shared helper, which also decides what beginTransformEdit
                    // recorded (skip the anchor, and anything beneath another selected entity, or
                    // a child would move twice) -- the undo record depends on matching that rule exactly, so both read from one place.
                    scene::World& w = scene::World::instance();
                    forEachMultiMoved([&](scene::Entity ent, const Transform& xf) {
                        Transform t = xf;
                        t.position += delta;
                        w.setLocalTransform(ent, t);
                    });
                }
            }
#endif
        }
        prevMouseX_=mx; prevMouseY_=my;
    }
#else
    (void)e;
#endif
}

#if AVER_MODULE_LANDSCAPE && AVER_WITH_IMGUI
// Applies the active sculpt brush continuously while LMB is held over the terrain. Reuses
// viewportRay() -- the SAME screen->ray conversion pick() and the asset drag-drop's drop point use
// -- and landscape::raycastHeightfield() for where that ray meets the section's surface.
void SandboxApp::handleSculpt(Engine& e, const ImGuiIO& io, bool overScene, f32 mx, f32 my) {
    sculptCursorValid_ = false;
    if (!landscape_.loaded()) { sculpting_ = false; return; }

    Vec3 ro, rd;
    viewportRay(mx, my, ro, rd);
    const f32 roA[3] = {ro.x, ro.y, ro.z}, rdA[3] = {rd.x, rd.y, rd.z};
    landscape::HeightfieldHit hit;
    const bool haveHit = overScene && landscape::raycastHeightfield(landscape_.data(), roA, rdA, hit);
    if (haveHit) {
        sculptCursorValid_ = true;
        sculptCursor_ = Vec3{hit.posCm[0], hit.posCm[1], hit.posCm[2]};
    }

    if (ImGui::IsMouseClicked(0) && overScene && haveHit) {
        beginSculptStroke();
        sculptFlattenTargetCm_ = hit.posCm[2];   // captured once per stroke -- see BrushParams
        // Ramp's start point AND its height, captured the same way and for the same reason:
        // see BrushParams::rampStartCm.
        sculptRampStartCm_[0] = hit.posCm[0];
        sculptRampStartCm_[1] = hit.posCm[1];
        sculptRampStartHeightCm_ = hit.posCm[2];
        // Noise seed captured once per stroke from the start position, not the clock (see
        // BrushParams::noiseSeed), so replaying the same drag noises the same way. fnv1a64 over
        // the two raw floats truncated to 32 bits: reproducible since it hashes the exact bit pattern, not a rounded position.
        const f32 seedInput[2] = {hit.posCm[0], hit.posCm[1]};
        sculptNoiseSeed_ = static_cast<u32>(fnv1a64(seedInput, sizeof(seedInput)));
    }
    // Release ends the stroke and pushes the undo entry: previously sculpting was invisible to
    // Ctrl+Z (handleSculpt mutated the heightfield in place; the undo stack only knew entity
    // transforms), so Ctrl+Z after terrain work silently undid a moved object instead.
    if (!io.MouseDown[0] && landscape_.strokeActive()) endSculptStroke();

    if (sculpting_ && haveHit) {
        landscape::BrushParams p;
        p.centerCm[0] = hit.posCm[0];
        p.centerCm[1] = hit.posCm[1];
        p.radiusCm = sculptRadiusCm_;
        p.strength = sculptStrengthCm_;
        p.flattenTargetCm = sculptFlattenTargetCm_;
        p.falloff = sculptFalloff_;
        p.rampStartCm[0] = sculptRampStartCm_[0];
        p.rampStartCm[1] = sculptRampStartCm_[1];
        p.rampStartHeightCm = sculptRampStartHeightCm_;
        p.noiseSeed = sculptNoiseSeed_;
        p.mode = sculptTool_==SculptTool::Raise   ? landscape::BrushMode::Raise
               : sculptTool_==SculptTool::Lower   ? landscape::BrushMode::Lower
               : sculptTool_==SculptTool::Smooth  ? landscape::BrushMode::Smooth
               : sculptTool_==SculptTool::Flatten ? landscape::BrushMode::Flatten
               : sculptTool_==SculptTool::Ramp    ? landscape::BrushMode::Ramp
                                            : landscape::BrushMode::Noise;

        // dt-scaled so holding paints at a constant rate regardless of frame rate; clamped like
        // other per-frame dt reads in this file (see the fly camera's dt clamp) so a stall doesn't
        // apply one giant stroke.
        const f32 dt = std::fmin(e.time().dt, 0.05f);
        const f32 amount = std::fmin(1.0f, dt * 6.0f);

        // GameLandscape::sculpt grows the stroke's undo buffer BEFORE applyBrush writes (so
        // "before" isn't already "after"), applies the brush, then rebuilds the WHOLE tree
        // (LandscapeTree::build has no incremental form), invalidating only the touched GPU meshes.
        landscape_.sculpt(e.device(), p, amount);
    }
}

#endif

// Draws the current tool's gizmo over the selection, on top of geometry.
void SandboxApp::drawGizmo(Engine& e) {
    // Only Move/Rotate/Scale get a gizmo (Select and the sculpt tools, which draw their own cursor
    // via drawSculptCursor(), don't) -- spelled as an allow-list so a tool added later defaults to
    // "no gizmo" rather than inheriting `: gzScale_`. Also gated to Select mode, else the gizmo
    // would keep drawing over the terrain during a brush (tool_ still holds the last object tool).
    if (editorModeIsLandscape()) return;
    if (tool_!=Tool::Move && tool_!=Tool::Rotate && tool_!=Tool::Scale) return;
    EditXform x;
    if (!selectedXform(x)) return;
    const Vec3 O = x.pos;
    const f32 L = gizmoLen(O);
    // The handles are drawn along the SAME axes pickAxis tests and applyMove drags. The line
    // meshes are built along the world axes, so local space rotates them here. Row-vector
    // order, matching Transform::matrix(): scale, then rotate, then translate.
    const bool localGizmo = !worldSpace_ || tool_ == Tool::Scale;
    const Mat4 w = localGizmo
        ? Mat4::scale(Vec3{L,L,L}) * Mat4::fromQuat(quatFromEulerDeg(x.rotDeg)) * Mat4::translation(O)
        : Mat4::scale(Vec3{L,L,L}) * Mat4::translation(O);
    const rhi::LineHandle* nrm = tool_==Tool::Move ? gzMove_ : tool_==Tool::Rotate ? gzRot_ : gzScale_;
    const rhi::LineHandle* hi  = tool_==Tool::Move ? gzMoveHi_ : tool_==Tool::Rotate ? gzRotHi_ : gzScaleHi_;
    e.device()->setLineDepth(false);
    // Unreal draws its gizmo handles a few pixels wide, not hairline -- setLineWidth is in DISPLAY
    // pixels (RHI.hpp), so this scales by DPI the same way the rest of the editor's own line widths do.
    e.device()->setLineWidth(3.0f * dpi_);
    for (int a=0;a<3;++a) {
        const bool active = (dragging_ && a==activeAxis_) || (!dragging_ && a==hoverAxis_);
        e.device()->drawLines(active ? hi[a] : nrm[a], &w.m[0][0]);
    }
    // Back to hairline for the grid and everything else: the setter is sticky.
    e.device()->setLineWidth(1.0f);
    e.device()->setLineDepth(true);
}

#if AVER_MODULE_LANDSCAPE
// The sculpt brush's footprint, drawn over the terrain the same undepth-tested way drawGizmo()
// draws over a selection -- it needs to read through the ground plane it is standing on.
void SandboxApp::drawSculptCursor(Engine& e) {
    if (!brushRing_ || !sculptCursorValid_) return;
    const bool sculptTool = editorModeIsLandscape();
    if (!sculptTool) return;
    const Mat4 w = Mat4::scale(Vec3{sculptRadiusCm_, sculptRadiusCm_, sculptRadiusCm_}) *
                   Mat4::translation(sculptCursor_);
    e.device()->setLineDepth(false);
    // Explicit, not relying on drawGizmo() having already reset it: these rings must stay hairline
    // regardless of what else ran before this call.
    e.device()->setLineWidth(1.0f);
    e.device()->drawLines(brushRing_, &w.m[0][0]);

    // Second ring is where full strength stops (without it the Falloff slider is invisible until
    // after a stroke): the outer ring alone shows brush WIDTH, not SHAPE, so falloff 0.1 and 0.9
    // would draw an identical cursor but behave completely differently.
    //
    // (1 - falloff) is the plateau radius Sculpt.cpp's remap actually uses -- full weight inside
    // it, the smoothstep shoulder compressed into what remains (applyBrush's `t <= 1.0f - soft`
    // branch) -- not a guess, kept honest if that curve is retuned.
    //
    // Drawn only when it says something: at falloff 1 the inner ring collapses to a dot, at 0 it
    // coincides with the outer ring -- both extremes are already unambiguous without it.
    const f32 soft  = sculptFalloff_ < 0.0f ? 0.0f : (sculptFalloff_ > 1.0f ? 1.0f : sculptFalloff_);
    const f32 inner = sculptRadiusCm_ * (1.0f - soft);
    if (inner > sculptRadiusCm_ * 0.04f && inner < sculptRadiusCm_ * 0.96f) {
        const Mat4 wi = Mat4::scale(Vec3{inner, inner, inner}) * Mat4::translation(sculptCursor_);
        e.device()->drawLines(brushRing_, &wi.m[0][0]);
    }
    e.device()->setLineDepth(true);
}

#endif

#if AVER_WITH_IMGUI
// Removes the selected entity or placeholder object, pushing an undo entry either way (pseudo-
// entries like sun/sky/post never land here). The objects_ branch used to push nothing, so
// deleting a placeholder object (the default path with no level loaded) was silently non-undoable.
void SandboxApp::deleteSelection() {
#if AVER_MODULE_SCENE
    // Every selected entity, not just the anchor (Delete on five rows used to remove one and leave
    // four highlighted).
    //
    // One undo record PER ENTITY, honestly: EditCmd describes a single destroy, so undoing a
    // five-object delete takes five Ctrl+Z -- grouping needs a compound command the undo stack doesn't have.
    {
        const std::vector<scene::Entity> victims = selectedEntities();
        if (victims.size() > 1) {
            for (const scene::Entity v : victims) {
                scene::World& w = scene::World::instance();
                if (!w.valid(v)) continue;
                EditCmd c = describeEntity(v);
                c.kind = EditCmd::Kind::Destroy;
                captureSubtree(c, v);
                destroyEntity(v);
                pushEdit(std::move(c));
            }
            AVER_INFO("[Editor] deleted {} selected entities", victims.size());
            multiClear();
            selEntity_ = scene::kInvalidEntity;
            sel_ = -1;
            return;
        }
    }
    if (sel_ == kSelScene && selEntity_ != scene::kInvalidEntity) {
        scene::World& w = scene::World::instance();
        if (w.valid(selEntity_)) {
            AVER_INFO("[Editor] deleted entity #{} '{}'", (u32)selEntity_, w.name(selEntity_));
            EditCmd c = describeEntity(selEntity_);
            c.kind = EditCmd::Kind::Destroy;
            // Before destroyEntity, which retires the subtree and its hierarchy links -- a capture
            // afterwards would find an empty subtree and report a complete undo record it didn't have.
            captureSubtree(c, selEntity_);
            if (!c.subtree.empty())
                AVER_INFO("[Editor] ...and {} descendant(s) with it", c.subtree.size());
            destroyEntity(selEntity_);
            pushEdit(std::move(c));
        }
        multiClear();
        selEntity_ = scene::kInvalidEntity;
        sel_ = -1;
        return;
    }
#endif
    if (sel_ >= 0 && sel_ < (int)objects_.size()) {
        EditCmd c;
        c.kind = EditCmd::Kind::DestroyObj;
        c.objIndex = sel_;
        c.objSnapshot = objects_[sel_];
        objects_.erase(objects_.begin() + sel_);
        objectsErasedAt(sel_);
        pushEdit(std::move(c));
        sel_ = -1;
    }
}

// Copies the selection into the editor's own clipboard, a pure read (nothing pushed onto the undo
// stack). A non-empty `entities` and `hasObject` are mutually exclusive, mirroring sel_/selEntity_.
void SandboxApp::copySelection() {
#if AVER_MODULE_SCENE
    // The whole selection, not just the anchor (same fix Ctrl+D got).
    //
    // Ancestors skipped using DUPLICATE's version of that test, not the mover's: the mover also
    // excludes `ent == selEntity_` (the drag anchor), but a copy has no anchor to exclude -- that
    // would omit the entity the author clicked first. Descendants of another selected entity are
    // still skipped: captureSubtree already takes them along inside their parent, so keeping them
    // too would paste them twice, once orphaned.
    if (sel_ == kSelScene && !multiStale() && multiSel_.size() > 1) {
        scene::World& w = scene::World::instance();
        clipboard_.entities.clear();
        for (const scene::Entity ent : multiSel_) {
            if (!w.valid(ent)) continue;
            bool ancestorSelected = false;
            for (scene::Entity p = w.parent(ent); p != scene::kInvalidEntity; p = w.parent(p))
                if (multiIsSelected(p)) { ancestorSelected = true; break; }
            if (ancestorSelected) continue;

            EditCmd c = describeEntity(ent);
            captureSubtree(c, ent);
            ClipboardEntity ce;
            ce.snap = c.snap;
            ce.subtree = std::move(c.subtree);
            ce.xform = c.after;
            ce.hadBody = c.hadBody;
            ce.hadCollide = c.hadCollide; ce.hadSnapZ = c.hadSnapZ; ce.snapZ = c.snapZ; ce.hadAnim = c.hadAnim;
            clipboard_.entities.push_back(std::move(ce));
        }
        clipboard_.hasObject = false;
        AVER_INFO("[Editor] copied {} entities", clipboard_.entities.size());
        return;
    }
    if (sel_ == kSelScene && selEntity_ != scene::kInvalidEntity && scene::World::instance().valid(selEntity_)) {
        EditCmd c = describeEntity(selEntity_);
        captureSubtree(c, selEntity_);
        ClipboardEntity ce;
        ce.snap = c.snap;
        ce.subtree = std::move(c.subtree);
        ce.xform = c.after;
        ce.hadBody = c.hadBody;
        ce.hadCollide = c.hadCollide; ce.hadSnapZ = c.hadSnapZ; ce.snapZ = c.snapZ; ce.hadAnim = c.hadAnim;
        clipboard_.entities.assign(1, std::move(ce));
        clipboard_.hasObject  = false;
        return;
    }
#endif
    if (movableSelected()) {
        clipboard_.hasObject = true;
        clipboard_.object = objects_[sel_];
        clipboard_.entities.clear();   // exclusive with the scene path, as the two halves always were
    }
}

// Rebuilds the clipboard's contents in front of the camera (same convention as spawnCube() /
// dropWorldPoint()), pushing a FRESH Create/CreateObj entry independent of the original copied
// thing's undo entry. No-op when the clipboard is empty (keybind dispatch doesn't gate on
// clipboard state, so that's expected).
void SandboxApp::pasteClipboard() {
    Vec3 at = camPos_ + camForward() * kAddDistance;
    if (snapMove_) for (int k = 0; k < 3; ++k) (&at.x)[k] = snapf((&at.x)[k], moveSnap_);
#if AVER_MODULE_SCENE
    if (!clipboard_.entities.empty()) {
        // Relative layout is preserved, not a loop pasting each item at `at` (which would stack
        // every copied entity on one point -- five props in a row would arrive as one heap).
        // Duplicate gets this for free by offsetting from each original; paste computes the same
        // thing since it moves the whole set to a NEW anchor. First entry lands exactly at `at`
        // (one-item case is bit-identical to before); every other entry keeps its offset from that one.
        const Vec3 origin = clipboard_.entities.front().xform.pos;
        std::vector<scene::Entity> made;
        for (const ClipboardEntity& ce : clipboard_.entities) {
            EditXform x = ce.xform;
            x.pos = at + (ce.xform.pos - origin);
            const std::string label = makeEntityLabel(std::string(), ce.snap.asset);
            const scene::Entity e = spawnEntityFrom(ce.snap, x, label, ce.hadBody, /*restoreObjectId=*/false,
                                                    scene::kInvalidEntity, ce.hadCollide, ce.hadSnapZ, ce.snapZ,
                                                    &ce.hadAnim);
            if (e == scene::kInvalidEntity) continue;
            spawnSubtreeUnder(e, ce.subtree, /*restoreIds=*/false);
            sel_ = kSelScene; selEntity_ = e;   // spawnSubtreeUnder selects whatever it made last
            EditCmd c = describeEntity(e);
            c.kind = EditCmd::Kind::Create;
            // Captured from the copy so REDO puts the children back too: undo of a Create destroys
            // the whole subtree (World::destroy retires it), so without this a paste-undo-redo left only the root.
            captureSubtree(c, e);
            // One record per pasted entity, matching Duplicate's/Delete's precedent (N undos for N
            // items) -- a compound Create recreating a whole set is a bigger change than this is worth.
            pushEdit(std::move(c));
            made.push_back(e);
        }
        if (made.empty()) return;
        // The pastes become the selection (matches Duplicate): a drag right after Ctrl+V moves what was just made, making paste-then-place one motion.
        multiSetSingle(made.front());
        for (usize i = 1; i < made.size(); ++i) multiToggle(made[i]);
        sel_ = kSelScene; selEntity_ = made.front();
        if (made.size() == 1)
            AVER_INFO("[Editor] pasted entity #{} and {} descendant(s)", (u32)made.front(),
                      clipboard_.entities.front().subtree.size());
        else
            AVER_INFO("[Editor] pasted {} entities", made.size());
        return;
    }
#endif
    if (clipboard_.hasObject) {
        MeshObj o = clipboard_.object;
        o.pos = at;
        o.name = makeEntityLabel(clipboard_.object.name, clipboard_.object.name);
        objects_.push_back(o);
        sel_ = (int)objects_.size() - 1; selEntity_ = kInvalidId;
        EditCmd c;
        c.kind = EditCmd::Kind::CreateObj;
        c.objIndex = sel_;
        c.objSnapshot = o;
        pushEdit(std::move(c));
    }
}

// Rebuilds the SELECTION (not the clipboard) offset by a small fixed delta from where it already
// sits: Duplicate makes a visible sibling next to the original, where Paste restores at the copied
// transform -- separately-reasoned placement. Never touches clipboard_.
void SandboxApp::duplicateSelection() {
    const f32 delta = snapMove_ ? moveSnap_ : kDuplicateOffset;
#if AVER_MODULE_SCENE
    // The whole selection, not just the anchor (previously read selEntity_ alone, so Ctrl+D on
    // five props duplicated one and dropped the other four).
    //
    // One record PER COPY, deleteSelection's precedent rather than multi-move's: N undos for N
    // duplicates -- a drag is one gesture, but five duplicates are five objects reasonably unpicked
    // one at a time, and a compound Create recreating a whole set is a bigger change than this is worth.
    //
    // Ancestors skipped like the mover skips them: duplicating a parent already duplicates its
    // children via captureSubtree, so a selected child would get a second, orphaned copy.
    if (sel_ == kSelScene && !multiStale() && multiSel_.size() > 1) {
        scene::World& w = scene::World::instance();
        std::vector<scene::Entity> made;
        for (const scene::Entity ent : multiSel_) {
            if (!w.valid(ent)) continue;
            bool ancestorSelected = false;
            for (scene::Entity p = w.parent(ent); p != scene::kInvalidEntity; p = w.parent(p))
                if (multiIsSelected(p)) { ancestorSelected = true; break; }
            if (ancestorSelected) continue;

            EditCmd src = describeEntity(ent);
            captureSubtree(src, ent);
            EditXform x = src.after;
            x.pos.x += delta; x.pos.y += delta;
            const std::string label = makeEntityLabel(std::string(), src.snap.asset);
            const scene::Entity e =
                spawnEntityFrom(src.snap, x, label, src.hadBody, /*restoreObjectId=*/false,
                                scene::kInvalidEntity, src.hadCollide, src.hadSnapZ, src.snapZ, &src.hadAnim);
            if (e == scene::kInvalidEntity) continue;
            spawnSubtreeUnder(e, src.subtree, /*restoreIds=*/false);
            EditCmd c = describeEntity(e);
            c.kind = EditCmd::Kind::Create;
            captureSubtree(c, e);
            pushEdit(std::move(c));
            made.push_back(e);
        }
        if (made.empty()) return;
        // The copies become the selection, so a drag right after Ctrl+D moves what was just made (as every editor does).
        multiSetSingle(made.front());
        for (usize i = 1; i < made.size(); ++i) multiToggle(made[i]);
        sel_ = kSelScene; selEntity_ = made.front();
        AVER_INFO("[Editor] duplicated {} entities", made.size());
        return;
    }
    if (sel_ == kSelScene && selEntity_ != scene::kInvalidEntity && scene::World::instance().valid(selEntity_)) {
        EditCmd src = describeEntity(selEntity_);
        // Descendants read BEFORE anything is spawned -- the copy is about to become the
        // selection, and captureSubtree walks whatever it is handed.
        captureSubtree(src, selEntity_);
        EditXform x = src.after;
        x.pos.x += delta; x.pos.y += delta;
        const std::string label = makeEntityLabel(std::string(), src.snap.asset);
        const scene::Entity e = spawnEntityFrom(src.snap, x, label, src.hadBody, /*restoreObjectId=*/false,
                                                scene::kInvalidEntity, src.hadCollide, src.hadSnapZ, src.snapZ,
                                                &src.hadAnim);
        if (e == scene::kInvalidEntity) return;
        spawnSubtreeUnder(e, src.subtree, /*restoreIds=*/false);
        sel_ = kSelScene; selEntity_ = e;
        EditCmd c = describeEntity(e);
        c.kind = EditCmd::Kind::Create;
        captureSubtree(c, e);              // so REDO rebuilds the children; see pasteClipboard
        pushEdit(std::move(c));
        AVER_INFO("[Editor] duplicated entity #{} and {} descendant(s)", (u32)e,
                  src.subtree.size());
        return;
    }
#endif
    if (movableSelected()) {
        MeshObj o = objects_[sel_];
        o.pos.x += delta; o.pos.y += delta;
        o.name = makeEntityLabel(objects_[sel_].name, objects_[sel_].name);
        objects_.push_back(o);
        sel_ = (int)objects_.size() - 1; selEntity_ = kInvalidId;
        EditCmd c;
        c.kind = EditCmd::Kind::CreateObj;
        c.objIndex = sel_;
        c.objSnapshot = o;
        pushEdit(std::move(c));
    }
}

// Unprojects a screen-space point within the viewport rect into a world-space ray. Shared by
// pick() and the asset drag-drop drop point, so there is exactly one screen->ray conversion.
//
// BOTH ends are unprojected; the near one isn't decoration. This used to read `ro = eye_`, the
// only such assumption in the screen-to-world conversion, right only because a perspective
// frustum has a centre of projection -- under orthographic every ray is PARALLEL and
// starts on the near plane, so converging on a point far away would pick along the wrong line
// (every caller -- pick, handleSculpt, handleFoliage, dropWorldPoint -- inherits this).
//
// Unprojecting ndc.z = 0 for the origin is correct under perspective too (ahead of any ortho
// work): it just moves ro from the eye to the near plane 2 cm in front of it, along the same ray,
// so every hit ro + rd*t is unchanged (pick() compares t values within one call, so the change of scale reorders nothing). The two
// expressions differ only in the iv.m[2][*] term (ndc.z): present at the far plane (z=1), absent
// at the near plane (z=0); depth range is [0,1], not [-1,1] (see Mat4::perspectiveLH).
void SandboxApp::viewportRay(f32 screenX, f32 screenY, Vec3& ro, Vec3& rd) const {
    const f32 nx = (screenX - vpX_) / vpW_ * 2.f - 1.f;   // NDC within the viewport rect
    const f32 ny = 1.f - (screenY - vpY_) / vpH_ * 2.f;
    const Mat4& iv = invVP_;
    const f32 fx = nx*iv.m[0][0]+ny*iv.m[1][0]+iv.m[2][0]+iv.m[3][0];
    const f32 fy = nx*iv.m[0][1]+ny*iv.m[1][1]+iv.m[2][1]+iv.m[3][1];
    const f32 fz = nx*iv.m[0][2]+ny*iv.m[1][2]+iv.m[2][2]+iv.m[3][2];
    const f32 fw = nx*iv.m[0][3]+ny*iv.m[1][3]+iv.m[2][3]+iv.m[3][3];
    const f32 ox = nx*iv.m[0][0]+ny*iv.m[1][0]+iv.m[3][0];
    const f32 oy = nx*iv.m[0][1]+ny*iv.m[1][1]+iv.m[3][1];
    const f32 oz = nx*iv.m[0][2]+ny*iv.m[1][2]+iv.m[3][2];
    const f32 ow = nx*iv.m[0][3]+ny*iv.m[1][3]+iv.m[3][3];
    const Vec3 farW{fx/fw, fy/fw, fz/fw};
    ro = Vec3{ox/ow, oy/ow, oz/ow};
    rd = farW - ro;
}

#endif

#if AVER_WITH_IMGUI
#if AVER_MODULE_SCENE
// Lazy, memoized PickGeometry for a PROJECT mesh (built-ins are seeded at creation, :2113-2142,
// and never reach the loading branch below). Mirrors selectionOutlineLines: loadProjectMeshes
// frees OcMeshData after GPU upload, so this re-reads the .ocmesh once per mesh a click ever
// reaches; an id mapped to an empty PickGeometry IS the cached "unavailable, use bounds" answer,
// warned once, not per click.
const aver::editor::PickGeometry& SandboxApp::pickGeometryFor(u64 meshId) {
    if (const auto it = pickGeometry_.find(meshId); it != pickGeometry_.end()) return it->second;

    aver::editor::PickGeometry g;   // stays empty (and is still cached) on either failure below
    const auto pit = meshPathById_.find(meshId);
    const std::string content = project_.contentDir();
    if (pit == meshPathById_.end() || content.empty()) {
        AVER_WARN("[Pick] mesh id {} has no known project path; clicking it falls back to its "
                  "bounding box.", meshId);
    } else {
        fmt::OcMeshData md;
        std::string why;
        if (fmt::loadOcMesh(content + "/" + pit->second, md, &why)) {
            g.positions = std::move(md.positions);
            g.normals = std::move(md.normals);
            g.indices = std::move(md.indices);
        } else {
            AVER_WARN("[Pick] '{}' would not (re)load for triangle picking ({}); falling back to "
                      "its bounding box.", pit->second, why);
        }
    }
    return pickGeometry_.emplace(meshId, std::move(g)).first->second;
}

#endif
#endif

#if AVER_WITH_IMGUI
// Selects whatever the cursor's ray hits first, across both the placeholder and scene worlds.
//
// Nearest triangle wins, DOUBLE-SIDED, because ray-driven primary visibility (the editor's default
// render mode) draws back faces too -- a click must hit what the camera can see. Exception: an
// entity's OWN back faces when the ray starts inside ITS bounds (insideBox, the ab3bca81 case) --
// those are the inside walls of whatever the camera stands in, so a closed shape can't be selected
// from its own inside; an enclosing SHELL (NewSponza's building) stays selectable from inside since
// its interior walls face INTO the room.
//
// Triangle testing also means a nearer wall now BLOCKS an object behind it (bounds-only picking
// never did this). A skinned/posed entity (vertices move in a compute pass this ray never runs
// against) or a mesh with no resident triangles falls back to the bounding-box test, keeping the
// original ab3bca81 rule (t > 0.0f) so bounds enclosing the camera aren't auto-selected by every click.
bool SandboxApp::pick(Engine& e, const ImGuiIO& io) {
    (void)e;
    Vec3 ro, rd;
    viewportRay(io.MousePos.x, io.MousePos.y, ro, rd);
    int best=-1; f32 bestT=1e30f;
    // Placeholder objects_ (MeshObj boxes/the floor) stay bounds-only: for these, the box IS the
    // geometry, so a triangle test would buy nothing. Same t > 0.0f start-inside guard as the scene
    // loop below, compared against the same bestT, so the nearer of the two worlds still wins.
    if (!hideEditorScene_)
        for (int i=0;i<(int)objects_.size();++i){
            MeshObj& o=objects_[i]; if(!o.visible) continue;
            Transform tr; tr.position=o.pos; tr.rotation=quatFromEulerDeg(o.rotDeg); tr.scale=o.scale;
            const Mat4 iw = tr.toMatrix().inverse();
            const Vec3 lo=xformPoint(iw,ro), ld=xformVec(iw,rd);
            f32 t; if (rayAabb(lo,ld,o.aabbMin,o.aabbMax,t) && t>0.0f && t<bestT){ bestT=t; best=i; }
        }

    // AvId, not scene::Entity: pick() spans both placeholder and scene worlds, so bestEnt is read
    // unguarded below even though only the scene-only loop can set it away from "nothing".
    AvId bestEnt = kInvalidId;
#if AVER_MODULE_SCENE
    {
        // (a) Broadphase: every eligible entity whose local-space box the ray enters, with tBox
        // (hit distance) and insideBox (ray started inside). No decision here -- a CANDIDATE only.
        struct PickCandidate { scene::Entity ent; u64 meshId; f32 tBox; bool insideBox; Vec3 lo, ld; };
        std::vector<PickCandidate> candidates;
        scene::World& w = scene::World::instance();
        const u32 n = w.count();
        for (u32 i = 0; i < n; ++i) {
            const scene::Entity ent = w.at(i);
            if (!w.valid(ent) || w.destroyPending(ent)) continue;
            // Streamed entities are not selectable: selection is the only door into the
            // gizmo/EditCmd path, and the streamer could evict one out from under an in-flight edit
            // (see setChunkStreamingEnabled, buildPanels).
            if (anyChunkWorldOwns(ent)) continue;
            // The Player Start's mesh is a stand-in cube around its feet, not what is drawn; it is
            // tested by its drawn shape below instead.
            if (ent == playerStart_) continue;
            const auto* mr = w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
            if (!mr || !(mr->flags & scene::kMeshRendererVisible) || mr->mesh == 0) continue;
            if (content_.meshFor(mr->mesh) == 0) continue;
            Vec3 lmin{mr->aabbMin[0], mr->aabbMin[1], mr->aabbMin[2]};
            Vec3 lmax{mr->aabbMax[0], mr->aabbMax[1], mr->aabbMax[2]};
            if (!(lmax.x > lmin.x && lmax.y > lmin.y && lmax.z > lmin.z)) { lmin = Vec3{-1,-1,-1}; lmax = Vec3{1,1,1}; }
            const Mat4 iw = w.worldMatrix(ent).inverse();
            const Vec3 lo = xformPoint(iw, ro), ld = xformVec(iw, rd);
            f32 tBox;
            if (!rayAabb(lo, ld, lmin, lmax, tBox)) continue;
            candidates.push_back({ent, mr->mesh, tBox, tBox <= 0.0f, lo, ld});
        }

        // THE PLAYER START, BY WHAT IS DRAWN: the capsule (which contains the sprite from every
        // angle) and the facing arrow, in the marker's own frame -- position and rotation, no
        // scale, the same world matrix SandboxRender.cpp draws them with. Its pick used to be the
        // stand-in cube's triangles, a metre-wide box at the feet, so the upper capsule and the
        // sprite were unclickable. Pickable only while the marker is drawn (the render's gate).
        // Tested before the candidates so a nearer mesh still wins through bestT.
#if AVER_MODULE_FRAMEWORK
        const bool markerDrawn = !noEditorChrome_ && !anyPlayActive();
#else
        const bool markerDrawn = !noEditorChrome_;
#endif
        if (markerDrawn && playerStart_ != scene::kInvalidEntity && w.valid(playerStart_)) {
            const Transform& psXf = w.localTransform(playerStart_);
            const Mat4 ipw = (Mat4::fromQuat(psXf.rotation) * Mat4::translation(psXf.position)).inverse();
            const Vec3 plo = xformPoint(ipw, ro), pld = xformVec(ipw, rd);
            f32 tHit = bestT;
            bool hit = aver::editor::rayUprightCapsule(plo, pld, kPlayerStartCapsuleRadius,
                                                       kPlayerStartCapsuleHalfHeight * 2.0f, bestT, tHit);
            // The arrow as a slab around its shaft: forward along local +X at the capsule's middle,
            // as wide as buildPlayerStartArrow's head (0.16 of its length each side).
            const f32 arrowHalfW = kPlayerStartArrowLength * 0.16f;
            constexpr f32 kArrowHalfThick = 8.0f;   // cm, above and below the shaft
            f32 tArrow;
            if (rayAabb(plo, pld,
                        Vec3{0.0f, -arrowHalfW, kPlayerStartCapsuleHalfHeight - kArrowHalfThick},
                        Vec3{kPlayerStartArrowLength, arrowHalfW, kPlayerStartCapsuleHalfHeight + kArrowHalfThick},
                        tArrow) && tArrow > 0.0f && tArrow < tHit)
                { tHit = tArrow; hit = true; }
            if (hit && tHit < bestT) { bestT = tHit; bestEnt = playerStart_; best = -1; }
        }

        // (b) Nearest box first, stopping once a candidate's tBox can no longer beat bestT:
        // rayAabb's tBox is a lower bound on any triangle hit inside that box, so every candidate
        // past that point is provably farther than the best hit already found -- a real early-out.
        std::sort(candidates.begin(), candidates.end(),
                  [](const PickCandidate& a, const PickCandidate& b) { return a.tBox < b.tBox; });

        for (const PickCandidate& c : candidates) {
            if (c.tBox >= bestT) break;
            // (c) A skinned/posed entity's resting geometry isn't what's drawn (vertices move in a
            // compute pass this ray never runs against), so -- like a non-resident mesh -- it keeps the bounds-only rule.
            const bool posedOrSkinned = skinnedMeshIds_.count(c.meshId) != 0 || posedHandle(c.ent) != 0;
            const aver::editor::PickGeometry* geo = posedOrSkinned ? nullptr : &pickGeometryFor(c.meshId);
            if (!geo || geo->empty()) {
                if (c.tBox > 0.0f) { bestT = c.tBox; bestEnt = c.ent; best = -1; }
                continue;
            }
            // skipBackFaces = insideBox: double-sided from outside (matching ray-driven rendering,
            // which draws back faces), single-sided when the ray starts inside this entity's own
            // box, where a back face is the inside of whatever the camera is standing in.
            f32 tTri;
            if (aver::editor::rayPickGeometry(*geo, c.lo, c.ld, /*skipBackFaces=*/c.insideBox, bestT, tTri))
                { bestT = tTri; bestEnt = c.ent; best = -1; }
        }
    }
#endif
    // Ctrl-click extends the selection (previously the viewport DESTROYED it): a bare assignment
    // to selEntity_ is what multiStale() watches for, so a plain click in the 3D view silently
    // collapsed a multi-select made in the Outliner. Only Ctrl, not Shift: Shift-range needs a
    // defined ORDER to range across (outlinerOrder_, which a 3D view has no equivalent of) --
    // inventing one here would be worse than no gesture.
    //
    // A Ctrl-click MISS (nothing under the cursor in either world) returns FALSE without touching
    // selection at all, so the caller can arm the Select-tool marquee ADDITIVELY instead of
    // clearing everything first. A miss that landed on a placeholder (best != -1) is not "empty
    // space" -- placeholders never joined the scene multi-select, so it falls through to the plain-click assignment below.
#if AVER_MODULE_SCENE
    const bool extend = ImGui::GetIO().KeyCtrl;
    if (extend && bestEnt != kInvalidId) {
        multiToggle(static_cast<scene::Entity>(bestEnt));
        return true;
    }
    if (extend && bestEnt == kInvalidId && best == -1) return false;
#endif
    if (bestEnt != kInvalidId) { sel_ = kSelScene; selEntity_ = bestEnt; }
    else                       { sel_ = best;     selEntity_ = kInvalidId; }
    return bestEnt != kInvalidId || best != -1;
}

#endif

#if AVER_MODULE_SCENE
// ---- VIEWPORT PLACEMENT VERBS (2026-09-16) -----------------------------------------------------
// End/H/Shift+H/Ctrl+H/arrows/PageUp/PageDown, dispatched from handleManip's edit-verb block.

// The union of every selected entity's world-space bounds, for F (Frame Selected) to fit the WHOLE
// set, not just the anchor selectedXform/selectedRadius describe.
//
// Each entity's own box is built like selectedRadius builds one for the anchor: local
// CMeshRenderer extent times world scale, centred on world position (not a properly rotated world
// AABB -- selectedRadius already makes that trade for a single object, and framing should size an
// object the same way alone or in a selection).
bool SandboxApp::selectionBounds(Vec3& center, f32& radius) const {
    const std::vector<scene::Entity> sel = selectedEntities();
    if (sel.empty()) return false;

    scene::World& w = scene::World::instance();
    Vec3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
    for (const scene::Entity e : sel) {
        const Transform t = worldTransformOf(w, e);
        f32 ex = 0.0f, ey = 0.0f, ez = 0.0f;
        if (const auto* mr = w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer)) {
            ex = (mr->aabbMax[0] - mr->aabbMin[0]) * std::fabs(t.scale.x);
            ey = (mr->aabbMax[1] - mr->aabbMin[1]) * std::fabs(t.scale.y);
            ez = (mr->aabbMax[2] - mr->aabbMin[2]) * std::fabs(t.scale.z);
        }
        // Zero/missing extent falls back to the scale itself, like selectedRadius: unfilled bounds must not frame as a point.
        if (ex <= 1e-3f && ey <= 1e-3f && ez <= 1e-3f) {
            const f32 s = 2.0f * std::fmax(1.0f, std::fmax(std::fabs(t.scale.x),
                                            std::fmax(std::fabs(t.scale.y), std::fabs(t.scale.z))));
            ex = ey = ez = s;
        }
        mn.x = std::fmin(mn.x, t.position.x - ex * 0.5f); mx.x = std::fmax(mx.x, t.position.x + ex * 0.5f);
        mn.y = std::fmin(mn.y, t.position.y - ey * 0.5f); mx.y = std::fmax(mx.y, t.position.y + ey * 0.5f);
        mn.z = std::fmin(mn.z, t.position.z - ez * 0.5f); mx.z = std::fmax(mx.z, t.position.z + ez * 0.5f);
    }

    center = (mn + mx) * 0.5f;
    // Half the union box's own space diagonal, so a sphere of this radius contains the whole set --
    // the same quantity focusOnSelection's `d = max(50, r/tan(30 deg)*1.6)` already expects from
    // selectedRadius for a single object.
    radius = std::fmax(1.0f, dist(mn, mx) * 0.5f);
    return true;
}

#if AVER_WITH_IMGUI
// End: drops every selected entity straight down onto whatever is beneath it.
//
// A straight-down ray, not a full pick()-style click ray (+Z is up, Math.hpp's top comment /
// dropRestLift's "along world +Z", so "the floor" is unambiguously -Z). Reuses pick()'s two-world
// broadphase (placeholder boxes, then scene entities nearest-box-first via rayPickGeometry/
// pickGeometryFor) plus a landscape query (surfaceHeightAt, a vertical query, not raycastHeightfield's march).
//
// Everything else in the selection is ineligible ground: an object mustn't land on another this
// same command is about to move, so each entity's ray skips every entity in `sel`, not just itself.
void SandboxApp::snapSelectionToFloor() {
    const std::vector<scene::Entity> sel = selectedEntities();
    if (sel.empty()) return;
    scene::World& w = scene::World::instance();

    // One undo entry for the whole operation: beginTransformEdit captures the anchor and (via
    // forEachMultiMoved) every other selected entity's LOCAL transform before any writes;
    // endTransformEdit pushes one Transform command covering everything that moved.
    if (!beginTransformEdit()) return;

    for (const scene::Entity e : sel) {
        if (!w.valid(e)) continue;
        const Transform before = worldTransformOf(w, e);
        const auto* selfMr = w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);

        // Ray starts clear of the entity's OWN top, not its pivot: starting inside a tall mesh
        // could hit its own back faces (or itself, if not already excluded below).
        f32 halfZ = 1.0f;
        if (selfMr) {
            const f32 ez = (selfMr->aabbMax[2] - selfMr->aabbMin[2]) * std::fabs(before.scale.z);
            if (ez > 1e-3f) halfZ = ez * 0.5f;
        }
        const Vec3 ro{before.position.x, before.position.y, before.position.z + halfZ + 5.0f};
        const Vec3 rd{0.0f, 0.0f, -1.0f};

        f32 bestT = 1e30f;
        bool hit = false;
        f32 hitZ = 0.0f;

#if AVER_MODULE_LANDSCAPE
        // Landscape first: straight down over (x, y) is exactly surfaceHeightAt's own vertical-query
        // case, not raycastHeightfield's march.
        if (landscape_.loaded()) {
            f32 z;
            if (landscape::surfaceHeightAt(landscape_.data(), ro.x, ro.y, z) && z < ro.z) {
                bestT = ro.z - z; hit = true; hitZ = z;
            }
        }
#endif
        // Placeholder boxes (Floor/Cube), bounds-only -- pick()'s own placeholder loop. Never one of
        // `sel`: the multi-selection this function walks is always scene entities.
        if (!hideEditorScene_)
            for (const MeshObj& o : objects_) {
                if (!o.visible) continue;
                Transform tr; tr.position = o.pos; tr.rotation = quatFromEulerDeg(o.rotDeg); tr.scale = o.scale;
                const Mat4 iw = tr.toMatrix().inverse();
                const Vec3 lo = xformPoint(iw, ro), ld = xformVec(iw, rd);
                f32 t;
                if (rayAabb(lo, ld, o.aabbMin, o.aabbMax, t) && t > 0.0f && t < bestT) {
                    bestT = t; hit = true; hitZ = ro.z - t;
                }
            }

        // Every other eligible scene entity -- pick()'s own broadphase (streamed entities excluded,
        // a visible resolved mesh required), minus this whole selection.
        {
            struct FloorCand { scene::Entity ent; u64 meshId; f32 tBox; bool insideBox; Vec3 lo, ld; };
            std::vector<FloorCand> cands;
            const u32 n = w.count();
            for (u32 i = 0; i < n; ++i) {
                const scene::Entity ent = w.at(i);
                if (!w.valid(ent) || w.destroyPending(ent)) continue;
                if (std::find(sel.begin(), sel.end(), ent) != sel.end()) continue;
                if (anyChunkWorldOwns(ent)) continue;
                const auto* mr = w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
                if (!mr || !(mr->flags & scene::kMeshRendererVisible) || mr->mesh == 0) continue;
                if (content_.meshFor(mr->mesh) == 0) continue;
                Vec3 lmin{mr->aabbMin[0], mr->aabbMin[1], mr->aabbMin[2]};
                Vec3 lmax{mr->aabbMax[0], mr->aabbMax[1], mr->aabbMax[2]};
                if (!(lmax.x > lmin.x && lmax.y > lmin.y && lmax.z > lmin.z)) { lmin = Vec3{-1,-1,-1}; lmax = Vec3{1,1,1}; }
                const Mat4 iw = w.worldMatrix(ent).inverse();
                const Vec3 lo = xformPoint(iw, ro), ld = xformVec(iw, rd);
                f32 tBox;
                if (!rayAabb(lo, ld, lmin, lmax, tBox)) continue;
                cands.push_back({ent, mr->mesh, tBox, tBox <= 0.0f, lo, ld});
            }
            std::sort(cands.begin(), cands.end(),
                      [](const FloorCand& a, const FloorCand& b) { return a.tBox < b.tBox; });
            for (const FloorCand& c : cands) {
                if (c.tBox >= bestT) break;
                const bool posedOrSkinned = skinnedMeshIds_.count(c.meshId) != 0 || posedHandle(c.ent) != 0;
                const aver::editor::PickGeometry* geo = posedOrSkinned ? nullptr : &pickGeometryFor(c.meshId);
                if (!geo || geo->empty()) {
                    if (c.tBox > 0.0f) { bestT = c.tBox; hit = true; hitZ = ro.z - c.tBox; }
                    continue;
                }
                f32 tTri;
                if (aver::editor::rayPickGeometry(*geo, c.lo, c.ld, /*skipBackFaces=*/c.insideBox, bestT, tTri))
                    { bestT = tTri; hit = true; hitZ = ro.z - tTri; }
            }
        }

        if (!hit) continue;   // nothing below it: leave it where it is

        const f32 liftZ = selfMr ? editor::dropRestLift(selfMr->aabbMin[2], before.scale.z) : 0.0f;
        Transform after = before;
        after.position.z = hitZ + liftZ;
        w.setLocalTransform(e, localFromWorldFor(w, e,
            EditXform{after.position, eulerDegFromQuat(after.rotation), after.scale}));
    }

    endTransformEdit();
}

#endif   // AVER_WITH_IMGUI

// Moves the whole selection by one world-space step. Undo lifecycle (one entry per HELD RUN, not
// per repeat) is the caller's job (handleManip's edit-verb block); this only applies one step and
// trusts a transform edit is already open.
void SandboxApp::nudgeSelection(const Vec3& deltaCm) {
    EditXform o;
    if (!selectedXform(o)) return;
    o.pos += deltaCm;
    setSelectedXform(o);
    scene::World& w = scene::World::instance();
    forEachMultiMoved([&](scene::Entity ent, const Transform& xf) {
        Transform t = xf;
        t.position += deltaCm;
        w.setLocalTransform(ent, t);
    });
}

// Authored visibility: what saveLevel writes and the Details panel's Visible checkbox shows, as
// distinct from the raw kMeshRendererVisible bit hideSelection/isolateSelection clear for a
// SESSION-ONLY hide. editorHidden_ records "bit is off because H did it", so an H-hidden entity
// (bit clear) still reads as authored visible.
bool SandboxApp::authoredVisible(scene::Entity e) const {
    scene::World& w = scene::World::instance();
    const auto* mr = w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
    if (!mr) return true;   // no renderer, nothing to hide -- matches OcWorldPlacement::visible's own default
    if (mr->flags & scene::kMeshRendererVisible) return true;
    return std::find(editorHidden_.begin(), editorHidden_.end(), e) != editorHidden_.end();
}

// Sets the AUTHORED bit directly (Details panel's write path; saveLevel reads the same bit).
// Removes `e` from editorHidden_ first: an authored edit supersedes any temporary H-hide, so the
// bit alone is the truth again -- unchecking Visible on something H hid actually hides it for
// real (and checking it un-hides for real), rather than leaving a stale editorHidden_ entry to
// reassert itself on the next Ctrl+H.
void SandboxApp::setAuthoredVisible(scene::Entity e, bool v) {
    scene::World& w = scene::World::instance();
    auto* mr = w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
    if (!mr) return;
    if (const auto it = std::find(editorHidden_.begin(), editorHidden_.end(), e); it != editorHidden_.end())
        editorHidden_.erase(it);
    if (v) mr->flags |=  scene::kMeshRendererVisible;
    else   mr->flags &= ~scene::kMeshRendererVisible;
}

// H: session-only visibility, off. Clears kMeshRendererVisible, the same flag the Details panel's
// Visible checkbox writes, so a hidden entity reads identically everywhere that flag is consulted
// (render loop, pick()'s eligibility). Deliberately not undoable/not a level edit -- H is
// temporary by choice, not because a level can't store visibility (it can; see
// authoredVisible/setAuthoredVisible above) -- so this never calls pushEdit, never marks the level unsaved.
void SandboxApp::hideSelection() {
    scene::World& w = scene::World::instance();
    for (const scene::Entity e : selectedEntities()) {
        auto* mr = w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
        if (!mr || !(mr->flags & scene::kMeshRendererVisible)) continue;   // already hidden: not ours to restore
        mr->flags &= ~scene::kMeshRendererVisible;
        editorHidden_.push_back(e);
    }
}

// Shift+H: hides every OTHER eligible entity ("eligible" = whatever pick() would select), so
// isolate never touches what a click couldn't reach (streamed, no visible mesh, unresolved mesh).
void SandboxApp::isolateSelection() {
    const std::vector<scene::Entity> sel = selectedEntities();
    scene::World& w = scene::World::instance();
    const u32 n = w.count();
    for (u32 i = 0; i < n; ++i) {
        const scene::Entity ent = w.at(i);
        if (!w.valid(ent) || w.destroyPending(ent)) continue;
        if (anyChunkWorldOwns(ent)) continue;
        if (std::find(sel.begin(), sel.end(), ent) != sel.end()) continue;
        auto* mr = w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
        if (!mr || !(mr->flags & scene::kMeshRendererVisible) || mr->mesh == 0) continue;
        if (content_.meshFor(mr->mesh) == 0) continue;
        mr->flags &= ~scene::kMeshRendererVisible;
        editorHidden_.push_back(ent);
    }
}

// Ctrl+H: restores exactly what hideSelection/isolateSelection turned off THIS SESSION -- not
// "everything", so an entity some other mechanism hid is left alone.
void SandboxApp::unhideAll() {
    scene::World& w = scene::World::instance();
    for (const scene::Entity e : editorHidden_) {
        if (!w.valid(e)) continue;
        if (auto* mr = w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer))
            mr->flags |= scene::kMeshRendererVisible;
    }
    editorHidden_.clear();
}

#endif   // AVER_MODULE_SCENE

#if AVER_WITH_IMGUI
// The viewport's RIGHT-CLICK MENU: Play From Here. RMB is also the fly-look button, so a press cannot
// open anything -- it ARMS here and the RELEASE decides. Only a click that stayed put opens the menu:
// the pointer inside ImGui's drag threshold AND the camera exactly where it was (holding RMB and
// flying with WASD moves the camera without moving the mouse), so letting go of a fly never pops one.
// Not BeginPopupContextWindow: it opens on the release of every fly-drag too, and it wants the item it
// is attached to, which is the Level's image over in SandboxShell.cpp. Called at the top level of the
// frame (after the overlay bars), where OpenPopup and BeginPopup share one ID scope.
void SandboxApp::drawViewportContextMenu() {
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
    const ImGuiIO& io = ImGui::GetIO();
    // levelHovered_ is the fly camera's own "pointer is over the Level, not a panel or a bar" test.
    const bool overLevel = levelHovered_ && inViewport(io.MousePos.x, io.MousePos.y);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && overLevel) {
        vpCtx_.armed = true;
        vpCtx_.camPos = camPos_;
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && vpCtx_.armed) {
        vpCtx_.armed = false;
        const ImVec2 down = io.MouseClickedPos[ImGuiMouseButton_Right];
        const f32 dx = io.MousePos.x - down.x, dy = io.MousePos.y - down.y;
        const bool still = dx * dx + dy * dy <= io.MouseDragThreshold * io.MouseDragThreshold &&
                           dist(camPos_, vpCtx_.camPos) < 0.01f;
        // Not while a session runs: the viewport is the game's then, and a second Play cannot layer.
        if (still && overLevel && !anyPlayActive()) {
            // Cast ONCE, at the release point, so what the tooltip shows is what the item plays at.
            vpCtx_.hasHit = pickSurfacePoint(io.MousePos.x, io.MousePos.y, vpCtx_.hit);
            ImGui::OpenPopup("##vpContext");
        }
    }
    if (ImGui::BeginPopup("##vpContext")) {
        const bool canPlay = vpCtx_.hasHit && !anyPlayActive();
        if (!canPlay) ImGui::BeginDisabled();
        if (ImGui::MenuItem(ICON_PLAY " Play From Here")) playFromHere(vpCtx_.hit);
        uiReg_.track("viewport.context.playFromHere");
        if (!canPlay) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (vpCtx_.hasHit)
                ImGui::SetTooltip("Play with the player standing at (%.0f, %.0f, %.0f), facing the view.\n"
                                  "The Play options' spawn choice is left alone.",
                                  vpCtx_.hit.x, vpCtx_.hit.y, vpCtx_.hit.z);
            else
                ImGui::SetTooltip("Nothing under the cursor to stand on.");
        }
        ImGui::EndPopup();
    }
#endif
}

// Draws the viewport's overlay bars: view options on the left, transform tools, snapping and
// camera speed on the right.
void SandboxApp::buildViewportOverlay() {
    if (vpW_ < 80.0f || vpH_ < 60.0f) return;
    const f32 pad = 8.0f*dpi_;
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|
                               ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_AlwaysAutoResize|
                               ImGuiWindowFlags_NoDocking|ImGuiWindowFlags_NoSavedSettings|
                               ImGuiWindowFlags_NoFocusOnAppearing|ImGuiWindowFlags_NoNavFocus;

    ImGui::SetNextWindowPos(ImVec2(vpX_+pad, vpY_+pad), ImGuiCond_Always, ImVec2(0,0));
    ImGui::SetNextWindowBgAlpha(0.62f);
    ImGui::Begin("##vpbar_left", nullptr, f);
    if (dropButton("Perspective")) ImGui::OpenPopup("viewType");
    uiReg_.track("viewport.perspective");
    if (ImGui::BeginPopup("viewType")) {
        ImGui::Selectable("Perspective", true);
        const char* orthos[] = {"Top","Bottom","Left","Right","Front","Back"};
        for (const char* o : orthos) ImGui::Selectable(o, false, ImGuiSelectableFlags_Disabled);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    // Wireframe/a G-buffer debug view force the rasteriser; a ray-hit/triangles debug view forces
    // ray-driven primary visibility -- onUpdate's scratch-copy override (SandboxApp.cpp), never
    // written back to the project. authoredRayDriven is whether that's actually a FALLBACK, the
    // only case worth a "(raster)" note: a project already on the rasteriser shows it as authored.
    bool authoredRayDriven = false;
#if AVER_MODULE_VOXI
    authoredRayDriven = voxi::Renderer::get().settings().rtRenderMode == 1u;
#endif
    std::string viewModeLabel = "Lit";
#if AVER_MODULE_VOXI
    if (debugView_ != voxi::VoxiRenderer::ViewDebug::None) {
        switch (debugView_) {
            case voxi::VoxiRenderer::ViewDebug::RayHitInstance: viewModeLabel = "Ray Hit: Instances"; break;
            case voxi::VoxiRenderer::ViewDebug::RayHitMaterial: viewModeLabel = "Ray Hit: Materials"; break;
            case voxi::VoxiRenderer::ViewDebug::RayHitDistance: viewModeLabel = "Ray Hit: Distance"; break;
            case voxi::VoxiRenderer::ViewDebug::Triangles:      viewModeLabel = "Triangles"; break;
            case voxi::VoxiRenderer::ViewDebug::AmbientOcclusion: viewModeLabel = "Ambient Occlusion"; break;
            default: break;
        }
    } else
#endif
    if (wireframe_) {
        viewModeLabel = "Wireframe";
    } else if (neuraaDebugView_ && gbufferDebugView_ == GBufferDebugFeature::Mode::Off) {
        viewModeLabel = "Edge Classes (NeuRAA)";
    } else if (gbufferDebugView_ != GBufferDebugFeature::Mode::Off) {
        using GDM = GBufferDebugFeature::Mode;
        viewModeLabel = gbufferDebugView_ == GDM::Velocity ? "G-Buffer: Velocity" :
                        gbufferDebugView_ == GDM::ViewZ ? "G-Buffer: View-Space Depth" : "G-Buffer: Normal + Roughness";
        if (authoredRayDriven) viewModeLabel += " (raster)";
    } else if (unlit_) {
        viewModeLabel = "Unlit";
    }
    if (dropButton(viewModeLabel.c_str())) ImGui::OpenPopup("viewMode");
    if (ImGui::BeginPopup("viewMode")) {
        if (ImGui::Selectable("Lit", !wireframe_ && !unlit_ && !neuraaDebugView_)) {
            wireframe_=false; unlit_=false; neuraaDebugView_=false;
        }
        // Unlit is not gated on the rasteriser: PSRayDriven honours gViewParams.x itself, so it
        // works under ray-driven as well as under Wireframe's forced fallback.
        if (ImGui::Selectable("Unlit", unlit_ && !wireframe_)) { unlit_ = true; wireframe_ = false; }
        uiReg_.track("viewMode.unlit");
        // Wireframe always selectable now: it used to BeginDisabled whenever ray-driven suppressed
        // the raster scene pass, with a tooltip pointing to Settings > Rendering, but onUpdate's
        // scratch-copy override (needRaster in its Voxi settings block) now does that for THIS
        // VIEWPORT ONLY every frame, so nothing is left to disable here.
        if (ImGui::Selectable("Wireframe", wireframe_)) {
            wireframe_ = true; unlit_ = false;
#if AVER_MODULE_VOXI
            debugView_ = voxi::VoxiRenderer::ViewDebug::None;
#endif
        }
        uiReg_.track("viewMode.wireframe");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Every mesh's edges, unlit, like Unreal's Wireframe view. Lighting, GI and\n"
                              "ray tracing pause while it is selected. Project Settings are unchanged.");
        ImGui::Selectable("Detail Lighting", false, ImGuiSelectableFlags_Disabled);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Needs a flat-albedo shading override the shader does not have yet.");
#if AVER_MODULE_VOXI
        ImGui::Separator();
        if (ImGui::Selectable("Voxel Radiance (GI debug)", giDebugView_)) giDebugView_ = !giDebugView_;
        // Same two console-var GI paints Settings > Rendering exposes (SandboxSettings.cpp): raw
        // bool slots owned by EditorConsole.hpp (see consoleGiPoisonViewSlot()'s comment there),
        // reasserted onto the live voxiRenderer_ every frame from onUpdate. Toggling the slots
        // directly here keeps this entry and the Settings checkbox the same switch, not two that
        // can disagree.
        if (ImGui::Selectable("GI Poison View (debug)", editor::consoleGiPoisonViewSlot()))
            editor::consoleGiPoisonViewSlot() = !editor::consoleGiPoisonViewSlot();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Paints an unmistakable colour over any pixel where a ReSTIR GI guard\n"
                              "fired this frame. See Settings > Rendering for the colour legend.");
        if (ImGui::Selectable("GI Visibility Path View (debug)", editor::consoleGiVisPathViewSlot()))
            editor::consoleGiVisPathViewSlot() = !editor::consoleGiVisPathViewSlot();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Paints F2's resolved GI visibility path (traced/reconstructed/\n"
                              "half-res/no-ray) over indirect diffuse. Suppressed while GI Poison\n"
                              "View above is also on.");
#endif
        // G-buffer debug views: generic IDevice state (RHI.hpp), not gated on any module. Selecting
        // the active one toggles it off (same toggle-back idiom the GI entry uses); a different
        // one switches directly. onUpdate ORs gbufferOverride_ with this value, so any pick here
        // is sufficient to turn the G-buffer on too.
        // Same raster-only note as Wireframe: these hang the GPU under ray-driven, so onUpdate
        // falls back to the rasteriser for these too, and clears an active ray-hit/triangles view
        // (which needs ray-driven) for the same reason.
        ImGui::Separator();
        {
            using GDM = GBufferDebugFeature::Mode;
            auto gbufItem = [&](const char* label, GDM m) {
                if (ImGui::Selectable(label, gbufferDebugView_ == m)) {
                    gbufferDebugView_ = (gbufferDebugView_ == m) ? GDM::Off : m;
#if AVER_MODULE_VOXI
                    if (gbufferDebugView_ != GDM::Off) debugView_ = voxi::VoxiRenderer::ViewDebug::None;
#endif
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Switches this viewport to the rasteriser while selected, and back\n"
                                      "when you leave it. Project Settings > Rendering > Ray Tracing is unchanged.");
            };
            // Scale documented once, in the label, rather than left for a reader to find in the
            // shader: a debug view whose scale is undocumented is decorative, not diagnostic.
            gbufItem("G-Buffer: Velocity (+/-8 texels/frame full-scale, debug)", GDM::Velocity);
            gbufItem("G-Buffer: View-Space Depth (debug)", GDM::ViewZ);
            gbufItem("G-Buffer: Normal + Roughness (debug)", GDM::NormalRoughness);
        }
#if AVER_MODULE_VOXI
        // Ray-hit/triangles views only PSRayDriven paints (voxi.hlsl, see ViewDebug in
        // VoxiRenderer.hpp). Radio-like against wireframe_/gbufferDebugView_ and each other:
        // re-selecting the active one turns it off; they need ray-driven, those need the
        // rasteriser (mutually exclusive by construction, see onUpdate's needRaster/needRayDriven).
        // Disabled with a reason tooltip when ray-driven can't engage even if forced (see
        // rayDrivenAvailable).
        ImGui::Separator();
        const bool rdAvailable = voxiRenderer_.rayDrivenAvailable();
        using VD = voxi::VoxiRenderer::ViewDebug;
        auto debugItem = [&](const char* label, VD m, const char* trackName) {
            ImGui::BeginDisabled(!rdAvailable);
            if (ImGui::Selectable(label, debugView_ == m)) {
                debugView_ = (debugView_ == m) ? VD::None : m;
                if (debugView_ != VD::None) {
                    wireframe_ = false;
                    gbufferDebugView_ = GBufferDebugFeature::Mode::Off;
                }
            }
            ImGui::EndDisabled();
            uiReg_.track(trackName);
            if (!rdAvailable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Needs ray tracing: Settings > Rendering > Ray Tracing.");
            else if (rdAvailable && ImGui::IsItemHovered())
                ImGui::SetTooltip("Switches this viewport to ray-driven primary visibility while selected\n"
                                  "if the project is set to the rasteriser, and back when you leave it.");
        };
        debugItem("Ray Hit: Instances", VD::RayHitInstance, "viewMode.rayHitInstance");
        debugItem("Ray Hit: Materials", VD::RayHitMaterial, "viewMode.rayHitMaterial");
        debugItem("Ray Hit: Distance", VD::RayHitDistance, "viewMode.rayHitDistance");
        debugItem("Triangles", VD::Triangles, "viewMode.triangles");
        debugItem("Ambient Occlusion", VD::AmbientOcclusion, "viewMode.ambientOcclusion");
        // NeuRAA's edge detection (docs/rendering/NEURAA_NRD.md): drawn by the AverSR seam, not
        // PSRayDriven, so the scene keeps its normal shading underneath.
        ImGui::BeginDisabled(!rdAvailable);
        if (ImGui::Selectable("Edge Classes (NeuRAA)", neuraaDebugView_)) {
            neuraaDebugView_ = !neuraaDebugView_;
            if (neuraaDebugView_) {
                wireframe_ = false;
                debugView_ = VD::None;
                gbufferDebugView_ = GBufferDebugFeature::Mode::Off;
            }
        }
        ImGui::EndDisabled();
        uiReg_.track("viewMode.neuraaEdges");
        if (!rdAvailable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Needs ray tracing: Settings > Rendering > Ray Tracing.");
        else if (rdAvailable && ImGui::IsItemHovered())
            ImGui::SetTooltip("Red: silhouette. Yellow: depth step. Cyan: crease.\n"
                              "Needs the staged ray-driven passes (Primary rays, stages 1 or 2).");
#endif
        // Undenoised is independent of every mode above (stays on across Lit/Unlit/Wireframe/
        // debug-view, combines with any) -- see onUpdate's UNDENOISED comment for the knobs this bundles.
        ImGui::Separator();
        if (ImGui::Selectable("Undenoised", undenoised_)) undenoised_ = !undenoised_;
        uiReg_.track("viewMode.undenoised");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Turns off the denoiser, the RT sun-shadow spatial filter and ReSTIR GI's spatial\n"
                              "reuse, forces every pixel to trace every frame, and resets GI/RT/denoiser\n"
                              "history every frame (no temporal accumulation). The reflection and\n"
                              "sky-occlusion spatial filters have no runtime knob and stay on.");
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (dropButton("Show")) ImGui::OpenPopup("showFlags");
    uiReg_.track("viewport.show");
    if (ImGui::BeginPopup("showFlags")) {
        ImGui::Checkbox("Grid", &showGrid_);
        uiReg_.track("show.grid");
        // These two used to share a stack-local `bool t=true;`, re-initialised every frame and
        // written by both boxes, so they always rendered ticked, could never be unticked, and
        // toggled nothing. Now real state.
        ImGui::Checkbox("Static Meshes", &showStaticMeshes_);
        uiReg_.track("show.staticMeshes");
        ImGui::Checkbox("Atmosphere", &showAtmosphere_);
        uiReg_.track("show.atmosphere");
        ImGui::EndPopup();
    }
    ImGui::End();

    const f32 icon = 26.0f*dpi_, caretW = 14.0f*dpi_, tiny = 2.0f*dpi_, gap = 6.0f*dpi_;
    ImGui::SetNextWindowPos(ImVec2(vpX_+vpW_-pad, vpY_+pad), ImGuiCond_Always, ImVec2(1,0));
    ImGui::SetNextWindowBgAlpha(0.62f);
    ImGui::Begin("##vpbar_right", nullptr, f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto toolBtn = [&](const char* id, int kind, bool active)->bool {
        return editor::toolButton(id, kind, active, icon, dpi_);
    };
    auto caretBtn = [&](const char* id, bool on)->bool {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(id, ImVec2(caretW, icon));
        const bool hov = ImGui::IsItemHovered(), clk = ImGui::IsItemClicked();
        if (hov) dl->AddRectFilled(p, ImVec2(p.x+caretW,p.y+icon), IM_COL32(74,76,82,255), 3.0f);
        const ImVec2 c(p.x+caretW*0.5f, p.y+icon*0.5f); const f32 s=3.0f*dpi_;
        const ImU32 col = on ? IM_COL32(232,150,60,255) : IM_COL32(190,191,195,255);
        dl->AddTriangleFilled(ImVec2(c.x-s,c.y-s*0.6f), ImVec2(c.x+s,c.y-s*0.6f), ImVec2(c.x,c.y+s*0.8f), col);
        return clk;
    };

    // Mode first, then that mode's tools, always leftmost ("what am I editing" before "with which
    // tool" -- previously one flat row, how a brush ended up next to Rotate).
    // A dropdown, not a row of buttons: two modes fit a row, four don't, and it'd grow per mode
    // added; a combo also lets an unavailable entry carry its own explanation, which a greyed
    // button can only deliver by hovering something that looks broken.
    {
        ImGui::PushStyleColor(ImGuiCol_Button, kAverOrangeDim);
        ImGui::PushStyleColor(ImGuiCol_Header, kAverOrangeDim);
        // Sized from the widest mode name, not a constant: this toolbar is right-anchored and
        // grows LEFTWARD, so an over-wide item marches across the viewport onto the left toolbar (a fixed 150*dpi did this at 300% DPI).
        f32 widest = 0.0f;
        for (int i = 0; i < kEditorModeCount; ++i)
            widest = std::fmax(widest, ImGui::CalcTextSize(kEditorModeNames[i]).x);
        ImGui::SetNextItemWidth(widest + ImGui::GetFrameHeight() +
                                ImGui::GetStyle().FramePadding.x * 4.0f);
        const char* modeLbl = kEditorModeNames[static_cast<int>(mode_)];
        if (ImGui::BeginCombo("##editorMode", modeLbl, ImGuiComboFlags_HeightLarge)) {
            for (int i = 0; i < kEditorModeCount; ++i) {
                const EditorMode m = static_cast<EditorMode>(i);
                const char* whyNot = "";
                const bool ok = editorModeAvailable(m, &whyNot);
                if (!ok) ImGui::BeginDisabled();
                if (ImGui::Selectable(kEditorModeNames[i], mode_ == m)) setEditorMode(m);
                ImGui::SameLine();
                ImGui::TextDisabled("-- %s", kEditorModeHints[i]);
                if (!ok) {
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("%s", whyNot);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleColor(2);
        uiReg_.track("mode.dropdown");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Editing mode. Tab switches between Select and the last mode used.");
        ImGui::SameLine(0, gap*2);
    }

#if AVER_MODULE_LANDSCAPE
    if (mode_ == EditorMode::Landscape) {
        if (toolBtn("##tSRaise", 4, sculptTool_==SculptTool::Raise)) sculptTool_=SculptTool::Raise;
        uiReg_.track("tool.sculptRaise");
        ImGui::SameLine(0, gap);
        if (toolBtn("##tSLower", 5, sculptTool_==SculptTool::Lower)) sculptTool_=SculptTool::Lower;
        uiReg_.track("tool.sculptLower");
        ImGui::SameLine(0, gap);
        if (toolBtn("##tSSmooth", 6, sculptTool_==SculptTool::Smooth)) sculptTool_=SculptTool::Smooth;
        uiReg_.track("tool.sculptSmooth");
        ImGui::SameLine(0, gap);
        if (toolBtn("##tSFlatten", 7, sculptTool_==SculptTool::Flatten)) sculptTool_=SculptTool::Flatten;
        uiReg_.track("tool.sculptFlatten");
        ImGui::SameLine(0, gap);
        if (toolBtn("##tSRamp", 8, sculptTool_==SculptTool::Ramp)) sculptTool_=SculptTool::Ramp;
        uiReg_.track("tool.sculptRamp");
        ImGui::SameLine(0, gap);
        if (toolBtn("##tSNoise", 9, sculptTool_==SculptTool::Noise)) sculptTool_=SculptTool::Noise;
        uiReg_.track("tool.sculptNoise");
        ImGui::SameLine(0, gap*2);
        // Brush settings live here, in the mode that owns them, not appearing/disappearing from a shared row by tool.
        char brushLbl[32]; std::snprintf(brushLbl, sizeof brushLbl, "Brush %.0f", sculptRadiusCm_);
        if (dropButton(brushLbl)) ImGui::OpenPopup("brushParams");
        ImGui::SameLine(0, gap);
        if (ImGui::Button(landscape_.dirty() ? "Save Terrain *" : "Save Terrain")) saveLandscape();
        uiReg_.track("landscape.save");
        if (ImGui::BeginPopup("brushParams")) {
            editor::panelFloat("Radius (cm)", &sculptRadiusCm_, 50.0f, 5000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Strength (cm)", &sculptStrengthCm_, 5.0f, 2000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
            ImGui::EndPopup();
        }
    } else
#endif
    {
    if (toolBtn("##tSel", 0, tool_==Tool::Select)) tool_=Tool::Select;
    uiReg_.track("tool.select");
    ImGui::SameLine(0, gap);
    if (toolBtn("##tMove", 1, tool_==Tool::Move)) tool_=Tool::Move;
    uiReg_.track("tool.move");
    ImGui::SameLine(0, tiny); if (caretBtn("##cMove", snapMove_)) ImGui::OpenPopup("snapMove");
    ImGui::SameLine(0, gap);
    if (toolBtn("##tRot", 2, tool_==Tool::Rotate)) tool_=Tool::Rotate;
    uiReg_.track("tool.rotate");
    ImGui::SameLine(0, tiny); if (caretBtn("##cRot", snapRot_)) ImGui::OpenPopup("snapRot");
    ImGui::SameLine(0, gap);
    if (toolBtn("##tScl", 3, tool_==Tool::Scale)) tool_=Tool::Scale;
    uiReg_.track("tool.scale");
    ImGui::SameLine(0, tiny); if (caretBtn("##cScl", snapScale_)) ImGui::OpenPopup("snapScale");
    ImGui::SameLine(0, gap*2);
    if (ImGui::Button(worldSpace_ ? "World" : "Local")) worldSpace_ = !worldSpace_;
    }

    ImGui::SameLine(0, gap);
    // "Cam 1" at the default 800 cm/s: %g (not %f) so the default reads as "Cam 1" not "Cam 1.00",
    // while slow speeds (0.25 vs 0.5 matters) keep 3 sig figs; whole numbers above 10 only -- at
    // speed 18 decimals would be false accuracy.
    char camLbl[32];
    const f32 camDial = flySpeed_ / kCamSpeedUnit;
    std::snprintf(camLbl, sizeof camLbl, camDial < 10.0f ? "Cam %.3g" : "Cam %.0f", camDial);
    if (dropButton(camLbl)) ImGui::OpenPopup("camSpeed");
    if (ImGui::BeginPopup("camSpeed")) {
        f32 dial = flySpeed_ / kCamSpeedUnit;
        if (ImGui::SliderFloat("Speed", &dial, 20.0f / kCamSpeedUnit, 20000.0f / kCamSpeedUnit,
                               "%.2f", ImGuiSliderFlags_Logarithmic))
            flySpeed_ = dial * kCamSpeedUnit;
        // Still shown (not the number you steer by) because someone measuring a fly-through needs the actual rate.
        ImGui::TextDisabled("%.0f cm/s", flySpeed_);
        ImGui::TextDisabled("Right-drag the viewport and scroll UP to speed up.");
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("snapMove")) {
        ImGui::Checkbox("Grid snap (position)", &snapMove_); ImGui::Separator();
        const f32 opts[] = {0.1f,0.25f,0.5f,1,2,5,10,50,100};
        for (f32 v : opts){ char b[24]; std::snprintf(b,sizeof b,"%g units", v); if (ImGui::Selectable(b, moveSnap_==v)){ moveSnap_=v; snapMove_=true; } }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("snapRot")) {
        ImGui::Checkbox("Angle snap (rotation)", &snapRot_); ImGui::Separator();
        const f32 opts[] = {1,5,10,15,30,45,90};
        for (f32 v : opts){ char b[24]; std::snprintf(b,sizeof b,"%gÃ‚Â°", v); if (ImGui::Selectable(b, rotSnap_==v)){ rotSnap_=v; snapRot_=true; } }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("snapScale")) {
        ImGui::Checkbox("Scale snap", &snapScale_); ImGui::Separator();
        const f32 opts[] = {0.05f,0.1f,0.25f,0.5f,1};
        for (f32 v : opts){ char b[24]; std::snprintf(b,sizeof b,"%g", v); if (ImGui::Selectable(b, scaleSnap_==v)){ scaleSnap_=v; snapScale_=true; } }
        ImGui::EndPopup();
    }
    ImGui::End();

    // Raised by the open drawer's height, a fix not a nicety: this hint anchors to the viewport's
    // bottom edge, but vpH_ isn't reduced when a drawer opens (an overlay, not a dock split), so it
    // used to sit on top of the drawer's last ~70px. Cosmetic for Content Browser/Output Log; the
    // Console's bottom row is its input line, invisible underneath but still clickable/typeable
    // blind (overlay is NoInputs). drawerPixelH_ is the ANIMATED height, so the hint slides with
    // it, zero when closed.
    ImGui::SetNextWindowPos(ImVec2(vpX_+pad, vpY_+vpH_-pad-drawerPixelH_), ImGuiCond_Always, ImVec2(0,1));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("##vphint", nullptr, f | ImGuiWindowFlags_NoInputs);
    // Ejected reads ahead of the mode switch: the mode is still whatever it was when Play started
    // (Simulate, usually), but the hint that matters now is "you have the editor back", not that one.
    // playEjected() is unguarded (false with no framework), so this needs no module guard either.
    if (playEjected()) {
        // The Pawn to Camera clause only when that command has a chord ("none brings the pawn here"
        // otherwise).
        const editor::Chord& pawnChord = keybinds_.chordFor(editor::CommandId::PlayPawnToCamera);
        const std::string pawnClause =
            pawnChord.isBound() ? editor::chordToString(pawnChord) + " brings the pawn here, " : std::string();
        ImGui::Text("Ejected  |  the game is running  |  %s possesses, %s%s stops",
                    editor::chordToString(keybinds_.chordFor(editor::CommandId::PlayEject)).c_str(),
                    pawnClause.c_str(),
                    editor::chordToString(keybinds_.chordFor(editor::CommandId::PlayStop)).c_str());
    } else
    // One arm per mode: the two-arm version said "Tab to Landscape" while in Foliage, and called
    // the mode "Select" while a foliage brush was armed -- worse than no hint at all.
    switch (mode_) {
#if AVER_MODULE_LANDSCAPE
        case EditorMode::Landscape:
            ImGui::Text("Landscape: %s  |  LMB paint  |  radius %.0f cm, strength %.0f cm  |  "
                        "1-4 brushes, Ctrl+Z undoes a stroke, Tab to Select",
                        kSculptToolNames[(int)sculptTool_], sculptRadiusCm_, sculptStrengthCm_);
            break;
        case EditorMode::Foliage:
            ImGui::Text("Foliage  |  LMB paint, Shift+LMB erase  |  radius %.0f cm, density %.0f  |  "
                        "Ctrl+Z undoes a whole stroke, Tab to Select", foliageRadiusCm_, foliageDensity_);
            break;
#else
        case EditorMode::Landscape:
        case EditorMode::Foliage:
            break;
#endif
        case EditorMode::Simulate:
            ImGui::Text("Simulate  |  the game is running in the viewport  |  "
                        "%s releases the mouse, Tab to Select",
                        editor::chordToString(keybinds_.chordFor(editor::CommandId::PlayReleaseMouse)).c_str());
            break;
        case EditorMode::Select:
        default:
            ImGui::Text("Select: %s  |  RMB fly (WASD/QE)  wheel speed  MMB pan  F focus  |  "
                        "1-4 tools, Tab to the last mode", kToolNames[(int)tool_]);
            break;
    }
    ImGui::End();

    drawViewportContextMenu();
}

#endif

// Builds an outliner label from a surface name plus an ordinal: "M_Wall" -> "Wall 3". Falls back
// to the asset's stem.
std::string SandboxApp::entityLabelBase(const std::string& surface, const std::string& asset) {
    std::string base = surface;
    if (base.rfind("M_", 0) == 0) base.erase(0, 2);
    if (base.empty()) {
        const std::size_t slash = asset.find_last_of("/\\");
        base = slash == std::string::npos ? asset : asset.substr(slash + 1);
        const std::size_t dot = base.find_last_of('.');
        if (dot != std::string::npos) base.erase(dot);
    }
    if (base.empty()) base = "Entity";
    return base;
}

std::string SandboxApp::makeEntityLabel(const std::string& surface, const std::string& asset) {
    const std::string base = entityLabelBase(surface, asset);
    return base + " " + std::to_string(++labelCounts_[base]);
}

// True only when terrain editing is actually possible right now: mode is Landscape AND a section
// is loaded. Every "should this click sculpt" test goes through here rather than checking the mode
// alone, so a level without terrain loaded cannot be painted into.
bool SandboxApp::editorModeIsLandscape() const {
#if AVER_MODULE_LANDSCAPE
    return mode_ == EditorMode::Landscape && landscape_.loaded();
#else
    return false;
#endif
}

// Whether a mode can be entered AT ALL right now, and why not if it can't. One predicate per mode,
// in one place (dropdown, keybind, setEditorMode all need the same answer); the reason lets the
// dropdown grey an entry AND say why on hover, instead of silently refusing a click.
bool SandboxApp::editorModeAvailable(EditorMode m, const char** whyNot) const {
    auto no = [&](const char* why) { if (whyNot) *whyNot = why; return false; };
    switch (m) {
        case EditorMode::Select:
            return true;
#if AVER_MODULE_LANDSCAPE
        case EditorMode::Landscape:
            // Entered without one, deliberately: it used to refuse, making the terrain mode the one
            // place you couldn't reach to MAKE terrain -- and named two workarounds in the refusal
            // (a hand-written LANDSCAPE record, a CLI flag), neither in the editor. The panel now
            // offers a Create Landscape button when none is resident; every tool inside is still
            // gated on landscape_.loaded().
            return true;
        case EditorMode::Foliage: {
            // See editor::foliageModeGate (FoliageTypeEditor.hpp) for why an empty palette no
            // longer refuses entry, and for the headless test covering both branches.
            const editor::FoliageModeGate gate =
                editor::foliageModeGate(landscape_.loaded(), foliagePalette_.empty());
            if (!gate.available) return no(gate.whyNot);
            return true;
        }
#else
        case EditorMode::Landscape:
        case EditorMode::Foliage:
            return no("This build has the landscape module switched off (AVER_MODULE_LANDSCAPE).");
#endif
        case EditorMode::Simulate:
            return true;
    }
    return no("Unknown mode");
}

// Foliage's equivalent of editorModeIsLandscape(): the mode is selected AND it can actually run.
bool SandboxApp::editorModeIsFoliage() const {
#if AVER_MODULE_LANDSCAPE
    return mode_ == EditorMode::Foliage && landscape_.loaded() && !foliagePalette_.empty();
#else
    return false;
#endif
}

// Switching mode ends whatever the previous one was mid-way through. A drag that began as a
// gizmo move and finishes as a brush stroke would apply one to the other's target.
void SandboxApp::setEditorMode(EditorMode m) {
    if (mode_ == m) return;
#if AVER_MODULE_LANDSCAPE
    // Refreshed here, BEFORE the availability check: a type authored moments ago (Content
    // Browser's New Foliage Type, or a palette row edit) must make the mode enterable on this
    // very click, not only after some other event calls refreshFoliagePalette() first.
    if (m == EditorMode::Foliage) refreshFoliagePalette();
#endif
    const char* whyNot = "";
    if (!editorModeAvailable(m, &whyNot)) {
        AVER_WARN("[Editor] cannot enter {} mode: {}", kEditorModeNames[static_cast<int>(m)], whyNot);
        return;
    }
#if AVER_MODULE_LANDSCAPE
    // A stroke in flight is ENDED, not abandoned: endSculptStroke pushes the undo entry for what
    // was already painted, else the terrain would change with nothing on the undo stack to reverse it.
    if (sculpting_) endSculptStroke();
    sculptCursorValid_ = false;
#endif
    dragging_ = false;
    mode_ = m;
    // Leaving Select with something selected is fine and even useful -- the selection is still
    // there when you come back -- but the gizmo must stop drawing, which it does because its
    // draw path tests the mode.
    AVER_INFO("[Editor] mode: {}", kEditorModeNames[static_cast<int>(m)]);
}

// Tab. Returns to Select from anywhere, and from Select goes to the last non-Select mode used --
// a toggle, not a cycle (cycling four modes would need three presses to get back).
void SandboxApp::toggleEditorMode() {
    if (mode_ != EditorMode::Select) {
        lastNonSelectMode_ = mode_;
        setEditorMode(EditorMode::Select);
    } else {
        setEditorMode(lastNonSelectMode_);
    }
}

// Applies --mode once, after the level has had a chance to load its terrain. Called every frame
// and self-clearing, which is the same shape the other deferred startup flags in this file use:
// there is no single "the project is now ready" callback to hang it off.
void SandboxApp::applyStartMode() {
    if (startMode_.empty()) return;
    const std::string want = startMode_;
    startMode_.clear();
    for (int i = 0; i < kEditorModeCount; ++i) {
        std::string lower = kEditorModeNames[i];
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == want) { setEditorMode(static_cast<EditorMode>(i)); return; }
    }
    AVER_WARN("[Editor] --mode '{}' is not a mode. Use select, landscape, foliage or simulate.", want);
}

} // namespace aver
