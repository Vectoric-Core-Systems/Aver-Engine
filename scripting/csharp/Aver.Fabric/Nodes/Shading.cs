// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Shader and ShaderMaterial under Godot's names, and RenderingServer.global_shader_parameter_* (docs/plans/fabric/G6_SHADERS.md, SH2).
//
// The converter wrote each shader as a material graph and each ShaderMaterial as an .ocmat holding the uniform table (GDREGS); Fabric/shaders.json says
// where every uniform lives in that table. A ShaderMaterial loaded from such an .ocmat packs set_shader_parameter values into the table the way the
// converter's emitter reads them and hands the table to the engine (aver_pbr_set_gd_registers), which re-uploads one small texture: nothing recompiles.
// A texture parameter goes to the material slot the shader's sampler was given (aver_pbr_set_texture_path).

using System.Text.Json;

namespace Aver.Fabric;

// ---- the layout the converter wrote (Fabric/shaders.json) -------------------------------------------------------------------------------------------

/// <summary>One non-sampler uniform and its place in the material's table.</summary>
internal sealed class ShaderUniformInfo
{
    public string Name = "";
    public string Type = "float";   // the shading-language name: float, vec3, ivec2, bool, mat3, ...
    public int Array;               // 0, or the element count
    public string Scope = "normal"; // normal | instance | global
    public bool SourceColor;
    public int Reg, Comp, Registers, Stride;
}

internal sealed class ShaderSamplerInfo
{
    public string Name = "";
    public int Slot = -1;
    public bool Bindable = true;
}

/// <summary>What the converter knows about one shader.</summary>
internal sealed class ShaderLayout
{
    public string Source = "", Graph = "";
    public int Registers;
    public List<ShaderUniformInfo> Uniforms = new();
    public List<ShaderSamplerInfo> Samplers = new();
    public List<string> Materials = new();
    public Dictionary<string, string> GlobalTypes = new();

    public ShaderUniformInfo? Uniform(string name) => Uniforms.Find(u => u.Name == name);
    public ShaderSamplerInfo? Sampler(string name) => Samplers.Find(s => s.Name == name);

    /// <summary>Reads Fabric/shaders.json text. Returns the shaders by graph asset path.</summary>
    internal static Dictionary<string, ShaderLayout> Parse(string json)
    {
        var result = new Dictionary<string, ShaderLayout>(StringComparer.Ordinal);
        using JsonDocument doc = JsonDocument.Parse(json);
        if (!doc.RootElement.TryGetProperty("shaders", out JsonElement shaders)) return result;
        foreach (JsonElement s in shaders.EnumerateArray())
        {
            var l = new ShaderLayout
            {
                Source = s.GetProperty("source").GetString() ?? "",
                Graph = s.GetProperty("graph").GetString() ?? "",
                Registers = s.GetProperty("registers").GetInt32(),
            };
            foreach (JsonElement u in s.GetProperty("uniforms").EnumerateArray())
                l.Uniforms.Add(new ShaderUniformInfo
                {
                    Name = u.GetProperty("name").GetString() ?? "",
                    Type = u.GetProperty("type").GetString() ?? "float",
                    Array = u.GetProperty("array").GetInt32(),
                    Scope = u.GetProperty("scope").GetString() ?? "normal",
                    SourceColor = u.GetProperty("sourceColor").GetBoolean(),
                    Reg = u.GetProperty("reg").GetInt32(),
                    Comp = u.GetProperty("comp").GetInt32(),
                    Registers = u.GetProperty("registers").GetInt32(),
                    Stride = u.GetProperty("stride").GetInt32(),
                });
            foreach (JsonElement sm in s.GetProperty("samplers").EnumerateArray())
                l.Samplers.Add(new ShaderSamplerInfo { Name = sm.GetProperty("name").GetString() ?? "", Slot = sm.GetProperty("slot").GetInt32(), Bindable = sm.GetProperty("bindable").GetBoolean() });
            foreach (JsonElement m in s.GetProperty("materials").EnumerateArray()) l.Materials.Add(m.GetString() ?? "");
            if (s.TryGetProperty("globalTypes", out JsonElement gt))
                foreach (JsonProperty p in gt.EnumerateObject()) l.GlobalTypes[p.Name] = p.Value.GetString() ?? "";
            result[l.Graph] = l;
        }
        return result;
    }
}

// ---- the engine's end ------------------------------------------------------------------------------------------------------------------------------

/// <summary>The calls a ShaderMaterial makes on the engine's material library. The engine's is Aver.Render.PBR (pbr_abi.h); a test installs a recording one.</summary>
internal interface IShaderBackend
{
    int FindMaterial(string name);                       // the library handle of the material named so, 0 when it is not loaded
    bool SetRegisters(int material, float[] values);     // replaces the whole table
    bool SetTexturePath(int material, int slot, string path);
    bool ClearTexture(int material, int slot);
}

internal sealed class NativeShaderBackend : IShaderBackend
{
    // The material library's binding is Aver.Scripting.Pbr (the seam's owner, docs/ABI.md section 16); a host without Aver.Render.PBR makes every call a no-op.
    private static int s_state;   // 0 unknown, 1 loaded, 2 missing

    private static bool Available
    {
        get
        {
            if (s_state == 0)
            {
                try { _ = Aver.Scripting.Pbr.Count; s_state = 1; }
                catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException) { s_state = 2; }
            }
            return s_state == 1;
        }
    }

    public int FindMaterial(string name)
    {
        if (!Available) return 0;
        int n = Aver.Scripting.Pbr.Count;
        for (int i = 0; i < n; i++)
        {
            Aver.Scripting.Pbr.PbrMaterial m = Aver.Scripting.Pbr.At(i);
            if (m.Handle > 0 && m.Name == name) return m.Handle;
        }
        return 0;
    }

    public bool SetRegisters(int material, float[] values) => Available && Aver.Scripting.Pbr.FromHandle(material).SetGdRegisters(values);
    public bool SetTexturePath(int material, int slot, string path) => Available && Aver.Scripting.Pbr.FromHandle(material).SetTexturePath((Aver.Scripting.PbrTextureSlot)slot, path);
    public bool ClearTexture(int material, int slot) => Available && Aver.Scripting.Pbr.FromHandle(material).ClearTexture((Aver.Scripting.PbrTextureSlot)slot);
}

internal static class ShaderBackend
{
    internal static IShaderBackend Current = new NativeShaderBackend();
}

// ---- packing ---------------------------------------------------------------------------------------------------------------------------------------

/// <summary>Writes a Variant into the uniform table the way the converter's emitter reads it (std140-like registers; ints and bools as bit patterns).</summary>
internal static class ShaderPacking
{
    internal static float SrgbToLinear(float c)
    {
        double s = Math.Clamp(c, 0f, 1f);
        return (float)(s <= 0.04045 ? s / 12.92 : Math.Pow((s + 0.055) / 1.055, 2.4));
    }

    private static int Dim(string type)
    {
        if (type.StartsWith("mat", StringComparison.Ordinal)) return type[3] - '0';
        char last = type[^1];
        return last is '2' or '3' or '4' ? last - '0' : 1;
    }

    private static bool IsMatrix(string type) => type.StartsWith("mat", StringComparison.Ordinal);
    private static bool IsInt(string type) => type == "int" || type.StartsWith("ivec", StringComparison.Ordinal);
    private static bool IsUInt(string type) => type == "uint" || type.StartsWith("uvec", StringComparison.Ordinal);
    private static bool IsBool(string type) => type == "bool" || type.StartsWith("bvec", StringComparison.Ordinal);

    /// <summary>The numbers of one value of <paramref name="type"/> (a matrix column-major), or false when the Variant does not fit.</summary>
    internal static bool Components(Variant v, string type, List<double> into)
    {
        int n = Dim(type);
        if (IsMatrix(type))
        {
            switch (v.VariantType)
            {
                case Variant.Type.Basis when n == 3:
                {
                    Basis b = v.AsBasis();   // the shader's mat3 holds the basis columns
                    foreach (Vector3 c in new[] { b.Column0, b.Column1, b.Column2 }) { into.Add(c.X); into.Add(c.Y); into.Add(c.Z); }
                    return true;
                }
                case Variant.Type.Transform3D when n == 4:
                {
                    Transform3D t = v.AsTransform3D();
                    foreach (Vector3 c in new[] { t.Basis.Column0, t.Basis.Column1, t.Basis.Column2 }) { into.Add(c.X); into.Add(c.Y); into.Add(c.Z); into.Add(0); }
                    into.Add(t.Origin.X); into.Add(t.Origin.Y); into.Add(t.Origin.Z); into.Add(1);
                    return true;
                }
                default: return false;
            }
        }
        switch (v.VariantType)
        {
            case Variant.Type.Bool: case Variant.Type.Int: case Variant.Type.Float:
                if (n != 1) return false;
                into.Add(v.VariantType == Variant.Type.Bool ? (v.AsBool() ? 1 : 0) : v.AsDouble());
                return true;
            case Variant.Type.Vector2: case Variant.Type.Vector2I:
            { Vector2 x = v.AsVector2(); if (n < 2) return false; into.Add(x.X); into.Add(x.Y); return true; }
            case Variant.Type.Vector3: case Variant.Type.Vector3I:
            { Vector3 x = v.AsVector3(); if (n < 3) return false; into.Add(x.X); into.Add(x.Y); into.Add(x.Z); if (n == 4) into.Add(0); return true; }
            case Variant.Type.Vector4: case Variant.Type.Vector4I:
            { Vector4 x = v.AsVector4(); if (n < 4) return false; into.Add(x.X); into.Add(x.Y); into.Add(x.Z); into.Add(x.W); return true; }
            case Variant.Type.Color:
            { Color x = v.AsColor(); if (n < 3) return false; into.Add(x.R); into.Add(x.G); into.Add(x.B); if (n == 4) into.Add(x.A); return true; }
            default: return false;
        }
    }

    /// <summary>Packs a value into <paramref name="regs"/> (4 floats a register). False when the Variant does not fit the uniform's type.</summary>
    internal static bool Pack(ShaderUniformInfo u, Variant value, float[] regs, int blockBase = 0)
    {
        var comps = new List<double>();
        int count = u.Array > 0 ? u.Array : 1;
        if (u.Array > 0)
        {
            if (value.VariantType != Variant.Type.Array) return false;
            Collections.Array arr = value.As<Collections.Array>();
            for (int i = 0; i < count && i < arr.Count; i++) if (!Components(arr[i], u.Type, comps)) return false;
        }
        else if (!Components(value, u.Type, comps)) return false;
        int n = Dim(u.Type);
        bool matrix = IsMatrix(u.Type), isInt = IsInt(u.Type), isUInt = IsUInt(u.Type), isBool = IsBool(u.Type);
        bool decode = u.SourceColor && (u.Type == "vec3" || u.Type == "vec4");
        int cursor = 0;
        void Store(int index, double v)
        {
            if (index < 0 || index >= regs.Length) return;
            regs[index] = isInt ? BitConverter.Int32BitsToSingle((int)v)
                        : isUInt ? BitConverter.Int32BitsToSingle(unchecked((int)(uint)v))
                        : isBool ? BitConverter.Int32BitsToSingle(v != 0 ? 1 : 0)
                        : (float)v;
        }
        for (int e = 0; e < count; e++)
        {
            int baseReg = u.Reg + blockBase + e * u.Stride;
            if (matrix)
            {
                for (int c = 0; c < n; c++)
                    for (int r = 0; r < n; r++, cursor++) Store((baseReg + c) * 4 + r, cursor < comps.Count ? comps[cursor] : 0.0);
            }
            else
            {
                for (int c = 0; c < n; c++, cursor++)
                {
                    double v = cursor < comps.Count ? comps[cursor] : 0.0;
                    if (decode && c < 3) v = SrgbToLinear((float)v);
                    Store(baseReg * 4 + u.Comp + c, v);
                }
            }
        }
        return true;
    }

    /// <summary>The Variant a register range holds, for get_shader_parameter (scalars, vectors, colours and basic matrices).</summary>
    internal static Variant Unpack(ShaderUniformInfo u, float[] regs)
    {
        if (u.Array > 0) return default;
        int n = Dim(u.Type);
        int at = u.Reg * 4 + u.Comp;
        float F(int i) => at + i < regs.Length ? regs[at + i] : 0f;
        int I(int i) => BitConverter.SingleToInt32Bits(F(i));
        if (IsMatrix(u.Type)) return default;
        if (IsBool(u.Type) && n == 1) return Variant.CreateFrom(I(0) != 0);
        if (IsInt(u.Type) || IsUInt(u.Type))
        {
            if (n == 1) return Variant.CreateFrom(I(0));
            return n == 2 ? Variant.CreateFrom(new Vector2I(I(0), I(1))) : n == 3 ? Variant.CreateFrom(new Vector3I(I(0), I(1), I(2))) : Variant.CreateFrom(new Vector4I(I(0), I(1), I(2), I(3)));
        }
        if (n == 1) return Variant.CreateFrom(F(0));
        if (n == 2) return Variant.CreateFrom(new Vector2(F(0), F(1)));
        if (n == 3) return Variant.CreateFrom(new Vector3(F(0), F(1), F(2)));
        return Variant.CreateFrom(new Vector4(F(0), F(1), F(2), F(3)));
    }
}

// ---- the classes -----------------------------------------------------------------------------------------------------------------------------------

/// <summary>Godot's Shader. A converted project's shaders are already compiled into the engine; this holds where they came from and their uniform layout.</summary>
public class Shader : Resource
{
    public enum Mode { Spatial = 0, CanvasItem = 1, Particles = 2, Sky = 3, Fog = 4 }

    /// <summary>The shader source. A converted shader does not carry it (the engine runs the HLSL the converter emitted); <see cref="SourcePath"/> names the .gdshader.</summary>
    public string Code { get; set; } = "";

    /// <summary>The res:// path of the .gdshader (or the scene and sub-resource of an inline one) this shader was converted from.</summary>
    public string SourcePath { get; internal set; } = "";

    internal ShaderLayout? Layout;

    internal Mode ModeValue = Mode.Spatial;   // G6 SH3: Sky, Fog or Particles for a converted environment shader
    public Mode GetMode() => ModeValue;

    /// <summary>The names of the shader's uniforms (not its samplers), in declaration order within the normal, instance and global blocks.</summary>
    public string[] GetUniformNames() => Layout is null ? System.Array.Empty<string>() : Layout.Uniforms.Select(u => u.Name).ToArray();
    /// <summary>The names of the shader's sampler uniforms.</summary>
    public string[] GetSamplerNames() => Layout is null ? System.Array.Empty<string>() : Layout.Samplers.Select(s => s.Name).ToArray();
}

/// <summary>Godot's ShaderMaterial. set_shader_parameter writes the uniform table the engine's material reads; no shader is recompiled.</summary>
public partial class ShaderMaterial : Material
{
    public Shader? Shader { get; set; }

    private readonly Dictionary<string, Variant> _params = new(StringComparer.Ordinal);
    private float[] _regs = System.Array.Empty<float>();
    private string _engineName = "";
    private int _handle;
    private Dictionary<int, string> _texturePaths = new();
    private static readonly List<WeakReference<ShaderMaterial>> s_live = new();

    /// <summary>The library name of the engine material this resource drives (the .ocmat's NAME), or empty for a ShaderMaterial made in code.</summary>
    public string EngineMaterialName => _engineName;

    /// <summary>The uniform table as the engine holds it, 4 floats a register (a copy).</summary>
    public float[] GetUniformTable() => (float[])_regs.Clone();

    public void SetShaderParameter(StringName param, Variant value)
    {
        string name = param.ToString();
        _params[name] = value;
        ShaderLayout? layout = Shader?.Layout;
        if (layout is null) return;
        if (layout.Sampler(name) is { } sampler)
        {
            PushTexture(sampler, value);
            return;
        }
        ShaderUniformInfo? u = layout.Uniform(name);
        if (u is null || u.Scope == "global") return;
        if (_regs.Length == 0) _regs = new float[layout.Registers * 4];
        if (!ShaderPacking.Pack(u, value, _regs))
        {
            Aver.Scripting.Log.Warn($"[Fabric] shader parameter '{name}' of {EngineMaterialName}: a {value.VariantType} does not fit the uniform's type {u.Type}");
            return;
        }
        Push();
    }

    public Variant GetShaderParameter(StringName param)
    {
        string name = param.ToString();
        if (_params.TryGetValue(name, out Variant v)) return v;
        ShaderUniformInfo? u = Shader?.Layout?.Uniform(name);
        return u is null || _regs.Length == 0 ? default : ShaderPacking.Unpack(u, _regs);
    }

    private void PushTexture(ShaderSamplerInfo sampler, Variant value)
    {
        if (!sampler.Bindable || sampler.Slot < 0) return;
        if (!EnsureHandle()) return;
        Texture2D? t = value.AsGodotObject() as Texture2D;
        if (t is null || t.ResourcePath.Length == 0)
        {
            _texturePaths.Remove(sampler.Slot);
            ShaderBackend.Current.ClearTexture(_handle, sampler.Slot);
            return;
        }
        string rel = t.ResourcePath.StartsWith("res://", StringComparison.Ordinal) ? "Textures/" + t.ResourcePath["res://".Length..] : t.ResourcePath;
        _texturePaths[sampler.Slot] = rel;
        ShaderBackend.Current.SetTexturePath(_handle, sampler.Slot, rel);
    }

    private bool EnsureHandle()
    {
        if (_handle > 0) return true;
        if (_engineName.Length == 0) return false;
        _handle = ShaderBackend.Current.FindMaterial(_engineName);
        return _handle > 0;
    }

    private void Push()
    {
        if (_envKind is not null) { PushEnv(); return; }   // G6 SH3: a sky / fog / particles shader has no engine material
        if (_regs.Length == 0 || !EnsureHandle()) return;
        ShaderBackend.Current.SetRegisters(_handle, _regs);
    }

    /// <summary>Re-resolves the engine material (a level change may have rebuilt it) and writes the table again.</summary>
    public void RefreshEngine()
    {
        _handle = 0;
        Push();
        foreach (var kv in _texturePaths) if (EnsureHandle()) ShaderBackend.Current.SetTexturePath(_handle, kv.Key, kv.Value);
    }

    // ---- loading from the converted project -----------------------------------------------------------------------------------------------

    private static Dictionary<string, ShaderLayout>? s_layouts;
    private static string? s_layoutsRoot;

    internal static void ResetLayouts() { s_layouts = null; s_layoutsRoot = null; s_live.Clear(); }

    private static Dictionary<string, ShaderLayout> Layouts()
    {
        string root = ResourceLoader.ContentRoot;
        if (s_layouts != null && s_layoutsRoot == root) return s_layouts;
        s_layoutsRoot = root;
        s_layouts = new Dictionary<string, ShaderLayout>(StringComparer.Ordinal);
        if (root.Length == 0) return s_layouts;
        string manifest = System.IO.Path.Combine(root, "..", "Fabric", "shaders.json");
        try { if (File.Exists(manifest)) s_layouts = ShaderLayout.Parse(File.ReadAllText(manifest)); }
        catch (Exception ex) when (ex is JsonException or IOException or KeyNotFoundException or InvalidOperationException)
        {
            Aver.Scripting.Log.Warn($"[Fabric] cannot read {manifest}: {ex.Message}");
        }
        return s_layouts;
    }

    /// <summary>A ShaderMaterial for an .ocmat that names a converted shader (GRAPHREF), or null when the file is an ordinary material.</summary>
    internal static ShaderMaterial? TryLoad(string file, string resPath)
    {
        string text;
        try { text = File.ReadAllText(file); } catch (IOException) { return null; }
        string? graph = null, name = null;
        var words = new List<uint>();
        foreach (string raw in text.Split('\n'))
        {
            string line = raw.Trim();
            if (line.StartsWith("GRAPHREF ", StringComparison.Ordinal)) graph = line["GRAPHREF ".Length..].Trim();
            else if (line.StartsWith("NAME ", StringComparison.Ordinal)) name = line["NAME ".Length..].Trim();
            else if (line.StartsWith("GDREGS", StringComparison.Ordinal))
                foreach (string w in line.Split(' ', StringSplitOptions.RemoveEmptyEntries).Skip(1))
                    if (uint.TryParse(w, System.Globalization.NumberStyles.HexNumber, System.Globalization.CultureInfo.InvariantCulture, out uint bits)) words.Add(bits);
        }
        if (graph is null || graph.Length == 0) return null;
        var m = new ShaderMaterial { _engineName = name ?? System.IO.Path.GetFileNameWithoutExtension(file) };
        _ = resPath;
        if (Layouts().TryGetValue(graph, out ShaderLayout? layout))
        {
            m.Shader = new Shader { Layout = layout, SourcePath = layout.Source, ResourcePath = layout.Graph };
        }
        else
        {
            Aver.Scripting.Log.Warn($"[Fabric] the shader graph '{graph}' of {m._engineName} is not in Fabric/shaders.json; set_shader_parameter will have no effect");
        }
        m._regs = new float[words.Count];
        for (int i = 0; i < words.Count; i++) m._regs[i] = BitConverter.Int32BitsToSingle(unchecked((int)words[i]));
        // global uniforms take the live global value when the game has set one
        if (layout != null)
            foreach (ShaderUniformInfo u in layout.Uniforms)
                if (u.Scope == "global" && RenderingServer.GlobalValue(u.Name) is { } gv && m._regs.Length > 0) ShaderPacking.Pack(u, gv, m._regs);
        lock (s_live) s_live.Add(new WeakReference<ShaderMaterial>(m));
        return m;
    }

    /// <summary>Called by RenderingServer.global_shader_parameter_set: repacks the global uniform of every live material that declares it.</summary>
    internal static void ApplyGlobal(string name, Variant value)
    {
        lock (s_live)
        {
            for (int i = s_live.Count - 1; i >= 0; i--)
            {
                if (!s_live[i].TryGetTarget(out ShaderMaterial? m)) { s_live.RemoveAt(i); continue; }
                ShaderUniformInfo? u = m.Shader?.Layout?.Uniform(name);
                if (u is null || u.Scope != "global" || m._regs.Length == 0) continue;
                if (ShaderPacking.Pack(u, value, m._regs)) m.Push();
            }
        }
    }
}

public static partial class RenderingServer
{
    /// <summary>Godot's RenderingServer.GlobalShaderParameterType (the numbering of GLOBAL_VAR_TYPE_*).</summary>
    public enum GlobalShaderParameterType
    {
        Bool = 0, Bvec2 = 1, Bvec3 = 2, Bvec4 = 3, Int = 4, Ivec2 = 5, Ivec3 = 6, Ivec4 = 7, Rect2I = 8, Uint = 9, Uvec2 = 10, Uvec3 = 11, Uvec4 = 12,
        Float = 13, Vec2 = 14, Vec3 = 15, Vec4 = 16, Color = 17, Rect2 = 18, Mat2 = 19, Mat3 = 20, Mat4 = 21, Transform2D = 22, Transform = 23,
        Sampler2D = 24, Sampler2DArray = 25, Sampler3D = 26, SamplerCube = 27, SamplerExternal = 28, Max = 29,
    }

    private static readonly Dictionary<string, Variant> s_globals = new(StringComparer.Ordinal);
    private static readonly Dictionary<string, GlobalShaderParameterType> s_globalTypes = new(StringComparer.Ordinal);

    internal static Variant? GlobalValue(string name) => s_globals.TryGetValue(name, out Variant v) ? (Variant?)v : null;

    public static void GlobalShaderParameterAdd(StringName name, GlobalShaderParameterType type, Variant defaultValue)
    {
        string n = name.ToString();
        s_globalTypes[n] = type;
        s_globals[n] = defaultValue;
        ShaderMaterial.ApplyGlobal(n, defaultValue);
    }

    public static void GlobalShaderParameterRemove(StringName name)
    {
        string n = name.ToString();
        s_globals.Remove(n);
        s_globalTypes.Remove(n);
    }

    /// <summary>Sets a global shader parameter and rewrites it in every live ShaderMaterial that declares it.</summary>
    public static void GlobalShaderParameterSet(StringName name, Variant value)
    {
        string n = name.ToString();
        s_globals[n] = value;
        ShaderMaterial.ApplyGlobal(n, value);
    }

    public static Variant GlobalShaderParameterGet(StringName name) => s_globals.TryGetValue(name.ToString(), out Variant v) ? v : default;
    public static GlobalShaderParameterType GlobalShaderParameterGetType(StringName name) => s_globalTypes.TryGetValue(name.ToString(), out GlobalShaderParameterType t) ? t : GlobalShaderParameterType.Max;
    public static StringName[] GlobalShaderParameterGetList() => s_globals.Keys.Select(k => new StringName(k)).ToArray();

    internal static void ResetGlobals() { s_globals.Clear(); s_globalTypes.Clear(); }
}
