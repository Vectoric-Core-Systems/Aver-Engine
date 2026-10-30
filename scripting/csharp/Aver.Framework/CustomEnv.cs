// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke declarations and the managed surface for framework_customenv_abi.h.

using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>The kinds of custom HLSL program the renderers can run beyond a mesh material (AVER_FW_CENV_*).</summary>
public enum CustomEnvKind
{
    /// <summary>A sky() program that replaces the background, reflections and ambient light.</summary>
    Sky = 0,
    /// <summary>A fog() program on a fog volume.</summary>
    Fog = 1,
    /// <summary>A particles program (start() and process()) on a GPU emitter.</summary>
    Particles = 2,
}

internal static class FwCustomEnv
{
    private const string Lib = "Aver.Framework";

    [DllImport(Lib)] internal static extern int aver_fw_cenv_abi_version();
    [DllImport(Lib)] internal static extern int aver_fw_cenv_register(int kind, [MarshalAs(UnmanagedType.LPUTF8Str)] string name, [MarshalAs(UnmanagedType.LPUTF8Str)] string ocgraphText);
    [DllImport(Lib)] internal static extern int aver_fw_cenv_set_registers(int kind, [MarshalAs(UnmanagedType.LPUTF8Str)] string name, float[] regs, int floatCount);
    [DllImport(Lib)] internal static extern int aver_fw_cenv_use_sky([MarshalAs(UnmanagedType.LPUTF8Str)] string? name);
    [DllImport(Lib)] internal static extern int aver_fw_cenv_unregister(int kind, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    [DllImport(Lib)] internal static extern int aver_fw_cenv_count(int kind);
}

/// <summary>Custom sky, fog and particles programs. A program is the <c>.ocgraph</c> a tool wrote (<c>DOMAIN customenv</c>, one EmbeddedHlslEnv node);
/// registering it hands the renderers its HLSL, and its uniform registers are set without rebuilding anything.</summary>
public static class CustomEnv
{
    /// <summary>Registers (or replaces) the program <paramref name="name"/> from the text of its .ocgraph. False for a malformed graph or a kind that does not match.</summary>
    public static bool Register(CustomEnvKind kind, string name, string ocgraphText) =>
        !string.IsNullOrEmpty(name) && !string.IsNullOrEmpty(ocgraphText) && FwCustomEnv.aver_fw_cenv_register((int)kind, name, ocgraphText) != 0;

    /// <summary>Replaces the program's uniform registers (four floats a register, normal block then global block).</summary>
    public static bool SetRegisters(CustomEnvKind kind, string name, float[] registers) =>
        !string.IsNullOrEmpty(name) && registers.Length > 0 && FwCustomEnv.aver_fw_cenv_set_registers((int)kind, name, registers, registers.Length) != 0;

    /// <summary>Makes the registered sky program the sky; null or empty returns to the engine's own sky.</summary>
    public static bool UseSky(string? name) => FwCustomEnv.aver_fw_cenv_use_sky(name) != 0;

    /// <summary>Removes the program; false when no program of that kind and name is registered.</summary>
    public static bool Unregister(CustomEnvKind kind, string name) => !string.IsNullOrEmpty(name) && FwCustomEnv.aver_fw_cenv_unregister((int)kind, name) != 0;

    /// <summary>How many programs of the kind are registered.</summary>
    public static int Count(CustomEnvKind kind) => FwCustomEnv.aver_fw_cenv_count((int)kind);
}
