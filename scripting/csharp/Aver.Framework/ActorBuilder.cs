// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// The model tree an actor carries: placed child meshes and the handle that names one.

using Aver.Scene;

namespace Aver.Framework;

/// <summary>A handle to one placed model inside an actor: a child entity with a mesh and a transform, but no class.</summary>
public readonly struct ModelHandle : IEquatable<ModelHandle>
{
    /// <summary>The child entity this model is. 0 == invalid.</summary>
    public Entity Entity { get; }

    /// <summary>The per-placement identity minted when the model was placed. Survives rename and reorder.</summary>
    public ulong ModelId { get; }

    internal ModelHandle(Entity entity, ulong modelId) { Entity = entity; ModelId = modelId; }

    public bool IsValid => Entity.IsValid;

    public bool Equals(ModelHandle o) => Entity.Equals(o.Entity) && ModelId == o.ModelId;
    public override bool Equals(object? o) => o is ModelHandle m && Equals(m);
    public override int GetHashCode() => HashCode.Combine(Entity, ModelId);
    public override string ToString() => $"Model 0x{ModelId:X16} -> {Entity}";
}

/// <summary>Builds the model tree an actor carries: each <see cref="Place"/> spawns a child mesh under the actor.</summary>
public sealed class ActorBuilder
{
    private readonly Entity _root;
    internal ActorBuilder(Entity root) => _root = root;

    /// <summary>Places one model under the actor and returns its handle. Local pos in centimetres, rot in degrees (yaw, pitch, roll), scale as multipliers.</summary>
    /// <remarks>FROZEN call shape: the editor rewrites only the numeric literals in pos/rot/scale, matched by modelId.</remarks>
    public ModelHandle Place(
        ulong modelId,
        string meshPath,
        string material,
        (float X, float Y, float Z) pos,
        (float Yaw, float Pitch, float Roll) rot,
        (float X, float Y, float Z) scale)
    {
        int child = SceneNative.aver_scene_create();
        var e = new Entity(child);

        SceneNative.aver_scene_set_object_id(child, unchecked((long)modelId));

        SceneNative.aver_scene_add_component(child, SceneIds.CMeshRenderer);
        SceneNative.aver_scene_set_i64(child, SceneIds.MeshMesh, Assets.ObjectIdOf(meshPath));
        if (!string.IsNullOrEmpty(material))
            SceneNative.aver_scene_set_i32(child, SceneIds.MeshMaterial, SceneNative.aver_scene_material(0, material));

        e.SetLocalPosition(new Vec3(pos.X, pos.Y, pos.Z));
        e.SetLocalRotation(new Rot(rot.Yaw, rot.Pitch, rot.Roll).ToQuat());
        e.SetLocalScale(new Vec3(scale.X, scale.Y, scale.Z));
        SceneNative.aver_scene_set_parent(child, _root.Handle);

        return new ModelHandle(e, modelId);
    }
}
