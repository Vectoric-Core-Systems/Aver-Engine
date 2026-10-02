// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
// The C# half of the PCG split: F# decides WHAT goes where, this makes it exist in the engine.
//
// The division is deliberate and was settled before either half was written. F# owns generation and
// orchestration because procedural content is pure functions over immutable data, which is what F#
// is good at. C# owns application because that is where the engine's object model already lives --
// ActorClass, Entity, spawn -- and reimplementing it in F# would buy nothing.
//
// Nothing here generates anything. If a rule ever appears in this file, it is in the wrong file.

using System.Collections.Generic;
using Aver.Framework;

// BOTH VECTOR TYPES ARE ALIASED AND NEITHER IS USED UNQUALIFIED, because this namespace is nested
// under Aver.Pcg. C# resolves an enclosing namespace BEFORE a using directive, so a bare `Vec3` here
// binds to Aver.Pcg.Vec3 even with `using Aver.Scene;` present -- which compiles wherever the F# type
// happens to fit and fails confusingly where it does not. Aliasing makes each side impossible to
// mistake for the other, and makes the conversion the visible thing it should be.
using EngineVec3 = Aver.Scene.Vec3;
using EngineQuat = Aver.Scene.Quat;
using PcgVec3 = Aver.Pcg.Vec3;
using PcgQuat = Aver.Pcg.Quat;

namespace Aver.Pcg.Apply;

/// <summary>What happened when placements were applied. Never silently drops work.</summary>
public readonly struct ApplyResult
{
    /// <summary>Placements that became live entities.</summary>
    public int Spawned { get; init; }

    /// <summary>Placements whose Asset named no known actor class.</summary>
    public int UnknownClass { get; init; }

    /// <summary>Placements with an empty Asset.</summary>
    public int EmptyAsset { get; init; }

    /// <summary>Total placements considered.</summary>
    public int Total => Spawned + UnknownClass + EmptyAsset;

    /// <summary>A one-line summary, so a caller can log the outcome without unpacking it.</summary>
    public override string ToString()
        => $"spawned {Spawned}/{Total} (unknown class {UnknownClass}, empty asset {EmptyAsset})";
}

/// <summary>Turns F#-generated placements into live entities.</summary>
public static class PcgApply
{
    /// <summary>Converts an F# PCG vector to the engine's.</summary>
    /// <remarks>They are separate types on purpose. Aver.Pcg has no reference to Aver.Scene and must
    /// not gain one: it is a pure library that has to stay testable with no engine present, and the
    /// moment it references the scene types it can only run inside a live host. Two small structs
    /// and one conversion is the cheaper half of that trade.</remarks>
    public static EngineVec3 ToEngine(in PcgVec3 v) => new(v.X, v.Y, v.Z);

    /// <summary>Converts an F# PCG quaternion to the engine's.</summary>
    public static EngineQuat ToEngine(in PcgQuat q) => new(q.X, q.Y, q.Z, q.W);

    /// <summary>Spawns one entity per placement.</summary>
    /// <param name="placements">Typically the return of <c>Pcg.Scatter</c>.</param>
    /// <param name="spawned">Optional sink for the entities created, so a caller can despawn them.</param>
    /// <remarks>
    /// Placement.Asset is read as an ACTOR CLASS NAME, because a class is what the framework can
    /// spawn. A placement naming something unknown is counted and skipped, never guessed at: a
    /// generator that quietly produces half a forest is worse than one that says it produced half a
    /// forest.
    ///
    /// ActorClass.Find is called once per DISTINCT name rather than once per placement. A scatter of
    /// ten thousand rocks names the same class ten thousand times, and Find crosses into native code
    /// every call.
    /// </remarks>
    public static ApplyResult Apply(Placement[] placements, List<Entity>? spawned = null)
    {
        if (placements is null || placements.Length == 0) return default;

        var classCache = new Dictionary<string, ActorClass>(System.StringComparer.Ordinal);
        int ok = 0, unknown = 0, empty = 0;

        foreach (Placement p in placements)
        {
            if (string.IsNullOrEmpty(p.Asset)) { ++empty; continue; }

            if (!classCache.TryGetValue(p.Asset, out ActorClass cls))
            {
                cls = ActorClass.Find(p.Asset);
                classCache[p.Asset] = cls;
            }
            if (!cls.IsValid) { ++unknown; continue; }

            Entity e = Actors.Spawn(cls, ToEngine(p.Position), ToEngine(p.Rotation), ToEngine(p.Scale));
            if (!e.IsValid) { ++unknown; continue; }

            spawned?.Add(e);
            ++ok;
        }

        return new ApplyResult { Spawned = ok, UnknownClass = unknown, EmptyAsset = empty };
    }
}
