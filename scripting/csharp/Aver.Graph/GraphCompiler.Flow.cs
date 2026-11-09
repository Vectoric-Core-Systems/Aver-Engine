// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Flow control nodes: switch, branch, do-once, gate, flip-flop, while and for-each.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Reflection.Emit;
using Aver.Framework;
using Aver.Scene;

namespace Aver.Graph;

public partial class GraphCompiler
{
    /// branch: a bool condition, one incoming exec pulse, exactly one of `true`/`false` fires --
    /// unlike EmitExecFanOut, which fires all exec-out pins. switchint (Blueprint's Switch on Int):
    /// evaluate the selector once, run exactly one case chain or the default. A chain of compares,
    /// not an IL `switch` opcode, since the selector is an arbitrary author-wired int (negative/
    /// sparse/out-of-range are ordinary) -- a dense jump table doesn't fit, and four compares cost
    /// nothing at this scale. The selector is pulled once into a local, not re-pulled per case, since
    /// EmitPullOutput is deliberately uncached (a Raycast-fed selector would retrace per case).
    private void EmitSwitchInt(Node node)
    {
        if (_il == null) return;

        var sel = _il.DeclareLocal(typeof(int));
        EmitPullInput(node, "selector");
        _il.Emit(OpCodes.Stloc, sel);

        var takenPin = node.Pins.FirstOrDefault(p => p.IsOutput && p.Name == "taken" && p.Type == PinType.Int);
        LocalBuilder? taken = takenPin != null ? GetOrCreateExecLocal(node.Id, "taken", typeof(int)) : null;

        var endLabel = _il.DefineLabel();
        // Every case that the node actually declares a pin for -- so a hand-written file that gives a
        // SwitchInt only case0 and default compiles to exactly that, rather than to four branches
        // three of which can never be reached.
        for (int i = 0; ; ++i)
        {
            string pinName = "case" + i.ToString(System.Globalization.CultureInfo.InvariantCulture);
            if (!node.Pins.Any(p => p.IsOutput && p.Name == pinName && p.Type == PinType.Exec)) break;

            var nextTest = _il.DefineLabel();
            _il.Emit(OpCodes.Ldloc, sel);
            _il.Emit(OpCodes.Ldc_I4, i);
            _il.Emit(OpCodes.Bne_Un, nextTest);
            if (taken != null) { _il.Emit(OpCodes.Ldc_I4, i); _il.Emit(OpCodes.Stloc, taken); }
            Node? target = FindExecTarget(node.Id, pinName);
            if (target != null) EmitExecNode(target);
            _il.Emit(OpCodes.Br, endLabel);
            _il.MarkLabel(nextTest);
        }

        // The default arm. -1 rather than the case count, so "no case matched" is one value whatever
        // the node's width is and a graph reading `taken` does not have to know how many cases exist.
        if (taken != null) { _il.Emit(OpCodes.Ldc_I4_M1); _il.Emit(OpCodes.Stloc, taken); }
        Node? def = FindExecTarget(node.Id, "default");
        if (def != null) EmitExecNode(def);

        _il.MarkLabel(endLabel);
    }

    private void EmitBranch(Node node)
    {
        if (_il == null) return;
        EmitPullInput(node, "cond");

        var tookTruePin = node.Pins.FirstOrDefault(p => p.IsOutput && p.Name == "tookTrue" && p.Type == PinType.Bool);
        LocalBuilder? tookTrue = tookTruePin != null ? GetOrCreateExecLocal(node.Id, "tookTrue", typeof(bool)) : null;

        var falseLabel = _il.DefineLabel();
        var endLabel = _il.DefineLabel();
        _il.Emit(OpCodes.Brfalse, falseLabel);

        if (tookTrue != null) { _il.Emit(OpCodes.Ldc_I4_1); _il.Emit(OpCodes.Stloc, tookTrue); }
        Node? trueTarget = FindExecTarget(node.Id, "true");
        if (trueTarget != null) EmitExecNode(trueTarget);
        _il.Emit(OpCodes.Br, endLabel);

        _il.MarkLabel(falseLabel);
        if (tookTrue != null) { _il.Emit(OpCodes.Ldc_I4_0); _il.Emit(OpCodes.Stloc, tookTrue); }
        Node? falseTarget = FindExecTarget(node.Id, "false");
        if (falseTarget != null) EmitExecNode(falseTarget);

        _il.MarkLabel(endLabel);
    }

    /// while: `cond` (re-pulled fresh every pass, see PUSH VS PULL) gates a `loop` body, up to
    /// MaxLoopIterations -- past that, WarnLoopGuardTripped logs (node + event) and the loop stops as
    /// if `cond` went false, so a buggy graph loses one tick's correctness rather than hanging the
    /// caller. `done` runs once either way.
    // The reserved GraphVarStore key a stateful flow node keeps its memory under -- node ids are
    // unique and a VAR name cannot contain '$', so this can never collide with an author's variable.
    private static string FlowStateKey(Node node) => "$flow$" + node.Id;

    /// Emits `store.GetBool("$flow$<id>")` onto the stack.
    private void EmitLoadFlowState(Node node)
    {
        _il!.Emit(OpCodes.Ldarg, (short)_varStoreArgIndex);
        _il.Emit(OpCodes.Ldstr, FlowStateKey(node));
        _il.Emit(OpCodes.Call, VarGetMethodFor(PinType.Bool));
    }

    /// Emits `store.SetBool("$flow$<id>", <value already on the stack>)`.
    private void EmitStoreFlowState(Node node)
    {
        var tmp = _il!.DeclareLocal(typeof(bool));
        _il.Emit(OpCodes.Stloc, tmp);
        _il.Emit(OpCodes.Ldarg, (short)_varStoreArgIndex);
        _il.Emit(OpCodes.Ldstr, FlowStateKey(node));
        _il.Emit(OpCodes.Ldloc, tmp);
        _il.Emit(OpCodes.Call, VarSetMethodFor(PinType.Bool));
    }

    /// Every stateful flow node needs somewhere to remember; a graph with no VAR records has no store
    /// argument, so name which node needs one rather than letting the IL reference argument -1.
    private void RequireFlowState(Node node)
    {
        if (_varStoreArgIndex < 0)
            throw new InvalidOperationException(
                $"'{node.Type}' node '{node.Id}' remembers state between activations and needs a " +
                "variable store, but this graph declares no VAR records -- declare any VAR to give it one");
    }

    /// doOnce: `then` fires the FIRST time exec runs and never again, until `reset` runs.
    /// The stored bool means "already fired", so a fresh instance starts false and fires.
    private void EmitDoOnce(Node node)
    {
        if (_il == null) return;
        RequireFlowState(node);

        // RESET IS SAMPLED FIRST, so reset=true re-arms and fires in the same pass -- what a caller
        // wiring "reset and go" means, and lets one exec chain use the node instead of needing two.
        var afterReset = _il.DefineLabel();
        EmitPullInput(node, "reset");
        _il.Emit(OpCodes.Brfalse, afterReset);
        _il.Emit(OpCodes.Ldc_I4_0);
        EmitStoreFlowState(node);
        _il.MarkLabel(afterReset);

        var alreadyFired = _il.DefineLabel();
        EmitLoadFlowState(node);
        _il.Emit(OpCodes.Brtrue, alreadyFired);

        _il.Emit(OpCodes.Ldc_I4_1);
        EmitStoreFlowState(node);
        Node? then = FindExecTarget(node.Id, "then");
        if (then != null) EmitExecNode(then);

        _il.MarkLabel(alreadyFired);
    }

    /// gate: `then` fires only while open. `open`/`close` are sampled every activation and applied
    /// BEFORE the test, so opening and firing in one activation works. Closed is default (Blueprint's Gate).
    private void EmitGate(Node node)
    {
        if (_il == null) return;
        RequireFlowState(node);

        var afterOpen = _il.DefineLabel();
        EmitPullInput(node, "open");
        _il.Emit(OpCodes.Brfalse, afterOpen);
        _il.Emit(OpCodes.Ldc_I4_1);
        EmitStoreFlowState(node);
        _il.MarkLabel(afterOpen);

        // close AFTER open, so a graph wiring both true ends closed -- one rule, stated here,
        // rather than an order that depends on which link the parser happened to read first.
        var afterClose = _il.DefineLabel();
        EmitPullInput(node, "close");
        _il.Emit(OpCodes.Brfalse, afterClose);
        _il.Emit(OpCodes.Ldc_I4_0);
        EmitStoreFlowState(node);
        _il.MarkLabel(afterClose);

        var shut = _il.DefineLabel();
        EmitLoadFlowState(node);
        _il.Emit(OpCodes.Brfalse, shut);
        Node? then = FindExecTarget(node.Id, "then");
        if (then != null) EmitExecNode(then);
        _il.MarkLabel(shut);
    }

    /// flipFlop: alternates between the `a` and `b` exec outputs, starting with `a`, and
    /// reports which one it just took on `isA`.
    private void EmitFlipFlop(Node node)
    {
        if (_il == null) return;
        RequireFlowState(node);

        // Stored bool means "b is next". Starts false, so the first activation takes `a`.
        var takeB = _il.DefineLabel();
        var done = _il.DefineLabel();

        EmitLoadFlowState(node);
        _il.Emit(OpCodes.Brtrue, takeB);

        _il.Emit(OpCodes.Ldc_I4_1);
        EmitStoreFlowState(node);
        if (_pinLocals.TryGetValue((node.Id, "isA"), out var isATrue))
        {
            _il.Emit(OpCodes.Ldc_I4_1);
            _il.Emit(OpCodes.Stloc, isATrue);
        }
        Node? a = FindExecTarget(node.Id, "a");
        if (a != null) EmitExecNode(a);
        _il.Emit(OpCodes.Br, done);

        _il.MarkLabel(takeB);
        _il.Emit(OpCodes.Ldc_I4_0);
        EmitStoreFlowState(node);
        if (_pinLocals.TryGetValue((node.Id, "isA"), out var isAFalse))
        {
            _il.Emit(OpCodes.Ldc_I4_0);
            _il.Emit(OpCodes.Stloc, isAFalse);
        }
        Node? b = FindExecTarget(node.Id, "b");
        if (b != null) EmitExecNode(b);

        _il.MarkLabel(done);
    }

    private void EmitWhile(Node node)
    {
        if (_il == null) return;
        var iterations = GetOrCreateExecLocal(node.Id, "iterations", typeof(int));
        _il.Emit(OpCodes.Ldc_I4_0);
        _il.Emit(OpCodes.Stloc, iterations);

        var loopCheck = _il.DefineLabel();
        var guardTripped = _il.DefineLabel();
        var loopDone = _il.DefineLabel();

        _il.MarkLabel(loopCheck);
        EmitPullInput(node, "cond");
        _il.Emit(OpCodes.Brfalse, loopDone);

        _il.Emit(OpCodes.Ldloc, iterations);
        _il.Emit(OpCodes.Ldc_I4, MaxLoopIterations);
        _il.Emit(OpCodes.Bge, guardTripped);

        _il.Emit(OpCodes.Ldloc, iterations);
        _il.Emit(OpCodes.Ldc_I4_1);
        _il.Emit(OpCodes.Add);
        _il.Emit(OpCodes.Stloc, iterations);

        Node? body = FindExecTarget(node.Id, "loop");
        if (body != null) EmitExecNode(body);
        _il.Emit(OpCodes.Br, loopCheck);

        _il.MarkLabel(guardTripped);
        _il.Emit(OpCodes.Ldstr, node.Id);
        _il.Emit(OpCodes.Ldstr, _currentEventName);
        _il.Emit(OpCodes.Call, WarnLoopGuardMethod);

        _il.MarkLabel(loopDone);
        Node? done = FindExecTarget(node.Id, "done");
        if (done != null) EmitExecNode(done);
    }

    /// forEach: the counted-repeat variant (no array/collection pin type exists yet -- see
    /// OcGraphParser.AddDefaultPins's "foreach" case). `index` runs
    /// 0..count-1 through `loop`; `index >= count` already prevents "forever", and
    /// MaxLoopIterations is a second, redundant guard for a corrupted/absurd `count`.
    private void EmitForEach(Node node)
    {
        if (_il == null) return;
        var count = _il.DeclareLocal(typeof(int)); // plain local: `count` is read once per activation, not exposed as a pin
        EmitPullInput(node, "count");
        _il.Emit(OpCodes.Stloc, count);

        var index = GetOrCreateExecLocal(node.Id, "index", typeof(int));
        _il.Emit(OpCodes.Ldc_I4_0);
        _il.Emit(OpCodes.Stloc, index);

        var loopCheck = _il.DefineLabel();
        var guardTripped = _il.DefineLabel();
        var loopDone = _il.DefineLabel();

        _il.MarkLabel(loopCheck);
        _il.Emit(OpCodes.Ldloc, index);
        _il.Emit(OpCodes.Ldloc, count);
        _il.Emit(OpCodes.Bge, loopDone); // index >= count -> done (also the whole story for count <= 0)

        _il.Emit(OpCodes.Ldloc, index);
        _il.Emit(OpCodes.Ldc_I4, MaxLoopIterations);
        _il.Emit(OpCodes.Bge, guardTripped);

        Node? body = FindExecTarget(node.Id, "loop");
        if (body != null) EmitExecNode(body);

        _il.Emit(OpCodes.Ldloc, index);
        _il.Emit(OpCodes.Ldc_I4_1);
        _il.Emit(OpCodes.Add);
        _il.Emit(OpCodes.Stloc, index);
        _il.Emit(OpCodes.Br, loopCheck);

        _il.MarkLabel(guardTripped);
        _il.Emit(OpCodes.Ldstr, node.Id);
        _il.Emit(OpCodes.Ldstr, _currentEventName);
        _il.Emit(OpCodes.Call, WarnLoopGuardMethod);

        _il.MarkLabel(loopDone);
        Node? done = FindExecTarget(node.Id, "done");
        if (done != null) EmitExecNode(done);
    }
}
