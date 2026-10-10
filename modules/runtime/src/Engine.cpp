// Engine implementation: startup, the frame loop, and shutdown.
#include "aver/runtime/Engine.hpp"
#include "aver/runtime/Application.hpp"

#include "aver/platform/Window.hpp"
#include "aver/platform/Splash.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Version.hpp"
#include "aver/core/HitchMarks.hpp"

#include <chrono>
#include <cstdlib>

namespace aver {

Engine::Engine() = default;
Engine::~Engine() = default;

// Full lifecycle: init subsystems, run the loop, shut down. Returns the process code.
int Engine::run(Application* app) {
    BootConfig cfg = app->config();
    // Name and version come from Version.hpp, which CMake injects from project(). This line used to
    // spell "Aver Engine 0.1.0" out by hand, so the banner stayed on 0.1.0 through any version bump
    // -- the one line in the process most likely to be quoted in a bug report, and the one guaranteed
    // to be wrong. Everything else that states a version (the scaffold's ENGINE line, the About box)
    // already reads it from there.
    AVER_INFO("{} Engine {} starting (headless={}, maxFrames={})", kEngineName, kEngineVersion,
              cfg.headless, cfg.maxFrames);

    // maxFrames > 0 is an automated capture run: no splash, and the window opens without focus.
    const bool interactive = cfg.maxFrames == 0;

    // --- Splash (shown during startup) ---
    Splash splash;
    if (!cfg.headless && interactive) splash.show(executableDir() + "\\splash.png");
    // The stages below are named as they START, not as they finish, because the point of the line is
    // to say what the process is busy with while it is unresponsive. Each call repaints
    // synchronously; a message that appears only after the work completes says nothing useful.
    splash.setStatus("Opening window");

    // --- Window (optional; fall back to headless on failure) ---
    if (!cfg.headless) {
        window_ = new Window();
        WindowDesc wd;
        wd.title = cfg.windowTitle;
        wd.width = cfg.windowWidth;
        wd.height = cfg.windowHeight;
        wd.activate = interactive;
        // `interactive` is the same signal WindowDesc::fullscreen keys off internally; passing it
        // through rather than short-circuiting here keeps the rule in ONE place (the platform layer),
        // so a second caller cannot get it wrong.
        wd.fullscreen = cfg.fullscreen;
        wd.resizable = cfg.resizable;
        // HIDDEN WHILE THE SPLASH IS UP. The window is fully created -- sized, DPI-resolved, ready
        // for the swapchain that reads those numbers below -- it just has no pixels on screen. It sat
        // behind the loading screen as a frozen grey rectangle otherwise, which reads as a hang.
        // Only when a splash is actually being shown: a capture run has no splash and must keep
        // behaving exactly as before, and a headless run has no window at all.
        wd.startHidden = !cfg.headless && interactive;
        if (!window_->create(wd)) {
            AVER_WARN("[Engine] window creation failed — continuing headless");
            delete window_;
            window_ = nullptr;
            cfg.headless = true;
        }
    }

    // --- RHI device. Default order is D3D12 -> D3D11 -> Vulkan -> Null. D3D11 is still a 13-line
    //     stub that returns nullptr; VULKAN IS NOT, and this comment claimed it was long after that
    //     stopped being true. It is a real backend, compiled by default (AVER_RHI_VULKAN is ON as of
    //     94e0091) and reachable from this fallback chain, so a machine without D3D12 now genuinely
    //     gets a working device here rather than Null. It is not at D3D12 parity -- the shadow
    //     cascade map is not written (see the Vulkan pushRenderScope note) -- so "falls through to
    //     Vulkan" means a picture, not the same picture.
    rhi::DeviceDesc dd;
    dd.enableDebug = cfg.enableDebugLayer;
    dd.useWarp = cfg.useWarp;
    if (cfg.backend && *cfg.backend) {
        rhi::Backend want{};
        if (rhi::parseBackendName(cfg.backend, want)) {
            // Requested FIRST, with the default order kept behind it: an explicit request is a
            // preference, not a demand, and the RHI warns when it has to fall past it.
            dd.preferred[0] = want;
            dd.preferred[1] = rhi::Backend::D3D12;
            dd.preferred[2] = rhi::Backend::Vulkan;
            dd.preferred[3] = rhi::Backend::Null;
            AVER_INFO("[Engine] backend '{}' requested", cfg.backend);
        } else {
            AVER_ERROR("[Engine] '{}' is not a backend name (d3d12, d3d11, vulkan, null); using the "
                       "default order", cfg.backend);
        }
    }
    splash.setStatus("Creating graphics device");
    device_ = rhi::createDevice(dd);

    // The one point at which an app can plug a backend-specific UI toolkit into the device (see
    // Application::onDeviceCreated's own comment) -- before uiInit below runs against it. A no-op for
    // an app that never overrides the hook, which is every app except the editor.
    app->onDeviceCreated(*this);

    if (window_) {
        splash.setStatus("Creating swapchain");
        rhi::SwapchainDesc sd;
        sd.windowHandle = window_->nativeHandle();
        sd.width = window_->width();
        sd.height = window_->height();
        swapchain_ = device_->createSwapchain(sd);

        // In-window UI, if the app installed a backend for one above. Route raw window messages to it.
        splash.setStatus("Initialising editor UI");
        if (device_->uiInit(window_->nativeHandle())) {
            window_->setMessageHook(&rhi::uiWndProc);
        }
    }

    app_ = app;
    // onInit is where the long tail lives -- shader preludes are compiled, the project's meshes,
    // materials and textures are loaded, scripts are hosted. The application reports its own stages
    // through this, which is why the splash is handed to it rather than kept private here.
    splash.setStatus("Compiling shaders");
    // LIVE FOR AS LONG AS THE SPLASH IS ON SCREEN, which is NOT until onInit returns. This was
    // cleared here, one line after onInit -- before the warm-up loop below, which renders the frames
    // that finish the loading with the splash still covering the window. setLoadingStatus() is a
    // no-op while this is null, so the text froze on whatever the last onInit stage happened to be
    // and then sat there for the entire visible tail of the load. Cleared beside splash.close()
    // instead, so the two facts -- "the splash exists" and "the app can write to it" -- stop and
    // start together.
    splashForApp_ = &splash;
    app->onInit(*this);
    // Render one frame per modal-loop timer tick.
    if (window_) window_->setRenderTick(&Engine::renderTickThunk, this);

    // --- Hold the splash over the frames that finish the loading ------------------------------
    //
    // These frames are REAL frames, rendered into the window while the splash still covers it, so
    // when it goes the editor is already drawing the loaded project rather than starting to. That is
    // the whole point: see Application::startupComplete for why onInit() returning was never the
    // right moment.
    //
    // BOUNDED THREE WAYS, because a loading screen that can outlive the loading is worse than one
    // that goes early: a frame cap, a wall-clock cap, and the window closing. A subclass whose
    // readiness never arrives costs a few seconds, not the session.
    if (!cfg.headless && interactive) {
        constexpr u32 kMaxWarmupFrames = 600;
        constexpr f64 kMaxWarmupSeconds = 20.0;
        // ACCUMULATED, because Clock::restart() is the only reader it has and it resets as it reads --
        // there is no "how long since you were made" call to ask.
        Clock warmupClock;
        f64 warmupSeconds = 0.0;
        u32 warmed = 0;
        frameClock_ = Clock{};
        while (!exit_ && warmed < kMaxWarmupFrames &&
               warmupSeconds < kMaxWarmupSeconds && !app->startupComplete()) {
            if (window_) {
                window_->pumpEvents();
                if (window_->shouldClose()) break;
            }
            frameStep();
            warmupSeconds += warmupClock.restart();
            ++warmed;
        }
        if (warmed >= kMaxWarmupFrames || warmupSeconds >= kMaxWarmupSeconds)
            AVER_WARN("[Engine] startup did not report complete within {} frames / {:.0f}s -- showing "
                      "the window anyway", kMaxWarmupFrames, kMaxWarmupSeconds);
        else
            AVER_INFO("[Engine] startup complete after {} warm-up frame(s)", warmed);
    }

    // 0, NOT 1100: the minimum-visible delay existed so a fast start did not flash the splash for a
    // few frames. The wait above has already kept it up for as long as the loading actually took, so
    // adding a second delay on top would only make a loaded editor sit behind a picture of itself.
    // REVEALED BEFORE THE SPLASH GOES, not after: showing the window first means the loading screen
    // lifts to a drawn editor rather than to the desktop for a frame. The warm-up loop above has
    // already rendered real frames into it, so there is something to reveal.
    if (window_ && !cfg.headless && interactive) window_->show(true);
    splashForApp_ = nullptr;
    if (!cfg.headless) splash.close(interactive ? 0 : 1100);

    // --- Frame loop ---
    frameClock_ = Clock{};
    time_ = {};
    while (!exit_) {
        if (window_) {
            window_->pumpEvents();
            if (window_->shouldClose()) break;
        }
        frameStep();
        if (cfg.maxFrames != 0 && time_.frame >= cfg.maxFrames) {
            AVER_INFO("[Engine] reached maxFrames={}, exiting", cfg.maxFrames);
            break;
        }
    }

    if (window_) window_->setRenderTick(nullptr, nullptr);
    app->onShutdown(*this);

    delete swapchain_;
    swapchain_ = nullptr;
    rhi::destroyDevice(device_);
    device_ = nullptr;
    if (window_) {
        window_->destroy();
        delete window_;
        window_ = nullptr;
    }

    // The application's verdict, not a constant. See Application::exitCode.
    const int code = app->exitCode();
    if (code != 0)
        AVER_ERROR("Aver Engine stopped after {} frame(s) with exit code {}", time_.frame, code);
    else
        AVER_INFO("Aver Engine stopped after {} frame(s)", time_.frame);
    return code;
}

// Names the startup stage on the splash's status line. See the header for why this exists.
void Engine::setLoadingStatus(const std::string& stage) {
    if (!splashForApp_) return;
    // PUMPED, NOT JUST SET. onInit is one long blocking call and nothing else services the splash's
    // message queue while it runs, so a status written without a pump reached the window's state and
    // never its pixels -- the text changed only when something else happened to pump later. That is
    // what made a loading screen with stages still look frozen. LoadingScreen::stage already paired
    // the two for its own splash; this is the same pairing for the borrowed one.
    static_cast<Splash*>(splashForApp_)->setStatus(stage);
    static_cast<Splash*>(splashForApp_)->pump();
}

void Engine::setLoadingProgress(f32 fraction) {
    if (!splashForApp_) return;
    // No pump() of its own: Splash::setProgress repaints and pumps, throttled -- a level load calls
    // this every few hundred placements, and an unconditional pump here would defeat the throttle.
    static_cast<Splash*>(splashForApp_)->setProgress(fraction);
}

// One frame: sync swapchain to the window size, update, render, present.
void Engine::frameStep() {
    if (!device_ || !app_ || inFrame_) return; // guard re-entrancy (timer tick vs main loop)

    // Presenting during a modal resize deadlocks the DWM.
    if (window_ && window_->inModalResize()) return;
    inFrame_ = true;

    // Keep the swapchain matched to the window's client size in physical pixels.
    if (window_ && swapchain_ && !window_->inModalSize()) {
        const u32 w = window_->width(), h = window_->height();
        if (w != 0 && h != 0 && (w != swapchain_->width() || h != swapchain_->height()))
            swapchain_->resize(w, h);
    }

    const f64 dt = frameClock_.restart();
    time_.dt = static_cast<f32>(dt);
    time_.total += time_.dt;
    time_.frame += 1;
    // Handed to the RHI here, at the one place the clock advances, so every shader sees the same
    // instant this frame shades. An animated material reads it from the engine's per-frame block.
    device_->setFrameTime(time_.total, time_.dt);

    // A LOST DEVICE STOPS THE FRAME, and stops it HERE rather than three layers down.
    //
    // The RHI can detect that the GPU has been taken away -- a driver timeout, a driver update, a
    // hardware fault -- but detecting it is worth nothing while the loop keeps calling onUpdate and
    // onRender against it. Every one of those calls fails silently, the log fills with the
    // consequences rather than the cause, and eventually something faults hard and the process
    // disappears. That is what "the engine crashed with no message" was made of.
    //
    // NOT AN EXIT, and that is deliberate. Nothing here can recreate a device -- that means
    // recreating every resource every module owns, which is a feature and not an error path -- so
    // the honest behaviour is to stop drawing and leave the last good frame on screen with the
    // window still answering the mouse. A user can read the title, read the log, and close it
    // normally; exiting would take the explanation off the screen along with everything else.
    //
    // AFTER THE FRAME COUNTER, NOT BEFORE IT, and that ordering is not cosmetic: run()'s own
    // `time_.frame >= cfg.maxFrames` is what ends an automated capture, so returning above the
    // increment froze the counter and left every --frames N run spinning forever with no way out.
    // A frame that draws nothing is still a frame.
    if (device_->deviceLost()) {
        if (!deviceLostHandled_) {
            deviceLostHandled_ = true;
            // The title is the only surface still available: nothing can be DRAWN any more, so an
            // in-editor dialog is not an option, and a log line alone is invisible to anyone who
            // did not start this from a console.
            if (window_) window_->setTitle("Aver Engine -- GPU DEVICE LOST, restart the editor");
            AVER_ERROR("[Engine] the GPU device was lost; rendering has stopped. The window stays "
                       "open so this can be read -- close it and start the editor again.");
        }
        inFrame_ = false;
        return;
    }

    // Phases of any frame longer than AVER_HITCH_MS (250 ms when unset; 0 off).
    const f64 hitchMs = hitchThresholdMs();
    using Clock = std::chrono::steady_clock;
    const Clock::time_point t0 = Clock::now();
    app_->onUpdate(*this, time_);
    const Clock::time_point t1 = Clock::now();
    device_->beginFrame();
    const Clock::time_point t2 = Clock::now();
    device_->uiNewFrame();   // ImGui NewFrame; the app builds widgets in onRender
    app_->onRender(*this);
    const Clock::time_point t3 = Clock::now();
    device_->endFrame();     // records ImGui draw data before present
    const Clock::time_point t4 = Clock::now();
    if (swapchain_) swapchain_->present();
    if (hitchMs > 0.0) {
        const Clock::time_point t5 = Clock::now();
        auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<f64, std::milli>(b - a).count(); };
        if (ms(t0, t5) > hitchMs)
            AVER_INFO("[FrameHitch] frame {} {:.1f} ms | update {:.1f} | beginFrame {:.1f} | render {:.1f} | endFrame {:.1f} | present {:.1f}",
                      time_.frame, ms(t0, t5), ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3, t4), ms(t4, t5));
    }

    inFrame_ = false;
}

// Window modal-loop timer callback; runs one frame.
void Engine::renderTickThunk(void* self) {
    auto* e = static_cast<Engine*>(self);
    if (e && !e->exit_) e->frameStep();
}

} // namespace aver
