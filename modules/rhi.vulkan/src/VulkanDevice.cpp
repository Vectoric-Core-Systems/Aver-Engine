// Vulkan backend entry point. Stub: compiled only when AVER_RHI_VULKAN=ON.
#include "aver/rhi/RHI.hpp"
#include "aver/core/Log.hpp"

namespace aver::rhi::detail {

// Creates the Vulkan device. Always returns null until the backend is implemented.
IDevice* createVulkanDevice(const DeviceDesc&) {
    AVER_TRACE("[RHI.Vulkan] backend stub — needs Vulkan SDK; real device later");
    return nullptr;
}

} // namespace aver::rhi::detail
