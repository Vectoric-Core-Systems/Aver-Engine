// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// ShaderTests: Shader, ShaderMaterial and RenderingServer.global_shader_parameter_* (G6 SH2). set_shader_parameter packs into the uniform table the way the converter's
// emitter reads it and hands the table to the engine; against a recording backend (nothing here touches the native material library), then the converter's own output
// for the ShaderGame fixture loaded back through ResourceLoader.

using Aver.Fabric;
using Collections = Aver.Fabric.Collections;

static class ShaderTests
{
    public static int RunAll() => T.Suite(() =>
    {
        T.Run("shader: the layout file the converter writes is read (G6 SH2)", LayoutParses);
        T.Run("shader: parameters are packed into the uniform table in the emitter's layout (G6 SH2)", Packing);
        T.Run("shader: a ShaderMaterial pushes its table and its textures to the engine (G6 SH2)", Pushes);
        T.Run("shader: global shader parameters reach every live material that declares them (G6 SH2)", Globals);
        T.Run("shader: the converter's own output for the ShaderGame project loads as ShaderMaterials (G6 SH2)", ConverterOutput);
    });

    private sealed class Fake : IShaderBackend
    {
        public readonly List<string> Calls = new();
        public float[] Table = Array.Empty<float>();
        public readonly Dictionary<int, string> Textures = new();
        public readonly Dictionary<string, int> Known = new();
        public int FindMaterial(string name) { Calls.Add("find " + name); return Known.TryGetValue(name, out int h) ? h : 0; }
        public bool SetRegisters(int material, float[] values) { Calls.Add($"regs {material} x{values.Length}"); Table = (float[])values.Clone(); return true; }
        public bool SetTexturePath(int material, int slot, string path) { Calls.Add($"tex {material} {slot} {path}"); Textures[slot] = path; return true; }
        public bool ClearTexture(int material, int slot) { Calls.Add($"clear {material} {slot}"); Textures.Remove(slot); return true; }
    }

    private static Fake Install()
    {
        var f = new Fake();
        ShaderBackend.Current = f;
        ShaderMaterial.ResetLayouts();
        RenderingServer.ResetGlobals();
        return f;
    }

    // The water fixture's layout as the converter wrote it: shallow vec4 (reg 0, source_color), deep vec3 (reg 1), amplitude float (1.w), speed (2.x), octaves int (2.y),
    // foam bool (2.z), flow vec2 (3.xy), the global wind_strength (reg 4); foam_tex in slot 0 and normal_tex in slot 1.
    private const string Json = @"{ ""schema"": 1, ""shaders"": [ { ""source"": ""res://shaders/water.gdshader"", ""graph"": ""Shaders/water.ocgraph"", ""registers"": 5, ""uniforms"": [
        {""name"": ""shallow"", ""type"": ""vec4"", ""array"": 0, ""scope"": ""normal"", ""sourceColor"": true, ""reg"": 0, ""comp"": 0, ""registers"": 1, ""stride"": 1},
        {""name"": ""deep"", ""type"": ""vec3"", ""array"": 0, ""scope"": ""normal"", ""sourceColor"": true, ""reg"": 1, ""comp"": 0, ""registers"": 1, ""stride"": 1},
        {""name"": ""amplitude"", ""type"": ""float"", ""array"": 0, ""scope"": ""normal"", ""sourceColor"": false, ""reg"": 1, ""comp"": 3, ""registers"": 1, ""stride"": 1},
        {""name"": ""speed"", ""type"": ""float"", ""array"": 0, ""scope"": ""normal"", ""sourceColor"": false, ""reg"": 2, ""comp"": 0, ""registers"": 1, ""stride"": 1},
        {""name"": ""octaves"", ""type"": ""int"", ""array"": 0, ""scope"": ""normal"", ""sourceColor"": false, ""reg"": 2, ""comp"": 1, ""registers"": 1, ""stride"": 1},
        {""name"": ""foam"", ""type"": ""bool"", ""array"": 0, ""scope"": ""normal"", ""sourceColor"": false, ""reg"": 2, ""comp"": 2, ""registers"": 1, ""stride"": 1},
        {""name"": ""flow"", ""type"": ""vec2"", ""array"": 0, ""scope"": ""normal"", ""sourceColor"": false, ""reg"": 3, ""comp"": 0, ""registers"": 1, ""stride"": 1},
        {""name"": ""wind_strength"", ""type"": ""float"", ""array"": 0, ""scope"": ""global"", ""sourceColor"": false, ""reg"": 4, ""comp"": 0, ""registers"": 1, ""stride"": 1} ],
        ""samplers"": [ {""name"": ""foam_tex"", ""slot"": 0, ""bindable"": true}, {""name"": ""normal_tex"", ""slot"": 1, ""bindable"": true}, {""name"": ""screen_tex"", ""slot"": 5, ""bindable"": false} ],
        ""materials"": [ ""Materials/Test_ocean.ocmat"" ], ""globalTypes"": {""wind_strength"": ""float""} } ] }";

    private static ShaderMaterial Make(ShaderLayout layout, string engineName, float[]? table = null)
    {
        var m = new ShaderMaterial { Shader = new Shader { Layout = layout } };
        typeof(ShaderMaterial).GetField("_engineName", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance)!.SetValue(m, engineName);
        typeof(ShaderMaterial).GetField("_regs", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance)!.SetValue(m, table ?? new float[layout.Registers * 4]);
        return m;
    }

    private static void LayoutParses()
    {
        var layouts = ShaderLayout.Parse(Json);
        T.Check("one shader, keyed by its graph", layouts.Count == 1 && layouts.ContainsKey("Shaders/water.ocgraph"));
        ShaderLayout l = layouts["Shaders/water.ocgraph"];
        T.Check("eight uniforms and three samplers", l.Uniforms.Count == 8 && l.Samplers.Count == 3 && l.Registers == 5);
        T.Check("a uniform's place", l.Uniform("octaves") is { Type: "int", Reg: 2, Comp: 1 } && l.Uniform("wind_strength") is { Scope: "global", Reg: 4 } && l.Uniform("shallow") is { SourceColor: true });
        T.Check("a sampler's slot", l.Sampler("normal_tex")?.Slot == 1 && l.Sampler("screen_tex") is { Bindable: false });
        var shader = new Shader { Layout = l };
        T.Check("the shader lists its uniforms and samplers", shader.GetUniformNames().Length == 8 && shader.GetSamplerNames().Contains("foam_tex") && shader.GetMode() == Shader.Mode.Spatial);
    }

    private static void Packing()
    {
        Fake f = Install();
        ShaderLayout l = ShaderLayout.Parse(Json)["Shaders/water.ocgraph"];
        var regs = new float[20];
        ShaderPacking.Pack(l.Uniform("shallow")!, Variant.CreateFrom(new Color(0.2f, 0.4f, 0.8f, 0.5f)), regs);
        T.Check("a source_color vec4 is decoded to linear, alpha as written",
            T.Near(regs[0], ShaderPacking.SrgbToLinear(0.2f)) && T.Near(regs[1], ShaderPacking.SrgbToLinear(0.4f)) && T.Near(regs[2], ShaderPacking.SrgbToLinear(0.8f)) && T.Near(regs[3], 0.5f));
        ShaderPacking.Pack(l.Uniform("deep")!, Variant.CreateFrom(new Vector3(1, 2, 3)), regs);
        ShaderPacking.Pack(l.Uniform("amplitude")!, Variant.CreateFrom(0.25f), regs);
        T.Check("a vec3 takes a register and a float follows it in the fourth component", T.Near(regs[4], ShaderPacking.SrgbToLinear(1f)) && T.Near(regs[7], 0.25f));
        ShaderPacking.Pack(l.Uniform("octaves")!, Variant.CreateFrom(3), regs);
        ShaderPacking.Pack(l.Uniform("foam")!, Variant.CreateFrom(true), regs);
        T.Check("an int and a bool are bit patterns in the float slots", BitConverter.SingleToInt32Bits(regs[9]) == 3 && BitConverter.SingleToInt32Bits(regs[10]) == 1);
        ShaderPacking.Pack(l.Uniform("flow")!, Variant.CreateFrom(new Vector2(0.5f, -2f)), regs);
        T.Check("a vec2 starts a register", T.Near(regs[12], 0.5f) && T.Near(regs[13], -2f));
        T.Check("a wrong type is refused", !ShaderPacking.Pack(l.Uniform("flow")!, Variant.CreateFrom("text"), regs) && !ShaderPacking.Pack(l.Uniform("speed")!, Variant.CreateFrom(new Vector3(1, 2, 3)), regs));
        // matrices and arrays
        var mat = new ShaderUniformInfo { Name = "m", Type = "mat3", Reg = 0, Comp = 0, Registers = 3, Stride = 3 };
        var mr = new float[12];
        ShaderPacking.Pack(mat, Variant.CreateFrom(new Basis(new Vector3(1, 2, 3), new Vector3(4, 5, 6), new Vector3(7, 8, 9))), mr);
        T.Check("a Basis is a mat3 of its columns, one per register", T.Near(mr[0], 1) && T.Near(mr[1], 2) && T.Near(mr[2], 3) && T.Near(mr[4], 4) && T.Near(mr[8], 7));
        var arr = new ShaderUniformInfo { Name = "w", Type = "float", Array = 3, Reg = 0, Comp = 0, Registers = 3, Stride = 1 };
        var ar = new float[12];
        var values = new Collections.Array { Variant.CreateFrom(0.1f), Variant.CreateFrom(0.2f), Variant.CreateFrom(0.3f) };
        ShaderPacking.Pack(arr, Variant.CreateFrom(values), ar);
        T.Check("a float[3] takes one register an element", T.Near(ar[0], 0.1f) && T.Near(ar[4], 0.2f) && T.Near(ar[8], 0.3f));
        _ = f;
    }

    private static void Pushes()
    {
        Fake f = Install();
        f.Known["Test_ocean"] = 7;
        ShaderLayout l = ShaderLayout.Parse(Json)["Shaders/water.ocgraph"];
        ShaderMaterial m = Make(l, "Test_ocean");
        m.SetShaderParameter("amplitude", Variant.CreateFrom(1.5f));
        T.Check("the first parameter finds the engine material and writes the whole table once", f.Calls.Contains("find Test_ocean") && f.Calls.Count(c => c.StartsWith("regs 7")) == 1 && f.Table.Length == 20);
        T.Check("the value is in its register", T.Near(f.Table[7], 1.5f));
        T.Check("get_shader_parameter returns what was set", T.Near(m.GetShaderParameter("amplitude").AsSingle(), 1.5f));
        m.SetShaderParameter("octaves", Variant.CreateFrom(4));
        T.Check("an int reads back as an int", m.GetShaderParameter("octaves").AsInt32() == 4 && BitConverter.SingleToInt32Bits(f.Table[9]) == 4);
        T.Check("a parameter the shader does not declare changes nothing", Count(f, "regs") == 2 && DoesNothing(m, f, "no_such_thing"));
        // textures go to the slot the sampler was given
        var tex = new Texture2D { ResourcePath = "res://textures/foam.png" };
        m.SetShaderParameter("foam_tex", Variant.CreateFrom(tex));
        T.Check("a texture parameter is bound in its sampler's slot", f.Textures.TryGetValue(0, out string? p) && p == "Textures/textures/foam.png");
        m.SetShaderParameter("foam_tex", default);
        T.Check("a nil texture clears the slot", !f.Textures.ContainsKey(0));
        int before = f.Calls.Count;
        m.SetShaderParameter("screen_tex", Variant.CreateFrom(tex));
        T.Check("a screen texture sampler is not bindable and is ignored", f.Calls.Count == before);
        // a material the engine has not built yet is found later
        ShaderMaterial late = Make(l, "Late");
        late.SetShaderParameter("speed", Variant.CreateFrom(2f));
        T.Check("an engine material that is not loaded yet pushes nothing", !f.Calls.Any(c => c.StartsWith("regs 0")));
        f.Known["Late"] = 9;
        late.RefreshEngine();
        T.Check("RefreshEngine finds it and writes the table", f.Calls.Contains("regs 9 x20") && T.Near(f.Table[8], 2f));
    }

    private static int Count(Fake f, string prefix) => f.Calls.Count(c => c.StartsWith(prefix));
    private static bool DoesNothing(ShaderMaterial m, Fake f, string name)
    {
        int n = f.Calls.Count;
        m.SetShaderParameter(name, Variant.CreateFrom(1f));
        return f.Calls.Count == n;
    }

    private static void Globals()
    {
        Fake f = Install();
        f.Known["A"] = 1; f.Known["B"] = 2;
        ShaderLayout l = ShaderLayout.Parse(Json)["Shaders/water.ocgraph"];
        ShaderMaterial a = Make(l, "A"), b = Make(l, "B");
        // live materials are registered by TryLoad; register these two the way it does
        var live = (List<WeakReference<ShaderMaterial>>)typeof(ShaderMaterial).GetField("s_live", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static)!.GetValue(null)!;
        live.Add(new WeakReference<ShaderMaterial>(a)); live.Add(new WeakReference<ShaderMaterial>(b));
        RenderingServer.GlobalShaderParameterAdd("wind_strength", RenderingServer.GlobalShaderParameterType.Float, Variant.CreateFrom(0.5f));
        T.Check("adding a global writes its default into every live material that declares it", Count(f, "regs 1") == 1 && Count(f, "regs 2") == 1 && T.Near(f.Table[16], 0.5f));
        RenderingServer.GlobalShaderParameterSet("wind_strength", Variant.CreateFrom(3f));
        T.Check("setting it rewrites the register in both", Count(f, "regs 1") == 2 && Count(f, "regs 2") == 2 && T.Near(f.Table[16], 3f));
        T.Check("get returns the value and the type", T.Near(RenderingServer.GlobalShaderParameterGet("wind_strength").AsSingle(), 3f) &&
            RenderingServer.GlobalShaderParameterGetType("wind_strength") == RenderingServer.GlobalShaderParameterType.Float);
        int before = f.Calls.Count;
        RenderingServer.GlobalShaderParameterSet("someone_elses", Variant.CreateFrom(1f));
        T.Check("a global no material declares writes nothing", f.Calls.Count == before);
        RenderingServer.GlobalShaderParameterRemove("wind_strength");
        T.Check("remove forgets it", RenderingServer.GlobalShaderParameterGet("wind_strength").IsNil);
        ShaderMaterial.ResetLayouts();
    }

    private static void ConverterOutput()
    {
        string? exe = null;
        for (DirectoryInfo? d = new DirectoryInfo(AppContext.BaseDirectory); d != null && exe == null; d = d.Parent)
        {
            string p = Path.Combine(d.FullName, "build", "bin", OperatingSystem.IsWindows() ? "AverAssetC.exe" : "AverAssetC");
            if (File.Exists(p)) exe = p;
        }
        if (exe == null) { Console.WriteLine("  SKIP: AverAssetC is not built (build/bin/AverAssetC)"); return; }
        string output = Path.Combine(Path.GetTempPath(), "fabric_shader_" + Guid.NewGuid().ToString("N"));
        Fake f = Install();
        string? savedRoot = ResourceLoader.ContentRootOverride;
        try
        {
            var psi = new System.Diagnostics.ProcessStartInfo(exe) { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false };
            psi.ArgumentList.Add("fabric"); psi.ArgumentList.Add(Path.Combine(T.DataDir(), "ShaderGame")); psi.ArgumentList.Add(output); psi.ArgumentList.Add("--scenes-only");
            using (var proc = System.Diagnostics.Process.Start(psi)!)
            {
                proc.StandardOutput.ReadToEnd(); proc.StandardError.ReadToEnd();
                proc.WaitForExit();
                T.Check("the converter ran", proc.ExitCode == 0, "exit " + proc.ExitCode);
            }
            ResourceLoader.ContentRootOverride = Path.Combine(output, "ShaderGame", "Content");
            ShaderMaterial.ResetLayouts();
            PackedScene scene = ResourceLoader.Load<PackedScene>("res://main.tscn")!;
            T.Check("the level can be instantiated", scene.CanInstantiate());
            T.Logs.Clear();
            Node main = scene.Instantiate();
            SceneTree.Instance.Root.AddChild(main);
            T.Check("no error was logged while it entered the tree", T.ErrorCount() == 0, string.Join(" | ", T.Logs.Where(l => l.Level == Aver.Scripting.Log.Level.Error).Select(l => l.Message)));

            var ocean = main.GetNode<MeshInstance3D>("Ocean");
            T.Check("the .tres material is a ShaderMaterial with its shader", ocean.MaterialOverride is ShaderMaterial { Shader: { Layout: not null } });
            var mat = (ShaderMaterial)ocean.MaterialOverride!;
            T.Check("it names the converted shader and the engine material", mat.Shader!.SourcePath == "res://shaders/water.gdshader" && mat.EngineMaterialName == "ShaderGame_ocean");
            float[] table = mat.GetUniformTable();
            T.Check("its table starts at the .ocmat's values (amplitude 0.5 in register 1.w, the global default 0.75 in register 4)", table.Length == 20 && T.Near(table[7], 0.5f) && T.Near(table[16], 0.75f));
            T.Check("get_shader_parameter reads the table before anything was set", T.Near(mat.GetShaderParameter("amplitude").AsSingle(), 0.5f) && mat.GetShaderParameter("octaves").AsInt32() == 3);
            f.Known["ShaderGame_ocean"] = 5;
            mat.SetShaderParameter("amplitude", Variant.CreateFrom(2f));
            T.Check("set_shader_parameter writes the table to the engine material", f.Calls.Contains("find ShaderGame_ocean") && f.Table.Length == 20 && T.Near(f.Table[7], 2f));
            RenderingServer.GlobalShaderParameterSet("wind_strength", Variant.CreateFrom(1.25f));
            T.Check("a global parameter reaches it", T.Near(f.Table[16], 1.25f));
            var water = main.GetNode<MeshInstance3D>("Water");
            T.Check("the sub-resource material shares the shader", water.MaterialOverride is null || water.GetSurfaceOverrideMaterial(0) is ShaderMaterial { Shader: { Layout: not null } });
            var dissolve = (ShaderMaterial)main.GetNode<MeshInstance3D>("Dissolve").MaterialOverride!;
            T.Check("the dissolve material's threshold reads 0.35", T.Near(dissolve.GetShaderParameter("threshold").AsSingle(), 0.35f));
            var broken = main.GetNode<MeshInstance3D>("Broken");
            T.Check("a shader that did not convert leaves no material", broken.MaterialOverride is null);
            main.QueueFree();
        }
        finally
        {
            ResourceLoader.ContentRootOverride = savedRoot;
            ShaderMaterial.ResetLayouts();
            RenderingServer.ResetGlobals();
            ShaderBackend.Current = new NativeShaderBackend();
            try { Directory.Delete(output, true); } catch (IOException) { }
        }
    }
}
