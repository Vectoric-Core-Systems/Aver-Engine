// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Custom events, save/load, printing and simple API call nodes.

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
    private void EmitExecApiCall(Node node)
    {
        if (_il == null) return;
        switch (node.Type.ToLowerInvariant())
        {
            case "setvelocity":
                EmitPullInput(node, "entity"); EmitPullInput(node, "x");
                EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, SetVelocityMethod); break;
            case "teleport":
                EmitPullInput(node, "entity"); EmitPullInput(node, "x");
                EmitPullInput(node, "y"); EmitPullInput(node, "z");
                _il.Emit(OpCodes.Call, TeleportMethod); break;
            case "possess":
                EmitPullInput(node, "controller"); EmitPullInput(node, "pawn");
                _il.Emit(OpCodes.Call, PossessMethod); break;
            case "setvisible":
                EmitPullInput(node, "entity"); EmitPullInput(node, "visible");
                _il.Emit(OpCodes.Call, SetVisibleMethod); break;
            case "addtag":
                EmitPullInput(node, "entity"); EmitPullInput(node, "mask");
                _il.Emit(OpCodes.Call, AddTagMethod); break;
            case "removetag":
                EmitPullInput(node, "entity"); EmitPullInput(node, "mask");
                _il.Emit(OpCodes.Call, RemoveTagMethod); break;
            // SetLayerCollision has no body/entity prefix -- edits the world's shared layer matrix,
            // hence sitting here rather than EmitExecPhysicsWrite (body-first shape).
            case "setlayercollision":
                EmitPullInput(node, "layerA"); EmitPullInput(node, "layerB"); EmitPullInput(node, "collide");
                _il.Emit(OpCodes.Call, SetLayerCollisionMethod); break;
            default:
                EmitPullInput(node, "controller");
                _il.Emit(OpCodes.Call, UnpossessMethod); break;
        }
        // Into an exec local, not _pinLocals -- a fix, not a preference: _pinLocals is empty while
        // CompileEntryPoint runs, and wiring into the exec chain alone didn't help either, since
        // EmitPullOutput checks _execLocals first. Makes `success` readable for
        // SetVelocity/Teleport/Possess/Unpossess/SetVisible/AddTag/RemoveTag (Teleport false on a
        // capsule-less character is the case that matters). Only this emitter, EmitExecPhysicsWrite
        // and EmitExecTransformWrite need the pattern; the rest (EmitSetField/SetParent/SetName/
        // SetMesh/SetMaterial/SetFieldVec3/GetForward/GetViewEntity) run from EmitNode's PULL switch
        // where _pinLocals is already populated. Nothing outstanding.
        var successPin = node.Pins.FirstOrDefault(p => p.IsOutput && p.Name == "success" && p.Type == PinType.Bool);
        if (successPin != null) _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, "success", typeof(bool)));
        else                    _il.Emit(OpCodes.Pop);
    }

    /// The single-output API reads: at most one input pin, one call, one output local.
    private void EmitSimpleApiRead(Node node)
    {
        if (_il == null) return;
        foreach (var p in node.Pins)
        {
            if (!p.IsOutput) continue;
            if (!_pinLocals.TryGetValue((node.Id, p.Name), out var local)) continue;
            EmitPullOutput(node, p.Name);
            _il.Emit(OpCodes.Stloc, local);
        }
    }

    /// Print(value) -> then: writes one line to the log labelled with the node's own id (no
    /// attribute needed), e.g. `NODE muzzleLen Print` prints "muzzleLen = 35". Pushed as a
    /// compile-time constant.
    private void EmitExecPrint(Node node)
    {
        if (_il == null) return;
        _il.Emit(OpCodes.Ldstr, node.Id);

        // PrintString takes no value pin, so it must not go through EmitPullInput at all -- pulling
        // a pin that doesn't exist would load an unset local (the shape the exec-emitter rule
        // already warns about) instead of failing. Both operands are compile-time constants here --
        // nothing has to be wired for this node to say something.
        if (node.Type.Equals("printstring", StringComparison.OrdinalIgnoreCase))
        {
            _il.Emit(OpCodes.Ldstr, node.PrintText ?? string.Empty);
            _il.Emit(OpCodes.Call, PrintStringMethod);
            return;
        }

        EmitPullInput(node, "value");
        _il.Emit(OpCodes.Call,
                 node.Type.Equals("printint", StringComparison.OrdinalIgnoreCase) ? PrintIntMethod : PrintMethod);
    }

    /// Print gets its own predicate for the reason every side effect above it does: one per type,
    /// so a predicate's name never stops describing what it matches.
    private static bool IsExecCapablePrintType(string type) =>
        type.Equals("print", StringComparison.OrdinalIgnoreCase) ||
        type.Equals("printint", StringComparison.OrdinalIgnoreCase) ||
        type.Equals("printstring", StringComparison.OrdinalIgnoreCase);

    private static bool IsExecCapableApiCallType(string type)
    {
        string t = type.ToLowerInvariant();
        return t == "setvelocity" || t == "teleport" || t == "possess" || t == "unpossess" ||
               t == "setvisible" || t == "addtag" || t == "removetag" || t == "setlayercollision";
    }

    /// FireEvent's own version of IsExecCapableSpawnType -- refused by the PULL compiler's
    /// topological pass entirely, same stricter treatment as Spawn/CharacterMove. FireEventForGraph
    /// routes into a different GraphHost (not P/Invoke, not GraphVarStore), with its own signature
    /// (target int, event name string in; bool "did it run" out) and failure mode (no live graph
    /// bound, or one that never declared this event).
    private static bool IsExecCapableFireEventType(string type) =>
        type.Equals("fireevent", StringComparison.OrdinalIgnoreCase);

    /// SaveGame/LoadGame's own version -- one predicate since both share one emitter
    /// (EmitExecSaveLoad). Refused by the PULL compiler entirely: LoadGame replaces the whole world.
    private static bool IsExecCapableSaveLoadType(string type) =>
        type.Equals("savegame", StringComparison.OrdinalIgnoreCase) ||
        type.Equals("loadgame", StringComparison.OrdinalIgnoreCase);

    /// Runs a FireEvent node's call once, when reached -- mirrors EmitExecSpawn/EmitExecCharacterMove's
    /// shape, but event= is a node-line attribute (Node.EventName) required non-empty at compile
    /// time, like Spawn's class= (empty could never fire anything). Unlike class=, `target` is an
    /// ordinary pin: the entity to fire at is computed at runtime.
    private void EmitExecFireEvent(Node node)
    {
        if (_il == null) return;
        if (string.IsNullOrEmpty(node.EventName))
            throw new InvalidOperationException(
                $"FireEvent node '{node.Id}' has no event= attribute naming which event to fire");

        EmitPullInput(node, "target");
        _il.Emit(OpCodes.Ldstr, node.EventName);
        _il.Emit(OpCodes.Call, FireEventMethod);

        if (node.Pins.Any(p => p.IsOutput && p.Name == "fired"))
        {
            var firedLocal = GetOrCreateExecLocal(node.Id, "fired", typeof(bool));
            _il.Emit(OpCodes.Stloc, firedLocal);
        }
        else
        {
            _il.Emit(OpCodes.Pop);
        }
    }

    /// Runs a SaveGame or LoadGame write once, when reached -- the one emitter both share (see
    /// IsExecCapableSaveLoadType). Mirrors EmitExecSetName otherwise: push path=, call, capture-or-
    /// discard "success". No entity pulled, unlike the rest of this family: these act on the whole world.
    private void EmitExecSaveLoad(Node node)
    {
        if (_il == null) return;

        bool isSave = node.Type.Equals("savegame", StringComparison.OrdinalIgnoreCase);
        if (string.IsNullOrEmpty(node.SavePath))
            throw new InvalidOperationException(
                $"{(isSave ? "SaveGame" : "LoadGame")} node '{node.Id}' has no path= attribute naming the file");

        _il.Emit(OpCodes.Ldstr, node.SavePath);
        _il.Emit(OpCodes.Call, isSave ? SaveGameMethod : LoadGameMethod);

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
    private static readonly MethodInfo SetLayerCollisionMethod =
        typeof(GraphInterop).GetMethod("SetLayerCollisionForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetLayerCollisionForGraph was not found by reflection");
    private static readonly MethodInfo SetVelocityMethod =
        typeof(GraphInterop).GetMethod("SetVelocityForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetVelocityForGraph was not found by reflection");

    // The tag/visibility family. Bound by REFLECTION like every other interop method here, which is
    // why the throw names the member: a rename on the Aver.Framework side would otherwise surface as
    // a null MethodInfo inside an emitter, thousands of lines from the cause.
    private static readonly MethodInfo SetVisibleMethod =
        typeof(GraphInterop).GetMethod("SetVisibleForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SetVisibleForGraph was not found by reflection");
    private static readonly MethodInfo AddTagMethod =
        typeof(GraphInterop).GetMethod("AddTagForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.AddTagForGraph was not found by reflection");
    private static readonly MethodInfo RemoveTagMethod =
        typeof(GraphInterop).GetMethod("RemoveTagForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.RemoveTagForGraph was not found by reflection");
    private static readonly MethodInfo TeleportMethod =
        typeof(GraphInterop).GetMethod("TeleportForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.TeleportForGraph was not found by reflection");
    private static readonly MethodInfo PossessMethod =
        typeof(GraphInterop).GetMethod("PossessForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.PossessForGraph was not found by reflection");
    private static readonly MethodInfo UnpossessMethod =
        typeof(GraphInterop).GetMethod("UnpossessForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.UnpossessForGraph was not found by reflection");
    private static readonly MethodInfo SaveGameMethod =
        typeof(GraphInterop).GetMethod("SaveGameForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.SaveGameForGraph was not found by reflection");
    private static readonly MethodInfo LoadGameMethod =
        typeof(GraphInterop).GetMethod("LoadGameForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.LoadGameForGraph was not found by reflection");
    // PrintInt: the same line for an INT pin, because a float cannot hold an entity handle.
    private static readonly MethodInfo PrintIntMethod =
        typeof(GraphInterop).GetMethod("PrintIntForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.PrintIntForGraph was not found by reflection");
    // PrintString: an authored message rather than a value, for a node that needs nothing wired.
    private static readonly MethodInfo PrintStringMethod =
        typeof(GraphInterop).GetMethod("PrintStringForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.PrintStringForGraph was not found by reflection");
    // Print: one line to the log, labelled with the node id the emitter pushes.
    private static readonly MethodInfo PrintMethod =
        typeof(GraphInterop).GetMethod("PrintForGraph", BindingFlags.NonPublic | BindingFlags.Static)
        ?? throw new InvalidOperationException("Aver.Framework.GraphInterop.PrintForGraph was not found by reflection");
    // FireEvent: GAP 3, the cross-entity event node -- reflected differently from every wrapper
    // above. GraphEvents lives in this same assembly (Aver.Graph) with a public Router-dispatching
    // method (see GraphEvents.cs for why), so an ordinary public GetMethod suffices -- no
    // BindingFlags.NonPublic, the same shape the GraphVarStore accessors below use, unlike
    // Native/Fw/GraphInterop's cross-assembly internals.
    private static readonly MethodInfo FireEventMethod =
        typeof(GraphEvents).GetMethod(nameof(GraphEvents.FireEventForGraph))
        ?? throw new InvalidOperationException("Aver.Graph.GraphEvents.FireEventForGraph was not found by reflection");
}
