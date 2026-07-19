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

int Engine::run(Application* app) {
    BootConfig cfg = app->config();
    AVER_INFO("Aver Engine 0.1.0 starting (headless={}, maxFrames={})", cfg.headless, cfg.maxFrames);

    // --- Splash (shown during startup) ---
    Splash splash;
    if (!cfg.headless) splash.show(executableDir() + "\\splash.png");

    // --- Window (optional; fall back to headless on failure) ---
    if (!cfg.headless) {
        window_ = new Window();
        WindowDesc wd;
        wd.title = cfg.windowTitle;
        wd.width = cfg.windowWidth;
        wd.height = cfg.windowHeight;
        if (!window_->create(wd)) {
            AVER_WARN("[Engine] window creation failed — continuing headless");
            delete window_;
            window_ = nullptr;
            cfg.headless = true;
        }
    }

    // --- RHI device (D3D12 -> D3D11 -> Vulkan -> Null, per stubs today -> Null) ---
    rhi::DeviceDesc dd;
    dd.enableDebug = true;
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
    // Render one frame per modal-loop timer tick so drag/size/maximise doesn't freeze the view.
    if (window_) window_->setRenderTick(&Engine::renderTickThunk, this);
    if (!cfg.headless) splash.close(1100); // keep the splash up briefly, then reveal the editor

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

    AVER_INFO("Aver Engine stopped after {} frame(s)", time_.frame);
    return 0;
}

void Engine::frameStep() {
    if (!device_ || !app_ || inFrame_) return; // guard re-entrancy (timer tick vs main loop)
    // While the user is actively resizing the window, presenting deadlocks the DWM; skip the
    // whole frame (the view resumes the moment the drag ends). A plain move still renders.
    if (window_ && window_->inModalResize()) return;
    inFrame_ = true;

    // Keep the swapchain matched to the window's client size (physical pixels). Skip while a
    // modal move loop is active (resize is handled once it ends).
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

void Engine::renderTickThunk(void* self) {
    auto* e = static_cast<Engine*>(self);
    if (e && !e->exit_) e->frameStep();
}

} // namespace aver
