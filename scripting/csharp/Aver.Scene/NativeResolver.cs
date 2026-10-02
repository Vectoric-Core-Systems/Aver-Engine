// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
// Redirects this assembly's P/Invokes to the native DLLs staged beside the executable.

using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Aver.Scene;

/// <summary>Loads the native <c>Aver.Scene</c> / <c>Aver.Framework</c> DLLs from the executable
/// directory. The managed contract assembly shares a file name with the native one, so the default
/// probe would find itself.</summary>
internal static class NativeResolver
{
    private static int s_installed;

    // CA2255: there is no earlier hook a hosted assembly can use to register the resolver.
#pragma warning disable CA2255
    /// <summary>Registers the resolver, once.</summary>
    [ModuleInitializer]
    internal static void Install()
    {
        if (Interlocked.Exchange(ref s_installed, 1) != 0) return;
        NativeLibrary.SetDllImportResolver(typeof(NativeResolver).Assembly, Resolve);
    }
#pragma warning restore CA2255

    /// <summary>Loads a known native library by full path. Zero for anything else.</summary>
    private static IntPtr Resolve(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (libraryName is not ("Aver.Scene" or "Aver.Framework"))
            return IntPtr.Zero;

        string? dir = ExecutableDirectory();
        if (dir is null) return IntPtr.Zero;

        string candidate = Path.Combine(dir, libraryName + ".dll");
        return NativeLibrary.TryLoad(candidate, out IntPtr handle) ? handle : IntPtr.Zero;
    }

    /// <summary>The directory of the running executable, or null if it cannot be read.</summary>
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
