// The preview's mesh cache: built-in primitives, character capsules, and .ocmesh files off disk.
#include "aver/render/preview/PreviewMeshCache.hpp"
#include "aver/formats/ActorScript.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>

namespace aver::render::preview {
namespace {

constexpr f32 kPi = 3.14159265358979f;

// Appends a unit box, half-extent one, per-face vertices. Must match the engine's Meshes/cube.ocmesh.
void appendUnitBox(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx) {
    const f32 h = 1.0f;
    const f32 n[6][3] = {{0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}};
    const f32 c[6][4][3] = {
        {{-h,-h, h},{ h,-h, h},{ h, h, h},{-h, h, h}},
        {{ h,-h,-h},{-h,-h,-h},{-h, h,-h},{ h, h,-h}},
        {{ h,-h, h},{ h,-h,-h},{ h, h,-h},{ h, h, h}},
        {{-h,-h,-h},{-h,-h, h},{-h, h, h},{-h, h,-h}},
        {{-h, h, h},{ h, h, h},{ h, h,-h},{-h, h,-h}},
        {{-h,-h,-h},{ h,-h,-h},{ h,-h, h},{-h,-h, h}},
    };
    for (int f = 0; f < 6; ++f) {
        const u32 base = static_cast<u32>(v.size());
        for (int k = 0; k < 4; ++k)
            v.push_back({c[f][k][0], c[f][k][1], c[f][k][2], n[f][0], n[f][1], n[f][2],
                         static_cast<f32>(k == 1 || k == 2), static_cast<f32>(k >= 2)});
        idx.push_back(base); idx.push_back(base + 1); idx.push_back(base + 2);
        idx.push_back(base); idx.push_back(base + 2); idx.push_back(base + 3);
    }
}

// Appends a unit sphere. Same ring/sector construction and +Z pole as the engine's own.
void appendUnitSphere(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx, u32 rings, u32 sectors) {
    const u32 base = static_cast<u32>(v.size());
    for (u32 ring = 0; ring <= rings; ++ring) {
        const f32 phi = kPi * (static_cast<f32>(ring) / static_cast<f32>(rings));
        const f32 z = std::cos(phi), rad = std::sin(phi);
        for (u32 sec = 0; sec <= sectors; ++sec) {
            const f32 theta = 2.0f * kPi * (static_cast<f32>(sec) / static_cast<f32>(sectors));
            const f32 nx = rad * std::cos(theta), ny = rad * std::sin(theta), nz = z;
            v.push_back({nx, ny, nz, nx, ny, nz,
                         static_cast<f32>(sec) / static_cast<f32>(sectors),
                         static_cast<f32>(ring) / static_cast<f32>(rings)});
        }
    }
    const u32 stride = sectors + 1;
    for (u32 ring = 0; ring < rings; ++ring)
        for (u32 sec = 0; sec < sectors; ++sec) {
            const u32 a = base + ring * stride + sec, b = a + stride;
            idx.push_back(a); idx.push_back(b); idx.push_back(a + 1);
            idx.push_back(a + 1); idx.push_back(b); idx.push_back(b + 1);
        }
}

} // namespace

// Sets the content root. Clears the cache when it changes.
void PreviewMeshCache::setContentRoot(rhi::IDevice& device, std::string root) {
    if (root == root_) return;
    clear(device);
    root_ = std::move(root);
}

// Forgets every cached mesh, radius and miss.
void PreviewMeshCache::clear(rhi::IDevice& device) {
    (void)device;   // meshes live for the device's lifetime; there is no destroyMesh
    meshes_.clear();
    radii_.clear();
    missing_.clear();
    loaded_ = 0;
}

namespace {
// The distance from the origin to the furthest vertex.
f32 maxRadius(const std::vector<rhi::MeshVertex>& v) {
    f32 r2 = 0.0f;
    for (const rhi::MeshVertex& x : v) {
        const f32 d = x.px * x.px + x.py * x.py + x.pz * x.pz;
        if (d > r2) r2 = d;
    }
    return std::sqrt(r2);
}

// Appends a capsule standing on Z = 0, as one ring-and-sector sweep. `height` is total, caps included.
void appendCapsule(std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx,
                   f32 height, f32 radius, u32 rings, u32 sectors) {
    const f32 pi = 3.14159265358979f;
    const f32 half = std::fmax(height * 0.5f - radius, 0.0f);
    const f32 centreZ = height * 0.5f;
    const u32 base = static_cast<u32>(v.size());
    for (u32 ring = 0; ring <= rings; ++ring) {
        const f32 phi = pi * (static_cast<f32>(ring) / static_cast<f32>(rings));
        const f32 nz = std::cos(phi), rad = std::sin(phi);
        const f32 off = nz >= 0.0f ? half : -half;
        for (u32 sec = 0; sec <= sectors; ++sec) {
            const f32 th = 2.0f * pi * (static_cast<f32>(sec) / static_cast<f32>(sectors));
            const f32 nx = rad * std::cos(th), ny = rad * std::sin(th);
            v.push_back({nx * radius, ny * radius, centreZ + nz * radius + off, nx, ny, nz,
                         static_cast<f32>(sec) / static_cast<f32>(sectors),
                         static_cast<f32>(ring) / static_cast<f32>(rings)});
        }
    }
    const u32 stride = sectors + 1;
    for (u32 ring = 0; ring < rings; ++ring)
        for (u32 sec = 0; sec < sectors; ++sec) {
            const u32 a = base + ring * stride + sec, b = a + stride;
            idx.push_back(a); idx.push_back(b); idx.push_back(a + 1);
            idx.push_back(a + 1); idx.push_back(b); idx.push_back(b + 1);
        }
}
} // namespace

// Builds or returns a character capsule, cached per (height, radius). Falls back to 180x34 cm.
rhi::MeshHandle PreviewMeshCache::capsule(rhi::IDevice& device, f32 heightCm, f32 radiusCm, f32* outRadius) {
    const f32 h = heightCm > 1.0f ? heightCm : 180.0f;
    const f32 r = radiusCm > 0.1f ? radiusCm : 34.0f;
    char key[64];
    std::snprintf(key, sizeof key, "$capsule/%.2f/%.2f", static_cast<double>(h), static_cast<double>(r));
    const std::string k = key;
    if (const auto it = meshes_.find(k); it != meshes_.end()) {
        if (outRadius) *outRadius = radiusOf(k);
        return it->second;
    }
    std::vector<rhi::MeshVertex> v;
    std::vector<u32> idx;
    appendCapsule(v, idx, h, r, 16, 24);
    const rhi::MeshHandle handle = device.createMesh(v.data(), static_cast<u32>(v.size()),
                                                     idx.data(), static_cast<u32>(idx.size()));
    meshes_[k] = handle;
    radii_[k] = maxRadius(v);
    if (outRadius) *outRadius = radii_[k];
    if (handle) { ++loaded_; AVER_INFO("[Preview] capsule {}x{} cm -> {} verts", h, r, v.size()); }
    return handle;
}

// The cached radius for a path, or 0 if it is not cached.
f32 PreviewMeshCache::radiusOf(std::string_view meshPath) const {
    const auto it = radii_.find(fmt::canonicalMeshPath(meshPath));
    return it == radii_.end() ? 0.0f : it->second;
}

// Resolves a mesh path to a handle, loading it on the first ask. Returns 0 on failure, and caches
// that miss as handle 0 so a bad path costs one disk touch rather than one per frame.
rhi::MeshHandle PreviewMeshCache::resolve(rhi::IDevice& device, std::string_view meshPath, f32* outRadius) {
    if (meshPath.empty()) return 0;
    const std::string key = fmt::canonicalMeshPath(meshPath);

    if (const auto it = meshes_.find(key); it != meshes_.end()) {
        if (outRadius) *outRadius = radiusOf(key);
        return it->second;
    }

    // The built-in primitives, which are generated names rather than files on disk.
    if (key == "Meshes/cube.ocmesh" || key == "Meshes/sphere.ocmesh") {
        std::vector<rhi::MeshVertex> v;
        std::vector<u32> idx;
        if (key == "Meshes/cube.ocmesh") appendUnitBox(v, idx);
        else                             appendUnitSphere(v, idx, 24, 48);
        const rhi::MeshHandle h = device.createMesh(v.data(), static_cast<u32>(v.size()),
                                                    idx.data(), static_cast<u32>(idx.size()));
        meshes_[key] = h;
        radii_[key] = maxRadius(v);
        if (outRadius) *outRadius = radii_[key];
        if (h) { ++loaded_; AVER_INFO("[Preview] built-in '{}' -> {} verts, radius {}", key, v.size(), radii_[key]); }
        return h;
    }

    if (root_.empty()) { meshes_[key] = 0; return 0; }

    const std::filesystem::path full = std::filesystem::path(root_) / key;
    std::error_code ec;
    if (!std::filesystem::exists(full, ec)) {
        meshes_[key] = 0;
        missing_.push_back(key);
        AVER_WARN("[Preview] no mesh at {}", full.string());
        return 0;
    }

    fmt::OcMeshData md;
    std::string why;
    if (!fmt::loadOcMesh(full.string(), md, &why)) {
        meshes_[key] = 0;
        missing_.push_back(key);
        AVER_WARN("[Preview] {} would not load: {}", key, why);
        return 0;
    }

    // Into the engine's interleaved 32-byte vertex; the file keeps the spec's stream layout.
    std::vector<rhi::MeshVertex> verts(md.vertexCount());
    for (u32 i = 0; i < md.vertexCount(); ++i) {
        rhi::MeshVertex& v = verts[i];
        v.px = md.positions[usize(i)*3+0]; v.py = md.positions[usize(i)*3+1]; v.pz = md.positions[usize(i)*3+2];
        v.nx = md.normals[usize(i)*3+0];   v.ny = md.normals[usize(i)*3+1];   v.nz = md.normals[usize(i)*3+2];
        v.u  = md.uvs[usize(i)*2+0];       v.v  = md.uvs[usize(i)*2+1];
    }

    const rhi::MeshHandle h = device.createMesh(verts.data(), static_cast<u32>(verts.size()),
                                                md.indices.data(), static_cast<u32>(md.indices.size()));
    if (!h) {
        meshes_[key] = 0;
        missing_.push_back(key);
        AVER_WARN("[Preview] the device refused '{}'", key);
        return 0;
    }
    meshes_[key] = h;
    radii_[key] = maxRadius(verts);
    if (outRadius) *outRadius = radii_[key];
    ++loaded_;
    AVER_INFO("[Preview] '{}' -> {} verts, {} indices, radius {}", key, verts.size(), md.indices.size(), radii_[key]);
    return h;
}

} // namespace aver::render::preview
