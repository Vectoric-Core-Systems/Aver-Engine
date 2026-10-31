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

    /// <summary>The names of RenderingServer's signals.</summary>
    public static class SignalName
    {
        public static readonly StringName FramePreDraw = "frame_pre_draw";
        public static readonly StringName FramePostDraw = "frame_post_draw";
    }

    private static Signals? s_signals;
    private static long s_drawMark = -1;

    /// <summary>The object that carries RenderingServer's signals (Godot's RenderingServer is an object; here it is a static class).</summary>
    public static GodotObject Singleton => s_signals ??= new Signals();

    private sealed class Signals : RefCounted
    {
        public new class SignalName : GodotObject.SignalName
        {
            public static readonly StringName FramePreDraw = "frame_pre_draw";
            public static readonly StringName FramePostDraw = "frame_post_draw";
        }
    }

    /// <summary>Emitted at the end of a process frame, before the frame is drawn.</summary>
    public static event Action FramePreDraw { add => Singleton.Connect(SignalName.FramePreDraw, Callable.From(value)); remove => Singleton.Disconnect(SignalName.FramePreDraw, Callable.From(value)); }

    /// <summary>Emitted after a frame was drawn. Aver draws after the engine's update, so it comes at the head of the next process frame; a
    /// connection made since the last frame ended waits for the frame after that, so a script that changes the screen and awaits this sees it drawn.</summary>
    public static event Action FramePostDraw { add => Singleton.Connect(SignalName.FramePostDraw, Callable.From(value)); remove => Singleton.Disconnect(SignalName.FramePostDraw, Callable.From(value)); }

    // The tail of a process frame (SceneTree.EndPass): the draw follows. The mark is what a post-draw signal may reach.
    internal static void FrameEnding()
    {
        if (s_signals == null) return;
        s_signals.EmitSignal(SignalName.FramePreDraw);
        s_drawMark = GodotObject.ConnectionSerial;
    }

    // The head of a process frame: the frame before it was drawn.
    internal static void FrameStarting()
    {
        if (s_signals == null || s_drawMark < 0) return;
        long mark = s_drawMark;
        s_drawMark = -1;
        s_signals.EmitSignalThrough(SignalName.FramePostDraw, mark, System.Array.Empty<Variant>());
    }

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
