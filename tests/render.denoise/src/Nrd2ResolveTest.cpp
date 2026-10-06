// Nrd2ResolveTest: NRD2's CPU twin (Nrd2ResolveReference, the spec for nrd2_resolve.hlsli and
// nrd2_capture.hlsl) and the pose file (Nrd2Dataset). Headless: no GPU, no device.
#include "aver/render/denoise/Nrd2Dataset.hpp"
#include "aver/render/denoise/Nrd2ResolveReference.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::render::denoise;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

namespace {

struct Rng {
    u32 s;
    explicit Rng(u32 seed) : s(seed ? seed : 1u) {}
    u32 next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    f32 uniform() { return static_cast<f32>(next() >> 8) * (1.0f / 16777216.0f); }
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * uniform(); }
};

// voxi.hlsl's averPackNormalRoughness.
void packNormal(f32 nx, f32 ny, f32 nz, f32 rough, f32* out) {
    const f32 l1 = std::fabs(nx) + std::fabs(ny) + std::fabs(nz);
    nx /= l1; ny /= l1; nz /= l1;
    f32 px = nx, py = ny;
    if (nz < 0.0f) {
        px = (1.0f - std::fabs(ny)) * (nx >= 0.0f ? 1.0f : -1.0f);
        py = (1.0f - std::fabs(nx)) * (ny >= 0.0f ? 1.0f : -1.0f);
    }
    out[0] = px * 0.5f + 0.5f; out[1] = py * 0.5f + 0.5f; out[2] = std::clamp(rough, 0.0f, 1.0f); out[3] = 0.0f;
}

// A clean scene: two depth planes (a step at x = 0.55 w), a tilted normal field, a sky corner, a few
// pixels without albedo; smooth diffuse and specular with an edge in brightness on the depth step.
Nrd2Frame cleanFrame(u32 w, u32 h) {
    Nrd2Frame f;
    f.width = w; f.height = h;
    const usize n = static_cast<usize>(w) * h;
    f.d.assign(4 * n, 0.0f); f.s.assign(4 * n, 0.0f); f.viewZ.assign(n, 0.0f); f.normal.assign(4 * n, 0.0f);
    for (u32 y = 0; y < h; ++y) for (u32 x = 0; x < w; ++x) {
        const usize i = static_cast<usize>(y) * w + x;
        const f32 u = static_cast<f32>(x) / w, v = static_cast<f32>(y) / h;
        const bool far = u > 0.55f;
        f.viewZ[i] = (x + y < 5) ? 0.0f : far ? 900.0f + 200.0f * v : 500.0f + 60.0f * u;
        const f32 nx = far ? 0.6f : 0.15f * std::sin(6.0f * v), ny = 0.2f * u, nz = 1.0f;
        const f32 nl = std::sqrt(nx * nx + ny * ny + nz * nz);
        packNormal(nx / nl, ny / nl, nz / nl, far ? 0.6f : 0.25f + 0.3f * v, &f.normal[4 * i]);
        const f32 bd = far ? 0.3f : 1.2f;
        f.d[4 * i + 0] = bd * (0.8f + 0.2f * u); f.d[4 * i + 1] = bd * (0.7f + 0.3f * v); f.d[4 * i + 2] = bd * 0.6f;
        f.d[4 * i + 3] = ((x * 7 + y * 3) % 23 == 0) ? 0.0f : 1.0f;
        const f32 bs = far ? 0.8f : 0.2f;
        f.s[4 * i + 0] = bs * (0.5f + v); f.s[4 * i + 1] = bs; f.s[4 * i + 2] = bs * (1.0f - 0.3f * u);
        f.s[4 * i + 3] = far ? 300.0f : 80.0f;
    }
    return f;
}

// Multiplicative noise with rare fireflies, on rgb only.
Nrd2Frame noisy(const Nrd2Frame& c, Rng& rng) {
    Nrd2Frame f = c;
    for (std::vector<f32>* v : {&f.d, &f.s})
        for (usize i = 0; i < v->size() / 4; ++i) {
            f32 m = rng.range(0.4f, 1.6f);
            if (rng.uniform() < 0.01f) m *= 12.0f;
            for (u32 k = 0; k < 3; ++k) (*v)[4 * i + k] *= m;
        }
    return f;
}

bool surface(const Nrd2Frame& f, usize i) { return f.viewZ[i] > 0.0f && f.viewZ[i] < 1.0e6f; }

void testHalf() {
    AVER_INFO("fp16 conversion");
    bool ok = true;
    const f32 exact[] = {0.0f, 1.0f, -2.5f, 65504.0f, 6.103515625e-05f, 5.9604645e-08f, 0.333251953125f};
    for (f32 v : exact) ok = ok && nrd2F16ToF32(nrd2F32ToF16(v)) == v;
    check(ok, "exactly representable values round-trip");
    check(nrd2F32ToF16(1.0f) == 0x3C00u && nrd2F32ToF16(-2.0f) == 0xC000u, "1 and -2 encode as IEEE half");
    check(nrd2F32ToF16(1.0f + 1.0f / 2048.0f) == 0x3C00u, "a tie rounds to even (down)");
    check(nrd2F32ToF16(1.0f + 3.0f / 2048.0f) == 0x3C02u, "a tie rounds to even (up)");
    check(nrd2F32ToF16(70000.0f) == 0x7C00u && nrd2F32ToF16(1e-9f) == 0u, "overflow to inf, underflow to zero");
    const f32 nan = std::nanf("");
    check(std::isnan(nrd2F16ToF32(nrd2F32ToF16(nan))), "NaN stays NaN");
    Rng rng(7);
    f32 worst = 0.0f;
    for (u32 i = 0; i < 10000; ++i) {
        const f32 v = rng.range(-1000.0f, 1000.0f);
        worst = std::max(worst, std::fabs(nrd2F16ToF32(nrd2F32ToF16(v)) - v) / std::max(std::fabs(v), 1e-3f));
    }
    check(worst <= 1.0f / 2048.0f + 1e-7f, "random values within half an ulp (rel " + std::to_string(worst) + ")");
}

void testPyramidConstant() {
    AVER_INFO("pyramid of a constant surface");
    Nrd2Frame f = cleanFrame(20, 12);
    for (usize i = 0; i < f.viewZ.size(); ++i) {
        f.viewZ[i] = 400.0f;
        packNormal(0, 0, 1, 0.5f, &f.normal[4 * i]);
        for (u32 k = 0; k < 3; ++k) { f.d[4 * i + k] = 0.5f; f.s[4 * i + k] = 2.0f; }
        f.d[4 * i + 3] = 1.0f;
    }
    Nrd2Pyramid p;
    nrd2BuildPyramid(f, p, false);
    check(p.width[0] == 12 && p.height[0] == 8 && p.width[2] == 3 && p.height[2] == 2, "level sizes are whole tiles");
    // Texel (0,0) of every level lies inside the viewport.
    bool ok = true;
    for (u32 l = 0; l < 3; ++l)
        ok = ok && std::fabs(p.value[0][l][0] - 0.5f) < 1e-6f && std::fabs(p.value[0][l][3] - 1.0f) < 1e-6f &&
             std::fabs(p.value[1][l][0] - 2.0f) < 1e-6f && std::fabs(p.guide[l][3] - 4.0f) < 1e-5f &&
             std::fabs(p.guide[l][2] - 1.0f) < 1e-6f;
    check(ok, "levels keep the value, full validity, the normal and view Z in metres");
    // Past the viewport (x >= 20 at full res): the 1/8 texel of tile x = 2 still has pixels 16..19.
    const usize edge = 2;   // level 3, texel (2, 0)
    check(p.value[0][2][4 * edge + 3] > 0.99f, "a partial tile's coarse texel is still valid");
}

void testEnergy() {
    AVER_INFO("energy: the resolve is a normalised weighted mean");
    Nrd2Frame f = cleanFrame(24, 16);
    for (usize i = 0; i < f.viewZ.size(); ++i) {
        for (u32 k = 0; k < 3; ++k) { f.d[4 * i + k] = 0.37f + 0.1f * k; f.s[4 * i + k] = 1.3f; }
        f.d[4 * i + 3] = 1.0f;
    }
    Nrd2Pyramid p;
    nrd2BuildPyramid(f, p, false);
    Rng rng(3);
    f32 worstVal = 0.0f, worstGrad = 0.0f;
    for (u32 trial = 0; trial < 200; ++trial) {
        f32 th[6];
        for (u32 k = 0; k < 6; ++k) th[k] = rng.range(-3.0f, 4.0f);
        const u32 x = rng.next() % f.width, y = rng.next() % f.height, sig = trial & 1u;
        if (!surface(f, static_cast<usize>(y) * f.width + x)) continue;
        f32 d[6][3];
        const auto o = nrd2ResolvePixel(f, p, x, y, sig, th, d);
        for (u32 c = 0; c < 3; ++c) {
            const f32 want = sig ? 1.3f : 0.37f + 0.1f * c;
            worstVal = std::max(worstVal, std::fabs(o[c] - want) / want);
            for (u32 k = 0; k < 6; ++k) worstGrad = std::max(worstGrad, std::fabs(d[k][c]));
        }
    }
    check(worstVal < 1e-5f, "equal candidates resolve to themselves for any parameters (rel " + std::to_string(worstVal) + ")");
    check(worstGrad < 1e-5f, "and nothing moves them: d(output)/d(theta) = 0 (" + std::to_string(worstGrad) + ")");

    // Noisy input: the output stays within the candidates' range per channel (convex combination).
    Rng nr(11);
    const Nrd2Frame n = noisy(cleanFrame(24, 16), nr);
    nrd2BuildPyramid(n, p, false);
    f32 hiD = 0.0f;
    for (usize i = 0; i < n.viewZ.size(); ++i) hiD = std::max(hiD, n.d[4 * i]);
    bool inside = true;
    for (u32 y = 0; y < n.height; ++y) for (u32 x = 0; x < n.width; ++x) {
        const f32 th[6] = {1, 2, 2, 4.5f, 3, -1};
        const auto o = nrd2ResolvePixel(n, p, x, y, 0, th);
        inside = inside && o[0] >= 0.0f && o[0] <= hiD * 1.0001f;
    }
    check(inside, "a noisy frame resolves inside its own value range");
}

void testFiniteDifferences() {
    AVER_INFO("backward against finite differences");
    Rng rng(5);
    const Nrd2Frame f = noisy(cleanFrame(40, 24), rng);
    Nrd2Pyramid p;
    nrd2BuildPyramid(f, p, true);
    u32 checks = 0, passes = 0, sameFwd = 0, fwd = 0, live[6] = {};
    f32 worstRel = 0.0f;
    for (u32 trial = 0; trial < 400; ++trial) {
        const u32 x = rng.next() % f.width, y = rng.next() % f.height, sig = trial & 1u;
        if (!surface(f, static_cast<usize>(y) * f.width + x)) continue;
        const auto def = nrd2DefaultTheta(sig);
        f32 th[6];
        for (u32 k = 0; k < 6; ++k) th[k] = def[k] + rng.range(-1.5f, 1.5f);
        f32 d[6][3];
        const auto o = nrd2ResolvePixel(f, p, x, y, sig, th, d);
        const auto o2 = nrd2ResolvePixel(f, p, x, y, sig, th);
        ++fwd;
        if (std::fabs(o[0] - o2[0]) <= 1e-6f * std::max(1.0f, std::fabs(o[0])) &&
            std::fabs(o[2] - o2[2]) <= 1e-6f * std::max(1.0f, std::fabs(o[2])))
            ++sameFwd;
        const f32 scale = std::max({std::fabs(o[0]), std::fabs(o[1]), std::fabs(o[2]), 1e-3f});
        for (u32 k = 0; k < 6; ++k) {
            const f32 h = 2e-3f;
            f32 tp[6], tm[6];
            std::copy(th, th + 6, tp); std::copy(th, th + 6, tm);
            tp[k] += h; tm[k] -= h;
            const auto a = nrd2ResolvePixel(f, p, x, y, sig, tp), b = nrd2ResolvePixel(f, p, x, y, sig, tm);
            for (u32 c = 0; c < 3; ++c) {
                const f32 fd = (a[c] - b[c]) / (2.0f * h);
                const f32 err = std::fabs(fd - d[k][c]);
                const f32 tol = 2e-2f * std::max(std::fabs(fd), std::fabs(d[k][c])) + 2e-3f * scale;
                ++checks;
                if (std::fabs(fd) > 1e-3f * scale) ++live[k];
                if (err <= tol) ++passes;
                else worstRel = std::max(worstRel, err / scale);
            }
        }
    }
    check(fwd > 100 && sameFwd == fwd, "backward's value equals the forward resolve (" + std::to_string(sameFwd) + "/" +
                                           std::to_string(fwd) + ")");
    const f32 rate = checks ? static_cast<f32>(passes) / static_cast<f32>(checks) : 0.0f;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "d(output)/d(theta) matches central differences on %u of %u (%.2f%%; worst miss %.3g of the output)",
                  passes, checks, 100.0 * rate, worstRel);
    check(checks > 3000 && rate >= 0.97f, buf);
    std::snprintf(buf, sizeof(buf), "every parameter moves the output somewhere (non-zero differences per parameter: %u %u %u %u %u %u)",
                  live[0], live[1], live[2], live[3], live[4], live[5]);
    check(*std::min_element(live, live + 6) > checks / 60, buf);
}

// Known per-tile parameters and noisy frames; targets are the resolve at the truth (+ slight noise).
void testOracle(Nrd2OracleMode mode) {
    AVER_INFO("oracle recovery ({})", mode == Nrd2OracleMode::Grad ? "gradient" : "grid");
    Rng rng(mode == Nrd2OracleMode::Grad ? 21u : 22u);
    const Nrd2Frame clean = cleanFrame(32, 24);
    std::vector<Nrd2Frame> frames;
    std::vector<Nrd2Pyramid> pyr(4);
    for (u32 k = 0; k < 4; ++k) {
        frames.push_back(noisy(clean, rng));
        nrd2BuildPyramid(frames[k], pyr[k], true);
    }
    const f32 truth[2][6] = {{2.5f, 3.0f, 1.0f, 5.5f, 2.0f, -2.0f}, {0.0f, 1.0f, 2.5f, 4.0f, 5.0f, -0.5f}};
    std::vector<std::vector<f32>> targets(4, std::vector<f32>(3 * frames[0].viewZ.size(), 0.0f));
    u32 better = 0, cases = 0;
    f32 errFit = 0.0f, errDef = 0.0f, lossRatio = 0.0f;
    const u32 tiles[4][2] = {{1, 0}, {3, 1}, {2, 2}, {0, 1}};
    Nrd2OracleDesc od;
    od.mode = mode;
    for (u32 sig = 0; sig < 2; ++sig) {
        for (u32 k = 0; k < 4; ++k)
            for (u32 y = 0; y < clean.height; ++y) for (u32 x = 0; x < clean.width; ++x) {
                const usize i = static_cast<usize>(y) * clean.width + x;
                const auto o = nrd2ResolvePixel(frames[k], pyr[k], x, y, sig, truth[sig]);
                for (u32 c = 0; c < 3; ++c) targets[k][3 * i + c] = o[c] * (1.0f + rng.range(-0.002f, 0.002f));
            }
        const auto def = nrd2DefaultTheta(sig);
        for (const auto& t : tiles) {
            Nrd2TileProblem pr;
            pr.frames = &frames; pr.pyramids = &pyr; pr.targets = &targets;
            pr.tileX = t[0]; pr.tileY = t[1]; pr.signal = sig;
            const Nrd2TileFit fit = nrd2FitTile(pr, def.data(), od);
            if (!fit.count) continue;
            ++cases;
            if (fit.loss < fit.defaultLoss) ++better;
            lossRatio += fit.loss / std::max(fit.defaultLoss, 1e-12f);
            // Logits and the luminance sensitivity are what this scene pins down; the geometry ones
            // only where the tile straddles the depth step.
            for (u32 k : {0u, 1u, 2u, 5u}) {
                errFit += std::fabs(fit.theta[k] - truth[sig][k]);
                errDef += std::fabs(def[k] - truth[sig][k]);
            }
            AVER_INFO("    tile {},{} {}: loss {:.3g} (default {:.3g}), theta {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f}",
                      t[0], t[1], sig ? "S" : "D", fit.loss, fit.defaultLoss, fit.theta[0], fit.theta[1], fit.theta[2],
                      fit.theta[3], fit.theta[4], fit.theta[5]);
        }
    }
    check(cases == 8 && better == cases, "the fit beats the default on every tile (" + std::to_string(better) + "/" +
                                             std::to_string(cases) + ")");
    char buf[160];
    std::snprintf(buf, sizeof(buf), "fitted loss averages %.3f of the default's", lossRatio / std::max(cases, 1u));
    check(lossRatio / std::max(cases, 1u) < 0.2f, buf);
    std::snprintf(buf, sizeof(buf), "parameters move toward the truth (mean |error| %.2f vs default %.2f)",
                  errFit / (4.0f * cases), errDef / (4.0f * cases));
    check(errFit < 0.5f * errDef, buf);
}

bool nearly(f32 a, f32 b, f32 tol = 1e-5f) { return std::fabs(a - b) <= tol; }

void testTemporal() {
    AVER_INFO("temporal stage");
    // History length: rest, ramp, fast, cut.
    check(nearly(nrd2StabMaxFrames(0.0f, 32.0f, 8.0f), 32.0f, 1e-3f) && nearly(nrd2StabMaxFrames(0.25f, 32.0f, 8.0f), 32.0f, 1e-3f),
          "full history length (32) at rest");
    check(nearly(nrd2StabMaxFrames(8.0f, 32.0f, 8.0f), 8.0f, 1e-3f) && nearly(nrd2StabMaxFrames(20.0f, 32.0f, 8.0f), 8.0f, 1e-3f),
          "min(8, N) from 8 px per frame");
    const f32 mid = nrd2StabMaxFrames(1.4142f, 32.0f, 8.0f);   // 2.5 of the 5 octaves: halfway in log space
    check(mid > 15.5f && mid < 16.5f, "log-space ramp between (" + std::to_string(mid) + ")");
    check(nrd2StabMaxFrames(100.0f, 32.0f, 8.0f) > 0.0f && nrd2StabMaxFrames(128.0f, 32.0f, 8.0f) == 0.0f &&
              nrd2StabMaxFrames(std::nanf(""), 32.0f, 8.0f) == 0.0f,
          "history kept through a fast pan, none from 128 px per frame or for a NaN speed");
    check(nearly(nrd2StabMaxFrames(0.0f, 2.0f, 8.0f), 2.0f) && nrd2StabMaxFrames(130.0f, 2.0f, 8.0f) == 0.0f,
          "the fast cap never exceeds the rest length");

    // Specular: glossy keeps 8 frames, rough takes the full count, never above the diffuse cap.
    check(nearly(nrd2StabSpecMaxFrames(32.0f, 0.0f), 8.0f) && nearly(nrd2StabSpecMaxFrames(32.0f, 1.0f), 32.0f, 1e-3f) &&
              nrd2StabSpecMaxFrames(32.0f, 0.02f) > 8.0f && nrd2StabSpecMaxFrames(4.0f, 0.0f) == 4.0f &&
              nrd2StabSpecMaxFrames(0.0f, 1.0f) == 0.0f,
          "specular cap: max(8, N (1 - exp(-100 r))), within the diffuse cap, none in a whip");

    // Sample count and blend weight: 1 from a disocclusion, +1 per frame, capped at N.
    f32 n = nrd2StabSamples(0.0f, 32.0f);
    bool ramp = n == 1.0f && nrd2StabBlend(n) == 0.0f;
    const f32 want[4] = {0.5f, 2.0f / 3.0f, 0.75f, 0.8f};
    for (u32 f = 0; f < 4; ++f) {
        n = nrd2StabSamples(n, 32.0f);
        ramp = ramp && nearly(nrd2StabBlend(n), want[f]);
    }
    check(ramp, "blend weight ramps 0, 1/2, 2/3, 3/4, 4/5 from a first frame");
    for (u32 f = 0; f < 80; ++f) n = nrd2StabSamples(n, 32.0f);
    check(n == 32.0f && nearly(nrd2StabBlend(n), 1.0f - 1.0f / 32.0f), "and caps at 1 - 1/32");
    check(nrd2StabSamples(20.0f, 8.0f) == 8.0f && nrd2StabSamples(20.0f, 0.0f) == 1.0f && nrd2StabBlend(nrd2StabSamples(20.0f, 0.0f)) == 0.0f,
          "a fast pan caps the count; a whip takes no history");

    {
        f32 glint[3] = {9.0f, 9.0f, 9.0f}, edge[3] = {1.5f, 1.5f, 1.5f};
        nrd2StabFirefly(glint, 1.0f);
        nrd2StabFirefly(edge, 1.0f);
        check(nearly(glint[0], 2.0f) && edge[0] == 1.5f,
              "firefly clamp: a lone glint drops to 2x its brightest neighbour; a pixel within 2x is untouched");
    }

    // Noise estimate: relative, bounded, floored at half the tile scale.
    check(nrd2StabRelVar(1.0f, 1.0f, 1.0f) == 0.0f && nearly(nrd2StabRelVar(1.0f, 0.0f, 2.0f), 1.0f) &&
              nearly(nrd2StabRelVar(0.1f, 0.0f, 2.0f), 0.01f) && nearly(nrd2StabRelVar(10.0f, 0.0f, 2.0f), 1.0f) &&
              nearly(nrd2StabRelVar(4.0f, 2.0f, 1.0f), 0.25f),
          "relative luminance change squared, in [0, 1]");

    // Anchor: a bright outlier is down-weighted against the tile, and the weight is unit-free.
    {
        std::vector<std::array<f32, 3>> tile(63, {1.0f, 1.0f, 1.0f});
        tile.push_back({100.0f, 100.0f, 100.0f});
        f32 a[3];
        nrd2TileAnchor(tile, a);
        const f32 plain = (63.0f + 100.0f) / 64.0f;
        check(a[0] > 0.99f && a[0] < 1.1f && a[0] < 0.5f * plain, "anchor of 63 ones and a 100: ~1, not the plain mean " + std::to_string(plain));
        std::vector<std::array<f32, 3>> scaled;
        for (const auto& v : tile) scaled.push_back({v[0] * 37.0f, v[1] * 37.0f, v[2] * 37.0f});
        f32 b[3];
        nrd2TileAnchor(scaled, b);
        check(nearly(b[0], 37.0f * a[0], 1e-3f), "the anchor scales with the signal (units do not matter)");
        check(nrd2AnchorWeight(0.1f, 1.0f) > nrd2AnchorWeight(1.0f, 1.0f) && nrd2AnchorWeight(1.0f, 1.0f) > nrd2AnchorWeight(10.0f, 1.0f) &&
                  nearly(nrd2AnchorWeight(1.0e6f, 1.0f), 0.01f) && nearly(nrd2AnchorWeight(5.0f * 3.0f, 3.0f), nrd2AnchorWeight(5.0f, 1.0f)),
              "anchor weight falls with brightness, floors at 0.01, depends on lum / scale only");
        f32 e[3];
        nrd2TileAnchor({}, e);
        check(e[0] == 0.0f, "an empty tile has no anchor");
    }

    // Prefilter weight: each stop cuts, the variance gain opens, the radiance floor keeps a sliver.
    {
        const f32 base = nrd2PrefilterWeight(1.0f, 0.0f, 0.0f, 1.0f);
        check(nearly(base, 1.0f - std::exp(-4.4f), 1e-4f), "an identical neighbour takes the variance gain alone");
        check(nrd2PrefilterWeight(0.99f, 0.0f, 0.0f, 1.0f) < 0.01f * base && nrd2PrefilterWeight(0.8f, 0.0f, 0.0f, 1.0f) < 1e-6f &&
                  nrd2PrefilterWeight(-0.5f, 0.0f, 0.0f, 1.0f) == 0.0f,
              "the weight falls with normal mismatch (cos^512)");
        check(nrd2PrefilterWeight(1.0f, 0.05f, 0.0f, 1.0f) < 0.3f * base && nrd2PrefilterWeight(1.0f, 0.5f, 0.0f, 1.0f) < 1e-5f,
              "and with relative depth error");
        check(nrd2PrefilterWeight(1.0f, 0.0f, 5.0f, 1.0f) < 0.1f * base &&
                  nearly(nrd2PrefilterWeight(1.0f, 0.0f, 1.0e4f, 0.0f), 0.01f * 0.1f, 1e-6f),
              "and with distance from the anchor, down to a 0.01 radiance floor");
        check(nrd2PrefilterWeight(1.0f, 0.0f, 0.0f, 0.0f) == 0.1f && nrd2PrefilterWeight(1.0f, 0.0f, 0.0f, 0.1f) > 0.1f &&
                  nrd2PrefilterWeight(1.0f, 0.0f, 0.0f, 1.0f) > nrd2PrefilterWeight(1.0f, 0.0f, 0.0f, 0.1f),
              "noisier pixels take more of their neighbours");
        check(nrd2PrefilterCentreWeight(0.0f, 1.0f) == 1.0f && nrd2PrefilterCentreWeight(5.0f, 0.0f) < nrd2PrefilterCentreWeight(1.0f, 0.0f),
              "the centre's own weight is its radiance stop");
    }

    // Temporal blend. The history is clipped to [min(lo, anchor), max(hi, anchor)], the blend ramps to 1 - 1/32.
    {
        const f32 cur[3] = {1.0f, 1.0f, 1.0f}, anc[3] = {1.0f, 1.0f, 1.0f}, lo[3] = {0.5f, 0.5f, 0.5f}, hi[3] = {3.0f, 3.0f, 3.0f};
        const f32 hist[3] = {2.0f, 2.0f, 2.0f};
        check(nearly(nrd2TemporalBlend(cur, anc, lo, hi, hist, 32.0f).out[1], 1.0f + 1.0f * (1.0f - 1.0f / 32.0f)) &&
                  nearly(nrd2TemporalBlend(cur, anc, lo, hi, hist, 2.0f).out[0], 1.5f) &&
                  nrd2TemporalBlend(cur, anc, lo, hi, hist, 1.0f).out[2] == 1.0f,
              "history weight 1 - 1/n: 31/32 at the cap, 1/2 at n = 2, none at n = 1");

        Rng rng(41);
        bool inside = true, histInside = true;
        for (u32 k = 0; k < 400; ++k) {
            f32 c[3], an[3], l[3], h[3], hs[3];
            for (u32 j = 0; j < 3; ++j) {
                l[j] = rng.range(0.1f, 1.0f); h[j] = l[j] + rng.range(0.0f, 1.0f);
                c[j] = rng.range(l[j], h[j]); an[j] = rng.range(0.0f, 2.0f); hs[j] = rng.range(-2.0f, 8.0f);
            }
            const Nrd2TemporalOut o = nrd2TemporalBlend(c, an, l, h, hs, rng.range(1.0f, 32.0f));
            for (u32 j = 0; j < 3; ++j) {
                const f32 bl = std::min(l[j], an[j]) - 1e-6f, bh = std::max(h[j], an[j]) + 1e-6f;
                inside = inside && o.out[j] >= bl && o.out[j] <= bh && o.cur[j] >= bl && o.cur[j] <= bh;
                histInside = histInside && o.hist[j] >= bl && o.hist[j] <= bh;
            }
        }
        check(inside && histInside, "the output and the clipped history stay inside the min/max box united with the anchor");

        const f32 spike[3] = {100.0f, 100.0f, 100.0f};
        check(nearly(nrd2TemporalBlend(cur, anc, lo, hi, spike, 32.0f).hist[0], 3.0f), "a history spike lands on the box edge");
        const f32 farAnchor[3] = {5.0f, 5.0f, 5.0f}, lo2[3] = {1.0f, 1.0f, 1.0f}, hi2[3] = {2.0f, 2.0f, 2.0f}, high[3] = {9.0f, 9.0f, 9.0f};
        const Nrd2TemporalOut u = nrd2TemporalBlend(lo2, farAnchor, lo2, hi2, high, 4.0f);
        check(nearly(u.hist[0], 5.0f) && nearly(u.cur[0], 1.0f + 4.0f * 0.2f),
              "the anchor widens the box; the current value moves toward it by 1/(n + 1)");
        const f32 c3[3] = {1.0f, 1.0f, 1.0f}, a3[3] = {3.0f, 3.0f, 3.0f}, lo3[3] = {0.0f, 0.0f, 0.0f}, hi3[3] = {10.0f, 10.0f, 10.0f};
        check(nearly(nrd2TemporalBlend(c3, a3, lo3, hi3, hist, 1.0f).out[0], 2.0f), "with no history (n = 1) the output is the current value pulled halfway to the anchor");
    }

    // The box helper.
    f32 lo[3], hi[3];
    nrd2StabBox({{1.0f, 5.0f, 2.0f}, {3.0f, 4.0f, 2.5f}, {2.0f, 6.0f, 0.5f}}, lo, hi);
    check(lo[0] == 1.0f && hi[0] == 3.0f && lo[1] == 4.0f && hi[1] == 6.0f && lo[2] == 0.5f && hi[2] == 2.5f,
          "the clamp box is the per-channel min and max");
}

void testCandidates() {
    AVER_INFO("grid, pattern and tile weight");
    const auto def = nrd2DefaultTheta(0);
    f32 c[6];
    nrd2GridCandidate(def.data(), kNrd2GridDefault, c);
    check(std::equal(c, c + 6, def.begin()), "grid candidate 13 is theta0");
    bool distinct = true;
    for (u32 a = 0; a < kNrd2GridStarts; ++a)
        for (u32 b = a + 1; b < kNrd2GridStarts; ++b) {
            f32 ca[6], cb[6];
            nrd2GridCandidate(def.data(), a, ca); nrd2GridCandidate(def.data(), b, cb);
            distinct = distinct && !std::equal(ca, ca + 6, cb);
        }
    check(distinct, "the 27 grid starts are distinct");
    nrd2PatternCandidate(def.data(), 0.5f, 7, c);
    check(c[3] == def[3] - 0.5f && c[0] == def[0], "pattern candidate 7 is -step on the depth sensitivity");
    f32 big[6] = {40, -40, 0, 9, -9, 0};
    nrd2ClampTheta(big);
    check(big[0] == 16 && big[1] == -16 && big[3] == 8 && big[4] == -8, "theta clamps to the shader's ranges");
    check(nrd2TileWeight(1.0f, 0.0f) == 1.0f && std::fabs(nrd2TileWeight(0.5f, kNrd2SplitHalfR0) - 0.25f) < 1e-6f,
          "tile weight = validity / (1 + r / r0)");
}

void testDataset() {
    AVER_INFO("pose file");
    Rng rng(9);
    Nrd2Pose p;
    p.stageBVersion = 1; p.scene = "NeonDistrict_Night"; p.poseIndex = 17; p.heldOut = true;
    p.tilesX = 5; p.tilesY = 3; p.halfW = 20; p.halfH = 12; p.frames = 4; p.channels = kNrd2FeatureCount;
    p.features.resize(static_cast<usize>(p.frames) * p.channels * p.halfW * p.halfH);
    for (u16& v : p.features) v = nrd2F32ToF16(rng.range(-4.0f, 4.0f));
    const usize tiles = 15;
    p.theta.resize(12 * tiles); p.weights.resize(2 * tiles); p.losses.resize(4 * tiles);
    for (f32& v : p.theta) v = rng.range(-8, 8);
    for (f32& v : p.weights) v = rng.uniform();
    for (f32& v : p.losses) v = rng.uniform();
    const std::string path = (std::filesystem::temp_directory_path() / "aver_nrd2_pose_test.n2p").string();
    std::string why;
    const bool wrote = writeNrd2Pose(path, p, &why);
    check(wrote, "writes " + why);
    Nrd2Pose q;
    const bool read = readNrd2Pose(path, q, &why);
    check(read, "reads back " + why);
    check(q.scene == p.scene && q.poseIndex == 17 && q.heldOut && q.stageBVersion == 1 && q.halfW == 20 &&
              q.tilesY == 3 && q.frames == 4 && q.features == p.features && q.theta == p.theta &&
              q.weights == p.weights && q.losses == p.losses,
          "every field round-trips");
    std::vector<char> bytes;
    {
        std::ifstream f(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    auto rewrite = [&](const std::vector<char>& b) {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(b.data(), static_cast<std::streamsize>(b.size()));
    };
    std::vector<char> bad = bytes;
    bad[bad.size() / 2] ^= 0x10;
    rewrite(bad);
    bool r = readNrd2Pose(path, q, &why);
    check(!r && why == "bad CRC", "a flipped bit fails the CRC");
    bad = bytes;
    bad.resize(bad.size() - 9);
    rewrite(bad);
    r = readNrd2Pose(path, q, &why);
    check(!r, "a truncated file is rejected (" + why + ")");
    bad = bytes;
    bad.push_back(0);
    rewrite(bad);
    r = readNrd2Pose(path, q, &why);
    check(!r, "trailing bytes are rejected (" + why + ")");
    Nrd2Pose wrong = p;
    wrong.halfW = 21;
    r = writeNrd2Pose(path, wrong, &why);
    check(!r, "a half width that is not 4 x tiles is refused (" + why + ")");
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

}  // namespace

int main() {
    AVER_INFO("Nrd2ResolveTest: NRD2 CPU twin + pose file");
    testHalf();
    testPyramidConstant();
    testEnergy();
    testFiniteDifferences();
    testTemporal();
    testCandidates();
    testOracle(Nrd2OracleMode::Grad);
    testOracle(Nrd2OracleMode::Grid);
    testDataset();
    if (g_failures) { AVER_ERROR("Nrd2ResolveTest: {} failure(s)", g_failures); return 1; }
    AVER_INFO("Nrd2ResolveTest: all passed");
    return 0;
}
