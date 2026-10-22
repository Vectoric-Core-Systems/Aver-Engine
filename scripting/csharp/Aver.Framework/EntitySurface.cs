// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The physical-surface and body-scale members of Entity: they act on the entity's CRigidBody body (the handle the host stores in
// CRigidBody.body when it builds the body), so an entity with no rigid body answers false / 0. Centimetres, absolute scale.

using Aver.Scene;
using AP = Aver.Physics;

namespace Aver.Framework;

public readonly partial struct Entity
{
    private const string RigidBodyType = "CRigidBody";

    // The physics handle of this entity's rigid body, or 0: no component, a body not built yet, a dead entity, or no native library.
    internal int RigidBodyHandle
    {
        get
        {
            try { return HasComponent(RigidBodyType) ? GetInt("CRigidBody.body") : 0; }
            catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return 0; }
        }
    }

    /// <summary>The surface label of this entity's rigid body (0 = none): identity for queries such as footsteps, not contact
    /// friction. 0 when the entity has no built rigid body.</summary>
    public int PhysicalSurface
    {
        get
        {
            int body = RigidBodyHandle;
            if (body == 0) return 0;
            try { return new AP.Body(body).Surface; }
            catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return 0; }
        }
    }

    /// <summary>Labels this entity's rigid body with a surface id (<see cref="Surface.IdOf"/>; 0 clears it).
    /// <paramref name="copyPhysics"/> also copies the surface's registered friction and restitution into the body once, the
    /// common "make this crate rubber" call. False when the entity has no built rigid body.</summary>
    public bool SetPhysicalSurface(int surfaceId, bool copyPhysics = false)
    {
        int body = RigidBodyHandle;
        if (body == 0) return false;
        try { return new AP.Body(body).SetSurface(surfaceId, copyPhysics); }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }

    /// <summary>Sets the scale of this entity's rigid body collision shape, ABSOLUTE (relative to the unscaled shape, not to the
    /// current scale). Mass follows volume unless <paramref name="keepMass"/>. Physics only: pair it with the transform scale
    /// to scale what is drawn. False when the entity has no built rigid body, the scale has a zero component, or the body's
    /// owner (a vehicle, a ragdoll) rebuilds its shape itself.</summary>
    public bool SetBodyScale(Vec3 scale, bool keepMass = false)
    {
        int body = RigidBodyHandle;
        if (body == 0) return false;
        try { return new AP.Body(body).SetScale(new AP.Float3(scale.X, scale.Y, scale.Z), keepMass); }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }
}
