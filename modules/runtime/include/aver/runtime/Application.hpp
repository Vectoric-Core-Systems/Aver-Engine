// The application hooks the engine calls, and the boot configuration it reads before starting.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/core/Time.hpp"

namespace aver {

class Engine;

// What the engine needs before it creates a window or a device.
struct BootConfig {
    const char* windowTitle = "Aver Engine";
    u32 windowWidth = 1280;
    u32 windowHeight = 720;
    u64 maxFrames = 0;     // 0 = run until the window is closed
    bool headless = false; // skip window creation entirely
    bool useWarp = false;  // ask the RHI for the software rasteriser
    bool enableDebugLayer = false;   // opt-in in every build type; `--debug-layer`
};

// Applications subclass this. The engine owns the loop and calls these hooks.
class Application {
public:
    virtual ~Application() = default;
    virtual BootConfig config() const { return {}; }
    virtual void onInit(Engine&) {}
    virtual void onUpdate(Engine&, const Timestep&) {}
    virtual void onRender(Engine&) {}
    virtual void onShutdown(Engine&) {}
};

// Defined by the application. The entry point calls this.
Application* createApplication(int argc, char** argv);

} // namespace aver
