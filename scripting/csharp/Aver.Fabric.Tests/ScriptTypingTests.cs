// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Converted scripts that compile with no errors and call what they can by type (docs/plans/fabric/G3_SCRIPTS.md): the runtime classes that closed the
// compile errors of two real Godot demos (CanvasLayer, screen touch events, DisplayServer, the RenderingServer shadow and method calls, the sun's sky
// mode, per-contact reads of a rigid body, get_gravity, set_as_top_level, Basis[i]) and the converter patterns around them (built-in constructors,
// typed $Path / get_node / %Unique / instantiate from Fabric/nodes.json, parameters typed from their call sites).

using System.Reflection;
using Aver.Fabric;
using Aver.Fabric.Convert;

static class ScriptTypingTests
{
    public static int RunAll() => T.Suite(() =>
    {
        T.Run("runtime: Basis[i] is the column, and a Basis Variant indexes the same way", BasisColumns);
        T.Run("runtime: CanvasLayer hides what is drawn on it, nested layers too", CanvasLayerVisibility);
        T.Run("runtime: RenderingServer method and shadow quality, DirectionalLight3D.sky_mode, DisplayServer", RenderingSettings);
        T.Run("runtime: InputEventScreenTouch and InputEventScreenDrag reach _Input", ScreenEvents);
        T.Run("runtime: set_as_top_level keeps the global transform inside the tree; get_gravity; reset_physics_interpolation", TopLevelAndGravity);
        T.Run("runtime: per-contact reads without a body answer nothing", ContactReadsWithoutBody);
        T.Run("convert: Basis(axis, angle), Basis(x, y, z), Basis.IDENTITY and basis[i] in a Node3D script are typed", ConvertBasis);
        T.Run("convert: RenderingServer, sky_mode, CanvasLayer, DisplayServer and touch events compile and run", ConvertEngineCalls);
        T.Run("convert: get_gravity, per-contact reads, set_as_top_level and an `or` of type tests are typed", ConvertPhysics);
        T.Run("convert: $Path, get_node, %Unique and instantiate take their type from the scenes (nodes.json)", ConvertSceneTyping);
        T.Run("convert: where the scenes disagree the nearest shared class is used, and a miss stays a call by name", ConvertSceneDisagree);
        T.Run("convert: a path the script casts elsewhere ($Model/Skeleton as Node3D) is typed by that cast", ConvertCastPaths);
        T.Run("convert: an untyped parameter every call site fills with one math type is that type", ConvertParamHints);
    });

    // ---- helpers ----

    private static ConversionResult Convert(IReadOnlyList<ScriptSource> sources, string? sceneNodes = null) =>
        ScriptConverter.Convert(sources, new ConvertSettings { RefsDir = AppContext.BaseDirectory, KeepAssembly = true, ExtraSources = System.Array.Empty<(string, string)>(), SceneNodes = sceneNodes });

    private static ScriptSource Gd(string resPath, string cls, string text) =>
        new() { ResPath = resPath, Text = text, Kind = "gdscript", ClassName = cls, Output = "Content/Scripts/" + cls + ".cs" };

    private static string Errors(ConversionResult r) => string.Join("; ", r.Errors.Take(4).Select(e => e.ToString())) + string.Join("; ", r.GeneralErrors);

    private static string Cs(ConversionResult r, string cls) => r.Scripts.First(s => s.ClassName == cls).Text;

    private static bool Has(string text, params string[] parts) => parts.All(text.Contains);

    private static bool Throws(Action a)
    {
        try { a(); return false; }
        catch (Exception) { return true; }
    }

    private static object Make(ConversionResult r, string cls) => Activator.CreateInstance(Assembly.Load(r.Assembly!).GetType(cls)!)!;

    private static object? Call(object o, string method, params object[] args) => o.GetType().GetMethod(method)!.Invoke(o, args);

    private static object? Field(object o, string name) => o.GetType().GetField(name)!.GetValue(o);

    // A node of nodes.json.
    private static string N(string path, string @base, string cls = "", string script = "", string owner = ".", bool unique = false, string packed = "") =>
        $"{{\"path\":\"{path}\",\"base\":\"{@base}\",\"class\":\"{(cls.Length > 0 ? cls : @base)}\",\"script\":\"{script}\",\"owner\":\"{(path == "." ? "" : owner)}\",\"unique\":{(unique ? "true" : "false")},\"packed\":{{{packed}}}}}";

    private static string Scene(string scene, params string[] nodes) => $"{{\"scene\":\"{scene}\",\"nodes\":[{string.Join(",", nodes)}]}}";

    private static string Nodes(params string[] scenes) => $"{{\"schema\":1,\"scenes\":[{string.Join(",", scenes)}]}}";

    // ---- the runtime ----

    private static void BasisColumns()
    {
        var b = new Basis(new Vector3(1, 2, 3), new Vector3(4, 5, 6), new Vector3(7, 8, 9));
        T.CheckVec("b[0] is the X column", new Vector3(1, 2, 3), b[0]);
        T.CheckVec("b[1] is the Y column", new Vector3(4, 5, 6), b[1]);
        T.CheckVec("b[2] is the Z column", new Vector3(7, 8, 9), b[2]);
        T.Check("and the same as b[column, row]", b[1][2] == b[1, 2] && b[2][0] == b[2, 0]);
        b[1] = new Vector3(0, 1, 0);
        T.CheckVec("setting a column writes it", new Vector3(0, 1, 0), b.Column1);
        T.Check("an index past the third column throws", Throws(() => _ = b[3]));
        T.CheckVec("GDScript.Index reads a Basis held in a Variant", new Vector3(7, 8, 9), GDScript.Index(Variant.From(b), 2).AsVector3());
    }

    private static void CanvasLayerVisibility()
    {
        var layer = new CanvasLayer();
        var panel = new Control();
        var inner = new Control();
        layer.AddChild(panel);
        panel.AddChild(inner);
        T.Check("a layer outside the tree shows nothing", !layer.IsVisibleInTree() && !panel.IsVisibleInTree());
        SceneTree.Instance.Root.AddChild(layer);
        T.Check("a layer is shown by default, on layer 1, and so is what is on it", layer.Visible && layer.Layer == 1 && panel.IsVisibleInTree() && inner.IsVisibleInTree());
        int changes = 0;
        layer.VisibilityChanged += () => changes++;
        layer.Hide();
        T.Check("hiding the layer hides every Control below it, whose own flag is untouched", !panel.IsVisibleInTree() && !inner.IsVisibleInTree() && panel.Visible && inner.Visible && changes == 1);
        layer.Hide();
        T.Check("hiding it again signals nothing", changes == 1);
        layer.Show();
        T.Check("showing it brings them back", panel.IsVisibleInTree() && inner.IsVisibleInTree() && changes == 2);

        var outer = new CanvasLayer();
        var nested = new CanvasLayer();
        var leaf = new Control();
        outer.AddChild(nested);
        nested.AddChild(leaf);
        SceneTree.Instance.Root.AddChild(outer);
        outer.Hide();
        T.Check("a hidden layer hides the layers below it", !nested.IsVisibleInTree() && !leaf.IsVisibleInTree() && nested.Visible);
        outer.Show();
        var control = new Control { Visible = false };
        nested.AddChild(control);
        T.Check("a Control's own flag still hides it", !control.IsVisibleInTree() && leaf.IsVisibleInTree());

        layer.Offset = new Vector2(3, 4);
        layer.RotationDegrees = 90f;
        layer.Scale = new Vector2(2, 2);
        T.Check("offset, rotation and scale are kept and make the transform", layer.Transform.Origin == new Vector2(3, 4) && T.Near(layer.Rotation, Mathf.Pi / 2) && T.Near(layer.GetFinalTransform().GetScale().X, 2f));
    }

    private static void RenderingSettings()
    {
        T.Check("the rendering method is forward_plus, so a gl_compatibility workaround is skipped", RenderingServer.GetCurrentRenderingMethod() == "forward_plus");
        T.Check("the driver name is one of Godot's", RenderingServer.GetCurrentRenderingDriverName() is "d3d12" or "vulkan" or "metal");
        T.Check("ShadowQuality has Godot's numbers", (int)RenderingServer.ShadowQuality.Hard == 0 && (int)RenderingServer.ShadowQuality.SoftHigh == 4 && (int)RenderingServer.ShadowQuality.Max == 6);
        RenderingServer.DirectionalSoftShadowFilterSetQuality(RenderingServer.ShadowQuality.SoftHigh);
        RenderingServer.PositionalSoftShadowFilterSetQuality(RenderingServer.ShadowQuality.SoftLow);
        T.Check("the filter qualities are kept as set", RenderingServer.DirectionalSoftShadowFilter == RenderingServer.ShadowQuality.SoftHigh && RenderingServer.PositionalSoftShadowFilter == RenderingServer.ShadowQuality.SoftLow);
        var sun = new DirectionalLight3D();
        T.Check("sky_mode defaults to light and sky", sun.SkyMode == DirectionalLight3D.SkyModeEnum.LightAndSky);
        sun.SkyMode = DirectionalLight3D.SkyModeEnum.SkyOnly;
        sun.Set("sky_mode", 1);
        T.Check("sky_mode has Godot's numbers and is kept (also when set by name, as a scene does)", (int)DirectionalLight3D.SkyModeEnum.SkyOnly == 2 && sun.SkyMode == DirectionalLight3D.SkyModeEnum.LightOnly);
        T.Check("a desktop has no touch screen", !DisplayServer.IsTouchscreenAvailable());
    }

    private sealed class Probe : Node
    {
        public readonly List<InputEvent> Seen = new();
        public override void _Input(InputEvent @event) => Seen.Add(@event);
    }

    private static void ScreenEvents()
    {
        var probe = new Probe();
        SceneTree.Instance.Root.AddChild(probe);
        var touch = new InputEventScreenTouch { Index = 1, Position = new Vector2(10, 20), Pressed = true, DoubleTap = true };
        Input.ParseInputEvent(touch);
        var drag = new InputEventScreenDrag { Index = 1, Position = new Vector2(12, 22), Relative = new Vector2(2, 2), Velocity = new Vector2(120, 120) };
        Input.ParseInputEvent(drag);
        Input.ParseInputEvent(new InputEventScreenTouch { Index = 1, Position = new Vector2(12, 22), Pressed = false });
        T.Check("the touch, the drag and the release reached _Input in order", probe.Seen.Count == 3 && probe.Seen[0] == touch && probe.Seen[1] == drag);
        T.Check("a touch is a press, with its finger and position", touch.IsPressed() && !touch.IsReleased() && touch.Index == 1 && touch.Position == new Vector2(10, 20) && touch.DoubleTap);
        T.Check("the release is a release and a drag is neither", probe.Seen[2].IsReleased() && !drag.IsPressed() && !drag.IsReleased() && drag.Relative == new Vector2(2, 2));
        T.Check("neither presses an action by itself", !Input.IsAnythingPressed());
    }

    private static void TopLevelAndGravity()
    {
        var parent = new Node3D { Position = new Vector3(10, 0, 0) };
        var child = new Node3D { Position = new Vector3(1, 0, 0) };
        parent.AddChild(child);
        SceneTree.Instance.Root.AddChild(parent);
        T.CheckVec("before: the child is at the parent's place plus its own", new Vector3(11, 0, 0), child.GlobalPosition);
        child.SetAsTopLevel(true);
        T.Check("set_as_top_level(true) reads back", child.IsSetAsTopLevel() && child.TopLevel);
        T.CheckVec("the child stays where it was", new Vector3(11, 0, 0), child.GlobalPosition);
        T.CheckVec("and its own transform is now the global one", new Vector3(11, 0, 0), child.Position);
        parent.Position = new Vector3(50, 0, 0);
        T.CheckVec("moving the parent no longer moves it", new Vector3(11, 0, 0), child.GlobalPosition);
        child.TopLevel = false;
        T.CheckVec("and turning it off keeps the place again, now relative to the parent", new Vector3(11, 0, 0), child.GlobalPosition);
        T.CheckVec("so its own position is the difference", new Vector3(-39, 0, 0), child.Position);
        var early = new Node3D { Position = new Vector3(1, 2, 3) };
        early.SetAsTopLevel(true);
        T.CheckVec("outside the tree nothing is moved", new Vector3(1, 2, 3), early.Position);

        var body = new CharacterBody3D();
        T.CheckVec("get_gravity before the body is in the engine is the project's gravity", new Vector3(0, -9.8f, 0), body.GetGravity(), 1e-3f);
        new RigidBody3D().ResetPhysicsInterpolation();
        T.Check("reset_physics_interpolation exists and does nothing", true);
    }

    private static void ContactReadsWithoutBody()
    {
        var rb = new RigidBody3D { ContactMonitor = true, MaxContactsReported = 4 };
        var state = new PhysicsDirectBodyState3D { Owner = rb };
        T.Check("no contact, no body: nothing is returned for any index", state.GetContactColliderObject(0) == null && state.GetContactColliderId(0) == 0 &&
                state.GetContactColliderRid(0).Id == 0 && state.GetContactLocalNormal(0) == Vector3.Zero && state.GetContactLocalPosition(0) == Vector3.Zero &&
                state.GetContactColliderPosition(0) == Vector3.Zero && state.GetContactLocalVelocityAtPosition(0) == Vector3.Zero && state.GetContactColliderVelocityAtPosition(0) == Vector3.Zero);
        T.Check("and an index below zero is the same", state.GetContactLocalNormal(-1) == Vector3.Zero && state.GetContactColliderObject(-1) == null);
        T.Check("a state with no owner answers the same", new PhysicsDirectBodyState3D().GetContactLocalNormal(0) == Vector3.Zero);
    }

    // ---- converted GDScript ----

    private static void ConvertBasis()
    {
        const string rot =
            "extends Node3D\n\nfunc turned(v: Vector3) -> Vector3:\n\treturn Basis(Vector3.UP, deg_to_rad(90.0)) * v\n\n" +
            "func columns(b: Basis) -> Vector3:\n\treturn -b[0].normalized() + b[2]\n\n" +
            "func identity() -> Basis:\n\treturn Basis.IDENTITY\n\n" +
            "func from_cols() -> Basis:\n\treturn Basis(Vector3(0, 0, 1), Vector3.UP, Vector3(-1, 0, 0))\n\n" +
            "func by_variant(b):\n\treturn b[1]\n";
        ConversionResult r = Convert(new[] { Gd("res://rot.gd", "Rot", rot) });
        T.Check("the script compiles", r.Success, Errors(r));
        if (!r.Success) return;
        string cs = Cs(r, "Rot");
        T.Check("Basis(axis, angle) is the constructor, not a call by name (the Node3D property Basis did not shadow the type)", Has(cs, "new Basis(Vector3.Up, ") && !cs.Contains("Call(\"Basis\""), cs);
        T.Check("Basis(x, y, z) is the constructor with the columns", cs.Contains("new Basis(new Vector3(0f, 0f, 1f), Vector3.Up, new Vector3(-1f, 0f, 0f))"));
        T.Check("basis[i] is the typed column, and Basis.IDENTITY the type's constant", Has(cs, "-b[0].Normalized() + b[2]", "return Basis.Identity;"));
        T.Check("nothing in the file is called or read by name", !cs.Contains(".Call(") && !cs.Contains(".Get(") && !cs.Contains("GetMember"));
        object rot3 = Make(r, "Rot");
        T.CheckVec("a quarter turn about up takes right to back", new Vector3(0, 0, -1), (Vector3)Call(rot3, "Turned", Vector3.Right)!);
        T.CheckVec("-b[0].normalized() + b[2] on a scaled basis", new Vector3(-1, 0, 4), (Vector3)Call(rot3, "Columns", new Basis(new Vector3(2, 0, 0), new Vector3(0, 3, 0), new Vector3(0, 0, 4)))!);
        T.Check("Basis.IDENTITY", ((Basis)Call(rot3, "Identity")!).Equals(Basis.Identity));
        var cols = (Basis)Call(rot3, "FromCols")!;
        T.CheckVec("Basis(a, b, c) takes columns", new Vector3(0, 0, 1), cols.Column0);
        T.CheckVec("and the third column is the third argument", new Vector3(-1, 0, 0), cols.Column2);
        T.CheckVec("an untyped basis indexes through the Variant helper to the column too", new Vector3(0, 3, 0),
                   ((Variant)Call(rot3, "ByVariant", Variant.From(new Basis(new Vector3(2, 0, 0), new Vector3(0, 3, 0), new Vector3(0, 0, 4))))!).AsVector3());
    }

    private static void ConvertEngineCalls()
    {
        const string rs =
            "extends Node3D\n\nvar method := \"\"\nvar mode := 0\n\nfunc _ready():\n\tmethod = RenderingServer.get_current_rendering_method()\n" +
            "\tif RenderingServer.get_current_rendering_method() == \"gl_compatibility\":\n\t\tmode = -1\n" +
            "\tRenderingServer.directional_soft_shadow_filter_set_quality(RenderingServer.SHADOW_QUALITY_SOFT_HIGH)\n" +
            "\tvar light := DirectionalLight3D.new()\n\tlight.sky_mode = DirectionalLight3D.SKY_MODE_SKY_ONLY\n\tadd_child(light)\n" +
            "\tmode = DirectionalLight3D.SKY_MODE_LIGHT_ONLY\n";
        const string ui =
            "extends CanvasLayer\n\nvar touched := Vector2.ZERO\nvar dragged := Vector2.ZERO\nvar finger := -1\n\nfunc _ready() -> void:\n\thide()\n" +
            "\tif DisplayServer.is_touchscreen_available():\n\t\tshow()\n\nfunc _input(event):\n" +
            "\tif event is InputEventScreenTouch:\n\t\tif event.pressed:\n\t\t\ttouched = event.position\n\t\t\tfinger = event.index\n" +
            "\telif event is InputEventScreenDrag:\n\t\tdragged = event.relative\n";
        ConversionResult r = Convert(new[] { Gd("res://rs.gd", "Rs", rs), Gd("res://touch_ui.gd", "TouchUi", ui) });
        T.Check("both scripts compile: no member of these classes is missing", r.Success, Errors(r));
        if (!r.Success) return;
        string a = Cs(r, "Rs"), b = Cs(r, "TouchUi");
        T.Check("RenderingServer and the sun's sky mode are typed calls and enum members", Has(a, "RenderingServer.GetCurrentRenderingMethod()", "RenderingServer.DirectionalSoftShadowFilterSetQuality(RenderingServer.ShadowQuality.SoftHigh)",
                "light.SkyMode = DirectionalLight3D.SkyModeEnum.SkyOnly", "DirectionalLight3D.SkyModeEnum.LightOnly") && !a.Contains(".Call(") && !a.Contains(".Set("), a);
        T.Check("a CanvasLayer script is a CanvasLayer with typed hide / show and DisplayServer", Has(b, ": CanvasLayer", "override void _Ready()", "Hide();", "DisplayServer.IsTouchscreenAvailable()", "Show();") && !b.Contains("Call(") && !b.Contains("Get("), b);
        T.Check("the touch events are typed members", Has(b, "is InputEventScreenTouch", "is InputEventScreenDrag", ".Pressed", ".Position", ".Index", ".Relative") && !b.Contains("GetMember"), b);

        object rso = Make(r, "Rs");
        SceneTree.Instance.Root.AddChild((Node)rso);
        T.Check("the method is forward_plus and the gl_compatibility branch did not run", (string)Field(rso, "Method")! == "forward_plus" && (long)Field(rso, "Mode")! != -1);
        T.Check("the shadow filter quality reached the runtime", RenderingServer.DirectionalSoftShadowFilter == RenderingServer.ShadowQuality.SoftHigh);
        T.Check("the new light has the sky mode the script set", ((Node)rso).GetChildren().OfType<DirectionalLight3D>().Any(l => l.SkyMode == DirectionalLight3D.SkyModeEnum.SkyOnly));

        var layer = (CanvasLayer)Make(r, "TouchUi");
        var panel = new Control();
        layer.AddChild(panel);
        SceneTree.Instance.Root.AddChild(layer);
        T.Check("_ready hid the layer (no touch screen), so its Controls are not shown", !layer.Visible && !panel.IsVisibleInTree());
        Input.ParseInputEvent(new InputEventScreenTouch { Index = 2, Position = new Vector2(7, 9), Pressed = true });
        Input.ParseInputEvent(new InputEventScreenDrag { Index = 2, Relative = new Vector2(1, -1) });
        T.Check("the touch and the drag reached the converted _input", (Vector2)Field(layer, "Touched")! == new Vector2(7, 9) && (long)Field(layer, "Finger")! == 2 && (Vector2)Field(layer, "Dragged")! == new Vector2(1, -1));
    }

    private static void ConvertPhysics()
    {
        const string rb =
            "extends RigidBody3D\n\nvar hits := 0\n\nfunc _integrate_forces(state):\n\tvar up = -get_gravity().normalized()\n\tvar g = get_gravity()\n" +
            "\tfor i in range(state.get_contact_count()):\n\t\tvar other = state.get_contact_collider_object(i)\n\t\tvar n = state.get_contact_local_normal(i)\n" +
            "\t\tvar p = state.get_contact_local_position(i)\n\t\tvar id = state.get_contact_collider_id(i)\n" +
            "\t\tif other and n.dot(up) > 0.5:\n\t\t\thits += 1\n\t\t\tstate.linear_velocity = state.get_contact_local_velocity_at_position(i) + p + g\n";
        const string ch =
            "extends CharacterBody3D\n\nvar found := false\n\nfunc _ready():\n\tvar node: Node = self\n\twhile is_instance_valid(node):\n" +
            "\t\tif node is RigidBody3D or node is CharacterBody3D:\n\t\t\tvar rid = node.get_rid()\n\t\t\tfound = rid.get_id() == 0\n\t\t\tbreak\n" +
            "\t\telse:\n\t\t\tnode = node.get_parent()\n\tset_as_top_level(true)\n\tvelocity += get_gravity() * 0.5\n";
        ConversionResult r = Convert(new[] { Gd("res://rb.gd", "Rb", rb), Gd("res://ch.gd", "Ch", ch) });
        T.Check("both scripts compile: the per-contact reads, get_gravity, set_as_top_level and get_rid exist", r.Success, Errors(r));
        if (!r.Success) return;
        string a = Cs(r, "Rb"), b = Cs(r, "Ch");
        T.Check("the contact reads and get_gravity are typed calls", Has(a, "GetGravity()", "state.GetContactColliderObject((int)i)", "state.GetContactLocalNormal((int)i)", "state.GetContactLocalPosition((int)i)",
                "state.GetContactColliderId((int)i)", "state.GetContactLocalVelocityAtPosition((int)i)") && !a.Contains("Call(") && !a.Contains("CallMethod") && !a.Contains("GetMember"), a);
        T.Check("`node is A or node is B` leaves node as the class they share inside the branch", Has(b, "((PhysicsBody3D)node).GetRid()", "SetAsTopLevel(true)", "GetGravity() * 0.5f") && !b.Contains("Call("), b);
        T.Check("outside the branch node is still a Node (the else branch reassigns it)", Has(b, "node = node.GetParent<Node>();"));

        var parent = new Node3D { Position = new Vector3(10, 0, 0) };
        var body = (CharacterBody3D)Make(r, "Ch");
        body.Position = new Vector3(1, 0, 0);
        parent.AddChild(body);
        SceneTree.Instance.Root.AddChild(parent);
        T.CheckVec("set_as_top_level in _ready kept the camera-style detach from moving the node", new Vector3(11, 0, 0), body.GlobalPosition);
        T.Check("and the body is detached from its parent", body.TopLevel);
        T.CheckVec("velocity += get_gravity() * 0.5", new Vector3(0, -4.9f, 0), body.Velocity, 1e-3f);
        T.Check("get_rid ran on the body found by the type test", (bool)Field(body, "Found")!);
    }

    private const string MainScript =
        "extends Node3D\n\n@export var mob_scene: PackedScene\n@onready var anim = $Anim\n\n" +
        "func start():\n\t$Anim.play(\"run\")\n\t$Sound.play()\n\t%Title.text = \"go\"\n\t$Hud.hide()\n\tget_node(\"Hud/Panel\").visible = false\n" +
        "\tvar mob = mob_scene.instantiate()\n\tmob.initialize(Vector3.ONE, Vector3(0, 0, 1))\n\tadd_child(mob)\n\tget_node(\"Missing\").foo()\n" +
        "\t$Hud.get_node(\"Panel\").hide()\n\treturn mob\n\n" +
        "func tidy():\n\t$Hud.hide()\n\t%Title.text = \"done\"\n\tget_node(\"Hud/Panel\").visible = true\n\tvar t = $Title\n\treturn t.text\n";

    private const string MobScript =
        "class_name Mob\nextends CharacterBody3D\n\nfunc initialize(start_position, player_position):\n\tvar target = Vector3(player_position.x, start_position.y, player_position.z)\n" +
        "\tlook_at_from_position(start_position, target, Vector3.UP)\n";

    private static string MainScenes() => Nodes(
        Scene("res://main.tscn",
            N(".", "Node3D", "Main", "res://main.gd", packed: "\"mob_scene\":\"res://mob.tscn\""),
            N("Anim", "AnimationPlayer"), N("Sound", "AudioStreamPlayer"), N("Title", "Label3D", unique: true),
            N("Hud", "CanvasLayer"), N("Hud/Panel", "Control")),
        Scene("res://mob.tscn", N(".", "CharacterBody3D", "Mob", "res://mob.gd")));

    private static void ConvertSceneTyping()
    {
        ConversionResult r = Convert(new[] { Gd("res://main.gd", "Main", MainScript), Gd("res://mob.gd", "Mob", MobScript) }, MainScenes());
        T.Check("both scripts compile", r.Success, Errors(r));
        if (!r.Success) return;
        string cs = Cs(r, "Main");
        T.Check("$Anim.play(...) and $Sound.play() are typed calls on the node classes the scene placed",
                Has(cs, "GetNode<AnimationPlayer>(\"Anim\").Play(\"run\")", "GetNode<AudioStreamPlayer>(\"Sound\").Play()"), cs);
        T.Check("%Title.text is the Label3D's Text, and $Hud / get_node(\"Hud/Panel\") the CanvasLayer and the Control",
                Has(cs, "GetNode<Label3D>(\"%Title\").Text = \"go\"", "GetNode<CanvasLayer>(\"Hud\").Hide()", "GetNode<Control>(\"Hud/Panel\").Visible = false"), cs);
        T.Check("a path from a node whose place is known is followed ($Hud.get_node(\"Panel\"))", cs.Contains("GetNode<CanvasLayer>(\"Hud\").GetNode<Control>(\"Panel\").Hide()"), cs);
        T.Check("@onready var anim = $Anim is a typed field", Has(cs, "public AnimationPlayer Anim;", "Anim = GetNode<AnimationPlayer>(\"Anim\");"), cs);
        T.Check("scene.instantiate() of an export set to mob.tscn is Instantiate<Mob>, and mob.initialize(...) a typed call",
                Has(cs, "Mob mob = MobScene.Instantiate<Mob>();", "mob.Initialize(Vector3.One, new Vector3(0f, 0f, 1f))"), cs);
        T.Check("what no scene knows ($Missing) stays a call by name and says so", Has(cs, "GetNode(\"Missing\").Call(\"foo\")") && r.Scripts.First(s => s.ClassName == "Main").Items.Any(i => i.Type == "dynamic" && i.Property == "foo"), cs);
        T.Check("no member of the typed nodes is read by name", !cs.Contains(".Set(\"text\"") && !cs.Contains(".Get(\"visible\")") && !cs.Contains("GetMember"), cs);

        // Run the part that does not need an animation or audio engine: tidy().
        var mainType = Assembly.Load(r.Assembly!).GetType("Main")!;
        var main = (Node3D)Activator.CreateInstance(mainType)!;
        var title = new Label3D { Name = "Title", UniqueNameInOwner = true };
        var hud = new CanvasLayer { Name = "Hud" };
        var panel = new Control { Name = "Panel", Visible = false };
        hud.AddChild(panel);
        main.AddChild(title);
        main.AddChild(hud);
        title.Owner = main;
        try { main.AddChild(new AnimationPlayer { Name = "Anim" }); } catch (Exception) { }
        SceneTree.Instance.Root.AddChild(main);
        object? text = null;
        try { text = Call(main, "Tidy"); }
        catch (TargetInvocationException ex) { Console.WriteLine("  tidy threw: " + ex.InnerException); }
        T.Check("tidy(): the unique name resolved, the layer hid its Control, get_node set a Control's flag, $Title read back", title.Text == "done" && !hud.Visible && panel.Visible && !panel.IsVisibleInTree() && text?.ToString() == "done", text?.ToString());
        T.Check("the function's own type: `var t = $Title; return t.text` is a typed string", mainType.GetMethod("Tidy")!.ReturnType == typeof(string));
    }

    private static void ConvertSceneDisagree()
    {
        const string script = "extends Node3D\n\nfunc f():\n\t$Thing.hide()\n\t$Thing.nothing_here()\n\t%Name.show()\n\t$Other.go()\n";
        string nodes = Nodes(
            Scene("res://a.tscn", N(".", "Node3D", "Pair", "res://pair.gd"), N("Thing", "MeshInstance3D"), N("Name", "Node3D", unique: true), N("Other", "Node3D", "Pair", "res://pair.gd")),
            Scene("res://b.tscn", N(".", "Node3D", "Pair", "res://pair.gd"), N("Thing", "Marker3D"), N("Name", "Node3D", unique: true)));
        ConversionResult r = Convert(new[] { Gd("res://pair.gd", "Pair", script) }, nodes);
        T.Check("the script compiles", r.Success, Errors(r));
        if (!r.Success) return;
        string cs = Cs(r, "Pair");
        T.Check("a MeshInstance3D in one scene and a Marker3D in the other: the node they share is Node3D, and Hide is its", cs.Contains("GetNode<Node3D>(\"Thing\").Hide()"), cs);
        T.Check("a method neither scene's class has is called by name on the typed node", cs.Contains("GetNode<Node3D>(\"Thing\").Call(\"nothing_here\")"), cs);
        T.Check("a %Unique path resolves in the scenes that have it", cs.Contains("GetNode<Node3D>(\"%Name\").Show()"), cs);
        T.Check("a path one scene lacks is typed from the scene that has it", cs.Contains("GetNode<Pair>(\"Other\").Go()") || cs.Contains("GetNode<Pair>(\"Other\")"), cs);
        // Without nodes.json nothing is typed and the output is what it was.
        ConversionResult plain = Convert(new[] { Gd("res://pair.gd", "Pair", script) });
        T.Check("without the scene list the same script keeps its calls by name", plain.Success && Cs(plain, "Pair").Contains("GetNode(\"Thing\").Call(\"hide\")"), Cs(plain, "Pair"));
    }

    private static void ConvertCastPaths()
    {
        const string script =
            "extends Node3D\n\nfunc f():\n\tvar skel = $Model/Skeleton as Node3D\n\t$Model/Skeleton.set_transform(skel.get_transform())\n\tget_node(\"Model/Skeleton\").hide()\n" +
            "\t$Model/Other.hide()\n\tvar a: Node3D = $Both\n\tvar b: MeshInstance3D = $Both\n\t$Both.hide()\n";
        ConversionResult r = Convert(new[] { Gd("res://cast.gd", "Cast", script) });
        T.Check("the script compiles", r.Success, Errors(r));
        if (!r.Success) return;
        string cs = Cs(r, "Cast");
        T.Check("set_transform and hide on the cast path are typed members (no scene list needed)", Has(cs, "GetNode<Node3D>(\"Model/Skeleton\").Transform = skel.Transform", "GetNode<Node3D>(\"Model/Skeleton\").Hide()"), cs);
        T.Check("a path nothing casts stays a call by name", cs.Contains("GetNode(\"Model/Other\").Call(\"hide\")"), cs);
        T.Check("a path cast two ways says nothing", cs.Contains("GetNode(\"Both\").Call(\"hide\")"), cs);
    }

    private static void ConvertParamHints()
    {
        const string callee = "class_name Aim\nextends Node3D\n\nfunc point(origin, target):\n\tvar d = Vector3(target.x, origin.y, target.z)\n\treturn d\n\nfunc untouched(a, b):\n\treturn a\n\nfunc other(a):\n\treturn a\n";
        const string caller =
            "extends Node3D\n\nvar aim: Aim\n\nfunc go() -> Vector3:\n\taim.untouched(Vector3.ONE, 1)\n\taim.untouched(Vector3.ONE, 2)\n\taim.other(Vector3.ONE)\n\taim.other(Vector2.ONE)\n" +
            "\treturn aim.point(Vector3(1, 2, 3), Vector3(4, 5, 6))\n";
        ConversionResult r = Convert(new[] { Gd("res://aim.gd", "Aim", callee), Gd("res://caller.gd", "Caller", caller) });
        T.Check("both scripts compile", r.Success, Errors(r));
        if (!r.Success) return;
        string cs = Cs(r, "Aim");
        T.Check("point(origin, target) is typed by its one call site, and the member reads on it are typed", Has(cs, "public Vector3 Point(Vector3 origin, Vector3 target)", "new Vector3(target.X, origin.Y, target.Z)") && !cs.Contains("GetMember"), cs);
        T.Check("a parameter given a Vector3 by every call site is a Vector3 even when another is a number", cs.Contains("Untouched(Vector3 a, Variant b)"), cs);
        T.Check("a parameter two call sites fill with different types stays a Variant", cs.Contains("Other(Variant a)"), cs);
        object aim = Make(r, "Aim");
        T.CheckVec("the converted function runs", new Vector3(4, 2, 6), (Vector3)Call(aim, "Point", new Vector3(1, 2, 3), new Vector3(4, 5, 6))!);
    }
}
