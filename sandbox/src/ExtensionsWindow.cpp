// Window > Extensions. See ExtensionsWindow.hpp; the install, replace and uninstall rules are Avext.cpp's.
#include "ExtensionsWindow.hpp"

#include "EditorNotifications.hpp"
#include "ExtensionImport.hpp"
#include "ShellIntegration.hpp"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

#if AVER_WITH_IMGUI
#include "imgui.h"
#endif

namespace aver::editor {

namespace {

std::string lowerCase(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string extensionsRoot() { return executableDir() + "/Extensions"; }

// The last path segment.
std::string leafOf(const std::string& path) {
    const usize at = path.find_last_of("/\\");
    return at == std::string::npos ? path : path.substr(at + 1);
}

}  // namespace

bool isWebLink(const std::string& text) { return text.rfind("https://", 0) == 0 || text.rfind("http://", 0) == 0; }

std::vector<ExtensionRow> extensionRows(const ext::Registry& registry) {
    std::vector<ExtensionRow> rows;
    for (const ext::Extension& e : registry.extensions()) {
        ExtensionRow r;
        r.name = e.name;
        r.displayName = e.displayName;
        r.version = e.version;
        r.license = e.license;
        r.source = e.source;
        r.dir = e.dir;
        r.ready = e.ready();
        r.reason = e.reason;
        r.sourceIsWebLink = isWebLink(e.source);
        rows.push_back(std::move(r));
    }
    return rows;
}

ExtensionsWindow& extensionsWindow() {
    static ExtensionsWindow window;
    return window;
}

void ExtensionsWindow::rescan() {
    registry_.clear();
    registry_.scanDefault();
    rows_ = extensionRows(registry_);
    scanned_ = true;
}

bool ExtensionsWindow::inspect(const std::string& avextPath, std::string* why) {
    havePending_ = false;
    if (!scanned_) rescan();
    if (!ext::inspectPackage(avextPath, pending_, why)) return false;
    pendingFolderExists_ = false;
    pendingInstalledVersion_.clear();
    for (const ExtensionRow& row : rows_)
        if (lowerCase(leafOf(row.dir)) == lowerCase(pending_.manifest.name) || lowerCase(row.name) == lowerCase(pending_.manifest.name)) {
            pendingFolderExists_ = true;
            pendingInstalledVersion_ = row.version;
        }
    havePending_ = true;
    return true;
}

void ExtensionsWindow::report(const ext::InstallResult& result) {
    message_ = result.message;
    messageOk_ = result.ok;
    Notification n;
    n.severity = result.ok ? NotifySeverity::Success : NotifySeverity::Error;
    n.title = result.ok ? "Extension installed" : "Extension not changed";
    n.body = result.message;
    if (result.ok) n.body += ". Importers are available now; restart the editor to load the extension's scripting part.";
    notifications().push(std::move(n));
    if (result.ok) AVER_INFO("[Extensions] {} ({})", result.message, result.dir);
    else AVER_WARN("[Extensions] {}", result.message);
}

ext::InstallResult ExtensionsWindow::installPending(bool replace) {
    ext::InstallResult result;
    if (!havePending_) {
        result.message = "No package is chosen.";
        return result;
    }
    ext::InstallOptions options;
    options.replace = replace;
    options.moveToTrash = [](const std::string& path) { return moveToRecycleBin(path); };
    result = ext::installPackage(pending_.path, extensionsRoot(), options);
    report(result);
    havePending_ = false;
    rescan();
    extensionImporter().scan();
    return result;
}

ext::InstallResult ExtensionsWindow::uninstall(const std::string& dirName) {
    ext::InstallResult result = ext::uninstallExtension(extensionsRoot(), dirName, [](const std::string& path) { return moveToRecycleBin(path); });
    if (result.ok) result.message += ". Restart the editor to unload its scripting part.";
    report(result);
    rescan();
    extensionImporter().scan();
    return result;
}

#if AVER_WITH_IMGUI

namespace {
const ImVec4 kGood(0.45f, 0.85f, 0.45f, 1), kBad(0.93f, 0.42f, 0.38f, 1), kWarn(0.95f, 0.72f, 0.25f, 1);

std::string megabytes(u64 bytes) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f MB", double(bytes) / (1024.0 * 1024.0));
    return buf;
}
}  // namespace

void ExtensionsWindow::draw(f32 dpi) {
    if (open_ && !wasOpen_) rescan();
    wasOpen_ = open_;

    if (open_) {
        ImGui::SetNextWindowSize(ImVec2(900.0f * dpi, 420.0f * dpi), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Extensions", &open_)) {
            if (ImGui::Button("Install from file...")) {
                std::string picked, why;
                if (openFileDialog("Install an extension", "Aver extension (*.avext)", "*.avext", executableDir(), picked)) {
                    if (inspect(picked, &why)) {
                        wantInstallPopup_ = true;
                        message_.clear();
                    } else {
                        message_ = "Cannot read " + picked + ": " + why;
                        messageOk_ = false;
                    }
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Pick an .avext package. Its manifest and SHA-256 are shown before anything is copied.");
            ImGui::SameLine();
            if (ImGui::Button("Rescan")) rescan();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", extensionsRoot().c_str());
            if (!message_.empty()) ImGui::TextColored(messageOk_ ? kGood : kBad, "%s", message_.c_str());
            ImGui::Spacing();

            if (rows_.empty()) {
                ImGui::TextWrapped("No extension is installed. Extensions live in the Extensions folder beside the editor; Install from file... unpacks an .avext package there.");
            } else if (ImGui::BeginTable("##extensions", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                                                                ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Extension", ImGuiTableColumnFlags_None, 2.0f);
                ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_None, 1.2f);
                ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_None, 3.0f);
                ImGui::TableSetupColumn("Licence", ImGuiTableColumnFlags_None, 1.2f);
                ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_None, 2.5f);
                ImGui::TableSetupColumn("Folder", ImGuiTableColumnFlags_None, 3.0f);
                ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, 150.0f * dpi);
                ImGui::TableHeadersRow();
                for (usize i = 0; i < rows_.size(); ++i) {
                    const ExtensionRow& r = rows_[i];
                    ImGui::PushID(int(i));
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextWrapped("%s", r.displayName.c_str());
                    if (r.displayName != r.name) ImGui::TextDisabled("%s", r.name.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextWrapped("%s", r.version.empty() ? "-" : r.version.c_str());
                    ImGui::TableSetColumnIndex(2);
                    if (r.ready) {
                        ImGui::TextColored(kGood, "Ready");
                    } else {
                        ImGui::TextColored(kBad, "Refused");
                        ImGui::TextWrapped("%s", r.reason.c_str());
                    }
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextWrapped("%s", r.license.empty() ? "-" : r.license.c_str());
                    ImGui::TableSetColumnIndex(4);
                    if (r.source.empty()) {
                        ImGui::TextDisabled("-");
                    } else if (r.sourceIsWebLink) {
                        if (ImGui::TextLink(r.source.c_str())) openUrl(r.source);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Opens %s in the browser", r.source.c_str());
                    } else {
                        ImGui::TextWrapped("%s", r.source.c_str());
                    }
                    ImGui::TableSetColumnIndex(5);
                    ImGui::TextDisabled("%s", r.dir.c_str());
                    ImGui::TableSetColumnIndex(6);
                    if (ImGui::SmallButton("Reveal")) revealInFileManager(r.dir);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Uninstall")) {
                        uninstallName_ = leafOf(r.dir);
                        wantUninstallPopup_ = true;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Moves the folder to the Recycle Bin.");
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }

    // The confirmations are modals of their own, opened from the window above and drawn at the top of the ID stack.
    if (wantInstallPopup_) {
        ImGui::OpenPopup("Install extension###avextInstall");
        wantInstallPopup_ = false;
    }
    if (wantUninstallPopup_) {
        ImGui::OpenPopup("Uninstall extension###avextUninstall");
        wantUninstallPopup_ = false;
    }

    if (ImGui::BeginPopupModal("Install extension###avextInstall", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        if (!havePending_) {
            ImGui::CloseCurrentPopup();
        } else {
            const ext::PackageInfo& p = pending_;
            const ext::Extension& m = p.manifest;
            ImGui::PushTextWrapPos(560.0f * dpi);
            ImGui::Text("%s %s", (m.displayName.empty() ? m.name : m.displayName).c_str(), m.version.c_str());
            ImGui::Separator();
            ImGui::Text("Name: %s", m.name.c_str());
            ImGui::Text("Licence: %s", m.license.empty() ? "(none stated)" : m.license.c_str());
            ImGui::TextWrapped("Source: %s", m.source.empty() ? "(none stated)" : m.source.c_str());
            ImGui::Text("Extension ABI %d, for engine %s", int(m.abi), m.engineRange.empty() ? "(unstated)" : m.engineRange.c_str());
            std::string platforms;
            for (const std::string& pl : p.platforms) platforms += (platforms.empty() ? "" : ", ") + pl;
            ImGui::TextWrapped("Native builds: %s", platforms.empty() ? "none" : platforms.c_str());
            ImGui::Text("%zu files, %s unpacked", p.fileCount, megabytes(p.unpackedSize).c_str());
            ImGui::TextWrapped("File: %s", p.path.c_str());
            ImGui::Text("SHA-256:");
            ImGui::TextUnformatted(p.sha256.c_str());
            if (ImGui::SmallButton("Copy SHA-256")) ImGui::SetClipboardText(p.sha256.c_str());
            ImGui::Spacing();
            if (!p.problem.empty()) {
                ImGui::TextColored(kBad, "Cannot be installed: %s", p.problem.c_str());
            } else if (pendingInstalled()) {
                ImGui::TextColored(kWarn, "%s%s%s is already installed. Replace moves the installed folder to the Recycle Bin and installs this package.", m.name.c_str(),
                                   pendingInstalledVersion_.empty() ? "" : " ", pendingInstalledVersion_.c_str());
            }
            ImGui::TextDisabled("An extension runs native code with your permissions. Install packages from sources you trust, and compare the SHA-256 with the release page.");
            ImGui::PopTextWrapPos();
            ImGui::Spacing();

            ImGui::BeginDisabled(!p.problem.empty());
            if (ImGui::Button(pendingInstalled() ? "Replace" : "Install", ImVec2(140.0f * dpi, 0))) {
                installPending(pendingInstalled());
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100.0f * dpi, 0))) {
                havePending_ = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Uninstall extension###avextUninstall", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushTextWrapPos(480.0f * dpi);
        ImGui::TextWrapped("Move %s to the Recycle Bin? Projects that use it will log that it is missing until it is installed again.", uninstallName_.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("Uninstall", ImVec2(140.0f * dpi, 0))) {
            uninstall(uninstallName_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100.0f * dpi, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

#else

void ExtensionsWindow::draw(f32) {}

#endif

}  // namespace aver::editor
