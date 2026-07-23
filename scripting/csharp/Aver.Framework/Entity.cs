using Aver.Scene;

namespace Aver.Framework;

/// <summary>
/// A handle to one entity in the world — the single field every base type wears a familiar name over.
/// It is NOT the storage: the world is dense component arrays in the built scene module, and this struct
/// is a 32-bit key into them. A default <see cref="Entity"/> is invalid (<c>Handle == 0</c>), honouring
/// the engine-wide "0 == invalid" contract, so <c>default</c> is safe to hand around and test.
/// </summary>
/// <remarks>
/// <para>
/// The transform accessors read and write <c>CLocal</c> — position and scale in centimetres, rotation as
/// the quaternion the field stores. Authors compose rotations in degrees through <see cref="Rot"/> and
/// convert with <see cref="Rot.ToQuat"/>; this type never invents an Euler convention of its own.
/// </para>
/// <para>
/// <b>No per-call allocation on the transform path.</b> Getting or setting a vector needs a small float
/// buffer to cross the ABI; a fresh array every frame per moving actor is exactly the GC pressure the
/// design set out to avoid, so the buffers are <c>[ThreadStatic]</c> scratch reused per call. The buffer
/// is handed to a synchronous P/Invoke and consumed before the call returns, so reuse is safe.
/// </para>
/// </remarks>
public readonly struct Entity : IEquatable<Entity>
{
    /// <summary>The raw ABI handle. 0 is invalid; a live entity is always positive (bit 31 stays clear).</summary>
    public int Handle { get; }

    internal Entity(int handle) => Handle = handle;

    /// <summary>The invalid handle. Equivalent to <c>default</c>.</summary>
    public static Entity None => default;

    public bool IsValid => Handle != 0;

    // --- Per-thread scratch. Vec3 needs 3 floats; Quat needs 4. Lazily allocated once per thread. ---
    [ThreadStatic] private static float[]? s_scratch3;
    [ThreadStatic] private static float[]? s_scratch4;
    internal static float[] Scratch3 => s_scratch3 ??= new float[3];
    internal static float[] Scratch4 => s_scratch4 ??= new float[4];

    // Transform WRITES are methods, not property setters, and that is deliberate. This is a handle
    // struct returned BY VALUE (Actor.Self, ModelHandle.Entity), and C# forbids assigning to a
    // property of a by-value return — `Self.LocalPosition = v` is CS1612 even though the setter only
    // P/Invokes and never touches `this`. A method has no such restriction and reads the same on a
    // local or an rvalue, so the transform reads below are get-only and the writes are Set*/Translate.

    /// <summary>Local position in centimetres. Neutral <see cref="Vec3.Zero"/> for a stale handle.</summary>
    public Vec3 LocalPosition
    {
        get
        {
            float[] o = Scratch3;
            if (SceneNative.aver_scene_get_vec(Handle, SceneIds.LocalPosition, o) == 0) return Vec3.Zero;
            return new Vec3(o[0], o[1], o[2]);
        }
    }

    /// <summary>Set local position, in centimetres.</summary>
    public void SetLocalPosition(Vec3 v)
    {
        float[] s = Scratch3;
        s[0] = v.X; s[1] = v.Y; s[2] = v.Z;
        SceneNative.aver_scene_set_vec(Handle, SceneIds.LocalPosition, s);
    }

    /// <summary>Move by <paramref name="delta"/> centimetres, relative to the current local position.</summary>
    public void Translate(Vec3 delta) => SetLocalPosition(LocalPosition + delta);

    /// <summary>Local rotation as the stored quaternion. Compose in degrees via <see cref="Rot"/>.</summary>
    public Quat LocalRotation
    {
        get
        {
            float[] o = Scratch4;
            if (SceneNative.aver_scene_get_vec(Handle, SceneIds.LocalRotation, o) == 0) return Quat.Identity;
            return new Quat(o[0], o[1], o[2], o[3]);
        }
    }

    /// <summary>Set local rotation. Compose in degrees via <see cref="Rot"/> and <see cref="Rot.ToQuat"/>.</summary>
    public void SetLocalRotation(Quat q)
    {
        float[] s = Scratch4;
        s[0] = q.X; s[1] = q.Y; s[2] = q.Z; s[3] = q.W;
        SceneNative.aver_scene_set_vec(Handle, SceneIds.LocalRotation, s);
    }

    /// <summary>Local scale (unitless multipliers). Neutral <see cref="Vec3.One"/> for a stale handle.</summary>
    public Vec3 LocalScale
    {
        get
        {
            float[] o = Scratch3;
            if (SceneNative.aver_scene_get_vec(Handle, SceneIds.LocalScale, o) == 0) return Vec3.One;
            return new Vec3(o[0], o[1], o[2]);
        }
    }

    /// <summary>Set local scale (unitless multipliers).</summary>
    public void SetLocalScale(Vec3 v)
    {
        float[] s = Scratch3;
        s[0] = v.X; s[1] = v.Y; s[2] = v.Z;
        SceneNative.aver_scene_set_vec(Handle, SceneIds.LocalScale, s);
    }

    /// <summary>
    /// The entity's forward axis (+X) rotated by its local rotation — the direction a pawn drives when it
    /// moves along <c>Self.Forward</c>. Left-handed, matching the coordinate contract.
    /// </summary>
    public Vec3 Forward => Rotate(LocalRotation, Vec3.Forward);

    /// <summary>The entity's right axis (+Y), rotated by its local rotation.</summary>
    public Vec3 Right => Rotate(LocalRotation, Vec3.Right);

    /// <summary>The entity's up axis (+Z), rotated by its local rotation.</summary>
    public Vec3 Up => Rotate(LocalRotation, Vec3.Up);

    // --- World-space transform, composed through the parent chain. Position is the common case; the axes
    //     are the world matrix's basis rows, normalised so a parent's scale does not skew a direction. ---
    [ThreadStatic] private static float[]? s_scratch16;
    private static float[] Scratch16 => s_scratch16 ??= new float[16];

    /// <summary>World-space position in centimetres, composed through any parents. Zero for a stale handle.</summary>
    public Vec3 WorldPosition
    {
        get
        {
            float[] m = Scratch16;
            if (SceneNative.aver_scene_world_matrix(Handle, m) == 0) return Vec3.Zero;
            return new Vec3(m[12], m[13], m[14]);   // translation is row 3 (row-vector convention)
        }
    }

    /// <summary>World-space forward (+X), composed through parents and normalised. Falls back to the local
    /// axis for a stale handle.</summary>
    public Vec3 WorldForward => WorldAxis(0, Forward);
    /// <summary>World-space right (+Y).</summary>
    public Vec3 WorldRight => WorldAxis(1, Right);
    /// <summary>World-space up (+Z).</summary>
    public Vec3 WorldUp => WorldAxis(2, Up);

    private Vec3 WorldAxis(int row, Vec3 fallback)
    {
        float[] m = Scratch16;
        if (SceneNative.aver_scene_world_matrix(Handle, m) == 0) return fallback;
        int i = row * 4;
        var v = new Vec3(m[i], m[i + 1], m[i + 2]);
        float len = MathF.Sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z);
        return len > 1e-6f ? v * (1f / len) : fallback;
    }

    /// <summary>The display name. Read-only here; <see cref="SetName"/> renames the <c>CName</c> slice.</summary>
    public string Name => Fw.Str(SceneNative.aver_scene_name(Handle));

    /// <summary>Rename the entity, rewriting its <c>CName</c> blob slice.</summary>
    public void SetName(string name) => SceneNative.aver_scene_set_name(Handle, name);

    /// <summary>
    /// The persisted identity — <c>CName.objectId</c>, the Assets fnv1a64 that survives serialisation.
    /// Runtime identity (<see cref="Handle"/>) is never persisted; this is. It is also the key a placed
    /// model carries so a dragged gizmo can find its generated line (see <see cref="ActorBuilder.Place"/>).
    /// </summary>
    public ulong ObjectId => unchecked((ulong)SceneNative.aver_scene_object_id(Handle));

    // --- Gameplay identity. An entity is an "actor" iff a framework class owns it (class_of != 0). ---

    /// <summary>True while this handle still addresses a live entity (rejects a stale generation or freed slot).</summary>
    public bool IsAlive => SceneNative.aver_scene_valid(Handle) != 0;

    /// <summary>True if a gameplay class owns this entity — i.e. it is an actor, not a plain scene entity.</summary>
    public bool IsActor => Fw.aver_fw_class_of(Handle) != 0;

    /// <summary>The gameplay class this entity is an instance of; invalid (<c>Handle == 0</c>) for a plain entity.</summary>
    public ActorClass Class => new ActorClass(Fw.aver_fw_class_of(Handle));

    /// <summary>The live managed actor bound to this entity, or null (dead, not an actor, or script disabled).</summary>
    public AverActor? Actor => Actors.Get(this);

    /// <summary>The managed actor bound to this entity as <typeparamref name="T"/>, or null if absent or another type.</summary>
    public T? As<T>() where T : AverActor => Actors.Get<T>(this);

    /// <summary>Destroy this entity. An actor runs the full framework teardown (OnEndPlay -> unbind); a plain
    /// entity is a scene destroy. Either takes the whole subtree and is deferred to the next world flush.</summary>
    public void Destroy()
    {
        if (IsActor) Fw.aver_fw_destroy(Handle);
        else SceneNative.aver_scene_destroy(Handle);
    }

    /// <summary>Rotate <paramref name="v"/> by unit quaternion <paramref name="q"/> (v' = q v q*).</summary>
    private static Vec3 Rotate(Quat q, Vec3 v)
    {
        // Standard quaternion-vector rotation. q is assumed unit (it comes from CLocal.rotation).
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
