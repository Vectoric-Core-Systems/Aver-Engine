#pragma once

namespace aver::platform {

// Hides the console window IF THIS PROCESS OWNS IT, and does nothing otherwise.
//
// WHY THE OWNERSHIP TEST IS THE WHOLE DESIGN. Sandbox.exe is a console-subsystem application, so
// Windows gives it a console window when it is launched from Explorer or a shortcut -- a black
// rectangle that opens beside the editor, sits there for the session and looks like a bug. The
// obvious fix, linking /SUBSYSTEM:WINDOWS the way AverCrashReporter does, is the wrong one here:
// scripts/gates.ps1 and scripts/test.ps1 READ this program's output, and a GUI-subsystem process
// launched from a shell has no stdout attached to it. Silencing the console would silence the
// measurement tooling with it.
//
// So the question is not "is this a GUI run" but "did anyone ask for this console". If the process
// was started from an existing terminal, or by a script that piped it, the console belongs to that
// parent and other processes are attached to it -- hiding it would take away a window the user is
// using. If this process is the ONLY one attached, Windows created the console for it alone and
// nobody is reading it. GetConsoleProcessList answers exactly that, and it is the standard test.
//
// Redirection is unaffected either way: a pipe or a file handle is not the window, so hiding the
// window cannot lose output that was being captured.
//
// A NO-OP on a platform with no console concept, and on a process with no console at all.
void hideOwnConsoleWindow();

}  // namespace aver::platform
