// File menu import entries that extensions add: see ExtensionImport.hpp.
#include "ExtensionImport.hpp"

#include "EditorNotifications.hpp"
#include "ProcessStream.hpp"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
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

// '*' matches any run of characters; case-insensitive, as a file dialog's patterns are.
bool wildcardMatch(const std::string& pattern, const std::string& text) {
    const auto lower = [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); };
    size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == '*') { star = p++; mark = t; }
        else if (p < pattern.size() && lower(pattern[p]) == lower(text[t])) { ++p; ++t; }
        else if (star != std::string::npos) { p = star + 1; t = ++mark; }
        else return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
    return s;
}

std::string joinPatterns(const std::vector<std::string>& patterns, const char* separator) {
    std::string out;
    for (const std::string& p : patterns) out += (out.empty() ? "" : separator) + p;
    return out;
}

}  // namespace

ImporterText importerText(const ext::ImporterInfo& info) {
    ImporterText t;
    std::string menu = info.menu;
    while (!menu.empty() && menu.back() == '.') menu.pop_back();
    t.title = menu;
    t.tooltip = info.tooltip.empty() ? info.description : info.tooltip;
    t.description = info.description;
    return t;
}

bool importInputValid(const ext::ImporterInfo& info, const std::string& input, std::string* why) {
    const auto fail = [&](const std::string& m) { if (why) *why = m; return false; };
    std::error_code ec;
    if (info.input.kind == "file") {
        if (input.empty()) return fail("Choose the input file.");
        if (!fsys::is_regular_file(fsys::path(input), ec)) return fail("There is no file " + input + ".");
        if (!info.input.patterns.empty()) {
            const std::string leaf = fsys::path(input).filename().string();
            const bool match = std::any_of(info.input.patterns.begin(), info.input.patterns.end(), [&](const std::string& p) { return wildcardMatch(p, leaf); });
            if (!match) return fail(leaf + " is not one of " + joinPatterns(info.input.patterns, ", ") + ".");
        }
        return true;
    }
    if (input.empty()) return fail("Choose the input folder.");
    if (!fsys::is_directory(fsys::path(input), ec)) return fail("There is no folder " + input + ".");
    if (!info.input.detect.empty() && !fsys::exists(fsys::path(input) / info.input.detect, ec)) return fail("There is no " + info.input.detect + " in " + input + ".");
    return true;
}

std::string importInputFrom(const ext::ImporterInfo& info, const std::string& picked) {
    if (picked.empty()) return {};
    std::error_code ec;
    fsys::path p(picked);
    if (info.input.kind == "file") return picked;
    if (!fsys::is_directory(p, ec)) p = p.parent_path();
    if (info.input.detect.empty()) return p.string();
    for (; !p.empty(); p = p.parent_path()) {
        if (fsys::exists(p / info.input.detect, ec)) return p.string();
        if (p == p.parent_path()) break;
    }
    return {};
}

std::string importPickerSpec(const ext::ImporterInfo& info) {
    if (!info.input.patterns.empty()) return joinPatterns(info.input.patterns, ";");
    if (!info.input.detect.empty()) return info.input.detect;
    return "*.*";
}

std::string importDefaultOutDir(const ext::ImporterInfo& info, const std::string& input) {
    fsys::path p(input);
    while (p.has_relative_path() && p.filename().empty()) p = p.parent_path();
    std::string name = info.input.kind == "file" ? p.stem().string() : p.filename().string();
    if (name.empty()) name = "input";
    const std::string pattern = info.outputDefault.empty() ? "{input}_out" : info.outputDefault;
    return (p.parent_path() / replaceAll(pattern, "{input}", name)).string();
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

ImportResult parseImportOutput(const std::string& output, int exitCode) {
    ImportResult r;
    r.started = true;
    r.exitCode = exitCode;
    r.log = output;
    std::istringstream in(output);
    std::string line, lastError;
    while (std::getline(in, line)) {
        line = trimmed(line);
        if (line.rfind("[ERROR]", 0) == 0) lastError = line.size() > 8 ? line.substr(8) : line;
        const ext::CommandMessage m = ext::parseCommandMessage(line);
        if (m.kind == ext::MessageKind::Progress) {
            r.fraction = m.fraction;
            r.progressText = m.text;
        } else if (m.kind == ext::MessageKind::Result) {
            r.haveResult = true;
            r.succeeded = m.ok;
            r.openPath = m.open;
            r.reportPath = m.report;
            r.message = m.message;
            r.warnings = m.warnings;
            r.errors = m.errors;
        }
    }
    if (exitCode != 0) r.error = !r.message.empty() && r.haveResult ? r.message : lastError.empty() ? "The command exited with code " + std::to_string(exitCode) + "." : lastError;
    else if (!r.haveResult) r.error = "The command finished without a result.";
    else if (!r.succeeded) r.error = r.message.empty() ? "The command reported a failure." : r.message;
    return r;
}

ImportResult runImport(const ImportRequest& request, const std::string& toolPath, const std::function<void(const std::string&)>& onLine) {
    const ProcessOutput out = runProcess(toolPath, importArgs(request), onLine);
    if (!out.started) {
        ImportResult r;
        r.error = "Could not start " + toolPath + ".";
        return r;
    }
    return parseImportOutput(out.text, out.exitCode);
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
            ImporterEntry entry;
            entry.extension = e.displayName;
            entry.info = i;
            entry.text = importerText(i);
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
    const std::string tag = job->entry.extension;
    {
        Notification n;
        n.severity = NotifySeverity::Info;
        n.title = job->entry.text.title;
        n.body = request.inDir;
        n.hasProgress = true;   // no expiry while it runs; the command's progress lines move the bar
        n.progress = -1.0f;
        job->notification = notifications().push(std::move(n));
    }
    AVER_INFO("[{}] importing {} into {}", tag, request.inDir, request.outDir);
    thread_ = std::thread([job, request, tool, tag] {
        job->result = runImport(request, tool, [&](const std::string& line) {
            if (line.empty()) return;
            const ext::CommandMessage m = ext::parseCommandMessage(line);
            if (m.kind == ext::MessageKind::Progress) {   // the protocol lines are not for people: they move the bar
                std::lock_guard<std::mutex> g(job->lock);
                job->fraction = m.fraction;
                job->progressText = m.text;
                return;
            }
            if (m.kind == ext::MessageKind::Result) return;
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
        std::string body = r.message;
        if (r.warnings > 0) body += std::string(body.empty() ? "" : ". ") + std::to_string(r.warnings) + " warning(s)";
        if (!r.reportPath.empty()) body += (body.empty() ? "Report: " : ". Report: ") + r.reportPath;
        notifications().finish(job_->notification, r.warnings > 0 ? NotifySeverity::Warning : NotifySeverity::Success, entry.text.title + " finished", body, 15.0);
        AVER_INFO("[{}] {}", entry.extension, body);
    } else {
        notifications().finish(job_->notification, NotifySeverity::Error, entry.text.title + " failed", r.error, 20.0);
        AVER_ERROR("[{}] import failed: {}", entry.extension, r.error);
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

bool isProjectFile(const std::string& path) {
    const std::string ext = fsys::path(path).extension().string();
    return ext.size() == 10 && std::equal(ext.begin(), ext.end(), ".ocproject", [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
}
}  // namespace

void ExtensionImporter::drawMenuItems(const std::function<void(const char* id)>& track) {
    const std::vector<ImporterEntry>& list = entries();
    const bool haveTool = !toolPath().empty();
    for (usize i = 0; i < list.size(); ++i) {
        const ImporterEntry& en = list[i];
        ImGui::BeginDisabled(!haveTool);
        if (ImGui::MenuItem(en.info.menu.c_str())) open(i);
        ImGui::EndDisabled();
        if (track) track(("file.import." + en.info.id).c_str());
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
    const bool isFile = info.input.kind == "file";
    if (!text.description.empty()) {
        ImGui::TextWrapped("%s", text.description.c_str());
        ImGui::Spacing();
    }

    ImGui::BeginDisabled(busy);
    ImGui::TextUnformatted(isFile ? "Input file" : "Input folder");
    ImGui::PushItemWidth(-(110.0f * dpi));
    if (ImGui::InputText("##importIn", form.inBuf, sizeof form.inBuf) && !form.outEdited && form.inBuf[0])
        setBuf(form.outBuf, sizeof form.outBuf, importDefaultOutDir(info, form.inBuf));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Browse...##importIn", ImVec2(100.0f * dpi, 0))) {
        std::string picked;
        const std::string spec = importPickerSpec(info);
        const std::string title = isFile ? "Pick the input file" : info.input.detect.empty() ? "Pick any file in the input folder" : "Pick the " + info.input.detect + " of the input";
        if (openFileDialog(title, "Input (" + spec + ")", spec, std::string(form.inBuf), picked)) {
            const std::string dir = importInputFrom(info, picked);
            setBuf(form.inBuf, sizeof form.inBuf, dir.empty() ? fsys::path(picked).parent_path().string() : dir);
            if (!form.outEdited) setBuf(form.outBuf, sizeof form.outBuf, importDefaultOutDir(info, form.inBuf));
        }
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Output folder");
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

    // the progress bar and the log
    ImGui::Separator();
    double fraction = -1.0;
    std::string progressText;
    if (job_ && lastEntry_ == active_) {
        std::lock_guard<std::mutex> g(job_->lock);
        shownLog_ = job_->lines;
        fraction = job_->fraction;
        progressText = job_->progressText;
    } else {
        shownLog_.clear();
    }
    if (busy) {
        const float width = -1.0f;
        if (fraction >= 0.0) ImGui::ProgressBar(float(fraction), ImVec2(width, 0), progressText.c_str());
        else ImGui::ProgressBar(-1.0f * float(ImGui::GetTime()), ImVec2(width, 0), progressText.c_str());
    }
    if (ImGui::BeginChild("##importLog", ImVec2(0, showLast ? -(80.0f * dpi) : 0), ImGuiChildFlags_Borders)) {
        for (const std::string& line : shownLog_) ImGui::TextUnformatted(line.c_str());
        if (busy) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    if (showLast) {
        if (last_.ok()) {
            if (!last_.message.empty()) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1), "%s", last_.message.c_str());
            if (last_.warnings > 0 || last_.errors > 0)
                ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.25f, 1), "%d warning(s), %d error(s)", last_.warnings, last_.errors);
            if (!last_.openPath.empty()) {
                const bool openable = isProjectFile(last_.openPath);
                ImGui::BeginDisabled(!openable);
                if (ImGui::Button("Open result", ImVec2(200.0f * dpi, 0))) {
                    openRequest_ = last_.openPath;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
                if (!openable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("The editor opens projects; this result is %s.", last_.openPath.c_str());
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
