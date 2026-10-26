// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
//
// The floating origin's P/Invokes (physics_origin_abi.h). A further part of Native, as Native.State.cs is, so the parity test in
// tests/abi checks every name and parameter name below against the header; they are copied from it character for character.
using System.Runtime.InteropServices;

namespace Aver.Physics;

internal static partial class Native
{
    // ---- Floating origin (physics_origin_abi.h) --------------------------------------------------------------

    [DllImport(Lib)] internal static extern int  aver_phys_origin_abi_version();

    [DllImport(Lib)] internal static extern int  aver_phys_origin_prepare(double dxCm, double dyCm, double dzCm);
    [DllImport(Lib)] internal static extern int  aver_phys_origin_shift(double dxCm, double dyCm, double dzCm);
    [DllImport(Lib)] internal static extern uint aver_phys_origin_generation();
    [DllImport(Lib)] internal static extern void aver_phys_origin_total(double[] outXyzCm);
    [DllImport(Lib)] internal static extern int  aver_phys_origin_stats(out float outMs, out int outBodies, out int outCharacters, out int outAnchors, out int outPulleysRebuilt, out int outStaticsMoved);
}
