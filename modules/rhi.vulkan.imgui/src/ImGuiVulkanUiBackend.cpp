// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
//
// Dear ImGui on Vulkan: the concrete implementation of Aver.RHI.Vulkan's IUiBackend seam, and the
// structural twin of modules/rhi.d3d12.imgui/src/ImGuiUiBackend.cpp. Read that file alongside this
// one -- the lifecycle is deliberately identical, and every difference below is a Vulkan fact
// rather than a change of approach.
//
// THREE OF THOSE DIFFERENCES ARE WORTH KNOWING BEFORE READING THE CODE:
//
//  1. NO SDK, SO THE ENTRY POINTS ARE LOADED BY HAND. Aver.RHI.Vulkan links no vulkan-1.lib and
//     resolves everything at runtime through LoadLibraryW + vkGetInstanceProcAddr (VulkanCommon.hpp
//     builds every TU with VK_NO_PROTOTYPES for exactly this reason). imgui_impl_vulkan is
//     compiled the same way and offers ImGui_ImplVulkan_LoadFunctions as the hook for it, which
//     MUST be called before ImGui_ImplVulkan_Init or the backend calls into null pointers.
//
//  2. DYNAMIC RENDERING, NOT A RENDER PASS. This engine's Vulkan backend has no VkRenderPass to
//     hand over -- see UiBackendInitDesc::dynamicRendering, which is why that field exists and is
//     always true. ImGui is told the colour format instead, through
//     PipelineInfoMain.PipelineRenderingCreateInfo.
//
//  3. THE HANDLES ARRIVE AS OPAQUE u64s. The seam is a public header and house rule 3 keeps Vulkan
//     types out of those, so UiBackendInitDesc carries everything pointer-sized as u64 and this
//     file -- which is allowed to include vulkan.h -- casts them back. Casting is confined to
//     init(); nothing below it re-derives a handle.
#include "aver/rhi/vulkan/ImGuiUiBackend.hpp"
#include "aver/core/Log.hpp"

#define VK_NO_PROTOTYPES
// For vkCreateWin32SurfaceKHR, which Platform_CreateVkSurface below needs -- see its own comment
// for why a single-viewport editor still has to supply that handler.
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <windows.h>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_vulkan.h"

// Declared by imgui_impl_win32.cpp; not in its header, exactly as the D3D12 backend declares it.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace aver::rhi::vkb {

namespace {

// vkGetInstanceProcAddr, out of the driver's own loader DLL. The SAME arrangement VulkanCommon.hpp
// documents for the RHI itself: vulkan-1.dll ships with the GPU driver, so there is nothing to
// install and nothing to link.
PFN_vkGetInstanceProcAddr loadGetInstanceProcAddr() {
    static PFN_vkGetInstanceProcAddr fn = nullptr;
    static bool tried = false;
    if (tried) return fn;
    tried = true;
    if (HMODULE dll = ::LoadLibraryW(L"vulkan-1.dll"))
        fn = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            reinterpret_cast<void*>(::GetProcAddress(dll, "vkGetInstanceProcAddr")));
    if (!fn) AVER_ERROR("[RHI.Vulkan.ImGui] vulkan-1.dll has no vkGetInstanceProcAddr");
    return fn;
}

// ImGui_ImplVulkan_LoadFunctions' callback shape. `user` is the VkInstance, which is what makes the
// device-level entry points resolvable too.
PFN_vkVoidFunction loaderThunk(const char* name, void* user) {
    PFN_vkGetInstanceProcAddr gipa = loadGetInstanceProcAddr();
    if (!gipa) return nullptr;
    return gipa(static_cast<VkInstance>(user), name);
}

// SURFACE CREATION FOR A SECONDARY VIEWPORT, and it has to exist even though this editor never
// opens one.
//
// imgui_impl_win32 advertises ImGuiBackendFlags_PlatformHasViewports unconditionally, and
// ImGui_ImplVulkan_InitMultiViewportSupport then ASSERTS that the platform supplied this handler --
// before anything checks whether ImGuiConfigFlags_ViewportsEnable is actually set. With no handler
// the assert fires inside ImGui_ImplVulkan_Init and pops a modal dialog, which on a headless
// `--frames N` capture run simply hangs forever. Supplying it is both the smaller fix and the
// honest one: the platform genuinely CAN make a surface, so saying so is true, and multi-viewport
// then works if anyone ever turns it on rather than being quietly foreclosed.
int createVkSurface(ImGuiViewport* vp, ImU64 instance, const void* allocator, ImU64* outSurface) {
    if (!vp || !outSurface) return static_cast<int>(VK_ERROR_INITIALIZATION_FAILED);
    // PlatformHandleRaw is the HWND; PlatformHandle is whatever the platform backend chose to mean.
    HWND hwnd = static_cast<HWND>(vp->PlatformHandleRaw ? vp->PlatformHandleRaw : vp->PlatformHandle);
    if (!hwnd) return static_cast<int>(VK_ERROR_INITIALIZATION_FAILED);

    auto create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
        loaderThunk("vkCreateWin32SurfaceKHR", reinterpret_cast<VkInstance>(instance)));
    if (!create) {
        AVER_ERROR("[RHI.Vulkan.ImGui] vkCreateWin32SurfaceKHR is unavailable; no viewport surface");
        return static_cast<int>(VK_ERROR_EXTENSION_NOT_PRESENT);
    }
    VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    ci.hinstance = ::GetModuleHandleW(nullptr);
    ci.hwnd = hwnd;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const VkResult r = create(reinterpret_cast<VkInstance>(instance), &ci,
                              static_cast<const VkAllocationCallbacks*>(allocator), &surface);
    *outSurface = reinterpret_cast<ImU64>(surface);
    return static_cast<int>(r);
}

void checkVkResult(VkResult err) {
    if (err != VK_SUCCESS) AVER_ERROR("[RHI.Vulkan.ImGui] Vulkan call failed inside ImGui: {}", static_cast<int>(err));
}

}  // namespace

class ImGuiVulkanBackend final : public IUiBackend {
public:
    // Safety net for a caller that skips the explicit device->uiShutdown()/shutdown() dance -- costs
    // nothing when shutdown() already ran, since it is idempotent on active_.
    ~ImGuiVulkanBackend() override { shutdown(); }

    bool init(void* hwnd, const UiBackendInitDesc& desc) override {
        if (active_) return true;
        if (!hwnd || !desc.device || !desc.instance || !desc.queue) {
            AVER_ERROR("[RHI.Vulkan.ImGui] init without a window or a complete device");
            return false;
        }
        if (!desc.descriptorPool) {
            AVER_ERROR("[RHI.Vulkan.ImGui] init without a descriptor pool to allocate from");
            return false;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.IniFilename = nullptr;
        ImGui::StyleColorsDark();

        if (!ImGui_ImplWin32_Init(hwnd)) {
            AVER_ERROR("[RHI.Vulkan.ImGui] ImGui_ImplWin32_Init failed");
            ImGui::DestroyContext();
            return false;
        }

        // BEFORE Init, and it is not optional: this module is built VK_NO_PROTOTYPES, so every
        // Vulkan symbol imgui_impl_vulkan calls is null until this fills them in.
        instance_ = reinterpret_cast<VkInstance>(desc.instance);
        if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, &loaderThunk, instance_)) {
            AVER_ERROR("[RHI.Vulkan.ImGui] ImGui_ImplVulkan_LoadFunctions failed -- no entry points");
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            return false;
        }

        // The format the overlay pass will have open when render() is called. Held as a member
        // because ImGui keeps the POINTER, not a copy -- see PipelineRenderingCreateInfo's own note
        // in imgui_impl_vulkan.h about data needing to outlive the backend.
        colorFormat_ = static_cast<VkFormat>(desc.colorFormat);

        // BEFORE Init: ImGui_ImplVulkan_InitMultiViewportSupport asserts on this from inside it.
        ImGui::GetPlatformIO().Platform_CreateVkSurface = &createVkSurface;

        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion     = VK_API_VERSION_1_3;
        info.Instance       = instance_;
        info.PhysicalDevice = reinterpret_cast<VkPhysicalDevice>(desc.physicalDevice);
        info.Device         = reinterpret_cast<VkDevice>(desc.device);
        info.QueueFamily    = desc.queueFamily;
        info.Queue          = reinterpret_cast<VkQueue>(desc.queue);
        // IMGUI MAKES ITS OWN POOL, and the seam's descriptorPool is deliberately NOT used.
        //
        // ImGui allocates VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER for every texture it draws, and
        // this backend's pool budgets none at all -- its types are SAMPLED_IMAGE, STORAGE_IMAGE,
        // STORAGE_BUFFER, UNIFORM_BUFFER_DYNAMIC, SAMPLER and ACCELERATION_STRUCTURE, because
        // separate images and samplers are what the engine's own shaders use. Handing that pool over
        // makes every ImGui_ImplVulkan_AddTexture fail with OUT_OF_POOL_MEMORY, which is silent
        // enough to look like "the viewport is just black".
        //
        // Setting DescriptorPoolSize instead makes the backend create a correctly-typed pool of its
        // own (imgui_impl_vulkan.cpp:1170), which also keeps ImGui's descriptor budget from
        // competing with the renderer's. Sized for the font atlas, the editor viewport and the
        // thumbnail cache's 64 resident previews, with room over.
        info.DescriptorPool = VK_NULL_HANDLE;
        info.DescriptorPoolSize = 256;
        // MinImageCount must be >= 2 and <= ImageCount. The swapchain negotiated imageCount; two is
        // this backend's own floor (kFrameCount), so clamping rather than trusting either alone.
        info.ImageCount     = desc.imageCount >= 2 ? desc.imageCount : 2;
        info.MinImageCount  = 2;
        info.UseDynamicRendering = true;
        info.CheckVkResultFn = &checkVkResult;

        renderingInfo_ = VkPipelineRenderingCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        renderingInfo_.colorAttachmentCount = 1;
        renderingInfo_.pColorAttachmentFormats = &colorFormat_;
        info.PipelineInfoMain.MSAASamples = static_cast<VkSampleCountFlagBits>(desc.sampleCount ? desc.sampleCount : 1);
        info.PipelineInfoMain.PipelineRenderingCreateInfo = renderingInfo_;

        if (!ImGui_ImplVulkan_Init(&info)) {
            AVER_ERROR("[RHI.Vulkan.ImGui] ImGui_ImplVulkan_Init failed");
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            return false;
        }
        active_ = true;
        AVER_INFO("[RHI.Vulkan.ImGui] ImGui UI initialised (docking, dynamic rendering)");
        return true;
    }

    void newFrame() override {
        if (!active_) return;
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
    }

    void shutdown() override {
        if (!active_) return;
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        active_ = false;
    }

    void render(u64 commandBuffer) override {
        if (!active_ || !commandBuffer) return;
        // ImGui::Render() HERE rather than in the caller, matching the D3D12 backend exactly: the
        // caller's contract is "draw whatever has been built since newFrame()", not "I have already
        // called Render for you".
        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),
                                        reinterpret_cast<VkCommandBuffer>(commandBuffer));
    }

    bool wantsMouse() const override    { return active_ && ImGui::GetIO().WantCaptureMouse; }
    bool wantsKeyboard() const override { return active_ && ImGui::GetIO().WantCaptureKeyboard; }

    bool wndProc(void* hwnd, u32 msg, u64 w, i64 l) override {
        return ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(hwnd), static_cast<UINT>(msg),
                                              static_cast<WPARAM>(w), static_cast<LPARAM>(l)) != 0;
    }

    // One descriptor set per texture, which is the whole of a Vulkan ImTextureID -- and the reason
    // this seam needs no shared allocator where D3D12's does: there is no single bound heap every
    // texture must live in.
    u64 textureId(u64 imageView, u64 sampler) override {
        if (!active_ || !imageView || !sampler) return 0;
        VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(reinterpret_cast<VkSampler>(sampler),
                                                         reinterpret_cast<VkImageView>(imageView),
                                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return reinterpret_cast<u64>(set);
    }

    void releaseTextureId(u64 id) override {
        if (!active_ || !id) return;
        ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(id));
    }

private:
    VkInstance instance_ = VK_NULL_HANDLE;
    // ImGui holds a POINTER to the format array inside PipelineRenderingCreateInfo, so both outlive
    // init() as members rather than locals.
    VkFormat colorFormat_ = VK_FORMAT_UNDEFINED;
    VkPipelineRenderingCreateInfo renderingInfo_{};
    bool active_ = false;
};

}  // namespace aver::rhi::vkb

namespace aver::rhi::vkb::imgui_backend {
IUiBackend* create() { return new ImGuiVulkanBackend(); }
}  // namespace aver::rhi::vkb::imgui_backend
