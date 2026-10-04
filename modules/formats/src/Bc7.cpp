// BC7 mode 6 encoder/decoder. See Bc7.hpp for why one mode.
#include "aver/formats/Bc7.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>

namespace aver::fmt {
namespace {

// Mode 6's 4-bit interpolation weights, out of 64 (the BC7 specification's table).
constexpr int kW4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

inline int interp(int e0, int e1, int w) { return ((64 - w) * e0 + w * e1 + 32) >> 6; }

// LSB-first bit packing, the order BC7 blocks are defined in.
struct BitWriter {
    u8* out;
    u32 pos = 0;
    void put(u32 v, u32 bits) {
        for (u32 i = 0; i < bits; ++i, ++pos)
            if ((v >> i) & 1u) out[pos >> 3] = u8(out[pos >> 3] | (1u << (pos & 7)));
    }
};
struct BitReader {
    const u8* in;
    u32 pos = 0;
    u32 get(u32 bits) {
        u32 v = 0;
        for (u32 i = 0; i < bits; ++i, ++pos) v |= u32((in[pos >> 3] >> (pos & 7)) & 1u) << i;
        return v;
    }
};

// Two endpoints as stored: 7-bit channels plus one p-bit each; the colour is (v << 1) | p.
struct Endpoints {
    int v[2][4];
    int p[2];
};
inline int full(const Endpoints& e, int k, int c) { return (e.v[k][c] << 1) | e.p[k]; }

struct Block {
    float px[16][4];
    float wrgb[16];   // colour error weight per texel; alpha always weighs 1
};

// Picks every texel's nearest palette entry; returns the weighted squared error.
float assignIndices(const Block& b, const Endpoints& e, u8 idx[16]) {
    float pal[16][4];
    for (int i = 0; i < 16; ++i)
        for (int c = 0; c < 4; ++c) pal[i][c] = float(interp(full(e, 0, c), full(e, 1, c), kW4[i]));
    float total = 0.0f;
    for (int t = 0; t < 16; ++t) {
        float best = 3.4e38f;
        int bi = 0;
        for (int i = 0; i < 16; ++i) {
            const float d0 = pal[i][0] - b.px[t][0], d1 = pal[i][1] - b.px[t][1];
            const float d2 = pal[i][2] - b.px[t][2], d3 = pal[i][3] - b.px[t][3];
            const float err = b.wrgb[t] * (d0 * d0 + d1 * d1 + d2 * d2) + d3 * d3;
            if (err < best) { best = err; bi = i; }
        }
        idx[t] = u8(bi);
        total += best;
    }
    return total;
}

// Quantises two float endpoints with the given p-bits (round to nearest representable).
Endpoints quantise(const float e0[4], const float e1[4], int p0, int p1) {
    Endpoints q{};
    q.p[0] = p0;
    q.p[1] = p1;
    for (int c = 0; c < 4; ++c) {
        q.v[0][c] = std::clamp(int(std::lround((e0[c] - float(p0)) * 0.5f)), 0, 127);
        q.v[1][c] = std::clamp(int(std::lround((e1[c] - float(p1)) * 0.5f)), 0, 127);
    }
    return q;
}

// The p-bit that represents one endpoint best on its own.
int bestPBit(const float e[4]) {
    float err[2] = {0, 0};
    for (int p = 0; p < 2; ++p)
        for (int c = 0; c < 4; ++c) {
            const int v = std::clamp(int(std::lround((e[c] - float(p)) * 0.5f)), 0, 127);
            const float d = float((v << 1) | p) - e[c];
            err[p] += d * d;
        }
    return err[1] < err[0] ? 1 : 0;
}

// Least-squares endpoints for fixed indices, per channel. False when the indices do not constrain
// both ends (every texel on one weight).
bool refit(const Block& b, const u8 idx[16], float e0[4], float e1[4]) {
    for (int c = 0; c < 4; ++c) {
        double A = 0, B = 0, C = 0, X0 = 0, X1 = 0;
        for (int t = 0; t < 16; ++t) {
            const double W = c < 3 ? double(b.wrgb[t]) : 1.0;
            const double w = kW4[idx[t]] / 64.0, iw = 1.0 - w;
            A += W * iw * iw;
            B += W * iw * w;
            C += W * w * w;
            X0 += W * iw * b.px[t][c];
            X1 += W * w * b.px[t][c];
        }
        const double det = A * C - B * B;
        if (std::fabs(det) < 1e-9) return false;
        e0[c] = float(std::clamp((C * X0 - B * X1) / det, 0.0, 255.0));
        e1[c] = float(std::clamp((A * X1 - B * X0) / det, 0.0, 255.0));
    }
    return true;
}

} // namespace

void encodeBc7Block(const u8 rgba[64], u8 out[16], const Bc7Options& opt) {
    Block b;
    float wsum = 0.0f, mean[4] = {0, 0, 0, 0};
    for (int t = 0; t < 16; ++t) {
        for (int c = 0; c < 4; ++c) b.px[t][c] = float(rgba[t * 4 + c]);
        // A texel the cutout discards is still ENCODED (its alpha must stay below the cut), but its
        // colour is invisible; a small weight rather than zero keeps the fit defined when a whole
        // block is cut away.
        b.wrgb[t] = (opt.cutout && rgba[t * 4 + 3] < opt.cutoff) ? 0.02f : 1.0f;
        wsum += b.wrgb[t];
        for (int c = 0; c < 3; ++c) mean[c] += b.wrgb[t] * b.px[t][c];
        mean[3] += b.px[t][3];
    }
    for (int c = 0; c < 3; ++c) mean[c] /= wsum;
    mean[3] /= 16.0f;

    // Principal axis by power iteration. Invisible texels take the visible mean colour first so they
    // cannot stretch the axis toward a colour nobody sees.
    float v[16][4];
    for (int t = 0; t < 16; ++t)
        for (int c = 0; c < 4; ++c)
            v[t][c] = (c < 3 && b.wrgb[t] < 1.0f ? mean[c] : b.px[t][c]) - mean[c];
    float cov[4][4] = {};
    for (int t = 0; t < 16; ++t)
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) cov[i][j] += v[t][i] * v[t][j];
    float axis[4] = {1.0f, 1.0f, 1.0f, 0.5f};
    for (int it = 0; it < 8; ++it) {
        float n[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) n[i] += cov[i][j] * axis[j];
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2] + n[3] * n[3]);
        if (len < 1e-6f) break;
        for (int i = 0; i < 4; ++i) axis[i] = n[i] / len;
    }
    float tmin = 0.0f, tmax = 0.0f;
    for (int t = 0; t < 16; ++t) {
        const float d = v[t][0] * axis[0] + v[t][1] * axis[1] + v[t][2] * axis[2] + v[t][3] * axis[3];
        tmin = std::min(tmin, d);
        tmax = std::max(tmax, d);
    }
    float e0[4], e1[4];
    for (int c = 0; c < 4; ++c) {
        e0[c] = std::clamp(mean[c] + tmin * axis[c], 0.0f, 255.0f);
        e1[c] = std::clamp(mean[c] + tmax * axis[c], 0.0f, 255.0f);
    }

    Endpoints best{};
    u8 bestIdx[16] = {};
    float bestErr = 3.4e38f;
    const auto tryEndpoints = [&](const Endpoints& q) {
        u8 idx[16];
        const float err = assignIndices(b, q, idx);
        if (err < bestErr) {
            bestErr = err;
            best = q;
            std::memcpy(bestIdx, idx, 16);
        }
    };
    for (int pass = 0; pass < 3; ++pass) {
        if (pass < 2) {
            tryEndpoints(quantise(e0, e1, bestPBit(e0), bestPBit(e1)));
        } else {
            for (int pc = 0; pc < 4; ++pc) tryEndpoints(quantise(e0, e1, pc & 1, pc >> 1));
        }
        if (bestErr == 0.0f || !refit(b, bestIdx, e0, e1)) break;
    }

    // The anchor texel's index MSB is implicit zero: flip the endpoints when it would be set.
    if (bestIdx[0] & 8) {
        for (int c = 0; c < 4; ++c) std::swap(best.v[0][c], best.v[1][c]);
        std::swap(best.p[0], best.p[1]);
        for (u8& i : bestIdx) i = u8(15 - i);
    }

    std::memset(out, 0, 16);
    BitWriter w{out};
    w.put(1u << 6, 7);                                   // mode 6: six zero bits, then a one
    for (int c = 0; c < 4; ++c) { w.put(u32(best.v[0][c]), 7); w.put(u32(best.v[1][c]), 7); }
    w.put(u32(best.p[0]), 1);
    w.put(u32(best.p[1]), 1);
    w.put(bestIdx[0], 3);
    for (int t = 1; t < 16; ++t) w.put(bestIdx[t], 4);
}

void decodeBc7Block(const u8 in[16], u8 rgba[64]) {
    if ((in[0] & 0x7F) != 0x40) {
        for (int t = 0; t < 16; ++t) { rgba[t * 4] = 255; rgba[t * 4 + 1] = 0; rgba[t * 4 + 2] = 255; rgba[t * 4 + 3] = 255; }
        return;
    }
    BitReader r{in};
    r.get(7);
    int e[2][4];
    for (int c = 0; c < 4; ++c) { e[0][c] = int(r.get(7)); e[1][c] = int(r.get(7)); }
    const int p0 = int(r.get(1)), p1 = int(r.get(1));
    for (int c = 0; c < 4; ++c) { e[0][c] = (e[0][c] << 1) | p0; e[1][c] = (e[1][c] << 1) | p1; }
    for (int t = 0; t < 16; ++t) {
        const int i = int(r.get(t == 0 ? 3 : 4));
        for (int c = 0; c < 4; ++c) rgba[t * 4 + c] = u8(interp(e[0][c], e[1][c], kW4[i]));
    }
}

void encodeBc7Image(const ImageData& img, std::vector<u8>& out, const Bc7Options& opt, u32 threads) {
    const u32 bx = (img.width + 3) / 4, by = (img.height + 3) / 4;
    out.assign(usize(bx) * by * 16, 0);
    if (!img.width || !img.height) return;
    const auto row = [&](u32 y) {
        u8 texels[64];
        for (u32 x = 0; x < bx; ++x) {
            for (u32 j = 0; j < 4; ++j)
                for (u32 i = 0; i < 4; ++i) {
                    const u32 sx = std::min(x * 4 + i, img.width - 1), sy = std::min(y * 4 + j, img.height - 1);
                    std::memcpy(&texels[(j * 4 + i) * 4], &img.pixels[(usize(sy) * img.width + sx) * 4], 4);
                }
            encodeBc7Block(texels, &out[(usize(y) * bx + x) * 16], opt);
        }
    };
    u32 n = threads ? threads : std::max(1u, std::thread::hardware_concurrency());
    n = std::min(n, by);
    if (n <= 1 || usize(bx) * by < 256) {
        for (u32 y = 0; y < by; ++y) row(y);
        return;
    }
    std::atomic<u32> next{0};
    std::vector<std::thread> pool;
    pool.reserve(n);
    for (u32 k = 0; k < n; ++k)
        pool.emplace_back([&] { for (u32 y; (y = next.fetch_add(1)) < by;) row(y); });
    for (std::thread& t : pool) t.join();
}

} // namespace aver::fmt
