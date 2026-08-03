// Engine implementation: startup, the frame loop, and shutdown.
#include "aver/runtime/Engine.hpp"
#include "aver/runtime/Application.hpp"

#include "aver/platform/Window.hpp"
#include "aver/platform/Splash.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/core/Log.hpp"

namespace aver {

Engine::Engine() = default;
Engine::~Engine() = default;

// Full lifecycle: init subsystems, run the loop, shut down. Returns the process code.
int Engine::run(Application* app) {
    BootConfig cfg = app->config();
    AVER_INFO("Aver Engine 0.1.0 starting (headless={}, maxFrames={})", cfg.headless, cfg.maxFrames);

    // maxFrames > 0 is an automated capture run: no splash, and the window opens without focus.
    const bool interactive = cfg.maxFrames == 0;

    // --- Splash (shown during startup) ---
    Splash splash;
    if (!cfg.headless && interactive) splash.show(executableDir() + "\\splash.png");

    // --- Window (optional; fall back to headless on failure) ---
    if (!cfg.headless) {
        window_ = new Window();
        WindowDesc wd;
        wd.title = cfg.windowTitle;
        wd.width = cfg.windowWidth;
        wd.height = cfg.windowHeight;
        wd.activate = interactive;
        if (!window_->create(wd)) {
            AVER_WARN("[Engine] window creation failed — continuing headless");
            delete window_;
            window_ = nullptr;
            cfg.headless = true;
        }
    }

    // --- RHI device. Default order is D3D12 -> D3D11 -> Vulkan -> Null; D3D11 and Vulkan are
    //     13-line stubs that return nullptr today, so the default order really means D3D12 or Null.
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
    device_ = rhi::createDevice(dd);

    if (window_) {
        rhi::SwapchainDesc sd;
        sd.windowHandle = window_->nativeHandle();
        sd.width = window_->width();
        sd.height = window_->height();
        swapchain_ = device_->createSwapchain(sd);

        // In-window editor UI (Dear ImGui). Route raw window messages to it.
        if (device_->uiInit(window_->nativeHandle())) {
            window_->setMessageHook(&rhi::uiWndProc);
        }
    }

    app_ = app;
    app->onInit(*this);
    // Render one frame per modal-loop timer tick.
    if (window_) window_->setRenderTick(&Engine::renderTickThunk, this);
    if (!cfg.headless) splash.close(1100);

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

    app_->onUpdate(*this, time_);
    device_->beginFrame();
    device_->uiNewFrame();   // ImGui NewFrame; the app builds widgets in onRender
    app_->onRender(*this);
    device_->endFrame();     // records ImGui draw data before present
    if (swapchain_) swapchain_->present();

    inFrame_ = false;
}

// Window modal-loop timer callback; runs one frame.
void Engine::renderTickThunk(void* self) {
    auto* e = static_cast<Engine*>(self);
    if (e && !e->exit_) e->frameStep();
}

} // namespace aver
