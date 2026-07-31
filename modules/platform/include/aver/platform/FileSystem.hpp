#pragma once
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver {

// The directory containing the running executable.
std::string executableDir();

// Per-user, per-machine state that is neither project nor engine content.
// `%LOCALAPPDATA%\AverEngine` on Windows; the executable directory if the OS will not name it.
std::string userDataDir();

// The user's Documents folder, as the OS reports it (it is routinely redirected).
std::string documentsDir();

// True if the path exists.
bool fileExists(const std::string& path);
// True if the path exists and is a directory.
bool directoryExists(const std::string& path);
// Creates the directory and every missing parent. True if it exists afterwards.
bool createDirectories(const std::string& path);
// Reads a whole file into `out`. False if it cannot be read.
bool readFileBytes(const std::string& path, std::vector<u8>& out);
// Reads a whole file into `out` as text. False if it cannot be read.
bool readFileText(const std::string& path, std::string& out);
// Writes `size` bytes to the file, truncating it. False on failure.
bool writeFileBytes(const std::string& path, const void* data, usize size);
// Writes `text` to the file, truncating it. False on failure.
bool writeFileText(const std::string& path, const std::string& text);

// Shows the native open-file dialog. `spec` is a semicolon-separated pattern list, e.g. "*.ocproject".
// False if the user cancelled or the shell dialog is unavailable.
bool openFileDialog(const std::string& title, const std::string& filterLabel,
                    const std::string& spec, const std::string& initialDir, std::string& out);

} // namespace aver
