using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;

namespace Aver.Scripting.Bridge;

/// <summary>
/// The managed end of the engine's in-process CLR host. Everything the native side calls lives
/// here, and nothing here may ever throw across that boundary.
/// </summary>
/// <remarks>
/// The four entry points carry <see cref="UnmanagedCallersOnlyAttribute"/>, so the host binds them
/// with <c>load_assembly_and_get_function_pointer</c> and <c>UNMANAGEDCALLERSONLY_METHOD</c> —
/// raw function pointers with no delegate marshalling in the way. That is also why every one of
/// them is wrapped whole in a try/catch: a managed exception escaping an <c>[UnmanagedCallersOnly]</c>
/// method does not become a C++ exception the engine could catch, it terminates the process.
/// </remarks>
public static class HostBridge
{
    // Must match AVER_SCRIPTING_CONTRACT_VERSION in modules/scripting/include/aver/scripting/scripting_abi.h.
    private const int ContractVersion = 1;

    // Must match the AVER_SCRIPT_* codes in the same header.
    private const int Ok = 0;
    private const int ErrContract = -1;
    private const int ErrManagedFault = -2;

    [StructLayout(LayoutKind.Sequential)]
    private struct HostApi
    {
        public int StructBytes;
        public int ContractVersion;
        public IntPtr Log;
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void LogFn(int level, IntPtr utf8Message);

    // Held in a static so the GC never collects the thunk the native side is holding.
    private static LogFn? s_log;
    private static ScriptLoadContext? s_context;
    private static readonly List<Live> s_live = new();

    private sealed class Live
    {
        public required AverBehaviour Instance;
        public required string Name;
        public bool Disabled;
    }

    // ------------------------------------------------------------------ entry points

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int Bootstrap(IntPtr apiPtr)
    {
        try
        {
            if (apiPtr == IntPtr.Zero)
                return ErrContract;

            HostApi api = Marshal.PtrToStructure<HostApi>(apiPtr);
            // Both halves of the contract are checked: the version says the shape was agreed, the
            // size says the struct we are about to read really is that shape. A host built against
            // an older header would otherwise have us read past the end of its struct.
            if (api.ContractVersion != ContractVersion || api.StructBytes != Marshal.SizeOf<HostApi>())
                return ErrContract;
            if (api.Log == IntPtr.Zero)
                return ErrContract;

            s_log = Marshal.GetDelegateForFunctionPointer<LogFn>(api.Log);
            Log.SetSink(static (level, message) => Emit((int)level, message));

            Emit((int)Log.Level.Info,
                 $"[Scripting] managed bridge online (contract v{ContractVersion}, "
                 + $"{Environment.Version}, API v{typeof(AverBehaviour).Assembly.GetName().Version})");
            return Ok;
        }
        catch
        {
            return ErrManagedFault;
        }
    }

    /// <summary>Loads every script assembly in a directory. Returns the number of live behaviours.</summary>
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int LoadScripts(IntPtr utf8Directory)
    {
        try
        {
            string? dir = Marshal.PtrToStringUTF8(utf8Directory);
            if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir))
                return 0;

            // Parented to the context the bridge itself lives in, which is NOT the default one:
            // hostfxr loads a component into its own isolated context. See ScriptLoadContext.Load.
            s_context ??= new ScriptLoadContext(
                AssemblyLoadContext.GetLoadContext(typeof(HostBridge).Assembly) ?? AssemblyLoadContext.Default);

            // Ordered so a run is reproducible; the discovery order decides OnStart order and an
            // unordered directory enumeration would make that vary between machines.
            foreach (string path in Directory.GetFiles(dir, "*.dll").OrderBy(p => p, StringComparer.Ordinal))
                TryLoadAssembly(path);

            return s_live.Count(b => !b.Disabled);
        }
        catch (Exception ex)
        {
            Emit((int)Log.Level.Error, $"[Scripting] script loading failed: {Describe(ex)}");
            return 0;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static void Update(float dt)
    {
        // No try/catch around the loop as a whole on purpose — one behaviour throwing must not
        // stop the ones after it in the list, so the guard is per behaviour, inside.
        for (int i = 0; i < s_live.Count; ++i)
        {
            Live b = s_live[i];
            if (b.Disabled) continue;
            try
            {
                b.Instance.OnUpdate(dt);
            }
            catch (Exception ex)
            {
                Disable(b, "OnUpdate", ex);
            }
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static void Shutdown()
    {
        try
        {
            foreach (Live b in s_live)
            {
                if (b.Disabled) continue;
                try
                {
                    b.Instance.OnShutdown();
                }
                catch (Exception ex)
                {
                    Emit((int)Log.Level.Error, $"[Scripting] {b.Name}.OnShutdown threw: {Describe(ex)}");
                }
            }
            s_live.Clear();

            // Unloading is a request, not a command: the context goes away once nothing references
            // anything in it, which is the collection after this returns. Nothing here waits for
            // it — a host shutting down has no reason to, and blocking on a GC would be worse.
            s_context?.Unload();
            s_context = null;

            Log.SetSink(null);
        }
        catch
        {
            // Swallowed deliberately. This is the last thing that runs; a throw here would take
            // the process out during an orderly exit, which is strictly worse than a lost message.
        }
    }

    // ------------------------------------------------------------------ loading

    private static void TryLoadAssembly(string path)
    {
        string file = Path.GetFileName(path);
        try
        {
            // The bridge and the binding assembly live next to the executable, but a user's build
            // output may well have copied them alongside their own DLL. Loading a second copy into
            // the collectible context would give AverBehaviour two identities and match nothing.
            string simple = Path.GetFileNameWithoutExtension(path);
            if (simple is "Aver.Scripting" or "Aver.Scripting.Bridge")
                return;

            Assembly asm = s_context!.LoadFromFileCopy(path);

            if (!CheckApiVersion(asm, file))
                return;

            int found = 0;
            foreach (Type type in asm.GetTypes())
            {
                if (!typeof(AverBehaviour).IsAssignableFrom(type) || type.IsAbstract)
                    continue;
                if (type.GetConstructor(Type.EmptyTypes) is null)
                {
                    Emit((int)Log.Level.Warn,
                         $"[Scripting] {type.FullName} derives from AverBehaviour but has no public "
                         + "parameterless constructor - skipped");
                    continue;
                }

                var live = new Live { Instance = (AverBehaviour)Activator.CreateInstance(type)!, Name = type.FullName ?? type.Name };
                try
                {
                    live.Instance.OnStart();
                }
                catch (Exception ex)
                {
                    Disable(live, "OnStart", ex);
                }
                s_live.Add(live);
                ++found;
            }

            Emit((int)Log.Level.Info, $"[Scripting] loaded {file}: {found} behaviour(s)");
        }
        catch (Exception ex)
        {
            // A DLL in the scripts folder that is not a managed assembly at all lands here, as does
            // one built for a different architecture. Neither is worth stopping the editor for.
            Emit((int)Log.Level.Warn, $"[Scripting] {file} could not be loaded: {Describe(ex)}");
        }
    }

    /// <summary>
    /// Rejects a user assembly built against a different Aver.Scripting than the one loaded.
    /// </summary>
    /// <remarks>
    /// The version is taken from the assembly's own reference table rather than from an attribute
    /// the author has to remember to apply — a check nobody can forget to opt into is the only kind
    /// that helps. An assembly that does not reference Aver.Scripting at all cannot contain a
    /// behaviour, so it is skipped silently: a scripts folder legitimately holds support libraries.
    /// </remarks>
    private static bool CheckApiVersion(Assembly asm, string file)
    {
        Version loaded = typeof(AverBehaviour).Assembly.GetName().Version ?? new Version(0, 0);
        AssemblyName? reference = asm.GetReferencedAssemblies()
                                     .FirstOrDefault(a => a.Name == "Aver.Scripting");
        if (reference is null)
            return false;

        Version referenced = reference.Version ?? new Version(0, 0);
        if (referenced.Major != loaded.Major || referenced > loaded)
        {
            Emit((int)Log.Level.Error,
                 $"[Scripting] {file} was built against Aver.Scripting {referenced} but this engine "
                 + $"provides {loaded} - the assembly was rejected. Rebuild it against this engine.");
            return false;
        }
        return true;
    }

    private static void Disable(Live b, string hook, Exception ex)
    {
        b.Disabled = true;
        Emit((int)Log.Level.Error,
             $"[Scripting] {b.Name}.{hook} threw: {Describe(ex)} - the behaviour has been disabled");
    }

    private static string Describe(Exception ex) =>
        $"{ex.GetType().Name}: {ex.Message}".ReplaceLineEndings(" ");

    // ------------------------------------------------------------------ log marshalling

    private static void Emit(int level, string message)
    {
        LogFn? log = s_log;
        if (log is null) return;
        IntPtr utf8 = IntPtr.Zero;
        try
        {
            utf8 = Marshal.StringToCoTaskMemUTF8(message);
            log(level, utf8);
        }
        catch
        {
            // The log is the last thing that should be able to break scripting.
        }
        finally
        {
            if (utf8 != IntPtr.Zero) Marshal.FreeCoTaskMem(utf8);
        }
    }
}
