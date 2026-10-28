// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Aver.Fabric.Convert (G3 F3): single rewrites on small sources, the generated members, the report and the command line, and the
// hand-written GodotSharp project in tests/fabric/data/csharp converted, compiled against this runtime and run in the managed loop.

using System.Reflection;
using System.Text.Json;
using Aver.Fabric;
using Aver.Fabric.Convert;

static class ConvertScriptsTests
{
    public static int RunAll() => T.Suite(() =>
    {
        T.Run("convert: Godot namespaces become Aver.Fabric in usings, aliases, qualified names and typeof", Namespaces);
        T.Run("convert: [Tool] [GlobalClass] [Icon] [Rpc] [Export*] [Signal] are kept or rewritten and reported", Attributes);
        T.Run("convert: a renamed script class is registered with [AverClass], an unrenamed one is not", RegistryName);
        T.Run("convert: header, #nullable, GODOT symbols only when the file asks for them, CRLF kept", Header);
        T.Run("convert: SignalName, events and EmitSignalX are generated, through base classes, and a class is made partial", GeneratedMembers);
        T.Run("convert: a name System.Threading shares with Aver.Fabric (Timer) gets a using alias", AmbiguousNames);
        T.Run("convert: what Aver.Fabric lacks is left, marked // FABRIC: and reported with the exact member and line", MissingMembers);
        T.Run("convert: Godot.Collections names System shares (Dictionary<,>, Array) are settled", CollectionNames);
        T.Run("convert: a field called Godot is not the namespace", NotTheNamespace);
        T.Run("project: the GodotSharp fixture converts and compiles with no errors", FixtureCompiles);
        T.Run("run: Door (exports, [Signal]s connected and emitted)", RunDoor);
        T.Run("run: Countdown (await on a timer, a Timer node)", RunCountdown);
        T.Run("run: Slider (Tween)", RunSlider);
        T.Run("run: Locator (GetNode with paths)", RunLocator);
        T.Run("run: PlayerController (CharacterBody3D, Input, MoveAndSlide) and Pickup (attributes)", RunPlayerAndPickup);
        T.Run("cli: scripts.json decides the class and the output, the report merges, a rerun changes nothing", CliManifestAndReport);
        T.Run("cli: errors give exit 1 with // FABRIC: lines, usage 2, a bad reference folder 3 with the scripts still written", CliExitCodes);
        T.Run("cli: --engine writes Scripts.csproj once, referencing Aver.Fabric", CliScriptsProject);
    });

    // ---- helpers ----

    private static string Refs => AppContext.BaseDirectory;

    private static ConversionResult Run(string text, string cls = "", string res = "res://scripts/Sample.cs", bool compile = true, bool keep = false) =>
        ScriptConverter.Convert(new[] { new ScriptSource { ResPath = res, Text = text, ClassName = cls, Output = "Content/Scripts/Sample.cs" } },
                                new ConvertSettings { RefsDir = compile ? Refs : null, KeepAssembly = keep });

    private static string TextOf(ConversionResult r) => r.Scripts[0].Text;

    private static ReportItem? Item(ConversionResult r, string type, string property) =>
        r.Items.FirstOrDefault(i => i.Type == type && i.Property == property);

    private static string RepoRoot()
    {
        for (DirectoryInfo? d = new DirectoryInfo(T.DataDir()); d != null; d = d.Parent)
            if (Directory.Exists(Path.Combine(d.FullName, "scripting", "csharp", "Aver.Fabric"))) return d.FullName;
        throw new DirectoryNotFoundException("the repository root was not found above tests/fabric/data");
    }

    private static string Temp(string what)
    {
        string p = Path.Combine(Path.GetTempPath(), "fabric_" + what + "_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(p);
        return p;
    }

    private static object? Field(object o, string name) =>
        (o.GetType().GetField(name, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance) ?? throw new MissingFieldException(o.GetType().Name, name)).GetValue(o);

    // ---- single rewrites ----

    private static void Namespaces()
    {
        const string src =
            "using Godot;\nusing Godot.Collections;\nusing static Godot.Mathf;\nusing GArray = Godot.Collections.Array;\nusing Alias = global::Godot.Vector3;\n\n" +
            "public partial class Sample : Node3D\n{\n" +
            "    public Godot.Vector3 A = Godot.Vector3.Zero;\n" +
            "    public global::Godot.Collections.Dictionary<string, int> D = new();\n" +
            "    public int godot = 1;\n" +
            "    public void M() { var x = typeof(Godot.Node); GD.Print(this.godot, x); }\n}\n";
        string text = TextOf(Run(src, compile: false));
        T.Check("using Godot; becomes using Aver.Fabric;", text.Contains("using Aver.Fabric;"));
        T.Check("using Godot.Collections; becomes the Aver.Fabric.Collections namespace", text.Contains("using Aver.Fabric.Collections;"));
        T.Check("using static follows", text.Contains("using static Aver.Fabric.Mathf;"));
        T.Check("an alias target follows", text.Contains("using GArray = Aver.Fabric.Collections.Array;"));
        T.Check("a global:: alias target follows", text.Contains("using Alias = global::Aver.Fabric.Vector3;"));
        T.Check("a qualified type and expression", text.Contains("public Aver.Fabric.Vector3 A = Aver.Fabric.Vector3.Zero;"));
        T.Check("global::Godot.Collections.Dictionary<K,V>", text.Contains("global::Aver.Fabric.Collections.Dictionary<string, int>"));
        T.Check("typeof(Godot.Node)", text.Contains("typeof(Aver.Fabric.Node)"));
        T.Check("no Godot. namespace reference is left", !text.Contains("Godot."));
        T.Check("a lower-case member named godot is not a namespace", text.Contains("public int godot = 1;") && text.Contains("this.godot"));
    }

    private static void NotTheNamespace()
    {
        const string src = "using Godot;\npublic partial class Sample : Node\n{\n    public void M(Sample godot) { var n = godot.Name; var g = Godot; }\n    static int Godot = 3;\n}\n";
        // `Godot` alone (not the left of a qualified name) is a field here and is left alone.
        string text = TextOf(Run(src, compile: false));
        T.Check("a bare identifier Godot is not rewritten", text.Contains("var g = Godot;") && text.Contains("static int Godot = 3;"));
    }

    private static void Attributes()
    {
        const string src =
            "using Godot;\n[Tool]\n[GlobalClass, Icon(\"res://i.svg\")]\npublic partial class Sample : Node3D\n{\n" +
            "    [Signal] public delegate void DoneEventHandler();\n" +
            "    [ExportGroup(\"G\")] [Export] public int A;\n" +
            "    [Rpc(MultiplayerApi.RpcMode.AnyPeer, CallLocal = true)] public void R1() { }\n" +
            "    [Rpc] public void R2() { }\n}\n";
        ConversionResult r = Run(src, compile: false);
        string text = TextOf(r);
        T.Check("[Tool] is kept and marked", text.Contains("[Tool] // FABRIC:"));
        ReportItem? tool = Item(r, "attribute", "Tool");
        // The output has the two header lines in front of the source's.
        T.Check("[Tool] is reported unsupported with its line in the output", tool is { Rating: Rating.Unsupported, Line: 4 }, tool?.ToJson() ?? "no item");
        T.Check("[GlobalClass] is converted", Item(r, "attribute", "GlobalClass") is { Rating: Rating.Converted });
        T.Check("[Icon] is approximated", Item(r, "attribute", "Icon") is { Rating: Rating.Approximated });
        T.Check("[Rpc(mode, options)] becomes [Rpc(\"AnyPeer\")]", text.Contains("[Rpc(\"AnyPeer\")]"));
        T.Check("a bare [Rpc] stays bare", text.Contains("[Rpc] public void R2()"));
        T.Check("both Rpcs are reported approximated", r.Items.Count(i => i.Property == "Rpc" && i.Rating == Rating.Approximated) == 2);
        T.Check("[Export] [ExportGroup] [Signal] are kept as written", text.Contains("[ExportGroup(\"G\")] [Export] public int A;") && text.Contains("[Signal] public delegate void DoneEventHandler();"));
        T.Check("the script's summary line counts the export", r.Scripts[0].Items[0].Reason.Contains("1 [Export] member(s)"));
    }

    private static void RegistryName()
    {
        const string src = "using Godot;\nnamespace Game;\npublic partial class Sample : Node3D { }\n";
        ConversionResult same = Run(src, cls: "Sample", compile: false);
        T.Check("the scene converter's name equals the declared one: no attribute", !TextOf(same).Contains("AverClass"));
        ConversionResult renamed = Run(src, cls: "SampleScript", compile: false);
        string text = TextOf(renamed);
        T.Check("a different name is registered with [AverClass]", text.Contains("[global::Aver.Framework.AverClass(\"SampleScript\")]"));
        T.Check("the attribute sits on the class, above it", text.IndexOf("AverClass", StringComparison.Ordinal) < text.IndexOf("class Sample", StringComparison.Ordinal));
        T.Check("the rename is reported approximated", Item(renamed, "class", "Sample") is { Rating: Rating.Approximated });
        T.Check("the script reports the registry name as its node", renamed.Scripts[0].ClassName == "SampleScript" && renamed.Scripts[0].Items[0].Node == "SampleScript");
        ConversionResult withReal = Run(src, cls: "SampleScript");
        T.Check("and the attribute compiles against Aver.Framework", withReal.Success, string.Join("; ", withReal.Errors));
    }

    private static void Header()
    {
        const string plain = "using Godot;\npublic partial class Sample : Node { }\n";
        string text = TextOf(Run(plain, compile: false));
        T.Check("the first line says where the file came from", text.StartsWith("// FABRIC: converted from res://scripts/Sample.cs by Aver.Fabric.Convert"));
        T.Check("#nullable disable keeps GodotSharp's reference-type rules", text.Contains("#nullable disable\n"));
        T.Check("no GODOT symbols are defined when nothing asks for them", !text.Contains("#define"));

        const string guarded = "#if GODOT\nusing Godot;\n#endif\npublic partial class Sample : Node\n{\n#if GODOT4_2_OR_GREATER\n    public int NewApi;\n#else\n    public int OldApi;\n#endif\n}\n";
        string g = TextOf(Run(guarded, compile: false));
        T.Check("#if GODOT in the source: the symbols are defined in the output", g.Contains("#define GODOT\n") && g.Contains("#define GODOT4_2_OR_GREATER\n"));
        T.Check("and the guarded using is rewritten", g.Contains("using Aver.Fabric;"));
        T.Check("the TOOLS symbol is never defined", !g.Contains("#define TOOLS"));
        ConversionResult gr = Run(guarded);
        T.Check("the guarded source compiles", gr.Success, string.Join("; ", gr.Errors));

        string crlf = TextOf(Run("using Godot;\r\npublic partial class Sample : Node\r\n{\r\n    [Signal] public delegate void XEventHandler();\r\n}\r\n"));
        T.Check("a CRLF file stays CRLF, generated members included", crlf.Contains("EmitSignalX") && !crlf.Replace("\r\n", "").Contains('\n'));
    }

    // ---- generated members ----

    private static void GeneratedMembers()
    {
        const string src =
            "using Godot;\n" +
            "public partial class Base : Node3D\n{\n    [Signal] public delegate void PingEventHandler(int n, Node3D who);\n    public void Beat() { }\n    [Export] public float Rate = 1;\n}\n" +
            "public partial class Child : Base\n{\n" +
            "    [Signal] public delegate void PongEventHandler();\n" +
            "    public override void _Ready() { EmitSignalPong(); }\n" +
            "    public void Run() { EmitSignalPing(3, null); EmitSignal(SignalName.Ping, 1, this); Connect(Base.SignalName.Ping, Callable.From<int, Node3D>(OnPing)); }\n" +
            "    void OnPing(int n, Node3D w) { }\n}\n" +
            "public class Loose : Node\n{\n    [Signal] public delegate void XEventHandler();\n    public void Fire() => EmitSignal(SignalName.X);\n}\n";
        ConversionResult r = Run(src, keep: true);
        T.Check("it compiles", r.Success, string.Join("; ", r.Errors));
        string text = TextOf(r);
        T.Check("SignalName derives from the base script's SignalName", text.Contains("public new class SignalName : global::Base.SignalName"));
        T.Check("a [Signal] delegate gets its event", text.Contains("public event PongEventHandler Pong { add => ConnectEvent(SignalName.Pong, value); remove => DisconnectEvent(SignalName.Pong, value); }"));
        T.Check("and an EmitSignalX helper with the delegate's parameters", text.Contains("protected void EmitSignalPing(int n, global::Aver.Fabric.Node3D who) => EmitSignal(SignalName.Ping, n, who);"));
        T.Check("an override of a name the base table has is declared new", text.Contains("public new static readonly global::Aver.Fabric.StringName _Ready = \"_Ready\";"));
        T.Check("a new name is not", text.Contains("public static readonly global::Aver.Fabric.StringName Beat = \"Beat\";"));
        T.Check("fields and properties go into PropertyName", text.Contains("StringName Rate = \"Rate\";"));
        T.Check("a class that was not partial is made so", text.Contains("public partial class Loose : Node"));
        T.Check("the generated block is marked", text.Contains("// FABRIC: generated."));

        Assembly asm = Assembly.Load(r.Assembly!);
        var child = (Node3D)Activator.CreateInstance(asm.GetType("Child")!)!;
        var seen = new List<string>();
        child.Connect("Ping", Callable.From<int, Node3D>((n, w) => seen.Add("ping" + n)));
        child.Connect("Pong", Callable.From(() => seen.Add("pong")));
        SceneTree.Instance.Root.AddChild(child);          // _Ready emits Pong
        child.Call("Run");                                // EmitSignalPing(3), EmitSignal(Ping, 1)
        T.CheckSeq("the generated helpers and the table reach the signal system", new[] { "pong", "ping3", "ping1" }, seen);
        var loose = (Node)Activator.CreateInstance(asm.GetType("Loose")!)!;
        int fired = 0;
        loose.Connect("X", Callable.From(() => fired++));
        loose.Call("Fire");
        T.Check("a class made partial works", fired == 1);
    }

    private static void AmbiguousNames()
    {
        const string src = "using Godot;\npublic partial class Sample : Node\n{\n    public Timer T1 = new Timer();\n    public Aver.Fabric.Timer T2;\n}\n";
        ConversionResult r = Run(src);
        T.Check("Timer would be System.Threading.Timer or Aver.Fabric.Timer: an alias settles it", r.Success, string.Join("; ", r.Errors));
        T.Check("the alias is in the file", TextOf(r).Contains("using Timer = Aver.Fabric.Timer;"));
        T.Check("and the report says so", r.Scripts[0].Items[0].Reason.Contains("using alias for Timer"));
        ConversionResult none = Run("using Godot;\npublic partial class Sample : Node { }\n");
        T.Check("no alias without an ambiguity", !TextOf(none).Contains("using Timer"));
    }

    private static void CollectionNames()
    {
        const string src =
            "using Godot;\nusing Godot.Collections;\npublic partial class Sample : Node\n{\n" +
            "    public Dictionary<string, int> Scores = new() { [\"a\"] = 2 };\n    public Array<int> Levels = new() { 1, 2, 3 };\n" +
            "    public Array Loose = new();\n    public Godot.Collections.Dictionary Plain = new();\n" +
            "    public int Total() { int t = 0; foreach (var kv in Scores) t += kv.Value; return t + Levels.Count; }\n}\n";
        ConversionResult r = Run(src, keep: true);
        T.Check("Godot.Collections types compile next to the implicit System usings", r.Success, string.Join("; ", r.Errors));
        string text = TextOf(r);
        T.Check("a generic name both namespaces have (Dictionary<,>) is qualified in place", text.Contains("global::Aver.Fabric.Collections.Dictionary<string, int> Scores"));
        T.Check("a plain name both have (Array) gets an alias", text.Contains("using Array = Aver.Fabric.Collections.Array;"));
        T.Check("the typed Array<T> needs neither", text.Contains("public Array<int> Levels"));
        var node = (Node)Activator.CreateInstance(Assembly.Load(r.Assembly!).GetType("Sample")!)!;
        T.Check("and it runs: typed collections hold their values", (int)node.GetType().GetMethod("Total")!.Invoke(node, null)! == 5);
    }

    private static void MissingMembers()
    {
        const string src =
            "using Godot;\npublic partial class Sample : CharacterBody3D\n{\n    private NavigationRegion3D _label;\n    public override void _PhysicsProcess(double delta)\n    {\n        MoveAndSlide();\n" +
            "        ApplyFloorSnap();\n        var p = GetNode<AudioStreamPlayer3D>(\"Voice\");\n    }\n}\n";
        ConversionResult r = Run(src);
        T.Check("it does not compile, and says so", !r.Success && r.Errors.Count == 3, string.Join("; ", r.Errors));
        string text = TextOf(r);
        T.Check("each failing line is marked", text.Split('\n').Count(l => l.Contains("// FABRIC: Aver.Fabric has no")) == 3);
        T.Check("the first mark names the type", text.Contains("private NavigationRegion3D _label; // FABRIC: Aver.Fabric has no 'NavigationRegion3D'"));
        ReportItem? snap = Item(r, "CS0103", "ApplyFloorSnap");
        // Source lines 4, 8 and 9; the output has the two header lines in front.
        T.Check("the missing method is reported by its exact name, unsupported, with its line", snap is { Rating: Rating.Unsupported, Line: 10 }, snap?.ToJson() ?? "no item");
        T.Check("the missing generic argument is reported", Item(r, "CS0246", "AudioStreamPlayer3D") is { Line: 11 });
        T.Check("the item names the class it is in", snap?.Node == "Sample");
        T.Check("the script as a whole is unsupported", r.Scripts[0].Items[0].Rating == Rating.Unsupported && r.Scripts[0].ErrorCount == 3);
        T.Check("an error carries the output path and position", r.Errors.Any(e => e.Output == "Content/Scripts/Sample.cs" && e.Line == 6 && e.Member == "NavigationRegion3D"));
        T.Check("and the mark is on that very line", text.Split('\n')[5].Contains("// FABRIC:") && text.Split('\n')[9].Contains("ApplyFloorSnap"));
    }

    // ---- the fixture project ----

    private static List<ScriptSource> FixtureSources(out Dictionary<string, string> classNames)
    {
        string dir = Path.Combine(T.DataDir(), "csharp");
        classNames = new Dictionary<string, string>();
        var list = new List<ScriptSource>();
        foreach (string f in Directory.GetFiles(Path.Combine(dir, "scripts"), "*.cs").OrderBy(p => p, StringComparer.Ordinal))
        {
            string name = Path.GetFileNameWithoutExtension(f);
            list.Add(new ScriptSource { ResPath = "res://scripts/" + Path.GetFileName(f), Text = File.ReadAllText(f), Output = "Content/Scripts/" + name + ".cs" });
        }
        return list;
    }

    private static ConversionResult? s_fixture;

    private static ConversionResult Fixture()
    {
        if (s_fixture != null) return s_fixture;
        s_fixture = ScriptConverter.Convert(FixtureSources(out _), new ConvertSettings { RefsDir = Refs, KeepAssembly = true });
        return s_fixture;
    }

    private static void FixtureCompiles()
    {
        ConversionResult r = Fixture();
        T.Check("the reference folder held the runtime", r.Compiled && r.GeneralErrors.Count == 0, string.Join("; ", r.GeneralErrors));
        T.Check("seven scripts were converted", r.Scripts.Count == 7);
        T.Check("zero compile errors", r.Errors.Count == 0, string.Join("\n", r.Errors));
        T.Check("an assembly came out", r.Assembly != null && r.Assembly.Length > 0);
        T.Check("nothing is left unconverted by name: no Godot namespace reference remains", r.Scripts.All(s => !s.Text.Contains("Godot.") && !s.Text.Contains("using Godot")));
        T.Check("every script reports a summary entry", r.Scripts.All(s => s.Items[0].Type == "csharp" && s.Items[0].Reason.Contains("compiles against Aver.Fabric")));
        T.Check("Pickup is approximated overall ([Tool] [Rpc] [Icon] notes), the rest converted",
                r.Scripts.Single(s => s.ResPath.EndsWith("Pickup.cs")).Items[0].Rating == Rating.Approximated &&
                r.Scripts.Where(s => !s.ResPath.EndsWith("Pickup.cs")).All(s => s.Items[0].Rating == Rating.Converted));
        T.Check("the #if TOOLS block was not compiled", !Fixture().Scripts.Single(s => s.ResPath.EndsWith("Pickup.cs")).Text.Contains("#define TOOLS"));
    }

    private static Assembly FixtureAssembly() => Assembly.Load(Fixture().Assembly!);

    private static void RunDoor()
    {
        Assembly asm = FixtureAssembly();
        Type type = asm.GetType("Door")!;
        var door = (Node3D)Activator.CreateInstance(type)!;
        T.Check("exports have their initialisers", (int)Field(door, "Health")! == 100 && (string)Field(door, "Label")! == "Door");
        ExportAttribute? mat = type.GetField("Material")!.GetCustomAttribute<ExportAttribute>();
        T.Check("[Export(PropertyHint.Enum, ...)] is kept", mat is { Hint: PropertyHint.Enum, HintString: "Wood,Iron,Glass" });
        T.Check("[ExportGroup] [ExportCategory] [ExportSubgroup] are kept",
                type.GetField("Health")!.GetCustomAttribute<ExportGroupAttribute>()?.Name == "Stats" &&
                type.GetField("Health")!.GetCustomAttribute<ExportCategoryAttribute>()?.Name == "Door" &&
                type.GetField("Tags")!.GetCustomAttribute<ExportSubgroupAttribute>()?.Name == "Lists");
        Type names = type.GetNestedType("SignalName")!;
        T.Check("SignalName.Opened is \"Opened\"", names.GetField("Opened")!.GetValue(null)!.ToString() == "Opened");
        T.Check("MethodName and PropertyName exist", type.GetNestedType("MethodName")!.GetField("Open") != null && type.GetNestedType("PropertyName")!.GetField("Health") != null);

        // The level's Props path: a value set by name before the node enters the tree.
        door.Set("Health", 40);
        var seen = new List<string>();
        door.Connect("Opened", Callable.From<string>(by => seen.Add("opened:" + by)));
        door.Connect("Damaged", Callable.From<int, Node3D>((amount, src) => seen.Add($"damaged:{amount}:{src == null}")));
        door.Connect("Broken", Callable.From(() => seen.Add("broken")));
        SceneTree.Instance.Root.AddChild(door);
        door.Call("Open", "bob");
        door.Call("Open", "again");
        T.Check("EmitSignal(SignalName.Opened, by) reaches the handler and the event handler (+=)", (int)Field(door, "OpenedCount")! == 1 && (string)Field(door, "LastOpenedBy")! == "bob");
        T.Check("and a Connect from outside", seen.Contains("opened:bob") && !seen.Contains("opened:again"));
        door.Call("Hit", 30);
        T.Check("EmitSignalDamaged passes both arguments (a null node too)", seen.Contains("damaged:30:True") && (int)Field(door, "Health")! == 10);
        door.Call("Hit", 30);
        T.Check("Health at zero emits Broken", seen.Contains("broken") && (int)Field(door, "Health")! == -20);
    }

    private static void RunCountdown()
    {
        Assembly asm = FixtureAssembly();
        var node = (Node)Activator.CreateInstance(asm.GetType("Countdown")!)!;
        double elapsed = -1;
        node.Connect("Finished", Callable.From<double>(e => elapsed = e));
        node.Set("Seconds", 0.5);
        SceneTree.Instance.Root.AddChild(node);
        T.Check("_Ready ran: a Timer child was added", node.GetChildCount() == 1 && node.GetChild(0) is Aver.Fabric.Timer);
        T.Frames(20, 1.0 / 60.0);
        T.Check("before the await's timer fires nothing is done", !(bool)Field(node, "Done")!);
        T.Frames(20, 1.0 / 60.0);
        T.Check("`await ToSignal(GetTree().CreateTimer(Seconds), ...)` resumes after Seconds", (bool)Field(node, "Done")! && Math.Abs(elapsed - 0.5) < 1e-9, $"done={Field(node, "Done")} elapsed={elapsed}");
        T.Check("the Timer node's Timeout event counted ticks while waiting", (int)Field(node, "Ticks")! >= 1, "ticks=" + Field(node, "Ticks"));
    }

    private static void RunSlider()
    {
        Assembly asm = FixtureAssembly();
        var slider = (Node3D)Activator.CreateInstance(asm.GetType("Slider")!)!;
        SceneTree.Instance.Root.AddChild(slider);
        bool arrived = false;
        slider.Connect("Arrived", Callable.From(() => arrived = true));
        slider.Call("Slide");
        T.Frames(10, 1.0 / 60.0);
        T.Check("the tween is under way", slider.Position.Y > 0 && slider.Position.Y < 2 && !arrived);
        T.Frames(30, 1.0 / 60.0);
        T.CheckVec("TweenProperty(this, \"position\", start + Offset, Duration) lands on the target", new Vector3(0, 2, 0), slider.Position);
        T.Check("TweenCallback(Callable.From(OnArrived)) ran and emitted Arrived", arrived && (bool)Field(slider, "HasArrived")!);

        var both = (Node3D)Activator.CreateInstance(asm.GetType("Slider")!)!;
        SceneTree.Instance.Root.AddChild(both);
        both.Call("SlideBoth");
        T.Frames(40, 1.0 / 60.0);
        T.Check("a parallel tween with \"position:y\" and \"scale\"", T.Near(both.Position.Y, 2f) && T.Near(both.Scale.X, 2f) && (bool)Field(both, "HasArrived")!);
    }

    private static void RunLocator()
    {
        Assembly asm = FixtureAssembly();
        var rig = new Node3D { Name = "Rig" };
        var locator = (Node3D)Activator.CreateInstance(asm.GetType("Locator")!)!;
        locator.Name = "Locator";
        var body = new Node3D { Name = "Body" };
        body.AddChild(new Node3D { Name = "Mesh" });
        locator.AddChild(body);
        var spawn = new Node3D { Name = "Spawn" };
        rig.AddChild(locator);
        rig.AddChild(spawn);
        SceneTree.Instance.Root.AddChild(rig);          // _Ready runs with the whole subtree in place
        T.Check("GetNode<Node3D>(\"Body/Mesh\")", Field(locator, "Child") is Node3D { Name: var n } && n.ToString() == "Mesh");
        T.Check("GetParent()", ReferenceEquals(Field(locator, "Parent"), rig));
        T.Check("GetNodeOrNull<Node3D>(NodePath) through an exported path (../Spawn)", ReferenceEquals(Field(locator, "Spawn"), spawn));
        T.Check("GetNodeOrNull of a path that does not exist is null", Field(locator, "Missing") == null);
        T.Check("GetNodeOrNull(\"%Special\") with no such unique name is null", Field(locator, "Unique") == null);
        T.CheckSeq("GetChildren() names", new[] { "Body" }, (List<string>)Field(locator, "ChildNames")!);
        T.Check("no error was logged", T.ErrorCount() == 0, string.Join("; ", T.Logs.Where(l => l.Level == Aver.Scripting.Log.Level.Error).Select(l => l.Message)));
    }

    private static void RunPlayerAndPickup()
    {
        Assembly asm = FixtureAssembly();
        Type pt = asm.GetType("CSharpGame.PlayerController")!;
        var player = (CharacterBody3D)Activator.CreateInstance(pt)!;
        T.Check("the exported auto-property and field keep their defaults", (float)pt.GetProperty("Speed")!.GetValue(player)! == 5f && (float)Field(player, "JumpVelocity")! == 4.5f);
        T.Check("[Export(PropertyHint.Range, ...)] is kept", pt.GetField("JumpVelocity")!.GetCustomAttribute<ExportAttribute>() is { Hint: PropertyHint.Range, HintString: "0,20,0.5" });
        T.Check("[Export(PropertyHint.Layers3DPhysics)] is kept", pt.GetField("InteractMask")!.GetCustomAttribute<ExportAttribute>() is { Hint: PropertyHint.Layers3DPhysics });
        SceneTree.Instance.Root.AddChild(player);
        player.Velocity = Vector3.Zero;
        pt.GetMethod("_PhysicsProcess")!.Invoke(player, new object[] { 0.1 });
        T.Check("_PhysicsProcess: gravity applies when not on the floor, MoveAndSlide runs", T.Near(player.Velocity.Y, -0.98f, 1e-3f), "velocity " + player.Velocity);

        Type kt = asm.GetType("Pickup")!;
        T.Check("[Tool] and [GlobalClass] are kept", kt.GetCustomAttribute<ToolAttribute>() != null && kt.GetCustomAttribute<GlobalClassAttribute>() != null);
        T.Check("[Icon] is kept", kt.GetCustomAttribute<IconAttribute>()?.Path == "res://icons/pickup.svg");
        T.Check("[Rpc(mode, CallLocal = true)] became [Rpc(\"AnyPeer\")]", kt.GetMethod("Collect")!.GetCustomAttribute<RpcAttribute>()?.Mode == "AnyPeer");
        T.Check("MethodName.Collect", kt.GetNestedType("MethodName")!.GetField("Collect")!.GetValue(null)!.ToString() == "Collect");
        var pickup = (Area3D)Activator.CreateInstance(kt)!;
        SceneTree.Instance.Root.AddChild(pickup);
        kt.GetMethod("Collect")!.Invoke(pickup, new object[] { player });
        T.Check("Collect(by) ran and queued the free", (int)Field(pickup, "Collected")! == 1 && pickup.IsQueuedForDeletion());
    }

    // ---- the command line ----

    private static int Cli(out string stdout, out string stderr, params string[] args)
    {
        var o = new StringWriter();
        var e = new StringWriter();
        int code = ConvertCli.Run(args, o, e);
        stdout = o.ToString();
        stderr = e.ToString();
        return code;
    }

    private static List<JsonElement> Lines(string path) =>
        File.ReadAllLines(path).Where(l => l.Length > 0).Select(l => JsonDocument.Parse(l).RootElement.Clone()).ToList();

    private static void CliManifestAndReport()
    {
        string data = Path.Combine(T.DataDir(), "csharp");
        string root = Temp("cli");
        try
        {
            string proj = Path.Combine(root, "CSharpGame");
            Directory.CreateDirectory(proj);
            // What the scene converter left: its report with a placeholder for Door.cs, plus an entry the scripts run must keep.
            string report = Path.Combine(root, "FabricReport.jsonl");
            File.WriteAllText(report,
                "{\"schema\":1,\"kind\":\"node\",\"file\":\"res://main.tscn\",\"node\":\"Player\",\"type\":\"Node3D\",\"property\":\"\",\"rating\":\"converted\",\"reason\":\"\",\"line\":3}\n" +
                "{\"schema\":1,\"kind\":\"script\",\"file\":\"res://scripts/Door.cs\",\"node\":\"\",\"type\":\"csharp\",\"property\":\"\",\"rating\":\"unsupported\",\"reason\":\"the script body is converted by a later package\",\"line\":0}\n" +
                "{\"schema\":1,\"kind\":\"script\",\"file\":\"res://scripts/boss.gd\",\"node\":\"\",\"type\":\"gdscript\",\"property\":\"\",\"rating\":\"unsupported\",\"reason\":\"later package\",\"line\":0}\n");
            string md = Path.Combine(root, "FabricReport.md");
            File.WriteAllText(md, "# Fabric conversion report: CSharp Game\n\n| Kind | n |\n");

            int code = Cli(out string so, out string se, "scripts", "--project", data, "--out", proj, "--manifest", Path.Combine(data, "scripts.json"), "--refs", Refs);
            T.Check("exit 0", code == 0, se);
            T.Check("the summary line is JSON with the counts", JsonDocument.Parse(so.Trim().Split('\n').Last()).RootElement is { } s && s.GetProperty("scripts").GetInt32() == 7 && s.GetProperty("errors").GetInt32() == 0 && s.GetProperty("compiled").GetBoolean());
            string scripts = Path.Combine(proj, "Content", "Scripts");
            T.Check("each script is written where scripts.json says", new[] { "Countdown", "Door", "VectorExtensions", "Locator", "Pickup", "PlayerController", "SlideScript" }.All(n => File.Exists(Path.Combine(scripts, n + ".cs"))));
            T.Check("a GDScript entry is left for the GDScript package", !File.Exists(Path.Combine(scripts, "Boss.cs")));
            string slide = File.ReadAllText(Path.Combine(scripts, "SlideScript.cs"));
            T.Check("a script whose class scripts.json renamed is registered under that name", slide.Contains("AverClass(\"SlideScript\")") && slide.Contains("partial class Slider"));

            List<JsonElement> lines = Lines(report);
            string K(JsonElement e, string p) => e.GetProperty(p).GetString() ?? "";
            T.Check("the scene converter's own entries are kept", lines.Any(l => K(l, "kind") == "node" && K(l, "file") == "res://main.tscn"));
            T.Check("its GDScript placeholder is kept", lines.Any(l => K(l, "file") == "res://scripts/boss.gd"));
            T.Check("the placeholder for Door.cs is replaced by the real entry", lines.Count(l => K(l, "file") == "res://scripts/Door.cs" && K(l, "type") == "csharp") == 1 &&
                    !lines.Any(l => K(l, "reason").Contains("later package") && K(l, "file").EndsWith("Door.cs")));
            T.Check("a script that does not exist is reported", lines.Any(l => K(l, "file") == "res://scripts/Missing.cs" && K(l, "rating") == "unsupported"));
            T.Check("the .csproj's NuGet package is reported by name", lines.Any(l => K(l, "type") == "PackageReference" && K(l, "property") == "Newtonsoft.Json" && K(l, "rating") == "unsupported"));
            T.Check("Pickup's [Tool] is an unsupported entry with a line", lines.Any(l => K(l, "file").EndsWith("Pickup.cs") && K(l, "property") == "Tool" && K(l, "rating") == "unsupported" && l.GetProperty("line").GetInt32() > 0));
            T.Check("every line has the schema's eight fields", lines.All(l => new[] { "schema", "kind", "file", "node", "type", "property", "rating", "reason", "line" }.All(f => l.TryGetProperty(f, out _))));

            string mdText = File.ReadAllText(md);
            T.Check("FabricReport.md gets a scripts section", mdText.Contains("## Scripts (GodotSharp, Aver.Fabric.Convert)") && mdText.Contains("res://scripts/Door.cs"));

            // A rerun changes nothing.
            string before = File.ReadAllText(report), mdBefore = mdText, doorBefore = File.ReadAllText(Path.Combine(scripts, "Door.cs"));
            int again = Cli(out _, out _, "scripts", "--project", data, "--out", proj, "--manifest", Path.Combine(data, "scripts.json"), "--refs", Refs);
            T.Check("rerun: exit 0 and the jsonl is byte for byte the same", again == 0 && File.ReadAllText(report) == before);
            T.Check("rerun: the markdown has one scripts section", File.ReadAllText(md) == mdBefore && mdBefore.Split("fabric-scripts-csharp:begin").Length == 2);
            T.Check("rerun: the output is the same", File.ReadAllText(Path.Combine(scripts, "Door.cs")) == doorBefore);

            // Converting the output again is stable: nothing in it is Godot any more.
            ConversionResult twice = ScriptConverter.Convert(new[] { new ScriptSource { ResPath = "res://scripts/Door.cs", Text = doorBefore, ClassName = "Door", Output = "Content/Scripts/Door.cs" } },
                                                              new ConvertSettings { RefsDir = Refs });
            T.Check("an already converted script converts and compiles again", twice.Success, string.Join("; ", twice.Errors));
        }
        finally { try { Directory.Delete(root, true); } catch (IOException) { } }
    }

    private static void CliExitCodes()
    {
        string data = Path.Combine(T.DataDir(), "csharp_unsupported");
        string root = Temp("cli2");
        try
        {
            string proj = Path.Combine(root, "P");
            int code = Cli(out string so, out string se, "scripts", "--project", data, "--out", proj, "--refs", Refs);
            T.Check("no manifest: the project is scanned; compile errors give exit 1", code == 1, se);
            T.Check("each error is printed as file(line,col): error CSxxxx", se.Contains("Content/Scripts/Narrator.cs(8,21): error CS0246") && se.Contains("error CS0103: The name 'ApplyFloorSnap'"));
            string text = File.ReadAllText(Path.Combine(proj, "Content", "Scripts", "Narrator.cs"));
            T.Check("the output is written anyway, with the FABRIC marks", text.Contains("// FABRIC: Aver.Fabric has no 'ApplyFloorSnap'"));
            T.Check("the summary says 4 errors", JsonDocument.Parse(so.Trim().Split('\n').Last()).RootElement.GetProperty("errors").GetInt32() == 4);
            T.Check("with no report to merge into, a new FabricReport.jsonl is made in the project", File.Exists(Path.Combine(proj, "FabricReport.jsonl")));

            T.Check("no arguments: usage, exit 2", Cli(out _, out string e2) == 2 && e2.Contains("usage:"));
            T.Check("a missing --out: exit 2", Cli(out _, out _, "scripts", "--project", data) == 2);
            T.Check("an unknown command: exit 2", Cli(out _, out _, "convert") == 2);
            T.Check("an unknown option: exit 2", Cli(out _, out _, "scripts", "--project", data, "--out", proj, "--frobnicate") == 2);
            T.Check("a project folder that does not exist: exit 1", Cli(out _, out _, "scripts", "--project", Path.Combine(root, "nope"), "--out", proj) == 1);

            string proj3 = Path.Combine(root, "Q");
            int bad = Cli(out string so3, out string se3, "scripts", "--project", Path.Combine(T.DataDir(), "csharp"), "--out", proj3, "--refs", Path.Combine(root, "no-such-folder"));
            T.Check("a reference folder that is not there: exit 3", bad == 3, se3);
            T.Check("the scripts are still written, and rated approximated since they were not compiled",
                    File.Exists(Path.Combine(proj3, "Content", "Scripts", "Door.cs")) &&
                    Lines(Path.Combine(proj3, "FabricReport.jsonl")).Any(l => l.GetProperty("file").GetString() == "res://scripts/Door.cs" && l.GetProperty("type").GetString() == "csharp" && l.GetProperty("rating").GetString() == "approximated"));
            T.Check("the summary says it was not compiled", JsonDocument.Parse(so3.Trim().Split('\n').Last()).RootElement.GetProperty("compiled").GetBoolean() == false);

            string empty = Temp("noaver");
            int missingDll = Cli(out _, out string se4, "scripts", "--project", data, "--out", Path.Combine(root, "R"), "--refs", empty);
            T.Check("a folder without Aver.Fabric.dll is named in the message", missingDll == 3 && se4.Contains("Aver.Fabric.dll was not found"), se4);
            Directory.Delete(empty, true);
        }
        finally { try { Directory.Delete(root, true); } catch (IOException) { } }
    }

    private static void CliScriptsProject()
    {
        string root = Temp("cli3");
        try
        {
            string proj = Path.Combine(root, "P");
            string repo = RepoRoot();
            int code = Cli(out _, out string se, "scripts", "--project", Path.Combine(T.DataDir(), "csharp"), "--out", proj, "--refs", Refs, "--engine", repo);
            T.Check("exit 0 with --engine", code == 0, se);
            string csproj = Path.Combine(proj, "Content", "Scripts", "Scripts.csproj");
            T.Check("Scripts.csproj is written", File.Exists(csproj));
            string text = File.ReadAllText(csproj);
            T.Check("it references Aver.Fabric and Aver.Framework by a relative path into the engine", text.Contains("Aver.Fabric\\Aver.Fabric.csproj") && text.Contains("Aver.Framework\\Aver.Framework.csproj") && text.Contains("..\\"));
            T.Check("with ImplicitUsings and Nullable like every project the editor makes", text.Contains("<ImplicitUsings>enable</ImplicitUsings>") && text.Contains("<Nullable>enable</Nullable>"));
            T.Check("and no XML-illegal -- in a comment", !System.Text.RegularExpressions.Regex.IsMatch(text, "<!--(?:(?!-->).)*--(?!>)", System.Text.RegularExpressions.RegexOptions.Singleline));
            File.WriteAllText(csproj, "<Project />");
            Cli(out _, out _, "scripts", "--project", Path.Combine(T.DataDir(), "csharp"), "--out", proj, "--refs", Refs, "--engine", repo);
            T.Check("an existing Scripts.csproj is never overwritten", File.ReadAllText(csproj) == "<Project />");
        }
        finally { try { Directory.Delete(root, true); } catch (IOException) { } }
    }
}
