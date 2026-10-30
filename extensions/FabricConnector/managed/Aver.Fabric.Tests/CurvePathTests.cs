// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// G5 A2, the parts with no engine behind them: Expression, Curve / Curve3D / Gradient against reference values, Path3D and PathFollow3D, the blend-space
// geometry, Skeleton3D and its followers and modifiers, SpringArm3D, RemoteTransform3D and the on-screen notifiers.

using Aver.Fabric;
using Collections = Aver.Fabric.Collections;

class RaiseModifier : SkeletonModifier3D
{
    public int Runs;
    public override void _ProcessModification()
    {
        Runs++;
        GetSkeleton()!.SetBonePosePosition(0, new Vector3(0, 10, 0));
    }
}

class ExprBase : Node
{
    public float Speed = 2f;
    public bool Grounded = true;
    public string Mode = "walk";
    public bool IsFast() => Speed > 5f;
    public int Twice(int n) => n * 2;
}

static class CurvePathTests
{
    public static int RunAll()
    {
        return T.Suite(() =>
        {
            T.Run("Expression: literals, operators, inputs, the base object, functions, errors", ExpressionEval);
            T.Run("Curve: cubic segments against reference values, tangent modes, domain, baked table, scene data", CurveSampling);
            T.Run("Curve3D: baked lengths, sampling, closest point, tilt and up vectors, rotation, closed curves, scene data", Curve3DBaking);
            T.Run("Gradient: linear, constant and cubic sampling, ordering", GradientSampling);
            T.Run("PathFollow3D: progress, ratio, loop, offsets, the rotation modes, curve changes", PathFollowing);
            T.Run("blend-space geometry: barycentric weights, edge snapping, Delaunay triangles", BlendSpaceGeometry);
            T.Run("Skeleton3D: the bone list, rests and poses, global poses, overrides, enabling, scene values", SkeletonApi);
            T.Run("SkeletonModifier3D, SkeletonIK3D and BoneAttachment3D", Modifiers);
            T.Run("SpringArm3D: the children are pulled in to the hit, with the margin, along +Z", SpringArm);
            T.Run("RemoteTransform3D: global and local, each component switchable", RemoteTransform);
            T.Run("VisibleOnScreenNotifier3D and Enabler3D: enter and exit from the camera's frustum", ScreenNotifiers);
        });
    }

    // ---- Expression ----

    private static void ExpressionEval()
    {
        var e = new Expression();
        T.Check("parse ok", e.Parse("1 + 2 * 3") == Error.Ok);
        T.Check("1 + 2 * 3 = 7 (precedence)", e.Execute().AsInt64() == 7);
        e.Parse("(1 + 2) * 3 - 4 / 2");
        T.Check("parentheses and mixed int division: 7 (4/2 is an int division in Godot, 2)", e.Execute().AsInt64() == 7);
        e.Parse("7 / 2.0");
        T.CheckNum("a float operand makes a float division", 3.5f, e.Execute().AsSingle());
        e.Parse("2 ** 3 ** 2");
        T.Check("** is right-associative: 512", e.Execute().AsInt64() == 512);
        e.Parse("-2 ** 2");
        T.Check("unary minus binds looser than ** : -4", e.Execute().AsInt64() == -4);
        e.Parse("10 % 4 == 2 && 3 >= 3 and not (1 > 2)");
        T.Check("modulo, comparisons, && / and / not", e.Execute().AsBool());
        e.Parse("false || 1 < 2 or false");
        T.Check("|| / or", e.Execute().AsBool());
        e.Parse("!true");
        T.Check("!", !e.Execute().AsBool());
        e.Parse("\"ab\" + 'cd' == \"abcd\"");
        T.Check("strings and ==", e.Execute().AsBool());
        e.Parse("3 in [1, 2, 3]");
        T.Check("in over an array literal", e.Execute().AsBool());
        e.Parse("[10, 20, 30][1] + 0x10 + 1e1");
        T.CheckNum("indexing, hex and exponent literals: 20 + 16 + 10", 46f, e.Execute().AsSingle());
        e.Parse("1 != 2 and 2 != 2");
        T.Check("!=", !e.Execute().AsBool());

        e.Parse("speed * 2 > limit", new[] { "speed", "limit" });
        T.Check("named inputs", e.Execute(new Collections.Array { 4, 7 }).AsBool() && !e.Execute(new Collections.Array { 3, 7 }).AsBool());

        var b = new ExprBase();
        e.Parse("Speed > 1 and Grounded and Mode == \"walk\"");
        T.Check("identifiers read the base object's members", e.Execute(null, b).AsBool());
        b.Speed = 0.5f;
        T.Check("and see changes", !e.Execute(null, b).AsBool());
        e.Parse("speed < 1.0");
        T.Check("snake_case names find PascalCase members", e.Execute(null, b).AsBool());
        e.Parse("IsFast() or Twice(4) == 8");
        T.Check("method calls on the base object", e.Execute(null, b).AsBool());
        e.Parse("max(2, 5) + abs(-3) + clamp(15, 0, 10) + floor(2.7)");
        T.CheckNum("built-in functions: 5 + 3 + 10 + 2", 20f, e.Execute().AsSingle());
        e.Parse("Vector2(3, 4).x + Vector3(1, 2, 3).z");
        T.CheckNum("vector constructors and members", 6f, e.Execute().AsSingle());
        e.Parse("str(\"a\", 1) == \"a1\"");
        T.Check("str()", e.Execute().AsBool());

        T.Check("a parse error is reported", new Expression().Parse("1 +") != Error.Ok);
        var bad = new Expression();
        bad.Parse("1 + ", null);
        bad.Execute();
        T.Check("executing an expression that did not parse fails", bad.HasExecuteFailed() && bad.GetErrorText().Length > 0);
        e.Parse("nope + 1");
        e.Execute(null, b, false);
        T.Check("an unknown name fails at run time with a message", e.HasExecuteFailed() && e.GetErrorText().Contains("nope"));
        e.Parse("nope() + 1");
        e.Execute(null, b, false);
        T.Check("an unknown function fails", e.HasExecuteFailed());
        e.Parse("1/0");
        e.Execute(null, null, false);
        T.Check("integer division by zero fails rather than crashing", e.HasExecuteFailed());
    }

    // ---- Curve ----

    private static void CurveSampling()
    {
        var c = new Curve();
        c.AddPoint(new Vector2(0, 0));
        c.AddPoint(new Vector2(1, 1));
        T.Check("two points", c.PointCount == 2);
        T.CheckNum("flat tangents give a smoothstep: 0.5", 0.5f, c.Sample(0.5f));
        T.CheckNum("smoothstep(0.25) = 0.15625", 0.15625f, c.Sample(0.25f), 1e-5f);
        T.CheckNum("before the first point it holds the first value", 0f, c.Sample(-3f));
        T.CheckNum("after the last it holds the last value", 1f, c.Sample(4f));

        var lin = new Curve();
        lin.AddPoint(new Vector2(0, 0), 0, 1);
        lin.AddPoint(new Vector2(1, 1), 1, 0);
        T.CheckNum("tangents of 1 (the slope) make a straight line: 0.3", 0.3f, lin.Sample(0.3f), 1e-5f);
        T.CheckNum("and 0.9", 0.9f, lin.Sample(0.9f), 1e-5f);

        var m = new Curve();
        m.AddPoint(new Vector2(0, 0), 0, 0, Curve.TangentMode.Linear, Curve.TangentMode.Linear);
        m.AddPoint(new Vector2(0.5f, 1), 0, 0, Curve.TangentMode.Linear, Curve.TangentMode.Linear);
        m.AddPoint(new Vector2(1, 0), 0, 0, Curve.TangentMode.Linear, Curve.TangentMode.Linear);
        T.CheckNum("Linear tangent mode follows the neighbours: the right tangent of point 0 is the slope 2", 2f, m.GetPointRightTangent(0));
        T.CheckNum("and the left tangent of point 2 is -2", -2f, m.GetPointLeftTangent(2));
        T.CheckNum("so the rising half is a line: 0.5 at 0.25", 0.5f, m.Sample(0.25f), 1e-5f);
        T.CheckNum("and the falling half: 0.5 at 0.75", 0.5f, m.Sample(0.75f), 1e-5f);
        T.CheckNum("the peak", 1f, m.Sample(0.5f), 1e-5f);

        int idx = m.AddPoint(new Vector2(0.25f, 5));
        T.Check("add_point keeps the points ordered by X and returns the index", idx == 1 && m.GetPointPosition(1).X == 0.25f && m.PointCount == 4);
        m.SetPointValue(1, 0.5f);
        T.CheckNum("set_point_value", 0.5f, m.GetPointPosition(1).Y);
        int moved = m.SetPointOffset(1, 0.9f);
        T.Check("set_point_offset re-sorts and returns the new index", moved == 2 && m.GetPointPosition(2).X == 0.9f && m.GetPointPosition(1).X == 0.5f);
        m.RemovePoint(2);
        T.Check("remove_point", m.PointCount == 3);

        var d = new Curve { MinDomain = 0, MaxDomain = 10 };
        d.AddPoint(new Vector2(0, 0), 0, 0.1f);
        d.AddPoint(new Vector2(10, 1), 0.1f, 0);
        T.CheckNum("a wider domain: the line from (0,0) to (10,1) at 5", 0.5f, d.Sample(5f), 1e-5f);

        // The baked table follows the curve.
        var s = new Curve();
        s.AddPoint(new Vector2(0, 0));
        s.AddPoint(new Vector2(1, 1));
        float worst = 0;
        for (int i = 0; i <= 200; i++) worst = MathF.Max(worst, MathF.Abs(s.Sample(i / 200f) - s.SampleBaked(i / 200f)));
        T.Check("the baked table stays within 0.001 of the curve", worst < 1e-3f, $"worst {worst}");
        T.CheckNum("the baked ends are exact", 1f, s.SampleBaked(1f));
        s.AddPoint(new Vector2(0.5f, 3));
        T.Check("changing the points rebuilds the table", MathF.Abs(s.SampleBaked(0.5f) - 3f) < 0.05f);

        // Scene data: position, left tangent, right tangent, left mode, right mode.
        var sd = new Curve();
        sd.Data = new Collections.Array { new Vector2(0, 0), 0f, 0f, 0, 0, new Vector2(1, 1), 2f, 0f, 0, 0 };
        T.Check("_data builds the points", sd.PointCount == 2 && T.Near(sd.GetPointLeftTangent(1), 2f));
        T.Check("and reads back", sd.Data.Count == 10 && sd.Data[5].AsVector2() == new Vector2(1, 1));
        T.Check("an empty curve answers 0", new Curve().Sample(0.3f) == 0f && new Curve().SampleBaked(0.3f) == 0f);
    }

    // ---- Curve3D ----

    private static Curve3D Line(float length)
    {
        var c = new Curve3D();
        c.AddPoint(new Vector3(0, 0, 0));
        c.AddPoint(new Vector3(length, 0, 0));
        return c;
    }

    private static void Curve3DBaking()
    {
        Curve3D line = Line(10f);
        T.CheckNum("a straight curve is as long as its points are apart (baked length)", 10f, line.GetBakedLength(), 1e-3f);
        Vector3[] pts = line.GetBakedPoints();
        float maxGap = 0;
        for (int i = 1; i < pts.Length; i++) maxGap = MathF.Max(maxGap, pts[i].DistanceTo(pts[i - 1]));
        T.Check("the baked points are about bake_interval (0.2) apart and cover the curve", pts.Length >= 50 && pts.Length <= 53 && maxGap < 0.2f + 5e-3f, $"{pts.Length} points, gap {maxGap}");
        T.CheckVec("sample_baked at the middle", new Vector3(5, 0, 0), line.SampleBaked(5f), 2e-3f);
        T.CheckVec("before the start it gives the start", Vector3.Zero, line.SampleBaked(-4f));
        T.CheckVec("past the end it gives the end", new Vector3(10, 0, 0), line.SampleBaked(99f), 1e-3f);
        T.CheckVec("closest point to (3, 4, 0)", new Vector3(3, 0, 0), line.GetClosestPoint(new Vector3(3, 4, 0)), 2e-3f);
        T.CheckNum("closest offset", 3f, line.GetClosestOffset(new Vector3(3, 4, 0)), 2e-3f);
        T.CheckVec("closest point clamps past the end", new Vector3(10, 0, 0), line.GetClosestPoint(new Vector3(20, 1, 0)), 1e-3f);
        T.CheckVec("sample(0, 0.5) on the bezier (zero handles: the middle)", new Vector3(5, 0, 0), line.Sample(0, 0.5f), 1e-4f);
        T.CheckVec("samplef(0.25)", new Vector3(10 * (3 * 0.0625f - 2 * 0.015625f), 0, 0), line.Samplef(0.25f), 1e-4f);

        // A quarter circle of radius 10 from four-third-rule handles: arc length pi/2 * 10.
        var arc = new Curve3D();
        const float k = 5.5228475f;
        arc.AddPoint(new Vector3(10, 0, 0), default, new Vector3(0, k, 0));
        arc.AddPoint(new Vector3(0, 10, 0), new Vector3(k, 0, 0), default);
        T.CheckNum("a quarter circle: baked length ~ 15.708", 15.708f, arc.GetBakedLength(), 0.03f);
        Vector3 mid = arc.SampleBaked(arc.GetBakedLength() * 0.5f);
        T.Check("its middle is on the circle (10 from the origin) at 45 degrees", MathF.Abs(mid.Length() - 10f) < 0.02f && MathF.Abs(mid.X - mid.Y) < 0.02f, mid.ToString());
        Vector3[] tess = arc.Tessellate(5, 4f);
        T.Check("tessellate keeps both ends and adds points where it bends", tess.Length > 4 && tess[0] == new Vector3(10, 0, 0) && T.Near(tess[^1], new Vector3(0, 10, 0)));
        Vector3[] even = arc.TessellateEvenLength(5, 1f);
        float step = 0;
        for (int i = 1; i < even.Length - 1; i++) step = MathF.Max(step, even[i].DistanceTo(even[i - 1]));
        T.Check("tessellate_even_length makes pieces of about the length asked for", even.Length >= 14 && step < 1.2f, $"{even.Length} points, step {step}");

        // Tilt, up vector, rotation.
        Curve3D tilted = Line(10f);
        tilted.SetPointTilt(1, MathF.PI / 2);
        T.CheckVec("the up vector of a +X path starts as +Y", Vector3.Up, tilted.SampleBakedUpVector(0f), 1e-4f);
        T.CheckVec("tilt is ignored unless asked for", Vector3.Up, tilted.SampleBakedUpVector(10f), 1e-4f);
        T.CheckVec("at the end a quarter-turn tilt rolls it to +Z (right-handed about the direction of travel)", new Vector3(0, 0, 1), tilted.SampleBakedUpVector(10f, true), 1e-3f);
        T.CheckNum("baked tilts interpolate along the way", MathF.PI / 4, tilted.GetBakedTilts()[tilted.GetBakedTilts().Length / 2], 0.02f);
        Transform3D t = Line(10f).SampleBakedWithRotation(4f);
        T.CheckVec("sample_baked_with_rotation: origin", new Vector3(4, 0, 0), t.Origin, 2e-3f);
        T.CheckVec("its -Z points along the path (+X)", new Vector3(-1, 0, 0), t.Basis.Z, 1e-3f);
        T.CheckVec("its Y is the up vector", Vector3.Up, t.Basis.Y, 1e-3f);
        T.CheckVec("and X is to the right of travel (+Z)", new Vector3(0, 0, 1), t.Basis.X, 1e-3f);
        var vertical = new Curve3D();
        vertical.AddPoint(Vector3.Zero);
        vertical.AddPoint(new Vector3(0, 5, 0));
        Vector3 vup = vertical.SampleBakedUpVector(2f);
        T.Check("a path straight up still has a unit up vector perpendicular to it", MathF.Abs(vup.Length() - 1f) < 1e-3f && MathF.Abs(vup.Dot(Vector3.Up)) < 1e-3f, vup.ToString());

        // Closed.
        var square = new Curve3D();
        foreach (Vector3 p in new[] { new Vector3(0, 0, 0), new Vector3(10, 0, 0), new Vector3(10, 0, 10), new Vector3(0, 0, 10) }) square.AddPoint(p);
        T.CheckNum("open: three sides", 30f, square.GetBakedLength(), 5e-3f);
        square.Closed = true;
        T.CheckNum("closed: the closing side counts", 40f, square.GetBakedLength(), 5e-3f);
        T.CheckVec("and a point on it", new Vector3(0, 0, 5), square.SampleBaked(35f), 5e-3f);

        // Edits and scene data.
        var e = new Curve3D();
        e.AddPoint(new Vector3(1, 1, 1), new Vector3(0, -1, 0), new Vector3(0, 1, 0));
        e.AddPoint(new Vector3(5, 1, 1), index: 0);
        T.Check("add_point at an index inserts", e.PointCount == 2 && e.GetPointPosition(0) == new Vector3(5, 1, 1) && e.GetPointOut(1) == new Vector3(0, 1, 0));
        e.SetPointIn(0, new Vector3(1, 2, 3)); e.SetPointOut(0, new Vector3(4, 5, 6)); e.SetPointTilt(0, 0.5f); e.SetPointPosition(0, new Vector3(7, 8, 9));
        T.Check("point setters", e.GetPointIn(0) == new Vector3(1, 2, 3) && e.GetPointOut(0) == new Vector3(4, 5, 6) && T.Near(e.GetPointTilt(0), 0.5f) && e.GetPointPosition(0) == new Vector3(7, 8, 9));
        Collections.Dictionary data = e.Data;
        var fresh = new Curve3D { Data = data };
        T.Check("_data round-trips (in, out, position per point, and the tilts)", fresh.PointCount == 2 && fresh.GetPointIn(0) == new Vector3(1, 2, 3) && fresh.GetPointOut(0) == new Vector3(4, 5, 6) &&
                fresh.GetPointPosition(0) == new Vector3(7, 8, 9) && T.Near(fresh.GetPointTilt(0), 0.5f));
        e.RemovePoint(0);
        T.Check("remove_point", e.PointCount == 1 && e.GetBakedLength() == 0 && e.GetBakedPoints().Length == 1);
        e.ClearPoints();
        T.Check("clear_points, and an empty curve has nothing baked", e.PointCount == 0 && e.GetBakedPoints().Length == 0 && e.SampleBaked(1f) == Vector3.Zero);
    }

    // ---- Gradient ----

    private static void GradientSampling()
    {
        var g = new Gradient();
        T.Check("a new gradient is black to white", g.PointCount == 2 && g.GetColor(0) == new Color(0, 0, 0, 1) && g.GetColor(1) == new Color(1, 1, 1, 1));
        Color mid = g.Sample(0.5f);
        T.Check("linear: the middle is grey", T.Near(mid.R, 0.5f) && T.Near(mid.G, 0.5f) && T.Near(mid.B, 0.5f) && T.Near(mid.A, 1f));
        g.AddPoint(0.5f, new Color(1, 0, 0, 1));
        T.Check("add_point keeps the order", g.PointCount == 3 && T.Near(g.GetOffset(1), 0.5f) && g.GetColor(1) == new Color(1, 0, 0, 1));
        Color q = g.Sample(0.75f);
        T.Check("linear between red and white at 0.75: (1, .5, .5)", T.Near(q.R, 1f) && T.Near(q.G, 0.5f) && T.Near(q.B, 0.5f));
        T.Check("before the first and after the last points it holds their colours", g.Sample(-1f) == new Color(0, 0, 0, 1) && g.Sample(2f) == new Color(1, 1, 1, 1));
        g.InterpolationMode = Gradient.InterpolationModeEnum.Constant;
        T.Check("constant: the colour of the point at or before the offset", g.Sample(0.4f) == new Color(0, 0, 0, 1) && g.Sample(0.6f) == new Color(1, 0, 0, 1));
        g.InterpolationMode = Gradient.InterpolationModeEnum.Cubic;
        T.Check("cubic passes through the points", T.Near(g.Sample(0.5f).R, 1f, 1e-3f) && T.Near(g.Sample(0.5f).G, 0f, 1e-3f));
        T.Check("and is smooth between them (differs from the line, stays near it)", MathF.Abs(g.Sample(0.25f).R - 0.5f) < 0.2f);
        g.SetOffset(1, 0.9f);
        T.Check("set_offset re-sorts", T.Near(g.GetOffset(1), 0.9f));
        g.RemovePoint(1);
        T.Check("remove_point (never the last)", g.PointCount == 2);
        g.Offsets = new[] { 0.8f, 0.2f };
        g.Colors = new[] { new Color(1, 1, 0, 1), new Color(0, 1, 1, 1) };
        T.Check("offsets and colours as scenes set them, sorted by offset", g.PointCount == 2 && T.Near(g.GetOffset(0), 0.2f));
        g.Reverse();
        T.Check("reverse mirrors the offsets", T.Near(g.GetOffset(0), 0.2f) && T.Near(g.GetOffset(1), 0.8f));
    }

    // ---- Path3D / PathFollow3D ----

    private static (Path3D Path, PathFollow3D Follow) Rider(Curve3D curve)
    {
        var path = new Path3D { Name = "Path" };
        var follow = new PathFollow3D { Name = "Follow" };
        path.AddChild(follow);
        SceneTree.Instance.Root.AddChild(path);
        path.Curve = curve;
        return (path, follow);
    }

    private static void PathFollowing()
    {
        var (path, f) = Rider(Line(10f));
        f.RotationMode = PathFollow3D.RotationModeEnum.None;
        f.Progress = 2.5f;
        T.CheckVec("progress places the node along the curve", new Vector3(2.5f, 0, 0), f.Position, 2e-3f);
        f.ProgressRatio = 0.5f;
        T.CheckNum("progress_ratio is a fraction of the length", 5f, f.Progress, 1e-2f);
        T.CheckVec("and places the node", new Vector3(5, 0, 0), f.Position, 2e-3f);
        T.CheckNum("progress_ratio reads back", 0.5f, f.ProgressRatio, 1e-3f);
        f.Progress = 12f;
        T.CheckNum("looping wraps 12 to 2", 2f, f.Progress, 1e-2f);
        f.Progress = -3f;
        T.CheckNum("and -3 to 7", 7f, f.Progress, 1e-2f);
        f.Progress = 20f;
        T.CheckNum("an exact multiple of the length lands on the end (10)", 10f, f.Progress, 1e-2f);
        f.Loop = false;
        f.Progress = 12f;
        T.CheckNum("without loop it clamps to the end", 10f, f.Progress, 1e-2f);
        f.Progress = -1f;
        T.CheckNum("and to the start", 0f, f.Progress, 1e-2f);
        f.Basis = Basis.Identity;
        f.Progress = 1f;
        T.Check("rotation_mode none leaves the basis alone", T.Near(f.Basis.X, new Vector3(1, 0, 0)) && T.Near(f.Basis.Y, new Vector3(0, 1, 0)) && T.Near(f.Basis.Z, new Vector3(0, 0, 1)));

        f.Loop = true;
        f.RotationMode = PathFollow3D.RotationModeEnum.Oriented;
        f.Progress = 4f;
        T.CheckVec("oriented: -Z along the path", new Vector3(-1, 0, 0), f.Basis.Z, 1e-3f);
        T.CheckVec("Y up", Vector3.Up, f.Basis.Y, 1e-3f);
        f.HOffset = 1f;
        f.VOffset = 2f;
        T.CheckVec("h_offset slides along the node's own X (to the right of travel), v_offset along Y", new Vector3(4, 2, 1), f.Position, 5e-3f);
        f.HOffset = 0; f.VOffset = 0;

        // A diagonal climb: Y yaws only, Xy also pitches.
        var diag = new Curve3D();
        diag.AddPoint(Vector3.Zero);
        diag.AddPoint(new Vector3(10, 5, 10));
        var (_, g) = Rider(diag);
        g.Progress = 3f;
        g.RotationMode = PathFollow3D.RotationModeEnum.Y;
        Vector3 forward = -g.Basis.Z;
        T.CheckVec("rotation_mode Y: -Z follows the tangent flattened onto the ground", new Vector3(0.7071f, 0, 0.7071f), forward, 2e-3f);
        g.RotationMode = PathFollow3D.RotationModeEnum.Xy;
        forward = -g.Basis.Z;
        T.CheckVec("rotation_mode XY: -Z follows the whole tangent", new Vector3(10, 5, 10).Normalized(), forward, 2e-3f);
        T.Check("Y stays in the vertical plane of the tangent (no roll: the node's X axis is horizontal)", MathF.Abs(g.Basis.X.Y) < 1e-3f && g.Basis.Y.Y > 0.5f);

        // Changing the curve is noticed.
        path.Curve!.SetPointPosition(1, new Vector3(4, 0, 0));
        f.RotationMode = PathFollow3D.RotationModeEnum.None;
        f.Loop = false;
        f.Progress = 9f;
        T.CheckNum("a shorter curve clamps the progress", 4f, f.Progress, 1e-2f);
        path.Curve = Line(100f);
        f.Progress = 50f;
        T.CheckVec("swapping the curve re-places the node", new Vector3(50, 0, 0), f.Position, 5e-3f);
        T.CheckNum("progress_ratio uses the new length", 0.5f, f.ProgressRatio, 1e-3f);
        f.Free();
        path.Free();
    }

    // ---- blend-space geometry ----

    private static void BlendSpaceGeometry()
    {
        var a = new Vector2(0, 0); var b = new Vector2(4, 0); var c = new Vector2(0, 4);
        T.Check("a point inside has barycentric weights", BlendSpaceMath.Barycentric(new Vector2(1, 1), a, b, c, out float wa, out float wb, out float wc) && T.Near(wa, 0.5f) && T.Near(wb, 0.25f) && T.Near(wc, 0.25f));
        T.Check("weights sum to 1", T.Near(wa + wb + wc, 1f));
        T.Check("a vertex is weight 1", BlendSpaceMath.Barycentric(b, a, b, c, out wa, out wb, out wc) && T.Near(wb, 1f) && wa == 0 && wc == 0);
        T.Check("a point on an edge has two weights", BlendSpaceMath.Barycentric(new Vector2(2, 2), a, b, c, out wa, out wb, out wc) && T.Near(wa, 0f) && T.Near(wb, 0.5f) && T.Near(wc, 0.5f));
        T.Check("a point outside is refused", !BlendSpaceMath.Barycentric(new Vector2(5, 5), a, b, c, out _, out _, out _));
        T.Check("so is a flat triangle", !BlendSpaceMath.Barycentric(new Vector2(1, 1), a, new Vector2(1, 0), new Vector2(2, 0), out _, out _, out _));
        T.CheckNum("closest point on a segment: the projection (0.25)", 0.25f, BlendSpaceMath.ClosestOnSegment(new Vector2(1, 5), a, b));
        T.CheckNum("clamped past the end", 1f, BlendSpaceMath.ClosestOnSegment(new Vector2(9, 5), a, b));

        // Four points, no four on a circle: the triangulation is unique.
        var pts = new List<Vector2> { new(0, 0), new(10, 0), new(5, 8), new(5, 3) };
        var tris = BlendSpaceMath.Triangulate(pts);
        T.Check("an interior point splits the triangle into three", tris.Count == 3);
        var sets = tris.Select(t => string.Join(",", new[] { t.A, t.B, t.C }.OrderBy(i => i))).OrderBy(s => s).ToArray();
        T.CheckSeq("they are 0-1-3, 0-2-3 and 1-2-3", new[] { "0,1,3", "0,2,3", "1,2,3" }, sets);
        T.Check("fewer than three points, or points on a line, give none", BlendSpaceMath.Triangulate(pts.Take(2).ToList()).Count == 0 &&
                BlendSpaceMath.Triangulate(new List<Vector2> { new(0, 0), new(1, 1), new(2, 2) }).Count == 0);
        var quad = BlendSpaceMath.Triangulate(new List<Vector2> { new(0, 0), new(10, 0), new(10, 1), new(0, 5) });
        T.Check("a quadrilateral that is not cyclic: two triangles covering it, the Delaunay diagonal", quad.Count == 2);

        // The node's weights.
        var space = new AnimationNodeBlendSpace2D();
        var leaf = new AnimationNodeAnimation();
        space.AddBlendPoint(leaf, new Vector2(0, 0));
        space.AddBlendPoint(leaf, new Vector2(10, 0));
        space.AddBlendPoint(leaf, new Vector2(5, 8));
        space.AddBlendPoint(leaf, new Vector2(5, 3));
        T.Check("auto triangles: three triangles", space.GetTriangleCount() == 3);
        float[] w = space.GetWeights(new Vector2(5, 3));
        T.Check("on a point: that point alone", T.Near(w[3], 1f) && T.Near(w.Sum(), 1f));
        w = space.GetWeights(new Vector2(5, 0));
        T.Check("on the edge between points 0 and 1: half each", T.Near(w[0], 0.5f) && T.Near(w[1], 0.5f) && T.Near(w.Sum(), 1f));
        w = space.GetWeights(new Vector2(5, -10));
        T.Check("below the triangles it snaps to the nearest edge point (5, 0): half of 0 and 1", T.Near(w[0], 0.5f) && T.Near(w[1], 0.5f));
        w = space.GetWeights(new Vector2(30, 0));
        T.Check("far to the right it snaps to the corner (10, 0)", T.Near(w[1], 1f));
        space.AutoTriangles = false;
        space.Triangles = new[] { 0, 1, 3 };
        T.Check("stored triangles are kept as they are", space.GetTriangleCount() == 1 && space.GetTrianglePoint(0, 2) == 3);
        space.RemoveBlendPoint(3);
        T.Check("removing a point drops its triangles", space.GetTriangleCount() == 0 && space.GetBlendPointCount() == 3);

        var one = new AnimationNodeBlendSpace1D { MinSpace = 0, MaxSpace = 3 };
        one.AddBlendPoint(leaf, 0f);
        one.AddBlendPoint(leaf, 1f);
        one.AddBlendPoint(leaf, 3f);
        T.Check("1D: halfway between points 0 and 1", T.Near(one.GetWeights(0.5f)[0], 0.5f) && T.Near(one.GetWeights(0.5f)[1], 0.5f));
        T.Check("1D: three quarters of the way from 1 to 3 is 0.25 / 0.75", T.Near(one.GetWeights(2.5f)[1], 0.25f) && T.Near(one.GetWeights(2.5f)[2], 0.75f));
        T.Check("1D: beyond the last point it is the last point alone", T.Near(one.GetWeights(7f)[2], 1f));
        one.MinSpace = -2;
        T.Check("1D: before the first point it is the first alone", T.Near(one.GetWeights(-1.5f)[0], 1f));
    }

    // ---- Skeleton3D ----

    private static Skeleton3D Rig()
    {
        var s = new Skeleton3D { Name = "Skel" };
        s.AddBone("Hips");
        s.AddBone("Spine");
        s.AddBone("Head");
        s.SetBoneParent(1, 0);
        s.SetBoneParent(2, 1);
        s.SetBoneRest(1, new Transform3D(Basis.Identity, new Vector3(0, 1, 0)));
        s.SetBoneRest(2, new Transform3D(Basis.Identity, new Vector3(0, 1, 0)));
        s.ResetBonePoses();
        return s;
    }

    private static void SkeletonApi()
    {
        var s = Rig();
        T.Check("three bones", s.GetBoneCount() == 3 && s.FindBone("Spine") == 1 && s.FindBone("Tail") == -1 && s.GetBoneName(2) == "Head");
        T.Check("add_bone refuses a taken or empty name", s.AddBone("Hips") == -1 && s.AddBone("") == -1);
        T.Check("parents and children", s.GetBoneParent(2) == 1 && s.GetBoneParent(0) == -1 && s.GetParentlessBones().SequenceEqual(new[] { 0 }) && s.GetBoneChildren(0).SequenceEqual(new[] { 1 }));
        s.SetBoneParent(0, 2);
        T.Check("a cycle is refused", s.GetBoneParent(0) == -1);
        T.CheckVec("global rest of the head", new Vector3(0, 2, 0), s.GetBoneGlobalRest(2).Origin);
        T.CheckVec("reset_bone_poses copies the rests into the poses", new Vector3(0, 1, 0), s.GetBonePosePosition(1));
        T.CheckVec("global pose of the head", new Vector3(0, 2, 0), s.GetBoneGlobalPose(2).Origin);

        s.SetBonePosePosition(0, new Vector3(5, 0, 0));
        T.CheckVec("moving the root moves everything below it", new Vector3(5, 2, 0), s.GetBoneGlobalPose(2).Origin);
        s.SetBonePosePosition(0, Vector3.Zero);
        s.SetBonePoseRotation(0, new Quaternion(new Vector3(0, 0, 1), MathF.PI / 2));
        T.CheckVec("a quarter turn about Z at the root swings the head to (-2, 0, 0)", new Vector3(-2, 0, 0), s.GetBoneGlobalPose(2).Origin, 1e-4f);
        T.Check("pose rotation reads back", T.Near(s.GetBonePoseRotation(0), new Quaternion(new Vector3(0, 0, 1), MathF.PI / 2)));
        s.SetBonePoseRotation(0, Quaternion.Identity);
        s.SetBonePoseScale(0, new Vector3(2, 2, 2));
        T.CheckVec("scale at the root scales the offsets below it", new Vector3(0, 4, 0), s.GetBoneGlobalPose(2).Origin, 1e-4f);
        T.CheckVec("scale reads back", new Vector3(2, 2, 2), s.GetBonePoseScale(0));
        s.SetBonePoseScale(0, Vector3.One);

        s.SetBoneGlobalPose(1, new Transform3D(Basis.Identity, new Vector3(3, 0, 0)));
        T.CheckVec("set_bone_global_pose edits the local pose to land the bone there", new Vector3(3, 0, 0), s.GetBoneGlobalPose(1).Origin);
        T.CheckVec("and the bone below follows", new Vector3(3, 1, 0), s.GetBoneGlobalPose(2).Origin);
        T.CheckVec("the local pose is relative to the parent", new Vector3(3, 0, 0), s.GetBonePose(1).Origin);
        s.SetBonePose(1, new Transform3D(Basis.Identity, new Vector3(0, 1, 0)));

        s.SetBoneGlobalPoseOverride(2, new Transform3D(Basis.Identity, new Vector3(0, 10, 0)), 0.5f);
        T.CheckVec("an override at 0.5 sits halfway to its pose", new Vector3(0, 6, 0), s.GetBoneGlobalPose(2).Origin, 1e-4f);
        T.CheckVec("without it the pose is as animated", new Vector3(0, 2, 0), s.GetBoneGlobalPoseNoOverride(2).Origin);
        T.CheckVec("the override reads back", new Vector3(0, 10, 0), s.GetBoneGlobalPoseOverride(2).Origin);
        s.AdvanceModifiers(0.016);
        T.CheckVec("a non-persistent override is dropped after a modifier pass", new Vector3(0, 2, 0), s.GetBoneGlobalPose(2).Origin);
        s.SetBoneGlobalPoseOverride(2, new Transform3D(Basis.Identity, new Vector3(0, 10, 0)), 1f, true);
        s.AdvanceModifiers(0.016);
        T.CheckVec("a persistent one stays", new Vector3(0, 10, 0), s.GetBoneGlobalPose(2).Origin, 1e-4f);
        s.ClearBonesGlobalPoseOverride();
        T.CheckVec("clear_bones_global_pose_override", new Vector3(0, 2, 0), s.GetBoneGlobalPose(2).Origin);

        s.SetBonePosePosition(1, new Vector3(0, 7, 0));
        s.SetBoneEnabled(1, false);
        T.Check("a disabled bone uses its rest", !s.IsBoneEnabled(1) && T.Near(s.GetBoneGlobalPose(1).Origin, new Vector3(0, 1, 0)));
        s.SetBoneEnabled(1, true);
        T.CheckVec("enabled again it uses its pose", new Vector3(0, 7, 0), s.GetBoneGlobalPose(1).Origin);
        s.ShowRestOnly = true;
        T.CheckVec("show_rest_only shows the rests", new Vector3(0, 2, 0), s.GetBoneGlobalPose(2).Origin);
        s.ShowRestOnly = false;
        s.ResetBonePose(1);
        T.CheckVec("reset_bone_pose", new Vector3(0, 1, 0), s.GetBonePosePosition(1));
        s.SetBoneMeta(1, "tag", "x");
        T.Check("bone meta", s.GetBoneMeta(1, "tag").AsString() == "x");

        // A parent that comes later in the list.
        var late = new Skeleton3D();
        late.AddBone("Child"); late.AddBone("Parent");
        late.SetBoneParent(0, 1);
        late.SetBoneRest(1, new Transform3D(Basis.Identity, new Vector3(0, 0, 4)));
        late.SetBoneRest(0, new Transform3D(Basis.Identity, new Vector3(0, 1, 0)));
        T.CheckVec("globals resolve parents that follow their children in the list", new Vector3(0, 1, 4), late.GetBoneGlobalRest(0).Origin);

        // Scene values: bones/N/...
        var scene = new Skeleton3D();
        scene.Set("bones/0/name", "Root");
        scene.Set("bones/0/parent", -1);
        scene.Set("bones/1/name", "Arm");
        scene.Set("bones/1/parent", 0);
        scene.Set("bones/1/rest", new Transform3D(Basis.Identity, new Vector3(0, 1, 0)));
        scene.Set("bones/1/position", new Vector3(0, 1, 0));
        scene.Set("bones/1/rotation", Quaternion.Identity);
        scene.Set("bones/1/scale", Vector3.One);
        scene.Set("bones/1/enabled", true);
        T.Check("bones/N/... builds the list as a scene stores it", scene.GetBoneCount() == 2 && scene.GetBoneName(1) == "Arm" && scene.GetBoneParent(1) == 0);
        T.CheckVec("and the poses", new Vector3(0, 1, 0), scene.GetBoneGlobalPose(1).Origin);
        T.Check("bones/N/name reads back", scene.Get("bones/1/name").AsString() == "Arm");
        s.ClearBones();
        T.Check("clear_bones", s.GetBoneCount() == 0);
    }

    // ---- modifiers ----

    private static void Modifiers()
    {
        var root = new Node3D { Name = "Root" };
        var s = Rig();
        var mod = new RaiseModifier { Name = "Raise" };
        s.AddChild(mod);
        root.AddChild(s);
        SceneTree.Instance.Root.AddChild(root);
        int processed = 0;
        mod.ModificationProcessed += () => processed++;
        s.AdvanceModifiers(0.016);
        T.Check("a modifier under the skeleton runs when the skeleton advances", mod.Runs == 1 && processed == 1);
        T.CheckVec("and edits the pose", new Vector3(0, 10, 0), s.GetBonePosePosition(0));
        s.ResetBonePoses();
        mod.Influence = 0.5f;
        s.AdvanceModifiers(0.016);
        T.CheckVec("influence 0.5 keeps half of the change", new Vector3(0, 5, 0), s.GetBonePosePosition(0), 1e-4f);
        s.ResetBonePoses();
        mod.Influence = 0f;
        s.AdvanceModifiers(0.016);
        T.Check("influence 0 does nothing", mod.Runs == 2 && T.Near(s.GetBonePosePosition(0), Vector3.Zero));
        mod.Influence = 1f;
        mod.Active = false;
        s.AdvanceModifiers(0.016);
        T.Check("an inactive modifier is skipped", mod.Runs == 2);
        mod.Active = true;
        s.ResetBonePoses();
        T.Frame(0.016);
        T.Check("the skeleton advances its modifiers every frame by itself", mod.Runs == 3);

        // BoneAttachment3D.
        var grip = new BoneAttachment3D { Name = "Grip", BoneName = "Head" };
        s.AddChild(grip);
        T.Frame(0.016);
        T.CheckVec("an attachment sits on its bone (the modifier lifted the root by 10: head at (0, 12, 0))", new Vector3(0, 12, 0), grip.Position, 1e-3f);
        mod.Active = false;
        s.ResetBonePoses();
        s.SetBonePosePosition(1, new Vector3(2, 1, 0));
        T.Frame(0.016);
        T.CheckVec("and follows it when the pose changes", new Vector3(2, 2, 0), grip.Position, 1e-3f);
        grip.BoneName = "Spine";
        T.CheckVec("changing bone_name moves it at once", new Vector3(2, 1, 0), grip.Position, 1e-3f);
        T.Check("bone_idx follows the name", grip.BoneIdx == 1);

        // SkeletonIK3D: a two-bone chain reaching.
        var ikRoot = new Node3D { Name = "IkRoot" };
        var ik = Rig();
        ik.Name = "IkSkel";
        var solver = new SkeletonIK3D { Name = "Reach", RootBone = "Hips", TipBone = "Head", MaxIterations = 20, Target = new Transform3D(Basis.Identity, new Vector3(1, 1, 0)) };
        ik.AddChild(solver);
        ikRoot.AddChild(ik);
        SceneTree.Instance.Root.AddChild(ikRoot);
        T.Check("an IK node is idle until started", !solver.IsRunning());
        solver.Start();
        T.Check("start", solver.IsRunning());
        ik.AdvanceModifiers(0.016);
        T.CheckVec("the tip reaches a reachable target", new Vector3(1, 1, 0), ik.GetBoneGlobalPose(2).Origin, 0.01f);
        T.Check("and the bones keep their lengths", MathF.Abs(ik.GetBoneGlobalPose(1).Origin.DistanceTo(ik.GetBoneGlobalPose(0).Origin) - 1f) < 1e-3f &&
                MathF.Abs(ik.GetBoneGlobalPose(2).Origin.DistanceTo(ik.GetBoneGlobalPose(1).Origin) - 1f) < 1e-3f);
        T.CheckVec("the root stays put", Vector3.Zero, ik.GetBoneGlobalPose(0).Origin, 1e-4f);
        solver.Target = new Transform3D(Basis.Identity, new Vector3(5, 0, 0));
        ik.ResetBonePoses();
        ik.AdvanceModifiers(0.016);
        T.CheckVec("an unreachable target stretches the chain straight at it: the tip two metres out", new Vector3(2, 0, 0), ik.GetBoneGlobalPose(2).Origin, 0.01f);
        solver.Stop();
        ik.ResetBonePoses();
        ik.AdvanceModifiers(0.016);
        T.CheckVec("stopped, it leaves the pose alone", new Vector3(0, 2, 0), ik.GetBoneGlobalPose(2).Origin, 1e-4f);
        solver.Start();
        solver.Target = new Transform3D(Basis.Identity, new Vector3(1, 1, 0));
        solver.Interpolation = 0.5f;
        ik.ResetBonePoses();
        ik.AdvanceModifiers(0.016);
        float tipX = ik.GetBoneGlobalPose(2).Origin.X;
        T.Check("interpolation 0.5 blends the solved pose with the unsolved one (the tip is part of the way)", tipX > 0.05f && tipX < 0.95f, $"x={tipX}");
    }

    // ---- SpringArm3D ----

    private static void SpringArm()
    {
        var root = new Node3D { Name = "Root" };
        var arm = new SpringArm3D { Name = "Arm", SpringLength = 4f };
        var cam = new Node3D { Name = "Cam" };
        arm.AddChild(cam);
        root.AddChild(arm);
        SceneTree.Instance.Root.AddChild(root);

        arm.Probe = (from, to) => null;
        arm.ForceUpdate();
        T.CheckVec("with nothing in the way the child sits at spring_length minus the margin along +Z", new Vector3(0, 0, 3.99f), cam.Position, 1e-4f);
        T.CheckNum("hit length is the full arm", 4f, arm.GetHitLength());

        Vector3 seenFrom = default, seenTo = default;
        arm.Probe = (from, to) => { seenFrom = from; seenTo = to; return 2f; };
        arm.ForceUpdate();
        T.CheckVec("a hit 2 m along pulls it in", new Vector3(0, 0, 1.99f), cam.Position, 1e-4f);
        T.CheckNum("hit length", 2f, arm.GetHitLength());
        T.Check("the cast runs from the arm's origin along +Z for spring_length", T.Near(seenFrom, Vector3.Zero) && T.Near(seenTo, new Vector3(0, 0, 4)));

        arm.Margin = 0.5f;
        T.CheckVec("the margin keeps the child off the hit", new Vector3(0, 0, 1.5f), cam.Position, 1e-4f);
        arm.Margin = 0.01f;

        arm.Rotation = new Vector3(0, MathF.PI / 2, 0);
        arm.ForceUpdate();
        T.CheckVec("the arm's own rotation aims it: +Z becomes +X after a quarter turn about Y", new Vector3(1.99f, 0, 0), cam.GlobalPosition, 1e-3f);
        T.Check("the child keeps the orientation it had (it is placed, not turned): the arm's quarter turn is already in it", T.Near(cam.GlobalTransform.Basis.Z, new Vector3(1, 0, 0)));

        arm.Probe = (from, to) => 99f;
        arm.ForceUpdate();
        T.CheckNum("a hit past the arm does not push the child out", 4f, arm.GetHitLength());
        arm.SpringLength = 0f;
        arm.Probe = (from, to) => 1f;
        arm.ForceUpdate();
        T.Check("a zero length arm casts nothing and does not divide by zero", arm.GetHitLength() == 0f);

        var rid = new Rid(5);
        arm.AddExcludedObject(rid);
        T.Check("excluded objects", arm.RemoveExcludedObject(rid) && !arm.RemoveExcludedObject(rid));
        arm.ClearExcludedObjects();
        T.Check("it is a Node3D that needs no physics world to exist", arm.CollisionMask == 1 && arm.Shape == null);

        arm.Probe = (from, to) => 1f;
        arm.SpringLength = 4f;
        T.Frame(0.016, 1);
        T.CheckNum("every physics step runs it", 1f, arm.GetHitLength());
    }

    // ---- RemoteTransform3D ----

    private static void RemoteTransform()
    {
        var root = new Node3D { Name = "Root" };
        var src = new RemoteTransform3D { Name = "Src", RemotePath = "../Dest" };
        var dest = new Node3D { Name = "Dest", Position = new Vector3(9, 9, 9), Scale = new Vector3(2, 2, 2) };
        root.AddChild(src);
        root.AddChild(dest);
        SceneTree.Instance.Root.AddChild(root);
        src.Transform = new Transform3D(new Basis(new Vector3(0, 1, 0), MathF.PI / 2), new Vector3(1, 2, 3));

        src.ForceUpdateCache();
        T.CheckVec("the remote node takes the transform (position)", new Vector3(1, 2, 3), dest.GlobalPosition, 1e-4f);
        T.Check("rotation", T.Near(dest.Quaternion, new Quaternion(new Vector3(0, 1, 0), MathF.PI / 2)));
        T.CheckVec("and scale (the source's: 1)", Vector3.One, dest.Scale);

        dest.Position = new Vector3(9, 9, 9); dest.Scale = new Vector3(2, 2, 2); dest.Quaternion = Quaternion.Identity;
        src.UpdateScale = false;
        src.ForceUpdateCache();
        T.CheckVec("update_scale off keeps the remote's scale", new Vector3(2, 2, 2), dest.Scale);
        T.CheckVec("and still copies the position", new Vector3(1, 2, 3), dest.Position);
        src.UpdatePosition = false;
        dest.Position = new Vector3(9, 9, 9);
        src.ForceUpdateCache();
        T.CheckVec("update_position off keeps the remote's position", new Vector3(9, 9, 9), dest.Position);
        T.Check("rotation still copied", T.Near(dest.Quaternion, new Quaternion(new Vector3(0, 1, 0), MathF.PI / 2)));
        src.UpdatePosition = true; src.UpdateScale = true;

        src.Position = new Vector3(4, 0, 0);
        T.Frame(0.016);
        T.CheckVec("it follows by itself every frame", new Vector3(4, 0, 0), dest.Position, 1e-4f);

        // Local coordinates.
        var shifted = new Node3D { Name = "Shifted", Position = new Vector3(100, 0, 0) };
        var src2 = new RemoteTransform3D { Name = "Src2", RemotePath = "../../Other/Dest2", UseGlobalCoordinates = false, Position = new Vector3(1, 0, 0) };
        var other = new Node3D { Name = "Other", Position = new Vector3(0, 50, 0) };
        var dest2 = new Node3D { Name = "Dest2" };
        shifted.AddChild(src2);
        other.AddChild(dest2);
        root.AddChild(shifted);
        root.AddChild(other);
        src2.ForceUpdateCache();
        T.CheckVec("local coordinates copy the local transform (1, 0, 0)", new Vector3(1, 0, 0), dest2.Position, 1e-4f);
        src2.UseGlobalCoordinates = true;
        src2.ForceUpdateCache();
        T.CheckVec("global coordinates copy the global one, whatever the remote's parent is: world (101, 0, 0)", new Vector3(101, 0, 0), dest2.GlobalPosition, 1e-4f);
        src2.RemotePath = "../nothing";
        T.Check("a path that names nothing is quietly ignored", true);
        src2.Position = new Vector3(5, 5, 5);
        T.Frame(0.016);
        T.CheckVec("and nothing moved", new Vector3(101, 0, 0), dest2.GlobalPosition, 1e-4f);
    }

    // ---- on-screen notifiers ----

    private static void ScreenNotifiers()
    {
        var root = new Node3D { Name = "Root" };
        var cam = new Camera3D { Name = "Cam", Fov = 75f };
        root.AddChild(cam);
        var n = new VisibleOnScreenNotifier3D { Name = "N", Position = new Vector3(0, 0, -10) };
        root.AddChild(n);
        SceneTree.Instance.Root.AddChild(root);
        cam.MakeCurrent();
        int entered = 0, exited = 0;
        n.ScreenEntered += () => entered++;
        n.ScreenExited += () => exited++;
        T.Check("not on screen before the first test", !n.IsOnScreen());
        T.Frame(0.016);
        T.Check("a box in front of the camera enters", n.IsOnScreen() && entered == 1 && exited == 0);
        T.Frame(0.016);
        T.Check("and the signal is not repeated", entered == 1);
        n.Position = new Vector3(0, 0, 10);
        T.Frame(0.016);
        T.Check("behind the camera it exits", !n.IsOnScreen() && exited == 1);
        // The view is 1280 x 720: at 10 m the half width is tan(37.5 degrees) * 10 * 16/9 = 13.64 m.
        n.Position = new Vector3(14.5f, 0, -10);
        T.Frame(0.016);
        T.Check("a box straddling the side of the view counts as on screen", n.IsOnScreen() && entered == 2);
        n.Position = new Vector3(20f, 0, -10);
        T.Frame(0.016);
        T.Check("one wholly outside does not", !n.IsOnScreen() && exited == 2);
        n.Position = new Vector3(0, 12, -10);
        T.Frame(0.016);
        T.Check("above the view (half height 7.67 m) it is off too", !n.IsOnScreen());
        n.Position = new Vector3(0, 0, -10);
        n.Aabb = new Aabb(new Vector3(-50, -50, -50), new Vector3(100, 100, 100));
        T.Frame(0.016);
        T.Check("a huge box around the camera is on screen", n.IsOnScreen());
        n.Visible = false;
        T.Frame(0.016);
        T.Check("a hidden notifier is never on screen", !n.IsOnScreen());

        // The enabler turns its target's processing on while seen.
        var thing = new Node { Name = "Thing" };
        var en = new VisibleOnScreenEnabler3D { Name = "En", Position = new Vector3(0, 0, -10), EnableMode = VisibleOnScreenEnabler3D.EnableModeEnum.Always };
        thing.AddChild(en);
        root.AddChild(thing);
        T.Check("the target starts disabled", thing.ProcessMode == Node.ProcessModeEnum.Disabled);
        T.Frame(0.016);
        T.Check("on screen it takes the enable mode (always)", en.IsOnScreen() && thing.ProcessMode == Node.ProcessModeEnum.Always);
        en.Position = new Vector3(0, 0, 10);
        T.Frame(0.016);
        T.Check("off screen it is disabled again", thing.ProcessMode == Node.ProcessModeEnum.Disabled);
        en.EnableMode = VisibleOnScreenEnabler3D.EnableModeEnum.WhenPaused;
        en.Position = new Vector3(0, 0, -10);
        T.Frame(0.016);
        T.Check("when_paused maps to process mode WhenPaused", thing.ProcessMode == Node.ProcessModeEnum.WhenPaused);
        T.Check("the enabler keeps watching while its target is off (it runs Always)", en.ProcessMode == Node.ProcessModeEnum.Always);
    }
}
