#include "aver/game/GameContent.hpp"

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>

// Always included: AssetType/assetTypeFromPath are used by both loadProjectMeshes (scene-guarded)
// and loadProjectParticleEffects (particles-guarded); scoping to either guard breaks the other.
#include "aver/assets/AssetId.hpp"

#if AVER_MODULE_PBR
#  include "aver/assets/TextureUpload.hpp"
#  include "aver/formats/OcGraph.hpp"
#  include "aver/formats/OcMat.hpp"
#  include "aver/pbr/MaterialGraphRegistry.hpp"
#endif

#if AVER_MODULE_SCENE
#  include "aver/anim/AnimSystem.hpp"
#  include "aver/formats/OcMesh.hpp"
#  include "aver/scene/scene_abi.h"
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
#endif

namespace aver::game {

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

    // Position, normal, uv only. rhi::MeshVertex has nowhere to put joints/weights.
    std::vector<rhi::MeshVertex> verts(md.vertexCount());
    for (u32 i = 0; i < md.vertexCount(); ++i) {
        rhi::MeshVertex& v = verts[i];
        v.px = md.positions[usize(i)*3+0]; v.py = md.positions[usize(i)*3+1]; v.pz = md.positions[usize(i)*3+2];
        v.nx = md.normals[usize(i)*3+0];   v.ny = md.normals[usize(i)*3+1];   v.nz = md.normals[usize(i)*3+2];
        v.u  = md.uvs[usize(i)*2+0];       v.v  = md.uvs[usize(i)*2+1];
    }
    const rhi::MeshHandle h = device.createMesh(verts.data(), (u32)verts.size(),
                                               md.indices.data(), (u32)md.indices.size());
    if (!h) { AVER_WARN("[Mesh] the device refused '{}'", rel); return false; }

    sceneMeshes_[id] = h;
    meshBounds_[id] = {md.boundsMin, md.boundsMax};
    // Mesh's own material slot; see GameContent.hpp::meshDefaultMaterial.
    if (!md.materialSlots.empty() && !md.materialSlots[0].empty()) {
        meshSlot0Material_[id] = aver_scene_material(0, md.materialSlots[0].c_str());
        meshSlot0Name_[id] = md.materialSlots[0];
    }
    // The per-submesh split: a mesh naming several materials draws one part per material.
    buildMeshParts(device, id, md, verts, rel);
    projectMeshIds_.push_back(id);
    if (meshLoaded_) meshLoaded_(LoadedMesh{id, rel, &md, verts, md.indices, h}, meshLoadedUser_);
    if (lazyLoading_ && meshAcquired_) meshAcquired_(LoadedMesh{id, rel, &md, verts, md.indices, h}, meshAcquiredUser_);

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
            const rhi::MeshHandle ph = device.createMesh(verts.data(), (u32)verts.size(),
                                                         lod.indices.data(), (u32)lod.indices.size());
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
        const bool ok = pit != contentIndex_.end() && loadOneMesh(device, id, pit->second, lit->second);
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
    projectMeshIds_.erase(std::remove(projectMeshIds_.begin(), projectMeshIds_.end(), id), projectMeshIds_.end());
    pendingMeshes_.push_back(std::move(p));
    if (meshReleased_) meshReleased_(id, meshReleasedUser_);
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
// COMPACTED PER PART, not sharing the parent's vertex array. createMesh COPIES what it is given, so
// handing every part of a multi-material mesh the WHOLE vertex buffer would upload that buffer once
// per part. The remap also gives each part honest bounds, which a future per-part culler would want
// anyway.
void GameContent::buildMeshParts(rhi::IDevice& device, u64 id, const fmt::OcMeshData& md,
                                  const std::vector<rhi::MeshVertex>& verts, const std::string& rel) {
    if (md.submeshes.size() <= 1) return;   // common case: nothing to split

    std::vector<MeshPart> parts;
    parts.reserve(md.submeshes.size());
    std::unordered_map<u32, u32> remap;
    std::vector<rhi::MeshVertex> pv;
    std::vector<u32> pi;
    // Unremapped slices for skinned meshes only (skin targets share base index buffer verbatim).
    const bool keepBaseIndices = md.hasSkin();
    std::vector<std::vector<u32>> baseIndices;

    for (const fmt::OcMeshSubmesh& sm : md.submeshes) {
        if (sm.indexCount == 0) continue;
        const usize end = usize(sm.indexStart) + sm.indexCount;
        if (end > md.indices.size()) {
            AVER_WARN("[Mesh] '{}' submesh '{}' runs past the index buffer; skipped", rel, sm.name);
            continue;
        }
        remap.clear(); pv.clear(); pi.clear();
        pi.reserve(sm.indexCount);
        bool bad = false;
        for (usize k = sm.indexStart; k < end; ++k) {
            const u32 vi = md.indices[k];
            if (vi >= verts.size()) { bad = true; break; }
            const auto [it2, inserted] = remap.try_emplace(vi, static_cast<u32>(pv.size()));
            if (inserted) pv.push_back(verts[vi]);
            pi.push_back(it2->second);
        }
        if (bad || pv.empty()) {
            AVER_WARN("[Mesh] '{}' submesh '{}' indexes a vertex it does not have; skipped", rel, sm.name);
            continue;
        }

        MeshPart part;
        part.mesh = device.createMesh(pv.data(), static_cast<u32>(pv.size()),
                                       pi.data(), static_cast<u32>(pi.size()));
        if (!part.mesh) {
            AVER_WARN("[Mesh] the device refused submesh '{}' of '{}'", sm.name, rel);
            continue;
        }
        // Slot names the material, resolving through the same path as an authored material.
        if (sm.materialSlot < md.materialSlots.size()) {
            const std::string& slot = md.materialSlots[sm.materialSlot];
            if (!slot.empty()) part.material = aver_scene_material(0, slot.c_str());
        }
        parts.push_back(part);
        // baseIndices[i] is the slice parts[i] was cut from (lockstep).
        if (keepBaseIndices)
            baseIndices.emplace_back(md.indices.data() + sm.indexStart, md.indices.data() + end);
    }

    // One surviving part is not a split; falling through costs one less draw call.
    if (parts.size() <= 1) {
        for (const MeshPart& p : parts) if (p.mesh) device.destroyMesh(p.mesh);
        return;
    }
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
constexpr u32 kCollisionCacheMagic   = 0x4C4F4341u;
constexpr u32 kCollisionCacheVersion = 1;

std::string collisionCacheDir(const std::string& projectDir) {
    return projectDir.empty() ? std::string() : projectDir + "\\Saved\\DerivedDataCache\\Collision";
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
    return dir + "\\" + name;
}

// Reads and validates cache entry against source mesh's current path/size/mtime.
bool readCollisionCacheFile(const std::string& cachePath, const std::string& meshPath, u64 fileSize,
                            i64 mtimeTicks, GameContent::CollisionMesh& out) {
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
        if (!out) { if (why) *why = "write failed for " + tmpPath; return false; }
    }
    std::filesystem::rename(tmpPath, cachePath, ec);
    if (ec) { if (why) *why = "could not publish " + cachePath + ": " + ec.message(); return false; }
    return true;
}

} // namespace

#if AVER_MODULE_PBR
// Material slot collision filter.
bool GameContent::collisionSlotCollides(const std::string& slotName) {
    if (slotName.empty()) return true;
    const pbr::MaterialHandle mh = materialForSurface(slotName);
    if (!mh) return true;   // no authored .ocmat: cannot resolve
    const pbr::MaterialDesc* d = pbr::MaterialLibrary::get().desc(mh);
    if (!d) return true;
    return d->alphaMode != pbr::AlphaMode::Mask && d->alphaMode != pbr::AlphaMode::Blend;
}
#else
// Every slot collides (no PBR module).
bool GameContent::collisionSlotCollides(const std::string&) { return true; }
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
    if (!cachePath.empty()) {
        auto cached = std::make_unique<CollisionMesh>();
        if (readCollisionCacheFile(cachePath, path, srcSize, srcMtime, *cached)) {
            ++collisionCacheHits_;
            AVER_INFO("[Collision] {}: {} tris from the disk cache", path, cached->indices.size() / 3);
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
    const u32 vertexCount = md.vertexCount();
    if (vertexCount == 0 || md.indices.size() < 3) return nullptr;

    // Drop cut-out/translucent material slots from LOD 0 submeshes.
    bool anyDropped = false;
    std::vector<u32> keptIndices;
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
            if (!collisionSlotCollides(slotName)) { anyDropped = true; ++droppedSlots; continue; }
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

    AVER_INFO("[Collision] {}: {} ({} tris from {} at LOD 0, {:.1f} cm error)", path, wonBy,
              mesh->indices.size() / 3, md.indices.size() / 3, mesh->errorCm);

    if (!cachePath.empty()) {
        std::string saveWhy;
        if (!writeCollisionCacheFile(cachePath, path, srcSize, srcMtime, *mesh, &saveWhy))
            AVER_WARN("[Collision] {}: could not write the disk cache: {}", path, saveWhy);
    }

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
            const std::string full = content + "\\" + p;
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

    // Slot decides colour space: normal maps sRGB-decoded give subtle shading bugs.
    assets::TextureUsage usage = assets::TextureUsage::Data;
    // Layer1 slots must be here too: Layer1BaseColor is sRGB, Layer1Normal is NormalMap.
    switch (slot) {
        case pbr::TextureSlot::BaseColor:
        case pbr::TextureSlot::Layer1BaseColor:
        case pbr::TextureSlot::Emissive:      usage = assets::TextureUsage::Colour;    break;
        case pbr::TextureSlot::Normal:
        case pbr::TextureSlot::Layer1Normal:  usage = assets::TextureUsage::NormalMap; break;
        default:                              usage = assets::TextureUsage::Data;      break;
    }

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

// .ocmat GRAPHREF path to gMaterialGraphId. Returns 0 for materials with no graph or load failure.
// Broken graph does not disable the material: falls back to stock factors/maps.
u32 GameContent::resolveMaterialGraph(const std::string& graphRef) const {
    if (graphRef.empty()) return 0;
    const std::string content = project_.contentDir();
    if (content.empty()) return 0;

    // Content-relative path (matches COMP mesh= and TEX conventions).
    std::string path = content + "\\" + graphRef;
    for (char& c : path) if (c == '/') c = '\\';

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
    const auto cached = materialAssets_.find(name);
    if (cached != materialAssets_.end()) return cached->second;

    pbr::MaterialHandle h = 0;
    const std::string content = project_.contentDir();
    if (!content.empty()) {
        // Built .ocmat (Binaries) wins over hand-authored (Content).
        const std::string candidates[3] = {
            project_.binariesDir() + "\\Materials\\" + name + ".ocmat",
            content + "\\Materials\\" + name + ".ocmat",
            content + "\\" + name,
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
    return h;
}

void GameContent::releaseProjectMaterials(bool clearGraphRegistry) {
    // Destroy materials (library owns them, map only names).
    for (const auto& kv : materialAssets_) if (kv.second) pbr::MaterialLibrary::get().destroy(kv.second);
    materialAssets_.clear();
    // Material graphs cached process-wide by path; clear to prevent id collisions on project reload.
    if (clearGraphRegistry) pbr::materialGraphs().clear();
#if AVER_MODULE_SCENE
    surfaceMaterials_.clear();
#endif
}

// Release all materials except those in keep set.
usize GameContent::releaseMaterialsExcept(const std::unordered_set<std::string>& keep) {
    usize released = 0;
    for (auto it = materialAssets_.begin(); it != materialAssets_.end();) {
        if (keep.count(it->first)) { ++it; continue; }
        if (it->second) { pbr::MaterialLibrary::get().destroy(it->second); ++released; }
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

        particles::ParticleEffect fx;
        std::string err;
        if (!fmt::loadOcparticle(full, fx, nullptr, &err)) {
            AVER_WARN("[Particles] {}", err);
            ++failed;
            continue;
        }

        // Effect id is fnv1a64(relative path), matches CParticleEmitter::effect.
        particles::particleEffects().set(fnv1a64(std::string_view(rel)), fx);
        ++loaded;
    }
    if (loaded || failed)
        AVER_INFO("[Particles] {} project effect(s) loaded from {}{}", loaded, dir,
                  failed ? (", " + std::to_string(failed) + " failed") : "");
}
#endif // AVER_MODULE_PARTICLES

// Mesh lookup in content index; empty table means no scene state.
rhi::MeshHandle GameContent::meshFor(u64 id) const {
    const auto it = sceneMeshes_.find(id);
    return it == sceneMeshes_.end() ? 0 : it->second;
}

} // namespace aver::game
