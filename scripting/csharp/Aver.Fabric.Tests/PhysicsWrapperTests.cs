// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The physics node wrappers and Camera3D without a physics world: Godot's defaults, unit and axis conversions at the boundary (the
// pseudovector sign of angular quantities), the state object _IntegrateForces gets, vehicle force distribution, query results when no
// physics library is present, and the camera's projection maths. Nothing here needs the engine; what does (a CharacterBody3D sliding
// on a floor) is reported as unverified in docs/plans/fabric/G3_RUNTIME.md.

using Aver.Fabric;
using Collections = Aver.Fabric.Collections;

static class PhysicsWrapperTests
{
    public static int RunAll() => T.Suite(() =>
    {
        T.Run("Camera3D: rays through the screen, project and unproject, behind, orthogonal, keep aspect", CameraProjection);
        T.Run("Camera3D: make_current keeps one current camera", CameraCurrent);
        T.Run("Angular quantities take the rotation map's sign", AngularSign);
        T.Run("PhysicsDirectBodyState3D: units, axes, impulses and write-back", DirectState);
        T.Run("RigidBody3D, StaticBody3D, CharacterBody3D, Area3D: Godot defaults and constant forces", Defaults);
        T.Run("VehicleBody3D spreads engine force, brake and steering over its wheels", VehicleDrive);
        T.Run("Space queries and the physics server answer empty without a physics world", Queries);
        T.Run("RayCast3D and ShapeCast3D: defaults and results with no hit", Casts);
        T.Run("Camera3D and lights: the world and viewport helpers", WorldHelpers);
    });

    private static Camera3D MakeCamera(float fov = 90f)
    {
        var cam = new Camera3D { Fov = fov };
        SceneTree.Instance.Root.AddChild(cam);
        return cam;
    }

    private static void CameraProjection()
    {
        var tree = SceneTree.Instance;
        tree.Root.Size = new Vector2I(1280, 720);
        Camera3D cam = MakeCamera(90f);
        T.CheckVec("the ray through the middle looks along -Z", new Vector3(0, 0, -1), cam.ProjectRayNormal(new Vector2(640, 360)));
        T.CheckVec("through the top middle: half the vertical field of view up (fov 90: 45 degrees)", new Vector3(0, 1, -1).Normalized(), cam.ProjectRayNormal(new Vector2(640, 0)));
        float aspect = 1280f / 720f;
        T.CheckVec("through the right edge: scaled by the aspect ratio", new Vector3(aspect, 0, -1).Normalized(), cam.ProjectRayNormal(new Vector2(1280, 360)));
        T.CheckVec("a perspective ray starts at the camera", Vector3.Zero, cam.ProjectRayOrigin(new Vector2(100, 100)));
        T.Check("a point straight ahead is the middle of the screen", cam.UnprojectPosition(new Vector3(0, 0, -10)).DistanceTo(new Vector2(640, 360)) < 1e-3f);
        T.Check("a point above is at the top edge when it is on the field-of-view line", cam.UnprojectPosition(new Vector3(0, 10, -10)).DistanceTo(new Vector2(640, 0)) < 1e-3f);
        T.CheckVec("project_position puts a point at a depth on the ray", new Vector3(0, 10, -10), cam.ProjectPosition(new Vector2(640, 0), 10f));
        var p = new Vector3(2.5f, -1f, -7f);
        Vector2 s = cam.UnprojectPosition(p);
        T.CheckVec("unproject then project at the same depth is the identity", p, cam.ProjectPosition(s, 7f), 1e-3f);
        T.Check("is_position_behind", cam.IsPositionBehind(new Vector3(0, 0, 10)) && !cam.IsPositionBehind(new Vector3(0, 0, -10)));
        T.Check("is_position_in_frustum", cam.IsPositionInFrustum(new Vector3(0, 0, -5)) && !cam.IsPositionInFrustum(new Vector3(100, 0, -5)) && !cam.IsPositionInFrustum(new Vector3(0, 0, 5)));

        // Moved and turned: 90 degrees about Y turns -Z into -X.
        cam.Position = new Vector3(0, 0, 5);
        cam.Rotation = new Vector3(0, Mathf.Pi / 2, 0);
        T.CheckVec("a turned camera shoots along its own -Z", new Vector3(-1, 0, 0), cam.ProjectRayNormal(new Vector2(640, 360)));
        T.CheckVec("and starts at its own position", new Vector3(0, 0, 5), cam.ProjectRayOrigin(new Vector2(640, 360)));
        T.Check("unproject follows the camera: a point 10 m ahead is the middle again", cam.UnprojectPosition(new Vector3(-10, 0, 5)).DistanceTo(new Vector2(640, 360)) < 1e-3f);

        // keep_aspect Width: the field of view is horizontal.
        cam.Position = Vector3.Zero; cam.Rotation = Vector3.Zero;
        cam.KeepAspect = Camera3D.KeepAspectEnum.Width;
        T.CheckVec("with keep_aspect Width the right edge is fov/2 off the axis (45 degrees)", new Vector3(1, 0, -1).Normalized(), cam.ProjectRayNormal(new Vector2(1280, 360)));
        cam.KeepAspect = Camera3D.KeepAspectEnum.Height;

        // Orthogonal: size is the height in metres.
        cam.Projection = Camera3D.ProjectionType.Orthogonal;
        cam.Size = 10f;
        T.CheckVec("an orthogonal ray looks along -Z whatever the pixel", new Vector3(0, 0, -1), cam.ProjectRayNormal(new Vector2(0, 0)));
        T.CheckVec("and starts on the view plane, at the pixel's place (top middle is 5 m up, at the near plane)", new Vector3(0, 5, -0.05f), cam.ProjectRayOrigin(new Vector2(640, 0)), 1e-3f);
        T.Check("unproject ignores depth", cam.UnprojectPosition(new Vector3(0, 5, -3)).DistanceTo(new Vector2(640, 0)) < 1e-3f && cam.UnprojectPosition(new Vector3(0, 5, -30)).DistanceTo(new Vector2(640, 0)) < 1e-3f);
        T.Check("the orthogonal projection spans the size", cam.GetCameraProjection() == Aver.Fabric.Projection.CreateOrthogonal(-5f * aspect, 5f * aspect, -5f, 5f, 0.05f, 4000f));
        cam.Projection = Camera3D.ProjectionType.Perspective;
        T.Check("the perspective projection is the standard one", cam.GetCameraProjection() == Aver.Fabric.Projection.CreatePerspective(90f, aspect, 0.05f, 4000f));
        T.Check("get_camera_transform is the orthonormal global transform", cam.GetCameraTransform().Origin == Vector3.Zero);
    }

    private static void CameraCurrent()
    {
        Camera3D a = MakeCamera(), b = MakeCamera();
        T.Check("no camera is current until one is made current", !a.IsCurrent() && !b.IsCurrent() && SceneTree.Instance.Root.GetCamera3D() == null);
        a.MakeCurrent();
        T.Check("make_current", a.IsCurrent() && a.Current && SceneTree.Instance.Root.GetCamera3D() == a);
        b.Current = true;
        T.Check("making another current clears the first", b.IsCurrent() && !a.IsCurrent() && SceneTree.Instance.Root.GetCamera3D() == b);
        b.ClearCurrent();
        T.Check("clear_current leaves none", !b.IsCurrent() && SceneTree.Instance.Root.GetCamera3D() == null);
    }

    private static void AngularSign()
    {
        var axis = new Vector3(1, 2, 3).Normalized();
        float angle = 0.3f;
        var q = new Quaternion(axis, angle);
        Aver.Scene.Quat aq = FabricSpace.ToAverRotation(q);
        Aver.Scene.Vec3 w = FabricSpace.ToAverAngular(axis);
        float s = MathF.Sin(angle / 2);
        T.Check("the vector part of the Aver rotation is sin(angle/2) times the mapped axis", T.Near(aq.X, s * w.X) && T.Near(aq.Y, s * w.Y) && T.Near(aq.Z, s * w.Z), $"{aq.X},{aq.Y},{aq.Z} vs {s * w.X},{s * w.Y},{s * w.Z}");
        T.Check("it is not the polar direction map (that has the opposite sign)", Math.Abs(aq.X - s * FabricSpace.ToAverDirection(axis).X) > 0.01f);
        T.CheckVec("the map round-trips", new Vector3(0.5f, -2f, 7f), FabricSpace.FromAverAngular(FabricSpace.ToAverAngular(new Vector3(0.5f, -2f, 7f))));
        var up = FabricSpace.ToAverAngular(new Vector3(0, 1, 0));
        T.Check("a spin about Godot's up (+Y) is a spin about -Z in Aver's (up is +Z)", up.X == 0 && up.Y == 0 && up.Z == -1);
        var body = new RigidBody3D { AngularVelocity = new Vector3(1, 2, 3) };
        T.CheckVec("without a body the velocities are what was set", new Vector3(1, 2, 3), body.AngularVelocity);
    }

    private static Aver.Physics.DirectBodyState MakeState() => new()
    {
        Dt = 1f / 60f,
        InverseMass = 0.5f,
        LinearVelocity = new Aver.Physics.Float3(0, 100, 0),          // Aver +Y is Godot +X
        AngularVelocity = new Aver.Physics.Float3(0, 0, 1),
        TotalGravity = new Aver.Physics.Float3(0, 0, -980),
        Position = new Aver.Physics.Float3(100, 0, 0),                // 1 m ahead in Aver is -Z in Godot
        Rotation = new Aver.Physics.Quaternion(0, 0, 0, 1),
        ContactCount = 2,
        InverseInertiaRow0 = new Aver.Physics.Float3(1e-4f, 0, 0),
        InverseInertiaRow1 = new Aver.Physics.Float3(0, 1e-4f, 0),
        InverseInertiaRow2 = new Aver.Physics.Float3(0, 0, 1e-4f),
    };

    private static void DirectState()
    {
        var st = new PhysicsDirectBodyState3D();
        Aver.Physics.DirectBodyState raw = MakeState();
        st.Load(ref raw);
        T.CheckNum("step", 1f / 60f, st.Step);
        T.CheckNum("inverse mass", 0.5f, st.InverseMass);
        T.CheckVec("linear velocity in m/s, Godot axes", new Vector3(1, 0, 0), st.LinearVelocity);
        T.CheckVec("angular velocity as a pseudovector (Aver +Z is Godot -Y)", new Vector3(0, -1, 0), st.AngularVelocity);
        T.CheckVec("total gravity: 9.8 m/s^2 down", new Vector3(0, -9.8f, 0), st.TotalGravity);
        T.CheckVec("the transform origin in metres", new Vector3(0, 0, -1), st.Transform.Origin);
        T.Check("contact count", st.GetContactCount() == 2);
        Aver.Physics.DirectBodyState untouched = MakeState();
        st.Store(ref untouched);
        T.Check("nothing written back when nothing changed", untouched.LinearVelocity.Y == 100f && untouched.AngularVelocity.Z == 1f);

        st.ApplyCentralImpulse(new Vector3(2, 0, 0));
        T.CheckVec("an impulse changes the velocity by impulse / mass: +1 m/s", new Vector3(2, 0, 0), st.LinearVelocity);
        st.ApplyCentralForce(new Vector3(0, 0, 60));
        T.CheckVec("a force over the step: 60 N * 0.5 / kg * 1/60 s = 0.5 m/s", new Vector3(2, 0, 0.5f), st.LinearVelocity);
        st.LinearVelocity = new Vector3(0, 0, -3);
        Aver.Physics.DirectBodyState written = MakeState();
        st.Store(ref written);
        T.Check("the velocity goes back in Aver units: Godot -Z is Aver +X, 3 m/s is 300 cm/s", T.Near(written.LinearVelocity.X, 300f) && T.Near(written.LinearVelocity.Y, 0f) && T.Near(written.LinearVelocity.Z, 0f), written.LinearVelocity.ToString());
        st.AngularVelocity = new Vector3(0, 2, 0);
        st.Store(ref written);
        T.Check("and the spin: Godot +Y 2 rad/s is Aver -Z 2 rad/s", T.Near(written.AngularVelocity.Z, -2f));

        st.Load(ref raw);
        st.ApplyTorqueImpulse(new Vector3(0, 0.5f, 0));
        T.Check("a torque impulse spins the body by inertia: 0.5 N m s * 1e4 cm^2/m^2 * 1e-4 = 0.5 rad/s about Godot +Y", T.Near(st.AngularVelocity.Y, -1f + 0.5f), st.AngularVelocity.ToString());
    }

    private static void Defaults()
    {
        var rb = new RigidBody3D();
        T.Check("RigidBody3D: mass 1, gravity scale 1, no damping of its own, can sleep, not frozen", rb.Mass == 1f && rb.GravityScale == 1f && rb.LinearDamp == 0f && rb.CanSleep && !rb.Freeze && rb.CollisionLayer == 1 && rb.CollisionMask == 1);
        rb.AddConstantForce(new Vector3(1, 0, 0), new Vector3(0, 1, 0));
        rb.AddConstantCentralForce(new Vector3(0, 0, 2));
        rb.AddConstantTorque(new Vector3(0, 3, 0));
        T.CheckVec("add_constant_force adds a force, and the torque it makes about the origin", new Vector3(1, 0, 2), rb.ConstantForce);
        T.CheckVec("the torque is position x force plus the torque added", new Vector3(0, 3, -1), rb.ConstantTorque);
        rb.Mass = 12f;
        rb.LinearVelocity = new Vector3(1, 2, 3);
        T.Check("settings can be changed before the body exists", rb.Mass == 12f && rb.LinearVelocity == new Vector3(1, 2, 3) && rb.GetContactCount() == 0 && rb.GetCollidingBodies().Count == 0 && !rb.Sleeping);
        var frozen = new RigidBody3D { Freeze = true };
        T.Check("freeze changes the motion type", frozen.Motion == Aver.Physics.MotionType.Static);

        var ch = new CharacterBody3D();
        T.Check("CharacterBody3D: Godot's defaults", ch.MaxSlides == 6 && T.Near(ch.FloorMaxAngle, 0.785398f) && T.Near(ch.FloorSnapLength, 0.1f) && T.Near(ch.SafeMargin, 0.001f) &&
                                                   T.Near(ch.WallMinSlideAngle, 0.261799f) && ch.UpDirection == Vector3.Up && ch.FloorStopOnSlope && ch.FloorBlockOnWall && ch.SlideOnCeiling && !ch.FloorConstantSpeed);
        T.Check("without a body it does not move, collide or touch anything", !ch.MoveAndSlide() && !ch.IsOnFloor() && !ch.IsOnWall() && !ch.IsOnCeiling() && ch.GetSlideCollisionCount() == 0 &&
                                                                              ch.GetSlideCollision(0) == null && ch.MoveAndCollide(Vector3.One) == null);

        var area = new Area3D();
        T.Check("Area3D: monitoring and monitorable, 9.8 gravity downwards, damping 0.1, no overrides", area.Monitoring && area.Monitorable && area.Gravity == 9.8f && area.GravityDirection == new Vector3(0, -1, 0) &&
                                                                                             area.LinearDamp == 0.1f && area.GravitySpaceOverride == Area3D.SpaceOverride.Disabled && !area.GravityPoint);
        T.Check("the body flags map onto Godot's layers", ((Func<bool>)(() => { rb.SetCollisionLayerValue(3, true); rb.SetCollisionMaskValue(1, false); return rb.GetCollisionLayerValue(3) && rb.CollisionLayer == 5 && rb.CollisionMask == 0; }))());
        var sb = new StaticBody3D { ConstantLinearVelocity = new Vector3(1, 0, 0) };
        T.Check("StaticBody3D keeps a conveyor velocity", sb.ConstantLinearVelocity == new Vector3(1, 0, 0) && sb.PhysicsMaterialOverride == null);
        var mat = new PhysicsMaterial();
        T.Check("PhysicsMaterial: Godot's defaults are friction 1 and bounce 0", mat.Friction == 1f && mat.Bounce == 0f && !mat.Rough && !mat.Absorbent);
        var hit = new KinematicCollision3D();
        T.Check("KinematicCollision3D with nothing in it", hit.GetCollisionCount() == 0 && hit.GetNormal() == Vector3.Zero && hit.GetCollider() == null && hit.GetAngle() == 0f);
        var anim = new AnimatableBody3D();
        T.Check("AnimatableBody3D is kinematic", anim.Motion == Aver.Physics.MotionType.Kinematic && !anim.SyncToPhysics);

        // Props written by the converter reach the new properties.
        var body = new RigidBody3D();
        body.PendingValues = new List<(string, string)> { ("axis_lock_angular_y", "true"), ("gravity_scale", "0.5"), ("linear_damp", "0.75"), ("freeze_mode", "1"), ("contact_monitor", "true"), ("max_contacts_reported", "4") };
        SceneTree.Instance.Root.AddChild(body);
        T.Check("converted properties: axis lock, gravity scale, damping, freeze mode, contact monitor", body.AxisLockAngularY && body.GravityScale == 0.5f && body.LinearDamp == 0.75f && body.FreezeMode == RigidBody3D.FreezeModeEnum.Kinematic && body.ContactMonitor && body.MaxContactsReported == 4);
    }

    private static void VehicleDrive()
    {
        var car = new VehicleBody3D();
        T.Check("a VehicleBody3D is heavier than a plain body (40 kg)", car.Mass == 40f);
        VehicleWheel3D fl = new() { Name = "FL", UseAsSteering = true }, fr = new() { Name = "FR", UseAsSteering = true },
                       rl = new() { Name = "RL", UseAsTraction = true }, rr = new() { Name = "RR", UseAsTraction = true };
        foreach (VehicleWheel3D w in new[] { fl, fr, rl, rr }) car.AddChild(w);
        SceneTree.Instance.Root.AddChild(car);
        car.EngineForce = 800f;
        car.Steering = 0.3f;
        car.Brake = 2f;
        T.Check("engine force goes to the traction wheels only", rl.EngineForce == 800f && rr.EngineForce == 800f && fl.EngineForce == 0f && fr.EngineForce == 0f);
        T.Check("steering to the steering wheels only", fl.Steering == 0.3f && fr.Steering == 0.3f && rl.Steering == 0f && rr.Steering == 0f);
        T.Check("the brake to every wheel", new[] { fl, fr, rl, rr }.All(w => w.Brake == 2f));
        rl.EngineForce = 0f;
        T.Check("a wheel can be driven on its own", rl.EngineForce == 0f && rr.EngineForce == 800f && car.EngineForce == 800f);
        T.Check("wheel defaults are Godot's", T.Near(fl.WheelRadius, 0.5f) && T.Near(fl.WheelRestLength, 0.15f) && T.Near(fl.SuspensionStiffness, 5.88f) && T.Near(fl.WheelFrictionSlip, 10.5f) &&
                                              T.Near(fl.SuspensionTravel, 0.2f) && T.Near(fl.SuspensionMaxForce, 6000f) && T.Near(fl.WheelRollInfluence, 0.1f));
        T.Check("a wheel with no vehicle reports no contact", !fl.IsInContact() && fl.GetRpm() == 0f && fl.GetSkidinfo() == 1f && fl.GetContactBody() == null);
        // The force conversions the wheels hand the vehicle (G2 helpers): metres/newtons into the ABI's units.
        T.CheckNum("engine force newtons to kg*cm/s^2", 80000f, Aver.Physics.RaycastWheelSettings.EngineForceFromGodot(800f));
        T.CheckNum("steering left is negative in the ABI", -0.3f, Aver.Physics.RaycastWheelSettings.SteeringFromGodot(0.3f));
        T.CheckNum("brake impulse per step to a force at 60 Hz", 2f * 60f * 100f, Aver.Physics.RaycastWheelSettings.BrakeFromGodot(2f, 60f));
    }

    private static void Queries()
    {
        var world = new World3D();
        var q = PhysicsRayQueryParameters3D.Create(new Vector3(0, 10, 0), new Vector3(0, -10, 0));
        T.Check("a ray query has Godot's defaults", q.CollideWithBodies && !q.CollideWithAreas && q.HitBackFaces && !q.HitFromInside && q.CollisionMask == 0xFFFFFFFF && q.Exclude.Count == 0);
        T.Check("intersect_ray with no physics world is an empty dictionary, not an error", world.DirectSpaceState.IntersectRay(q).Count == 0);
        var sq = new PhysicsShapeQueryParameters3D { Shape = new SphereShape3D { Radius = 1f } };
        T.Check("shape queries too", world.DirectSpaceState.IntersectShape(sq).Count == 0 && world.DirectSpaceState.CollideShape(sq).Count == 0 && world.DirectSpaceState.GetRestInfo(sq).Count == 0);
        float[] cast = world.DirectSpaceState.CastMotion(sq);
        T.Check("cast_motion says nothing is in the way", cast.Length == 2 && cast[0] == 1f && cast[1] == 1f);
        T.Check("a query with no shape is empty", world.DirectSpaceState.IntersectShape(new PhysicsShapeQueryParameters3D()).Count == 0);
        var body = new StaticBody3D();
        T.Check("a body with no physics body has no rid", !body.GetRid().IsValid);
        var none = new Rid(0);
        T.Check("the physics server by rid does nothing for an unknown one", PhysicsServer3D.BodyGetState(none, PhysicsServer3D.BodyState.LinearVelocity).IsNil && PhysicsServer3D.BodyGetCollisionLayer(none) == 0);
        PhysicsServer3D.BodySetState(none, PhysicsServer3D.BodyState.LinearVelocity, Vector3.One);
        T.Check("and the direct state is the world's", PhysicsServer3D.SpaceGetDirectState(world.Space) == SceneTree.Instance.Root.World3D.DirectSpaceState);
        var stub = new Collections.Dictionary { ["__class"] = "NoSuchThing", ["x"] = 1 };
        T.Check("a dictionary naming an unknown class is not turned into an object", stub.Count == 2 && Variant.From(stub).ToType(typeof(PhysicsMaterial)) == null);
        var good = new Collections.Dictionary { ["__class"] = "PhysicsMaterial", ["friction"] = 0.3, ["rough"] = true };
        T.Check("one naming a known class is, with its entries as properties", Variant.From(good).ToType(typeof(PhysicsMaterial)) is PhysicsMaterial { Rough: true } pm && T.Near(pm.Friction, 0.3f));
        T.Check("but only for a type the class fits", Variant.From(good).ToType(typeof(BoxShape3D)) == null);
    }

    private static void Casts()
    {
        var ray = new RayCast3D();
        T.Check("RayCast3D defaults: enabled, excludes its parent, aims down 1 m, mask 1, bodies yes, areas no, back faces yes, from inside no",
                ray.Enabled && ray.ExcludeParent && ray.TargetPosition == new Vector3(0, -1, 0) && ray.CollisionMask == 1 && ray.CollideWithBodies && !ray.CollideWithAreas && ray.HitBackFaces && !ray.HitFromInside);
        ray.ForceRaycastUpdate();
        T.Check("without a physics world it hits nothing", !ray.IsColliding() && ray.GetCollider() == null && ray.GetCollisionPoint() == Vector3.Zero && ray.GetCollisionNormal() == Vector3.Zero && !ray.GetColliderRid().IsValid);
        ray.SetCollisionMaskValue(3, true);
        T.Check("mask values by layer number", ray.GetCollisionMaskValue(3) && ray.CollisionMask == 5);
        var body = new StaticBody3D();
        ray.AddException(body);
        ray.AddExceptionRid(new Rid(7));
        ray.ClearExceptions();
        var shape = new ShapeCast3D { Shape = new SphereShape3D { Radius = 0.5f } };
        T.Check("ShapeCast3D defaults: enabled, aims down 1 m, margin 0, up to 32 results", shape.Enabled && shape.TargetPosition == new Vector3(0, -1, 0) && shape.Margin == 0f && shape.MaxResults == 32 && shape.ExcludeParent);
        shape.ForceShapecastUpdate();
        T.Check("and finds nothing with no world", !shape.IsColliding() && shape.GetCollisionCount() == 0 && shape.GetCollider(0) == null && shape.GetClosestCollisionSafeFraction() == 1f);
        var tree = SceneTree.Instance;
        tree.Root.AddChild(ray);
        tree.Root.AddChild(shape);
        T.Frames(2, 0.016, 2);
        T.Check("they tick with the physics step without a world and without errors", T.ErrorCount() == 0);
    }

    private static void WorldHelpers()
    {
        var tree = SceneTree.Instance;
        var n = new Node3D { Name = "N" };
        T.Check("outside the tree there is no world", n.GetWorld3D() == null);
        tree.Root.AddChild(n);
        T.Check("inside it, the viewport's", n.GetWorld3D() == tree.Root.World3D && n.GetViewport() == tree.Root);
        var sh = new CollisionShape3D();
        var body = new StaticBody3D();
        body.AddChild(sh);
        tree.Root.AddChild(body);
        sh.Shape = new BoxShape3D();
        sh.Disabled = true;
        sh.Disabled = false;
        T.Check("changing a shape under a body that has no physics body is harmless", T.ErrorCount() == 0);
        var light = new DirectionalLight3D { LightEnergy = 2f };
        tree.Root.AddChild(light);
        var mesh = new MeshInstance3D { MaterialOverride = new StandardMaterial3D { ResourcePath = "Materials/A.ocmat" } };
        tree.Root.AddChild(mesh);
        mesh.Visible = false;
        T.Check("a mesh node hides with visible and keeps its material override", !mesh.IsVisibleInTree() && mesh.MaterialOverride!.ResourcePath == "Materials/A.ocmat");
        mesh.MaterialOverride = null;
        T.Check("the material override can be removed again", mesh.MaterialOverride == null && light.LightEnergy == 2f);
    }
}
