// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
// DllImport resolver that loads the native Aver.Framework / Aver.Scene DLLs from the executable directory.

using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>Redirects this assembly's P/Invokes past the managed DLLs of the same name to the native ones beside the executable.</summary>
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

    /// <summary>Loads the two colliding library names by full path; zero for everything else.</summary>
    private static IntPtr Resolve(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        // "Aver.Physics" JOINED THIS LIST THE DAY A MANAGED ASSEMBLY TOOK THAT NAME. Until then the
        // only Aver.Physics.dll anywhere was the native one in bin/, so the loader found it with no
        // help; now a managed Aver.Physics.dll sits in bin/Scripting/ beside THIS assembly, wins the
        // probe, and every P/Invoke against it fails with EntryPointNotFoundException naming a
        // function that plainly exists. Aver.Framework still calls into native physics directly (see
        // Physics.cs's Phys block, kept for Character.cs), so it needs the redirect for its own
        // P/Invokes -- a resolver is registered per ASSEMBLY, and Aver.Physics registers its own.
        if (libraryName is not ("Aver.Framework" or "Aver.Scene" or "Aver.Physics"))
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
