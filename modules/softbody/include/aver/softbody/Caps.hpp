// Thick breaks as derived draw geometry: a back skin and wall quads along every boundary edge of the visible
// triangles. Nothing here touches the cage, the Jolt body or the tear pipeline; the inputs are the piece's draw
// indices and positions, so a crack, a hole or a missing triangle grows a wall by itself. Pure arithmetic, no
// scene, no GPU. See docs/SOFTBODY.md "Capped breaks".
#pragma once

#include "aver/core/Math.hpp"

#include <vector>

namespace aver::softbody {

// `Auto`: all pieces when the authored mesh has boundary edges (an open sheet), debris only when it is closed.
enum class BackSkin : u8 { Auto = 0, Off = 1, Debris = 2, All = 3 };

struct CapSpec {
    f32 thicknessCm = 0.5f;          // world cm (the caller multiplies by the entity's uniform scale); <= 0 builds nothing
    bool backSkin = true;            // resolved from BackSkin and the piece by the caller
    f32 uvCmPerTile = 40.0f;
    u32 vertexLimit = 8192;          // per piece, front included; over it the piece draws without caps
};

// position = P(src) - N(src) * depth * thickness, with depth 0 or 1 and src a draw vertex of the piece.
struct CapVertex { u32 src; f32 depth; f32 u, v; };

struct CapGeometry {
    std::vector<CapVertex> backVerts;  std::vector<u32> backIndices;   // reversed copies of the visible triangles
    std::vector<CapVertex> wallVerts;  std::vector<u32> wallIndices;   // one quad per boundary edge
    u32 boundaryEdges = 0;
    bool overLimit = false;
    bool empty() const { return backVerts.empty() && wallVerts.empty(); }
};

// `idx` are the piece's visible triangles over `n` draw vertices; `weldOf[d]` is the identity of the surface point
// (a cage particle, so a UV seam is not an edge); `rest` are the draw positions at the epoch (lengths for UVs only);
// `uv` are 2 floats per draw vertex (null reads 0). Index lists of the result are local to their own vertex list.
CapGeometry buildCaps(const u32* idx, u32 idxCount, const u32* weldOf, const Vec3* rest, const f32* uv, u32 n,
                      const CapSpec& spec);

// Per frame: the smooth normal of every draw vertex (area weighted over its weld group, from the visible
// triangles; zero for a vertex no triangle uses) and the cap vertex positions from it.
void smoothNormals(const Vec3* pos, u32 n, const u32* idx, u32 idxCount, const u32* weldOf, Vec3* outNormal);
void capPositions(const std::vector<CapVertex>& verts, const Vec3* pos, const Vec3* normal, f32 thicknessCm, Vec3* out);

// Whether the open-sheet rule of BackSkin::Auto holds: some edge of the triangle set is used exactly once.
bool hasBoundaryEdges(const u32* idx, u32 idxCount, const u32* weldOf);

// Resolves the back-skin policy for one piece.
inline bool backSkinFor(BackSkin mode, bool debris, bool openSheet) {
    switch (mode) {
        case BackSkin::Off:    return false;
        case BackSkin::Debris: return debris;
        case BackSkin::All:    return true;
        default:               return openSheet || debris;
    }
}

} // namespace aver::softbody
