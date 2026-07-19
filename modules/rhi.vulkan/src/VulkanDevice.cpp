#include "aver/rhi/RHI.hpp"
#include "aver/core/Log.hpp"

// Compiled only when AVER_RHI_VULKAN=ON. Phase 1 stub; real Vulkan device (instance,
// physical/logical device, VkSwapchainKHR, SPIR-V PSOs) arrives once the SDK is installed.
namespace aver::rhi::detail {

IDevice* createVulkanDevice(const DeviceDesc&) {
    AVER_TRACE("[RHI.Vulkan] backend stub — needs Vulkan SDK; real device later");
    return nullptr;
}

} // namespace aver::rhi::detail
