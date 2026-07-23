using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>
/// The native managed-dispatch table (framework_hooks.h <c>AvManagedDispatch</c>) and its install/clear
/// ABI, as the bridge fills and installs it.
/// </summary>
/// <remarks>
/// <para>
/// It lives in THIS assembly rather than in the bridge for one concrete reason: the P/Invoke into the
/// native <c>Aver.Framework</c> DLL must run through the <see cref="NativeResolver"/> registered here, or
/// it would bind to the managed <c>Aver.Framework.dll</c> of the same name sitting beside the bridge. The
/// bridge supplies the seven <c>[UnmanagedCallersOnly]</c> thunk pointers and calls <see cref="Install"/>;
/// nothing in this file knows what a thunk does. Exposed <c>internal</c> to the bridge via
/// <c>InternalsVisibleTo</c>.
/// </para>
/// <para>
/// The struct is the idiom every Aver ABI table uses: <c>structBytes</c> then <c>contractVersion</c>
/// first, so the native side rejects a table whose size or version does not match what it was compiled
/// against rather than crashing through on the first indirect call.
/// </para>
/// </remarks>
internal static class ManagedDispatch
{
    // Must match AVER_FW_DISPATCH_VERSION in modules/framework/include/aver/framework/framework_hooks.h.
    internal const int ContractVersion = 2;

    // Mirror of AvManagedDispatch: int32 structBytes, int32 contractVersion, then the function pointers in
    // the exact header order — bind, unbind, beginPlay, tick_all, endPlay, rebound, build_models, then the
    // v2 possession/session hooks possessed, unpossessed, post_login.
    [StructLayout(LayoutKind.Sequential)]
    private struct Table
    {
        public int StructBytes;
        public int ContractVersion;
        public IntPtr Bind;
        public IntPtr Unbind;
        public IntPtr BeginPlay;
        public IntPtr TickAll;
        public IntPtr EndPlay;
        public IntPtr Rebound;
        public IntPtr BuildModels;
        public IntPtr Possessed;
        public IntPtr Unpossessed;
        public IntPtr PostLogin;
    }

    [DllImport("Aver.Framework")]
    private static extern int aver_fw_install_managed_dispatch(in Table d);
    [DllImport("Aver.Framework")]
    private static extern int aver_fw_clear_managed_dispatch();
    [DllImport("Aver.Framework")]
    private static extern int aver_fw_managed_dispatch_installed();

    /// <summary>Install the bridge's dispatch table. Returns false if the framework refused it.</summary>
    internal static bool Install(IntPtr bind, IntPtr unbind, IntPtr beginPlay, IntPtr tickAll,
                                 IntPtr endPlay, IntPtr rebound, IntPtr buildModels,
                                 IntPtr possessed, IntPtr unpossessed, IntPtr postLogin)
    {
        Table t = new()
        {
            StructBytes = Marshal.SizeOf<Table>(),
            ContractVersion = ContractVersion,
            Bind = bind,
            Unbind = unbind,
            BeginPlay = beginPlay,
            TickAll = tickAll,
            EndPlay = endPlay,
            Rebound = rebound,
            BuildModels = buildModels,
            Possessed = possessed,
            Unpossessed = unpossessed,
            PostLogin = postLogin,
        };
        return aver_fw_install_managed_dispatch(in t) != 0;
    }

    /// <summary>Clear the installed table to NULL (a no-op on the native side after this, never a fault).</summary>
    internal static bool Clear() => aver_fw_clear_managed_dispatch() != 0;

    /// <summary>1 while a table is live on the native side, else 0.</summary>
    internal static bool Installed => aver_fw_managed_dispatch_installed() != 0;
}
