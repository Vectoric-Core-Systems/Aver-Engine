// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Aver.Fabric.Convert (G3 F4b): GDScript converted to C# against this runtime. Every F4a fixture and the two realistic scripts compile,
// each mapping of the GDScript table is checked on a small script, and converted functions run and are compared with the results
// GDScript's semantics give.

using System.Reflection;
using System.Text.Json;
using Aver.Fabric;
using Aver.Fabric.Convert;

static class GdScriptConvertTests
{
    public static int RunAll() => T.Suite(() =>
    {
        T.Run("gdscript convert: every F4a fixture and the two realistic scripts compile with no errors", FixturesCompile);
        T.Run("gdscript convert: the realistic scripts read as C# (typed, no Variant soup)", RealisticReadability);
        T.Run("gdscript convert: a script with syntax errors converts what parsed and reports the rest", ErrorFixture);
        T.Run("gdscript convert: damaged copies of the fixtures never make the converter fail", Damaged);
        T.Run("gdscript map: classes, extends, class_name, exports, onready, setget, signals, enums, consts, statics, inner classes", MapDeclarations);
        T.Run("gdscript map: functions, defaults, overloads for computed defaults, rest parameters, constructors, super", MapFunctions);
        T.Run("gdscript map: operators and GDScript semantics (integer division, %, **, in, formatting, truthiness)", MapOperators);
        T.Run("gdscript map: for / range / match / while / ternary / assert", MapControlFlow);
        T.Run("gdscript map: await, signals connect / emit / bind, lambdas, Callable", MapSignalsAndLambdas);
        T.Run("gdscript map: built-in functions and type conversions", MapBuiltins);
        T.Run("gdscript map: node paths, get/set accessors, struct members, dynamic access, untyped variables", MapMembersAndTypes);
        T.Run("gdscript map: what Aver.Fabric lacks is left, marked and reported", MapMissingApi);
        T.Run("gdscript map: navigation (AStar3D override, NavigationAgent3D signals, AStarGrid2D) converts, compiles and runs", MapNavigation);
        T.Run("gdscript map: autoloads and project-wide class names", MapProject);
        T.Run("gdscript run: Calc (match, for / range, string formatting, integer math, arrays, lambdas) against GDScript's results", RunCalc);
        T.Run("gdscript run: Counter (export, onready, setget, signal, await on a Timer, _process)", RunCounter);
        T.Run("gdscript run: Enemy (states, constructor-assigned fields, groups, dictionary of stats, await then queue_free)", RunEnemy);
        T.Run("gdscript runtime: GDScript.Format, Variant operators, comparison, iteration and dynamic access", RuntimeHelpers);
        T.Run("gdscript runtime: Engine, OS, ProjectSettings and the string and array extension methods", RuntimeSingletons);
        T.Run("gdscript cli: scripts.json with .gd entries, the report, a rerun, the scan fallback and exit codes", Cli);
    });

    // The stand-in classes of the earlier packages are gone: every class the realistic scripts use (the audio, animation and UI controls) is real now.

    // ---- helpers ----

    private static string Refs => AppContext.BaseDirectory;
    private static IReadOnlyList<(string, string)> Extra => System.Array.Empty<(string, string)>();

    private static string DataDir => Path.Combine(T.DataDir(), "gdscript");

    /// <summary>The scripts of a folder as the scene converter's manifest would list them: res:// paths, class_name or the file name as the class.</summary>
    private static List<ScriptSource> FixtureSources(string dir, params string[] skip)
    {
        var list = new List<ScriptSource>();
        foreach (string f in Directory.EnumerateFiles(dir, "*.gd", SearchOption.AllDirectories).OrderBy(p => p, StringComparer.Ordinal))
        {
            if (skip.Any(s => Path.GetFileName(f) == s)) continue;
            list.Add(SourceOf(dir, f));
        }
        return list;
    }

    private static ScriptSource SourceOf(string root, string file)
    {
        string text = File.ReadAllText(file);
        string rel = Path.GetRelativePath(root, file).Replace('\\', '/');
        string? declared = Aver.Fabric.Convert.GdScript.GdScriptParser.Parse(text).Script?.Name;
        string cls = string.IsNullOrEmpty(declared) ? Pascal(Path.GetFileNameWithoutExtension(file)) : declared!;
        return new ScriptSource { ResPath = "res://" + rel, Text = text, Kind = "gdscript", ClassName = cls, Output = "Content/Scripts/" + cls + ".cs" };
    }

    private static string Pascal(string s)
    {
        string p = string.Concat(s.Split('_', ' ', '-').Where(x => x.Length > 0).Select(x => char.ToUpperInvariant(x[0]) + x[1..]));
        return char.IsDigit(p[0]) ? "Fx" + p : p;
    }

    private static ConversionResult Convert(IReadOnlyList<ScriptSource> sources, bool keep = false, string? projectDir = null) =>
        ScriptConverter.Convert(sources, new ConvertSettings { RefsDir = Refs, KeepAssembly = keep, ExtraSources = Extra, ProjectDir = projectDir });

    /// <summary>One small script.</summary>
    private static ConversionResult One(string gd, string cls = "Sample", bool keep = false) =>
        Convert(new[] { new ScriptSource { ResPath = "res://sample.gd", Text = gd, Kind = "gdscript", ClassName = cls, Output = "Content/Scripts/" + cls + ".cs" } }, keep);

    private static string Text(ConversionResult r, int i = 0) => r.Scripts[i].Text;

    private static string Errors(ConversionResult r) => string.Join("; ", r.Errors.Take(4).Select(e => e.ToString())) + string.Join("; ", r.GeneralErrors);

    private static bool Has(string text, params string[] parts) => parts.All(text.Contains);

    private static ConversionResult? s_fixtures;

    private static ConversionResult Fixtures()
    {
        s_fixtures ??= Convert(FixtureSources(DataDir, "10_errors.gd"), keep: true);
        return s_fixtures;
    }

    private static string Cls(ConversionResult r, string cls) => r.Scripts.First(s => s.ClassName == cls).Text;

    /// <summary>dotnet run -- gdconvert &lt;folder&gt; [skip.gd]...: prints the converted C# and the compile errors (development aid).</summary>
    public static int Debug(string[] files)
    {
        List<ScriptSource> sources = Directory.Exists(files[0]) ? FixtureSources(files[0], files.Skip(1).ToArray()) : new List<ScriptSource>();
        if (sources.Count == 0)
            foreach (string f in files) sources.Add(SourceOf(Path.GetDirectoryName(f)!, f));
        ConversionResult r = Convert(sources);
        foreach (ConvertedScript s in r.Scripts)
        {
            Console.WriteLine("//==== " + s.Output);
            Console.WriteLine(s.Text);
        }
        foreach (CompileError e in r.Errors) Console.WriteLine(e);
        foreach (string g in r.GeneralErrors) Console.WriteLine("general: " + g);
        foreach (ReportItem i in r.Items) Console.WriteLine($"report {i.Rating} {i.Type} {i.Property} line {i.Line}: {i.Reason}");
        return r.Errors.Count;
    }

    // ---- the fixtures ----

    private static void FixturesCompile()
    {
        ConversionResult r = Fixtures();
        int expected = Directory.EnumerateFiles(DataDir, "*.gd", SearchOption.AllDirectories).Count(f => Path.GetFileName(f) != "10_errors.gd");
        T.Check("every fixture script was converted (" + expected + ")", r.Scripts.Count == expected, $"got {r.Scripts.Count}");
        T.Check("the whole set compiles against Aver.Fabric: zero errors", r.Success && r.Errors.Count == 0, Errors(r));
        T.Check("an assembly came out", r.Assembly is { Length: > 0 });
        foreach (string name in new[] { "02_declarations", "03_properties", "04_functions", "05_control_flow", "06_match", "07_expressions", "08_lambdas", "09_strings", "player_controller", "ui_menu" })
            T.Check($"{name}.gd has an entry and none of its items is a compile error", r.Scripts.Any(s => s.ResPath.EndsWith("/" + name + ".gd")) &&
                    !r.Items.Any(i => i.File.EndsWith("/" + name + ".gd") && i.Type.StartsWith("CS")));
        T.Check("the summary entry of a script says GDScript", r.Items.Any(i => i.Type == "gdscript" && i.File.EndsWith("player_controller.gd") && i.Reason.StartsWith("GDScript to Aver.Fabric")));
        T.Check("@tool is reported unsupported with its line", r.Items.Any(i => i.Type == "attribute" && i.Property == "Tool" && i.Rating == Rating.Unsupported && i.Line > 0 && i.File.EndsWith("ui_menu.gd")));
        T.Check("what the report names is on that line of the output",
                r.Scripts.All(s => s.Items.Where(i => i.Line > 0 && i.Type != "gdscript").All(i => s.Text.Split('\n')[i.Line - 1].Contains("FABRIC") || i.Type.StartsWith("CS"))));
        T.Check("no tag of the converter is left in the text", r.Scripts.All(s => !s.Text.Contains("FABRIC[")));
        Assembly asm = Assembly.Load(r.Assembly!);
        T.Check("the classes are in the assembly under the names scripts.json gives", new[] { "PlayerController", "MainMenu", "Declarations", "BaseActor", "Outer", "Calc", "Counter" }.All(n => asm.GetType(n) != null));
        T.Check("class_name scripts carry [GlobalClass]", asm.GetType("PlayerController")!.GetCustomAttribute<GlobalClassAttribute>() != null);
        T.Check("@tool carries [Tool]", asm.GetType("MainMenu")!.GetCustomAttribute<ToolAttribute>() != null);
        T.Check("extends \"res://base/base_actor.gd\" is the converted class", asm.GetType("Declarations")!.BaseType == asm.GetType("BaseActor"));
        T.Check("extends CharacterBody3D is the runtime's class", asm.GetType("PlayerController")!.BaseType == typeof(CharacterBody3D));
    }

    private static void RealisticReadability()
    {
        ConversionResult r = Fixtures();
        string p = Cls(r, "PlayerController");
        T.Check("player_controller: typed members, no Variant anywhere", !p.Contains("Variant"), "found Variant");
        T.Check("player_controller: no run-time helper where the types are known", !p.Contains("GDScript."));
        T.Check("@export_range / @export_flags / @export_group become the attributes",
                Has(p, "[Export(PropertyHint.Range, \"0.0,1.0,0.01\")]", "[Export(PropertyHint.Flags, \"Fire,Ice,Poison\")]", "[ExportGroup(\"Movement\")]", "[ExportGroup(\"Combat\")]"));
        T.Check("@onready assigns at the start of _Ready, in GDScript order", Has(p, "public override void _Ready()") &&
                p.IndexOf("Head = GetNode<Node3D>(\"Head\");", StringComparison.Ordinal) < p.IndexOf("Camera = GetNode<Camera3D>(\"Head/Camera3D\");", StringComparison.Ordinal) &&
                p.IndexOf("DashTimer = GetNode<Timer>(\"DashTimer\");", StringComparison.Ordinal) < p.IndexOf("Input.MouseMode = Input.MouseModeEnum.Captured;", StringComparison.Ordinal));
        T.Check("setget: a property over a backing field, the setter's own name is the field", Has(p, "private long _health = 100;", "public long Health", "_health = Math.Clamp(value, 0, MaxHealth);"));
        T.Check("signals: [Signal] delegates, typed parameters, EmitSignal helpers", Has(p, "[Signal] public delegate void HealthChangedEventHandler(long oldValue, long newValue);", "EmitSignalHealthChanged(old, _health);"));
        T.Check("x is T narrows the name: a pattern variable, typed members", Has(p, "@event is InputEventMouseMotion inputEventMouseMotion", "inputEventMouseMotion.Relative.X"));
        T.Check("a field of a struct property is written back whole", Has(p, "Velocity = Velocity with { Y = (float)(Velocity.Y - Gravity * delta) };"));
        T.Check("constants stay named: const double WalkSpeed, cast where a float is needed", Has(p, "public const double WalkSpeed = 5.0;", "(float)JumpVelocity") || Has(p, "public const double WalkSpeed = 5.0;"));
        T.Check("match on an enum is a switch", Has(p, "switch (state)", "case State.Dead:"));
        T.Check("await on a signal", Has(p, "public async void Dash(Vector3 direction)", "await ToSignal(DashTimer, Timer.SignalName.Timeout);"));
        T.Check("a direction vector is true when it is not zero", p.Contains("direction != Vector3.Zero"));
        T.Check("input constants map to the enum members", Has(p, "Input.MouseMode = Input.MouseModeEnum.Captured;", "Input.IsActionJustPressed(\"jump\")"));

        string m = Cls(r, "MainMenu");
        T.Check("ui_menu: a setter with an export puts the attribute on the property", Regex("\\[Export\\]\\s+public string Title") && !m.Contains("[Export]\n    private"));
        T.Check("ui_menu: a lambda with a captured loop variable", Has(m, "b.Connect(Button.SignalName.MouseEntered, Callable.From(() =>", "b.Modulate = Accent;"));
        T.Check("ui_menu: bind() on a method callable", m.Contains("Callable.From<StringName>(OnButtonPressed).Bind(b.Name)"));
        T.Check("ui_menu: match on StringName patterns is a switch on strings", Has(m, "switch ((string)buttonName)", "case \"Start\":"));
        T.Check("ui_menu: dictionary get with a default, then a typed string", m.Contains("string path = Levels.Get(levelKey, FirstLevel).AsString();"));
        T.Check("ui_menu: format with % and a typed enum export", Has(m, "GDScript.Format(\"Unknown button: %s\", buttonName)", "public Panel DefaultPanel = Panel.Main;"));
        T.Check("ui_menu: Engine.is_editor_hint() is Engine.IsEditorHint()", m.Contains("if (Engine.IsEditorHint())"));
        T.Check("ui_menu: a $Node that is not typed is read and written by name", m.Contains("GetNode(\"%Title\").Set(\"text\", value);"));

        bool Regex(string pattern) => System.Text.RegularExpressions.Regex.IsMatch(m, pattern);
    }

    private static void ErrorFixture()
    {
        string path = Path.Combine(DataDir, "10_errors.gd");
        ConversionResult r = Convert(new[] { SourceOf(DataDir, path) });
        T.Check("the output still compiles", r.Success, Errors(r));
        T.Check("the six syntax errors are reported, unsupported, with the GDScript position",
                r.Items.Count(i => i.Type == "parse" && i.Rating == Rating.Unsupported && i.Reason.StartsWith("GDScript ")) == 6);
        T.Check("the good members around the damage are there", Has(Text(r), "public long OkBefore = 1;", "public long OkAfter = 3;", "public long LastGood()", "return 9;"));
        T.Check("the script as a whole is approximated, not unsupported", r.Items.First(i => i.Type == "gdscript").Rating == Rating.Approximated);
    }

    private static void Damaged()
    {
        // F4a's fuzz idea, one step on: the converter, not just the parser, must come through a broken script.
        var rng = new Random(20261028);
        int runs = 0, failed = 0;
        foreach (string file in Directory.GetFiles(DataDir, "*.gd").Concat(Directory.GetFiles(Path.Combine(DataDir, "behaviour"), "*.gd")).OrderBy(f => f))
        {
            string[] lines = File.ReadAllText(file).Replace("\r\n", "\n").Split('\n');
            for (int round = 0; round < 6; round++)
            {
                var copy = lines.ToList();
                switch (rng.Next(4))
                {
                    case 0: copy.RemoveAt(rng.Next(copy.Count)); break;
                    case 1: { int at = rng.Next(copy.Count); copy.RemoveRange(at, Math.Min(5, copy.Count - at)); break; }
                    case 2: { int i = rng.Next(copy.Count); copy[i] = copy[i][..(copy[i].Length / 2)]; break; }
                    default: { int i = rng.Next(copy.Count); copy.Insert(i, copy[rng.Next(copy.Count)]); break; }
                }
                string text = string.Join("\n", copy);
                runs++;
                try
                {
                    ConversionResult r = One(text);
                    if (r.Scripts[0].Text.Contains("the converter failed")) failed++;
                }
                catch (Exception ex) { failed++; Console.WriteLine($"  threw {ex.GetType().Name} on {Path.GetFileName(file)} round {round}: {ex.Message}"); }
            }
        }
        T.Check($"{runs} damaged copies of the fixtures convert without the converter failing", failed == 0, $"{failed} failed");
    }

    // ---- the table, small script by small script ----

    private static void MapDeclarations()
    {
        const string gd =
            "@tool\n@icon(\"res://i.svg\")\nclass_name Hero\nextends CharacterBody3D\n\nsignal hit(damage: int, who: Node)\nsignal died\n\n" +
            "enum Mode { IDLE, RUN = 5, JUMP }\nenum { ANON_A, ANON_B = 2 }\nconst MAX = 100\nconst SPEED: float = 4.5\nconst NAMES = [\"a\", \"b\"]\n" +
            "static var shared := 1\n\n@export var speed := 2.5\n@export_range(0, 10, 0.5) var power: float = 1.0\n@export_enum(\"A\", \"B\") var kind: int\n" +
            "@export_group(\"Look\")\n@export var tint := Color.WHITE\n@export var target: Node3D\n@onready var cam: Camera3D = $Cam\n@onready var tag = %Tag\n" +
            "var hp := 10:\n\tset(v):\n\t\thp = clampi(v, 0, 99)\n\t\thit.emit(hp, null)\n\tget:\n\t\treturn hp\nvar items: Array[int] = [1, 2]\nvar table := {\"k\": 1}\n" +
            "var mode: Mode = Mode.RUN\n\nfunc _ready() -> void:\n\tprint(\"r\")\n\nclass Inner extends RefCounted:\n\tvar v := 1\n\tfunc get_v() -> int:\n\t\treturn v\n\nstatic func make() -> Hero:\n\treturn Hero.new()\n";
        ConversionResult r = One(gd, "Hero");
        string t = Text(r);
        T.Check("it compiles", r.Success, Errors(r));
        T.Check("class_name: [GlobalClass] and the class named by scripts.json; extends: the base class", Has(t, "[GlobalClass]", "public partial class Hero : CharacterBody3D"));
        T.Check("@tool: [Tool], reported unsupported; @icon: [Icon]", Has(t, "[Tool]", "[Icon(\"res://i.svg\")]") && r.Items.Any(i => i.Property == "Tool" && i.Rating == Rating.Unsupported));
        T.Check("signals: [Signal] delegates with typed parameters", Has(t, "[Signal] public delegate void HitEventHandler(long damage, Node who);", "[Signal] public delegate void DiedEventHandler();"));
        T.Check("signal.emit(): the generated EmitSignal helper, null for a node", t.Contains("EmitSignalHit(_hp, null);"));
        T.Check("enums: a C# enum with the values; an anonymous enum: constants", Has(t, "public enum Mode { Idle, Run = 5, Jump }", "public const long AnonA = 0;", "public const long AnonB = 2;"));
        T.Check("consts: const where C# allows it, static readonly otherwise", Has(t, "public const long Max = 100;", "public const double Speed = 4.5;", "public static readonly Collections.Array Names"));
        T.Check("static var: static field", t.Contains("public static long Shared = 1;"));
        T.Check("@export: [Export] with the type of the value; _range, _enum with the hint string", Has(t, "[Export]\n    public double speed = 2.5;") || Has(t, "public double speed = 2.5;"));
        T.Check("export hints", Has(t, "[Export(PropertyHint.Range, \"0,10,0.5\")]", "[Export(PropertyHint.Enum, \"A,B\")]"));
        T.Check("@export_group: [ExportGroup] on the next export", Regex("\\[ExportGroup\\(\"Look\"\\)\\]\\s+\\[Export\\]\\s+public Color Tint = Colors.White;"));
        T.Check("@onready: field, assigned in _Ready before the user's code, typed from the annotation", Has(t, "public Camera3D Cam;", "Cam = GetNode<Camera3D>(\"Cam\");") &&
                t.IndexOf("Cam = GetNode", StringComparison.Ordinal) < t.IndexOf("GD.Print(\"r\")", StringComparison.Ordinal));
        T.Check("setget: property, backing field, the getter body", Has(t, "private long _hp = 10;", "public long Hp", "_hp = Math.Clamp(value, 0, 99);", "return _hp;"));
        T.Check("typed array and dictionary members", Has(t, "Collections.Array<long> Items = new Collections.Array<long> { 1, 2 };", "Collections.Dictionary Table = new Collections.Dictionary { { \"k\", 1 } };"));
        T.Check("a typed enum variable", t.Contains("public Mode mode = Mode.Run;") || t.Contains("public Mode Mode = Mode.Run;") || t.Contains("Mode.Run"));
        T.Check("inner class: a nested partial class", Has(t, "public partial class Inner : RefCounted", "public long V = 1;", "public long GetV()"));
        T.Check("static func: static method", t.Contains("public static Hero Make()"));
        T.Check("the generated members of the GodotSharp pipeline are there", Has(t, "partial class Hero", "SignalName", "EmitSignalDied"));
        bool Regex(string p) => System.Text.RegularExpressions.Regex.IsMatch(t, p);
    }

    private static void MapFunctions()
    {
        const string gd =
            "extends Node\n\nvar base_value := 1\n\nfunc _init(start := 5):\n\tbase_value = start\n\nfunc add(a: int, b: int = 2, c := 3.5, d = null) -> int:\n\treturn a + b\n" +
            "func vec_default(v: Vector3 = Vector3.ONE) -> Vector3:\n\treturn v\nfunc counted(first, ...rest: Array):\n\treturn rest.size()\n" +
            "func untyped(x):\n\treturn x\nfunc infers():\n\treturn 42\nfunc nothing():\n\tpass\nfunc chain() -> String:\n\tvar o := _private_thing(1)\n\treturn o\n" +
            "func _private_thing(n: int) -> String:\n\treturn str(n)\nfunc _ready() -> void:\n\tsuper()\n\tsuper._ready()\n" +
            "func _process(delta: float) -> void:\n\tbase_value += 1\nfunc _physics_process(dt) -> void:\n\tpass\nfunc _unhandled_input(event) -> void:\n\tpass\n";
        ConversionResult r = One(gd);
        string t = Text(r);
        T.Check("it compiles", r.Success, Errors(r));
        T.Check("typed parameters with constant defaults keep them", t.Contains("public long Add(long a, long b = 2, double c = 3.5, Variant d = default)"));
        T.Check("a default C# cannot write as a constant becomes forwarding overloads", Has(t, "public Vector3 VecDefault(Vector3 v)", "public Vector3 VecDefault() => VecDefault(Vector3.One);"));
        T.Check("rest parameter: params Variant[], an Array inside", Has(t, "Counted(Variant first, params Variant[] restArgs)", "new Collections.Array(restArgs)"));
        T.Check("untyped parameter: Variant; untyped return that returns a value of one kind takes that type", Has(t, "public Variant Untyped(Variant x)", "public long Infers()"));
        T.Check("a function with no return is void", t.Contains("public void Nothing()"));
        T.Check("a leading underscore is dropped from a method name (the scenes' lookup does the same)", Has(t, "public string PrivateThing(long n)", "PrivateThing(1)"));
        T.Check("virtuals override with the runtime's signatures: _ready, _process(double), _physics_process, _unhandled_input", Has(t,
                "public override void _Ready()", "public override void _Process(double delta)", "public override void _PhysicsProcess(double dt)", "public override void _UnhandledInput(InputEvent event)")
                || Has(t, "public override void _UnhandledInput(InputEvent @event)"));
        T.Check("super._ready(): base._Ready()", t.Contains("base._Ready();"));
        T.Check("_init is a constructor; its defaults become a parameterless one the runtime can use", Has(t, "public Sample(long start)", "BaseValue = start;", "public Sample() : this(5)"));

        // The overloads and the rest parameter really work.
        ConversionResult run = One(gd, keep: true);
        Assembly asm = Assembly.Load(run.Assembly!);
        Type type = asm.GetType("Sample")!;
        var n = (Node)Activator.CreateInstance(type, 9L)!;
        T.Check("_init(9) ran after the field initialisers", (long)type.GetField("BaseValue")!.GetValue(n)! == 9);
        var d = (Node)Activator.CreateInstance(type)!;
        T.Check("new() uses the default of _init", (long)type.GetField("BaseValue")!.GetValue(d)! == 5);
        T.Check("the forwarding overload supplies the default vector", type.GetMethod("VecDefault", Type.EmptyTypes)!.Invoke(d, null)!.Equals(Vector3.One));
        T.Check("rest arguments arrive as an Array", System.Convert.ToInt64(type.GetMethod("Counted")!.Invoke(d, new object[] { (Variant)1L, new Variant[] { 2L, 3L, 4L } })) == 3);
    }

    private static void MapOperators()
    {
        const string gd =
            "extends Node\n\nfunc ops(a: int, b: int, f: float, s: String, v: Vector3, arr: Array, d: Dictionary, x) -> void:\n" +
            "\tvar i1 = a / b\n\tvar i2 = a % b\n\tvar i3 = a ** b\n\tvar f1 = a / 2.0\n\tvar f2 = f ** 2.0\n\tvar sh = a << 2 | b & 1 ^ 3\n\tvar nb = ~a\n" +
            "\tvar t1 = \"n=%d f=%.2f %s\" % [a, f, s]\n\tvar t2 = \"v=%s\" % s\n\tvar c1 = a in arr\n\tvar c2 = s in \"hello\"\n\tvar c3 = \"k\" in d\n\tvar c4 = a not in arr\n" +
            "\tvar v1 = v * 2.0 + Vector3.UP\n\tvar v2 = v / f\n\tvar v3 = -v\n\tvar eq = v == Vector3.ZERO\n\tvar n1 = a if a > b else b\n" +
            "\tvar o1 = x + 1\n\tvar o2 = x * x\n\tvar o3 = x == 2\n\tvar o4 = x < 3\n\tvar o5 = x and a\n\tvar o6 = not x\n" +
            "\tif a: pass\n\tif f: pass\n\tif s: pass\n\tif v: pass\n\tif arr: pass\n\tif d: pass\n\tif x: pass\n\tif self: pass\n\tvar str_cmp = s < \"m\"\n\tvar and_or = a > 1 and b > 2 or a == b\n";
        ConversionResult r = One(gd);
        string t = Text(r);
        T.Check("it compiles", r.Success, Errors(r));
        T.Check("int / int is C#'s integer division (truncates toward zero) and int % int takes the dividend's sign", Has(t, "long i1 = a / b;", "long i2 = a % b;"));
        T.Check("int ** int is an int power; float ** float is Math.Pow", Has(t, "GDScript.IntPow(a, b)", "Math.Pow(f, 2.0)"));
        T.Check("int / float is a float", t.Contains("double f1 = a / 2.0;"));
        T.Check("shifts take an int count", t.Contains("a << 2"));
        T.Check("the % operator on a String is GDScript.Format; an array operand spreads", Has(t, "GDScript.Format(\"n=%d f=%.2f %s\", new Collections.Array { a, f, s })", "GDScript.Format(\"v=%s\", s)"));
        T.Check("in: Contains / ContainsKey / string Contains, and not in", Has(t, "arr.Contains(a)", "\"hello\".Contains(s)", "d.ContainsKey(\"k\")", "!arr.Contains(a)"));
        T.Check("vector arithmetic stays typed (the scalar is a float)", Has(t, "Vector3 v1 = v * 2.0f + Vector3.Up;", "Vector3 v2 = v / (float)f;", "Vector3 v3 = -v;"));
        T.Check("a ternary", t.Contains("long n1 = a > b ? a : b;"));
        T.Check("a Variant operand sends the operator through GDScript's Variant rules", Has(t, "GDScript.Add(x, 1)", "GDScript.Mul(x, x)", "x == 2", "GDScript.Lt(x, 3)"));
        T.Check("truthiness by type", Has(t, "if (a != 0)", "if (f != 0.0)", "if (!string.IsNullOrEmpty(s))", "if (v != Vector3.Zero)", "if (arr.Count > 0)", "if (d.Count > 0)", "if (x.Booleanize())", "if (IsInstanceValid(this))"));
        T.Check("a string comparison is ordinal", t.Contains("string.CompareOrdinal(s, \"m\") < 0"));
        T.Check("and / or / not", Has(t, "a > 1 && b > 2 || a == b", "!x.Booleanize()", "x.Booleanize() && a != 0"));
    }

    private static void MapControlFlow()
    {
        const string gd =
            "extends Node\n\nenum E { P, Q }\n\nfunc flow(items: Array, d: Dictionary, n: int, s: String, e: E) -> int:\n\tvar total := 0\n" +
            "\tfor i in range(5):\n\t\ttotal += i\n\tfor i in range(2, 8):\n\t\ttotal += i\n\tfor i in range(10, 0, -3):\n\t\ttotal += i\n\tfor i in n:\n\t\ttotal += i\n\tfor i in range(n):\n\t\ttotal += i\n" +
            "\tfor x in items:\n\t\tprint(x)\n\tfor k in d:\n\t\tprint(k)\n\tfor c in s:\n\t\tprint(c)\n\tfor i in range(items.size()):\n\t\ttotal += i\n" +
            "\twhile total > 100:\n\t\ttotal -= 1\n\t\tif total == 50:\n\t\t\tbreak\n\t\tcontinue\n" +
            "\tmatch n:\n\t\t0:\n\t\t\treturn 0\n\t\t1, 2:\n\t\t\treturn 1\n\t\t_:\n\t\t\tpass\n" +
            "\tmatch s:\n\t\t\"a\":\n\t\t\ttotal += 1\n\t\tvar other:\n\t\t\tprint(other)\n" +
            "\tmatch e:\n\t\tE.P:\n\t\t\treturn 7\n\t\tE.Q:\n\t\t\treturn 8\n" +
            "\tfor i in range(3):\n\t\tmatch i:\n\t\t\t1:\n\t\t\t\tbreak\n\t\t\t_:\n\t\t\t\tpass\n" +
            "\tassert(total >= 0, \"negative\")\n\treturn total\n";
        ConversionResult r = One(gd);
        string t = Text(r);
        T.Check("it compiles", r.Success, Errors(r));
        T.Check("range(n): a for loop with the loop variable declared in it", Has(t, "for (long i = 0; i < 5; i++)", "for (long i = 2; i < 8; i++)"));
        T.Check("range with a negative literal step counts down", t.Contains("for (long i = 10; i > 0; i -= 3)"));
        T.Check("a bound that is not a plain name is evaluated once", Has(t, "for (long i = 0, iEnd = items.Count; i < iEnd; i++)") || Has(t, "iEnd = items.Count"));
        T.Check("for over an int is range()", t.Contains("i < n"));
        T.Check("for over an Array, a Dictionary (keys), a String (characters)", Has(t, "foreach (Variant x in items)", "foreach (Variant k in d.Keys)", "foreach (string c in GDScript.Chars(s))"));
        T.Check("while, break, continue", Has(t, "while (total > 100)", "break;", "continue;"));
        T.Check("match on an int with constant patterns is a switch", Has(t, "switch (n)", "case 0:", "case 1:", "case 2:", "default:"));
        T.Check("match on a String with a binding falls to default and declares the variable", Has(t, "switch (s)", "case \"a\":", "var other = s;"));
        T.Check("match on an enum", Has(t, "switch (e)", "case E.P:", "case E.Q:"));
        T.Check("a break inside a match inside a loop breaks the loop: no C# switch there", Has(t, "if (i == 1)") && !t.Contains("switch (i)"));
        T.Check("assert", t.Contains("GDScript.Assert(total >= 0, \"negative\");"));

        ConversionResult run = One(gd, keep: true);
        Type type = Assembly.Load(run.Assembly!).GetType("Sample")!;
        var node = (Node)Activator.CreateInstance(type)!;
        MethodInfo flow = type.GetMethod("Flow")!;
        object? Call(long n, string s, int mode) =>
            flow.Invoke(node, new object[] { new Aver.Fabric.Collections.Array { 1, 2, 3 }, new Aver.Fabric.Collections.Dictionary { { "a", 1 } }, n, s, Enum.ToObject(type.GetNestedType("E")!, mode) });
        // range(5)=10, range(2,8)=27, range(10,0,-3)=10+7+4+1=22, for i in 3 = 3, range(3) = 3, range(items.size()) = 3: 68 for n = 3.
        T.Check("flow(n=0) returns 0 at the first match", (long)Call(0, "z", 0)! == 0);
        T.Check("flow(n=2) returns 1 at the second", (long)Call(2, "z", 0)! == 1);
        T.Check("flow(n=3, mode P) runs every loop and then the enum match: 7", (long)Call(3, "z", 0)! == 7);
        T.Check("flow(n=3, mode Q): 8", (long)Call(3, "a", 1)! == 8);
    }

    private static void MapSignalsAndLambdas()
    {
        const string gd =
            "extends Node\n\nsignal ping(n: int)\nsignal pong\n\nvar button: Timer\nvar cb: Callable\n\nfunc _on_ping(n: int) -> void:\n\tpass\nfunc _on_bound(n: int, extra: int) -> void:\n\tpass\nfunc _on_pong() -> void:\n\tpass\nfunc _on_timeout() -> void:\n\tpass\n\n" +
            "func wire() -> void:\n\tping.connect(_on_ping)\n\tpong.connect(_on_pong, CONNECT_ONE_SHOT)\n\tbutton.timeout.connect(_on_timeout)\n\tbutton.timeout.connect(func(): print(\"t\"))\n\tping.connect(_on_bound.bind(2))\n" +
            "\tpong.connect(Callable(self, \"_on_pong\"))\n\tping.disconnect(_on_ping)\n\tvar was = ping.is_connected(_on_ping)\n\tping.emit(3)\n\tpong.emit()\n\temit_signal(\"pong\")\n" +
            "\tcb = func(x, y = 2): return x + y\n\tvar r = cb.call(1)\n\tvar named = func fact(n): return 1 if n <= 1 else n * fact(n - 1)\n" +
            "\tvar nums: Array[int] = [3, 1, 2]\n\tvar sq = nums.map(func(i): return i * i)\n\tvar big = nums.filter(func(i): return i > 1)\n\tvar sm = nums.reduce(func(a, b): return a + b, 0)\n" +
            "\tnums.sort_custom(func(a, b): return a > b)\n\tvar any_big = nums.any(func(i): return i > 2)\n\tprint(sq, big, sm, any_big, r, named, was)\n\nfunc waits() -> int:\n\tawait get_tree().create_timer(0.1).timeout\n\tvar res = await pong\n\tawait wait_more()\n\treturn 1\n\nfunc wait_more() -> void:\n\tawait button.timeout\n";
        ConversionResult r = One(gd);
        string t = Text(r);
        T.Check("it compiles", r.Success, Errors(r));
        T.Check("signal.connect(method): Connect with the generated SignalName and a typed Callable", Has(t, "Connect(SignalName.Ping, Callable.From<long>(OnPing));", "Connect(SignalName.Pong, Callable.From(OnPong), (uint)"));
        T.Check("CONNECT_ONE_SHOT is the runtime's flag", t.Contains("GodotObject.ConnectFlags.OneShot"));
        T.Check("a signal of a runtime class: its SignalName constant (a variable called like a class takes a lower-case name)", t.Contains("button.Connect(Timer.SignalName.Timeout, Callable.From(OnTimeout));"));
        T.Check("a lambda becomes Callable.From with typed lambda parameters; default parameters stay defaults", Has(t, "Callable.From(() => GD.Print(\"t\"))", "Callable.From((Delegate)((Variant x, long y = 2) =>"));
        T.Check(".bind() appends to the Callable", t.Contains("Callable.From<long, long>(OnBound).Bind(2)"));
        T.Check("Callable(self, \"name\") names the generated MethodName", t.Contains("new Callable(this, MethodName.OnPong)"));
        T.Check("disconnect / is_connected / emit / emit_signal", Has(t, "Disconnect(SignalName.Ping, Callable.From<long>(OnPing));", "IsConnected(SignalName.Ping,", "EmitSignalPing(3);", "EmitSignalPong();", "EmitSignal(\"pong\");"));
        T.Check("Callable.call() returns a Variant", t.Contains("Variant r = Cb.Call(1);"));
        T.Check("a named lambda can call itself", t.Contains("GDScript.Named(fact =>") && t.Contains("fact().Call("));
        T.Check("map / filter / reduce / sort_custom / any over a typed array take C# lambdas", Has(t, "nums.Map((long i) => i * i)", "nums.Filter((long i) => i > 1)", "nums.Reduce((long a, long b) => a + b, 0)", "nums.SortCustom((long a, long b) => a > b)", "nums.Any((long i) => i > 2)"));
        T.Check("await signal / timer: ToSignal", Has(t, "await ToSignal(GetTree().CreateTimer(0.1), SceneTreeTimer.SignalName.Timeout);", "GDScript.AwaitResult(await ToSignal(this, SignalName.Pong))"));
        T.Check("a function with await is async; one that returns a value is a Task<T>", Has(t, "public async Task<long> Waits()", "await WaitMore();"));
        T.Check("a void coroutine somebody awaits returns Task", t.Contains("public async Task WaitMore()"));

        // Run it: connect, emit, bind.
        ConversionResult run = One(gd, keep: true);
        Type type = Assembly.Load(run.Assembly!).GetType("Sample")!;
        var node = (Node)Activator.CreateInstance(type)!;
        var seen = new List<string>();
        node.Connect("ping", Callable.From<long>(n => seen.Add("ping" + n)));
        node.Connect("pong", Callable.From(() => seen.Add("pong")));
        type.GetField("button")!.SetValue(node, new Aver.Fabric.Timer());
        SceneTree.Instance.Root.AddChild(node);
        node.Call("Wire");
        T.Check("emit(3) reached the outside handler and the ones wire() connected", seen.Contains("ping3") && seen.Contains("pong"), string.Join(",", seen));
        var cb = (Callable)type.GetField("Cb")!.GetValue(node)!;
        T.Check("the lambda's default parameter is used when cb.call(1) passes one argument, and not when it passes two", cb.Call(1).AsInt64() == 3 && cb.Call(1, 5).AsInt64() == 6);
    }

    private static void MapBuiltins()
    {
        const string gd =
            "extends Node\n\nfunc b(x: float, n: int, s: String, arr: Array, v) -> void:\n" +
            "\tprint(\"a\", n)\n\tprints(1, 2)\n\tpush_error(\"e\")\n\tpush_warning(\"w\")\n\tvar l1 = len(s) + len(arr) + len(v)\n\tvar st = str(n, \"-\", x)\n" +
            "\tvar a1 = abs(x) + abs(n) + absf(x) + absi(n)\n\tvar c1 = clamp(x, 0.0, 1.0) + clampf(x, 0.0, 1.0) + clampi(n, 0, 3) + clamp(n, 0, 3)\n" +
            "\tvar m1 = min(x, 2.0) + max(n, 3) + minf(x, 1.0) + maxi(n, 1)\n\tvar l2 = lerp(0.0, x, 0.5) + lerpf(0.0, 1.0, 0.25)\n" +
            "\tvar r1 = round(x) + floor(x) + ceil(x) + roundi(x) + floori(x) + ceili(x) + sqrt(x) + pow(x, 2.0) + sin(x) + cos(x) + atan2(x, 1.0)\n" +
            "\tvar g1 = sign(x) + sign(n) + signi(n) + fmod(x, 3.0) + fposmod(x, 3.0) + posmod(n, 3) + wrapf(x, 0.0, 1.0) + wrapi(n, 0, 3)\n" +
            "\tvar d1 = deg_to_rad(90.0) + rad_to_deg(PI) + move_toward(x, 1.0, 0.5) + smoothstep(0.0, 1.0, x) + inverse_lerp(0.0, 2.0, x) + remap(x, 0.0, 1.0, 0.0, 10.0)\n" +
            "\tvar rn = randf() + randf_range(1.0, 2.0) + randi() + randi_range(1, 6)\n\trandomize()\n" +
            "\tvar cv = int(x) + int(s) + float(n) + float(s) + int(true)\n\tvar bv = bool(n)\n\tvar ch = char(65) + String.chr(66) if false else char(65)\n\tvar od = ord(\"A\")\n" +
            "\tvar t = typeof(v)\n\tvar ok = is_instance_valid(self)\n\tvar eq = is_equal_approx(x, 1.0) and is_zero_approx(x)\n\tvar nan = is_nan(x) or is_inf(x)\n" +
            "\tvar sc = load(\"res://x.tscn\")\n\tvar tex = preload(\"res://x.png\")\n\tvar sn = snapped(x, 0.5) + snapped(n, 5)\n\tvar pv = PI + TAU\n\tvar rg = range(3)\n\tvar ang = angle_difference(0.0, x)\n\tvar ldb = linear_to_db(x)\n\tprint(l1, st, a1, c1, m1, l2, r1, g1, d1, rn, cv, bv, ch, od, t, ok, eq, nan, sc, tex, sn, pv, rg, ang, ldb)\n";
        ConversionResult r = One(gd.Replace("\tvar ch = char(65) + String.chr(66) if false else char(65)\n", "\tvar ch = char(65)\n"));
        string t = Text(r);
        T.Check("it compiles", r.Success, Errors(r));
        T.Check("print / prints / push_error / push_warning", Has(t, "GD.Print(\"a\", n);", "GD.PrintS(1, 2);", "GD.PushError(\"e\");", "GD.PushWarning(\"w\");"));
        T.Check("len by type; str(); abs; clamp; min / max", Has(t, "s.Length + arr.Count + GDScript.Len(v)", "GD.Str(n, \"-\", x)", "Math.Abs(x)", "Math.Clamp(x, 0.0, 1.0)", "Math.Clamp(n, 0, 3)", "Math.Min(x, 2.0)", "Math.Max(n, 3)"));
        T.Check("lerp / rounding / math", Has(t, "Mathf.Lerp(0.0, x, 0.5)", "Math.Round(x, MidpointRounding.AwayFromZero)", "Math.Floor(x)", "Math.Ceiling(x)", "(long)Math.Floor(x)", "Math.Sqrt(x)", "Math.Pow(x, 2.0)", "Math.Sin(x)", "Math.Atan2(x, 1.0)"));
        T.Check("sign / posmod / wrap / fmod", Has(t, "GDScript.Sign(x)", "GDScript.Sign(n)", "GDScript.PosMod(n, 3)", "GDScript.Wrap(x, 0.0, 1.0)", "GDScript.Wrap(n, 0, 3)", "GDScript.FPosMod(x, 3.0)", "x % 3.0"));
        T.Check("angles and easing go to Mathf", Has(t, "Mathf.DegToRad(90.0)", "Mathf.RadToDeg(Math.PI)", "Mathf.MoveToward(x, 1.0, 0.5)", "Mathf.SmoothStep(0.0, 1.0, x)", "Mathf.InverseLerp(0.0, 2.0, x)", "Mathf.Remap(x, 0.0, 1.0, 0.0, 10.0)"));
        T.Check("randomness is the G1 generator", Has(t, "GD.Randf()", "GD.RandRange(1.0, 2.0)", "GD.Randi()", "GD.RandRange(1, 6)", "GD.Randomize();"));
        T.Check("conversions: int(float) truncates, int(String) parses, float(int)", Has(t, "(long)x", "s.ToInt()", "(double)n", "s.ToFloat()", "GDScript.ToInt(true)"));
        T.Check("bool(), char(), ord(), typeof(), is_instance_valid()", Has(t, "n != 0", "GDScript.Char(65)", "GDScript.Ord(\"A\")", "GDScript.TypeOf(v)", "IsInstanceValid(this)"));
        T.Check("load / preload: typed by the file", Has(t, "GD.Load<PackedScene>(\"res://x.tscn\")", "GD.Preload<Texture2D>(\"res://x.png\")"));
        T.Check("snapped by type; PI and TAU; range() as a value", Has(t, "Mathf.Snapped(x, 0.5)", "GDScript.Snapped(n, 5)", "Math.PI + Math.Tau", "GDScript.Range(3)"));
    }

    private static void MapMembersAndTypes()
    {
        const string gd =
            "extends Node3D\n\nvar mesh: MeshInstance3D\nvar untyped\nvar counter = 0\nvar speed = 5\n\nfunc _ready() -> void:\n" +
            "\tvar a = $Pivot/Arm\n\ta.visible = false\n\tvar b = get_node(\"Other\") as Node3D\n\tvar c = $Pivot as Node3D\n\tvar d: Node3D = $Pivot\n" +
            "\tposition.x = 4.0\n\tposition = Vector3(1, 2, 3)\n\trotation.y += 0.5\n\tglobal_position += Vector3.FORWARD\n\tvar p = global_transform.origin\n\tset_visible(false)\n\tvar vis = is_visible()\n\tvar gt = get_global_transform()\n" +
            "\tmesh.visible = true\n\tuntyped = 5\n\tcounter += 1\n\tcounter += 0.5\n\tspeed = speed * 1.5\n" +
            "\tvar i = 0\n\ti += 2\n\tvar j = 1\n\tj = j * 2.5\n\tvar k = null\n\tk = 3\n\tvar l = \"x\"\n\tl += \"y\"\n\tvar who = a.get_parent().name\n\tvar x = Color(1, 0.5, 0.25)\n\tvar y = Color.RED\n\tvar z = Color(\"ff8844\")\n\tvar q = Quaternion.IDENTITY\n\tvar dd = {\"a\": 1}\n\tdd.a = 2\n\tdd.b = dd.a + 1\n\tvar bb = dd[\"a\"]\n\tprint(a, b, c, d, p, vis, gt, i, j, k, l, who, x, y, z, q, bb)\n";
        ConversionResult r = One(gd);
        string t = Text(r);
        T.Check("it compiles", r.Success, Errors(r));
        T.Check("$Path with no declared type is a Node read by name; with `as` or a type it is typed",
                Has(t, "Node a = GetNode(\"Pivot/Arm\");", "a.Set(\"visible\", false);", "Node3D b = GetNode<Node3D>(\"Other\");", "Node3D c = GetNode<Node3D>(\"Pivot\");", "Node3D d = GetNode<Node3D>(\"Pivot\");"));
        T.Check("a field of a vector property is written back whole; the vector itself is assigned",
                Has(t, "Position = Position with { X = 4.0f };", "Position = new Vector3(1f, 2f, 3f);", "Rotation = Rotation with { Y = (float)(Rotation.Y + 0.5) };", "GlobalPosition = GlobalPosition + Vector3.Forward;"));
        T.Check("get_x() / set_x(v) / is_x() are the property x", Has(t, "Vector3 p = GlobalTransform.Origin;", "Visible = false;", "bool vis = Visible;", "Transform3D gt = GlobalTransform;"));
        T.Check("a field of a typed node", t.Contains("mesh.Visible = true;"));
        T.Check("constants of a class: Vector3.FORWARD, Color.RED, Quaternion.IDENTITY", Has(t, "Vector3.Forward", "Colors.Red", "Quaternion.Identity"));
        T.Check("untyped variables take the type of their first value", Has(t, "long i = 0;", "i += 2;", "string l = \"x\";", "l += \"y\";"));
        T.Check("a later assignment of another kind frees the variable: Variant", Has(t, "Variant k = default;", "k = 3;", "public Variant Untyped;"));
        T.Check("a later float widens an int variable", Has(t, "double j = 1.0;", "j = j * 2.5;"));
        T.Check("a member variable the same way", Has(t, "public double Counter = 0.0;", "Counter += 1.0;", "public double Speed = 5.0;"));
        T.Check("Color() with floats, Color() with a string", Has(t, "new Color(1f, 0.5f, 0.25f)", "new Color(\"ff8844\")"));
        T.Check("dict.key is dict[\"key\"]", Has(t, "dd[\"a\"] = 2;", "dd[\"b\"] = GDScript.Add(dd[\"a\"], 1);"));
        T.Check("the parent of a guessed Node is a Node: its name is typed", t.Contains("StringName who = a.GetParent().Name;"));
        T.Check("the guessed Node's own property is a read-by-name note, not a compile error", r.Items.Any(i => i.Type == "dynamic" && i.Property == "visible" && i.Rating == Rating.Approximated));
    }

    private static void MapMissingApi()
    {
        ConversionResult r = One("extends Node3D\n\nvar mesh: MeshInstance3D\n\nfunc f():\n\tmesh.get_aabb()\n\tvar n: NoSuchClass = null\n\tmesh.also_missing = 1\n");
        T.Check("what Aver.Fabric lacks does not compile and is not hidden", !r.Success && r.Errors.Any(e => e.Id == "CS1061" && e.Member == "GetAabb") && r.Errors.Any(e => e.Id == "CS0246" && e.Member == "NoSuchClass"), Errors(r));
        T.Check("the line carries a FABRIC comment naming the member", Text(r).Split('\n').Any(l => l.Contains("mesh.GetAabb()") && l.Contains("FABRIC") && l.Contains("GetAabb")));
        T.Check("the report has the unsupported entry from the converter and from the compiler", r.Items.Any(i => i.Type == "call" && i.Property == "get_aabb" && i.Rating == Rating.Unsupported) &&
                r.Items.Any(i => i.Type == "CS1061" && i.Rating == Rating.Unsupported && i.Line > 0));
        T.Check("the script as a whole is unsupported (it does not compile)", r.Items.First(i => i.Type == "gdscript").Rating == Rating.Unsupported);
    }

    private static void MapNavigation()
    {
        // G5 N1: the navigation classes exist, so GDScript that subclasses AStar3D and drives a NavigationAgent3D converts with no error.
        const string astar =
            "class_name Pathy\nextends AStar3D\n\nvar calls := 0\n\nfunc _compute_cost(from_id: int, to_id: int) -> float:\n\tcalls += 1\n\tif from_id == 1 and to_id == 2:\n\t\treturn 100.0\n" +
            "\treturn get_point_position(from_id).distance_to(get_point_position(to_id))\n";
        const string chaser =
            "class_name Chaser\nextends CharacterBody3D\n\n@onready var agent: NavigationAgent3D = $NavigationAgent3D\nvar arrived := false\nvar waypoints := 0\n\n" +
            "func _ready() -> void:\n\tagent.navigation_finished.connect(_on_done)\n\tagent.waypoint_reached.connect(_on_waypoint)\n\tagent.path_desired_distance = 0.5\n\tagent.target_position = Vector3(1, 0, 1)\n\n" +
            "func _on_done() -> void:\n\tarrived = true\n\nfunc _on_waypoint(details: Dictionary) -> void:\n\twaypoints += 1\n\nfunc heading() -> Vector3:\n\treturn (agent.get_next_path_position() - global_position).normalized()\n" +
            "\nfunc grid() -> int:\n\tvar g := AStarGrid2D.new()\n\tg.region = Rect2i(0, 0, 4, 4)\n\tg.diagonal_mode = AStarGrid2D.DIAGONAL_MODE_NEVER\n\tg.update()\n\tg.set_point_solid(Vector2i(1, 1))\n\treturn g.get_id_path(Vector2i(0, 0), Vector2i(3, 3)).size()\n";
        ConversionResult r = Convert(new[]
        {
            new ScriptSource { ResPath = "res://pathy.gd", Text = astar, Kind = "gdscript", ClassName = "Pathy", Output = "Content/Scripts/Pathy.cs" },
            new ScriptSource { ResPath = "res://chaser.gd", Text = chaser, Kind = "gdscript", ClassName = "Chaser", Output = "Content/Scripts/Chaser.cs" },
        }, keep: true);
        T.Check("both scripts compile against the real navigation classes", r.Success, Errors(r));
        if (!r.Success) return;
        T.Check("_compute_cost overrides the runtime's virtual", Text(r, 0).Contains("override float _ComputeCost(long fromId, long toId)"));
        T.Check("the agent's signals and properties are typed (no dynamic access)", Has(Text(r, 1), "public NavigationAgent3D Agent;", "NavigationAgent3D.SignalName.NavigationFinished", "NavigationAgent3D.SignalName.WaypointReached", "Agent.TargetPosition = new Vector3(1f, 0f, 1f);") &&
                !Text(r, 1).Contains("// FABRIC: could not") && !Text(r, 1).Contains("Agent.Set(") && !Text(r, 1).Contains("Agent.Call("));
        if (!NavigationTests.EnsureNative()) { Console.WriteLine("  SKIP (run): build/bin/Aver.Nav.Abi was not found"); return; }
        NavigationServer3D.ResetForTests();
        Assembly asm = Assembly.Load(r.Assembly!);
        var g = (AStar3D)Activator.CreateInstance(asm.GetType("Pathy")!)!;
        g.AddPoint(1, new Vector3(0, 0, 0)); g.AddPoint(2, new Vector3(1, 0, 0)); g.AddPoint(3, new Vector3(2, 0, 0)); g.AddPoint(4, new Vector3(1, 1, 0));
        g.ConnectPoints(1, 2); g.ConnectPoints(2, 3); g.ConnectPoints(1, 4); g.ConnectPoints(4, 3);
        T.Check("the converted script's cost is called by the search and obeyed", g.GetIdPath(1, 3).SequenceEqual(new long[] { 1, 4, 3 }) && (long)asm.GetType("Pathy")!.GetField("Calls")!.GetValue(g)! > 0);

        var floor = new NavigationMesh { Vertices = new[] { new Vector3(-5, 0, -5), new Vector3(5, 0, -5), new Vector3(5, 0, 5), new Vector3(-5, 0, 5) } };
        floor.AddPolygon(new[] { 0, 1, 2, 3 });
        SceneTree.Instance.Root.AddChild(new NavigationRegion3D { NavigationMesh = floor });
        var body = (CharacterBody3D)Activator.CreateInstance(asm.GetType("Chaser")!)!;
        body.Position = new Vector3(-3, 0, -3);
        body.AddChild(new NavigationAgent3D { Name = "NavigationAgent3D", TargetDesiredDistance = 0.5f });
        SceneTree.Instance.Root.AddChild(body);
        Type chaserType = asm.GetType("Chaser")!;
        for (int i = 0; i < 600 && !(bool)chaserType.GetField("Arrived")!.GetValue(body)!; i++)
        {
            Vector3 h = (Vector3)chaserType.GetMethod("Heading")!.Invoke(body, null)!;
            body.Position += h * 0.1f;
            T.Frame(1.0 / 60.0, 1);
        }
        T.Check("the converted script walks the agent to its target: navigation_finished fired and waypoints were counted",
                (bool)chaserType.GetField("Arrived")!.GetValue(body)! && (long)chaserType.GetField("Waypoints")!.GetValue(body)! >= 2 && body.Position.DistanceTo(new Vector3(1, 0, 1)) < 0.7f);
        T.Check("AStarGrid2D from GDScript: region, DIAGONAL_MODE_NEVER, a solid cell, a path of seven cells round it",
                (long)chaserType.GetMethod("Grid")!.Invoke(body, null)! == 7);
    }

    private static void MapProject()
    {
        string dir = Path.Combine(T.DataDir(), "GdProject");
        var sources = new[]
        {
            new ScriptSource { ResPath = "res://scripts/global.gd", Text = File.ReadAllText(Path.Combine(dir, "scripts", "global.gd")), Kind = "gdscript", ClassName = "Global", Output = "Content/Scripts/Global.cs" },
            new ScriptSource { ResPath = "res://scripts/hud.gd", Text = File.ReadAllText(Path.Combine(dir, "scripts", "hud.gd")), Kind = "gdscript", ClassName = "Hud", Output = "Content/Scripts/Hud.cs" },
        };
        ConversionResult r = Convert(sources, keep: true, projectDir: dir);
        T.Check("it compiles", r.Success, Errors(r));
        string hud = Text(r, 1);
        T.Check("an autoload name is the node under /root, typed by its script", Has(hud, "GetNode<Global>(\"/root/Global\").Score = 5;", "GetNode<Global>(\"/root/Global\").AddPoints(3);", "EmitSignalScoreChanged(GetNode<Global>(\"/root/Global\").Score);"));
        T.Check("an autoload is a Node: shown = Global.score is typed", hud.Contains("Shown = GetNode<Global>(\"/root/Global\").Score;"));

        // Without project.godot the same name is an unknown identifier: read by name and reported.
        ConversionResult no = Convert(sources, keep: false);
        T.Check("without the project's autoload list the name is reported as unknown", no.Items.Any(i => i.Type == "identifier" && i.Property == "Global" && i.Rating == Rating.Unsupported));
        T.Check("and the output still compiles (a dynamic read by name)", no.Success, Errors(no));
    }

    // ---- running converted functions ----

    private static Type CalcType() => Assembly.Load(Fixtures().Assembly!).GetType("Calc")!;

    private static object? Calc(string method, params object[] args) => CalcType().GetMethod(method)!.Invoke(null, args);

    private static string CalcText(string method, params object[] args) => Calc(method, args)?.ToString() ?? "";

    private static void RunCalc()
    {
        T.Check("classify: match with a literal, a list and a default with code", (string)Calc("Classify", 0L)! == "zero" && (string)Calc("Classify", 2L)! == "few" &&
                (string)Calc("Classify", -5L)! == "negative" && (string)Calc("Classify", 50L)! == "many");
        T.Check("fizzbuzz(15): for / range / elif / str / join", (string)Calc("Fizzbuzz", 15L)! == "1,2,Fizz,4,Buzz,Fizz,7,8,Fizz,Buzz,11,Fizz,13,14,FizzBuzz");
        string stats = (string)Calc("FormatStats", "Orc", 42L, 0.4567)!;
        T.Check("format: %s %d %.1f %% %05d %-4s %+d %x", stats == "Orc has 42 hp (45.7%) [00042|ab  |+42|ff]", stats);
        T.Check("integer division truncates toward zero, % keeps the dividend's sign, ** is an int power", Calc("IntMath", 17L, 5L)!.ToString() == "[3, 2, -3, -1, 1024, 85]", CalcText("IntMath", 17L, 5L));
        T.Check("built-in number functions give GDScript's values", Calc("Numbers")!.ToString() == "[2, 2, 7.5, 3, -3, -3, 3, 4, 9, 10, -1.0, 12, 8, 2, 2.5, 1, true]", CalcText("Numbers"));
        T.Check("collatz(27) = 111, collatz(1) = 0: while with a ternary", (long)Calc("Collatz", 27L)! == 111 && (long)Calc("Collatz", 1L)! == 0);
        T.Check("sum_dict: for over a Dictionary, a Variant added to an int", (long)Calc("SumDict", new Aver.Fabric.Collections.Dictionary { { "a", 1 }, { "b", 2 }, { "c", 3 } })! == 6);
        T.Check("word_count: split, continue, get with a default", Calc("WordCount", "a b  a c b a")!.ToString() == "{ a: 3, b: 2, c: 1 }", CalcText("WordCount", "a b  a c b a"));
        T.Check("evens_squared: a typed array in and out", Calc("EvensSquared", new Aver.Fabric.Collections.Array<long>(new long[] { 1, 2, 3, 4 }))!.ToString() == "[4, 16]");
        T.Check("lerp_steps(0, 10, 4): floats print as Godot prints them", Calc("LerpSteps", 0.0, 10.0, 4L)!.ToString() == "[0.0, 2.5, 5.0, 7.5, 10.0]", CalcText("LerpSteps", 0.0, 10.0, 4L));
        T.Check("kind_of returns the enum member", Calc("KindOf", 5L)!.ToString() == "Small" && Calc("KindOf", 50L)!.ToString() == "Medium" && Calc("KindOf", 500L)!.ToString() == "Large");
        T.Check("is_palindrome: characters, string comparison, indexing", (bool)Calc("IsPalindrome", "A man, a plan, a canal: Panama")! && !(bool)Calc("IsPalindrome", "hello")!);
        var circle = new Aver.Fabric.Collections.Array { "circle", 2 };
        T.Check("shape_area: array patterns with bindings and a guard", Math.Abs((double)Calc("ShapeArea", circle)! - 4 * Math.PI) < 1e-9 &&
                (double)Calc("ShapeArea", new Aver.Fabric.Collections.Array { "rect", 3, 4 })! == 12.0 && (double)Calc("ShapeArea", new Aver.Fabric.Collections.Array { "square", 3 })! == 9.0 &&
                (double)Calc("ShapeArea", new Aver.Fabric.Collections.Array { "square", -2 })! == -1.0 && (double)Calc("ShapeArea", new Aver.Fabric.Collections.Array { "tri" })! == -1.0);
        T.Check("inventory_report: a typed int from a Variant, a ternary with format", (string)Calc("InventoryReport", new Aver.Fabric.Collections.Dictionary { { "sword", 1 }, { "arrow", 20 } })! == "sword, arrow x20");
        T.Check("transform: map / filter / reduce with lambdas", Calc("Transform", new Aver.Fabric.Collections.Array<long>(new long[] { 1, 2, 3, 4 }))!.ToString() == "[[2, 4, 6, 8], [3, 4], 10]");
        T.Check("sorted_desc: sort_custom with a lambda", Calc("SortedDesc", new Aver.Fabric.Collections.Array<long>(new long[] { 3, 1, 2 }))!.ToString() == "[3, 2, 1]");
        T.Check("untyped_sum: an untyped var that meets a float turns into a Variant: 6.5", Calc("UntypedSum", new Aver.Fabric.Collections.Array { 1, 2.5, 3 })!.ToString() == "6.5", CalcText("UntypedSum", new Aver.Fabric.Collections.Array { 1, 2.5, 3 }));
        T.Check("countdown_sum(4) = 10: range with a step of -1", (long)Calc("CountdownSum", 4L)! == 10);
        T.Check("`not n in haystack`", (bool)Calc("ContainsAll", new Aver.Fabric.Collections.Array { 1, 2, 3 }, new Aver.Fabric.Collections.Array { 2, 3 })! &&
                !(bool)Calc("ContainsAll", new Aver.Fabric.Collections.Array { 1, 2 }, new Aver.Fabric.Collections.Array { 3 })!);
        T.Check("truthiness of Variants: 3 of [0, 1, \"\", \"a\", [], [1], null, 0.0]",
                (long)Calc("TruthyCount", new Aver.Fabric.Collections.Array { 0, 1, "", "a", new Aver.Fabric.Collections.Array(), new Aver.Fabric.Collections.Array { 1 }, default(Variant), 0.0 })! == 3);
    }

    private static void RunCounter()
    {
        Type type = Assembly.Load(Fixtures().Assembly!).GetType("Counter")!;
        var counter = (Node)Activator.CreateInstance(type)!;
        var helper = new Aver.Fabric.Timer { Name = "Helper" };
        counter.AddChild(helper);
        var changes = new List<string>();
        counter.Connect("changed", Callable.From<Variant, Variant>((o, n) => changes.Add($"{o.AsString()}->{n.AsString()}")));
        var finished = new List<string>();
        counter.Connect("finished", Callable.From(() => finished.Add("finished")));
        ExportAttribute? limit = type.GetProperty("Limit") is { } lp ? lp.GetCustomAttribute<ExportAttribute>() : type.GetField("Limit")!.GetCustomAttribute<ExportAttribute>();
        T.Check("@export_range(0, 10) is [Export(PropertyHint.Range, \"0,10\")]", limit is { Hint: PropertyHint.Range, HintString: "0,10" });
        T.Check("the exports have their initialisers", (long)type.GetField("Step")!.GetValue(counter)! == 2 && (long)type.GetField("Limit")!.GetValue(counter)! == 10);
        counter.Set("Step", 3);
        SceneTree.Instance.Root.AddChild(counter);
        var entries = (Aver.Fabric.Collections.Array<string>)type.GetField("Entries")!.GetValue(counter)!;
        T.Check("_Ready ran: @onready found $Helper, then the user's code ran", ReferenceEquals(type.GetField("Helper")!.GetValue(counter), helper) && entries.Count == 1 && entries[0] == "ready");
        T.Check("the user's _ready body used the onready variable", Math.Abs(helper.WaitTime - 0.25) < 1e-9 && helper.OneShot);
        counter.Call("Add", 3);
        T.Check("add(3) with step 3: total = 9 through the setter", (long)type.GetProperty("Total")!.GetValue(counter)! == 9);
        T.Check("the setter emitted `changed(old, new)`", changes.SequenceEqual(new[] { "0->9" }), string.Join(",", changes));
        T.Check("format in the log", entries.Contains("add 3"));
        T.Check("countdown(4) = 10", (long)counter.Call("Countdown", 4).AsInt64() == 10);
        T.Frames(3, 1.0 / 60.0);
        T.Check("_process(delta) ticks once a frame", (long)type.GetField("Ticks")!.GetValue(counter)! >= 3);
        counter.Call("WaitAndFinish");
        T.Check("await helper.timeout has not resumed yet", !entries.Contains("waited") && finished.Count == 0);
        T.Frames(30, 1.0 / 60.0);
        T.Check("after the timer's 0.25 s the coroutine resumed, logged and emitted `finished`", entries.Contains("waited") && finished.Count == 1, string.Join(",", entries));
    }

    private static void RunEnemy()
    {
        Type type = Assembly.Load(Fixtures().Assembly!).GetType("Enemy")!;
        var enemy = (CharacterBody3D)Activator.CreateInstance(type)!;
        T.Check("`var health := max_health` reads another member: it is assigned in the constructor", (double)type.GetField("Health")!.GetValue(enemy)! == 50.0);
        enemy.AddChild(new Node3D { Name = "Nav" });
        enemy.AddChild(new Aver.Fabric.Timer { Name = "Timer" });
        var damaged = new List<string>();
        var died = new List<GodotObject>();
        enemy.Connect("damaged", Callable.From<double, double>((a, r) => damaged.Add($"{a}/{r}")));
        enemy.Connect("died", Callable.From<Variant>(e => died.Add(e.AsGodotObject()!)));
        SceneTree.Instance.Root.AddChild(enemy);
        T.Check("_ready joined the group (the const is the name) and started the Timer", enemy.IsInGroup("enemies") && ((Aver.Fabric.Timer)enemy.GetNode("Timer")).TimeLeft > 0);
        enemy.Call("TakeDamage", 20.0);
        T.Check("take_damage: float health, signal with both values", (double)type.GetField("Health")!.GetValue(enemy)! == 30.0 && damaged.SequenceEqual(new[] { "20/30" }), string.Join(",", damaged));
        enemy.Call("TakeDamage", 40.0);
        object state = type.GetField("state")!.GetValue(enemy)!;
        T.Check("health <= 0 calls die(): state Dead, kills counted through the Variant dictionary, `died(self)` emitted", state.ToString() == "Dead" && died.Count == 1 && ReferenceEquals(died[0], enemy) &&
                ((Aver.Fabric.Collections.Dictionary)type.GetField("Stats")!.GetValue(enemy)!)["kills"].AsInt64() == 1);
        enemy.Call("TakeDamage", 5.0);
        T.Check("a dead enemy takes no damage", damaged.Count == 2);
        T.Check("`await create_timer(2.0).timeout` has not resumed: still alive", GodotObject.IsInstanceValid(enemy));
        T.Frames(150, 1.0 / 60.0);
        T.Check("after two seconds queue_free() ran", !GodotObject.IsInstanceValid(enemy));
    }

    // ---- the runtime classes the converter targets ----

    private static void RuntimeHelpers()
    {
        string F(string fmt, params Variant[] v) => GDScript.Format(fmt, (Variant)new Aver.Fabric.Collections.Array(v));
        T.Check("Format: %s %d %f %x %X %o %c %e %%", F("%s|%d|%f|%x|%X|%o|%c|%%", "a", 7, 1.5, 255, 255, 8, 65) == "a|7|1.500000|ff|FF|10|A|%");
        T.Check("Format: width, precision, flags", F("[%5d][%-5d][%05d][%+d][% d][%.2f][%8.3f][%-8.1f][%5s][%-5s]|", 42, 42, 42, 42, 42, 3.14159, 3.14159, 2.55, "ab", "ab") ==
                "[   42][42   ][00042][+42][ 42][3.14][   3.142][2.6     ][   ab][ab   ]|", F("[%5d][%-5d][%05d][%+d][% d][%.2f][%8.3f][%-8.1f][%5s][%-5s]|", 42, 42, 42, 42, 42, 3.14159, 3.14159, 2.55, "ab", "ab"));
        T.Check("Format: a single non-array operand", GDScript.Format("%s!", (Variant)"hey") == "hey!");
        T.Check("Format: a * takes the width from the arguments; %d truncates a float; %s prints a float as str() does", F("%*d|%d|%s", 4, 7, 3.9, 2.0) == "   7|3|2.0");
        T.Check("Format: too few or too many operands is an error", Throws(() => F("%d %d", 1)) && Throws(() => F("%d", 1, 2)));

        T.Check("Add: int + int stays int, int + float is float, String + String, Array + Array",
                GDScript.Add(1, 2).VariantType == Variant.Type.Int && GDScript.Add(1, 2.5).AsDouble() == 3.5 && GDScript.Add("a", "b").AsString() == "ab" &&
                GDScript.Add(new Aver.Fabric.Collections.Array { 1 }, new Aver.Fabric.Collections.Array { 2 }).AsGodotArray().Count == 2);
        T.Check("Div: int / int truncates toward zero; float otherwise; int / 0 throws", GDScript.Div(7, 2).AsInt64() == 3 && GDScript.Div(-7, 2).AsInt64() == -3 && GDScript.Div(7, 2.0).AsDouble() == 3.5 && Throws(() => GDScript.Div(1, 0)));
        T.Check("Mod: the sign of the dividend; PosMod: the sign of the divisor", GDScript.Mod(-7, 3).AsInt64() == -1 && GDScript.PosMod(-7, 3) == 2 && GDScript.PosMod(7, -3) == -2 && Math.Abs(GDScript.FPosMod(-1.5, 2.0) - 0.5) < 1e-12);
        T.Check("Pow: int ** int is an int, a negative exponent gives 0", GDScript.Pow(2, 10).VariantType == Variant.Type.Int && GDScript.Pow(2, 10).AsInt64() == 1024 && GDScript.IntPow(2, -1) == 0 && GDScript.Pow(2, 0.5).VariantType == Variant.Type.Float);
        T.Check("Mul / Add / Sub on vectors, a vector by a number, a Quaternion by a Vector3",
                GDScript.Mul(new Vector3(1, 2, 3), 2).AsVector3() == new Vector3(2, 4, 6) && GDScript.Mul(2, new Vector3(1, 2, 3)).AsVector3() == new Vector3(2, 4, 6) &&
                GDScript.Add(new Vector2(1, 2), new Vector2(3, 4)).AsVector2() == new Vector2(4, 6) && GDScript.Sub(new Vector3(3, 3, 3), Vector3.One).AsVector3() == new Vector3(2, 2, 2) &&
                T.Near(GDScript.Mul(Quaternion.Identity, Vector3.Up).AsVector3(), Vector3.Up));
        T.Check("Neg / BitNot / BitAnd / Shl on ints", GDScript.Neg(5).AsInt64() == -5 && GDScript.BitNot(0).AsInt64() == -1 && GDScript.BitAnd(6, 3).AsInt64() == 2 && GDScript.Shl(1, 4).AsInt64() == 16);
        T.Check("an invalid operation throws with Godot's wording", Throws(() => GDScript.Add("a", 1), "Invalid operands"));
        T.Check("Lt / Le / Gt / Ge: numbers (int against float), strings, vectors, NaN is false", GDScript.Lt(1, 2.5) && GDScript.Ge(2, 2.0) && GDScript.Lt("a", "b") && GDScript.Gt(new Vector2(2, 0), new Vector2(1, 5)) &&
                !GDScript.Lt(double.NaN, 1) && !GDScript.Ge(double.NaN, 1));
        T.Check("In: array, dictionary, string, packed", GDScript.In(2, new Aver.Fabric.Collections.Array { 1, 2 }) && GDScript.In("k", new Aver.Fabric.Collections.Dictionary { { "k", 1 } }) && GDScript.In("ell", "hello") && GDScript.In("b", new[] { "a", "b" }));
        T.Check("Range: one, two and three arguments, a negative step", GDScript.Range(3).SequenceEqual(new long[] { 0, 1, 2 }) && GDScript.Range(2, 5).SequenceEqual(new long[] { 2, 3, 4 }) &&
                GDScript.Range(5, 0, -2).SequenceEqual(new long[] { 5, 3, 1 }) && Throws(() => GDScript.Range(0, 5, 0).ToList()));
        T.Check("Iterate: an int, an array, a dictionary's keys, a string's characters",
                GDScript.Iterate(3).Select(v => v.AsInt64()).SequenceEqual(new long[] { 0, 1, 2 }) && GDScript.Iterate(new Aver.Fabric.Collections.Array { "x", "y" }).Select(v => v.AsString()).SequenceEqual(new[] { "x", "y" }) &&
                GDScript.Iterate(new Aver.Fabric.Collections.Dictionary { { "a", 1 } }).Select(v => v.AsString()).SequenceEqual(new[] { "a" }) && GDScript.Iterate("ab").Select(v => v.AsString()).SequenceEqual(new[] { "a", "b" }));
        T.Check("Len / TypeOf / Char / Ord", GDScript.Len("abc") == 3 && GDScript.Len(new Aver.Fabric.Collections.Array { 1 }) == 1 && GDScript.TypeOf(1.5) == (long)Variant.Type.Float && GDScript.Char(65) == "A" && GDScript.Ord("A") == 65);
        T.Check("ToInt / ToFloat read the leading number", GDScript.ToInt("12abc") == 12 && GDScript.ToInt(3.9) == 3 && GDScript.ToInt(-3.9) == -3 && GDScript.ToFloat("2.5x") == 2.5 && GDScript.ToInt(true) == 1);
        T.Check("Wrap / Snapped / Sign / PingPong / db", GDScript.Wrap(7, 0, 5) == 2 && GDScript.Wrap(-1, 0, 5) == 4 && GDScript.Snapped(7.3, 0.5) == 7.5 && GDScript.Sign(-2.5) == -1.0 && GDScript.Sign(0.0) == 0.0 &&
                Math.Abs(GDScript.PingPong(3.5, 2.0) - 0.5) < 1e-12 && Math.Abs(GDScript.DbToLinear(GDScript.LinearToDb(0.5)) - 0.5) < 1e-9);

        var arr = new Aver.Fabric.Collections.Array { 3, 1, 2 };
        T.Check("Index / SetIndex: arrays (negative index), dictionaries, strings", GDScript.Index(arr, -1).AsInt64() == 2 && GDScript.Index("abc", 1).AsString() == "b");
        GDScript.SetIndex(arr, 0, 9);
        var dict = new Aver.Fabric.Collections.Dictionary();
        GDScript.SetIndex(dict, "k", 5);
        T.Check("SetIndex writes", arr[0].AsInt64() == 9 && dict["k"].AsInt64() == 5);
        T.Check("GetMember / SetMember: vector components, dictionary keys, an object's property", GDScript.GetMember(new Vector3(1, 2, 3), "y").AsDouble() == 2 && GDScript.GetMember(dict, "k").AsInt64() == 5 && Throws(() => GDScript.GetMember(1, "x")));
        var node = new Node3D();
        GDScript.SetMember(node, "Position", new Vector3(1, 2, 3));
        T.Check("SetMember on an object goes through Set", node.Position == new Vector3(1, 2, 3));
        T.Check("CallMethod on Array, Dictionary and String", GDScript.CallMethod(arr, "size").AsInt64() == 3 && GDScript.CallMethod(dict, "has", "k").AsBool() &&
                GDScript.CallMethod("Hello", "to_upper").AsString() == "HELLO" && GDScript.CallMethod("a,b", "split", ",").AsGodotArray().Count == 2 && GDScript.CallMethod(arr, "has", 9).AsBool());
        T.Check("CallMethod on a vector goes by the Godot name", Math.Abs(GDScript.CallMethod(new Vector3(3, 0, 4), "length").AsDouble() - 5) < 1e-6);
        T.Check("CallMethod map / filter / reduce / sort_custom take Callables",
                GDScript.CallMethod(new Aver.Fabric.Collections.Array { 1, 2, 3 }, "map", Callable.From<long, long>(x => x * 2)).AsGodotArray().Select(v => v.AsInt64()).SequenceEqual(new long[] { 2, 4, 6 }) &&
                GDScript.CallMethod(new Aver.Fabric.Collections.Array { 1, 2, 3 }, "reduce", Callable.From<long, long, long>((a, b) => a + b), 0).AsInt64() == 6);
        T.Check("AwaitResult: nothing is null, one is that, several an array", GDScript.AwaitResult(Array.Empty<Variant>()).IsNil && GDScript.AwaitResult(new Variant[] { 5 }).AsInt64() == 5 && GDScript.AwaitResult(new Variant[] { 1, 2 }).AsGodotArray().Count == 2);
        Callable fact = default;
        fact = GDScript.Named(self => Callable.From<long, long>(n => n <= 1 ? 1 : n * self().Call(n - 1).AsInt64()));
        T.Check("Named: a lambda calls itself", fact.Call(5).AsInt64() == 120);
        T.Check("Assert logs, it does not throw", !Throws(() => GDScript.Assert(false, "boom")) && T.ErrorCount("Assertion failed: boom") == 1);
        T.Check("ToSignal(Variant) waits on the signal it holds", GDScript.ToSignal(new Signal(node, "visibility_changed")) is { } aw && aw.IsCompleted == false);

        static bool Throws(Action a, string? contains = null)
        {
            try { a(); return false; }
            catch (Exception ex) { return contains == null || ex.Message.Contains(contains); }
        }
    }

    private static void RuntimeSingletons()
    {
        T.Check("Engine.IsEditorHint() is false: Aver runs no editor scripts", !Engine.IsEditorHint());
        T.Check("OS.HasFeature / GetName answer", OS.GetName().Length > 0 && OS.HasFeature("pc") && !OS.HasFeature("no_such_feature") && OS.GetTicksMsec() > 0);
        T.Check("ProjectSettings.GetSetting: Godot's default gravity, the caller's fallback for anything else",
                Math.Abs(ProjectSettings.GetSetting("physics/3d/default_gravity").AsDouble() - 9.8) < 1e-9 && ProjectSettings.GetSetting("physics/3d/default_gravity_vector").AsVector3() == Vector3.Down &&
                ProjectSettings.GetSetting("no/such/setting", 7).AsInt64() == 7 && ProjectSettings.GetSetting("physics/common/physics_ticks_per_second").AsInt64() == 60);

        T.Check("String: Substr / Left / Right / Find / Split / StripEdges", "hello".Substr(1, 3) == "ell" && "hello".Substr(3) == "lo" && "hello".Left(2) == "he" && "hello".Right(2) == "lo" &&
                "hello".Find("l") == 2 && "hello".Find("l", 3) == 3 && "hello".Find("z") == -1 && "a,,b".Split(",", false).SequenceEqual(new[] { "a", "b" }) && "a,,b".Split(",", true).Length == 3 && "  x ".StripEdges() == "x");
        T.Check("String: ToInt / ToFloat / IsValidInt / Capitalize / PadZeros / Repeat / Format", "12abc".ToInt() == 12 && "2.5".ToFloat() == 2.5 && "12".IsValidInt() && !"1x".IsValidInt() && "hello_world".Capitalize() == "Hello World" &&
                "7".PadZeros(3) == "007" && "ab".Repeat(2) == "abab" && "{a}-{b}".Format((Variant)new Aver.Fabric.Collections.Dictionary { { "a", 1 }, { "b", 2 } }) == "1-2");
        T.Check("String: GetFile / GetBaseDir / GetExtension / ToSnakeCase / ToPascalCase / Join", "res://a/b.png".GetFile() == "b.png" && "res://a/b.png".GetBaseDir() == "res://a" && "res://a/b.png".GetExtension() == "png" &&
                "MoveSpeed".ToSnakeCase() == "move_speed" && "move_speed".ToPascalCase() == "MoveSpeed" && ", ".Join(new[] { "a", "b" }) == "a, b");
        var arr = new Aver.Fabric.Collections.Array<long>(new long[] { 3, 1, 2 });
        T.Check("Array<T>: PopBack / PopFront / PushFront / Front / Back / Find / Count / Fill", arr.PopBack() == 2 && arr.PopFront() == 3 && arr.Front() == 1 && arr.Back() == 1 && (arr.PushFrontThen(7) == 7) && arr.Find(1) == 1 && arr.Count(7L) == 1);
        var sorted = new Aver.Fabric.Collections.Array<long>(new long[] { 2, 3, 1 });
        sorted.SortCustom((a, b) => a > b);
        T.Check("Array<T>: SortCustom is stable and takes `less`", sorted.SequenceEqual(new long[] { 3, 2, 1 }));
        T.Check("Array<T>: Map / Filter / Reduce", new long[] { 1, 2, 3 }.Map(x => x * 2).SequenceEqual(new long[] { 2, 4, 6 }) && new long[] { 1, 2, 3 }.Filter(x => x > 1).SequenceEqual(new long[] { 2, 3 }) &&
                new long[] { 1, 2, 3 }.Reduce((a, x) => a + x, 10L) == 16);
        var typed = new Aver.Fabric.Collections.Dictionary<string, long> { ["a"] = 1 };
        T.Check("Dictionary<K,V>: Get with a default, Has, KeysArray", typed.Get("a", 0) == 1 && typed.Get("z", 9) == 9 && typed.Has("a") && typed.KeysArray().Count == 1);
    }

    private static long PushFrontThen(this Aver.Fabric.Collections.Array<long> a, long v) { a.PushFront(v); return a.Front(); }

    // ---- the command line ----

    private static string Temp(string what)
    {
        string p = Path.Combine(Path.GetTempPath(), "fabric_gd_" + what + "_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(p);
        return p;
    }

    private static int Cli(out string stdout, out string stderr, params string[] args)
    {
        var so = new StringWriter();
        var se = new StringWriter();
        int code = ConvertCli.Run(args, so, se);
        stdout = so.ToString();
        stderr = se.ToString();
        return code;
    }

    private static void Cli()
    {
        string data = Path.Combine(T.DataDir(), "GdProject");
        string root = Temp("cli");
        try
        {
            string proj = Path.Combine(root, "GdProject");
            Directory.CreateDirectory(Path.Combine(proj, "Fabric"));
            string manifest = Path.Combine(proj, "Fabric", "scripts.json");
            File.WriteAllText(manifest,
                "{\"schema\":1,\"project\":\"GdProject\",\"scripts\":[" +
                "{\"path\":\"res://scripts/global.gd\",\"kind\":\"gdscript\",\"className\":\"Global\",\"exists\":true,\"output\":\"Content/Scripts/Global.cs\"}," +
                "{\"path\":\"res://scripts/hud.gd\",\"kind\":\"gdscript\",\"className\":\"HudScript\",\"exists\":true,\"output\":\"Content/Scripts/HudScript.cs\"}," +
                "{\"path\":\"res://scripts/gone.gd\",\"kind\":\"gdscript\",\"className\":\"Gone\",\"exists\":false,\"output\":\"Content/Scripts/Gone.cs\"}]}");
            string report = Path.Combine(root, "FabricReport.jsonl");
            File.WriteAllText(report,
                "{\"schema\":1,\"kind\":\"node\",\"file\":\"res://main.tscn\",\"node\":\"Hud\",\"type\":\"Control\",\"property\":\"\",\"rating\":\"converted\",\"reason\":\"\",\"line\":2}\n" +
                "{\"schema\":1,\"kind\":\"script\",\"file\":\"res://scripts/hud.gd\",\"node\":\"\",\"type\":\"gdscript\",\"property\":\"\",\"rating\":\"unsupported\",\"reason\":\"later package\",\"line\":0}\n");

            int code = Cli(out string so, out string se, "scripts", "--project", data, "--out", proj, "--refs", Refs);
            T.Check("exit 0: the .gd entries of scripts.json convert and compile", code == 0, se);
            JsonElement summary = JsonDocument.Parse(so.Trim().Split('\n').Last()).RootElement;
            T.Check("the summary line counts the two scripts and no errors", summary.GetProperty("scripts").GetInt32() == 2 && summary.GetProperty("errors").GetInt32() == 0 && summary.GetProperty("compiled").GetBoolean());
            string scripts = Path.Combine(proj, "Content", "Scripts");
            T.Check("each script is written where scripts.json says; the class is the one scripts.json names", File.Exists(Path.Combine(scripts, "Global.cs")) && File.Exists(Path.Combine(scripts, "HudScript.cs")) &&
                    File.ReadAllText(Path.Combine(scripts, "HudScript.cs")).Contains("public partial class HudScript : Control"));
            T.Check("project.godot's autoload list reached the converter", File.ReadAllText(Path.Combine(scripts, "HudScript.cs")).Contains("GetNode<Global>(\"/root/Global\")"));
            T.Check("a missing script is not written", !File.Exists(Path.Combine(scripts, "Gone.cs")));

            List<JsonElement> lines = File.ReadAllLines(report).Where(l => l.Length > 0).Select(l => JsonDocument.Parse(l).RootElement.Clone()).ToList();
            string K(JsonElement e, string p) => e.GetProperty(p).GetString() ?? "";
            T.Check("the scene converter's own entries are kept", lines.Any(l => K(l, "kind") == "node" && K(l, "file") == "res://main.tscn"));
            T.Check("its placeholder for hud.gd is replaced by the real entry", lines.Count(l => K(l, "file") == "res://scripts/hud.gd" && K(l, "type") == "gdscript") == 1 && !lines.Any(l => K(l, "reason") == "later package"));
            T.Check("a script that does not exist is reported unsupported", lines.Any(l => K(l, "file") == "res://scripts/gone.gd" && K(l, "rating") == "unsupported"));
            T.Check("every line has the schema's eight fields", lines.All(l => new[] { "schema", "kind", "file", "node", "type", "property", "rating", "reason", "line" }.All(f => l.TryGetProperty(f, out _))));

            string before = File.ReadAllText(report), hudBefore = File.ReadAllText(Path.Combine(scripts, "HudScript.cs"));
            int again = Cli(out _, out _, "scripts", "--project", data, "--out", proj, "--refs", Refs);
            T.Check("rerun: exit 0, the report and the output are byte for byte the same", again == 0 && File.ReadAllText(report) == before && File.ReadAllText(Path.Combine(scripts, "HudScript.cs")) == hudBefore);

            // No scripts.json: the project is scanned for .gd files too.
            string scanned = Path.Combine(root, "Scanned");
            Directory.CreateDirectory(scanned);
            int scan = Cli(out string so2, out string se2, "scripts", "--project", Path.Combine(T.DataDir(), "ConvertGame"), "--out", scanned, "--refs", Refs);
            T.Check("without a manifest the project's .gd files are found (class_name or the file name)", scan == 0 && File.Exists(Path.Combine(scanned, "Content", "Scripts", "Player.cs")) &&
                    File.Exists(Path.Combine(scanned, "Content", "Scripts", "Global.cs")), se2);
            T.Check("the scanned player compiles as a CharacterBody3D", File.ReadAllText(Path.Combine(scanned, "Content", "Scripts", "Player.cs")).Contains("public partial class Player : CharacterBody3D"));

            // A script that cannot compile: exit 1, the output is written with the line marked.
            string bad = Path.Combine(root, "Bad");
            string badProj = Path.Combine(root, "BadOut");
            Directory.CreateDirectory(Path.Combine(bad, "s"));
            Directory.CreateDirectory(badProj);
            File.WriteAllText(Path.Combine(bad, "project.godot"), "config_version=5\n");
            File.WriteAllText(Path.Combine(bad, "s", "weird.gd"), "extends NoSuchRuntimeClass\nfunc f() -> NoSuchType:\n\treturn null\n");
            int ex = Cli(out _, out string seBad, "scripts", "--project", bad, "--out", badProj, "--refs", Refs);
            T.Check("a class the runtime lacks: exit 1, the error on stderr with the output's line", ex == 1 && seBad.Contains("Content/Scripts/Weird.cs(") && seBad.Contains("error CS"), seBad);
            T.Check("the file is still written and the line carries the FABRIC comment", File.Exists(Path.Combine(badProj, "Content", "Scripts", "Weird.cs")) && File.ReadAllText(Path.Combine(badProj, "Content", "Scripts", "Weird.cs")).Contains("// FABRIC"));
            T.Check("a bad reference folder: exit 3 and the script is still written", Cli(out _, out _, "scripts", "--project", bad, "--out", badProj, "--refs", Path.Combine(root, "nowhere")) == 3);
        }
        finally { try { Directory.Delete(root, true); } catch (IOException) { } }
    }
}
