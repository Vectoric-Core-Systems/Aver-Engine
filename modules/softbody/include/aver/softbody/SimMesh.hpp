// From a render mesh to the simulation: weld the render vertices into particles, make the cage, and
// keep the map back to the render vertices (RenderBinding) so a tear can follow in the draw mesh.
//
// Pure arithmetic over views of an .ocmesh; the header does not include Aver.Formats' mesh type.
#pragma once

#include "aver/softbody/Cage.hpp"
#include "aver/softbody/RenderBinding.hpp"
#include "aver/softbody/SoftAsset.hpp"

#include <functional>
#include <string>
#include <vector>

namespace aver::softbody {

struct SimMeshInput {                 // views over fmt::OcMeshData
    const f32* positions = nullptr;   // 3 per vertex, mesh-local cm
    const f32* uvs = nullptr;         // 2 per vertex, optional
    const u32* indices = nullptr;
    u32 vertexCount = 0, indexCount = 0;
    const u16* joints = nullptr;      // optional, 4 per vertex
    const f32* weights = nullptr;
};

// R and G of a mask texture at (u, v). False when the texture cannot be sampled.
using MaskSampler = std::function<bool(const std::string& texture, f32 u, f32 v, f32& r, f32& g)>;
// A render vertex's weight to the named bone (0 when it has none).
using BoneWeightQuery = std::function<f32(const std::string& bone, u32 renderVertex)>;

struct SimBuildSpec {
    Material material;
    f32 weldCm = 0.01f;
    u32 maxParticles = 4096;
    Mat4 entityWorld;                 // baked into the particle positions
    f32 uniformScale = 1.0f;          // the scale inside entityWorld; mesh-local cm lengths (reach) scale by it
    PaintData paint;
    bool destructible = false;
    f32 defaultReachCm = 6.0f;        // maps the painted reach bytes and the mask G channel
    MaskSampler mask;
    BoneWeightQuery boneWeight;
};

struct SimMesh {
    Cage cage;                        // particles in WORLD space, beams, triangles, bends
    RenderBinding binding;            // render vertex -> particle, render tri -> sim tri
    std::vector<Vec3> bindLocal;      // per particle, mesh-local (its first render vertex)
    std::vector<u32>  repVertex;      // per particle, that first render vertex
    std::vector<u16>  joint4;         // per particle, 4 each; empty when the mesh has no skin
    std::vector<f32>  weight4;
    u64 meshKey = 0;                  // simMeshKey of the input
    bool paintStale = false;          // spec.paint was for a different mesh; rules were used instead
    std::string error;                // empty on success
};

bool buildSimMesh(const SimMeshInput& in, const SimBuildSpec& spec, SimMesh& out);

// FNV-1a over the welded particle count and the welded positions at 1/1000 cm. Changes when a vertex moves.
u64 simMeshKey(const SimMeshInput& in, f32 weldCm);

struct PaintEnv {
    const SimMeshInput* mesh = nullptr;
    const RenderBinding* binding = nullptr;
    MaskSampler mask;
    BoneWeightQuery boneWeight;
    u64 meshKey = 0;                  // of the current mesh; a different paint.meshKey makes the painted arrays stale
    f32 defaultReachCm = 6.0f;
    f32 uniformScale = 1.0f;
    bool defaultTopPin = false;       // nothing painted and no rules: pin the top 2% of the mesh (+Z)
};

// Pins and skin reach onto the cage particles: the painted arrays when they match the mesh, else the
// rules. Returns the pinned count. Rules read MESH-LOCAL positions of each particle's first render vertex.
u32 applyPaint(Cage& cage, const PaintData& paint, const PaintEnv& env);

} // namespace aver::softbody
