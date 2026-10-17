// Editor: the script ABI provider for model import (framework_import_abi.h), and the extension router the
// Content Browser's importModel goes through. Editor build only: a packaged game installs no provider, so
// its import calls answer "not available". Design: docs/plans/wave9/FBX_IMPORT.md 2.10.

#include "ModelImportDialog.hpp"

#include "SandboxApp.hpp"
#include "aver/core/Log.hpp"
#include "aver/framework/framework_import_abi.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace aver {
namespace fsys = std::filesystem;

namespace {

ModelImportHost g_host;

void setMessage(char* out, int32_t cap, const std::string& text) {
    if (!out || cap <= 0) return;
    const usize keep = std::min<usize>(text.size(), static_cast<usize>(cap) - 1);
    std::memcpy(out, text.data(), keep);
    out[keep] = '\0';
}

// A Content-relative folder: no drive, no leading slash, no ".." component. Empty is the Content root.
bool contentRelativeOk(const std::string& rel) {
    if (rel.empty()) return true;
    if (rel[0] == '/' || rel[0] == '\\') return false;
    if (rel.size() > 1 && rel[1] == ':') return false;
    usize pos = 0;
    while (pos <= rel.size()) {
        usize end = rel.find_first_of("/\\", pos);
        if (end == std::string::npos) end = rel.size();
        if (rel.compare(pos, end - pos, "..") == 0) return false;
        pos = end + 1;
    }
    return true;
}

int32_t AVER_FW_CALL providerFn(const char* utf8Source, const char* utf8Dest, const char* utf8Options, int32_t overwrite,
                                char* outMessage, int32_t cap, int32_t* counts, void* /*user*/) {
    const auto refuse = [&](const std::string& why) {
        setMessage(outMessage, cap, why);
        return int32_t(0);
    };
    const std::string contentDir = g_host.contentDir ? g_host.contentDir() : std::string();
    if (contentDir.empty()) return refuse("no project is open, so there is no Content folder to import into");

    std::string rel = utf8Dest ? utf8Dest : "";
    if (!contentRelativeOk(rel))
        return refuse("the destination must be a folder inside Content (no drive, no leading slash, no '..'): " + rel);
    while (!rel.empty() && (rel.back() == '/' || rel.back() == '\\')) rel.pop_back();
    const std::string destDir = rel.empty() ? contentDir : contentDir + "/" + rel;

    std::string src = utf8Source ? utf8Source : "";
    if (src.empty()) return refuse("no source file was given");
    if (!fsys::path(src).is_absolute()) {
        const std::string projectDir = g_host.projectDir ? g_host.projectDir() : std::string();
        if (projectDir.empty()) return refuse("a relative source needs an open project: " + src);
        src = projectDir + "/" + src;
    }
    std::error_code ec;
    if (!fsys::exists(src, ec)) return refuse("source not found: " + src);

    ModelImportSummary sum;
    std::string why;
    bool ok = false;
    switch (modelFormatOfPath(src)) {
        case ModelFormat::Fbx: {
            fmt::FbxImportOptions opt;
            if (utf8Options && *utf8Options && !fmt::fbxOptionsFromText(utf8Options, opt, &why)) return refuse(why);
            ok = importFbxToDir(src, destDir, contentDir, overwrite != 0, opt, sum, &why);
            break;
        }
        case ModelFormat::Gltf: {
            GltfImportSummary gs;
            ok = importGltfToDir(src, destDir, contentDir, overwrite != 0, gs, &why);
            sum.meshesWritten = gs.meshesWritten; sum.rigsWritten = gs.rigsWritten; sum.clipsWritten = gs.clipsWritten;
            sum.materialsWritten = gs.materialsWritten; sum.texturesWritten = gs.texturesWritten;
            break;
        }
        default:
            return refuse("not a model file this editor imports (.fbx, .gltf, .glb): " + src);
    }
    if (!ok) return refuse(why.empty() ? "the import failed" : why);

    if (counts) {
        counts[0] = static_cast<int32_t>(sum.meshesWritten);
        counts[1] = static_cast<int32_t>(sum.rigsWritten);
        counts[2] = static_cast<int32_t>(sum.clipsWritten);
        counts[3] = static_cast<int32_t>(sum.materialsWritten);
        counts[4] = static_cast<int32_t>(sum.texturesWritten);
    }
    const std::string name = fsys::path(src).filename().string();
    if (sum.meshesWritten == 0 && sum.rigsWritten == 0 && sum.clipsWritten == 0) {
        setMessage(outMessage, cap, name + " parsed but nothing new was written (everything may already exist; pass overwrite)");
        return 0;
    }
    setMessage(outMessage, cap, "imported " + name + ": " + std::to_string(sum.meshesWritten) + " mesh(es), " +
                                std::to_string(sum.rigsWritten) + " skeleton(s), " + std::to_string(sum.clipsWritten) + " clip(s)");
    if (g_host.imported) g_host.imported(destDir);
    return 1;
}

} // namespace

void installModelImportProvider(const ModelImportHost& host) {
    g_host = host;
    aver_fw_set_import_provider(&providerFn, nullptr);
}

bool importModelFile(const std::string& src, const std::string& destDir, const std::string& contentDir,
                     bool overwrite, ModelImportSummary& out, std::string* why) {
    switch (modelFormatOfPath(src)) {
        case ModelFormat::Fbx: {
            fmt::FbxImportOptions opt;
#if AVER_WITH_IMGUI
            modelImportTakeArmedOptions(src, opt);      // the Import dialog's choices; a drag-and-drop gets the defaults
#endif
            return importFbxToDir(src, destDir, contentDir, overwrite, opt, out, why);
        }
        case ModelFormat::Gltf: {
            GltfImportSummary gs;
            const bool ok = importGltfToDir(src, destDir, contentDir, overwrite, gs, why);
            out.meshesWritten = gs.meshesWritten; out.rigsWritten = gs.rigsWritten; out.clipsWritten = gs.clipsWritten;
            out.materialsWritten = gs.materialsWritten; out.texturesWritten = gs.texturesWritten;
            return ok;
        }
        default:
            if (why) *why = "not a model file this editor imports: " + src;
            return false;
    }
}

} // namespace aver
