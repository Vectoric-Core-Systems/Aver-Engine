// Lifecycle: construction, the splash and log sink, config, DPI and fonts, onInit, onUpdate, onShutdown.
// Part of SandboxApp, split out of the single 29,952-line SandboxApp.cpp on 2026-09-16 by moving method bodies
// verbatim; the class itself is declared in SandboxApp.hpp.

// The one translation unit that compiles stb_image_write's implementation.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#undef STB_IMAGE_WRITE_IMPLEMENTATION
#include "SandboxApp.hpp"
#include "aver/game/GameCamera.hpp"
#include "aver/game/GameTick.hpp"

namespace aver {
SandboxApp::SandboxApp(u64 maxFrames, bool headless, std::string beamPath, std::string shot, Tool initialTool)
    : maxFrames_(maxFrames), headless_(headless), beamPath_(std::move(beamPath)), shot_(std::move(shot)), initialTool_(initialTool) {
    setLogSink(&SandboxApp::logSink, this);
}

// Appends one engine log line to the Output Log buffer. Must not itself log: the core log mutex is held.
// One log line, trimmed to something a single-line splash can show. The subsystem tag is kept
// -- "[Material] foo.ocmat" says more than "foo.ocmat" -- but the level prefix and any absolute
// path are not: DT_END_ELLIPSIS clips from the RIGHT, so a full path would show the drive letter
// and hide the filename, which is the only part worth reading.
 std::string SandboxApp::splashTextFor(std::string_view msg) {
    std::string t(msg);
    if (!t.empty() && t[0] == '[') {                       // drop the level prefix, keep the tag
        const usize close = t.find(']');
        if (close != std::string::npos) t.erase(0, close + 1);
    }
    while (!t.empty() && t.front() == ' ') t.erase(0, 1);
    // Keep the last THREE path segments of anything that looks like a path -- enough to read as
    // "JungleRuins/Sponza/arch_stones_01.ocmesh" rather than a bare filename, which is what makes
    // it obvious WHICH thing is loading rather than merely that something is. Not the whole path:
    // DT_END_ELLIPSIS clips from the RIGHT, so a full absolute path spends the line on a drive
    // letter and directories and hides the only part worth reading. Both separators, since these
    // lines carry either.
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

    // THE LOADING SCREEN SAYS WHAT IS ACTUALLY LOADING, and this is where it learns it. The
    // splash used to show only the half-dozen stage names Engine::run and applyProject hand it,
    // so it read "Loading level" for the entire tail of a large project -- the log knew which
    // mesh, material and texture were arriving, and the screen in front of the user did not.
    //
    // MAIN THREAD ONLY, and that is not caution, it is required: the splash's GDI objects belong
    // to the thread that called show(), and Jolt wires JPH::Trace straight into AVER_INFO from
    // its job pool, so a worker's log line arriving here would touch them from the wrong thread.
    // A dropped line costs nothing -- the next main-thread line replaces it a moment later.
    //
    // AND IT MUST NOT LOG. This runs under the core log mutex, which is not recursive, so a
    // single AVER_* call from inside here deadlocks against itself -- a trap this file has
    // already been caught by once. Nothing below logs: setLoadingStatus sets a string and pumps
    // a message queue, and the splash has no custom WndProc to log from.
    // EITHER LOADING SCREEN, whichever is up. There are two: the engine's startup splash, which
    // applyProject BORROWS when a project is named on the command line, and the one applyProject
    // owns when a project is opened from the browser frames later. Only forwarding to the first
    // meant a browser-opened project showed its six stage names and none of the assets, which is
    // the case a user actually watches most often.
    if (level >= LogLevel::Info && std::this_thread::get_id() == self->mainThreadId_) {
        const std::string text = splashTextFor(msg);
        if (self->projectLoading_) self->projectLoading_->stage(text.c_str());
        else if (self->engineForSplash_ && self->engineForSplash_->loadingScreenActive())
            self->engineForSplash_->setLoadingStatus(text);
    }

    // GRAPH PRINTS ALSO GO ON SCREEN. Visual scripting had no debugging surface at all: a Print
    // node wrote one line into a firehose of engine logging, so watching a graph run meant
    // reading the Output Log for "[Graph]" among the render, asset and physics lines, and a
    // print fired once during a jump was gone by the time you found it.
    //
    // FILTERED HERE rather than at draw time, and on the prefix GraphInterop writes, so the
    // overlay costs one substring test per log line instead of a scan of the whole buffer every
    // frame. rfind(x, 0) == 0 is "starts with" without allocating.
    //
    // NO TIMESTAMP TAKEN HERE. This runs under the core log mutex, from whichever thread logged,
    // and the contract says be quick and do not log; the frame that draws it stamps it instead
    // (see drawGraphPrintOverlay), which is also the clock the fade needs to agree with.
    if (msg.rfind("[Graph] ", 0) == 0) {
        std::string text(msg.substr(8));
        // CONSECUTIVE DUPLICATES COLLAPSE, and this is not tidiness -- without it the feature is
        // unusable. A PrintString on an OnTick chain fires EVERY FRAME: sixty identical lines a
        // second, a ring buffer that churns faster than anyone can read, and an overlay that is
        // a solid block of the same sentence. Collapsed, that same graph shows one line with a
        // rising count, which is also strictly more information -- "still firing, 143 times now"
        // rather than "firing".
        //
        // CORRECTING THIS COMMENT'S FIRST DRAFT, which said the flood happens "just from having
        // the level open" because the editor ticks graphs ungated on play state. IT DOES NOT.
        // HostBridge.GraphTickBoundInstances is ungated INTERNALLY, and that callee comment is
        // what I read -- but its caller gates it (see the tickGraphClassInstances call in this
        // file, guarded on aver_fw_play_state() == AVER_FW_PLAY_PLAYING, with a long comment
        // recording the measurement that forced that gate: 4003 tick lines and a VAR climbing to
        // 12.31s while nobody pressed Play). Reading a callee's comment instead of tracing its
        // caller is the exact mistake this codebase keeps paying for.
        //
        // The flood is real regardless -- it happens the moment you press Play, and in the
        // packaged game every frame -- which is precisely when someone is watching this feed.
        //
        // CONSECUTIVE only, deliberately, not deduplicated across the whole buffer: two prints
        // alternating (a Branch taking each arm in turn) is exactly the pattern an author is
        // watching for, and merging those into two static rows would hide the alternation that
        // is the whole signal.
        if (!self->graphPrints_.empty() && self->graphPrints_.back().text == text) {
            ++self->graphPrints_.back().count;
            // Back to unstamped, so the next frame re-stamps it to now: a print that is still
            // firing must not fade out underneath its own rising count.
            self->graphPrints_.back().at = -1.0;
        } else {
            self->graphPrints_.push_back({std::move(text), -1.0, 1});
            if (self->graphPrints_.size() > kMaxGraphPrints) self->graphPrints_.pop_front();
        }
    }

    // ERRORS AND CRITICALS BECOME NOTIFICATIONS, from inside THIS sink rather than a second one:
    // setLogSink holds a single global slot (Log.cpp), and this application already owns it.
    //
    // Everything this call is allowed to do is written on pushFromLog itself, because the cost
    // of getting it wrong is a deadlock rather than a failing test -- we are three locks deep
    // here (the core log mutex, then logMutex_ above, then the queue's own), and a single
    // AVER_* call from inside it would hang the editor against a non-recursive mutex.
    editor::notifications().pushFromLog(level, msg);
}

// Returns the boot configuration for the editor window.
BootConfig SandboxApp::config() const  {
    BootConfig c; c.windowTitle="Aver Engine \xE2\x80\x94 Editor"; c.windowWidth=1600; c.windowHeight=900;
    c.maxFrames=maxFrames_; c.headless=headless_; c.useWarp=useWarp_;
    // WINDOWED, and borderless fullscreen only when it is ASKED FOR. This was the other way
    // round -- an interactive run started borderless-fullscreen and --windowed opted out --
    // which is the wrong default for an editor: it covers the taskbar and whatever the user
    // was reading beside it, and an editor is a tool you sit next to other windows, not a
    // game you launch into. --fullscreen asks for it explicitly now.
    //
    // maxFrames_ == 0 IS STILL PART OF THE TEST, and stays here rather than being dropped as
    // redundant: a bounded capture run (--frames) is how every recorded gate baseline was
    // measured, and its probes are pixels at a fixed rect in a fixed client area. Even an
    // explicit --fullscreen must not reshape the window under a measurement.
    c.fullscreen = fullscreenOverride_ && maxFrames_ == 0;
    c.enableDebugLayer=debugLayer_;
    c.backend = backendName_.empty() ? nullptr : backendName_.c_str();
    return c;
}

// Has the project actually reached the screen? See Application::startupComplete.
//
// TWO CASES, and the empty one matters as much as the other. With NO project requested there is
// nothing to wait for beyond the editor's own first frame, so one rendered frame is the whole
// condition -- otherwise an empty editor would sit behind the splash for the full warm-up cap.
//
// With a project, the honest signal is that the scene has DRAWN something: lastSceneDrawn_ is
// set from the per-frame walk and stays -1 until a frame has actually submitted meshes, which is
// downstream of the mesh uploads, the material/texture residency and the first voxelisation --
// exactly the work that used to happen after the splash had gone. A project that legitimately
// draws nothing (an empty level) falls through to the engine's warm-up cap and costs a few
// seconds; that is the right way round, because the alternative is calling a project loaded
// before it is.
// lastSceneDrawn_ IS THE FRAME COUNTER AS WELL AS THE DRAW COUNT: it starts at -1 and the scene
// walk assigns it on the first frame that runs, whatever the count, so >= 0 means "a frame has
// been walked" and > 0 means "a frame has drawn something". One member answers both halves and
// there is no second counter to keep in step with it.
// AND IT HAS TO STOP CHANGING, which is the half that was missing. "A frame drew something" is
// true of the FIRST mesh, not the last: a project streams its meshes in over many frames, so the
// splash lifted partway through with the rest of the level still arriving behind it. Waiting for
// the draw count to hold steady for a few consecutive frames waits for the tail instead of the
// head, and costs nothing on a project that is already settled -- the count is equal to itself
// from the first frame and the run clears in kSettleFrames.
//
// A COUNT, NOT A TIMER. Frames are the unit the thing being waited on actually advances in, and a
// wall-clock wait would be a different length on every machine for no reason. The engine's own
// warm-up cap (600 frames / 20s) still bounds this, so a project that never settles -- streaming
// that genuinely never ends -- costs seconds rather than the session.
//
// THE WHOLE DETECTOR IS SCENE WORK, and it reads three counters that say so in their own names --
// lastSceneDrawn_ and the two settle members beside it, all declared `#if AVER_MODULE_SCENE` and
// written only by the scene walk. This predicate is an Engine override and has to exist in every
// build, so the guard goes around the body rather than around the function.
bool SandboxApp::startupComplete() const  {
#if AVER_MODULE_SCENE
    if (lastSceneDrawn_ < 0) return false;             // no frame has walked the scene yet
    // THE LIVE PROJECT, NOT THE COMMAND LINE. projectPath_ is set once from argv and is empty
    // for every project opened through the browser -- so asking it here answered "nothing was
    // asked to load" for the most common way a project is actually opened, and the loading
    // screen lifted on the first walked frame with the whole load still ahead of it. That is
    // the midway disappearance, moved rather than fixed: the startup path was cured and the
    // browser path still had it. project_ is assigned at the top of applyProject and tracks
    // opens and switches, which is the same reason handleOpenRequest reads it instead.
    // A FAILED OPEN leaves it empty and falls out here after one frame, which is right: there
    // is nothing arriving to wait for, and the alternative is holding a splash over an error
    // until the engine's 600-frame warm-up cap expires.
    if (project_.manifestPath.empty()) return true;    // no project is loading
    if (lastSceneDrawn_ == 0) return false;            // walked, but nothing has drawn yet
    constexpr int kSettleFrames = 8;
    // Mutable because this is asked once per warm-up frame and there is nowhere else to tick
    // from: Engine's loop calls exactly this, and a second per-frame hook to update a counter
    // read only here would be two things to keep in step instead of one.
    if (lastSceneDrawn_ == startupSettleCount_) ++startupSettleFrames_;
    else { startupSettleCount_ = lastSceneDrawn_; startupSettleFrames_ = 0; }
    return startupSettleFrames_ >= kSettleFrames;
#else
    // NOTHING WALKS A SCENE IN THIS TREE, so there is no draw count to hold steady and the settle
    // run above would never begin -- lastSceneDrawn_ would sit at its -1 start and this would
    // answer "not yet" until the engine's 600-frame warm-up cap gave up, holding a loading screen
    // over an editor that had finished loading. What the detector waits for is a level's meshes,
    // materials and textures arriving behind the first drawn frame; with no world to instantiate
    // them into there is no tail to wait for, and the editor is up as soon as the engine says so.
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
    // Pushed rather than pulled: AssetEditor::draw() carries no dpi argument, and widening that
    // interface for one subclass is the wrong trade. ActorEditor's content-root setter set this
    // precedent; GraphEditor follows it.
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
        // The medium weight gets its own copy of the icons: a font is a separate atlas entry,
        // so an icon pushed under fontMedium_ would otherwise be a notdef box.
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
    // STATIC, because ImGui keeps the pointer rather than copying the range: a local array here
    // would dangle the moment this function returned, and the atlas would build from freed
    // stack memory. The terminating 0 is required.
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

// ---- THE GAME UI'S FONT -----------------------------------------------------------------
//
// Aver.UI could not draw a character until now, so this is the first thing that gives it one.
// The .ocfont and its atlas are staged beside the exe exactly like splash.png and the icon
// sheets, and loaded the same way -- decodeImage plus createTexture plus uiTextureId, which is
// the path the Compile C# icons already use.
//
// NON-FATAL, deliberately: a missing font leaves uiFont_ invalid, addText draws nothing, and
// the HUD is a HUD without labels. An editor that refuses to start because a font is missing
// would be a worse trade than one whose demo overlay is quieter.
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
    // THE RAW TextureHandle, NOT uiTextureId(). UiDrawCmd::texture is cast straight back to an
    // rhi::TextureHandle by UiRenderer (`static_cast<rhi::TextureHandle>(c.texture)`), so this
    // is the game UI's own texture channel and not ImGui's. uiTextureId returns a DESCRIPTOR
    // handle for ImTextureID, and handing one to this path produced exactly what you would
    // expect: "[RHI.D3D12] setSrv with an invalid handle", and no text.
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
    // Latched once, because the Project Settings page shows it next to the backend the
    // project ASKS for and has no Engine& in scope to ask again.
    runningBackend_ = rhi::backendName(e.device()->backend());
#if AVER_MODULE_SR
    // AverSR quality sets the same renderScaleOverride_ knob --render-scale drives, so an explicit
    // --render-scale still wins (same 1.0-sentinel precedent loadEditorPreferences() uses). At Off
    // (default) this is a no-op -- bit-identical to a tree without AverSR.
    if (averSrQuality_ != aver::sr::Quality::Off && renderScaleOverride_ == 1.0f)
        renderScaleOverride_ = aver::sr::renderScaleFor(averSrQuality_);
#endif
    // --render-scale F: applied once, here, before anything sizes itself off the device. 1.0 (no
    // flag) is a no-op -- setRenderScale clamps into [0.25,1] but a backend without a swapchain
    // yet just stores it for createSwapchainResources to pick up.
    if (renderScaleOverride_ != 1.0f) {
        e.device()->setRenderScale(renderScaleOverride_);
        AVER_INFO("[Sandbox] render scale {:.2f} (--render-scale)", e.device()->renderScale());
    }
#if AVER_MODULE_SR
    // Constructs SpatialUpscaler -- a real GPU-resource-owning object, not the CLI-only
    // renderScaleOverride_ float above -- whenever a non-Off level was requested. See
    // logAverSrActive() for the honest limit of what that buys today.
    if (averSrQuality_ != aver::sr::Quality::Off) {
        ensureAverSrUpscaler(e.device());
        logAverSrActive(e.device());
    }
    // --edge-aa: see edgeAaEnabled_'s own member comment for why this shares AverSR's upscaler
    // slot and who wins when both are requested. Off (the default) never constructs FxaaResolve
    // and never touches the slot -- bit-identical to a build without this flag.
    if (edgeAaEnabled_) ensureEdgeAaUpscaler(e.device());
#endif
    // --depth-prepass: a same-frame depth-only pass ahead of the opaque colour walk -- see the
    // entity loop's comment ("depth prepass phase") for the two-walk mechanism. Generic on
    // IDevice: a no-op on any backend that never implements depthPrepassPipeline().
    if (depthPrepassOverride_) {
        e.device()->setDepthPrepassEnabled(true);
        AVER_INFO("[Sandbox] depth prepass enabled (--depth-prepass)");
    }

    // GBufferDebugFeature registered unconditionally (mode defaults to Off) so the viewport
    // view-mode dropdown can enable it live without a second registration. Whether the G-buffer is
    // actually written is decided separately, per frame, in onUpdate (gbufferOverride_).
    e.device()->addRenderFeature(&gbufferDebugFeature_);
    gbufferDebugAttached_ = true;

    // Registration order is precedence: the first factory that accepts a path wins.
    assetEditors_.registerFactory(&editor::makeMeshEditor);
    assetEditors_.registerFactory(&editor::makeActorEditor);
    assetEditors_.registerFactory(&editor::makeAnimEditor);
    // APPENDED, not inserted: AssetEditorHost::open() tries factories in registration order, so
    // moving this ahead of the others would change which editor claims a file they both accept.
    assetEditors_.registerFactory(&editor::makeGraphEditor);
    // THE GRAPH EDITOR CAN FINALLY ASK WHETHER A GRAPH IS VALID. Graph.Validate() and
    // OcGraphParser carry about thirty errors that name the offending node and say what to do,
    // and nothing here had ever called one: the checks are C# and the editor is C++. Installed
    // before any file opens, because makeGraphEditor takes only a path and hands the validator
    // to each editor it builds.
    //
    // A LAMBDA OVER scripts_, not a direct dependency, so GraphEditor keeps its Core + Formats +
    // ImGui dependency set and stays drivable from a headless test with no .NET runtime at all.
    // ScriptHost::graphValidate is itself a no-op returning "available? no" when the staged
    // bridge predates the GraphValidate export, so an old bridge greys the button out rather
    // than claiming every graph is fine.
    //
    // GUARDED ON SCRIPTING, and this whole run of four calls with it: every one of them reaches
    // scripts_, which IS the .NET host (declared `#if AVER_MODULE_SCRIPTING` beside the rest of
    // the bridge). The validator and the hit table are answers only managed code can give -- there
    // is no C++ implementation of Graph.Validate to fall back on -- so a scripting-off tree leaves
    // both seams uninstalled, which is the state GraphEditor already handles: setGraphValidator is
    // never called, the Validate button greys out exactly as it does against an old bridge, and
    // setGraphNodeHitSource's absence leaves the node-hit overlay with nothing to draw. The graph
    // editor itself stays registered above, because opening and editing a .ocgraph needs no host.
#if AVER_MODULE_SCRIPTING
    editor::setGraphValidator([this](const std::string& text, std::string& err) {
        if (!scripts_.graphValidateAvailable()) { err = "the .NET bridge exports no GraphValidate"; return false; }
        return scripts_.graphValidate(text, err);
    });
    // AND WHICH NODES ARE RUNNING. Recording is armed here rather than per tab because the
    // managed table is keyed by graph NAME and costs a static bool test when off -- arming it once
    // while the editor is up is simpler than tracking tab lifetimes, and a packaged game (which
    // has no editor) never arms it at all.
    editor::setGraphNodeHitSource([this](const std::string& graphName, f32 maxAge,
                                         std::vector<std::pair<std::string, f32>>& out) {
        scripts_.graphNodeHits(graphName, maxAge, out);
    });
    scripts_.graphSetHitRecording(true);
#endif  // AVER_MODULE_SCRIPTING
    // Appended for the same reason, and it claims only .ocbt, which nothing above accepts.
    assetEditors_.registerFactory(&editor::makeBtEditor);
    // And again for .ocsnd, which likewise nothing above claims. See SoundEditor.hpp.
    assetEditors_.registerFactory(&editor::makeSoundEditor);
    // And again for .ocparticle -- the format, the runtime and level-drop placement all already
    // existed with zero authoring UI until now. See ParticleEditor.hpp.
#if AVER_MODULE_PARTICLES
    assetEditors_.registerFactory(&editor::makeParticleEditor);
#endif
    // And again for .ocfoliage -- the eighth factory. Unconditional, unlike the particle one just
    // above: see FoliageTypeEditor.hpp/OcFoliage.hpp for why this tab needs no optional module.
    assetEditors_.registerFactory(&editor::makeFoliageTypeEditor);
    // And again for .ocinput -- the ninth factory. Unconditional, like the foliage one just above:
    // see InputSchemeEditor.hpp/OcInput.hpp for why this tab needs no optional module either.
    assetEditors_.registerFactory(&editor::makeInputSchemeEditor);
    {
        // What the tab needs to answer "is this the project's Input Scheme?" and to make it one --
        // see InputSchemeEditorHooks' own comment (InputSchemeEditor.hpp) for why this is a hooks
        // struct rather than a project pointer on AssetEditor itself.
        editor::InputSchemeEditorHooks hooks;
        hooks.contentDir = [this] { return project_.contentDir(); };
        hooks.projectInputScheme = [this] { return project_.inputScheme; };
        hooks.useAsProjectInputScheme = [this](const std::string& contentRelativePath) {
            if (!project_.valid()) return false;
            // THE SAME projectDirty_ FLAG Project Settings > Description's own fields set
            // (SandboxSettings.cpp's "Input Scheme" combo right beside this) -- the actual write
            // happens on the existing autosave timer (maybeAutosaveProject) or that page's Save
            // button, not here, so this button behaves exactly like typing a new Start Map there.
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
#endif  // AVER_WITH_IMGUI -- openInIde/ideName reach the content browser's IDE picker; without a
        // UI there is no picker and no actor editor to open a file from in the first place.
        editor::setActorEditorHooks(std::move(hooks));
    }
    window_ = e.window();
    // The one event sink the editor installs, identical to GameApp's. Everything the window
    // produces lands in the accumulator; nothing is filtered here, because filtering is policy
    // and policy is decided per-consumer, per-frame, further down.
    if (window_) window_->setEventCallback(&sandboxWindowEvent, &input_);
    // Dragging files in from Explorer. Off by default in Window itself (see
    // Window::setAcceptDroppedFiles); the editor is the one host that wants it. Polled once a frame
    // in onUpdate, next to the identically-shaped hasPendingOpenRequest().
    if (window_) window_->setAcceptDroppedFiles(true);

    // Single-instance forwarding, receiver registration. singleInstanceEligible_ is a superset of
    // the sender's own forward-attempt gate ("argc==2 and argv[1] doesn't start with '-'") by
    // construction, so no separate bookkeeping is needed to keep them in sync. A --frames capture
    // is a real windowed launch (Window.hpp:17) but always carries a flag, so it's ineligible --
    // two concurrent captures never race to open the same named mutex.
    if (window_ && window_->valid() && singleInstanceEligible_) {
        Window::registerAsSingleInstancePrimary(window_->nativeHandle());
        // THE HOOK IS THE LEVEL-OPEN PATH, which is why it carries a guard the registration above
        // does not: onOpenRequestThunk forwards to handleOpenRequest and on to requestOpenLevel
        // (SandboxLevelEdit.cpp), all three declared `#if AVER_MODULE_SCENE` because what a
        // forwarded path asks for is a level instantiated into a world. Staying the primary
        // instance is still right in a scene-less tree -- a second launch should still focus this
        // window rather than open a rival editor; it simply has nothing to open once it is here.
#if AVER_MODULE_SCENE
        window_->setOpenRequestHook(&SandboxApp::onOpenRequestThunk, this);
#endif
    }
    // THE WINDOW'S X BUTTON GOES THROUGH THE SAME UNSAVED-CHANGES CHECK AS File > Exit. It did
    // not: WM_CLOSE set shouldClose_ and Engine::run tests that BEFORE the next frameStep, so
    // the prompt could never be drawn and an unsaved level died with the window. Registered
    // unconditionally, not behind singleInstanceEligible_ above -- a --frames capture wants this
    // guard to say yes, which it does, rather than not to exist.
    if (window_ && window_->valid()) window_->setCloseGuard(&SandboxApp::onCloseGuardThunk, this);

#if AVER_MODULE_MCP
    // 0 means --mcp was never given. mcpStart (SandboxMcp.cpp) is the whole of what used to be
    // inline here: ABI registration, the UI-only widget hooks, and the listen itself, now shared
    // with the status-bar widget's own Start button so the two can never start the channel two
    // different ways.
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
    // THE CONTROL RIG, registered and INSTALLED here for the same reason the Synapse systems are
    // above: before any project opens, needing no level. Registration alone would be the defect
    // this repository keeps finding -- a component type nothing ever reads -- so the system that
    // applies it is installed in the same breath, as AnimSystem's pose modifier.
    //
    // Installed unconditionally rather than when a rig first appears: the modifier costs one
    // component lookup per animated entity per tick and returns immediately when there is no
    // CControlRig, whereas installing it lazily would mean a rig attached at run time did
    // nothing until something noticed.
    anim::controlRigSystem().registerComponents(scene::World::instance());
    anim::controlRigSystem().install(anim::animSystem(), scene::World::instance());
#endif

    // RESOLVED BEFORE THE PROJECT OPENS -- moved up from further down onInit, where the same code
    // silently disabled the entire GPU per-cluster path for the process lifetime: loadProjectMeshes
    // only builds meshClusterGpu_ under lodMeshShaderEnabled_, and ensureLodMeshPipeline latches
    // "already tried" BEFORE testing the flag, with no warning at all.
#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
    // OFF UNLESS ASKED FOR, a downgrade from "on by default where hardware allows" -- it was
    // never actually on: the first real run measured 22% faster (frame 76.3->59.8ms, scene draw
    // 51.7->41.5, triangles 8M->2.9M) but rendered every plant in Electric Dreams as a black
    // shredded silhouette.
    // SHADING FIXED since: PSClusterMain is now a real material shader (ClusterMaterialShader.hpp).
    // STAGE 3 added shadows/GI (still opt-in): Voxi's GI volume and shadow map MERGE into this
    // pipeline's table 0 (ensureLodMeshPipeline, D3D12Device.cpp's nullFill keeps
    // rhi::kBindingTableCount at 2); PSClusterMain runs the same shadowFactor()/
    // coneTracedIndirect() PSMainVoxi's non-ray-traced fallback does.
    // STILL NOT PARITY, hence still off by default: no ray tracing on this path ever, so it looks
    // visibly softer/un-reflective. D3D12 only; a build without AVER_MODULE_VOXI gets none of it.
    lodMeshShaderEnabled_ = lodMeshShaderRequest_ > 0;
    if (lodMeshShaderEnabled_) {
        const rhi::DeviceCaps mcaps = e.device()->caps();
        // D3D12 only, enforced here: Voxi's merge into table 0 puts a Texture3D, an acceleration
        // structure and structured buffers in a table Vulkan's descriptorLayout() still builds as
        // if every slot were a Texture2D. meshShaderTier is NOT a backend proxy: VulkanDevice sets
        // it from VK_EXT_mesh_shader, so a Vulkan device with mesh shaders passes every other check.
        const bool ok = mcaps.meshShaderTier > 0 && mcaps.shaderModel >= 65 && mcaps.dxcAvailable
#if AVER_MODULE_VOXI
                        && e.device()->backend() == rhi::Backend::D3D12
#endif
                        ;
        lodMeshShaderEnabled_ = ok;
        if (ok) {
#if AVER_MODULE_VOXI
            // Receives AND casts: cluster-dispatch calls voxiRenderer_.submit() directly with the
            // same (mesh, world, material) shape drawMesh() uses, even though it still skips
            // IRenderFeature::submitDraw. A cluster-drawn plant's shadow proxy is now in the shadow
            // cascade and GI voxelisation like any other instance; only its lit geometry stays
            // cluster-dispatched, never ray traced.
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
        // Through the same gate as a click: a CLI-named project used to reach browser_.open()
        // directly, skipping the upgrade prompt a double-clicked card got. When the gate declines,
        // it has raised the modal, so this arms the browser rather than reporting failure.
        // --open-legacy is the answer given in advance: it opens an older-series project WITHOUT
        // upgrading, what a benchmark or capture wants and the modal can't express.
        std::string err;
        const bool opened = openLegacy_ ? browser_.open(projectPath_, &err)
                                        : browser_.openOrOfferUpgrade(projectPath_, &err);
        if (opened) applyProject(e);
        else if (!openLegacy_ && browser_.upgradePending()) {
            armBrowser(true);
            // A question nobody is there to answer is a failed run: with a frame limit set, the
            // session renders an EMPTY EDITOR and reports timings/screenshots that look ordinary
            // and mean nothing (the tell, `over 0 entities`, is buried in a scene-walk line).
            // maxFrames_ != 0 is this file's established test for "not interactive".
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
        // A level with no project above it still opens: everything else hangs off applyProject,
        // so without this branch a lone .ocmap would produce an empty editor with no explanation.
        // Its placements won't resolve, but seeing the level's shape beats seeing nothing.
        //
        // GUARDED because loading a level IS instantiating entities: loadStartMap is declared
        // `#if AVER_MODULE_SCENE` with the rest of the level verbs, and there is no world for a
        // placement to become without the module. The path is still accepted off the command line,
        // so the #else says it was seen and why it went nowhere -- which is the whole complaint
        // the branch above exists to answer, an editor sitting empty with no explanation.
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
    // Built-in primitive meshes (sphere/cube/drone), their bounds, and the named-surface look
    // table gameplay can ask for all come from content_ now -- GameContent::registerBuiltins
    // uploads the identical set. The hook below is this editor's own per-mesh follow-up (pick
    // geometry, triangle counts; see onMeshLoaded), run once per built-in as it uploads.
    {
        MeshLoadPass pass; pass.app = this; pass.engine = &e;
        content_.setMeshLoadedHook(&SandboxApp::onMeshLoaded, &pass);   // seeds meshTris_ and pickGeometry_ per built-in
        content_.registerBuiltins(*e.device());
        content_.setMeshLoadedHook(nullptr, nullptr);

        // The built-in unit cube's own handle, kept so a dev check can build geometry of its own
        // without re-uploading a cube.
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

    // Per-mode gizmos: normal + amber-highlight variant of each axis.
    for (int a = 0; a < 3; ++a) {
        auto mv=buildMoveAxis(a,kAxisCol[a]);   gzMove_[a]  =e.device()->createLineMesh(mv.data(),(u32)mv.size());
        auto mh=buildMoveAxis(a,kAxisHi);       gzMoveHi_[a]=e.device()->createLineMesh(mh.data(),(u32)mh.size());
        auto rr=buildRotRing(a,kAxisCol[a]);    gzRot_[a]   =e.device()->createLineMesh(rr.data(),(u32)rr.size());
        auto rh=buildRotRing(a,kAxisHi);        gzRotHi_[a] =e.device()->createLineMesh(rh.data(),(u32)rh.size());
        auto sc=buildScaleAxis(a,kAxisCol[a]);  gzScale_[a] =e.device()->createLineMesh(sc.data(),(u32)sc.size());
        auto sh=buildScaleAxis(a,kAxisHi);      gzScaleHi_[a]=e.device()->createLineMesh(sh.data(),(u32)sh.size());
    }

#if AVER_MODULE_LANDSCAPE
    // The sculpt brush's footprint ring: buildRotRing(2, ...) is perpendicular to Z, i.e. already
    // flat in the XY ground plane heights are measured from (OcLand.hpp: "heights along +Z").
    // Reused rather than duplicated; only radius and world position vary, per-frame via the draw matrix.
    { auto bring = buildRotRing(2, kAxisHi); brushRing_ = e.device()->createLineMesh(bring.data(), (u32)bring.size()); }
#endif

#if AVER_MODULE_SCENE
    // The scene join. Registered HERE, before the Voxi renderer, for the reason spelled out
    // below: features run prePass in registration order and Voxi reads vertex buffers in its.
    skinnedScene_ = std::make_unique<aver::render::SkinnedScene>();
    if (skinnedScene_->init(*e.device())) {
        skinnedScene_->setResolvers(&game::GameContent::resolveAnimAsset, &game::GameContent::resolveSceneMesh, &content_);
        e.device()->addRenderFeature(skinnedScene_.get());
    } else {
        skinnedScene_.reset();   // init already said why; skinned entities draw at rest
    }
#if AVER_MODULE_RENDER_SOFTBODY
    // The same two resolvers skinnedScene_ takes (id->path, id->MeshHandle already uploaded) --
    // reused rather than adding a third pair, so both features agree on one resolver convention.
    softBodyScene_ = std::make_unique<aver::render::SoftBodyScene>();
    if (softBodyScene_->init(*e.device())) {
        softBodyScene_->setResolvers(&game::GameContent::resolveSceneMesh, &game::GameContent::resolveAnimAsset, &content_);
        e.device()->addRenderFeature(softBodyScene_.get());
    } else {
        softBodyScene_.reset();   // init said why; soft entities draw their authored mesh
    }
#endif

// The content browser's thumbnails. init() registers its own preview/copy-pass features
// internally, so unlike skinnedScene_ above there's no addRenderFeature call here. A failed init()
// leaves ready() false and textureId() permanently 0, which the gallery treats as "draw the typed icon".
    thumbnails_.init(*e.device());

    // --skin-scene-test <dir>: the same question as --skin-draw-test but through the WHOLE
    // chain -- a real .ocmesh with skin streams, a real .ocskel, a real .ocanim, AnimSystem,
    // SkinnedScene's per-entity target, and the substituted draw handle.
    if (!skinSceneDir_.empty() && skinnedScene_) {
        skinScene_ = std::make_unique<aver::editor::SkinSceneTest>();
        u64 meshId = 0, skelId = 0, clipId = 0;
        u32 meshHandle = 0;
        if (skinScene_->setup(e, skinSceneDir_, &meshId, &skelId, &clipId, &meshHandle)) {
            // The draw pass looks the entity's mesh up in content_'s meshes, and SkinnedScene
            // resolves through the SAME table. Registering here is what makes a spawned
            // entity actually reach a draw call without a project having been opened.
            std::pair<Vec3, Vec3> bounds;
            skinScene_->restBounds(bounds.first, bounds.second);
            content_.registerMesh(meshId, meshHandle, bounds);
            // The three ids the entities name, pointed at the cooked files. content_'s index is
            // what AnimSystem and SkinnedScene both resolve through, so registering here is
            // what makes the rig reachable without a project.
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
    // Registered unconditionally, spawned only if a level asks for it -- an idle feature costs
    // nothing, and this keeps feature order identical with/without fluid in a level. Still before
    // Voxi: its acceleration-structure build reads the vertex buffer this feature's prePass writes;
    // registered after, every ray-traced effect would see the volume one frame stale.
    water_.init(*e.device());
#endif

#if AVER_FLUIDS_SIMULATED
    // The 3D-viewport icon renderer. Registered here because it draws in transparentPass (after
    // every opaque draw and the deferred sky) but must exist before level loading creates a Player
    // Start.
    // A failed init is not fatal: viewportIconsReady_ staying false just means the Player Start
    // keeps its old cube look -- permanent on Vulkan, which has no transparentPass call at all.
    viewportIconsReady_ = viewportIcons_.init(*e.device());
    if (viewportIconsReady_) {
        e.device()->addRenderFeature(&viewportIcons_);
        playerStartIcon_ = viewportIcons_.loadIcon(executableDir() + "\\" + "player-start-icon.png",
                                                   "PlayerStartIcon");
        // No icon file, no icon renderer: falling back to the cube is better than a Player Start
        // that is invisible because its picture failed to load.
        if (playerStartIcon_ == editor::ViewportIconRenderer::kNoIcon) viewportIconsReady_ = false;
    }
#endif

    // --furnace-test: does the shading model CONSERVE ENERGY? A uniform environment of
    // radiance L and surfaces of albedo 1 -- every one must read the same, whatever its
    // orientation and whatever surrounds it.
    if (furnaceTest_) {
        objects_.clear();
        sel_ = -1;
        sky_.furnaceRadiance = 0.25f;
        sky_.furnaceSun = furnaceSun_;
        sunAmbient_ = 1.0f;

        // Albedo ONE, fully rough, non-metallic: the furnace's premise is a perfect Lambertian
        // white, and any of those three wrong makes the answer legitimately not L.
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
        // A flat slab, and a tall one. Different faces point in different directions, so the
        // probes across them sample several orientations of the same white surface.
        white("FurnaceFloor", Vec3{0, 0, -30}, Vec3{900.0f, 900.0f, 20.0f});
        white("FurnaceTall",  Vec3{0, 0, 200}, Vec3{160.0f, 160.0f, 220.0f});
        // THE ONE THAT MATTERS: a box open only toward the camera, so its back surface is
        // occluded from most of the hemisphere. In a furnace it must STILL read L, since the
        // occluding walls emit L too -- where a forgetful occlusion term shows up.
        white("FurnaceCaveBack",  Vec3{-520, -520, 160}, Vec3{20.0f, 200.0f, 200.0f});
        white("FurnaceCaveLeft",  Vec3{-330, -700, 160}, Vec3{200.0f, 20.0f, 200.0f});
        white("FurnaceCaveTop",   Vec3{-330, -520, 350}, Vec3{200.0f, 200.0f, 20.0f});

        // --furnace-grid: the specular half of the same question. The boxes above are albedo 1,
        // roughness 1, metallic 0, exercising only the diffuse path. A flat plate faces the camera
        // down -X so each cell is at normal incidence (ndv=1, the SMALLEST the loss ever gets --
        // a floor on the error, not the whole of it). Replaces the five boxes, so existing probes see the same thing.
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
                    // THIN, so a tilt shows the same face at a glancing angle rather than swapping
                    // to the side one -- at 5cm thick and 75deg yaw the side face was wider on
                    // screen than the front, and the tilt sweep came back flat and meant nothing.
                    o.scale = Vec3{0.5f, 60.0f, 60.0f};
                    // --furnace-tilt rotates each plate about Z so the cell reads at grazing
                    // incidence, not face-on -- single-scatter GGX loses most energy at low ndv.
                    // What tilt cannot show: at albedo 1 a metal has F0=1 and averEnvBRDF keeps
                    // dfg.x+dfg.y nearly constant in ndv there, so the white metal row reads the
                    // same at every tilt regardless of correctness -- it only earns its keep on
                    // coloured metals/dielectrics.
                    o.rotDeg = Vec3{furnaceTilt_, 0.0f, 0.0f};
                    o.color[0] = o.color[1] = o.color[2] = 1.0f;
                    o.metallic  = metal[mi];
                    o.roughness = rough[ri];
                    o.aabbMin = Vec3{-o.scale.x*1.1f, -o.scale.y*1.1f, -o.scale.z*1.1f};
                    o.aabbMax = Vec3{ o.scale.x*1.1f,  o.scale.y*1.1f,  o.scale.z*1.1f};
                    objects_.push_back(o);
                }
            }
            // The plates normally have no material at all -- they draw through the fallback path
            // with metallic/roughness per-draw, how every furnace measurement to date was taken;
            // giving them materials unconditionally would move all of them. Only when a coat is
            // asked for, since the furnace's oracle is intra-frame and nothing is compared across runs.
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

        // The mirror: a large flat quad at z=0, fully metallic and almost perfectly smooth, so
        // what it shows is almost entirely the reflection rather than its own colour.
        MeshObj m;
        m.name = "ReflMirror";
        m.mesh = unitCubeMesh_;
        m.pos = Vec3{0, 0, -20.0f};
        // HALF-EXTENTS IN CENTIMETRES: the unit cube is half-extent 1, so this is an 1800 cm
        // mirror. The first version used 18 and made a 36 cm slab that every probe missed.
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
        // Big enough to subtend several degrees from 5200 cm away, or the reflection of it
        // lands between probes.
        bcn.scale = Vec3{700.0f, 700.0f, 700.0f};
        // Emissive-bright green: nothing else in this scene is green, so a probe that turns
        // green can only be showing the beacon.
        bcn.color[0] = 0.02f; bcn.color[1] = 0.95f; bcn.color[2] = 0.05f;
        bcn.roughness = 1.0f;
        bcn.aabbMin = Vec3{-950, -950, -950}; bcn.aabbMax = Vec3{950, 950, 950};
        objects_.push_back(bcn);
        reflBeaconIndex_ = static_cast<int>(objects_.size()) - 1;

        refl_->setBeaconPosition(bp);
        refl_->setVolumeExtent(giExtent_);
    }

    // --skin-draw-test: the same question one level up -- does anything DRAW the skinned buffer.
    // This block's position is the contract: prePass runs features in registration order, so a
    // skinning dispatch must be registered BEFORE the scene renderer, whose Voxi replay reads the
    // vertex buffer this dispatch writes -- registered after, it reads unwritten data every frame.
    if (skinDrawTest_) {
        skinDraw_ = std::make_unique<aver::editor::SkinDrawTest>();
        if (skinDraw_->init(*e.device())) {
            e.device()->addRenderFeature(skinDraw_.get());
            // The scene becomes exactly one object, so the probe reads the box or the sky and
            // never an unrelated mesh that happens to sit behind it.
            objects_.clear();
            sel_ = -1;
            // The ground first, so it is behind the box in submission order as well as in
            // depth. Pale and rough, because the assertion is about how much light reaches it.
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

            // The ground probe is only an acceleration-structure test when ray tracing draws the
            // shadow -- told rather than guessed, so the report says which question it answered.
            // Whether RT is ON is the test's own schedule to drive: the experiment IS the toggle.
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

        // ANIMATION NOTIFIES: the editor wants this as much as a shipped game does (a clip
        // previewed in Play mode should fire what it fires), and the sink survives every level/
        // project reload, since AnimSystem::clear() drops clips and playheads without dropping the wire.
        //
        // THE WHOLE INSTALL NEEDS THE SCENE, not just the synapse half that already said so.
        // aver::anim::animSystem() lives in Aver.Anim.Scene, which the root CMakeLists only adds
        // inside `if(AVER_MODULE_SCENE)`, and the sink being installed -- animNotify -- takes a
        // scene::Entity and is now declared under the same condition. A notify is a thing that
        // happens TO AN ENTITY; with no entities there is nothing to route, so the sink is not
        // installed rather than installed against a world that does not exist.
#if AVER_MODULE_SCENE
        if (scripts_.graphFireAvailable()) {
            anim::animSystem().setNotifySink(&SandboxApp::animNotify, this);
#if AVER_MODULE_SYNAPSE_SCENE
            // The SAME sink as animNotify immediately above -- its body is just
            // scripts_.graphFire(entity, name), nothing anim-specific, and PerceptionSystem's
            // NotifyFn is byte-for-byte AnimNotifyFn's own signature (SynapsePerception.hpp).
            synapse::perceptionSystem().setNotifySink(&SandboxApp::animNotify, this);
            // The built-in "FireEvent" BT action reaches a graph the SAME way.
            synapse::btSystem().setNotifySink(&SandboxApp::animNotify, this);
#endif
        }
#endif  // AVER_MODULE_SCENE

        // ANIMATION CURVES. The framework relays a query it cannot answer itself; this is where
        // the answer comes from. Installed unconditionally -- unlike the notify sink it needs no
        // scripting host, because a C++ caller can ask too.
        //
        // "UNCONDITIONALLY" MEANT "WITHOUT ASKING THE HOST", not "in every build": every
        // aver_fw_* name in this run comes from aver/framework/framework_abi.h, which the header
        // includes under `#if AVER_MODULE_FRAMEWORK`. The relay cannot exist without the thing
        // relaying. Scripting is a separate option from the framework (module-matrix.ps1's
        // scene-off row turns FRAMEWORK off and leaves SCRIPTING on), which is what exposed it.
#if AVER_MODULE_FRAMEWORK
        aver_fw_set_anim_curve_provider(&SandboxApp::animCurve, this);
#endif

#if AVER_MODULE_SYNAPSE_SCENE
        // THE FRAMEWORK GUARD MOVED OUT to cover all three, because the two relays it did not
        // cover are aver_fw_* names too -- it sat around the resolver alone, which reads as "the
        // resolver is the framework-dependent one" when in fact it is the least so.
#if AVER_MODULE_FRAMEWORK
        // GetSynapseTarget (Aver Node) reaches CSynapseAgent's current waypoint through this --
        // same reason and same placement as the anim-curve provider immediately above.
        aver_fw_set_synapse_target_provider(&SandboxApp::synapseTarget, this);
        // GetSynapsePerception (Aver Node) reaches CSynapsePerception's current sight state
        // the same way.
        aver_fw_set_synapse_perception_provider(&SandboxApp::synapsePerception, this);
        // PerceptionSystem's own resolver seam (SynapsePerception.hpp), NOT a framework_abi.h
        // relay -- see that header's own comment for why Aver.Synapse.Scene must not link
        // Aver.Framework at all, so only a composition root (linking both) can answer this. It
        // still needs the guard: synapseTargetResolver is itself declared under it.
        synapse::perceptionSystem().setTargetResolver(&SandboxApp::synapseTargetResolver, this);
#endif
#endif

        // SAVE/LOAD, so a project can test its own save path in the editor rather than only in
        // a shipped game -- which, given there is no packaged game today, is the only place it
        // can be tested at all.
        //
        // BOTH CONDITIONS, not just the framework one the ABI call needs: saveWriteProvider and
        // saveLoadProvider are the inline pair declared `#if AVER_MODULE_SCENE &&
        // AVER_MODULE_FRAMEWORK` in the header (they save and load a whole scene::World through
        // Aver.Save, which the root CMakeLists adds only with the scene on), so naming them here
        // under a narrower condition than their own declaration would be the same inconsistency
        // one level down.
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
        aver_fw_set_save_provider(&saveWriteProvider, &saveLoadProvider, this);
#endif

        // Graph-as-class catch-up, for a project opened from the command line: applyProject's own
        // "Starting scripts" stage already tries this, but a CLI project opens above this block,
        // before the scripting host bootstraps, so scripts_.ready() was false and that attempt was
        // a documented no-op. Both calls are idempotent, so calling them again here changes nothing.
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

        // Virtualized geometry is on by default -- was opt-in behind --lod-mesh-shader, an
        // indefensible default: pine_tree_01 is 17.18M triangles, and with the flag off the
        // editor drew all of them at LOD0 (~258M triangles for "2 actors", 13 FPS). Auto (-1)
        // decides from caps; the flag pins it either way.
        // Decision moved to just before the project opens: it must precede applyProject, which
        // loads meshes whose GPU cluster buffers are built only when this flag is already true.

        voxi::DeviceInfo di;
        di.msaaMask = caps.msaaMask; di.maxMsaaSamples = caps.maxMsaaSamples;
        di.rayTracingTier = caps.rayTracingTier; di.computeShaders = caps.computeShaders;
        di.typedUavLoads = caps.typedUavLoads; di.conservativeRaster = caps.conservativeRaster;
        di.shaderModel = caps.shaderModel; di.meshShaderTier = caps.meshShaderTier;
        di.dxcAvailable = caps.dxcAvailable;
        // R1: computed only at this site and GameApp::attachVoxi's mirror, same expression both
        // places -- see DeviceInfo::nrdSupported's own comment (Voxi.hpp) for why the struct only
        // carries the answer rather than deriving it itself.
        di.nrdSupported = e.device()->backend() == rhi::Backend::D3D12 && render::nrd::Denoiser::available();
        voxi::Renderer::get().setDeviceInfo(di);

        // THE PROJECT MANIFEST GOES IN HERE, BEFORE THE FLAGS AND BEFORE init().
        //
        // WHY BEFORE init(): init() calls createVoxelVolume(settings_.voxelResolution) and that
        // is the ONLY place the volume is ever sized -- nothing in the renderer watches the field
        // afterwards. The manifest used to arrive after init, via the post-attach
        // applyProjectRenderSettings call, so every project ran the 128^3 default no matter what
        // it asked for. PTTest states RENDER.VOXELRES 512 and ran a grid 64x smaller in every
        // dimension, while the log said "applied render settings from ...". Seeding here means
        // the volume is simply CREATED at the right size: no resize, no descriptor rebind, and
        // no GPU resource recreated at a moment that could remove the device.
        //
        // WHY BEFORE THE FLAGS: `s` below is seeded FROM this singleton, and the flag block that
        // follows writes over it. Putting the manifest first is what keeps "a flag is a human
        // standing right there, a manifest is a recorded preference" true at startup, with no
        // second precedence pass needed here -- the ordering does it. (applyProjectRenderSettings
        // still runs its own take() pass later, for the mid-session project-open path where the
        // flags were consumed long ago.)
        //
        // AFTER setDeviceInfo, because setSettings clamps against the device it is told about.
        applyProjectVoxiSettings();

        voxi::Settings s = voxi::Renderer::get().settings();
        s.msaa = static_cast<voxi::Msaa>(e.device()->sampleCount());
        if (msaaOverride_) s.msaa = static_cast<voxi::Msaa>(msaaOverride_);
        // --no-gi wins over --gi.
        if (giForceOff_)          s.globalIllumination = voxi::Quality::Off;
        else if (giOverride_ >= 0) s.globalIllumination = static_cast<voxi::Quality>(giOverride_);
        // --no-rt wins over --rt, mirroring --no-gi above. No longer the ONLY way to reach Off --
        // the tier overrides now use -1 for "not given", so `--rt 0` says it too -- but it stays
        // since every existing script/gate config spells it this way.
        if (rtForceOff_)          s.rayTracing = voxi::Quality::Off;
        else if (rtOverride_ >= 0) s.rayTracing = static_cast<voxi::Quality>(rtOverride_);
        // Path tracing is its own tier, its own flag: gained one when ptBounces started gating on
        // it, or --pt-bounces would be permanently inert in the automation context it's benchmarked
        // in. No --no-pt; `--pt 0` turns the scene-suppressing path-traced view off.
        if (ptOverride_ >= 0) s.pathTracing = static_cast<voxi::Quality>(ptOverride_);
        if (msOverride_) s.meshShaders = true;
        // Applied to `s`, not voxiRenderer_ directly, and BEFORE the singleton below: setSettings()
        // reruns every frame from voxi::Renderer::get().settings(), so a one-time poke would be
        // overwritten. A negative count is reported, not cast-and-clamped (old warning read
        // "4294967291 shadow rays clamped to 32").
        // TIERS FIRST, IN THEIR OWN CALL: setSettings decides "did the caller set this" BY VALUE,
        // so asking for the already-held value is indistinguishable from never asking, and the
        // tier silently wins (`--rt 4 --rt-rays 1` measured the same 17.4ms as `--rt 4` alone).
        // Two calls fixes it: apply tiers, read back, then apply explicit knobs in a SECOND call.
        voxi::Renderer::get().setSettings(s);
        auto k = voxi::Renderer::get().settings();
        if (rtRaysOverride_ > 0) k.rtShadowRays = static_cast<u32>(rtRaysOverride_);
        else if (rtRaysOverride_ < 0)
            AVER_WARN("[Sandbox] --rt-rays {} is not a ray count; the default of {} stands",
                      rtRaysOverride_, k.rtShadowRays);
        // >= 0, not > 0: 0 is a MEANING here (filter off), not "flag absent" -- the sentinel
        // is -1, unlike its two neighbours whose valid range starts at 1.
        if (rtShadowDenoiseOverride_ >= 0) k.rtShadowDenoise = static_cast<u32>(rtShadowDenoiseOverride_);
        if (rtRenderModeOverride_    >= 0) k.rtRenderMode    = static_cast<u32>(rtRenderModeOverride_);
        if (ptBouncesOverride_       >= 0) k.ptBounces       = static_cast<u32>(ptBouncesOverride_);
        if (layeredBsdfOverride_     >= 0) k.layeredBsdf     = static_cast<voxi::Quality>(layeredBsdfOverride_);
        // IN THE SECOND CALL, and that is load-bearing rather than tidy: giSkyOcclusionRays is
        // derived from the rayTracing tier on a tier CHANGE, and the block at the top of this
        // function explains why a knob set during the FIRST call cannot be distinguished from one
        // never set at all. >= 0 rather than > 0 because 0 means "use the cone gather's
        // occlusion", not "flag absent".
        if (giSkyOccRaysOverride_ >= 0) k.giSkyOcclusionRays = static_cast<u32>(giSkyOccRaysOverride_);
        if (giSkyOccTileOverride_ >= 0) k.giSkyOcclusionTile = static_cast<u32>(giSkyOccTileOverride_);
        if (rtPixelsPerRayOverride_ > 0) k.rtPixelsPerRayTile = static_cast<u32>(rtPixelsPerRayOverride_);
        else if (rtPixelsPerRayOverride_ < 0)
            AVER_WARN("[Sandbox] --rt-pixels-per-ray {} is not a tile edge; the default of {} stands",
                      rtPixelsPerRayOverride_, k.rtPixelsPerRayTile);
        // Refraction's knobs go in the SECOND call for the reason the block above spells out:
        // a tier is changing in the first, and refractionMode is tier-derived, so setting it
        // there would be indistinguishable from not setting it and the tier would win.
        if (refractionOverride_ >= 0)          k.refractionMode = static_cast<u32>(refractionOverride_);
        if (refractionStrengthOverride_ >= 0.0f) k.refractionStrength = refractionStrengthOverride_;
        if (refractionFadeOverride_ >= 0.0f)     k.refractionEdgeFade = refractionFadeOverride_;
        if (giUpdateIntervalOverride_ > 0) k.giUpdateInterval = static_cast<u32>(giUpdateIntervalOverride_);
        else if (giUpdateIntervalOverride_ < 0)
            AVER_WARN("[Sandbox] --gi-update-interval {} is not a frame count; the default of {} stands",
                      giUpdateIntervalOverride_, k.giUpdateInterval);
        // >= 0, not > 0: 0 is a real ESTIMATOR ("voxel cones"), not "flag not given" -- the
        // sentinel is -1, same reasoning as rtShadowDenoiseOverride_ and giSkyOccRaysOverride_ above.
        if (giModeOverride_ >= 0) k.giMode = static_cast<u32>(giModeOverride_);
        // Same -1 sentinel reasoning: 0 is a real answer ("denoiser off"), not an absent flag.
        if (denoiserOverride_ >= 0) k.denoiser = denoiserOverride_ != 0;
        // --reblur-accum N: the console's voxi.reblurMaxAccumulatedFrameNum, for a --frames run that
        // has no console. No manifest key names it, and the manifest apply starts from the live
        // settings, so setting it here once is enough; Voxi clamps it to NRD's own [0,63].
        if (reblurAccumOverride_ >= 0) {
            k.reblurMaxAccumulatedFrameNum = static_cast<u32>(reblurAccumOverride_);
            AVER_INFO("[Voxi] --reblur-accum {}: REBLUR_DIFFUSE history depth", reblurAccumOverride_);
        }
        voxi::Renderer::get().setSettings(k);
        AVER_INFO("[Voxi] attached: MSAA {}x, RT tier {}, SM {}, mesh tier {}", caps.maxMsaaSamples, caps.rayTracingTier, caps.shaderModel, caps.meshShaderTier);

        // Registration is non-owning: voxiRenderer_ must outlive the device, torn down in onShutdown.
        // Read back from the singleton rather than reusing `s` directly, so this sees the same
        // clamping voxi::Renderer::setSettings just applied.
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
        // Unlike --rd-ablate this is NOT a shader define, so it does not have to precede
        // init() -- but it is set here anyway, beside its sibling, so both measurement
        // dials are handed over in one place rather than one here and one somewhere else.
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
            // --import's deferred handshake, UI-only on purpose: importAsset belongs to the
            // content-browser half of this file and calls cbIsEditable/cbInvalidate/importModel,
            // which are the browser's own. A UI-less editor has no browser to import into.
            if (!importSrc_.empty() && !importDone_) {
                importDone_ = true;
                importAsset(importSrc_, importDst_);
            }
#endif
#if AVER_MODULE_PBR
            content_.setTextureFactory(e.device()->resources());
            voxiRenderer_.materials().setTextureResolver(&game::GameContent::resolveMaterialTexture, &content_);
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
    // NOT an IRenderFeature: it has no prePass/scenePass, only two methods
    // renderSceneEntities() calls directly from the CPU entity walk. Built unconditionally: cheap
    // to own, and every call it drives is already gated on occlusionCullEnabled_.
    if (rhi::IResourceFactory* occRes = e.device()->resources()) {
        occluder_ = aver::occlusion::createOcclusionCuller(*occRes);
        // --occlusion-waitidle's seed, applied once here and reasserted every frame from onUpdate
        // (see occlusionDebugForceWaitIdleArg_'s own comment) -- editor::
        // consoleOcclusionForceWaitIdleSlot() is EditorConsole.hpp's live source of truth from
        // this point on, so a console `set occlusion.debugForceWaitIdle` and this CLI flag are the
        // exact same switch, not two that can disagree.
        editor::consoleOcclusionForceWaitIdleSlot() = occlusionDebugForceWaitIdleArg_;
        occluder_->setDebugForceWaitIdle(occlusionDebugForceWaitIdleArg_);
        // Warmed up here, not on first per-frame call -- a safety requirement. ensureSized()
        // builds the module's three compute pipelines, and D3D12RenderContext caches the bound
        // pipeline as a raw pointer (pipe_): a push_back reallocating while another feature's
        // pointer rests on the old array is a dangling-pointer read. Calling it lazily from the
        // scene walk corrupted Voxi's cached pointer mid-frame and crashed the driver's shader
        // compiler. Warming up before Engine::run's first beginFrame() means nothing has a live
        // pointer into pipelines_ yet. A later resize still rebuilds the pyramid lazily -- accepted,
        // since resizes are rare and user-driven.
        rhi::TextureDesc occSceneDesc;
        if (const rhi::TextureHandle occDepth = e.device()->sceneDepthTexture();
            occDepth && occRes->textureInfo(occDepth, occSceneDesc)) {
            occluder_->ensureSized(*occRes, occSceneDesc.width, occSceneDesc.height, e.device()->sampleCount());
        }
    }
#endif

#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // Registered unconditionally (like skinnedScene_ above): a project's own CParticleEmitter
    // placements must draw without a command-line flag. Both the system and the effect library are
    // process-global singletons, so registerEffect() anywhere reaches whatever ticks it.
    particles::particleSystem().setEffectLibrary(&particles::particleEffects());
    if (particleRenderer_.init(*e.device())) {
        particleRenderer_.setSystem(&particles::particleSystem());
        e.device()->addRenderFeature(&particleRenderer_);
        particlesAttached_ = true;
#if AVER_MODULE_VOXI
        // DECIDED 4's seam, installed only once Voxi has attached (voxiAttached_) -- see
        // particleGiPrepare/particleGiBind. particleRenderer_ never learns Voxi's name; this is the
        // one call site handing it a way to reach it. --no-particle-gi skips just this line.
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
    // Water, registered on the same terms as everything else here: if init fails the editor is
    // unaffected and simply has no water, because HLSL is compiled at RUNTIME and can fail on a
    // machine whose build was perfectly green.
    if (waterEnabled_) {
        // A default swell rather than a flat mirror: four waves at spread headings, so the
        // surface reads as water immediately. Wavelengths are deliberately non-multiples of one
        // another -- harmonic ones re-phase into a visibly repeating tile.
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
            // The simulated surface is the same number as the rendered one -- two independent
            // heights would look like broken buoyancy rather than a mismatch, so the plane is set
            // from water_'s own level. Guarded on PHYSICS, not just fluids: rendering a
            // surface and giving things something to float on are different capabilities.
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

    // --particle-test: a dust cloud straddling an opaque cube, so the transparent pass's own
    // depth test is visible in one screenshot -- some particles nearer the camera than the cube,
    // some farther and hidden behind it.
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

        // A slow-drifting dust cloud spanning well in front of AND behind the cube along the
        // camera's forward axis (+X) -- that spread is what makes the occlusion visible in a single
        // static frame, without needing the camera or particles to move.
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

        // A second emitter, proving the opposite half of the seam: embers that ARE their own
        // light source (receivesGI=false, additive), placed beside the cube so they never overlap
        // the dust. If the dust's brightness changes under --no-particle-gi and this doesn't, that's the seam working.
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

        // High above the dust cloud's own top, clear of its footprint, so the two effects never
        // overlap. Open sky, no occluder behind it -- deliberate: this placement first exposed the
        // transparent-pass/sky ordering bug the reorder in D3D12Device::endFrame fixed (a particle
        // with no opaque depth behind it was silently overdrawn by the sky's depth-EQUAL-clear
        // fill). Left here rather than beside the cube: an emitter with nothing opaque behind it is
        // the common case and must render correctly without one.
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
    // --particle-stress <N> <M>: VERIFICATION-ONLY test content for the parity-and-price pass.
    // One fog/mist effect referenced by N emitter entities on a grid, each capped at M particles,
    // so emitter count and per-emitter count can each be varied independently to price the system.
    // No weapon vocabulary, no occluder: this measures cost, not the depth test --particle-test already proved.
    else if (particleStressEmitters_ > 0) {
        objects_.clear();
        sel_ = -1;

        particles::ParticleEffect fx;
        fx.shape = particles::EmitterShape::Box;
        fx.shapeSize = Vec3{350.0f, 60.0f, 90.0f};
        fx.emissionRate = 150.0f;   // a steady fog/mist emission rate, not a weapon effect
        fx.maxParticles = static_cast<u32>(particleStressMaxParticles_ > 0 ? particleStressMaxParticles_ : 400);
        // PRICE MEASUREMENT: burst the whole cap on frame 1 so steady-state population (and its
        // CPU/GPU cost) is reached immediately, instead of waiting emissionRate-many seconds for
        // the accumulator to fill it -- a --particle-stress-only choice, not authored-effect data.
        fx.burstCount = fx.maxParticles;
        // PRICE MEASUREMENT: a long, near-constant lifetime keeps the burst-filled population
        // steady for the whole capture window instead of decaying mid-run, which would contaminate
        // the price with a shrinking population -- a stress-harness-only choice.
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

        // --particle-stress2: VERIFICATION-ONLY, adds a second, differently-blended (additive)
        // small sparks-like emitter alongside the box mist above, so the price measurement can
        // also cover a scene mixing both blend pipelines in one frame, not just one.
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

    // --skin-test: the GPU skinning pass against its CPU reference, on this machine's real
    // device. Registered only when asked for (costs a waitIdle); typically run alongside
    // --debug-layer, which catches a malformed descriptor as opposed to a wrong number.
    if (skinTest_) {
        skinSelfTest_ = std::make_unique<aver::render::SkinSelfTest>();
        if (skinSelfTest_->init(*e.device())) e.device()->addRenderFeature(skinSelfTest_.get());
        else { AVER_ERROR("[Skin] self-test unavailable on this device"); skinSelfTest_.reset(); }
    }

    // --pt-furnace: does the PATH TRACER conserve energy? It brings its own geometry,
    // acceleration structures and accumulators; the editor only supplies the furnace itself
    // (setFurnaceTest put SkyAtmosphere::furnaceRadiance on, read through skyColor()).
    if (ptFurnaceTest_) {
        ptFurnace_ = std::make_unique<aver::pt::PtFurnaceTest>();
        if (ptFurnace_->init(*e.device())) e.device()->addRenderFeature(ptFurnace_.get());
        else { AVER_ERROR("[PT] furnace unavailable on this device"); ptFurnace_.reset(); }
    }

    // --pt-scene: the path tracer pointed at the REAL scene instead of the furnace's own private
    // geometry -- a progressive, still-camera reference view that SUPPRESSES the raster scene
    // while registered (sky plus one authored sun, static geometry, flat albedo only -- see
    // PtSceneView.hpp). setPtSceneView() already set ptSceneViewWantEnabled_; this call turns that
    // want into a registration, the same path the Path Tracing Quality combo uses later.
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
    // A RESTORED CAMERA COUNTS AS FRAMED, or the default view below would throw it away. The
    // entity test alone is not enough: a level can carry a CAMERA record and no placements at
    // all -- an empty level somebody has started laying out is exactly that -- and without this
    // the restore would be silently overwritten by the 700,700,450 fallback one line later.
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

// KEEPS THE TITLE BAR IN STEP WITH THE OPEN LEVEL. Before 2026-09-16 applyProject
// (SandboxProject.cpp) set the title exactly once, to the project's name, and nothing ever
// touched it again -- so two different levels in the same project showed the same title, and a
// level with unsaved edits looked identical to one just saved. Called once a frame from onUpdate.
//
// NO PROJECT OPEN: LEAVES THE TITLE ALONE. There is nothing to build a level-qualified title
// from, and BootConfig already put a sensible default there (see the constructor's `c.windowTitle`).
void SandboxApp::refreshWindowTitle(Engine& e) {
    if (!e.window() || !project_.valid()) return;
    // levelName_ FIRST: it is the level's own declared name (the world's `name` field on load, or
    // what Save As just wrote it to), so it survives a level that has moved on disk since. Only a
    // level that has never set one falls back to the saved file's stem -- and a level that has
    // neither yet (a brand new, never-saved one) reads as "untitled", matching every other place
    // in this file that names an unnamed level (see the exit prompt and Save Level As, both in
    // SandboxShell.cpp).
    //
    // THE LEVEL HALF IS GUARDED, the project half is not, and that split is the honest one: both
    // levelName_ and levelPath_ are declared `#if AVER_MODULE_SCENE` beside levelEntities_, because
    // every verb that can set either of them -- load, Save As, New Level -- is guarded there too.
    // A tree with no scene can never have a level open, so it shows the project-only title this
    // function was written to replace, rather than a permanent "untitled" naming nothing.
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
    // --set NAME VALUE: applied once, at frame 5 rather than frame 1. The project's own
    // RENDER.* apply runs during startup and would overwrite anything staged before it, so a
    // capture's dials have to land AFTER that -- and a handful of frames costs nothing in a run
    // long enough to measure a temporal artifact. runConsoleLine is the console's own entry point,
    // so a --set takes exactly the text the drawer takes, and reports the same errors to the log.
    //
    // WHICH IS ALSO WHY IT IS GUARDED: "the console's own entry point" means runConsoleLine is
    // part of the console, declared and defined `#if AVER_WITH_IMGUI` (SandboxShell.cpp). That
    // macro is defined only when Aver.RHI.D3D12.ImGui is built, so -DAVER_RHI_D3D12=OFF takes the
    // editor UI down with the backend and leaves this call naming nothing. The flag is still
    // PARSED either way, and the #else says so rather than dropping a caller's dials in silence --
    // the same choice waterEnabled_ makes for --water.
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
    // FIRST in the frame, so everything downstream (view matrix, gPrevViewProj reprojection,
    // shadow history) sees one consistent camera. Latched base yaw rather than accumulating onto
    // yaw_: accumulating would drift with floating-point error and never return exactly to the start.
#if AVER_MODULE_VOXI
    // THE VIEW MODE REACHES VOXI FROM onUpdate, NOT FROM THE SCENE PASS, and the difference
    // is the whole reason unlit did nothing under ray-driven. Its twin, device()->setUnlit,
    // is set and cleared around the scene draw because that flag rides per-draw state and
    // would otherwise leak into editor chrome. Voxi does not read it per draw: it composes
    // its frame constants in prePass, which the engine runs inside beginFrame -- BEFORE
    // onRender. Setting it there meant prePass always read the value the previous frame had
    // just cleared, so gViewParams.x was permanently 0 and PSRayDriven never took the branch.
    // No clear is needed here for the same reason: nothing else draws through this pass.
    voxiRenderer_.setUnlit(unlit_);
#endif
    // --cam-wobble-stop N: the wobble above runs only while the frame counter is below N, then the
    // camera simply stays where that last update left it. Measuring an artifact that appears WHILE
    // the camera moves and decays AFTER it stops needs both halves in one deterministic run: a
    // capture at frame N+k is then "k frames after motion ended", which no continuous wobble can
    // express. 0 (the default) leaves --cam-wobble exactly as it was.
    if (camWobbleDeg_ != 0.0f && camWobblePeriod_ > 0 &&
        (camWobbleStopFrame_ <= 0 || t.frame < (u64)camWobbleStopFrame_)) {
        if (!camWobbleBased_) { camWobbleBaseYaw_ = yaw_; camWobbleBased_ = true; }
        const f32 phase = 6.2831853f * (f32)(t.frame - 1) / (f32)camWobblePeriod_;
        yaw_ = camWobbleBaseYaw_ + camWobbleDeg_ * 0.01745329252f * std::sin(phase);
    }
    // --cam-translate SPEED: see setCamTranslate's own comment. One fixed step per frame along
    // whatever camForward() is THIS frame (after the wobble update above, so the two compose),
    // so occlusion relationships -- what is in front of what -- change every frame instead of
    // just the screen-space position of the same relationships a pure yaw wobble produces.
    // --cam-wobble-stop N stops this too, not just the yaw above: "the camera stopped" has to mean
    // ALL of its motion, or a capture after the stop frame is still translating and measures
    // nothing about what decays once motion ends.
    if (camTranslateSpeed_ != 0.0f &&
        (camWobbleStopFrame_ <= 0 || t.frame < (u64)camWobbleStopFrame_)) {
        camPos_ += camForward() * camTranslateSpeed_;
    }
#if AVER_MODULE_OCCLUSION && AVER_MODULE_SCENE
    // --no-occlusion-cull: see setOcclusionCullForceOff's own comment for why this cannot be a
    // one-shot CLI setter -- applyProjectVoxiSettings rewrites occlusionCullEnabled_ from the
    // manifest during project/level load, which happens after every CLI setter has already run.
    // Reasserted every frame here, which also means it wins back over a LATER --open-level or
    // project reload mid-run, not just the first one.
    if (occlusionCullForceOff_) occlusionCullEnabled_ = false;
    // --occlusion-waitidle / occlusion.debugForceWaitIdle: reasserted every frame for the same
    // reason -- and so a console `set` this frame is live on occluder_ before this SAME frame's
    // testBatch() call runs (occlusionBuildAndTest() below onUpdate returns), not one frame late.
    if (occluder_) occluder_->setDebugForceWaitIdle(editor::consoleOcclusionForceWaitIdleSlot());
    // 3B (occlusion-fix-plan.md): occlusion.showCulled / occlusion.cullUnderSuppression, read
    // every frame beside debugForceWaitIdle's own reassertion just above, for the same reason --
    // so a console `set` this frame is live for THIS SAME frame's onRender walk
    // (chooseRoute()/occlusionTestShouldRun(), aver/game/SceneSubmission.hpp) rather than one frame
    // late.
    occlusionShowCulled_ = editor::consoleOcclusionShowCulledSlot();
    occlusionCullUnderSuppression_ = editor::consoleOcclusionCullUnderSuppressionSlot();
#endif
    // Single-instance forwarding, drain. Polled rather than an Event (Event.hpp is a fixed POD
    // with no string field), latched on window_ and drained unconditionally every frame -- safe
    // since window_->pumpEvents() runs BEFORE onUpdate and WM_COPYDATA is synchronous on this thread.
    maybeAutosave(t.dt);
    maybeAutosavePrefs(t.dt);
    // Reasserted every frame, like the autosave calls just above: cheap when nothing about the
    // level's name or dirty state has changed, since it only touches the window when the built
    // string actually differs from what is already shown.
    refreshWindowTitle(e);
#if AVER_MODULE_SCENE
    // See multiStale(): anything that moved the anchor without touching the set means the
    // selection collapsed to one, and this is where that is made true rather than merely
    // believed.
    multiSyncToAnchor();
#endif
    // Project settings follow the same rule preferences do now: an edit reaches the file
    // without anybody having to find a button.
    maybeAutosaveProject(t.dt);
    // ONE LINE, ONCE, and not before frame 2. applyProject's scripting stage and
    // spawnClassPlacements both run during startup, so a census taken on the first frame would
    // report a world that is still filling -- and would then differ from the game's for a
    // reason that is about timing rather than about content.
#if AVER_MODULE_SCENE
    if (sceneCensus_ && !sceneCensusDone_ && t.frame >= 2) {
        sceneCensusDone_ = true;
        // playerStart_ is excluded: the editor synthesises that marker from the level's SPAWN
        // record and the shipped game does not, so counting it would report a divergence that is
        // really the editor doing its job. See takeSceneCensus' own comment.
        AVER_INFO("[Census] {}",
                  world::formatSceneCensus(
                      world::takeSceneCensus(scene::World::instance(), playerStart_)));
    }
#endif
#if AVER_MODULE_SCENE
    // --save-level <out>: write the OPEN level to another path and log the result.
    //
    // saveLevel had no caller but a mouse click -- Ctrl+S, the File menu and the toolbar button
    // -- so nothing could prove a round trip headlessly, which is the same gap the four project
    // flags were added to close. Writes ELSEWHERE rather than over levelPath_, so proving the
    // save never costs the content it proved on.
    //
    // IT SAVED THE WRONG LEVEL. This sat inside the one-shot block that runs when the Voxi
    // renderer finishes initialising -- a convenient "once" hook that has nothing to do with
    // saving -- which is the first frame, BEFORE --open-level has been applied. So
    // `--open-level Arena --save-level out.ocworld` faithfully wrote the project's START map
    // every time, with a log line naming the file it wrote and nothing naming the level it came
    // from. Two runs of mine went by before I noticed the placement count was wrong.
    //
    // WAITS FOR EVERY PENDING OPEN, rather than merely moving later: --open-level is consumed on
    // the first UI draw, a forwarded launch can arrive on any frame, and either can be sitting
    // behind an unsaved-changes prompt. Saving what is open the moment nothing is queued to
    // replace it is the only rule that is right for all three.
    if (!saveLevelTo_.empty() && !saveLevelDone_ && !levelPath_.empty() &&
        openLevelByName_.empty() && pendingOpenPath_.empty() && !pendingOpenPrompt_) {
        saveLevelDone_ = true;
        // The SOURCE is named too. A log line that says only where it wrote cannot tell you it
        // saved the wrong thing, which is exactly how this went unnoticed.
        if (saveLevel(saveLevelTo_))
            AVER_INFO("[Level] --save-level wrote '{}' ({}) to {}",
                      levelName_, levelPath_, saveLevelTo_);
        else
            AVER_ERROR("[Level] --save-level failed for {}", saveLevelTo_);
    }
#endif
    // GUARDED AS A WHOLE, unlike the dropped-files drain below, because nothing can ever be
    // pending here without the scene: the queue is filled only by the open-request hook, and
    // onInit installs that hook under the same condition (see setOpenRequestHook). requestOpenLevel
    // and openLevelError_ are `#if AVER_MODULE_SCENE` for the same reason they are -- a forwarded
    // path asks for a level in a world.
#if AVER_MODULE_SCENE
    if (window_ && window_->hasPendingOpenRequest()) {
        const std::string path = window_->takePendingOpenRequest();
        if (isLevelFile(path.c_str())) {
            // Through the SAME funnel as the picker and the Content Browser: the unsaved-changes
            // check and the class-placement spawn live in there, and this path used to do the
            // first and forget the second.
            if (!requestOpenLevel(path, "opened, forwarded from another launch"))
                AVER_WARN("[Sandbox] forwarded level '{}': {}", path, openLevelError_);
        }
        // A forwarded BARE PROJECT with no level attached has nothing further to do here:
        // handleOpenRequest already confirmed it matches the live project, and Window::focus()
        // already brought this window forward from the WM_COPYDATA receive itself.
    }
#endif  // AVER_MODULE_SCENE
    // Files dragged in from Explorer, latched the same way as the open request just above (see
    // Window::hasPendingDroppedFiles). Only meaningful with a project open -- importDroppedFiles
    // imports into the Content Browser's current folder, and there is no Content Browser, and
    // nowhere to copy TO, before a project exists.
    //
    // THE DESTINATION IS THE CONTENT BROWSER, and that is the guard's whole justification: both
    // importDroppedFiles and notifyOutcome are declared and defined `#if AVER_WITH_IMGUI`
    // (SandboxContentBrowser.cpp), so a tree built without the D3D12 ImGui backend has neither the
    // folder to import into nor the toast to report it with. THE QUEUE IS STILL DRAINED in both
    // arms: Window keeps accepting WM_DROPFILES regardless of what is drawn on top of it, and a
    // pending list nothing ever takes would grow for the life of the process.
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
    // --pt-scene-toggle-on/-off: verification-only (see the members' own comment). Checked BEFORE
    // syncPtSceneView() so the same onUpdate() that flips the want-flag is the same one that acts
    // on it, rather than costing a whole extra frame of lag for no reason.
    if (ptSceneToggleOnAutoFrames_  > 0 && --ptSceneToggleOnAutoFrames_  == 0) ptSceneViewWantEnabled_ = true;
    if (ptSceneToggleOffAutoFrames_ > 0 && --ptSceneToggleOffAutoFrames_ == 0) ptSceneViewWantEnabled_ = false;
    // --sun-set-at: the same write the Directional Light panel's Elevation/Azimuth sliders make.
    if (sunSetAtFrames_ > 0 && --sunSetAtFrames_ == 0) {
        sky_.setSunAngles(sunSetElevDeg_, sunSetAzimDeg_);
        AVER_INFO("[Sandbox] --sun-set-at: sun moved to elevation {:.1f} deg, azimuth {:.1f} deg",
                  sunSetElevDeg_, sunSetAzimDeg_);
    }
#if AVER_MODULE_VOXI
    // --gi-history-reset-at: the resetgihistory and resetnrdhistory console commands' own requests.
    if (giHistoryResetAtFrames_ > 0 && --giHistoryResetAtFrames_ == 0) {
        voxi::Renderer::get().requestGiHistoryReset();
        voxi::Renderer::get().requestNrdHistoryReset();
        AVER_INFO("[Sandbox] --gi-history-reset-at: GI reservoir and NRD history reset requested");
    }
#endif
    // Clears the render-scale crash cookie once this session proves the scale survivable. Thirty
    // frames, not one: device loss is noticed at Present.
    // --shader-source: pick up an HLSL edit without restarting.
    // "ONLY THE FIRST EDIT IS EVER DELIVERED" was a measurement artifact: a `--frames 900
    // --no-vsync` run lasts ~12s (measured: 900 frames, 12.0s, 75fps), so edits 2/3 of a 21-second
    // test landed after the process had exited -- the log's own "stopped after 900 frame(s)" sits
    // between append one and append two. Re-measured with `--frames 9000` and five edits nine
    // seconds apart (via Add-Content/Set-Content/Copy-Item -Force): all delivered.
    // This function only bumps an integer: rhi::reloadShaderFiles() drops the text cache and
    // increments a revision. VoxiRenderer::prePass rebuilds pipelines THERE, where that's safe --
    // doing it here would free pipelines a command list is recording against.
    // Started lazily on first update, so the common no-flag case costs nothing.
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
    // --aversr-cycle [N]: verification-only, exists because THIS TRANSITION HAD A BUG. Turning
    // AverSR on then off crashed the editor -- Off's branch reset the unique_ptr while the device
    // still held the raw pointer applyUpscalerSlot had given it, so the next composite called
    // execute() on freed memory. --aversr only ever ATTACHES, so nothing exercised this path.
    if (averSrCycleFrames_ > 0 && --averSrCycleFrames_ == 0) {
        AVER_INFO("[AverSR] --aversr-cycle: Performance -> Off, the transition that used to crash");
        applyAverSrQuality(e.device(), aver::sr::Quality::Performance);
        applyAverSrQuality(e.device(), aver::sr::Quality::Off);
        AVER_INFO("[AverSR] --aversr-cycle: survived the round trip");
    }
#endif
#if AVER_MODULE_VOXI
    // N8 FIX, PART 1: THE PT VIEW NOW FOLLOWS A PATH TRACING TIER CHANGE FROM ANYWHERE. Before
    // this, ptSceneViewWantEnabled_ was only ever flipped by four call sites this file's own
    // ptSceneViewFromCli_ comment already lists (CLI, the settings-page Quality combo, the
    // toggle-test flags, a project manifest) -- and NONE of them fired for a change made through
    // the console's voxi.pathTracing var, the new Overall Quality preset (applyOverall,
    // Scalability.hpp), or the new voxi.scalability var: all three write vx.settings().pathTracing
    // directly, so the tier changed but the want flag, and therefore PtSceneView's registration,
    // silently did not.
    {
        const voxi::Quality curPtTier = voxi::Renderer::get().settings().pathTracing;
        if (curPtTier != ptTierSeen_) {
            if (curPtTier != voxi::Quality::Off) {
                ptSceneViewWantEnabled_ = true;
                // Quality::Low is 1, so the rung is one less; mirrors buildRenderingSettings'
                // (page == 4) own live re-quality call for the case the view is already registered
                // and only its accumulator resolution needs to move.
                if (ptSceneView_) ptSceneView_->setQuality(static_cast<u32>(curPtTier) - 1);
            } else if (!ptSceneViewFromCli_) {
                // --pt-scene still wins even here: a console edit or a mid-session project open
                // that turns Path Tracing Off must not silently take away a view the command line
                // explicitly asked to keep, the same precedence applyProjectRenderSettings already
                // honours for the manifest's own RENDER.PATHTRACING.
                ptSceneViewWantEnabled_ = false;
            }
            ptTierSeen_ = curPtTier;
        }
    }
#endif
    // BEFORE device_->beginFrame() (see Engine::frameStep()) -- the only safe place to add or
    // remove a render feature. See syncPtSceneView()'s own comment for why.
    syncPtSceneView(e.device());
    // The path tracer's matched-environment legacy switch (contrast-fix plan F6/F7; root cause
    // R5) -- OUTSIDE any AVER_MODULE_VOXI guard and right beside syncPtSceneView() rather than
    // beside voxiRenderer_'s own reasserts further down, because ptSceneView_'s registration and
    // tier selection have to keep working with AVER_MODULE_VOXI off (see ptSceneViewWantEnabled_'s
    // own comment on this file), and consolePtLegacyEnvSlot() (EditorConsole.hpp) is declared
    // outside that guard for the identical reason. Reasserted every frame regardless of whether
    // the user just touched it, the same idiom as every other raw console slot in this file;
    // PtSceneView::setLegacyEnvironment only acts, and re-arms accumulation, on an actual change.
    if (ptSceneView_) ptSceneView_->setLegacyEnvironment(editor::consolePtLegacyEnvSlot());
#if AVER_MODULE_VOXI
    // --pt-quality-ramp [N]: verification-only, exists because THIS TRANSITION HAD A BUG no flag
    // could reach. Raising the PT rung on an already-rendered view re-arms PtSceneView at a new
    // resolution, and used to point an over-sized descriptor at the denoiser's still-small buffers
    // and remove the device -- reachable only by a human clicking the combo.
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
    // The RUNG, reconciled on the same cadence as the registration: --pt N, a project manifest
    // and the settings combo all write voxi::Settings, and this is the one place that want becomes
    // a call. setQuality() is idempotent, so calling it every frame costs one comparison.
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
    // THE ENGINE'S DEFAULT PAWN IS AN EXCEPTION TO "the game owns the input": leaving it out made
    // PIE feel broken -- gameHasInput() is true for any live session, so the viewport camera stood
    // down the moment Play possessed one (--pie-camera-test measured drift of exactly 0.0000,
    // i.e. "nothing ran"). The engine's stand-in is the editor camera wearing a pawn, so it's the
    // one session that must not stand the editor camera down.
    // ONE ARBITRATION, handed to every consumer: computed once here instead of eleven slightly
    // different spellings (InputOwnership.hpp).
#if AVER_WITH_IMGUI
    // THE #if IS COMPILE-TIME AND THIS QUESTION IS NOT -- the same distinction the capture block
    // below spells out. A headless run never calls uiInit() (no hwnd, so it early-returns), which
    // means CreateContext() never ran and GImGui is null: GetIO() then returns a reference off a
    // null pointer and the first member read faults. It did, at 0x00000000000000fa, which is
    // offsetof(ImGuiContext, IO) + offsetof(ImGuiIO, WantTextInput) exactly.
    //
    // ASKING uiActive() AS THE GATE rather than adding a new flag, because the answer was already
    // being computed one line down into ic.uiActive, whose own declaration in InputOwnership.hpp
    // describes the false case as "a headless/no-UI build". Leaving own_ at its default costs
    // nothing: resolveInputOwnership returns all-false for uiActive = false, which is the honest
    // answer when there is no UI and nothing can own the devices.
    if (e.device()->uiActive()) {
        const ImGuiIO& oio = ImGui::GetIO();
        editor::InputConditions ic;
        ic.uiActive          = e.device()->uiActive();
        ic.browserActive     = browserActive_;
        ic.textInput         = oio.WantTextInput;
        ic.uiWantsKeyboard   = oio.WantCaptureKeyboard;
        ic.uiWantsMouse      = oio.WantCaptureMouse;
        ic.playing           = playSessionActive();
        ic.releasedByUser    = releasedByUser_;
        // LEFT AT ITS DEFAULT WITHOUT THE SCENE, not guarded away: defaultPawnPlay_ is declared
        // `#if AVER_MODULE_SCENE` beside the Play snapshot it belongs to, and false is the honest
        // value for a tree that cannot enter Play at all -- InputConditions is a plain struct with
        // the field either way, so resolveInputOwnership still answers the same question.
#if AVER_MODULE_SCENE
        ic.defaultPawnPlay   = defaultPawnPlay_;
#endif
        ic.mouseCaptured     = mouse_.captured();
        ic.pointerInViewport = levelHovered_ && inViewport(oio.MousePos.x, oio.MousePos.y);
        ic.drawerOpen        = drawer_ != Drawer::None;
        ic.landscapeMode     = mode_ == EditorMode::Landscape;
        // --wheel-speed-test STANDS IN FOR A POINTER IN THE VIEWPORT. Headlessly there is no
        // real cursor, so ImGui reports WantCaptureKeyboard and WantCaptureMouse -- measured as
        // wantKb=1 wantMouse=1 ptrInViewport=0 -- and resolveInputOwnership correctly denies the
        // tools. That is the right answer for an unhovered window and the wrong precondition for
        // this test, which is about what happens once the gate is OPEN.
        //
        // These three values ARE the state of a person right-dragging the viewport, so asserting
        // them is standing in for the human, not weakening the test: everything downstream --
        // resolveInputOwnership itself, the flying_ block, the wheel read -- runs exactly as it
        // does in a real session. Confined to the test's own frames.
        //
        // GUARDED TO MATCH ITS DECLARATION: wheelTestForceFly_ lives in the framework block with
        // maybeWheelSpeedTest and the other Play self-tests, so -DAVER_MODULE_FRAMEWORK=OFF takes
        // the flag and the test that sets it together. Nothing else in this frame changes: the
        // three overrides only ever move off their real values while the test is running.
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
    // LEAVING FLY MODE IS UNCONDITIONAL, and must be: flying_ used to be cleared only inside the
    // narrower gate below (which splits keyboard from mouse), so a focused text field or pointer
    // off-viewport left the camera stuck in fly mode with the cursor hidden. Releasing the button
    // always means stop flying, whoever owns input.
#if AVER_WITH_IMGUI
    // uiActive() first, for the reason given above: headless has no ImGui context to ask. Nothing
    // is lost by skipping it there -- flying_ starts false and its only writer is the own_-gated
    // block below, which cannot run without a UI.
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
        // this gate -- see that clear's own comment for why a state whose only reset lives inside
        // a conditional block is the same defect as the input publisher that latched every key.
        if (ImGui::IsMouseClicked(1) && !overUI) flying_ = true;
        // --wheel-speed-test drives this directly; see maybeWheelSpeedTest for why it forces
        // the state rather than synthesising the right-drag that normally opens it. Guarded for
        // the same reason the InputConditions overrides above are -- the flag is declared with
        // the test that owns it, inside the framework block.
#if AVER_MODULE_FRAMEWORK
        if (wheelTestForceFly_) flying_ = true;
#endif
        if (flying_) ImGui::SetMouseCursor(ImGuiMouseCursor_None);

        if (flying_) {
            // input_.mouseDX/DY AND NOT io.MouseDelta, FOR THE REASON THE WHEEL BLOCK BELOW
            // ALREADY SPELLS OUT AT LENGTH -- this is the same phase bug, and only the wheel
            // half of it was ever fixed.
            //
            // ImGui computes io.MouseDelta inside NewFrame (UpdateMouseInputs), and
            // Engine::frameStep runs onUpdate BEFORE uiNewFrame. So every read of io.MouseDelta
            // from this function sees the delta computed by the PREVIOUS frame's NewFrame: the
            // camera turns by where the mouse was a frame ago, on every frame, by construction.
            // That is not a stutter and not a GPU cost -- it is a fixed one-frame lag between
            // the hand and the picture, which is what "it lags behind my inputs" describes.
            //
            // InputState is fed by pumpEvents (before frameStep) and rolled at the END of
            // onRender, so during onUpdate it holds THIS frame's accumulated motion. It is the
            // correctly-phased source for anything in this function.
            yaw_   += static_cast<f32>(input_.mouseDX()) * lookSpeed_;
            pitch_ -= static_cast<f32>(input_.mouseDY()) * lookSpeed_;
            pitch_ = pitch_ < -1.54f ? -1.54f : (pitch_ > 1.54f ? 1.54f : pitch_);
            // WHEEL WHILE FLYING CHANGES SPEED, and SCROLL UP MAKES IT FASTER -- Unreal's own
            // direction, and the one that survived contact with a hand. It shipped inverted
            // first because that was asked for; using it settled the question the other way.
            // Which way a wheel means "more" is a preference, not a fact, so it is written
            // down here rather than argued about: up is faster.
            //
            // MULTIPLICATIVE, so a notch feels the same at 1 as it does at 20: an additive step
            // would be imperceptible when flying fast and violent when creeping. 1.25 per notch
            // is roughly three notches to double, which is coarse enough to be useful in one
            // flick and fine enough to settle on a speed.
            //
            // input_.wheel() AND NOT io.MouseWheel, WHICH IS ALWAYS ZERO HERE. This is the bug
            // that made the whole control dead, and it was dead long before the direction was
            // ever changed -- the previous `flySpeed_ *= (1.0f + io.MouseWheel * 0.15f)` sat on
            // this exact line and could not fire either.
            //
            // WHY: Engine::frameStep runs onUpdate BEFORE uiNewFrame (Engine.cpp:208-212), and
            // ImGui::EndFrame zeroes io.MouseWheel at the tail of the PREVIOUS frame
            // (imgui.cpp:6420). NewFrame is the only thing that merges queued wheel events back
            // in, and it has not run yet. So every read of io.MouseWheel from onUpdate observes
            // the previous frame's reset -- not intermittently, but on every frame by
            // construction. GraphEditor's and AssetEditor's wheel zoom work because they are
            // drawn from onRender, which is after NewFrame.
            //
            // InputState is fed by pumpEvents, which runs before frameStep, and is rolled at the
            // END of onRender (see input_.newFrame()) -- so during onUpdate it holds exactly this
            // frame's accumulated notches. It is the correctly-phased source for anything in
            // this function, and io.MouseWheel is the wrong one no matter how it is spelled.
            const f32 wheel = input_.wheel();
            if (wheel != 0.0f) {
                flySpeed_ *= std::pow(1.25f, wheel);
                flySpeed_ = flySpeed_ < 20.0f ? 20.0f : (flySpeed_ > 40000.0f ? 40000.0f : flySpeed_);
            }
        }

        const Vec3 fwd = camForward();
        const Vec3 up{0, 0, 1};
        const Vec3 right = cross(up, fwd).getSafeNormal();

        // SPECTATOR PLAY FLIES WITHOUT HOLDING THE RIGHT BUTTON: pressing Play with no GameMode
        // hands you a plain camera, and having to hold a mouse button to walk it isn't what anyone
        // means by that; mouse look still wants the button.
        // A synthetic look enters here, where the real one does: --pie-camera-test used to bump
        // yaw_/pitch_ from its own LATER tick, so the bump never carried into the pawn -- and
        // drivePlayCamera restored it from the pawn's unchanged forward next frame, a "steady
        // camera" that was actually just bad ordering, not ignored input.
        // All four pieCam members are declared inside the framework block with maybePieCameraTest,
        // which is honest -- there is no Play-in-Editor camera to test without a GameMode to enter
        // Play through -- so the injection point is guarded to match rather than the members moved.
#if AVER_MODULE_FRAMEWORK
        if (pieCamPendingLook_) {
            pieCamPendingLook_ = false;
            yaw_   += 0.5f;
            pitch_ += 0.3f;
            pieCamWantYaw_ = yaw_; pieCamWantPitch_ = pitch_;
        }
#endif
        // defaultPawnPlay_ is scene-guarded (see the InputConditions fill above). Without the
        // module the editor camera flies on the right button alone, which is what it did before
        // spectator Play existed and the only behaviour a scene-less tree can offer.
#if AVER_MODULE_SCENE
        if ((flying_ || defaultPawnPlay_) && !io.WantCaptureKeyboard) {
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
            // MOVE THE PAWN, NOT THE CAMERA, while the engine's default pawn is possessed:
            // drivePlayCamera() rewrites the view from the pawn every frame, so a camPos_ nudge
            // here would be overwritten before it was ever seen. Driving the pawn is what makes this a PAWN.
            if (defaultPawnPlay_) {
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
            // THE SAME DEFECT, in the scroll-to-dolly this branch exists for: it read
            // io.MouseWheel from onUpdate, so it has never moved the camera either. Found by
            // following the fly-speed bug rather than by anybody reporting it, which is what a
            // control with no headless witness looks like when it breaks. See the long note on
            // input_.wheel() in the flying_ block above for why the phase is wrong.
            const f32 wheel = input_.wheel();
            if (wheel != 0.0f) camPos_ += fwd * wheel * (flySpeed_ * 0.15f);
            // Same phase argument as the look block above: MMB pan read a frame-old delta too.
            if (io.MouseDown[2]) {
                camPos_ -= right * static_cast<f32>(input_.mouseDX()) * 0.02f;
                camPos_ += up    * static_cast<f32>(input_.mouseDY()) * 0.02f;
            }
        }
        // CTRL+S SAVES THE LEVEL, which the File menu has claimed it does for as long as that
        // menu has existed -- the "Ctrl+S" beside Save Level is the shortcut-LABEL parameter of
        // ImGui::MenuItem, which draws text and wires nothing. The only ImGuiKey_S in this file
        // was the camera's strafe-left, so the shortcut a person reaches for by reflex while
        // building a level did nothing at all, and there was no feedback to say so.
        //
        // NOT GATED ON levelFocused_, unlike F below: saving is not a viewport gesture and
        // wanting it while the cursor sits over the Outliner is not a mistake. It IS gated on
        // WantTextInput, or renaming an entity would save the level on the "s" of a name.
        if (!io.WantTextInput && keybinds_.pressed(editor::CommandId::LevelSave, io))
            saveLevelInteractive();

        // CTRL+SHIFT+S SAVES EVERYTHING: the level plus every dirty asset tab (saveAll(),
        // SandboxShell.cpp). SAME SITE AS LEVELSAVE ABOVE, deliberately -- pressed() does not
        // consult the live scope mask (EditorKeybinds.cpp), so this is already live from inside an
        // asset editor tab exactly as LevelSave is from the Outliner, with no separate focus gate
        // to add for it.
        if (!io.WantTextInput && keybinds_.pressed(editor::CommandId::SaveAll, io))
            saveAll();

        // CTRL+SHIFT+B / CTRL+SHIFT+R mirror the Tools menu's Compile/Reload Scripts items. Same
        // site and same reasoning as SaveAll just above: these are not a viewport gesture either.
        if (!io.WantTextInput && keybinds_.pressed(editor::CommandId::CompileScripts, io))
            tools_.requestCompileScripts(project_);
        if (!io.WantTextInput && keybinds_.pressed(editor::CommandId::ReloadScripts, io))
            tools_.requestReloadScripts(project_);

        // F frames the selection at a distance derived from its radius.
        //
        // THE OUTLINER AND DETAILS COUNT, for the reason the edit verbs below do: levelFocused_ is
        // true only while the 3D VIEWPORT holds ImGui's keyboard focus, and clicking a row in the
        // World Outliner -- the ordinary way to pick the thing you then want to look at -- moves
        // focus to that panel. Select in the list, press F, nothing happens. Reported as "press F
        // to focus is broken", and it is the identical gate that had Delete and Undo silently
        // doing nothing from the same panel.
        //
        // SCENE-GUARDED AS A WHOLE: what F frames is a SELECTED ENTITY's bounds, and
        // selectionBounds is declared `#if AVER_MODULE_SCENE` because it walks the selection set
        // through the world. anySelected() is not guarded -- the sun, sky and post rows are
        // selectable without a scene -- but none of those has bounds to frame a camera on, so the
        // binding does nothing in that tree rather than framing a point at the origin.
#if AVER_MODULE_SCENE
        if ((levelFocused_ || outlinerFocused_ || detailsFocused_) && !io.WantCaptureKeyboard &&
            keybinds_.pressed(editor::CommandId::ViewFrameSelected, io) && anySelected()) {
            // selectionBounds, NOT selectedXform/selectedRadius: those describe only the selection's
            // ANCHOR, which is what the gizmo draws on, so a spread multi-selection framed around
            // them put most of the set outside the view. This is the union of the whole selection.
            Vec3 center; f32 r;
            if (selectionBounds(center, r)) {
                const f32 d = std::fmax(50.0f, r / std::tan(radians(30.0f)) * 1.6f);
                camPos_ = center - fwd * d;
                flySpeed_ = std::fmax(flySpeed_, r * 0.4f);
                // streaming_ carried its own `#if AVER_MODULE_SCENE` here, which the guard now
                // opened above makes a repeat of itself -- one condition, stated once.
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
    // --recapture-test OPTS IN, and it has to: the gesture it exercises lives entirely inside
    // this block, so a bounded run that skips it cannot reach the bug. Safe now that mouse
    // re-centring refuses to move a background window's cursor.
    const bool interactive = (maxFrames_ == 0 && !playTest_) || recapFrames_ > 0;
    // THE #if IS NOT REDUNDANT WITH uiActive(): that's a RUNTIME question that doesn't help the
    // ImGuiIO/ImGui:: names below COMPILE on a UI-less build (module-matrix.ps1's no-ui and
    // d3d12-off rows both failed here). Guarding the whole block keeps the capture policy in one piece.
#if AVER_WITH_IMGUI
    if (e.device()->uiActive() && interactive) {
        // Clicking the viewport puts the mouse back in the game. Tested before wantCapture below.
        {
            // levelHovered_, NOT !WantCaptureMouse -- the old test could never pass, since the 3D
            // view is an ImGui dock window ImGui always wants the mouse over (measured
            // mid-gesture: wantMouse=1 always). levelHovered_ asks the question actually meant: is
            // the pointer over the LEVEL window, the same `overUI` idiom the fly camera uses.
            const ImGuiIO& mio = ImGui::GetIO();
            if (playSessionActive() && releasedByUser_ && ImGui::IsMouseClicked(0) &&
                levelHovered_ && inViewport(mio.MousePos.x, mio.MousePos.y)) {
                releasedByUser_ = false;
                // AND THAT CLICK IS NOT A TRIGGER PULL: it means "give the game the mouse back",
                // and publishing it as MOUSE_LEFT would keep firing for as long as the button is
                // held (a level-triggered fire gate behind a cooldown can't tell one long click
                // from many). Eaten until the button comes up -- see pushInput, where the latch is consumed.
                eatRecaptureClick_ = true;
            }
        }
        const bool wantCapture = playSessionActive() && !releasedByUser_;
        // ALT+P STARTS PLAY, the same anyPlayActive() precondition the toolbar's own Play button
        // disables itself on (SandboxShell.cpp) -- the chord cannot layer a second session onto one
        // already running any more than the button can.
        if (!ImGui::GetIO().WantTextInput && !anyPlayActive() &&
            keybinds_.pressed(editor::CommandId::PlayStart, ImGui::GetIO()))
            startPlay();
        if (keybinds_.pressed(editor::CommandId::PlayReleaseMouse, ImGui::GetIO()) && playSessionActive())
            releasedByUser_ = !releasedByUser_;
        // ESCAPE STOPS PLAY-IN-EDITOR, the same as clicking Stop. Checked here rather than in
        // pushInput: this must win over the game seeing the keypress, so a script reading Escape
        // for its own pause menu doesn't race the editor for what the key means.
        // Escape ends a drone stand-in too: Play started it, so Play's exit has to end it.
        if (keybinds_.pressed(editor::CommandId::PlayStop, ImGui::GetIO()) && (playSessionActive() || dronePlayActive() || spectatorPlayActive()))
            stopPlay();
        // F9 SCREENSHOTS THE VIEWPORT, IN EDIT MODE OR DURING PLAY. Checked HERE rather than beside
        // LevelSave above: this whole block runs whenever the UI is interactive, in edit mode AND
        // through a play session alike, so one check covers both -- adding a second at LevelSave's
        // site (which also runs in edit mode) would fire requestViewportScreenshot() twice on the
        // same frame there.
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
    // The tick groups bracket the physics step: PrePhysics -> Physics -> PostPhysics.
    if (!spawnTestClass_.empty() || aver_fw_play_state() == AVER_FW_PLAY_PLAYING) {
        game::tickGameplayGroups(t.dt);
    }
#endif
#if AVER_MODULE_FLUIDS
    // After the physics step above, before any prePass -- not gated on Play.
    water_.update(*e.device(), t.dt);
#endif
#if AVER_MODULE_SCENE
    // Retires deferred destroys and propagates world matrices once, after gameplay and before onRender.
    // The animation clock runs UNCONDITIONALLY, not from the gameplay tick above: those tick groups
    // gate on PLAYING, and hanging this off them would freeze every preview outside Play mode.
    anim::animSystem().tick(scene::World::instance(), t.dt);
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // Same "unconditionally, not from the gameplay tick" reasoning as the animation clock above:
    // a preview outside Play mode should still show its effects playing.
    // VERIFICATION-ONLY: the steady_clock pair brackets ONLY the CPU simulation call, isolated
    // from the GPU draw the --frame-time report already accounts for separately.
    {
        const auto tickStart = std::chrono::steady_clock::now();
        particles::particleSystem().tick(scene::World::instance(), t.dt);
        particleTickAccumSec_ += std::chrono::duration<f64>(std::chrono::steady_clock::now() - tickStart).count();
        ++particleTickFrames_;
    }
#endif
    // AFTER the tick and BEFORE anything draws: update() is what creates the per-entity skin
    // targets the draw pass is about to ask for, and what copies this frame's matrices out of
    // the AnimSystem -- whose skinning() is only valid until the next tick.
    if (skinnedScene_)
        skinnedScene_->update(scene::World::instance(), anim::animSystem(), *e.device());
#if AVER_MODULE_RENDER_SOFTBODY
    // AFTER the physics step and World::flush, BEFORE the draw -- the draw asks drawHandle() for
    // a handle that has to exist by then. Same ordering skinnedScene_ above requires, and for
    // the same reason.
    if (softBodyScene_) softBodyScene_->update(scene::World::instance(), *e.device());
#endif
    // ONCE per frame, BEFORE the render features run (see ThumbnailCache.hpp): update() points
    // its preview at the next pending request, so the copy feature it registered has something
    // fresh to copy by the time features draw this frame. Guarded on ready(), matching every other
    // conditionally-initialised helper above.
    if (thumbnails_.ready()) thumbnails_.update();
    // --drone: switches the graph-driven drone on N frames in, on its own, mirroring
    // --chunk-stream immediately below so a --frames capture run can prove it without a human
    // clicking Window > Drone.
    if (droneAutoFrames_ > 0 && --droneAutoFrames_ == 0) setDroneEnabled(true);
    // --undo-test: fires runUndoTest() N frames in, then exits -- see its own comment for what it
    // proves and why. runUndoTest() lives inside the same `#if AVER_WITH_IMGUI` block as the
    // commands it proves, so this call site needs the same guard.
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
    // Ticks the graph-driven drone, if one is live. Runs BEFORE chunk streaming below so this
    // frame's drone position/velocity are what chunk streaming's extra StreamSource (and the log
    // line right under it) see -- not last frame's.
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
    // GRAPH-AS-CLASS instances -- GATED ON PLAY. The old defence rested on a false premise ("a
    // graph-only project never calls aver_fw_begin_play"). PTTest disproves it every Play:
    //     [Graph] GameMode 'AN_FPRules' begins play with pawn='AN_FPCharacter' ...
    //     [Sandbox] Play: begin_play GameMode='AN_FPRules'
    // HostBridge registers graph classes into the SAME native class registry a C# GameMode uses,
    // so a graph-only project reaches Play like any other.
    // What ungated did: every class-placed graph ran OnTick every frame while someone just looked
    // around. Measured on PTTest over 1000 frames, Play never pressed: AN_FPRules' `elapsed` VAR
    // climbed from 3.6e-05 to 12.31s, 4003 tick lines written, no begin_play -- a graph is free to
    // move entities, fire events and write VARs, so browsing a level mutated it.
    // Same condition as the framework tick groups above, deliberately: one spelling, not two.
    //
    // WHICH IS ALSO WHY IT NEEDS THE FRAMEWORK GUARD the tick groups already have: the condition
    // is spelled with aver_fw_play_state(), out of framework_abi.h, and this block sat under SCENE
    // and SCRIPTING alone. There is nothing to relax here -- a graph CLASS is placed by a
    // GameMode's world and ticked because Play began, so with no framework there is no Play to
    // gate on and no class instances to tick.
#if AVER_MODULE_FRAMEWORK
    if (!spawnTestClass_.empty() || aver_fw_play_state() == AVER_FW_PLAY_PLAYING)
        scripts_.tickGraphClassInstances(t.dt);
#endif
#endif
    // --chunk-stream: switches streaming on N frames in, on its own, so a --frames capture run
    // can prove it happened without a human clicking Window > Chunk Streaming.
    if (chunkStreamAutoFrames_ > 0 && --chunkStreamAutoFrames_ == 0) setChunkStreamingEnabled(true);
    // Chunk streaming, if switched on. Runs here so it sees THIS frame's camPos_ (the WASD/fly
    // block above already finalized it) and its evictions land in the flush() right below -- also
    // runs while just idling in the editor outside Play, deliberately: that's exactly who this feature is for.
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
    scene::World::instance().flush();
#if AVER_WITH_AUDIO_ABI
    // Reclaim finished voices, every frame, Play or not -- the second half of the audio-device
    // gap: aver_audio_collect had the same single caller as aver_audio_init, so a graph-started
    // voice was never reclaimed and its slot leaked until the mixer ran out. Not gated on Play:
    // voices outlive a session, so gating this would leak voices that finish after Stop.
    aver_audio_collect();
#endif
#if AVER_MODULE_SYNAPSE_SCENE && AVER_MODULE_FRAMEWORK
    // GATED ON PLAY, matching the physics/fw_tick block above -- pathing and movement targets are
    // gameplay, not an authoring-time preview (an AI agent chasing a goal has nothing meaningful to
    // do while nothing else in the level is moving). Same condition as that block, re-evaluated
    // here rather than threaded through as a local.
    // nav_ may be empty or a frame stale (loadNavForLevel/navBakeCheck poll from onRender, not
    // here) -- AgentSystem::tick treats that as "wait for a grid", not an error.
    if (!spawnTestClass_.empty() || aver_fw_play_state() == AVER_FW_PLAY_PLAYING) {
        game::tickAi(t.dt, &nav_);
    }
#endif
#endif
#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
    drivePlayCamera();
#endif
    // G-buffer: pushed every frame so a live dropdown click or --gbuffer-debug takes effect
    // immediately. OR'd together: --gbuffer alone must still write with no view selected, and a
    // debug view alone must still turn the G-buffer on. No module gate -- default stays OFF for
    // the render-gate oracle and the 89 headless suites.
    // VOXI'S DENOISER IS THE THIRD REASON THE G-BUFFER EXISTS, alongside --gbuffer and the
    // debug views. NRD is its only consumer in the engine and it reads these three targets every
    // frame it runs, so the setting that turns it on has to turn them on too -- otherwise the
    // checkbox in Project Settings silently does nothing, which is how this pass spent its whole
    // life reachable only from a command line.
#if AVER_MODULE_VOXI
    // N3 fix: the raw `denoiser` checkbox no longer gates this alone -- a ticked denoiser that can
    // never actually run (no NRD on this device, RT tier Off, nothing producing a signal to
    // filter) must not still cost the ~54 MB G-buffer allocation every frame. See
    // Resolution::denoiserGBufferWanted (RenderSettingsResolver.hpp) for the exact rule: wanted
    // when denoiser is requested and nothing but the soft MSAA reason (if even that) stands
    // between the request and NRD actually running.
    voxi::Renderer& vx = voxi::Renderer::get();
    const bool wantGbufForDenoiser = voxi::resolve(vx.settings(), vx.deviceInfo()).denoiserGBufferWanted;
#else
    const bool wantGbufForDenoiser = false;
#endif
    e.device()->setGBufferEnabled(gbufferOverride_ || wantGbufForDenoiser ||
                                  gbufferDebugView_ != GBufferDebugFeature::Mode::Off);
    gbufferDebugFeature_.setDevice(e.device());
    // vpX_/vpY_/vpW_/vpH_ are this frame's 3D-viewport rect (see buildViewportOverlay), a frame
    // stale at worst on the very first draw -- the identical tolerance captureCheck()'s own
    // VIEWPORT-MOVED check already accepts for the pixel probe this view exists to feed.
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
        // A COPY, and that is load-bearing: the controller must never write back into the
        // singleton, or one throttled frame would become the new authored baseline and the
        // quality could only ever ratchet down. Project Settings still shows what was authored.
        voxi::Settings vs = voxi::Renderer::get().settings();
        frameBudgetTick(t.dt, vs);
        const f32 c[3] = {giCenter_.x, giCenter_.y, giCenter_.z};
        voxiRenderer_.setSettings(vs);
        // Consume-and-forward for the five reset* console commands (EditorConsole.hpp) -- one
        // request flag per history, raised on voxi::Renderer (the settings singleton the console
        // can reach) and consumed here, right beside setSettings, into the actual VoxiRenderer
        // instance the console cannot reach directly. Same "raise on the singleton, consume once
        // a frame" shape as consumeMsaaDirty() just above onUpdate's own Voxi block.
        if (voxi::Renderer::get().consumeGiHistoryResetRequest())  voxiRenderer_.resetGiHistory();
        if (voxi::Renderer::get().consumeRtHistoryResetRequest())  voxiRenderer_.resetRtHistory();
        if (voxi::Renderer::get().consumeAoHistoryResetRequest())  voxiRenderer_.resetAoHistory();
        if (voxi::Renderer::get().consumeNrdHistoryResetRequest()) voxiRenderer_.resetNrdHistory();
        // voxi.debugResetHistoryEveryFrame: the same resets, every frame, quietly -- see
        // editor::consoleResetHistoryEveryFrameSlot()'s own comment.
        if (const u32 everyFrame = editor::consoleResetHistoryEveryFrameSlot()) {
            if (everyFrame & 1u) voxiRenderer_.resetGiHistory(/*quiet=*/true);
            if (everyFrame & 2u) voxiRenderer_.resetRtHistory(/*quiet=*/true);
            if (everyFrame & 4u) voxiRenderer_.resetNrdHistory(/*quiet=*/true);
        }
        voxiRenderer_.setVolume(c, giExtent_);
        // WHERE A BAKED VOLUME MAY BE REMEMBERED. Pushed every frame like everything else here,
        // and empty with no project open -- which disables the cache rather than scattering
        // derived data beside the executable. See VoxiRenderer::setGiCacheDir.
        voxiRenderer_.setGiCacheDir(project_.valid() ? fmt::giCacheDir(project_.dir) : std::string());
        voxiRenderer_.setDebugView(giDebugView_);
        // voxi.giPoisonView: EditorConsole.hpp's own live source of truth, reasserted every frame
        // the same way occlusion.debugForceWaitIdle already is -- see
        // editor::consoleGiPoisonViewSlot()'s own comment.
        voxiRenderer_.setGiPoisonView(editor::consoleGiPoisonViewSlot());
        // voxi.nrdLegacyCamera: identical idiom, right beside the toggle it mirrors -- see
        // editor::consoleNrdLegacyCameraSlot()'s own comment and VoxiRenderer::setNrdLegacyCamera's
        // for what this reasserts and why the setter itself only acts on an actual change.
        voxiRenderer_.setNrdLegacyCamera(editor::consoleNrdLegacyCameraSlot());
        // The lighting-contrast fix's legacy bitmask: identical idiom, right beside the toggle it
        // mirrors -- see editor::consoleLightingLegacySlot()'s own comment and
        // VoxiRenderer::setLightingLegacyBits' for the bit table and what reasserting this every
        // frame (regardless of whether the user just touched it) costs versus what it buys.
        voxiRenderer_.setLightingLegacyBits(editor::consoleLightingLegacySlot());
        // engine-optimisation-plan wave 1 (M1-M4/W3/W12, C-2/C-5): identical idiom, right beside
        // the toggles it mirrors just above -- see consoleGiForceRebuildSlot()'s own comment
        // (EditorConsole.hpp) for why these three are raw slots rather than ordinary dials. Each
        // setter acts only on an actual change and logs only on change (C-2's own contract), so
        // reasserting all three unconditionally every frame costs nothing when nobody has touched
        // the console since the last frame.
        voxiRenderer_.setGiForceRebuild(editor::consoleGiForceRebuildSlot());
        voxiRenderer_.setGiBoundedDispatch(editor::consoleGiBoundedDispatchSlot());
        voxiRenderer_.setGiFreeAccumulator(editor::consoleGiFreeAccumulatorSlot());
        // optimisation-wave-2, U1/W6/M5: identical idiom, right beside the toggles it mirrors just
        // above -- see consoleGiVisPathViewSlot()'s and consoleBlendedGiConeSlot()'s own comments
        // (EditorConsole.hpp) for why these two are raw slots rather than ordinary dials. Both
        // setters act only on an actual change, so reasserting them unconditionally every frame
        // costs nothing when nobody has touched the console since the last frame.
        voxiRenderer_.setGiVisPathView(editor::consoleGiVisPathViewSlot());
        voxiRenderer_.setBlendedGiCone(editor::consoleBlendedGiConeSlot());
        // --no-gi-cone: see setGiConeTraceOff's own comment. Applied every frame, same as
        // setDebugView beside it, so the toggle takes effect the instant the flag is set rather
        // than only at attach time.
        voxiRenderer_.setConeTraceEnabled(!giConeTraceOff_);
#if AVER_MODULE_SR
        // optimisation-wave-2, U2 (3.3 A): resolves and applies AverSR's level fresh every frame
        // from the SAME precedence chain the Project Settings upscaling line reads (CLI >
        // --render-scale > the user's Display choice > the project manifest > the Overall rung's
        // own ladder default). Auto is not a one-shot decision made at project-open time, because
        // the rung it follows can change under it (a scalability button, a manifest reload) --
        // outside beginFrame/endFrame, like every other Voxi reassert in this block.
        updateAverSrAuto(e);
#endif
        const Vec3 sd = Vec3{sky_.sunDirection[0], sky_.sunDirection[1],
                             sky_.sunDirection[2]}.getSafeNormal();
        if (sunAngle_ > 0.0f) sky_.sunAngularDiameterDeg = sunAngle_;
        // Direction only. sunColor_ and sunAmbient_ reach the shaders through sky_ below
        // (:1136, :1141) and the device's frame constants -- passing them here as well was
        // storing a second copy nothing read.
        voxiRenderer_.setSunDirection(&sd.x);
    }
#endif
    // Confine the scene to the dockspace's central node (latched by buildUI last frame).
    e.device()->setViewportRect((u32)vpX_, (u32)vpY_, (u32)std::fmax(1.0f, vpW_), (u32)std::fmax(1.0f, vpH_));

    const Vec3 fwd = camForward();
    const f32 aspect = viewAspect();
    const game::CameraMatrices cam = game::pushCamera(*e.device(), camPos_, fwd, aspect);
    invVP_ = cam.invVP; viewProj_ = cam.viewProj; eye_ = camPos_;

    // ONE MEMBER FOR ONE VALUE. This used to read `fog = fogDensity_` and then, on a level
    // with a FOG record, overwrite it with a SECOND member `levelFog_` that saveLevel wrote
    // and the slider never touched. So the Fog Density slider was dead in both directions:
    // inert when the level had fog (levelFog_ won), and unsaved when it did not
    // (hasLevelFog_ was false). The load now writes the slider's own member and the save
    // reads it, so what is on screen is what is in the file.
    f32 fog = fogDensity_;
#if AVER_MODULE_SCENE
    // OPT-IN, and overrides whatever the level/slider said while on: match fog density to the
    // streaming load boundary instead. See fogDensityForOpacityAt and matchFogToStreamRadius_'s
    // comment -- this makes the world visibly foggier, on purpose, only when asked for.
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
    // --sky-light N OUTRANKS THE LEVEL, and is applied HERE rather than once at startup because
    // sunAmbient_ is overwritten by applyLevelSky every time a level opens -- a value set before
    // that would be silently discarded, which is the precedence bug this file has already
    // recorded four times over (see the --gi-update-interval note in the flag-override block).
    //
    // WHY THE KNOB EXISTS AT ALL: skyLightIntensity scales the ONLY term in the frame that is
    // added without being occluded by anything but a six-cone AO -- diffAmbient in
    // material_prelude.hlsl -- and it had a slider in the Rendering panel and no command-line
    // twin, so its share of the image could never be swept or measured. That share is the open
    // question behind "the colours look washed out": the sky is Rayleigh-blue and the bounce off
    // stone is warm, so a large unoccluded ambient dilutes chroma toward grey.
    if (skyLightOverride_ >= 0.0f) sunAmbient_ = skyLightOverride_;
    // --sky-physical / --sky-authored / the sun elevation, re-applied HERE for the same reason
    // --sky-light is: applyLevelSky overwrites sky_ when a level opens, so anything written at
    // startup is gone by the first frame. See setSkyPhysical for what that cost.
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
    // UNDERWATER, APPLIED TO A COPY: sky_ is the AUTHORED sky and must stay that way, or folding
    // the override back in would accumulate every frame spent below the surface, and a level saved
    // from that state would carry underwater fog as its authored weather.
#if AVER_MODULE_FLUIDS
    e.device()->setSkyAtmosphere(water_.applyUnderwaterFog(sky_, camPos_.z));
#else
    e.device()->setSkyAtmosphere(sky_);
#endif
    // Outside the viewport rect is editor chrome, not sky.
    e.device()->setClearColor(0.055f, 0.055f, 0.062f, 1);
    e.device()->setPostProcess(post_);
}

// Tears the editor down: MCP, prefs, physics, UI textures, materials, render features, scripts.
// The process exit code. Non-zero when a test mode that was ASKED FOR did not pass.
// Only modes the command line requested are judged: an ordinary session must exit 0, a test never
// started is not a failure, but a requested test that never finished IS one -- silence would read as success.
int SandboxApp::exitCode() const  {
    if (skinScene_) {
        if (!skinScene_->finished()) {
            AVER_ERROR("[Skin] --skin-scene-test did not finish; reporting failure rather than "
                       "letting an unfinished run look like a pass");
            return 2;
        }
        if (!skinScene_->passed()) return 1;
    }
    // THE OTHER TWO SKIN TESTS, WHICH USED TO EXIT 0 WHATEVER THEY FOUND. Read from the latched
    // scalars rather than the objects: onShutdown runs BEFORE this and destroys both, so asking
    // skinDraw_/skinSelfTest_ here would always see null and always report success -- and a
    // falsification written against that would pass while proving nothing.
    // -1 means the test was never asked for, which is not a verdict and must not become one.
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
    // THE BAKED GI GOES TO DISK HERE. Volumes are buffered in RAM while the editor runs and
    // only reach the filesystem when the budget is exceeded or right now -- so without this
    // line a whole session's lighting work would be thrown away and rebuilt on the next open.
    // BEFORE the render features are torn down below: giCacheFlush only touches std::vectors
    // and the filesystem, but it belongs with the state capture rather than after the device
    // has started coming apart.
    if (const u32 wrote = voxiRenderer_.giCacheFlush())
        AVER_INFO("[Editor] wrote {} buffered GI cache entr(ies) on shutdown", wrote);
#endif
    // ---- CAPTURE LIVE STATE BEFORE FLUSHING IT ----
    // flushEditorPrefs() writes the pref STORE but does not look at the editor, so it only
    // persists what saveEditorPreferences() already pushed in -- and that only runs from
    // buildEditorPrefs()'s tail, which early-returns when the Preferences window is closed.
    // Several settings change the member through a more convenient control that never reaches
    // that function (mouse wheel -> flySpeed_, toolbar -> wireframe_, Content Browser Tiles/List
    // and zoom, drawer grip -> drawerFrac_): change any the natural way, close the editor, and the
    // value was silently gone.
    // Calling the sync here reads the live members regardless of which UI last touched them --
    // safe unconditionally since setPref*/flushEditorPrefs() are no-ops on nothing dirty.
    //
    // "UNCONDITIONALLY" IS ABOUT THE DIRTY CHECK, not about the build: saveEditorPreferences is
    // the Preferences window's own push and is declared and defined `#if AVER_WITH_IMGUI`
    // (SandboxSettings.cpp), so it does not exist in a tree built without the D3D12 ImGui backend.
    // flushEditorPrefs STAYS OUTSIDE the guard -- it is editor::, not a panel, and a pref set from
    // the command line still deserves to reach disk in a build with no window to change it from.
#if AVER_WITH_IMGUI
    saveEditorPreferences();
#endif
    editor::flushEditorPrefs();
    // THE MANIFEST TOO, for the same reason and one the preferences do not have: the project
    // autosave is a 0.5s DEBOUNCE (kProjectAutosaveSec), so an edit made and immediately
    // followed by File > Exit or the window's X is still sitting in projectDirty_ when the
    // process goes. Neither exit path checked it -- requestExitChecked and onCloseGuard both
    // test only assetEditors_.anyDirty() || levelHasUnsavedEdits() -- so the edit was lost with
    // no prompt and no log line.
    //
    // FLUSHED, NOT PROMPTED. A prompt would contradict the design stated at projectDirty_'s
    // declaration: settings save on edit precisely so a page cannot be "edited and closed, and
    // the edit gone". Writing here closes the window for EVERY exit path at once.
    //
    // THE maxFrames_ GUARD IS LOAD-BEARING, not tidiness -- maybeAutosaveProject documents why:
    // --frames sets render settings from the command line and applyProjectRenderSettings marks
    // the project dirty when it does, so flushing here unguarded would write a capture run's
    // CLI flags into the user's manifest as if they had chosen them.
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
    // BEFORE aver_phys_shutdown below, explicitly rather than leaving it to water_'s own
    // destructor: that runs after onShutdown returns, when the solver is gone and retiring a live
    // volume would call into a shut-down physics system.
    water_.shutdown(e.device());
#endif
#if AVER_FLUIDS_SIMULATED
    if (viewportIconsReady_) {
        e.device()->removeRenderFeature(&viewportIcons_);
        viewportIcons_.shutdown();
        viewportIconsReady_ = false;
    }
#endif
    // THE SAME REASON, THE SAME FIX, one member along -- see the water_ note above:
    // GBufferDebugFeature's destructor calls releaseGpu() after onShutdown returns, by which point
    // the device and its resource factory are gone, so `res_` dangles and the process dies on the
    // way out.
    // MEASURED, NOT FEARED: `--gbuffer-debug velocity --msaa 1` exited 0xC0000005 with 0 debug-
    // layer errors -- a clean frame followed by a CPU access violation at teardown.
    e.device()->removeRenderFeature(&gbufferDebugFeature_);
    gbufferDebugFeature_.shutdown();
#if AVER_MODULE_PHYSICS
    aver_phys_shutdown();
#endif
#if AVER_WITH_AUDIO_ABI
    // Stops the mixer, releases the device and forgets every loaded sound. Idempotent, and a
    // no-op when the device was never opened -- so a build with no output device, or one that
    // never reached the init above, is unaffected.
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
    // DETACH FROM THE DEVICE FIRST, THEN DESTROY. The old comment said resetting the unique_ptrs
    // was safe because "e.device() is still known good" -- the wrong question: it's
    // `dev->upscaler()`, a RAW pointer set by applyUpscalerSlot(), that must stop pointing here
    // first. --edge-aa's first --frames run crashed (SIGSEGV) AT PROCESS EXIT with the measurement
    // already logged -- only teardown order was wrong. Clearing the slot unconditionally, for
    // BOTH, before either reset() runs, is the exact bug clearAverSrUpscaler(dev) exists to
    // prevent and was never called anywhere in this file.
    e.device()->setUpscaler(nullptr);
    averSrUpscaler_.reset();
    edgeAaUpscaler_.reset();
#endif
    if (gameUi_) {
        e.device()->removeRenderFeature(gameUi_);
        delete gameUi_;
        gameUi_ = nullptr;
    }
    if (skinSelfTest_) {
        // LATCHED BEFORE THE RESET, and that ordering is the whole point. Engine::run calls
        // onShutdown and reads exitCode() AFTER it, so a verdict left inside the object is gone
        // by the time anything can ask for it -- which is why --skin-test exited 0 however it
        // went. --skin-scene-test only escaped this by never being reset here.
        skinSelfTestExit_ = !skinSelfTest_->finished() ? 2 : (skinSelfTest_->passed() ? 0 : 1);
        e.device()->removeRenderFeature(skinSelfTest_.get());
        skinSelfTest_.reset();
    }
    if (ptFurnace_) {
        e.device()->removeRenderFeature(ptFurnace_.get());
        ptFurnace_.reset();
    }
    // GBufferDebugFeature: see its registration in onInit for why this is unconditional rather
    // than gated on gbufferOverride_/gbufferDebugView_ -- the feature is always registered, so
    // it must always be the thing that unregisters it.
    if (gbufferDebugAttached_) {
        e.device()->removeRenderFeature(&gbufferDebugFeature_);
        gbufferDebugAttached_ = false;
    }
    // Routed through the same reconciler the editor's live toggle uses (not a hand-written
    // removeRenderFeature()+reset() here) so process-exit teardown and a user-driven "turn it off"
    // are provably the same code path. onShutdown() runs well outside any frame, exactly as safe as its usual onUpdate() call site.
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
    // copyFeature() is exposed for exactly this call (see its own comment): thumbnails_ owns and
    // unregisters its ActorPreview internally in shutdown(), but the copy pass is the host's to
    // remove, the same removeRenderFeature()-then-teardown shape as skinnedScene_ just above.
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

} // namespace aver
