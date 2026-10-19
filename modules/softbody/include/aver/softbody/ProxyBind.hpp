// Binding a render mesh to a coarse proxy cage. A mesh over the particle cap simulates a cooked, simplified copy of
// itself (the proxy); every render vertex follows it by plain closest-point barycentric coordinates on one proxy
// triangle plus a signed offset along the smooth proxy normal. Deliberately NOT mean-value, harmonic or Green
// coordinates (the harmonic family is patented: docs/SOFTBODY.md 2.19). Evaluation is on demand against whichever
// triangle a piece owns, so a tear never stretches a render triangle across a crack. Pure arithmetic, Core only.
#pragma once

#include "aver/softbody/SimMesh.hpp"

#include <string>
#include <vector>

namespace aver::softbody {

struct ProxyBind {
    std::vector<u32> homeTri;       // per render vertex: the proxy (cage) triangle it is closest to
    std::vector<f32> bary;          // 3 per render vertex, relative to homeTri's corners v0[0..2] (plane projection, may extrapolate)
    std::vector<f32> offset;        // mesh-local cm along the smooth proxy normal at rest (signed)
    std::vector<u32> triTri;        // per render triangle: the proxy triangle nearest its centroid
    f32 maxDistanceCm = 0.0f;       // the bound the bind was checked against
    f32 offsetScale = 1.0f;         // the entity's uniform scale: offsets are mesh-local, the cage is in world space
};

// `render` is the authored mesh (mesh-local cm); `proxyCage` a built cage of the proxy mesh whose particles carry
// world positions, `bindLocal` the same particles in mesh-local cm (SimMesh::bindLocal). maxDistanceCm <= 0 means 5%
// of the render mesh's bounding diagonal. False with the first offending vertex in `why` when one is farther than
// that from every proxy triangle.
bool bindRenderToProxy(const SimMeshInput& render, const Cage& proxyCage, const std::vector<Vec3>& bindLocal,
                       f32 maxDistanceCm, ProxyBind& out, std::string* why = nullptr);

// Per-particle smooth normals from the live (not dead) triangles at the particles' current positions, area weighted.
void smoothParticleNormals(const Cage& cage, std::vector<Vec3>& out);

// Position (cage space) of a render vertex evaluated against ANY proxy triangle `tri`: against the home triangle the
// stored coordinates are used directly, against another one the vertex's rest position is re-expressed in that
// triangle (barycentric coordinates extrapolated when it lies outside) so evaluation is continuous across a shared
// edge. `smoothNormal` (optional) is smoothParticleNormals' output; null uses the triangle's own normal.
Vec3 proxyEvaluate(const ProxyBind& bind, u32 renderVertex, u32 tri, const Cage& cage, const Vec3* smoothNormal);

} // namespace aver::softbody
