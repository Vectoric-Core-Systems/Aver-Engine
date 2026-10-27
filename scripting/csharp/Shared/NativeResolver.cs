// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Loads the engine's native libraries from the executable directory. One source file, linked into every assembly
// that P/Invokes an Aver.* library (see each .csproj): a resolver is registered per assembly.

using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Aver.Interop;

/// <summary>Resolves this assembly's P/Invokes of <c>Aver.*</c> libraries to the native files beside the executable.</summary>
/// <remarks>
/// Two reasons, both platform probing. Some native libraries share a name with a managed assembly
/// (<c>Aver.Framework</c>, <c>Aver.Scene</c>, <c>Aver.Physics</c>), and the default probe finds the managed one in
/// <c>bin/Scripting/</c> first, so every call fails with EntryPointNotFoundException. And on Linux and macOS the
/// default probe never looks in the executable's folder, where every native library is staged: a library the host
/// does not link (<c>Aver.Settings</c>, issue #3) fails with DllNotFoundException. Zero falls back to the default
/// probe, so a host without the file (a test executable) behaves as before.
/// </remarks>
internal static class NativeResolver
{
    private static int s_installed;

    // CA2255: the resolver must be in place before this assembly's first P/Invoke, and a hosted assembly has no
    // earlier hook.
#pragma warning disable CA2255
    /// <summary>Registers the resolver for this assembly, once.</summary>
    [ModuleInitializer]
    internal static void Install()
    {
        if (Interlocked.Exchange(ref s_installed, 1) != 0) return;
        NativeLibrary.SetDllImportResolver(typeof(NativeResolver).Assembly, Resolve);
    }
#pragma warning restore CA2255

    private static IntPtr Resolve(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (!libraryName.StartsWith("Aver.", StringComparison.Ordinal)) return IntPtr.Zero;
        string? dir = ExecutableDirectory();
        if (dir is null) return IntPtr.Zero;
        return NativeLibrary.TryLoad(Path.Combine(dir, NativeFileName(libraryName)), out IntPtr handle) ? handle : IntPtr.Zero;
    }

    /// <summary>CMake's SHARED naming: Aver.Scene.dll, libAver.Scene.so, libAver.Scene.dylib.</summary>
    internal static string NativeFileName(string libraryName)
    {
        if (OperatingSystem.IsWindows()) return libraryName + ".dll";
        if (OperatingSystem.IsMacOS()) return "lib" + libraryName + ".dylib";
        return "lib" + libraryName + ".so";
    }

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
