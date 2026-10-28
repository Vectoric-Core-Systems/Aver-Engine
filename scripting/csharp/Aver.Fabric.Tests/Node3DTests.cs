// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Node3DTests: the transform API in Godot space, the node classes' defaults, and the physics signal routing (the engine's
// contact and area events reach body_entered / area_entered through FabricPhysics, tested here without a physics world).

using Aver.Fabric;

static class Node3DTests
{
    private const float Pi = MathF.PI;

    public static int RunAll() => T.Suite(() =>
    {
        T.Run("Node3D: local transform properties", LocalTransform);
        T.Run("Node3D: global transform composes through parents", GlobalTransform);
        T.Run("Node3D: look_at, translate, rotate, to_local, to_global", Movement);
        T.Run("Node3D: visibility propagates and signals", Visibility);
        T.Run("Node3D: top_level and reparent keep the global transform", TopLevelAndReparent);
        T.Run("Node3D: what the engine is given at spawn (Godot numbers to Aver space)", EngineValues);
        T.Run("classes: the converter's node types and their Godot defaults", ClassDefaults);
        T.Run("physics: area and body signals", PhysicsSignals);
        T.Run("physics: nodes without a world do not throw", PhysicsWithoutWorld);
    });

    private static void LocalTransform()
    {
        var n = new Node3D();
        T.Check("starts at the identity", n.Position == Vector3.Zero && n.Scale == Vector3.One && n.Transform == Transform3D.Identity);
        n.Position = new Vector3(1, 2, 3);
        T.Check("position", n.Position == new Vector3(1, 2, 3) && n.Transform.Origin == new Vector3(1, 2, 3));
        n.Rotation = new Vector3(0, Pi / 2, 0);
        T.CheckVec("rotation sets the basis (+X goes to -Z)", new Vector3(0, 0, -1), n.Basis * new Vector3(1, 0, 0));
        T.Check("rotation reads back exactly as set", n.Rotation == new Vector3(0, Pi / 2, 0));
        T.CheckVec("rotation degrees", new Vector3(0, 90, 0), n.RotationDegrees, 1e-3f);
        n.RotationDegrees = new Vector3(30, 40, 50);
        T.CheckVec("degrees round trip", new Vector3(30, 40, 50), n.RotationDegrees, 1e-3f);
        n.Scale = new Vector3(2, 3, 4);
        T.CheckVec("scale is kept apart from rotation", new Vector3(2, 3, 4), n.Basis.GetScale());
        T.CheckVec("and the rotation survives it", new Vector3(30, 40, 50) * (Pi / 180f), n.Rotation, 1e-4f);
        var q = new Quaternion(Vector3.Up, 1f);
        n.Quaternion = q;
        T.Check("quaternion", T.Near(n.Quaternion, q) && T.Near(n.Basis.GetRotationQuaternion(), q));
        T.CheckVec("and the euler angles follow", new Vector3(0, 1f, 0), n.Rotation, 1e-4f);
        n.Transform = new Transform3D(new Basis(Vector3.Right, 0.5f), new Vector3(7, 8, 9));
        T.Check("transform sets all three", n.Position == new Vector3(7, 8, 9) && T.Near(n.Quaternion, new Quaternion(Vector3.Right, 0.5f)) && T.Near(n.Scale, Vector3.One));
        n.RotationOrder = EulerOrder.Xyz;
        n.Rotation = new Vector3(0.1f, 0.2f, 0.3f);
        T.Check("rotation_order changes how Euler angles compose", n.Basis.IsEqualApprox(Basis.FromEuler(new Vector3(0.1f, 0.2f, 0.3f), EulerOrder.Xyz)));
        n.SetIdentity();
        T.Check("set_identity", n.Transform == Transform3D.Identity);
    }

    private static void GlobalTransform()
    {
        var parent = new Node3D { Position = new Vector3(10, 0, 0), RotationDegrees = new Vector3(0, 90, 0) };
        var child = new Node3D { Position = new Vector3(1, 0, 0) };
        parent.AddChild(child);
        SceneTree.Instance.Root.AddChild(parent);
        T.CheckVec("child global position = parent * local", new Vector3(10, 0, -1), child.GlobalPosition);
        T.CheckVec("global rotation includes the parent's", new Vector3(0, Pi / 2, 0), child.GlobalRotation, 1e-4f);
        child.GlobalPosition = Vector3.Zero;
        T.CheckVec("setting the global position solves the local one", Vector3.Zero, child.GlobalPosition, 1e-4f);
        T.CheckVec("which is (0, 0, -10) here", new Vector3(0, 0, -10), child.Position, 1e-4f);
        var grand = new Node3D { Position = new Vector3(0, 0, 1) };
        child.AddChild(grand);
        parent.Scale = new Vector3(2, 2, 2);
        // child is at local (0,0,-10), grand at (0,0,1) below it: parent * (0,0,-9) = Ry(90) * (0,0,-18) + (10,0,0) = (-8,0,0).
        T.CheckVec("a parent's scale scales its descendants' positions", new Vector3(-8, 0, 0), grand.GlobalPosition, 1e-3f);
        var plain = new Node { Name = "Plain" };
        var under = new Node3D { Position = new Vector3(1, 2, 3) };
        plain.AddChild(under);
        T.CheckVec("a Node3D under a plain Node starts its own chain", new Vector3(1, 2, 3), under.GlobalPosition);
        under.GlobalRotation = new Vector3(0, 2.0f, 0);
        T.CheckVec("global rotation can be set", new Vector3(0, 2.0f, 0), under.GlobalRotation, 1e-3f);
        under.GlobalTransform = new Transform3D(Basis.Identity, new Vector3(5, 5, 5));
        T.Check("global transform can be set", under.Position == new Vector3(5, 5, 5));
        under.GlobalRotationDegrees = new Vector3(0, 90, 0);
        T.CheckVec("global rotation degrees", new Vector3(0, 90, 0), under.GlobalRotationDegrees, 1e-3f);
    }

    private static void Movement()
    {
        var n = new Node3D { Scale = new Vector3(2, 2, 2) };
        n.LookAt(new Vector3(10, 0, 0));
        T.CheckVec("look_at turns -Z toward the target", new Vector3(1, 0, 0), n.GlobalTransform.Basis.Orthonormalized() * new Vector3(0, 0, -1));
        T.CheckVec("and keeps the scale", new Vector3(2, 2, 2), n.Scale, 1e-4f);
        var m = new Node3D { Position = new Vector3(0, 5, 0) };
        m.LookAtFromPosition(new Vector3(1, 1, 1), new Vector3(1, 1, 5));
        T.CheckVec("look_at_from_position moves to the position", new Vector3(1, 1, 1), m.GlobalPosition);
        T.CheckVec("and faces the target (+Z)", new Vector3(0, 0, 1), m.GlobalTransform.Basis * new Vector3(0, 0, -1));

        var t = new Node3D { RotationDegrees = new Vector3(0, 90, 0) };
        t.Translate(new Vector3(0, 0, -3));
        T.CheckVec("translate moves along the node's own axes", new Vector3(-3, 0, 0), t.Position, 1e-4f);
        t.GlobalTranslate(new Vector3(0, 1, 0));
        T.CheckVec("global_translate along the parent's", new Vector3(-3, 1, 0), t.Position, 1e-4f);

        var r = new Node3D { Position = new Vector3(1, 0, 0) };
        r.Rotate(Vector3.Up, Pi / 2);
        T.CheckVec("rotate turns the basis about a parent-space axis", new Vector3(0, 0, -1), r.Basis * new Vector3(1, 0, 0));
        T.CheckVec("and leaves the position alone", new Vector3(1, 0, 0), r.Position);
        r.RotateObjectLocal(new Vector3(1, 0, 0), Pi / 2);
        T.CheckVec("rotate_object_local turns about the node's own X axis (-Z goes to +Y)", Vector3.Up, r.Basis * new Vector3(0, 0, -1), 1e-4f);
        var rx = new Node3D(); rx.RotateX(Pi / 2); rx.RotateY(0f);
        T.CheckVec("rotate_x", new Vector3(0, 0, 1), rx.Basis * new Vector3(0, 1, 0));
        var rz = new Node3D(); rz.RotateZ(Pi / 2);
        T.CheckVec("rotate_z", new Vector3(0, 1, 0), rz.Basis * new Vector3(1, 0, 0));
        var gr = new Node3D(); gr.GlobalRotate(Vector3.Up, Pi / 2);
        T.CheckVec("global_rotate", new Vector3(0, 0, -1), gr.Basis * new Vector3(1, 0, 0));
        var sc = new Node3D { RotationDegrees = new Vector3(0, 90, 0) }; sc.ScaleObjectLocal(new Vector3(2, 1, 1));
        T.CheckVec("scale_object_local scales the node's own X", new Vector3(2, 1, 1), sc.Scale, 1e-4f);

        var a = new Node3D { Position = new Vector3(5, 0, 0) };
        var b = new Node3D { Position = new Vector3(0, 0, 1) };
        a.AddChild(b);
        T.CheckVec("to_global", new Vector3(5, 0, 2), b.ToGlobal(new Vector3(0, 0, 1)));
        T.CheckVec("to_local", new Vector3(0, 0, 1), b.ToLocal(new Vector3(5, 0, 2)));
        T.Check("get_parent_node_3d", b.GetParentNode3D() == a && a.GetParentNode3D() == null);
    }

    private static void Visibility()
    {
        var top = new Node3D(); var mid = new Node3D(); var low = new Node3D();
        top.AddChild(mid); mid.AddChild(low);
        SceneTree.Instance.Root.AddChild(top);
        int midChanged = 0, lowChanged = 0, notifs = 0;
        mid.Connect("visibility_changed", Callable.From(() => midChanged++));
        low.Connect("visibility_changed", Callable.From(() => lowChanged++));
        T.Check("visible in the tree", top.IsVisibleInTree() && low.IsVisibleInTree());
        top.Hide();
        T.Check("hiding the top hides the tree below it", !low.IsVisibleInTree() && low.Visible && midChanged == 1 && lowChanged == 1);
        mid.Hide();
        T.Check("hiding something already hidden in the tree changes nothing visible", midChanged == 1);
        top.Show();
        T.Check("showing the top shows only what is not itself hidden", !mid.IsVisibleInTree() && !low.IsVisibleInTree());
        mid.Show();
        T.Check("showing the middle shows its child", low.IsVisibleInTree() && lowChanged == 2);
        T.Check("a node outside the tree is not visible in it", !new Node3D().IsVisibleInTree());
        _ = notifs;
    }

    private static void TopLevelAndReparent()
    {
        var parent = new Node3D { Position = new Vector3(100, 0, 0) };
        var child = new Node3D { Position = new Vector3(1, 2, 3), TopLevel = true };
        parent.AddChild(child);
        T.CheckVec("a top-level node ignores its parent", new Vector3(1, 2, 3), child.GlobalPosition);
        var other = new Node3D { Position = new Vector3(0, 10, 0), RotationDegrees = new Vector3(0, 0, 90) };
        var moved = new Node3D { Position = new Vector3(1, 0, 0) };
        parent.AddChild(moved);
        SceneTree.Instance.Root.AddChild(parent);
        SceneTree.Instance.Root.AddChild(other);
        Vector3 before = moved.GlobalPosition;
        moved.Reparent(other);
        T.CheckVec("reparent keeps the global transform", before, moved.GlobalPosition, 1e-3f);
        T.Check("and the node is under the new parent", moved.GetParent() == other);
        Vector3 local = moved.Position;
        moved.Reparent(parent, false);
        T.CheckVec("reparent without keeping uses the local transform as it is", local, moved.Position, 1e-4f);
        T.CheckVec("so the global position changes with the new parent", parent.GlobalPosition + local, moved.GlobalPosition, 1e-3f);
    }

    private static void EngineValues()
    {
        var n = new Node3D { Position = new Vector3(1, 2, 3), Rotation = new Vector3(0, Pi / 2, 0), Scale = new Vector3(1, 2, 3) };
        var pos = new float[3]; var quat = new float[4]; var scale = new float[3];
        n.GetLocalForEngine(pos, quat, scale);
        T.Check("position goes to centimetres on Aver's axes", pos[0] == -300f && pos[1] == 100f && pos[2] == 200f);
        float s = MathF.Sqrt(0.5f);
        T.Check("a quarter turn about Godot's Y is a turn about Aver's Z, negated", T.Near(quat[0], 0f) && T.Near(quat[1], 0f) && T.Near(quat[2], -s) && T.Near(quat[3], s));
        T.Check("scale is permuted, not rescaled", scale[0] == 3f && scale[1] == 1f && scale[2] == 2f);
    }

    private static void ClassDefaults()
    {
        var cam = new Camera3D();
        T.Check("camera defaults", cam.Fov == 75f && cam.Near == 0.05f && cam.Far == 4000f && cam.Projection == Camera3D.ProjectionType.Perspective && !cam.IsCurrent());
        cam.MakeCurrent();
        T.Check("make_current without an engine only records it", cam.Current && cam.IsCurrent());
        var omni = new OmniLight3D();
        T.Check("omni light defaults", omni.OmniRange == 5f && omni.LightEnergy == 1f && !omni.ShadowEnabled && omni is Light3D);
        var spot = new SpotLight3D();
        T.Check("spot light defaults", spot.SpotAngle == 45f && spot.SpotRange == 5f);
        var mesh = new MeshInstance3D { Mesh = new BoxMesh { Size = new Vector3(2, 2, 2) } };
        T.Check("mesh instance holds its mesh", mesh.Mesh is BoxMesh { Size.X: 2 } && mesh.GetClass() == "MeshInstance3D");
        T.Check("mesh asset path", MeshInstance3D.AssetPath("res://meshes/a.ocmesh") == "meshes/a.ocmesh" && MeshInstance3D.AssetPath("meshes/a.ocmesh") == "meshes/a.ocmesh");
        T.Check("the class hierarchy", new CharacterBody3D() is PhysicsBody3D && new RigidBody3D() is CollisionObject3D && new Area3D() is CollisionObject3D &&
                                       new StaticBody3D() is PhysicsBody3D && new AnimatableBody3D() is StaticBody3D && new Marker3D() is Node3D && new CollisionShape3D() is Node3D &&
                                       new DirectionalLight3D() is Light3D && new MeshInstance3D() is GeometryInstance3D && new Control() is CanvasItem && new CanvasItem() is Node);
        var body = new RigidBody3D();
        T.Check("rigid body defaults", body.Mass == 1f && body.GravityScale == 1f && !body.Freeze && !body.ContactMonitor && body.CollisionLayer == 1 && body.CollisionMask == 1);
        body.SetCollisionLayerValue(3, true); body.SetCollisionMaskValue(2, true); body.SetCollisionLayerValue(1, false);
        T.Check("layer values are 1-based bits", body.CollisionLayer == 4u && body.CollisionMask == 3u && body.GetCollisionLayerValue(3) && !body.GetCollisionLayerValue(1) && body.GetCollisionMaskValue(2));
        var ch = new CharacterBody3D();
        T.Check("character body defaults follow Godot", ch.UpDirection == Vector3.Up && ch.MaxSlides == 6 && T.Near(ch.FloorMaxAngle, 0.785398f) && ch.FloorSnapLength == 0.1f && ch.FloorStopOnSlope && ch.SlideOnCeiling);
        T.Check("a body's motion type follows its class", new StaticBody3D().Motion == Aver.Physics.MotionType.Static && new AnimatableBody3D().Motion == Aver.Physics.MotionType.Kinematic &&
                                                         new RigidBody3D().Motion == Aver.Physics.MotionType.Dynamic && new CharacterBody3D().Motion == Aver.Physics.MotionType.Kinematic &&
                                                         new RigidBody3D { Freeze = true }.Motion == Aver.Physics.MotionType.Static &&
                                                         new RigidBody3D { Freeze = true, FreezeMode = RigidBody3D.FreezeModeEnum.Kinematic }.Motion == Aver.Physics.MotionType.Kinematic);
        T.Check("an area is a sensor", new Area3D().IsSensor && !new RigidBody3D().IsSensor);
        var ctl = new Control { Size = new Vector2(100, 50), Position = new Vector2(5, 6) };
        T.Check("control placeholder keeps its layout values", ctl.GetRect() == new Rect2(5, 6, 100, 50) && ctl.MouseFilter == Control.MouseFilterEnum.Stop);
        var mat = new StandardMaterial3D { AlbedoColor = Colors.Red };
        T.Check("materials and shapes are resources", mat is Resource && new BoxShape3D { Size = new Vector3(1, 2, 3) }.Size.Y == 2f && new CapsuleShape3D().Height == 2f && new SphereShape3D().Radius == 0.5f);
    }

    private static void PhysicsSignals()
    {
        var area = new Area3D(); var body = new RigidBody3D { ContactMonitor = true }; var other = new StaticBody3D(); var area2 = new Area3D();
        FabricPhysics.Register(101, area); FabricPhysics.Register(102, body); FabricPhysics.Register(103, other); FabricPhysics.Register(104, area2);
        var log = new List<string>();
        area.Connect("body_entered", Callable.From((Node n) => log.Add("entered " + n.GetType().Name)));
        area.Connect("body_exited", Callable.From((Node n) => log.Add("exited " + n.GetType().Name)));
        area.Connect("area_entered", Callable.From((Node n) => log.Add("area_entered")));
        area.Connect("area_exited", Callable.From((Node n) => log.Add("area_exited")));
        T.Check("an Area3D declares Godot's signals", area.HasSignal("body_shape_entered") && area.HasSignal("area_shape_exited") && area.HasSignal("input_event"));

        FabricPhysics.DispatchArea(101, 102, true, false);
        T.Check("a body entering an area emits body_entered with the node", log.SequenceEqual(new[] { "entered RigidBody3D" }));
        T.Check("the overlap lists", area.HasOverlappingBodies() && area.GetOverlappingBodies().Count == 1 && area.OverlapsBody(body) && !area.OverlapsBody(other));
        FabricPhysics.DispatchArea(101, 102, true, false);
        T.Check("the same body entering twice is reported once", log.Count == 1);
        FabricPhysics.DispatchArea(101, 103, true, false);
        FabricPhysics.DispatchArea(101, 102, false, false);
        T.Check("leaving emits body_exited and updates the list", log.Last() == "exited RigidBody3D" && area.GetOverlappingBodies().Count == 1 && area.OverlapsBody(other));
        FabricPhysics.DispatchArea(101, 102, false, false);
        T.Check("leaving when not inside emits nothing", log.Count(l => l.StartsWith("exited")) == 1);
        FabricPhysics.DispatchArea(101, 104, true, true);
        T.Check("an area entering an area emits area_entered", log.Contains("area_entered") && area.HasOverlappingAreas() && area.OverlapsArea(area2));
        FabricPhysics.DispatchArea(101, 104, false, true);
        T.Check("and area_exited", log.Contains("area_exited") && !area.HasOverlappingAreas());
        area.Monitoring = false;
        int before = log.Count;
        FabricPhysics.DispatchArea(101, 102, true, false);
        T.Check("a node that does not monitor hears nothing", log.Count == before);
        FabricPhysics.DispatchArea(999, 102, true, false);
        FabricPhysics.DispatchArea(101, 999, true, false);
        T.Check("handles of unknown bodies are ignored", log.Count == before);

        // Contact monitor: RigidBody3D.body_entered / body_exited.
        var contacts = new List<string>();
        body.Connect("body_entered", Callable.From((Node n) => contacts.Add("touch " + n.GetType().Name)));
        body.Connect("body_exited", Callable.From((Node n) => contacts.Add("leave " + n.GetType().Name)));
        FabricPhysics.DispatchContact(102, 103, true);
        FabricPhysics.DispatchContact(102, 103, false);
        T.Check("contact begin and end become body_entered and body_exited", contacts.SequenceEqual(new[] { "touch StaticBody3D", "leave StaticBody3D" }));
        body.ContactMonitor = false;
        FabricPhysics.DispatchContact(102, 103, true);
        T.Check("without contact_monitor there are no signals", contacts.Count == 2);
        FabricPhysics.DispatchContact(103, 102, true);
        T.Check("only a rigid body reports contacts", contacts.Count == 2);
        foreach (int h in new[] { 101, 102, 103, 104 }) FabricPhysics.Unregister(h);
        FabricPhysics.DispatchArea(101, 102, true, false);
        T.Check("an unregistered body routes nowhere", log.Count == before);
    }

    private static void PhysicsWithoutWorld()
    {
        var ch = new CharacterBody3D { Velocity = new Vector3(0, -9.8f, 0) };
        SceneTree.Instance.Root.AddChild(ch);
        T.Check("move_and_slide without a controller does nothing", !ch.MoveAndSlide() && !ch.IsOnFloor() && !ch.IsOnWall() && !ch.IsOnCeiling() && ch.Velocity == new Vector3(0, -9.8f, 0));
        T.Check("the queries report zero", ch.GetFloorNormal() == Vector3.Zero && ch.GetSlideCollisionCount() == 0 && ch.GetRealVelocity() == Vector3.Zero);
        var rb = new RigidBody3D { LinearVelocity = new Vector3(1, 2, 3) };
        SceneTree.Instance.Root.AddChild(rb);
        rb.ApplyImpulse(new Vector3(1, 0, 0)); rb.ApplyCentralForce(Vector3.One); rb.ApplyTorque(Vector3.One);
        T.Check("a rigid body keeps its velocity and ignores forces without a body", rb.LinearVelocity == new Vector3(1, 2, 3) && !rb.Sleeping);
        T.Check("no error was logged", T.ErrorCount() == 0);
        ch.Free(); rb.Free();
    }
}
