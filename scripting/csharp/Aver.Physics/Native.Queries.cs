// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// P/Invokes for physics_queries_abi.h. Names are copied from the header character for character (the
// ABI parity test compares them); Queries.cs is the public surface.
using System.Runtime.InteropServices;

namespace Aver.Physics;

internal static partial class Native
{
    [DllImport(Lib)] internal static extern int aver_phys_box_cast(float cx, float cy, float cz, float hx, float hy, float hz, float qx, float qy, float qz, float qw, float dx, float dy, float dz, uint layerMask, int ignoreBody, int flags, out float outDistance, float[] outPoint, float[] outNormal);
    [DllImport(Lib)] internal static extern int aver_phys_capsule_cast(float cx, float cy, float cz, float radius, float height, float qx, float qy, float qz, float qw, float dx, float dy, float dz, uint layerMask, int ignoreBody, int flags, out float outDistance, float[] outPoint, float[] outNormal);
    [DllImport(Lib)] internal static extern int aver_phys_sphere_cast_q(float cx, float cy, float cz, float dx, float dy, float dz, float radius, uint layerMask, int ignoreBodyA, int ignoreBodyB, int flags, out float outDistance, float[] outPoint, float[] outNormal);
    [DllImport(Lib)] internal static extern int aver_phys_body_cast(int body, float dx, float dy, float dz, uint layerMask, int ignoreBody, int flags, out float outDistance, float[] outPoint, float[] outNormal);

    [DllImport(Lib)] internal static extern int aver_phys_overlap_box(float cx, float cy, float cz, float hx, float hy, float hz, float qx, float qy, float qz, float qw, uint layerMask, int ignoreBody, int flags, int[] outBodies, int maxCount);
    [DllImport(Lib)] internal static extern int aver_phys_overlap_capsule(float cx, float cy, float cz, float radius, float height, float qx, float qy, float qz, float qw, uint layerMask, int ignoreBody, int flags, int[] outBodies, int maxCount);
    [DllImport(Lib)] internal static extern int aver_phys_overlap_body(int body, float x, float y, float z, float qx, float qy, float qz, float qw, uint layerMask, int ignoreBody, int flags, int[] outBodies, int maxCount);

    [DllImport(Lib)] internal static extern int aver_phys_raycast_all(float ox, float oy, float oz, float dx, float dy, float dz, float maxDistCm, uint layerMask, int ignoreBody, int flags, int maxCount, int[] outBodies, float[] outDistances, float[] outPoints, float[] outNormals);
    [DllImport(Lib)] internal static extern int aver_phys_point_query(float x, float y, float z, uint layerMask, int ignoreBody, int flags, int[] outBodies, int maxCount);

    // Space-state queries.
    [DllImport(Lib)] internal static extern int aver_phys_space_raycast(float[] origin3, float[] direction3, float maxDistCm, uint collisionMask, int flags, int[]? excludeBodies, int excludeCount, out float outDistance, float[] outPoint, float[] outNormal, out int outEntity, out int outSubShape);
    [DllImport(Lib)] internal static extern int aver_phys_space_intersect_shape(int shape, float[] pose7, float marginCm, uint collisionMask, int flags, int[]? excludeBodies, int excludeCount, int maxResults, int[] outBodies, int[] outSubShapes);
    [DllImport(Lib)] internal static extern int aver_phys_space_collide_shape(int shape, float[] pose7, float marginCm, uint collisionMask, int flags, int[]? excludeBodies, int excludeCount, int maxContacts, int[] outBodies, float[] outPointsOnShape, float[] outPointsOnOther, float[] outNormals, float[] outDepths);
    [DllImport(Lib)] internal static extern int aver_phys_space_rest_info(int shape, float[] pose7, float marginCm, uint collisionMask, int flags, int[]? excludeBodies, int excludeCount, float[] outPoint, float[] outNormal, float[] outVelocity, out float outDepth, out int outEntity, out int outSubShape);
    [DllImport(Lib)] internal static extern int aver_phys_space_cast_motion(int shape, float[] pose7, float[] motion3, float marginCm, uint collisionMask, int flags, int[]? excludeBodies, int excludeCount, out float outSafe, out float outUnsafe, out int outBody, float[] outPoint, float[] outNormal);
    [DllImport(Lib)] internal static extern int aver_phys_body_test_motion(int body, float[]? originPose7, float[] motion3, float marginCm, int flags, int[]? excludeBodies, int excludeCount, int maxContacts, float[] outF, int[] outI, float[]? outContactF, int[]? outContactI);
}
