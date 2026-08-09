// Builds the landscape quadtree and selects the nodes to draw. See LandscapeTree.hpp.
#include "aver/landscape/LandscapeTree.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace aver::landscape {
namespace {

// Writes `m` into `why` when given and returns false.
bool fail(std::string* why, const char* m) { if (why) *why = m; return false; }

// Largest vertical distance between the strided source samples and the chord drawn across them.
f32 runDeviation(const fmt::OcLandData& d, u32 x0, u32 y0, u32 dx, u32 dy, u32 span) {
    const f32 h0 = d.heightAt(x0, y0);
    const f32 h1 = d.heightAt(x0 + dx * span, y0 + dy * span);
    f32 worst = 0.0f;
    for (u32 s = 1; s < span; ++s) {
        const f32 t = static_cast<f32>(s) / static_cast<f32>(span);
        const f32 chord = h0 + (h1 - h0) * t;
        worst = std::fmax(worst, std::fabs(d.heightAt(x0 + dx * s, y0 + dy * s) - chord));
    }
    return worst;
}

} // namespace

// Extracts the six normalised planes from a row-vector view-projection.
Frustum Frustum::fromViewProj(const f32 m[16]) {
    // ROW VECTORS: clip = [x y z 1] * M, so clip.x comes from COLUMN 0.
    const f32 cx[4] = {m[0], m[4], m[8],  m[12]};
    const f32 cy[4] = {m[1], m[5], m[9],  m[13]};
    const f32 cz[4] = {m[2], m[6], m[10], m[14]};
    const f32 cw[4] = {m[3], m[7], m[11], m[15]};

    Frustum f;
    for (int i = 0; i < 4; ++i) {
        f.plane[0][i] = cw[i] + cx[i];   // left
        f.plane[1][i] = cw[i] - cx[i];   // right
        f.plane[2][i] = cw[i] + cy[i];   // bottom
        f.plane[3][i] = cw[i] - cy[i];   // top
        f.plane[4][i] = cz[i];           // near: clip.z >= 0, the LH [0,1] depth range
        f.plane[5][i] = cw[i] - cz[i];   // far
    }
    // Normalised so a plane evaluation is a distance comparable against a radius.
    for (int p = 0; p < 6; ++p) {
        const f32 len = std::sqrt(f.plane[p][0]*f.plane[p][0] +
                                  f.plane[p][1]*f.plane[p][1] +
                                  f.plane[p][2]*f.plane[p][2]);
        if (len > 1e-20f)
            for (int i = 0; i < 4; ++i) f.plane[p][i] /= len;
    }
    return f;
}

// True if any part of the sphere is inside all six planes.
bool Frustum::intersectsSphere(const f32 c[3], f32 radius) const {
    for (int p = 0; p < 6; ++p) {
        const f32 dist = plane[p][0]*c[0] + plane[p][1]*c[1] + plane[p][2]*c[2] + plane[p][3];
        if (dist < -radius) return false;
    }
    return true;
}

// Subdivides a section into the node tree. Returns false and fills `why` on a bad section or nodeQuads.
bool LandscapeTree::build(const fmt::OcLandData& d, u32 nodeQuads, std::string* why) {
    nodes_.clear();
    levelError_.clear();
    refined_.clear();
    rootIndex_ = kInvalidNode;
    levels_ = 0;
    nodeQuads_ = nodeQuads;

    if (!d.valid()) return fail(why, "the section is not internally consistent");
    if (nodeQuads < 2 || (nodeQuads & (nodeQuads - 1)) != 0)
        return fail(why, "nodeQuads must be a power of two of at least 2");

    const u32 quads = d.sampleCount - 1;
    if (quads % nodeQuads != 0)
        return fail(why, "sampleCount must be (k * nodeQuads) + 1");
    u32 nodesAcross = quads / nodeQuads;
    if ((nodesAcross & (nodesAcross - 1)) != 0)
        return fail(why, "(sampleCount - 1) / nodeQuads must be a power of two");

    // Count the levels: nodesAcross halves each time until one node covers the section.
    u32 top = 0;
    for (u32 n = nodesAcross; n > 1; n >>= 1) ++top;
    levels_ = top + 1;
    levelError_.assign(levels_, 0.0f);

    // Built bottom-up, so a parent can be given its children's indices.
    std::vector<std::vector<u32>> byLevel(levels_);

    for (u32 level = 0; level < levels_; ++level) {
        const u32 stride = 1u << level;
        const u32 span   = nodeQuads * stride;          // source quads on a side
        const u32 across = nodesAcross >> level;

        for (u32 ny = 0; ny < across; ++ny) {
            for (u32 nx = 0; nx < across; ++nx) {
                LandscapeNode n;
                n.level = level;
                n.stride = stride;
                n.spanQuads = span;
                n.sampleX = nx * span;
                n.sampleY = ny * span;

                // Height extent over every source sample the node covers, not just the ones it draws.
                f32 lo =  1e30f, hi = -1e30f;
                for (u32 y = 0; y <= span; ++y) {
                    for (u32 x = 0; x <= span; ++x) {
                        const f32 h = d.heightAt(n.sampleX + x, n.sampleY + y);
                        lo = std::fmin(lo, h);
                        hi = std::fmax(hi, h);
                    }
                }

                const f32 x0 = d.originCm[0] + static_cast<f32>(n.sampleX) * d.spacingCm;
                const f32 y0 = d.originCm[1] + static_cast<f32>(n.sampleY) * d.spacingCm;
                const f32 side = static_cast<f32>(span) * d.spacingCm;
                n.centre[0] = x0 + side * 0.5f;
                n.centre[1] = y0 + side * 0.5f;
                n.centre[2] = (lo + hi) * 0.5f;
                const f32 hx = side * 0.5f, hz = (hi - lo) * 0.5f;
                n.radius = std::sqrt(hx*hx + hx*hx + hz*hz);   // half-diagonal of the AABB

                // Geometric error: the worst strided-run deviation in either direction. Zero at level 0.
                if (level > 0) {
                    f32 e = 0.0f;
                    for (u32 y = 0; y <= span; y += stride)
                        for (u32 x = 0; x < span; x += stride)
                            e = std::fmax(e, runDeviation(d, n.sampleX + x, n.sampleY + y, 1, 0, stride));
                    for (u32 x = 0; x <= span; x += stride)
                        for (u32 y = 0; y < span; y += stride)
                            e = std::fmax(e, runDeviation(d, n.sampleX + x, n.sampleY + y, 0, 1, stride));
                    n.errorCm = e;
                }
                levelError_[level] = std::fmax(levelError_[level], n.errorCm);

                if (level > 0) {
                    // The four children are the 2x2 block at (2nx, 2ny) on the level below.
                    const u32 belowAcross = across * 2;
                    const std::vector<u32>& below = byLevel[level - 1];
                    const u32 c[4] = {
                        (2*ny + 0) * belowAcross + (2*nx + 0),
                        (2*ny + 0) * belowAcross + (2*nx + 1),
                        (2*ny + 1) * belowAcross + (2*nx + 0),
                        (2*ny + 1) * belowAcross + (2*nx + 1),
                    };
                    for (int i = 0; i < 4; ++i)
                        n.child[i] = c[i] < below.size() ? below[c[i]] : kInvalidNode;
                }

                byLevel[level].push_back(static_cast<u32>(nodes_.size()));
                nodes_.push_back(n);
            }
        }
    }

    rootIndex_ = byLevel[levels_ - 1].front();

    // Skirt depth, sized last: it is the COARSER neighbour's error that opens the crack, and 2:1
    // balanced selection makes that neighbour exactly one level up.
    for (LandscapeNode& n : nodes_) {
        const f32 coarser = levelErrorCm(n.level + 1);
        n.skirtCm = std::fmax(coarser * 1.5f, d.spacingCm * 0.25f);
    }

    refined_.assign(nodes_.size(), 0);
    return true;
}

// Forgets which nodes are refined, so the next select is unbiased.
void LandscapeTree::resetHysteresis() const {
    std::fill(refined_.begin(), refined_.end(), static_cast<u8>(0));
}

// Widens the outer-rim skirt; see the header for why an inner-boundary formula cannot cover this case.
void LandscapeTree::widenRimSkirts(f32 minSkirtCm) {
    if (nodes_.empty() || rootIndex_ == kInvalidNode) return;
    // The root always covers the whole section, so its own span is the section's total quad count --
    // the same number every node's sampleX/sampleY/spanQuads are expressed against.
    const u32 total = nodes_[rootIndex_].spanQuads;
    for (LandscapeNode& n : nodes_) {
        const bool onRim = n.sampleX == 0 || n.sampleY == 0 ||
                            n.sampleX + n.spanQuads >= total || n.sampleY + n.spanQuads >= total;
        if (onRim) n.skirtCm = std::fmax(n.skirtCm, minSkirtCm);
    }
}

// Walks one node: emits it, or recurses into its children when its projected error is too large.
void LandscapeTree::descend(u32 index, const SelectParams& p, SelectResult& out,
                            std::vector<f32>& errors) const {
    const LandscapeNode& n = nodes_[index];

    if (p.useFrustum && !p.frustum.intersectsSphere(n.centre, n.radius)) {
        ++out.culled;
        return;
    }

    // Radial distance to the sphere's surface, floored at zero for a camera inside it.
    const f32 dx = p.cameraCm[0] - n.centre[0];
    const f32 dy = p.cameraCm[1] - n.centre[1];
    const f32 dz = p.cameraCm[2] - n.centre[2];
    const f32 dist = std::fmax(std::sqrt(dx*dx + dy*dy + dz*dz) - n.radius, 0.0f);

    // Screen-space error in pixels; unbounded at zero distance.
    const f32 screen = dist > 1e-3f ? n.errorCm * p.projScale / dist : 1e30f;

    // Hysteresis: refine above the threshold, un-refine only below 0.8x it.
    const bool wasRefined = refined_[index] != 0;
    const f32 limit = wasRefined ? p.screenErrorPx * 0.8f : p.screenErrorPx;
    const bool wantRefine = screen > limit;

    if (!wantRefine || n.leaf()) {
        refined_[index] = 0;
        out.nodes.push_back(index);
        errors.push_back(screen);
        return;
    }
    refined_[index] = 1;
    for (int i = 0; i < 4; ++i)
        if (n.child[i] != kInvalidNode) descend(n.child[i], p, out, errors);
}

// Picks the nodes to draw this frame and clamps the result to maxDraws.
void LandscapeTree::select(const SelectParams& p, SelectResult& out) const {
    out.nodes.clear();
    out.culled = 0;
    out.dropped = 0;
    if (nodes_.empty() || rootIndex_ == kInvalidNode) return;

    std::vector<f32> errors;
    descend(rootIndex_, p, out, errors);

    if (p.maxDraws > 0 && out.nodes.size() > p.maxDraws) {
        // Dropped in ascending screen error, by sorting an index permutation.
        std::vector<u32> order(out.nodes.size());
        for (u32 i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(),
                         [&](u32 a, u32 b) { return errors[a] > errors[b]; });
        std::vector<u32> keep;
        keep.reserve(p.maxDraws);
        for (u32 i = 0; i < p.maxDraws; ++i) keep.push_back(out.nodes[order[i]]);
        out.dropped = static_cast<u32>(out.nodes.size() - p.maxDraws);
        out.nodes.swap(keep);
    }
}

} // namespace aver::landscape
