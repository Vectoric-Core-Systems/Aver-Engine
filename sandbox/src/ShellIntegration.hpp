#pragma once
// Asking the operating system to do something with a path: show it, open it, or throw it away.
// Windows-only today; every function returns false rather than throwing.
#include <string>

namespace aver::editor {

// Opens the OS file manager with `path` selected. A directory opens showing itself.
bool revealInFileManager(const std::string& path);

// Hands `path` to whatever the shell has registered for its extension.
bool openWithShell(const std::string& path);

// Moves `path` to the OS recycle bin. Works on directories too. Fails rather than falling back to a
// permanent delete.
bool moveToRecycleBin(const std::string& path);

} // namespace aver::editor
