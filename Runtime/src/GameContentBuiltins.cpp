#include "GameContentInternal.hpp"

namespace aver::game {

#if AVER_MODULE_SCENE

namespace {

using rhi::primitives::appendBoxYaw;
using rhi::primitives::appendCylinderZ;

// Placeholder quadcopter: body, four arms with motor pods and rotors, landing skids with struts.
// Normalised like the unit cube/sphere: nothing goes past 1.0 from origin. Per-axis reach varies.
// Matched vertex-for-vertex with SandboxApp.cpp's appendDrone.
void appendDrone(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx) {
    // Central body: squarish box.
    constexpr f32 kBodyHX = 0.26f, kBodyHY = 0.26f, kBodyHZ = 0.15f;
    appendBoxYaw(v, idx, 0, 0, 0, kBodyHX, kBodyHY, kBodyHZ, 0.0f);

    // Four arms out to corners, ending in motor pods and rotor discs.
    constexpr f32 kArmAngleDeg[4] = {45.0f, 135.0f, 225.0f, 315.0f};
    constexpr f32 kArmRInner = 0.34f;
    constexpr f32 kArmROuter = 0.80f;
    constexpr f32 kArmHalfLen = (kArmROuter - kArmRInner) * 0.5f;
    constexpr f32 kArmCenterR = (kArmROuter + kArmRInner) * 0.5f;
    constexpr f32 kArmHalfWidth = 0.045f, kArmHalfThick = 0.032f;
    constexpr f32 kHubRadius = 0.11f, kHubHalfHeight = 0.05f, kHubCenterZ = 0.08f;
    constexpr f32 kDiscRadius = 0.20f, kDiscHalfHeight = 0.014f, kDiscCenterZ = 0.14f;
    for (f32 deg : kArmAngleDeg) {
        const f32 rad = deg * kDegToRad;
        const f32 armX = kArmCenterR * std::cos(rad), armY = kArmCenterR * std::sin(rad);
        appendBoxYaw(v, idx, armX, armY, 0.0f, kArmHalfLen, kArmHalfWidth, kArmHalfThick, deg);

        const f32 hubX = kArmROuter * std::cos(rad), hubY = kArmROuter * std::sin(rad);
        appendCylinderZ(v, idx, hubX, hubY, kHubCenterZ, kHubRadius, kHubHalfHeight, 8);
        // Rotor disc stands in for the swept area of a spinning prop; flat approximation.
        appendCylinderZ(v, idx, hubX, hubY, kDiscCenterZ, kDiscRadius, kDiscHalfHeight, 10);
    }

    // Landing skids and struts.
    constexpr f32 kSkidHalfLen = 0.30f, kSkidHalfWidth = 0.02f, kSkidHalfThick = 0.018f;
    constexpr f32 kSkidY = 0.20f, kSkidZ = -0.20f;
    appendBoxYaw(v, idx, 0.0f,  kSkidY, kSkidZ, kSkidHalfLen, kSkidHalfWidth, kSkidHalfThick, 0.0f);
    appendBoxYaw(v, idx, 0.0f, -kSkidY, kSkidZ, kSkidHalfLen, kSkidHalfWidth, kSkidHalfThick, 0.0f);

    constexpr f32 kStrutHalfX = 0.02f, kStrutHalfY = 0.02f, kStrutHalfZ = 0.016f;
    constexpr f32 kStrutX = 0.16f, kStrutZ = -0.166f;
    for (f32 sx : {-kStrutX, kStrutX})
        for (f32 sy : {-kSkidY, kSkidY})
            appendBoxYaw(v, idx, sx, sy, kStrutZ, kStrutHalfX, kStrutHalfY, kStrutHalfZ, 0.0f);
}

} // namespace

void GameContent::registerBuiltins(rhi::IDevice& device) {
    // Unit cube half-extent stays 1 (frozen): PLACEG scale is half-extent in cm on this mesh.
    // Bounds recorded here; SandboxApp does not. Degenerate bounds cause frustum cull exemption.
    const std::pair<Vec3, Vec3> unitBounds{Vec3{-1.0f, -1.0f, -1.0f}, Vec3{1.0f, 1.0f, 1.0f}};
    const auto add = [&](const std::string& path, const std::vector<rhi::MeshVertex>& v,
                         const std::vector<u32>& i, const std::pair<Vec3, Vec3>& bounds) {
        const u64 id = fnv1a64(std::string_view(path));
        const rhi::MeshHandle h = device.createMesh(v.data(), (u32)v.size(), i.data(), (u32)i.size());
        sceneMeshes_[id] = h;
        meshBounds_[id]  = bounds;
        if (meshLoaded_) meshLoaded_(LoadedMesh{id, path, nullptr, v, i, h}, meshLoadedUser_);
    };
    {
        std::vector<rhi::MeshVertex> v; std::vector<u32> i;
        appendSphere(v, i, 1.0f, 24, 48);
        add("Meshes/sphere.ocmesh", v, i, unitBounds);
    }
    {
        std::vector<rhi::MeshVertex> v; std::vector<u32> i;
        appendBox(v, i, 0, 0, 0, 1.0f);
        add("Meshes/cube.ocmesh", v, i, unitBounds);
    }
    {
        // Quadcopter for the graph-driven drone actor (replaces bare unit cube).
        std::vector<rhi::MeshVertex> v; std::vector<u32> i;
        appendDrone(v, i);
        // Not isotropic: exact per-axis reach differs. Bounds padded beyond exact numbers.
        add("Meshes/drone.ocmesh", v, i, {Vec3{-0.78f, -0.78f, -0.22f}, Vec3{0.78f, 0.78f, 0.16f}});
    }
    // Plane, cylinder, cone, capsule, torus, tube, pyramid, wedge, banana (aver/rhi/PrimitiveMeshes.hpp, shared with the actor preview).
    for (const rhi::primitives::BuiltinPrimitive& p : rhi::primitives::kBuiltinPrimitives) {
        std::vector<rhi::MeshVertex> v; std::vector<u32> i;
        p.build(v, i);
        f32 mn[3], mx[3];
        rhi::primitives::primitiveBounds(v, mn, mx);
        add(p.path, v, i, {Vec3{mn[0], mn[1], mn[2]}, Vec3{mx[0], mx[1], mx[2]}});
    }

    // Named surfaces gameplay can ask for. Must stay in step with SandboxApp.cpp's look()/surfaceLooks_.
    auto look = [this](const char* name, f32 r, f32 g, f32 b, f32 metal, f32 rough) {
        surfaceLooks_[aver_scene_material(0, name)] = SurfaceLook{{r, g, b}, metal, rough};
    };
    look("M_Floor",  0.22f, 0.23f, 0.26f, 0.02f, 0.85f);
    look("M_Wall",   0.48f, 0.50f, 0.55f, 0.03f, 0.72f);
    // Generic surfaces in the engine's default palette.
    look("M_Concrete", 0.55f, 0.54f, 0.51f, 0.00f, 0.88f);
    look("M_Trim",   0.30f, 0.33f, 0.38f, 0.35f, 0.45f);
    look("M_Crate",  0.62f, 0.44f, 0.22f, 0.02f, 0.78f);
    look("M_Target", 0.86f, 0.20f, 0.16f, 0.05f, 0.40f);
    look("M_Metal",  0.55f, 0.57f, 0.60f, 0.85f, 0.28f);
    look("M_Accent", 0.95f, 0.66f, 0.15f, 0.30f, 0.35f);
    // Outdoor vocabulary.
    look("M_Foliage", 0.16f, 0.42f, 0.14f, 0.00f, 0.85f);
    look("M_Bark",    0.35f, 0.24f, 0.15f, 0.00f, 0.85f);
    look("M_Rock",    0.42f, 0.40f, 0.37f, 0.05f, 0.80f);
    // M_Glass: opaque fallback (SurfaceLook has no alphaMode; actual translucency needs authored .ocmat).
    look("M_Glass", 0.92f, 0.94f, 0.95f, 0.00f, 0.05f);
    look("M_Banana", 0.90f, 0.68f, 0.10f, 0.00f, 0.55f);   // Meshes/banana.ocmesh's look in the editor's Add menu

    AVER_INFO("[Mesh] {} built-in primitive(s), {} named surface(s)", sceneMeshes_.size(), surfaceLooks_.size());
}

#endif // AVER_MODULE_SCENE

} // namespace aver::game
