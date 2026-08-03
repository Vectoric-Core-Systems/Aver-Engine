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

static bool usda(const std::string& text, fmt::UsdImportResult& out,
                 const fmt::UsdImportOptions& opt = {}) {
    std::string why;
    const bool ok = fmt::importUsdFromMemory(reinterpret_cast<const u8*>(text.data()), text.size(),
                                             out, opt, &why);
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
                                                 r, {}, &why);
        check(!ok, "a USDC crate is REFUSED");
        check(r.encoding == fmt::UsdEncoding::Usdc, "and identified as USDC");
        check(why.find("usdcat") != std::string::npos, "with the conversion command in the message");
    }
    {
        fmt::UsdImportResult r;
        std::string why;
        const char zip[] = "PK\x03\x04rest";
        const bool ok = fmt::importUsdFromMemory(reinterpret_cast<const u8*>(zip), sizeof(zip) - 1,
                                                 r, {}, &why);
        check(!ok, "a USDZ archive is REFUSED");
        check(r.encoding == fmt::UsdEncoding::Usdz, "and identified as USDZ");
    }
    {
        fmt::UsdImportResult r;
        std::string why;
        const char junk[] = "this is not a usd file at all";
        const bool ok = fmt::importUsdFromMemory(reinterpret_cast<const u8*>(junk), sizeof(junk) - 1,
                                                 r, {}, &why);
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
                                                 r, {}, &why);
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

    if (g_failures == 0) AVER_INFO("=== all import tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
