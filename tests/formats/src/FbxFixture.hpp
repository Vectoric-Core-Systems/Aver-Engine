#pragma once
// FbxFixture -- writes ASCII FBX 7.4 from code, so every FBX test reads as the scene it builds and a failure names
// the field that broke. Header-only, no engine dependency: ufbx parses what this emits.
//
//   fbxfix::Fbx f;                                 // Y-up, 1 unit = 1 metre by default (UnitScaleFactor 100)
//   const int m = f.mesh("Cube", 0, fbxfix::triangle());
//   const std::string text = f.toString();         // feed to importFbxFromMemory
//
// Ids are handed out by the builder; connections follow FBX's own: "OO" object to object, "OP" object to a
// property of another. Only what the tests need is modelled: nodes, meshes, materials and textures, bones with
// skin clusters, blend shapes, animation stacks, cameras and lights. A construct the pinned ufbx rejects shows up
// as the importer's error text in the test that built it.
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace fbxfix {

constexpr double kTicksPerSecond = 46186158000.0;   // FBX KTime ticks in one second

struct V3 { double x = 0, y = 0, z = 0; };

// A polygon mesh in file space. Per-polygon-vertex arrays (normals, uvs) follow the faces' order.
struct MeshDesc {
    std::vector<double> verts;                 // xyz per control point
    std::vector<std::vector<int>> faces;       // control point indices per polygon
    std::vector<double> normals;               // xyz per polygon vertex, or empty for none
    std::vector<double> uvs;                   // uv per polygon vertex, or empty for none
    std::vector<int> faceMaterial;             // per polygon, or empty for none
};

// A counter-clockwise triangle in the XY plane facing +Z (unit legs), normals +Z.
inline MeshDesc triangle(double ox = 0, double oy = 0, double oz = 0) {
    MeshDesc m;
    m.verts = {ox, oy, oz, ox + 1, oy, oz, ox, oy + 1, oz};
    m.faces = {{0, 1, 2}};
    m.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1};
    return m;
}

// A unit quad in the XY plane facing +Z, normals +Z.
inline MeshDesc quad() {
    MeshDesc m;
    m.verts = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    m.faces = {{0, 1, 2, 3}};
    m.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    return m;
}

struct ClusterDesc {
    int bone = 0;                              // the bone's model id
    std::vector<int> indexes;                  // control points it weights
    std::vector<double> weights;               // parallel to indexes
    std::vector<double> transform;             // 16: the mesh node's bind world; empty = identity
    std::vector<double> link;                  // 16: the bone's bind world; empty = identity
};

struct ShapeDesc {
    std::string name;
    double fullWeight = 100.0;                 // the DeformPercent at which this shape is complete
    std::vector<int> indexes;                  // control points moved
    std::vector<double> verts;                 // xyz offsets, parallel to indexes
    std::vector<double> normals;               // xyz normal offsets, or empty
};

// An FBX 4x4 as 16 numbers (translation at 12..14) from translation / XYZ Euler degrees / scale.
inline std::vector<double> matTRS(V3 t, V3 rDeg = {}, V3 s = {1, 1, 1}) {
    const double d2r = 3.14159265358979323846 / 180.0;
    const double cx = std::cos(rDeg.x * d2r), sx = std::sin(rDeg.x * d2r);
    const double cy = std::cos(rDeg.y * d2r), sy = std::sin(rDeg.y * d2r);
    const double cz = std::cos(rDeg.z * d2r), sz = std::sin(rDeg.z * d2r);
    // R = Rz * Ry * Rx (FBX's XYZ order applies X first), column-vector convention.
    const double Rx[3][3] = {{1, 0, 0}, {0, cx, -sx}, {0, sx, cx}};
    const double Ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
    const double Rz[3][3] = {{cz, -sz, 0}, {sz, cz, 0}, {0, 0, 1}};
    double T1[3][3], R[3][3];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { T1[i][j] = 0; for (int k = 0; k < 3; ++k) T1[i][j] += Ry[i][k] * Rx[k][j]; }
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { R[i][j] = 0; for (int k = 0; k < 3; ++k) R[i][j] += Rz[i][k] * T1[k][j]; }
    const double sc[3] = {s.x, s.y, s.z};
    std::vector<double> a(16, 0.0);
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 3; ++row) a[col * 4 + row] = R[row][col] * sc[col];
    a[12] = t.x; a[13] = t.y; a[14] = t.z; a[15] = 1.0;
    return a;
}

inline std::string base64(const std::vector<unsigned char>& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < in.size(); i += 3) {
        const unsigned a = in[i], b = i + 1 < in.size() ? in[i + 1] : 0, c = i + 2 < in.size() ? in[i + 2] : 0;
        const unsigned v = (a << 16) | (b << 8) | c;
        out.push_back(T[(v >> 18) & 63]);
        out.push_back(T[(v >> 12) & 63]);
        out.push_back(i + 1 < in.size() ? T[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < in.size() ? T[v & 63] : '=');
    }
    return out;
}

class Fbx {
public:
    // ---- the scene's settings ----
    // 1 = centimetres (FBX's own default), 100 = metres, 2.54 = inches.
    Fbx& unitScaleFactor(double f) { unit_ = f; return *this; }
    Fbx& yUp() { up_ = 1; upSign_ = 1; front_ = 2; frontSign_ = 1; return *this; }
    Fbx& zUp() { up_ = 2; upSign_ = 1; front_ = 1; frontSign_ = -1; return *this; }
    Fbx& creator(const std::string& c) { creator_ = c; return *this; }

    // ---- nodes ----
    // A node. kind: "Null", "Mesh", "LimbNode", "Camera", "Light". parent 0 = the scene root. Euler degrees, XYZ order.
    int node(const std::string& name, int parent, V3 t = {}, V3 rDeg = {}, V3 s = {1, 1, 1},
             const char* kind = "Null", bool visible = true) {
        const int id = next_++;
        std::string o = "\tModel: " + std::to_string(id) + ", \"Model::" + name + "\", \"" + kind + "\" {\n";
        o += "\t\tVersion: 232\n\t\tProperties70:  {\n";
        o += vec("Lcl Translation", t) + vec("Lcl Rotation", rDeg) + vec("Lcl Scaling", s);
        if (!visible) o += "\t\t\tP: \"Visibility\", \"Visibility\", \"\", \"A\",0\n";
        o += "\t\t}\n\t\tShading: T\n\t\tCulling: \"CullingOff\"\n\t}\n";
        objects_ += o;
        connect(id, parent);
        return id;
    }

    // A geometry object. Connect it to a node with attachGeometry.
    int geometry(const std::string& name, const MeshDesc& m) {
        const int id = next_++;
        std::string o = "\tGeometry: " + std::to_string(id) + ", \"Geometry::" + name + "\", \"Mesh\" {\n";
        o += "\t\tProperties70:  {\n\t\t}\n\t\tGeometryVersion: 124\n";
        o += array("Vertices", m.verts);
        std::vector<double> idx;
        for (const auto& f : m.faces)
            for (size_t k = 0; k < f.size(); ++k) idx.push_back(k + 1 == f.size() ? double(-f[k] - 1) : double(f[k]));
        o += intArray("PolygonVertexIndex", idx);
        int layer = 0;
        std::string layers;
        if (!m.normals.empty()) {
            o += "\t\tLayerElementNormal: 0 {\n\t\t\tVersion: 101\n\t\t\tName: \"\"\n\t\t\tMappingInformationType: \"ByPolygonVertex\"\n"
                 "\t\t\tReferenceInformationType: \"Direct\"\n" + array("Normals", m.normals, 3) + "\t\t}\n";
            layers += "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementNormal\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n";
        }
        if (!m.uvs.empty()) {
            o += "\t\tLayerElementUV: 0 {\n\t\t\tVersion: 101\n\t\t\tName: \"UVMap\"\n\t\t\tMappingInformationType: \"ByPolygonVertex\"\n"
                 "\t\t\tReferenceInformationType: \"Direct\"\n" + array("UV", m.uvs, 2) + "\t\t}\n";
            layers += "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementUV\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n";
        }
        if (!m.faceMaterial.empty()) {
            std::vector<double> mats(m.faceMaterial.begin(), m.faceMaterial.end());
            o += "\t\tLayerElementMaterial: 0 {\n\t\t\tVersion: 101\n\t\t\tName: \"\"\n\t\t\tMappingInformationType: \"ByPolygon\"\n"
                 "\t\t\tReferenceInformationType: \"IndexToDirect\"\n" + intArray("Materials", mats) + "\t\t}\n";
            layers += "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementMaterial\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n";
        }
        o += "\t\tLayer: " + std::to_string(layer) + " {\n\t\t\tVersion: 100\n" + layers + "\t\t}\n\t}\n";
        objects_ += o;
        return id;
    }
    void attachGeometry(int geometryId, int nodeId) { connect(geometryId, nodeId); }

    // A mesh node: geometry plus the node that carries it. Returns the NODE's id; geometryOf() gives the geometry's.
    int mesh(const std::string& name, int parent, const MeshDesc& m, V3 t = {}, V3 rDeg = {}, V3 s = {1, 1, 1},
             bool visible = true) {
        const int g = geometry(name, m);
        const int n = node(name, parent, t, rDeg, s, "Mesh", visible);
        attachGeometry(g, n);
        geoms_[n] = g;
        return n;
    }
    // A second node instancing an existing node's geometry.
    int instance(const std::string& name, int parent, int meshNode, V3 t = {}, V3 rDeg = {}, V3 s = {1, 1, 1}) {
        const int n = node(name, parent, t, rDeg, s, "Mesh");
        attachGeometry(geoms_.at(meshNode), n);
        geoms_[n] = geoms_.at(meshNode);
        return n;
    }
    int geometryOf(int meshNode) const { return geoms_.at(meshNode); }

    // ---- materials and textures ----
    // shading "phong" or "lambert". shininess < 0 leaves it out; transparency < 0 leaves TransparencyFactor out.
    int material(const std::string& name, V3 diffuse, const char* shading = "phong", double shininess = -1,
                 double transparency = -1) {
        const int id = next_++;
        std::string o = "\tMaterial: " + std::to_string(id) + ", \"Material::" + name + "\", \"\" {\n\t\tVersion: 102\n";
        o += std::string("\t\tShadingModel: \"") + shading + "\"\n\t\tMultiLayer: 0\n\t\tProperties70:  {\n";
        o += vec("DiffuseColor", diffuse, "Color");
        if (shininess >= 0) o += "\t\t\tP: \"ShininessExponent\", \"Number\", \"\", \"A\"," + num(shininess) + "\n";
        if (transparency >= 0) o += "\t\t\tP: \"TransparencyFactor\", \"Number\", \"\", \"A\"," + num(transparency) + "\n";
        o += "\t\t}\n\t}\n";
        objects_ += o;
        return id;
    }
    void assignMaterial(int nodeId, int materialId) { connect(materialId, nodeId); }

    // A texture on a material property ("DiffuseColor", "NormalMap", ...). `embedded` non-empty = a Video object
    // carrying the bytes (base64 Content), as an "Embed Textures" export writes.
    int texture(int materialId, const std::string& property, const std::string& name, const std::string& absFile,
                const std::string& relFile, const std::vector<unsigned char>& embedded = {}) {
        const int id = next_++;
        std::string o = "\tTexture: " + std::to_string(id) + ", \"Texture::" + name + "\", \"\" {\n\t\tType: \"TextureVideoClip\"\n"
                        "\t\tVersion: 202\n\t\tTextureName: \"Texture::" + name + "\"\n\t\tProperties70:  {\n\t\t}\n"
                        "\t\tMedia: \"Video::" + name + "\"\n\t\tFileName: \"" + escape(absFile) + "\"\n"
                        "\t\tRelativeFilename: \"" + escape(relFile) + "\"\n\t}\n";
        objects_ += o;
        connectProp(id, materialId, property);
        if (!embedded.empty()) {
            const int vid = next_++;
            std::string v = "\tVideo: " + std::to_string(vid) + ", \"Video::" + name + "\", \"Clip\" {\n\t\tType: \"Clip\"\n"
                            "\t\tProperties70:  {\n\t\t}\n\t\tUseMipMap: 0\n\t\tFilename: \"" + escape(absFile) + "\"\n"
                            "\t\tRelativeFilename: \"" + escape(relFile) + "\"\n\t\tContent: , \"" + base64(embedded) + "\"\n\t}\n";
            objects_ += v;
            connect(vid, id);
        }
        return id;
    }

    // ---- skeletons ----
    int bone(const std::string& name, int parent, V3 t = {}, V3 rDeg = {}, V3 s = {1, 1, 1}) {
        const int n = node(name, parent, t, rDeg, s, "LimbNode");
        const int a = next_++;
        objects_ += "\tNodeAttribute: " + std::to_string(a) + ", \"NodeAttribute::" + name + "\", \"LimbNode\" {\n"
                    "\t\tProperties70:  {\n\t\t}\n\t\tTypeFlags: \"Skeleton\"\n\t}\n";
        connect(a, n);
        return n;
    }

    // A skin on a geometry, one cluster per bone.
    int skin(int geometryId, const std::vector<ClusterDesc>& clusters) {
        const int sid = next_++;
        objects_ += "\tDeformer: " + std::to_string(sid) + ", \"Deformer::Skin\", \"Skin\" {\n\t\tVersion: 101\n"
                    "\t\tLink_DeformAcuracy: 50\n\t\tSkinningType: \"Linear\"\n\t}\n";
        connect(sid, geometryId);
        int n = 0;
        for (const ClusterDesc& c : clusters) {
            const int cid = next_++;
            std::string o = "\tDeformer: " + std::to_string(cid) + ", \"SubDeformer::Cluster" + std::to_string(n++) + "\", \"Cluster\" {\n"
                            "\t\tVersion: 100\n\t\tUserData: \"\", \"\"\n";
            o += intArray("Indexes", std::vector<double>(c.indexes.begin(), c.indexes.end()));
            o += array("Weights", c.weights);
            o += array("Transform", c.transform.empty() ? identity16() : c.transform);
            o += array("TransformLink", c.link.empty() ? identity16() : c.link);
            o += "\t}\n";
            objects_ += o;
            connect(cid, sid);
            connect(c.bone, cid);
        }
        return sid;
    }

    // ---- blend shapes ----
    // One blend channel on a geometry, with one shape per entry of `shapes` (in-between shapes have a smaller fullWeight).
    int blendChannel(int geometryId, const std::string& channelName, const std::vector<ShapeDesc>& shapes) {
        if (blendDeformer_.find(geometryId) == blendDeformer_.end()) {
            const int did = next_++;
            objects_ += "\tDeformer: " + std::to_string(did) + ", \"Deformer::BlendShapes\", \"BlendShape\" {\n\t\tVersion: 100\n\t}\n";
            connect(did, geometryId);
            blendDeformer_[geometryId] = did;
        }
        const int cid = next_++;
        std::vector<double> fw;
        for (const ShapeDesc& s : shapes) fw.push_back(s.fullWeight);
        objects_ += "\tDeformer: " + std::to_string(cid) + ", \"SubDeformer::" + channelName + "\", \"BlendShapeChannel\" {\n"
                    "\t\tVersion: 100\n\t\tDeformPercent: 0\n" + array("FullWeights", fw) + "\t}\n";
        connect(cid, blendDeformer_[geometryId]);
        for (const ShapeDesc& s : shapes) {
            const int gid = next_++;
            std::string o = "\tGeometry: " + std::to_string(gid) + ", \"Geometry::" + s.name + "\", \"Shape\" {\n\t\tVersion: 100\n";
            o += intArray("Indexes", std::vector<double>(s.indexes.begin(), s.indexes.end()));
            o += array("Vertices", s.verts);
            if (!s.normals.empty()) o += array("Normals", s.normals);
            o += "\t}\n";
            objects_ += o;
            connect(gid, cid);
        }
        return cid;
    }

    // ---- cameras and lights ----
    int camera(const std::string& name, int parent, V3 t = {}, V3 rDeg = {}) {
        const int n = node(name, parent, t, rDeg, {1, 1, 1}, "Camera");
        const int a = next_++;
        objects_ += "\tNodeAttribute: " + std::to_string(a) + ", \"NodeAttribute::" + name + "\", \"Camera\" {\n"
                    "\t\tProperties70:  {\n\t\t\tP: \"FieldOfView\", \"FieldOfView\", \"\", \"A\",50\n\t\t}\n"
                    "\t\tTypeFlags: \"Camera\"\n\t}\n";
        connect(a, n);
        return n;
    }
    int light(const std::string& name, int parent) {
        const int n = node(name, parent, {}, {}, {1, 1, 1}, "Light");
        const int a = next_++;
        objects_ += "\tNodeAttribute: " + std::to_string(a) + ", \"NodeAttribute::" + name + "\", \"Light\" {\n"
                    "\t\tProperties70:  {\n\t\t}\n\t\tTypeFlags: \"Light\"\n\t}\n";
        connect(a, n);
        return n;
    }

    // ---- animation ----
    // One animated channel: `prop` is "Lcl Translation", "Lcl Rotation", "Lcl Scaling" (axis 'X','Y','Z') or
    // "DeformPercent" (axis ignored, target = a blend channel id). keys are (seconds, value); step = constant
    // interpolation, otherwise linear.
    struct Curve {
        int target = 0;
        std::string prop;
        char axis = 'X';
        std::vector<std::pair<double, double>> keys;
        bool step = false;
        V3 base{};                 // the node's static value of the property: the other axes' defaults
    };
    int animStack(const std::string& name, double durationSeconds, const std::vector<Curve>& curves) {
        const int stack = next_++;
        const std::string ticks = std::to_string(static_cast<long long>(std::llround(durationSeconds * kTicksPerSecond)));
        objects_ += "\tAnimationStack: " + std::to_string(stack) + ", \"AnimStack::" + name + "\", \"\" {\n\t\tProperties70:  {\n"
                    "\t\t\tP: \"LocalStart\", \"KTime\", \"Time\", \"\",0\n\t\t\tP: \"LocalStop\", \"KTime\", \"Time\", \"\"," + ticks + "\n"
                    "\t\t\tP: \"ReferenceStart\", \"KTime\", \"Time\", \"\",0\n\t\t\tP: \"ReferenceStop\", \"KTime\", \"Time\", \"\"," + ticks + "\n"
                    "\t\t}\n\t}\n";
        const int layer = next_++;
        objects_ += "\tAnimationLayer: " + std::to_string(layer) + ", \"AnimLayer::BaseLayer\", \"\" {\n\t}\n";
        connect(layer, stack);
        std::map<std::pair<int, std::string>, int> nodes;
        for (const Curve& c : curves) {
            const auto key = std::make_pair(c.target, c.prop);
            int cn;
            auto it = nodes.find(key);
            if (it == nodes.end()) {
                cn = next_++;
                std::string o = "\tAnimationCurveNode: " + std::to_string(cn) + ", \"AnimCurveNode::" + c.prop + "\", \"\" {\n\t\tProperties70:  {\n";
                if (c.prop == "DeformPercent") {
                    o += "\t\t\tP: \"d|DeformPercent\", \"Number\", \"\", \"A\",0\n";
                } else {
                    o += "\t\t\tP: \"d|X\", \"Number\", \"\", \"A\"," + num(c.base.x) + "\n\t\t\tP: \"d|Y\", \"Number\", \"\", \"A\"," +
                         num(c.base.y) + "\n\t\t\tP: \"d|Z\", \"Number\", \"\", \"A\"," + num(c.base.z) + "\n";
                }
                o += "\t\t}\n\t}\n";
                objects_ += o;
                connect(cn, layer);
                connectProp(cn, c.target, c.prop);
                nodes[key] = cn;
            } else {
                cn = it->second;
            }
            const int cv = next_++;
            std::string times, values;
            for (size_t i = 0; i < c.keys.size(); ++i) {
                times += (i ? "," : "") + std::to_string(static_cast<long long>(std::llround(c.keys[i].first * kTicksPerSecond)));
                values += (i ? "," : "") + num(c.keys[i].second, true);
            }
            const std::string n = std::to_string(c.keys.size());
            objects_ += "\tAnimationCurve: " + std::to_string(cv) + ", \"AnimCurve::\", \"\" {\n\t\tDefault: 0\n\t\tKeyVer: 4009\n"
                        "\t\tKeyTime: *" + n + " {\n\t\t\ta: " + times + "\n\t\t}\n"
                        "\t\tKeyValueFloat: *" + n + " {\n\t\t\ta: " + values + "\n\t\t}\n"
                        "\t\tKeyAttrFlags: *1 {\n\t\t\ta: " + (c.step ? "2" : "4") + "\n\t\t}\n"
                        "\t\tKeyAttrDataFloat: *4 {\n\t\t\ta: 0,0,0,0\n\t\t}\n"
                        "\t\tKeyAttrRefCount: *1 {\n\t\t\ta: " + n + "\n\t\t}\n\t}\n";
            const std::string axisProp = c.prop == "DeformPercent" ? "d|DeformPercent" : std::string("d|") + c.axis;
            connectProp(cv, cn, axisProp);
        }
        return stack;
    }

    // ---- output ----
    std::string toString() const {
        std::string s = "; FBX 7.4.0 project file\n; ------------------------------------------------\n\n";
        s += "FBXHeaderExtension:  {\n\tFBXHeaderVersion: 1003\n\tFBXVersion: 7400\n\tCreator: \"" + creator_ + "\"\n}\n";
        s += "GlobalSettings:  {\n\tVersion: 1000\n\tProperties70:  {\n";
        s += "\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\"," + std::to_string(up_) + "\n";
        s += "\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\"," + std::to_string(upSign_) + "\n";
        s += "\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\"," + std::to_string(front_) + "\n";
        s += "\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\"," + std::to_string(frontSign_) + "\n";
        s += "\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n";
        s += "\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\"," + num(unit_) + "\n";
        s += "\t\tP: \"OriginalUnitScaleFactor\", \"double\", \"Number\", \"\"," + num(unit_) + "\n";
        s += "\t}\n}\n";
        s += "Objects:  {\n" + objects_ + "}\n";
        s += "Connections:  {\n" + connections_ + "}\n";
        return s;
    }
    std::vector<unsigned char> bytes() const { const std::string t = toString(); return std::vector<unsigned char>(t.begin(), t.end()); }

private:
    int next_ = 1000;
    std::string objects_, connections_, creator_ = "Aver FbxFixture";
    double unit_ = 100.0;
    int up_ = 1, upSign_ = 1, front_ = 2, frontSign_ = 1;
    std::map<int, int> geoms_, blendDeformer_;

    static std::string num(double v, bool forceDecimal = false) {
        char b[64];
        std::snprintf(b, sizeof b, "%.9g", v);
        std::string s = b;
        if (forceDecimal && s.find_first_of(".eE") == std::string::npos) s += ".0";
        return s;
    }
    static std::string escape(const std::string& s) {
        std::string o;
        for (char c : s) { if (c == '"') o += "\\\""; else o += c; }
        return o;
    }
    static std::vector<double> identity16() {
        std::vector<double> a(16, 0.0);
        a[0] = a[5] = a[10] = a[15] = 1.0;
        return a;
    }
    static std::string vec(const char* name, V3 v, const char* type = nullptr) {
        return std::string("\t\t\tP: \"") + name + "\", \"" + (type ? type : name) + "\", \"\", \"A\"," + num(v.x) + "," + num(v.y) + "," + num(v.z) + "\n";
    }
    // "Name: *N {\n a: ... \n}" with doubles printed so ufbx reads them as reals.
    static std::string array(const char* name, const std::vector<double>& v, int perUnit = 0) {
        (void)perUnit;
        std::string a;
        for (size_t i = 0; i < v.size(); ++i) a += (i ? "," : "") + num(v[i], true);
        return std::string("\t\t") + name + ": *" + std::to_string(v.size()) + " {\n\t\t\ta: " + a + "\n\t\t}\n";
    }
    static std::string intArray(const char* name, const std::vector<double>& v) {
        std::string a;
        for (size_t i = 0; i < v.size(); ++i) a += (i ? "," : "") + std::to_string(static_cast<long long>(v[i]));
        return std::string("\t\t") + name + ": *" + std::to_string(v.size()) + " {\n\t\t\ta: " + a + "\n\t\t}\n";
    }
    void connect(int child, int parent) {
        connections_ += "\tC: \"OO\"," + std::to_string(child) + "," + std::to_string(parent) + "\n";
    }
    void connectProp(int child, int parent, const std::string& prop) {
        connections_ += "\tC: \"OP\"," + std::to_string(child) + "," + std::to_string(parent) + ", \"" + prop + "\"\n";
    }
};

} // namespace fbxfix
