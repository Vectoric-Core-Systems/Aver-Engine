#pragma once
// The soft-body proxy tool (editor only): cooks the simplified cage a mesh over the particle cap simulates instead of itself.
// docs/SOFTBODY.md "Proxy cage". Welds the mesh with the sim weld, makes a single-submesh copy, simplifies it with the
// already-vendored meshoptimizer through trifactor::simplifyMesh (edge collapses onto existing vertices, so the proxy's particles
// lie ON the authored surface and the binding offsets are near zero) and writes `<mesh>.proxy.ocmesh` beside the mesh. The
// runtime never simplifies: Trifactor stays out of the packaged soft-body path, and with AVER_MODULE_TRIFACTOR off the tool
// says so and cooks nothing.
//
// The cooking is pure data in, data out (cookProxyMesh) so tests/editor can drive it with no file and no window; the file
// helper and the Details button sit on top.
#include "aver/core/Types.hpp"
#include "aver/formats/OcMesh.hpp"

#include <string>

namespace aver::editor {

struct ProxyCookResult {
    bool ok = false;
    std::string why;                 // the reason when !ok, else a one-line summary
    u32 renderTriangles = 0;         // the authored mesh
    u32 weldedVertices = 0;          // particles the authored mesh welds to (the number the cap is about)
    u32 weldedTriangles = 0;
    u32 proxyVertices = 0;           // particles the proxy will simulate
    u32 proxyTriangles = 0;
    f32 ratio = 0.0f;                // the simplification ratio used
    std::string outPath;             // the file written by cookProxyFile
};

struct ProxyCookRequest {
    u32 targetParticles = 2048;      // roughly how many particles the proxy should weld to
    f32 weldCm = 0.01f;              // the sim weld (SimParams::weldCm), so "the particles" means the same here and at runtime
};

// True when this build can simplify (AVER_MODULE_TRIFACTOR).
bool proxyToolAvailable();

// Welds `in`, simplifies a single-submesh copy and returns it in `out` (positions, normals, indices; UVs zero). False with
// info.why set when the mesh needs no proxy (it already welds under the target), has no triangles, or the simplifier refuses.
bool cookProxyMesh(const fmt::OcMeshData& in, const ProxyCookRequest& req, fmt::OcMeshData& out, ProxyCookResult& info);

// Loads `meshPath`, cooks and saves `outPath`. The result carries the numbers either way.
ProxyCookResult cookProxyFile(const std::string& meshPath, const std::string& outPath, const ProxyCookRequest& req);

// The one tool the Details section shares: the target the user picked and the last result to show.
class SoftProxyTool {
public:
    ProxyCookRequest request;
    ProxyCookResult last;
    // Cooks meshRel (content-relative) to proxyRel under contentDir and remembers the result.
    bool cook(const std::string& contentDir, const std::string& meshRel, const std::string& proxyRel);
};

SoftProxyTool& softProxyTool();

}  // namespace aver::editor
