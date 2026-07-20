#pragma once
#include "aver/core/Types.hpp"

// Voxi — Aver Engine's optional render-feature module.
//
// Voxi owns the renderer's *quality settings* (anti-aliasing, global illumination, ray tracing,
// path tracing) and decides, from the device's capabilities, which of them are actually usable.
// It deliberately knows nothing about the RHI: the host pushes capabilities in and reads the
// settings back out to drive the device. That keeps this module a leaf (Core only) so it can
// ship as a shared library the C# scripting layer binds to.
//
// The module is optional: build with -DAVER_MODULE_VOXI=OFF and the engine runs without it.
namespace aver::voxi {

#if defined(_WIN32)
#  if defined(AVER_VOXI_BUILD)
#    define AVER_VOXI_API __declspec(dllexport)
#  else
#    define AVER_VOXI_API __declspec(dllimport)
#  endif
#else
#  define AVER_VOXI_API
#endif

enum class Msaa : u32 { Off = 1, X2 = 2, X4 = 4, X8 = 8 };

// Shared quality ladder for the trace-based features. Off means "don't run this pass".
enum class Quality : u32 { Off = 0, Low = 1, Medium = 2, High = 3, Epic = 4 };

enum class Feature : u32 { Msaa = 0, GlobalIllumination, RayTracing, PathTracing, Count };

// Why a feature can or cannot be used right now. Reported honestly so the editor never
// advertises something that will silently do nothing.
enum class Status : u32 {
    Ready = 0,        // implemented here AND supported by the device
    NotImplemented,   // Voxi declares the setting, the renderer cannot do it yet
    Unsupported,      // the device/driver cannot do it at all
};

// What the host's GPU can do. Pushed in by the runtime (mirrors rhi::DeviceCaps) so this
// module needs no RHI dependency.
struct DeviceInfo {
    u32 msaaMask = 1;         // bit N set => N samples supported
    u32 maxMsaaSamples = 1;
    u32 rayTracingTier = 0;   // 0 none, 10 DXR 1.0, 11 DXR 1.1
    bool computeShaders = false;
    bool typedUavLoads = false;
    bool conservativeRaster = false;
};

struct Settings {
    Msaa    msaa               = Msaa::X4;
    Quality globalIllumination = Quality::Off;
    Quality rayTracing         = Quality::Off;
    Quality pathTracing        = Quality::Off;

    // Voxel-cone-traced GI tunables (used when globalIllumination != Off).
    u32 voxelResolution = 128;      // cubic voxel grid edge (64/128/256)
    f32 giIntensity     = 1.0f;     // indirect bounce multiplier
    f32 giMaxDistance   = 4000.0f;  // cone trace range, centimetres
};

// Process-wide settings service. Single instance so the editor, the runtime and the C ABI all
// see the same state.
class AVER_VOXI_API Renderer {
public:
    static Renderer& get();

    void setDeviceInfo(const DeviceInfo& info);
    const DeviceInfo& deviceInfo() const { return device_; }

    const Settings& settings() const { return settings_; }
    // Applies what is legal for this device; unsupported requests are clamped, not silently kept.
    void setSettings(const Settings& s);

    Status status(Feature f) const;
    const char* statusText(Feature f) const;   // human-readable reason, for the editor + logs
    bool available(Feature f) const { return status(f) == Status::Ready; }

    // True when the caller still needs to push settings.msaa to the device.
    bool consumeMsaaDirty();

    static const char* featureName(Feature f);
    static const char* qualityName(Quality q);

private:
    Renderer() = default;
    Settings settings_{};
    DeviceInfo device_{};
    bool msaaDirty_ = true;
};

} // namespace aver::voxi
