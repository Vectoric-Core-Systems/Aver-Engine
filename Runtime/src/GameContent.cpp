#include "aver/game/GameContent.hpp"
#include "aver/core/Types.hpp"   // AVER_PATH_SEP

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/HitchMarks.hpp"
#include "aver/core/SurfaceId.hpp"

#include <algorithm>
#include <unordered_set>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <mutex>
#include <system_error>
#include <thread>

// Always included: AssetType/assetTypeFromPath are used by both loadProjectMeshes (scene-guarded)
// and loadProjectParticleEffects (particles-guarded); scoping to either guard breaks the other.
#include "aver/assets/AssetId.hpp"
#include "aver/rhi/PrimitiveMeshes.hpp"
#include "aver/platform/FileSystem.hpp"   // toNativeSeparators

#if AVER_MODULE_PBR
#  include "aver/assets/TextureAtlas.hpp"
#  include "aver/assets/TextureUpload.hpp"
#  include "aver/formats/OcGraph.hpp"
#  include "aver/formats/OcMat.hpp"
#  include "aver/pbr/MaterialGraphRegistry.hpp"
#endif

#if AVER_MODULE_SCENE
#  include "aver/anim/AnimSystem.hpp"
#  include "aver/formats/OcMesh.hpp"
#  include "aver/scene/scene_abi.h"
#  if AVER_MODULE_FRAMEWORK
#    include "aver/framework/framework_rtmesh_abi.h"   // G4 R7: script-built meshes
#  endif
#  if AVER_MODULE_PHYSICS
#    include "aver/physics/physics_abi.h"
#    include "aver/physics/physics_shape_builder_abi.h"   // aver_phys_shape_mesh: a collision shape with a surface palette
#    include "aver/world/PhysicsShapeCache.hpp"
#  endif
#  include "GameMath.hpp"
#  if AVER_MODULE_TRIFACTOR
#    include "aver/trifactor/ClusterAdapt.hpp"
#  endif
// Vendored meshoptimizer include, exposed by third_party/meshoptimizer CMakeLists.txt.
#  include <meshoptimizer.h>
#endif

#if AVER_MODULE_PARTICLES
#  include "aver/formats/OcParticle.hpp"
#  include "aver/particles/ParticleEffectLibrary.hpp"
#  include "aver/particles/ParticleRenderer.hpp"   // ParticleMeshData
#endif

namespace aver::game {

#if AVER_MODULE_PBR
namespace {
// Slot decides colour space: normal maps sRGB-decoded give subtle shading bugs. Layer1 slots too:
// Layer1BaseColor is sRGB, Layer1Normal is NormalMap.
assets::TextureUsage textureUsageFor(pbr::TextureSlot slot) {
    switch (slot) {
        case pbr::TextureSlot::BaseColor:
        case pbr::TextureSlot::Layer1BaseColor:
        case pbr::TextureSlot::Emissive:      return assets::TextureUsage::Colour;
        case pbr::TextureSlot::Normal:
        case pbr::TextureSlot::Layer1Normal:  return assets::TextureUsage::NormalMap;
        default:                              return assets::TextureUsage::Data;
    }
}
} // namespace
#endif

void GameContent::adopt(const fmt::ProjectDesc& project) {
    project_ = project;
    contentIndex_.clear();
#if AVER_MODULE_SCENE
    // A project switch is the one place lazy refcounts are dropped (a reload keeps them).
    meshRefs_.clear();
    lazyRel_.clear();
#endif

    const std::string content = project_.contentDir();
    if (content.empty()) return;

    std::error_code ec;
    if (!std::filesystem::exists(content, ec)) {
        AVER_WARN("[Content] the project's content root does not exist: {}", content);
        return;
    }

    // Use error_code overload to stop on unreadable subdirectory, not throw.
    for (std::filesystem::recursive_directory_iterator it(content, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string rel = std::filesystem::relative(it->path(), content, ec).string();
        if (ec || rel.empty()) continue;
        // IDs hash forward-slash spelling, matching C# Assets.ObjectIdOf, so packaging is nearly free.
        for (char& c : rel) if (c == '\\') c = '/';
        contentIndex_[fnv1a64(std::string_view(rel))] = it->path().string();
    }
    AVER_INFO("[Content] indexed {} asset(s) under {}", contentIndex_.size(), content);

#if AVER_MODULE_SCENE
    // Animation system caches by id; re-index must clear its cache.
    anim::animSystem().clear();
    anim::animSystem().setResolver(&GameContent::resolveAnimAsset, this);
#endif
}

std::string GameContent::pathFor(u64 id) const {
    const auto it = contentIndex_.find(id);
    return it == contentIndex_.end() ? std::string() : it->second;
}

std::vector<std::string> GameContent::pathsWithExtension(std::string_view ext) const {
    std::vector<std::string> out;
    for (const auto& [id, path] : contentIndex_) {
        if (path.size() < ext.size()) continue;
        // Case-insensitive suffix compare (Windows paths are case-insensitive but string compare is not).
        bool match = true;
        for (usize i = 0; i < ext.size(); ++i) {
            char a = path[path.size() - ext.size() + i];
            char b = ext[i];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            if (a != b) { match = false; break; }
        }
        if (match) out.push_back(path);
    }
    // Sort for reproducible order (unordered_map iteration is not stable).
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::pair<u64, std::string>> GameContent::idsWithExtension(std::string_view ext) const {
    std::vector<std::pair<u64, std::string>> out;
    for (const auto& [id, path] : contentIndex_) {
        if (path.size() < ext.size()) continue;
        bool match = true;
        for (usize i = 0; i < ext.size() && match; ++i) {
            char a = path[path.size() - ext.size() + i];
            char b = ext[i];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            match = a == b;
        }
        if (match) out.emplace_back(id, path);
    }
    std::sort(out.begin(), out.end(), [](const auto& l, const auto& r) { return l.second < r.second; });
    return out;
}

std::string GameContent::resolveAnimAsset(u64 id, void* user) {
    auto* self = static_cast<GameContent*>(user);
    return self ? self->pathFor(id) : std::string();
}

#if AVER_MODULE_SCENE

namespace {

// Axis-aligned box with independent per-axis half-extents, yawed around Z, placed at (cx,cy,cz).
// Face table and winding copied from appendBox.
void appendBoxYaw(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx,
                   f32 cx, f32 cy, f32 cz, f32 hx, f32 hy, f32 hz, f32 yawDeg) {
    const f32 rad = yawDeg * kDegToRad;
    const f32 cs = std::cos(rad), sn = std::sin(rad);
    // Rotation around Z preserves winding; every face stays CCW-outward.
    auto rotZ = [cs, sn](f32 lx, f32 ly, f32 lz, f32& ox, f32& oy, f32& oz) {
        ox = lx * cs - ly * sn; oy = lx * sn + ly * cs; oz = lz;
    };
    const f32 p[8][3] = {{-hx,-hy,-hz},{hx,-hy,-hz},{hx,hy,-hz},{-hx,hy,-hz},
                         {-hx,-hy,hz},{hx,-hy,hz},{hx,hy,hz},{-hx,hy,hz}};
    struct Face { f32 n[3]; int c[4]; };
    const Face faces[6] = {{{1,0,0},{1,2,6,5}},{{-1,0,0},{0,4,7,3}},{{0,1,0},{3,7,6,2}},
                           {{0,-1,0},{0,1,5,4}},{{0,0,1},{4,5,6,7}},{{0,0,-1},{0,3,2,1}}};
    const f32 quadUV[4][2] = {{0,0},{1,0},{1,1},{0,1}};
    for (const Face& f : faces) {
        f32 nx = 0.0f, ny = 0.0f, nz = 0.0f; rotZ(f.n[0], f.n[1], f.n[2], nx, ny, nz);
        const u32 b = static_cast<u32>(v.size());
        for (int k = 0; k < 4; ++k) {
            const f32* c = p[f.c[k]];
            f32 wx = 0.0f, wy = 0.0f, wz = 0.0f; rotZ(c[0], c[1], c[2], wx, wy, wz);
            v.push_back({cx+wx, cy+wy, cz+wz, nx, ny, nz, quadUV[k][0], quadUV[k][1]});
        }
        idx.push_back(b); idx.push_back(b+1); idx.push_back(b+2);
        idx.push_back(b); idx.push_back(b+2); idx.push_back(b+3);
    }
}

// Capped cylinder standing along +Z, centred at (cx,cy,cz), flat-shaded per face like appendBox.
// A second copy of SandboxApp.cpp's own appendCylinderZ for file-ownership reasons.
void appendCylinderZ(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx,
                      f32 cx, f32 cy, f32 cz, f32 radius, f32 halfHeight, u32 segments) {
    for (u32 s = 0; s < segments; ++s) {
        const f32 a0 = kTwoPi * static_cast<f32>(s) / static_cast<f32>(segments);
        const f32 a1 = kTwoPi * static_cast<f32>(s + 1) / static_cast<f32>(segments);
        const f32 x0 = std::cos(a0), y0 = std::sin(a0);
        const f32 x1 = std::cos(a1), y1 = std::sin(a1);
        // Side quad: one flat normal per face, computed as the average radial direction.
        f32 nx = x0 + x1, ny = y0 + y1;
        const f32 nl = std::sqrt(nx * nx + ny * ny);
        if (nl > 1e-6f) { nx /= nl; ny /= nl; }
        const u32 b = static_cast<u32>(v.size());
        v.push_back({cx + x0 * radius, cy + y0 * radius, cz - halfHeight, nx, ny, 0, 0, 0});
        v.push_back({cx + x1 * radius, cy + y1 * radius, cz - halfHeight, nx, ny, 0, 1, 0});
        v.push_back({cx + x1 * radius, cy + y1 * radius, cz + halfHeight, nx, ny, 0, 1, 1});
        v.push_back({cx + x0 * radius, cy + y0 * radius, cz + halfHeight, nx, ny, 0, 0, 1});
        idx.push_back(b); idx.push_back(b+1); idx.push_back(b+2);
        idx.push_back(b); idx.push_back(b+2); idx.push_back(b+3);
        // Top and bottom caps as fan triangles per segment; flat-shaded.
        const u32 ct = static_cast<u32>(v.size());
        v.push_back({cx, cy, cz + halfHeight, 0, 0, 1, 0.5f, 0.5f});
        v.push_back({cx + x0 * radius, cy + y0 * radius, cz + halfHeight, 0, 0, 1, x0*0.5f+0.5f, y0*0.5f+0.5f});
        v.push_back({cx + x1 * radius, cy + y1 * radius, cz + halfHeight, 0, 0, 1, x1*0.5f+0.5f, y1*0.5f+0.5f});
        idx.push_back(ct); idx.push_back(ct+1); idx.push_back(ct+2);
        const u32 cb = static_cast<u32>(v.size());
        v.push_back({cx, cy, cz - halfHeight, 0, 0, -1, 0.5f, 0.5f});
        v.push_back({cx + x1 * radius, cy + y1 * radius, cz - halfHeight, 0, 0, -1, x1*0.5f+0.5f, y1*0.5f+0.5f});
        v.push_back({cx + x0 * radius, cy + y0 * radius, cz - halfHeight, 0, 0, -1, x0*0.5f+0.5f, y0*0.5f+0.5f});
        idx.push_back(cb); idx.push_back(cb+1); idx.push_back(cb+2);
    }
}

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

namespace {

// Lowercased, forward-slash, no trailing slash: how lazy folders and relative paths are compared.
std::string lazyKey(std::string s) {
    for (char& c : s) { if (c == '\\') c = '/'; c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    while (s.size() >= 2 && s[0] == '.' && s[1] == '/') s.erase(0, 2);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

// The `lazy=` folders from the STREAM header record of every Content/Maps/*.ocworld. Header lines only:
// stops at the first PLACE/PLACEG/CHILD record.
std::vector<std::string> collectLazyFolders(const std::string& contentDir) {
    std::vector<std::string> out;
    std::error_code ec;
    const std::filesystem::path maps = std::filesystem::path(contentDir) / "Maps";
    if (!std::filesystem::is_directory(maps, ec)) return out;
    for (std::filesystem::directory_iterator it(maps, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string ext = it->path().extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".ocworld") continue;
        std::ifstream in(it->path());
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("PLACE", 0) == 0 || line.rfind("CHILD", 0) == 0) break;
            if (line.rfind("STREAM", 0) != 0) continue;
            usize p = 6;
            while (p < line.size()) {
                while (p < line.size() && (line[p] == ' ' || line[p] == '\t' || line[p] == '\r')) ++p;
                const usize s = p;
                while (p < line.size() && line[p] != ' ' && line[p] != '\t' && line[p] != '\r') ++p;
                const std::string_view tok(line.data() + s, p - s);
                if (tok.rfind("lazy=", 0) != 0) continue;
                usize q = 5;
                while (q <= tok.size()) {
                    usize e = tok.find(',', q);
                    if (e == std::string_view::npos) e = tok.size();
                    std::string dir = lazyKey(std::string(tok.substr(q, e - q)));
                    if (!dir.empty() && std::find(out.begin(), out.end(), dir) == out.end()) out.push_back(std::move(dir));
                    q = e + 1;
                }
            }
        }
    }
    return out;
}

} // namespace

bool GameContent::loadOneMesh(rhi::IDevice& device, u64 id, const std::string& full, const std::string& rel) {
    fmt::OcMeshData md;
    std::string why;
    if (!fmt::loadOcMesh(full, md, &why)) { AVER_WARN("[Mesh] {}", why); return false; }
    return uploadMesh(device, id, md, rel);
}

// Thread-free: touches no table and no device, so prefetch workers run it.
void GameContent::prepareMeshCpu(const fmt::OcMeshData& md, const std::string& rel, MeshCpu& out) {
    // Position, normal, uv only. rhi::MeshVertex has nowhere to put joints/weights.
    std::vector<rhi::MeshVertex>& verts = out.verts;
    verts.resize(md.vertexCount());
    for (u32 i = 0; i < md.vertexCount(); ++i) {
        rhi::MeshVertex& v = verts[i];
        v.px = md.positions[usize(i)*3+0]; v.py = md.positions[usize(i)*3+1]; v.pz = md.positions[usize(i)*3+2];
        v.nx = md.normals[usize(i)*3+0];   v.ny = md.normals[usize(i)*3+1];   v.nz = md.normals[usize(i)*3+2];
        v.u  = md.uvs[usize(i)*2+0];       v.v  = md.uvs[usize(i)*2+1];
    }
    // R1: the second vertex stream, for a static mesh that has any of colour, tangent or UV1 (a skinned mesh's vertices are written by compute).
    out.extra.clear();
    if (!md.hasSkin() && md.hasExtra()) {
        out.extra.resize(md.vertexCount());
        const bool c = md.hasColor(), t = md.hasTangent(), u = md.hasUv1();
        for (u32 i = 0; i < md.vertexCount(); ++i)
            out.extra[i] = rhi::packMeshVertexExtra(c ? &md.colors[usize(i)*4] : nullptr, t ? &md.tangents[usize(i)*4] : nullptr,
                                                    u ? &md.uvs1[usize(i)*2] : nullptr);
    }
    // Per-material split: each part an index slice over these vertices (see buildMeshParts).
    out.parts.clear();
    if (md.submeshes.size() <= 1) return;   // common case: nothing to split
    const bool keepBaseIndices = md.hasSkin();
    for (const fmt::OcMeshSubmesh& sm : md.submeshes) {
        if (sm.indexCount == 0) continue;
        const usize end = usize(sm.indexStart) + sm.indexCount;
        if (end > md.indices.size()) {
            AVER_WARN("[Mesh] '{}' submesh '{}' runs past the index buffer; skipped", rel, sm.name);
            continue;
        }
        PartCpu part;
        bool bad = false;
        for (usize k = sm.indexStart; k < end && !bad; ++k) bad = md.indices[k] >= verts.size();
        if (bad) {
            AVER_WARN("[Mesh] '{}' submesh '{}' indexes a vertex it does not have; skipped", rel, sm.name);
            continue;
        }
        part.indices.assign(md.indices.data() + sm.indexStart, md.indices.data() + end);
        part.slot = sm.materialSlot;
        part.name = sm.name;
        // baseIndices[i] is the slice parts[i] was cut from (lockstep).
        if (keepBaseIndices) part.baseIndices = part.indices;
        out.parts.push_back(std::move(part));
    }
}

bool GameContent::uploadMesh(rhi::IDevice& device, u64 id, const fmt::OcMeshData& md, const std::string& rel,
                             MeshCpu* cpu) {
    HitchMarks hm(rel.c_str(), 0.25);
    MeshCpu local;
    if (!cpu) { prepareMeshCpu(md, rel, local); cpu = &local; }
    hm.mark("upload:prepare");
    const std::vector<rhi::MeshVertex>& verts = cpu->verts;
    const bool staged = cpu->staged != nullptr;
    rhi::MeshHandle h = staged ? device.adoptMesh(std::move(cpu->staged)) : 0;
    if (hitchVerbose() && !h && meshStager_) AVER_INFO("[Mesh] {} not staged ({})", rel, staged ? "adopt refused" : "none");
    if (!h) h = device.createMesh(verts.data(), (u32)verts.size(), md.indices.data(), (u32)md.indices.size());
    hm.mark("upload:createMesh");
    if (!h) { AVER_WARN("[Mesh] the device refused '{}'", rel); return false; }
    // R1: attached before any part or LOD shares this mesh's vertices, so they borrow the stream. Refused = the mesh draws without it.
    if (!cpu->extra.empty() && !device.setMeshExtra(h, cpu->extra.data(), static_cast<u32>(cpu->extra.size())))
        AVER_WARN("[Mesh] '{}': the device kept no vertex colour / tangent / UV1 stream; it draws without them", rel);

    sceneMeshes_[id] = h;
    meshBounds_[id] = {md.boundsMin, md.boundsMax};
    // Mesh's own material slot; see GameContent.hpp::meshDefaultMaterial.
    if (!md.materialSlots.empty() && !md.materialSlots[0].empty()) {
        meshSlot0Material_[id] = aver_scene_material(0, md.materialSlots[0].c_str());
        meshSlot0Name_[id] = md.materialSlots[0];
    }
    // The per-submesh split: a mesh naming several materials draws one part per material.
    buildMeshParts(device, id, md, h, verts, cpu->extra, cpu->parts, rel);
    hm.mark("upload:parts");
    projectMeshIds_.push_back(id);
    if (meshLoaded_) meshLoaded_(LoadedMesh{id, rel, &md, verts, md.indices, h}, meshLoadedUser_);
    if (lazyLoading_ && meshAcquired_)
        meshAcquired_(LoadedMesh{id, rel, &md, verts, md.indices, h, &cpu->lodStaged}, meshAcquiredUser_);
    hm.mark("upload:hooks");

    // Coarse LOD for shadow/GI/voxelise passes (depth-only). 20cm target error (one shadow texel).
#if AVER_MODULE_TRIFACTOR
    if (buildDepthProxies_ && md.lodCount() > 1) {
        constexpr f32 kShadowErrorCm = 20.0f;
        u32 pick = 0;
        for (u32 lvl = 1; lvl < md.lodCount(); ++lvl)
            if (trifactor::levelWorldErrorCm(md, lvl) <= kShadowErrorCm) pick = lvl;

        // Only upload if it's actually cheaper than LOD 0.
        const u32 tris0 = trifactor::levelTriangleCount(md, 0);
        if (pick > 0 && trifactor::levelTriangleCount(md, pick) < tris0) {
            const fmt::OcMeshLod& lod = md.coarserLods[pick - 1];
            // LOD 0's OWN vertex array: a coarser level owns its index buffer but shares the one
            // VTXS block (see OcMeshData::coarserLods), which is why `verts` is correct here.
            rhi::MeshHandle ph = device.createMeshSharingVertices(h, lod.indices.data(), (u32)lod.indices.size());
            if (!ph) ph = device.createMesh(verts.data(), (u32)verts.size(), lod.indices.data(), (u32)lod.indices.size());
            if (ph) {
                depthProxyMap_[h] = ph;
                AVER_INFO("[Mesh] '{}' depth proxy: LOD {} ({} tris, {:.1f}x less than LOD 0, {:.1f}cm error)",
                          rel, pick, trifactor::levelTriangleCount(md, pick),
                          static_cast<f64>(tris0) /
                              static_cast<f64>(trifactor::levelTriangleCount(md, pick)),
                          trifactor::levelWorldErrorCm(md, pick));
            } else {
                AVER_WARN("[Mesh] '{}' depth proxy LOD {} refused by the device; it draws at full detail",
                          rel, pick);
            }
        }
    }
#endif
    return true;
}

void GameContent::loadProjectMeshes(rhi::IDevice& device) {
    const std::string dir = project_.contentDir();
    if (dir.empty()) return;
    meshStager_ = device.meshStager();
    AVER_INFO("[Mesh] streamed meshes' GPU buffers are made {}",
              meshStager_ ? "on the prefetch workers" : "on the main thread (no stager on this device)");
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return;

    const std::vector<std::string> lazyDirs = collectLazyFolders(dir);
    lazyRel_.clear();   // rebuilt by the scan below; refcounts stay

    u32 loaded = 0, failed = 0, lazy = 0;
    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string full = it->path().string();
        if (assetTypeFromPath(full) != AssetType::Mesh) continue;

        std::string rel = std::filesystem::relative(it->path(), dir, ec).string();
        if (ec) continue;
        for (char& c : rel) if (c == '\\') c = '/';

        const u64 id = fnv1a64(std::string_view(rel));
        if (!lazyDirs.empty()) {
            const std::string key = lazyKey(rel);
            bool isLazy = false;
            for (const std::string& d : lazyDirs)
                if (key.size() > d.size() && key.compare(0, d.size(), d) == 0 && key[d.size()] == '/') { isLazy = true; break; }
            if (isLazy) {
                contentIndex_[id] = full;
                lazyRel_[id] = rel;
                ++lazy;
                continue;
            }
            lazyRel_.erase(id);   // stale entry from an earlier load: this mesh is eager now
        }

        if (loadOneMesh(device, id, full, rel)) ++loaded; else ++failed;
    }
    // A reload keeps refcounts: re-upload lazy meshes still held by resident entities/foliage.
    for (const auto& [id, refs] : meshRefs_) {
        if (refs == 0 || sceneMeshes_.count(id)) continue;
        const auto lit = lazyRel_.find(id);
        const auto pit = contentIndex_.find(id);
        if (lit == lazyRel_.end() || pit == contentIndex_.end() || !loadOneMesh(device, id, pit->second, lit->second)) {
            ++failed;
            AVER_WARN("[Mesh] held lazy mesh {} could not be re-uploaded after reload", id);
        } else ++loaded;
    }
    if (loaded || failed || lazy)
        AVER_INFO("[Mesh] {} project mesh(es) loaded from {}{}{}", loaded, dir,
                  failed ? (", " + std::to_string(failed) + " failed") : "",
                  lazy ? (", " + std::to_string(lazy) + " indexed for on-demand load") : "");
}

bool GameContent::acquireMesh(rhi::IDevice& device, u64 id) {
    const auto lit = lazyRel_.find(id);
    if (lit == lazyRel_.end()) return sceneMeshes_.count(id) != 0;   // eager: permanently loaded
    u32& refs = meshRefs_[id];
    if (refs == 0 && sceneMeshes_.count(id) == 0) {
        const auto pit = contentIndex_.find(id);
        lazyLoading_ = true;
        bool ok = false;
        if (pit != contentIndex_.end()) {
            fmt::OcMeshData md;
            MeshCpu cpu;
            std::unique_ptr<CollisionMesh> collision;
            i32 shape = 0;
            std::string why;
            const int got = takePrefetch(id, md, cpu, collision, shape, why);
            if (got < 0) {
                if (hitchVerbose())
                    AVER_INFO("[Mesh] {} read on the main thread ({}, {})", lit->second,
                              got == -2 ? "its prefetch was still reading" : "never prefetched",
                              acquireTag ? acquireTag : "other");
                ok = loadOneMesh(device, id, pit->second, lit->second);
            } else if (got == 0) {
                AVER_WARN("[Mesh] {}", why);
            } else {
                ok = uploadMesh(device, id, md, lit->second, &cpu);
                if (ok && collision && !collisionMeshCache_.count(id)) collisionMeshCache_[id] = std::move(collision);
                if (ok && shape) { readyShapes_[id] = shape; shape = 0; }
            }
#  if AVER_MODULE_PHYSICS
            if (shape) aver_phys_release_mesh_shape(shape);
#  endif
        }
        lazyLoading_ = false;
        if (!ok) {
            meshRefs_.erase(id);
            return false;
        }
    }
    ++refs;
    return true;
}

void GameContent::releaseMesh(rhi::IDevice& device, u64 id) {
    (void)device;
    if (!lazyRel_.count(id)) return;
    const auto rit = meshRefs_.find(id);
    if (rit == meshRefs_.end() || rit->second == 0) return;
    if (--rit->second > 0) return;
    meshRefs_.erase(rit);
    unloadToPending(id);
}

bool GameContent::meshLoaded(u64 id) const { return sceneMeshes_.count(id) != 0; }

// G4 R7: script-built meshes (framework_rtmesh_abi.h). They go through uploadMesh like a streamed mesh (lazyLoading_ makes the acquired hook fire, which is what
// builds the ray-traced geometry for a mesh that appears after load), and an update replaces the mesh: the old handles wait in the release queue.
u32 GameContent::syncRuntimeMeshes(rhi::IDevice& device) {
    u32 uploaded = 0;
#if AVER_MODULE_FRAMEWORK
    aver_fw_rtmesh_pump();
    u64 removed[32];
    while (const i32 n = aver_fw_rtmesh_take_removed(removed, 32)) {
        for (i32 i = 0; i < n; ++i)
            if (sceneMeshes_.count(removed[i])) unloadToPending(removed[i]);
    }
    i32 ids[32];
    while (const i32 n = aver_fw_rtmesh_take_dirty(ids, 32)) {
        for (i32 k = 0; k < n; ++k) {
            const u64 oid = aver_fw_rtmesh_object_id(ids[k]);
            if (oid == 0) continue;
            AverFwRtMeshView v{};
            if (!aver_fw_rtmesh_view(ids[k], &v)) {   // emptied: nothing to draw
                if (sceneMeshes_.count(oid)) unloadToPending(oid);
                continue;
            }
            fmt::OcMeshData md;
            const usize vc = static_cast<usize>(v.vertexCount);
            md.positions.assign(v.pos, v.pos + vc * 3);
            md.normals.assign(v.nrm, v.nrm + vc * 3);
            md.uvs.assign(v.uv, v.uv + vc * 2);
            if (v.col) md.colors.assign(v.col, v.col + vc * 4);
            if (v.uv1) md.uvs1.assign(v.uv1, v.uv1 + vc * 2);
            if (v.tan) md.tangents.assign(v.tan, v.tan + vc * 4);
            md.indices.assign(v.idx, v.idx + v.indexCount);
            for (i32 s = 0; s < v.submeshCount; ++s) {
                const std::string mat = v.subMaterial[s] ? v.subMaterial[s] : "M_Default";
                u32 slot = 0;
                for (; slot < md.materialSlots.size(); ++slot) if (md.materialSlots[slot] == mat) break;
                if (slot == md.materialSlots.size()) md.materialSlots.push_back(mat);
                fmt::OcMeshSubmesh sm;
                sm.name = mat;
                sm.materialSlot = slot;
                sm.indexStart = static_cast<u32>(v.subStart[s]);
                sm.indexCount = static_cast<u32>(v.subCount[s]);
                sm.baseVertex = 0;
                sm.vertexCount = static_cast<u32>(vc);
                md.submeshes.push_back(std::move(sm));
            }
            md.computeBounds();
            if (!md.valid()) { AVER_WARN("[RuntimeMesh] id {} is not a valid mesh; not uploaded", ids[k]); continue; }
            char key[128] = {};
            aver_fw_rtmesh_key(ids[k], key, sizeof key);
            const std::string rel = std::string(AVER_FW_RTMESH_PATH_PREFIX) + key;
            if (sceneMeshes_.count(oid)) unloadToPending(oid);
            lazyLoading_ = true;
            if (uploadMesh(device, oid, md, rel)) ++uploaded;
            lazyLoading_ = false;
        }
    }
#else
    (void)device;
#endif
    return uploaded;
}

// Forgets every table entry for `id` at once (nothing can draw it) and queues its handles; the GPU
// frees wait in flushMeshReleases because D3D12Device::destroyMesh frees the mesh's own resources now.
void GameContent::unloadToPending(u64 id) {
    PendingMesh p;
    p.releasedAt = lastFlushFrame_;
    for (auto it = posedParts_.begin(); it != posedParts_.end();) {
        if (it->second.meshId == id) {
            for (const MeshPart& q : it->second.parts) if (q.mesh) p.others.push_back(q.mesh);
            it = posedParts_.erase(it);
        } else ++it;
    }
    meshPartBaseIndices_.erase(id);
    if (const auto pit = meshParts_.find(id); pit != meshParts_.end()) {
        for (const MeshPart& q : pit->second) if (q.mesh) p.others.push_back(q.mesh);
        meshParts_.erase(pit);
    }
    if (const auto sit = sceneMeshes_.find(id); sit != sceneMeshes_.end()) {
        if (const auto dit = depthProxyMap_.find(sit->second); dit != depthProxyMap_.end()) {
            if (dit->second) p.others.push_back(dit->second);
            depthProxyMap_.erase(dit);
        }
        p.base = sit->second;
        sceneMeshes_.erase(sit);
    }
    meshBounds_.erase(id);
    meshSlot0Material_.erase(id);
    meshSlot0Name_.erase(id);
    collisionMeshCache_.erase(id);
#  if AVER_MODULE_PBR
    if (const auto mn = meshMaterialNames_.find(id); mn != meshMaterialNames_.end()) {
        for (const std::string& name : mn->second)
            if (const auto u = materialUse_.find(name); u != materialUse_.end() && u->second.meshes && !--u->second.meshes)
                u->second.idleSince = streamTick_;
        meshMaterialNames_.erase(mn);
    }
#  endif
    if (const auto rs = readyShapes_.find(id); rs != readyShapes_.end()) {
#  if AVER_MODULE_PHYSICS
        aver_phys_release_mesh_shape(rs->second);
#  endif
        readyShapes_.erase(rs);
    }
    projectMeshIds_.erase(std::remove(projectMeshIds_.begin(), projectMeshIds_.end(), id), projectMeshIds_.end());
    pendingMeshes_.push_back(std::move(p));
    if (meshReleased_) meshReleased_(id, meshReleasedUser_);
}

void GameContent::releaseMeshHandle(rhi::MeshHandle h) {
    if (!h) return;
    // Into the entry the released hook's own mesh just made, whose `others` go before its base.
    if (pendingMeshes_.empty() || pendingMeshes_.back().releasedAt != lastFlushFrame_) {
        PendingMesh p;
        p.releasedAt = lastFlushFrame_;
        pendingMeshes_.push_back(std::move(p));
    }
    pendingMeshes_.back().others.push_back(h);
}

void GameContent::flushMeshReleases(rhi::IDevice& device, u64 frameIndex) {
    lastFlushFrame_ = frameIndex;
    usize w = 0;
    for (usize i = 0; i < pendingMeshes_.size(); ++i) {
        PendingMesh& p = pendingMeshes_[i];
        if (frameIndex < p.releasedAt + 3) {
            if (w != i) pendingMeshes_[w] = std::move(p);
            ++w;
            continue;
        }
        // destroyMesh refuses while shared buffers are still referenced: keep those and retry.
        usize kept = 0;
        for (const rhi::MeshHandle m : p.others)
            if (!device.destroyMesh(m)) p.others[kept++] = m;
        p.others.resize(kept);
        if (p.base && device.destroyMesh(p.base)) p.base = 0;
        if (p.others.empty() && !p.base) continue;
        if (!p.warned) {
            p.warned = true;
            AVER_WARN("[Mesh] a released mesh could not be destroyed yet (shared buffers still referenced); retrying");
        }
        if (w != i) pendingMeshes_[w] = std::move(p);
        ++w;
    }
    pendingMeshes_.resize(w);
}

// Splits a mesh that names more than one material into one MeshHandle per slot. Ported from
// SandboxApp::buildMeshParts (sandbox/src/SandboxAssets.cpp) -- see that function's own comment for
// why splitting at load time, rather than drawing per-submesh RANGES, is what makes this tractable at
// all (the ray path's BLAS carries one materialIndex per instance, so a range draw would still shade
// flat in the renderer that is actually on screen).
//
// SHARING THE PARENT'S VERTICES: each part owns only its index slice (createMeshSharingVertices), so a
// multi-material mesh holds its vertices once (compacted copies doubled them, in VRAM since meshes moved to
// the Default heap). Parts take the parent's bounds. A device that refuses the share gets the whole vertex
// array per part, which is correct, just larger.
void GameContent::buildMeshParts(rhi::IDevice& device, u64 id, const fmt::OcMeshData& md, rhi::MeshHandle whole,
                                  const std::vector<rhi::MeshVertex>& verts, const std::vector<rhi::MeshVertexExtra>& extra,
                                  std::vector<PartCpu>& cpuParts, const std::string& rel) {
    if (cpuParts.empty()) return;
    std::vector<MeshPart> parts;
    parts.reserve(cpuParts.size());
    const bool keepBaseIndices = md.hasSkin();
    std::vector<std::vector<u32>> baseIndices;
    for (PartCpu& pc : cpuParts) {
        MeshPart part;
        part.mesh = pc.staged ? device.adoptMeshSharingVertices(whole, std::move(pc.staged)) : 0;
        if (!part.mesh)
            part.mesh = device.createMeshSharingVertices(whole, pc.indices.data(), static_cast<u32>(pc.indices.size()));
        if (!part.mesh) {
            part.mesh = device.createMesh(verts.data(), static_cast<u32>(verts.size()),
                                          pc.indices.data(), static_cast<u32>(pc.indices.size()));
            // The device refused the share, so this part owns a whole vertex copy and needs its own second stream.
            if (part.mesh && !extra.empty()) device.setMeshExtra(part.mesh, extra.data(), static_cast<u32>(extra.size()));
        }
        if (!part.mesh) {
            AVER_WARN("[Mesh] the device refused submesh '{}' of '{}'", pc.name, rel);
            continue;
        }
        // Slot names the material, resolving through the same path as an authored material.
        if (pc.slot < md.materialSlots.size()) {
            const std::string& slot = md.materialSlots[pc.slot];
            if (!slot.empty()) part.material = aver_scene_material(0, slot.c_str());
        }
        parts.push_back(part);
        if (keepBaseIndices) baseIndices.push_back(std::move(pc.baseIndices));
    }

    // One surviving part is not a split; falling through costs one less draw call.
    if (parts.size() <= 1) {
        for (const MeshPart& p : parts) if (p.mesh) device.destroyMesh(p.mesh);
        return;
    }
    if (!lazyLoading_)
        AVER_INFO("[Mesh] '{}' names {} materials; split into {} part(s) so each draws its own",
                  rel, md.materialSlots.size(), parts.size());
    meshParts_[id] = std::move(parts);
    if (keepBaseIndices) meshPartBaseIndices_[id] = std::move(baseIndices);
}

const std::vector<GameContent::MeshPart>* GameContent::partsFor(u64 id) const {
    const auto it = meshParts_.find(id);
    return it == meshParts_.end() ? nullptr : &it->second;
}

const std::vector<GameContent::MeshPart>* GameContent::posedPartsFor(rhi::IDevice& device, u64 id,
        rhi::MeshHandle baseMesh, rhi::MeshHandle posedMesh) {
    if (!posedMesh || !baseMesh || posedMesh == baseMesh) return nullptr;
    if (const auto it = posedParts_.find(posedMesh); it != posedParts_.end())
        return (it->second.meshId == id && !it->second.parts.empty()) ? &it->second.parts : nullptr;
    PosedParts& entry = posedParts_[posedMesh];   // cache the refusal so it is not retried per frame
    entry.meshId = id;
    const auto pit = meshParts_.find(id);
    const auto bit = meshPartBaseIndices_.find(id);
    // Refusal (cached per posed handle): multi-material character draws whole under one material.
    if (pit == meshParts_.end() || bit == meshPartBaseIndices_.end() ||
        bit->second.size() != pit->second.size()) {
        if (pit != meshParts_.end())   // no parts at all is a single-material mesh: nothing to say
            AVER_WARN("[Mesh] posed split skipped for mesh {} (posed handle {}): its per-slot index "
                      "slices were not kept; it draws as one mesh", id, posedMesh);
        return nullptr;
    }
    // Skin target shares source's index buffer verbatim, so equal index buffers and vertex counts
    // mean it was cut from baseMesh. After mesh reload they differ; entity keeps single draw.
    rhi::BufferHandle baseIb = 0, posedIb = 0;
    u32 baseVc = 0, posedVc = 0;
    if (!device.meshGeometry(baseMesh, nullptr, &baseIb, &baseVc, nullptr) ||
        !device.meshGeometry(posedMesh, nullptr, &posedIb, &posedVc, nullptr) ||
        baseIb == 0 || baseIb != posedIb || baseVc != posedVc) {
        AVER_WARN("[Mesh] posed split skipped for mesh {}: posed handle {} was not cut from this "
                  "upload (base {}; reloaded since it was skinned?); it draws as one mesh",
                  id, posedMesh, baseMesh);
        return nullptr;
    }
    std::vector<MeshPart> out;
    out.reserve(pit->second.size());
    for (usize i = 0; i < pit->second.size(); ++i) {
        MeshPart pp;
        pp.material = pit->second[i].material;
        const std::vector<u32>& idx = bit->second[i];
        if (pit->second[i].mesh && !idx.empty()) {
            pp.mesh = device.createPosedPartMesh(posedMesh, idx.data(), static_cast<u32>(idx.size()));
            // All or nothing: half-split character silently loses failed parts; whole-mesh fallback draws all.
            if (!pp.mesh) {
                for (const MeshPart& q : out) if (q.mesh) device.destroyMesh(q.mesh);
                AVER_WARN("[Mesh] posed split refused for mesh {} (posed handle {}); it draws as one mesh",
                          id, posedMesh);
                return nullptr;
            }
        }
        out.push_back(pp);
    }
    entry.parts = std::move(out);
    AVER_INFO("[Mesh] mesh {}: {} posed part(s) over posed handle {}", id, entry.parts.size(), posedMesh);
    return &entry.parts;
}

void GameContent::registerMesh(u64 id, rhi::MeshHandle handle, const std::pair<Vec3, Vec3>& bounds) {
    sceneMeshes_[id] = handle;
    meshBounds_[id] = bounds;
}

void GameContent::releaseProjectMeshes(rhi::IDevice& device, bool destroyBaseHandles) {
    dropPrefetches();
    // Posed parts hold a vertex share on a skin target (which would refuse its own destruction).
    for (auto& kv : posedParts_)
        for (const MeshPart& p : kv.second.parts)
            if (p.mesh) device.destroyMesh(p.mesh);
    posedParts_.clear();
    for (const u64 id : projectMeshIds_) {
        meshPartBaseIndices_.erase(id);
        // Split parts are this class's uploads; nothing else keys on them.
        if (const auto pit = meshParts_.find(id); pit != meshParts_.end()) {
            for (const MeshPart& p : pit->second)
                if (p.mesh) device.destroyMesh(p.mesh);
            meshParts_.erase(pit);
        }
        if (const auto sit = sceneMeshes_.find(id); sit != sceneMeshes_.end()) {
            // Depth proxy is keyed on the base handle.
            if (const auto dit = depthProxyMap_.find(sit->second); dit != depthProxyMap_.end()) {
                if (dit->second) device.destroyMesh(dit->second);
                depthProxyMap_.erase(dit);
            }
            if (destroyBaseHandles && sit->second) device.destroyMesh(sit->second);
            sceneMeshes_.erase(sit);
        }
        meshBounds_.erase(id);
        meshSlot0Material_.erase(id);
        meshSlot0Name_.erase(id);
        collisionMeshCache_.erase(id);
    }
    projectMeshIds_.clear();
    // Lazy meshes already released but still waiting out their frames: nothing draws them, free now.
    for (const PendingMesh& p : pendingMeshes_) {
        for (const rhi::MeshHandle m : p.others) device.destroyMesh(m);
        if (destroyBaseHandles && p.base) device.destroyMesh(p.base);
    }
    pendingMeshes_.clear();
}

const std::pair<Vec3, Vec3>* GameContent::boundsFor(u64 id) const {
    const auto it = meshBounds_.find(id);
    return it == meshBounds_.end() ? nullptr : &it->second;
}

namespace {

// ---- collisionMeshFor's building blocks ------------------------------------------------------------

// Compacts indices down to only the vertices they reference, dropping degenerate triangles.
bool compactTriangles(const f32* srcPositions, u32 srcVertexCount,
                      const u32* srcIndices, usize srcIndexCount,
                      std::vector<f32>& outPositions, std::vector<u32>& outIndices) {
    outPositions.clear();
    outIndices.clear();
    std::unordered_map<u32, u32> remap;
    remap.reserve(srcIndexCount);
    outIndices.reserve(srcIndexCount);
    for (usize k = 0; k + 2 < srcIndexCount; k += 3) {
        const u32 ia = srcIndices[k], ib = srcIndices[k + 1], ic = srcIndices[k + 2];
        if (ia >= srcVertexCount || ib >= srcVertexCount || ic >= srcVertexCount) continue;
        if (ia == ib || ib == ic || ia == ic) continue;
        for (const u32 orig : {ia, ib, ic}) {
            const auto [it, inserted] = remap.try_emplace(orig, static_cast<u32>(outPositions.size() / 3));
            if (inserted) {
                outPositions.push_back(srcPositions[usize(orig) * 3 + 0]);
                outPositions.push_back(srcPositions[usize(orig) * 3 + 1]);
                outPositions.push_back(srcPositions[usize(orig) * 3 + 2]);
            }
            outIndices.push_back(it->second);
        }
    }
    return outIndices.size() >= 3;
}

// Vertex position's exact bit pattern for welding by position, not vertex index.
struct WeldKey {
    u32 xb, yb, zb;
    bool operator==(const WeldKey& o) const { return xb == o.xb && yb == o.yb && zb == o.zb; }
};
struct WeldKeyHash {
    usize operator()(const WeldKey& k) const {
        u64 h = kFnv1a64OffsetBasis;
        h = (h ^ k.xb) * kFnv1a64Prime;
        h = (h ^ k.yb) * kFnv1a64Prime;
        h = (h ^ k.zb) * kFnv1a64Prime;
        return static_cast<usize>(h);
    }
};

// Welds positions/indices (already compacted) by exact position.
// meshopt_simplify preserves topological borders; welding first lets it see the real topology.
void weldByPosition(const std::vector<f32>& positions, const std::vector<u32>& indices,
                    std::vector<f32>& outPositions, std::vector<u32>& outIndices) {
    const usize vertexCount = positions.size() / 3;
    std::unordered_map<WeldKey, u32, WeldKeyHash> weld;
    weld.reserve(vertexCount);
    std::vector<u32> remap(vertexCount);
    for (usize v = 0; v < vertexCount; ++v) {
        // Normalise ±0 to +0 (different IEEE 754 bit patterns).
        f32 x = positions[v * 3 + 0], y = positions[v * 3 + 1], z = positions[v * 3 + 2];
        if (x == 0.0f) x = 0.0f;
        if (y == 0.0f) y = 0.0f;
        if (z == 0.0f) z = 0.0f;
        WeldKey key;
        std::memcpy(&key.xb, &x, sizeof(u32));
        std::memcpy(&key.yb, &y, sizeof(u32));
        std::memcpy(&key.zb, &z, sizeof(u32));
        const auto [it, inserted] = weld.try_emplace(key, static_cast<u32>(outPositions.size() / 3));
        if (inserted) { outPositions.push_back(x); outPositions.push_back(y); outPositions.push_back(z); }
        remap[v] = it->second;
    }
    outIndices.resize(indices.size());
    for (usize k = 0; k < indices.size(); ++k) outIndices[k] = remap[indices[k]];
}

// Simplifies welded positions/indices toward ~2 cm world error, capped at 65536 triangles.
std::vector<u32> simplifyCollisionMesh(const std::vector<f32>& positions, const std::vector<u32>& indices,
                                       const std::string& meshPathForLog, f32& outErrorCm) {
    // ERRORABSOLUTE: error in the SAME units as positions (centimetres).
    constexpr f32 kTargetErrorCm = 2.0f;
    constexpr usize kTriangleCeiling = 65536;
    // LOCKBORDER: preserve genuine open edges and ones from material-slot filtering.
    constexpr unsigned kOptions =
        static_cast<unsigned>(meshopt_SimplifyLockBorder) | static_cast<unsigned>(meshopt_SimplifyErrorAbsolute);

    const usize targetIndexCount = std::min(indices.size(), kTriangleCeiling * 3);
    std::vector<u32> dest(indices.size());
    f32 resultError = 0.0f;
    usize resultCount = meshopt_simplify(dest.data(), indices.data(), indices.size(), positions.data(),
                                         positions.size() / 3, 3 * sizeof(f32), targetIndexCount,
                                         kTargetErrorCm, kOptions, &resultError);

    if (resultCount / 3 > kTriangleCeiling) {
        // 2 cm not enough; ceiling is the hard limit. Error is allowed to grow.
        resultCount = meshopt_simplify(dest.data(), indices.data(), indices.size(), positions.data(),
                                       positions.size() / 3, 3 * sizeof(f32), targetIndexCount,
                                       std::numeric_limits<f32>::max(), kOptions, &resultError);
        AVER_WARN("[Collision] {}: {} triangles would not fit the {}-triangle ceiling within {:.1f} cm "
                  "error; used {:.1f} cm instead", meshPathForLog, indices.size() / 3, kTriangleCeiling,
                  kTargetErrorCm, resultError);
    }

    dest.resize(resultCount);
    outErrorCm = resultError;
    return dest;
}

// ---- the disk cache: <project>/Saved/DerivedDataCache/Collision/<hash>.occol -------------------------
// Derived data (not authored). Whole directory can be deleted for cost of one rebuild per mesh.
// Bump kCollisionCacheVersion when filter rule, simplification target or layout changes.
// 2: the physical-surface palette and the per-slot signature follow the original fields (a version-1 file is a miss).
constexpr u32 kCollisionCacheMagic   = 0x4C4F4341u;
constexpr u32 kCollisionCacheVersion = 2;

// What a material slot says NOW: does it collide, and which physical surface does it carry (0 = none). An empty function is
// "not asked" (collides, no surface). The build uses it to filter and group; a cache read uses it to decide whether the entry
// still describes the mesh, since the answers live in .ocmat files the cache key does not cover.
struct SlotQuery {
    std::function<bool(const std::string&)> collides;
    std::function<i32(const std::string&)> surface;
};

std::string collisionCacheDir(const std::string& projectDir) {
    return projectDir.empty() ? std::string() : projectDir + AVER_PATH_SEP "Saved" AVER_PATH_SEP "DerivedDataCache" AVER_PATH_SEP "Collision";
}

// "<hash>.occol", FNV-1a hash of the key (mesh path, size, mtime, version).
std::string collisionCachePathFor(const std::string& projectDir, const std::string& meshPath,
                                  u64 fileSize, i64 mtimeTicks) {
    const std::string dir = collisionCacheDir(projectDir);
    if (dir.empty()) return std::string();
    const std::string key = meshPath + "|" + std::to_string(fileSize) + "|" +
                            std::to_string(mtimeTicks) + "|" + std::to_string(kCollisionCacheVersion);
    const u64 h = fnv1a64(std::string_view(key));
    char name[24];
    std::snprintf(name, sizeof(name), "%016llx.occol", static_cast<unsigned long long>(h));
    return dir + AVER_PATH_SEP + name;
}

// The physics shape cache's source key for a collision mesh: what the .occol key covers (path, size, mtime, version) plus what each
// material slot said (collides, surface), because those answers live in .ocmat files the file key does not cover and a slot flip that
// keeps the triangle count would otherwise reuse a stale BVH. world::createMeshShapeFromSource adds its own version and the counts.
u64 shapeSourceKey(const std::string& meshPath, u64 fileSize, i64 mtimeTicks, const GameContent::CollisionMesh& m) {
    std::string k = meshPath + "|" + std::to_string(fileSize) + "|" + std::to_string(mtimeTicks) + "|" +
                    std::to_string(kCollisionCacheVersion);
    for (const GameContent::CollisionMesh::SlotSig& s : m.slotSig)
        k += "|" + s.slot + (s.collides ? "+" : "-") + std::to_string(s.surface);
    return fnv1a64(std::string_view(k));
}
// Reads and validates cache entry against source mesh's current path/size/mtime.
bool readCollisionCacheFile(const std::string& cachePath, const std::string& meshPath, u64 fileSize,
                            i64 mtimeTicks, GameContent::CollisionMesh& out, const SlotQuery& slots) {
    std::ifstream in(cachePath, std::ios::binary);
    if (!in) return false;
    const auto get = [&in](void* p, usize n) { in.read(static_cast<char*>(p), static_cast<std::streamsize>(n)); return static_cast<bool>(in); };

    u32 magic = 0, version = 0;
    if (!get(&magic, sizeof(magic)) || magic != kCollisionCacheMagic) return false;
    if (!get(&version, sizeof(version)) || version != kCollisionCacheVersion) return false;

    u64 pathLen = 0;
    if (!get(&pathLen, sizeof(pathLen)) || pathLen == 0 || pathLen > (1ull << 20)) return false;
    std::string storedPath(static_cast<usize>(pathLen), '\0');
    if (!get(storedPath.data(), static_cast<usize>(pathLen)) || storedPath != meshPath) return false;

    u64 storedSize = 0;
    i64 storedMtime = 0;
    if (!get(&storedSize, sizeof(storedSize)) || storedSize != fileSize) return false;
    if (!get(&storedMtime, sizeof(storedMtime)) || storedMtime != mtimeTicks) return false;

    u32 vertexCount = 0, indexCount = 0;
    if (!get(&vertexCount, sizeof(vertexCount)) || !get(&indexCount, sizeof(indexCount))) return false;
    // Sanity check: a corrupt count is not a huge allocation.
    constexpr u32 kMaxReasonableVertices = 64u * 1024u * 1024u;
    if (vertexCount == 0 || vertexCount > kMaxReasonableVertices ||
        indexCount < 3 || indexCount > kMaxReasonableVertices * 3u) return false;

    out.positions.resize(usize(vertexCount) * 3);
    if (!get(out.positions.data(), out.positions.size() * sizeof(f32))) return false;
    out.indices.resize(indexCount);
    if (!get(out.indices.data(), out.indices.size() * sizeof(u32))) return false;
    if (!get(&out.lod, sizeof(out.lod)) || !get(&out.errorCm, sizeof(out.errorCm))) return false;

    // The physical-surface palette (empty for a mesh that names none) and one palette index per triangle.
    u32 paletteCount = 0, triCount = 0;
    if (!get(&paletteCount, sizeof(paletteCount)) || paletteCount > 32) return false;
    out.palette.resize(paletteCount);
    if (paletteCount && !get(out.palette.data(), usize(paletteCount) * sizeof(i32))) return false;
    if (!get(&triCount, sizeof(triCount))) return false;
    if ((triCount == 0) != (paletteCount == 0) || (triCount != 0 && triCount != indexCount / 3)) return false;
    out.triPalette.resize(triCount);
    if (triCount && !get(out.triPalette.data(), triCount)) return false;
    for (const u8 index : out.triPalette)
        if (index >= paletteCount) return false;   // corrupt

    // What the build saw of each material slot: the entry is stale if a slot's .ocmat says something else now.
    u32 sigCount = 0;
    if (!get(&sigCount, sizeof(sigCount)) || sigCount > 4096) return false;
    out.slotSig.resize(sigCount);
    for (GameContent::CollisionMesh::SlotSig& sig : out.slotSig) {
        u32 len = 0;
        u8 collides = 1;
        if (!get(&len, sizeof(len)) || len > 4096) return false;
        sig.slot.resize(len);
        if (len && !get(sig.slot.data(), len)) return false;
        if (!get(&collides, sizeof(collides)) || !get(&sig.surface, sizeof(sig.surface))) return false;
        sig.collides = collides != 0;
    }
    for (const GameContent::CollisionMesh::SlotSig& sig : out.slotSig) {
        if (slots.collides && slots.collides(sig.slot) != sig.collides) return false;
        if (slots.surface && slots.surface(sig.slot) != sig.surface) return false;
    }
    return true;
}

// Writes cache entry atomically: record goes to "<cachePath>.tmp" first; successful rename publishes it.
bool writeCollisionCacheFile(const std::string& cachePath, const std::string& meshPath, u64 fileSize,
                             i64 mtimeTicks, const GameContent::CollisionMesh& mesh, std::string* why) {
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(cachePath).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    const std::string tmpPath = cachePath + ".tmp";
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out) { if (why) *why = "could not open " + tmpPath; return false; }
        const auto put = [&out](const void* p, usize n) { out.write(static_cast<const char*>(p), static_cast<std::streamsize>(n)); };
        put(&kCollisionCacheMagic, sizeof(kCollisionCacheMagic));
        put(&kCollisionCacheVersion, sizeof(kCollisionCacheVersion));
        const u64 pathLen = meshPath.size();
        put(&pathLen, sizeof(pathLen));
        put(meshPath.data(), meshPath.size());
        put(&fileSize, sizeof(fileSize));
        put(&mtimeTicks, sizeof(mtimeTicks));
        const u32 vertexCount = static_cast<u32>(mesh.positions.size() / 3);
        const u32 indexCount = static_cast<u32>(mesh.indices.size());
        put(&vertexCount, sizeof(vertexCount));
        put(&indexCount, sizeof(indexCount));
        put(mesh.positions.data(), mesh.positions.size() * sizeof(f32));
        put(mesh.indices.data(), mesh.indices.size() * sizeof(u32));
        put(&mesh.lod, sizeof(mesh.lod));
        put(&mesh.errorCm, sizeof(mesh.errorCm));
        const u32 paletteCount = static_cast<u32>(mesh.palette.size());
        const u32 triCount = static_cast<u32>(mesh.triPalette.size());
        put(&paletteCount, sizeof(paletteCount));
        put(mesh.palette.data(), usize(paletteCount) * sizeof(i32));
        put(&triCount, sizeof(triCount));
        put(mesh.triPalette.data(), triCount);
        const u32 sigCount = static_cast<u32>(mesh.slotSig.size());
        put(&sigCount, sizeof(sigCount));
        for (const GameContent::CollisionMesh::SlotSig& sig : mesh.slotSig) {
            const u32 len = static_cast<u32>(sig.slot.size());
            const u8 collides = sig.collides ? 1 : 0;
            put(&len, sizeof(len));
            put(sig.slot.data(), len);
            put(&collides, sizeof(collides));
            put(&sig.surface, sizeof(sig.surface));
        }
        if (!out) { if (why) *why = "write failed for " + tmpPath; return false; }
    }
    std::filesystem::rename(tmpPath, cachePath, ec);
    if (ec) { if (why) *why = "could not publish " + cachePath + ": " + ec.message(); return false; }
    return true;
}

// One kept run of a mesh's index buffer and the physical surface its material slot names (0 = none).
struct SurfaceRun {
    i32 surface = 0;
    usize begin = 0, end = 0;   // [begin, end) into the mesh's LOD 0 indices
};

// The surface-carrying build (see buildCollisionMesh): triangles grouped by surface, each group welded and simplified on its
// own with its borders locked, the groups joined and welded once more across their seams.
std::unique_ptr<GameContent::CollisionMesh> buildSurfaceCollisionMesh(const fmt::OcMeshData& md, const std::string& path,
                                                                     const std::vector<SurfaceRun>& runs) {
    using CollisionMesh = GameContent::CollisionMesh;
    constexpr usize kMaxPalette = 32;   // Jolt packs a mesh triangle's material index in 5 bits

    // The distinct surfaces in first-appearance order. Past the 31st, a surface joins surface 0 so the palette stays at 32.
    std::vector<i32> distinct;
    for (const SurfaceRun& r : runs)
        if (std::find(distinct.begin(), distinct.end(), r.surface) == distinct.end()) distinct.push_back(r.surface);
    const bool overflow = distinct.size() > kMaxPalette;
    if (overflow)
        AVER_WARN("[Collision] {}: {} distinct physical surfaces, but a mesh holds {}; the rest are treated as no surface",
                  path, distinct.size(), kMaxPalette);
    const auto effective = [&](i32 surface) -> i32 {
        if (!overflow) return surface;
        const auto rank = std::find(distinct.begin(), distinct.end(), surface) - distinct.begin();
        return rank < static_cast<std::ptrdiff_t>(kMaxPalette - 1) ? surface : 0;
    };

    struct Group { i32 surface = 0; std::vector<u32> indices; };
    std::vector<Group> groups;
    for (const SurfaceRun& r : runs) {
        const i32 s = effective(r.surface);
        auto it = std::find_if(groups.begin(), groups.end(), [s](const Group& g) { return g.surface == s; });
        if (it == groups.end()) { groups.push_back(Group{s, {}}); it = groups.end() - 1; }
        it->indices.insert(it->indices.end(), md.indices.begin() + static_cast<std::ptrdiff_t>(r.begin),
                           md.indices.begin() + static_cast<std::ptrdiff_t>(r.end));
    }

    std::vector<f32> allPositions;
    std::vector<u32> allIndices;
    std::vector<u8> triPalette;
    std::vector<i32> palette;
    f32 worstErrorCm = 0.0f;
    for (const Group& g : groups) {
        std::vector<f32> compactPositions;
        std::vector<u32> compactIndices;
        if (!compactTriangles(md.positions.data(), md.vertexCount(), g.indices.data(), g.indices.size(),
                              compactPositions, compactIndices))
            continue;   // every triangle of this surface degenerate or out of range
        std::vector<f32> weldedPositions;
        std::vector<u32> weldedIndices;
        weldByPosition(compactPositions, compactIndices, weldedPositions, weldedIndices);

        f32 errorCm = 0.0f;
        const std::vector<u32> simplified = simplifyCollisionMesh(weldedPositions, weldedIndices, path, errorCm);
        std::vector<f32> groupPositions;
        std::vector<u32> groupIndices;
        if (!compactTriangles(weldedPositions.data(), static_cast<u32>(weldedPositions.size() / 3),
                              simplified.data(), simplified.size(), groupPositions, groupIndices)) {
            groupPositions = std::move(weldedPositions);   // simplification left nothing: keep the group as it was
            groupIndices = std::move(weldedIndices);
            errorCm = 0.0f;
        }

        const u8 paletteIndex = static_cast<u8>(palette.size());
        palette.push_back(g.surface);
        const u32 base = static_cast<u32>(allPositions.size() / 3);
        allPositions.insert(allPositions.end(), groupPositions.begin(), groupPositions.end());
        for (const u32 k : groupIndices) allIndices.push_back(base + k);
        triPalette.insert(triPalette.end(), groupIndices.size() / 3, paletteIndex);
        worstErrorCm = std::max(worstErrorCm, errorCm);
    }
    if (allIndices.size() < 3) return nullptr;

    // The vertices along a seam are separate copies after the per-group passes, at identical positions: one weld joins them.
    auto mesh = std::make_unique<CollisionMesh>();
    weldByPosition(allPositions, allIndices, mesh->positions, mesh->indices);
    mesh->lod = 0;
    mesh->errorCm = worstErrorCm;
    mesh->palette = std::move(palette);
    mesh->triPalette = std::move(triPalette);

    AVER_INFO("[Collision] {}: meshoptimizer, {} surface group(s) ({} tris from {} at LOD 0, {:.1f} cm error)", path,
              mesh->palette.size(), mesh->indices.size() / 3, md.indices.size() / 3, mesh->errorCm);
    return mesh;
}

// What the build saw of each (distinct, named) material slot, for the disk cache's check against the .ocmat files.
void fillSlotSig(GameContent::CollisionMesh& mesh, const fmt::OcMeshData& md, const SlotQuery& slots) {
    for (const std::string& name : md.materialSlots) {
        if (name.empty()) continue;
        const bool seen = std::any_of(mesh.slotSig.begin(), mesh.slotSig.end(),
                                      [&](const GameContent::CollisionMesh::SlotSig& s) { return s.slot == name; });
        if (seen) continue;
        GameContent::CollisionMesh::SlotSig sig;
        sig.slot = name;
        sig.collides = !slots.collides || slots.collides(name);
        sig.surface = slots.surface ? slots.surface(name) : 0;
        mesh.slotSig.push_back(std::move(sig));
    }
}

// Collision geometry for a parsed mesh: slots `collides` rejects are dropped, then the cheaper of a
// meshoptimizer simplification and a Trifactor LOD is kept. Touches no GameContent state (prefetch
// workers run it).
//
// A mesh whose kept slots NAME PHYSICAL SURFACES takes buildSurfaceCollisionMesh instead (groups per surface, Trifactor not
// used: its coarser levels know nothing of material slots). A mesh that names none is built exactly as it was before
// surfaces existed.
std::unique_ptr<GameContent::CollisionMesh> buildCollisionMesh(const fmt::OcMeshData& md, const std::string& path,
                                                              const SlotQuery& slots) {
    using CollisionMesh = GameContent::CollisionMesh;
    const u32 vertexCount = md.vertexCount();
    if (vertexCount == 0 || md.indices.size() < 3) return nullptr;

    // Drop cut-out/translucent material slots from LOD 0 submeshes.
    bool anyDropped = false, anySurface = false;
    std::vector<u32> keptIndices;
    std::vector<SurfaceRun> runs;
    if (md.submeshes.empty()) {
        keptIndices = md.indices;   // no submesh table: keep all
    } else {
        keptIndices.reserve(md.indices.size());
        u32 droppedSlots = 0;
        for (const fmt::OcMeshSubmesh& sm : md.submeshes) {
            if (sm.indexCount == 0) continue;
            const usize end = usize(sm.indexStart) + sm.indexCount;
            if (end > md.indices.size()) continue;   // malformed range
            const std::string slotName =
                sm.materialSlot < md.materialSlots.size() ? md.materialSlots[sm.materialSlot] : std::string();
            if (slots.collides && !slots.collides(slotName)) { anyDropped = true; ++droppedSlots; continue; }
            const i32 surface = slots.surface ? slots.surface(slotName) : 0;
            if (surface != 0) anySurface = true;
            runs.push_back(SurfaceRun{surface, usize(sm.indexStart), end});
            keptIndices.insert(keptIndices.end(),
                               md.indices.begin() + static_cast<std::ptrdiff_t>(sm.indexStart),
                               md.indices.begin() + static_cast<std::ptrdiff_t>(end));
        }
        if (anyDropped)
            AVER_INFO("[Collision] {}: {} of {} material slot(s) are alpha-masked or translucent; "
                      "excluded from collision", path, droppedSlots, md.submeshes.size());
    }
    if (keptIndices.empty()) {
        // Logged once per mesh id (cache prevents reruns).
        AVER_WARN("[Collision] {}: every material slot is alpha-masked or translucent; no collision "
                  "geometry", path);
        return nullptr;
    }

    if (anySurface) {
        auto surfaced = buildSurfaceCollisionMesh(md, path, runs);
        if (surfaced) fillSlotSig(*surfaced, md, slots);
        return surfaced;
    }

    // Compact: keep only referenced vertices, remove degenerate triangles.
    std::vector<f32> filteredPositions;
    std::vector<u32> filteredIndices;
    if (!compactTriangles(md.positions.data(), vertexCount, keptIndices.data(), keptIndices.size(),
                          filteredPositions, filteredIndices))
        return nullptr;   // all kept triangles degenerate or out of range

    // Weld by position, simplify with meshoptimizer toward ~2 cm.
    std::vector<f32> weldedPositions;
    std::vector<u32> weldedIndices;
    weldByPosition(filteredPositions, filteredIndices, weldedPositions, weldedIndices);

    f32 meshoptErrorCm = 0.0f;
    const std::vector<u32> simplified =
        simplifyCollisionMesh(weldedPositions, weldedIndices, path, meshoptErrorCm);
    std::vector<f32> meshoptPositions;
    std::vector<u32> meshoptIndices;
    const bool meshoptOk = compactTriangles(weldedPositions.data(),
                                            static_cast<u32>(weldedPositions.size() / 3),
                                            simplified.data(), simplified.size(),
                                            meshoptPositions, meshoptIndices);

    // Trifactor coarsest LOD only when nothing was filtered (no submesh table to filter through).
    u32 trifactorLod = 0;
    f32 trifactorErrorCm = 0.0f;
    std::vector<f32> trifactorPositions;
    std::vector<u32> trifactorIndices;
    bool haveTrifactor = false;
#if AVER_MODULE_TRIFACTOR
    if (!anyDropped) {
        constexpr f32 kCollisionMaxErrorCm = 2.0f;
        for (u32 lvl = 1; lvl < md.lodCount(); ++lvl) {
            const f32 err = trifactor::levelWorldErrorCm(md, lvl);
            if (err <= kCollisionMaxErrorCm) { trifactorLod = lvl; trifactorErrorCm = err; }
        }
        const std::vector<u32>& lodSrc =
            trifactorLod == 0 ? md.indices : md.coarserLods[trifactorLod - 1].indices;
        haveTrifactor = compactTriangles(md.positions.data(), vertexCount, lodSrc.data(), lodSrc.size(),
                                         trifactorPositions, trifactorIndices);
    }
#endif

    // Fewer triangles wins; a tie keeps Trifactor (no further cost).
    auto mesh = std::make_unique<CollisionMesh>();
    const bool useTrifactor =
        haveTrifactor && (!meshoptOk || trifactorIndices.size() <= meshoptIndices.size());
    const char* wonBy = "meshoptimizer";
    if (useTrifactor) {
        mesh->positions = std::move(trifactorPositions);
        mesh->indices = std::move(trifactorIndices);
        mesh->lod = trifactorLod;
        mesh->errorCm = trifactorErrorCm;
        wonBy = "Trifactor LOD";
    } else if (meshoptOk) {
        mesh->positions = std::move(meshoptPositions);
        mesh->indices = std::move(meshoptIndices);
        mesh->lod = 0;
        mesh->errorCm = meshoptErrorCm;
    } else {
        // Fallback: neither simplifier produced output; use filtered set before simplification.
        mesh->positions = std::move(filteredPositions);
        mesh->indices = std::move(filteredIndices);
        mesh->lod = 0;
        mesh->errorCm = 0.0f;
        wonBy = "unsimplified";
    }
    if (mesh->indices.size() < 3) return nullptr;

    fillSlotSig(*mesh, md, slots);
    AVER_INFO("[Collision] {}: {} ({} tris from {} at LOD 0, {:.1f} cm error)", path, wonBy,
              mesh->indices.size() / 3, md.indices.size() / 3, mesh->errorCm);
    return mesh;
}

#  if AVER_MODULE_PBR
// collisionSlotCollides without the material library: the same file lookup materialForSurface makes,
// parsed directly (thread-free).
bool slotCollidesFromFiles(const std::string& binariesDir, const std::string& contentDir, const std::string& name) {
    if (name.empty() || contentDir.empty()) return true;
    const std::string candidates[3] = {
        binariesDir + AVER_PATH_SEP "Materials" AVER_PATH_SEP + name + ".ocmat",
        contentDir + AVER_PATH_SEP "Materials" AVER_PATH_SEP + name + ".ocmat",
        contentDir + AVER_PATH_SEP + name,
    };
    for (const std::string& path : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) continue;
        pbr::MaterialDesc d;
        fmt::OcMatExtras extras;
        std::string err;
        if (!fmt::loadOcmat(path, d, &extras, &err)) return true;
        return d.alphaMode != pbr::AlphaMode::Mask && d.alphaMode != pbr::AlphaMode::Blend;
    }
    return true;
}

// The physical surface a material slot carries: its .ocmat PHYSSURFACE hashed to the id the physics registry and a raycast
// use, or 0 for a slot with no material file or no such record. The same file lookup as slotCollidesFromFiles (thread-free).
i32 slotSurfaceFromFiles(const std::string& binariesDir, const std::string& contentDir, const std::string& name) {
    if (name.empty() || contentDir.empty()) return 0;
    const std::string candidates[3] = {
        binariesDir + AVER_PATH_SEP "Materials" AVER_PATH_SEP + name + ".ocmat",
        contentDir + AVER_PATH_SEP "Materials" AVER_PATH_SEP + name + ".ocmat",
        contentDir + AVER_PATH_SEP + name,
    };
    for (const std::string& path : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) continue;
        pbr::MaterialDesc d;
        fmt::OcMatExtras extras;
        std::string err;
        if (!fmt::loadOcmat(path, d, &extras, &err)) return 0;
        return aver::surfaceIdFromName(extras.physSurface);
    }
    return 0;
}

// The texture files a surface material names, as resolveMaterialTexture would load them: paths under
// the content dir (content-index ids and terrain layers are left to the main thread).
std::vector<std::pair<std::string, assets::TextureUsage>> slotTexturesFromFiles(
        const std::string& binariesDir, const std::string& contentDir, const std::string& name) {
    std::vector<std::pair<std::string, assets::TextureUsage>> out;
    if (name.empty() || contentDir.empty()) return out;
    const std::string candidates[3] = {
        binariesDir + AVER_PATH_SEP "Materials" AVER_PATH_SEP + name + ".ocmat",
        contentDir + AVER_PATH_SEP "Materials" AVER_PATH_SEP + name + ".ocmat",
        contentDir + AVER_PATH_SEP + name,
    };
    for (const std::string& path : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) continue;
        pbr::MaterialDesc d;
        fmt::OcMatExtras extras;
        std::string err;
        if (!fmt::loadOcmat(path, d, &extras, &err) || d.terrain.enabled()) return out;
        for (u32 i = 0; i < pbr::kTextureSlotCount; ++i) {
            const std::string& p = d.textures[i].path;
            if (p.empty() || p.rfind("@rt:", 0) == 0) continue;
            const bool absolute = p.size() > 1 && (p[1] == ':' || p[0] == '\\' || p[0] == '/');
            std::string full = absolute ? p : contentDir + AVER_PATH_SEP + p;
            if (!absolute && !std::filesystem::exists(full, ec)) continue;
            out.emplace_back(std::move(full), textureUsageFor(static_cast<pbr::TextureSlot>(i)));
        }
        return out;
    }
    return out;
}
#  endif

} // namespace

// Two workers read .ocmesh files and collision caches; uploads stay on the main thread.
struct GameContent::MeshPrefetch {
    struct Job {
        std::string path, rel, projectDir, binariesDir, contentDir;
        bool collisionBuilt = false;   // made on the worker (not read from the disk cache)
        fmt::OcMeshData md;
        MeshCpu cpu;
        bool meshOk = false;
        std::string why;
        std::shared_ptr<rhi::IMeshStager> stager;   // null: the main thread creates the GPU mesh
        std::unique_ptr<CollisionMesh> collision;
        i32 shape = 0;   // released here unless taken
        std::atomic<bool> done{false};
        u64 order = 0;
        u64 touchedTick = 0;   // prefetchTick() count when last asked for
        f32 priority = 0.0f;   // guarded by MeshPrefetch::m while queued
        ~Job() {
#  if AVER_MODULE_PHYSICS
            if (shape) aver_phys_release_mesh_shape(shape);
#  endif
        }
    };
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::shared_ptr<Job>> queue;
    std::vector<std::thread> workers;
    bool stop = false;
    std::unordered_map<u64, std::shared_ptr<Job>> jobs;   // main thread only
    // Dropped reads are freed off the main thread: their staged upload buffers were up to 240 ms of a tick.
    std::vector<std::shared_ptr<Job>> dead;
    std::future<void> reaper;
    void drop(u64 id) {
        if (const auto it = jobs.find(id); it != jobs.end()) { dead.push_back(std::move(it->second)); jobs.erase(it); }
    }
    void reap() {
        if (dead.empty() || (reaper.valid() && reaper.wait_for(std::chrono::seconds(0)) != std::future_status::ready)) return;
        reaper = std::async(std::launch::async, [d = std::move(dead)]() mutable { d.clear(); });
        dead.clear();
    }
    u64 nextOrder = 0;
    u64 tick = 0;
    std::mutex slotMutex;
    std::unordered_map<std::string, bool> slotCollidesMemo;   // material slot -> collides (workers)
    std::unordered_map<std::string, i32> slotSurfaceMemo;     // material slot -> physical surface id (workers)
#  if AVER_MODULE_PBR
    std::unordered_map<std::string, std::vector<std::pair<std::string, assets::TextureUsage>>> slotTexturesMemo;

    std::vector<std::pair<std::string, assets::TextureUsage>> slotTextures(const Job& j, const std::string& slot) {
        {
            std::lock_guard<std::mutex> l(slotMutex);
            if (const auto it = slotTexturesMemo.find(slot); it != slotTexturesMemo.end()) return it->second;
        }
        auto t = slotTexturesFromFiles(j.binariesDir, j.contentDir, slot);
        if (t.empty()) return t;   // not remembered: the material may not be written yet
        std::lock_guard<std::mutex> l(slotMutex);
        return slotTexturesMemo.emplace(slot, std::move(t)).first->second;
    }
#  endif

    bool slotCollides(const Job& j, const std::string& slot) {
#  if AVER_MODULE_PBR
        {
            std::lock_guard<std::mutex> l(slotMutex);
            if (const auto it = slotCollidesMemo.find(slot); it != slotCollidesMemo.end()) return it->second;
        }
        const bool c = slotCollidesFromFiles(j.binariesDir, j.contentDir, slot);
        std::lock_guard<std::mutex> l(slotMutex);
        slotCollidesMemo.emplace(slot, c);
        return c;
#  else
        (void)j; (void)slot;
        return true;
#  endif
    }

    i32 slotSurface(const Job& j, const std::string& slot) {
#  if AVER_MODULE_PBR
        {
            std::lock_guard<std::mutex> l(slotMutex);
            if (const auto it = slotSurfaceMemo.find(slot); it != slotSurfaceMemo.end()) return it->second;
        }
        const i32 surface = slotSurfaceFromFiles(j.binariesDir, j.contentDir, slot);
        std::lock_guard<std::mutex> l(slotMutex);
        slotSurfaceMemo.emplace(slot, surface);
        return surface;
#  else
        (void)j; (void)slot;
        return 0;
#  endif
    }

    ~MeshPrefetch() {
        { std::lock_guard<std::mutex> l(m); stop = true; }
        cv.notify_all();
        for (std::thread& t : workers) t.join();
    }
    void push(std::shared_ptr<Job> j) {
        if (workers.empty())
            for (int i = 0; i < 3; ++i) workers.emplace_back([this] { run(); });
        { std::lock_guard<std::mutex> l(m); queue.push_back(std::move(j)); }
        cv.notify_one();
    }
    void run() {
        for (;;) {
            std::shared_ptr<Job> j;
            {
                std::unique_lock<std::mutex> l(m);
                cv.wait(l, [this] { return stop || !queue.empty(); });
                if (stop) return;
                // Nearest first: the queue holds what the streamer asked for over many ticks.
                auto best = queue.begin();
                for (auto it = queue.begin(); it != queue.end(); ++it)
                    if ((*it)->priority < (*best)->priority) best = it;
                j = std::move(*best);
                queue.erase(best);
            }
            j->meshOk = fmt::loadOcMesh(j->path, j->md, &j->why);
            if (j->meshOk) prepareMeshCpu(j->md, j->rel, j->cpu);
            // The GPU buffers too: allocating and filling a large mesh's was 25-75 ms on the main thread.
            if (j->meshOk && j->stager) {
                j->cpu.staged = j->stager->stage(j->cpu.verts.data(), static_cast<u32>(j->cpu.verts.size()),
                                                 j->md.indices.data(), static_cast<u32>(j->md.indices.size()));
                for (PartCpu& p : j->cpu.parts)
                    p.staged = j->stager->stageIndices(p.indices.data(), static_cast<u32>(p.indices.size()));
                for (const fmt::OcMeshLod& lod : j->md.coarserLods)
                    j->cpu.lodStaged.push_back(j->stager->stageIndices(lod.indices.data(),
                                                                       static_cast<u32>(lod.indices.size())));
            }
            if (j->meshOk && !j->projectDir.empty()) {
                std::error_code ec;
                const u64 size = static_cast<u64>(std::filesystem::file_size(j->path, ec));
                const i64 mtime = ec ? 0 : static_cast<i64>(
                    std::filesystem::last_write_time(j->path, ec).time_since_epoch().count());
                if (!ec) {
                    const std::string cachePath = collisionCachePathFor(j->projectDir, j->path, size, mtime);
                    auto c = std::make_unique<CollisionMesh>();
                    // What each material slot says (collides, physical surface): the build's input, and the check a cached
                    // entry must still pass.
                    SlotQuery slots;
                    slots.collides = [this, &j](const std::string& slot) { return slotCollides(*j, slot); };
                    slots.surface = [this, &j](const std::string& slot) { return slotSurface(*j, slot); };
                    if (!cachePath.empty() && readCollisionCacheFile(cachePath, j->path, size, mtime, *c, slots)) {
                        j->collision = std::move(c);
                        j->collision->shapeSource = shapeSourceKey(j->path, size, mtime, *j->collision);
                    } else {
                        // First visit: build it here and fill the disk cache.
                        j->collision = buildCollisionMesh(j->md, j->path, slots);
                        if (j->collision && !cachePath.empty()) {
                            std::string why;
                            if (!writeCollisionCacheFile(cachePath, j->path, size, mtime, *j->collision, &why))
                                AVER_WARN("[Collision] {}: could not write the disk cache: {}", j->path, why);
                        }
                        j->collisionBuilt = true;
                        if (j->collision) j->collision->shapeSource = shapeSourceKey(j->path, size, mtime, *j->collision);
                    }
                }
            }
#  if AVER_MODULE_PHYSICS
            // The mesh's shared collision shape (its BVH build is the costly part of a placement's body).
            if (j->collision && j->collision->indices.size() >= 3 && aver_phys_ready()) {
                const CollisionMesh& cm = *j->collision;
                // A mesh with physical surfaces becomes a shape with a palette, so a ray on any placement reports the slot's.
                j->shape = !cm.palette.empty() && cm.triPalette.size() == cm.indices.size() / 3
                    ? aver_phys_shape_mesh(cm.positions.data(), static_cast<i32>(cm.positions.size() / 3),
                                           reinterpret_cast<const i32*>(cm.indices.data()), static_cast<i32>(cm.indices.size()),
                                           cm.triPalette.data(), cm.palette.data(), static_cast<i32>(cm.palette.size()),
                                           nullptr, nullptr, 0)
                    : aver::world::createMeshShapeFromSource(cm.positions.data(), static_cast<i32>(cm.positions.size() / 3),
                                                             reinterpret_cast<const i32*>(cm.indices.data()), static_cast<i32>(cm.indices.size()),
                                                             aver::world::physicsShapeCacheDir(j->projectDir), cm.shapeSource);
            }
#  endif
#  if AVER_MODULE_PBR
            // Its materials' textures, so binding them on the main thread is a GPU copy, not a disk read.
            if (j->meshOk)
                for (const std::string& slot : j->md.materialSlots)
                    for (const auto& [path, usage] : slotTextures(*j, slot)) assets::prefetchTexture(path, usage);
#  endif
            j->done.store(true, std::memory_order_release);
        }
    }
};

void GameContent::prefetchMesh(u64 id, f32 priority) {
    if (!lazyRel_.count(id) || sceneMeshes_.count(id)) return;
    const auto pit = contentIndex_.find(id);
    if (pit == contentIndex_.end()) return;
    if (!prefetch_) prefetch_ = std::make_shared<MeshPrefetch>();
    MeshPrefetch& pf = *prefetch_;
    if (const auto it = pf.jobs.find(id); it != pf.jobs.end()) {
        it->second->order = ++pf.nextOrder;
        it->second->touchedTick = pf.tick;
        std::lock_guard<std::mutex> l(pf.m);
        it->second->priority = priority;
        return;
    }
    // Finished reads the viewer turned away from are dropped, oldest request first -- never one asked
    // for in the last two streaming ticks (a streamer may be about to take it).
    constexpr usize kMaxJobs = 1024;
    if (pf.jobs.size() >= kMaxJobs) {
        std::vector<std::pair<u64, u64>> finished;
        for (const auto& [jid, j] : pf.jobs)
            if (j->done.load(std::memory_order_acquire) && j->touchedTick + 2 < pf.tick)
                finished.emplace_back(j->order, jid);
        std::sort(finished.begin(), finished.end());
        for (usize i = 0; i < finished.size() && pf.jobs.size() > kMaxJobs * 3 / 4; ++i) pf.drop(finished[i].second);
        if (pf.jobs.size() >= kMaxJobs) {
            // Still full: finished reads farther than this request give way, farthest first, down to 7/8 of the
            // pool. A streamer loads a root only once ALL its meshes are read, so a pool full of finished
            // reads for half-read sets (a jump into dense content asks for many at once) would otherwise refuse
            // every remaining mesh forever and nothing would load again.
            std::lock_guard<std::mutex> l(pf.m);
            std::vector<std::pair<f32, u64>> far;
            for (const auto& [jid, j] : pf.jobs)
                if (j->done.load(std::memory_order_acquire) && j->priority > priority) far.emplace_back(j->priority, jid);
            std::sort(far.begin(), far.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            for (usize i = 0; i < far.size() && pf.jobs.size() > kMaxJobs * 7 / 8; ++i) pf.drop(far[i].second);
            if (pf.jobs.size() >= kMaxJobs) {
                // Then queued reads: the farthest gives way to a nearer one.
                auto q = pf.queue.end();
                for (auto it = pf.queue.begin(); it != pf.queue.end(); ++it)
                    if (q == pf.queue.end() || (*it)->priority > (*q)->priority) q = it;
                if (q == pf.queue.end() || (*q)->priority <= priority) return;
                for (auto it = pf.jobs.begin(); it != pf.jobs.end(); ++it)
                    if (it->second == *q) { pf.drop(it->first); break; }
                pf.queue.erase(q);
            }
        }
    }
    auto j = std::make_shared<MeshPrefetch::Job>();
    j->path = pit->second;
    j->rel = lazyRel_[id];
    j->projectDir = project_.dir;
    j->binariesDir = project_.binariesDir();
    j->contentDir = project_.contentDir();
    j->stager = meshStager_;
    j->order = ++pf.nextOrder;
    j->touchedTick = pf.tick;
    j->priority = priority;
    pf.jobs.emplace(id, j);
    pf.push(std::move(j));
}

void GameContent::prefetchTick() {
    HitchMarks hm("content tick", 0.25);
    if (++streamTick_ % 60 == 0) {
#  if AVER_MODULE_PBR
        releaseIdleMaterials();
        hm.mark("materials");
        assets::trimPrefetchedTextures();
        hm.mark("textures");
        if (hitchVerbose() && streamTick_ % 120 == 0)
            AVER_INFO("[Stream] held: {} mesh read(s), textures {} MB; {} material(s), {} mesh(es) loaded",
                      prefetch_ ? prefetch_->jobs.size() : 0, assets::prefetchedTextureBytes() >> 20,
                      materialAssets_.size(), sceneMeshes_.size());
#  endif
    }
    if (!prefetch_) return;
    MeshPrefetch& pf = *prefetch_;
    ++pf.tick;
    pf.reap();
    // Distance-ranked requests (priority > 0) nobody asked for in ~2 s are dropped: a queued one would keep
    // its old, now wrong distance and be read before what the viewer needs after a jump, and a finished one
    // would hold a pool slot forever. (Foliage re-asks on a timer, so not after two ticks.) Unranked ones
    // (streamers re-ask every tick while they want them) after ~10 s, so a cell flown past frees its reads.
    constexpr u64 kStaleTicks = 120, kUnrankedStaleTicks = 600;
    std::unordered_set<const MeshPrefetch::Job*> stale;
    for (const auto& [id, j] : pf.jobs)
        if (j->touchedTick + (j->priority > 0.0f ? kStaleTicks : kUnrankedStaleTicks) < pf.tick) stale.insert(j.get());
    if (stale.empty()) return;
    std::lock_guard<std::mutex> l(pf.m);
    std::unordered_set<const MeshPrefetch::Job*> queued;
    for (const auto& j : pf.queue) queued.insert(j.get());
    for (auto it = pf.jobs.begin(); it != pf.jobs.end();) {
        const MeshPrefetch::Job* j = it->second.get();   // one being read stays until it finishes
        if (stale.count(j) && (queued.count(j) || j->done.load(std::memory_order_acquire))) {
            pf.dead.push_back(std::move(it->second));
            it = pf.jobs.erase(it);
        } else ++it;
    }
    pf.queue.erase(std::remove_if(pf.queue.begin(), pf.queue.end(),
                                  [&](const std::shared_ptr<MeshPrefetch::Job>& j) { return stale.count(j.get()) != 0; }),
                   pf.queue.end());
    pf.reap();
    hm.mark("reads");
}

bool GameContent::meshReady(u64 id) const {
    if (!lazyRel_.count(id) || sceneMeshes_.count(id)) return true;
    if (!prefetch_) return false;
    const auto it = prefetch_->jobs.find(id);
    return it != prefetch_->jobs.end() && it->second->done.load(std::memory_order_acquire);
}

int GameContent::takePrefetch(u64 id, fmt::OcMeshData& md, MeshCpu& cpu, std::unique_ptr<CollisionMesh>& collision,
                              i32& shape, std::string& why) {
    if (!prefetch_) return -1;
    const auto it = prefetch_->jobs.find(id);
    if (it == prefetch_->jobs.end()) return -1;
    const std::shared_ptr<MeshPrefetch::Job> j = it->second;
    prefetch_->jobs.erase(it);   // a read still running finishes into its own job and is dropped
    if (!j->done.load(std::memory_order_acquire)) return -2;   // still reading: the caller reads it itself
    if (!j->meshOk) { why = std::move(j->why); return 0; }
    md = std::move(j->md);
    cpu = std::move(j->cpu);
    collision = std::move(j->collision);
    shape = j->shape;
    j->shape = 0;
    return 1;
}

i32 GameContent::takeMeshShape(u64 id) {
    const auto it = readyShapes_.find(id);
    if (it == readyShapes_.end()) return 0;
    const i32 h = it->second;
    readyShapes_.erase(it);
    return h;
}

void GameContent::dropPrefetches() {
    if (prefetch_) prefetch_->jobs.clear();
#  if AVER_MODULE_PHYSICS
    for (const auto& [mesh, shape] : readyShapes_) aver_phys_release_mesh_shape(shape);
#  endif
    readyShapes_.clear();
}

#if AVER_MODULE_PBR
// Material slot collision filter.
bool GameContent::collisionSlotCollides(const std::string& slotName) {
    if (slotName.empty()) return true;
    // A loaded material's desc, else its file: asking would load (and keep) a material nothing draws.
    const auto it = materialAssets_.find(slotName);
    if (it == materialAssets_.end()) return slotCollidesFromFiles(project_.binariesDir(), project_.contentDir(), slotName);
    const pbr::MaterialDesc* d = it->second ? pbr::MaterialLibrary::get().desc(it->second) : nullptr;
    if (!d) return true;   // no authored .ocmat: cannot resolve
    return d->alphaMode != pbr::AlphaMode::Mask && d->alphaMode != pbr::AlphaMode::Blend;
}

i32 GameContent::collisionSlotSurface(const std::string& slotName) {
    // The file, always: the loaded material desc carries no PHYSSURFACE (it is not a rendering property).
    return slotSurfaceFromFiles(project_.binariesDir(), project_.contentDir(), slotName);
}
#else
// Every slot collides (no PBR module), and none names a surface.
bool GameContent::collisionSlotCollides(const std::string&) { return true; }
i32 GameContent::collisionSlotSurface(const std::string&) { return 0; }
#endif

// Filters and simplifies collision geometry from meshes with multiple per-material submeshes.
const GameContent::CollisionMesh* GameContent::collisionMeshFor(u64 id) {
    if (const auto it = collisionMeshCache_.find(id); it != collisionMeshCache_.end())
        return it->second.get();

    // Cache null entries so failed lookups are O(1) on retry.
    std::unique_ptr<CollisionMesh>& slot = collisionMeshCache_[id];

    const std::string path = pathFor(id);
    if (path.empty()) return nullptr;   // built-in or unrecognized id

    // Disk cache: check source file size and mtime.
    std::error_code statEc;
    const u64 srcSize = static_cast<u64>(std::filesystem::file_size(path, statEc));
    const i64 srcMtime = statEc ? 0 : static_cast<i64>(
        std::filesystem::last_write_time(path, statEc).time_since_epoch().count());
    const bool canCache = !statEc && !project_.dir.empty();
    const std::string cachePath = canCache ? collisionCachePathFor(project_.dir, path, srcSize, srcMtime)
                                           : std::string();
    SlotQuery slots;
    slots.collides = [this](const std::string& slotName) { return collisionSlotCollides(slotName); };
    slots.surface = [this](const std::string& slotName) { return collisionSlotSurface(slotName); };
    if (!cachePath.empty()) {
        auto cached = std::make_unique<CollisionMesh>();
        if (readCollisionCacheFile(cachePath, path, srcSize, srcMtime, *cached, slots)) {
            ++collisionCacheHits_;
            AVER_INFO("[Collision] {}: {} tris from the disk cache", path, cached->indices.size() / 3);
            cached->shapeSource = shapeSourceKey(path, srcSize, srcMtime, *cached);
            slot = std::move(cached);
            return slot.get();
        }
    }

    fmt::OcMeshData md;
    std::string why;
    if (!fmt::loadOcMesh(path, md, &why)) {
        AVER_WARN("[Collision] {}", why);
        return nullptr;
    }
    auto mesh = buildCollisionMesh(md, path, slots);
    if (!mesh) return nullptr;

    if (!cachePath.empty()) {
        std::string saveWhy;
        if (!writeCollisionCacheFile(cachePath, path, srcSize, srcMtime, *mesh, &saveWhy))
            AVER_WARN("[Collision] {}: could not write the disk cache: {}", path, saveWhy);
    }

    if (canCache) mesh->shapeSource = shapeSourceKey(path, srcSize, srcMtime, *mesh);
    slot = std::move(mesh);
    return slot.get();
}

rhi::MeshHandle GameContent::resolveSceneMesh(u64 id, void* user) {
    auto* self = static_cast<GameContent*>(user);
    return self ? self->meshFor(id) : 0;
}

const GameContent::SurfaceLook* GameContent::lookFor(i32 material) const {
    const auto it = surfaceLooks_.find(material);
    return it == surfaceLooks_.end() ? nullptr : &it->second;
}

#endif // AVER_MODULE_SCENE

#if AVER_MODULE_PBR

std::string GameContent::resolveAssetPath(const pbr::TextureRef& ref) const {
    if (!ref.path.empty()) {
        const std::string& p = ref.path;
        const bool absolute = p.size() > 1 && (p[1] == ':' || p[0] == '\\' || p[0] == '/');
        if (absolute) return p;
        const std::string content = project_.contentDir();
        if (!content.empty()) {
            const std::string full = content + AVER_PATH_SEP + p;
            std::error_code ec;
            if (std::filesystem::exists(full, ec)) return full;
        }
        return p;
    }
    if (ref.id) return pathFor(ref.id);
    return {};
}

pbr::MaterialSystem::ResolvedTexture GameContent::resolveMaterialTexture(const pbr::TextureRef& ref,
                                                                         pbr::TextureSlot slot, void* user) {
    auto* self = static_cast<GameContent*>(user);
    if (!self || !self->textureFactory_) return {};

    const std::string path = self->resolveAssetPath(ref);
    if (path.empty()) {
        AVER_WARN("[Material] texture id 0x{:016X} is not in the content index; slot '{}' keeps its fallback",
                  ref.id, pbr::MaterialLibrary::textureSlotName(slot));
        return {};
    }

    const assets::TextureUsage usage = textureUsageFor(slot);

    std::string err;
    assets::TextureUploadInfo info;
    const rhi::TextureHandle h = assets::uploadTexture(*self->textureFactory_, path, usage, &err, &info);
    if (!h) {
        AVER_WARN("[Material] {} - slot '{}' keeps its fallback", err,
                  pbr::MaterialLibrary::textureSlotName(slot));
        return {};
    }
    AVER_INFO("[Material] {} -> {}x{}, {} mips ({} KB) for slot '{}'", path, info.width, info.height,
              info.mips, info.bytes / 1024, pbr::MaterialLibrary::textureSlotName(slot));
    // Mean in handle: needed for sampling fallbacks.
    pbr::MaterialSystem::ResolvedTexture out;
    out.handle = h;
    for (int c = 0; c < 3; ++c) out.averageLinear[c] = info.averageLinear[c];
    // Bytes: used for level-change eviction log.
    out.bytes = info.bytes;
    return out;
}

// ---- wave3 terrain layers ----
bool GameContent::resolveTerrainAtlas(const pbr::MaterialDesc& d, pbr::TerrainTextures& out, void* user) {
    auto* self = static_cast<GameContent*>(user);
    if (!self || !self->textureFactory_ || !d.terrain.enabled()) return false;
    const u32 count = static_cast<u32>(std::min<usize>(d.terrain.layers.size(), pbr::kMaxTerrainLayers));

    // Up to 4 columns: 8 layers are 4 x 2, fewer shrink the atlas to what they need.
    assets::AtlasSpec spec;
    spec.cols = std::min<u32>(count, 4u);
    spec.rows = (count + spec.cols - 1) / spec.cols;
    spec.tileTexels = d.terrain.tileTexels;
    spec.padTexels = d.terrain.padTexels;
    assets::AtlasSpec albedo = spec, normal = spec, orm = spec;
    albedo.usage = assets::TextureUsage::Colour;
    normal.usage = assets::TextureUsage::NormalMap;
    orm.usage = assets::TextureUsage::Data;
    for (u32 i = 0; i < count; ++i) {
        const pbr::TerrainLayerDesc& l = d.terrain.layers[i];
        assets::AtlasTile a, n, o;
        a.colorPath = self->resolveAssetPath(l.baseColor);
        a.alphaPath = self->resolveAssetPath(l.height);
        a.alphaFromLuma = a.alphaPath.empty();            // no height map: the albedo's luminance stands in
        n.colorPath = self->resolveAssetPath(l.normal);
        n.fill[0] = 0.5f; n.fill[1] = 0.5f; n.fill[2] = 1.0f; n.fill[3] = 1.0f;   // a flat tangent-space normal
        o.colorPath = self->resolveAssetPath(l.metalRough);
        o.fill[0] = 1.0f; o.fill[1] = 1.0f; o.fill[2] = 0.0f; o.fill[3] = 1.0f;   // full AO, rough, non-metal
        albedo.tiles.push_back(std::move(a));
        normal.tiles.push_back(std::move(n));
        orm.tiles.push_back(std::move(o));
    }

    assets::AtlasImage ia, in, io;
    std::string err;
    if (!assets::buildAtlasImage(albedo, ia, &err) || !assets::buildAtlasImage(normal, in, &err) ||
        !assets::buildAtlasImage(orm, io, &err)) {
        AVER_WARN("[Terrain] material '{}': the atlas could not be built: {}", d.name, err);
        return false;
    }
    const usize bytes = [&] { usize b = 0; for (const auto* im : {&ia, &in, &io}) for (const auto& l : im->levels) b += l.size(); return b; }();
    if (bytes > (160u << 20))
        AVER_WARN("[Terrain] material '{}': the atlases take {} MiB of GPU memory (tile {}); use 512 texel tiles to save memory",
                  d.name, bytes >> 20, d.terrain.tileTexels);

    out = pbr::TerrainTextures{};
    out.albedo = assets::uploadAtlas(*self->textureFactory_, ia, true, &err);
    out.normal = assets::uploadAtlas(*self->textureFactory_, in, false, &err);
    out.orm = assets::uploadAtlas(*self->textureFactory_, io, false, &err);
    if (!out.albedo || !out.normal || !out.orm) {
        AVER_WARN("[Terrain] material '{}': the atlas could not be uploaded: {}", d.name, err);
        for (rhi::TextureHandle t : {out.albedo, out.normal, out.orm}) if (t) self->textureFactory_->destroyTexture(t);
        out = pbr::TerrainTextures{};
        return false;
    }
    const u32 S = spec.tileTexels + 2 * spec.padTexels;
    out.cols = spec.cols; out.rows = spec.rows; out.stride = S; out.tile = spec.tileTexels; out.pad = spec.padTexels;
    out.atlasW = ia.width; out.atlasH = ia.height;
    out.bytes = bytes;
    for (u32 i = 0; i < count; ++i) {
        out.avg[i][0] = ia.tileAverage[i][0]; out.avg[i][1] = ia.tileAverage[i][1]; out.avg[i][2] = ia.tileAverage[i][2];
        out.avg[i][3] = io.tileAverage[i][1];   // ORM: G = roughness
        out.avg[i][4] = io.tileAverage[i][2];   // B = metallic
    }
    AVER_INFO("[Terrain] material '{}': {} layer(s) packed into {}x{} atlases ({} MiB)", d.name, count, ia.width, ia.height, bytes >> 20);
    return true;
}
// ---- end wave3 terrain layers ----

// .ocmat GRAPHREF path to gMaterialGraphId. Returns 0 for materials with no graph or load failure.
// Broken graph does not disable the material: falls back to stock factors/maps.
u32 GameContent::resolveMaterialGraph(const std::string& graphRef) const {
    if (graphRef.empty()) return 0;
    const std::string content = project_.contentDir();
    if (content.empty()) return 0;

    // Content-relative path (matches COMP mesh= and TEX conventions).
    std::string path = content + AVER_PATH_SEP + graphRef;
    toNativeSeparators(path);

    // Compile once: multiple materials can share a graph.
    if (const u32 known = pbr::materialGraphs().idOf(path)) return known;

    fmt::OcGraphData g;
    std::string err;
    if (!fmt::loadOcgraph(path, g, &err)) {
        AVER_ERROR("[MaterialGraph] '{}' could not be read, so the material shades as a stock "
                   "one: {}", path, err);
        return 0;
    }
    return pbr::materialGraphs().add(path, g.name, g);
}

pbr::MaterialHandle GameContent::materialForSurface(const std::string& name) {
    if (name.empty()) return 0;
    materialUse_[name].kept = true;
    return loadSurfaceMaterial(name);
}

pbr::MaterialHandle GameContent::loadSurfaceMaterial(const std::string& name) {
    if (name.empty()) return 0;
    const auto cached = materialAssets_.find(name);
    if (cached != materialAssets_.end()) return cached->second;

    pbr::MaterialHandle h = 0;
    const std::string content = project_.contentDir();
    if (!content.empty()) {
        // Built .ocmat (Binaries) wins over hand-authored (Content).
        const std::string candidates[3] = {
            project_.binariesDir() + AVER_PATH_SEP "Materials" AVER_PATH_SEP + name + ".ocmat",
            content + AVER_PATH_SEP "Materials" AVER_PATH_SEP + name + ".ocmat",
            content + AVER_PATH_SEP + name,
        };
        for (const std::string& path : candidates) {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) continue;
            pbr::MaterialDesc d;
            fmt::OcMatExtras extras;
            std::string err;
            // Parse failure breaks: corrupt built material must not fall through to stale hand-authored.
            if (!fmt::loadOcmat(path, d, &extras, &err)) { AVER_WARN("[Material] {}", err); break; }
            d.graphId = resolveMaterialGraph(extras.graphRef);
            h = pbr::MaterialLibrary::get().create(d);
            if (h) AVER_INFO("[Material] '{}' loaded from {}{}", d.name, path,
                              d.graphId ? " (graph " + std::to_string(d.graphId) + ")" : "");
            break;
        }
    }
    // Cache 0 (no retries) to avoid stat calls for missing surfaces.
    materialAssets_.emplace(name, h);
    MaterialUse& u = materialUse_[name];
    if (!u.meshes) u.idleSince = streamTick_;
    return h;
}

void GameContent::releaseIdleMaterials() {
    constexpr u64 kIdleTicks = 600;   // ~10 s: a cell flickering at the evict edge does not reload them
    usize released = 0;
    for (auto it = materialUse_.begin(); it != materialUse_.end();) {
        const MaterialUse& u = it->second;
        if (u.kept || u.meshes || u.idleSince + kIdleTicks > streamTick_) { ++it; continue; }
        if (const auto a = materialAssets_.find(it->first); a != materialAssets_.end()) {
            if (const pbr::MaterialHandle h = a->second) {
#  if AVER_MODULE_SCENE
                for (auto s = surfaceMaterials_.begin(); s != surfaceMaterials_.end();)
                    s = s->second == h ? surfaceMaterials_.erase(s) : std::next(s);
#  endif
                pbr::MaterialLibrary::get().destroy(h);   // MaterialSystem frees its textures at its next update
                ++released;
            }
            materialAssets_.erase(a);
        }
        it = materialUse_.erase(it);
    }
    if (!released) return;
    assets::forgetUploadedTextures();   // their textures may be read ahead again
    AVER_INFO("[Material] {} streamed material(s) no loaded mesh uses released", released);
}

bool GameContent::buildNextMaterial() {
    if (materialBinds_.empty()) return false;
    const MaterialBind b = std::move(materialBinds_.front());
    materialBinds_.pop_front();
    if (const pbr::MaterialHandle h = loadSurfaceMaterial(b.name)) {
        if (materialSystem_) materialSystem_->bindingSet(h);   // builds it unless already built
        bindSurfaceMaterial(b.token, h);
    }
    return true;
}

void GameContent::releaseProjectMaterials(bool clearGraphRegistry) {
    materialBinds_.clear();
    assets::dropPrefetchedTextures();
    // Destroy materials (library owns them, map only names).
    for (const auto& kv : materialAssets_) if (kv.second) pbr::MaterialLibrary::get().destroy(kv.second);
    materialAssets_.clear();
    materialUse_.clear();
    meshMaterialNames_.clear();
    // Material graphs cached process-wide by path; clear to prevent id collisions on project reload.
    if (clearGraphRegistry) pbr::materialGraphs().clear();
#if AVER_MODULE_SCENE
    surfaceMaterials_.clear();
#endif
}

// Release all materials except those in keep set.
usize GameContent::releaseMaterialsExcept(const std::unordered_set<std::string>& keep) {
    assets::dropPrefetchedTextures();   // what it releases may be needed again
    usize released = 0;
    for (auto it = materialAssets_.begin(); it != materialAssets_.end();) {
        if (keep.count(it->first)) { ++it; continue; }
        if (it->second) { pbr::MaterialLibrary::get().destroy(it->second); ++released; }
        materialUse_.erase(it->first);
        it = materialAssets_.erase(it);
    }
#if AVER_MODULE_SCENE
    // Clean dead handles from surface map.
    if (released) {
        for (auto it = surfaceMaterials_.begin(); it != surfaceMaterials_.end();) {
            if (it->second && !pbr::MaterialLibrary::get().valid(it->second)) it = surfaceMaterials_.erase(it);
            else ++it;
        }
    }
#endif
    return released;
}

#endif // AVER_MODULE_PBR

#if AVER_MODULE_SCENE
i32 GameContent::meshDefaultMaterial(u64 meshId) const {
    const auto it = meshSlot0Material_.find(meshId);
    return it == meshSlot0Material_.end() ? 0 : it->second;
}

const std::string& GameContent::meshSlot0Name(u64 meshId) const {
    static const std::string kNone;
    const auto it = meshSlot0Name_.find(meshId);
    return it == meshSlot0Name_.end() ? kNone : it->second;
}
#endif

#if AVER_MODULE_PBR && AVER_MODULE_SCENE
void GameContent::queueMeshMaterials(u64 meshId) {
    if (meshMaterialNames_.count(meshId)) return;
    std::vector<std::string>& names = meshMaterialNames_[meshId];
    const auto queue = [&](i32 token, const std::string& name) {
        queueSurfaceMaterial(token, name);
        if (std::find(names.begin(), names.end(), name) != names.end()) return;
        names.push_back(name);
        ++materialUse_[name].meshes;
    };
    if (const i32 slot0Token = meshDefaultMaterial(meshId)) {
        const std::string& name = meshSlot0Name(meshId);
        if (!name.empty()) queue(slot0Token, name);
    }
    if (const std::vector<MeshPart>* parts = partsFor(meshId)) {
        for (const MeshPart& part : *parts) {
            if (!part.material) continue;
            const char* partName = aver_scene_material_name(part.material);
            if (partName && *partName) queue(part.material, partName);
        }
    }
}

pbr::MaterialHandle GameContent::authoredFor(i32 token) const {
    const auto it = surfaceMaterials_.find(token);
    return it == surfaceMaterials_.end() ? 0 : it->second;
}
#endif

#if AVER_MODULE_PARTICLES
void GameContent::loadProjectParticleEffects() {
    // Clear process-global table on reload.
    particles::particleEffects().clear();
    const std::string dir = project_.contentDir();
    if (dir.empty()) return;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return;

    u32 loaded = 0, failed = 0;
    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string full = it->path().string();
        if (assetTypeFromPath(full) != AssetType::Particle) continue;

        std::string rel = std::filesystem::relative(it->path(), dir, ec).string();
        if (ec) continue;
        for (char& c : rel) if (c == '\\') c = '/';

        particles::ParticleSystemDef def;
        std::string err;
        if (!fmt::loadOcparticleSystem(full, def, nullptr, &err)) {
            AVER_WARN("[Particles] {}", err);
            ++failed;
            continue;
        }

        // Effect id is fnv1a64(relative path), matches CParticleEmitter::effect. A v1 file arrives as a one-emitter system.
        particles::particleEffects().setSystem(fnv1a64(std::string_view(rel)), def);
        ++loaded;
    }
    if (loaded || failed)
        AVER_INFO("[Particles] {} project effect(s) loaded from {}{}", loaded, dir,
                  failed ? (", " + std::to_string(failed) + " failed") : "");
}

rhi::TextureHandle GameContent::resolveParticleTexture(u64 textureId, void* user) {
#if AVER_MODULE_PBR
    auto* self = static_cast<GameContent*>(user);
    if (!self || !self->textureFactory_ || textureId == 0) return 0;
    const auto it = self->particleTextures_.find(textureId);
    if (it != self->particleTextures_.end()) return it->second;
    rhi::TextureHandle h = 0;
    const std::string path = self->pathFor(textureId);
    if (path.empty()) {
        AVER_WARN("[Particles] texture id 0x{:016X} is not in the content index; the effect draws the procedural dot", textureId);
    } else {
        std::string err;
        h = assets::uploadTexture(*self->textureFactory_, path, assets::TextureUsage::Colour, &err, nullptr);
        if (!h) AVER_WARN("[Particles] {} - the effect draws the procedural dot", err);
    }
    self->particleTextures_[textureId] = h;   // a failure is cached too, so it is logged once
    return h;
#else
    (void)textureId; (void)user;
    return 0;
#endif
}

bool GameContent::resolveParticleMesh(u64 meshId, particles::ParticleMeshData& out, void* user) {
#if AVER_MODULE_SCENE
    auto* self = static_cast<GameContent*>(user);
    if (!self || meshId == 0) return false;
    const std::string path = self->pathFor(meshId);
    if (path.empty()) {
        // The two built-in primitives (registerBuiltins) have no file; a particle may still name them. Coarser than the scene's
        // own sphere: a mesh particle is a few hundred triangles at most (ParticleMeshCache::kMaxTriangles).
        std::vector<rhi::MeshVertex> v;
        std::vector<u32> idx;
        if (meshId == fnv1a64(std::string_view("Meshes/cube.ocmesh"))) appendBox(v, idx, 0, 0, 0, 1.0f);
        else if (meshId == fnv1a64(std::string_view("Meshes/sphere.ocmesh"))) appendSphere(v, idx, 1.0f, 12, 20);
        else return false;
        out.positions.clear(); out.normals.clear(); out.uvs.clear();
        for (const rhi::MeshVertex& mv : v) {
            out.positions.insert(out.positions.end(), {mv.px, mv.py, mv.pz});
            out.normals.insert(out.normals.end(), {mv.nx, mv.ny, mv.nz});
            out.uvs.insert(out.uvs.end(), {mv.u, mv.v});
        }
        out.indices = std::move(idx);
        return !out.indices.empty();
    }
    fmt::OcMeshData md;
    std::string why;
    if (!fmt::loadOcMesh(path, md, &why)) { AVER_WARN("[Particles] {}", why); return false; }
    out.positions = std::move(md.positions);
    out.normals = std::move(md.normals);
    out.uvs = std::move(md.uvs);
    out.indices = std::move(md.indices);
    return !out.indices.empty() && !out.positions.empty();
#else
    (void)meshId; (void)out; (void)user;
    return false;
#endif
}

void GameContent::releaseParticleTextures() {
#if AVER_MODULE_PBR
    if (textureFactory_)
        for (const auto& kv : particleTextures_)
            if (kv.second) textureFactory_->destroyTexture(kv.second);
    particleTextures_.clear();
#endif
}
#endif // AVER_MODULE_PARTICLES

// Mesh lookup in content index; empty table means no scene state.
rhi::MeshHandle GameContent::meshFor(u64 id) const {
    const auto it = sceneMeshes_.find(id);
    return it == sceneMeshes_.end() ? 0 : it->second;
}

} // namespace aver::game
