// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke declarations and the managed surface for framework_godotenv_abi.h (Fabric G6 SH3).

using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>The kinds of converted Godot shader the renderers can run beyond a mesh material (AVER_FW_GENV_*).</summary>
public enum GodotEnvKind
{
    /// <summary>A sky() shader that replaces the background, reflections and ambient light.</summary>
    Sky = 0,
    /// <summary>A fog() shader on a FogVolume.</summary>
    Fog = 1,
    /// <summary>A particles shader (start() and process()) on a GPU emitter.</summary>
    Particles = 2,
}

internal static class FwGodotEnv
{
    private const string Lib = "Aver.Framework";

    [DllImport(Lib)] internal static extern int aver_fw_genv_abi_version();
    [DllImport(Lib)] internal static extern int aver_fw_genv_register(int kind, [MarshalAs(UnmanagedType.LPUTF8Str)] string name, [MarshalAs(UnmanagedType.LPUTF8Str)] string ocgraphText);
    [DllImport(Lib)] internal static extern int aver_fw_genv_set_registers(int kind, [MarshalAs(UnmanagedType.LPUTF8Str)] string name, float[] regs, int floatCount);
    [DllImport(Lib)] internal static extern int aver_fw_genv_use_sky([MarshalAs(UnmanagedType.LPUTF8Str)] string? name);
    [DllImport(Lib)] internal static extern int aver_fw_genv_unregister(int kind, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    [DllImport(Lib)] internal static extern int aver_fw_genv_count(int kind);
}

/// <summary>Converted Godot sky, fog and particles shaders (<c>docs/plans/fabric/G6_SHADERS.md</c> SH3). A shader is the <c>.ocgraph</c> the converter wrote
/// (<c>DOMAIN godotenv</c>); registering it hands the renderers its HLSL, and its uniform registers are set without rebuilding anything.</summary>
public static class GodotEnv
{
    /// <summary>Registers (or replaces) the shader <paramref name="name"/> from the text of its .ocgraph. False for a malformed graph or a kind that does not match.</summary>
    public static bool Register(GodotEnvKind kind, string name, string ocgraphText) =>
        !string.IsNullOrEmpty(name) && !string.IsNullOrEmpty(ocgraphText) && FwGodotEnv.aver_fw_genv_register((int)kind, name, ocgraphText) != 0;

    /// <summary>Replaces the shader's uniform registers (four floats a register, normal block then global block).</summary>
    public static bool SetRegisters(GodotEnvKind kind, string name, float[] registers) =>
        !string.IsNullOrEmpty(name) && registers.Length > 0 && FwGodotEnv.aver_fw_genv_set_registers((int)kind, name, registers, registers.Length) != 0;

    /// <summary>Makes the registered sky shader the sky; null or empty returns to the engine's own sky.</summary>
    public static bool UseSky(string? name) => FwGodotEnv.aver_fw_genv_use_sky(name) != 0;

    /// <summary>Removes the shader; false when no shader of that kind and name is registered.</summary>
    public static bool Unregister(GodotEnvKind kind, string name) => !string.IsNullOrEmpty(name) && FwGodotEnv.aver_fw_genv_unregister((int)kind, name) != 0;

    /// <summary>How many shaders of the kind are registered.</summary>
    public static int Count(GodotEnvKind kind) => FwGodotEnv.aver_fw_genv_count((int)kind);
}
