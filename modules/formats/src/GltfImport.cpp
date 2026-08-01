// Imports glTF 2.0 and GLB into engine meshes: accessors, node transforms and axis conversion.

#include "aver/formats/GltfImport.hpp"

#include "aver/formats/Json.hpp"

#include <cmath>
#include <cstring>
#include <fstream>

namespace aver::fmt {
namespace {

// Sets `why` and returns false.
bool fail(std::string* why, std::string m) { if (why) *why = std::move(m); return false; }

// glTF component types (5.24 accessor.componentType).
constexpr i64 kByte = 5120, kUByte = 5121, kShort = 5122, kUShort = 5123, kUInt = 5125, kFloat = 5126;

// Size in bytes of one glTF component type. Zero when unknown.
u32 componentBytes(i64 t) {
    switch (t) {
        case kByte: case kUByte:   return 1;
        case kShort: case kUShort: return 2;
        case kUInt: case kFloat:   return 4;
        default:                   return 0;
    }
}

// Component count of a glTF accessor type name. Zero when unknown.
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

// Decodes base64, as found in a data: URI. Returns false on an illegal character.
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

// Every glTF buffer, resolved to bytes.
struct Buffers {
    std::vector<std::vector<u8>> data;
};

// One decoded accessor, widened to f32.
struct AccessorF {
    std::vector<f32> v;
    u32 components = 0;
    u32 count = 0;
};

// Walks one glTF document and fills a GltfImportResult.
class Gltf {
public:
    Gltf(const JsonValue& doc, Buffers& bufs, const GltfImportOptions& opt, GltfImportResult& out)
        : d_(doc), b_(bufs), o_(opt), r_(out) {}

    // Imports every mesh in the document. Returns false with `why` set.
    bool run(std::string* why);

private:
    const JsonValue& d_;
    Buffers& b_;
    const GltfImportOptions& o_;
    GltfImportResult& r_;
    // Old joint index -> new, from the parents-before-children sort of skin 0. Meshes and animation
    // channels both address joints by the FILE's order, so both have to be rewritten through this.
    std::vector<i32> jointRemap_;

    // Records an unsupported feature, once each.
    void note(const std::string& what) {
        for (const std::string& s : r_.unsupported) if (s == what) return;
        r_.unsupported.push_back(what);
    }

    // Resolves a bufferView to the bytes it names, with its stride.
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

    // Decodes an accessor to floats, applying `normalized`. Returns false with `why` set.
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

        // Per glTF, an accessor with no bufferView reads as zeros.
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

    // Converts glTF space (right-handed, +Y up, metres) to engine space (left-handed, +Z up, cm).
    Vec3 toEngine(f32 x, f32 y, f32 z, bool isDirection) const {
        Vec3 v = o_.convertAxes ? Vec3{-z, x, y} : Vec3{x, y, z};
        if (!isDirection) { v.x *= o_.scale; v.y *= o_.scale; v.z *= o_.scale; }
        return v;
    }

    // A rotation through the same basis change. NOT the obvious component shuffle: the map has
    // determinant -1, so conjugating a rotation by it flips the sense of the turn. The axis is a
    // pseudovector, so it picks up a sign the position vectors do not, and the angle is unchanged --
    // which is why the vector part negates and w does not.
    Quat toEngineQuat(f32 x, f32 y, f32 z, f32 w) const {
        return o_.convertAxes ? Quat{z, -x, -y, w} : Quat{x, y, z, w};
    }
    // Scale is dimensionless, so it permutes with the axes and takes neither sign nor unit change.
    Vec3 toEngineScale(f32 x, f32 y, f32 z) const {
        return o_.convertAxes ? Vec3{z, x, y} : Vec3{x, y, z};
    }

    // Appends one primitive to `m` as a submesh. Returns false with `why` set.
    bool importPrimitive(const JsonValue& prim, const f32 node[16], OcMeshData& m, std::string* why);
    // Appends every primitive of a mesh to `m`. Returns false with `why` set.
    bool importMesh(const JsonValue& mesh, const f32 node[16], OcMeshData& m, std::string* why);
    // Builds one OcSkeleton per glTF skin, in the skin's own joint order.
    bool importSkins(std::string* why);
    // Builds one OcAnimation per glTF animation, addressing skin 0's bones.
    bool importAnimations(std::string* why);
};

// Row-vector transform of a point or direction by a 4x4 row-major matrix.
void xform(const f32 m[16], f32 x, f32 y, f32 z, bool point, f32 out[3]) {
    const f32 w = point ? 1.0f : 0.0f;
    out[0] = x*m[0] + y*m[4] + z*m[8]  + w*m[12];
    out[1] = x*m[1] + y*m[5] + z*m[9]  + w*m[13];
    out[2] = x*m[2] + y*m[6] + z*m[10] + w*m[14];
}

bool Gltf::importPrimitive(const JsonValue& prim, const f32 node[16], OcMeshData& m, std::string* why) {
    // Only triangles; other modes are noted and skipped.
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
    if (attrs.has("COLOR_0"))  note("vertex colour");
    if (attrs.has("JOINTS_1")) note("a second set of bone influences (only JOINTS_0 is carried)");

    // Skinning. Both must be present: joints with no weights move nothing, weights with no joints
    // have nothing to move.
    AccessorF joints, weights;
    const bool hasSkin = attrs.has("JOINTS_0") && attrs.has("WEIGHTS_0")
                      && readAccessor(attrs["JOINTS_0"].asInt(-1), joints, why)
                      && readAccessor(attrs["WEIGHTS_0"].asInt(-1), weights, why)
                      && joints.components == 4 && weights.components == 4
                      && joints.count == pos.count && weights.count == pos.count;
    if (!hasSkin && attrs.has("JOINTS_0"))
        note("skinning on a primitive whose JOINTS_0/WEIGHTS_0 do not match its POSITION");

    const u32 baseVertex = m.vertexCount();
    // A skinned primitive after an unskinned one: back-fill the earlier vertices so the streams stay
    // 1:1 with the positions. Binding them all to bone 0 leaves them rigid, which is what they were.
    if (hasSkin && m.joints.size() < usize(baseVertex) * kOcMeshInfluences) {
        while (m.joints.size() < usize(baseVertex) * kOcMeshInfluences) {
            m.joints.push_back(0);
            m.weights.push_back(m.weights.size() % kOcMeshInfluences == 0 ? 1.0f : 0.0f);
        }
    }
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

        // Bone influences pass through UNCONVERTED: they index a skin's joint list and weight it,
        // and neither is a direction, so the basis change does not touch them.
        if (hasSkin) {
            for (u32 k = 0; k < kOcMeshInfluences; ++k) {
                m.joints.push_back(static_cast<u16>(joints.v[usize(i)*4 + k]));
                m.weights.push_back(weights.v[usize(i)*4 + k]);
            }
        }
    }
    // A mesh whose primitives disagree about skinning would leave the streams the wrong length, so
    // an unskinned primitive after a skinned one is padded to keep them 1:1 with the vertices.
    if (!hasSkin && !m.joints.empty()) {
        for (u32 i = 0; i < pos.count; ++i)
            for (u32 k = 0; k < kOcMeshInfluences; ++k) {
                m.joints.push_back(0);
                m.weights.push_back(k == 0 ? 1.0f : 0.0f);
            }
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
            // Winding reverses with the handedness.
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

    // Flat normals where the source had none, computed after the winding fix so they face outward.
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

    // One submesh per primitive, carrying the material split.
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

// A node's local transform: either its 16-float matrix, or its TRS composed.
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

// Multiplies two 4x4 row-major matrices.
void mul(const f32 a[16], const f32 b[16], f32 out[16]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            f32 s = 0;
            for (int k = 0; k < 4; ++k) s += a[r*4+k] * b[k*4+c];
            out[r*4+c] = s;
        }
}

// One OcSkeleton per glTF skin. Bone order IS the skin's joint order, so a mesh's JOINTS_0 indices
// address the result directly with no remap.
bool Gltf::importSkins(std::string* why) {
    const JsonValue& skins = d_["skins"];
    const JsonValue& nodes = d_["nodes"];
    if (skins.size() == 0) return true;

    for (usize s = 0; s < skins.size(); ++s) {
        const JsonValue& skin = skins[s];
        const JsonValue& joints = skin["joints"];
        if (joints.size() == 0) { note("a skin with no joints"); continue; }

        // node index -> bone index, so a parent can be found by walking the node tree.
        std::vector<i32> boneOfNode(nodes.size(), -1);
        for (usize j = 0; j < joints.size(); ++j) {
            const i64 n = joints[j].asInt(-1);
            if (n >= 0 && usize(n) < nodes.size()) boneOfNode[usize(n)] = static_cast<i32>(j);
        }
        // The reverse edge glTF does not store: a node lists its children, never its parent.
        std::vector<i32> parentOfNode(nodes.size(), -1);
        for (usize n = 0; n < nodes.size(); ++n) {
            const JsonValue& kids = nodes[n]["children"];
            for (usize k = 0; k < kids.size(); ++k) {
                const i64 c = kids[k].asInt(-1);
                if (c >= 0 && usize(c) < nodes.size()) parentOfNode[usize(c)] = static_cast<i32>(n);
            }
        }

        OcSkeleton sk;
        sk.bones.resize(joints.size());
        for (usize j = 0; j < joints.size(); ++j) {
            const i64 ni = joints[j].asInt(-1);
            OcBone& b = sk.bones[j];
            if (ni < 0 || usize(ni) >= nodes.size()) continue;
            const JsonValue& n = nodes[usize(ni)];
            if (n.has("name")) b.name = std::string(n["name"].asString());

            // A joint's parent is its nearest ANCESTOR that is also in this skin. glTF allows
            // non-joint nodes in between, and treating those as roots would break the chain.
            i32 p = parentOfNode[usize(ni)];
            while (p >= 0 && boneOfNode[usize(p)] < 0) p = parentOfNode[usize(p)];
            b.parent = p >= 0 ? boneOfNode[usize(p)] : kOcBoneNoParent;

            if (n.has("matrix")) {
                note("a joint whose transform is a matrix rather than TRS (rest pose taken as identity)");
            } else {
                f32 t[3] = {0,0,0}, r[4] = {0,0,0,1}, sc[3] = {1,1,1};
                if (n.has("translation")) for (int i = 0; i < 3; ++i) t[i] = n["translation"][usize(i)].asFloat();
                if (n.has("rotation"))    for (int i = 0; i < 4; ++i) r[i] = n["rotation"][usize(i)].asFloat();
                if (n.has("scale"))       for (int i = 0; i < 3; ++i) sc[i] = n["scale"][usize(i)].asFloat(1.0f);
                b.translation = toEngine(t[0], t[1], t[2], false);
                b.rotation    = toEngineQuat(r[0], r[1], r[2], r[3]);
                b.scale       = toEngineScale(sc[0], sc[1], sc[2]);
            }
        }

        // PARENTS BEFORE CHILDREN is a contract of the format, and glTF does not promise it. Sort
        // topologically and rewrite every parent index, or OcSkeleton::valid() rejects the result
        // and poseToModel would read a parent that has not been resolved yet.
        std::vector<i32> order;
        order.reserve(sk.bones.size());
        std::vector<u8> placed(sk.bones.size(), 0);
        bool progress = true;
        while (order.size() < sk.bones.size() && progress) {
            progress = false;
            for (usize i = 0; i < sk.bones.size(); ++i) {
                if (placed[i]) continue;
                const i32 p = sk.bones[i].parent;
                if (p == kOcBoneNoParent || placed[usize(p)]) {
                    order.push_back(static_cast<i32>(i));
                    placed[i] = 1;
                    progress = true;
                }
            }
        }
        if (order.size() != sk.bones.size()) {
            note("a skin whose joint hierarchy has a cycle");
            continue;
        }
        std::vector<i32> newIndexOf(sk.bones.size(), -1);
        for (usize i = 0; i < order.size(); ++i) newIndexOf[usize(order[i])] = static_cast<i32>(i);

        OcSkeleton sorted;
        sorted.bones.reserve(sk.bones.size());
        for (const i32 old : order) {
            OcBone b = sk.bones[usize(old)];
            if (b.parent != kOcBoneNoParent) b.parent = newIndexOf[usize(b.parent)];
            sorted.bones.push_back(std::move(b));
        }
        sorted.rootBone = 0;

        // THE INVERSE BIND IS RE-DERIVED from the rest pose rather than read from the file.
        // glTF supplies inverseBindMatrices, but converting a matrix through a determinant -1 basis
        // change is a second, independent chance to get the handedness wrong -- and the value is
        // definitionally the inverse of the bone's model-space rest transform, which this importer
        // has just built. Deriving it makes a bind-pose skeleton skin to the identity by
        // construction. A file whose bind pose genuinely differs from its node rest pose is noted.
        if (skin.has("inverseBindMatrices"))
            note("inverseBindMatrices (re-derived from the rest pose, which is equivalent unless the "
                 "file's bind pose differs from its node transforms)");

        std::vector<Mat4> model(sorted.bones.size(), Mat4::identity());
        for (usize i = 0; i < sorted.bones.size(); ++i) {
            Transform x;
            x.position = sorted.bones[i].translation;
            x.rotation = sorted.bones[i].rotation;
            x.scale    = sorted.bones[i].scale;
            const Mat4 local = x.toMatrix();
            const i32 p = sorted.bones[i].parent;
            model[i] = (p >= 0 && usize(p) < i) ? local * model[usize(p)] : local;
            const Mat4 inv = model[i].inverse();
            for (int rr = 0; rr < 4; ++rr)
                for (int cc = 0; cc < 4; ++cc) sorted.bones[i].inverseBind[rr * 4 + cc] = inv.m[rr][cc];
        }

        // The joint reorder has to reach the meshes too, or every JOINTS_0 index now names the
        // wrong bone. Only skin 0's meshes are remapped, which is the case the result documents.
        if (s == 0) {
            for (OcMeshData& m : r_.meshes)
                for (u16& j : m.joints)
                    if (usize(j) < newIndexOf.size() && newIndexOf[j] >= 0)
                        j = static_cast<u16>(newIndexOf[j]);
            jointRemap_ = newIndexOf;
        }

        r_.skeletons.push_back(std::move(sorted));
        r_.skeletonNames.push_back(skin.has("name") ? std::string(skin["name"].asString()) : std::string());
    }
    (void)why;
    return true;
}

// One OcAnimation per glTF animation. One OcTrack PER CHANNEL rather than per bone: glTF gives each
// channel its own time accessor, and merging two channels that do not share a timeline would mean
// resampling one of them and losing exactly the fidelity .ocanim exists to keep.
bool Gltf::importAnimations(std::string* why) {
    const JsonValue& anims = d_["animations"];
    if (anims.size() == 0) return true;
    if (r_.skeletons.empty()) { note("animations on a file with no skin"); return true; }

    const JsonValue& skins = d_["skins"];
    const JsonValue& joints = skins[0]["joints"];
    const JsonValue& nodes = d_["nodes"];
    std::vector<i32> boneOfNode(nodes.size(), -1);
    for (usize j = 0; j < joints.size(); ++j) {
        const i64 n = joints[j].asInt(-1);
        if (n >= 0 && usize(n) < nodes.size()) {
            const i32 pre = static_cast<i32>(j);
            boneOfNode[usize(n)] = (usize(pre) < jointRemap_.size() && jointRemap_[usize(pre)] >= 0)
                                 ? jointRemap_[usize(pre)] : pre;
        }
    }

    for (usize a = 0; a < anims.size(); ++a) {
        const JsonValue& an = anims[a];
        const JsonValue& channels = an["channels"];
        const JsonValue& samplers = an["samplers"];
        OcAnimation clip;
        clip.storage = OcAnimStorage::Keyframed;
        clip.skeletonRef = r_.skeletonNames.empty() ? std::string() : r_.skeletonNames[0];

        for (usize c = 0; c < channels.size(); ++c) {
            const JsonValue& ch = channels[c];
            const JsonValue& target = ch["target"];
            const i64 nodeIdx = target["node"].asInt(-1);
            const std::string path(target["path"].asString());
            if (path == "weights") { note("morph-target animation"); continue; }
            if (nodeIdx < 0 || usize(nodeIdx) >= boneOfNode.size() || boneOfNode[usize(nodeIdx)] < 0) {
                note("an animation channel targeting a node that is not a joint of skin 0");
                continue;
            }
            const i64 si = ch["sampler"].asInt(-1);
            if (si < 0 || usize(si) >= samplers.size()) { note("an animation channel with no sampler"); continue; }
            const JsonValue& sm = samplers[usize(si)];

            AccessorF in, outv;
            if (!readAccessor(sm["input"].asInt(-1), in, why)) return false;
            if (!readAccessor(sm["output"].asInt(-1), outv, why)) return false;
            if (in.components != 1 || in.count == 0) { note("an animation sampler whose input is not scalar"); continue; }

            const std::string interp(sm["interpolation"].asString("LINEAR"));
            OcTrack t;
            t.boneIndex = static_cast<u16>(boneOfNode[usize(nodeIdx)]);
            t.interp = interp == "STEP" ? OcInterp::Step
                     : interp == "CUBICSPLINE" ? OcInterp::CubicSpline : OcInterp::Linear;
            t.channels = path == "translation" ? kOcChannelTranslation
                       : path == "rotation"    ? kOcChannelRotation
                       : path == "scale"       ? kOcChannelScale : 0;
            if (t.channels == 0) { note("an animation channel with an unknown path '" + path + "'"); continue; }

            const u32 width = t.channels == kOcChannelRotation ? 4u : 3u;
            const u32 slots = t.interp == OcInterp::CubicSpline ? 3u : 1u;
            if (outv.components != width || outv.count != in.count * slots) {
                note("an animation sampler whose output does not match its input");
                continue;
            }

            t.times.assign(in.v.begin(), in.v.begin() + in.count);
            t.values.reserve(usize(in.count) * width * slots);
            // Cubic stores in-tangent, value and out-tangent per key, and all three convert the same
            // way: a tangent of a position is a position per second, a tangent of a rotation a
            // quaternion per second, and the basis change is linear.
            for (u32 k = 0; k < in.count * slots; ++k) {
                const f32* v = &outv.v[usize(k) * width];
                if (t.channels == kOcChannelTranslation) {
                    const Vec3 p = toEngine(v[0], v[1], v[2], false);
                    t.values.insert(t.values.end(), {p.x, p.y, p.z});
                } else if (t.channels == kOcChannelRotation) {
                    const Quat q = toEngineQuat(v[0], v[1], v[2], v[3]);
                    t.values.insert(t.values.end(), {q.x, q.y, q.z, q.w});
                } else {
                    const Vec3 sv = toEngineScale(v[0], v[1], v[2]);
                    t.values.insert(t.values.end(), {sv.x, sv.y, sv.z});
                }
            }
            for (const f32 tt : t.times) if (tt > clip.duration) clip.duration = tt;
            clip.tracks.push_back(std::move(t));
        }

        if (clip.tracks.empty()) { note("an animation with no usable channels"); continue; }
        r_.animations.push_back(std::move(clip));
        r_.animationNames.push_back(an.has("name") ? std::string(an["name"].asString()) : std::string());
    }
    return true;
}

bool Gltf::run(std::string* why) {
    if (d_.has("materials"))   note("material definitions (names are kept as slots; parameters are not)");
    if (d_.has("images") || d_.has("textures")) note("textures");

    const JsonValue& meshes = d_["meshes"];
    if (meshes.size() == 0) return fail(why, "glTF: the file contains no meshes");

    // Walk the scene so a mesh under a transformed node arrives where the author put it.
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

    // Skins before animations: a clip addresses bones, and the bone order is not settled until the
    // skin has been sorted parents-before-children.
    if (!importSkins(why)) return false;
    if (!importAnimations(why)) return false;
    return true;
}

// Resolves every buffer: the GLB BIN chunk, a base64 data URI, or a sibling file.
bool loadBuffers(const JsonValue& d, const std::vector<u8>& glbBin, const std::string& baseDir,
                 Buffers& out, std::string* why) {
    const JsonValue& bufs = d["buffers"];
    out.data.resize(bufs.size());
    for (usize i = 0; i < bufs.size(); ++i) {
        const JsonValue& b = bufs[i];
        if (!b.has("uri")) {
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
        // Percent-decode the relative path.
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

// Imports glTF or GLB from a byte range. `baseDir` resolves external buffers. False with `why` set.
bool importGltfFromMemory(const u8* bytes, usize size, const std::string& baseDir,
                          GltfImportResult& out, const GltfImportOptions& opt, std::string* why) {
    if (!bytes || size < 4) return fail(why, "glTF: file is too small");
    out.meshes.clear(); out.meshNames.clear(); out.unsupported.clear();

    std::string_view json;
    std::vector<u8> glbBin;
    std::string jsonOwned;

    // GLB is told from .gltf by magic, not by extension.
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

// Imports a glTF or GLB file from disk. Returns false with `why` set.
bool importGltf(const std::string& path, GltfImportResult& out,
                const GltfImportOptions& opt, std::string* why) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return fail(why, "glTF: cannot open " + path);
    const std::streamoff n = f.tellg();
    if (n <= 0) return fail(why, "glTF: empty file " + path);
    // static_cast, not usize(n): the latter is the most vexing parse.
    std::vector<u8> bytes(static_cast<usize>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()), n);
    if (!f) return fail(why, "glTF: short read on " + path);

    const usize slash = path.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? std::string(".") : path.substr(0, slash);
    return importGltfFromMemory(bytes.data(), bytes.size(), dir, out, opt, why);
}

} // namespace aver::fmt
