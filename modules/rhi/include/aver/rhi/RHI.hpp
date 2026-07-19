#pragma once
#include "aver/core/Types.hpp"

// Aver RHI — the single render-hardware abstraction every GPU consumer targets.
// Backends (D3D12/D3D11/Vulkan) implement these interfaces; a Null backend always
// exists as a fallback so the engine runs headless / on unsupported hardware.
namespace aver::rhi {

enum class Backend { Null, D3D12, D3D11, Vulkan };
const char* backendName(Backend b);

struct SwapchainDesc {
    void* windowHandle = nullptr; // HWND
    u32 width = 0;
    u32 height = 0;
    u32 bufferCount = 2;
};

class ISwapchain {
public:
    virtual ~ISwapchain() = default;
    virtual void present() = 0;
    virtual void resize(u32 width, u32 height) = 0;
    virtual u32 width() const = 0;
    virtual u32 height() const = 0;
};

struct DeviceDesc {
    // Preference order; createDevice() returns the first compiled-in backend that
    // initialises, falling back to Null.
    Backend preferred[4] = {Backend::D3D12, Backend::D3D11, Backend::Vulkan, Backend::Null};
    u32 preferredCount = 4;
    bool enableDebug = false;
};

class IDevice {
public:
    virtual ~IDevice() = default;
    virtual Backend backend() const = 0;
    virtual const char* adapterName() const = 0;
    virtual ISwapchain* createSwapchain(const SwapchainDesc& desc) = 0;
    virtual void beginFrame() = 0;
    virtual void endFrame() = 0;
};

IDevice* createDevice(const DeviceDesc& desc = {});
void destroyDevice(IDevice* device);

} // namespace aver::rhi
