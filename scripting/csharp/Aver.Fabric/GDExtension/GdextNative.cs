// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
// P/Invoke of the aver_gdext_* C ABI (modules/fabric.gdext, fabric_gdext_abi.h). Handles are int32; names UTF-8; values travel in
// the flat wire form VariantWire reads and writes. Layout of every call: docs/plans/fabric/G3_GDEXT.md.

using System.Runtime.InteropServices;

namespace Aver.Fabric;

internal static class GdextNative
{
    private const string Lib = "Aver.Fabric.GDExt.Abi";

    /// <summary>A signal an object emitted: the object handle, the signal name and the arguments as a wire-form Array.</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void SignalFn(IntPtr user, int obj, IntPtr signal, IntPtr args, int argsLen);

    internal static string Str(IntPtr p) => p == IntPtr.Zero ? "" : Marshal.PtrToStringUTF8(p) ?? "";

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_abi_version();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] private static extern IntPtr aver_gdext_last_error();
    internal static string LastError() => Str(aver_gdext_last_error());

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_set_api_path([MarshalAs(UnmanagedType.LPUTF8Str)] string path);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_set_features([MarshalAs(UnmanagedType.LPUTF8Str)] string csv);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_load([MarshalAs(UnmanagedType.LPUTF8Str)] string path, [MarshalAs(UnmanagedType.LPUTF8Str)] string? projectDir);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_unload(int library);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_is_loaded([MarshalAs(UnmanagedType.LPUTF8Str)] string path);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_library_count();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_library_at(int index);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] private static extern IntPtr aver_gdext_library_path(int library);
    internal static string LibraryPath(int library) => Str(aver_gdext_library_path(library));

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_class_count(int library);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] private static extern IntPtr aver_gdext_class_name(int library, int index);
    internal static string ClassName(int library, int index) => Str(aver_gdext_class_name(library, index));
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_class_exists([MarshalAs(UnmanagedType.LPUTF8Str)] string cls);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] private static extern IntPtr aver_gdext_class_parent([MarshalAs(UnmanagedType.LPUTF8Str)] string cls);
    internal static string ClassParent(string cls) => Str(aver_gdext_class_parent(cls));
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_class_is_a([MarshalAs(UnmanagedType.LPUTF8Str)] string cls, [MarshalAs(UnmanagedType.LPUTF8Str)] string baseClass);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_class_can_instantiate([MarshalAs(UnmanagedType.LPUTF8Str)] string cls);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_method_count([MarshalAs(UnmanagedType.LPUTF8Str)] string cls);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] private static extern IntPtr aver_gdext_method_name([MarshalAs(UnmanagedType.LPUTF8Str)] string cls, int index);
    internal static string MethodName(string cls, int index) => Str(aver_gdext_method_name(cls, index));
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_property_count([MarshalAs(UnmanagedType.LPUTF8Str)] string cls);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] private static extern IntPtr aver_gdext_property_name([MarshalAs(UnmanagedType.LPUTF8Str)] string cls, int index);
    internal static string PropertyName(string cls, int index) => Str(aver_gdext_property_name(cls, index));
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_signal_count([MarshalAs(UnmanagedType.LPUTF8Str)] string cls);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] private static extern IntPtr aver_gdext_signal_name([MarshalAs(UnmanagedType.LPUTF8Str)] string cls, int index);
    internal static string SignalName(string cls, int index) => Str(aver_gdext_signal_name(cls, index));
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_constant_count([MarshalAs(UnmanagedType.LPUTF8Str)] string cls);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] private static extern IntPtr aver_gdext_constant_name([MarshalAs(UnmanagedType.LPUTF8Str)] string cls, int index);
    internal static string ConstantName(string cls, int index) => Str(aver_gdext_constant_name(cls, index));
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern long aver_gdext_constant_value([MarshalAs(UnmanagedType.LPUTF8Str)] string cls, int index);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] private static extern IntPtr aver_gdext_object_class(int obj);
    internal static string ObjectClass(int obj) => Str(aver_gdext_object_class(obj));
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_instantiate([MarshalAs(UnmanagedType.LPUTF8Str)] string cls);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_free(int obj);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_object_is_valid(int obj);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern long aver_gdext_object_instance_id(int obj);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_object_by_instance_id(long id);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_call(int obj, [MarshalAs(UnmanagedType.LPUTF8Str)] string method, byte[]? args, int argsLen);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_get(int obj, [MarshalAs(UnmanagedType.LPUTF8Str)] string property);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_set(int obj, [MarshalAs(UnmanagedType.LPUTF8Str)] string property, byte[] value, int valueLen);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_has_method(int obj, [MarshalAs(UnmanagedType.LPUTF8Str)] string method);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_result(byte[] output, int cap);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_listen(int obj, SignalFn fn, IntPtr user);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_emit(int obj, [MarshalAs(UnmanagedType.LPUTF8Str)] string signal, byte[]? args, int argsLen);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_has_virtual(int obj, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_enter_tree(int obj, int recursive);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_ready(int obj, int recursive);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_exit_tree(int obj, int recursive);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_process(int obj, double delta, int physics, int recursive);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_notification(int obj, int what);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_flush_deferred();

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_node_set_name(int obj, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_node_set_parent(int child, int parent);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_node_pending_child(int obj);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_node_process_flags(int obj);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] private static extern IntPtr aver_gdext_node_get_name(int obj);
    internal static string NodeName(int obj) => Str(aver_gdext_node_get_name(obj));
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_node3d_get_transform(int obj, float[] out12);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_node3d_set_transform(int obj, float[] in12);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_node3d_transform_version(int obj);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] internal static extern int aver_gdext_unsupported_count();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] private static extern IntPtr aver_gdext_unsupported_name(int index);
    internal static string UnsupportedName(int index) => Str(aver_gdext_unsupported_name(index));

    /// <summary>The bytes of the result of the last call/get that returned a non-negative size.</summary>
    internal static byte[] Result(int size)
    {
        var buf = new byte[Math.Max(size, 0)];
        if (size > 0) aver_gdext_result(buf, size);
        return buf;
    }
}
