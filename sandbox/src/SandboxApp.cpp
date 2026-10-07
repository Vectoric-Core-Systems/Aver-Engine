// The one translation unit that compiles stb_image_write's implementation.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#undef STB_IMAGE_WRITE_IMPLEMENTATION
#include "SandboxApp.hpp"
#include "TextureEditor.hpp"   // makeTextureEditor; SandboxApp.hpp does not pull this one in
#include "aver/game/GameCamera.hpp"
#include "aver/game/GameTick.hpp"
#include "aver/game/GameSystemsWiring.hpp"
#include "aver/game/GameUiInput.hpp"
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

SandboxApp::SandboxApp(u64 maxFrames, bool headless, std::string beamPath, std::string shot, Tool initialTool)
    : maxFrames_(maxFrames), headless_(headless), beamPath_(std::move(beamPath)), shot_(std::move(shot)), initialTool_(initialTool) {
    setLogSink(&SandboxApp::logSink, this);
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
#if AVER_MODULE_VOXI
    // Pipelines still building off the main thread: the viewport is black with its own notification, so the splash
    // need not wait for a scene that cannot draw yet.
    if (voxiAttached_ && voxiRenderer_.pipelinesBuilding()) return true;
#endif
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

// Merge icon font into the last added font for inline rendering. Missing file is non-fatal.
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

// Upload Compile C# button's three-tile status sprite sheet. Missing file is non-fatal.
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
// Load from .ocfont file staged beside exe. Missing font is non-fatal.
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

    // Atlas sits beside .ocfont. Use only the filename (editor's copy is staged flat).
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
// Install ImGui backend before uiInit() runs (onInit is too late).
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

// Build editor: asset editors, physics, scene, gizmos, render features.
void SandboxApp::onInit(Engine& e)  {
    // FIRST: set engineForSplash_ before anything logs (logSink uses it to update splash).
    engineForSplash_ = &e;
    mainThreadId_ = std::this_thread::get_id();
    AVER_INFO("[Sandbox] backend={} adapter='{}'", rhi::backendName(e.device()->backend()), e.device()->adapterName());
    // Latched for Project Settings display (no Engine& to re-ask).
    runningBackend_ = rhi::backendName(e.device()->backend());
#if AVER_MODULE_SR
    // AverSR quality drives renderScaleOverride_; --render-scale wins if both set.
    if (averSrQuality_ != aver::sr::Quality::Off && renderScaleOverride_ == 1.0f)
        renderScaleOverride_ = aver::sr::renderScaleFor(averSrQuality_);
#endif
    // Apply render scale once, before device sizes are determined.
    if (renderScaleOverride_ != 1.0f) {
        e.device()->setRenderScale(renderScaleOverride_);
        AVER_INFO("[Sandbox] render scale {:.2f} (--render-scale)", e.device()->renderScale());
    }
#if AVER_MODULE_SR
    // Build GPU upscaler when non-Off quality is set.
    if (averSrQuality_ != aver::sr::Quality::Off) {
        ensureAverSrUpscaler(e.device());
        logAverSrActive(e.device());
    }
    applyUpscalerSlot(e.device());   // a manual scale or edge AA wants FSR too
#endif
    // Depth-only pass before opaque walk. See entity loop for two-walk mechanism.
    if (depthPrepassOverride_) {
        e.device()->setDepthPrepassEnabled(true);
        AVER_INFO("[Sandbox] depth prepass enabled (--depth-prepass)");
    }

    // Register GBuffer debug unconditionally (mode defaults to Off). Per-frame write gated in onUpdate.
    e.device()->addRenderFeature(&gbufferDebugFeature_);
    gbufferDebugAttached_ = true;
#if AVER_WITH_SYNAPSE_AI && AVER_MODULE_SYNAPSE_GPU
    crowdGpuAttached_ = game::installCrowdGpu(*e.device(), crowdGpu_);
#endif
    // NeuraFI viz draws nothing until Neural Visualiser picks a view.
    e.device()->addRenderFeature(&neurafiViz_);

    // Registration order is precedence: first factory that accepts path wins.
    assetEditors_.registerFactory(&editor::makeMeshEditor);
    assetEditors_.registerFactory(&editor::makeActorEditor);
    assetEditors_.registerFactory(&editor::makeAnimEditor);
    // Append graph editor (order matters; would change which editor claims shared file types).
    assetEditors_.registerFactory(&editor::makeGraphEditor);
    // Wire graph validator via lambda to avoid .NET dependency in GraphEditor.
#if AVER_MODULE_SCRIPTING
    editor::setGraphValidator([this](const std::string& text, std::string& err) {
        if (!scripts_.graphValidateAvailable()) { err = "the .NET bridge exports no GraphValidate"; return false; }
        return scripts_.graphValidate(text, err);
    });
    // Arm node-hit recording (keyed by graph name; off costs one bool test).
    editor::setGraphNodeHitSource([this](const std::string& graphName, f32 maxAge,
                                         std::vector<std::pair<std::string, f32>>& out) {
        scripts_.graphNodeHits(graphName, maxAge, out);
    });
    scripts_.graphSetHitRecording(true);
#endif  // AVER_MODULE_SCRIPTING
    // The visual behaviour-tree editor claims .ocbt ahead of the plain list editor (registration order is
    // precedence); BtEditor stays registered as the fallback.
    assetEditors_.registerFactory(&editor::makeBtGraphEditor);
    // Append BtEditor (claims .ocbt).
    assetEditors_.registerFactory(&editor::makeBtEditor);
    assetEditors_.registerFactory(&editor::makeUiLayoutEditor);            // .ocui
    assetEditors_.registerFactory(&editor::makeBlendSpaceEditor);          // .ocblend
    assetEditors_.registerFactory(&editor::makeAnimStateMachineEditor);    // .ocasm
    // Append SoundEditor (claims .ocsnd).
    assetEditors_.registerFactory(&editor::makeSoundEditor);
#if AVER_MODULE_PARTICLES
    assetEditors_.registerFactory(&editor::makeParticleEditor);  // .ocparticle
#endif
    assetEditors_.registerFactory(&editor::makeFoliageTypeEditor);  // .ocfoliage
    assetEditors_.registerFactory(&editor::makeInputSchemeEditor);  // .ocinput
    assetEditors_.registerFactory(&editor::makeTextureEditor);  // image formats, read-only
    {
        // Hooks to query/set project's Input Scheme.
        editor::InputSchemeEditorHooks hooks;
        hooks.contentDir = [this] { return project_.contentDir(); };
        hooks.projectInputScheme = [this] { return project_.inputScheme; };
        hooks.useAsProjectInputScheme = [this](const std::string& contentRelativePath) {
            if (!project_.valid()) return false;
            // Write happens on autosave timer or Project Settings Save button.
            project_.inputScheme = contentRelativePath;
            projectDirty_ = true;
            return true;
        };
        editor::setInputSchemeEditorHooks(std::move(hooks));
    }
    // Locate AverDesign before ActorEditor (caches Roslyn availability).
    locateAverDesign();
    {
        editor::ActorEditorHooks hooks;
        hooks.compileScripts = [this] { tools_.triggerToolbarCompile(project_); };
        hooks.compileBusy    = [this] { return tools_.compiling(); };
        hooks.drawCompileButton = [this] { tools_.drawCompileButton(project_, dpi_, compileIconUiId_); };
#if AVER_WITH_IMGUI
        hooks.openInIde = [this](const std::string& p) {
            const editor::IdeInfo& ide = cbIde();
            if (!editor::openInIde(ide, p)) AVER_WARN("[Editor] could not open {} in {}", p, ide.name);
        };
        hooks.ideName = cbIde().name;
#endif
        editor::setActorEditorHooks(std::move(hooks));
    }
    window_ = e.window();
    // Set event sink. Everything lands in accumulator unfiltered.
    if (window_) window_->setEventCallback(&sandboxWindowEvent, &input_);
    // Enable drag-drop from Explorer (polled once per frame in onUpdate).
    if (window_) window_->setAcceptDroppedFiles(true);

    // Single-instance forwarding receiver (singleInstanceEligible_ gates both registration and hook).
    if (window_ && window_->valid() && singleInstanceEligible_) {
        Window::registerAsSingleInstancePrimary(window_->nativeHandle());
#if AVER_MODULE_SCENE
        window_->setOpenRequestHook(&SandboxApp::onOpenRequestThunk, this);
#endif
    }
    // Window X button goes through same unsaved-changes check as File > Exit.
    if (window_ && window_->valid()) window_->setCloseGuard(&SandboxApp::onCloseGuardThunk, this);

#if AVER_MODULE_MCP
    // Start MCP channel if port is set (ABI registration, widget hooks, listen shared with status bar).
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
    // Register synapse components before project opens (needs no level, no physics).
    synapse::agentSystem().registerComponents(scene::World::instance());
    synapse::perceptionSystem().registerComponents(scene::World::instance());
    synapse::btSystem().registerComponents(scene::World::instance());
    synapse::registerBuiltinBehaviors(synapse::btSystem());
#endif

#if AVER_MODULE_SCENE
    // Control rig: register and install before project opens (needs no level).
    anim::controlRigSystem().registerComponents(scene::World::instance());
    anim::controlRigSystem().install(anim::animSystem(), scene::World::instance());
#endif
    // Animation state machines, reverb zones, blackboard relay, crowd/hearing/cover behaviours.
    game::registerGameSystems();
#if AVER_MODULE_SCENE
    installPrefabHooks();
#endif

#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
    // Off unless --lod-mesh-shader (opt-in). D3D12 only; needs AVER_MODULE_VOXI for GI/shadows.
    lodMeshShaderEnabled_ = lodMeshShaderRequest_ > 0;
    if (lodMeshShaderEnabled_) {
        const rhi::DeviceCaps mcaps = e.device()->caps();
        // D3D12 only (Voxi table-0 merge is D3D12-specific).
        const bool ok = mcaps.meshShaderTier > 0 && mcaps.shaderModel >= 65 && mcaps.dxcAvailable
#if AVER_MODULE_VOXI
                        && e.device()->backend() == rhi::Backend::D3D12
#endif
                        ;
        lodMeshShaderEnabled_ = ok;
        if (ok) {
#if AVER_MODULE_VOXI
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
        // --open-legacy skips upgrade prompt and opens old project directly.
        std::string err;
        const bool opened = openLegacy_ ? browser_.open(projectPath_, &err)
                                        : browser_.openOrOfferUpgrade(projectPath_, &err);
        if (opened) applyProject(e);
        else if (!openLegacy_ && browser_.upgradePending()) {
            armBrowser(true);
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
        // Load level with no project. Everything else hangs off applyProject.
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
    // Default blank map: ground + cube + sun + sky + atmosphere.
    std::vector<rhi::MeshVertex> gv, gi_v; std::vector<u32> gi, ci;
    appendGround(gv, gi, kEditorFloorHalf);
    rhi::MeshHandle ground = e.device()->createMesh(gv.data(), (u32)gv.size(), gi.data(), (u32)gi.size());
    // Unit cube stays half-extent 1 (.ocworld PLACEG applies cm scales to it).
    appendBox(gi_v, ci, 0,0,0, kEditorCubeHalf);
    rhi::MeshHandle cube = e.device()->createMesh(gi_v.data(), (u32)gi_v.size(), ci.data(), (u32)ci.size());

#if AVER_MODULE_SCENE
    // Register built-in primitive meshes and seed per-mesh pick geometry.
    {
        MeshLoadPass pass; pass.app = this; pass.engine = &e;
        content_.setMeshLoadedHook(&SandboxApp::onMeshLoaded, &pass);
        content_.registerBuiltins(*e.device());
        content_.setMeshLoadedHook(nullptr, nullptr);
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
    // Sculpt brush footprint ring (flat in XY plane; reused, radius/position vary per-frame).
    { auto bring = buildRotRing(2, kAxisHi); brushRing_ = e.device()->createLineMesh(bring.data(), (u32)bring.size()); }
#endif

#if AVER_MODULE_SCENE
    // Scene join, registered before Voxi (reads vertex buffers in prePass).
    skinnedScene_ = std::make_unique<aver::render::SkinnedScene>();
    if (skinnedScene_->init(*e.device())) {
        skinnedScene_->setResolvers(&game::GameContent::resolveAnimAsset, &game::GameContent::resolveSceneMesh, &content_);
        e.device()->addRenderFeature(skinnedScene_.get());
    } else {
        skinnedScene_.reset();   // skinned entities draw at rest
    }
#if AVER_MODULE_RENDER_SOFTBODY
    // Reuse skinnedScene_'s resolver pair.
    softBodyScene_ = std::make_unique<aver::render::SoftBodyScene>();
    if (softBodyScene_->init(*e.device())) {
        softBodyScene_->setResolvers(&game::GameContent::resolveSceneMesh, &game::GameContent::resolveAnimAsset, &content_);
        e.device()->addRenderFeature(softBodyScene_.get());
    } else {
        softBodyScene_.reset();   // soft entities draw their authored mesh
    }
#endif

    // Content browser thumbnails (inits own features; failed init -> textureId is 0).
    thumbnails_.init(*e.device());

    // Test rigged mesh through AnimSystem and SkinnedScene (--skin-scene-test <dir>).
    if (!skinSceneDir_.empty() && skinnedScene_) {
        skinScene_ = std::make_unique<aver::editor::SkinSceneTest>();
        u64 meshId = 0, skelId = 0, clipId = 0;
        u32 meshHandle = 0;
        if (skinScene_->setup(e, skinSceneDir_, &meshId, &skelId, &clipId, &meshHandle)) {
            std::pair<Vec3, Vec3> bounds;
            skinScene_->restBounds(bounds.first, bounds.second);
            content_.registerMesh(meshId, meshHandle, bounds);
            std::string d = skinSceneDir_;
            while (!d.empty() && (d.back() == 92 || d.back() == '/')) d.pop_back();
            content_.indexAsset(meshId, d + "/Rig.ocmesh");
            content_.indexAsset(skelId, d + "/Rig.ocskel");
            content_.indexAsset(clipId, d + "/Rig_Bend.ocanim");
            anim::animSystem().setResolver(&game::GameContent::resolveAnimAsset, &content_);
            objects_.clear();   // nothing unrelated on screen
            sel_ = -1;
        } else {
            skinScene_.reset();
        }
    } else if (!skinSceneDir_.empty()) {
        AVER_ERROR("[Skin] --skin-scene-test needs the scene join, which did not initialise");
    }
#endif

#if AVER_MODULE_FLUIDS
    // Register fluid (spawned only if level asks for it). Must precede Voxi (reads vertex buffer).
    water_.init(*e.device());
#endif

#if AVER_FLUIDS_SIMULATED
    // 3D-viewport icon renderer (overlayPass; must exist before level loading creates Player Start).
    viewportIconsReady_ = viewportIcons_.init(*e.device());
    if (viewportIconsReady_) {
        e.device()->addRenderFeature(&viewportIcons_);
        playerStartIcon_ = viewportIcons_.loadIcon(executableDir() + "\\" + "player-start-icon.png",
                                                   "PlayerStartIcon");
        if (playerStartIcon_ == editor::ViewportIconRenderer::kNoIcon) viewportIconsReady_ = false;
    }
#endif

    // Furnace test: shading model energy conservation with uniform white diffuse (--furnace-test).
    if (furnaceTest_) {
        objects_.clear();
        sel_ = -1;
        sky_.furnaceRadiance = 0.25f;
        sky_.furnaceSun = furnaceSun_;
        sunAmbient_ = 1.0f;

        // Albedo 1, fully rough, non-metallic white surfaces.
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
        white("FurnaceFloor", Vec3{0, 0, -30}, Vec3{900.0f, 900.0f, 20.0f});
        white("FurnaceTall",  Vec3{0, 0, 200}, Vec3{160.0f, 160.0f, 220.0f});
        white("FurnaceCaveBack",  Vec3{-520, -520, 160}, Vec3{20.0f, 200.0f, 200.0f});
        white("FurnaceCaveLeft",  Vec3{-330, -700, 160}, Vec3{200.0f, 20.0f, 200.0f});
        white("FurnaceCaveTop",   Vec3{-330, -520, 350}, Vec3{200.0f, 200.0f, 20.0f});

        // Furnace grid: specular half (6 roughness x 3 metallic at albedo 1).
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
                    // Thin plate; tilt shows glancing incidence (--furnace-tilt).
                    o.scale = Vec3{0.5f, 60.0f, 60.0f};
                    o.rotDeg = Vec3{furnaceTilt_, 0.0f, 0.0f};
                    o.color[0] = o.color[1] = o.color[2] = 1.0f;
                    o.metallic  = metal[mi];
                    o.roughness = rough[ri];
                    o.aabbMin = Vec3{-o.scale.x*1.1f, -o.scale.y*1.1f, -o.scale.z*1.1f};
                    o.aabbMax = Vec3{ o.scale.x*1.1f,  o.scale.y*1.1f,  o.scale.z*1.1f};
                    objects_.push_back(o);
                }
            }
            // Give plates materials only when a coat is asked for (normally use per-draw fallback).
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

    // Reflection test: mirror and beacon to verify ray-traced reflection is global (--refl-test).
    if (reflTest_) {
        refl_ = std::make_unique<aver::editor::ReflTest>();
        objects_.clear();
        sel_ = -1;

        // Metallic near-smooth mirror.
        MeshObj m;
        m.name = "ReflMirror";
        m.mesh = unitCubeMesh_;
        m.pos = Vec3{0, 0, -20.0f};
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
        // Bright green beacon (unique color; reflection lands outside voxel volume).
        bcn.scale = Vec3{700.0f, 700.0f, 700.0f};
        bcn.color[0] = 0.02f; bcn.color[1] = 0.95f; bcn.color[2] = 0.05f;
        bcn.roughness = 1.0f;
        bcn.aabbMin = Vec3{-950, -950, -950}; bcn.aabbMax = Vec3{950, 950, 950};
        objects_.push_back(bcn);
        reflBeaconIndex_ = static_cast<int>(objects_.size()) - 1;

        refl_->setBeaconPosition(bp);
        refl_->setVolumeExtent(giExtent_);
    }

    // Test GPU skinning: register before scene renderer (reads vertex buffer this writes).
    if (skinDrawTest_) {
        skinDraw_ = std::make_unique<aver::editor::SkinDrawTest>();
        if (skinDraw_->init(*e.device())) {
            e.device()->addRenderFeature(skinDraw_.get());
            objects_.clear();
            sel_ = -1;
            // Ground pale/rough (measures light reach).
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

        // Install animation notify sink (survives level reload; needs AVER_MODULE_SCENE).
#if AVER_MODULE_SCENE
        if (scripts_.graphFireAvailable()) {
            anim::animSystem().setNotifySink(&SandboxApp::animNotify, this);
#if AVER_MODULE_SYNAPSE_SCENE
            // Same sink for synapse (SynapsePerception.hpp).
            synapse::perceptionSystem().setNotifySink(&SandboxApp::animNotify, this);
            synapse::btSystem().setNotifySink(&SandboxApp::animNotify, this);
#endif
        }
#endif

        // Install animation curve provider (framework relay).
#if AVER_MODULE_FRAMEWORK
        aver_fw_set_anim_curve_provider(&SandboxApp::animCurve, this);
#endif

#if AVER_MODULE_SYNAPSE_SCENE
#if AVER_MODULE_FRAMEWORK
        // Install synapse target and perception providers (Aver Node queries).
        aver_fw_set_synapse_target_provider(&SandboxApp::synapseTarget, this);
        aver_fw_set_synapse_perception_provider(&SandboxApp::synapsePerception, this);
        // Perception resolver (composition root link only).
        synapse::perceptionSystem().setTargetResolver(&SandboxApp::synapseTargetResolver, this);
#endif
#endif

        // Install save/load providers (guarded on both SCENE and FRAMEWORK).
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
        aver_fw_set_save_provider(&saveWriteProvider, &saveLoadProvider, this);
#endif

        // Declare graph classes (retry after scripting host bootstraps).
#if AVER_MODULE_FRAMEWORK
        if (scripts_.ready() && project_.valid()) {
            const i32 graphClasses = scripts_.declareGraphClasses(project_.contentDir());
            if (graphClasses > 0)
                AVER_INFO("[Graph] {} graph class(es) declared from '{}'", graphClasses, project_.contentDir());
            spawnClassPlacements();
        }
#endif

#if AVER_MODULE_VOXI
    // Set GPU capabilities and Voxi device info.
    {
        const rhi::DeviceCaps caps = e.device()->caps();

        voxi::DeviceInfo di;
        di.msaaMask = caps.msaaMask; di.maxMsaaSamples = caps.maxMsaaSamples;
        di.rayTracingTier = caps.rayTracingTier; di.computeShaders = caps.computeShaders;
        di.typedUavLoads = caps.typedUavLoads; di.conservativeRaster = caps.conservativeRaster;
        di.shaderModel = caps.shaderModel; di.meshShaderTier = caps.meshShaderTier;
        di.dxcAvailable = caps.dxcAvailable;
        // Denoiser support (D3D12-specific; mirrored in GameApp::attachVoxi).
        di.denoiserSupported = e.device()->backend() == rhi::Backend::D3D12;
        voxi::Renderer::get().setDeviceInfo(di);

        // Apply project Voxi settings BEFORE flags and BEFORE init() (createVoxelVolume uses them).
        // Before flags so flag overwrites preserve "flag > manifest" precedence.
        applyProjectVoxiSettings();

        voxi::Settings s = voxi::Renderer::get().settings();
        s.msaa = static_cast<voxi::Msaa>(e.device()->sampleCount());
        if (msaaOverride_) s.msaa = static_cast<voxi::Msaa>(msaaOverride_);
        // --no-gi/no-rt win over --gi/--rt (force-off take precedence).
        if (giForceOff_)          s.globalIllumination = voxi::Quality::Off;
        else if (giOverride_ >= 0) s.globalIllumination = static_cast<voxi::Quality>(giOverride_);
        if (rtForceOff_)          s.rayTracing = voxi::Quality::Off;
        else if (rtOverride_ >= 0) s.rayTracing = static_cast<voxi::Quality>(rtOverride_);
        if (ptOverride_ >= 0) s.pathTracing = static_cast<voxi::Quality>(ptOverride_);
        if (msOverride_) s.meshShaders = true;
        // Apply tier settings first, then knobs in second call (setSettings re-runs each frame).
        voxi::Renderer::get().setSettings(s);
        auto k = voxi::Renderer::get().settings();
        if (rtRaysOverride_ > 0) k.rtShadowRays = static_cast<u32>(rtRaysOverride_);
        else if (rtRaysOverride_ < 0)
            AVER_WARN("[Sandbox] --rt-rays {} is not a ray count; the default of {} stands",
                      rtRaysOverride_, k.rtShadowRays);
        if (rtShadowDenoiseOverride_ >= 0) k.rtShadowDenoise = static_cast<u32>(rtShadowDenoiseOverride_);
        if (rtRenderModeOverride_    >= 0) k.rtRenderMode    = static_cast<u32>(rtRenderModeOverride_);
        if (ptBouncesOverride_       >= 0) k.ptBounces       = static_cast<u32>(ptBouncesOverride_);
        if (layeredBsdfOverride_     >= 0) k.layeredBsdf     = static_cast<voxi::Quality>(layeredBsdfOverride_);
        // GI sky occlusion rays derive from RT tier change (set in second call only).
        if (giSkyOccRaysOverride_ >= 0) k.giSkyOcclusionRays = static_cast<u32>(giSkyOccRaysOverride_);
        if (giSkyOccTileOverride_ >= 0) k.giSkyOcclusionTile = static_cast<u32>(giSkyOccTileOverride_);
        if (rtPixelsPerRayOverride_ > 0) k.rtPixelsPerRayTile = static_cast<u32>(rtPixelsPerRayOverride_);
        else if (rtPixelsPerRayOverride_ < 0)
            AVER_WARN("[Sandbox] --rt-pixels-per-ray {} is not a tile edge; the default of {} stands",
                      rtPixelsPerRayOverride_, k.rtPixelsPerRayTile);
        // Refraction mode is tier-derived (set in second call only).
        if (refractionOverride_ >= 0)          k.refractionMode = static_cast<u32>(refractionOverride_);
        if (refractionStrengthOverride_ >= 0.0f) k.refractionStrength = refractionStrengthOverride_;
        if (refractionFadeOverride_ >= 0.0f)     k.refractionEdgeFade = refractionFadeOverride_;
        if (giUpdateIntervalOverride_ > 0) k.giUpdateInterval = static_cast<u32>(giUpdateIntervalOverride_);
        else if (giUpdateIntervalOverride_ < 0)
            AVER_WARN("[Sandbox] --gi-update-interval {} is not a frame count; the default of {} stands",
                      giUpdateIntervalOverride_, k.giUpdateInterval);
        if (giModeOverride_ >= 0) k.giMode = static_cast<u32>(giModeOverride_);
        if (denoiserOverride_ >= 0) voxi::setDenoiserMode(k, denoiserOverride_ > 2 ? 2u : static_cast<u32>(denoiserOverride_));
        voxi::Renderer::get().setSettings(k);
        AVER_INFO("[Voxi] attached: MSAA {}x, RT tier {}, SM {}, mesh tier {}", caps.maxMsaaSamples, caps.rayTracingTier, caps.shaderModel, caps.meshShaderTier);

        // VoxiRenderer non-owning; torn down in onShutdown. Read back settings to see clamping.
        voxiRenderer_.setSettings(voxi::Renderer::get().settings());
        // NRD2's network: the user's trained weights over the shipped ones.
        voxiRenderer_.setNrd2WeightsPaths(
            aver::userDataDir().empty() ? std::string() : aver::userDataDir() + "\\" + render::denoise::kNrd2WeightsFileName,
            aver::executableDir() + "\\data\\" + render::denoise::kNrd2WeightsFileName);
        if (frameTimeReport_) voxiRenderer_.setFrameTimeReport(true);
        // Set measurement dials before init().
        if (rdAblate_) {
            voxiRenderer_.setRayDrivenAblation(static_cast<u32>(rdAblate_));
            AVER_WARN("[Sandbox] --rd-ablate {}: the ray-driven frame is DELIBERATELY WRONG. "
                      "This is a stopwatch for attributing PSRayDriven's cost, not a quality "
                      "setting -- see AVER_RD_ABLATE in voxi.hlsl.", rdAblate_);
        }
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
            // Deferred import handshake (UI-only; needs browser).
            if (!importSrc_.empty() && !importDone_) {
                importDone_ = true;
                importAsset(importSrc_, importDst_);
            }
#endif
#if AVER_MODULE_PBR
            content_.setTextureFactory(e.device()->resources());
            voxiRenderer_.materials().setTextureResolver(&game::GameContent::resolveMaterialTexture, &content_);
            // Same resolver for asset tabs' preview (consistent look across tabs and level).
            editor::setPreviewTextureResolver(&game::GameContent::resolveMaterialTexture, &content_);
            editor::setPreviewMaterialLookup(
                [](const std::string& surface, void* user) -> u32 {
                    return static_cast<u32>(static_cast<game::GameContent*>(user)->materialForSurface(surface));
                }, &content_);
#endif
            // Depth proxy installed unconditionally (empty on builds without Trifactor).
            voxiRenderer_.setDepthProxy(&SandboxApp::depthProxyLookup, this);
        }
    }
#endif
    // The game UI's render feature: overlay pass only, so ordering against scene features is free.
    gameUi_ = aver::render::ui::UiRenderer::create(*e.device());
    if (gameUi_) e.device()->addRenderFeature(gameUi_);

#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
    // Occlusion culler (not IRenderFeature; calls renderSceneEntities() directly).
    if (rhi::IResourceFactory* occRes = e.device()->resources()) {
        occluder_ = aver::occlusion::createOcclusionCuller(*occRes);
        // Sync --occlusion-waitidle flag (console and CLI stay in sync).
        editor::consoleOcclusionForceWaitIdleSlot() = occlusionDebugForceWaitIdleArg_;
        occluder_->setDebugForceWaitIdle(occlusionDebugForceWaitIdleArg_);
        // Warm up compute pipelines before frame loop (required for pointer stability).
        rhi::TextureDesc occSceneDesc;
        if (const rhi::TextureHandle occDepth = e.device()->sceneDepthTexture();
            occDepth && occRes->textureInfo(occDepth, occSceneDesc)) {
            occluder_->ensureSized(*occRes, occSceneDesc.width, occSceneDesc.height, e.device()->sampleCount());
        }
    }
#endif

#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // Particle renderer (registered unconditionally; system/effects are singletons).
    particles::particleSystem().setEffectLibrary(&particles::particleEffects());
    if (particleRenderer_.init(*e.device())) {
        particleRenderer_.setSystem(&particles::particleSystem());
        e.device()->addRenderFeature(&particleRenderer_);
        particlesAttached_ = true;
#if AVER_MODULE_VOXI
        // Install GI seam once Voxi has attached (gated on --no-particle-gi).
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
    // Water renderer (failed init means no water; HLSL compiles at runtime).
    if (waterEnabled_) {
        // Default swell: 4 non-harmonic waves.
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
            // Set physics plane from water level (rendering and buoyancy are separate capabilities).
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

    // Particle test: dust cloud overlapping cube (shows transparent pass depth test).
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

        // Ember emitter (receivesGI=false; additive; non-GI part of seam test).
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

        // High above dust cloud, open sky (tests transparent-pass/sky ordering).
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
    // Particle stress: N grid emitters (measures cost, tests parity/price pass).
    else if (particleStressEmitters_ > 0) {
        objects_.clear();
        sel_ = -1;

        particles::ParticleEffect fx;
        fx.shape = particles::EmitterShape::Box;
        fx.shapeSize = Vec3{350.0f, 60.0f, 90.0f};
        fx.emissionRate = 150.0f;   // steady fog/mist emission rate
        fx.maxParticles = static_cast<u32>(particleStressMaxParticles_ > 0 ? particleStressMaxParticles_ : 400);
        // Burst whole cap on frame 1 to reach steady-state cost immediately.
        fx.burstCount = fx.maxParticles;
        // Long lifetime keeps burst-filled population steady across capture window.
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

        // Add second additive emitter to cover both blend pipelines.
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

    // GPU skinning self-test (--skin-test; costs waitIdle).
    if (skinTest_) {
        skinSelfTest_ = std::make_unique<aver::render::SkinSelfTest>();
        if (skinSelfTest_->init(*e.device())) e.device()->addRenderFeature(skinSelfTest_.get());
        else { AVER_ERROR("[Skin] self-test unavailable on this device"); skinSelfTest_.reset(); }
    }

    // Path tracer furnace test (energy conservation; --pt-furnace).
    if (ptFurnaceTest_) {
        ptFurnace_ = std::make_unique<aver::pt::PtFurnaceTest>();
        if (ptFurnace_->init(*e.device())) e.device()->addRenderFeature(ptFurnace_.get());
        else { AVER_ERROR("[PT] furnace unavailable on this device"); ptFurnace_.reset(); }
    }

    // Path tracer scene view (progressive still-camera reference; --pt-scene).
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
    // Restored camera counts as framed (level can have CAMERA record with no placements).
    if (levelCameraRestored_) framedByLevel = true;
    if (!framedByLevel) {
        camPos_ = Vec3{700.0f, 700.0f, 450.0f};
        const Vec3 d = (Vec3{0,0,1} - camPos_).getSafeNormal();
        yaw_ = std::atan2(d.y, d.x);
        pitch_ = std::asin(d.z);
    }
    // Apply --cam override last (outranks level framing and default).
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
    // Forced unpause on scope exit (holds through all exit paths).
    struct FrameStepGuard {
        bool active = false;
        ~FrameStepGuard() {
#if AVER_MODULE_FRAMEWORK
            if (active) aver_fw_set_paused(1);
#endif
        }
    } frameStepGuard;
    // --set NAME VALUE applied at frame 5 (after project's RENDER.* apply).
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
    // Camera set first so downstream (view matrix, reprojection, shadow history) sees one consistent frame.
    bool debugViewActiveThisFrame = false;
#if AVER_MODULE_VOXI
    // View mode reaches Voxi from onUpdate (before prePass), unlike unlit which rides per-draw state.
    voxiRenderer_.setUnlit(unlit_);
    // Debug views need rasteriser; reasserted here before prePass for the same reason.
    const bool viewModeNeedsRaster = wireframe_ || gbufferDebugView_ != GBufferDebugFeature::Mode::Off;
    // Raster-only mode clears ray-hit/triangles view (wireframe_ can be ticked from Settings too).
    if (viewModeNeedsRaster) debugView_ = voxi::VoxiRenderer::ViewDebug::None;
    const bool viewModeNeedsRayDriven = !viewModeNeedsRaster &&
        debugView_ != voxi::VoxiRenderer::ViewDebug::None && voxiRenderer_.rayDrivenAvailable();
    voxiRenderer_.setViewDebug(viewModeNeedsRayDriven ? debugView_ : voxi::VoxiRenderer::ViewDebug::None);
    debugViewActiveThisFrame = viewModeNeedsRayDriven;
    // Wireframe disables shadows, GI, ray tracing and denoising (see setPaused).
    voxiRenderer_.setPaused(wireframe_);
#endif
    // --cam-wobble-stop N: wobble runs only below frame N, then holds. Measuring artifact appearance/decay needs both.
    if (camWobbleDeg_ != 0.0f && camWobblePeriod_ > 0 &&
        (camWobbleStopFrame_ <= 0 || t.frame < (u64)camWobbleStopFrame_)) {
        if (!camWobbleBased_) { camWobbleBaseYaw_ = yaw_; camWobbleBased_ = true; }
        const f32 phase = 6.2831853f * (f32)(t.frame - 1) / (f32)camWobblePeriod_;
        yaw_ = camWobbleBaseYaw_ + camWobbleDeg_ * 0.01745329252f * std::sin(phase);
    }
    // --cam-translate SPEED: one fixed step per frame along camForward(), composed with wobble.
    if (camTranslateSpeed_ != 0.0f &&
        (camWobbleStopFrame_ <= 0 || t.frame < (u64)camWobbleStopFrame_)) {
        camPos_ += camForward() * camTranslateSpeed_;
    }
    // --cam-wander AMP SPEED: follows sum of sines at irrational frequencies (NeuraFI training trajectory).
    // NeuRAA's training capture holds it still while it captures a pose, then it resumes where it was.
#if AVER_MODULE_SR
    bool neuraaHold = neuraa_ && neuraa_->captureHolding();
#else
    bool neuraaHold = false;
#endif
#if AVER_MODULE_VOXI
    // NRD2 training (Tools > Train Neural Denoiser, --nrd2-train): its GPU hook goes on or off here, before beginFrame.
    if (!nrd2MenuWired_ && nrd2Session_.available()) {
        tools_.setNeuralDenoiserOpener([this] { nrd2Session_.openWindow(); });
        nrd2MenuWired_ = true;
    }
    nrd2Session_.tick(e.device());
    shaderWarmup_.poll();
    // --nrd2-capture: handed over once the renderer is attached and the level has a name (or after ~10 s);
    // it starts stepping on NRD2 frames and holds the camera the same way.
    if (nrd2CapturePoses_ && voxiAttached_ && !nrd2CaptureStarted_ && (!levelName_.empty() || t.frame > 600)) {
        render::denoise::Nrd2CaptureConfig c;
        c.scene = levelName_.empty() ? std::string("unnamed") : levelName_;
        // Path Tracing feeds NRD2 different noise: its poses form their own scene for stratified training.
        if (voxi::Renderer::get().settings().pathTracing != voxi::Quality::Off) c.scene += "_PT";
        for (char& ch : c.scene)
            if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-')) ch = '_';
        c.dir = (nrd2CaptureDir_.empty() || nrd2CaptureDir_ == "default" || nrd2CaptureDir_ == "-")
              ? userDataDir() + "\\nrd2_dataset\\" + c.scene : nrd2CaptureDir_;
        c.poses = nrd2CapturePoses_;
        c.hold = nrd2CaptureHold_;
        c.heldOutFrom = nrd2CaptureHeldOut_;
        c.oracle = nrd2CaptureGrid_ ? render::denoise::Nrd2OracleMode::Grid : render::denoise::Nrd2OracleMode::Grad;
        if (camWanderAmp_ <= 0.0f)
            AVER_WARN("[NRD2] --nrd2-capture without --cam-wander: every pose is the same view");
        voxiRenderer_.startNrd2Capture(c);
        nrd2CaptureStarted_ = true;
    }
    if (voxiAttached_ && voxiRenderer_.nrd2CaptureHolding()) neuraaHold = true;
#endif
    if (neuraaHold) ++camWanderHeld_;
    if (camWanderAmp_ > 0.0f && !neuraaHold && (camWobbleStopFrame_ <= 0 || t.frame < (u64)camWobbleStopFrame_)) {
        if (!camWanderBased_) {
            camWanderBasePos_ = camPos_; camWanderBaseYaw_ = yaw_; camWanderBasePitch_ = pitch_;
            camWanderBased_ = true;
        }
        const f32 s = static_cast<f32>(t.frame - 1 - camWanderHeld_) * camWanderSpeed_;
        const f32 a = camWanderAmp_;
        yaw_ = camWanderBaseYaw_ + a * 0.4363f *   // +-25 degrees at AMP 1
               (std::sin(0.0131f * s) + 0.6f * std::sin(0.0317f * s + 1.1f) + 0.3f * std::sin(0.0719f * s + 2.3f)) / 1.9f;
        pitch_ = camWanderBasePitch_ + a * 0.1396f *   // +-8 degrees
                 (std::sin(0.0173f * s + 0.5f) + 0.5f * std::sin(0.0437f * s + 1.3f)) / 1.5f;
        camPos_ = camWanderBasePos_ + Vec3{a * 300.0f * std::sin(0.0091f * s + 0.2f),    // +-3 m across
                                           a * 300.0f * std::sin(0.0113f * s + 1.9f),
                                           a * 60.0f * std::sin(0.0213f * s + 0.7f)};    // +-0.6 m up/down (Z up)
    }
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
    // --no-occlusion-cull: reasserted every frame (applyProjectVoxiSettings rewrites it during level load).
    if (occlusionCullForceOff_) occlusionCullEnabled_ = false;
    // --occlusion-waitidle: reasserted every frame so console `set` is live before this frame's testBatch().
    if (occluder_) occluder_->setDebugForceWaitIdle(editor::consoleOcclusionForceWaitIdleSlot());
    // occlusion.showCulled/occlusion.cullUnderSuppression read every frame (live for this frame's onRender walk).
    occlusionShowCulled_ = editor::consoleOcclusionShowCulledSlot();
    occlusionCullUnderSuppression_ = editor::consoleOcclusionCullUnderSuppressionSlot();
#endif
    // Autosave polling (drained unconditionally every frame, safe since pumpEvents runs before onUpdate).
    maybeAutosave(t.dt);
    maybeAutosavePrefs(t.dt);
    // Refresh window title when built string differs from what's shown.
    refreshWindowTitle(e);
#if AVER_MODULE_SCENE
    // Collapse selection to one if anchor moved without touching the set (see multiStale()).
    multiSyncToAnchor();
#endif
    // Project settings auto-save (like preferences).
    maybeAutosaveProject(t.dt);
    // Census at frame 2+ (after applyProject's scripting and spawnClassPlacements).
#if AVER_MODULE_SCENE
    if (sceneCensus_ && !sceneCensusDone_ && t.frame >= 2) {
        sceneCensusDone_ = true;
        // playerStart_ excluded (editor synthesises it but shipped game doesn't).
        AVER_INFO("[Census] {}",
                  world::formatSceneCensus(
                      world::takeSceneCensus(scene::World::instance(), playerStart_)));
    }
#endif
#if AVER_MODULE_SCENE
    // --save-level <out>: writes the OPEN level to another path (not over levelPath_).
    // Waits for every pending open (--open-level, forwarded launch, unsaved-changes prompt) before acting.
    if (!saveLevelTo_.empty() && !saveLevelDone_ && !levelPath_.empty() &&
        openLevelByName_.empty() && pendingOpenPath_.empty() && !pendingOpenPrompt_) {
        saveLevelDone_ = true;
        if (saveLevel(saveLevelTo_))
            AVER_INFO("[Level] --save-level wrote '{}' ({}) to {}",
                      levelName_, levelPath_, saveLevelTo_);
        else
            AVER_ERROR("[Level] --save-level failed for {}", saveLevelTo_);
    }
#endif
    // Forwarded open request (guarded as a whole; queue filled only by setOpenRequestHook).
#if AVER_MODULE_SCENE
    if (window_ && window_->hasPendingOpenRequest()) {
        const std::string path = window_->takePendingOpenRequest();
        if (isLevelFile(path.c_str())) {
            // Same funnel as Content Browser (unsaved-changes check and class-placement spawn).
            if (!requestOpenLevel(path, "opened, forwarded from another launch"))
                AVER_WARN("[Sandbox] forwarded level '{}': {}", path, openLevelError_);
        }
    }
#endif  // AVER_MODULE_SCENE
    // Dropped files from Explorer (drained unconditionally; Window keeps accepting WM_DROPFILES regardless).
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
    // --pt-scene-toggle-on/-off: checked before syncPtSceneView() so the same onUpdate() that flips the flag also acts on it.
    if (ptSceneToggleOnAutoFrames_  > 0 && --ptSceneToggleOnAutoFrames_  == 0) ptSceneViewWantEnabled_ = true;
    if (ptSceneToggleOffAutoFrames_ > 0 && --ptSceneToggleOffAutoFrames_ == 0) ptSceneViewWantEnabled_ = false;
    // --sun-set-at: writes the same setting as Directional Light panel's sliders.
    if (sunSetAtFrames_ > 0 && --sunSetAtFrames_ == 0) {
        sky_.setSunAngles(sunSetElevDeg_, sunSetAzimDeg_);
        AVER_INFO("[Sandbox] --sun-set-at: sun moved to elevation {:.1f} deg, azimuth {:.1f} deg",
                  sunSetElevDeg_, sunSetAzimDeg_);
    }
    // --sun-sweep: turns the sun every frame until turnsLeft reaches 0.
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
    // --gi-history-reset-at: resets GI reservoir and denoiser history.
    if (giHistoryResetAtFrames_ > 0 && --giHistoryResetAtFrames_ == 0) {
        voxi::Renderer::get().requestGiHistoryReset();
        voxi::Renderer::get().requestDenoiserHistoryReset();
        AVER_INFO("[Sandbox] --gi-history-reset-at: GI reservoir and denoiser history reset requested");
    }
#endif
    // Clear render-scale crash cookie after 30 frames (device loss noticed at Present).
    // --shader-source: pick up an HLSL edit without restarting (lazy start).
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
    // --aversr-cycle: On/Off/On to exercise the transition that used to crash.
    if (averSrCycleFrames_ > 0 && --averSrCycleFrames_ == 0) {
        AVER_INFO("[AverSR] --aversr-cycle: Performance -> Off, the transition that used to crash");
        applyAverSrQuality(e.device(), aver::sr::Quality::Performance);
        applyAverSrQuality(e.device(), aver::sr::Quality::Off);
        AVER_INFO("[AverSR] --aversr-cycle: survived the round trip");
    }
#endif
    // The Path Tracing tier drives Voxi's path-traced frame (voxi_pt.hlsli); the standalone reference
    // view below is --pt-scene only. Add/remove render feature (only safe before device_->beginFrame; see syncPtSceneView()).
    syncPtSceneView(e.device());
    // Path tracer's matched-environment legacy switch (contrast-fix F6/F7, root cause R5).
    if (ptSceneView_) ptSceneView_->setLegacyEnvironment(editor::consolePtLegacyEnvSlot());
#if AVER_MODULE_VOXI
    // --pt-quality-ramp: verification-only, raises PT rung on live view every N frames.
    if (ptQualityRampEvery_ > 0 && --ptQualityRampCountdown_ <= 0) {
        ptQualityRampCountdown_ = ptQualityRampEvery_;
        voxi::Settings ramped = voxi::Renderer::get().settings();
        const u32 rung = static_cast<u32>(ramped.pathTracing);
        // Off is never raised INTO (the ramp exercises rung changes on a live view).
        if (ramped.pathTracing != voxi::Quality::Off && rung < static_cast<u32>(voxi::Quality::Epic)) {
            ramped.pathTracing = static_cast<voxi::Quality>(rung + 1);
            voxi::Renderer::get().setSettings(ramped);
            AVER_INFO("[PT] --pt-quality-ramp: Path Tracing raised to quality {}",
                      static_cast<u32>(ramped.pathTracing));
        }
    }
    // --gi-method-cycle: verification-only, steps the GI page's Method combo every N frames
    // (cones -> ReSTIR GI -> ReSTIR PT -> reference PT), exactly as a click would; nothing is saved.
    if (giMethodCycleEvery_ > 0 && --giMethodCycleCountdown_ <= 0) {
        giMethodCycleCountdown_ = giMethodCycleEvery_;
        voxi::Settings m = voxi::Renderer::get().settings();
        const bool ptOn = m.pathTracing != voxi::Quality::Off;
        const int cur  = ptOn ? (m.ptMode == 1u ? 3 : 2) : (m.giMode != 0 ? 1 : 0);
        const int next = (cur + 1) % 4;
        m.giMode = next == 0 ? 0u : 1u;
        if (next < 2) {
            m.pathTracing = voxi::Quality::Off;
        } else {
            m.ptMode = static_cast<u32>(next - 2);
            if (!ptOn) { m.pathTracing = voxi::Quality::High; m.ptBounces = voxi::ladder::ptBounces(m.pathTracing); }
        }
        voxi::Renderer::get().setSettings(m);
        static const char* kNames[] = {"Voxel cones", "ReSTIR GI", "ReSTIR path tracing", "reference path tracing"};
        AVER_INFO("[Sandbox] --gi-method-cycle: Method -> {}", kNames[next]);
    }
    // Rung reconciled on same cadence as registration (idempotent setQuality call every frame).
    if (ptSceneView_) {
        const voxi::Quality q = voxi::Renderer::get().settings().pathTracing;
        if (q != voxi::Quality::Off) ptSceneView_->setQuality(static_cast<u32>(q) - 1);
    }
#endif
#if AVER_MODULE_MCP
    // One event per frame (click needs press and release).
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
        // Must stay outside the ImGui frame (font atlas cannot be touched inside one).
        const f32 dpi = e.window() ? e.window()->dpiScale() : dpi_;
        if (std::fabs(dpi - dpi_) > 0.01f) {
            AVER_INFO("[Sandbox] DPI changed {:.2f} -> {:.2f}, re-rasterising the UI font", dpi_, dpi);
            applyDpi(dpi);
        }
    }
    updatePlayWindow(e);
    // Engine's default pawn is an exception: it must not stand down (leaving it out broke PIE feel).
    // One arbitration computed once instead of eleven spellings.
#if AVER_WITH_IMGUI
    // Headless run never calls uiInit(), so CreateContext() never ran and GImGui is null (GetIO() would fault).
    if (e.device()->uiActive()) {
        const ImGuiIO& oio = ImGui::GetIO();
        editor::InputConditions ic;
        ic.uiActive          = e.device()->uiActive();
        ic.browserActive     = browserActive_;
        ic.textInput         = oio.WantTextInput;
        ic.uiWantsKeyboard   = oio.WantCaptureKeyboard;
        ic.uiWantsMouse      = oio.WantCaptureMouse;
        // FALSE WHILE EJECTED: ejecting hands viewport tools back like leaving Play; toolsLive's defaultPawnPlay exception exists to keep spectator pawn flyable pre-eject.
        ic.playing           = playSessionActive() && !playEjected();
        ic.releasedByUser    = releasedByUser_;
        // Left at default without the scene (defaultPawnPlay_ is `#if AVER_MODULE_SCENE`).
#if AVER_MODULE_SCENE
        ic.defaultPawnPlay   = defaultPawnPlay_ && !playEjected();
#endif
        ic.mouseCaptured     = mouse_.captured();
        ic.pointerInViewport = levelHovered_ && inViewport(oio.MousePos.x, oio.MousePos.y);
        ic.drawerOpen        = drawer_ != Drawer::None;
        ic.landscapeMode     = mode_ == EditorMode::Landscape;
        // ImGui sees only the editor window: the play window's keys and mouse are the game's.
        if (playWindowFocused()) {
            ic.textInput = ic.uiWantsKeyboard = ic.uiWantsMouse = false;
            ic.pointerInViewport = true;
        }
        // --wheel-speed-test overrides stand in for right-drag state (headless has no real cursor).
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
    // Leaving fly mode is unconditional (flying_ used to clear only in narrower gate, so focused text fields left camera stuck flying).
#if AVER_WITH_IMGUI
    // uiActive() first (headless has no ImGui context). Nothing lost skipping it there.
    // The drone's right-drag look reads InputState too (captured mouse, play window), so both must release.
    if (e.device()->uiActive() && !ImGui::GetIO().MouseDown[1] && !input_.mouseHeld(1)) flying_ = false;
#endif
    if (own_.keyboardToTool || own_.mouseToTool) {
        const ImGuiIO& io = ImGui::GetIO();
        const bool inPlayWindow = playWindowFocused();
        const bool overUI = !inPlayWindow && (!levelHovered_ || !inViewport(io.MousePos.x, io.MousePos.y));
        // The play window's keys arrive through input_ only (ImGui sees the editor window).
        auto flyKey = [&](ImGuiKey k, i32 vk) { return ImGui::IsKeyDown(k) || (inPlayWindow && input_.keyHeld(vk)); };
        if (inputProbe_ && (ImGui::GetFrameCount() % 30) == 0)
            AVER_INFO("[input-probe] mouse=({},{}) wantCaptureMouse={} hovered={} inViewport={} "
                      "focused={} -> overUI={} | flying={} cam=({:.0f},{:.0f},{:.0f}) yaw={:.2f}",
                      (int)io.MousePos.x, (int)io.MousePos.y, (int)io.WantCaptureMouse,
                      (int)levelHovered_, (int)inViewport(io.MousePos.x, io.MousePos.y),
                      (int)levelFocused_, (int)overUI, (int)flying_,
                      camPos_.x, camPos_.y, camPos_.z, yaw_);

        // Right mouse enters fly mode; leaving is handled unconditionally above (outside this gate).
        if (ImGui::IsMouseClicked(1) && !overUI) flying_ = true;
        // The drone pawn looks while the right button is held, read from InputState: ImGui sees
        // neither a captured mouse's clicks over the viewport reliably nor the play window at all.
#if AVER_MODULE_SCENE
        if (defaultPawnPlay_ && !playEjected() && input_.mouseHeld(1) &&
            (!overUI || inPlayWindow || mouse_.captured()))
            flying_ = true;
#endif
        // --wheel-speed-test drives this directly (forces state rather than synthesising right-drag).
#if AVER_MODULE_FRAMEWORK
        if (wheelTestForceFly_) flying_ = true;
#endif
        if (flying_) ImGui::SetMouseCursor(ImGuiMouseCursor_None);

        // Wheel changes fly speed, up = faster (Unreal's direction), 1.25x per notch so a notch feels
        // the same at any speed. The flying drone pawn takes it without RMB too: in Play the mouse
        // is the game's, and the drone moves at this same speed.
        auto wheelFlySpeed = [&] {
            const f32 wheel = input_.wheel();
            if (wheel == 0.0f) return;
            flySpeed_ *= std::pow(1.25f, wheel);
            flySpeed_ = flySpeed_ < 20.0f ? 20.0f : (flySpeed_ > 40000.0f ? 40000.0f : flySpeed_);
        };
#if AVER_MODULE_SCENE
        if (!flying_ && defaultPawnPlay_ && !playEjected() && !walkCapsule_ && (!overUI || inPlayWindow))
            wheelFlySpeed();
#endif

        if (flying_) {
            // input_.mouseDX/DY not io.MouseDelta: same phase bug as wheel below.
            // ImGui computes io.MouseDelta in NewFrame, but Engine::frameStep runs onUpdate before uiNewFrame.
            // InputState holds THIS frame's motion (fed by pumpEvents before frameStep).
            // CAPTURED (Play): the cursor is re-centred every frame, and that warp arrives as a
            // WM_MOUSEMOVE straight back, so input_'s delta nets to zero -- the view turned, then
            // snapped back. MouseCapture measures from its anchor instead (last frame's poll).
            const bool cap = mouse_.captured();
            const f32 mdx = cap ? mouse_.dx() : static_cast<f32>(input_.mouseDX());
            const f32 mdy = cap ? mouse_.dy() : static_cast<f32>(input_.mouseDY());
            yaw_   += mdx * lookSpeed_;
            pitch_ -= mdy * lookSpeed_;
            pitch_ = pitch_ < -1.54f ? -1.54f : (pitch_ > 1.54f ? 1.54f : pitch_);
            wheelFlySpeed();
        }

        const Vec3 fwd = camForward();
        const Vec3 up{0, 0, 1};
        const Vec3 right = cross(up, fwd).getSafeNormal();

        // Spectator Play flies without holding right button (mouse look still wants it).
        // Synthetic look injection point matches real look (--pie-camera-test).
#if AVER_MODULE_FRAMEWORK
        if (pieCamPendingLook_) {
            pieCamPendingLook_ = false;
            yaw_   += 0.5f;
            pitch_ += 0.3f;
            pieCamWantYaw_ = yaw_; pieCamWantPitch_ = pitch_;
        }
#endif
        // defaultPawnPlay_ is scene-guarded. Without the module, camera flies on right button alone.
        // EJECTED DROPS THE EXCEPTION: spectator pawn stops flying without RMB, block below moves camPos_ instead.
        bool flyPawnDriven = false;
#if AVER_MODULE_SCENE
        if ((flying_ || (defaultPawnPlay_ && !playEjected())) && (!io.WantCaptureKeyboard || inPlayWindow)) {
#else
        if (flying_ && (!io.WantCaptureKeyboard || inPlayWindow)) {
#endif
            const f32 sp = flySpeed_ * t.dt;
            // The possessed default pawn also takes the game's own input (AVER_FW_KEY_A.. is A-Z in
            // order), like the walking one, so --play-test and a game's input mapping can fly it.
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
            const bool pawnInput = defaultPawnPlay_ && !playEjected();
            auto moveKey = [&](ImGuiKey k, i32 vk) {
                return flyKey(k, vk) || (pawnInput && aver_fw_input_key(vk - 'A') != 0);
            };
#else
            auto moveKey = flyKey;
#endif
            Vec3 step{0, 0, 0};
            if (moveKey(ImGuiKey_W, 'W')) step += fwd * sp;
            if (moveKey(ImGuiKey_S, 'S')) step -= fwd * sp;
            if (moveKey(ImGuiKey_D, 'D')) step += right * sp;
            if (moveKey(ImGuiKey_A, 'A')) step -= right * sp;
            if (moveKey(ImGuiKey_E, 'E')) step += up * sp;
            if (moveKey(ImGuiKey_Q, 'Q')) step -= up * sp;
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
            // Move the PAWN (not camera) while default pawn is possessed and not ejected (drivePlayCamera rewrites view every frame).
            if (defaultPawnPlay_ && !playEjected() && walkCapsule_) {
                driveDefaultPawnWalk(fwd, right);    // the walking default pawn (Play options > Walk)
            } else if (defaultPawnPlay_ && !playEjected() && flyCapsule_) {
                driveDefaultPawnFly(t.dt > 0.0f ? step * (1.0f / t.dt) : Vec3{0, 0, 0}, t.dt);   // collides, drifts
                flyPawnDriven = true;
            } else if (defaultPawnPlay_ && !playEjected()) {
                const int32_t pn = aver_fw_controlled_pawn(aver_fw_player_controller(0));
                if (pn) {
                    const scene::Entity pe = static_cast<scene::Entity>(static_cast<uint32_t>(pn));
                    scene::World& pw = scene::World::instance();
                    if (pw.valid(pe)) {
                        const Transform& cur = pw.localTransform(pe);
                        pw.setLocalPosition(pe, cur.position + step);
                        // Yaw about world up, pitch about pawn's right (same as viewport camera).
                        const Quat qy = Quat::fromAxisAngle(Vec3{0, 0, 1}, yaw_);
                        const Quat qp = Quat::fromAxisAngle(Vec3{0, 1, 0}, -pitch_);
                        pw.setLocalRotation(pe, qy * qp);
                    }
                }
            } else
#endif
            camPos_ += step;
        } else if (!overUI) {
            // Scroll-to-dolly (uses input_.wheel() not io.MouseWheel, which is always zero here).
            const f32 wheel = input_.wheel();
            if (wheel != 0.0f) camPos_ += fwd * wheel * (flySpeed_ * 0.15f);
            // MMB pan (uses input_.mouseDX/DY, not frame-old delta).
            if (io.MouseDown[2]) {
                camPos_ -= right * static_cast<f32>(input_.mouseDX()) * 0.02f;
                camPos_ += up    * static_cast<f32>(input_.mouseDY()) * 0.02f;
            }
        }
        // Nothing drove the flying pawn: it glides to rest (the keyboard is the UI's for a moment), or
        // stops dead while ejected, where it must stay where Pawn to Camera puts it.
        if (flyCapsule_ && !flyPawnDriven) driveDefaultPawnFly(Vec3{0, 0, 0}, playEjected() ? 0.0f : t.dt);
        // Ctrl+S saves the level (the File menu's label for this shortcut wires nothing).
        // Not gated on levelFocused_ (saving isn't a viewport gesture) but gated on WantTextInput.
        if (!io.WantTextInput && keybinds_.pressed(editor::CommandId::LevelSave, io))
            saveLevelInteractive();

        // Ctrl+Shift+S saves everything (level plus every dirty asset tab). Same site as LevelSave.
        if (!io.WantTextInput && keybinds_.pressed(editor::CommandId::SaveAll, io))
            saveAll();

        // Ctrl+Shift+B / Ctrl+Shift+R mirror the Tools menu's Compile/Reload Scripts items. Same site.
        if (!io.WantTextInput && keybinds_.pressed(editor::CommandId::CompileScripts, io))
            tools_.requestCompileScripts(project_);
        if (!io.WantTextInput && keybinds_.pressed(editor::CommandId::ReloadScripts, io))
            tools_.requestReloadScripts(project_);

        // F frames the selection at a distance derived from its radius.
        // Outliner/Details count too (levelFocused_ is false when those panels have focus).
        // Scene-guarded: F frames a SELECTED ENTITY's bounds (selectionBounds is `#if AVER_MODULE_SCENE`).
        // STANDS DOWN FOR PAWN TO CAMERA: Shift+F in ejected session both teleports pawn and frames the camera.
#if AVER_MODULE_SCENE
        if ((levelFocused_ || outlinerFocused_ || detailsFocused_) && !io.WantCaptureKeyboard &&
            keybinds_.pressed(editor::CommandId::ViewFrameSelected, io) && anySelected() &&
            !(playEjected() && hasPossessedPawn() &&
              keybinds_.held(editor::CommandId::PlayPawnToCamera, io))) {
            // selectionBounds: union of whole selection (not just anchor, which put multi-selection outside view).
            Vec3 center; f32 r;
            if (selectionBounds(center, r)) {
                const f32 d = std::fmax(50.0f, r / std::tan(radians(30.0f)) * 1.6f);
                camPos_ = center - fwd * d;
                flySpeed_ = std::fmax(flySpeed_, r * 0.4f);
                // Teleport (see frameCameraOnLevel for why streaming_.resetVelocityTracking() is needed).
                streaming_.resetVelocityTracking();
            }
        }
#endif  // AVER_MODULE_SCENE
    }
#endif
#if AVER_MODULE_SCRIPTING
    scripts_.update(t.dt);
#endif
#if AVER_MODULE_FRAMEWORK
    // --recapture-test opts in (gesture lives entirely inside this block).
    const bool interactive = (maxFrames_ == 0 && !playTest_) || recapFrames_ > 0;
    // The #if isn't redundant with uiActive(): runtime flag doesn't make ImGuiIO names COMPILE on UI-less build.
#if AVER_WITH_IMGUI
    if (e.device()->uiActive() && interactive) {
        // Clicking viewport puts mouse back in game (tested before wantCapture below).
        {
            // levelHovered_ asks if pointer is over the LEVEL window (true precondition for capture).
            const ImGuiIO& mio = ImGui::GetIO();
            // Not while ejected (that click is a viewport selection, mouse stays free).
            if (playSessionActive() && !playEjected() && releasedByUser_ && ImGui::IsMouseClicked(0) &&
                levelHovered_ && inViewport(mio.MousePos.x, mio.MousePos.y)) {
                releasedByUser_ = false;
                // Click means "give the mouse back" not a trigger pull. Eaten until button-up (see pushInput).
                eatRecaptureClick_ = true;
            }
        }
        // The play window's chords come through input_ (Win32 virtual keys): Esc stops, Shift+F1
        // releases the mouse, a click takes it back.
        if (playWindowFocused()) {
            if (input_.keyPressed(0x1B /*VK_ESCAPE*/)) { stopPlay(); }
            else if (playSessionActive() && !playEjected()) {
                if (input_.keyHeld(0x10 /*VK_SHIFT*/) && input_.keyPressed(0x70 /*VK_F1*/))
                    releasedByUser_ = !releasedByUser_;
                else if (releasedByUser_ && input_.mousePressed(0)) {
                    releasedByUser_ = false;
                    eatRecaptureClick_ = true;
                }
            }
        }
        // A game-UI menu that wants the cursor frees it, exactly like releasing the mouse by hand.
        const bool wantCapture = playSessionActive() && !releasedByUser_ && !playEjected() && !game::uiWantsCursor();
        // Alt+P/Alt+S start Play (same anyPlayActive() precondition the toolbar Play button uses).
        if (!ImGui::GetIO().WantTextInput && !anyPlayActive() &&
            keybinds_.pressed(editor::CommandId::PlayStart, ImGui::GetIO()))
            launchPlay(e, PlayMode::SelectedViewport);
        if (!ImGui::GetIO().WantTextInput && !anyPlayActive() &&
            keybinds_.pressed(editor::CommandId::PlaySimulate, ImGui::GetIO()))
            launchPlay(e, PlayMode::Simulate);
        if (keybinds_.pressed(editor::CommandId::PlayReleaseMouse, ImGui::GetIO()) && playSessionActive() && !playEjected())
            releasedByUser_ = !releasedByUser_;
        // F8: possess while ejected, eject while possessed (one chord, gated on real session).
        if (keybinds_.pressed(editor::CommandId::PlayEject, ImGui::GetIO()) && playSessionActive())
            togglePlayEject();
        // Shift+F (ejected only): pawn comes to editor camera, then F8 possesses it there.
        // GATED LIKE FRAME SELECTED (level viewport, Outliner, Details), not asset tabs or with WantTextInput.
        {
            const ImGuiIO& pio = ImGui::GetIO();
            if ((levelFocused_ || levelHovered_ || outlinerFocused_ || detailsFocused_) &&
                !pio.WantTextInput && !pio.WantCaptureKeyboard && playEjected() &&
                keybinds_.pressed(editor::CommandId::PlayPawnToCamera, pio) &&
                // Only warn about missing pawn if nothing else answered (Frame Selected shares this press).
                (hasPossessedPawn() || !anySelected()))
                teleportPawnToCamera();
        }
        if (keybinds_.pressed(editor::CommandId::PlayPause, ImGui::GetIO()) && playSessionActive())
            aver_fw_set_paused(aver_fw_play_state() != AVER_FW_PLAY_PAUSED ? 1 : 0);
        if (keybinds_.pressed(editor::CommandId::PlayFrameSkip, ImGui::GetIO()))
            requestPlayFrameStep();
        // Escape stops Play-in-Editor (same as Stop). Checked here, not pushInput, so this wins over game seeing the keypress.
        // Escape ends a drone stand-in too (Play started it, Play's exit ends it).
        if (keybinds_.pressed(editor::CommandId::PlayStop, ImGui::GetIO()) && (playSessionActive() || dronePlayActive() || spectatorPlayActive()))
            stopPlay();
        // F9 screenshots viewport (edit or Play). Checked here, not beside LevelSave: covers both modes with one check.
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
    // UI frame opens before gameplay ticks (ticking is when a game draws its HUD).
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
    // Font and pointer lent per frame, right after frame opens. Font re-lent every frame (loadGameUiFont can leave it invalid).
    aver_ui_set_font(&uiFont_);
    // Pointer converted here (only place that can do it correctly): UI frame is laid out against viewport, not window.
    {
        const f32 ox = hudPreviewActive() ? hudRectX_ : static_cast<f32>(vpX_);
        const f32 oy = hudPreviewActive() ? hudRectY_ : static_cast<f32>(vpY_);
        f32 px = 0.0f, py = 0.0f;
        u32 buttons = 0;
#if AVER_WITH_IMGUI
        // No ImGui context when headless (--headless): GetIO() would read through null.
        if (ImGui::GetCurrentContext()) {
            const ImGuiIO& uiIo = ImGui::GetIO();
            px = uiIo.MousePos.x; py = uiIo.MousePos.y;
            // ImGui reports outside cursor as -FLT_MAX; pushed further negative to hit nothing.
            if (px < -1.0e6f || py < -1.0e6f) { px = -1.0e6f; py = -1.0e6f; }
            else { px -= ox; py -= oy; }
            for (int b = 0; b < 3; ++b)
                if (ImGui::IsMouseDown(static_cast<ImGuiMouseButton>(b))) buttons |= (1u << b);
        } else {
            px = py = -1.0e6f;
        }
        // In the play window the HUD is laid out at its origin, and only input_ sees its pointer.
        if (playWindowFocused() && !hudPreviewActive()) {
            px = static_cast<f32>(input_.mouseX());
            py = static_cast<f32>(input_.mouseY());
            buttons = 0;
            for (int b = 0; b < 3; ++b)
                if (input_.mouseHeld(b)) buttons |= (1u << b);
        }
#else
        (void)ox; (void)oy;
#endif
        aver_ui_set_pointer(px, py, buttons);
    }
#if AVER_MODULE_SCRIPTING
    if (hudPreviewActive()) scripts_.hudDraw(hudPreviewIndex_, t.dt);
#endif
    // Retained game-UI widgets: Play feeds them this frame's keyboard/pad/wheel; the tree itself lays out,
    // updates and draws every frame (it is empty unless a script or graph made a widget).
    if (playSessionActive()) game::uiFeedInput(input_, t.dt, !mouse_.captured());
    aver_ui_widgets_frame(t.dt);
    maybeSpawnTestActor();
    maybePlayTest();
    maybePieCameraTest();
    maybeInputStuckTest();
    maybeInputSourceTest();
    maybeWheelSpeedTest();
    maybeRecaptureTest();
    maybeViewmodelTest();
    // Frame Skip: lift pause for exactly this frame's tick groups. frameStepGuard's destructor puts it back.
    if (playFrameStepPending_ && aver_fw_play_state() == AVER_FW_PLAY_PAUSED) {
        playFrameStepPending_ = false;
        aver_fw_set_paused(0);
        frameStepGuard.active = true;
    }
    // Tick groups bracket physics step: PrePhysics -> Physics -> PostPhysics.
    if (!spawnTestClass_.empty() || aver_fw_play_state() == AVER_FW_PLAY_PLAYING) {
#if AVER_MODULE_SCENE && AVER_MODULE_PHYSICS
        // Level's cars: each driver sets input before physics step, each entity written after.
        // Inside this gate so Pause and Frame Skip hold traffic with everything else.
        // Focus is the view (pawn's eye while possessed, free camera while ejected): cars far from it think less often.
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
    // After physics step, before any prePass (not gated on Play).
    water_.update(*e.device(), t.dt);
#endif
#if AVER_MODULE_SCENE
    // Retires deferred destroys and propagates world matrices once, after gameplay and before onRender.
    // Animation clock runs UNCONDITIONALLY (not gated on PLAYING; hanging it there would freeze previews outside Play mode).
#if AVER_MODULE_FRAMEWORK
    // Object animation follows Play's pause: held while PAUSED, lifted by Frame Skip.
    // Set every frame from play state so Stop or level change never leaves it stuck.
    anim::animSystem().setObjectAnimationPaused(aver_fw_play_state() == AVER_FW_PLAY_PAUSED);
#endif
    playProf_.begin(editor::PlayPhase::ObjectAnim);
    game::tickAnimGraphs(t.dt);   // state machines write the pose the clip sampler would
    anim::animSystem().tick(scene::World::instance(), t.dt);
    playProf_.end(editor::PlayPhase::ObjectAnim);
    // The level sequence (Animate preview, Play), after the clips so a sequenced actor wins.
    tickSequence(t.dt);
#if AVER_MODULE_PHYSICS
    // After the tick that moved them: animated placement's kinematic body follows it (carries standing characters).
    playProf_.begin(editor::PlayPhase::DriveBodies);
    driveAnimatedBodies(t.dt);
    playProf_.end(editor::PlayPhase::DriveBodies);
#endif
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // Unconditionally (like animation clock): previews outside Play should still show effects.
    {
        const auto tickStart = std::chrono::steady_clock::now();
        particles::particleSystem().tick(scene::World::instance(), t.dt);
        particleTickAccumSec_ += std::chrono::duration<f64>(std::chrono::steady_clock::now() - tickStart).count();
        ++particleTickFrames_;
    }
#endif
    // After the tick, before anything draws: update() creates per-entity skin targets and copies matrices out of AnimSystem.
    playProf_.begin(editor::PlayPhase::Skinned);
    if (skinnedScene_)
        skinnedScene_->update(scene::World::instance(), anim::animSystem(), *e.device());
    playProf_.end(editor::PlayPhase::Skinned);
#if AVER_MODULE_RENDER_SOFTBODY
    // After physics and World::flush, before draw (drawHandle() must exist).
    if (softBodyScene_) softBodyScene_->update(scene::World::instance(), *e.device());
#endif
    // Once per frame, before render features run (ThumbnailCache.hpp): update() points preview at next pending request.
    if (thumbnails_.ready()) thumbnails_.update();
    // --drone: switches drone on N frames in (mirroring --chunk-stream so a capture can prove it without clicking Window > Drone).
    if (droneAutoFrames_ > 0 && --droneAutoFrames_ == 0) setDroneEnabled(true);
    // --undo-test: fires runUndoTest() N frames in. Lives inside `#if AVER_WITH_IMGUI` block (same as commands it proves).
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
    // Armed once: markLevelUnsaved gives autosave timer something to do; re-marking would loop forever.
    if (autosaveTestArm_) { autosaveTestArm_ = false; markLevelUnsaved(); }
    if (keybindTestAutoFrames_ > 0 && --keybindTestAutoFrames_ == 0) runKeybindPersistTest(keybindTestMode_);
#endif
#if AVER_MODULE_SCRIPTING
    // Ticks the graph-driven drone if live. Runs before chunk streaming so it sees this frame's drone position/velocity.
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
            // Greppable proof the drone MOVED across several frames (not just spawned).
            if (droneLogsLeft_ > 0) {
                --droneLogsLeft_;
                AVER_INFO("[Drone] t={:.3f}s pos=({:.1f},{:.1f},{:.1f}) vel=({:.1f},{:.1f},{:.1f})cm/s",
                          droneTimeSeconds_, dronePos_.x, dronePos_.y, dronePos_.z,
                          droneVel_.x, droneVel_.y, droneVel_.z);
            }
        }
    }
    // Graph-as-class instances gated on Play. HostBridge registers graph classes into native class registry.
    // Ungated: every class-placed graph ran OnTick while browsing, mutating level. Need FRAMEWORK guard for aver_fw_play_state().
#if AVER_MODULE_FRAMEWORK
    playProf_.begin(editor::PlayPhase::GraphTicks);
    if (!spawnTestClass_.empty() || aver_fw_play_state() == AVER_FW_PLAY_PLAYING)
        scripts_.tickGraphClassInstances(t.dt);
    playProf_.end(editor::PlayPhase::GraphTicks);
#endif
#endif
    // --chunk-stream: switches streaming on N frames in so a capture can prove it happened.
    if (chunkStreamAutoFrames_ > 0 && --chunkStreamAutoFrames_ == 0) setChunkStreamingEnabled(true);
    // Chunk streaming. Runs here so it sees THIS frame's camPos_ and evictions land in flush() below.
    // Runs while idling outside Play too (exactly who this feature is for).
    if (streaming_.enabled()) {
        const game::GameStreaming::TriangleLookupFn tris = [this](u64 id) -> u32 {
            const auto it = meshTris_.find(id);
            return it != meshTris_.end() ? it->second : 0u;
        };
#if AVER_MODULE_SCRIPTING
        // Drone keeps its own corridor resident while flying (both are StreamSource entries).
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
    // Reclaims finished voices every frame (Play or not). Gap: aver_audio_init had single caller, so voices leaked until mixer ran out.
    // Not gated on Play: voices outlive a session, so gating would leak voices that finish after Stop.
    aver_audio_collect();
#endif
#if AVER_MODULE_SYNAPSE_SCENE && AVER_MODULE_FRAMEWORK
    // Gated on Play (like physics/fw_tick above): pathing is gameplay, not authoring preview.
    // nav_ may be empty or frame stale; AgentSystem::tick treats that as "wait for a grid".
    if (!spawnTestClass_.empty() || aver_fw_play_state() == AVER_FW_PLAY_PLAYING) {
        game::tickAi(t.dt, &nav_);
    }
#endif
#endif
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
    // Ejected: leave camPos_/yaw_/pitch_ where the user flew them (drivePlayCamera snaps view to pawn every frame) and clear owner-hide root.
    // Re-possessing calls drivePlayCamera() again next frame.
    if (playEjected()) firstPersonPawn_ = scene::kInvalidEntity;
    else                drivePlayCamera();
#endif
    // A sequence camera (Animate's pilot view, or Play's camera track) replaces the view for this
    // frame only: camPos_/yaw_/pitch_ are the persisted editor camera and stay untouched.
    viewOverride_ = false;
#if AVER_MODULE_SCENE
    {
        game::SeqCameraPose sp;
        if (seqEditor_.viewPose(playEjected(), sp)) {
            viewOverride_ = true;
            viewPosOv_ = sp.position;
            viewFwdOv_ = game::cameraForward(sp.yaw, sp.pitch);
            // The pawn is in view now, not behind the eye.
            if (seqEditor_.playRunning()) firstPersonPawn_ = scene::kInvalidEntity;
        }
    }
#endif
    // G-buffer: pushed every frame so live dropdown click or --gbuffer-debug takes effect immediately.
    // OR'd together: --gbuffer alone must write with no view selected.
    // Voxi's denoiser reads these targets every frame it runs.
    // Frame interpolation reads motion and depth from G-buffer.
#if AVER_MODULE_VOXI
    voxi::Renderer& vx = voxi::Renderer::get();
    const voxi::Resolution gbufRes = voxi::resolve(vx.settings(), vx.deviceInfo());
    const bool wantGbufForDenoiser = gbufRes.denoiserGBufferWanted;
    // NeuRAA runs only on ray-found first surfaces (it needs the visibility buffer); raster frames use MSAA.
    [[maybe_unused]] const bool rayPrimaryFrame = gbufRes.rtRenderMode.effective == 1u || vx.settings().pathTracing != voxi::Quality::Off;
#else
    const bool wantGbufForDenoiser = false;
    [[maybe_unused]] const bool rayPrimaryFrame = true;
#endif
    const bool wantGbufForFrameInterp = updateFrameInterpolation(e.device());
#if AVER_MODULE_SR
    // TAA's resolve reads velocity and view Z; NeuRAA reads view Z and normals.
    const bool wantGbufForTaa = temporalAaEnabled_ || (neuraaEnabled_ && rayPrimaryFrame);
#else
    const bool wantGbufForTaa = false;
#endif
    e.device()->setGBufferEnabled(gbufferOverride_ || wantGbufForDenoiser || wantGbufForFrameInterp ||
                                  wantGbufForTaa || neuraaDebugView_ ||
                                  gbufferDebugView_ != GBufferDebugFeature::Mode::Off);
    gbufferDebugFeature_.setDevice(e.device());
    // vpX_/vpY_/vpW_/vpH_: this frame's 3D-viewport rect (frame stale at worst on first draw).
    gbufferDebugFeature_.setViewportRect(static_cast<u32>(vpX_), static_cast<u32>(vpY_),
                                         static_cast<u32>(vpW_), static_cast<u32>(vpH_));
    gbufferDebugFeature_.setMode(gbufferDebugView_);
#if AVER_MODULE_VOXI
    e.device()->setMeshShaders(voxi::Renderer::get().settings().meshShaders);
#else
    e.device()->setMeshShaders(msOverride_);
#endif
#if AVER_MODULE_VOXI
    // Voxi owns the AA setting; push it when it changes (rebuilds targets+PSOs). Ray-driven frames run
    // at 1x whatever MSAA says (voxi::Resolution::sampleCount).
    {
        const u32 samples = voxi::resolve(vx.settings(), vx.deviceInfo()).sampleCount;
        if (vx.consumeMsaaDirty() || samples != msaaPushed_) {
            e.device()->setSampleCount(samples);
            msaaPushed_ = samples;
        }
    }

    if (voxiAttached_) {
        // Copy, load-bearing: must never write back to the singleton.
        voxi::Settings vs = voxi::Renderer::get().settings();
        frameBudgetTick(t.dt, vs);
        const f32 c[3] = {giCenter_.x, giCenter_.y, giCenter_.z};
        // Auto-switch: ray-hit/triangles debug views can't draw through the rasteriser; Wireframe/G-buffer can't draw through ray-driven.
        const bool needRaster = wireframe_ || gbufferDebugView_ != GBufferDebugFeature::Mode::Off;
        const bool needRayDriven = !needRaster &&
            (debugView_ != voxi::VoxiRenderer::ViewDebug::None || neuraaDebugView_) &&
            voxiRenderer_.rayDrivenAvailable();
        // Path Tracing forces ray-driven primary (VoxiRenderer::setSettings), so a raster view mode or
        // the --pt-scene reference view turns it off for the frame.
        if (needRaster || ptTakesViewport()) { vs.rtRenderMode = 0; vs.pathTracing = voxi::Quality::Off; }
        else if (needRayDriven) vs.rtRenderMode = 1;
        // Reset history when renderer mode changes (compared against last frame's effective mode).
        if (static_cast<i32>(vs.rtRenderMode) != lastEffectiveRtRenderMode_) {
            voxiRenderer_.resetRtHistory(true);
            voxiRenderer_.resetAoHistory();
            voxiRenderer_.resetGiHistory(true);
            voxiRenderer_.resetDenoiserHistory(true);
            lastEffectiveRtRenderMode_ = static_cast<i32>(vs.rtRenderMode);
        }
        // Undenoised mode: toggles denoiser, ray-tile amortisation, shadow denoise, GI spatial reuse.
        if (undenoised_) {
            vs.denoiser = false;
            vs.rtPixelsPerRayTile = 1;
            vs.rtShadowDenoise = 0;
            vs.giRestirSpatialSamples = 0;
        }
        // A heavy mode switch is announced a frame before it is applied (ModeSwitchNotice.hpp).
        static editor::ModeSwitchNotice modeNotice;
        if (modeNotice.shouldApply(vs, e.window() != nullptr && maxFrames_ == 0))
            voxiRenderer_.setSettings(vs);
        // Consume-and-forward: console reset commands routed through singleton flags to the renderer instance.
        if (voxi::Renderer::get().consumeGiHistoryResetRequest())  voxiRenderer_.resetGiHistory();
        if (voxi::Renderer::get().consumeRtHistoryResetRequest())  voxiRenderer_.resetRtHistory();
        if (voxi::Renderer::get().consumeAoHistoryResetRequest())  voxiRenderer_.resetAoHistory();
        if (voxi::Renderer::get().consumeDenoiserHistoryResetRequest()) voxiRenderer_.resetDenoiserHistory();
        // Reset GI/RT/denoiser history every frame if console flag set or Undenoised active.
        const u32 everyFrame = editor::consoleResetHistoryEveryFrameSlot();
        if (everyFrame || undenoised_) {
            if ((everyFrame & 1u) || undenoised_) voxiRenderer_.resetGiHistory(/*quiet=*/true);
            if ((everyFrame & 2u) || undenoised_) voxiRenderer_.resetRtHistory(/*quiet=*/true);
            if ((everyFrame & 4u) || undenoised_) voxiRenderer_.resetDenoiserHistory(/*quiet=*/true);
        }
        voxiRenderer_.setVolume(c, giExtent_);
        // Baked volume cache directory; empty when no project open.
        voxiRenderer_.setGiCacheDir(project_.valid() ? fmt::giCacheDir(project_.dir) : std::string());
        voxiRenderer_.setDebugView(giDebugView_);
        // Console knob voxi.giPoisonView reasserted each frame.
        voxiRenderer_.setGiPoisonView(editor::consoleGiPoisonViewSlot());
        // Lighting-contrast legacy bitmask.
        voxiRenderer_.setLightingLegacyBits(editor::consoleLightingLegacySlot());
        // Console knobs for GI optimisations.
        voxiRenderer_.setGiForceRebuild(editor::consoleGiForceRebuildSlot());
        voxiRenderer_.setGiBoundedDispatch(editor::consoleGiBoundedDispatchSlot());
        voxiRenderer_.setGiFreeAccumulator(editor::consoleGiFreeAccumulatorSlot());
        // Console knobs for GI vis and blended cone.
        voxiRenderer_.setGiVisPathView(editor::consoleGiVisPathViewSlot());
        voxiRenderer_.setBlendedGiCone(editor::consoleBlendedGiConeSlot());
        // Neural Visualiser NeuRaC view.
        voxiRenderer_.setNeuRaCView(static_cast<u32>(neuracViewMode_ < 0 ? 0 : neuracViewMode_), neuracViewGrid_);
        // See setGiConeTraceOff's comment; applied every frame.
        voxiRenderer_.setConeTraceEnabled(!giConeTraceOff_);
#if AVER_MODULE_SR
        // AverSR level resolved fresh each frame from precedence chain.
        updateAverSrAuto(e);
        applyUpscalerSlot(e.device());
#endif
        const Vec3 sd = Vec3{sky_.sunDirection[0], sky_.sunDirection[1],
                             sky_.sunDirection[2]}.getSafeNormal();
        if (sunAngle_ > 0.0f) sky_.sunAngularDiameterDeg = sunAngle_;
        // Direction only; color/ambient reach shaders through device frame constants.
        voxiRenderer_.setSunDirection(&sd.x);
    }
#endif
    // Confine the scene to the dockspace's central node (latched by buildUI last frame).
    e.device()->setViewportRect((u32)vpX_, (u32)vpY_, (u32)std::fmax(1.0f, vpW_), (u32)std::fmax(1.0f, vpH_));

    const Vec3 fwd = viewForward();
    const f32 aspect = viewAspect();
    const game::CameraMatrices cam = game::pushCamera(*e.device(), viewPos(), fwd, aspect);
    invVP_ = cam.invVP; viewProj_ = cam.viewProj; eye_ = viewPos();

    // One member for one value; fixes dead code in fog handling.
    f32 fog = fogDensity_;
#if AVER_MODULE_SCENE
    // Opt-in: match fog density to streaming load boundary (foggier, only when asked).
    if (matchFogToStreamRadius_ && streaming_.enabled()) {
        const world::StreamSettings& st = streaming_.settings().stream;
        const f32 boundaryCm = static_cast<f32>(st.loadRadius) * static_cast<f32>(st.chunkSizeCm);
        const f32 matched = fogDensityForOpacityAt(boundaryCm, fogMatchTargetOpacity_);
        if (matched > 0.0f) fog = matched;
    }
#endif
    // sunDirection stays unnormalised; shaders normalise it.
    sky_.enabled = showAtmosphere_;   // Show > Atmosphere; the backend already gates the sky draw on this
    for (int i = 0; i < 3; ++i) {
        sky_.sunColor[i] = sunColor_[i];
        sky_.zenith[i]   = skyZenith_[i];
        sky_.horizon[i]  = skyHorizon_[i];
        sky_.fogColor[i] = fogColor_[i];
    }
    // --sky-light overrides the level, reapplied here since applyLevelSky overwrites sunAmbient_ on level open.
    if (skyLightOverride_ >= 0.0f) sunAmbient_ = skyLightOverride_;
    // --sky-physical/--sky-authored/sun elevation, reapplied here for the same reason.
    if (skyModelOverride_ >= 0)
        sky_.model = skyModelOverride_ ? rhi::SkyModel::Physical : rhi::SkyModel::Authored;
    if (sunElevationOverride_ > -90.0f) {
        f32 elev = 0.0f, azim = 0.0f;
        sky_.sunAngles(elev, azim);
        // Azimuth unchanged; flag names elevation only, silently rotating the sun would make runs differ.
        sky_.setSunAngles(sunElevationOverride_, azim);
    }
    sky_.skyLightIntensity = sunAmbient_;
    sky_.fogDensity = fog;
    sky_.cloudTime = cloudTime_;
    // Underwater fog applied to a copy; sky_ is the authored sky and must stay unchanged.
#if AVER_MODULE_FLUIDS
    e.device()->setSkyAtmosphere(water_.applyUnderwaterFog(sky_, viewPos().z));
#else
    e.device()->setSkyAtmosphere(sky_);
#endif
    // Outside the viewport rect is editor chrome, not sky.
    e.device()->setClearColor(0.055f, 0.055f, 0.062f, 1);
    // Console post.* vars write the device, not post_; adopt device changes into post_ before pushing.
    if (postPushedValid_ && !rhi::postSettingsEqual(e.device()->postProcess(), postPushed_))
        post_ = e.device()->postProcess();
    // Debug views paint flat diagnostic colours; disable auto-exposure/bloom/local exposure.
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
#endif
#if AVER_MODULE_SR
    // Detach from device first, then destroy: raw pointers must be cleared before unique_ptrs reset.
    e.device()->setUpscaler(nullptr);
    averSrUpscaler_.reset();
    taaUpscaler_.reset();
#endif
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
                frameInterpolator_->setWeightsPath(dir + "\\" + neurafi::NeuraFI::kWeightsFileName);
            frameInterpolator_->setShippedWeightsPath(aver::executableDir() + "\\data\\" +
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
