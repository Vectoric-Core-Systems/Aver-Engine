#pragma once
// Aver.Runtime: the Engine that owns the window, the RHI device and the frame loop.
#include "aver/core/Types.hpp"
#include "aver/core/Time.hpp"

#include <string>

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

    // Names the startup stage currently underway, on the splash screen's status line.
    //
    // FOR onInit's USE. Everything slow about starting this editor happens inside
    // Application::onInit -- shader preludes, a project's meshes and materials, the scripting host --
    // and the engine cannot name those stages because it does not know what the application is
    // doing. A no-op once the splash has closed, and in headless or capture runs where there never
    // was one, so a caller never has to ask whether it is safe to call.
    void setLoadingStatus(const std::string& stage);
    // The same splash's progress bar, 0..1 (negative hides it). Splash::setProgress pumps, throttled.
    void setLoadingProgress(f32 fraction);
    // Whether a loading splash is CURRENTLY up -- i.e. whether setLoadingStatus above will actually
    // show anything. True only between show() and close() during startup.
    //
    // It exists so a long blocking operation that can run EITHER during startup or later (opening a
    // project is the one that matters: from the command line it lands inside onInit, from the
    // browser it lands frames later) can reuse this splash when there is one and create its own only
    // when there is not. Two overlapping top-most splash windows is the failure this prevents.
    bool loadingScreenActive() const { return splashForApp_ != nullptr; }

private:
    // One frame: sync swapchain to the window size, update, render, present.
    void frameStep();
    // Window modal-loop timer callback; runs one frame.
    static void renderTickThunk(void* self);

    Window* window_ = nullptr;
    rhi::IDevice* device_ = nullptr;
    rhi::ISwapchain* swapchain_ = nullptr;
    Application* app_ = nullptr;
    // Borrowed, non-owning, and only non-null for the duration of onInit -- the Splash itself is a
    // local in run() and is destroyed when startup ends. Typed as void* so this header does not have
    // to include a platform one for a pointer it never dereferences.
    void* splashForApp_ = nullptr;
    Clock frameClock_;
    Timestep time_;
    bool exit_ = false;
    bool inFrame_ = false;
    // Whether the one-time device-lost report has been made. See frameStep: the condition is
    // permanent, so without this the log and the retitle would repeat for every message pump.
    bool deviceLostHandled_ = false;
};

} // namespace aver
