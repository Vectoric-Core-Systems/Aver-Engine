// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Physics nodes: bodies, joints, casts and queries, ragdolls and character movement.

using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using Aver.Framework;
using Aver.Scene;

namespace Aver.Graph;

public partial class GraphCompiler
{
    /// Jump(entity) -> jumped: a side effect, refused when pulled as data (IsExecCapableSideEffectType).
    /// The bool is stored when the node declares `jumped`, popped otherwise, to balance the stack.
    private void EmitJump(Node node)
    {
        if (_il == null) return;
        // Must be EmitPullInput, not LoadPin: LoadPin reads `_pinLocals`, which the exec compiler
        // never populates for data nodes (see EmitPullInput's own comment below). Every other
        // exec-path emitter (CharacterMove, SetParent, Spawn, FireEvent, SetVar...) pulls its
        // inputs; this one read an unset local and pushed zero, so Jump always got entity 0, found
        // no actor, warned, and returned false -- never once fired. Masked because no shipped
        // FirstPerson template presses jump and --play-test only synthesises Space from frame 100
        // (SandboxApp.cpp); read the warning as "Jump is broken", not "the first frame is odd".
        EmitPullInput(node, "entity");
        _il.Emit(OpCodes.Call, JumpMethod);
        if (_pinLocals.TryGetValue((node.Id, "jumped"), out var local)) _il.Emit(OpCodes.Stloc, local);
        else                                                           _il.Emit(OpCodes.Pop);
    }

    /// The three transform writers: same shape as EmitExecApiCall -- push, call, keep or drop the bool.
    /// The physics writers: push the body (none for SetGravity), push the vector, call, keep or drop.
    private void EmitExecPhysicsWrite(Node node)
    {
        if (_il == null) return;
        string t = node.Type.ToLowerInvariant();
        if (t != "setgravity") EmitPullInput(node, "body");
        switch (t)
        {
            case "setbodyposition":
                EmitPullInput(node, "x"); EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, SetBodyPositionMethod); break;
            case "setbodyvelocity":
                EmitPullInput(node, "x"); EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, SetBodyVelocityMethod); break;
            case "addbodyvelocity":
                EmitPullInput(node, "x"); EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, AddBodyVelocityMethod); break;
            // Forces/impulses: same "pull body, pull x/y/z, call" shape as addbodyvelocity above.
            case "addforce":
                EmitPullInput(node, "x"); EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, AddForceMethod); break;
            case "addimpulse":
                EmitPullInput(node, "x"); EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, AddImpulseMethod); break;
            case "addtorque":
                EmitPullInput(node, "x"); EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, AddTorqueMethod); break;
            case "addangularimpulse":
                EmitPullInput(node, "x"); EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, AddAngularImpulseMethod); break;
            case "setbodyangularvelocity":
                EmitPullInput(node, "x"); EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, SetBodyAngularVelocityMethod); break;
            case "setbodyentity":
                EmitPullInput(node, "entity");
                _il.Emit(OpCodes.Call, SetBodyEntityMethod); break;
            // Material, mass, motion type, layer -- one scalar pin each after body.
            case "setbodyfriction":
                EmitPullInput(node, "friction");
                _il.Emit(OpCodes.Call, SetBodyFrictionMethod); break;
            case "setbodyrestitution":
                EmitPullInput(node, "restitution");
                _il.Emit(OpCodes.Call, SetBodyRestitutionMethod); break;
            case "setbodygravityfactor":
                EmitPullInput(node, "factor");
                _il.Emit(OpCodes.Call, SetBodyGravityFactorMethod); break;
            case "setbodymass":
                EmitPullInput(node, "mass");
                _il.Emit(OpCodes.Call, SetBodyMassMethod); break;
            case "setbodymotiontype":
                EmitPullInput(node, "motionType");
                _il.Emit(OpCodes.Call, SetBodyMotionTypeMethod); break;
            case "activatebody":
                _il.Emit(OpCodes.Call, ActivateBodyMethod); break;
            case "setbodylayer":
                EmitPullInput(node, "layer");
                _il.Emit(OpCodes.Call, SetBodyLayerMethod); break;
            case "setgravity":
                EmitPullInput(node, "x"); EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, SetGravityMethod); break;
            // Wave 1b: six lock flags (translation XYZ, then rotation XYZ) and a CCD switch.
            case "setlockedaxes":
                foreach (string a in new[] { "lockX", "lockY", "lockZ", "lockRotX", "lockRotY", "lockRotZ" })
                    EmitPullInput(node, a);
                _il.Emit(OpCodes.Call, SetLockedAxesMethod); break;
            case "setccd":
                EmitPullInput(node, "enabled");
                _il.Emit(OpCodes.Call, SetCcdMethod); break;
            default:
                _il.Emit(OpCodes.Call, DestroyBodyMethod); break;
        }
        // Into an exec local, not _pinLocals (see EmitExecSideEffect): this emitter runs only from
        // exec dispatch, where _pinLocals is empty, so a failed write (stale handle) used to look
        // identical to a working one.
        var successPin = node.Pins.FirstOrDefault(p => p.IsOutput && p.Name == "success" && p.Type == PinType.Bool);
        if (successPin != null) _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, "success", typeof(bool)));
        else                    _il.Emit(OpCodes.Pop);
    }

    /// The creators: each returns a body handle into an exec-local, not a pin local -- read AFTER
    /// this node runs by whatever the exec chain reaches next (one creation, many readers).
    private void EmitExecPhysicsCreate(Node node)
    {
        if (_il == null) return;
        EmitPullInput(node, "cx"); EmitPullInput(node, "cy"); EmitPullInput(node, "cz");
        switch (node.Type.ToLowerInvariant())
        {
            case "addstaticbox":
                EmitPullInput(node, "hx"); EmitPullInput(node, "hy"); EmitPullInput(node, "hz");
                _il.Emit(OpCodes.Call, AddStaticBoxMethod); break;
            case "adddynamicbox":
                EmitPullInput(node, "hx"); EmitPullInput(node, "hy"); EmitPullInput(node, "hz");
                EmitPullInput(node, "mass");
                _il.Emit(OpCodes.Call, AddDynamicBoxMethod); break;
            case "adddynamicsphere":
                EmitPullInput(node, "radius"); EmitPullInput(node, "mass");
                _il.Emit(OpCodes.Call, AddDynamicSphereMethod); break;
            case "addsensorbox":
                EmitPullInput(node, "hx"); EmitPullInput(node, "hy"); EmitPullInput(node, "hz");
                _il.Emit(OpCodes.Call, AddSensorBoxMethod); break;
            default:
                EmitPullInput(node, "radius");
                _il.Emit(OpCodes.Call, AddSensorSphereMethod); break;
        }
        _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, "body", typeof(int)));
    }

    /// The joint creators: bodyA/bodyB first (bodyB == 0 means "the world", not "dead" -- see
    /// GraphNodeDefs.hpp's JOINTS banner), then shape-specific floats, then a call whose return goes
    /// into exec-local "joint" instead of "body" -- same EmitExecPhysicsCreate reasoning otherwise.
    private void EmitExecJointCreate(Node node)
    {
        if (_il == null) return;
        EmitPullInput(node, "bodyA"); EmitPullInput(node, "bodyB");
        switch (node.Type.ToLowerInvariant())
        {
            case "jointfixed":
                EmitPullInput(node, "px"); EmitPullInput(node, "py"); EmitPullInput(node, "pz");
                EmitPullInput(node, "axX"); EmitPullInput(node, "axY"); EmitPullInput(node, "axZ");
                EmitPullInput(node, "ayX"); EmitPullInput(node, "ayY"); EmitPullInput(node, "ayZ");
                _il.Emit(OpCodes.Call, JointFixedMethod); break;
            case "jointpoint":
                EmitPullInput(node, "px"); EmitPullInput(node, "py"); EmitPullInput(node, "pz");
                _il.Emit(OpCodes.Call, JointPointMethod); break;
            case "jointdistance":
                EmitPullInput(node, "paX"); EmitPullInput(node, "paY"); EmitPullInput(node, "paZ");
                EmitPullInput(node, "pbX"); EmitPullInput(node, "pbY"); EmitPullInput(node, "pbZ");
                EmitPullInput(node, "minDist"); EmitPullInput(node, "maxDist");
                _il.Emit(OpCodes.Call, JointDistanceMethod); break;
            case "jointslider":
                EmitPullInput(node, "px"); EmitPullInput(node, "py"); EmitPullInput(node, "pz");
                EmitPullInput(node, "sx"); EmitPullInput(node, "sy"); EmitPullInput(node, "sz");
                EmitPullInput(node, "nx"); EmitPullInput(node, "ny"); EmitPullInput(node, "nz");
                EmitPullInput(node, "minCm"); EmitPullInput(node, "maxCm");
                _il.Emit(OpCodes.Call, JointSliderMethod); break;
            default: // jointhinge
                EmitPullInput(node, "px"); EmitPullInput(node, "py"); EmitPullInput(node, "pz");
                EmitPullInput(node, "hx"); EmitPullInput(node, "hy"); EmitPullInput(node, "hz");
                EmitPullInput(node, "nx"); EmitPullInput(node, "ny"); EmitPullInput(node, "nz");
                EmitPullInput(node, "minAngleRad"); EmitPullInput(node, "maxAngleRad");
                _il.Emit(OpCodes.Call, JointHingeMethod); break;
        }
        _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, "joint", typeof(int)));
    }

    /// A joint handle (never a body) pulled first, then op-specific args, then the same "success
    /// into an exec local" tail as EmitExecPhysicsWrite/EmitExecTransformWrite/EmitExecApiCall (see
    /// EmitExecPhysicsWrite for why exec local, not pin local).
    private void EmitExecJointOp(Node node)
    {
        if (_il == null) return;
        EmitPullInput(node, "joint");
        switch (node.Type.ToLowerInvariant())
        {
            case "jointsetmotor":
                EmitPullInput(node, "state"); EmitPullInput(node, "target");
                _il.Emit(OpCodes.Call, JointSetMotorMethod); break;
            case "jointsetenabled":
                EmitPullInput(node, "enabled");
                _il.Emit(OpCodes.Call, JointSetEnabledMethod); break;
            case "setjointbreakable":
                EmitPullInput(node, "maxForceN"); EmitPullInput(node, "maxTorqueNm");
                _il.Emit(OpCodes.Call, JointSetBreakableMethod); break;
            default: // jointremove
                _il.Emit(OpCodes.Call, JointRemoveMethod); break;
        }
        var successPin = node.Pins.FirstOrDefault(p => p.IsOutput && p.Name == "success" && p.Type == PinType.Bool);
        if (successPin != null) _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, "success", typeof(bool)));
        else                    _il.Emit(OpCodes.Pop);
    }

    /// SphereCast, shaped exactly like EmitExecRaycast: one call, N results into N exec-locals, so
    /// a sweep costs the same whether one output pin is read or six.
    private void EmitExecSphereCast(Node node)
    {
        if (_il == null) return;
        foreach (string a in new[] { "originX", "originY", "originZ", "dirX", "dirY", "dirZ", "maxDist", "radius" })
            EmitPullInput(node, a);
        _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "hit", typeof(bool)));
        _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "body", typeof(int)));
        _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "pointX", typeof(float)));
        _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "pointY", typeof(float)));
        _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "pointZ", typeof(float)));
        _il.Emit(OpCodes.Call, SphereCastMethod);
    }

    /// The wave-1b queries, EmitExecSphereCast's shape: pull the inputs, hand a local address per
    /// output, one call. The interop method's parameters are exactly the input pins then the output
    /// pins in the order below, so the pin lists here ARE the signature.
    private void EmitExecPhysicsQuery(Node node)
    {
        if (_il == null) return;
        string[] filters = { "layerMask", "ignoreBody", "includeSensors" };
        string[] rotation = { "yaw", "pitch", "roll" };
        string[] castOut = { "hit", "body", "distance", "pointX", "pointY", "pointZ", "normalX", "normalY", "normalZ" };
        string[] ins, outs;
        MethodInfo method;
        switch (node.Type.ToLowerInvariant())
        {
            case "boxcast":
                ins = new[] { "cx", "cy", "cz", "hx", "hy", "hz" }.Concat(rotation)
                    .Concat(new[] { "dirX", "dirY", "dirZ", "maxDist" }).Concat(filters).ToArray();
                outs = castOut; method = BoxCastMethod; break;
            case "capsulecast":
                ins = new[] { "cx", "cy", "cz", "radius", "height" }.Concat(rotation)
                    .Concat(new[] { "dirX", "dirY", "dirZ", "maxDist" }).Concat(filters).ToArray();
                outs = castOut; method = CapsuleCastMethod; break;
            case "overlapbox":
                ins = new[] { "cx", "cy", "cz", "hx", "hy", "hz" }.Concat(rotation).Concat(filters).ToArray();
                outs = new[] { "count", "body" }; method = OverlapBoxMethod; break;
            case "raycastall":
                ins = new[] { "originX", "originY", "originZ", "dirX", "dirY", "dirZ", "maxDist" }.Concat(filters).ToArray();
                outs = new[] { "count", "body", "distance", "pointX", "pointY", "pointZ" }; method = RaycastAllMethod; break;
            default: // bodiesatpoint
                ins = new[] { "x", "y", "z" }.Concat(filters).ToArray();
                outs = new[] { "count", "body" }; method = BodiesAtPointMethod; break;
        }
        foreach (string a in ins) EmitPullInput(node, a);
        foreach (string o in outs)
        {
            var pin = PinOf(node, o) ?? throw new InvalidOperationException(
                $"{node.Type} node '{node.Id}' has no output pin '{o}' (a node with PIN records needs every pin)");
            _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, o, PinTypeToCLRType(pin.Type)));
        }
        _il.Emit(OpCodes.Call, method);
    }

    /// The four ragdoll writes: entity first, then op-specific args, then the usual "success into an
    /// exec local" tail. RagdollImpulse's bone is a NODE-line bone= attribute, empty (= root part) when absent.
    private void EmitExecRagdoll(Node node)
    {
        if (_il == null) return;
        EmitPullInput(node, "entity");
        switch (node.Type.ToLowerInvariant())
        {
            case "ragdollstart":
                EmitPullInput(node, "mode"); EmitPullInput(node, "blend");
                _il.Emit(OpCodes.Call, RagdollStartMethod); break;
            case "ragdollstop":
                EmitPullInput(node, "blend");
                _il.Emit(OpCodes.Call, RagdollStopMethod); break;
            case "setragdollstrength":
                EmitPullInput(node, "strength");
                _il.Emit(OpCodes.Call, SetRagdollStrengthMethod); break;
            default: // ragdollimpulse
                _il.Emit(OpCodes.Ldstr, AttributeFor(node, "bone?"));
                EmitPullInput(node, "x"); EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, RagdollImpulseMethod); break;
        }
        var successPin = node.Pins.FirstOrDefault(p => p.IsOutput && p.Name == "success" && p.Type == PinType.Bool);
        if (successPin != null) _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, "success", typeof(bool)));
        else                    _il.Emit(OpCodes.Pop);
    }

    /// CharacterCanStand's PULL emission, EmitPullSynapsePerception's shape with one out-parameter.
    private void EmitPullCharacterCanStand(Node node, string pinName)
    {
        if (_il == null) return;
        EmitPullInput(node, "entity"); EmitPullInput(node, "radius"); EmitPullInput(node, "halfHeight");
        var canStandL = _il.DeclareLocal(typeof(bool));
        _il.Emit(OpCodes.Ldloca, canStandL);
        _il.Emit(OpCodes.Call, CharacterCanStandMethod);
        if (pinName == "success") return;
        _il.Emit(OpCodes.Pop);
        _il.Emit(OpCodes.Ldloc, canStandL);
    }

    /// CharacterMove's own version of IsExecCapableSpawnType -- refused by the PULL compiler's
    /// topological pass entirely (EmitNode's "charactermove" case), the same stricter-than-SetField
    /// treatment as Spawn: CharacterMoveForGraph -> AverCharacter.Drive mutates _yaw/_pitch and
    /// capsule velocity on every call, a materially worse hazard than the idempotent
    /// SetParent/SetViewEntity/SetName/SetMesh/SetMaterial family. Own predicate: a different native
    /// surface (AverCharacter/Actors) with its own signature (six scalars in, bool out).
    private static bool IsExecCapableCharacterMoveType(string type) =>
        type.Equals("charactermove", StringComparison.OrdinalIgnoreCase);

    /// Jump gets its OWN predicate rather than joining CharacterMove's -- a different call, emitter,
    /// and failure mode (declined in mid-air, ordinary); a predicate matching two unrelated types is
    /// one whose name stops describing what it matches.
    private static bool IsExecCapableJumpType(string type) =>
        type.Equals("jump", StringComparison.OrdinalIgnoreCase);

    /// The four framework write calls, grouped into one predicate (unlike Jump/CharacterMove): all
    /// share EmitExecApiCall exactly, as the three transform writers share one emitter. Physics
    /// writers/creators/sweep get three predicates for three emitters (a write returns bool, a
    /// creator returns a body handle, the sweep fills exec-locals like Raycast) -- widened past the
    /// original six: each new physics write (forces/impulses/mass/motion-type/layer/etc) joins this
    /// switch rather than getting a new one.
    private static bool IsExecCapablePhysicsWriteType(string type)
    {
        string t = type.ToLowerInvariant();
        return t == "setbodyposition" || t == "setbodyvelocity" || t == "addbodyvelocity" || t == "destroybody" ||
               t == "setbodyentity" || t == "setgravity" ||
               t == "addforce" || t == "addimpulse" || t == "addtorque" || t == "addangularimpulse" ||
               t == "setbodyangularvelocity" || t == "setbodyfriction" || t == "setbodyrestitution" ||
               t == "setbodygravityfactor" || t == "setbodymass" || t == "setbodymotiontype" ||
               t == "activatebody" || t == "setbodylayer" ||
               t == "setlockedaxes" || t == "setccd";
    }

    private static bool IsExecCapablePhysicsCreateType(string type)
    {
        string t = type.ToLowerInvariant();
        return t == "addstaticbox" || t == "adddynamicbox" || t == "adddynamicsphere" || t == "addsensorbox" || t == "addsensorsphere";
    }

    private static bool IsExecCapableSphereCastType(string type) =>
        type.Equals("spherecast", StringComparison.OrdinalIgnoreCase);

    /// The joint creators -- EmitExecJointCreate's dispatch list. A FOURTH physics-family predicate,
    /// not folded into IsExecCapablePhysicsCreateType: return goes into exec-local "joint" not "body",
    /// and inputs start with bodyA/bodyB, not a single centre point.
    private static bool IsExecCapableJointCreateType(string type)
    {
        string t = type.ToLowerInvariant();
        return t == "jointhinge" || t == "jointpoint" || t == "jointdistance" || t == "jointslider" || t == "jointfixed";
    }

    /// The joint operations -- EmitExecJointOp's dispatch list. Apart from IsExecCapablePhysicsWriteType
    /// for the same reason as the creators: these key off a JOINT handle, never a body.
    private static bool IsExecCapableJointOpType(string type)
    {
        string t = type.ToLowerInvariant();
        return t == "jointsetmotor" || t == "jointsetenabled" || t == "jointremove" || t == "setjointbreakable";
    }

    /// The wave-1b shape queries -- EmitExecPhysicsQuery's dispatch list. Exec-cached like SphereCast:
    /// one native call per visit, N results into N exec-locals.
    private static bool IsExecCapablePhysicsQueryType(string type)
    {
        string t = type.ToLowerInvariant();
        return t == "boxcast" || t == "capsulecast" || t == "overlapbox" || t == "raycastall" || t == "bodiesatpoint";
    }

    /// The four ragdoll writes -- EmitExecRagdoll's dispatch list. Entity-keyed, so apart from the body writers.
    private static bool IsExecCapableRagdollType(string type)
    {
        string t = type.ToLowerInvariant();
        return t == "ragdollstart" || t == "ragdollstop" || t == "setragdollstrength" || t == "ragdollimpulse";
    }

    /// Runs a CharacterMove node's native call once, when reached -- mirrors EmitExecSpawn's shape,
    /// but no attribute check: unlike Spawn's class=, all six CharacterMoveForGraph parameters are
    /// ordinary pins (see OcGraphParser.AddDefaultPins's "CharacterMove" comment).
    private void EmitExecCharacterMove(Node node)
    {
        if (_il == null) return;

        EmitPullInput(node, "entity");
        EmitPullInput(node, "dt");
        EmitPullInput(node, "forward");
        EmitPullInput(node, "right");
        EmitPullInput(node, "yawDelta");
        EmitPullInput(node, "pitchDelta");
        _il.Emit(OpCodes.Call, CharacterMoveMethod);

        if (node.Pins.Any(p => p.IsOutput && p.Name == "success"))
        {
            var successLocal = GetOrCreateExecLocal(node.Id, "success", typeof(bool));
            _il.Emit(OpCodes.Stloc, successLocal);
        }
        else
        {
            _il.Emit(OpCodes.Pop);
        }
    }
    // CharacterMove: the last Blueprint-parity node, reflected like every other GraphInterop wrapper
    // above -- see GraphInterop.CharacterMoveForGraph for why this call, not a cast, is the seam.
    private static readonly MethodInfo CharacterMoveMethod =
        typeof(GraphInterop).GetMethod("CharacterMoveForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.CharacterMoveForGraph was not found by reflection");
    // GetViewEntity: the camera node a character looks through -- what a viewmodel parents to.
    // The physics surfaces.
    private static readonly MethodInfo BodyPositionMethod =
        typeof(GraphInterop).GetMethod("BodyPositionForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.BodyPositionForGraph was not found by reflection");
    private static readonly MethodInfo BodyVelocityMethod =
        typeof(GraphInterop).GetMethod("BodyVelocityForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.BodyVelocityForGraph was not found by reflection");
    private static readonly MethodInfo BodyValidMethod =
        typeof(GraphInterop).GetMethod("BodyValidForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.BodyValidForGraph was not found by reflection");
    private static readonly MethodInfo BodyCountMethod =
        typeof(GraphInterop).GetMethod("BodyCountForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.BodyCountForGraph was not found by reflection");
    private static readonly MethodInfo PhysicsReadyMethod =
        typeof(GraphInterop).GetMethod("PhysicsReadyForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.PhysicsReadyForGraph was not found by reflection");
    private static readonly MethodInfo PhysicsFixedStepMethod =
        typeof(GraphInterop).GetMethod("PhysicsFixedStepForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.PhysicsFixedStepForGraph was not found by reflection");
    private static readonly MethodInfo SetBodyPositionMethod =
        typeof(GraphInterop).GetMethod("SetBodyPositionForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetBodyPositionForGraph was not found by reflection");
    private static readonly MethodInfo SetBodyVelocityMethod =
        typeof(GraphInterop).GetMethod("SetBodyVelocityForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetBodyVelocityForGraph was not found by reflection");
    private static readonly MethodInfo AddBodyVelocityMethod =
        typeof(GraphInterop).GetMethod("AddBodyVelocityForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.AddBodyVelocityForGraph was not found by reflection");
    private static readonly MethodInfo DestroyBodyMethod =
        typeof(GraphInterop).GetMethod("DestroyBodyForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.DestroyBodyForGraph was not found by reflection");
    private static readonly MethodInfo SetBodyEntityMethod =
        typeof(GraphInterop).GetMethod("SetBodyEntityForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetBodyEntityForGraph was not found by reflection");
    private static readonly MethodInfo SetGravityMethod =
        typeof(GraphInterop).GetMethod("SetGravityForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetGravityForGraph was not found by reflection");
    private static readonly MethodInfo AddStaticBoxMethod =
        typeof(GraphInterop).GetMethod("AddStaticBoxForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.AddStaticBoxForGraph was not found by reflection");
    private static readonly MethodInfo AddDynamicBoxMethod =
        typeof(GraphInterop).GetMethod("AddDynamicBoxForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.AddDynamicBoxForGraph was not found by reflection");
    private static readonly MethodInfo AddDynamicSphereMethod =
        typeof(GraphInterop).GetMethod("AddDynamicSphereForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.AddDynamicSphereForGraph was not found by reflection");
    private static readonly MethodInfo AddSensorBoxMethod =
        typeof(GraphInterop).GetMethod("AddSensorBoxForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.AddSensorBoxForGraph was not found by reflection");
    private static readonly MethodInfo AddSensorSphereMethod =
        typeof(GraphInterop).GetMethod("AddSensorSphereForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.AddSensorSphereForGraph was not found by reflection");
    private static readonly MethodInfo RaycastAnyMethod =
        typeof(GraphInterop).GetMethod("RaycastAnyForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.RaycastAnyForGraph was not found by reflection");
    private static readonly MethodInfo SphereCastMethod =
        typeof(GraphInterop).GetMethod("SphereCastForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SphereCastForGraph was not found by reflection");
    // Forces, impulses, spin, material, mass, motion type, layers -- all body-keyed, resolved by name
    // up front like every MethodInfo above: a lookup naming a nonexistent method throws here, at
    // static init, breaking every graph in the process, not just the one using the new node.
    private static readonly MethodInfo AddForceMethod =
        typeof(GraphInterop).GetMethod("AddForceForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.AddForceForGraph was not found by reflection");
    private static readonly MethodInfo AddImpulseMethod =
        typeof(GraphInterop).GetMethod("AddImpulseForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.AddImpulseForGraph was not found by reflection");
    private static readonly MethodInfo AddTorqueMethod =
        typeof(GraphInterop).GetMethod("AddTorqueForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.AddTorqueForGraph was not found by reflection");
    private static readonly MethodInfo AddAngularImpulseMethod =
        typeof(GraphInterop).GetMethod("AddAngularImpulseForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.AddAngularImpulseForGraph was not found by reflection");
    private static readonly MethodInfo BodyAngularVelocityMethod =
        typeof(GraphInterop).GetMethod("BodyAngularVelocityForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.BodyAngularVelocityForGraph was not found by reflection");
    private static readonly MethodInfo SetBodyAngularVelocityMethod =
        typeof(GraphInterop).GetMethod("SetBodyAngularVelocityForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetBodyAngularVelocityForGraph was not found by reflection");
    private static readonly MethodInfo SetBodyFrictionMethod =
        typeof(GraphInterop).GetMethod("SetBodyFrictionForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetBodyFrictionForGraph was not found by reflection");
    private static readonly MethodInfo SetBodyRestitutionMethod =
        typeof(GraphInterop).GetMethod("SetBodyRestitutionForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetBodyRestitutionForGraph was not found by reflection");
    private static readonly MethodInfo SetBodyGravityFactorMethod =
        typeof(GraphInterop).GetMethod("SetBodyGravityFactorForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetBodyGravityFactorForGraph was not found by reflection");
    private static readonly MethodInfo SetBodyMassMethod =
        typeof(GraphInterop).GetMethod("SetBodyMassForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetBodyMassForGraph was not found by reflection");
    private static readonly MethodInfo BodyMassMethod =
        typeof(GraphInterop).GetMethod("BodyMassForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.BodyMassForGraph was not found by reflection");
    private static readonly MethodInfo SetBodyMotionTypeMethod =
        typeof(GraphInterop).GetMethod("SetBodyMotionTypeForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetBodyMotionTypeForGraph was not found by reflection");
    private static readonly MethodInfo BodyMotionTypeMethod =
        typeof(GraphInterop).GetMethod("BodyMotionTypeForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.BodyMotionTypeForGraph was not found by reflection");
    private static readonly MethodInfo ActivateBodyMethod =
        typeof(GraphInterop).GetMethod("ActivateBodyForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.ActivateBodyForGraph was not found by reflection");
    private static readonly MethodInfo BodyActiveMethod =
        typeof(GraphInterop).GetMethod("BodyActiveForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.BodyActiveForGraph was not found by reflection");
    private static readonly MethodInfo SetBodyLayerMethod =
        typeof(GraphInterop).GetMethod("SetBodyLayerForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetBodyLayerForGraph was not found by reflection");
    private static readonly MethodInfo BodyLayerMethod =
        typeof(GraphInterop).GetMethod("BodyLayerForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.BodyLayerForGraph was not found by reflection");
    // Joints.
    private static readonly MethodInfo JointFixedMethod =
        typeof(GraphInterop).GetMethod("JointFixedForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.JointFixedForGraph was not found by reflection");
    private static readonly MethodInfo JointPointMethod =
        typeof(GraphInterop).GetMethod("JointPointForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.JointPointForGraph was not found by reflection");
    private static readonly MethodInfo JointDistanceMethod =
        typeof(GraphInterop).GetMethod("JointDistanceForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.JointDistanceForGraph was not found by reflection");
    private static readonly MethodInfo JointHingeMethod =
        typeof(GraphInterop).GetMethod("JointHingeForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.JointHingeForGraph was not found by reflection");
    private static readonly MethodInfo JointSliderMethod =
        typeof(GraphInterop).GetMethod("JointSliderForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.JointSliderForGraph was not found by reflection");
    private static readonly MethodInfo JointSetMotorMethod =
        typeof(GraphInterop).GetMethod("JointSetMotorForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.JointSetMotorForGraph was not found by reflection");
    // Physics wave 1b: breakable joints, body locks/CCD, shape queries, can-stand, ragdolls.
    private static readonly MethodInfo JointSetBreakableMethod =
        typeof(GraphInterop).GetMethod("JointSetBreakableForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.JointSetBreakableForGraph was not found by reflection");
    private static readonly MethodInfo SetLockedAxesMethod =
        typeof(GraphInterop).GetMethod("SetLockedAxesForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetLockedAxesForGraph was not found by reflection");
    private static readonly MethodInfo SetCcdMethod =
        typeof(GraphInterop).GetMethod("SetCcdForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetCcdForGraph was not found by reflection");
    private static readonly MethodInfo BoxCastMethod =
        typeof(GraphInterop).GetMethod("BoxCastForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.BoxCastForGraph was not found by reflection");
    private static readonly MethodInfo CapsuleCastMethod =
        typeof(GraphInterop).GetMethod("CapsuleCastForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.CapsuleCastForGraph was not found by reflection");
    private static readonly MethodInfo OverlapBoxMethod =
        typeof(GraphInterop).GetMethod("OverlapBoxForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.OverlapBoxForGraph was not found by reflection");
    private static readonly MethodInfo RaycastAllMethod =
        typeof(GraphInterop).GetMethod("RaycastAllForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.RaycastAllForGraph was not found by reflection");
    private static readonly MethodInfo BodiesAtPointMethod =
        typeof(GraphInterop).GetMethod("BodiesAtPointForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.BodiesAtPointForGraph was not found by reflection");
    private static readonly MethodInfo CharacterCanStandMethod =
        typeof(GraphInterop).GetMethod("CharacterCanStandForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.CharacterCanStandForGraph was not found by reflection");
    private static readonly MethodInfo RagdollStartMethod =
        typeof(GraphInterop).GetMethod("RagdollStartForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.RagdollStartForGraph was not found by reflection");
    private static readonly MethodInfo RagdollStopMethod =
        typeof(GraphInterop).GetMethod("RagdollStopForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.RagdollStopForGraph was not found by reflection");
    private static readonly MethodInfo SetRagdollStrengthMethod =
        typeof(GraphInterop).GetMethod("SetRagdollStrengthForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetRagdollStrengthForGraph was not found by reflection");
    private static readonly MethodInfo RagdollImpulseMethod =
        typeof(GraphInterop).GetMethod("RagdollImpulseForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.RagdollImpulseForGraph was not found by reflection");
    private static readonly MethodInfo JointSetEnabledMethod =
        typeof(GraphInterop).GetMethod("JointSetEnabledForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.JointSetEnabledForGraph was not found by reflection");
    private static readonly MethodInfo JointRemoveMethod =
        typeof(GraphInterop).GetMethod("JointRemoveForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.JointRemoveForGraph was not found by reflection");
    private static readonly MethodInfo JointValueMethod =
        typeof(GraphInterop).GetMethod("JointValueForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.JointValueForGraph was not found by reflection");
    // Jump: one call into AverCharacter.Jump, which declines in mid-air on its own.
    private static readonly MethodInfo JumpMethod =
        typeof(GraphInterop).GetMethod("JumpForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.JumpForGraph was not found by reflection");
}
