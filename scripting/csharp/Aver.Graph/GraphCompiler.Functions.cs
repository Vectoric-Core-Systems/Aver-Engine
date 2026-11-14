// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// User-defined functions: signatures, bodies, calls and returns.

using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection.Emit;
using Aver.Framework;
using Aver.Scene;

namespace Aver.Graph;

public partial class GraphCompiler
{
    // USER-DEFINED FUNCTIONS

    /// Creates a DynamicMethod for every declared function and emits each body. Idempotent; called at
    /// the top of both Compile() and CompileEntryPoint(), before either creates its own method, since
    /// emitting a body overwrites _il/_pinLocals/_execLocals (EmitFunctionBody still saves/restores
    /// them, since a CallFunc in a body re-enters this class).
    private bool EnsureFunctionsCompiled(out string? err)
    {
        err = null;
        if (_functionsCompiled || _graph.Functions.Count == 0) return true;
        _functionsCompiled = true;   // set FIRST: a failure leaves the partial state, and retrying
                                      // would emit every body a second time into fresh methods.

        // PASS 1 -- every signature, before any body. See _funcMethods.
        foreach (var fn in _graph.Functions)
        {
            Type[] argTypes = fn.Inputs.Select(p => PinTypeToCLRType(p.Type)).ToArray();
            if (_graph.Variables.Count > 0)
                argTypes = argTypes.Append(typeof(GraphVarStore)).ToArray();
            Type ret = fn.Outputs.Count switch
            {
                0 => typeof(void),
                1 => PinTypeToCLRType(fn.Outputs[0].Type),
                // Several outputs box into an object[], same as Compile() for several OUT records --
                // one convention, so a caller unpacking one has already seen how.
                _ => typeof(object[]),
            };
            _funcMethods[fn.Name] = new DynamicMethod($"GraphFunc_{fn.Name}", ret, argTypes, restrictedSkipVisibility: true);
        }

        // PASS 2 -- the bodies.
        foreach (var fn in _graph.Functions)
        {
            if (!EmitFunctionBody(fn, out err)) return false;
        }
        return true;
    }

    private bool EmitFunctionBody(GraphFunction fn, out string? err)
    {
        err = null;
        var dm = _funcMethods[fn.Name];

        // SAVE EVERYTHING: a CallFunc inside this body doesn't re-enter here (bodies emit one at a
        // time in pass 2), but a future caller might. Cheap, and removes a whole class of "why did
        // the event graph get this function's locals" bug before it can exist.
        var savedIl = _il;
        var savedPin = _pinLocals;
        var savedExec = _execLocals;
        var savedNode = _nodeLocals;
        var savedExecVisiting = _execVisiting;
        var savedPullVisiting = _pullVisiting;
        var savedVarStore = _varStoreArgIndex;
        var savedFunc = _currentFunc;
        var savedOutLocals = _funcOutLocals;
        try
        {
            _il = dm.GetILGenerator();
            _pinLocals = new Dictionary<(string, string), LocalBuilder>();
            _execLocals = new Dictionary<(string, string), LocalBuilder>();
            _nodeLocals = new Dictionary<string, LocalBuilder>();
            _execVisiting = new HashSet<string>();
            _pullVisiting = new HashSet<string>();
            _varStoreArgIndex = _graph.Variables.Count > 0 ? fn.Inputs.Count : -1;
            _currentFunc = fn;
            _funcOutLocals = new List<LocalBuilder>();
            foreach (var o in fn.Outputs)
            {
                var outLocal = _il.DeclareLocal(PinTypeToCLRType(o.Type));
                _funcOutLocals.Add(outLocal);
                if (o.Type is PinType.String or PinType.Struct || PinTypeRules.IsArray(o.Type))
                {
                    EmitZeroOf(o.Type, o.StructName);
                    _il.Emit(OpCodes.Stloc, outLocal);
                }
            }

            // Recursion guard as a helper call, not emitted IL: an unbounded recursive function would
            // overflow the CLR stack (uncatchable StackOverflowException, taking the whole editor
            // down), so this counts depth in managed code and throws a catchable exception instead.
            // Not a try/finally in IL either -- that would spread verifiability rules into every
            // branch/loop emitter for no real gain, since GraphCallGuard resets to zero at the start
            // of every top-level invocation anyway, so a leaked depth (from an exception unwinding
            // past an Exit) can't accumulate.
            _il.Emit(OpCodes.Ldstr, fn.Name);
            _il.Emit(OpCodes.Call, GraphCallGuardEnter);

            if (fn.IsPure)
            {
                // No exec chain to walk: pull each declared output straight through the FuncReturn's
                // matching input pin, which recursively evaluates the body's data graph.
                if (fn.Outputs.Count > 0)
                {
                    if (fn.ReturnNodeId == null || !_graph.Nodes.TryGetValue(fn.ReturnNodeId, out var rn))
                    {
                        err = $"function '{fn.Name}' has outputs but no FuncReturn node";
                        return false;
                    }
                    for (int i = 0; i < fn.Outputs.Count; i++)
                    {
                        EmitPullInput(rn, fn.Outputs[i].Name);
                        _il.Emit(OpCodes.Stloc, _funcOutLocals[i]);
                    }
                }
            }
            else
            {
                if (fn.EntryNodeId == null || !_graph.Nodes.TryGetValue(fn.EntryNodeId, out var en))
                {
                    err = $"function '{fn.Name}' has no FuncEntry node";
                    return false;
                }
                EmitExecNode(en);
            }

            _il.Emit(OpCodes.Call, GraphCallGuardExit);

            // One Ret, reached after the whole body runs -- why a function has exactly one FuncReturn
            // (Validate refuses a second): it stores into these locals when reached, not returns, so
            // every branch inside the body converges here.
            if (fn.Outputs.Count == 1)
            {
                _il.Emit(OpCodes.Ldloc, _funcOutLocals[0]);
            }
            else if (fn.Outputs.Count > 1)
            {
                _il.Emit(OpCodes.Ldc_I4, fn.Outputs.Count);
                _il.Emit(OpCodes.Newarr, typeof(object));
                for (int i = 0; i < fn.Outputs.Count; i++)
                {
                    _il.Emit(OpCodes.Dup);
                    _il.Emit(OpCodes.Ldc_I4, i);
                    _il.Emit(OpCodes.Ldloc, _funcOutLocals[i]);
                    _il.Emit(OpCodes.Box, _funcOutLocals[i].LocalType!);
                    _il.Emit(OpCodes.Stelem_Ref);
                }
            }
            _il.Emit(OpCodes.Ret);
            return true;
        }
        finally
        {
            _il = savedIl;
            _pinLocals = savedPin;
            _execLocals = savedExec;
            _nodeLocals = savedNode;
            _execVisiting = savedExecVisiting;
            _pullVisiting = savedPullVisiting;
            _varStoreArgIndex = savedVarStore;
            _currentFunc = savedFunc;
            _funcOutLocals = savedOutLocals;
        }
    }

    /// Loads a CallFunc node's arguments and emits the direct IL Call, leaving the callee's return
    /// (or nothing, or object[]) on the stack -- callers decide what to do with it: a pure pull wants
    /// ONE value, an exec call wants every output stored into its own local.
    private GraphFunction? EmitCallFuncInvoke(Node node)
    {
        if (_il == null) return null;
        var fn = _graph.Functions.FirstOrDefault(f => string.Equals(f.Name, node.CallTarget, StringComparison.OrdinalIgnoreCase));
        if (fn == null || !_funcMethods.TryGetValue(fn.Name, out var dm)) return null;
        foreach (var p in fn.Inputs) EmitPullInput(node, p.Name);
        // The variable store rides through every call (loaded from the caller's own store argument),
        // so a function can read/write the same per-instance VARs its caller can -- present on every
        // function in a VAR-bearing graph whether that function touches one or not -- a uniform
        // signature that costs one argument and removes a conditional.
        if (_graph.Variables.Count > 0 && _varStoreArgIndex >= 0)
            _il.Emit(OpCodes.Ldarg, (short)_varStoreArgIndex);
        _il.Emit(OpCodes.Call, dm);
        return fn;
    }

    /// A CallFunc reached by the exec walk: call once, store every output into its own exec local, so
    /// reading two of a function's outputs downstream does not call it twice.
    private void EmitExecCallFunc(Node node)
    {
        if (_il == null) return;
        var fn = EmitCallFuncInvoke(node);
        if (fn == null) return;
        if (fn.Outputs.Count == 0) return;
        if (fn.Outputs.Count == 1)
        {
            _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, fn.Outputs[0].Name, PinTypeToCLRType(fn.Outputs[0].Type)));
            return;
        }
        // object[] on the stack: keep it in a local, then unbox one element per output pin.
        var arr = _il.DeclareLocal(typeof(object[]));
        _il.Emit(OpCodes.Stloc, arr);
        for (int i = 0; i < fn.Outputs.Count; i++)
        {
            Type t = PinTypeToCLRType(fn.Outputs[i].Type);
            _il.Emit(OpCodes.Ldloc, arr);
            _il.Emit(OpCodes.Ldc_I4, i);
            _il.Emit(OpCodes.Ldelem_Ref);
            _il.Emit(OpCodes.Unbox_Any, t);
            _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, fn.Outputs[i].Name, t));
        }
    }

    /// A CallFunc pulled as data -- only legal for a PURE function: safe to evaluate whenever and as
    /// often as a reader asks. Leaves the ONE requested pin's value on the stack.
    private void EmitPullCallFunc(Node node, string pinName)
    {
        if (_il == null) return;
        var fn = EmitCallFuncInvoke(node);
        if (fn == null) throw new InvalidOperationException(
            $"CallFunc node '{node.Id}' calls '{node.CallTarget}', which is not a declared function");
        int idx = fn.Outputs.FindIndex(o => o.Name == pinName);
        if (idx < 0) throw new InvalidOperationException(
            $"function '{fn.Name}' has no output named '{pinName}'");
        if (fn.Outputs.Count == 1) return;   // already the value on the stack
        Type t = PinTypeToCLRType(fn.Outputs[idx].Type);
        _il.Emit(OpCodes.Ldc_I4, idx);
        _il.Emit(OpCodes.Ldelem_Ref);
        _il.Emit(OpCodes.Unbox_Any, t);
    }

    /// A CallFunc met by the PULL compiler's topological pass -- a call in a graph with no ENTRY
    /// records, the shape every pre-exec .ocgraph in this repo still has. An impure function is refused here, same reason as Spawn/CharacterMove/FireEvent/
    /// SetVar: the topological pass runs every node exactly once with no branch to gate a side
    /// effect. Declaring a function `pure` is the promise that this is safe.
    private void EmitCallFuncTopological(Node node)
    {
        if (_il == null) return;
        var fn = _graph.Functions.FirstOrDefault(f => string.Equals(f.Name, node.CallTarget, StringComparison.OrdinalIgnoreCase));
        if (fn == null)
            throw new InvalidOperationException(
                $"CallFunc node '{node.Id}' calls '{node.CallTarget}', which is not a declared function");
        if (!fn.IsPure)
            throw new InvalidOperationException(
                $"CallFunc node '{node.Id}' calls '{fn.Name}', which is not pure, and Compile() has no exec " +
                "chain to run it from -- a pure-dataflow graph has no notion of WHEN to run a side effect. " +
                "Either declare the function pure (FUNC " + fn.Name + " pure), or give this graph an ENTRY " +
                "record and wire the call into its exec chain so CompileEntryPoint() runs it.");

        // One call, results into this node's pin locals -- same "call once, store every output"
        // shape as EmitExecCallFunc, but against _pinLocals since that's what LoadPin reads.
        var invoked = EmitCallFuncInvoke(node);
        if (invoked == null) return;
        if (fn.Outputs.Count == 0) return;
        if (fn.Outputs.Count == 1)
        {
            _il.Emit(OpCodes.Stloc, RequirePinLocal(node, fn.Outputs[0].Name));
            return;
        }
        var arr = _il.DeclareLocal(typeof(object[]));
        _il.Emit(OpCodes.Stloc, arr);
        for (int i = 0; i < fn.Outputs.Count; i++)
        {
            _il.Emit(OpCodes.Ldloc, arr);
            _il.Emit(OpCodes.Ldc_I4, i);
            _il.Emit(OpCodes.Ldelem_Ref);
            _il.Emit(OpCodes.Unbox_Any, PinTypeToCLRType(fn.Outputs[i].Type));
            _il.Emit(OpCodes.Stloc, RequirePinLocal(node, fn.Outputs[i].Name));
        }
    }

    /// A FuncReturn reached by the exec walk: pulls each declared output into the enclosing
    /// function's output local. Does NOT return -- see EmitFunctionBody for the single shared Ret.
    private void EmitFuncReturnStores(Node node)
    {
        if (_il == null || _currentFunc == null) return;
        for (int i = 0; i < _currentFunc.Outputs.Count && i < _funcOutLocals.Count; i++)
        {
            EmitPullInput(node, _currentFunc.Outputs[i].Name);
            _il.Emit(OpCodes.Stloc, _funcOutLocals[i]);
        }
    }

    /// A FuncEntry's output pin IS an argument of the enclosing method (same shape as EmitParam for a
    /// graph-level PARAM). The index is the pin's position in the function's input list, so a FUNCIN
    /// reordered in the file reorders the arguments and loads together.
    private void EmitFuncEntryLoad(string pinName)
    {
        if (_il == null || _currentFunc == null) return;
        int index = _currentFunc.Inputs.FindIndex(p => p.Name == pinName);
        if (index < 0)
            throw new InvalidOperationException(
                $"function '{_currentFunc.Name}' has no input named '{pinName}' -- add 'FUNCIN {_currentFunc.Name} {pinName} <type>'");
        _il.Emit(OpCodes.Ldarg, (short)index);
    }

    /// A CallFunc reached by the exec walk. Purity doesn't decide this -- a PURE function can be
    /// called from an exec chain too (it just has no exec pins to wire); this only asks "is it a call".
    private static bool IsExecCapableCallFuncType(string type) =>
        type.Equals("callfunc", StringComparison.OrdinalIgnoreCase);

    private static bool IsExecCapableFuncReturnType(string type) =>
        type.Equals("funcreturn", StringComparison.OrdinalIgnoreCase);
}
