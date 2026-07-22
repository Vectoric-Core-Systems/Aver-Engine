using Aver.Scene;

namespace Aver.Framework;

/// <summary>
/// A handle to one placed model inside an actor — a child entity carrying a mesh and its own transform,
/// but no class (so <c>aver_fw_class_of</c> on it is 0: it is data, not an actor). Hand-written behaviour
/// reads a strongly-typed <see cref="ModelHandle"/> property to drive a specific model (spin a wheel,
/// open a door); the editor drives the same model's transform through the gizmo.
/// </summary>
public readonly struct ModelHandle : IEquatable<ModelHandle>
{
    /// <summary>The child entity this model is. 0 == invalid.</summary>
    public Entity Entity { get; }

    /// <summary>The per-placement identity minted when the model was placed — the key a dragged gizmo
    /// matches to its generated <see cref="ActorBuilder.Place"/> line. Survives rename and reorder.</summary>
    public ulong ModelId { get; }

    internal ModelHandle(Entity entity, ulong modelId) { Entity = entity; ModelId = modelId; }

    public bool IsValid => Entity.IsValid;

    public bool Equals(ModelHandle o) => Entity.Equals(o.Entity) && ModelId == o.ModelId;
    public override bool Equals(object? o) => o is ModelHandle m && Equals(m);
    public override int GetHashCode() => HashCode.Combine(Entity, ModelId);
    public override string ToString() => $"Model 0x{ModelId:X16} -> {Entity}";
}

/// <summary>
/// Builds the model tree an actor carries inside it — the thing the actor-editor viewport shows and a
/// gizmo edits. Every <see cref="Place"/> call spawns a child entity, gives it a mesh and a transform,
/// parents it under the actor, and stamps it with a stable per-placement identity. The whole tree is
/// written from a single generated method (<c>BuildModels</c>) in the actor's <c>.Designer.cs</c> half,
/// which the editor owns and rewrites on save.
/// </summary>
/// <remarks>
/// <para>
/// This is the sibling of <see cref="ClassBuilder"/>: <c>Configure</c> is the hand-written class recipe,
/// <c>BuildModels</c> is the editor-owned model layout. They are split because one is a formula a human
/// reasons about and the other is coordinates a human DRAGS — and you cannot drag a formula. Anything a
/// script computes or loops over lives in <c>Configure</c> or a hook and is, correctly, not
/// gizmo-editable.
/// </para>
/// <para>
/// Transforms are authored the way the coordinate contract and <c>.ocmap</c>'s PLACE author them:
/// position and scale in centimetres, rotation as degrees (yaw about +Z, pitch about +Y, roll about +X).
/// The rotation is converted to the quaternion the field stores through <see cref="Rot.ToQuat"/>.
/// </para>
/// </remarks>
public sealed class ActorBuilder
{
    private readonly Entity _root;
    internal ActorBuilder(Entity root) => _root = root;

    /// <summary>
    /// Place one model under the actor. The call shape is FIXED because the editor both writes and rewrites
    /// it (see DESIGNER_REWRITE.md): the id first, then the mesh, then the material, then the three named
    /// coordinate tuples last. On save the editor rewrites ONLY the numeric literals inside
    /// <paramref name="pos"/>/<paramref name="rot"/>/<paramref name="scale"/>, matched by
    /// <paramref name="modelId"/> — nothing else on the line is touched.
    /// </summary>
    /// <param name="modelId">The per-placement identity (an ObjectId-shaped u64 minted by the editor). Stamped
    /// onto the child entity so a gizmo in the viewport can find this exact line again.</param>
    /// <param name="meshPath">Content path to the mesh, hashed to its I64 ObjectId at placement.</param>
    /// <param name="material">Optional material name, resolved to its I32 handle; empty means default material.</param>
    /// <param name="pos">Local position in centimetres.</param>
    /// <param name="rot">Local rotation in degrees: (yaw, pitch, roll).</param>
    /// <param name="scale">Local scale multipliers.</param>
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

        // Identity first: this is the key everything downstream — gizmo, world file, reload — matches on.
        SceneNative.aver_scene_set_object_id(child, unchecked((long)modelId));

        // Mesh: path -> I64 ObjectId; material name -> I32 handle. Correct kinds, never a string setter.
        SceneNative.aver_scene_add_component(child, SceneIds.CMeshRenderer);
        SceneNative.aver_scene_set_i64(child, SceneIds.MeshMesh, Assets.ObjectIdOf(meshPath));
        if (!string.IsNullOrEmpty(material))
            SceneNative.aver_scene_set_i32(child, SceneIds.MeshMaterial, SceneNative.aver_scene_material(0, material));

        // Transform, then parent (keeping local space — the placement IS in the actor's frame).
        e.SetLocalPosition(new Vec3(pos.X, pos.Y, pos.Z));
        e.SetLocalRotation(new Rot(rot.Yaw, rot.Pitch, rot.Roll).ToQuat());
        e.SetLocalScale(new Vec3(scale.X, scale.Y, scale.Z));
        SceneNative.aver_scene_set_parent(child, _root.Handle);

        return new ModelHandle(e, modelId);
    }
}
