// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// MathTests: Godot's math types against values that follow from their documented definitions (Godot space: +Y up, -Z forward,
// right-handed, rotations counter-clockwise about the axis seen from its tip).

using Aver.Fabric;

static class MathTests
{
    private const float Pi = MathF.PI;

    public static int RunAll() => T.Suite(() =>
    {
        T.Run("Vector3: arithmetic, cross, dot, length, normalized", Vector3Basics);
        T.Run("Vector3: lerp, slerp, move_toward, angle_to, rotated, reflect, bounce, slide, project", Vector3Functions);
        T.Run("Vector2: angle, rotated, cross, orthogonal", Vector2Basics);
        T.Run("Quaternion: euler, axis-angle, composition, slerp, arc, inverse", QuaternionBasics);
        T.Run("Basis: rotation axes, determinant, inverse, orthonormalize, scale", BasisBasics);
        T.Run("Basis: euler round trip in all six orders", BasisEuler);
        T.Run("Basis: looking_at points -Z at the target", BasisLookingAt);
        T.Run("Transform3D: xform, inverse, rotated vs rotated_local, scaled, looking_at, interpolate", TransformOps);
        T.Run("Plane, Aabb", PlaneAabb);
        T.Run("Rect2, Transform2D", Rect2D);
        T.Run("Color: html, hsv, srgb, blend", ColorOps);
        T.Run("Projection: perspective and orthogonal", ProjectionOps);
        T.Run("Mathf", MathfOps);
        T.Run("FabricSpace: golden values of the glTF map, round trips, rotation consistency", SpaceMap);
    });

    private static void Vector3Basics()
    {
        var x = new Vector3(1, 0, 0); var y = new Vector3(0, 1, 0);
        T.CheckVec("x cross y is z", new Vector3(0, 0, 1), x.Cross(y));
        T.CheckNum("dot", 32f, new Vector3(1, 2, 3).Dot(new Vector3(4, 5, 6)));
        T.CheckNum("length of (3,4,0)", 5f, new Vector3(3, 4, 0).Length());
        T.CheckVec("normalized", new Vector3(0.6f, 0.8f, 0), new Vector3(3, 4, 0).Normalized());
        T.Check("normalized zero stays zero", Vector3.Zero.Normalized() == Vector3.Zero);
        T.CheckVec("component-wise operators", new Vector3(5, 7, 9), new Vector3(1, 2, 3) + new Vector3(4, 5, 6));
        T.CheckVec("scalar multiply both sides", new Vector3(2, 4, 6), 2f * new Vector3(1, 2, 3));
        T.CheckVec("divide", new Vector3(0.5f, 1, 1.5f), new Vector3(1, 2, 3) / 2f);
        T.CheckVec("modulo", new Vector3(1, 1, 2), new Vector3(7, 4, 5) % 3f);
        T.Check("Forward is -Z and Up is +Y", Vector3.Forward == new Vector3(0, 0, -1) && Vector3.Up == new Vector3(0, 1, 0));
        T.Check("lexicographic compare", new Vector3(1, 2, 3) < new Vector3(1, 3, 0) && new Vector3(2, 0, 0) > new Vector3(1, 9, 9));
        T.CheckNum("distance_to", 5f, new Vector3(1, 1, 1).DistanceTo(new Vector3(4, 5, 1)));
        T.Check("indexer", new Vector3(7, 8, 9)[2] == 9f);
        var (a, b, c) = new Vector3(1, 2, 3);
        T.Check("deconstruct", a == 1f && b == 2f && c == 3f);
        T.Check("is_equal_approx", new Vector3(1, 1, 1).IsEqualApprox(new Vector3(1.0000001f, 1, 1)));
    }

    private static void Vector3Functions()
    {
        var x = new Vector3(1, 0, 0); var y = new Vector3(0, 1, 0);
        T.CheckVec("lerp", new Vector3(0.5f, 0.5f, 0), x.Lerp(y, 0.5f));
        T.CheckVec("slerp keeps length", new Vector3(MathF.Sqrt(0.5f), MathF.Sqrt(0.5f), 0), x.Slerp(y, 0.5f));
        T.CheckVec("slerp of colinear vectors lerps", new Vector3(0, 0, 0), x.Slerp(-x, 0.5f), 1e-3f);
        T.CheckNum("angle_to", Pi / 2, x.AngleTo(y));
        T.CheckNum("signed_angle_to about +Z", Pi / 2, x.SignedAngleTo(y, new Vector3(0, 0, 1)));
        T.CheckNum("signed_angle_to about -Z", -Pi / 2, x.SignedAngleTo(y, new Vector3(0, 0, -1)));
        T.CheckVec("rotated about Up by 90 degrees takes +X to -Z", new Vector3(0, 0, -1), x.Rotated(Vector3.Up, Pi / 2));
        T.CheckVec("reflect about the Up normal", new Vector3(-1, -1, 0), new Vector3(1, -1, 0).Reflect(Vector3.Up));
        T.CheckVec("bounce is minus reflect", new Vector3(1, 1, 0), new Vector3(1, -1, 0).Bounce(Vector3.Up));
        T.CheckVec("slide removes the normal part", new Vector3(1, 0, 0), new Vector3(1, -1, 0).Slide(Vector3.Up));
        T.CheckVec("project", new Vector3(2, 0, 0), new Vector3(2, 5, 0).Project(x));
        T.CheckVec("move_toward does not overshoot", new Vector3(3, 0, 0), new Vector3(2, 0, 0).MoveToward(new Vector3(3, 0, 0), 5f));
        T.CheckVec("move_toward steps", new Vector3(1, 0, 0), Vector3.Zero.MoveToward(new Vector3(10, 0, 0), 1f));
        T.CheckVec("limit_length", new Vector3(3, 0, 0), new Vector3(10, 0, 0).LimitLength(3f));
        T.CheckVec("snapped", new Vector3(1.5f, 0, -1.0f), new Vector3(1.4f, 0.2f, -0.9f).Snapped(new Vector3(0.5f, 1f, 1f)));
        T.CheckVec("sign has zero for zero", new Vector3(1, 0, -1), new Vector3(5, 0, -2).Sign());
        T.CheckVec("direction_to", new Vector3(0, 0, 1), Vector3.Zero.DirectionTo(new Vector3(0, 0, 9)));
        T.Check("max_axis_index", new Vector3(1, 5, 3).MaxAxisIndex() == Vector3.Axis.Y && new Vector3(1, 5, 3).MinAxisIndex() == Vector3.Axis.X);
        T.CheckVec("bezier at t=0.5", new Vector3(0.5f, 0, 0), Vector3.Zero.BezierInterpolate(Vector3.Zero, new Vector3(1, 0, 0), new Vector3(1, 0, 0), 0.5f));
    }

    private static void Vector2Basics()
    {
        T.CheckNum("angle of (0,1)", Pi / 2, new Vector2(0, 1).Angle());
        var r = new Vector2(1, 0).Rotated(Pi / 2);
        T.Check("rotated by 90 degrees", T.Near(r.X, 0) && T.Near(r.Y, 1));
        T.CheckNum("cross", 1f, new Vector2(1, 0).Cross(new Vector2(0, 1)));
        T.Check("orthogonal is (y, -x)", new Vector2(2, 3).Orthogonal() == new Vector2(3, -2));
        T.Check("Up is (0,-1) in 2D", Vector2.Up == new Vector2(0, -1));
        T.CheckNum("angle_to_point", Pi / 4, Vector2.Zero.AngleToPoint(new Vector2(2, 2)));
        T.Check("Vector2I to Vector2 is implicit", (Vector2)new Vector2I(2, 3) == new Vector2(2, 3));
    }

    private static void QuaternionBasics()
    {
        Quaternion qy = Quaternion.FromEuler(new Vector3(0, Pi / 2, 0));
        T.CheckVec("euler (0, 90deg, 0) takes +X to -Z", new Vector3(0, 0, -1), qy * new Vector3(1, 0, 0));
        T.Check("axis-angle gives the same rotation", T.Near(qy, new Quaternion(Vector3.Up, Pi / 2)));
        T.Check("y-up quaternion components", T.Near(new Quaternion(Vector3.Up, Pi / 2), new Quaternion(0, MathF.Sqrt(0.5f), 0, MathF.Sqrt(0.5f))));
        T.CheckVec("composition: two quarter turns are a half turn", new Vector3(-1, 0, 0), (qy * qy) * new Vector3(1, 0, 0));
        T.CheckVec("inverse undoes", new Vector3(1, 0, 0), qy.Inverse() * (qy * new Vector3(1, 0, 0)));
        T.CheckVec("vector times quaternion is the inverse rotation", new Vector3(1, 0, 0), new Vector3(0, 0, -1) * qy);
        T.Check("slerp halfway is an eighth turn", T.Near(Quaternion.Identity.Slerp(qy, 0.5f), new Quaternion(Vector3.Up, Pi / 4)));
        T.CheckNum("angle_to", Pi / 2, Quaternion.Identity.AngleTo(qy));
        T.CheckNum("get_angle", Pi / 2, qy.GetAngle());
        T.CheckVec("get_axis", Vector3.Up, qy.GetAxis());
        T.CheckVec("arc between +X and +Y", new Vector3(0, 1, 0), new Quaternion(new Vector3(1, 0, 0), new Vector3(0, 1, 0)) * new Vector3(1, 0, 0));
        Quaternion qa = Quaternion.FromEuler(new Vector3(0.3f, -0.7f, 1.1f));
        T.Check("quaternion from euler is a unit", qa.IsNormalized());
        T.CheckVec("euler round trip (YXZ)", new Vector3(0.3f, -0.7f, 1.1f), qa.GetEuler(), 1e-4f);
        T.Check("quaternion from euler matches the basis from euler",
            T.Near(qa, Basis.FromEuler(new Vector3(0.3f, -0.7f, 1.1f)).GetRotationQuaternion()));
        T.Check("quaternion to basis to quaternion", T.Near(qa, new Quaternion(new Basis(qa))));
        T.CheckVec("basis from quaternion rotates the same", qa * new Vector3(1, 2, 3), new Basis(qa) * new Vector3(1, 2, 3));
    }

    private static void BasisBasics()
    {
        var rot = new Basis(Vector3.Up, Pi / 2);
        T.CheckVec("rotation about Up maps +X to -Z", new Vector3(0, 0, -1), rot * new Vector3(1, 0, 0));
        T.CheckVec("and +Z to +X", new Vector3(1, 0, 0), rot * new Vector3(0, 0, 1));
        T.CheckNum("determinant of a rotation", 1f, rot.Determinant());
        T.CheckNum("determinant of a mirror", -1f, Basis.FlipX.Determinant());
        T.Check("inverse times self is the identity", (rot.Inverse() * rot).IsEqualApprox(Basis.Identity));
        T.Check("transposed equals inverse for a rotation", rot.Transposed().IsEqualApprox(rot.Inverse()));
        var scaled = Basis.FromScale(new Vector3(2, 3, 4));
        T.CheckVec("get_scale", new Vector3(2, 3, 4), scaled.GetScale());
        T.CheckVec("get_scale of a mirrored basis is negated (Godot takes the determinant's sign)", new Vector3(-2, -3, -4), (Basis.FlipX * scaled).GetScale(), 1e-4f);
        Basis tilted = new Basis(new Vector3(1, 2, 3).Normalized(), 0.8f) * scaled;
        T.Check("orthonormalized removes the scale", tilted.Orthonormalized().IsEqualApprox(new Basis(new Vector3(1, 2, 3).Normalized(), 0.8f)));
        T.Check("columns are the transformed axes", rot.Column0.IsEqualApprox(new Vector3(0, 0, -1)) && rot.X.IsEqualApprox(rot.Column0));
        T.Check("indexer is [column, row]", T.Near(rot[0, 2], -1f));
        T.CheckVec("scaled scales global axes", new Vector3(2, 0, 0), Basis.Identity.Scaled(new Vector3(2, 3, 4)) * new Vector3(1, 0, 0));
        T.Check("scaled_local after a rotation scales the object's own axes",
            (rot.ScaledLocal(new Vector3(2, 1, 1)) * new Vector3(1, 0, 0)).IsEqualApprox(new Vector3(0, 0, -2)));
        T.Check("rotated applies on the left (global axis)", rot.Rotated(Vector3.Right, Pi / 2).IsEqualApprox(new Basis(Vector3.Right, Pi / 2) * rot));
        T.Check("quaternion extraction from a rotation with scale", T.Near((new Basis(Vector3.Up, 0.5f) * scaled).GetRotationQuaternion(), new Quaternion(Vector3.Up, 0.5f)));
    }

    private static void BasisEuler()
    {
        var e = new Vector3(0.3f, 0.5f, -0.4f);
        foreach (EulerOrder order in Enum.GetValues<EulerOrder>())
        {
            Vector3 back = Basis.FromEuler(e, order).GetEuler(order);
            T.CheckVec($"order {order}: from_euler then get_euler returns the angles", e, back, 2e-4f);
        }
        // The order decides the composition: Godot's default YXZ applies Z first, then X, then Y.
        Basis b = Basis.FromEuler(new Vector3(0.4f, 0.6f, 0.2f));
        Basis expected = new Basis(Vector3.Up, 0.6f) * new Basis(Vector3.Right, 0.4f) * new Basis(new Vector3(0, 0, 1), 0.2f);
        T.Check("default order is Ry * Rx * Rz", b.IsEqualApprox(expected));
        Basis xyz = Basis.FromEuler(new Vector3(0.4f, 0.6f, 0.2f), EulerOrder.Xyz);
        T.Check("XYZ order is Rx * Ry * Rz", xyz.IsEqualApprox(new Basis(Vector3.Right, 0.4f) * new Basis(Vector3.Up, 0.6f) * new Basis(new Vector3(0, 0, 1), 0.2f)));
        T.CheckVec("a pure X rotation reads back as itself", new Vector3(1.0f, 0, 0), Basis.FromEuler(new Vector3(1.0f, 0, 0)).GetEuler(), 1e-5f);
        T.CheckVec("a large turn about Y keeps its angle", new Vector3(0, 2.5f, 0), Basis.FromEuler(new Vector3(0, 2.5f, 0)).GetEuler(), 1e-4f);
    }

    private static void BasisLookingAt()
    {
        T.Check("looking along -Z is the identity", Basis.LookingAt(new Vector3(0, 0, -1)).IsEqualApprox(Basis.Identity));
        T.CheckVec("looking along +X sends -Z to +X", new Vector3(1, 0, 0), Basis.LookingAt(new Vector3(1, 0, 0)) * new Vector3(0, 0, -1));
        T.CheckVec("the up axis follows the up vector", Vector3.Up, Basis.LookingAt(new Vector3(1, 0, 0)) * Vector3.Up);
        T.CheckVec("use_model_front looks along +Z", new Vector3(1, 0, 0), Basis.LookingAt(new Vector3(1, 0, 0), null, true) * new Vector3(0, 0, 1));
        bool threw = false;
        try { Basis.LookingAt(Vector3.Up, Vector3.Up); } catch (ArgumentException) { threw = true; }
        T.Check("a target parallel to up is refused", threw);
    }

    private static void TransformOps()
    {
        var t = new Transform3D(new Basis(Vector3.Up, Pi / 2), new Vector3(1, 2, 3));
        T.CheckVec("transform point", new Vector3(1, 2, 2), t * new Vector3(1, 0, 0));   // rotate (1,0,0) to (0,0,-1), add the origin
        T.CheckVec("inverse transform with vector * transform", new Vector3(1, 0, 0), new Vector3(1, 2, 2) * t);
        T.Check("affine_inverse * transform is identity", (t.AffineInverse() * t).IsEqualApprox(Transform3D.Identity));
        T.Check("inverse equals affine_inverse for an orthonormal transform", t.Inverse().IsEqualApprox(t.AffineInverse()));
        Transform3D tr = Transform3D.Identity.Translated(new Vector3(1, 2, 3)).Rotated(Vector3.Up, Pi / 2);
        T.CheckVec("rotated rotates the origin too", new Vector3(3, 2, -1), tr.Origin);
        Transform3D tl = Transform3D.Identity.Translated(new Vector3(1, 2, 3)).RotatedLocal(Vector3.Up, Pi / 2);
        T.CheckVec("rotated_local keeps the origin", new Vector3(1, 2, 3), tl.Origin);
        T.CheckVec("scaled scales the origin", new Vector3(2, 6, 12), Transform3D.Identity.Translated(new Vector3(1, 2, 3)).Scaled(new Vector3(2, 3, 4)).Origin);
        T.CheckVec("scaled_local leaves the origin", new Vector3(1, 2, 3), Transform3D.Identity.Translated(new Vector3(1, 2, 3)).ScaledLocal(new Vector3(2, 3, 4)).Origin);
        T.CheckVec("translated_local moves along the basis", new Vector3(0, 0, -5), new Transform3D(new Basis(Vector3.Up, Pi / 2), Vector3.Zero).TranslatedLocal(new Vector3(5, 0, 0)).Origin);
        Transform3D look = new Transform3D(Basis.Identity, new Vector3(0, 0, 0)).LookingAt(new Vector3(0, 0, 10) , Vector3.Up);
        T.CheckVec("looking_at +Z turns -Z to +Z", new Vector3(0, 0, 1), look.Basis * new Vector3(0, 0, -1));
        Transform3D mid = Transform3D.Identity.InterpolateWith(new Transform3D(new Basis(Vector3.Up, Pi / 2), new Vector3(2, 0, 0)), 0.5f);
        T.CheckVec("interpolate_with lerps the origin", new Vector3(1, 0, 0), mid.Origin);
        T.Check("and slerps the rotation", T.Near(mid.Basis.GetRotationQuaternion(), new Quaternion(Vector3.Up, Pi / 4)));
        T.Check("transform times transform composes", (t * t).Origin.IsEqualApprox(t * t.Origin));
        Aabb moved = Transform3D.Identity.Translated(new Vector3(1, 0, 0)) * new Aabb(new Vector3(-1, -1, -1), new Vector3(2, 2, 2));
        T.CheckVec("transforming an Aabb moves it", new Vector3(0, -1, -1), moved.Position);
    }

    private static void PlaneAabb()
    {
        var p = new Plane(Vector3.Up, 2f);
        T.CheckNum("distance_to above", 3f, p.DistanceTo(new Vector3(0, 5, 0)));
        T.Check("is_point_over", p.IsPointOver(new Vector3(0, 3, 0)) && !p.IsPointOver(new Vector3(0, 1, 0)));
        T.CheckVec("project", new Vector3(4, 2, 4), p.Project(new Vector3(4, 9, 4)));
        T.Check("intersects_ray from above", p.IntersectsRay(new Vector3(1, 10, 1), Vector3.Down) is Vector3 hit && T.Near(hit, new Vector3(1, 2, 1)));
        T.Check("a ray parallel to the plane misses", p.IntersectsRay(Vector3.Zero, Vector3.Right) is null);
        T.Check("three planes meet in a point", new Plane(Vector3.Right, 1).Intersect3(new Plane(Vector3.Up, 2), new Plane(new Vector3(0, 0, 1), 3)) is Vector3 m && T.Near(m, new Vector3(1, 2, 3)));
        T.CheckVec("plane from three points (clockwise seen from the normal)", Vector3.Up, new Plane(new Vector3(0, 0, 0), new Vector3(1, 0, 0), new Vector3(0, 0, 1)).Normal);

        var a = new Aabb(new Vector3(0, 0, 0), new Vector3(2, 2, 2));
        var b = new Aabb(new Vector3(1, 1, 1), new Vector3(2, 2, 2));
        T.Check("intersects", a.Intersects(b));
        T.Check("intersection", a.Intersection(b) == new Aabb(new Vector3(1, 1, 1), new Vector3(1, 1, 1)));
        T.Check("merge", a.Merge(b) == new Aabb(Vector3.Zero, new Vector3(3, 3, 3)));
        T.Check("encloses", a.Merge(b).Encloses(a) && !a.Encloses(b));
        T.CheckVec("center", new Vector3(1, 1, 1), a.GetCenter());
        T.CheckNum("volume", 8f, a.GetVolume());
        T.Check("expand", a.Expand(new Vector3(5, 0, 0)).End == new Vector3(5, 2, 2));
        T.Check("has_point", a.HasPoint(new Vector3(1, 1, 1)) && !a.HasPoint(new Vector3(3, 1, 1)));
        T.Check("grow", a.Grow(1) == new Aabb(new Vector3(-1, -1, -1), new Vector3(4, 4, 4)));
        T.Check("longest axis", new Aabb(Vector3.Zero, new Vector3(1, 5, 2)).GetLongestAxisIndex() == Vector3.Axis.Y);
    }

    private static void Rect2D()
    {
        var r = new Rect2(0, 0, 4, 2);
        T.Check("area and center", r.GetArea() == 8 && r.GetCenter() == new Vector2(2, 1));
        T.Check("has_point excludes the far edge", r.HasPoint(new Vector2(0, 0)) && !r.HasPoint(new Vector2(4, 1)));
        T.Check("merge", r.Merge(new Rect2(3, 1, 4, 4)) == new Rect2(0, 0, 7, 5));
        T.Check("intersection", r.Intersection(new Rect2(2, 1, 4, 4)) == new Rect2(2, 1, 2, 1));
        T.Check("grow", r.Grow(1) == new Rect2(-1, -1, 6, 4));
        var t = new Transform2D(Pi / 2, new Vector2(10, 0));
        var p = t * new Vector2(1, 0);
        T.Check("Transform2D rotates then translates", T.Near(p.X, 10) && T.Near(p.Y, 1));
        var back = t.AffineInverse() * p;
        T.Check("affine_inverse undoes it", T.Near(back.X, 1) && T.Near(back.Y, 0));
        T.CheckNum("rotation", Pi / 2, t.GetRotation());
        T.Check("scale", new Transform2D(0, new Vector2(2, 3), 0, Vector2.Zero).GetScale() == new Vector2(2, 3));
    }

    private static void ColorOps()
    {
        Color c = Color.FromHtml("#ff8000");
        T.Check("from_html", T.Near(c.R, 1f) && T.Near(c.G, 128f / 255f) && T.Near(c.B, 0f) && c.A == 1f);
        T.Check("to_html without alpha", c.ToHtml(false) == "ff8000");
        T.Check("to_html with alpha", c.ToHtml() == "ff8000ff");
        T.Check("short form #f80", T.Near(Color.FromHtml("f80").G, 136f / 255f));
        T.Check("with alpha", T.Near(Color.FromHtml("#ff000080").A, 128f / 255f));
        T.Check("named colours", Colors.Red == new Color(1, 0, 0) && new Color("red") == Colors.Red);
        Color hsv = Color.FromHsv(0f, 1f, 1f);
        T.Check("hsv red", hsv == new Color(1, 0, 0) && hsv.H == 0f && hsv.S == 1f && hsv.V == 1f);
        Color g = Color.FromHsv(1f / 3f, 1f, 1f);
        T.Check("hsv green", T.Near(g.G, 1f) && T.Near(g.R, 0f) && T.Near(g.H, 1f / 3f));
        T.Check("lightened and darkened", Colors.Black.Lightened(0.5f) == new Color(0.5f, 0.5f, 0.5f) && new Color(1, 1, 1).Darkened(0.25f) == new Color(0.75f, 0.75f, 0.75f));
        T.Check("inverted keeps alpha", new Color(1, 0, 0, 0.5f).Inverted() == new Color(0, 1, 1, 0.5f));
        T.CheckNum("srgb to linear of 0.5", 0.21404f, new Color(0.5f, 0.5f, 0.5f).SrgbToLinear().R, 1e-4f);
        T.CheckNum("and back", 0.5f, new Color(0.5f, 0.5f, 0.5f).SrgbToLinear().LinearToSrgb().R, 1e-4f);
        T.Check("blend over an opaque colour covers it", new Color(0, 0, 1, 1).Blend(new Color(1, 0, 0, 1)) == new Color(1, 0, 0, 1));
        T.Check("blend of a half-transparent colour over opaque", new Color(0, 0, 1, 1).Blend(new Color(1, 0, 0, 0.5f)).IsEqualApprox(new Color(0.5f, 0, 0.5f, 1)));
        T.Check("8-bit channels", new Color(1, 0.5f, 0).G8 == 128 && Color.Color8(255, 0, 0) == new Color(1, 0, 0));
        T.Check("rgba32", Colors.Red.ToRgba32() == 0xFF0000FFu && Color.FromRgba32(0x00FF00FFu) == new Color(0, 1, 0, 1));
        T.CheckNum("luminance of white", 1f, Colors.White.Luminance());
        T.Check("bad code is refused", !Color.HtmlIsValid("#12") && Color.HtmlIsValid("#123"));
    }

    private static void ProjectionOps()
    {
        Projection p = Projection.CreatePerspective(90f, 2f, 0.1f, 100f);
        T.CheckNum("fov", 90f, p.GetFov(), 1e-3f);
        T.CheckNum("aspect", 2f, p.GetAspect(), 1e-4f);
        T.CheckNum("z_near", 0.1f, p.GetZNear(), 1e-4f);
        T.CheckNum("z_far", 100f, p.GetZFar(), 1e-2f);
        T.Check("inverse times self is the identity", (p.Inverse() * p) is Projection id && T.Near(id.X.X, 1) && T.Near(id.Y.Y, 1) && T.Near(id.Z.Z, 1) && T.Near(id.W.W, 1) && T.Near(id.W.Z, 0, 1e-3f));
        Vector4 clip = p * new Vector4(0, 0, -0.1f, 1);
        T.CheckNum("a point on the near plane lands at -w in clip z", -clip.W, clip.Z, 1e-3f);
        Projection o = Projection.CreateOrthogonal(-1, 1, -1, 1, 0.1f, 10f);
        T.Check("orthogonal is flagged", o.IsOrthogonal() && !p.IsOrthogonal());
        T.CheckNum("orthogonal x scale", 1f, o.X.X);
        T.CheckNum("determinant of the identity", 1f, Projection.Identity.Determinant());
    }

    private static void MathfOps()
    {
        T.Check("pos_mod of a negative", Mathf.PosMod(-1, 3) == 2 && T.Near(Mathf.PosMod(-1.5f, 3f), 1.5f));
        T.Check("wrap int", Mathf.Wrap(7, 0, 5) == 2 && Mathf.Wrap(-1, 0, 5) == 4);
        T.CheckNum("wrap float", 0.5f, Mathf.Wrap(5.5f, 0f, 5f));
        T.Check("round half away from zero", Mathf.RoundToInt(2.5f) == 3 && Mathf.RoundToInt(-2.5f) == -3 && Mathf.Round(0.5f) == 1f);
        T.Check("sign", Mathf.Sign(0f) == 0 && Mathf.Sign(-2f) == -1 && Mathf.Sign(3) == 1);
        T.CheckNum("snapped", 7.0f, Mathf.Snapped(7.2f, 0.5f));
        T.CheckNum("lerp_angle takes the short way round", Pi, Mathf.LerpAngle(Pi - 0.2f, -Pi + 0.2f, 0.5f), 1e-3f);
        T.CheckNum("angle_difference", 0.4f, Mathf.AngleDifference(Pi - 0.2f, -Pi + 0.2f), 1e-4f);
        T.CheckNum("inverse_lerp", 0.25f, Mathf.InverseLerp(0f, 8f, 2f));
        T.CheckNum("remap", 15f, Mathf.Remap(0.5f, 0f, 1f, 10f, 20f));
        T.CheckNum("move_toward", 3f, Mathf.MoveToward(0f, 10f, 3f));
        T.CheckNum("smoothstep midpoint", 0.5f, Mathf.SmoothStep(0f, 1f, 0.5f));
        T.CheckNum("ease in", 0.25f, Mathf.Ease(0.5f, 2f));
        T.CheckNum("deg to rad", Pi, Mathf.DegToRad(180f));
        T.Check("is_equal_approx and is_zero_approx", Mathf.IsEqualApprox(1f, 1.0000001f) && Mathf.IsZeroApprox(1e-7f) && !Mathf.IsZeroApprox(1e-3f));
        T.Check("nearest_po2", Mathf.NearestPo2(33) == 64 && Mathf.NearestPo2(64) == 64);
        T.Check("clamp and constants", Mathf.Clamp(5, 0, 3) == 3 && T.Near(Mathf.Tau, 2 * Mathf.Pi));
    }

    // The glTF importer's map, written out: (x, y, z) -> (-z, x, y), metres to centimetres, quaternion (x, y, z, w) -> (z, -x, -y, w).
    private static void SpaceMap()
    {
        var p = FabricSpace.ToAverPosition(new Vector3(1, 2, 3));
        T.Check("position (1,2,3) m becomes (-300, 100, 200) cm", p.X == -300f && p.Y == 100f && p.Z == 200f);
        var f = FabricSpace.ToAverDirection(Vector3.Forward);
        T.Check("Godot forward -Z is Aver +X", f.X == 1f && f.Y == 0f && f.Z == 0f);
        var u = FabricSpace.ToAverDirection(Vector3.Up);
        T.Check("Godot up +Y is Aver +Z", u.X == 0f && u.Y == 0f && u.Z == 1f);
        var r = FabricSpace.ToAverDirection(Vector3.Right);
        T.Check("Godot right +X is Aver +Y", r.X == 0f && r.Y == 1f && r.Z == 0f);
        var q = FabricSpace.ToAverRotation(new Quaternion(0.1f, 0.2f, 0.3f, 0.9f));
        T.Check("quaternion (x,y,z,w) becomes (z,-x,-y,w)", q.X == 0.3f && q.Y == -0.1f && q.Z == -0.2f && q.W == 0.9f);
        var s = FabricSpace.ToAverScale(new Vector3(1, 2, 3));
        T.Check("scale (x,y,z) becomes (z,x,y)", s.X == 3f && s.Y == 1f && s.Z == 2f);

        var v = new Vector3(1.25f, -2.5f, 7f);
        T.CheckVec("position round trip", v, FabricSpace.FromAverPosition(FabricSpace.ToAverPosition(v)));
        T.CheckVec("direction round trip", v, FabricSpace.FromAverDirection(FabricSpace.ToAverDirection(v)));
        T.CheckVec("scale round trip", v, FabricSpace.FromAverScale(FabricSpace.ToAverScale(v)));
        Quaternion qq = Quaternion.FromEuler(new Vector3(0.3f, -0.7f, 1.1f));
        T.Check("rotation round trip", T.Near(qq, FabricSpace.FromAverRotation(FabricSpace.ToAverRotation(qq))));

        // The map must carry a rotation of vectors: rotating in Aver space by the converted quaternion equals converting the rotated vector.
        foreach (var euler in new[] { new Vector3(0, Pi / 2, 0), new Vector3(0.4f, 0, 0), new Vector3(0, 0, 0.9f), new Vector3(0.3f, -0.7f, 1.1f) })
        {
            Quaternion g = Quaternion.FromEuler(euler);
            var v0 = new Vector3(1, 2, 3);
            var aver = AverRotate(FabricSpace.ToAverRotation(g), FabricSpace.ToAverDirection(v0));
            var want = FabricSpace.ToAverDirection(g * v0);
            T.Check($"rotation {euler} commutes with the axis map", T.Near(aver.X, want.X) && T.Near(aver.Y, want.Y) && T.Near(aver.Z, want.Z), $"{aver.X},{aver.Y},{aver.Z} vs {want.X},{want.Y},{want.Z}");
        }

        // A whole transform.
        var t = new Transform3D(Basis.FromEuler(new Vector3(0.2f, 0.9f, -0.3f)) * Basis.FromScale(new Vector3(1, 2, 3)), new Vector3(4, 5, 6));
        FabricSpace.ToAver(t, out var pos, out var rot, out var scale);
        Transform3D back = FabricSpace.FromAver(pos, rot, scale);
        T.Check("transform round trip", T.Near(back.Origin, t.Origin) && T.Near(back.Basis.Column0, t.Basis.Column0) &&
                                        T.Near(back.Basis.Column1, t.Basis.Column1) && T.Near(back.Basis.Column2, t.Basis.Column2));
        T.Check("one centimetre is a hundredth of a metre", FabricSpace.FromAverLength(250f) == 2.5f && FabricSpace.ToAverLength(2.5f) == 250f);
    }

    // Aver's rotation of a vector by a unit quaternion, as Entity.Rotate does it.
    private static Aver.Scene.Vec3 AverRotate(Aver.Scene.Quat q, Aver.Scene.Vec3 v)
    {
        float tx = 2f * (q.Y * v.Z - q.Z * v.Y);
        float ty = 2f * (q.Z * v.X - q.X * v.Z);
        float tz = 2f * (q.X * v.Y - q.Y * v.X);
        return new Aver.Scene.Vec3(
            v.X + q.W * tx + (q.Y * tz - q.Z * ty),
            v.Y + q.W * ty + (q.Z * tx - q.X * tz),
            v.Z + q.W * tz + (q.X * ty - q.Y * tx));
    }
}
