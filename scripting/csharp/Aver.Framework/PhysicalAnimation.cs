// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Physical animation from a script: hit reactions, partial-body simulation, per-bone drive, death and getting up
// on a skinned entity. Design and the .ocphysanim profile: docs/physics/PHYSICAL_ANIMATION.md.
//
// RELAYED through the framework like ragdolls are (aver_fw_physanim_*): the framework links neither the animation
// module nor physics, so a host that installed no provider answers false from every call here rather than throwing.

using Aver.Scene;

namespace Aver.Framework;

/// <summary>Where a character is between being alive and dead. Pinned to the ragdoll module's Life.</summary>
public enum LifeState
{
    Alive = 0,
    /// <summary>The motors are letting go over the profile's death time.</summary>
    Dying = 1,
    /// <summary>Limp.</summary>
    Dead = 2,
    /// <summary>The get-up montage plays while the simulation blends out.</summary>
    GettingUp = 3,
}

/// <summary>How a fallen body lies. Pinned to the ragdoll module's Facing.</summary>
public enum BodyFacing
{
    Upright = 0,
    /// <summary>On its back, face up.</summary>
    Supine = 1,
    /// <summary>On its front, face down.</summary>
    Prone = 2,
    Side = 3,
}

/// <summary>
/// Something hit an entity. The one description of a hit that physical animation (hit reactions) and destruction
/// share: whatever deals damage fills one in (a weapon, an explosion, a game converted by Fabric mapping its own
/// damage event onto it) and hands it to <see cref="Entity.ApplyHit"/>. Nothing in the engine is keyed on a game.
/// </summary>
/// <param name="Target">The entity that was hit.</param>
/// <param name="Bone">The bone that was hit; null or empty = the root part.</param>
/// <param name="Impulse">World space, kg·cm/s. About 3000 is a full reaction with the default profile.</param>
/// <param name="Point">World space, centimetres. Only a radial event needs it.</param>
/// <param name="Damage">Gameplay damage; carried for consumers that want it.</param>
/// <param name="Radius">&gt; 0 makes it a radial event (an explosion) of this reach around <paramref name="Point"/>.</param>
public readonly record struct HitEvent(Entity Target, string? Bone, Vec3 Impulse, Vec3 Point = default, float Damage = 0f, float Radius = 0f);

public readonly partial struct Entity
{
    private static long PhysAnimHash(string? name) => string.IsNullOrEmpty(name) ? 0 : Assets.ObjectIdOf(name);

    /// <summary>Reacts to a hit: the hit bone and its neighbours along the skeleton blend into the physics along
    /// their envelopes (attack, hold, decay from the profile), the hit part takes the impulse, and the ragdoll that
    /// was started for it removes itself when the reaction has run out. A ragdoll already running (started with
    /// <see cref="StartRagdoll"/>, or dying) only takes the impulse. False when the entity has no posed skeleton, the
    /// bone is unknown, or the host has no ragdoll support.</summary>
    public bool ApplyHit(HitEvent hit) =>
        FwPhysAnim.aver_fw_physanim_hit(hit.Target.Handle, PhysAnimHash(hit.Bone), hit.Impulse.X, hit.Impulse.Y, hit.Impulse.Z,
                                        hit.Point.X, hit.Point.Y, hit.Point.Z, hit.Damage, hit.Radius) != 0;

    /// <summary>As <see cref="ApplyHit"/> for a point hit on this entity.</summary>
    public bool ApplyHitReaction(string? bone, Vec3 impulse, float damage = 0f) =>
        ApplyHit(new HitEvent(this, bone, impulse, default, damage));

    /// <summary>Sets the physical animation profile (an .ocphysanim asset) that shapes this entity's reactions, drive
    /// and death; null or empty goes back to the defaults. Takes effect for the next reaction.</summary>
    public bool SetPhysicalAnimationProfile(string? profileAsset) =>
        FwPhysAnim.aver_fw_physanim_set_profile(Handle, PhysAnimHash(profileAsset)) != 0;

    /// <summary>Ramps the whole-body physical animation weight to <paramref name="weight"/> (0 the animation, 1 the
    /// powered simulation) over <paramref name="seconds"/>, starting a powered ragdoll when there is none.</summary>
    public bool BlendPhysicalAnimation(float weight, float seconds = 0.2f) =>
        FwPhysAnim.aver_fw_physanim_blend(Handle, weight, seconds) != 0;

    /// <summary>Simulates <paramref name="bone"/> and everything below it; the rest of the body follows the animation
    /// exactly. Null or empty simulates the whole body again. Needs a ragdoll (<see cref="StartRagdoll"/> in
    /// <see cref="RagdollMode.Powered"/> or Limp).</summary>
    public bool SimulateBoneSubtree(string? bone) => FwPhysAnim.aver_fw_physanim_simulate_subtree(Handle, PhysAnimHash(bone)) != 0;

    /// <summary>Powered motor strength of one bone's joint, 0..1; negative goes back to the ragdoll-wide strength.</summary>
    public bool SetBoneStrength(string bone, float strength) => SetBonePart(bone, 0, strength);
    /// <summary>Motor damping ratio of one bone's joint (default 1); negative goes back to the default.</summary>
    public bool SetBoneDamping(string bone, float damping) => SetBonePart(bone, 1, damping);
    /// <summary>Multiplier on the torque ceiling of one bone's motor (default 1).</summary>
    public bool SetBoneTorqueScale(string bone, float scale) => SetBonePart(bone, 2, scale);
    /// <summary>How much of the simulated pose shows for one bone, 0..1 (default 1), whatever the ragdoll-wide weight is.</summary>
    public bool SetBoneBlend(string bone, float blend) => SetBonePart(bone, 3, blend);
    /// <summary>A pinned bone follows the animation exactly while the rest of the body simulates.</summary>
    public bool PinBone(string bone, bool pinned = true) => SetBonePart(bone, 4, pinned ? 1f : 0f);

    private bool SetBonePart(string bone, int kind, float value) =>
        !string.IsNullOrEmpty(bone) && FwPhysAnim.aver_fw_physanim_set_part(Handle, Assets.ObjectIdOf(bone), kind, value) != 0;

    /// <summary>Starts dying: the whole body goes powered and the motors let go over the profile's death time, then
    /// the body is limp (<see cref="LifeState.Dead"/>).</summary>
    public bool DieAsRagdoll() => FwPhysAnim.aver_fw_physanim_die(Handle) != 0;

    /// <summary>Gets a <see cref="LifeState.Dead"/> body up: picks the profile's montage by how it lies (supine,
    /// prone, side), plays it, and blends the simulation out under it. The entity's transform is not moved to where
    /// the body lies; a game that wants that moves the entity first.</summary>
    public bool GetUpFromRagdoll() => FwPhysAnim.aver_fw_physanim_get_up(Handle) != 0;

    /// <summary>The life state, how the body lies and how many bones are still reacting to hits. False (and Alive,
    /// Upright, 0) when the entity has no ragdoll.</summary>
    public bool TryGetPhysicalAnimationState(out LifeState life, out BodyFacing facing, out int reactingBones)
    {
        bool has = FwPhysAnim.aver_fw_physanim_state(Handle, out int l, out int f, out int r) != 0;
        life = (LifeState)l;
        facing = (BodyFacing)f;
        reactingBones = r;
        return has;
    }
}

/// <summary>The graph-node surface for the physical animation nodes: the same Entity calls with the int-entity /
/// primitive-only signatures GraphCompiler's emitted IL needs. An empty string means "not given".</summary>
internal static class PhysAnimGraphInterop
{
    internal static bool ApplyHitReactionForGraph(int entity, string bone, float x, float y, float z, float px, float py, float pz,
                                                  float damage, float radius) =>
        new Entity(entity).ApplyHit(new HitEvent(new Entity(entity), bone, new Vec3(x, y, z), new Vec3(px, py, pz), damage, radius));

    internal static bool SetPhysicalAnimationProfileForGraph(int entity, string profile) =>
        new Entity(entity).SetPhysicalAnimationProfile(profile);

    internal static bool BlendPhysicalAnimationForGraph(int entity, float weight, float seconds) =>
        new Entity(entity).BlendPhysicalAnimation(weight, seconds);

    internal static bool SimulateBoneSubtreeForGraph(int entity, string bone) =>
        new Entity(entity).SimulateBoneSubtree(bone);

    internal static bool SetBoneStrengthForGraph(int entity, string bone, float strength) =>
        new Entity(entity).SetBoneStrength(bone, strength);

    internal static bool SetBoneBlendForGraph(int entity, string bone, float blend) =>
        new Entity(entity).SetBoneBlend(bone, blend);

    internal static bool PinBoneForGraph(int entity, string bone, bool pinned) =>
        new Entity(entity).PinBone(bone, pinned);

    internal static bool DieAsRagdollForGraph(int entity) => new Entity(entity).DieAsRagdoll();

    internal static bool GetUpFromRagdollForGraph(int entity) => new Entity(entity).GetUpFromRagdoll();

    internal static bool GetPhysicalAnimationStateForGraph(int entity, out int life, out int facing, out int reacting)
    {
        bool has = new Entity(entity).TryGetPhysicalAnimationState(out LifeState l, out BodyFacing f, out reacting);
        life = (int)l;
        facing = (int)f;
        return has;
    }
}
