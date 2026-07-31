#pragma once
// Aver.Runtime: the Engine that owns the window, the RHI device and the frame loop.
#include "aver/core/Types.hpp"
#include "aver/core/Time.hpp"

namespace aver {

class Window;
class Application;
namespace rhi { class IDevice; class ISwapchain; }

// The composition root. Owns the window, the RHI device, and the frame loop.
class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Full lifecycle: init subsystems, run the loop, shut down. Returns process code.
    int run(Application* app);

    void requestExit() { exit_ = true; }

    Window* window() const { return window_; }
    rhi::IDevice* device() const { return device_; }
    const Timestep& time() const { return time_; }

private:
    // One frame: sync swapchain to the window size, update, render, present.
    void frameStep();
    // Window modal-loop timer callback; runs one frame.
    static void renderTickThunk(void* self);

    Window* window_ = nullptr;
    rhi::IDevice* device_ = nullptr;
    rhi::ISwapchain* swapchain_ = nullptr;
    Application* app_ = nullptr;
    Clock frameClock_;
    Timestep time_;
    bool exit_ = false;
    bool inFrame_ = false;
};

} // namespace aver
