// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Sky, fog and particles shaders (docs/plans/fabric/G6_SHADERS.md, SH3), VisualShader, and GPUParticles3D.
//
// The converter wrote each sky / fog / particles ShaderMaterial as Shaders/<name>.<kind>.ocgraph (DOMAIN customenv) with the material's shader_parameter values baked
// into its register table. Loading that path gives a ShaderMaterial in "environment" form: registering it hands the engine the HLSL (framework_customenv_abi.h) and
// set_shader_parameter repacks the register table the way the converter did. The engine side: a sky shader is baked into Voxi's scene shaders (a changed value
// recompiles them), a fog shader is read by every FogVolume that names it (registers ride the froxel constants), a particles shader's uniforms are the effect's
// user parameters (reg0, reg1, ...: colour-kind, four floats each).

using Aver.Framework;

namespace Aver.Fabric;

/// <summary>Where converted environment shaders are registered. The default talks to the engine; tests install a recording one.</summary>
internal interface IEnvShaderBackend
{
    bool Register(CustomEnvKind kind, string name, string graphText);
    bool SetRegisters(CustomEnvKind kind, string name, float[] registers);
    bool UseSky(string? name);
}

internal sealed class NativeEnvShaderBackend : IEnvShaderBackend
{
    // A host without the engine's Aver.Framework library (a headless run) simply has nothing to register with.
    public bool Register(CustomEnvKind kind, string name, string graphText) { try { return CustomEnv.Register(kind, name, graphText); } catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; } }
    public bool SetRegisters(CustomEnvKind kind, string name, float[] registers) { try { return CustomEnv.SetRegisters(kind, name, registers); } catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; } }
    public bool UseSky(string? name) { try { return CustomEnv.UseSky(name); } catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; } }
}

internal static class EnvShaderBackend
{
    internal static IEnvShaderBackend Current = new NativeEnvShaderBackend();
}

public partial class ShaderMaterial
{
    private CustomEnvKind? _envKind;
    private string _envName = "";
    private string _envGraphText = "";
    private bool _envRegistered;

    /// <summary>"sky", "fog" or "particles" for a converted environment shader, else empty.</summary>
    public string EnvironmentKind => _envKind switch { CustomEnvKind.Sky => "sky", CustomEnvKind.Fog => "fog", CustomEnvKind.Particles => "particles", _ => "" };

    /// <summary>The shader's name in the engine's registry (the graph's name).</summary>
    internal string EnvName => _envName;

    /// <summary>Raised after the register table changed (a particles shader's emitters copy it into their user parameters).</summary>
    internal event Action<ShaderMaterial>? EnvRegistersChanged;

    /// <summary>The register table as 4-float registers (a copy).</summary>
    internal float[] EnvRegisters => (float[])_regs.Clone();

    // The value of the node's reg= attribute: hexadecimal 32-bit words, ',' joined.
    private static List<float> ReadRegisterWords(string graphText)
    {
        var values = new List<float>();
        foreach (string raw in graphText.Split('\n'))
        {
            foreach (string token in raw.Split(' ', '\t', '\r'))
            {
                if (!token.StartsWith("reg=", StringComparison.Ordinal)) continue;
                foreach (string w in token["reg=".Length..].Split(',', StringSplitOptions.RemoveEmptyEntries))
                    if (uint.TryParse(w, System.Globalization.NumberStyles.HexNumber, System.Globalization.CultureInfo.InvariantCulture, out uint bits))
                        values.Add(BitConverter.Int32BitsToSingle(unchecked((int)bits)));
            }
        }
        return values;
    }

    private static string? ReadAttribute(string graphText, string key)
    {
        foreach (string raw in graphText.Split('\n'))
            foreach (string token in raw.Split(' ', '\t', '\r'))
                if (token.StartsWith(key + "=", StringComparison.Ordinal)) return token[(key.Length + 1)..];
        return null;
    }

    /// <summary>A ShaderMaterial for a converted sky / fog / particles graph, or null when the file is not one.</summary>
    internal static ShaderMaterial? TryLoadEnv(string file, string resPath)
    {
        string text;
        try { text = File.ReadAllText(file); } catch (IOException) { return null; }
        // "DOMAIN godotenv" is the spelling graphs written before the rename carry.
        if (!text.Contains("DOMAIN customenv", StringComparison.Ordinal) && !text.Contains("DOMAIN godotenv", StringComparison.Ordinal)) return null;
        string? kind = ReadAttribute(text, "kind");
        CustomEnvKind? k = kind switch { "sky" => CustomEnvKind.Sky, "fog" => CustomEnvKind.Fog, "particles" => CustomEnvKind.Particles, _ => null };
        if (k is null) return null;
        string name = System.IO.Path.GetFileNameWithoutExtension(System.IO.Path.GetFileNameWithoutExtension(file));
        var m = new ShaderMaterial { _envKind = k, _envName = name, _envGraphText = text, _engineName = name };
        m._regs = ReadRegisterWords(text).ToArray();
        string graphKey = resPath.StartsWith("res://", StringComparison.Ordinal) ? resPath["res://".Length..] : resPath;
        if (Layouts().TryGetValue(graphKey, out ShaderLayout? layout) || Layouts().TryGetValue("Shaders/" + System.IO.Path.GetFileName(file), out layout))
            m.Shader = new Shader { Layout = layout, SourcePath = layout.Source, ResourcePath = layout.Graph, ModeValue = k == CustomEnvKind.Sky ? Shader.Mode.Sky : k == CustomEnvKind.Fog ? Shader.Mode.Fog : Shader.Mode.Particles };
        else
            Aver.Scripting.Log.Warn($"[Fabric] the shader graph '{resPath}' is not in Fabric/shaders.json; set_shader_parameter will have no effect");
        lock (s_live) s_live.Add(new WeakReference<ShaderMaterial>(m));
        return m;
    }

    /// <summary>Hands the engine this shader (sky and fog; a particles shader is registered by the first emitter that uses it). Safe to call again.</summary>
    internal bool RegisterWithEngine()
    {
        if (_envKind is null) return false;
        if (!_envRegistered)
        {
            if (!EnvShaderBackend.Current.Register(_envKind.Value, _envName, _envGraphText)) return false;
            _envRegistered = true;
            if (_regs.Length > 0) EnvShaderBackend.Current.SetRegisters(_envKind.Value, _envName, _regs);
        }
        return true;
    }

    /// <summary>Makes this sky shader the sky (Sky.sky_material set on the current Environment). False for another kind.</summary>
    internal bool UseAsSky()
    {
        if (_envKind != CustomEnvKind.Sky || !RegisterWithEngine()) return false;
        return EnvShaderBackend.Current.UseSky(_envName);
    }

    private void PushEnv()
    {
        if (_envKind is null || _regs.Length == 0) return;
        if (_envRegistered) EnvShaderBackend.Current.SetRegisters(_envKind.Value, _envName, _regs);
        EnvRegistersChanged?.Invoke(this);
    }
}

/// <summary>Godot's VisualShader: a shader built from nodes. The converter turned the graph into shading-language text (the way Godot generates it) and the result
/// into an ordinary shader; <see cref="GetCode"/> returns that text when the converter wrote it (Fabric/visualshaders).</summary>
public class VisualShader : Shader
{
    /// <summary>The shading-language text Godot would generate for the graph, as the converter re-generated it. Empty when it is not available.</summary>
    public string GetCode()
    {
        if (Code.Length > 0) return Code;
        string root = ResourceLoader.ContentRoot;
        if (root.Length == 0) return "";
        string stem = System.IO.Path.GetFileNameWithoutExtension(SourcePath.Length > 0 ? SourcePath : ResourcePath);
        string file = System.IO.Path.Combine(root, "..", "Fabric", "visualshaders", stem + ".gdshader.txt");
        try { if (File.Exists(file)) Code = File.ReadAllText(file); } catch (IOException) { }
        return Code;
    }
}
