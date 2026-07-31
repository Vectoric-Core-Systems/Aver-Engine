// The scene half of Entity: hierarchy, component management, generic field access, mesh and tags.

using System.Collections.Generic;
using Aver.Scene;

namespace Aver.Framework;

// Entity, continued: everything that reads or writes the live world through the scene C ABI.
public readonly partial struct Entity
{
    /// <summary>Immediate parent, or <see cref="Entity.None"/> for a root or a stale handle.</summary>
    public Entity Parent => new(SceneNative.aver_scene_parent(Handle));

    /// <summary>First child, or <see cref="Entity.None"/>.</summary>
    public Entity FirstChild => new(SceneNative.aver_scene_first_child(Handle));

    /// <summary>Next sibling under the same parent, or <see cref="Entity.None"/>.</summary>
    public Entity NextSibling => new(SceneNative.aver_scene_next_sibling(Handle));

    /// <summary>Number of immediate children.</summary>
    public int ChildCount => SceneNative.aver_scene_child_count(Handle);

    /// <summary>The immediate children, in link order.</summary>
    public IEnumerable<Entity> Children
    {
        get { for (Entity c = FirstChild; c.IsValid; c = c.NextSibling) yield return c; }
    }

    /// <summary>Reparents under <paramref name="parent"/>. False on rejection (a cycle or a self-parent).</summary>
    public bool SetParent(Entity parent) => SceneNative.aver_scene_set_parent(Handle, parent.Handle) != 0;

    /// <summary>Detaches from any parent, making this a root.</summary>
    public bool Detach() => SceneNative.aver_scene_set_parent(Handle, 0) != 0;

    /// <summary>True if this entity currently carries <paramref name="component"/>.</summary>
    public bool HasComponent(Component component) => SceneNative.aver_scene_has_component(Handle, (int)component) != 0;

    /// <summary>Attaches <paramref name="component"/> (idempotent). False if the type or handle is bad.</summary>
    public bool AddComponent(Component component) => SceneNative.aver_scene_add_component(Handle, (int)component) != 0;

    /// <summary>Reads a float field by qualified name, or 0 if absent.</summary>
    public float GetFloat(string field) => SceneNative.aver_scene_get_f32(Handle, SceneIds.Field(field));
    /// <summary>Writes a float field. False if the field is unknown or the component is absent.</summary>
    public bool SetFloat(string field, float value) => SceneNative.aver_scene_set_f32(Handle, SceneIds.Field(field), value) != 0;

    /// <summary>Reads an int (or bool) field, or 0 if absent.</summary>
    public int GetInt(string field) => SceneNative.aver_scene_get_i32(Handle, SceneIds.Field(field));
    /// <summary>Writes an int (or bool) field. False if unknown or absent.</summary>
    public bool SetInt(string field, int value) => SceneNative.aver_scene_set_i32(Handle, SceneIds.Field(field), value) != 0;

    /// <summary>Reads a 64-bit field, or 0 if absent.</summary>
    public long GetInt64(string field) => SceneNative.aver_scene_get_i64(Handle, SceneIds.Field(field));
    /// <summary>Writes a 64-bit field. False if unknown or absent.</summary>
    public bool SetInt64(string field, long value) => SceneNative.aver_scene_set_i64(Handle, SceneIds.Field(field), value) != 0;

    /// <summary>Reads a Vec3 field, or <see cref="Vec3.Zero"/> if absent or not a Vec3.</summary>
    public Vec3 GetVec3(string field)
    {
        int id = SceneIds.Field(field);
        // get_vec copies sizeof(float)*arity bytes, so a Quat or Mat4 field would overrun the 3-float scratch.
        if (SceneNative.aver_scene_field_arity(id) != 3) return Vec3.Zero;
        float[] o = Scratch3;
        return SceneNative.aver_scene_get_vec(Handle, id, o) != 0 ? new Vec3(o[0], o[1], o[2]) : Vec3.Zero;
    }
    /// <summary>Writes a Vec3 field. False if unknown, absent, or not arity 3.</summary>
    public bool SetVec3(string field, Vec3 value)
    {
        int id = SceneIds.Field(field);
        if (SceneNative.aver_scene_field_arity(id) != 3) return false;   // see GetVec3: refuse Quat/Mat4
        float[] s = Scratch3;
        s[0] = value.X; s[1] = value.Y; s[2] = value.Z;
        return SceneNative.aver_scene_set_vec(Handle, id, s) != 0;
    }

    /// <summary>Reads a string field, or "" if absent.</summary>
    public string GetString(string field) => Fw.Str(SceneNative.aver_scene_get_str(Handle, SceneIds.Field(field)));
    /// <summary>Writes a string field. False if unknown or absent.</summary>
    public bool SetString(string field, string value) => SceneNative.aver_scene_set_str(Handle, SceneIds.Field(field), value) != 0;

    private const int MeshVisibleBit = 0x1;   // CMeshRenderer.flags bit 0 == kMeshRendererVisible

    // Attaches a mesh renderer if the entity has none, seeding the visible bit that add_component zero-fills.
    private void EnsureMeshRenderer()
    {
        if (HasComponent(Component.MeshRenderer)) return;
        AddComponent(Component.MeshRenderer);
        SetInt("CMeshRenderer.flags", MeshVisibleBit);
    }

    /// <summary>Whether this entity's mesh is drawn. False if it has no mesh.</summary>
    public bool Visible => (GetInt("CMeshRenderer.flags") & MeshVisibleBit) != 0;

    /// <summary>Shows or hides the mesh, adding a mesh renderer if the entity lacks one.</summary>
    public bool SetVisible(bool visible)
    {
        EnsureMeshRenderer();
        int flags = GetInt("CMeshRenderer.flags");
        return SetInt("CMeshRenderer.flags", visible ? (flags | MeshVisibleBit) : (flags & ~MeshVisibleBit));
    }

    /// <summary>Sets the drawn mesh by asset path, adding a mesh renderer if absent.</summary>
    public bool SetMesh(string meshPath)
    {
        EnsureMeshRenderer();
        return SetInt64("CMeshRenderer.mesh", Assets.ObjectIdOf(meshPath));
    }

    /// <summary>Sets the material by name, adding a mesh renderer if absent.</summary>
    public bool SetMaterial(string materialName)
    {
        EnsureMeshRenderer();
        return SetInt("CMeshRenderer.material", SceneNative.aver_scene_material(0, materialName));
    }

    /// <summary>The raw 32-bit tag mask (CTags.bits), or 0 if the entity has no tags.</summary>
    public uint Tags => unchecked((uint)GetInt("CTags.bits"));

    /// <summary>Replaces the whole tag mask, adding a Tags component if absent.</summary>
    public bool SetTags(uint mask)
    {
        AddComponent(Component.Tags);
        return SetInt("CTags.bits", unchecked((int)mask));
    }

    /// <summary>True if every bit in <paramref name="mask"/> is set, and the mask is non-zero.</summary>
    public bool HasTag(uint mask) => mask != 0 && (Tags & mask) == mask;

    /// <summary>True if any bit in <paramref name="mask"/> is set.</summary>
    public bool HasAnyTag(uint mask) => (Tags & mask) != 0;

    /// <summary>Sets the bits in <paramref name="mask"/>.</summary>
    public bool AddTag(uint mask) => SetTags(Tags | mask);

    /// <summary>Clears the bits in <paramref name="mask"/>.</summary>
    public bool RemoveTag(uint mask) => SetTags(Tags & ~mask);
}
