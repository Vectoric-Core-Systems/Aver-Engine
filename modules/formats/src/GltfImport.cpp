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

// A URI's escapes turned back into bytes. Lifted out of loadBuffers rather than copied: images
// resolve their relative URIs by exactly the same rule buffers do, and a second copy of this loop
// would be a second place for "%20" to stop meaning a space.
std::string percentDecode(std::string_view uri) {
    const auto hex = [](char c) {
        return c >= '0' && c <= '9' ? c - '0'
             : (c >= 'a' && c <= 'f') ? c - 'a' + 10
             : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
    };
    std::string out;
    out.reserve(uri.size());
    for (usize k = 0; k < uri.size(); ++k) {
        if (uri[k] == '%' && k + 2 < uri.size()) {
            const int h = hex(uri[k + 1]), l = hex(uri[k + 2]);
            if (h >= 0 && l >= 0) { out.push_back(char(h * 16 + l)); k += 2; continue; }
        }
        out.push_back(uri[k]);
    }
    return out;
}

// A name safe to use as a filename stem. Anything that is not a letter, digit, dash or underscore
// becomes an underscore, because a glTF is free to name a material "Wall / Brick (wet)" and this
// engine addresses materials BY FILENAME.
std::string sanitizeStem(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        out.push_back(ok ? c : '_');
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
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
    // baseDir resolves an image's relative URI, exactly as it already resolves a buffer's. Empty
    // means "there is no directory to be relative to" -- a .glb handed over as bytes -- and an
    // external URI is then refused by name rather than guessed at.
    Gltf(const JsonValue& doc, Buffers& bufs, const GltfImportOptions& opt, GltfImportResult& out,
         const std::string& baseDir)
        : d_(doc), b_(bufs), o_(opt), r_(out), baseDir_(baseDir) {}

    // Imports every mesh in the document. Returns false with `why` set.
    bool run(std::string* why);

private:
    const JsonValue& d_;
    Buffers& b_;
    const GltfImportOptions& o_;
    GltfImportResult& r_;
    std::string baseDir_;   // for relative image URIs; empty refuses them
    // Old joint index -> new, from the parents-before-children sort of skin 0. Meshes and animation
    // channels both address joints by the FILE's order, so both have to be rewritten through this.
    std::vector<i32> jointRemap_;
    // Parallel to r_.meshes: which d_["skins"] entry (by RAW file index, before importSkins()'s
    // dedup) the owning node named, or -1. Read off the node in run()'s scene walk -- the skin
    // association lives there, not on the mesh -- and consumed by importSkins() to know which
    // meshes to remap for each skin, and to fill in the PUBLIC, POST-DEDUP r_.meshSkinIndex.
    std::vector<i32> meshRawSkin_;

    // Records an unsupported feature, once each.
    void note(const std::string& what) {
        for (const std::string& s : r_.unsupported) if (s == what) return;
        r_.unsupported.push_back(what);
    }

    // The name a material is known by. SHARED with importPrimitive's slot naming on purpose: the
    // slot string and GltfMaterial::name have to agree exactly or the rename that joins a cooked
    // .ocmat to its mesh slot silently matches nothing.
    std::string materialName(i64 idx) const {
        const JsonValue& mats = d_["materials"];
        if (idx >= 0 && usize(idx) < mats.size() && mats[usize(idx)].has("name"))
            return std::string(mats[usize(idx)]["name"].asString());
        return "Material_" + std::to_string(idx);
    }

    // Every image the file names, as encoded bytes. Runs BEFORE importMaterials, which bounds-checks
    // texture references against r_.images.size().
    void importImages();

    // texture index -> image index, with the indirection glTF puts in the way:
    //   material -> textureInfo{index} -> textures[index]{source} -> images[source]
    GltfMaterial::TexRef readTexRef(const JsonValue& info, const char* slot, const std::string& matName);

    // Every material, flattened. Notes each extension it had to ignore, by name.
    void importMaterials();

    // Resolves a bufferView to the bytes it names, with its stride.
    bool viewBytes(i64 viewIdx, const u8*& base, usize& len, u32& stride, std::string* why) const {
        const JsonValue& views = d_["bufferViews"];
        if (viewIdx < 0 || usize(viewIdx) >= views.size()) return fail(why, "glTF: bufferView index out of range");
        const JsonValue& bv = views[usize(viewIdx)];
        const i64 bufIdx = bv["buffer"].asInt(-1);
        if (bufIdx < 0 || usize(bufIdx) >= b_.data.size()) return fail(why, "glTF: buffer index out of range");
        const std::vector<u8>& buf = b_.data[usize(bufIdx)];
        // NEGATIVES ARE REJECTED BEFORE THE CAST, and the bound is checked by subtraction.
        // asInt returns a SIGNED i64 straight from the JSON, so a file writing byteOffset -1 became
        // 2^64-1 on the cast; `off + n > size` then wrapped back into range, passed, and
        // `buf.data() + off` was a pointer arbitrarily far from the buffer.
        const i64 offRaw = bv["byteOffset"].asInt(0);
        const i64 nRaw   = bv["byteLength"].asInt(0);
        if (offRaw < 0 || nRaw < 0) return fail(why, "glTF: bufferView has a negative byteOffset or byteLength");
        const u64 off = u64(offRaw);
        const u64 n   = u64(nRaw);
        const u64 size = u64(buf.size());
        if (off > size || n > size - off)
            return fail(why, "glTF: bufferView runs past the end of its buffer");
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
        // Same family as viewBytes above: a signed JSON integer, and a bound built by addition.
        // The span is (count-1) strides plus one element, and every term of it comes from the file.
        const i64 accOffRaw = a["byteOffset"].asInt(0);
        if (accOffRaw < 0) return fail(why, "glTF: accessor has a negative byteOffset");
        const u64 accOff = u64(accOffRaw);
        const u32 elemSize = cb * nc;
        if (stride == 0) stride = elemSize;
        const u64 span = u64(stride) * u64(count - 1) + u64(elemSize);
        if (accOff > u64(len) || span > u64(len) - accOff)
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

    // EVERY INDEX IS CHECKED BEFORE ANY OF THEM IS USED. The values came out of the file and were
    // trusted: the normal loop below indexes m.positions and m.normals with them directly, so a
    // glTF naming vertex 40000 in a 12-vertex primitive wrote three floats a long way outside both
    // vectors. That is a heap corruption reachable by opening a downloaded model.
    //
    // Refused rather than clamped. An index past the end is not a mesh with one bad triangle, it is
    // a mesh whose index buffer does not describe its vertex buffer, and drawing the rest would be
    // guessing at what the author meant.
    {
        const u32 vcount = static_cast<u32>(m.positions.size() / 3);
        for (usize i = first; i < m.indices.size(); ++i) {
            if (m.indices[i] < vcount) continue;
            return fail(why, "glTF: primitive index " + std::to_string(m.indices[i]) +
                             " is past the end of its " + std::to_string(vcount) + "-vertex buffer");
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
        // Through the shared helper, so this string and GltfMaterial::name cannot drift apart.
        const std::string name = materialName(mat);
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

// Splits a row-major, row-vector 4x4 back into the translation/rotation/scale a glTF node could
// have spelled out instead. Returns false when the matrix carries something TRS cannot represent,
// with `why` set to the reason -- the caller notes it and keeps whatever was recovered.
//
// WHY THIS HAS TO EXIST. glTF lets a node give its transform EITHER as translation/rotation/scale
// OR as a baked 16-float `matrix`, and the two are equally valid and equally common. Which one a
// file uses is decided by the exporter, not the artist: assimp emits `matrix` for every node, and
// so does anything that flattens an FBX rig, so an entire class of perfectly ordinary character
// assets arrives in the form this function exists to read. A skeleton importer that only handles
// the TRS spelling does not fail loudly on those files -- every bone silently keeps OcBone's
// default rest pose, which is identity.
//
// AND IDENTITY REST POSES HIDE. The inverse bind is re-derived from the rest pose (see importSkins
// below), so if every bone is identity then every inverse bind is identity too, and the two cancel:
// the bind-pose render is pixel-for-pixel correct. Nothing looks wrong until someone plays a clip
// against the rig, or opens it in the animation editor, at which point the rest pose the whole
// skeleton is defined relative to turns out to be nothing at all. Measured on a 45-joint character:
// 45 of 45 bones at translation (0,0,0), largest bone offset in the entire rig 0.000 cm.
//
// SHEAR IS THE ONE THING THAT CANNOT SURVIVE. A general affine matrix can shear; a TRS triple
// cannot express that, and OcBone stores a TRS triple. Rather than silently drop it, the skew is
// measured against the axes it would distort and reported, because a sheared joint means the DCC
// baked something into the rig that this format cannot carry and the artist needs to know. The
// decomposition itself is the engine's own transformFromMatrix; only the shear test is new here.
bool decomposeTrs(const f32 m[16], f32 t[3], f32 r[4], f32 s[3], std::string* why) {
    // Row lengths are the scale, and a zero one means a collapsed axis: there is no rotation to
    // recover from it, and normalising would divide by zero. Checked HERE rather than left to
    // transformFromMatrix, which returns an identity rotation in that case without saying so --
    // silence being exactly what this function exists to replace.
    f32 len[3];
    for (int i = 0; i < 3; ++i)
        len[i] = std::sqrt(m[i*4+0]*m[i*4+0] + m[i*4+1]*m[i*4+1] + m[i*4+2]*m[i*4+2]);
    if (len[0] <= 1e-8f || len[1] <= 1e-8f || len[2] <= 1e-8f) {
        t[0] = m[12]; t[1] = m[13]; t[2] = m[14];
        r[0] = r[1] = r[2] = 0.0f; r[3] = 1.0f;
        s[0] = s[1] = s[2] = 1.0f;
        if (why) *why = "a degenerate (zero-scale) axis";
        return false;
    }

    // Orthogonality of the normalised rows IS the absence of shear -- a sheared basis has axes that
    // are not at right angles, and the dot product of two unit rows is the cosine of the angle
    // between them. 1e-3 is about a twentieth of a degree: loose enough for float round-trips
    // through an exporter, tight enough that real skew does not pass. This is the one thing
    // transformFromMatrix does not check (it documents that it assumes no shear), and the reason
    // this wrapper exists at all rather than the call site using that helper directly.
    f32 skew = 0.0f;
    for (int a = 0; a < 3; ++a)
        for (int b = a + 1; b < 3; ++b) {
            f32 d = 0.0f;
            for (int k = 0; k < 3; ++k) d += (m[a*4+k]/len[a]) * (m[b*4+k]/len[b]);
            d = std::fabs(d);
            skew = d > skew ? d : skew;
        }

    // THE ENGINE ALREADY KNOWS HOW TO DO THIS. transformFromMatrix (Math.hpp) is the inverse of
    // Transform::toMatrix, handles the negative-determinant reflection, and uses Shepperd's method
    // with all four branches. Re-deriving any of that here would be a second copy to keep in step
    // with the first -- and the layouts already agree: glTF stores `matrix` column-major, which is
    // the transpose of this engine's row-major row-vector Mat4 and therefore the same sixteen
    // floats in the same order.
    Mat4 mm;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) mm.m[i][j] = m[i*4+j];
    const Transform x = transformFromMatrix(mm);

    t[0] = x.position.x; t[1] = x.position.y; t[2] = x.position.z;
    r[0] = x.rotation.x; r[1] = x.rotation.y; r[2] = x.rotation.z; r[3] = x.rotation.w;
    s[0] = x.scale.x;    s[1] = x.scale.y;    s[2] = x.scale.z;

    if (skew > 1.0e-3f) {
        if (why) *why = "shear, which a translation/rotation/scale triple cannot represent";
        return false;
    }
    return true;
}

// A node's transform as TRS, however the file spelled it -- the `matrix` form decomposed, the TRS
// form read straight out. Both call sites below hand the result to the SAME toEngine/toEngineQuat/
// toEngineScale conversion, which is the point of routing them through one function: a rest pose
// and a socket offset are the same kind of quantity, and two branches converting them separately is
// how they drift apart.
bool nodeTrs(const JsonValue& n, f32 t[3], f32 r[4], f32 s[3], std::string* why) {
    t[0] = t[1] = t[2] = 0.0f;
    r[0] = r[1] = r[2] = 0.0f; r[3] = 1.0f;
    s[0] = s[1] = s[2] = 1.0f;
    if (n.has("matrix")) {
        f32 m[16];
        nodeLocal(n, m);
        return decomposeTrs(m, t, r, s, why);
    }
    if (n.has("translation")) for (int i = 0; i < 3; ++i) t[i] = n["translation"][usize(i)].asFloat();
    if (n.has("rotation"))    for (int i = 0; i < 4; ++i) r[i] = n["rotation"][usize(i)].asFloat();
    if (n.has("scale"))       for (int i = 0; i < 3; ++i) s[i] = n["scale"][usize(i)].asFloat(1.0f);
    return true;
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

// One OcSkeleton per DISTINCT glTF skin. Bone order IS the skin's joint order, so a mesh's
// JOINTS_0 indices address the result directly once importSkins() has remapped them (below).
//
// "Distinct" rather than "one per skin OBJECT": a Kenney/Blender export routinely emits one skin
// per mesh even when every mesh shares a single armature, because Blender's glTF exporter builds
// a skin from whichever Armature modifier touched that mesh, with no notion of "this is the same
// skin as that other mesh's". The result is two (or more) skin objects that name the EXACT SAME
// joints in the EXACT SAME order -- provably the same skeleton, not merely similar ones. Detected
// by comparing each skin's raw (pre-sort) joint node-index sequence: since `sorted` below is a
// pure function of that sequence plus the shared `nodes` array (identical for every skin in the
// file), an identical sequence guarantees a bit-identical `sorted` with no need to compare the
// derived bones themselves. Where two skins collapse, `meshSkinIndex` (GltfImportResult.hpp) is
// what still tells their meshes apart -- it is set per mesh below, independent of the collapse.
bool Gltf::importSkins(std::string* why) {
    const JsonValue& skins = d_["skins"];
    const JsonValue& nodes = d_["nodes"];
    if (skins.size() == 0) return true;

    // Raw joint sequence behind each entry already pushed to r_.skeletons, parallel to it.
    std::vector<std::vector<i64>> skeletonJoints;

    for (usize s = 0; s < skins.size(); ++s) {
        const JsonValue& skin = skins[s];
        const JsonValue& joints = skin["joints"];
        if (joints.size() == 0) { note("a skin with no joints"); continue; }

        std::vector<i64> rawJoints(joints.size());
        for (usize j = 0; j < joints.size(); ++j) rawJoints[j] = joints[j].asInt(-1);

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

            // NO BRANCH ON HOW THE FILE SPELLED IT. nodeTrs reads a baked `matrix` and a TRS
            // triple into the same three values, so a rig exported one way gets the same rest pose
            // as the identical rig exported the other. This used to note the matrix case and fall
            // through with OcBone's defaults, which is an IDENTITY rest pose for every joint -- and
            // an identity rest pose is invisible until it is not, because the inverse bind is
            // re-derived from it a few lines below and the two cancel exactly on a bind-pose render.
            // EVERYTHING ABOVE THE TOPMOST JOINT STILL COUNTS, and dropping it was a real bug.
            //
            // This importer does not follow glTF's own division of labour, and says so at the
            // inverse-bind comment further down: rather than leaving a skinned mesh's vertices in
            // mesh space and letting the file's inverseBindMatrices bridge to the joints, it BAKES
            // each mesh's whole node-chain transform into its vertices and RE-DERIVES the inverse
            // bind from the rest pose. That is a coherent design -- it avoids carrying a matrix
            // through a determinant -1 basis change, which is a second independent chance to get
            // the handedness wrong -- but it only holds together if the skeleton is baked the same
            // way the mesh is. It was not: run() composes the full ancestor chain for a mesh, while
            // this loop read each joint's LOCAL transform and ignored every node above the topmost
            // joint entirely.
            //
            // WHAT THAT COST. assimp writes the FBX-to-glTF axis and unit conversion as one matrix
            // on both the armature root and the mesh node -- on the rig this was found on,
            // literally the same matrix on each: a quarter turn about X and a scale of 100. The
            // mesh got it and the skeleton did not, so the two disagreed in orientation AND by two
            // orders of magnitude: a mesh 37,653 cm tall against a rig spanning 335 cm, and lying
            // on its back relative to it. Invisible at rest, because the re-derived inverse bind
            // cancels against whatever the rest pose happens to be, and ruinous the moment a clip
            // plays -- every rotation pivoting about the wrong point, in the wrong orientation, at
            // a hundredth of the scale.
            //
            // ONLY ON THE TOPMOST JOINTS. A joint with a parent inside the skin already inherits
            // the chain through that parent, so applying it at every level would raise it to the
            // power of the hierarchy's depth.
            f32 t[3], r[4], sc[3];
            std::string lost;
            if (b.parent == kOcBoneNoParent && parentOfNode[usize(ni)] >= 0) {
                f32 above[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
                for (i32 anc = parentOfNode[usize(ni)]; anc >= 0; anc = parentOfNode[usize(anc)]) {
                    f32 al[16], composed[16];
                    nodeLocal(nodes[usize(anc)], al);
                    mul(above, al, composed);          // row-vector: child then parent
                    std::memcpy(above, composed, sizeof(above));
                }
                // Folded in as a MATRIX rather than by composing TRS triples, because a rotation
                // composed with a non-uniform parent scale is not itself a TRS -- it can shear,
                // which decomposeTrs then reports instead of quietly dropping.
                f32 self[16], total[16];
                nodeLocal(n, self);
                mul(self, above, total);
                if (!decomposeTrs(total, t, r, sc, &lost))
                    note("a root joint ('" + b.name + "') whose chain to the scene root carries " +
                         lost + "; the rest of its rest pose was recovered");
            } else {
                if (!nodeTrs(n, t, r, sc, &lost))
                    note("a joint ('" + b.name + "') whose baked transform carries " + lost +
                         "; the rest of its rest pose was recovered");
            }
            b.translation = toEngine(t[0], t[1], t[2], false);
            b.rotation    = toEngineQuat(r[0], r[1], r[2], r[3]);
            b.scale       = toEngineScale(sc[0], sc[1], sc[2]);
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

        // ---- SOCKETS ------------------------------------------------------------------------
        //
        // AN EMPTY PARENTED TO A BONE. That is how an artist authors an attachment point in Blender
        // or Maya, and glTF carries it as an ordinary node: a child of a joint that is not itself a
        // joint and holds no geometry. Nothing in glTF calls it a socket, so this is a RULE this
        // importer applies rather than a field it reads -- which is why the rule is spelled out and
        // why each rejection below is a separate, stated condition instead of one loose heuristic.
        //
        // A node qualifies when ALL of these hold:
        //   1. its parent is a joint of THIS skin -- the socket has a bone to hang from;
        //   2. it is not itself a joint -- a bone is a bone;
        //   3. it carries no mesh, camera or skin -- a skinned mesh is routinely parented under the
        //      armature, and treating it as a socket would make every character export sprout one;
        //   4. no descendant of it is a joint. glTF permits non-joint nodes BETWEEN joints (see the
        //      "nearest ancestor that is also in this skin" walk above), and such a spacer is part of
        //      the hierarchy, not an attachment point. This is the condition a looser rule misses.
        {
            // Does this subtree contain a joint? Iterative, because a node graph from a DCC is not
            // guaranteed shallow and a recursive walk over an untrusted file is a stack risk.
            auto subtreeHasJoint = [&](usize root) {
                std::vector<usize> stack{root};
                std::vector<u8> seen(nodes.size(), 0);
                while (!stack.empty()) {
                    const usize n = stack.back(); stack.pop_back();
                    if (n >= nodes.size() || seen[n]) continue;
                    seen[n] = 1;
                    if (n != root && boneOfNode[n] >= 0) return true;
                    const JsonValue& kids = nodes[n]["children"];
                    for (usize k = 0; k < kids.size(); ++k) {
                        const i64 c = kids[k].asInt(-1);
                        if (c >= 0) stack.push_back(usize(c));
                    }
                }
                return false;
            };

            for (usize n = 0; n < nodes.size(); ++n) {
                if (boneOfNode[n] >= 0) continue;                       // 2: it is a joint
                const i32 parentNode = parentOfNode[n];
                if (parentNode < 0 || boneOfNode[usize(parentNode)] < 0) continue;   // 1
                const JsonValue& nd = nodes[n];
                if (nd.has("mesh") || nd.has("camera") || nd.has("skin")) continue;  // 3
                if (subtreeHasJoint(n)) continue;                       // 4

                const i32 oldBone = boneOfNode[usize(parentNode)];
                if (oldBone < 0 || usize(oldBone) >= newIndexOf.size()) continue;
                const i32 bone = newIndexOf[usize(oldBone)];
                if (bone < 0) continue;

                OcSocket k;
                k.bone = static_cast<u32>(bone);
                k.name = nd.has("name") ? std::string(nd["name"].asString()) : std::string();
                if (k.name.empty()) k.name = "Socket";
                // MADE UNIQUE, matching what the editor does on add and for the same reason:
                // OcSkeleton::socket() returns the FIRST match, so a duplicate name would leave the
                // loser permanently unreachable. Noted, because a rename in the DCC is the real fix.
                if (sorted.socket(k.name)) {
                    note("two sockets named '" + k.name + "'; the second was renamed to keep both "
                         "reachable by name");
                    const std::string base = k.name;
                    for (int i = 2; sorted.socket(k.name); ++i) k.name = base + std::to_string(i);
                }

                // The joints' defect exactly, and fixed the same way: a socket authored on a
                // node the exporter baked to a matrix used to land at the bone's own origin, which
                // reads as "the weapon is inside the character's wrist" rather than as an import
                // warning anyone would go looking for.
                f32 t[3], r[4], sc[3];
                std::string lost;
                if (!nodeTrs(nd, t, r, sc, &lost))
                    note("a socket ('" + k.name + "') whose baked transform carries " + lost +
                         "; the rest of its offset was recovered");
                // THE SAME BASIS CHANGE THE BONES GET, not a hand-rolled one. A socket offset is
                // a bone-local transform exactly as a joint's rest transform is, so anything else
                // here would put attachments in a mirrored place on an otherwise correct rig.
                k.translation = toEngine(t[0], t[1], t[2], false);
                k.rotation    = toEngineQuat(r[0], r[1], r[2], r[3]);
                k.scale       = toEngineScale(sc[0], sc[1], sc[2]);
                sorted.sockets.push_back(std::move(k));
            }
        }

        // The joint reorder has to reach every mesh THIS skin owns, or its JOINTS_0 indices now
        // name the wrong bone -- restricted to those meshes via meshRawSkin_ (set from the owning
        // node in run()), not applied to every mesh in the file: skin s's raw joint order is not
        // even the same INDEX SPACE as some other skin's meshes, so remapping them through it
        // would corrupt, not merely mis-target, their joint indices. (This is the loop-scoping bug
        // the old `if (s == 0)` version of this block had: it applied skin 0's remap to every mesh
        // in r_.meshes unconditionally, and no other skin's own remap ever ran at all. Harmless on
        // a file whose skins are already identical, as importAnimations()'s own skins[0]-only
        // assumption below remains -- but live corruption on a file with a genuinely different
        // second skin whose native joint order is not already parents-before-children.)
        for (usize mi = 0; mi < r_.meshes.size(); ++mi) {
            if (meshRawSkin_[mi] != static_cast<i32>(s)) continue;
            for (u16& j : r_.meshes[mi].joints)
                if (usize(j) < newIndexOf.size() && newIndexOf[j] >= 0)
                    j = static_cast<u16>(newIndexOf[j]);
        }
        // importAnimations() still only ever reads skins[0]'s own joint list (see its own comment
        // for why fixing that is out of scope here), so only skin 0's remap needs to survive.
        if (s == 0) jointRemap_ = newIndexOf;

        // Same skeleton as one already written? Reuse its index rather than writing a duplicate
        // .ocskel -- see this function's own header comment for why an identical raw joint
        // sequence proves the two skeletons are identical, not merely similar.
        i32 finalIdx = -1;
        for (usize e = 0; e < skeletonJoints.size(); ++e) {
            if (skeletonJoints[e] == rawJoints) { finalIdx = static_cast<i32>(e); break; }
        }
        if (finalIdx < 0) {
            finalIdx = static_cast<i32>(r_.skeletons.size());
            r_.skeletons.push_back(std::move(sorted));
            r_.skeletonNames.push_back(skin.has("name") ? std::string(skin["name"].asString()) : std::string());
            skeletonJoints.push_back(std::move(rawJoints));
        }
        for (usize mi = 0; mi < r_.meshes.size(); ++mi)
            if (meshRawSkin_[mi] == static_cast<i32>(s)) r_.meshSkinIndex[mi] = finalIdx;
    }
    (void)why;
    return true;
}

// One OcAnimation per glTF animation. One OcTrack PER CHANNEL rather than per bone: glTF gives each
// channel its own time accessor, and merging two channels that do not share a timeline would mean
// resampling one of them and losing exactly the fidelity .ocanim exists to keep.
//
// KNOWN LIMITATION, NOT FIXED HERE: this always resolves a channel's target node against skins[0]
// ALONE, never against any later skin, even one importSkins() decided was a genuinely distinct
// skeleton. Harmless for every file in the tree today -- either there is one skin, or (this file's
// case) every later skin is a DUPLICATE of skin 0 (see importSkins()'s own comment) and so shares
// its node set exactly -- but it would silently drop a channel targeting a joint that exists only
// in skin 1+ of a file with genuinely different skins. No asset exercises that case, so fixing it
// now would be unverifiable; flagged rather than guessed at.
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

void Gltf::importImages() {
    const JsonValue& imgs = d_["images"];
    r_.images.resize(imgs.size());
    for (usize i = 0; i < imgs.size(); ++i) {
        const JsonValue& img = imgs[i];
        GltfImage& out = r_.images[i];

        const std::string declaredName = img.has("name") ? std::string(img["name"].asString()) : std::string();
        const std::string mime = img.has("mimeType") ? std::string(img["mimeType"].asString()) : std::string();
        out.ext = (mime == "image/jpeg") ? ".jpeg" : ".png";

        if (img.has("uri")) {
            const std::string uri(img["uri"].asString());
            if (uri.rfind("data:", 0) == 0) {
                // A base64 data: URI. The mimeType lives in the URI itself here, not in a sibling
                // field, so it is read from there.
                constexpr std::string_view kB64 = "base64,";
                const usize marker = uri.find(kB64);
                if (uri.find("image/jpeg") != std::string::npos) out.ext = ".jpeg";
                if (marker == std::string::npos) {
                    note("an image data: URI that is not base64 (image " + std::to_string(i) + ")");
                    continue;
                }
                if (!base64Decode(std::string_view(uri).substr(marker + kB64.size()), out.bytes)) {
                    note("an image data: URI that failed to decode (image " + std::to_string(i) + ")");
                    out.bytes.clear();
                    continue;
                }
                out.suggestedName = sanitizeStem(declaredName.empty() ? ("image" + std::to_string(i)) : declaredName);
                out.ok = true;
                continue;
            }
            // An external file, relative to the glTF. THE COMMON REAL-WORLD FAILURE: a .glb that
            // names a sibling texture which was never shipped with it. Both Kenney packs in this
            // tree do exactly that. It is refused BY PATH -- a material that quietly kept a texture
            // slot pointing at a file that is not there would look like a renderer fault.
            const std::string rel = percentDecode(uri);
            if (baseDir_.empty()) {
                note("an external image URI with no directory to resolve it against ('" + rel + "')");
                continue;
            }
            const std::string full = baseDir_ + "\\" + rel;
            std::ifstream f(full, std::ios::binary | std::ios::ate);
            if (!f) {
                note("an image the file names but does not ship: '" + rel + "'");
                continue;
            }
            const std::streamoff n = f.tellg();
            out.bytes.resize(usize(n > 0 ? n : 0));
            f.seekg(0);
            if (!out.bytes.empty()) f.read(reinterpret_cast<char*>(out.bytes.data()), std::streamsize(out.bytes.size()));
            // The URI's own basename beats the JSON name: it is what the author actually called the
            // file, and it keeps a re-import landing on the same filename.
            std::string stem = rel;
            const usize slash = stem.find_last_of("/\\");
            if (slash != std::string::npos) stem = stem.substr(slash + 1);
            const usize dot = stem.find_last_of('.');
            if (dot != std::string::npos) { out.ext = stem.substr(dot); stem = stem.substr(0, dot); }
            out.suggestedName = sanitizeStem(stem);
            out.ok = true;
            continue;
        }

        if (img.has("bufferView")) {
            // Inside the binary chunk. viewBytes gives the span; the bytes are copied VERBATIM --
            // never decoded and re-encoded, which would change bytes the source is entitled to get
            // back and would throw away quality on a JPEG for nothing.
            const u8* base = nullptr; usize len = 0; u32 stride = 0;
            std::string ignored;
            if (!viewBytes(img["bufferView"].asInt(-1), base, len, stride, &ignored)) {
                note("an image whose bufferView could not be read (image " + std::to_string(i) + ")");
                continue;
            }
            out.bytes.assign(base, base + len);
            out.suggestedName = sanitizeStem(declaredName.empty() ? ("image" + std::to_string(i)) : declaredName);
            out.ok = true;
            continue;
        }

        note("an image with neither a uri nor a bufferView (image " + std::to_string(i) + ")");
    }
}

GltfMaterial::TexRef Gltf::readTexRef(const JsonValue& info, const char* slot, const std::string& matName) {
    GltfMaterial::TexRef ref;
    if (!info.has("index")) return ref;

    // EXTENSIONS HANG OFF THE textureInfo TOO, not just the material, and that is where the common
    // one actually lives: both Kenney packs in this tree put KHR_texture_transform here. Checking
    // only the material's own extensions block misses every one of them.
    if (info.has("extensions"))
        for (const JsonMember& e : info["extensions"].members())
            note("a texture extension this importer does not carry: " + e.key +
                 " (on " + matName + "." + slot + ")");

    const i64 texIdx = info["index"].asInt(-1);
    const JsonValue& texes = d_["textures"];
    if (texIdx < 0 || usize(texIdx) >= texes.size()) {
        note(std::string("a texture index out of range on ") + matName + "." + slot);
        return ref;
    }
    const JsonValue& tex = texes[usize(texIdx)];

    // Sampling state is not carried. Said once rather than per material, because a scene has one
    // answer to this and repeating it per material would bury everything else.
    if (tex.has("sampler")) note("texture samplers (wrap and filter modes; engine defaults apply)");

    const i64 src = tex["source"].asInt(-1);
    if (src < 0 || usize(src) >= r_.images.size()) {
        note(std::string("a texture with no readable image on ") + matName + "." + slot);
        return ref;
    }
    if (!r_.images[usize(src)].ok) {
        // The image itself already said why, by name. Leaving the slot unbound is the point.
        return ref;
    }

    ref.texCoord = static_cast<u32>(info["texCoord"].asInt(0));
    if (ref.texCoord != 0) {
        // OcMeshData carries exactly one UV stream, so a second set has nowhere to go.
        note(std::string("texCoord ") + std::to_string(ref.texCoord) + " on " + matName + "." + slot +
             " (only uv0 exists; uv0 was used)");
        ref.texCoord = 0;
    }
    ref.imageIndex = static_cast<i32>(src);
    return ref;
}

void Gltf::importMaterials() {
    const JsonValue& mats = d_["materials"];
    r_.materials.resize(mats.size());
    for (usize i = 0; i < mats.size(); ++i) {
        const JsonValue& jm = mats[i];
        GltfMaterial& m = r_.materials[i];
        m.name = materialName(static_cast<i64>(i));

        // EVERY FALLBACK BELOW IS glTF'S OWN DEFAULT, PASSED EXPLICITLY. asFloat/asBool default to
        // 0.0f/false, which is the wrong answer for four of these -- metallic and roughness are 1.0
        // by spec, so a material stating neither is a rough METAL. Taking the implicit fallback
        // would produce a plausible dielectric that is wrong on every asset relying on the spec.
        if (jm.has("pbrMetallicRoughness")) {
            const JsonValue& p = jm["pbrMetallicRoughness"];
            if (p.has("baseColorFactor")) {
                const JsonValue& v = p["baseColorFactor"];
                for (usize k = 0; k < 4 && k < v.size(); ++k) m.baseColorFactor[k] = v[k].asFloat(1.0f);
            }
            m.metallicFactor  = p["metallicFactor"].asFloat(1.0f);
            m.roughnessFactor = p["roughnessFactor"].asFloat(1.0f);
            if (p.has("baseColorTexture"))         m.baseColorTex  = readTexRef(p["baseColorTexture"], "baseColor", m.name);
            if (p.has("metallicRoughnessTexture")) m.metalRoughTex = readTexRef(p["metallicRoughnessTexture"], "metalRough", m.name);
        }
        if (jm.has("normalTexture")) {
            m.normalTex   = readTexRef(jm["normalTexture"], "normal", m.name);
            m.normalScale = jm["normalTexture"]["scale"].asFloat(1.0f);
        }
        if (jm.has("occlusionTexture")) {
            m.occlusionTex      = readTexRef(jm["occlusionTexture"], "occlusion", m.name);
            m.occlusionStrength = jm["occlusionTexture"]["strength"].asFloat(1.0f);
        }
        if (jm.has("emissiveTexture")) m.emissiveTex = readTexRef(jm["emissiveTexture"], "emissive", m.name);
        if (jm.has("emissiveFactor")) {
            const JsonValue& v = jm["emissiveFactor"];
            for (usize k = 0; k < 3 && k < v.size(); ++k) m.emissiveFactor[k] = v[k].asFloat(0.0f);
        }

        // glTF core caps emissiveFactor at [0,1], so a bright emitter -- a lamp bulb, a neon tube --
        // cannot be authored with the core fields alone. KHR_materials_emissive_strength carries the
        // multiplier that lifts it past 1, and it is read here, before the "extensions we don't
        // carry" loop below, so that loop can name-check and skip it rather than reporting the very
        // extension this importer just applied as one it dropped.
        if (jm["extensions"].has("KHR_materials_emissive_strength")) {
            // Absent emissiveStrength is the spec's own default of 1.0 (no change) -- mirrored by
            // asFloat's own fallback here, the same idiom occlusionStrength above uses. A non-finite
            // or negative value can't scale a radiance sanely -- multiplying it in would leave inf/
            // NaN/negative glow sitting in emissiveFactor -- so it is noted and left unscaled instead.
            const f32 strength = jm["extensions"]["KHR_materials_emissive_strength"]["emissiveStrength"].asFloat(1.0f);
            if (std::isfinite(strength) && strength >= 0.0f) {
                for (usize k = 0; k < 3; ++k) m.emissiveFactor[k] *= strength;
            } else {
                note("a KHR_materials_emissive_strength.emissiveStrength that is not a finite, "
                     "non-negative number on " + m.name + " (left unscaled)");
            }
        }

        m.alphaMode    = jm.has("alphaMode") ? std::string(jm["alphaMode"].asString("OPAQUE")) : std::string("OPAQUE");
        m.alphaCutoff  = jm["alphaCutoff"].asFloat(0.5f);
        m.doubleSided  = jm["doubleSided"].asBool(false);

        // Extensions, NAMED. A material carrying KHR_materials_transmission is not a material this
        // importer understood and quietly simplified -- it is one whose glass is missing, and the
        // author is entitled to be told which one. KHR_materials_emissive_strength is excluded here:
        // it was read and applied above, so listing it again would call an applied extension one
        // this importer dropped.
        if (jm.has("extensions"))
            for (const JsonMember& e : jm["extensions"].members())
                if (e.key != "KHR_materials_emissive_strength")
                    note("a material extension this importer does not carry: " + e.key);
    }
}

bool Gltf::run(std::string* why) {
    // Images first: readTexRef bounds-checks against r_.images.
    importImages();
    importMaterials();

    const JsonValue& meshes = d_["meshes"];
    if (meshes.size() == 0) return fail(why, "glTF: the file contains no meshes");

    // Walk the scene so a mesh under a transformed node arrives where the author put it.
    std::vector<u8> visited(meshes.size(), 0);
    r_.meshes.assign(meshes.size(), OcMeshData{});
    r_.meshNames.assign(meshes.size(), std::string());
    r_.meshSkinIndex.assign(meshes.size(), -1);
    meshRawSkin_.assign(meshes.size(), -1);
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
            // THE TRANSLATION COMES OUT OF THE GEOMETRY AND BECOMES A PLACEMENT. Rotation and scale
            // stay baked -- they are what the mesh IS -- but the node's position never was geometry,
            // and baking it put every mesh's pivot wherever the author's world origin happened to be.
            // See GltfPlacement for what that cost, measured.
            //
            // Row-major, row-vector: the translation is row 3, elements 12..14 (see nodeLocal's own
            // comment). Zeroing that row leaves exactly the rotation/scale basis in rows 0-2, so the
            // split is a copy and two assignments rather than a decomposition that could fail on
            // shear the way splitTrs() has to worry about.
            f32 basis[16];
            std::memcpy(basis, world, sizeof(basis));
            basis[12] = basis[13] = basis[14] = 0.0f;

            if (!importMesh(meshes[usize(meshIdx)], basis, r_.meshes[usize(meshIdx)], why)) return false;
            visited[usize(meshIdx)] = 1;
            if (n.has("skin")) meshRawSkin_[usize(meshIdx)] = static_cast<i32>(n["skin"].asInt(-1));

            // THROUGH toEngine, exactly as a vertex position is, so the placement lands in the same
            // space and unit as the geometry it positions -- axis-swapped and metres-to-centimetres.
            // Doing this by hand here is how a 100x or a Y/Z swap gets in.
            GltfPlacement pl;
            pl.meshIndex = static_cast<i32>(meshIdx);
            pl.position  = toEngine(world[12], world[13], world[14], false);
            pl.name      = n["name"].asString("");
            if (pl.name.empty()) pl.name = r_.meshNames[usize(meshIdx)];
            r_.placements.push_back(std::move(pl));
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
        const std::string full = baseDir + "\\" + percentDecode(uri);
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

    Gltf g(doc, bufs, opt, out, baseDir);
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
