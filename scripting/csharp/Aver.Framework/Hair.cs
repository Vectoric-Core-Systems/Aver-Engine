// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Strand hair from a script: the CHair surface on Entity and the world wind. Design: docs/plans/wave11/SOFTBODY_HAIR.md 2.20, docs/HAIR.md.
//
// CHair is registered at RUNTIME (like CCloth), so adding and configuring it is a field write over the generic scene ABI and a host that never
// registered it gets false, not a throw. The component goes on the entity that owns the skeleton the scalp is skinned to (the character root). A
// groom is built by the hair system at its next tick; what HAPPENS (a teleport, a collider) goes through the framework relay
// (framework_hair_abi.h) and applies at once, in arrival order. Scales read 0 as 1 on the native side, so a zero-filled component is the default.

using Aver.Scene;

namespace Aver.Framework;

/// <summary>The shape of a hair collider (pinned to AVER_FW_HAIR_SHAPE_*, the .ochair PROXY shapes).</summary>
public enum HairColliderShape
{
    /// <summary>Size.X is the radius.</summary>
    Sphere = 0,
    /// <summary>Size.X is the radius, Size.Y the half height of the straight part (along engine +Z).</summary>
    Capsule = 1,
    /// <summary>Size is the half extents.</summary>
    Box = 2,
}

/// <summary>What an entity's hair is doing: simulating, frozen by distance, inert (the groom did not build, or there is no compute device),
/// still building, the strand level it is drawn at (0 every strand .. 3 the fewest), how many strands are drawn and simulated.</summary>
public readonly record struct HairState(bool Simulating, bool Frozen, bool Inert, bool Building, bool Built, int Lod, int RenderStrands, int SimStrands, int Colliders);

public readonly partial struct Entity
{
    private const string HairType = "CHair";
    private const int HairFlagNoCollision = 0x1, HairFlagNoShadow = 0x2, HairFlagNoWind = 0x8, HairFlagHidden = 0x10;

    // False, not a throw, in a process with no native scene (the bare-process graph tests, a tool).
    private bool HairHas()
    {
        try { return HasComponent(HairType); }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    // A scale field reads 0 as "the default 1" on the native side; a negative or non-finite value is refused here.
    private bool HairSetScale(string field, float value)
    {
        if (!float.IsFinite(value) || value < 0f || !HairHas()) return false;
        try { return SetFloat("CHair." + field, value); }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    private bool HairSetFlag(int bit, bool on)
    {
        if (!HairHas()) return false;
        try
        {
            int flags = GetInt("CHair.flags");
            return SetInt("CHair.flags", on ? flags | bit : flags & ~bit);
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>Gives this character strand hair from an .ochair groom. The component goes on the entity that owns the skeleton (the character
    /// root); the hair system builds the groom at its next tick, makes a child entity "Hair" that carries the draw, and shares the build with every
    /// other entity using the same groom. False for a dead entity, an empty asset or a build without the hair module.</summary>
    public bool AddHair(string asset)
    {
        if (string.IsNullOrEmpty(asset)) return false;
        try
        {
            if (!HairHas() && !AddComponent(HairType)) return false;
            return SetInt64("CHair.asset", Assets.ObjectIdOf(asset));
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>Draws the hair with this .ocmat (a material whose shading model is hair) instead of the groom's own look. An empty string goes
    /// back to the groom's look.</summary>
    public bool SetHairLook(string materialAsset)
    {
        if (!HairHas()) return false;
        try { return SetInt64("CHair.material", string.IsNullOrEmpty(materialAsset) ? 0 : Assets.ObjectIdOf(materialAsset)); }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>Multiplies the width of every strand (the groom's look width times this). 1 is the groom's own.</summary>
    public bool SetHairWidthScale(float scale) => HairSetScale("widthScale", scale);

    /// <summary>The fraction of the render strands drawn on top of the distance levels, 0 to 1 (1 = all; the renderer clamps higher values).</summary>
    public bool SetHairDensityScale(float scale) => HairSetScale("densityScale", scale);

    /// <summary>Divides the bend and stretch compliance of every material of the groom, so 2 is twice as stiff. Shared by every instance of the
    /// groom: the last entity to ask wins.</summary>
    public bool SetHairStiffness(float scale) => HairSetScale("stiffnessScale", scale);

    /// <summary>Multiplies the world wind this hair feels. 0 is the default 1; use <see cref="SetHairWindEnabled"/> for none.</summary>
    public bool SetHairWindScale(float scale) => HairSetScale("windScale", scale);

    /// <summary>Whether the world wind moves this hair.</summary>
    public bool SetHairWindEnabled(bool on) => HairSetFlag(HairFlagNoWind, !on);

    /// <summary>Whether the hair collides with the character's head, neck and shoulder proxies and with colliders added by
    /// <see cref="AddHairCollider"/>. The layer is chosen when the hair is built, so this takes effect for a rebuilt groom.</summary>
    public bool SetHairCollides(bool on) => HairSetFlag(HairFlagNoCollision, !on);

    /// <summary>Whether the strands cast shadow-map shadows.</summary>
    public bool SetHairCastsShadow(bool on) => HairSetFlag(HairFlagNoShadow, !on);

    /// <summary>Shows or hides the hair. Hidden hair is neither drawn nor expanded; the simulation still follows the distance rules.</summary>
    public bool SetHairEnabled(bool on) => HairSetFlag(HairFlagHidden, !on);

    /// <summary>Puts the hair back in its default pose with no velocity at the next update, wherever the head now is (after a cut or a respawn).
    /// False while the groom is not built.</summary>
    public bool TeleportHair()
    {
        try { return FwHair.aver_fw_hair_teleport(Handle) != 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>Adds a hull that follows <paramref name="bone"/> of this character's skeleton and that hair collides with: a held prop, a hat, a
    /// shoulder pad. <paramref name="size"/> is in centimetres (see <see cref="HairColliderShape"/>), <paramref name="offset"/> in the bone's frame.
    /// At most 16 per character. The hull is made once the groom is built; a bone the skeleton does not have is logged and skipped then.</summary>
    public bool AddHairCollider(HairColliderShape shape, Vec3 size, string bone, Vec3 offset = default)
    {
        if (string.IsNullOrEmpty(bone) || (int)shape < 0 || (int)shape > 2) return false;
        try { return FwHair.aver_fw_hair_add_collider(Handle, (int)shape, size.X, size.Y, size.Z, offset.X, offset.Y, offset.Z, bone, out _) != 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>Removes every collider <see cref="AddHairCollider"/> added to this character.</summary>
    public bool ClearHairColliders()
    {
        try { return FwHair.aver_fw_hair_clear_colliders(Handle) != 0; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>What this character's hair is doing as of the last tick. False when it has no hair component.</summary>
    public bool TryGetHair(out HairState state)
    {
        state = default;
        int[] v = new int[6];
        try
        {
            if (FwHair.aver_fw_hair_state(Handle, v) == 0) return false;
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
        int s = v[0];
        state = new HairState((s & 0x1) != 0, (s & 0x2) != 0, (s & 0x4) != 0, (s & 0x8) != 0, v[5] != 0, v[1], v[2], v[3], v[4]);
        return true;
    }
}

/// <summary>Hair at world level.</summary>
public static class Hair
{
    /// <summary>The world wind the hair feels: the soft bodies' wind (<see cref="SoftBodies.SetWind"/>), which the hair system takes from the same
    /// place, so cloth, ropes and hair move together. Centimetres per second; the gust is an amplitude in cm/s.</summary>
    public static bool SetWind(Vec3 velocityCmPerS, float gustCmPerS = 0f, float gustHz = 0.3f, float turbulence = 0f)
    {
        try { return SoftBodies.SetWind(velocityCmPerS, gustCmPerS, gustHz, turbulence); }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }
}

/// <summary>The graph-node surface for the Hair family (GraphHairNodeTable.cs): the same calls with the int-entity / primitive-only signatures
/// GraphCompiler's emitted IL needs. A hair crosses the graph as its entity id.</summary>
internal static class HairGraphInterop
{
    internal static bool AddHairForGraph(int entity, string asset) => new Entity(entity).AddHair(asset ?? string.Empty);

    /// <summary>Width, density, stiffness and wind scale in one node; a negative value leaves that scale alone (so a node can set just one).</summary>
    internal static bool SetHairParamsForGraph(int entity, float width, float density, float stiffness, float wind)
    {
        var e = new Entity(entity);
        bool ok = true;
        if (width >= 0f) ok &= e.SetHairWidthScale(width);
        if (density >= 0f) ok &= e.SetHairDensityScale(density);
        if (stiffness >= 0f) ok &= e.SetHairStiffness(stiffness);
        if (wind >= 0f) ok &= e.SetHairWindScale(wind);
        return ok;
    }

    internal static bool SetHairLookForGraph(int entity, string material) => new Entity(entity).SetHairLook(material ?? string.Empty);

    internal static bool SetHairEnabledForGraph(int entity, bool on) => new Entity(entity).SetHairEnabled(on);

    internal static bool TeleportHairForGraph(int entity) => new Entity(entity).TeleportHair();

    internal static bool AddHairColliderForGraph(int entity, int shape, float sx, float sy, float sz, string bone) =>
        shape >= 0 && shape <= 2 && new Entity(entity).AddHairCollider((HairColliderShape)shape, new Vec3(sx, sy, sz), bone ?? string.Empty);

    internal static bool GetHairForGraph(int entity, out bool simulating, out int lod, out int renderStrands, out int simStrands)
    {
        bool ok = new Entity(entity).TryGetHair(out HairState s);
        simulating = s.Simulating;
        lod = s.Lod;
        renderStrands = s.RenderStrands;
        simStrands = s.SimStrands;
        return ok;
    }

    internal static bool SetHairWindForGraph(float x, float y, float z, float gust) => Hair.SetWind(new Vec3(x, y, z), gust);
}
