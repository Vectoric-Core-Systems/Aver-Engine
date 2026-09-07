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
    // Borderless fullscreen for an INTERACTIVE run; see WindowDesc::fullscreen for why a capture run
    // ignores it.
    bool fullscreen = false;
    bool enableDebugLayer = false;   // opt-in in every build type; `--debug-layer`
    // Which RHI backend to ASK FOR first: "d3d12", "d3d11", "vulkan", "null", or empty for the
    // compiled-in default order. The engine still falls back if it is unavailable, and says loudly
    // that it did -- a run that silently used a different backend than the one requested is worse
    // than a run that refused to start.
    const char* backend = nullptr;
};

// Applications subclass this. The engine owns the loop and calls these hooks.
class Application {
public:
    virtual ~Application() = default;
    virtual BootConfig config() const { return {}; }

    // Fires once, right after the RHI device exists but BEFORE Engine::run creates the swapchain or
    // calls IDevice::uiInit -- the one point at which an app can plug a backend-specific UI toolkit
    // into the device (see aver::rhi::d3d12::installUiBackend) in time for that uiInit call to find
    // one installed. onInit() itself runs too late for this: by the time it is called, uiInit has
    // already run. Empty default, so only an app that overrides it (the editor) pays for it existing.
    virtual void onDeviceCreated(Engine&) {}

    virtual void onInit(Engine&) {}

    // Has everything the first frame needs finished arriving?
    //
    // WHY onInit() RETURNING IS NOT THAT. The splash used to close the moment onInit() returned, and
    // onInit() is only the SYNCHRONOUS half of loading: the meshes are uploaded, the shader
    // permutations compiled and the GI volume first voxelised on the first RENDERED frames, which is
    // seconds of work that happened after the splash was already gone. So the window appeared, empty
    // or half-lit, and the person watching saw the editor "start" and then hang.
    //
    // DEFAULT TRUE so every other Application is unaffected -- the engine closes the splash exactly
    // where it used to unless something opts in by overriding this. The engine also caps how long it
    // will wait, so a subclass that never returns true delays startup rather than hanging it.
    virtual bool startupComplete() const { return true; }
    virtual void onUpdate(Engine&, const Timestep&) {}
    virtual void onRender(Engine&) {}
    virtual void onShutdown(Engine&) {}

    // The process exit code, read once after onShutdown. Zero means success.
    //
    // THIS EXISTS SO A TEST MODE CAN FAIL. The editor has half a dozen of them -- --skin-scene-test,
    // --skin-draw-test, --furnace-test, --refl-test, --spawn-test, --play-test -- and every one of
    // them reported its verdict only to the log while the process exited 0 regardless. A test a
    // script cannot check is not a test: CI would have gone green with the renderer broken. The
    // default keeps every ordinary application exiting 0 without knowing this hook is here.
    virtual int exitCode() const { return 0; }
};

// Defined by the application. The entry point calls this.
Application* createApplication(int argc, char** argv);

} // namespace aver
