// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
// DllImport resolver that loads the native Aver.Physics DLL from the executable directory.

using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Aver.Physics;

/// <summary>
/// Redirects this assembly's P/Invokes past the managed DLL of the same name to the native one
/// beside the executable.
/// </summary>
/// <remarks>
/// THIS ASSEMBLY IS NAMED AFTER THE NATIVE LIBRARY IT BINDS, and that is a collision, not a
/// coincidence: <c>Aver.Physics.dll</c> is both the native module and this managed assembly.
/// modules/scripting/CMakeLists.txt already keeps the two apart on disk by staging every managed
/// assembly into <c>bin/Scripting/</c> rather than <c>bin/</c>, so they do not overwrite each other
/// — but the runtime still probes for a P/Invoke target next to the CALLING assembly first, finds
/// the managed <c>Aver.Physics.dll</c> sitting right there, and fails with an
/// <c>EntryPointNotFoundException</c> naming a function that plainly exists in the native one.
/// <para>
/// That is not a hypothetical: it is exactly what happened the first time this assembly was built,
/// and it took down every physics call in a running play session. Aver.Framework and Aver.Scene each
/// carry the same resolver for the same reason.
/// </para>
/// <para>
/// A RESOLVER IS REGISTERED PER ASSEMBLY, which is why this file exists at all rather than
/// Aver.Framework's copy covering it: <c>NativeLibrary.SetDllImportResolver</c> takes the assembly
/// whose P/Invokes it governs, so each assembly that names a colliding library needs its own.
/// </para>
/// </remarks>
internal static class NativeResolver
{
    private static int s_installed;

    // CA2255 suppressed: the resolver must be registered before this assembly's first P/Invoke, and a
    // hosted contract assembly has no earlier hook.
#pragma warning disable CA2255
    /// <summary>Registers the resolver. Idempotent.</summary>
    [ModuleInitializer]
    internal static void Install()
    {
        if (Interlocked.Exchange(ref s_installed, 1) != 0) return;
        NativeLibrary.SetDllImportResolver(typeof(NativeResolver).Assembly, Resolve);
    }
#pragma warning restore CA2255

    /// <summary>Loads the one colliding library name by full path; zero for everything else.</summary>
    private static IntPtr Resolve(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (libraryName is not "Aver.Physics")
            return IntPtr.Zero;

        string? dir = ExecutableDirectory();
        if (dir is null) return IntPtr.Zero;

        string candidate = Path.Combine(dir, libraryName + ".dll");
        return NativeLibrary.TryLoad(candidate, out IntPtr handle) ? handle : IntPtr.Zero;
    }

    /// <summary>The directory of the process executable, where the native DLLs are staged. Null if unknown.</summary>
    private static string? ExecutableDirectory()
    {
        try
        {
            string? path = Process.GetCurrentProcess().MainModule?.FileName;
            return path is null ? null : Path.GetDirectoryName(path);
        }
        catch
        {
            return null;
        }
    }
}
