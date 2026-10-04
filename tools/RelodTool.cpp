// RelodTool -- reports the LOD ladder Trifactor WOULD build for an .ocmesh today, against the one
// the file already carries, and (opt-in, see --write below) writes re-cooked copies somewhere else.
//
// WHY THIS EXISTS. A cooked .ocmesh carries LOD 0's geometry AND the coarser ladder Trifactor built
// from it. Change the simplifier and every ladder already on disk is stale -- and the only way to
// see what the new one would look like was to re-import the original source asset, which for a
// project that ships cooked meshes (Electric Dreams has 69 .ocmesh files and not one .gltf) does not
// exist any more. The ladder is derivable from LOD 0 alone, so needing the source to inspect it was
// never a real requirement, just a missing tool.
//
// REPORTS ONLY BY DEFAULT, and every invocation without --write is UNCHANGED from before this flag
// existed: same per-mesh line, same closing "nothing was written" line, same exit code. Rebuilding a
// ladder in place is a lossy, irreversible rewrite of somebody's art asset, so this tool never
// touches an input file, with or without the flag -- see --write's own comment below for the write
// path this stage adds, and why it writes to a caller-chosen DIRECTORY rather than in place.
//
// The conversion from a built LodDag back into a mesh's own meshlets/coarserLods, which used to live
// only in ConvertTool's anonymous namespace (the reason a write path did not exist before this
// stage), now lives in aver::trifactor::packLodDag (ClusterBuilder.hpp) -- see that function's own
// doc comment for why it belongs in Trifactor rather than Formats or here. This tool calls it, the
// same way ConvertTool does, rather than keeping a third copy.
//
//     RelodTool.exe <file-or-directory> [--write <output-directory>]
//
// --write <dir>: pack and save a re-cooked copy of every mesh RelodTool would otherwise only report
// on, into `dir`, one new file per input, under the SAME filename -- never touching the input. Only
// meshlets/coarserLods/builderVersion (Aver.Trifactor's own contribution to a cooked mesh) differ
// from the source file; every other stream (positions, normals, UVs, submeshes, material slots,
// skin, bounds, flags) is carried across byte-for-byte, via a full in-memory copy of the loaded mesh
// -- see relod()'s own comment for why that copy, not the stripped `src` this tool already built for
// its own reporting, is what gets packed and saved. Refuses, loudly and before touching anything, if
// `dir` resolves to the same place as the input (a single file's own directory, or the input
// directory itself) -- see main()'s own comment for how that comparison is made robust to a
// not-yet-existing output directory.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/trifactor/ClusterBuilder.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

using namespace aver;

namespace {

// writeAttempted/writeFailed/written and the three staleness counters are all zero for the ENTIRE
// run whenever --write was never passed (relod() only ever touches them inside its `if (writeDir)`
// block below) -- which is what keeps the closing summary line's arithmetic below identical to what
// it always was in that case, rather than needing a second code path to reproduce the old numbers.
struct Totals {
    u32 files = 0, failed = 0, improved = 0, invalid = 0;
    u64 oldCoarsest = 0, newCoarsest = 0;
    u32 written = 0, writeFailed = 0;
    u32 staleCount = 0, currentCount = 0, unknownCount = 0;
};

// Triangles in one level of the DAG, summed over its clusters. Cluster::triangles holds LOCAL
// indices, three per triangle, so the triangle count is its size divided by three.
u64 levelTris(const trifactor::LodDag& dag, u32 level) {
    u64 n = 0;
    for (const u32 cid : dag.levels[level]) n += dag.clusters[cid].triangles.size() / 3;
    return n;
}

// Classifies a mesh's ON-DISK builderVersion (whatever fmt::loadOcMesh just handed back, BEFORE this
// run re-cooks anything) against the version this build of Trifactor would stamp a fresh cook with.
// "unknown" and "stale" are both, in the end, "needs re-cooking" -- but they are named separately
// because they mean different things to a human deciding whether to trust a ladder: "unknown" is a
// file from before this field existed (Reserved was always 0 -- see OcMeshData::builderVersion's own
// comment on why 0 can never mean "current"), "stale" is a file that WAS cooked by a version-aware
// builder, just an older one than this binary.
const char* stalenessLabel(u32 stored) {
    if (stored == trifactor::kBuilderVersion) return "current";
    if (stored == 0) return "unknown";
    return "stale";
}

void relod(const std::filesystem::path& path, const std::filesystem::path* writeDir, bool force, Totals& t) {
    fmt::OcMeshData md;
    std::string why;
    if (!fmt::loadOcMesh(path.string(), md, &why)) {
        AVER_WARN("[Relod] {}: {}", path.filename().string(), why);
        ++t.failed;
        return;
    }
    ++t.files;

    const u32 lod0Tris = static_cast<u32>(md.indices.size() / 3);
    const u32 oldLevels = md.lodCount();
    const u64 oldCoarsest = md.coarserLods.empty()
        ? lod0Tris
        : md.coarserLods.back().indices.size() / 3;

    // FROM LOD 0 ONLY. The coarserLods already in the file are the stale output being compared
    // against; feeding them back in would compound the old simplifier's decisions into the new
    // ladder instead of redoing them. buildClusters/buildLodHierarchy read only positions+indices.
    fmt::OcMeshData src;
    src.positions = md.positions;
    src.normals   = md.normals;
    src.uvs       = md.uvs;
    src.indices   = md.indices;
    src.boundsMin = md.boundsMin;
    src.boundsMax = md.boundsMax;
    src.flags     = md.flags;

    trifactor::LodDag dag;
    if (!trifactor::buildClusters(src, dag, &why)) {
        AVER_WARN("[Relod] {}: buildClusters: {}", path.filename().string(), why);
        ++t.failed;
        return;
    }
    if (!trifactor::buildLodHierarchy(src, dag, &why))
        AVER_WARN("[Relod] {}: buildLodHierarchy: {}", path.filename().string(), why);

    // AND CHECK THE DAG IT JUST BUILT, which this tool did not do and should have. It reported
    // triangle counts only, so every corpus run in this project's history measured how much a ladder
    // REDUCES without ever asking whether the ladder is VALID -- and the two are independent. The
    // shell-routing change in particular makes one specific invariant easier to break: a group whose
    // simplify collapses entirely to zero triangles produces no parent clusters, leaving its members
    // permanently parentless, and nothing about a triangle count would show it. validateLodDag tests
    // exactly that ("every non-root cluster has at least one parent") alongside LOD-0 coverage, the
    // 64/124 limits, cone validity and error monotonicity. It is cheap next to the build that
    // preceded it, so it runs on every mesh rather than behind a flag.
    const trifactor::ValidationReport report = trifactor::validateLodDag(src, dag);

    // STAGE 4: the streaming topology's own invariants (fallbackAncestorId validity, group-sphere
    // containment, ownerGroupId round-tripping) -- a SEPARATE call, not folded into validateLodDag
    // itself, for the same reason validateClusterHierarchy is a separate function in the first place
    // (see its own doc comment): a topology regression should name itself as one, not read as a
    // generic "DAG invalid" that sends someone hunting through the wrong file. Folded into the SAME
    // `invalid` counter below, though -- both are "this mesh's DAG cannot be trusted, refuse to write
    // it", and this tool's summary line has always reported that as one number.
    std::string hierarchyWhy;
    const bool hierarchyOk = trifactor::validateClusterHierarchy(dag, &hierarchyWhy);

    if (!report.ok || !hierarchyOk) {
        ++t.invalid;
        AVER_ERROR("[Relod] {}: DAG INVALID -- {} issue(s)", path.filename().string(),
                   report.issues.size() + (hierarchyOk ? 0 : 1));
        // Capped, because one broken invariant on a 49,000-shell mesh can report thousands of times
        // and the first few name the cause just as well as all of them do.
        u32 shown = 0;
        for (const trifactor::ValidationIssue& issue : report.issues) {
            AVER_ERROR("[Relod]     {}: {}", issue.where, issue.detail);
            if (++shown >= 5) {
                if (report.issues.size() > shown)
                    AVER_ERROR("[Relod]     ... and {} more", report.issues.size() - shown);
                break;
            }
        }
        if (!hierarchyOk) AVER_ERROR("[Relod]     cluster-hierarchy: {}", hierarchyWhy);
    }

    const u32 newLevels = dag.levelCount();
    const u64 newCoarsest = newLevels ? levelTris(dag, newLevels - 1) : lod0Tris;

    t.oldCoarsest += oldCoarsest;
    t.newCoarsest += newCoarsest;
    if (newCoarsest < oldCoarsest) ++t.improved;

    AVER_INFO("[Relod] {:<34} LOD0 {:>7} | was {:>7} ({:>2} lv, {:>5.1f}x) -> now {:>7} ({:>2} lv, {:>5.1f}x)",
              path.filename().string(), lod0Tris,
              oldCoarsest, oldLevels,
              oldCoarsest ? static_cast<f64>(lod0Tris) / static_cast<f64>(oldCoarsest) : 0.0,
              newCoarsest, newLevels,
              newCoarsest ? static_cast<f64>(lod0Tris) / static_cast<f64>(newCoarsest) : 0.0);

    // EVERYTHING BELOW THIS LINE ONLY RUNS WHEN --write WAS PASSED (writeDir != nullptr) -- the per-
    // mesh line just printed above is the ENTIRE default-mode output for this mesh, unchanged from
    // before this stage, which is what keeps a plain `RelodTool <dir>` run byte-for-byte identical to
    // what it always produced (see this file's own header comment).
    if (!writeDir) return;

    const char* label = stalenessLabel(md.builderVersion);
    if (md.builderVersion == trifactor::kBuilderVersion) ++t.currentCount;
    else if (md.builderVersion == 0)                     ++t.unknownCount;
    else                                                  ++t.staleCount;

    // A DAG this run already flagged INVALID (report.ok == false or hierarchyOk == false, just
    // reported above) must not be written: packLodDag would very likely refuse it too
    // (validateClusterErrorBounds/validateClusterHierarchy re-check much of the same ground), but
    // refusing HERE, before even calling it, keeps the reason in the log right next to the
    // DAG-INVALID block that explains it, rather than behind a second, differently-worded failure
    // from deeper in the pack path.
    if (!report.ok || !hierarchyOk) {
        AVER_ERROR("[Relod] {}: refusing to write -- its DAG failed validateLodDag/validateClusterHierarchy above",
                   path.filename().string());
        ++t.writeFailed;
        return;
    }

    // A FULL COPY OF THE ORIGINAL FILE, not `src` (the stripped positions/normals/uvs/indices/bounds/
    // flags copy built above for buildClusters/buildLodHierarchy's own consumption). `src` is missing
    // submeshes, materialSlots, and the skin streams entirely -- packing a fresh ladder into it and
    // saving THAT would silently drop every submesh boundary and material assignment the original file
    // had, which is a far more destructive kind of data loss than a stale LOD ladder ever was. `out`
    // starts as an exact copy of `md` (everything this tool already loaded from the real file) so that
    // packLodDag -- which only ever touches meshlets/coarserLods/builderVersion, see its own doc
    // comment -- is the ONLY thing that can make `out` differ from `md`.
    fmt::OcMeshData out = md;
    std::string packWhy;
    if (!trifactor::packLodDag(dag, out, &packWhy)) {
        AVER_ERROR("[Relod] {}: packLodDag: {}", path.filename().string(), packWhy);
        ++t.writeFailed;
        return;
    }

    // SAME FILENAME, DIFFERENT DIRECTORY -- never the input path itself, and main() has already
    // refused to run at all if `*writeDir` resolves to the input's own directory (see main()'s own
    // comment), so this concatenation cannot land back on `path` no matter what filename it carries.
    const std::filesystem::path outPath = *writeDir / path.filename();

    // AND REFUSE TO CLOBBER SOMETHING ALREADY THERE. The same-directory guard in main() protects the
    // one directory that must never be damaged; it does nothing for any OTHER directory a mistyped
    // --write might land on. saveOcMesh opens with std::ios::trunc, so without this an output folder
    // that happens to hold same-named meshes -- a sibling project, a previous run someone still
    // wanted, a typo one character off -- is overwritten silently, with no line in the log naming
    // what was destroyed. This tool's header promises it does not write; the moment it does, the
    // promise has to become "and never over anything", or the guarantee is only as good as the
    // typing. --force is the deliberate override, and it says what it replaced.
    std::error_code exEc;
    if (std::filesystem::exists(outPath, exEc) && !force) {
        AVER_ERROR("[Relod] {}: {} already exists; refusing to overwrite it. Pass --force to replace.",
                   path.filename().string(), outPath.string());
        ++t.writeFailed;
        return;
    }
    if (force && std::filesystem::exists(outPath, exEc))
        AVER_WARN("[Relod] {}: --force is replacing the existing {}", path.filename().string(), outPath.string());

    std::string saveWhy;
    if (!fmt::saveOcMesh(outPath.string(), out, &saveWhy)) {
        AVER_ERROR("[Relod] {}: write to {}: {}", path.filename().string(), outPath.string(), saveWhy);
        ++t.writeFailed;
        return;
    }
    ++t.written;
    AVER_INFO("[Relod]   -> wrote {} | builder was {} (v{}), now v{}",
              outPath.string(), label, md.builderVersion, out.builderVersion);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        AVER_ERROR("usage: RelodTool <file-or-directory> [--write <output-directory>]");
        return exitCode(ExitCode::Usage);
    }
    const std::filesystem::path root = argv[1];

    // --write <dir>, parsed out of the remaining args the same way ConvertTool parses --lod: found
    // anywhere after the positional argument, consuming the token that follows it. `doWrite` is what
    // every byte-for-byte-unchanged claim in this file's header comment rests on: it is false unless
    // this loop actually finds "--write" followed by another argument, and every new code path below
    // (and inside relod() itself) is conditioned on it.
    std::filesystem::path writeDirArg;
    bool doWrite = false;
    bool force = false;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--write") {
            if (i + 1 >= argc) {
                AVER_ERROR("--write requires an output directory argument");
                return exitCode(ExitCode::Usage);
            }
            writeDirArg = argv[++i];
            doWrite = true;
        }
        // Only meaningful with --write, and deliberately NOT implied by it: replacing files someone
        // already has is a separate decision from writing files at all.
        else if (std::strcmp(argv[i], "--force") == 0) force = true;
    }

    std::error_code ec;
    Totals t;
    std::filesystem::path writeDir;

    if (doWrite) {
        // THE ONE CHECK THIS WHOLE STAGE EXISTS TO GET RIGHT: refuse, before a single mesh is even
        // loaded, if `writeDirArg` resolves to the same place as the input. std::filesystem::path
        // equality is a lexical string comparison -- "C:/Meshes" and "C:/Meshes/" already compare
        // unequal despite naming the same directory, and Windows paths compare unequal on CASE alone
        // ("C:/Meshes" vs "c:/meshes") despite NTFS being case-insensitive for lookup -- so a
        // string-level comparison here would be exactly the kind of check that LOOKS safe and passes
        // review while a differently-spelled alias of the input sails straight through it.
        // std::filesystem::equivalent, by contrast, asks the OS whether two paths name the same
        // filesystem entry (same volume + file id), which is immune to every one of those spellings
        // and to symlinks/junctions besides. It requires both paths to EXIST, which is why the output
        // directory is created (an idempotent, harmless step even if it turns out to BE the input
        // directory -- creating a directory that already exists changes nothing) before this check
        // runs, rather than after.
        std::filesystem::create_directories(writeDirArg, ec);

        const std::filesystem::path inputAnchor = std::filesystem::is_directory(root, ec)
            ? root
            : (root.has_parent_path() ? root.parent_path() : std::filesystem::path("."));

        std::error_code eqEc;
        const bool sameAsInput = std::filesystem::equivalent(writeDirArg, inputAnchor, eqEc);
        // FAILS CLOSED. `eqEc` set means the filesystem could not tell us whether these are the same
        // directory -- and "could not prove they differ" must refuse, not proceed. The original
        // reading (!eqEc && sameAsInput) treated an error as "different", which is the one
        // interpretation that can end with the tool writing over the corpus it was pointed at.
        if (eqEc || sameAsInput) {
            AVER_ERROR("[Relod] --write {} refused: {} the input directory ({}) -- this tool writes "
                       "ONLY somewhere else, and refuses outright rather than risk overwriting an "
                       "original .ocmesh",
                       writeDirArg.string(),
                       eqEc ? "the filesystem could not confirm it differs from"
                            : "it resolves to the SAME directory as",
                       inputAnchor.string());
            return exitCode(ExitCode::Usage);
        }
        writeDir = writeDirArg;
        AVER_INFO("[Relod] --write: re-cooked copies will be saved to {} (inputs are never modified)",
                  writeDir.string());
    }

    if (std::filesystem::is_directory(root, ec)) {
        std::vector<std::filesystem::path> files;
        for (const auto& e : std::filesystem::directory_iterator(root, ec))
            if (e.is_regular_file(ec) && e.path().extension() == ".ocmesh") files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const auto& f : files) relod(f, doWrite ? &writeDir : nullptr, force, t);
    } else {
        relod(root, doWrite ? &writeDir : nullptr, force, t);
    }

    AVER_INFO("[Relod] {} file(s), {} failed, {} invalid, {} would get a coarser floor -- "
              "summed coarsest level {} -> {} triangles",
              t.files, t.failed, t.invalid, t.improved, t.oldCoarsest, t.newCoarsest);

    if (!doWrite) {
        AVER_INFO("[Relod] nothing was written; this tool only reports.");
    } else {
        // Staleness, PART C's second deliverable: how many of the meshes this run just READ (the
        // original files' own on-disk builderVersion, from before this run touched anything) were
        // already current vs. stale vs. from before this field existed at all. This is the line that
        // tells someone the corpus needs re-cooking -- and, per the task, later becomes the DDC's own
        // freshness key, read instead of re-derived.
        AVER_INFO("[Relod] on-disk builder staleness: {} current, {} stale, {} unknown (pre-dates builderVersion)",
                  t.currentCount, t.staleCount, t.unknownCount);
        AVER_INFO("[Relod] wrote {} re-cooked file(s) to {}, {} failed to write -- every input was left untouched",
                  t.written, writeDir.string(), t.writeFailed);
    }
    // A DAG that fails its own invariants is a failure of the run, not a footnote in it: a cook
    // driven off this ladder would produce cracks. Non-zero exit so a sweep cannot pass silently.
    // t.writeFailed is always 0 when --write was never passed (relod() never increments it outside
    // its `if (writeDir)` block), so this is exactly the original condition in that case.
    return exitCode((t.failed || t.invalid || t.writeFailed) ? ExitCode::Failed : ExitCode::Ok);
}
