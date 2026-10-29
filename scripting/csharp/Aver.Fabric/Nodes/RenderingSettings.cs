// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The few RenderingServer and DisplayServer calls a converted game makes to adapt itself to the renderer it runs on. Aver draws with its own
// renderer, so these answer for it and keep the settings; none of them changes a frame.

namespace Aver.Fabric;

public static partial class RenderingServer
{
    /// <summary>Godot's RenderingServer.ShadowQuality (SHADOW_QUALITY_*).</summary>
    public enum ShadowQuality { Hard = 0, SoftVeryLow = 1, SoftLow = 2, SoftMedium = 3, SoftHigh = 4, SoftUltra = 5, Max = 6 }

    /// <summary>The rendering method in Godot's spelling. Aver's renderer is a ray-traced, clustered one with no compatibility path, so the answer is
    /// "forward_plus": a script that works around the OpenGL compatibility renderer ("gl_compatibility") skips its workaround.</summary>
    public static string GetCurrentRenderingMethod() => "forward_plus";

    /// <summary>The graphics API name in Godot's spelling ("vulkan", "d3d12" or "metal"); Aver picks by platform.</summary>
    public static string GetCurrentRenderingDriverName() => OperatingSystem.IsWindows() ? "d3d12" : OperatingSystem.IsMacOS() ? "metal" : "vulkan";

    private static ShadowQuality s_directionalFilter = ShadowQuality.SoftMedium, s_positionalFilter = ShadowQuality.SoftMedium;

    /// <summary>Kept as set. The sun's shadow filter width follows its light_angular_distance; this value does not change it.</summary>
    public static void DirectionalSoftShadowFilterSetQuality(ShadowQuality quality) => s_directionalFilter = quality;

    /// <summary>Kept as set. A local light's filter width follows its light_size.</summary>
    public static void PositionalSoftShadowFilterSetQuality(ShadowQuality quality) => s_positionalFilter = quality;

    /// <summary>The value last given to <see cref="DirectionalSoftShadowFilterSetQuality"/> (Godot has no getter; tests and tools read it).</summary>
    internal static ShadowQuality DirectionalSoftShadowFilter => s_directionalFilter;
    internal static ShadowQuality PositionalSoftShadowFilter => s_positionalFilter;
}

/// <summary>Godot's DisplayServer singleton, as far as game scripts use it to ask what device they run on.</summary>
public static class DisplayServer
{
    /// <summary>Whether the device has a touch screen. Aver's input chain has no touch source yet, so this is false; a script that turns its on-screen
    /// controls on only for a touch screen leaves them off.</summary>
    public static bool IsTouchscreenAvailable() => false;
}
