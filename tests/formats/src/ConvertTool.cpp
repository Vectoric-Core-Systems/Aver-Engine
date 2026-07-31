// Command-line tool: imports a glTF/GLB file, writes a .ocmesh, and reads it back.
#include "aver/formats/GltfImport.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/core/Log.hpp"

using namespace aver;

// Converts argv[1] to argv[2]. Returns 0 on success, 1 on a conversion error, 2 on bad usage.
int main(int argc, char** argv) {
    if (argc < 3) { AVER_ERROR("usage: convert <in.glb> <out.ocmesh>"); return 2; }
    fmt::GltfImportResult res;
    std::string why;
    if (!fmt::importGltf(argv[1], res, {}, &why)) { AVER_ERROR("import: {}", why); return 1; }
    for (const std::string& u : res.unsupported) AVER_WARN("unsupported: {}", u);
    if (res.meshes.empty()) { AVER_ERROR("no meshes"); return 1; }

    const fmt::OcMeshData& m = res.meshes[0];
    AVER_INFO("imported '{}': {} verts, {} tris, bounds ({:.1f},{:.1f},{:.1f})..({:.1f},{:.1f},{:.1f})",
              res.meshNames[0], m.vertexCount(), m.indices.size() / 3,
              m.boundsMin.x, m.boundsMin.y, m.boundsMin.z, m.boundsMax.x, m.boundsMax.y, m.boundsMax.z);

    if (!fmt::saveOcMesh(argv[2], m, &why)) { AVER_ERROR("save: {}", why); return 1; }
    AVER_INFO("wrote {}", argv[2]);

    fmt::OcMeshData back;
    if (!fmt::loadOcMesh(argv[2], back, &why)) { AVER_ERROR("reload: {}", why); return 1; }
    AVER_INFO("reloaded: {} verts, {} tris", back.vertexCount(), back.indices.size() / 3);
    return 0;
}
