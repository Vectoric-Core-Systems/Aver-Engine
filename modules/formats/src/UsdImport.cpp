#include "aver/formats/UsdImport.hpp"
#include "UsdImportInternal.hpp"

#include "aver/platform/Image.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace aver::fmt {
// Named rather than anonymous since UsdStageImport.cpp shares these (UsdImportInternal.hpp).
namespace usd_detail {

bool fail(std::string* why, std::string m) { if (why) *why = std::move(m); return false; }

// ---- 4x4, row-major, row-vector (v * M) -------------------------------------------------------
//
// THE SAME CONVENTION USD USES, which is the happy part of this importer: GfMatrix4d is row-major
// and pre-multiplies row vectors, exactly like this engine. So a matrix4d in a .usda can be copied
// straight across without a transpose, and composition order reads the same in both. Getting this
// backwards transposes every rotation, which looks like an axis-convention bug and is not one.
// (M4 itself is declared in UsdImportInternal.hpp.)

M4 mul(const M4& a, const M4& b) {
    M4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            f32 s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}

void xformPoint(const M4& m, f32 x, f32 y, f32 z, f32 out[3]) {
    out[0] = x * m.m[0] + y * m.m[4] + z * m.m[8]  + m.m[12];
    out[1] = x * m.m[1] + y * m.m[5] + z * m.m[9]  + m.m[13];
    out[2] = x * m.m[2] + y * m.m[6] + z * m.m[10] + m.m[14];
}

// Normals ignore translation. They are NOT inverse-transpose corrected here, which is correct only
// for rigid + uniform scale; a non-uniform scale in the stage is reported as unsupported rather
// than silently shading wrong.
void xformDir(const M4& m, f32 x, f32 y, f32 z, f32 out[3]) {
    out[0] = x * m.m[0] + y * m.m[4] + z * m.m[8];
    out[1] = x * m.m[1] + y * m.m[5] + z * m.m[9];
    out[2] = x * m.m[2] + y * m.m[6] + z * m.m[10];
}

M4 translate(f32 x, f32 y, f32 z) {
    M4 r = M4::identity();
    r.m[12] = x; r.m[13] = y; r.m[14] = z;
    return r;
}
M4 scaleM(f32 x, f32 y, f32 z) {
    M4 r = M4::identity();
    r.m[0] = x; r.m[5] = y; r.m[10] = z;
    return r;
}
M4 rotateAxis(int axis, f32 deg) {
    const f32 r = deg * 3.14159265358979323846f / 180.0f;
    const f32 c = std::cos(r), s = std::sin(r);
    M4 o = M4::identity();
    // Row-vector rotations: the sign pattern is the transpose of the column-vector form.
    if (axis == 0)      { o.m[5] = c;  o.m[6] = s;  o.m[9]  = -s; o.m[10] = c; }
    else if (axis == 1) { o.m[0] = c;  o.m[2] = -s; o.m[8]  = s;  o.m[10] = c; }
    else                { o.m[0] = c;  o.m[1] = s;  o.m[4]  = -s; o.m[5]  = c; }
    return o;
}

// The inverse of an affine row-vector matrix: the 3x3 basis inverted by cofactors, the translation
// carried back through it. Identity when the basis is singular (a zero scale), rather than NaN.
M4 inverseAffine(const M4& a) {
    const f32* m = a.m;
    const f32 c00 = m[5] * m[10] - m[6] * m[9];
    const f32 c01 = m[6] * m[8]  - m[4] * m[10];
    const f32 c02 = m[4] * m[9]  - m[5] * m[8];
    const f32 det = m[0] * c00 + m[1] * c01 + m[2] * c02;
    if (std::fabs(det) < 1e-20f) return M4::identity();
    const f32 inv = 1.0f / det;
    M4 r = M4::identity();
    r.m[0]  = c00 * inv;
    r.m[1]  = (m[2] * m[9]  - m[1] * m[10]) * inv;
    r.m[2]  = (m[1] * m[6]  - m[2] * m[5])  * inv;
    r.m[4]  = c01 * inv;
    r.m[5]  = (m[0] * m[10] - m[2] * m[8])  * inv;
    r.m[6]  = (m[2] * m[4]  - m[0] * m[6])  * inv;
    r.m[8]  = c02 * inv;
    r.m[9]  = (m[1] * m[8]  - m[0] * m[9])  * inv;
    r.m[10] = (m[0] * m[5]  - m[1] * m[4])  * inv;
    // Row-vector: v' = v*B + t, so v = (v' - t) * B^-1, i.e. the new translation is -t * B^-1.
    for (int j = 0; j < 3; ++j)
        r.m[12 + j] = -(m[12] * r.m[j] + m[13] * r.m[4 + j] + m[14] * r.m[8 + j]);
    return r;
}

// ---- scanning ---------------------------------------------------------------------------------

struct Scanner {
    const char* p;
    const char* end;

    void skip() {
        while (p < end) {
            if (*p == '#') { while (p < end && *p != '\n') ++p; }
            else if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') ++p;
            else break;
        }
    }
    bool eof() { skip(); return p >= end; }
    char peek() { skip(); return p < end ? *p : '\0'; }
    bool accept(char c) { if (peek() == c) { ++p; return true; } return false; }

    // An identifier, possibly with USD's namespace colons and brackets: `primvars:st`,
    // `xformOp:translate`, `point3f[]`.
    std::string_view ident() {
        skip();
        const char* s = p;
        while (p < end && (std::isalnum(static_cast<unsigned char>(*p)) || *p == '_' || *p == ':' ||
                           *p == '.' || *p == '[' || *p == ']'))
            ++p;
        return std::string_view(s, static_cast<usize>(p - s));
    }

    std::string quoted() {
        skip();
        std::string out;
        if (p >= end) return out;
        // USD also has triple-quoted strings; treat them as ordinary so the closing quote logic
        // does not run away to the end of file.
        if (*p != '"' && *p != '\'') return out;
        const char q = *p++;
        while (p < end && *p != q) {
            if (*p == '\\' && p + 1 < end) ++p;
            out.push_back(*p++);
        }
        if (p < end) ++p;
        return out;
    }

    f32 number(bool* ok = nullptr) {
        skip();
        char* endp = nullptr;
        const f32 v = std::strtof(p, &endp);
        if (endp == p) { if (ok) *ok = false; return 0.0f; }
        p = endp;
        if (ok) *ok = true;
        return v;
    }

    // Consumes a balanced region starting at the current `open` char, returning its interior.
    std::string_view balanced(char open, char close) {
        skip();
        if (p >= end || *p != open) return {};
        ++p;
        const char* s = p;
        int depth = 1;
        while (p < end && depth > 0) {
            if (*p == '"' || *p == '\'') { const char q = *p++; while (p < end && *p != q) { if (*p == '\\' && p + 1 < end) ++p; ++p; } if (p < end) ++p; continue; }
            if (*p == open) ++depth;
            else if (*p == close) { if (--depth == 0) break; }
            ++p;
        }
        const std::string_view r(s, static_cast<usize>(p - s));
        if (p < end) ++p;
        return r;
    }
};

// Pulls every number out of a region, ignoring the tuple/array punctuation that separates them.
// USD writes `[(0, 0, 1), (1, 0, 0)]`, and for the attributes this importer wants, the flat number
// sequence IS the data — the grouping is implied by the component count.
void allNumbers(std::string_view region, std::vector<f32>& out) {
    const char* p = region.data();
    const char* end = p + region.size();
    while (p < end) {
        if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' ||
            (*p == '.' && p + 1 < end && p[1] >= '0' && p[1] <= '9')) {
            char* endp = nullptr;
            const f32 v = std::strtof(p, &endp);
            if (endp == p) { ++p; continue; }
            out.push_back(v);
            p = endp;
        } else ++p;
    }
}

void allInts(std::string_view region, std::vector<i32>& out) {
    const char* p = region.data();
    const char* end = p + region.size();
    while (p < end) {
        if ((*p >= '0' && *p <= '9') || *p == '-') {
            char* endp = nullptr;
            const long v = std::strtol(p, &endp, 10);
            if (endp == p) { ++p; continue; }
            out.push_back(static_cast<i32>(v));
            p = endp;
        } else ++p;
    }
}

// ---- a prim under construction ----------------------------------------------------------------
// MeshAttrs, GeomSubsetDef, ShaderPrim, RawMesh and Ctx are declared, with their field notes, in
// UsdImportInternal.hpp, because the stage importer fills the same structs from binary layers.

// The basis change, applied AFTER the prim's own transform has placed the point in stage space.
//
// A Y-UP STAGE is the glTF/OBJ case and gets the same {-z, x, y} used there. A Z-UP STAGE already
// agrees with this engine about which way is up and needs only the handedness flip, which is one
// axis negation — USD is right-handed, the engine is left-handed. Treating a Z-up stage as if it
// were Y-up is the single most common USD import bug and it lays the model on its side.
inline void toEngine(bool convert, bool yUp, const f32 in[3], f32 out[3]) {
    if (!convert)   { out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; return; }
    if (yUp)        { out[0] = -in[2]; out[1] = in[0]; out[2] = in[1]; }
    else            { out[0] = in[0];  out[1] = -in[1]; out[2] = in[2]; }
}

void computeBounds(OcMeshData& m) {
    if (m.positions.size() < 3) { m.boundsMin = m.boundsMax = Vec3{0, 0, 0}; return; }
    Vec3 lo{m.positions[0], m.positions[1], m.positions[2]}, hi = lo;
    for (usize i = 3; i + 2 < m.positions.size(); i += 3) {
        lo.x = std::min(lo.x, m.positions[i]);     hi.x = std::max(hi.x, m.positions[i]);
        lo.y = std::min(lo.y, m.positions[i + 1]); hi.y = std::max(hi.y, m.positions[i + 1]);
        lo.z = std::min(lo.z, m.positions[i + 2]); hi.z = std::max(hi.z, m.positions[i + 2]);
    }
    m.boundsMin = lo;
    m.boundsMax = hi;
}

void generateNormals(OcMeshData& m) {
    const usize vn = m.positions.size() / 3;
    m.normals.assign(vn * 3, 0.0f);
    for (usize i = 0; i + 2 < m.indices.size(); i += 3) {
        const u32 a = m.indices[i], b = m.indices[i + 1], c = m.indices[i + 2];
        if (a >= vn || b >= vn || c >= vn) continue;
        const f32* pa = &m.positions[a * 3];
        const f32* pb = &m.positions[b * 3];
        const f32* pc = &m.positions[c * 3];
        const f32 e1[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
        const f32 e2[3] = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
        const f32 n[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                          e1[2] * e2[0] - e1[0] * e2[2],
                          e1[0] * e2[1] - e1[1] * e2[0]};
        for (const u32 v : {a, b, c})
            for (int k = 0; k < 3; ++k) m.normals[v * 3 + k] += n[k];
    }
    for (usize v = 0; v < vn; ++v) {
        f32* n = &m.normals[v * 3];
        const f32 len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 1e-12f) { n[0] /= len; n[1] /= len; n[2] /= len; }
        else { n[0] = 0.0f; n[1] = 0.0f; n[2] = 1.0f; }
    }
}

// Merges vertices whose position, normal and UV are BIT-IDENTICAL, rewriting the index buffer.
//
// THE CORNER-PER-VERTEX BUILD BELOW IS SAFE, NOT CHEAP. A smooth, continuously-mapped mesh -- a
// terrain tile -- gives every shared point the same normal and UV at each of its corners, so the
// build emits ~2.5 vertices per triangle where the welded mesh needs ~0.5. Measured on Jungle Ruins:
// 39.6M vertices (1.2 GiB of 32-byte vertices) for 15.5M triangles, carried into VRAM, the ray-tracing
// geometry and a 13.7-second collision build. Exact equality only, so nothing a renderer could see
// changes: a crease (split normal) or a UV seam keeps its distinct vertices.
void weldIdenticalVertices(OcMeshData& m) {
    const u32 n = m.vertexCount();
    if (n < 2 || m.normals.size() != usize(n) * 3 || m.uvs.size() != usize(n) * 2) return;
    struct Key { f32 v[8]; };
    struct KeyHash {
        usize operator()(const Key& k) const {
            u64 h = 1469598103934665603ull;
            const u8* b = reinterpret_cast<const u8*>(k.v);
            for (usize i = 0; i < sizeof k.v; ++i) h = (h ^ b[i]) * 1099511628211ull;
            return static_cast<usize>(h);
        }
    };
    struct KeyEq {
        bool operator()(const Key& a, const Key& b) const { return std::memcmp(a.v, b.v, sizeof a.v) == 0; }
    };
    std::unordered_map<Key, u32, KeyHash, KeyEq> first;
    first.reserve(n);
    std::vector<u32> remap(n);
    std::vector<f32> pos, nrm, uv;
    pos.reserve(m.positions.size()); nrm.reserve(m.normals.size()); uv.reserve(m.uvs.size());
    for (u32 i = 0; i < n; ++i) {
        Key k;
        std::memcpy(k.v,     &m.positions[usize(i) * 3], 3 * sizeof(f32));
        std::memcpy(k.v + 3, &m.normals[usize(i) * 3],   3 * sizeof(f32));
        std::memcpy(k.v + 6, &m.uvs[usize(i) * 2],       2 * sizeof(f32));
        const auto [it, inserted] = first.try_emplace(k, static_cast<u32>(pos.size() / 3));
        if (inserted) {
            pos.insert(pos.end(), k.v, k.v + 3);
            nrm.insert(nrm.end(), k.v + 3, k.v + 6);
            uv.insert(uv.end(), k.v + 6, k.v + 8);
        }
        remap[i] = it->second;
    }
    if (pos.size() == m.positions.size()) return;   // nothing shared: leave the mesh untouched
    for (u32& idx : m.indices) idx = remap[idx];
    m.positions = std::move(pos);
    m.normals = std::move(nrm);
    m.uvs = std::move(uv);
}

// True when `a` is Blender's exporter contradicting itself: faceVarying normals that are FLAT PER
// FACE (a face's corners all but identical) on a mesh whose `primvars:sharp_face` marks most faces
// smooth. See UsdImportOptions::honourSharpFace.
bool flatNormalsOnSmoothFaces(const MeshAttrs& a) {
    const usize faces = a.faceVertexCounts.size();
    if (faces == 0 || a.sharpFace.size() != faces) return false;
    usize smooth = 0;
    for (const i32 s : a.sharpFace) if (!s) ++smooth;
    if (smooth * 2 < faces) return false;
    const usize corners = a.faceVertexIndices.size();
    if (a.normals.size() != corners * 3) return false;
    usize flat = 0, checked = 0, corner = 0;
    for (const i32 raw : a.faceVertexCounts) {
        const usize n = raw > 0 ? static_cast<usize>(raw) : 0;
        if (corner + n > corners) break;
        if (n >= 3) {
            ++checked;
            const f32* n0 = &a.normals[corner * 3];
            bool same = true;
            for (usize k = 1; k < n && same; ++k) {
                const f32* nk = &a.normals[(corner + k) * 3];
                same = n0[0] * nk[0] + n0[1] * nk[1] + n0[2] * nk[2] > 0.9999995f;   // within ~0.06 deg
            }
            if (same) ++flat;
        }
        corner += n;
    }
    return checked && flat * 10 >= checked * 9;
}

// Rebuilds `a.normals` (faceVarying) as smooth normals from the geometry: each corner of a smooth
// face takes the corner-angle-weighted mean of the face normals around its POSITION, limited to faces
// within 60 degrees of its own (Blender's smooth-by-angle, at a generous angle); a sharp face keeps
// its own flat normal.
//
// BY POSITION, NOT BY POINT INDEX: Jungle Ruins' terrain is a quad soup -- every quad owns its four
// points (166,462 points for 83,232 triangles where a welded grid needs ~42,000), so averaging per
// point index smooths only the two triangles of one quad and the tile stays faceted quad by quad.
// Points at bit-identical positions are the same surface point for this purpose. THE ANGLE LIMIT
// keeps a real crease a crease: a terrain's bumps are tens of degrees, a building's corner is ninety.
//
// Each new normal is kept on the side of the one it replaces, so the source's orientation and winding
// are honoured whichever they were.
void rebuildSmoothNormals(MeshAttrs& a) {
    const usize points = a.points.size() / 3;
    const usize corners = a.faceVertexIndices.size();
    constexpr f64 kCosLimit = 0.5;   // 60 degrees

    // Canonical id per point: its bit-identical position.
    struct PosKey { f32 v[3]; };
    struct PosHash {
        usize operator()(const PosKey& k) const {
            u64 h = 1469598103934665603ull;
            const u8* b = reinterpret_cast<const u8*>(k.v);
            for (usize i = 0; i < sizeof k.v; ++i) h = (h ^ b[i]) * 1099511628211ull;
            return static_cast<usize>(h);
        }
    };
    struct PosEq { bool operator()(const PosKey& x, const PosKey& y) const { return std::memcmp(x.v, y.v, sizeof x.v) == 0; } };
    std::unordered_map<PosKey, u32, PosHash, PosEq> byPos;
    byPos.reserve(points);
    std::vector<u32> canon(points);
    for (usize p = 0; p < points; ++p) {
        PosKey k;
        std::memcpy(k.v, &a.points[p * 3], sizeof k.v);
        canon[p] = byPos.try_emplace(k, static_cast<u32>(byPos.size())).first->second;
    }
    const usize canonCount = byPos.size();

    // Per face: its unit normal (Newell) and whether it is smooth. Per corner: its weight (the
    // polygon's interior angle there) and its canonical point.
    const usize faces = a.faceVertexCounts.size();
    std::vector<f64> faceN(faces * 3, 0.0);
    std::vector<u8> faceSmooth(faces, 1);
    std::vector<f64> cornerW(corners, 0.0);
    std::vector<u32> cornerFace(corners, 0), cornerCanon(corners, 0xFFFFFFFFu);
    usize corner = 0;
    for (usize f = 0; f < faces; ++f) {
        const i32 raw = a.faceVertexCounts[f];
        const usize n = raw > 0 ? static_cast<usize>(raw) : 0;
        if (corner + n > corners) break;
        faceSmooth[f] = f < a.sharpFace.size() && a.sharpFace[f] ? 0 : 1;
        bool ok = n >= 3;
        for (usize k = 0; k < n && ok; ++k) {
            const i32 i = a.faceVertexIndices[corner + k];
            ok = i >= 0 && usize(i) < points;
        }
        if (ok) {
            f64* fn = &faceN[f * 3];
            for (usize k = 0; k < n; ++k) {
                const f32* p = &a.points[usize(a.faceVertexIndices[corner + k]) * 3];
                const f32* q = &a.points[usize(a.faceVertexIndices[corner + (k + 1) % n]) * 3];
                fn[0] += (static_cast<f64>(p[1]) - q[1]) * (static_cast<f64>(p[2]) + q[2]);
                fn[1] += (static_cast<f64>(p[2]) - q[2]) * (static_cast<f64>(p[0]) + q[0]);
                fn[2] += (static_cast<f64>(p[0]) - q[0]) * (static_cast<f64>(p[1]) + q[1]);
            }
            const f64 len = std::sqrt(fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2]);
            if (len > 1e-30) { fn[0] /= len; fn[1] /= len; fn[2] /= len; }
            for (usize k = 0; k < n; ++k) {
                const usize ci = corner + k;
                const f32* c = &a.points[usize(a.faceVertexIndices[ci]) * 3];
                const f32* pp = &a.points[usize(a.faceVertexIndices[corner + (k + n - 1) % n]) * 3];
                const f32* pn = &a.points[usize(a.faceVertexIndices[corner + (k + 1) % n]) * 3];
                f64 e0[3], e1[3];
                for (int j = 0; j < 3; ++j) { e0[j] = static_cast<f64>(pp[j]) - c[j]; e1[j] = static_cast<f64>(pn[j]) - c[j]; }
                const f64 l0 = std::sqrt(e0[0] * e0[0] + e0[1] * e0[1] + e0[2] * e0[2]);
                const f64 l1 = std::sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
                if (l0 > 1e-30 && l1 > 1e-30)
                    cornerW[ci] = std::acos(std::fmax(-1.0, std::fmin(1.0, (e0[0] * e1[0] + e0[1] * e1[1] + e0[2] * e1[2]) / (l0 * l1))));
                cornerFace[ci] = static_cast<u32>(f);
                cornerCanon[ci] = canon[usize(a.faceVertexIndices[ci])];
            }
        }
        corner += n;
    }

    // The smooth corners around each canonical point, as a compact list (counting sort).
    std::vector<u32> start(canonCount + 1, 0);
    for (usize ci = 0; ci < corners; ++ci)
        if (cornerCanon[ci] != 0xFFFFFFFFu && faceSmooth[cornerFace[ci]]) ++start[cornerCanon[ci] + 1];
    for (usize k = 0; k < canonCount; ++k) start[k + 1] += start[k];
    std::vector<u32> around(start[canonCount]);
    {
        std::vector<u32> fill(start.begin(), start.end() - 1);
        for (usize ci = 0; ci < corners; ++ci)
            if (cornerCanon[ci] != 0xFFFFFFFFu && faceSmooth[cornerFace[ci]]) around[fill[cornerCanon[ci]]++] = static_cast<u32>(ci);
    }

    for (usize ci = 0; ci < corners; ++ci) {
        if (cornerCanon[ci] == 0xFFFFFFFFu) continue;
        const u32 f = cornerFace[ci];
        const f64* own = &faceN[usize(f) * 3];
        f64 nn[3] = {own[0], own[1], own[2]};
        if (faceSmooth[f]) {
            f64 acc[3] = {0.0, 0.0, 0.0};
            for (u32 k = start[cornerCanon[ci]]; k < start[cornerCanon[ci] + 1]; ++k) {
                const u32 oc = around[k];
                const f64* on = &faceN[usize(cornerFace[oc]) * 3];
                if (own[0] * on[0] + own[1] * on[1] + own[2] * on[2] < kCosLimit) continue;
                for (int j = 0; j < 3; ++j) acc[j] += cornerW[oc] * on[j];
            }
            const f64 len = std::sqrt(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]);
            if (len > 1e-30) { nn[0] = acc[0] / len; nn[1] = acc[1] / len; nn[2] = acc[2] / len; }
        }
        f32* dst = &a.normals[ci * 3];
        const f64 sgn = (nn[0] * dst[0] + nn[1] * dst[1] + nn[2] * dst[2]) < 0.0 ? -1.0 : 1.0;
        dst[0] = static_cast<f32>(sgn * nn[0]); dst[1] = static_cast<f32>(sgn * nn[1]); dst[2] = static_cast<f32>(sgn * nn[2]);
    }
}

// Turns a gathered UsdGeomMesh into an OcMeshData in engine space.
void buildMesh(Ctx& c, const MeshAttrs& authored, const M4& world, const std::string& primPath,
               const std::vector<GeomSubsetDef>& subsets) {
    // Copied only in the rare case that needs it, so the common path costs nothing.
    MeshAttrs rebuilt;
    const MeshAttrs* chosen = &authored;
    if (c.opt->honourSharpFace && flatNormalsOnSmoothFaces(authored)) {
        rebuilt = authored;
        rebuildSmoothNormals(rebuilt);
        chosen = &rebuilt;
        ++c.rebuiltNormalMeshes;
    }
    const MeshAttrs& a = *chosen;
    if (!a.hasPoints || a.points.size() < 9) return;
    if (a.faceVertexIndices.empty() || a.faceVertexCounts.empty()) return;

    const usize pointCount = a.points.size() / 3;

    // USD stores points once and indexes them per face corner, but normals and UVs are commonly
    // FACE-VARYING — one per corner, not one per point. Those two layouts need different handling,
    // and guessing wrong scrambles the UVs. Corner-indexed data forces a de-index, so this always
    // builds one engine vertex per CORNER, then weldIdenticalVertices merges the corners that turned
    // out identical -- the shared points of a smooth, continuously-mapped surface.
    const usize cornerCount = a.faceVertexIndices.size();
    const bool normalsPerCorner = a.normals.size() / 3 == cornerCount && cornerCount != pointCount;
    const bool uvsPerCorner     = a.uvs.size() / 2 == cornerCount && cornerCount != pointCount;
    const bool haveNormals      = a.normals.size() >= 3;
    const bool haveUvs          = a.uvs.size() >= 2;

    OcMeshData m;
    m.positions.reserve(cornerCount * 3);
    m.normals.reserve(cornerCount * 3);
    m.uvs.reserve(cornerCount * 2);

    // USD's default face winding is counter-clockwise ("rightHanded"); `orientation` may say
    // "leftHanded". The engine flips winding as part of the handedness change, so a leftHanded
    // source cancels that flip rather than doubling it.
    const bool srcLeftHanded = a.orientation == "leftHanded";
    const bool flipWinding = c.opt->convertAxes ? !srcLeftHanded : srcLeftHanded;

    // WHERE EACH SOURCE FACE'S TRIANGLES LANDED, so GeomSubsets -- which address faces, not
    // triangles -- can be turned into submeshes below. A face this loop SKIPS still gets an entry,
    // empty: subset indices count every face the source declared, degenerate ones included, and
    // dropping those entries would shift every later face's number.
    std::vector<std::pair<u32, u32>> faceRange;
    faceRange.reserve(a.faceVertexCounts.size());

    usize corner = 0;
    for (const i32 rawCount : a.faceVertexCounts) {
        const usize n = rawCount > 0 ? static_cast<usize>(rawCount) : 0;
        if (n < 3 || corner + n > cornerCount) { corner += n; faceRange.emplace_back(0u, 0u); continue; }
        const u32 faceIndexStart = static_cast<u32>(m.indices.size());

        const u32 base = static_cast<u32>(m.positions.size() / 3);
        for (usize k = 0; k < n; ++k) {
            const usize ci = corner + k;
            const i32 pi = a.faceVertexIndices[ci];
            if (pi < 0 || static_cast<usize>(pi) >= pointCount) {
                m.positions.insert(m.positions.end(), {0.0f, 0.0f, 0.0f});
                m.normals.insert(m.normals.end(), {0.0f, 0.0f, 0.0f});
                m.uvs.insert(m.uvs.end(), {0.0f, 0.0f});
                continue;
            }

            f32 world3[3];
            xformPoint(world, a.points[pi * 3], a.points[pi * 3 + 1], a.points[pi * 3 + 2], world3);
            f32 eng[3];
            toEngine(c.opt->convertAxes, c.yUp, world3, eng);
            m.positions.push_back(eng[0] * c.unitScale);
            m.positions.push_back(eng[1] * c.unitScale);
            m.positions.push_back(eng[2] * c.unitScale);

            if (haveNormals) {
                const usize ni = normalsPerCorner ? ci : static_cast<usize>(pi);
                if (ni * 3 + 2 < a.normals.size()) {
                    f32 wn[3];
                    xformDir(world, a.normals[ni * 3], a.normals[ni * 3 + 1], a.normals[ni * 3 + 2], wn);
                    f32 en[3];
                    toEngine(c.opt->convertAxes, c.yUp, wn, en);
                    const f32 len = std::sqrt(en[0] * en[0] + en[1] * en[1] + en[2] * en[2]);
                    if (len > 1e-12f) { en[0] /= len; en[1] /= len; en[2] /= len; }
                    m.normals.insert(m.normals.end(), {en[0], en[1], en[2]});
                } else m.normals.insert(m.normals.end(), {0.0f, 0.0f, 0.0f});
            } else m.normals.insert(m.normals.end(), {0.0f, 0.0f, 0.0f});

            if (haveUvs) {
                const usize ti = uvsPerCorner ? ci : static_cast<usize>(pi);
                if (ti * 2 + 1 < a.uvs.size()) {
                    // V flips: USD's texture origin is bottom-left, the engine samples top-left.
                    m.uvs.push_back(a.uvs[ti * 2]);
                    m.uvs.push_back(1.0f - a.uvs[ti * 2 + 1]);
                } else m.uvs.insert(m.uvs.end(), {0.0f, 0.0f});
            } else m.uvs.insert(m.uvs.end(), {0.0f, 0.0f});
        }

        // Fan-triangulate the polygon. Same caveat as the OBJ path: correct for convex faces, and
        // a concave face produces triangles outside its outline.
        for (usize k = 1; k + 1 < n; ++k) {
            if (flipWinding) {
                m.indices.push_back(base);
                m.indices.push_back(base + static_cast<u32>(k + 1));
                m.indices.push_back(base + static_cast<u32>(k));
            } else {
                m.indices.push_back(base);
                m.indices.push_back(base + static_cast<u32>(k));
                m.indices.push_back(base + static_cast<u32>(k + 1));
            }
        }
        faceRange.emplace_back(faceIndexStart,
                               static_cast<u32>(m.indices.size()) - faceIndexStart);
        corner += n;
    }

    if (m.positions.empty() || m.indices.empty()) return;
    if (!haveNormals && c.opt->generateMissingNormals) generateNormals(m);
    weldIdenticalVertices(m);

    // ---- submeshes -----------------------------------------------------------------------
    //
    // THE INDEX BUFFER IS REORDERED, because a submesh is a contiguous [indexStart, indexCount) run
    // and a subset's faces are scattered through the source. The VERTEX buffer is left alone and
    // every submesh spans all of it -- the same shape the glTF path produces, where baseVertex is 0
    // and vertexCount is the whole mesh.
    //
    // A face in no subset keeps the mesh-level binding, in a LAST slot. Blender writes subsets that
    // cover every face, so that group is usually empty; a partial cover would otherwise lose
    // geometry silently, which is the one outcome worth writing code to avoid.
    std::vector<std::string> slotBinding;
    std::vector<u32> reordered;
    reordered.reserve(m.indices.size());
    std::vector<bool> claimed(faceRange.size(), false);

    const auto appendGroup = [&](const std::vector<i32>& faces, const std::string& binding,
                                 const std::string& name) {
        const u32 start = static_cast<u32>(reordered.size());
        for (const i32 f : faces) {
            if (f < 0 || usize(f) >= faceRange.size()) continue;
            const auto& r = faceRange[usize(f)];
            reordered.insert(reordered.end(), m.indices.begin() + r.first,
                             m.indices.begin() + r.first + r.second);
            claimed[usize(f)] = true;
        }
        const u32 count = static_cast<u32>(reordered.size()) - start;
        if (count == 0) return;
        OcMeshSubmesh sm;
        sm.name         = name;
        sm.materialSlot = static_cast<u32>(slotBinding.size());
        sm.indexStart   = start;
        sm.indexCount   = count;
        sm.baseVertex   = 0;
        sm.vertexCount  = m.vertexCount();
        m.submeshes.push_back(std::move(sm));
        // Prefixed the same way buildMaterial registers the material (empty for a plain import).
        slotBinding.push_back(binding.empty() ? std::string() : c.pathPrefix + binding);
    };

    for (const GeomSubsetDef& g : subsets)
        appendGroup(g.faces, g.binding, primPath + "/" + g.name);

    std::vector<i32> leftover;
    for (usize f = 0; f < faceRange.size(); ++f)
        if (!claimed[f] && faceRange[f].second) leftover.push_back(static_cast<i32>(f));
    if (!subsets.empty() && !leftover.empty())
        c.sawPartialSubsetCover = true;
    if (subsets.empty() || !leftover.empty())
        appendGroup(leftover, a.materialBinding, primPath);

    if (m.submeshes.empty()) return;
    m.indices = std::move(reordered);
    // EMPTY FOR NOW, always: a binding names a prim path that may not have been read yet, so the
    // slots are filled in after the walk -- see resolveBindings.
    m.materialSlots.assign(slotBinding.size(), std::string());
    computeBounds(m);

    c.out->meshes.push_back(std::move(m));
    c.out->meshNames.push_back(primPath);
    c.slotDoubleSided.emplace_back(slotBinding.size(), a.doubleSided);
    c.meshBinding.push_back(std::move(slotBinding));
}

// ---- materials --------------------------------------------------------------------------------
//
// The subset is UsdPreviewSurface, the one shading model USD itself specifies rather than leaves to
// a renderer. Anything else (a MaterialX graph, a renderer-specific shader like PxrSurface) has no
// mapping onto pbr::MaterialDesc that would not be invented, so it is left alone and the mesh keeps
// an unbound slot -- which is visible -- rather than a guess, which is not.
//
// SHAPE OF WHAT IS BEING READ:
//
//   def Material "Bark" {
//       token outputs:surface.connect = </Looks/Bark/Surface.outputs:surface>
//       def Shader "Surface" {
//           uniform token info:id = "UsdPreviewSurface"
//           color3f inputs:diffuseColor.connect = </Looks/Bark/Albedo.outputs:rgb>
//           float   inputs:roughness = 0.7
//       }
//       def Shader "Albedo" {
//           uniform token info:id = "UsdUVTexture"
//           asset inputs:file = @textures/bark_c.png@
//       }
//   }
//
// so a surface input is EITHER a literal value OR a `.connect` naming a sibling texture shader. Both
// forms occur in the same material, which is why every input below is read twice: once as a value,
// once as a connection.

// ShaderPrim (one Shader prim, flattened) is declared in UsdImportInternal.hpp.

// `@foo.png@` or `@@@foo@bar.png@@@` -> the path. USD's triple form exists so a path may contain a
// single '@'; anything else is returned as-is so a malformed value stays visible rather than being
// silently truncated.
std::string assetPath(std::string_view raw) {
    usize a = 0, b = raw.size();
    while (a < b && (raw[a] == ' ' || raw[a] == '\t' || raw[a] == '\r' || raw[a] == '\n')) ++a;
    while (b > a && (raw[b - 1] == ' ' || raw[b - 1] == '\t' || raw[b - 1] == '\r' || raw[b - 1] == '\n')) --b;
    const std::string_view t = raw.substr(a, b - a);
    if (t.size() >= 6 && t.compare(0, 3, "@@@") == 0 && t.compare(t.size() - 3, 3, "@@@") == 0)
        return std::string(t.substr(3, t.size() - 6));
    if (t.size() >= 2 && t.front() == '@' && t.back() == '@')
        return std::string(t.substr(1, t.size() - 2));
    return std::string(t);
}

// `</Looks/Bark/Albedo.outputs:rgb>` -> `/Looks/Bark/Albedo`. The output name is dropped HERE and
// recovered by connectionOutput() below, because most slots bind a whole texture and genuinely do
// not care which channel set was named.
std::string connectionTarget(std::string_view raw) {
    const usize lt = raw.find('<');
    const usize gt = raw.rfind('>');
    if (lt == std::string_view::npos || gt == std::string_view::npos || gt <= lt) return {};
    std::string_view t = raw.substr(lt + 1, gt - lt - 1);
    const usize dot = t.find(".outputs:");
    if (dot != std::string_view::npos) t = t.substr(0, dot);
    return std::string(t);
}

// The other half of the same text: `</…/Albedo.outputs:a>` -> 'a'. Returns 0 for `rgb`, for a
// missing output, or for anything longer than one letter -- all of which mean "not a single named
// channel", which is the only thing a caller can act on.
char connectionOutput(std::string_view raw) {
    const usize lt = raw.find('<');
    const usize gt = raw.rfind('>');
    if (lt == std::string_view::npos || gt == std::string_view::npos || gt <= lt) return 0;
    std::string_view t = raw.substr(lt + 1, gt - lt - 1);
    const usize dot = t.find(".outputs:");
    if (dot == std::string_view::npos) return 0;
    const std::string_view out = t.substr(dot + 9);
    if (out.size() != 1) return 0;                       // "rgb", "surface", …
    const char ch = out[0];
    return (ch == 'r' || ch == 'g' || ch == 'b' || ch == 'a') ? ch : 0;
}

// Reads one attribute's value text, leaving the scanner just past it. Shared by the two readers
// below so the forms a USD value can take -- bracketed, parenthesised, quoted, or bare to the end of
// the line -- are handled in exactly one place.
std::string attrValue(Scanner& s, std::string* quoted) {
    std::string raw;
    const char c0 = s.peek();
    if (c0 == '[')      raw = std::string(s.balanced('[', ']'));
    else if (c0 == '(') raw = std::string(s.balanced('(', ')'));
    else if (c0 == '"' || c0 == 39) { raw = s.quoted(); if (quoted) *quoted = raw; }
    else {
        const char* start = s.p;
        while (s.p < s.end && *s.p != '\n' && *s.p != '(') ++s.p;
        raw.assign(start, static_cast<usize>(s.p - start));
    }
    if (s.peek() == '(') s.balanced('(', ')');   // trailing metadata
    return raw;
}

// Skips the qualifier keywords that may stack in front of an attribute's type. `rel` is absent for
// the reason parseMember's own copy explains: it is a declaration keyword, not a type.
std::string_view skipQualifiers(Scanner& s, std::string_view kw) {
    std::string_view type = kw;
    while (type == "uniform" || type == "custom" || type == "varying" || type == "prepend" ||
           type == "append" || type == "add" || type == "delete")
        type = s.ident();
    return type;
}

// Reads a Shader body. Deliberately its own reader rather than a branch inside parseMember: that one
// is a mesh reader with a transform stack and a deferred-child list, none of which a Shader has.
void parseShaderBody(Scanner& s, ShaderPrim& sh) {
    while (!s.eof()) {
        if (s.peek() == '}') { ++s.p; return; }
        const std::string_view kw = s.ident();
        if (kw.empty()) { if (s.p < s.end) ++s.p; continue; }

        // A nested prim inside a Shader is not something UsdPreviewSurface has; its body is skipped
        // whole so the scan stays in sync rather than reading its attributes as this shader's.
        if (kw == "def" || kw == "over" || kw == "class") {
            if (s.peek() != '"') s.ident();
            s.quoted();
            if (s.peek() == '(') s.balanced('(', ')');
            if (s.peek() == '{') s.balanced('{', '}');
            continue;
        }

        skipQualifiers(s, kw);
        const std::string_view name = s.ident();
        if (name.empty()) continue;
        if (!s.accept('=')) { if (s.peek() == '(') s.balanced('(', ')'); continue; }

        std::string strVal;
        std::string raw = attrValue(s, &strVal);

        if (name == "info:id") { sh.id = strVal; continue; }
        if (name.rfind("inputs:", 0) != 0) continue;

        const std::string input(name.substr(7));
        const usize dot = input.find(".connect");
        if (dot != std::string::npos) {
            std::string target = connectionTarget(raw);
            if (!target.empty()) {
                std::string in = input.substr(0, dot);
                sh.connectOuts.emplace_back(in, connectionOutput(raw));
                sh.connects.emplace_back(std::move(in), std::move(target));
            }
            continue;
        }
        if (input == "file") sh.file = assetPath(raw);
        sh.values.emplace_back(input, std::move(raw));
    }
}

// Collects every Shader in a Material's body, including any nested one level down inside a Scope --
// the shape a DCC that groups its texture nodes writes.
void collectShaders(Scanner& s, const std::string& path, std::vector<ShaderPrim>& out,
                    std::string& surfaceConnect, int depth) {
    while (!s.eof()) {
        if (s.peek() == '}') { ++s.p; return; }
        const std::string_view kw = s.ident();
        if (kw.empty()) { if (s.p < s.end) ++s.p; continue; }

        if (kw == "def" || kw == "over" || kw == "class") {
            std::string_view type;
            if (s.peek() != '"') type = s.ident();
            const std::string name = s.quoted();
            if (s.peek() == '(') s.balanced('(', ')');
            if (s.peek() != '{') continue;
            const std::string childPath = path + "/" + name;
            const std::string_view body = s.balanced('{', '}');
            Scanner cs{body.data(), body.data() + body.size()};
            if (type == "Shader") {
                ShaderPrim sh;
                sh.path = childPath;
                parseShaderBody(cs, sh);
                out.push_back(std::move(sh));
            } else if (depth < 2) {
                std::string ignored;
                collectShaders(cs, childPath, out, ignored, depth + 1);
            }
            continue;
        }

        skipQualifiers(s, kw);
        const std::string_view name = s.ident();
        if (name.empty()) continue;
        if (!s.accept('=')) { if (s.peek() == '(') s.balanced('(', ')'); continue; }
        const std::string raw = attrValue(s, nullptr);
        if (name == "outputs:surface.connect" && surfaceConnect.empty())
            surfaceConnect = connectionTarget(raw);
    }
}

namespace {

// A UsdUVTexture's `@file@` as written: "./" dropped, forward slashes.
std::string cleanTexturePath(const std::string& rel) {
    std::string clean = rel;
    while (clean.rfind("./", 0) == 0) clean.erase(0, 2);
    for (char& ch : clean) if (ch == '\\') ch = '/';
    return clean;
}

// The file on disk a UsdUVTexture names, NORMALISED so one file is one string. Layers in different
// folders name a shared texture by different relative paths (RiverForest/../../../textures/X and
// RiverSapling/../../../textures/X); compared raw, Jungle Ruins loaded its shared leaf atlases once
// per layer -- 20 duplicate textures -- and sameLook() then saw different image indices and split
// identical materials into path-named copies.
std::string resolveUsdTexture(const Ctx& c, const std::string& rel) {
    const std::string clean = cleanTexturePath(rel);
    // An absolute or rooted path is taken as written; USD allows both, and rewriting one would be
    // this importer inventing an asset resolver it does not have.
    const bool rooted = (clean.size() > 1 && clean[1] == ':') || (!clean.empty() && clean[0] == '/');
    const std::string full = (rooted || c.baseDir.empty()) ? clean : c.baseDir + "/" + clean;
    return std::filesystem::path(full).lexically_normal().generic_string();
}

std::string lowerAscii(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// A translucency map on disk beside a base-colour file (ImportedMaterial::translucencyTex): the
// colour file's stem up to its colour word, then "_Translucency" or "_Transmission", matched without
// regard to case -- Intel's Jungle Ruins pairs anthurium_botany_01_BaseColor.tif with
// Anthurium_Botany_01_Translucency.png, and PX_..._Leaf_01_albedo_E.jpg with PX_..._Leaf_01_translucency.jpg.
// A decodable container is preferred over a TIFF. Empty when there is none.
std::string findTranslucencySibling(const std::string& colourPath) {
    namespace fs = std::filesystem;
    const fs::path p(colourPath);
    const std::string stem = p.stem().string();
    const std::string lower = lowerAscii(stem);

    usize cut = std::string::npos;
    usize at = 0;
    while (at < lower.size()) {
        usize end = lower.find('_', at);
        if (end == std::string::npos) end = lower.size();
        const std::string_view tok(lower.data() + at, end - at);
        for (const char* w : {"basecolor", "basecolour", "albedo", "diffuse", "diff", "color", "colour"})
            if (tok == w && at > 0) cut = at - 1;   // the separator before the word
        at = end + 1;
    }
    if (cut == std::string::npos || cut == 0) return {};
    const std::string prefix = lower.substr(0, cut);

    std::error_code ec;
    std::string best;
    int bestRank = 99;
    for (fs::directory_iterator it(p.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string s = lowerAscii(it->path().stem().string());
        if (s != prefix + "_translucency" && s != prefix + "_transmission") continue;
        const std::string ext = lowerAscii(it->path().extension().string());
        const int rank = (ext == ".png") ? 0 : (ext == ".jpg" || ext == ".jpeg") ? 1
                       : (ext == ".tga") ? 2 : (ext == ".tif" || ext == ".tiff") ? 3 : 99;
        if (rank < bestRank) { bestRank = rank; best = it->path().generic_string(); }
    }
    return best;
}

} // namespace

// Loads one texture file, or records why it could not be. Returns the index into out->images, or -1.
i32 loadUsdTexture(Ctx& c, const std::string& rel) {
    if (rel.empty()) return -1;
    const std::string clean = cleanTexturePath(rel);
    const std::string full = resolveUsdTexture(c, rel);
    // Windows paths compare without regard to case; the key is only for the comparison.
#ifdef _WIN32
    const std::string key = lowerAscii(full);
#else
    const std::string& key = full;
#endif

    for (usize i = 0; i < c.imageSources.size(); ++i)
        if (c.imageSources[i] == key) return c.out->images[i].ok ? static_cast<i32>(i) : -1;

    ImportedImage img;
    img.sourcePath = clean;
    std::string stem = clean;
    const usize slash = stem.find_last_of('/');
    if (slash != std::string::npos) stem = stem.substr(slash + 1);
    const usize dot = stem.find_last_of('.');
    img.ext = (dot == std::string::npos) ? std::string(".png") : stem.substr(dot);
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    for (char& ch : stem) {
        const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        if (!ok) ch = '_';
    }
    img.suggestedName = stem;

    const auto readAll = [](const std::string& path, std::vector<u8>& into) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) return false;
        const std::streamoff n = f.tellg();
        into.resize(usize(n > 0 ? n : 0));
        f.seekg(0);
        if (!into.empty()) f.read(reinterpret_cast<char*>(into.data()), std::streamsize(into.size()));
        return !into.empty();
    };
    img.ok = readAll(full, img.bytes);

    // A CONTAINER THE ENGINE CANNOT DECODE, ANSWERED WITH THE SIBLING BESIDE IT.
    //
    // The cook's advice for this case has always been "convert it to PNG or TGA and re-import" --
    // which could never work on its own, because re-importing reads the .tif path straight back out
    // of the .usda and never looks at the converted file. This closes that loop.
    //
    // IT MUST HAPPEN HERE, not in the cook. This is the single call site behind every slot --
    // baseColor, normal, occlusion, emissive, metalRough AND opacity -- and, decisively, it runs
    // before AverAssetC's mergeOpacityMaps, which gives up on an undecodable mask and then discards
    // it unconditionally. By the time the cook sees these images the opacity map is already gone.
    if (img.ok) {
        if (const char* container = aver::undecodableContainer(img.bytes)) {
            const std::string alt = aver::findDecodableSibling(full);
            std::vector<u8> altBytes;
            if (!alt.empty() && readAll(alt, altBytes)) {
                img.bytes = std::move(altBytes);
                const usize adot = alt.find_last_of('.');
                if (adot != std::string::npos) img.ext = alt.substr(adot);
                c.sawTextureSubstituted = true;
            } else {
                // Left as-is on purpose: the cook's existing refusal is the right backstop, and it
                // names the container in a message this function has no way to phrase as well.
                (void)container;
            }
        }
    }
    if (!img.ok) c.sawUnreadableTexture = true;

    const bool ok = img.ok;
    c.out->images.push_back(std::move(img));
    c.imageSources.push_back(key);
    return ok ? static_cast<i32>(c.out->images.size() - 1) : -1;
}

// Two materials that differ only in name are the SAME look. Used to collapse the copies a DCC
// writes per object; see the call site for why that shape is the common one.
bool sameLook(const ImportedMaterial& a, const ImportedMaterial& b) {
    const auto tex = [](const ImportedTexture& x, const ImportedTexture& y) {
        // CHANNEL IS PART OF IDENTITY: two slots on the same image reading different channels are
        // two different looks, and collapsing them would give one of them the other's mask.
        return x.imageIndex == y.imageIndex && x.texCoord == y.texCoord && x.channel == y.channel;
    };
    for (int k = 0; k < 4; ++k) if (a.baseColorFactor[k] != b.baseColorFactor[k]) return false;
    for (int k = 0; k < 3; ++k) if (a.emissiveFactor[k]  != b.emissiveFactor[k])  return false;
    return a.metallicFactor == b.metallicFactor && a.roughnessFactor == b.roughnessFactor &&
           a.normalScale == b.normalScale && a.occlusionStrength == b.occlusionStrength &&
           a.alphaCutoff == b.alphaCutoff && a.alphaMode == b.alphaMode &&
           a.doubleSided == b.doubleSided &&
           tex(a.baseColorTex, b.baseColorTex) && tex(a.metalRoughTex, b.metalRoughTex) &&
           tex(a.normalTex, b.normalTex) && tex(a.occlusionTex, b.occlusionTex) &&
           tex(a.emissiveTex, b.emissiveTex) && tex(a.opacityTex, b.opacityTex) &&
           tex(a.translucencyTex, b.translucencyTex);
}

// Parses one `def Material` body into an ImportedMaterial, appended to out->materials.
void parseMaterial(Scanner& s, Ctx& c, const std::string& path) {
    std::vector<ShaderPrim> shaders;
    std::string surfacePath;
    collectShaders(s, path, shaders, surfacePath, 0);
    buildMaterial(c, path, shaders, surfacePath);
}

// One Material's shaders -> an ImportedMaterial. Split from parseMaterial so a Material read from a
// binary layer (UsdStageImport.cpp) goes through exactly this.
void buildMaterial(Ctx& c, const std::string& path, const std::vector<ShaderPrim>& shaders,
                   const std::string& surfacePath) {
    if (shaders.empty()) return;

    const ShaderPrim* surface = nullptr;
    for (const ShaderPrim& sh : shaders)
        if (sh.path == surfacePath) { surface = &sh; break; }
    // NO outputs:surface, or one naming a prim that is not here. Falling back to the material's
    // single UsdPreviewSurface is right far more often than giving up: the connection is boilerplate
    // a hand-written .usda routinely omits, and a material with two surfaces is a multi-purpose
    // binding this importer could not choose between anyway -- so that case gives up, deliberately.
    if (!surface) {
        for (const ShaderPrim& sh : shaders)
            if (sh.id == "UsdPreviewSurface") {
                if (surface) { surface = nullptr; break; }
                surface = &sh;
            }
    }
    if (!surface || surface->id != "UsdPreviewSurface") return;

    const auto shaderAt = [&](const std::string& p) -> const ShaderPrim* {
        for (const ShaderPrim& sh : shaders) if (sh.path == p) return &sh;
        return nullptr;
    };

    ImportedMaterial m;

    // ---- the literal values ----
    const auto rgb = [&](const char* input, f32* dst) {
        const std::string* v = surface->value(input);
        if (!v) return;
        std::vector<f32> n;
        allNumbers(*v, n);
        if (n.size() >= 3) { dst[0] = n[0]; dst[1] = n[1]; dst[2] = n[2]; }
    };
    const auto scalar = [&](const char* input, f32& dst) {
        const std::string* v = surface->value(input);
        if (!v) return false;
        std::vector<f32> n;
        allNumbers(*v, n);
        if (n.empty()) return false;
        dst = n[0];
        return true;
    };

    rgb("diffuseColor", m.baseColorFactor);
    m.baseColorFactorLinear = true;      // a color3f in the stage's linear rendering space
    rgb("emissiveColor", m.emissiveFactor);
    // UsdPreviewSurface's OWN defaults are metallic 0 and roughness 0.5, not the glTF defaults of 1
    // and 1 that ImportedMaterial carries. A surface stating neither is a mid-rough dielectric here,
    // and would arrive as a fully rough metal if the struct's defaults were left standing.
    m.metallicFactor  = 0.0f;
    m.roughnessFactor = 0.5f;
    scalar("metallic", m.metallicFactor);
    scalar("roughness", m.roughnessFactor);
    scalar("opacity", m.baseColorFactor[3]);
    // opacityThreshold is USD's cutout: > 0 means alpha test, which is exactly glTF's MASK. Blend is
    // inferred from a partial opacity instead, because USD has no blend mode of its own to read.
    if (scalar("opacityThreshold", m.alphaCutoff) && m.alphaCutoff > 0.0f) m.alphaMode = "MASK";
    else if (m.baseColorFactor[3] < 1.0f) m.alphaMode = "BLEND";

    // ---- the connections ----
    const auto bindTex = [&](const char* input, ImportedTexture& slot) {
        const std::string* target = surface->connect(input);
        if (!target) return;
        const ShaderPrim* tex = shaderAt(*target);
        if (!tex || tex->id != "UsdUVTexture" || tex->file.empty()) return;
        slot.imageIndex = loadUsdTexture(c, tex->file);
        slot.channel = surface->connectOut(input);
    };
    bindTex("diffuseColor",  m.baseColorTex);
    // The translucency map the preview surface cannot name (ImportedMaterial::translucencyTex).
    if (!m.baseColorTex.empty()) {
        const ShaderPrim* tex = nullptr;
        if (const std::string* target = surface->connect("diffuseColor")) tex = shaderAt(*target);
        const std::string sib = tex ? findTranslucencySibling(resolveUsdTexture(c, tex->file)) : std::string{};
        if (!sib.empty()) {
            m.translucencyTex.imageIndex = loadUsdTexture(c, sib);
            m.translucencyTex.channel = 'r';
        }
    }
    bindTex("normal",        m.normalTex);
    bindTex("occlusion",     m.occlusionTex);
    bindTex("emissiveColor", m.emissiveTex);
    // ROUGHNESS AND METAL SHARE ONE .ocmat SLOT (glTF's ORM packing) while USD connects them
    // separately -- and, in practice, usually to the same packed file's G and B channels, so binding
    // either one gets the right texture. Roughness first, because it is the channel the engine's
    // shading is most sensitive to.
    bindTex("roughness", m.metalRoughTex);
    if (m.metalRoughTex.empty()) bindTex("metallic", m.metalRoughTex);

    // A CONNECTED INPUT TAKES ITS VALUE FROM THE TEXTURE, WHOLE. UsdPreviewSurface has no factor to
    // multiply a connection by, while .ocmat multiplies the sampled channel by the factor -- so the
    // factor behind a connection must be 1, not the unconnected default above. Leaving roughness at
    // 0.5 halved every textured surface's roughness: Jungle Ruins' terrain, authored fully rough,
    // rendered half-glossy and reflected the sky in a speckled lattice.
    //
    // The engine reads roughness from G and metallic from B of the ONE bound image. Metallic is taken
    // from it only when USD connects metallic to that same image's blue channel; a greyscale roughness
    // map would otherwise double as metalness. A metallic connection anywhere else keeps its literal
    // value (the default 0) and is reported.
    if (!m.metalRoughTex.empty()) {
        if (surface->connect("roughness")) m.roughnessFactor = 1.0f;
        if (surface->connect("metallic")) {
            ImportedTexture metal;
            bindTex("metallic", metal);
            if (!metal.empty() && metal.imageIndex == m.metalRoughTex.imageIndex && metal.channel == 'b')
                m.metallicFactor = 1.0f;
            else
                c.sawSeparateMetallic = true;
        }
    }
    // A SEPARATE opacity map. .ocmat has no slot for one -- cutout is the base colour's alpha -- so
    // this is recorded for the tool to fold in, not bound. Without it every leaf on a tree whose
    // albedo is a JPEG is a solid quad, because a JPEG cannot carry alpha at all.
    bindTex("opacity", m.opacityTex);

    // THE CUTOUT IS ALREADY IN THE BASE COLOUR, so there is nothing to fold.
    //
    // Blender's USD exporter connects `inputs:opacity` and `inputs:diffuseColor` to the SAME
    // UsdUVTexture prim, differing only by `.outputs:a` versus `.outputs:rgb` -- it is how every
    // Jungle Ruins plant is authored. Left as a separate opacity map, the downstream fold treats
    // that image as a standalone mask and copies its RED channel into alpha, so a green leaf atlas
    // becomes almost entirely transparent: worse than not folding at all, and silently so.
    //
    // Clearing the slot is the whole fix. The base colour's own alpha IS the mask the source asked
    // for, alphaMode below still records that this is a cutout material, and the fold is skipped
    // rather than performed wrongly. A genuinely separate mask -- a different image, or the same
    // one read through a different channel -- is left alone and folded as before.
    if (!m.opacityTex.empty() && m.opacityTex.imageIndex == m.baseColorTex.imageIndex &&
        m.opacityTex.channel == 'a') {
        m.opacityTex = ImportedTexture{};
    }

    // MASK FROM THE CONNECTION, NOT FROM THE LOAD. A file that drives opacity from a texture is
    // describing a cutout whether or not this machine could read that texture -- and the two are
    // different questions, which the unbound-slot rule elsewhere would otherwise conflate. With no
    // map bound the alpha stays 1, so a masked material with a missing mask renders as the opaque
    // one it would have been anyway; what is preserved is what the source said.
    if (surface->connect("opacity") && m.alphaMode == "OPAQUE") m.alphaMode = "MASK";

    // ---- one entry per DISTINCT look, however many prims declare it ----------------------------
    //
    // Blender's USD exporter writes a full copy of a Material under EVERY object that uses it, so
    // grass_B_classes.usda declares `MI_Grass_02_TwoSided` five times at five paths with identical
    // bodies. Without this, that is five .ocmat files and five names for one look -- and, worse, the
    // name-collision rule below would rename four of them to path-shaped stems, so the same plant
    // would arrive wearing four differently-named copies of the same material.
    //
    // IDENTITY IS THE PARSED CONTENT, not the prim name: two prims with the same name and different
    // bodies are genuinely two materials and still get distinct names, which is what the collision
    // rule underneath is for.
    for (usize k = 0; k < c.out->materials.size(); ++k)
        if (sameLook(c.out->materials[k], m)) {
            c.materialPaths.emplace_back(c.pathPrefix + path, k);
            return;
        }

    // ---- the name, which is what the mesh's slot will hold ----
    std::string name = path;
    const usize slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    // TWO DIFFERENT LOOKS MAY SHARE A PRIM NAME at different paths, and the slot rewrite downstream
    // matches on this string -- so a duplicate would silently give both meshes the first one. The
    // full path, sanitised, is unique by construction.
    for (const ImportedMaterial& prev : c.out->materials)
        if (prev.name == name) {
            const usize first = path.find_first_not_of('/');
            name = (first == std::string::npos) ? path : path.substr(first);
            for (char& ch : name) if (ch == '/') ch = '_';
            break;
        }
    m.name = std::move(name);

    c.out->materials.push_back(std::move(m));
    c.materialPaths.emplace_back(c.pathPrefix + path, c.out->materials.size() - 1);
}

// ---- the prim walk ----------------------------------------------------------------------------

void parsePrimBody(Scanner& s, Ctx& c, const M4& parent, const std::string& path);

// A child prim, captured but NOT yet walked. See parsePrimBody for why it has to wait.
struct DeferredChild {
    std::string      path;
    std::string_view body;
    std::string      type;   // the prim's declared type, so a Material can take its own reader
};

// Reads one attribute or nested prim inside a prim body. Returns false at the end of the body.
bool parseMember(Scanner& s, Ctx& c, MeshAttrs& attrs,
                 std::vector<std::pair<std::string, M4>>& ops,
                 std::vector<DeferredChild>& children, const std::string& path) {
    if (s.eof()) return false;
    if (s.peek() == '}') { ++s.p; return false; }

    const std::string_view kw = s.ident();
    if (kw.empty()) { if (s.p < s.end) ++s.p; return true; }

    // A nested prim.
    if (kw == "def" || kw == "over" || kw == "class") {
        if (kw == "over") c.sawReference = true;
        std::string_view type;
        if (s.peek() != '"') type = s.ident();
        const std::string name = s.quoted();
        if (s.peek() == '(') { const std::string_view meta = s.balanced('(', ')');
                               if (meta.find("references") != std::string_view::npos ||
                                   meta.find("payload") != std::string_view::npos) c.sawReference = true;
                               if (meta.find("variantSet") != std::string_view::npos) c.sawVariant = true; }
        if (type == "PointInstancer") c.sawInstancing = true;
        if (type == "Material" || type == "Shader") c.sawMaterial = true;

        // CAPTURED, NOT WALKED. A child cannot be parsed here because this prim's own xformOps may
        // not have been read yet -- USD does not require attributes to precede nested prims, and
        // walking now would hand the child its GRANDparent's matrix. So the body is set aside and
        // parsePrimBody comes back to it once the transform is actually known.
        if (s.peek() == '{')
            children.push_back(DeferredChild{path + "/" + name, s.balanced('{', '}'), std::string(type)});
        return true;
    }

    if (kw == "variantSet") { c.sawVariant = true; s.quoted(); if (s.peek() == '{') s.balanced('{', '}'); return true; }

    // An attribute. Qualifier keywords stack (`custom uniform float`, `prepend rel`), so this is a
    // loop rather than the single step it used to be.
    //
    // `rel` IS NOT A QUALIFIER, which is why it left this list. A relationship is written
    // `rel material:binding = </X>` -- no type token at all -- so consuming it like `uniform` took
    // the NAME as the type, left `name` empty, and dropped out with the `= </X>` still unread, for
    // the resync path below to grind through one character at a time. `prepend rel material:binding`
    // happened to work, which is why this went unnoticed: there the qualifier step ate `rel`.
    std::string_view type = kw;
    while (type == "uniform" || type == "custom" || type == "varying" || type == "prepend" ||
           type == "append" || type == "add" || type == "delete")
        type = s.ident();

    const std::string_view name = s.ident();
    if (name.empty()) return true;

    // `attr.timeSamples = { ... }` — animation. Skipped, and reported.
    if (name.find(".timeSamples") != std::string_view::npos) {
        c.sawTimeSamples = true;
        if (s.accept('=')) { if (s.peek() == '{') s.balanced('{', '}'); else if (s.peek() == '[') s.balanced('[', ']'); }
        return true;
    }

    std::string_view interp;
    if (!s.accept('=')) {
        // A declaration with no value, possibly with metadata. Nothing to gather.
        if (s.peek() == '(') s.balanced('(', ')');
        return true;
    }

    std::string_view region;
    std::string strVal;
    const char c0 = s.peek();
    if (c0 == '[')      region = s.balanced('[', ']');
    else if (c0 == '(') region = s.balanced('(', ')');
    else if (c0 == '"' || c0 == '\'') strVal = s.quoted();
    else {
        const char* start = s.p;
        // A bare scalar or token runs to the end of the line.
        while (s.p < s.end && *s.p != '\n' && *s.p != '(') ++s.p;
        region = std::string_view(start, static_cast<usize>(s.p - start));
    }
    // Trailing metadata, e.g. `( interpolation = "faceVarying" )`.
    if (s.peek() == '(') {
        const std::string_view meta = s.balanced('(', ')');
        if (meta.find("faceVarying") != std::string_view::npos) interp = "faceVarying";
    }

    auto is = [&](const char* n) { return name == n; };

    if (is("points")) { allNumbers(region, attrs.points); attrs.hasPoints = true; }
    else if (is("faceVertexIndices")) allInts(region, attrs.faceVertexIndices);
    else if (is("faceVertexCounts"))  allInts(region, attrs.faceVertexCounts);
    else if (is("normals")) { allNumbers(region, attrs.normals); attrs.normalsInterp = std::string(interp); }
    else if (is("primvars:st") || is("primvars:st0") || is("primvars:UVMap") || is("primvars:uv")) {
        if (attrs.uvs.empty()) { allNumbers(region, attrs.uvs); attrs.uvInterp = std::string(interp); }
    }
    else if (is("subdivisionScheme")) { attrs.subdivisionScheme = strVal; if (!strVal.empty() && strVal != "none") c.sawSubdiv = true; }
    else if (is("orientation")) attrs.orientation = strVal;
    else if (is("elementType")) attrs.subsetElementType = strVal;
    else if (is("indices")) allInts(region, attrs.subsetIndices);
    else if (is("doubleSided")) {
        // `uniform bool doubleSided = 1`, or `= true`. Both spellings occur.
        attrs.doubleSided = region.find('1') != std::string_view::npos ||
                            region.find("true") != std::string_view::npos;
    }
    else if (is("primvars:sharp_face")) {
        // `bool[] primvars:sharp_face = [0, 1, ...]` (or true/false): one flag per face.
        attrs.sharpFace.clear();
        usize k = 0;
        while (k < region.size()) {
            while (k < region.size() && !std::isalnum(static_cast<unsigned char>(region[k]))) ++k;
            const usize start = k;
            while (k < region.size() && std::isalnum(static_cast<unsigned char>(region[k]))) ++k;
            if (k == start) break;
            const std::string_view tok = region.substr(start, k - start);
            attrs.sharpFace.push_back(tok == "1" || tok == "true" ? 1 : 0);
        }
    }
    else if (name.rfind("material:binding", 0) == 0) {
        // `rel material:binding = </Looks/Bark>`, and the purpose-qualified forms
        // `material:binding:preview` / `:full`. Recorded, not resolved: the Material it names may
        // be declared later in the file. The first one wins, so an unqualified binding beats a
        // purpose-qualified one that follows it and neither silently replaces the other.
        if (attrs.materialBinding.empty()) attrs.materialBinding = connectionTarget(region);
    }
    else if (is("xformOp:translate")) {
        std::vector<f32> v; allNumbers(region, v);
        if (v.size() >= 3) ops.emplace_back("translate", translate(v[0], v[1], v[2]));
    }
    else if (is("xformOp:scale")) {
        std::vector<f32> v; allNumbers(region, v);
        if (v.size() >= 3) {
            if (std::fabs(v[0] - v[1]) > 1e-6f || std::fabs(v[0] - v[2]) > 1e-6f) c.sawNonUniformScale = true;
            ops.emplace_back("scale", scaleM(v[0], v[1], v[2]));
        }
    }
    else if (is("xformOp:rotateXYZ") || is("xformOp:rotateX") || is("xformOp:rotateY") || is("xformOp:rotateZ")) {
        std::vector<f32> v; allNumbers(region, v);
        M4 r = M4::identity();
        if (name == "xformOp:rotateX" && v.size() >= 1) r = rotateAxis(0, v[0]);
        else if (name == "xformOp:rotateY" && v.size() >= 1) r = rotateAxis(1, v[0]);
        else if (name == "xformOp:rotateZ" && v.size() >= 1) r = rotateAxis(2, v[0]);
        else if (v.size() >= 3) r = mul(mul(rotateAxis(0, v[0]), rotateAxis(1, v[1])), rotateAxis(2, v[2]));
        ops.emplace_back("rotate", r);
    }
    else if (is("xformOp:transform")) {
        std::vector<f32> v; allNumbers(region, v);
        if (v.size() >= 16) { M4 t{}; for (int i = 0; i < 16; ++i) t.m[i] = v[i]; ops.emplace_back("transform", t); }
    }
    return true;
}

void parsePrimBody(Scanner& s, Ctx& c, const M4& parent, const std::string& path) {
    MeshAttrs attrs;
    std::vector<std::pair<std::string, M4>> ops;
    std::vector<DeferredChild> children;

    while (parseMember(s, c, attrs, ops, children, path)) {}

    // Order is scale, then rotate, then translate: USD's convention when xformOpOrder is absent,
    // and the order xformOpOrder itself lists in practice. Row-vector composition puts the matrix
    // applied FIRST on the left, which is why scale leads.
    M4 node = M4::identity();
    for (const char* want : {"scale", "rotate", "transform"})
        for (const auto& o : ops) if (o.first == want) node = mul(node, o.second);
    for (const auto& o : ops) if (o.first == "translate") node = mul(node, o.second);

    const M4 world = mul(node, parent);
    // The stage importer's view of this layer: every prim's transform, so a reference to any prim in
    // it can be resolved relative to that prim (see UsdImportInternal.hpp).
    if (c.primWorlds) (*c.primWorlds)[path] = world;
    // THE SUBSETS FIRST, and this is why they cannot wait with the other children: they decide how
    // many material slots the mesh has, and buildMesh writes those slots.
    std::vector<GeomSubsetDef> subsets;
    for (const DeferredChild& ch : children) {
        if (ch.type != "GeomSubset") continue;
        Scanner ss{ch.body.data(), ch.body.data() + ch.body.size()};
        MeshAttrs sa;
        std::vector<std::pair<std::string, M4>> sops;
        std::vector<DeferredChild> skids;
        while (parseMember(ss, c, sa, sops, skids, ch.path)) {}
        // `face` is the only elementType this can act on; a "point" or "edge" subset addresses
        // something OcMeshData has no submesh concept for. familyName is not required to be
        // materialBind, but a subset with no binding at all has nothing to contribute.
        if (sa.subsetElementType != "face" || sa.materialBinding.empty()) continue;
        GeomSubsetDef g;
        const usize slash = ch.path.find_last_of('/');
        g.name    = slash == std::string::npos ? ch.path : ch.path.substr(slash + 1);
        g.binding = sa.materialBinding;
        g.faces   = std::move(sa.subsetIndices);
        if (!g.faces.empty()) subsets.push_back(std::move(g));
    }

    if (attrs.hasPoints && c.rawSink) {
        // Collected, not built: the stage importer decides the transform the vertices are baked with.
        c.rawSink->push_back(RawMesh{path, std::move(attrs), std::move(subsets), world});
    } else if (attrs.hasPoints) {
        // THE TRANSLATION COMES OUT AND BECOMES A PLACEMENT, exactly as it now does for glTF, and
        // for the same measured reason -- see UsdImportResult::meshes for what baking it cost.
        // Rotation and scale stay in the geometry: they are what the mesh looks like.
        //
        // Row-vector, row-major (see xformPoint's own indices): the translation is m[12..14], so
        // zeroing those three leaves precisely the rotation/scale basis behind. A copy and three
        // assignments, with nothing to fail on -- no decomposition, so shear cannot break it.
        M4 basis = world;
        basis.m[12] = basis.m[13] = basis.m[14] = 0.0f;
        const usize meshIndexBefore = c.out->meshes.size();

        buildMesh(c, attrs, basis, path, subsets);

        // Only when buildMesh actually produced one -- it can reject a prim (no faces, a degenerate
        // topology), and a placement pointing at a mesh that was never written is a dangling path.
        if (c.out->meshes.size() > meshIndexBefore) {
            // Through the SAME conversion a vertex takes, so the placement lands in the space and
            // unit its geometry did: axis convention, then the stage's metresPerUnit scale.
            f32 eng[3];
            const f32 t[3] = {world.m[12], world.m[13], world.m[14]};
            toEngine(c.opt->convertAxes, c.yUp, t, eng);
            UsdPlacement pl;
            pl.meshIndex = static_cast<i32>(meshIndexBefore);
            pl.position  = Vec3{eng[0] * c.unitScale, eng[1] * c.unitScale, eng[2] * c.unitScale};
            pl.name      = path;
            c.out->placements.push_back(std::move(pl));
        }
    }

    // NOW the children, with a transform that finally includes this prim's own ops.
    for (const DeferredChild& ch : children) {
        Scanner cs{ch.body.data(), ch.body.data() + ch.body.size()};
        // A Material carries no geometry and no transform, so it goes to the shader reader rather
        // than through the mesh walk that would tokenise its inputs and drop every one. A GeomSubset
        // was already consumed above, and walking it again would only re-parse its index array.
        if (ch.type == "GeomSubset") continue;
        if (ch.type == "Material") parseMaterial(cs, c, ch.path);
        else                       parsePrimBody(cs, c, world, ch.path);
        if (ch.type == "Camera" && c.cameraPaths) c.cameraPaths->push_back(ch.path);
        if ((ch.type == "DomeLight" || ch.type == "DistantLight") && c.lights)
            c.lights->push_back(LightPrim{ch.path, ch.type, std::string(ch.body)});
    }
}

// Fills each mesh's material slot from the prim path its `rel material:binding` named. Runs after
// the whole stage is walked, which is the point: USD does not require a Material to be declared
// before the mesh that binds it, and a Looks scope written after the geometry is an ordinary shape
// for an exported stage.
void resolveBindings(Ctx& c) {
    for (usize i = 0; i < c.out->meshes.size() && i < c.meshBinding.size(); ++i)
    for (usize sl = 0; sl < c.meshBinding[i].size() && sl < c.out->meshes[i].materialSlots.size(); ++sl) {
        const std::string& want = c.meshBinding[i][sl];
        if (want.empty()) continue;
        bool found = false;
        for (const auto& mp : c.materialPaths)
            if (mp.first == want) {
                c.out->meshes[i].materialSlots[sl] = c.out->materials[mp.second].name;
                // DOUBLE-SIDEDNESS CROSSES HERE, because USD authors it on the geometry while .ocmat
                // carries it on the material -- Jungle Ruins' foliage cards are `doubleSided = 1`
                // meshes bound to a Material that says nothing about culling, so without this every
                // leaf is invisible from behind.
                //
                // ORed across every mesh that binds the material rather than split into two
                // materials: if two binders disagree, the merged answer draws backfaces that one of
                // them did not ask for, which is a shading difference, while the other direction
                // makes geometry vanish.
                if (i < c.slotDoubleSided.size() && sl < c.slotDoubleSided[i].size() && c.slotDoubleSided[i][sl])
                    c.out->materials[mp.second].doubleSided = true;
                found = true;
                break;
            }
        // A binding this file cannot satisfy -- almost always a Material behind a reference this
        // importer does not compose. The slot stays EMPTY rather than being pointed at a name that
        // is not there, which would read downstream as a missing material file.
        if (!found) c.sawUnresolvedBinding = true;
    }
}

void reportUnsupported(Ctx& c) {
    UsdImportResult& out = *c.out;
    if (c.sawReference)   out.unsupported.push_back("references, payloads and `over` opinions were not composed; only geometry written directly in this file was read");
    if (c.sawVariant)     out.unsupported.push_back("variant sets were not resolved; no variant selection was applied");
    if (c.sawInstancing)  out.unsupported.push_back("PointInstancer prims were skipped; instanced geometry did not import");
    if (c.sawMaterial && out.materials.empty())
        out.unsupported.push_back("Material prims were present but none used UsdPreviewSurface, which "
                                  "is the only shading model this importer reads; meshes import with an empty material slot");
    if (c.sawUnresolvedBinding)
        out.unsupported.push_back("a mesh bound a Material this file does not declare -- almost always one "
                                  "behind a reference; that mesh imports with an empty material slot");
    if (c.sawPartialSubsetCover)
        out.unsupported.push_back("a mesh's GeomSubsets did not cover every face; the remainder kept "
                                  "the mesh-level material binding rather than being dropped");
    if (c.sawUnreadableTexture)
        out.unsupported.push_back("a texture a material named could not be read from disk; that slot is "
                                  "unbound rather than pointing at a file that is not there");
    if (c.sawTextureSubstituted)
        out.unsupported.push_back("a texture this file named is in a container the engine cannot decode "
                                  "(TIFF, EXR and the like); a same-named sibling beside it on disk was "
                                  "used instead -- check it is the image the source intended");
    if (c.rebuiltNormalMeshes)
        out.unsupported.push_back(std::to_string(c.rebuiltNormalMeshes) + " mesh(es) carried FLAT per-face normals although "
                                  "primvars:sharp_face marked their faces smooth; their smooth normals were rebuilt as "
                                  "Blender shades them (UsdImportOptions::honourSharpFace)");
    usize translucent = 0;
    for (const ImportedMaterial& m : out.materials) if (!m.translucencyTex.empty()) ++translucent;
    if (translucent)
        out.unsupported.push_back(std::to_string(translucent) + " material(s) took a translucency map found on disk beside "
                                  "their base colour (<stem>_Translucency.*), which UsdPreviewSurface cannot name; it "
                                  "becomes the material's subsurface weight");
    if (c.sawSeparateMetallic)
        out.unsupported.push_back("a material connected metallic to a texture other than the blue channel of its "
                                  "roughness image; .ocmat reads both from one image, so that metallic map was not "
                                  "applied and the literal metallic value (default 0) was used");
    if (c.sawTimeSamples) out.unsupported.push_back("time-sampled attributes were ignored; the default (non-animated) value was used");
    if (c.sawSubdiv)      out.unsupported.push_back("subdivisionScheme was not 'none'; the control cage was imported as-is, NOT subdivided, so the model will look faceted");
    if (c.sawNonUniformScale) out.unsupported.push_back("a non-uniform xformOp:scale was applied to normals without an inverse-transpose, so shading on that prim is approximate");
}

// The `@path@` asset paths inside a `[ ... ]` list, in order (subLayers).
std::vector<std::string> assetPathList(std::string_view list) {
    std::vector<std::string> out;
    usize p = 0;
    while ((p = list.find('@', p)) != std::string_view::npos) {
        const bool triple = list.compare(p, 3, "@@@") == 0;
        const usize open = p + (triple ? 3 : 1);
        const usize close = list.find(triple ? "@@@" : "@", open);
        if (close == std::string_view::npos) break;
        out.emplace_back(list.substr(open, close - open));
        p = close + (triple ? 3 : 1);
    }
    return out;
}

void parseUsdaText(const std::string& buf, Ctx& c, UsdaHeader& hdr, bool applyUnits) {
    const char* text = buf.c_str();
    Scanner s{text, text + buf.size()};

    // The `#usda 1.0` line, then optional stage metadata.
    while (s.p < s.end && *s.p != '\n') ++s.p;

    hdr = UsdaHeader{};
    // USD's own default is Y-up when the stage says nothing.
    hdr.upAxis = "Y";
    if (s.peek() == '(') {
        const std::string_view meta = s.balanced('(', ')');
        const usize up = meta.find("upAxis");
        if (up != std::string_view::npos) {
            const std::string_view rest = meta.substr(up);
            if (rest.find("\"Z\"") != std::string_view::npos || rest.find("'Z'") != std::string_view::npos) {
                hdr.yUp = false;
                hdr.upAxis = "Z";
            }
        }
        const usize mp = meta.find("metersPerUnit");
        if (mp != std::string_view::npos) {
            Scanner ms{meta.data() + mp, meta.data() + meta.size()};
            ms.ident();
            if (ms.accept('=')) { bool ok = false; const f32 v = ms.number(&ok); if (ok && v > 0.0f) hdr.metersPerUnit = v; }
        }
        const usize sl = meta.find("subLayers");
        if (sl != std::string_view::npos) {
            Scanner ss{meta.data() + sl, meta.data() + meta.size()};
            ss.ident();
            if (ss.accept('=')) hdr.subLayers = assetPathList(ss.balanced('[', ']'));
        }
        const usize dp = meta.find("defaultPrim");
        if (dp != std::string_view::npos) {
            Scanner ds{meta.data() + dp, meta.data() + meta.size()};
            ds.ident();
            if (ds.accept('=')) hdr.defaultPrim = ds.quoted();
        }
    }

    // The stage's units folded with the caller's. opt.scale defaults to 100 (metres to centimetres),
    // so a stage already in centimetres (metersPerUnit = 0.01) ends up multiplied by exactly 1. NOT
    // applied for a layer the stage importer is reading on behalf of a larger stage: USD interprets
    // every layer in the ROOT stage's units, whatever the layer itself declares.
    if (applyUnits) {
        c.yUp = hdr.yUp;
        c.unitScale = hdr.metersPerUnit * c.opt->scale;
    }

    const M4 root = M4::identity();
    while (!s.eof()) {
        const std::string_view kw = s.ident();
        if (kw.empty()) { if (s.p < s.end) ++s.p; continue; }
        if (kw == "def" || kw == "over" || kw == "class") {
            if (kw == "over") c.sawReference = true;
            std::string_view type;
            if (s.peek() != '"') type = s.ident();
            const std::string name = s.quoted();
            if (s.peek() == '(') {
                const std::string_view meta = s.balanced('(', ')');
                if (meta.find("references") != std::string_view::npos ||
                    meta.find("payload") != std::string_view::npos) c.sawReference = true;
                if (meta.find("variantSet") != std::string_view::npos) c.sawVariant = true;
            }
            if (type == "PointInstancer") c.sawInstancing = true;
            if (type == "Material" || type == "Shader") c.sawMaterial = true;
            if (s.peek() == '{') {
                const char* bodyStart = s.p + 1;
                if (type == "Material") { ++s.p; parseMaterial(s, c, "/" + name); }
                else                    { ++s.p; parsePrimBody(s, c, root, "/" + name); }
                if (type == "Camera" && c.cameraPaths) c.cameraPaths->push_back("/" + name);
                if ((type == "DomeLight" || type == "DistantLight") && c.lights)
                    c.lights->push_back(LightPrim{"/" + name, std::string(type), std::string(bodyStart, s.p)});
            }
            continue;
        }
        // Anything else at stage scope: skip its value so the walk stays in sync.
        if (s.accept('=')) {
            const char ch = s.peek();
            if (ch == '[') s.balanced('[', ']');
            else if (ch == '(') s.balanced('(', ')');
            else if (ch == '{') s.balanced('{', '}');
            else while (s.p < s.end && *s.p != '\n') ++s.p;
        }
    }
}

} // namespace usd_detail

using namespace usd_detail;

UsdEncoding usdSniff(const u8* bytes, usize size) {
    if (!bytes || size < 4) return UsdEncoding::Unknown;
    if (size >= 8 && std::memcmp(bytes, "PXR-USDC", 8) == 0) return UsdEncoding::Usdc;
    if (bytes[0] == 'P' && bytes[1] == 'K' && (bytes[2] == 3 || bytes[2] == 5 || bytes[2] == 7))
        return UsdEncoding::Usdz;
    // `#usda 1.0`, possibly behind a UTF-8 BOM.
    usize o = (size >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) ? 3 : 0;
    if (size - o >= 5 && std::memcmp(bytes + o, "#usda", 5) == 0) return UsdEncoding::Usda;
    return UsdEncoding::Unknown;
}

const char* usdEncodingName(UsdEncoding e) {
    switch (e) {
        case UsdEncoding::Usda: return "USDA (ASCII)";
        case UsdEncoding::Usdc: return "USDC (binary crate)";
        case UsdEncoding::Usdz: return "USDZ (zip archive)";
        default:                return "unknown";
    }
}

bool importUsdFromMemory(const u8* bytes, usize size, const std::string& baseDir,
                         UsdImportResult& out, const UsdImportOptions& opt, std::string* why) {
    out = UsdImportResult{};
    out.encoding = usdSniff(bytes, size);

    // THE REFUSALS ARE BY NAME AND SAY HOW TO FIX IT. An importer that returned true with zero
    // meshes here would be indistinguishable from a file that really had none.
    if (out.encoding == UsdEncoding::Usdc)
        return fail(why, "USD: this is the USDC binary crate encoding, which this single-layer text reader "
                         "does not read. Import it with importUsdStage (AverAssetC convert does), or convert "
                         "it with `usdcat -o out.usda in.usdc`.");
    if (out.encoding == UsdEncoding::Usdz)
        return fail(why, "USD: this is a USDZ archive, which this importer does not unpack. Extract "
                         "it (it is a zip) and import the .usda inside, converting with `usdcat` if "
                         "that inner file is itself USDC.");
    if (out.encoding != UsdEncoding::Usda)
        return fail(why, "USD: not a USD file -- no '#usda' header, no 'PXR-USDC' magic, no zip magic");

    // COPIED SO IT IS NUL-TERMINATED, which is not fussiness. This parser reaches for strtof/strtol
    // on interior pointers, and those read until a non-numeric byte with no idea where the buffer
    // ends -- on a caller's unterminated span that is a read past the end. std::string guarantees
    // the terminator, so one allocation buys the whole parser its bound.
    const std::string buf(reinterpret_cast<const char*>(bytes), size);

    Ctx c;
    c.out = &out;
    c.opt = &opt;
    c.baseDir = baseDir;
    while (!c.baseDir.empty() && (c.baseDir.back() == '/' || c.baseDir.back() == '\\')) c.baseDir.pop_back();

    UsdaHeader hdr;
    parseUsdaText(buf, c, hdr, /*applyUnits=*/true);
    out.sourceUpAxis = hdr.upAxis;
    out.sourceMetersPerUnit = hdr.metersPerUnit;
    // Only the stage importer composes these; a lone layer that names them has geometry elsewhere.
    if (!hdr.subLayers.empty()) c.sawReference = true;

    // THE JOIN, and it has to be here rather than in buildMesh: a `rel material:binding` names a
    // prim path, and USD does not require that prim to appear first.
    resolveBindings(c);
    reportUnsupported(c);

    if (out.meshes.empty())
        return fail(why, "USD: the stage parsed, but it contains no UsdGeomMesh with points -- "
                         "geometry behind references, payloads or variants is not composed by this importer");
    return true;
}

bool importUsd(const std::string& path, UsdImportResult& out,
               const UsdImportOptions& opt, std::string* why) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return fail(why, "USD: cannot open " + path);
    const std::streamoff n = f.tellg();
    if (n <= 0) return fail(why, "USD: empty file " + path);
    std::vector<u8> bytes(static_cast<usize>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()), n);
    if (!f) return fail(why, "USD: short read on " + path);
    const usize slash = path.find_last_of("/\\");
    const std::string baseDir = slash == std::string::npos ? std::string() : path.substr(0, slash);
    return importUsdFromMemory(bytes.data(), bytes.size(), baseDir, out, opt, why);
}

} // namespace aver::fmt
