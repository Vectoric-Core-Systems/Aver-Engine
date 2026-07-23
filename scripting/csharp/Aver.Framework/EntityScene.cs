using System.Collections.Generic;
using Aver.Scene;   // Vec3, SceneIds, Assets

namespace Aver.Framework;

// The scene-access half of Entity: the hierarchy, component management, tags, and the generic component
// FIELD accessors the typed conveniences (Visible, SetMesh, Tags...) are built on. Kept in its own partial
// so Entity.cs stays focused on identity and the transform. All of it reads/writes the live world through
// the scene C ABI; a call on a stale handle is a harmless no-op returning a neutral value.
public readonly partial struct Entity
{
    // ---- hierarchy ----

    /// <summary>Immediate parent, or <see cref="Entity.None"/> for a root or a stale handle.</summary>
    public Entity Parent => new(SceneNative.aver_scene_parent(Handle));

    /// <summary>First child, or <see cref="Entity.None"/>. Prefer <see cref="Children"/> to walk them.</summary>
    public Entity FirstChild => new(SceneNative.aver_scene_first_child(Handle));

    /// <summary>Next sibling under the same parent, or <see cref="Entity.None"/> when this is the last.</summary>
    public Entity NextSibling => new(SceneNative.aver_scene_next_sibling(Handle));

    /// <summary>Number of immediate children.</summary>
    public int ChildCount => SceneNative.aver_scene_child_count(Handle);

    /// <summary>The immediate children, in link order. Snapshot the sequence before reparenting during it.</summary>
    public IEnumerable<Entity> Children
    {
        get { for (Entity c = FirstChild; c.IsValid; c = c.NextSibling) yield return c; }
    }

    /// <summary>Reparent under <paramref name="parent"/> (<see cref="Entity.None"/> makes this a root).
    /// Refuses a cycle or a self-parent. False on rejection.</summary>
    public bool SetParent(Entity parent) => SceneNative.aver_scene_set_parent(Handle, parent.Handle) != 0;

    /// <summary>Detach from any parent, making this a root.</summary>
    public bool Detach() => SceneNative.aver_scene_set_parent(Handle, 0) != 0;

    // ---- component management ----

    /// <summary>True if this entity currently carries <paramref name="component"/>.</summary>
    public bool HasComponent(Component component) => SceneNative.aver_scene_has_component(Handle, (int)component) != 0;

    /// <summary>Attach <paramref name="component"/> (idempotent). False if the type or handle is bad.</summary>
    public bool AddComponent(Component component) => SceneNative.aver_scene_add_component(Handle, (int)component) != 0;

    // ---- generic component field access ----
    // Read/write ANY component field by its qualified name ("CLight.intensityLux", an [Editable] field, ...).
    // The name resolves to a dense id once and is cached (SceneIds.Field), so repeated access is cheap. A
    // read of an unknown field or an absent component returns a neutral default; a write returns false.

    /// <summary>Read a float field by qualified name, or 0 if absent.</summary>
    public float GetFloat(string field) => SceneNative.aver_scene_get_f32(Handle, SceneIds.Field(field));
    /// <summary>Write a float field. False if the field is unknown or the component is absent.</summary>
    public bool SetFloat(string field, float value) => SceneNative.aver_scene_set_f32(Handle, SceneIds.Field(field), value) != 0;

    /// <summary>Read an int (or bool) field, or 0 if absent.</summary>
    public int GetInt(string field) => SceneNative.aver_scene_get_i32(Handle, SceneIds.Field(field));
    /// <summary>Write an int (or bool) field. False if unknown/absent.</summary>
    public bool SetInt(string field, int value) => SceneNative.aver_scene_set_i32(Handle, SceneIds.Field(field), value) != 0;

    /// <summary>Read a 64-bit field (an ObjectId/mesh id family), or 0 if absent.</summary>
    public long GetInt64(string field) => SceneNative.aver_scene_get_i64(Handle, SceneIds.Field(field));
    /// <summary>Write a 64-bit field. False if unknown/absent.</summary>
    public bool SetInt64(string field, long value) => SceneNative.aver_scene_set_i64(Handle, SceneIds.Field(field), value) != 0;

    /// <summary>Read a Vec3 field, or <see cref="Vec3.Zero"/> if absent.</summary>
    public Vec3 GetVec3(string field)
    {
        float[] o = Scratch3;
        return SceneNative.aver_scene_get_vec(Handle, SceneIds.Field(field), o) != 0 ? new Vec3(o[0], o[1], o[2]) : Vec3.Zero;
    }
    /// <summary>Write a Vec3 field. False if unknown/absent.</summary>
    public bool SetVec3(string field, Vec3 value)
    {
        float[] s = Scratch3;
        s[0] = value.X; s[1] = value.Y; s[2] = value.Z;
        return SceneNative.aver_scene_set_vec(Handle, SceneIds.Field(field), s) != 0;
    }

    /// <summary>Read a string field, or "" if absent.</summary>
    public string GetString(string field) => Fw.Str(SceneNative.aver_scene_get_str(Handle, SceneIds.Field(field)));
    /// <summary>Write a string field. False if unknown/absent.</summary>
    public bool SetString(string field, string value) => SceneNative.aver_scene_set_str(Handle, SceneIds.Field(field), value) != 0;

    // ---- typed component conveniences ----

    private const int MeshVisibleBit = 0x1;   // CMeshRenderer.flags bit 0 == kMeshRendererVisible

    /// <summary>Whether this entity's mesh is drawn (CMeshRenderer visible bit). False if it has no mesh.</summary>
    public bool Visible => (GetInt("CMeshRenderer.flags") & MeshVisibleBit) != 0;

    /// <summary>Show or hide the mesh. Adds a mesh renderer if the entity lacks one, so a bare entity can
    /// be made drawable; combine with <see cref="SetMesh"/>.</summary>
    public bool SetVisible(bool visible)
    {
        AddComponent(Component.MeshRenderer);
        int flags = GetInt("CMeshRenderer.flags");
        return SetInt("CMeshRenderer.flags", visible ? (flags | MeshVisibleBit) : (flags & ~MeshVisibleBit));
    }

    /// <summary>Set the drawn mesh by asset path (hashed to its ObjectId). Adds a mesh renderer if absent.</summary>
    public bool SetMesh(string meshPath)
    {
        AddComponent(Component.MeshRenderer);
        return SetInt64("CMeshRenderer.mesh", Assets.ObjectIdOf(meshPath));
    }

    /// <summary>Set the material by name (resolved to its opaque handle). Adds a mesh renderer if absent.</summary>
    public bool SetMaterial(string materialName)
    {
        AddComponent(Component.MeshRenderer);
        return SetInt("CMeshRenderer.material", SceneNative.aver_scene_material(0, materialName));
    }

    // ---- tags (CTags.bits, a 32-bit mask the gameplay layer gives meaning to) ----

    /// <summary>The raw 32-bit tag mask, or 0 if the entity has no tags. Define your own bit constants.</summary>
    public uint Tags => unchecked((uint)GetInt("CTags.bits"));

    /// <summary>Replace the whole tag mask. Adds a Tags component if the entity lacks one.</summary>
    public bool SetTags(uint mask)
    {
        AddComponent(Component.Tags);
        return SetInt("CTags.bits", unchecked((int)mask));
    }

    /// <summary>True if EVERY bit in <paramref name="mask"/> is set (and the mask is non-zero).</summary>
    public bool HasTag(uint mask) => mask != 0 && (Tags & mask) == mask;

    /// <summary>True if ANY bit in <paramref name="mask"/> is set.</summary>
    public bool HasAnyTag(uint mask) => (Tags & mask) != 0;

    /// <summary>Set the bits in <paramref name="mask"/>.</summary>
    public bool AddTag(uint mask) => SetTags(Tags | mask);

    /// <summary>Clear the bits in <paramref name="mask"/>.</summary>
    public bool RemoveTag(uint mask) => SetTags(Tags & ~mask);
}
