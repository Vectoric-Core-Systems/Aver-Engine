using Aver.Framework;
using Aver.Scene;      // Vec3
using Aver.Scripting;  // Log

namespace Aver.Framework.SampleActor;

/// <summary>
/// The smallest STATIC-MESH actor: it carries the built-in sphere and nothing else. Spawning one puts a
/// <c>CMeshRenderer</c> on its entity whose <c>mesh</c> is <c>fnv1a64("Meshes/sphere.ocmesh")</c>, which is
/// exactly the id the sandbox's scene-render pass registers its procedural sphere under — so a spawned
/// <c>Sphere</c> is the first thing gameplay puts on screen.
/// </summary>
/// <remarks>
/// It ticks in nothing and overrides no per-frame hook: a bare mesh needs no logic. The one log line at
/// begin-play is proof the class declared, spawned and bound; the sphere then appears because the render
/// walk finds its component, not because of anything this type does each frame.
/// </remarks>
[AverClass("Sphere")]
public sealed class Sphere : AverActor
{
    /// <summary>The class recipe: give the entity the built-in sphere mesh, no material (fallback shading).</summary>
    public static void Configure(ClassBuilder b) => b.Mesh("Meshes/sphere.ocmesh");

    public override void OnBeginPlay(BeginReason reason) =>
        Log.Info($"[Sphere] OnBeginPlay reason={reason} entity={Self.Handle}");
}

/// <summary>
/// The actor you actually place: on begin-play it spawns a <see cref="Sphere"/> a metre above its origin.
/// This is the "a C# script that spawns a simple sphere" shape — one <c>Spawn&lt;T&gt;</c> call, the class
/// it names carrying the mesh. The spawned sphere then renders through the scene-render pass on its own.
/// </summary>
[AverClass("SphereSpawner")]
public sealed class SphereSpawner : AverActor
{
    public override void OnBeginPlay(BeginReason reason)
    {
        // Placed three units to +Y (right) of the spawner and at the reference cube's height, so it lands
        // beside the cube in the sandbox's default view rather than on top of it or below the ground.
        Entity s = Spawn<Sphere>(new Vec3(0f, 3f, 1f));
        Log.Info($"[SphereSpawner] spawned a Sphere as entity {s.Handle}");
    }
}
