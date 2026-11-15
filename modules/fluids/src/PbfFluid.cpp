// Position-based fluid solver and the static-collider distance field. See PbfFluid.hpp.
#include "aver/fluids/PbfFluid.hpp"

#include <algorithm>
#include <cmath>

namespace aver::fluids {
namespace {

constexpr f32 kPi = 3.14159265358979f;
constexpr f32 kFar = 1.0e9f;

// Two-pass 3x3x3 chamfer distance transform over a dim^3 grid of nodes spaced `cell` apart. `d` holds 0
// at sources and a large value elsewhere on entry; distance to the nearest source on exit.
void chamfer(std::vector<f32>& d, const i32 dim[3], f32 cell) {
    struct Off { i32 x, y, z; f32 w; };
    std::vector<Off> fwd, bwd;
    for (i32 z = -1; z <= 1; ++z)
        for (i32 y = -1; y <= 1; ++y)
            for (i32 x = -1; x <= 1; ++x) {
                if (x == 0 && y == 0 && z == 0) continue;
                const Off o{x, y, z, cell * std::sqrt(static_cast<f32>(x * x + y * y + z * z))};
                const bool earlier = z < 0 || (z == 0 && (y < 0 || (y == 0 && x < 0)));
                (earlier ? fwd : bwd).push_back(o);
            }
    auto idx = [&](i32 x, i32 y, i32 z) { return (static_cast<usize>(z) * dim[1] + y) * dim[0] + x; };
    auto relax = [&](i32 x, i32 y, i32 z, const std::vector<Off>& offs) {
        f32& v = d[idx(x, y, z)];
        for (const Off& o : offs) {
            const i32 nx = x + o.x, ny = y + o.y, nz = z + o.z;
            if (nx < 0 || ny < 0 || nz < 0 || nx >= dim[0] || ny >= dim[1] || nz >= dim[2]) continue;
            v = std::min(v, d[idx(nx, ny, nz)] + o.w);
        }
    };
    for (i32 z = 0; z < dim[2]; ++z)
        for (i32 y = 0; y < dim[1]; ++y)
            for (i32 x = 0; x < dim[0]; ++x) relax(x, y, z, fwd);
    for (i32 z = dim[2] - 1; z >= 0; --z)
        for (i32 y = dim[1] - 1; y >= 0; --y)
            for (i32 x = dim[0] - 1; x >= 0; --x) relax(x, y, z, bwd);
}

inline u32 cellHash(i32 x, i32 y, i32 z, u32 mask) {
    return (static_cast<u32>(x) * 73856093u ^ static_cast<u32>(y) * 19349663u ^
            static_cast<u32>(z) * 83492791u) & mask;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// SolidField

void SolidField::build(const f32 lo[3], const f32 hi[3], f32 cellM, const OverlapFn& overlap, u32 maxCells) {
    phi_.clear();
    solidCells_ = 0;
    if (!overlap) return;
    cell_ = std::max(cellM, 1.0e-3f);
    for (int guard = 0; guard < 64; ++guard) {
        u64 total = 1;
        for (int a = 0; a < 3; ++a) {
            dim_[a] = std::max(2, static_cast<i32>(std::ceil((hi[a] - lo[a]) / cell_)) + 1);
            total *= static_cast<u64>(dim_[a]);
        }
        if (total <= maxCells) break;
        cell_ *= 1.25f;
    }
    for (int a = 0; a < 3; ++a) origin_[a] = lo[a];

    const usize total = static_cast<usize>(dim_[0]) * dim_[1] * dim_[2];
    std::vector<u8> solid(total, 0);
    auto idx = [&](i32 x, i32 y, i32 z) { return (static_cast<usize>(z) * dim_[1] + y) * dim_[0] + x; };

    constexpr i32 kBlock = 4;
    for (i32 bz = 0; bz < dim_[2]; bz += kBlock)
        for (i32 by = 0; by < dim_[1]; by += kBlock)
            for (i32 bx = 0; bx < dim_[0]; bx += kBlock) {
                const i32 ex = std::min(bx + kBlock, dim_[0]) - bx;
                const i32 ey = std::min(by + kBlock, dim_[1]) - by;
                const i32 ez = std::min(bz + kBlock, dim_[2]) - bz;
                const f32 bc[3] = {origin_[0] + (bx + 0.5f * (ex - 1)) * cell_,
                                   origin_[1] + (by + 0.5f * (ey - 1)) * cell_,
                                   origin_[2] + (bz + 0.5f * (ez - 1)) * cell_};
                const f32 br = 0.5f * cell_ * std::sqrt(static_cast<f32>(ex * ex + ey * ey + ez * ez));
                if (!overlap(bc, br)) continue;
                for (i32 z = bz; z < bz + ez; ++z)
                    for (i32 y = by; y < by + ey; ++y)
                        for (i32 x = bx; x < bx + ex; ++x) {
                            const f32 c[3] = {origin_[0] + x * cell_, origin_[1] + y * cell_,
                                              origin_[2] + z * cell_};
                            if (overlap(c, 0.5f * cell_)) {
                                solid[idx(x, y, z)] = 1;
                                ++solidCells_;
                            }
                        }
            }
    if (solidCells_ == 0) return;

    std::vector<f32> dOut(total), dIn(total);
    for (usize i = 0; i < total; ++i) {
        dOut[i] = solid[i] ? 0.0f : kFar;
        dIn[i]  = solid[i] ? kFar : 0.0f;
    }
    chamfer(dOut, dim_, cell_);
    chamfer(dIn, dim_, cell_);
    phi_.resize(total);
    for (usize i = 0; i < total; ++i)
        phi_[i] = solid[i] ? std::max(0.5f * cell_ - dIn[i], -100.0f) : dOut[i] - 0.5f * cell_;
}

f32 SolidField::at(i32 x, i32 y, i32 z) const {
    return phi_[(static_cast<usize>(z) * dim_[1] + y) * dim_[0] + x];
}

f32 SolidField::sample(const f32 p[3], f32* outGrad) const {
    if (outGrad) outGrad[0] = outGrad[1] = outGrad[2] = 0.0f;
    if (phi_.empty()) return kFar;
    f32 u[3];
    i32 i0[3];
    for (int a = 0; a < 3; ++a) {
        u[a] = (p[a] - origin_[a]) / cell_;
        if (!(u[a] >= 0.0f) || u[a] >= static_cast<f32>(dim_[a] - 1)) return kFar;
        i0[a] = static_cast<i32>(u[a]);
        u[a] -= static_cast<f32>(i0[a]);
    }
    f32 c[2][2][2];
    for (int dz = 0; dz < 2; ++dz)
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) c[dz][dy][dx] = at(i0[0] + dx, i0[1] + dy, i0[2] + dz);
    auto lerp = [](f32 a, f32 b, f32 t) { return a + (b - a) * t; };
    const f32 fx = u[0], fy = u[1], fz = u[2];
    auto planeX = [&](int dx) {   // value on the x = dx face, bilinear in (y, z)
        return lerp(lerp(c[0][0][dx], c[0][1][dx], fy), lerp(c[1][0][dx], c[1][1][dx], fy), fz);
    };
    const f32 vx0 = planeX(0), vx1 = planeX(1);
    const f32 value = lerp(vx0, vx1, fx);
    if (outGrad) {
        auto planeY = [&](int dy) {
            return lerp(lerp(c[0][dy][0], c[0][dy][1], fx), lerp(c[1][dy][0], c[1][dy][1], fx), fz);
        };
        auto planeZ = [&](int dz) {
            return lerp(lerp(c[dz][0][0], c[dz][0][1], fx), lerp(c[dz][1][0], c[dz][1][1], fx), fy);
        };
        const f32 g[3] = {vx1 - vx0, planeY(1) - planeY(0), planeZ(1) - planeZ(0)};
        const f32 len = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        if (len > 1.0e-6f) for (int a = 0; a < 3; ++a) outGrad[a] = g[a] / len;
    }
    return value;
}

// ---------------------------------------------------------------------------------------------
// PbfFluid

f32 PbfFluid::kernelW(f32 r2) const {
    const f32 d = h2_ - r2;
    return poly6C_ * d * d * d;
}

u32 PbfFluid::init(const PbfSettings& s, const SolidField* solid) {
    s_ = s;
    solid_ = (solid && !solid->empty()) ? solid : nullptr;
    n_ = 0;
    steps_ = 0;
    accum_ = 0.0f;
    pos_.clear(); vel_.clear();

    f32 fLo[3], ext[3];
    for (int a = 0; a < 3; ++a) {
        fLo[a] = s_.useFill ? std::max(s_.fillLo[a], s_.bounds.lo[a]) : s_.bounds.lo[a];
        const f32 fHi = s_.useFill ? std::min(s_.fillHi[a], s_.bounds.hi[a]) : s_.bounds.hi[a];
        ext[a] = fHi - fLo[a];
    }
    if (!(ext[0] > 0.0f && ext[1] > 0.0f && ext[2] > 0.0f)) return 0;

    f32 spacing = std::max(s_.spacingM, 1.0e-3f);
    i32 nc[3] = {1, 1, 1};
    for (int guard = 0; guard < 200; ++guard) {
        u64 total = 1;
        for (int a = 0; a < 3; ++a) {
            nc[a] = std::max(1, static_cast<i32>(std::floor(ext[a] / spacing + 1.0e-3f)));
            total *= static_cast<u64>(nc[a]);
        }
        if (total <= std::max(1u, s_.maxParticles)) break;
        spacing *= 1.05f;
    }
    const f32 step[3] = {ext[0] / nc[0], ext[1] / nc[1], ext[2] / nc[2]};
    spacing_ = std::cbrt(step[0] * step[1] * step[2]);
    h_ = 2.0f * spacing_;
    h2_ = h_ * h_;
    radius_ = 0.5f * std::min(step[0], std::min(step[1], step[2]));
    mass_ = kPbfRestDensity * step[0] * step[1] * step[2];
    poly6C_ = 315.0f / (64.0f * kPi * std::pow(h_, 9.0f));
    spikyC_ = -45.0f / (kPi * std::pow(h_, 6.0f));

    // Rest state: sum the kernels over an unbounded lattice with this fill's own steps, so a settled
    // interior particle sits at C = 0 exactly rather than at whatever mass/volume gives.
    f32 wSum = 0.0f, gradSq = 0.0f;
    const i32 reach[3] = {static_cast<i32>(std::ceil(h_ / step[0])), static_cast<i32>(std::ceil(h_ / step[1])),
                          static_cast<i32>(std::ceil(h_ / step[2]))};
    for (i32 k = -reach[2]; k <= reach[2]; ++k)
        for (i32 j = -reach[1]; j <= reach[1]; ++j)
            for (i32 i = -reach[0]; i <= reach[0]; ++i) {
                const f32 r2 = (i * step[0]) * (i * step[0]) + (j * step[1]) * (j * step[1]) +
                               (k * step[2]) * (k * step[2]);
                if (r2 >= h2_) continue;
                wSum += kernelW(r2);
                if (r2 > 0.0f) {
                    const f32 g = spikyC_ * (h_ - std::sqrt(r2)) * (h_ - std::sqrt(r2));
                    gradSq += g * g;
                }
            }
    rho0_ = mass_ * wSum;
    mOverRho0_ = mass_ / rho0_;
    const f32 denomRef = std::max(mOverRho0_ * mOverRho0_ * gradSq, 1.0e-12f);
    eps_ = s_.relaxation * denomRef;
    lambdaRef_ = 1.0f / denomRef;
    wq_ = kernelW((0.2f * h_) * (0.2f * h_));

    // A wall is a mirrored lattice of ghost particles behind it (the fill's own half-step offset), so a
    // particle at the floor sees the density of a full neighbourhood and does not over-compress to
    // make up the missing half. Tabulated against the particle's distance to the wall surface.
    wallW_.assign(kWallTable + 1, 0.0f);
    wallDW_.assign(kWallTable + 1, 0.0f);
    {
        const f32 st = spacing_;
        const i32 reachW = static_cast<i32>(std::ceil(h_ / st)) + 1;
        for (u32 t = 0; t <= kWallTable; ++t) {
            const f32 d = h_ * static_cast<f32>(t) / static_cast<f32>(kWallTable);
            f32 w = 0.0f, dw = 0.0f;
            for (i32 m = 0; m <= reachW; ++m) {
                const f32 dz = d + (0.5f + static_cast<f32>(m)) * st;
                for (i32 j = -reachW; j <= reachW; ++j)
                    for (i32 i = -reachW; i <= reachW; ++i) {
                        const f32 r2 = (i * st) * (i * st) + (j * st) * (j * st) + dz * dz;
                        if (r2 >= h2_) continue;
                        const f32 q = h2_ - r2;
                        w += poly6C_ * q * q * q;
                        dw += -6.0f * poly6C_ * q * q * dz;
                    }
            }
            wallW_[t] = w;
            wallDW_[t] = dw;
        }
    }

    pos_.reserve(static_cast<usize>(nc[0]) * nc[1] * nc[2] * 3);
    for (i32 k = 0; k < nc[2]; ++k)
        for (i32 j = 0; j < nc[1]; ++j)
            for (i32 i = 0; i < nc[0]; ++i) {
                const f32 p[3] = {fLo[0] + (i + 0.5f) * step[0],
                                  fLo[1] + (j + 0.5f) * step[1],
                                  fLo[2] + (k + 0.5f) * step[2]};
                if (solid_ && solid_->sample(p) < radius_) continue;   // inside geometry: no water there
                pos_.push_back(p[0]); pos_.push_back(p[1]); pos_.push_back(p[2]);
            }
    n_ = static_cast<u32>(pos_.size() / 3);
    vel_.assign(pos_.size(), 0.0f);
    pred_.assign(pos_.size(), 0.0f);
    delta_.assign(pos_.size(), 0.0f);
    omega_.assign(pos_.size(), 0.0f);
    lambda_.assign(n_, 0.0f);
    return n_;
}

u32 PbfFluid::advance(f32 dtSeconds) {
    if (!(dtSeconds > 0.0f)) return 0;
    accum_ += std::min(dtSeconds, 1.0f);
    u32 steps = 0;
    while (accum_ >= kPbfFixedDt && steps < kPbfMaxStepsPerAdvance) {
        step(kPbfFixedDt);
        accum_ -= kPbfFixedDt;
        ++steps;
    }
    if (accum_ >= kPbfFixedDt) accum_ = 0.0f;   // behind: drop the backlog
    return steps;
}

void PbfFluid::addImpulse(const f32 c[3], f32 radiusM, const f32 dv[3]) {
    if (!(radiusM > 0.0f)) return;
    for (u32 i = 0; i < n_; ++i) {
        const f32 dx = pos_[i * 3] - c[0], dy = pos_[i * 3 + 1] - c[1], dz = pos_[i * 3 + 2] - c[2];
        const f32 d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d >= radiusM) continue;
        const f32 w = 1.0f - d / radiusM;
        for (int a = 0; a < 3; ++a) vel_[i * 3 + a] += dv[a] * w;
    }
}

void PbfFluid::collide(u32 /*i*/, f32* p) const {
    const PbfBounds& b = s_.bounds;
    for (int a = 0; a < 3; ++a) {
        if (b.wallLo[a] && p[a] < b.lo[a] + radius_) p[a] = b.lo[a] + radius_;
        if (b.wallHi[a] && p[a] > b.hi[a] - radius_) p[a] = b.hi[a] - radius_;
    }
    if (solid_) {
        f32 g[3];
        const f32 phi = solid_->sample(p, g);
        if (phi < radius_) {
            const f32 push = radius_ - phi;
            for (int a = 0; a < 3; ++a) p[a] += g[a] * push;
        }
    }
}

f32 PbfFluid::wallTerm(const f32* p, f32 outGrad[3]) const {
    outGrad[0] = outGrad[1] = outGrad[2] = 0.0f;
    f32 rho = 0.0f;
    auto add = [&](f32 d, const f32 n[3]) {
        if (!(d < h_)) return;
        const f32 u = std::max(d, 0.0f) / h_ * static_cast<f32>(kWallTable);
        const u32 k = std::min(static_cast<u32>(u), kWallTable - 1);
        const f32 t = u - static_cast<f32>(k);
        rho += wallW_[k] + (wallW_[k + 1] - wallW_[k]) * t;
        const f32 dw = wallDW_[k] + (wallDW_[k + 1] - wallDW_[k]) * t;
        for (int a = 0; a < 3; ++a) outGrad[a] += dw * n[a];
    };
    const PbfBounds& b = s_.bounds;
    for (int a = 0; a < 3; ++a) {
        f32 n[3] = {0, 0, 0};
        if (b.wallLo[a]) { n[a] = 1.0f;  add(p[a] - b.lo[a], n); }
        if (b.wallHi[a]) { n[a] = -1.0f; add(b.hi[a] - p[a], n); }
    }
    if (solid_) {
        f32 g[3];
        const f32 phi = solid_->sample(p, g);
        if (phi < h_) add(phi, g);
    }
    return rho;
}

void PbfFluid::buildNeighbours() {
    const f32 inv = 1.0f / h_;
    u32 table = 64;
    while (table < n_ * 2u) table <<= 1;
    const u32 mask = table - 1;

    cellKey_.resize(n_);
    cellStart_.assign(static_cast<usize>(table) + 1, 0u);
    for (u32 i = 0; i < n_; ++i) {
        const i32 cx = static_cast<i32>(std::floor(pred_[i * 3] * inv));
        const i32 cy = static_cast<i32>(std::floor(pred_[i * 3 + 1] * inv));
        const i32 cz = static_cast<i32>(std::floor(pred_[i * 3 + 2] * inv));
        const u32 k = cellHash(cx, cy, cz, mask);
        cellKey_[i] = k;
        ++cellStart_[k + 1];
    }
    for (u32 k = 0; k < table; ++k) cellStart_[k + 1] += cellStart_[k];
    sorted_.resize(n_);
    {
        std::vector<u32> cursor(cellStart_.begin(), cellStart_.end() - 1);
        for (u32 i = 0; i < n_; ++i) sorted_[cursor[cellKey_[i]]++] = i;
    }

    nStart_.resize(static_cast<usize>(n_) + 1);
    nIdx_.clear();
    nIdx_.reserve(static_cast<usize>(n_) * 36);
    for (u32 i = 0; i < n_; ++i) {
        nStart_[i] = static_cast<u32>(nIdx_.size());
        const f32* pi = &pred_[i * 3];
        const i32 cx = static_cast<i32>(std::floor(pi[0] * inv));
        const i32 cy = static_cast<i32>(std::floor(pi[1] * inv));
        const i32 cz = static_cast<i32>(std::floor(pi[2] * inv));
        u32 keys[27];
        u32 nk = 0;
        for (i32 dz = -1; dz <= 1; ++dz)
            for (i32 dy = -1; dy <= 1; ++dy)
                for (i32 dx = -1; dx <= 1; ++dx) {
                    const u32 k = cellHash(cx + dx, cy + dy, cz + dz, mask);
                    bool seen = false;
                    for (u32 q = 0; q < nk; ++q) if (keys[q] == k) { seen = true; break; }   // hash buckets can alias
                    if (!seen) keys[nk++] = k;
                }
        for (u32 q = 0; q < nk; ++q) {
            for (u32 s = cellStart_[keys[q]]; s < cellStart_[keys[q] + 1]; ++s) {
                const u32 j = sorted_[s];
                if (j == i) continue;
                const f32 dx = pi[0] - pred_[j * 3], dy = pi[1] - pred_[j * 3 + 1], dz = pi[2] - pred_[j * 3 + 2];
                if (dx * dx + dy * dy + dz * dz < h2_) nIdx_.push_back(j);
            }
        }
    }
    nStart_[n_] = static_cast<u32>(nIdx_.size());
}

void PbfFluid::step(f32 dt) {
    if (n_ == 0 || !(dt > 0.0f)) return;
    maxSpeed_ = 0.75f * h_ / dt;   // keeps a step under about one kernel radius

    for (u32 i = 0; i < n_; ++i) {
        f32* v = &vel_[i * 3];
        for (int a = 0; a < 3; ++a) v[a] += s_.gravity[a] * dt;
        const f32 sp2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        if (sp2 > maxSpeed_ * maxSpeed_) {
            const f32 k = maxSpeed_ / std::sqrt(sp2);
            for (int a = 0; a < 3; ++a) v[a] *= k;
        }
        for (int a = 0; a < 3; ++a) pred_[i * 3 + a] = pos_[i * 3 + a] + v[a] * dt;
        collide(i, &pred_[i * 3]);
    }

    buildNeighbours();

    const u32 iters = std::max(1u, s_.iterations);
    for (u32 it = 0; it < iters; ++it) {
        for (u32 i = 0; i < n_; ++i) {
            const f32* pi = &pred_[i * 3];
            f32 gs[3];
            f32 rho = mass_ * (kernelW(0.0f) + wallTerm(pi, gs));
            f32 sq = 0.0f;
            for (u32 q = nStart_[i]; q < nStart_[i + 1]; ++q) {
                const u32 j = nIdx_[q];
                const f32 d[3] = {pi[0] - pred_[j * 3], pi[1] - pred_[j * 3 + 1], pi[2] - pred_[j * 3 + 2]};
                const f32 r2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                if (r2 >= h2_) continue;
                rho += mass_ * kernelW(r2);
                if (r2 < 1.0e-12f) continue;
                const f32 r = std::sqrt(r2);
                const f32 coef = spikyC_ * (h_ - r) * (h_ - r) / r;
                for (int a = 0; a < 3; ++a) { const f32 g = coef * d[a]; gs[a] += g; sq += g * g; }
            }
            // Only compression is corrected: a free-surface particle has too few neighbours by
            // construction, and pulling it inward would clump the surface.
            const f32 c = std::max(rho / rho0_ - 1.0f, 0.0f);
            const f32 denom = mOverRho0_ * mOverRho0_ * (gs[0] * gs[0] + gs[1] * gs[1] + gs[2] * gs[2] + sq) + eps_;
            lambda_[i] = -c / denom;
        }
        for (u32 i = 0; i < n_; ++i) {
            const f32* pi = &pred_[i * 3];
            f32 wg[3];
            wallTerm(pi, wg);
            f32 dp[3] = {lambda_[i] * wg[0], lambda_[i] * wg[1], lambda_[i] * wg[2]};   // ghosts carry no lambda
            for (u32 q = nStart_[i]; q < nStart_[i + 1]; ++q) {
                const u32 j = nIdx_[q];
                const f32 d[3] = {pi[0] - pred_[j * 3], pi[1] - pred_[j * 3 + 1], pi[2] - pred_[j * 3 + 2]};
                const f32 r2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                if (r2 >= h2_ || r2 < 1.0e-12f) continue;
                const f32 r = std::sqrt(r2);
                const f32 coef = spikyC_ * (h_ - r) * (h_ - r) / r;
                const f32 ratio = kernelW(r2) / wq_;
                const f32 scorr = -s_.tensileK * lambdaRef_ * ratio * ratio * ratio * ratio;
                const f32 l = lambda_[i] + lambda_[j] + scorr;
                for (int a = 0; a < 3; ++a) dp[a] += l * coef * d[a];
            }
            for (int a = 0; a < 3; ++a) delta_[i * 3 + a] = mOverRho0_ * dp[a];
        }
        for (u32 i = 0; i < n_; ++i) {
            for (int a = 0; a < 3; ++a) pred_[i * 3 + a] += delta_[i * 3 + a];
            collide(i, &pred_[i * 3]);
        }
    }

    // Velocity from the position change, then wall friction on tangential motion.
    const f32 invDt = 1.0f / dt;
    const PbfBounds& b = s_.bounds;
    for (u32 i = 0; i < n_; ++i) {
        f32* v = &vel_[i * 3];
        for (int a = 0; a < 3; ++a) v[a] = (pred_[i * 3 + a] - pos_[i * 3 + a]) * invDt;
        const f32 sp2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        if (sp2 > maxSpeed_ * maxSpeed_) {
            const f32 k = maxSpeed_ / std::sqrt(sp2);
            for (int a = 0; a < 3; ++a) v[a] *= k;
        }
        for (int a = 0; a < 3; ++a) {
            const bool touchLo = b.wallLo[a] && pred_[i * 3 + a] <= b.lo[a] + radius_ * 1.01f;
            const bool touchHi = b.wallHi[a] && pred_[i * 3 + a] >= b.hi[a] - radius_ * 1.01f;
            if (!touchLo && !touchHi) continue;
            for (int t = 0; t < 3; ++t) if (t != a) v[t] *= 1.0f - s_.wallFriction;
            if (s_.restitution > 0.0f && ((touchLo && v[a] < 0.0f) || (touchHi && v[a] > 0.0f))) v[a] *= -s_.restitution;
        }
    }

    // XSPH viscosity (and optional vorticity confinement) from the settled neighbour set.
    if (s_.vorticity > 0.0f) {
        for (u32 i = 0; i < n_; ++i) {
            f32 w[3] = {0, 0, 0};
            const f32* pi = &pred_[i * 3];
            for (u32 q = nStart_[i]; q < nStart_[i + 1]; ++q) {
                const u32 j = nIdx_[q];
                const f32 d[3] = {pi[0] - pred_[j * 3], pi[1] - pred_[j * 3 + 1], pi[2] - pred_[j * 3 + 2]};
                const f32 r2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                if (r2 >= h2_ || r2 < 1.0e-12f) continue;
                const f32 r = std::sqrt(r2);
                const f32 coef = spikyC_ * (h_ - r) * (h_ - r) / r;
                const f32 g[3] = {coef * d[0], coef * d[1], coef * d[2]};
                const f32 vij[3] = {vel_[j * 3] - vel_[i * 3], vel_[j * 3 + 1] - vel_[i * 3 + 1],
                                    vel_[j * 3 + 2] - vel_[i * 3 + 2]};
                w[0] += vij[1] * g[2] - vij[2] * g[1];
                w[1] += vij[2] * g[0] - vij[0] * g[2];
                w[2] += vij[0] * g[1] - vij[1] * g[0];
            }
            for (int a = 0; a < 3; ++a) omega_[i * 3 + a] = w[a] * mOverRho0_;
            lambda_[i] = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) * mOverRho0_;   // |omega|; lambda is spent
        }
    }
    for (u32 i = 0; i < n_; ++i) {
        const f32* pi = &pred_[i * 3];
        f32 acc[3] = {0, 0, 0};
        f32 eta[3] = {0, 0, 0};
        for (u32 q = nStart_[i]; q < nStart_[i + 1]; ++q) {
            const u32 j = nIdx_[q];
            const f32 d[3] = {pi[0] - pred_[j * 3], pi[1] - pred_[j * 3 + 1], pi[2] - pred_[j * 3 + 2]};
            const f32 r2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            if (r2 >= h2_) continue;
            const f32 w = kernelW(r2);
            for (int a = 0; a < 3; ++a) acc[a] += (vel_[j * 3 + a] - vel_[i * 3 + a]) * w;
            if (s_.vorticity > 0.0f && r2 > 1.0e-12f) {
                const f32 r = std::sqrt(r2);
                const f32 coef = spikyC_ * (h_ - r) * (h_ - r) / r;
                for (int a = 0; a < 3; ++a) eta[a] += (lambda_[j] - lambda_[i]) * coef * d[a];
            }
        }
        for (int a = 0; a < 3; ++a) delta_[i * 3 + a] = vel_[i * 3 + a] + s_.xsph * mOverRho0_ * acc[a];
        if (s_.vorticity > 0.0f) {
            const f32 el = std::sqrt(eta[0] * eta[0] + eta[1] * eta[1] + eta[2] * eta[2]);
            if (el > 1.0e-9f) {
                const f32 nrm[3] = {eta[0] / el, eta[1] / el, eta[2] / el};
                const f32* om = &omega_[i * 3];
                const f32 f[3] = {nrm[1] * om[2] - nrm[2] * om[1], nrm[2] * om[0] - nrm[0] * om[2],
                                  nrm[0] * om[1] - nrm[1] * om[0]};
                for (int a = 0; a < 3; ++a) delta_[i * 3 + a] += s_.vorticity * dt * f[a];
            }
        }
    }
    vel_.swap(delta_);
    pos_.swap(pred_);
    ++steps_;
}

} // namespace aver::fluids
