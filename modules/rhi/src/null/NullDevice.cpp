// The Null RHI backend: a fully-valid no-op device so the engine boots and runs
// its frame loop with no GPU (headless / CI).
#include "aver/rhi/RHI.hpp"
#include "aver/core/Log.hpp"

namespace aver::rhi {

namespace {

// Swapchain that presents nothing and only remembers its size.
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

// Device with no GPU behind it: every frame operation is a no-op.
class NullDevice final : public IDevice {
public:
    Backend backend() const override { return Backend::Null; }
    const char* adapterName() const override { return "Null Device (no GPU)"; }
    // Creates a swapchain that records the requested size and does nothing else.
    ISwapchain* createSwapchain(const SwapchainDesc& d) override {
        AVER_TRACE("[RHI.Null] createSwapchain {}x{}", d.width, d.height);
        return new NullSwapchain(d.width, d.height);
    }
    void beginFrame() override {}
    void endFrame() override {}
};

} // namespace

namespace detail {
// Creates the Null device.
IDevice* createNullDevice(const DeviceDesc&) { return new NullDevice(); }
}

} // namespace aver::rhi
