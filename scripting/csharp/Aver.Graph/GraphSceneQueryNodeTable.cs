// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The SceneQuery family of the game-systems node table (docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md 2.2): counted access
// to entity sets (count plus item at an index), hierarchy reads, component checks, distance and ragdoll state. Every method
// is in GraphSceneQueries (Aver.Framework/GraphSceneQueries.cs) over the public SceneQueries; class and component names are
// NODE attributes.
// Palette mirror: sandbox/src/GraphNodeDefsSceneQuery.hpp.

using Aver.Framework;

namespace Aver.Graph;

internal static partial class GameSystemNodes
{
    private static GenericNodeSpec[] SceneQueryTable() => new[]
    {
        Reg("an_countactorswithtag", GenericKind.Pure, typeof(GraphSceneQueries), "CountActorsWithTagForGraph", "mask:int", "count:int", "p:mask", "count"),
        Reg("an_getactorwithtagat", GenericKind.Pure, typeof(GraphSceneQueries), "GetActorWithTagAtForGraph", "mask:int,index:int", "entity:int", "p:mask,p:index", "entity"),
        Reg("an_countactorsofclass", GenericKind.Pure, typeof(GraphSceneQueries), "CountActorsOfClassForGraph", "", "count:int", "@class", "count"),
        Reg("an_getactorofclassat", GenericKind.Pure, typeof(GraphSceneQueries), "GetActorOfClassAtForGraph", "index:int", "entity:int", "@class,p:index", "entity"),
        Reg("an_getchildcount", GenericKind.Pure, typeof(GraphSceneQueries), "GetChildCountForGraph", "entity:int", "count:int", "p:entity", "count"),
        Reg("an_getchildat", GenericKind.Pure, typeof(GraphSceneQueries), "GetChildAtForGraph", "entity:int,index:int", "child:int", "p:entity,p:index", "child"),
        Reg("an_getparent", GenericKind.Pure, typeof(GraphSceneQueries), "GetParentForGraph", "entity:int", "parent:int", "p:entity", "parent"),
        Reg("an_hascomponent", GenericKind.Pure, typeof(GraphSceneQueries), "HasComponentForGraph", "entity:int", "has:bool", "p:entity,@component", "has"),
        Reg("an_addcomponent", GenericKind.Exec, typeof(GraphSceneQueries), "AddComponentForGraph", "entity:int", "success:bool", "p:entity,@component", "success"),
        Reg("an_getdistance", GenericKind.Pure, typeof(GraphSceneQueries), "GetDistanceForGraph", "a:int,b:int", "distance:float,success:bool", "p:a,p:b,>distance", "success"),
        Reg("an_getragdollstate", GenericKind.Pure, typeof(GraphSceneQueries), "GetRagdollStateForGraph", "entity:int", "mode:int,has:bool", "p:entity,>mode", "has"),
        Reg("an_getactorcount", GenericKind.Pure, typeof(GraphSceneQueries), "GetActorCountForGraph", "", "count:int", "", "count"),
    };
}
