// File menu import entries that extensions add: see ExtensionImport.hpp.
#include "ExtensionImport.hpp"

#include "EditorNotifications.hpp"
#include "ProcessStream.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/Json.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>

#if AVER_WITH_IMGUI
#include "imgui.h"
#endif

namespace aver::editor {

namespace {

namespace fsys = std::filesystem;

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

std::string orDefault(const std::string& value, const char* fallback) { return value.empty() ? std::string(fallback) : value; }

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
    return s;
}

// A number as the summary prints it: whole numbers without a fraction.
std::string numberText(double v) {
    char buf[40];
    if (v == static_cast<double>(static_cast<long long>(v))) std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
    else std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

}  // namespace

std::string ImportResult::value(const std::string& key) const {
    const auto it = values.find(key);
    return it == values.end() ? std::string() : it->second;
}

bool ImportResult::truthy(const std::string& key) const {
    const std::string v = value(key);
    if (v.empty() || v == "false") return false;
    if (v == "true") return true;
    return std::strtod(v.c_str(), nullptr) != 0.0;
}

ImporterText importerText(const ext::ImporterInfo& info, const std::string& extensionName) {
    ImporterText t;
    std::string menu = info.menu;
    while (!menu.empty() && menu.back() == '.') menu.pop_back();
    t.title = info.title.empty() ? menu : info.title;
    t.tooltip = info.tooltip.empty() ? info.description : info.tooltip;
    t.description = info.description;
    t.inputLabel = orDefault(info.inputLabel, "Input");
    t.outputLabel = orDefault(info.outputLabel, "Output folder");
    t.outputSuffix = orDefault(info.outputSuffix, "_Imported");
    t.openLabel = orDefault(info.openLabel, "Open result");
    t.busyText = orDefault(info.busyText, "Working...");
    t.logTag = info.logTag.empty() ? extensionName : info.logTag;
    t.running = orDefault(info.notifyRunning, "Importing");
    t.done = orDefault(info.notifyDone, "Import finished");
    t.checked = orDefault(info.notifyChecked, "Import checked");
    t.failed = orDefault(info.notifyFailed, "Import failed");
    return t;
}

bool importInputValid(const ext::ImporterInfo& info, const std::string& dir, std::string* why) {
    const auto fail = [&](const std::string& m) { if (why) *why = m; return false; };
    const ImporterText text = importerText(info, {});
    std::error_code ec;
    if (info.input == "file") {
        if (dir.empty()) return fail("Choose the file of a " + text.inputLabel + ".");
        if (!fsys::is_regular_file(fsys::path(dir), ec)) return fail("There is no file " + dir + ".");
        return true;
    }
    if (dir.empty()) return fail("Choose the folder of a " + text.inputLabel + ".");
    if (info.detect.empty()) {
        if (!fsys::is_directory(fsys::path(dir), ec)) return fail("There is no folder " + dir + ".");
        return true;
    }
    const fsys::path file = fsys::path(dir) / info.detect;
    if (!fsys::exists(file, ec)) return fail("There is no " + info.detect + " in " + dir + ".");
    if (info.detectKey.empty()) return true;
    std::string body;
    if (!readFileText(file.string(), body)) return fail(info.detect + " cannot be read.");
    const size_t at = body.find(info.detectKey);
    if (at != std::string::npos) {
        const long version = std::strtol(body.c_str() + at + info.detectKey.size(), nullptr, 10);
        if (version < info.detectMin) {
            std::string message = info.detectMessage.empty() ? info.detect + " is too old (" + info.detectKey + "{value})." : info.detectMessage;
            return fail(replaceAll(message, "{value}", std::to_string(version)));
        }
    }
    return true;
}

std::string importInputFrom(const ext::ImporterInfo& info, const std::string& picked) {
    if (picked.empty()) return {};
    std::error_code ec;
    fsys::path p(picked);
    if (info.input == "file") return picked;
    if (!fsys::is_directory(p, ec)) p = p.parent_path();
    if (info.detect.empty()) return p.string();
    for (; !p.empty(); p = p.parent_path()) {
        if (fsys::exists(p / info.detect, ec)) return p.string();
        if (p == p.parent_path()) break;
    }
    return {};
}

std::string importDefaultOutDir(const ext::ImporterInfo& info, const std::string& inDir) {
    fsys::path p(inDir);
    while (p.has_relative_path() && p.filename().empty()) p = p.parent_path();
    std::string folder = p.filename().string();
    if (info.input == "file") folder = p.stem().string();
    if (folder.empty()) {
        for (const char c : importerText(info, {}).inputLabel) if (c != ' ') folder.push_back(c);
    }
    return (p.parent_path() / (folder + importerText(info, {}).outputSuffix)).string();
}

std::string importToolPath() {
    const std::string path = executableDir() + AVER_PATH_SEP "AverAssetC" + kExecutableSuffix;
    return fileExists(path) ? path : std::string();
}

std::vector<std::string> importArgs(const ImportRequest& r) {
    std::vector<std::string> args = {r.command, r.inDir, r.outDir};
    args.insert(args.end(), r.flags.begin(), r.flags.end());
    return args;
}

ImportResult parseImportOutput(const std::string& command, const std::string& output, int exitCode) {
    ImportResult r;
    r.started = true;
    r.exitCode = exitCode;
    r.log = output;
    const std::string prefix = "{\"schemaVersion\":1,\"summary\":true,\"kind\":\"" + command + "\"";
    std::istringstream in(output);
    std::string line, lastError;
    while (std::getline(in, line)) {
        line = trimmed(line);
        if (line.rfind("[ERROR]", 0) == 0) lastError = line.size() > 8 ? line.substr(8) : line;
        if (line.rfind(prefix, 0) != 0) continue;
        fmt::JsonValue json;
        if (!fmt::parseJson(line, json) || !json.isObject()) continue;
        r.haveSummary = true;
        r.values.clear();
        for (const fmt::JsonMember& m : json.members()) {
            if (m.value.isString()) r.values[m.key] = std::string(m.value.asString());
            else if (m.value.isBool()) r.values[m.key] = m.value.asBool() ? "true" : "false";
            else if (m.value.isNumber()) r.values[m.key] = numberText(m.value.asDouble());
        }
        r.projectDir = r.value("output");
        r.projectName = r.value("project");
        r.reportPath = r.value("report");
        r.dryRun = r.truthy("dryRun");
    }
    if (r.haveSummary && !r.dryRun && !r.projectDir.empty() && !r.projectName.empty()) r.manifestPath = r.projectDir + "/" + r.projectName + ".ocproject";
    if (exitCode != 0) r.error = lastError.empty() ? "The converter exited with code " + std::to_string(exitCode) + "." : lastError;
    else if (!r.haveSummary) r.error = "The converter finished without a summary.";
    return r;
}

std::string expandSummary(const std::string& text, const ImportResult& result) {
    std::string out;
    for (size_t i = 0; i < text.size();) {
        const size_t close = text[i] == '{' ? text.find('}', i) : std::string::npos;
        if (close == std::string::npos) { out.push_back(text[i++]); continue; }
        out += result.value(text.substr(i + 1, close - i - 1));
        i = close + 1;
    }
    return out;
}

std::vector<std::string> summaryNotes(const ext::ImporterInfo& info, const ImportResult& result) {
    std::vector<std::string> notes;
    for (const ext::ImporterInfo::Note& n : info.notes) {
        if (!n.when.empty() && !result.truthy(n.when)) continue;
        if (!n.unless.empty() && result.truthy(n.unless)) continue;
        notes.push_back(expandSummary(n.text, result));
    }
    return notes;
}

ImportResult runImport(const ImportRequest& request, const std::string& toolPath, const std::function<void(const std::string&)>& onLine) {
    const ProcessOutput out = runProcess(toolPath, importArgs(request), onLine);
    if (!out.started) {
        ImportResult r;
        r.error = "Could not start " + toolPath + ".";
        return r;
    }
    return parseImportOutput(request.command, out.text, out.exitCode);
}

// ---- the editor half ------------------------------------------------------------------------------------------------------

ExtensionImporter& extensionImporter() {
    static ExtensionImporter importer;
    return importer;
}

ExtensionImporter::~ExtensionImporter() {
    if (thread_.joinable()) thread_.join();   // the worker writes into job_, which this object keeps alive; joined, not detached
}

void ExtensionImporter::scan(const std::string& root) {
    scanned_ = true;
    entries_.clear();
    forms_.clear();
    ext::Registry registry;
    if (root.empty()) registry.scanDefault(); else registry.scan(root);
    for (const ext::Extension& e : registry.extensions()) {
        if (!e.ready()) continue;
        for (const ext::ImporterInfo& i : e.importers) {
            if (i.menu.empty() || i.command.empty()) continue;
            ImporterEntry entry;
            entry.extension = e.displayName;
            entry.info = i;
            entry.text = importerText(i, e.displayName);
            entries_.push_back(std::move(entry));
        }
    }
    forms_.resize(entries_.size());
    for (usize k = 0; k < entries_.size(); ++k) {
        forms_[k].optionOn.reserve(entries_[k].info.options.size());
        for (const ext::ImporterInfo::Option& o : entries_[k].info.options) forms_[k].optionOn.push_back(o.onByDefault ? 1 : 0);
    }
}

const std::vector<ImporterEntry>& ExtensionImporter::entries() {
    if (!scanned_) scan();
    return entries_;
}

void ExtensionImporter::open(usize index) { if (index < entries().size()) wantOpen_ = index; }

bool ExtensionImporter::running() const { return job_ && !job_->done.load(); }

bool ExtensionImporter::start(usize index, const ImportRequest& request, std::string* why) {
    if (index >= entries().size()) { if (why) *why = "That import is no longer installed."; return false; }
    if (running()) { if (why) *why = "An import is already running."; return false; }
    if (thread_.joinable()) thread_.join();
    const std::string tool = toolPath();
    if (tool.empty()) {
        if (why) *why = "AverAssetC is not beside the editor; build the AverAssetC target.";
        return false;
    }
    auto job = std::make_shared<Job>();
    job->entry = entries_[index];
    job_ = job;
    lastEntry_ = index;
    haveLast_ = false;
    const std::string tag = job->entry.text.logTag;
    {
        Notification n;
        n.severity = NotifySeverity::Info;
        n.title = job->entry.text.running;
        n.body = request.inDir;
        n.hasProgress = true;   // no expiry while it runs; a command reports no percentage
        n.progress = -1.0f;
        job->notification = notifications().push(std::move(n));
    }
    AVER_INFO("[{}] importing {} into {}", tag, request.inDir, request.outDir);
    thread_ = std::thread([job, request, tool, tag] {
        job->result = runImport(request, tool, [&](const std::string& line) {
            if (line.empty() || line.rfind("{\"schemaVersion\"", 0) == 0) return;   // the machine-readable lines are not for people
            AVER_INFO("[{}] {}", tag, line);
            std::lock_guard<std::mutex> g(job->lock);
            job->lines.push_back(line);
            if (job->lines.size() > 400) job->lines.erase(job->lines.begin(), job->lines.begin() + 100);
        });
        job->done.store(true);   // last: the UI thread reads the result once this is set
    });
    return true;
}

void ExtensionImporter::reap() {
    if (!job_ || !job_->done.load() || haveLast_) return;
    if (thread_.joinable()) thread_.join();
    last_ = job_->result;
    haveLast_ = true;
    const ImportResult& r = last_;
    const ImporterEntry& entry = job_->entry;
    if (r.ok()) {
        std::string body = expandSummary(entry.info.summaryBrief, r);
        if (r.dryRun) body += " (report only)";
        if (!r.reportPath.empty()) body += (body.empty() ? "Report: " : ". Report: ") + r.reportPath;
        const bool warn = !entry.info.summaryWarnKey.empty() && r.truthy(entry.info.summaryWarnKey);
        notifications().finish(job_->notification, warn ? NotifySeverity::Warning : NotifySeverity::Success,
                               r.dryRun ? entry.text.checked : entry.text.done, body, 15.0);
        AVER_INFO("[{}] {}", entry.text.logTag, body);
    } else {
        notifications().finish(job_->notification, NotifySeverity::Error, entry.text.failed, r.error, 20.0);
        AVER_ERROR("[{}] import failed: {}", entry.text.logTag, r.error);
    }
}

std::string ExtensionImporter::takeOpenRequest() {
    std::string s;
    s.swap(openRequest_);
    return s;
}

#if AVER_WITH_IMGUI

namespace {
void setBuf(char* buf, usize size, const std::string& s) { std::snprintf(buf, size, "%s", s.c_str()); }
}  // namespace

void ExtensionImporter::drawMenuItems(const std::function<void(const char* id)>& track) {
    const std::vector<ImporterEntry>& list = entries();
    const bool haveTool = !toolPath().empty();
    for (usize i = 0; i < list.size(); ++i) {
        const ImporterEntry& en = list[i];
        ImGui::BeginDisabled(!haveTool);
        if (ImGui::MenuItem(en.info.menu.c_str())) open(i);
        ImGui::EndDisabled();
        if (track) track(("file.import." + en.info.command).c_str());
        if (!haveTool && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("AverAssetC is not beside the editor - build the AverAssetC target.");
        else if (ImGui::IsItemHovered() && !en.text.tooltip.empty())
            ImGui::SetTooltip("%s", en.text.tooltip.c_str());
    }
}

void ExtensionImporter::drawModal(f32 dpi) {
    reap();
    if (wantOpen_ != static_cast<usize>(-1)) {
        if (wantOpen_ < entries_.size()) {
            active_ = wantOpen_;
            if (!forms_[active_].inBuf[0] && !(haveLast_ && lastEntry_ == active_)) forms_[active_].error.clear();
            ImGui::OpenPopup("###extensionImport");
        }
        wantOpen_ = static_cast<usize>(-1);
    }
    if (active_ >= entries_.size()) return;
    const ImporterEntry& entry = entries_[active_];
    const ext::ImporterInfo& info = entry.info;
    const ImporterText& text = entry.text;
    Form& form = forms_[active_];

    ImGui::SetNextWindowSize(ImVec2(780.0f * dpi, 540.0f * dpi), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal((text.title + "###extensionImport").c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) return;

    const bool busy = running();
    const bool showLast = haveLast_ && lastEntry_ == active_;
    if (!text.description.empty()) {
        ImGui::TextWrapped("%s", text.description.c_str());
        ImGui::Spacing();
    }

    ImGui::BeginDisabled(busy);
    ImGui::TextUnformatted(text.inputLabel.c_str());
    ImGui::PushItemWidth(-(110.0f * dpi));
    if (ImGui::InputText("##importIn", form.inBuf, sizeof form.inBuf) && !form.outEdited && form.inBuf[0])
        setBuf(form.outBuf, sizeof form.outBuf, importDefaultOutDir(info, form.inBuf));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Browse...##importIn", ImVec2(100.0f * dpi, 0))) {
        std::string picked;
        const bool isFile = info.input == "file";
        const std::string title = info.detect.empty() ? (isFile ? "Pick the " + text.inputLabel : "Pick any file in the " + text.inputLabel + " folder")
                                                      : "Pick the " + info.detect + " of the " + text.inputLabel;
        const std::string filter = info.detect.empty() ? "All files (*.*)" : text.inputLabel + " (" + info.detect + ")";
        const std::string spec = info.detect.empty() ? "*.*" : info.detect;
        if (openFileDialog(title, filter, spec, std::string(form.inBuf), picked)) {
            const std::string dir = importInputFrom(info, picked);
            setBuf(form.inBuf, sizeof form.inBuf, dir.empty() ? fsys::path(picked).parent_path().string() : dir);
            if (!form.outEdited) setBuf(form.outBuf, sizeof form.outBuf, importDefaultOutDir(info, form.inBuf));
        }
    }

    ImGui::Spacing();
    ImGui::TextUnformatted(text.outputLabel.c_str());
    ImGui::PushItemWidth(-(110.0f * dpi));
    if (ImGui::InputText("##importOut", form.outBuf, sizeof form.outBuf)) form.outEdited = true;
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Browse...##importOut", ImVec2(100.0f * dpi, 0))) {
        std::string picked;
        if (openFileDialog("Pick any file in the output folder", "All files (*.*)", "*.*", std::string(form.outBuf), picked)) {
            setBuf(form.outBuf, sizeof form.outBuf, fsys::path(picked).parent_path().string());
            form.outEdited = true;
        }
    }
    for (usize k = 0; k < info.options.size(); ++k) {
        if (k > 0) ImGui::SameLine(0, 24.0f * dpi);
        bool on = form.optionOn[k] != 0;
        if (ImGui::Checkbox((info.options[k].label + "##importOpt" + std::to_string(k)).c_str(), &on)) form.optionOn[k] = on ? 1 : 0;
    }
    ImGui::EndDisabled();

    if (!form.error.empty()) ImGui::TextColored(ImVec4(0.93f, 0.42f, 0.38f, 1), "%s", form.error.c_str());
    ImGui::Spacing();

    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Import", ImVec2(140.0f * dpi, 0))) {
        form.error.clear();
        std::string why;
        if (!importInputValid(info, form.inBuf, &why)) {
            form.error = why;
        } else if (!form.outBuf[0]) {
            form.error = "Choose an output folder.";
        } else {
            ImportRequest req;
            req.command = info.command;
            req.inDir = form.inBuf;
            req.outDir = form.outBuf;
            for (usize k = 0; k < info.options.size(); ++k)
                if (form.optionOn[k]) req.flags.push_back(info.options[k].flag);
            if (!start(active_, req, &why)) form.error = why;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(100.0f * dpi, 0))) ImGui::CloseCurrentPopup();   // a running import carries on; its notification reports the end
    if (busy) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", text.busyText.c_str());
    }

    // the log
    ImGui::Separator();
    if (job_ && lastEntry_ == active_) {
        std::lock_guard<std::mutex> g(job_->lock);
        shownLog_ = job_->lines;
    } else {
        shownLog_.clear();
    }
    if (ImGui::BeginChild("##importLog", ImVec2(0, showLast ? -(80.0f * dpi) : 0), ImGuiChildFlags_Borders)) {
        for (const std::string& line : shownLog_) ImGui::TextUnformatted(line.c_str());
        if (busy) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    if (showLast) {
        if (last_.ok()) {
            const std::string summary = expandSummary(info.summaryText, last_);
            if (!summary.empty()) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "%s", summary.c_str());
            for (const std::string& note : summaryNotes(info, last_)) ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.25f, 1), "%s", note.c_str());
            if (info.opensResult) {
                ImGui::BeginDisabled(last_.manifestPath.empty());
                if (ImGui::Button(text.openLabel.c_str(), ImVec2(200.0f * dpi, 0))) {
                    openRequest_ = last_.manifestPath;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
            }
            if (!last_.reportPath.empty()) {
                if (ImGui::Button("Copy report path", ImVec2(160.0f * dpi, 0))) ImGui::SetClipboardText(last_.reportPath.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("%s", last_.reportPath.c_str());
            }
        } else {
            ImGui::TextColored(ImVec4(0.93f, 0.42f, 0.38f, 1), "%s", last_.error.c_str());
        }
    }
    ImGui::EndPopup();
}

#else

void ExtensionImporter::drawMenuItems(const std::function<void(const char*)>&) {}

void ExtensionImporter::drawModal(f32) { reap(); }

#endif

}  // namespace aver::editor
