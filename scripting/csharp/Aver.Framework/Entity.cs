// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// Entity handle: a 32-bit key into the native scene, with transform, naming and identity accessors.

using Aver.Scene;

namespace Aver.Framework;

/// <summary>A handle to one entity in the world. <c>default</c> is invalid (<c>Handle == 0</c>).</summary>
public readonly partial struct Entity : IEquatable<Entity>
{
    /// <summary>The raw ABI handle. 0 is invalid; a live entity is always positive.</summary>
    public int Handle { get; }

    internal Entity(int handle) => Handle = handle;

    /// <summary>The invalid handle. Equivalent to <c>default</c>.</summary>
    public static Entity None => default;

    public bool IsValid => Handle != 0;

    // Per-thread scratch buffers for the ABI float arrays.
    [ThreadStatic] private static float[]? s_scratch3;
    [ThreadStatic] private static float[]? s_scratch4;
    internal static float[] Scratch3 => s_scratch3 ??= new float[3];
    internal static float[] Scratch4 => s_scratch4 ??= new float[4];

    // Transform writes are methods, not property setters: this struct is returned by value, and
    // assigning to a property of a by-value return is CS1612.

    /// <summary>Local position in centimetres. Zero for a stale handle.</summary>
    public Vec3 LocalPosition
    {
        get
        {
            float[] o = Scratch3;
            if (SceneNative.aver_scene_get_vec(Handle, SceneIds.LocalPosition, o) == 0) return Vec3.Zero;
            return new Vec3(o[0], o[1], o[2]);
        }
    }

    /// <summary>Sets local position, in centimetres.</summary>
    public void SetLocalPosition(Vec3 v)
    {
        float[] s = Scratch3;
        s[0] = v.X; s[1] = v.Y; s[2] = v.Z;
        SceneNative.aver_scene_set_vec(Handle, SceneIds.LocalPosition, s);
    }

    /// <summary>Moves by <paramref name="delta"/> centimetres, relative to the current local position.</summary>
    public void Translate(Vec3 delta) => SetLocalPosition(LocalPosition + delta);

    /// <summary>Local rotation as the stored quaternion.</summary>
    public Quat LocalRotation
    {
        get
        {
            float[] o = Scratch4;
            if (SceneNative.aver_scene_get_vec(Handle, SceneIds.LocalRotation, o) == 0) return Quat.Identity;
            return new Quat(o[0], o[1], o[2], o[3]);
        }
    }

    /// <summary>Sets local rotation. Compose in degrees via <see cref="Rot"/> and <see cref="Rot.ToQuat"/>.</summary>
    public void SetLocalRotation(Quat q)
    {
        float[] s = Scratch4;
        s[0] = q.X; s[1] = q.Y; s[2] = q.Z; s[3] = q.W;
        SceneNative.aver_scene_set_vec(Handle, SceneIds.LocalRotation, s);
    }

    /// <summary>Local scale (unitless multipliers). One for a stale handle.</summary>
    public Vec3 LocalScale
    {
        get
        {
            float[] o = Scratch3;
            if (SceneNative.aver_scene_get_vec(Handle, SceneIds.LocalScale, o) == 0) return Vec3.One;
            return new Vec3(o[0], o[1], o[2]);
        }
    }

    /// <summary>Sets local scale (unitless multipliers).</summary>
    public void SetLocalScale(Vec3 v)
    {
        float[] s = Scratch3;
        s[0] = v.X; s[1] = v.Y; s[2] = v.Z;
        SceneNative.aver_scene_set_vec(Handle, SceneIds.LocalScale, s);
    }

    /// <summary>The entity's forward axis (+X) rotated by its local rotation.</summary>
    public Vec3 Forward => Rotate(LocalRotation, Vec3.Forward);

    /// <summary>The entity's right axis (+Y), rotated by its local rotation.</summary>
    public Vec3 Right => Rotate(LocalRotation, Vec3.Right);

    /// <summary>The entity's up axis (+Z), rotated by its local rotation.</summary>
    public Vec3 Up => Rotate(LocalRotation, Vec3.Up);

    [ThreadStatic] private static float[]? s_scratch16;
    private static float[] Scratch16 => s_scratch16 ??= new float[16];

    /// <summary>World-space position in centimetres, composed through any parents.</summary>
    public Vec3 WorldPosition
    {
        get
        {
            float[] m = Scratch16;
            if (SceneNative.aver_scene_world_matrix(Handle, m) == 0) return Vec3.Zero;
            return new Vec3(m[12], m[13], m[14]);   // translation is row 3 (row-vector convention)
        }
    }

    /// <summary>World-space forward (+X), composed through parents and normalised.</summary>
    public Vec3 WorldForward => WorldAxis(0, Vec3.Forward);
    /// <summary>World-space right (+Y).</summary>
    public Vec3 WorldRight => WorldAxis(1, Vec3.Right);
    /// <summary>World-space up (+Z).</summary>
    public Vec3 WorldUp => WorldAxis(2, Vec3.Up);

    // Returns one normalised basis row of the world matrix, falling back to the local axis.
    private Vec3 WorldAxis(int row, Vec3 localAxis)
    {
        float[] m = Scratch16;
        if (SceneNative.aver_scene_world_matrix(Handle, m) == 0) return Rotate(LocalRotation, localAxis);
        int i = row * 4;
        var v = new Vec3(m[i], m[i + 1], m[i + 2]);
        float len = MathF.Sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z);
        return len > 1e-6f ? v * (1f / len) : Rotate(LocalRotation, localAxis);
    }

    /// <summary>The display name.</summary>
    public string Name => Fw.Str(SceneNative.aver_scene_name(Handle));

    /// <summary>Renames the entity, rewriting its <c>CName</c> blob slice.</summary>
    public void SetName(string name) => SceneNative.aver_scene_set_name(Handle, name);

    /// <summary>The persisted identity — <c>CName.objectId</c>, the fnv1a64 that survives serialisation.</summary>
    public ulong ObjectId => unchecked((ulong)SceneNative.aver_scene_object_id(Handle));

    /// <summary>True while this handle still addresses a live entity.</summary>
    public bool IsAlive => SceneNative.aver_scene_valid(Handle) != 0;

    /// <summary>True if a gameplay class owns this entity — i.e. it is an actor, not a plain scene entity.</summary>
    public bool IsActor => Fw.aver_fw_class_of(Handle) != 0;

    /// <summary>The gameplay class this entity is an instance of; invalid for a plain entity.</summary>
    public ActorClass Class => new ActorClass(Fw.aver_fw_class_of(Handle));

    /// <summary>The live managed actor bound to this entity, or null.</summary>
    public AverActor? Actor => Actors.Get(this);

    /// <summary>The managed actor bound to this entity as <typeparamref name="T"/>, or null.</summary>
    public T? As<T>() where T : AverActor => Actors.Get<T>(this);

    /// <summary>Destroys this entity and its subtree; an actor runs the full framework teardown.</summary>
    public void Destroy()
    {
        if (IsActor) Fw.aver_fw_destroy(Handle);
        else SceneNative.aver_scene_destroy(Handle);
    }

    /// <summary>Rotates <paramref name="v"/> by unit quaternion <paramref name="q"/> (v' = q v q*).</summary>
    private static Vec3 Rotate(Quat q, Vec3 v)
    {
        float tx = 2f * (q.Y * v.Z - q.Z * v.Y);
        float ty = 2f * (q.Z * v.X - q.X * v.Z);
        float tz = 2f * (q.X * v.Y - q.Y * v.X);
        return new Vec3(
            v.X + q.W * tx + (q.Y * tz - q.Z * ty),
            v.Y + q.W * ty + (q.Z * tx - q.X * tz),
            v.Z + q.W * tz + (q.X * ty - q.Y * tx));
    }

    public bool Equals(Entity o) => Handle == o.Handle;
    public override bool Equals(object? o) => o is Entity e && Equals(e);
    public override int GetHashCode() => Handle;
    public override string ToString() => IsValid ? $"Entity#{Handle}" : "Entity.None";
}
