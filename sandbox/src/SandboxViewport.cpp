// Editor: the viewport -- gizmos and manipulation, picking, spawning and drops, sculpt and foliage brushes, PlayerStart authoring, overlays, editor modes.
// Part of SandboxApp, split out of the single 29,952-line SandboxApp.cpp on 2026-09-16 by moving method bodies
// verbatim; the class itself is declared in SandboxApp.hpp.

#include "SandboxApp.hpp"
#include "ViewportMarquee.hpp"

namespace aver {
#if AVER_MODULE_SYNAPSE
#if AVER_MODULE_PHYSICS
// Twelve edges per body's world-space AABB, in world space, rebuilt each frame the toggle is on.
//
// AN AABB PER BODY, and the overlay says so on the toggle's tooltip rather than letting someone
// read a box around a sphere as the sphere's own shape. See aver_phys_body_aabb for why the ABI
// reports a bound rather than the shape tree.
void SandboxApp::rebuildColliderOverlay(Engine& e) {
    if (colliderMesh_) { e.device()->destroyLineMesh(colliderMesh_); colliderMesh_ = 0; }
    const int32_t n = aver_phys_body_count();
    if (n <= 0) return;

    std::vector<rhi::LineVertex> lines;
    lines.reserve(static_cast<usize>(n) * 24);
    for (int32_t i = 0; i < n; ++i) {
        const int32_t body = aver_phys_body_at(i);
        if (!body) continue;
        f32 lo[3], hi[3];
        if (!aver_phys_body_aabb(body, lo, hi)) continue;

        // GREEN FOR STATIC, AMBER FOR ANYTHING THAT MOVES. The distinction is the one a person
        // is usually looking for -- "why is this not falling" and "why is this not stopping
        // anything" are different questions and this separates them at a glance.
        const int32_t motion = aver_phys_body_motion_type(body);
        const f32 r = (motion == 0) ? 0.35f : 1.0f;
        const f32 g = (motion == 0) ? 0.95f : 0.72f;
        const f32 b = (motion == 0) ? 0.45f : 0.25f;

        const f32 xs[2] = {lo[0], hi[0]};
        const f32 ys[2] = {lo[1], hi[1]};
        const f32 zs[2] = {lo[2], hi[2]};
        const auto edge = [&](int x0, int y0, int z0, int x1, int y1, int z1) {
            rhi::LineVertex a{}, c{};
            a.px = xs[x0]; a.py = ys[y0]; a.pz = zs[z0]; a.r = r; a.g = g; a.b = b;
            c.px = xs[x1]; c.py = ys[y1]; c.pz = zs[z1]; c.r = r; c.g = g; c.b = b;
            lines.push_back(a);
            lines.push_back(c);
        };
        // Four along each axis: the twelve edges of a box, written out rather than looped so
        // the shape is legible and a wrong corner is visible in the source.
        edge(0,0,0, 1,0,0); edge(0,1,0, 1,1,0); edge(0,0,1, 1,0,1); edge(0,1,1, 1,1,1);
        edge(0,0,0, 0,1,0); edge(1,0,0, 1,1,0); edge(0,0,1, 0,1,1); edge(1,0,1, 1,1,1);
        edge(0,0,0, 0,0,1); edge(1,0,0, 1,0,1); edge(0,1,0, 0,1,1); edge(1,1,0, 1,1,1);
    }
    if (!lines.empty())
        colliderMesh_ = e.device()->createLineMesh(lines.data(), static_cast<u32>(lines.size()));
}

#endif
#endif

#if AVER_MODULE_SYNAPSE
void SandboxApp::rebuildNavOverlay(Engine& e) {
    if (navMesh_) { e.device()->destroyLineMesh(navMesh_); navMesh_ = 0; }
    if (nav_.cells.empty()) return;
    const std::vector<rhi::LineVertex> lines = editor::buildNavOverlay(nav_, navRegionColours_);
    if (lines.empty()) return;
    navMesh_ = e.device()->createLineMesh(lines.data(), (u32)lines.size());
}

#endif

#if AVER_MODULE_LANDSCAPE
// ---- the selection outline, as LINES ----------------------------------------------------------
//
// WHY LINES AND NOT A MESH, which is the whole bug this replaces. The outline used to be the mesh
// redrawn through drawMesh with setWireframe(true) -- and drawMesh is gated on sceneSuppressed(),
// which VoxiRenderer returns TRUE for whenever ray-driven primary visibility is on. Ray-driven is
// this engine's STANDING DEFAULT, so the outline was drawn into nothing: measured on a bounded
// capture with the interactive gate lifted, ONE orange pixel appeared anywhere in the viewport,
// and it was a leaf vein. The device log says it outright -- "The rasteriser draws NOTHING while
// this holds".
//
// drawLines is gated on the much narrower suppressesWholeFrame(), which VoxiRenderer deliberately
// keeps FALSE for ray-driven, and D3D12Device::drawLines says why in as many words: "Gizmos and
// wireframes belong in a ray-driven viewport as much as in a rastered one, and they depth-test
// against the real depth the ray pass writes." The grid, the gizmo, the nav mesh and the collider
// overlay all already ride that path. The outline simply was not on it.
//
// BOUNDARY AND CREASE EDGES, NOT EVERY EDGE. A wireframe of a 31k-triangle plant is an orange
// thicket, not an outline. An edge is drawn when it belongs to exactly ONE triangle (a true
// boundary -- for a leaf, its rim) or when its two triangles disagree in direction by more than
// kCreaseCos. On flat foliage cards that is precisely the silhouette; on a hard-surface prop it is
// the shape's own edges. Camera-independent, so it is built ONCE per mesh and cached rather than
// recomputed as the view moves.
//
// WHY IT READS THE .ocmesh AGAIN: loadProjectMeshes uploads to the GPU and lets the CPU-side
// OcMeshData go, so the triangles are not in memory to walk. A selection change is a click, not a
// frame, and the result is cached by mesh id -- so this costs one file read the first time an
// asset is ever selected and nothing on any later selection of it.
rhi::LineHandle SandboxApp::selectionOutlineLines(Engine& e, u64 meshId) {
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

    // ADJACENCY BY POSITION, NOT BY INDEX, and this is the whole difference between an outline
    // and an orange thicket.
    //
    // Game meshes split a vertex wherever a UV or a normal seam runs, so the two triangles either
    // side of a smooth edge routinely carry DIFFERENT indices for the same corner. Keyed by index,
    // almost no edge finds its neighbour, every edge looks like a boundary, and the "outline"
    // becomes a full wireframe: this Anthurium reported 31,113 edges from 15,544 triangles --
    // more than the ~23k a closed mesh of that size even has -- which is what that measurement
    // means. Welding by position finds the neighbours the indices hide.
    //
    // QUANTISED TO 1/100 cm before hashing, because two authored copies of one corner are equal
    // in intent and rarely equal in float. The engine's unit is the centimetre, so this welds
    // anything within 10 microns and nothing a person would call two places.
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
    // Keyed by the PAIR of welded position ids, order-independent, so an edge walked from either
    // of its two triangles lands in the same bucket.
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
    static constexpr f32 kSelR = 1.0f, kSelG = 0.62f, kSelB = 0.12f;   // the selection orange
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
                         kSelR, kSelG, kSelB});
        lines.push_back({md.positions[usize(y)*3+0], md.positions[usize(y)*3+1], md.positions[usize(y)*3+2],
                         kSelR, kSelG, kSelB});
    }

    const rhi::LineHandle h = lines.empty() ? 0
                            : e.device()->createLineMesh(lines.data(), static_cast<u32>(lines.size()));
    AVER_INFO("[Editor] selection outline for '{}': {} edge(s) of {} triangle(s)",
              pit->second, lines.size() / 2, triCount);
    selOutlineLines_[meshId] = h;
    return h;
}

// Writes one stored rect of samples back into the section and rebuilds what it touched.
// SHARED BY UNDO AND REDO, which differ only in which of the two stored buffers they write. The
// rebuild afterwards is the same work handleSculpt does per tick: the quadtree's per-level error
// and skirt values are maxima over the level, so any height change can move them.
void SandboxApp::applyLandscapeRect(const EditCmd& c, const std::vector<f32>& src) {
    if (!landscapeLoaded_ || src.empty()) return;
    const u32 n = landscapeData_.sampleCount;
    if (c.landX1 >= n || c.landY1 >= n) return;   // section was reloaded at a different size
    const u32 w = c.landX1 - c.landX0 + 1;
    for (u32 y = c.landY0; y <= c.landY1; ++y)
        for (u32 x = c.landX0; x <= c.landX1; ++x)
            landscapeData_.heights[y * n + x] = src[(y - c.landY0) * w + (x - c.landX0)];

    // Bounds are derived, not stored, and applyBrush is what normally recomputes them -- writing
    // samples directly bypasses that, so a stroke undone at the section's high point would leave
    // boundsMax describing terrain that no longer exists, and the quadtree's culling with it.
    landscapeData_.boundsMin[2] = landscapeData_.boundsMax[2] = landscapeData_.heights.empty() ? 0.0f
                                                                             : landscapeData_.heights[0];
    for (f32 h : landscapeData_.heights) {
        landscapeData_.boundsMin[2] = std::fmin(landscapeData_.boundsMin[2], h);
        landscapeData_.boundsMax[2] = std::fmax(landscapeData_.boundsMax[2], h);
    }

    std::string why;
    if (!landscapeTree_.build(landscapeData_, landscapeTree_.nodeQuads(), &why)) {
        AVER_ERROR("[Landscape] undo left the section unbuildable: {}", why);
        return;
    }
    landscapeTree_.widenRimSkirts(2.0f * landscapeNoiseParams_.amplitudeCm);
    landscapeTree_.resetHysteresis();

    // THE GPU HALF IS DEFERRED ONE FRAME, why this function takes no Engine: undo()/redo() are
    // reachable from places with no Engine to hand (the keybind path has one, the headless
    // self-test does not). The heightfield and quadtree update HERE, synchronously, so queries see
    // the undone state immediately; only the cached GPU meshes lag by a frame, invisibly.
    pendingLandInvalidate_ = true;
    pendingLandX0_ = c.landX0; pendingLandY0_ = c.landY0;
    pendingLandX1_ = c.landX1; pendingLandY1_ = c.landY1;
    landscapeDirty_ = true;
}

// Drains the deferred invalidation above. Called once per frame from onRender, which has the
// device that forgetOverlapping needs.
void SandboxApp::flushLandscapeInvalidate(Engine& e) {
    if (!pendingLandInvalidate_) return;
    pendingLandInvalidate_ = false;
    if (landscapeRenderer_ && e.device())
        landscapeRenderer_->forgetOverlapping(*e.device(), landscapeTree_,
                                              pendingLandX0_, pendingLandY0_,
                                              pendingLandX1_, pendingLandY1_);
}

// Replaces every height in the section with the noise parameters the panel is showing.
// ONE UNDO ENTRY covering the whole section, using the same LandscapeStroke machinery a brush
// stroke uses -- the difference between a generator you dare experiment with and one you only run
// on an empty level.
// terrainHeightAt() existed all along and was reachable from nowhere: only wired as the height
// source for the ring tiles past a section's rim, with no way to ask for it inside.
void SandboxApp::generateLandscapeNoise(Engine& e) {
    (void)e;
    if (!landscapeLoaded_ || landscapeData_.sampleCount == 0) return;
    const u32 n = landscapeData_.sampleCount;

    EditCmd c;
    c.kind = EditCmd::Kind::LandscapeStroke;
    c.label = "Generate terrain";
    c.landX0 = 0; c.landY0 = 0; c.landX1 = n - 1; c.landY1 = n - 1;
    c.landBefore = landscapeData_.heights;

    for (u32 y = 0; y < n; ++y) {
        for (u32 x = 0; x < n; ++x) {
            const f32 wx = landscapeData_.originCm[0] + static_cast<f32>(x) * landscapeData_.spacingCm;
            const f32 wy = landscapeData_.originCm[1] + static_cast<f32>(y) * landscapeData_.spacingCm;
            landscapeData_.heights[static_cast<size_t>(y) * n + x] =
                landscape::terrainHeightAt(wx, wy, landscapeNoiseParams_);
        }
    }
    c.landAfter = landscapeData_.heights;
    if (c.landBefore == c.landAfter) return;

    // applyLandscapeRect does the bounds recompute, the tree rebuild and the deferred GPU
    // invalidation. Called with the values just written so the one code path handles generate,
    // undo and redo identically rather than three near-copies drifting apart.
    applyLandscapeRect(c, c.landAfter);
    pushEdit(std::move(c));
    AVER_INFO("[Landscape] generated {}x{} samples from noise (seed {}, {} octaves)",
              n, n, landscapeNoiseParams_.seed, landscapeNoiseParams_.octaves);
}

// Starts a stroke: remembers that nothing has been touched yet. The BEFORE samples are captured
// lazily as the rect grows (see growSculptStroke) rather than up front, because at stroke start
// the rect is not known -- a drag can wander anywhere.
void SandboxApp::beginSculptStroke() {
    sculpting_ = true;
    strokeActive_ = true;
    strokeEmpty_ = true;
    strokeBefore_.clear();
}

// Unions this tick's touched rect into the stroke's, capturing the pre-stroke heights of anything
// newly covered.
// THE ORDER MATTERS: this must run BEFORE applyBrush writes, or the "before" it captures is
// already the "after". brushRect() -- previously computed only inside applyBrush and unused by the editor -- is now called by the editor directly.
void SandboxApp::growSculptStroke(const landscape::BrushRect& r) {
    if (r.empty || !strokeActive_) return;
    const u32 n = landscapeData_.sampleCount;
    u32 nx0 = r.x0, ny0 = r.y0, nx1 = r.x1, ny1 = r.y1;
    if (!strokeEmpty_) {
        nx0 = std::min(nx0, strokeX0_); ny0 = std::min(ny0, strokeY0_);
        nx1 = std::max(nx1, strokeX1_); ny1 = std::max(ny1, strokeY1_);
    }
    if (nx1 >= n || ny1 >= n) return;

    // The rect grew, so the captured buffer has to be rebuilt at the new size. Samples already
    // inside the old rect keep their ORIGINAL pre-stroke value -- copied across from the old
    // buffer, not re-read from the section, which by now holds painted values.
    const u32 nw = nx1 - nx0 + 1, nh = ny1 - ny0 + 1;
    std::vector<f32> grown(static_cast<size_t>(nw) * nh);
    for (u32 y = ny0; y <= ny1; ++y) {
        for (u32 x = nx0; x <= nx1; ++x) {
            const bool inOld = !strokeEmpty_ && x >= strokeX0_ && x <= strokeX1_ &&
                               y >= strokeY0_ && y <= strokeY1_;
            grown[(y - ny0) * nw + (x - nx0)] =
                inOld ? strokeBefore_[(y - strokeY0_) * (strokeX1_ - strokeX0_ + 1) + (x - strokeX0_)]
                      : landscapeData_.heights[y * n + x];
        }
    }
    strokeBefore_ = std::move(grown);
    strokeX0_ = nx0; strokeY0_ = ny0; strokeX1_ = nx1; strokeY1_ = ny1;
    strokeEmpty_ = false;
}

// Ends a stroke and pushes ONE undo entry for the whole thing.
void SandboxApp::endSculptStroke() {
    sculpting_ = false;
    if (!strokeActive_) return;
    strokeActive_ = false;
    if (strokeEmpty_ || !landscapeLoaded_) { strokeBefore_.clear(); return; }

    const u32 n = landscapeData_.sampleCount;
    const u32 w = strokeX1_ - strokeX0_ + 1, h = strokeY1_ - strokeY0_ + 1;
    EditCmd c;
    c.kind   = EditCmd::Kind::LandscapeStroke;
    c.label  = "Sculpt";
    c.landX0 = strokeX0_; c.landY0 = strokeY0_; c.landX1 = strokeX1_; c.landY1 = strokeY1_;
    c.landBefore = std::move(strokeBefore_);
    c.landAfter.resize(static_cast<size_t>(w) * h);
    for (u32 y = strokeY0_; y <= strokeY1_; ++y)
        for (u32 x = strokeX0_; x <= strokeX1_; ++x)
            c.landAfter[(y - strokeY0_) * w + (x - strokeX0_)] = landscapeData_.heights[y * n + x];

    // A stroke that changed nothing -- clicking on terrain already at the flatten target, or a
    // Smooth pass over a plane -- pushes no entry. Otherwise every stray click would cost the
    // user a Ctrl+Z that appears to do nothing.
    if (c.landBefore == c.landAfter) { strokeBefore_.clear(); return; }
    pushEdit(std::move(c));
    strokeBefore_.clear();
}

// Fills the foliage palette from every .ocfoliage TYPE ASSET under the project's content folder --
// NOT one entry per loaded mesh, unlike before this format existed (see FoliageSpecies' own
// comment). A type whose meshPath does not resolve in sceneMeshes_ is skipped with a warning
// rather than added: loadProjectMeshes' own reason for keying its discovery off already-resolved
// meshes (not a raw filesystem walk) applies here one layer up -- a type naming a mesh that
// failed to load, or was never imported, cannot be placed without a synchronous reload, so
// offering it would be a palette entry that silently does nothing when picked.
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
        if (sceneMeshes_.find(meshId) == sceneMeshes_.end()) {
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
    // ONE SPECIES TICKED, not all of them: a project with many types would otherwise open with a
    // brush painting a uniform random mix of every asset -- boulders, ferns and tree trunks
    // together -- never what anyone wants and many clicks to undo. Starting from one is the right
    // direction.
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
// The foliage brush: scatter meshes across the terrain under the cursor, or erase them.
// WHAT THIS DELIBERATELY IS NOT: the world module's procedural scatter (world::ScatterPalette /
// GeneratedChunkSource), which is density-field driven and authored as SCATTER records -- "cover
// this whole region by rule". This answers "put some here, by hand"; the two are complementary.
// Placements here are ordinary scene entities: they save with the level, select, move and undo.
void SandboxApp::handleFoliage(Engine& e, const ImGuiIO& io, bool overScene, f32 mx, f32 my) {
    sculptCursorValid_ = false;
    if (!landscapeLoaded_ || foliagePalette_.empty()) { foliageStroking_ = false; return; }

    Vec3 ro, rd;
    viewportRay(mx, my, ro, rd);
    const f32 roA[3] = {ro.x, ro.y, ro.z}, rdA[3] = {rd.x, rd.y, rd.z};
    landscape::HeightfieldHit hit;
    const bool haveHit = overScene && landscape::raycastHeightfield(landscapeData_, roA, rdA, hit);
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

// One placement attempt inside the brush disc. Rejected if it lands too close to something
// already there (per the picked species' OWN collisionRadiusCm), which is what stops a held
// brush from stacking meshes in a single spot.
//
// NOT SPLIT INTO A SEPARATE "pick a species" HELPER, deliberately: a member function's own
// signature is evaluated at its point of declaration, not deferred into the class's later
// complete-class context the way a function BODY is -- so a helper returning `FoliageSpecies*`
// declared up here (before FoliageSpecies itself, far below in the AVER_MODULE_LANDSCAPE member
// block) would not compile. Everything referencing the type stays inside a function BODY instead,
// exactly like the rest of this class already relies on for the identical reason.
void SandboxApp::foliagePlaceOne(Engine& e, f32 cx, f32 cy) {
    std::vector<const FoliageSpecies*> live;
    for (const auto& sp : foliagePalette_) if (sp.enabled) live.push_back(&sp);
    if (live.empty()) return;

    // Uniform over the DISC, not the square: sqrt on the radius is what keeps a brush from
    // clumping everything toward the centre.
    const f32 ang = foliageRand(foliageSeed_) * 6.2831853f;
    const f32 rad = std::sqrt(foliageRand(foliageSeed_)) * foliageRadiusCm_;
    const f32 x = cx + std::cos(ang) * rad;
    const f32 y = cy + std::sin(ang) * rad;

    f32 z = 0.0f;
    if (!landscape::surfaceHeightAt(landscapeData_, x, y, z)) return;   // off the section

    // WHICH SPECIES, weighted by its own type.weight -- mirroring
    // aver::world::ChunkGenerator.cpp's pickSpecies, minus the density-band test that has no
    // meaning here (see OcFoliage.hpp's own comment on why this format carries no density band
    // at all). A species with weight <= 0 is never picked, matching ScatterSpecies' documented
    // rule exactly; if EVERY live species happens to have weight <= 0 (a freshly zeroed
    // palette), this falls back to a uniform pick rather than placing nothing -- ticking a
    // species should never silently stop it from ever being chosen. Decided before the
    // collision test below: collisionRadiusCm and the scale it is multiplied against are both
    // per-type now, so which species this attempt is testing has to be known first, unlike the
    // single global spacing value this replaced.
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
    if (sceneMeshes_.find(meshId) == sceneMeshes_.end()) return;

    const f32 sc = sp.type.scaleMin +
                   foliageRand(foliageSeed_) * (sp.type.scaleMax - sp.type.scaleMin);

    // Interpenetration check, mirroring aver::world::ChunkGenerator.cpp's placedSolid exactly:
    // SUMMED radii (this instance's own, scaled, plus whatever the neighbour was placed with),
    // and a neighbour that was placed with collisionRadiusCm == 0 (grass and other
    // overlap-tolerant fill) never blocks anything and is never itself blocked by it -- it simply
    // never entered the check at all, on either side.
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
    // "Align to slope" is now authored PER TYPE (sp.type.alignToNormal), not a single checkbox
    // for the whole palette -- foliagePlacementRotation (FoliageAlign.hpp) is unchanged, only
    // where its `alignToNormal` argument comes from. Yaw stays a random azimuth unless the type
    // says otherwise (randomizeYaw == false means every instance faces the same way). eps of
    // 10 cm sits comfortably inside a section's own sample spacing (see OcLandData::spacingCm's
    // typical range), so the four extra probes stay local to this instance's own patch of ground
    // rather than blurring across several samples.
    const f32 yaw = sp.type.randomizeYaw ? foliageRand(foliageSeed_) * 6.2831853f : 0.0f;
    xf.rotation = editor::foliagePlacementRotation(landscapeData_, x, y, /*epsCm=*/10.0f, yaw,
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
        // MATERIAL OVERRIDE, empty means "the mesh's own cooked material" -- OcFoliageData's own
        // documented default, and previously not honoured at all: the old ad hoc FoliageSpecies
        // carried no material field, so a painted instance could never be anything but whatever
        // the mesh itself cooked with. Same resolve-and-bind pair LevelInstance.cpp uses for an
        // .ocworld PLACE's own `material` field.
        if (!sp.type.material.empty()) {
            mr->material = aver_scene_material(0, sp.type.material.c_str());
#if AVER_MODULE_PBR
            if (mr->material) {
                const pbr::MaterialHandle h = materialForSurface(sp.type.material);
                if (h) surfaceMaterials_[mr->material] = h;
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

// Removes painted instances within the brush. Only touches entities this brush placed, tracked
// in foliagePlaced_, so an erase pass cannot delete level geometry that merely happens to be
// under the cursor.
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
// Re-finds the Player Start by NAME, which is what makes an entity one (see makePlayerStart).
//
// WHY IT HAS TO BE RE-DERIVED. playerStart_ is a cached handle, and three things invalidate it
// without going anywhere near this cache: destroying the marker, undoing that destroy (which
// recreates the entity with a DIFFERENT handle), and redoing it again. The stale handle then
// either points at nothing -- Add > Player Start refuses to add a second one because it thinks
// one exists -- or, worse, at whatever entity id got reused, which the viewport then draws a
// spawn icon on. Cheap: one pass over the level's own entities, only on undo/redo and delete.
//
// FIXED (was a real bug, not cosmetic): both comparisons here used to be
// `w.name(x) == "PlayerStart"`. World::name returns `const char*`, and comparing that against a
// string literal with `==` compares POINTERS, not characters -- a heap-owned name buffer never
// lives at a string literal's address, so neither comparison could ever match. The early-out at
// the top never fired and the loop below never found anything, so playerStart_ was silently reset
// to kInvalidEntity on EVERY undo, redo and delete: precisely the failure this function exists to
// prevent, per the paragraph above. Now std::string_view, which compares content.
//
// DELIBERATELY NOT world::find("PlayerStart"): that runs a linear scan over EVERY live entity in
// the one process-global World, not just this level's own levelEntities_ -- a broader scope than
// the walk below is deliberately restricted to (levelEntities_ is the level-ownership boundary
// isLevelOwned() itself is keyed on). A same-named entity that is not part of this level would
// make find() return the wrong handle with nothing to signal the mismatch; the loop below cannot
// make that mistake because it only ever looks at entities this level itself owns.
void SandboxApp::refreshPlayerStart() {
#if AVER_MODULE_SCENE
    // The decision itself lives in editor::refreshPlayerStart (PlayerStartRefresh.hpp) -- pulled
    // out as a free function over plain scene::World + std::vector<Entity> so a headless test can
    // exercise it against a real scene::World. See that header for the pointer-comparison bug
    // this used to hide.
    playerStart_ = editor::refreshPlayerStart(scene::World::instance(), playerStart_, levelEntities_);
#endif
}

#endif

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
        // TALLER THAN THE CUBE IT BOUNDS, ON PURPOSE, ONLY IN +Z: this box is what ray picking
        // tests, and the marker no longer draws as this cube -- ViewportIconRenderer's pin has its
        // TIP at the origin and its head ~90cm above, so a box centred on the origin would leave
        // the top half unclickable while making floor beneath it clickable instead.
        // Still reaches -1 rather than 0 so it also contains the fallback CUBE (used when the icon
        // renderer or PNG is unavailable); one box serves both looks.
        mr->aabbMin[2] = -1.0f;
        mr->aabbMax[2] =  1.8f;
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
    // Faces the way the camera is facing, which is what someone placing a spawn point means by
    // "the player starts here": atan2 of the forward vector, in the same +X-forward/+Y-right
    // frame the level format's yaw is authored in.
    const Vec3 f = camForward();
    const f32 yaw = degrees(std::atan2(f.y, f.x));
    playerStart_ = makePlayerStart(at, yaw);
    if (playerStart_ == scene::kInvalidEntity) return;
    sel_ = kSelScene; selEntity_ = playerStart_;
    // AN UNDO ENTRY, WHICH THIS ALONE AMONG THE Add ITEMS DID NOT PUSH. spawnPrimitive and
    // spawnFromAssetDrop both end with describeEntity()+pushEdit(); this did not, so Ctrl+Z
    // after adding a Player Start did not remove it -- it silently undid whatever edit came
    // BEFORE, which is worse than doing nothing.
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

// Spawns a cube in front of the camera, in whichever world owns the viewport, and selects it.
// Adds one of the engine's built-in primitives. Cube and sphere are both synthesised at
// startup (appendBox/appendSphere) and registered in sceneMeshes_/meshBounds_/meshTris_ under
// their asset ids, so "Add > Sphere" needed no new asset, no new loader and no new bounds --
// only for this function to stop hardcoding the cube. It was disabled in the menu for as long
// as sphere.ocmesh had been a registered built-in.
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
    // The placeholder world (no level loaded), which keeps its OWN cube mesh at
    // kEditorCubeHalf rather than the unit one sceneMeshes_ registers.
    //
    // THE TWO CUBES ARE DIFFERENT SIZES, which is the trap here. cubeMesh_ is appendBox at
    // half-extent 50 and is drawn at scale 1; sceneMeshes_["Meshes/cube.ocmesh"] is the UNIT
    // cube, because .ocworld PLACEG scales are half-extents in cm applied to a unit mesh.
    // Looking the cube up in sceneMeshes_ here would silently shrink the placeholder cube 50x.
    // So the cube keeps its own handle, and anything else comes from the registry scaled up to
    // match it.
    rhi::MeshHandle mesh = cubeMesh_;
    u32 tris = cubeTris_;
    Vec3 scale{1, 1, 1};
    const u64 assetId = fnv1a64(std::string_view(assetPath));
    if (assetId != fnv1a64(std::string_view("Meshes/cube.ocmesh"))) {
        const auto meshIt = sceneMeshes_.find(assetId);
        if (meshIt == sceneMeshes_.end()) {
            AVER_WARN("[Editor] Add: no built-in mesh registered for '{}'", assetPath);
            return;
        }
        mesh = meshIt->second;
        const auto trisIt = meshTris_.find(assetId);
        tris = trisIt != meshTris_.end() ? trisIt->second : 0;
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
    // Pushes a CreateObj undo entry -- this branch used to push NOTHING, so adding a placeholder
    // cube (the default path with no level loaded) was silently non-undoable. Mirrors the scene
    // branch's own describeEntity()+pushEdit() tail, minus the EditId/World indirection only the scene entity needs.
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
// Finds a finite world point to drop an asset at, from a screen-space mouse position. Order:
// nearest scene-entity hit, else the ground plane, else a fixed distance along the ray from the
// camera. Z is up in this engine (see averAtmoCamAlt()), so the ground plane is Z = 0, not Y = 0.
// The world point under the cursor for a drop, and whether it is ON SOMETHING.
//
// `onSurface` is what tells the caller to rest the new object on what it hit rather than leave
// its origin buried in it -- see restOnSurface. The ground-plane and in-front-of-camera
// fallbacks report false: there is nothing there to sit on, and a mesh authored around its own
// middle should not float half its height above an empty floor just because it was dropped at
// one. Z = 0 IS a surface in the sense that matters, so the ground case reports true; only the
// "ray points at the sky" fallback does not.
Vec3 SandboxApp::dropWorldPoint(f32 screenX, f32 screenY, bool* onSurface) const {
    if (onSurface) *onSurface = true;
    Vec3 ro, rd;
    viewportRay(screenX, screenY, ro, rd);

    f32 bestT = 1e30f; bool hit = false;

    // NO TEST AGAINST THE PLACEHOLDER Floor/Cube, and that is a statement rather than an omission.
    // They are on screen only while hideEditorScene_ is false, which is now exactly "no level is
    // open" -- and spawnFromAssetDrop refuses a drop in that state. A loop over them here could
    // never run. Written down because the obvious next edit is to add one.

    // THE SCENE ENTITIES, ALWAYS -- and this guard being here was the whole bug.
    //
    // This loop used to sit inside `if (!hideEditorScene_)`, and hideEditorScene_ is true exactly
    // WHEN THE LEVEL HAS ENTITIES. So the drop ray tested the level's objects only while the level
    // had none: the moment there was anything to land on, every object was ignored and the drop
    // fell through to the ground plane at Z = 0. Aim at the top of a crate, get the floor.
    //
    // pick() one screen away has had it right the whole time -- placeholders under the visibility
    // guard, scene entities unconditional -- which is what this now mirrors. Two ray loops over the
    // same world that disagreed about which objects exist.
    {
        scene::World& w = scene::World::instance();
        const u32 n = w.count();
        for (u32 i = 0; i < n; ++i) {
            const scene::Entity ent = w.at(i);
            if (!w.valid(ent) || w.destroyPending(ent)) continue;
            const auto* mr = w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
            if (!mr || !(mr->flags & scene::kMeshRendererVisible) || mr->mesh == 0) continue;
            if (sceneMeshes_.find(mr->mesh) == sceneMeshes_.end()) continue;
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
        // A drop-to-place entry point for DECIDED 3's format, mirroring the .ocmesh path below
        // rather than growing its own copy of the drop-target/level-active checks. Loaded directly
        // (not through loadProjectParticleEffects' whole-tree walk) so an effect just authored
        // resolves immediately, the same reasoning the mesh path gives for reloading synchronously.
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
    if (sceneMeshes_.find(meshId) == sceneMeshes_.end()) {
        // Not loaded yet -- most likely imported moments ago. Reload synchronously (the same
        // release+load pair buildUI() runs for wantMeshReload_) rather than waiting a frame, so
        // the drop the user just made actually lands.
        releaseProjectMeshes(e);
        loadProjectMeshes(e);
    }
    const auto meshIt = sceneMeshes_.find(meshId);
    if (meshIt == sceneMeshes_.end()) {
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

    // ON TOP OF WHAT IT LANDED ON, not inside it. Applied BEFORE the snap so the snap still
    // quantises the final position rather than a value the lift then knocks off the grid.
    // A MESH WITH NO KNOWN BOUNDS IS LEFT WHERE IT LANDED. meshBounds_ is filled at load from
    // the .ocmesh's own header, so a miss means nothing measured this mesh -- and inventing a
    // lift for it would move the object for a reason nobody could see.
    f32 lift = 0.0f;
    if (onSurface) {
        if (const auto bit = meshBounds_.find(meshId); bit != meshBounds_.end())
            lift = editor::dropRestLift(bit->second.first.z, xf.scale.z);
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

// The gizmo's three axes IN WORLD SPACE, for the current tool and the World/Local button.
// SCALE IS ALWAYS LOCAL, whatever the button says: applyScale writes o.scale.x/y/z, the object's
// own axes, so drawing that handle along a world axis on a rotated object meant the arrow you
// dragged and the number that changed pointed different ways. UE hides the world option here too.
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
    // Grid snap stays in WORLD space even for a local-axis drag. A grid the object is not
    // aligned to is still the grid the level is built on, and snapping to the object's own
    // rotated lattice would put nothing on round numbers.
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

// Rotates the transform about the active gizmo axis by the swept cursor angle.
// BY QUATERNION COMPOSITION, not by adding to an Euler component: `rotDeg[axis] += angle` is only
// correct when the other two components are zero, so on an already-rotated object it produced a
// rotation about the wrong axis.
// The order is `dq * q`: this Quat's operator* is the Hamilton product, so composing "q first,
// then dq" is dq on the LEFT -- backwards rotates about the object's axes instead of the world's,
// agreeing only at identity.
void SandboxApp::applyRotate(EditXform& o, f32 px, f32 py, f32 mx, f32 my) {
    f32 ox, oy; if (!project(o.pos, ox, oy)) return;
    const f32 a0=std::atan2(py-oy, px-ox), a1=std::atan2(my-oy, mx-ox);
    f32 da=a1-a0; while (da> kPi) da-=kTwoPi; while (da< -kPi) da+=kTwoPi;

    Vec3 ax[3]; gizmoBasis(o, ax);
    const Vec3 A = ax[activeAxis_];
    // Which way the ring turns on screen depends on which side of it the camera is.
    const f32 sgn = dot(A, camForward()) >= 0.0f ? -1.0f : 1.0f;

    rotDragDeg_ += degrees(da) * sgn;
    // SNAPPING IS ON THE ACCUMULATED ANGLE, not the per-frame delta. Snapping each frame's
    // delta would round most of them to zero and the object would never turn.
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
        // 1..4 select a tool WITHIN the active mode, so the same keys mean "the four things this
        // mode does" rather than a flat list that grows with every mode. Tab switches mode, the
        // one binding meaning the same thing in both. Both halves of the 1..4 dispatch are 4
        // commands, not 8, whose scopes never overlap since mode_ can only be one value at a time.
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
        if (landscapeLoaded_) {
        }
#endif
    }
    const f32 mx=io.MousePos.x, my=io.MousePos.y;
    const bool overScene = levelHovered_ && inViewport(mx, my);

#if AVER_MODULE_LANDSCAPE
    const bool isSculptTool = editorModeIsLandscape();
#else
    // Unused with the module off -- the isSculptTool branch below compiles out along with it --
    // but declared anyway so isXformTool's "everything that is not a sculpt tool" phrasing does
    // not need its own second definition per configuration.
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
    // Foliage owns the click the same way Landscape does: no picking, no gizmo, the brush
    // instead. It has to be tested here and not fall through to the Select branch, or painting
    // would also be selecting whatever it painted on.
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
                // DRAGGING THE OBJECT ITSELF MOVES IT, not just the three hairline axis handles.
                //
                // Reaching a gizmo handle means landing within 16px of a two-pixel line, and
                // until now that was the ONLY way to move anything: a click anywhere on the
                // object's own body fell straight through to a re-pick, so the obvious gesture --
                // grab the thing and drag -- did nothing at all and read as "the editor cannot
                // move objects".
                //
                // ALREADY-SELECTED IS THE TEST, and it is what keeps this safe. pick() is left to
                // decide what was hit, exactly as before; if it lands on what was ALREADY
                // selected, the click is a grab rather than a selection change. So the first
                // click still only selects -- nothing can be nudged by the click that selected
                // it -- and a click on empty space or on a different object keeps its old
                // meaning entirely.
                //
                // AXIS 3 is applyMove's screen-plane branch, which already existed and was
                // reachable only through a 13px invisible hotspot at the pivot. This gives it the
                // whole silhouette to be grabbed by.
                //
                // No drag threshold is needed: a click that does not move produces no transform
                // change, and endTransformEdit already drops a no-op edit (nearlySameXform).
                const int  prevSel = sel_;
#if AVER_MODULE_SCENE
                const scene::Entity prevEnt = selEntity_;
#endif
                const bool hadSel = anySelected();
                pick(e, io);
                const bool sameTarget = hadSel && anySelected() && sel_ == prevSel
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
                // MARQUEE ARMS ON A MISS, Select tool only (the brief above is about picking an
                // OBJECT; Move/Rotate/Scale keep their old "click empty space, deselect" meaning
                // unchanged). pick() just cleared the selection above (or left it cleared under
                // Ctrl -- see pick()'s own tail), so arming here costs a click on empty space
                // nothing it did not already do; a release with no further movement below the
                // threshold reads as that exact same click, per marqueeActive_ staying false.
                else if (tool_ == Tool::Select && !anySelected()) {
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
        // MARQUEE: continues while armed, becomes ACTIVE (and starts drawing) once the drag clears
        // the threshold, and commits on release -- all independent of `overScene` above, since a
        // drag that started inside the viewport must keep tracking even if the cursor strays past
        // its edge.
        if (marqueeArmed_) {
            marqueeX1_ = mx; marqueeY1_ = my;
            if (!marqueeActive_ &&
                editor::marqueeExceedsThreshold(marqueeX0_, marqueeY0_, marqueeX1_, marqueeY1_))
                marqueeActive_ = true;

            if (marqueeActive_) {
                f32 loX, loY, hiX, hiY;
                editor::normalizeMarqueeRect(marqueeX0_, marqueeY0_, marqueeX1_, marqueeY1_, loX, loY, hiX, hiY);
                // The selection outline's own orange (selectionOutlineLines' kSelR/G/B, above),
                // diluted for the fill so the scene underneath a drag stays readable.
                ImDrawList* dl = ImGui::GetForegroundDrawList();
                dl->AddRectFilled(ImVec2(loX, loY), ImVec2(hiX, hiY), IM_COL32(255, 158, 31, 40));
                dl->AddRect(ImVec2(loX, loY), ImVec2(hiX, hiY), IM_COL32(255, 158, 31, 220), 0.0f, 0, 1.5f*dpi_);
            }

            if (!io.MouseDown[0]) {
                if (marqueeActive_) {
                    f32 loX, loY, hiX, hiY;
                    editor::normalizeMarqueeRect(marqueeX0_, marqueeY0_, marqueeX1_, marqueeY1_, loX, loY, hiX, hiY);
                    // Every eligible entity -- pick()'s own broadphase eligibility -- whose world
                    // AABB, projected corner by corner, intersects the rectangle.
                    std::vector<scene::Entity> hitEnts;
                    scene::World& w = scene::World::instance();
                    const u32 n = w.count();
                    for (u32 i = 0; i < n; ++i) {
                        const scene::Entity ent = w.at(i);
                        if (!w.valid(ent) || w.destroyPending(ent)) continue;
                        if (anyChunkWorldOwns(ent)) continue;
                        const auto* mr = w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
                        if (!mr || !(mr->flags & scene::kMeshRendererVisible) || mr->mesh == 0) continue;
                        if (sceneMeshes_.find(mr->mesh) == sceneMeshes_.end()) continue;
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

    // THE OUTLINER AND DETAILS COUNT AS "THE LEVEL", for the edit verbs.
    //
    // This was `levelFocused_` alone, and levelFocused_ is only true while the 3D VIEWPORT holds
    // ImGui's keyboard focus. Clicking a row in the World Outliner -- the ordinary way to select
    // something, and the way that names it -- moves focus to that panel, so Delete, Copy, Paste,
    // Duplicate and even UNDO all silently stopped working. Select in the list, press Delete,
    // nothing happens, with no message: reported as "deleting is a bit broken".
    //
    // NOT a blanket "any window": a text field is excluded by WantTextInput below, and the Content
    // Browser deliberately keeps its own Delete (which deletes a FILE) -- widening this to its
    // panel would put two different destructive meanings on one key.
    // NOT WHILE A GIZMO DRAG IS IN FLIGHT, and this is a correctness gate rather than a nicety.
    //
    // beginTransformEdit captures editBefore_ from whatever is selected AT GRAB TIME, but
    // nothing pins the gesture to that entity: the drag block below reads and writes whatever
    // selectedXform() resolves to THIS frame. So pressing Ctrl+A -- or Delete, or Ctrl+V --
    // with the mouse button still held changes the selection under the gesture, and the drag
    // silently continues on a different object. The undo record endTransformEdit then pushes
    // describes an entity that was never dragged, which is corruption that an assertion about
    // the anchor would pass straight through.
    //
    // Repro before this line: two objects, select the one that is NOT first in the Outliner,
    // start dragging its gizmo, press Ctrl+A while still holding. The gizmo jumps to the other
    // object and drags that instead.
    //
    // Gating the verbs rather than pinning a dragEntity_ through the drag update and
    // endTransformEdit: a mouse button is already held on the gizmo, so a click cannot change
    // the selection either, which leaves the keyboard verbs as the whole of the hole. The
    // narrower fix is the one that cannot itself introduce a second source of truth for "what
    // is being dragged".
    if (!dragging_ && (levelFocused_ || outlinerFocused_ || detailsFocused_) && !io.WantTextInput) {
        if (keybinds_.pressed(editor::CommandId::EditDelete, io))    deleteSelection();
        if (keybinds_.pressed(editor::CommandId::EditCopy, io))      copySelection();
        if (keybinds_.pressed(editor::CommandId::EditPaste, io))     pasteClipboard();
        if (keybinds_.pressed(editor::CommandId::EditDuplicate, io)) duplicateSelection();
        // Guarded on the SAME emptiness the menu item greys itself on, so the key and the menu
        // agree about when Select All does nothing.
        if (!outlinerOrder_.empty() && keybinds_.pressed(editor::CommandId::EditSelectAll, io))
            selectAllInOutliner();
        if (keybinds_.pressed(editor::CommandId::EditUndo, io)) undo();
        // Ctrl+Shift+Z: an intentionally NOT-rebindable alternate spelling of Redo (same command,
        // not a second one) -- kept as a small hardcoded fallback next to the registry-driven
        // checks, exactly as it was hardcoded before this file existed.
        if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)) redo();
        if (keybinds_.pressed(editor::CommandId::EditRedo, io)) redo();

#if AVER_MODULE_SCENE
        if (keybinds_.pressed(editor::CommandId::SnapToFloor, io))     snapSelectionToFloor();
        if (keybinds_.pressed(editor::CommandId::HideSelected, io))    hideSelection();
        if (keybinds_.pressed(editor::CommandId::IsolateSelected, io)) isolateSelection();
        if (keybinds_.pressed(editor::CommandId::UnhideAll, io))       unhideAll();

        // NUDGE: camera-relative, snapped to whichever single world axis each direction most
        // agrees with -- moving "left" along a camera that is not axis-aligned would otherwise
        // nudge diagonally, which is not what a cardinal step means.
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
            // ONE UNDO ENTRY PER HELD RUN, not one per repeat: open it the first time a nudge key is
            // down and close it once every nudge key is back up. The run tracks ITS OWN flag rather
            // than editBeforeValid_, because a Details panel field drag holds that one open too and
            // this block would otherwise close the drag's entry on every frame of it.
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
            // THE REST OF THE SELECTION FOLLOWS, by the same world-space DELTA the anchor just
            // moved -- which is why this reads `before` rather than recomputing anything: grid
            // snap, axis constraint and the screen-plane projection have all already been applied
            // to the anchor, and re-deriving them per entity would let a snapped anchor drag an
            // unsnapped crowd, or worse, snap each one separately and change their spacing.
            //
            // MOVE ONLY. Rotate and scale about a shared pivot need a pivot to be CHOSEN (the
            // anchor? the centroid? each object's own?) and each answer is right for different
            // work; picking one silently would be a worse answer than the honest gap. They still
            // act on the anchor alone.
            if (tool_ == Tool::Move) {
                const Vec3 delta = o.pos - before;
                if (delta.x != 0.0f || delta.y != 0.0f || delta.z != 0.0f) {
                    // THROUGH THE SHARED HELPER, which also decides what beginTransformEdit
                    // recorded. The skip rule (not the anchor, not beneath another selected
                    // entity -- a child would otherwise move twice, once with its parent's
                    // transform and once on its own) used to be written out here and nowhere
                    // else; the undo record now depends on matching it exactly, so the two read
                    // from one place.
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
    if (!landscapeLoaded_) { sculpting_ = false; return; }

    Vec3 ro, rd;
    viewportRay(mx, my, ro, rd);
    const f32 roA[3] = {ro.x, ro.y, ro.z}, rdA[3] = {rd.x, rd.y, rd.z};
    landscape::HeightfieldHit hit;
    const bool haveHit = overScene && landscape::raycastHeightfield(landscapeData_, roA, rdA, hit);
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
        // Noise's seed, captured once per stroke from the stroke's own start position -- NOT the
        // clock (see BrushParams::noiseSeed) -- so replaying the same drag always noises the same
        // way. fnv1a64 over the two raw floats, truncated to 32 bits: plenty of entropy for a
        // seed, and bit-for-bit reproducible across runs since it hashes the exact bit pattern
        // rather than a printed/rounded form of the position.
        const f32 seedInput[2] = {hit.posCm[0], hit.posCm[1]};
        sculptNoiseSeed_ = static_cast<u32>(fnv1a64(seedInput, sizeof(seedInput)));
    }
    // RELEASE ENDS THE STROKE AND PUSHES THE UNDO ENTRY: before this existed, sculpting was
    // completely invisible to Ctrl+Z, since handleSculpt mutated the heightfield in place and the
    // undo stack only ever knew about entity transforms -- Ctrl+Z after ten minutes of terrain work silently undid a moved object instead.
    if (!io.MouseDown[0] && strokeActive_) endSculptStroke();

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

        // dt-scaled so holding the button paints at a constant rate regardless of frame rate,
        // clamped the same way the other per-frame dt reads in this file are (see e.g. the fly
        // camera's own dt clamp) so a stall does not apply one giant, frame-skipping stroke.
        const f32 dt = std::fmin(e.time().dt, 0.05f);
        const f32 amount = std::fmin(1.0f, dt * 6.0f);

        // BEFORE applyBrush, not after: growSculptStroke captures the pre-stroke heights of any
        // samples this tick is about to newly touch, since after the write they're no longer
        // "pre". brushRect() existed all along; the editor had just never called it.
        growSculptStroke(landscape::brushRect(landscapeData_, p));

        const landscape::BrushRect touched = landscape::applyBrush(landscapeData_, p, amount);
        if (!touched.empty) {
            // REBUILDS THE WHOLE TREE, not just the touched nodes: LandscapeTree::build() has no
            // incremental form (a coarser node's errorCm/skirtCm/radius are per-level MAXIMA, so a
            // local change can in principle move any of them). Proportional to the section's total
            // sample count, not the brush footprint -- cheap here, but real on a large section. Only the GPU mesh cache invalidation below is footprint-local.
            std::string why;
            if (landscapeTree_.build(landscapeData_, landscapeTree_.nodeQuads(), &why)) {
                landscapeTree_.widenRimSkirts(2.0f * landscapeNoiseParams_.amplitudeCm);
                landscapeTree_.resetHysteresis();
                if (landscapeRenderer_)
                    landscapeRenderer_->forgetOverlapping(*e.device(), landscapeTree_,
                                                           touched.x0, touched.y0, touched.x1, touched.y1);
            } else {
                AVER_ERROR("[Landscape] sculpt left the section unbuildable: {}", why);
            }
            landscapeDirty_ = true;
        }
    }
}

#endif

// Draws the current tool's gizmo over the selection, on top of geometry.
void SandboxApp::drawGizmo(Engine& e) {
    // Anything but Move/Rotate/Scale has no gizmo -- Select has none, and neither do the sculpt
    // tools, which draw their OWN cursor via drawSculptCursor(). Spelled as the allow-list the
    // three real gizmo tools are, rather than a denylist, so a tool added later defaults to "no
    // gizmo" instead of silently inheriting the old `: gzScale_` fallback.
    // The gizmo belongs to Select mode: without this it would keep drawing over the terrain while
    // a brush was active, since tool_ still holds whatever object tool was last used.
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
    // A SLIGHT GLOW ON THE HANDLES. Lines already write into the pre-tonemap HDR target and
    // bloom runs before the tonemap, so this is real bloom, not a fake halo -- it only needed
    // headroom above 1.0. See IDevice::setLineGlow for why the multiplier applies after the
    // inverse tonemap, not to the authored hue.
    // Deliberately small: a bloom wide enough to be obvious would bury the two-pixel handle you're
    // aiming at, and axis colours are how you tell the three apart. 1.35 reads as lit, not painted.
    static constexpr f32 kGizmoGlow = 1.35f;
    e.device()->setLineGlow(kGizmoGlow);
    for (int a=0;a<3;++a) {
        const bool active = (dragging_ && a==activeAxis_) || (!dragging_ && a==hoverAxis_);
        e.device()->drawLines(active ? hi[a] : nrm[a], &w.m[0][0]);
    }
    // Back to unglowed for the grid and everything else: the setter is sticky.
    e.device()->setLineGlow(1.0f);
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
    e.device()->drawLines(brushRing_, &w.m[0][0]);
    e.device()->setLineDepth(true);
}

#endif

#if AVER_WITH_IMGUI
// Removes the selected entity or placeholder object from the world, pushing an undo entry
// either way. Pseudo-entries (sun/sky/post) are ignored -- sel_ never lands here for them.
// The objects_ branch below now pushes a DestroyObj entry; it used to push nothing, so deleting a
// placeholder object (the default path with no level loaded) was silently non-undoable.
void SandboxApp::deleteSelection() {
#if AVER_MODULE_SCENE
    // EVERY SELECTED ENTITY, not just the anchor. Selecting five rows and pressing Delete used to
    // remove one and leave four still highlighted, which is the most confusing way for a
    // multi-select to be half-finished.
    //
    // ONE UNDO RECORD PER ENTITY, honestly: EditCmd describes a single destroy, so undoing a
    // five-object delete takes five Ctrl+Z. Grouping them needs a compound command the undo stack
    // does not have, and inventing one here -- inside a delete -- is how an undo stack acquires a
    // shape nothing else understands. Stated rather than hidden.
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
            // BEFORE destroyEntity, which retires the subtree and takes the hierarchy links with
            // it -- a capture afterwards would find an empty subtree and report a complete undo
            // record it did not have.
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

// Copies the selection into the editor's own clipboard. A pure read: nothing changes in the
// world, so nothing is pushed onto the undo stack. A non-empty `entities` and `hasObject` are
// other, mirroring the existing loose pairing of sel_/selEntity_.
void SandboxApp::copySelection() {
#if AVER_MODULE_SCENE
    // THE WHOLE SELECTION, NOT JUST THE ANCHOR -- the same fix Ctrl+D got, in the same shape.
    //
    // ANCESTORS SKIPPED, and it must be DUPLICATE's version of that test, not the mover's. The
    // mover's forEachMultiMoved also excludes `ent == selEntity_`, because a drag moves the
    // anchor through the gizmo and the others relative to it. A copy has no anchor to exclude:
    // dropping it would silently omit the very entity the author clicked first. Skipping a
    // descendant of another selected entity IS still required, though -- captureSubtree already
    // takes it along inside its parent, so keeping it would paste it twice, once orphaned.
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
            ce.bodyHalf = c.bodyHalf;
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
        ce.bodyHalf = c.bodyHalf;
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

// Rebuilds the clipboard's contents in front of the camera -- the SAME convention spawnCube()
// and dropWorldPoint() use -- and pushes a FRESH Create/CreateObj entry, independent of the
// ORIGINAL copied thing's undo entry. Silently does nothing when the clipboard is empty: the
// keybind dispatch doesn't gate on clipboard state, so a no-op Paste is expected.
void SandboxApp::pasteClipboard() {
    Vec3 at = camPos_ + camForward() * kAddDistance;
    if (snapMove_) for (int k = 0; k < 3; ++k) (&at.x)[k] = snapf((&at.x)[k], moveSnap_);
#if AVER_MODULE_SCENE
    if (!clipboard_.entities.empty()) {
        // RELATIVE LAYOUT IS PRESERVED, and that is the whole reason this is not just a loop
        // pasting each item at `at`. Doing that would stack every copied entity on the same point
        // in front of the camera -- five props arranged in a row would arrive as one heap, and the
        // author would have to rebuild an arrangement they had already made. Duplicate gets this
        // for free by offsetting each copy from its own original; paste has to compute the same
        // thing, because it is moving the whole set to a NEW anchor.
        //
        // The FIRST clipboard entry lands exactly at `at` (the one-item case is then bit-identical
        // to what this did before), and every other entry keeps its offset from that first one.
        const Vec3 origin = clipboard_.entities.front().xform.pos;
        std::vector<scene::Entity> made;
        for (const ClipboardEntity& ce : clipboard_.entities) {
            EditXform x = ce.xform;
            x.pos = at + (ce.xform.pos - origin);
            const std::string label = makeEntityLabel(std::string(), ce.snap.asset);
            const scene::Entity e = spawnEntityFrom(ce.snap, x, label, ce.hadBody, ce.bodyHalf, /*restoreObjectId=*/false);
            if (e == scene::kInvalidEntity) continue;
            spawnSubtreeUnder(e, ce.subtree, /*restoreIds=*/false);
            sel_ = kSelScene; selEntity_ = e;   // spawnSubtreeUnder selects whatever it made last
            EditCmd c = describeEntity(e);
            c.kind = EditCmd::Kind::Create;
            // CAPTURED FROM THE COPY, so REDO puts the children back too. Undo of a Create destroys
            // the whole subtree (World::destroy retires it), so without this a paste-undo-redo cycle
            // returned the root alone -- the children were destroyed by the undo and never rebuilt.
            captureSubtree(c, e);
            // ONE RECORD PER PASTED ENTITY, which is Duplicate's and Delete's precedent: N undos
            // for N items. This file states outright why there is no compound kind (a compound
            // Create that recreates a whole set is a bigger change than this is worth), and
            // inventing one here would make paste the only verb that disagreed.
            pushEdit(std::move(c));
            made.push_back(e);
        }
        if (made.empty()) return;
        // The pastes become the selection, matching Duplicate: a drag straight after Ctrl+V moves
        // what was just made, which is what makes paste-then-place one motion.
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
    // THE WHOLE SELECTION, NOT JUST THE ANCHOR. This read selEntity_ alone, so Ctrl+D on five
    // selected props duplicated one and silently dropped the other four -- the same
    // "applies to the set, acts on the anchor" shape that made multi-move unundoable.
    //
    // ONE RECORD PER COPY, which is deleteSelection's precedent rather than multi-move's: N
    // undos for N duplicates. A drag is one gesture whose half-undone state the author never
    // saw; five duplicates are five objects that can reasonably be unpicked one at a time, and
    // grouping Create commands would need a compound kind that recreates a whole set, which is
    // a bigger change than this is worth.
    //
    // ANCESTORS SKIPPED for the reason the mover skips them: duplicating a parent already
    // duplicates its children through captureSubtree, so a selected child would otherwise get a
    // second, orphaned copy.
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
                spawnEntityFrom(src.snap, x, label, src.hadBody, src.bodyHalf, /*restoreObjectId=*/false);
            if (e == scene::kInvalidEntity) continue;
            spawnSubtreeUnder(e, src.subtree, /*restoreIds=*/false);
            EditCmd c = describeEntity(e);
            c.kind = EditCmd::Kind::Create;
            captureSubtree(c, e);
            pushEdit(std::move(c));
            made.push_back(e);
        }
        if (made.empty()) return;
        // The copies become the selection, so a drag straight after Ctrl+D moves what was just
        // made -- which is what every editor does and what makes duplicate-then-place one motion.
        multiSetSingle(made.front());
        for (usize i = 1; i < made.size(); ++i) multiToggle(made[i]);
        sel_ = kSelScene; selEntity_ = made.front();
        AVER_INFO("[Editor] duplicated {} entities", made.size());
        return;
    }
    if (sel_ == kSelScene && selEntity_ != scene::kInvalidEntity && scene::World::instance().valid(selEntity_)) {
        EditCmd src = describeEntity(selEntity_);
        // The source's descendants, read BEFORE anything is spawned -- the copy is about to
        // become the selection, and captureSubtree walks whatever it is handed.
        captureSubtree(src, selEntity_);
        EditXform x = src.after;
        x.pos.x += delta; x.pos.y += delta;
        const std::string label = makeEntityLabel(std::string(), src.snap.asset);
        const scene::Entity e = spawnEntityFrom(src.snap, x, label, src.hadBody, src.bodyHalf, /*restoreObjectId=*/false);
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
// pick() and the asset drag-drop drop point so there is exactly one screen->ray conversion.
// BOTH ENDS ARE UNPROJECTED, and the near one is not decoration.
//
// This used to read `ro = eye_`, which is right only because a perspective frustum has one. It
// is the SINGLE assumption in the editor's screen-to-world conversion that a projection matrix
// has a centre of projection at all, and it is wrong the moment one does not: under an
// orthographic view every ray is PARALLEL and starts on the near plane, so a fan converging on
// a camera point tens of thousands of centimetres away picks along a line that is nowhere near
// the pixel. Every caller inherits it -- pick, handleSculpt, handleFoliage, dropWorldPoint.
//
// Unprojecting ndc.z = 0 instead is CORRECT TODAY, under perspective, which is why it lands on
// its own ahead of any ortho work: the origin moves from the eye to the near plane 2 cm in
// front of it, along the same ray, so every hit point ro + rd*t is unchanged and only the
// parameterisation shifts. `pick()` passes an unnormalised rd to rayAabb and compares t between
// objects, so a uniform change of scale within one call reorders nothing.
//
// The two expressions differ only in the iv.m[2][*] term, which carries ndc.z: present for the
// far plane at z = 1, absent for the near plane at z = 0. The depth range is [0,1], not
// [-1,1] -- see Mat4::perspectiveLH.
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
// Lazy, memoized PickGeometry for a PROJECT mesh (the built-ins are seeded at creation, :2113-2142,
// and never reach the loading branch below). Mirrors selectionOutlineLines' own reason for reading
// a .ocmesh a second time: loadProjectMeshes uploads to the GPU and lets OcMeshData go, so a mesh's
// positions/normals/indices are not in memory for pick() to test a ray against. A pick is a click,
// not a frame -- this costs one file read the first time a click ray reaches a given mesh, and
// (like selectionOutlineLines' cache) nothing on any later click near it, success OR failure: an
// id mapped to an empty PickGeometry IS the cached "unavailable, use the bounds" answer, so a
// missing path or a load failure is warned about once, not on every subsequent click.
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
// THE NEAREST TRIANGLE WINS, DOUBLE-SIDED, because ray-driven primary visibility (the editor's
// default render mode) traces with no cull flag and draws back faces too -- a click has to be able
// to hit what the camera can actually see. The one exception is an entity's OWN back faces when the
// ray starts inside ITS bounds (insideBox below, the ab3bca81 case): those are the inside walls of
// whatever the camera is standing in, so a closed shape (a rock, a crate) cannot be selected from
// its own inside. An enclosing SHELL (NewSponza's building) stays selectable from inside it: its
// interior walls are authored facing INTO the room, so from in there they are front faces.
//
// Testing triangles rather than bounds also means a nearer wall now BLOCKS an object behind it,
// which bounds-only picking never did: previously, clicking a wall could still select a prop
// standing behind it whenever the prop's own box happened to win the t comparison.
//
// A skinned/posed entity (its drawn vertices move in a compute pass this ray never runs against)
// and any mesh whose triangles are not resident -- pickGeometryFor came back empty: an unresolved
// id, or a project .ocmesh that would not load -- both fall back to the bounding-box test instead,
// keeping the ORIGINAL ab3bca81 rule: t > 0.0f, so an object whose bounds enclose the camera is not
// auto-selected by every click (it stays reachable from the Outliner).
void SandboxApp::pick(Engine& e, const ImGuiIO& io) {
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

    // AvId, not scene::Entity: pick() spans both the placeholder and scene worlds, so bestEnt is
    // read and compared unguarded below even though only the loop that can set it away from
    // "nothing" is scene-only.
    AvId bestEnt = kInvalidId;
#if AVER_MODULE_SCENE
    {
        // (a) BROADPHASE: every eligible entity whose local-space box the ray actually enters,
        // with the box's own hit distance tBox and whether the ray started inside it (insideBox).
        // No selection decision is made here -- an entry is a CANDIDATE for the triangle test
        // below, not a pick.
        struct PickCandidate { scene::Entity ent; u64 meshId; f32 tBox; bool insideBox; Vec3 lo, ld; };
        std::vector<PickCandidate> candidates;
        scene::World& w = scene::World::instance();
        const u32 n = w.count();
        for (u32 i = 0; i < n; ++i) {
            const scene::Entity ent = w.at(i);
            if (!w.valid(ent) || w.destroyPending(ent)) continue;
            // Streamed entities are not selectable: selection is the only door into the
            // gizmo/EditCmd path, and an entity the streamer can evict out from under an in-flight
            // edit must never go through that door (see setChunkStreamingEnabled and buildPanels).
            if (anyChunkWorldOwns(ent)) continue;
            const auto* mr = w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
            if (!mr || !(mr->flags & scene::kMeshRendererVisible) || mr->mesh == 0) continue;
            if (sceneMeshes_.find(mr->mesh) == sceneMeshes_.end()) continue;
            Vec3 lmin{mr->aabbMin[0], mr->aabbMin[1], mr->aabbMin[2]};
            Vec3 lmax{mr->aabbMax[0], mr->aabbMax[1], mr->aabbMax[2]};
            if (!(lmax.x > lmin.x && lmax.y > lmin.y && lmax.z > lmin.z)) { lmin = Vec3{-1,-1,-1}; lmax = Vec3{1,1,1}; }
            const Mat4 iw = w.worldMatrix(ent).inverse();
            const Vec3 lo = xformPoint(iw, ro), ld = xformVec(iw, rd);
            f32 tBox;
            if (!rayAabb(lo, ld, lmin, lmax, tBox)) continue;
            candidates.push_back({ent, mr->mesh, tBox, tBox <= 0.0f, lo, ld});
        }

        // (b) NEAREST BOX FIRST, then stop as soon as a candidate's own tBox can no longer beat
        // bestT: rayAabb's tBox is a lower bound on any triangle hit inside that same box (a
        // triangle cannot be nearer than the box that contains it), so every candidate after that
        // point is provably farther than the best hit already found -- a real early-out, not a
        // heuristic.
        std::sort(candidates.begin(), candidates.end(),
                  [](const PickCandidate& a, const PickCandidate& b) { return a.tBox < b.tBox; });

        for (const PickCandidate& c : candidates) {
            if (c.tBox >= bestT) break;
            // (c) A skinned/posed entity's resting geometry is not what is actually drawn (its
            // vertices move in a compute pass this ray was never run against), so -- like a mesh
            // whose triangles are not resident -- it keeps the bounds-only rule instead.
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
    // CTRL-CLICK EXTENDS THE SELECTION, and without this the viewport DESTROYED one.
    //
    // A bare assignment to selEntity_ is what multiStale() watches for -- an anchor landing
    // outside the set means "this one thing now", which is right for a plain click and wrong for
    // every modified one. So multi-select was reachable only by clicking rows in the Outliner,
    // and a single click anywhere in the 3D view silently collapsed it. Selecting five props in
    // the tree and then clicking one of them in the viewport to check it dropped the other four.
    //
    // ONLY CTRL, NOT SHIFT. Shift-range needs a defined ORDER to range across, which the outliner
    // has (outlinerOrder_, what multiRange walks) and a 3D view does not -- "every entity between
    // these two" is not a question a viewport can answer. Adding it here would mean inventing an
    // order, and an order the user cannot see is worse than no gesture.
#if AVER_MODULE_SCENE
    const bool extend = ImGui::GetIO().KeyCtrl;
    if (extend && bestEnt != kInvalidId) {
        multiToggle(static_cast<scene::Entity>(bestEnt));
        return;
    }
#endif
    if (bestEnt != kInvalidId) { sel_ = kSelScene; selEntity_ = bestEnt; }
    else                       { sel_ = best;     selEntity_ = kInvalidId; }
}

#endif

#if AVER_MODULE_SCENE
// ---- VIEWPORT PLACEMENT VERBS (2026-09-16) -----------------------------------------------------
// End/H/Shift+H/Ctrl+H/arrows/PageUp/PageDown, dispatched from handleManip's edit-verb block.

// The union of every selected entity's world-space bounds, for F (Frame Selected) to fit the WHOLE
// set rather than just the anchor selectedXform/selectedRadius describe alone.
//
// EACH ENTITY'S OWN BOX is built the same way selectedRadius builds one for the anchor: its local
// CMeshRenderer extent times the entity's OWN world scale, centred on its OWN world position -- not
// a properly rotated world AABB. A rotated mesh's true world bounds run wider than this along axes
// its local bounds do not already cover; selectedRadius already makes that exact trade for a single
// object, and a more exact box here would let "frame this one object" and "frame a five-object
// selection that happens to include it" size the same object two different ways for no reason a
// user could see.
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
        // A zero (or missing) extent falls back to the scale itself, exactly as selectedRadius does
        // for the identical reason: bounds that were never filled in must not frame as a point.
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
// A STRAIGHT-DOWN RAY, not a full pick()-style click ray -- +Z is up in this engine (Math.hpp's own
// top comment, and dropRestLift's "along world +Z"), so "the floor" is unambiguously -Z from each
// entity's own position. Reuses pick()'s own two-world broadphase (placeholder boxes,
// then scene entities sorted nearest-box-first with rayPickGeometry/pickGeometryFor refining each
// candidate) so a selected object rests on exactly what a click on it would have hit, minus the
// landscape height query neither pick() nor dropWorldPoint ever learned (see this function's own
// use of surfaceHeightAt -- a straight-down query, not raycastHeightfield's march, because the ray
// already IS straight down).
//
// EVERYTHING ELSE IN THE SELECTION IS INELIGIBLE GROUND: an object must not land on top of another
// that this same command is about to move (or already has), which is why each entity's own ray
// skips every entity in `sel`, not just itself.
void SandboxApp::snapSelectionToFloor() {
    const std::vector<scene::Entity> sel = selectedEntities();
    if (sel.empty()) return;
    scene::World& w = scene::World::instance();

    // ONE UNDO ENTRY FOR THE WHOLE OPERATION: beginTransformEdit captures the anchor's transform and
    // (via forEachMultiMoved) every other selected entity's LOCAL transform before anything below
    // writes to them; endTransformEdit reads back wherever they ended up and pushes one Transform
    // command covering every entity that actually moved.
    if (!beginTransformEdit()) return;

    for (const scene::Entity e : sel) {
        if (!w.valid(e)) continue;
        const Transform before = worldTransformOf(w, e);
        const auto* selfMr = w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);

        // The ray starts clear of the entity's OWN top, not at its pivot -- starting inside a tall
        // mesh could report a hit on its own back faces (or, worse, on itself if it were not already
        // excluded below).
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
        if (landscapeLoaded_) {
            f32 z;
            if (landscape::surfaceHeightAt(landscapeData_, ro.x, ro.y, z) && z < ro.z) {
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
                if (sceneMeshes_.find(mr->mesh) == sceneMeshes_.end()) continue;
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

// Moves the whole selection by one world-space step. The undo lifecycle -- one entry per HELD RUN,
// not one per repeat -- is the caller's job (handleManip's edit-verb block), exactly like the gizmo
// drag's own beginTransformEdit/endTransformEdit pair a few lines above it: this only ever applies
// one step and trusts a transform edit is already open around it.
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

// H: session-only visibility, off. Clears CMeshRenderer's own kMeshRendererVisible bit -- the exact
// flag the Details panel's Visible checkbox writes, so a hidden entity reads identically everywhere
// else that flag is already consulted (the render loop, pick()'s own eligibility test). NOT an
// undoable edit and NOT a level edit: levels do not store visibility, so this never calls pushEdit
// and never has to mark the level unsaved.
void SandboxApp::hideSelection() {
    scene::World& w = scene::World::instance();
    for (const scene::Entity e : selectedEntities()) {
        auto* mr = w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
        if (!mr || !(mr->flags & scene::kMeshRendererVisible)) continue;   // already hidden: not ours to restore
        mr->flags &= ~scene::kMeshRendererVisible;
        editorHidden_.push_back(e);
    }
}

// Shift+H: hides every OTHER eligible entity -- "eligible" meaning whatever pick() itself would have
// been willing to select, so isolate never touches something a click could not have reached either
// (a streamed chunk entity, one with no visible mesh, one whose mesh never resolved).
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
        if (sceneMeshes_.find(mr->mesh) == sceneMeshes_.end()) continue;
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
    if (dropButton(wireframe_ ? "Wireframe" : (unlit_ ? "Unlit" : "Lit"))) ImGui::OpenPopup("viewMode");
    if (ImGui::BeginPopup("viewMode")) {
        if (ImGui::Selectable("Lit", !wireframe_ && !unlit_)) { wireframe_=false; unlit_=false; }
        // WIREFRAME NEEDS THE RASTERISER; UNLIT NO LONGER DOES, and the split is the point.
        // Both used to be gated here, on a probe showing the same pixel byte-identical with
        // and without --unlit under the default settings. That measurement was right and
        // the conclusion drawn from it -- that the mode was impossible here -- was not: it
        // showed only that the mode was a per-draw flag on a draw call ray-driven never
        // makes. Given a pass-level one (gViewParams.x) PSRayDriven answers it directly.
        //
        // Wireframe is genuinely different and stays disabled with its reason shown, which
        // is still better than enabled-and-inert. docs/rendering/VIEW_MODES_PLAN.md calls
        // it "structurally impossible in ray-driven primary visibility as currently built";
        // that verdict now applies to wireframe alone.
        bool rasterModes = true;
#if AVER_MODULE_VOXI
        // suppressesScene() rather than rayDrivenActive(): it is the public predicate for
        // exactly this question -- 'the raster scene pass will not run this frame' -- and it
        // covers the GI debug raymarch too, which replaces the image for the same reason.
        rasterModes = !voxiRenderer_.suppressesScene();
#endif
        // UNLIT IS NO LONGER GATED ON THE RASTERISER. It was, correctly, while the mode
        // existed only as a per-draw flag that ray-driven never sees. PSRayDriven honours
        // gViewParams.x itself now, so the mode works in the DEFAULT renderer.
        if (ImGui::Selectable("Unlit", unlit_ && !wireframe_)) { unlit_ = true; wireframe_ = false; }
        // WIREFRAME STILL IS, for a reason unlit no longer shares: it needs a different
        // RASTERISER STATE rather than a different shading branch, and in ray-driven mode
        // there is no rasteriser in the loop to put into that state.
        ImGui::BeginDisabled(!rasterModes);
        if (ImGui::Selectable("Wireframe", wireframe_)) { wireframe_=true; unlit_=false; }
        ImGui::EndDisabled();
        uiReg_.track("viewMode.unlit");
        if (!rasterModes && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Ray-driven primary visibility draws the image without the rasteriser,\nso wireframe cannot apply. Turn it off in Settings > Rendering, or run with --no-rt.");
        ImGui::Selectable("Detail Lighting", false, ImGuiSelectableFlags_Disabled);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Needs a flat-albedo shading override the shader does not have yet.");
#if AVER_MODULE_VOXI
        ImGui::Separator();
        if (ImGui::Selectable("Voxel Radiance (GI debug)", giDebugView_)) giDebugView_ = !giDebugView_;
        // THE SAME TWO CONSOLE-VAR-ONLY GI PAINTS Settings > Rendering exposes (SandboxSettings.cpp):
        // raw bool slots owned by EditorConsole.hpp (consoleGiPoisonViewSlot()'s own comment there),
        // reasserted onto the live voxiRenderer_ every frame from onUpdate. Reading and toggling the
        // slots directly here, exactly as that page does, means this entry and the Settings checkbox
        // are the same switch rather than two that can disagree.
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
        // the ALREADY-active one turns it off (same toggle-back idiom the GI entry uses);
        // selecting a different one switches directly. Turning the G-buffer itself on is not this
        // dropdown's job -- onUpdate ORs gbufferOverride_ with this value, so picking any entry here is already sufficient.
        ImGui::Separator();
        {
            using GDM = GBufferDebugFeature::Mode;
            auto gbufItem = [&](const char* label, GDM m) {
                if (ImGui::Selectable(label, gbufferDebugView_ == m))
                    gbufferDebugView_ = (gbufferDebugView_ == m) ? GDM::Off : m;
            };
            // Scale documented once, in the label, rather than left for a reader to find in the
            // shader: a debug view whose scale is undocumented is decorative, not diagnostic.
            gbufItem("G-Buffer: Velocity (+/-8 texels/frame full-scale, debug)", GDM::Velocity);
            gbufItem("G-Buffer: View-Space Depth (debug)", GDM::ViewZ);
            gbufItem("G-Buffer: Normal + Roughness (debug)", GDM::NormalRoughness);
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (dropButton("Show")) ImGui::OpenPopup("showFlags");
    uiReg_.track("viewport.show");
    if (ImGui::BeginPopup("showFlags")) {
        ImGui::Checkbox("Grid", &showGrid_);
        uiReg_.track("show.grid");
        // THESE TWO USED TO SHARE A STACK-LOCAL. `bool t=true;` was re-initialised every
        // frame and written by both boxes, so they always rendered ticked, could never be
        // unticked, and toggled nothing. Both now drive real state that the frame reads.
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

    // MODE FIRST, then that mode's tools. The switcher is always the leftmost thing in the
    // toolbar so "what am I editing" is answered before "with which tool" -- previously the same
    // flat row, which is how a brush ended up sitting next to Rotate.
    // A DROPDOWN, not the row of buttons this used to be: two modes fit in a row, four do not, and
    // the row would grow every time a mode is added. A combo also lets an unavailable entry carry
    // its own explanation, which a greyed button can only deliver by hovering something that looks broken.
    {
        ImGui::PushStyleColor(ImGuiCol_Button, kAverOrangeDim);
        ImGui::PushStyleColor(ImGuiCol_Header, kAverOrangeDim);
        // SIZED FROM THE WIDEST MODE NAME, not a constant: this toolbar is right-anchored and
        // grows LEFTWARD, so an over-wide item does not clip, it marches across the viewport and
        // lands on top of the left-hand toolbar. A fixed 150*dpi did exactly that at 300% DPI.
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
        // The brush settings live HERE, in the mode that owns them, rather than appearing and
        // disappearing from a shared row depending on which tool happened to be selected.
        char brushLbl[32]; std::snprintf(brushLbl, sizeof brushLbl, "Brush %.0f", sculptRadiusCm_);
        if (dropButton(brushLbl)) ImGui::OpenPopup("brushParams");
        ImGui::SameLine(0, gap);
        if (ImGui::Button(landscapeDirty_ ? "Save Terrain *" : "Save Terrain")) saveLandscape();
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
    // "Cam 1" at the default 800 cm/s -- %g rather than %f so the default reads as "Cam 1" and
    // not "Cam 1.00", while the slow end, where a person placing something cares about 0.25
    // against 0.5, keeps three significant figures. Whole numbers above 10 only: at speed 18 the
    // decimals would be false accuracy.
    char camLbl[32];
    const f32 camDial = flySpeed_ / kCamSpeedUnit;
    std::snprintf(camLbl, sizeof camLbl, camDial < 10.0f ? "Cam %.3g" : "Cam %.0f", camDial);
    if (dropButton(camLbl)) ImGui::OpenPopup("camSpeed");
    if (ImGui::BeginPopup("camSpeed")) {
        f32 dial = flySpeed_ / kCamSpeedUnit;
        if (ImGui::SliderFloat("Speed", &dial, 20.0f / kCamSpeedUnit, 20000.0f / kCamSpeedUnit,
                               "%.2f", ImGuiSliderFlags_Logarithmic))
            flySpeed_ = dial * kCamSpeedUnit;
        // The rate is still what the camera actually moves at, and somebody measuring a fly-through
        // needs it -- so it is shown, just not as the number you steer by.
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

    // RAISED BY THE OPEN DRAWER'S HEIGHT -- a fix, not a nicety: this hint anchors to the
    // viewport's bottom edge, but vpH_ isn't reduced when a drawer opens (an overlay, not a dock
    // split), so the hint sat on top of the drawer's last ~70px.
    // Cosmetic for the Content Browser/Output Log; NOT for the Console, whose bottom row is its
    // INPUT LINE -- invisible underneath, yet still clickable/typeable blind since the overlay is
    // NoInputs.
    // drawerPixelH_ is the ANIMATED height, so the hint slides with it and returns to zero when closed.
    ImGui::SetNextWindowPos(ImVec2(vpX_+pad, vpY_+vpH_-pad-drawerPixelH_), ImGuiCond_Always, ImVec2(0,1));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("##vphint", nullptr, f | ImGuiWindowFlags_NoInputs);
    // ONE ARM PER MODE. The two-arm version said "Tab to Landscape" while standing in Foliage,
    // and called the mode "Select" while a foliage brush was armed -- a hint line that describes
    // a different mode from the one you are in is worse than no hint at all.
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
                        "Shift+Esc releases the mouse, Tab to Select");
            break;
        case EditorMode::Select:
        default:
            ImGui::Text("Select: %s  |  RMB fly (WASD/QE)  wheel speed  MMB pan  F focus  |  "
                        "1-4 tools, Tab to the last mode", kToolNames[(int)tool_]);
            break;
    }
    ImGui::End();
}

#endif

// Builds an outliner label from a surface name plus an ordinal: "M_Wall" -> "Wall 3". Falls back
// to the asset's stem.
std::string SandboxApp::makeEntityLabel(const std::string& surface, const std::string& asset) {
    std::string base = surface;
    if (base.rfind("M_", 0) == 0) base.erase(0, 2);
    if (base.empty()) {
        const std::size_t slash = asset.find_last_of("/\\");
        base = slash == std::string::npos ? asset : asset.substr(slash + 1);
        const std::size_t dot = base.find_last_of('.');
        if (dot != std::string::npos) base.erase(dot);
    }
    if (base.empty()) base = "Entity";
    return base + " " + std::to_string(++labelCounts_[base]);
}

// True only when terrain editing is actually possible right now: mode is Landscape AND a section
// is loaded. Every "should this click sculpt" test goes through here rather than checking the mode
// alone, so a level without terrain loaded cannot be painted into.
bool SandboxApp::editorModeIsLandscape() const {
#if AVER_MODULE_LANDSCAPE
    return mode_ == EditorMode::Landscape && landscapeLoaded_;
#else
    return false;
#endif
}

// Whether a mode can be entered AT ALL right now, and why not if it cannot.
// ONE PREDICATE PER MODE, in one place, because the dropdown, the keybind and setEditorMode all
// need the same answer. Returning the reason as well as the verdict is what lets the dropdown grey
// an entry AND say why on hover, instead of silently refusing a click.
bool SandboxApp::editorModeAvailable(EditorMode m, const char** whyNot) const {
    auto no = [&](const char* why) { if (whyNot) *whyNot = why; return false; };
    switch (m) {
        case EditorMode::Select:
            return true;
#if AVER_MODULE_LANDSCAPE
        case EditorMode::Landscape:
            // ENTERED WITHOUT ONE, DELIBERATELY. This used to refuse, which made the mode whose
            // whole job is terrain the one place you could not get to in order to MAKE terrain --
            // and the message it refused with named two workarounds, a hand-written LANDSCAPE
            // record and a CLI flag, neither of which is in the editor. The panel now offers a
            // Create Landscape button when none is resident; every tool inside it is still gated
            // on landscapeLoaded_ by that same early return.
            return true;
        case EditorMode::Foliage: {
            // See editor::foliageModeGate (FoliageTypeEditor.hpp) for why an empty palette no
            // longer refuses entry, and for the headless test covering both branches.
            const editor::FoliageModeGate gate =
                editor::foliageModeGate(landscapeLoaded_, foliagePalette_.empty());
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
    return mode_ == EditorMode::Foliage && landscapeLoaded_ && !foliagePalette_.empty();
#else
    return false;
#endif
}

// Switching mode ends whatever the previous one was mid-way through. A drag that began as a
// gizmo move and finishes as a brush stroke would apply one to the other's target.
void SandboxApp::setEditorMode(EditorMode m) {
    if (mode_ == m) return;
#if AVER_MODULE_LANDSCAPE
    // Refreshed here, BEFORE the availability check just below: a type authored moments ago
    // (Content Browser's New Foliage Type, or an edit saved from a palette row's own tab) must
    // make the mode enterable on this very click, not only after some other event happens to
    // call refreshFoliagePalette() first.
    if (m == EditorMode::Foliage) refreshFoliagePalette();
#endif
    const char* whyNot = "";
    if (!editorModeAvailable(m, &whyNot)) {
        AVER_WARN("[Editor] cannot enter {} mode: {}", kEditorModeNames[static_cast<int>(m)], whyNot);
        return;
    }
#if AVER_MODULE_LANDSCAPE
    // A stroke in flight is ENDED, not abandoned: endSculptStroke pushes the undo entry for
    // whatever was already painted. Dropping it instead would leave the terrain changed with
    // nothing on the undo stack to reverse it, which is the worst of both.
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
// which is what makes Tab a toggle rather than a cycle. Cycling through four modes with one key
// would mean pressing it three times to get back to where you were.
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
