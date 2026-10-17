// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// The builder-style soft-body P/Invokes (physics_softbody_abi.h). A second part of Native, as Native.cs is,
// so the parity test in tests/abi checks these names against the header the same way; every name below is
// copied from the header character for character.
using System.Runtime.InteropServices;

namespace Aver.Physics;

internal static partial class Native
{
    // ---- Builder soft bodies (physics_softbody_abi.h) ---------------------------------------------

    [DllImport(Lib)] internal static extern int   aver_phys_sb_builder_begin(int vertexCount);
    [DllImport(Lib)] internal static extern int   aver_phys_sb_builder_vertices(int b, float[] restXyz, float[]? invMass, float[]? statePosXyz, float[]? stateVelXyz);
    [DllImport(Lib)] internal static extern int   aver_phys_sb_builder_faces(int b, int[] tri3, int triCount);
    [DllImport(Lib)] internal static extern int   aver_phys_sb_builder_edges(int b, int[] ab, float[] restCm, float[]? compliance, int edgeCount);
    [DllImport(Lib)] internal static extern int   aver_phys_sb_builder_bends(int b, int[] quad4, float[]? compliance, int bendCount);
    [DllImport(Lib)] internal static extern int   aver_phys_sb_builder_skin(int b, int[] joints4, float[] weights4, float[] reachCm, float backstopCm, int jointCount);
    [DllImport(Lib)] internal static extern int   aver_phys_sb_builder_params(int b, float[] p, int n);
    [DllImport(Lib)] internal static extern int   aver_phys_sb_builder_finish(int b, float cx, float cy, float cz, int replaceBody, int stateSrcBody, int[]? stateMap, int[]? outEdgeSlot, int[]? outBendSlot);
    [DllImport(Lib)] internal static extern int   aver_phys_sb_builder_cancel(int b);

    [DllImport(Lib)] internal static extern int   aver_phys_sb_state(int body, float[]? outPosXyz, float[]? outVelXyz, int maxVertices);
    [DllImport(Lib)] internal static extern int   aver_phys_sb_vertex_count(int body);
    [DllImport(Lib)] internal static extern int   aver_phys_sb_set_edge_rest(int body, int[] edgeSlot, float[] restCm, int n);
    [DllImport(Lib)] internal static extern int   aver_phys_sb_rest_bends_from_pose(int body, int[] bendSlot, int n);
    [DllImport(Lib)] internal static extern int   aver_phys_sb_impulse(int body, float[] centreCm, float radiusCm, float[] dirUnit, float speedCmS, float radialBlend, float falloffPower);
    [DllImport(Lib)] internal static extern int   aver_phys_sb_skin_world(int body, float[] jointMatrices, int jointCount, float[]? entityWorld16, int hardSkin);
    [DllImport(Lib)] internal static extern int   aver_phys_sb_set_active(int body, int active);
    [DllImport(Lib)] internal static extern int   aver_phys_sb_is_active(int body);
    [DllImport(Lib)] internal static extern int   aver_phys_sb_set_params(int body, float[] p, int n);
    [DllImport(Lib)] internal static extern int   aver_phys_sb_count();

    [DllImport(Lib)] internal static extern int   aver_phys_sb_set_aero(int body, float drag, float lift, int flags);
    [DllImport(Lib)] internal static extern void  aver_phys_sb_set_wind(float vx, float vy, float vz, float gustSpeed, float gustHz, float turbulence);

    [DllImport(Lib)] internal static extern int   aver_phys_sb_impact_count();
    [DllImport(Lib)] internal static extern int   aver_phys_sb_impact_get(int index, out int outSoftBody, out int outOtherBody, float[] outPoint3, float[] outNormal3, out float outClosingCmS, out float outOtherMassKg, out int outContactVertices);
}
