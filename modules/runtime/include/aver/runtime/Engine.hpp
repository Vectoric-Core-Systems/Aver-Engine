#pragma once
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
    Window* window_ = nullptr;
    rhi::IDevice* device_ = nullptr;
    rhi::ISwapchain* swapchain_ = nullptr;
    Timestep time_;
    bool exit_ = false;
};

} // namespace aver
