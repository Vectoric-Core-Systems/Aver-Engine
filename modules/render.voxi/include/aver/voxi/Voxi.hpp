// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
#pragma once
#include "aver/core/Types.hpp"

// Voxi's render-feature settings: anti-aliasing, global illumination, ray tracing, path tracing.
// Core-only, no RHI dependency: the host pushes device capabilities in and reads settings back out.
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

// Multisample count.
enum class Msaa : u32 { Off = 1, X2 = 2, X4 = 4, X8 = 8 };

// Shared quality ladder for the trace-based features. Off means "don't run this pass".
enum class Quality : u32 { Off = 0, Low = 1, Medium = 2, High = 3, Epic = 4 };

// The features Voxi owns settings for.
enum class Feature : u32 { Msaa = 0, GlobalIllumination, RayTracing, PathTracing, MeshShaders, Count };

// Whether a feature is usable: implemented and supported, declared but not implemented yet, or
// refused by the device.
enum class Status : u32 {
    Ready = 0,
    NotImplemented,
    Unsupported,
};

// What the host's GPU can do. Pushed in by the runtime; mirrors rhi::DeviceCaps.
struct DeviceInfo {
    u32 msaaMask = 1;         // bit N set => N samples supported
    u32 maxMsaaSamples = 1;
    u32 rayTracingTier = 0;   // 0 none, 10 DXR 1.0, 11 DXR 1.1
    bool computeShaders = false;
    bool typedUavLoads = false;
    bool conservativeRaster = false;
    u32 shaderModel = 50;      // 60 = SM 6.0, 65 = SM 6.5
    u32 meshShaderTier = 0;    // 0 = none, 1 = Tier 1
    bool dxcAvailable = false; // DXIL compiler present
};

// The renderer quality settings, as requested. Clamped to the device by Renderer::setSettings.
struct Settings {
    Msaa    msaa               = Msaa::X4;
    Quality globalIllumination = Quality::Medium;
    Quality rayTracing         = Quality::Off;
    Quality pathTracing        = Quality::Off;
    bool    meshShaders        = false;

    // Cubic voxel grid edge; the volume's memory and per-voxel GPU cost are both O(this^3). Defaults
    // to Medium's rung (128) below. Renderer::setSettings derives this from globalIllumination
    // whenever the tier changes and this field arrives unchanged -- see voxelResolutionForQuality
    // and setSettings. Set it explicitly (a different value than what's currently active, in the
    // same call that changes the tier) to override the tier's rung.
    u32 voxelResolution = 128;
    f32 giIntensity     = 1.0f;
    f32 giMaxDistance   = 4000.0f;  // centimetres

    // ---- ray-traced sun shadow: rays per trace, and how many pixels amortise one trace ----
    u32 rtShadowRays = 4;           // occlusion rays per pixel, when this pixel traces this frame.
                                     // Clamped to [1, VoxiRenderer::kMaxShadowRays].
    // Edge length of the square tile a single traced pixel is amortised over via the ray-traced
    // shadow's temporal history: 1 = every pixel traces every frame (bit-identical to no denoiser
    // at all); N>1 = one pixel in each NxN tile traces per frame, rotating which one so every pixel
    // gets its own turn every N*N frames, and every OTHER pixel reuses a reprojected history sample
    // instead of tracing. MUST be a power of two -- VoxiRenderer::setPixelsPerRayTile rounds to the
    // nearest one -- so the per-pixel schedule is a bitmask against the pixel coordinate rather than
    // a modulo, and the pixel COUNT one ray covers (N*N) is a clean power of two throughout: 1, 4,
    // 16, 64, 256 for tile edges 1, 2, 4, 8, 16. Clamped to [1, VoxiRenderer::kMaxPixelsPerRayTile].
    // NOTE: this governs the RT (DXR RayQuery) sun-shadow/reflection history and only has any effect
    // while rayTracing != Quality::Off; it does nothing to the voxel cone-trace GI cost below, which
    // is governed instead by giUpdateInterval.
    u32 rtPixelsPerRayTile = 1;

    // How many frames apart the GI volume is re-voxelised: 1 (the default) revoxelises and re-filters
    // every frame, identical to the original always-fresh behaviour. N>1 reuses the previous frame's
    // voxelised+filtered volume for the N-1 frames in between, amortising the voxelise-rasterise pass
    // and the mip filter chain (VoxiRenderer::voxelizePass / filterMips) at the cost of the indirect
    // lighting lagging scene changes by up to N-1 frames -- a visible latency trade, not a resolution
    // one, and the reason the default keeps it off. Clamped to [1, VoxiRenderer::kMaxGiUpdateInterval].
    u32 giUpdateInterval = 1;
};

// Process-wide settings service. Single instance shared by the editor, the runtime and the C ABI.
class AVER_VOXI_API Renderer {
public:
    // Returns the process-wide instance.
    static Renderer& get();

    // Records what the device can do and re-clamps the current settings against it.
    void setDeviceInfo(const DeviceInfo& info);
    const DeviceInfo& deviceInfo() const { return device_; }

    const Settings& settings() const { return settings_; }
    // Applies what is legal for this device; unsupported requests are clamped, not silently kept.
    void setSettings(const Settings& s);

    // Returns whether a feature is usable on this device.
    Status status(Feature f) const;
    // Returns a readable reason for a feature's status.
    const char* statusText(Feature f) const;
    bool available(Feature f) const { return status(f) == Status::Ready; }

    // Returns true once after settings.msaa changes, then clears the flag.
    bool consumeMsaaDirty();

    // Returns a feature's display name.
    static const char* featureName(Feature f);
    // Returns a quality level's display name.
    static const char* qualityName(Quality q);
    // Returns the voxel grid edge a GI quality tier resolves to when setSettings derives
    // voxelResolution from a tier change -- see setSettings and Voxi.cpp for the ladder and why it
    // only ever applies when the caller left voxelResolution untouched.
    static u32 voxelResolutionForQuality(Quality q);

private:
    Renderer() = default;
    Settings settings_{};
    DeviceInfo device_{};
    bool msaaDirty_ = true;
    u32 refusalLogged_ = 0;   // one bit per Feature: its refusal has already been logged
};

} // namespace aver::voxi
