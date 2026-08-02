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

    OcMeshData m = in;
    m.computeBounds();

    const u32 vcount = m.vertexCount();
    const bool index32 = vcount > 0xFFFF;
    const bool skinned = m.hasSkin();

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

    // ---- IDXS ----
    std::vector<u8> idxs;
    {
        W w{idxs};
        for (const u32 idx : m.indices) { if (index32) w.u32v(idx); else w.u16v(static_cast<u16>(idx)); }
        w.pad16();
    }

    // ---- MADR: slot index -> surface name ----
    std::vector<u8> madr;
    {
        W w{madr};
        w.u32v(static_cast<u32>(m.materialSlots.size()));
        for (const std::string& s : m.materialSlots) w.u32v(strt.add(s));
    }

    // ---- MHDR ----
    std::vector<u8> mhdr;
    {
        W w{mhdr};
        u32 flags = m.flags & ~(kOcMeshIndex32 | kOcMeshMeshlets | kOcMeshHasColor | kOcMeshHasUV1 | kOcMeshHasSkin);
        if (index32) flags |= kOcMeshIndex32;
        if (skinned) flags |= kOcMeshHasSkin;
        w.u32v(flags);
        w.u8v(1);                                             // LODCount
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
        w.u32v(0);                                             // Reserved

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

        // LodDesc[1] (§5.5)
        const u64 posBytes  = u64(vcount) * 12;
        const u64 posPadded = (posBytes + 15) & ~u64(15);
        w.u32v(vcount);
        w.u32v(static_cast<u32>(m.indices.size()));
        w.u64v(0);                                   // VtxOffset
        w.u64v(static_cast<u64>(vtxs.size()));       // VtxSize
        w.u64v(0);                                   // IdxOffset
        w.u64v(static_cast<u64>(idxs.size()));       // IdxSize
        w.u64v(0);                                   // MeshletOffset (none)
        w.u32v(0);                                   // MeshletCount
        w.f32v(0.0f);                                // ScreenErrorThreshold (single LOD)
        (void)posPadded;

        // SubmeshDesc[] (§5.6)
        for (usize i = 0; i < m.submeshes.size(); ++i) {
            w.u32v(m.submeshes[i].materialSlot);
            w.u32v(nameRefs[i]);
            w.u32v(0);                               // Flags
        }
        // SubmeshRange[LOD*Submesh] (§5.6)
        for (const OcMeshSubmesh& s : m.submeshes) {
            w.u32v(s.indexStart); w.u32v(s.indexCount);
            w.u32v(s.baseVertex); w.u32v(s.vertexCount);
            w.f32v(0); w.f32v(0); w.f32v(0); w.f32v(0);   // per-submesh bounds sphere: not computed
            w.u32v(0); w.u32v(0);                          // meshlet start/count
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
    r.u32v();                                 // Reserved
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

    // LodDesc[0] only; later LODs are read past rather than parsed.
    const u32 vcount = r.u32v();
    const u32 icount = r.u32v();
    r.u64v(); r.u64v(); r.u64v(); r.u64v(); r.u64v(); r.u32v(); r.f32v();
    if (!r.ok) return fail(why, ".ocmesh: truncated LOD table");
    if (vcount == 0 || icount == 0) return fail(why, ".ocmesh: LOD 0 is empty");

    // Skip any further LodDescs so the submesh tables land at the right place.
    for (u8 i = 1; i < lodCount; ++i) { for (int k = 0; k < 14; ++k) r.u32v(); }

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
    if (posPadded + u64(vcount) * attrStride > vtxs->data.size())
        return fail(why, ".ocmesh: VTXS is smaller than the LOD's vertex count requires");

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
        if (attrPadded + u64(vcount) * skinStride > vtxs->data.size())
            return fail(why, ".ocmesh: VTXS is smaller than the skin streams require");

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

    // ---- indices ----
    const bool index32 = (out.flags & kOcMeshIndex32) != 0;
    const usize istride = index32 ? 4u : 2u;
    if (u64(icount) * istride > idxs->data.size())
        return fail(why, ".ocmesh: IDXS is smaller than the LOD's index count requires");
    out.indices.resize(icount);
    for (u32 i = 0; i < icount; ++i) {
        if (index32) { u32 v; std::memcpy(&v, idxs->data.data() + usize(i) * 4, 4); out.indices[i] = v; }
        else         { u16 v; std::memcpy(&v, idxs->data.data() + usize(i) * 2, 2); out.indices[i] = v; }
        if (out.indices[i] >= vcount)
            return fail(why, ".ocmesh: index " + std::to_string(out.indices[i]) + " is past the vertex count");
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
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return fail(why, ".ocmesh: cannot write " + path);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f) return fail(why, ".ocmesh: write failed on " + path);
    return true;
}

} // namespace aver::fmt
