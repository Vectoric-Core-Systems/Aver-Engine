// Lifecycle: construction, splash/log sink, config, DPI/fonts, onInit, onUpdate, onShutdown.
// Split out of the single 29,952-line SandboxApp.cpp 2026-09-16 by moving method bodies verbatim;
// class declared in SandboxApp.hpp.

// The one translation unit that compiles stb_image_write's implementation.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#undef STB_IMAGE_WRITE_IMPLEMENTATION
#include "SandboxApp.hpp"
#include "TextureEditor.hpp"   // makeTextureEditor; SandboxApp.hpp does not pull this one in
#include "aver/game/GameCamera.hpp"
#include "aver/game/GameTick.hpp"
#include "aver/platform/FileSystem.hpp"   // userDataDir: where the trajectory network's weights live

namespace aver {
SandboxApp::SandboxApp(u64 maxFrames, bool headless, std::string beamPath, std::string shot, Tool initialTool)
    : maxFrames_(maxFrames), headless_(headless), beamPath_(std::move(beamPath)), shot_(std::move(shot)), initialTool_(initialTool) {
    setLogSink(&SandboxApp::logSink, this);
}

// Appends one engine log line to the Output Log buffer. Must not itself log: the core log mutex is held.
// Trims a log line for a single-line splash: keeps the subsystem tag ("[Material] foo.ocmat" says
// more than "foo.ocmat") but drops the level prefix and path directories -- DT_END_ELLIPSIS clips
// from the right, so a full path would hide the filename behind the drive letter.
 std::string SandboxApp::splashTextFor(std::string_view msg) {
    std::string t(msg);
    if (!t.empty() && t[0] == '[') {                       // drop the level prefix, keep the tag
        const usize close = t.find(']');
        if (close != std::string::npos) t.erase(0, close + 1);
    }
    while (!t.empty() && t.front() == ' ') t.erase(0, 1);
    // Keeps the last 3 path segments (e.g. "JungleRuins/Sponza/arch_stones_01.ocmesh") so the
    // splash shows which asset is loading, not just a bare filename. Handles both separators.
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
    self->logLines_.push_back({level, std::string(msg)});
    if (self->logLines_.size() > kMaxLogLines) self->logLines_.pop_front();

    // Forwards Info+ lines (trimmed via splashTextFor) to whichever loading screen is active --
    // the engine splash borrowed via a CLI project, or applyProject's own for a browser-opened one
    // -- so it shows the actual asset loading instead of a handful of fixed stage names.
    // MAIN THREAD ONLY: splash GDI objects belong to the thread that called show(), and Jolt wires
    // JPH::Trace straight into AVER_INFO from its job pool; a dropped worker-thread line is just
    // replaced by the next one.
    // MUST NOT LOG: runs under the core log mutex (non-recursive); a single AVER_* call from in
    // here has already deadlocked this file once. Nothing below logs.
    if (level >= LogLevel::Info && std::this_thread::get_id() == self->mainThreadId_) {
        const std::string text = splashTextFor(msg);
        if (self->projectLoading_) self->projectLoading_->stage(text.c_str());
        else if (self->engineForSplash_ && self->engineForSplash_->loadingScreenActive())
            self->engineForSplash_->setLoadingStatus(text);
    }

    // Graph "Print" nodes get an on-screen overlay (see drawGraphPrintOverlay) instead of being
    // lost in the Output Log. Filtered here on GraphInterop's "[Graph] " prefix -- cheaper than
    // scanning at draw time; rfind(x,0)==0 is an allocation-free "starts with".
    // No timestamp here (mutex held, must stay quick) -- the draw call stamps it instead, which
    // is also the clock the fade needs to agree with.
    if (msg.rfind("[Graph] ", 0) == 0) {
        std::string text(msg.substr(8));
        // Collapses consecutive duplicate prints into one line with a rising count instead of
        // flooding the buffer -- an OnTick PrintString fires every frame (60 lines/sec unfiltered).
        // Consecutive only (not deduped across the buffer) so an alternating Branch still shows
        // both arms. NOT gated by "level open": HostBridge.GraphTickBoundInstances is ungated
        // internally; the caller gates it (tickGraphClassInstances in this file, on
        // aver_fw_play_state()==AVER_FW_PLAY_PLAYING) -- ungated, this measured 4003 tick lines
        // and a VAR climbing to 12.31s with nobody pressing Play.
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

    // Errors/criticals also become notifications, from this sink (setLogSink is a single global
    // slot) rather than a second one. Three locks deep here (core log mutex, logMutex_, queue's
    // own) -- pushFromLog's own contract governs what it may do; a stray AVER_* call here deadlocks.
    editor::notifications().pushFromLog(level, msg);
}

// Returns the boot configuration for the editor window.
BootConfig SandboxApp::config() const  {
    BootConfig c; c.windowTitle="Aver Engine \xE2\x80\x94 Editor"; c.windowWidth=1600; c.windowHeight=900;
    c.maxFrames=maxFrames_; c.headless=headless_; c.useWarp=useWarp_;
    // Windowed by default; borderless fullscreen only when --fullscreen asks for it explicitly
    // (flipped from the old default, an interactive run starting borderless-fullscreen, which
    // covers the taskbar). maxFrames_==0 check stays: every recorded gate baseline was measured
    // via a bounded --frames capture at a fixed-rect pixel probe, and must not be reshaped even
    // by --fullscreen.
    c.fullscreen = fullscreenOverride_ && maxFrames_ == 0;
    c.enableDebugLayer=debugLayer_;
    c.backend = backendName_.empty() ? nullptr : backendName_.c_str();
    return c;
}

// Has the project reached the screen? (Application::startupComplete override.)
// No project requested: one rendered frame is enough. With a project: lastSceneDrawn_ is set on
// every walked frame (>=0 = walked, >0 = drew something) and must hold STEADY for kSettleFrames
// frames before returning true -- a project streams meshes in over many frames, so "drew one
// thing" fires while the rest of the level is still arriving. Frame count, not wall-clock: that's
// the unit the wait is measured in; still bounded by the engine's own warm-up cap (600 frames/20s).
// The guard is around the body, not the function, since this override must exist in every build.
bool SandboxApp::startupComplete() const  {
#if AVER_MODULE_SCENE
    if (lastSceneDrawn_ < 0) return false;             // no frame has walked the scene yet
    // project_ (not projectPath_, which is only the CLI arg) tracks the live project including
    // ones opened from the browser -- see handleOpenRequest, same reasoning. Using projectPath_
    // here previously cured the midway-disappearance bug for the CLI path only and left the
    // browser path with it. A failed open also leaves this empty, so it falls out here too
    // rather than waiting on the warm-up cap.
    if (project_.manifestPath.empty()) return true;    // no project is loading
    if (lastSceneDrawn_ == 0) return false;            // walked, but nothing has drawn yet
    constexpr int kSettleFrames = 8;
    // Mutable: ticked from this call itself (once per warm-up frame), with nowhere else to hook in.
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

// --frame-budget <ms>: target frame time, and run the controller even in a bounded capture.
void SandboxApp::setFrameBudget(f32 ms) { frameBudgetMs_ = ms; frameBudgetForced_ = ms > 0.0f; }

void SandboxApp::setDebugLayer(bool d) { debugLayer_ = d; }

#if AVER_WITH_IMGUI
// Rebuilds the ImGui style and font atlas for the given DPI scale.
void SandboxApp::applyDpi(f32 dpi) {
    dpi_ = dpi;
    // Pushed not pulled: AssetEditor::draw() takes no dpi arg, and widening it for one subclass
    // isn't worth it. Follows ActorEditor's content-root-setter precedent.
    editor::setGraphEditorDpi(dpi_);
    applyEditorMetrics();
    applyEditorColors();
    if (dpi_ > 1.01f) ImGui::GetStyle().ScaleAllSizes(dpi_);

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    fontMedium_ = nullptr;

    const std::string dir = executableDir();
    const std::string regular = dir + "\\Roboto-Regular.ttf";
    const std::string medium  = dir + "\\Roboto-Medium.ttf";
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

// Merges the icon font into whichever font was added last, so an icon sits inline with text (one
// draw call, one hit-test rect). Safe: glyphs live in the Private Use Area (U+E000-U+F8FF). A
// missing file is a warning, not a failure -- icons render as notdef boxes rather than refusing to start.
void SandboxApp::mergeIconFont(f32 px) {
#if AVER_WITH_IMGUI
    const std::string icons = executableDir() + "\\MaterialIcons-Regular.ttf";
    if (!fileExists(icons)) {
        if (!iconWarned_) {
            iconWarned_ = true;
            AVER_WARN("[Sandbox] '{}' missing -- icons will render as empty boxes", icons);
        }
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    // static: ImGui keeps this pointer rather than copying it; a local would dangle after return.
    // Terminating 0 required.
    static const ImWchar range[] = { editor::kIconRangeFirst, editor::kIconRangeLast, 0 };
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.PixelSnapH = true;
    // Icons are drawn on the text baseline and read a touch large beside Roboto at the same
    // size; nudging them down and shrinking slightly seats them on the same optical line.
    cfg.GlyphMinAdvanceX = px;
    cfg.GlyphOffset = ImVec2(0.0f, px * 0.10f);
    io.Fonts->AddFontFromFileTTF(icons.c_str(), px * 0.86f, &cfg, range);
#else
    (void)px;
#endif
}

// Uploads branding/logo.png for the start screen. A missing file is a warning, not a failure.
void SandboxApp::loadLogo(Engine& e) {
    rhi::IResourceFactory* res = e.device()->resources();
    if (!res) return;

    const std::string path = executableDir() + "\\logo.png";
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

// Uploads the Compile C# button's three-tile status sprite sheet. A miss is a warning.
void SandboxApp::loadCompileIcon(Engine& e) {
    rhi::IResourceFactory* res = e.device()->resources();
    if (!res) return;

    const std::string path = executableDir() + "\\compile-status.png";
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
// Aver.UI could not draw a character until now. Staged beside the exe like splash.png/icon
// sheets, loaded the same way (decodeImage + createTexture + uiTextureId).
// Non-fatal: a missing font leaves uiFont_ invalid and addText draws nothing -- quieter beats refusing to start.
void SandboxApp::loadGameUiFont(Engine& e) {
    rhi::IResourceFactory* res = e.device()->resources();
    if (!res) return;

    const std::string fontPath = executableDir() + "\\Roboto-Regular.ocfont";
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

    // The atlas sits beside the .ocfont. Its ATLAS key is content-relative for a project that
    // ships one; the editor's own copy is staged flat, so only the file name is used.
    std::string atlas = uiFont_.atlasPath;
    const usize slash = atlas.find_last_of("/\\");
    if (slash != std::string::npos) atlas = atlas.substr(slash + 1);
    const std::string atlasPath = executableDir() + "\\" + atlas;

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
    // Raw TextureHandle, not uiTextureId(): UiRenderer casts c.texture straight back to
    // rhi::TextureHandle, a different channel from ImGui's descriptor-based ImTextureID (which
    // caused "[RHI.D3D12] setSrv with an invalid handle" here).
    uiFont_.atlasTexture = static_cast<u64>(uiFontTexture_);
    AVER_INFO("[Sandbox] game-UI font '{}' loaded: {} glyph(s), atlas {}x{}",
              uiFont_.name, uiFont_.glyphs.size(), img.width, img.height);
}

// Uploads an N-tile sprite sheet staged next to the exe and measures its tile aspect. False on any miss.
bool SandboxApp::loadIconSheet(Engine& e, const char* file, int tiles, const char* debugName,
                   rhi::TextureHandle& outTex, u64& outId, f32& outAspect) {
    rhi::IResourceFactory* res = e.device()->resources();
    if (!res) return false;
    const std::string path = executableDir() + "\\" + file;
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

#if AVER_WITH_IMGUI || AVER_WITH_IMGUI_VULKAN
// Plugs the editor's Dear ImGui backend into the device before Engine::run's uiInit() runs --
// onInit() is too late (see Application::onDeviceCreated). Chosen by what the device actually is:
// both installUiBackend functions verify the backend tag and no-op otherwise.
void SandboxApp::onDeviceCreated(Engine& e)  {
    const rhi::Backend which = e.device() ? e.device()->backend() : rhi::Backend::D3D12;
#if AVER_WITH_IMGUI
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

// Builds the editor: asset editors, MCP, physics, the placeholder scene, gizmos, and the render features.
void SandboxApp::onInit(Engine& e)  {
    // FIRST, and before anything that logs: from here on every main-thread log line also
    // names itself on the startup splash. See logSink for why the thread is checked there.
    engineForSplash_ = &e;
    mainThreadId_ = std::this_thread::get_id();
    AVER_INFO("[Sandbox] backend={} adapter='{}'", rhi::backendName(e.device()->backend()), e.device()->adapterName());
    // Latched: Project Settings shows this beside the requested backend but has no Engine& to re-ask.
    runningBackend_ = rhi::backendName(e.device()->backend());
#if AVER_MODULE_SR
    // AverSR quality drives the same renderScaleOverride_ knob as --render-scale; an explicit
    // --render-scale still wins (1.0 sentinel, same precedent as loadEditorPreferences()). No-op at Off.
    if (averSrQuality_ != aver::sr::Quality::Off && renderScaleOverride_ == 1.0f)
        renderScaleOverride_ = aver::sr::renderScaleFor(averSrQuality_);
#endif
    // --render-scale F: applied once here, before anything sizes off the device. 1.0 (no flag) is
    // a no-op; setRenderScale clamps to [0.25,1], stored for createSwapchainResources if no swapchain yet.
    if (renderScaleOverride_ != 1.0f) {
        e.device()->setRenderScale(renderScaleOverride_);
        AVER_INFO("[Sandbox] render scale {:.2f} (--render-scale)", e.device()->renderScale());
    }
#if AVER_MODULE_SR
    // Constructs SpatialUpscaler (a real GPU-resource-owning object, unlike renderScaleOverride_
    // above) whenever a non-Off level was requested; see logAverSrActive() for its current limits.
    if (averSrQuality_ != aver::sr::Quality::Off) {
        ensureAverSrUpscaler(e.device());
        logAverSrActive(e.device());
    }
    // --edge-aa shares AverSR's upscaler slot; see edgeAaEnabled_'s member comment for who wins
    // when both are requested. Off (default): no-op, bit-identical to a build without the flag.
    if (edgeAaEnabled_) ensureEdgeAaUpscaler(e.device());
#endif
    // --depth-prepass: same-frame depth-only pass ahead of the opaque colour walk -- see the
    // entity loop's "depth prepass phase" comment for the two-walk mechanism. No-op on any
    // backend that doesn't implement depthPrepassPipeline().
    if (depthPrepassOverride_) {
        e.device()->setDepthPrepassEnabled(true);
        AVER_INFO("[Sandbox] depth prepass enabled (--depth-prepass)");
    }

    // Registered unconditionally (mode defaults to Off) so the view-mode dropdown can enable it
    // live without a second registration; whether the G-buffer is written is decided per-frame in
    // onUpdate (gbufferOverride_).
    e.device()->addRenderFeature(&gbufferDebugFeature_);
    gbufferDebugAttached_ = true;

    // Registration order is precedence: the first factory that accepts a path wins.
    assetEditors_.registerFactory(&editor::makeMeshEditor);
    assetEditors_.registerFactory(&editor::makeActorEditor);
    assetEditors_.registerFactory(&editor::makeAnimEditor);
    // APPENDED, not inserted: AssetEditorHost::open() tries factories in registration order, so
    // moving this ahead of the others would change which editor claims a file they both accept.
    assetEditors_.registerFactory(&editor::makeGraphEditor);
    // Wires the graph validator (Graph.Validate()/OcGraphParser, ~30 named node errors) into the
    // editor via a lambda over scripts_ rather than a direct dependency, so GraphEditor keeps its
    // Core+Formats+ImGui-only dependency set and stays drivable headless with no .NET runtime.
    // graphValidateAvailable() is false against a bridge predating the GraphValidate export, so an
    // old bridge greys the button rather than claiming every graph is fine.
    //
    // Guarded on AVER_MODULE_SCRIPTING along with the three calls below it, since the validator and
    // hit table are answers only managed code can give. With scripting off, GraphEditor is still
    // registered and still opens/edits .ocgraph -- it just never gets setGraphValidator/
    // setGraphNodeHitSource, so Validate greys out and the node-hit overlay draws nothing.
#if AVER_MODULE_SCRIPTING
    editor::setGraphValidator([this](const std::string& text, std::string& err) {
        if (!scripts_.graphValidateAvailable()) { err = "the .NET bridge exports no GraphValidate"; return false; }
        return scripts_.graphValidate(text, err);
    });
    // Node-hit recording armed once here (not per tab): the managed table is keyed by graph name
    // and costs only a static bool test when off; a packaged game (no editor) never arms it.
    editor::setGraphNodeHitSource([this](const std::string& graphName, f32 maxAge,
                                         std::vector<std::pair<std::string, f32>>& out) {
        scripts_.graphNodeHits(graphName, maxAge, out);
    });
    scripts_.graphSetHitRecording(true);
#endif  // AVER_MODULE_SCRIPTING
    // Appended for the same reason; claims only .ocbt, which nothing above accepts.
    assetEditors_.registerFactory(&editor::makeBtEditor);
    // Appended for .ocsnd, likewise unclaimed above. See SoundEditor.hpp.
    assetEditors_.registerFactory(&editor::makeSoundEditor);
    // Appended for .ocparticle -- format/runtime/placement existed with no authoring UI until now.
    // See ParticleEditor.hpp.
#if AVER_MODULE_PARTICLES
    assetEditors_.registerFactory(&editor::makeParticleEditor);
#endif
    // Appended for .ocfoliage -- the eighth factory, unconditional unlike particle above. See
    // FoliageTypeEditor.hpp/OcFoliage.hpp.
    assetEditors_.registerFactory(&editor::makeFoliageTypeEditor);
    // Appended for .ocinput -- the ninth factory, also unconditional. See InputSchemeEditor.hpp/OcInput.hpp.
    assetEditors_.registerFactory(&editor::makeInputSchemeEditor);
    // Appended for the image formats decodeImage can read -- the tenth factory, read-only,
    // unconditional; see TextureEditor.hpp for why .dds isn't among them.
    assetEditors_.registerFactory(&editor::makeTextureEditor);
    {
        // Answers "is this the project's Input Scheme?" and can set it; a hooks struct rather than
        // a project pointer on AssetEditor -- see InputSchemeEditorHooks' comment (InputSchemeEditor.hpp).
        editor::InputSchemeEditorHooks hooks;
        hooks.contentDir = [this] { return project_.contentDir(); };
        hooks.projectInputScheme = [this] { return project_.inputScheme; };
        hooks.useAsProjectInputScheme = [this](const std::string& contentRelativePath) {
            if (!project_.valid()) return false;
            // Same projectDirty_ flag Project Settings > Description sets (SandboxSettings.cpp);
            // the write happens on the autosave timer or that page's Save button, not here.
            project_.inputScheme = contentRelativePath;
            projectDirty_ = true;
            return true;
        };
        editor::setInputSchemeEditorHooks(std::move(hooks));
    }
    // Must run before any actor factory: the "is Roslyn available" answer is cached on first ask.
    locateAverDesign();
    {
        editor::ActorEditorHooks hooks;
        hooks.compileScripts = [this] { tools_.triggerToolbarCompile(project_); };
        hooks.compileBusy    = [this] { return tools_.compiling(); };
        hooks.drawCompileButton = [this] {
            tools_.drawCompileButton(project_, dpi_, compileIconUiId_);
        };
#if AVER_WITH_IMGUI
        hooks.openInIde      = [this](const std::string& p) {
            const editor::IdeInfo& ide = cbIde();
            if (!editor::openInIde(ide, p)) AVER_WARN("[Editor] could not open {} in {}", p, ide.name);
        };
        hooks.ideName = cbIde().name;
#endif  // AVER_WITH_IMGUI -- openInIde/ideName need the content browser's IDE picker; without a
        // UI there is no picker and no actor editor to open a file from in the first place.
        editor::setActorEditorHooks(std::move(hooks));
    }
    window_ = e.window();
    // The one event sink the editor installs (identical to GameApp's); everything lands in the
    // accumulator unfiltered -- filtering is policy, decided per-consumer further down.
    if (window_) window_->setEventCallback(&sandboxWindowEvent, &input_);
    // Drag-drop from Explorer, off by default in Window itself (Window::setAcceptDroppedFiles) --
    // the editor is the one host that wants it. Polled once a frame in onUpdate.
    if (window_) window_->setAcceptDroppedFiles(true);

    // Single-instance forwarding receiver. singleInstanceEligible_ is by construction a superset
    // of the sender's forward-attempt gate (argc==2, argv[1] not a flag), so no separate
    // bookkeeping is needed. A --frames capture is a real windowed launch (Window.hpp:17) but
    // always carries a flag, so it's ineligible -- concurrent captures never race on the same
    // named mutex.
    if (window_ && window_->valid() && singleInstanceEligible_) {
        Window::registerAsSingleInstancePrimary(window_->nativeHandle());
        // Guarded here (unlike the registration above) because the hook is the level-open path:
        // onOpenRequestThunk -> handleOpenRequest -> requestOpenLevel (SandboxLevelEdit.cpp), all
        // needing a world to instantiate into. A scene-less tree still stays primary instance --
        // a second launch focuses this window, it just has nothing to open once here.
#if AVER_MODULE_SCENE
        window_->setOpenRequestHook(&SandboxApp::onOpenRequestThunk, this);
#endif
    }
    // The window's X button goes through the same unsaved-changes check as File > Exit (it didn't:
    // WM_CLOSE set shouldClose_ before Engine::run's next frameStep could draw the prompt).
    // Unconditional, not behind singleInstanceEligible_: a --frames capture wants this guard to
    // exist and say yes.
    if (window_ && window_->valid()) window_->setCloseGuard(&SandboxApp::onCloseGuardThunk, this);

#if AVER_MODULE_MCP
    // 0 = --mcp was never given. mcpStart (SandboxMcp.cpp) does ABI registration, widget hooks and
    // the listen itself, shared with the status-bar Start button so the channel starts only one way.
    if (mcpPort_ && !mcpStart(mcpPort_))
        AVER_WARN("[Mcp] --mcp was given but the channel did not start; the editor is "
                  "unaffected and carries on");
#endif

    browser_.init();

#if AVER_MODULE_PHYSICS
    // Must start before any level loads: loading builds a static body per colliding placement.
    game::startPhysics("Sandbox");
#endif

#if AVER_WITH_AUDIO_ABI
    // Opens the audio device -- see GameTick.hpp for why "no output device" stays quiet.
    game::startAudio("Sandbox");
#endif

#if AVER_MODULE_SYNAPSE_SCENE
    // Mirrors GameApp::onInit's own placement: before any project opens, needs no level and no
    // physics. registerComponent is idempotent, so re-entering onInit (there is no such path
    // today, but nothing here assumes otherwise) would not re-register a second CSynapseAgent.
    synapse::agentSystem().registerComponents(scene::World::instance());
    synapse::perceptionSystem().registerComponents(scene::World::instance());
    synapse::btSystem().registerComponents(scene::World::instance());
    synapse::registerBuiltinBehaviors(synapse::btSystem());
#endif

#if AVER_MODULE_SCENE
    // Control rig: registered AND installed here (before any project opens, needing no level) so
    // it isn't a component type nothing reads. Installed unconditionally, not lazily on first rig,
    // since the modifier is one cheap component lookup per animated entity per tick and a lazy
    // install would leave a runtime-attached rig doing nothing until noticed.
    anim::controlRigSystem().registerComponents(scene::World::instance());
    anim::controlRigSystem().install(anim::animSystem(), scene::World::instance());
#endif

    // Moved up from later in onInit, where the same code silently disabled the whole GPU
    // per-cluster path for the process lifetime (loadProjectMeshes gates meshClusterGpu_ on
    // lodMeshShaderEnabled_, and ensureLodMeshPipeline latched "already tried" before the flag
    // was even tested, with no warning).
#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
    // Off unless asked for -- a downgrade from "on by default where hardware allows", which was
    // never actually true: first measured 22% faster (frame 76.3->59.8ms, scene draw 51.7->41.5,
    // tris 8M->2.9M) but rendered every plant in Electric Dreams as a black shredded silhouette --
    // shading is now fixed (PSClusterMain, ClusterMaterialShader.hpp, a real material shader).
    // Stage 3 added opt-in shadows/GI: Voxi's GI volume and shadow map merge into this pipeline's
    // table 0 (ensureLodMeshPipeline; D3D12Device.cpp's nullFill keeps kBindingTableCount at 2);
    // PSClusterMain runs the same shadowFactor()/coneTracedIndirect() PSMainVoxi's non-RT fallback
    // does. Still not parity (no ray tracing on this path) so still off by default. D3D12 only;
    // needs AVER_MODULE_VOXI for GI.
    lodMeshShaderEnabled_ = lodMeshShaderRequest_ > 0;
    if (lodMeshShaderEnabled_) {
        const rhi::DeviceCaps mcaps = e.device()->caps();
        // D3D12 only, enforced here: Voxi's table-0 merge puts a Texture3D/acceleration
        // structure/structured buffers where Vulkan's descriptorLayout() still assumes Texture2D.
        // meshShaderTier isn't a backend proxy -- VulkanDevice sets it too (from VK_EXT_mesh_shader),
        // so it alone won't catch this.
        const bool ok = mcaps.meshShaderTier > 0 && mcaps.shaderModel >= 65 && mcaps.dxcAvailable
#if AVER_MODULE_VOXI
                        && e.device()->backend() == rhi::Backend::D3D12
#endif
                        ;
        lodMeshShaderEnabled_ = ok;
        if (ok) {
#if AVER_MODULE_VOXI
            // Cluster-dispatch calls voxiRenderer_.submit() directly (same (mesh,world,material)
            // shape as drawMesh(), skipping IRenderFeature::submitDraw) so its shadow proxy joins
            // the cascade and GI voxelisation like any instance; only lit geometry stays cluster-only.
            AVER_INFO("[LOD] per-cluster mesh-shader path ON by request (mesh tier {}, SM {}) "
                      "-- faster and textured, and casts cascade shadows and voxel-cone GI through "
                      "its own depth proxy (via voxiRenderer_.submit()), though PSClusterMain's own "
                      "shading is never ray traced even when the project has ray tracing on",
                      mcaps.meshShaderTier, mcaps.shaderModel);
#else
            AVER_WARN("[LOD] per-cluster mesh-shader path ON by request (mesh tier {}, SM {}) "
                      "-- faster and textured, but NO SHADOWS AND NO GI on these draws (built "
                      "without AVER_MODULE_VOXI)",
                      mcaps.meshShaderTier, mcaps.shaderModel);
#endif
        }
        else    AVER_INFO("[LOD] per-cluster mesh-shader path unavailable (mesh tier {}, SM {}, "
                          "DXC {}); drawing without it", mcaps.meshShaderTier, mcaps.shaderModel,
                          mcaps.dxcAvailable);
    }
#endif

    if (!projectPath_.empty()) {
        // Same gate as a double-clicked card (used to skip the upgrade prompt via browser_.open()
        // directly). --open-legacy answers the prompt in advance: opens an older-series project
        // without upgrading, for benchmarks/captures the modal can't express.
        std::string err;
        const bool opened = openLegacy_ ? browser_.open(projectPath_, &err)
                                        : browser_.openOrOfferUpgrade(projectPath_, &err);
        if (opened) applyProject(e);
        else if (!openLegacy_ && browser_.upgradePending()) {
            armBrowser(true);
            // A question nobody answers is a failed run: with a frame limit, the session renders
            // an empty editor and reports timings/screenshots that look ordinary and mean nothing
            // (the tell, `over 0 entities`, is buried in a scene-walk line).
            // maxFrames_ != 0 is this file's test for "not interactive".
            if (maxFrames_ != 0) {
                AVER_ERROR("[Sandbox] '{}' was made by an older series and this run has a frame "
                           "limit, so nothing can answer the upgrade prompt. THE PROJECT WILL NOT "
                           "OPEN and every timing, probe and screenshot from this run measures an "
                           "empty editor. Pass --open-legacy to open it as-is without upgrading.",
                           projectPath_);
            } else {
                AVER_INFO("[Sandbox] '{}' was made by an older series; asking before opening it",
                          projectPath_);
            }
        } else {
            AVER_WARN("[Sandbox] '{}' not loaded: {}", projectPath_, err);
        }
    } else if (!openMapPath_.empty()) {
        // A level with no project still opens: everything else hangs off applyProject, so without
        // this a lone .ocmap gives an empty editor with no explanation. Placements won't resolve
        // but the level's shape beats nothing. Guarded (loading IS instantiating entities): no
        // world without AVER_MODULE_SCENE; the #else logs why it went nowhere instead of silence.
#if AVER_MODULE_SCENE
        loadStartMap(e);
#else
        AVER_WARN("[Sandbox] '{}' not opened: this build has no scene to instantiate a level into",
                  openMapPath_);
#endif
    }
#if AVER_WITH_IMGUI
    if (e.device()->uiActive()) {
        applyDpi(e.window() ? e.window()->dpiScale() : 1.0f);
        AVER_INFO("[Sandbox] DPI scale {:.2f}, UI font rasterised at {:.0f}px", dpi_, 16.0f * dpi_);
        if (browserActive_) loadLogo(e);
        loadCompileIcon(e);
        loadIconSheet(e, "file-icons.png",   kFileIconTiles, "FileTypeIcons", fileIconsTexture_,   fileIconsUiId_,   fileIconAspect_);
        loadIconSheet(e, "folder-icons.png", kFolderIconTiles, "FolderIcons",  folderIconsTexture_, folderIconsUiId_, folderIconAspect_);
        loadIconSheet(e, "asset-icons.png",  kAssetIconTiles,  "AssetTypeIcons", assetIconsTexture_,  assetIconsUiId_,  assetIconAspect_);
        // The game UI's font, staged the same way and loaded through the same decode path.
        loadGameUiFont(e);
        if (const std::string er = editor::engineRoot(); !er.empty()) {
            std::error_code ec;
            const std::filesystem::path cs = std::filesystem::path(er) / "scripting" / "csharp";
            if (std::filesystem::is_directory(cs, ec)) cbEngineRoot_ = cs.string();
        }
        AVER_INFO("[Sandbox] Content Browser engine root: {}",
                  cbEngineRoot_.empty() ? "(none - shipped build)" : cbEngineRoot_.c_str());
    }
#endif
    // Default blank map: ground floor + cube + sun + sky + atmosphere.
    std::vector<rhi::MeshVertex> gv, gi_v; std::vector<u32> gi, ci;
    appendGround(gv, gi, kEditorFloorHalf);
    rhi::MeshHandle ground = e.device()->createMesh(gv.data(), (u32)gv.size(), gi.data(), (u32)gi.size());
    // FROZEN: unitCube stays half-extent 1 -- .ocworld PLACEG scales are half-extents in cm applied to it.
    appendBox(gi_v, ci, 0,0,0, kEditorCubeHalf);
    rhi::MeshHandle cube = e.device()->createMesh(gi_v.data(), (u32)gi_v.size(), ci.data(), (u32)ci.size());

#if AVER_MODULE_SCENE
    // Built-in primitive meshes/bounds/look table come from content_ (GameContent::registerBuiltins
    // uploads them); the hook below is this editor's per-mesh follow-up (pick geometry, tri counts).
    {
        MeshLoadPass pass; pass.app = this; pass.engine = &e;
        content_.setMeshLoadedHook(&SandboxApp::onMeshLoaded, &pass);   // seeds meshTris_ and pickGeometry_ per built-in
        content_.registerBuiltins(*e.device());
        content_.setMeshLoadedHook(nullptr, nullptr);

        // Kept so a dev check can build geometry of its own without re-uploading a cube.
        unitCubeMesh_ = content_.meshFor(fnv1a64(std::string_view("Meshes/cube.ocmesh")));
    }
#endif

    MeshObj floor; floor.name="Floor"; floor.mesh=ground; floor.tris=(u32)gi.size()/3;
    floor.color[0]=0.34f; floor.color[1]=0.35f; floor.color[2]=0.37f;
    floor.metallic=0.0f; floor.roughness=0.9f;
    floor.aabbMin=Vec3{-kEditorFloorHalf,-kEditorFloorHalf,-5.0f};
    floor.aabbMax=Vec3{ kEditorFloorHalf, kEditorFloorHalf, 5.0f};
    objects_.push_back(floor);
    cubeMesh_ = cube; cubeTris_ = (u32)ci.size()/3;
    MeshObj c; c.name="Cube"; c.mesh=cube; c.tris=(u32)ci.size()/3; c.pos=Vec3{0,0,kEditorCubeHalf};
    c.color[0]=0.85f; c.color[1]=0.36f; c.color[2]=0.22f;
    c.metallic=0.1f; c.roughness=0.35f;
    objects_.push_back(c);

    if (!beamPath_.empty()) {
        fmt::OcBeamData beam; std::string err;
        if (fmt::loadOcbeam(beamPath_, beam, &err) && !beam.nodes.empty()) {
            std::vector<rhi::MeshVertex> bv; std::vector<u32> bi;
            AABB bb; bb.min=bb.max=Vec3{beam.nodes[0].x,beam.nodes[0].y,beam.nodes[0].z};
            for (auto& n : beam.nodes) bb.expand(Vec3{n.x,n.y,n.z});
            Vec3 ex=bb.extent(); f32 dot=(ex.x+ex.y+ex.z)*0.004f+0.5f;
            for (auto& n : beam.nodes) appendBox(bv,bi,n.x,n.y,n.z,dot);
            MeshObj cg; cg.name="Vehicle Cage"; cg.mesh=e.device()->createMesh(bv.data(),(u32)bv.size(),bi.data(),(u32)bi.size());
            cg.tris=(u32)bi.size()/3; cg.color[0]=0.85f;cg.color[1]=0.36f;cg.color[2]=0.22f;
            cg.aabbMin=bb.min; cg.aabbMax=bb.max;
            objects_.push_back(cg);
        }
    }

    for (MeshObj& o : objects_) makeMaterialFor(o);

    std::vector<rhi::LineVertex> gl; buildGrid(gl, kEditorGridHalf, kEditorGridCell);
    gridMesh_ = e.device()->createLineMesh(gl.data(), (u32)gl.size());

    // Per-mode gizmos: normal + yellow-highlight variant of each axis.
    for (int a = 0; a < 3; ++a) {
        auto mv=buildMoveAxis(a,kAxisCol[a]);   gzMove_[a]  =e.device()->createLineMesh(mv.data(),(u32)mv.size());
        auto mh=buildMoveAxis(a,kAxisHi);       gzMoveHi_[a]=e.device()->createLineMesh(mh.data(),(u32)mh.size());
        auto rr=buildRotRing(a,kAxisCol[a]);    gzRot_[a]   =e.device()->createLineMesh(rr.data(),(u32)rr.size());
        auto rh=buildRotRing(a,kAxisHi);        gzRotHi_[a] =e.device()->createLineMesh(rh.data(),(u32)rh.size());
        auto sc=buildScaleAxis(a,kAxisCol[a]);  gzScale_[a] =e.device()->createLineMesh(sc.data(),(u32)sc.size());
        auto sh=buildScaleAxis(a,kAxisHi);      gzScaleHi_[a]=e.device()->createLineMesh(sh.data(),(u32)sh.size());
    }

    // The Player Start's capsule and facing arrow, built once (like the gizmo handles just above) and
    // placed with a world matrix per frame (SandboxRender.cpp). The selected-state capsule is a
    // second, differently-coloured mesh rather than a per-draw tint -- drawLines has no colour
    // parameter, same reason gzMove_/gzMoveHi_ are two meshes instead of one recoloured in place.
    {
        auto cap = buildCapsuleWire(kPlayerStartCapsuleRadius, kPlayerStartCapsuleHalfHeight, kPlayerStartColor);
        playerStartCapsule_ = e.device()->createLineMesh(cap.data(), (u32)cap.size());
        auto capSel = buildCapsuleWire(kPlayerStartCapsuleRadius, kPlayerStartCapsuleHalfHeight,
                                        kSelectionColor);
        playerStartCapsuleSel_ = e.device()->createLineMesh(capSel.data(), (u32)capSel.size());
        auto arrow = buildPlayerStartArrow(kPlayerStartCapsuleHalfHeight, kPlayerStartArrowLength,
                                            kPlayerStartArrowColor);
        playerStartArrow_ = e.device()->createLineMesh(arrow.data(), (u32)arrow.size());
    }

#if AVER_MODULE_LANDSCAPE
    // The sculpt brush's footprint ring: buildRotRing(2, ...) is perpendicular to Z, i.e. already
    // flat in the XY ground plane heights are measured from (OcLand.hpp: "heights along +Z").
    // Reused rather than duplicated; only radius and world position vary, per-frame via the draw matrix.
    { auto bring = buildRotRing(2, kAxisHi); brushRing_ = e.device()->createLineMesh(bring.data(), (u32)bring.size()); }
#endif

#if AVER_MODULE_SCENE
    // Scene join, registered before Voxi: features run prePass in registration order and Voxi
    // reads vertex buffers in its own.
    skinnedScene_ = std::make_unique<aver::render::SkinnedScene>();
    if (skinnedScene_->init(*e.device())) {
        skinnedScene_->setResolvers(&game::GameContent::resolveAnimAsset, &game::GameContent::resolveSceneMesh, &content_);
        e.device()->addRenderFeature(skinnedScene_.get());
    } else {
        skinnedScene_.reset();   // init already said why; skinned entities draw at rest
    }
#if AVER_MODULE_RENDER_SOFTBODY
    // Reuses skinnedScene_'s resolver pair (id->path, id->MeshHandle) rather than adding a third.
    softBodyScene_ = std::make_unique<aver::render::SoftBodyScene>();
    if (softBodyScene_->init(*e.device())) {
        softBodyScene_->setResolvers(&game::GameContent::resolveSceneMesh, &game::GameContent::resolveAnimAsset, &content_);
        e.device()->addRenderFeature(softBodyScene_.get());
    } else {
        softBodyScene_.reset();   // init said why; soft entities draw their authored mesh
    }
#endif

// Content browser thumbnails: init() registers its own preview/copy-pass features, so unlike
// skinnedScene_ above there's no addRenderFeature call. Failed init -> ready() false, textureId()
// stays 0, gallery draws the typed icon instead.
    thumbnails_.init(*e.device());

    // --skin-scene-test <dir>: same question as --skin-draw-test but through the whole chain --
    // real .ocmesh/.ocskel/.ocanim, AnimSystem, SkinnedScene's per-entity target, substituted draw handle.
    if (!skinSceneDir_.empty() && skinnedScene_) {
        skinScene_ = std::make_unique<aver::editor::SkinSceneTest>();
        u64 meshId = 0, skelId = 0, clipId = 0;
        u32 meshHandle = 0;
        if (skinScene_->setup(e, skinSceneDir_, &meshId, &skelId, &clipId, &meshHandle)) {
            // The draw pass and SkinnedScene both resolve meshes through content_'s table;
            // registering here is what makes a spawned entity reach a draw call with no project open.
            std::pair<Vec3, Vec3> bounds;
            skinScene_->restBounds(bounds.first, bounds.second);
            content_.registerMesh(meshId, meshHandle, bounds);
            // The three ids the entities name, pointed at the cooked files -- content_'s index is
            // what AnimSystem/SkinnedScene resolve through, making the rig reachable with no project.
            std::string d = skinSceneDir_;
            while (!d.empty() && (d.back() == 92 || d.back() == '/')) d.pop_back();
            content_.indexAsset(meshId, d + "/Rig.ocmesh");
            content_.indexAsset(skelId, d + "/Rig.ocskel");
            content_.indexAsset(clipId, d + "/Rig_Bend.ocanim");
            anim::animSystem().setResolver(&game::GameContent::resolveAnimAsset, &content_);
            objects_.clear();   // nothing unrelated on screen; the probes classify by colour
            sel_ = -1;
        } else {
            skinScene_.reset();
        }
    } else if (!skinSceneDir_.empty()) {
        AVER_ERROR("[Skin] --skin-scene-test needs the scene join, which did not initialise");
    }
#endif

#if AVER_MODULE_FLUIDS
    // Registered unconditionally, spawned only if a level asks for it (idle costs nothing, keeps
    // feature order identical with/without fluid). Must precede Voxi: its acceleration-structure
    // build reads the vertex buffer this feature's prePass writes; registered after, every
    // ray-traced effect (not just GI) would see the volume one frame stale.
    water_.init(*e.device());
#endif

#if AVER_FLUIDS_SIMULATED
    // 3D-viewport icon renderer: draws in overlayPass (after the camera post chain, before the
    // editor's own UI) but must exist before level loading creates a Player Start. Failed init isn't
    // fatal -- Player Start just keeps its cube look. overlayPass IS called on Vulkan (unlike the old
    // transparentPass, which VulkanDevice never implemented), so this renders there too once
    // VulkanDevice's resource factory exists.
    viewportIconsReady_ = viewportIcons_.init(*e.device());
    if (viewportIconsReady_) {
        e.device()->addRenderFeature(&viewportIcons_);
        playerStartIcon_ = viewportIcons_.loadIcon(executableDir() + "\\" + "player-start-icon.png",
                                                   "PlayerStartIcon");
        // No icon file: fall back to the cube rather than an invisible Player Start.
        if (playerStartIcon_ == editor::ViewportIconRenderer::kNoIcon) viewportIconsReady_ = false;
    }
#endif

    // --furnace-test: does the shading model conserve energy? Uniform env radiance L, albedo-1
    // surfaces must all read the same regardless of orientation or surroundings.
    if (furnaceTest_) {
        objects_.clear();
        sel_ = -1;
        sky_.furnaceRadiance = 0.25f;
        sky_.furnaceSun = furnaceSun_;
        sunAmbient_ = 1.0f;

        // Albedo 1, fully rough, non-metallic: the furnace premise is a perfect Lambertian white;
        // wrong on any of the three and the answer legitimately isn't L.
        const auto white = [&](const char* nm, Vec3 pos, Vec3 scale) {
            MeshObj o;
            o.name = nm;
            o.mesh = unitCubeMesh_;
            o.pos = pos; o.scale = scale;
            o.color[0] = o.color[1] = o.color[2] = 1.0f;
            o.metallic = 0.0f; o.roughness = 1.0f;
            o.aabbMin = Vec3{-scale.x*1.1f, -scale.y*1.1f, -scale.z*1.1f};
            o.aabbMax = Vec3{ scale.x*1.1f,  scale.y*1.1f,  scale.z*1.1f};
            objects_.push_back(o);
        };
        // Flat + tall slabs: different faces sample several orientations of the same white surface.
        white("FurnaceFloor", Vec3{0, 0, -30}, Vec3{900.0f, 900.0f, 20.0f});
        white("FurnaceTall",  Vec3{0, 0, 200}, Vec3{160.0f, 160.0f, 220.0f});
        // The one that matters: a box open only toward the camera, back surface occluded from most
        // of the hemisphere -- must still read L (occluding walls emit L too); this is where a
        // forgetful occlusion term shows up.
        white("FurnaceCaveBack",  Vec3{-520, -520, 160}, Vec3{20.0f, 200.0f, 200.0f});
        white("FurnaceCaveLeft",  Vec3{-330, -700, 160}, Vec3{200.0f, 20.0f, 200.0f});
        white("FurnaceCaveTop",   Vec3{-330, -520, 350}, Vec3{200.0f, 200.0f, 20.0f});

        // --furnace-grid: specular half of the same question (boxes above are albedo1/rough1/
        // metal0, diffuse-only). Plate faces camera down -X, each cell at normal incidence (ndv=1,
        // the smallest the loss ever gets -- a floor, not the whole error). Replaces the five boxes,
        // so existing probes see the same thing.
        if (furnaceGrid_) {
            objects_.clear();
            const f32 rough[6] = {0.05f, 0.25f, 0.45f, 0.65f, 0.85f, 1.0f};
            const f32 metal[3] = {0.0f, 0.5f, 1.0f};
            for (int mi = 0; mi < 3; ++mi) {
                for (int ri = 0; ri < 6; ++ri) {
                    MeshObj o;
                    char nm[64];
                    std::snprintf(nm, sizeof(nm), "Cell_m%.1f_r%.2f", metal[mi], rough[ri]);
                    o.name = nm;
                    o.mesh = unitCubeMesh_;
                    // Columns march along +Y (right), rows up +Z, on one plane at +X.
                    o.pos   = Vec3{700.0f, -450.0f + 180.0f * (f32)ri, 60.0f + 140.0f * (f32)mi};
                    // Thin, so a tilt shows the same face at a glancing angle instead of swapping to
                    // the side one (at 5cm thick/75deg yaw the side face read wider than the front,
                    // and the tilt sweep came back flat and meant nothing).
                    o.scale = Vec3{0.5f, 60.0f, 60.0f};
                    // --furnace-tilt rotates each plate about Z for grazing incidence (single-scatter
                    // GGX loses most energy at low ndv). Can't show white-metal error: at albedo 1,
                    // F0=1, averEnvBRDF keeps dfg.x+dfg.y ~constant in ndv, so that row reads the
                    // same at every tilt regardless of correctness -- only useful on coloured metals/dielectrics.
                    o.rotDeg = Vec3{furnaceTilt_, 0.0f, 0.0f};
                    o.color[0] = o.color[1] = o.color[2] = 1.0f;
                    o.metallic  = metal[mi];
                    o.roughness = rough[ri];
                    o.aabbMin = Vec3{-o.scale.x*1.1f, -o.scale.y*1.1f, -o.scale.z*1.1f};
                    o.aabbMax = Vec3{ o.scale.x*1.1f,  o.scale.y*1.1f,  o.scale.z*1.1f};
                    objects_.push_back(o);
                }
            }
            // Plates normally have no material (draw through the per-draw metallic/roughness
            // fallback -- how every furnace measurement to date was taken; giving them materials
            // unconditionally would move all of them); only given one when a coat is asked for,
            // since the oracle is intra-frame and nothing compares across runs.
            if (coatWeight_ > 0.0f) {
                for (MeshObj& o : objects_) makeMaterialFor(o);
                AVER_INFO("[Furnace] grid plates given materials so the coat has somewhere to "
                          "live (coat {:.2f} rough {:.2f} f0 {:.3f}); an uncoated run still uses "
                          "the per-draw fallback path and reads exactly what it always did.",
                          coatWeight_, coatRough_, coatF0_);
            }
            AVER_INFO("[Furnace] grid: 6 roughness x 3 metallic at albedo 1, L = {}",
                      sky_.furnaceRadiance);
        }
    }

    // --refl-test: are ray-traced reflections GLOBAL? A mirror, and a beacon parked on its
    // reflection vector far outside the voxel volume, run past both tracers.
    if (reflTest_) {
        refl_ = std::make_unique<aver::editor::ReflTest>();
        objects_.clear();
        sel_ = -1;

        // Large flat quad at z=0, fully metallic and near-smooth, so it shows almost entirely reflection.
        MeshObj m;
        m.name = "ReflMirror";
        m.mesh = unitCubeMesh_;
        m.pos = Vec3{0, 0, -20.0f};
        // Half-extents in cm (unit cube = half-extent 1): 1800cm mirror. (18 gave a 36cm slab every probe missed.)
        m.scale = Vec3{1800.0f, 1800.0f, 20.0f};
        m.color[0] = 0.02f; m.color[1] = 0.02f; m.color[2] = 0.02f;
        m.metallic = 1.0f; m.roughness = 0.03f;
        m.aabbMin = Vec3{-1900, -1900, -60}; m.aabbMax = Vec3{1900, 1900, 20};
        objects_.push_back(m);

        const Vec3 bp = aver::editor::ReflTest::beaconPosition(camPos_, Vec3{0, 0, 0});
        MeshObj bcn;
        bcn.name = "ReflBeacon";
        bcn.mesh = unitCubeMesh_;
        bcn.pos = bp;
        // Big enough to subtend several degrees from 5200 cm away, or its reflection lands between probes.
        bcn.scale = Vec3{700.0f, 700.0f, 700.0f};
        // Emissive-bright green: nothing else in scene is green, so a green probe must be the beacon.
        bcn.color[0] = 0.02f; bcn.color[1] = 0.95f; bcn.color[2] = 0.05f;
        bcn.roughness = 1.0f;
        bcn.aabbMin = Vec3{-950, -950, -950}; bcn.aabbMax = Vec3{950, 950, 950};
        objects_.push_back(bcn);
        reflBeaconIndex_ = static_cast<int>(objects_.size()) - 1;

        refl_->setBeaconPosition(bp);
        refl_->setVolumeExtent(giExtent_);
    }

    // --skin-draw-test: does anything draw the skinned buffer. Position is the contract: prePass
    // runs features in registration order, so the skinning dispatch must register BEFORE the scene
    // renderer, whose Voxi replay reads the vertex buffer this dispatch writes -- registered after,
    // it reads unwritten data every frame.
    if (skinDrawTest_) {
        skinDraw_ = std::make_unique<aver::editor::SkinDrawTest>();
        if (skinDraw_->init(*e.device())) {
            e.device()->addRenderFeature(skinDraw_.get());
            // Scene becomes exactly one object so the probe reads the box or sky, never an unrelated mesh.
            objects_.clear();
            sel_ = -1;
            // Ground first (behind the box in submission order and depth); pale/rough since the
            // assertion is about how much light reaches it.
            MeshObj g;
            g.name = "SkinDrawTest.Ground";
            g.mesh = skinDraw_->ground();
            g.color[0] = 0.75f; g.color[1] = 0.75f; g.color[2] = 0.72f;
            g.roughness = 0.9f;
            g.aabbMin = Vec3{-1700, -1700, -300};
            g.aabbMax = Vec3{ 1700,  1700, -160};
            objects_.push_back(g);

            MeshObj o;
            o.name = "SkinDrawTest";
            o.mesh = skinDraw_->mesh();
            o.color[0] = 0.9f; o.color[1] = 0.15f; o.color[2] = 0.9f;   // magenta: unlike ground and sky in every channel
            o.roughness = 0.6f;
            o.aabbMin = Vec3{-260, -260, -260};
            o.aabbMax = Vec3{ 260,  260,  260};
            objects_.push_back(o);

            // Ground probe is only an acceleration-structure test when RT draws the shadow --
            // told rather than guessed, so the report names which question it answered. Whether
            // RT is on is the test's own schedule to drive: the experiment IS the toggle.
            skinDraw_->setRayTracingAvailable(e.device()->caps().rayTracingTier >= 11);
        } else {
            AVER_ERROR("[Skin] draw test unavailable on this device");
            skinDraw_.reset();
        }
    }
#if AVER_MODULE_SCRIPTING
    {
        scripting::HostDesc hd;
        hd.bridgeDir = executableDir() + "\\Scripting";
        hd.scriptsDir = resolveScriptsDir();
        scripts_.init(hd);

        // Animation notifies: editor wants this as much as a shipped game (a Play-mode preview
        // should fire what it fires); survives level/project reload since AnimSystem::clear() drops
        // clips/playheads but not the wire.
        //
        // Needs the scene, not just the synapse half: animSystem() lives in Aver.Anim.Scene (added
        // only under AVER_MODULE_SCENE), and animNotify takes a scene::Entity. A notify happens TO
        // an entity, so with no entities there's nothing to route -- the sink stays uninstalled.
#if AVER_MODULE_SCENE
        if (scripts_.graphFireAvailable()) {
            anim::animSystem().setNotifySink(&SandboxApp::animNotify, this);
#if AVER_MODULE_SYNAPSE_SCENE
            // Same sink as animNotify above (just scripts_.graphFire(entity,name); NotifyFn matches
            // AnimNotifyFn's signature byte-for-byte -- SynapsePerception.hpp). BT's FireEvent uses it too.
            synapse::perceptionSystem().setNotifySink(&SandboxApp::animNotify, this);
            synapse::btSystem().setNotifySink(&SandboxApp::animNotify, this);
#endif
        }
#endif  // AVER_MODULE_SCENE

        // Animation curves: the framework relays a query it can't answer itself; this supplies the
        // answer. Installed unconditionally (unlike the notify sink) since a C++ caller can ask too
        // -- "unconditionally" means without asking the host, not in every build: aver_fw_* names
        // only exist under AVER_MODULE_FRAMEWORK, a separate option from scripting (module-matrix.ps1's
        // scene-off row turns FRAMEWORK off and leaves SCRIPTING on, which is what exposed it).
#if AVER_MODULE_FRAMEWORK
        aver_fw_set_anim_curve_provider(&SandboxApp::animCurve, this);
#endif

#if AVER_MODULE_SYNAPSE_SCENE
        // Framework guard covers all three below: the two relays are aver_fw_* names too, not just
        // the resolver (it used to sit around the resolver alone, reading as if the resolver were
        // the framework-dependent one, when it's actually the least so).
#if AVER_MODULE_FRAMEWORK
        // GetSynapseTarget (Aver Node) reaches CSynapseAgent's waypoint through this -- same reason
        // and placement as the anim-curve provider above.
        aver_fw_set_synapse_target_provider(&SandboxApp::synapseTarget, this);
        // GetSynapsePerception (Aver Node) reaches CSynapsePerception's sight state the same way.
        aver_fw_set_synapse_perception_provider(&SandboxApp::synapsePerception, this);
        // PerceptionSystem's own resolver seam (SynapsePerception.hpp), not a framework_abi.h relay:
        // Aver.Synapse.Scene must not link Aver.Framework, so only a composition root (linking both)
        // can answer this -- still needs the guard since synapseTargetResolver is declared under it.
        synapse::perceptionSystem().setTargetResolver(&SandboxApp::synapseTargetResolver, this);
#endif
#endif

        // Save/load: lets a project test its save path in the editor rather than only in a
        // shipped game (there is none today). Guarded on both conditions, not just the framework
        // one the ABI call needs: saveWriteProvider/saveLoadProvider are declared `#if
        // AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK` in the header (they save/load a whole
        // scene::World through Aver.Save, itself scene-gated), so this must match.
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
        aver_fw_set_save_provider(&saveWriteProvider, &saveLoadProvider, this);
#endif

        // Graph-as-class catch-up for a CLI-opened project: applyProject's "Starting scripts" stage
        // already tried this, but a CLI project opens before the scripting host bootstraps, so
        // scripts_.ready() was false and that attempt was a no-op. Idempotent, so retrying here is safe.
#if AVER_MODULE_FRAMEWORK
        if (scripts_.ready() && project_.valid()) {
            const i32 graphClasses = scripts_.declareGraphClasses(project_.contentDir());
            if (graphClasses > 0)
                AVER_INFO("[Graph] {} graph class(es) declared from '{}'", graphClasses, project_.contentDir());
            spawnClassPlacements();
        }
#endif

#if AVER_MODULE_VOXI
    // Hand the GPU's real capabilities to Voxi so its settings reflect this hardware.
    {
        // `caps`, not `c`: a MeshObj named `c` is still in scope from the editor-cube setup
        // two hundred lines up, and shadowing it warned (C4456).
        const rhi::DeviceCaps caps = e.device()->caps();

        // On by default now (was opt-in behind --lod-mesh-shader): pine_tree_01 is 17.18M
        // triangles, and with the flag off the editor drew all of them at LOD0 (~258M triangles
        // for "2 actors", 13 FPS). Auto (-1) decides from caps; the flag pins it either way. Decided just before the
        // project opens since it must precede applyProject, whose mesh loads build GPU cluster
        // buffers only when this flag is already true.

        voxi::DeviceInfo di;
        di.msaaMask = caps.msaaMask; di.maxMsaaSamples = caps.maxMsaaSamples;
        di.rayTracingTier = caps.rayTracingTier; di.computeShaders = caps.computeShaders;
        di.typedUavLoads = caps.typedUavLoads; di.conservativeRaster = caps.conservativeRaster;
        di.shaderModel = caps.shaderModel; di.meshShaderTier = caps.meshShaderTier;
        di.dxcAvailable = caps.dxcAvailable;
        // R1: computed here and mirrored in GameApp::attachVoxi (same expression); see
        // DeviceInfo::denoiserSupported's comment (Voxi.hpp) for why the struct carries the answer
        // rather than deriving it itself.
        di.denoiserSupported = e.device()->backend() == rhi::Backend::D3D12;
        voxi::Renderer::get().setDeviceInfo(di);

        // Project manifest applied HERE: before the flags and before init(). Before init() because
        // init() calls createVoxelVolume(settings_.voxelResolution) -- the only place the volume is
        // ever sized. Seeding here means the volume is simply CREATED at the right size: no resize,
        // no descriptor rebind, no GPU resource recreated at a moment that could remove the device.
        // Arriving later (via the old post-attach applyProjectRenderSettings call) meant every
        // project ran the 128^3 default regardless of what it asked for (PTTest's RENDER.VOXELRES
        // 512 ran a grid 64x smaller while the log said "applied render settings from ..."). Before
        // the flags because `s` below is seeded from this singleton and the flag block overwrites
        // it -- ordering alone keeps "a flag is a human standing right there, a manifest is a
        // recorded preference" true, no separate precedence pass needed.
        // (applyProjectRenderSettings still runs its own take() later, for mid-session project-open.)
        // After setDeviceInfo, since setSettings clamps against the device it's told about.
        applyProjectVoxiSettings();

        voxi::Settings s = voxi::Renderer::get().settings();
        s.msaa = static_cast<voxi::Msaa>(e.device()->sampleCount());
        if (msaaOverride_) s.msaa = static_cast<voxi::Msaa>(msaaOverride_);
        // --no-gi wins over --gi.
        if (giForceOff_)          s.globalIllumination = voxi::Quality::Off;
        else if (giOverride_ >= 0) s.globalIllumination = static_cast<voxi::Quality>(giOverride_);
        // --no-rt wins over --rt (mirrors --no-gi). No longer the only way to reach Off (--rt 0
        // does too, via the -1 "not given" sentinel), kept since existing configs spell it this way.
        if (rtForceOff_)          s.rayTracing = voxi::Quality::Off;
        else if (rtOverride_ >= 0) s.rayTracing = static_cast<voxi::Quality>(rtOverride_);
        // Path tracing is its own tier/flag: gained one when ptBounces started gating on it, or
        // --pt-bounces would be inert where it's benchmarked. No --no-pt; `--pt 0` turns it off.
        if (ptOverride_ >= 0) s.pathTracing = static_cast<voxi::Quality>(ptOverride_);
        if (msOverride_) s.meshShaders = true;
        // Applied to `s`, not voxiRenderer_ directly, before the singleton below: setSettings() reruns every frame from
        // voxi::Renderer::get().settings(), so a one-time poke would be overwritten. Negative counts
        // are reported, not clamped (old warning: "4294967291 shadow rays clamped to 32").
        // Tiers applied in their OWN call first: setSettings decides "did the caller set this" by
        // value, so re-sending the already-held value is indistinguishable from not asking and the
        // tier silently wins (`--rt 4 --rt-rays 1` measured the same 17.4ms as `--rt 4` alone). Two
        // calls fixes it: tiers, read back, then explicit knobs in a second call.
        voxi::Renderer::get().setSettings(s);
        auto k = voxi::Renderer::get().settings();
        if (rtRaysOverride_ > 0) k.rtShadowRays = static_cast<u32>(rtRaysOverride_);
        else if (rtRaysOverride_ < 0)
            AVER_WARN("[Sandbox] --rt-rays {} is not a ray count; the default of {} stands",
                      rtRaysOverride_, k.rtShadowRays);
        // >=0 not >0: 0 means filter off here (sentinel is -1), unlike its two neighbours whose valid range starts at 1.
        if (rtShadowDenoiseOverride_ >= 0) k.rtShadowDenoise = static_cast<u32>(rtShadowDenoiseOverride_);
        if (rtRenderModeOverride_    >= 0) k.rtRenderMode    = static_cast<u32>(rtRenderModeOverride_);
        if (ptBouncesOverride_       >= 0) k.ptBounces       = static_cast<u32>(ptBouncesOverride_);
        if (layeredBsdfOverride_     >= 0) k.layeredBsdf     = static_cast<voxi::Quality>(layeredBsdfOverride_);
        // Load-bearing, not tidy, that this is in the SECOND call: giSkyOcclusionRays derives from
        // the rayTracing tier on a tier change (see the two-call reasoning above). >=0 not >0
        // because 0 means "use the cone gather's occlusion", not "flag absent".
        if (giSkyOccRaysOverride_ >= 0) k.giSkyOcclusionRays = static_cast<u32>(giSkyOccRaysOverride_);
        if (giSkyOccTileOverride_ >= 0) k.giSkyOcclusionTile = static_cast<u32>(giSkyOccTileOverride_);
        if (rtPixelsPerRayOverride_ > 0) k.rtPixelsPerRayTile = static_cast<u32>(rtPixelsPerRayOverride_);
        else if (rtPixelsPerRayOverride_ < 0)
            AVER_WARN("[Sandbox] --rt-pixels-per-ray {} is not a tile edge; the default of {} stands",
                      rtPixelsPerRayOverride_, k.rtPixelsPerRayTile);
        // Refraction's knobs are in the SECOND call too: refractionMode is tier-derived, so setting
        // it in the first (tier-changing) call would be indistinguishable from not setting it.
        if (refractionOverride_ >= 0)          k.refractionMode = static_cast<u32>(refractionOverride_);
        if (refractionStrengthOverride_ >= 0.0f) k.refractionStrength = refractionStrengthOverride_;
        if (refractionFadeOverride_ >= 0.0f)     k.refractionEdgeFade = refractionFadeOverride_;
        if (giUpdateIntervalOverride_ > 0) k.giUpdateInterval = static_cast<u32>(giUpdateIntervalOverride_);
        else if (giUpdateIntervalOverride_ < 0)
            AVER_WARN("[Sandbox] --gi-update-interval {} is not a frame count; the default of {} stands",
                      giUpdateIntervalOverride_, k.giUpdateInterval);
        // >=0 not >0: 0 is a real estimator (voxel cones), not absent -- same -1-sentinel reasoning
        // as rtShadowDenoiseOverride_ and giSkyOccRaysOverride_ above.
        if (giModeOverride_ >= 0) k.giMode = static_cast<u32>(giModeOverride_);
        // Same -1 sentinel reasoning: 0 is a real answer ("denoiser off"), not an absent flag.
        if (denoiserOverride_ >= 0) k.denoiser = denoiserOverride_ != 0;
        voxi::Renderer::get().setSettings(k);
        AVER_INFO("[Voxi] attached: MSAA {}x, RT tier {}, SM {}, mesh tier {}", caps.maxMsaaSamples, caps.rayTracingTier, caps.shaderModel, caps.meshShaderTier);

        // Registration is non-owning: voxiRenderer_ must outlive the device, torn down in onShutdown.
        // Read back from the singleton (not `s` directly) to see the clamping setSettings just applied.
        voxiRenderer_.setSettings(voxi::Renderer::get().settings());
        if (frameTimeReport_) voxiRenderer_.setFrameTimeReport(true);
        // Before init() below, which is where the pipelines -- and therefore the shader defines
        // this changes -- are actually compiled.
        if (rdAblate_) {
            voxiRenderer_.setRayDrivenAblation(static_cast<u32>(rdAblate_));
            AVER_WARN("[Sandbox] --rd-ablate {}: the ray-driven frame is DELIBERATELY WRONG. "
                      "This is a stopwatch for attributing PSRayDriven's cost, not a quality "
                      "setting -- see AVER_RD_ABLATE in voxi.hlsl.", rdAblate_);
        }
        // Unlike --rd-ablate this isn't a shader define so needn't precede init(), but sits here
        // beside its sibling so both measurement dials are handed over in one place.
        if (rtDenoiseMotionTaper_ > 0.0f) {
            voxiRenderer_.setRtDenoiseMotionTaper(rtDenoiseMotionTaper_);
            AVER_INFO("[Sandbox] --rt-denoise-motion {}: the shadow denoiser will fade out "
                      "as the gather centre reprojects. 0 (the default) is no taper.",
                      rtDenoiseMotionTaper_);
        }
        if (voxiRenderer_.init(*e.device())) {
            e.device()->addRenderFeature(&voxiRenderer_);
            voxiAttached_ = true;
            if (projectRenderPending_) applyProjectRenderSettings();
            if (saveProject_ && !saveProjectDone_) { saveProjectDone_ = true; seedAndSaveProject(); }
#if AVER_WITH_IMGUI
            // --import's deferred handshake, UI-only on purpose: importAsset calls
            // cbIsEditable/cbInvalidate/importModel, the browser's own; a UI-less editor has no
            // browser to import into.
            if (!importSrc_.empty() && !importDone_) {
                importDone_ = true;
                importAsset(importSrc_, importDst_);
            }
#endif
#if AVER_MODULE_PBR
            content_.setTextureFactory(e.device()->resources());
            voxiRenderer_.materials().setTextureResolver(&game::GameContent::resolveMaterialTexture, &content_);
            // Same resolver, handed to the asset tabs' shared preview: identical function/
            // GameContent so a mesh's material looks the same in a tab as in the level -- two
            // resolvers would be two answers that silently drift. Without this the preview samples
            // its own identity textures and shades white (the animation editor's blank-mesh bug).
            // Set here, not in the preview, since the resolver needs content_ and ActorEditor.cpp
            // shouldn't know what a project is.
            editor::setPreviewTextureResolver(&game::GameContent::resolveMaterialTexture, &content_);
            editor::setPreviewMaterialLookup(
                [](const std::string& surface, void* user) -> u32 {
                    return static_cast<u32>(static_cast<game::GameContent*>(user)->materialForSurface(surface));
                }, &content_);
#endif
            // Installed unconditionally, even in a build with no Trifactor: depthProxy_ is then
            // simply empty, every lookup answers 0, and every pass draws what it drew before.
            voxiRenderer_.setDepthProxy(&SandboxApp::depthProxyLookup, this);
        }
    }
#endif
    // The game UI's render feature: overlay pass only, so ordering against scene features is free.
    gameUi_ = aver::render::ui::UiRenderer::create(*e.device());
    if (gameUi_) e.device()->addRenderFeature(gameUi_);

#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
    // Not an IRenderFeature: no prePass/scenePass, just two methods renderSceneEntities() calls
    // directly. Built unconditionally (cheap); every call is already gated on occlusionCullEnabled_.
    if (rhi::IResourceFactory* occRes = e.device()->resources()) {
        occluder_ = aver::occlusion::createOcclusionCuller(*occRes);
        // --occlusion-waitidle's seed, applied once here and reasserted every frame from onUpdate.
        // editor::consoleOcclusionForceWaitIdleSlot() (EditorConsole.hpp) is the live source of truth
        // from here on, so a console `set occlusion.debugForceWaitIdle` and this CLI flag are the
        // same switch, not two that can disagree.
        editor::consoleOcclusionForceWaitIdleSlot() = occlusionDebugForceWaitIdleArg_;
        occluder_->setDebugForceWaitIdle(occlusionDebugForceWaitIdleArg_);
        // Warmed up here, not on first per-frame call: a safety requirement, not tidiness.
        // ensureSized() builds 3 compute pipelines; D3D12RenderContext caches the bound pipeline as
        // a raw pointer (pipe_), so a push_back reallocating while another feature's pointer rests
        // on the old array is a dangling read -- calling this lazily from the scene walk once
        // corrupted Voxi's cached pointer mid-frame and crashed the driver's shader compiler. Doing
        // it before Engine::run's first beginFrame() means nothing yet holds a live pointer. A later
        // resize still rebuilds the pyramid lazily -- accepted, since resizes are rare and user-driven.
        rhi::TextureDesc occSceneDesc;
        if (const rhi::TextureHandle occDepth = e.device()->sceneDepthTexture();
            occDepth && occRes->textureInfo(occDepth, occSceneDesc)) {
            occluder_->ensureSized(*occRes, occSceneDesc.width, occSceneDesc.height, e.device()->sampleCount());
        }
    }
#endif

#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // Registered unconditionally (like skinnedScene_ above): a project's CParticleEmitter
    // placements must draw with no CLI flag. System and effect library are process-global
    // singletons, so registerEffect() anywhere reaches whatever ticks it.
    particles::particleSystem().setEffectLibrary(&particles::particleEffects());
    if (particleRenderer_.init(*e.device())) {
        particleRenderer_.setSystem(&particles::particleSystem());
        e.device()->addRenderFeature(&particleRenderer_);
        particlesAttached_ = true;
#if AVER_MODULE_VOXI
        // Installed only once Voxi has attached (voxiAttached_) -- see particleGiPrepare/
        // particleGiBind. particleRenderer_ never learns Voxi's name; --no-particle-gi skips this line.
        if (voxiAttached_ && !noParticleGi_) {
            particles::ParticleRenderer::GiSeam seam;
            seam.prepare = &SandboxApp::particleGiPrepare;
            seam.bind = &SandboxApp::particleGiBind;
            seam.user = this;
            particleRenderer_.setGiSeam(seam);
        }
#endif
    } else {
        AVER_ERROR("[Particles] renderer unavailable on this device");
    }

#if AVER_MODULE_FLUIDS
    // Water, registered on the same terms as everything else here: failed init just means no water
    // (HLSL compiles at RUNTIME and can fail on a machine whose build was perfectly green).
    if (waterEnabled_) {
        // Default swell (not a flat mirror): 4 waves at spread headings so it reads as water
        // immediately. Wavelengths deliberately non-harmonic -- multiples re-phase into a visible tile.
        fluids::GerstnerWave waves[4];
        const f32 dirs[4][2] = {{1.0f, 0.15f}, {0.6f, -0.8f}, {-0.3f, 0.95f}, {-0.85f, -0.5f}};
        const f32 lengths[4] = {1450.0f, 890.0f, 520.0f, 310.0f};
        const f32 amps[4]    = {34.0f, 19.0f, 9.0f, 4.5f};
        for (int i = 0; i < 4; ++i) {
            waves[i].dirX = dirs[i][0];
            waves[i].dirZ = dirs[i][1];
            waves[i].wavelengthCm = lengths[i];
            waves[i].amplitudeCm  = amps[i];
            waves[i].steepness    = 0.75f;
        }
        if (water_.attachSurface(*e.device(), waterHeightCm_, waves, 4)) {
            // Plane set from water_'s own level, not a second number: two independent heights would
            // look like broken buoyancy. Guarded on PHYSICS too: rendering water and floating on
            // it are different capabilities.
#if AVER_MODULE_PHYSICS
            const f32 normal[3]  = {0.0f, 0.0f, 1.0f};
            const f32 current[3] = {0.0f, 0.0f, 0.0f};
            aver_phys_set_water_plane(water_.waterLevelCm(), normal,
                                      1.0f, 0.5f, 0.05f, current);
#endif
            AVER_INFO("[Water] surface and buoyancy plane at z = {} cm", waterHeightCm_);
        } else {
            AVER_ERROR("[Water] renderer unavailable on this device");
        }
#else
    if (waterEnabled_)
        AVER_WARN("[Water] --water was given, but this build has no water module");
#endif
    }

    // --particle-test: dust cloud straddling an opaque cube so the transparent pass's depth test
    // shows in one screenshot -- some particles nearer the camera, some farther and occluded.
    if (particleTest_) {
        objects_.clear();
        sel_ = -1;

        MeshObj occluderCube;
        occluderCube.name = "ParticleTest.Occluder";
        occluderCube.mesh = unitCubeMesh_;
        occluderCube.pos = Vec3{600.0f, 0.0f, 50.0f};
        occluderCube.scale = Vec3{80.0f, 80.0f, 80.0f};
        occluderCube.color[0] = 0.55f; occluderCube.color[1] = 0.5f; occluderCube.color[2] = 0.45f;
        occluderCube.roughness = 0.8f;
        occluderCube.aabbMin = Vec3{512.0f, -88.0f, -38.0f};
        occluderCube.aabbMax = Vec3{688.0f,  88.0f, 138.0f};
        objects_.push_back(occluderCube);

        // Dust cloud spans well in front of AND behind the cube along the camera's forward axis
        // (+X) -- that spread makes occlusion visible in one static frame, no motion needed.
        particles::ParticleEffect fx;
        fx.shape = particles::EmitterShape::Box;
        fx.shapeSize = Vec3{350.0f, 60.0f, 90.0f};
        fx.emissionRate = 150.0f;
        fx.burstCount = 0;
        fx.maxParticles = 400;
        fx.lifetimeMin = 3.0f;
        fx.lifetimeMax = 5.0f;
        fx.direction = Vec3{0.0f, 0.0f, 1.0f};
        fx.spreadDeg = 60.0f;
        fx.speedMin = 5.0f;
        fx.speedMax = 15.0f;
        fx.gravity = Vec3{0.0f, 0.0f, -5.0f};
        fx.damping = 0.3f;
        fx.sizeStart = 25.0f;
        fx.sizeEnd = 45.0f;
        fx.colorStart[0] = 0.75f; fx.colorStart[1] = 0.70f; fx.colorStart[2] = 0.62f; fx.colorStart[3] = 0.35f;
        fx.colorEnd[0]   = 0.75f; fx.colorEnd[1]   = 0.70f; fx.colorEnd[2]   = 0.62f; fx.colorEnd[3]   = 0.0f;
        fx.blend = rhi::BlendMode::PremultipliedAlpha;
        constexpr u64 kDustCloudEffectId = 0x50415254'44550001ull;   // arbitrary, non-zero
        particles::particleEffects().set(kDustCloudEffectId, fx);

        scene::World& world = scene::World::instance();
        Transform xf;
        xf.position = occluderCube.pos;
        const scene::Entity emitter = world.create("ParticleTest.DustCloud", scene::kInvalidEntity, xf);
        if (auto* emitterComp = static_cast<scene::CParticleEmitter*>(
                world.addComponent(emitter, scene::kComponentParticleEmitter))) {
            emitterComp->effect = kDustCloudEffectId;
        }
        AVER_INFO("[Particles] --particle-test: dust cloud entity {} around effect 0x{:016X}",
                  emitter, kDustCloudEffectId);

        // Second emitter proves the opposite half of the seam: embers ARE their own light source
        // (receivesGI=false, additive), placed beside the cube so they never overlap the dust. If
        // dust brightness changes under --no-particle-gi and embers don't, the seam works.
        particles::ParticleEffect emberFx;
        emberFx.shape = particles::EmitterShape::Sphere;
        emberFx.shapeSize = Vec3{10.0f, 0.0f, 0.0f};
        emberFx.emissionRate = 80.0f;
        emberFx.burstCount = 0;
        emberFx.maxParticles = 200;
        emberFx.lifetimeMin = 1.0f;
        emberFx.lifetimeMax = 1.6f;
        emberFx.direction = Vec3{0.0f, 0.0f, 1.0f};
        emberFx.spreadDeg = 35.0f;
        emberFx.speedMin = 40.0f;
        emberFx.speedMax = 80.0f;
        emberFx.gravity = Vec3{0.0f, 0.0f, -25.0f};
        emberFx.damping = 0.1f;
        emberFx.sizeStart = 22.0f;
        emberFx.sizeEnd = 6.0f;
        emberFx.colorStart[0] = 1.0f; emberFx.colorStart[1] = 0.55f; emberFx.colorStart[2] = 0.12f; emberFx.colorStart[3] = 1.0f;
        emberFx.colorEnd[0]   = 1.0f; emberFx.colorEnd[1]   = 0.15f; emberFx.colorEnd[2]   = 0.02f; emberFx.colorEnd[3] = 0.0f;
        emberFx.blend = rhi::BlendMode::Additive;
        emberFx.receivesGI = false;   // DECIDED 4: an ember is its own light source
        constexpr u64 kEmberEffectId = 0x50415254'45420001ull;   // arbitrary, non-zero
        particles::particleEffects().set(kEmberEffectId, emberFx);

        // High above the dust cloud, clear of its footprint, so the two effects never overlap. Open sky, no occluder behind it --
        // deliberate: this placement first exposed the transparent-pass/sky ordering bug fixed in
        // D3D12Device::endFrame (a particle with no opaque depth behind it was silently overdrawn
        // by the sky's depth-EQUAL-clear fill). Kept here since that's the common case and must
        // render correctly.
        Transform emberXf;
        emberXf.position = occluderCube.pos + Vec3{0.0f, 0.0f, 220.0f};
        const scene::Entity emberEmitter =
            world.create("ParticleTest.Embers", scene::kInvalidEntity, emberXf);
        if (auto* emberComp = static_cast<scene::CParticleEmitter*>(
                world.addComponent(emberEmitter, scene::kComponentParticleEmitter))) {
            emberComp->effect = kEmberEffectId;
        }
        AVER_INFO("[Particles] --particle-test: ember entity {} around effect 0x{:016X} (receivesGI=false)",
                  emberEmitter, kEmberEffectId);
    }
    // --particle-stress <N> <M>: verification-only test for the parity-and-price pass. One fog/mist
    // effect referenced by N grid emitters, each capped at M particles, so emitter count and
    // per-emitter count vary independently. No occluder: measures cost, not the depth test
    // --particle-test already proved.
    else if (particleStressEmitters_ > 0) {
        objects_.clear();
        sel_ = -1;

        particles::ParticleEffect fx;
        fx.shape = particles::EmitterShape::Box;
        fx.shapeSize = Vec3{350.0f, 60.0f, 90.0f};
        fx.emissionRate = 150.0f;   // a steady fog/mist emission rate, not a weapon effect
        fx.maxParticles = static_cast<u32>(particleStressMaxParticles_ > 0 ? particleStressMaxParticles_ : 400);
        // Price measurement: bursts the whole cap on frame 1 so steady-state cost is reached
        // immediately instead of waiting emissionRate-many seconds -- stress-harness-only choice,
        // not authored-effect data.
        fx.burstCount = fx.maxParticles;
        // Price measurement: long near-constant lifetime keeps the burst-filled population steady
        // for the whole capture window instead of decaying and shrinking mid-run.
        fx.lifetimeMin = 120.0f;
        fx.lifetimeMax = 120.0f;
        fx.direction = Vec3{0.0f, 0.0f, 1.0f};
        fx.spreadDeg = 60.0f;
        fx.speedMin = 5.0f;
        fx.speedMax = 15.0f;
        fx.gravity = Vec3{0.0f, 0.0f, -5.0f};
        fx.damping = 0.3f;
        fx.sizeStart = 25.0f;
        fx.sizeEnd = 45.0f;
        fx.colorStart[0] = 0.75f; fx.colorStart[1] = 0.70f; fx.colorStart[2] = 0.62f; fx.colorStart[3] = 0.35f;
        fx.colorEnd[0]   = 0.75f; fx.colorEnd[1]   = 0.70f; fx.colorEnd[2]   = 0.62f; fx.colorEnd[3]   = 0.0f;
        fx.blend = rhi::BlendMode::PremultipliedAlpha;
        constexpr u64 kStressEffectId = 0x50415254'53545301ull;   // "PART" + "STS\1", arbitrary
        particles::particleEffects().set(kStressEffectId, fx);

        scene::World& world = scene::World::instance();
        const int n = particleStressEmitters_;
        const int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<f64>(n))));
        const f32 spacing = 140.0f;
        for (int idx = 0; idx < n; ++idx) {
            const int col = idx % cols;
            const int row = idx / cols;
            Transform xf;
            xf.position = Vec3{600.0f,
                                (static_cast<f32>(col) - static_cast<f32>(cols - 1) * 0.5f) * spacing,
                                50.0f + static_cast<f32>(row) * spacing};
            const scene::Entity emitter =
                world.create("ParticleStress.Emitter", scene::kInvalidEntity, xf);
            if (auto* stressComp = static_cast<scene::CParticleEmitter*>(
                    world.addComponent(emitter, scene::kComponentParticleEmitter))) {
                stressComp->effect = kStressEffectId;
            }
        }
        AVER_INFO("[Particles] --particle-stress: {} emitter(s), {} max particles each, effect 0x{:016X}",
                  n, fx.maxParticles, kStressEffectId);

        // --particle-stress2: verification-only; adds a second, additive sparks-like emitter beside
        // the mist so the price measurement covers both blend pipelines mixed in one frame.
        if (particleStressSecondEmitter_) {
            particles::ParticleEffect fx2;
            fx2.shape = particles::EmitterShape::Sphere;
            fx2.shapeSize = Vec3{10.0f, 0.0f, 0.0f};
            fx2.emissionRate = 80.0f;
            fx2.burstCount = 0;
            fx2.maxParticles = 200;
            fx2.lifetimeMin = 1.0f;
            fx2.lifetimeMax = 1.6f;
            fx2.direction = Vec3{0.0f, 0.0f, 1.0f};
            fx2.spreadDeg = 35.0f;
            fx2.speedMin = 40.0f;
            fx2.speedMax = 80.0f;
            fx2.gravity = Vec3{0.0f, 0.0f, -25.0f};
            fx2.damping = 0.1f;
            fx2.sizeStart = 22.0f;
            fx2.sizeEnd = 6.0f;
            fx2.colorStart[0] = 1.0f; fx2.colorStart[1] = 0.55f; fx2.colorStart[2] = 0.12f; fx2.colorStart[3] = 1.0f;
            fx2.colorEnd[0]   = 1.0f; fx2.colorEnd[1]   = 0.15f; fx2.colorEnd[2]   = 0.02f; fx2.colorEnd[3] = 0.0f;
            fx2.blend = rhi::BlendMode::Additive;
            fx2.receivesGI = false;
            constexpr u64 kStressEffect2Id = 0x50415254'53545302ull;
            particles::particleEffects().set(kStressEffect2Id, fx2);

            Transform xf2;
            xf2.position = Vec3{600.0f, 0.0f, 270.0f};
            const scene::Entity emitter2 =
                world.create("ParticleStress.Emitter2", scene::kInvalidEntity, xf2);
            if (auto* c2 = static_cast<scene::CParticleEmitter*>(
                    world.addComponent(emitter2, scene::kComponentParticleEmitter))) {
                c2->effect = kStressEffect2Id;
            }
            AVER_INFO("[Particles] --particle-stress: diag second emitter {} effect 0x{:016X}",
                      emitter2, kStressEffect2Id);
        }
    }
#endif

    // --skin-test: GPU skinning pass against its CPU reference, on the real device. Registered
    // only when asked for (costs a waitIdle); typically run alongside --debug-layer, which catches
    // a malformed descriptor as opposed to a wrong number.
    if (skinTest_) {
        skinSelfTest_ = std::make_unique<aver::render::SkinSelfTest>();
        if (skinSelfTest_->init(*e.device())) e.device()->addRenderFeature(skinSelfTest_.get());
        else { AVER_ERROR("[Skin] self-test unavailable on this device"); skinSelfTest_.reset(); }
    }

    // --pt-furnace: does the path tracer conserve energy? Brings its own geometry/acceleration
    // structures/accumulators; the editor only supplies the furnace (setFurnaceTest puts
    // SkyAtmosphere::furnaceRadiance on, read through skyColor()).
    if (ptFurnaceTest_) {
        ptFurnace_ = std::make_unique<aver::pt::PtFurnaceTest>();
        if (ptFurnace_->init(*e.device())) e.device()->addRenderFeature(ptFurnace_.get());
        else { AVER_ERROR("[PT] furnace unavailable on this device"); ptFurnace_.reset(); }
    }

    // --pt-scene: path tracer pointed at the REAL scene instead of the furnace's private geometry
    // -- a progressive still-camera reference that suppresses the raster scene while registered
    // (sky + one sun, static geometry, flat albedo -- PtSceneView.hpp). setPtSceneView() already set
    // ptSceneViewWantEnabled_; this call turns that want into a registration, same path the Path
    // Tracing Quality combo uses later.
    syncPtSceneView(e.device());

        tools_.setAutoCompileFlag(autoCompileFlag());
        tools_.setReloader([this](const std::string& binDir, std::string* status) {
            return reloadScripts(binDir, status);
        });
    }
#endif
    tool_ = initialTool_;
    sel_ = 1;
    bool framedByLevel = false;
#if AVER_MODULE_SCENE
    framedByLevel = !levelEntities_.empty();
#endif
    // A restored camera counts as framed too, or the default view below would discard it: a level
    // can carry a CAMERA record with no placements (an empty level being laid out).
    if (levelCameraRestored_) framedByLevel = true;
    if (!framedByLevel) {
        camPos_ = Vec3{700.0f, 700.0f, 450.0f};
        const Vec3 d = (Vec3{0,0,1} - camPos_).getSafeNormal();
        yaw_ = std::atan2(d.y, d.x);
        pitch_ = std::asin(d.z);
    }
    // Last, so --cam outranks both writers above it: frameCameraOnLevel ran back in loadStartMap,
    // and the default framing is the branch immediately before this.
    if (camOverride_) {
        camPos_ = camPosOverride_;
        pitch_  = pitchOverride_;
        yaw_    = yawOverride_;
    }
}

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

// Advances one frame: MCP commands, camera, gameplay tick, physics, and the render state.
void SandboxApp::onUpdate(Engine& e, const Timestep& t)  {
    // Frame Skip's forced unpause, undone on scope exit -- declared at function scope so the
    // repause still runs however this function returns, not only when control reaches the tick
    // groups below in the order this comment assumes.
    struct FrameStepGuard {
        bool active = false;
        ~FrameStepGuard() {
#if AVER_MODULE_FRAMEWORK
            if (active) aver_fw_set_paused(1);
#endif
        }
    } frameStepGuard;
    // --set NAME VALUE: applied once, at frame 5 not frame 1, since the project's own RENDER.*
    // apply runs during startup and would overwrite anything staged earlier -- a handful of frames
    // costs nothing in a run long enough to measure a temporal artifact. runConsoleLine is the
    // console's own entry point, so a --set takes the same text the drawer takes and reports the
    // same errors.
    // Guarded because runConsoleLine is part of the console (`#if AVER_WITH_IMGUI`, SandboxShell.cpp),
    // defined only when Aver.RHI.D3D12.ImGui is built, and disappears with -DAVER_RHI_D3D12=OFF; the
    // flag is still PARSED either way, and the #else logs that rather than dropping the caller's
    // dials in silence (same choice as waterEnabled_).
#if AVER_WITH_IMGUI
    if (!consoleSetsApplied_ && !consoleSets_.empty() && t.frame >= 5 && e.device()) {
        consoleSetsApplied_ = true;
        for (const auto& kv : consoleSets_) {
            AVER_INFO("[Sandbox] --set {} {}", kv.first, kv.second);
            runConsoleLine(e, "set " + kv.first + " " + kv.second);
        }
    }
#else
    if (!consoleSetsApplied_ && !consoleSets_.empty()) {
        consoleSetsApplied_ = true;
        AVER_WARN("[Sandbox] --set ignored: this build has no editor console to route {} pair(s) through",
                  consoleSets_.size());
    }
#endif
    // First in the frame so everything downstream (view matrix, gPrevViewProj reprojection, shadow
    // history) sees one consistent camera; latched base yaw rather than accumulated onto yaw_
    // (accumulating drifts with float error and never returns exactly to start).
    // debugViewActiveThisFrame is set inside `#if AVER_MODULE_VOXI` below, beside setViewDebug -- a
    // build without the module never reaches that block, so it stays false and the post-process
    // push far below (which runs unconditionally) never applies the debug-view exposure override.
    bool debugViewActiveThisFrame = false;
#if AVER_MODULE_VOXI
    // View mode reaches Voxi from onUpdate, NOT the scene pass -- the whole reason unlit did
    // nothing under ray-driven. Its twin, device()->setUnlit, is set/cleared around the scene draw
    // since that flag rides per-draw state and would leak into editor chrome; Voxi instead composes
    // its frame constants in prePass (which beginFrame runs before onRender), so setting it there
    // meant prePass always read last frame's already-cleared value (gViewParams.x stuck at 0,
    // PSRayDriven never branching). No clear needed here: nothing else draws through this pass.
    voxiRenderer_.setUnlit(unlit_);
    // debugView_'s twin: wireframe/G-buffer debug need the rasteriser, so a ray-hit/triangles debug
    // view can only draw while ray-driven primary visibility is selected AND available (see
    // ViewDebug/setViewDebug, VoxiRenderer.hpp). Reasserted here, beside setUnlit, for the same
    // "before prePass" reason. The scratch-copy block further down recomputes these two conditions
    // independently for vs.rtRenderMode; see that block's comment for why it isn't read back from here instead.
    const bool viewModeNeedsRaster = wireframe_ || gbufferDebugView_ != GBufferDebugFeature::Mode::Off;
    // A raster-only mode CLEARS a ray-hit/triangles view rather than merely outranking it:
    // wireframe_ can be ticked from Settings/Preferences too, which know nothing of debugView_,
    // and leaving it set would keep its name on the dropdown button over a raster frame.
    if (viewModeNeedsRaster) debugView_ = voxi::VoxiRenderer::ViewDebug::None;
    const bool viewModeNeedsRayDriven = !viewModeNeedsRaster &&
        debugView_ != voxi::VoxiRenderer::ViewDebug::None && voxiRenderer_.rayDrivenAvailable();
    voxiRenderer_.setViewDebug(viewModeNeedsRayDriven ? debugView_ : voxi::VoxiRenderer::ViewDebug::None);
    debugViewActiveThisFrame = viewModeNeedsRayDriven;
    // Wireframe shows only mesh edges (IDevice::setWireframe), so the lit renderer stops working
    // for it: no shadows, GI, ray tracing or denoising while it is on (VoxiRenderer::setPaused).
    voxiRenderer_.setPaused(wireframe_);
#endif
    // --cam-wobble-stop N: wobble runs only below frame N, then the camera holds. Measuring an
    // artifact that appears WHILE moving and decays AFTER stopping needs both in one deterministic
    // run ("k frames after motion ended"). 0 (default) leaves --cam-wobble unchanged.
    if (camWobbleDeg_ != 0.0f && camWobblePeriod_ > 0 &&
        (camWobbleStopFrame_ <= 0 || t.frame < (u64)camWobbleStopFrame_)) {
        if (!camWobbleBased_) { camWobbleBaseYaw_ = yaw_; camWobbleBased_ = true; }
        const f32 phase = 6.2831853f * (f32)(t.frame - 1) / (f32)camWobblePeriod_;
        yaw_ = camWobbleBaseYaw_ + camWobbleDeg_ * 0.01745329252f * std::sin(phase);
    }
    // --cam-translate SPEED: see setCamTranslate's own comment. One fixed step per frame along
    // this frame's camForward() (after the
    // wobble update, so the two compose), so occlusion relationships change frame to frame instead
    // of just their screen-space position under a pure yaw wobble. --cam-wobble-stop N stops this
    // too: "the camera stopped" must mean ALL motion, or a post-stop capture still measures nothing useful.
    if (camTranslateSpeed_ != 0.0f &&
        (camWobbleStopFrame_ <= 0 || t.frame < (u64)camWobbleStopFrame_)) {
        camPos_ += camForward() * camTranslateSpeed_;
    }
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
    // --no-occlusion-cull: see setOcclusionCullForceOff's own comment for why it can't be a
    // one-shot CLI setter -- applyProjectVoxiSettings rewrites
    // occlusionCullEnabled_ from the manifest during project/level load, after CLI setters have run.
    // Reasserted every frame so it also wins back over a later --open-level or project reload.
    if (occlusionCullForceOff_) occlusionCullEnabled_ = false;
    // --occlusion-waitidle/occlusion.debugForceWaitIdle: reasserted every frame so a console `set`
    // this frame is live on occluder_ before this frame's testBatch() call (occlusionBuildAndTest(),
    // below), not one frame late.
    if (occluder_) occluder_->setDebugForceWaitIdle(editor::consoleOcclusionForceWaitIdleSlot());
    // 3B (occlusion-fix-plan.md): occlusion.showCulled/occlusion.cullUnderSuppression, read every
    // frame for the same reason, so a console `set` is live for this frame's onRender walk
    // (chooseRoute()/occlusionTestShouldRun(), SceneSubmission.hpp), not one frame late.
    occlusionShowCulled_ = editor::consoleOcclusionShowCulledSlot();
    occlusionCullUnderSuppression_ = editor::consoleOcclusionCullUnderSuppressionSlot();
#endif
    // Single-instance forwarding, drain. Polled rather than an Event (fixed POD, no string field),
    // drained unconditionally every frame -- safe since pumpEvents() runs BEFORE onUpdate and
    // WM_COPYDATA is synchronous on this thread.
    maybeAutosave(t.dt);
    maybeAutosavePrefs(t.dt);
    // Reasserted every frame like the autosave calls above; cheap since it only touches the window
    // when the built string differs from what's shown.
    refreshWindowTitle(e);
#if AVER_MODULE_SCENE
    // See multiStale(): anything that moved the anchor without touching the set means the
    // selection collapsed to one; this is where that is made true rather than merely believed.
    multiSyncToAnchor();
#endif
    // Project settings follow the same rule preferences do now: an edit reaches the file without
    // anybody having to find a button.
    maybeAutosaveProject(t.dt);
    // One line, once, not before frame 2: applyProject's scripting stage and spawnClassPlacements
    // both run during startup, so a frame-1 census would report a still-filling world, differing
    // from the game's for a reason that's about timing, not content.
#if AVER_MODULE_SCENE
    if (sceneCensus_ && !sceneCensusDone_ && t.frame >= 2) {
        sceneCensusDone_ = true;
        // playerStart_ excluded: the editor synthesises that marker from the level's SPAWN record
        // but the shipped game doesn't, so counting it would be a false divergence. See
        // takeSceneCensus' own comment.
        AVER_INFO("[Census] {}",
                  world::formatSceneCensus(
                      world::takeSceneCensus(scene::World::instance(), playerStart_)));
    }
#endif
#if AVER_MODULE_SCENE
    // --save-level <out>: writes the OPEN level to another path and logs the result. Writes
    // ELSEWHERE rather than over levelPath_, so proving the save never costs the content it proved
    // on -- needed since saveLevel previously had no caller but a mouse click (Ctrl+S, the File
    // menu, the toolbar button), so nothing could prove a round trip headlessly, the same gap the
    // four project flags were added to close.
    // Used to save the WRONG level: this ran in the Voxi-init one-shot hook (frame 1, before
    // --open-level applied), so `--open-level Arena --save-level out.ocworld` always wrote the
    // project's start map -- two runs went by before the placement count was noticed wrong. Now
    // waits for every pending open (--open-level, consumed on the first UI draw; a forwarded
    // launch; an unsaved-changes prompt) rather than just moving later, since any of the three can
    // still be queued.
    if (!saveLevelTo_.empty() && !saveLevelDone_ && !levelPath_.empty() &&
        openLevelByName_.empty() && pendingOpenPath_.empty() && !pendingOpenPrompt_) {
        saveLevelDone_ = true;
        // Source named too: a log line naming only the destination is how the wrong-level bug went unnoticed.
        if (saveLevel(saveLevelTo_))
            AVER_INFO("[Level] --save-level wrote '{}' ({}) to {}",
                      levelName_, levelPath_, saveLevelTo_);
        else
            AVER_ERROR("[Level] --save-level failed for {}", saveLevelTo_);
    }
#endif
    // Guarded as a whole (unlike the dropped-files drain below): the queue is filled only by the
    // open-request hook, installed under the same condition (setOpenRequestHook). requestOpenLevel/
    // openLevelError_ are `#if AVER_MODULE_SCENE` too -- a forwarded path asks for a level in a world.
#if AVER_MODULE_SCENE
    if (window_ && window_->hasPendingOpenRequest()) {
        const std::string path = window_->takePendingOpenRequest();
        if (isLevelFile(path.c_str())) {
            // Same funnel as the picker/Content Browser: unsaved-changes check and class-placement
            // spawn live there -- this path used to do the first and forget the second.
            if (!requestOpenLevel(path, "opened, forwarded from another launch"))
                AVER_WARN("[Sandbox] forwarded level '{}': {}", path, openLevelError_);
        }
        // A forwarded bare project with no level has nothing more to do here: handleOpenRequest
        // already confirmed it matches the live project, and Window::focus() already raised this window.
    }
#endif  // AVER_MODULE_SCENE
    // Files dragged in from Explorer, latched like the open request above (see
    // Window::hasPendingDroppedFiles). Only meaningful with a project open -- importDroppedFiles
    // imports into the Content Browser's current folder, which doesn't exist before a project does.
    //
    // Destination is the Content Browser: importDroppedFiles/notifyOutcome are `#if AVER_WITH_IMGUI`
    // (SandboxContentBrowser.cpp), so a build without the D3D12 ImGui backend has neither folder
    // nor toast. The queue is
    // still drained in both arms -- Window keeps accepting WM_DROPFILES regardless, and an undrained
    // list would grow forever.
    if (window_ && window_->hasPendingDroppedFiles()) {
        const std::vector<std::string> dropped = window_->takePendingDroppedFiles();
#if AVER_WITH_IMGUI
        if (project_.valid()) {
            importDroppedFiles(dropped);
        } else {
            AVER_WARN("[Import] {} file(s) dropped with no project open; ignored", dropped.size());
            notifyOutcome(editor::NotifySeverity::Warning, "Nothing to import into",
                          "Open a project before dropping files into the editor.");
        }
#else
        AVER_WARN("[Import] {} file(s) dropped; this build has no Content Browser to import into",
                  dropped.size());
#endif
    }
    // --pt-scene-toggle-on/-off: verification-only. Checked BEFORE syncPtSceneView() so the same
    // onUpdate() that flips the want-flag also acts on it, with no extra frame of lag.
    if (ptSceneToggleOnAutoFrames_  > 0 && --ptSceneToggleOnAutoFrames_  == 0) ptSceneViewWantEnabled_ = true;
    if (ptSceneToggleOffAutoFrames_ > 0 && --ptSceneToggleOffAutoFrames_ == 0) ptSceneViewWantEnabled_ = false;
    // --sun-set-at: the same write the Directional Light panel's Elevation/Azimuth sliders make.
    if (sunSetAtFrames_ > 0 && --sunSetAtFrames_ == 0) {
        sky_.setSunAngles(sunSetElevDeg_, sunSetAzimDeg_);
        AVER_INFO("[Sandbox] --sun-set-at: sun moved to elevation {:.1f} deg, azimuth {:.1f} deg",
                  sunSetElevDeg_, sunSetAzimDeg_);
    }
    // --sun-sweep: once the countdown reaches 1 it stays there, and every frame from then on turns the
    // sun -- the same setSunAngles write the Directional Light panel's sliders make while dragged.
    if (sunSweepFrames_ > 0) {
        if (sunSweepFrames_ > 1) {
            --sunSweepFrames_;
        } else {
            f32 el = 0.0f, az = 0.0f;
            sky_.sunAngles(el, az);
            sky_.setSunAngles(el, az + sunSweepDeg_);
            if (sunSweepTurnsLeft_ > 0 && --sunSweepTurnsLeft_ == 0) {
                sunSweepFrames_ = 0;
                sky_.sunAngles(el, az);
                AVER_INFO("[Sandbox] --sun-sweep-frames: the drag let go at frame {}, sun at elevation "
                          "{:.3f} deg, azimuth {:.3f} deg", t.frame, el, az);
            }
        }
    }
#if AVER_MODULE_VOXI
    // --gi-history-reset-at: the resetgihistory and resetdenoiserhistory console commands' own requests.
    if (giHistoryResetAtFrames_ > 0 && --giHistoryResetAtFrames_ == 0) {
        voxi::Renderer::get().requestGiHistoryReset();
        voxi::Renderer::get().requestDenoiserHistoryReset();
        AVER_INFO("[Sandbox] --gi-history-reset-at: GI reservoir and denoiser history reset requested");
    }
#endif
    // Clears the render-scale crash cookie once this session proves the scale survivable. Thirty
    // frames, not one: device loss is noticed at Present.
    // --shader-source: pick up an HLSL edit without restarting.
    // "Only the first edit is ever delivered" was a measurement artifact: `--frames 900 --no-vsync`
    // lasts ~12s (measured: 900 frames/12.0s/75fps), so 2/3 of a 21-second test's edits landed
    // after the process had exited (the log's own "stopped after 900 frame(s)" sits between append
    // one and append two). Re-measured with `--frames 9000`, five edits nine seconds apart: all delivered.
    // Only bumps an integer here: rhi::reloadShaderFiles() drops the text cache and increments a
    // revision; VoxiRenderer::prePass rebuilds pipelines there, where it's safe (doing it here would
    // free pipelines a command list is recording against). Started lazily, so no-flag costs nothing.
    if (!shaderSourceDir_.empty()) {
        if (!shaderWatch_.watching()) {
            if (shaderWatch_.start(shaderSourceDir_))
                AVER_INFO("[Sandbox] watching {} for shader edits", shaderSourceDir_);
            else
                shaderSourceDir_.clear();   // start() logged why; do not retry every frame
        } else {
            std::vector<aver::FileEvent> events;
            const bool rescan = shaderWatch_.poll(events);
            if (rescan || !events.empty()) {
                // `evt`, not `e`: onUpdate's own Engine& parameter is named `e`, and this loop
                // variable shadowed it (C4457).
                for (const aver::FileEvent& evt : events)
                    AVER_INFO("[Sandbox] shader file changed: {}", evt.path);
                if (aver::rhi::reloadShaderFiles() > 0)
                    AVER_INFO("[Sandbox] shaders will recompile on the next frame's prePass");
            }
        }
    }
    if (renderScaleCookieArmed_ && t.frame >= 30) {
        renderScaleCookieArmed_ = false;
        editor::setPrefBool("display.renderScalePending", false);
        editor::flushEditorPrefs();
    }
#if AVER_MODULE_SR
    // --aversr-cycle [N]: verification-only, exists because this transition had a bug -- turning
    // AverSR on then off crashed the editor: Off's branch
    // reset the unique_ptr while the device still held the raw pointer applyUpscalerSlot gave it,
    // so the next composite called execute() on freed memory. --aversr only ever ATTACHES, so
    // nothing exercised this path.
    if (averSrCycleFrames_ > 0 && --averSrCycleFrames_ == 0) {
        AVER_INFO("[AverSR] --aversr-cycle: Performance -> Off, the transition that used to crash");
        applyAverSrQuality(e.device(), aver::sr::Quality::Performance);
        applyAverSrQuality(e.device(), aver::sr::Quality::Off);
        AVER_INFO("[AverSR] --aversr-cycle: survived the round trip");
    }
#endif
#if AVER_MODULE_VOXI
    // N8 fix, part 1: the PT view now follows a path tracing tier change from anywhere. Before, only 4
    // sites flipped ptSceneViewWantEnabled_ (CLI, Quality combo, toggle-test flags, manifest -- see
    // ptSceneViewFromCli_'s comment); none fired for the console's voxi.pathTracing, Overall Quality
    // (applyOverall, Scalability.hpp) or voxi.scalability, which write the tier directly.
    {
        const voxi::Quality curPtTier = voxi::Renderer::get().settings().pathTracing;
        if (curPtTier != ptTierSeen_) {
            if (curPtTier != voxi::Quality::Off) {
                ptSceneViewWantEnabled_ = true;
                // Quality::Low is 1, so the rung is one less; mirrors buildRenderingSettings' (page
                // == 4) re-quality call when the view is already registered and only its resolution moves.
                if (ptSceneView_) ptSceneView_->setQuality(static_cast<u32>(curPtTier) - 1);
            } else if (!ptSceneViewFromCli_) {
                // --pt-scene still wins even here: a console edit or mid-session project open that
                // turns PT Off must not silently drop a view the command line asked to keep (same
                // precedence applyProjectRenderSettings honours for RENDER.PATHTRACING).
                ptSceneViewWantEnabled_ = false;
            }
            ptTierSeen_ = curPtTier;
        }
    }
#endif
    // BEFORE device_->beginFrame() (see Engine::frameStep()) -- the only safe place to add or
    // remove a render feature. See syncPtSceneView()'s own comment for why.
    syncPtSceneView(e.device());
    // Path tracer's matched-environment legacy switch (contrast-fix F6/F7, root cause R5) --
    // outside any AVER_MODULE_VOXI guard, beside syncPtSceneView(), because ptSceneView_'s
    // registration/tier selection must keep working with the module off (see
    // ptSceneViewWantEnabled_'s own comment), the same reason consolePtLegacyEnvSlot()
    // (EditorConsole.hpp) is declared outside it. Reasserted every frame like every other raw
    // console slot here; setLegacyEnvironment only acts (and re-arms accumulation) on an actual change.
    if (ptSceneView_) ptSceneView_->setLegacyEnvironment(editor::consolePtLegacyEnvSlot());
#if AVER_MODULE_VOXI
    // --pt-quality-ramp [N]: verification-only, for a bug no flag could reach: raising the PT rung
    // on an already-rendered view re-arms PtSceneView at a new resolution, and used to point an
    // over-sized descriptor at the denoiser's still-small buffers and remove the device --
    // reachable only by a human clicking the combo.
    if (ptQualityRampEvery_ > 0 && --ptQualityRampCountdown_ <= 0) {
        ptQualityRampCountdown_ = ptQualityRampEvery_;
        voxi::Settings ramped = voxi::Renderer::get().settings();
        const u32 rung = static_cast<u32>(ramped.pathTracing);
        // Off is never raised INTO: the ramp exercises rung changes on a live view, and turning
        // the feature on is the registration path --pt-scene-toggle-on already covers.
        if (ramped.pathTracing != voxi::Quality::Off && rung < static_cast<u32>(voxi::Quality::Epic)) {
            ramped.pathTracing = static_cast<voxi::Quality>(rung + 1);
            voxi::Renderer::get().setSettings(ramped);
            AVER_INFO("[PT] --pt-quality-ramp: Path Tracing raised to quality {}",
                      static_cast<u32>(ramped.pathTracing));
        }
    }
    // Rung reconciled on the same cadence as registration: --pt N, a manifest and the settings
    // combo all write voxi::Settings, and this is the one place that want becomes a call;
    // setQuality() is idempotent so calling it every frame costs one comparison.
    if (ptSceneView_) {
        const voxi::Quality q = voxi::Renderer::get().settings().pathTracing;
        if (q != voxi::Quality::Off) ptSceneView_->setQuality(static_cast<u32>(q) - 1);
    }
#endif
#if AVER_MODULE_MCP
    // One event per frame: a click needs a press frame and a later release frame to register.
    if (mcp_.listening()) mcp_.pump([this](const mcp::Command& c) { applyMcpCommand(c); });
#endif
    if (sky_.cloudsEnabled) cloudTime_ += t.dt;
    if (showUiDemo_) {
        uiDemoHealth_  = 0.5f + 0.5f * std::sin(uiDemoClock_ * 0.7f);
        uiDemoStamina_ = 0.5f + 0.5f * std::sin(uiDemoClock_ * 1.6f + 1.0f);
        uiDemoScroll_  = std::fmod(uiDemoScroll_ + t.dt * 24.0f, 216.0f);   // px, wraps at 12 rows
        uiDemoClock_  += t.dt;
    }
#if AVER_WITH_IMGUI
    if (e.device()->uiActive()) {
        // Must stay outside the ImGui frame: the font atlas may not be touched inside one.
        const f32 dpi = e.window() ? e.window()->dpiScale() : dpi_;
        if (std::fabs(dpi - dpi_) > 0.01f) {
            AVER_INFO("[Sandbox] DPI changed {:.2f} -> {:.2f}, re-rasterising the UI font", dpi_, dpi);
            applyDpi(dpi);
        }
    }
    // Engine's default pawn is an exception to "the game owns the input": leaving it out made PIE
    // feel broken (gameHasInput() true for any live session stood the viewport camera down the
    // moment Play possessed one -- --pie-camera-test measured drift of exactly 0.0000, "nothing
    // ran"). The engine's stand-in is the editor camera wearing a pawn, the one session that must
    // not stand down.
    // One arbitration, handed to every consumer, computed once instead of eleven spellings (InputOwnership.hpp).
#if AVER_WITH_IMGUI
    // The #if is compile-time, this question is not (the same distinction the capture block below
    // spells out): a headless run never calls uiInit() (no hwnd,
    // early-returns), so CreateContext() never ran and GImGui is null -- GetIO() then faults reading
    // off a null pointer (crashed at 0x00000000000000fa, exactly
    // offsetof(ImGuiContext,IO)+offsetof(ImGuiIO,WantTextInput)).
    // Asking uiActive() as the gate rather than a new flag: the answer is already computed into
    // ic.uiActive one line down (InputOwnership.hpp calls false "a headless/no-UI build"). Leaving
    // own_ at its default costs nothing: resolveInputOwnership returns all-false for uiActive=false.
    if (e.device()->uiActive()) {
        const ImGuiIO& oio = ImGui::GetIO();
        editor::InputConditions ic;
        ic.uiActive          = e.device()->uiActive();
        ic.browserActive     = browserActive_;
        ic.textInput         = oio.WantTextInput;
        ic.uiWantsKeyboard   = oio.WantCaptureKeyboard;
        ic.uiWantsMouse      = oio.WantCaptureMouse;
        // FALSE WHILE EJECTED, both of these: ejecting hands the viewport tools back exactly like
        // leaving Play, and toolsLive's defaultPawnPlay exception (InputOwnership.cpp) exists to
        // keep the spectator pawn flyable pre-eject -- once ejected the fly block below drives
        // camPos_ instead, so the exception must not fire either. playEjected() needs no guard of
        // its own (it reads false without the framework).
        ic.playing           = playSessionActive() && !playEjected();
        ic.releasedByUser    = releasedByUser_;
        // Left at its default without the scene, not guarded away: defaultPawnPlay_ is `#if
        // AVER_MODULE_SCENE`, and false is honest for a tree that can't enter Play; InputConditions
        // has the field either way.
#if AVER_MODULE_SCENE
        ic.defaultPawnPlay   = defaultPawnPlay_ && !playEjected();
#endif
        ic.mouseCaptured     = mouse_.captured();
        ic.pointerInViewport = levelHovered_ && inViewport(oio.MousePos.x, oio.MousePos.y);
        ic.drawerOpen        = drawer_ != Drawer::None;
        ic.landscapeMode     = mode_ == EditorMode::Landscape;
        // --wheel-speed-test stands in for a pointer in the viewport: headlessly there's no real
        // cursor, so ImGui reports wantKb=1 wantMouse=1 ptrInViewport=0 and resolveInputOwnership
        // correctly denies the tools -- right for an unhovered window, wrong precondition for this
        // test (which is about what happens once the gate is OPEN). These three overrides ARE the
        // state of a person right-dragging, so setting them stands in for the human; everything
        // downstream runs exactly as in a real session, confined to the test's own frames -- nothing
        // else in this frame changes.
        // Guarded to match its declaration: wheelTestForceFly_ lives in the framework block with
        // maybeWheelSpeedTest and the other Play self-tests, so -DAVER_MODULE_FRAMEWORK=OFF removes
        // flag and test together.
#if AVER_MODULE_FRAMEWORK
        if (wheelTestForceFly_) {
            ic.uiWantsKeyboard = false;
            ic.uiWantsMouse = false;
            ic.pointerInViewport = true;
        }
#endif
        own_ = editor::resolveInputOwnership(ic);
    }
#endif
    // Leaving fly mode is unconditional, and must be: flying_ used to clear only inside the
    // narrower gate below (which splits keyboard from mouse), so a focused text field or
    // off-viewport pointer left the camera stuck flying with the cursor hidden. Releasing the
    // button always means stop flying, whoever owns input.
#if AVER_WITH_IMGUI
    // uiActive() first (headless has no ImGui context to ask). Nothing lost skipping it there --
    // flying_ starts false and its only writer is the own_-gated block below, UI-only anyway.
    if (e.device()->uiActive() && !ImGui::GetIO().MouseDown[1]) flying_ = false;
#endif
    if (own_.keyboardToTool || own_.mouseToTool) {
        const ImGuiIO& io = ImGui::GetIO();
        const bool overUI = !levelHovered_ || !inViewport(io.MousePos.x, io.MousePos.y);
        if (inputProbe_ && (ImGui::GetFrameCount() % 30) == 0)
            AVER_INFO("[input-probe] mouse=({},{}) wantCaptureMouse={} hovered={} inViewport={} "
                      "focused={} -> overUI={} | flying={} cam=({:.0f},{:.0f},{:.0f}) yaw={:.2f}",
                      (int)io.MousePos.x, (int)io.MousePos.y, (int)io.WantCaptureMouse,
                      (int)levelHovered_, (int)inViewport(io.MousePos.x, io.MousePos.y),
                      (int)levelFocused_, (int)overUI, (int)flying_,
                      camPos_.x, camPos_.y, camPos_.z, yaw_);

        // Right mouse ENTERS fly mode here; LEAVING it is handled unconditionally above, outside
        // this gate -- the same defect class as the input publisher that used to latch every key.
        if (ImGui::IsMouseClicked(1) && !overUI) flying_ = true;
        // --wheel-speed-test drives this directly (see maybeWheelSpeedTest for why it forces state
        // rather than synthesising the right-drag). Guarded like the InputConditions overrides above.
#if AVER_MODULE_FRAMEWORK
        if (wheelTestForceFly_) flying_ = true;
#endif
        if (flying_) ImGui::SetMouseCursor(ImGuiMouseCursor_None);

        if (flying_) {
            // input_.mouseDX/DY, not io.MouseDelta -- same phase bug as the wheel block below, of
            // which only the wheel half was ever fixed. ImGui computes io.MouseDelta inside
            // NewFrame (UpdateMouseInputs), but Engine::frameStep runs onUpdate BEFORE uiNewFrame,
            // so any read here sees the PREVIOUS frame's delta: not a stutter, not a GPU cost, but
            // a fixed one-frame lag between hand and picture on every frame, by construction ("it
            // lags behind my inputs"). InputState is fed by pumpEvents (before frameStep) and
            // rolled at the end of onRender, so during onUpdate it holds THIS frame's motion --
            // the correctly-phased source.
            yaw_   += static_cast<f32>(input_.mouseDX()) * lookSpeed_;
            pitch_ -= static_cast<f32>(input_.mouseDY()) * lookSpeed_;
            pitch_ = pitch_ < -1.54f ? -1.54f : (pitch_ > 1.54f ? 1.54f : pitch_);
            // Wheel while flying changes speed, up = faster (Unreal's direction; shipped inverted
            // first, then settled this way -- a preference, written down rather than re-argued).
            // Multiplicative (1.25/notch, ~3 notches to double: coarse enough for one flick, fine
            // enough to settle a speed) so a notch feels the same at speed 1 as at 20.
            // input_.wheel(), not io.MouseWheel, which is ALWAYS zero here -- the bug that made the
            // whole control dead (the old `flySpeed_ *= (1+io.MouseWheel*0.15)` sat on this exact
            // line and never fired either). Same phase bug as the mouseDX/DY note above:
            // Engine::frameStep runs onUpdate before uiNewFrame (Engine.cpp:208-212), and EndFrame
            // zeroes io.MouseWheel at the tail of the previous frame (imgui.cpp:6420), so onUpdate
            // only ever sees the stale reset. GraphEditor/AssetEditor's wheel zoom work because they
            // run from onRender, after NewFrame. input_.wheel() (via pumpEvents/newFrame()) holds
            // this frame's notches correctly.
            const f32 wheel = input_.wheel();
            if (wheel != 0.0f) {
                flySpeed_ *= std::pow(1.25f, wheel);
                flySpeed_ = flySpeed_ < 20.0f ? 20.0f : (flySpeed_ > 40000.0f ? 40000.0f : flySpeed_);
            }
        }

        const Vec3 fwd = camForward();
        const Vec3 up{0, 0, 1};
        const Vec3 right = cross(up, fwd).getSafeNormal();

        // Spectator Play flies without holding the right button: pressing Play with no GameMode
        // gives a plain camera, and having to hold a button to walk it isn't what anyone means by
        // that; mouse look still wants the button.
        // Synthetic look enters here, where the real one does: --pie-camera-test used to bump
        // yaw_/pitch_ from a later tick that never carried into the pawn, and drivePlayCamera
        // restored the pawn's unchanged forward next frame -- a "steady camera" that was just bad
        // ordering, not ignored input. All four pieCam members are declared with maybePieCameraTest
        // in the framework block (no PIE camera without a GameMode), so the injection point matches
        // rather than moving the members.
#if AVER_MODULE_FRAMEWORK
        if (pieCamPendingLook_) {
            pieCamPendingLook_ = false;
            yaw_   += 0.5f;
            pitch_ += 0.3f;
            pieCamWantYaw_ = yaw_; pieCamWantPitch_ = pitch_;
        }
#endif
        // defaultPawnPlay_ is scene-guarded (see InputConditions above). Without the module the
        // camera flies on the right button alone, as it did before spectator Play existed.
        // EJECTED DROPS THE EXCEPTION: the spectator pawn stops flying without RMB and the block
        // below moves camPos_ instead of the pawn, i.e. edit-mode flying, same as any other session.
#if AVER_MODULE_SCENE
        if ((flying_ || (defaultPawnPlay_ && !playEjected())) && !io.WantCaptureKeyboard) {
#else
        if (flying_ && !io.WantCaptureKeyboard) {
#endif
            const f32 sp = flySpeed_ * t.dt;
            Vec3 step{0, 0, 0};
            if (ImGui::IsKeyDown(ImGuiKey_W)) step += fwd * sp;
            if (ImGui::IsKeyDown(ImGuiKey_S)) step -= fwd * sp;
            if (ImGui::IsKeyDown(ImGuiKey_D)) step += right * sp;
            if (ImGui::IsKeyDown(ImGuiKey_A)) step -= right * sp;
            if (ImGui::IsKeyDown(ImGuiKey_E)) step += up * sp;
            if (ImGui::IsKeyDown(ImGuiKey_Q)) step -= up * sp;
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
            // Moves the PAWN, not the camera, while the default pawn is possessed and not ejected:
            // drivePlayCamera() rewrites the view from the pawn every frame, so a camPos_ nudge
            // would be overwritten. Ejected takes the camPos_ branch below like edit mode.
            if (defaultPawnPlay_ && !playEjected() && walkCapsule_) {
                driveDefaultPawnWalk(fwd, right);    // the walking default pawn (Play options > Walk)
            } else if (defaultPawnPlay_ && !playEjected()) {
                const int32_t pn = aver_fw_controlled_pawn(aver_fw_player_controller(0));
                if (pn) {
                    const scene::Entity pe = static_cast<scene::Entity>(static_cast<uint32_t>(pn));
                    scene::World& pw = scene::World::instance();
                    if (pw.valid(pe)) {
                        const Transform& cur = pw.localTransform(pe);
                        pw.setLocalPosition(pe, cur.position + step);
                        // Yaw about world up, then pitch about the pawn's own right: the same two
                        // angles the viewport camera uses, so looking with RMB turns the pawn and
                        // the follow puts the view exactly where the editor camera would have been.
                        const Quat qy = Quat::fromAxisAngle(Vec3{0, 0, 1}, yaw_);
                        const Quat qp = Quat::fromAxisAngle(Vec3{0, 1, 0}, -pitch_);
                        pw.setLocalRotation(pe, qy * qp);
                    }
                }
            } else
#endif
            camPos_ += step;
        } else if (!overUI) {
            // Same defect as the fly-speed wheel bug: this scroll-to-dolly read io.MouseWheel too
            // and never moved the camera either -- found by following that bug, not by a report,
            // which is what a control with no headless witness looks like when it breaks. See the
            // wheel note in the flying_ block above.
            const f32 wheel = input_.wheel();
            if (wheel != 0.0f) camPos_ += fwd * wheel * (flySpeed_ * 0.15f);
            // Same phase argument as the look block above: MMB pan read a frame-old delta too.
            if (io.MouseDown[2]) {
                camPos_ -= right * static_cast<f32>(input_.mouseDX()) * 0.02f;
                camPos_ += up    * static_cast<f32>(input_.mouseDY()) * 0.02f;
            }
        }
        // Ctrl+S saves the level, which the File menu has claimed for as long as it's existed --
        // that "Ctrl+S" label is just ImGui::MenuItem's shortcut-LABEL parameter, wiring nothing.
        // The only ImGuiKey_S in this file was the camera's strafe-left, so the reflex shortcut did
        // nothing, with no feedback to say so.
        //
        // Not gated on levelFocused_ (unlike F below): saving isn't a viewport gesture. Gated on
        // WantTextInput though, or renaming an entity would save on the "s" of its name.
        if (!io.WantTextInput && keybinds_.pressed(editor::CommandId::LevelSave, io))
            saveLevelInteractive();

        // Ctrl+Shift+S saves everything: the level plus every dirty asset tab (saveAll(),
        // SandboxShell.cpp). Same site as LevelSave above, deliberately: pressed() doesn't consult
        // the live scope mask (EditorKeybinds.cpp), so this is already live from inside an asset tab too.
        if (!io.WantTextInput && keybinds_.pressed(editor::CommandId::SaveAll, io))
            saveAll();

        // Ctrl+Shift+B / Ctrl+Shift+R mirror the Tools menu's Compile/Reload Scripts items. Same
        // site and reasoning as SaveAll above: not a viewport gesture either.
        if (!io.WantTextInput && keybinds_.pressed(editor::CommandId::CompileScripts, io))
            tools_.requestCompileScripts(project_);
        if (!io.WantTextInput && keybinds_.pressed(editor::CommandId::ReloadScripts, io))
            tools_.requestReloadScripts(project_);

        // F frames the selection at a distance derived from its radius.
        // Outliner/Details count too: levelFocused_ is true only while the viewport holds ImGui's
        // keyboard focus, and clicking an Outliner row (the ordinary way to pick something) moves
        // focus there -- select in the list, press F, nothing happened. Reported as "press F to
        // focus is broken"; same gate that had Delete/Undo silently doing nothing from the same panel.
        // Scene-guarded as a whole: F frames a SELECTED ENTITY's bounds, and selectionBounds is
        // `#if AVER_MODULE_SCENE`, walking the selection set through the world. anySelected() isn't
        // guarded (sun/sky/post rows are selectable without a scene), but none of those has bounds
        // to frame a camera on, so the binding just does nothing there rather than framing a point
        // at the origin.
        // STANDS DOWN FOR PAWN TO CAMERA: this row never checked Shift (EditorKeybinds.cpp), so in an
        // ejected session Shift+F would send the pawn to the camera AND fly that camera off to frame
        // the selection. The other command is asked rather than the Shift key, so it still holds after
        // either has been rebound -- and asked with held(), not pressed(): this row repeats while F is
        // down, Pawn to Camera does not, so pressed() would let the second auto-repeat pulse through.
        // Only while there is a pawn to send: without one, Shift+F frames as it always did.
#if AVER_MODULE_SCENE
        if ((levelFocused_ || outlinerFocused_ || detailsFocused_) && !io.WantCaptureKeyboard &&
            keybinds_.pressed(editor::CommandId::ViewFrameSelected, io) && anySelected() &&
            !(playEjected() && hasPossessedPawn() &&
              keybinds_.held(editor::CommandId::PlayPawnToCamera, io))) {
            // selectionBounds, not selectedXform/selectedRadius: those describe only the anchor
            // (what the gizmo draws on), which put most of a spread multi-selection outside the
            // view. This is the union of the whole selection.
            Vec3 center; f32 r;
            if (selectionBounds(center, r)) {
                const f32 d = std::fmax(50.0f, r / std::tan(radians(30.0f)) * 1.6f);
                camPos_ = center - fwd * d;
                flySpeed_ = std::fmax(flySpeed_, r * 0.4f);
                // streaming_'s own `#if AVER_MODULE_SCENE` here would repeat the guard already opened above.
                streaming_.resetVelocityTracking();   // teleport; see frameCameraOnLevel for why
            }
        }
#endif  // AVER_MODULE_SCENE
    }
#endif
#if AVER_MODULE_SCRIPTING
    scripts_.update(t.dt);
#endif
#if AVER_MODULE_FRAMEWORK
    // --recapture-test opts in (the gesture it exercises lives entirely inside this block, so a
    // bounded run that skips it can't reach the bug). Safe now that re-centring refuses to move a
    // background window's cursor.
    const bool interactive = (maxFrames_ == 0 && !playTest_) || recapFrames_ > 0;
    // The #if isn't redundant with uiActive(): that's runtime, and doesn't make ImGuiIO/ImGui::
    // names below COMPILE on a UI-less build (module-matrix.ps1's no-ui/d3d12-off rows failed here).
#if AVER_WITH_IMGUI
    if (e.device()->uiActive() && interactive) {
        // Clicking the viewport puts the mouse back in the game. Tested before wantCapture below.
        {
            // levelHovered_, not !WantCaptureMouse -- the old test could never pass since ImGui
            // always wants the mouse over its own dock window (measured: wantMouse=1 always).
            // levelHovered_ asks the question actually meant -- is the pointer over the LEVEL
            // window, the same `overUI` idiom the fly camera uses.
            const ImGuiIO& mio = ImGui::GetIO();
            // Not while ejected: that click is a viewport selection, and the mouse stays free.
            if (playSessionActive() && !playEjected() && releasedByUser_ && ImGui::IsMouseClicked(0) &&
                levelHovered_ && inViewport(mio.MousePos.x, mio.MousePos.y)) {
                releasedByUser_ = false;
                // That click is NOT a trigger pull, it means "give the mouse back" -- publishing it
                // as MOUSE_LEFT would fire for as long as held (a level-triggered fire gate behind
                // a cooldown can't tell one long click from many). Eaten until button-up (see pushInput).
                eatRecaptureClick_ = true;
            }
        }
        const bool wantCapture = playSessionActive() && !releasedByUser_ && !playEjected();
        // Alt+P/Alt+S start Play, same anyPlayActive() precondition the toolbar Play button
        // disables itself on -- can't layer a second session any more than the button can.
        if (!ImGui::GetIO().WantTextInput && !anyPlayActive() &&
            keybinds_.pressed(editor::CommandId::PlayStart, ImGui::GetIO()))
            launchPlay(e, PlayMode::SelectedViewport);
        if (!ImGui::GetIO().WantTextInput && !anyPlayActive() &&
            keybinds_.pressed(editor::CommandId::PlaySimulate, ImGui::GetIO()))
            launchPlay(e, PlayMode::Simulate);
        if (keybinds_.pressed(editor::CommandId::PlayReleaseMouse, ImGui::GetIO()) && playSessionActive() && !playEjected())
            releasedByUser_ = !releasedByUser_;
        // F8: possess while ejected, eject while possessed -- one chord, gated on a real session
        // rather than on scope (KeybindRegistry::pressed() doesn't consult it either way).
        if (keybinds_.pressed(editor::CommandId::PlayEject, ImGui::GetIO()) && playSessionActive())
            togglePlayEject();
        // Shift+F, ejected only: the possessed pawn comes to the editor camera, and F8 possesses it
        // there. GATED LIKE FRAME SELECTED, with which it shares F: the level viewport (focused or
        // under the pointer), the Outliner or Details -- not an asset tab, whose own F handling ignores
        // Shift and would otherwise frame its mesh AND move the running game's pawn -- and not while
        // ImGui wants the keyboard (a capital F typed into a rename, the console, a modal). F8 needs
        // none of this: it is not a printable key and no other panel binds it.
        {
            const ImGuiIO& pio = ImGui::GetIO();
            if ((levelFocused_ || levelHovered_ || outlinerFocused_ || detailsFocused_) &&
                !pio.WantTextInput && !pio.WantCaptureKeyboard && playEjected() &&
                keybinds_.pressed(editor::CommandId::PlayPawnToCamera, pio) &&
                // With no pawn and a selection, Frame Selected took this press (its stand-down asks
                // the same question); only warn about the missing pawn when nothing else answered.
                (hasPossessedPawn() || !anySelected()))
                teleportPawnToCamera();
        }
        if (keybinds_.pressed(editor::CommandId::PlayPause, ImGui::GetIO()) && playSessionActive())
            aver_fw_set_paused(aver_fw_play_state() != AVER_FW_PLAY_PAUSED ? 1 : 0);
        if (keybinds_.pressed(editor::CommandId::PlayFrameSkip, ImGui::GetIO()))
            requestPlayFrameStep();
        // Escape stops Play-in-Editor (same as Stop). Checked here, not pushInput, so this wins
        // over the game seeing the keypress -- a script's own pause menu can't race the editor for it.
        // Escape ends a drone stand-in too: Play started it, so Play's exit ends it. Fires while
        // ejected too: playSessionActive() doesn't care which camera the session is showing.
        if (keybinds_.pressed(editor::CommandId::PlayStop, ImGui::GetIO()) && (playSessionActive() || dronePlayActive() || spectatorPlayActive()))
            stopPlay();
        // F9 screenshots the viewport, in edit mode or during Play. Checked here, not beside
        // LevelSave above: this block runs in both modes, so one check covers both; a second check
        // at LevelSave's (edit-mode-only) site would fire it twice.
        if (!ImGui::GetIO().WantTextInput && keybinds_.pressed(editor::CommandId::Screenshot, ImGui::GetIO()))
            requestViewportScreenshot();
        if (!playSessionActive()) releasedByUser_ = false;
        setMouseCaptured(wantCapture && !ImGui::GetIO().WantTextInput);
    }
#else
    (void)interactive;   // no ImGui to arbitrate mouse/keyboard capture with
#endif
    pollCapturedMouse();
    pushInput(e.device()->uiActive());
    // The UI frame opens before gameplay ticks, because ticking is when a game draws its HUD.
#if AVER_MODULE_SCRIPTING
    // --hud-preview <n>: name every HUD once, then preview one over the level viewport.
    if (hudTest_ >= 0 && !hudTestReported_ && scripts_.ready()) {
        hudTestReported_ = true;
        const i32 n = scripts_.hudCount();
        AVER_INFO("[HUD] {} declared", n);
        for (i32 i = 0; i < n; ++i) AVER_INFO("[HUD]   {}: '{}'", i, scripts_.hudName(i));
        if (hudTest_ >= n) AVER_WARN("[HUD] no HUD at index {}", hudTest_);
    }
    if (hudTest_ >= 0 && hudTest_ < scripts_.hudCount())
        setHudPreview(hudTest_, vpX_, vpY_, vpW_, vpH_);
#endif
    if (hudPreviewActive()) aver_ui_begin_frame(hudRectX_, hudRectY_, hudRectW_, hudRectH_);
    else                    aver_ui_begin_frame(vpX_, vpY_, vpW_, vpH_);
    // Font and pointer, both lent per frame, right after the frame opens. Font is re-lent every
    // frame (not once at load) since loadGameUiFont is non-fatal and can leave uiFont_ invalid;
    // handing over the same address each frame costs one store and spares a host that reloads a
    // font a second call site.
    aver_ui_set_font(&uiFont_);
    // Pointer converted here, the only place that can do it correctly: the UI frame is laid out
    // against the viewport (or HUD preview rect), not the window, so a window-relative cursor would
    // miss every rect by the dockspace's offset.
    {
        const f32 ox = hudPreviewActive() ? hudRectX_ : static_cast<f32>(vpX_);
        const f32 oy = hudPreviewActive() ? hudRectY_ : static_cast<f32>(vpY_);
        f32 px = 0.0f, py = 0.0f;
        u32 buttons = 0;
#if AVER_WITH_IMGUI
        const ImGuiIO& uiIo = ImGui::GetIO();
        px = uiIo.MousePos.x; py = uiIo.MousePos.y;
        // ImGui reports an outside cursor as -FLT_MAX; left as-is it could land inside a rect after
        // offset subtraction on a wide viewport, so it's pushed further negative to hit nothing --
        // that's what "not here" should mean.
        if (px < -1.0e6f || py < -1.0e6f) { px = -1.0e6f; py = -1.0e6f; }
        else { px -= ox; py -= oy; }
        for (int b = 0; b < 3; ++b)
            if (ImGui::IsMouseDown(static_cast<ImGuiMouseButton>(b))) buttons |= (1u << b);
#else
        (void)ox; (void)oy;
#endif
        aver_ui_set_pointer(px, py, buttons);
    }
#if AVER_MODULE_SCRIPTING
    if (hudPreviewActive()) scripts_.hudDraw(hudPreviewIndex_, t.dt);
#endif
    maybeSpawnTestActor();
    maybePlayTest();
    maybePieCameraTest();
    maybeInputStuckTest();
    maybeInputSourceTest();
    maybeWheelSpeedTest();
    maybeRecaptureTest();
    maybeViewmodelTest();
    // Frame Skip: lift the pause for exactly this frame's tick groups (through tickAi below);
    // frameStepGuard's destructor puts it back whether or not that reach happens, so a paused
    // session can never come out of this stuck running.
    if (playFrameStepPending_ && aver_fw_play_state() == AVER_FW_PLAY_PAUSED) {
        playFrameStepPending_ = false;
        aver_fw_set_paused(0);
        frameStepGuard.active = true;
    }
    // The tick groups bracket the physics step: PrePhysics -> Physics -> PostPhysics.
    if (!spawnTestClass_.empty() || aver_fw_play_state() == AVER_FW_PLAY_PLAYING) {
#if AVER_MODULE_SCENE && AVER_MODULE_PHYSICS
        // The level's cars: each driver sets its input immediately before the physics step inside
        // the groups, and each entity is written from its body immediately after them -- before
        // driveAnimatedBodies and the flush below see the frame. Inside this gate, so Pause and Frame
        // Skip hold the traffic with everything else. The focus is the view (the pawn's eye while
        // possessed, the free camera while ejected): cars far from it think less often.
        playProf_.begin(editor::PlayPhase::Vehicles);
        vehicles_.prePhysics(t.dt, camPos_);
        playProf_.end(editor::PlayPhase::Vehicles);
#endif
        playProf_.begin(editor::PlayPhase::Gameplay);
        playProf_.addPhysicsSteps(game::tickGameplayGroups(t.dt));
        playProf_.end(editor::PlayPhase::Gameplay);
#if AVER_MODULE_SCENE && AVER_MODULE_PHYSICS
        playProf_.begin(editor::PlayPhase::Vehicles);
        vehicles_.postPhysics(scene::World::instance());
        playProf_.end(editor::PlayPhase::Vehicles);
#endif
    }
#endif
#if AVER_MODULE_FLUIDS
    // After the physics step above, before any prePass -- not gated on Play.
    water_.update(*e.device(), t.dt);
#endif
#if AVER_MODULE_SCENE
    // Retires deferred destroys and propagates world matrices once, after gameplay and before onRender.
    // Animation clock runs UNCONDITIONALLY, not off the gameplay tick above (which gates on
    // PLAYING) -- hanging it there would freeze every preview outside Play mode.
#if AVER_MODULE_FRAMEWORK
    // Object animation follows Play's pause like the gameplay groups above: held while PAUSED, and a
    // Frame Skip has lifted the pause by this point so it advances exactly one frame. Set every frame
    // from the play state, so Stop or a level change can never leave it stuck.
    anim::animSystem().setObjectAnimationPaused(aver_fw_play_state() == AVER_FW_PLAY_PAUSED);
#endif
    playProf_.begin(editor::PlayPhase::ObjectAnim);
    anim::animSystem().tick(scene::World::instance(), t.dt);
    playProf_.end(editor::PlayPhase::ObjectAnim);
#if AVER_MODULE_PHYSICS
    // After the tick that moved them: an animated placement's kinematic body follows it (only while
    // Play has object animation live), which is what carries a character standing on it.
    playProf_.begin(editor::PlayPhase::DriveBodies);
    driveAnimatedBodies(t.dt);
    playProf_.end(editor::PlayPhase::DriveBodies);
#endif
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // Same "unconditionally" reasoning as the animation clock above: previews outside Play should
    // still show effects playing. Verification-only: steady_clock brackets only the CPU sim call,
    // isolated from GPU draw (already covered by --frame-time).
    {
        const auto tickStart = std::chrono::steady_clock::now();
        particles::particleSystem().tick(scene::World::instance(), t.dt);
        particleTickAccumSec_ += std::chrono::duration<f64>(std::chrono::steady_clock::now() - tickStart).count();
        ++particleTickFrames_;
    }
#endif
    // After the tick, before anything draws: update() creates the per-entity skin targets the
    // draw pass asks for and copies this frame's matrices out of AnimSystem before its skinning() invalidates.
    playProf_.begin(editor::PlayPhase::Skinned);
    if (skinnedScene_)
        skinnedScene_->update(scene::World::instance(), anim::animSystem(), *e.device());
    playProf_.end(editor::PlayPhase::Skinned);
#if AVER_MODULE_RENDER_SOFTBODY
    // After the physics step and World::flush, before the draw (drawHandle() must exist by then) --
    // same ordering skinnedScene_ above requires, for the same reason.
    if (softBodyScene_) softBodyScene_->update(scene::World::instance(), *e.device());
#endif
    // Once per frame, before the render features run (ThumbnailCache.hpp): update() points its
    // preview at the next pending request so the copy feature has something fresh to copy this frame.
    // Guarded on ready(), matching every other conditionally-initialised helper above.
    if (thumbnails_.ready()) thumbnails_.update();
    // --drone: switches the graph-driven drone on N frames in, mirroring --chunk-stream below so a
    // --frames capture can prove it without clicking Window > Drone.
    if (droneAutoFrames_ > 0 && --droneAutoFrames_ == 0) setDroneEnabled(true);
    // --undo-test: fires runUndoTest() N frames in, then exits (see its own comment). Lives inside
    // the same `#if AVER_WITH_IMGUI` block as the commands it proves, so this call site needs the same guard.
#if AVER_WITH_IMGUI
    if (undoTestAutoFrames_ > 0 && --undoTestAutoFrames_ == 0) runUndoTest(e);
    if (multiSelTestFrames_ > 0 && --multiSelTestFrames_ == 0) runMultiSelectTest(e);
    if (cbMoveTestFrames_ > 0 && --cbMoveTestFrames_ == 0) runCbMoveTest(cbMoveTestDir_);
    if (saveDirtyTestFrames_ > 0 && --saveDirtyTestFrames_ == 0) runSaveDirtyTest();
    if (findRefsFrames_ > 0 && --findRefsFrames_ == 0) runFindRefs();
    if (projectSwitchFrames_ > 0 && --projectSwitchFrames_ == 0) runProjectSwitchTest();
    if (validateGraphFrames_ > 0 && --validateGraphFrames_ == 0) runValidateGraph();
    if (graphPrintTestFrames_ > 0 && --graphPrintTestFrames_ == 0) runGraphPrintTest();
    if (clearShaderCacheFrames_ > 0 && --clearShaderCacheFrames_ == 0) runClearShaderCache();
    if (assetAssignTestFrames_ > 0 && --assetAssignTestFrames_ == 0) runAssetAssignTest();
    if (graphHitsTestFrames_ > 0 && --graphHitsTestFrames_ == 0) runGraphHitsTest();
    if (renameRepointFrames_ > 0 && --renameRepointFrames_ == 0) runRenameRepointTest();
    if (prefsWriteTestFrames_ > 0 && --prefsWriteTestFrames_ == 0) runPrefsWriteTest();
    if (notifyTestFrames_ > 0 && --notifyTestFrames_ == 0) runNotifyTest();
    // Armed once, not every frame: markLevelUnsaved is what gives the autosave timer something
    // to do, and re-marking after each save would loop forever instead of showing one cycle.
    if (autosaveTestArm_) { autosaveTestArm_ = false; markLevelUnsaved(); }
    if (keybindTestAutoFrames_ > 0 && --keybindTestAutoFrames_ == 0) runKeybindPersistTest(keybindTestMode_);
#endif
#if AVER_MODULE_SCRIPTING
    // Ticks the graph-driven drone if live. Runs BEFORE chunk streaming below so its extra
    // StreamSource (and the log line right under it) sees this frame's drone position/velocity, not last frame's.
    if (droneEntity_ != scene::kInvalidEntity && droneGraphLoaded_) {
        droneTimeSeconds_ += t.dt;
        scripts_.graphTick(static_cast<i32>(droneEntity_), droneTimeSeconds_);
        scene::World& dw = scene::World::instance();
        if (dw.valid(droneEntity_)) {
            dronePos_ = dw.localTransform(droneEntity_).position;
            const f32 droneInvDt = t.dt > 1e-6f ? 1.0f / t.dt : 0.0f;
            droneVel_ = droneHaveLastPos_ ? (dronePos_ - droneLastPos_) * droneInvDt
                                          : Vec3{0.0f, 0.0f, 0.0f};
            droneLastPos_ = dronePos_;
            droneHaveLastPos_ = true;
            // Greppable proof the drone actually MOVED, across several frames -- not just that it
            // spawned. 30 lines, not chunk streaming's 8: the ask here is a trajectory.
            if (droneLogsLeft_ > 0) {
                --droneLogsLeft_;
                AVER_INFO("[Drone] t={:.3f}s pos=({:.1f},{:.1f},{:.1f}) vel=({:.1f},{:.1f},{:.1f})cm/s",
                          droneTimeSeconds_, dronePos_.x, dronePos_.y, dronePos_.z,
                          droneVel_.x, droneVel_.y, droneVel_.z);
            }
        }
    }
    // Graph-as-class instances, gated on Play. The old defence assumed "a graph-only project never
    // calls aver_fw_begin_play" -- false, per PTTest's own logs every Play:
    //     [Graph] GameMode 'AN_FPRules' begins play with pawn='AN_FPCharacter' ...
    //     [Sandbox] Play: begin_play GameMode='AN_FPRules'
    // HostBridge registers graph classes into the same native class registry a C# GameMode uses,
    // so a graph-only project reaches Play like any other.
    // Ungated, every class-placed graph ran OnTick while someone just looked around: measured on
    // PTTest over 1000 frames with Play never pressed, AN_FPRules' `elapsed` VAR climbed from
    // 3.6e-05 to 12.31s, 4003 tick lines written, no begin_play -- a graph is free to move
    // entities, fire events and write VARs, so browsing a level mutated it.
    // Same condition as the framework tick groups above, one spelling not two (this block used to
    // sit under SCENE and SCRIPTING alone) -- which is also why it needs their AVER_MODULE_FRAMEWORK
    // guard: aver_fw_play_state() comes from framework_abi.h, a graph CLASS is ticked because Play
    // began, and with no framework there's no Play to gate on and no class instances to tick.
#if AVER_MODULE_FRAMEWORK
    playProf_.begin(editor::PlayPhase::GraphTicks);
    if (!spawnTestClass_.empty() || aver_fw_play_state() == AVER_FW_PLAY_PLAYING)
        scripts_.tickGraphClassInstances(t.dt);
    playProf_.end(editor::PlayPhase::GraphTicks);
#endif
#endif
    // --chunk-stream: switches streaming on N frames in, on its own, so a --frames capture run
    // can prove it happened without a human clicking Window > Chunk Streaming.
    if (chunkStreamAutoFrames_ > 0 && --chunkStreamAutoFrames_ == 0) setChunkStreamingEnabled(true);
    // Chunk streaming, if on. Runs here so it sees THIS frame's camPos_ (WASD/fly already
    // finalized it) and evictions land in the flush() below -- runs while idling outside Play too,
    // deliberately: that's exactly who this feature is for.
    if (streaming_.enabled()) {
        const game::GameStreaming::TriangleLookupFn tris = [this](u64 id) -> u32 {
            const auto it = meshTris_.find(id);
            return it != meshTris_.end() ? it->second : 0u;
        };
#if AVER_MODULE_SCRIPTING
        // The drone keeps its own corridor resident while it flies, not just the one around the
        // camera -- both are StreamSource entries to streaming_.tick's second-source overload.
        if (droneEntity_ != scene::kInvalidEntity && droneGraphLoaded_) {
            const world::StreamSource drone{dronePos_, droneVel_};
            streaming_.tick(camPos_, t.dt, &drone, tris);
        } else
#endif
        streaming_.tick(camPos_, t.dt, nullptr, tris);
    }
    playProf_.begin(editor::PlayPhase::WorldFlush);
    scene::World::instance().flush();
    playProf_.end(editor::PlayPhase::WorldFlush);
#if AVER_WITH_AUDIO_ABI
    // Reclaims finished voices every frame, Play or not -- the second half of the audio-device
    // gap: aver_audio_collect had the same single caller as aver_audio_init, so a graph-started
    // voice leaked its slot until the mixer ran out.
    // Not gated on Play: voices outlive a session, so gating would leak voices that finish after Stop.
    aver_audio_collect();
#endif
#if AVER_MODULE_SYNAPSE_SCENE && AVER_MODULE_FRAMEWORK
    // Gated on Play, matching the physics/fw_tick block above: pathing/movement targets are
    // gameplay, not an authoring-time preview (an AI agent chasing a goal has nothing meaningful
    // to do while nothing else in the level is moving). Same condition, re-evaluated here rather
    // than threaded through as a local.
    // nav_ may be empty or a frame stale (loadNavForLevel/navBakeCheck poll from onRender, not
    // here) -- AgentSystem::tick treats that as "wait for a grid", not an error.
    if (!spawnTestClass_.empty() || aver_fw_play_state() == AVER_FW_PLAY_PLAYING) {
        game::tickAi(t.dt, &nav_);
    }
#endif
#endif
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
    // Ejected: leave camPos_/yaw_/pitch_ exactly where the user flew them (drivePlayCamera would
    // snap the view back onto the pawn every frame) and clear the owner-hide root so the pawn's
    // body draws. Re-possessing calls drivePlayCamera() again next frame, snapping the view back.
    if (playEjected()) firstPersonPawn_ = scene::kInvalidEntity;
    else                drivePlayCamera();
#endif
    // G-buffer: pushed every frame so a live dropdown click or --gbuffer-debug takes effect
    // immediately. OR'd together: --gbuffer alone must still write with no view selected, and a
    // debug view alone must still turn it on. No module gate -- default stays OFF for the
    // render-gate oracle and the 89 headless suites.
    // Voxi's denoiser is the third reason the G-buffer exists: the denoiser is its only engine
    // consumer and reads these three targets every frame it runs, so turning it on has to turn them on too --
    // otherwise the Project Settings checkbox silently did nothing (reachable only from CLI before).
#if AVER_MODULE_VOXI
    // N3 fix: the raw `denoiser` checkbox no longer gates this alone -- a ticked denoiser that can
    // never actually run (not D3D12, RT tier Off, nothing to filter) must not still cost the ~54 MB
    // G-buffer allocation every frame. See Resolution::denoiserGBufferWanted
    // (RenderSettingsResolver.hpp) for the exact rule: wanted unless only the soft MSAA reason, if
    // even that, stands between the request and the denoiser actually running.
    voxi::Renderer& vx = voxi::Renderer::get();
    const bool wantGbufForDenoiser = voxi::resolve(vx.settings(), vx.deviceInfo()).denoiserGBufferWanted;
#else
    const bool wantGbufForDenoiser = false;
#endif
    // Frame generation reads motion and depth from the G-buffer -- the fourth reason it exists.
    const bool wantGbufForFrameGen = updateFrameGeneration(e.device());
    e.device()->setGBufferEnabled(gbufferOverride_ || wantGbufForDenoiser || wantGbufForFrameGen ||
                                  gbufferDebugView_ != GBufferDebugFeature::Mode::Off);
    gbufferDebugFeature_.setDevice(e.device());
    // vpX_/vpY_/vpW_/vpH_: this frame's 3D-viewport rect (buildViewportOverlay), a frame stale at
    // worst on the very first draw -- the same tolerance captureCheck()'s VIEWPORT-MOVED check accepts.
    gbufferDebugFeature_.setViewportRect(static_cast<u32>(vpX_), static_cast<u32>(vpY_),
                                         static_cast<u32>(vpW_), static_cast<u32>(vpH_));
    gbufferDebugFeature_.setMode(gbufferDebugView_);
#if AVER_MODULE_VOXI
    e.device()->setMeshShaders(voxi::Renderer::get().settings().meshShaders);
#else
    e.device()->setMeshShaders(msOverride_);
#endif
#if AVER_MODULE_VOXI
    // Voxi owns the AA setting; push it to the device when it changes (rebuilds targets+PSOs).
    if (voxi::Renderer::get().consumeMsaaDirty())
        e.device()->setSampleCount(static_cast<u32>(voxi::Renderer::get().settings().msaa));

    if (voxiAttached_) {
        // A copy, load-bearing: the controller must never write back into the singleton, or one
        // throttled frame would become the new authored baseline and quality could only ratchet
        // down. Project Settings still shows what was authored.
        voxi::Settings vs = voxi::Renderer::get().settings();
        frameBudgetTick(t.dt, vs);
        const f32 c[3] = {giCenter_.x, giCenter_.y, giCenter_.z};
        // Auto-switch: a ray-hit/triangles debug view can't draw through the rasteriser, and
        // Wireframe/G-buffer debug can't draw through ray-driven primary visibility -- see
        // ViewDebug's own comment (VoxiRenderer.hpp) for why they share one pass-level float.
        // Applied to this scratch copy only, like frameBudgetTick above -- never
        // written back to the singleton, so Project Settings/prefs still show what was authored,
        // and the override releases the frame that mode is left. See setViewDebug's call site
        // (above, beside setUnlit) for why these are recomputed here rather than read back from there.
        const bool needRaster = wireframe_ || gbufferDebugView_ != GBufferDebugFeature::Mode::Off;
        const bool needRayDriven = !needRaster &&
            debugView_ != voxi::VoxiRenderer::ViewDebug::None && voxiRenderer_.rayDrivenAvailable();
        if (needRaster) vs.rtRenderMode = 0;
        else if (needRayDriven) vs.rtRenderMode = 1;
        // A renderer just switched INTO starts from stale history otherwise (e.g. leaving Wireframe
        // back to ray-driven ghosts old raster through the new reprojection). Compared against last
        // frame's EFFECTIVE mode, not the authored one, so an ordinary frame with no override
        // active never resets anything.
        if (static_cast<i32>(vs.rtRenderMode) != lastEffectiveRtRenderMode_) {
            voxiRenderer_.resetRtHistory(true);
            voxiRenderer_.resetAoHistory();
            voxiRenderer_.resetGiHistory(true);
            voxiRenderer_.resetDenoiserHistory(true);
            lastEffectiveRtRenderMode_ = static_cast<i32>(vs.rtRenderMode);
        }
        // Undenoised (--view-mode undenoised / the dropdown's independent toggle): existing runtime
        // knobs applied to this scratch copy only, never persisted (see undenoised_'s declaration,
        // SandboxApp.hpp). Turns off:
        //   - the denoiser entirely (denoiser)
        //   - ray-tile amortisation (rtPixelsPerRayTile 1: no tiling, no reprojected history, no temporal blend)
        //   - RT sun-shadow spatial filter (rtShadowDenoise 0 -- the radius/take pair packed into
        //     cb_.rtDenoiseParams.xy)
        //   - ReSTIR GI's spatial reuse (giRestirSpatialSamples 0; 15 is auto; 0 "disables spatial
        //     reuse OUTRIGHT" per that field's own comment)
        // Temporal accumulation is turned off below, beside voxi.debugResetHistoryEveryFrame.
        //
        // Known gap: no runtime knob exists for the reflection spatial filter (rtReflectionSpatial,
        // CSRdReflFilter) or a sky-occlusion spatial filter -- neither is gated by any Settings
        // field today (checked against Voxi.hpp/VoxiRenderer.cpp: rtReflectionSpatial runs
        // unconditionally as part of the reflection compose, and sky occlusion has no spatial pass
        // at all, only the temporal rtSkyOcclusionHalfRate toggle), so Undenoised leaves both
        // running rather than claiming to handle them.
        if (undenoised_) {
            vs.denoiser = false;
            vs.rtPixelsPerRayTile = 1;
            vs.rtShadowDenoise = 0;
            vs.giRestirSpatialSamples = 0;
        }
        voxiRenderer_.setSettings(vs);
        // Consume-and-forward for the five reset* console commands (EditorConsole.hpp) -- one
        // request flag per history, raised on the singleton the console can reach, consumed here
        // into the VoxiRenderer instance it can't reach directly (same shape as consumeMsaaDirty() above).
        if (voxi::Renderer::get().consumeGiHistoryResetRequest())  voxiRenderer_.resetGiHistory();
        if (voxi::Renderer::get().consumeRtHistoryResetRequest())  voxiRenderer_.resetRtHistory();
        if (voxi::Renderer::get().consumeAoHistoryResetRequest())  voxiRenderer_.resetAoHistory();
        if (voxi::Renderer::get().consumeDenoiserHistoryResetRequest()) voxiRenderer_.resetDenoiserHistory();
        // voxi.debugResetHistoryEveryFrame, OR'd with Undenoised (same "no temporal accumulation"
        // reason as above): console slot is only ever READ here, so toggling Undenoised off leaves
        // it untouched.
        const u32 everyFrame = editor::consoleResetHistoryEveryFrameSlot();
        if (everyFrame || undenoised_) {
            if ((everyFrame & 1u) || undenoised_) voxiRenderer_.resetGiHistory(/*quiet=*/true);
            if ((everyFrame & 2u) || undenoised_) voxiRenderer_.resetRtHistory(/*quiet=*/true);
            if ((everyFrame & 4u) || undenoised_) voxiRenderer_.resetDenoiserHistory(/*quiet=*/true);
        }
        voxiRenderer_.setVolume(c, giExtent_);
        // Where a baked volume may be remembered. Pushed every frame like everything here; empty
        // with no project open, which disables the cache rather than scattering data beside the exe.
        // See VoxiRenderer::setGiCacheDir.
        voxiRenderer_.setGiCacheDir(project_.valid() ? fmt::giCacheDir(project_.dir) : std::string());
        voxiRenderer_.setDebugView(giDebugView_);
        // voxi.giPoisonView: EditorConsole.hpp's live source of truth, reasserted every frame like
        // occlusion.debugForceWaitIdle (see consoleGiPoisonViewSlot()).
        voxiRenderer_.setGiPoisonView(editor::consoleGiPoisonViewSlot());
        // Lighting-contrast fix's legacy bitmask: same idiom (see consoleLightingLegacySlot()/
        // setLightingLegacyBits for the bit table).
        voxiRenderer_.setLightingLegacyBits(editor::consoleLightingLegacySlot());
        // Optimisation wave 1 (M1-M4/W3/W12, C-2/C-5): same idiom (consoleGiForceRebuildSlot(),
        // EditorConsole.hpp) -- raw slots rather than ordinary dials. Each setter acts/logs only
        // on an actual change (C-2's own contract), so reasserting all three every frame is
        // free when untouched.
        voxiRenderer_.setGiForceRebuild(editor::consoleGiForceRebuildSlot());
        voxiRenderer_.setGiBoundedDispatch(editor::consoleGiBoundedDispatchSlot());
        voxiRenderer_.setGiFreeAccumulator(editor::consoleGiFreeAccumulatorSlot());
        // Optimisation wave 2 (U1/W6/M5): same idiom (consoleGiVisPathViewSlot()/
        // consoleBlendedGiConeSlot()) -- raw slots rather than ordinary dials; both setters act
        // only on an actual change, so reasserting both every frame is free when untouched.
        voxiRenderer_.setGiVisPathView(editor::consoleGiVisPathViewSlot());
        voxiRenderer_.setBlendedGiCone(editor::consoleBlendedGiConeSlot());
        // --no-gi-cone: see setGiConeTraceOff's own comment. Applied every frame, same as
        // setDebugView beside it, so it takes effect the instant the flag is set rather than only at attach time.
        voxiRenderer_.setConeTraceEnabled(!giConeTraceOff_);
#if AVER_MODULE_SR
        // Optimisation wave 2, U2 (3.3 A): resolves AverSR's level fresh every frame from the same
        // precedence chain Project Settings reads (CLI > --render-scale > Display choice > manifest
        // > Overall rung default). Not a one-shot decision: the rung it follows can change under it
        // (a scalability button, a manifest reload) -- outside beginFrame/endFrame, like every
        // other Voxi reassert in this block.
        updateAverSrAuto(e);
#endif
        const Vec3 sd = Vec3{sky_.sunDirection[0], sky_.sunDirection[1],
                             sky_.sunDirection[2]}.getSafeNormal();
        if (sunAngle_ > 0.0f) sky_.sunAngularDiameterDeg = sunAngle_;
        // Direction only: sunColor_/sunAmbient_ reach the shaders through sky_ below (:1136, :1141)
        // and the device's frame constants; passing them here too would store a second copy nothing reads.
        voxiRenderer_.setSunDirection(&sd.x);
    }
#endif
    // Confine the scene to the dockspace's central node (latched by buildUI last frame).
    e.device()->setViewportRect((u32)vpX_, (u32)vpY_, (u32)std::fmax(1.0f, vpW_), (u32)std::fmax(1.0f, vpH_));

    const Vec3 fwd = camForward();
    const f32 aspect = viewAspect();
    const game::CameraMatrices cam = game::pushCamera(*e.device(), camPos_, fwd, aspect);
    invVP_ = cam.invVP; viewProj_ = cam.viewProj; eye_ = camPos_;

    // One member for one value: used to read `fog = fogDensity_` then overwrite it with a second
    // member `levelFog_` the slider never touched, so Fog Density was dead both ways (inert when
    // the level had fog, unsaved when it didn't). Load now writes the slider's own member; save reads it.
    f32 fog = fogDensity_;
#if AVER_MODULE_SCENE
    // Opt-in, overrides the level/slider while on: matches fog density to the streaming load
    // boundary (fogDensityForOpacityAt) -- deliberately foggier, only when asked for.
    if (matchFogToStreamRadius_ && streaming_.enabled()) {
        const world::StreamSettings& st = streaming_.settings().stream;
        const f32 boundaryCm = static_cast<f32>(st.loadRadius) * static_cast<f32>(st.chunkSizeCm);
        const f32 matched = fogDensityForOpacityAt(boundaryCm, fogMatchTargetOpacity_);
        if (matched > 0.0f) fog = matched;
    }
#endif
    // FROZEN: sunDirection stays unnormalised here -- the shaders normalise it.
    sky_.enabled = showAtmosphere_;   // Show > Atmosphere; the backend already gates the sky draw on this
    for (int i = 0; i < 3; ++i) {
        sky_.sunColor[i] = sunColor_[i];
        sky_.zenith[i]   = skyZenith_[i];
        sky_.horizon[i]  = skyHorizon_[i];
        sky_.fogColor[i] = fogColor_[i];
    }
    // --sky-light N outranks the level, applied HERE not once at startup, since sunAmbient_ is
    // overwritten by applyLevelSky every time a level opens -- the same precedence bug recorded
    // several times in this file (see --gi-update-interval in the flag-override block).
    // Why the knob exists: skyLightIntensity scales the one frame term added without being
    // occluded by anything but a six-cone AO (diffAmbient, material_prelude.hlsl), previously
    // sliderable but not CLI-measurable. That share is the open question behind "washed out"
    // colours: sky is Rayleigh-blue, stone bounce is warm, so a large unoccluded ambient dilutes chroma toward grey.
    if (skyLightOverride_ >= 0.0f) sunAmbient_ = skyLightOverride_;
    // --sky-physical/--sky-authored/sun elevation, re-applied HERE for the same reason --sky-light
    // is: applyLevelSky overwrites sky_ on level open, so a startup write is gone by frame 1.
    if (skyModelOverride_ >= 0)
        sky_.model = skyModelOverride_ ? rhi::SkyModel::Physical : rhi::SkyModel::Authored;
    if (sunElevationOverride_ > -90.0f) {
        f32 elev = 0.0f, azim = 0.0f;
        sky_.sunAngles(elev, azim);
        // Azimuth is left where the level put it: the flag names an elevation, and silently
        // rotating the sun as well would make two runs differ by more than they claim to.
        sky_.setSunAngles(sunElevationOverride_, azim);
    }
    sky_.skyLightIntensity = sunAmbient_;
    sky_.fogDensity = fog;
    sky_.cloudTime = cloudTime_;
    // Underwater fog applied to a copy: sky_ is the AUTHORED sky and must stay that way, or the
    // override would accumulate every frame underwater and get saved as authored weather.
#if AVER_MODULE_FLUIDS
    e.device()->setSkyAtmosphere(water_.applyUnderwaterFog(sky_, camPos_.z));
#else
    e.device()->setSkyAtmosphere(sky_);
#endif
    // Outside the viewport rect is editor chrome, not sky.
    e.device()->setClearColor(0.055f, 0.055f, 0.062f, 1);
    // Console post.* vars write the device, not post_ -- pushing post_ unconditionally undid them a
    // frame later. If the device no longer holds what was last pushed, something changed it on
    // purpose: adopt that into post_ (so the Post panel/prefs see it), then push as usual.
    if (postPushedValid_ && !rhi::postSettingsEqual(e.device()->postProcess(), postPushed_))
        post_ = e.device()->postProcess();
    // A debug view paints a flat diagnostic colour, not a lit scene -- auto exposure/bloom/local
    // exposure would smear or re-grade it, defeating the point (an unambiguous, comparable colour
    // per pixel). Pushed as a copy of post_, with postPushed_ set to that same copy, so the
    // adopt-if-changed check above never mistakes the override for a user change and folds it into
    // post_ -- prefs, which only save post_, never see it.
    if (debugViewActiveThisFrame) {
        rhi::PostSettings dbg = post_;
        dbg.autoExposure = false;
        dbg.exposure = 1.0f;
        dbg.bloomIntensity = 0.0f;
        dbg.localExposureShadows = 0.0f;
        dbg.localExposureHighlights = 0.0f;
        e.device()->setPostProcess(dbg);
        postPushed_ = dbg;
    } else {
        e.device()->setPostProcess(post_);
        postPushed_ = post_;
    }
    postPushedValid_ = true;
}

// Tears the editor down: MCP, prefs, physics, UI textures, materials, render features, scripts.
// The process exit code. Non-zero only when a REQUESTED test mode did not pass: an ordinary
// session exits 0, an unstarted test is not a failure, but a requested test that never finished IS
// one -- silence would read as success.
int SandboxApp::exitCode() const  {
    if (skinScene_) {
        if (!skinScene_->finished()) {
            AVER_ERROR("[Skin] --skin-scene-test did not finish; reporting failure rather than "
                       "letting an unfinished run look like a pass");
            return 2;
        }
        if (!skinScene_->passed()) return 1;
    }
    // The other two skin tests used to exit 0 whatever they found. Read from the latched scalars,
    // not the objects: onShutdown runs BEFORE this and destroys both, so asking the objects here
    // would always see null and report success -- and a falsification written against that would
    // pass while proving nothing. -1 means never asked for, not a verdict.
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
    return 0;
}

void SandboxApp::onShutdown(Engine& e)  {
#if AVER_MODULE_MCP
    mcp_.stop();
#endif
    setLogSink(nullptr, nullptr);
#if AVER_MODULE_VOXI
    // Baked GI goes to disk here: volumes buffer in RAM and only reach the filesystem when the
    // budget is exceeded or right now, so without this a whole session's lighting work would be
    // thrown away and rebuilt on the next open. Before render features are torn down: giCacheFlush
    // only touches std::vectors/filesystem, but belongs with the state capture rather than after
    // the device has started coming apart.
    if (const u32 wrote = voxiRenderer_.giCacheFlush())
        AVER_INFO("[Editor] wrote {} buffered GI cache entr(ies) on shutdown", wrote);
#endif
    // ---- Capture live state before flushing it ----
#if AVER_MODULE_SCENE
    // Closing the editor is a way out of the open level that never reaches unloadLevel.
    storeLevelView();
#endif
    // flushEditorPrefs() writes the pref store but doesn't look at the editor -- it only persists
    // what saveEditorPreferences() already pushed in, which only runs from buildEditorPrefs()'s
    // tail and early-returns when Preferences is closed. Several settings bypass it entirely
    // (mouse wheel -> flySpeed_, toolbar -> wireframe_, Content Browser Tiles/List and zoom,
    // drawer grip -> drawerFrac_): change any the natural way, close the editor, value silently gone.
    // Calling the sync here reads the live members regardless of which UI last touched them --
    // safe unconditionally since setPref*/flushEditorPrefs() are no-ops on nothing dirty.
    // "Unconditionally" is about the dirty check, not the build: saveEditorPreferences is
    // `#if AVER_WITH_IMGUI` (Preferences window's own push, SandboxSettings.cpp), so it does not
    // exist in a tree built without the D3D12 ImGui backend. flushEditorPrefs stays outside
    // the guard -- it is editor::, not a panel, and a CLI-set pref still deserves to reach disk with no window to change it from.
#if AVER_WITH_IMGUI
    saveEditorPreferences();
#endif
    editor::flushEditorPrefs();
    // The manifest too, same reason plus one prefs don't have: project autosave is a 0.5s debounce
    // (kProjectAutosaveSec), so an edit immediately followed by exit was still sitting in
    // projectDirty_ when the process went -- neither exit path checked it (requestExitChecked and
    // onCloseGuard both test only anyDirty()/levelHasUnsavedEdits()), so the edit was lost with no
    // prompt and no log line.
    // Flushed, not prompted: a prompt would contradict "settings save on edit" (projectDirty_'s
    // declaration). Writing here closes every exit path at once.
    // maxFrames_ guard is load-bearing, not tidiness -- maybeAutosaveProject documents why:
    // --frames sets render settings from the CLI and applyProjectRenderSettings marks the project
    // dirty when it does, so an unguarded flush would write a capture run's flags into the user's manifest.
    if (maxFrames_ == 0 && projectDirty_ && project_.valid()) {
        std::string why;
        if (!saveProjectManifest(&why))
            AVER_WARN("[Project] could not flush unsaved settings on exit: {}", why);
    }
    editor::shutdownActorEditors();
editor::shutdownAnimEditors();
#if AVER_MODULE_PARTICLES
    editor::shutdownParticleEditors();
#endif
    setMouseCaptured(false);
#if AVER_MODULE_FLUIDS
    // Before aver_phys_shutdown below, explicitly (not left to water_'s destructor, which runs
    // after onShutdown returns, once the solver is gone and retiring a live volume would call
    // into a shut-down physics system).
    water_.shutdown(e.device());
#endif
#if AVER_FLUIDS_SIMULATED
    if (viewportIconsReady_) {
        e.device()->removeRenderFeature(&viewportIcons_);
        viewportIcons_.shutdown();
        viewportIconsReady_ = false;
    }
#endif
    // Same reason, same fix, one member along (see water_ above): GBufferDebugFeature's destructor
    // calls releaseGpu() after onShutdown returns, once the device/resource factory are gone, so
    // `res_` dangles. Measured, not feared: `--gbuffer-debug velocity --msaa 1` exited 0xC0000005
    // with 0 debug-layer errors -- a clean frame followed by a CPU access violation at teardown.
    e.device()->removeRenderFeature(&gbufferDebugFeature_);
    gbufferDebugFeature_.shutdown();
#if AVER_MODULE_PHYSICS
#if AVER_MODULE_SCENE
    // Closing the window mid-Play never reaches stopPlay: the cars' bodies and constraints come down
    // here, while the world that owns them still exists, rather than in vehicles_'s destructor after it.
    vehicles_.end();
#endif
    aver_phys_shutdown();
#endif
#if AVER_WITH_AUDIO_ABI
    // Stops the mixer, releases the device, forgets every loaded sound. Idempotent and a no-op
    // when the device was never opened -- so a build with no output device, or one that never
    // reached the init above, is unaffected.
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
#endif
#if AVER_MODULE_SR
    // Detach from the device first, then destroy: the old comment said resetting the unique_ptrs
    // was safe because "e.device() is still known good" -- the wrong question: it's
    // `dev->upscaler()`, a raw pointer applyUpscalerSlot() set, that must stop pointing here
    // first. --edge-aa's first --frames run crashed (SIGSEGV at process exit, with the
    // measurement already logged -- only teardown order was wrong). Clearing the slot for both,
    // before either reset(), is the exact bug clearAverSrUpscaler(dev) exists to prevent and was never called here.
    e.device()->setUpscaler(nullptr);
    averSrUpscaler_.reset();
    edgeAaUpscaler_.reset();
#endif
    // Same order as the upscaler, same reason: the device's raw pointer first, then the object.
    e.device()->setFrameGeneration(false);
    e.device()->setFrameGenerator(nullptr);
    frameGenerator_.reset();
    if (gameUi_) {
        e.device()->removeRenderFeature(gameUi_);
        delete gameUi_;
        gameUi_ = nullptr;
    }
    if (skinSelfTest_) {
        // Latched before the reset -- the whole point. Engine::run reads exitCode() AFTER
        // onShutdown, so a verdict left inside the object is gone by then (why --skin-test used
        // to exit 0 regardless). --skin-scene-test only escaped this by never being reset here.
        skinSelfTestExit_ = !skinSelfTest_->finished() ? 2 : (skinSelfTest_->passed() ? 0 : 1);
        e.device()->removeRenderFeature(skinSelfTest_.get());
        skinSelfTest_.reset();
    }
    if (ptFurnace_) {
        e.device()->removeRenderFeature(ptFurnace_.get());
        ptFurnace_.reset();
    }
    // GBufferDebugFeature: unconditional, not gated on gbufferOverride_/gbufferDebugView_, since
    // the feature is always registered (see onInit) so must always be unregistered here.
    if (gbufferDebugAttached_) {
        e.device()->removeRenderFeature(&gbufferDebugFeature_);
        gbufferDebugAttached_ = false;
    }
    // Routed through the same reconciler the editor's live toggle uses, not a hand-written
    // removeRenderFeature()+reset(), so exit teardown and a user "turn it off" are the same code
    // path. onShutdown() runs well outside any frame, exactly as safe as its usual onUpdate() call site.
    ptSceneViewWantEnabled_ = false;
    syncPtSceneView(e.device());
    if (skinDraw_) {
        // Latched before the reset, for the reason spelled out at skinSelfTest_ above.
        skinDrawExit_ = !skinDraw_->finished() ? 2 : (skinDraw_->passed() ? 0 : 1);
        e.device()->removeRenderFeature(skinDraw_.get());
        skinDraw_.reset();
    }
    // skinnedScene_ is declared only under AVER_MODULE_SCENE (it is the scene join, meaningless
    // without a world to join to), but this teardown block was unguarded.
#if AVER_MODULE_SCENE
    if (skinnedScene_) {
        e.device()->removeRenderFeature(skinnedScene_.get());
        skinnedScene_.reset();
    }
    // copyFeature() is exposed for exactly this call: thumbnails_ owns/unregisters its ActorPreview
    // internally, but the copy pass is the host's to remove (same shape as skinnedScene_ above).
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
    // VERIFICATION-ONLY: prints the CPU simulation cost this run measured, once, before teardown
    // -- see particleTickAccumSec_'s own comment.
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

// Frame generation's on/off for this frame (docs/rendering/FRAME_INTERPOLATION.md, decision 8):
// --frame-gen wins; otherwise Play follows the project's RENDER.FRAMEGEN and editing follows the
// Editor Preference. The device still declines frame by frame when it cannot run (vsync off, MSAA,
// no G-buffer yet) and says why once.
bool SandboxApp::updateFrameGeneration(rhi::IDevice* dev) {
    if (!dev) return false;
#if AVER_MODULE_FRAMEWORK
    const bool playing = anyPlayActive();
#else
    const bool playing = false;
#endif
    bool want = frameGenCli_ >= 0 ? frameGenCli_ >= 1
              : playing           ? project_.frameGen == 1
                                  : frameGenWhileEditing_;
    dev->setFrameGenCaptureGenerated(frameGenCli_ == 2);
    if (want && !frameGenerator_) {
        if (dev->resources()) {
            frameGenerator_ = std::make_unique<framegen::ProceduralFrameGenerator>(*dev);
            // Trained trajectory weights live beside editor.ini: per machine, shared by every project.
            const std::string dir = aver::userDataDir();
            if (!dir.empty()) frameGenerator_->setWeightsPath(dir + "\\framegen_trajectory.avnn");
            dev->setFrameGenerator(frameGenerator_.get());
        } else {
            want = false;
        }
    }
    if (frameGenerator_) {
        frameGenerator_->setTrajectory(static_cast<framegen::Trajectory>(frameGenTrajectory_));
        frameGenerator_->setTraining(frameGenTrain_);
    }
    dev->setFrameGeneration(want);
    return want;
}

} // namespace aver
