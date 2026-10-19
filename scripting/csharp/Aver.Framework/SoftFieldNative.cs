// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke declarations for framework_softfield_abi.h.

using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>The soft-body field C ABI (framework_softfield_abi.h). Declared here, not in a bridge assembly, for the same
/// reason as <see cref="FwSoftBody"/>: only this assembly has the DllImport resolver. Every call is deferred to the soft-body
/// system's next tick and applied in arrival order. Names and parameter names are copied from the header character for
/// character (tests/abi checks them).</summary>
internal static class FwSoftField
{
    private const string Lib = "Aver.Framework";

    [DllImport(Lib)] internal static extern int aver_fw_softfield_abi_version();

    [DllImport(Lib)] internal static extern int aver_fw_softfield_pulse(float[] spec, int n);
    [DllImport(Lib)] internal static extern int aver_fw_softfield_start(float[] spec, int n);
    [DllImport(Lib)] internal static extern int aver_fw_softfield_stop(int handle);
    [DllImport(Lib)] internal static extern int aver_fw_softfield_impact(float x, float y, float z, float dx, float dy, float dz, float radiusCm, float energyJ, float kickCmS);
    [DllImport(Lib)] internal static extern int aver_fw_softfield_spawn(float[] spec, int n);
}
