#include "aver/rhi/RHI.hpp"
#include "aver/core/Log.hpp"

// Phase 1 stub. Returning nullptr makes createDevice() fall through to the next
// preferred backend (ultimately Null). Phase 3 implements the real D3D12 device
// (DXGI factory, ID3D12Device, command queues, swapchain) here.
namespace aver::rhi::detail {

IDevice* createD3D12Device(const DeviceDesc&) {
    AVER_TRACE("[RHI.D3D12] backend stub — real device arrives in Phase 3");
    return nullptr;
}

} // namespace aver::rhi::detail
