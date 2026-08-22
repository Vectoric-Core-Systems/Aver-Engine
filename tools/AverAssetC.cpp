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
#include "aver/formats/GltfImport.hpp"
#include "aver/formats/ObjImport.hpp"
#include "aver/formats/UsdImport.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/core/Log.hpp"

#if AVER_HAVE_AUDIO_IMPORT
#include "aver/formats/OcAudio.hpp"
#endif

// Clustering is entirely OPTIONAL, exactly as in ConvertTool.cpp: this tool must still import and
// write .ocmesh with AVER_MODULE_TRIFACTOR=OFF (the tree's default), so every use of it is behind
// this guard, reached only through the link interface (`if(TARGET Aver.Trifactor)` below).
#if AVER_MODULE_TRIFACTOR
#include "aver/trifactor/ClusterBuilder.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace aver;

namespace {

// ---- small helpers, ported from ConvertTool.cpp (see that file for the reasoning) ----

std::string stemOf(const std::string& path) {
    usize a = path.find_last_of("/\\");
    a = (a == std::string::npos) ? 0 : a + 1;
    const usize b = path.find_last_of('.');
    return path.substr(a, (b == std::string::npos || b < a) ? std::string::npos : b - a);
}

std::string extOf(const std::string& path) {
    const usize b = path.find_last_of('.');
    if (b == std::string::npos) return {};
    std::string e = path.substr(b);
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

// A name safe to hang on a file, so an unnamed or oddly-named source node cannot escape into a path.
std::string safe(const std::string& in, const std::string& fallback) {
    std::string out;
    for (char c : in)
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
            out.push_back(c);
    return out.empty() ? fallback : out;
}

// ---- the JSON-Lines result contract ----

std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

void emit(const std::string& json) {
    std::fputs(json.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);   // a caller reading line-by-line must see this before the process can crash
}

struct RunStats { int written = 0; int failed = 0; };

void emitArtifact(const std::string& input, const std::string& output, const char* kind,
                  bool ok, bool verified, const std::string& error, RunStats& stats,
                  const std::string& extra = {}) {
    std::string j = "{\"schemaVersion\":1,\"input\":\"" + jsonEscape(input) + "\",\"output\":\""
        + jsonEscape(output) + "\",\"kind\":\"" + kind + "\",\"ok\":" + (ok ? "true" : "false")
        + ",\"verified\":" + (verified ? "true" : "false");
    if (!error.empty()) j += ",\"error\":\"" + jsonEscape(error) + "\"";
    if (!extra.empty()) j += "," + extra;
    j += "}";
    emit(j);
    if (ok && verified) ++stats.written; else ++stats.failed;
}

void emitSummary(const std::string& input, const RunStats& stats, int exitCode) {
    emit("{\"schemaVersion\":1,\"summary\":true,\"input\":\"" + jsonEscape(input)
        + "\",\"itemsWritten\":" + std::to_string(stats.written)
        + ",\"itemsFailed\":" + std::to_string(stats.failed)
        + ",\"exitCode\":" + std::to_string(exitCode) + "}");
}

// ---- optional decimation + clustering, ported from ConvertTool.cpp unchanged ----

#if AVER_MODULE_TRIFACTOR
bool addMeshlets(fmt::OcMeshData& m, std::string* why) {
    aver::trifactor::LodDag dag;
    if (!aver::trifactor::buildClusters(m, dag, why)) return false;
    std::string hierWhy;
    if (!aver::trifactor::buildLodHierarchy(m, dag, &hierWhy))
        AVER_WARN("buildLodHierarchy: {} (saving LOD 0 only)", hierWhy);
    return aver::trifactor::packLodDag(dag, m, why);
}

// Best-effort, exactly like ConvertTool: a mesh too small/degenerate to cluster still gets saved,
// just without an MLET chunk.
void applyLodAndClustering(fmt::OcMeshData& m, f32 lodRatio, const std::string& label) {
    std::string why;
    if (lodRatio > 0.0f) {
        const usize before = m.indices.size() / 3;
        if (!aver::trifactor::simplifyMesh(m, lodRatio, &why)) {
            AVER_WARN("--lod {}: {} (saving '{}' at full density)", lodRatio, why, label);
        } else {
            AVER_INFO("simplified '{}' to {:.1f}%: {} -> {} tris", label, double(lodRatio) * 100.0,
                      before, m.indices.size() / 3);
        }
    }
    if (!addMeshlets(m, &why)) {
        AVER_WARN("clustering '{}': {} (saving without meshlets)", label, why);
    } else {
        AVER_INFO("clustered '{}': {} LOD(s), {} meshlets at LOD 0", label, m.lodCount(), m.meshlets.size());
    }
}
#endif

// ---- writing and verifying one mesh, ported from ConvertTool.cpp unchanged ----

// Writes one .ocmesh, reloads it, and confirms the skin (the one stream that used to be dropped
// silently) survived the round trip. Reports the outcome as one JSON line; never throws.
bool writeAndVerifyMesh(const std::string& input, const std::string& path, const fmt::OcMeshData& m,
                        RunStats& stats) {
    std::string why;
    if (!fmt::saveOcMesh(path, m, &why)) {
        emitArtifact(input, path, "mesh", false, false, why, stats);
        return false;
    }
    fmt::OcMeshData back;
    if (!fmt::loadOcMesh(path, back, &why)) {
        emitArtifact(input, path, "mesh", true, false, "reload failed: " + why, stats);
        return false;
    }
    if (m.hasSkin() && !back.hasSkin()) {
        emitArtifact(input, path, "mesh", true, false, "the skin did not survive the round trip", stats);
        return false;
    }
    std::string extra = "\"vertices\":" + std::to_string(back.vertexCount())
        + ",\"triangles\":" + std::to_string(back.indices.size() / 3)
        + ",\"hasSkin\":" + (back.hasSkin() ? "true" : "false");
    emitArtifact(input, path, "mesh", true, true, {}, stats, extra);
    return true;
}

// One imported mesh plus the skin identity it needs to merge safely. `skinIndex` mirrors
// GltfImportResult::meshSkinIndex: -1 when unskinned, otherwise an index two items share only when
// they are provably bound to the same skeleton (see GltfImport.cpp). OBJ and USD never populate a
// skin, so their items are always -1, which makes the merge below unconditionally safe for them.
struct MeshItem {
    std::string name;
    fmt::OcMeshData data;
    i32 skinIndex = -1;
};

// Merges every item into the first, following ConvertTool.cpp's exact rule: two items resolving to
// the SAME skin index share one bone-index space and merge their joints/weights too; a different (or
// one-sided) skin would remap joints wrongly, so the merge stops there instead, keeping only what
// was already merged. See ConvertTool.cpp for the full reasoning -- this is that algorithm, lifted
// out so both tools call the same logic rather than risk two copies drifting apart.
fmt::OcMeshData mergeAll(const std::vector<MeshItem>& items, usize& mergedCount, std::string* warn) {
    fmt::OcMeshData m = items[0].data;
    mergedCount = 1;
    const i32 accSkin = items[0].skinIndex;
    for (usize mi = 1; mi < items.size(); ++mi) {
        const fmt::OcMeshData& src = items[mi].data;
        if (src.positions.empty() || src.indices.empty()) continue;

        const i32 srcSkin = items[mi].skinIndex;
        const bool sameSkeleton = src.hasSkin() && m.hasSkin() && accSkin >= 0 && srcSkin == accSkin;
        if ((src.hasSkin() || m.hasSkin()) && !sameSkeleton) {
            if (warn) {
                *warn = "'" + items[mi].name + "' is skinned by a different skeleton than the "
                    + std::to_string(mergedCount) + " mesh(es) merged so far; only "
                    + std::to_string(mergedCount) + " of " + std::to_string(items.size())
                    + " meshes were merged";
            }
            break;
        }

        const u32 base = m.vertexCount();
        const u32 firstIndex = static_cast<u32>(m.indices.size());
        m.positions.insert(m.positions.end(), src.positions.begin(), src.positions.end());
        m.normals.insert(m.normals.end(), src.normals.begin(), src.normals.end());
        m.uvs.insert(m.uvs.end(), src.uvs.begin(), src.uvs.end());
        if (sameSkeleton) {
            m.joints.insert(m.joints.end(), src.joints.begin(), src.joints.end());
            m.weights.insert(m.weights.end(), src.weights.begin(), src.weights.end());
        }
        for (const u32 idx : src.indices) m.indices.push_back(idx + base);

        const u32 slotBase = static_cast<u32>(m.materialSlots.size());
        m.materialSlots.insert(m.materialSlots.end(), src.materialSlots.begin(), src.materialSlots.end());
        if (src.submeshes.empty()) {
            m.submeshes.push_back(fmt::OcMeshSubmesh{items[mi].name, slotBase, firstIndex,
                                                     static_cast<u32>(src.indices.size()), base,
                                                     src.vertexCount()});
        } else {
            for (fmt::OcMeshSubmesh sm : src.submeshes) {
                sm.materialSlot += slotBase;
                sm.indexStart   += firstIndex;
                sm.baseVertex   += base;
                m.submeshes.push_back(std::move(sm));
            }
        }

        m.boundsMin = Vec3{std::min(m.boundsMin.x, src.boundsMin.x), std::min(m.boundsMin.y, src.boundsMin.y),
                           std::min(m.boundsMin.z, src.boundsMin.z)};
        m.boundsMax = Vec3{std::max(m.boundsMax.x, src.boundsMax.x), std::max(m.boundsMax.y, src.boundsMax.y),
                           std::max(m.boundsMax.z, src.boundsMax.z)};
        ++mergedCount;
    }
    return m;
}

// Writes `items` as one merged .ocmesh (--merge) or one .ocmesh per item (the default), naming each
// output after its own mesh name -- sanitised, and de-duplicated within this run so two same-named
// source meshes cannot overwrite one another in the staging directory. Shared by all three mesh
// formats; glTF calls this and then separately writes its skeleton/clips, since OBJ and USD have
// neither.
bool writeMeshItems(const std::string& input, const std::string& outDir, const std::string& base,
                    const std::vector<MeshItem>& items, bool merge, f32 lodRatio, RunStats& stats) {
    bool anyFailed = false;
    if (merge && items.size() > 1) {
        usize mergedCount = 0;
        std::string warn;
        fmt::OcMeshData m = mergeAll(items, mergedCount, &warn);
        if (!warn.empty()) AVER_WARN("{}", warn);
#if AVER_MODULE_TRIFACTOR
        applyLodAndClustering(m, lodRatio, base);
#endif
        if (!writeAndVerifyMesh(input, outDir + "/" + base + ".ocmesh", m, stats)) anyFailed = true;
        return anyFailed;
    }

    std::vector<std::string> used;
    for (usize i = 0; i < items.size(); ++i) {
        fmt::OcMeshData m = items[i].data;
#if AVER_MODULE_TRIFACTOR
        applyLodAndClustering(m, lodRatio, items[i].name.empty() ? base : items[i].name);
#else
        (void)lodRatio;
#endif
        std::string stem = items.size() == 1 ? base : safe(items[i].name, base + std::to_string(i));
        std::string candidate = stem;
        int suffix = 1;
        while (std::find(used.begin(), used.end(), candidate) != used.end())
            candidate = stem + "_" + std::to_string(++suffix);
        used.push_back(candidate);
        if (!writeAndVerifyMesh(input, outDir + "/" + candidate + ".ocmesh", m, stats)) anyFailed = true;
    }
    return anyFailed;
}

} // namespace

// Converts argv[2] (a glTF/GLB, OBJ, USDA, or -- when this build has audio import -- a WAV/MP3/M4A/
// FLAC) into argv following --out-dir, naming outputs after --base or the source stem. Every result,
// and a final summary, are reported as JSON Lines on stdout; AVER_* log lines (always '['-prefixed)
// may be interleaved and are for human troubleshooting only. Exit 0: everything declared verified.
// Exit 1: an import or verify failure. Exit 2: bad usage.
int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) != "convert") {
        AVER_ERROR("usage: AverAssetC convert <input-file> --out-dir <dir> [--base <name>] [--merge] [--lod <ratio>]");
        return 2;
    }

    std::string input, outDir, baseOverride;
    bool merge = false;
    bool haveInput = false;
    f32 lodRatio = 0.0f;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out-dir" && i + 1 < argc)      outDir = argv[++i];
        else if (a == "--base" && i + 1 < argc)    baseOverride = argv[++i];
        else if (a == "--merge")                   merge = true;
        else if (a == "--lod" && i + 1 < argc)      lodRatio = static_cast<f32>(std::atof(argv[++i]));
        else if (!haveInput) { input = a; haveInput = true; }
    }
    if (!haveInput || outDir.empty()) {
        AVER_ERROR("usage: AverAssetC convert <input-file> --out-dir <dir> [--base <name>] [--merge] [--lod <ratio>]");
        return 2;
    }
    while (!outDir.empty() && (outDir.back() == '\\' || outDir.back() == '/')) outDir.pop_back();
    const std::string base = !baseOverride.empty() ? baseOverride : stemOf(input);

    RunStats stats;
    const std::string ext = extOf(input);

    if (ext == ".gltf" || ext == ".glb") {
        fmt::GltfImportResult res;
        std::string why;
        if (!fmt::importGltf(input, res, {}, &why)) {
            emitArtifact(input, {}, "mesh", false, false, why, stats);
            emitSummary(input, stats, 1);
            return 1;
        }
        for (const std::string& u : res.unsupported) AVER_WARN("unsupported: {}", u);
        if (res.meshes.empty()) {
            emitArtifact(input, {}, "mesh", false, false, "no meshes", stats);
            emitSummary(input, stats, 1);
            return 1;
        }

        std::vector<MeshItem> items;
        items.reserve(res.meshes.size());
        for (usize i = 0; i < res.meshes.size(); ++i) {
            items.push_back(MeshItem{
                i < res.meshNames.size() ? res.meshNames[i] : std::string{},
                res.meshes[i],
                i < res.meshSkinIndex.size() ? res.meshSkinIndex[i] : -1});
        }

        bool anyFailed = writeMeshItems(input, outDir, base, items, merge, lodRatio, stats);

        bool anySkinned = false;
        for (const fmt::OcMeshData& mesh : res.meshes) if (mesh.hasSkin()) { anySkinned = true; break; }
        if (anySkinned && res.skeletons.empty()) {
            AVER_WARN("the file carries skin but no skin node, so its joint indices address a "
                      "skeleton that was not written");
        }

        // ---- skeleton(s), named after `base` so a clip's skeletonRef resolves by stem ----
        for (usize i = 0; i < res.skeletons.size(); ++i) {
            const std::string p = outDir + "/" + base + (i == 0 ? "" : std::to_string(i)) + ".ocskel";
            if (!fmt::saveOcSkel(p, res.skeletons[i], &why)) {
                emitArtifact(input, p, "skeleton", false, false, why, stats);
                anyFailed = true;
            } else {
                emitArtifact(input, p, "skeleton", true, true, {}, stats,
                             "\"bones\":" + std::to_string(res.skeletons[i].bones.size()));
            }
        }

        // ---- clips ----
        for (usize i = 0; i < res.animations.size(); ++i) {
            fmt::OcAnimation clip = res.animations[i];
            clip.skeletonRef = base;
            const std::string name = safe(i < res.animationNames.size() ? res.animationNames[i] : "",
                                          "Clip" + std::to_string(i));
            const std::string p = outDir + "/" + base + "_" + name + ".ocanim";
            if (!fmt::saveOcAnim(p, clip, &why)) {
                emitArtifact(input, p, "animation", false, false, why, stats);
                anyFailed = true;
            } else {
                emitArtifact(input, p, "animation", true, true, {}, stats,
                             "\"durationSeconds\":" + std::to_string(clip.duration));
            }
        }

        emitSummary(input, stats, anyFailed ? 1 : 0);
        return anyFailed ? 1 : 0;
    }

    if (ext == ".obj") {
        fmt::ObjImportResult res;
        std::string why;
        if (!fmt::importObj(input, res, {}, &why)) {
            emitArtifact(input, {}, "mesh", false, false, why, stats);
            emitSummary(input, stats, 1);
            return 1;
        }
        for (const std::string& u : res.unsupported) AVER_WARN("unsupported: {}", u);
        if (res.meshes.empty()) {
            emitArtifact(input, {}, "mesh", false, false, "no meshes", stats);
            emitSummary(input, stats, 1);
            return 1;
        }
        std::vector<MeshItem> items;
        items.reserve(res.meshes.size());
        for (usize i = 0; i < res.meshes.size(); ++i) {
            items.push_back(MeshItem{i < res.meshNames.size() ? res.meshNames[i] : std::string{},
                                     res.meshes[i], -1});
        }
        const bool anyFailed = writeMeshItems(input, outDir, base, items, merge, lodRatio, stats);
        emitSummary(input, stats, anyFailed ? 1 : 0);
        return anyFailed ? 1 : 0;
    }

    if (ext == ".usd" || ext == ".usda") {
        fmt::UsdImportResult res;
        std::string why;
        if (!fmt::importUsd(input, res, {}, &why)) {
            emitArtifact(input, {}, "mesh", false, false, why, stats);
            emitSummary(input, stats, 1);
            return 1;
        }
        for (const std::string& u : res.unsupported) AVER_WARN("unsupported: {}", u);
        if (res.meshes.empty()) {
            emitArtifact(input, {}, "mesh", false, false, "no meshes", stats);
            emitSummary(input, stats, 1);
            return 1;
        }
        std::vector<MeshItem> items;
        items.reserve(res.meshes.size());
        for (usize i = 0; i < res.meshes.size(); ++i) {
            items.push_back(MeshItem{i < res.meshNames.size() ? res.meshNames[i] : std::string{},
                                     res.meshes[i], -1});
        }
        const bool anyFailed = writeMeshItems(input, outDir, base, items, merge, lodRatio, stats);
        emitSummary(input, stats, anyFailed ? 1 : 0);
        return anyFailed ? 1 : 0;
    }

#if AVER_HAVE_AUDIO_IMPORT
    if (fmt::isImportableAudio(input)) {
        audio::SoundData sound;
        fmt::AudioImportResult imp = fmt::audioImportFile(input, sound);
        if (!imp.ok) {
            emitArtifact(input, {}, "audio", false, false, imp.error, stats);
            emitSummary(input, stats, 1);
            return 1;
        }
        const std::string path = outDir + "/" + base + ".ocaudio";
        std::string why;
        if (!fmt::saveOcAudio(path, sound, stemOf(input), &why)) {
            emitArtifact(input, path, "audio", false, false, why, stats);
            emitSummary(input, stats, 1);
            return 1;
        }
        audio::SoundData back;
        const bool reloaded = fmt::loadOcAudio(path, back, &why);
        if (!reloaded || back.frames() != sound.frames() || back.channels != sound.channels) {
            emitArtifact(input, path, "audio", true, false,
                         !reloaded && !why.empty() ? why : "reloaded audio does not match the source",
                         stats);
            emitSummary(input, stats, 1);
            return 1;
        }
        emitArtifact(input, path, "audio", true, true, {}, stats,
                     "\"frames\":" + std::to_string(back.frames())
                     + ",\"channels\":" + std::to_string(back.channels)
                     + ",\"sampleRate\":" + std::to_string(back.sampleRate));
        emitSummary(input, stats, 0);
        return 0;
    }
#endif

    AVER_ERROR("unsupported input format: {}", input);
    emitArtifact(input, {}, "unknown", false, false, "unsupported input format", stats);
    emitSummary(input, stats, 1);
    return 1;
}
