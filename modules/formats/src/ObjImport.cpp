#include "aver/formats/ObjImport.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string_view>
#include <unordered_map>

namespace aver::fmt {
namespace {

bool fail(std::string* why, std::string m) { if (why) *why = std::move(m); return false; }

// ---- scanning -------------------------------------------------------------------------------
//
// Hand-rolled rather than istringstream. An OBJ from a scanner or a photogrammetry tool runs to tens
// of millions of vertices, and the stream operators spend most of their time in locale machinery
// this parser has no use for. This also lets `f` parse a v/vt/vn triplet without allocating.

inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

inline void skipSpace(const char*& p, const char* end) {
    while (p < end && isSpace(*p)) ++p;
}

// A token up to the next whitespace. Returns empty at end of line.
inline std::string_view token(const char*& p, const char* end) {
    skipSpace(p, end);
    const char* s = p;
    while (p < end && !isSpace(*p) && *p != '\n') ++p;
    return std::string_view(s, static_cast<usize>(p - s));
}

// strtof on a bounded range. OBJ numbers are plain ASCII floats; anything else is a malformed file
// and parses as 0, which is what `ok` reports.
inline f32 parseFloat(std::string_view t, bool* ok = nullptr) {
    if (t.empty()) { if (ok) *ok = false; return 0.0f; }
    char buf[64];
    const usize n = t.size() < sizeof(buf) - 1 ? t.size() : sizeof(buf) - 1;
    std::memcpy(buf, t.data(), n);
    buf[n] = '\0';
    char* endp = nullptr;
    const f32 v = std::strtof(buf, &endp);
    if (ok) *ok = endp != buf;
    return v;
}

// ---- the vertex key -------------------------------------------------------------------------
//
// OBJ indexes position, uv and normal SEPARATELY per face corner, so one position can appear with
// several different normals -- that is exactly how it represents a hard edge. The engine's vertex
// buffer has one index stream, so every distinct (v, vt, vn) triple becomes its own vertex. Merging
// them by position instead would smooth every hard edge in the file, which is the classic OBJ
// import bug and it looks like broken shading rather than like a broken importer.
struct Corner {
    i32 v = 0, vt = 0, vn = 0;     // already resolved to 0-based; -1 means absent
    bool operator==(const Corner& o) const { return v == o.v && vt == o.vt && vn == o.vn; }
};

struct CornerHash {
    usize operator()(const Corner& c) const noexcept {
        // splitmix-flavoured mix. Three ints into one hash, cheap and well spread; a naive
        // xor-shift here collides badly because v and vt are usually near-equal small integers.
        u64 h = static_cast<u64>(static_cast<u32>(c.v)) * 0x9E3779B97F4A7C15ull;
        h ^= static_cast<u64>(static_cast<u32>(c.vt)) * 0xC2B2AE3D27D4EB4Full;
        h ^= (h >> 29);
        h ^= static_cast<u64>(static_cast<u32>(c.vn)) * 0x165667B19E3779F9ull;
        h ^= (h >> 32);
        return static_cast<usize>(h);
    }
};

// Resolves an OBJ index to 0-based. OBJ is 1-based, and a NEGATIVE index counts back from the end of
// what has been declared SO FAR -- not from the end of the file. That "so far" is why this takes the
// current count rather than the final one, and why it must be called during the walk.
inline i32 resolveIndex(i64 raw, usize declaredSoFar) {
    if (raw > 0) return static_cast<i32>(raw - 1);
    if (raw < 0) {
        const i64 r = static_cast<i64>(declaredSoFar) + raw;   // raw is negative
        return r >= 0 ? static_cast<i32>(r) : -1;
    }
    return -1;                                                  // 0 is not a legal OBJ index
}

// Parses "v", "v/vt", "v//vn" or "v/vt/vn" out of one whitespace-delimited token.
bool parseCorner(std::string_view t, usize nv, usize nvt, usize nvn, Corner& out) {
    if (t.empty()) return false;
    i64 idx[3] = {0, 0, 0};
    int field = 0;
    usize i = 0;
    while (i < t.size() && field < 3) {
        if (t[i] == '/') { ++field; ++i; continue; }
        bool neg = false;
        if (t[i] == '-') { neg = true; ++i; }
        else if (t[i] == '+') { ++i; }
        i64 acc = 0;
        bool any = false;
        while (i < t.size() && t[i] >= '0' && t[i] <= '9') { acc = acc * 10 + (t[i] - '0'); ++i; any = true; }
        if (any) idx[field] = neg ? -acc : acc;
        // No digits between two slashes means the field is absent (the `v//vn` form), which is
        // legal and leaves idx[field] at 0 -- resolveIndex turns that into -1.
        if (i < t.size() && t[i] != '/') break;                 // garbage; stop at the first oddity
    }
    out.v  = resolveIndex(idx[0], nv);
    out.vt = resolveIndex(idx[1], nvt);
    out.vn = resolveIndex(idx[2], nvn);
    return out.v >= 0;
}

// ---- .mtl -----------------------------------------------------------------------------------

// A map_* line may carry options before the filename: `map_Kd -bm 0.2 -o 1 1 1 tex.png`. The
// filename is the LAST token, and it may itself contain spaces, which is why this takes everything
// after the final recognised option rather than just the last token.
std::string mapPath(const char* p, const char* end) {
    std::vector<std::string_view> toks;
    while (p < end) {
        const std::string_view t = token(p, end);
        if (t.empty()) break;
        toks.push_back(t);
    }
    // Walk from the front, skipping `-opt` and the numeric arguments that follow it. Everything from
    // the first token that is neither an option nor an option argument is the path.
    usize i = 0;
    while (i < toks.size() && !toks[i].empty() && toks[i][0] == '-') {
        const std::string_view o = toks[i];
        ++i;
        // -o, -s and -t take three floats; -mm takes two; the rest take one. Over-consuming here
        // would eat the filename, so unknown options take exactly one argument.
        usize args = 1;
        if (o == "-o" || o == "-s" || o == "-t") args = 3;
        else if (o == "-mm") args = 2;
        else if (o == "-clamp" || o == "-blendu" || o == "-blendv" || o == "-cc") args = 1;
        while (args-- > 0 && i < toks.size()) {
            // Stop early if the "argument" is plainly a filename rather than a number.
            bool numeric = false;
            parseFloat(toks[i], &numeric);
            if (!numeric) break;
            ++i;
        }
    }
    std::string outPath;
    for (; i < toks.size(); ++i) {
        if (!outPath.empty()) outPath += ' ';                    // rejoin a filename with spaces
        outPath.append(toks[i].data(), toks[i].size());
    }
    return outPath;
}

bool parseMtlText(const char* text, usize size, std::vector<ObjMaterial>& out) {
    const char* p = text;
    const char* end = text + size;
    ObjMaterial* cur = nullptr;

    auto rgb = [](const char*& q, const char* e, f32 dst[3]) {
        for (int k = 0; k < 3; ++k) {
            const std::string_view t = token(q, e);
            if (t.empty()) { if (k == 1) { dst[1] = dst[0]; dst[2] = dst[0]; } return; }
            dst[k] = parseFloat(t);
        }
    };

    while (p < end) {
        const char* lineEnd = static_cast<const char*>(std::memchr(p, '\n', static_cast<usize>(end - p)));
        if (!lineEnd) lineEnd = end;
        const char* q = p;
        p = lineEnd + 1;

        skipSpace(q, lineEnd);
        if (q >= lineEnd || *q == '#') continue;
        const std::string_view kw = token(q, lineEnd);

        if (kw == "newmtl") {
            out.push_back(ObjMaterial{});
            cur = &out.back();
            const std::string_view n = token(q, lineEnd);
            cur->name.assign(n.data(), n.size());
            continue;
        }
        if (!cur) continue;                                     // a property before any newmtl

        if      (kw == "Kd") rgb(q, lineEnd, cur->baseColor);
        else if (kw == "Ks") rgb(q, lineEnd, cur->specular);
        else if (kw == "Ke") rgb(q, lineEnd, cur->emissive);
        else if (kw == "Ns") cur->shininess = parseFloat(token(q, lineEnd));
        else if (kw == "Ni") cur->ior       = parseFloat(token(q, lineEnd));
        else if (kw == "d")  cur->opacity   = parseFloat(token(q, lineEnd));
        else if (kw == "Tr") cur->opacity   = 1.0f - parseFloat(token(q, lineEnd));   // the inverse of d
        else if (kw == "Pr") { cur->roughness = parseFloat(token(q, lineEnd)); cur->hasPbr = true; }
        else if (kw == "Pm") { cur->metallic  = parseFloat(token(q, lineEnd)); cur->hasPbr = true; }
        else if (kw == "map_Kd")   cur->mapBaseColor = mapPath(q, lineEnd);
        else if (kw == "map_Ke")   cur->mapEmissive  = mapPath(q, lineEnd);
        else if (kw == "map_d")    cur->mapOpacity   = mapPath(q, lineEnd);
        else if (kw == "map_Ka")   cur->mapAmbientOcclusion = mapPath(q, lineEnd);
        else if (kw == "map_Pr") { cur->mapRoughness = mapPath(q, lineEnd); cur->hasPbr = true; }
        else if (kw == "map_Pm") { cur->mapMetallic  = mapPath(q, lineEnd); cur->hasPbr = true; }
        else if (kw == "map_Ns")  { if (cur->mapRoughness.empty()) cur->mapRoughness = mapPath(q, lineEnd); }
        // Normal maps have three spellings in the wild and no winner. `norm` is the PBR extension,
        // `bump`/`map_Bump` is the 1995 keyword that everyone reuses for normal maps anyway.
        else if (kw == "norm" || kw == "bump" || kw == "map_Bump" || kw == "map_bump")
            cur->mapNormal = mapPath(q, lineEnd);
    }
    return true;
}

// ---- geometry accumulation --------------------------------------------------------------------

// One output mesh under construction: its own vertex array, its own corner-dedup map, and the
// usemtl runs that become submeshes.
struct MeshBuild {
    std::string name;
    OcMeshData  data;
    std::unordered_map<Corner, u32, CornerHash> dedup;
    std::string  curMtl;
    u32          curSubmeshStart = 0;
    bool         sawUv = false, sawNormal = false;
};

void flushSubmesh(MeshBuild& mb) {
    const u32 end = static_cast<u32>(mb.data.indices.size());
    if (end == mb.curSubmeshStart) return;                      // an empty run writes nothing

    u32 slot = 0;
    const auto it = std::find(mb.data.materialSlots.begin(), mb.data.materialSlots.end(), mb.curMtl);
    if (it != mb.data.materialSlots.end()) {
        slot = static_cast<u32>(it - mb.data.materialSlots.begin());
    } else {
        slot = static_cast<u32>(mb.data.materialSlots.size());
        mb.data.materialSlots.push_back(mb.curMtl);
    }

    // A material used in two separate runs gets two submeshes with the SAME slot rather than one
    // merged submesh. That keeps index ranges contiguous, which is what a draw call needs, and the
    // renderer binds per slot so it costs one extra draw and never a wrong material.
    OcMeshSubmesh sm;
    sm.name         = mb.curMtl;
    sm.materialSlot = slot;
    sm.indexStart   = mb.curSubmeshStart;
    sm.indexCount   = end - mb.curSubmeshStart;
    sm.baseVertex   = 0;
    sm.vertexCount  = mb.data.vertexCount();
    mb.data.submeshes.push_back(std::move(sm));
    mb.curSubmeshStart = end;
}

// Flat normals, computed AFTER the winding fix so they face outward in the engine's handedness.
// Accumulated per vertex and normalised, so a shared corner gets the average of its faces -- which
// is right precisely because a hard edge already split the corner into separate vertices above.
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
        else { n[0] = 0.0f; n[1] = 0.0f; n[2] = 1.0f; }         // +Z up: the engine's "no idea" normal
    }
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

} // namespace

bool importObjFromMemory(const char* text, usize size, const std::string& baseDir,
                         ObjImportResult& out, const ObjImportOptions& opt, std::string* why) {
    if (!text || size == 0) return fail(why, "OBJ: empty input");

    out = ObjImportResult{};

    // File-global vertex streams. OBJ indices address these across the WHOLE file, not per object,
    // which is why they live out here while each output mesh keeps its own de-indexed arrays.
    std::vector<f32> gp, gt, gn;                                // positions x3, uvs x2, normals x3
    gp.reserve(size / 24);                                      // ~one vertex per 24 bytes of text

    std::vector<MeshBuild> builds;
    MeshBuild* mb = nullptr;
    bool sawNonTriangle = false, sawSmoothingGroup = false, sawFreeform = false;

    auto beginMesh = [&](std::string name) -> MeshBuild& {
        builds.push_back(MeshBuild{});
        builds.back().name = std::move(name);
        return builds.back();
    };

    const char* p = text;
    const char* end = text + size;

    while (p < end) {
        const char* lineEnd = static_cast<const char*>(std::memchr(p, '\n', static_cast<usize>(end - p)));
        if (!lineEnd) lineEnd = end;
        const char* q = p;
        p = lineEnd + 1;

        skipSpace(q, lineEnd);
        if (q >= lineEnd || *q == '#') continue;
        const std::string_view kw = token(q, lineEnd);

        if (kw == "v") {
            const f32 x = parseFloat(token(q, lineEnd));
            const f32 y = parseFloat(token(q, lineEnd));
            const f32 z = parseFloat(token(q, lineEnd));
            // The basis change, matching GltfImport.cpp:173 exactly: engine.x = -src.z,
            // engine.y = src.x, engine.z = src.y. Scale applies after, so it is a pure unit change.
            if (opt.convertAxes) {
                gp.push_back(-z * opt.scale); gp.push_back(x * opt.scale); gp.push_back(y * opt.scale);
            } else {
                gp.push_back(x * opt.scale); gp.push_back(y * opt.scale); gp.push_back(z * opt.scale);
            }
            // A `v` line may carry r g b after xyz. The engine's MeshVertex has no colour channel,
            // so it is dropped -- reported once rather than per vertex.
            continue;
        }
        if (kw == "vt") {
            const f32 u = parseFloat(token(q, lineEnd));
            const f32 v = parseFloat(token(q, lineEnd));
            // V FLIPS. OBJ's origin is bottom-left; the engine samples top-left, the same convention
            // the glTF path and every .octex assume. Without this every imported model is mirrored
            // vertically in UV space, which reads as "the artist's texture is upside down".
            gt.push_back(u);
            gt.push_back(1.0f - v);
            continue;
        }
        if (kw == "vn") {
            const f32 x = parseFloat(token(q, lineEnd));
            const f32 y = parseFloat(token(q, lineEnd));
            const f32 z = parseFloat(token(q, lineEnd));
            if (opt.convertAxes) { gn.push_back(-z); gn.push_back(x); gn.push_back(y); }
            else                 { gn.push_back(x);  gn.push_back(y); gn.push_back(z); }
            continue;
        }
        if (kw == "o" || (kw == "g" && opt.splitByObject && builds.empty())) {
            const std::string_view n = token(q, lineEnd);
            if (opt.splitByObject) {
                if (mb) flushSubmesh(*mb);
                mb = &beginMesh(std::string(n));
            } else if (!mb) {
                mb = &beginMesh(std::string(n));
            }
            continue;
        }
        if (kw == "g") continue;                                 // a group inside an object: ignored
        if (kw == "s") { sawSmoothingGroup = true; continue; }   // smoothing groups: see `unsupported`
        if (kw == "mtllib") {
            while (true) {
                const std::string_view n = token(q, lineEnd);
                if (n.empty()) break;
                out.materialLibs.emplace_back(n);
            }
            continue;
        }
        if (kw == "usemtl") {
            if (!mb) mb = &beginMesh(std::string());
            flushSubmesh(*mb);
            const std::string_view n = token(q, lineEnd);
            mb->curMtl.assign(n.data(), n.size());
            continue;
        }
        // Freeform geometry: curves, surfaces and their control statements. This importer is a
        // polygon importer and says so rather than silently producing a mesh with holes in it.
        if (kw == "curv" || kw == "curv2" || kw == "surf" || kw == "cstype" || kw == "vp") {
            sawFreeform = true;
            continue;
        }
        if (kw != "f") continue;

        if (!mb) mb = &beginMesh(std::string());

        // ---- a face ----
        Corner corners[64];
        int n = 0;
        while (n < 64) {
            const std::string_view t = token(q, lineEnd);
            if (t.empty()) break;
            Corner c;
            if (!parseCorner(t, gp.size() / 3, gt.size() / 2, gn.size() / 3, c)) break;
            corners[n++] = c;
        }
        if (n < 3) continue;                                     // a degenerate or malformed face
        if (n > 3) sawNonTriangle = true;

        // De-index each corner into this mesh's vertex array.
        u32 vidx[64];
        for (int i = 0; i < n; ++i) {
            const Corner& c = corners[i];
            const auto it = mb->dedup.find(c);
            if (it != mb->dedup.end()) { vidx[i] = it->second; continue; }

            const u32 nv = static_cast<u32>(mb->data.positions.size() / 3);
            const usize pi = static_cast<usize>(c.v) * 3;
            if (pi + 2 >= gp.size()) { vidx[i] = nv ? nv - 1 : 0; continue; }   // out-of-range index
            mb->data.positions.push_back(gp[pi]);
            mb->data.positions.push_back(gp[pi + 1]);
            mb->data.positions.push_back(gp[pi + 2]);

            if (c.vt >= 0 && static_cast<usize>(c.vt) * 2 + 1 < gt.size()) {
                mb->data.uvs.push_back(gt[static_cast<usize>(c.vt) * 2]);
                mb->data.uvs.push_back(gt[static_cast<usize>(c.vt) * 2 + 1]);
                mb->sawUv = true;
            } else {
                mb->data.uvs.push_back(0.0f);
                mb->data.uvs.push_back(0.0f);
            }

            if (c.vn >= 0 && static_cast<usize>(c.vn) * 3 + 2 < gn.size()) {
                mb->data.normals.push_back(gn[static_cast<usize>(c.vn) * 3]);
                mb->data.normals.push_back(gn[static_cast<usize>(c.vn) * 3 + 1]);
                mb->data.normals.push_back(gn[static_cast<usize>(c.vn) * 3 + 2]);
                mb->sawNormal = true;
            } else {
                mb->data.normals.push_back(0.0f);
                mb->data.normals.push_back(0.0f);
                mb->data.normals.push_back(0.0f);
            }

            mb->dedup.emplace(c, nv);
            vidx[i] = nv;
        }

        // FAN TRIANGULATION from the first corner. Correct for any convex polygon and for the
        // overwhelmingly common quad; a CONCAVE n-gon will produce triangles outside the polygon.
        // That is a real limitation and it is reported in `unsupported` rather than hidden, because
        // the alternative -- ear clipping in 3D -- needs a reliable face plane that an arbitrary
        // OBJ does not always give, and would fail differently rather than never.
        for (int i = 1; i + 1 < n; ++i) {
            if (opt.convertAxes) {
                // Winding reverses with the handedness, exactly as GltfImport.cpp:297 does.
                mb->data.indices.push_back(vidx[0]);
                mb->data.indices.push_back(vidx[i + 1]);
                mb->data.indices.push_back(vidx[i]);
            } else {
                mb->data.indices.push_back(vidx[0]);
                mb->data.indices.push_back(vidx[i]);
                mb->data.indices.push_back(vidx[i + 1]);
            }
        }
    }

    if (mb) flushSubmesh(*mb);
    if (builds.empty()) return fail(why, "OBJ: no geometry -- the file declared no faces");

    for (MeshBuild& b : builds) {
        if (b.data.positions.empty()) continue;
        if (!b.sawNormal && opt.generateMissingNormals) generateNormals(b.data);
        if (b.data.submeshes.empty()) {
            // A file with faces but no usemtl still needs one slot, or the renderer has nothing to
            // bind and draws the whole mesh with slot 0 by accident rather than by decision.
            b.curMtl.clear();
            b.curSubmeshStart = 0;
            flushSubmesh(b);
        }
        computeBounds(b.data);
        out.meshes.push_back(std::move(b.data));
        out.meshNames.push_back(std::move(b.name));
    }
    if (out.meshes.empty()) return fail(why, "OBJ: every object was empty");

    if (opt.loadMaterialLibs && !baseDir.empty()) {
        for (const std::string& lib : out.materialLibs) {
            std::string mtlWhy;
            std::vector<ObjMaterial> mats;
            if (importMtl(baseDir + "/" + lib, mats, &mtlWhy))
                out.materials.insert(out.materials.end(), mats.begin(), mats.end());
            else
                out.unsupported.push_back("mtllib '" + lib + "' could not be read: " + mtlWhy);
        }
    } else if (opt.loadMaterialLibs && !out.materialLibs.empty()) {
        out.unsupported.push_back("mtllib ignored: no base directory was given, and searching the "
                                  "working directory would make the import depend on where it ran");
    }

    if (sawNonTriangle)
        out.unsupported.push_back("polygons with more than three sides were fan-triangulated; a "
                                  "CONCAVE polygon will have produced triangles outside its outline");
    if (sawSmoothingGroup)
        out.unsupported.push_back("smoothing groups (s) were ignored; normals come from vn, or are "
                                  "generated per split corner when the file has none");
    if (sawFreeform)
        out.unsupported.push_back("freeform geometry (curv/surf/cstype/vp) was skipped; this "
                                  "importer reads polygons only");

    return true;
}

bool importObj(const std::string& path, ObjImportResult& out,
               const ObjImportOptions& opt, std::string* why) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return fail(why, "OBJ: cannot open " + path);
    const std::streamoff n = f.tellg();
    if (n <= 0) return fail(why, "OBJ: empty file " + path);
    std::vector<char> bytes(static_cast<usize>(n));
    f.seekg(0);
    f.read(bytes.data(), n);
    if (!f) return fail(why, "OBJ: short read on " + path);

    const usize slash = path.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? std::string(".") : path.substr(0, slash);
    return importObjFromMemory(bytes.data(), bytes.size(), dir, out, opt, why);
}

bool importMtl(const std::string& path, std::vector<ObjMaterial>& out, std::string* why) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return fail(why, "cannot open " + path);
    const std::streamoff n = f.tellg();
    if (n < 0) return fail(why, "cannot size " + path);
    std::vector<char> bytes(static_cast<usize>(n));
    f.seekg(0);
    if (n > 0) f.read(bytes.data(), n);
    if (n > 0 && !f) return fail(why, "short read on " + path);
    return parseMtlText(bytes.data(), bytes.size(), out);
}

} // namespace aver::fmt
