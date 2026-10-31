// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Aver.Framework.Hosting: how an engine extension's managed half starts up and the framework internals it is built on (docs/EXTENSIONS.md).

using System.ComponentModel;
using System.Reflection;
using System.Runtime.InteropServices;

namespace Aver.Framework.Hosting;

/// <summary>The entry point of an extension's managed half: the type named by <c>managed.entry</c> in the extension's manifest. The engine
/// creates one instance (it needs a public parameterless constructor), calls <see cref="Load"/> once after the extension's assemblies are
/// loaded into the host context and before any project script loads, and keeps no reference to it afterwards. Throwing from
/// <see cref="Load"/> skips the extension with one log line; nothing else is affected.</summary>
public interface IAverExtension
{
    /// <summary>Starts the extension. Declare its actor types and set up whatever it needs through <paramref name="context"/>.</summary>
    void Load(ExtensionContext context);
}

/// <summary>What the engine tells an extension at <see cref="IAverExtension.Load"/>, and what it lets the extension ask for.</summary>
public sealed class ExtensionContext
{
    private readonly List<Func<Type, bool>> _actorFilters = new();

    internal ExtensionContext(string name, string version, string directory, IReadOnlyList<Assembly> assemblies)
    {
        Name = name;
        Version = version;
        Directory = directory;
        Assemblies = assemblies;
    }

    /// <summary>The extension's name (its folder and the <c>name</c> of its manifest).</summary>
    public string Name { get; }

    /// <summary>The <c>version</c> of its manifest; empty when it names none.</summary>
    public string Version { get; }

    /// <summary>The extension's installed folder, <c>&lt;executable dir&gt;/Extensions/&lt;Name&gt;</c>.</summary>
    public string Directory { get; }

    /// <summary>The assemblies of the manifest's <c>managed.runtime</c>, loaded into the host context.</summary>
    public IReadOnlyList<Assembly> Assemblies { get; }

    /// <summary>Declares every non-abstract actor type in <see cref="Assemblies"/> that derives from <paramref name="baseType"/>. A type is
    /// declared once however many requests match it.</summary>
    public void DeclareActors(Type baseType)
    {
        ArgumentNullException.ThrowIfNull(baseType);
        _actorFilters.Add(baseType.IsAssignableFrom);
    }

    /// <summary>Declares every non-abstract actor type in <see cref="Assemblies"/> for which <paramref name="predicate"/> is true.</summary>
    public void DeclareActors(Func<Type, bool> predicate)
    {
        ArgumentNullException.ThrowIfNull(predicate);
        _actorFilters.Add(predicate);
    }

    /// <summary>Declares every non-abstract actor type in <see cref="Assemblies"/>.</summary>
    public void DeclareAllActors() => _actorFilters.Add(static _ => true);

    /// <summary>Writes one line to the engine's log, prefixed with the extension's name.</summary>
    public void Log(string message, global::Aver.Scripting.Log.Level level = global::Aver.Scripting.Log.Level.Info) =>
        global::Aver.Scripting.Log.Write(level, $"[{Name}] {message}");

    /// <summary>The requests made through the <c>DeclareActors</c> methods, in order. Read by the bridge once <see cref="IAverExtension.Load"/> returns.</summary>
    internal IReadOnlyList<Func<Type, bool>> ActorFilters => _actorFilters;
}

/// <summary>For extensions, not for game scripts. What a managed extension needs from the framework and cannot get from the public gameplay
/// API: spawning an actor around an instance the extension already made, reading an entity's level values, driving a tick pass without the
/// engine, and the few setters the bridge normally owns. Each member does what the framework's own code does at that point, so an extension
/// needs no <c>InternalsVisibleTo</c>. The members are grouped by what they act on: spawning, entity data, the view and input, binding, and
/// frame driving.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public static class ExtensionHost
{
    #region Spawning

    /// <summary>The entity for a raw ABI handle (the <see cref="Entity.Handle"/> of a live entity); <c>default</c> for 0.</summary>
    public static Entity EntityFromHandle(int handle) => new Entity(handle);

    /// <summary>Spawns an actor of <paramref name="cls"/> and binds <paramref name="instance"/> to it instead of constructing one, so a
    /// node an extension made with <c>new</c> becomes the actor. Returns the entity handle, 0 when the spawn failed. A null instance
    /// spawns a plain actor of the class.</summary>
    public static int SpawnActor(ActorClass cls, string? name, float[] position, float[] rotation, float[] scale, AverActor? instance)
    {
        AverActor.OfferPending(instance);
        try { return Fw.aver_fw_spawn(cls.Handle, name, position, rotation, scale); }
        finally { AverActor.OfferPending(null); }
    }

    /// <summary>Starts a spawn batch: the actors spawned until <see cref="EndSpawnBatch"/> run their begin-play together, parents first.</summary>
    public static void BeginSpawnBatch() => Fw.aver_fw_spawn_batch_begin();

    /// <summary>Ends the batch <see cref="BeginSpawnBatch"/> started. Returns the number of actors that began play.</summary>
    public static int EndSpawnBatch() => Fw.aver_fw_spawn_batch_end();

    #endregion

    #region Entity data

    /// <summary>The level-authored property values of <paramref name="entity"/> (the key/value pairs of its placement), in file order.
    /// Throws <see cref="DllNotFoundException"/> or <see cref="EntryPointNotFoundException"/> in a process with no engine library.</summary>
    public static List<(string Key, string Text)> ReadProperties(Entity entity)
    {
        int n = Fw.aver_fw_props_count(entity.Handle);
        var values = new List<(string Key, string Text)>(n);
        for (int i = 0; i < n; i++)
            values.Add((Marshal.PtrToStringUTF8(Fw.aver_fw_props_name(entity.Handle, i)) ?? "",
                        Marshal.PtrToStringUTF8(Fw.aver_fw_props_value(entity.Handle, i)) ?? ""));
        return values;
    }

    #endregion

    #region View and input

    /// <summary>Makes <paramref name="entity"/> the entity the renderer views from.</summary>
    public static void SetViewEntity(Entity entity) => Fw.aver_fw_set_view_entity(entity.Handle);

    /// <summary>The names of the input actions the project registered natively, in handle order.</summary>
    public static List<string> RegisteredActionNames()
    {
        var names = new List<string>();
        for (int i = 1, n = FwIn.aver_fw_action_count(); i <= n; i++)
            if (InputAction.FromHandle(i) is { } a) names.Add(a.Name);
        return names;
    }

    /// <summary>Marks an input event handled, so it goes no further down the chain.</summary>
    public static void MarkHandled(InputEvent e) => e.Handled = true;

    #endregion

    #region Binding

    /// <summary>Binds an actor instance to an entity without a spawn (a wrapper around an entity that already exists).</summary>
    public static void SetSelf(AverActor actor, Entity entity) => actor.Self = entity;

    /// <summary>Tells the bridge the actor's <c>[Editable]</c> values were written early, so its write before begin-play skips it.</summary>
    public static void MarkEditablesApplied(AverActor actor) => actor.EditablesApplied = true;

    #endregion

    #region Frame driving

    /// <summary>Starts a tick pass on the game thread: wakes what awaited the next frame and runs the continuations. For a host that
    /// ticks without the engine (tests, tools).</summary>
    public static void BeginPass(bool physics) => GameThread.BeginPass(physics);

    /// <summary>Ends a frame on the game thread: flushes the deferred calls and the end-of-frame destroys.</summary>
    public static void EndFrame() => GameThread.EndFrame();

    /// <summary>Runs the queued deferred calls now, including ones queued while it runs. Returns how many ran.</summary>
    public static int FlushDeferred() => GameThread.Flush();

    /// <summary>Drops every queued deferred call and pending frame wait. For a host that starts a clean run (tests).</summary>
    public static void ResetDeferred() => GameThread.Reset();

    #endregion
}
