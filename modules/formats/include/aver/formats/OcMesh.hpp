#pragma once
// .ocmesh — the static mesh format (FORMAT_SPECS.md §5), an AVR1 container with subtype 'MESH'.
// The file keeps the spec's stream grouping; the loader de-interleaves into rhi::MeshVertex's shape.
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// MeshFlags (§5.1).
inline constexpr u32 kOcMeshHasColor  = 1u << 0;
inline constexpr u32 kOcMeshHasUV1    = 1u << 1;
inline constexpr u32 kOcMeshMeshlets  = 1u << 2;
inline constexpr u32 kOcMeshIndex32   = 1u << 3;
inline constexpr u32 kOcMeshHasSkin   = 1u << 5;
inline constexpr u32 kOcMeshTwoSided  = 1u << 9;

// One material-slot group. The renderer draws a submesh with the material bound for its slot.
struct OcMeshSubmesh {
    std::string name;
    u32 materialSlot = 0;
    u32 indexStart   = 0;
    u32 indexCount   = 0;
    u32 baseVertex   = 0;
    u32 vertexCount  = 0;
};

// A decoded mesh: one vertex array and one index array, as rhi::IDevice::createMesh takes them.
// Bone influences per vertex. Four is the spec's JOINTS/WEIGHTS width (5.3) and the GPU stride.
inline constexpr u32 kOcMeshInfluences = 4;

struct OcMeshData {
    std::vector<f32> positions;      // 3 per vertex
    std::vector<f32> normals;        // 3 per vertex
    std::vector<f32> uvs;            // 2 per vertex
    std::vector<u32> indices;
    std::vector<OcMeshSubmesh> submeshes;
    std::vector<std::string>   materialSlots;   // slot index -> surface name

    // Skinning, present only when kOcMeshHasSkin is set. Four per vertex each.
    //
    // WEIGHTS ARE STORED AS R8G8B8A8_UNORM on disk per FORMAT_SPECS.md 5.3, so a round trip
    // quantises them to 1/255. The writer spends the rounding remainder on the largest influence,
    // so what comes back still sums to exactly one -- which is what skinning needs from them.
    std::vector<u16> joints;         // bone index into the .ocskel this mesh is bound to
    std::vector<f32> weights;        // normalised; the writer renormalises if they are not

    Vec3 boundsMin{0, 0, 0}, boundsMax{0, 0, 0};
    u32  flags = 0;

    u32  vertexCount() const { return static_cast<u32>(positions.size() / 3); }
    // True when the skin streams are present and correctly sized for the vertex count.
    bool hasSkin() const {
        const usize v = positions.size() / 3;
        return v > 0 && joints.size() == v * kOcMeshInfluences && weights.size() == v * kOcMeshInfluences;
    }
    // True when the streams are non-empty and the same length.
    bool valid() const {
        const usize v = positions.size() / 3;
        if (!(v > 0 && !indices.empty() && normals.size() == v * 3 && uvs.size() == v * 2)) return false;
        // Half a skin is worse than none: it would write a stream the reader then mis-sizes.
        if (!joints.empty() || !weights.empty()) return hasSkin();
        return true;
    }
    // Recomputes boundsMin/boundsMax from positions.
    void computeBounds();
};

// Reads and writes .ocmesh on disk. `why` is set on failure and untouched on success.
bool loadOcMesh(const std::string& path, OcMeshData& out, std::string* why = nullptr);
bool saveOcMesh(const std::string& path, const OcMeshData& in, std::string* why = nullptr);

// The same, against memory.
bool parseOcMesh(const u8* bytes, usize size, OcMeshData& out, std::string* why = nullptr);
bool writeOcMesh(const OcMeshData& in, std::vector<u8>& out, std::string* why = nullptr);

} // namespace aver::fmt
