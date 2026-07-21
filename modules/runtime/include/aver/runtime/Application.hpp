#pragma once
#include "aver/core/Types.hpp"
#include "aver/core/Time.hpp"

namespace aver {

class Engine;

struct BootConfig {
    const char* windowTitle = "Aver Engine";
    u32 windowWidth = 1280;
    u32 windowHeight = 720;
    u64 maxFrames = 0;     // 0 = run until the window is closed
    bool headless = false; // skip window creation entirely
    // Ask the RHI for the software rasteriser instead of a hardware adapter. A development
    // switch for exercising fallback paths on hardware that does not need them; see
    // `rhi::DeviceDesc::useWarp`.
    bool useWarp = false;
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

// Defined by the application (e.g. sandbox). The entry point calls this.
Application* createApplication(int argc, char** argv);

} // namespace aver
