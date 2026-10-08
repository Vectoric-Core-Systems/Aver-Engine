// importUsdStage: a whole USD stage from its root layer -- the layer stack, binary (USDC) layers,
// references, inherits and PointInstancers. See UsdImport.hpp for the contract.
//
// WHAT "COMPOSED" MEANS HERE, precisely, because it is a subset of USD's composition engine:
//   - The ROOT LAYER STACK: the root and its subLayers (recursively), strongest first. Binary layers
//     in it are merged BY PRIM PATH -- a prim's children are the union over layers, a field's value
//     comes from the strongest layer that authors it -- which is what USD does and what makes a scene
//     split across files (Jungle Ruins' eighteen element layers all define /root) come out whole.
//   - INHERITS and REFERENCES, resolved to the geometry they bring in, recursively. A reference into
//     a text layer uses the text reader's own walk, so its materials come out identical to importUsd.
//   - POINTINSTANCERS: each prototype is resolved and built ONCE, as its own mesh, with the prototype
//     root's transform baked in (UsdGeomPointInstancer's IncludeProtoXform); each kept instance is a
//     placement carrying scale * orientation * position * the instancer's own transform.
// Not composed: variant sets, specializes, relocates, and text layers are not merged by path with
// binary ones (each text layer contributes what it defines itself). Time-sampled attributes use their
// default. Each of these is reported in `unsupported` when met, never silently skipped.
#include "aver/formats/UsdImport.hpp"
#include "aver/formats/UsdCrate.hpp"
#include "UsdImportInternal.hpp"

#include "aver/core/Math.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace aver::fmt {
namespace {

using namespace usd_detail;

bool fail(std::string* why, std::string m) { if (why) *why = std::move(m); return false; }

std::string normalisedKey(const std::string& p) {
    std::string s = std::filesystem::path(p).lexically_normal().generic_string();
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string parentPath(const std::string& prim) {
    const usize slash = prim.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return "/";
    return prim.substr(0, slash);
}

std::string leafOf(const std::string& prim) {
    const usize slash = prim.find_last_of('/');
    return slash == std::string::npos ? prim : prim.substr(slash + 1);
}

bool under(const std::string& path, const std::string& root) {
    return path == root || (path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
                            path[root.size()] == '/');
}

// ---- one opened layer --------------------------------------------------------------------------

struct Source {
    std::string path, dir;
    // The namespace this layer's Material paths and bindings are registered under (Ctx::pathPrefix),
    // so two files declaring /root/materials/Bark cannot bind each other's.
    std::string prefix;
    bool crate = false;
    std::unique_ptr<UsdCrate> cr;
    // Text layers: every mesh (unbuilt) and every prim's transform, from one walk of the file.
    std::vector<RawMesh> raw;
    std::unordered_map<std::string, M4> worlds;
    std::vector<std::string> cameraPaths;
    std::vector<LightPrim> lights;
    // Header metadata, either encoding.
    f32 metersPerUnit = 1.0f;
    bool yUp = true;
    std::string upAxis = "Y";
    std::string defaultPrim;
    std::vector<std::string> subLayers;
};

// A layer stack: crate sources, strongest first, sharing one material namespace.
struct Stack {
    std::vector<i32> layers;
    std::string prefix;
    bool materialsRegistered = false;
};

// Where a prim lives: in a crate stack, or in a text layer.
struct Loc {
    Stack* stack = nullptr;
    i32 text = -1;
    std::string path;
};

// One mesh a prototype is made of, with the matrix that carries it into prototype space.
struct Piece {
    const RawMesh* mesh = nullptr;
    M4 rel;
    std::string prefix;
    std::string group;           // UsdImportResult::meshGroups
};

struct Proto {
    i32 meshIndex = -1;
    u64 tris = 0;
    f32 radius = 1.0f;
    u64 instances = 0;
    u64 kept = 0;
    f64 keep = 1.0;
    // Mean uniform instance scale, so the budget weighs the size an instance is DRAWN at.
    f64 scaleSum = 0.0;
    u64 scaleCount = 0;
    f64 focusSum = 0.0;          // instances weighted by Focus::weight
    bool exempt = false;         // at most keepAllBelow instances: all kept, no thinning
    // focusBySize: instances per log2-spaced distance band from the focus (kDistBins), and the
    // full-density radius allocateBySize chose, engine cm.
    std::vector<u64> distHist;
    f64 reach = 0.0;
    f64 worldRadius() const { return radius * (scaleCount ? scaleSum / static_cast<f64>(scaleCount) : 1.0); }
};

// Distance bands for focusBySize: eight per doubling from 1 m, 128 bands (to ~65 km).
constexpr usize kDistBins = 128;
usize distBin(f64 dCm) {
    if (dCm <= 100.0) return 0;
    return std::min(kDistBins - 1, static_cast<usize>(std::log2(dCm / 100.0) * 8.0));
}
f64 distBinCentre(usize b) { return 100.0 * std::exp2((static_cast<f64>(b) + 0.5) / 8.0); }

struct Instancer {
    Stack* stack = nullptr;
    std::string path;
    M4 world;
    std::vector<i32> protoOfTarget;   // prototypes rel target k -> Proto index, -1 = unresolved
    u64 id = 0;
};

struct Stage {
    Ctx c;
    std::vector<std::unique_ptr<Source>> sources;
    std::unordered_map<std::string, i32> sourceByKey;
    std::deque<Stack> stacks;                               // stable addresses; Loc points in
    std::unordered_map<i32, Stack*> singleStack;            // crate source -> its own stack
    std::deque<RawMesh> rawPool;                            // crate meshes gathered for prototypes
    std::unordered_map<std::string, const RawMesh*> rawByKey;
    std::vector<Proto> protos;
    std::unordered_map<std::string, i32> protoByKey;        // piece signature -> proto
    std::unordered_map<std::string, i32> protoByTarget;     // stack prefix + target path -> proto
    std::vector<Instancer> instancers;
    std::unordered_set<std::string> notes;                  // unsupported lines, deduplicated
    std::string rootError;
    // The first DistantLight and the first DomeLight that yielded a sun; a DistantLight wins.
    UsdSun sunDistant, sunDome;
    std::unordered_set<std::string> excludeHits;            // excludePrims entries that matched

    void note(const std::string& s) { notes.insert(s); }
};

// ---- opening layers ----------------------------------------------------------------------------

i32 loadSource(Stage& st, const std::string& path, std::string* why) {
    const std::string key = normalisedKey(path);
    if (const auto it = st.sourceByKey.find(key); it != st.sourceByKey.end()) return it->second;

    auto s = std::make_unique<Source>();
    s->path = path;
    s->dir = std::filesystem::path(path).parent_path().string();
    s->prefix = "S" + std::to_string(st.sources.size()) + "|";

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { fail(why, "USD: cannot open " + path); return -1; }
    const std::streamoff n = f.tellg();
    if (n <= 0) { fail(why, "USD: empty file " + path); return -1; }
    std::vector<u8> bytes(static_cast<usize>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()), n);
    if (!f) { fail(why, "USD: short read on " + path); return -1; }

    const UsdEncoding enc = usdSniff(bytes.data(), bytes.size());
    if (enc == UsdEncoding::Usdc) {
        s->crate = true;
        s->cr = std::make_unique<UsdCrate>();
        std::string cwhy;
        if (!s->cr->loadMemory(std::move(bytes), &cwhy)) { fail(why, path + ": " + cwhy); return -1; }
        UsdCrateValue v;
        const i32 rootSpec = s->cr->specIndex("/");
        if (s->cr->field(rootSpec, "metersPerUnit", v) && v.numberCount()) s->metersPerUnit = static_cast<f32>(v.number());
        if (s->cr->field(rootSpec, "upAxis", v) && v.str()) { s->upAxis = *v.str(); s->yUp = s->upAxis != "Z"; }
        if (s->cr->field(rootSpec, "defaultPrim", v) && v.str()) s->defaultPrim = *v.str();
        if (s->cr->field(rootSpec, "subLayers", v)) s->subLayers = v.s;
    } else if (enc == UsdEncoding::Usda) {
        // Walked once, with the text reader's own walk, collecting meshes unbuilt: the stage decides
        // their transforms. Materials ARE built here, into the stage's result, under this layer's
        // prefix and with its directory for texture paths.
        const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        Ctx& c = st.c;
        const std::string saveDir = c.baseDir, savePrefix = c.pathPrefix;
        c.baseDir = s->dir;
        c.pathPrefix = s->prefix;
        c.rawSink = &s->raw;
        c.primWorlds = &s->worlds;
        c.cameraPaths = &s->cameraPaths;
        c.lights = &s->lights;
        UsdaHeader hdr;
        parseUsdaText(text, c, hdr, /*applyUnits=*/false);
        c.rawSink = nullptr;
        c.primWorlds = nullptr;
        c.cameraPaths = nullptr;
        c.lights = nullptr;
        c.baseDir = saveDir;
        c.pathPrefix = savePrefix;
        s->metersPerUnit = hdr.metersPerUnit;
        s->yUp = hdr.yUp;
        s->upAxis = hdr.upAxis;
        s->defaultPrim = hdr.defaultPrim;
        s->subLayers = hdr.subLayers;
    } else if (enc == UsdEncoding::Usdz) {
        fail(why, "USD: " + path + " is a USDZ archive, which is not unpacked -- extract it and import the layer inside");
        return -1;
    } else {
        fail(why, "USD: " + path + " is not a USD layer");
        return -1;
    }
    if (!s->defaultPrim.empty() && s->defaultPrim[0] != '/') s->defaultPrim = "/" + s->defaultPrim;

    const i32 idx = static_cast<i32>(st.sources.size());
    st.sources.push_back(std::move(s));
    st.sourceByKey.emplace(key, idx);
    return idx;
}

// UsdImportResult::meshGroups for geometry from `src`: its folder's name.
std::string groupOf(const Source& src) {
    return std::filesystem::path(src.dir).filename().string();
}

// The same for a prim in a crate stack: the folder of the strongest layer that has a spec for it.
std::string groupOf(const Stage& st, const Stack& sk, const std::string& path) {
    for (const i32 li : sk.layers)
        if (st.sources[static_cast<usize>(li)]->cr->specIndex(path) >= 0) return groupOf(*st.sources[static_cast<usize>(li)]);
    return {};
}

void setGroup(UsdImportResult& out, usize mesh, const std::string& group) {
    if (out.meshGroups.size() <= mesh) out.meshGroups.resize(mesh + 1);
    out.meshGroups[mesh] = group;
}

Stack* stackForCrate(Stage& st, i32 src) {
    if (const auto it = st.singleStack.find(src); it != st.singleStack.end()) return it->second;
    st.stacks.push_back(Stack{{src}, st.sources[static_cast<usize>(src)]->prefix, false});
    Stack* s = &st.stacks.back();
    st.singleStack.emplace(src, s);
    return s;
}

// ---- composed queries over a crate stack -------------------------------------------------------

// The strongest layer's opinion for `name` on the spec at `path`. `from`, when given, receives the
// source that authored it -- a reference's asset path is relative to THAT layer.
bool fieldOf(Stage& st, const Stack& sk, const std::string& path, const char* name, UsdCrateValue& v,
             i32* from = nullptr) {
    for (const i32 li : sk.layers) {
        const UsdCrate& cr = *st.sources[static_cast<usize>(li)]->cr;
        const i32 spec = cr.specIndex(path);
        if (spec < 0 || !cr.hasField(spec, name)) continue;
        std::string why;
        if (!cr.field(spec, name, v, &why)) {
            if (why.find("time-sampled") != std::string::npos) st.c.sawTimeSamples = true;
            else st.note("a field this importer does not decode was skipped (" + std::string(name) + " on " + path + ": " + why + ")");
            return false;
        }
        if (from) *from = li;
        return true;
    }
    return false;
}

// An attribute's default value. A time-sampled attribute with no default counts as absent.
bool attr(Stage& st, const Stack& sk, const std::string& prim, const std::string& prop, UsdCrateValue& v) {
    const std::string ap = prim + "." + prop;
    if (fieldOf(st, sk, ap, "default", v)) return true;
    for (const i32 li : sk.layers) {
        const UsdCrate& cr = *st.sources[static_cast<usize>(li)]->cr;
        const i32 spec = cr.specIndex(ap);
        if (spec >= 0 && cr.hasField(spec, "timeSamples")) { st.c.sawTimeSamples = true; break; }
    }
    return false;
}

std::string tokenOf(Stage& st, const Stack& sk, const std::string& path, const char* name) {
    UsdCrateValue v;
    return fieldOf(st, sk, path, name, v) && v.str() ? *v.str() : std::string();
}

std::vector<std::string> childrenOf(Stage& st, const Stack& sk, const std::string& path) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const i32 li : sk.layers) {
        const UsdCrate& cr = *st.sources[static_cast<usize>(li)]->cr;
        UsdCrateValue v;
        if (!cr.field(path, "primChildren", v)) continue;
        for (std::string& n : v.s) if (seen.insert(n).second) out.push_back(std::move(n));
    }
    return out;
}

std::string childPath(const std::string& parent, const std::string& name) {
    return parent == "/" ? "/" + name : parent + "/" + name;
}

// def (0), over (1) or class (2), composed: defined anywhere wins, then class, then over.
int specifierOf(Stage& st, const Stack& sk, const std::string& path) {
    bool anyClass = false, anyOver = false;
    for (const i32 li : sk.layers) {
        const UsdCrate& cr = *st.sources[static_cast<usize>(li)]->cr;
        UsdCrateValue v;
        if (!cr.field(path, "specifier", v) || v.i.empty()) continue;
        if (v.i[0] == 0) return 0;
        if (v.i[0] == 2) anyClass = true; else anyOver = true;
    }
    return anyClass ? 2 : anyOver ? 1 : 0;
}

bool isActive(Stage& st, const Stack& sk, const std::string& path) {
    UsdCrateValue v;
    return !(fieldOf(st, sk, path, "active", v) && !v.i.empty() && v.i[0] == 0);
}

// Guide/proxy purpose and invisible prims (collision, occlusion, lighting volumes) are not rendered geometry.
bool hiddenPrim(Stage& st, const Stack& sk, const std::string& path) {
    UsdCrateValue v;
    if (attr(st, sk, path, "purpose", v) && v.str() && (*v.str() == "guide" || *v.str() == "proxy")) {
        st.note("prims with purpose guide/proxy were skipped");
        return true;
    }
    if (attr(st, sk, path, "visibility", v) && v.str() && *v.str() == "invisible") {
        st.note("invisible prims were skipped");
        return true;
    }
    return false;
}

// "/p{set=sel}" for each variant selection authored on `path`: the specs a selected variant adds to the prim.
std::vector<std::string> selectedVariants(Stage& st, const Stack& sk, const std::string& path) {
    UsdCrateValue v;
    std::vector<std::string> out;
    if (!fieldOf(st, sk, path, "variantSelection", v)) return out;
    for (usize k = 0; k + 1 < v.s.size(); k += 2)
        if (!v.s[k + 1].empty()) out.push_back(path + "{" + v.s[k] + "=" + v.s[k + 1] + "}");
    return out;
}

std::vector<std::string> relTargets(Stage& st, const Stack& sk, const std::string& prim, const std::string& rel) {
    UsdCrateValue v;
    return fieldOf(st, sk, prim + "." + rel, "targetPaths", v) ? v.s : std::vector<std::string>{};
}

// "/Looks/Bark/Albedo.outputs:rgb" -> "/Looks/Bark/Albedo", and 'r'/'g'/'b'/'a' for a single channel.
std::string stripOutput(const std::string& conn, char* channel) {
    if (channel) *channel = 0;
    const usize dot = conn.find(".outputs:");
    if (dot == std::string::npos) return conn;
    const std::string out = conn.substr(dot + 9);
    if (channel && out.size() == 1 && (out[0] == 'r' || out[0] == 'g' || out[0] == 'b' || out[0] == 'a'))
        *channel = out[0];
    return conn.substr(0, dot);
}

// ---- transforms --------------------------------------------------------------------------------

M4 quatRow(f32 x, f32 y, f32 z, f32 w) {
    // Row-vector rotation (v * R rotates v by the quaternion), the same form as aver::Mat4::fromQuat
    // and USD's GfMatrix4d::SetRotate.
    const f32 n = std::sqrt(x * x + y * y + z * z + w * w);
    if (n > 1e-12f) { x /= n; y /= n; z /= n; w /= n; }
    M4 r = M4::identity();
    r.m[0] = 1 - 2 * (y * y + z * z); r.m[1] = 2 * (x * y + w * z);     r.m[2]  = 2 * (x * z - w * y);
    r.m[4] = 2 * (x * y - w * z);     r.m[5] = 1 - 2 * (x * x + z * z); r.m[6]  = 2 * (y * z + w * x);
    r.m[8] = 2 * (x * z + w * y);     r.m[9] = 2 * (y * z - w * x);     r.m[10] = 1 - 2 * (x * x + y * y);
    return r;
}

// A prim's own xformOps, in xformOpOrder. `has` says whether it authored any.
//
// ORDER: USD lists ops outermost first and applies them to COLUMN vectors, so [translate, rotate,
// scale] means "scale, then rotate, then translate". In this row-vector convention that is
// S * R * T -- the list multiplied in reverse.
M4 localXform(Stage& st, const Stack& sk, const std::string& path, bool& has) {
    has = false;
    UsdCrateValue order;
    if (!attr(st, sk, path, "xformOpOrder", order) || order.s.empty()) return M4::identity();
    has = true;
    M4 m = M4::identity();
    for (usize k = order.s.size(); k-- > 0;) {
        std::string op = order.s[k];
        bool invert = false;
        if (op.rfind("!invert!", 0) == 0) { invert = true; op = op.substr(8); }
        if (op == "!resetXformStack!") { st.note("xformOp !resetXformStack! was ignored"); continue; }
        UsdCrateValue v;
        if (!attr(st, sk, path, op, v)) continue;
        const std::vector<f32> n = v.asFloats();
        // The op KIND is the second namespace component: "xformOp:rotateXYZ:pivot" is a rotateXYZ.
        std::string kind = op.size() > 8 ? op.substr(8) : std::string();
        if (const usize colon = kind.find(':'); colon != std::string::npos) kind = kind.substr(0, colon);
        M4 om = M4::identity();
        if (kind == "translate" && n.size() >= 3)      om = translate(n[0], n[1], n[2]);
        else if (kind == "scale" && n.size() >= 3) {
            if (std::fabs(n[0] - n[1]) > 1e-6f || std::fabs(n[0] - n[2]) > 1e-6f) st.c.sawNonUniformScale = true;
            om = scaleM(n[0], n[1], n[2]);
        }
        else if (kind == "scale" && n.size() == 1)     om = scaleM(n[0], n[0], n[0]);
        else if (kind == "rotateX" && !n.empty())      om = rotateAxis(0, n[0]);
        else if (kind == "rotateY" && !n.empty())      om = rotateAxis(1, n[0]);
        else if (kind == "rotateZ" && !n.empty())      om = rotateAxis(2, n[0]);
        else if (kind.rfind("rotate", 0) == 0 && kind.size() == 9 && n.size() >= 3) {
            // rotateXYZ: X first, then Y, then Z -- row-vector Rx * Ry * Rz. Any other order the same way.
            for (int a = 0; a < 3; ++a) {
                const char axis = kind[6 + a];
                const int ai = axis == 'X' ? 0 : axis == 'Y' ? 1 : 2;
                om = mul(om, rotateAxis(ai, n[static_cast<usize>(ai)]));
            }
        }
        else if (kind == "orient" && n.size() >= 4)    om = quatRow(n[0], n[1], n[2], n[3]);
        else if (kind == "transform" && n.size() >= 16) for (int i = 0; i < 16; ++i) om.m[i] = n[static_cast<usize>(i)];
        else { st.note("xformOp '" + op + "' is not a kind this importer applies; it was ignored"); continue; }
        if (invert) om = inverseAffine(om);
        m = mul(m, om);
    }
    return m;
}

// ---- meshes from a crate stack -----------------------------------------------------------------

void gatherMesh(Stage& st, const Stack& sk, const std::string& path, RawMesh& out) {
    out.path = path;
    MeshAttrs& a = out.attrs;
    UsdCrateValue v;
    if (attr(st, sk, path, "points", v)) { a.points = v.asFloats(); a.hasPoints = a.points.size() >= 9; }
    if (attr(st, sk, path, "faceVertexCounts", v))  a.faceVertexCounts = v.asInts();
    if (attr(st, sk, path, "faceVertexIndices", v)) a.faceVertexIndices = v.asInts();
    if (attr(st, sk, path, "normals", v)) {
        a.normals = v.asFloats();
        a.normalsInterp = tokenOf(st, sk, path + ".normals", "interpolation");
    }
    for (const char* uvName : {"primvars:st", "primvars:st0", "primvars:UVMap", "primvars:uv"}) {
        if (!attr(st, sk, path, uvName, v)) continue;
        a.uvs = v.asFloats();
        a.uvInterp = tokenOf(st, sk, path + "." + uvName, "interpolation");
        // An INDEXED primvar stores each distinct value once plus an index per element; the mesh
        // builder wants the element-order values, so they are expanded here.
        UsdCrateValue idx;
        if (attr(st, sk, path, std::string(uvName) + ":indices", idx)) {
            const std::vector<i32> ix = idx.asInts();
            std::vector<f32> flat(ix.size() * 2, 0.0f);
            for (usize k = 0; k < ix.size(); ++k)
                if (ix[k] >= 0 && static_cast<usize>(ix[k]) * 2 + 1 < a.uvs.size()) {
                    flat[k * 2]     = a.uvs[static_cast<usize>(ix[k]) * 2];
                    flat[k * 2 + 1] = a.uvs[static_cast<usize>(ix[k]) * 2 + 1];
                }
            a.uvs = std::move(flat);
        }
        break;
    }
    a.subdivisionScheme = tokenOf(st, sk, path + ".subdivisionScheme", "default");
    if (!a.subdivisionScheme.empty() && a.subdivisionScheme != "none") st.c.sawSubdiv = true;
    a.orientation = tokenOf(st, sk, path + ".orientation", "default");
    if (attr(st, sk, path, "doubleSided", v) && !v.i.empty()) a.doubleSided = v.i[0] != 0;
    if (attr(st, sk, path, "primvars:sharp_face", v)) a.sharpFace = v.asInts();
    const std::vector<std::string> bind = relTargets(st, sk, path, "material:binding");
    if (!bind.empty()) a.materialBinding = bind[0];

    for (const std::string& name : childrenOf(st, sk, path)) {
        const std::string sp = childPath(path, name);
        if (tokenOf(st, sk, sp, "typeName") != "GeomSubset") continue;
        if (tokenOf(st, sk, sp + ".elementType", "default") != "face") continue;
        const std::vector<std::string> sb = relTargets(st, sk, sp, "material:binding");
        // An unbound materialBind subset still names its material (Caldera); kept so a material map can use it.
        if (sb.empty() && tokenOf(st, sk, sp + ".familyName", "default") != "materialBind") continue;
        GeomSubsetDef g;
        g.name = name;
        if (!sb.empty()) g.binding = sb[0];
        if (attr(st, sk, sp, "indices", v)) g.faces = v.asInts();
        if (!g.faces.empty()) out.subsets.push_back(std::move(g));
    }
}

const RawMesh* gatherCached(Stage& st, const Stack& sk, const std::string& path) {
    const std::string key = sk.prefix + path;
    if (const auto it = st.rawByKey.find(key); it != st.rawByKey.end()) return it->second;
    st.rawPool.emplace_back();
    RawMesh& rm = st.rawPool.back();
    gatherMesh(st, sk, path, rm);
    rm.world = M4::identity();
    st.rawByKey.emplace(key, &rm);
    return &rm;
}

// ---- materials from a crate stack --------------------------------------------------------------

std::string numbersText(const UsdCrateValue& v) {
    std::string s;
    char buf[32];
    const usize n = v.numberCount();
    for (usize k = 0; k < n; ++k) {
        std::snprintf(buf, sizeof buf, "%.9g", v.number(k));
        if (k) s += ' ';
        s += buf;
    }
    return s;
}

void collectCrateShader(Stage& st, const Stack& sk, const std::string& path, ShaderPrim& sh) {
    sh.path = path;
    sh.id = tokenOf(st, sk, path + ".info:id", "default");
    UsdCrateValue props;
    std::vector<std::string> names;
    std::unordered_set<std::string> seen;
    for (const i32 li : sk.layers) {
        const UsdCrate& cr = *st.sources[static_cast<usize>(li)]->cr;
        if (!cr.field(path, "properties", props)) continue;
        for (std::string& n : props.s) if (seen.insert(n).second) names.push_back(std::move(n));
    }
    for (const std::string& prop : names) {
        if (prop.rfind("inputs:", 0) != 0) continue;
        const std::string input = prop.substr(7);
        const std::string ap = path + "." + prop;
        UsdCrateValue v;
        if (fieldOf(st, sk, ap, "connectionPaths", v) && !v.s.empty()) {
            char ch = 0;
            const std::string target = stripOutput(v.s[0], &ch);
            sh.connects.emplace_back(input, target);
            sh.connectOuts.emplace_back(input, ch);
            continue;
        }
        if (!fieldOf(st, sk, ap, "default", v)) continue;
        std::string text;
        if (!v.s.empty()) {
            text = v.s[0];
            if (input == "file") sh.file = text;
        } else {
            text = numbersText(v);
        }
        sh.values.emplace_back(input, std::move(text));
    }
}

void collectCrateShaders(Stage& st, const Stack& sk, const std::string& path, std::vector<ShaderPrim>& out, int depth) {
    for (const std::string& name : childrenOf(st, sk, path)) {
        const std::string cp = childPath(path, name);
        const std::string type = tokenOf(st, sk, cp, "typeName");
        if (type == "Shader") {
            ShaderPrim sh;
            collectCrateShader(st, sk, cp, sh);
            out.push_back(std::move(sh));
        } else if (depth < 2) {
            collectCrateShaders(st, sk, cp, out, depth + 1);
        }
    }
}

// Every Material prim in the stack, through the text reader's own buildMaterial. Texture paths
// resolve against the directory of the layer that DEFINES the material.
void registerMaterials(Stage& st, Stack& sk) {
    if (sk.materialsRegistered) return;
    sk.materialsRegistered = true;
    std::unordered_set<std::string> done;
    for (const i32 li : sk.layers) {
        const Source& src = *st.sources[static_cast<usize>(li)];
        const UsdCrate& cr = *src.cr;
        for (i32 k = 0; k < static_cast<i32>(cr.specCount()); ++k) {
            if (cr.specType(k) != UsdSpecType::Prim) continue;
            UsdCrateValue tn;
            if (!cr.field(k, "typeName", tn) || !tn.str() || *tn.str() != "Material") continue;
            const std::string& path = cr.specPath(k);
            if (!done.insert(path).second) continue;
            std::vector<ShaderPrim> shaders;
            collectCrateShaders(st, sk, path, shaders, 0);
            UsdCrateValue surf;
            std::string surfacePath;
            if (fieldOf(st, sk, path + ".outputs:surface", "connectionPaths", surf) && !surf.s.empty())
                surfacePath = stripOutput(surf.s[0], nullptr);
            st.c.sawMaterial = true;
            st.c.baseDir = src.dir;
            st.c.pathPrefix = sk.prefix;
            buildMaterial(st.c, path, shaders, surfacePath);
        }
    }
    st.c.pathPrefix.clear();
}

// ---- composition arcs --------------------------------------------------------------------------

// Where a reference points: an internal one stays in this stack; an external one opens its layer.
bool resolveRef(Stage& st, const Loc& from, i32 authoringSource, const UsdCrateRef& ref, Loc& out) {
    if (ref.assetPath.empty()) {
        out = from;
        out.path = ref.primPath;
        if (out.path.empty() && from.stack)
            out.path = st.sources[static_cast<usize>(from.stack->layers.front())]->defaultPrim;
        return !out.path.empty();
    }
    const std::string baseDir = authoringSource >= 0 ? st.sources[static_cast<usize>(authoringSource)]->dir : std::string();
    const std::string full = (std::filesystem::path(baseDir) / ref.assetPath).lexically_normal().string();
    std::string why;
    const i32 idx = loadSource(st, full, &why);
    if (idx < 0) { st.note("a referenced layer could not be opened (" + why + "); what it held did not import"); return false; }
    const Source& s = *st.sources[static_cast<usize>(idx)];
    out = Loc{};
    out.path = ref.primPath.empty() ? s.defaultPrim : ref.primPath;
    if (out.path.empty()) { st.note("a reference to " + ref.assetPath + " named no prim and the layer has no defaultPrim"); return false; }
    if (s.crate) out.stack = stackForCrate(st, idx);
    else         out.text = idx;
    return true;
}

// The prim's local transform as composed, strongest opinion first: its own ops, else an inherited
// class's (which may itself come through the class's reference), else a reference target's. A text
// layer's prim: its world relative to its parent's. False when nothing along the way authored one.
bool composedLocal(Stage& st, const Loc& loc, M4& out, int depth) {
    out = M4::identity();
    if (depth > 32) return false;
    if (loc.text >= 0) {
        const Source& s = *st.sources[static_cast<usize>(loc.text)];
        const auto self = s.worlds.find(loc.path);
        if (self == s.worlds.end()) return false;
        const auto par = s.worlds.find(parentPath(loc.path));
        out = par == s.worlds.end() ? self->second : mul(self->second, inverseAffine(par->second));
        return true;
    }
    bool has = false;
    out = localXform(st, *loc.stack, loc.path, has);
    if (has) return true;
    UsdCrateValue v;
    if (fieldOf(st, *loc.stack, loc.path, "inheritPaths", v))
        for (const std::string& cls : v.s)
            if (composedLocal(st, Loc{loc.stack, -1, cls}, out, depth + 1)) return true;
    i32 from = -1;
    if (fieldOf(st, *loc.stack, loc.path, "references", v, &from))
        for (const UsdCrateRef& ref : v.refs) {
            Loc t;
            if (resolveRef(st, loc, from, ref, t) && composedLocal(st, t, out, depth + 1)) return true;
        }
    out = M4::identity();
    return false;
}

M4 composedLocal(Stage& st, const Loc& loc) {
    M4 m;
    composedLocal(st, loc, m, 0);
    return m;
}

// Everything geometric INSIDE a prim -- its descendants and whatever its arcs bring -- expressed in
// the prim's own space and carried by `toOut`. The prim's own transform is the caller's to apply.
void contents(Stage& st, const Loc& loc, const M4& toOut, std::vector<Piece>& out, int depth) {
    if (depth > 64) { st.note("composition nested deeper than 64 levels; the rest was not followed"); return; }
    if (loc.text >= 0) {
        const Source& s = *st.sources[static_cast<usize>(loc.text)];
        const auto self = s.worlds.find(loc.path);
        if (self == s.worlds.end()) { st.note("a reference named " + loc.path + ", which " + s.path + " does not define"); return; }
        const M4 base = inverseAffine(self->second);
        for (const RawMesh& rm : s.raw)
            if (under(rm.path, loc.path)) out.push_back(Piece{&rm, mul(mul(rm.world, base), toOut), s.prefix, groupOf(s)});
        return;
    }
    Stack& sk = *loc.stack;
    if (hiddenPrim(st, sk, loc.path)) return;
    if (tokenOf(st, sk, loc.path, "typeName") == "Mesh")
        out.push_back(Piece{gatherCached(st, sk, loc.path), toOut, sk.prefix, groupOf(st, sk, loc.path)});

    UsdCrateValue v;
    if (fieldOf(st, sk, loc.path, "inheritPaths", v))
        for (const std::string& cls : v.s) contents(st, Loc{loc.stack, -1, cls}, toOut, out, depth + 1);
    i32 from = -1;
    if (fieldOf(st, sk, loc.path, "references", v, &from))
        for (const UsdCrateRef& ref : v.refs) {
            Loc t;
            if (resolveRef(st, loc, from, ref, t)) contents(st, t, toOut, out, depth + 1);
        }
    if (fieldOf(st, sk, loc.path, "payload", v, &from))
        for (const UsdCrateRef& ref : v.refs) {
            Loc t;
            if (resolveRef(st, loc, from, ref, t)) contents(st, t, toOut, out, depth + 1);
        }
    const std::vector<std::string> variants = selectedVariants(st, sk, loc.path);
    for (const std::string& vp : variants) contents(st, Loc{loc.stack, -1, vp}, toOut, out, depth + 1);
    for (const i32 li : sk.layers) {
        const UsdCrate& cr = *st.sources[static_cast<usize>(li)]->cr;
        const i32 spec = cr.specIndex(loc.path);
        if (spec < 0) continue;
        if (cr.hasField(spec, "specializes")) st.note("specializes arcs were not composed");
        if (cr.hasField(spec, "variantSetNames") && variants.empty()) st.c.sawVariant = true;
    }

    for (const std::string& name : childrenOf(st, sk, loc.path)) {
        const std::string cp = childPath(loc.path, name);
        if (specifierOf(st, sk, cp) == 2 || !isActive(st, sk, cp) || hiddenPrim(st, sk, cp)) continue;
        const std::string type = tokenOf(st, sk, cp, "typeName");
        if (type == "Material" || type == "Shader" || type == "GeomSubset") continue;
        if (type == "PointInstancer") { st.note("a PointInstancer nested inside a prototype was not expanded"); continue; }
        const Loc child{loc.stack, -1, cp};
        contents(st, child, mul(composedLocal(st, child), toOut), out, depth + 1);
    }
}

// ---- prototypes --------------------------------------------------------------------------------

// Appends `src` onto `dst` as further submeshes -- AverAssetC's mergeAll rule: indices rebased,
// each submesh's baseVertex set to where its vertices now start.
void appendMesh(OcMeshData& dst, OcMeshData&& src) {
    const u32 base = dst.vertexCount();
    const u32 firstIndex = static_cast<u32>(dst.indices.size());
    const u32 slotBase = static_cast<u32>(dst.materialSlots.size());
    dst.positions.insert(dst.positions.end(), src.positions.begin(), src.positions.end());
    dst.normals.insert(dst.normals.end(), src.normals.begin(), src.normals.end());
    dst.uvs.insert(dst.uvs.end(), src.uvs.begin(), src.uvs.end());
    for (const u32 idx : src.indices) dst.indices.push_back(idx + base);
    dst.materialSlots.insert(dst.materialSlots.end(), src.materialSlots.begin(), src.materialSlots.end());
    for (OcMeshSubmesh sm : src.submeshes) {
        sm.materialSlot += slotBase;
        sm.indexStart   += firstIndex;
        sm.baseVertex   += base;
        dst.submeshes.push_back(std::move(sm));
    }
    dst.boundsMin = Vec3{std::min(dst.boundsMin.x, src.boundsMin.x), std::min(dst.boundsMin.y, src.boundsMin.y),
                         std::min(dst.boundsMin.z, src.boundsMin.z)};
    dst.boundsMax = Vec3{std::max(dst.boundsMax.x, src.boundsMax.x), std::max(dst.boundsMax.y, src.boundsMax.y),
                         std::max(dst.boundsMax.z, src.boundsMax.z)};
}

// Builds the prototype at `loc` as ONE mesh, in the prototype prim's PARENT space (its own transform
// baked in), or returns the one already built from identical pieces. -1 when it has no geometry.
i32 buildProto(Stage& st, const Loc& loc) {
    const M4 local = composedLocal(st, loc);
    std::vector<Piece> pieces;
    contents(st, loc, local, pieces, 0);
    if (pieces.empty()) return -1;

    // Identity is the geometry it would build: the same meshes under the same matrices. Two
    // instancers whose prototypes inherit the same class share one mesh this way.
    std::string key;
    key.reserve(pieces.size() * 80);
    for (const Piece& p : pieces) {
        key.append(reinterpret_cast<const char*>(&p.mesh), sizeof(p.mesh));
        key.append(reinterpret_cast<const char*>(p.rel.m), sizeof(p.rel.m));
    }
    if (const auto it = st.protoByKey.find(key); it != st.protoByKey.end()) return it->second;

    Ctx& c = st.c;
    UsdImportResult& out = *c.out;
    const usize first = out.meshes.size();
    for (const Piece& p : pieces) {
        c.pathPrefix = p.prefix;
        buildMesh(c, p.mesh->attrs, p.rel, p.mesh->path, p.mesh->subsets);
    }
    c.pathPrefix.clear();
    if (out.meshes.size() == first) return -1;

    // One mesh per prototype: a plant made of a trunk mesh and a leaf mesh is still ONE placement
    // per instance.
    for (usize k = first + 1; k < out.meshes.size(); ++k) {
        appendMesh(out.meshes[first], std::move(out.meshes[k]));
        c.meshBinding[first].insert(c.meshBinding[first].end(), c.meshBinding[k].begin(), c.meshBinding[k].end());
        c.slotDoubleSided[first].insert(c.slotDoubleSided[first].end(), c.slotDoubleSided[k].begin(),
                                        c.slotDoubleSided[k].end());
    }
    out.meshes.resize(first + 1);
    out.meshNames.resize(first + 1);
    c.meshBinding.resize(first + 1);
    c.slotDoubleSided.resize(first + 1);
    // Named for the prim the geometry came from -- "Anthurium_01_Translucent", not the instancer's
    // "/World/X/Prototypes/X" bookkeeping.
    const std::string& srcPath = pieces.front().mesh->path;
    std::string name = leafOf(loc.path);
    if (pieces.size() == 1 && !srcPath.empty()) {
        // A single-mesh prototype: its mesh's parent is the plant, the mesh itself is often named the same.
        name = leafOf(parentPath(srcPath)) == "root" ? leafOf(srcPath) : leafOf(parentPath(srcPath));
    }
    out.meshNames[first] = name;
    setGroup(out, first, pieces.front().group);

    Proto pr;
    pr.meshIndex = static_cast<i32>(first);
    const OcMeshData& m = out.meshes[first];
    pr.tris = m.indices.size() / 3;
    const Vec3 e{m.boundsMax.x - m.boundsMin.x, m.boundsMax.y - m.boundsMin.y, m.boundsMax.z - m.boundsMin.z};
    pr.radius = std::max(1e-3f, 0.5f * std::sqrt(e.x * e.x + e.y * e.y + e.z * e.z));
    const i32 idx = static_cast<i32>(st.protos.size());
    st.protos.push_back(pr);
    st.protoByKey.emplace(std::move(key), idx);
    return idx;
}

// ---- the walk over the root stack --------------------------------------------------------------

void toEngineV(const Ctx& c, const f32 in[3], f32 out[3]) {
    if (!c.opt->convertAxes) { out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; return; }
    if (c.yUp) { out[0] = -in[2]; out[1] = in[0]; out[2] = in[1]; }
    else       { out[0] = in[0];  out[1] = -in[1]; out[2] = in[2]; }
}

// A placement for `meshIndex` at USD-space row-vector matrix `m`: translation through the same
// conversion a vertex takes, and the basis moved into engine space (C^-1 * B * C for the basis
// change C) and decomposed into the rotation and scale a level record carries.
UsdPlacement placementFor(const Ctx& c, i32 meshIndex, const M4& m) {
    UsdPlacement pl;
    pl.meshIndex = meshIndex;
    f32 t[3] = {m.m[12], m.m[13], m.m[14]}, te[3];
    toEngineV(c, t, te);
    pl.position = Vec3{te[0] * c.unitScale, te[1] * c.unitScale, te[2] * c.unitScale};

    f32 b[3][3];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) b[i][j] = m.m[i * 4 + j];
    f32 e[3][3];
    if (!c.opt->convertAxes) {
        std::memcpy(e, b, sizeof e);
    } else {
        // C as a row-vector map (engine = usd * C). Z-up: diag(1,-1,1). Y-up: {-z, x, y}.
        f32 C[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, 1}};
        if (c.yUp) { const f32 Y[3][3] = {{0, 1, 0}, {0, 0, 1}, {-1, 0, 0}}; std::memcpy(C, Y, sizeof C); }
        f32 tmp[3][3] = {};
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) tmp[i][j] += b[i][k] * C[k][j];
        // C is orthonormal, so C^-1 is its transpose.
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { e[i][j] = 0; for (int k = 0; k < 3; ++k) e[i][j] += C[k][i] * tmp[k][j]; }
    }
    Mat4 em = Mat4::identity();
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) em.m[i][j] = e[i][j];
    const Transform tr = transformFromMatrix(em);
    pl.rotation[0] = tr.rotation.x; pl.rotation[1] = tr.rotation.y;
    pl.rotation[2] = tr.rotation.z; pl.rotation[3] = tr.rotation.w;
    pl.scale = tr.scale;
    return pl;
}

// A UsdGeomCamera as a viewpoint. USD cameras look down their local -Z: as a row vector through the
// basis, that is minus the matrix's third row.
UsdCameraPose cameraPose(const Ctx& c, const std::string& name, const M4& world) {
    UsdCameraPose cp;
    cp.name = name;
    f32 t[3] = {world.m[12], world.m[13], world.m[14]}, te[3];
    toEngineV(c, t, te);
    cp.position = Vec3{te[0] * c.unitScale, te[1] * c.unitScale, te[2] * c.unitScale};
    f32 f[3] = {-world.m[8], -world.m[9], -world.m[10]}, fe[3];
    toEngineV(c, f, fe);
    const f32 len = std::sqrt(fe[0] * fe[0] + fe[1] * fe[1] + fe[2] * fe[2]);
    if (len < 1e-12f) return cp;
    constexpr f32 kDeg = 57.29577951308232f;
    cp.yawDeg = std::atan2(fe[1], fe[0]) * kDeg;
    cp.pitchDeg = std::asin(std::fmax(-1.0f, std::fmin(1.0f, fe[2] / len))) * kDeg;
    return cp;
}

// ---- lights: the stage's sun -------------------------------------------------------------------

bool excluded(Stage& st, const std::string& path) {
    for (const std::string& ex : st.c.opt->excludePrims)
        if (!ex.empty() && under(path, ex)) { st.excludeHits.insert(ex); return true; }
    return false;
}

// A Radiance .hdr (RGBE, flat or new-style run-length scanlines) reduced to per-texel luminance.
bool loadRadianceLuminance(const std::string& path, u32& w, u32& h, std::vector<f32>& lum, std::string& why) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { why = "cannot open " + path; return false; }
    const std::streamoff n = f.tellg();
    std::vector<u8> d(static_cast<usize>(std::max<std::streamoff>(n, 0)));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(d.data()), n);
    if (!f || d.size() < 16 || std::memcmp(d.data(), "#?", 2) != 0) { why = path + " is not a Radiance .hdr"; return false; }

    // Header lines end at a blank line; the next line is the resolution, "-Y <h> +X <w>".
    usize p = 0;
    const auto line = [&](std::string& out) {
        out.clear();
        while (p < d.size() && d[p] != '\n') out.push_back(static_cast<char>(d[p++]));
        if (p < d.size()) ++p;
        return p <= d.size();
    };
    std::string ln;
    bool rgbe = false;
    for (;;) {
        if (p >= d.size()) { why = path + ": truncated header"; return false; }
        line(ln);
        if (ln.empty()) break;
        if (ln.find("FORMAT=32-bit_rle_rgbe") != std::string::npos) rgbe = true;
    }
    line(ln);
    std::istringstream res(ln);
    std::string ya, xa;
    unsigned hh = 0, ww = 0;
    res >> ya >> hh >> xa >> ww;
    if (!rgbe || !res || ya != "-Y" || xa != "+X" || !ww || !hh || ww > 65536 || hh > 65536) {
        why = path + ": only top-down, left-to-right RGBE .hdr files are read";
        return false;
    }
    w = ww; h = hh;
    lum.assign(static_cast<usize>(w) * h, 0.0f);
    std::vector<u8> row(static_cast<usize>(w) * 4);
    for (u32 y = 0; y < h; ++y) {
        if (w >= 8 && w < 32768 && p + 4 <= d.size() && d[p] == 2 && d[p + 1] == 2 && (d[p + 2] & 0x80) == 0) {
            // New-style RLE: four planes (R, G, B, E), each run-length coded separately.
            p += 4;
            for (u32 ch = 0; ch < 4; ++ch) {
                u32 x = 0;
                while (x < w) {
                    if (p >= d.size()) { why = path + ": truncated scanline"; return false; }
                    u32 count = d[p++];
                    if (count > 128) {
                        count -= 128;
                        if (p >= d.size() || x + count > w) { why = path + ": bad run"; return false; }
                        const u8 v = d[p++];
                        for (u32 k = 0; k < count; ++k) row[(x++) * 4 + ch] = v;
                    } else {
                        if (count == 0 || p + count > d.size() || x + count > w) { why = path + ": bad literal"; return false; }
                        for (u32 k = 0; k < count; ++k) row[(x++) * 4 + ch] = d[p++];
                    }
                }
            }
        } else {
            if (p + static_cast<usize>(w) * 4 > d.size()) { why = path + ": truncated flat scanline"; return false; }
            std::memcpy(row.data(), d.data() + p, static_cast<usize>(w) * 4);
            p += static_cast<usize>(w) * 4;
        }
        for (u32 x = 0; x < w; ++x) {
            const u8* px = &row[static_cast<usize>(x) * 4];
            if (!px[3]) continue;
            const f32 scale = std::ldexp(1.0f, static_cast<int>(px[3]) - 136);
            lum[static_cast<usize>(y) * w + x] = (0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2]) * scale;
        }
    }
    return true;
}

// The unit direction, in the DOME's own frame (Y is its pole), of latlong texel (x, y) -- see
// UsdSun for why this orientation.
void latLongDir(u32 x, u32 y, u32 w, u32 h, f32 out[3]) {
    const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(w);
    const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(h);
    const f32 lon = (u - 0.5f) * 6.2831853f;
    const f32 lat = (0.5f - v) * 3.14159265f;
    out[0] = std::sin(lon) * std::cos(lat);
    out[1] = std::sin(lat);
    out[2] = std::cos(lon) * std::cos(lat);
}

// A direction in a light prim's own frame -> engine space, TOWARD the sun.
bool lightDirToEngine(const Stage& st, const M4& world, const f32 local[3], Vec3& out) {
    f32 s[3];
    for (int j = 0; j < 3; ++j) s[j] = local[0] * world.m[j] + local[1] * world.m[4 + j] + local[2] * world.m[8 + j];
    f32 e[3];
    toEngineV(st.c, s, e);
    const f32 len = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
    if (!(len > 1e-8f)) return false;
    out = Vec3{e[0] / len, e[1] / len, e[2] / len};
    return true;
}

// A DomeLight's sun: the luminance-weighted mean direction of its texture's brightest texels, when
// the texture HAS a sun -- a peak at least 20x the sky's mean, so an overcast or studio dome does not
// invent one.
void domeSun(Stage& st, const std::string& path, const M4& world, const std::string& textureFile,
             const std::string& layerDir, const std::string& poleAxis, bool orientOp) {
    if (st.sunDome.found || textureFile.empty()) return;
    const std::string full = (std::filesystem::path(layerDir) / textureFile).lexically_normal().string();
    std::string ext = std::filesystem::path(full).extension().string();
    for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (ext != ".hdr") {
        st.note("a DomeLight's texture (" + textureFile + ") is not a Radiance .hdr, the one format a sun is read from; no sun was taken from it");
        return;
    }
    u32 w = 0, h = 0;
    std::vector<f32> lum;
    std::string why;
    if (!loadRadianceLuminance(full, w, h, lum, why)) { st.note("a DomeLight's texture could not be read (" + why + "); no sun was taken from it"); return; }
    f64 sum = 0.0;
    f32 peak = 0.0f;
    for (const f32 l : lum) { sum += l; peak = std::max(peak, l); }
    const f64 mean = sum / static_cast<f64>(lum.size());
    if (!(peak > 20.0 * mean)) {
        st.note("the DomeLight's sky has no distinct sun (peak " + std::to_string(peak) + " vs mean " + std::to_string(mean) + "); the level keeps its own sun");
        return;
    }
    f64 acc[3] = {0.0, 0.0, 0.0};
    const f32 thr = 0.5f * peak;
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            const f32 l = lum[static_cast<usize>(y) * w + x];
            if (l < thr) continue;
            f32 dd[3];
            latLongDir(x, y, w, h, dd);
            for (int k = 0; k < 3; ++k) acc[k] += static_cast<f64>(l) * dd[k];
        }
    f32 dir[3] = {static_cast<f32>(acc[0]), static_cast<f32>(acc[1]), static_cast<f32>(acc[2])};
    // THE POLE: the texture's own pole is the dome's +Y. poleAxis "Z", or "scene" on a Z-up stage,
    // turns it +90 degrees about X onto +Z; "Y" leaves it. An unauthored poleAxis means "scene"
    // (UsdLux's fallback) -- except on a dome that already carries the orientToStageUpAxis xformOp,
    // which does that turn itself.
    const std::string pole = poleAxis.empty() ? (orientOp ? std::string("Y") : std::string("scene")) : poleAxis;
    const bool turn = pole == "Z" || (pole == "scene" && !st.c.yUp);
    if (turn) { const f32 y = dir[1], z = dir[2]; dir[1] = -z; dir[2] = y; }
    Vec3 e;
    if (!lightDirToEngine(st, world, dir, e)) return;
    st.sunDome.found = true;
    st.sunDome.direction = e;
    st.sunDome.source = path;
}

// A DistantLight shines down its -Z, so the sun is along its +Z.
void distantSun(Stage& st, const std::string& path, const M4& world, f32 angleDeg) {
    if (st.sunDistant.found) return;
    const f32 z[3] = {0.0f, 0.0f, 1.0f};
    Vec3 e;
    if (!lightDirToEngine(st, world, z, e)) return;
    st.sunDistant.found = true;
    st.sunDistant.direction = e;
    if (angleDeg > 0.0f) st.sunDistant.angularDiameterDeg = angleDeg;
    st.sunDistant.source = path;
}

// The first token/number/asset after `name` in a text prim body -- enough for the light attributes
// read here, which are all single values.
std::string bodyValue(const std::string& body, const char* name) {
    usize at = 0;
    while ((at = body.find(name, at)) != std::string::npos) {
        const usize after = at + std::strlen(name);
        // A whole attribute name: not a prefix of a longer one (inputs:texture:file vs ...:format).
        if (after < body.size() && (std::isalnum(static_cast<unsigned char>(body[after])) || body[after] == ':')) { at = after; continue; }
        const usize eq = body.find('=', after);
        const usize nl = body.find('\n', after);
        if (eq == std::string::npos || (nl != std::string::npos && eq > nl)) { at = after; continue; }
        usize v = eq + 1;
        while (v < body.size() && (body[v] == ' ' || body[v] == '\t')) ++v;
        if (v >= body.size()) return {};
        const char open = body[v];
        if (open == '@' || open == '"' || open == '\'') {
            const usize close = body.find(open, v + 1);
            return close == std::string::npos ? std::string() : body.substr(v + 1, close - v - 1);
        }
        usize e = v;
        while (e < body.size() && body[e] != '\n' && body[e] != ' ' && body[e] != ')' && body[e] != '(') ++e;
        return body.substr(v, e - v);
    }
    return {};
}

// Resolves an instancer's prototypes (building each once) and records it. Its instances are counted
// after the walk (countInstances), once the budget's focus -- possibly a camera found later in the
// walk -- is known.
void registerInstancer(Stage& st, Stack& sk, const std::string& path, const M4& world) {
    Instancer in;
    in.stack = &sk;
    in.path = path;
    in.world = world;
    in.id = st.instancers.size() + 1;
    for (const std::string& target : relTargets(st, sk, path, "prototypes")) {
        const std::string key = sk.prefix + target;
        i32 proto = -1;
        if (const auto it = st.protoByTarget.find(key); it != st.protoByTarget.end()) proto = it->second;
        else {
            proto = buildProto(st, Loc{&sk, -1, target});
            st.protoByTarget.emplace(key, proto);
            if (proto < 0) st.note("a PointInstancer prototype (" + target + ") resolved to no geometry; its instances were dropped");
        }
        in.protoOfTarget.push_back(proto);
    }
    st.instancers.push_back(std::move(in));
}

// PointInstancers inside what an arc brings in (a terrain tile's clutter), registered at their world transform.
// Follows the same arcs and skips as contents(); `toWorld` carries `loc`'s own space to the root.
void nestedInstancers(Stage& st, const Loc& loc, const M4& toWorld, int depth) {
    if (depth > 64 || loc.text >= 0) return;
    Stack& sk = *loc.stack;
    if (hiddenPrim(st, sk, loc.path)) return;
    UsdCrateValue v;
    if (fieldOf(st, sk, loc.path, "inheritPaths", v))
        for (const std::string& cls : v.s) nestedInstancers(st, Loc{loc.stack, -1, cls}, toWorld, depth + 1);
    i32 from = -1;
    for (const char* arc : {"references", "payload"})
        if (fieldOf(st, sk, loc.path, arc, v, &from))
            for (const UsdCrateRef& ref : v.refs) {
                Loc t;
                if (resolveRef(st, loc, from, ref, t)) nestedInstancers(st, t, toWorld, depth + 1);
            }
    for (const std::string& vp : selectedVariants(st, sk, loc.path))
        nestedInstancers(st, Loc{loc.stack, -1, vp}, toWorld, depth + 1);
    for (const std::string& name : childrenOf(st, sk, loc.path)) {
        const std::string cp = childPath(loc.path, name);
        if (specifierOf(st, sk, cp) == 2 || !isActive(st, sk, cp) || hiddenPrim(st, sk, cp)) continue;
        const std::string type = tokenOf(st, sk, cp, "typeName");
        if (type == "Material" || type == "Shader" || type == "GeomSubset" || type == "Mesh") continue;
        const Loc child{loc.stack, -1, cp};
        const M4 world = mul(composedLocal(st, child), toWorld);
        if (type == "PointInstancer") registerInstancer(st, sk, cp, world);
        else nestedInstancers(st, child, world, depth + 1);
    }
}

void buildStatic(Stage& st, const RawMesh& rm, const M4& world, const std::string& prefix,
                 const std::string& group) {
    // THE TRANSLATION BECOMES A PLACEMENT and rotation/scale stay in the vertices -- importUsd's
    // rule, for importUsd's reason (UsdImportResult::meshes).
    M4 basis = world;
    basis.m[12] = basis.m[13] = basis.m[14] = 0.0f;
    Ctx& c = st.c;
    const usize before = c.out->meshes.size();
    c.pathPrefix = prefix;
    buildMesh(c, rm.attrs, basis, rm.path, rm.subsets);
    c.pathPrefix.clear();
    if (c.out->meshes.size() == before) return;
    setGroup(*c.out, before, group);
    M4 t = M4::identity();
    t.m[12] = world.m[12]; t.m[13] = world.m[13]; t.m[14] = world.m[14];
    UsdPlacement pl = placementFor(c, static_cast<i32>(before), t);
    pl.name = rm.path;
    c.out->placements.push_back(std::move(pl));
}

void walkStatic(Stage& st, Stack& sk, const std::string& path, const M4& parent, int depth) {
    if (depth > 256) return;
    for (const std::string& name : childrenOf(st, sk, path)) {
        const std::string cp = childPath(path, name);
        if (specifierOf(st, sk, cp) != 0 || !isActive(st, sk, cp) || hiddenPrim(st, sk, cp)) continue;   // classes and bare overs
        if (excluded(st, cp)) continue;
        const std::string type = tokenOf(st, sk, cp, "typeName");
        if (type == "Material" || type == "Shader" || type == "GeomSubset") continue;
        const Loc loc{&sk, -1, cp};
        const M4 world = mul(composedLocal(st, loc), parent);
        if (type == "PointInstancer") { registerInstancer(st, sk, cp, world); continue; }
        if (type == "Camera") st.c.out->cameras.push_back(cameraPose(st.c, cp, world));
        UsdCrateValue v;
        if (type == "DistantLight") {
            const f32 angle = attr(st, sk, cp, "inputs:angle", v) && v.numberCount() ? static_cast<f32>(v.number()) : 0.0f;
            distantSun(st, cp, world, angle);
            continue;
        }
        if (type == "DomeLight") {
            i32 from = -1;
            std::string file, dir;
            if (fieldOf(st, sk, cp + ".inputs:texture:file", "default", v, &from) && v.str() && from >= 0) {
                file = *v.str();
                dir = st.sources[static_cast<usize>(from)]->dir;
            }
            bool orientOp = false;
            if (attr(st, sk, cp, "xformOpOrder", v))
                for (const std::string& op : v.s) if (op.find("orientToStageUpAxis") != std::string::npos) orientOp = true;
            domeSun(st, cp, world, file, dir, tokenOf(st, sk, cp + ".poleAxis", "default"), orientOp);
            continue;
        }
        if (type == "RectLight" || type == "SphereLight" || type == "DiskLight" || type == "CylinderLight")
            st.note("UsdLux area lights are not imported (a DistantLight or a DomeLight's sun becomes the level's sun)");
        // A prim that brings geometry in through an arc is placed like a one-off instance of it.
        const auto hasArcs = [&](const std::string& p) {
            return fieldOf(st, sk, p, "references", v) || fieldOf(st, sk, p, "inheritPaths", v) ||
                   fieldOf(st, sk, p, "payload", v);
        };
        const std::vector<std::string> variants = selectedVariants(st, sk, cp);
        bool arcs = hasArcs(cp);
        for (const std::string& vp : variants) arcs = arcs || hasArcs(vp);
        if (arcs) {
            const i32 proto = buildProto(st, loc);
            if (proto >= 0) {
                UsdPlacement pl = placementFor(st.c, st.protos[static_cast<usize>(proto)].meshIndex, parent);
                pl.name = cp;
                st.c.out->placements.push_back(std::move(pl));
            }
            nestedInstancers(st, loc, world, 0);
            continue;
        }
        if (type == "Mesh") {
            // Gathered, built and dropped: a static mesh is used once, and a terrain layer's meshes
            // held until the end would double the import's peak memory.
            RawMesh rm;
            gatherMesh(st, sk, cp, rm);
            buildStatic(st, rm, world, sk.prefix, groupOf(st, sk, cp));
        }
        walkStatic(st, sk, cp, world, depth + 1);
        for (const std::string& vp : variants) walkStatic(st, sk, vp, world, depth + 1);
    }
}

// ---- the instance budget -----------------------------------------------------------------------

// Where the budget concentrates, resolved from the options once the walk has found any cameras.
struct Focus {
    bool on = false;
    f32 x = 0.0f, y = 0.0f;       // engine space, cm
    f32 radius = 1.0f;

    // How strongly an instance at horizontal distance d competes for the budget: 1 inside the radius,
    // (radius / d)^3 beyond. CUBED, not squared, because the ground is two-dimensional: under a
    // squared falloff the ring beyond the radius outweighs the disc inside it by 2 ln(extent/radius)
    // -- about 7x for a hundred-metre focus on an eight-kilometre forest -- and the budget drains to
    // the horizon. Cubed, the whole tail weighs about twice the disc.
    f64 weight(f32 px, f32 py) const { return weightWithin(px, py, radius); }
    // The same falloff around a prototype's own full-density radius (UsdImportOptions::focusBySize).
    f64 weightWithin(f32 px, f32 py, f64 reach) const {
        if (!on) return 1.0;
        const f64 d = std::hypot(static_cast<f64>(px) - x, static_cast<f64>(py) - y);
        if (d <= reach) return 1.0;
        const f64 r = reach / d;
        return r * r * r;
    }
    f64 distance(f32 px, f32 py) const { return std::hypot(static_cast<f64>(px) - x, static_cast<f64>(py) - y); }
    bool inside(f32 px, f32 py) const {
        return on && std::hypot(static_cast<f64>(px) - x, static_cast<f64>(py) - y) <= radius;
    }
};

// Engine-space horizontal position of instance k: its position through the instancer's transform.
void instanceXY(const Ctx& c, const M4& world, const f32* p, f32& ex, f32& ey) {
    const f32 w[3] = {p[0] * world.m[0] + p[1] * world.m[4] + p[2] * world.m[8] + world.m[12],
                      p[0] * world.m[1] + p[1] * world.m[5] + p[2] * world.m[9] + world.m[13],
                      p[0] * world.m[2] + p[1] * world.m[6] + p[2] * world.m[10] + world.m[14]};
    f32 e[3];
    toEngineV(c, w, e);
    ex = e[0] * c.unitScale;
    ey = e[1] * c.unitScale;
}

// Every instance, counted per prototype: how many, their mean scale, and their summed focus weight.
void countInstances(Stage& st, const Focus& focus) {
    UsdImportResult& out = *st.c.out;
    for (const Instancer& in : st.instancers) {
        Stack& sk = *in.stack;
        UsdCrateValue pi, pos, sc;
        attr(st, sk, in.path, "protoIndices", pi);
        attr(st, sk, in.path, "positions", pos);
        attr(st, sk, in.path, "scales", sc);
        const std::vector<f32> p = pos.asFloats(), s = sc.asFloats();
        // The instancer's own uniform scale, times each instance's.
        const f32* w = in.world.m;
        const f64 det = w[0] * (w[5] * w[10] - w[6] * w[9]) - w[1] * (w[4] * w[10] - w[6] * w[8]) + w[2] * (w[4] * w[9] - w[5] * w[8]);
        const f64 worldScale = std::cbrt(std::fabs(det));
        const usize count = std::min(pi.i.size(), p.size() / 3);
        for (usize k = 0; k < count; ++k) {
            const i64 x = pi.i[k];
            if (x < 0 || static_cast<usize>(x) >= in.protoOfTarget.size() || in.protoOfTarget[static_cast<usize>(x)] < 0) continue;
            Proto& pr = st.protos[static_cast<usize>(in.protoOfTarget[static_cast<usize>(x)])];
            ++pr.instances;
            const f64 is = s.size() >= (k + 1) * 3 ? std::cbrt(std::fabs(static_cast<f64>(s[k * 3]) * s[k * 3 + 1] * s[k * 3 + 2])) : 1.0;
            pr.scaleSum += is * worldScale;
            ++pr.scaleCount;
            f32 ex = 0.0f, ey = 0.0f;
            if (focus.on) instanceXY(st.c, in.world, &p[k * 3], ex, ey);
            pr.focusSum += focus.weight(ex, ey);
            if (focus.on) {
                if (pr.distHist.empty()) pr.distHist.assign(kDistBins, 0);
                ++pr.distHist[distBin(focus.distance(ex, ey))];
            }
            if (focus.inside(ex, ey)) ++out.instancing.sourceInFocus;
        }
    }
}

// UsdImportOptions::focusBySize: every non-exempt prototype keeps full density out to
// reach = max(focus radius, K x its drawn radius), with K bisected so the expected kept count fits the
// budget. Expected counts come from the distance histograms, so the bisection never revisits an
// instance. Returns K (infinity when the whole stage fits).
f64 allocateBySize(Stage& st, const UsdImportOptions& opt, const Focus& focus) {
    f64 fixedCount = 0.0;
    for (Proto& p : st.protos) {
        p.exempt = opt.keepAllBelow && p.instances <= opt.keepAllBelow;
        p.keep = 1.0;
        if (p.exempt) fixedCount += static_cast<f64>(p.instances);
    }
    const auto expected = [&](f64 K) {
        f64 n = fixedCount;
        for (const Proto& p : st.protos) {
            if (p.exempt || p.distHist.empty()) continue;
            const f64 reach = std::max<f64>(focus.radius, K * p.worldRadius() * st.c.unitScale);
            for (usize b = 0; b < kDistBins; ++b) {
                if (!p.distHist[b]) continue;
                const f64 d = distBinCentre(b);
                const f64 r = d <= reach ? 1.0 : reach / d;
                n += static_cast<f64>(p.distHist[b]) * r * r * r;
            }
        }
        return n;
    };
    const f64 budget = static_cast<f64>(opt.maxInstances);
    f64 K = std::numeric_limits<f64>::infinity();
    if (opt.maxInstances && expected(1.0e9) > budget) {
        f64 lo = 0.0, hi = 1.0e9;
        for (int it = 0; it < 80; ++it) {
            const f64 mid = 0.5 * (lo + hi);
            (expected(mid) > budget ? hi : lo) = mid;
        }
        K = lo;
    }
    for (Proto& p : st.protos)
        p.reach = std::isinf(K) ? std::numeric_limits<f64>::infinity()
                                : std::max<f64>(focus.radius, K * p.worldRadius() * st.c.unitScale);
    return K;
}

// Shares the instance budget out, and sets each prototype's `keep` so that keeping an instance with
// probability keep x its focus weight lands on the share.
//
// THE POPULATION a prototype competes with is its focus-weighted count (every instance counts 1
// without a focus), except that one with at most keepAllBelow instances is EXEMPT: it counts in full
// and keeps every instance wherever it stands. A few dozen hand-placed hero plants on the far side of
// the map are the scene, not scatter to thin.
//
// WATER-FILLING by sqrt(population) x drawn radius: a prototype whose share would exceed its
// population keeps all of it and the surplus goes to the rest. The triangle budget then halves the
// costliest prototype's CAP and fills again, so the instances it gives up go to the prototypes that
// are cheap to draw rather than being lost.
void allocate(Stage& st, const UsdImportOptions& opt) {
    const usize n = st.protos.size();
    std::vector<f64> pop(n, 0.0), cap(n, 0.0), w(n, 0.0), alloc(n, 0.0);
    f64 total = 0.0;
    for (usize k = 0; k < n; ++k) {
        Proto& p = st.protos[k];
        p.exempt = opt.keepAllBelow && p.instances <= opt.keepAllBelow;
        pop[k] = p.exempt ? static_cast<f64>(p.instances) : p.focusSum;
        cap[k] = pop[k];
        w[k] = std::sqrt(pop[k]) * std::max(1.0, p.worldRadius());
        total += pop[k];
    }
    const f64 budget = opt.maxInstances ? std::min(static_cast<f64>(opt.maxInstances), total) : total;

    const auto fill = [&] {
        std::vector<bool> active(n, false);
        f64 remaining = budget;
        for (usize k = 0; k < n; ++k) { alloc[k] = 0.0; active[k] = cap[k] > 0.0 && w[k] > 0.0; }
        for (bool changed = true; changed;) {
            changed = false;
            f64 sum = 0.0;
            for (usize k = 0; k < n; ++k) if (active[k]) sum += w[k];
            if (sum <= 0.0 || remaining <= 0.0) break;
            // Saturate every prototype the CURRENT split would overfill, then split again.
            std::vector<usize> full;
            for (usize k = 0; k < n; ++k)
                if (active[k] && remaining * w[k] / sum >= cap[k]) full.push_back(k);
            for (const usize k : full) { alloc[k] = cap[k]; remaining -= cap[k]; active[k] = false; changed = true; }
            if (!changed)
                for (usize k = 0; k < n; ++k) if (active[k]) alloc[k] = remaining * w[k] / sum;
        }
    };
    fill();
    if (opt.maxInstanceTriangles) {
        for (int guard = 0; guard < 8192; ++guard) {
            f64 cost = 0.0, worst = 0.0;
            usize wk = 0;
            for (usize k = 0; k < n; ++k) {
                const f64 ck = alloc[k] * static_cast<f64>(st.protos[k].tris);
                cost += ck;
                if (ck > worst) { worst = ck; wk = k; }
            }
            if (cost <= static_cast<f64>(opt.maxInstanceTriangles) || worst <= 0.0) break;
            cap[wk] = std::floor(alloc[wk] * 0.5);
            fill();
        }
    }
    for (usize k = 0; k < n; ++k) st.protos[k].keep = pop[k] > 0.0 ? std::min(1.0, alloc[k] / pop[k]) : 0.0;
}

u64 splitmix(u64 x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

f64 hash01(u64 a, u64 b) {
    return static_cast<f64>(splitmix(splitmix(a) ^ (b * 0x100000001B3ull)) >> 11) * (1.0 / 9007199254740992.0);
}

void emitInstances(Stage& st, const Focus& focus, bool bySize) {
    Ctx& c = st.c;
    UsdImportResult& out = *c.out;
    for (const Instancer& in : st.instancers) {
        Stack& sk = *in.stack;
        UsdCrateValue pos, ori, scl, pidx, inv, ids;
        attr(st, sk, in.path, "positions", pos);
        attr(st, sk, in.path, "orientations", ori);
        attr(st, sk, in.path, "scales", scl);
        attr(st, sk, in.path, "protoIndices", pidx);
        attr(st, sk, in.path, "invisibleIds", inv);
        attr(st, sk, in.path, "ids", ids);
        const std::vector<f32> p = pos.asFloats(), q = ori.asFloats(), s = scl.asFloats();
        const std::vector<i32> ix = pidx.asInts();
        std::unordered_set<i64> hidden(inv.i.begin(), inv.i.end());
        const usize count = std::min(ix.size(), p.size() / 3);
        for (usize k = 0; k < count; ++k) {
            const i32 t = ix[k];
            if (t < 0 || static_cast<usize>(t) >= in.protoOfTarget.size()) continue;
            const i32 pr = in.protoOfTarget[static_cast<usize>(t)];
            if (pr < 0) continue;
            if (!hidden.empty() && hidden.count(k < ids.i.size() ? ids.i[k] : static_cast<i64>(k))) continue;
            Proto& proto = st.protos[static_cast<usize>(pr)];
            f32 ex = 0.0f, ey = 0.0f;
            if (focus.on) instanceXY(c, in.world, &p[k * 3], ex, ey);
            const f64 keep = proto.exempt ? 1.0
                           : bySize       ? focus.weightWithin(ex, ey, proto.reach)
                                          : proto.keep * focus.weight(ex, ey);
            if (keep < 1.0 && hash01(in.id, k) >= keep) continue;
            ++proto.kept;
            if (focus.inside(ex, ey)) ++out.instancing.keptInFocus;
            M4 m = M4::identity();
            if (s.size() >= (k + 1) * 3) m = scaleM(s[k * 3], s[k * 3 + 1], s[k * 3 + 2]);
            // Orientations are stored imaginary-first (i, j, k, real), GfQuath/GfQuatf's own layout.
            if (q.size() >= (k + 1) * 4) m = mul(m, quatRow(q[k * 4], q[k * 4 + 1], q[k * 4 + 2], q[k * 4 + 3]));
            m = mul(m, translate(p[k * 3], p[k * 3 + 1], p[k * 3 + 2]));
            m = mul(m, in.world);
            out.placements.push_back(placementFor(c, proto.meshIndex, m));
            ++out.instancing.keptInstances;
        }
        out.instancing.sourceInstances += count;
    }
}

// Moves each prototype's TYPICAL instance scale into its mesh. Jungle Ruins' plants are authored in
// metres and scattered at scale 400 in a centimetre stage, so the mesh alone is a third of a
// centimetre across: correct once placed, but a speck in the content browser, the mesh viewer and
// anything else that shows the asset on its own. The median uniform scale of its placements is baked
// into the vertices and divided back out of every placement -- the placed result is unchanged.
void normalisePrototypeScale(Stage& st) {
    UsdImportResult& out = *st.c.out;
    std::unordered_map<i32, std::vector<f32>> scales;
    for (const Proto& p : st.protos) scales[p.meshIndex];
    for (const UsdPlacement& pl : out.placements) {
        const auto it = scales.find(pl.meshIndex);
        if (it != scales.end()) it->second.push_back(std::cbrt(std::fabs(pl.scale.x * pl.scale.y * pl.scale.z)));
    }
    for (auto& [mesh, list] : scales) {
        if (list.empty() || mesh < 0 || static_cast<usize>(mesh) >= out.meshes.size()) continue;
        std::nth_element(list.begin(), list.begin() + static_cast<std::ptrdiff_t>(list.size() / 2), list.end());
        const f32 s = list[list.size() / 2];
        if (!(s > 0.0f) || !std::isfinite(s) || (s > 0.5f && s < 2.0f)) continue;
        OcMeshData& m = out.meshes[static_cast<usize>(mesh)];
        for (f32& v : m.positions) v *= s;
        m.boundsMin = Vec3{m.boundsMin.x * s, m.boundsMin.y * s, m.boundsMin.z * s};
        m.boundsMax = Vec3{m.boundsMax.x * s, m.boundsMax.y * s, m.boundsMax.z * s};
        for (UsdPlacement& pl : out.placements)
            if (pl.meshIndex == mesh) pl.scale = Vec3{pl.scale.x / s, pl.scale.y / s, pl.scale.z / s};
    }
}

// The root and its subLayers, depth-first, strongest first.
void collectLayerStack(Stage& st, i32 src, std::vector<i32>& order, std::unordered_set<i32>& seen, int depth) {
    if (src < 0 || depth > 32 || !seen.insert(src).second) return;
    order.push_back(src);
    const Source& s = *st.sources[static_cast<usize>(src)];
    const std::vector<std::string> subs = s.subLayers;
    const std::string dir = s.dir;
    for (const std::string& sub : subs) {
        const std::string full = (std::filesystem::path(dir) / sub).lexically_normal().string();
        std::string why;
        const i32 idx = loadSource(st, full, &why);
        if (idx < 0) { st.note("a sublayer could not be opened (" + why + "); what it held did not import"); continue; }
        collectLayerStack(st, idx, order, seen, depth + 1);
    }
}

} // namespace

bool importUsdStage(const std::string& path, UsdImportResult& out, const UsdImportOptions& opt,
                    std::string* why) {
    out = UsdImportResult{};
    Stage st;
    st.c.out = &out;
    st.c.opt = &opt;

    const i32 root = loadSource(st, path, why);
    if (root < 0) return false;
    {
        const Source& r = *st.sources[static_cast<usize>(root)];
        out.encoding = r.crate ? UsdEncoding::Usdc : UsdEncoding::Usda;
        out.sourceUpAxis = r.upAxis;
        out.sourceMetersPerUnit = r.metersPerUnit;
        // THE ROOT'S UNITS FOR EVERYTHING: USD composes every layer in the root stage's units,
        // whatever an individual layer declares.
        st.c.yUp = r.yUp;
        st.c.unitScale = r.metersPerUnit * opt.scale;
    }

    std::vector<i32> order;
    std::unordered_set<i32> seen;
    collectLayerStack(st, root, order, seen, 0);

    // The binary layers form one composed stack; each text layer contributes its own prims.
    st.stacks.push_back(Stack{{}, "R|", false});
    Stack& rootStack = st.stacks.back();
    for (const i32 li : order)
        if (st.sources[static_cast<usize>(li)]->crate) rootStack.layers.push_back(li);
    // Text layers' cameras first: they were found as the layers were read, and a stage's camera
    // usually lives in a small text sublayer of its own.
    for (const i32 li : order) {
        const Source& s = *st.sources[static_cast<usize>(li)];
        for (const std::string& cp : s.cameraPaths)
            if (const auto it = s.worlds.find(cp); it != s.worlds.end() && !excluded(st, cp))
                out.cameras.push_back(cameraPose(st.c, cp, it->second));
        // Text layers' lights, read off the body the text walk kept (LightPrim).
        for (const LightPrim& lp : s.lights) {
            const auto it = s.worlds.find(lp.path);
            if (it == s.worlds.end() || excluded(st, lp.path)) continue;
            if (lp.type == "DistantLight") {
                distantSun(st, lp.path, it->second, static_cast<f32>(std::atof(bodyValue(lp.body, "inputs:angle").c_str())));
            } else {
                domeSun(st, lp.path, it->second, bodyValue(lp.body, "inputs:texture:file"), s.dir,
                        bodyValue(lp.body, "poleAxis"), lp.body.find("orientToStageUpAxis") != std::string::npos);
            }
        }
    }
    if (!rootStack.layers.empty()) walkStatic(st, rootStack, "/", M4::identity(), 0);
    bool mixed = false;
    for (const i32 li : order) {
        const Source& s = *st.sources[static_cast<usize>(li)];
        if (s.crate) continue;
        if (!rootStack.layers.empty() && !s.raw.empty()) mixed = true;
        for (const RawMesh& rm : s.raw)
            if (!excluded(st, rm.path)) buildStatic(st, rm, rm.world, s.prefix, groupOf(s));
    }
    if (mixed) st.note("text and binary layers in the root stack were not merged by prim path; each contributed what it defines itself");

    // Materials: the root stack's, and those of every binary layer a prototype reached into. Text
    // layers built theirs as they were read.
    for (Stack& sk : st.stacks) if (!sk.layers.empty()) registerMaterials(st, sk);

    Focus focus;
    if (opt.focus == UsdInstanceFocus::Point) {
        focus.on = true;
        focus.x = opt.focusPoint.x;
        focus.y = opt.focusPoint.y;
    } else if (opt.focus == UsdInstanceFocus::FirstCamera) {
        if (!out.cameras.empty()) {
            focus.on = true;
            focus.x = out.cameras.front().position.x;
            focus.y = out.cameras.front().position.y;
        } else if (!st.instancers.empty()) {
            st.note("the instance budget was asked to focus on the stage's camera, but the stage has none; it was spread evenly instead");
        }
    }
    focus.radius = std::max(1.0f, opt.focusRadius);
    out.instancing.focused = focus.on;
    out.instancing.focusPoint = Vec3{focus.x, focus.y, 0.0f};
    out.instancing.focusRadius = focus.on ? focus.radius : 0.0f;

    out.sun = st.sunDistant.found ? st.sunDistant : st.sunDome;
    for (const std::string& ex : opt.excludePrims)
        if (!ex.empty() && !st.excludeHits.count(ex))
            st.note("excludePrims named " + ex + ", which matched no prim in the stage (paths are the stage's own, e.g. /root/Name)");

    countInstances(st, focus);
    const bool bySize = opt.focusBySize && focus.on;
    if (opt.focusBySize && !focus.on && !st.instancers.empty())
        st.note("focusBySize needs a focus (a stage camera or a point); the budget was shared per prototype instead");
    if (bySize) allocateBySize(st, opt, focus);
    else        allocate(st, opt);
    emitInstances(st, focus, bySize);
    normalisePrototypeScale(st);
    out.instancing.instancers = st.instancers.size();
    out.instancing.prototypes = st.protos.size();
    for (const Proto& p : st.protos) {
        UsdPrototypeStats ps{p.meshIndex, p.tris, p.instances, p.kept,
                             static_cast<f32>(p.worldRadius() * st.c.unitScale)};
        if (bySize) ps.fullDensityRadius = p.exempt || std::isinf(p.reach) ? -1.0f : static_cast<f32>(p.reach);
        out.instancing.perPrototype.push_back(ps);
    }

    resolveBindings(st.c);
    // The text walk flags references and instancers it met in TEXT layers; those are genuinely not
    // composed, but the wording importUsd uses would also claim the binary ones were not.
    const bool textRefs = st.c.sawReference, textInst = st.c.sawInstancing;
    st.c.sawReference = st.c.sawInstancing = false;
    reportUnsupported(st.c);
    if (textRefs) out.unsupported.push_back("references and `over` opinions inside TEXT layers were not composed (binary layers' were)");
    if (textInst) out.unsupported.push_back("PointInstancers inside TEXT layers were not expanded (binary layers' were)");
    if (out.instancing.keptInstances < out.instancing.sourceInstances)
        out.unsupported.push_back("PointInstancers declare " + std::to_string(out.instancing.sourceInstances) +
                                  " instances; " + std::to_string(out.instancing.keptInstances) +
                                  " were kept within the instance budget, thinned per prototype" +
                                  (focus.on ? " and densest around the focus" : ""));
    for (const std::string& n : st.notes) out.unsupported.push_back(n);

    if (out.meshes.empty())
        return fail(why, "USD: the stage composed, but no layer in it defines a UsdGeomMesh with points");
    return true;
}

} // namespace aver::fmt
