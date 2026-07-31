// The native managed-dispatch table and its install/clear ABI, as the scripting bridge fills it.

using System.Runtime.InteropServices;

namespace Aver.Framework;

/// <summary>Installs and clears the framework's managed-dispatch table of thunk pointers.</summary>
internal static class ManagedDispatch
{
    // Must match AVER_FW_DISPATCH_VERSION in modules/framework/include/aver/framework/framework_hooks.h.
    internal const int ContractVersion = 2;

    // Mirrors AvManagedDispatch field for field, in header order.
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

    /// <summary>Installs the bridge's dispatch table. Returns false if the framework refused it.</summary>
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

    /// <summary>Clears the installed table to null.</summary>
    internal static bool Clear() => aver_fw_clear_managed_dispatch() != 0;

    /// <summary>True while a table is live on the native side.</summary>
    internal static bool Installed => aver_fw_managed_dispatch_installed() != 0;
}
