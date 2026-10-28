// File > Import Godot Project...: see FabricImport.hpp.
#include "FabricImport.hpp"

#include "EditorNotifications.hpp"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"
#include "fabric/FabricProcess.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <sstream>

#if AVER_WITH_IMGUI
#include "imgui.h"
#endif

namespace aver::editor {

namespace {

namespace fsys = std::filesystem;

// "key":"value" of a one-line JSON object, with the escapes the converter writes decoded.
std::string jsonText(const std::string& line, const char* key) {
    const std::string needle = std::string("\"") + key + "\":\"";
    size_t at = line.find(needle);
    if (at == std::string::npos) return {};
    std::string out;
    for (at += needle.size(); at < line.size() && line[at] != '"'; ++at) {
        if (line[at] != '\\' || at + 1 >= line.size()) { out.push_back(line[at]); continue; }
        const char e = line[++at];
        switch (e) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            default: out.push_back(e);
        }
    }
    return out;
}

long jsonInt(const std::string& line, const char* key, long def = 0) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t at = line.find(needle);
    return at == std::string::npos ? def : std::strtol(line.c_str() + at + needle.size(), nullptr, 10);
}

bool jsonBool(const std::string& line, const char* key) {
    return line.find(std::string("\"") + key + "\":true") != std::string::npos;
}

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

}  // namespace

bool isGodotProjectDir(const std::string& dir, std::string* why) {
    const auto fail = [&](const std::string& m) { if (why) *why = m; return false; };
    if (dir.empty()) return fail("Choose the folder of a Godot project.");
    std::error_code ec;
    const fsys::path file = fsys::path(dir) / "project.godot";
    if (!fsys::exists(file, ec)) return fail("There is no project.godot in " + dir + ".");
    std::string text;
    if (!readFileText(file.string(), text)) return fail("project.godot cannot be read.");
    const size_t at = text.find("config_version=");
    if (at != std::string::npos) {
        const long version = std::strtol(text.c_str() + at + 15, nullptr, 10);
        if (version < 5) return fail("This is a Godot 3 project (config_version " + std::to_string(version) + "). Fabric converts Godot 4.1 and later.");
    }
    return true;
}

std::string godotProjectDirFrom(const std::string& picked) {
    if (picked.empty()) return {};
    std::error_code ec;
    fsys::path p(picked);
    if (!fsys::is_directory(p, ec)) p = p.parent_path();
    for (; !p.empty(); p = p.parent_path()) {
        if (fsys::exists(p / "project.godot", ec)) return p.string();
        if (p == p.parent_path()) break;
    }
    return {};
}

std::string defaultFabricOutDir(const std::string& godotDir) {
    fsys::path p(godotDir);
    while (p.has_relative_path() && p.filename().empty()) p = p.parent_path();
    const std::string folder = p.filename().string();
    return (p.parent_path() / ((folder.empty() ? std::string("GodotProject") : folder) + "_Aver")).string();
}

std::string fabricConverterPath() {
    const std::string path = executableDir() + AVER_PATH_SEP "AverAssetC" + kExecutableSuffix;
    return fileExists(path) ? path : std::string();
}

std::vector<std::string> fabricConverterArgs(const FabricImportRequest& r) {
    std::vector<std::string> args = {"fabric", r.godotDir, r.outDir};
    if (r.overwrite) args.push_back("--overwrite");
    if (r.dryRun) args.push_back("--dry-run");
    if (r.scenesOnly) args.push_back("--scenes-only");
    return args;
}

FabricImportResult parseFabricOutput(const std::string& output, int exitCode) {
    FabricImportResult r;
    r.started = true;
    r.exitCode = exitCode;
    r.log = output;
    std::istringstream in(output);
    std::string line, lastError;
    while (std::getline(in, line)) {
        line = trimmed(line);
        if (line.rfind("[ERROR]", 0) == 0) lastError = line.size() > 8 ? line.substr(8) : line;
        if (line.rfind("{\"schemaVersion\":1,\"summary\":true,\"kind\":\"fabric\"", 0) != 0) continue;
        r.haveSummary = true;
        r.projectDir = jsonText(line, "output");
        r.projectName = jsonText(line, "project");
        r.reportPath = jsonText(line, "report");
        r.levels = static_cast<int>(jsonInt(line, "levels"));
        r.converted = static_cast<int>(jsonInt(line, "converted"));
        r.approximated = static_cast<int>(jsonInt(line, "approximated"));
        r.unsupported = static_cast<int>(jsonInt(line, "unsupported"));
        r.scripts = static_cast<int>(jsonInt(line, "scripts"));
        r.extensions = static_cast<int>(jsonInt(line, "extensions"));
        r.scriptsConverted = jsonBool(line, "scriptsConverted");
        r.dryRun = jsonBool(line, "dryRun");
    }
    if (r.haveSummary && !r.dryRun && !r.projectDir.empty() && !r.projectName.empty()) r.manifestPath = r.projectDir + "/" + r.projectName + ".ocproject";
    if (exitCode != 0) r.error = lastError.empty() ? "The converter exited with code " + std::to_string(exitCode) + "." : lastError;
    else if (!r.haveSummary) r.error = "The converter finished without a summary.";
    return r;
}

FabricImportResult runFabricImport(const FabricImportRequest& request, const std::string& converterPath,
                                   const std::function<void(const std::string&)>& onLine) {
    const fabric::ProcessOutput out = fabric::runProcess(converterPath, fabricConverterArgs(request), onLine);
    if (!out.started) {
        FabricImportResult r;
        r.error = "Could not start " + converterPath + ".";
        return r;
    }
    return parseFabricOutput(out.text, out.exitCode);
}

// ---- the editor half ------------------------------------------------------------------------------------------------------

FabricImporter& fabricImporter() {
    static FabricImporter importer;
    return importer;
}

FabricImporter::~FabricImporter() {
    if (thread_.joinable()) thread_.join();   // the worker writes into job_, which this object keeps alive; joined, not detached
}

bool FabricImporter::running() const { return job_ && !job_->done.load(); }

bool FabricImporter::start(const FabricImportRequest& request, std::string* why) {
    if (running()) { if (why) *why = "An import is already running."; return false; }
    if (thread_.joinable()) thread_.join();
    const std::string converter = fabricConverterPath();
    if (converter.empty()) {
        if (why) *why = "AverAssetC is not beside the editor; build the AverAssetC target.";
        return false;
    }
    auto job = std::make_shared<Job>();
    job_ = job;
    haveLast_ = false;
    {
        Notification n;
        n.severity = NotifySeverity::Info;
        n.title = "Importing Godot project";
        n.body = request.godotDir;
        n.hasProgress = true;   // no expiry while it runs; a conversion reports no percentage
        n.progress = -1.0f;
        job->notification = notifications().push(std::move(n));
    }
    AVER_INFO("[Fabric] importing {} into {}", request.godotDir, request.outDir);
    thread_ = std::thread([job, request, converter] {
        job->result = runFabricImport(request, converter, [&](const std::string& line) {
            if (line.empty() || line.rfind("{\"schemaVersion\"", 0) == 0) return;   // the machine-readable lines are not for people
            AVER_INFO("[Fabric] {}", line);
            std::lock_guard<std::mutex> g(job->lock);
            job->lines.push_back(line);
            if (job->lines.size() > 400) job->lines.erase(job->lines.begin(), job->lines.begin() + 100);
        });
        job->done.store(true);   // last: the UI thread reads the result once this is set
    });
    return true;
}

void FabricImporter::reap() {
    if (!job_ || !job_->done.load() || haveLast_) return;
    if (thread_.joinable()) thread_.join();
    last_ = job_->result;
    haveLast_ = true;
    const FabricImportResult& r = last_;
    if (r.ok()) {
        const std::string body = std::to_string(r.converted) + " converted, " + std::to_string(r.approximated) + " approximated, " +
                                 std::to_string(r.unsupported) + " unsupported" + (r.dryRun ? " (report only)" : "") + ". Report: " + r.reportPath;
        notifications().finish(job_->notification, r.unsupported > 0 ? NotifySeverity::Warning : NotifySeverity::Success,
                               r.dryRun ? "Godot project checked" : "Godot project imported", body, 15.0);
        AVER_INFO("[Fabric] {}", body);
    } else {
        notifications().finish(job_->notification, NotifySeverity::Error, "Godot import failed", r.error, 20.0);
        AVER_ERROR("[Fabric] import failed: {}", r.error);
    }
}

std::string FabricImporter::takeOpenRequest() {
    std::string s;
    s.swap(openRequest_);
    return s;
}

#if AVER_WITH_IMGUI

namespace {
void setBuf(char* buf, usize size, const std::string& s) { std::snprintf(buf, size, "%s", s.c_str()); }
}  // namespace

void FabricImporter::drawModal(f32 dpi) {
    reap();
    if (wantOpen_) {
        if (!godotDirBuf_[0] && !haveLast_) { formError_.clear(); }
        ImGui::OpenPopup("Import Godot Project");
        wantOpen_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(780.0f * dpi, 540.0f * dpi), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Import Godot Project", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;

    const bool busy = running();
    ImGui::TextWrapped("Converts a Godot 4.1+ project (3D scenes, assets, scripts and GDExtensions) into a standalone Aver project. "
                       "The Godot project is only read. What did not convert is listed in FabricReport.md.");
    ImGui::Spacing();

    ImGui::BeginDisabled(busy);
    ImGui::TextUnformatted("Godot project");
    ImGui::PushItemWidth(-(110.0f * dpi));
    if (ImGui::InputText("##fabricGodot", godotDirBuf_, sizeof godotDirBuf_) && !outEdited_ && godotDirBuf_[0])
        setBuf(outDirBuf_, sizeof outDirBuf_, defaultFabricOutDir(godotDirBuf_));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Browse...##fabricGodot", ImVec2(100.0f * dpi, 0))) {
        std::string picked;
        if (openFileDialog("Pick the project.godot of the Godot project", "Godot project (project.godot)", "project.godot", std::string(godotDirBuf_), picked)) {
            const std::string dir = godotProjectDirFrom(picked);
            setBuf(godotDirBuf_, sizeof godotDirBuf_, dir.empty() ? fsys::path(picked).parent_path().string() : dir);
            if (!outEdited_) setBuf(outDirBuf_, sizeof outDirBuf_, defaultFabricOutDir(godotDirBuf_));
        }
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Output folder (the project is written to <folder>/<Name>)");
    ImGui::PushItemWidth(-(110.0f * dpi));
    if (ImGui::InputText("##fabricOut", outDirBuf_, sizeof outDirBuf_)) outEdited_ = true;
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Browse...##fabricOut", ImVec2(100.0f * dpi, 0))) {
        std::string picked;
        if (openFileDialog("Pick any file in the output folder", "All files (*.*)", "*.*", std::string(outDirBuf_), picked)) {
            setBuf(outDirBuf_, sizeof outDirBuf_, fsys::path(picked).parent_path().string());
            outEdited_ = true;
        }
    }
    ImGui::Checkbox("Replace a project already converted there", &overwrite_);
    ImGui::SameLine(0, 24.0f * dpi);
    ImGui::Checkbox("Report only (convert nothing to disk)", &dryRun_);
    ImGui::EndDisabled();

    if (!formError_.empty()) ImGui::TextColored(ImVec4(0.93f, 0.42f, 0.38f, 1), "%s", formError_.c_str());
    ImGui::Spacing();

    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Import", ImVec2(140.0f * dpi, 0))) {
        formError_.clear();
        std::string why;
        if (!isGodotProjectDir(godotDirBuf_, &why)) {
            formError_ = why;
        } else if (!outDirBuf_[0]) {
            formError_ = "Choose an output folder.";
        } else {
            FabricImportRequest req;
            req.godotDir = godotDirBuf_;
            req.outDir = outDirBuf_;
            req.overwrite = overwrite_;
            req.dryRun = dryRun_;
            if (!start(req, &why)) formError_ = why;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(100.0f * dpi, 0))) ImGui::CloseCurrentPopup();   // a running import carries on; its notification reports the end
    if (busy) {
        ImGui::SameLine();
        ImGui::TextDisabled("Converting...");
    }

    // the log
    ImGui::Separator();
    if (job_) {
        std::lock_guard<std::mutex> g(job_->lock);
        shownLog_ = job_->lines;
    }
    if (ImGui::BeginChild("##fabricLog", ImVec2(0, haveLast_ ? -(80.0f * dpi) : 0), ImGuiChildFlags_Borders)) {
        for (const std::string& line : shownLog_) ImGui::TextUnformatted(line.c_str());
        if (busy) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    if (haveLast_) {
        if (last_.ok()) {
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "%d converted, %d approximated, %d unsupported; %d level(s), %d script(s), %d GDExtension(s).",
                               last_.converted, last_.approximated, last_.unsupported, last_.levels, last_.scripts, last_.extensions);
            if (!last_.scriptsConverted && last_.scripts > 0) ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.25f, 1), "The scripts were not converted; see the report.");
            ImGui::BeginDisabled(last_.manifestPath.empty());
            if (ImGui::Button("Open converted project", ImVec2(200.0f * dpi, 0))) {
                openRequest_ = last_.manifestPath;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Copy report path", ImVec2(160.0f * dpi, 0))) ImGui::SetClipboardText(last_.reportPath.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", last_.reportPath.c_str());
        } else {
            ImGui::TextColored(ImVec4(0.93f, 0.42f, 0.38f, 1), "%s", last_.error.c_str());
        }
    }
    ImGui::EndPopup();
}

#else

void FabricImporter::drawModal(f32) { reap(); }

#endif

}  // namespace aver::editor
