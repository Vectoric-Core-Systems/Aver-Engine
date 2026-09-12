// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
//
// The PURE half of HZB occlusion culling: no RHI, no texture, no device -- three free functions that
// project a box, pick a pyramid level, and combine a handful of sampled depths into one yes/no. This
// is what makes the algorithm checkable at all: OcclusionCuller.cpp's HLSL runs the SAME three steps
// per candidate every frame, on the GPU, against a real pyramid this header has never heard of, and
// nothing here can watch that shader run. What CAN be watched is whether these three steps, alone,
// answer the geometry question they claim to -- and tests/occlusion/src/OcclusionMathTest.cpp does
// exactly that, against small synthetic pyramids built by hand so the right answer is known
// independently of any renderer. Keep this file free of aver/rhi includes; that freedom is the point,
// the same way modules/landscape's CPU half stays free of one (see that module's own README).
#pragma once
#include "aver/core/Types.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace aver::occlusion {

// ---------------------------------------------------------------------------------------------
// 1. Project a world-space AABB through the camera.
// ---------------------------------------------------------------------------------------------

// Row-major, row-vector `viewProj` (ENGINE convention throughout this codebase -- see RHI.hpp's
// IDevice::setCamera and SandboxApp.cpp's frustum-plane extraction, which reads the identical
// `m.m[row][col]` layout this function flattens into `viewProj[row*4+col]`): a point transforms as
// `clip[c] = sum_r pos[r] * viewProj[r*4+c]` with pos treated as (x, y, z, 1).
//
// Projects the box's 8 corners and returns:
//  - outMinUV/outMaxUV: the screen-space bounds in TEXTURE UV (0..1, V DOWN -- matching every render
//    target this engine samples, not clip-space Y UP), clamped to the visible rect.
//  - outNearestZ: the MINIMUM device-space depth (D3D convention: near = 0, far = 1 -- see
//    D3D12Device's depth-buffer clear value of 1.0f, which is only correct for that convention) among
//    the 8 corners, i.e. the closest the box can possibly be to the camera. This is the value
//    conservativelyHidden compares against the pyramid: if even the box's CLOSEST point is farther
//    than everything already drawn across its whole footprint, nothing about the box is visible.
//
// Returns false when every corner is behind the near plane (w <= 0 for all eight) or the projected
// rect does not overlap [0,1]x[0,1] at all. EITHER case means occlusion has nothing useful to say —
// not that the box is hidden, just that this function could not bound it — and the caller (see
// conservativelyHidden's own "FALSE IS ALWAYS SAFE" note) must fall back to drawing it.
inline bool projectAabbScreenBounds(const f32 viewProj[16], const f32 worldMin[3], const f32 worldMax[3],
                                     f32 outMinUV[2], f32 outMaxUV[2], f32& outNearestZ) {
    f32 minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f, nearestZ = 1e30f;
    bool anyInFront = false;
    for (u32 c = 0; c < 8; ++c) {
        const f32 pos[3] = {
            (c & 1) ? worldMax[0] : worldMin[0],
            (c & 2) ? worldMax[1] : worldMin[1],
            (c & 4) ? worldMax[2] : worldMin[2],
        };
        f32 clip[4] = {0, 0, 0, 0};
        for (u32 col = 0; col < 4; ++col) {
            clip[col] = pos[0] * viewProj[0 * 4 + col] + pos[1] * viewProj[1 * 4 + col] +
                        pos[2] * viewProj[2 * 4 + col] + viewProj[3 * 4 + col];
        }
        if (clip[3] <= 1e-5f) continue;   // behind (or at) the eye: this corner projects nowhere sane
        anyInFront = true;
        const f32 invW = 1.0f / clip[3];
        const f32 ndcX = clip[0] * invW, ndcY = clip[1] * invW, ndcZ = clip[2] * invW;
        const f32 u = ndcX * 0.5f + 0.5f;
        const f32 v = 0.5f - ndcY * 0.5f;   // clip Y is UP; texture V is DOWN
        minX = std::min(minX, u); maxX = std::max(maxX, u);
        minY = std::min(minY, v); maxY = std::max(maxY, v);
        nearestZ = std::min(nearestZ, ndcZ);
    }
    if (!anyInFront) return false;
    minX = std::max(0.0f, minX); minY = std::max(0.0f, minY);
    maxX = std::min(1.0f, maxX); maxY = std::min(1.0f, maxY);
    if (maxX <= minX || maxY <= minY) return false;
    outMinUV[0] = minX; outMinUV[1] = minY;
    outMaxUV[0] = maxX; outMaxUV[1] = maxY;
    outNearestZ = std::max(0.0f, nearestZ);
    return true;
}

// ---------------------------------------------------------------------------------------------
// 2. Pick the coarsest pyramid mip that still resolves the box's footprint to no more than a 2x2
//    texel neighbourhood -- coarser than that and a single 2x2 sample could straddle geometry the
//    box's own footprint never touches, UNDERSHOOTING nothing (a smaller mip is always safe, just
//    more expensive) but never OVERSHOOTING either, which is the property this function exists to
//    give a caller who wants the cheapest mip that is still honest.
// ---------------------------------------------------------------------------------------------
// SAFETY INVARIANT this function must uphold for conservativelyHidden's 4-CORNER sample to be
// honest: the footprint, measured in texels of the MIP IT RETURNS, must never exceed 1.0. Only an
// interval of AT MOST 1 texel can touch at most 2 texel indices (an interval strictly under 1 texel
// crosses at most one boundary; exactly 1.0, aligned corner-to-corner, still lands on exactly 2), so
// the min-corner and max-corner samples cover everything the footprint could touch. Anything WIDER
// than 1 texel can straddle 3+ indices while both corners still land on the two occluded outer ones,
// leaving a middle texel (which might be open sky) untested -- see tests/occlusion/src/
// OcclusionMathTest.cpp's FalseCull_ThreeTexelGapNotCulled for the exact counterexample this bound
// closes (a 1.992-texel-wide footprint over occluded/gap/occluded was being read back as fully
// hidden, corners landing on the two occluded outer texels, the visible gap between them never
// sampled). Earlier revisions returned mip 0 for anything up to 2.0 texels wide -- a 2x2 neighbourhood
// bound that sounds right for a 4-corner sample but is not: 2x2 CORNERS cover a 2x2-texel
// neighbourhood only when the footprint is texel-aligned, and a footprint is not guaranteed to be.
inline u32 selectConservativeMip(f32 uvWidth, f32 uvHeight, u32 pyramidW, u32 pyramidH, u32 maxMip) {
    const f32 texelW = uvWidth * static_cast<f32>(pyramidW);
    const f32 texelH = uvHeight * static_cast<f32>(pyramidH);
    const f32 largest = std::max(texelW, texelH);
    if (largest <= 1.0f) return 0;
    // No `* 0.5f` here (unlike the pre-fix formula): halving `largest` before taking log2 is what
    // let a >1-texel footprint alias back down to mip 0. Each mip step halves texel density, so the
    // footprint measured AT mip `m` is `largest / 2^m`; solving `largest / 2^m <= 1.0` for the
    // smallest such `m` is exactly `m = ceil(log2(largest))`, which is what this now computes.
    const f32 mipF = std::ceil(std::log2(largest));
    const u32 mip = mipF <= 0.0f ? 0u : static_cast<u32>(mipF);
    return mip < maxMip ? mip : maxMip;
}

// ---------------------------------------------------------------------------------------------
// 3. The conservative decision.
// ---------------------------------------------------------------------------------------------

// One pyramid texel lookup: (mip, u, v) in the same UV space projectAabbScreenBounds hands back ->
// the device-space z stored there (the MAX/furthest reduction OcclusionCuller.cpp's mip-build pass
// keeps at every level -- see that file's own comment for why max, not min, is the conservative
// choice going INTO a mip, mirrored here by this function's use of it coming back OUT).
using PyramidSample = std::function<f32(u32 mip, f32 u, f32 v)>;

// True only when `nearestZ` (the box's closest possible point) is farther from the camera than EVERY
// one of the up-to-4 texels covering [minUV, maxUV] at the mip selectConservativeMip picked. Samples
// the 4 CORNERS of the rect rather than its centre: a box whose footprint straddles a texel boundary
// must not read as hidden just because the one texel nearest its centre happened to be far, when a
// neighbour one texel over is close and the box's own edge overlaps it.
//
// FALSE IS ALWAYS SAFE — a box this says "false" for might still be genuinely invisible; the caller
// draws it anyway, at the cost of an unnecessary draw call and nothing else. TRUE is a PROMISE: every
// texel sampled stores a depth that is itself the max (furthest) of everything actually rasterised
// under its footprint (OcclusionCuller.cpp's reduction pass), so `nearestZ > sampled` for all four
// means the box's closest point is behind the closest thing drawn ANYWHERE in that footprint — not
// just behind the average, not just behind what one sub-cell saw. That is what makes the coarsening
// step "conservative" mean something stronger than "approximately correct": it is a one-directional
// error bound, and this function is where the bound gets spent.
inline bool conservativelyHidden(const f32 minUV[2], const f32 maxUV[2], f32 nearestZ,
                                  u32 pyramidW, u32 pyramidH, u32 mipCount, const PyramidSample& sample4) {
    if (mipCount == 0) return false;
    const u32 maxMip = mipCount - 1;
    const u32 mip = selectConservativeMip(maxUV[0] - minUV[0], maxUV[1] - minUV[1], pyramidW, pyramidH, maxMip);
    const f32 corners[4][2] = {
        {minUV[0], minUV[1]}, {maxUV[0], minUV[1]}, {minUV[0], maxUV[1]}, {maxUV[0], maxUV[1]},
    };
    for (const auto& uv : corners) {
        const f32 stored = sample4(mip, uv[0], uv[1]);
        if (nearestZ <= stored) return false;   // this corner's texel: something at least as close is there
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// 4. A content fingerprint for "is this the SAME batch of boxes as last call", the piece a caller
//    needs to make testBatch()'s one-call-stale answer (OcclusionCuller.cpp) actually SAFE to use.
// ---------------------------------------------------------------------------------------------
//
// THE GAP THIS CLOSES. testBatch()'s own generation stamp (OcclusionCuller.cpp) proves the bytes it
// hands back were produced by EXACTLY the immediately preceding call's GPU dispatch -- a TIMING
// fact. It proves nothing about whether that previous call was even asking about the SAME boxes:
// this module has no entity/chunk/cluster concept (see this header's own top comment on why), so a
// caller's box[i] this call and box[i] last call are matched by BARE ARRAY INDEX alone. Every caller
// today (SandboxApp.cpp) rebuilds that array fresh every frame from whichever entities currently
// pass its own visibility/streaming gates, and the instant that population's SIZE OR CONTENT changes
// between two consecutive calls -- an entity streamed in or out, one crossing from degenerate to
// valid bounds -- index i no longer names the same logical box it did one call ago. Applying the
// stale answer anyway hands a box a verdict some OTHER box earned, indistinguishable from a real
// cull with no GPU-timing anomaly to explain it: exactly the "silent wrong answer no screenshot diff
// would necessarily catch" the generation stamp was built to catch a different version of.
//
// MEASURED, not just reasoned: instrumenting OcclusionCuller.cpp's testBatch() with this exact
// fingerprint against a live Sponza run (--project PTTest --open-level NewSponza_Main_glTF_003,
// occlusion on, static camera) found it disagree with the previous call's fingerprint on exactly 2
// of the run's first several calls -- while the box population was still arriving from level load --
// and never again once the population settled at 112 boxes per call; culling continued normally
// after (1.2% then climbing to 1.8%, unchanged from a run with no fingerprint check at all). So the
// mechanism this catches is real and fires in practice, though a single before/after frame showing
// the specific object it would have mis-culled was not isolated -- the two disagreeing calls landed
// during level load, before there was a stable image to screenshot-diff.
//
// A bare count compare would already catch the common case above (streaming adds/removes an
// entity), but hashing the packed bytes also catches the rarer same-count swap -- one entity's box
// leaves an index the same call another's arrives there -- that a count alone would miss.
//
// `packed` is the caller's OWN packed layout (8 floats/box: min.xyz+pad, max.xyz+pad, matching
// OcclusionCuller.cpp's own upload buffer), so this reads exactly the bytes already being uploaded
// with no second copy or second format to keep in sync. `count` is folded in explicitly rather than
// left implicit in packed's length: two different counts happening to hash their (different-length)
// byte runs to the same value is astronomically unlikely already, but the fold costs one line and
// removes even that dependence on FNV's own distribution.
inline u64 hashPackedBoxes(const f32* packed, usize floatCount, u32 count) {
    u64 h = 1469598103934665603ull;   // FNV-1a 64-bit offset basis
    for (usize i = 0; i < floatCount; ++i) {
        u32 bits;
        std::memcpy(&bits, &packed[i], sizeof(bits));
        h ^= bits;
        h *= 1099511628211ull;   // FNV-1a 64-bit prime
    }
    h ^= count;
    h *= 1099511628211ull;
    return h;
}

} // namespace aver::occlusion
