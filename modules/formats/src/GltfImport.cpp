#include "aver/formats/GltfImport.hpp"

#include "aver/formats/Json.hpp"

#include <cmath>
#include <cstring>
#include <fstream>

namespace aver::fmt {
namespace {

bool fail(std::string* why, std::string m) { if (why) *why = std::move(m); return false; }

// glTF component types (5.24 accessor.componentType).
constexpr i64 kByte = 5120, kUByte = 5121, kShort = 5122, kUShort = 5123, kUInt = 5125, kFloat = 5126;

u32 componentBytes(i64 t) {
    switch (t) {
        case kByte: case kUByte:   return 1;
        case kShort: case kUShort: return 2;
        case kUInt: case kFloat:   return 4;
        default:                   return 0;
    }
}

u32 typeComponents(std::string_view t) {
    if (t == "SCALAR") return 1;
    if (t == "VEC2")   return 2;
    if (t == "VEC3")   return 3;
    if (t == "VEC4")   return 4;
    if (t == "MAT2")   return 4;
    if (t == "MAT3")   return 9;
    if (t == "MAT4")   return 16;
    return 0;
}

// ---- base64, for data: URIs ----
// glTF embeds whole buffers this way, so this is not an edge case: a single-file .gltf export from
// Blender puts the entire mesh here.
bool base64Decode(std::string_view in, std::vector<u8>& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    out.reserve(in.size() / 4 * 3);
    u32 acc = 0; int bits = 0;
    for (const char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        const int v = val(c);
        if (v < 0) return false;
        acc = (acc << 6) | u32(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(u8((acc >> bits) & 0xFF)); }
    }
    return true;
}

struct Buffers {
    std::vector<std::vector<u8>> data;
};

// One decoded accessor, always widened to f32 (or u32 for indices) so callers do not each repeat
// the component-type switch.
struct AccessorF {
    std::vector<f32> v;
    u32 components = 0;
    u32 count = 0;
};

class Gltf {
public:
    Gltf(const JsonValue& doc, Buffers& bufs, const GltfImportOptions& opt, GltfImportResult& out)
        : d_(doc), b_(bufs), o_(opt), r_(out) {}

    bool run(std::string* why);

private:
    const JsonValue& d_;
    Buffers& b_;
    const GltfImportOptions& o_;
    GltfImportResult& r_;

    void note(const std::string& what) {
        for (const std::string& s : r_.unsupported) if (s == what) return;   // once each, not per use
        r_.unsupported.push_back(what);
    }

    // bufferView -> the bytes it names, with its stride.
    bool viewBytes(i64 viewIdx, const u8*& base, usize& len, u32& stride, std::string* why) const {
        const JsonValue& views = d_["bufferViews"];
        if (viewIdx < 0 || usize(viewIdx) >= views.size()) return fail(why, "glTF: bufferView index out of range");
        const JsonValue& bv = views[usize(viewIdx)];
        const i64 bufIdx = bv["buffer"].asInt(-1);
        if (bufIdx < 0 || usize(bufIdx) >= b_.data.size()) return fail(why, "glTF: buffer index out of range");
        const std::vector<u8>& buf = b_.data[usize(bufIdx)];
        const u64 off = u64(bv["byteOffset"].asInt(0));
        const u64 n   = u64(bv["byteLength"].asInt(0));
        if (off + n > buf.size()) return fail(why, "glTF: bufferView runs past the end of its buffer");
        base = buf.data() + off;
        len = usize(n);
        stride = u32(bv["byteStride"].asInt(0));
        return true;
    }

    bool readAccessor(i64 idx, AccessorF& out, std::string* why) const {
        const JsonValue& accs = d_["accessors"];
        if (idx < 0 || usize(idx) >= accs.size()) return fail(why, "glTF: accessor index out of range");
        const JsonValue& a = accs[usize(idx)];
        if (a.has("sparse")) return fail(why, "glTF: sparse accessors are not supported");

        const i64 ct = a["componentType"].asInt(0);
        const u32 cb = componentBytes(ct);
        const u32 nc = typeComponents(a["type"].asString());
        const u32 count = u32(a["count"].asInt(0));
        if (cb == 0) return fail(why, "glTF: unknown componentType");
        if (nc == 0) return fail(why, "glTF: unknown accessor type");
        out.components = nc;
        out.count = count;
        out.v.assign(usize(count) * nc, 0.0f);
        if (count == 0) return true;

        // An accessor with no bufferView reads as zeros -- that is glTF's own rule, and it is how a
        // sparse-only accessor is spelled. Handled rather than treated as an error.
        if (!a.has("bufferView")) return true;

        const u8* base = nullptr; usize len = 0; u32 stride = 0;
        if (!viewBytes(a["bufferView"].asInt(-1), base, len, stride, why)) return false;
        const u64 accOff = u64(a["byteOffset"].asInt(0));
        const u32 elemSize = cb * nc;
        if (stride == 0) stride = elemSize;
        if (accOff + u64(stride) * (count - 1) + elemSize > len)
            return fail(why, "glTF: accessor runs past the end of its bufferView");

        const bool normalized = a["normalized"].asBool(false);
        const u8* p = base + accOff;
        for (u32 i = 0; i < count; ++i, p += stride) {
            for (u32 c = 0; c < nc; ++c) {
                const u8* e = p + c * cb;
                f32 val = 0.0f;
                switch (ct) {
                    case kFloat:  { f32 f; std::memcpy(&f, e, 4); val = f; break; }
                    case kUByte:  { val = f32(*e); if (normalized) val /= 255.0f; break; }
                    case kByte:   { i8 s; std::memcpy(&s, e, 1); val = f32(s);
                                    if (normalized) val = std::fmax(val / 127.0f, -1.0f); break; }
                    case kUShort: { u16 s; std::memcpy(&s, e, 2); val = f32(s); if (normalized) val /= 65535.0f; break; }
                    case kShort:  { i16 s; std::memcpy(&s, e, 2); val = f32(s);
                                    if (normalized) val = std::fmax(val / 32767.0f, -1.0f); break; }
                    case kUInt:   { u32 s; std::memcpy(&s, e, 4); val = f32(s); break; }
                    default: break;
                }
                out.v[usize(i) * nc + c] = val;
            }
        }
        return true;
    }

    // glTF (right-handed, +Y up, -Z forward, metres) -> engine (left-handed, +Z up, +X forward,
    // +Y right, centimetres). See the header: the determinant is -1, so winding is reversed too.
    Vec3 toEngine(f32 x, f32 y, f32 z, bool isDirection) const {
        Vec3 v = o_.convertAxes ? Vec3{-z, x, y} : Vec3{x, y, z};
        if (!isDirection) { v.x *= o_.scale; v.y *= o_.scale; v.z *= o_.scale; }
        return v;
    }

    bool importPrimitive(const JsonValue& prim, const f32 node[16], OcMeshData& m, std::string* why);
    bool importMesh(const JsonValue& mesh, const f32 node[16], OcMeshData& m, std::string* why);
};

// Row-vector transform of a point / direction by a 4x4 row-major matrix.
void xform(const f32 m[16], f32 x, f32 y, f32 z, bool point, f32 out[3]) {
    const f32 w = point ? 1.0f : 0.0f;
    out[0] = x*m[0] + y*m[4] + z*m[8]  + w*m[12];
    out[1] = x*m[1] + y*m[5] + z*m[9]  + w*m[13];
    out[2] = x*m[2] + y*m[6] + z*m[10] + w*m[14];
}

bool Gltf::importPrimitive(const JsonValue& prim, const f32 node[16], OcMeshData& m, std::string* why) {
    // Only triangles. glTF's other modes exist and none of them is a static mesh this renderer can
    // draw, so they are named and skipped rather than silently producing an empty submesh.
    const i64 mode = prim["mode"].asInt(4);
    if (mode != 4) { note("primitive mode " + std::to_string(mode) + " (only triangles are imported)"); return true; }
    if (prim.has("targets")) note("morph targets");
    if (prim["extensions"].has("KHR_draco_mesh_compression")) return fail(why, "glTF: Draco-compressed meshes are not supported");

    const JsonValue& attrs = prim["attributes"];
    if (!attrs.has("POSITION")) return fail(why, "glTF: a primitive has no POSITION attribute");

    AccessorF pos, nrm, uv;
    if (!readAccessor(attrs["POSITION"].asInt(-1), pos, why)) return false;
    if (pos.components != 3) return fail(why, "glTF: POSITION is not VEC3");
    const bool hasNrm = attrs.has("NORMAL") && readAccessor(attrs["NORMAL"].asInt(-1), nrm, why) && nrm.components == 3;
    const bool hasUv  = attrs.has("TEXCOORD_0") && readAccessor(attrs["TEXCOORD_0"].asInt(-1), uv, why) && uv.components == 2;
    if (attrs.has("JOINTS_0")) note("skinning (JOINTS/WEIGHTS)");
    if (attrs.has("COLOR_0"))  note("vertex colour");

    const u32 baseVertex = m.vertexCount();
    for (u32 i = 0; i < pos.count; ++i) {
        f32 t[3];
        xform(node, pos.v[usize(i)*3+0], pos.v[usize(i)*3+1], pos.v[usize(i)*3+2], true, t);
        const Vec3 p = toEngine(t[0], t[1], t[2], false);
        m.positions.insert(m.positions.end(), {p.x, p.y, p.z});

        if (hasNrm && i < nrm.count) {
            f32 n[3];
            xform(node, nrm.v[usize(i)*3+0], nrm.v[usize(i)*3+1], nrm.v[usize(i)*3+2], false, n);
            Vec3 e = toEngine(n[0], n[1], n[2], true);
            const f32 len = std::sqrt(e.x*e.x + e.y*e.y + e.z*e.z);
            if (len > 1e-12f) { e.x /= len; e.y /= len; e.z /= len; } else { e = Vec3{0,0,1}; }
            m.normals.insert(m.normals.end(), {e.x, e.y, e.z});
        } else {
            m.normals.insert(m.normals.end(), {0.0f, 0.0f, 1.0f});   // replaced below if asked
        }

        if (hasUv && i < uv.count) m.uvs.insert(m.uvs.end(), {uv.v[usize(i)*2+0], uv.v[usize(i)*2+1]});
        else                        m.uvs.insert(m.uvs.end(), {0.0f, 0.0f});
    }

    // Indices. glTF allows a primitive with none, meaning "draw the vertices in order".
    const u32 first = static_cast<u32>(m.indices.size());
    if (prim.has("indices")) {
        AccessorF idx;
        if (!readAccessor(prim["indices"].asInt(-1), idx, why)) return false;
        if (idx.components != 1) return fail(why, "glTF: indices accessor is not SCALAR");
        if (idx.count % 3 != 0) return fail(why, "glTF: index count is not a multiple of three");
        for (u32 i = 0; i < idx.count; i += 3) {
            const u32 a = baseVertex + u32(idx.v[i + 0]);
            const u32 b = baseVertex + u32(idx.v[i + 1]);
            const u32 c = baseVertex + u32(idx.v[i + 2]);
            // WINDING REVERSED with the handedness. Without this the mesh is inside out and looks
            // perfect until something enables backface culling.
            if (o_.convertAxes) { m.indices.push_back(a); m.indices.push_back(c); m.indices.push_back(b); }
            else                { m.indices.push_back(a); m.indices.push_back(b); m.indices.push_back(c); }
        }
    } else {
        if (pos.count % 3 != 0) return fail(why, "glTF: unindexed primitive vertex count is not a multiple of three");
        for (u32 i = 0; i < pos.count; i += 3) {
            const u32 a = baseVertex + i, b = baseVertex + i + 1, c = baseVertex + i + 2;
            if (o_.convertAxes) { m.indices.push_back(a); m.indices.push_back(c); m.indices.push_back(b); }
            else                { m.indices.push_back(a); m.indices.push_back(b); m.indices.push_back(c); }
        }
    }

    // Flat normals where the source had none, computed AFTER the winding fix so they face outward.
    if (!hasNrm && o_.generateMissingNormals) {
        for (usize i = first; i + 2 < m.indices.size(); i += 3) {
            const u32 ia = m.indices[i], ib = m.indices[i+1], ic = m.indices[i+2];
            const f32* A = &m.positions[usize(ia)*3];
            const f32* B = &m.positions[usize(ib)*3];
            const f32* C = &m.positions[usize(ic)*3];
            const f32 e1[3] = {B[0]-A[0], B[1]-A[1], B[2]-A[2]};
            const f32 e2[3] = {C[0]-A[0], C[1]-A[1], C[2]-A[2]};
            f32 n[3] = {e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0]};
            const f32 len = std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
            if (len > 1e-12f) { n[0]/=len; n[1]/=len; n[2]/=len; } else { n[0]=0; n[1]=0; n[2]=1; }
            for (const u32 v : {ia, ib, ic}) {
                m.normals[usize(v)*3+0] = n[0]; m.normals[usize(v)*3+1] = n[1]; m.normals[usize(v)*3+2] = n[2];
            }
        }
    }

    // A submesh per primitive, which is what carries the material split.
    const i64 mat = prim["material"].asInt(-1);
    u32 slot = 0;
    if (mat >= 0) {
        const JsonValue& mats = d_["materials"];
        std::string name = "Material_" + std::to_string(mat);
        if (usize(mat) < mats.size() && mats[usize(mat)].has("name"))
            name = std::string(mats[usize(mat)]["name"].asString());
        for (usize i = 0; i < m.materialSlots.size(); ++i) if (m.materialSlots[i] == name) { slot = u32(i); goto found; }
        slot = static_cast<u32>(m.materialSlots.size());
        m.materialSlots.push_back(name);
        found:;
    } else if (m.materialSlots.empty()) {
        m.materialSlots.push_back("M_Default");
    }

    OcMeshSubmesh sm;
    sm.name = "prim" + std::to_string(m.submeshes.size());
    sm.materialSlot = slot;
    sm.indexStart = first;
    sm.indexCount = static_cast<u32>(m.indices.size() - first);
    sm.baseVertex = 0;
    sm.vertexCount = m.vertexCount();
    m.submeshes.push_back(std::move(sm));
    return true;
}

bool Gltf::importMesh(const JsonValue& mesh, const f32 node[16], OcMeshData& m, std::string* why) {
    const JsonValue& prims = mesh["primitives"];
    if (prims.size() == 0) return fail(why, "glTF: a mesh has no primitives");
    for (usize i = 0; i < prims.size(); ++i)
        if (!importPrimitive(prims[i], node, m, why)) return false;
    return true;
}

// Node local transform: either a 16-float matrix, or TRS. Composed into the parent's.
void nodeLocal(const JsonValue& n, f32 out[16]) {
    static const f32 kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::memcpy(out, kIdentity, sizeof(kIdentity));
    if (n.has("matrix")) {
        const JsonValue& m = n["matrix"];
        if (m.size() == 16) for (int i = 0; i < 16; ++i) out[i] = m[usize(i)].asFloat();
        return;                                    // glTF forbids matrix together with TRS
    }
    f32 t[3] = {0,0,0}, r[4] = {0,0,0,1}, s[3] = {1,1,1};
    if (n.has("translation")) for (int i = 0; i < 3; ++i) t[i] = n["translation"][usize(i)].asFloat();
    if (n.has("rotation"))    for (int i = 0; i < 4; ++i) r[i] = n["rotation"][usize(i)].asFloat();
    if (n.has("scale"))       for (int i = 0; i < 3; ++i) s[i] = n["scale"][usize(i)].asFloat(1.0f);

    const f32 x=r[0], y=r[1], z=r[2], w=r[3];
    // Row-major, row-vector: the basis lives in rows 0-2, translation in row 3.
    out[0] = (1-2*(y*y+z*z))*s[0]; out[1] = (2*(x*y+z*w))*s[0];   out[2] = (2*(x*z-y*w))*s[0];   out[3] = 0;
    out[4] = (2*(x*y-z*w))*s[1];   out[5] = (1-2*(x*x+z*z))*s[1]; out[6] = (2*(y*z+x*w))*s[1];   out[7] = 0;
    out[8] = (2*(x*z+y*w))*s[2];   out[9] = (2*(y*z-x*w))*s[2];   out[10]= (1-2*(x*x+y*y))*s[2]; out[11]= 0;
    out[12]= t[0]; out[13]= t[1]; out[14]= t[2]; out[15]= 1;
}

void mul(const f32 a[16], const f32 b[16], f32 out[16]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            f32 s = 0;
            for (int k = 0; k < 4; ++k) s += a[r*4+k] * b[k*4+c];
            out[r*4+c] = s;
        }
}

bool Gltf::run(std::string* why) {
    if (d_.has("skins"))       note("skins (the .ocskel format exists; extraction does not yet)");
    if (d_.has("animations"))  note("animations (the .ocanim format exists; extraction does not yet)");
    if (d_.has("materials"))   note("material definitions (names are kept as slots; parameters are not)");
    if (d_.has("images") || d_.has("textures")) note("textures");

    const JsonValue& meshes = d_["meshes"];
    if (meshes.size() == 0) return fail(why, "glTF: the file contains no meshes");

    // Walk the scene so a mesh parented under a rotated or offset node arrives where the author put
    // it. A mesh referenced by no node is still imported, at identity, rather than dropped -- some
    // exporters emit those and losing geometry silently is the worst outcome available.
    std::vector<u8> visited(meshes.size(), 0);
    r_.meshes.assign(meshes.size(), OcMeshData{});
    r_.meshNames.assign(meshes.size(), std::string());
    for (usize i = 0; i < meshes.size(); ++i)
        if (meshes[i].has("name")) r_.meshNames[i] = std::string(meshes[i]["name"].asString());

    const JsonValue& nodes = d_["nodes"];
    struct Pending { i64 node; f32 xf[16]; };
    std::vector<Pending> stack;
    static const f32 kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    // Roots: the default scene's nodes, or every node when there is no scene.
    const i64 sceneIdx = d_["scene"].asInt(0);
    const JsonValue& scenes = d_["scenes"];
    if (usize(sceneIdx) < scenes.size()) {
        const JsonValue& roots = scenes[usize(sceneIdx)]["nodes"];
        for (usize i = 0; i < roots.size(); ++i) { Pending p{roots[i].asInt(-1), {}};
            std::memcpy(p.xf, kIdentity, sizeof(kIdentity)); stack.push_back(p); }
    } else {
        for (usize i = 0; i < nodes.size(); ++i) { Pending p{i64(i), {}};
            std::memcpy(p.xf, kIdentity, sizeof(kIdentity)); stack.push_back(p); }
    }

    usize guard = 0;
    while (!stack.empty()) {
        // A cycle in the node graph is illegal glTF but a corrupt file can carry one, and an
        // unbounded walk would hang the editor rather than report a bad asset.
        if (++guard > 1000000) return fail(why, "glTF: node hierarchy is cyclic or absurdly large");
        const Pending cur = stack.back();
        stack.pop_back();
        if (cur.node < 0 || usize(cur.node) >= nodes.size()) continue;
        const JsonValue& n = nodes[usize(cur.node)];

        f32 local[16], world[16];
        nodeLocal(n, local);
        mul(local, cur.xf, world);                 // row-vector: child then parent

        const i64 meshIdx = n["mesh"].asInt(-1);
        if (meshIdx >= 0 && usize(meshIdx) < meshes.size()) {
            if (!importMesh(meshes[usize(meshIdx)], world, r_.meshes[usize(meshIdx)], why)) return false;
            visited[usize(meshIdx)] = 1;
        }
        const JsonValue& kids = n["children"];
        for (usize i = 0; i < kids.size(); ++i) {
            Pending p{kids[i].asInt(-1), {}};
            std::memcpy(p.xf, world, sizeof(world));
            stack.push_back(p);
        }
    }

    for (usize i = 0; i < meshes.size(); ++i) {
        if (!visited[i]) {
            if (!importMesh(meshes[i], kIdentity, r_.meshes[i], why)) return false;
            note("a mesh referenced by no node (imported at identity)");
        }
        r_.meshes[i].computeBounds();
    }
    return true;
}

// Resolve every buffer: the GLB BIN chunk, a base64 data URI, or a sibling file.
bool loadBuffers(const JsonValue& d, const std::vector<u8>& glbBin, const std::string& baseDir,
                 Buffers& out, std::string* why) {
    const JsonValue& bufs = d["buffers"];
    out.data.resize(bufs.size());
    for (usize i = 0; i < bufs.size(); ++i) {
        const JsonValue& b = bufs[i];
        if (!b.has("uri")) {
            // No URI means the GLB binary chunk, and only buffer 0 may do that.
            if (i != 0 || glbBin.empty()) return fail(why, "glTF: a buffer has no uri and there is no GLB binary chunk");
            out.data[i] = glbBin;
            continue;
        }
        const std::string uri(b["uri"].asString());
        constexpr std::string_view kB64 = "base64,";
        const usize marker = uri.find(kB64);
        if (uri.rfind("data:", 0) == 0) {
            if (marker == std::string::npos) return fail(why, "glTF: a data URI is not base64");
            if (!base64Decode(std::string_view(uri).substr(marker + kB64.size()), out.data[i]))
                return fail(why, "glTF: a base64 data URI failed to decode");
            continue;
        }
        if (baseDir.empty()) return fail(why, "glTF: the file references '" + uri + "' but no directory context was given");
        // Percent-encoding is the only URI escaping an exporter reliably emits.
        std::string rel;
        for (usize k = 0; k < uri.size(); ++k) {
            if (uri[k] == '%' && k + 2 < uri.size()) {
                auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0'
                                             : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                                             : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1; };
                const int h = hex(uri[k+1]), l = hex(uri[k+2]);
                if (h >= 0 && l >= 0) { rel.push_back(char(h * 16 + l)); k += 2; continue; }
            }
            rel.push_back(uri[k]);
        }
        const std::string full = baseDir + "\\" + rel;
        std::ifstream f(full, std::ios::binary | std::ios::ate);
        if (!f) return fail(why, "glTF: cannot open referenced buffer " + full);
        const std::streamoff n = f.tellg();
        out.data[i].resize(usize(n > 0 ? n : 0));
        f.seekg(0);
        f.read(reinterpret_cast<char*>(out.data[i].data()), n);
        if (!f) return fail(why, "glTF: short read on " + full);
    }
    return true;
}

} // namespace

bool importGltfFromMemory(const u8* bytes, usize size, const std::string& baseDir,
                          GltfImportResult& out, const GltfImportOptions& opt, std::string* why) {
    if (!bytes || size < 4) return fail(why, "glTF: file is too small");
    out.meshes.clear(); out.meshNames.clear(); out.unsupported.clear();

    std::string_view json;
    std::vector<u8> glbBin;
    std::string jsonOwned;

    // GLB is told from .gltf by MAGIC, not by extension: an extension is a claim, the magic is
    // evidence, and exporters mislabel often enough to matter.
    if (std::memcmp(bytes, "glTF", 4) == 0) {
        if (size < 12) return fail(why, "GLB: truncated header");
        u32 ver, total;
        std::memcpy(&ver, bytes + 4, 4);
        std::memcpy(&total, bytes + 8, 4);
        if (ver != 2) return fail(why, "GLB: only version 2 is supported");
        if (total > size) return fail(why, "GLB: header length exceeds the file");
        usize p = 12;
        while (p + 8 <= size) {
            u32 clen, ctype;
            std::memcpy(&clen, bytes + p, 4);
            std::memcpy(&ctype, bytes + p + 4, 4);
            p += 8;
            if (u64(p) + clen > size) return fail(why, "GLB: a chunk runs past the end of the file");
            if (ctype == 0x4E4F534A) jsonOwned.assign(reinterpret_cast<const char*>(bytes + p), clen); // 'JSON'
            else if (ctype == 0x004E4942) glbBin.assign(bytes + p, bytes + p + clen);                  // 'BIN\0'
            p += clen;
            p = (p + 3) & ~usize(3);           // chunks are 4-byte aligned
        }
        if (jsonOwned.empty()) return fail(why, "GLB: no JSON chunk");
        json = jsonOwned;
    } else {
        json = std::string_view(reinterpret_cast<const char*>(bytes), size);
    }

    JsonValue doc;
    if (!parseJson(json, doc, why)) return false;
    if (!doc.isObject()) return fail(why, "glTF: the document is not a JSON object");

    const i64 major = doc["asset"]["version"].isString()
                    ? std::atoi(std::string(doc["asset"]["version"].asString()).c_str()) : 0;
    if (major != 2) return fail(why, "glTF: only version 2.0 is supported (asset.version says '" +
                                     std::string(doc["asset"]["version"].asString("missing")) + "')");

    Buffers bufs;
    if (!loadBuffers(doc, glbBin, baseDir, bufs, why)) return false;

    Gltf g(doc, bufs, opt, out);
    return g.run(why);
}

bool importGltf(const std::string& path, GltfImportResult& out,
                const GltfImportOptions& opt, std::string* why) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return fail(why, "glTF: cannot open " + path);
    const std::streamoff n = f.tellg();
    if (n <= 0) return fail(why, "glTF: empty file " + path);
    // static_cast, not usize(n): `std::vector<u8> bytes(usize(n));` is a function declaration --
    // the most vexing parse -- and the error it produces points at the next line instead.
    std::vector<u8> bytes(static_cast<usize>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()), n);
    if (!f) return fail(why, "glTF: short read on " + path);

    const usize slash = path.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? std::string(".") : path.substr(0, slash);
    return importGltfFromMemory(bytes.data(), bytes.size(), dir, out, opt, why);
}

} // namespace aver::fmt
