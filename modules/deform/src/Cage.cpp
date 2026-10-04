// Cage generation, binding and the deform itself.
#include "aver/deform/Cage.hpp"

#include <algorithm>
#include <cmath>

namespace aver::deform {

namespace {

// Mean distance from each node to its nearest neighbour: the natural scale for an influence radius,
// and it adapts to a cage that is dense in some places and sparse in others.
f32 meanNodeSpacing(const Cage& cage) {
    const usize n = cage.nodes.size();
    if (n < 2) return 1.0f;
    f64 total = 0.0;
    for (usize i = 0; i < n; ++i) {
        f32 best = 1e30f;
        for (usize j = 0; j < n; ++j) {
            if (i == j) continue;
            const f32 d = (cage.nodes[i].rest - cage.nodes[j].rest).sizeSquared();
            if (d < best) best = d;
        }
        total += std::sqrt(best);
    }
    const f32 mean = static_cast<f32>(total / static_cast<f64>(n));
    return mean > 1e-6f ? mean : 1.0f;
}

} // namespace

bool CageBinding::valid() const {
    if (nodeIndex.size() != weight.size()) return false;
    if (nodeIndex.size() % kCageInfluences != 0) return false;
    for (u32 v = 0; v < vertexCount(); ++v) {
        f32 sum = 0.0f;
        for (u32 k = 0; k < kCageInfluences; ++k) sum += weight[v * kCageInfluences + k];
        if (std::fabs(sum - 1.0f) > 1e-3f) return false;
    }
    return true;
}

void generateSubBoneCage(const fmt::OcSkeleton& skel, const CageOptions& opt, Cage& out) {
    out.nodes.clear();
    if (skel.bones.empty()) return;

    anim::Pose rest;
    anim::restPose(skel, rest);
    std::vector<Mat4> model;
    anim::poseToModel(skel, rest, model);

    auto jointOf = [&](usize b) {
        return Vec3{model[b].m[3][0], model[b].m[3][1], model[b].m[3][2]};
    };

    for (usize b = 0; b < skel.bones.size(); ++b) {
        const Vec3 here = jointOf(b);
        const i32 parent = skel.bones[b].parent;

        // A root has no segment to subdivide, so it contributes its joint alone.
        if (parent < 0 || static_cast<usize>(parent) >= model.size()) {
            out.nodes.push_back(CageNode{here, static_cast<i32>(b), 1.0f});
            continue;
        }

        // Nodes are placed along the segment EXCLUDING the parent's own joint, which the parent
        // already contributed -- otherwise every interior joint would get a duplicate node.
        const Vec3 from = jointOf(static_cast<usize>(parent));
        const u32 steps = opt.subdivisionsPerBone + 1;
        for (u32 s = 1; s <= steps; ++s) {
            const f32 t = static_cast<f32>(s) / static_cast<f32>(steps);
            out.nodes.push_back(CageNode{from + (here - from) * t, static_cast<i32>(b), t});
        }
    }
}

void generateLatticeCage(const Vec3& boundsMin, const Vec3& boundsMax, const CageOptions& opt, Cage& out) {
    out.nodes.clear();
    const u32 nx = opt.latticeX > 1 ? opt.latticeX : 2;
    const u32 ny = opt.latticeY > 1 ? opt.latticeY : 2;
    const u32 nz = opt.latticeZ > 1 ? opt.latticeZ : 2;

    const Vec3 extent = boundsMax - boundsMin;
    const Vec3 pad = extent * opt.latticePadding;
    const Vec3 lo = boundsMin - pad;
    const Vec3 hi = boundsMax + pad;

    out.nodes.reserve(static_cast<usize>(nx) * ny * nz);
    for (u32 iz = 0; iz < nz; ++iz)
        for (u32 iy = 0; iy < ny; ++iy)
            for (u32 ix = 0; ix < nx; ++ix) {
                const f32 fx = static_cast<f32>(ix) / static_cast<f32>(nx - 1);
                const f32 fy = static_cast<f32>(iy) / static_cast<f32>(ny - 1);
                const f32 fz = static_cast<f32>(iz) / static_cast<f32>(nz - 1);
                out.nodes.push_back(CageNode{
                    Vec3{lo.x + (hi.x - lo.x) * fx,
                         lo.y + (hi.y - lo.y) * fy,
                         lo.z + (hi.z - lo.z) * fz}, -1, 0.0f});
            }
}

void bindVerticesToCage(const Cage& cage, const f32* positions, u32 vertexCount,
                        const CageOptions& opt, CageBinding& out) {
    out.nodeIndex.assign(static_cast<usize>(vertexCount) * kCageInfluences, -1);
    out.weight.assign(static_cast<usize>(vertexCount) * kCageInfluences, 0.0f);
    const u32 n = cage.nodeCount();
    if (n == 0 || vertexCount == 0 || !positions) return;

    const f32 radius = meanNodeSpacing(cage) * (opt.influenceRadius > 0.0f ? opt.influenceRadius : 1.0f);

    std::vector<std::pair<f32, i32>> near;   // squared distance, node
    near.reserve(n);

    for (u32 v = 0; v < vertexCount; ++v) {
        const Vec3 p{positions[v * 3 + 0], positions[v * 3 + 1], positions[v * 3 + 2]};
        near.clear();
        for (u32 i = 0; i < n; ++i)
            near.push_back({(cage.nodes[i].rest - p).sizeSquared(), static_cast<i32>(i)});

        const u32 take = std::min<u32>(kCageInfluences, n);
        std::partial_sort(near.begin(), near.begin() + take, near.end(),
                          [](const auto& a, const auto& b) { return a.first < b.first; });

        i32* idx = &out.nodeIndex[static_cast<usize>(v) * kCageInfluences];
        f32* wt  = &out.weight[static_cast<usize>(v) * kCageInfluences];

        // A vertex sitting on a node belongs to that node alone; a falloff would divide by zero.
        if (near[0].first <= 1e-12f) {
            idx[0] = near[0].second;
            wt[0] = 1.0f;
            continue;
        }

        // Falloff that reaches zero at `radius`, so influence is LOCAL: a node moving does not drag
        // the whole mesh. Squared for a smooth first derivative across the cutoff.
        f32 sum = 0.0f;
        for (u32 k = 0; k < take; ++k) {
            const f32 d = std::sqrt(near[k].first);
            const f32 f = d < radius ? (1.0f - d / radius) : 0.0f;
            idx[k] = near[k].second;
            wt[k] = f * f;
            sum += wt[k];
        }

        // Out of the cage's reach entirely: bind to the nearest node at full weight rather than
        // leaving the vertex with no influence, which would tear it off a rigid motion.
        if (sum <= 1e-12f) {
            for (u32 k = 0; k < kCageInfluences; ++k) { idx[k] = -1; wt[k] = 0.0f; }
            idx[0] = near[0].second;
            wt[0] = 1.0f;
            continue;
        }
        // NORMALISED HERE, ONCE. The deform sums and never re-divides -- that is the contract the
        // GPU port has to reproduce byte for byte.
        const f32 inv = 1.0f / sum;
        for (u32 k = 0; k < take; ++k) wt[k] *= inv;
    }
}

void buildCageForMesh(const fmt::OcSkeleton* skel, const f32* positions, u32 vertexCount,
                      const CageOptions& opt, Cage& outCage, CageBinding& outBinding) {
    if (skel && !skel->bones.empty()) {
        generateSubBoneCage(*skel, opt, outCage);
    } else if (positions && vertexCount > 0) {
        Vec3 lo{positions[0], positions[1], positions[2]}, hi = lo;
        for (u32 v = 1; v < vertexCount; ++v) {
            const Vec3 p{positions[v * 3 + 0], positions[v * 3 + 1], positions[v * 3 + 2]};
            lo = Vec3{std::fmin(lo.x, p.x), std::fmin(lo.y, p.y), std::fmin(lo.z, p.z)};
            hi = Vec3{std::fmax(hi.x, p.x), std::fmax(hi.y, p.y), std::fmax(hi.z, p.z)};
        }
        generateLatticeCage(lo, hi, opt, outCage);
    }
    bindVerticesToCage(outCage, positions, vertexCount, opt, outBinding);
}

void deformVertices(const Cage& cage, const CageBinding& binding, const f32* nodeDisp,
                    const f32* restPositions, u32 vertexCount, const DeformParams& params,
                    f32* outPositions) {
    if (!restPositions || !outPositions) return;
    const i32 nodeCount = static_cast<i32>(cage.nodeCount());

    for (u32 v = 0; v < vertexCount; ++v) {
        const Vec3 rest{restPositions[v * 3 + 0], restPositions[v * 3 + 1], restPositions[v * 3 + 2]};
        Vec3 disp{0, 0, 0};

        if (nodeDisp && v < binding.vertexCount()) {
            const i32* idx = &binding.nodeIndex[static_cast<usize>(v) * kCageInfluences];
            const f32* wt  = &binding.weight[static_cast<usize>(v) * kCageInfluences];
            for (u32 k = 0; k < kCageInfluences; ++k) {
                const i32 ni = idx[k];
                if (ni < 0 || ni >= nodeCount) continue;   // the contract's own guard
                disp = disp + Vec3{nodeDisp[ni * 3 + 0], nodeDisp[ni * 3 + 1], nodeDisp[ni * 3 + 2]} * wt[k];
            }
        }

        if (params.mirrorY) disp.y = -disp.y;
        disp = disp * params.gain;
        const f32 len = disp.size();
        if (len > params.maxDisplacement && len > 1e-9f)
            disp = disp * (params.maxDisplacement / len);

        const Vec3 o = rest + disp;
        outPositions[v * 3 + 0] = o.x;
        outPositions[v * 3 + 1] = o.y;
        outPositions[v * 3 + 2] = o.z;
    }
}

} // namespace aver::deform
