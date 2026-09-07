#include "aver/platform/Console.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace aver::platform {

// See the header for why ownership is the test rather than the subsystem.
void hideOwnConsoleWindow() {
    const HWND console = GetConsoleWindow();
    if (!console) return;   // no console attached at all; nothing to do

    // ASKED FOR THE COUNT, NOT THE LIST. GetConsoleProcessList needs a buffer big enough for every
    // attached process and returns the REQUIRED size when the buffer is too small -- so a two-entry
    // buffer answers the only question being asked ("is it exactly one?") without a heap allocation
    // and without a second call. A return of 0 is a failure, and the safe reading of a failure is
    // "someone else might be using this", so it leaves the window alone.
    DWORD pids[2] = {};
    const DWORD attached = GetConsoleProcessList(pids, 2);
    if (attached != 1) return;   // shared with a shell or a script: not ours to hide

    ShowWindow(console, SW_HIDE);
}

}  // namespace aver::platform
