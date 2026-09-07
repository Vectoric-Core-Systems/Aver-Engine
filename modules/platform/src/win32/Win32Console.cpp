#include "aver/platform/Console.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>

namespace aver::platform {
namespace {

// Is this standard handle already pointing somewhere real? A GUI-subsystem process gets std handles
// ONLY from the parent's STARTUPINFO, so a valid handle here means the caller redirected us -- a
// pipe from PowerShell capturing output, CTest, Python's subprocess -- and reopening the CRT stream
// onto a console would throw that capture away.
//
// BOTH NULL AND INVALID_HANDLE_VALUE MEAN "NOTHING", and they are different values: an unset handle
// comes back NULL, while a handle the OS refused comes back INVALID_HANDLE_VALUE. Testing only one
// of them is the classic way this check passes when it should not.
bool handleIsLive(DWORD which) {
    const HANDLE h = GetStdHandle(which);
    return h != nullptr && h != INVALID_HANDLE_VALUE;
}

// Points one CRT stream at the console this process is now attached to. freopen_s rather than
// freopen because MSVC's CRT deprecates the latter into a hard error under /sdl.
void bindToConsole(const char* device, const char* mode, FILE* stream) {
    FILE* reopened = nullptr;
    (void)freopen_s(&reopened, device, mode, stream);
}

}  // namespace

void attachParentConsole() {
    // ALREADY REDIRECTED: nothing to do, and doing something would be the bug. See the header.
    if (handleIsLive(STD_OUTPUT_HANDLE) || handleIsLive(STD_ERROR_HANDLE)) return;

    // Borrow the launching terminal's console. Fails -- and is meant to -- when the parent has no
    // console: Explorer, a shortcut, the launcher. AllocConsole is deliberately not the fallback.
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;

    // The CRT captured its stdio targets at startup, when there was no console, so attaching is not
    // by itself enough for printf to appear: the streams have to be pointed at the console device.
    // CONOUT$/CONIN$ name "the console this process is attached to" rather than any file on disk.
    bindToConsole("CONOUT$", "w", stdout);
    bindToConsole("CONOUT$", "w", stderr);
    bindToConsole("CONIN$",  "r", stdin);
    // std::cout needs nothing further: it is synchronised with stdout by default, so pointing the C
    // stream at the console carries the C++ one with it.
}

}  // namespace aver::platform
