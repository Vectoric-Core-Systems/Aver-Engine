#include "aver/runtime/EntryPoint.hpp"
#include "aver/core/Log.hpp"

#include <cstdlib>
#include <cstring>

namespace aver {

class SandboxApp final : public Application {
public:
    SandboxApp(u64 maxFrames, bool headless) : maxFrames_(maxFrames), headless_(headless) {}

    BootConfig config() const override {
        BootConfig c;
        c.windowTitle = "Aver Engine \xE2\x80\x94 Sandbox";
        c.windowWidth = 1280;
        c.windowHeight = 720;
        c.maxFrames = maxFrames_;
        c.headless = headless_;
        return c;
    }

    void onInit(Engine&) override {
        AVER_INFO("[Sandbox] init");
    }

    void onUpdate(Engine&, const Timestep& t) override {
        // Log the first few frames and then once a second-ish so runs stay readable.
        if (t.frame <= 3 || (t.frame % 60) == 0) {
            AVER_INFO("[Sandbox] frame {} dt={:.4f}s total={:.3f}s", t.frame, t.dt, t.total);
        }
    }

    void onRender(Engine&) override {}

    void onShutdown(Engine&) override {
        AVER_INFO("[Sandbox] shutdown");
    }

private:
    u64 maxFrames_;
    bool headless_;
};

Application* createApplication(int argc, char** argv) {
    u64 frames = 0;
    bool headless = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = std::strtoull(argv[++i], nullptr, 10);
        }
    }
    return new SandboxApp(frames, headless);
}

} // namespace aver
