// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// The data layer rows (docs/LEVEL_STREAMING.md). Every method is in DataLayersGraph (Aver.Framework/DataLayersGraph.cs). The layer name is the
// node's layer= attribute. Palette mirror: sandbox/src/GraphNodeDefsDataLayer.hpp.

using Aver.Framework;

namespace Aver.Graph;

internal static partial class GameSystemNodes
{
    private static GenericNodeSpec[] DataLayerTable() => new[]
    {
        Reg("an_getdatalayerstate", GenericKind.Pure, typeof(DataLayersGraph), "GetStateForGraph", "", "state:int", "@layer", "state"),
        Reg("an_setdatalayerstate", GenericKind.Exec, typeof(DataLayersGraph), "SetStateForGraph", "state:int", "success:bool", "@layer,p:state", "success"),
    };
}
