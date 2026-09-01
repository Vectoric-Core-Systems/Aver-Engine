// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
//
// The P/Invoke surface: one extern per aver_phys_* export, across all five native headers. Nothing
// here is meant to be called directly by a game script -- Physics/Body/Joint/Shapes/CharacterBody are
// that surface. This file exists so the parameter list can be checked mechanically against the C
// headers it mirrors (tests/abi/src/AbiEnumTest.cpp), which is also why every name below is copied
// from its header character for character rather than renamed to a C# convention: the check compares
// spelling, and a friendlier name here is a rename the checker cannot tell from a swapped argument.
//
// A LOCAL DllImport, not Aver.Framework's Fw class: this targets a different native DLL
// ("Aver.Physics", the physics module's own export name) from the framework's.
using System.Runtime.InteropServices;

namespace Aver.Physics;

internal static class Native
{
    private const string Lib = "Aver.Physics";

    // ---- World (physics_abi.h) --------------------------------------------------------------------

    [DllImport(Lib)] internal static extern int   aver_phys_init();
    [DllImport(Lib)] internal static extern void  aver_phys_shutdown();
    [DllImport(Lib)] internal static extern int   aver_phys_ready();
    [DllImport(Lib)] internal static extern void  aver_phys_set_gravity(float x, float y, float z);
    [DllImport(Lib)] internal static extern int   aver_phys_step(float dt);
    [DllImport(Lib)] internal static extern float aver_phys_fixed_step();
    [DllImport(Lib)] internal static extern int   aver_phys_set_fixed_step(float seconds);

    // ---- Bodies -------------------------------------------------------------------------------------

    [DllImport(Lib)] internal static extern int aver_phys_add_static_box(float cx, float cy, float cz, float hx, float hy, float hz);
    [DllImport(Lib)] internal static extern int aver_phys_add_dynamic_box(float cx, float cy, float cz, float hx, float hy, float hz, float massKg);
    [DllImport(Lib)] internal static extern int aver_phys_add_dynamic_sphere(float cx, float cy, float cz, float radius, float massKg);
    [DllImport(Lib)] internal static extern int aver_phys_remove_body(int body);
    [DllImport(Lib)] internal static extern int aver_phys_body_position(int body, float[] outXyz);
    [DllImport(Lib)] internal static extern int aver_phys_body_rotation(int body, float[] outQuat);
    [DllImport(Lib)] internal static extern int aver_phys_body_velocity(int body, float[] outXyz);
    [DllImport(Lib)] internal static extern int aver_phys_body_set_position(int body, float x, float y, float z);
    [DllImport(Lib)] internal static extern int aver_phys_body_set_velocity(int body, float x, float y, float z);
    [DllImport(Lib)] internal static extern int aver_phys_body_count();

    // ---- Body dynamics --------------------------------------------------------------------------------

    [DllImport(Lib)] internal static extern int aver_phys_body_set_motion_type(int body, int motionType);
    [DllImport(Lib)] internal static extern int aver_phys_body_motion_type(int body);
    [DllImport(Lib)] internal static extern int aver_phys_body_set_rotation(int body, float x, float y, float z, float w);
    [DllImport(Lib)] internal static extern int aver_phys_body_angular_velocity(int body, float[] outXyz);
    [DllImport(Lib)] internal static extern int aver_phys_body_set_angular_velocity(int body, float wx, float wy, float wz);
    [DllImport(Lib)] internal static extern int aver_phys_body_add_velocity(int body, float vx, float vy, float vz);

    // ---- Forces and impulses ---------------------------------------------------------------------------

    [DllImport(Lib)] internal static extern int aver_phys_body_add_force(int body, float fx, float fy, float fz);
    [DllImport(Lib)] internal static extern int aver_phys_body_add_force_at(int body, float fx, float fy, float fz, float px, float py, float pz);
    [DllImport(Lib)] internal static extern int aver_phys_body_add_torque(int body, float tx, float ty, float tz);
    [DllImport(Lib)] internal static extern int aver_phys_body_add_impulse(int body, float ix, float iy, float iz);
    [DllImport(Lib)] internal static extern int aver_phys_body_add_impulse_at(int body, float ix, float iy, float iz, float px, float py, float pz);
    [DllImport(Lib)] internal static extern int aver_phys_body_add_angular_impulse(int body, float ax, float ay, float az);

    // ---- Material and mass ------------------------------------------------------------------------------

    [DllImport(Lib)] internal static extern int aver_phys_body_set_friction(int body, float friction);
    [DllImport(Lib)] internal static extern int aver_phys_body_friction(int body, float[] outFriction);
    [DllImport(Lib)] internal static extern int aver_phys_body_set_restitution(int body, float restitution);
    [DllImport(Lib)] internal static extern int aver_phys_body_restitution(int body, float[] outRestitution);
    [DllImport(Lib)] internal static extern int aver_phys_body_set_gravity_factor(int body, float factor);
    [DllImport(Lib)] internal static extern int aver_phys_body_gravity_factor(int body, float[] outFactor);
    [DllImport(Lib)] internal static extern int aver_phys_body_set_damping(int body, float linear, float angular);
    [DllImport(Lib)] internal static extern int aver_phys_body_damping(int body, float[] outLinear, float[] outAngular);
    [DllImport(Lib)] internal static extern int aver_phys_body_set_mass(int body, float massKg);
    [DllImport(Lib)] internal static extern int aver_phys_body_mass(int body, float[] outMassKg);

    // ---- Sleeping ---------------------------------------------------------------------------------------

    [DllImport(Lib)] internal static extern int aver_phys_body_activate(int body);
    [DllImport(Lib)] internal static extern int aver_phys_body_deactivate(int body);
    [DllImport(Lib)] internal static extern int aver_phys_body_is_active(int body);

    // ---- Character (physics_abi.h) -----------------------------------------------------------------

    [DllImport(Lib)] internal static extern int aver_phys_character_create(float radius, float height, float x, float y, float z);
    [DllImport(Lib)] internal static extern int aver_phys_character_destroy(int ch);
    [DllImport(Lib)] internal static extern int aver_phys_character_set_velocity(int ch, float vx, float vy, float vz);
    [DllImport(Lib)] internal static extern int aver_phys_character_velocity(int ch, float[] outXyz);
    [DllImport(Lib)] internal static extern int aver_phys_character_position(int ch, float[] outXyz);
    [DllImport(Lib)] internal static extern int aver_phys_character_set_position(int ch, float x, float y, float z);
    [DllImport(Lib)] internal static extern int aver_phys_character_grounded(int ch);

    // ---- Entity association ---------------------------------------------------------------------------

    [DllImport(Lib)] internal static extern int aver_phys_set_entity(int handle, int entity);

    // ---- Arbitrary collision geometry ------------------------------------------------------------------

    [DllImport(Lib)] internal static extern int aver_phys_add_convex_hull(float[] pointsXyz, int count, float cx, float cy, float cz, int dynamic, float massKg);
    [DllImport(Lib)] internal static extern int aver_phys_add_mesh(float[] verticesXyz, int vertexCount, int[] indices, int indexCount, float cx, float cy, float cz);
    [DllImport(Lib)] internal static extern int aver_phys_add_heightfield(float[] samples, int sampleCount, float spacingCm, float cx, float cy, float cz);

    // ---- Sensors (triggers) --------------------------------------------------------------------------

    [DllImport(Lib)] internal static extern int aver_phys_add_sensor_box(float cx, float cy, float cz, float hx, float hy, float hz);
    [DllImport(Lib)] internal static extern int aver_phys_add_sensor_sphere(float cx, float cy, float cz, float radius);

    // ---- Buoyancy -----------------------------------------------------------------------------------

    [DllImport(Lib)] internal static extern int  aver_phys_set_water_plane(float heightCm, float[] normalUnit, float buoyancy, float linearDrag, float angularDrag, float[] fluidVelocityCmS);
    [DllImport(Lib)] internal static extern void aver_phys_clear_water_plane();
    [DllImport(Lib)] internal static extern int  aver_phys_water_plane(float[]? outHeightCm);
    [DllImport(Lib)] internal static extern int  aver_phys_set_water_volume(int body, float[] surfacePosCm, float[] surfaceNormalUnit, float buoyancy, float linearDrag, float angularDrag, float[] fluidVelocityCmS);
    [DllImport(Lib)] internal static extern int  aver_phys_clear_water_volume(int body);
    [DllImport(Lib)] internal static extern int  aver_phys_buoyant_body_count();

    // ---- Contact and overlap events ------------------------------------------------------------------

    [DllImport(Lib)] internal static extern int aver_phys_contact_count();
    [DllImport(Lib)] internal static extern int aver_phys_contact_get(int index, out int outBodyA, out int outBodyB, float[] outPoint, float[] outNormal);
    [DllImport(Lib)] internal static extern int aver_phys_overlap_count();
    [DllImport(Lib)] internal static extern int aver_phys_overlap_get(int index, out int outSensor, out int outBody, out int outEntered);

    // ---- Soft bodies ------------------------------------------------------------------------------------

    [DllImport(Lib)] internal static extern int aver_phys_softbody_create(float[] verticesXyz, int vertexCount, int[] indices, int indexCount, float[]? invMasses, float cx, float cy, float cz, float compliance, float pressure, float damping, int iterations);
    [DllImport(Lib)] internal static extern int aver_phys_softbody_create_skinned(float[] verticesXyz, int vertexCount, int[] indices, int indexCount, float[]? invMasses, int[] jointIndices, float[] jointWeights, int influences, int jointCount, float maxDistanceCm, float backStopDistanceCm, float cx, float cy, float cz, float compliance);
    [DllImport(Lib)] internal static extern int aver_phys_softbody_skin(int body, float[] jointMatrices, int jointCount, int hardSkin);
    [DllImport(Lib)] internal static extern int aver_phys_softbody_vertex_count(int body);
    [DllImport(Lib)] internal static extern int aver_phys_softbody_vertices(int body, float[] outXyz, int maxVertices);
    [DllImport(Lib)] internal static extern int aver_phys_softbody_apply_impulse(int body, float[] centreCm, float radiusCm, float[] velocityCmPerS, float strength);

    // ---- Queries -------------------------------------------------------------------------------------

    [DllImport(Lib)] internal static extern int aver_phys_raycast(float ox, float oy, float oz, float dx, float dy, float dz, float maxDistCm, float[] outPoint, float[] outNormal, out int outEntity);
    [DllImport(Lib)] internal static extern int aver_phys_overlap_sphere(float x, float y, float z, float radius, int[] outBodies, int maxBodies);
    [DllImport(Lib)] internal static extern int aver_phys_sphere_cast(float ox, float oy, float oz, float dx, float dy, float dz, float maxDistCm, float radius, float[] outPoint, float[] outNormal);

    // ---- Joints (physics_joints_abi.h) -----------------------------------------------------------------

    [DllImport(Lib)] internal static extern int aver_phys_joint_remove(int joint);
    [DllImport(Lib)] internal static extern int aver_phys_joint_count();
    [DllImport(Lib)] internal static extern int aver_phys_joint_bodies(int joint, int[]? outBodyA, int[]? outBodyB);
    [DllImport(Lib)] internal static extern int aver_phys_joint_set_enabled(int joint, int enabled);
    [DllImport(Lib)] internal static extern int aver_phys_joint_enabled(int joint);

    [DllImport(Lib)] internal static extern int aver_phys_joint_fixed(int bodyA, int bodyB, float[] pointCm, float[] axisX, float[] axisY);
    [DllImport(Lib)] internal static extern int aver_phys_joint_point(int bodyA, int bodyB, float[] pointCm);
    [DllImport(Lib)] internal static extern int aver_phys_joint_distance(int bodyA, int bodyB, float[] pointACm, float[] pointBCm, float minDistanceCm, float maxDistanceCm);
    [DllImport(Lib)] internal static extern int aver_phys_joint_hinge(int bodyA, int bodyB, float[] pointCm, float[] hingeAxis, float[] normalAxis, float minAngleRad, float maxAngleRad);
    [DllImport(Lib)] internal static extern int aver_phys_joint_slider(int bodyA, int bodyB, float[] pointCm, float[] sliderAxis, float[] normalAxis, float minCm, float maxCm);
    [DllImport(Lib)] internal static extern int aver_phys_joint_cone(int bodyA, int bodyB, float[] pointCm, float[] twistAxis, float halfConeAngleRad);
    [DllImport(Lib)] internal static extern int aver_phys_joint_swing_twist(int bodyA, int bodyB, float[] pointCm, float[] twistAxis, float[] planeAxis, float normalHalfConeRad, float planeHalfConeRad, float twistMinRad, float twistMaxRad);
    [DllImport(Lib)] internal static extern int aver_phys_joint_six_dof(int bodyA, int bodyB, float[] pointCm, float[] axisX, float[] axisY, float[] limitMin, float[] limitMax);
    [DllImport(Lib)] internal static extern int aver_phys_joint_gear(int bodyA, int bodyB, float[] hingeAxisA, float[] hingeAxisB, float ratio);
    [DllImport(Lib)] internal static extern int aver_phys_joint_rack_and_pinion(int bodyA, int bodyB, float[] hingeAxisA, float[] sliderAxisB, float ratioRadPerCm);
    [DllImport(Lib)] internal static extern int aver_phys_joint_pulley(int bodyA, int bodyB, float[] bodyPointACm, float[] fixedPointACm, float[] bodyPointBCm, float[] fixedPointBCm, float ratio, float minLengthCm, float maxLengthCm);
    [DllImport(Lib)] internal static extern int aver_phys_joint_path(int bodyA, int bodyB, float[] pointsCm, int pointCount, int closed, float maxSlideCm);

    [DllImport(Lib)] internal static extern int aver_phys_joint_set_motor(int joint, int axis, int state, float target);
    [DllImport(Lib)] internal static extern int aver_phys_joint_set_motor_strength(int joint, int axis, float maxForceOrTorque);
    [DllImport(Lib)] internal static extern int aver_phys_joint_set_limits(int joint, int axis, float minimum, float maximum);
    [DllImport(Lib)] internal static extern int aver_phys_joint_value(int joint, float[] outValue);

    // ---- Shapes (physics_shapes_abi.h) -----------------------------------------------------------------

    [DllImport(Lib)] internal static extern int aver_phys_add_static_capsule(float cx, float cy, float cz, float radius, float height);
    [DllImport(Lib)] internal static extern int aver_phys_add_dynamic_capsule(float cx, float cy, float cz, float radius, float height, float massKg);
    [DllImport(Lib)] internal static extern int aver_phys_add_static_cylinder(float cx, float cy, float cz, float radius, float height);
    [DllImport(Lib)] internal static extern int aver_phys_add_dynamic_cylinder(float cx, float cy, float cz, float radius, float height, float massKg);
    [DllImport(Lib)] internal static extern int aver_phys_add_static_tapered_capsule(float cx, float cy, float cz, float topRadius, float bottomRadius, float height);
    [DllImport(Lib)] internal static extern int aver_phys_add_dynamic_tapered_capsule(float cx, float cy, float cz, float topRadius, float bottomRadius, float height, float massKg);
    [DllImport(Lib)] internal static extern int aver_phys_add_static_compound_boxes(float cx, float cy, float cz, float[] offsetsCm, float[] halfExtentsCm, int count);
    [DllImport(Lib)] internal static extern int aver_phys_add_dynamic_compound_boxes(float cx, float cy, float cz, float[] offsetsCm, float[] halfExtentsCm, int count, float massKg);

    // ---- Layers (physics_layers_abi.h) -----------------------------------------------------------------

    [DllImport(Lib)] internal static extern int  aver_phys_set_layer_collision(int layerA, int layerB, int enabled);
    [DllImport(Lib)] internal static extern int  aver_phys_layer_collision(int layerA, int layerB);
    [DllImport(Lib)] internal static extern void aver_phys_reset_layer_collisions();
    [DllImport(Lib)] internal static extern int  aver_phys_body_set_layer(int body, int layer);
    [DllImport(Lib)] internal static extern int  aver_phys_body_layer(int body);
    [DllImport(Lib)] internal static extern int  aver_phys_raycast_ex(float ox, float oy, float oz, float dx, float dy, float dz, float maxDistCm, uint layerMask, int ignoreBody, float[] outPoint, float[] outNormal, out int outEntity);
    [DllImport(Lib)] internal static extern int  aver_phys_overlap_sphere_ex(float x, float y, float z, float radius, uint layerMask, int ignoreBody, int[] outBodies, int maxBodies);
    [DllImport(Lib)] internal static extern int  aver_phys_sphere_cast_ex(float ox, float oy, float oz, float dx, float dy, float dz, float maxDistCm, float radius, uint layerMask, int ignoreBody, float[] outPoint, float[] outNormal);

    // ---- Character, the rest of it (physics_character_abi.h) --------------------------------------------

    [DllImport(Lib)] internal static extern int aver_phys_character_set_max_slope_angle(int ch, float radians);
    [DllImport(Lib)] internal static extern int aver_phys_character_max_slope_angle(int ch, float[]? outRadians);
    [DllImport(Lib)] internal static extern int aver_phys_character_set_stair_stepping(int ch, float stepUpCm, float stepDownCm);
    [DllImport(Lib)] internal static extern int aver_phys_character_stair_stepping(int ch, float[] outStepUpCm, float[] outStepDownCm);
    [DllImport(Lib)] internal static extern int aver_phys_character_ground_state(int ch);
    [DllImport(Lib)] internal static extern int aver_phys_character_ground_normal(int ch, float[] outXyz);
    [DllImport(Lib)] internal static extern int aver_phys_character_ground_position(int ch, float[] outXyz);
    [DllImport(Lib)] internal static extern int aver_phys_character_ground_body(int ch);
    [DllImport(Lib)] internal static extern int aver_phys_character_ground_velocity(int ch, float[] outXyz);
    [DllImport(Lib)] internal static extern int aver_phys_character_set_shape(int ch, float radius, float height, float maxPenetrationCm);
    [DllImport(Lib)] internal static extern int aver_phys_character_set_mass(int ch, float massKg);
    [DllImport(Lib)] internal static extern int aver_phys_character_mass(int ch, float[] outMassKg);
    [DllImport(Lib)] internal static extern int aver_phys_character_set_max_strength(int ch, float maxStrengthKgCmS2);
    [DllImport(Lib)] internal static extern int aver_phys_character_max_strength(int ch, float[] outMaxStrengthKgCmS2);
}
