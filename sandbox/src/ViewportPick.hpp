// THE RULE THIS HEADER ENFORCES: the viewport selects by TRIANGLES, not by bounding box, and a
// triangle's facing comes from its VERTEX NORMALS, never from its winding order. rayPickGeometry is
// the one nearest-hit test both of pick()'s object loops call once they know an entity's box is even
// worth testing; SandboxApp.cpp still owns the broadphase (which entities are eligible, which box each
// one uses, how the per-mesh triangle data gets here in the first place) and everything downstream of
// a hit (selection, Ctrl-extend). This header only ever answers "does this ray hit this mesh, and
// where" -- it does not know what an entity, a component, or a device is.
//
// A PURE HEADER, DELIBERATELY, for aver/game/SceneSubmission.hpp's exact reason (see that file's own
// top comment; that one has since been promoted out of sandbox/src into Aver.Runtime.Game.Core, but
// the reasoning it states is why this file is shaped the way it is, and only the path changed): no
// ImGui types, no SandboxApp state, no rhi::, no fmt::, no AVER_WARN, no globals -- plain values in,
// a plain bool and a plain f32 out. aver::Vec3 (aver/core/Math.hpp) is the one exception,
// used because SandboxApp.cpp's own ray already is one and a second, header-local vector type would
// only add conversions at every call site; Math.hpp is itself header-only inline and pulls in nothing
// beyond Types.hpp and <cmath>, so it costs this header nothing SandboxApp.cpp was not already paying.
//
// Converting a loaded mesh (rhi::MeshVertex arrays, or fmt::OcMeshData) into a PickGeometry stays in
// SandboxApp.cpp on purpose -- either source type would drag rhi:: or fmt:: headers in here for a
// conversion this header does not otherwise need to know how to do.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include <cmath>
#include <vector>

namespace aver::editor {

// One mesh's triangles, in the mesh's own local space -- everything rayPickGeometry needs and nothing
// an rhi:: handle or an fmt:: loader owns.
struct PickGeometry {
    std::vector<f32> positions;   // 3 per vertex
    std::vector<f32> normals;     // 3 per vertex; MAY BE EMPTY -- see rayPickGeometry's own fallback
    std::vector<u32> indices;     // 3 per triangle

    // No triangles to test -- the caller's cue to fall back to the entity's bounding box instead.
    bool empty() const { return indices.empty() || positions.empty(); }
};

// Nearest-hit ray/triangle test over one mesh (Moller-Trumbore), DOUBLE-SIDED by default because
// ray-driven primary visibility -- the editor's default render mode -- traces with no cull flag and
// draws back faces too, so a click must be able to hit what the camera actually sees. `o`/`d` must
// already be in the SAME local space as `g`'s positions; `d` is UNNORMALISED and can be arbitrarily
// long (the caller hands it a near-to-far vector, possibly transformed by a non-uniform scale), so the
// parallel/degenerate test below is SCALE-AWARE -- it compares the signed area against the triangle's
// and ray's own magnitude rather than a fixed constant, because a fixed epsilon compared against a
// determinant built from a long `d` would reject real hits, and the same epsilon against a short one
// would accept false ones. Every intermediate is f64: f32 alone loses enough precision over a
// level-sized `d` that a triangle lying close to parallel with the ray can flip between "hit" and
// "miss" depending on unrelated bits elsewhere in the mesh.
//
// BACK FACE = the triangle's three VERTEX NORMALS, averaged, pointing along the ray: dot(nAvg, d) > 0.
// This is deliberately NOT winding order -- a flipped submesh, or a mesh whose winding disagrees with
// its authored normals (both happen in imported content), must cull the way it actually SHADES, not
// the way its indices happen to wind. A triangle with no normals, or whose three vertex normals cancel
// to ~zero, is treated as front-facing rather than guessed at (skipBackFaces never rejects it).
//
// Only a hit with t in (0, tMax) is accepted, and tMax itself tightens to the nearest hit found so far
// WITHIN this one call -- the caller still has to compare the winning t across meshes/objects itself,
// exactly as it already does for rayAabb. A triangle with an out-of-range index, or one whose three
// points are collinear/coincident (zero area), is skipped rather than treated as a miss that could
// still corrupt tHit.
inline bool rayPickGeometry(const PickGeometry& g, Vec3 o, Vec3 d, bool skipBackFaces, f32 tMax,
                             f32& tHit) {
    if (g.empty()) return false;
    const usize vertCount = g.positions.size() / 3;
    const bool haveNormals = g.normals.size() == g.positions.size();   // else treated as unlit -> front-facing

    const f64 ox = o.x, oy = o.y, oz = o.z;
    const f64 dx = d.x, dy = d.y, dz = d.z;
    const f64 dlen = std::sqrt(dx*dx + dy*dy + dz*dz);

    bool found = false;
    f64 bestT = static_cast<f64>(tMax);

    for (usize tri = 0; tri + 2 < g.indices.size(); tri += 3) {
        const u32 i0 = g.indices[tri + 0], i1 = g.indices[tri + 1], i2 = g.indices[tri + 2];
        if (i0 >= vertCount || i1 >= vertCount || i2 >= vertCount) continue;   // out-of-range: skip, safely

        const f64 v0x = g.positions[usize(i0)*3+0], v0y = g.positions[usize(i0)*3+1], v0z = g.positions[usize(i0)*3+2];
        const f64 v1x = g.positions[usize(i1)*3+0], v1y = g.positions[usize(i1)*3+1], v1z = g.positions[usize(i1)*3+2];
        const f64 v2x = g.positions[usize(i2)*3+0], v2y = g.positions[usize(i2)*3+1], v2z = g.positions[usize(i2)*3+2];

        const f64 e1x = v1x - v0x, e1y = v1y - v0y, e1z = v1z - v0z;
        const f64 e2x = v2x - v0x, e2y = v2y - v0y, e2z = v2z - v0z;

        // pvec = d x e2; det = e1 . pvec
        const f64 pvx = dy*e2z - dz*e2y, pvy = dz*e2x - dx*e2z, pvz = dx*e2y - dy*e2x;
        const f64 det = e1x*pvx + e1y*pvy + e1z*pvz;

        // SCALE-AWARE degeneracy test in place of a fixed epsilon: |det| is the (signed) parallelepiped
        // volume of e1, e2 and d, so it naturally scales with all three -- normalising it against their
        // product's magnitude gives a tolerance that means the same thing (how close to parallel/planar)
        // regardless of how long `d` or the triangle's own edges are.
        const f64 e1Len = std::sqrt(e1x*e1x + e1y*e1y + e1z*e1z);
        const f64 e2Len = std::sqrt(e2x*e2x + e2y*e2y + e2z*e2z);
        const f64 scale = e1Len * e2Len * dlen;
        if (scale <= 0.0 || std::fabs(det) < 1e-9 * scale) continue;   // parallel to the ray, or zero-area

        const f64 invDet = 1.0 / det;
        const f64 tvx = ox - v0x, tvy = oy - v0y, tvz = oz - v0z;
        const f64 u = (tvx*pvx + tvy*pvy + tvz*pvz) * invDet;
        if (u < 0.0 || u > 1.0) continue;

        // qvec = tv x e1
        const f64 qvx = tvy*e1z - tvz*e1y, qvy = tvz*e1x - tvx*e1z, qvz = tvx*e1y - tvy*e1x;
        const f64 v = (dx*qvx + dy*qvy + dz*qvz) * invDet;
        if (v < 0.0 || u + v > 1.0) continue;

        const f64 t = (e2x*qvx + e2y*qvy + e2z*qvz) * invDet;
        if (t <= 0.0 || t >= bestT) continue;   // behind the ray origin, or not nearer than the best so far

        if (skipBackFaces && haveNormals) {
            const f64 nx = g.normals[usize(i0)*3+0] + g.normals[usize(i1)*3+0] + g.normals[usize(i2)*3+0];
            const f64 ny = g.normals[usize(i0)*3+1] + g.normals[usize(i1)*3+1] + g.normals[usize(i2)*3+1];
            const f64 nz = g.normals[usize(i0)*3+2] + g.normals[usize(i1)*3+2] + g.normals[usize(i2)*3+2];
            const f64 nLen2 = nx*nx + ny*ny + nz*nz;
            // A ~zero average (three normals cancelling, e.g. a sliver at a hard crease) can't say
            // which way the triangle faces, so it is kept rather than guessed away.
            if (nLen2 > 1e-12) {
                const f64 facing = nx*dx + ny*dy + nz*dz;
                if (facing > 0.0) continue;   // the averaged normal points along the ray: a back face
            }
        }

        bestT = t;
        found = true;
    }

    if (found) tHit = static_cast<f32>(bestT);
    return found;
}

// Nearest ENTRY of a ray into an upright capsule standing on the local origin: axis along +Z, poles
// at z = 0 and z = height, radius `radius` -- the Player Start's shape (buildCapsuleWire). For a
// marker drawn as lines and a sprite there are no triangles to test, and its bounding box is both
// too wide at the corners and too short to reach the sprite.
//
// The capsule is the union of a finite cylinder and two spheres, and with the origin outside all
// three the first entry into the union is the smallest entry into any one part. A ray that STARTS
// inside is no hit, pick()'s start-inside rule: standing in the marker must not select it on every
// click. `o`/`d` are local and `d` may be unnormalised, as for rayPickGeometry; t is in (0, tMax).
inline bool rayUprightCapsule(Vec3 o, Vec3 d, f32 radius, f32 height, f32 tMax, f32& tHit) {
    const f64 r = radius;
    const f64 zLo = r, zHi = std::fmax(r, static_cast<f64>(height) - r);   // hemisphere seams
    const f64 ox = o.x, oy = o.y, oz = o.z, dx = d.x, dy = d.y, dz = d.z;

    const f64 cz = oz < zLo ? zLo : (oz > zHi ? zHi : oz);
    if (ox*ox + oy*oy + (oz - cz)*(oz - cz) <= r*r) return false;

    bool found = false;
    f64 best = static_cast<f64>(tMax);
    // Side wall: the infinite cylinder's entry, kept only between the seams.
    const f64 a = dx*dx + dy*dy;
    if (a > 0.0) {
        const f64 hb = ox*dx + oy*dy, c = ox*ox + oy*oy - r*r;
        const f64 disc = hb*hb - a*c;
        if (disc >= 0.0) {
            const f64 t = (-hb - std::sqrt(disc)) / a;
            const f64 z = oz + t*dz;
            if (t > 0.0 && t < best && z >= zLo && z <= zHi) { best = t; found = true; }
        }
    }
    // Domes: each seam's full sphere. An entry through the half inside the cylinder can never be
    // the nearest -- the ray crossed the side wall or the other dome first -- so no half test.
    const f64 dd = dx*dx + dy*dy + dz*dz;
    if (dd > 0.0) {
        for (const f64 sz : {zLo, zHi}) {
            const f64 qz = oz - sz;
            const f64 hb = ox*dx + oy*dy + qz*dz, c = ox*ox + oy*oy + qz*qz - r*r;
            const f64 disc = hb*hb - dd*c;
            if (disc < 0.0) continue;
            const f64 t = (-hb - std::sqrt(disc)) / dd;
            if (t > 0.0 && t < best) { best = t; found = true; }
        }
    }
    if (found) tHit = static_cast<f32>(best);
    return found;
}

} // namespace aver::editor
