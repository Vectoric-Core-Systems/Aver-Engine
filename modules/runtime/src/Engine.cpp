#include "aver/runtime/Engine.hpp"
#include "aver/runtime/Application.hpp"

#include "aver/platform/Window.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/core/Log.hpp"

namespace aver {

Engine::Engine() = default;
Engine::~Engine() = default;

int Engine::run(Application* app) {
    BootConfig cfg = app->config();
    AVER_INFO("Aver Engine 0.1.0 starting (headless={}, maxFrames={})", cfg.headless, cfg.maxFrames);

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
    }

    app->onInit(*this);

    // --- Frame loop ---
    Clock frameClock;
    time_ = {};
    while (!exit_) {
        if (window_) {
            window_->pumpEvents();
            if (window_->shouldClose()) break;
            // Forward window resizes to the swapchain (no-op for the Null backend).
            if (swapchain_) {
                const u32 w = window_->width(), h = window_->height();
                if (w != 0 && h != 0 && (w != swapchain_->width() || h != swapchain_->height())) {
                    swapchain_->resize(w, h);
                }
            }
        }

        const f64 dt = frameClock.restart();
        time_.dt = static_cast<f32>(dt);
        time_.total += time_.dt;
        time_.frame += 1;

        app->onUpdate(*this, time_);

        device_->beginFrame();
        app->onRender(*this);
        device_->endFrame();
        if (swapchain_) swapchain_->present();

        if (cfg.maxFrames != 0 && time_.frame >= cfg.maxFrames) {
            AVER_INFO("[Engine] reached maxFrames={}, exiting", cfg.maxFrames);
            break;
        }
    }

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

} // namespace aver
