// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The ready-made settings screen (graphics, audio, controls) as a game script drives it.
namespace Aver.UI;

/// <summary>One row of the controls page: which binding of which action.</summary>
/// <param name="Action">The input action's name.</param>
/// <param name="Slot">Which binding of that action (0 is the first).</param>
/// <param name="Label">What the player reads.</param>
/// <param name="Rebindable">False draws the row greyed out.</param>
public readonly record struct RebindRow(string Action, int Slot, string Label, bool Rebindable = true);

/// <summary>The input system as the controls page sees it. <c>Aver.Framework.FrameworkRebindProvider</c>
/// is the implementation over <c>EnhancedInput</c>.</summary>
public interface IRebindProvider
{
    /// <summary>The rows to show, in order.</summary>
    IReadOnlyList<RebindRow> Rows { get; }

    /// <summary>The current key for a row, as text ("Space").</summary>
    string BindingText(RebindRow row);

    /// <summary>Rebinds an action's binding to a framework key slot. False leaves it unchanged.</summary>
    bool TryRebind(string action, int slot, int rawKey);

    /// <summary>Restores every binding to the defaults.</summary>
    void ResetToDefaults();

    /// <summary>Persists the bindings.</summary>
    void Save();
}

/// <summary>Where setting values are kept between runs. <c>Aver.Framework.FrameworkSettingsStore</c>
/// is the implementation over <c>Settings</c>.</summary>
public interface ISettingsStore
{
    /// <summary>The stored value, or <paramref name="fallback"/>.</summary>
    float Get(string key, float fallback);
    /// <summary>Stores a value (not necessarily to disk yet).</summary>
    void Set(string key, float value);
    /// <summary>Writes to disk.</summary>
    void Flush();
}

/// <summary>A settings screen: graphics, audio and (given a provider) a controls page that rebinds
/// keys. The screen edits values and raises <see cref="SettingChanged"/>; the game applies them.</summary>
/// <remarks>Needs <see cref="UiSystem.Pump"/> to run each frame. Setting keys: graphics.quality (index),
/// graphics.renderScale, graphics.taa, graphics.vsync, graphics.fullscreen, graphics.uiScale,
/// audio.master, audio.music, audio.sfx, audio.voice. Toggles are 0 or 1. The screen applies
/// graphics.uiScale itself.</remarks>
public sealed class SettingsScreen
{
    private static readonly string[] s_defaultKeys =
    {
        "graphics.quality", "graphics.renderScale", "graphics.taa", "graphics.vsync", "graphics.fullscreen",
        "graphics.uiScale", "audio.master", "audio.music", "audio.sfx", "audio.voice",
    };

    private readonly ISettingsStore? _store;
    private readonly IRebindProvider? _rebind;
    private readonly List<string> _keys = new();
    private readonly Dictionary<string, float> _values = new();
    private bool _subscribed;

    /// <summary>The screen's root widget once <see cref="Build"/> has run.</summary>
    public Widget Root { get; private set; }

    /// <summary>Raised when the player changes a setting, and by <see cref="ApplyAll"/>: key and value.</summary>
    public event Action<string, float>? SettingChanged;

    /// <summary>Creates the screen's model: the default graphics and audio items when
    /// <paramref name="defaultItems"/>, with persisted values read from <paramref name="store"/>.</summary>
    public SettingsScreen(ISettingsStore? store = null, IRebindProvider? rebind = null, bool defaultItems = true)
    {
        _store = store;
        _rebind = rebind;
        if (defaultItems)
        {
            Native.aver_ui_settings_reset_default();
            foreach (string key in s_defaultKeys) Track(key);
        }
        else
        {
            Native.aver_ui_settings_clear();
        }
    }

    private void Track(string key)
    {
        if (!_keys.Contains(key)) _keys.Add(key);
        float v = Native.aver_ui_settings_value(key);
        if (_store != null) v = _store.Get(key, v);
        _values[key] = v;
        Native.aver_ui_settings_set_value(key, v);
    }

    /// <summary>Adds an on/off setting.</summary>
    public SettingsScreen AddToggle(string key, string label, string tab, bool value)
    {
        Native.aver_ui_settings_add(0, key, label, tab, value ? 1f : 0f, 0f, 1f, 0f, string.Empty);
        Track(key);
        return this;
    }

    /// <summary>Adds a slider.</summary>
    public SettingsScreen AddSlider(string key, string label, string tab, float value, float min, float max, float step = 0f)
    {
        Native.aver_ui_settings_add(1, key, label, tab, value, min, max, step, string.Empty);
        Track(key);
        return this;
    }

    /// <summary>Adds a list of options; the value is the chosen index.</summary>
    public SettingsScreen AddChoice(string key, string label, string tab, IEnumerable<string> choices, int index = 0)
    {
        Native.aver_ui_settings_add(2, key, label, tab, index, 0f, 0f, 0f, string.Join('\n', choices));
        Track(key);
        return this;
    }

    /// <summary>The current value of a setting.</summary>
    public float this[string key] => _values.TryGetValue(key, out float v) ? v : 0f;

    /// <summary>Raises <see cref="SettingChanged"/> for every setting with its persisted value, so the
    /// game applies saved settings at startup without opening the screen.</summary>
    public void ApplyAll()
    {
        foreach (string key in _keys)
        {
            if (key == "graphics.uiScale") UiSystem.SetUserScale(_values[key]);
            SettingChanged?.Invoke(key, _values[key]);
        }
    }

    /// <summary>Creates the screen's widgets (a menu root that pauses the game) and starts listening.
    /// Hide it with <c>Root.Visible = false</c> or the Back button.</summary>
    public Widget Build(string rootName = "SettingsScreen")
    {
        if (_rebind != null)
        {
            Native.aver_ui_rebind_clear();
            foreach (RebindRow row in _rebind.Rows)
                Native.aver_ui_rebind_add(row.Action, row.Slot, row.Label, _rebind.BindingText(row), row.Rebindable ? 1 : 0);
        }
        Root = new Widget(Native.aver_ui_settings_build(rootName));
        if (!_subscribed)
        {
            UiSystem.Event += OnEvent;
            _subscribed = true;
        }
        return Root;
    }

    /// <summary>Stops listening. Call when the screen is thrown away.</summary>
    public void Dispose()
    {
        if (_subscribed) UiSystem.Event -= OnEvent;
        _subscribed = false;
    }

    /// <summary>Writes persisted settings and bindings to disk.</summary>
    public void Flush()
    {
        _store?.Flush();
        _rebind?.Save();
    }

    private void OnEvent(UiEvent e)
    {
        if (e.Kind == UiEventKind.Cancel) { Flush(); return; }
        if (e.Kind != UiEventKind.Command) return;

        if (e.Text == "ui.close") { Flush(); return; }

        if (e.Text.StartsWith("setting:", StringComparison.Ordinal))
        {
            string key = e.Text.Substring("setting:".Length);
            _values[key] = e.Value;
            _store?.Set(key, e.Value);
            SettingChanged?.Invoke(key, e.Value);
            return;
        }

        if (_rebind == null) return;

        if (e.Text.StartsWith("rebind:", StringComparison.Ordinal))
        {
            string body = e.Text.Substring("rebind:".Length);
            int colon = body.LastIndexOf(':');
            if (colon <= 0 || !int.TryParse(body.AsSpan(colon + 1), out int slot)) return;
            string action = body.Substring(0, colon);
            if (_rebind.TryRebind(action, slot, e.Index)) RefreshRows();
        }
        else if (e.Text == "rebind.reset")
        {
            _rebind.ResetToDefaults();
            RefreshRows();
        }
    }

    private void RefreshRows()
    {
        if (_rebind == null) return;
        IReadOnlyList<RebindRow> rows = _rebind.Rows;
        for (int i = 0; i < rows.Count; i++)
            Native.aver_ui_rebind_set_binding(i, _rebind.BindingText(rows[i]));
    }
}
