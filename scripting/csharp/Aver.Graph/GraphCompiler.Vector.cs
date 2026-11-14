// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Emission for the vector, string and entity-pin nodes (GraphDataNodes.cs lists them). Pure ones are emitted
// once, in EmitPureDataPull, which both the pull compiler and Compile()'s topological pass reach.

using System;
using System.Linq;
using System.Numerics;
using System.Reflection;
using System.Reflection.Emit;

namespace Aver.Graph;

public partial class GraphCompiler
{
    private static readonly ConstructorInfo Vector3Ctor =
        typeof(Vector3).GetConstructor(new[] { typeof(float), typeof(float), typeof(float) })!;
    private static readonly FieldInfo Vector3X = typeof(Vector3).GetField("X")!;
    private static readonly FieldInfo Vector3Y = typeof(Vector3).GetField("Y")!;
    private static readonly FieldInfo Vector3Z = typeof(Vector3).GetField("Z")!;
    private static readonly MethodInfo Vector3Zero = typeof(Vector3).GetProperty("Zero")!.GetGetMethod()!;

    private static MethodInfo Vec3Op(string name, params Type[] args) =>
        typeof(Vector3).GetMethod(name, args) ?? throw new InvalidOperationException($"Vector3.{name} was not found by reflection");

    private static MethodInfo DataOp(string name) =>
        typeof(GraphDataOps).GetMethod(name) ?? throw new InvalidOperationException($"GraphDataOps.{name} was not found by reflection");

    private static readonly Type V3 = typeof(Vector3);

    /// The zero of a data pin: what an unwired input with no value reads as.
    private void EmitZeroOf(PinType type)
    {
        if (_il == null) return;
        switch (type)
        {
            case PinType.Float3: _il.Emit(OpCodes.Call, Vector3Zero); break;
            case PinType.String: _il.Emit(OpCodes.Ldstr, ""); break;
            case PinType.Bool:
            case PinType.Int:
            case PinType.Entity: _il.Emit(OpCodes.Ldc_I4_0); break;
            default: _il.Emit(OpCodes.Ldc_R4, 0f); break;
        }
    }

    private void EmitVector3(Vector3 v)
    {
        if (_il == null) return;
        _il.Emit(OpCodes.Ldc_R4, v.X);
        _il.Emit(OpCodes.Ldc_R4, v.Y);
        _il.Emit(OpCodes.Ldc_R4, v.Z);
        _il.Emit(OpCodes.Newobj, Vector3Ctor);
    }

    /// One output of a pure vector/string/entity node onto the stack.
    private void EmitPureDataPull(Node node, string pinName)
    {
        if (_il == null) return;
        void In(string p) => EmitPullInput(node, p);
        void Call(MethodInfo m) => _il.Emit(OpCodes.Call, m);

        switch (node.Type.ToLowerInvariant())
        {
            case "constvector":
            {
                var co = _graph.ConstantOutputs.FirstOrDefault(c => c.NodeId == node.Id);
                EmitVector3(co?.Value is Vector3 v ? v : Vector3.Zero);
                return;
            }
            case "conststring":
            {
                var co = _graph.ConstantOutputs.FirstOrDefault(c => c.NodeId == node.Id);
                _il.Emit(OpCodes.Ldstr, co?.Value as string ?? "");
                return;
            }
            case "makevector":
                In("x"); In("y"); In("z"); _il.Emit(OpCodes.Newobj, Vector3Ctor); return;
            case "breakvector":
                In("v");
                _il.Emit(OpCodes.Ldfld, pinName switch
                {
                    "x" => Vector3X,
                    "y" => Vector3Y,
                    "z" => Vector3Z,
                    _ => throw new InvalidOperationException($"BreakVector node '{node.Id}' has no output pin '{pinName}' (x, y, z)"),
                });
                return;
            case "vectoradd": In("a"); In("b"); Call(Vec3Op("Add", V3, V3)); return;
            case "vectorsubtract": In("a"); In("b"); Call(Vec3Op("Subtract", V3, V3)); return;
            case "vectorscale": In("v"); In("s"); Call(Vec3Op("Multiply", V3, typeof(float))); return;
            case "vectordot": In("a"); In("b"); Call(Vec3Op("Dot", V3, V3)); return;
            case "vectorcross": In("a"); In("b"); Call(Vec3Op("Cross", V3, V3)); return;
            case "vectorlength": In("v"); Call(DataOp("Length")); return;
            case "vectornormalize": In("v"); Call(DataOp("Normalize")); return;
            case "vectorlerp": In("a"); In("b"); In("t"); Call(DataOp("Lerp")); return;
            case "vectordistance": In("a"); In("b"); Call(Vec3Op("Distance", V3, V3)); return;
            case "stringappend":
                In("a"); In("b"); Call(typeof(string).GetMethod("Concat", new[] { typeof(string), typeof(string) })!); return;
            case "floattostring": In("a"); Call(DataOp("FloatToString")); return;
            case "inttostring": In("a"); Call(DataOp("IntToString")); return;
            case "booltostring": In("a"); Call(DataOp("BoolToString")); return;
            case "vectortostring": In("v"); Call(DataOp("VectorToString")); return;
            case "stringequals":
                In("a"); In("b"); Call(typeof(string).GetMethod("Equals", new[] { typeof(string), typeof(string) })!); return;
            case "stringlength": In("s"); Call(DataOp("StringLength")); return;
            case "isvalidentity": In("e"); Call(IsAliveMethod); return;
            case "getentitylocation":
            {
                In("e");
                var x = _il.DeclareLocal(typeof(float));
                var y = _il.DeclareLocal(typeof(float));
                var z = _il.DeclareLocal(typeof(float));
                _il.Emit(OpCodes.Ldloca, x);
                _il.Emit(OpCodes.Ldloca, y);
                _il.Emit(OpCodes.Ldloca, z);
                Call(WorldPositionMethod);
                _il.Emit(OpCodes.Pop);
                _il.Emit(OpCodes.Ldloc, x);
                _il.Emit(OpCodes.Ldloc, y);
                _il.Emit(OpCodes.Ldloc, z);
                _il.Emit(OpCodes.Newobj, Vector3Ctor);
                return;
            }
            default:
                throw new InvalidOperationException($"data node '{node.Id}' has type {node.Type}, which EmitPureDataPull does not know");
        }
    }

    /// Runs one exec data node when the exec walk reaches it.
    private void EmitExecDataNode(Node node)
    {
        if (_il == null) return;
        void In(string p) => EmitPullInput(node, p);
        LocalBuilder Vec(string pin)
        {
            In(pin);
            var l = _il.DeclareLocal(typeof(Vector3));
            _il.Emit(OpCodes.Stloc, l);
            return l;
        }
        void Comp(LocalBuilder l, FieldInfo f) { _il.Emit(OpCodes.Ldloc, l); _il.Emit(OpCodes.Ldfld, f); }

        switch (node.Type.ToLowerInvariant())
        {
            case "setentitylocation":
            {
                In("e");
                var loc = Vec("location");
                _il.Emit(OpCodes.Ldloc, loc); _il.Emit(OpCodes.Ldfld, Vector3X);
                _il.Emit(OpCodes.Ldloc, loc); _il.Emit(OpCodes.Ldfld, Vector3Y);
                _il.Emit(OpCodes.Ldloc, loc); _il.Emit(OpCodes.Ldfld, Vector3Z);
                _il.Emit(OpCodes.Call, SetLocalPositionMethod);
                _il.Emit(OpCodes.Pop);
                return;
            }
            case "raycastvector":
            {
                var o = Vec("origin");
                var d = Vec("direction");
                Comp(o, Vector3X); Comp(o, Vector3Y); Comp(o, Vector3Z);
                Comp(d, Vector3X); Comp(d, Vector3Y); Comp(d, Vector3Z);
                In("maxDist");
                var px = _il.DeclareLocal(typeof(float));
                var py = _il.DeclareLocal(typeof(float));
                var pz = _il.DeclareLocal(typeof(float));
                _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "hit", typeof(bool)));
                _il.Emit(OpCodes.Ldloca, GetOrCreateExecLocal(node.Id, "entity", typeof(int)));
                _il.Emit(OpCodes.Ldloca, px);
                _il.Emit(OpCodes.Ldloca, py);
                _il.Emit(OpCodes.Ldloca, pz);
                _il.Emit(OpCodes.Call, RaycastMethod);
                _il.Emit(OpCodes.Ldloc, px);
                _il.Emit(OpCodes.Ldloc, py);
                _il.Emit(OpCodes.Ldloc, pz);
                _il.Emit(OpCodes.Newobj, Vector3Ctor);
                _il.Emit(OpCodes.Stloc, GetOrCreateExecLocal(node.Id, "point", typeof(Vector3)));
                return;
            }
            case "printtext":
                _il.Emit(OpCodes.Ldstr, node.Id);
                In("text");
                _il.Emit(OpCodes.Call, PrintStringMethod);
                return;
            default:
                throw new InvalidOperationException($"data node '{node.Id}' has type {node.Type}, which EmitExecDataNode does not know");
        }
    }
}
