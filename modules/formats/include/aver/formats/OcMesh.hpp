#pragma once
// .ocmesh — the static mesh format (FORMAT_SPECS.md §5), an AVR1 container with subtype 'MESH'.
// The file keeps the spec's stream grouping; the loader de-interleaves into rhi::MeshVertex's shape.
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"

#include <limits>
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

// One meshlet, matching the on-disk MLET shape byte for byte (FORMAT_SPECS.md 5.7): MeshletDesc's
// VertexIndexOffset/TriangleOffset/Pad are a WRITE-TIME detail (computed from where this meshlet
// lands among its siblings), not carried here -- this struct is the DECODED, offset-free form both
// the writer starts from and the reader hands back.
//
// This type deliberately does NOT come from Aver.Trifactor's Cluster (modules/trifactor/include/
// aver/trifactor/ClusterBuilder.hpp), even though the two are near-identical in shape. Aver.Formats
// sits BELOW Aver.Trifactor in the module DAG (cmake/AvModule.cmake, aver_check_module_dag) and a
// format reader/writer must load with AVER_MODULE_TRIFACTOR=OFF, so it cannot name a Trifactor type.
// The conversion from Cluster to OcMeshMeshlet lives at the one call site that is allowed to see
// both: tests/formats/src/ConvertTool.cpp, behind `#if AVER_MODULE_TRIFACTOR`.
struct OcMeshMeshlet {
    std::vector<u32> vertices;    // GLOBAL indices into this LOD's vertex buffer, <= 64 entries
    std::vector<u8>  triangles;   // LOCAL indices (0..vertices.size()-1), 3 per triangle, <= 124 tris

    Vec3 sphereCenter{0, 0, 0};
    f32  sphereRadius = 0.0f;
    Vec3 coneApex{0, 0, 0};
    i8   coneAxis[3] = {0, 0, 0};   // snorm8: value/127.0 -> [-1,1]
    i8   coneCutoff  = -127;        // snorm8; -127 (not -128) is the conservative "never cull" value

    // Per-cluster screen-space error, the two floats that turn whole-LOD selection into per-cluster
    // selection (FORMAT_SPECS.md 5.7, MeshletBounds chunk-version 2). `ownError` is this cluster's
    // own converted ScreenErrorThreshold; `parentError` is the error of the coarser cluster/group
    // this one feeds into. The runtime's local cut test is then purely per-cluster:
    //     draw this cluster  iff  ownError < pixelBudget  AND  parentError >= pixelBudget
    // which covers a mesh's surface exactly once, with no gaps and no overlap, PROVIDED
    // ownError <= parentError holds for every cluster (see aver::trifactor::
    // validateClusterErrorBounds, which is what actually enforces that at cook time).
    //
    // ROOT clusters (no coarser cluster to feed into) get parentError = +FLT_MAX, not IEEE +inf: a
    // finite comparison keeps `parentError >= pixelBudget` well-defined for any budget without
    // risking NaN/inf propagating into a shader that later does arithmetic on it, and it is exactly
    // the "always draw when nothing finer already qualified" behaviour a root needs.
    //
    // Defaults (ownError=0, parentError=FLT_MAX) are deliberately the "always drawable, and nothing
    // finer needs to exist" pair -- the same shape a root cluster gets -- so an OcMeshMeshlet built
    // by code that does not know about per-cluster LOD (a hand-built fixture, an old on-disk MLET
    // chunk version 1 read back with these fields never written) is harmless if it ever reaches the
    // local cut test, rather than defaulting to "never draw" (0, 0) or "undefined" (uninitialized).
    f32 ownError    = 0.0f;
    f32 parentError = std::numeric_limits<f32>::max();

    u32 triangleCount() const { return static_cast<u32>(triangles.size() / 3); }
};

// One coarser LOD level (level >= 1) of a Trifactor DAG, beyond the base LOD 0 that
// OcMeshData::indices/meshlets always carries. See OcMeshData::coarserLods for the full reasoning on
// why a coarser level owns its own index buffer but not its own vertex buffer.
struct OcMeshLod {
    std::vector<u32> indices;             // this level's own triangle list, global indices into `positions`
    std::vector<OcMeshMeshlet> meshlets;  // this level's meshlet partition of `indices`

    // FORMAT_SPECS.md 5.5 ScreenErrorThreshold: worldErrorCm * kReferenceProjScale, i.e. the
    // distance-independent half of screenErrorPx = worldErrorCm * projScale / distanceCm. See
    // aver::trifactor::toScreenErrorThreshold (modules/trifactor/include/aver/trifactor/
    // ClusterBuilder.hpp) for the exact formula and the reference viewport/FOV it assumes.
    f32 screenErrorThreshold = 0.0f;
};

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

    // Meshlets for LOD 0, present only when kOcMeshMeshlets is set. LOD 0 is always `indices` (above)
    // + `meshlets` (here); this is unchanged from before coarser levels existed, which is exactly
    // what keeps a single-LOD mesh's shape -- and its on-disk bytes -- identical to before this
    // feature existed. Coarser levels, when a Trifactor LOD DAG produced more than one, live in
    // `coarserLods` below.
    std::vector<OcMeshMeshlet> meshlets;

    // Coarser LOD levels, in ascending coarseness: coarserLods[0] is LOD 1 (one level up from
    // indices/meshlets above), coarserLods.back() is the coarsest / DAG root level. Empty for a
    // single-LOD mesh -- every mesh before this feature, and every mesh Trifactor's
    // buildLodHierarchy could not reduce any further (e.g. a mesh smaller than one cluster) -- and
    // that emptiness is what keeps such a mesh's LODCount at 1 and its on-disk bytes unaffected by
    // this feature existing (see writeOcMesh).
    //
    // A coarser level is a DIFFERENT triangle list, not a subset of LOD 0's (buildLodHierarchy
    // simplifies a GROUP of clusters as a unit and re-splits the result), so it carries its own
    // index buffer here. It does NOT carry its own vertex buffer: every level of a Trifactor DAG is
    // simplified and re-split against the SAME mesh.positions array (see
    // modules/trifactor/src/ClusterBuilder.cpp's file-level comment on why that is what keeps a
    // boundary vertex bit-identical at every LOD), so a coarser level's `indices` and its meshlets'
    // `vertices` reference the exact same global vertex ids LOD 0 does, and share LOD 0's on-disk
    // vertex streams (writeOcMesh writes one VTXS block; every LodDesc points at it).
    std::vector<OcMeshLod> coarserLods;

    Vec3 boundsMin{0, 0, 0}, boundsMax{0, 0, 0};
    u32  flags = 0;

    // MHDR's Reserved u32 (FORMAT_SPECS.md 5.1, offset 0x34), repurposed: which Aver.Trifactor
    // builder cooked this mesh's meshlets/coarserLods, so a tool (or later a derived-data cache) can
    // tell a STALE ladder from a CURRENT one without re-deriving the whole hierarchy just to find out.
    // Aver.Trifactor owns the actual version constant (aver::trifactor::kBuilderVersion,
    // ClusterBuilder.hpp) -- Aver.Formats sits below Aver.Trifactor in the module DAG and cannot name
    // it, so this field is just the plain u32 slot the writer copies that constant into and the
    // reader hands back untouched; see aver::trifactor::packLodDag for the one place that sets it.
    //
    // 0 IS "UNKNOWN/STALE", NEVER "CURRENT", by construction rather than by convention: every .ocmesh
    // written before this field existed has Reserved == 0 (FORMAT_SPECS.md 5.1's own "Reserved fields
    // are zero" rule, upheld by every writer that came before this one), and
    // aver::trifactor::kBuilderVersion starts at 1, not 0 -- so an old file and a current build can
    // never collide on the same value, and a reader that finds 0 here always has grounds to say "this
    // ladder's provenance is unknown, and therefore not provably current" rather than lucking into a
    // false "matches" by accident of an old field defaulting to the same number a new one starts at.
    u32 builderVersion = 0;

    // 1 (just LOD 0) + however many coarser levels are present.
    u32 lodCount() const { return 1u + static_cast<u32>(coarserLods.size()); }

    u32  vertexCount() const { return static_cast<u32>(positions.size() / 3); }
    // True when the skin streams are present and correctly sized for the vertex count.
    bool hasSkin() const {
        const usize v = positions.size() / 3;
        return v > 0 && joints.size() == v * kOcMeshInfluences && weights.size() == v * kOcMeshInfluences;
    }
    // True when at least one meshlet is present. Byte-level validity (limits, index ranges) is
    // checked by writeOcMesh/parseOcMesh, not here -- this mirrors hasSkin()'s split of "is the
    // feature present" from "is the file well-formed".
    bool hasMeshlets() const { return !meshlets.empty(); }
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
