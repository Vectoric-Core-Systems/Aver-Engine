// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// A friendly wrapper over the Aver.Settings C ABI (settings_abi.h) -- the FIRST C# binding to it.

namespace Aver.Framework;

/// <summary>Durable per-player key/value storage that outlives any save file -- a save is a world,
/// and settings belong to the player, not to a playthrough (settings_abi.h's own opening comment:
/// "Deleting a save must not reset the volume, and loading one must not change the resolution").
///
/// Nothing in C# has called Aver.Settings before this file. It is what per-player input rebinds
/// (<see cref="EnhancedInput"/>'s bindings) will persist through once a rebind UI exists: an
/// <see cref="InputMappingContext"/> is reconstructed from code every time its owning script spawns,
/// so a player's remapped key has to live somewhere that outlives the C# object that describes the
/// default -- this store, not a save file, is that somewhere.
///
/// Static and stateless from the caller's side, matching <see cref="Input"/>'s own shape: the actual
/// state is the native store's, reached through <see cref="SettingsNative"/> in Native.cs.</summary>
public static class Settings
{
    /// <summary>Opens (or creates) the store at <paramref name="path"/>. Reading a file that does not
    /// exist is not an error -- a first run has no settings and that is the ordinary case. Calling
    /// this again with a different path closes whatever was open WITHOUT flushing it first -- call
    /// <see cref="Flush"/> yourself beforehand if the prior store's pending changes matter.</summary>
    public static bool Open(string path) => SettingsNative.aver_settings_open(path) != 0;

    /// <summary>Opens the engine's own default store (<see cref="DefaultPath"/>). A project that wants
    /// its own settings file should call <see cref="Open"/> with its own path instead -- the engine
    /// does not decide where a project's settings live (settings_abi.h's own comment on
    /// aver_settings_default_path).</summary>
    public static bool OpenDefault() => Open(DefaultPath);

    /// <summary>The default store location: <c>&lt;user data dir&gt;/Aver/settings.ini</c>. Reading
    /// this does not open anything by itself -- see <see cref="OpenDefault"/> for that.</summary>
    public static string DefaultPath => Fw.Str(SettingsNative.aver_settings_default_path());

    /// <summary>Writes the store to disk if anything changed since the last flush. True when the file
    /// is on disk and current -- INCLUDING when nothing needed writing; false only when a write was
    /// actually attempted and failed. The write is atomic (temp file, then rename) on the native side:
    /// settings are small and rewritten often, and a half-written settings file is a game that will
    /// not start (settings_abi.h's own aver_settings_flush comment).</summary>
    public static bool Flush() => SettingsNative.aver_settings_flush() != 0;

    /// <summary>Reads a float, or <paramref name="fallback"/> if the key is missing or does not parse
    /// as a float -- a hand-edited file never reads back as a silent 0 (settings_abi.h's own readers
    /// comment).</summary>
    public static float GetFloat(string key, float fallback = 0f) =>
        SettingsNative.aver_settings_get_f32(key, fallback);

    /// <summary>Reads an int, or <paramref name="fallback"/> if the key is missing or does not parse.</summary>
    public static int GetInt(string key, int fallback = 0) =>
        SettingsNative.aver_settings_get_i32(key, fallback);

    /// <summary>Reads a bool, or <paramref name="fallback"/> if the key is missing or does not parse.</summary>
    public static bool GetBool(string key, bool fallback = false) =>
        SettingsNative.aver_settings_get_bool(key, fallback ? 1 : 0) != 0;

    /// <summary>Reads a string, or <paramref name="fallback"/> if the key is missing. Decoded
    /// immediately -- the returned native pointer is only valid until the next Settings call
    /// (settings_abi.h's own comment), the identical lifetime <see cref="DefaultPath"/> already
    /// respects by copying through <c>Fw.Str</c> before returning.</summary>
    public static string GetString(string key, string fallback = "") =>
        Fw.Str(SettingsNative.aver_settings_get_str(key, fallback));

    /// <summary>Writes a float. True on success, false for an empty key.</summary>
    public static bool SetFloat(string key, float value) => SettingsNative.aver_settings_set_f32(key, value) != 0;

    /// <summary>Writes an int. True on success, false for an empty key.</summary>
    public static bool SetInt(string key, int value) => SettingsNative.aver_settings_set_i32(key, value) != 0;

    /// <summary>Writes a bool. True on success, false for an empty key.</summary>
    public static bool SetBool(string key, bool value) => SettingsNative.aver_settings_set_bool(key, value ? 1 : 0) != 0;

    /// <summary>Writes a string. False (and nothing stored) if <paramref name="key"/> is empty OR
    /// <paramref name="value"/> contains a newline -- the store's format is one key=value line each
    /// with no escaping, so a smuggled newline would silently become a second key
    /// (settings_abi.h's own aver_settings_set_str comment). Callers that need to persist a rebound
    /// key name should never hit this: no <see cref="Key"/> or gamepad enum name contains one.</summary>
    public static bool SetString(string key, string value) => SettingsNative.aver_settings_set_str(key, value) != 0;

    /// <summary>True when the key is present, whatever its stored type.</summary>
    public static bool Has(string key) => SettingsNative.aver_settings_has(key) != 0;

    /// <summary>Removes a key. True if it is gone afterwards -- INCLUDING when it never existed, so
    /// this is a postcondition check, not a "did anything change" flag.</summary>
    public static bool Remove(string key) => SettingsNative.aver_settings_remove(key) != 0;

    /// <summary>How many keys the store holds. Mostly useful for a settings screen that wants to say
    /// "nothing has been changed from its default yet".</summary>
    public static int Count => SettingsNative.aver_settings_count();
}
