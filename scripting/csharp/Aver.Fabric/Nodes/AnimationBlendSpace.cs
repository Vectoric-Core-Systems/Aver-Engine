// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// AnimationNodeBlendSpace1D and AnimationNodeBlendSpace2D (Godot's): points with an animation node each, weighted by a position
// (parameters/.../blend_position). 1D blends the two points either side of the position; 2D blends the three corners of the triangle that
// holds it (the triangles are a Delaunay triangulation of the points unless a scene stored its own), and outside the triangles takes the
// nearest point of the nearest edge. docs/plans/fabric/G5_PARITY.md, A2.

namespace Aver.Fabric;

/// <summary>The geometry of a 2D blend space, separate from the nodes so it can be checked alone.</summary>
public static class BlendSpaceMath
{
    /// <summary>The barycentric weights of <paramref name="p"/> in the triangle, or false when it lies outside it (or the triangle is flat).</summary>
    public static bool Barycentric(Vector2 p, Vector2 a, Vector2 b, Vector2 c, out float wa, out float wb, out float wc)
    {
        wa = wb = wc = 0;
        if (p.IsEqualApprox(a)) { wa = 1; return true; }
        if (p.IsEqualApprox(b)) { wb = 1; return true; }
        if (p.IsEqualApprox(c)) { wc = 1; return true; }
        Vector2 v0 = b - a, v1 = c - a, v2 = p - a;
        float d00 = v0.Dot(v0), d01 = v0.Dot(v1), d11 = v1.Dot(v1), d20 = v2.Dot(v0), d21 = v2.Dot(v1);
        float denom = d00 * d11 - d01 * d01;
        if (denom == 0) return false;
        wb = (d11 * d20 - d01 * d21) / denom;
        wc = (d00 * d21 - d01 * d20) / denom;
        wa = 1f - wb - wc;
        const float eps = 1e-5f;
        if (wa < -eps || wb < -eps || wc < -eps) { wa = wb = wc = 0; return false; }
        wa = MathF.Max(wa, 0); wb = MathF.Max(wb, 0); wc = MathF.Max(wc, 0);
        float sum = wa + wb + wc;
        wa /= sum; wb /= sum; wc /= sum;
        return true;
    }

    /// <summary>The point of segment a-b nearest to <paramref name="p"/>, as the fraction t along it.</summary>
    public static float ClosestOnSegment(Vector2 p, Vector2 a, Vector2 b)
    {
        Vector2 ab = b - a;
        float len2 = ab.Dot(ab);
        return len2 <= 0 ? 0 : Math.Clamp((p - a).Dot(ab) / len2, 0f, 1f);
    }

    /// <summary>The triangles of a Delaunay triangulation of the points (Bowyer-Watson). Fewer than three points, or points on one line, give none.</summary>
    public static List<(int A, int B, int C)> Triangulate(IReadOnlyList<Vector2> points)
    {
        var result = new List<(int, int, int)>();
        int n = points.Count;
        if (n < 3) return result;
        double minX = double.MaxValue, minY = double.MaxValue, maxX = double.MinValue, maxY = double.MinValue;
        foreach (Vector2 p in points) { minX = Math.Min(minX, p.X); minY = Math.Min(minY, p.Y); maxX = Math.Max(maxX, p.X); maxY = Math.Max(maxY, p.Y); }
        double dm = Math.Max(Math.Max(maxX - minX, maxY - minY), 1e-6) * 20, cx = (minX + maxX) / 2, cy = (minY + maxY) / 2;
        var pts = new List<(double X, double Y)>();
        foreach (Vector2 p in points) pts.Add((p.X, p.Y));
        pts.Add((cx - dm, cy - dm));
        pts.Add((cx, cy + dm));
        pts.Add((cx + dm, cy - dm));
        var tris = new List<(int A, int B, int C)> { (n, n + 1, n + 2) };

        bool InCircle((int A, int B, int C) t, int pi)
        {
            var (ax, ay) = pts[t.A]; var (bx, by) = pts[t.B]; var (cx2, cy2) = pts[t.C]; var (px, py) = pts[pi];
            double adx = ax - px, ady = ay - py, bdx = bx - px, bdy = by - py, cdx = cx2 - px, cdy = cy2 - py;
            double det = (adx * adx + ady * ady) * (bdx * cdy - cdx * bdy) - (bdx * bdx + bdy * bdy) * (adx * cdy - cdx * ady) + (cdx * cdx + cdy * cdy) * (adx * bdy - bdx * ady);
            double orient = (bx - ax) * (cy2 - ay) - (by - ay) * (cx2 - ax);
            return orient > 0 ? det > 1e-12 : det < -1e-12;
        }

        var seen = new HashSet<(double, double)>();
        for (int i = 0; i < n; i++)
        {
            if (!seen.Add(pts[i])) continue;
            var bad = new List<(int A, int B, int C)>();
            foreach (var t in tris) if (InCircle(t, i)) bad.Add(t);
            var edges = new List<(int, int)>();
            foreach (var t in bad)
            {
                foreach (var e in new[] { (t.A, t.B), (t.B, t.C), (t.C, t.A) })
                {
                    bool shared = false;
                    foreach (var u in bad)
                    {
                        if (u.Equals(t)) continue;
                        foreach (var f in new[] { (u.A, u.B), (u.B, u.C), (u.C, u.A) })
                            if ((f.Item1 == e.Item1 && f.Item2 == e.Item2) || (f.Item1 == e.Item2 && f.Item2 == e.Item1)) shared = true;
                    }
                    if (!shared) edges.Add(e);
                }
            }
            tris.RemoveAll(bad.Contains);
            foreach (var e in edges) tris.Add((e.Item1, e.Item2, i));
        }
        foreach (var t in tris)
        {
            if (t.A >= n || t.B >= n || t.C >= n) continue;
            double area = (pts[t.B].X - pts[t.A].X) * (pts[t.C].Y - pts[t.A].Y) - (pts[t.B].Y - pts[t.A].Y) * (pts[t.C].X - pts[t.A].X);
            if (Math.Abs(area) < 1e-12) continue;
            result.Add((t.A, t.B, t.C));
        }
        return result;
    }
}

public class AnimationNodeBlendSpace1D : AnimationRootNode
{
    public enum BlendModeEnum { Interpolated = 0, Discrete = 1, DiscreteCarry = 2 }

    private sealed class Carry { public int Closest = -1; public double Ratio; }

    private readonly List<(AnimationRootNode? Node, float Pos)> _points = new();

    public float MinSpace { get; set; } = -1f;
    public float MaxSpace { get; set; } = 1f;
    public float Snap { get; set; } = 0.1f;
    public string ValueLabel { get; set; } = "value";
    public BlendModeEnum BlendMode { get; set; } = BlendModeEnum.Interpolated;
    public bool Sync { get; set; }

    public override string GetCaption() => "BlendSpace1D";

    public void AddBlendPoint(AnimationRootNode node, float pos, int atIndex = -1)
    {
        if (atIndex < 0 || atIndex >= _points.Count) _points.Add((node, pos));
        else _points.Insert(atIndex, (node, pos));
        EmitSignal(AnimationNode.SignalName.TreeChanged);
    }

    public void SetBlendPointPosition(int point, float pos) { if (point >= 0 && point < _points.Count) _points[point] = (_points[point].Node, pos); }
    public float GetBlendPointPosition(int point) => point >= 0 && point < _points.Count ? _points[point].Pos : 0;
    public void SetBlendPointNode(int point, AnimationRootNode node) { if (point >= 0 && point < _points.Count) _points[point] = (node, _points[point].Pos); }
    public AnimationRootNode? GetBlendPointNode(int point) => point >= 0 && point < _points.Count ? _points[point].Node : null;
    public void RemoveBlendPoint(int point) { if (point >= 0 && point < _points.Count) { _points.RemoveAt(point); EmitSignal(AnimationNode.SignalName.TreeChanged); } }
    public int GetBlendPointCount() => _points.Count;

    internal override bool TryDynamicSet(string name, Variant value)
    {
        if (!name.StartsWith("blend_point_", StringComparison.Ordinal)) return false;
        string[] p = name.Split('/');
        if (p.Length != 2 || !int.TryParse(p[0].AsSpan(12), out int i) || i < 0 || i > 1024) return false;
        while (_points.Count <= i) _points.Add((null, 0));
        if (p[1] == "node") { _points[i] = (value.AsGodotObject() as AnimationRootNode, _points[i].Pos); return true; }
        if (p[1] == "pos") { _points[i] = (_points[i].Node, value.AsSingle()); return true; }
        return false;
    }

    /// <summary>The weight of each point at <paramref name="position"/>: the two either side share it, the end points take it outside.</summary>
    public float[] GetWeights(float position)
    {
        var w = new float[_points.Count];
        position = Math.Clamp(position, MinSpace, MaxSpace);
        int lower = -1, higher = -1;
        float posLower = 0, posHigher = 0;
        for (int i = 0; i < _points.Count; i++)
        {
            float p = _points[i].Pos;
            if (p <= position) { if (lower == -1 || p > posLower) { lower = i; posLower = p; } }
            else if (higher == -1 || p < posHigher) { higher = i; posHigher = p; }
        }
        if (lower == -1 && higher != -1) w[higher] = 1;
        else if (higher == -1 && lower != -1) w[lower] = 1;
        else if (lower != -1)
        {
            float t = (position - posLower) / (posHigher - posLower);
            w[lower] = 1f - t;
            w[higher] = t;
        }
        return w;
    }

    internal override void CollectParameters(AnimationTree tree, string @base)
    {
        tree.DeclareParam(@base + "blend_position", 0.0);
        tree.DeclareState<Carry>(@base + "carry");
        for (int i = 0; i < _points.Count; i++) _points[i].Node?.CollectParameters(tree, @base + "point_" + i + "/");
    }

    internal override void ResetState(AnimationTree tree, string @base)
    {
        for (int i = 0; i < _points.Count; i++) _points[i].Node?.ResetState(tree, @base + "point_" + i + "/");
        tree.StateOf<Carry>(@base + "carry").Closest = -1;
    }

    internal override NodeTime Run(Frame f, double time, bool seek, bool external, bool testOnly)
    {
        if (_points.Count == 0) return default;
        float position = f.Tree.Param(f.Base + "blend_position", 0.0).AsSingle();
        float[] w;
        if (BlendMode == BlendModeEnum.Interpolated) w = GetWeights(position);
        else
        {
            w = new float[_points.Count];
            int best = 0;
            float bestD = float.MaxValue;
            position = Math.Clamp(position, MinSpace, MaxSpace);
            for (int i = 0; i < _points.Count; i++)
            {
                float d = MathF.Abs(_points[i].Pos - position);
                if (d < bestD) { bestD = d; best = i; }
            }
            w[best] = 1;
        }
        return BlendSpaceRun.Play(this, f, time, seek, external, testOnly, Sync, BlendMode == BlendModeEnum.DiscreteCarry, w, i => (_points[i].Node, f.Base + "point_" + i + "/"),
            () => f.Tree.StateOf<Carry>(f.Base + "carry"), c => c.Closest, (c, v) => c.Closest = v, c => c.Ratio, (c, v) => c.Ratio = v);
    }
}

public class AnimationNodeBlendSpace2D : AnimationRootNode
{
    public enum BlendModeEnum { Interpolated = 0, Discrete = 1, DiscreteCarry = 2 }

    private sealed class Carry { public int Closest = -1; public double Ratio; }

    private readonly List<(AnimationRootNode? Node, Vector2 Pos)> _points = new();
    private readonly List<(int A, int B, int C)> _triangles = new();
    private bool _auto = true;

    public Vector2 MinSpace { get; set; } = new(-1, -1);
    public Vector2 MaxSpace { get; set; } = new(1, 1);
    public Vector2 Snap { get; set; } = new(0.1f, 0.1f);
    public string XLabel { get; set; } = "x";
    public string YLabel { get; set; } = "y";
    public BlendModeEnum BlendMode { get; set; } = BlendModeEnum.Interpolated;
    public bool Sync { get; set; }

    /// <summary>When true (the default) the triangles are recomputed whenever a point is added, moved or removed.</summary>
    public bool AutoTriangles { get => _auto; set { _auto = value; if (value) Retriangulate(); } }

    public override string GetCaption() => "BlendSpace2D";

    private void Retriangulate()
    {
        _triangles.Clear();
        _triangles.AddRange(BlendSpaceMath.Triangulate(_points.Select(p => p.Pos).ToList()));
    }

    public void AddBlendPoint(AnimationRootNode node, Vector2 pos, int atIndex = -1)
    {
        if (atIndex < 0 || atIndex >= _points.Count) _points.Add((node, pos));
        else _points.Insert(atIndex, (node, pos));
        if (_auto) Retriangulate();
        EmitSignal(AnimationNode.SignalName.TreeChanged);
    }

    public void SetBlendPointPosition(int point, Vector2 pos)
    {
        if (point < 0 || point >= _points.Count) return;
        _points[point] = (_points[point].Node, pos);
        if (_auto) Retriangulate();
    }

    public Vector2 GetBlendPointPosition(int point) => point >= 0 && point < _points.Count ? _points[point].Pos : default;
    public void SetBlendPointNode(int point, AnimationRootNode node) { if (point >= 0 && point < _points.Count) _points[point] = (node, _points[point].Pos); }
    public AnimationRootNode? GetBlendPointNode(int point) => point >= 0 && point < _points.Count ? _points[point].Node : null;

    public void RemoveBlendPoint(int point)
    {
        if (point < 0 || point >= _points.Count) return;
        _points.RemoveAt(point);
        _triangles.RemoveAll(t => t.A == point || t.B == point || t.C == point);
        for (int i = 0; i < _triangles.Count; i++)
        {
            var t = _triangles[i];
            _triangles[i] = (t.A > point ? t.A - 1 : t.A, t.B > point ? t.B - 1 : t.B, t.C > point ? t.C - 1 : t.C);
        }
        if (_auto) Retriangulate();
        EmitSignal(AnimationNode.SignalName.TreeChanged);
    }

    public int GetBlendPointCount() => _points.Count;

    public void AddTriangle(int x, int y, int z, int atIndex = -1)
    {
        int n = _points.Count;
        if (x < 0 || y < 0 || z < 0 || x >= n || y >= n || z >= n || x == y || y == z || x == z) return;
        if (atIndex < 0 || atIndex >= _triangles.Count) _triangles.Add((x, y, z));
        else _triangles.Insert(atIndex, (x, y, z));
    }

    public int GetTrianglePoint(int triangle, int point)
    {
        if (triangle < 0 || triangle >= _triangles.Count) return -1;
        var t = _triangles[triangle];
        return point switch { 0 => t.A, 1 => t.B, 2 => t.C, _ => -1 };
    }

    public void RemoveTriangle(int triangle) { if (triangle >= 0 && triangle < _triangles.Count) _triangles.RemoveAt(triangle); }
    public int GetTriangleCount() => _triangles.Count;

    /// <summary>The triangles as a scene stores them: three point indices each.</summary>
    public int[] Triangles
    {
        get => _triangles.SelectMany(t => new[] { t.A, t.B, t.C }).ToArray();
        set
        {
            _triangles.Clear();
            for (int i = 0; i + 2 < value.Length; i += 3) _triangles.Add((value[i], value[i + 1], value[i + 2]));
        }
    }

    internal override bool TryDynamicSet(string name, Variant value)
    {
        if (!name.StartsWith("blend_point_", StringComparison.Ordinal)) return false;
        string[] p = name.Split('/');
        if (p.Length != 2 || !int.TryParse(p[0].AsSpan(12), out int i) || i < 0 || i > 1024) return false;
        while (_points.Count <= i) _points.Add((null, default));
        if (p[1] == "node") { _points[i] = (value.AsGodotObject() as AnimationRootNode, _points[i].Pos); return true; }
        if (p[1] == "pos") { _points[i] = (_points[i].Node, value.AsVector2()); return true; }
        return false;
    }

    /// <summary>The weight of each point at <paramref name="position"/>: the three corners of the triangle that holds it share it by barycentric
    /// weights; outside every triangle the nearest point of the nearest edge decides.</summary>
    public float[] GetWeights(Vector2 position)
    {
        var w = new float[_points.Count];
        foreach (var t in _triangles)
            if (BlendSpaceMath.Barycentric(position, _points[t.A].Pos, _points[t.B].Pos, _points[t.C].Pos, out float wa, out float wb, out float wc))
            {
                w[t.A] = wa; w[t.B] = wb; w[t.C] = wc;
                return w;
            }
        float bestD = float.MaxValue;
        int ia = -1, ib = -1;
        float bt = 0;
        foreach (var t in _triangles)
        {
            int[] c = { t.A, t.B, t.C };
            for (int j = 0; j < 3; j++)
            {
                int a = c[j], b = c[(j + 1) % 3];
                float s = BlendSpaceMath.ClosestOnSegment(position, _points[a].Pos, _points[b].Pos);
                Vector2 q = _points[a].Pos.Lerp(_points[b].Pos, s);
                float d = q.DistanceSquaredTo(position);
                if (d < bestD) { bestD = d; ia = a; ib = b; bt = s; }
            }
        }
        if (ia >= 0) { w[ia] = 1f - bt; w[ib] = bt; }
        return w;
    }

    internal override void CollectParameters(AnimationTree tree, string @base)
    {
        tree.DeclareParam(@base + "blend_position", Vector2.Zero);
        tree.DeclareState<Carry>(@base + "carry");
        for (int i = 0; i < _points.Count; i++) _points[i].Node?.CollectParameters(tree, @base + "point_" + i + "/");
    }

    internal override void ResetState(AnimationTree tree, string @base)
    {
        for (int i = 0; i < _points.Count; i++) _points[i].Node?.ResetState(tree, @base + "point_" + i + "/");
        tree.StateOf<Carry>(@base + "carry").Closest = -1;
    }

    internal override NodeTime Run(Frame f, double time, bool seek, bool external, bool testOnly)
    {
        if (_points.Count == 0) return default;
        Vector2 position = f.Tree.Param(f.Base + "blend_position", Vector2.Zero).AsVector2();
        float[] w;
        if (BlendMode == BlendModeEnum.Interpolated) w = GetWeights(position);
        else
        {
            w = new float[_points.Count];
            int best = 0;
            float bestD = float.MaxValue;
            for (int i = 0; i < _points.Count; i++)
            {
                float d = _points[i].Pos.DistanceSquaredTo(position);
                if (d < bestD) { bestD = d; best = i; }
            }
            w[best] = 1;
        }
        return BlendSpaceRun.Play(this, f, time, seek, external, testOnly, Sync, BlendMode == BlendModeEnum.DiscreteCarry, w, i => (_points[i].Node, f.Base + "point_" + i + "/"),
            () => f.Tree.StateOf<Carry>(f.Base + "carry"), c => c.Closest, (c, v) => c.Closest = v, c => c.Ratio, (c, v) => c.Ratio = v);
    }
}

// What both blend spaces do once they have their weights: play every point with its weight, carry the position over in the discrete-carry mode.
internal static class BlendSpaceRun
{
    internal static NodeTime Play<TState>(AnimationNode owner, Frame f, double time, bool seek, bool external, bool testOnly, bool sync, bool carry, float[] weights,
        Func<int, (AnimationRootNode? Node, string Base)> point, Func<TState> state, Func<TState, int> getClosest, Action<TState, int> setClosest,
        Func<TState, double> getRatio, Action<TState, double> setRatio)
    {
        int count = weights.Length;
        int closest = 0;
        for (int i = 1; i < count; i++) if (weights[i] > weights[closest]) closest = i;

        TState st = state();
        if (carry && !testOnly)
        {
            int last = getClosest(st);
            if (last >= 0 && last != closest)
            {
                // The new point continues from the fraction of its length the old one had reached.
                var (node, b) = point(closest);
                if (node != null)
                {
                    node.ResetState(f.Tree, b);
                    NodeTime len = owner.BlendChild(f, node, b, 0, false, false, 0, AnimationNode.FilterAction.Ignore, true, true);
                    owner.BlendChild(f, node, b, getRatio(st) * len.Length, true, false, 0, AnimationNode.FilterAction.Ignore, true, false);
                }
            }
            setClosest(st, closest);
        }

        NodeTime result = default;
        double bestWeight = -1;
        for (int i = 0; i < count; i++)
        {
            var (node, b) = point(i);
            if (node == null) continue;
            NodeTime nt = owner.BlendChild(f, node, b, time, seek, external, weights[i], AnimationNode.FilterAction.Ignore, sync, testOnly);
            if (weights[i] > bestWeight) { bestWeight = weights[i]; result = nt; }
        }
        if (carry && !testOnly && result.Length > 0) setRatio(st, Math.Clamp(result.Position / result.Length, 0.0, 1.0));
        return result;
    }
}
