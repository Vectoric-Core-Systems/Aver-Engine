// RadianceCacheTest -- the radiance cache's CPU-checkable half (aver/voxi/RadianceCacheLayout.hpp).
// See docs/rendering/RADIANCE_CACHE.md.
//
// NO GPU, NO RHI, same shape as GiVisibilityTest in this directory: the header under test depends on
// nothing but aver/core/Types.hpp, so this links Aver.Core alone and reaches it with an include path.
//
// WHAT THIS CAN AND CANNOT CATCH. It checks the ARITHMETIC both sides share -- the fixed-point headroom
// (a wrapped InterlockedAdd is a wrong colour, not a crash), tag and cell addressing including negative
// world cells, origin snapping, the fp16 SH and normal packing, and the SH cosine convolution against a
// Monte Carlo estimate built with the scatter's own weighting. It also reads the HLSL files and checks
// that the AVER_RC_* #define literals equal the header's constants. It does NOT compile the HLSL or run
// the resolve: whether voxi_radiance_cache_resolve.hlsl builds and blends correctly is the engine's to
// show, not this file's.
#include "aver/core/Log.hpp"
#include "aver/voxi/RadianceCacheLayout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace aver;
namespace rc = aver::voxi::radiancecache;

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
        // An unreadable file must FAIL every assertion against it, not skip: an empty string would make
        // every "does not contain" style check pass vacuously.
        AVER_ERROR("[RadianceCache] could not read {} -- every source assertion against it is a failure, "
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
        check(rc::kAccumInts == 12582912u && rc::kAccumBytes == 50331648ull, "accumulator is 12,582,912 ints = 50,331,648 B");
        check(rc::kCellsBytes == 25165824ull, "cells are 786,432 x 32 B = 25,165,824 B");
        check(sizeof(rc::RcInfo) == 80 && sizeof(rc::RcCell) == 32, "RcInfo is 80 B and RcCell is 32 B");
        check(rc::kResolveGroups == 12288u && rc::kResolveGroups * rc::kResolveGroupSize == rc::kCells,
              "12,288 groups of 64 threads cover every cell exactly once");
        check(rc::kCellSizeCm[0] == 25.0f && rc::kCellSizeCm[1] == 100.0f && rc::kCellSizeCm[2] == 400.0f,
              "cell sizes are 25 / 100 / 400 cm");
        const f64 total = static_cast<f64>(rc::kAccumBytes + rc::kCellsBytes) / (1024.0 * 1024.0);
        check(total > 71.0 && total < 73.0, "the VRAM bill is ~72 MiB (75.5 MB), as the owner accepted");
    }

    // ---- 2. fixed-point headroom ------------------------------------------------------------------
    AVER_INFO("=== fixed-point headroom ===");
    {
        // The worst sample the scatter can produce, summed cap times in int64 (so the test itself cannot
        // wrap), must stay under half of int32 max.
        const f64 worst = rc::kLMax * 0.488603 * rc::kPi / rc::kMinCos;
        std::int64_t sum = 0;
        for (u32 i = 0; i < rc::kMaxCap; ++i) sum += static_cast<std::int64_t>(std::llround(worst * rc::kShScale));
        check(sum < 2147483647ll / 2, "64 worst-case SH samples sum to " + std::to_string(sum) + ", under half of int32 max");
        std::int64_t nsum = 0;
        for (u32 i = 0; i < rc::kMaxCap; ++i) nsum += static_cast<std::int64_t>(std::llround(1.0 * rc::kNScale));
        check(nsum < 2147483647ll / 2, "64 unit normals sum to " + std::to_string(nsum) + ", far under int32 max");
        check(rc::kWorstSample > 480.0 && rc::kWorstSample < 500.0, "the documented worst sample is ~491");
        check(rc::kShScale == 32768.0 && rc::kNScale == 65536.0, "scales are 2^15 and 2^16");
    }

    // ---- 3. tags and cell indices ------------------------------------------------------------------
    AVER_INFO("=== tag and cell addressing ===");
    {
        check(rc::tagPack(0, 0, 0) == 0u, "world cell (0,0,0) has tag 0");
        check(rc::tagPack(63, 63, 63) == 0u, "the first 64-cell block shares one tag");
        check(rc::tagPack(64, 0, 0) == 1u && rc::tagPack(0, 64, 0) == (1u << 8) && rc::tagPack(0, 0, 64) == (1u << 16),
              "one block along an axis moves that axis' byte by one");
        // Negative cells: arithmetic shift puts -1 in block -1 (0xFF), which is distinct from block 0.
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

        // A window anchored at a NEGATIVE origin: every one of its 64^3 cells must land on a distinct texel,
        // and exactly two tag values per axis at most (the toroidal property the resolve relies on).
        const i32 ox = -37, oy = -90, oz = 17;
        std::vector<std::uint8_t> seen(rc::kCellsPerCascade, 0);
        u32 collisions = 0;
        bool tagsOk = true;
        for (i32 z = 0; z < 64; ++z) for (i32 y = 0; y < 64; ++y) for (i32 x = 0; x < 64; ++x) {
            const u32 idx = rc::cellIndex(1, ox + x, oy + y, oz + z) - rc::kCellsPerCascade;
            if (seen[idx]++) ++collisions;
            // The resolve's derivation: world = origin + ((texel - origin) & 63) must return the world cell.
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
        // 99 is 0b1100011: only its low 4 bits (3) may land in each 4-bit field, and the 32-bit tag
        // only its low 24 -- so every field reads back masked and nothing spills into a neighbour.
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
        for (int i = 0; i < 20000; ++i) {
            f32 v[3] = {d(rng), d(rng), d(rng)};
            const f32 l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (l < 0.05f) continue;
            const f32 len = std::min(1.0f, l);   // lengths are mean-normal lengths, <= 1
            for (f32& c : v) c = c / l * len;
            f32 dir[3], outLen;
            rc::unpackNormal(rc::packNormal(v), dir, outLen);
            const f32 cosA = (dir[0] * v[0] + dir[1] * v[1] + dir[2] * v[2]) / len;
            worstCos = std::min(worstCos, cosA);
            worstLen = std::max(worstLen, std::fabs(outLen - len));
        }
        check(worstCos > 0.99998f, "octahedral directions round-trip within ~0.4 degrees (worst cos " + std::to_string(worstCos) + ")");
        check(worstLen < 1.0f / 1023.0f, "the length field is within one 10-bit step (worst error " + std::to_string(worstLen) + ")");
        // Axis-aligned normals are the common case: they must land exactly on the axis.
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
        // Layout: k = 0 lives in the low half of word 0, k = 1 in its high half.
        f32 sh[rc::kShFloats] = {1.0f, 2.0f};
        u32 w[6];
        rc::packSh(sh, w);
        check((w[0] & 0xFFFFu) == 0x3C00u && (w[0] >> 16) == 0x4000u && w[1] == 0u, "low half = even k, high half = odd k");
    }

    // ---- 7. SH convolution and the scatter's estimator --------------------------------------------
    AVER_INFO("=== SH irradiance and the scatter estimator ===");
    {
        // Analytic: constant L over the hemisphere around N -> c0 = Y0 * 2 PI L, cN = 0.488603 * PI L, and
        // the cosine convolution gives back exactly L (the contract's own check).
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

        // Monte Carlo with the scatter's own weighting: cosine-sampled directions (Malley), L clamped, weight
        // PI / max(cos, MIN_COS), fixed-point rounded, summed in int64, divided by n * scale.
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
            // The cos floor under-weights the 1% of samples below 0.1 (documented bias): a few percent low,
            // never high.
            check(out[0] > 0.94f * Lsample && out[0] < 1.01f * Lsample,
                  "Monte Carlo of constant L reproduces L within the floor's few-percent bias (got " + std::to_string(out[0]) + ")");
            const f64 nx = static_cast<f64>(nacc[0]) / (nSamples * rc::kNScale);
            const f64 ny = static_cast<f64>(nacc[1]) / (nSamples * rc::kNScale);
            const f64 nz = static_cast<f64>(nacc[2]) / (nSamples * rc::kNScale);
            check(approx(nx, N[0], 1e-3) && approx(ny, N[1], 1e-3) && approx(nz, N[2], 1e-3),
                  "the summed-normal fixed point recovers the normal");
        }
    }

    // ---- 8. the HLSL mirrors this header ----------------------------------------------------------
    AVER_INFO("=== HLSL literals match the header ===");
    {
        const std::string root   = std::string(AVER_REPO_ROOT) + "/modules/render.voxi/";
        const std::string pure   = readFile(root + "shaders/voxi_radiance_cache.hlsli");
        const std::string resolve = readFile(root + "shaders/voxi_radiance_cache_resolve.hlsl");
        const std::string cpp    = readFile(root + "src/RadianceCache.cpp");

        struct Lit { const char* name; f64 value; };
        const Lit lits[] = {
            {"AVER_RC_SH_SCALE", rc::kShScale},   {"AVER_RC_N_SCALE", rc::kNScale},
            {"AVER_RC_MAX_CAP", rc::kMaxCap},     {"AVER_RC_LMAX", rc::kLMax},
            {"AVER_RC_MIN_COS", rc::kMinCos},     {"AVER_RC_NEFF_MAX", rc::kNEffMax},
            {"AVER_RC_AGE_MAX", rc::kAgeMax},     {"AVER_RC_NORMAL_K", rc::kNormalK},
        };
        for (const Lit& l : lits) {
            const f64 v = defineValue(pure, l.name);
            check(!std::isnan(v) && approx(v, l.value, 1e-9),
                  std::string("voxi_radiance_cache.hlsli #defines ") + l.name + " equal to RadianceCacheLayout.hpp's value");
        }

        const Lit resolveLits[] = {
            {"RCR_RES", rc::kRes}, {"RCR_PER_CASC", rc::kCellsPerCascade}, {"RCR_CELLS", rc::kCells},
            {"RCR_PAYLOAD", rc::kPayloadInts},
        };
        for (const Lit& l : resolveLits) {
            const f64 v = defineValue(resolve, l.name);
            check(!std::isnan(v) && approx(v, l.value, 1e-9),
                  std::string("voxi_radiance_cache_resolve.hlsl #defines ") + l.name + " equal to the header's value");
        }
        check(resolve.find("[numthreads(64, 1, 1)]") != std::string::npos && rc::kResolveGroupSize == 64,
              "the resolve kernel is [numthreads(64, 1, 1)], matching the 12,288-group dispatch");
        check(resolve.find("void CSRcResolve(") != std::string::npos && cpp.find("\"CSRcResolve\"") != std::string::npos,
              "the resolve entry point the C++ asks for exists in the shader");
        check(resolve.find("register(t0)") != std::string::npos && resolve.find("register(u0)") != std::string::npos &&
              resolve.find("register(u1)") != std::string::npos,
              "the resolve declares t0/u0/u1, the registers RadianceCache's layout has");
        check(resolve.find("#include \"voxi_radiance_cache.hlsli\"") != std::string::npos,
              "the resolve includes the pure-maths header, not the resource-bound one");
        check(cpp.find("voxi_radiance_cache_resolve.hlsl") != std::string::npos, "RadianceCache.cpp loads that shader file");
    }

    if (g_failures == 0) {
        AVER_INFO("=== all {} radiance-cache checks passed ===", g_checks);
        return 0;
    }
    AVER_ERROR("=== {} FAILED ===", g_failures);
    return 1;
}
