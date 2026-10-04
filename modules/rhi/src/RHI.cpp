// RHI entry points: backend selection, device creation, the capability clamp, and UI message
// routing.
#include "aver/rhi/RHI.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

namespace aver::rhi {

// Human-readable name for a backend.
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

// Creates one backend, or nullptr when it is not compiled in.
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

// Parses a backend name. Case-insensitive, and the spellings match what backendName() prints, so a
// log line can be pasted straight back in as a flag.
bool parseBackendName(const char* name, Backend& out) {
    if (!name) return false;
    std::string s;
    for (const char* p = name; *p; ++p) s.push_back(static_cast<char>(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p));
    if (s == "d3d12" || s == "dx12") { out = Backend::D3D12;  return true; }
    if (s == "d3d11" || s == "dx11") { out = Backend::D3D11;  return true; }
    if (s == "vulkan" || s == "vk")  { out = Backend::Vulkan; return true; }
    if (s == "null")                 { out = Backend::Null;   return true; }
    return false;
}

// Creates the first backend in the preference order that initialises, falling back to Null.
IDevice* createDevice(const DeviceDesc& desc) {
    const u32 count = desc.preferredCount < 4 ? desc.preferredCount : 4;
    for (u32 i = 0; i < count; ++i) {
        const Backend b = desc.preferred[i];
        if (IDevice* dev = tryBackend(b, desc)) {
            // Says which was ASKED FOR first as well as which was got, because a silent fall
            // through to D3D12 is exactly how a Vulkan run gets mistaken for a Vulkan run.
            if (i == 0) AVER_INFO("[RHI] device created (backend={})", backendName(dev->backend()));
            else        AVER_WARN("[RHI] device created (backend={}), but {} was preferred and was "
                                  "unavailable -- this run is NOT using the backend that was asked for",
                                  backendName(dev->backend()), backendName(desc.preferred[0]));
            return dev;
        }
        if (b != Backend::Null) {
            AVER_TRACE("[RHI] backend {} unavailable, trying next", backendName(b));
        }
    }
    AVER_WARN("[RHI] no preferred backend available; using Null");
    return detail::createNullDevice(desc);
}

// Destroys a device created by createDevice.
void destroyDevice(IDevice* device) { delete device; }

// ----- Capability clamp ---------------------------------------------------------------------
namespace {
CapsOverride g_capsOverride;

// True for the two-digit shader-model codes this engine reports.
bool validShaderModel(u32 sm) { return sm == 51 || sm == 60 || sm == 61 || sm == 65 || sm == 66; }
}

// The active capability override.
const CapsOverride& capsOverride() { return g_capsOverride; }

// See the declaration for why this is not a CapsOverride field.
u32 g_simulatedDeviceLoss = 0;
void setSimulatedDeviceLoss(u32 afterPresentedFrames) { g_simulatedDeviceLoss = afterPresentedFrames; }
u32  simulatedDeviceLoss() { return g_simulatedDeviceLoss; }

// Parses the --force-caps token list. Returns false and applies nothing on any bad token.
bool setCapsOverride(const char* list) {
    if (!list || !*list) return true;
    CapsOverride o;
    o.active = true;
    bool ok = true;
    const std::string all(list);
    for (usize b = 0; b <= all.size();) {
        const usize e = std::min(all.find(',', b), all.size());
        const std::string t = all.substr(b, e - b);
        b = e + 1;
        if (t.empty()) continue;
        if      (t == "no-rt")           o.noRayTracing = true;
        else if (t == "no-ms")           o.noMeshShaders = true;
        else if (t == "no-cons-raster")  o.noConservativeRaster = true;
        else if (t == "no-typed-uav")    o.noTypedUavLoads = true;
        else if (t == "no-dxc")          o.noDxc = true;
        else if (t == "tier1")           o.maxResourceBindingTier = 1;
        else if (t.rfind("sm=", 0) == 0) {
            const u32 sm = static_cast<u32>(std::atoi(t.c_str() + 3));
            if (!validShaderModel(sm)) { AVER_ERROR("[RHI] --force-caps: '{}' is not a shader model this engine reports (51/60/61/65/66)", t); ok = false; }
            else o.maxShaderModel = sm;
        } else if (t.rfind("msaa=", 0) == 0) {
            const u32 n = static_cast<u32>(std::atoi(t.c_str() + 5));
            if (n != 1 && n != 2 && n != 4 && n != 8) { AVER_ERROR("[RHI] --force-caps: '{}' is not a sample count (1/2/4/8)", t); ok = false; }
            else o.maxMsaaSamples = n;
        } else {
            AVER_ERROR("[RHI] --force-caps: unrecognised token '{}'", t);
            ok = false;
        }
    }
    if (!ok) return false;
    g_capsOverride = o;
    AVER_INFO("[RHI] capability override active: {}", list);
    return true;
}

// Applies the active override to a queried device. Monotonically reducing.
void clampCaps(DeviceCaps& c) {
    const CapsOverride& o = g_capsOverride;
    if (!o.active) return;
    const DeviceCaps hw = c;

    if (o.noRayTracing)          c.rayTracingTier = 0;
    if (o.noMeshShaders)         c.meshShaderTier = 0;
    if (o.noConservativeRaster)  c.conservativeRaster = false;
    if (o.noTypedUavLoads)       c.typedUavLoads = false;
    if (o.noDxc)                 c.dxcAvailable = false;
    if (o.maxShaderModel && c.shaderModel > o.maxShaderModel) c.shaderModel = o.maxShaderModel;
    if (o.maxResourceBindingTier && c.resourceBindingTier > o.maxResourceBindingTier)
        c.resourceBindingTier = o.maxResourceBindingTier;
    if (o.maxMsaaSamples) {
        // The mask is a set of sample counts, so bits above the ceiling are cleared, not lowered.
        for (u32 s : {2u, 4u, 8u}) if (s > o.maxMsaaSamples) c.msaaMask &= ~s;
        if (c.maxMsaaSamples > o.maxMsaaSamples) c.maxMsaaSamples = o.maxMsaaSamples;
    }
    // Without DXC there is no DXIL, so SM 6.x and both SM6-only features are unreachable.
    if (!c.dxcAvailable) {
        c.shaderModel = 51;
        c.meshShaderTier = 0;
        if (c.rayTracingTier > 0) c.rayTracingTier = 0;  // RayQuery needs SM 6.5
    }
    if (c.shaderModel < 65) { c.meshShaderTier = 0; c.rayTracingTier = 0; }

    // An override may only ever subtract.
    if (c.rayTracingTier > hw.rayTracingTier)             c.rayTracingTier = hw.rayTracingTier;
    if (c.meshShaderTier > hw.meshShaderTier)             c.meshShaderTier = hw.meshShaderTier;
    if (c.shaderModel > hw.shaderModel)                   c.shaderModel = hw.shaderModel;
    if (c.maxMsaaSamples > hw.maxMsaaSamples)             c.maxMsaaSamples = hw.maxMsaaSamples;
    if (c.resourceBindingTier > hw.resourceBindingTier)   c.resourceBindingTier = hw.resourceBindingTier;
    c.msaaMask &= hw.msaaMask;
    c.conservativeRaster = c.conservativeRaster && hw.conservativeRaster;
    c.typedUavLoads      = c.typedUavLoads && hw.typedUavLoads;
    c.dxcAvailable       = c.dxcAvailable && hw.dxcAvailable;
    c.computeShaders     = c.computeShaders && hw.computeShaders;
}

// ----- UI window-message routing -------------------------------------------------------------
namespace {
UiWndProcFn g_uiWndProc = nullptr;
}
// Registers the handler a backend hosting ImGui wants raw window messages sent to.
void registerUiWndProc(UiWndProcFn fn) { g_uiWndProc = fn; }
// Forwards one window message to the registered handler. False when there is none.
bool uiWndProc(void* hwnd, u32 msg, u64 wparam, i64 lparam) {
    return g_uiWndProc ? g_uiWndProc(hwnd, msg, wparam, lparam) : false;
}

} // namespace aver::rhi
