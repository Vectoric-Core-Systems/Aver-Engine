// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The floating-origin rows (docs/plans/wave11/LARGE_WORLD.md 2.5). Every method is in WorldOriginGraph (Aver.Framework/WorldOriginGraph.cs).
// A graph float is a single, so the rows carry whole regions (integers, 16.384 km each) plus a position relative to the region's centre,
// never a bare absolute. Palette mirror: sandbox/src/GraphNodeDefsWorldOrigin.hpp.

using Aver.Framework;

namespace Aver.Graph;

internal static partial class GameSystemNodes
{
    private static GenericNodeSpec[] WorldOriginTable() => new[]
    {
        Reg("an_getworldorigin", GenericKind.Pure, typeof(WorldOriginGraph), "GetOriginForGraph", "", "regionX:int,regionY:int,generation:int,enabled:bool", ">regionY,>generation,>enabled", "regionX"),
        Reg("an_getworldregion", GenericKind.Pure, typeof(WorldOriginGraph), "GetRegionForGraph", "entity:int", "regionX:int,regionY:int,regionZ:int,x:float,y:float,z:float", "p:entity,>regionY,>regionZ,>x,>y,>z", "regionX"),
        Reg("an_setworldregion", GenericKind.Exec, typeof(WorldOriginGraph), "SetRegionForGraph", "entity:int,regionX:int,regionY:int,regionZ:int,x:float,y:float,z:float", "success:bool", "p:entity,p:regionX,p:regionY,p:regionZ,p:x,p:y,p:z", "success"),
    };
}
