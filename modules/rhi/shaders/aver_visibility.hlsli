#ifndef AVER_VISIBILITY_HLSLI
#define AVER_VISIBILITY_HLSLI
// The primary-visibility record (rhi::PrimaryVisibility; Voxi's ray-driven visibility stage): uint4 of
// (instance ref, triangle, barycentric x, barycentric y as float bits), one per scene pixel at
// p.y * rowPitch + p.x. One definition for every reader: NeuRAA's edge classes and NRD2's object stop.
static const uint kAverRefMiss      = 0xFFFFFFFFu;
static const uint kAverRefFoliage   = 0x80000000u;   // voxi_rt.hlsli's AVER_RT_REF_FOLIAGE
static const uint kAverRefIndexMask = 0x07FFFFFFu;   // ... AVER_RT_REF_INDEX_MASK

// The object a ref belongs to. A foliage ref also carries its part (trunk, leaves) in bits 27-30; the
// parts of one tree are one object.
uint averObjectOf(uint ref) {
    return (ref != kAverRefMiss && (ref & kAverRefFoliage) != 0u) ? (ref & (kAverRefFoliage | kAverRefIndexMask)) : ref;
}
#endif
