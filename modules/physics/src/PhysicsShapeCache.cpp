// PhysicsShapeCache.cpp -- the shape binary cache (physics_state_abi.h): a mesh or heightfield
// shape to a byte blob and back, so a host can keep the BVH on disk instead of building it every launch.
//
// THE BLOB. A 48-byte header, then Jolt's own Shape::SaveBinaryState:
//
//   "APSH", shape cache version, sizeof(JPH::Real), Jolt version triple, the caller's key, the kind (1 mesh, 2 heightfield),
//   payload size, FNV-1a 64 of the payload
//
// Every field is checked before Jolt reads a byte: a MeshShape restores its BVH as raw bytes and does not validate it, so the
// checksum is what keeps a damaged file from becoming a crash in a ray query. A mismatch of any kind is a MISS (0), never an
// error the host has to handle.
//
// WHAT IS NOT SAVED. A shape that carries physical-surface materials (a palette of live surface ids) or per-triangle user data is
// refused by save, because neither survives a file. A restored heightfield has its user data cleared: the tag that ties a field to
// its edit metadata (PhysicsTerrain.cpp) names a record that only the building process holds, and a stale one could name another
// field's.
#include "PhysicsShapeTable.hpp"
#include "PhysicsStateRecorder.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Hash.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/physics/physics_state_abi.h"

#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace {

constexpr size_t kHeaderBytes = 48;
constexpr char kMagic[4] = {'A', 'P', 'S', 'H'};
constexpr uint32_t kKindMesh = 1, kKindHeightfield = 2;

// -1 = not decided: the environment variable answers until a host or the project says otherwise.
std::atomic<int32_t> g_enabled{-1};

uint32_t kindOf(const JPH::Shape& s) {
    switch (s.GetSubType()) {
        case JPH::EShapeSubType::Mesh:        return kKindMesh;
        case JPH::EShapeSubType::HeightField: return kKindHeightfield;
        default:                              return 0;
    }
}

template <class T> void putAt(uint8_t* p, size_t off, T v) { std::memcpy(p + off, &v, sizeof v); }
template <class T> T getAt(const uint8_t* p, size_t off) { T v; std::memcpy(&v, p + off, sizeof v); return v; }

// The payload Jolt writes for `s`, or empty when the shape cannot be saved (see the file header).
bool savePayload(const JPH::Shape& s, std::vector<uint8_t>& out) {
    JPH::PhysicsMaterialList materials;
    s.SaveMaterialState(materials);
    if (!materials.empty()) return false;                                 // a surface palette
    if (s.GetSubType() == JPH::EShapeSubType::Mesh && s.GetUserData() != 0) return false;   // per-triangle user data
    ByteStateRecorder rec(out);
    s.SaveBinaryState(rec);
    return !rec.IsFailed() && !out.empty();
}

} // namespace

extern "C" {

uint32_t aver_phys_shape_cache_version(void) { return AVER_PHYS_SHAPECACHE_VERSION; }

int32_t aver_phys_shape_cache_enable(int32_t on) {
    g_enabled.store(on != 0 ? 1 : 0, std::memory_order_relaxed);
    return 1;
}

int32_t aver_phys_shape_cache_enabled(void) {
    int32_t v = g_enabled.load(std::memory_order_relaxed);
    if (v < 0) {
        const char* e = std::getenv("AVER_PHYS_SHAPE_CACHE");
        v = (e && e[0] == '1') ? 1 : 0;
        g_enabled.store(v, std::memory_order_relaxed);
    }
    return v;
}

int32_t aver_phys_create_heightfield_shape(const float* samples, int32_t sampleCount, float spacingCm) {
    if (!g_world) { aver::setAbiError(AbiError::NotInitialised); return 0; }
    if (!samples) { aver::setAbiError(AbiError::NullPointer); return 0; }
    if (sampleCount < 2 || !(spacingCm > 0.0f)) { aver::setAbiError(AbiError::InvalidArgument); return 0; }
    // aver_phys_add_heightfield's builder with an empty parameter block, so the two make the same field.
    const JPH::Ref<JPH::Shape> shape = makeHeightfield(samples, sampleCount, sampleCount, spacingCm,
                                                       nullptr, nullptr, 0, HeightfieldParams{});
    if (!shape) return 0;
    aver::setAbiError(AbiError::Ok);
    return shapeTableAdd(shape);
}

int32_t aver_phys_shape_save(int32_t shape, uint64_t key, uint8_t* outData, int32_t cap) {
    const JPH::RefConst<JPH::Shape> s = shapeTableGet(shape);
    if (!s) { aver::setAbiError(AbiError::BadHandle); return 0; }
    const uint32_t kind = kindOf(*s);
    if (kind == 0) { aver::setAbiError(AbiError::InvalidArgument); return 0; }

    std::vector<uint8_t> payload;
    if (!savePayload(*s, payload)) { aver::setAbiError(AbiError::Unsupported); return 0; }
    if (payload.size() > 0x7FFFFFFFull - kHeaderBytes) { aver::setAbiError(AbiError::OutOfRange); return 0; }
    const size_t total = kHeaderBytes + payload.size();
    aver::setAbiError(AbiError::Ok);
    if (!outData) return static_cast<int32_t>(total);
    if (cap < 0 || static_cast<size_t>(cap) < total) { aver::setAbiError(AbiError::OutOfRange); return 0; }

    std::memcpy(outData, kMagic, sizeof kMagic);
    putAt<uint32_t>(outData, 4, AVER_PHYS_SHAPECACHE_VERSION);
    putAt<uint32_t>(outData, 8, static_cast<uint32_t>(sizeof(JPH::Real)));
    putAt<uint32_t>(outData, 12, JPH_VERSION_MAJOR);
    putAt<uint32_t>(outData, 16, JPH_VERSION_MINOR);
    putAt<uint32_t>(outData, 20, JPH_VERSION_PATCH);
    putAt<uint64_t>(outData, 24, key);
    putAt<uint32_t>(outData, 32, kind);
    putAt<uint32_t>(outData, 36, static_cast<uint32_t>(payload.size()));
    putAt<uint64_t>(outData, 40, fnv1a64(payload.data(), payload.size()));
    std::memcpy(outData + kHeaderBytes, payload.data(), payload.size());
    return static_cast<int32_t>(total);
}

int32_t aver_phys_shape_restore(const uint8_t* data, int32_t size, uint64_t key) {
    // Every refusal below is a miss: the caller builds the shape itself. No log line, because a cold cache is the normal case.
    if (!g_world) { aver::setAbiError(AbiError::NotInitialised); return 0; }
    if (!data || size < static_cast<int32_t>(kHeaderBytes)) { aver::setAbiError(AbiError::InvalidArgument); return 0; }
    if (std::memcmp(data, kMagic, sizeof kMagic) != 0) { aver::setAbiError(AbiError::InvalidArgument); return 0; }
    if (getAt<uint32_t>(data, 4) != AVER_PHYS_SHAPECACHE_VERSION ||
        getAt<uint32_t>(data, 8) != static_cast<uint32_t>(sizeof(JPH::Real)) ||
        getAt<uint32_t>(data, 12) != static_cast<uint32_t>(JPH_VERSION_MAJOR) ||
        getAt<uint32_t>(data, 16) != static_cast<uint32_t>(JPH_VERSION_MINOR) ||
        getAt<uint32_t>(data, 20) != static_cast<uint32_t>(JPH_VERSION_PATCH) ||
        getAt<uint64_t>(data, 24) != key) {
        aver::setAbiError(AbiError::InvalidArgument);
        return 0;
    }
    const uint32_t kind = getAt<uint32_t>(data, 32);
    const uint32_t payloadSize = getAt<uint32_t>(data, 36);
    if ((kind != kKindMesh && kind != kKindHeightfield) || payloadSize == 0 ||
        static_cast<size_t>(size) != kHeaderBytes + static_cast<size_t>(payloadSize) ||
        fnv1a64(data + kHeaderBytes, payloadSize) != getAt<uint64_t>(data, 40)) {
        aver::setAbiError(AbiError::InvalidArgument);
        return 0;
    }

    ByteStateRecorder rec(data + kHeaderBytes, payloadSize);
    JPH::Shape::ShapeResult res = JPH::Shape::sRestoreFromBinaryState(rec);
    if (res.HasError()) { aver::setAbiError(AbiError::InvalidArgument); return 0; }
    JPH::Ref<JPH::Shape> shape = res.Get();
    if (!shape || kindOf(*shape) != kind) { aver::setAbiError(AbiError::InvalidArgument); return 0; }
    // The saved shape had no materials (save refuses one that has), so the restored one gets none.
    shape->RestoreMaterialState(nullptr, 0);
    if (kind == kKindHeightfield) shape->SetUserData(0);   // see the file header
    aver::setAbiError(AbiError::Ok);
    return shapeTableAdd(shape);
}

} // extern "C"
