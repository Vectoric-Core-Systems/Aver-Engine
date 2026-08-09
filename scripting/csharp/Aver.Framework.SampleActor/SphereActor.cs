// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
using Aver.Framework;
using Aver.Scene;
using Aver.Scripting;

namespace Aver.Framework.SampleActor;

// The sample static-mesh actor and the actor that spawns one.

/// <summary>The smallest static-mesh actor: it carries the built-in sphere and nothing else.</summary>
[AverClass("Sphere")]
public sealed class Sphere : AverActor
{
    /// <summary>The class recipe: the built-in sphere mesh, no material.</summary>
    public static void Configure(ClassBuilder b) => b.Mesh("Meshes/sphere.ocmesh");

    /// <summary>Logs that the instance spawned and bound.</summary>
    public override void OnBeginPlay(BeginReason reason) =>
        Log.Info($"[Sphere] OnBeginPlay reason={reason} entity={Self.Handle}");
}

/// <summary>An actor that spawns a <see cref="Sphere"/> beside itself when play begins.</summary>
[AverClass("SphereSpawner")]
public sealed class SphereSpawner : AverActor
{
    /// <summary>Spawns one sphere three units to +Y and logs where it landed.</summary>
    public override void OnBeginPlay(BeginReason reason)
    {
        Sphere? sphere = Spawn<Sphere>(new Vec3(0f, 3f, 1f));
        Log.Info($"[SphereSpawner] spawned '{sphere?.Self.Name}' at world {sphere?.Self.WorldPosition}");
    }
}
