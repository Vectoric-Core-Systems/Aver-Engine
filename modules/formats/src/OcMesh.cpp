// Reader and writer for .ocmesh: an AVR1 container holding one mesh's streams, indices and submeshes.

#include "aver/formats/OcMesh.hpp"

#include "aver/formats/Avr1.hpp"

#include <cmath>
#include <cstring>
#include "aver/platform/FileSystem.hpp"

#include <fstream>

namespace aver::fmt {
namespace {

constexpr u32 kChunkMHDR = avrFourCC("MHDR");
constexpr u32 kChunkSTRT = avrFourCC("STRT");
constexpr u32 kChunkVTXS = avrFourCC("VTXS");
constexpr u32 kChunkIDXS = avrFourCC("IDXS");
constexpr u32 kChunkMADR = avrFourCC("MADR");
constexpr u32 kChunkMLET = avrFourCC("MLET");

// MLET limits (FORMAT_SPECS.md 5.7) now live in OcMesh.hpp, so Trifactor can ALIAS them rather
// than declare a second pair. The reasoning that put them here was half right: Formats sits below
// Trifactor and cannot include its header -- but that argues against the wrong direction only, and
// the DAG running one way is exactly what lets Formats own the number. See OcMesh.hpp.

// MLET chunk versions (FORMAT_SPECS.md 5.7). Version is a property of the whole MLET chunk (every
// LOD's sub-arrays inside it share one AvrChunk::version), not per meshlet -- there is exactly one
// MLET chunk per file. v1 is every MLET chunk written before per-cluster LOD existed: MeshletBounds
// is 32 B (Sphere + ConeApex + ConeAxis/Cutoff), no error fields. v2 appends OwnError/ParentError
// (2x f32, 8 B) to MeshletBounds, making it 40 B -- additive growth per the container's own
// versioning rule (docs/formats/FORMAT_SPECS.md §3.2: "additive fields grow the struct; readers ...
// read min(known_size, on_disk_size) and zero-fill the rest"), applied here as a version-gated
// stride rather than a tail append because MeshletBounds is a repeated array, not a single struct --
// the effect is the same: an old (v1) file's bytes are read with the old 32 B stride exactly as
// before, unaffected by this feature existing, and OwnError/ParentError simply default (see
// OcMeshMeshlet's own defaults) rather than appearing from nowhere.
//
// v3 (Stage 4) appends fallbackAncestorId/ownerGroupId (2x u32, 8 B) to MeshletBounds the SAME
// additive way, making it 48 B, and ALSO grows the chunk with two brand-new per-LOD sub-arrays
// (ClusterGroupNode[], ClusterGroupChildren[]) that v1/v2 never had at all -- see the GroupTable
// comment at this chunk's write site below for where those live and how a v1/v2 reader, which never
// looks for a GroupTable, is unaffected by its presence. AN OLD FILE'S CHUNK VERSION NEVER CHANGES:
// nothing in this codebase rewrites an .ocmesh's bytes just because a newer writer exists (RelodTool
// --write is opt-in and writes elsewhere -- see tools/RelodTool.cpp's own header), so a v1 or v2 file
// on disk today stays v1/v2 forever unless something explicitly re-cooks and re-saves it; this
// reader's job is only to keep loading it correctly regardless, which the version-gated stride and
// GroupTable-presence check below both do without needing the file itself to change.
constexpr u16 kMlChunkVersionLegacy  = 1;   // MeshletBounds 32 B, no OwnError/ParentError
constexpr u16 kMlChunkVersionErrors  = 2;   // MeshletBounds 40 B, + OwnError, ParentError
constexpr u16 kMlChunkVersionTopology = 3;  // MeshletBounds 48 B, + FallbackAncestorId/OwnerGroupId,
                                             // + a per-LOD GroupTable and ClusterGroupNode[]/
                                             // ClusterGroupChildren[] sub-arrays (see the writer)
constexpr u32 kMeshletBoundsBytesV1 = 32;
constexpr u32 kMeshletBoundsBytesV2 = 40;
constexpr u32 kMeshletBoundsBytesV3 = 48;

// One ClusterGroupNode's on-disk size (FORMAT_SPECS.md 5.7): Sphere (16 B) + OwnClusterStart/Count
// (8 B) + ChildClusterStart/Count (8 B) = 32 B. A fixed struct, unlike MeshletBounds, because chunk-
// version 3 is the version that INTRODUCES this sub-array -- there is no earlier layout for it to
// grow additively out of, so it does not need its own version-gated stride the way MeshletBounds
// does.
constexpr u32 kGroupNodeBytes = 32;
// One GroupTable entry (per LOD level, chunk-version 3 only): GroupNodeOffset (u64, chunk-relative)
// + GroupNodeCount (u32) + GroupChildOffset (u64, chunk-relative) + GroupChildCount (u32) = 24 B.
constexpr u32 kGroupTableEntryBytes = 24;

// Stream semantics and formats (§5.2, §5.3).
constexpr u8 kSemPosition = 0, kSemTangentFrame = 1, kSemUV0 = 2, kSemJoints = 5, kSemWeights = 6;
constexpr u8 kFmtR32G32B32F = 0, kFmtR16G16B16A16S = 1, kFmtR16G16F = 2;
constexpr u8 kFmtR8G8B8A8Unorm = 4, kFmtR16G16B16A16Uint = 7;

// JOINTS (4xu16) then WEIGHTS (4xu8) share one slot, the way UV0 rides inside the attribute slot.
constexpr u16 kSkinStride = 12, kSkinWeightOffset = 8;

// Sets `why` and returns false.
bool fail(std::string* why, std::string m) { if (why) *why = std::move(m); return false; }

// Converts a float to IEEE half. No subnormal or NaN handling.
u16 floatToHalf(f32 f) {
    u32 x; std::memcpy(&x, &f, 4);
    const u32 sign = (x >> 16) & 0x8000u;
    i32 exp = i32((x >> 23) & 0xFF) - 127 + 15;
    u32 man = x & 0x7FFFFFu;
    if (exp <= 0) return u16(sign);                       // underflow to signed zero
    if (exp >= 31) return u16(sign | 0x7C00u);            // overflow to infinity
    return u16(sign | (u32(exp) << 10) | (man >> 13));
}
// Converts an IEEE half back to a float.
f32 halfToFloat(u16 h) {
    const u32 sign = u32(h & 0x8000u) << 16;
    const u32 exp  = (h >> 10) & 0x1Fu;
    const u32 man  = h & 0x3FFu;
    u32 out;
    if (exp == 0)        out = sign;                                   // zero / subnormal -> zero
    else if (exp == 31)  out = sign | 0x7F800000u | (man << 13);       // inf / nan
    else                 out = sign | ((exp - 15 + 127) << 23) | (man << 13);
    f32 f; std::memcpy(&f, &out, 4); return f;
}

// Clamps to [-1, 1] and encodes as signed-normalised 16-bit.
inline i16 toSnorm16(f32 v) {
    v = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<i16>(std::lround(v * 32767.0f));
}
inline f32 fromSnorm16(i16 v) { const f32 f = static_cast<f32>(v) / 32767.0f; return f < -1.0f ? -1.0f : f; }

// Encodes a normal as a QTangent (§5.4): the shortest-arc rotation from +Z to the normal.
void normalToQTangent(const f32 n[3], i16 out[4]) {
    const f32 x = n[0], y = n[1], z = n[2];
    f32 qx, qy, qz, qw;
    const f32 d = z;                       // dot(+Z, n)
    if (d < -0.999999f) { qx = 1.0f; qy = 0.0f; qz = 0.0f; qw = 0.0f; }
    else {
        // axis = cross(+Z, n) = (-y, x, 0)
        qx = -y; qy = x; qz = 0.0f; qw = 1.0f + d;
        const f32 len = std::sqrt(qx*qx + qy*qy + qz*qz + qw*qw);
        const f32 inv = len > 1e-20f ? 1.0f / len : 0.0f;
        qx *= inv; qy *= inv; qz *= inv; qw *= inv;
    }
    // The spec folds bitangent handedness into w's sign, so w is never stored negative here.
    if (qw < 0.0f) { qx = -qx; qy = -qy; qz = -qz; qw = -qw; }
    out[0] = toSnorm16(qx); out[1] = toSnorm16(qy); out[2] = toSnorm16(qz); out[3] = toSnorm16(qw);
}

// Recovers the normal from a QTangent.
void qTangentToNormal(const i16 q[4], f32 n[3]) {
    f32 x = fromSnorm16(q[0]), y = fromSnorm16(q[1]), z = fromSnorm16(q[2]), w = fromSnorm16(q[3]);
    const f32 len = std::sqrt(x*x + y*y + z*z + w*w);
    const f32 inv = len > 1e-20f ? 1.0f / len : 0.0f;
    x *= inv; y *= inv; z *= inv; w *= inv;
    // rotate(q, +Z)
    n[0] = 2.0f * (x*z + w*y);
    n[1] = 2.0f * (y*z - w*x);
    n[2] = 1.0f - 2.0f * (x*x + y*y);
    const f32 nl = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
    if (nl > 1e-20f) { n[0] /= nl; n[1] /= nl; n[2] /= nl; } else { n[0] = 0; n[1] = 0; n[2] = 1; }
}

// Little-endian byte writer over a growing buffer.
struct W {
    std::vector<u8>& b;
    void u8v (u8 v)  { b.push_back(v); }
    void u16v(u16 v) { b.push_back(u8(v)); b.push_back(u8(v >> 8)); }
    void u32v(u32 v) { for (int i = 0; i < 4; ++i) b.push_back(u8(v >> (i * 8))); }
    void u64v(u64 v) { for (int i = 0; i < 8; ++i) b.push_back(u8(v >> (i * 8))); }
    void f32v(f32 v) { u32 x; std::memcpy(&x, &v, 4); u32v(x); }
    void i16v(i16 v) { u16v(static_cast<u16>(v)); }
    void pad16()     { while (b.size() % 16) b.push_back(0); }
};

// Bounds-checked little-endian byte reader. `ok` goes false on the first short read.
struct R {
    const u8* p; const u8* e; bool ok = true;
    bool need(usize n) { if (usize(e - p) < n) { ok = false; return false; } return true; }
    u8  u8v () { if (!need(1)) return 0; return *p++; }
    u16 u16v() { if (!need(2)) return 0; u16 v = u16(p[0]) | u16(u16(p[1]) << 8); p += 2; return v; }
    u32 u32v() { if (!need(4)) return 0; u32 v; std::memcpy(&v, p, 4); p += 4; return v; }
    u64 u64v() { if (!need(8)) return 0; u64 v; std::memcpy(&v, p, 8); p += 8; return v; }
    f32 f32v() { u32 x = u32v(); f32 f; std::memcpy(&f, &x, 4); return f; }
    i16 i16v() { return static_cast<i16>(u16v()); }
};

} // namespace

// Recomputes the axis-aligned bounds from the position stream.
void OcMeshData::computeBounds() {
    if (positions.size() < 3) { boundsMin = boundsMax = Vec3{0, 0, 0}; return; }
    Vec3 lo{positions[0], positions[1], positions[2]}, hi = lo;
    for (usize i = 3; i + 2 < positions.size(); i += 3) {
        lo.x = std::fmin(lo.x, positions[i]);     hi.x = std::fmax(hi.x, positions[i]);
        lo.y = std::fmin(lo.y, positions[i + 1]); hi.y = std::fmax(hi.y, positions[i + 1]);
        lo.z = std::fmin(lo.z, positions[i + 2]); hi.z = std::fmax(hi.z, positions[i + 2]);
    }
    boundsMin = lo; boundsMax = hi;
}

// Encodes a mesh into an .ocmesh container. Returns false with `why` set on invalid input.
bool writeOcMesh(const OcMeshData& in, std::vector<u8>& out, std::string* why) {
    if (!in.valid()) return fail(why, ".ocmesh: mesh has no vertices, no indices, or mismatched attribute counts");
    if (in.submeshes.empty()) return fail(why, ".ocmesh: at least one submesh is required");
    if (in.submeshes.size() > 255) return fail(why, ".ocmesh: more than 255 submeshes");
    if (in.lodCount() > 255) return fail(why, ".ocmesh: more than 255 LODs");

    OcMeshData m = in;
    m.computeBounds();

    const u32 vcount = m.vertexCount();
    const bool index32 = vcount > 0xFFFF;
    const bool skinned = m.hasSkin();

    // ---- per-LOD views: LOD 0 is `indices`/`meshlets` (unchanged from before coarser levels
    // existed), LOD 1+ are `coarserLods`. Every level shares the SAME vertex buffer (see
    // OcMeshData::coarserLods), so only indices/meshlets/screenError vary per level below. ----
    struct LodView {
        const std::vector<u32>* indices;
        const std::vector<OcMeshMeshlet>* meshlets;
        f32 screenError;
        // Both null for LOD 0: OcMeshData has no groupNodes/groupChildren field of its own (LOD 0 is
        // never produced by a group -- see OcMeshMeshlet::ownerGroupId's own comment), so there is no
        // member to point at. The MLET writer below treats a null pointer here exactly like an empty
        // vector (zero group nodes for this level), never dereferencing it.
        const std::vector<OcMeshClusterGroup>* groupNodes;
        const std::vector<u32>* groupChildren;
    };
    std::vector<LodView> lods;
    lods.reserve(m.lodCount());
    lods.push_back({&m.indices, &m.meshlets, 0.0f, nullptr, nullptr});   // LOD 0: no measured error, no group data
    for (const OcMeshLod& cl : m.coarserLods)
        lods.push_back({&cl.indices, &cl.meshlets, cl.screenErrorThreshold, &cl.groupNodes, &cl.groupChildren});

    // ---- per-level validation, before any output buffer is built (fail fast, like the checks
    // below): meshlet limits/ranges over every level's meshlets, and index range/multiple-of-3 over
    // every level's OWN triangle list (LOD 0's is covered by in.valid() already, but a coarser
    // level's is not, and a bad one here would walk the MLET/IDXS writers off the end of `positions`
    // just as surely as a bad LOD-0 index would). ----
    for (usize L = 0; L < lods.size(); ++L) {
        const std::vector<u32>& idx = *lods[L].indices;
        if (L > 0) {
            if (idx.empty()) return fail(why, ".ocmesh: LOD " + std::to_string(L) + " has no indices");
            if (idx.size() % 3 != 0)
                return fail(why, ".ocmesh: LOD " + std::to_string(L) + "'s index count is not a multiple of 3");
            for (u32 v : idx)
                if (v >= vcount)
                    return fail(why, ".ocmesh: LOD " + std::to_string(L) + " references vertex " +
                                          std::to_string(v) + ", past the mesh's " + std::to_string(vcount) + " vertices");
        }
        const std::vector<OcMeshMeshlet>& mls = *lods[L].meshlets;
        for (usize mi = 0; mi < mls.size(); ++mi) {
            const OcMeshMeshlet& ml = mls[mi];
            if (ml.vertices.size() > kMaxMeshletVertices)
                return fail(why, ".ocmesh: LOD " + std::to_string(L) + " meshlet " + std::to_string(mi) +
                                      " has more than " + std::to_string(kMaxMeshletVertices) + " vertices");
            if (ml.triangles.size() % 3 != 0)
                return fail(why, ".ocmesh: LOD " + std::to_string(L) + " meshlet " + std::to_string(mi) +
                                      "'s triangle list is not a multiple of 3");
            if (ml.triangleCount() > kMaxMeshletTriangles)
                return fail(why, ".ocmesh: LOD " + std::to_string(L) + " meshlet " + std::to_string(mi) +
                                      " has more than " + std::to_string(kMaxMeshletTriangles) + " triangles");
            for (u32 v : ml.vertices)
                if (v >= vcount)
                    return fail(why, ".ocmesh: LOD " + std::to_string(L) + " meshlet " + std::to_string(mi) +
                                          " references vertex " + std::to_string(v) + ", past the mesh's " +
                                          std::to_string(vcount) + " vertices");
            for (u8 t : ml.triangles)
                if (t >= ml.vertices.size())
                    return fail(why, ".ocmesh: LOD " + std::to_string(L) + " meshlet " + std::to_string(mi) +
                                          " has a local triangle index " + std::to_string(t) + " past its own " +
                                          std::to_string(ml.vertices.size()) + " vertices");

            // ---- MLET chunk-version 3: fallbackAncestorId/ownerGroupId, validated for the same
            // reason every other range in this loop is -- a bad value here would let the group-table
            // offset arithmetic below (or a future reader) walk off the end of a buffer. See
            // OcMeshMeshlet's own comment for what these two fields mean and why kInvalidClusterId is
            // their required value at a level that structurally cannot have a real one. ----
            const bool isRootLevel = (L + 1 == lods.size());
            if (isRootLevel) {
                if (ml.fallbackAncestorId != kInvalidClusterId)
                    return fail(why, ".ocmesh: LOD " + std::to_string(L) + " meshlet " + std::to_string(mi) +
                                          " is at the root level but has a fallbackAncestorId set -- a root has "
                                          "no coarser level to fall back to");
            } else {
                const usize nextLevelCount = lods[L + 1].meshlets->size();
                if (ml.fallbackAncestorId != kInvalidClusterId && ml.fallbackAncestorId >= nextLevelCount)
                    return fail(why, ".ocmesh: LOD " + std::to_string(L) + " meshlet " + std::to_string(mi) +
                                          "'s fallbackAncestorId " + std::to_string(ml.fallbackAncestorId) +
                                          " is past LOD " + std::to_string(L + 1) + "'s " +
                                          std::to_string(nextLevelCount) + " meshlets");
            }
            if (L == 0) {
                if (ml.ownerGroupId != kInvalidClusterId)
                    return fail(why, ".ocmesh: LOD 0 meshlet " + std::to_string(mi) +
                                          " has an ownerGroupId set, but LOD 0 is never produced by a group");
            } else {
                const usize groupCount = lods[L].groupNodes ? lods[L].groupNodes->size() : 0;
                if (ml.ownerGroupId != kInvalidClusterId && ml.ownerGroupId >= groupCount)
                    return fail(why, ".ocmesh: LOD " + std::to_string(L) + " meshlet " + std::to_string(mi) +
                                          "'s ownerGroupId " + std::to_string(ml.ownerGroupId) +
                                          " is past its own level's " + std::to_string(groupCount) + " group nodes");
            }
        }

        // ---- MLET chunk-version 3: the group hierarchy itself, one level's worth. Null for LOD 0 by
        // construction (see LodView's own comment), so this is skipped entirely there rather than
        // dereferencing a null groupChildren pointer. ----
        if (lods[L].groupNodes) {
            const std::vector<OcMeshClusterGroup>& nodes = *lods[L].groupNodes;
            const std::vector<u32>& children = *lods[L].groupChildren;
            const usize finerCount = (L > 0) ? lods[L - 1].meshlets->size() : 0;
            for (usize gi = 0; gi < nodes.size(); ++gi) {
                const OcMeshClusterGroup& g = nodes[gi];
                if (u64(g.ownClusterStart) + g.ownClusterCount > mls.size())
                    return fail(why, ".ocmesh: LOD " + std::to_string(L) + " group " + std::to_string(gi) +
                                          "'s ownClusterRange runs past this level's " + std::to_string(mls.size()) +
                                          " meshlets");
                if (u64(g.childClusterStart) + g.childClusterCount > children.size())
                    return fail(why, ".ocmesh: LOD " + std::to_string(L) + " group " + std::to_string(gi) +
                                          "'s childClusterRange runs past its own " + std::to_string(children.size()) +
                                          "-entry groupChildren array");
                for (u32 k = 0; k < g.childClusterCount; ++k) {
                    const u32 childId = children[usize(g.childClusterStart) + k];
                    if (childId >= finerCount)
                        return fail(why, ".ocmesh: LOD " + std::to_string(L) + " group " + std::to_string(gi) +
                                              " references child cluster " + std::to_string(childId) + ", past LOD " +
                                              std::to_string(L > 0 ? L - 1 : 0) + "'s " + std::to_string(finerCount) +
                                              " meshlets");
                }
            }
        }
    }

    AvrStringTable strt;
    std::vector<u32> nameRefs;
    nameRefs.reserve(m.submeshes.size());
    for (const OcMeshSubmesh& s : m.submeshes) nameRefs.push_back(strt.add(s.name));

    // ---- VTXS: slot 0 is position alone, slot 1 interleaves tangent frame and UV0 (§5.2) ----
    std::vector<u8> vtxs;
    {
        W w{vtxs};
        for (u32 i = 0; i < vcount; ++i) {
            w.f32v(m.positions[i * 3 + 0]); w.f32v(m.positions[i * 3 + 1]); w.f32v(m.positions[i * 3 + 2]);
        }
        w.pad16();
        for (u32 i = 0; i < vcount; ++i) {
            i16 q[4];
            const f32 n[3] = {m.normals[i * 3 + 0], m.normals[i * 3 + 1], m.normals[i * 3 + 2]};
            normalToQTangent(n, q);
            w.i16v(q[0]); w.i16v(q[1]); w.i16v(q[2]); w.i16v(q[3]);
            w.u16v(floatToHalf(m.uvs[i * 2 + 0]));
            w.u16v(floatToHalf(m.uvs[i * 2 + 1]));
        }
        w.pad16();

        // ---- slot 2: JOINTS then WEIGHTS, when the mesh is skinned (5.2/5.3) ----
        if (skinned) {
            for (u32 i = 0; i < vcount; ++i) {
                for (u32 k = 0; k < kOcMeshInfluences; ++k) w.u16v(m.joints[i * kOcMeshInfluences + k]);

                // Quantise to 8 bits, then SPEND THE REMAINDER ON THE LARGEST influence so the four
                // still sum to exactly 255. Rounding each independently loses up to 2/255 of a
                // vertex's weight, which reads as a skinned mesh that quietly shrinks at the joints.
                f32 sum = 0.0f;
                for (u32 k = 0; k < kOcMeshInfluences; ++k) sum += m.weights[i * kOcMeshInfluences + k];
                const f32 inv = sum > 1e-8f ? 1.0f / sum : 0.0f;

                u32 q[kOcMeshInfluences] = {};
                u32 total = 0, biggest = 0;
                for (u32 k = 0; k < kOcMeshInfluences; ++k) {
                    const f32 wn = m.weights[i * kOcMeshInfluences + k] * inv;
                    q[k] = static_cast<u32>(std::lround(wn * 255.0f));
                    if (q[k] > 255u) q[k] = 255u;
                    total += q[k];
                    if (q[k] > q[biggest]) biggest = k;
                }
                if (sum <= 1e-8f) { q[0] = 255; total = 255; biggest = 0; }   // an unweighted vertex
                const i32 slack = 255 - static_cast<i32>(total);
                const i32 fixed = static_cast<i32>(q[biggest]) + slack;
                q[biggest] = static_cast<u32>(fixed < 0 ? 0 : (fixed > 255 ? 255 : fixed));

                for (u32 k = 0; k < kOcMeshInfluences; ++k) w.u8v(static_cast<u8>(q[k]));
            }
            w.pad16();
        }
    }

    // ---- IDXS: one block per LOD, back to back. A coarser LOD is a DIFFERENT triangle list (not a
    // subset of LOD 0's -- buildLodHierarchy re-splits a simplified GROUP into new clusters), so it
    // needs its own slice of IDXS; LodDesc[L].IdxOffset/IdxSize below records where each one landed.
    // For a single-LOD mesh (lods.size() == 1, every mesh before this feature) this reduces to
    // exactly the single write + one pad16() the format always had -- IdxOffset 0, IdxSize
    // idxs.size() -- so a meshlet-free or LOD-0-only mesh's bytes here are unchanged. ----
    std::vector<u8> idxs;
    struct IdxPlacement { u64 offset = 0, size = 0; };
    std::vector<IdxPlacement> idxPlacement(lods.size());
    {
        W w{idxs};
        for (usize L = 0; L < lods.size(); ++L) {
            const u64 start = idxs.size();
            for (const u32 idx : *lods[L].indices) { if (index32) w.u32v(idx); else w.u16v(static_cast<u16>(idx)); }
            w.pad16();
            idxPlacement[L] = {start, static_cast<u64>(idxs.size()) - start};
        }
    }

    // ---- MADR: slot index -> surface name ----
    std::vector<u8> madr;
    {
        W w{madr};
        w.u32v(static_cast<u32>(m.materialSlots.size()));
        for (const std::string& s : m.materialSlots) w.u32v(strt.add(s));
    }

    // ---- MLET: meshlets (§5.7), four back-to-back sub-arrays PER LOD, only when at least one level
    // has meshlets. Each level's four sub-arrays are self-contained -- MeshletDesc's
    // VertexIndexOffset/TriangleOffset are relative to THAT LEVEL's own sub-array 3/4, restarting at
    // 0 for every level -- matching §5.7's "offsets from the LOD's MeshletOffset". For a single-LOD
    // mesh this is exactly the one block the format always wrote. ----
    const bool anyMeshlets = [&] {
        for (const LodView& l : lods) if (!l.meshlets->empty()) return true;
        return false;
    }();
    std::vector<u8> mlet;
    std::vector<u64> meshletOffsetPerLod(lods.size(), 0);
    std::vector<u32> meshletCountPerLod(lods.size(), 0);
    if (anyMeshlets) {
        // ---- meshlet section: the same four sub-arrays PER LOD this chunk has always written, now
        // with MeshletBounds grown to 48 B (chunk version 3: the 40 B version-2 shape, then
        // FallbackAncestorId/OwnerGroupId appended -- see kMlChunkVersionTopology above). Built into
        // its OWN buffer first, offsets relative to ITS start, because chunk-version 3 prepends a
        // GroupTable (below) whose own size is not known until every level's group data has ALSO been
        // laid out -- exactly the same reason meshletOffsetPerLod ends up needing to be shifted by
        // `groupTableBytes` once, near the bottom of this block, rather than accumulated directly
        // against the final chunk buffer the way it was before chunk version 3 existed. ----
        std::vector<u8> meshletSection;
        for (usize L = 0; L < lods.size(); ++L) {
            const std::vector<OcMeshMeshlet>& mls = *lods[L].meshlets;
            meshletOffsetPerLod[L] = meshletSection.size();
            meshletCountPerLod[L]  = static_cast<u32>(mls.size());
            if (mls.empty()) continue;   // offset recorded, but nothing to write for this level

            // VertexIndexOffset (elements, into sub-array 3) and TriangleOffset (bytes, into
            // sub-array 4, each meshlet's triangle block padded to 4 B) are computed here, in writer
            // order -- nothing in OcMeshMeshlet stores them, because they are a function of where a
            // meshlet lands among its siblings, not a property of the meshlet itself.
            struct Placement { u32 vertexIndexOffset; u32 triangleOffset; };
            std::vector<Placement> placement(mls.size());
            u32 vtxCursor = 0, triCursor = 0;
            for (usize i = 0; i < mls.size(); ++i) {
                const OcMeshMeshlet& ml = mls[i];
                placement[i].vertexIndexOffset = vtxCursor;
                placement[i].triangleOffset    = triCursor;
                vtxCursor += static_cast<u32>(ml.vertices.size());
                triCursor += (static_cast<u32>(ml.triangles.size()) + 3u) & ~3u;
            }

            W w{meshletSection};
            // Sub-array 1: MeshletDesc[] (12 B each).
            for (usize i = 0; i < mls.size(); ++i) {
                const OcMeshMeshlet& ml = mls[i];
                w.u32v(placement[i].vertexIndexOffset);
                w.u32v(placement[i].triangleOffset);
                w.u8v(static_cast<u8>(ml.vertices.size()));
                w.u8v(static_cast<u8>(ml.triangleCount()));
                w.u16v(0);   // Pad
            }
            // Sub-array 2: MeshletBounds[] (48 B each, chunk version 3: the 32 B Sphere + ConeApex +
            // ConeAxis/Cutoff, then OwnError/ParentError (version 2), then FallbackAncestorId/
            // OwnerGroupId (version 3) appended -- see kMlChunkVersionTopology above).
            for (const OcMeshMeshlet& ml : mls) {
                w.f32v(ml.sphereCenter.x); w.f32v(ml.sphereCenter.y); w.f32v(ml.sphereCenter.z);
                w.f32v(ml.sphereRadius);
                w.f32v(ml.coneApex.x); w.f32v(ml.coneApex.y); w.f32v(ml.coneApex.z);
                w.u8v(static_cast<u8>(ml.coneAxis[0])); w.u8v(static_cast<u8>(ml.coneAxis[1]));
                w.u8v(static_cast<u8>(ml.coneAxis[2])); w.u8v(static_cast<u8>(ml.coneCutoff));
                w.f32v(ml.ownError);
                w.f32v(ml.parentError);
                w.u32v(ml.fallbackAncestorId);
                w.u32v(ml.ownerGroupId);
            }
            // Sub-array 3: MeshletVertices[] (u32 each), all this level's meshlets' vertex lists back
            // to back.
            for (const OcMeshMeshlet& ml : mls)
                for (u32 v : ml.vertices) w.u32v(v);
            // Sub-array 4: MeshletTriangles[] (u8, 3 per triangle), each meshlet's block zero-padded
            // to a 4 B boundary -- must match the `triCursor` padding computed above exactly, or
            // every TriangleOffset after the first meshlet whose triangle count isn't a multiple of
            // 4 bytes would point at the wrong meshlet's data.
            const usize triSectionStart = meshletSection.size();
            for (const OcMeshMeshlet& ml : mls) {
                for (u8 t : ml.triangles) w.u8v(t);
                while ((meshletSection.size() - triSectionStart) % 4 != 0) w.u8v(0);
            }
        }

        // ---- group section: ClusterGroupNode[] then ClusterGroupChildren[] PER LOD, back to back,
        // in its own buffer for the same reason the meshlet section above is -- its final chunk-
        // relative offset depends on the GroupTable's size, computed below. Empty (both offset
        // vectors stay all-zero) for LOD 0 (groupNodes is null there -- see LodView's own comment)
        // and for any level nobody ever put group data on (a hand-built fixture, or the one-cluster
        // degenerate case where buildLodHierarchy never ran at all). ----
        std::vector<u8> groupSection;
        std::vector<u64> groupNodeOffsetPerLod(lods.size(), 0), groupChildOffsetPerLod(lods.size(), 0);
        std::vector<u32> groupNodeCountPerLod(lods.size(), 0), groupChildCountPerLod(lods.size(), 0);
        for (usize L = 0; L < lods.size(); ++L) {
            if (!lods[L].groupNodes || lods[L].groupNodes->empty()) continue;
            const std::vector<OcMeshClusterGroup>& nodes = *lods[L].groupNodes;
            const std::vector<u32>& children = *lods[L].groupChildren;

            groupNodeOffsetPerLod[L] = groupSection.size();
            groupNodeCountPerLod[L]  = static_cast<u32>(nodes.size());
            W w{groupSection};
            for (const OcMeshClusterGroup& g : nodes) {
                w.f32v(g.sphereCenter.x); w.f32v(g.sphereCenter.y); w.f32v(g.sphereCenter.z);
                w.f32v(g.sphereRadius);
                w.u32v(g.ownClusterStart); w.u32v(g.ownClusterCount);
                w.u32v(g.childClusterStart); w.u32v(g.childClusterCount);
            }

            groupChildOffsetPerLod[L] = groupSection.size();
            groupChildCountPerLod[L]  = static_cast<u32>(children.size());
            for (u32 v : children) w.u32v(v);
        }

        // ---- assemble: GroupTable, then the meshlet section, then the group section -- in that
        // order so the GroupTable (whose per-LOD entries name chunk-relative offsets into the OTHER
        // two sections) can sit at a FIXED, version-gated location (byte 0) that a chunk-version 1/2
        // reader, which never looks for it, simply never reads. A v1/v2 chunk has no such prefix, so
        // its own meshletOffsetPerLod entries (computed the OLD way, directly against `mlet`) would
        // be wrong by exactly `groupTableBytes` if this writer ever emitted anything but version 3 --
        // it does not (anyMeshlets always writes version 3 now), so that mismatch cannot occur here,
        // but see decodeMeshlets/decodeGroupNodes in parseOcMesh for the reader half of this contract.
        const u64 groupTableBytes = u64(kGroupTableEntryBytes) * lods.size();
        {
            W w{mlet};
            for (usize L = 0; L < lods.size(); ++L) {
                const u64 gnOff = groupNodeCountPerLod[L]
                    ? groupTableBytes + meshletSection.size() + groupNodeOffsetPerLod[L] : 0;
                const u64 gcOff = groupChildCountPerLod[L]
                    ? groupTableBytes + meshletSection.size() + groupChildOffsetPerLod[L] : 0;
                w.u64v(gnOff);
                w.u32v(groupNodeCountPerLod[L]);
                w.u64v(gcOff);
                w.u32v(groupChildCountPerLod[L]);
            }
        }
        mlet.insert(mlet.end(), meshletSection.begin(), meshletSection.end());
        mlet.insert(mlet.end(), groupSection.begin(), groupSection.end());

        // meshletOffsetPerLod was computed against meshletSection's own start (0); shift every entry
        // by the GroupTable's byte length so LodDesc.MeshletOffset (still read as "chunk-relative",
        // unchanged semantics) lands on the right bytes in the FINAL chunk.
        for (usize L = 0; L < lods.size(); ++L) meshletOffsetPerLod[L] += groupTableBytes;
    }

    // ---- MHDR ----
    std::vector<u8> mhdr;
    {
        W w{mhdr};
        u32 flags = m.flags & ~(kOcMeshIndex32 | kOcMeshMeshlets | kOcMeshHasColor | kOcMeshHasUV1 | kOcMeshHasSkin);
        if (index32) flags |= kOcMeshIndex32;
        if (skinned) flags |= kOcMeshHasSkin;
        if (anyMeshlets) flags |= kOcMeshMeshlets;
        w.u32v(flags);
        w.u8v(static_cast<u8>(lods.size()));                  // LODCount
        w.u8v(static_cast<u8>(m.submeshes.size()));            // SubmeshCount
        w.u8v(skinned ? 4 : 2);                               // StreamCount: +JOINTS +WEIGHTS
        w.u8v(1);                                             // UVChannelCount
        w.f32v(m.boundsMin.x); w.f32v(m.boundsMin.y); w.f32v(m.boundsMin.z);
        w.f32v(m.boundsMax.x); w.f32v(m.boundsMax.y); w.f32v(m.boundsMax.z);
        // BoundsSphere: centre + radius, derived from the box.
        const Vec3 c{(m.boundsMin.x + m.boundsMax.x) * 0.5f, (m.boundsMin.y + m.boundsMax.y) * 0.5f,
                     (m.boundsMin.z + m.boundsMax.z) * 0.5f};
        const f32 rx = m.boundsMax.x - c.x, ry = m.boundsMax.y - c.y, rz = m.boundsMax.z - c.z;
        w.f32v(c.x); w.f32v(c.y); w.f32v(c.z); w.f32v(std::sqrt(rx*rx + ry*ry + rz*rz));
        w.u32v(static_cast<u32>(m.materialSlots.size()));      // MaterialSlotCount
        // Reserved, repurposed as BuilderVersion (see OcMeshData::builderVersion's own comment): a
        // caller that never cooked meshlets through aver::trifactor::packLodDag leaves
        // m.builderVersion at its default (0), so a plain mesh (or one loaded from an old file and
        // re-saved untouched) writes exactly the "Reserved fields are zero" byte FORMAT_SPECS.md
        // always required here -- this field's existence changes nothing for a caller that does not
        // know about it.
        w.u32v(m.builderVersion);                              // Reserved / BuilderVersion

        // StreamDesc[2] (§5.2)
        const u16 attrStride = 12;   // QTangent 8 + UV0 4
        w.u8v(kSemPosition);     w.u8v(kFmtR32G32B32F);   w.u8v(0); w.u8v(0); w.u16v(12);         w.u16v(0);
        w.u8v(kSemTangentFrame); w.u8v(kFmtR16G16B16A16S); w.u8v(1); w.u8v(1); w.u16v(attrStride); w.u16v(0);
        // UV0 has no descriptor of its own: it lives inside slot 1's stride at offset +8.
        if (skinned) {
            w.u8v(kSemJoints);  w.u8v(kFmtR16G16B16A16Uint); w.u8v(2); w.u8v(3);
            w.u16v(kSkinStride); w.u16v(0);
            w.u8v(kSemWeights); w.u8v(kFmtR8G8B8A8Unorm);    w.u8v(2); w.u8v(4);
            w.u16v(kSkinStride); w.u16v(kSkinWeightOffset);
        }

        // LodDesc[LODCount] (§5.5). VertexCount/VtxOffset/VtxSize are the SAME for every level --
        // every level of a Trifactor DAG shares LOD 0's vertex buffer (see OcMeshData::coarserLods)
        // -- so only IdxOffset/IdxSize/MeshletOffset/MeshletCount/ScreenErrorThreshold vary per LOD.
        // For a single-LOD mesh (lods.size() == 1) this writes exactly the bytes the format always
        // wrote: IdxOffset 0, MeshletOffset 0 when empty, ScreenErrorThreshold 0.0f.
        const u64 posBytes  = u64(vcount) * 12;
        const u64 posPadded = (posBytes + 15) & ~u64(15);
        (void)posPadded;
        for (usize L = 0; L < lods.size(); ++L) {
            w.u32v(vcount);
            w.u32v(static_cast<u32>(lods[L].indices->size()));
            w.u64v(0);                              // VtxOffset -- shared vertex block, every LOD
            w.u64v(static_cast<u64>(vtxs.size()));  // VtxSize
            w.u64v(idxPlacement[L].offset);          // IdxOffset (chunk-relative, into IDXS)
            w.u64v(idxPlacement[L].size);            // IdxSize
            w.u64v(meshletOffsetPerLod[L]);          // MeshletOffset (chunk-relative, into MLET)
            w.u32v(meshletCountPerLod[L]);           // MeshletCount
            w.f32v(lods[L].screenError);             // ScreenErrorThreshold
        }

        // SubmeshDesc[] (§5.6)
        for (usize i = 0; i < m.submeshes.size(); ++i) {
            w.u32v(m.submeshes[i].materialSlot);
            w.u32v(nameRefs[i]);
            w.u32v(0);                               // Flags
        }
        // SubmeshRange (§5.6). Spec-literal layout is [LODCount*SubmeshCount], one 40 B block per
        // (LOD, submesh) pair -- but this writer emits exactly SubmeshCount blocks, describing LOD
        // 0's partition ONLY, regardless of LODCount. Deliberate, for the same reason
        // MeshletStart/MeshletCount stay 0,0 below: buildLodHierarchy (Aver.Trifactor) simplifies the
        // mesh's WHOLE merged index buffer as one unit per group, with no regard to submesh/material
        // boundaries, so there is no honest per-submesh [start,count) range to report for LOD >= 1 --
        // fabricating one (e.g. by copying LOD 0's ranges, or scaling them) would be worse than
        // omitting it. Fixing this needs submesh-aware clustering upstream in Trifactor, out of this
        // slice's scope. The reader below reads exactly SubmeshCount entries to match, so this is
        // self-consistent rather than a spec violation waiting to desync a reader -- see parseOcMesh.
        //
        // MeshletStart/MeshletCount stay 0,0 here EVEN WHEN m.meshlets is non-empty, for the same
        // "no honest per-submesh range" reason: buildClusters (Aver.Trifactor) partitions the mesh's
        // WHOLE merged index buffer into meshlets without regard to submesh/material boundaries, so a
        // meshlet can straddle two submeshes. A renderer that wants "meshlets for submesh N" cannot
        // yet get that from this file; it can only draw a whole LOD's meshlet set.
        for (const OcMeshSubmesh& s : m.submeshes) {
            w.u32v(s.indexStart); w.u32v(s.indexCount);
            w.u32v(s.baseVertex); w.u32v(s.vertexCount);
            w.f32v(0); w.f32v(0); w.f32v(0); w.f32v(0);   // per-submesh bounds sphere: not computed
            w.u32v(0); w.u32v(0);                          // meshlet start/count: see comment above
        }
    }

    Avr1File f;
    f.subtype = kAvrSubtypeMesh;
    f.contentVersion = 1;
    f.flags = kAvrFlagCooked;
    f.add(kChunkMHDR, std::move(mhdr), kAvrChunkRequired);
    f.add(kChunkSTRT, strt.bytes());
    f.add(kChunkVTXS, std::move(vtxs), kAvrChunkGpuUploadable);
    f.add(kChunkIDXS, std::move(idxs), kAvrChunkGpuUploadable);
    f.add(kChunkMADR, std::move(madr));
    // MLET is genuinely OPTIONAL (§5): omitted entirely, not written empty, when there are no
    // meshlets -- this is what makes an old .ocmesh's bytes for its non-meshlet chunks unaffected by
    // this feature existing at all, and is the backward-compat contract §3.2's "unknown chunks
    // skipped" is built to support (never marked kAvrChunkRequired, so an old reader that doesn't
    // look for MLET is unaffected by its presence in a NEW file either).
    if (anyMeshlets) f.add(kChunkMLET, std::move(mlet), kAvrChunkGpuUploadable, kMlChunkVersionTopology);
    return writeAvr1(f, out, why);
}

// Decodes an .ocmesh container into `out`. Returns false with `why` set on a malformed file.
bool parseOcMesh(const u8* bytes, usize size, OcMeshData& out, std::string* why) {
    Avr1File f;
    if (!parseAvr1(bytes, size, f, why)) return false;
    if (f.subtype != kAvrSubtypeMesh) return fail(why, ".ocmesh: container subtype is not MESH");

    const AvrChunk* mhdr = f.find(kChunkMHDR);
    const AvrChunk* vtxs = f.find(kChunkVTXS);
    const AvrChunk* idxs = f.find(kChunkIDXS);
    if (!mhdr) return fail(why, ".ocmesh: no MHDR chunk");
    if (!vtxs) return fail(why, ".ocmesh: no VTXS chunk");
    if (!idxs) return fail(why, ".ocmesh: no IDXS chunk");

    AvrStringTable strt;
    if (const AvrChunk* s = f.find(kChunkSTRT)) strt.setBytes(s->data);

    R r{mhdr->data.data(), mhdr->data.data() + mhdr->data.size()};
    out.flags            = r.u32v();
    const u8 lodCount    = r.u8v();
    const u8 submeshCount= r.u8v();
    const u8 streamCount = r.u8v();
    r.u8v();                                  // UVChannelCount
    out.boundsMin = Vec3{r.f32v(), r.f32v(), r.f32v()};
    out.boundsMax = Vec3{r.f32v(), r.f32v(), r.f32v()};
    r.f32v(); r.f32v(); r.f32v(); r.f32v();   // bounds sphere
    const u32 slotCount  = r.u32v();
    out.builderVersion   = r.u32v();          // Reserved / BuilderVersion (0 on any pre-existing file)
    if (!r.ok) return fail(why, ".ocmesh: truncated MHDR");
    if (lodCount < 1) return fail(why, ".ocmesh: LODCount is 0");
    if (submeshCount < 1) return fail(why, ".ocmesh: SubmeshCount is 0");

    // Stream table. Only position and the attribute stream are understood; anything else is skipped.
    u16 posStride = 12, attrStride = 12, uvOffset = 8;
    u16 skinStride = kSkinStride, jointOffset = 0, weightOffset = kSkinWeightOffset;
    bool sawPosition = false, sawAttrs = false, sawJoints = false, sawWeights = false;
    for (u8 i = 0; i < streamCount; ++i) {
        const u8 sem = r.u8v(); const u8 fmtv = r.u8v(); r.u8v(); r.u8v();
        const u16 stride = r.u16v(); const u16 offInStride = r.u16v();
        if (sem == kSemPosition) {
            if (fmtv != kFmtR32G32B32F)
                return fail(why, ".ocmesh: quantized positions are not supported by this reader");
            posStride = stride ? stride : 12; sawPosition = true;
        } else if (sem == kSemTangentFrame) {
            if (fmtv != kFmtR16G16B16A16S)
                return fail(why, ".ocmesh: tangent frame is not a QTangent");
            attrStride = stride ? stride : 12; sawAttrs = true; (void)offInStride;
        } else if (sem == kSemUV0) {
            if (fmtv != kFmtR16G16F)
                return fail(why, ".ocmesh: UV0 is not R16G16_FLOAT");
            uvOffset = offInStride;
        } else if (sem == kSemJoints) {
            if (fmtv != kFmtR16G16B16A16Uint)
                return fail(why, ".ocmesh: JOINTS is not R16G16B16A16_UINT");
            skinStride = stride ? stride : kSkinStride; jointOffset = offInStride; sawJoints = true;
        } else if (sem == kSemWeights) {
            if (fmtv != kFmtR8G8B8A8Unorm)
                return fail(why, ".ocmesh: WEIGHTS is not R8G8B8A8_UNORM");
            skinStride = stride ? stride : kSkinStride; weightOffset = offInStride; sawWeights = true;
        }
    }
    // One without the other would size the skin block wrong and read past it.
    if (sawJoints != sawWeights)
        return fail(why, ".ocmesh: JOINTS and WEIGHTS must both be present or both absent");
    if (!r.ok) return fail(why, ".ocmesh: truncated stream table");
    if (!sawPosition || !sawAttrs) return fail(why, ".ocmesh: missing the position or tangent-frame stream");

    // LodDesc[LODCount] (§5.5). Every level is parsed now, not skipped past: LOD 0 populates
    // out.indices/out.meshlets exactly as it always has, LOD 1+ populate out.coarserLods (below,
    // after the vertex/skin streams that only need LOD 0's VertexCount). VtxOffset/VtxSize are read
    // but not kept per level -- every LOD shares LOD 0's vertex buffer (see
    // OcMeshData::coarserLods), so `vcount` from LOD 0 alone sizes the position/attribute/skin
    // decode below, exactly as before this feature existed.
    struct LodDescIn {
        u32 vcount = 0, icount = 0;
        u64 idxOffset = 0, idxSize = 0;
        u64 meshletOffset = 0;
        u32 meshletCount = 0;
        f32 screenError = 0.0f;
    };
    std::vector<LodDescIn> lodDescs(lodCount);
    for (u8 i = 0; i < lodCount; ++i) {
        LodDescIn& d = lodDescs[i];
        d.vcount = r.u32v();
        d.icount = r.u32v();
        r.u64v(); r.u64v();          // VtxOffset, VtxSize (shared across every LOD; not kept)
        d.idxOffset = r.u64v();
        d.idxSize   = r.u64v();
        d.meshletOffset = r.u64v();
        d.meshletCount  = r.u32v();
        d.screenError   = r.f32v();
    }
    if (!r.ok) return fail(why, ".ocmesh: truncated LOD table");
    const u32 vcount = lodDescs[0].vcount;
    const u32 icount = lodDescs[0].icount;
    if (vcount == 0 || icount == 0) return fail(why, ".ocmesh: LOD 0 is empty");

    out.submeshes.clear();
    out.submeshes.resize(submeshCount);
    for (u8 i = 0; i < submeshCount; ++i) {
        out.submeshes[i].materialSlot = r.u32v();
        out.submeshes[i].name = std::string(strt.get(r.u32v()));
        r.u32v();
    }
    for (u8 i = 0; i < submeshCount; ++i) {
        out.submeshes[i].indexStart  = r.u32v();
        out.submeshes[i].indexCount  = r.u32v();
        out.submeshes[i].baseVertex  = r.u32v();
        out.submeshes[i].vertexCount = r.u32v();
        r.f32v(); r.f32v(); r.f32v(); r.f32v();
        r.u32v(); r.u32v();
    }
    if (!r.ok) return fail(why, ".ocmesh: truncated submesh tables");

    // ---- vertices ----
    const u64 posBytes = u64(vcount) * posStride;
    const u64 posPadded = (posBytes + 15) & ~u64(15);
    const u64 vtxsSize  = u64(vtxs->data.size());
    if (posPadded > vtxsSize || u64(vcount) * attrStride > vtxsSize - posPadded)
        return fail(why, ".ocmesh: VTXS is smaller than the LOD's vertex count requires");

    // THE OFFSETS HAVE TO BE CHECKED, NOT JUST THE STRIDES. uvOffset, jointOffset and weightOffset
    // come verbatim from the file's stream table, and the per-vertex reads below add them to the
    // stride-sized cursor -- so a file declaring a stride that fits and an offset that does not
    // pushed every read up to 64 KB past the end of the chunk while passing the check above.
    //
    // Each attribute is bounded against ITS OWN stride, because that is what the last vertex's read
    // actually runs off the end of: the last element sits at (vcount-1)*stride + offset + width.
    if (u64(uvOffset) + 4 > u64(attrStride))
        return fail(why, ".ocmesh: the UV stream's offset falls outside the attribute stride");

    // AND THE STRIDES THEMSELVES, which checking the offsets does not cover. Position and the
    // tangent frame are read at FIXED WIDTHS (12 bytes and 8) from offset 0, so no offset check
    // constrains them -- only the stride does, and posStride/attrStride come verbatim from the
    // file's stream table with nothing but a zero-means-default fallback.
    //
    // The size check above does not save it either, because it bounds vcount*stride while the LAST
    // vertex reads (vcount-1)*stride + width. Concretely, with posStride 2, attrStride 1 and
    // vcount 8: posPadded is 16, the smallest VTXS that passes is 24 bytes, and vertex 7's position
    // read runs [14, 26) -- two bytes off the end of the chunk. The tangent frame is worse because
    // it needs no arithmetic at all: ANY attrStride below 8 overruns the last vertex by 8-attrStride.
    //
    // The skin block below needs no equivalent because its reads are offset-based, and bounding
    // jointOffset/weightOffset against skinStride already keeps them inside one stride.
    if (u64(posStride) < 12)
        return fail(why, ".ocmesh: the position stream's stride is smaller than the position it must hold");
    if (u64(attrStride) < 8)
        return fail(why, ".ocmesh: the attribute stream's stride is smaller than the tangent frame it must hold");

    out.positions.resize(usize(vcount) * 3);
    out.normals.resize(usize(vcount) * 3);
    out.uvs.resize(usize(vcount) * 2);
    for (u32 i = 0; i < vcount; ++i) {
        const u8* p = vtxs->data.data() + usize(i) * posStride;
        std::memcpy(&out.positions[usize(i) * 3], p, 12);

        const u8* a = vtxs->data.data() + posPadded + usize(i) * attrStride;
        i16 q[4];
        for (int k = 0; k < 4; ++k) { u16 h; std::memcpy(&h, a + k * 2, 2); q[k] = static_cast<i16>(h); }
        f32 n[3]; qTangentToNormal(q, n);
        out.normals[usize(i) * 3 + 0] = n[0];
        out.normals[usize(i) * 3 + 1] = n[1];
        out.normals[usize(i) * 3 + 2] = n[2];

        u16 hu, hv; std::memcpy(&hu, a + uvOffset, 2); std::memcpy(&hv, a + uvOffset + 2, 2);
        out.uvs[usize(i) * 2 + 0] = halfToFloat(hu);
        out.uvs[usize(i) * 2 + 1] = halfToFloat(hv);
    }

    // ---- skin, when the file carries it ----
    if (sawJoints) {
        const u64 attrPadded = (posPadded + u64(vcount) * attrStride + 15) & ~u64(15);
        if (attrPadded > vtxsSize || u64(vcount) * skinStride > vtxsSize - attrPadded)
            return fail(why, ".ocmesh: VTXS is smaller than the skin streams require");
        // As above: the joint and weight offsets are the file's, and the reads below add them.
        if (u64(jointOffset) + u64(kOcMeshInfluences) * 2 > u64(skinStride) ||
            u64(weightOffset) + u64(kOcMeshInfluences) > u64(skinStride))
            return fail(why, ".ocmesh: a skin stream's offset falls outside the skin stride");

        out.joints.resize(usize(vcount) * kOcMeshInfluences);
        out.weights.resize(usize(vcount) * kOcMeshInfluences);
        for (u32 i = 0; i < vcount; ++i) {
            const u8* s = vtxs->data.data() + attrPadded + usize(i) * skinStride;
            for (u32 k = 0; k < kOcMeshInfluences; ++k) {
                u16 j; std::memcpy(&j, s + jointOffset + k * 2, 2);
                out.joints[usize(i) * kOcMeshInfluences + k] = j;
                out.weights[usize(i) * kOcMeshInfluences + k] =
                    static_cast<f32>(s[weightOffset + k]) / 255.0f;
            }
        }
    }

    // ---- indices, per LOD. `decodeIndices` is shared by LOD 0 (into out.indices, exactly as
    // before) and every coarser level (into coarserLods[i-1].indices, below) ----
    const bool index32 = (out.flags & kOcMeshIndex32) != 0;
    const usize istride = index32 ? 4u : 2u;
    auto decodeIndices = [&](const LodDescIn& d, std::vector<u32>& dst, u32 level) -> bool {
        if (d.idxOffset > idxs->data.size() || u64(d.icount) * istride > idxs->data.size() - d.idxOffset)
            return fail(why, ".ocmesh: IDXS is smaller than LOD " + std::to_string(level) + "'s index count requires");
        dst.resize(d.icount);
        const u8* base = idxs->data.data() + d.idxOffset;
        for (u32 i = 0; i < d.icount; ++i) {
            if (index32) { u32 v; std::memcpy(&v, base + usize(i) * 4, 4); dst[i] = v; }
            else         { u16 v; std::memcpy(&v, base + usize(i) * 2, 2); dst[i] = v; }
            if (dst[i] >= vcount)
                return fail(why, ".ocmesh: LOD " + std::to_string(level) + " index " + std::to_string(dst[i]) +
                                      " is past the vertex count");
        }
        return true;
    };
    if (!decodeIndices(lodDescs[0], out.indices, 0)) return false;

    // ---- MLET chunk-version 3's GroupTable, parsed ONCE for every LOD before decodeMeshlets or
    // decodeGroupNodes runs for any of them -- decodeMeshlets below needs `groupTable[level]`'s
    // groupNodeCount for ownerGroupId's own-level bounds check, and needs it available regardless of
    // when (or whether) decodeGroupNodes for that same level has run yet, so this cannot simply live
    // inside decodeGroupNodes the way the rest of the group data is read. Left EMPTY (size 0) for a
    // chunk-version 1/2 file, or a file with no MLET chunk at all -- decodeMeshlets and
    // decodeGroupNodes both treat that as "no group data for any level" exactly like a v1/v2 file
    // that never had a GroupTable to begin with.
    struct GroupTableEntry { u64 groupNodeOffset = 0; u32 groupNodeCount = 0; u64 groupChildOffset = 0; u32 groupChildCount = 0; };
    std::vector<GroupTableEntry> groupTable;
    if ((out.flags & kOcMeshMeshlets) != 0) {
        if (const AvrChunk* mletForTable = f.find(kChunkMLET); mletForTable && mletForTable->version >= kMlChunkVersionTopology) {
            const u64 tableBytes = u64(kGroupTableEntryBytes) * lodCount;
            if (tableBytes > mletForTable->data.size())
                return fail(why, ".ocmesh: MLET is smaller than its own GroupTable requires");
            groupTable.resize(lodCount);
            R gr{mletForTable->data.data(), mletForTable->data.data() + tableBytes};
            for (u8 i = 0; i < lodCount; ++i) {
                groupTable[i].groupNodeOffset  = gr.u64v();
                groupTable[i].groupNodeCount   = gr.u32v();
                groupTable[i].groupChildOffset = gr.u64v();
                groupTable[i].groupChildCount  = gr.u32v();
            }
            if (!gr.ok) return fail(why, ".ocmesh: truncated MLET GroupTable");
        }
    }

    // ---- meshlets, per LOD, when the file carries them (§5.7). `decodeMeshlets` is shared by LOD 0
    // (into out.meshlets, exactly as before) and every coarser level (below).
    //
    // Gated on kOcMeshMeshlets AND MeshletCount > 0, not on the chunk's mere presence: a file with
    // the flag clear but a stray MLET chunk (hand-edited, or written by a future tool this reader
    // does not know about) should NOT populate any level's meshlets, exactly as an old file with no
    // MLET chunk at all does not. This is also what keeps a plain flags-only round trip
    // byte-identical -- see writeOcMesh, which never emits MLET unless some level's meshlets are
    // non-empty. ----
    auto decodeMeshlets = [&](const LodDescIn& d, std::vector<OcMeshMeshlet>& dst, u32 level) -> bool {
        dst.clear();
        if ((out.flags & kOcMeshMeshlets) == 0 || d.meshletCount == 0) return true;

        const AvrChunk* mlet = f.find(kChunkMLET);
        if (!mlet) return fail(why, ".ocmesh: MeshletCount is nonzero but there is no MLET chunk");
        const u8* base = mlet->data.data();
        const u64 chunkSize = mlet->data.size();
        const u64 meshletOffset = d.meshletOffset;
        const u32 meshletCount = d.meshletCount;
        if (meshletOffset > chunkSize)
            return fail(why, ".ocmesh: LOD " + std::to_string(level) + "'s MeshletOffset is past the end of MLET");

        // The MeshletBounds stride depends on the CHUNK's version, not this LOD's: v1 files (every
        // MLET written before per-cluster LOD existed) are 32 B/meshlet with no OwnError/ParentError,
        // v2 files are 40 B/meshlet, v3 files are 48 B/meshlet with FallbackAncestorId/OwnerGroupId
        // appended (see kMlChunkVersionTopology above). Gating on the version rather than assuming
        // the newer stride is what lets an old .ocmesh keep loading unchanged.
        const bool hasErrorFields    = mlet->version >= kMlChunkVersionErrors;
        const bool hasTopologyFields = mlet->version >= kMlChunkVersionTopology;
        const u32 boundsStride = hasTopologyFields ? kMeshletBoundsBytesV3
                                : hasErrorFields    ? kMeshletBoundsBytesV2
                                                     : kMeshletBoundsBytesV1;

        const u64 descBytes = u64(meshletCount) * 12;
        const u64 boundsBytes = u64(meshletCount) * boundsStride;
        if (descBytes > chunkSize - meshletOffset || boundsBytes > chunkSize - meshletOffset - descBytes)
            return fail(why, ".ocmesh: MLET is smaller than LOD " + std::to_string(level) +
                                  "'s MeshletDesc[]/MeshletBounds[] require");

        // ---- sub-array 1: MeshletDesc[] ----
        struct DescIn { u32 vtxOff = 0, triOff = 0; u8 vtxCount = 0, triCount = 0; };
        std::vector<DescIn> descs(meshletCount);
        {
            R dr{base + meshletOffset, base + meshletOffset + descBytes};
            for (u32 i = 0; i < meshletCount; ++i) {
                descs[i].vtxOff   = dr.u32v();
                descs[i].triOff   = dr.u32v();
                descs[i].vtxCount = dr.u8v();
                descs[i].triCount = dr.u8v();
                dr.u16v();   // Pad
            }
            if (!dr.ok) return fail(why, ".ocmesh: truncated MeshletDesc[] for LOD " + std::to_string(level));
        }
        for (u32 i = 0; i < meshletCount; ++i) {
            if (descs[i].vtxCount > kMaxMeshletVertices)
                return fail(why, ".ocmesh: LOD " + std::to_string(level) + " meshlet " + std::to_string(i) +
                                      " declares more than " + std::to_string(kMaxMeshletVertices) + " vertices");
            if (descs[i].triCount > kMaxMeshletTriangles)
                return fail(why, ".ocmesh: LOD " + std::to_string(level) + " meshlet " + std::to_string(i) +
                                      " declares more than " + std::to_string(kMaxMeshletTriangles) + " triangles");
        }

        // ---- sub-array 2: MeshletBounds[] ----
        const u64 boundsStart = meshletOffset + descBytes;
        dst.resize(meshletCount);
        {
            R br{base + boundsStart, base + boundsStart + boundsBytes};
            for (u32 i = 0; i < meshletCount; ++i) {
                OcMeshMeshlet& ml = dst[i];
                ml.sphereCenter = Vec3{br.f32v(), br.f32v(), br.f32v()};
                ml.sphereRadius = br.f32v();
                ml.coneApex     = Vec3{br.f32v(), br.f32v(), br.f32v()};
                ml.coneAxis[0]  = static_cast<i8>(br.u8v());
                ml.coneAxis[1]  = static_cast<i8>(br.u8v());
                ml.coneAxis[2]  = static_cast<i8>(br.u8v());
                ml.coneCutoff   = static_cast<i8>(br.u8v());
                // v1 chunks stop here (32 B/meshlet): ml.ownError/parentError keep the defaults
                // OcMeshMeshlet's default constructor already gave them (0.0f, +FLT_MAX) -- the same
                // "always drawable, and nothing finer needs to exist" pair a root cluster gets, which
                // is the harmless choice for data written before this field existed.
                if (hasErrorFields) {
                    ml.ownError    = br.f32v();
                    ml.parentError = br.f32v();
                }
                // v1/v2 chunks stop here: ml.fallbackAncestorId/ownerGroupId keep OcMeshMeshlet's own
                // defaults (both kInvalidClusterId) -- see that field's own comment for why "no
                // fallback / no owner" is the correct, harmless reading of data written before this
                // feature existed, not merely a convenient one.
                if (hasTopologyFields) {
                    ml.fallbackAncestorId = br.u32v();
                    ml.ownerGroupId       = br.u32v();
                }
            }
            if (!br.ok) return fail(why, ".ocmesh: truncated MeshletBounds[] for LOD " + std::to_string(level));
        }

        // ---- chunk-version 3: fallbackAncestorId/ownerGroupId range checks, against the SAME two
        // per-level counts the writer validated them against (see writeOcMesh's own comment) -- a
        // reader has no reason to trust a stored value is in range just because the writer once
        // checked it; the bytes it is reading now could be hand-edited or corrupt. lodDescs and
        // groupTable are both already fully parsed at this point (lodDescs before this lambda is
        // even defined; groupTable just above it), so both counts are available regardless of
        // whether decodeGroupNodes for this or the next level has run yet. ----
        if (hasTopologyFields) {
            const bool isRootLevel = (usize(level) + 1 >= lodDescs.size());
            const u32 nextLevelCount = isRootLevel ? 0 : lodDescs[usize(level) + 1].meshletCount;
            const u32 ownGroupCount = (usize(level) < groupTable.size()) ? groupTable[level].groupNodeCount : 0;
            for (u32 i = 0; i < meshletCount; ++i) {
                const OcMeshMeshlet& ml = dst[i];
                if (isRootLevel) {
                    if (ml.fallbackAncestorId != kInvalidClusterId)
                        return fail(why, ".ocmesh: LOD " + std::to_string(level) + " meshlet " + std::to_string(i) +
                                              " is at the root level but has a fallbackAncestorId set");
                } else if (ml.fallbackAncestorId != kInvalidClusterId && ml.fallbackAncestorId >= nextLevelCount) {
                    return fail(why, ".ocmesh: LOD " + std::to_string(level) + " meshlet " + std::to_string(i) +
                                          "'s fallbackAncestorId " + std::to_string(ml.fallbackAncestorId) +
                                          " is past the next level's " + std::to_string(nextLevelCount) + " meshlets");
                }
                if (level == 0) {
                    if (ml.ownerGroupId != kInvalidClusterId)
                        return fail(why, ".ocmesh: LOD 0 meshlet " + std::to_string(i) +
                                              " has an ownerGroupId set, but LOD 0 is never produced by a group");
                } else if (ml.ownerGroupId != kInvalidClusterId && ml.ownerGroupId >= ownGroupCount) {
                    return fail(why, ".ocmesh: LOD " + std::to_string(level) + " meshlet " + std::to_string(i) +
                                          "'s ownerGroupId " + std::to_string(ml.ownerGroupId) +
                                          " is past this level's " + std::to_string(ownGroupCount) + " group nodes");
                }
            }
        }

        // ---- sub-arrays 3/4 start where 1/2 end, WITHIN THIS LEVEL'S OWN BLOCK. Nothing in the
        // header names these two offsets directly (see FORMAT_SPECS.md 5.7: "four back-to-back
        // sub-arrays"), so sub-array 3's start is computed here, and sub-array 4's start/size from
        // the TOTAL vertex/triangle bytes just read out of every MeshletDesc -- both are then
        // re-validated against each meshlet's own VertexIndexOffset/TriangleOffset below, so a
        // corrupt Desc cannot walk this reader off the end of the chunk. Bounding TriangleOffset to
        // THIS LEVEL's own byte count (not whatever remains in the whole MLET chunk) matters once
        // more than one LOD's block shares one chunk: without it, a corrupt offset could silently
        // read a NEIGHBOURING level's bytes instead of failing. ----
        u64 totalVerts = 0;
        for (const DescIn& d2 : descs) totalVerts += d2.vtxCount;
        const u64 vertsStart = boundsStart + boundsBytes;
        const u64 vertsBytes = totalVerts * 4;
        if (vertsBytes > chunkSize - vertsStart)
            return fail(why, ".ocmesh: MLET is smaller than LOD " + std::to_string(level) + "'s MeshletVertices[] requires");
        const u64 trisStart = vertsStart + vertsBytes;

        u64 trisBytesForLevel = 0;
        for (const DescIn& d2 : descs) trisBytesForLevel += (u64(d2.triCount) * 3 + 3) & ~u64(3);
        if (trisBytesForLevel > chunkSize - trisStart)
            return fail(why, ".ocmesh: MLET is smaller than LOD " + std::to_string(level) + "'s MeshletTriangles[] requires");

        for (u32 i = 0; i < meshletCount; ++i) {
            OcMeshMeshlet& ml = dst[i];
            const DescIn& d2 = descs[i];

            const u64 vOff = u64(d2.vtxOff) * 4;
            if (vOff + u64(d2.vtxCount) * 4 > vertsBytes)
                return fail(why, ".ocmesh: LOD " + std::to_string(level) + " meshlet " + std::to_string(i) +
                                      "'s VertexIndexOffset is out of range");
            ml.vertices.resize(d2.vtxCount);
            for (u32 k = 0; k < d2.vtxCount; ++k) {
                u32 v; std::memcpy(&v, base + vertsStart + vOff + u64(k) * 4, 4);
                if (v >= vcount)
                    return fail(why, ".ocmesh: LOD " + std::to_string(level) + " meshlet " + std::to_string(i) +
                                          " references vertex " + std::to_string(v) + ", past the mesh's " +
                                          std::to_string(vcount) + " vertices");
                ml.vertices[k] = v;
            }

            const u64 triByteCount = u64(d2.triCount) * 3;
            const u64 tOff = u64(d2.triOff);
            if (tOff + triByteCount > trisBytesForLevel)
                return fail(why, ".ocmesh: LOD " + std::to_string(level) + " meshlet " + std::to_string(i) +
                                      "'s TriangleOffset is out of range");
            ml.triangles.resize(triByteCount);
            for (u64 k = 0; k < triByteCount; ++k) {
                const u8 local = base[trisStart + tOff + k];
                if (local >= d2.vtxCount)
                    return fail(why, ".ocmesh: LOD " + std::to_string(level) + " meshlet " + std::to_string(i) +
                                          " has a local triangle index past its own vertex list");
                ml.triangles[k] = local;
            }
        }
        return true;
    };
    if (!decodeMeshlets(lodDescs[0], out.meshlets, 0)) return false;

    // ---- chunk-version 3: the group hierarchy for one LOD level, from `groupTable` (parsed once,
    // above). LOD 0 never calls this (see the coarser-LOD loop below) -- its own groupTable entry is
    // always zero-count on a file this writer produced, but this function does not special-case
    // level 0 itself; it is simply never asked to run there. ----
    auto decodeGroupNodes = [&](u32 level, std::vector<OcMeshClusterGroup>& dstNodes,
                                 std::vector<u32>& dstChildren) -> bool {
        dstNodes.clear();
        dstChildren.clear();
        if (usize(level) >= groupTable.size()) return true;   // v1/v2 chunk, or no MLET at all
        const GroupTableEntry& e = groupTable[level];
        if (e.groupNodeCount == 0) return true;

        const AvrChunk* mlet = f.find(kChunkMLET);
        if (!mlet) return fail(why, ".ocmesh: LOD " + std::to_string(level) + " has group nodes but there is no MLET chunk");
        const u8* base = mlet->data.data();
        const u64 chunkSize = mlet->data.size();

        const u64 nodeBytes = u64(e.groupNodeCount) * kGroupNodeBytes;
        if (e.groupNodeOffset > chunkSize || nodeBytes > chunkSize - e.groupNodeOffset)
            return fail(why, ".ocmesh: MLET is smaller than LOD " + std::to_string(level) + "'s ClusterGroupNode[] requires");
        dstNodes.resize(e.groupNodeCount);
        {
            R nr{base + e.groupNodeOffset, base + e.groupNodeOffset + nodeBytes};
            for (u32 i = 0; i < e.groupNodeCount; ++i) {
                OcMeshClusterGroup& n = dstNodes[i];
                n.sphereCenter = Vec3{nr.f32v(), nr.f32v(), nr.f32v()};
                n.sphereRadius = nr.f32v();
                n.ownClusterStart = nr.u32v();
                n.ownClusterCount = nr.u32v();
                n.childClusterStart = nr.u32v();
                n.childClusterCount = nr.u32v();
            }
            if (!nr.ok) return fail(why, ".ocmesh: truncated ClusterGroupNode[] for LOD " + std::to_string(level));
        }

        if (e.groupChildCount > 0) {
            const u64 childBytes = u64(e.groupChildCount) * 4;
            if (e.groupChildOffset > chunkSize || childBytes > chunkSize - e.groupChildOffset)
                return fail(why, ".ocmesh: MLET is smaller than LOD " + std::to_string(level) + "'s ClusterGroupChildren[] requires");
            dstChildren.resize(e.groupChildCount);
            R cr{base + e.groupChildOffset, base + e.groupChildOffset + childBytes};
            for (u32 i = 0; i < e.groupChildCount; ++i) dstChildren[i] = cr.u32v();
            if (!cr.ok) return fail(why, ".ocmesh: truncated ClusterGroupChildren[] for LOD " + std::to_string(level));
        }

        // Range-check every node's own two ranges NOW, against the two counts this reader already
        // has to hand: this level's own meshletCount (lodDescs[level]) for ownClusterRange, and the
        // FINER level's meshletCount (lodDescs[level-1] -- a group's children are always exactly one
        // level finer than its own, see aver::trifactor::ClusterGroupNode's own comment) for every
        // value childClusterRange's span pulls out of dstChildren.
        const u32 ownLevelCount = lodDescs[level].meshletCount;
        const u32 finerLevelCount = (level > 0) ? lodDescs[usize(level) - 1].meshletCount : 0;
        for (usize gi = 0; gi < dstNodes.size(); ++gi) {
            const OcMeshClusterGroup& n = dstNodes[gi];
            if (u64(n.ownClusterStart) + n.ownClusterCount > ownLevelCount)
                return fail(why, ".ocmesh: LOD " + std::to_string(level) + " group " + std::to_string(gi) +
                                      "'s ownClusterRange runs past this level's " + std::to_string(ownLevelCount) +
                                      " meshlets");
            if (u64(n.childClusterStart) + n.childClusterCount > dstChildren.size())
                return fail(why, ".ocmesh: LOD " + std::to_string(level) + " group " + std::to_string(gi) +
                                      "'s childClusterRange runs past its own " + std::to_string(dstChildren.size()) +
                                      "-entry ClusterGroupChildren[]");
            for (u32 k = 0; k < n.childClusterCount; ++k) {
                const u32 childId = dstChildren[usize(n.childClusterStart) + k];
                if (childId >= finerLevelCount)
                    return fail(why, ".ocmesh: LOD " + std::to_string(level) + " group " + std::to_string(gi) +
                                          " references child cluster " + std::to_string(childId) + ", past LOD " +
                                          std::to_string(level > 0 ? level - 1 : 0) + "'s " +
                                          std::to_string(finerLevelCount) + " meshlets");
            }
        }
        return true;
    };

    // ---- coarser LODs (level >= 1): each gets its own index buffer (decodeIndices), its own meshlet
    // partition of it (decodeMeshlets), and (chunk-version 3 only) the group hierarchy that produced
    // it (decodeGroupNodes) -- sharing LOD 0's vertex buffer throughout. Empty for a single-LOD file
    // (lodCount == 1) -- the loop below then does not run at all, which combined with everything
    // above is what makes an old .ocmesh load completely unchanged. ----
    out.coarserLods.clear();
    out.coarserLods.resize(lodCount > 0 ? usize(lodCount) - 1 : 0);
    for (u8 i = 1; i < lodCount; ++i) {
        OcMeshLod& cl = out.coarserLods[usize(i) - 1];
        cl.screenErrorThreshold = lodDescs[i].screenError;
        if (lodDescs[i].icount == 0)
            return fail(why, ".ocmesh: LOD " + std::to_string(i) + " has no indices");
        if (!decodeIndices(lodDescs[i], cl.indices, i)) return false;
        if (!decodeMeshlets(lodDescs[i], cl.meshlets, i)) return false;
        if (!decodeGroupNodes(i, cl.groupNodes, cl.groupChildren)) return false;
    }

    // ---- material slots ----
    out.materialSlots.clear();
    if (const AvrChunk* madr = f.find(kChunkMADR)) {
        R mr{madr->data.data(), madr->data.data() + madr->data.size()};
        const u32 n = mr.u32v();
        for (u32 i = 0; i < n && mr.ok; ++i) out.materialSlots.push_back(std::string(strt.get(mr.u32v())));
    }
    if (out.materialSlots.empty() && slotCount) out.materialSlots.resize(slotCount);
    return true;
}

// Reads an .ocmesh file from disk. Returns false with `why` set.
bool loadOcMesh(const std::string& path, OcMeshData& out, std::string* why) {
    // Traced like the text loaders are. These open their own ifstream rather than going
    // through platform::readFileBytes, so without this line a .ocmesh/.ocanim/AVR1 read from
    // OUTSIDE a package would not appear in --trace-opens and verify-game.ps1 would pass a
    // package that reaches into the dev tree for its geometry.
    aver::traceFileOpen(path);
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return fail(why, ".ocmesh: cannot open " + path);
    const std::streamoff n = f.tellg();
    if (n <= 0) return fail(why, ".ocmesh: empty file " + path);
    std::vector<u8> bytes(static_cast<usize>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()), n);
    if (!f) return fail(why, ".ocmesh: short read on " + path);
    return parseOcMesh(bytes.data(), bytes.size(), out, why);
}

// Writes a mesh to an .ocmesh file. Returns false with `why` set.
bool saveOcMesh(const std::string& path, const OcMeshData& in, std::string* why) {
    std::vector<u8> bytes;
    if (!writeOcMesh(in, bytes, why)) return false;
    // ATOMIC, NOT TRUNCATE-THEN-WRITE. An ofstream opened with ios::trunc zeroes the file when the
    // STREAM IS CONSTRUCTED, before a byte of `bytes` is written, and this overwrites a real asset in
    // place -- so a crash, a kill or a full disk in that window destroyed the previously-good file and
    // not merely the unsaved edit. writeFileBytesAtomic writes a temporary beside the target and swaps
    // only a complete one into place; see its comment in FileSystem.hpp, which already names this
    // class of caller.
    if (!writeFileBytesAtomic(path, bytes.data(), bytes.size()))
        return fail(why, ".ocmesh: write failed on " + path);
    return true;
}

} // namespace aver::fmt
