#pragma once
// A dependency-free lightmap UV unwrapper: chart-by-normal, then a shelf packer.
//
// WHY THIS EXISTS INSTEAD OF xatlas. The engine docs name xatlas as the intended lightmap unwrapper,
// and nothing here disputes that -- it is the better algorithm. It is also not vendored (no network
// fetch is available to pull it in; see aver-subagents-cannot-vendor in project memory) and pulling
// in third-party parameterization code is a bigger decision than one v1 task should make on its own.
// So this is a deliberate, disclosed downgrade: geometry that is mostly axis-aligned boxes and walls
// -- which is what a level's static architecture actually is -- unwraps reasonably with a per-face
// planar projection, and the alternative to shipping this was shipping nothing.
//
// WHAT xatlas WOULD DO BETTER, so the next person knows what they are upgrading FROM:
//   - Real parameterization (LSCM/ABF), not a planar cube projection. A curved or angled surface
//     gets stretched and pinched here because every triangle in a chart is projected flat onto one
//     of 6 axes regardless of how much its actual normal has drifted from that axis; xatlas charts
//     surfaces by distortion and bends the UV to match the geometry.
//   - Charts chosen for parameterization quality, not for which of 6 buckets a face's normal falls
//     into. A gently curved dome comes out as one xatlas chart and as many small, badly-utilized
//     charts here, because every triangle whose normal crosses an axis boundary starts a new chart
//     even when the surface is perfectly smooth.
//   - A real bin/polygon packer with rotation. This packer places axis-aligned bounding RECTANGLES
//     on shelves and never rotates a chart to fit better, so irregular (e.g. triangular or L-shaped)
//     charts waste a lot of atlas area inside their own bounding box. xatlas packs the actual chart
//     silhouette and rotates freely.
//   - Automatic seam vertex duplication with a rebuilt index buffer. See the note on UnwrapResult::uv
//     below -- this v1 does not do that, and it is the sharpest edge of using it as-is.
//
// ALGORITHM.
//   1. CHART: every triangle is assigned to one of 6 signed axis buckets (+X,-X,+Y,-Y,+Z,-Z) by its
//      dominant normal component, then split into connected components within its bucket by shared
//      EDGES (shared vertex INDEX pairs, not shared position -- see the .cpp for why that is the
//      chosen tradeoff). This is what keeps two unrelated faces that both happen to face +X (two
//      opposite walls of a room, say) from landing in the same chart and overlapping in UV space:
//      they are not connected by any edge, so they are never unioned into one component no matter
//      how identical their normals are.
//   2. PROJECT: each chart is projected onto its dominant axis's plane, dropping that axis's
//      coordinate. No mirror-correction is applied for the negative axes (-X/-Y/-Z project with the
//      same (u,v) formula as their positive counterpart) -- that would matter for a shared decal
//      texture, where a mirrored UV shows the artwork backwards, but it does not matter for a
//      lightmap: every texel bakes its own irradiance sample independently, so a "mirrored" chart
//      still bakes correct lighting, just along a flipped V axis relative to some other convention
//      nothing here needs to agree with.
//   3. PACK: chart bounding rectangles are packed into a square atlas with a shelf/skyline packer
//      (sort tallest first, fill a shelf left to right, start a new shelf when a chart would not fit
//      the current one). This is the "v1" the task brief asks for -- no rotation, no polygon packing,
//      just rectangles on shelves -- and it can waste real atlas area on non-rectangular charts. If
//      the first packing attempt does not fit `targetAtlasEdge`, the whole layout is rescaled down
//      and retried; the atlas dimensions returned are always exactly targetAtlasEdge x
//      targetAtlasEdge (this never grows the atlas past what was asked for).
//   4. GUTTER: every chart is padded by at least 2 texels of empty space from its neighbours AND
//      from the atlas edge (see kGutterTexels in the .cpp). Lightmaps are sampled bilinearly and
//      mip-mapped, and both operations read past a chart's own border; without a gutter, one chart's
//      baked light bleeds into its neighbour's texels the moment either is minified or sampled near
//      its edge.
//
// SEAM VERTICES ARE DUPLICATED, AND THE OUTPUT IS THEREFORE A NEW VERTEX BUFFER. A vertex shared
// by triangles in two different charts -- every corner of a cube, every hard edge in any real mesh
// -- cannot hold one UV pair: each chart needs its own. An earlier version of this header argued
// that last-write-wins was an acceptable v1 and that callers should "treat visible seams at hard
// edges as expected". That was wrong, and its own test caught it: the overlap check failed because
// a shared vertex really does land inside another chart's rectangle, which is not a cosmetic seam
// but a vertex sampling a completely unrelated part of the atlas. A lightmap that does that is not
// usable for the shipping case this exists for.
//
// So this function REBUILDS the vertex list. `vertexSource` maps each output vertex back to the
// input vertex it was copied from, and `indices` is the caller's index buffer remapped onto the new
// vertices. A caller builds its lightmapped mesh by gathering position/normal/UV0 through
// vertexSource and drawing with `indices`; nothing is lost, and the original buffers are untouched.
//
// THE COST, stated: output vertex count is >= input, and for a faceted mesh it approaches
// 3 x triangleCount, because a vertex is duplicated once per chart that touches it. That is what
// every real unwrapper (xatlas included) does for the same reason.
//
// Engine space: centimetres, +X forward, +Y right, +Z up (aver::Vec3 / aver/core/Math.hpp), though
// this module only reads raw float triples so it never actually includes that header.
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// One unwrap's output, addressed by NEW vertex indices -- see the seam-duplication note above.
// Every UV is inside [0,1] by construction (the packer never places content outside the atlas).
struct UnwrapResult {
    // 2 per OUTPUT vertex: (u0,v0,u1,v1,...). uv.size() == vertexSource.size() * 2.
    std::vector<f32> uv;
    // For each output vertex, the input vertex it was copied from. Gather positions, normals and
    // UV0 through this to build the lightmapped vertex buffer.
    std::vector<u32> vertexSource;
    // The caller's triangles, remapped onto the output vertices. Same length and winding as the
    // input index buffer -- only the values change.
    std::vector<u32> indices;
    u32 atlasWidth = 0;
    u32 atlasHeight = 0;
    u32 chartCount = 0;
};

// Builds a lightmap unwrap for one mesh.
//
// positions: vertexCount * 3 floats (x,y,z). normals: vertexCount * 3 floats, used only to help
// classify each triangle's dominant axis (see the .cpp for how a degenerate average normal is
// handled) -- geometry is still read from `positions`, so a caller passing zeroed normals still
// gets a legal, if less faithfully classified, unwrap rather than a refusal.
// indices: indexCount indices into positions/normals, indexCount % 3 == 0, one triangle per 3.
// targetAtlasEdge: the atlas is always exactly this many texels on each side; this call fits chart
// content into that budget by shrinking, never by returning a larger atlas.
//
// Returns false with `err` set (when non-null) on a malformed input (null/zero-length buffers that
// disagree with their counts, an index out of range, indexCount not a multiple of 3, or an
// atlasEdge too small to hold even the gutter). An input with zero triangles after validation is
// not an error: it returns true with chartCount == 0 and empty uv/vertexSource/indices -- there is
// no geometry to address, so there are no output vertices either.
bool unwrapForLightmap(const f32* positions, const f32* normals, u32 vertexCount,
                        const u32* indices, u32 indexCount, u32 targetAtlasEdge,
                        UnwrapResult& out, std::string* err = nullptr);

} // namespace aver::fmt
