// Runs the asset validator from the editor and writes every issue to the log, which is what the
// Message Log panel shows. Shared by the `validate` console command and the Content Browser menu.
#pragma once

#include "aver/core/Log.hpp"
#include "aver/formats/AssetValidate.hpp"

#include <filesystem>
#include <string>

namespace aver::editor {

// The open project's content directory, kept current by the project loader. Empty with no project.
inline std::string& toolingContentDirSlot() { static std::string s; return s; }

// Validates `target` (a path under contentDir, a content-relative path, or empty for everything).
// Returns the one-line summary; each issue has already been logged.
inline std::string runAssetValidation(const std::string& contentDir, const std::string& target) {
    namespace fs = std::filesystem;
    if (contentDir.empty()) return "No project is open.";
    fmt::ValidateContext ctx;
    ctx.contentDir = contentDir;
    const fmt::AssetValidator validator;
    fmt::ValidateReport rep;

    std::error_code ec;
    fs::path p = target;
    if (!target.empty() && p.is_relative()) p = fs::path(contentDir) / p;
    std::string rel = target.empty() ? std::string() : fs::relative(p, contentDir, ec).generic_string();
    if (ec) rel.clear();
    if (!target.empty() && fs::is_regular_file(p, ec)) {
        validator.validateFile(ctx, rel, rep);
    } else if (!target.empty() && fs::is_directory(p, ec) && !rel.empty() && rel != ".") {
        const fmt::ValidateReport all = validator.validateAll(ctx);
        for (const fmt::ValidateIssue& i : all.issues)
            if (i.asset.rfind(rel + "/", 0) == 0) rep.issues.push_back(i);
        rep.filesChecked = all.filesChecked;
    } else {
        rep = validator.validateAll(ctx);
    }

    for (const fmt::ValidateIssue& i : rep.issues) {
        const std::string line = fmt::formatValidateIssue(i);
        if (i.severity == fmt::ValidateSeverity::Error)        AVER_ERROR("[Validate] {}", line);
        else if (i.severity == fmt::ValidateSeverity::Warning) AVER_WARN("[Validate] {}", line);
        else                                                   AVER_INFO("[Validate] {}", line);
    }
    const std::string summary = "Validated " + std::to_string(rep.filesChecked) + " files: " +
        std::to_string(rep.count(fmt::ValidateSeverity::Error)) + " errors, " +
        std::to_string(rep.count(fmt::ValidateSeverity::Warning)) + " warnings, " +
        std::to_string(rep.count(fmt::ValidateSeverity::Info)) + " info";
    AVER_INFO("[Validate] {}", summary);
    return summary;
}

}  // namespace aver::editor
