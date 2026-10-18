// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Game accessibility for scripts and graphs: the option store, captions, speech and camera feel
// (docs/ACCESSIBILITY.md, docs/plans/wave9/ACCESSIBILITY.md). The state lives in Aver.Framework.dll;
// these are thin wrappers, and every call is safe when the native library is absent.

using Aver.Scene;
using Aver.UI;

namespace Aver.Framework;

/// <summary>The colour-vision filter modes. Mirrors <c>access.colour.mode</c>.</summary>
public enum ColourVisionMode
{
    /// <summary>No filter.</summary>
    Off = 0,
    /// <summary>Red-weak vision.</summary>
    Protanopia = 1,
    /// <summary>Green-weak vision.</summary>
    Deuteranopia = 2,
    /// <summary>Blue-weak vision.</summary>
    Tritanopia = 3,
}

/// <summary>What the captions show. Mirrors <c>access.captions.mode</c>.</summary>
public enum CaptionMode
{
    /// <summary>Nothing.</summary>
    Off = 0,
    /// <summary>Dialogue only.</summary>
    Subtitles = 1,
    /// <summary>Dialogue, sound effects and music.</summary>
    Captions = 2,
}

/// <summary>The kind of a caption cue. Mirrors <c>OcCaptionKind</c>.</summary>
public enum CaptionKind
{
    /// <summary>Spoken lines.</summary>
    Dialogue = 0,
    /// <summary>Sound effects.</summary>
    Sound = 1,
    /// <summary>Music.</summary>
    Music = 2,
}

/// <summary>The player's accessibility options: every one is a float under a stable key
/// (<c>access.*</c>, plus <c>graphics.uiScale</c>), clamped and snapped by the native table. Changes persist
/// into <see cref="Settings"/> from <see cref="Tick"/> (flushed after half a second of quiet).</summary>
public static class Accessibility
{
    private static readonly byte[] s_keyBuf = new byte[160];
    private static readonly float[] s_info = new float[6];
    private static float _flushIn = -1f;

    /// <summary>Raised from <see cref="Tick"/>, once per accepted change: key and new value. The same change is
    /// also posted as the bus event <c>Access.Changed</c> (string key, float value) for graphs.</summary>
    public static event Action<string, float>? Changed;

    /// <summary>The value of any key in the table (or a dynamic <c>access.input.toggle.&lt;Action&gt;</c>);
    /// <paramref name="fallback"/> for an unknown key or when the framework is not loaded.</summary>
    public static float Get(string key, float fallback = 0f)
    {
        try { return FwAccess.aver_fw_access_key_info(key, s_info) != 0 ? FwAccess.aver_fw_access_get(key) : fallback; }
        catch (DllNotFoundException) { return fallback; }
    }

    /// <summary>Sets a key, clamped to its range. False for an unknown key.</summary>
    public static bool Set(string key, float value)
    {
        try { return FwAccess.aver_fw_access_set(key, value) != 0; }
        catch (DllNotFoundException) { return false; }
    }

    /// <summary>Puts every option back to its default.</summary>
    public static void ResetToDefaults()
    {
        try { FwAccess.aver_fw_access_reset_defaults(); }
        catch (DllNotFoundException) { }
    }

    /// <summary>Bumps on every accepted change.</summary>
    public static int Revision
    {
        get { try { return FwAccess.aver_fw_access_revision(); } catch (DllNotFoundException) { return 0; } }
    }

    /// <summary>Once a frame (HostBridge group 0): drains the accepted changes into <see cref="Settings"/>, raises
    /// <see cref="Changed"/>, flushes the file after 0.5 s without a change, and runs the speech queue.</summary>
    public static void Tick(float dt)
    {
        try
        {
            while (FwAccess.aver_fw_access_take_dirty(0, s_keyBuf, s_keyBuf.Length, out float value) != 0)
            {
                int n = Array.IndexOf(s_keyBuf, (byte)0);
                string key = System.Text.Encoding.UTF8.GetString(s_keyBuf, 0, n < 0 ? s_keyBuf.Length : n);
                Settings.SetFloat(key, value);
                _flushIn = 0.5f;
                Changed?.Invoke(key, value);
            }
            if (_flushIn >= 0f && (_flushIn -= dt) <= 0f)
            {
                Settings.Flush();
                _flushIn = -1f;
            }
            FwAccess.aver_fw_speech_tick();
        }
        catch (DllNotFoundException) { }
    }

    /// <summary>Writes any pending change to disk now (the settings screen closing).</summary>
    public static void FlushNow()
    {
        Tick(0f);
        if (_flushIn >= 0f) { Settings.Flush(); _flushIn = -1f; }
    }

    private static bool Flag(string key) => Get(key) > 0.5f;
    private static void SetFlag(string key, bool on) => Set(key, on ? 1f : 0f);

    /// <summary>The whole-UI scale, 0.75 to 2.0 (key <c>graphics.uiScale</c>). Applied to the UI by the host.</summary>
    public static float UiScale { get => Get("graphics.uiScale", 1f); set => Set("graphics.uiScale", value); }
    /// <summary>Text-only scale, 0.75 to 2.0.</summary>
    public static float TextScale { get => Get("access.ui.textScale", 1f); set => Set("access.ui.textScale", value); }
    /// <summary>High-contrast UI: the contrast theme, authored colours ignored.</summary>
    public static bool HighContrast { get => Flag("access.ui.highContrast"); set => SetFlag("access.ui.highContrast", value); }
    /// <summary>The colour-vision filter.</summary>
    public static ColourVisionMode ColourVision
    {
        get => (ColourVisionMode)(int)Get("access.colour.mode");
        set => Set("access.colour.mode", (float)(int)value);
    }
    /// <summary>How far the filter moves colour, 0 to 1.</summary>
    public static float ColourStrength { get => Get("access.colour.strength", 1f); set => Set("access.colour.strength", value); }
    /// <summary>Developer preview: show what a colour-blind viewer sees instead of correcting for them.</summary>
    public static bool SimulateColourVision { get => Flag("access.colour.simulate"); set => SetFlag("access.colour.simulate", value); }
    /// <summary>The master "reduce motion" gate: zeroes shake, bob, motion blur and the lens effects while on.</summary>
    public static bool ReduceMotion { get => Flag("access.motion.reduce"); set => SetFlag("access.motion.reduce", value); }
    /// <summary>Camera shake amount, 0 to 1.</summary>
    public static float ShakeScale { get => Get("access.motion.shake", 1f); set => Set("access.motion.shake", value); }
    /// <summary>Head bob amount, 0 to 1.</summary>
    public static float BobScale { get => Get("access.motion.bob", 1f); set => Set("access.motion.bob", value); }
    /// <summary>Whether motion blur is allowed.</summary>
    public static bool MotionBlurAllowed { get => Flag("access.motion.blur"); set => SetFlag("access.motion.blur", value); }
    /// <summary>Whether chromatic aberration, film grain and lens flare are allowed.</summary>
    public static bool LensEffectsAllowed { get => Flag("access.motion.lens"); set => SetFlag("access.motion.lens", value); }

    /// <summary>Input assistance: toggle instead of hold, and timing scales. Assists named actions only
    /// (<see cref="EnhancedInput"/>); raw <c>Input.IsKeyDown</c> reads are not assisted.</summary>
    public static class Input
    {
        private const string Prefix = "access.input.toggle.";

        /// <summary>Whether the action is a toggle (a tap latches it on, another releases it).</summary>
        public static bool IsToggleEnabled(string action) => Flag(Prefix + action);
        /// <summary>Turns toggle-instead-of-hold on or off for a Digital action.</summary>
        public static void SetToggle(string action, bool on) => SetFlag(Prefix + action, on);
        /// <summary>False hides the action from the settings page and ignores a saved toggle (a Pause key, say).</summary>
        public static void SetToggleAllowed(string action, bool allowed)
        {
            try { FwAccess.aver_fw_access_set_toggle_allowed(action, allowed ? 1 : 0); }
            catch (DllNotFoundException) { }
        }
        /// <summary>Whether the action may be a toggle.</summary>
        public static bool IsToggleAllowed(string action)
        {
            try { return FwAccess.aver_fw_access_toggle_allowed(action) != 0; }
            catch (DllNotFoundException) { return true; }
        }
        /// <summary>Multiplies hold times, 0.25 to 1 (shorter holds).</summary>
        public static float HoldScale { get => Get("access.input.holdScale", 1f); set => Set("access.input.holdScale", value); }
        /// <summary>Multiplies tap and combo windows, 1 to 4 (longer windows).</summary>
        public static float WindowScale { get => Get("access.input.windowScale", 1f); set => Set("access.input.windowScale", value); }
    }
}

/// <summary>Subtitles and sound captions (<c>.occaptions</c>). Cues are raised by the sounds the audio ABI plays
/// (nothing to call), by the sequencer's <c>Caption.Show</c> event key, or from here.</summary>
public static class Captions
{
    /// <summary>What the captions show.</summary>
    public static CaptionMode Mode { get => (CaptionMode)(int)Accessibility.Get("access.captions.mode", 1f); set => Accessibility.Set("access.captions.mode", (float)(int)value); }
    /// <summary>0 Small, 1 Medium, 2 Large, 3 Extra large.</summary>
    public static int SizePreset { get => (int)Accessibility.Get("access.captions.size", 1f); set => Accessibility.Set("access.captions.size", value); }
    /// <summary>Background opacity, 0 to 1.</summary>
    public static float BackgroundOpacity { get => Accessibility.Get("access.captions.bgOpacity", 0.6f); set => Accessibility.Set("access.captions.bgOpacity", value); }
    /// <summary>True for the top of the screen, false for the bottom.</summary>
    public static bool TopOfScreen { get => Accessibility.Get("access.captions.position") > 0.5f; set => Accessibility.Set("access.captions.position", value ? 1f : 0f); }
    /// <summary>The most lines shown at once, 1 to 4.</summary>
    public static int MaxLines { get => (int)Accessibility.Get("access.captions.lines", 3f); set => Accessibility.Set("access.captions.lines", value); }
    /// <summary>Show who is speaking.</summary>
    public static bool ShowSpeakerNames { get => Accessibility.Get("access.captions.speaker", 1f) > 0.5f; set => Accessibility.Set("access.captions.speaker", value ? 1f : 0f); }
    /// <summary>Colour lines by speaker.</summary>
    public static bool SpeakerColours { get => Accessibility.Get("access.captions.colours", 1f) > 0.5f; set => Accessibility.Set("access.captions.colours", value ? 1f : 0f); }
    /// <summary>Show a &lt; &gt; v hint for where a positional sound is.</summary>
    public static bool DirectionHints { get => Accessibility.Get("access.captions.direction") > 0.5f; set => Accessibility.Set("access.captions.direction", value ? 1f : 0f); }

    /// <summary>Loads every <c>.occaptions</c> in a content-relative folder, replacing the cue set. Returns the cue
    /// count (-1 for a missing folder). The host already loaded <c>Captions</c> at start.</summary>
    public static int Load(string contentRelativeDir = "Captions")
    {
        try
        {
            string root = FwTable.Str(FwTable.aver_fw_content_root()).TrimEnd('/', '\\');
            return FwAccess.aver_fw_caption_load_dir(root.Length == 0 ? contentRelativeDir : root + "/" + contentRelativeDir);
        }
        catch (DllNotFoundException) { return -1; }
    }

    /// <summary>Shows a cue by id. False when there is no such cue or the mode excludes it.</summary>
    public static bool Show(string cue)
    {
        try { return FwAccess.aver_fw_caption_show(cue, 0, 0f, 0f, 0f) != 0; }
        catch (DllNotFoundException) { return false; }
    }

    /// <summary>Shows a cue from a world position: its radius and the direction hint apply.</summary>
    public static bool ShowAt(string cue, Vec3 position)
    {
        try { return FwAccess.aver_fw_caption_show(cue, 1, position.X, position.Y, position.Z) != 0; }
        catch (DllNotFoundException) { return false; }
    }

    /// <summary>Shows an ad-hoc line with no cue. Speaker and text may be localization references; rgba is
    /// 0xRRGGBBAA (the speaker colour when the colours option is on).</summary>
    public static bool ShowText(string speaker, string text, float seconds = 3f, CaptionKind kind = CaptionKind.Dialogue, uint rgba = 0xFFFFFFFF)
    {
        try { return FwAccess.aver_fw_caption_show_text(speaker ?? string.Empty, text, seconds, rgba, (int)kind) != 0; }
        catch (DllNotFoundException) { return false; }
    }

    /// <summary>Stops a cue's captions, or every caption with null.</summary>
    public static void Stop(string? cue = null)
    {
        try { FwAccess.aver_fw_caption_stop(cue); }
        catch (DllNotFoundException) { }
    }
}

/// <summary>The game talking through the OS voice (SAPI on Windows): narrated menus, captions and script
/// announcements. It is not a screen-reader bridge: NVDA and Narrator do not see the widgets.</summary>
public static class Speech
{
    /// <summary>An OS voice exists. The first call may take a moment.</summary>
    public static bool Available
    {
        get { try { return FwAccess.aver_fw_speech_available() != 0; } catch (DllNotFoundException) { return false; } }
    }
    /// <summary>The master switch (also the settings page's Text to speech).</summary>
    public static bool Enabled { get => Accessibility.Get("access.speech.enabled") > 0.5f; set => Accessibility.Set("access.speech.enabled", value ? 1f : 0f); }
    /// <summary>Read focused menu items aloud.</summary>
    public static bool NarrateUi { get => Accessibility.Get("access.speech.ui", 1f) > 0.5f; set => Accessibility.Set("access.speech.ui", value ? 1f : 0f); }
    /// <summary>Read new caption lines aloud.</summary>
    public static bool NarrateCaptions { get => Accessibility.Get("access.speech.captions") > 0.5f; set => Accessibility.Set("access.speech.captions", value ? 1f : 0f); }
    /// <summary>Speech rate, -10 to 10 (SAPI's range).</summary>
    public static int Rate { get => (int)Accessibility.Get("access.speech.rate"); set => Accessibility.Set("access.speech.rate", value); }
    /// <summary>Speech volume, 0 to 1.</summary>
    public static float Volume { get => Accessibility.Get("access.speech.volume", 1f); set => Accessibility.Set("access.speech.volume", value); }

    /// <summary>Says a string (or a localization reference). False when speech is disabled or unavailable.</summary>
    public static bool Announce(string textOrRef, bool interrupt = false)
    {
        try { return FwAccess.aver_fw_speech_say(textOrRef, interrupt ? 1 : 0) != 0; }
        catch (DllNotFoundException) { return false; }
    }

    /// <summary>Cuts off everything being said and queued.</summary>
    public static void Stop()
    {
        try { FwAccess.aver_fw_speech_stop(); }
        catch (DllNotFoundException) { }
    }

    /// <summary>Whether speech is in progress or queued.</summary>
    public static bool IsSpeaking
    {
        get { try { return FwAccess.aver_fw_speech_speaking() != 0; } catch (DllNotFoundException) { return false; } }
    }
}

/// <summary>Camera shake and head bob. Nothing calls these by itself: a character script calls
/// <see cref="AddShake"/> on impacts and <see cref="SetBob"/> each frame while moving. The player's
/// shake, bob and reduce-motion options scale or zero both.</summary>
public static class CameraFx
{
    /// <summary>Adds shake trauma, 0 to 1; it saturates at 1 and decays on its own.</summary>
    public static void AddShake(float trauma)
    {
        try { FwAccess.aver_fw_camera_shake_add(trauma); }
        catch (DllNotFoundException) { }
    }

    /// <summary>Head bob for this frame: amount 0 to 1 and the ground speed in cm/s. Call each frame; 0 stops it.</summary>
    public static void SetBob(float amount01, float speedCmPerSec)
    {
        try { FwAccess.aver_fw_camera_bob_set(amount01, speedCmPerSec); }
        catch (DllNotFoundException) { }
    }
}

/// <summary><see cref="ISettingsStore"/> that answers every accessibility key (<c>access.*</c> and
/// <c>graphics.uiScale</c>) from <see cref="Accessibility"/> and the rest from <see cref="Settings"/>.
/// The native table clamps and snaps each value; persistence is <see cref="Accessibility.Tick"/>'s.</summary>
public sealed class AccessSettingsStore : ISettingsStore
{
    private readonly FrameworkSettingsStore _base = new();

    /// <summary>True for the keys this store owns.</summary>
    public static bool Owns(string key) => key == "graphics.uiScale" || key.StartsWith("access.", StringComparison.Ordinal);

    /// <inheritdoc/>
    public float Get(string key, float fallback) => Owns(key) ? Accessibility.Get(key, fallback) : _base.Get(key, fallback);
    /// <inheritdoc/>
    public void Set(string key, float value)
    {
        if (Owns(key)) Accessibility.Set(key, value);
        else _base.Set(key, value);
    }
    /// <inheritdoc/>
    public void Flush()
    {
        Accessibility.FlushNow();
        _base.Flush();
    }
}

/// <summary>The graph-node surface for the Accessibility nodes: the same calls with the primitive-only
/// signatures GraphCompiler's emitted IL needs. An empty string means "not given".
/// Palette mirror: sandbox/src/GraphNodeDefsAccessibility.hpp.</summary>
internal static class AccessibilityGraphInterop
{
    internal static bool SetOptionForGraph(string key, float value) => Accessibility.Set(key, value);

    internal static bool GetOptionForGraph(string key, out float value)
    {
        value = 0f;
        try
        {
            if (FwAccess.aver_fw_access_key_info(key, new float[6]) == 0) return false;
            value = FwAccess.aver_fw_access_get(key);
            return true;
        }
        catch (DllNotFoundException) { return false; }
    }

    internal static bool ShowCaptionForGraph(string cue, float x, float y, float z, bool positional) =>
        positional ? Captions.ShowAt(cue, new Vec3(x, y, z)) : Captions.Show(cue);

    internal static bool ShowCaptionTextForGraph(string speaker, string text, float seconds, int kind) =>
        Captions.ShowText(speaker, text, seconds, kind is >= 0 and <= 2 ? (CaptionKind)kind : CaptionKind.Dialogue);

    internal static bool StopCaptionForGraph(string cue)
    {
        Captions.Stop(string.IsNullOrEmpty(cue) ? null : cue);
        return true;
    }

    internal static bool AnnounceForGraph(string text, bool interrupt) => Speech.Announce(text, interrupt);

    internal static bool StopSpeakingForGraph()
    {
        Speech.Stop();
        return true;
    }

    internal static bool IsSpeakingForGraph() => Speech.IsSpeaking;

    internal static bool AddCameraShakeForGraph(float trauma)
    {
        CameraFx.AddShake(trauma);
        return true;
    }

    internal static bool SetCameraBobForGraph(float amount, float speed)
    {
        CameraFx.SetBob(amount, speed);
        return true;
    }

    internal static bool SetActionToggleAllowedForGraph(string action, bool allowed)
    {
        if (string.IsNullOrEmpty(action)) return false;
        Accessibility.Input.SetToggleAllowed(action, allowed);
        return true;
    }
}
