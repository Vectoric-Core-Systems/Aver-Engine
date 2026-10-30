// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// GPUParticles3D under Godot's names (docs/plans/fabric/G6_SHADERS.md, SH3).
//
// The converter turns a GPUParticles3D whose process material is a ShaderMaterial over a particles shader into a placement carrying a ParticleEmitter component
// (the .ocparticle it plays, simulated on the GPU with the shader as the custom stage). This node keeps Godot's properties and drives the shader's uniforms: the
// shader's registers are the effect's user parameters reg0, reg1, ... (colour kind, four floats each), so set_shader_parameter reaches the running emitter. A
// ParticleProcessMaterial is not converted; such a node keeps its properties and emits nothing.

using Aver.Framework;

namespace Aver.Fabric;

/// <summary>Where a particle node's shader registers go. The default writes the entity's user parameters; tests install a recording one.</summary>
internal interface IParticleParamBackend
{
    bool SetRegister(Entity entity, int register, float x, float y, float z, float w);
}

internal sealed class NativeParticleParamBackend : IParticleParamBackend
{
    public bool SetRegister(Entity entity, int register, float x, float y, float z, float w)
    {
        try { return entity.SetParticleColor("reg" + register, x, y, z, w); }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return false; }
    }
}

internal static class ParticleParamBackend
{
    internal static IParticleParamBackend Current = new NativeParticleParamBackend();
}

/// <summary>Godot's GPUParticles3D.</summary>
public class GPUParticles3D : GeometryInstance3D
{
    public enum DrawOrderEnum { Index = 0, Lifetime = 1, ReverseLifetime = 2, ViewDepth = 3 }
    public enum TransformAlignEnum { Disabled = 0, ZBillboard = 1, YToVelocity = 2, ZBillboardYToVelocity = 3 }

    private ShaderMaterial? _process;
    private bool _emitting = true;


    /// <summary>Whether the emitter is playing.</summary>
    public bool Emitting { get => _emitting; set { _emitting = value; PushState(); } }
    public int Amount { get; set; } = 8;
    public double Lifetime { get; set; } = 1.0;
    public bool OneShot { get; set; }
    public double Preprocess { get; set; }
    public double SpeedScale { get; set; } = 1.0;
    public double Explosiveness { get; set; }
    public double Randomness { get; set; }
    public bool LocalCoords { get; set; }
    public int FixedFps { get; set; }
    public bool Interpolate { get; set; } = true;
    public bool FractDelta { get; set; } = true;
    public DrawOrderEnum DrawOrder { get; set; } = DrawOrderEnum.Index;
    public TransformAlignEnum TransformAlign { get; set; } = TransformAlignEnum.Disabled;
    public Aabb VisibilityAabb { get; set; } = new Aabb(new Vector3(-4, -4, -4), new Vector3(8, 8, 8));
    public Mesh? DrawPass1 { get; set; }
    public int DrawPasses { get; set; } = 1;

    /// <summary>A <see cref="ShaderMaterial"/> over a particles shader (G6 SH3). Another material is kept and does nothing.</summary>
    public Material? ProcessMaterial
    {
        get => _process;
        set
        {
            if (_process != null) _process.EnvRegistersChanged -= OnRegisters;
            _process = value as ShaderMaterial;
            if (_process is { EnvironmentKind: not "particles" }) _process = null;
            if (_process != null) { _process.EnvRegistersChanged += OnRegisters; PushRegisters(); }
        }
    }

    internal int RegisterPushes { get; private set; }

    private void OnRegisters(ShaderMaterial m) => PushRegisters();

    // The registers the shader declares become the emitter's user parameters (reg<i>, colour kind).
    private void PushRegisters()
    {
        if (_process is null || !IsInsideTree()) return;
        float[] regs = _process.EnvRegisters;
        for (int r = 0; r * 4 + 3 < regs.Length && r < 16; r++)
            ParticleParamBackend.Current.SetRegister(Self, r, regs[r * 4], regs[r * 4 + 1], regs[r * 4 + 2], regs[r * 4 + 3]);
        RegisterPushes++;
    }

    private void PushState()
    {
        if (!IsInsideTree()) return;
        try { if (_emitting) Self.BurstEmitter(0); else Self.StopEmitter(false); }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { }
    }

    internal override void OnEnteringTree()
    {
        base.OnEnteringTree();
        if (_process != null) { _process.RegisterWithEngine(); PushRegisters(); }
    }

    /// <summary>Restarts the emission (a burst of <see cref="Amount"/> for a one-shot emitter).</summary>
    public void Restart()
    {
        if (!IsInsideTree()) return;
        try { Self.BurstEmitter(OneShot ? Amount : 1); }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { }
    }
}
