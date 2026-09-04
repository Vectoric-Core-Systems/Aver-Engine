#include "aver/formats/UsdImport.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string_view>

namespace aver::fmt {
namespace {

bool fail(std::string* why, std::string m) { if (why) *why = std::move(m); return false; }

// ---- 4x4, row-major, row-vector (v * M) -------------------------------------------------------
//
// THE SAME CONVENTION USD USES, which is the happy part of this importer: GfMatrix4d is row-major
// and pre-multiplies row vectors, exactly like this engine. So a matrix4d in a .usda can be copied
// straight across without a transpose, and composition order reads the same in both. Getting this
// backwards transposes every rotation, which looks like an axis-convention bug and is not one.
struct M4 {
    f32 m[16];
    static M4 identity() {
        M4 r{};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }
};

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

struct MeshAttrs {
    std::vector<f32> points, normals, uvs;
    std::vector<i32> faceVertexIndices, faceVertexCounts;
    std::string subdivisionScheme;
    std::string orientation;
    std::string normalsInterp;      // the `interpolation` metadata on `normals`, when stated
    std::string uvInterp;
    // The prim path a `rel material:binding` named, unresolved. Resolved AFTER the walk, because
    // USD does not require a Material to be declared before the mesh that binds it.
    std::string materialBinding;
    // USD puts doubleSided on the GEOMETRY; glTF and .ocmat put it on the material. Carried here and
    // folded into the bound material by resolveBindings.
    bool doubleSided = false;
    bool hasPoints = false;
};

struct Ctx {
    UsdImportResult* out;
    const UsdImportOptions* opt;
    f32 unitScale = 100.0f;         // stage metersPerUnit folded with opt->scale
    bool yUp = true;

    std::string baseDir;            // resolves a UsdUVTexture's `@path@`; empty refuses them

    // Parallel to out->meshes: the prim path each mesh's `rel material:binding` named, or empty.
    // A second array rather than a field on OcMeshData because the binding is a USD concept that
    // does not survive into the engine's mesh -- only the resolved slot name does.
    std::vector<std::string> meshBinding;
    std::vector<bool>        meshDoubleSided;   // parallel to out->meshes
    // Every Material prim path, and the out->materials entry it resolved to. NOT parallel to
    // out->materials: several paths may share one entry -- see parseMaterial's dedup.
    std::vector<std::pair<std::string, usize>> materialPaths;
    // Parallel to out->images: the RESOLVED path each was read from, so two materials naming the
    // same texture share one image rather than writing its bytes twice under two names.
    std::vector<std::string> imageSources;
    bool sawUnreadableTexture = false;
    bool sawUnresolvedBinding = false;
    bool sawNonUniformScale = false;
    bool sawTimeSamples = false;
    bool sawReference = false;
    bool sawVariant = false;
    bool sawInstancing = false;
    bool sawMaterial = false;
    bool sawSubdiv = false;
};

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

// Turns a gathered UsdGeomMesh into an OcMeshData in engine space.
void buildMesh(Ctx& c, const MeshAttrs& a, const M4& world, const std::string& primPath) {
    if (!a.hasPoints || a.points.size() < 9) return;
    if (a.faceVertexIndices.empty() || a.faceVertexCounts.empty()) return;

    const usize pointCount = a.points.size() / 3;

    // USD stores points once and indexes them per face corner, but normals and UVs are commonly
    // FACE-VARYING — one per corner, not one per point. Those two layouts need different handling,
    // and guessing wrong scrambles the UVs. Corner-indexed data forces a de-index, so this always
    // builds one engine vertex per CORNER and lets the shared-point case pay a little extra memory
    // rather than risk the mismatch. Meshes are welded downstream if that ever matters.
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

    usize corner = 0;
    for (const i32 rawCount : a.faceVertexCounts) {
        const usize n = rawCount > 0 ? static_cast<usize>(rawCount) : 0;
        if (n < 3 || corner + n > cornerCount) { corner += n; continue; }

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
        corner += n;
    }

    if (m.positions.empty() || m.indices.empty()) return;
    if (!haveNormals && c.opt->generateMissingNormals) generateNormals(m);

    // EMPTY FOR NOW, always. The binding names a prim path that may not have been read yet, so the
    // slot is filled in after the walk -- see resolveBindings.
    m.materialSlots.push_back(std::string());
    OcMeshSubmesh sm;
    sm.name         = primPath;
    sm.materialSlot = 0;
    sm.indexStart   = 0;
    sm.indexCount   = static_cast<u32>(m.indices.size());
    sm.baseVertex   = 0;
    sm.vertexCount  = m.vertexCount();
    m.submeshes.push_back(std::move(sm));
    computeBounds(m);

    c.out->meshes.push_back(std::move(m));
    c.out->meshNames.push_back(primPath);
    c.meshBinding.push_back(a.materialBinding);
    c.meshDoubleSided.push_back(a.doubleSided);
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

// One Shader prim, flattened. `values` and `connects` are small enough that a linear scan beats a
// map: a UsdPreviewSurface has at most a dozen inputs.
struct ShaderPrim {
    std::string path;
    std::string id;                                              // info:id
    std::vector<std::pair<std::string, std::string>> values;     // input name -> raw text
    std::vector<std::pair<std::string, std::string>> connects;   // input name -> source prim path
    std::string file;                                            // inputs:file, @@ stripped

    const std::string* value(const char* n) const {
        for (const auto& v : values) if (v.first == n) return &v.second;
        return nullptr;
    }
    const std::string* connect(const char* n) const {
        for (const auto& v : connects) if (v.first == n) return &v.second;
        return nullptr;
    }
};

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

// `</Looks/Bark/Albedo.outputs:rgb>` -> `/Looks/Bark/Albedo`. The output name is dropped: this
// importer binds a whole texture to a slot, so which channel set the connection named tells it
// nothing it can act on.
std::string connectionTarget(std::string_view raw) {
    const usize lt = raw.find('<');
    const usize gt = raw.rfind('>');
    if (lt == std::string_view::npos || gt == std::string_view::npos || gt <= lt) return {};
    std::string_view t = raw.substr(lt + 1, gt - lt - 1);
    const usize dot = t.find(".outputs:");
    if (dot != std::string_view::npos) t = t.substr(0, dot);
    return std::string(t);
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
            if (!target.empty()) sh.connects.emplace_back(input.substr(0, dot), std::move(target));
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

// Loads one texture file, or records why it could not be. Returns the index into out->images, or -1.
i32 loadUsdTexture(Ctx& c, const std::string& rel) {
    if (rel.empty()) return -1;
    std::string clean = rel;
    while (clean.rfind("./", 0) == 0) clean.erase(0, 2);
    for (char& ch : clean) if (ch == '\\') ch = '/';

    // An absolute or rooted path is taken as written; USD allows both, and rewriting one would be
    // this importer inventing an asset resolver it does not have.
    const bool rooted = (clean.size() > 1 && clean[1] == ':') || (!clean.empty() && clean[0] == '/');
    const std::string full = (rooted || c.baseDir.empty()) ? clean : c.baseDir + "/" + clean;

    for (usize i = 0; i < c.imageSources.size(); ++i)
        if (c.imageSources[i] == full) return c.out->images[i].ok ? static_cast<i32>(i) : -1;

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

    std::ifstream f(full, std::ios::binary | std::ios::ate);
    if (f) {
        const std::streamoff n = f.tellg();
        img.bytes.resize(usize(n > 0 ? n : 0));
        f.seekg(0);
        if (!img.bytes.empty())
            f.read(reinterpret_cast<char*>(img.bytes.data()), std::streamsize(img.bytes.size()));
        img.ok = !img.bytes.empty();
    }
    if (!img.ok) c.sawUnreadableTexture = true;

    const bool ok = img.ok;
    c.out->images.push_back(std::move(img));
    c.imageSources.push_back(full);
    return ok ? static_cast<i32>(c.out->images.size() - 1) : -1;
}

// Two materials that differ only in name are the SAME look. Used to collapse the copies a DCC
// writes per object; see the call site for why that shape is the common one.
bool sameLook(const ImportedMaterial& a, const ImportedMaterial& b) {
    const auto tex = [](const ImportedTexture& x, const ImportedTexture& y) {
        return x.imageIndex == y.imageIndex && x.texCoord == y.texCoord;
    };
    for (int k = 0; k < 4; ++k) if (a.baseColorFactor[k] != b.baseColorFactor[k]) return false;
    for (int k = 0; k < 3; ++k) if (a.emissiveFactor[k]  != b.emissiveFactor[k])  return false;
    return a.metallicFactor == b.metallicFactor && a.roughnessFactor == b.roughnessFactor &&
           a.normalScale == b.normalScale && a.occlusionStrength == b.occlusionStrength &&
           a.alphaCutoff == b.alphaCutoff && a.alphaMode == b.alphaMode &&
           a.doubleSided == b.doubleSided &&
           tex(a.baseColorTex, b.baseColorTex) && tex(a.metalRoughTex, b.metalRoughTex) &&
           tex(a.normalTex, b.normalTex) && tex(a.occlusionTex, b.occlusionTex) &&
           tex(a.emissiveTex, b.emissiveTex);
}

// Parses one `def Material` body into an ImportedMaterial, appended to out->materials.
void parseMaterial(Scanner& s, Ctx& c, const std::string& path) {
    std::vector<ShaderPrim> shaders;
    std::string surfacePath;
    collectShaders(s, path, shaders, surfacePath, 0);
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
    };
    bindTex("diffuseColor",  m.baseColorTex);
    bindTex("normal",        m.normalTex);
    bindTex("occlusion",     m.occlusionTex);
    bindTex("emissiveColor", m.emissiveTex);
    // ROUGHNESS AND METAL SHARE ONE .ocmat SLOT (glTF's ORM packing) while USD connects them
    // separately -- and, in practice, usually to the same packed file's G and B channels, so binding
    // either one gets the right texture. Roughness first, because it is the channel the engine's
    // shading is most sensitive to.
    bindTex("roughness", m.metalRoughTex);
    if (m.metalRoughTex.empty()) bindTex("metallic", m.metalRoughTex);

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
            c.materialPaths.emplace_back(path, k);
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
    c.materialPaths.emplace_back(path, c.out->materials.size() - 1);
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
    else if (is("doubleSided")) {
        // `uniform bool doubleSided = 1`, or `= true`. Both spellings occur.
        attrs.doubleSided = region.find('1') != std::string_view::npos ||
                            region.find("true") != std::string_view::npos;
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
    if (attrs.hasPoints) buildMesh(c, attrs, world, path);

    // NOW the children, with a transform that finally includes this prim's own ops.
    for (const DeferredChild& ch : children) {
        Scanner cs{ch.body.data(), ch.body.data() + ch.body.size()};
        // A Material carries no geometry and no transform, so it goes to the shader reader rather
        // than through the mesh walk that would tokenise its inputs and drop every one.
        if (ch.type == "Material") parseMaterial(cs, c, ch.path);
        else                       parsePrimBody(cs, c, world, ch.path);
    }
}

} // namespace

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

// Fills each mesh's material slot from the prim path its `rel material:binding` named. Runs after
// the whole stage is walked, which is the point: USD does not require a Material to be declared
// before the mesh that binds it, and a Looks scope written after the geometry is an ordinary shape
// for an exported stage.
void resolveBindings(Ctx& c) {
    for (usize i = 0; i < c.out->meshes.size() && i < c.meshBinding.size(); ++i) {
        const std::string& want = c.meshBinding[i];
        if (want.empty() || c.out->meshes[i].materialSlots.empty()) continue;
        bool found = false;
        for (const auto& mp : c.materialPaths)
            if (mp.first == want) {
                c.out->meshes[i].materialSlots[0] = c.out->materials[mp.second].name;
                // DOUBLE-SIDEDNESS CROSSES HERE, because USD authors it on the geometry while .ocmat
                // carries it on the material -- Jungle Ruins' foliage cards are `doubleSided = 1`
                // meshes bound to a Material that says nothing about culling, so without this every
                // leaf is invisible from behind.
                //
                // ORed across every mesh that binds the material rather than split into two
                // materials: if two binders disagree, the merged answer draws backfaces that one of
                // them did not ask for, which is a shading difference, while the other direction
                // makes geometry vanish.
                if (i < c.meshDoubleSided.size() && c.meshDoubleSided[i])
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

bool importUsdFromMemory(const u8* bytes, usize size, const std::string& baseDir,
                         UsdImportResult& out, const UsdImportOptions& opt, std::string* why) {
    out = UsdImportResult{};
    out.encoding = usdSniff(bytes, size);

    // THE REFUSALS ARE BY NAME AND SAY HOW TO FIX IT. An importer that returned true with zero
    // meshes here would be indistinguishable from a file that really had none.
    if (out.encoding == UsdEncoding::Usdc)
        return fail(why, "USD: this is the USDC binary crate encoding, which this importer does not "
                         "read. Convert it with `usdcat -o out.usda in.usdc`, or export as .usda.");
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
    const char* text = buf.c_str();
    Scanner s{text, text + buf.size()};

    // The `#usda 1.0` line, then optional stage metadata.
    while (s.p < s.end && *s.p != '\n') ++s.p;

    Ctx c;
    c.out = &out;
    c.opt = &opt;
    c.baseDir = baseDir;
    while (!c.baseDir.empty() && (c.baseDir.back() == '/' || c.baseDir.back() == '\\')) c.baseDir.pop_back();

    f32 metersPerUnit = 1.0f;
    if (s.peek() == '(') {
        const std::string_view meta = s.balanced('(', ')');
        const usize up = meta.find("upAxis");
        if (up != std::string_view::npos) {
            const std::string_view rest = meta.substr(up);
            if (rest.find("\"Z\"") != std::string_view::npos || rest.find("'Z'") != std::string_view::npos) {
                c.yUp = false;
                out.sourceUpAxis = "Z";
            } else {
                out.sourceUpAxis = "Y";
            }
        } else {
            // USD's own default is Y-up when the stage says nothing.
            out.sourceUpAxis = "Y";
        }
        const usize mp = meta.find("metersPerUnit");
        if (mp != std::string_view::npos) {
            Scanner ms{meta.data() + mp, meta.data() + meta.size()};
            ms.ident();
            if (ms.accept('=')) { bool ok = false; const f32 v = ms.number(&ok); if (ok && v > 0.0f) metersPerUnit = v; }
        }
    } else {
        out.sourceUpAxis = "Y";
    }
    out.sourceMetersPerUnit = metersPerUnit;

    // The stage's units folded with the caller's. opt.scale defaults to 100 (metres to centimetres),
    // so a stage already in centimetres (metersPerUnit = 0.01) ends up multiplied by exactly 1.
    c.unitScale = metersPerUnit * opt.scale;

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
                if (type == "Material") { ++s.p; parseMaterial(s, c, "/" + name); }
                else                    { ++s.p; parsePrimBody(s, c, root, "/" + name); }
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

    // THE JOIN, and it has to be here rather than in buildMesh: a `rel material:binding` names a
    // prim path, and USD does not require that prim to appear first.
    resolveBindings(c);

    if (c.sawReference)   out.unsupported.push_back("references, payloads and `over` opinions were not composed; only geometry written directly in this file was read");
    if (c.sawVariant)     out.unsupported.push_back("variant sets were not resolved; no variant selection was applied");
    if (c.sawInstancing)  out.unsupported.push_back("PointInstancer prims were skipped; instanced geometry did not import");
    if (c.sawMaterial && out.materials.empty())
        out.unsupported.push_back("Material prims were present but none used UsdPreviewSurface, which "
                                  "is the only shading model this importer reads; meshes import with an empty material slot");
    if (c.sawUnresolvedBinding)
        out.unsupported.push_back("a mesh bound a Material this file does not declare -- almost always one "
                                  "behind a reference; that mesh imports with an empty material slot");
    if (c.sawUnreadableTexture)
        out.unsupported.push_back("a texture a material named could not be read from disk; that slot is "
                                  "unbound rather than pointing at a file that is not there");
    if (c.sawTimeSamples) out.unsupported.push_back("time-sampled attributes were ignored; the default (non-animated) value was used");
    if (c.sawSubdiv)      out.unsupported.push_back("subdivisionScheme was not 'none'; the control cage was imported as-is, NOT subdivided, so the model will look faceted");
    if (c.sawNonUniformScale) out.unsupported.push_back("a non-uniform xformOp:scale was applied to normals without an inverse-transpose, so shading on that prim is approximate");

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
