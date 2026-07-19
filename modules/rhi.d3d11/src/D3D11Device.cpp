#include "aver/rhi/RHI.hpp"
#include "aver/core/Log.hpp"

// Phase 1 stub (fallback backend for older hardware). Real device arrives in Phase 3.
namespace aver::rhi::detail {

IDevice* createD3D11Device(const DeviceDesc&) {
    AVER_TRACE("[RHI.D3D11] backend stub — real device arrives in Phase 3");
    return nullptr;
}

} // namespace aver::rhi::detail
