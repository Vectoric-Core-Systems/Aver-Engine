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
        T.Run("lights: calibration and the values pushed to the engine (G4 R2)", LightValues);
        T.Run("lights: one sun, owned by the sky (G4 R2)", OneSun);
        T.Run("cameras: what the engine is given, the frustum maths and keep_aspect (G4 R4)", CameraValues);
        T.Run("layers, visibility ranges and light cull masks: the values pushed to the engine (G4 R4)", LayerValues);
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
        T.Check("material vertex colour: Godot's names, off by default",
                !mat.VertexColorUseAsAlbedo && !mat.VertexColorIsSrgb && new StandardMaterial3D { VertexColorUseAsAlbedo = true }.VertexColorUseAsAlbedo);
        MaterialFeatures();
    }

    // G4 R6: BaseMaterial3D under Godot's names, the defaults Godot has, and the FX record the properties bake to (the grammar tools/fabric writes and
    // aver::fmt::parseOcmatFx reads: tests/formats MaterialFxTest and FabricMaterialFxTest hold the native side).
    private static void MaterialFeatures()
    {
        var def = new StandardMaterial3D();
        T.Check("material features: Godot's defaults",
                def.Uv1Scale == Vector3.One && def.Uv1Offset == Vector3.Zero && def.Transparency == BaseMaterial3D.TransparencyEnum.Disabled && def.CullMode == BaseMaterial3D.CullModeEnum.Back &&
                def.BlendMode == BaseMaterial3D.BlendModeEnum.Mix && def.ShadingMode == BaseMaterial3D.ShadingModeEnum.PerPixel && def.MetallicSpecular == 0.5f && def.Rim == 1f &&
                def.RimTint == 0.5f && def.Clearcoat == 1f && def.ClearcoatRoughness == 0.5f && def.HeightmapScale == 5f && def.DistanceFadeMaxDistance == 10f &&
                def.RefractionScale == 0.05f && def.AlphaScissorThreshold == 0.5f && def.RenderPriority == 0 && def.Uv1TriplanarSharpness == 1f);
        T.Check("a default material bakes to the cull mode alone (back-face culling is Godot's default and a feature here)", def.FxRecord() == "cull=back");
        T.Check("ORMMaterial3D is a base material with the packed texture slot", new ORMMaterial3D() is BaseMaterial3D { } && new ORMMaterial3D().OrmTexture == null && ClassDB.TypeOf("ORMMaterial3D") == typeof(ORMMaterial3D));
        var m = new StandardMaterial3D
        {
            Uv1Scale = new Vector3(2, 3, 1), Uv1Offset = new Vector3(0.25f, 0.5f, 0), Uv1Triplanar = true, Uv1WorldTriplanar = true, Uv1TriplanarSharpness = 4f,
            Transparency = BaseMaterial3D.TransparencyEnum.Alpha, BlendMode = BaseMaterial3D.BlendModeEnum.Add, CullMode = BaseMaterial3D.CullModeEnum.Disabled,
            DepthDrawMode = BaseMaterial3D.DepthDrawModeEnum.Always, NoDepthTest = true, RenderPriority = 5, ShadingMode = BaseMaterial3D.ShadingModeEnum.Unshaded,
            BillboardMode = BaseMaterial3D.BillboardModeEnum.FixedY, BillboardKeepScale = true, FixedSize = true, Grow = true, GrowAmount = 0.02f,
            DistanceFadeMode = BaseMaterial3D.DistanceFadeModeEnum.PixelDither, DistanceFadeMinDistance = 2f, DistanceFadeMaxDistance = 20f,
            RimEnabled = true, Rim = 0.8f, RimTint = 0.2f, HeightmapEnabled = true, HeightmapScale = 8f, HeightmapDeepParallax = true, HeightmapMinLayers = 4, HeightmapMaxLayers = 16,
            DetailEnabled = true, DetailBlendMode = BaseMaterial3D.BlendModeEnum.Mul, DetailUvLayer = BaseMaterial3D.DetailUVEnum.DetailUv2,
            SpecularMode = BaseMaterial3D.SpecularModeEnum.Disabled, DiffuseMode = BaseMaterial3D.DiffuseModeEnum.LambertWrap, MetallicSpecular = 0.25f,
        };
        var rec = " " + m.FxRecord() + " ";
        T.Check("the record names UV scale, offset and the triplanar settings", rec.Contains(" uv1scale=2,3 ") && rec.Contains(" uv1offset=0.25,0.5 ") && rec.Contains(" uv1tri=1 ") && rec.Contains(" uv1world=1 ") && rec.Contains(" uv1sharp=4 "));
        T.Check("pipeline state: two-sided, add blend, depth modes, priority, unshaded", rec.Contains(" cull=none ") && rec.Contains(" blend=add ") && rec.Contains(" depthdraw=always ") && rec.Contains(" nodepthtest=1 ") && rec.Contains(" priority=5 ") && rec.Contains(" unshaded=1 "));
        T.Check("vertex stage in centimetres: grow 0.02 m is 2 cm, distances 2 m and 20 m are 200 and 2000", rec.Contains(" billboard=y ") && rec.Contains(" keepscale=1 ") && rec.Contains(" fixedsize=1 ") && rec.Contains(" growcm=2 ") && rec.Contains(" fade=dither ") && rec.Contains(" fademin=200 ") && rec.Contains(" fademax=2000 "));
        T.Check("surface features", rec.Contains(" rim=1 ") && rec.Contains(" rimamount=0.8 ") && rec.Contains(" height=1 ") && rec.Contains(" deepparallax=1 ") && rec.Contains(" layers=4,16 ") && rec.Contains(" detail=1 ") && rec.Contains(" detailblend=3 ") && rec.Contains(" detailuv=2 ") && rec.Contains(" spec=off ") && rec.Contains(" diffuse=wrap "));
        T.Check("metallic_specular 0.25 is a reflectance of 0.02", T.Near(m.Reflectance, 0.02f, 1e-6f));
    }

    // Godot energy 1 on a white surface returns 1 in Godot (energy * pi, diffuse 1/pi); Aver's diffuse carries 1/pi and its lights are engine
    // radiance units of 100000/3 lux (the 100000 lux sun is 3.0), so energy 1 is pi units. The same constant is in tools/fabric (the SUN
    // record) and aver/formats/GodotLight.hpp; RasterLightsTest and FabricConvertTest check those against 104719.755.
    private static void CameraValues()
    {
        T.Reset();
        var def = new Camera3D().ComputeFields();
        T.Check("a default camera is perspective, keep height, every layer, no offsets", def.Projection == 0 && (def.Flags & Camera3D.FlagKeepWidth) == 0 && def.CullInv == 0
                && def.HOffsetCm == 0f && def.VOffsetCm == 0f);
        T.Check("it asks to drive the view", (def.Flags & Camera3D.FlagDrivesView) != 0);
        T.Check("fov 75 degrees, near 0.05 m = 5 cm, far 4000 m = 400000 cm", T.Near(def.FovRad, 75f * MathF.PI / 180f, 1e-5f) && T.Near(def.NearCm, 5f) && T.Near(def.FarCm, 400000f, 0.1f));

        var cam = new Camera3D { Projection = Camera3D.ProjectionType.Orthogonal, Size = 12f, KeepAspect = Camera3D.KeepAspectEnum.Width, CullMask = 6u,
                                 HOffset = 0.5f, VOffset = -0.25f, FrustumOffset = new Vector2(0.3f, -0.1f) };
        var f = cam.ComputeFields();
        T.Check("orthogonal is projection 1 with the size in cm", f.Projection == 1 && T.Near(f.SizeCm, 1200f));
        T.Check("keep_aspect width sets the flag", (f.Flags & Camera3D.FlagKeepWidth) != 0);
        T.Check("cull_mask 6 (layers 2 and 3) is stored inverted: 0xFFFF9", f.CullInv == 0xFFFF9);
        T.Check("h_offset and v_offset, metres to cm", T.Near(f.HOffsetCm, 50f) && T.Near(f.VOffsetCm, -25f));
        T.Check("frustum_offset, metres to cm", T.Near(f.OffsetXCm, 30f) && T.Near(f.OffsetYCm, -10f));
        T.Check("cull_mask 0xFFFFF (every layer) is stored as 0", new Camera3D { CullMask = 0xFFFFFu }.ComputeFields().CullInv == 0);

        // The frustum camera: the projection, the ray and the unprojection agree.
        var fr = new Camera3D { Projection = Camera3D.ProjectionType.Frustum, Size = 2f, Near = 0.5f, Far = 100f, FrustumOffset = new Vector2(0.3f, -0.1f) };
        float aspect = 1280f / 720f;
        Projection p = fr.GetCameraProjection();
        T.Check("a frustum projection is the offset-window frustum", p == Aver.Fabric.Projection.CreateFrustum(-aspect + 0.3f, aspect + 0.3f, -1f - 0.1f, 1f - 0.1f, 0.5f, 100f));
        // top-left pixel: the ray goes through the near plane's top-left corner of the shifted window
        Vector3 dir = fr.ProjectRayNormal(new Vector2(0, 0));
        T.CheckVec("the top-left ray points through the shifted window corner", new Vector3(-aspect + 0.3f, 1f - 0.1f, -0.5f).Normalized(), dir, 1e-4f);
        T.CheckVec("a frustum ray starts at the camera", Vector3.Zero, fr.ProjectRayOrigin(new Vector2(10, 10)), 1e-6f);
        Vector3 world = fr.ProjectPosition(new Vector2(320, 90), 7f);
        T.Check("unprojecting a projected position returns the pixel", fr.UnprojectPosition(world).DistanceTo(new Vector2(320, 90)) < 1e-2f);
        T.Check("the centre pixel is not on the axis of a shifted frustum", MathF.Abs(fr.ProjectRayNormal(new Vector2(640, 360)).X) > 1e-3f);

        // h_offset slides the camera along its own X: the transform and the rays move with it.
        var slid = new Camera3D { HOffset = 2f, VOffset = 1f };
        T.CheckVec("h_offset / v_offset are in get_camera_transform", new Vector3(2, 1, 0), slid.GetCameraTransform().Origin, 1e-5f);
        T.CheckVec("and in the ray origin", new Vector3(2, 1, 0), slid.ProjectRayOrigin(new Vector2(0, 0)), 1e-5f);

        // keep_aspect width on a perspective camera: fov is the horizontal one.
        var kw = new Camera3D { Fov = 90f, KeepAspect = Camera3D.KeepAspectEnum.Width };
        Vector3 edge = kw.ProjectRayNormal(new Vector2(1280, 360));
        T.Check("horizontal fov 90: the right edge ray is 45 degrees off the axis", T.Near(MathF.Atan2(edge.X, -edge.Z), MathF.PI / 4f, 1e-4f));
        var kh = new Camera3D { Fov = 90f };
        Vector3 top = kh.ProjectRayNormal(new Vector2(640, 0));
        T.Check("vertical fov 90: the top edge ray is 45 degrees off the axis", T.Near(MathF.Atan2(top.Y, -top.Z), MathF.PI / 4f, 1e-4f));
    }

    private static void LayerValues()
    {
        T.Reset();
        var mesh = new MeshInstance3D();
        T.Check("a mesh instance is on layer 1 by default", mesh.Layers == 1u && VisualInstance3D.StoreMask(mesh.Layers) == 0xFFFFE);
        T.Check("storing every layer gives zero (the engine's zero-fill default)", VisualInstance3D.StoreMask(0xFFFFFu) == 0 && VisualInstance3D.StoreMask(0xFFFFFFFFu) == 0);
        mesh.SetLayerMaskValue(3, true);
        T.Check("set_layer_mask_value(3) adds layer 3", mesh.Layers == 5u && mesh.GetLayerMaskValue(3) && mesh.GetLayerMaskValue(1) && !mesh.GetLayerMaskValue(2));

        var none = mesh.ComputeRangeFields();
        T.Check("no visibility range by default", none.BeginCm == 0f && none.EndCm == 0f && none.FadeMode == 0);
        var geo = new MeshInstance3D { VisibilityRangeBegin = 5f, VisibilityRangeBeginMargin = 1f, VisibilityRangeEnd = 40f, VisibilityRangeEndMargin = 2f,
                                       VisibilityRangeFadeMode = GeometryInstance3D.VisibilityRangeFadeModeEnum.Self };
        var r = geo.ComputeRangeFields();
        T.Check("range and margins, metres to cm", T.Near(r.BeginCm, 500f) && T.Near(r.EndCm, 4000f) && T.Near(r.BeginMarginCm, 100f) && T.Near(r.EndMarginCm, 200f));
        T.Check("the fade mode carries (the engine hard-cuts it)", r.FadeMode == 1);
        T.Check("a negative range is no range", new MeshInstance3D { VisibilityRangeEnd = -3f }.ComputeRangeFields().EndCm == 0f);

        var light = new OmniLight3D();
        var d = light.ComputeFields();
        T.Check("a light affects and is shadowed by every layer by default", d.CullInv == 0 && d.ShadowCasterInv == 0 && light.LightCullMask == 0xFFFFFFFFu);
        var masked = new OmniLight3D { LightCullMask = 2u, ShadowCasterMask = 4u }.ComputeFields();
        T.Check("light_cull_mask 2 and shadow_caster_mask 4 are stored inverted", masked.CullInv == 0xFFFFD && masked.ShadowCasterInv == 0xFFFFB);
        var spot = new SpotLight3D { LightCullMask = 8u }.ComputeFields();
        T.Check("a spot light carries its mask too", spot.CullInv == 0xFFFF7 && spot.Kind == 1);
        T.Check("the kind-specific values survive the mask (range)", T.Near(new OmniLight3D { OmniRange = 3f, LightCullMask = 1u }.ComputeFields().RangeCm, 300f));
    }

    private static void LightValues()
    {
        T.Reset();
        const float engineUnitLux = 100000f / 3f;
        T.Check("Light3D.LuxPerEnergy is pi engine radiance units", MathF.Abs(Light3D.LuxPerEnergy / engineUnitLux - MathF.PI) < 1e-4f && MathF.Abs(Light3D.LuxPerEnergy - 104719.755f) < 0.01f);
        T.Check("an omni's candela per energy is the same number (irradiance at 1 m)", Light3D.CandelaPerEnergy == Light3D.LuxPerEnergy);

        var omni = new OmniLight3D { LightEnergy = 2f, OmniRange = 8f, OmniAttenuation = 2f, ShadowEnabled = true, LightSpecular = 0.25f, LightSize = 0.1f,
                                     LightColor = new Color(0.5f, 0.5f, 0.5f), ShadowBias = 0.3f, ShadowNormalBias = 4f };
        LightFields f = omni.ComputeFields();
        T.Check("omni: kind point, candela = energy * calibration", f.Kind == 0 && T.Near(f.Intensity, 2f * Light3D.CandelaPerEnergy, 0.05f));
        T.Check("omni: range metres to cm, decay as authored", T.Near(f.RangeCm, 800f) && f.Attenuation == 2f);
        T.Check("omni: sRGB 0.5 is linear 0.2140", T.Near(f.Colour.X, 0.21404f, 1e-4f) && T.Near(f.Colour.Y, f.Colour.X) && T.Near(f.Colour.Z, f.Colour.X));
        T.Check("omni: light_specular 0.25 is a factor 0.5 (Godot doubles it)", T.Near(f.SpecularScale, 0.5f));
        T.Check("omni: light_size 0.1 m is a 10 cm emitter radius", T.Near(f.SourceRadiusCm, 10f));
        T.Check("omni: shadows and the bias values carry", f.Shadows && T.Near(f.ShadowBias, 0.3f) && T.Near(f.ShadowNormalBias, 4f));
        T.Check("omni: no distance fade unless enabled", f.FadeBeginCm == 0f && f.FadeLengthCm == 0f && f.FadeShadowCm == 0f);

        var defaults = new OmniLight3D().ComputeFields();
        T.Check("Godot's defaults: specular 0.5 -> 1, attenuation 1, range 5 m, no shadow, bias 0.1 / 2",
                T.Near(defaults.SpecularScale, 1f) && defaults.Attenuation == 1f && T.Near(defaults.RangeCm, 500f) && !defaults.Shadows &&
                T.Near(defaults.ShadowBias, 0.1f) && T.Near(defaults.ShadowNormalBias, 2f) && !defaults.Negative && defaults.SourceRadiusCm == 0f);

        var fading = new OmniLight3D { DistanceFadeEnabled = true }.ComputeFields();
        T.Check("distance fade: Godot's 40 / 50 / 10 m defaults in cm", T.Near(fading.FadeBeginCm, 4000f) && T.Near(fading.FadeShadowCm, 5000f) && T.Near(fading.FadeLengthCm, 1000f));
        T.Check("light_negative is carried", new OmniLight3D { LightNegative = true }.ComputeFields().Negative);

        var spot = new SpotLight3D { SpotRange = 12f, SpotAngle = 30f, SpotAttenuation = 2f, SpotAngleAttenuation = 0.5f, LightEnergy = 0.5f };
        LightFields s = spot.ComputeFields();
        T.Check("spot: kind spot, range, outer angle = spot_angle, inner 80 percent for the ray-traced path",
                s.Kind == 1 && T.Near(s.RangeCm, 1200f) && T.Near(s.OuterDeg, 30f) && T.Near(s.InnerDeg, 24f));
        T.Check("spot: distance decay and cone-edge curve are separate", s.Attenuation == 2f && s.SpotAttenuation == 0.5f && T.Near(s.Intensity, 0.5f * Light3D.CandelaPerEnergy, 0.05f));

        var sun = new DirectionalLight3D { LightEnergy = 1.5f }.ComputeFields();
        T.Check("sun: kind directional, lux = energy * calibration", sun.Kind == 2 && T.Near(sun.Intensity, 1.5f * Light3D.LuxPerEnergy, 0.05f));
        T.Check("pushing without an engine does nothing and logs nothing", T.ErrorCount() == 0);
    }

    private static void OneSun()
    {
        T.Reset();
        var tree = SceneTree.Instance;
        var a = new DirectionalLight3D { Name = "A" };
        var b = new DirectionalLight3D { Name = "B" };
        T.Check("no sun before any directional light is in the tree", DirectionalLight3D.Sun == null && !a.IsSun);
        tree.Root.AddChild(a);
        tree.Root.AddChild(b);
        T.Check("the first directional light to enter the tree is the sun", a.IsSun && !b.IsSun && ReferenceEquals(DirectionalLight3D.Sun, a));
        T.CheckVec("unrotated, the sun is behind the node (+Z in Godot) -> Aver -X", new Vector3(-1, 0, 0), new Vector3(a.SunDirection().X, a.SunDirection().Y, a.SunDirection().Z));
        a.Rotation = new Vector3(-MathF.PI / 2, 0, 0);   // looking straight down
        var d = a.SunDirection();
        T.CheckVec("a light pitched to look straight down puts the sun overhead (+Z in Aver)", new Vector3(0, 0, 1), new Vector3(d.X, d.Y, d.Z), 1e-4f);
        a.QueueFree();
        T.Frames(2, 1.0 / 60.0);
        var c = new DirectionalLight3D { Name = "C" };
        tree.Root.AddChild(c);
        T.Check("when the sun node leaves, the next directional light to enter takes over", c.IsSun && !a.IsInsideTree());
        SceneTree.Reset();
        T.Check("a reset forgets the sun", DirectionalLight3D.Sun == null);
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
