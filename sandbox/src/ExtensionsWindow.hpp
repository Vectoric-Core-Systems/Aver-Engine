#pragma once
// Window > Extensions: every extension the loader found under <executable dir>/Extensions (name, version, status and the reason it is refused, source
// link, licence, path), Install from file... for an .avext package, and Uninstall. All of the work (reading the package, checking it, unpacking it
// safely, replacing and uninstalling through the trash) is Aver.Extensions' Avext.hpp; this file lists, asks and shows. The rows are plain data, so
// they are built without ImGui; the window at the bottom of ExtensionsWindow.cpp is the only part that includes it.
#include "aver/core/Types.hpp"
#include "aver/extensions/Avext.hpp"
#include "aver/extensions/Extensions.hpp"

#include <string>
#include <vector>

namespace aver::editor {

// One line of the list.
struct ExtensionRow {
    std::string name, displayName, version, license, source, dir;
    bool ready = false;
    std::string reason;          // why it is refused; empty when Ready
    bool sourceIsWebLink = false;
};

std::vector<ExtensionRow> extensionRows(const ext::Registry& registry);

// True for an address openUrl would open (http:// or https://).
bool isWebLink(const std::string& text);

class ExtensionsWindow {
public:
    // The flag the Window menu toggles.
    bool* openFlag() { return &open_; }
    void open() { open_ = true; }

    // Re-reads <executable dir>/Extensions.
    void rescan();
    const std::vector<ExtensionRow>& rows() const { return rows_; }

    // Reads the package and, when it is a package, holds it for the confirmation (installPending / replacePending). False with `why` when it is not one.
    bool inspect(const std::string& avextPath, std::string* why);
    const ext::PackageInfo& pending() const { return pending_; }
    bool pendingInstalled() const { return !pendingInstalledVersion_.empty() || pendingFolderExists_; }
    // Installs (replace = false) or replaces the held package, then rescans. The folder it replaces goes to the Recycle Bin / trash.
    ext::InstallResult installPending(bool replace);
    // Moves the installed folder to the Recycle Bin / trash, then rescans.
    ext::InstallResult uninstall(const std::string& dirName);

    // The window and its confirmation popups. Call every frame, outside the menu bar.
    void draw(f32 dpi);

    const std::string& lastMessage() const { return message_; }
    bool lastOk() const { return messageOk_; }

private:
    void report(const ext::InstallResult& result);

    bool open_ = false, wasOpen_ = false, scanned_ = false;
    ext::Registry registry_;
    std::vector<ExtensionRow> rows_;
    ext::PackageInfo pending_;
    bool havePending_ = false, pendingFolderExists_ = false;
    std::string pendingInstalledVersion_;
    std::string message_;
    bool messageOk_ = true;
    bool wantInstallPopup_ = false;
    std::string uninstallName_;
    bool wantUninstallPopup_ = false;
};

// The one window the Window menu and the shell share.
ExtensionsWindow& extensionsWindow();

}  // namespace aver::editor
