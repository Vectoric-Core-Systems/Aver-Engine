// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// EnvFxTests: FogVolume, FogMaterial, ReflectionProbe, LightmapGI, LightmapProbe and VoxelGI (G4 R8). The classes under Godot's names and defaults, what a fog
// volume and a probe publish to the engine (against a recording backend: nothing here touches the native framework), the axis and unit mapping of their boxes,
// republishing on change, on a moving parent and on visibility, and the classes that keep their properties and publish nothing.

using Aver.Fabric;
using Aver.Framework;

static class EnvFxTests
{
    public static int RunAll() => T.Suite(() =>
    {
        T.Run("env fx: the classes and Godot's defaults (G4 R8)", Classes);
        T.Run("env fx: a fog volume publishes its box, density and colours (G4 R8)", FogPublishes);
        T.Run("env fx: a fog volume follows its material, its parent and its visibility (G4 R8)", FogFollows);
        T.Run("env fx: a reflection probe publishes and answers queries (G4 R8)", Probe);
        T.Run("env fx: baked-GI nodes keep their properties and publish nothing (G4 R8)", BakedNodes);
        T.Run("env fx: no engine, nothing published (G4 R8)", NoEngine);
        T.Run("env fx: the converter's own output for the EnvVolGame project instantiates and publishes (G4 R8)", ConverterOutput);
    });

    private sealed class Fake : IEnvFxBackend
    {
        public readonly List<string> Calls = new();
        public readonly Dictionary<string, int> Ids = new();
        public FogVolumeFields? LastFog;
        public float[] LastWorld = Array.Empty<float>(), LastHalf = Array.Empty<float>(), LastOrigin = Array.Empty<float>();
        public float[] LastAlbedo = Array.Empty<float>(), LastEmission = Array.Empty<float>(), LastAmbient = Array.Empty<float>();
        public float LastDensity, LastFalloff, LastEdge, LastIntensity, LastBlend, LastMax;
        public bool LastBox, LastInterior, LastAlways;
        public FogVolumeShapeKind LastShape;
        public int NextId = 1;
        public ProbeBlend Next = new() { Ids = Array.Empty<int>(), Weights = Array.Empty<float>(), Rest = 1f };

        private int Make(string kind, string key)
        {
            Calls.Add($"create {kind} {key}");
            if (Ids.TryGetValue(kind + key, out int id)) return id;
            return Ids[kind + key] = NextId++;
        }
        public int CreateFogVolume(string key) => Make("fog", key);
        public bool DestroyFogVolume(int id) { Calls.Add($"destroy fog {id}"); return true; }
        public bool SetFogVolume(int id, FogVolumeShapeKind shape, float[] world, float[] half, float density, float[] albedo, float[] emission, float falloff, float edge)
        {
            Calls.Add($"fog {id}");
            LastShape = shape; LastWorld = world; LastHalf = half; LastDensity = density; LastAlbedo = albedo; LastEmission = emission; LastFalloff = falloff; LastEdge = edge;
            return true;
        }
        public bool SetFogVolumeEnabled(int id, bool on) { Calls.Add($"fogenabled {id} {on}"); return true; }
        public int CreateProbe(string key) => Make("probe", key);
        public bool DestroyProbe(int id) { Calls.Add($"destroy probe {id}"); return true; }
        public bool SetProbeBox(int id, float[] world, float[] half, float[] origin) { Calls.Add($"probebox {id}"); LastWorld = world; LastHalf = half; LastOrigin = origin; return true; }
        public bool SetProbeLook(int id, float intensity, float blend, float max, bool box, bool interior, bool always, int res)
        {
            Calls.Add($"probelook {id}");
            LastIntensity = intensity; LastBlend = blend; LastMax = max; LastBox = box; LastInterior = interior; LastAlways = always;
            return true;
        }
        public bool SetProbeAmbient(int id, int mode, float[] colour, float energy) { Calls.Add($"probeambient {id} {mode} {energy}"); LastAmbient = colour; return true; }
        public bool SetProbeMasks(int id, uint cull, uint reflection) { Calls.Add($"probemasks {id} {cull} {reflection}"); return true; }
        public bool SetProbeEnabled(int id, bool on) { Calls.Add($"probeenabled {id} {on}"); return true; }
        // + 0f: no negative zero in the text
        public ProbeBlend SelectProbes(float x, float y, float z, float cx, float cy, float cz) { Calls.Add($"select {x + 0f:0.##} {y + 0f:0.##} {z + 0f:0.##}"); return Next; }
        public int Count(string prefix) => Calls.Count(c => c.StartsWith(prefix, StringComparison.Ordinal));
    }

    private static Fake Install()
    {
        var f = new Fake();
        EnvFxBackend.Override = f;
        return f;
    }

    private static void Done() => EnvFxBackend.Override = null;

    private static bool Near(float a, float b, float tol = 1e-3f) => MathF.Abs(a - b) <= tol * MathF.Max(1f, MathF.Abs(b));

    private static void Classes()
    {
        foreach (string name in new[] { "FogVolume", "FogMaterial", "ReflectionProbe", "LightmapGI", "LightmapProbe", "VoxelGI", "LightmapGIData", "VoxelGIData", "Texture3D" })
            T.Check($"ClassDB knows {name}", ClassDB.ClassExists(name));
        T.Check("FogVolume, ReflectionProbe, LightmapGI and VoxelGI are VisualInstance3D; LightmapProbe a Node3D; FogMaterial a Material",
            ClassDB.IsParentClass("FogVolume", "VisualInstance3D") && ClassDB.IsParentClass("ReflectionProbe", "VisualInstance3D") &&
            ClassDB.IsParentClass("LightmapGI", "VisualInstance3D") && ClassDB.IsParentClass("VoxelGI", "VisualInstance3D") &&
            ClassDB.IsParentClass("LightmapProbe", "Node3D") && ClassDB.IsParentClass("FogMaterial", "Material"));
        var fv = new FogVolume();
        T.Check("FogVolume: Godot's defaults, a 2 m box", fv.Size == new Vector3(2, 2, 2) && fv.Shape == RenderingServer.FogVolumeShape.Box && fv.Material == null);
        T.Check("the shape numbers are Godot's and the engine's",
            (int)RenderingServer.FogVolumeShape.Ellipsoid == 0 && (int)RenderingServer.FogVolumeShape.Cone == 1 && (int)RenderingServer.FogVolumeShape.Cylinder == 2 &&
            (int)RenderingServer.FogVolumeShape.Box == 3 && (int)RenderingServer.FogVolumeShape.World == 4 &&
            (int)FogVolumeShapeKind.World == 4 && (int)FogVolumeShapeKind.Box == 3);
        var fm = new FogMaterial();
        T.Check("FogMaterial: density 1, white albedo, no emission, no falloff, edge fade 0.1",
            fm.Density == 1f && fm.Albedo == new Color(1, 1, 1, 1) && fm.Emission == new Color(0, 0, 0, 1) && fm.HeightFalloff == 0f && Near(fm.EdgeFade, 0.1f));
        var rp = new ReflectionProbe();
        T.Check("ReflectionProbe: a 20 m box, intensity 1, blend 1 m, no box projection, ambient from the environment, once",
            rp.Size == new Vector3(20, 20, 20) && rp.Intensity == 1f && rp.BlendDistance == 1f && !rp.BoxProjection && !rp.Interior &&
            rp.AmbientMode == ReflectionProbe.AmbientModeEnum.Environment && rp.UpdateMode == ReflectionProbe.UpdateModeEnum.Once && rp.MaxDistance == 0f && rp.AmbientColorEnergy == 1f);
        var lg = new LightmapGI();
        T.Check("LightmapGI: Godot's defaults (medium, 3 bounces, denoiser on) and never baked", lg.Quality == LightmapGI.BakeQualityEnum.Medium && lg.Bounces == 3 && lg.UseDenoiser && !lg.IsBaked);
        var vg = new VoxelGI();
        T.Check("VoxelGI: 128 subdivisions, a 20 m box, never baked", vg.Subdiv == VoxelGI.SubdivEnum.Subdiv128 && vg.Size == new Vector3(20, 20, 20) && !vg.IsBaked);
    }

    private static void FogPublishes()
    {
        Fake f = Install();
        try
        {
            var root = new Node { Name = "Level" };
            var mat = new FogMaterial { Density = 3f, Albedo = new Color(0.5f, 0.5f, 0.5f, 1f), Emission = new Color(1f, 0f, 0f, 1f), HeightFalloff = 0.5f, EdgeFade = 0.2f };
            // size (x, y, z) Godot: 4 wide, 2 high, 6 deep.
            var fog = new FogVolume { Name = "Mist", Size = new Vector3(4, 2, 6), Shape = RenderingServer.FogVolumeShape.Cylinder, Material = mat, Position = new Vector3(1, 2, 3) };
            root.AddChild(fog);
            T.Check("a volume not in the tree has published nothing", f.Calls.Count == 0 && fog.VolumeId == 0);
            SceneTree.Instance.Root.AddChild(root);
            T.Check("entering the tree creates the volume under its tree path", f.Calls.Count > 0 && f.Calls[0] == "create fog /root/Level/Mist" && fog.VolumeId == 1);
            T.Check("the shape, density and falloff reach the engine as Godot states them (density per metre)",
                f.LastShape == FogVolumeShapeKind.Cylinder && f.LastDensity == 3f && f.LastFalloff == 0.5f && Near(f.LastEdge, 0.2f));
            // Engine X is Godot -Z, Y is Godot X, Z is Godot Y: half sizes (6 / 2, 4 / 2, 2 / 2) metres = (300, 200, 100) cm.
            T.Check("the half sizes follow the axis map in centimetres", Near(f.LastHalf[0], 300) && Near(f.LastHalf[1], 200) && Near(f.LastHalf[2], 100));
            // Position (1, 2, 3) Godot metres is (-3, 1, 2) * 100 cm.
            T.Check("the matrix carries the position in engine axes and centimetres", Near(f.LastWorld[9], -300) && Near(f.LastWorld[10], 100) && Near(f.LastWorld[11], 200));
            T.Check("an identity rotation is an identity basis", Near(f.LastWorld[0], 1) && Near(f.LastWorld[4], 1) && Near(f.LastWorld[8], 1) && Near(f.LastWorld[1], 0));
            T.Check("colours are linear: sRGB 0.5 is about 0.214", Near(f.LastAlbedo[0], 0.2140f, 5e-3f) && Near(f.LastEmission[0], 1f) && Near(f.LastEmission[1], 0f));

            // A node scale scales the box through the matrix, not the half sizes.
            fog.Scale = new Vector3(2, 1, 1);
            T.Frame();   // a changed global transform is republished on the next frame
            T.Check("a node scaled 2 along Godot X scales engine row Y", Near(f.LastWorld[4], 2) && Near(f.LastHalf[1], 200));
            var plain = new FogVolume { Name = "Plain" };
            root.AddChild(plain);
            T.Check("no material is Godot's default fog: density 1, white, no emission", f.LastDensity == 1f && f.LastAlbedo[0] == 1f && f.LastEmission[0] == 0f && f.LastEdge == 0.1f);

            root.RemoveChild(fog);
            T.Check("leaving the tree destroys the volume", f.Calls.Contains("destroy fog 1") && fog.VolumeId == 0);
            root.QueueFree();
            T.Frame();
        }
        finally { Done(); }
    }

    private static void FogFollows()
    {
        Fake f = Install();
        try
        {
            var root = new Node3D { Name = "Platform" };
            var mat = new FogMaterial();
            var fog = new FogVolume { Name = "Mist", Material = mat };
            root.AddChild(fog);
            SceneTree.Instance.Root.AddChild(root);
            int n = fog.PublishCount;
            mat.Density = 5f;
            T.Check("a material change republishes", fog.PublishCount == n + 1 && f.LastDensity == 5f);
            mat.Albedo = new Color(1, 0, 0, 1);
            T.Check("and so does a colour", f.LastAlbedo[0] == 1f && f.LastAlbedo[1] == 0f);
            fog.Size = new Vector3(10, 10, 10);
            T.Check("a size change republishes", Near(f.LastHalf[0], 500));
            n = fog.PublishCount;
            T.Frame();
            T.Check("a still volume is not republished each frame", fog.PublishCount == n);

            root.Position = new Vector3(0, 0, -10);   // the parent moves: Godot -Z is engine +X
            T.Frame();
            T.Check("a moving parent republishes the volume's world matrix", fog.PublishCount > n && Near(f.LastWorld[9], 1000), $"x = {f.LastWorld[9]}");

            fog.Visible = false;
            T.Check("hiding the volume disables it", f.Calls.Contains("fogenabled 1 False"));
            fog.Visible = true;
            T.Check("showing it enables it again", f.Calls.Last() == "fogenabled 1 True" || f.Calls.Contains("fogenabled 1 True"));

            fog.Material = null;
            T.Check("dropping the material falls back to the default fog", f.LastDensity == 1f);
            mat.Density = 9f;
            T.Check("and the old material no longer republishes", f.LastDensity == 1f);
            root.QueueFree();
            T.Frame();
        }
        finally { Done(); }
    }

    private static void Probe()
    {
        Fake f = Install();
        try
        {
            var room = new Node3D { Name = "Room" };
            var probe = new ReflectionProbe
            {
                Name = "Probe", Size = new Vector3(10, 4, 6), OriginOffset = new Vector3(1, 0, 2), BoxProjection = true, Interior = true, Intensity = 0.5f,
                BlendDistance = 2f, MaxDistance = 30f, UpdateMode = ReflectionProbe.UpdateModeEnum.Always, AmbientMode = ReflectionProbe.AmbientModeEnum.Color,
                AmbientColor = new Color(1, 1, 1, 1), AmbientColorEnergy = 2f, Position = new Vector3(0, 2, 0),
            };
            room.AddChild(probe);
            SceneTree.Instance.Root.AddChild(room);
            T.Check("entering the tree creates the probe under its path", f.Calls.Contains("create probe /root/Room/Probe") && probe.ProbeId == 1);
            T.Check("the look reaches the engine in centimetres", Near(f.LastIntensity, 0.5f) && Near(f.LastBlend, 200) && Near(f.LastMax, 3000) && f.LastBox && f.LastInterior && f.LastAlways);
            T.Check("half sizes follow the axis map (engine X is Godot -Z)", Near(f.LastHalf[0], 300) && Near(f.LastHalf[1], 500) && Near(f.LastHalf[2], 200));
            T.Check("the origin offset follows it too: Godot (1, 0, 2) is engine (-200, 100, 0)", Near(f.LastOrigin[0], -200) && Near(f.LastOrigin[1], 100) && Near(f.LastOrigin[2], 0));
            T.Check("the ambient colour is linear and the mode and energy pass", f.Calls.Contains("probeambient 1 2 2") && Near(f.LastAmbient[0], 1f));
            T.Check("the masks pass", f.Calls.Contains($"probemasks 1 {0xFFFFFu} {0xFFFFFu}"));

            probe.Intensity = 1f;
            T.Check("a property change republishes", Near(f.LastIntensity, 1f));

            f.Next = new ProbeBlend { Ids = new[] { 1 }, Weights = new[] { 0.75f }, Rest = 0.25f };
            (ReflectionProbe Probe, float Weight)[] sel = ReflectionProbe.Select(new Vector3(0, 1, 0), new Vector3(0, 1, 5), out float rest);
            T.Check("Select maps the engine's answer to nodes", sel.Length == 1 && ReferenceEquals(sel[0].Probe, probe) && Near(sel[0].Weight, 0.75f) && Near(rest, 0.25f));
            T.Check("and asks in engine space and centimetres: Godot (0, 1, 0) is (0, 0, 100)", f.Calls.Contains("select 0 0 100"));

            probe.Visible = false;
            T.Check("a hidden probe is disabled", f.Calls.Contains("probeenabled 1 False"));
            room.RemoveChild(probe);
            T.Check("leaving the tree destroys the probe and forgets the id", f.Calls.Contains("destroy probe 1") && probe.ProbeId == 0);
            f.Next = new ProbeBlend { Ids = new[] { 1 }, Weights = new[] { 1f }, Rest = 0f };
            T.Check("a destroyed probe is not returned by Select", ReflectionProbe.Select(Vector3.Zero, Vector3.Zero, out _).Length == 0);
            room.QueueFree();
            T.Frame();
        }
        finally { Done(); }
    }

    private static void BakedNodes()
    {
        Fake f = Install();
        try
        {
            var root = new Node { Name = "Baked" };
            var lg = new LightmapGI { Name = "Lightmap", Quality = LightmapGI.BakeQualityEnum.Ultra, Bounces = 5, LightData = new LightmapGIData() };
            var lp = new LightmapProbe { Name = "Probe" };
            var vg = new VoxelGI { Name = "Voxels", Subdiv = VoxelGI.SubdivEnum.Subdiv256, Size = new Vector3(40, 10, 40), Data = new VoxelGIData() };
            root.AddChild(lg); root.AddChild(lp); root.AddChild(vg);
            SceneTree.Instance.Root.AddChild(root);
            T.Check("properties are kept", lg.Quality == LightmapGI.BakeQualityEnum.Ultra && lg.Bounces == 5 && lg.LightData != null && vg.Subdiv == VoxelGI.SubdivEnum.Subdiv256 && vg.Size.X == 40);
            T.Check("none of them is baked, and none publishes anything", !lg.IsBaked && !vg.IsBaked && f.Calls.Count == 0);
            root.QueueFree();
            T.Frame();
        }
        finally { Done(); }
    }

    private static void NoEngine()
    {
        EnvFxBackend.Override = null;
        var root = new Node { Name = "Headless" };
        var fog = new FogVolume { Name = "Mist" };
        var probe = new ReflectionProbe { Name = "Probe" };
        root.AddChild(fog); root.AddChild(probe);
        SceneTree.Instance.Root.AddChild(root);
        T.Check("without an engine a volume and a probe publish nothing and have no id", fog.VolumeId == 0 && probe.ProbeId == 0 && fog.PublishCount == 0 && probe.PublishCount == 0);
        T.Check("queries answer empty", ReflectionProbe.Select(Vector3.Zero, Vector3.Zero, out float rest).Length == 0 && rest == 1f);
        T.Check("a projected direction is the input, normalised", Near(probe.ProjectDirection(Vector3.Zero, new Vector3(0, 3, 0)).Y, 1f));
        root.QueueFree();
        T.Frame();
    }

    // Runs `AverAssetC fabric` on the hand-written EnvVolGame project and instantiates its level against the recording backend: the contract between what the
    // scene converter writes (Props, an inline FogMaterial) and what these classes read. Skipped when the tool has not been built.
    private static void ConverterOutput()
    {
        string? exe = null;
        for (DirectoryInfo? d = new DirectoryInfo(AppContext.BaseDirectory); d != null && exe == null; d = d.Parent)
        {
            string p = Path.Combine(d.FullName, "build", "bin", OperatingSystem.IsWindows() ? "AverAssetC.exe" : "AverAssetC");
            if (File.Exists(p)) exe = p;
        }
        if (exe == null) { Console.WriteLine("  SKIP: AverAssetC is not built (build/bin/AverAssetC)"); return; }
        string output = Path.Combine(Path.GetTempPath(), "fabric_envvol_" + Guid.NewGuid().ToString("N"));
        Fake f = Install();
        string? savedRoot = ResourceLoader.ContentRootOverride;
        try
        {
            var psi = new System.Diagnostics.ProcessStartInfo(exe) { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false };
            psi.ArgumentList.Add("fabric"); psi.ArgumentList.Add(Path.Combine(T.DataDir(), "EnvVolGame")); psi.ArgumentList.Add(output); psi.ArgumentList.Add("--scenes-only");
            using (var proc = System.Diagnostics.Process.Start(psi)!)
            {
                proc.StandardOutput.ReadToEnd(); proc.StandardError.ReadToEnd();
                proc.WaitForExit();
                T.Check("the converter ran", proc.ExitCode == 0, "exit " + proc.ExitCode);
            }
            ResourceLoader.ContentRootOverride = Path.Combine(output, "EnvVolGame", "Content");
            PackedScene scene = ResourceLoader.Load<PackedScene>("res://main.tscn")!;
            T.Check("the level can be instantiated", scene.CanInstantiate());
            T.Logs.Clear();
            Node main = scene.Instantiate();
            SceneTree.Instance.Root.AddChild(main);
            T.Check("no error was logged while it entered the tree", T.ErrorCount() == 0, string.Join(" | ", T.Logs.Where(l => l.Level == Aver.Scripting.Log.Level.Error).Select(l => l.Message)));

            var mist = main.GetNode<FogVolume>("Mist");
            T.Check("the volume: a cylinder, 4 x 2 x 6 m", mist.Shape == RenderingServer.FogVolumeShape.Cylinder && mist.Size == new Vector3(4, 2, 6));
            T.Check("its FogMaterial came through inline with Godot's values",
                mist.Material is FogMaterial { Density: 2f, HeightFalloff: > 0.39f and < 0.41f } fm && T.Near(fm.EdgeFade, 0.3f) && T.Near(fm.Albedo.G, 0.6f) && T.Near(fm.Emission.R, 0.1f));
            T.Check("it published under its tree path, in centimetres on the engine's axes",
                mist.VolumeId != 0 && f.Calls.Any(c => c.StartsWith("create fog /root/") && c.EndsWith("/Mist")) && f.Calls.Contains("fog " + mist.VolumeId));
            var haze = main.GetNode<FogVolume>("Haze");
            T.Check("a world volume", haze.Shape == RenderingServer.FogVolumeShape.World && haze.Material == null);
            var custom = main.GetNode<FogVolume>("Custom");
            T.Check("a volume whose ShaderMaterial was not converted has no material (Godot's default fog)", custom.Material == null && custom.Shape == RenderingServer.FogVolumeShape.Box);

            var probe = main.GetNode<ReflectionProbe>("Probe");
            T.Check("the probe: Godot's values",
                probe.UpdateMode == ReflectionProbe.UpdateModeEnum.Always && T.Near(probe.Intensity, 0.5f) && T.Near(probe.MaxDistance, 30f) && probe.Size == new Vector3(10, 4, 6) &&
                probe.OriginOffset == new Vector3(1, 0, 2) && probe.BoxProjection && probe.Interior && probe.EnableShadows && T.Near(probe.BlendDistance, 2f) &&
                probe.AmbientMode == ReflectionProbe.AmbientModeEnum.Color && T.Near(probe.AmbientColorEnergy, 2f));
            T.Check("and it published", probe.ProbeId != 0 && f.LastBox && f.LastInterior && f.LastAlways && T.Near(f.LastMax, 3000f));

            var lm = main.GetNode<LightmapGI>("Lightmap");
            T.Check("LightmapGI keeps its bake settings and has no baked data", lm.Quality == LightmapGI.BakeQualityEnum.High && lm.Bounces == 5 && lm.Directional && lm.LightData == null);
            T.Check("LightmapProbe and VoxelGI are placed as themselves", main.GetNode("LMProbe") is LightmapProbe && main.GetNode<VoxelGI>("Voxels") is { Subdiv: VoxelGI.SubdivEnum.Subdiv256 } vg && vg.Size == new Vector3(40, 10, 40));
            main.QueueFree();
            T.Frame();
        }
        finally
        {
            ResourceLoader.ContentRootOverride = savedRoot;
            Done();
            try { Directory.Delete(output, true); } catch (IOException) { }
        }
    }
}
