// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Synapse steering and perception nodes.

using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using Aver.Framework;
using Aver.Scene;

namespace Aver.Graph;

public partial class GraphCompiler
{
    /// A three-out-parameter read of an entity for whichever component was asked for; `axis` is
    /// pushed only when not -1, letting the three orientation nodes share one interop surface with
    /// the two that take no extra argument.
    private void EmitPullVec3Read(Node node, string pinName, MethodInfo method, int axis,
                                  string inputPin = "entity")
    {
        if (_il == null) return;
        EmitPullInput(node, inputPin);
        if (axis >= 0) _il.Emit(OpCodes.Ldc_I4, axis);
        var xL = _il.DeclareLocal(typeof(float));
        var yL = _il.DeclareLocal(typeof(float));
        var zL = _il.DeclareLocal(typeof(float));
        _il.Emit(OpCodes.Ldloca, xL);
        _il.Emit(OpCodes.Ldloca, yL);
        _il.Emit(OpCodes.Ldloca, zL);
        _il.Emit(OpCodes.Call, method);
        if (pinName == "success") return;
        _il.Emit(OpCodes.Pop);
        _il.Emit(OpCodes.Ldloc, pinName == "x" ? xL : pinName == "y" ? yL : zL);
    }

    /// EmitPullVec3Read's shape narrowed to ONE out-parameter: GetBodyMass/GetBodyMotionType/
    /// GetBodyLayer/GetJointValue each call `bool Method(int handle, out T value)`. `success` is the
    /// bool return; the one remaining pin needs no x/y/z-style dispatch.
    private void EmitPullScalarRead(Node node, string pinName, MethodInfo method, Type outType, string inputPin)
    {
        if (_il == null) return;
        EmitPullInput(node, inputPin);
        var outL = _il.DeclareLocal(outType);
        _il.Emit(OpCodes.Ldloca, outL);
        _il.Emit(OpCodes.Call, method);
        if (pinName == "success") return;
        _il.Emit(OpCodes.Pop);
        _il.Emit(OpCodes.Ldloc, outL);
    }

    /// SynapseSteer's own emission -- not EmitPullVec3Read, since this node has four real outputs
    /// (forward, right, yawDelta, arrived) plus the method's own bool return ("success"). Same
    /// overall shape otherwise: push inputs, push one local address per out-parameter, Call, then
    /// either leave the bool on the stack or pop it and push the requested local.
    private void EmitPullSynapseSteer(Node node, string pinName)
    {
        if (_il == null) return;
        EmitPullInput(node, "entity");
        EmitPullInput(node, "dt");
        EmitPullInput(node, "targetX");
        EmitPullInput(node, "targetY");
        EmitPullInput(node, "targetZ");
        EmitPullInput(node, "turnRate");
        EmitPullInput(node, "arriveRadius");
        var forwardL = _il.DeclareLocal(typeof(float));
        var rightL = _il.DeclareLocal(typeof(float));
        var yawDeltaL = _il.DeclareLocal(typeof(float));
        var arrivedL = _il.DeclareLocal(typeof(bool));
        _il.Emit(OpCodes.Ldloca, forwardL);
        _il.Emit(OpCodes.Ldloca, rightL);
        _il.Emit(OpCodes.Ldloca, yawDeltaL);
        _il.Emit(OpCodes.Ldloca, arrivedL);
        _il.Emit(OpCodes.Call, SynapseSteerMethod);
        if (pinName == "success") return;
        _il.Emit(OpCodes.Pop);
        switch (pinName)
        {
            case "forward":  _il.Emit(OpCodes.Ldloc, forwardL);  break;
            case "right":    _il.Emit(OpCodes.Ldloc, rightL);    break;
            case "yawDelta": _il.Emit(OpCodes.Ldloc, yawDeltaL); break;
            case "arrived":  _il.Emit(OpCodes.Ldloc, arrivedL);  break;
        }
    }

    /// GetSynapsePerception's emission -- same shape as EmitPullSynapseSteer (three real outputs of
    /// MIXED type + bool return), since EmitPullVec3Read only produces float/float/float, not
    /// bool/int/float.
    private void EmitPullSynapsePerception(Node node, string pinName)
    {
        if (_il == null) return;
        EmitPullInput(node, "entity");
        var canSeeL = _il.DeclareLocal(typeof(bool));
        var lastTargetL = _il.DeclareLocal(typeof(int));
        var timeSinceSeenL = _il.DeclareLocal(typeof(float));
        _il.Emit(OpCodes.Ldloca, canSeeL);
        _il.Emit(OpCodes.Ldloca, lastTargetL);
        _il.Emit(OpCodes.Ldloca, timeSinceSeenL);
        _il.Emit(OpCodes.Call, SynapseGetPerceptionMethod);
        if (pinName == "success") return;
        _il.Emit(OpCodes.Pop);
        switch (pinName)
        {
            case "canSeeTarget":    _il.Emit(OpCodes.Ldloc, canSeeL);         break;
            case "lastKnownTarget": _il.Emit(OpCodes.Ldloc, lastTargetL);     break;
            case "timeSinceSeen":   _il.Emit(OpCodes.Ldloc, timeSinceSeenL);  break;
        }
    }
    private static readonly MethodInfo SynapseGetTargetMethod =
        typeof(GraphInterop).GetMethod("SynapseGetTargetForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SynapseGetTargetForGraph was not found by reflection");
    private static readonly MethodInfo SynapseSteerMethod =
        typeof(GraphInterop).GetMethod("SynapseSteerForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SynapseSteerForGraph was not found by reflection");
    private static readonly MethodInfo SynapseGetPerceptionMethod =
        typeof(GraphInterop).GetMethod("SynapseGetPerceptionForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SynapseGetPerceptionForGraph was not found by reflection");
}
