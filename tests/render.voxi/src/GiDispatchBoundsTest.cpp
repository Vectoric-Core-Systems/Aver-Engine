// GiDispatchBoundsTest -- W3's occupied-bounds arithmetic (aver/voxi/GiDispatchBounds.hpp): the box a
// bounded GI rebuild confines CSClear/CSResolve/CSMip to, and its byte-for-byte mirror of voxi.hlsl's
// MipCB cbuffer.
//
// NO GPU, NO RHI, same shape of test as CameraFactorTest and VoxiRtSeqTest in this same directory: the
// header under test is pure integer/float arithmetic on three axes at a time, decidable on the CPU
// alone, and (see its own top comment) deliberately depends on nothing but aver/core/Types.hpp.
//
// WHAT WOULD HAVE CAUGHT THE BUGS THIS SHAPE OF HEADER INVITES. A one-voxel error in mipBox's rounding
// direction would still LOOK like GI -- indirect light would simply go missing or stay stale at a mip
// boundary, which reads as "GI is a little dim there today" rather than as a crash. Section 4/5 below
// check mipBox against TWO independent routes (a double-precision ceil, and a footprint-intersection
// definition of "which mip-m voxels this mip-0 box actually touches") rather than re-deriving the same
// integer formula the header itself uses, which would only prove the code agrees with itself.
//
// THE FINAL SECTION ties this file's arithmetic to the shader it exists to describe: it reads
// modules/render.voxi/shaders/voxi.hlsl (through AVER_REPO_ROOT, the same approach VoxiRtSeqTest and
// MaterialTest use) and checks the MipCB declaration and the CSClear/CSResolve/CSMip guard line are
// still the exact text C-4 specifies. Without it this suite would keep passing about a cbuffer layout
// the shader had already drifted away from.
#include "aver/core/Log.hpp"
#include "aver/voxi/GiDispatchBounds.hpp"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <string>

using namespace aver;
using namespace aver::voxi;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

bool boxEq(const VoxelBox& a, const VoxelBox& b) {
    for (int i = 0; i < 3; ++i)
        if (a.lo[i] != b.lo[i] || a.hi[i] != b.hi[i]) return false;
    return true;
}

VoxelBox makeBox(u32 lox, u32 loy, u32 loz, u32 hix, u32 hiy, u32 hiz) {
    VoxelBox b;
    b.lo[0] = lox; b.lo[1] = loy; b.lo[2] = loz;
    b.hi[0] = hix; b.hi[1] = hiy; b.hi[2] = hiz;
    return b;
}

// Does mip-`m` voxel index `v`'s 2^m footprint at mip 0 ([v*2^m, (v+1)*2^m)) intersect the half-open
// mip-0 interval [lo0, hi0)? Written independently of mipBox's own integer ceil-division so that
// section 4 below is a check of mipBox against a DIFFERENT definition of correctness, not a restatement
// of the same formula.
bool footprintIntersects(u32 v, u32 m, u32 lo0, u32 hi0) {
    const u64 fLo = static_cast<u64>(v) << m;
    const u64 fHi = fLo + (static_cast<u64>(1) << m);
    return fLo < static_cast<u64>(hi0) && static_cast<u64>(lo0) < fHi;
}

// ---------------------------------------------------------------- the shader source assertions
const std::string& hlslText() {
    static const std::string s = [] {
        const std::string path =
            std::string(AVER_REPO_ROOT) + "/modules/render.voxi/shaders/voxi.hlsl";
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            // AN UNREADABLE FILE MUST FAIL, NOT SKIP: every assertion below is a substring search, so
            // an empty string would make all of them pass vacuously.
            AVER_ERROR("[GiDispatchBounds] could not read {} -- every source assertion below would "
                       "pass vacuously against an empty string, so this is a failure, not a skip.",
                       path);
            return std::string();
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }();
    return s;
}

bool hlslHas(const char* needle) { return hlslText().find(needle) != std::string::npos; }

// The text of one HLSL function, from its signature to the first line that closes it -- same
// convention VoxiRtSeqTest's hlslBody uses, so the guard line can be located INSIDE a named kernel
// rather than anywhere in the file (a guard line that merely exists somewhere would be a much weaker
// claim than "CSResolve itself begins with it").
std::string hlslBody(const char* signature) {
    const char* start = std::strstr(hlslText().c_str(), signature);
    if (!start) return {};
    const char* end = std::strstr(start, "\n}");
    return end ? std::string(start, static_cast<usize>(end - start)) : std::string(start);
}

} // namespace

int main() {
    AVER_INFO("[GiDispatchBounds] W3's occupied-bounds arithmetic for CSClear/CSResolve/CSMip");

    // ---- 1. unionBox: identity, commutativity, correctness ----
    {
        const VoxelBox a = makeBox(4, 10, 0, 20, 30, 8);
        const VoxelBox b = makeBox(0, 15, 2, 12, 40, 6);
        const VoxelBox empty{};   // lo == hi == {0,0,0}: empty by construction

        check(boxEq(unionBox(a, empty), a), "unionBox: an empty right operand is the identity");
        check(boxEq(unionBox(empty, a), a), "unionBox: an empty left operand is the identity");
        check(boxEq(unionBox(a, b), unionBox(b, a)), "unionBox is commutative");

        const VoxelBox u = unionBox(a, b);
        const VoxelBox want = makeBox(0, 10, 0, 20, 40, 8);
        check(boxEq(u, want), "unionBox takes the componentwise min(lo) / max(hi) of the two boxes");

        check(unionBox(empty, empty).empty(), "unionBox of two empty boxes is empty");
    }

    // ---- 2. voxelBoxFromWorldAabb: padding, clamping, wholly-outside, non-finite ----
    {
        const f32 origin[3] = {0.0f, 0.0f, 0.0f};
        const f32 volumeSize = 100.0f;   // 100 world units, so 1 voxel = 100/res units at res below
        const u32 res = 128;             // 1 voxel = 0.78125 world units

        // A box comfortably inside the volume: padding should simply subtract/add padVoxels from the
        // unpadded floor/ceil result, no clamping in play.
        {
            const f32 mn[3] = {10.0f, 10.0f, 10.0f}, mx[3] = {20.0f, 20.0f, 20.0f};
            const VoxelBox b0 = voxelBoxFromWorldAabb(mn, mx, origin, volumeSize, res, 0);
            const VoxelBox bp = voxelBoxFromWorldAabb(mn, mx, origin, volumeSize, res, 2);
            check(!b0.empty(), "an AABB inside the volume produces a nonempty box");
            for (int i = 0; i < 3; ++i) {
                check(bp.lo[i] == b0.lo[i] - 2, "padVoxels=2 subtracts exactly 2 from lo on axis " +
                                                std::to_string(i) + " away from any edge");
                check(bp.hi[i] == b0.hi[i] + 2, "padVoxels=2 adds exactly 2 to hi on axis " +
                                                std::to_string(i) + " away from any edge");
            }
        }
        // Clamping at 0: an AABB straddling the volume's lower corner, padded, must not go negative.
        {
            const f32 mn[3] = {-5.0f, -5.0f, -5.0f}, mx[3] = {1.0f, 1.0f, 1.0f};
            const VoxelBox b = voxelBoxFromWorldAabb(mn, mx, origin, volumeSize, res, 4);
            for (int i = 0; i < 3; ++i)
                check(b.lo[i] == 0, "padding into the volume's lower edge clamps lo to 0, axis " +
                                    std::to_string(i));
        }
        // Clamping at res: an AABB straddling the upper corner, padded, must not exceed res.
        {
            const f32 mn[3] = {95.0f, 95.0f, 95.0f}, mx[3] = {110.0f, 110.0f, 110.0f};
            const VoxelBox b = voxelBoxFromWorldAabb(mn, mx, origin, volumeSize, res, 8);
            for (int i = 0; i < 3; ++i)
                check(b.hi[i] == res, "padding past the volume's upper edge clamps hi to res, axis " +
                                      std::to_string(i));
        }
        // Wholly outside, below: even with generous padding, the axis (and so the box) is empty.
        {
            const f32 mn[3] = {-500.0f, 10.0f, 10.0f}, mx[3] = {-200.0f, 20.0f, 20.0f};
            const VoxelBox b = voxelBoxFromWorldAabb(mn, mx, origin, volumeSize, res, 16);
            check(b.empty(), "an AABB entirely below the volume on one axis gives an empty box, "
                              "even with padVoxels=16");
        }
        // Wholly outside, above.
        {
            const f32 mn[3] = {10.0f, 10.0f, 300.0f}, mx[3] = {20.0f, 20.0f, 400.0f};
            const VoxelBox b = voxelBoxFromWorldAabb(mn, mx, origin, volumeSize, res, 16);
            check(b.empty(), "an AABB entirely above the volume on one axis gives an empty box, "
                              "even with padVoxels=16");
        }
        // Non-finite input: NaN and +-infinity in the AABB, the origin, or a non-positive volumeSize
        // all fall back to the full grid.
        {
            const f32 nan = std::numeric_limits<f32>::quiet_NaN();
            const f32 inf = std::numeric_limits<f32>::infinity();
            const f32 mnGood[3] = {1.0f, 1.0f, 1.0f}, mxGood[3] = {2.0f, 2.0f, 2.0f};

            f32 mnNaN[3] = {1.0f, 1.0f, 1.0f}; mnNaN[1] = nan;
            check(boxEq(voxelBoxFromWorldAabb(mnNaN, mxGood, origin, volumeSize, res, 0),
                        fullVoxelBox(res)),
                  "a NaN in worldMin falls back to the full grid");

            f32 mxInf[3] = {2.0f, 2.0f, 2.0f}; mxInf[0] = inf;
            check(boxEq(voxelBoxFromWorldAabb(mnGood, mxInf, origin, volumeSize, res, 0),
                        fullVoxelBox(res)),
                  "an infinity in worldMax falls back to the full grid");

            f32 originNaN[3] = {0.0f, 0.0f, 0.0f}; originNaN[2] = nan;
            check(boxEq(voxelBoxFromWorldAabb(mnGood, mxGood, originNaN, volumeSize, res, 0),
                        fullVoxelBox(res)),
                  "a NaN in volumeOrigin falls back to the full grid");

            check(boxEq(voxelBoxFromWorldAabb(mnGood, mxGood, origin, 0.0f, res, 0), fullVoxelBox(res)),
                  "volumeSize == 0 falls back to the full grid");
            check(boxEq(voxelBoxFromWorldAabb(mnGood, mxGood, origin, -10.0f, res, 0), fullVoxelBox(res)),
                  "a negative volumeSize falls back to the full grid");
        }
    }

    // ---- 3. alignOutward: contains its input, and both bounds land where the spec says ----
    {
        std::mt19937 rng(777);
        std::uniform_int_distribution<u32> loDist(0, 500), spanDist(1, 50);
        const u32 res = 512;
        u32 checked = 0, failures = 0;
        for (int trial = 0; trial < 64; ++trial) {
            VoxelBox b;
            for (int i = 0; i < 3; ++i) {
                u32 lo = loDist(rng) % res;
                u32 span = spanDist(rng);
                b.lo[i] = lo;
                b.hi[i] = lo + span > res ? res : lo + span;
            }
            if (b.empty()) continue;
            const VoxelBox a = alignOutward(b, 4, res);
            ++checked;
            for (int i = 0; i < 3; ++i) {
                if (a.lo[i] > b.lo[i]) ++failures;              // must round DOWN, never past lo
                if (a.hi[i] < b.hi[i]) ++failures;              // must round UP, never short of hi
                if (a.lo[i] % 4 != 0) ++failures;
                if (a.hi[i] % 4 != 0 && a.hi[i] != res) ++failures;
            }
        }
        check(checked > 0 && failures == 0,
              "alignOutward(.,4,res) contains its input on every axis, lo is a multiple of 4, and hi "
              "is a multiple of 4 or exactly res -- " + std::to_string(failures) +
              " failing axis-checks over " + std::to_string(checked) + " random nonempty boxes");

        const VoxelBox emptyIn = makeBox(5, 5, 5, 5, 9, 9);   // degenerate on axis 0 only
        check(alignOutward(emptyIn, 4, res).empty(), "alignOutward of an empty box stays empty");
    }

    // ---- 4. mipBox: the footprint-intersection definition, and an independent double-ceil route ----
    {
        std::mt19937 rng(2026);
        const u32 resList[] = {128, 256, 512};
        for (u32 res : resList) {
            u32 maxMip = 0;
            for (u32 d = res; d > 1; d >>= 1) ++maxMip;

            std::uniform_int_distribution<u32> loDist(0, res - 1), spanDist(1, res);
            u32 footprintChecks = 0, footprintFail = 0;
            u32 refChecks = 0, refFail = 0;

            for (int trial = 0; trial < 40; ++trial) {
                VoxelBox b0;
                for (int i = 0; i < 3; ++i) {
                    u32 lo = loDist(rng);
                    u32 span = spanDist(rng);
                    b0.lo[i] = lo;
                    b0.hi[i] = lo + span > res ? res : lo + span;
                }
                if (b0.empty()) continue;

                for (u32 mip = 0; mip <= maxMip + 1; ++mip) {
                    const VoxelBox bm = mipBox(b0, mip, res);
                    const u32 dim = mipDim(res, mip);

                    for (int i = 0; i < 3; ++i) {
                        // Independent route: double-precision ceil for hi, plain integer division for
                        // lo (mathematically the same operation as the header's shift, but a different
                        // line of code, so a shift-amount typo in the header would not also be present
                        // here).
                        ++refChecks;
                        const u32 divisor = 1u << mip;
                        u32 refLo = b0.lo[i] / divisor;
                        const double hiF =
                            std::ceil(static_cast<double>(b0.hi[i]) / static_cast<double>(divisor));
                        u32 refHi = static_cast<u32>(hiF);
                        if (refLo > dim) refLo = dim;
                        if (refHi > dim) refHi = dim;
                        if (bm.lo[i] != refLo || bm.hi[i] != refHi) ++refFail;

                        if (bm.empty()) continue;
                        // Footprint-intersection definition: the lowest and highest voxels mipBox KEPT
                        // must actually intersect the mip-0 interval, and their immediate neighbours
                        // OUTSIDE the returned range must not.
                        ++footprintChecks;
                        bool ok = footprintIntersects(bm.lo[i], mip, b0.lo[i], b0.hi[i]) &&
                                  footprintIntersects(bm.hi[i] - 1, mip, b0.lo[i], b0.hi[i]);
                        if (bm.lo[i] > 0)
                            ok = ok && !footprintIntersects(bm.lo[i] - 1, mip, b0.lo[i], b0.hi[i]);
                        if (bm.hi[i] < dim)
                            ok = ok && !footprintIntersects(bm.hi[i], mip, b0.lo[i], b0.hi[i]);
                        if (!ok) ++footprintFail;
                    }
                }
            }
            check(refChecks > 0 && refFail == 0,
                  "res " + std::to_string(res) + ": mipBox matches an independent double-ceil "
                  "computation on every axis -- " + std::to_string(refFail) + " of " +
                  std::to_string(refChecks) + " axis-checks disagreed");
            check(footprintChecks > 0 && footprintFail == 0,
                  "res " + std::to_string(res) + ": mipBox's returned range is EXACTLY the mip-m "
                  "voxels whose 2^m footprint intersects the mip-0 box (boundary voxels checked both "
                  "sides) -- " + std::to_string(footprintFail) + " of " +
                  std::to_string(footprintChecks) + " axis-checks disagreed");
        }
    }

    // ---- 5. mipBox nesting identity: mipBox(b, m) == mipBox(mipBox(b, m-1), 1) ----
    {
        std::mt19937 rng(4040);
        const u32 resList[] = {128, 256, 512};
        for (u32 res : resList) {
            u32 maxMip = 0;
            for (u32 d = res; d > 1; d >>= 1) ++maxMip;
            std::uniform_int_distribution<u32> loDist(0, res - 1), spanDist(1, res);
            u32 pairs = 0, mismatches = 0;
            for (int trial = 0; trial < 40; ++trial) {
                VoxelBox b0;
                for (int i = 0; i < 3; ++i) {
                    u32 lo = loDist(rng);
                    u32 span = spanDist(rng);
                    b0.lo[i] = lo;
                    b0.hi[i] = lo + span > res ? res : lo + span;
                }
                if (b0.empty()) continue;
                for (u32 m = 1; m <= maxMip; ++m, ++pairs) {
                    const VoxelBox direct = mipBox(b0, m, res);
                    const VoxelBox nested = mipBox(mipBox(b0, m - 1, res), 1, res);
                    if (!boxEq(direct, nested)) ++mismatches;
                }
            }
            check(pairs > 0 && mismatches == 0,
                  "res " + std::to_string(res) + ": mipBox(b,m) == mipBox(mipBox(b,m-1),1) for lo and "
                  "hi over " + std::to_string(pairs) + " (box, mip) pairs, " +
                  std::to_string(mismatches) + " mismatch(es)");
        }
    }

    // ---- 6. dispatchGroups: ceiling behaviour and the empty case ----
    {
        const VoxelBox b = makeBox(3, 3, 3, 10, 11, 12);   // extents 7, 8, 9
        u32 groups[3];
        dispatchGroups(b, 4, groups);
        check(groups[0] == 2 && groups[1] == 2 && groups[2] == 3,
              "dispatchGroups: ceil(7/4)=2, ceil(8/4)=2, ceil(9/4)=3");
        for (int i = 0; i < 3; ++i) {
            const u32 extent = b.hi[i] - b.lo[i];
            check(groups[i] * 4 >= extent, "dispatchGroups*4 covers the full extent on axis " +
                                            std::to_string(i));
            check(groups[i] * 4 < extent + 4, "dispatchGroups is the CEILING, not an overshoot, "
                                               "on axis " + std::to_string(i));
        }

        u32 emptyGroups[3] = {9, 9, 9};
        dispatchGroups(VoxelBox{}, 4, emptyGroups);
        check(emptyGroups[0] == 0 && emptyGroups[1] == 0 && emptyGroups[2] == 0,
              "dispatchGroups of an empty box is (0,0,0)");
    }

    // ---- 7. dispatchConstants + dispatchGroups on fullVoxelBox match today's unbounded group counts
    //         (res+3)/4 at mip 0 and (mipDim+3)/4 at every other mip -- the numbers a bounded dispatch
    //         must reproduce exactly with bounded dispatch OFF (C-4/C-8: identical group counts). ----
    {
        const u32 res = 512;
        u32 maxMip = 0;
        for (u32 d = res; d > 1; d >>= 1) ++maxMip;
        u32 mismatches = 0, checked = 0;
        for (u32 mip = 0; mip <= maxMip; ++mip) {
            const u32 dim = mipDim(res, mip);
            const VoxelBox full = mipBox(fullVoxelBox(res), mip, res);
            check(full.lo[0] == 0 && full.hi[0] == dim && full.hi[1] == dim && full.hi[2] == dim,
                  "mip " + std::to_string(mip) + " of the full box is [0, " + std::to_string(dim) +
                  ") on every axis");

            const GiDispatchConstants c = dispatchConstants(full, mip);
            check(c.srcMip == mip && c.boxLo[0] == 0 && c.boxHi[0] == dim,
                  "dispatchConstants round-trips the box and mip for mip " + std::to_string(mip));

            u32 groups[3];
            dispatchGroups(full, 4, groups);
            const u32 want = (dim + 3) / 4;
            ++checked;
            if (groups[0] != want || groups[1] != want || groups[2] != want) ++mismatches;
        }
        check(checked > 0 && mismatches == 0,
              "dispatchGroups on the full box matches (dim+3)/4 -- today's unbounded group count -- "
              "at every mip from 0 to " + std::to_string(maxMip) + " (" + std::to_string(mismatches) +
              " mismatch(es))");
    }

    // ---- 8. GiDispatchConstants layout: the C++ side of the MipCB mirror ----
    {
        check(sizeof(GiDispatchConstants) == 32, "sizeof(GiDispatchConstants) == 32 bytes, matching "
                                                  "voxi.hlsl's cbuffer MipCB");
        check(offsetof(GiDispatchConstants, boxLo) == 4, "boxLo sits at byte offset 4 (after the "
                                                          "4-byte gSrcMip), matching gBoxLo@4");
        check(offsetof(GiDispatchConstants, boxHi) == 16, "boxHi sits at byte offset 16 (gBoxLo's own "
                                                           "16-byte slot ends at 16), matching gBoxHi@16");
        check(offsetof(GiDispatchConstants, srcMip) == 0, "srcMip sits at byte offset 0, matching "
                                                           "gSrcMip@0");
        check(offsetof(GiDispatchConstants, slabZ) == 28, "slabZ sits at byte offset 28, matching "
                                                           "gSlabZ@28");
    }

    // ---- 9. SOURCE ASSERTIONS: voxi.hlsl still declares the exact C-4 cbuffer and guard line ----
    {
        check(hlslHas("cbuffer MipCB : register(b3) { uint gSrcMip; uint3 gBoxLo; uint3 gBoxHi; "
                      "uint _boxPad; };"),
              "voxi.hlsl declares MipCB with exactly the C-4 field list and order");

        const char* guard = "uint3 v = id + gBoxLo; if (any(v >= gBoxHi)) return;";
        const std::string clearBody = hlslBody("void CSClear(uint3 id : SV_DispatchThreadID)");
        const std::string resolveBody = hlslBody("void CSResolve(uint3 id : SV_DispatchThreadID)");
        const std::string mipBodyText = hlslBody("void CSMip(uint3 id : SV_DispatchThreadID)");

        check(!clearBody.empty() && clearBody.find(guard) != std::string::npos,
              "CSClear begins with the bounded-dispatch guard line");
        check(!resolveBody.empty() && resolveBody.find(guard) != std::string::npos,
              "CSResolve begins with the bounded-dispatch guard line");
        check(!mipBodyText.empty() && mipBodyText.find(guard) != std::string::npos,
              "CSMip begins with the bounded-dispatch guard line");
    }

    if (g_failures == 0) {
        AVER_INFO("[GiDispatchBounds] PASS: union/pad/clamp/align/mip arithmetic checked against two "
                  "independent methods each, the nesting identity holds, dispatch group counts "
                  "reproduce today's unbounded numbers with bounded dispatch off, the C++ mirror is "
                  "32 bytes at the offsets voxi.hlsl expects, and voxi.hlsl still carries the exact "
                  "MipCB declaration and per-kernel guard line this header's callers depend on");
        return 0;
    }
    AVER_ERROR("[GiDispatchBounds] FAIL: {} check(s)", g_failures);
    return 1;
}
