// The OBJ and USDA importers: the basis change, the winding reversal, the V flip, face
// de-indexing, n-gon triangulation, material runs, and what each importer refuses.
//
// Documents are built by hand here, for GltfTest's reason: what a case exercises should be legible
// in the case, and a failure should name the field that broke rather than a byte offset into a
// fixture nobody can read.
#include "aver/formats/ObjImport.hpp"
#include "aver/formats/UsdImport.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static void checkNear(f32 got, f32 want, f32 tol, const std::string& what) {
    check(std::fabs(got - want) <= tol,
          what + "  (got " + std::to_string(got) + ", want " + std::to_string(want) + ")");
}

static bool obj(const std::string& text, fmt::ObjImportResult& out,
                const fmt::ObjImportOptions& opt = {}) {
    std::string why;
    // Empty baseDir: these cases have no .mtl on disk, and the importer must not go looking in the
    // working directory for one.
    const bool ok = fmt::importObjFromMemory(text.data(), text.size(), std::string(), out, opt, &why);
    if (!ok) AVER_INFO("    (importObj said: {})", why);
    return ok;
}

// A real 1x1 RGBA PNG. Its alpha is 0x40 -- a value nothing else here produces, so a test that
// finds those bytes knows they came from THIS file and were not invented somewhere along the way.
static const u8 kPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f,
    0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x10,
    0x54, 0x32, 0x76, 0x00, 0x00, 0x01, 0x55, 0x00, 0xa7, 0x07, 0x84, 0x51, 0x51, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};
// Little-endian TIFF magic, then filler. Enough for the container sniff; it is never decoded.
static const u8 kTiff[] = { 0x49, 0x49, 0x2a, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00 };

static void putFile(const std::filesystem::path& p, const u8* b, usize n) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b), std::streamsize(n));
}

static bool usda(const std::string& text, fmt::UsdImportResult& out,
                 const fmt::UsdImportOptions& opt = {}, const std::string& baseDir = {}) {
    std::string why;
    const bool ok = fmt::importUsdFromMemory(reinterpret_cast<const u8*>(text.data()), text.size(),
                                             baseDir, out, opt, &why);
    if (!ok) AVER_INFO("    (importUsd said: {})", why);
    return ok;
}

int main() {
    AVER_INFO("=== OBJ ===");

    // ---- the basis change, the V flip and the winding reversal, on one triangle ----
    {
        // Source is right-handed +Y up, in metres. Engine is left-handed +Z up, in centimetres, and
        // the transform is engine = (-z, x, y) * 100 -- the same one GltfImport.cpp:173 applies.
        fmt::ObjImportResult r;
        const bool ok = obj(
            "v 1 2 3\n"
            "v 4 5 6\n"
            "v 7 8 9\n"
            "vt 0.25 0.75\n"
            "vt 0 0\n"
            "vt 1 1\n"
            "vn 0 1 0\n"
            "f 1/1/1 2/2/1 3/3/1\n", r);
        check(ok, "a triangle imports");
        if (ok && r.meshes.size() == 1) {
            const fmt::OcMeshData& m = r.meshes[0];
            check(m.vertexCount() == 3, "three vertices");
            checkNear(m.positions[0], -300.0f, 1e-3f, "v(1,2,3): engine x is -z, in centimetres");
            checkNear(m.positions[1],  100.0f, 1e-3f, "v(1,2,3): engine y is  x");
            checkNear(m.positions[2],  200.0f, 1e-3f, "v(1,2,3): engine z is  y");
            checkNear(m.uvs[0], 0.25f, 1e-6f, "u passes through");
            checkNear(m.uvs[1], 0.25f, 1e-6f, "v FLIPS: OBJ's origin is bottom-left, the engine's is top-left");
            checkNear(m.normals[0], 0.0f, 1e-6f, "vn(0,1,0) -> engine x");
            checkNear(m.normals[1], 0.0f, 1e-6f, "vn(0,1,0) -> engine y");
            checkNear(m.normals[2], 1.0f, 1e-6f, "vn(0,1,0) is glTF up, so engine +Z up");
            check(m.indices.size() == 3, "three indices");
            check(m.indices[0] == 0 && m.indices[1] == 2 && m.indices[2] == 1,
                  "winding REVERSES with the handedness: 0,2,1 not 0,1,2");
        } else check(false, "exactly one mesh");
    }

    // ---- convertAxes off leaves the source basis alone ----
    {
        fmt::ObjImportResult r;
        fmt::ObjImportOptions o;
        o.convertAxes = false;
        o.scale = 1.0f;
        const bool ok = obj("v 1 2 3\nv 4 5 6\nv 7 8 9\nf 1 2 3\n", r, o);
        check(ok && r.meshes.size() == 1, "convertAxes=false imports");
        if (ok && r.meshes.size() == 1) {
            checkNear(r.meshes[0].positions[0], 1.0f, 1e-6f, "x untouched");
            checkNear(r.meshes[0].positions[2], 3.0f, 1e-6f, "z untouched");
            check(r.meshes[0].indices[1] == 1, "and the winding is NOT reversed");
        }
    }

    // ---- a quad fan-triangulates ----
    {
        fmt::ObjImportResult r;
        const bool ok = obj("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3 4\n", r);
        check(ok, "a quad imports");
        if (ok && r.meshes.size() == 1) {
            check(r.meshes[0].indices.size() == 6, "one quad becomes two triangles");
            check(r.meshes[0].vertexCount() == 4, "sharing four vertices, not six");
            bool told = false;
            for (const std::string& u : r.unsupported)
                if (u.find("more than three sides") != std::string::npos) told = true;
            check(told, "and the concave-fan caveat is REPORTED rather than hidden");
        }
    }

    // ---- negative indices count back from what has been declared so far ----
    {
        fmt::ObjImportResult r;
        const bool ok = obj("v 1 0 0\nv 0 1 0\nv 0 0 1\nf -3 -2 -1\n", r);
        check(ok && r.meshes.size() == 1, "negative (relative) indices import");
        if (ok && r.meshes.size() == 1)
            check(r.meshes[0].vertexCount() == 3, "resolving to the three declared vertices");
    }

    // ---- one position with two normals is TWO vertices: that is what a hard edge is ----
    {
        fmt::ObjImportResult r;
        const bool ok = obj(
            "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
            "vn 0 0 1\nvn 1 0 0\n"
            "f 1//1 2//1 3//1\n"
            "f 1//2 2//2 3//2\n", r);
        check(ok, "a hard edge imports");
        if (ok && r.meshes.size() == 1)
            check(r.meshes[0].vertexCount() == 6,
                  "the same position with a different normal is a SEPARATE vertex (6, not 3) -- "
                  "merging them would smooth every hard edge in the file");
    }

    // ---- usemtl runs become submeshes, and a repeated material reuses its slot ----
    {
        fmt::ObjImportResult r;
        const bool ok = obj(
            "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\n"
            "usemtl red\nf 1 2 3\n"
            "usemtl blue\nf 2 3 4\n"
            "usemtl red\nf 1 2 4\n", r);
        check(ok, "usemtl runs import");
        if (ok && r.meshes.size() == 1) {
            const fmt::OcMeshData& m = r.meshes[0];
            check(m.submeshes.size() == 3, "three runs become three submeshes");
            check(m.materialSlots.size() == 2, "but only two material SLOTS");
            if (m.submeshes.size() == 3)
                check(m.submeshes[0].materialSlot == m.submeshes[2].materialSlot,
                      "and the repeated material reuses its slot");
        }
    }

    // ---- `o` splits the file into separate meshes, sharing the file-global vertex pool ----
    {
        fmt::ObjImportResult r;
        const bool ok = obj(
            "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
            "o first\nf 1 2 3\n"
            "o second\nf 3 2 1\n", r);
        check(ok, "two objects import");
        check(r.meshes.size() == 2, "as two meshes");
        if (r.meshes.size() == 2) {
            check(r.meshNames[0] == "first" && r.meshNames[1] == "second", "keeping their names");
            check(r.meshes[1].vertexCount() == 3,
                  "the second object re-indexes the FILE-GLOBAL vertices into its own array");
        }
    }

    // ---- a file with no faces is an error, not an empty success ----
    {
        fmt::ObjImportResult r;
        std::string why;
        const std::string text = "v 0 0 0\nv 1 0 0\n";
        const bool ok = fmt::importObjFromMemory(text.data(), text.size(), std::string(), r, {}, &why);
        check(!ok, "an OBJ with no faces FAILS rather than returning an empty mesh");
        check(!why.empty(), "and says why");
    }

    // ---- .mtl: options before the filename, and the PBR extensions ----
    //
    // Through importMtl on a real file, because that IS the public entry point -- a test that
    // reached the parser some other way would not exercise the path anything actually calls.
    {
        const std::string path = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") +
                                 "/aver-import-test.mtl";
        {
            std::ofstream f(path, std::ios::binary);
            f << "newmtl bark\n"
                 "Kd 0.4 0.26 0.13\n"
                 "Ns 30\n"
                 "Pr 0.8\n"
                 "Pm 0.0\n"
                 // The filename comes AFTER a run of options, which is what makes "the last token"
                 // the wrong rule and this case worth having.
                 "map_Kd -bm 0.2 -o 1 1 1 T_Bark_BC.png\n"
                 "map_Bump T_Bark_N.png\n"
                 "newmtl leaf\n"
                 "Kd 0.2 0.5 0.15\n"
                 "d 0.5\n";
        }
        std::vector<fmt::ObjMaterial> mats;
        std::string why;
        const bool ok = fmt::importMtl(path, mats, &why);
        check(ok, "a .mtl imports");
        check(mats.size() == 2, "two materials");
        if (mats.size() == 2) {
            check(mats[0].name == "bark" && mats[1].name == "leaf", "keeping their names in file order");
            checkNear(mats[0].baseColor[1], 0.26f, 1e-6f, "Kd is read");
            checkNear(mats[0].roughness, 0.8f, 1e-6f, "the Pr PBR extension is read");
            check(mats[0].hasPbr, "and flagged as PBR");
            check(mats[0].mapBaseColor == "T_Bark_BC.png",
                  "map_Kd skips its options and keeps only the filename");
            check(mats[0].mapNormal == "T_Bark_N.png", "map_Bump is taken as the normal map");
            checkNear(mats[1].opacity, 0.5f, 1e-6f, "d is read as opacity");
            check(!mats[1].hasPbr, "a material with no Pr/Pm is not flagged as PBR");
        }
        std::remove(path.c_str());
    }

    AVER_INFO("=== USDA ===");

    // ---- a Y-up stage in metres ----
    {
        fmt::UsdImportResult r;
        const bool ok = usda(
            "#usda 1.0\n"
            "(\n"
            "    upAxis = \"Y\"\n"
            "    metersPerUnit = 1\n"
            ")\n"
            "def Mesh \"tri\"\n"
            "{\n"
            "    int[] faceVertexCounts = [3]\n"
            "    int[] faceVertexIndices = [0, 1, 2]\n"
            "    point3f[] points = [(1, 2, 3), (4, 5, 6), (7, 8, 9)]\n"
            "    texCoord2f[] primvars:st = [(0.25, 0.75), (0, 0), (1, 1)]\n"
            "}\n", r);
        check(ok, "a Y-up USDA triangle imports");
        check(r.encoding == fmt::UsdEncoding::Usda, "sniffed as USDA");
        check(r.sourceUpAxis == "Y", "the stage's upAxis is reported");
        if (ok && r.meshes.size() == 1) {
            const fmt::OcMeshData& m = r.meshes[0];
            check(m.vertexCount() == 3, "three vertices (one per face corner)");
            checkNear(m.positions[0], -300.0f, 1e-3f, "Y-up gets the same (-z, x, y) as glTF/OBJ");
            checkNear(m.positions[1],  100.0f, 1e-3f, "engine y is source x");
            checkNear(m.positions[2],  200.0f, 1e-3f, "engine z is source y");
            checkNear(m.uvs[1], 0.25f, 1e-6f, "and v flips");
        } else check(false, "exactly one mesh");
    }

    // ---- a Z-up stage already agrees about up, and needs only the handedness flip ----
    {
        fmt::UsdImportResult r;
        const bool ok = usda(
            "#usda 1.0\n"
            "(\n"
            "    upAxis = \"Z\"\n"
            "    metersPerUnit = 0.01\n"
            ")\n"
            "def Mesh \"tri\"\n"
            "{\n"
            "    int[] faceVertexCounts = [3]\n"
            "    int[] faceVertexIndices = [0, 1, 2]\n"
            "    point3f[] points = [(1, 2, 3), (4, 5, 6), (7, 8, 9)]\n"
            "}\n", r);
        check(ok, "a Z-up USDA triangle imports");
        check(r.sourceUpAxis == "Z", "the stage's Z-up is honoured, not assumed away");
        if (ok && r.meshes.size() == 1) {
            // metersPerUnit 0.01 is already centimetres, so 0.01 * 100 = exactly 1.
            checkNear(r.meshes[0].positions[0],  1.0f, 1e-4f, "Z-up keeps x");
            checkNear(r.meshes[0].positions[1], -2.0f, 1e-4f, "and negates y for the handedness flip");
            checkNear(r.meshes[0].positions[2],  3.0f, 1e-4f, "and keeps z, because up already agrees");
        }
    }

    // ---- metersPerUnit folds with the caller's scale ----
    {
        fmt::UsdImportResult r;
        const bool ok = usda(
            "#usda 1.0\n(\n    upAxis = \"Z\"\n    metersPerUnit = 1\n)\n"
            "def Mesh \"t\"\n{\n"
            "    int[] faceVertexCounts = [3]\n"
            "    int[] faceVertexIndices = [0, 1, 2]\n"
            "    point3f[] points = [(1, 0, 0), (0, 1, 0), (0, 0, 1)]\n"
            "}\n", r);
        check(ok, "a metre-unit stage imports");
        checkNear(r.sourceMetersPerUnit, 1.0f, 1e-6f, "metersPerUnit is reported");
        if (ok && r.meshes.size() == 1)
            checkNear(r.meshes[0].positions[0], 100.0f, 1e-3f,
                      "one metre becomes 100 cm: the stage's units fold with the option");
    }

    // ---- a quad triangulates, and xformOp:translate is applied ----
    {
        fmt::UsdImportResult r;
        const bool ok = usda(
            "#usda 1.0\n(\n    upAxis = \"Z\"\n    metersPerUnit = 0.01\n)\n"
            "def Xform \"root\"\n{\n"
            "    double3 xformOp:translate = (10, 0, 0)\n"
            "    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
            "    def Mesh \"quad\"\n    {\n"
            "        int[] faceVertexCounts = [4]\n"
            "        int[] faceVertexIndices = [0, 1, 2, 3]\n"
            "        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]\n"
            "    }\n"
            "}\n", r);
        check(ok, "a nested Mesh under an Xform imports");
        if (ok && r.meshes.size() == 1) {
            check(r.meshes[0].indices.size() == 6, "the quad becomes two triangles");
            checkNear(r.meshes[0].positions[0], 10.0f, 1e-3f,
                      "and the PARENT Xform's translate reached the child's points");
            check(r.meshNames[0] == "/root/quad", "the mesh is named by its prim path");
        }
    }

    // ---- the refusals name the encoding instead of returning an empty mesh ----
    {
        fmt::UsdImportResult r;
        std::string why;
        const char crate[] = "PXR-USDC\0\0\0\0";
        const bool ok = fmt::importUsdFromMemory(reinterpret_cast<const u8*>(crate), sizeof(crate) - 1,
                                                 {}, r, {}, &why);
        check(!ok, "a USDC crate is REFUSED");
        check(r.encoding == fmt::UsdEncoding::Usdc, "and identified as USDC");
        check(why.find("usdcat") != std::string::npos, "with the conversion command in the message");
    }
    {
        fmt::UsdImportResult r;
        std::string why;
        const char zip[] = "PK\x03\x04rest";
        const bool ok = fmt::importUsdFromMemory(reinterpret_cast<const u8*>(zip), sizeof(zip) - 1,
                                                 {}, r, {}, &why);
        check(!ok, "a USDZ archive is REFUSED");
        check(r.encoding == fmt::UsdEncoding::Usdz, "and identified as USDZ");
    }
    {
        fmt::UsdImportResult r;
        std::string why;
        const char junk[] = "this is not a usd file at all";
        const bool ok = fmt::importUsdFromMemory(reinterpret_cast<const u8*>(junk), sizeof(junk) - 1,
                                                 {}, r, {}, &why);
        check(!ok, "and a non-USD file is refused too");
    }

    // ---- a stage that parses but composes nothing is an ERROR, not a silent empty import ----
    {
        fmt::UsdImportResult r;
        std::string why;
        const std::string text =
            "#usda 1.0\n(\n    upAxis = \"Z\"\n)\n"
            "def Xform \"empty\"\n{\n}\n";
        const bool ok = fmt::importUsdFromMemory(reinterpret_cast<const u8*>(text.data()), text.size(),
                                                 {}, r, {}, &why);
        check(!ok, "a stage with no UsdGeomMesh FAILS rather than succeeding with zero meshes");
        check(why.find("no UsdGeomMesh") != std::string::npos, "and says exactly that");
    }

    // ---- subdivision is reported, because importing a control cage as-is looks like a bug ----
    {
        fmt::UsdImportResult r;
        const bool ok = usda(
            "#usda 1.0\n(\n    upAxis = \"Z\"\n)\n"
            "def Mesh \"sub\"\n{\n"
            "    uniform token subdivisionScheme = \"catmullClark\"\n"
            "    int[] faceVertexCounts = [3]\n"
            "    int[] faceVertexIndices = [0, 1, 2]\n"
            "    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n"
            "}\n", r);
        check(ok, "a subdivision mesh imports its control cage");
        bool told = false;
        for (const std::string& u : r.unsupported)
            if (u.find("subdivisionScheme") != std::string::npos) told = true;
        check(told, "and says it was NOT subdivided");
    }

    // ---- .mtl -> the shared import shape, which is what finally gives OBJ materials -----------
    //
    // These go through objMaterialsToImported rather than importObj, because the conversion is the
    // part with judgement in it: the parse just records what the file said.
    {
        std::vector<fmt::ObjMaterial> in;
        fmt::ObjMaterial phong;
        phong.name = "Classic";
        phong.baseColor[0] = 0.8f; phong.baseColor[1] = 0.2f; phong.baseColor[2] = 0.1f;
        phong.emissive[1] = 0.5f;
        phong.shininess = 200.0f;
        phong.opacity = 0.5f;
        in.push_back(phong);

        fmt::ObjMaterial pbr;
        pbr.name = "Modern";
        pbr.hasPbr = true;
        pbr.roughness = 0.35f;
        pbr.metallic = 1.0f;
        in.push_back(pbr);

        std::vector<fmt::ImportedMaterial> mats;
        std::vector<fmt::ImportedImage> imgs;
        std::vector<std::string> warn;
        fmt::objMaterialsToImported(in, {}, mats, imgs, &warn);

        check(mats.size() == 2, "both .mtl materials converted");
        if (mats.size() == 2) {
            check(std::fabs(mats[0].baseColorFactor[0] - 0.8f) < 1e-5f, "Kd became baseColorFactor");
            check(std::fabs(mats[0].baseColorFactor[3] - 0.5f) < 1e-5f, "and d became its alpha");
            check(mats[0].alphaMode == "BLEND", "which makes it BLEND -- .mtl has no cutoff to mean MASK");
            check(std::fabs(mats[0].emissiveFactor[1] - 0.5f) < 1e-5f, "Ke became emissiveFactor");
            // NO Pr/Pm, so roughness is DERIVED from Ns: sqrt(2 / (200 + 2)) = 0.0995. Leaving the
            // shared struct's default of 1.0 standing would make every classic .mtl uniformly matte,
            // and its metallic default of 1.0 would make every one of them metal.
            check(std::fabs(mats[0].roughnessFactor - 0.0995037f) < 1e-4f,
                  "and with no Pr, roughness is derived from the Phong exponent Ns");
            check(mats[0].metallicFactor == 0.0f, "with metallic 0: a .mtl with no Pm is a dielectric");

            check(std::fabs(mats[1].roughnessFactor - 0.35f) < 1e-5f, "a stated Pr wins outright");
            check(mats[1].metallicFactor == 1.0f, "as does a stated Pm");
            check(mats[1].alphaMode == "OPAQUE", "and d of 1 stays opaque");
        }
    }

    // ---- a texture the .mtl names but which is not there -------------------------------------
    {
        std::vector<fmt::ObjMaterial> in;
        fmt::ObjMaterial m;
        m.name = "Missing";
        m.mapBaseColor = "no_such_file_anywhere.png";
        in.push_back(m);

        std::vector<fmt::ImportedMaterial> mats;
        std::vector<fmt::ImportedImage> imgs;
        std::vector<std::string> warn;
        fmt::objMaterialsToImported(in, {}, mats, imgs, &warn);

        check(mats.size() == 1 && mats[0].baseColorTex.empty(),
              "a texture that cannot be read leaves its slot UNBOUND");
        bool named = false;
        for (const std::string& w : warn)
            if (w.find("no_such_file_anywhere.png") != std::string::npos) named = true;
        check(named, "and the file is named in the warnings, not swallowed");
    }

    // ---- map_Pr and map_Pm both stated, when .ocmat has one packed slot -----------------------
    {
        std::vector<fmt::ObjMaterial> in;
        fmt::ObjMaterial m;
        m.name = "Both";
        m.mapRoughness = "r.png";
        m.mapMetallic  = "m.png";
        in.push_back(m);

        std::vector<fmt::ImportedMaterial> mats;
        std::vector<fmt::ImportedImage> imgs;
        std::vector<std::string> warn;
        fmt::objMaterialsToImported(in, {}, mats, imgs, &warn);

        bool told = false;
        for (const std::string& w : warn)
            if (w.find("map_Pm dropped") != std::string::npos) told = true;
        check(told, "naming separate map_Pr and map_Pm says which one was dropped");
    }

    // ---- UsdPreviewSurface: the values, the connections, and the mesh->material join ----------
    //
    // The three meshes are the three outcomes the join can have, and they are here together because
    // the bound one alone would pass just as well with the resolution deleted and every slot filled
    // by accident: `Unbound` proves an unbound mesh stays unbound, and `Dangling` proves a binding
    // this file cannot satisfy does NOT invent a name.
    {
        fmt::UsdImportResult r;
        const bool ok = usda(
            "#usda 1.0\n(\n    upAxis = \"Y\"\n)\n"
            "def Xform \"root\"\n{\n"
            "    def Mesh \"Bound\"\n    {\n"
            "        rel material:binding = </root/Looks/Bark>\n"
            "        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 0, 1)]\n"
            "        int[] faceVertexCounts = [3]\n"
            "        int[] faceVertexIndices = [0, 1, 2]\n"
            "    }\n"
            "    def Mesh \"Unbound\"\n    {\n"
            "        point3f[] points = [(2, 0, 0), (3, 0, 0), (2, 0, 1)]\n"
            "        int[] faceVertexCounts = [3]\n"
            "        int[] faceVertexIndices = [0, 1, 2]\n"
            "    }\n"
            "    def Mesh \"Dangling\"\n    {\n"
            "        prepend rel material:binding = </root/Looks/NotHere>\n"
            "        point3f[] points = [(4, 0, 0), (5, 0, 0), (4, 0, 1)]\n"
            "        int[] faceVertexCounts = [3]\n"
            "        int[] faceVertexIndices = [0, 1, 2]\n"
            "    }\n"
            "    def Scope \"Looks\"\n    {\n"
            "        def Material \"Bark\"\n        {\n"
            "            token outputs:surface.connect = </root/Looks/Bark/Surface.outputs:surface>\n"
            "            def Shader \"Surface\"\n            {\n"
            "                uniform token info:id = \"UsdPreviewSurface\"\n"
            "                color3f inputs:diffuseColor = (0.25, 0.5, 0.75)\n"
            "                color3f inputs:emissiveColor = (0.1, 0.2, 0.3)\n"
            "                float inputs:roughness = 0.7\n"
            "                float inputs:metallic = 0.25\n"
            "                normal3f inputs:normal.connect = </root/Looks/Bark/Nrm.outputs:rgb>\n"
            "                token outputs:surface\n"
            "            }\n"
            "            def Shader \"Nrm\"\n            {\n"
            "                uniform token info:id = \"UsdUVTexture\"\n"
            "                asset inputs:file = @./textures/bark_n.png@\n"
            "                float3 outputs:rgb\n"
            "            }\n"
            "        }\n"
            "    }\n"
            "}\n", r);
        check(ok, "a stage with a Looks scope imports");
        check(r.meshes.size() == 3, "all three meshes came through");
        check(r.materials.size() == 1, "and its one UsdPreviewSurface became one material");
        if (r.materials.size() == 1) {
            const fmt::ImportedMaterial& m = r.materials[0];
            check(m.name == "Bark", "named after the Material prim");
            check(std::fabs(m.baseColorFactor[0] - 0.25f) < 1e-5f &&
                  std::fabs(m.baseColorFactor[1] - 0.5f)  < 1e-5f &&
                  std::fabs(m.baseColorFactor[2] - 0.75f) < 1e-5f,
                  "diffuseColor became baseColorFactor");
            check(std::fabs(m.emissiveFactor[2] - 0.3f) < 1e-5f, "emissiveColor came across");
            check(std::fabs(m.roughnessFactor - 0.7f) < 1e-5f, "roughness came across");
            check(std::fabs(m.metallicFactor - 0.25f) < 1e-5f, "metallic came across");
            check(m.alphaMode == "OPAQUE", "and with no opacity stated it is opaque");
            // The texture file is not on disk in this test, so the SLOT must stay unset -- a
            // material pointing at a file that is not there reads downstream as a renderer fault
            // rather than the import failure it is.
            check(m.normalTex.empty(), "a normal map that could not be read leaves its slot UNBOUND");
        }
        bool toldTexture = false, toldBinding = false;
        for (const std::string& u : r.unsupported) {
            if (u.find("could not be read") != std::string::npos) toldTexture = true;
            if (u.find("does not declare") != std::string::npos)  toldBinding = true;
        }
        check(toldTexture, "and names the unreadable texture as unsupported");

        // THE JOIN.
        if (r.meshes.size() == 3) {
            check(!r.meshes[0].materialSlots.empty() && r.meshes[0].materialSlots[0] == "Bark",
                  "the bound mesh got the material's name in its slot");
            check(!r.meshes[1].materialSlots.empty() && r.meshes[1].materialSlots[0].empty(),
                  "the mesh with NO binding still has an empty slot");
            check(!r.meshes[2].materialSlots.empty() && r.meshes[2].materialSlots[0].empty(),
                  "and a binding this file cannot satisfy leaves the slot empty rather than guessing");
        }
        check(toldBinding, "which is reported, not silent");
    }

    // ---- UsdPreviewSurface's own defaults, which are NOT ImportedMaterial's -------------------
    //
    // THIS TEST HAS TO STATE ONE AND OMIT THE OTHER. A surface stating neither would pass against
    // the wrong defaults too, because the two omissions are indistinguishable from a struct that
    // happened to be initialised the same way. Roughness is stated, metallic is not: if the shared
    // struct's metallic default of 1.0 (glTF's) were left standing, this material would arrive as a
    // fully rough metal instead of the dielectric the file describes.
    {
        fmt::UsdImportResult r;
        const bool ok = usda(
            "#usda 1.0\n(\n    upAxis = \"Z\"\n)\n"
            "def Mesh \"m\"\n{\n"
            "    rel material:binding = </Look>\n"
            "    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n"
            "    int[] faceVertexCounts = [3]\n"
            "    int[] faceVertexIndices = [0, 1, 2]\n"
            "}\n"
            "def Material \"Look\"\n{\n"
            "    def Shader \"S\"\n    {\n"
            "        uniform token info:id = \"UsdPreviewSurface\"\n"
            "        float inputs:roughness = 0.9\n"
            "    }\n"
            "}\n", r);
        check(ok, "a Material at stage scope with no outputs:surface still imports");
        check(r.materials.size() == 1, "falling back to its single UsdPreviewSurface");
        if (r.materials.size() == 1) {
            check(std::fabs(r.materials[0].roughnessFactor - 0.9f) < 1e-5f, "the stated roughness wins");
            check(r.materials[0].metallicFactor == 0.0f,
                  "and an UNSTATED metallic is 0, UsdPreviewSurface's default, not the struct's 1");
        }
        if (!r.meshes.empty() && !r.meshes[0].materialSlots.empty())
            check(r.meshes[0].materialSlots[0] == "Look",
                  "and a binding to a stage-scope Material resolves");
    }

    // ---- opacity and opacityThreshold, USD's only two blend signals ---------------------------
    {
        fmt::UsdImportResult r;
        const bool ok = usda(
            "#usda 1.0\n(\n    upAxis = \"Z\"\n)\n"
            "def Mesh \"m\"\n{\n"
            "    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n"
            "    int[] faceVertexCounts = [3]\n"
            "    int[] faceVertexIndices = [0, 1, 2]\n"
            "}\n"
            "def Material \"Fade\"\n{\n"
            "    def Shader \"S\"\n    {\n"
            "        uniform token info:id = \"UsdPreviewSurface\"\n"
            "        float inputs:opacity = 0.4\n"
            "    }\n"
            "}\n"
            "def Material \"Cutout\"\n{\n"
            "    def Shader \"S\"\n    {\n"
            "        uniform token info:id = \"UsdPreviewSurface\"\n"
            "        float inputs:opacityThreshold = 0.33\n"
            "    }\n"
            "}\n", r);
        check(ok, "two materials on one stage import");
        check(r.materials.size() == 2, "as two");
        if (r.materials.size() == 2) {
            check(r.materials[0].alphaMode == "BLEND" &&
                  std::fabs(r.materials[0].baseColorFactor[3] - 0.4f) < 1e-5f,
                  "a partial opacity becomes BLEND with alpha in baseColorFactor.a");
            check(r.materials[1].alphaMode == "MASK" &&
                  std::fabs(r.materials[1].alphaCutoff - 0.33f) < 1e-5f,
                  "and opacityThreshold becomes MASK at that cutoff");
        }
    }

    // ---- a shading model this importer does not read is refused BY NAME, not guessed at --------
    {
        fmt::UsdImportResult r;
        const bool ok = usda(
            "#usda 1.0\n(\n    upAxis = \"Z\"\n)\n"
            "def Mesh \"m\"\n{\n"
            "    rel material:binding = </Fancy>\n"
            "    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n"
            "    int[] faceVertexCounts = [3]\n"
            "    int[] faceVertexIndices = [0, 1, 2]\n"
            "}\n"
            "def Material \"Fancy\"\n{\n"
            "    def Shader \"S\"\n    {\n"
            "        uniform token info:id = \"PxrSurface\"\n"
            "        color3f inputs:diffuseColor = (1, 0, 0)\n"
            "    }\n"
            "}\n", r);
        check(ok, "a stage whose only Material is a renderer-specific shader still imports geometry");
        check(r.materials.empty(), "and produces NO material rather than a guess at one");
        bool told = false;
        for (const std::string& u : r.unsupported)
            if (u.find("UsdPreviewSurface") != std::string::npos) told = true;
        check(told, "saying which shading model it does read");
    }

    // ---- two Material prims may share a name; the slot rewrite downstream matches on it --------
    {
        fmt::UsdImportResult r;
        const bool ok = usda(
            "#usda 1.0\n(\n    upAxis = \"Z\"\n)\n"
            "def Mesh \"m\"\n{\n"
            "    rel material:binding = </B/Look>\n"
            "    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n"
            "    int[] faceVertexCounts = [3]\n"
            "    int[] faceVertexIndices = [0, 1, 2]\n"
            "}\n"
            "def Scope \"A\"\n{\n    def Material \"Look\"\n    {\n"
            "        def Shader \"S\"\n        {\n"
            "            uniform token info:id = \"UsdPreviewSurface\"\n"
            "            float inputs:roughness = 0.1\n"
            "        }\n    }\n}\n"
            "def Scope \"B\"\n{\n    def Material \"Look\"\n    {\n"
            "        def Shader \"S\"\n        {\n"
            "            uniform token info:id = \"UsdPreviewSurface\"\n"
            "            float inputs:roughness = 0.9\n"
            "        }\n    }\n}\n", r);
        check(ok, "two same-named Materials in different scopes import");
        check(r.materials.size() == 2, "as two materials");
        if (r.materials.size() == 2)
            check(r.materials[0].name != r.materials[1].name,
                  "with DISTINCT names, or the slot rewrite would give both meshes the first look");
        if (!r.meshes.empty() && !r.meshes[0].materialSlots.empty() && r.materials.size() == 2)
            check(r.meshes[0].materialSlots[0] == r.materials[1].name,
                  "and the binding picked the one it actually named");
    }

    // ---- the copies a DCC writes per object collapse to one look ------------------------------
    //
    // Blender's USD exporter puts a full copy of a Material under EVERY object that uses it, which
    // is why Jungle Ruins' grass_B_classes.usda declares MI_Grass_02_TwoSided five times. Five
    // .ocmat files for one look would be waste; five DIFFERENT NAMES for it would be worse, because
    // the same plant would then arrive wearing four renamed copies of its own material.
    {
        fmt::UsdImportResult r;
        const std::string one =
            "        def Material \"Shared\"\n        {\n"
            "            def Shader \"S\"\n            {\n"
            "                uniform token info:id = \"UsdPreviewSurface\"\n"
            "                color3f inputs:diffuseColor = (0.3, 0.6, 0.2)\n"
            "                float inputs:roughness = 0.8\n"
            "            }\n        }\n";
        const bool ok = usda(
            "#usda 1.0\n(\n    upAxis = \"Z\"\n)\n"
            "def Xform \"A\"\n{\n"
            "    def Mesh \"m\"\n    {\n"
            "        rel material:binding = </A/Looks/Shared>\n"
            "        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n"
            "        int[] faceVertexCounts = [3]\n"
            "        int[] faceVertexIndices = [0, 1, 2]\n"
            "    }\n"
            "    def Scope \"Looks\"\n    {\n" + one + "    }\n}\n"
            "def Xform \"B\"\n{\n"
            "    def Mesh \"m\"\n    {\n"
            "        rel material:binding = </B/Looks/Shared>\n"
            "        point3f[] points = [(2, 0, 0), (3, 0, 0), (2, 1, 0)]\n"
            "        int[] faceVertexCounts = [3]\n"
            "        int[] faceVertexIndices = [0, 1, 2]\n"
            "    }\n"
            "    def Scope \"Looks\"\n    {\n" + one + "    }\n}\n", r);
        check(ok, "two objects each carrying their own copy of one material import");
        check(r.materials.size() == 1, "as ONE material, because the two bodies are identical");
        if (r.materials.size() == 1)
            check(r.materials[0].name == "Shared", "keeping the prim's own name, un-disambiguated");
        if (r.meshes.size() == 2 && r.materials.size() == 1) {
            check(r.meshes[0].materialSlots[0] == "Shared" &&
                  r.meshes[1].materialSlots[0] == "Shared",
                  "and BOTH bindings resolve to it, from their two different prim paths");
        }
    }

    // ---- doubleSided is authored on the GEOMETRY in USD and on the MATERIAL in .ocmat ----------
    {
        fmt::UsdImportResult r;
        const bool ok = usda(
            "#usda 1.0\n(\n    upAxis = \"Z\"\n)\n"
            "def Mesh \"card\"\n{\n"
            "    uniform bool doubleSided = 1\n"
            "    rel material:binding = </Leaf>\n"
            "    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n"
            "    int[] faceVertexCounts = [3]\n"
            "    int[] faceVertexIndices = [0, 1, 2]\n"
            "}\n"
            "def Mesh \"solid\"\n{\n"
            "    rel material:binding = </Rock>\n"
            "    point3f[] points = [(2, 0, 0), (3, 0, 0), (2, 1, 0)]\n"
            "    int[] faceVertexCounts = [3]\n"
            "    int[] faceVertexIndices = [0, 1, 2]\n"
            "}\n"
            "def Material \"Leaf\"\n{\n    def Shader \"S\"\n    {\n"
            "        uniform token info:id = \"UsdPreviewSurface\"\n"
            "        float inputs:roughness = 0.8\n"
            "    }\n}\n"
            "def Material \"Rock\"\n{\n    def Shader \"S\"\n    {\n"
            "        uniform token info:id = \"UsdPreviewSurface\"\n"
            "        float inputs:roughness = 0.2\n"
            "    }\n}\n", r);
        check(ok, "a doubleSided mesh imports");
        check(r.materials.size() == 2, "with both materials");
        if (r.materials.size() == 2) {
            check(r.materials[0].doubleSided,
                  "and the mesh's doubleSided crossed onto the material it bound");
            check(!r.materials[1].doubleSided,
                  "while a material bound only by a single-sided mesh stays single-sided");
        }
    }

    // ---- GeomSubsets: several materials on one mesh -------------------------------------------
    //
    // This is how every tree in Intel's Jungle Ruins is authored -- trunk, branches and leaves are
    // three face subsets of a single Mesh -- and it is the difference between importing a tree and
    // importing a tree-shaped piece of bark. The failure has no error: the mesh-level binding wins
    // for every face and the leaf material is simply never referenced.
    //
    // FOUR faces, so the subsets can be non-contiguous: 0 and 2 to one material, 1 and 3 to another.
    // Contiguous subsets would pass even if the reordering below were a no-op.
    {
        fmt::UsdImportResult r;
        const bool ok = usda(
            "#usda 1.0\n(\n    upAxis = \"Z\"\n)\n"
            "def Mesh \"tree\"\n{\n"
            "    uniform bool doubleSided = 1\n"
            "    rel material:binding = </Bark>\n"
            "    point3f[] points = [(0,0,0), (1,0,0), (2,0,0), (3,0,0), (4,0,0),\n"
            "                        (0,1,0), (1,1,0), (2,1,0), (3,1,0), (4,1,0)]\n"
            "    int[] faceVertexCounts = [3, 3, 3, 3]\n"
            "    int[] faceVertexIndices = [0,1,5,  1,2,6,  2,3,7,  3,4,8]\n"
            "    def GeomSubset \"Bark\"\n    {\n"
            "        uniform token elementType = \"face\"\n"
            "        uniform token familyName = \"materialBind\"\n"
            "        int[] indices = [0, 2]\n"
            "        rel material:binding = </Bark>\n"
            "    }\n"
            "    def GeomSubset \"Leaves\"\n    {\n"
            "        uniform token elementType = \"face\"\n"
            "        uniform token familyName = \"materialBind\"\n"
            "        int[] indices = [1, 3]\n"
            "        rel material:binding = </Leaves>\n"
            "    }\n"
            "}\n"
            "def Material \"Bark\"\n{\n    def Shader \"S\"\n    {\n"
            "        uniform token info:id = \"UsdPreviewSurface\"\n"
            "        float inputs:roughness = 0.9\n    }\n}\n"
            "def Material \"Leaves\"\n{\n    def Shader \"S\"\n    {\n"
            "        uniform token info:id = \"UsdPreviewSurface\"\n"
            "        float inputs:roughness = 0.2\n    }\n}\n", r);
        check(ok, "a mesh with GeomSubsets imports");
        check(r.meshes.size() == 1, "as ONE mesh");
        if (r.meshes.size() == 1) {
            const fmt::OcMeshData& m = r.meshes[0];
            check(m.submeshes.size() == 2, "with one submesh per subset (got " +
                  std::to_string(m.submeshes.size()) + ")");
            check(m.materialSlots.size() == 2, "and one material slot per submesh");
            check(m.indices.size() == 12, "with every triangle kept: 4 faces, 12 indices");
            if (m.submeshes.size() == 2 && m.materialSlots.size() == 2) {
                check(m.materialSlots[0] == "Bark" && m.materialSlots[1] == "Leaves",
                      "each slot resolved to ITS subset's material, in declaration order");
                // THE REORDER IS THE POINT. Faces 0 and 2 are not adjacent in the source, so a
                // submesh that is a contiguous [start, count) run can only exist if the index
                // buffer was rebuilt. Overlapping or zero-length runs would mean it was not.
                check(m.submeshes[0].indexStart == 0 && m.submeshes[0].indexCount == 6,
                      "the first subset is a contiguous run from 0");
                check(m.submeshes[1].indexStart == 6 && m.submeshes[1].indexCount == 6,
                      "and the second follows it, so scattered faces became contiguous runs");
                check(m.submeshes[0].vertexCount == m.vertexCount() &&
                      m.submeshes[1].vertexCount == m.vertexCount(),
                      "both span the whole vertex buffer, as the glTF path's submeshes do");
            }
        }
        if (r.materials.size() == 2) {
            check(r.materials[0].doubleSided && r.materials[1].doubleSided,
                  "and the mesh's doubleSided reached BOTH of its subsets' materials");
        }
    }

    // ---- a subset that does not cover every face keeps the rest, and says so -------------------
    {
        fmt::UsdImportResult r;
        const bool ok = usda(
            "#usda 1.0\n(\n    upAxis = \"Z\"\n)\n"
            "def Mesh \"partial\"\n{\n"
            "    rel material:binding = </Rest>\n"
            "    point3f[] points = [(0,0,0), (1,0,0), (2,0,0), (0,1,0), (1,1,0)]\n"
            "    int[] faceVertexCounts = [3, 3]\n"
            "    int[] faceVertexIndices = [0,1,3,  1,2,4]\n"
            "    def GeomSubset \"Half\"\n    {\n"
            "        uniform token elementType = \"face\"\n"
            "        int[] indices = [0]\n"
            "        rel material:binding = </Half>\n"
            "    }\n"
            "}\n"
            "def Material \"Half\"\n{\n    def Shader \"S\"\n    {\n"
            "        uniform token info:id = \"UsdPreviewSurface\"\n    }\n}\n"
            "def Material \"Rest\"\n{\n    def Shader \"S\"\n    {\n"
            "        uniform token info:id = \"UsdPreviewSurface\"\n"
            "        float inputs:roughness = 0.3\n    }\n}\n", r);
        check(ok, "a mesh whose subsets cover only some faces imports");
        if (!r.meshes.empty()) {
            const fmt::OcMeshData& m = r.meshes[0];
            check(m.submeshes.size() == 2,
                  "the uncovered faces become their own submesh rather than vanishing");
            check(m.indices.size() == 6, "so no triangle is lost");
            if (m.materialSlots.size() == 2)
                check(m.materialSlots[1] == "Rest",
                      "and they keep the MESH-level binding, not the subset's");
        }
        bool told = false;
        for (const std::string& u : r.unsupported)
            if (u.find("did not cover every face") != std::string::npos) told = true;
        check(told, "which is reported rather than left for the eye to find");
    }

    // ---- a subset this importer cannot act on is ignored, not half-applied ---------------------
    {
        fmt::UsdImportResult r;
        const bool ok = usda(
            "#usda 1.0\n(\n    upAxis = \"Z\"\n)\n"
            "def Mesh \"pts\"\n{\n"
            "    rel material:binding = </Only>\n"
            "    point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]\n"
            "    int[] faceVertexCounts = [3]\n"
            "    int[] faceVertexIndices = [0,1,2]\n"
            "    def GeomSubset \"Verts\"\n    {\n"
            "        uniform token elementType = \"point\"\n"
            "        int[] indices = [0, 1]\n"
            "        rel material:binding = </Other>\n"
            "    }\n"
            "}\n"
            "def Material \"Only\"\n{\n    def Shader \"S\"\n    {\n"
            "        uniform token info:id = \"UsdPreviewSurface\"\n    }\n}\n", r);
        check(ok, "a POINT subset does not break the import");
        if (!r.meshes.empty()) {
            check(r.meshes[0].submeshes.size() == 1,
                  "it is ignored -- .ocmesh has no submesh concept for a set of vertices");
            if (!r.meshes[0].materialSlots.empty())
                check(r.meshes[0].materialSlots[0] == "Only",
                      "and the mesh-level binding still applies to the whole thing");
        }
    }

    // ---- a SEPARATE opacity map is recorded, because .ocmat has nowhere to put it -------------
    //
    // The fold into base-colour alpha happens in AverAssetC, where a decoder is; what the importer
    // owes is the FACT that the file named one. Losing it here renders every leaf on a tree with a
    // JPEG albedo as a solid quad, and a JPEG cannot carry alpha at all.
    {
        fmt::UsdImportResult r;
        const bool ok = usda(
            "#usda 1.0\n(\n    upAxis = \"Z\"\n)\n"
            "def Mesh \"leaf\"\n{\n"
            "    rel material:binding = </Leaf>\n"
            "    point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]\n"
            "    int[] faceVertexCounts = [3]\n"
            "    int[] faceVertexIndices = [0,1,2]\n"
            "}\n"
            "def Material \"Leaf\"\n{\n"
            "    def Shader \"S\"\n    {\n"
            "        uniform token info:id = \"UsdPreviewSurface\"\n"
            "        color3f inputs:diffuseColor.connect = </Leaf/Alb.outputs:rgb>\n"
            "        float inputs:opacity.connect = </Leaf/Cut.outputs:r>\n"
            "    }\n"
            "    def Shader \"Alb\"\n    {\n"
            "        uniform token info:id = \"UsdUVTexture\"\n"
            "        asset inputs:file = @leaf_albedo.jpg@\n    }\n"
            "    def Shader \"Cut\"\n    {\n"
            "        uniform token info:id = \"UsdUVTexture\"\n"
            "        asset inputs:file = @leaf_opacity.jpg@\n    }\n"
            "}\n", r);
        check(ok, "a material with a separate opacity map imports");
        if (r.materials.size() == 1) {
            // Neither file is on disk here, so BOTH slots stay unbound -- what is being asserted is
            // that the importer looked for the opacity map at all, which the image list proves.
            bool sawOpacityFile = false;
            for (const fmt::ImportedImage& im : r.images)
                if (im.sourcePath.find("leaf_opacity") != std::string::npos) sawOpacityFile = true;
            check(sawOpacityFile,
                  "the opacity texture was FOLLOWED, not skipped with the other unread inputs");
            check(r.materials[0].alphaMode == "MASK",
                  "and a material with an opacity map is a cutout, not opaque");
        }
    }

    // ---- an undecodable container, answered by the sibling beside it on disk -------------------
    //
    // Intel's Jungle Ruins binds every base colour as a .tif, and stb_image -- the one decoder in
    // the tree -- reads no TIFF. The cook's standing advice was "convert it to PNG or TGA and
    // re-import", which on its own could never work: re-importing reads the .tif path straight back
    // out of the .usda and never looks at the converted file.
    //
    // ON DISK, not in memory, because the whole point is a lookup relative to a resolved path.
    {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "aver_import_sibling_test";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);

        putFile(dir / "bark.tif", kTiff, sizeof kTiff);
        putFile(dir / "bark.png", kPng, sizeof kPng);
        putFile(dir / "lonely.tif", kTiff, sizeof kTiff);  // no sibling: must stay refused

        const std::string doc =
            "#usda 1.0\n"
            "(\n    defaultPrim = \"root\"\n    metersPerUnit = 0.01\n    upAxis = \"Z\"\n)\n"
            "def Xform \"root\"\n{\n"
            "    def Mesh \"m\" (apiSchemas = [\"MaterialBindingAPI\"])\n    {\n"
            "        int[] faceVertexCounts = [3]\n"
            "        int[] faceVertexIndices = [0, 1, 2]\n"
            "        point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]\n"
            "        rel material:binding = </root/M>\n    }\n"
            "    def Material \"M\"\n    {\n"
            "        token outputs:surface.connect = </root/M/S.outputs:surface>\n"
            "        def Shader \"S\"\n        {\n"
            "            uniform token info:id = \"UsdPreviewSurface\"\n"
            "            color3f inputs:diffuseColor.connect = </root/M/Alb.outputs:rgb>\n"
            "            normal3f inputs:normal.connect = </root/M/Nrm.outputs:rgb>\n"
            "        }\n"
            "        def Shader \"Alb\"\n        {\n"
            "            uniform token info:id = \"UsdUVTexture\"\n"
            "            asset inputs:file = @bark.tif@\n        }\n"
            "        def Shader \"Nrm\"\n        {\n"
            "            uniform token info:id = \"UsdUVTexture\"\n"
            "            asset inputs:file = @lonely.tif@\n        }\n"
            "    }\n}\n";

        fmt::UsdImportResult r;
        const bool ok = usda(doc, r, {}, dir.string());
        check(ok, "a USDA naming a TIFF base colour imports");
        if (ok && r.materials.size() == 1) {
            const i32 bi = r.materials[0].baseColorTex.imageIndex;
            const i32 ni = r.materials[0].normalTex.imageIndex;
            check(bi >= 0 && usize(bi) < r.images.size(), "its base colour resolved to an image");
            if (bi >= 0 && usize(bi) < r.images.size()) {
                const fmt::ImportedImage& im = r.images[usize(bi)];
                // THE BYTES, not the name: the .usda still says .tif and always will, so the only
                // honest proof the substitution happened is what was actually loaded.
                check(im.bytes.size() == sizeof kPng &&
                          std::memcmp(im.bytes.data(), kPng, sizeof kPng) == 0,
                      "and the PNG sibling's bytes were loaded in place of the TIFF's");
                check(im.ext == ".png", "with the extension corrected to match the bytes");
            }
            if (ni >= 0 && usize(ni) < r.images.size()) {
                // NO SIBLING MEANS NO SUBSTITUTION. Falling back to some other image in the folder
                // would put the wrong picture on the slot, which is worse than leaving it unbound.
                const fmt::ImportedImage& im = r.images[usize(ni)];
                check(im.bytes.size() == sizeof kTiff,
                      "a TIFF with no sibling is left exactly as it was, for the cook to refuse");
            }
            bool told = false;
            for (const std::string& u : r.unsupported)
                if (u.find("sibling") != std::string::npos) told = true;
            check(told, "and the substitution is REPORTED rather than made silently");
        }
        fs::remove_all(dir, ec);
    }

    // ---- opacity that names the base colour's OWN alpha channel --------------------------------
    //
    // Blender's USD exporter connects inputs:opacity and inputs:diffuseColor to the SAME
    // UsdUVTexture, differing only by `.outputs:a` versus `.outputs:rgb`. With the output name
    // dropped the two slots became indistinguishable, and the downstream fold copied that image's
    // RED channel into alpha -- which on a green leaf atlas is very nearly a full erase.
    //
    // THE FILE HAS TO BE ON DISK or this case proves nothing: an unresolved slot is empty already,
    // so "the opacity slot is cleared" would pass without the clearing code ever running.
    {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "aver_import_ownalpha_test";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        putFile(dir / "leaf.png", kPng, sizeof kPng);

        const std::string doc =
            "#usda 1.0\n"
            "(\n    defaultPrim = \"root\"\n    metersPerUnit = 0.01\n    upAxis = \"Z\"\n)\n"
            "def Xform \"root\"\n{\n"
            "    def Mesh \"m\" (apiSchemas = [\"MaterialBindingAPI\"])\n    {\n"
            "        int[] faceVertexCounts = [3]\n"
            "        int[] faceVertexIndices = [0, 1, 2]\n"
            "        point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]\n"
            "        rel material:binding = </root/M>\n    }\n"
            "    def Material \"M\"\n    {\n"
            "        token outputs:surface.connect = </root/M/S.outputs:surface>\n"
            "        def Shader \"S\"\n        {\n"
            "            uniform token info:id = \"UsdPreviewSurface\"\n"
            "            color3f inputs:diffuseColor.connect = </root/M/Alb.outputs:rgb>\n"
            "            float inputs:opacity.connect = </root/M/Alb.outputs:a>\n"
            "            float inputs:opacityThreshold = 0.5\n"
            "        }\n"
            "        def Shader \"Alb\"\n        {\n"
            "            uniform token info:id = \"UsdUVTexture\"\n"
            "            asset inputs:file = @leaf.png@\n        }\n"
            "    }\n}\n";
        fmt::UsdImportResult r;
        const bool ok = usda(doc, r, {}, dir.string());
        check(ok, "a material whose opacity names its own base colour's alpha imports");
        if (ok && r.materials.size() == 1) {
            const fmt::ImportedMaterial& m = r.materials[0];
            // GUARD AGAINST A VACUOUS PASS: the base colour must actually have resolved, or the
            // "cleared" assertion below is just describing a slot that was never filled.
            check(!m.baseColorTex.empty(), "its base colour resolved to a real image on disk");
            check(m.opacityTex.empty(),
                  "the opacity slot is CLEARED -- the base colour's own alpha already is the cutout");
            check(m.alphaMode == "MASK",
                  "but the material is still a cutout, because the connection said so");
            checkNear(m.alphaCutoff, 0.5f, 1e-6f, "and opacityThreshold survives as the cutoff");
        }
        fs::remove_all(dir, ec);
    }

    // A separate mask is still a separate mask. This is the case the old hardcoded-red fold was
    // written for, and it must keep working -- with the channel now recorded rather than assumed.
    //
    // BOTH FILES MUST EXIST. An unresolved slot lands at imageIndex -1, and two unresolved slots
    // compare equal at -1 -- so a version of this case with no files on disk would assert that two
    // absent images are "different" and pass or fail for reasons unrelated to what it is testing.
    {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "aver_import_mask_test";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        // Same bytes, two names: the importer dedupes images by resolved PATH, so these are two
        // distinct entries -- which is exactly the shape being asserted.
        putFile(dir / "leaf_albedo.png", kPng, sizeof kPng);
        putFile(dir / "leaf_mask.png", kPng, sizeof kPng);

        const std::string doc =
            "#usda 1.0\n"
            "(\n    defaultPrim = \"root\"\n    metersPerUnit = 0.01\n    upAxis = \"Z\"\n)\n"
            "def Xform \"root\"\n{\n"
            "    def Mesh \"m\" (apiSchemas = [\"MaterialBindingAPI\"])\n    {\n"
            "        int[] faceVertexCounts = [3]\n"
            "        int[] faceVertexIndices = [0, 1, 2]\n"
            "        point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]\n"
            "        rel material:binding = </root/M>\n    }\n"
            "    def Material \"M\"\n    {\n"
            "        token outputs:surface.connect = </root/M/S.outputs:surface>\n"
            "        def Shader \"S\"\n        {\n"
            "            uniform token info:id = \"UsdPreviewSurface\"\n"
            "            color3f inputs:diffuseColor.connect = </root/M/Alb.outputs:rgb>\n"
            "            float inputs:opacity.connect = </root/M/Cut.outputs:r>\n"
            "        }\n"
            "        def Shader \"Alb\"\n        {\n"
            "            uniform token info:id = \"UsdUVTexture\"\n"
            "            asset inputs:file = @leaf_albedo.png@\n        }\n"
            "        def Shader \"Cut\"\n        {\n"
            "            uniform token info:id = \"UsdUVTexture\"\n"
            "            asset inputs:file = @leaf_mask.png@\n        }\n"
            "    }\n}\n";
        fmt::UsdImportResult r;
        const bool ok = usda(doc, r, {}, dir.string());
        check(ok, "a material with a genuinely separate opacity map imports");
        if (ok && r.materials.size() == 1) {
            const fmt::ImportedMaterial& m = r.materials[0];
            check(!m.opacityTex.empty(), "its opacity slot is KEPT, because the mask is another image");
            check(m.opacityTex.imageIndex != m.baseColorTex.imageIndex,
                  "and it is a different image from the base colour");
            check(m.opacityTex.channel == 'r',
                  "with the channel the connection named recorded, not assumed");
        }
        fs::remove_all(dir, ec);
    }

    if (g_failures == 0) AVER_INFO("=== all import tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
