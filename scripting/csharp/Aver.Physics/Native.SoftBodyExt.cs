// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// The soft-body P/Invokes of physics_softbody_ext_abi.h. A further part of Native, as Native.VehicleExt.cs
// is, so the parity test in tests/abi checks every name and parameter name below against the header; they are copied
// from it character for character.
using System.Runtime.InteropServices;

namespace Aver.Physics;

internal static partial class Native
{
    // ---- Soft bodies, the rest of Jolt (physics_softbody_ext_abi.h) --------------------------------------

    [DllImport(Lib)] internal static extern int aver_phys_sbx_abi_version();

    [DllImport(Lib)] internal static extern int aver_phys_sbx_builder_volumes(int b, int[] tet4, float[]? compliance, int count);
    [DllImport(Lib)] internal static extern int aver_phys_sbx_builder_lra(int b, int[] pairs2, float[] maxDistCm, int count);
    [DllImport(Lib)] internal static extern int aver_phys_sbx_builder_lra_auto(int b, int mode, float distanceMultiplier);
    [DllImport(Lib)] internal static extern int aver_phys_sbx_builder_rods(int b, int[] ab, float[]? stretchCompliance, int count, int[] rodPairs2, float[]? bendTwistCompliance, int pairCount);
    [DllImport(Lib)] internal static extern int aver_phys_sbx_builder_rod_chain(int b, int firstVertex, int vertexCount, float stretchCompliance, float bendTwistCompliance);
    [DllImport(Lib)] internal static extern int aver_phys_sbx_builder_auto(int b, float[] attrs5, int attrCount, int bendType, float angleToleranceDeg);
    [DllImport(Lib)] internal static extern int aver_phys_sbx_builder_params(int b, float[] p, int n);
    [DllImport(Lib)] internal static extern int aver_phys_sbx_slots(int body, int kind, int[]? outSlots, int maxCount);

    [DllImport(Lib)] internal static extern int aver_phys_sbx_set_pressure(int body, float pressureKPa);
    [DllImport(Lib)] internal static extern int aver_phys_sbx_get_pressure(int body, out float outKPa);
    [DllImport(Lib)] internal static extern int aver_phys_sbx_volume(int body, out float outRestCm3, out float outNowCm3);
    [DllImport(Lib)] internal static extern int aver_phys_sbx_set_skin(int body, int enabled, float distanceScale);
    [DllImport(Lib)] internal static extern int aver_phys_sbx_set_inv_mass(int body, int[] vertex, float[] invMass, int n);

    [DllImport(Lib)] internal static extern int aver_phys_sbx_attach(int body, int vertex, float[] targetXyzCm, float maxSpeedCmS);
    [DllImport(Lib)] internal static extern int aver_phys_sbx_attach_to_body(int body, int vertex, int otherBody, float[] localOffsetXyzCm, float maxSpeedCmS);
    [DllImport(Lib)] internal static extern int aver_phys_sbx_move_attachment(int body, int vertex, float[] targetXyzCm);
    [DllImport(Lib)] internal static extern int aver_phys_sbx_detach(int body, int vertex, float restoreInvMass);
    [DllImport(Lib)] internal static extern int aver_phys_sbx_attachment_count(int body);

    [DllImport(Lib)] internal static extern int aver_phys_sbx_set_contact_policy(int body, float softInvMassScale, float otherInvMassScale, float otherInvInertiaScale);
    [DllImport(Lib)] internal static extern int aver_phys_sbx_set_pair_policy(int body, int otherBody, int mode, float softInvMassScale, float otherInvMassScale, float otherInvInertiaScale);
    [DllImport(Lib)] internal static extern int aver_phys_sbx_clear_pair_policy(int body, int otherBody);
    [DllImport(Lib)] internal static extern int aver_phys_sbx_sensor_count();
    [DllImport(Lib)] internal static extern int aver_phys_sbx_sensor_get(int index, out int outSoftBody, out int outSensorBody, out int outEntered);

    [DllImport(Lib)] internal static extern int aver_phys_sbx_set_collision_group(int body, int group);

    [DllImport(Lib)] internal static extern int aver_phys_sbx_set_buoyancy(int body, float fluidDensityKgM3, float drag, int flags);
    [DllImport(Lib)] internal static extern int aver_phys_sbx_set_water_plane(int body, float[] pointXyzCm, float[] normalXyz, float[]? currentXyzCmS);
    [DllImport(Lib)] internal static extern int aver_phys_sbx_submerged(int body, out float outFraction01, out float outVolumeCm3);

    [DllImport(Lib)] internal static extern int aver_phys_sbx_rod_state(int body, int firstRod, int count, float[]? outQuatXyzw, float[]? outAngVelRadS);
    [DllImport(Lib)] internal static extern int aver_phys_sbx_rod_count(int body);
}
