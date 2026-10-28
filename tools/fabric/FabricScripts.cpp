// The script step of `AverAssetC fabric`: launches Aver.Fabric.Convert (the Roslyn / GDScript converter, a .NET tool staged to
// <exe dir>/Tools) on the manifest the scene step wrote, then reads the merged report back. Contract: docs/plans/fabric/G3_SCRIPTS.md.
#include "FabricInternal.hpp"
#include "FabricProcess.hpp"

#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace aver::fabric {

namespace {

namespace fsys = std::filesystem;

// The engine checkout above the executable (a build tree inside a checkout), where Scripts.csproj's project references point.
std::string engineCheckout() {
    std::error_code ec;
    for (fsys::path p = fsys::path(executableDir()); !p.empty(); p = p.parent_path()) {
        if (fsys::exists(p / "scripting" / "csharp" / "Aver.Fabric" / "Aver.Fabric.csproj", ec)) return p.string();
        if (p == p.parent_path()) break;
    }
    return {};
}

// The number after `"key":` in a one-line JSON object, or `def`.
long jsonNumber(const std::string& line, const char* key, long def) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t at = line.find(needle);
    if (at == std::string::npos) return def;
    return std::strtol(line.c_str() + at + needle.size(), nullptr, 10);
}

}  // namespace

bool Converter::convertScripts() {
    scriptsRan_ = false;
    if (scripts_.empty()) return true;

    const std::string exeDir = executableDir();
    const std::string tool = exeDir + "/Tools/Aver.Fabric.Convert.dll";
    const std::string refs = exeDir + "/Scripting";
    const std::string dotnet = findDotnet();
    std::string why;
    if (dotnet.empty()) why = "dotnet was not found (install the .NET SDK or set DOTNET_ROOT)";
    else if (!fileExists(tool)) why = "Aver.Fabric.Convert.dll is not beside AverAssetC (" + tool + "): build the Aver.Fabric.Convert target";
    else if (!fileExists(refs + "/Aver.Fabric.dll")) why = "Aver.Fabric.dll is not in " + refs + ": build the Aver.Scripting.Bridge target";
    if (!why.empty()) {
        AVER_ERROR("fabric: scripts not converted: {}", why);
        for (const auto& [path, si] : scripts_)
            report_.add("script", path, "", si.kind, "", Rating::Unsupported, "the script was not converted: " + why, 0);
        return false;
    }

    std::error_code ec;
    const std::string godotAbs = fsys::absolute(godotDir_, ec).string();
    std::vector<std::string> args = {tool, "scripts", "--project", godotAbs, "--out", projectDir_, "--manifest", fabricDir_ + "/scripts.json",
                                     "--refs", refs, "--report", outDir_ + "/FabricReport.jsonl"};
    if (const std::string engine = engineCheckout(); !engine.empty()) {
        args.push_back("--engine");
        args.push_back(engine);
    }
    AVER_INFO("fabric: converting {} script(s) with Aver.Fabric.Convert", scripts_.size());
    std::string summary;
    const ProcessOutput out = runProcess(dotnet, args, [&](const std::string& line) {
        if (line.find("\"kind\":\"fabric-scripts\"") != std::string::npos) { summary = line; return; }
        if (!line.empty()) AVER_INFO("fabric: scripts: {}", line);
    });
    if (!out.started) {
        AVER_ERROR("fabric: could not start {}", dotnet);
        for (const auto& [path, si] : scripts_)
            report_.add("script", path, "", si.kind, "", Rating::Unsupported, "the script was not converted: could not start dotnet", 0);
        return false;
    }
    // 0 converted and compiled; 1 with a summary: compile errors remain (written, marked, reported); 3: written, not compiled (reported).
    // Anything else (no summary, usage error, a crash) is the step failing, not the scripts.
    const bool soft = out.exitCode == 0 || out.exitCode == 3 || (out.exitCode == 1 && !summary.empty());
    if (!soft) {
        AVER_ERROR("fabric: the script converter failed (exit {})", out.exitCode);
        for (const auto& [path, si] : scripts_)
            report_.add("script", path, "", si.kind, "", Rating::Unsupported, "the script converter failed (exit " + std::to_string(out.exitCode) + ")", 0);
        return false;
    }
    scriptsRan_ = true;
    if (!summary.empty())
        AVER_INFO("fabric: scripts: {} compiled={} errors={}", jsonNumber(summary, "scripts", 0), summary.find("\"compiled\":true") != std::string::npos ? "yes" : "no",
                  jsonNumber(summary, "errors", 0));
    // The tool merged its entries into the report file; ours is read back from it.
    Report merged;
    if (merged.readJsonl(outDir_ + "/FabricReport.jsonl")) report_ = std::move(merged);
    return true;
}

}  // namespace aver::fabric
