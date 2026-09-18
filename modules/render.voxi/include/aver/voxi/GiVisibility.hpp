#pragma once
// GiVisibility.hpp -- U1's shared bit definitions and reconstruction arithmetic: the ReSTIR GI
// visibility modes (giRestirVisibility) F2 (candidate-hit sky) and F3 (reuse visibility) select
// between, and the half-resolution reconstruction Half runs on the three pixels of every 2x2 block
// that do not trace this frame. See optimisation-wave2-plan.md sections 2.9/2.10/2.12 for the full
// design; this header is section 2.12's own deliverable.
//
// WHY THIS EXISTS, RATHER THAN LEAVING THE BIT PACKING AND THE PHASE TABLE AS TWO SEPARATE, HAND-
// WRITTEN COPIES (one in VoxiRenderer.cpp, one in voxi_restir.hlsli): gAmbientParams.w is a single
// float-encoded u32 that BOTH sides read and write -- the C++ side packs it once per frame
// (VoxiRenderer::beginShadowHistory) and the shader decodes it at the top of every giRestirIndirect
// invocation (voxi_restir.hlsli's own decode block, 2.10 A). A bit meaning that drifted between the
// two would not fail to compile or even fail an obvious test -- it would silently read the wrong mode,
// exactly the class of bug aver-voxi-cbuffer-three-mirrors already warns about for the cbuffer layout
// itself. packAmbientW is therefore the ONE place either side computes this packing, called from
// VoxiRenderer.cpp directly and checked against voxi_restir.hlsli's own decode arithmetic by
// GiVisibilityTest -- not shared code (HLSL cannot include a C++ header), but one definition each side
// is tested against instead of two independently maintained ones.
//
// HEADER-ONLY, NO ALLOCATION, NO RHI -- the same minimalism aver/voxi/CameraFactor.hpp and
// aver/voxi/GiDispatchBounds.hpp already use (see either file's own top comment), for the identical
// reason: every function below is closed-form arithmetic, decidable on the CPU alone, and needs no
// device or shader compiler to exercise. aver/core/Types.hpp is the only dependency.
#include "aver/core/Types.hpp"

#include <cmath>

namespace aver::voxi::givis {

// ---- THE FIVE UNMEASURED TUNABLES, MIRRORED BYTE-FOR-BYTE AGAINST voxi_restir.hlsli's OWN
// AVER_GI_VIS_* #defines (declared beside kGiNeighborOffsetMask there) ----
// GiVisibilityTest reads voxi_restir.hlsli's source text and checks these five literals against the
// #define values verbatim, so a change to one side that forgets the other fails a test instead of
// silently drifting. None of these was fit against a ground truth -- see each #define's own comment,
// in voxi_restir.hlsli, for what it does and why it is reasoned rather than measured.
inline constexpr f32 kHistWeight  = 0.8f;    // AVER_GI_VIS_HIST_WEIGHT: the half-res EMA weight at rest
inline constexpr f32 kRhoMax      = 4.0f;    // AVER_GI_VIS_RHO_MAX: ceiling on F2's reconstructed sky ratio
inline constexpr f32 kNormalPow   = 8.0f;    // AVER_GI_VIS_NORMAL_POW: exponent on the normal-agreement term
inline constexpr f32 kPlaneTolRel = 0.02f;   // AVER_GI_VIS_PLANE_TOL_REL: plane-test tolerance, per unit depth
inline constexpr f32 kPlaneTolCm  = 1.0f;    // AVER_GI_VIS_PLANE_TOL_CM: plane-test tolerance, flat floor

// (d + 1) / 2 -- the half-resolution history pair's own edge length for a full-resolution edge `d`,
// matching VoxiRenderer::ensureShadowHistory's own sizing (2.11: "(w+1)/2 x (h+1)/2") and the memory
// table in 2.10 F (native 3532x1987 -> 1766x994 per channel). Integer division rounds DOWN, so the
// `+1` is what makes an odd edge round UP instead -- a 2x2 block with one full-resolution row or
// column left over still needs a texel to hold it, the same reason a mip chain's own halving always
// takes max(dim >> 1, 1) rather than a plain shift.
constexpr u32 halfDim(u32 d) { return (d + 1u) / 2u; }

// Does pixel (x, y) trace F2/F3 for real on frame `frame`, rather than reconstruct from the half-res
// history? SAME TABLE AS voxi_restir.hlsli's kGiVisPhase -- {(0,0), (1,1), (1,0), (0,1)} -- inlined
// here rather than shared (HLSL cannot include this header), and checked against the shader's own
// source text by GiVisibilityTest so the two tables cannot silently diverge. `frame & 3u`, not
// `frame % 4u`: 4 is a compile-time power of two, so the two are identical, and the mask form matches
// this codebase's own standing convention at every other cyclic-schedule site (kGiNeighborOffsetMask,
// tileMask/turnMask in voxi_rt.hlsli).
constexpr bool tracedPixel(u32 x, u32 y, u32 frame) {
    // A plain lookup table indexed by frame & 3u, spelled out as two 4-entry arrays rather than one
    // array of a 2-field struct: a constexpr function returning through an out-of-line struct array
    // is more machinery than four u32 comparisons need.
    const u32 idx = frame & 3u;
    const u32 phaseX = (idx == 0u) ? 0u : (idx == 1u) ? 1u : (idx == 2u) ? 1u : 0u;
    const u32 phaseY = (idx == 0u) ? 0u : (idx == 1u) ? 1u : (idx == 2u) ? 0u : 1u;
    return (x & 1u) == phaseX && (y & 1u) == phaseY;
}

// ---- gAmbientParams.w's BIT PACKING (2.9's own table, voxi.hlsl's gAmbientParams comment) ----
// bits 0-1 `mode` (giRestirVisibility, 0..3, caller's responsibility to have already clamped --
//          this function does not re-clamp, so a caller passing an out-of-range mode gets whatever
//          the low two bits of it happen to be, matching the shader's own `& 3u` decode exactly);
// bit  4   `histBound`   -- the half-res visibility pair (t16/u10) is bound this frame;
// bit  8   `histValid`   -- t16 holds a real previous frame, not just-created/resized storage;
// bit 16   `blendedCone` -- W6/M5: blended-replay fragments take the cone gather instead of ReSTIR;
// bit 32   `blendedReplay` -- the backend replayed translucent draws blended this frame (D3D12 only);
// bit 64   `pathView`    -- voxi.giVisPathView, the F2-path debug view (2.10 I);
// bits 7-11 `movingAge`  -- Settings::giRestirMovingAge (0..31, caller's responsibility to have
//          already clamped, same contract as `mode` above) -- the moving-camera ReSTIR reservoir-age
//          cap that replaces stparams.maxReservoirAge while the camera is in motion, 0 meaning
//          "legacy, always 30" (see that field's own comment for the fade this exists to remove).
//          FIVE bits, not the two 3dbc9a42's reverted giRestirReuse briefly claimed at this same
//          offset (8daed7f1 freed them again): giRestirReuse pinned a boolean-ish 0..2 selector,
//          this carries a frame COUNT up to 31, so it needs the room.
// bits 12-15 `spatialSamples` -- Settings::giRestirSpatialSamples (0..15, caller's responsibility to
//          have already clamped, same contract as `movingAge` just below it): overrides
//          RTXDI_GISpatioTemporalResamplingParameters.numSamples in place of whatever the moving-
//          camera motion discount would otherwise compute, splitting spatial reuse from temporal
//          reuse for the fade bisection that field's own comment (Voxi.hpp) is written against. 15
//          means AUTO -- leave the discount's own count alone, byte-identical to today's image; 0
//          disables spatial reuse outright (temporal only); 1..8 pin the count. FOUR bits, directly
//          above movingAge's own five, because 15 needs a sentinel distinct from every real count
//          0..8 fits comfortably inside one nibble.
// Eight arguments, not a bitmask the caller assembles by hand: every call site (VoxiRenderer.cpp's
// beginShadowHistory, both the unconditional write before the shadow-history early return and the
// recomputed write inside the giSurf block, 2.11) states its eight inputs by name, so a reordered bit
// in this function is the only place that has to change, not every caller.
constexpr u32 packAmbientW(u32 mode, bool histBound, bool histValid, bool blendedCone,
                            bool blendedReplay, bool pathView, u32 movingAge, u32 spatialSamples) {
    u32 w = mode & 3u;
    if (histBound)     w |= 4u;
    if (histValid)      w |= 8u;
    if (blendedCone)    w |= 16u;
    if (blendedReplay)  w |= 32u;
    if (pathView)       w |= 64u;
    w |= (movingAge & 31u) << 7;
    w |= (spatialSamples & 15u) << 12;
    return w;
}

// Mirrors voxi_restir.hlsli's giVisReconstruct (2.10 D) one weight-computation at a time: given one
// tap's bilinear weight, the (already-saturated-to-[0,1]) dot of its normal against the receiver's,
// the signed plane distance |dot(neighbourWorldPos - wpos, N)|, and the neighbour's own linear depth
// (for the tolerance's relative term), returns the tap's final weight -- 0.0 whenever the shader's own
// tap would have `continue`d past it (a non-positive bilinear corner, a normal that disagrees outright,
// or a plane distance past tolerance), so a caller can sum this directly the way the shader sums `w`
// into wsum/rsum/gsum/bsum.
//
// nDot IS TAKEN PRE-SATURATED, matching the shader's own `saturate(dot(ps.normal, N))` computed
// before the `pow` call -- this function does not re-saturate it, so a caller passing a raw,
// unclamped dot product (which can be negative) gets exactly the "opposite normals reject" behaviour
// the shader's own saturate already produces: pow() of a value <= 0 at a non-integer exponent is
// this function's own explicit floor at 0.0 below, not undefined-behaviour territory.
inline f32 reconstructWeight(f32 bilinear, f32 nDot, f32 planeDist, f32 viewDepth) {
    if (!(bilinear > 0.0f)) return 0.0f;   // `!(x > 0)` catches NaN too, matching this codebase's own
                                            // documented "comparison against NaN is false" idiom
    const f32 tol = kPlaneTolCm + kPlaneTolRel * viewDepth;
    if (std::fabs(planeDist) > tol) return 0.0f;
    if (!(nDot > 0.0f)) return 0.0f;       // opposite or grazing-to-negative normal: no weight at all
    return bilinear * std::pow(nDot, kNormalPow);
}

} // namespace aver::voxi::givis
