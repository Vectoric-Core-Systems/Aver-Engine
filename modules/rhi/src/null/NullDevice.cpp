#include "aver/rhi/RHI.hpp"
#include "aver/core/Log.hpp"

// The Null backend is a fully-valid no-op device. It lets the engine boot and run
// the frame loop with no GPU, which is what Phase 1 exercises (and what a headless
// server / CI uses forever).
namespace aver::rhi {

namespace {

class NullSwapchain final : public ISwapchain {
public:
    NullSwapchain(u32 w, u32 h) : width_(w), height_(h) {}
    void present() override {}
    void resize(u32 w, u32 h) override { width_ = w; height_ = h; }
    u32 width() const override { return width_; }
    u32 height() const override { return height_; }
private:
    u32 width_, height_;
};

class NullDevice final : public IDevice {
public:
    Backend backend() const override { return Backend::Null; }
    const char* adapterName() const override { return "Null Device (no GPU)"; }
    ISwapchain* createSwapchain(const SwapchainDesc& d) override {
        AVER_TRACE("[RHI.Null] createSwapchain {}x{}", d.width, d.height);
        return new NullSwapchain(d.width, d.height);
    }
    void beginFrame() override {}
    void endFrame() override {}
};

} // namespace

namespace detail {
IDevice* createNullDevice(const DeviceDesc&) { return new NullDevice(); }
}

} // namespace aver::rhi
