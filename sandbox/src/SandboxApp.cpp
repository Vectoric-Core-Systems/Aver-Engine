// The one translation unit that compiles stb_image_write's implementation.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "aver/core/HitchMarks.hpp"
#include "aver/core/Types.hpp"   // AVER_PATH_SEP
#include "stb_image_write.h"
#undef STB_IMAGE_WRITE_IMPLEMENTATION
#include "SandboxApp.hpp"
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
#  include "aver/framework/framework_anim_abi.h"   // aver_fw_set_anim_provider (montages)
#  include "aver/framework/framework_ik_abi.h"     // [wave5/ik-retarget] aver_fw_set_ik_provider

#  include "aver/framework/framework_morph_abi.h"   // [wave9/morph] aver_fw_set_morph_provider
#  include "aver/framework/framework_worldfx_abi.h"   // [wave9/worldfx] aver_fw_worldfx_tick
#endif
#if AVER_MODULE_FRAMEWORK
#  include "aver/framework/framework_loc_abi.h"    // the UI layout editor's culture preview
#endif
#if AVER_MODULE_RAGDOLL
#  include "aver/ragdoll/RagdollSystem.hpp"
#endif
#include "aver/assets/LevelSky.hpp"
#include "TextureEditor.hpp"   // makeTextureEditor; SandboxApp.hpp does not pull this one in
#include "InstanceSetEditor.hpp"   // makeInstanceSetEditor (.ocinst); not pulled in by SandboxApp.hpp
#include "aver/game/GameCamera.hpp"
#include "aver/game/GameIkProbe.hpp"   // [wave5/ik-retarget] the foot-IK ground probe
#include "aver/game/GameTick.hpp"
#include "aver/game/GameSystemsWiring.hpp"
#include "aver/game/SequenceHost.hpp"   // [wave5/sequencer] the ABI provider
#include "aver/game/GameUiInput.hpp"
#include "aver/game/GameUiText.hpp"
#include "aver/platform/FileSystem.hpp"   // userDataDir: where the trajectory network's weights live
#if AVER_MODULE_VOXI
#include "ModeSwitchNotice.hpp"
#endif

namespace aver {
// Rows ImGui draws `t` as: one per newline, plus a last partial row when it has text.
static u32 logTextRows(std::string_view t) {
    u32 n = static_cast<u32>(std::count(t.begin(), t.end(), '\n'));
    const usize nl = t.find_last_of('\n');
    const std::string_view tail = nl == std::string_view::npos ? t : t.substr(nl + 1);
    if (tail.find_first_not_of('\r') != std::string_view::npos) ++n;
    return n ? n : 1u;
}

// Appends log to Output Log buffer; trims for splash (keeps subsystem tag, drops level/paths).
// Must not log: core log mutex is held.
 std::string SandboxApp::splashTextFor(std::string_view msg) {
    std::string t(msg);
    if (!t.empty() && t[0] == '[') {                       // drop the level prefix, keep the tag
        const usize close = t.find(']');
        if (close != std::string::npos) t.erase(0, close + 1);
    }
    while (!t.empty() && t.front() == ' ') t.erase(0, 1);
    // Keep last 3 path segments to show which asset is loading.
    constexpr int kKeepSegments = 3;
    usize cut = std::string::npos;
    usize probe = t.find_last_of("\\/");
    for (int i = 0; i < kKeepSegments && probe != std::string::npos; ++i) {
        cut = probe;
        probe = probe ? t.find_last_of("\\/", probe - 1) : std::string::npos;
    }
    if (cut != std::string::npos && probe != std::string::npos && probe + 1 < t.size()) {
        // Only trim when there was MORE path than we keep; a short relative path is left alone.
        const usize wordStart = t.find_last_of(" \t'\"", probe);
        t = (wordStart == std::string::npos ? std::string() : t.substr(0, wordStart + 1)) +
            t.substr(probe + 1);
    }
    if (t.size() > 110) t.resize(110);
    return t;
}

 void SandboxApp::logSink(void* ctx, LogLevel level, std::string_view msg) {
    auto* self = static_cast<SandboxApp*>(ctx);
    std::lock_guard<std::mutex> lock(self->logMutex_);
    const u32 rows = logTextRows(msg);
    self->logLines_.push_back({level, std::string(msg), rows});
    if (rows > 1) ++self->logMultiRowCount_;
    if (self->logLines_.size() > kMaxLogLines) {
        if (self->logLines_.front().rows > 1) --self->logMultiRowCount_;
        self->logLines_.pop_front();
    }

    // Forwards Info+ lines to loading screen (main thread only; Jolt job pool may log too).
    // MUST NOT LOG: core log mutex is held and non-recursive.
    if (level >= LogLevel::Info && std::this_thread::get_id() == self->mainThreadId_) {
        const std::string text = splashTextFor(msg);
        if (self->projectLoading_) self->projectLoading_->stage(text.c_str());
        else if (self->engineForSplash_ && self->engineForSplash_->loadingScreenActive())
            self->engineForSplash_->setLoadingStatus(text);
    }

    // Graph "Print" nodes get on-screen overlay. Filtered on "[Graph] " prefix (cheaper than draw-time scan).
    if (msg.rfind("[Graph] ", 0) == 0) {
        std::string text(msg.substr(8));
        // Collapse consecutive duplicates with rising count instead of flooding buffer.
        if (!self->graphPrints_.empty() && self->graphPrints_.back().text == text) {
            ++self->graphPrints_.back().count;
            // Unstamped so it re-stamps to now next frame: a still-firing print must not fade
            // under its own rising count.
            self->graphPrints_.back().at = -1.0;
        } else {
            self->graphPrints_.push_back({std::move(text), -1.0, 1});
            if (self->graphPrints_.size() > kMaxGraphPrints) self->graphPrints_.pop_front();
        }
    }

    // Errors/criticals become notifications. pushFromLog's contract governs what it may do.
    editor::notifications().pushFromLog(level, msg);
}

// Boot configuration for the editor window.
BootConfig SandboxApp::config() const  {
    BootConfig c; c.windowTitle="Aver Engine \xE2\x80\x94 Editor"; c.windowWidth=1600; c.windowHeight=900;
    c.maxFrames=maxFrames_; c.headless=headless_; c.useWarp=useWarp_;
    // Windowed by default; bounded captures must not be reshaped by --fullscreen.
    c.fullscreen = fullscreenOverride_ && maxFrames_ == 0;
    c.enableDebugLayer=debugLayer_;
    c.backend = backendName_.empty() ? nullptr : backendName_.c_str();
    return c;
}

// Has project reached screen? Returns true when draw count settles for kSettleFrames frames.
bool SandboxApp::startupComplete() const  {
#if AVER_MODULE_SCENE
    if (lastSceneDrawn_ < 0) return false;
    if (project_.manifestPath.empty()) return true;    // no project loading
    if (lastSceneDrawn_ == 0) return false;
    constexpr int kSettleFrames = 8;
    if (lastSceneDrawn_ == startupSettleCount_) ++startupSettleFrames_;
    else { startupSettleCount_ = lastSceneDrawn_; startupSettleFrames_ = 0; }
    return startupSettleFrames_ >= kSettleFrames;
#else
    // No scene walk in this tree, so there's no draw count to settle on; up as soon as the engine says so.
    return true;
#endif  // AVER_MODULE_SCENE
}

void SandboxApp::setUseWarp(bool w) { useWarp_ = w; }

void SandboxApp::setWindowed(bool w) { windowedOverride_ = w; }

void SandboxApp::setFullscreen(bool f) { fullscreenOverride_ = f; }

void SandboxApp::setBackend(std::string b) { backendName_ = std::move(b); }

void SandboxApp::setCultureOverride(std::string tag) { cultureOverride_ = std::move(tag); }

// --frame-budget <ms>: target frame time, and run the controller even in a bounded capture.
void SandboxApp::setFrameBudget(f32 ms) { frameBudgetMs_ = ms; frameBudgetForced_ = ms > 0.0f; }

void SandboxApp::setDebugLayer(bool d) { debugLayer_ = d; }

#if AVER_WITH_IMGUI
// Rebuild ImGui style and font atlas for the given DPI scale.
void SandboxApp::applyDpi(f32 dpi) {
    dpi_ = dpi;
    // Push dpi through editor::setGraphEditorDpi (not pulled from AssetEditor::draw()).
    editor::setGraphEditorDpi(dpi_);
    applyEditorMetrics();
    applyEditorColors();
    if (dpi_ > 1.01f) ImGui::GetStyle().ScaleAllSizes(dpi_);

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    fontMedium_ = nullptr;

    const std::string dir = executableDir();
    const std::string regular = dir + AVER_PATH_SEP "Roboto-Regular.ttf";
    const std::string medium  = dir + AVER_PATH_SEP "Roboto-Medium.ttf";
    const f32 px = 16.0f * dpi_;

    ImFont* body = fileExists(regular) ? io.Fonts->AddFontFromFileTTF(regular.c_str(), px) : nullptr;
    if (!body) {
        AVER_WARN("[Sandbox] '{}' missing or unreadable -- falling back to the built-in bitmap font", regular);
        io.Fonts->AddFontDefault();
        return;
    }
    io.FontDefault = body;
    mergeIconFont(px);
    if (fileExists(medium)) {
        fontMedium_ = io.Fonts->AddFontFromFileTTF(medium.c_str(), px);
        // Medium weight needs its own icon copy: fonts are separate atlas entries, else icons
        // under fontMedium_ show as notdef boxes.
        if (fontMedium_) mergeIconFont(px);
    }
}

// Merge icon font into the last added font for inline rendering. Missing file is non-fatal.
void SandboxApp::mergeIconFont(f32 px) {
#if AVER_WITH_IMGUI
    const std::string icons = executableDir() + AVER_PATH_SEP "MaterialIcons-Regular.ttf";
    if (!fileExists(icons)) {
        if (!iconWarned_) {
            iconWarned_ = true;
            AVER_WARN("[Sandbox] '{}' missing -- icons will render as empty boxes", icons);
        }
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    // Static: ImGui keeps this pointer; local would dangle. Terminating 0 required.
    static const ImWchar range[] = { editor::kIconRangeFirst, editor::kIconRangeLast, 0 };
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.PixelSnapH = true;
    // Nudge icons down and shrink slightly to align with Roboto baseline.
    cfg.GlyphMinAdvanceX = px;
    cfg.GlyphOffset = ImVec2(0.0f, px * 0.10f);
    io.Fonts->AddFontFromFileTTF(icons.c_str(), px * 0.86f, &cfg, range);
#else
    (void)px;
#endif
}

// Upload branding/logo.png for start screen. Missing file is non-fatal.
void SandboxApp::loadLogo(Engine& e) {
    rhi::IResourceFactory* res = e.device()->resources();
    if (!res) return;

    const std::string path = executableDir() + AVER_PATH_SEP "logo.png";
    ImageData img;
    std::string why;
    if (!decodeImage(path, img, &why)) {
        AVER_WARN("[Sandbox] '{}' not loaded ({}) -- the start screen falls back to a drawn badge",
                  path, why);
        return;
    }
    const int w = static_cast<int>(img.width), h = static_cast<int>(img.height);

    rhi::TextureDesc td;
    td.width = img.width;
    td.height = img.height;
    td.format = rhi::Format::RGBA8Unorm;
    td.bind = rhi::ResourceBind::ShaderResource;
    td.initialState = rhi::ResourceState::ShaderResource;
    td.debugName = "EditorLogo";
    const void* levels[1] = {img.pixels.data()};
    td.initialData = levels;
    td.initialDataCount = 1;
    td.initialRowPitch = img.rowPitch();
    logoTexture_ = res->createTexture(td);

    if (!logoTexture_) { AVER_WARN("[Sandbox] the start-screen mark could not be uploaded"); return; }
    logoUiId_ = e.device()->uiTextureId(logoTexture_);
    if (!logoUiId_) { AVER_WARN("[Sandbox] the start-screen mark is not reachable from the UI"); return; }
    logoAspect_ = h > 0 ? static_cast<f32>(w) / static_cast<f32>(h) : 1.0f;
    AVER_INFO("[Sandbox] start-screen mark decoded from {} ({}x{})", path, w, h);
}

// Upload Compile C# button's three-tile status sprite sheet. Missing file is non-fatal.
void SandboxApp::loadCompileIcon(Engine& e) {
    rhi::IResourceFactory* res = e.device()->resources();
    if (!res) return;

    const std::string path = executableDir() + AVER_PATH_SEP "compile-status.png";
    ImageData img;
    std::string why;
    if (!decodeImage(path, img, &why)) {
        AVER_WARN("[Sandbox] '{}' not loaded ({}) -- the Compile C# button falls back to a drawn dot",
                  path, why);
        return;
    }

    rhi::TextureDesc td;
    td.width = img.width;
    td.height = img.height;
    td.format = rhi::Format::RGBA8Unorm;
    td.bind = rhi::ResourceBind::ShaderResource;
    td.initialState = rhi::ResourceState::ShaderResource;
    td.debugName = "CompileStatusIcons";
    const void* levels[1] = {img.pixels.data()};
    td.initialData = levels;
    td.initialDataCount = 1;
    td.initialRowPitch = img.rowPitch();
    compileIconTexture_ = res->createTexture(td);
    if (!compileIconTexture_) { AVER_WARN("[Sandbox] the Compile C# icon could not be uploaded"); return; }
    compileIconUiId_ = e.device()->uiTextureId(compileIconTexture_);
    if (!compileIconUiId_) { AVER_WARN("[Sandbox] the Compile C# icon is not reachable from the UI"); return; }
    AVER_INFO("[Sandbox] Compile C# status icons decoded from {} ({}x{})", path, img.width, img.height);
}

// ---- The game UI's font ----
// Load from .ocfont file staged beside exe. Missing font is non-fatal.
void SandboxApp::loadGameUiFont(Engine& e) {
    rhi::IResourceFactory* res = e.device()->resources();
    if (!res) return;

    const std::string fontPath = executableDir() + AVER_PATH_SEP "Roboto-Regular.ocfont";
    std::string text;
    if (!readFileText(fontPath, text)) {
        AVER_INFO("[Sandbox] no game-UI font at {} -- the HUD draws without text", fontPath);
        return;
    }
    std::string why;
    if (!ui::parseOcfont(text, uiFont_, &why)) {
        AVER_WARN("[Sandbox] '{}': {}", fontPath, why);
        return;
    }

    // Atlas sits beside .ocfont. Use only the filename (editor's copy is staged flat).
    std::string atlas = uiFont_.atlasPath;
    const usize slash = atlas.find_last_of("/\\");
    if (slash != std::string::npos) atlas = atlas.substr(slash + 1);
    const std::string atlasPath = executableDir() + AVER_PATH_SEP + atlas;

    ImageData img;
    if (!decodeImage(atlasPath, img, &why)) {
        AVER_WARN("[Sandbox] the game-UI font atlas '{}' could not be read ({})", atlasPath, why);
        uiFont_ = ui::UiFont{};   // no atlas means no glyphs worth drawing
        return;
    }
    rhi::TextureDesc td;
    td.width = img.width;
    td.height = img.height;
    td.format = rhi::Format::RGBA8Unorm;
    td.bind = rhi::ResourceBind::ShaderResource;
    td.initialState = rhi::ResourceState::ShaderResource;
    td.debugName = "GameUiFontAtlas";
    const void* levels[1] = {img.pixels.data()};
    td.initialData = levels;
    td.initialDataCount = 1;
    td.initialRowPitch = img.rowPitch();
    uiFontTexture_ = res->createTexture(td);
    if (!uiFontTexture_) { AVER_WARN("[Sandbox] the game-UI font atlas could not be uploaded"); return; }
    // Raw TextureHandle, not uiTextureId(): UiRenderer casts back to rhi::TextureHandle.
    uiFont_.atlasTexture = static_cast<u64>(uiFontTexture_);
    AVER_INFO("[Sandbox] game-UI font '{}' loaded: {} glyph(s), atlas {}x{}",
              uiFont_.name, uiFont_.glyphs.size(), img.width, img.height);
}

// Upload N-tile sprite sheet and measure tile aspect. Returns false on miss.
bool SandboxApp::loadIconSheet(Engine& e, const char* file, int tiles, const char* debugName,
                   rhi::TextureHandle& outTex, u64& outId, f32& outAspect) {
    rhi::IResourceFactory* res = e.device()->resources();
    if (!res) return false;
    const std::string path = executableDir() + AVER_PATH_SEP + file;
    ImageData img;
    std::string why;
    if (!decodeImage(path, img, &why)) {
        AVER_WARN("[Sandbox] '{}' not loaded ({}) -- the Content Browser falls back to drawn glyphs",
                  path, why);
        return false;
    }
    rhi::TextureDesc td;
    td.width = img.width; td.height = img.height;
    td.format = rhi::Format::RGBA8Unorm;
    td.bind = rhi::ResourceBind::ShaderResource;
    td.initialState = rhi::ResourceState::ShaderResource;
    td.debugName = debugName;
    const void* levels[1] = {img.pixels.data()};
    td.initialData = levels; td.initialDataCount = 1; td.initialRowPitch = img.rowPitch();
    outTex = res->createTexture(td);
    if (!outTex) { AVER_WARN("[Sandbox] '{}' could not be uploaded", path); return false; }
    outId = e.device()->uiTextureId(outTex);
    if (!outId) { AVER_WARN("[Sandbox] '{}' is not reachable from the UI", path); return false; }
    outAspect = (img.height > 0 && tiles > 0)
              ? static_cast<f32>(img.width) / static_cast<f32>(tiles) / static_cast<f32>(img.height)
              : 1.0f;
    AVER_INFO("[Sandbox] {} decoded from {} ({}x{}, {} tiles, aspect {:.3f})",
              debugName, path, img.width, img.height, tiles, outAspect);
    return true;
}

#endif

#if AVER_WITH_IMGUI
// Install ImGui backend before uiInit() runs (onInit is too late).
void SandboxApp::onDeviceCreated(Engine& e)  {
    const rhi::Backend which = e.device() ? e.device()->backend() : rhi::Backend::D3D12;
#if AVER_WITH_IMGUI_D3D12
    if (which == rhi::Backend::D3D12) {
        uiBackend_.reset(rhi::d3d12::imgui_backend::create());
        rhi::d3d12::installUiBackend(e.device(), uiBackend_.get());
        return;
    }
#endif
#if AVER_WITH_IMGUI_VULKAN
    if (which == rhi::Backend::Vulkan) {
        uiBackendVk_.reset(rhi::vkb::imgui_backend::create());
        rhi::vkb::installUiBackend(e.device(), uiBackendVk_.get());
        return;
    }
#endif
    (void)which;
}

#endif

// Keeps the title bar in step with the open level (called once a frame from onUpdate). Before
// 2026-09-16, applyProject (SandboxProject.cpp) set the title once to the project's name and never
// touched it again, so different levels -- and unsaved edits -- looked identical. No project open:
// leaves the title alone; BootConfig's constructor default (`c.windowTitle`) already covers that case.
void SandboxApp::refreshWindowTitle(Engine& e) {
    if (!e.window() || !project_.valid()) return;
    // levelName_ first (the world's `name` field, survives the level moving on disk); falls back
    // to the saved file's stem, then "untitled" for a never-saved level -- matching the exit prompt
    // and Save Level As (SandboxShell.cpp).
    //
    // Level half guarded, project half not: levelName_/levelPath_ are `#if AVER_MODULE_SCENE`
    // (every verb that sets them -- load, Save As, New Level -- is too). A scene-less tree can
    // never have a level open, so it falls back to the project-only title this function replaced.
    std::string title = "Aver Engine \xE2\x80\x94 Editor \xE2\x80\x94 " + project_.name;
#if AVER_MODULE_SCENE
    std::string display = levelName_;
    if (display.empty() && !levelPath_.empty())
        display = std::filesystem::path(levelPath_).stem().string();
    if (display.empty()) display = "untitled";
    title += " \xE2\x80\x94 " + display;
    if (levelHasUnsavedEdits()) title += "*";
#endif
    // ONLY WHEN IT CHANGES: setTitle is a Win32 call, and this runs every frame -- most of which
    // change nothing about the level's name or dirty state.
    if (title != windowTitleShown_) {
        windowTitleShown_ = title;
        e.window()->setTitle(windowTitleShown_);
    }
}

#if AVER_MODULE_SCRIPTING
// The graph debugger's per-frame work, and the Problems panel's pass when Play starts.
//
// A breakpoint does not stop the tick (single-threaded engine, the editor draws on the same thread): the
// managed hooks latch it, and this reads the latch once a frame, pauses the session and brings the node
// into view. Resume is the Pause chord and the step is Frame Skip, both unchanged; a node on an OnTick
// chain breaks again every frame after resume until its breakpoint is disabled.
void SandboxApp::pumpGraphDebugger() {
    // Every open graph is checked the moment Play begins, so one that cannot run is flagged now and not
    // only in the log. (Play launching is observed here, not hooked in launchPlay, to leave that alone.)
    const bool playing = playSessionActive();
    if (playing && !graphDiagPlayWas_ && scripts_.graphDiagnoseAvailable()) {
        usize warnings = 0;
        const usize errors = editor::runGraphDiagnosticsForOpenTabs(&warnings);
        if (errors > 0)
            notifyOutcome(editor::NotifySeverity::Warning, "Graph problems",
                          std::to_string(errors) + " error(s) in the open graphs; see each graph's Problems list.");
    }
    graphDiagPlayWas_ = playing;

    if (!scripts_.graphDebugAvailable()) return;
    editor::DebugSession& dbg = editor::debugSession();
    if (dbg.needsArmPush()) {
        scripts_.graphDebugSetEnabled(dbg.armed);
        dbg.markArmPushed();
    }
    if (dbg.needsBreakpointPush()) {
        scripts_.graphDebugSetBreakpoints(dbg.breakpoints.wire());
        dbg.markBreakpointsPushed();
    }
    if (!dbg.armed) return;

#if AVER_MODULE_FRAMEWORK
    // A session that is running again has resumed past whatever it last broke on.
    if (aver_fw_play_state() == AVER_FW_PLAY_PLAYING) dbg.lastBreak.reset();
#endif

    std::string latched;
    editor::BreakEvent hit;
    if (!scripts_.graphDebugTakeBreak(latched) || !editor::parseBreakEvent(latched, hit)) return;
    // A breakpoint disabled or removed since it latched is not a stop.
    if (!dbg.breakpoints.enabled(hit.graph, hit.node)) return;
    dbg.lastBreak = hit;
#if AVER_MODULE_FRAMEWORK
    aver_fw_set_paused(1);
#endif
    std::string path = editor::graphPathForName(hit.graph);
    if (path.empty() && project_.valid()) path = editor::findGraphFileByName(project_.contentDir(), hit.graph);
    if (!path.empty()) editor::revealGraphNode(path, hit.node);
    notifyOutcome(editor::NotifySeverity::Info, "Breakpoint hit",
                  "'" + hit.node + "' in " + hit.graph + ". Play is paused at the end of this frame; "
                  "Frame Skip steps and the Pause chord resumes.");
}
#endif  // AVER_MODULE_SCRIPTING

// Tears down the editor: MCP, prefs, physics, UI textures, materials, render features, scripts.
// Returns non-zero only when a requested test mode did not pass.
int SandboxApp::exitCode() const  {
    if (skinScene_) {
        if (!skinScene_->finished()) {
            AVER_ERROR("[Skin] --skin-scene-test did not finish; reporting failure rather than "
                       "letting an unfinished run look like a pass");
            return 2;
        }
        if (!skinScene_->passed()) return 1;
    }
    // Read from latched scalars, not objects: onShutdown destroys both before exitCode() is called.
    if (skinSelfTestExit_ > 0) {
        AVER_ERROR("[Skin] --skin-test reported {}", skinSelfTestExit_ == 2 ? "no verdict (it did "
                   "not finish)" : "FAIL");
        return skinSelfTestExit_;
    }
    if (skinDrawExit_ > 0) {
        AVER_ERROR("[Skin] --skin-draw-test reported {}", skinDrawExit_ == 2 ? "no verdict (it did "
                   "not finish)" : "FAIL");
        return skinDrawExit_;
    }
#if AVER_MODULE_SCENE
    if (const int rc = sequenceRunExit()) return rc;
#endif
    return 0;
}

void SandboxApp::onShutdown(Engine& e)  {
    closePlayWindow();   // its swapchain must go before the device does
    nrd2Session_.shutdown();   // a running NRD2 training session saves its checkpoint while the device lives
#if AVER_MODULE_MCP
    mcp_.stop();
#endif
    setLogSink(nullptr, nullptr);
#if AVER_MODULE_VOXI
    // Flush baked GI to disk; volumes buffer in RAM until budget exceeded or shutdown.
    if (const u32 wrote = voxiRenderer_.giCacheFlush())
        AVER_INFO("[Editor] wrote {} buffered GI cache entr(ies) on shutdown", wrote);
#endif
    // ---- Capture live state before flushing it ----
#if AVER_MODULE_SCENE
    // Closing the editor is a way out of the open level that never reaches unloadLevel.
    storeLevelView();
#endif
    // Sync live members to disk; several settings bypass this (mouse wheel, toolbar, drawer grip).
#if AVER_WITH_IMGUI
    saveEditorPreferences();
#endif
    editor::flushEditorPrefs();
    // Flush project manifest; project autosave has 0.5s debounce, so immediate exit loses edits.
    // maxFrames_ guard: --frames sets render settings, so unguarded flush would save capture flags into manifest.
    if (maxFrames_ == 0 && projectDirty_ && project_.valid()) {
        std::string why;
        if (!saveProjectManifest(&why))
            AVER_WARN("[Project] could not flush unsaved settings on exit: {}", why);
    }
    editor::shutdownActorEditors();
    editor::shutdownInstanceSetEditors();
editor::shutdownAnimEditors();
#if AVER_MODULE_PARTICLES
    editor::shutdownParticleEditors();
#endif
    setMouseCaptured(false);
#if AVER_MODULE_FLUIDS
    // Before aver_phys_shutdown: retire water volumes while the solver is live.
    water_.shutdown(e.device());
#endif
#if AVER_FLUIDS_SIMULATED
    if (viewportIconsReady_) {
        e.device()->removeRenderFeature(&viewportIcons_);
        viewportIcons_.shutdown();
        viewportIconsReady_ = false;
    }
#endif
    // Remove render features before the device tears down.
#if AVER_WITH_SYNAPSE_AI && AVER_MODULE_SYNAPSE_GPU
    if (crowdGpuAttached_) { game::removeCrowdGpu(e.device(), crowdGpu_); crowdGpuAttached_ = false; }
#endif
    e.device()->removeRenderFeature(&gbufferDebugFeature_);
    gbufferDebugFeature_.shutdown();
    e.device()->removeRenderFeature(&neurafiViz_);
    neurafiViz_.setSource(nullptr, 0.0f);
    neurafiViz_.shutdown();
#if AVER_MODULE_PHYSICS
#if AVER_MODULE_SCENE
    // Stop Play before closing the window; vehicle bodies/constraints come down here.
    vehicles_.end();
#endif
#if AVER_MODULE_RAGDOLL
    ragdoll::ragdollSystem().clear();
#endif
    aver_phys_shutdown();
#endif
#if AVER_WITH_AUDIO_ABI
    // Stops mixer, releases device, forgets loaded sounds.
    aver_audio_shutdown();
#endif
#if AVER_WITH_IMGUI
    if (logoTexture_ || compileIconTexture_ || fileIconsTexture_ || folderIconsTexture_) {
        if (rhi::IResourceFactory* res = e.device()->resources()) {
            res->waitIdle();
            if (logoTexture_) res->destroyTexture(logoTexture_);
            if (compileIconTexture_) res->destroyTexture(compileIconTexture_);
            if (fileIconsTexture_) res->destroyTexture(fileIconsTexture_);
            if (folderIconsTexture_) res->destroyTexture(folderIconsTexture_);
        }
        logoTexture_ = 0; logoUiId_ = 0;
        compileIconTexture_ = 0; compileIconUiId_ = 0;
        fileIconsTexture_ = 0; fileIconsUiId_ = 0;
        folderIconsTexture_ = 0; folderIconsUiId_ = 0;
    }
#endif
#if AVER_MODULE_PBR
    releaseProjectMaterials();
    content_.setTextureFactory(nullptr);
    content_.setMaterialSystem(nullptr);
#endif
#if AVER_MODULE_SR
    // Detach from device first, then destroy: raw pointers must be cleared before unique_ptrs reset.
    e.device()->setUpscaler(nullptr);
    averSrUpscaler_.reset();
    taaUpscaler_.reset();
    neuraa_.reset();   // holds the device's factory: must go while the device lives (it crashed in ~SandboxApp)
#endif
    game::uiTextShutdown();   // unbind the atlas from gameUi_ and close the fonts before the renderer and device go
    // Same order: clear device pointer before destroying the object.
    e.device()->setFrameInterpolation(false);
    e.device()->setFrameInterpolator(nullptr);
    frameInterpolator_.reset();
    if (gameUi_) {
        e.device()->removeRenderFeature(gameUi_);
        delete gameUi_;
        gameUi_ = nullptr;
    }
    if (skinSelfTest_) {
        // Latch verdict before reset; Engine::run calls exitCode() after onShutdown.
        skinSelfTestExit_ = !skinSelfTest_->finished() ? 2 : (skinSelfTest_->passed() ? 0 : 1);
        e.device()->removeRenderFeature(skinSelfTest_.get());
        skinSelfTest_.reset();
    }
    if (ptFurnace_) {
        e.device()->removeRenderFeature(ptFurnace_.get());
        ptFurnace_.reset();
    }
    // GBufferDebugFeature always registered, so always unregister here.
    if (gbufferDebugAttached_) {
        e.device()->removeRenderFeature(&gbufferDebugFeature_);
        gbufferDebugAttached_ = false;
    }
    // Routed through the same reconciler the editor's live toggle uses.
    ptSceneViewWantEnabled_ = false;
    syncPtSceneView(e.device());
    if (skinDraw_) {
        // Latch verdict before reset.
        skinDrawExit_ = !skinDraw_->finished() ? 2 : (skinDraw_->passed() ? 0 : 1);
        e.device()->removeRenderFeature(skinDraw_.get());
        skinDraw_.reset();
    }
#if AVER_MODULE_SCENE
    if (skinnedScene_) {
        e.device()->removeRenderFeature(skinnedScene_.get());
        skinnedScene_.reset();
    }
#if AVER_MODULE_RENDER_SOFTBODY
    // While the device lives: ~SoftBodyScene frees its draw meshes through dev_, which is gone after onShutdown.
    if (softBodyScene_) {
        e.device()->removeRenderFeature(softBodyScene_.get());
        softBodyScene_.reset();
    }
#endif
    // copyFeature() exposed for this call; host owns the copy pass.
    if (thumbnails_.ready()) e.device()->removeRenderFeature(thumbnails_.copyFeature());
    thumbnails_.shutdown();
#endif
#if AVER_MODULE_VOXI
    if (voxiAttached_) { e.device()->removeRenderFeature(&voxiRenderer_); voxiAttached_ = false; }
    voxiRenderer_.shutdown();
#else
    (void)e;
#endif
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // Prints CPU simulation cost measured this run, before teardown.
    if (particleTickFrames_ > 0) {
        u32 totalLive = 0, liveEmitters = particles::particleSystem().liveEmitters();
        particles::particleSystem().forEachEmitter([&](const particles::EmitterView& ev) {
            totalLive += ev.particles ? static_cast<u32>(ev.particles->size()) : 0;
        });
        AVER_INFO("[Particles] CPU tick: {:.2f}us/frame avg over {} frame(s) ({:.3f}ms total), "
                  "{} emitter(s) / {} live particle(s) at shutdown",
                  (particleTickAccumSec_ / static_cast<f64>(particleTickFrames_)) * 1e6,
                  particleTickFrames_, particleTickAccumSec_ * 1e3, liveEmitters, totalLive);
    }
    if (particlesAttached_) { e.device()->removeRenderFeature(&particleRenderer_); particlesAttached_ = false; }
    content_.releaseParticleTextures();
    particleRenderer_.shutdown();
#endif
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
    if (occluder_) {
        if (rhi::IResourceFactory* occRes = e.device()->resources())
            aver::occlusion::destroyOcclusionCuller(*occRes, occluder_);
        occluder_ = nullptr;
    }
#endif
#if AVER_MODULE_SCRIPTING
    scripts_.shutdown();
#endif
    AVER_INFO("[Sandbox] shutdown");
}

void SandboxApp::setVSyncOff(bool off) { vsyncOffRequested_ = off; }

// Frame interpolation decision; see docs/rendering/NEURAFI.md decision 8.
// --frame-interp wins; otherwise Play follows project RENDER.FRAMEINTERP, editing follows Editor Preference.
bool SandboxApp::updateFrameInterpolation(rhi::IDevice* dev) {
    if (!dev) return false;
#if AVER_MODULE_FRAMEWORK
    const bool playing = anyPlayActive();
#else
    const bool playing = false;
#endif
    bool want = frameInterpCli_ >= 0 ? frameInterpCli_ >= 1
              : playing           ? project_.frameInterp == 1
                                  : frameInterpWhileEditing_;
    dev->setFrameInterpCaptureGenerated(frameInterpCli_ == 2);
    if (want && !frameInterpolator_) {
        if (dev->resources()) {
            frameInterpolator_ = std::make_unique<neurafi::NeuraFI>(*dev);
            // Trained weights: user's in editor.ini (per machine), else shipped in bin/data.
            const std::string dir = aver::userDataDir();
            if (!dir.empty())
                frameInterpolator_->setWeightsPath(dir + AVER_PATH_SEP + neurafi::NeuraFI::kWeightsFileName);
            frameInterpolator_->setShippedWeightsPath(aver::executableDir() + AVER_PATH_SEP "data" AVER_PATH_SEP +
                                                   neurafi::NeuraFI::kWeightsFileName);
            dev->setFrameInterpolator(frameInterpolator_.get());
        } else {
            want = false;
        }
    }
    if (frameInterpolator_) {
        const int traj = frameInterpTrajectoryCli_ >= 0 ? frameInterpTrajectoryCli_ : frameInterpTrajectory_;
        frameInterpolator_->setTrajectory(static_cast<neurafi::Trajectory>(traj < 0 ? 0 : (traj > 2 ? 2 : traj)));
        frameInterpolator_->setTraining(frameInterpTrainCli_ || frameInterpTrain_);
        // Window > Neural Visualiser.
        const int vm = neurafiVizMode_ < 0 ? 0 : (neurafiVizMode_ > 4 ? 4 : neurafiVizMode_);
        frameInterpolator_->setVisualisation(static_cast<neurafi::Visualisation>(vm), neurafiVizScalePx_);
    }
    neurafiViz_.setDevice(dev);
    neurafiViz_.setViewportRect(static_cast<u32>(vpX_), static_cast<u32>(vpY_),
                                static_cast<u32>(vpW_), static_cast<u32>(vpH_));
    neurafiViz_.setSource(frameInterpolator_ && neurafiVizMode_ > 0 ? frameInterpolator_.get() : nullptr,
                          neurafiVizOpacity_);
    dev->setFrameInterpShowGeneratedOnly(neurafiShowGeneratedOnly_);
    dev->setFrameInterpolation(want);
    return want;
}

} // namespace aver
