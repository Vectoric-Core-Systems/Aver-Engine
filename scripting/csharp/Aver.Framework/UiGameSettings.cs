// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Wires the ready-made UI settings screen (Aver.UI) to the engine's own systems: Settings for
// persistence, Audio for the volume sliders, EnhancedInput for key rebinding.
using Aver.UI;

namespace Aver.Framework;

/// <summary><see cref="ISettingsStore"/> over <see cref="Settings"/>: values are stored as floats
/// under their setting key, flushed when the screen closes.</summary>
public sealed class FrameworkSettingsStore : ISettingsStore
{
    /// <inheritdoc/>
    public float Get(string key, float fallback) => Settings.GetFloat(key, fallback);
    /// <inheritdoc/>
    public void Set(string key, float value) => Settings.SetFloat(key, value);
    /// <inheritdoc/>
    public void Flush() => Settings.Flush();
}

/// <summary><see cref="IRebindProvider"/> over <see cref="EnhancedInput"/>: shows the actions a game
/// lists and rebinds their keyboard and mouse slots.</summary>
public sealed class FrameworkRebindProvider : IRebindProvider
{
    private readonly List<RebindRow> _rows;

    /// <summary>Shows exactly these rows.</summary>
    public FrameworkRebindProvider(IEnumerable<RebindRow> rows) => _rows = rows.ToList();

    /// <summary>One row for every binding in <paramref name="context"/>, in order. Only keyboard and
    /// mouse-button bindings are rebindable from the screen; the others are shown greyed out.</summary>
    public static FrameworkRebindProvider FromContext(InputMappingContext context)
    {
        var rows = new List<RebindRow>();
        var slotOf = new Dictionary<string, int>();
        foreach (InputBinding b in context.Bindings)
        {
            if (b.Source is InputSource.MouseX or InputSource.MouseY or InputSource.MouseWheel) continue;
            slotOf.TryGetValue(b.Action.Name, out int slot);
            slotOf[b.Action.Name] = slot + 1;
            string label = slot == 0 ? b.Action.Name : $"{b.Action.Name} ({slot + 1})";
            rows.Add(new RebindRow(b.Action.Name, slot, label, b.Source == InputSource.Key));
        }
        return new FrameworkRebindProvider(rows);
    }

    /// <inheritdoc/>
    public IReadOnlyList<RebindRow> Rows => _rows;

    /// <inheritdoc/>
    public string BindingText(RebindRow row)
    {
        if (!EnhancedInput.TryGetBindingKey(row.Action, row.Slot, out int rawKey, out InputSource source)) return "Unbound";
        return source switch
        {
            InputSource.GamepadButton => Enum.IsDefined(typeof(GamepadButton), rawKey) ? ((GamepadButton)rawKey).ToString() : "?",
            InputSource.GamepadAxis => Enum.IsDefined(typeof(GamepadAxis), rawKey) ? ((GamepadAxis)rawKey).ToString() : "?",
            _ => Enum.IsDefined(typeof(Key), rawKey) ? KeyLabel((Key)rawKey) : "?",
        };
    }

    private static string KeyLabel(Key k) => k switch
    {
        >= Key.D0 and <= Key.D9 => ((int)k - (int)Key.D0).ToString(),
        Key.MouseLeft => "Mouse Left",
        Key.MouseRight => "Mouse Right",
        Key.MouseMiddle => "Mouse Middle",
        Key.LeftShift => "Shift",
        Key.LeftCtrl => "Ctrl",
        Key.LeftAlt => "Alt",
        _ => k.ToString(),
    };

    /// <inheritdoc/>
    public bool TryRebind(string action, int slot, int rawKey) => EnhancedInput.RebindAction(action, slot, rawKey);

    /// <inheritdoc/>
    public void ResetToDefaults() => EnhancedInput.ResetAllBindings();

    /// <inheritdoc/>
    public void Save() => EnhancedInput.SaveAllBindings();
}

/// <summary>The engine's default wiring of the settings screen. Audio keys apply to <see cref="Audio"/>
/// here; graphics keys are stored and handed to <see cref="GraphicsChanged"/> for the game to apply,
/// because what "quality" means is the game's call.</summary>
public static class UiGameSettings
{
    /// <summary>Raised for a graphics.* setting that changed (or was restored by ApplyAll): key and value.
    /// Nothing in the engine applies these yet; a game subscribes and drives its own render settings.</summary>
    public static event Action<string, float>? GraphicsChanged;

    /// <summary>Creates the screen over <see cref="Settings"/> and <see cref="Audio"/>. Pass the
    /// controls page's rows (or <see cref="FrameworkRebindProvider.FromContext"/>) to include a
    /// Controls tab. Call <see cref="SettingsScreen.ApplyAll"/> once at startup to restore saved values.</summary>
    public static SettingsScreen Create(IRebindProvider? rebind = null)
    {
        var screen = new SettingsScreen(new FrameworkSettingsStore(), rebind);
        screen.SettingChanged += Apply;
        return screen;
    }

    private static void Apply(string key, float value)
    {
        switch (key)
        {
            case "audio.master": Audio.MasterVolume = value; break;
            case "audio.music": Audio.SetBusVolume(Bus.Music, value); break;
            case "audio.sfx": Audio.SetBusVolume(Bus.Sfx, value); break;
            case "audio.voice": Audio.SetBusVolume(Bus.Voice, value); break;
            default:
                if (key.StartsWith("graphics.", StringComparison.Ordinal)) GraphicsChanged?.Invoke(key, value);
                break;
        }
    }
}
