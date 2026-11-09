#include "AssetC.hpp"

namespace assetc {

// `validate <project-dir|content-dir>`: runs the asset validation rules (AssetValidate.hpp). Exit 1 on any error
// (or any warning with --strict).
int runValidate(int argc, char** argv) {
    std::string root;
    std::vector<std::string> extra;
    bool strict = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--engine-content" && i + 1 < argc) extra.push_back(argv[++i]);
        else if (a == "--strict")                    strict = true;
        else if (root.empty() && a.rfind("--", 0) != 0) root = a;
        else { AVER_ERROR("[validate] unknown argument '{}'", a); return exitCode(ExitCode::Usage); }
    }
    if (root.empty()) {
        AVER_ERROR("[validate] usage: AverAssetC validate <project-dir|content-dir> [--engine-content <dir>]... [--strict]");
        return exitCode(ExitCode::Usage);
    }
    std::error_code ec;
    fmt::ValidateContext ctx;
    ctx.contentDir = std::filesystem::is_directory(std::filesystem::path(root) / "Content", ec)
                         ? (std::filesystem::path(root) / "Content").string() : root;
    ctx.extraRoots = extra;
    if (!std::filesystem::is_directory(ctx.contentDir, ec)) {
        AVER_ERROR("[validate] '{}' is not a directory", ctx.contentDir);
        return exitCode(ExitCode::Usage);
    }
    const fmt::ValidateReport rep = fmt::AssetValidator().validateAll(ctx);
    for (const fmt::ValidateIssue& i : rep.issues) {
        if (i.severity == fmt::ValidateSeverity::Error)        AVER_ERROR("{}", fmt::formatValidateIssue(i));
        else if (i.severity == fmt::ValidateSeverity::Warning) AVER_WARN("{}", fmt::formatValidateIssue(i));
        else                                                   AVER_INFO("{}", fmt::formatValidateIssue(i));
    }
    const u32 errors = rep.count(fmt::ValidateSeverity::Error), warnings = rep.count(fmt::ValidateSeverity::Warning);
    AVER_INFO("[validate] {} files checked: {} errors, {} warnings, {} info", rep.filesChecked, errors, warnings,
              rep.count(fmt::ValidateSeverity::Info));
    return (errors > 0 || (strict && warnings > 0)) ? exitCode(ExitCode::Failed) : exitCode(ExitCode::Ok);
}

} // namespace assetc
