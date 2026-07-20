#pragma once
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver {

// Minimal file I/O used by the asset/format loaders (Phase 2 onward).
// Directory containing the running executable (for locating assets next to it).
std::string executableDir();

// Per-user, per-machine state that is NOT project content and NOT engine content — the recent
// project list and anything like it. `%LOCALAPPDATA%\AverEngine` on Windows. Falls back to the
// executable directory only if the OS will not name the folder.
std::string userDataDir();

// The user's Documents folder, asked of the OS rather than assembled from %USERPROFILE%: it is
// routinely redirected (OneDrive), and guessing puts the default projects root in the wrong place.
std::string documentsDir();

bool fileExists(const std::string& path);
bool directoryExists(const std::string& path);
bool createDirectories(const std::string& path);
bool readFileBytes(const std::string& path, std::vector<u8>& out);
bool readFileText(const std::string& path, std::string& out);
bool writeFileBytes(const std::string& path, const void* data, usize size);
bool writeFileText(const std::string& path, const std::string& text);

// Native "open file" dialog (Windows SDK IFileOpenDialog; no third-party dependency).
// `spec` is a semicolon-separated pattern list, e.g. "*.ocproject". Returns false if the user
// cancelled or the shell dialog is unavailable — callers must always keep a typed-path route.
bool openFileDialog(const std::string& title, const std::string& filterLabel,
                    const std::string& spec, const std::string& initialDir, std::string& out);

} // namespace aver
