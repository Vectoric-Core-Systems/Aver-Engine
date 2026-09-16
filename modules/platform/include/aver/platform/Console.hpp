#pragma once

namespace aver::platform {

// Adopts the parent's console when there is one, and does nothing otherwise.
//
// WHY THIS EXISTS. Sandbox.exe used to be a console-subsystem program, so Windows gave it a console
// window of its own whenever it was launched from Explorer, a shortcut or the launcher -- a black
// rectangle that opened beside the editor and sat there for the whole session. It is linked
// /SUBSYSTEM:WINDOWS now, exactly like AverEngineRuntime.exe and AverCrashReporter.exe, so that window is
// never created at all: not hidden a few milliseconds in, not flashed on the way past, never made.
//
// THAT ALONE WOULD HAVE COST THE MEASUREMENT TOOLING ITS OUTPUT, which is why this function is the
// other half of the change rather than an optional extra. scripts/gates.ps1, scripts/test.ps1 and
// tools/mcp/aver_mcp.py all READ this program's stdout, and a GUI-subsystem process attaches to no
// console on its own -- run one from a terminal and its output goes nowhere anybody can see.
//
// So: if the parent already handed us stdout -- a pipe, a file, CTest, Python's subprocess, any
// PowerShell invocation whose output is captured -- LEAVE IT ALONE. Redirection has nothing to do
// with the subsystem and already works; touching it here would clobber the very capture the caller
// set up. Only when there is no handle at all is a console worth looking for, and then only the
// PARENT's: AttachConsole(ATTACH_PARENT_PROCESS) borrows the terminal the user launched from and
// allocates nothing, so a run from a shell prints as it always did while a run from Explorer -- no
// parent console to attach to -- stays silent and windowless.
//
// AllocConsole IS DELIBERATELY NOT CALLED. Creating a console when none exists would put back the
// exact window this change removes.
//
// A NO-OP on a platform with no console concept.
void attachParentConsole();

}  // namespace aver::platform
