// `AverAssetC fabric`: the driver, the script registry and the manifests (Fabric/scripts.json, Fabric/project.json).
#include "FabricConvert.hpp"
#include "FabricInternal.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <sstream>

namespace aver::fabric {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> words(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : line) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string identifierAt(const std::string& s, size_t from) {
    while (from < s.size() && std::isspace(static_cast<unsigned char>(s[from]))) ++from;
    size_t e = from;
    while (e < s.size() && (std::isalnum(static_cast<unsigned char>(s[e])) || s[e] == '_')) ++e;
    return s.substr(from, e - from);
}

void scanGdScript(const std::string& text, ScriptInfo& si) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> w = words(line);
        for (size_t i = 0; i < w.size(); ++i) {
            if (w[i] == "class_name" && i + 1 < w.size() && si.className.empty()) {
                si.className = identifierAt(w[i + 1], 0);
                si.declaredClassName = !si.className.empty();
            } else if (w[i] == "extends" && i + 1 < w.size() && si.extends.empty() && si.extendsPath.empty()) {
                const std::string& v = w[i + 1];
                if (v[0] == '"' || v[0] == '\'') si.extendsPath = v.substr(1, v.size() >= 2 ? v.size() - 2 : 0);
                else si.extends = identifierAt(v, 0);
            }
        }
        if (w[0] == "func" || w[0] == "var" || w[0] == "const" || w[0] == "signal" || w[0] == "@export" || w[0] == "enum") break;
    }
}

void scanCsScript(const std::string& text, const std::string& stem, ScriptInfo& si) {
    std::istringstream in(text);
    std::string line;
    std::string bestName, bestBase;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.rfind("//", 0) == 0) continue;
        size_t at = 0;
        while ((at = t.find("class ", at)) != std::string::npos) {
            if (at > 0 && !std::isspace(static_cast<unsigned char>(t[at - 1]))) { at += 6; continue; }
            const std::string name = identifierAt(t, at + 6);
            if (name.empty()) break;
            std::string base;
            const size_t colon = t.find(':', at + 6 + name.size());
            if (colon != std::string::npos) base = identifierAt(t, colon + 1);
            if (bestName.empty() || name == stem) { bestName = name; bestBase = base; }
            break;
        }
    }
    si.className = bestName;
    si.declaredClassName = !bestName.empty();
    si.extends = bestBase;
}

bool reservedClassName(const std::string& n) {
    static const std::set<std::string> extra = {"Object", "GodotObject", "RefCounted", "Resource", "Mesh", "Material", "Shape3D", "Texture2D", "PackedScene",
                                                "SceneTree", "Variant", "Callable", "Signal", "StringName", "NodePath", "Vector2", "Vector3",
                                                "Transform3D", "Basis", "Quaternion", "Color", "Aabb", "Plane", "Projection", "GD", "Mathf", "Input"};
    return extra.count(n) || isControlClass(n) || is2DClass(n) || isNode3DClass(n) || n == "Node" || n == "Timer" || n == "Viewport" ||
           n == "Window" || n == "CanvasItem" || n == "WorldEnvironment" || n == "AnimationPlayer" || n == "AudioStreamPlayer" ||
           n == "AudioStreamPlayer3D" || n == "AudioListener3D" || n == "AudioServer" || n == "AudioStream" || n == "NavigationAgent3D" ||
           n == "AStar3D" || n == "AStar2D" || n == "AStarGrid2D" || n == "NavigationMesh";
}

}  // namespace

Converter::Converter(std::string godotDir, std::string outDir, Host host)
    : godotDir_(std::move(godotDir)), outDir_(std::move(outDir)), host_(std::move(host)) {}

// ---- scripts -------------------------------------------------------------------------------------------------------

void Converter::registerScripts() {
    for (const std::string& res : fs_.files()) {
        const size_t d = res.find_last_of('.');
        if (d == std::string::npos) continue;
        const std::string ext = lower(res.substr(d));
        if (ext != ".gd" && ext != ".cs") continue;
        script(res);
    }
    // Resolve `extends` through other scripts to the Godot class at the bottom.
    for (auto& [path, si] : scripts_) {
        std::string ext = si.extends;
        std::string viaPath = si.extendsPath;
        for (int hop = 0; hop < 16; ++hop) {
            const ScriptInfo* next = nullptr;
            if (!viaPath.empty()) {
                const auto it = scripts_.find(gd::GodotProjectFs::normalizeRes(viaPath));
                if (it != scripts_.end()) next = &it->second;
            } else if (!ext.empty()) {
                for (const auto& [p2, other] : scripts_)
                    if (other.declaredClassName && other.className == ext) { next = &other; break; }
            }
            if (!next) break;
            ext = next->extends;
            viaPath = next->extendsPath;
            if (ext.empty() && viaPath.empty()) break;
        }
        si.extends = ext.empty() ? "Node" : ext;
    }
}

ScriptInfo* Converter::script(const std::string& resPath) {
    if (resPath.empty()) return nullptr;
    if (const auto it = scripts_.find(resPath); it != scripts_.end()) return &it->second;
    ScriptInfo si;
    si.path = resPath;
    const size_t d = resPath.find_last_of('.');
    const std::string ext = d == std::string::npos ? std::string{} : lower(resPath.substr(d));
    si.kind = ext == ".cs" ? "csharp" : "gdscript";
    std::string text;
    const std::string disk = fs_.toDiskPath(resPath);
    si.exists = !disk.empty() && readFileText(disk, text);
    if (si.exists) {
        if (si.kind == "csharp") scanCsScript(text, stemOf(resPath), si);
        else scanGdScript(text, si);
    }
    // A sidecar uid, when the project has one.
    std::string uidText;
    if (!disk.empty() && readFileText(disk + ".uid", uidText)) si.uid = trim(uidText);

    std::string name = si.className.empty() ? pascalCase(stemOf(resPath)) : si.className;
    if (reservedClassName(name)) name += "Script";
    std::string unique = name;
    for (int n = 2; usedClassNames_.count(unique); ++n) unique = name + std::to_string(n);
    usedClassNames_.insert(unique);
    si.className = unique;
    if (si.extends.empty() && si.extendsPath.empty()) si.extends = "Node";   // a script with no extends, or a missing file
    return &(scripts_[resPath] = std::move(si));
}

namespace {

std::string jsonList(const std::vector<std::string>& v) {
    std::string out = "[";
    for (size_t i = 0; i < v.size(); ++i) out += (i ? "," : "") + jsonString(v[i]);
    return out + "]";
}

}  // namespace

bool Converter::writeScriptsManifest() {
    std::string j = "{\n  \"schema\": 1,\n  \"project\": " + jsonString(name_) + ",\n  \"scripts\": [";
    bool first = true;
    for (const auto& [path, si] : scripts_) {
        j += first ? "\n" : ",\n";
        first = false;
        j += "    {\"path\": " + jsonString(si.path) + ", \"kind\": " + jsonString(si.kind) + ", \"className\": " + jsonString(si.className) +
             ", \"declaredClassName\": " + (si.declaredClassName ? "true" : "false") + ", \"extends\": " + jsonString(si.extends) +
             ", \"extendsPath\": " + jsonString(si.extendsPath) + ", \"exists\": " + (si.exists ? "true" : "false") +
             ", \"autoload\": " + (si.autoload ? "true" : "false") + ", \"uid\": " + jsonString(si.uid) +
             ", \"output\": " + jsonString("Content/Scripts/" + si.className + ".cs") + ", \"uses\": " + jsonList(si.uses) + "}";
    }
    j += first ? "]\n}\n" : "\n  ]\n}\n";
    return writeFileText(fabricDir_ + "/scripts.json", j);
}

bool Converter::writeProjectManifest() {
    std::string j = "{\n  \"schema\": 1,\n  \"name\": " + jsonString(name_) + ",\n  \"godotName\": " + jsonString(project_.name) +
                    ",\n  \"godotVersion\": " + jsonString(project_.engineVersion) + ",\n  \"usesCSharp\": " + (project_.usesCSharp ? "true" : "false") +
                    ",\n  \"renderingMethod\": " + jsonString(project_.renderingMethod) + ",\n  \"startScene\": " + jsonString(startSceneRes_) +
                    ",\n  \"startLevel\": " + jsonString(startLevelAsset_) + ",\n  \"levels\": " + jsonList(sceneLevels_) +
                    ",\n  \"prefabs\": " + jsonList(prefabs_) + ",\n  \"autoloads\": [";
    for (size_t i = 0; i < project_.autoloads.size(); ++i) {
        const gd::GodotAutoload& a = project_.autoloads[i];
        j += std::string(i ? "," : "") + "\n    {\"name\": " + jsonString(a.name) + ", \"path\": " + jsonString(a.path) + ", \"singleton\": " +
             (a.singleton ? "true" : "false") + "}";
    }
    j += project_.autoloads.empty() ? "]" : "\n  ]";
    j += ",\n  \"layerNames\": {";
    for (size_t i = 0; i < project_.layerNames.size(); ++i)
        j += std::string(i ? ", " : "") + jsonString(project_.layerNames[i].first) + ": " + jsonString(project_.layerNames[i].second);
    j += "},\n  \"enabledPlugins\": " + jsonList(project_.enabledPlugins);
    j += ",\n  \"environment\": {\"found\": " + std::string(projectEnv_.found ? "true" : "false") + ", \"tonemapMode\": " + std::to_string(projectEnv_.tonemapMode) +
         ", \"tonemapExposure\": " + std::to_string(projectEnv_.tonemapExposure) + ", \"glow\": " + (projectEnv_.glow ? "true" : "false") +
         ", \"source\": " + jsonString(projectEnv_.source) + "}";
    j += ",\n  \"extensions\": [";
    for (size_t i = 0; i < extensions_.size(); ++i) {
        const ExtensionInfo& x = extensions_[i];
        j += std::string(i ? "," : "") + "\n    {\"path\": " + jsonString(x.resPath) + ", \"content\": " + jsonString("Content/" + x.contentPath) +
             ", \"entrySymbol\": " + jsonString(x.entrySymbol) + ", \"compatibilityMinimum\": " + jsonString(x.compatibilityMinimum) +
             ", \"compatibilityMaximum\": " + jsonString(x.compatibilityMaximum) + ", \"hostLibrary\": " + (x.hostLibrary ? "true" : "false") +
             ", \"classes\": " + jsonList(x.classes) + ", \"files\": " + jsonList(x.libraries) + ", \"missing\": " + jsonList(x.missing) + "}";
    }
    j += extensions_.empty() ? "]" : "\n  ]";
    j += "\n}\n";
    return writeFileText(fabricDir_ + "/project.json", j);
}

// ---- the run ---------------------------------------------------------------------------------------------------------------

int Converter::run() {
    gd::GodotDiagnostics diag;
    if (!fs_.open(godotDir_, diag)) {
        AVER_ERROR("fabric: {}", diag.format());
        return 1;
    }
    if (!fs_.scan(diag)) AVER_WARN("fabric: scanning the project reported errors");

    gd::GodotDiagnostics pdiag;
    if (!gd::loadGodotProject(godotDir_, project_, pdiag)) {
        AVER_ERROR("fabric: project.godot: {}", pdiag.format());
        return 1;
    }

    std::string folder = std::filesystem::path(godotDir_).filename().string();
    if (folder.empty()) folder = std::filesystem::path(godotDir_).parent_path().filename().string();
    name_ = safeName(project_.name, safeName(folder, "FabricProject"));

    // A dry run converts into a scratch folder and keeps only the report.
    const std::string requestedOut = outDir_;
    if (options_.dryRun) {
        std::error_code ec;
        outDir_ = (std::filesystem::temp_directory_path(ec) / ("aver-fabric-dry-" + fnvHex(godotDir_ + "|" + requestedOut))).string();
        std::filesystem::remove_all(outDir_, ec);
    }
    projectDir_ = outDir_ + "/" + name_;
    contentDir_ = projectDir_ + "/Content";
    fabricDir_ = projectDir_ + "/Fabric";
    const std::string jsonlPath = outDir_ + "/FabricReport.jsonl", mdPath = outDir_ + "/FabricReport.md";

    if (!options_.scriptsOnly && !options_.overwrite && fileExists(projectDir_ + "/" + name_ + ".ocproject")) {
        AVER_ERROR("fabric: {} already holds a converted project; pass --overwrite to replace it (or --scripts-only to convert only its scripts)", projectDir_);
        return 1;
    }
    if (options_.scriptsOnly) {
        if (!fileExists(projectDir_ + "/" + name_ + ".ocproject")) {
            AVER_ERROR("fabric: --scripts-only needs a project converted before: {} has no {}.ocproject", projectDir_, name_);
            return 1;
        }
        report_.readJsonl(jsonlPath);   // the earlier run's entries; the script step replaces its own
    }

    for (const gd::GodotDiag& x : diag.items)
        if (x.severity != gd::GodotSeverity::Info)
            report_.add("file", x.file, "", "", "", x.severity == gd::GodotSeverity::Error ? Rating::Unsupported : Rating::Approximated, x.message, x.line);
    for (const gd::GodotDiag& x : pdiag.items)
        if (x.severity != gd::GodotSeverity::Info)
            report_.add("project", "res://project.godot", "", "", "", x.severity == gd::GodotSeverity::Error ? Rating::Unsupported : Rating::Approximated, x.message, x.line);
    if (project_.engineVersion.empty()) {
        report_.add("project", "res://project.godot", "", "", "config/features", Rating::Approximated, "the project does not say which Godot version made it; 4.1 or later is assumed", 0);
    }

    createDirectories(contentDir_);
    createDirectories(fabricDir_);
    AVER_INFO("fabric: {} -> {}", godotDir_, options_.dryRun ? requestedOut + " (report only)" : projectDir_);

    registerScripts();
    bool ok = true;
    if (options_.scriptsOnly) {
        if (!fileExists(fabricDir_ + "/scripts.json")) writeScriptsManifest();
    } else {
        convertExtensions();
        ok = convertScenes();
        convertStandaloneResources();
        reportProjectFiles();
        convertInput();
        convertLocalization();
        ok = convertProject(startLevelAsset_) && ok;
        if (options_.scenesOnly)
            for (const auto& [path, si] : scripts_)
                report_.add("script", path, "", si.kind, "", Rating::Unsupported,
                            "script conversion was skipped (--scenes-only); its class is " + si.className + " (extends " + (si.extends.empty() ? "?" : si.extends) +
                                "), recorded in Fabric/scripts.json" + (si.uses.empty() ? "; no scene uses it" : ""),
                            0);
        writeScriptsManifest();
        writeProjectManifest();
        writeShadersManifest();   // G6 SH2
    }

    const std::string reportTitle = project_.name.empty() ? name_ : project_.name;
    if (!report_.writeMarkdown(mdPath, reportTitle) || !report_.writeJsonl(jsonlPath)) {
        AVER_ERROR("fabric: could not write the report to {}", outDir_);
        ok = false;
    }

    // Scripts last: the tool merges its entries into the report just written, and the final FabricReport.md is made from the merged file.
    if (!options_.scenesOnly) {
        ok = convertScripts() && ok;
        if (!report_.writeMarkdown(mdPath, reportTitle) || !report_.writeJsonl(jsonlPath)) {
            AVER_ERROR("fabric: could not write the report to {}", outDir_);
            ok = false;
        }
    }

    std::string reportOut = outDir_;
    if (options_.dryRun) {
        std::error_code ec;
        createDirectories(requestedOut);
        std::filesystem::copy_file(mdPath, requestedOut + "/FabricReport.md", std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::copy_file(jsonlPath, requestedOut + "/FabricReport.jsonl", std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove_all(outDir_, ec);
        reportOut = requestedOut;
    }
    AVER_INFO("fabric: {} converted, {} approximated, {} unsupported ({} level(s), {} prefab(s), {} script(s), {} GDExtension(s))", report_.count(Rating::Converted),
              report_.count(Rating::Approximated), report_.count(Rating::Unsupported), sceneLevels_.size(), prefabs_.size(), scripts_.size(), extensions_.size());
    std::printf("{\"schemaVersion\":1,\"summary\":true,\"kind\":\"fabric\",\"input\":%s,\"output\":%s,\"project\":%s,\"levels\":%zu,\"converted\":%zu,"
                "\"approximated\":%zu,\"unsupported\":%zu,\"scripts\":%zu,\"scriptsConverted\":%s,\"extensions\":%zu,\"report\":%s,\"dryRun\":%s,\"exitCode\":%d}\n",
                jsonString(godotDir_).c_str(), jsonString(options_.dryRun ? reportOut : projectDir_).c_str(), jsonString(name_).c_str(), sceneLevels_.size(),
                report_.count(Rating::Converted), report_.count(Rating::Approximated), report_.count(Rating::Unsupported), scripts_.size(),
                scriptsRan_ ? "true" : "false", extensions_.size(), jsonString(reportOut + "/FabricReport.md").c_str(), options_.dryRun ? "true" : "false", ok ? 0 : 1);
    std::fflush(stdout);
    return ok ? 0 : 1;
}

namespace {
constexpr const char* kFabricUsage =
    "usage: AverAssetC fabric <godot-project-dir> <out-dir> [--dry-run] [--scenes-only] [--scripts-only] [--overwrite]\n"
    "  converts a Godot 4.1+ project into <out-dir>/<Name>/ (an Aver project) and writes <out-dir>/FabricReport.md and .jsonl\n"
    "  --dry-run      convert into a scratch folder, keep only the report\n"
    "  --scenes-only  scenes, assets, project settings and GDExtension files; do not convert scripts\n"
    "  --scripts-only convert only the scripts, into a project converted before\n"
    "  --overwrite    replace a project already converted into <out-dir>\n"
    "exit: 0 converted (the report lists what was approximated or unsupported), 1 the conversion failed, 2 bad usage";
}

int run(int argc, char** argv, const Host& host) {
    Options opt;
    std::vector<std::string> positional;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dry-run") opt.dryRun = true;
        else if (a == "--scenes-only") opt.scenesOnly = true;
        else if (a == "--scripts-only") opt.scriptsOnly = true;
        else if (a == "--overwrite") opt.overwrite = true;
        else if (a.rfind("--", 0) == 0) {
            AVER_ERROR("fabric: unknown option '{}'\n{}", a, kFabricUsage);
            return exitCode(ExitCode::Usage);
        } else {
            positional.push_back(a);
        }
    }
    if (positional.size() != 2 || (opt.scenesOnly && opt.scriptsOnly)) {
        AVER_ERROR("{}", kFabricUsage);
        return exitCode(ExitCode::Usage);
    }
    std::string in = positional[0], out = positional[1];
    while (in.size() > 1 && (in.back() == '/' || in.back() == '\\')) in.pop_back();
    while (out.size() > 1 && (out.back() == '/' || out.back() == '\\')) out.pop_back();
    Converter c(in, out, host);
    c.options_ = opt;
    return c.run() == 0 ? exitCode(ExitCode::Ok) : exitCode(ExitCode::Failed);
}

}  // namespace aver::fabric
