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
    bool hasPoints = false;
};

struct Ctx {
    UsdImportResult* out;
    const UsdImportOptions* opt;
    f32 unitScale = 100.0f;         // stage metersPerUnit folded with opt->scale
    bool yUp = true;
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
}

// ---- the prim walk ----------------------------------------------------------------------------

void parsePrimBody(Scanner& s, Ctx& c, const M4& parent, const std::string& path);

// A child prim, captured but NOT yet walked. See parsePrimBody for why it has to wait.
struct DeferredChild {
    std::string      path;
    std::string_view body;
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
            children.push_back(DeferredChild{path + "/" + name, s.balanced('{', '}')});
        return true;
    }

    if (kw == "variantSet") { c.sawVariant = true; s.quoted(); if (s.peek() == '{') s.balanced('{', '}'); return true; }

    // An attribute. Possible prefixes first.
    std::string_view type = kw;
    if (type == "uniform" || type == "custom" || type == "varying" || type == "prepend" ||
        type == "append" || type == "add" || type == "delete" || type == "rel")
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
        parsePrimBody(cs, c, world, ch.path);
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

bool importUsdFromMemory(const u8* bytes, usize size, UsdImportResult& out,
                         const UsdImportOptions& opt, std::string* why) {
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
            if (s.peek() == '{') { ++s.p; parsePrimBody(s, c, root, "/" + name); }
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

    if (c.sawReference)   out.unsupported.push_back("references, payloads and `over` opinions were not composed; only geometry written directly in this file was read");
    if (c.sawVariant)     out.unsupported.push_back("variant sets were not resolved; no variant selection was applied");
    if (c.sawInstancing)  out.unsupported.push_back("PointInstancer prims were skipped; instanced geometry did not import");
    if (c.sawMaterial)    out.unsupported.push_back("Material and Shader prims were skipped; meshes import with an empty material slot");
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
    return importUsdFromMemory(bytes.data(), bytes.size(), out, opt, why);
}

} // namespace aver::fmt
