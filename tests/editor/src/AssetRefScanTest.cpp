// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
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

    // ---- the rewriter -------------------------------------------------------------------------
    //
    // This is the half that can destroy someone's project, so it is a pure string function and the
    // cases below are the ways it could do that.
    {
        const std::string oldRel = "Meshes/Cube.ocmesh";
        const std::string newRel = "Meshes/Box.ocmesh";

        // The ordinary case, and the near-miss beside it that must survive untouched.
        {
            const std::string in = "MESH Meshes/Cube.ocmesh\nMESH PropMeshes/Cube.ocmesh\n";
            const auto r = rewriteAssetRefs(in, oldRel, newRel);
            check(r.count == 1, "one reference rewritten");
            check(r.text == "MESH Meshes/Box.ocmesh\nMESH PropMeshes/Cube.ocmesh\n",
                  "AND THE UNRELATED ASSET IS UNTOUCHED -- the whole reason for anchoring");
        }

        // SEVERAL OCCURRENCES, which is where a front-to-back splice corrupts the tail: each
        // replacement shifts every later offset. A different-length new name makes that visible.
        {
            const std::string in = "a Meshes/Cube.ocmesh b Meshes/Cube.ocmesh c Meshes/Cube.ocmesh d";
            const auto r = rewriteAssetRefs(in, oldRel, "Meshes/AMuchLongerName.ocmesh");
            check(r.count == 3, "three references rewritten");
            check(r.text == "a Meshes/AMuchLongerName.ocmesh b Meshes/AMuchLongerName.ocmesh "
                            "c Meshes/AMuchLongerName.ocmesh d",
                  "all three land correctly even though the new name is longer");
        }
        {
            // And shorter, the other direction of the same bug.
            const std::string in = "a Meshes/Cube.ocmesh b Meshes/Cube.ocmesh";
            const auto r = rewriteAssetRefs(in, oldRel, "M/C.ocmesh");
            check(r.text == "a M/C.ocmesh b M/C.ocmesh", "and a shorter new name too");
        }

        // SEPARATOR STYLE IS PRESERVED PER OCCURRENCE. A hand-written .ocgraph uses backslashes; a
        // tool-written .ocworld uses forward slashes; one file can hold both, and rewriting either
        // into the other style is an edit nobody asked for in a file somebody maintains by hand.
        {
            const std::string in = "one Meshes\\Cube.ocmesh two Meshes/Cube.ocmesh";
            const auto r = rewriteAssetRefs(in, oldRel, newRel);
            check(r.count == 2, "both separator styles are found");
            check(r.text == "one Meshes\\Box.ocmesh two Meshes/Box.ocmesh",
                  "and EACH keeps the style it was written with");
        }

        // Case in the FILE differs from case in the browser; the replacement is the new name as
        // given, because that is the name the asset now actually has on disk.
        {
            const std::string in = "MESH MESHES/CUBE.OCMESH";
            const auto r = rewriteAssetRefs(in, oldRel, newRel);
            check(r.count == 1, "a differently-cased reference is found");
            check(r.text == "MESH Meshes/Box.ocmesh", "and replaced with the real new name");
        }

        // Nothing to do must change nothing at all -- a caller writes the result back to disk, so a
        // spurious edit here is a modified file and a dirtied timestamp for no reason.
        {
            const std::string in = "MESH Props/Other.ocmesh\n# a comment\n";
            const auto r = rewriteAssetRefs(in, oldRel, newRel);
            check(r.count == 0 && r.text == in, "a file with no references is returned byte-identical");
        }
        {
            const auto r = rewriteAssetRefs("MESH Meshes/Cube.ocmesh", oldRel, "");
            check(r.count == 0 && r.text == "MESH Meshes/Cube.ocmesh",
                  "an empty new name rewrites nothing rather than deleting the reference");
        }
        {
            const auto r = rewriteAssetRefs("MESH Meshes/Cube.ocmesh", "", newRel);
            check(r.count == 0, "an empty old name rewrites nothing");
        }
    }

    AVER_INFO(g_failures ? "AssetRefScanTest: {} FAILURES" : "AssetRefScanTest: all checks passed ({})",
              g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
