#include "aver/rhi/RHI.hpp"
#include "aver/core/Log.hpp"

namespace aver::rhi {

const char* backendName(Backend b) {
    switch (b) {
        case Backend::Null:   return "Null";
        case Backend::D3D12:  return "D3D12";
        case Backend::D3D11:  return "D3D11";
        case Backend::Vulkan: return "Vulkan";
    }
    return "Unknown";
}

namespace detail {
IDevice* createNullDevice(const DeviceDesc& desc);
#if AVER_HAS_D3D12
IDevice* createD3D12Device(const DeviceDesc& desc);
#endif
#if AVER_HAS_D3D11
IDevice* createD3D11Device(const DeviceDesc& desc);
#endif
#if AVER_HAS_VULKAN
IDevice* createVulkanDevice(const DeviceDesc& desc);
#endif
} // namespace detail

static IDevice* tryBackend(Backend b, const DeviceDesc& desc) {
    switch (b) {
        case Backend::Null: return detail::createNullDevice(desc);
        case Backend::D3D12:
#if AVER_HAS_D3D12
            return detail::createD3D12Device(desc);
#else
            return nullptr;
#endif
        case Backend::D3D11:
#if AVER_HAS_D3D11
            return detail::createD3D11Device(desc);
#else
            return nullptr;
#endif
        case Backend::Vulkan:
#if AVER_HAS_VULKAN
            return detail::createVulkanDevice(desc);
#else
            return nullptr;
#endif
    }
    return nullptr;
}

IDevice* createDevice(const DeviceDesc& desc) {
    const u32 count = desc.preferredCount < 4 ? desc.preferredCount : 4;
    for (u32 i = 0; i < count; ++i) {
        const Backend b = desc.preferred[i];
        if (IDevice* dev = tryBackend(b, desc)) {
            AVER_INFO("[RHI] device created (backend={})", backendName(dev->backend()));
            return dev;
        }
        if (b != Backend::Null) {
            AVER_TRACE("[RHI] backend {} unavailable, trying next", backendName(b));
        }
    }
    AVER_WARN("[RHI] no preferred backend available; using Null");
    return detail::createNullDevice(desc);
}

void destroyDevice(IDevice* device) { delete device; }

// UI window-message routing registry (set by whichever backend hosts ImGui).
namespace {
UiWndProcFn g_uiWndProc = nullptr;
}
void registerUiWndProc(UiWndProcFn fn) { g_uiWndProc = fn; }
bool uiWndProc(void* hwnd, u32 msg, u64 wparam, i64 lparam) {
    return g_uiWndProc ? g_uiWndProc(hwnd, msg, wparam, lparam) : false;
}

} // namespace aver::rhi
