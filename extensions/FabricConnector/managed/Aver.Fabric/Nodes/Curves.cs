// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Curve (a 1D function from points and tangents), Curve3D (a 3D path of bezier points with tilt, baked by distance) and Gradient (colours
// along 0..1), with Godot's sampling rules. Pure managed maths, in Godot's space (metres). docs/plans/fabric/G5_PARITY.md, A2.

namespace Aver.Fabric;

/// <summary>A function of one variable made of points with tangents (Godot's Curve): sampled with cubic segments, or from a baked table.</summary>
public class Curve : Resource
{
    public enum TangentMode { Free = 0, Linear = 1 }

    public new class SignalName : Resource.SignalName
    {
        public static readonly StringName RangeChanged = "range_changed";
        public static readonly StringName DomainChanged = "domain_changed";
    }

    private struct Point
    {
        public Vector2 Position;
        public float LeftTangent, RightTangent;
        public TangentMode LeftMode, RightMode;
    }

    private readonly List<Point> _points = new();
    private float[]? _baked;
    private int _bakeResolution = 100;
    private float _minValue, _maxValue = 1f, _minDomain, _maxDomain = 1f;

    public float MinValue { get => _minValue; set { _minValue = value; if (_maxValue < value) _maxValue = value; Dirty(); EmitSignal(SignalName.RangeChanged); } }
    public float MaxValue { get => _maxValue; set { _maxValue = value; if (_minValue > value) _minValue = value; Dirty(); EmitSignal(SignalName.RangeChanged); } }
    public float MinDomain { get => _minDomain; set { _minDomain = value; if (_maxDomain < value) _maxDomain = value; Dirty(); EmitSignal(SignalName.DomainChanged); } }
    public float MaxDomain { get => _maxDomain; set { _maxDomain = value; if (_minDomain > value) _minDomain = value; Dirty(); EmitSignal(SignalName.DomainChanged); } }

    public int BakeResolution { get => _bakeResolution; set { _bakeResolution = Math.Clamp(value, 2, 1000); Dirty(); } }

    public int PointCount
    {
        get => _points.Count;
        set
        {
            value = Math.Max(value, 0);
            while (_points.Count > value) _points.RemoveAt(_points.Count - 1);
            while (_points.Count < value) _points.Add(new Point { Position = new Vector2(_points.Count > 0 ? _points[^1].Position.X + 0.001f : _minDomain, 0) });
            Dirty();
        }
    }

    private void Dirty()
    {
        _baked = null;
        EmitSignal(Resource.SignalName.Changed);
    }

    // ---- points ----

    /// <summary>Adds a point, keeping the points in order of X; returns its index. A Linear mode makes the tangent follow the neighbouring point.</summary>
    public int AddPoint(Vector2 position, float leftTangent = 0, float rightTangent = 0, TangentMode leftMode = TangentMode.Free, TangentMode rightMode = TangentMode.Free)
    {
        var p = new Point { Position = position, LeftTangent = leftTangent, RightTangent = rightTangent, LeftMode = leftMode, RightMode = rightMode };
        int at = 0;
        while (at < _points.Count && _points[at].Position.X <= position.X) at++;
        _points.Insert(at, p);
        UpdateAutoTangents(at);
        Dirty();
        return at;
    }

    public void RemovePoint(int index)
    {
        if (index < 0 || index >= _points.Count) return;
        _points.RemoveAt(index);
        if (index > 0) UpdateAutoTangents(index - 1);
        else if (_points.Count > 0) UpdateAutoTangents(0);
        Dirty();
    }

    public void ClearPoints() { _points.Clear(); Dirty(); }

    public Vector2 GetPointPosition(int index) => index >= 0 && index < _points.Count ? _points[index].Position : default;

    public void SetPointValue(int index, float y)
    {
        if (index < 0 || index >= _points.Count) return;
        Point p = _points[index];
        p.Position.Y = y;
        _points[index] = p;
        UpdateAutoTangents(index);
        Dirty();
    }

    /// <summary>Moves a point along X; its place in the order may change. Returns its new index.</summary>
    public int SetPointOffset(int index, float offset)
    {
        if (index < 0 || index >= _points.Count) return -1;
        Point p = _points[index];
        _points.RemoveAt(index);
        p.Position.X = offset;
        int at = 0;
        while (at < _points.Count && _points[at].Position.X <= offset) at++;
        _points.Insert(at, p);
        UpdateAutoTangents(at);
        if (at > 0) UpdateAutoTangents(at - 1);
        Dirty();
        return at;
    }

    public float GetPointLeftTangent(int index) => index >= 0 && index < _points.Count ? _points[index].LeftTangent : 0;
    public float GetPointRightTangent(int index) => index >= 0 && index < _points.Count ? _points[index].RightTangent : 0;
    public TangentMode GetPointLeftMode(int index) => index >= 0 && index < _points.Count ? _points[index].LeftMode : TangentMode.Free;
    public TangentMode GetPointRightMode(int index) => index >= 0 && index < _points.Count ? _points[index].RightMode : TangentMode.Free;

    public void SetPointLeftTangent(int index, float tangent)
    {
        if (index < 0 || index >= _points.Count) return;
        Point p = _points[index];
        p.LeftTangent = tangent;
        p.LeftMode = TangentMode.Free;
        _points[index] = p;
        Dirty();
    }

    public void SetPointRightTangent(int index, float tangent)
    {
        if (index < 0 || index >= _points.Count) return;
        Point p = _points[index];
        p.RightTangent = tangent;
        p.RightMode = TangentMode.Free;
        _points[index] = p;
        Dirty();
    }

    public void SetPointLeftMode(int index, TangentMode mode)
    {
        if (index < 0 || index >= _points.Count) return;
        Point p = _points[index];
        p.LeftMode = mode;
        _points[index] = p;
        UpdateAutoTangents(index);
        Dirty();
    }

    public void SetPointRightMode(int index, TangentMode mode)
    {
        if (index < 0 || index >= _points.Count) return;
        Point p = _points[index];
        p.RightMode = mode;
        _points[index] = p;
        UpdateAutoTangents(index);
        Dirty();
    }

    // A Linear tangent is the slope towards the neighbouring point.
    private void UpdateAutoTangents(int i)
    {
        if (i < 0 || i >= _points.Count) return;
        Point p = _points[i];
        if (i > 0)
        {
            Point q = _points[i - 1];
            float slope = Slope(q.Position, p.Position);
            if (p.LeftMode == TangentMode.Linear) p.LeftTangent = slope;
            if (q.RightMode == TangentMode.Linear) { q.RightTangent = slope; _points[i - 1] = q; }
        }
        if (i + 1 < _points.Count)
        {
            Point q = _points[i + 1];
            float slope = Slope(p.Position, q.Position);
            if (p.RightMode == TangentMode.Linear) p.RightTangent = slope;
            if (q.LeftMode == TangentMode.Linear) { q.LeftTangent = slope; _points[i + 1] = q; }
        }
        _points[i] = p;
    }

    private static float Slope(Vector2 a, Vector2 b)
    {
        float dx = b.X - a.X;
        return Mathf.IsZeroApprox(dx) ? 0 : (b.Y - a.Y) / dx;
    }

    /// <summary>Removes points that sit on top of each other.</summary>
    public void CleanDupes()
    {
        bool removed = false;
        for (int i = _points.Count - 1; i > 0; i--)
            if (Mathf.IsZeroApprox(_points[i].Position.X - _points[i - 1].Position.X)) { _points.RemoveAt(i); removed = true; }
        if (removed) Dirty();
    }

    /// <summary>The points as a scene stores them: position, left tangent, right tangent, left mode, right mode, for each point.</summary>
    public Collections.Array Data
    {
        get
        {
            var a = new Collections.Array();
            foreach (Point p in _points) { a.Add(p.Position); a.Add(p.LeftTangent); a.Add(p.RightTangent); a.Add((int)p.LeftMode); a.Add((int)p.RightMode); }
            return a;
        }
        set
        {
            _points.Clear();
            for (int i = 0; i + 4 < value.Count; i += 5)
                _points.Add(new Point { Position = value[i].AsVector2(), LeftTangent = value[i + 1].AsSingle(), RightTangent = value[i + 2].AsSingle(), LeftMode = (TangentMode)value[i + 3].AsInt32(), RightMode = (TangentMode)value[i + 4].AsInt32() });
            Dirty();
        }
    }

    // ---- sampling ----

    // The index of the last point whose X is at or before the offset (0 before the first).
    private int IndexAt(float x)
    {
        int lo = 0, hi = _points.Count - 1;
        while (hi - lo > 1)
        {
            int m = (lo + hi) / 2;
            float a = _points[m].Position.X, b = _points[m + 1].Position.X;
            if (a < x && b < x) lo = m;
            else if (a > x) hi = m;
            else return m;
        }
        if (x > _points[hi].Position.X) return hi;
        return lo;
    }

    private static float Bezier(float start, float c1, float c2, float end, float t)
    {
        float omt = 1f - t;
        return start * omt * omt * omt + c1 * omt * omt * t * 3f + c2 * omt * t * t * 3f + end * t * t * t;
    }

    /// <summary>The value at <paramref name="offset"/>, from the points' cubic segments. Before the first point and after the last it holds their values.</summary>
    public float Sample(float offset)
    {
        if (_points.Count == 0) return 0;
        if (_points.Count == 1) return _points[0].Position.Y;
        int i = IndexAt(offset);
        if (i == _points.Count - 1) return _points[i].Position.Y;
        float local = offset - _points[i].Position.X;
        if (i == 0 && local <= 0) return _points[0].Position.Y;
        return SampleLocal(i, local);
    }

    private float SampleLocal(int index, float local)
    {
        Point a = _points[index], b = _points[index + 1];
        float d = b.Position.X - a.Position.X;
        if (Mathf.IsZeroApprox(d)) return b.Position.Y;
        local /= d;
        d /= 3f;
        float yac = a.Position.Y + d * a.RightTangent;
        float ybc = b.Position.Y - d * b.LeftTangent;
        return Bezier(a.Position.Y, yac, ybc, b.Position.Y, local);
    }

    /// <summary>Fills the table <see cref="SampleBaked"/> reads (done on demand, and again after any change).</summary>
    public void Bake()
    {
        _baked = new float[_bakeResolution];
        float range = _maxDomain - _minDomain;
        for (int i = 1; i < _bakeResolution - 1; i++)
            _baked[i] = Sample(_minDomain + range * i / (_bakeResolution - 1));
        if (_points.Count != 0)
        {
            _baked[0] = _points[0].Position.Y;
            _baked[^1] = _points[^1].Position.Y;
        }
    }

    /// <summary>The value at <paramref name="offset"/> from the baked table, linearly interpolated (cheaper than <see cref="Sample"/>).</summary>
    public float SampleBaked(float offset)
    {
        if (_baked == null) Bake();
        float[] b = _baked!;
        if (b.Length == 0) return _points.Count == 0 ? 0 : _points[0].Position.Y;
        if (b.Length == 1) return b[0];
        float range = _maxDomain - _minDomain;
        float fi = range > 0 ? (offset - _minDomain) / range * (b.Length - 1) : 0;
        int i = (int)MathF.Floor(fi);
        if (i < 0) { i = 0; fi = 0; }
        else if (i >= b.Length) { i = b.Length - 1; fi = 0; }
        if (i + 1 < b.Length) return Mathf.Lerp(b[i], b[i + 1], fi - i);
        return b[^1];
    }
}

/// <summary>One curve in 3D: bezier points with in and out handles and a tilt, baked into a polyline spaced <see cref="BakeInterval"/> apart
/// (Godot's Curve3D). All positions are in the curve's own space.</summary>
public class Curve3D : Resource
{
    private struct Pt
    {
        public Vector3 Position, In, Out;
        public float Tilt;
    }

    private readonly List<Pt> _pts = new();
    private float _bakeInterval = 0.2f;
    private bool _closed, _upVectorEnabled = true;
    private bool _dirty = true;
    private Vector3[] _bakedPoints = System.Array.Empty<Vector3>();
    private float[] _bakedTilts = System.Array.Empty<float>(), _bakedDist = System.Array.Empty<float>();
    private Vector3[] _bakedUp = System.Array.Empty<Vector3>();
    private float _bakedLength;

    public float BakeInterval { get => _bakeInterval; set { _bakeInterval = Math.Max(value, 0.001f); MarkDirty(); } }

    public bool UpVectorEnabled { get => _upVectorEnabled; set { _upVectorEnabled = value; MarkDirty(); } }

    public bool Closed { get => _closed; set { _closed = value; MarkDirty(); } }

    public int PointCount
    {
        get => _pts.Count;
        set
        {
            value = Math.Max(value, 0);
            while (_pts.Count > value) _pts.RemoveAt(_pts.Count - 1);
            while (_pts.Count < value) _pts.Add(default);
            MarkDirty();
        }
    }

    private void MarkDirty()
    {
        _dirty = true;
        EmitSignal(Resource.SignalName.Changed);
    }

    // ---- points ----

    /// <summary>Adds a point (the handles are relative to the position); <paramref name="index"/> -1 appends.</summary>
    public void AddPoint(Vector3 position, Vector3 inHandle = default, Vector3 outHandle = default, int index = -1)
    {
        var p = new Pt { Position = position, In = inHandle, Out = outHandle };
        if (index < 0 || index >= _pts.Count) _pts.Add(p);
        else _pts.Insert(index, p);
        MarkDirty();
    }

    public void RemovePoint(int index) { if (index >= 0 && index < _pts.Count) { _pts.RemoveAt(index); MarkDirty(); } }
    public void ClearPoints() { _pts.Clear(); MarkDirty(); }

    public Vector3 GetPointPosition(int index) => index >= 0 && index < _pts.Count ? _pts[index].Position : default;
    public Vector3 GetPointIn(int index) => index >= 0 && index < _pts.Count ? _pts[index].In : default;
    public Vector3 GetPointOut(int index) => index >= 0 && index < _pts.Count ? _pts[index].Out : default;
    public float GetPointTilt(int index) => index >= 0 && index < _pts.Count ? _pts[index].Tilt : 0;

    public void SetPointPosition(int index, Vector3 position) { if (Edit(index, (ref Pt p) => p.Position = position)) MarkDirty(); }
    public void SetPointIn(int index, Vector3 position) { if (Edit(index, (ref Pt p) => p.In = position)) MarkDirty(); }
    public void SetPointOut(int index, Vector3 position) { if (Edit(index, (ref Pt p) => p.Out = position)) MarkDirty(); }
    public void SetPointTilt(int index, float tilt) { if (Edit(index, (ref Pt p) => p.Tilt = tilt)) MarkDirty(); }

    private delegate void PtEdit(ref Pt p);

    private bool Edit(int index, PtEdit edit)
    {
        if (index < 0 || index >= _pts.Count) return false;
        Pt p = _pts[index];
        edit(ref p);
        _pts[index] = p;
        return true;
    }

    /// <summary>The points as a scene stores them: "points" (in, out and position of each point) and "tilts".</summary>
    public Collections.Dictionary Data
    {
        get
        {
            var d = new Collections.Dictionary();
            var pts = new Vector3[_pts.Count * 3];
            var tilts = new float[_pts.Count];
            for (int i = 0; i < _pts.Count; i++) { pts[i * 3] = _pts[i].In; pts[i * 3 + 1] = _pts[i].Out; pts[i * 3 + 2] = _pts[i].Position; tilts[i] = _pts[i].Tilt; }
            d["points"] = pts;
            d["tilts"] = tilts;
            return d;
        }
        set
        {
            _pts.Clear();
            Vector3[] pts = value.Get("points").AsVector3Array();
            float[] tilts = value.Get("tilts").AsFloat32Array();
            for (int i = 0; i + 2 < pts.Length; i += 3)
                _pts.Add(new Pt { In = pts[i], Out = pts[i + 1], Position = pts[i + 2], Tilt = i / 3 < tilts.Length ? tilts[i / 3] : 0 });
            MarkDirty();
        }
    }

    // ---- unbaked sampling ----

    private static Vector3 BezierAt(float t, Vector3 p0, Vector3 p1, Vector3 p2, Vector3 p3)
    {
        float omt = 1f - t;
        return p0 * (omt * omt * omt) + p1 * (omt * omt * t * 3f) + p2 * (omt * t * t * 3f) + p3 * (t * t * t);
    }

    private (Vector3 a, Vector3 b, Vector3 c, Vector3 d) Segment(int i)
    {
        Pt a = _pts[i], b = _pts[(i + 1) % _pts.Count];
        return (a.Position, a.Position + a.Out, b.Position + b.In, b.Position);
    }

    private int SegmentCount => _pts.Count < 2 ? 0 : _closed ? _pts.Count : _pts.Count - 1;

    /// <summary>The point on the bezier segment starting at <paramref name="idx"/>, at <paramref name="t"/> in 0..1.</summary>
    public Vector3 Sample(int idx, float t)
    {
        if (_pts.Count == 0) return default;
        if (idx >= SegmentCount) return _pts[Math.Min(idx, _pts.Count - 1)].Position;
        if (idx < 0) return _pts[0].Position;
        var (a, b, c, d) = Segment(idx);
        return BezierAt(Math.Clamp(t, 0f, 1f), a, b, c, d);
    }

    /// <summary>The point at <paramref name="fofs"/>: the integer part is the segment, the fraction the place along it.</summary>
    public Vector3 Samplef(float fofs)
    {
        if (fofs < 0) fofs = 0;
        int i = (int)MathF.Floor(fofs);
        return Sample(i, fofs - i);
    }

    /// <summary>The curve cut into straight pieces, adding points where a bezier bends by more than <paramref name="toleranceDegrees"/>.</summary>
    public Vector3[] Tessellate(int maxStages = 5, float toleranceDegrees = 4f)
    {
        var res = new List<Vector3>();
        if (_pts.Count == 0) return res.ToArray();
        if (_pts.Count == 1) return new[] { _pts[0].Position };
        float tol = Mathf.DegToRad(toleranceDegrees);
        for (int i = 0; i < SegmentCount; i++)
        {
            var (a, b, c, d) = Segment(i);
            if (i == 0) res.Add(a);
            Subdivide(a, b, c, d, 0f, 1f, 0, maxStages, tol, res);
            res.Add(d);
        }
        return res.ToArray();
    }

    private static void Subdivide(Vector3 a, Vector3 b, Vector3 c, Vector3 d, float t0, float t1, int depth, int maxDepth, float tol, List<Vector3> res)
    {
        if (depth >= maxDepth) return;
        float tm = (t0 + t1) * 0.5f;
        Vector3 p0 = BezierAt(t0, a, b, c, d), pm = BezierAt(tm, a, b, c, d), p1 = BezierAt(t1, a, b, c, d);
        Vector3 v0 = pm - p0, v1 = p1 - pm;
        if (v0.LengthSquared() < 1e-12f || v1.LengthSquared() < 1e-12f) return;
        if (v0.AngleTo(v1) <= tol) return;
        Subdivide(a, b, c, d, t0, tm, depth + 1, maxDepth, tol, res);
        res.Add(pm);
        Subdivide(a, b, c, d, tm, t1, depth + 1, maxDepth, tol, res);
    }

    /// <summary>The curve cut into pieces about <paramref name="lengthM"/> long.</summary>
    public Vector3[] TessellateEvenLength(int maxStages = 5, float lengthM = 0.2f)
    {
        var res = new List<Vector3>();
        if (_pts.Count == 0) return res.ToArray();
        if (_pts.Count == 1) return new[] { _pts[0].Position };
        res.Add(_pts[0].Position);
        Vector3 last = _pts[0].Position;
        for (int i = 0; i < SegmentCount; i++)
        {
            var (a, b, c, d) = Segment(i);
            int steps = 1 << Math.Clamp(maxStages, 1, 8);
            float t = 0;
            for (int s = 1; s <= steps * 4; s++)
            {
                float nt = (float)s / (steps * 4);
                Vector3 p = BezierAt(nt, a, b, c, d);
                if (last.DistanceTo(p) >= lengthM) { res.Add(p); last = p; }
                t = nt;
            }
            Vector3 end = _pts[(i + 1) % _pts.Count].Position;
            if (last.DistanceTo(end) > 1e-6f && (i == SegmentCount - 1 || last.DistanceTo(end) >= lengthM * 0.5f)) { res.Add(end); last = end; }
        }
        return res.ToArray();
    }

    // ---- baking ----

    private void EnsureBaked()
    {
        if (!_dirty) return;
        _dirty = false;
        Bake();
    }

    private void Bake()
    {
        _bakedPoints = System.Array.Empty<Vector3>();
        _bakedTilts = System.Array.Empty<float>();
        _bakedDist = System.Array.Empty<float>();
        _bakedUp = System.Array.Empty<Vector3>();
        _bakedLength = 0;
        if (_pts.Count == 0) return;
        if (_pts.Count == 1)
        {
            _bakedPoints = new[] { _pts[0].Position };
            _bakedTilts = new[] { _pts[0].Tilt };
            _bakedDist = new[] { 0f };
            _bakedUp = new[] { Vector3.Up };
            return;
        }

        var pos = new List<Vector3> { _pts[0].Position };
        var tilt = new List<float> { _pts[0].Tilt };
        Vector3 position = _pts[0].Position;
        for (int i = 0; i < SegmentCount; i++)
        {
            Pt a = _pts[i], b = _pts[(i + 1) % _pts.Count];
            Vector3 p0 = a.Position, p1 = a.Position + a.Out, p2 = b.Position + b.In, p3 = b.Position;
            const float step = 0.1f;
            float p = 0f;
            while (p < 1f)
            {
                float np = Math.Min(p + step, 1f);
                Vector3 npp = BezierAt(np, p0, p1, p2, p3);
                float d = position.DistanceTo(npp);
                if (d > _bakeInterval)
                {
                    // Bisect for the point exactly one interval from the last one.
                    float low = p, hi = np, mid = low + (hi - low) * 0.5f;
                    for (int j = 0; j < 10; j++)
                    {
                        npp = BezierAt(mid, p0, p1, p2, p3);
                        d = position.DistanceTo(npp);
                        if (_bakeInterval < d) hi = mid; else low = mid;
                        mid = low + (hi - low) * 0.5f;
                    }
                    position = npp;
                    p = mid;
                    pos.Add(position);
                    tilt.Add(Mathf.Lerp(a.Tilt, b.Tilt, mid));
                }
                else p = np;
            }
            position = p3;
            pos.Add(position);
            tilt.Add(b.Tilt);
        }

        int n = pos.Count;
        _bakedPoints = pos.ToArray();
        _bakedTilts = tilt.ToArray();
        _bakedDist = new float[n];
        for (int i = 1; i < n; i++) _bakedDist[i] = _bakedDist[i - 1] + _bakedPoints[i - 1].DistanceTo(_bakedPoints[i]);
        _bakedLength = _bakedDist[n - 1];

        // Up vectors: carry the previous one along the path by the smallest rotation that turns the old direction into the new.
        _bakedUp = new Vector3[n];
        if (_upVectorEnabled)
        {
            Vector3 prevFwd = Forward(0);
            Vector3 up = Vector3.Up;
            if (MathF.Abs(up.Dot(prevFwd)) > 0.999f) up = new Vector3(0, 0, -1);
            up = (up - prevFwd * up.Dot(prevFwd)).Normalized();
            _bakedUp[0] = up;
            for (int i = 1; i < n; i++)
            {
                Vector3 fwd = Forward(i);
                Vector3 axis = prevFwd.Cross(fwd);
                if (axis.LengthSquared() > 1e-12f)
                {
                    float angle = MathF.Acos(Math.Clamp(prevFwd.Dot(fwd), -1f, 1f));
                    up = up.Rotated(axis.Normalized(), angle);
                }
                up = (up - fwd * up.Dot(fwd)).Normalized();
                _bakedUp[i] = up;
                prevFwd = fwd;
            }
        }
        else for (int i = 0; i < n; i++) _bakedUp[i] = Vector3.Up;
    }

    // The direction of the baked polyline at point i (the last point repeats the direction before it).
    private Vector3 Forward(int i)
    {
        int n = _bakedPoints.Length;
        for (int k = Math.Min(i, n - 2); k >= 0; k--)
        {
            Vector3 d = _bakedPoints[k + 1] - _bakedPoints[k];
            if (d.LengthSquared() > 1e-12f) return d.Normalized();
        }
        return new Vector3(0, 0, -1);
    }

    /// <summary>How long the baked curve is.</summary>
    public float GetBakedLength() { EnsureBaked(); return _bakedLength; }

    public Vector3[] GetBakedPoints() { EnsureBaked(); return (Vector3[])_bakedPoints.Clone(); }
    public float[] GetBakedTilts() { EnsureBaked(); return (float[])_bakedTilts.Clone(); }
    public Vector3[] GetBakedUpVectors() { EnsureBaked(); return (Vector3[])_bakedUp.Clone(); }

    // The baked segment holding the offset: its first point and the fraction along it.
    private bool Locate(float offset, out int idx, out float frac)
    {
        idx = 0; frac = 0;
        int pc = _bakedPoints.Length;
        if (pc < 2) return false;
        int start = 0, end = pc;
        idx = (end + start) / 2;
        while (start < idx)
        {
            if (offset <= _bakedDist[idx]) end = idx; else start = idx;
            idx = (end + start) / 2;
        }
        float begin = _bakedDist[idx], stop = _bakedDist[idx + 1];
        float interval = stop - begin;
        frac = interval > 0 ? Math.Clamp((offset - begin) / interval, 0f, 1f) : 0f;
        return true;
    }

    /// <summary>The point <paramref name="offset"/> metres along the baked curve (clamped to its ends).</summary>
    public Vector3 SampleBaked(float offset, bool cubic = false)
    {
        EnsureBaked();
        int pc = _bakedPoints.Length;
        if (pc == 0) return default;
        if (pc == 1) return _bakedPoints[0];
        if (offset < 0) return _bakedPoints[0];
        if (offset >= _bakedLength) return _bakedPoints[pc - 1];
        Locate(offset, out int idx, out float frac);
        Vector3 a = _bakedPoints[idx], b = _bakedPoints[idx + 1];
        if (!cubic) return a.Lerp(b, frac);
        Vector3 pre = idx > 0 ? _bakedPoints[idx - 1] : a;
        Vector3 post = idx < pc - 2 ? _bakedPoints[idx + 2] : b;
        return a.CubicInterpolate(b, pre, post, frac);
    }

    /// <summary>The up vector at <paramref name="offset"/>, turned about the path direction by the interpolated tilt when <paramref name="applyTilt"/>.</summary>
    public Vector3 SampleBakedUpVector(float offset, bool applyTilt = false)
    {
        EnsureBaked();
        int pc = _bakedPoints.Length;
        if (pc == 0) return default;
        if (pc == 1) return _bakedUp[0];
        float o = Math.Clamp(offset, 0f, _bakedLength);
        Locate(o, out int idx, out float frac);
        Vector3 up = _bakedUp[idx].Lerp(_bakedUp[idx + 1], frac);
        up = up.LengthSquared() > 1e-12f ? up.Normalized() : _bakedUp[idx];
        if (applyTilt)
        {
            float tilt = Mathf.Lerp(_bakedTilts[idx], _bakedTilts[idx + 1], frac);
            Vector3 fwd = DirectionAt(o);
            up = up.Rotated(fwd, tilt);
        }
        return up;
    }

    // The path direction around an offset, from points a half interval either side.
    private Vector3 DirectionAt(float offset)
    {
        float e = Math.Max(_bakeInterval * 0.5f, 1e-4f);
        Vector3 a = SampleBaked(Math.Max(0f, offset - e)), b = SampleBaked(Math.Min(_bakedLength, offset + e));
        Vector3 d = b - a;
        return d.LengthSquared() > 1e-12f ? d.Normalized() : new Vector3(0, 0, -1);
    }

    /// <summary>The position and the orientation (-Z along the path, Y the up vector) at <paramref name="offset"/>.</summary>
    public Transform3D SampleBakedWithRotation(float offset, bool cubic = false, bool applyTilt = false)
    {
        EnsureBaked();
        Vector3 p = SampleBaked(offset, cubic);
        if (_bakedPoints.Length < 2) return new Transform3D(Basis.Identity, p);
        float o = Math.Clamp(offset, 0f, _bakedLength);
        Vector3 fwd = DirectionAt(o);
        Vector3 up = SampleBakedUpVector(o, applyTilt);
        return new Transform3D(Basis.LookingAt(fwd, up), p);
    }

    /// <summary>The point of the baked curve nearest to <paramref name="toPoint"/>.</summary>
    public Vector3 GetClosestPoint(Vector3 toPoint)
    {
        EnsureBaked();
        Closest(toPoint, out Vector3 best, out _);
        return best;
    }

    /// <summary>How far along the baked curve the point nearest to <paramref name="toPoint"/> is.</summary>
    public float GetClosestOffset(Vector3 toPoint)
    {
        EnsureBaked();
        Closest(toPoint, out _, out float off);
        return off;
    }

    private void Closest(Vector3 q, out Vector3 best, out float offset)
    {
        best = default;
        offset = 0;
        int pc = _bakedPoints.Length;
        if (pc == 0) return;
        best = _bakedPoints[0];
        if (pc == 1) return;
        float bestD = float.MaxValue;
        for (int i = 0; i + 1 < pc; i++)
        {
            Vector3 a = _bakedPoints[i], ab = _bakedPoints[i + 1] - a;
            float len2 = ab.LengthSquared();
            float t = len2 > 1e-12f ? Math.Clamp((q - a).Dot(ab) / len2, 0f, 1f) : 0f;
            Vector3 c = a + ab * t;
            float d = c.DistanceSquaredTo(q);
            if (d < bestD) { bestD = d; best = c; offset = _bakedDist[i] + MathF.Sqrt(len2) * t; }
        }
    }
}

/// <summary>Colours along 0..1 (Godot's Gradient).</summary>
public class Gradient : Resource
{
    public enum InterpolationModeEnum { Linear = 0, Constant = 1, Cubic = 2 }
    public enum ColorSpaceEnum { Srgb = 0, LinearSrgb = 1, Oklab = 2 }

    private readonly List<(float Offset, Color Color)> _pts = new() { (0f, new Color(0, 0, 0, 1)), (1f, new Color(1, 1, 1, 1)) };
    private InterpolationModeEnum _mode = InterpolationModeEnum.Linear;

    /// <summary>How colours between two points are chosen.</summary>
    public InterpolationModeEnum InterpolationMode { get => _mode; set { _mode = value; EmitSignal(Resource.SignalName.Changed); } }

    /// <summary>Stored; blending is done in sRGB whatever this is.</summary>
    public ColorSpaceEnum InterpolationColorSpace { get; set; } = ColorSpaceEnum.Srgb;

    public int PointCount => _pts.Count;

    public float[] Offsets
    {
        get => _pts.Select(p => p.Offset).ToArray();
        set
        {
            var colors = _pts.Select(p => p.Color).ToList();
            _pts.Clear();
            for (int i = 0; i < value.Length; i++) _pts.Add((value[i], i < colors.Count ? colors[i] : new Color(0, 0, 0, 1)));
            Sort();
        }
    }

    public Color[] Colors
    {
        get => _pts.Select(p => p.Color).ToArray();
        set
        {
            var offs = _pts.Select(p => p.Offset).ToList();
            _pts.Clear();
            for (int i = 0; i < value.Length; i++) _pts.Add((i < offs.Count ? offs[i] : 1f, value[i]));
            Sort();
        }
    }

    private void Sort()
    {
        _pts.Sort((a, b) => a.Offset.CompareTo(b.Offset));
        EmitSignal(Resource.SignalName.Changed);
    }

    public void AddPoint(float offset, Color color)
    {
        int at = 0;
        while (at < _pts.Count && _pts[at].Offset <= offset) at++;
        _pts.Insert(at, (offset, color));
        EmitSignal(Resource.SignalName.Changed);
    }

    public void RemovePoint(int point)
    {
        if (point < 0 || point >= _pts.Count || _pts.Count <= 1) return;
        _pts.RemoveAt(point);
        EmitSignal(Resource.SignalName.Changed);
    }

    public void SetOffset(int point, float offset)
    {
        if (point < 0 || point >= _pts.Count) return;
        _pts[point] = (Math.Clamp(offset, 0f, 1f), _pts[point].Color);
        Sort();
    }

    public float GetOffset(int point) => point >= 0 && point < _pts.Count ? _pts[point].Offset : 0;

    public void SetColor(int point, Color color)
    {
        if (point < 0 || point >= _pts.Count) return;
        _pts[point] = (_pts[point].Offset, color);
        EmitSignal(Resource.SignalName.Changed);
    }

    public Color GetColor(int point) => point >= 0 && point < _pts.Count ? _pts[point].Color : default;

    public void Reverse()
    {
        var copy = _pts.Select(p => (1f - p.Offset, p.Color)).Reverse().ToList();
        _pts.Clear();
        _pts.AddRange(copy);
        EmitSignal(Resource.SignalName.Changed);
    }

    /// <summary>The colour at <paramref name="offset"/>: the end colours outside the points, Godot's mode between them.</summary>
    public Color Sample(float offset)
    {
        int n = _pts.Count;
        if (n == 0) return new Color(0, 0, 0, 1);
        if (offset <= _pts[0].Offset) return _pts[0].Color;
        if (offset >= _pts[n - 1].Offset) return _pts[n - 1].Color;
        int hi = 1;
        while (hi < n - 1 && _pts[hi].Offset < offset) hi++;
        int lo = hi - 1;
        var (oa, ca) = _pts[lo];
        var (ob, cb) = _pts[hi];
        if (Mathf.IsZeroApprox(ob - oa)) return cb;
        float t = (offset - oa) / (ob - oa);
        switch (_mode)
        {
            case InterpolationModeEnum.Constant:
                return ca;
            case InterpolationModeEnum.Cubic:
            {
                Color pre = _pts[Math.Max(lo - 1, 0)].Color, post = _pts[Math.Min(hi + 1, n - 1)].Color;
                return new Color(Cubic(ca.R, cb.R, pre.R, post.R, t), Cubic(ca.G, cb.G, pre.G, post.G, t), Cubic(ca.B, cb.B, pre.B, post.B, t), Cubic(ca.A, cb.A, pre.A, post.A, t));
            }
            default:
                return ca.Lerp(cb, t);
        }
    }

    private static float Cubic(float from, float to, float pre, float post, float w) =>
        0.5f * (from * 2f + (-pre + to) * w + (2f * pre - 5f * from + 4f * to - post) * (w * w) + (-pre + 3f * from - 3f * to + post) * (w * w * w));
}
