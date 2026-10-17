// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// Builder-style soft bodies (physics_softbody_abi.h): cloth and destructibles that can dent, tear and be rebuilt
// under the same handle. A thin wrapper for tools and tests; game scripts use Entity and SoftBodies in
// Aver.Framework. Units are engine centimetres and engine axes throughout.

namespace Aver.Physics;

/// <summary>Indices of the parameter array (AVER_PHYS_SB_P_*, physics_softbody_abi.h). A NaN entry means
/// "leave as is".</summary>
public enum SoftBodyParam
{
    Iterations = 0,
    Damping = 1,
    Friction = 2,
    Restitution = 3,
    GravityScale = 4,
    VertexRadiusCm = 5,
    MaxSpeedCms = 6,
    AllowSleep = 7,
    DoubleSided = 8,
    Layer = 9,
    Entity = 10,
    Impacts = 11,
}

/// <summary>One contact between a soft body and a rigid body, as reported after a step.</summary>
public readonly record struct SoftBodyImpact(int SoftBody, int OtherBody, Float3 Point, Float3 Normal,
                                             float ClosingCmPerS, float OtherMassKg, int ContactVertices);

/// <summary>Stages the arrays of one soft body and makes it. Disposing without <see cref="Finish"/> cancels.</summary>
public sealed class SoftBodyBuilder : IDisposable
{
    private const int ParamCount = 12;
    private int _id;
    private readonly float[] _params = new float[ParamCount];
    private int _edgeCount, _bendCount;

    /// <summary>The Jolt edge and bend index of each staged edge and bend, filled by <see cref="Finish"/>.</summary>
    public int[] EdgeSlot { get; private set; } = Array.Empty<int>();
    public int[] BendSlot { get; private set; } = Array.Empty<int>();

    public SoftBodyBuilder(int vertexCount)
    {
        _id = Native.aver_phys_sb_builder_begin(vertexCount);
        Array.Fill(_params, float.NaN);
    }

    public bool IsValid => _id != 0;

    public bool Vertices(float[] restXyz, float[]? invMass = null, float[]? statePosXyz = null, float[]? stateVelXyz = null) =>
        _id != 0 && Native.aver_phys_sb_builder_vertices(_id, restXyz, invMass, statePosXyz, stateVelXyz) != 0;

    public bool Faces(int[] tri3) => _id != 0 && Native.aver_phys_sb_builder_faces(_id, tri3, tri3.Length / 3) != 0;

    public bool Edges(int[] ab, float[] restCm, float[]? compliance = null)
    {
        _edgeCount = restCm.Length;
        return _id != 0 && Native.aver_phys_sb_builder_edges(_id, ab, restCm, compliance, restCm.Length) != 0;
    }

    public bool Bends(int[] quad4, float[]? compliance = null)
    {
        _bendCount = quad4.Length / 4;
        return _id != 0 && Native.aver_phys_sb_builder_bends(_id, quad4, compliance, quad4.Length / 4) != 0;
    }

    public bool Skin(int[] joints4, float[] weights4, float[] reachCm, float backstopCm, int jointCount) =>
        _id != 0 && Native.aver_phys_sb_builder_skin(_id, joints4, weights4, reachCm, backstopCm, jointCount) != 0;

    public void Set(SoftBodyParam param, float value) => _params[(int)param] = value;

    /// <summary>Makes the body and returns its handle, or 0 on refusal. <paramref name="replaceBody"/> keeps that
    /// handle; <paramref name="stateSrcBody"/> carries vertex state through <paramref name="stateMap"/>
    /// (new vertex to old vertex, -1 = rest).</summary>
    public int Finish(Float3 centre, int replaceBody = 0, int stateSrcBody = 0, int[]? stateMap = null)
    {
        if (_id == 0) return 0;
        Native.aver_phys_sb_builder_params(_id, _params, _params.Length);
        EdgeSlot = new int[_edgeCount];
        BendSlot = new int[_bendCount];
        int handle = Native.aver_phys_sb_builder_finish(_id, centre.X, centre.Y, centre.Z, replaceBody, stateSrcBody, stateMap,
                                                         EdgeSlot.Length > 0 ? EdgeSlot : null, BendSlot.Length > 0 ? BendSlot : null);
        if (handle != 0) _id = 0;
        return handle;
    }

    public void Dispose()
    {
        if (_id != 0) { Native.aver_phys_sb_builder_cancel(_id); _id = 0; }
    }
}

/// <summary>A body made by <see cref="SoftBodyBuilder"/>: state, in-place edits, skin, aero, activation.</summary>
public readonly struct SoftBodyHandle
{
    public int Handle { get; }
    public SoftBodyHandle(int handle) { Handle = handle; }

    public bool IsValid => Handle != 0 && Native.aver_phys_sb_vertex_count(Handle) > 0;
    public int VertexCount => Native.aver_phys_sb_vertex_count(Handle);

    /// <summary>World positions (cm) and velocities (cm/s), 3 floats per vertex. Returns the vertices written.</summary>
    public int State(float[]? positions, float[]? velocities, int maxVertices) =>
        Native.aver_phys_sb_state(Handle, positions, velocities, maxVertices);

    /// <summary>A dent: the rest length of each listed edge slot.</summary>
    public bool SetEdgeRest(int[] edgeSlot, float[] restCm) => Native.aver_phys_sb_set_edge_rest(Handle, edgeSlot, restCm, edgeSlot.Length) != 0;

    /// <summary>Takes the current pose as the rest angle of the listed bend slots.</summary>
    public bool RestBendsFromPose(int[] bendSlot) => Native.aver_phys_sb_rest_bends_from_pose(Handle, bendSlot, bendSlot.Length) != 0;

    public bool Impulse(Float3 centre, float radiusCm, Float3 direction, float speedCmPerS, float radialBlend = 0, float falloffPower = 1) =>
        Native.aver_phys_sb_impulse(Handle, centre.ToArray(), radiusCm, direction.ToArray(), speedCmPerS, radialBlend, falloffPower) != 0;

    public bool SkinWorld(float[] jointMatrices, int jointCount, float[]? entityWorld16, bool hardSkin) =>
        Native.aver_phys_sb_skin_world(Handle, jointMatrices, jointCount, entityWorld16, hardSkin ? 1 : 0) != 0;

    public bool SetActive(bool active) => Native.aver_phys_sb_set_active(Handle, active ? 1 : 0) != 0;
    public bool IsActive => Native.aver_phys_sb_is_active(Handle) != 0;

    public bool SetParams(float[] p) => Native.aver_phys_sb_set_params(Handle, p, p.Length) != 0;
    public bool SetAero(float drag, float lift, bool keepAwake = false) => Native.aver_phys_sb_set_aero(Handle, drag, lift, keepAwake ? 1 : 0) != 0;

    /// <summary>The world wind, cm/s.</summary>
    public static void SetWind(Float3 velocity, float gustSpeed = 0, float gustHz = 0.3f, float turbulence = 0) =>
        Native.aver_phys_sb_set_wind(velocity.X, velocity.Y, velocity.Z, gustSpeed, gustHz, turbulence);

    /// <summary>Impacts reported by the last <c>Physics.Step</c> (only bodies built with the Impacts parameter).</summary>
    public static int DrainImpacts(Span<SoftBodyImpact> buffer)
    {
        int n = Math.Min(buffer.Length, Native.aver_phys_sb_impact_count());
        var point = new float[3];
        var normal = new float[3];
        for (int i = 0; i < n; i++)
        {
            if (Native.aver_phys_sb_impact_get(i, out int soft, out int other, point, normal, out float closing, out float mass, out int verts) == 0)
                return i;
            buffer[i] = new SoftBodyImpact(soft, other, new Float3(point[0], point[1], point[2]),
                                           new Float3(normal[0], normal[1], normal[2]), closing, mass, verts);
        }
        return n;
    }
}
