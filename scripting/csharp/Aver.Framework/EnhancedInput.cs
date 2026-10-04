// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Action-mapped input: named actions, stacked mapping contexts, and the layer that binds them onto
// the native action ABI.
//
// WHY THE IMPLEMENTATION MOVED DOWN A LAYER. Until now, everything in this file -- InputAction's
// accumulated value, InputMappingContext's priority-stacked consumption, the 0.15 dead zone -- was
// pure C#, computed by an internal Update() this class alone drove. It was complete, documented,
// and had ZERO CONSUMERS: a graph node has no way to call a C# static method, and neither does a C++
// system, so nothing outside a hand-written script could ever ask "is Jump held" without itself
// being C#. A well-designed layer nobody but its own author can reach is a liability (someone
// maintains it) without being an asset (nothing uses it) -- the exact framing the ABI's own header
// comment uses for this section (framework_abi.h's NAMED ACTIONS section, minor 5).
//
// The fix ported the ALGORITHM verbatim onto aver_fw_action_* (FrameworkAbi.cpp) -- same dead zone,
// same scale/component accumulation, same strictly-higher-priority key consumption -- and this file
// is now a THIN WRAPPER over that: every field this file used to own (Raw, Prev, the consumed-key
// set, the per-frame Update() that recomputed them) is gone, not moved, because the native side
// recomputes held/pressed/released/value2 ON DEMAND straight from InputState's cur/prev, the same
// bytes aver_fw_input_key already reads. There is nothing left in C# to roll once a frame -- see
// HostBridge.cs's DispTickAll for where that call used to sit and why it was deleted outright rather
// than kept as a no-op.

using System;
using System.Collections.Generic;
using Aver.Scene;

namespace Aver.Framework;

/// <summary>What shape of value an <see cref="InputAction"/> carries. Values are pinned to the
/// framework's AVER_FW_ACTION_* enum (framework_abi.h) -- cast directly to int when calling
/// aver_fw_action_register, never through a lookup table.</summary>
public enum InputValueType
{
    /// <summary>On or off — a button.</summary>
    Digital,
    /// <summary>One axis, -1..1.</summary>
    Axis1D,
    /// <summary>Two axes, carried in X and Y of a <see cref="Vec3"/>.</summary>
    Axis2D,
}

/// <summary>Where one binding reads from. Values are pinned to the framework's AVER_FW_ACTION_SRC_*
/// enum (framework_abi.h) for the identical reason <see cref="InputValueType"/>'s are.</summary>
public enum InputSource
{
    /// <summary>A key or mouse button, held = 1.</summary>
    Key,
    /// <summary>Mouse movement this frame, in pixels.</summary>
    MouseX,
    /// <summary>Mouse movement this frame, in pixels.</summary>
    MouseY,
    /// <summary>Wheel notches this frame.</summary>
    MouseWheel,
    /// <summary>A gamepad button on pad 0, held = 1.</summary>
    GamepadButton,
    /// <summary>A gamepad axis (a thumbstick half or a trigger) on pad 0 -- raw value, no dead zone
    /// applied at this layer (the 0.15 action dead zone still gates Held/WasPressed/WasReleased, the
    /// same as it does for any other axis-sourced binding).</summary>
    GamepadAxis,
}

/// <summary>A named thing the player can do — "Jump", "Fire", "Move" — independent of any key. A THIN
/// WRAPPER over a native aver_fw_action_* handle; see this file's own top-of-file comment for why the
/// value it reads is no longer stored here at all.</summary>
public sealed class InputAction
{
    /// <summary>Display name, used in logs and as the native registry key.</summary>
    public string Name { get; }

    /// <summary>The shape of this action's value.</summary>
    public InputValueType ValueType { get; }

    /// <summary>The native handle from aver_fw_action_register. Internal: EnhancedInput.Rebind() is
    /// the only other reader, when it re-issues every binding after a context stack change.</summary>
    internal readonly int Handle;

    // aver_fw_action_register is IDEMPOTENT BY NAME (framework_abi.h:412) -- the same reason
    // aver_fw_class_declare is idempotent: an actor's OnBeginPlay runs every time it (re)spawns, and
    // re-declaring "Jump" on every possession must hand back the ORIGINAL action, never multiply it.
    // Caching HERE, by handle, extends that guarantee to the MANAGED wrapper too: two call sites that
    // each write InputAction.Digital("Jump") now get the SAME InputAction object, not two independent
    // ones that happen to share a display string. That is a real behaviour change from the old
    // pure-C# version, where only a SHARED STATIC FIELD made two call sites the same action -- but it
    // is the correct behaviour change, not an accidental one: it is exactly the property that makes
    // idempotent registration useful for the OnBeginPlay case in the first place, and the doc's own
    // "declare each once, usually as a static readonly field" guidance still works unchanged for a
    // script that follows it.
    private static readonly Dictionary<int, InputAction> s_byHandle = new();

    private InputAction(string name, InputValueType type, int handle)
    { Name = name; ValueType = type; Handle = handle; }

    /// <summary>Declares a button action.</summary>
    public static InputAction Digital(string name) => Of(name, InputValueType.Digital);
    /// <summary>Declares a single-axis action.</summary>
    public static InputAction Axis1D(string name) => Of(name, InputValueType.Axis1D);
    /// <summary>Declares a two-axis action, read through <see cref="Value2D"/>.</summary>
    public static InputAction Axis2D(string name) => Of(name, InputValueType.Axis2D);

    private static InputAction Of(string name, InputValueType type)
    {
        int handle = Fw.aver_fw_action_register(name, (int)type);
        // handle == 0 means a null/empty name or an invalid valueType (framework_abi.h's own
        // comment) -- do not cache under 0, or every failed registration would collapse into one
        // shared "invalid" object that silently answers for all of them.
        if (handle != 0 && s_byHandle.TryGetValue(handle, out InputAction? existing)) return existing;
        var a = new InputAction(name, type, handle);
        if (handle != 0) s_byHandle[handle] = a;
        return a;
    }

    /// <summary>Looks up a previously registered action by name, or null if none exists. Only ever
    /// returns an action THIS C# runtime itself created via Digital/Axis1D/Axis2D: the ABI has no
    /// getter for a handle's declared valueType (aver_fw_action_find returns the handle alone), so a
    /// handle registered from elsewhere -- a graph node, a future C++ caller -- has no way to become a
    /// strongly-typed InputAction here without this runtime already knowing what shape to build.</summary>
    public static InputAction? Find(string name)
    {
        int handle = Fw.aver_fw_action_find(name);
        return handle != 0 && s_byHandle.TryGetValue(handle, out InputAction? existing) ? existing : null;
    }

    [ThreadStatic] private static float[]? s_value2Buf;
    private static float[] Value2Buf => s_value2Buf ??= new float[2];

    /// <summary>The current accumulated value: X for a 1D axis, X and Y for 2D, X = 0 or 1 for a
    /// button. Z is always 0 now -- aver_fw_action_value2 only carries two channels across the ABI
    /// (framework_abi.h's own comment: "without the Z channel this ABI has no consumer for"), and
    /// none of this file's own binding methods below ever targeted component 2 either, so nothing
    /// that used to read a nonzero Z through this property existed to begin with.</summary>
    public Vec3 Value2D
    {
        get
        {
            float[] v = Value2Buf;
            Fw.aver_fw_action_value2(Handle, v);
            return new Vec3(v[0], v[1], 0f);
        }
    }

    /// <summary>The single-axis value.</summary>
    public float Value1D => Value2D.X;

    /// <summary>True while the action is active (any channel's magnitude exceeds the 0.15 dead zone,
    /// pinned on the native side -- not a parameter here either, for the same reason it is not one
    /// there: a caller who needs a different threshold wants a different feature).</summary>
    public bool IsHeld => Fw.aver_fw_action_held(Handle) != 0;

    /// <summary>True on the frame the action became active.</summary>
    public bool WasPressed => Fw.aver_fw_action_pressed(Handle) != 0;

    /// <summary>True on the frame the action stopped being active.</summary>
    public bool WasReleased => Fw.aver_fw_action_released(Handle) != 0;

    public override string ToString() => $"{Name} ({ValueType})";
}

/// <summary>One key-to-action mapping inside a context. Pure bookkeeping -- this struct never crosses
/// the ABI itself; EnhancedInput.Rebind() unpacks it into aver_fw_action_bind calls, adding the
/// owning context's priority at that point.</summary>
internal readonly struct InputBinding
{
    public readonly InputAction Action;
    public readonly InputSource Source;

    // RawKey is a bare int, not a Key, because `Source` decides which enum it actually names -- a
    // Key for InputSource.Key, a GamepadButton for GamepadButton, a GamepadAxis for GamepadAxis, and
    // unused (0, the Key.A placeholder's own value) for a mouse source. One strongly-typed field can
    // only ever be right for ONE of those; a bare int cast at each read site (BindKey/
    // BindGamepadButton/BindGamepadAxis write it, Rebind() and SaveBindings/LoadBindings's own
    // RawKeyName/TryParseRawKey read it back) is what makes a single InputBinding shape cover all of
    // them, the same reason the native aver_fw_action_bind's own `key` parameter is a plain int32_t.
    public readonly int   RawKey;
    public readonly float Scale;
    public readonly int   Component;  // 0=X, 1=Y

    /// <summary>Builds one binding. <paramref name="rawKey"/> is whatever id <paramref name="source"/>
    /// reads, already cast to int by the caller -- see <see cref="RawKey"/>'s own comment for which
    /// enum that is per source.</summary>
    public InputBinding(InputAction action, InputSource source, int rawKey, float scale, int component)
    { Action = action; Source = source; RawKey = rawKey; Scale = scale; Component = component; }
}

/// <summary>A set of key-to-action mappings pushed and popped as a whole. Derive it and bind in the
/// constructor. Unchanged by the move to a native action ABI: this class never touched Raw/Prev
/// itself, it only ever recorded intent, and EnhancedInput now replays that intent onto
/// aver_fw_action_bind instead of onto its own in-process evaluator.</summary>
public abstract class InputMappingContext
{
    internal readonly List<InputBinding> Bindings = new();

    /// <summary>Name shown in logs; defaults to the class name.</summary>
    public virtual string Name => GetType().Name;

    /// <summary>Binds a key to an action. <paramref name="scale"/> can negate it for an axis.</summary>
    protected void BindKey(InputAction action, Key key, float scale = 1f, int component = 0) =>
        Bindings.Add(new InputBinding(action, InputSource.Key, (int)key, scale, component));

    /// <summary>Binds a gamepad button to an action. <paramref name="scale"/> can negate it for an
    /// axis, the same as <see cref="BindKey"/>.</summary>
    protected void BindGamepadButton(InputAction action, GamepadButton button, float scale = 1f, int component = 0) =>
        Bindings.Add(new InputBinding(action, InputSource.GamepadButton, (int)button, scale, component));

    /// <summary>Binds a gamepad axis -- a thumbstick half or a trigger -- to an action.
    /// <paramref name="scale"/> scales the raw value, the same as <see cref="BindMouseLook"/> scales a
    /// mouse delta.</summary>
    protected void BindGamepadAxis(InputAction action, GamepadAxis axis, float scale = 1f, int component = 0) =>
        Bindings.Add(new InputBinding(action, InputSource.GamepadAxis, (int)axis, scale, component));

    /// <summary>Binds two opposed keys to one -1..1 axis.</summary>
    protected void BindAxis1D(InputAction action, Key positive, Key negative, int component = 0)
    {
        BindKey(action, positive,  1f, component);
        BindKey(action, negative, -1f, component);
    }

    /// <summary>Binds four keys to a 2D axis: X is up/down (forward), Y is right/left.</summary>
    protected void BindAxis2D(InputAction action, Key up, Key down, Key right, Key left)
    {
        BindAxis1D(action, up, down, component: 0);
        BindAxis1D(action, right, left, component: 1);
    }

    /// <summary>Binds mouse movement to a 2D action: X gets horizontal, Y gets vertical. `Key.A` on
    /// both bindings is an unused placeholder -- the native ABI's own aver_fw_action_bind comment
    /// notes it ignores `key` whenever `source` is not AVER_FW_ACTION_SRC_KEY, ported as-is rather
    /// than inventing a second binding shape just to avoid one ignored field.</summary>
    protected void BindMouseLook(InputAction action, float sensitivity = 1f)
    {
        Bindings.Add(new InputBinding(action, InputSource.MouseX, (int)Key.A, sensitivity, 0));
        Bindings.Add(new InputBinding(action, InputSource.MouseY, (int)Key.A, sensitivity, 1));
    }

    /// <summary>Binds the wheel to a single axis.</summary>
    protected void BindMouseWheel(InputAction action, float scale = 1f) =>
        Bindings.Add(new InputBinding(action, InputSource.MouseWheel, (int)Key.A, scale, 0));

    // The bindings a subclass's constructor set up, captured the first time any of Save/Load/
    // ResetToDefaults below touches this instance -- NOT in a constructor of this base class, because
    // that runs BEFORE the derived constructor's BindKey/BindAxis1D/... calls have added anything.
    // Capturing lazily, on first touch, is equivalent as long as nothing else mutates Bindings first,
    // and nothing else in this assembly does: these three methods are the only writers Bindings has.
    private List<InputBinding>? _defaultBindings;

    private void CaptureDefaultsIfNeeded() => _defaultBindings ??= new List<InputBinding>(Bindings);

    // One key per binding SLOT, not per binding list index: BindAxis2D alone adds four bindings that
    // all share one InputAction, and inserting a fifth ahead of them in a future version of a context
    // would silently renumber -- and thus misload -- every slot after it. Names, not the InputSource/
    // Key enum's underlying ints, so a value read back after an enum is reordered or extended still
    // resolves to the right member instead of silently becoming a different one.
    private static string SlotKeyPrefix(string contextName, string actionName, int slot) =>
        $"Rebind.{contextName}.{actionName}.{slot}";

    // The enum member NAME for one binding's RawKey -- the ".Key" a slot stores is always a name, per
    // SlotKeyPrefix's own comment, but WHICH enum that name belongs to depends on Source (RawKey's own
    // comment on InputBinding explains why the field itself is a bare int). A mouse source falls to
    // the `Key` case same as it always did -- RawKey is (int)Key.A there (BindMouseLook/BindMouseWheel
    // above), so this reproduces the exact "A" string SaveBindings wrote before RawKey existed.
    private static string RawKeyName(InputSource source, int rawKey) => source switch
    {
        InputSource.GamepadButton => ((GamepadButton)rawKey).ToString(),
        InputSource.GamepadAxis   => ((GamepadAxis)rawKey).ToString(),
        _                         => ((Key)rawKey).ToString(),
    };

    // The reverse of RawKeyName, with the SAME IsDefined guard LoadBindings already applies to Source
    // below -- a hand-edited or stale settings file's ".Key" string must never resolve to a member that
    // does not exist in whichever enum `source` picks. False (and rawKey left at 0) for anything that
    // fails to parse or isn't defined.
    private static bool TryParseRawKey(InputSource source, string name, out int rawKey)
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

    /// <summary>Writes this context's current bindings to <see cref="Settings"/> (which must already be
    /// open), one Source/Key pair per binding slot. Does not call <see cref="Settings.Flush"/> itself
    /// -- call it once after saving whatever else belongs to the same save point, the same way every
    /// other <c>Settings.Set*</c> call works.</summary>
    public void SaveBindings()
    {
        CaptureDefaultsIfNeeded();
        var slotByAction = new Dictionary<string, int>();
        foreach (InputBinding b in Bindings)
        {
            slotByAction.TryGetValue(b.Action.Name, out int slot);
            slotByAction[b.Action.Name] = slot + 1;
            string prefix = SlotKeyPrefix(Name, b.Action.Name, slot);
            Settings.SetString(prefix + ".Source", b.Source.ToString());
            Settings.SetString(prefix + ".Key", RawKeyName(b.Source, b.RawKey));
        }
    }

    /// <summary>Reads bindings <see cref="SaveBindings"/> previously wrote back over this context's
    /// current ones. A slot with nothing stored, or a stored Source/Key that no longer names an enum
    /// member (a hand-edited or stale settings file, or one saved by a build that has since dropped a
    /// binding) is left exactly as it already was -- THIS NEVER THROWS and never blanks a default down
    /// to some zero value. Only touches <see cref="Bindings"/>; if this context is already pushed, call
    /// <see cref="EnhancedInput.AddContext"/> again afterwards (safe to call on an already-pushed
    /// context -- it replaces it) to carry the change onto the native side.</summary>
    public void LoadBindings()
    {
        CaptureDefaultsIfNeeded();
        var slotByAction = new Dictionary<string, int>();
        for (int i = 0; i < Bindings.Count; i++)
        {
            InputBinding b = Bindings[i];
            slotByAction.TryGetValue(b.Action.Name, out int slot);
            slotByAction[b.Action.Name] = slot + 1;
            string prefix = SlotKeyPrefix(Name, b.Action.Name, slot);
            if (!Settings.Has(prefix + ".Source") || !Settings.Has(prefix + ".Key")) continue;
            // TryParse alone is not enough: it also accepts a bare numeric string ("Key=9999") and
            // hands back that int reinterpreted as the enum, defined member or not -- exactly the
            // silent-garbage case a hand-edited settings file is here to guard against. IsDefined
            // rejects that the same way it would reject a name that never existed.
            if (!Enum.TryParse(Settings.GetString(prefix + ".Source"), out InputSource source) ||
                !Enum.IsDefined(typeof(InputSource), source)) continue;
            // Which enum the ".Key" string is parsed against depends on the SOURCE just parsed above --
            // see TryParseRawKey's own comment. Must run after the Source parse for that reason.
            if (!TryParseRawKey(source, Settings.GetString(prefix + ".Key"), out int rawKey)) continue;
            Bindings[i] = new InputBinding(b.Action, source, rawKey, b.Scale, b.Component);
        }
    }

    /// <summary>Restores every binding to what this context's own constructor set up, discarding any
    /// <see cref="LoadBindings"/> or rebind since. Touches only the in-memory list -- it does not clear
    /// or overwrite anything in the settings store, so a caller that wants the reset to persist must
    /// call <see cref="SaveBindings"/> (and eventually <see cref="Settings.Flush"/>) afterwards, and
    /// one that wants it to reach the native side must re-push this context the same way
    /// <see cref="LoadBindings"/> does.</summary>
    public void ResetToDefaults()
    {
        CaptureDefaultsIfNeeded();
        Bindings.Clear();
        Bindings.AddRange(_defaultBindings!);
    }
}

/// <summary>The input router: mapping contexts go in, action values come out. No per-frame step of
/// its own any more -- see this file's own top-of-file comment for why Update() is gone rather than
/// merely empty.</summary>
public static class EnhancedInput
{
    /// <summary>One context at one priority.</summary>
    private sealed class Layer
    {
        public InputMappingContext Context = null!;
        public int Priority;
    }

    private static readonly List<Layer> s_layers = new();

    /// <summary>Pushes a context. Higher <paramref name="priority"/> consumes the keys it binds,
    /// blocking a lower-priority context from ever seeing them -- see <see cref="Rebind"/> for how
    /// that maps onto the native ABI, which has no context object of its own to carry it.</summary>
    public static void AddContext(InputMappingContext context, int priority = 0)
    {
        if (context is null) return;
        s_layers.RemoveAll(l => ReferenceEquals(l.Context, context));
        s_layers.Add(new Layer { Context = context, Priority = priority });
        // Sorting is no longer load-bearing for consumption -- the native side compares priority
        // VALUES directly (FrameworkAbi.cpp's actionKeyConsumedByHigherPriority), not list position --
        // but it keeps Rebind()'s re-issue order matching what a debugger dump of s_layers would show,
        // which cost nothing to keep.
        s_layers.Sort((a, b) => b.Priority.CompareTo(a.Priority));
        Rebind();
    }

    /// <summary>Pops a context. Safe for one that was never added.</summary>
    public static void RemoveContext(InputMappingContext context)
    {
        if (context is null) return;
        if (s_layers.RemoveAll(l => ReferenceEquals(l.Context, context)) > 0) Rebind();
    }

    /// <summary>Drops every context.</summary>
    public static void ClearContexts()
    {
        if (s_layers.Count == 0) return;
        s_layers.Clear();
        Fw.aver_fw_action_clear_bindings();
    }

    /// <summary>How many contexts are active.</summary>
    public static int ContextCount => s_layers.Count;

    // THE ABI HAS NO CONTEXT HANDLE (framework_abi.h's own NAMED ACTIONS section says so explicitly)
    // and consequently no partial "remove just this context" call either -- aver_fw_action_clear_
    // bindings drops every binding at once. So every AddContext/RemoveContext here clears the WHOLE
    // native binding table and re-issues it from s_layers, which is now the ONLY thing this class
    // still keeps in C# -- not because the framework needs a copy, but because the native side is
    // stateless about "what was pushed" by design, and somebody has to remember that in order to
    // rebuild it after a change. That somebody is whichever caller mutates the layer stack, i.e. us.
    private static void Rebind()
    {
        Fw.aver_fw_action_clear_bindings();
        for (int li = 0; li < s_layers.Count; li++)
        {
            Layer layer = s_layers[li];
            List<InputBinding> bindings = layer.Context.Bindings;
            for (int bi = 0; bi < bindings.Count; bi++)
            {
                InputBinding b = bindings[bi];
                Fw.aver_fw_action_bind(b.Action.Handle, (int)b.Source, b.RawKey, b.Scale, b.Component, layer.Priority);
            }
        }
    }

    // ---- rebind-menu operations, covering every pushed context at once -----------------------------
    //
    // These four cover a WHOLE rebind session -- Save/Load/Reset across every layer in one call --
    // rather than making a caller loop s_layers itself, which it cannot do anyway (s_layers is
    // private, per this file's own design). They are deliberately indifferent to WHICH kind of context
    // each layer is: a hand-written InputMappingContext subclass and an InputScheme-loaded one
    // (InputScheme.cs) are both just entries in s_layers by the time anything gets here, so a rebind
    // menu wired to these four needs no special case for a graph-only project's scheme context.
    //
    /// <summary>Writes every pushed context's bindings to <see cref="Settings"/>, then flushes the
    /// store to disk -- the trailing <see cref="Settings.Flush"/> call a caller looping
    /// <see cref="InputMappingContext.SaveBindings"/> itself would otherwise have to remember, made
    /// exactly once for the whole stack rather than once per context. Returns the flush's result.</summary>
    public static bool SaveAllBindings()
    {
        for (int i = 0; i < s_layers.Count; i++) s_layers[i].Context.SaveBindings();
        return Settings.Flush();
    }

    /// <summary>Reads every pushed context's bindings back from <see cref="Settings"/>, then re-pushes
    /// the whole stack ONCE so the native action-binding table picks up every context's change
    /// together -- not once per context, which is what a caller looping LoadBindings()+AddContext()
    /// itself would otherwise pay for every layer.</summary>
    public static void LoadAllBindings()
    {
        for (int i = 0; i < s_layers.Count; i++) s_layers[i].Context.LoadBindings();
        Rebind();
    }

    /// <summary>Restores every pushed context to the bindings its own constructor set up, in memory
    /// only -- see <see cref="InputMappingContext.ResetToDefaults"/>'s own comment for why this does
    /// not touch <see cref="Settings"/> or persist by itself; a caller wanting the reset to stick calls
    /// <see cref="SaveAllBindings"/> afterwards. Re-pushes the whole stack once, the same as
    /// <see cref="LoadAllBindings"/>.</summary>
    public static void ResetAllBindings()
    {
        for (int i = 0; i < s_layers.Count; i++) s_layers[i].Context.ResetToDefaults();
        Rebind();
    }

    // A slot is rebindable only when it has exactly one key/button/axis to replace -- the three
    // mouse-axis sources (MouseX/MouseY/MouseWheel) bind to "the mouse", not to a value a rebind menu
    // could offer a list of alternatives for, so RebindAction refuses them the same way it refuses a
    // slot that does not exist.
    private static bool IsRebindableSource(InputSource source) =>
        source is InputSource.Key or InputSource.GamepadButton or InputSource.GamepadAxis;

    // The SAME Enum.IsDefined guard TryParseRawKey (above) applies when reading a saved binding back
    // off Settings -- a caller handing RebindAction a rawKey it typed by hand (or read off a graph's
    // GetPressedKey) gets the identical protection against an undefined enum value silently taking up
    // residence in a binding.
    private static bool IsDefinedRawKey(InputSource source, int rawKey) => source switch
    {
        InputSource.GamepadButton => Enum.IsDefined(typeof(GamepadButton), rawKey),
        InputSource.GamepadAxis   => Enum.IsDefined(typeof(GamepadAxis), rawKey),
        _                         => Enum.IsDefined(typeof(Key), rawKey),
    };

    /// <summary>Rebinds the <paramref name="slot"/>-th binding of <paramref name="actionName"/>,
    /// walking pushed contexts HIGHEST PRIORITY FIRST and, within each context, its bindings in the
    /// order <see cref="InputMappingContext.SaveBindings"/> would number them -- the slot count RESETS
    /// at each context, the same as SaveBindings's own per-context <c>slotByAction</c> dictionary does,
    /// so a slot this method rebinds is the exact slot a save/load round-trip would persist under that
    /// context's own settings key. The first context that has <paramref name="slot"/> bindings for this
    /// action wins; a context with FEWER matching bindings than <paramref name="slot"/> is skipped
    /// entirely, not partially counted into the next one.
    ///
    /// False, with nothing changed, when: no pushed context has that many bindings for this action; the
    /// slot found is not <see cref="InputSource.Key"/>, <see cref="InputSource.GamepadButton"/> or
    /// <see cref="InputSource.GamepadAxis"/> (see <see cref="IsRebindableSource"/>); or
    /// <paramref name="rawKey"/> is not a defined member of whichever enum that slot's own Source
    /// names. Source/Scale/Component are kept exactly as they were -- only the raw key changes -- and
    /// the whole stack is re-pushed once on success so the rebind takes effect immediately.</summary>
    public static bool RebindAction(string actionName, int slot, int rawKey)
    {
        if (string.IsNullOrEmpty(actionName) || slot < 0) return false;
        for (int li = 0; li < s_layers.Count; li++)
        {
            List<InputBinding> bindings = s_layers[li].Context.Bindings;
            int seen = 0;
            for (int bi = 0; bi < bindings.Count; bi++)
            {
                InputBinding b = bindings[bi];
                if (b.Action.Name != actionName) continue;
                if (seen != slot) { seen++; continue; }

                if (!IsRebindableSource(b.Source) || !IsDefinedRawKey(b.Source, rawKey)) return false;

                bindings[bi] = new InputBinding(b.Action, b.Source, rawKey, b.Scale, b.Component);
                Rebind();
                return true;
            }
        }
        return false;
    }

    /// <summary>The <paramref name="slot"/>-th binding of <paramref name="actionName"/>'s CURRENT
    /// Source and raw key, using the identical per-context slot walk <see cref="RebindAction"/> uses
    /// (see its own comment). False, with both out parameters left at their defaults, when no pushed
    /// context has that many bindings for this action -- reports whatever source and key are actually
    /// there, including a non-rebindable mouse-axis slot, so a caller that only wants to DISPLAY the
    /// current binding is not filtered the way <see cref="RebindAction"/> filters what it will
    /// change.</summary>
    public static bool TryGetBindingKey(string actionName, int slot, out int rawKey, out InputSource source)
    {
        rawKey = -1;
        source = default;
        if (string.IsNullOrEmpty(actionName) || slot < 0) return false;
        for (int li = 0; li < s_layers.Count; li++)
        {
            List<InputBinding> bindings = s_layers[li].Context.Bindings;
            int seen = 0;
            for (int bi = 0; bi < bindings.Count; bi++)
            {
                InputBinding b = bindings[bi];
                if (b.Action.Name != actionName) continue;
                if (seen != slot) { seen++; continue; }
                rawKey = b.RawKey;
                source = b.Source;
                return true;
            }
        }
        return false;
    }
}
