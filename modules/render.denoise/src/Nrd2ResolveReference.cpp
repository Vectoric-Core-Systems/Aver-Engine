// CPU twin of NRD2's pyramid, resolve (forward + backward) and oracle fit.
// KEEP IN SYNC WITH nrd2.hlsl / nrd2_resolve.hlsli / nrd2_capture.hlsl.
#include "aver/render/denoise/Nrd2ResolveReference.hpp"

#include "aver/render/denoise/Nrd2.hpp"
#include "aver/render/denoise/Nrd2Dataset.hpp"
#include "aver/render/neural/NeuralOptimiser.hpp"

#include <algorithm>
#include <cmath>

namespace aver::render::denoise {

namespace {

struct V4 { f32 x = 0, y = 0, z = 0, w = 0; };
struct V3 { f32 x = 0, y = 0, z = 0; };

V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(f32 s, V3 a) { return {s * a.x, s * a.y, s * a.z}; }
V3 operator/(V3 a, f32 s) { return {a.x / s, a.y / s, a.z / s}; }

constexpr f32 kLn2 = 0.69314718f;

f32 sat(f32 v) { return std::clamp(v, 0.0f, 1.0f); }
f32 lum(V3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
bool isNan(f32 v) { return v != v; }

V4 load4(const std::vector<f32>& v, usize i) { return {v[4 * i], v[4 * i + 1], v[4 * i + 2], v[4 * i + 3]}; }
V3 rgb(V4 v) { return {v.x, v.y, v.z}; }

V3 decodeNormal(V4 e) {
    const f32 fx = e.x * 2.0f - 1.0f, fy = e.y * 2.0f - 1.0f;
    V3 n{fx, fy, 1.0f - std::fabs(fx) - std::fabs(fy)};
    if (n.z < 0.0f) {
        const f32 ox = n.x, oy = n.y;
        n.x = (1.0f - std::fabs(oy)) * (ox >= 0.0f ? 1.0f : -1.0f);
        n.y = (1.0f - std::fabs(ox)) * (oy >= 0.0f ? 1.0f : -1.0f);
    }
    const f32 l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    return n / l;
}

bool finite3(V4 v) {
    return !isNan(v.x) && !isNan(v.y) && !isNan(v.z) && std::fabs(v.x) < 6.0e4f && std::fabs(v.y) < 6.0e4f &&
           std::fabs(v.z) < 6.0e4f;
}

// nrd2Reduce.
constexpr f32 kReduceDepth = 23.0f;
void reduce(const V4* G, const V4* D, const V4* S, const u32 idx[4], V4& g, V4& dv, V4& sv) {
    f32 zmin = 1.0e30f;
    for (u32 i = 0; i < 4; ++i) if (G[idx[i]].w > 0.0f) zmin = std::min(zmin, G[idx[i]].w);
    g = {}; dv = {}; sv = {};
    if (zmin >= 1.0e30f) return;
    V3 nsum, dsum, ssum;
    f32 zsum = 0, wsum = 0, dw = 0, sw = 0;
    for (u32 j = 0; j < 4; ++j) {
        const V4 Gj = G[idx[j]];
        if (Gj.w <= 0.0f) continue;
        const f32 w = std::exp2(-std::min((Gj.w - zmin) / zmin, 64.0f) * kReduceDepth);
        const V4 Dj = D[idx[j]], Sj = S[idx[j]];
        nsum = nsum + w * V3{Gj.x, Gj.y, Gj.z}; zsum += w * Gj.w; wsum += w;
        dsum = dsum + (w * Dj.w) * rgb(Dj); dw += w * Dj.w;
        ssum = ssum + (w * Sj.w) * rgb(Sj); sw += w * Sj.w;
    }
    const V3 n = nsum / wsum;
    g = {n.x, n.y, n.z, zsum / wsum};
    const V3 d = dw > 0.0f ? dsum / dw : V3{};
    const V3 s = sw > 0.0f ? ssum / sw : V3{};
    dv = {d.x, d.y, d.z, dw / wsum};
    sv = {s.x, s.y, s.z, sw / wsum};
}

void store(std::vector<f32>& dst, usize i, V4 v, bool half) {
    const f32 c[4] = {v.x, v.y, v.z, v.w};
    for (u32 k = 0; k < 4; ++k) dst[4 * i + k] = half ? nrd2F16ToF32(nrd2F32ToF16(c[k])) : c[k];
}

// ---- resolve ----

struct Params { f32 logit[3]; f32 log2Depth, log2Normal, log2Lum; };

f32 saneParam(f32 v, f32 def, f32 lo, f32 hi) { return (!isNan(v) && std::fabs(v) < 1.0e6f) ? std::clamp(v, lo, hi) : def; }

Params sanitise(const f32 v[6], const f32 d[6]) {
    Params p;
    for (u32 k = 0; k < 3; ++k) p.logit[k] = saneParam(v[k], d[k], -16.0f, 16.0f);
    p.log2Depth  = saneParam(v[3], d[3], -8.0f, 8.0f);
    p.log2Normal = saneParam(v[4], d[4], -8.0f, 8.0f);
    p.log2Lum    = saneParam(v[5], d[5], -8.0f, 8.0f);
    return p;
}

struct Taps { V4 g[4]; V4 x[4]; f32 b[4]; };

Taps loadTaps(const Nrd2Pyramid& p, u32 signal, u32 shift, u32 qx, u32 qy) {
    const u32 l = shift - 1;
    const i32 lw = static_cast<i32>(p.width[l]), lh = static_cast<i32>(p.height[l]);
    const f32 px = (static_cast<f32>(qx) + 0.5f) / static_cast<f32>(1u << shift) - 0.5f;
    const f32 py = (static_cast<f32>(qy) + 0.5f) / static_cast<f32>(1u << shift) - 0.5f;
    const i32 bx = static_cast<i32>(std::floor(px)), by = static_cast<i32>(std::floor(py));
    const f32 fx = px - static_cast<f32>(bx), fy = py - static_cast<f32>(by);
    Taps t;
    for (u32 i = 0; i < 4; ++i) {
        const i32 ox = static_cast<i32>(i & 1u), oy = static_cast<i32>(i >> 1);
        const i32 cx = std::clamp(bx + ox, 0, lw - 1), cy = std::clamp(by + oy, 0, lh - 1);
        const usize at = static_cast<usize>(cy) * static_cast<usize>(lw) + static_cast<usize>(cx);
        t.g[i] = load4(p.guide[l], at);
        t.x[i] = load4(p.value[signal][l], at);
        t.b[i] = (ox ? fx : 1.0f - fx) * (oy ? fy : 1.0f - fy);
    }
    return t;
}

f32 tapWeight(V4 g, V4 x, f32 b, f32 zm, V3 n, f32 dS, f32 nP, f32& dz, f32& cosN) {
    dz = 0.0f; cosN = 0.0f;
    const f32 nl = std::sqrt(g.x * g.x + g.y * g.y + g.z * g.z);
    if (g.w <= 0.0f || x.w <= 0.0f || nl < 1.0e-3f) return 0.0f;
    dz = std::min(std::fabs(g.w - zm) / std::max(zm, 1.0e-4f), 64.0f);
    cosN = sat((n.x * g.x + n.y * g.y + n.z * g.z) / nl);
    const f32 wd = std::exp2(-dS * dz);
    const f32 wn = std::pow(cosN, nP);
    return b * wd * wn * sat(x.w);
}

struct Level { V3 c; f32 conf = 0; V3 dc[2]; f32 dconf[2] = {}; };

Level upsample(const Taps& t, f32 zm, V3 n, f32 log2Depth, f32 log2Normal) {
    const f32 dS = std::exp2(log2Depth), nP = std::exp2(log2Normal);
    V3 sum, ds0, ds1;
    f32 wsum = 0, dw0 = 0, dw1 = 0;
    for (u32 i = 0; i < 4; ++i) {
        f32 dz, cosN;
        const f32 w = tapWeight(t.g[i], t.x[i], t.b[i], zm, n, dS, nP, dz, cosN);
        if (w <= 0.0f) continue;
        const f32 gd = -w * kLn2 * kLn2 * dS * dz;
        const f32 gn = w * std::log(std::max(cosN, 1.0e-30f)) * kLn2 * nP;
        const V3 x = rgb(t.x[i]);
        sum = sum + w * x; wsum += w;
        ds0 = ds0 + gd * x; dw0 += gd;
        ds1 = ds1 + gn * x; dw1 += gn;
    }
    Level L;
    L.conf = wsum;
    L.dconf[0] = dw0;
    L.dconf[1] = dw1;
    if (wsum > 1.0e-6f) {
        L.c = sum / wsum;
        L.dc[0] = (ds0 - dw0 * L.c) / wsum;
        L.dc[1] = (ds1 - dw1 * L.c) / wsum;
    }
    return L;
}

// nrd2Combine (forward only).
V3 combine(V3 c0, bool own, const Level lv[3], const Params& p, V3 extra) {
    const f32 l0 = lum(c0), l1 = lum(lv[0].c), l2 = lum(lv[1].c), l3 = lum(lv[2].c);
    const f32 ref = lv[2].conf > 0.05f ? l3 : lv[1].conf > 0.05f ? l2 : lv[0].conf > 0.05f ? l1 : l0;
    const f32 eps = std::max(ref, 0.0f) * 1.0e-3f + 1.0e-7f;
    const f32 sL = std::exp2(p.log2Lum);
    const f32 lr = std::log2(std::max(ref, 0.0f) + eps);
    auto lumw = [&](f32 l) { return std::exp2(-sL * std::min(std::fabs(std::log2(std::max(l, 0.0f) + eps) - lr), 16.0f)); };
    const f32 ex[3] = {extra.x, extra.y, extra.z};
    const f32 w0 = own ? lumw(l0) : 0.0f;
    f32 w[3];
    const f32 ls[3] = {l1, l2, l3};
    for (u32 k = 0; k < 3; ++k)
        w[k] = std::exp(std::clamp(p.logit[k] + ex[k], -16.0f, 16.0f)) * sat(lv[k].conf) * lumw(ls[k]);
    const f32 wsum = w0 + w[0] + w[1] + w[2];
    if (!(wsum > 1.0e-8f)) return own ? c0 : V3{};
    return (w0 * c0 + w[0] * lv[0].c + w[1] * lv[1].c + w[2] * lv[2].c) / wsum;
}

// nrd2ResolveBackward.
V3 combineBackward(V3 c0, bool own, const Level lv[3], const Params& p, V3 extra, V3 dOut[6]) {
    for (u32 z = 0; z < 6; ++z) dOut[z] = {};
    const f32 conf[3] = {lv[0].conf, lv[1].conf, lv[2].conf};
    f32 l[4];
    l[0] = lum(c0);
    for (u32 a = 0; a < 3; ++a) l[a + 1] = lum(lv[a].c);
    const u32 r = conf[2] > 0.05f ? 3u : conf[1] > 0.05f ? 2u : conf[0] > 0.05f ? 1u : 0u;
    const f32 ref = l[r];
    const f32 eps = std::max(ref, 0.0f) * 1.0e-3f + 1.0e-7f;
    const f32 sL = std::exp2(p.log2Lum);
    const f32 lr = std::log2(std::max(ref, 0.0f) + eps);
    const f32 logitIn[3] = {p.logit[0] + extra.x, p.logit[1] + extra.y, p.logit[2] + extra.z};
    f32 w[4], m[4], e[3] = {}, s[3] = {};
    V3 c[4];
    c[0] = c0;
    for (u32 k = 0; k < 4; ++k) {
        if (k > 0) c[k] = lv[k - 1].c;
        m[k] = std::min(std::fabs(std::log2(std::max(l[k], 0.0f) + eps) - lr), 16.0f);
        const f32 lw = std::exp2(-sL * m[k]);
        if (k == 0) { w[0] = own ? lw : 0.0f; continue; }
        e[k - 1] = std::exp(std::clamp(logitIn[k - 1], -16.0f, 16.0f));
        s[k - 1] = sat(conf[k - 1]);
        w[k] = e[k - 1] * s[k - 1] * lw;
    }
    const f32 wsum = w[0] + w[1] + w[2] + w[3];
    if (!(wsum > 1.0e-8f)) return own ? c0 : V3{};
    const V3 out = (w[0] * c0 + w[1] * c[1] + w[2] * c[2] + w[3] * c[3]) / wsum;
    for (u32 j = 0; j < 6; ++j) {
        f32 dl[4], dconf[3];
        V3 dc[4];
        dl[0] = 0.0f;
        for (u32 b = 0; b < 3; ++b) {
            const u32 g = j == 3 ? 0u : 1u;
            const bool geo = j == 3 || j == 4;
            dc[b + 1] = geo ? lv[b].dc[g] : V3{};
            dconf[b] = geo ? lv[b].dconf[g] : 0.0f;
            dl[b + 1] = lum(dc[b + 1]);
        }
        const f32 dref = dl[r];
        const f32 deps = ref > 0.0f ? 1.0e-3f * dref : 0.0f;
        const f32 dlr = ((ref > 0.0f ? dref : 0.0f) + deps) / ((std::max(ref, 0.0f) + eps) * kLn2);
        f32 dW = 0.0f;
        V3 dNum;
        for (u32 k2 = 0; k2 < 4; ++k2) {
            const f32 a = std::max(l[k2], 0.0f) + eps;
            const f32 u = std::log2(a) - lr;
            const f32 du = ((l[k2] > 0.0f ? dl[k2] : 0.0f) + deps) / (a * kLn2) - dlr;
            const f32 dm = std::fabs(u) < 16.0f ? (u > 0.0f ? du : u < 0.0f ? -du : 0.0f) : 0.0f;
            const f32 lw = std::exp2(-sL * m[k2]);
            const f32 dlw = -kLn2 * lw * (sL * dm + (j == 5 ? kLn2 * sL * m[k2] : 0.0f));
            f32 dw;
            if (k2 == 0) {
                dw = own ? dlw : 0.0f;
            } else {
                const u32 i = k2 - 1;
                const f32 de = (j == i && std::fabs(logitIn[i]) < 16.0f) ? e[i] : 0.0f;
                const f32 ds = (conf[i] > 0.0f && conf[i] < 1.0f) ? dconf[i] : 0.0f;
                dw = de * s[i] * lw + e[i] * ds * lw + e[i] * s[i] * dlw;
            }
            dW += dw;
            dNum = dNum + dw * c[k2] + w[k2] * (k2 == 0 ? V3{} : dc[k2]);
        }
        dOut[j] = (dNum - dW * out) / wsum;
    }
    return out;
}

V3 specularExtraLogit(f32 rough, f32 hitT, f32 viewZ) {
    const f32 g = sat((rough - 0.05f) / 0.3f);
    const f32 contact = hitT > 0.0f ? 1.0f - sat(hitT / std::max(0.3f * viewZ, 1.0e-3f)) : 0.0f;
    const f32 a = -3.0f * (1.0f - g), b = -2.0f * contact;
    return {a * 1.0f + b, a * 2.0f + b, a * 3.0f + b};
}

f32 regulariser(const f32 t[6], const f32 t0[6]) {
    f32 r = 0.0f;
    for (u32 k = 0; k < 6; ++k) r += (t[k] - t0[k]) * (t[k] - t0[k]);
    return r;
}

}  // namespace

void nrd2BuildPyramid(const Nrd2Frame& f, Nrd2Pyramid& out, bool half) {
    const u32 tx = (f.width + 7) / 8, ty = (f.height + 7) / 8;
    for (u32 l = 0; l < 3; ++l) {
        out.width[l] = tx * (4u >> l);
        out.height[l] = ty * (4u >> l);
        const usize n = static_cast<usize>(out.width[l]) * out.height[l] * 4;
        out.guide[l].assign(n, 0.0f);
        out.value[0][l].assign(n, 0.0f);
        out.value[1][l].assign(n, 0.0f);
    }
    V4 G[64], D[64], S[64];
    for (u32 gy = 0; gy < ty; ++gy) for (u32 gx = 0; gx < tx; ++gx) {
        for (u32 i = 0; i < 64; ++i) {
            const u32 qx = gx * 8 + (i & 7), qy = gy * 8 + (i >> 3);
            const bool inside = qx < f.width && qy < f.height;
            const usize p = static_cast<usize>(std::min(qy, std::max(f.height, 1u) - 1)) * f.width +
                            std::min(qx, std::max(f.width, 1u) - 1);
            const f32 z = f.viewZ[p];
            const V4 d = load4(f.d, p), s = load4(f.s, p);
            const bool surf = inside && z > 0.0f && z < 1.0e6f;
            if (surf) { const V3 n = decodeNormal(load4(f.normal, p)); G[i] = {n.x, n.y, n.z, z * 0.01f}; }
            else      G[i] = {};
            D[i] = (surf && d.w > 0.5f && finite3(d)) ? V4{d.x, d.y, d.z, 1.0f} : V4{};
            S[i] = (surf && finite3(s)) ? V4{s.x, s.y, s.z, 1.0f} : V4{};
        }
        auto write = [&](u32 l, u32 i, u32 shift, const V4& g, const V4& dv, const V4& sv) {
            const u32 tqx = (gx * 8 + (i & 7)) >> shift, tqy = (gy * 8 + (i >> 3)) >> shift;
            const usize at = static_cast<usize>(tqy) * out.width[l] + tqx;
            store(out.guide[l], at, g, half);
            store(out.value[0][l], at, dv, half);
            store(out.value[1][l], at, sv, half);
        };
        V4 g, dv, sv;
        for (u32 i = 0; i < 64; ++i) {
            if (((i & 7) | (i >> 3)) & 1u) continue;
            const u32 idx[4] = {i, i + 1, i + 8, i + 9};
            reduce(G, D, S, idx, g, dv, sv);
            write(0, i, 1, g, dv, sv);
            G[i] = g; D[i] = dv; S[i] = sv;
        }
        for (u32 i = 0; i < 64; ++i) {
            if (((i & 7) | (i >> 3)) & 3u) continue;
            const u32 idx[4] = {i, i + 2, i + 16, i + 18};
            reduce(G, D, S, idx, g, dv, sv);
            write(1, i, 2, g, dv, sv);
            G[i] = g; D[i] = dv; S[i] = sv;
        }
        const u32 idx[4] = {0, 4, 32, 36};
        reduce(G, D, S, idx, g, dv, sv);
        write(2, 0, 3, g, dv, sv);
    }
}

std::array<f32, 6> nrd2DefaultTheta(u32 signal) {
    const Nrd2Params d{};
    std::array<f32, 6> t{};
    for (u32 k = 0; k < 6; ++k) t[k] = signal ? d.specular[k] : d.diffuse[k];
    return t;
}

std::array<f32, 3> nrd2ResolvePixel(const Nrd2Frame& f, const Nrd2Pyramid& py, u32 x, u32 y, u32 signal,
                                    const f32 theta[6], f32 (*dOut)[3]) {
    if (dOut) for (u32 k = 0; k < 6; ++k) dOut[k][0] = dOut[k][1] = dOut[k][2] = 0.0f;
    const usize i = static_cast<usize>(y) * f.width + x;
    const f32 z = f.viewZ[i];
    if (!(z > 0.0f && z < 1.0e6f)) return {0.0f, 0.0f, 0.0f};
    const V4 nr = load4(f.normal, i);
    const V3 n = decodeNormal(nr);
    const V4 v0 = signal ? load4(f.s, i) : load4(f.d, i);
    const bool own = signal ? (!isNan(v0.x) && !isNan(v0.y) && !isNan(v0.z))
                            : (v0.w > 0.5f && !isNan(v0.x) && !isNan(v0.y) && !isNan(v0.z));
    const std::array<f32, 6> def = nrd2DefaultTheta(signal);
    const Params p = sanitise(theta, def.data());
    const f32 zm = z * 0.01f;
    Level lv[3];
    for (u32 l = 0; l < 3; ++l) lv[l] = upsample(loadTaps(py, signal, l + 1, x, y), zm, n, p.log2Depth, p.log2Normal);
    const V3 extra = signal ? specularExtraLogit(nr.z, v0.w, z) : V3{};
    V3 out;
    if (dOut) {
        V3 d[6];
        out = combineBackward(rgb(v0), own, lv, p, extra, d);
        for (u32 k = 0; k < 6; ++k) { dOut[k][0] = d[k].x; dOut[k][1] = d[k].y; dOut[k][2] = d[k].z; }
    } else {
        out = combine(rgb(v0), own, lv, p, extra);
    }
    return {out.x, out.y, out.z};
}

f32 nrd2StabMaxFrames(f32 speed, f32 nStill, f32 nFast) {
    if (!(speed < 128.0f)) return 0.0f;
    const f32 t = sat(std::log2(std::max(speed, 0.25f) / 0.25f) / std::log2(8.0f / 0.25f));
    const f32 hi = std::max(nStill, 1.0f);
    const f32 a = std::log2(hi), b = std::log2(std::clamp(nFast, 1.0f, hi));
    return std::exp2(a + (b - a) * t);
}

f32 nrd2StabAlpha(f32 age, f32 nMax) {
    return sat(1.0f - 1.0f / std::max(std::min(age + 1.0f, nMax), 1.0f));
}

void nrd2StabBox(const std::vector<std::array<f32, 3>>& values, f32 lo[3], f32 hi[3]) {
    for (u32 c = 0; c < 3; ++c) { lo[c] = 3.0e38f; hi[c] = -3.0e38f; }
    for (const auto& v : values)
        for (u32 c = 0; c < 3; ++c) { lo[c] = std::min(lo[c], v[c]); hi[c] = std::max(hi[c], v[c]); }
}

Nrd2StabOut nrd2StabilisePixel(const Nrd2StabIn& in) {
    f32 wsum = 0.0f, nAge = 1.0e9f;
    f32 hd[3] = {}, hs[3] = {};
    for (const Nrd2StabTap& t : in.tap) {
        if (!(in.historyValid && t.b > 0.0f && t.age > 0.0f && std::fabs(t.zm - in.zExp) <= in.zTol)) continue;
        wsum += t.b;
        for (u32 c = 0; c < 3; ++c) { hd[c] += t.b * t.d[c]; hs[c] += t.b * t.s[c]; }
        if (t.b > 0.1f) nAge = std::min(nAge, t.age);
    }
    Nrd2StabOut o;
    if (wsum >= 0.5f && in.nMax > 0.0f) {
        o.alphaD = nrd2StabAlpha(nAge, in.nMax);
        o.alphaS = o.alphaD * sat((in.roughness - 0.35f) / 0.3f);
        o.age = std::min(nAge + 1.0f, 255.0f);
        for (u32 c = 0; c < 3; ++c) {
            hd[c] = std::clamp(hd[c] / wsum, in.loD[c], in.hiD[c]);
            hs[c] = std::clamp(hs[c] / wsum, in.loS[c], in.hiS[c]);
        }
    }
    for (u32 c = 0; c < 3; ++c) {
        o.d[c] = in.curD[c] + (hd[c] - in.curD[c]) * o.alphaD;
        o.s[c] = in.curS[c] + (hs[c] - in.curS[c]) * o.alphaS;
    }
    return o;
}

void nrd2ClampTheta(f32 t[6]) {
    for (u32 k = 0; k < 3; ++k) t[k] = std::clamp(t[k], -16.0f, 16.0f);
    for (u32 k = 3; k < 6; ++k) t[k] = std::clamp(t[k], -8.0f, 8.0f);
}

void nrd2GridCandidate(const f32 theta0[6], u32 c, f32 out[6]) {
    static const f32 kLogit[3] = {-3.0f, 0.0f, 3.0f}, kDepth[3] = {-1.5f, 0.0f, 1.5f}, kLum[3] = {-2.0f, 0.0f, 2.0f};
    for (u32 k = 0; k < 6; ++k) out[k] = theta0[k];
    for (u32 k = 0; k < 3; ++k) out[k] += kLogit[c % 3];
    out[3] += kDepth[(c / 3) % 3];
    out[5] += kLum[(c / 9) % 3];
    nrd2ClampTheta(out);
}

void nrd2PatternCandidate(const f32 theta[6], f32 step, u32 c, f32 out[6]) {
    for (u32 k = 0; k < 6; ++k) out[k] = theta[k];
    out[(c / 2) % 6] += (c & 1u) ? -step : step;
    nrd2ClampTheta(out);
}

f32 nrd2TileWeight(f32 validFraction, f32 r) {
    return sat(validFraction) / (1.0f + std::max(r, 0.0f) / kNrd2SplitHalfR0);
}

f32 nrd2TileLoss(const Nrd2TileProblem& p, const f32 theta[6], f32 grad[6], u32* countOut) {
    if (grad) for (u32 k = 0; k < 6; ++k) grad[k] = 0.0f;
    const std::vector<Nrd2Frame>& frames = *p.frames;
    const u32 K = static_cast<u32>(frames.size());
    const Nrd2Frame& f0 = frames[0];
    std::vector<usize> px;
    for (u32 y = p.tileY * 8; y < std::min(p.tileY * 8 + 8, f0.height); ++y)
        for (u32 x = p.tileX * 8; x < std::min(p.tileX * 8 + 8, f0.width); ++x) {
            const usize i = static_cast<usize>(y) * f0.width + x;
            const f32 z = f0.viewZ[i];
            if (!(z > 0.0f && z < 1.0e6f)) continue;
            if (p.signal == 0 && !(f0.d[4 * i + 3] > 0.5f)) continue;
            px.push_back(i);
        }
    if (countOut) *countOut = static_cast<u32>(px.size());
    if (px.empty() || K == 0) return 0.0f;
    f32 lsum = 0.0f;
    for (u32 k = 0; k < K; ++k)
        for (usize i : px) {
            const std::vector<f32>& T = (*p.targets)[k];
            lsum += lum({T[3 * i], T[3 * i + 1], T[3 * i + 2]});
        }
    const f32 norm = static_cast<f32>(K) * static_cast<f32>(px.size());
    const f32 meanLum = lsum / norm;
    const f32 denom = meanLum * meanLum + kNrd2LossEps;
    f32 loss = 0.0f;
    f32 d[6][3];
    for (u32 k = 0; k < K; ++k)
        for (usize i : px) {
            const u32 x = static_cast<u32>(i % f0.width), y = static_cast<u32>(i / f0.width);
            const std::array<f32, 3> o = nrd2ResolvePixel(frames[k], (*p.pyramids)[k], x, y, p.signal, theta,
                                                          grad ? d : nullptr);
            const std::vector<f32>& T = (*p.targets)[k];
            f32 r[3];
            for (u32 c = 0; c < 3; ++c) { r[c] = o[c] - T[3 * i + c]; loss += r[c] * r[c] / denom; }
            if (grad)
                for (u32 j = 0; j < 6; ++j)
                    grad[j] += 2.0f * (r[0] * d[j][0] + r[1] * d[j][1] + r[2] * d[j][2]) / denom;
        }
    if (grad) for (u32 j = 0; j < 6; ++j) grad[j] /= norm;
    return loss / norm;
}

Nrd2TileFit nrd2FitTile(const Nrd2TileProblem& p, const f32 theta0[6], const Nrd2OracleDesc& d) {
    Nrd2TileFit fit;
    for (u32 k = 0; k < 6; ++k) fit.theta[k] = theta0[k];
    u32 count = 0;
    nrd2TileLoss(p, theta0, nullptr, &count);
    fit.count = count;
    if (!count) return fit;

    f32 theta[6], best[6], cand[6];
    f32 bestObj = 3.0e38f, bestData = 0.0f;
    for (u32 c = 0; c < kNrd2GridStarts; ++c) {
        nrd2GridCandidate(theta0, c, cand);
        const f32 data = nrd2TileLoss(p, cand);
        const f32 obj = data + d.lambda * regulariser(cand, theta0);
        if (c == kNrd2GridDefault) fit.defaultLoss = data;
        if (obj < bestObj) { bestObj = obj; bestData = data; std::copy(cand, cand + 6, best); }
    }
    std::copy(best, best + 6, theta);

    if (d.mode == Nrd2OracleMode::Grad) {
        f32 m[6] = {}, v[6] = {}, g[6];
        auto consider = [&](f32 data) {
            const f32 obj = data + d.lambda * regulariser(theta, theta0);
            if (obj < bestObj) { bestObj = obj; bestData = data; std::copy(theta, theta + 6, best); }
        };
        for (u32 t = 1; t <= d.adamIters; ++t) {
            consider(nrd2TileLoss(p, theta, g));
            const f32 bc1 = neural::adamBiasCorrection(d.beta1, t), bc2 = neural::adamBiasCorrection(d.beta2, t);
            for (u32 k = 0; k < 6; ++k) {
                const f32 gk = g[k] + 2.0f * d.lambda * (theta[k] - theta0[k]);
                m[k] = d.beta1 * m[k] + (1.0f - d.beta1) * gk;
                v[k] = d.beta2 * v[k] + (1.0f - d.beta2) * gk * gk;
                theta[k] -= d.lr * (m[k] / bc1) / (std::sqrt(v[k] / bc2) + d.adamEps);
            }
            nrd2ClampTheta(theta);
        }
        consider(nrd2TileLoss(p, theta));
    } else {
        f32 step = kNrd2PatternStep0;
        for (u32 it = 0; it < d.patternIters; ++it) {
            f32 roundObj = 3.0e38f, roundData = 0.0f, roundTheta[6] = {};
            for (u32 c = 0; c < kNrd2PatternCandidates; ++c) {
                nrd2PatternCandidate(theta, step, c, cand);
                const f32 data = nrd2TileLoss(p, cand);
                const f32 obj = data + d.lambda * regulariser(cand, theta0);
                if (obj < roundObj) { roundObj = obj; roundData = data; std::copy(cand, cand + 6, roundTheta); }
            }
            if (roundObj < bestObj) {
                bestObj = roundObj; bestData = roundData;
                std::copy(roundTheta, roundTheta + 6, best);
                std::copy(roundTheta, roundTheta + 6, theta);
            } else {
                step *= 0.5f;
            }
        }
    }
    std::copy(best, best + 6, fit.theta);
    fit.loss = bestData;
    return fit;
}

}  // namespace aver::render::denoise
