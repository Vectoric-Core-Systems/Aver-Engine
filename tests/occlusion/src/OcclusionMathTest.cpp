// Aver.Occlusion's pure half, checked with no GPU and no renderer -- see
// modules/occlusion/include/aver/occlusion/OcclusionMath.hpp for what these three functions promise
// and modules/occlusion/include/aver/occlusion/Occlusion.hpp for why the promise matters (the
// "FALSE IS ALWAYS SAFE" / "TRUE IS A PROMISE" split this file exists to hold to account).
#include "aver/occlusion/OcclusionMath.hpp"
#include "aver/core/Log.hpp"

#include <cstring>
#include <string>

using namespace aver;
using namespace aver::occlusion;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static void checkNear(f32 got, f32 want, f32 tol, const std::string& what) {
    if (std::fabs(got - want) <= tol) { AVER_INFO("  ok    {} ({:.6g} vs {:.6g})", what, got, want); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {} ({:.6g} vs {:.6g}, tol {:.3g})", what, got, want, tol);
}

// Row-major, row-vector identity: clip == (x, y, z, 1), so ndc == world position directly and every
// expected UV/Z below can be hand-computed from the box's own corners.
static const f32 kIdentity[16] = {
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1,
};

// A synthetic pyramid, built by hand rather than rendered: mip 0 is 8x8, storing a NEAR wall
// (z = 0.30) covering its left THREE columns (x < 3, u < 0.375) and open sky (z = 1.0, the far clear
// value) everywhere else. Mip 1 (4x4) is the max() of mip 0's 2x2 blocks, mip 2 (2x2) the max() of
// mip 1's, and mip 3 (1x1) covers everything -- computed by the SAME rule buildPyramid()'s CSReduce
// kernel runs on the GPU, so a bug in this test fixture would itself be caught by checkTest's
// specific-camera cases below (a wrong mip 3 would make the "definitely open sky" case fail).
//
// THE BOUNDARY SITS AT AN ODD COLUMN (3, not 4) ON PURPOSE: a boundary aligned to a mip-1 block
// (every 2 columns) would never produce a MIXED 2x2 block, so max() and min() reduction would
// coincidentally agree everywhere and the min()-vs-max() demonstration below would prove nothing.
// Column 3 falls inside mip1's block 1 (source columns 2-3: wall, sky), which is exactly the mixed
// block that makes the two reductions diverge.
struct SyntheticPyramid {
    static constexpr u32 kW = 8, kH = 8, kMips = 4;
    f32 mip0[kH][kW];
    f32 mip1[4][4];
    f32 mip2[2][2];
    f32 mip3[1][1];

    SyntheticPyramid() {
        for (u32 y = 0; y < kH; ++y)
            for (u32 x = 0; x < kW; ++x)
                mip0[y][x] = (x < 3) ? 0.30f : 1.0f;
        for (u32 y = 0; y < 4; ++y)
            for (u32 x = 0; x < 4; ++x) {
                f32 m = 0.0f;
                for (u32 dy = 0; dy < 2; ++dy)
                    for (u32 dx = 0; dx < 2; ++dx)
                        m = std::max(m, mip0[y * 2 + dy][x * 2 + dx]);
                mip1[y][x] = m;
            }
        for (u32 y = 0; y < 2; ++y)
            for (u32 x = 0; x < 2; ++x) {
                f32 m = 0.0f;
                for (u32 dy = 0; dy < 2; ++dy)
                    for (u32 dx = 0; dx < 2; ++dx)
                        m = std::max(m, mip1[y * 2 + dy][x * 2 + dx]);
                mip2[y][x] = m;
            }
        mip3[0][0] = std::max({mip2[0][0], mip2[0][1], mip2[1][0], mip2[1][1]});
    }

    f32 sample(u32 mip, f32 u, f32 v) const {
        u32 w = std::max(kW >> mip, 1u), h = std::max(kH >> mip, 1u);
        u32 x = static_cast<u32>(std::min(std::max(u * static_cast<f32>(w), 0.0f), static_cast<f32>(w) - 1.0f));
        u32 y = static_cast<u32>(std::min(std::max(v * static_cast<f32>(h), 0.0f), static_cast<f32>(h) - 1.0f));
        switch (mip) {
            case 0: return mip0[y][x];
            case 1: return mip1[y][x];
            case 2: return mip2[y][x];
            default: return mip3[0][0];
        }
    }
};

static void checkProjection() {
    AVER_INFO("projectAabbScreenBounds (identity camera)");

    // A box entirely in front of the eye (z in [0.2, 0.4], all w > 0 for identity where w == 1).
    {
        const f32 lo[3] = {-0.25f, -0.25f, 0.20f}, hi[3] = {0.25f, 0.25f, 0.40f};
        f32 minUV[2], maxUV[2], nearestZ = 0.0f;
        const bool ok = projectAabbScreenBounds(kIdentity, lo, hi, minUV, maxUV, nearestZ);
        check(ok, "in-front box projects");
        // u = x*0.5+0.5: x in [-0.25,0.25] -> u in [0.375,0.625].
        checkNear(minUV[0], 0.375f, 1e-5f, "minU");
        checkNear(maxUV[0], 0.625f, 1e-5f, "maxU");
        // world +Y maps to LOWER v (texture V is down, clip Y is up): hi.y (+0.25) -> minV.
        checkNear(minUV[1], 0.375f, 1e-5f, "minV (from world +Y)");
        checkNear(maxUV[1], 0.625f, 1e-5f, "maxV (from world -Y)");
        checkNear(nearestZ, 0.20f, 1e-5f, "nearestZ is the box's CLOSEST corner, not its centre or its far side");
    }

    // A box entirely behind the eye (w <= 0 at every corner under this identity stand-in, where
    // clip.w == 1 always -- so instead use a box whose w would be <= 0 under a REAL perspective
    // matrix is not expressible with the identity fixture; cover the "no corner in front" path via
    // the degenerate all-clipped rect below instead, which exercises the same early-return.)
    {
        // A box entirely outside the [0,1]x[0,1] screen (off to the right) must not silently report
        // some clamped sliver as its bounds: it should fail to project a NON-DEGENERATE rect.
        const f32 lo[3] = {3.0f, -0.1f, 0.5f}, hi[3] = {3.2f, 0.1f, 0.6f};
        f32 minUV[2], maxUV[2], nearestZ = 0.0f;
        const bool ok = projectAabbScreenBounds(kIdentity, lo, hi, minUV, maxUV, nearestZ);
        check(!ok, "off-screen box does not produce a visible rect");
    }
}

static void checkMipSelect() {
    AVER_INFO("selectConservativeMip");
    // A footprint AT MOST 1.0 texel wide (the safe bound -- see the function's own comment) stays at
    // mip 0: at most 1 texel can touch at most 2 texel indices, so a 4-corner sample cannot miss one.
    check(selectConservativeMip(1.0f / 64.0f, 1.0f / 64.0f, 64, 64, 6) == 0, "1-texel footprint -> mip 0 (the safe boundary, inclusive)");
    // Anything over 1.0 texel must bump to mip 1 or coarser -- this is BUG 1's fix: the pre-fix
    // formula kept mip 0 for anything up to 2.0 texels, which is what let a footprint spanning 3
    // texel indices sample only 2 of them. See checkFalseCullRegression() for the end-to-end box
    // this boundary actually protects.
    check(selectConservativeMip(1.01f / 64.0f, 1.0f / 64.0f, 64, 64, 6) == 1, "just-over-1-texel footprint -> mip 1, not mip 0");
    // A footprint spanning the WHOLE screen at a 64-wide pyramid needs mip 6 (64 texels -> 1 texel is
    // 6 halvings) at most, clamped to whatever maxMip the pyramid actually has.
    check(selectConservativeMip(1.0f, 1.0f, 64, 64, 5) == 5, "full-screen footprint clamps to maxMip");
    // Monotonic: a wider footprint never selects a FINER mip than a narrower one at the same height.
    const u32 a = selectConservativeMip(0.10f, 0.02f, 256, 256, 8);
    const u32 b = selectConservativeMip(0.40f, 0.02f, 256, 256, 8);
    check(b >= a, "wider footprint selects an equal-or-coarser mip");
}

static void checkConservativelyHidden() {
    AVER_INFO("conservativelyHidden (synthetic pyramid: near wall on the left half, open sky on the right)");
    const SyntheticPyramid pyr;
    PyramidSample sample4 = [&](u32 mip, f32 u, f32 v) { return pyr.sample(mip, u, v); };

    // A box entirely on the LEFT (u < 0.5) and farther than the wall (z = 0.30) is hidden. Kept at
    // 0.1x0.1 UV (0.8x0.8 mip-0 texels, <= the 1.0-texel safe bound selectConservativeMip now
    // enforces -- see its own comment) so this stays at mip 0, squarely inside the wall's u < 0.375
    // region with no neighbouring mip-1 block in play: a wider box here (originally 0.2x0.2, 1.6
    // texels) legitimately bumps to mip 1 post-fix, whose block 1 (source columns 2-3) is the
    // MIXED wall/sky block the file's own comment calls out on purpose, and reading sky there is
    // the CORRECT answer, not a regression -- covered instead by the "wall/sky boundary" and
    // "min()-reduced pyramid" cases below, and by checkFalseCullRegression()'s own dedicated cases.
    {
        const f32 minUV[2] = {0.10f, 0.40f}, maxUV[2] = {0.20f, 0.50f};
        const bool hidden = conservativelyHidden(minUV, maxUV, /*nearestZ*/ 0.5f, SyntheticPyramid::kW,
                                                  SyntheticPyramid::kH, SyntheticPyramid::kMips, sample4);
        check(hidden, "box behind the wall, fully inside the wall's footprint -> hidden");
    }

    // The SAME box position, but closer than the wall: must NOT be hidden.
    {
        const f32 minUV[2] = {0.10f, 0.40f}, maxUV[2] = {0.20f, 0.50f};
        const bool hidden = conservativelyHidden(minUV, maxUV, /*nearestZ*/ 0.10f, SyntheticPyramid::kW,
                                                  SyntheticPyramid::kH, SyntheticPyramid::kMips, sample4);
        check(!hidden, "box in front of the wall -> not hidden");
    }

    // A box straddling the wall/sky boundary (u from 0.30 to 0.42, the boundary sits at 0.375) at a
    // depth that would be hidden against the wall alone must NOT be culled: part of its footprint is
    // open sky, and reporting it hidden there would be exactly the false cull this whole design
    // exists to rule out. At a small enough footprint this samples mip 0 directly, where the boundary
    // is exact.
    {
        const f32 minUV[2] = {0.30f, 0.30f}, maxUV[2] = {0.42f, 0.42f};
        const bool hidden = conservativelyHidden(minUV, maxUV, /*nearestZ*/ 0.5f, SyntheticPyramid::kW,
                                                  SyntheticPyramid::kH, SyntheticPyramid::kMips, sample4);
        check(!hidden, "box straddling wall/sky boundary -> not hidden (a false cull here is a bug)");
    }

    // A box spanning nearly the WHOLE screen (forces a coarse mip) at a depth beyond the wall must
    // still not be hidden -- the sky half of the coarse mip's own max() is 1.0 (open), so even the
    // coarsest mip cannot manufacture a false "hidden" out of a footprint that includes open sky.
    {
        const f32 minUV[2] = {0.02f, 0.02f}, maxUV[2] = {0.98f, 0.98f};
        const bool hidden = conservativelyHidden(minUV, maxUV, /*nearestZ*/ 0.5f, SyntheticPyramid::kW,
                                                  SyntheticPyramid::kH, SyntheticPyramid::kMips, sample4);
        check(!hidden, "near-full-screen footprint spanning both wall and sky -> not hidden");
    }

    // Getting the reduction backwards (min() of the four parents instead of max()) is exactly the
    // bug this design's own comment warns about (Occlusion.hpp: "getting this backwards... is caught
    // by a box popping invisible"). Demonstrate it here with a hand-built min()-reduced mip 1 standing
    // in for a buggy CSReduce: mip1's block 1 (source columns 2-3: wall 0.30, sky 1.0) is a MIXED
    // block -- max()-reducing it keeps 1.0 (correctly "nothing occludes here"), min()-reducing it
    // keeps 0.30 (the wall's depth leaking into a cell that is half open sky).
    {
        f32 buggyMip1[4][4];
        for (u32 y = 0; y < 4; ++y)
            for (u32 x = 0; x < 4; ++x) {
                f32 m = 1e30f;
                for (u32 dy = 0; dy < 2; ++dy)
                    for (u32 dx = 0; dx < 2; ++dx)
                        m = std::min(m, pyr.mip0[y * 2 + dy][x * 2 + dx]);
                buggyMip1[y][x] = m;
            }
        PyramidSample buggySample = [&](u32 mip, f32 u, f32 v) -> f32 {
            if (mip == 0) return pyr.sample(0, u, v);
            u32 x = static_cast<u32>(std::min(std::max(u * 4.0f, 0.0f), 3.0f));
            u32 y = static_cast<u32>(std::min(std::max(v * 4.0f, 0.0f), 3.0f));
            return buggyMip1[y][x];
        };
        // A footprint narrow enough in U that every corner lands in the SAME mip-1 texel (block 1,
        // u in [0.25, 0.5)) -- so there is no OTHER, unambiguous corner left to rescue a wrong answer
        // there -- but tall enough in V (texelH > 2 mip-0 texels) that selectConservativeMip still
        // picks mip 1 rather than mip 0, where the boundary is exact and neither reduction is in play.
        const f32 minUV[2] = {0.26f, 0.10f}, maxUV[2] = {0.49f, 0.40f};
        const bool hiddenCorrect = conservativelyHidden(minUV, maxUV, 0.5f, SyntheticPyramid::kW,
                                                         SyntheticPyramid::kH, SyntheticPyramid::kMips, sample4);
        const bool hiddenBuggy = conservativelyHidden(minUV, maxUV, 0.5f, SyntheticPyramid::kW,
                                                       SyntheticPyramid::kH, SyntheticPyramid::kMips, buggySample);
        check(!hiddenCorrect, "max()-reduced pyramid: mixed-block footprint stays visible");
        check(hiddenBuggy, "min()-reduced pyramid WOULD falsely hide the same box (this is the bug the design avoids)");
    }
}

// ---------------------------------------------------------------------------------------------
// Regression: BUG 1, a false cull through an untested middle texel. Before the fix,
// selectConservativeMip returned mip 0 for any footprint up to 2.0 mip-0 texels wide, and
// conservativelyHidden then sampled only that mip's 4 CORNERS. A footprint wide enough to straddle
// 3 texel indices (anything over 1.0 texel, off any texel boundary) could land both corner samples
// on occluded texels while a genuinely open texel sat, untested, between them -- see
// OcclusionMath.hpp's selectConservativeMip for the derivation of why the safe bound is 1.0 texel,
// not 2.0.
//
// Own small pyramid (not SyntheticPyramid above): a single ROW pattern is enough to demonstrate a
// gap between two occluded texels, reduced through the same max()-of-2x2, edge-clamped rule
// OcclusionCuller.cpp's CSReduce kernel runs on the GPU, so the mip chain this test exercises is
// built the same way the real one is, not a hand-waved stand-in.
// ---------------------------------------------------------------------------------------------
struct GapPyramid {
    static constexpr u32 kW = 8, kH = 8, kMips = 4;
    f32 mip0[kH][kW];
    f32 mip1[4][4];
    f32 mip2[2][2];
    f32 mip3[1][1];

    explicit GapPyramid(const f32 (&baseRow)[kW]) {
        for (u32 y = 0; y < kH; ++y)
            for (u32 x = 0; x < kW; ++x)
                mip0[y][x] = baseRow[x];
        for (u32 y = 0; y < 4; ++y)
            for (u32 x = 0; x < 4; ++x) {
                f32 m = 0.0f;
                for (u32 dy = 0; dy < 2; ++dy)
                    for (u32 dx = 0; dx < 2; ++dx)
                        m = std::max(m, mip0[y * 2 + dy][x * 2 + dx]);
                mip1[y][x] = m;
            }
        for (u32 y = 0; y < 2; ++y)
            for (u32 x = 0; x < 2; ++x) {
                f32 m = 0.0f;
                for (u32 dy = 0; dy < 2; ++dy)
                    for (u32 dx = 0; dx < 2; ++dx)
                        m = std::max(m, mip1[y * 2 + dy][x * 2 + dx]);
                mip2[y][x] = m;
            }
        mip3[0][0] = std::max({mip2[0][0], mip2[0][1], mip2[1][0], mip2[1][1]});
    }

    f32 sample(u32 mip, f32 u, f32 v) const {
        u32 w = std::max(kW >> mip, 1u), h = std::max(kH >> mip, 1u);
        u32 x = static_cast<u32>(std::min(std::max(u * static_cast<f32>(w), 0.0f), static_cast<f32>(w) - 1.0f));
        u32 y = static_cast<u32>(std::min(std::max(v * static_cast<f32>(h), 0.0f), static_cast<f32>(h) - 1.0f));
        switch (mip) {
            case 0: return mip0[y][x];
            case 1: return mip1[y][x];
            case 2: return mip2[y][x];
            default: return mip3[0][0];
        }
    }
};

static void checkFalseCullRegression() {
    AVER_INFO("conservativelyHidden regression: BUG 1, false cull through an untested middle texel");

    // 1.992 mip-0 texels wide, offset half a texel off any boundary so its span touches EXACTLY
    // three texel indices (0, 1, 2) with the two corner samples landing on indices 0 and 2 -- index
    // 1, in the middle, is never sampled by a 4-corner test.
    const f32 minUV[2] = {0.05f, 0.05f};
    const f32 maxUV[2] = {0.299f, 0.06f};

    // NEGATIVE: occluded / GAP / occluded / sky sky sky sky sky -- genuinely visible through the gap
    // at texel 1. This is the exact counterexample from the bug report: on the unfixed
    // selectConservativeMip (largest <= 2.0f keeps mip 0 regardless), this comes back TRUE (culled),
    // which is wrong -- the box is visible.
    {
        const f32 gapRow[GapPyramid::kW] = {0.30f, 1.00f, 0.30f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        const GapPyramid pyr(gapRow);
        PyramidSample sample4 = [&](u32 mip, f32 u, f32 v) { return pyr.sample(mip, u, v); };
        const bool hidden = conservativelyHidden(minUV, maxUV, /*nearestZ*/ 0.5f, GapPyramid::kW,
                                                  GapPyramid::kH, GapPyramid::kMips, sample4);
        check(!hidden, "FalseCull_ThreeTexelGapNotCulled: box visible through an untested middle "
                        "texel must not be culled (this check fails against the unfixed "
                        "selectConservativeMip -- see OcclusionMath.hpp)");
    }

    // POSITIVE CONTROL: solid occlusion at every texel, at every mip built from it (no gap for the
    // coarser mip's max() reduction to ever expose) -- a fix that closes the false cull by simply
    // disabling/weakening culling would pass the negative case above and fail this one.
    {
        const f32 wallRow[GapPyramid::kW] = {0.30f, 0.30f, 0.30f, 0.30f, 0.30f, 0.30f, 0.30f, 0.30f};
        const GapPyramid pyr(wallRow);
        PyramidSample sample4 = [&](u32 mip, f32 u, f32 v) { return pyr.sample(mip, u, v); };
        const bool hidden = conservativelyHidden(minUV, maxUV, /*nearestZ*/ 0.5f, GapPyramid::kW,
                                                  GapPyramid::kH, GapPyramid::kMips, sample4);
        check(hidden, "FullyOccluded_StillCulled: a genuinely, fully occluded footprint (no gap "
                       "anywhere in it, at any mip) must still be culled");
    }
}

int main() {
    AVER_INFO("OcclusionMathTest");
    checkProjection();
    checkMipSelect();
    checkConservativelyHidden();
    checkFalseCullRegression();
    if (g_failures) AVER_ERROR("OcclusionMathTest: {} failure(s)", g_failures);
    else            AVER_INFO("OcclusionMathTest: all checks passed");
    return g_failures ? 1 : 0;
}
