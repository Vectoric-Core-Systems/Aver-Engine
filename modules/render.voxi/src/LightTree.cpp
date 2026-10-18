#include "aver/voxi/LightTree.hpp"

#include <cstring>

// The light tree (docs/plans/wave4/MANY_LIGHTS.md 3.1, 3.2). Aver.Core only; the HLSL twin is
// shaders/voxi_lighttree.hlsli and must stay in step with importance(), sample() and pdfOf() below.
namespace aver::voxi {

namespace {

constexpr f32 kPi = 3.14159265358979f;

u64 bytesHash(const PackedLight& l) {
    u64 w[sizeof(PackedLight) / 8];
    std::memcpy(w, &l, sizeof(w));
    u64 h = 1469598103934665603ull;
    for (const u64 x : w) { h = (h ^ x) * 1099511628211ull; h ^= h >> 29; }
    return h;
}

u32 expandBits(u32 v) {
    v &= 1023u;
    v = (v * 0x00010001u) & 0xFF0000FFu;
    v = (v * 0x00000101u) & 0x0F00F00Fu;
    v = (v * 0x00000011u) & 0xC30C30C3u;
    v = (v * 0x00000005u) & 0x49249249u;
    return v;
}

u32 ceilLog2(u32 n) {
    u32 r = 0;
    while ((1u << r) < n) ++r;
    return r;
}

f32 dot3(const f32 a[3], const f32 b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Canonical order: by the lights' bytes (a total order independent of where a light sat in the list). The 64-bit
// hash leads so the sort moves 16-byte pairs; equal hashes fall back to the bytes.
void canonicalOrder(const PackedLight* L, u32 n, std::vector<u32>& order) {
    std::vector<std::pair<u64, u32>> h(n);
    for (u32 i = 0; i < n; ++i) h[i] = {bytesHash(L[i]), i};
    std::sort(h.begin(), h.end(), [&](const std::pair<u64, u32>& a, const std::pair<u64, u32>& b) {
        if (a.first != b.first) return a.first < b.first;
        const int c = std::memcmp(&L[a.second], &L[b.second], sizeof(PackedLight));
        return c != 0 ? c < 0 : a.second < b.second;
    });
    order.resize(n);
    for (u32 i = 0; i < n; ++i) order[i] = h[i].second;
}

// One light as a leaf. False on a NaN, infinite or negative power or a non-finite position or range.
bool leafOf(const PackedLight& l, TreeNodeRec& n) {
    n = TreeNodeRec{};
    const u32 kind = static_cast<u32>(l.axisKind[3] + 0.5f) % 8u;
    if (kind == kLightKindDirectional) return false;
    f32 phi = 0.2126f * l.radianceRange[0] + 0.7152f * l.radianceRange[1] + 0.0722f * l.radianceRange[2];
    if (kind == kLightKindRect) {
        const f32 areaM2 = 4.0f * l.shape[0] * l.shape[1] * 1e-4f;
        phi *= std::max(areaM2, 1.0f);   // never below the 1 m^2 the cheap irradiance assumes for a rect
    }
    const f32 range = l.radianceRange[3];
    if (!std::isfinite(phi) || phi < 0.0f || !std::isfinite(range) || range < 0.0f) return false;
    for (int a = 0; a < 3; ++a) {
        if (!std::isfinite(l.posRadius[a])) return false;
        n.bmin[a] = n.bmax[a] = l.posRadius[a];
    }
    n.phi = phi;
    n.range = range;
    n.maxRadius = std::isfinite(l.posRadius[3]) ? std::max(l.posRadius[3], 0.0f) : 0.0f;
    n.left = -1.0f;
    n.parent = -1.0f;
    u32 flags = kTreeFlagLeaf;
    if (((static_cast<u32>(l.axisKind[3] + 0.5f)) >> 3) != 0u) flags |= kTreeFlagNoShadow;
    if (kind == kLightKindRect) flags |= kTreeFlagRect;
    n.flags = static_cast<f32>(flags);

    // Orientation cone (zero width for a spot or rect axis, all directions for a point) and emission spread.
    const f32 len = std::sqrt(dot3(l.axisKind, l.axisKind));
    if (kind == kLightKindPoint || !(len > 1e-6f)) {
        n.axis[0] = n.axis[1] = n.axis[2] = 0.0f;
        n.cosThetaO = -1.0f;
        n.cosThetaE = -1.0f;
    } else {
        for (int a = 0; a < 3; ++a) n.axis[a] = l.axisKind[a] / len;
        n.cosThetaO = 1.0f;
        n.cosThetaE = kind == kLightKindSpot ? clampf(l.shape[1], -1.0f, 1.0f) : 0.0f;
    }
    return true;
}

// The standard cone union (Conty Estevez and Kulla 2018): the smallest cone holding both, omni past pi.
void mergeCone(const TreeNodeRec& a, const TreeNodeRec& b, f32 outAxis[3], f32& outCos) {
    const f32 t1 = std::acos(clampf(a.cosThetaO, -1.0f, 1.0f));
    const f32 t2 = std::acos(clampf(b.cosThetaO, -1.0f, 1.0f));
    const bool omni1 = a.cosThetaO <= -0.99999f, omni2 = b.cosThetaO <= -0.99999f;
    auto omni = [&] { outAxis[0] = outAxis[1] = outAxis[2] = 0.0f; outCos = -1.0f; };
    if (omni1 || omni2) { omni(); return; }
    const f32 td = std::acos(clampf(dot3(a.axis, b.axis), -1.0f, 1.0f));
    if (std::min(td + t2, kPi) <= t1) { for (int i = 0; i < 3; ++i) outAxis[i] = a.axis[i]; outCos = a.cosThetaO; return; }
    if (std::min(td + t1, kPi) <= t2) { for (int i = 0; i < 3; ++i) outAxis[i] = b.axis[i]; outCos = b.cosThetaO; return; }
    const f32 tn = 0.5f * (t1 + td + t2);
    if (tn >= kPi) { omni(); return; }
    // Rotate a toward b by tn - t1.
    const f32 r = tn - t1;
    const f32 ab = dot3(a.axis, b.axis);
    f32 t[3] = {b.axis[0] - a.axis[0] * ab, b.axis[1] - a.axis[1] * ab, b.axis[2] - a.axis[2] * ab};
    const f32 tl = std::sqrt(dot3(t, t));
    const f32 cr = std::cos(r), sr = std::sin(r);
    for (int i = 0; i < 3; ++i) outAxis[i] = a.axis[i] * cr + (tl > 1e-12f ? t[i] / tl : 0.0f) * sr;
    const f32 ol = std::sqrt(dot3(outAxis, outAxis));
    for (int i = 0; i < 3; ++i) outAxis[i] /= std::max(ol, 1e-12f);
    outCos = std::cos(tn);
}

} // namespace

u64 LightTree::keyOf(const PackedLight* lights, u32 count) {
    u64 k = 0;
    for (u32 i = 0; i < count; ++i) k += bytesHash(lights[i]);
    return (k ^ (static_cast<u64>(count) * 0x9E3779B97F4A7C15ull)) * 1099511628211ull;
}

bool LightTree::build(const PackedLight* lights, u32 count) {
    valid_ = false;
    canon_.clear(); leafRecs_.clear(); morton_.clear(); sorted_.clear(); leafOfCanon_.clear(); leafMap_.clear();
    nodes_.clear();
    key_ = 0;
    if (!lights || count == 0 || count > kMaxTreeLights) return false;
    std::vector<u32> order;
    canonicalOrder(lights, count, order);
    return buildOver(lights, count, order);
}

bool LightTree::buildOver(const PackedLight* lights, u32 count, const std::vector<u32>& order) {
    canon_.resize(count);
    leafRecs_.resize(count);
    for (u32 c = 0; c < count; ++c) {
        canon_[c] = lights[order[c]];
        if (!leafOf(canon_[c], leafRecs_[c])) { canon_.clear(); leafRecs_.clear(); return false; }
    }
    // Morton codes over the lights' bounds (10 bits an axis).
    f32 lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const PackedLight& l : canon_)
        for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], l.posRadius[a]); hi[a] = std::max(hi[a], l.posRadius[a]); }
    morton_.assign(count, 0u);
    for (u32 c = 0; c < count; ++c) {
        u32 q[3];
        for (int a = 0; a < 3; ++a) {
            const f32 ext = hi[a] - lo[a];
            const f32 t = ext > 0.0f ? (canon_[c].posRadius[a] - lo[a]) / ext : 0.0f;
            q[a] = static_cast<u32>(clampf(t * 1024.0f, 0.0f, 1023.0f));
        }
        morton_[c] = expandBits(q[0]) | (expandBits(q[1]) << 1) | (expandBits(q[2]) << 2);
    }
    sorted_.resize(count);
    for (u32 c = 0; c < count; ++c) sorted_[c] = c;
    std::sort(sorted_.begin(), sorted_.end(), [&](u32 a, u32 b) {
        return morton_[a] != morton_[b] ? morton_[a] < morton_[b] : a < b;
    });
    nodes_.clear();
    nodes_.reserve(static_cast<usize>(count) * 2 - 1);
    leafOfCanon_.assign(count, 0u);
    buildRange(0, count, 0, 0xFFFFFFFFu);
    rewriteLeaves(order);
    key_ = keyOf(lights, count);
    valid_ = true;
    return true;
}

// Builds the subtree over sorted_[lo, hi); returns its node index. Pre-order allocation, so the root is node 0.
u32 LightTree::buildRange(u32 lo, u32 hi, u32 depth, u32 parent) {
    const u32 idx = static_cast<u32>(nodes_.size());
    nodes_.emplace_back();
    const u32 n = hi - lo;
    if (n == 1) {
        const u32 c = sorted_[lo];
        nodes_[idx] = leafRecs_[c];
        nodes_[idx].parent = parent == 0xFFFFFFFFu ? -1.0f : static_cast<f32>(parent);
        leafOfCanon_[c] = idx;
        return idx;
    }
    u32 mid = lo + n / 2;   // the median split: coincident lights, and the depth cap below
    const u32 codeLo = morton_[sorted_[lo]], codeHi = morton_[sorted_[hi - 1]];
    // A Morton split can leave a long chain; once the depth left would not hold a balanced split, halve instead.
    if (codeLo != codeHi && depth + ceilLog2(n) < kLightTreeMaxDepth) {
        const u32 diff = codeLo ^ codeHi;
        u32 bit = 31u;
        while (!((diff >> bit) & 1u)) --bit;
        u32 a = lo + 1, b = hi - 1;   // first index whose code has `bit` set
        while (a < b) {
            const u32 m = a + (b - a) / 2;
            if ((morton_[sorted_[m]] >> bit) & 1u) b = m; else a = m + 1;
        }
        mid = a;
    }
    const u32 l = buildRange(lo, mid, depth + 1, idx);
    const u32 r = buildRange(mid, hi, depth + 1, idx);
    const TreeNodeRec A = nodes_[l];
    const TreeNodeRec B = nodes_[r];
    TreeNodeRec m{};
    for (int a = 0; a < 3; ++a) { m.bmin[a] = std::min(A.bmin[a], B.bmin[a]); m.bmax[a] = std::max(A.bmax[a], B.bmax[a]); }
    m.phi = A.phi + B.phi;
    m.range = std::max(A.range, B.range);
    m.maxRadius = std::max(A.maxRadius, B.maxRadius);
    m.cosThetaE = std::min(A.cosThetaE, B.cosThetaE);
    mergeCone(A, B, m.axis, m.cosThetaO);
    const u32 fa = static_cast<u32>(A.flags), fb = static_cast<u32>(B.flags);
    m.flags = static_cast<f32>((fa & fb & kTreeFlagNoShadow) | ((fa | fb) & kTreeFlagRect));
    m.left = static_cast<f32>(l);
    m.right = static_cast<f32>(r);
    m.parent = parent == 0xFFFFFFFFu ? -1.0f : static_cast<f32>(parent);
    nodes_[idx] = m;
    return idx;
}

void LightTree::rewriteLeaves(const std::vector<u32>& order) {
    u32 top = 0;
    for (const u32 o : order) top = std::max(top, o + 1u);
    leafMap_.assign(top, -1);
    for (u32 c = 0; c < order.size(); ++c) {
        nodes_[leafOfCanon_[c]].right = static_cast<f32>(order[c]);
        leafMap_[order[c]] = static_cast<i32>(leafOfCanon_[c]);
    }
}

bool LightTree::fill(const PackedLight* lights, u32 count) {
    if (!valid_ || !lights || count != canon_.size()) return false;
    std::vector<u32> order;
    canonicalOrder(lights, count, order);
    for (u32 c = 0; c < count; ++c)
        if (std::memcmp(&lights[order[c]], &canon_[c], sizeof(PackedLight)) != 0) return false;
    rewriteLeaves(order);
    return true;
}

std::vector<f32> LightTree::leafMapFloats(u32 listCount) const {
    std::vector<f32> out(listCount, -1.0f);
    for (u32 i = 0; i < listCount && i < leafMap_.size(); ++i) out[i] = static_cast<f32>(leafMap_[i]);
    return out;
}

u32 LightTree::depth() const {
    u32 best = 0;
    for (u32 c = 0; c < leafOfCanon_.size(); ++c) {
        u32 d = 0;
        for (i32 n = static_cast<i32>(leafOfCanon_[c]); nodes_[static_cast<usize>(n)].parent >= 0.0f;
             n = static_cast<i32>(nodes_[static_cast<usize>(n)].parent))
            ++d;
        best = std::max(best, d);
    }
    return best;
}

// ---- the bound (CPU twin of rdTreeImportance) ----
f32 LightTree::importance(const TreeNodeRec& nd, const f32 p[3], const f32 n[3]) {
    const f32 R = nd.range;
    if (!(nd.phi > 0.0f)) return 0.0f;
    // Nearest the box of centres comes to p: no light is closer, so the window and the distance are bounds.
    f32 dmin2 = 0.0f;
    for (int a = 0; a < 3; ++a) {
        const f32 q = std::max(std::max(nd.bmin[a] - p[a], p[a] - nd.bmax[a]), 0.0f);
        dmin2 += q * q;
    }
    if (dmin2 >= R * R) return 0.0f;   // outside every light's range window: exactly zero
    const u32 flags = static_cast<u32>(nd.flags + 0.5f);
    const bool leaf = (flags & kTreeFlagLeaf) != 0u, rect = (flags & kTreeFlagRect) != 0u;
    const f32 c[3] = {0.5f * (nd.bmin[0] + nd.bmax[0]), 0.5f * (nd.bmin[1] + nd.bmax[1]), 0.5f * (nd.bmin[2] + nd.bmax[2])};
    const f32 e[3] = {nd.bmax[0] - nd.bmin[0], nd.bmax[1] - nd.bmin[1], nd.bmax[2] - nd.bmin[2]};
    const f32 hd = 0.5f * std::sqrt(dot3(e, e));
    const f32 toC[3] = {c[0] - p[0], c[1] - p[1], c[2] - p[2]};
    const f32 d = std::sqrt(dot3(toC, toC));
    f32 cosI = 1.0f;
    bool emitOk = true;
    if (d > hd) {   // outside the sphere around the centres: the box subtends a cone of half angle thetaU
        const f32 invD = 1.0f / d;
        const f32 sinU = std::min(hd * invD, 1.0f);
        const f32 cosU = std::sqrt(std::max(1.0f - sinU * sinU, 0.0f));
        const f32 cosIc = dot3(n, toC) * invD;
        if (cosIc < cosU) {
            const f32 sinI = std::sqrt(std::max(1.0f - cosIc * cosIc, 0.0f));
            cosI = std::max(cosIc * cosU + sinI * sinU, 0.0f);
        }
        if (nd.cosThetaO > -0.9999f && nd.cosThetaE > -0.9999f) {   // not omni: the emission cone
            const f32 thetaU = std::asin(sinU);
            const f32 thetaO = std::acos(clampf(nd.cosThetaO, -1.0f, 1.0f));
            const f32 thetaE = std::acos(clampf(nd.cosThetaE, -1.0f, 1.0f));
            const f32 cosW = -dot3(nd.axis, toC) * invD;
            const f32 thetaW = std::acos(clampf(cosW, -1.0f, 1.0f));
            emitOk = std::max(thetaW - thetaO - thetaU, 0.0f) < thetaE;
        }
    }
    if (!emitOk) return 0.0f;
    if (rect) cosI = std::max(cosI, 0.1f);
    const f32 x2 = dmin2 / (R * R);
    f32 win = std::clamp(1.0f - x2 * x2, 0.0f, 1.0f);
    win *= win;
    // Distance term: a leaf is exact (its emitter radius floors it). An inner node's nearest-possible distance is floored
    // at its half-diagonal, as the paper regularises: with the point inside the cloud of lights (the camera in a city)
    // the strict 1 / dmin^2 piles the whole probability on the node under the camera. The zero cases above and the
    // gating stay strict, so a lit light is never missed; only the magnitude (the variance) is regularised.
    const f32 floor2 = leaf ? nd.maxRadius * nd.maxRadius : hd * hd;
    return nd.phi * 1e4f * cosI * win / std::max(std::max(dmin2, floor2), 1.0f);
}

bool LightTree::sample(const f32 p[3], const f32 n[3], f32 u, u32& listIndex, f32& pdf) const {
    listIndex = 0xFFFFFFFFu;
    pdf = 0.0f;
    if (!valid_) return false;
    u = clampf(u, 0.0f, 0.99999994f);
    u32 node = 0;
    f32 prob = 1.0f;
    for (u32 it = 0; it <= kLightTreeMaxDepth; ++it) {
        const TreeNodeRec& nd = nodes_[node];
        if ((static_cast<u32>(nd.flags + 0.5f) & kTreeFlagLeaf) != 0u) {
            if (node == 0 && !(importance(nd, p, n) > 0.0f)) return false;   // a one-light tree
            listIndex = static_cast<u32>(nd.right + 0.5f);
            pdf = prob;
            return true;
        }
        const u32 l = static_cast<u32>(nd.left + 0.5f), r = static_cast<u32>(nd.right + 0.5f);
        const f32 il = importance(nodes_[l], p, n), ir = importance(nodes_[r], p, n);
        const f32 sum = il + ir;
        if (!(sum > 0.0f) || !std::isfinite(sum)) return false;
        const f32 pl = il / sum, pr = ir / sum;
        if (u < pl) { u = clampf(u / pl, 0.0f, 0.99999994f); prob *= pl; node = l; }
        else        { u = clampf((u - pl) / pr, 0.0f, 0.99999994f); prob *= pr; node = r; }
    }
    return false;
}

f32 LightTree::pdfOf(u32 leaf, const f32 p[3], const f32 n[3]) const {
    if (!valid_ || leaf >= nodes_.size()) return 0.0f;
    if (leaf == 0) return importance(nodes_[0], p, n) > 0.0f ? 1.0f : 0.0f;
    f32 prob = 1.0f;
    u32 child = leaf;
    while (nodes_[child].parent >= 0.0f) {
        const u32 par = static_cast<u32>(nodes_[child].parent + 0.5f);
        const TreeNodeRec& nd = nodes_[par];
        const u32 l = static_cast<u32>(nd.left + 0.5f), r = static_cast<u32>(nd.right + 0.5f);
        const f32 il = importance(nodes_[l], p, n), ir = importance(nodes_[r], p, n);
        const f32 sum = il + ir;
        if (!(sum > 0.0f)) return 0.0f;
        prob *= (child == l ? il : ir) / sum;
        child = par;
    }
    return prob;
}

} // namespace aver::voxi
