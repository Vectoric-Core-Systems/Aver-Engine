// AverAssetC — the standalone asset compiler the launcher's Aver Exchange feature invokes out of
// process.
//
// Not built on top of ConvertTool.cpp, but built FROM it: that harness proved the import, merge and
// reload-and-verify path against real files (see its own header comment for the polycount numbers
// that motivated the merge), but it is a dev tool that writes AVER_INFO text and only reads glTF. A
// caller across a process boundary needs a result it can parse without screen-scraping and formats
// beyond glTF, so this tool ports ConvertTool's merge algorithm and verify discipline unchanged and
// adds: OBJ/USDA/audio dispatch, per-mesh output by default (a multi-prop source stays separable
// unless --merge is asked for), and JSON-Lines results instead of log lines.
//
// THE RESULT CONTRACT. AVER_INFO/WARN/ERROR always write to stdout (Log.cpp's logWrite does that
// unconditionally; setLogSink only mirrors, it does not suppress), and every one of those lines
// starts with '['. So this tool's own result records are the only lines starting with '{': one JSON
// object PER ARTIFACT, flushed immediately after it is written, plus a required final
// {"summary":true,...} line on a clean exit. One line per artifact rather than one document for the
// whole run means a crash after writing N of M outputs still leaves N individually verified,
// trustworthy result lines -- a caller that never saw the summary line treats the whole run as
// incomplete, but nothing already reported as ok+verified is a lie.

#include "assetc/AssetC.hpp"

using namespace aver;
using namespace assetc;

namespace assetc {

const char* const kUsage =
        "usage: AverAssetC convert <input-file> --out-dir <dir> [--base <name>] [--merge] [--lod <ratio>]"
        "\n                          [--instances-as foliage|entities]   USD PointInstancer instances as"
        "\n                                                              baked .ocinst foliage (default) or PLACE entities"
        "\n                          [--max-instances <n>] [--max-instance-tris <n>]   USD PointInstancer budget, 0 = none;"
        "\n                                                              defaults depend on --instances-as (see docs/ASSET_IMPORT.md)"
        "\n                          [--focus camera|none|<x>,<y>] [--focus-radius <cm>] [--keep-all-below <n>]"
        "\n                          [--exclude <prim path>[,<prim path>...]]   USD prims to leave out"
        "\n                          [--material-map <file>]   USD: '<pattern> <material stem|->' per line, for subsets with"
        "\n                                                              no bound material; most specific rule wins (see docs/ASSET_IMPORT.md)"
        "\n                          [--material-map-legacy]   first substring match in file order, as before the wildcard syntax"
        "\n                          [--instances-dir <content-relative dir>]   where the level's .ocinst goes (Instances)"
        "\n                          [--import-report <file>]   USD, FBX: write the importer's diagnostics as text"
        "\n                          [--scale <f>] [--unit-meters <f>] [--root-motion off|auto|all] [--bake-fps <n>]"
        "\n                          [--no-animations] [--no-blend-shapes]   FBX only: see docs/FBX_IMPORT.md"
        "\n                          [--resolve-coplanar report|drop-later]   USD: coplanar duplicate triangles (report)"
        "\n                          [--trace-prims <prim>[,<prim>...]]   USD: log how these prims were composed"
#if AVER_HAVE_MATERIAL_COMPILE
        "\n                          [--content-dir <dir>]   write materials and textures too"
        "\n                          [--max-texture <n>]     downscale imported textures to n px"
#endif
#if AVER_HAVE_MATERIAL_COMPILE
        "\n       AverAssetC material <texture-file>... --out-dir <dir> --base <name>"
#endif
        "\n       AverAssetC stream <level.ocworld> --content-dir <dir> [--cell m] [--load m] [--evict m]"
        "\n                          [--foliage-cell m]   bake placement bounds, ids, STREAM record, foliage cells"
        "\n       AverAssetC hlod <level.ocworld> --content-dir <dir> [--project-dir <dir>] [--hlod m] [--max-tris n]"
        "\n                          [--min-tris n] [--drop-smaller m] [--drop-enclosed] [--force]   bake per-cell HLOD proxies (needs `stream`)"
#if AVER_HAVE_COOK
        "\n       AverAssetC cook-chunks <level.ocworld> --content-dir <dir> [--project-dir <dir>] [--out <dir>] [--chunk <cm>]"
        "\n                          [--wide <m>] [--check] [--no-header]   cook the level into streamable region files under"
        "\n                          <project>/Binaries/Chunks/<level> (--check: exit 1 when the set is missing or stale)"
#endif
        "\n       AverAssetC retarget <asset.ocretarget> --clips <file|dir>... --out <dir> [--suffix S] [--fps N]"
        "\n                          [--overwrite] [--content-dir <dir>]   carry clips onto the target skeleton"
        "\n       AverAssetC visbake <track.ocvis> --rig <x.ocface> --out <clip.ocanim> [--fps N]   lip-sync track -> curve-only clip"
        "\n       AverAssetC hair <file.gltf|.glb> --out <groom.ochair> [--scalp <mesh.ocmesh>] [--scalp-asset <ref>] [--submesh N]"
        "\n                          [--joints a,b,c] [--default-width-cm F] [--max-root-cm F]   glTF line strands -> strand groom"
        "\n       AverAssetC hair-text <groom.txt> --out <groom.ochair>   the HEAD records of a groom (SCALP, SIM, MATERIAL, LOOK, GENERATE ...) as text -> .ochair"
        ;

} // namespace assetc

// `convert` converts argv[2] (a glTF/GLB, OBJ, USD stage, or -- when this build has audio import -- a
// WAV/MP3/M4A/FLAC) into argv following --out-dir, naming outputs after --base or the source stem.
// `material` (when this build has material compiling) converts a whole texture SET the same way --
// see runMaterial's own comment for why it is a separate subcommand rather than another `convert`
// extension. Every result, and a final summary, are reported as JSON Lines on stdout; AVER_* log
// lines (always '['-prefixed) may be interleaved and are for human troubleshooting only. Exit 0:
// everything declared verified. Exit 1: an import or verify failure. Exit 2: bad usage (incl.
// unsupported format or unreadable --material-map).
int main(int argc, char** argv) {
    // The usage text, then the commands installed extensions add. Printed for a usage error (stderr, exit 2) and for --help (exit 0).
    auto usageWithExtensions = [&]() {
        ext::Registry registry;
        openExtensions(registry);
        return std::string(kUsage) + extensionUsage(registry);
    };

    if (argc < 2) {
        AVER_ERROR("{}", usageWithExtensions());
        return exitCode(ExitCode::Usage);
    }
    const std::string subcommand = argv[1];
    if (subcommand == "--help" || subcommand == "-h" || subcommand == "help") {
        AVER_INFO("{}", usageWithExtensions());
        return exitCode(ExitCode::Ok);
    }

#if AVER_HAVE_MATERIAL_COMPILE
    if (subcommand == "material") return runMaterial(argc, argv);
#endif

    if (subcommand == "stream") return runStream(argc, argv);
    if (subcommand == "hlod") return runHlod(argc, argv);
#if AVER_HAVE_COOK
    if (subcommand == "cook-chunks") return runCookChunks(argc, argv);
#endif
    if (subcommand == "retarget") return runRetarget(argc, argv);
    if (subcommand == "visbake") return runVisBake(argc, argv);
    if (subcommand == "validate") return runValidate(argc, argv);
    if (subcommand == "hair") return runHair(argc, argv);
    if (subcommand == "hair-text") return runHairText(argc, argv);

    if (subcommand != "convert") {
        // Not one of ours: an installed extension may list it. argv[1..] is the extension's argv (argv[0] there is the command).
        ext::Registry registry;
        openExtensions(registry);
        int code = 0;
        std::string why;
        if (registry.run(subcommand, argc - 1, argv + 1, code, &why)) return code;
        AVER_ERROR("unknown command '{}': {}\n{}{}", subcommand, why, kUsage, extensionUsage(registry));
        return exitCode(ExitCode::Usage);
    }

    return runConvert(argc, argv);
}
