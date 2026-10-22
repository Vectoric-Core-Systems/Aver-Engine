// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The physics-world rows of the game-systems node table (docs/plans/wave11/WORLD_STATE_DEBUG.md section 2.12; family version 1):
// a live solver setting by name, and in-session snapshots of the physics world for a rewind or a checkpoint. Every method is in
// PhysicsWorldGraph (Aver.Framework/PhysicsWorldGraph.cs), a thin wrapper over Aver.Physics. Exact stepping, the profiler, the
// debug draw and the large-world origin are not nodes. Palette mirror: sandbox/src/GraphNodeDefsPhysicsWorld.hpp.

using Aver.Framework;

namespace Aver.Graph;

internal static partial class GameSystemNodes
{
    private static GenericNodeSpec[] PhysicsWorldTable() => new[]
    {
        Reg("an_physicssetsetting", GenericKind.Exec, typeof(PhysicsWorldGraph), "SetSettingForGraph", "value:float", "success:bool", "@setting,p:value", "success"),
        Reg("an_physicsgetsetting", GenericKind.Pure, typeof(PhysicsWorldGraph), "GetSettingForGraph", "", "found:bool,value:float", "@setting,>value", "found"),
        Reg("an_physicssnapshot", GenericKind.Exec, typeof(PhysicsWorldGraph), "SnapshotForGraph", "", "snapshot:int,success:bool", "@parts?,>snapshot", "success"),
        Reg("an_physicsrestore", GenericKind.Exec, typeof(PhysicsWorldGraph), "RestoreForGraph", "snapshot:int", "success:bool", "p:snapshot", "success"),
        Reg("an_physicsreleasesnapshot", GenericKind.Exec, typeof(PhysicsWorldGraph), "ReleaseSnapshotForGraph", "snapshot:int", "success:bool", "p:snapshot", "success"),
        Reg("an_physicsstatehash", GenericKind.Pure, typeof(PhysicsWorldGraph), "StateHashForGraph", "", "low:int,high:int", ">high", "low"),
        Reg("an_physicsstats", GenericKind.Pure, typeof(PhysicsWorldGraph), "StatsForGraph", "", "bodies:int,activeDynamic:int,steps:int,stepMs:float,updateErrors:int", ">activeDynamic,>steps,>stepMs,>updateErrors", "bodies"),
    };
}
