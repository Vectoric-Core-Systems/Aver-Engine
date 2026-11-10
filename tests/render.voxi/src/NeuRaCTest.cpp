// Tests NeuRaCLayout.hpp (see docs/rendering/NEURAC.md).
#include "aver/core/Log.hpp"
#include "aver/voxi/NeuRaCLayout.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace aver;
namespace rc = aver::voxi::neurac;

namespace {

int g_failures = 0;
int g_checks   = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        // Unreadable file must FAIL assertions, not skip (empty string would pass vacuously).
        AVER_ERROR("[NeuRaC] could not read {} -- every source assertion against it is a failure, "
                   "not a skip.", path);
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Value of `#define NAME value` in `text`, comments/parentheses/suffixes stripped; NaN when absent.
f64 defineValue(const std::string& text, const std::string& name) {
    size_t at = 0;
    while ((at = text.find("#define", at)) != std::string::npos) {
        size_t p = at + 7;
        while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
        if (text.compare(p, name.size(), name) == 0) {
            const size_t after = p + name.size();
            if (after < text.size() && (text[after] == ' ' || text[after] == '\t')) {
                size_t e = text.find('\n', after);
                std::string v = text.substr(after, e == std::string::npos ? std::string::npos : e - after);
                const size_t c = v.find("//");
                if (c != std::string::npos) v.resize(c);
                std::string clean;
                for (char ch : v) if (ch != '(' && ch != ')' && ch != '\r' && ch != ' ' && ch != '\t') clean += ch;
                while (!clean.empty() && (clean.back() == 'f' || clean.back() == 'F' || clean.back() == 'u' ||
                                          clean.back() == 'U'))
                    clean.pop_back();
                char* end = nullptr;
                const f64 d = std::strtod(clean.c_str(), &end);
                if (end != clean.c_str() && *end == '\0') return d;
            }
        }
        at = p;
    }
    return std::numeric_limits<f64>::quiet_NaN();
}

bool approx(f64 a, f64 b, f64 tol) { return std::fabs(a - b) <= tol; }

}  // namespace

int main() {
    // ---- 1. the numbers the contract states -------------------------------------------------------
    AVER_INFO("=== layout constants ===");
    {
        check(rc::kCells == 786432u && rc::kCellsPerCascade == 262144u, "3 x 64^3 = 786,432 cells");
        check(rc::kAccumInts == 14155776u && rc::kAccumBytes == 56623104ull, "accumulator is 14,155,776 ints = 56,623,104 B");
        check(rc::kCellsBytes == 25165824ull, "cells are 786,432 x 32 B = 25,165,824 B");
        check(sizeof(rc::RcInfo) == 80 && sizeof(rc::RcCell) == 32, "RcInfo is 80 B and RcCell is 32 B");
        check(rc::kResolveGroups == 12288u && rc::kResolveGroups * rc::kResolveGroupSize == rc::kCells,
              "12,288 groups of 64 threads cover every cell exactly once");
        check(rc::kCellSizeCm[0] == 25.0f && rc::kCellSizeCm[1] == 100.0f && rc::kCellSizeCm[2] == 400.0f,
              "cell sizes are 25 / 100 / 400 cm");
        const f64 total = static_cast<f64>(rc::kAccumBytes + rc::kCellsBytes) / (1024.0 * 1024.0);
        check(total > 77.5 && total < 78.5, "the VRAM bill is ~78 MiB (81.8 MB): 3.1 MiB over the 75 MiB before the sum-of-squares block");
    }

    // ---- 2. fixed-point headroom ------------------------------------------------------------------
    AVER_INFO("=== fixed-point headroom ===");
    {
        // Worst sample summed in int64 (test cannot wrap) must stay under half of int32 max.
        const f64 worst = rc::kLMax * 0.488603 * rc::kPi / rc::kMinCos;
        std::int64_t sum = 0;
        for (u32 i = 0; i < rc::kMaxCap; ++i) sum += static_cast<std::int64_t>(std::llround(worst * rc::kShScale));
        check(sum < 2147483647ll / 2, "64 worst-case SH samples sum to " + std::to_string(sum) + ", under half of int32 max");
        std::int64_t nsum = 0;
        for (u32 i = 0; i < rc::kMaxCap; ++i) nsum += static_cast<std::int64_t>(std::llround(1.0 * rc::kNScale));
        check(nsum < 2147483647ll / 2, "64 unit normals sum to " + std::to_string(nsum) + ", far under int32 max");
        check(rc::kWorstSample > 480.0 && rc::kWorstSample < 500.0, "the documented worst sample is ~491");
        check(rc::kShScale == 32768.0 && rc::kNScale == 65536.0 && rc::kSqScale == 2048.0, "scales are 2^15, 2^16 and 2^11");
        // The label's per-sample scalar luminance(L) * PI / max(cos, 0.1) / (4 PI), worst case 64 samples, int64 so the test cannot wrap.
        check(approx(rc::kWorstScalar, 80.0, 1e-9), "the worst per-sample scalar is 32 * 10 / 4 = 80");
        std::int64_t sqSum = 0;
        for (u32 i = 0; i < rc::kMaxCap; ++i)
            sqSum += static_cast<std::int64_t>(std::llround(rc::kWorstScalar * rc::kWorstScalar * rc::kSqScale));
        check(sqSum < 2147483647ll / 2 && approx(static_cast<f64>(sqSum), rc::kWorstSqSum, 1.0),
              "64 worst-case squared scalars sum to " + std::to_string(sqSum) + ", under half of int32 max");
    }

    // ---- 3. tags and cell indices ------------------------------------------------------------------
    AVER_INFO("=== tag and cell addressing ===");
    {
        check(rc::tagPack(0, 0, 0) == 0u, "world cell (0,0,0) has tag 0");
        check(rc::tagPack(63, 63, 63) == 0u, "the first 64-cell block shares one tag");
        check(rc::tagPack(64, 0, 0) == 1u && rc::tagPack(0, 64, 0) == (1u << 8) && rc::tagPack(0, 0, 64) == (1u << 16),
              "one block along an axis moves that axis' byte by one");
        // Negative cells: floor(cell/64) fits in one byte via arithmetic shift, distinct from block 0.
        check(rc::tagPack(-1, 0, 0) == 0xFFu && rc::tagPack(-64, 0, 0) == 0xFFu && rc::tagPack(-65, 0, 0) == 0xFEu,
              "negative world cells tag by floor(cell/64): -1..-64 -> 0xFF, -65 -> 0xFE");
        check(rc::tagPack(-1, 0, 0) != rc::tagPack(0, 0, 0), "cell -1 and cell 0 do not alias");
        check(rc::tagPack(64 * 256, 0, 0) == rc::tagPack(0, 0, 0), "tags alias every 256 blocks (the documented 24-bit limit)");

        check(rc::cellIndex(0, 0, 0, 0) == 0u && rc::cellIndex(1, 0, 0, 0) == rc::kCellsPerCascade,
              "cascade-major indexing");
        check(rc::cellIndex(0, 1, 0, 0) == 1u && rc::cellIndex(0, 0, 1, 0) == 64u && rc::cellIndex(0, 0, 0, 1) == 4096u,
              "x fastest, then y, then z");
        check(rc::cellIndex(0, -1, 0, 0) == 63u && rc::cellIndex(2, -1, -1, -1) == rc::kCells - 1,
              "negative cells wrap into the window with &63");

        // 64^3 window anchored at negative origin: each cell maps to distinct texel with correct world recovery.
        const i32 ox = -37, oy = -90, oz = 17;
        std::vector<std::uint8_t> seen(rc::kCellsPerCascade, 0);
        u32 collisions = 0;
        bool tagsOk = true;
        for (i32 z = 0; z < 64; ++z) for (i32 y = 0; y < 64; ++y) for (i32 x = 0; x < 64; ++x) {
            const u32 idx = rc::cellIndex(1, ox + x, oy + y, oz + z) - rc::kCellsPerCascade;
            if (seen[idx]++) ++collisions;
            // world = origin + ((texel - origin) & 63) recovers the world cell.
            const i32 tx = idx & 63, ty = (idx >> 6) & 63, tz = idx >> 12;
            const i32 wx = ox + ((tx - ox) & 63), wy = oy + ((ty - oy) & 63), wz = oz + ((tz - oz) & 63);
            if (wx != ox + x || wy != oy + y || wz != oz + z) tagsOk = false;
        }
        check(collisions == 0, "a 64^3 window at a negative origin covers 262,144 distinct texels");
        check(tagsOk, "origin + ((texel - origin) & 63) recovers the world cell for every texel");
    }

    // ---- 4. origin snapping -----------------------------------------------------------------------
    AVER_INFO("=== origin snapping ===");
    {
        check(rc::snapOrigin(0.0f, 25.0f) == -32, "camera at 0 -> origin -32");
        check(rc::snapOrigin(24.9f, 25.0f) == -32 && rc::snapOrigin(25.0f, 25.0f) == -31, "origin steps at cell boundaries");
        check(rc::snapOrigin(-0.1f, 25.0f) == -33, "a hair west of 0 floors to cell -1 -> origin -33");
        check(rc::snapOrigin(5000.0f, 25.0f) == 200 - 32, "5000 cm / 25 = cell 200 -> origin 168");
        check(rc::snapOrigin(399.0f, 400.0f) == -32 && rc::snapOrigin(400.0f, 400.0f) == -31, "coarse cascade snaps in 4 m steps");
        bool centred = true;
        for (f32 cam = -20000.0f; cam < 20000.0f; cam += 137.0f) {
            for (u32 c = 0; c < rc::kCascades; ++c) {
                const i32 o = rc::snapOrigin(cam, rc::kCellSizeCm[c]);
                const i32 camCell = static_cast<i32>(std::floor(cam / rc::kCellSizeCm[c]));
                if (camCell - o != rc::kHalfRes) centred = false;   // exactly 32 cells of window on the min side
            }
        }
        check(centred, "the camera's cell is always exactly 32 cells in from the window's min corner");
    }

    // ---- 5. meta and normal words -----------------------------------------------------------------
    AVER_INFO("=== meta word and normal word ===");
    {
        const u32 tag = rc::tagPack(-3, 130, 700);
        const u32 m = rc::metaPack(tag, 9, 4);
        check(rc::metaTag(m) == tag && rc::metaNEff(m) == 9 && rc::metaAge(m) == 4, "meta round-trips tag, n_eff, age");
        // Oversized field values mask to their width without spilling into neighbours.
        const u32 big = rc::metaPack(0xFFFFFFFFu, 99, 99);
        check(rc::metaTag(big) == 0xFFFFFFu && rc::metaNEff(big) == 3u && rc::metaAge(big) == 3u &&
                  big == 0x33FFFFFFu,
              "oversized fields mask to their widths, not into neighbours");
        check(rc::cellValid(m, tag), "occupied, matching tag, young -> valid");
        check(!rc::cellValid(m, tag ^ 1u), "a stale tag is invalid");
        check(!rc::cellValid(rc::metaPack(tag, 0, 0), tag), "n_eff 0 is empty, hence invalid");
        check(!rc::cellValid(rc::metaPack(tag, 5, rc::kAgeMax), tag), "age at the limit is invalid");
        check(rc::cellValid(rc::metaPack(tag, 5, rc::kAgeMax - 1), tag), "age one below the limit is valid");

        std::mt19937 rng(1234);
        std::uniform_real_distribution<f32> d(-1.0f, 1.0f);
        f32 worstCos = 1.0f, worstLen = 0.0f;
        f64 sumDeg = 0.0;
        int nDir = 0;
        for (int i = 0; i < 20000; ++i) {
            f32 v[3] = {d(rng), d(rng), d(rng)};
            const f32 l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (l < 0.05f) continue;
            const f32 len = std::min(1.0f, l);   // lengths are mean-normal lengths, <= 1
            for (f32& c : v) c = c / l * len;
            f32 dir[3], outLen;
            rc::unpackNormal(rc::packNormal(v), dir, outLen);
            const f32 cosA = std::min(1.0f, (dir[0] * v[0] + dir[1] * v[1] + dir[2] * v[2]) / len);
            worstCos = std::min(worstCos, cosA);
            sumDeg += std::acos(cosA) * 180.0 / rc::kPi;
            ++nDir;
            worstLen = std::max(worstLen, std::fabs(outLen - len));
        }
        // 10 bits per octahedral axis: measured worst ~0.24 degrees, mean ~0.08.
        check(worstCos > 0.999986f, "10-bit octahedral directions round-trip within 0.3 degrees (worst cos " + std::to_string(worstCos) + ")");
        check(sumDeg / nDir < 0.1, "the mean direction error is under 0.1 degrees (" + std::to_string(sumDeg / nDir) + ")");
        check(worstLen <= 0.5f / 63.0f + 1e-4f, "the 6-bit length field is within half a 1/63 step (worst error " + std::to_string(worstLen) + ")");
        // Axis-aligned normals survive the pack exactly.
        f32 up[3] = {0.0f, 1.0f, 0.0f}, dir[3], len;
        rc::unpackNormal(rc::packNormal(up), dir, len);
        check(approx(dir[1], 1.0, 1e-3) && approx(len, 1.0, 1e-3), "+Y at full length survives the pack");
        f32 dn[3] = {0.0f, 0.0f, -1.0f};
        rc::unpackNormal(rc::packNormal(dn), dir, len);
        check(approx(dir[2], -1.0, 1e-3), "-Z (the folded octant) survives the pack");
        f32 zero[3] = {0.0f, 0.0f, 0.0f};
        rc::unpackNormal(rc::packNormal(zero), dir, len);
        check(len == 0.0f && std::isfinite(dir[0]) && std::isfinite(dir[1]) && std::isfinite(dir[2]),
              "a zero vector packs to length 0 with a finite direction (no NaN in a cell)");

        // Bit layout: octU 10 | octV 10 << 10 | len 6 << 20 | trust 6 << 26. +Z maps to oct (0,0) = code 512.
        f32 pz[3] = {0.0f, 0.0f, 1.0f};
        check(rc::packNormal(pz, 63u) == (512u | (512u << 10) | (63u << 20) | (63u << 26)),
              "the normal word is octU 10 | octV 10 | len 6 | trust 6 (+Z, full length, trust 63)");
        check(rc::packNormal(pz) == rc::packNormal(pz, 63u), "trust defaults to 63 (1.0): a new cell is fully trusted");

        // Trust: every 6-bit value round-trips, without disturbing the direction or the length.
        bool trustOk = true, sideOk = true;
        f32 half[3] = {0.0f, 0.6f, 0.0f};
        f32 refDir[3], refLen;
        rc::unpackNormal(rc::packNormal(half, 63u), refDir, refLen);
        for (u32 t = 0; t <= rc::kTrustMax; ++t) {
            f32 dd[3], ll, tr;
            const u32 w = rc::packNormal(half, t);
            rc::unpackNormal(w, dd, ll, tr);
            if (std::fabs(tr - static_cast<f32>(t) / 63.0f) > 1e-6f || rc::normalTrust6(w) != t) trustOk = false;
            if (dd[0] != refDir[0] || dd[1] != refDir[1] || dd[2] != refDir[2] || ll != refLen) sideOk = false;
        }
        check(trustOk, "all 64 trust values round-trip as t / 63 (63 is exactly 1.0, 0 is exactly 0)");
        check(sideOk, "changing the trust leaves the direction and the length bit-identical");
        f32 dd[3], ll, tr;
        rc::unpackNormal(rc::packNormal(half, 200u), dd, ll, tr);
        check(tr == 1.0f, "a trust above 63 clamps to 1.0 instead of spilling into the word's top bits");
        // The length field has 63 steps of 1/63: 0.5 lands on a step edge, 1.0 on the last code.
        f32 lenCases[3][3] = {{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.5f}, {0.0f, 0.0f, 1.0f / 63.0f}};
        const u32 wantLen[3] = {63u, 32u, 1u};
        bool lenOk = true;
        for (int i = 0; i < 3; ++i) lenOk = lenOk && ((rc::packNormal(lenCases[i]) >> 20) & 63u) == wantLen[i];
        check(lenOk, "the length codes are round(len * 63): 1 -> 63, 0.5 -> 32, 1/63 -> 1");
    }

    // ---- 6. fp16 SH packing -----------------------------------------------------------------------
    AVER_INFO("=== fp16 SH packing ===");
    {
        check(rc::halfToFloat(rc::floatToHalf(1.0f)) == 1.0f && rc::floatToHalf(1.0f) == 0x3C00u, "1.0 is 0x3C00");
        check(rc::floatToHalf(-2.0f) == 0xC000u, "-2.0 is 0xC000");
        check(rc::floatToHalf(0.0f) == 0u, "0 is 0");
        check(rc::floatToHalf(1.0e9f) == 0x7C00u, "overflow saturates to +inf");
        check(rc::halfToFloat(0x0001u) > 5.9e-8f && rc::halfToFloat(0x0001u) < 6.0e-8f, "the smallest subnormal decodes to 2^-24");
        check(rc::halfToFloat(rc::floatToHalf(6.0e-8f)) > 0.0f, "a subnormal-range float is not flushed to zero");
        std::mt19937 rng(77);
        std::uniform_real_distribution<f32> d(-50.0f, 50.0f);
        f32 worstRel = 0.0f;
        for (int i = 0; i < 20000; ++i) {
            f32 sh[rc::kShFloats], back[rc::kShFloats];
            u32 w[6];
            for (f32& v : sh) v = d(rng);
            rc::packSh(sh, w);
            rc::unpackSh(w, back);
            for (u32 k = 0; k < rc::kShFloats; ++k) {
                const f32 rel = std::fabs(back[k] - sh[k]) / std::max(std::fabs(sh[k]), 1e-3f);
                worstRel = std::max(worstRel, rel);
            }
        }
        check(worstRel < 1.0f / 1024.0f, "12 SH floats round-trip within fp16's 2^-11 relative step (worst " + std::to_string(worstRel) + ")");
        // k = 0 in low half of word 0, k = 1 in high half.
        f32 sh[rc::kShFloats] = {1.0f, 2.0f};
        u32 w[6];
        rc::packSh(sh, w);
        check((w[0] & 0xFFFFu) == 0x3C00u && (w[0] >> 16) == 0x4000u && w[1] == 0u, "low half = even k, high half = odd k");
    }

    // ---- 7. SH convolution and the scatter's estimator --------------------------------------------
    AVER_INFO("=== SH irradiance and the scatter estimator ===");
    {
        // Constant L over hemisphere: c0 = Y0 * 2π L, cN = 0.488603 * π L; cosine convolution returns L.
        const f32 L = 3.0f;
        const f32 n[3] = {0.0f, 1.0f, 0.0f};
        f32 c[4][3] = {};
        for (int ch = 0; ch < 3; ++ch) {
            c[0][ch] = static_cast<f32>(0.282095 * 2.0 * rc::kPi * L);
            c[1][ch] = static_cast<f32>(0.488603 * rc::kPi * L);   // cY: N is +Y
        }
        f32 e[3];
        rc::shIrradianceOverPi(c, n, e);
        check(approx(e[0], L, 1e-3) && approx(e[1], L, 1e-3), "analytic constant radiance convolves back to L exactly");
        const f32 nDown[3] = {0.0f, -1.0f, 0.0f};
        rc::shIrradianceOverPi(c, nDown, e);
        check(e[0] >= 0.0f && e[0] < 0.2f * L, "the opposite normal sees almost nothing and is clamped >= 0");
        f32 neg[4][3] = {};
        neg[0][0] = -1.0f;
        rc::shIrradianceOverPi(neg, n, e);
        check(e[0] == 0.0f, "negative irradiance clamps to zero");

        // Monte Carlo: cosine-sampled directions (Malley), L clamped, weight π / max(cos, MIN_COS), fixed-point sum.
        const f32 normals[3][3] = {{0.0f, 1.0f, 0.0f}, {0.6f, 0.0f, 0.8f}, {-0.267261f, 0.534522f, -0.801784f}};
        for (const auto& N : normals) {
            // Tangent frame.
            f32 t[3], b[3];
            const f32 ax[3] = {std::fabs(N[0]) < 0.9f ? 1.0f : 0.0f, std::fabs(N[0]) < 0.9f ? 0.0f : 1.0f, 0.0f};
            t[0] = N[1] * ax[2] - N[2] * ax[1]; t[1] = N[2] * ax[0] - N[0] * ax[2]; t[2] = N[0] * ax[1] - N[1] * ax[0];
            const f32 tl = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
            for (f32& v : t) v /= tl;
            b[0] = N[1] * t[2] - N[2] * t[1]; b[1] = N[2] * t[0] - N[0] * t[2]; b[2] = N[0] * t[1] - N[1] * t[0];

            const int side = 128;
            std::int64_t acc[4][3] = {};
            std::int64_t nacc[3] = {};
            const f32 Lsample = 1.0f;
            for (int i = 0; i < side * side; ++i) {
                const f64 u = (i / side + 0.5) / side, v = (i % side + 0.5) / side;
                const f64 r = std::sqrt(u), phi = 2.0 * rc::kPi * v;
                const f64 z = std::sqrt(1.0 - u);
                f32 dir[3];
                for (int a = 0; a < 3; ++a)
                    dir[a] = static_cast<f32>(r * std::cos(phi) * t[a] + r * std::sin(phi) * b[a] + z * N[a]);
                f32 y[4];
                rc::shBasis(dir, y);
                const f64 w = rc::kPi / std::max(z, rc::kMinCos);
                for (int j = 0; j < 4; ++j)
                    for (int ch = 0; ch < 3; ++ch)
                        acc[j][ch] += std::llround(std::min<f64>(Lsample, rc::kLMax) * y[j] * w * rc::kShScale);
                for (int a = 0; a < 3; ++a) nacc[a] += std::llround(N[a] * rc::kNScale);
            }
            const f64 nSamples = static_cast<f64>(side) * side;
            f32 mean[4][3];
            for (int j = 0; j < 4; ++j)
                for (int ch = 0; ch < 3; ++ch) mean[j][ch] = static_cast<f32>(static_cast<f64>(acc[j][ch]) / (nSamples * rc::kShScale));
            f32 out[3];
            rc::shIrradianceOverPi(mean, N, out);
            // cos floor under-weights <0.1 angles: few-percent bias, never high.
            check(out[0] > 0.94f * Lsample && out[0] < 1.01f * Lsample,
                  "Monte Carlo of constant L reproduces L within the floor's few-percent bias (got " + std::to_string(out[0]) + ")");
            const f64 nx = static_cast<f64>(nacc[0]) / (nSamples * rc::kNScale);
            const f64 ny = static_cast<f64>(nacc[1]) / (nSamples * rc::kNScale);
            const f64 nz = static_cast<f64>(nacc[2]) / (nSamples * rc::kNScale);
            check(approx(nx, N[0], 1e-3) && approx(ny, N[1], 1e-3) && approx(nz, N[2], 1e-3),
                  "the summed-normal fixed point recovers the normal");
        }
    }

    // ---- 7b. layout arithmetic -----------------------------------------------------------------------
    AVER_INFO("=== accumulator layout and RcInfo ===");
    {
        check(rc::kAccumInts == 18u * rc::kCells, "kAccumInts == 18 * kCells (counts, arrivals, 15 payload ints, 1 sum of squares per cell)");
        check(rc::kArrivalsBase == rc::kCells && rc::kPayloadBase == 2u * rc::kCells,
              "arrivals start at kCells and the payload at 2 * kCells (1,572,864)");
        check(rc::kPayloadBase + rc::kCells * rc::kPayloadInts == rc::kSqBase && rc::kSqBase == 17u * rc::kCells &&
                  rc::kSqBase + rc::kCells == rc::kAccumInts,
              "the payload region ends where the sum-of-squares block starts, and that block ends the accumulator");
        const u64 lastPayload = static_cast<u64>(rc::kPayloadBase) + static_cast<u64>(rc::kCells - 1) * rc::kPayloadInts + 14u;
        check(lastPayload == rc::kSqBase - 1u, "the last cell's last payload int is the last int before the sum-of-squares block");
        check(static_cast<u64>(rc::kSqBase) + (rc::kCells - 1) == rc::kAccumInts - 1u, "the last cell's sum of squares is the last accumulator int");
        // Headroom: the worst per-cell sums keep their 2x margin under int32, and one cell's cap fits the
        // 8-bit coarse fields and the 16-bit accepted counter.
        check(rc::kWorstShSum * 2.0 < 2147483647.0 && rc::kWorstNormalSum * 2.0 < 2147483647.0 && rc::kWorstSqSum * 2.0 < 2147483647.0,
              "the fixed-point sums keep their 2x margin under int32 max");
        check(rc::kMaxCap < 256u, "caps fit the 8-bit coarse fields and the 16-bit accepted counter");
        check(sizeof(rc::RcInfo) == 80 && rc::kInfoStride == 80, "RcInfo is still 80 B");
        check(offsetof(rc::RcInfo, alphaMin) == 16 && offsetof(rc::RcInfo, relaxAlpha) == 20 &&
                  offsetof(rc::RcInfo, coarseCaps) == 24 && offsetof(rc::RcInfo, netStrength) == 28 &&
                  offsetof(rc::RcInfo, cas) == 32,
              "hdr1 = (alphaMin, relaxAlpha, coarseCaps, netStrength) at bytes 16..31 and the cascades from byte 32");
        check(rc::kDefaultCoarseCap1 == 16u && rc::kDefaultCoarseCap2 == 8u && rc::kMaxCap == 64u,
              "the per-cascade caps default to 64 / 16 / 8");
    }

    // ---- 7c. scatter binning, the wall case ----------------------------------------------------------
    AVER_INFO("=== scatter binning (scatterCell) ===");
    {
        const f32 camCm[3] = {1234.5f, 87.0f, -400.0f};
        // Window origins for every cascade, as NeuRaC::beginFrame snaps them.
        i32 org[rc::kCascades][3];
        for (u32 c = 0; c < rc::kCascades; ++c)
            for (int a = 0; a < 3; ++a) org[c][a] = rc::snapOrigin(camCm[a], rc::kCellSizeCm[c]);

        // Interior test: relative cells 1 .. 62 train, 0 and 63 do not, per axis and per cascade.
        bool interiorOk = true;
        for (u32 c = 0; c < rc::kCascades; ++c) {
            const f32 size = rc::kCellSizeCm[c];
            for (int axis = 0; axis < 3; ++axis)
                for (i32 rel : {-1, 0, 1, 2, 31, 61, 62, 63, 64}) {
                    f32 hit[3];
                    const f32 n[3] = {0.0f, 0.0f, 0.0f};
                    for (int a = 0; a < 3; ++a) hit[a] = (static_cast<f32>(org[c][a] + 32) + 0.5f) * size;
                    hit[axis] = (static_cast<f32>(org[c][axis] + rel) + 0.5f) * size;
                    i32 cell[3];
                    const bool in = rc::scatterCell(hit, n, c, org[c], size, cell);
                    const bool want = rel >= 1 && rel <= 62;
                    if (in != want || cell[axis] != org[c][axis] + rel) interiorOk = false;
                }
        }
        check(interiorOk, "scatterCell trains relative cells 1..62 only, at every cascade and axis");

        // The normal offset: a point 0.1 cells into a cell stays in it facing +x and falls to the previous
        // cell facing -x; 0.6 cells in, the +x face moves on to the next cell.
        {
            const f32 size = rc::kCellSizeCm[0];
            const f32 base = static_cast<f32>(org[0][0] + 10);
            const f32 hit[3] = {(base + 0.1f) * size, (static_cast<f32>(org[0][1] + 20) + 0.5f) * size,
                                (static_cast<f32>(org[0][2] + 20) + 0.5f) * size};
            const f32 pos[3] = {1.0f, 0.0f, 0.0f}, neg[3] = {-1.0f, 0.0f, 0.0f};
            i32 cp[3], cn[3];
            rc::scatterCell(hit, pos, 0, org[0], size, cp);
            rc::scatterCell(hit, neg, 0, org[0], size, cn);
            check(cp[0] == org[0][0] + 10 && cn[0] == org[0][0] + 9,
                  "0.1 cells into cell 10: the +x face trains cell 10 and the -x face cell 9 (offset half a cell along N)");
            const f32 hit2[3] = {(base + 0.6f) * size, hit[1], hit[2]};
            rc::scatterCell(hit2, pos, 0, org[0], size, cp);
            rc::scatterCell(hit2, neg, 0, org[0], size, cn);
            check(cp[0] == org[0][0] + 11 && cn[0] == org[0][0] + 10,
                  "0.6 cells in, the +x face trains cell 11 and the -x face cell 10");
        }

        // The wall case. N along +-x, a surface at every fractional position u. The lookup shifts by
        // N * size / 2 and interpolates trilinearly between cell centres; the trained cell must carry at least
        // half of the 1D weight along the normal, whichever sign the normal has.
        for (u32 c = 0; c < rc::kCascades; ++c) {
            const f32 size = rc::kCellSizeCm[c];
            for (int sign = -1; sign <= 1; sign += 2) {
                f32 worst = 1.0f, worstOld = 1.0f;
                bool thinWallOk = true, inside = true;
                const int steps = 4 * 64;
                for (int k = 0; k < steps; ++k) {
                    const f32 u = (static_cast<f32>(k) + 0.37f) / static_cast<f32>(steps);   // fraction within a cell
                    const f32 cellX = static_cast<f32>(org[c][0] + 10 + (k % 20));
                    const f32 hit[3] = {(cellX + u) * size, (static_cast<f32>(org[c][1] + 32) + 0.31f) * size,
                                        (static_cast<f32>(org[c][2] + 32) + 0.77f) * size};
                    const f32 n[3] = {static_cast<f32>(sign), 0.0f, 0.0f};
                    i32 cell[3];
                    if (!rc::scatterCell(hit, n, c, org[c], size, cell)) { inside = false; break; }
                    // The lookup's window-space sample point along x, its base corner and fraction.
                    const f32 q = (hit[0] + n[0] * 0.5f * size) / size - static_cast<f32>(org[c][0]);
                    const f32 g = q - 0.5f, fl = std::floor(g), f = g - fl;
                    const i32 base = static_cast<i32>(fl);
                    const auto weightOf = [&](i32 rel) { return rel == base ? 1.0f - f : (rel == base + 1 ? f : 0.0f); };
                    worst = std::min(worst, weightOf(cell[0] - org[c][0]));
                    // The old binning, floor(p / size), for contrast: it can fall to nearly nothing.
                    worstOld = std::min(worstOld, weightOf(static_cast<i32>(std::floor(hit[0] / size)) - org[c][0]));
                    // The two faces of a thin wall land in different cells.
                    const f32 nOpp[3] = {-n[0], 0.0f, 0.0f};
                    i32 other[3];
                    rc::scatterCell(hit, nOpp, c, org[c], size, other);
                    if (other[0] == cell[0]) thinWallOk = false;
                }
                const std::string tag = "cascade " + std::to_string(c) + ", normal " + (sign > 0 ? "+x" : "-x") + ": ";
                check(inside, tag + "the test surfaces sit inside the window interior");
                check(worst >= 0.5f - 1e-5f, tag + "the trained cell carries at least 0.5 of the 1D lookup weight (min " +
                                                  std::to_string(worst) + ")");
                check(worstOld < 0.1f, tag + "binning at floor(p / size) would drop to " + std::to_string(worstOld) +
                                           " (the red walls), so the test tells the two apart");
                check(thinWallOk, tag + "the two faces of a thin wall train different cells");
            }
        }
    }

    // ---- 7d. accept rate and caps ---------------------------------------------------------------------
    AVER_INFO("=== accept rate and per-cascade caps ===");
    {
        check(rc::acceptRate(0, 64) == 1.0f && rc::acceptRate(1, 64) == 1.0f && rc::acceptRate(64, 64) == 1.0f,
              "up to cap arrivals last frame, every sample is accepted");
        check(approx(rc::acceptRate(65, 64), 64.0 / 65.0, 1e-6) && approx(rc::acceptRate(128, 64), 0.5, 1e-6) &&
                  approx(rc::acceptRate(640, 64), 0.1, 1e-6),
              "beyond the cap the rate is cap / prev (65 -> 64/65, 128 -> 1/2, 640 -> 1/10)");
        check(approx(rc::acceptRate(2048, 64), 1.0 / 32.0, 1e-9) && approx(rc::acceptRate(65535, 64), 1.0 / 32.0, 1e-9),
              "the rate floors at 1/32 (cap 64, from 2048 arrivals up)");
        check(approx(rc::acceptRate(17, 16), 16.0 / 17.0, 1e-6) && approx(rc::acceptRate(512, 16), 1.0 / 32.0, 1e-9) &&
                  approx(rc::acceptRate(9, 8), 8.0 / 9.0, 1e-6) && approx(rc::acceptRate(256, 8), 1.0 / 32.0, 1e-9),
              "the cap-16 and cap-8 tables follow the same law");
        bool monotone = true, boundOk = true;
        for (u32 cap : {8u, 16u, 64u}) {
            f32 last = 1.0f;
            for (u32 prev = 0; prev <= 70000; prev += 7) {
                const f32 r = rc::acceptRate(prev, cap);
                if (r > last || r <= 0.0f || r > 1.0f) monotone = false;
                last = r;
                // Expected accepted samples sit at the cap while the floor allows it, and never above the load.
                const f64 expect = static_cast<f64>(prev) * r;
                if (prev >= cap && prev <= 32u * cap && std::fabs(expect - cap) > 1e-3 * cap) boundOk = false;
                if (expect > static_cast<f64>(prev) + 1e-6) boundOk = false;
            }
        }
        check(monotone, "the rate never rises with the load and stays in (0, 1]");
        check(boundOk, "expected accepted samples equal the cap for loads in [cap, 32 cap] (a random subset, so unbiased)");

        rc::RcInfo info{};
        info.sampleCap = 64;
        info.coarseCaps = rc::kDefaultCoarseCap1 | (rc::kDefaultCoarseCap2 << 8);
        check(rc::capForCascade(info, 0) == 64u && rc::capForCascade(info, 1) == 16u && rc::capForCascade(info, 2) == 8u,
              "caps are 64 / 16 / 8 per cascade (finest from sampleCap, coarse two from coarseCaps)");
        info.sampleCap = 1000;
        info.coarseCaps = 200u | (255u << 8);
        check(rc::capForCascade(info, 0) == rc::kMaxCap && rc::capForCascade(info, 1) == rc::kMaxCap &&
                  rc::capForCascade(info, 2) == rc::kMaxCap,
              "every cap clamps to kMaxCap");
        info.sampleCap = 5;
        info.coarseCaps = 0u;
        check(rc::capForCascade(info, 0) == 5u && rc::capForCascade(info, 1) == 0u && rc::capForCascade(info, 2) == 0u,
              "a zero coarse cap switches that cascade's training off");
    }

    // ---- 7e. lookup corner weight ---------------------------------------------------------------------
    AVER_INFO("=== lookup corner weight ===");
    {
        // The stage-1 weight written out independently: tri * saturate(dot * K) * len * min(n_eff / 4, 1) * (1 - age / 15).
        const auto stage1 = [](f32 tri, f32 dotNN, f32 len, u32 nEff, u32 age) {
            const f32 a = std::min(1.0f, std::max(0.0f, dotNN * 2.0f));
            return tri * a * len * std::min(static_cast<f32>(nEff) * 0.25f, 1.0f) * (1.0f - static_cast<f32>(age) / 15.0f);
        };
        bool sameAsStage1 = true;
        for (f32 tri : {0.0f, 0.125f, 0.5f, 1.0f})
            for (f32 dot : {-1.0f, 0.0f, 0.2f, 0.5f, 0.9f, 1.0f})
                for (f32 len : {0.0f, 0.4f, 1.0f})
                    for (u32 nEff : {1u, 2u, 3u, 4u, 15u})
                        for (u32 age : {0u, 1u, 7u, 14u})
                            for (f32 trust : {0.0f, 0.3f, 1.0f})
                                if (rc::lookupCornerWeight(tri, dot, len, nEff, age, trust, 0.0f) !=
                                    stage1(tri, dot, len, nEff, age))
                                    sameAsStage1 = false;
        check(sameAsStage1, "with netStrength 0 the weight is the stage-1 formula exactly, whatever the trust");

        const f32 base = rc::lookupCornerWeight(0.5f, 0.8f, 0.9f, 8, 3, 1.0f, 1.0f);
        check(base == stage1(0.5f, 0.8f, 0.9f, 8, 3), "trust 1.0 changes nothing at full strength");
        check(approx(rc::lookupCornerWeight(0.5f, 0.8f, 0.9f, 8, 3, 0.5f, 1.0f), 0.5 * base, 1e-6),
              "trust 0.5 at strength 1 halves the weight");
        check(rc::lookupCornerWeight(0.5f, 0.8f, 0.9f, 8, 3, 0.0f, 1.0f) == 0.0f, "trust 0 at strength 1 removes the corner");
        check(approx(rc::lookupCornerWeight(0.5f, 0.8f, 0.9f, 8, 3, 0.0f, 0.25f), 0.75 * base, 1e-6),
              "trust 0 at strength 0.25 removes a quarter: lerp(1, trust, strength)");
        bool monoTrust = true;
        f32 last = -1.0f;
        for (u32 t = 0; t <= rc::kTrustMax; ++t) {
            const f32 w = rc::lookupCornerWeight(0.5f, 0.8f, 0.9f, 8, 3, static_cast<f32>(t) / 63.0f, 0.8f);
            if (w < last) monoTrust = false;
            last = w;
        }
        check(monoTrust, "the weight rises with the trust");
        check(rc::lookupCornerWeight(1.0f, -0.5f, 1.0f, 8, 0, 1.0f, 0.0f) == 0.0f, "a cell facing away (dot < 0) gets no weight");
        check(rc::lookupCornerWeight(1.0f, 0.5f, 1.0f, 8, 0, 1.0f, 0.0f) == rc::lookupCornerWeight(1.0f, 1.0f, 1.0f, 8, 0, 1.0f, 0.0f),
              "the normal agreement saturates at dot 1 / K = 0.5");
        check(rc::lookupCornerWeight(1.0f, 1.0f, 1.0f, 4, 0, 1.0f, 0.0f) == rc::lookupCornerWeight(1.0f, 1.0f, 1.0f, 15, 0, 1.0f, 0.0f) &&
                  rc::lookupCornerWeight(1.0f, 1.0f, 1.0f, 2, 0, 1.0f, 0.0f) < rc::lookupCornerWeight(1.0f, 1.0f, 1.0f, 4, 0, 1.0f, 0.0f),
              "n_eff ramps in over four frames and saturates");
        check(rc::lookupCornerWeight(1.0f, 1.0f, 1.0f, 8, rc::kAgeMax, 1.0f, 0.0f) == 0.0f, "an age-15 cell has no weight");
    }

    // ---- 8. the HLSL mirrors this header ----------------------------------------------------------
    AVER_INFO("=== HLSL literals match the header ===");
    {
        const std::string root   = std::string(AVER_REPO_ROOT) + "/modules/render.voxi/";
        const std::string pure   = readFile(root + "shaders/voxi_neurac.hlsli");
        const std::string resolve = readFile(root + "shaders/voxi_neurac_resolve.hlsl");
        const std::string cpp    = readFile(root + "src/NeuRaC.cpp");
        const std::string io     = readFile(root + "shaders/voxi_neurac_io.hlsli");
        const std::string lookup = readFile(root + "shaders/voxi_neurac_lookup.hlsli");

        struct Lit { const char* name; f64 value; };
        const Lit lits[] = {
            {"AVER_RC_SH_SCALE", rc::kShScale},   {"AVER_RC_N_SCALE", rc::kNScale},
            {"AVER_RC_SQ_SCALE", rc::kSqScale},   {"AVER_RC_DC_TO_E", rc::kDcToE},
            {"AVER_RC_SQ_BASE", rc::kSqBase},
            {"AVER_RC_MAX_CAP", rc::kMaxCap},     {"AVER_RC_LMAX", rc::kLMax},
            {"AVER_RC_MIN_COS", rc::kMinCos},     {"AVER_RC_NEFF_MAX", rc::kNEffMax},
            {"AVER_RC_AGE_MAX", rc::kAgeMax},     {"AVER_RC_NORMAL_K", rc::kNormalK},
            {"AVER_RC_CASCADES", rc::kCascades},  {"AVER_RC_RES", rc::kRes},
            {"AVER_RC_CELLS_PER_CASC", rc::kCellsPerCascade}, {"AVER_RC_CELLS", rc::kCells},
            {"AVER_RC_PAYLOAD_BASE", rc::kPayloadBase},       {"AVER_RC_PAYLOAD_INTS", rc::kPayloadInts},
            {"AVER_RC_MARGIN_CELLS", rc::kMarginCells},       {"AVER_RC_CROSSFADE_CELLS", rc::kCrossFadeCells},
            {"AVER_RC_MIN_COVER", rc::kMinCover},
            {"AVER_RC_MIN_ACCEPT_RATE", rc::kMinAcceptRate},
            {"AVER_RC_TRUST_MAX", rc::kTrustMax},             {"AVER_RC_LEN_MAX", rc::kLenMax},
        };
        for (const Lit& l : lits) {
            const f64 v = defineValue(pure, l.name);
            check(!std::isnan(v) && approx(v, l.value, 1e-9),
                  std::string("voxi_neurac.hlsli #defines ") + l.name + " equal to NeuRaCLayout.hpp's value");
        }

        const Lit resolveLits[] = {
            {"RCR_RES", rc::kRes}, {"RCR_PER_CASC", rc::kCellsPerCascade}, {"RCR_CELLS", rc::kCells},
            {"RCR_PAYLOAD", rc::kPayloadInts}, {"RCR_PAYLOAD_BASE", rc::kPayloadBase}, {"RCR_SQ_BASE", rc::kSqBase},
        };
        for (const Lit& l : resolveLits) {
            const f64 v = defineValue(resolve, l.name);
            check(!std::isnan(v) && approx(v, l.value, 1e-9),
                  std::string("voxi_neurac_resolve.hlsl #defines ") + l.name + " equal to the header's value");
        }
        check(resolve.find("[numthreads(64, 1, 1)]") != std::string::npos && rc::kResolveGroupSize == 64,
              "the resolve kernel is [numthreads(64, 1, 1)], matching the 12,288-group dispatch");
        check(resolve.find("void CSRcResolve(") != std::string::npos && cpp.find("\"CSRcResolve\"") != std::string::npos,
              "the resolve entry point the C++ asks for exists in the shader");
        check(resolve.find("register(t0)") != std::string::npos && resolve.find("register(u0)") != std::string::npos &&
              resolve.find("register(u1)") != std::string::npos,
              "the resolve declares t0/u0/u1, the registers NeuRaC's layout has");
        check(resolve.find("#include \"voxi_neurac.hlsli\"") != std::string::npos,
              "the resolve includes the pure-maths header, not the resource-bound one");
        check(cpp.find("voxi_neurac_resolve.hlsl") != std::string::npos, "NeuRaC.cpp loads that shader file");

        // The stage-1 scatter and the shared lookup: the structure the CPU mirrors assume.
        const auto has = [](const std::string& t, const char* what) { return t.find(what) != std::string::npos; };
        check(has(io, "void rcScatterCascade(") &&
                  has(io, "[loop] for (uint c = 0; c < (uint)AVER_RC_CASCADES; ++c) rcScatterCascade("),
              "rcScatter loops over the cascades with [loop] (the twin is register-bound, so no unroll)");
        check(has(io, "floor((hitPos + N * (0.5 * size)) / size)"),
              "the scatter bins at floor((hit + N * size / 2) / size), the lookup's own cell (scatterCell)");
        check(has(io, "any(rel < AVER_RC_MARGIN_CELLS) || any(rel > AVER_RC_RES - 1 - AVER_RC_MARGIN_CELLS)"),
              "the scatter trains interior cells only: relative 1 .. 62 (scatterCell's test)");
        check(has(io, "InterlockedAdd(gRcAccum[AVER_RC_CELLS + idx], 1);") && has(io, "AVER_RC_PAYLOAD_BASE + idx"),
              "every arrival does one atomic on the arrivals array and the payload sits at AVER_RC_PAYLOAD_BASE");
        check(has(io, "InterlockedAdd(gRcAccum[AVER_RC_SQ_BASE + idx], sqFix, ignored);") &&
                  has(io, "(int)round(sE * sE * AVER_RC_SQ_SCALE)") && has(io, "dot(L, float3(0.2126, 0.7152, 0.0722)) * wgt * AVER_RC_DC_TO_E"),
              "an accepted sample adds one atomic: the square of lum(L) * weight / 4 PI, in fixed point, at AVER_RC_SQ_BASE + cell");
        check(has(resolve, "gRcAccum[RCR_SQ_BASE + cell] = 0;") && resolve.find("gRcAccum[RCR_SQ_BASE + cell] = 0;") !=
                  resolve.rfind("gRcAccum[RCR_SQ_BASE + cell] = 0;"),
              "the resolve zeroes the sum of squares with the payload, on the clear-all path and after a resolved cell");
        check(has(io, "prev <= cap ? 1.0 : max((float)cap / (float)prev, (float)AVER_RC_MIN_ACCEPT_RATE)"),
              "the accept rate is cap / prev floored at AVER_RC_MIN_ACCEPT_RATE (acceptRate)");
        check(has(io, "0x9E3779B9u") && has(io, "0x85EBCA6Bu") && has(io, "0xC2B2AE35u") && has(io, "return h >> 16;"),
              "rcHash16 is the murmur3-style mix hash16 mirrors");
        check(has(lookup, "lerp(1.0, trust, netStrength)") && has(lookup, "info.hdr1.w"),
              "the lookup scales each corner by lerp(1, trust, hdr1.w), the net strength");
        std::string lookupCode;   // the lookup header without its // comments, which name gAmbientParams to say it is absent
        {
            std::istringstream lines(lookup);
            std::string line;
            while (std::getline(lines, line)) lookupCode += line.substr(0, line.find("//")) + " ";
        }
        check(!lookupCode.empty() && !has(lookupCode, "gAmbientParams"),
              "the shared lookup header's code does not touch the twin's gAmbientParams");
        check(has(resolve, "rcCapFor(info, cascade)") && has(resolve, "max(info.hdr1.x, info.hdr1.y)") &&
                  has(resolve, "rcNormalTrust6(stored.b.z)"),
              "the resolve caps per cascade, floors alpha at max(alphaMin, relaxAlpha) and carries a valid cell's trust");
        check(has(cpp, "cap1 | (cap2 << 8)") && has(cpp, "info.relaxAlpha"),
              "NeuRaC.cpp packs the coarse caps as cap1 | cap2 << 8 and writes relaxAlpha");
    }

    if (g_failures == 0) {
        AVER_INFO("=== all {} radiance-cache checks passed ===", g_checks);
        return 0;
    }
    AVER_ERROR("=== {} FAILED ===", g_failures);
    return 1;
}
