// D3D11 RHI backend: the fallback device for older hardware. Not implemented yet.
#include "aver/rhi/RHI.hpp"
#include "aver/core/Log.hpp"

namespace aver::rhi::detail {

// Returns null: no D3D11 device is created.
IDevice* createD3D11Device(const DeviceDesc&) {
    AVER_TRACE("[RHI.D3D11] backend stub — real device arrives in Phase 3");
    return nullptr;
}

} // namespace aver::rhi::detail
