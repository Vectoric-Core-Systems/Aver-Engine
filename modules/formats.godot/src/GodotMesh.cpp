#include "aver/formats/godot/GodotMesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aver::fmt::godot {

namespace {

constexpr double kPi = 3.14159265358979323846;

u16 rd16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
u32 rd32(const u8* p) { return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24); }
f32 rdF(const u8* p) { const u32 u = rd32(p); f32 f; std::memcpy(&f, &u, 4); return f; }

void normalize3(f32 v[3]) {
    const double l = std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]);
    if (l > 1e-20) { v[0] = f32(v[0] / l); v[1] = f32(v[1] / l); v[2] = f32(v[2] / l); }
}

// Bytes one custom array element takes (ArrayCustomFormat).
int customSize(int fmtId) {
    switch (fmtId) {
        case 0: case 1: case 2: case 4: return 4;
        case 3: case 5: return 8;
        case 6: return 12;
        case 7: return 16;
        default: return 0;
    }
}

// Godot's Basis(axis, angle) rows 0 and 2: the tangent and the normal of a compressed vertex.
void tbnFromAxisAngle(const f32 axis[3], double angle01, f32 outNormal[3], f32 outTangent[4]) {
    const double sign = angle01 > 0.5 ? 1.0 : -1.0;
    const double a = std::fabs(angle01 * 2.0 - 1.0) * kPi;
    const double c = std::cos(a), s = std::sin(a), t = 1.0 - c;
    const double x = axis[0], y = axis[1], z = axis[2];
    outTangent[0] = f32(c + x * x * t);
    outTangent[1] = f32(x * y * t - z * s);
    outTangent[2] = f32(z * x * t + y * s);
    outTangent[3] = f32(sign);
    outNormal[0] = f32(z * x * t - y * s);
    outNormal[1] = f32(y * z * t + x * s);
    outNormal[2] = f32(c + z * z * t);
}

const GodotVariant* arr(const GodotVariant& d, const char* key) { return d.find(key); }

}  // namespace

void gdOctEncode(const f32 nIn[3], f32 out[2]) {
    double n[3] = {nIn[0], nIn[1], nIn[2]};
    const double l1 = std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]);
    if (l1 > 1e-20) { n[0] /= l1; n[1] /= l1; n[2] /= l1; }
    double ox, oy;
    if (n[2] >= 0.0) { ox = n[0]; oy = n[1]; }
    else {
        ox = (1.0 - std::fabs(n[1])) * (n[0] >= 0.0 ? 1.0 : -1.0);
        oy = (1.0 - std::fabs(n[0])) * (n[1] >= 0.0 ? 1.0 : -1.0);
    }
    out[0] = f32(ox * 0.5 + 0.5);
    out[1] = f32(oy * 0.5 + 0.5);
}

void gdOctDecode(const f32 in[2], f32 out[3]) {
    const double fx = in[0] * 2.0 - 1.0, fy = in[1] * 2.0 - 1.0;
    double x = fx, y = fy, z = 1.0 - std::fabs(fx) - std::fabs(fy);
    const double t = std::min(1.0, std::max(0.0, -z));
    x += x >= 0 ? -t : t;
    y += y >= 0 ? -t : t;
    out[0] = f32(x); out[1] = f32(y); out[2] = f32(z);
    normalize3(out);
}

bool decodeArrayMeshSurface(const GodotVariant& d, GodotSurface& s, std::string* why, std::vector<std::string>* notes) {
    const auto fail = [&](const std::string& m) { if (why) *why = m; return false; };
    const auto note = [&](const std::string& m) { if (notes) notes->push_back(m); };
    if (d.type != GodotVType::Dictionary) return fail("a surface is not a dictionary");
    s = GodotSurface{};
    const GodotVariant* fv = arr(d, "format");
    const GodotVariant* vc = arr(d, "vertex_count");
    const GodotVariant* vd = arr(d, "vertex_data");
    if (!fv || !vc || !vd || vd->type != GodotVType::PackedByteArray) return fail("the surface lacks format, vertex_count or vertex_data");
    s.format = static_cast<u64>(fv->asInt());
    s.formatVersion = static_cast<int>((s.format >> kGdFormatVersionShift) & 0xFF);
    s.vertexCount = static_cast<u32>(std::max<i64>(vc->asInt(), 0));
    if (const GodotVariant* p = arr(d, "primitive")) s.primitive = static_cast<int>(p->asInt(3));
    if (s.primitive != 3 && s.primitive != 4) return fail("only triangle surfaces are decoded (primitive " + std::to_string(s.primitive) + " is a point or line list)");
    if (const GodotVariant* b = arr(d, "aabb"); b && b->type == GodotVType::AABB)
        for (int i = 0; i < 6; ++i) s.aabb[i] = f32(b->num(static_cast<usize>(i)));
    f32 uvScale[4] = {0, 0, 0, 0};
    if (const GodotVariant* u = arr(d, "uv_scale"); u && u->type == GodotVType::Vector4)
        for (int i = 0; i < 4; ++i) uvScale[i] = f32(u->num(static_cast<usize>(i)));
    s.compressed = (s.format & kGdFlagCompressAttributes) != 0;
    if (s.formatVersion > 1) note("format version " + std::to_string(s.formatVersion) + " is newer than the layout this reads (version 1); the data may be misread");
    if (s.formatVersion == 0) note("an old-format (Godot 4.0/4.1, no version bit) surface: read with the current layout; compressed attributes in it are not the same coding");
    s.hasLods = arr(d, "lods") != nullptr;
    s.hasBlendShapes = arr(d, "blend_shape_data") != nullptr;
    if (s.hasLods) note("the surface's LOD index buffers are dropped (the full-detail triangles are used)");
    if (s.hasBlendShapes) note("blend shapes are not decoded");

    const u32 n = s.vertexCount;
    const bool d2 = (s.format & kGdFlag2dVertices) != 0;
    if (n == 0) return fail("the surface has no vertices");
    if (!(s.format & kGdArrayVertex) && !(s.format & kGdFlagEmptyVertexArray)) return fail("the surface has no vertex array");

    // ---- strides and offsets, in Godot's order (mesh_surface_make_offsets_from_format) ----
    const int boneCount = (s.format & kGdFlag8BoneWeights) ? 8 : 4;
    const usize vertexStride = d2 ? 8 : (s.compressed ? 8 : 12);
    usize normalStride = 0, normalOff = 0, tangentOff = 0;
    usize attribStride = 0, colorOff = 0, uvOff = 0, uv2Off = 0, customOff[4] = {0, 0, 0, 0};
    usize skinStride = 0, bonesOff = 0, weightsOff = 0;
    if (s.format & kGdArrayNormal) { normalOff = normalStride; normalStride += 4; }
    if (s.format & kGdArrayTangent) { tangentOff = normalStride; normalStride += s.compressed ? 0 : 4; }
    if (s.format & kGdArrayColor) { colorOff = attribStride; attribStride += 4; }
    if (s.format & kGdArrayUv) { uvOff = attribStride; attribStride += s.compressed ? 4 : 8; }
    if (s.format & kGdArrayUv2) { uv2Off = attribStride; attribStride += s.compressed ? 4 : 8; }
    for (int c = 0; c < 4; ++c) {
        if (!(s.format & (1ull << (6 + c)))) continue;
        const int cf = static_cast<int>((s.format >> kGdArrayCustomShift[c]) & 7);
        s.customFormat[c] = cf;
        customOff[c] = attribStride;
        attribStride += static_cast<usize>(customSize(cf));
        note("custom data " + std::to_string(c) + " is not decoded");
    }
    if (s.format & kGdArrayBones) { bonesOff = skinStride; skinStride += sizeof(u16) * static_cast<usize>(boneCount); }
    if (s.format & kGdArrayWeights) { weightsOff = skinStride; skinStride += sizeof(u16) * static_cast<usize>(boneCount); }

    const std::vector<u8>& V = vd->bytes;
    const usize positionsBytes = vertexStride * n;
    if (V.size() < positionsBytes + normalStride * n) return fail("vertex_data is " + std::to_string(V.size()) + " bytes; the format needs " + std::to_string(positionsBytes + normalStride * n));
    const u8* P = V.data();
    const u8* NT = V.data() + positionsBytes;

    // ---- positions ----
    s.pos.assign(usize(n) * 3, 0.0f);
    if (d2) {
        for (u32 i = 0; i < n; ++i) { s.pos[usize(i) * 3] = rdF(P + usize(i) * 8); s.pos[usize(i) * 3 + 1] = rdF(P + usize(i) * 8 + 4); }
    } else if (s.compressed) {
        for (u32 i = 0; i < n; ++i)
            for (int k = 0; k < 3; ++k)
                s.pos[usize(i) * 3 + usize(k)] = f32(double(rd16(P + usize(i) * 8 + usize(k) * 2)) / 65535.0 * s.aabb[3 + k] + s.aabb[k]);
    } else {
        for (u32 i = 0; i < n; ++i)
            for (int k = 0; k < 3; ++k) s.pos[usize(i) * 3 + usize(k)] = rdF(P + usize(i) * 12 + usize(k) * 4);
    }

    // ---- normals and tangents ----
    const bool hasN = (s.format & kGdArrayNormal) != 0, hasT = (s.format & kGdArrayTangent) != 0;
    if (hasN) s.nrm.assign(usize(n) * 3, 0.0f);
    if (hasT) s.tan.assign(usize(n) * 4, 0.0f);
    for (u32 i = 0; i < n; ++i) {
        if (s.compressed && !d2) {
            // The normal word is the rotation axis of the tangent frame; the position's fourth u16 is the angle (with the handedness in which half it falls).
            if (!hasN && !hasT) continue;
            const u32 w = rd32(NT + usize(i) * normalStride + normalOff);
            const f32 oct[2] = {f32((w & 0xFFFF) / 65535.0), f32((w >> 16) / 65535.0)};
            f32 axis[3];
            gdOctDecode(oct, axis);
            f32 nn[3], tt[4];
            tbnFromAxisAngle(axis, double(rd16(P + usize(i) * 8 + 6)) / 65535.0, nn, tt);
            if (hasN) for (int k = 0; k < 3; ++k) s.nrm[usize(i) * 3 + usize(k)] = nn[k];
            if (hasT) for (int k = 0; k < 4; ++k) s.tan[usize(i) * 4 + usize(k)] = tt[k];
            continue;
        }
        if (hasN) {
            const u32 w = rd32(NT + usize(i) * normalStride + normalOff);
            const f32 oct[2] = {f32((w & 0xFFFF) / 65535.0), f32((w >> 16) / 65535.0)};
            gdOctDecode(oct, &s.nrm[usize(i) * 3]);
        }
        if (hasT) {
            const u32 w = rd32(NT + usize(i) * normalStride + tangentOff);
            f32 oy = f32((w >> 16) / 65535.0) * 2.0f - 1.0f;
            const f32 sign = oy >= 0.0f ? 1.0f : -1.0f;
            const f32 oct[2] = {f32((w & 0xFFFF) / 65535.0), std::fabs(oy)};
            gdOctDecode(oct, &s.tan[usize(i) * 4]);
            s.tan[usize(i) * 4 + 3] = sign;
        }
    }

    // ---- attributes (colour, UV, UV2) ----
    const bool hasC = (s.format & kGdArrayColor) != 0, hasU = (s.format & kGdArrayUv) != 0, hasU2 = (s.format & kGdArrayUv2) != 0;
    if (hasC || hasU || hasU2) {
        const GodotVariant* ad = arr(d, "attribute_data");
        if (!ad || ad->type != GodotVType::PackedByteArray || ad->bytes.size() < attribStride * n)
            return fail("attribute_data is missing or shorter than the format needs (" + std::to_string(attribStride * n) + " bytes)");
        const u8* A = ad->bytes.data();
        if (hasC) {
            s.col.assign(usize(n) * 4, 1.0f);
            for (u32 i = 0; i < n; ++i)
                for (int k = 0; k < 4; ++k) s.col[usize(i) * 4 + usize(k)] = f32(A[usize(i) * attribStride + colorOff + usize(k)] / 255.0);
        }
        const auto readUv = [&](std::vector<f32>& dst, usize off, const f32* scale) {
            dst.assign(usize(n) * 2, 0.0f);
            for (u32 i = 0; i < n; ++i) {
                const u8* q = A + usize(i) * attribStride + off;
                if (s.compressed) {
                    double u = rd16(q) / 65535.0, v = rd16(q + 2) / 65535.0;
                    if (scale[0] != 0.0f || scale[1] != 0.0f) { u = (u - 0.5) * scale[0]; v = (v - 0.5) * scale[1]; }
                    dst[usize(i) * 2] = f32(u); dst[usize(i) * 2 + 1] = f32(v);
                } else {
                    dst[usize(i) * 2] = rdF(q); dst[usize(i) * 2 + 1] = rdF(q + 4);
                }
            }
        };
        if (hasU) readUv(s.uv, uvOff, uvScale);
        if (hasU2) readUv(s.uv2, uv2Off, uvScale + 2);
    }

    // ---- skin ----
    if (s.format & kGdArrayBones) {
        const GodotVariant* sd = arr(d, "skin_data");
        if (!sd || sd->type != GodotVType::PackedByteArray || sd->bytes.size() < skinStride * n) return fail("skin_data is missing or shorter than the format needs");
        s.boneCount = boneCount;
        s.bones.assign(usize(n) * usize(boneCount), 0);
        s.weights.assign(usize(n) * usize(boneCount), 0.0f);
        for (u32 i = 0; i < n; ++i)
            for (int k = 0; k < boneCount; ++k) {
                s.bones[usize(i) * usize(boneCount) + usize(k)] = rd16(sd->bytes.data() + usize(i) * skinStride + bonesOff + usize(k) * 2);
                if (s.format & kGdArrayWeights)
                    s.weights[usize(i) * usize(boneCount) + usize(k)] = f32(rd16(sd->bytes.data() + usize(i) * skinStride + weightsOff + usize(k) * 2) / 65535.0);
            }
    }

    // ---- indices ----
    std::vector<u32> raw;
    if (s.format & kGdArrayIndex) {
        const GodotVariant* id = arr(d, "index_data");
        const GodotVariant* ic = arr(d, "index_count");
        if (!id || id->type != GodotVType::PackedByteArray || !ic) return fail("the format says indexed but index_data or index_count is missing");
        const usize cnt = static_cast<usize>(std::max<i64>(ic->asInt(), 0));
        const usize w = n <= (1u << 16) ? 2 : 4;
        if (id->bytes.size() < cnt * w) return fail("index_data is shorter than index_count says");
        raw.resize(cnt);
        for (usize i = 0; i < cnt; ++i) raw[i] = w == 2 ? rd16(id->bytes.data() + i * 2) : rd32(id->bytes.data() + i * 4);
    } else {
        raw.resize(n);
        for (u32 i = 0; i < n; ++i) raw[i] = i;
    }
    for (const u32 i : raw)
        if (i >= n) return fail("an index (" + std::to_string(i) + ") is past the vertex count " + std::to_string(n));
    if (s.primitive == 4) {   // triangle strip: alternate the winding
        if (raw.size() >= 3)
            for (usize i = 0; i + 2 < raw.size(); ++i) {
                const u32 a = raw[i], b = raw[i + 1], c = raw[i + 2];
                if (a == b || b == c || a == c) continue;   // a degenerate triangle joins strips
                if (i % 2 == 0) { s.indices.push_back(a); s.indices.push_back(b); s.indices.push_back(c); }
                else            { s.indices.push_back(b); s.indices.push_back(a); s.indices.push_back(c); }
            }
        note("a triangle strip was expanded to triangles");
    } else {
        raw.resize(raw.size() / 3 * 3);
        s.indices = std::move(raw);
    }
    if (s.indices.empty()) return fail("the surface has no triangles");
    return true;
}

// ---- GridMap ----

bool decodeGridCells(const GodotVariant& data, std::vector<GodotGridCell>& out, std::string* why) {
    out.clear();
    const GodotVariant* cells = data.type == GodotVType::Dictionary ? data.find("cells") : (data.type == GodotVType::PackedInt32Array ? &data : nullptr);
    if (!cells) return true;   // no cells is an empty grid
    if (cells->type != GodotVType::PackedInt32Array) { if (why) *why = "cells is not a PackedInt32Array"; return false; }
    if (cells->nums.size() % 3 != 0) { if (why) *why = "the cells array is not a multiple of three ints"; return false; }
    for (usize i = 0; i + 2 < cells->nums.size(); i += 3) {
        const u32 w0 = static_cast<u32>(static_cast<i32>(cells->nums[i])), w1 = static_cast<u32>(static_cast<i32>(cells->nums[i + 1]));
        const u32 v = static_cast<u32>(static_cast<i32>(cells->nums[i + 2]));
        GodotGridCell c;
        c.x = static_cast<i16>(w0 & 0xFFFF);
        c.y = static_cast<i16>(w0 >> 16);
        c.z = static_cast<i16>(w1 & 0xFFFF);
        c.item = static_cast<int>(v & 0xFFFF);
        c.orientation = static_cast<int>((v >> 16) & 0x1F);
        c.layer = static_cast<int>((v >> 21) & 0xFF);
        if (c.item == 0xFFFF) continue;
        out.push_back(c);
    }
    return true;
}

void encodeGridCell(const GodotGridCell& c, i32 out3[3]) {
    out3[0] = static_cast<i32>((static_cast<u32>(static_cast<u16>(c.x))) | (static_cast<u32>(static_cast<u16>(c.y)) << 16));
    out3[1] = static_cast<i32>(static_cast<u32>(static_cast<u16>(c.z)));
    out3[2] = static_cast<i32>((static_cast<u32>(c.item) & 0xFFFF) | ((static_cast<u32>(c.orientation) & 0x1F) << 16) | ((static_cast<u32>(c.layer) & 0xFF) << 21));
}

void gdGridBasis(int index, double out[9]) {
    static const signed char t[24][9] = {
        {1, 0, 0, 0, 1, 0, 0, 0, 1},   {0, -1, 0, 1, 0, 0, 0, 0, 1},  {-1, 0, 0, 0, -1, 0, 0, 0, 1},  {0, 1, 0, -1, 0, 0, 0, 0, 1},
        {1, 0, 0, 0, 0, -1, 0, 1, 0},  {0, 0, 1, 1, 0, 0, 0, 1, 0},   {-1, 0, 0, 0, 0, 1, 0, 1, 0},   {0, 0, -1, -1, 0, 0, 0, 1, 0},
        {1, 0, 0, 0, -1, 0, 0, 0, -1}, {0, 1, 0, 1, 0, 0, 0, 0, -1},  {-1, 0, 0, 0, 1, 0, 0, 0, -1},  {0, -1, 0, -1, 0, 0, 0, 0, -1},
        {1, 0, 0, 0, 0, 1, 0, -1, 0},  {0, 0, -1, 1, 0, 0, 0, -1, 0}, {-1, 0, 0, 0, 0, -1, 0, -1, 0},  {0, 0, 1, -1, 0, 0, 0, -1, 0},
        {0, 0, 1, 0, 1, 0, -1, 0, 0},  {0, -1, 0, 0, 0, 1, -1, 0, 0}, {0, 0, -1, 0, -1, 0, -1, 0, 0},  {0, 1, 0, 0, 0, -1, -1, 0, 0},
        {0, 0, 1, 0, -1, 0, 1, 0, 0},  {0, 1, 0, 0, 0, 1, 1, 0, 0},   {0, 0, -1, 0, 1, 0, 1, 0, 0},    {0, -1, 0, 0, 0, -1, 1, 0, 0}};
    const int i = index >= 0 && index < 24 ? index : 0;   // Godot answers an identity Basis for an index out of range
    for (int k = 0; k < 9; ++k) out[k] = t[i][k];
}

}  // namespace aver::fmt::godot
