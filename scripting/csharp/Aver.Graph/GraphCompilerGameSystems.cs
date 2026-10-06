// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The one emitter behind every node in GameSystemNodes (GraphGameSystemNodes.cs): it pushes the
// method's arguments as the row's tokens say, calls it, and routes the return value and out
// parameters to the node's output pins.

using System.Reflection;
using System.Reflection.Emit;

namespace Aver.Graph;

public partial class GraphCompiler
{
    private static InvalidOperationException GenericPushOnly(Node node) => new(
        $"{node.Type} node '{node.Id}' cannot be compiled by Compile() -- it has a side effect, and a " +
        "pure-dataflow graph has no notion of WHEN to run one: Compile()'s topological pass would run it " +
        "unconditionally on every invocation, with no branch structure available to gate it. The node " +
        "type is supported; this compiler is not the one that runs it. Give the node an ENTRY-driven " +
        "exec chain and reach it through CompileEntryPoint() instead.");

    /// <summary>The exec walk reached a table node: call it, then the caller fans out its exec outputs.</summary>
    private void EmitExecGeneric(Node node)
    {
        if (_il == null) return;
        var spec = GameSystemNodes.TryGet(node.Type, out var s) ? s : throw new InvalidOperationException(node.Type);
        EmitGenericInvoke(node, spec, pin => EmitPullInput(node, pin),
                          (pin, type) => PinOf(node, pin) == null ? null : GetOrCreateExecLocal(node.Id, pin, type));
    }

    /// <summary>The topological pass reached a pure table node: call it once, filling its pin locals.</summary>
    private void EmitGenericPureTopological(Node node, GenericNodeSpec spec)
    {
        if (_il == null) return;
        EmitGenericInvoke(node, spec, pin => LoadPin(node.Id, pin),
                          (pin, _) => _pinLocals.TryGetValue((node.Id, pin), out var l) ? l : null);
    }

    /// <summary>A reader pulled output <paramref name="pinName"/> of a pure table node: recompute and push it.</summary>
    private void EmitGenericPurePull(Node source, GenericNodeSpec spec, string pinName)
    {
        if (_il == null) return;
        if (!spec.HasOutput(pinName))
            throw new InvalidOperationException($"{source.Type} node '{source.Id}' has no output pin '{pinName}'");
        var temps = new Dictionary<string, LocalBuilder>();
        EmitGenericInvoke(source, spec, pin => EmitPullInput(source, pin), (pin, type) =>
        {
            if (!temps.TryGetValue(pin, out var l)) temps[pin] = l = _il!.DeclareLocal(type);
            return l;
        });
        if (temps.TryGetValue(pinName, out var local)) _il!.Emit(OpCodes.Ldloc, local);
        else throw new InvalidOperationException($"{source.Type} node '{source.Id}': pin '{pinName}' is not produced by {spec.MethodName}");
    }

    private static Pin? PinOf(Node node, string name) => node.Pins.FirstOrDefault(p => p.IsOutput && p.Name == name);

    private void EmitGenericInvoke(Node node, GenericNodeSpec spec, Action<string> loadInput,
                                   Func<string, Type, LocalBuilder?> destination)
    {
        var il = _il!;
        MethodInfo method = spec.Method;
        var ps = method.GetParameters();
        if (ps.Length != spec.Args.Length)
            throw new InvalidOperationException(
                $"{node.Type}: {spec.MethodName} takes {ps.Length} parameters but the node table feeds {spec.Args.Length}");

        var outs = new List<(string Pin, LocalBuilder Temp)>();
        for (int i = 0; i < ps.Length; i++)
        {
            string a = spec.Args[i];
            if (a.StartsWith("p:", StringComparison.Ordinal))
            {
                loadInput(a[2..]);
            }
            else if (a.StartsWith('@'))
            {
                il.Emit(OpCodes.Ldstr, AttributeFor(node, a[1..]));
            }
            else if (a.StartsWith('='))
            {
                il.Emit(OpCodes.Ldstr, a[1..]);
            }
            else if (a.StartsWith("i:", StringComparison.Ordinal))
            {
                il.Emit(OpCodes.Ldc_I4, int.Parse(a[2..], System.Globalization.CultureInfo.InvariantCulture));
            }
            else if (a.StartsWith("f:", StringComparison.Ordinal))
            {
                il.Emit(OpCodes.Ldc_R4, float.Parse(a[2..], System.Globalization.CultureInfo.InvariantCulture));
            }
            else if (a.StartsWith("b:", StringComparison.Ordinal))
            {
                il.Emit(a[2..] == "0" ? OpCodes.Ldc_I4_0 : OpCodes.Ldc_I4_1);
            }
            else if (a.StartsWith('>'))
            {
                var tmp = il.DeclareLocal(ps[i].ParameterType.GetElementType()!);
                il.Emit(OpCodes.Ldloca, tmp);
                if (a != ">_") outs.Add((a[1..], tmp));
            }
            else
            {
                throw new InvalidOperationException($"{node.Type}: argument token '{a}' is not understood");
            }
        }

        il.Emit(OpCodes.Call, method);

        if (method.ReturnType != typeof(void))
        {
            LocalBuilder? dest = spec.Ret == null ? null : destination(spec.Ret, method.ReturnType);
            if (dest != null) il.Emit(OpCodes.Stloc, dest); else il.Emit(OpCodes.Pop);
        }

        foreach (var (pin, temp) in outs)
        {
            var dest = destination(pin, temp.LocalType);
            if (dest == null) continue;
            il.Emit(OpCodes.Ldloc, temp);
            il.Emit(OpCodes.Stloc, dest);
        }
    }

    /// <summary>The string a table node takes from the NODE line: first of the alternatives present.
    /// A trailing '?' makes it optional (empty when absent); otherwise a missing one fails the compile,
    /// since an empty key, event or path could never do anything.</summary>
    private static string AttributeFor(Node node, string spec)
    {
        bool optional = spec.EndsWith('?');
        if (optional) spec = spec[..^1];
        string[] keys = spec.Split('|');
        foreach (var k in keys)
            if (node.Attrs.TryGetValue(k, out var v) && !string.IsNullOrEmpty(v)) return v;
        if (optional) return string.Empty;
        throw new InvalidOperationException(
            $"{node.Type} node '{node.Id}' has no {string.Join("= / ", keys)}= attribute");
    }
}
