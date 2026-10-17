// GameUiText: the host half of the game-UI text engine (modules/ui UiTextEngine, docs/GAME_UI_TEXT.md).
// The engine lives in Aver.UI.Abi; what only a HOST can do is open font files, name the font set, follow
// the UI language and lend the renderer the atlas. Shared by the editor's Play/Preview and the packaged
// game for the reason GameUiInput.hpp is: two copies drift.
//
//   uiTextInit(contentDir)     once, after the UI ABI is up: installs the loader, loads
//                              <contentDir>/UI/Fonts.ocfonts (or the built-in default), follows aver_fw_loc_culture
//   uiTextBindRenderer(r)      once, after UiRenderer::create: lends the renderer the atlas
//   uiTextReloadIfChanged(ev)  the editor's content watcher: reloads on .ocfonts / .ttf / .otf / .ttc
//   uiTextShutdown()           before the device is torn down: unbinds the renderer, closes every font
//
// FILES. Project fonts (Content, and the executable's directory via an "@exe/" path) are mapped read only
// and TRACED (traceFileOpen) -- inside a package that is the proof the package is self-contained. OS fonts
// (Windows only in v1) are mapped read only and deliberately NOT traced: a packaged game legitimately
// reads C:\Windows\Fonts, and scripts/verify-game.ps1 flags any traced open outside the package.
//
// HEADER-ONLY, evaluated per host with its own AVER_WITH_UI_ABI.
#pragma once
#include "aver/core/Types.hpp"

#include "aver/platform/DirectoryWatcher.hpp"

#include <string>
#include <vector>

// Declared here so the entry points below compile (as no-ops) in a host built without the UI ABI.
namespace aver::render::ui { class UiRenderer; }

#if AVER_WITH_UI_ABI
#  include "aver/core/Log.hpp"
#  include "aver/platform/FileSystem.hpp"
#  include "aver/render/ui/UiRenderer.hpp"
#  include "aver/ui/ui_abi.h"
#  if AVER_MODULE_FRAMEWORK
#    include "aver/framework/framework_loc_abi.h"
#  endif
#  include <cstdlib>
#endif

namespace aver::game {

#if AVER_WITH_UI_ABI
namespace uitext_detail {

// The font set used when the project ships no Content/UI/Fonts.ocfonts: Roboto next to the executable,
// the OS fonts, and the baked bitmap font lent with aver_ui_set_font.
inline const char* defaultFontSet() {
    return R"(OCFONTS 1
DEFAULT Default
FAMILY Default
FACE @exe/Roboto-Regular.ttf
FACE @exe/Roboto-Medium.ttf weight=bold
SYSTEM auto
BITMAP legacy
)";
}

struct State {
    std::string contentDir;
    render::ui::UiRenderer* renderer = nullptr;
    bool installed = false;
};

inline State& state() {
    static State s;
    return s;
}

inline std::string joinPath(const std::string& dir, const char* name) {
    std::string p = dir;
    if (!p.empty() && p.back() != '/' && p.back() != '\\') p += AVER_PATH_SEP;
    p += name;
    toNativeSeparators(p);
    return p;
}

// AverUiFontOpenFn. Project fonts are traced, OS fonts are not (see the header note).
inline int32_t openFont(int32_t kind, const char* name, const uint8_t** outData, uint64_t* outSize, void** outToken) {
    if (!name || !*name || !outData || !outSize || !outToken) return 0;
    std::string path;
    if (kind == AVER_UI_FONT_CONTENT) {
        path = joinPath(state().contentDir, name);
    } else if (kind == AVER_UI_FONT_ENGINE) {
        path = joinPath(executableDir(), name);
    } else if (kind == AVER_UI_FONT_SYSTEM) {
#if defined(_WIN32)
        const char* win = std::getenv("WINDIR");
        if (!win) win = std::getenv("SystemRoot");
        const char* local = std::getenv("LOCALAPPDATA");
        MappedFile probe;
        if (win && mapFileReadOnly(joinPath(std::string(win) + "\\Fonts", name), probe)) {
            auto* held = new MappedFile(probe);
            AVER_INFO("[UiText] system font {}", name);
            *outData = held->data; *outSize = held->size; *outToken = held;
            return 1;
        }
        if (local && mapFileReadOnly(joinPath(std::string(local) + "\\Microsoft\\Windows\\Fonts", name), probe)) {
            auto* held = new MappedFile(probe);
            AVER_INFO("[UiText] system font {}", name);
            *outData = held->data; *outSize = held->size; *outToken = held;
            return 1;
        }
        return 0;
#else
        return 0;   // OS fonts are Windows only in v1: other hosts use project fonts
#endif
    } else {
        return 0;
    }
    auto* held = new MappedFile();
    if (!mapFileReadOnly(path, *held)) { delete held; return 0; }
    traceFileOpen(path);   // a project font: inside a package this is what the isolation check sees
    *outData = held->data;
    *outSize = held->size;
    *outToken = held;
    return 1;
}

// AverUiFontCloseFn.
inline void closeFont(void* token) {
    auto* held = static_cast<MappedFile*>(token);
    if (!held) return;
    unmap(*held);
    delete held;
}

inline bool loadSet() {
    State& s = state();
    std::string text;
    const std::string path = joinPath(s.contentDir, "UI/Fonts.ocfonts");
    const bool authored = fileExists(path) && readFileText(path, text);
    if (authored && aver_ui_text_load_fontset(text.c_str())) {
        AVER_INFO("[UiText] font set {}", path);
        return true;
    }
    if (authored) AVER_WARN("[UiText] '{}': {} -- using the default font set", path, aver_ui_text_error());
    if (!aver_ui_text_load_fontset(defaultFontSet())) {
        AVER_WARN("[UiText] the default font set did not load: {}", aver_ui_text_error());
        return false;
    }
    return true;
}

} // namespace uitext_detail
#endif

// Installs the font loader and loads the font set. `contentDir` is the project's Content folder (empty for
// a host with none: only the default set). False when no font set could be loaded -- text then falls back
// to the lent bitmap font exactly as before.
inline bool uiTextInit(const std::string& contentDir) {
#if AVER_WITH_UI_ABI
    uitext_detail::State& s = uitext_detail::state();
    s.contentDir = contentDir;
    aver_ui_text_set_loader(&uitext_detail::openFont, &uitext_detail::closeFont);
    s.installed = true;
#  if AVER_MODULE_FRAMEWORK
    aver_ui_text_set_language_fn(&aver_fw_loc_culture);
#  endif
    return uitext_detail::loadSet();
#else
    (void)contentDir;
    return false;
#endif
}

// Lends the renderer the glyph atlas (UiRenderer::setTextAtlas). Pass the renderer created for this host.
inline void uiTextBindRenderer(render::ui::UiRenderer* renderer) {
#if AVER_WITH_UI_ABI
    uitext_detail::state().renderer = renderer;
    if (renderer)
        renderer->setTextAtlas(static_cast<ui::UiGlyphAtlas*>(const_cast<void*>(aver_ui_text_atlas())));
#else
    (void)renderer;
#endif
}

// Reloads the font set when the content watcher reports a font or .ocfonts change. True when it reloaded.
inline bool uiTextReloadIfChanged(const std::vector<FileEvent>& events) {
#if AVER_WITH_UI_ABI
    if (!uitext_detail::state().installed) return false;
    const auto ends = [](const std::string& p, const char* suffix) {
        const std::string s(suffix);
        if (p.size() < s.size()) return false;
        for (usize i = 0; i < s.size(); ++i) {
            char a = p[p.size() - s.size() + i], b = s[i];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (a != b) return false;
        }
        return true;
    };
    bool changed = false;
    for (const FileEvent& e : events)
        if (ends(e.path, ".ocfonts") || ends(e.path, ".ttf") || ends(e.path, ".otf") || ends(e.path, ".ttc")) changed = true;
    if (!changed) return false;
    return uitext_detail::loadSet();
#else
    (void)events;
    return false;
#endif
}

// Unbinds the renderer (BEFORE the device is torn down: the renderer reads the atlas) and closes every font.
inline void uiTextShutdown() {
#if AVER_WITH_UI_ABI
    uitext_detail::State& s = uitext_detail::state();
    if (s.renderer) s.renderer->setTextAtlas(nullptr);
    s.renderer = nullptr;
    if (s.installed) aver_ui_text_set_loader(nullptr, nullptr);
    s.installed = false;
#endif
}

} // namespace aver::game
