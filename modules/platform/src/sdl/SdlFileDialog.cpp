// openFileDialog over SDL 3, everywhere but Windows (FileSystem.cpp holds the IFileOpenDialog one).
//
// SDL shows the platform's own dialog: NSOpenPanel on macOS, the XDG desktop portal on Linux (so
// GNOME's, KDE's or COSMIC's picker, matching the rest of the desktop), with zenity as SDL's fallback
// when no portal answers.
//
// SYNCHRONOUS ON TOP OF AN ASYNCHRONOUS API. SDL_ShowFileDialogWithProperties returns at once and
// reports through a callback; every caller in the editor expects the Win32 shape, a call that returns
// the answer. So this waits for the callback, pumping the OS event queue so the dialog (and the
// portal's D-Bus reply) can be serviced. SDL_PumpEvents only moves events INTO SDL's queue, so input
// the editor's window receives meanwhile is still there for its next Window::pumpEvents(), the same
// way IFileOpenDialog::Show leaves the editor's own messages queued while it runs.
#include "aver/platform/FileSystem.hpp"
#include "SdlVideo.hpp"
#include "aver/core/Log.hpp"

#include <SDL3/SDL.h>

#include <atomic>
#include <string>
#include <vector>

namespace aver {
namespace {

struct DialogResult {
    std::atomic<bool> done{false};
    bool ok = false;
    std::string path;
};

// The callback may run on another thread (the portal's), so it only records and raises the flag.
void SDLCALL onDialog(void* user, const char* const* files, int) {
    auto* r = static_cast<DialogResult*>(user);
    if (files && files[0]) {   // null = error, an empty list = cancelled
        r->path = files[0];
        r->ok = true;
    } else if (!files) {
        AVER_WARN("[Platform] file dialog failed: {}", SDL_GetError());
    }
    r->done.store(true, std::memory_order_release);
}

// "*.gltf;*.glb" -> "gltf;glb", the extension list SDL_DialogFileFilter wants. "*" or "*.*" -> "*".
std::string sdlPattern(const std::string& spec) {
    std::string out;
    usize b = 0;
    while (b <= spec.size()) {
        usize e = spec.find(';', b);
        if (e == std::string::npos) e = spec.size();
        std::string p = spec.substr(b, e - b);
        if (p == "*" || p == "*.*") return "*";
        if (p.rfind("*.", 0) == 0) p = p.substr(2);
        if (!p.empty()) { if (!out.empty()) out += ';'; out += p; }
        b = e + 1;
    }
    return out.empty() ? std::string("*") : out;
}

}  // namespace

bool openFileDialog(const std::string& title, const std::string& filterLabel,
                    const std::string& spec, const std::string& initialDir, std::string& out) {
    if (!detail::sdlVideoUp()) return false;

    const std::string pattern = sdlPattern(spec);
    const SDL_DialogFileFilter filters[] = {{filterLabel.c_str(), pattern.c_str()}, {"All files", "*"}};
    // A folder, with the trailing separator some backends need to read it as one rather than as a
    // file name to preselect.
    std::string location = initialDir;
    if (!location.empty() && location.back() != '/') location += '/';

    DialogResult result;
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, const_cast<SDL_DialogFileFilter*>(filters));
    SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 2);
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title.c_str());
    if (!location.empty()) SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, location.c_str());
    // Parented to whatever has the keyboard, so the dialog opens over the editor and not behind it.
    if (SDL_Window* parent = SDL_GetKeyboardFocus()) SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, parent);
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, &onDialog, &result, props);

    // `filters`, `pattern`, `location` and the properties stay alive until the callback has run:
    // SDL may read them from the dialog's own thread until then.
    while (!result.done.load(std::memory_order_acquire)) {
        SDL_PumpEvents();
        SDL_Delay(10);
    }
    SDL_DestroyProperties(props);
    if (result.ok) out = result.path;
    return result.ok;
}

} // namespace aver
