// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// Strand hair on Jolt's Hair (physics_hair_abi.h): the settings builder, the settings and instance handles and the index blocks of
// the parameter arrays. A thin wrapper for tools and tests; game scripts use Entity.AddHair and the Hair node family in Aver.Framework.
// Units are engine centimetres and engine axes throughout, hair space being the space of the scalp mesh.

using System.Runtime.CompilerServices;

namespace Aver.Physics;

/// <summary>Indices of the settings parameter array (AVER_PHYS_HAIRP_*). NaN = leave as is.</summary>
public enum HairParam
{
    IterationsPerSecond = 0,
    MaxDeltaTime = 1,
    GridX = 2,
    GridY = 3,
    GridZ = 4,
    BoundsPaddingCm = 5,
    SimFraction = 6,
    InitialGravityX = 7,
    InitialGravityY = 8,
    InitialGravityZ = 9,
    /// <summary>How many settings parameters there are (AVER_PHYS_HAIRP_COUNT); not one.</summary>
    Count = 10,
}

/// <summary>Indices of a material's parameter block (AVER_PHYS_HAIRM_*). A gradient takes four floats from its index: min, max, min
/// fraction, max fraction.</summary>
public enum HairMaterialParam
{
    EnableCollision = 0,
    EnableLra = 1,
    LinearDamping = 2,
    AngularDamping = 3,
    MaxLinearCms = 4,
    MaxAngularRads = 5,
    Gravity = 6,
    Friction = 10,
    BendCompliance = 11,
    BendMultiplier = 12,
    StretchCompliance = 16,
    InertiaMultiplier = 17,
    RadiusCm = 18,
    WorldInfluence = 22,
    GridVelocity = 26,
    GridDensityForce = 30,
    GlobalPose = 31,
    SkinGlobalPose = 35,
    SimFraction = 39,
    GravityPreload = 40,
    /// <summary>How many floats a material block holds (AVER_PHYS_HAIRM_COUNT); not one.</summary>
    Count = 41,
}

/// <summary>Integer facts about a settings object (AVER_PHYS_HAIRI_*).</summary>
public enum HairInfoInt
{
    SimStrands = 0,
    SimVertices = 1,
    RenderStrands = 2,
    RenderVertices = 3,
    MaxVerticesPerStrand = 4,
    Materials = 5,
    GridCells = 6,
    ScalpJoints = 7,
    Count = 8,
}

/// <summary>Float facts about a settings object (AVER_PHYS_HAIRF_*).</summary>
public enum HairInfoFloat
{
    RootToScalpCm = 0,
    BoundsMinX = 1,
    BoundsMinY = 2,
    BoundsMinZ = 3,
    BoundsMaxX = 4,
    BoundsMaxY = 5,
    BoundsMaxZ = 6,
    Count = 7,
}

/// <summary>Integer facts about the last update of a hair (AVER_PHYS_HAIRUI_*).</summary>
public enum HairUpdateInt
{
    Iterations = 0,
    Dispatches = 1,
    Updates = 2,
    Wind = 3,
    Count = 4,
}

/// <summary>Float facts about the last update of a hair (AVER_PHYS_HAIRUF_*).</summary>
public enum HairUpdateFloat
{
    DtUsed = 0,
    Count = 1,
}

/// <summary>The kinds of buffer a compute host creates (AVER_PHYS_CB_*).</summary>
public enum HairBufferType
{
    Upload = 0,
    Readback = 1,
    Constant = 2,
    Buffer = 3,
    Rw = 4,
}

/// <summary>How a compute host binds a buffer (AVER_PHYS_BIND_*).</summary>
public enum HairBindRole
{
    Constant = 0,
    Read = 1,
    Rw = 2,
}

/// <summary>The engine side of hair that is not an instance: the ABI version and which compute backend runs the kernels.</summary>
public static class HairPhysics
{
    public static int AbiVersion => Native.aver_phys_hair_abi_version();

    /// <summary>0 = Jolt's CPU backend, 1 = a host installed by the composition root.</summary>
    public static int HostKind => Native.aver_phys_hair_host_kind();

    /// <summary>Back to Jolt's CPU backend. Refused while any settings object exists.</summary>
    public static bool UseCpuHost()
    {
        return Native.aver_phys_hair_set_compute_host(ref Unsafe.NullRef<AverPhysComputeHostV1>()) != 0;
    }

    /// <summary>Executes and waits for everything this tick's updates recorded. Once per tick, after all updates.</summary>
    public static bool Flush() => Native.aver_phys_hair_flush() != 0;
}

/// <summary>Stages a groom for <see cref="Finish"/>: strands, the scalp they grow from, materials and parameters.</summary>
public sealed class HairSettingsBuilder
{
    private int _id;

    public HairSettingsBuilder()
    {
        _id = Native.aver_phys_hair_settings_begin();
    }

    public bool IsOpen => _id != 0;

    /// <summary>Render strands: points in cm (3 per vertex, scalp-mesh space), the first vertex of each strand in strandStart (and one past
    /// the last), a material per strand and an optional inverse mass per vertex. Every strand has 3 to 255 vertices.</summary>
    public bool SetStrands(float[] pointsXyz, int[] strandStart, int[]? strandMaterial = null, float[]? invMass = null)
    {
        if (_id == 0 || strandStart.Length < 2) return false;
        return Native.aver_phys_hair_settings_strands(_id, pointsXyz, invMass, pointsXyz.Length / 3, strandStart, strandMaterial, strandStart.Length - 1) != 0;
    }

    /// <summary>The scalp mesh the roots attach to. Joints and weights (4 per vertex) are indices into the scalp joint list; both null
    /// skins everything to one joint. A joint count of 0 counts as 1.</summary>
    public bool SetScalp(float[] vertsXyz, int[] tri3, int[]? joints4 = null, float[]? weights4 = null, int jointCount = 1)
    {
        return _id != 0 && Native.aver_phys_hair_settings_scalp(_id, vertsXyz, vertsXyz.Length / 3, tri3, tri3.Length / 3, joints4, weights4, jointCount) != 0;
    }

    /// <summary>One material's parameters, indexed by <see cref="HairMaterialParam"/>. NaN entries keep the default.</summary>
    public bool SetMaterial(int index, float[] p) => _id != 0 && Native.aver_phys_hair_settings_material(_id, index, p, p.Length) != 0;

    /// <summary>The settings' parameters, indexed by <see cref="HairParam"/>. NaN entries keep the default.</summary>
    public bool SetParams(float[] p) => _id != 0 && Native.aver_phys_hair_settings_params(_id, p, p.Length) != 0;

    /// <summary>Splits render and simulated strands and creates the compute buffers. Returns the settings handle, 0 on refusal; the builder is
    /// closed either way. Resample 0 keeps every strand as it is. Slow for a large groom: call it from a worker.</summary>
    public HairSettingsHandle Finish(int resampleVertices, out float rootToScalpCm)
    {
        rootToScalpCm = 0;
        if (_id == 0) return default;
        int s = _id;
        _id = 0;
        return new HairSettingsHandle(Native.aver_phys_hair_settings_finish(s, resampleVertices, out rootToScalpCm));
    }

    public void Cancel()
    {
        if (_id != 0) Native.aver_phys_hair_settings_cancel(_id);
        _id = 0;
    }
}

/// <summary>A finished groom, shared by any number of instances.</summary>
public readonly struct HairSettingsHandle
{
    public int Handle { get; }
    public HairSettingsHandle(int handle) { Handle = handle; }
    public bool IsValid => Handle != 0;

    /// <summary>Refused while an instance uses it.</summary>
    public bool Destroy() => Native.aver_phys_hair_settings_destroy(Handle) != 0;

    /// <summary>Live: the next update reads the new values.</summary>
    public bool SetMaterial(int index, float[] p) => Native.aver_phys_hair_settings_set_material(Handle, index, p, p.Length) != 0;

    public int[] InfoInts()
    {
        var v = new int[(int)HairInfoInt.Count];
        Native.aver_phys_hair_settings_info(Handle, v, v.Length, null, 0);
        return v;
    }

    public float[] InfoFloats()
    {
        var v = new float[(int)HairInfoFloat.Count];
        Native.aver_phys_hair_settings_info(Handle, null, 0, v, v.Length);
        return v;
    }

    /// <summary>{first render vertex, vertex count} per render strand, in the order the render positions buffer holds them.</summary>
    public int[] RenderStrands()
    {
        int n = Native.aver_phys_hair_settings_render_strands(Handle, null, 0);
        var v = new int[n * 2];
        if (n > 0) Native.aver_phys_hair_settings_render_strands(Handle, v, n);
        return v;
    }

    /// <summary>A hair of this groom. World16 is the hair space to world matrix, row-major, engine convention; layer is a user layer 0..15.</summary>
    public HairHandle Create(float[] world16, int layer = 0) => new HairHandle(Native.aver_phys_hair_create(Handle, world16, layer));
}

/// <summary>A hair instance.</summary>
public readonly struct HairHandle
{
    public int Handle { get; }
    public HairHandle(int handle) { Handle = handle; }
    public bool IsValid => Handle != 0;

    public bool Destroy() => Native.aver_phys_hair_destroy(Handle) != 0;
    public bool SetTransform(float[] world16) => Native.aver_phys_hair_set_transform(Handle, world16) != 0;

    /// <summary>The default pose and zero velocity at the next update.</summary>
    public bool Teleport() => Native.aver_phys_hair_teleport(Handle) != 0;

    /// <summary>The engine skinning matrices (model x inverse bind) of the scalp joints, in scalp joint order, 16 floats each.</summary>
    public bool SetPalette(float[] joints16) => Native.aver_phys_hair_set_palette(Handle, joints16, joints16.Length / 16) != 0;

    /// <summary>World wind in cm/s, a drag rate in 1/s and a turbulence amplitude in cm/s.</summary>
    public bool SetWind(float x, float y, float z, float dragPerS, float turbulenceCmS = 0) =>
        Native.aver_phys_hair_set_wind(Handle, new[] { x, y, z }, dragPerS, turbulenceCmS) != 0;

    /// <summary>Records one step; call <see cref="HairPhysics.Flush"/> once after all hair has been updated.</summary>
    public bool Update(float dt) => Native.aver_phys_hair_update(Handle, dt) != 0;

    /// <summary>The host handle of the render positions buffer, 0 with the CPU backend.</summary>
    public ulong RenderBuffer => Native.aver_phys_hair_render_buffer(Handle);

    public int[] UpdateInts()
    {
        var v = new int[(int)HairUpdateInt.Count];
        Native.aver_phys_hair_info(Handle, v, v.Length, null, 0);
        return v;
    }

    public float[] UpdateFloats()
    {
        var v = new float[(int)HairUpdateFloat.Count];
        Native.aver_phys_hair_info(Handle, null, 0, v, v.Length);
        return v;
    }

    /// <summary>Debug and tests: the simulated vertices and the render positions in cm, hair space, engine axes. Slow.</summary>
    public bool ReadPositions(float[]? sim, float[]? render) =>
        Native.aver_phys_hair_read_positions(Handle, sim, sim == null ? 0 : sim.Length / 3, render, render == null ? 0 : render.Length / 3) != 0;
}
