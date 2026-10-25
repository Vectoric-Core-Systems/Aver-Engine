// PhysicsHairSettings.cpp -- the settings half of physics_hair_abi.h: the builder (strands, scalp, materials, params), finish (the conversion
// to Jolt space and HairSettings::Init), the settings table and its live material edits. Instances, update and the compute host are
// PhysicsHair.cpp.
//
// THE CONVERSION happens once, at finish: positions and lengths go from engine centimetres to Jolt metres through toJolt, directions through
// toJoltDir, and every JPH::HairSettings field is then in Jolt's own units. A material block is converted when it is applied, which is also
// how aver_phys_hair_settings_set_material edits a live settings object.
#include "aver/physics/physics_hair_abi.h"
#include "Convert.hpp"
#include "HairInternal.hpp"
#include "PhysicsInternal.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Geometry/IndexedTriangle.h>
#include <Jolt/Math/Float3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

using namespace aver;
using namespace aver::physics;
using namespace aver::physics::detail;

namespace {

int32_t refuse(AbiError e) {
    setAbiError(e);
    return 0;
}

int32_t succeed() {
    setAbiError(AbiError::Ok);
    return 1;
}

constexpr int32_t kMaxMaterials = 256;           // Jolt keeps the material index in 8 bits
constexpr int32_t kMaxStrandVertices = 255;      // and the vertex count of a strand
constexpr int32_t kMinStrandVertices = 3;
constexpr float   kMinSegmentCm = 1.0e-3f;       // Jolt asserts on a rod of zero length; a hair thinner than this is not a rod
constexpr int32_t kMaxVertices = 1 << 24;

const float kNaN = std::numeric_limits<float>::quiet_NaN();

bool isNaN(float v) { return std::isnan(v); }

// True when every entry is finite or NaN (NaN = leave as is).
bool finiteOrNaN(const float* p, int32_t n) {
    for (int32_t i = 0; i < n; ++i)
        if (!isNaN(p[i]) && !std::isfinite(p[i])) return false;
    return true;
}

JPH::Float3 toFloat3(const aver::Vec3& cm) {
    JPH::Float3 f;
    toJolt(cm).StoreFloat3(&f);
    return f;
}

// ---- parameters ------------------------------------------------------------------------------------------------------------

bool paramsLegal(const float* p, int32_t n) {
    if (!finiteOrNaN(p, n)) return false;
    auto in = [&](int i, float lo, float hi) { return i >= n || isNaN(p[i]) || (p[i] >= lo && p[i] <= hi); };
    return in(AVER_PHYS_HAIRP_ITERATIONS_PER_SECOND, 1.0f, 4096.0f) && in(AVER_PHYS_HAIRP_MAX_DELTA_TIME, 1.0e-4f, 1.0f) &&
           in(AVER_PHYS_HAIRP_GRID_X, 4.0f, 128.0f) && in(AVER_PHYS_HAIRP_GRID_Y, 4.0f, 128.0f) && in(AVER_PHYS_HAIRP_GRID_Z, 4.0f, 128.0f) &&
           in(AVER_PHYS_HAIRP_BOUNDS_PADDING_CM, 0.0f, 1.0e4f) && in(AVER_PHYS_HAIRP_SIM_FRACTION, 1.0e-4f, 1.0f);
}

float paramOr(const float* p, int i, float fallback) { return isNaN(p[i]) ? fallback : p[i]; }

// A gradient field set from four ABI floats (min, max, minFraction, maxFraction), NaN entries keeping the old value; `scale` converts the
// value (not the fractions) to Jolt's unit. False when the result would divide by zero in the sampler.
bool setGradient(JPH::HairSettings::Gradient& g, const float* p, int32_t n, int32_t base, float scale) {
    JPH::HairSettings::Gradient t = g;
    auto pick = [&](int k, float& dst, float mul) {
        const int32_t i = base + k;
        if (i < n && !isNaN(p[i])) dst = p[i] * mul;
    };
    pick(0, t.mMin, scale);
    pick(1, t.mMax, scale);
    pick(2, t.mMinFraction, 1.0f);
    pick(3, t.mMaxFraction, 1.0f);
    if (t.mMaxFraction == t.mMinFraction) return false;
    g = t;
    return true;
}

} // namespace

namespace aver::physics::detail {

HairBuilderRec::HairBuilderRec() {
    for (float& f : params) f = kNaN;
}

bool hairApplyMaterialBlock(JPH::HairSettings::Material& m, const float* p, int32_t n) {
    if (n < 0 || n > AVER_PHYS_HAIRM_COUNT || (n > 0 && !p)) return false;
    if (!finiteOrNaN(p, n)) return false;
    auto has = [&](int i) { return i < n && !isNaN(p[i]); };
    auto atLeast = [&](int i, float lo) { return !has(i) || p[i] >= lo; };
    auto above = [&](int i, float lo) { return !has(i) || p[i] > lo; };
    if (!atLeast(AVER_PHYS_HAIRM_LINEAR_DAMPING, 0.0f) || !atLeast(AVER_PHYS_HAIRM_ANGULAR_DAMPING, 0.0f) ||
        !above(AVER_PHYS_HAIRM_MAX_LINEAR_CMS, 0.0f) || !above(AVER_PHYS_HAIRM_MAX_ANGULAR_RADS, 0.0f) ||
        !atLeast(AVER_PHYS_HAIRM_FRICTION, 0.0f) || !atLeast(AVER_PHYS_HAIRM_BEND_COMPLIANCE, 0.0f) ||
        !atLeast(AVER_PHYS_HAIRM_STRETCH_COMPLIANCE, 0.0f) || !above(AVER_PHYS_HAIRM_INERTIA_MULTIPLIER, 0.0f) ||
        !atLeast(AVER_PHYS_HAIRM_GRID_DENSITY_FORCE, 0.0f) || !atLeast(AVER_PHYS_HAIRM_GRAVITY_PRELOAD, 0.0f))
        return false;
    if (has(AVER_PHYS_HAIRM_SIM_FRACTION) && !(p[AVER_PHYS_HAIRM_SIM_FRACTION] > 0.0f && p[AVER_PHYS_HAIRM_SIM_FRACTION] <= 1.0f)) return false;
    for (int k = 0; k < 4; ++k)
        if (!atLeast(AVER_PHYS_HAIRM_BEND_MULTIPLIER + k, 0.0f)) return false;
    for (int k = 0; k < 2; ++k)
        if (!atLeast(AVER_PHYS_HAIRM_RADIUS_CM + k, 0.0f)) return false;

    // Work on a copy so a refusal changes nothing.
    JPH::HairSettings::Material t = m;
    if (has(AVER_PHYS_HAIRM_ENABLE_COLLISION)) t.mEnableCollision = p[AVER_PHYS_HAIRM_ENABLE_COLLISION] > 0.5f;
    if (has(AVER_PHYS_HAIRM_ENABLE_LRA)) t.mEnableLRA = p[AVER_PHYS_HAIRM_ENABLE_LRA] > 0.5f;
    if (has(AVER_PHYS_HAIRM_LINEAR_DAMPING)) t.mLinearDamping = p[AVER_PHYS_HAIRM_LINEAR_DAMPING];
    if (has(AVER_PHYS_HAIRM_ANGULAR_DAMPING)) t.mAngularDamping = p[AVER_PHYS_HAIRM_ANGULAR_DAMPING];
    if (has(AVER_PHYS_HAIRM_MAX_LINEAR_CMS)) t.mMaxLinearVelocity = p[AVER_PHYS_HAIRM_MAX_LINEAR_CMS] / kCmPerMetre;
    if (has(AVER_PHYS_HAIRM_MAX_ANGULAR_RADS)) t.mMaxAngularVelocity = p[AVER_PHYS_HAIRM_MAX_ANGULAR_RADS];
    if (!setGradient(t.mGravityFactor, p, n, AVER_PHYS_HAIRM_GRAVITY, 1.0f)) return false;
    if (has(AVER_PHYS_HAIRM_FRICTION)) t.mFriction = p[AVER_PHYS_HAIRM_FRICTION];
    if (has(AVER_PHYS_HAIRM_BEND_COMPLIANCE)) t.mBendCompliance = p[AVER_PHYS_HAIRM_BEND_COMPLIANCE];
    // Float4 has no mutable operator[]: the four multipliers are set by field.
    if (has(AVER_PHYS_HAIRM_BEND_MULTIPLIER + 0)) t.mBendComplianceMultiplier.x = p[AVER_PHYS_HAIRM_BEND_MULTIPLIER + 0];
    if (has(AVER_PHYS_HAIRM_BEND_MULTIPLIER + 1)) t.mBendComplianceMultiplier.y = p[AVER_PHYS_HAIRM_BEND_MULTIPLIER + 1];
    if (has(AVER_PHYS_HAIRM_BEND_MULTIPLIER + 2)) t.mBendComplianceMultiplier.z = p[AVER_PHYS_HAIRM_BEND_MULTIPLIER + 2];
    if (has(AVER_PHYS_HAIRM_BEND_MULTIPLIER + 3)) t.mBendComplianceMultiplier.w = p[AVER_PHYS_HAIRM_BEND_MULTIPLIER + 3];
    if (has(AVER_PHYS_HAIRM_STRETCH_COMPLIANCE)) t.mStretchCompliance = p[AVER_PHYS_HAIRM_STRETCH_COMPLIANCE];
    if (has(AVER_PHYS_HAIRM_INERTIA_MULTIPLIER)) t.mInertiaMultiplier = p[AVER_PHYS_HAIRM_INERTIA_MULTIPLIER];
    if (!setGradient(t.mHairRadius, p, n, AVER_PHYS_HAIRM_RADIUS_CM, 1.0f / kCmPerMetre)) return false;
    if (!setGradient(t.mWorldTransformInfluence, p, n, AVER_PHYS_HAIRM_WORLD_INFLUENCE, 1.0f)) return false;
    if (!setGradient(t.mGridVelocityFactor, p, n, AVER_PHYS_HAIRM_GRID_VELOCITY, 1.0f)) return false;
    if (has(AVER_PHYS_HAIRM_GRID_DENSITY_FORCE)) t.mGridDensityForceFactor = p[AVER_PHYS_HAIRM_GRID_DENSITY_FORCE];
    if (!setGradient(t.mGlobalPose, p, n, AVER_PHYS_HAIRM_GLOBAL_POSE, 1.0f)) return false;
    if (!setGradient(t.mSkinGlobalPose, p, n, AVER_PHYS_HAIRM_SKIN_GLOBAL_POSE, 1.0f)) return false;
    if (has(AVER_PHYS_HAIRM_SIM_FRACTION)) t.mSimulationStrandsFraction = p[AVER_PHYS_HAIRM_SIM_FRACTION];
    if (has(AVER_PHYS_HAIRM_GRAVITY_PRELOAD)) t.mGravityPreloadFactor = p[AVER_PHYS_HAIRM_GRAVITY_PRELOAD];
    m = t;
    return true;
}

} // namespace aver::physics::detail

namespace {

HairBuilderRec* stage(HairGlobals& g, int32_t s) {
    const auto it = g.builders.find(s);
    if (it == g.builders.end()) { setAbiError(AbiError::BadHandle); return nullptr; }
    return &it->second;
}

HairSettingsRec* settingsOf(HairGlobals& g, int32_t h) {
    const auto it = g.settings.find(h);
    if (it == g.settings.end()) { setAbiError(AbiError::BadHandle); return nullptr; }
    return &it->second;
}

} // namespace

extern "C" {

int32_t aver_phys_hair_settings_begin(void) {
    HairGlobals& g = hairGlobals();
    std::lock_guard<std::mutex> lock(g.mutex);
    const int32_t id = g.nextBuilder++;
    g.builders.emplace(id, HairBuilderRec());
    setAbiError(AbiError::Ok);
    return id;
}

int32_t aver_phys_hair_settings_strands(int32_t s, const float* pointsXyz, const float* invMass, int32_t vertexCount,
                                        const int32_t* strandStart, const int32_t* strandMaterial, int32_t strandCount) {
    if (!pointsXyz || !strandStart) return refuse(AbiError::NullPointer);
    if (vertexCount <= 0 || strandCount <= 0 || vertexCount > kMaxVertices || strandCount > vertexCount / kMinStrandVertices)
        return refuse(AbiError::InvalidArgument);
    // Validate everything before touching the builder, so a refusal leaves the earlier strands in place.
    if (strandStart[0] != 0 || strandStart[strandCount] != vertexCount) return refuse(AbiError::InvalidArgument);
    for (int32_t i = 0; i < strandCount; ++i) {
        const int32_t count = strandStart[i + 1] - strandStart[i];
        if (count < kMinStrandVertices || count > kMaxStrandVertices) return refuse(AbiError::InvalidArgument);
        if (strandMaterial && (strandMaterial[i] < 0 || strandMaterial[i] >= kMaxMaterials)) return refuse(AbiError::OutOfRange);
        for (int32_t v = strandStart[i]; v < strandStart[i + 1]; ++v) {
            const float* p = pointsXyz + static_cast<size_t>(v) * 3;
            if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return refuse(AbiError::InvalidArgument);
            if (invMass && (!std::isfinite(invMass[v]) || invMass[v] < 0.0f)) return refuse(AbiError::InvalidArgument);
            if (v > strandStart[i]) {
                const float* q = p - 3;
                const float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
                if (dx * dx + dy * dy + dz * dz < kMinSegmentCm * kMinSegmentCm) return refuse(AbiError::InvalidArgument);
            }
        }
    }
    HairGlobals& g = hairGlobals();
    std::lock_guard<std::mutex> lock(g.mutex);
    HairBuilderRec* b = stage(g, s);
    if (!b) return 0;
    b->points.assign(pointsXyz, pointsXyz + static_cast<size_t>(vertexCount) * 3);
    b->invMass.clear();
    if (invMass) b->invMass.assign(invMass, invMass + vertexCount);
    b->strandStart.assign(strandStart, strandStart + strandCount + 1);
    b->strandMaterial.clear();
    if (strandMaterial) b->strandMaterial.assign(strandMaterial, strandMaterial + strandCount);
    b->vertexCount = vertexCount;
    b->strandCount = strandCount;
    return succeed();
}

int32_t aver_phys_hair_settings_scalp(int32_t s, const float* vertsXyz, int32_t vertexCount, const int32_t* tri3, int32_t triCount,
                                      const int32_t* joints4, const float* weights4, int32_t jointCount) {
    if (!vertsXyz || !tri3) return refuse(AbiError::NullPointer);
    if (vertexCount < 3 || triCount < 1 || vertexCount > kMaxVertices || triCount > kMaxVertices) return refuse(AbiError::InvalidArgument);
    if (jointCount < 0 || jointCount > 255) return refuse(AbiError::OutOfRange);
    if ((joints4 == nullptr) != (weights4 == nullptr)) return refuse(AbiError::InvalidArgument);
    const int32_t joints = jointCount == 0 ? 1 : jointCount;
    for (int32_t i = 0; i < vertexCount * 3; ++i)
        if (!std::isfinite(vertsXyz[i])) return refuse(AbiError::InvalidArgument);
    for (int32_t i = 0; i < triCount * 3; ++i)
        if (tri3[i] < 0 || tri3[i] >= vertexCount) return refuse(AbiError::OutOfRange);
    if (joints4) {
        for (int32_t i = 0; i < vertexCount * 4; ++i) {
            if (joints4[i] < 0 || joints4[i] >= joints) return refuse(AbiError::OutOfRange);
            if (!std::isfinite(weights4[i]) || weights4[i] < 0.0f) return refuse(AbiError::InvalidArgument);
        }
    }
    HairGlobals& g = hairGlobals();
    std::lock_guard<std::mutex> lock(g.mutex);
    HairBuilderRec* b = stage(g, s);
    if (!b) return 0;
    b->hasScalp = true;
    b->scalpVerts.assign(vertsXyz, vertsXyz + static_cast<size_t>(vertexCount) * 3);
    b->scalpTris.assign(tri3, tri3 + static_cast<size_t>(triCount) * 3);
    b->scalpJoints.clear();
    b->scalpWeights.clear();
    if (joints4) {
        b->scalpJoints.assign(joints4, joints4 + static_cast<size_t>(vertexCount) * 4);
        b->scalpWeights.assign(weights4, weights4 + static_cast<size_t>(vertexCount) * 4);
    }
    b->scalpVertexCount = vertexCount;
    b->scalpTriCount = triCount;
    b->scalpJointCount = joints;
    return succeed();
}

int32_t aver_phys_hair_settings_material(int32_t s, int32_t index, const float* p, int32_t n) {
    if (index < 0 || index >= kMaxMaterials) return refuse(AbiError::OutOfRange);
    if (n < 0 || n > AVER_PHYS_HAIRM_COUNT) return refuse(AbiError::InvalidArgument);
    if (n > 0 && !p) return refuse(AbiError::NullPointer);
    // Check the block against a scratch material now, so a bad value is refused here and not at finish.
    JPH::HairSettings::Material scratch;
    if (!hairApplyMaterialBlock(scratch, p, n)) return refuse(AbiError::InvalidArgument);
    HairGlobals& g = hairGlobals();
    std::lock_guard<std::mutex> lock(g.mutex);
    HairBuilderRec* b = stage(g, s);
    if (!b) return 0;
    if (static_cast<int32_t>(b->materials.size()) <= index) b->materials.resize(static_cast<size_t>(index) + 1);
    HairBuilderRec::MaterialBlock& mb = b->materials[static_cast<size_t>(index)];
    if (!mb.described) {
        for (float& f : mb.p) f = kNaN;
        mb.described = true;
    }
    for (int32_t i = 0; i < n; ++i)
        if (!isNaN(p[i])) mb.p[i] = p[i];
    return succeed();
}

int32_t aver_phys_hair_settings_params(int32_t s, const float* p, int32_t n) {
    if (n < 0 || n > AVER_PHYS_HAIRP_COUNT) return refuse(AbiError::InvalidArgument);
    if (n > 0 && !p) return refuse(AbiError::NullPointer);
    if (!paramsLegal(p, n)) return refuse(AbiError::InvalidArgument);
    HairGlobals& g = hairGlobals();
    std::lock_guard<std::mutex> lock(g.mutex);
    HairBuilderRec* b = stage(g, s);
    if (!b) return 0;
    for (int32_t i = 0; i < n; ++i)
        if (!isNaN(p[i])) b->params[i] = p[i];
    return succeed();
}

int32_t aver_phys_hair_settings_cancel(int32_t s) {
    HairGlobals& g = hairGlobals();
    std::lock_guard<std::mutex> lock(g.mutex);
    if (!stage(g, s)) return 0;
    g.builders.erase(s);
    return succeed();
}

int32_t aver_phys_hair_settings_finish(int32_t s, int32_t resampleVertices, float* outRootToScalpCm) {
    // The builder is consumed whatever the outcome; the heavy work runs on the copy, unlocked.
    HairBuilderRec b;
    {
        HairGlobals& g = hairGlobals();
        std::lock_guard<std::mutex> lock(g.mutex);
        HairBuilderRec* staged = stage(g, s);
        if (!staged) return 0;
        b = std::move(*staged);
        g.builders.erase(s);
    }
    if (outRootToScalpCm) *outRootToScalpCm = 0.0f;
    if (!g_world) return refuse(AbiError::NotInitialised);
    if (b.vertexCount <= 0 || b.strandCount <= 0) return refuse(AbiError::InvalidArgument);
    if (resampleVertices != 0 && (resampleVertices < kMinStrandVertices || resampleVertices > kMaxStrandVertices)) return refuse(AbiError::InvalidArgument);

    // ---- materials ----
    int32_t highest = static_cast<int32_t>(b.materials.size()) - 1;
    for (int32_t i = 0; i < b.strandCount && !b.strandMaterial.empty(); ++i) highest = std::max(highest, b.strandMaterial[static_cast<size_t>(i)]);
    if (highest < 0) highest = 0;
    const float simFraction = paramOr(b.params, AVER_PHYS_HAIRP_SIM_FRACTION, 0.1f);
    JPH::Array<JPH::HairSettings::Material> materials;
    materials.resize(static_cast<size_t>(highest) + 1);
    for (size_t i = 0; i < materials.size(); ++i) {
        JPH::HairSettings::Material& m = materials[i];
        m.mSimulationStrandsFraction = simFraction;
        if (i < b.materials.size() && b.materials[i].described &&
            !hairApplyMaterialBlock(m, b.materials[i].p, AVER_PHYS_HAIRM_COUNT))
            return refuse(AbiError::InvalidArgument);
    }

    // ---- strands, converted once to Jolt space ----
    JPH::Array<JPH::HairSettings::SVertex> vertices;
    vertices.reserve(static_cast<size_t>(b.vertexCount));
    JPH::Array<JPH::HairSettings::SStrand> strands;
    strands.reserve(static_cast<size_t>(b.strandCount));
    for (int32_t si = 0; si < b.strandCount; ++si) {
        const int32_t first = b.strandStart[static_cast<size_t>(si)], last = b.strandStart[static_cast<size_t>(si) + 1];
        const int32_t material = b.strandMaterial.empty() ? 0 : b.strandMaterial[static_cast<size_t>(si)];
        strands.push_back(JPH::HairSettings::SStrand(static_cast<JPH::uint32>(first), static_cast<JPH::uint32>(last), static_cast<JPH::uint32>(material)));
        for (int32_t v = first; v < last; ++v) {
            const float* p = &b.points[static_cast<size_t>(v) * 3];
            // The first vertex of a strand is always pinned to the scalp (Jolt skips it in the solver).
            const float invMass = v == first ? 0.0f : (b.invMass.empty() ? 1.0f : b.invMass[static_cast<size_t>(v)]);
            vertices.push_back(JPH::HairSettings::SVertex(toFloat3(aver::Vec3(p[0], p[1], p[2])), invMass));
        }
    }

    JPH::Ref<JPH::HairSettings> hs = new JPH::HairSettings();
    hs->mMaterials = std::move(materials);
    hs->mNumIterationsPerSecond = static_cast<JPH::uint32>(paramOr(b.params, AVER_PHYS_HAIRP_ITERATIONS_PER_SECOND, 360.0f) + 0.5f);
    hs->mMaxDeltaTime = paramOr(b.params, AVER_PHYS_HAIRP_MAX_DELTA_TIME, 1.0f / 30.0f);
    hs->mGridSize = JPH::UVec4(static_cast<JPH::uint32>(paramOr(b.params, AVER_PHYS_HAIRP_GRID_X, 32.0f) + 0.5f),
                               static_cast<JPH::uint32>(paramOr(b.params, AVER_PHYS_HAIRP_GRID_Y, 32.0f) + 0.5f),
                               static_cast<JPH::uint32>(paramOr(b.params, AVER_PHYS_HAIRP_GRID_Z, 32.0f) + 0.5f), 0);
    hs->mSimulationBoundsPadding = JPH::Vec3::sReplicate(paramOr(b.params, AVER_PHYS_HAIRP_BOUNDS_PADDING_CM, 10.0f) / kCmPerMetre);
    hs->mInitialGravity = toJoltDir(aver::Vec3(paramOr(b.params, AVER_PHYS_HAIRP_INITIAL_GRAVITY_X, 0.0f),
                                               paramOr(b.params, AVER_PHYS_HAIRP_INITIAL_GRAVITY_Y, 0.0f),
                                               paramOr(b.params, AVER_PHYS_HAIRP_INITIAL_GRAVITY_Z, -981.0f)));

    // ---- the scalp: identity inverse binds, because the palette the engine hands in already carries them (model x inverse bind) ----
    int32_t scalpJoints = 0;
    if (b.hasScalp) {
        scalpJoints = b.scalpJointCount;
        hs->mScalpVertices.reserve(static_cast<size_t>(b.scalpVertexCount));
        for (int32_t i = 0; i < b.scalpVertexCount; ++i) {
            const float* p = &b.scalpVerts[static_cast<size_t>(i) * 3];
            hs->mScalpVertices.push_back(toFloat3(aver::Vec3(p[0], p[1], p[2])));
        }
        hs->mScalpTriangles.reserve(static_cast<size_t>(b.scalpTriCount));
        for (int32_t i = 0; i < b.scalpTriCount; ++i)
            hs->mScalpTriangles.push_back(JPH::IndexedTriangleNoMaterial(static_cast<JPH::uint32>(b.scalpTris[static_cast<size_t>(i) * 3]),
                                                                         static_cast<JPH::uint32>(b.scalpTris[static_cast<size_t>(i) * 3 + 1]),
                                                                         static_cast<JPH::uint32>(b.scalpTris[static_cast<size_t>(i) * 3 + 2])));
        hs->mScalpInverseBindPose.resize(static_cast<size_t>(scalpJoints), JPH::Mat44::sIdentity());
        hs->mScalpNumSkinWeightsPerVertex = 4;
        hs->mScalpSkinWeights.resize(static_cast<size_t>(b.scalpVertexCount) * 4);
        for (int32_t i = 0; i < b.scalpVertexCount; ++i) {
            float w[4] = {1, 0, 0, 0};
            int32_t j[4] = {0, 0, 0, 0};
            if (!b.scalpJoints.empty()) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    j[k] = b.scalpJoints[static_cast<size_t>(i) * 4 + static_cast<size_t>(k)];
                    w[k] = b.scalpWeights[static_cast<size_t>(i) * 4 + static_cast<size_t>(k)];
                    sum += w[k];
                }
                if (sum > 1.0e-6f) for (float& f : w) f /= sum;
                else { w[0] = 1; w[1] = w[2] = w[3] = 0; j[0] = 0; }
            }
            for (int k = 0; k < 4; ++k) {
                JPH::HairSettings::SkinWeight& sw = hs->mScalpSkinWeights[static_cast<size_t>(i) * 4 + static_cast<size_t>(k)];
                sw.mJointIdx = static_cast<JPH::uint32>(j[k]);
                sw.mWeight = w[k];
            }
        }
    }

    if (resampleVertices > 0) JPH::HairSettings::sResample(vertices, strands, static_cast<JPH::uint32>(resampleVertices));
    hs->InitRenderAndSimulationStrands(vertices, strands);
    float maxDistSq = 0.0f;
    hs->Init(maxDistSq);
    const float rootToScalpCm = std::sqrt(maxDistSq) * kCmPerMetre;

    // ---- compute buffers, on the installed host: back under the lock ----
    HairGlobals& g = hairGlobals();
    std::lock_guard<std::mutex> lock(g.mutex);
    if (!g_world) return refuse(AbiError::NotInitialised);
    if (!hairEnsureCompute(g)) return refuse(AbiError::Unsupported);
    hs->InitCompute(g.compute);

    HairSettingsRec rec;
    rec.settings = hs;
    rec.scalpJoints = scalpJoints;
    rec.rootToScalpCm = rootToScalpCm;
    // The simulation bounds, as an engine-axes box: the map permutes axes with signs, so the box of the eight corners is the box of two.
    {
        const JPH::AABox& box = hs->mSimulationBounds;
        float lo[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        float hi[3] = {-lo[0], -lo[1], -lo[2]};
        for (int c = 0; c < 8; ++c) {
            const JPH::Vec3 corner((c & 1) ? box.mMax.GetX() : box.mMin.GetX(), (c & 2) ? box.mMax.GetY() : box.mMin.GetY(),
                                   (c & 4) ? box.mMax.GetZ() : box.mMin.GetZ());
            const aver::Vec3 e = fromJolt(corner);
            const float v[3] = {e.x, e.y, e.z};
            for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], v[k]); hi[k] = std::max(hi[k], v[k]); }
        }
        for (int k = 0; k < 3; ++k) { rec.boundsCm[k] = lo[k]; rec.boundsCm[3 + k] = hi[k]; }
    }
    rec.renderTable.reserve(hs->mRenderStrands.size() * 2);
    for (const JPH::HairSettings::RStrand& rs : hs->mRenderStrands) {
        rec.renderTable.push_back(static_cast<int32_t>(rs.mStartVtx));
        rec.renderTable.push_back(static_cast<int32_t>(rs.VertexCount()));
    }
    const int32_t handle = g.nextSettings++;
    g.settings.emplace(handle, std::move(rec));
    if (outRootToScalpCm) *outRootToScalpCm = rootToScalpCm;
    AVER_INFO("[Hair] settings {}: {} render strands, {} simulated, {} vertices, root-to-scalp {:.3f} cm", handle,
              hs->mRenderStrands.size(), hs->mSimStrands.size(), hs->mRenderVertices.size(), rootToScalpCm);
    setAbiError(AbiError::Ok);
    return handle;
}

int32_t aver_phys_hair_settings_destroy(int32_t settings) {
    HairGlobals& g = hairGlobals();
    std::lock_guard<std::mutex> lock(g.mutex);
    HairSettingsRec* r = settingsOf(g, settings);
    if (!r) return 0;
    if (r->instances > 0) return refuse(AbiError::Unsupported);
    g.settings.erase(settings);
    return succeed();
}

int32_t aver_phys_hair_settings_set_material(int32_t settings, int32_t index, const float* p, int32_t n) {
    if (n < 0 || n > AVER_PHYS_HAIRM_COUNT) return refuse(AbiError::InvalidArgument);
    if (n > 0 && !p) return refuse(AbiError::NullPointer);
    HairGlobals& g = hairGlobals();
    std::lock_guard<std::mutex> lock(g.mutex);
    HairSettingsRec* r = settingsOf(g, settings);
    if (!r) return 0;
    if (index < 0 || static_cast<size_t>(index) >= r->settings->mMaterials.size()) return refuse(AbiError::OutOfRange);
    if (!hairApplyMaterialBlock(r->settings->mMaterials[static_cast<size_t>(index)], p, n)) return refuse(AbiError::InvalidArgument);
    return succeed();
}

int32_t aver_phys_hair_settings_info(int32_t settings, int32_t* outI, int32_t nI, float* outF, int32_t nF) {
    HairGlobals& g = hairGlobals();
    std::lock_guard<std::mutex> lock(g.mutex);
    HairSettingsRec* r = settingsOf(g, settings);
    if (!r) return 0;
    const JPH::HairSettings& hs = *r->settings;
    if (outI) {
        const int32_t v[AVER_PHYS_HAIRI_COUNT] = {
            static_cast<int32_t>(hs.mSimStrands.size()),       static_cast<int32_t>(hs.mSimVertices.size()),
            static_cast<int32_t>(hs.mRenderStrands.size()),    static_cast<int32_t>(hs.mRenderVertices.size()),
            static_cast<int32_t>(hs.mMaxVerticesPerStrand),    static_cast<int32_t>(hs.mMaterials.size()),
            static_cast<int32_t>(hs.mNeutralDensity.size()),   r->scalpJoints};
        for (int32_t i = 0; i < nI && i < AVER_PHYS_HAIRI_COUNT; ++i) outI[i] = v[i];
    }
    if (outF) {
        float v[AVER_PHYS_HAIRF_COUNT];
        v[AVER_PHYS_HAIRF_ROOT_TO_SCALP_CM] = r->rootToScalpCm;
        for (int k = 0; k < 6; ++k) v[AVER_PHYS_HAIRF_BOUNDS_MIN_X + k] = r->boundsCm[k];
        for (int32_t i = 0; i < nF && i < AVER_PHYS_HAIRF_COUNT; ++i) outF[i] = v[i];
    }
    return succeed();
}

int32_t aver_phys_hair_settings_render_strands(int32_t settings, int32_t* outStartCount2, int32_t maxStrands) {
    HairGlobals& g = hairGlobals();
    std::lock_guard<std::mutex> lock(g.mutex);
    HairSettingsRec* r = settingsOf(g, settings);
    if (!r) return 0;
    const int32_t total = static_cast<int32_t>(r->renderTable.size() / 2);
    if (outStartCount2)
        for (int32_t i = 0; i < total && i < maxStrands; ++i) {
            outStartCount2[i * 2] = r->renderTable[static_cast<size_t>(i) * 2];
            outStartCount2[i * 2 + 1] = r->renderTable[static_cast<size_t>(i) * 2 + 1];
        }
    setAbiError(AbiError::Ok);
    return total;
}

} // extern "C"
