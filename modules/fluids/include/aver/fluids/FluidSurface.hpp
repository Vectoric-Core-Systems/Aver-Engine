#pragma once
// The renderable surface of a particle fluid: particles are splatted into a scalar density grid and
// the iso-surface is extracted with marching tetrahedra (6 tets per cell along the cell diagonal, so
// neighbouring cells agree on every shared face). Normals come from the field gradient. Output is a
// triangle SOUP -- 3 vertices per triangle, nothing shared -- because the render path the old shell
// used (createSkinTargetMesh) has a fixed vertex and index count: the host makes an identity index
// buffer once and fills the used prefix each step. Pure CPU arithmetic, no device.
//
// Walls: a face of the volume that is a wall mirrors the particles near it into the field, so the
// water meets the wall flat instead of rounding off, and triangles wholly beyond a wall are dropped.
#include "aver/core/Types.hpp"
#include "aver/fluids/PbfFluid.hpp"

#include <vector>

namespace aver::fluids {

class FluidSurfaceMesher {
public:
    // `bounds` is the volume's box (local metres), `spacingM` the particle spacing, `maxTris` the most
    // triangles kept (extra ones are dropped and truncated() reports it).
    void configure(const PbfBounds& bounds, f32 spacingM, u32 maxTris);

    // Triangles a surface in this box may need: a sheet across each of the box's three cross-sections, about eight
    // triangles a mesher cell (a plane cuts four to six of a cell's tetrahedra), doubled for waves and splashes.
    // Surface area, not particle count, sets it: a wide shallow pool has far more surface per particle than a tank.
    static u32 surfaceBudget(const PbfBounds& bounds, f32 spacingM);

    // Rebuilds the mesh from xyz-interleaved local-metre positions.
    void build(const f32* posM, u32 n);

    u32  triangleCount() const { return tris_; }
    bool truncated() const { return truncated_; }
    // 9 floats per triangle (3 xyz vertices), local metres; normals are the same layout.
    const std::vector<f32>& positions() const { return pos_; }
    const std::vector<f32>& normals() const { return nrm_; }

private:
    struct Vert { f32 p[3]; f32 n[3]; };

    void splat(const f32 p[3]);
    void cornerNormal(i32 x, i32 y, i32 z, f32 out[3]) const;
    Vert edgeVert(const i32 ca[3], f32 fa, const i32 cb[3], f32 fb) const;
    void pushTri(Vert a, Vert b, Vert c);
    void tet(const i32 corner[4][3], const f32 value[4]);

    PbfBounds bounds_;
    f32 origin_[3] = {0, 0, 0};
    f32 cell_ = 0.1f, splatR_ = 0.2f, iso_ = 0.5f;
    i32 dim_[3] = {0, 0, 0};
    u32 maxTris_ = 0, tris_ = 0;
    bool truncated_ = false;
    std::vector<f32> field_, pos_, nrm_;

    f32 at(i32 x, i32 y, i32 z) const {
        return field_[(static_cast<usize>(z) * dim_[1] + y) * dim_[0] + x];
    }
};

} // namespace aver::fluids
