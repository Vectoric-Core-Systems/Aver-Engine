// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Widget animation as a game script sees it: the animations a layout's ANIM records define.
namespace Aver.UI;

/// <summary>Plays the animations of an <c>.ocui</c> layout (its <c>ANIM</c> records): tracks over a widget's opacity, colours
/// and render transform. The values sit over the widget's properties and never change them, so stopping an animation brings
/// the authored look back. <paramref name="root"/> is the widget <see cref="UiSystem.OpenLayout(string, bool)"/> returned; an
/// animation of a nested layout is named "&lt;instance&gt;/&lt;name&gt;". A finished animation raises
/// <see cref="UiEventKind.AnimationFinished"/> and keeps its last values until released with <see cref="Stop"/>.</summary>
public static class UiAnimation
{
    /// <summary>The widget ABI version the native library reports (2 and up has animation).</summary>
    public static int AbiVersion => Native.aver_ui_widget_abi_version();

    /// <summary>The names of the layout's animations.</summary>
    public static IReadOnlyList<string> List(Widget root)
    {
        var names = new List<string>();
        int n = Native.aver_ui_anim_count(root.Id);
        for (int i = 0; i < n; i++) names.Add(Native.Utf8(Native.aver_ui_anim_name(root.Id, i)));
        return names;
    }

    /// <summary>The animation's length in seconds, -1 for an unknown one.</summary>
    public static float Length(Widget root, string name) => Native.aver_ui_anim_length(root.Id, name ?? string.Empty);

    /// <summary>Starts an animation from its start (its end when <paramref name="reverse"/>), replacing a run of the same name.
    /// <paramref name="loops"/> is how many times it plays, 0 until stopped; <paramref name="pingPong"/> turns round at each
    /// end instead of jumping back. False for an unknown root or animation.</summary>
    public static bool Play(Widget root, string name, float speed = 1f, int loops = 1, bool reverse = false, bool pingPong = false)
        => Native.aver_ui_anim_play(root.Id, name ?? string.Empty, speed, loops, reverse ? 1 : 0, pingPong ? 1 : 0) != 0;

    /// <summary>Stops it. With <paramref name="release"/> the widgets draw as authored again.</summary>
    public static bool Stop(Widget root, string name, bool release = true)
        => Native.aver_ui_anim_stop(root.Id, name ?? string.Empty, release ? 1 : 0) != 0;

    /// <summary>Jumps to a time (clamped) and applies it; an animation that was not running stays paused there.</summary>
    public static bool Seek(Widget root, string name, float seconds)
        => Native.aver_ui_anim_seek(root.Id, name ?? string.Empty, seconds) != 0;

    /// <summary>Flips a running animation's direction; one that is not running starts from its end, backwards.</summary>
    public static bool Reverse(Widget root, string name) => Native.aver_ui_anim_reverse(root.Id, name ?? string.Empty) != 0;

    /// <summary>Pauses or resumes a running animation.</summary>
    public static bool Pause(Widget root, string name, bool paused = true)
        => Native.aver_ui_anim_pause(root.Id, name ?? string.Empty, paused ? 1 : 0) != 0;

    /// <summary>Whether it is running and not paused.</summary>
    public static bool IsPlaying(Widget root, string name) => Native.aver_ui_anim_is_playing(root.Id, name ?? string.Empty) != 0;

    /// <summary>Where a running animation is, in seconds; -1 when it is not running.</summary>
    public static float Time(Widget root, string name) => Native.aver_ui_anim_time(root.Id, name ?? string.Empty);
}
