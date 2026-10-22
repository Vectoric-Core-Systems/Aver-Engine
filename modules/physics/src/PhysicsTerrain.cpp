// PhysicsTerrain.cpp -- heightfields v2 (physics_terrain_abi.h): the one heightfield builder, non-square fields with holes
// and a surface per cell, and edits that clone the shape, change the clone and swap it onto the body.
//
// aver_phys_add_heightfield (PhysicsWorld.cpp) builds through makeHeightfield below with an empty parameter block, so a
// terrain made the old way and one made through aver_phys_heightfield_shape are the same shape bit for bit.
//
// Jolt's shape cannot say how big the caller's rectangle was, which edit counters it has taken or which edge angle it was
// built with, so every field made here carries a small record (HfMeta) found through the shape's user data: a tag in the
// top 16 bits and a serial. HeightFieldShape::Clone copies the user data, so the record follows a field through its edits.
#include "aver/physics/physics_terrain_abi.h"
#include "PhysicsShapeTable.hpp"
#include "PhysicsSurface.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace {

using aver::AbiError;

constexpr int32_t kMaxSide = 16384;          // 14 bits per axis (HeightFieldShape.h)
constexpr int32_t kMaxPalette = 256;         // material indices are bytes
constexpr float   kDefaultEdgeCos = 0.996195f;   // cos(5 degrees): the default of HeightFieldShape::SetHeights

int32_t fail(AbiError e) {
    aver::setAbiError(e);
    return 0;
}

bool refuse(const char* what) {
    AVER_WARN("[Physics] heightfield parameters: {}", what);
    aver::setAbiError(AbiError::InvalidArgument);
    return false;
}

// ---- the per-field record ------------------------------------------------------------------------------------------

constexpr uint64_t kTagMask = 0xFFFF000000000000ull;
constexpr uint64_t kTag = 0x4846000000000000ull;   // "HF"

struct HfMeta {
    int32_t cols = 0, rows = 0, bits = 8;
    float spacingCm = 0.0f;
    float edgeCos = kDefaultEdgeCos;
    std::atomic<uint64_t> clamped{0};
};

std::mutex g_metaMutex;
std::unordered_map<uint64_t, std::unique_ptr<HfMeta>> g_meta;   // never freed: a few bytes per field built
uint64_t g_nextSerial = 1;

uint64_t newSerial() {
    std::lock_guard<std::mutex> lock(g_metaMutex);
    return g_nextSerial++;
}

HfMeta* metaOf(const JPH::Shape& s) {
    const uint64_t u = s.GetUserData();
    if ((u & kTagMask) != kTag) return nullptr;
    std::lock_guard<std::mutex> lock(g_metaMutex);
    const auto it = g_meta.find(u & ~kTagMask);
    return it == g_meta.end() ? nullptr : it->second.get();
}

// ---- a body's heightfield ------------------------------------------------------------------------------------------

struct Field {
    JPH::BodyID id;
    JPH::RefConst<JPH::Shape> shape;
    const JPH::HeightFieldShape* hf = nullptr;
    HfMeta* meta = nullptr;
};

// The heightfield a body's shape IS (a scaled one has a decorator around it and is refused) and the record of the
// module that made it. False with the reason recorded.
bool fieldOf(int32_t body, Field& f) {
    const JPH::BodyID* idp = findBody(body);
    if (!idp) return false;
    f.id = *idp;
    f.shape = bi().GetShape(f.id);
    if (!f.shape || f.shape->GetSubType() != JPH::EShapeSubType::HeightField) {
        AVER_WARN("[Physics] body {} is not a heightfield (a scaled one cannot be edited)", body);
        aver::setAbiError(AbiError::Unsupported);
        return false;
    }
    f.hf = static_cast<const JPH::HeightFieldShape*>(f.shape.GetPtr());
    f.meta = metaOf(*f.shape);
    if (!f.meta) {
        AVER_WARN("[Physics] body {}: this heightfield was not made by aver_phys_heightfield_shape or aver_phys_add_heightfield", body);
        aver::setAbiError(AbiError::Unsupported);
        return false;
    }
    return true;
}

// A w * h rectangle at (x0, y0) inside a cols * rows grid.
bool rectInside(int32_t x0, int32_t y0, int32_t w, int32_t h, int32_t cols, int32_t rows) {
    return x0 >= 0 && y0 >= 0 && w >= 1 && h >= 1 &&
           static_cast<int64_t>(x0) + w <= cols && static_cast<int64_t>(y0) + h <= rows;
}

uint32_t alignDown(int32_t v, uint32_t bs) { return static_cast<uint32_t>(v) / bs * bs; }
uint32_t alignUp(int32_t v, uint32_t bs) { return (static_cast<uint32_t>(v) + bs - 1) / bs * bs; }

JPH::PhysicsMaterialList materialList(const int32_t* palette, int32_t count) {
    JPH::PhysicsMaterialList out;
    out.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i)
        out.push_back(palette[i] != 0 ? surfaceMaterial(palette[i]) : JPH::RefConst<JPH::PhysicsMaterial>(JPH::PhysicsMaterial::sDefault));
    return out;
}

} // namespace

namespace aver::physics::detail {

bool parseHeightfieldParams(const float* p, int32_t n, HeightfieldParams& out) {
    out = HeightfieldParams{};
    if (n < 0 || n > AVER_PHYS_HP_COUNT) return refuse("more entries than the block defines");
    if (n > 0 && !p) return refuse("a count without a block");
    for (int32_t i = 0; i < n; ++i)
        if (!std::isfinite(p[i])) return refuse("a non-finite entry");

    if (n > AVER_PHYS_HP_BLOCK_SIZE && p[AVER_PHYS_HP_BLOCK_SIZE] >= 0.0f) {
        const long b = std::lround(p[AVER_PHYS_HP_BLOCK_SIZE]);
        if (b < 2 || b > 8) return refuse("block size is 2 to 8");
        out.blockSize = static_cast<int>(b);
    }
    if (n > AVER_PHYS_HP_BITS_PER_SAMPLE && p[AVER_PHYS_HP_BITS_PER_SAMPLE] >= 0.0f) {
        const long b = std::lround(p[AVER_PHYS_HP_BITS_PER_SAMPLE]);
        if (b < 1 || b > 16) return refuse("bits per sample is 1 to 16");
        out.bits = static_cast<int>(b);
    }
    if (n > AVER_PHYS_HP_EDGE_ANGLE_DEG && p[AVER_PHYS_HP_EDGE_ANGLE_DEG] >= 0.0f) {
        if (p[AVER_PHYS_HP_EDGE_ANGLE_DEG] > 90.0f) return refuse("edge angle is 0 to 90 degrees");
        out.edgeSet = true;
        out.edgeAngleDeg = p[AVER_PHYS_HP_EDGE_ANGLE_DEG];
    }
    if (n > AVER_PHYS_HP_EDIT_HEADROOM_CM && p[AVER_PHYS_HP_EDIT_HEADROOM_CM] >= 0.0f) {
        if (p[AVER_PHYS_HP_EDIT_HEADROOM_CM] > 1.0e6f) return refuse("edit headroom is at most 1e6 cm");
        out.headroomCm = p[AVER_PHYS_HP_EDIT_HEADROOM_CM];
    }
    return true;
}

JPH::Ref<JPH::Shape> makeHeightfield(const float* samples, int32_t cols, int32_t rows, float spacingCm,
                                     const uint8_t* cellPalette, const int32_t* palette, int32_t paletteCount,
                                     const HeightfieldParams& hp) {
    if (!samples) { aver::setAbiError(AbiError::NullPointer); return nullptr; }
    if (cols < 2 || rows < 2 || cols > kMaxSide || rows > kMaxSide) {
        AVER_WARN("[Physics] heightfield needs 2 to {} samples per side (got {} x {})", kMaxSide, cols, rows);
        aver::setAbiError(AbiError::InvalidArgument);
        return nullptr;
    }
    if (!(spacingCm > 0.0f) || !std::isfinite(spacingCm)) {
        AVER_WARN("[Physics] heightfield spacing must be positive (got {} cm)", spacingCm);
        aver::setAbiError(AbiError::InvalidArgument);
        return nullptr;
    }
    if (paletteCount < 0 || paletteCount > kMaxPalette || (paletteCount > 0 && !palette)) {
        AVER_WARN("[Physics] heightfield: a field holds at most {} surfaces (got {})", kMaxPalette, paletteCount);
        aver::setAbiError(AbiError::InvalidArgument);
        return nullptr;
    }

    // Jolt's field is square, and needs at least two blocks a side: one more sample than a block does it.
    const uint32_t bs = static_cast<uint32_t>(hp.blockSize);
    const uint32_t n = std::max<uint32_t>({static_cast<uint32_t>(cols), static_cast<uint32_t>(rows), bs + 1u});
    const size_t nn = static_cast<size_t>(n) * n;

    // Built directly in Jolt's Y-up axes rather than through the axis map: sample (x, y) is Jolt (x, h, y) * scale.
    JPH::Array<float> heights;
    heights.resize(nn, JPH::HeightFieldShapeConstants::cNoCollisionValue);
    float lo = FLT_MAX, hi = -FLT_MAX;
    size_t solid = 0;
    for (int32_t y = 0; y < rows; ++y)
        for (int32_t x = 0; x < cols; ++x) {
            const float v = samples[static_cast<size_t>(y) * static_cast<size_t>(cols) + static_cast<size_t>(x)];
            if (!std::isfinite(v) || v >= FLT_MAX) continue;   // a hole
            const float m = cmToM(v);
            heights[static_cast<size_t>(y) * n + static_cast<size_t>(x)] = m;
            lo = std::min(lo, m);
            hi = std::max(hi, m);
            ++solid;
        }
    if (solid == 0) {
        AVER_WARN("[Physics] heightfield: every sample is a hole, so there is nothing to collide with");
        aver::setAbiError(AbiError::InvalidArgument);
        return nullptr;
    }

    JPH::PhysicsMaterialList materials;
    JPH::Array<JPH::uint8> indices;
    if (paletteCount > 0) {
        materials = materialList(palette, paletteCount);
        const size_t side = n - 1;
        indices.resize(side * side, JPH::uint8(0));   // padded cells and a missing index array are palette entry 0
        if (cellPalette) {
            for (int32_t y = 0; y + 1 < rows; ++y)
                for (int32_t x = 0; x + 1 < cols; ++x) {
                    const uint8_t v = cellPalette[static_cast<size_t>(y) * static_cast<size_t>(cols - 1) + static_cast<size_t>(x)];
                    if (v >= paletteCount) {
                        AVER_WARN("[Physics] heightfield: cell ({}, {}) names palette entry {} of {}", x, y, static_cast<int>(v), paletteCount);
                        aver::setAbiError(AbiError::InvalidArgument);
                        return nullptr;
                    }
                    indices[static_cast<size_t>(y) * side + static_cast<size_t>(x)] = v;
                }
        }
    }

    const float spM = cmToM(spacingCm);
    JPH::HeightFieldShapeSettings s(heights.data(), JPH::Vec3::sZero(), JPH::Vec3(spM, 1.0f, spM), n,
                                    indices.empty() ? nullptr : indices.data(), materials);
    s.SetEmbedded();
    s.mBlockSize = bs;
    s.mBitsPerSample = static_cast<uint32_t>(hp.bits);
    float edgeCos = kDefaultEdgeCos;
    if (hp.edgeSet) {
        edgeCos = std::cos(JPH::DegreesToRadians(hp.edgeAngleDeg));
        s.mActiveEdgeCosThresholdAngle = edgeCos;
    }
    // The range edits can encode: Jolt takes the samples' own min and max unless these reach further.
    if (hp.headroomCm > 0.0f) {
        const float h = cmToM(hp.headroomCm);
        s.mMinHeightValue = lo - h;
        s.mMaxHeightValue = hi + h;
    }
    const uint64_t serial = newSerial();
    s.mUserData = kTag | serial;

    auto res = s.Create();
    if (res.HasError()) {
        AVER_WARN("[Physics] heightfield: {}", res.GetError().c_str());
        aver::setAbiError(AbiError::InvalidArgument);
        return nullptr;
    }
    auto meta = std::make_unique<HfMeta>();
    meta->cols = cols;
    meta->rows = rows;
    meta->bits = hp.bits;
    meta->spacingCm = spacingCm;
    meta->edgeCos = edgeCos;
    {
        std::lock_guard<std::mutex> lock(g_metaMutex);
        g_meta.emplace(serial, std::move(meta));
    }
    return res.Get();
}

} // namespace aver::physics::detail

extern "C" {

int32_t aver_phys_terrain_abi_version(void) { return AVER_PHYS_TERRAIN_ABI_VERSION; }

int32_t aver_phys_heightfield_shape(const float* samples, int32_t cols, int32_t rows, float spacingCm,
                                    const uint8_t* cellPaletteIndex, const int32_t* palette, int32_t paletteCount,
                                    const float* p, int32_t n) {
    if (!g_world) return fail(AbiError::NotInitialised);
    HeightfieldParams hp;
    if (!parseHeightfieldParams(p, n, hp)) return 0;
    const JPH::Ref<JPH::Shape> shape = makeHeightfield(samples, cols, rows, spacingCm, cellPaletteIndex, palette, paletteCount, hp);
    if (!shape) return 0;
    aver::setAbiError(AbiError::Ok);
    return shapeTableAdd(shape);
}

int32_t aver_phys_heightfield_set_heights(int32_t body, int32_t x0, int32_t y0, int32_t w, int32_t h,
                                          const float* heights, int32_t flags) {
    Field f;
    if (!fieldOf(body, f)) return 0;
    if (!heights) return fail(AbiError::NullPointer);
    if ((flags & ~AVER_PHYS_HF_WAKE) != 0) return fail(AbiError::InvalidArgument);
    if (!rectInside(x0, y0, w, h, f.meta->cols, f.meta->rows)) {
        AVER_WARN("[Physics] heightfield edit: the rectangle {}x{} at ({}, {}) is outside the {}x{} field", w, h, x0, y0, f.meta->cols, f.meta->rows);
        return fail(AbiError::OutOfRange);
    }

    // Jolt edits whole blocks: widen the rectangle outward to block multiples and fill the margin from what is there.
    const uint32_t bs = f.hf->GetBlockSize(), padded = f.hf->GetSampleCount();
    const uint32_t ax0 = alignDown(x0, bs), ay0 = alignDown(y0, bs);
    const uint32_t ax1 = std::min(padded, alignUp(x0 + w, bs)), ay1 = std::min(padded, alignUp(y0 + h, bs));
    const uint32_t aw = ax1 - ax0, ah = ay1 - ay0;
    std::vector<float> buf(static_cast<size_t>(aw) * ah);
    f.hf->GetHeights(ax0, ay0, aw, ah, buf.data(), static_cast<intptr_t>(aw));

    const float lo = f.hf->GetMinHeightValue(), hi = f.hf->GetMaxHeightValue();
    uint64_t clamped = 0;
    for (int32_t yy = 0; yy < h; ++yy)
        for (int32_t xx = 0; xx < w; ++xx) {
            const float v = heights[static_cast<size_t>(yy) * static_cast<size_t>(w) + static_cast<size_t>(xx)];
            float m = JPH::HeightFieldShapeConstants::cNoCollisionValue;
            if (std::isfinite(v) && v < FLT_MAX) {
                m = cmToM(v);
                if (m < lo) { m = lo; ++clamped; }
                else if (m > hi) { m = hi; ++clamped; }
            }
            buf[static_cast<size_t>(y0 - static_cast<int32_t>(ay0) + yy) * aw + static_cast<size_t>(x0 - static_cast<int32_t>(ax0) + xx)] = m;
        }

    // Jolt's route for a field that queries may be reading: edit a clone and swap it in.
    JPH::Ref<JPH::HeightFieldShape> clone = f.hf->Clone();
    clone->SetHeights(ax0, ay0, aw, ah, buf.data(), static_cast<intptr_t>(aw), *g_world->temp, f.meta->edgeCos);
    bi().SetShape(f.id, clone.GetPtr(), false, JPH::EActivation::DontActivate);
    if (clamped) f.meta->clamped.fetch_add(clamped, std::memory_order_relaxed);

    if (flags & AVER_PHYS_HF_WAKE) {
        // The edited area plus one cell around it, over the field's whole height range, in world space.
        const float sp = cmToM(f.meta->spacingCm);
        JPH::AABox local(JPH::Vec3((static_cast<float>(x0) - 1.0f) * sp, lo, (static_cast<float>(y0) - 1.0f) * sp),
                         JPH::Vec3(static_cast<float>(x0 + w) * sp, hi, static_cast<float>(y0 + h) * sp));
        const JPH::Mat44 xf = JPH::Mat44::sRotationTranslation(bi().GetRotation(f.id), JPH::Vec3(bi().GetPosition(f.id)));
        bi().ActivateBodiesInAABox(local.Transformed(xf), JPH::BroadPhaseLayerFilter(), JPH::ObjectLayerFilter());
    }
    aver::setAbiError(AbiError::Ok);
    return 1;
}

int32_t aver_phys_heightfield_get_heights(int32_t body, int32_t x0, int32_t y0, int32_t w, int32_t h, float* outHeights) {
    Field f;
    if (!fieldOf(body, f)) return 0;
    if (!outHeights) return fail(AbiError::NullPointer);
    if (!rectInside(x0, y0, w, h, f.meta->cols, f.meta->rows)) return fail(AbiError::OutOfRange);

    const uint32_t bs = f.hf->GetBlockSize(), padded = f.hf->GetSampleCount();
    const uint32_t ax0 = alignDown(x0, bs), ay0 = alignDown(y0, bs);
    const uint32_t ax1 = std::min(padded, alignUp(x0 + w, bs)), ay1 = std::min(padded, alignUp(y0 + h, bs));
    const uint32_t aw = ax1 - ax0, ah = ay1 - ay0;
    std::vector<float> buf(static_cast<size_t>(aw) * ah);
    f.hf->GetHeights(ax0, ay0, aw, ah, buf.data(), static_cast<intptr_t>(aw));

    for (int32_t yy = 0; yy < h; ++yy)
        for (int32_t xx = 0; xx < w; ++xx) {
            const float m = buf[static_cast<size_t>(y0 - static_cast<int32_t>(ay0) + yy) * aw + static_cast<size_t>(x0 - static_cast<int32_t>(ax0) + xx)];
            outHeights[static_cast<size_t>(yy) * static_cast<size_t>(w) + static_cast<size_t>(xx)] =
                m == JPH::HeightFieldShapeConstants::cNoCollisionValue ? FLT_MAX : mToCm(m);
        }
    aver::setAbiError(AbiError::Ok);
    return 1;
}

int32_t aver_phys_heightfield_set_cell_surfaces(int32_t body, int32_t x0, int32_t y0, int32_t w, int32_t h,
                                                const uint8_t* cellPaletteIndex, const int32_t* palette, int32_t paletteCount) {
    Field f;
    if (!fieldOf(body, f)) return 0;
    if (!palette || paletteCount < 1) return fail(palette ? AbiError::InvalidArgument : AbiError::NullPointer);
    if (paletteCount > kMaxPalette) {
        AVER_WARN("[Physics] heightfield: a field holds at most {} surfaces (got {})", kMaxPalette, paletteCount);
        return fail(AbiError::InvalidArgument);
    }
    if (!rectInside(x0, y0, w, h, f.meta->cols - 1, f.meta->rows - 1)) {
        AVER_WARN("[Physics] heightfield cells: the rectangle {}x{} at ({}, {}) is outside the {}x{} cells", w, h, x0, y0, f.meta->cols - 1, f.meta->rows - 1);
        return fail(AbiError::OutOfRange);
    }
    std::vector<uint8_t> idx(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
    if (cellPaletteIndex) {
        for (size_t i = 0; i < idx.size(); ++i) {
            if (cellPaletteIndex[i] >= paletteCount) {
                AVER_WARN("[Physics] heightfield cells: entry {} names palette entry {} of {}", i, static_cast<int>(cellPaletteIndex[i]), paletteCount);
                return fail(AbiError::InvalidArgument);
            }
            idx[i] = cellPaletteIndex[i];
        }
    }

    JPH::Ref<JPH::HeightFieldShape> clone = f.hf->Clone();
    // A field built with no surfaces has an empty list and every cell reads Jolt's default material. Give it that material as
    // entry 0 first, so the cells this call does not touch keep reading it instead of the palette's first entry.
    if (clone->GetMaterialList().empty()) {
        JPH::PhysicsMaterialList seed;
        seed.push_back(JPH::RefConst<JPH::PhysicsMaterial>(JPH::PhysicsMaterial::sDefault));
        const JPH::uint8 zero = 0;
        clone->SetMaterials(0, 0, 1, 1, &zero, 1, &seed, *g_world->temp);
    }
    const JPH::PhysicsMaterialList list = materialList(palette, paletteCount);
    if (!clone->SetMaterials(static_cast<JPH::uint>(x0), static_cast<JPH::uint>(y0), static_cast<JPH::uint>(w), static_cast<JPH::uint>(h),
                             idx.data(), static_cast<intptr_t>(w), &list, *g_world->temp)) {
        AVER_WARN("[Physics] heightfield cells: a field holds at most {} surfaces in all", kMaxPalette);
        return fail(AbiError::OutOfRange);
    }
    bi().SetShape(f.id, clone.GetPtr(), false, JPH::EActivation::DontActivate);
    aver::setAbiError(AbiError::Ok);
    return 1;
}

int32_t aver_phys_heightfield_info(int32_t body, float* outF, int32_t nF, int32_t* outI, int32_t nI) {
    Field f;
    if (!fieldOf(body, f)) return 0;
    if (nF < 0 || nI < 0) return fail(AbiError::InvalidArgument);
    if ((nF > 0 && !outF) || (nI > 0 && !outI)) return fail(AbiError::NullPointer);

    float fv[AVER_PHYS_HFI_COUNT];
    fv[AVER_PHYS_HFI_SPACING_CM] = f.meta->spacingCm;
    fv[AVER_PHYS_HFI_MIN_HEIGHT_CM] = mToCm(f.hf->GetMinHeightValue());
    fv[AVER_PHYS_HFI_MAX_HEIGHT_CM] = mToCm(f.hf->GetMaxHeightValue());
    fv[AVER_PHYS_HFI_CLAMPED_TOTAL] = static_cast<float>(f.meta->clamped.load(std::memory_order_relaxed));
    int32_t iv[AVER_PHYS_HII_COUNT];
    iv[AVER_PHYS_HII_COLS] = f.meta->cols;
    iv[AVER_PHYS_HII_ROWS] = f.meta->rows;
    iv[AVER_PHYS_HII_PADDED] = static_cast<int32_t>(f.hf->GetSampleCount());
    iv[AVER_PHYS_HII_BLOCK_SIZE] = static_cast<int32_t>(f.hf->GetBlockSize());
    iv[AVER_PHYS_HII_BITS] = f.meta->bits;
    iv[AVER_PHYS_HII_MATERIALS] = static_cast<int32_t>(f.hf->GetMaterialList().size());
    for (int32_t k = 0; k < std::min<int32_t>(nF, AVER_PHYS_HFI_COUNT); ++k) outF[k] = fv[k];
    for (int32_t k = 0; k < std::min<int32_t>(nI, AVER_PHYS_HII_COUNT); ++k) outI[k] = iv[k];
    aver::setAbiError(AbiError::Ok);
    return 1;
}

} // extern "C"
