// Density splat + marching tetrahedra. See FluidSurface.hpp.
#include "aver/fluids/FluidSurface.hpp"

#include <algorithm>
#include <cmath>

namespace aver::fluids {
namespace {

// Kuhn split of a cube (corner bit 0 = +x, 1 = +y, 2 = +z) into six tets sharing the 0-7 diagonal.
constexpr int kTets[6][4] = {{0, 1, 3, 7}, {0, 1, 5, 7}, {0, 2, 3, 7}, {0, 2, 6, 7}, {0, 4, 5, 7}, {0, 4, 6, 7}};

inline f32 splatWeight(f32 r2, f32 R2) {
    if (r2 >= R2) return 0.0f;
    const f32 t = 1.0f - r2 / R2;
    return t * t * t;
}

} // namespace

u32 FluidSurfaceMesher::surfaceBudget(const PbfBounds& bounds, f32 spacingM) {
    const f32 cell = 0.6f * std::max(spacingM, 1.0e-3f);   // configure()'s starting cell
    f64 n[3];
    for (int a = 0; a < 3; ++a) n[a] = std::max(1.0, std::ceil(static_cast<f64>(bounds.hi[a] - bounds.lo[a]) / cell));
    const f64 tris = 16.0 * (n[0] * n[1] + n[0] * n[2] + n[1] * n[2]);
    return static_cast<u32>(std::min(tris, 4.0e9));
}

void FluidSurfaceMesher::configure(const PbfBounds& bounds, f32 spacingM, u32 maxTris) {
    bounds_ = bounds;
    const f32 sp = std::max(spacingM, 1.0e-3f);
    cell_ = 0.6f * sp;
    splatR_ = 1.6f * sp;
    maxTris_ = maxTris;

    // Iso level: a fraction of the field an interior particle sees in a perfect lattice.
    f32 interior = 0.0f;
    const i32 reach = static_cast<i32>(std::ceil(splatR_ / sp));
    for (i32 k = -reach; k <= reach; ++k)
        for (i32 j = -reach; j <= reach; ++j)
            for (i32 i = -reach; i <= reach; ++i)
                interior += splatWeight(sp * sp * static_cast<f32>(i * i + j * j + k * k), splatR_ * splatR_);
    iso_ = 0.4f * interior;

    for (int guard = 0; guard < 64; ++guard) {
        const f32 pad = splatR_ + 2.0f * cell_;
        u64 total = 1;
        for (int a = 0; a < 3; ++a) {
            origin_[a] = bounds.lo[a] - pad;
            dim_[a] = std::max(2, static_cast<i32>(std::ceil((bounds.hi[a] - bounds.lo[a] + 2.0f * pad) / cell_)) + 1);
            total *= static_cast<u64>(dim_[a]);
        }
        if (total <= 3000000u) break;
        cell_ *= 1.25f;
    }
    field_.assign(static_cast<usize>(dim_[0]) * dim_[1] * dim_[2], 0.0f);
    pos_.assign(static_cast<usize>(maxTris_) * 9, 0.0f);
    nrm_.assign(static_cast<usize>(maxTris_) * 9, 0.0f);
    tris_ = 0;
    truncated_ = false;
}

void FluidSurfaceMesher::splat(const f32 p[3]) {
    const f32 R2 = splatR_ * splatR_;
    i32 lo[3], hi[3];
    for (int a = 0; a < 3; ++a) {
        const f32 u = (p[a] - origin_[a]) / cell_;
        lo[a] = std::max(0, static_cast<i32>(std::floor(u - splatR_ / cell_)));
        hi[a] = std::min(dim_[a] - 1, static_cast<i32>(std::ceil(u + splatR_ / cell_)));
    }
    for (i32 z = lo[2]; z <= hi[2]; ++z) {
        const f32 dz = origin_[2] + z * cell_ - p[2];
        for (i32 y = lo[1]; y <= hi[1]; ++y) {
            const f32 dy = origin_[1] + y * cell_ - p[1];
            f32* row = &field_[(static_cast<usize>(z) * dim_[1] + y) * dim_[0]];
            for (i32 x = lo[0]; x <= hi[0]; ++x) {
                const f32 dx = origin_[0] + x * cell_ - p[0];
                row[x] += splatWeight(dx * dx + dy * dy + dz * dz, R2);
            }
        }
    }
}

void FluidSurfaceMesher::cornerNormal(i32 x, i32 y, i32 z, f32 out[3]) const {
    auto f = [&](i32 a, i32 b, i32 c) {
        return at(std::clamp(a, 0, dim_[0] - 1), std::clamp(b, 0, dim_[1] - 1), std::clamp(c, 0, dim_[2] - 1));
    };
    // Outward = down the density gradient.
    const f32 g[3] = {f(x - 1, y, z) - f(x + 1, y, z), f(x, y - 1, z) - f(x, y + 1, z), f(x, y, z - 1) - f(x, y, z + 1)};
    const f32 len = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    if (len > 1.0e-9f) { out[0] = g[0] / len; out[1] = g[1] / len; out[2] = g[2] / len; }
    else { out[0] = 0.0f; out[1] = 0.0f; out[2] = 1.0f; }
}

FluidSurfaceMesher::Vert FluidSurfaceMesher::edgeVert(const i32 ca[3], f32 fa, const i32 cb[3], f32 fb) const {
    const f32 d = fb - fa;
    const f32 t = std::fabs(d) > 1.0e-12f ? std::clamp((iso_ - fa) / d, 0.0f, 1.0f) : 0.5f;
    f32 na[3], nb[3];
    cornerNormal(ca[0], ca[1], ca[2], na);
    cornerNormal(cb[0], cb[1], cb[2], nb);
    Vert v;
    for (int a = 0; a < 3; ++a) {
        const f32 pa = origin_[a] + ca[a] * cell_, pb = origin_[a] + cb[a] * cell_;
        v.p[a] = pa + (pb - pa) * t;
        v.n[a] = na[a] + (nb[a] - na[a]) * t;
    }
    const f32 len = std::sqrt(v.n[0] * v.n[0] + v.n[1] * v.n[1] + v.n[2] * v.n[2]);
    if (len > 1.0e-9f) { for (int a = 0; a < 3; ++a) v.n[a] /= len; }
    else { v.n[0] = 0.0f; v.n[1] = 0.0f; v.n[2] = 1.0f; }
    return v;
}

void FluidSurfaceMesher::pushTri(Vert a, Vert b, Vert c) {
    // Drop triangles wholly beyond a wall (the mirrored ghost half of the field).
    for (int ax = 0; ax < 3; ++ax) {
        if (bounds_.wallHi[ax] && a.p[ax] >= bounds_.hi[ax] && b.p[ax] >= bounds_.hi[ax] && c.p[ax] >= bounds_.hi[ax]) return;
        if (bounds_.wallLo[ax] && a.p[ax] <= bounds_.lo[ax] && b.p[ax] <= bounds_.lo[ax] && c.p[ax] <= bounds_.lo[ax]) return;
    }
    // Wind so (C-A) x (B-A) points along the field normal (the engine's front-face convention).
    const f32 e1[3] = {c.p[0] - a.p[0], c.p[1] - a.p[1], c.p[2] - a.p[2]};
    const f32 e2[3] = {b.p[0] - a.p[0], b.p[1] - a.p[1], b.p[2] - a.p[2]};
    const f32 gn[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    const f32 dot = gn[0] * (a.n[0] + b.n[0] + c.n[0]) + gn[1] * (a.n[1] + b.n[1] + c.n[1]) +
                    gn[2] * (a.n[2] + b.n[2] + c.n[2]);
    if (dot < 0.0f) std::swap(b, c);
    if (tris_ >= maxTris_) { truncated_ = true; return; }
    f32* P = &pos_[static_cast<usize>(tris_) * 9];
    f32* N = &nrm_[static_cast<usize>(tris_) * 9];
    const Vert* vs[3] = {&a, &b, &c};
    for (int k = 0; k < 3; ++k)
        for (int ax = 0; ax < 3; ++ax) { P[k * 3 + ax] = vs[k]->p[ax]; N[k * 3 + ax] = vs[k]->n[ax]; }
    ++tris_;
}

void FluidSurfaceMesher::tet(const i32 corner[4][3], const f32 value[4]) {
    int in[4], out[4], ni = 0, no = 0;
    for (int k = 0; k < 4; ++k) {
        if (value[k] >= iso_) in[ni++] = k;
        else out[no++] = k;
    }
    auto E = [&](int a, int b) { return edgeVert(corner[a], value[a], corner[b], value[b]); };
    if (ni == 1) {
        pushTri(E(in[0], out[0]), E(in[0], out[1]), E(in[0], out[2]));
    } else if (ni == 3) {
        pushTri(E(out[0], in[0]), E(out[0], in[1]), E(out[0], in[2]));
    } else if (ni == 2) {
        const Vert ac = E(in[0], out[0]), ad = E(in[0], out[1]), bd = E(in[1], out[1]), bc = E(in[1], out[0]);
        pushTri(ac, ad, bd);
        pushTri(ac, bd, bc);
    }
}

void FluidSurfaceMesher::build(const f32* posM, u32 n) {
    tris_ = 0;
    truncated_ = false;
    if (field_.empty()) return;
    std::fill(field_.begin(), field_.end(), 0.0f);
    for (u32 i = 0; i < n; ++i) {
        const f32 p[3] = {posM[i * 3], posM[i * 3 + 1], posM[i * 3 + 2]};
        splat(p);
        for (int a = 0; a < 3; ++a) {
            f32 q[3] = {p[0], p[1], p[2]};
            if (bounds_.wallLo[a] && p[a] - bounds_.lo[a] < splatR_) { q[a] = 2.0f * bounds_.lo[a] - p[a]; splat(q); }
            q[a] = p[a];
            if (bounds_.wallHi[a] && bounds_.hi[a] - p[a] < splatR_) { q[a] = 2.0f * bounds_.hi[a] - p[a]; splat(q); }
        }
    }

    for (i32 z = 0; z + 1 < dim_[2]; ++z)
        for (i32 y = 0; y + 1 < dim_[1]; ++y)
            for (i32 x = 0; x + 1 < dim_[0]; ++x) {
                f32 v[8];
                f32 mn = 1.0e30f, mx = -1.0e30f;
                for (int c = 0; c < 8; ++c) {
                    v[c] = at(x + (c & 1), y + ((c >> 1) & 1), z + ((c >> 2) & 1));
                    mn = std::min(mn, v[c]);
                    mx = std::max(mx, v[c]);
                }
                if (mx < iso_ || mn >= iso_) continue;
                for (const auto& t : kTets) {
                    i32 corner[4][3];
                    f32 value[4];
                    for (int k = 0; k < 4; ++k) {
                        const int c = t[k];
                        corner[k][0] = x + (c & 1);
                        corner[k][1] = y + ((c >> 1) & 1);
                        corner[k][2] = z + ((c >> 2) & 1);
                        value[k] = v[c];
                    }
                    tet(corner, value);
                }
            }
}

} // namespace aver::fluids
