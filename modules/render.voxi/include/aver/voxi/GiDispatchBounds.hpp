#pragma once
// GiDispatchBounds.hpp -- W3's occupied-bounds arithmetic: the voxel-space box a GI rebuild's
// CSClear/CSResolve/CSMip passes are confined to when bounded dispatch (voxi.giBoundedDispatch) is
// on, and the CPU-side mirror of voxi.hlsl's MipCB cbuffer (register b3) that carries one dispatch's
// box down to the shader.
//
// HEADER-ONLY, NO ALLOCATION, NO RHI. Every function below is closed-form integer/float arithmetic on
// three axes at a time -- cheap enough to call once per rebuild per mip level -- and needs no device,
// no shader compiler and no test double to exercise. aver/core/Types.hpp is the only dependency, the
// same minimalism aver/voxi/CameraFactor.hpp already uses (see its own top comment and
// tests/render.voxi/src/CameraFactorTest.cpp's), which is what lets GiDispatchBoundsTest link nothing
// but Aver.Core.
//
// WHO OWNS THE BOX. VoxiRenderer (a sibling file this header does not touch) accumulates the union of
// every draw a rebuild injects with voxelBoxFromWorldAabb + unionBox, pads the mip-0 result outward
// with alignOutward, and derives each coarser mip's box with mipBox before that level's
// CSClear/CSResolve/CSMip dispatch. This header supplies the arithmetic only -- deciding WHEN to fall
// back to the full grid (first build, resolution change, a moved/resized volume, an unbounded draw,
// or a GI cache restore -- see the W3 spec) reads VoxiRenderer's own state and belongs there, not here.
//
// THE SHADER SIDE. voxi.hlsl's CSClear/CSResolve/CSMip each begin with
//     uint3 v = id + gBoxLo; if (any(v >= gBoxHi)) return;
// against the MipCB cbuffer this header's GiDispatchConstants mirrors byte-for-byte. See that
// declaration's own comment in voxi.hlsl for the field packing.
#include "aver/core/Types.hpp"

#include <cmath>

namespace aver::voxi {

// A half-open voxel-space box, [lo, hi) independently on every axis. empty() is true the moment any
// ONE axis is degenerate (lo >= hi there) -- there is no single canonical "the empty box" bit pattern
// every function must agree to produce, because every function below already treats any degenerate
// axis as "no voxels on this axis, so none in the box" and reads correctly regardless of what the
// other two axes happen to hold.
struct VoxelBox {
    u32 lo[3] = {0, 0, 0};
    u32 hi[3] = {0, 0, 0};
    bool empty() const { return lo[0] >= hi[0] || lo[1] >= hi[1] || lo[2] >= hi[2]; }
};

// The MipCB payload's dword count (voxi.hlsl's cbuffer MipCB, register b3) -- named once rather than
// repeated as a bare 8 at every call site that sizes or offsets a constants upload.
inline constexpr u32 kGiDispatchConstantDwords = 8;

// Byte-for-byte mirror of voxi.hlsl's `cbuffer MipCB : register(b3) { uint gSrcMip; uint3 gBoxLo;
// uint3 gBoxHi; uint _boxPad; };`. HLSL cbuffer packing puts gSrcMip at byte 0 (4 B) and gBoxLo
// immediately after it at byte 4 -- a uint3 needs 12 B and HLSL only pushes a field to the NEXT
// 16-byte slot when it would otherwise straddle one, and 4..15 does not cross the 16 B boundary at 16,
// so gBoxLo fits in what is left of gSrcMip's own slot. gBoxHi then starts a fresh 16-byte slot at
// byte 16, and _boxPad fills its last 4 B. No slot is left partially empty, so this struct needs no
// compiler-inserted padding of its own to reach the same 32 B layout: every member here is 4-byte
// aligned and the total is already a multiple of 4. static_assert below pins that rather than trusting
// the paragraph. gSrcMip is CSMip's source-level index; CSClear/CSResolve never read it and leave it
// default-constructed.
struct GiDispatchConstants {
    u32 srcMip = 0;
    u32 boxLo[3] = {0, 0, 0};
    u32 boxHi[3] = {0, 0, 0};
    u32 pad = 0;
};
static_assert(sizeof(GiDispatchConstants) == kGiDispatchConstantDwords * sizeof(u32));

// max(res >> mip, 1) -- the edge length of mip level `mip` of a `res`-wide cube. Every tier's voxel
// resolution is a power of two (QualityLadder.hpp), so this halves exactly at every level down to 1
// and then stays 1 (mip filtering never produces a 0-wide texture).
inline u32 mipDim(u32 res, u32 mip) {
    // A shift of 32 or more is undefined behaviour in C++, not merely "shifts in zero" the way x86
    // hardware happens to behave. Nothing legitimate ever asks for mip 32 -- the largest grid is
    // 512^3, nine mip levels -- but the clamp is free and turns a caller's bug into a wrong-but-
    // defined answer instead of UB.
    const u32 shift = mip > 31u ? 31u : mip;
    const u32 d = res >> shift;
    return d == 0 ? 1u : d;
}

// [0, res) on every axis -- the box a full, unbounded dispatch covers today. Also what an occupied
// box's mip is compared against below: this is the identity every full-grid fallback in VoxiRenderer
// (first build, a moved volume, an unbounded draw, a cache restore) hands to bounded dispatch's own
// machinery, so a "full grid" tick still uses the SAME clear/resolve/mip guard, just with a box that
// never rejects anything mip 0's dispatch already covers.
inline VoxelBox fullVoxelBox(u32 res) {
    VoxelBox b;
    b.hi[0] = b.hi[1] = b.hi[2] = res;
    return b;
}

// The voxel-space box one draw's world-space AABB touches, mip 0, padded outward by `padVoxels` and
// clamped to the volume.
//
// voxel = floor((w - origin) / volumeSize * res) on each bound -- the SAME transform PSVoxel injects
// through: voxelUVW (voxi_cone.hlsli:56) computes (wp - gVoxelOrigin.xyz) * gVoxelOrigin.w (i.e.
// (w - origin) / volumeSize, since gVoxelOrigin.w is 1/volumeSize), and PSVoxel then does
// uint3(uvw * gVoxelParams.x) (voxi.hlsl:2333) -- a truncating cast that agrees with floor() for every
// non-negative uvw a fragment inside or straddling the volume can produce. floor() is used explicitly
// here rather than a cast so a bound OUTSIDE the volume (this is an AABB corner, not a clamped
// fragment) rounds a defined direction instead of toward zero the way a negative-to-uint cast would.
//
// ANY non-finite input (a NaN bound from a degenerate mesh, an AABB nobody actually set) or a
// non-positive volumeSize returns fullVoxelBox(res): the honest answer to "where does this draw touch
// the grid" is then "cannot tell", which gets exactly the reply an unbounded draw already gets in the
// W3 spec -- the rebuild covers everything, so it plainly cannot miss this draw.
inline VoxelBox voxelBoxFromWorldAabb(const f32 worldMin[3], const f32 worldMax[3],
                                       const f32 volumeOrigin[3], f32 volumeSize, u32 res,
                                       u32 padVoxels) {
    bool finite = volumeSize > 0.0f && std::isfinite(volumeSize);
    for (int i = 0; i < 3 && finite; ++i) {
        finite = std::isfinite(worldMin[i]) && std::isfinite(worldMax[i]) &&
                 std::isfinite(volumeOrigin[i]);
    }
    if (!finite) return fullVoxelBox(res);

    const f32 scale = static_cast<f32>(res) / volumeSize;
    const f32 resF = static_cast<f32>(res);
    VoxelBox b;
    for (int i = 0; i < 3; ++i) {
        const f32 lo0 = std::floor((worldMin[i] - volumeOrigin[i]) * scale);
        const f32 hi0 = std::floor((worldMax[i] - volumeOrigin[i]) * scale) + 1.0f;
        if (hi0 <= 0.0f || lo0 >= resF) {
            // Wholly outside the volume on THIS axis -- not a rounding artefact for the pad to paper
            // over, so this axis comes back empty no matter how large padVoxels is, which makes the
            // whole box empty via VoxelBox::empty()'s per-axis check.
            b.lo[i] = b.hi[i] = 0;
            continue;
        }
        // Clamp the RAW (unpadded) box to the volume first: padding means "2 voxels of conservative-
        // raster slop around a box that really is there", not licence to invent coverage at the edge
        // of the grid the AABB never reached.
        const f32 loClamped = lo0 < 0.0f ? 0.0f : lo0;
        const f32 hiClamped = hi0 > resF ? resF : hi0;
        const f32 loPadded = loClamped - static_cast<f32>(padVoxels);
        const f32 hiPadded = hiClamped + static_cast<f32>(padVoxels);
        b.lo[i] = static_cast<u32>(loPadded < 0.0f ? 0.0f : loPadded);
        b.hi[i] = static_cast<u32>(hiPadded > resF ? resF : hiPadded);
    }
    return b;
}

// Union of two boxes. An EMPTY operand is the identity -- returns the other box completely unchanged
// -- rather than forcing every accumulation loop that starts from "nothing unioned yet" to special-
// case its first iteration.
inline VoxelBox unionBox(const VoxelBox& a, const VoxelBox& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    VoxelBox r;
    for (int i = 0; i < 3; ++i) {
        r.lo[i] = a.lo[i] < b.lo[i] ? a.lo[i] : b.lo[i];
        r.hi[i] = a.hi[i] > b.hi[i] ? a.hi[i] : b.hi[i];
    }
    return r;
}

// Rounds lo DOWN and hi UP to the nearest multiple of `multiple`, hi additionally clamped to `res` so
// an aligned box never claims voxels the grid does not have. An empty box stays empty: rounding a
// degenerate [5,5) outward would otherwise manufacture a nonempty box out of nothing.
//
// alignOutward(box, 4, res) is what makes CSClear and CSResolve -- both [numthreads(4,4,4)], both
// bounded by the SAME gBoxLo/gBoxHi -- cover EXACTLY the same voxel set at mip 0. Without alignment, a
// lo or hi that is not a multiple of 4 would still pass through unchanged (the per-thread guard does
// not need alignment to be correct on its own), but the two passes' *dispatched group rectangles*
// would then only approximately agree with the box, and a boundary voxel could end up resolved without
// ever having been cleared, or vice versa, purely from the two kernels rounding their own group counts
// independently. Aligning the BOX itself once, before either dispatch, removes that seam.
inline VoxelBox alignOutward(const VoxelBox& box, u32 multiple, u32 res) {
    if (box.empty() || multiple == 0) return box;
    VoxelBox r;
    for (int i = 0; i < 3; ++i) {
        r.lo[i] = (box.lo[i] / multiple) * multiple;
        const u32 hiAligned = ((box.hi[i] + multiple - 1) / multiple) * multiple;
        r.hi[i] = hiAligned > res ? res : hiAligned;
    }
    return r;
}

// Projects a mip-0 box onto mip level `mip`: lo >> mip, ceil(hi / 2^mip), each clamped to
// mipDim(res,mip). An empty box stays empty (returned unchanged; its exact degenerate lo/hi carry no
// meaning once empty() is true, so there is nothing to recompute).
//
// WHY OUTWARD, AND WHY THIS IS EXACT rather than merely conservative: CSMip's box filter reads a
// 2x2x2 block of the source level for every destination texel (voxi.hlsl's CSMip, the a += ... loop
// over x,y,z in {0,1}). A mip-0 voxel at index h-1 -- the last one INSIDE a half-open box ending at h
// -- is read by exactly the destination texel (h-1)>>mip, and floor/ceil rounding of a half-open range
// under a power-of-two division is exactly the "smallest destination range whose footprints cover the
// source range" -- neither wider (an off-by-one bigger box would ask CSMip to filter texels whose
// entire 2x2x2 footprint sits outside the source box, reading resolved-but-uninteresting mip-0 data
// into the mip chain for no reason) nor narrower (that would drop real source texels from ever being
// filtered up). So the mip box is not a safe overestimate the way the mip-0 pad is -- it is the
// SMALLEST box with no gap, which is why alignOutward is applied only once, before mip 0, and never
// again per mip level: rounding outward twice would just be redundant here.
inline VoxelBox mipBox(const VoxelBox& mip0, u32 mip, u32 res) {
    if (mip0.empty()) return mip0;
    const u32 dim = mipDim(res, mip);
    const u32 shift = mip > 31u ? 31u : mip;   // see mipDim's own comment on shifts >= 32
    const u64 roundUp = (static_cast<u64>(1) << shift) - 1;   // 2^mip - 1, computed in u64 to stay
                                                                // defined even at the clamp above
    VoxelBox r;
    for (int i = 0; i < 3; ++i) {
        u32 lo = mip0.lo[i] >> shift;
        u64 hi64 = (static_cast<u64>(mip0.hi[i]) + roundUp) >> shift;   // ceil(hi / 2^mip)
        u32 hi = hi64 > dim ? dim : static_cast<u32>(hi64);
        if (lo > dim) lo = dim;
        r.lo[i] = lo;
        r.hi[i] = hi;
    }
    return r;
}

// Dispatch group counts for a box under a `threadsPerAxis`-wide compute group (4 for every kernel
// here, [numthreads(4,4,4)]): ceil(extent / threadsPerAxis) per axis. All three are 0 for an empty box
// or a zero thread count -- a dispatch this asks for should simply not run, rather than run with a
// group count of 0 on some axes and something nonzero on others.
inline void dispatchGroups(const VoxelBox& b, u32 threadsPerAxis, u32 out[3]) {
    if (b.empty() || threadsPerAxis == 0) {
        out[0] = out[1] = out[2] = 0;
        return;
    }
    for (int i = 0; i < 3; ++i) {
        const u32 extent = b.hi[i] - b.lo[i];
        out[i] = (extent + threadsPerAxis - 1) / threadsPerAxis;
    }
}

// Packs a box plus a source mip index into the exact bytes GiDispatchConstants (and therefore
// voxi.hlsl's MipCB) expects.
inline GiDispatchConstants dispatchConstants(const VoxelBox& b, u32 srcMip) {
    GiDispatchConstants c;
    c.srcMip = srcMip;
    for (int i = 0; i < 3; ++i) {
        c.boxLo[i] = b.lo[i];
        c.boxHi[i] = b.hi[i];
    }
    return c;
}

// The number of voxels a box covers; 0 for an empty box (checked explicitly rather than relying on a
// subtraction underflowing into something enormous when lo > hi on some axis).
inline u64 voxelCount(const VoxelBox& b) {
    if (b.empty()) return 0;
    return static_cast<u64>(b.hi[0] - b.lo[0]) * static_cast<u64>(b.hi[1] - b.lo[1]) *
           static_cast<u64>(b.hi[2] - b.lo[2]);
}

// voxelCount(b) / res^3 -- what the "injected-draw box N% of the grid" log line (C-7) reports. 0 for
// res 0 rather than a division by zero, since a zero-resolution grid has no meaningful fraction.
inline f64 gridFraction(const VoxelBox& b, u32 res) {
    if (res == 0) return 0.0;
    const f64 total = static_cast<f64>(res) * static_cast<f64>(res) * static_cast<f64>(res);
    return static_cast<f64>(voxelCount(b)) / total;
}

} // namespace aver::voxi
