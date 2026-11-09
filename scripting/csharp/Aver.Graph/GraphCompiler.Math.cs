// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Constants, vectors, comparisons and scalar arithmetic nodes.

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
    private void EmitConstFloat(Node node)
    {
        if (_il == null) return;

        float value = 0f;
        var co = _graph.ConstantOutputs.FirstOrDefault(c => c.NodeId == node.Id);
        if (co != null && co.Value is float f)
            value = f;

        _il.Emit(OpCodes.Ldc_R4, value);

        if (_pinLocals.TryGetValue((node.Id, "value"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitConstInt(Node node)
    {
        if (_il == null) return;

        int value = 0;
        var co = _graph.ConstantOutputs.FirstOrDefault(c => c.NodeId == node.Id);
        if (co != null && co.Value is int i)
            value = i;

        _il.Emit(OpCodes.Ldc_I4, value);

        if (_pinLocals.TryGetValue((node.Id, "value"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitConstBool(Node node)
    {
        if (_il == null) return;

        bool value = false;
        var co = _graph.ConstantOutputs.FirstOrDefault(c => c.NodeId == node.Id);
        if (co != null && co.Value is bool b)
            value = b;

        _il.Emit(OpCodes.Ldc_I4, value ? 1 : 0);

        if (_pinLocals.TryGetValue((node.Id, "value"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    /// One output component of a vector node, onto the stack.
    ///
    /// Recomputed per output rather than hoisted into shared locals, deliberately: both compilers
    /// already re-emit a pure expression at every read (that's EmitPullOutput), and Normalize's
    /// worst case (a sqrt per component) is still just a handful of float ops. Dup-then-Mul squares
    /// so a squared term pulls its input once -- pulling twice would emit a subgraph input twice.
    private void EmitVecComponent(Node node, string pinName)
    {
        if (_il == null) return;

        void In(string p) => EmitPullInput(node, p);
        void Sq(string p) { In(p); _il.Emit(OpCodes.Dup); _il.Emit(OpCodes.Mul); }
        void DiffSq(string a, string b) { In(a); In(b); _il.Emit(OpCodes.Sub); _il.Emit(OpCodes.Dup); _il.Emit(OpCodes.Mul); }
        void Sqrt() { _il.Emit(OpCodes.Conv_R8); _il.Emit(OpCodes.Call, MathSqrtMethod); _il.Emit(OpCodes.Conv_R4); }
        void Len() { Sq("ax"); Sq("ay"); _il.Emit(OpCodes.Add); Sq("az"); _il.Emit(OpCodes.Add); Sqrt(); }

        string other = pinName == "x" ? "ax" : pinName == "y" ? "ay" : "az";
        string bOther = pinName == "x" ? "bx" : pinName == "y" ? "by" : "bz";

        switch (node.Type.ToLowerInvariant())
        {
            case "vecadd": In(other); In(bOther); _il.Emit(OpCodes.Add); return;
            case "vecsub": In(other); In(bOther); _il.Emit(OpCodes.Sub); return;
            case "vecscale": In(other); In("s"); _il.Emit(OpCodes.Mul); return;

            // x = ay*bz - az*by, and the two cyclic rotations of it.
            case "veccross":
                if (pinName == "x") { In("ay"); In("bz"); _il.Emit(OpCodes.Mul); In("az"); In("by"); _il.Emit(OpCodes.Mul); _il.Emit(OpCodes.Sub); }
                else if (pinName == "y") { In("az"); In("bx"); _il.Emit(OpCodes.Mul); In("ax"); In("bz"); _il.Emit(OpCodes.Mul); _il.Emit(OpCodes.Sub); }
                else { In("ax"); In("by"); _il.Emit(OpCodes.Mul); In("ay"); In("bx"); _il.Emit(OpCodes.Mul); _il.Emit(OpCodes.Sub); }
                return;

            // Divided by max(length, 1e-6): a zero vector normalises to zero rather than to NaN,
            // and a NaN here would propagate into a transform and take the actor with it.
            case "vecnormalize":
                In(other); Len(); _il.Emit(OpCodes.Ldc_R4, 1e-6f); _il.Emit(OpCodes.Call, MathMaxMethod);
                _il.Emit(OpCodes.Div); return;

            // a + (b - a) * t, which is exact at t = 0 and t = 1 -- unlike a*(1-t) + b*t.
            case "veclerp":
                In(other); In(bOther); In(other); _il.Emit(OpCodes.Sub); In("t"); _il.Emit(OpCodes.Mul);
                _il.Emit(OpCodes.Add); return;

            case "vecdot":
                In("ax"); In("bx"); _il.Emit(OpCodes.Mul);
                In("ay"); In("by"); _il.Emit(OpCodes.Mul); _il.Emit(OpCodes.Add);
                In("az"); In("bz"); _il.Emit(OpCodes.Mul); _il.Emit(OpCodes.Add); return;

            case "veclength": Len(); return;

            case "vecdistance":
                DiffSq("ax", "bx"); DiffSq("ay", "by"); _il.Emit(OpCodes.Add);
                DiffSq("az", "bz"); _il.Emit(OpCodes.Add); Sqrt(); return;

            default:
                throw new InvalidOperationException(
                    $"vector node '{node.Id}' has type {node.Type}, which EmitVecComponent does not know");
        }
    }

    /// The PULL compiler's topological visit of a vector node: every output with a declared local
    /// gets computed and stored; an unread output has no local, so an unread Cross costs nothing.
    private void EmitVecNode(Node node)
    {
        if (_il == null) return;
        foreach (var p in node.Pins)
        {
            if (!p.IsOutput) continue;
            if (!_pinLocals.TryGetValue((node.Id, p.Name), out var local)) continue;
            EmitVecComponent(node, p.Name);
            _il.Emit(OpCodes.Stloc, local);
        }
    }

    private void EmitAdd(Node node)
    {
        if (_il == null) return;

        LoadPin(node.Id, "a");
        LoadPin(node.Id, "b");
        _il.Emit(OpCodes.Add);
        if (_pinLocals.TryGetValue((node.Id, "result"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitMultiply(Node node)
    {
        if (_il == null) return;

        LoadPin(node.Id, "a");
        LoadPin(node.Id, "b");
        _il.Emit(OpCodes.Mul);
        if (_pinLocals.TryGetValue((node.Id, "result"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitCompare(Node node)
    {
        if (_il == null) return;

        LoadPin(node.Id, "a");
        LoadPin(node.Id, "b");
        _il.Emit(OpCodes.Cgt);
        if (_pinLocals.TryGetValue((node.Id, "result"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitSin(Node node)
    {
        if (_il == null) return;

        // Math.Sin takes/returns double; the graph is float end to end, so widen/narrow is required.
        // Skipping either still compiles (verifier accepts it) but silently reinterprets bits.
        LoadPin(node.Id, "a");
        _il.Emit(OpCodes.Conv_R8);
        _il.Emit(OpCodes.Call, MathSinMethod);
        _il.Emit(OpCodes.Conv_R4);

        if (_pinLocals.TryGetValue((node.Id, "result"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitCos(Node node)
    {
        if (_il == null) return;

        LoadPin(node.Id, "a");
        _il.Emit(OpCodes.Conv_R8);
        _il.Emit(OpCodes.Call, MathCosMethod);
        _il.Emit(OpCodes.Conv_R4);

        if (_pinLocals.TryGetValue((node.Id, "result"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitSubtract(Node node)
    {
        if (_il == null) return;

        LoadPin(node.Id, "a");
        LoadPin(node.Id, "b");
        _il.Emit(OpCodes.Sub);

        if (_pinLocals.TryGetValue((node.Id, "result"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitDivide(Node node)
    {
        if (_il == null) return;

        // Divide-by-zero convention: b == 0.0 yields 0.0, not IEEE754 NaN/Infinity -- a bare `div`
        // never throws (5/0 = +Inf, 0/0 = NaN), and NaN/Inf reaching a transform propagates silently
        // until an object vanishes or explodes frames later. 0.0 is inert, so a flight path can
        // continue through it where NaN cannot; deliberate choice -- flag if a consumer ever needs
        // propagating NaN.
        var aLocal = _il.DeclareLocal(typeof(float));
        var bLocal = _il.DeclareLocal(typeof(float));
        LoadPin(node.Id, "a");
        _il.Emit(OpCodes.Stloc, aLocal);
        LoadPin(node.Id, "b");
        _il.Emit(OpCodes.Stloc, bLocal);

        var zeroLabel = _il.DefineLabel();
        var endLabel = _il.DefineLabel();

        _il.Emit(OpCodes.Ldloc, bLocal);
        _il.Emit(OpCodes.Ldc_R4, 0f);
        _il.Emit(OpCodes.Ceq);
        _il.Emit(OpCodes.Brtrue, zeroLabel);

        _il.Emit(OpCodes.Ldloc, aLocal);
        _il.Emit(OpCodes.Ldloc, bLocal);
        _il.Emit(OpCodes.Div);
        _il.Emit(OpCodes.Br, endLabel);

        _il.MarkLabel(zeroLabel);
        _il.Emit(OpCodes.Ldc_R4, 0f);

        _il.MarkLabel(endLabel);
        if (_pinLocals.TryGetValue((node.Id, "result"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    /// Select(cond, ifTrue, ifFalse) -> result: picks one of two float values by a bool condition.
    /// Not short-circuiting: ifTrue/ifFalse are already computed upstream (topological pass runs
    /// every node once regardless of condition), unlike Branch's exec fan-out which genuinely skips
    /// the untaken arm. The Brfalse/Br pair here only saves the final Ldloc -- a branchless
    /// "compute both, keep one" would be equally correct.
    private void EmitSelect(Node node)
    {
        if (_il == null) return;

        LoadPin(node.Id, "cond");

        var falseLabel = _il.DefineLabel();
        var endLabel = _il.DefineLabel();
        _il.Emit(OpCodes.Brfalse, falseLabel);

        LoadPin(node.Id, "ifTrue");
        _il.Emit(OpCodes.Br, endLabel);

        _il.MarkLabel(falseLabel);
        LoadPin(node.Id, "ifFalse");

        _il.MarkLabel(endLabel);
        if (_pinLocals.TryGetValue((node.Id, "result"), out var local))
            _il.Emit(OpCodes.Stloc, local);
    }

    /// Mirrors EmitDivide's zero-divisor convention (b == 0 yields 0.0, never NaN/Infinity -- see
    /// that comment), but pushes the result rather than storing it to `_pinLocals`.
    private void EmitPullDivide(Node node)
    {
        if (_il == null) return;
        var aLocal = _il.DeclareLocal(typeof(float));
        var bLocal = _il.DeclareLocal(typeof(float));
        EmitPullInput(node, "a"); _il.Emit(OpCodes.Stloc, aLocal);
        EmitPullInput(node, "b"); _il.Emit(OpCodes.Stloc, bLocal);

        var zeroLabel = _il.DefineLabel();
        var endLabel = _il.DefineLabel();
        _il.Emit(OpCodes.Ldloc, bLocal);
        _il.Emit(OpCodes.Ldc_R4, 0f);
        _il.Emit(OpCodes.Ceq);
        _il.Emit(OpCodes.Brtrue, zeroLabel);
        _il.Emit(OpCodes.Ldloc, aLocal);
        _il.Emit(OpCodes.Ldloc, bLocal);
        _il.Emit(OpCodes.Div);
        _il.Emit(OpCodes.Br, endLabel);
        _il.MarkLabel(zeroLabel);
        _il.Emit(OpCodes.Ldc_R4, 0f);
        _il.MarkLabel(endLabel);
    }
    // One emitter, called from both compilers: the "operands on the stack" -> "result on the stack"
    // arithmetic is identical either way, so it lives once here.
    // The PUSH-path wrappers: load each input from its pin local, run the shared arithmetic, store
    // the result. Split by arity, not one variadic helper, so a wrong-arity node fails at compile
    // time in the emitter it named, rather than silently reading a stale stack slot.
    private void EmitScalarUnary(Node node)
    {
        if (_il == null) return;
        LoadPin(node.Id, "a");
        EmitScalarOp(node.Type);
        if (_pinLocals.TryGetValue((node.Id, "result"), out var local)) _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitScalarBinary(Node node)
    {
        if (_il == null) return;
        LoadPin(node.Id, "a");
        LoadPin(node.Id, "b");
        EmitScalarOp(node.Type);
        if (_pinLocals.TryGetValue((node.Id, "result"), out var local)) _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitScalarTernary(Node node)
    {
        if (_il == null) return;
        if (node.Type == "clamp")
        {
            LoadPin(node.Id, "a"); LoadPin(node.Id, "min"); LoadPin(node.Id, "max");
        }
        else
        {
            LoadPin(node.Id, "a"); LoadPin(node.Id, "b"); LoadPin(node.Id, "t");
        }
        EmitScalarOp(node.Type);
        if (_pinLocals.TryGetValue((node.Id, "result"), out var local)) _il.Emit(OpCodes.Stloc, local);
    }

    private void EmitScalarOp(string type)
    {
        if (_il == null) return;
        // Lowercased here, not by callers: Node.Type keeps whatever case the .ocgraph wrote, so
        // doing it once here stops "Lerp" compiling and "lerp" not.
        switch (type.ToLowerInvariant())
        {
            // Bools are I4 on the stack, so bitwise ops ARE logical ops -- every producer of a Bool
            // pin emits Ceq/Cgt/Clt or a const, guaranteeing 0 or 1, never an arbitrary integer.
            case "and": _il.Emit(OpCodes.And); break;
            case "or":   _il.Emit(OpCodes.Or); break;
            case "xor": _il.Emit(OpCodes.Xor); break;
            // NOT is "== 0", not a bitwise complement: ~1 is -2, which is truthy everywhere it
            // would later be tested.
            case "not": _il.Emit(OpCodes.Ldc_I4_0); _il.Emit(OpCodes.Ceq); break;

            case "greater": _il.Emit(OpCodes.Cgt); break;
            case "less":    _il.Emit(OpCodes.Clt); break;
            case "equal":   _il.Emit(OpCodes.Ceq); break;
            // CIL has no Cge/Cle/Cne, so each is the opposite comparison inverted by "== 0".
            case "greaterequal":
                _il.Emit(OpCodes.Clt); _il.Emit(OpCodes.Ldc_I4_0); _il.Emit(OpCodes.Ceq); break;
            case "lessequal":
                _il.Emit(OpCodes.Cgt); _il.Emit(OpCodes.Ldc_I4_0); _il.Emit(OpCodes.Ceq); break;
            case "notequal":
                _il.Emit(OpCodes.Ceq); _il.Emit(OpCodes.Ldc_I4_0); _il.Emit(OpCodes.Ceq); break;

            case "min": _il.Emit(OpCodes.Call, MathMinMethod); break;
            case "max": _il.Emit(OpCodes.Call, MathMaxMethod); break;
            case "mod": _il.Emit(OpCodes.Rem); break;
            case "abs": _il.Emit(OpCodes.Call, MathAbsMethod); break;
            case "negate": _il.Emit(OpCodes.Neg); break;
            case "pow":
                // Two operands, so BOTH need widening -- and the stack order forbids simply
                // converting the top one twice. Round-trip through locals rather than guess.
                {
                    var pb = _il.DeclareLocal(typeof(float));
                    _il.Emit(OpCodes.Stloc, pb);
                    _il.Emit(OpCodes.Conv_R8);
                    _il.Emit(OpCodes.Ldloc, pb);
                    _il.Emit(OpCodes.Conv_R8);
                    _il.Emit(OpCodes.Call, MathPowMethod);
                    _il.Emit(OpCodes.Conv_R4);
                }
                break;
            case "sqrt":
                _il.Emit(OpCodes.Conv_R8); _il.Emit(OpCodes.Call, MathSqrtMethod); _il.Emit(OpCodes.Conv_R4); break;
            case "floor":
                _il.Emit(OpCodes.Conv_R8); _il.Emit(OpCodes.Call, MathFloorMethod); _il.Emit(OpCodes.Conv_R4); break;
            case "ceil":
                _il.Emit(OpCodes.Conv_R8); _il.Emit(OpCodes.Call, MathCeilMethod); _il.Emit(OpCodes.Conv_R4); break;
            case "round":
                _il.Emit(OpCodes.Conv_R8); _il.Emit(OpCodes.Call, MathRoundMethod); _il.Emit(OpCodes.Conv_R4); break;
            // saturate is clamp(x, 0, 1), spelled with the same Min/Max the clamp node uses so
            // the two cannot disagree about edge behaviour.
            case "saturate":
                _il.Emit(OpCodes.Ldc_R4, 1.0f); _il.Emit(OpCodes.Call, MathMinMethod);
                _il.Emit(OpCodes.Ldc_R4, 0.0f); _il.Emit(OpCodes.Call, MathMaxMethod); break;
            // clamp takes THREE operands (a, min, max) already on the stack in that order.
            case "clamp":
                {
                    var hi = _il.DeclareLocal(typeof(float));
                    _il.Emit(OpCodes.Stloc, hi);              // stack: a, min
                    _il.Emit(OpCodes.Call, MathMaxMethod);    // stack: max(a, min)
                    _il.Emit(OpCodes.Ldloc, hi);
                    _il.Emit(OpCodes.Call, MathMinMethod);
                }
                break;
            // lerp(a, b, t) = a + (b - a) * t, with a, b, t on the stack in that order.
            case "lerp":
                {
                    var lt = _il.DeclareLocal(typeof(float));
                    var lb = _il.DeclareLocal(typeof(float));
                    var la = _il.DeclareLocal(typeof(float));
                    _il.Emit(OpCodes.Stloc, lt);
                    _il.Emit(OpCodes.Stloc, lb);
                    _il.Emit(OpCodes.Stloc, la);
                    _il.Emit(OpCodes.Ldloc, la);
                    _il.Emit(OpCodes.Ldloc, lb);
                    _il.Emit(OpCodes.Ldloc, la);
                    _il.Emit(OpCodes.Sub);
                    _il.Emit(OpCodes.Ldloc, lt);
                    _il.Emit(OpCodes.Mul);
                    _il.Emit(OpCodes.Add);
                }
                break;
            default:
                throw new InvalidOperationException($"EmitScalarOp has no arithmetic for '{type}'");
        }
    }

    private static readonly MethodInfo MathSinMethod =
        typeof(Math).GetMethod(nameof(Math.Sin), new[] { typeof(double) })
        ?? throw new InvalidOperationException("System.Math.Sin(double) was not found by reflection");
    private static readonly MethodInfo MathCosMethod =
        typeof(Math).GetMethod(nameof(Math.Cos), new[] { typeof(double) })
        ?? throw new InvalidOperationException("System.Math.Cos(double) was not found by reflection");
    // Double-precision System.Math entry points the scalar nodes call; each call site brackets with
    // Conv_R8/Conv_R4 since the graph value type is float throughout. Sin/Cos below predate these and
    // use the identical shape.
    private static readonly MethodInfo MathMinMethod =
        typeof(Math).GetMethod(nameof(Math.Min), new[] { typeof(float), typeof(float) })
        ?? throw new InvalidOperationException("System.Math.Min(float,float) was not found by reflection");
    private static readonly MethodInfo MathMaxMethod =
        typeof(Math).GetMethod(nameof(Math.Max), new[] { typeof(float), typeof(float) })
        ?? throw new InvalidOperationException("System.Math.Max(float,float) was not found by reflection");
    private static readonly MethodInfo MathAbsMethod =
        typeof(Math).GetMethod(nameof(Math.Abs), new[] { typeof(float) })
        ?? throw new InvalidOperationException("System.Math.Abs(float) was not found by reflection");
    private static readonly MethodInfo MathSqrtMethod =
        typeof(Math).GetMethod(nameof(Math.Sqrt), new[] { typeof(double) })
        ?? throw new InvalidOperationException("System.Math.Sqrt(double) was not found by reflection");
    private static readonly MethodInfo MathPowMethod =
        typeof(Math).GetMethod(nameof(Math.Pow), new[] { typeof(double), typeof(double) })
        ?? throw new InvalidOperationException("System.Math.Pow(double,double) was not found by reflection");
    private static readonly MethodInfo MathFloorMethod =
        typeof(Math).GetMethod(nameof(Math.Floor), new[] { typeof(double) })
        ?? throw new InvalidOperationException("System.Math.Floor(double) was not found by reflection");
    private static readonly MethodInfo MathCeilMethod =
        typeof(Math).GetMethod(nameof(Math.Ceiling), new[] { typeof(double) })
        ?? throw new InvalidOperationException("System.Math.Ceiling(double) was not found by reflection");
    private static readonly MethodInfo MathRoundMethod =
        typeof(Math).GetMethod(nameof(Math.Round), new[] { typeof(double) })
        ?? throw new InvalidOperationException("System.Math.Round(double) was not found by reflection");
}
