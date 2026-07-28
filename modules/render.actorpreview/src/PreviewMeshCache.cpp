#include "aver/render/preview/PreviewMeshCache.hpp"
#include "aver/formats/ActorScript.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/core/Log.hpp"

#include <filesystem>

namespace aver::render::preview {

void PreviewMeshCache::setContentRoot(rhi::IDevice& device, std::string root) {
    if (root == root_) return;
    // Cleared rather than merged. The same relative path under a different content root is a
    // different file, and keeping the handle would draw the previous project's geometry here.
    clear(device);
    root_ = std::move(root);
}

void PreviewMeshCache::clear(rhi::IDevice& device) {
    (void)device;   // meshes are owned by the device for its life; there is no destroyMesh to call
    meshes_.clear();
    missing_.clear();
    loaded_ = 0;
}

rhi::MeshHandle PreviewMeshCache::resolve(rhi::IDevice& device, std::string_view meshPath) {
    if (meshPath.empty()) return 0;
    const std::string key = fmt::canonicalMeshPath(meshPath);

    // Both a hit AND a cached miss come out here: a miss is stored as handle 0, so an actor naming a
    // file that is not there costs one disk touch rather than one per frame.
    if (const auto it = meshes_.find(key); it != meshes_.end()) return it->second;

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

    // Into the engine's interleaved 32-byte vertex. The FILE keeps the spec's stream layout; this is
    // the conversion the .ocmesh reader exists to make cheap.
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
    ++loaded_;
    AVER_INFO("[Preview] '{}' -> {} verts, {} indices", key, verts.size(), md.indices.size());
    return h;
}

} // namespace aver::render::preview
