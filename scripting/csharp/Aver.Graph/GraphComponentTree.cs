// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// Turns a class graph's COMP records into real child entities under a spawned instance.

using System;
using System.Collections.Generic;
using Aver.Framework;
using Aver.Scene;
using Aver.Scripting;

namespace Aver.Graph;

/// <summary>Builds the child entities a class graph's <c>COMP</c> records describe, under one spawned
/// instance. See <see cref="GraphComponent"/> for the record and
/// <c>modules/formats/include/aver/formats/OcGraph.hpp</c> for the format half.</summary>
///
/// <remarks>ONE CALL PER INSTANCE, NOT PER CLASS, and that is the whole shape of this. The framework's
/// class-default machinery (<c>aver_fw_class_add_component</c> + <c>class_set_default_*</c>, which
/// <c>ClassBuilder.Mesh</c> uses for <c>CLASS mesh=</c>) can only reach the class's OWN entity: one
/// component of each kind, no transform of their own, no tree. A component TREE is child entities, and
/// child entities exist per instance, so this runs where an instance is born -- HostBridge's DispBind.
///
/// NOTHING HERE DESTROYS ANYTHING. Every child is parented under the actor's entity, and
/// <c>World::destroy</c> expands the destroy queue to whole subtrees (World.cpp's <c>collectAndRetire</c>),
/// so despawning the actor takes its components with it. A teardown path here would be a second owner
/// of the same lifetime, which is how double-frees start.
///
/// EVERY FAILURE IS REPORTED AND SURVIVED. A component whose mesh does not exist, whose kind this
/// version has never heard of, or whose attribute does not parse leaves the rest of the tree standing --
/// an actor missing one part, with a line in the log naming it, beats an actor that refuses to spawn
/// because one attribute was mistyped. The one thing never done silently is nothing: a kind that does
/// not attach says so by name.</remarks>
public static class GraphComponentTree
{
    /// <summary>Attaches every component in <paramref name="components"/> as a child of
    /// <paramref name="root"/>. Returns the id-to-entity map, so a caller that wants to find, say,
    /// the muzzle later can. Empty list in, empty map out, no native calls made.</summary>
    public static Dictionary<string, Entity> Build(Entity root, IReadOnlyList<GraphComponent> components,
                                                   string classNameForLog)
    {
        var byId = new Dictionary<string, Entity>(StringComparer.Ordinal);
        if (components.Count == 0) return byId;

        // PASS ONE CREATES, PASS TWO PARENTS. A COMP may name a parent declared later in the file --
        // the format allows it and the C++ reader has already proved the chain is a tree -- so every
        // entity has to exist before any parent link can be resolved. Doing it in one pass would work
        // for files that happen to be written parent-first and fail on the ones that are not, which is
        // the worst of the three possible behaviours.
        foreach (GraphComponent c in components)
        {
            Entity e = Entity.Create(c.Id);
            if (!e.IsValid)
            {
                Log.Error($"[Graph] {classNameForLog}: could not create an entity for component '{c.Id}' " +
                          "-- the scene's index space may be exhausted");
                continue;
            }
            byId[c.Id] = e;
            ApplyKind(e, c, classNameForLog);
            ApplyTransform(e, c);
        }

        foreach (GraphComponent c in components)
        {
            if (!byId.TryGetValue(c.Id, out Entity e)) continue;   // creation failed above, already logged
            Entity parent = root;
            if (c.Parent is not null && byId.TryGetValue(c.Parent, out Entity named)) parent = named;
            e.SetParent(parent);
        }

        return byId;
    }

    // Local transform, relative to whatever this component ends up parented to. Written even when it
    // is identity: an entity is born with an identity CLocal anyway, so this is three redundant writes
    // in that case and one less branch to be wrong about.
    private static void ApplyTransform(Entity e, GraphComponent c)
    {
        e.SetLocalPosition(new Vec3(c.Position[0], c.Position[1], c.Position[2]));
        // Rot takes YAW, PITCH, ROLL in that order -- the same order ActorBuilder.Place takes and the
        // same order the record is documented in. Getting this backwards produces an actor whose parts
        // are all present and all facing wrong, which reads as an art problem rather than a code one.
        e.SetLocalRotation(new Rot(c.RotationDeg[0], c.RotationDeg[1], c.RotationDeg[2]).ToQuat());
        e.SetLocalScale(new Vec3(c.Scale[0], c.Scale[1], c.Scale[2]));
    }

    private static void ApplyKind(Entity e, GraphComponent c, string classNameForLog)
    {
        switch (c.Kind.ToLowerInvariant())
        {
            case "scene":
                // Nothing to attach. A Scene component is a NAMED TRANSFORM and that is the entire
                // point of it: a muzzle, a hand socket, an attach point. It is not a degenerate case
                // to be optimised away into its parent -- a graph asks "where is `muzzle`" by name,
                // and something has to be there to answer.
                break;

            case "mesh":
                // SetVisible, NOT AddComponent, and the difference is not cosmetic. World::addComponent
                // hands back ZERO-FILLED storage, and CMeshRenderer's visible bit is POSITIVE -- so a
                // renderer attached directly is attached, correct, and invisible. Entity.SetVisible goes
                // through EnsureMeshRenderer, which attaches AND seeds that bit; calling AddComponent
                // first defeats it, because EnsureMeshRenderer then sees a component already there and
                // leaves the flags at zero. This was measured, not reasoned about: the first run of the
                // spawn test created every entity correctly and drew none of them.
                e.SetVisible(true);
                // `mesh=` is CONTENT-RELATIVE -- "Meshes/Blaster.ocmesh", not
                // "Content/Meshes/Blaster.ocmesh" -- because ObjectIdOf hashes the string itself and
                // the content index registers the relative form. A path with the content root on the
                // front hashes to an id nothing answers to, and the entity then spawns, parents,
                // transforms and draws NOTHING, with no error anywhere. Same convention as `CLASS
                // mesh=`, which is the only reason it is discoverable at all.
                if (c.Attributes.TryGetValue("mesh", out string? meshPath) && meshPath.Length > 0)
                    e.SetMesh(meshPath);
                if (c.Attributes.TryGetValue("material", out string? material) && material.Length > 0)
                    e.SetMaterial(material);
                ApplyOwnerHide(e, c);
                break;

            case "skeletalmesh":
                // A skinned part carries BOTH: CMeshRenderer for the geometry and CSkeletalMesh for
                // the rig, exactly as CSkeletalMesh's own comment in Components.hpp describes -- it
                // binds a skeleton to whatever the entity already draws rather than being a second
                // kind of mesh renderer. A `SkeletalMesh` with no `mesh=` would therefore be a rig
                // posing nothing.
                e.SetVisible(true);   // see the `mesh` case above on why this and not AddComponent
                e.AddComponent(Component.SkeletalMesh);
                if (c.Attributes.TryGetValue("mesh", out string? skinMesh) && skinMesh.Length > 0)
                    e.SetMesh(skinMesh);
                if (c.Attributes.TryGetValue("material", out string? skinMat) && skinMat.Length > 0)
                    e.SetMaterial(skinMat);
                if (c.Attributes.TryGetValue("skeleton", out string? skel) && skel.Length > 0)
                    e.SetInt64("CSkeletalMesh.skeleton", Assets.ObjectIdOf(skel));
                ApplyOwnerHide(e, c);
                break;

            case "animator":
                e.AddComponent(Component.Animator);
                if (c.Attributes.TryGetValue("clip", out string? clip) && clip.Length > 0)
                    e.SetInt64("CAnimator.clip", Assets.ObjectIdOf(clip));
                e.SetFloat("CAnimator.speed", c.AttrFloat("speed", 1f));
                e.SetFloat("CAnimator.blendWeight", c.AttrFloat("weight", 1f));
                break;

            case "particles":
                e.AddComponent(Component.ParticleEmitter);
                if (c.Attributes.TryGetValue("effect", out string? effect) && effect.Length > 0)
                    e.SetInt64("CParticleEmitter.effect", Assets.ObjectIdOf(effect));
                e.SetInt("CParticleEmitter.seed", (int)c.AttrFloat("seed", 0f));
                break;

            case "camera":
                e.AddComponent(Component.Camera);
                // fovYRad, note -- the field stores RADIANS while the record is authored in degrees,
                // the same conversion ClassBuilder.Camera does. Near/far are centimetres, like every
                // other distance in this engine.
                e.SetFloat("CCamera.fovYRad", c.AttrFloat("fov", 60f) * (MathF.PI / 180f));
                e.SetFloat("CCamera.nearCm", c.AttrFloat("near", 10f));
                e.SetFloat("CCamera.farCm", c.AttrFloat("far", 100000f));
                e.SetInt("CCamera.priority", (int)c.AttrFloat("priority", 0f));
                WarnNothingReadsIt(classNameForLog, c, "CCamera");
                break;

            case "light":
                e.AddComponent(Component.Light);
                e.SetInt("CLight.kind", LightKindFromName(c.AttrString("kind")));
                e.SetVec3("CLight.colour", new Vec3(c.AttrFloat("r", 1f), c.AttrFloat("g", 1f), c.AttrFloat("b", 1f)));
                e.SetFloat("CLight.intensityLux", c.AttrFloat("intensity", 100000f));
                e.SetFloat("CLight.rangeCm", c.AttrFloat("range", 0f));
                // innerCos/outerCos are COSINES of the cone half-angles, not the angles. Authoring in
                // degrees and converting here is the only sane split: nobody types 0.966 meaning 15
                // degrees, and nobody wants the renderer doing a trig call per light per frame.
                e.SetFloat("CLight.innerCos", MathF.Cos(c.AttrFloat("inner", 0f) * (MathF.PI / 180f)));
                e.SetFloat("CLight.outerCos", MathF.Cos(c.AttrFloat("outer", 45f) * (MathF.PI / 180f)));
                WarnNothingReadsIt(classNameForLog, c, "CLight");
                break;

            case "fluid":
                ApplyFluid(c, classNameForLog);
                break;

            default:
                // ATTACHED AS A BARE TRANSFORM, NOT DROPPED. The entity still exists, still sits where
                // the record put it, and still answers to its name, so the rest of the tree hanging
                // off it survives an unrecognised kind. What it does not do is happen quietly.
                Log.Warn($"[Graph] {classNameForLog}: component '{c.Id}' has unrecognised kind '{c.Kind}' " +
                         "-- it will exist as a plain transform and draw nothing. Known kinds: Scene, " +
                         "Mesh, SkeletalMesh, Animator, Particles, Camera, Light, Fluid");
                break;
        }
    }

    // A FLUID COMPONENT AUTHORS A SOLVER REQUEST, NOT AN ECS PRESENCE. fluids::FluidScene owns no
    // entity and reads no CWorld at all (FluidScene.hpp's own "THERE IS NO OWNING ENTITY" comment),
    // so unlike every kind above, `e` itself is never told to draw or carry a component here -- the
    // placeholder entity Build() already created for `c` still gets its ApplyTransform call right
    // after this returns (so it exists and sits where the record says, for an inspector to find),
    // but nothing the solver reads ever comes from `e`.
    //
    // pos= IS READ AS AN ABSOLUTE WORLD centreCm, not a parent-relative offset -- the identical
    // "authored, not composed through a transform chain" placement SandboxApp::applyLevelWater
    // already gives a WATER record's own centreCm. A true hierarchical placement would need this
    // component's OWN world transform, which does not exist yet at the point ApplyKind runs
    // (Build()'s own comment: "PASS ONE CREATES ... ApplyTransform" runs AFTER ApplyKind, and
    // parenting runs in a second pass after that) -- reaching for the ROOT's world position instead
    // would just as often be stale, since world matrices are propagated once per frame, after
    // gameplay (see the composition root's own onUpdate comment), not the instant a spawn call
    // returns. Given the desc it feeds has no rotation field at all (FluidVolumeDesc is
    // axis-aligned only), pretending to compose a full parent transform onto it would be more
    // fiction than the plain absolute reading this uses.
    //
    // scale= MULTIPLIES FluidVolumeDesc's own default half-extent (100, 100, 50 cm --
    // FluidVolume.hpp), the identical "scale multiplies a unit shape" meaning Mesh's and
    // SkeletalMesh's scale= already carry (Graph.cs's own GraphComponent doc: "scale multiplier"),
    // just applied to a procedural half-extent instead of an authored mesh's bind pose. scale=1,1,1
    // -- what an omitted scale= already defaults to -- therefore spawns the exact pool an unauthored
    // WATER record's own FluidVolumeDesc default would.
    private static void ApplyFluid(GraphComponent c, string classNameForLog)
    {
        var centreCm = new Vec3(c.Position[0], c.Position[1], c.Position[2]);
        var halfExtentCm = new Vec3(100f * c.Scale[0], 100f * c.Scale[1], 50f * c.Scale[2]);
        // compliance/damping/iterations/pressure fall back to FluidVolumeDesc's own defaults
        // (kHeavyLiquidCompliance=1.0e-4f, kDefaultFluidDamping=0.1f, kDefaultFluidIterations=5,
        // kFluidPressureAuto=-1.0f) when the record names none -- an unauthored `COMP ... Fluid`
        // therefore simulates with the identical solver settings an unauthored WATER record gets,
        // not a second set of made-up numbers.
        //
        // THE MATERIAL LAYER: `preset=`/`density=`/`viscosity=` ride the SAME relay call as the four
        // raw knobs above (Game.SpawnFluidVolumeMaterial, not a second Kind or a second branch here)
        // -- design brief section 6's own grammar. Their AttrFloat/AttrString fallbacks (-1, "")
        // mean exactly "not given" on the native side too (framework_abi.h's own sentinel), so a
        // `COMP ... Fluid` naming none of the three spawns with no material at all, identical to
        // before this layer existed.
        //
        // THE PRECEDENCE CHECK IS NOT HERE. A line naming BOTH a material and a non-default
        // `damping=` is forwarded through exactly as authored -- see
        // Game.SpawnFluidVolumeMaterial's own doc comment for why the refusal happens once,
        // downstream, at fluids::FluidScene::spawn, not in every place that can construct a request.
        bool accepted = Game.SpawnFluidVolumeMaterial(
            centreCm, halfExtentCm, $"{classNameForLog}.{c.Id}",
            c.AttrFloat("compliance", 1.0e-4f),
            c.AttrFloat("damping", 0.1f),
            (int)c.AttrFloat("iterations", 5f),
            c.AttrFloat("pressure", -1f),
            c.AttrFloat("density", -1f),
            c.AttrFloat("viscosity", -1f),
            c.AttrString("preset"));
        if (!accepted)
            Log.Warn($"[Graph] {classNameForLog}: component '{c.Id}' declared kind Fluid, but no " +
                     "fluid-spawn provider is installed -- this build has no simulated-fluids " +
                     "module linked, so it will exist as a plain transform and simulate nothing");
    }

    // `hidden=owner` ON A Mesh OR SkeletalMesh COMPONENT: keeps this mesh drawing for every camera
    // except the one belonging to whatever entity it is a child of -- see Components.hpp's
    // kMeshRendererHiddenFromOwner for the whole reasoning (why zero-cost by default, why the shadow
    // still casts) and the FirstPerson template's own `COMP body` line for the motivating case: a
    // first-person camera sits at the character's own eye position, INSIDE its skinned body, so with
    // nothing to opt out the character fills its own screen with the inside of its own mesh.
    //
    // A STRING ATTRIBUTE, NOT A BARE BOOLEAN, on purpose -- `hidden=1` would answer "hidden from
    // whom?" with nothing, and this codebase does not have a second audience for a mesh to be hidden
    // from today (no per-player visibility, no editor-only ghosting through this mechanism). Reading
    // it as a string leaves room for a future `hidden=always` or similar without a second attribute
    // key or a format change -- the same opaque-string-now, meaning-later shape `kind=` and `hidden=`'s
    // own sibling attributes already use throughout this file. Anything other than exactly "owner"
    // (case-insensitively) is left alone rather than guessed at: a typo should draw the mesh it always
    // did, not silently hide it from nobody or from everybody.
    private static void ApplyOwnerHide(Entity e, GraphComponent c)
    {
        if (string.Equals(c.AttrString("hidden"), "owner", StringComparison.OrdinalIgnoreCase))
            e.SetHiddenFromOwner(true);
    }

    // Attached, correct, and inert -- so say so ONCE, by name, rather than letting an author conclude
    // their transform is wrong. No renderer in this engine reads CLight or CCamera today (the Voxi
    // path lights from the sun and GI; the camera comes from the controlled pawn), so a Light or
    // Camera component is authored data waiting for a consumer. That is a reasonable thing to let
    // someone write -- the day a renderer learns the component, every existing graph lights up -- and
    // an unreasonable thing to let them discover by staring at a black scene.
    private static void WarnNothingReadsIt(string classNameForLog, GraphComponent c, string component)
    {
        Log.Warn($"[Graph] {classNameForLog}: component '{c.Id}' attached a {component}, but nothing in " +
                 "this engine reads that component yet -- it is stored correctly and will start working " +
                 "when a renderer consumes it, but it has no effect today");
    }

    // CLight.kind is an i32 whose meaning no consumer has fixed yet (nothing reads CLight -- see
    // WarnNothingReadsIt). Point/spot/directional in declaration order is the convention every engine
    // in this shape uses and the one Components.hpp's field order implies; an unrecognised name gets
    // point rather than an error, matching how the rest of this file treats a bad attribute.
    private static int LightKindFromName(string name) => name.ToLowerInvariant() switch
    {
        "spot" => 1,
        "directional" or "dir" or "sun" => 2,
        _ => 0,   // point, and the fallback
    };
}
