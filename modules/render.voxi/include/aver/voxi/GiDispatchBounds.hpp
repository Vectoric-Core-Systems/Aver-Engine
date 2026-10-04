#pragma once
// GiDispatchBounds.hpp -- voxel-space box arithmetic for bounded GI dispatch. Mirrors voxi.hlsl's MipCB cbuffer.
// Header-only, no allocation, no RHI. Closed-form integer/float arithmetic on three axes; no device/compiler/test double needed.
#include "aver/core/Types.hpp"

#include <cmath>

namespace aver::voxi {

// Half-open voxel-space box [lo, hi); empty() true when any axis is degenerate (lo >= hi).
struct VoxelBox {
    u32 lo[3] = {0, 0, 0};
    u32 hi[3] = {0, 0, 0};
    bool empty() const { return lo[0] >= hi[0] || lo[1] >= hi[1] || lo[2] >= hi[2]; }
};

// MipCB dword count (voxi.hlsl cbuffer MipCB, register b3).
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

// Edge length of mip level mip of a res-wide cube; max(res >> mip, 1).
inline u32 mipDim(u32 res, u32 mip) {
    // Shift >= 32 is UB; clamp to defined behaviour (largest grid 512^3 = 9 mips).
    const u32 shift = mip > 31u ? 31u : mip;
    const u32 d = res >> shift;
    return d == 0 ? 1u : d;
}

// Full grid [0, res) on every axis; identity for unbounded/fallback dispatches.
inline VoxelBox fullVoxelBox(u32 res) {
    VoxelBox b;
    b.hi[0] = b.hi[1] = b.hi[2] = res;
    return b;
}

// Voxel-space box one draw's world AABB touches (mip 0), padded outward and clamped to volume.
// Non-finite input returns fullVoxelBox(res) (unbounded fallback).
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
            // Wholly outside volume on this axis; empty regardless of padVoxels.
            b.lo[i] = b.hi[i] = 0;
            continue;
        }
        // Clamp raw box to volume before padding.
        const f32 loClamped = lo0 < 0.0f ? 0.0f : lo0;
        const f32 hiClamped = hi0 > resF ? resF : hi0;
        const f32 loPadded = loClamped - static_cast<f32>(padVoxels);
        const f32 hiPadded = hiClamped + static_cast<f32>(padVoxels);
        b.lo[i] = static_cast<u32>(loPadded < 0.0f ? 0.0f : loPadded);
        b.hi[i] = static_cast<u32>(hiPadded > resF ? resF : hiPadded);
    }
    return b;
}

// Union of two boxes. Empty operand is identity (no special-case needed for first iteration).
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

// Round lo down, hi up to nearest multiple; hi clamped to res. Empty box stays empty.
// Aligns CSClear/CSResolve [numthreads(4,4,4)] dispatch rectangles so same voxel set is affected.
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

// Project mip-0 box to mip level: lo >> mip, ceil(hi / 2^mip), each clamped to mipDim(res,mip).
// Exact, not conservative: CSMip's 2x2x2 block filter reads exactly the source range; no gaps or overreach.
inline VoxelBox mipBox(const VoxelBox& mip0, u32 mip, u32 res) {
    if (mip0.empty()) return mip0;
    const u32 dim = mipDim(res, mip);
    const u32 shift = mip > 31u ? 31u : mip;
    const u64 roundUp = (static_cast<u64>(1) << shift) - 1;
    VoxelBox r;
    for (int i = 0; i < 3; ++i) {
        u32 lo = mip0.lo[i] >> shift;
        u64 hi64 = (static_cast<u64>(mip0.hi[i]) + roundUp) >> shift;
        u32 hi = hi64 > dim ? dim : static_cast<u32>(hi64);
        if (lo > dim) lo = dim;
        r.lo[i] = lo;
        r.hi[i] = hi;
    }
    return r;
}

// Dispatch group counts for box under threadsPerAxis-wide compute group: ceil(extent / threadsPerAxis) per axis.
// Returns 0 for all axes if box is empty or threadsPerAxis is 0.
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

// Pack box and source mip index into GiDispatchConstants bytes.
inline GiDispatchConstants dispatchConstants(const VoxelBox& b, u32 srcMip) {
    GiDispatchConstants c;
    c.srcMip = srcMip;
    for (int i = 0; i < 3; ++i) {
        c.boxLo[i] = b.lo[i];
        c.boxHi[i] = b.hi[i];
    }
    return c;
}

// Number of voxels a box covers; 0 for empty (checked explicitly).
inline u64 voxelCount(const VoxelBox& b) {
    if (b.empty()) return 0;
    return static_cast<u64>(b.hi[0] - b.lo[0]) * static_cast<u64>(b.hi[1] - b.lo[1]) *
           static_cast<u64>(b.hi[2] - b.lo[2]);
}

// voxelCount(b) / res^3; 0 for res 0 (no division by zero).
inline f64 gridFraction(const VoxelBox& b, u32 res) {
    if (res == 0) return 0.0;
    const f64 total = static_cast<f64>(res) * static_cast<f64>(res) * static_cast<f64>(res);
    return static_cast<f64>(voxelCount(b)) / total;
}

} // namespace aver::voxi
