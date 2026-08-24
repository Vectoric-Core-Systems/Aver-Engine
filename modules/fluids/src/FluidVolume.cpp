// Builds the seed shell and holds the simulated shape a renderer reads back. See FluidVolume.hpp.
#include "aver/fluids/FluidVolume.hpp"

#include <algorithm>
#include <cmath>

namespace aver::fluids {
namespace {

// Position along one axis at lattice index `idx` of `n` equal segments, spanning
// [-halfExtentCm, +halfExtentCm]. `n` is always >= 1 by the time this is called --
// generateFluidSeedShell clamps every subdivision count before touching this -- so there is no
// divide-by-zero to guard against here, unlike GerstnerWave.hpp's wavelength clamp.
inline f32 latticeLerp(f32 halfExtentCm, i32 idx, i32 n) {
    const f32 t = static_cast<f32>(idx) / static_cast<f32>(n);
    return -halfExtentCm + t * (2.0f * halfExtentCm);
}

} // namespace

f32 fluidPressureFor(const FluidVolumeDesc& desc, f32 gravityCmPerS2) {
    // The same clamp generateFluidSeedShell applies, for the same reason: a subdivision count below 1
    // is not a shell, and reading one here would make the pressure disagree with the mesh the solver
    // is actually given.
    // Particles ALONG each top-face axis: one more than the number of segments.
    const f32 nx = static_cast<f32>(std::max(1, desc.subdivisions[0]) + 1);
    const f32 ny = static_cast<f32>(std::max(1, desc.subdivisions[1]) + 1);
    const f32 hz = std::fabs(desc.halfExtentCm[2]);
    const f32 g  = std::fabs(gravityCmPerS2);
    // 2 * g * hz * nx * ny, scaled out of centimetres into the metres Jolt integrates in -- both
    // steps derived in the header. Zero depth or zero gravity legitimately wants zero pressure, since
    // there is nothing for it to hold up, and Jolt reads a non-positive coefficient as "no pressure
    // at all", so neither needs a special case here.
    return kFluidPressureHeadroom * kFluidCmToJolt * 2.0f * g * hz * nx * ny;
}

// ---------------------------------------------------------------------------------------------
// (a) THE SEED SHELL.
//
// A box's boundary lattice point (i, j, k) -- i in [0,nx], j in [0,ny], k in [0,nz] -- is shared
// by up to three of the box's six faces whenever it sits on an edge or corner. Rather than build
// six independent (nu+1)x(nv+1) grids and merge coincident vertices by comparing floating-point
// positions -- fragile the moment two faces' lerps disagree by an ULP at a shared edge, which is
// exactly the kind of "closed except for one silent gap" defect this function exists to avoid --
// every boundary point is assigned to exactly ONE of three non-overlapping groups, decided purely
// from its integer (i, j, k):
//
//   Block A: i == 0 or i == nx, for EVERY (j, k). Both X faces, in full.
//   Block B: j == 0 or j == ny, for i in [1, nx-1] (i == 0 or nx is already block A) and every k.
//            Both Y faces, minus the two columns block A already owns.
//   Block C: k == 0 or k == nz, for i in [1, nx-1] and j in [1, ny-1] (both already-owned edges
//            excluded). Both Z faces, minus the border blocks A and B already own.
//
// Every boundary point satisfies exactly one of these in that priority order -- a point with i at
// an extreme is block A even if j or k is also at an extreme; failing that, a point with j at an
// extreme is block B even if k is also at an extreme; anything left over has already failed both
// tests, so it can only be on the boundary through k. vertexIndex() below re-derives the same case
// split to look a shared vertex back up while stitching the six faces together, so the array
// position a vertex was pushed at and the index a triangle later asks for it by are the same
// computation, not two that merely need to agree. No lookup table, no hashing, no float
// comparison anywhere in the sharing -- only which of three integer ranges (i, j, k) falls into.
void generateFluidSeedShell(const FluidVolumeDesc& desc,
                             std::vector<f32>& outPositionsCm,
                             std::vector<i32>& outIndices) {
    outPositionsCm.clear();
    outIndices.clear();

    const i32 nx = std::max(desc.subdivisions[0], 1);
    const i32 ny = std::max(desc.subdivisions[1], 1);
    const i32 nz = std::max(desc.subdivisions[2], 1);
    const f32 hx = desc.halfExtentCm[0];
    const f32 hy = desc.halfExtentCm[1];
    const f32 hz = desc.halfExtentCm[2];

    const i32 blockAVerts = 2 * (ny + 1) * (nz + 1);
    const i32 blockBVerts = 2 * (nx - 1) * (nz + 1);
    const i32 blockCVerts = 2 * (nx - 1) * (ny - 1);
    outPositionsCm.reserve(static_cast<usize>(blockAVerts + blockBVerts + blockCVerts) * 3);

    // Block A: both X faces, i == 0 then i == nx, each walked in full (j, k) order -- the SAME
    // order vertexIndex()'s first branch computes below, so the n-th vertex pushed here is exactly
    // the vertex vertexIndex() returns the index n for.
    for (i32 iSel = 0; iSel < 2; ++iSel) {
        const f32 x = latticeLerp(hx, iSel == 0 ? 0 : nx, nx);
        for (i32 j = 0; j <= ny; ++j) {
            const f32 y = latticeLerp(hy, j, ny);
            for (i32 k = 0; k <= nz; ++k) {
                outPositionsCm.push_back(x);
                outPositionsCm.push_back(y);
                outPositionsCm.push_back(latticeLerp(hz, k, nz));
            }
        }
    }
    // Block B: both Y faces, j == 0 then j == ny, i restricted to the interior columns block A
    // does not already own.
    for (i32 jSel = 0; jSel < 2; ++jSel) {
        const f32 y = latticeLerp(hy, jSel == 0 ? 0 : ny, ny);
        for (i32 i = 1; i <= nx - 1; ++i) {
            const f32 x = latticeLerp(hx, i, nx);
            for (i32 k = 0; k <= nz; ++k) {
                outPositionsCm.push_back(x);
                outPositionsCm.push_back(y);
                outPositionsCm.push_back(latticeLerp(hz, k, nz));
            }
        }
    }
    // Block C: both Z faces, k == 0 then k == nz, i AND j restricted to the interior rows blocks A
    // and B do not already own.
    for (i32 kSel = 0; kSel < 2; ++kSel) {
        const f32 z = latticeLerp(hz, kSel == 0 ? 0 : nz, nz);
        for (i32 i = 1; i <= nx - 1; ++i) {
            const f32 x = latticeLerp(hx, i, nx);
            for (i32 j = 1; j <= ny - 1; ++j) {
                outPositionsCm.push_back(x);
                outPositionsCm.push_back(latticeLerp(hy, j, ny));
                outPositionsCm.push_back(z);
            }
        }
    }

    // Reproduces exactly which array slot the three loops above put (i, j, k) into -- see this
    // function's own top-of-file comment for why a case split on (i, j, k) rather than a lookup
    // table. Every (i, j, k) this is ever called with, from the face loops below, is a genuine
    // boundary point by construction, so the final branch (neither i nor j at an extreme) is
    // always reached with k at one.
    auto vertexIndex = [&](i32 i, i32 j, i32 k) -> i32 {
        if (i == 0 || i == nx) {
            const i32 iSel = i == 0 ? 0 : 1;
            return iSel * (ny + 1) * (nz + 1) + j * (nz + 1) + k;
        }
        if (j == 0 || j == ny) {
            const i32 jSel = j == 0 ? 0 : 1;
            return blockAVerts + jSel * (nx - 1) * (nz + 1) + (i - 1) * (nz + 1) + k;
        }
        const i32 kSel = k == 0 ? 0 : 1;
        return blockAVerts + blockBVerts + kSel * (nx - 1) * (ny - 1) + (i - 1) * (ny - 1) + (j - 1);
    };

    const usize totalIndices = 12 * (static_cast<usize>(nx) * static_cast<usize>(ny) +
                                     static_cast<usize>(ny) * static_cast<usize>(nz) +
                                     static_cast<usize>(nz) * static_cast<usize>(nx));
    outIndices.reserve(totalIndices);

    // Every face below is walked over its own two tangent axes (p, q), mapped into (i, j, k) so
    // that TANGENT-P x TANGENT-Q equals that face's own outward normal -- e.g. the +Z face's
    // tangents are +X then +Y, and X x Y = +Z. Given that, ONE winding pattern --
    // (p,q),(p,q+1),(p+1,q) then (p+1,q),(p,q+1),(p+1,q+1) -- is front-facing and outward on
    // EVERY face, not only the one it happens to be demonstrated on: for a triangle (A, B, C)
    // wound that way, (C-A) x (B-A) reduces to TANGENT-P x TANGENT-Q (C-A runs one step along
    // tangent P, B-A one step along tangent Q), which is that face's outward normal by
    // construction -- and this engine takes (C-A) x (B-A) as the outward normal of a front-facing
    // triangle. modules/landscape/src/ChunkMesh.cpp's own surface-grid comment -- "seen from
    // above, (v0, v2, v1) is clockwise and front-facing" -- is this exact relation, for the one
    // tangent pair (+X, +Y) that file happens to draw. Nothing below is independently re-derived
    // per face; it is that one identity, carried to the other five tangent pairs a box needs.
    //
    // This has not been checked by running anything -- see FluidVolume.hpp's own caveat on this
    // point. A wrong tangent pair here would still leave the mesh CLOSED (vertexIndex()'s sharing
    // does not depend on winding at all), just with that one face turned inside out.
    auto pushQuad = [&](i32 v00, i32 v01, i32 v10, i32 v11) {
        outIndices.push_back(v00); outIndices.push_back(v01); outIndices.push_back(v10);
        outIndices.push_back(v10); outIndices.push_back(v01); outIndices.push_back(v11);
    };

    // +X: tangents +Y (p), +Z (q); Y x Z = +X.
    for (i32 p = 0; p < ny; ++p) {
        for (i32 q = 0; q < nz; ++q) {
            pushQuad(vertexIndex(nx, p, q), vertexIndex(nx, p, q + 1),
                     vertexIndex(nx, p + 1, q), vertexIndex(nx, p + 1, q + 1));
        }
    }
    // -X: tangents +Z (p), +Y (q); Z x Y = -X.
    for (i32 p = 0; p < nz; ++p) {
        for (i32 q = 0; q < ny; ++q) {
            pushQuad(vertexIndex(0, q, p), vertexIndex(0, q + 1, p),
                     vertexIndex(0, q, p + 1), vertexIndex(0, q + 1, p + 1));
        }
    }
    // +Y: tangents +Z (p), +X (q); Z x X = +Y.
    for (i32 p = 0; p < nz; ++p) {
        for (i32 q = 0; q < nx; ++q) {
            pushQuad(vertexIndex(q, ny, p), vertexIndex(q + 1, ny, p),
                     vertexIndex(q, ny, p + 1), vertexIndex(q + 1, ny, p + 1));
        }
    }
    // -Y: tangents +X (p), +Z (q); X x Z = -Y.
    for (i32 p = 0; p < nx; ++p) {
        for (i32 q = 0; q < nz; ++q) {
            pushQuad(vertexIndex(p, 0, q), vertexIndex(p, 0, q + 1),
                     vertexIndex(p + 1, 0, q), vertexIndex(p + 1, 0, q + 1));
        }
    }
    // +Z: tangents +X (p), +Y (q); X x Y = +Z. Same pair ChunkMesh.cpp's own comment names.
    for (i32 p = 0; p < nx; ++p) {
        for (i32 q = 0; q < ny; ++q) {
            pushQuad(vertexIndex(p, q, nz), vertexIndex(p, q + 1, nz),
                     vertexIndex(p + 1, q, nz), vertexIndex(p + 1, q + 1, nz));
        }
    }
    // -Z: tangents +Y (p), +X (q); Y x X = -Z.
    for (i32 p = 0; p < ny; ++p) {
        for (i32 q = 0; q < nx; ++q) {
            pushQuad(vertexIndex(q, p, 0), vertexIndex(q + 1, p, 0),
                     vertexIndex(q, p + 1, 0), vertexIndex(q + 1, p + 1, 0));
        }
    }
}

// ---------------------------------------------------------------------------------------------
// (b) THE STATEFUL HALF.

void FluidVolume::generateSeedShell() {
    generateFluidSeedShell(desc_, seedPositionsCm_, indices_);

    // Seeds positionsCm()/normals() from the LOCAL shell placed at desc().centreCm (LOCAL + centre
    // = WORLD -- see generateFluidSeedShell's own header comment for why the shell itself is built
    // about local (0,0,0) rather than pre-offset), so a renderer that draws before the physics
    // solver's first callback still gets a correctly-shaped, correctly-lit body rather than an
    // empty one. updateFromSimulation below replaces this the moment the solver's own positions
    // start arriving; nothing re-adds centreCm there, because the solver already returns WORLD
    // positions.
    const usize n = seedPositionsCm_.size();
    positionsCm_.resize(n);
    for (usize v = 0; v < n; v += 3) {
        positionsCm_[v + 0] = seedPositionsCm_[v + 0] + desc_.centreCm[0];
        positionsCm_[v + 1] = seedPositionsCm_[v + 1] + desc_.centreCm[1];
        positionsCm_[v + 2] = seedPositionsCm_[v + 2] + desc_.centreCm[2];
    }
    recomputeNormals();
}

void FluidVolume::updateFromSimulation(const f32* verticesXyz, i32 vertexCount) {
    if (verticesXyz == nullptr) return;

    // Only the overlapping prefix is copied and the rest of positionsCm() is left exactly as it
    // was -- see this function's own header comment for why a count mismatch is handled this way
    // instead of trusted or asserted.
    const i32 n = std::min(vertexCount, this->vertexCount());
    if (n <= 0) return;
    std::copy(verticesXyz, verticesXyz + static_cast<usize>(n) * 3, positionsCm_.begin());
    recomputeNormals();
}

void FluidVolume::recomputeNormals() {
    const usize vertCount = positionsCm_.size() / 3;
    normals_.assign(vertCount * 3, 0.0f);

    // Area-weighted face averaging: the seed shell's own normals are only ever right for the shape
    // it was built from, and stop matching the instant the body deforms -- which is the entire
    // reason to simulate it in the first place. Recomputing here, every time new positions arrive
    // (including the seed shell's own first frame, via generateSeedShell's call into this), is what
    // keeps positionsCm() and normals() a matched pair a renderer can light correctly always.
    for (usize t = 0; t + 2 < indices_.size(); t += 3) {
        const usize ia = static_cast<usize>(indices_[t + 0]);
        const usize ib = static_cast<usize>(indices_[t + 1]);
        const usize ic = static_cast<usize>(indices_[t + 2]);
        const f32* pa = &positionsCm_[ia * 3];
        const f32* pb = &positionsCm_[ib * 3];
        const f32* pc = &positionsCm_[ic * 3];

        const f32 eABx = pb[0] - pa[0], eABy = pb[1] - pa[1], eABz = pb[2] - pa[2];
        const f32 eACx = pc[0] - pa[0], eACy = pc[1] - pa[1], eACz = pc[2] - pa[2];
        // eAC x eAB, left UN-normalised: its length is twice the triangle's area, so accumulating
        // it as-is into each of the triangle's three corners -- rather than normalising to a unit
        // face normal first -- is what makes the average AREA-WEIGHTED. A large triangle pulls its
        // corners' summed normal toward its own face normal harder than a sliver triangle sharing
        // the same vertex does. This is the same (C-A) x (B-A) outward-normal relation
        // generateFluidSeedShell's own comment derives for this engine's winding convention,
        // evaluated per deformed triangle here instead of once per authored face.
        const f32 nx = eACy * eABz - eACz * eABy;
        const f32 ny = eACz * eABx - eACx * eABz;
        const f32 nz = eACx * eABy - eACy * eABx;

        normals_[ia * 3 + 0] += nx; normals_[ia * 3 + 1] += ny; normals_[ia * 3 + 2] += nz;
        normals_[ib * 3 + 0] += nx; normals_[ib * 3 + 1] += ny; normals_[ib * 3 + 2] += nz;
        normals_[ic * 3 + 0] += nx; normals_[ic * 3 + 1] += ny; normals_[ic * 3 + 2] += nz;
    }

    for (usize v = 0; v < vertCount; ++v) {
        f32& x = normals_[v * 3 + 0];
        f32& y = normals_[v * 3 + 1];
        f32& z = normals_[v * 3 + 2];
        const f32 lenSq = x * x + y * y + z * z;
        // A vertex with no incident triangle sums to exactly zero here rather than an arbitrary
        // unit direction. That should not happen for a closed shell fed straight back from the
        // solver, but updateFromSimulation trusts whatever positions a caller hands it, not only
        // ones this file generated.
        const f32 invLen = lenSq > 1e-12f ? 1.0f / std::sqrt(lenSq) : 0.0f;
        x *= invLen; y *= invLen; z *= invLen;
    }
}

} // namespace aver::fluids
