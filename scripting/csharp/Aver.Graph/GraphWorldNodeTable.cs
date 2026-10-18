// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The World family of the game-systems node table (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md 2.2): the sky's cloud layer
// and simulated fluid volumes. Every method is in WorldGraph (Aver.Framework/WorldGraph.cs) over Sky and Game. Sun, fog and
// post layers are the WorldFx family.
// Palette mirror: sandbox/src/GraphNodeDefsWorld.hpp.

using Aver.Framework;

namespace Aver.Graph;

internal static partial class GameSystemNodes
{
    private static GenericNodeSpec[] WorldTable() => new[]
    {
        Reg("an_setskyclouds", GenericKind.Exec, typeof(WorldGraph), "SetSkyCloudsForGraph", "seed:int,coverage:float=0.45,density:float=1,bottomCm:float=150000,topCm:float=280000,featureScale:float=0.00002,windX:float=900,windY:float=260", "success:bool", "p:seed,p:coverage,p:density,p:bottomCm,p:topCm,p:featureScale,p:windX,p:windY", "success"),
        Reg("an_clearskyclouds", GenericKind.Exec, typeof(WorldGraph), "ClearSkyCloudsForGraph", "", "success:bool", "", "success"),
        Reg("an_spawnfluidvolume", GenericKind.Exec, typeof(WorldGraph), "SpawnFluidVolumeForGraph", "cx:float,cy:float,cz:float,hx:float,hy:float,hz:float", "success:bool", "p:cx,p:cy,p:cz,p:hx,p:hy,p:hz,@name?", "success"),
    };
}
