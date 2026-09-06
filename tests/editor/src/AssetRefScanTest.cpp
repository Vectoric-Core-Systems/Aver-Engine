// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
//
// Matching one asset path inside another file's text, at a path-segment boundary.
//
// WHAT THIS GUARDS, and why it is not a formality. The Content Browser's "who references this asset"
// scan used a bare `hay.find(want)`. Every reference in a project is a content-relative path, so the
// needle for `Meshes/Cube.ocmesh` is that whole string -- and it is a SUBSTRING of
// `PropMeshes/Cube.ocmesh` and of `Sub/Meshes/Cube.ocmesh`, which are different files entirely.
//
// As a warning that is noise. It stops being noise the moment anything REWRITES what the scan finds:
// a rename that string-replaced every hit would repoint an asset the author never touched, silently.
// Anchoring is the precondition for a rewriter, so it is tested before one exists.
//
// The false-positive cases below are therefore the point of this file. A matcher that returned true
// for everything would pass every "does find it" assertion here and fail only these.
#include "AssetRefScan.hpp"

#include "aver/core/Log.hpp"
#include "aver/core/ErrorCodes.hpp"

#include <string>

using namespace aver;
using namespace aver::editor;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) AVER_INFO("   PASS  {}", what);
    else      { AVER_ERROR("   FAIL  {}", what); ++g_failures; }
}

// The scan compares normalised text, so every fixture goes through the same normaliser the caller
// uses -- testing the pair together is the point, since a mismatch between them is invisible.
static bool refs(const std::string& haystack, const std::string& needle) {
    return referencesAsset(normaliseForRefScan(haystack), normaliseForRefScan(needle));
}

int main() {
    AVER_INFO("======== AssetRefScanTest ========");

    const std::string mesh = "Meshes/Cube.ocmesh";

    // ---- it still finds real references, in the shapes the formats actually use ----------------
    check(refs("PLACE Meshes/Cube.ocmesh 0 0 0", mesh), "a bare PLACE record matches");
    check(refs("MESH Meshes/Cube.ocmesh\n", mesh), "a record followed by a newline matches");
    check(refs("mesh=Meshes/Cube.ocmesh", mesh), "a key=value attribute matches");
    check(refs("TEX base \"Meshes/Cube.ocmesh\"", mesh), "a quoted path matches");
    check(refs("Meshes/Cube.ocmesh", mesh), "the whole file being exactly the path matches");
    check(refs("a\nMeshes/Cube.ocmesh\nb", mesh), "surrounded by line breaks matches");

    // BACKSLASHES AND CASE, both of which real files mix: a .ocgraph written by hand says
    // Meshes\Cube.ocmesh where the browser reports Meshes/Cube.ocmesh, and a miss there reads as
    // "nothing references it" -- the exact false negative the normaliser exists to prevent.
    check(refs("MESH Meshes\\Cube.ocmesh", mesh), "a backslash-separated reference still matches");
    check(refs("MESH MESHES/CUBE.OCMESH", mesh), "case differences still match");
    check(refs("MESH Meshes\\Cube.ocmesh", "meshes/cube.ocmesh"), "and the needle may differ in both too");

    // ---- THE FALSE POSITIVES. This is what the anchoring is for. -------------------------------
    check(!refs("PLACE PropMeshes/Cube.ocmesh 0 0 0", mesh),
          "a LONGER FOLDER NAME ending in the needle's first segment does NOT match "
          "(PropMeshes/Cube.ocmesh is a different asset)");
    check(!refs("PLACE Sub/Meshes/Cube.ocmesh 0 0 0", mesh),
          "the SAME path nested one folder deeper does NOT match -- it is a different file");
    check(!refs("MESH Meshes/Cube.ocmesh2", mesh),
          "a longer FILENAME beginning with the needle does not match");
    check(!refs("MESH Meshes/Cube.ocmeshx", mesh),
          "nor a longer extension");
    check(!refs("MESH XMeshes/Cube.ocmesh", mesh),
          "nor a folder whose name merely ends with the needle's folder");

    // A path that only SHARES A LEAF is the commonest real-world near-miss: two Cube.ocmesh files in
    // different folders is an ordinary way to organise a project.
    check(!refs("MESH Props/Cube.ocmesh", mesh), "a same-named file in another folder does not match");

    // ---- boundary characters that DO delimit a path -------------------------------------------
    check(refs("{Meshes/Cube.ocmesh}", mesh), "braces delimit");
    check(refs("=Meshes/Cube.ocmesh;", mesh), "'=' and ';' delimit");
    check(refs("\tMeshes/Cube.ocmesh\t", mesh), "tabs delimit");

    // ---- offsets, which a rewriter needs ------------------------------------------------------
    //
    // OFFSETS INTO THE NORMALISED BUFFER MUST INDEX THE ORIGINAL ONE TOO. Normalising lower-cases and
    // swaps separators, both length-preserving -- if either step ever changed length, a rewriter
    // splicing at these offsets would corrupt the file. Checked here because the property lives in
    // the normaliser and nothing else would notice it breaking.
    {
        const std::string original = "MESH Meshes/Cube.ocmesh\nMESH Props/Cube.ocmesh\n";
        const std::string norm = normaliseForRefScan(original);
        check(norm.size() == original.size(), "normalising preserves length, so offsets stay valid");

        const auto hits = findAnchoredAssetRefs(norm, normaliseForRefScan(mesh));
        check(hits.size() == 1, "exactly one anchored hit in a file that also holds a near-miss");
        if (hits.size() == 1) {
            check(original.compare(hits[0], mesh.size(), mesh) == 0,
                  "and the offset indexes the ORIGINAL text, with its original case, exactly");
        }
    }
    {
        // Two real hits, so a rewriter would splice twice.
        const std::string norm = normaliseForRefScan("a Meshes/Cube.ocmesh b Meshes/Cube.ocmesh c");
        check(findAnchoredAssetRefs(norm, normaliseForRefScan(mesh)).size() == 2,
              "two real references report two offsets");
    }

    // ---- degenerate inputs --------------------------------------------------------------------
    check(!refs("anything at all", ""), "an empty needle matches nothing");
    check(!refs("", mesh), "an empty haystack matches nothing");
    check(!refs("Cube", mesh), "a haystack shorter than the needle matches nothing");
    check(!isAnchoredAssetRef("abc", 1, 5), "a length running past the end is not a match");
    check(!isAnchoredAssetRef("abc", 9, 1), "an offset past the end is not a match");
    check(!isAnchoredAssetRef("abc", 0, 0), "a zero-length match is not a match");

    AVER_INFO(g_failures ? "AssetRefScanTest: {} FAILURES" : "AssetRefScanTest: all checks passed ({})",
              g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
