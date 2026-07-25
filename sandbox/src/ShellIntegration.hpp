#pragma once
// Asking the OPERATING SYSTEM to do something with a path: show it, open it, or throw it away.
//
// Separate from IdeIntegration.hpp, which answers "which code editors exist and how do I make one
// jump to a file:line". None of this is about editors — "Show in Explorer" is a shell verb, and the
// recycle bin is a shell API — and lumping them together would make that header about two things.
//
// Windows-only today, like the rest of the sandbox's platform edges. Each function returns false
// rather than throwing: every caller is a click handler in a UI frame, where an exception escaping
// is std::terminate.
#include <string>

namespace aver::editor {

// Open the OS file manager with `path` selected. A directory opens showing itself.
bool revealInFileManager(const std::string& path);

// Hand `path` to whatever the shell has registered for its extension — the "open it with the normal
// thing" answer, for files no IDE should claim (a .png, a .txt, a .ocmesh).
bool openWithShell(const std::string& path);

// Move `path` to the OS recycle bin, where the user can get it back.
//
// Deliberately NOT std::filesystem::remove. The Content Browser's delete acts on files a person
// authored, from a single keypress, and an unlink is unrecoverable — the recycle bin turns a
// misclick into an annoyance instead of lost work. Fails (returning false) rather than falling back
// to a permanent delete, because a silent escalation from "recoverable" to "gone" is the one
// behaviour that would make this worse than not having it.
//
// Works on directories too, taking their contents with them.
bool moveToRecycleBin(const std::string& path);

} // namespace aver::editor
