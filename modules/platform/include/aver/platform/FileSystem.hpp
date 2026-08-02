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
// Observes every path the engine reads through readFileBytes/readFileText.
//
// EXISTS FOR ONE PURPOSE: letting a packaged game prove it is not reading the tree that built it.
// A package that quietly falls back to a dev-tree asset runs perfectly on the machine that made it
// and fails on every other, and no exit code says so -- the only way to catch it is to ask what was
// actually opened. scripts/verify-game.ps1 asserts every traced path is under the package root.
//
// This is the WHOLE engine file API, so the trace is complete for aver::platform. It does NOT cover
// LoadLibraryW (dxcompiler, nethost, hostfxr), the CLR's own probing, or stbi_load's internal
// fopen. Those limits are stated in verify-game.ps1 rather than implied.
using FileTraceFn = void (*)(const char* path, void* user);
void setFileTrace(FileTraceFn fn, void* user);

// Reports a file open that did NOT go through readFileBytes/readFileText.
//
// The binary format loaders (Avr1, OcMesh, OcAnim) open their own ifstream, so without this the
// trace would cover the manifest and the level and miss every mesh -- and verify-game.ps1 would
// happily pass a package that reaches into the dev tree for its geometry. Measured: a packaged
// game reported 2 traced opens before these loaders called it.
void traceFileOpen(const std::string& path);

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
