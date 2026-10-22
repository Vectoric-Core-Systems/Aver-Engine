// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// Aver.Physics world-configuration enums against the native name table (physics_world_abi.h), and the snapshot ABI
// (physics_state_abi.h) through the native library.
//
// The three name getters (aver_phys_setting_name / _unit / aver_phys_config_name) return pointers to static literals, so the
// managed binding cannot import them as `string` (the marshaller would free the pointer). This file imports them as IntPtr
// through its own loader instead. It must LOAD THE NATIVE DLL BY PATH: a plain DllImport("Aver.Physics") from here would
// find the MANAGED Aver.Physics.dll beside the test exe first (the collision NativeResolver.cs explains). When no native DLL
// is found the suite reports SKIP and passes: this project is otherwise engine-free.

using System.Runtime.InteropServices;
using Aver.Physics;

static class PhysicsStateApiTests
{
    public static int RunAll() => T.Suite(() =>
    {
        T.Run("physics world: PhysicsSetting and PhysicsConfigKey equal the native name tables", EnumsMatchNativeTables);
        T.Run("physics state: the managed enums carry the ABI's values", StateEnumsCarryTheAbiValues);
        T.Run("physics state: capture, run, restore, run again through the native library", SnapshotRoundTrip);
        T.Run("physics state: a refused restore changes nothing, and a blob survives a transport", SnapshotRefusalsAndTransport);
        T.Run("physics shape cache: a mesh shape saves, restores to the same bytes, and every bad blob is a miss", ShapeCacheRoundTrip);
        T.Run("physics state: release_all drops every snapshot of the world", ReleaseAllDropsEverySnapshot);
    });

    private delegate int CountFn();
    private delegate IntPtr NameFn(int key);

    // The state ABI, called by function pointer for the reason above: this project cannot P/Invoke the native DLL by name.
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int InitFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void ShutdownFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int AddStaticBoxFn(float cx, float cy, float cz, float hx, float hy, float hz);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int AddDynamicBoxFn(float cx, float cy, float cz, float hx, float hy, float hz, float massKg);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int RemoveBodyFn(int body);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int StepExactFn(int steps);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate ulong StateHashFn(uint flags);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int CaptureFn(int reuse, uint flags);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int RestoreFn(int snapshot, uint options);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int OneIntFn(int a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int NoArgFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate ulong HashFn(int snapshot);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int ReadFn(int snapshot, byte[]? outData, int cap);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int ImportFn(byte[] data, int size);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate long StepTotalFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int CreateMeshFn(float[] verticesXyz, int vertexCount, int[] indices, int indexCount);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int ShapeSaveFn(int shape, ulong key, byte[]? outData, int cap);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int ShapeRestoreFn(byte[] data, int size, ulong key);

    // Finds the native library: AVER_PHYSICS_NATIVE (a file or a directory), else build/bin above the test's working directory.
    private static string? FindNative()
    {
        string? env = Environment.GetEnvironmentVariable("AVER_PHYSICS_NATIVE");
        if (!string.IsNullOrEmpty(env))
        {
            if (File.Exists(env)) return env;
            string inDir = Path.Combine(env, "Aver.Physics.dll");
            if (File.Exists(inDir)) return inDir;
        }
        for (DirectoryInfo? d = new DirectoryInfo(AppContext.BaseDirectory); d != null; d = d.Parent)
        {
            string p = Path.Combine(d.FullName, "build", "bin", "Aver.Physics.dll");
            if (File.Exists(p)) return p;
        }
        return null;
    }

    // "VelocitySteps" -> "velocitySteps": the native names are camelCase, the managed members PascalCase.
    private static string Camel(string pascal) => pascal.Length == 0 ? pascal : char.ToLowerInvariant(pascal[0]) + pascal[1..];

    private static void EnumsMatchNativeTables()
    {
        string? path = FindNative();
        if (path == null)
        {
            Console.WriteLine("  SKIP: native Aver.Physics.dll not found (set AVER_PHYSICS_NATIVE or build into build/bin)");
            return;
        }
        if (!NativeLibrary.TryLoad(path, out IntPtr lib))
        {
            Console.WriteLine($"  SKIP: {path} did not load");
            return;
        }
        try
        {
            var settingCount = Marshal.GetDelegateForFunctionPointer<CountFn>(NativeLibrary.GetExport(lib, "aver_phys_setting_count"));
            var settingName = Marshal.GetDelegateForFunctionPointer<NameFn>(NativeLibrary.GetExport(lib, "aver_phys_setting_name"));
            var configName = Marshal.GetDelegateForFunctionPointer<NameFn>(NativeLibrary.GetExport(lib, "aver_phys_config_name"));

            int n = settingCount();
            T.Check("PhysicsSetting.Count equals aver_phys_setting_count", (int)PhysicsSetting.Count == n, $"enum {(int)PhysicsSetting.Count}, native {n}");
            foreach (PhysicsSetting s in Enum.GetValues<PhysicsSetting>())
            {
                if (s == PhysicsSetting.Count) continue;
                string? native = Marshal.PtrToStringUTF8(settingName((int)s));
                T.Check($"setting {(int)s} is {Camel(s.ToString())}", native == Camel(s.ToString()), $"native '{native}'");
            }
            T.Check("no native setting name past the enum", settingName(n) == IntPtr.Zero);

            // The one deliberate rename: TempMb (matches AVER_PHYS_CF_TEMP_MB) against the table's tempAllocatorMb.
            foreach (PhysicsConfigKey k in Enum.GetValues<PhysicsConfigKey>())
            {
                if (k == PhysicsConfigKey.Count) continue;
                string expect = k == PhysicsConfigKey.TempMb ? "tempAllocatorMb" : Camel(k.ToString());
                string? native = Marshal.PtrToStringUTF8(configName((int)k));
                T.Check($"config {(int)k} is {expect}", native == expect, $"native '{native}'");
            }
            T.Check("no native config name past the enum", configName((int)PhysicsConfigKey.Count) == IntPtr.Zero);
        }
        finally
        {
            NativeLibrary.Free(lib);
        }
    }

    // ---- the snapshot ABI through the native library ----------------------------------------------------------------

    private sealed class NativeState : IDisposable
    {
        private readonly IntPtr _lib;
        public readonly InitFn Init;
        public readonly ShutdownFn Shutdown;
        public readonly AddStaticBoxFn AddStaticBox;
        public readonly AddDynamicBoxFn AddDynamicBox;
        public readonly RemoveBodyFn RemoveBody;
        public readonly StepExactFn StepExact;
        public readonly StateHashFn StateHash;
        public readonly CaptureFn Capture;
        public readonly RestoreFn Restore;
        public readonly NoArgFn LastResult;
        public readonly OneIntFn Release;
        public readonly OneIntFn Bytes;
        public readonly HashFn SnapshotHash;
        public readonly ReadFn Read;
        public readonly ImportFn Import;
        public readonly StepTotalFn StepTotal;
        public readonly CreateMeshFn CreateMesh;
        public readonly ShapeSaveFn ShapeSave;
        public readonly ShapeRestoreFn ShapeRestore;
        public readonly NoArgFn ReleaseAll;

        private NativeState(IntPtr lib)
        {
            _lib = lib;
            Init = Fn<InitFn>(lib, "aver_phys_init");
            Shutdown = Fn<ShutdownFn>(lib, "aver_phys_shutdown");
            AddStaticBox = Fn<AddStaticBoxFn>(lib, "aver_phys_add_static_box");
            AddDynamicBox = Fn<AddDynamicBoxFn>(lib, "aver_phys_add_dynamic_box");
            RemoveBody = Fn<RemoveBodyFn>(lib, "aver_phys_remove_body");
            StepExact = Fn<StepExactFn>(lib, "aver_phys_step_exact");
            StateHash = Fn<StateHashFn>(lib, "aver_phys_state_hash");
            Capture = Fn<CaptureFn>(lib, "aver_phys_snapshot_capture");
            Restore = Fn<RestoreFn>(lib, "aver_phys_snapshot_restore");
            LastResult = Fn<NoArgFn>(lib, "aver_phys_snapshot_last_result");
            Release = Fn<OneIntFn>(lib, "aver_phys_snapshot_release");
            Bytes = Fn<OneIntFn>(lib, "aver_phys_snapshot_bytes");
            SnapshotHash = Fn<HashFn>(lib, "aver_phys_snapshot_hash");
            Read = Fn<ReadFn>(lib, "aver_phys_snapshot_read");
            Import = Fn<ImportFn>(lib, "aver_phys_snapshot_import");
            StepTotal = Fn<StepTotalFn>(lib, "aver_phys_step_total");
            CreateMesh = Fn<CreateMeshFn>(lib, "aver_phys_create_mesh_shape");
            ShapeSave = Fn<ShapeSaveFn>(lib, "aver_phys_shape_save");
            ShapeRestore = Fn<ShapeRestoreFn>(lib, "aver_phys_shape_restore");
            ReleaseAll = Fn<NoArgFn>(lib, "aver_phys_snapshot_release_all");
        }

        private static TDelegate Fn<TDelegate>(IntPtr lib, string name) where TDelegate : Delegate =>
            Marshal.GetDelegateForFunctionPointer<TDelegate>(NativeLibrary.GetExport(lib, name));

        /// <summary>Loads the library, or null (a SKIP line printed) when it is absent or has no state ABI.</summary>
        public static NativeState? Open()
        {
            string? path = FindNative();
            if (path == null)
            {
                Console.WriteLine("  SKIP: native Aver.Physics.dll not found (set AVER_PHYSICS_NATIVE or build into build/bin)");
                return null;
            }
            if (!NativeLibrary.TryLoad(path, out IntPtr lib))
            {
                Console.WriteLine($"  SKIP: {path} did not load");
                return null;
            }
            try { return new NativeState(lib); }
            catch (EntryPointNotFoundException)
            {
                Console.WriteLine($"  SKIP: {path} has no snapshot entry points (an older build)");
                NativeLibrary.Free(lib);
                return null;
            }
        }

        public void Dispose()
        {
            Shutdown();
            NativeLibrary.Free(_lib);
        }

        /// <summary>A floor and eight boxes in two layers, in a fresh world.</summary>
        public int[] Scene()
        {
            Shutdown();
            Init();
            AddStaticBox(0, 0, -50, 3000, 3000, 50);
            var boxes = new int[8];
            for (int i = 0; i < boxes.Length; i++)
                boxes[i] = AddDynamicBox((i % 4) * 40f, (i / 4) * 40f, 12f + (i / 4) * 30f, 10, 10, 10, 5f);
            return boxes;
        }
    }

    private static void StateEnumsCarryTheAbiValues()
    {
        // The full C-to-C# parity check is tests/abi's; these are the values a caller of this file's enums relies on.
        T.Check("SnapshotParts.All is bodies | contacts | constraints | characters | engine",
            (uint)SnapshotParts.All == ((uint)SnapshotParts.Bodies | (uint)SnapshotParts.Contacts | (uint)SnapshotParts.Constraints |
                                        (uint)SnapshotParts.Characters | (uint)SnapshotParts.Engine));
        T.Check("statics are not part of All", ((uint)SnapshotParts.All & (uint)SnapshotParts.Statics) == 0);
        T.Check("RestoreResult.Topology is -4 and Corrupt -5", (int)RestoreResult.Topology == -4 && (int)RestoreResult.Corrupt == -5);
        T.Check("RestoreResult.Ok is 0", (int)RestoreResult.Ok == 0);
        T.Check("RestoreOptions.Strict is 1", (uint)RestoreOptions.Strict == 1);
    }

    private static void SnapshotRoundTrip()
    {
        using NativeState? n = NativeState.Open();
        if (n == null) return;
        n.Scene();
        n.StepExact(2);   // mid-fall: by step 60 the boxes have landed and slept, and a sleeping world hashes the same
        int snap = n.Capture(0, 0);
        T.Check("a capture returns a handle", snap != 0 && n.Bytes(snap) > 64);
        ulong atCapture = n.StateHash(0);
        T.Check("the snapshot's hash is the state hash at that instant", n.SnapshotHash(snap) == atCapture);

        var first = new ulong[60];
        for (int i = 0; i < first.Length; i++) { n.StepExact(1); first[i] = n.StateHash(0); }
        T.Check("the state moved", first[^1] != atCapture);

        T.Check("restore succeeds", n.Restore(snap, 0) == 1 && n.LastResult() == (int)RestoreResult.Ok);
        T.Check("and puts the state back", n.StateHash(0) == atCapture);
        T.Check("and the step count", n.StepTotal() == 2);
        bool equal = true;
        for (int i = 0; i < first.Length; i++) { n.StepExact(1); if (n.StateHash(0) != first[i]) { equal = false; break; } }
        T.Check("the replayed 60 steps hash equal at every step", equal);
    }

    private static void SnapshotRefusalsAndTransport()
    {
        using NativeState? n = NativeState.Open();
        if (n == null) return;
        int[] boxes = n.Scene();
        n.StepExact(30);
        int snap = n.Capture(0, 0);

        // A captured body that is gone refuses the restore and leaves the world alone.
        n.RemoveBody(boxes[2]);
        ulong before = n.StateHash(0);
        T.Check("a missing body refuses the restore", n.Restore(snap, 0) == 0 && n.LastResult() == (int)RestoreResult.Topology);
        T.Check("and the world is untouched", n.StateHash(0) == before);

        // Transport: a blob taken from one world imports into an identical one.
        n.Scene();
        n.StepExact(30);
        int snap2 = n.Capture(0, 0);
        ulong captured = n.SnapshotHash(snap2);
        int size = n.Read(snap2, null, 0);
        var blob = new byte[size];
        T.Check("the blob is copied out", size > 64 && n.Read(snap2, blob, size) == size);

        n.Scene();
        n.StepExact(11);
        int imported = n.Import(blob, size);
        T.Check("the blob imports into a rebuilt world", imported != 0 && n.SnapshotHash(imported) == captured);
        T.Check("and restores to the captured state", n.Restore(imported, 0) == 1 && n.StateHash(0) == captured);

        var bad = (byte[])blob.Clone();
        bad[bad.Length / 2] ^= 0xFF;
        T.Check("a flipped payload byte is Corrupt", n.Import(bad, size) == 0 && n.LastResult() == (int)RestoreResult.Corrupt);
        T.Check("a truncated blob is BadHeader", n.Import(blob, size - 1) == 0 && n.LastResult() == (int)RestoreResult.BadHeader);
        n.Release(imported);
    }

    private static void ShapeCacheRoundTrip()
    {
        using NativeState? n = NativeState.Open();
        if (n == null) return;
        n.Scene();
        float[] v = { -300, -300, 0, 300, -300, 0, 300, 300, 0, -300, 300, 0 };
        int[] idx = { 0, 1, 2, 0, 2, 3 };
        int shape = n.CreateMesh(v, 4, idx, 6);
        T.Check("a mesh shape builds", shape != 0);

        const ulong key = 0x1234ABCD5678EF01UL;
        int size = n.ShapeSave(shape, key, null, 0);
        T.Check("save with a null buffer answers the size", size > 48, $"size {size}");
        var blob = new byte[size];
        T.Check("save writes it", n.ShapeSave(shape, key, blob, size) == size);
        T.Check("a buffer one byte short is refused", n.ShapeSave(shape, key, new byte[size - 1], size - 1) == 0);

        int back = n.ShapeRestore(blob, size, key);
        T.Check("restore returns a new handle", back != 0 && back != shape);
        var again = new byte[size];
        T.Check("and the restored shape saves to the same bytes", n.ShapeSave(back, key, again, size) == size && again.AsSpan().SequenceEqual(blob));

        T.Check("a wrong key is a miss", n.ShapeRestore(blob, size, key + 1) == 0);
        T.Check("a truncated blob is a miss", n.ShapeRestore(blob, size - 1, key) == 0);
        var damaged = (byte[])blob.Clone();
        damaged[damaged.Length - 1] ^= 0xFF;
        T.Check("a damaged payload byte is a miss", n.ShapeRestore(damaged, size, key) == 0);
        var otherPrecision = (byte[])blob.Clone();
        otherPrecision[8] = (byte)(otherPrecision[8] == 4 ? 8 : 4);
        T.Check("the other precision is a miss", n.ShapeRestore(otherPrecision, size, key) == 0);
        T.Check("and the good blob still restores afterwards", n.ShapeRestore(blob, size, key) != 0);
    }

    private static void ReleaseAllDropsEverySnapshot()
    {
        using NativeState? n = NativeState.Open();
        if (n == null) return;
        n.Scene();
        n.StepExact(10);
        int a = n.Capture(0, 0);
        int b = n.Capture(0, 0);
        T.Check("two snapshots exist", a != 0 && b != 0 && a != b);
        T.Check("release_all returns how many it dropped", n.ReleaseAll() == 2);
        T.Check("and both handles read as dead", n.Bytes(a) == 0 && n.Bytes(b) == 0 && n.Restore(a, 0) == 0);
        T.Check("a second call drops none", n.ReleaseAll() == 0);
    }
}
