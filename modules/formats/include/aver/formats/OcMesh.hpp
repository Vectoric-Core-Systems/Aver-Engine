#pragma once
// .ocmesh — the static mesh format (FORMAT_SPECS.md §5), an AVR1 container with subtype 'MESH'.
//
// This is the format whose absence made the engine unable to render anything but a cube and a
// sphere. SkyForge's weapon is five scaled boxes because of it, and Content/Meshes/ in every project
// is an empty directory.
//
// WHAT IS IMPLEMENTED, and what is deliberately not.
//
// Implemented: the MHDR header, STRT string table, VTXS vertex data, IDXS index data and MADR
// material-slot table, for ONE LOD and any number of submeshes. That is the subset a static mesh
// needs in order to be drawn, and it is spec-shaped rather than convenient -- the vertex data really
// is written in the canonical stream grouping (§5.2), position standalone in bind slot 0 and the
// tangent frame plus UV0 interleaved in slot 1, so a future renderer that binds slots natively reads
// these same files without a format break.
//
// NOT implemented, and skipped rather than faked: MLET (meshlets), COLL (collision hulls), CBND
// (cage bind), multiple LODs, quantized positions, vertex colour and UV1. Every one of those is an
// OPTIONAL chunk or an optional flag, so a file this writes is a valid .ocmesh and a reader that
// grows to understand them will read it unchanged. The spec's forward-compatibility rule is the
// whole reason it is safe to ship a subset: an unknown chunk is skipped unless marked Required, and
// nothing here marks anything Required except MHDR.
//
// THE READER CONVERTS TO rhi::MeshVertex, and that is a decision worth stating. The engine's vertex
// is 32 bytes of interleaved position/normal/uv, static_assert'd against three consumers that cannot
// check it -- the D3D12 input layout's literal offsets, the mesh-shader path's raw SRV element size,
// and the ray-tracing BLAS stride. Reading .ocmesh natively into slot buffers would mean changing
// that ABI in all three at once. So the file keeps the canonical layout and the loader de-interleaves
// on the way in: the format is right for the future, and today's renderer is untouched.
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"

#include <string>
#include <vector>

namespace aver::fmt {

// MeshFlags (§5.1). Only the ones this writer can produce or this reader must honour.
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

// A mesh as the engine wants it: one interleaved vertex array and one index array, which is what
// rhi::IDevice::createMesh takes. The FILE is stored in the spec's stream layout; this is the
// decoded form, and the conversion is the loader's job -- see the header note.
struct OcMeshData {
    std::vector<f32> positions;      // 3 per vertex
    std::vector<f32> normals;        // 3 per vertex
    std::vector<f32> uvs;            // 2 per vertex
    std::vector<u32> indices;
    std::vector<OcMeshSubmesh> submeshes;
    std::vector<std::string>   materialSlots;   // slot index -> surface name

    Vec3 boundsMin{0, 0, 0}, boundsMax{0, 0, 0};
    u32  flags = 0;

    u32  vertexCount() const { return static_cast<u32>(positions.size() / 3); }
    bool valid() const {
        const usize v = positions.size() / 3;
        return v > 0 && !indices.empty() && normals.size() == v * 3 && uvs.size() == v * 2;
    }
    // Recomputes boundsMin/boundsMax from positions. Called by the writer, because a mesh whose
    // stored bounds disagree with its vertices culls wrongly and nothing reports it.
    void computeBounds();
};

// Read/write .ocmesh. `why` is set on failure and untouched on success.
bool loadOcMesh(const std::string& path, OcMeshData& out, std::string* why = nullptr);
bool saveOcMesh(const std::string& path, const OcMeshData& in, std::string* why = nullptr);

// The same, against memory. The importer writes through these so it can hash or inspect the bytes
// before they reach a disk.
bool parseOcMesh(const u8* bytes, usize size, OcMeshData& out, std::string* why = nullptr);
bool writeOcMesh(const OcMeshData& in, std::vector<u8>& out, std::string* why = nullptr);

} // namespace aver::fmt
