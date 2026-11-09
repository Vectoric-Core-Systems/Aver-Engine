#include "AssetC.hpp"

#include "aver/formats/OcHair.hpp"   // `hair`: the .ochair it writes
#if AVER_HAVE_HAIR
#include "aver/hair/HairCurves.hpp"  // `hair`: glTF line strands into explicit strands (Aver.Hair)
#endif

namespace assetc {

// `hair <file.gltf|.glb> --out <groom.ochair> [--scalp <mesh.ocmesh>] [--scalp-asset <ref>] [--submesh N] [--joints a,b,c]
//        [--default-width-cm F] [--max-root-cm F]`: the LINES and LINE_STRIP primitives of a glTF become the strands of an `.ochair` (docs/HAIR.md,
// docs/plans/wave11/SOFTBODY_HAIR.md 2.13). The groom gets the default SIM and one default MATERIAL and LOOK per strand material the file used.
// The strands are written in the glTF's own space converted to the engine's (cm, +Z up); that must be the space of the scalp mesh, because Jolt's Hair
// attaches every root to the scalp. With --scalp the tool loads that mesh and measures the largest root-to-scalp distance, and fails above
// --max-root-cm (2 by default); SCALP in the groom names --scalp-asset, else Meshes/<mesh stem>.ocmesh.
#if AVER_HAVE_HAIR
// The squared distance from p to triangle abc (Ericson, Real-Time Collision Detection 5.1.5).
f32 distSqPointTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 ab = b - a, ac = c - a, ap = p - a;
    const f32 d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return distSquared(p, a);
    const Vec3 bp = p - b;
    const f32 d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return distSquared(p, b);
    const f32 vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const f32 v = d1 / (d1 - d3);
        return distSquared(p, a + ab * v);
    }
    const Vec3 cp = p - c;
    const f32 d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return distSquared(p, c);
    const f32 vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const f32 w = d2 / (d2 - d6);
        return distSquared(p, a + ac * w);
    }
    const f32 va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const f32 w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return distSquared(p, b + (c - b) * w);
    }
    const f32 denom = 1.0f / (va + vb + vc);
    const f32 v = vb * denom, w = vc * denom;
    return distSquared(p, a + ab * v + ac * w);
}

int runHair(int argc, char** argv) {
    std::string input, scalpPath, scalpAsset, outPath, joints;
    i32 submesh = -1;
    f32 defaultWidthCm = 0.0f, maxRootCm = 2.0f;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--scalp" && i + 1 < argc)               scalpPath = argv[++i];
        else if (a == "--scalp-asset" && i + 1 < argc)    scalpAsset = argv[++i];
        else if (a == "--submesh" && i + 1 < argc)        submesh = std::atoi(argv[++i]);
        else if (a == "--joints" && i + 1 < argc)         joints = argv[++i];
        else if (a == "--out" && i + 1 < argc)            outPath = argv[++i];
        else if (a == "--default-width-cm" && i + 1 < argc) defaultWidthCm = static_cast<f32>(std::atof(argv[++i]));
        else if (a == "--max-root-cm" && i + 1 < argc)    maxRootCm = static_cast<f32>(std::atof(argv[++i]));
        else if (!a.empty() && a[0] != '-' && input.empty()) input = a;
        else { AVER_ERROR("[hair] unknown argument '{}'", a); return exitCode(ExitCode::Usage); }
    }
    if (input.empty() || outPath.empty() || !(maxRootCm > 0.0f)) {
        AVER_ERROR("[hair] usage: AverAssetC hair <file.gltf|.glb> --out <groom.ochair> [--scalp <mesh.ocmesh>] [--scalp-asset <ref>] [--submesh N] "
                   "[--joints a,b,c] [--default-width-cm F] [--max-root-cm F]");
        return exitCode(ExitCode::Usage);
    }

    RunStats stats;
    std::string why;
    auto failWith = [&](const std::string& reason) {
        emitArtifact(input, outPath, "hair", false, false, reason, stats);
        emitSummary(input, stats, 1);
        return exitCode(ExitCode::Failed);
    };

    fmt::GltfImportResult res;
    if (!fmt::importGltf(input, res, {}, &why)) return failWith(why);
    hair::CurveImportOptions opt;
    opt.defaultWidthCm = defaultWidthCm;
    hair::CurveImportReport rep;
    fmt::OcHairData groom;
    if (!hair::curvesToStrands(res.hairCurves, groom.strands, opt, &rep, &why)) return failWith(why);
    if (rep.droppedShort > 0) AVER_WARN("[hair] {} strand(s) with fewer than 3 usable points were dropped", rep.droppedShort);
    if (rep.resampled > 0) AVER_WARN("[hair] {} strand(s) longer than {} points were resampled", rep.resampled, opt.maxPoints);
    if (rep.materialsClamped > 0) AVER_WARN("[hair] {} strand(s) named a material above {}; clamped", rep.materialsClamped, fmt::kOcHairMaxMaterials);

    // The scalp: the file names it; if it is on disk, the roots are checked against it.
    f32 worstRoot = -1.0f;
    if (!scalpPath.empty()) {
        fmt::OcMeshData mesh;
        if (!fmt::loadOcMesh(scalpPath, mesh, &why)) return failWith("cannot load the scalp mesh: " + why);
        const usize roots = groom.strands.strandCount();
        const usize tris = mesh.indices.size() / 3;
        if (tris == 0) return failWith("the scalp mesh has no triangles");
        // Brute force over every triangle, thinned when it would take long: the answer is a sanity check, not a measurement of record.
        const usize step = std::max<usize>(1, (roots * tris) / 40000000u + 1);
        f32 worstSq = 0.0f;
        for (usize s = 0; s < roots; s += step) {
            const usize pi = static_cast<usize>(groom.strands.start[s]) * 3;
            const Vec3 root(groom.strands.points[pi], groom.strands.points[pi + 1], groom.strands.points[pi + 2]);
            f32 best = 3.0e38f;
            for (usize t = 0; t < tris; ++t) {
                const u32 ia = mesh.indices[t * 3], ib = mesh.indices[t * 3 + 1], ic = mesh.indices[t * 3 + 2];
                if (ia >= mesh.vertexCount() || ib >= mesh.vertexCount() || ic >= mesh.vertexCount()) continue;
                const Vec3 pa(mesh.positions[ia * 3], mesh.positions[ia * 3 + 1], mesh.positions[ia * 3 + 2]);
                const Vec3 pb(mesh.positions[ib * 3], mesh.positions[ib * 3 + 1], mesh.positions[ib * 3 + 2]);
                const Vec3 pc(mesh.positions[ic * 3], mesh.positions[ic * 3 + 1], mesh.positions[ic * 3 + 2]);
                best = std::min(best, distSqPointTriangle(root, pa, pb, pc));
            }
            worstSq = std::max(worstSq, best);
        }
        worstRoot = std::sqrt(worstSq);
        if (step > 1) AVER_INFO("[hair] root-to-scalp measured on every {}th strand", step);
        if (worstRoot > maxRootCm)
            return failWith("the roots are up to " + std::to_string(worstRoot) + " cm from the scalp (limit " + std::to_string(maxRootCm) +
                            " cm): the curves and the scalp mesh are not in the same space");
        if (scalpAsset.empty()) {
            const std::string stem = std::filesystem::path(scalpPath).stem().string();
            scalpAsset = "Meshes/" + stem + ".ocmesh";
        }
    }
    groom.scalpMesh = scalpAsset;
    groom.scalpSubmesh = submesh;
    for (usize start = 0; start < joints.size();) {
        usize comma = joints.find(',', start);
        if (comma == std::string::npos) comma = joints.size();
        if (comma > start) groom.scalpJoints.push_back(joints.substr(start, comma - start));
        start = comma + 1;
    }

    // One default MATERIAL and LOOK per strand material the file used.
    std::vector<u8> used;
    for (u8 m : groom.strands.material)
        if (std::find(used.begin(), used.end(), m) == used.end()) used.push_back(m);
    std::sort(used.begin(), used.end());
    for (u8 m : used) {
        fmt::OcHairMaterial mat;
        mat.index = m;
        groom.materials.push_back(mat);
        fmt::OcHairLook look;
        look.material = m;
        groom.looks.push_back(look);
    }

    if (!fmt::saveOcHair(outPath, groom, &why)) return failWith(why);
    fmt::OcHairData back;
    std::string verr;
    const bool verified = fmt::loadOcHair(outPath, back, &verr) && back.strands.points == groom.strands.points && back.strands.start == groom.strands.start;
    std::string extra = "\"strands\":" + std::to_string(groom.strands.strandCount()) + ",\"points\":" + std::to_string(groom.strands.pointCount()) +
                        ",\"droppedShort\":" + std::to_string(rep.droppedShort) + ",\"resampled\":" + std::to_string(rep.resampled);
    if (worstRoot >= 0.0f) extra += ",\"rootToScalpCm\":" + std::to_string(worstRoot);
    emitArtifact(input, outPath, "hair", true, verified, verified ? std::string() : "the written groom did not reload: " + verr, stats, extra);
    const int code = stats.failed > 0 ? 1 : 0;
    emitSummary(input, stats, code);
    return exitCode(code ? ExitCode::Failed : ExitCode::Ok);
}
#else
int runHair(int, char**) {
    AVER_ERROR("[hair] this AverAssetC was built without Aver.Hair (AVER_MODULE_HAIR is off)");
    return exitCode(ExitCode::Usage);
}
#endif

// `hair-text <groom.txt> --out <groom.ochair>`: the HEAD records of an `.ochair` (OcHair.hpp grammar: SCALP, SCALPJOINTS, SIM, MATERIAL, LOOK, GENERATE,
// LOD, PROXY) written as a plain text file become the binary groom, so a procedural groom is authored in a text editor and diffs as text. A groom with a
// GENERATE record and no strands is regenerated from its scalp at load. Needs only Aver.Formats (no Aver.Hair), so every AverAssetC has it.
int runHairText(int argc, char** argv) {
    std::string input, outPath;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc)                 outPath = argv[++i];
        else if (!a.empty() && a[0] != '-' && input.empty()) input = a;
        else { AVER_ERROR("[hair-text] unknown argument '{}'", a); return exitCode(ExitCode::Usage); }
    }
    if (input.empty() || outPath.empty()) {
        AVER_ERROR("[hair-text] usage: AverAssetC hair-text <groom.txt> --out <groom.ochair>");
        return exitCode(ExitCode::Usage);
    }
    RunStats stats;
    std::string why;
    auto failWith = [&](const std::string& reason) {
        emitArtifact(input, outPath, "hair", false, false, reason, stats);
        emitSummary(input, stats, 1);
        return exitCode(ExitCode::Failed);
    };
    std::ifstream in(aver::nativePath(input), std::ios::binary);
    if (!in) return failWith("cannot read the text file");
    std::ostringstream text;
    text << in.rdbuf();
    fmt::OcHairData groom;
    if (!fmt::parseOcHairHead(text.str(), groom, &why)) return failWith(why);
    if (!fmt::saveOcHair(outPath, groom, &why)) return failWith(why);
    fmt::OcHairData back;
    std::string verr;
    const bool verified = fmt::loadOcHair(outPath, back, &verr) && fmt::ocHairHeadText(back) == fmt::ocHairHeadText(groom);
    const std::string extra = "\"strands\":" + std::to_string(groom.strands.strandCount()) + ",\"generate\":" + (groom.generate.present ? "true" : "false");
    emitArtifact(input, outPath, "hair", true, verified, verified ? std::string() : "the written groom did not reload: " + verr, stats, extra);
    const int code = stats.failed > 0 ? 1 : 0;
    emitSummary(input, stats, code);
    return exitCode(code ? ExitCode::Failed : ExitCode::Ok);
}

} // namespace assetc
