// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Aver.Framework.Hosting: the framework internals an engine extension's managed half is built on (docs/EXTENSIONS.md).

using System.ComponentModel;
using System.Runtime.InteropServices;

namespace Aver.Framework.Hosting;

/// <summary>For extensions, not for game scripts. What a managed extension (the Fabric Connector's <c>Aver.Fabric</c>) needs from the
/// framework and cannot get from the public gameplay API: spawning an actor around an instance the extension already made, reading an
/// entity's level values, driving a tick pass without the engine, and the few setters the bridge normally owns. Each member does what
/// the framework's own code does at that point, so an extension no longer needs <c>InternalsVisibleTo</c>.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public static class ExtensionHost
{
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

    /// <summary>Binds an actor instance to an entity without a spawn (a wrapper around an entity that already exists).</summary>
    public static void SetSelf(AverActor actor, Entity entity) => actor.Self = entity;

    /// <summary>Tells the bridge the actor's <c>[Editable]</c> values were written early, so its write before begin-play skips it.</summary>
    public static void MarkEditablesApplied(AverActor actor) => actor.EditablesApplied = true;

    /// <summary>Marks an input event handled, so it goes no further down the chain.</summary>
    public static void MarkHandled(InputEvent e) => e.Handled = true;

    /// <summary>Starts a tick pass on the game thread: wakes what awaited the next frame and runs the continuations. For a host that
    /// ticks without the engine (tests, tools).</summary>
    public static void BeginPass(bool physics) => GameThread.BeginPass(physics);

    /// <summary>Ends a frame on the game thread: flushes the deferred calls and the end-of-frame destroys.</summary>
    public static void EndFrame() => GameThread.EndFrame();

    /// <summary>Runs the queued deferred calls now, including ones queued while it runs. Returns how many ran.</summary>
    public static int FlushDeferred() => GameThread.Flush();

    /// <summary>Drops every queued deferred call and pending frame wait. For a host that starts a clean run (tests).</summary>
    public static void ResetDeferred() => GameThread.Reset();
}
