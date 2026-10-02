// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
// Loads a project's INPUT.SCHEME (.ocinput) into a live, rebindable EnhancedInput context.

using Aver.Scripting;

namespace Aver.Framework;

/// <summary>
/// The bridge between a project's INPUT.SCHEME and <see cref="EnhancedInput"/>. A project that
/// authors only Aver Node graphs -- no C# <see cref="InputMappingContext"/> subclass anywhere -- still
/// gets rebindable, Unreal-Enhanced-Input-style controls this way: <see cref="Load"/> reads the file
/// through <c>aver_fw_input_scheme_load</c> (the ONE existing C++ .ocinput parser, OcInput.cpp,
/// exposed across the framework ABI rather than duplicated here), builds an internal
/// <see cref="InputMappingContext"/> from its ACTION/BIND/CONTEXT records, and pushes it exactly the
/// way a hand-written subclass would push itself.
///
/// AT MOST ONE SCHEME CONTEXT IS EVER LIVE THROUGH THIS CLASS. <see cref="Load"/> replaces whatever it
/// last pushed rather than stacking a second one -- a project has one INPUT.SCHEME
/// (OcProject.hpp's own <c>inputScheme</c> field is a single path, not a list) -- and both hosts
/// (contract E) call this again at every Play/session start specifically so a saved .ocinput edit
/// applies on the next run without a restart.
///
/// THE SAME SAVE/LOAD/RESET/REBIND OPERATIONS COVER A C#-PUSHED CONTEXT TOO. Nothing here is
/// special-cased to the scheme context: <see cref="EnhancedInput"/>.SaveAllBindings/LoadAllBindings/
/// ResetAllBindings/RebindAction all walk EVERY pushed context, scheme or hand-written alike, because a
/// project can freely mix both (a C# gameplay system pushing its own context on top of a
/// graph-authored scheme, say) and a rebind menu should not have to know which kind of context it is
/// rebinding.
/// </summary>
public static class InputScheme
{
    // The one scheme context this class has pushed, or null once nothing has been loaded (or the last
    // Load failed). Tracked here rather than read back off EnhancedInput's own stack, because
    // EnhancedInput exposes no "find by some marker" query and does not need one just for this.
    private static SchemeContext? s_current;

    /// <summary>The context the last successful <see cref="Load"/> built, or null.</summary>
    public static InputMappingContext? Current => s_current;

    /// <summary>The last <see cref="Load"/>'s error text; empty after a success.</summary>
    public static string LastError { get; private set; } = string.Empty;

    /// <summary>
    /// Loads <paramref name="path"/> and replaces whatever scheme context this class last pushed.
    /// Returns false, with <see cref="LastError"/> set and the previous scheme removed either way, on
    /// a parse failure -- <c>aver_fw_input_scheme_load</c>'s own contract (framework_abi.h) is that a
    /// failed load leaves nothing behind for this method to build from, so there is no partial scheme
    /// to fall back to.
    ///
    /// A BINDING NAMING AN UNKNOWN KEY/GAMEPAD BUTTON/AXIS IS SKIPPED, NOT FATAL -- logged and dropped,
    /// the same tolerance <see cref="InputMappingContext"/>'s own key-name parsing applies to a stale
    /// saved rebind (EnhancedInput.cs's LoadBindings): a hand-edited .ocinput with one typo'd BIND
    /// still loads every other binding, rather than refusing the whole scheme over it.
    ///
    /// BINDINGS APPLY TWICE, ON PURPOSE. The context is pushed once with the file's own ACTION/BIND
    /// defaults, THEN <see cref="InputMappingContext.LoadBindings"/> overlays whatever the player
    /// already saved for it, THEN it is pushed again so the native action table picks up that overlay
    /// -- the file's own defaults have to already be sitting in
    /// <see cref="InputMappingContext.Bindings"/> before the FIRST LoadBindings call ever touches this
    /// context, because it captures ResetToDefaults' target lazily, on that very first touch
    /// (InputMappingContext's own comment on <c>CaptureDefaultsIfNeeded</c>).
    /// </summary>
    public static bool Load(string path)
    {
        // Drop whatever this class last pushed BEFORE attempting the native parse: a failed load must
        // leave no scheme live, and doing this first satisfies that on either branch below, with no
        // separate "undo" path needed for the failure case.
        Unload();

        if (Fw.aver_fw_input_scheme_load(path) == 0)
        {
            LastError = Fw.Str(Fw.aver_fw_input_scheme_error());
            Log.Warn($"[InputScheme] failed to load '{path}': {LastError}");
            return false;
        }
        LastError = string.Empty;

        string contextName = Fw.Str(Fw.aver_fw_input_scheme_context_name());
        if (string.IsNullOrEmpty(contextName))
            contextName = Path.GetFileNameWithoutExtension(path);
        int priority = Fw.aver_fw_input_scheme_context_priority();

        var ctx = new SchemeContext(contextName);

        int actionCount = Fw.aver_fw_input_scheme_action_count();
        var actions = new InputAction[actionCount];
        for (int i = 0; i < actionCount; i++)
        {
            string name = Fw.Str(Fw.aver_fw_input_scheme_action_name(i));
            actions[i] = (InputValueType)Fw.aver_fw_input_scheme_action_type(i) switch
            {
                InputValueType.Axis1D => InputAction.Axis1D(name),
                InputValueType.Axis2D => InputAction.Axis2D(name),
                _                     => InputAction.Digital(name),
            };
        }

        int bindingCount = Fw.aver_fw_input_scheme_binding_count();
        for (int i = 0; i < bindingCount; i++)
        {
            if (Fw.aver_fw_input_scheme_binding(i, out int actionIndex, out int rawSource, out float scale, out int component) == 0)
                continue;
            if (actionIndex < 0 || actionIndex >= actions.Length) continue;

            var source = (InputSource)rawSource;
            int rawKey = 0;
            // Only Key/GamepadButton/GamepadAxis carry a name to resolve -- the three mouse-axis
            // sources leave the key column empty in the file (OcInput.hpp's own comment) and RawKey
            // unused at read time, the identical placeholder convention BindMouseLook/BindMouseWheel
            // already establish for a hand-written context.
            if (source is InputSource.Key or InputSource.GamepadButton or InputSource.GamepadAxis)
            {
                string keyName = Fw.Str(Fw.aver_fw_input_scheme_binding_key(i));
                if (!TryParseKeyName(source, keyName, out rawKey))
                {
                    Log.Warn($"[InputScheme] '{path}': binding {i} on action '{actions[actionIndex].Name}' names an unknown {source} '{keyName}', skipped");
                    continue;
                }
            }
            ctx.AddBinding(actions[actionIndex], source, rawKey, scale, component);
        }

        EnhancedInput.AddContext(ctx, priority);
        ctx.LoadBindings();
        EnhancedInput.AddContext(ctx, priority);

        s_current = ctx;
        return true;
    }

    /// <summary>Removes the loaded scheme context, if any. Safe to call with nothing loaded.</summary>
    public static void Unload()
    {
        if (s_current is null) return;
        EnhancedInput.RemoveContext(s_current);
        s_current = null;
    }

    // The SAME Enum.TryParse + IsDefined rule InputMappingContext's own TryParseRawKey applies when
    // reading a saved binding back off Settings (EnhancedInput.cs) -- a scheme file's key name gets no
    // more, and no less, tolerance than a hand-edited settings file already does.
    private static bool TryParseKeyName(InputSource source, string name, out int rawKey)
    {
        switch (source)
        {
            case InputSource.GamepadButton:
                if (Enum.TryParse(name, out GamepadButton gb) && Enum.IsDefined(typeof(GamepadButton), gb))
                { rawKey = (int)gb; return true; }
                break;
            case InputSource.GamepadAxis:
                if (Enum.TryParse(name, out GamepadAxis ga) && Enum.IsDefined(typeof(GamepadAxis), ga))
                { rawKey = (int)ga; return true; }
                break;
            default:
                if (Enum.TryParse(name, out Key k) && Enum.IsDefined(typeof(Key), k))
                { rawKey = (int)k; return true; }
                break;
        }
        rawKey = 0;
        return false;
    }

    /// <summary>An <see cref="InputMappingContext"/> built entirely from a loaded .ocinput file's own
    /// records, rather than from a hand-written subclass's protected Bind* calls. Same assembly as the
    /// base class, so <see cref="InputBinding"/>'s internal constructor and
    /// <see cref="InputMappingContext.Bindings"/>'s internal list are both reachable directly here --
    /// no new protected surface needed on the base class just for this one caller.</summary>
    private sealed class SchemeContext : InputMappingContext
    {
        private readonly string _name;
        public override string Name => _name;

        public SchemeContext(string name) => _name = name;

        internal void AddBinding(InputAction action, InputSource source, int rawKey, float scale, int component) =>
            Bindings.Add(new InputBinding(action, source, rawKey, scale, component));
    }
}
