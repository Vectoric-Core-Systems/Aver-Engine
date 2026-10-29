// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// EnvShaderTests: sky, fog and particles shaders, GPUParticles3D and VisualShader (G6 SH3). The converter's own output for the EnvShaderGame fixture is loaded through
// ResourceLoader and instantiated; the engine side is a set of recording backends (nothing here touches the native registry), so the checks are what the nodes
// hand the engine and when.

using Aver.Fabric;
using Aver.Framework;

static class EnvShaderTests
{
    public static int RunAll() => T.Suite(() =>
    {
        T.Run("env shaders: the converter's output for the EnvShaderGame project loads and registers with the engine (G6 SH3)", ConverterOutput);
        T.Run("env shaders: VisualShader.GetCode reads the text the converter generated (G6 SH3)", VisualShaderCode);
        T.Run("env shaders: a ShaderMaterial of an unknown kind or a missing graph is not an environment shader (G6 SH3)", Refusals);
    });

    private sealed class FakeEnv : IEnvShaderBackend
    {
        public readonly List<string> Calls = new();
        public readonly Dictionary<string, float[]> Registers = new();
        public string? Sky;
        public bool Register(GodotEnvKind kind, string name, string graphText) { Calls.Add($"register {kind} {name}"); return graphText.Contains("DOMAIN godotenv"); }
        public bool SetRegisters(GodotEnvKind kind, string name, float[] registers) { Calls.Add($"regs {kind} {name} x{registers.Length}"); Registers[kind + ":" + name] = (float[])registers.Clone(); return true; }
        public bool UseSky(string? name) { Calls.Add("sky " + (name ?? "<engine>")); Sky = name; return true; }
    }

    private sealed class FakeParams : IParticleParamBackend
    {
        public readonly Dictionary<int, float[]> Regs = new();
        public int Calls;
        public bool SetRegister(Entity entity, int register, float x, float y, float z, float w) { Calls++; Regs[register] = new[] { x, y, z, w }; return true; }
    }

    private sealed class FakeFog : IEnvFxBackend
    {
        public readonly Dictionary<int, string?> Shaders = new();
        public int CreateFogVolume(string key) => 1;
        public bool DestroyFogVolume(int id) => true;
        public bool SetFogVolume(int id, FogVolumeShapeKind shape, float[] w, float[] h, float d, float[] a, float[] e, float f, float edge) => true;
        public bool SetFogVolumeEnabled(int id, bool on) => true;
        public bool SetFogVolumeShader(int id, string? name) { Shaders[id] = name; return true; }
        public int CreateProbe(string key) => 0;
        public bool DestroyProbe(int id) => true;
        public bool SetProbeBox(int id, float[] w, float[] h, float[] o) => true;
        public bool SetProbeLook(int id, float i, float b, float m, bool bp, bool interior, bool always, int res) => true;
        public bool SetProbeAmbient(int id, int mode, float[] c, float energy) => true;
        public bool SetProbeMasks(int id, uint cull, uint reflection) => true;
        public bool SetProbeEnabled(int id, bool on) => true;
        public ProbeBlend SelectProbes(float x, float y, float z, float cx, float cy, float cz) => new() { Ids = Array.Empty<int>(), Weights = Array.Empty<float>(), Rest = 1f };
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
        string output = Path.Combine(Path.GetTempPath(), "fabric_envshader_" + Guid.NewGuid().ToString("N"));
        var env = new FakeEnv();
        var parts = new FakeParams();
        var fog = new FakeFog();
        IEnvShaderBackend savedEnv = EnvShaderBackend.Current;
        IParticleParamBackend savedParts = ParticleParamBackend.Current;
        EnvShaderBackend.Current = env;
        ParticleParamBackend.Current = parts;
        EnvFxBackend.Override = fog;
        string? savedRoot = ResourceLoader.ContentRootOverride;
        try
        {
            var psi = new System.Diagnostics.ProcessStartInfo(exe) { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false };
            psi.ArgumentList.Add("fabric"); psi.ArgumentList.Add(Path.Combine(T.DataDir(), "EnvShaderGame")); psi.ArgumentList.Add(output); psi.ArgumentList.Add("--scenes-only");
            using (var proc = System.Diagnostics.Process.Start(psi)!)
            {
                proc.StandardOutput.ReadToEnd(); proc.StandardError.ReadToEnd();
                proc.WaitForExit();
                T.Check("the converter ran", proc.ExitCode == 0, "exit " + proc.ExitCode);
            }
            ResourceLoader.ContentRootOverride = Path.Combine(output, "EnvShaderGame", "Content");
            ShaderMaterial.ResetLayouts();
            PackedScene scene = ResourceLoader.Load<PackedScene>("res://main.tscn")!;
            T.Check("the level can be instantiated", scene.CanInstantiate());
            T.Logs.Clear();
            Node main = scene.Instantiate();
            SceneTree.Instance.Root.AddChild(main);
            T.Check("no error was logged while it entered the tree", T.ErrorCount() == 0, string.Join(" | ", T.Logs.Where(l => l.Level == Aver.Scripting.Log.Level.Error).Select(l => l.Message)));

            // ---- sky ----
            var we = main.GetNode<WorldEnvironment>("Env");
            var skyMat = we.Environment?.Sky?.SkyMaterial as ShaderMaterial;
            T.Check("the Sky's material is a ShaderMaterial over a sky shader", skyMat is { EnvironmentKind: "sky" } && skyMat.Shader?.GetMode() == Shader.Mode.Sky);
            T.Check("the sky shader was registered and made the sky", env.Calls.Any(c => c.StartsWith("register Sky ", StringComparison.Ordinal)) && env.Sky is { Length: > 0 });
            T.Check("its register table starts at the material's values (sun_size 0.05, show_stars false)",
                skyMat != null && T.Near(skyMat.GetShaderParameter("sun_size").AsSingle(), 0.05f) && skyMat.GetShaderParameter("star_density").AsInt32() == 5 && !skyMat.GetShaderParameter("show_stars").AsBool());
            skyMat!.SetShaderParameter("sun_size", Variant.CreateFrom(0.1f));
            T.Check("set_shader_parameter reaches the engine's register table", env.Registers.TryGetValue("Sky:" + skyMat.EnvName, out float[]? sr) && T.Near(sr[7], 0.1f));

            // ---- fog ----
            var mist = main.GetNode<FogVolume>("Mist");
            T.Check("the FogVolume's material is a ShaderMaterial over a fog shader", mist.Material is ShaderMaterial { EnvironmentKind: "fog" });
            T.Check("the fog shader was registered and the volume names it", env.Calls.Any(c => c.StartsWith("register Fog ", StringComparison.Ordinal)) &&
                    fog.Shaders.TryGetValue(mist.VolumeId, out string? fname) && fname is { Length: > 0 } && fname == ((ShaderMaterial)mist.Material!).EnvName);
            ((ShaderMaterial)mist.Material!).SetShaderParameter("density", Variant.CreateFrom(4f));
            T.Check("a fog parameter reaches the engine", env.Registers.TryGetValue("Fog:" + ((ShaderMaterial)mist.Material!).EnvName, out float[]? fr) && T.Near(fr[0], 4f));

            // ---- particles ----
            var sparks = main.GetNode<GPUParticles3D>("Sparks");
            T.Check("the GPUParticles3D keeps Godot's properties", sparks.Amount == 64 && T.Near((float)sparks.Lifetime, 2f));
            T.Check("its process material is a particles ShaderMaterial", sparks.ProcessMaterial is ShaderMaterial { EnvironmentKind: "particles" });
            T.Check("the particles shader was registered", env.Calls.Any(c => c.StartsWith("register Particles ", StringComparison.Ordinal)));
            T.Check("its two registers became the emitter's user parameters reg0 and reg1", parts.Regs.ContainsKey(0) && parts.Regs.ContainsKey(1) && T.Near(parts.Regs[1][1], 2f) && T.Near(parts.Regs[1][3], 4f));
            int before = parts.Calls;
            ((ShaderMaterial)sparks.ProcessMaterial!).SetShaderParameter("spin", Variant.CreateFrom(8f));
            T.Check("set_shader_parameter reaches the running emitter's user parameters", parts.Calls > before && T.Near(parts.Regs[1][3], 8f));
            var plain = main.GetNode<GPUParticles3D>("Plain");
            T.Check("a ParticleProcessMaterial is kept as a value and drives nothing", plain.ProcessMaterial is null);
            main.QueueFree();
        }
        finally
        {
            EnvShaderBackend.Current = savedEnv;
            ParticleParamBackend.Current = savedParts;
            EnvFxBackend.Override = null;
            ResourceLoader.ContentRootOverride = savedRoot;
            ShaderMaterial.ResetLayouts();
            try { if (Directory.Exists(output)) Directory.Delete(output, true); } catch (IOException) { }
        }
    }

    private static void VisualShaderCode()
    {
        string root = Path.Combine(Path.GetTempPath(), "fabric_vs_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Path.Combine(root, "Content"));
        Directory.CreateDirectory(Path.Combine(root, "Fabric", "visualshaders"));
        File.WriteAllText(Path.Combine(root, "Fabric", "visualshaders", "rim.gdshader.txt"), "shader_type spatial;\nuniform float rim_power = 3.0;\n");
        string? saved = ResourceLoader.ContentRootOverride;
        try
        {
            ResourceLoader.ContentRootOverride = Path.Combine(root, "Content");
            var vs = new VisualShader { SourcePath = "res://visual/rim.tres" };
            T.Check("GetCode returns the generated shading-language text", vs.GetCode().Contains("uniform float rim_power"));
            T.Check("a VisualShader is a Shader", vs is Shader);
            T.Check("with no converted text GetCode is empty", new VisualShader { SourcePath = "res://visual/none.tres" }.GetCode().Length == 0);
        }
        finally
        {
            ResourceLoader.ContentRootOverride = saved;
            try { Directory.Delete(root, true); } catch (IOException) { }
        }
    }

    private static void Refusals()
    {
        string dir = Path.Combine(Path.GetTempPath(), "fabric_envref_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Path.Combine(dir, "Shaders"));
        string file = Path.Combine(dir, "Shaders", "x.sky.ocgraph");
        File.WriteAllText(file, "OCGRAPH 1\nDOMAIN material\n");
        try
        {
            T.Check("a graph that is not a godotenv graph loads as nothing", ShaderMaterial.TryLoadEnv(file, "Shaders/x.sky.ocgraph") is null);
            File.WriteAllText(file, "OCGRAPH 1\nDOMAIN godotenv\nNODE gd GodotEnvShader kind=bogus name=x ns=ge1_ regs=0 gregs=0 start=0 process=0 src0=a\n");
            T.Check("an unknown kind loads as nothing", ShaderMaterial.TryLoadEnv(file, "Shaders/x.sky.ocgraph") is null);
            File.WriteAllText(file, "OCGRAPH 1\nDOMAIN godotenv\nNODE gd GodotEnvShader kind=sky name=x ns=ge1_ regs=1 gregs=0 start=0 process=0 reg=3F800000,00000000,00000000,3F800000 src0=a\n");
            ShaderMaterial? m = ShaderMaterial.TryLoadEnv(file, "Shaders/x.sky.ocgraph");
            T.Check("a sky graph loads as a ShaderMaterial of kind sky with its default registers", m is { EnvironmentKind: "sky" } && m.EnvRegisters.Length == 4 && T.Near(m.EnvRegisters[0], 1f) && T.Near(m.EnvRegisters[3], 1f));
            T.Check("a plain ShaderMaterial is not an environment shader", new ShaderMaterial().EnvironmentKind.Length == 0 && !new ShaderMaterial().RegisterWithEngine());
        }
        finally
        {
            try { Directory.Delete(dir, true); } catch (IOException) { }
        }
    }
}
