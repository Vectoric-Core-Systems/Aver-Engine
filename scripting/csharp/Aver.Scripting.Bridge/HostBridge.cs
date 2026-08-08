// The managed side of the CLR host: bootstrap, script loading, actor dispatch thunks and the log bridge.

using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;

using Aver.Framework;
using Aver.Graph;

namespace Aver.Scripting.Bridge;

/// <summary>The managed end of the engine's in-process CLR host: everything the native side calls.</summary>
public static class HostBridge
{
    // Must match AVER_SCRIPTING_CONTRACT_VERSION in modules/scripting/include/aver/scripting/scripting_abi.h.
    private const int ContractVersion = 3;

    // Must match the AVER_SCRIPT_* codes in the same header.
    private const int Ok = 0;
    private const int ErrContract = -1;
    private const int ErrManagedFault = -2;

    // Mirrors AvScriptHostApi in scripting_abi.h field for field.
    [StructLayout(LayoutKind.Sequential)]
    private struct HostApi
    {
        public int StructBytes;
        public int ContractVersion;
        public IntPtr Log;
    }

    // The native log sink.
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void LogFn(int level, IntPtr utf8Message);

    private static LogFn? s_log;
    private static ScriptLoadContext? s_context;
    private static readonly List<Live> s_live = new();

    // One live AverBehaviour instance.
    private sealed class Live
    {
        public required AverBehaviour Instance;
        public required string Name;
        public bool Disabled;
    }

    // ------------------------------------------------------------------ actor state

    private const int TickGroupCount = 3;   // PrePhysics, Physics, PostPhysics (framework_abi.h)

    // A declared managed class: the C# type bind() constructs, plus the tick wiring Configure asked for.
    private sealed class ClassInfo
    {
        public required Type Type;
        public required bool Ticks;
        public required int TickGroup;
        public required string RegistryName;
        public required int Handle;   // the native class handle
    }

    // One live managed actor instance, bound to an entity.
    private sealed class ActorLive
    {
        public required AverActor Instance;
        public required int Entity;
        public required int TickGroup;
        public required bool Ticks;
        public required string Name;
        public bool Disabled;
    }

    // Every declared managed class, keyed by fnv1a64 of its registry name: the hash native spawn
    // hands bind().
    private static readonly Dictionary<long, ClassInfo> s_classes = new();

    /// <summary>One discovered [AverHud]: its instance, its display name, and the Draw it promised.</summary>
    private sealed class LiveHud
    {
        public required object Instance;
        public required string Name;
        public required MethodInfo Draw;
        public bool Disabled;
    }

    private static readonly List<LiveHud> s_huds = new();
    private static readonly Dictionary<int, ActorLive> s_actorsByEntity = new();
    // Dense per-group lists tick_all walks; only ticking actors are added.
    private static readonly List<ActorLive>[] s_tickBuckets =
        { new List<ActorLive>(), new List<ActorLive>(), new List<ActorLive>() };

    // ------------------------------------------------------------------ entry points

    /// <summary>Takes the host's API table and brings the bridge online. Returns an AVER_SCRIPT_* code.</summary>
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int Bootstrap(IntPtr apiPtr)
    {
        try
        {
            if (apiPtr == IntPtr.Zero)
                return ErrContract;

            HostApi api = Marshal.PtrToStructure<HostApi>(apiPtr);
            if (api.ContractVersion != ContractVersion || api.StructBytes != Marshal.SizeOf<HostApi>())
                return ErrContract;
            if (api.Log == IntPtr.Zero)
                return ErrContract;

            s_log = Marshal.GetDelegateForFunctionPointer<LogFn>(api.Log);
            Log.SetSink(static (level, message) => Emit((int)level, message));

            Emit((int)Log.Level.Info,
                 $"[Scripting] managed bridge online (contract v{ContractVersion}, "
                 + $"{Environment.Version}, API v{typeof(AverBehaviour).Assembly.GetName().Version})");

            SetupManagedActors();

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

            s_context ??= new ScriptLoadContext(
                AssemblyLoadContext.GetLoadContext(typeof(HostBridge).Assembly) ?? AssemblyLoadContext.Default);

            // Registered BEFORE the enumeration below: a script's private dependency is resolved lazily, at
            // the first call into the code that needs it, and OnStart runs inside that enumeration. An F#
            // script assembly sorts ahead of its own FSharp.Core.dll, so a probe list filled afterwards
            // would be empty at exactly the moment it is needed.
            s_context.AddProbeDirectory(dir);

            foreach (string path in Directory.GetFiles(dir, "*.dll").OrderBy(p => p, StringComparer.Ordinal))
                TryLoadAssembly(path);

            foreach (ClassInfo ci in s_classes.Values)
                Fw.aver_fw_class_seal(ci.Handle);

            return s_live.Count(b => !b.Disabled);
        }
        catch (Exception ex)
        {
            Emit((int)Log.Level.Error, $"[Scripting] script loading failed: {Describe(ex)}");
            return 0;
        }
    }

    /// <summary>Drains every live behaviour and unloads the collectible load context. Returns 1 when the
    /// old context was fully collected, 0 when it is still finalising; both are success.</summary>
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int UnloadScripts()
    {
        try
        {
            WeakReference? old = DrainAndUnload();
            if (old is null)
                return 1;

            for (int i = 0; i < 2 && old.IsAlive; ++i)
            {
                GC.Collect();
                GC.WaitForPendingFinalizers();
            }

            if (old.IsAlive)
            {
                Emit((int)Log.Level.Warn,
                     "[Scripting] the previous script context is still finalising - its assemblies stay "
                     + "in memory until every reference to them is dropped. The new ones are live regardless.");
                return 0;
            }
            return 1;
        }
        catch (Exception ex)
        {
            Emit((int)Log.Level.Error, $"[Scripting] unload failed: {Describe(ex)}");
            return 0;
        }
    }

    /// <summary>Calls OnShutdown on everything live, drops the behaviour list and unloads the context.</summary>
    /// <remarks>NoInlining is load-bearing: an inlined local would keep the context alive and fake a leak.</remarks>
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static WeakReference? DrainAndUnload()
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
        s_huds.Clear();

        s_actorsByEntity.Clear();
        foreach (List<ActorLive> bucket in s_tickBuckets) bucket.Clear();
        s_classes.Clear();

        ScriptLoadContext? ctx = s_context;
        s_context = null;
        if (ctx is null) return null;

        var weak = new WeakReference(ctx, trackResurrection: true);
        ctx.Unload();
        return weak;
    }

    /// <summary>How many [AverHud] classes the loaded assemblies declared.</summary>
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int HudCount()
    {
        try { return s_huds.Count; } catch { return 0; }
    }

    /// <summary>Copies HUD <paramref name="index"/>'s display name into <paramref name="buffer"/> as UTF-8,
    /// NUL-terminated and truncated to fit. Returns the byte count written, or 0.</summary>
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static unsafe int HudName(int index, byte* buffer, int capacity)
    {
        try
        {
            if (buffer is null || capacity <= 1) return 0;
            if (index < 0 || index >= s_huds.Count) return 0;
            byte[] utf8 = System.Text.Encoding.UTF8.GetBytes(s_huds[index].Name);
            int n = Math.Min(utf8.Length, capacity - 1);
            for (int i = 0; i < n; ++i) buffer[i] = utf8[i];
            buffer[n] = 0;
            return n;
        }
        catch { return 0; }
    }

    /// <summary>Calls HUD <paramref name="index"/>'s Draw. Returns 1 if it ran, 0 otherwise. A HUD that
    /// throws is disabled rather than retried.</summary>
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int HudDraw(int index, float dt)
    {
        try
        {
            if (index < 0 || index >= s_huds.Count) return 0;
            LiveHud h = s_huds[index];
            if (h.Disabled) return 0;
            try
            {
                h.Draw.Invoke(h.Instance, new object[] { dt });
                return 1;
            }
            catch (Exception ex)
            {
                h.Disabled = true;
                Emit((int)Log.Level.Error,
                     $"[Scripting] HUD '{h.Name}'.Draw threw: {Describe(ex)} - it has been disabled");
                return 0;
            }
        }
        catch { return 0; }
    }

    /// <summary>Calls OnUpdate on every live behaviour. One that throws is disabled, not retried.</summary>
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static void Update(float dt)
    {
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

    /// <summary>Clears the native dispatch, drains everything live and drops the log sink.</summary>
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static void Shutdown()
    {
        try
        {
            ManagedDispatch.Clear();

            s_graphs.Clear();
            DrainAndUnload();
            Log.SetSink(null);
        }
        catch
        {
        }
    }

    // ------------------------------------------------------------------ graph hosting

    // One GraphHost per driven entity. GraphHost.Load compiles the .ocgraph EXACTLY ONCE (see its
    // own doc comment) and its default PositionSink already P/Invokes straight into
    // aver_scene_set_vec against CLocal.position, so a loaded graph drives a real native entity the
    // moment GraphTick is called -- nothing else in this file needs to know where the numbers go.
    private static readonly Dictionary<int, GraphHost> s_graphs = new();

    /// <summary>Loads and compiles the .ocgraph at <paramref name="utf8Path"/>, binding it to
    /// <paramref name="entity"/>. Returns 1 on success, 0 on any failure -- bad path, parse error,
    /// compile error, or a PARAM shape GraphHost cannot supply (see GraphHost.Load). A graph already
    /// bound to this entity is replaced.</summary>
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int GraphLoad(int entity, IntPtr utf8Path)
    {
        try
        {
            string? path = Marshal.PtrToStringUTF8(utf8Path);
            if (string.IsNullOrEmpty(path))
                return 0;

            var host = new GraphHost();
            if (!host.Load(path, out string? err))
            {
                Emit((int)Log.Level.Error, $"[Graph] entity {entity}: {err}");
                return 0;
            }
            s_graphs[entity] = host;
            return 1;
        }
        catch (Exception ex)
        {
            Emit((int)Log.Level.Error, $"[Graph] entity {entity}: load threw: {Describe(ex)}");
            return 0;
        }
    }

    /// <summary>Ticks the graph bound to <paramref name="entity"/>, if any -- a silent no-op
    /// otherwise, so the native caller does not have to track which entities are graph-driven
    /// separately from the ones that are not. A tick that throws unloads the graph rather than
    /// retrying it every frame; RUNTIME ERRORS are not swallowed at the GraphHost.Tick level (see
    /// its own doc comment) but a per-entity bridge cannot let one bad graph take the whole
    /// Update() loop down, so it stops here instead.</summary>
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static void GraphTick(int entity, float timeSeconds)
    {
        if (!s_graphs.TryGetValue(entity, out GraphHost? host))
            return;
        try
        {
            host.Tick(entity, timeSeconds);
        }
        catch (Exception ex)
        {
            Emit((int)Log.Level.Error,
                 $"[Graph] entity {entity}: tick threw: {Describe(ex)} - the graph has been unloaded");
            s_graphs.Remove(entity);
        }
    }

    /// <summary>Drops the graph bound to <paramref name="entity"/>, if any.</summary>
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static void GraphUnload(int entity)
    {
        try { s_graphs.Remove(entity); } catch { }
    }

    // ------------------------------------------------------------------ loading

    // Loads one assembly and discovers its behaviours, actor classes and HUDs.
    private static void TryLoadAssembly(string path)
    {
        string file = Path.GetFileName(path);
        try
        {
            string simple = Path.GetFileNameWithoutExtension(path);
            if (simple is "Aver.Scripting" or "Aver.Scripting.Bridge" or "Aver.Framework" or "Aver.Scene")
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

            int actors = DeclareActors(asm, file);
            DiscoverHuds(asm, file);

            if (found == 0 && actors == 0) WarnAboutNearMisses(asm, file);
        }
        catch (Exception ex)
        {
            Emit((int)Log.Level.Warn, $"[Scripting] {file} could not be loaded: {Describe(ex)}");
        }
    }

    /// <summary>Names types that look like a behaviour but are not one, when an assembly yielded none.</summary>
    private static void WarnAboutNearMisses(Assembly asm, string file)
    {
        foreach (Type type in asm.GetTypes())
        {
            if (type.IsAbstract || !type.IsClass) continue;
            if (type.GetMethod("OnStart", Type.EmptyTypes) is null &&
                type.GetMethod("OnUpdate", new[] { typeof(float) }) is null &&
                type.GetMethod("OnTick", new[] { typeof(float) }) is null)
                continue;

            if (type.GetCustomAttributesData().Any(a =>
                    a.AttributeType.Name is "AverClassAttribute" or "AverGameModeAttribute"))
                continue;

            Emit((int)Log.Level.Warn,
                 $"[Scripting] {file}: {type.FullName} has lifecycle-shaped methods but does not derive "
                 + "from AverBehaviour, so nothing will call them. Add ': AverBehaviour' and mark the "
                 + "hooks 'override'.");
        }
    }

    /// <summary>True when an assembly references Aver.Scripting or Aver.Framework at a compatible version.
    /// False both for a rejected version and for one that references neither.</summary>
    private static bool CheckApiVersion(Assembly asm, string file)
    {
        AssemblyName[] refs = asm.GetReferencedAssemblies();
        if (!VersionMatches(refs, "Aver.Scripting", typeof(AverBehaviour).Assembly, file, out bool sawScripting)
            && sawScripting)
            return false;
        if (!VersionMatches(refs, "Aver.Framework", typeof(AverActor).Assembly, file, out bool sawFramework)
            && sawFramework)
            return false;

        return sawScripting || sawFramework;
    }

    // True unless the assembly references <name> at an incompatible version, which is logged. <saw>
    // reports whether the reference was present at all.
    private static bool VersionMatches(AssemblyName[] refs, string name, Assembly engineAsm, string file, out bool saw)
    {
        AssemblyName? reference = refs.FirstOrDefault(a => a.Name == name);
        saw = reference is not null;
        if (reference is null) return true;

        Version loaded = engineAsm.GetName().Version ?? new Version(0, 0);
        Version referenced = reference.Version ?? new Version(0, 0);
        if (referenced.Major != loaded.Major || referenced > loaded)
        {
            Emit((int)Log.Level.Error,
                 $"[Scripting] {file} was built against {name} {referenced} but this engine provides "
                 + $"{loaded} - the assembly was rejected. Rebuild it against this engine.");
            return false;
        }
        return true;
    }

    // Disables one behaviour and says which hook threw.
    private static void Disable(Live b, string hook, Exception ex)
    {
        b.Disabled = true;
        Emit((int)Log.Level.Error,
             $"[Scripting] {b.Name}.{hook} threw: {Describe(ex)} - the behaviour has been disabled");
    }

    // Renders an exception as one log line.
    private static string Describe(Exception ex) =>
        $"{ex.GetType().Name}: {ex.Message}".ReplaceLineEndings(" ");

    // ================================================================== actor integration

    /// <summary>Installs the managed dispatch table and declares the framework base classes. A failure here
    /// disables actors only; behaviour scripting is unaffected.</summary>
    private static void SetupManagedActors()
    {
        try
        {
            if (!InstallManagedDispatch())
                return;
            DeclareBaseClasses();
            // Resolves an Entity back to its live managed instance. Disabled instances resolve to null.
            Actors.Resolver = handle =>
                s_actorsByEntity.TryGetValue(handle, out ActorLive? live) && !live.Disabled ? live.Instance : null;
        }
        catch (Exception ex)
        {
            Emit((int)Log.Level.Error,
                 $"[Scripting] managed actor support failed to initialise: {Describe(ex)} - "
                 + "actor scripts are disabled; behaviour scripts are unaffected");
        }
    }

    // Hands the framework a dispatch table of function pointers to the thunks below. Returns false if
    // the framework refused it.
    private static unsafe bool InstallManagedDispatch()
    {
        if (ManagedDispatch.Installed)
        {
            Emit((int)Log.Level.Warn, "[Scripting] a managed dispatch is already installed; not re-installing");
            return true;
        }

        bool ok = ManagedDispatch.Install(
            (IntPtr)(delegate* unmanaged[Cdecl]<long, int, int>)&DispBind,
            (IntPtr)(delegate* unmanaged[Cdecl]<int, void>)&DispUnbind,
            (IntPtr)(delegate* unmanaged[Cdecl]<int, int, void>)&DispBeginPlay,
            (IntPtr)(delegate* unmanaged[Cdecl]<int, float, void>)&DispTickAll,
            (IntPtr)(delegate* unmanaged[Cdecl]<int, int, void>)&DispEndPlay,
            (IntPtr)(delegate* unmanaged[Cdecl]<int, void>)&DispRebound,
            (IntPtr)(delegate* unmanaged[Cdecl]<int, void>)&DispBuildModels,
            (IntPtr)(delegate* unmanaged[Cdecl]<int, int, void>)&DispPossessed,
            (IntPtr)(delegate* unmanaged[Cdecl]<int, void>)&DispUnpossessed,
            (IntPtr)(delegate* unmanaged[Cdecl]<int, int, void>)&DispPostLogin);

        if (ok)
            Emit((int)Log.Level.Info, "[Scripting] managed actor dispatch installed");
        else
            Emit((int)Log.Level.Error,
                 "[Scripting] the framework refused the managed dispatch table (contract or size mismatch) "
                 + "- actor scripts are disabled");
        return ok;
    }

    // Declares the five framework base types as lineage roots. Abstract, and never MANAGED.
    private static void DeclareBaseClasses()
    {
        DeclareBase("Actor", "", ClassFlags.Abstract);
        DeclareBase("Pawn", "Actor", ClassFlags.Pawn | ClassFlags.Abstract);
        DeclareBase("PlayerController", "Actor", ClassFlags.Controller | ClassFlags.Abstract);
        DeclareBase("GameMode", "Actor", ClassFlags.GameMode | ClassFlags.Abstract);
        DeclareBase("GameInstance", "Actor", ClassFlags.GameInstance | ClassFlags.Abstract);
    }

    // Declares and seals one base class row.
    private static void DeclareBase(string name, string parent, int flags)
    {
        int c = Fw.aver_fw_class_declare(name, parent);
        if (c == 0)
        {
            Emit((int)Log.Level.Warn, $"[Scripting] could not declare base class {name}");
            return;
        }
        if (flags != 0)
            Fw.aver_fw_class_set_flags(c, Fw.aver_fw_class_get_flags(c) | flags);
        Fw.aver_fw_class_seal(c);
    }

    /// <summary>Finds every [AverHud] class in an assembly and constructs one of each. The attribute is
    /// matched by name, and Draw is located by signature.</summary>
    private static void DiscoverHuds(Assembly asm, string file)
    {
        foreach (Type type in asm.GetTypes())
        {
            CustomAttributeData? marker = type.GetCustomAttributesData()
                .FirstOrDefault(a => a.AttributeType.Name == "AverHudAttribute");
            if (marker is null || type.IsAbstract) continue;

            string name = marker.ConstructorArguments.Count > 0 &&
                          marker.ConstructorArguments[0].Value is string n && n.Length > 0
                        ? n : (type.Name);

            if (type.GetConstructor(Type.EmptyTypes) is null)
            {
                Emit((int)Log.Level.Warn,
                     $"[Scripting] {file}: {type.FullName} is [AverHud] but has no public parameterless "
                     + "constructor - skipped");
                continue;
            }
            MethodInfo? draw = type.GetMethod("Draw", BindingFlags.Public | BindingFlags.Instance,
                                              null, new[] { typeof(float) }, null);
            if (draw is null)
            {
                Emit((int)Log.Level.Warn,
                     $"[Scripting] {file}: {type.FullName} is [AverHud] but has no "
                     + "'public void Draw(float dt)' - skipped");
                continue;
            }
            try
            {
                s_huds.Add(new LiveHud { Instance = Activator.CreateInstance(type)!, Name = name, Draw = draw });
                Emit((int)Log.Level.Info, $"[Scripting] HUD '{name}' ({type.FullName})");
            }
            catch (Exception ex)
            {
                Emit((int)Log.Level.Error, $"[Scripting] {type.FullName} would not construct: {Describe(ex)}");
            }
        }
    }

    /// <summary>Declares every non-abstract AverActor-derived type in an assembly. Returns the count.</summary>
    private static int DeclareActors(Assembly asm, string file)
    {
        int declared = 0;
        foreach (Type type in asm.GetTypes())
        {
            if (!typeof(AverActor).IsAssignableFrom(type) || type.IsAbstract)
                continue;
            if (type.GetConstructor(Type.EmptyTypes) is null)
            {
                Emit((int)Log.Level.Warn,
                     $"[Scripting] {type.FullName} derives from AverActor but has no public parameterless "
                     + "constructor - skipped");
                continue;
            }
            try
            {
                DeclareActorClass(type);
                ++declared;
            }
            catch (Exception ex)
            {
                Emit((int)Log.Level.Error,
                     $"[Scripting] {type.FullName} could not be declared: {Describe(ex)} - skipped");
            }
        }
        if (declared > 0)
            Emit((int)Log.Level.Info, $"[Scripting] {file}: declared {declared} actor class(es)");
        return declared;
    }

    // Declares one class: registers it, runs its Configure recipe, sets flags from the base type,
    // resolves a GameMode's pawn and controller, seals, and records what bind() needs.
    private static void DeclareActorClass(Type type)
    {
        (string name, string parent, int baseFlags) = ResolveClassIdentity(type);

        int c = Fw.aver_fw_class_declare(name, parent);
        if (c == 0)
        {
            Emit((int)Log.Level.Error, $"[Scripting] could not declare class '{name}'");
            return;
        }

        var builder = new ClassBuilder(c);
        MethodInfo? configure = type.GetMethod(
            "Configure",
            BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
            binder: null, types: new[] { typeof(ClassBuilder) }, modifiers: null);
        configure?.Invoke(null, new object[] { builder });

        Fw.aver_fw_class_set_flags(c, Fw.aver_fw_class_get_flags(c) | ClassFlags.Managed | baseFlags);

        if (type.GetCustomAttribute<AverGameModeAttribute>(inherit: false) is { } gm)
        {
            Fw.aver_fw_class_set_default_pawn(c, gm.DefaultPawnClass);
            Fw.aver_fw_class_set_player_controller(c, gm.PlayerControllerClass);
        }

        if (Fw.aver_fw_class_seal(c) == 0)
            Emit((int)Log.Level.Warn,
                 $"[Scripting] class '{name}' did not seal - check its parent '{parent}' is a declared class");

        s_classes[unchecked((long)Fnv1a64(name))] =
            new ClassInfo { Type = type, Ticks = builder.WantsTick, TickGroup = builder.TickGroupId, RegistryName = name, Handle = c };
    }

    // The registry name, parent and base-type flags for a discovered actor type.
    private static (string name, string parent, int baseFlags) ResolveClassIdentity(Type type)
    {
        if (type.GetCustomAttribute<AverClassAttribute>(inherit: false) is { } cls)
        {
            string parent = cls.Parent == "Actor" ? BaseRegistryName(type) : cls.Parent;
            return (cls.Name, parent, BaseFlagsOf(type));
        }
        if (type.GetCustomAttribute<AverGameModeAttribute>(inherit: false) is { } gm)
            return (gm.Name, "GameMode", ClassFlags.GameMode);
        return (type.Name, BaseRegistryName(type), BaseFlagsOf(type));
    }

    private static int BaseFlagsOf(Type type)
    {
        if (typeof(AverPawn).IsAssignableFrom(type)) return ClassFlags.Pawn;
        if (typeof(AverPlayerController).IsAssignableFrom(type)) return ClassFlags.Controller;
        if (typeof(AverGameMode).IsAssignableFrom(type)) return ClassFlags.GameMode;
        if (typeof(AverGameInstance).IsAssignableFrom(type)) return ClassFlags.GameInstance;
        return 0;
    }

    private static string BaseRegistryName(Type type)
    {
        if (typeof(AverPawn).IsAssignableFrom(type)) return "Pawn";
        if (typeof(AverPlayerController).IsAssignableFrom(type)) return "PlayerController";
        if (typeof(AverGameMode).IsAssignableFrom(type)) return "GameMode";
        if (typeof(AverGameInstance).IsAssignableFrom(type)) return "GameInstance";
        return "Actor";
    }

    // FNV-1a 64-bit over the UTF-8 bytes: must stay byte-for-byte the native aver::fnv1a64 (Hash.hpp).
    private static ulong Fnv1a64(string s)
    {
        const ulong offset = 0xcbf29ce484222325UL;
        const ulong prime = 1099511628211UL;
        ulong h = offset;
        foreach (byte b in Encoding.UTF8.GetBytes(s))
        {
            h ^= b;
            h *= prime;
        }
        return h;
    }

    // Disables one actor and says which hook threw.
    private static void DisableActor(ActorLive a, string hook, Exception ex)
    {
        a.Disabled = true;
        Emit((int)Log.Level.Error,
             $"[Scripting] {a.Name}.{hook} threw: {Describe(ex)} - the actor has been disabled");
    }

    // ------------------------------------------------------------------ dispatch thunks
    // The ten entries of AvManagedDispatch. No managed exception may cross back into native code.

    // Constructs the managed instance for a spawned entity. Returns 1 when one was bound.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static int DispBind(long classNameHash, int entity)
    {
        try
        {
            if (!s_classes.TryGetValue(classNameHash, out ClassInfo? info))
                return 0;

            var instance = (AverActor)Activator.CreateInstance(info.Type)!;
            instance.Self = new Entity(entity);

            var live = new ActorLive
            {
                Instance = instance,
                Entity = entity,
                TickGroup = info.TickGroup,
                Ticks = info.Ticks,
                Name = info.RegistryName,
            };
            s_actorsByEntity[entity] = live;
            if (live.Ticks && live.TickGroup >= 0 && live.TickGroup < TickGroupCount)
                s_tickBuckets[live.TickGroup].Add(live);
            return 1;
        }
        catch (Exception ex)
        {
            Emit((int)Log.Level.Error, $"[Scripting] bind of entity {entity} failed: {Describe(ex)}");
            return 0;
        }
    }

    // Runs the actor's BuildModels recipe.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void DispBuildModels(int entity)
    {
        if (!s_actorsByEntity.TryGetValue(entity, out ActorLive? live) || live.Disabled) return;
        try { live.Instance.InvokeBuildModels(new ActorBuilder(live.Instance.Self)); }
        catch (Exception ex) { DisableActor(live, "BuildModels", ex); }
    }

    // Calls the actor's OnBeginPlay.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void DispBeginPlay(int entity, int reason)
    {
        if (!s_actorsByEntity.TryGetValue(entity, out ActorLive? live) || live.Disabled) return;
        try { live.Instance.OnBeginPlay((BeginReason)reason); }
        catch (Exception ex) { DisableActor(live, "OnBeginPlay", ex); }
    }

    // Ticks every actor in one group, and refreshes the frame's input before the first group.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void DispTickAll(int group, float dt)
    {
        if (group < 0 || group >= TickGroupCount) return;

        if (group == 0)
        {
            try { EnhancedInput.Update(); }
            catch (Exception ex) { Emit(3, $"[bridge] input update threw: {ex.Message}"); }
        }
        // A SNAPSHOT, not the live list. The walk used to index s_tickBuckets[group] directly, and
        // DispUnbind REMOVES from that same list (:808) -- so an actor destroying an actor during
        // OnTick shifted every later element down one, and the next ++i stepped straight over
        // whichever actor slid into the vacated slot. It lost a whole frame, silently, and only when
        // something else had just been destroyed, which is exactly the kind of intermittent that
        // never gets reported as a bug.
        //
        // The `i < bucket.Count` guard the old loop carried prevented the out-of-range read at the
        // end but did nothing about the skip in the middle.
        //
        // Copying also preserves the property the old comment claimed: an actor spawned during
        // OnTick is not in the snapshot, so it ticks next frame rather than this one.
        ActorLive[] snapshot = s_tickBuckets[group].ToArray();
        foreach (ActorLive live in snapshot)
        {
            if (live.Disabled) continue;
            // Destroyed earlier in THIS walk. DispUnbind drops it from s_actorsByEntity, so absence
            // there is the liveness test. Reference equality rather than mere presence, because an
            // entity id can be reused by a spawn within the same tick and the new actor is not the
            // one this slot is holding.
            if (!s_actorsByEntity.TryGetValue(live.Entity, out ActorLive? cur) || !ReferenceEquals(cur, live))
                continue;
            try { live.Instance.OnTick(dt); }
            catch (Exception ex) { DisableActor(live, "OnTick", ex); }
        }
    }

    // Calls the actor's OnEndPlay.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void DispEndPlay(int entity, int reason)
    {
        if (!s_actorsByEntity.TryGetValue(entity, out ActorLive? live) || live.Disabled) return;
        try { live.Instance.OnEndPlay((EndReason)reason); }
        catch (Exception ex) { DisableActor(live, "OnEndPlay", ex); }
    }

    // Calls the actor's OnRebound after a reload rebinds it.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void DispRebound(int entity)
    {
        if (!s_actorsByEntity.TryGetValue(entity, out ActorLive? live) || live.Disabled) return;
        try { live.Instance.OnRebound(); }
        catch (Exception ex) { DisableActor(live, "OnRebound", ex); }
    }

    // Calls the pawn's OnPossessed.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void DispPossessed(int pawnEntity, int controllerEntity)
    {
        if (!s_actorsByEntity.TryGetValue(pawnEntity, out ActorLive? live) || live.Disabled) return;
        if (live.Instance is not AverPawn pawn) return;
        try { pawn.OnPossessed(new Entity(controllerEntity)); }
        catch (Exception ex) { DisableActor(live, "OnPossessed", ex); }
    }

    // Calls the pawn's OnUnpossessed.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void DispUnpossessed(int pawnEntity)
    {
        if (!s_actorsByEntity.TryGetValue(pawnEntity, out ActorLive? live) || live.Disabled) return;
        if (live.Instance is not AverPawn pawn) return;
        try { pawn.OnUnpossessed(); }
        catch (Exception ex) { DisableActor(live, "OnUnpossessed", ex); }
    }

    // Calls the game mode's OnPostLogin.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void DispPostLogin(int gameModeEntity, int controllerEntity)
    {
        if (!s_actorsByEntity.TryGetValue(gameModeEntity, out ActorLive? live) || live.Disabled) return;
        if (live.Instance is not AverGameMode mode) return;
        try { mode.OnPostLogin(new Entity(controllerEntity)); }
        catch (Exception ex) { DisableActor(live, "OnPostLogin", ex); }
    }

    // Drops the instance from the entity map and its tick bucket, then calls its OnUnbound.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void DispUnbind(int entity)
    {
        if (!s_actorsByEntity.Remove(entity, out ActorLive? live)) return;
        if (live.Ticks && live.TickGroup >= 0 && live.TickGroup < TickGroupCount)
            s_tickBuckets[live.TickGroup].Remove(live);

        try { live.Instance.OnUnbound(); }
        catch (Exception ex) { Emit(3, $"[bridge] OnUnbound threw for entity {entity}: {ex.Message}"); }
    }

    // ------------------------------------------------------------------ log marshalling

    // Marshals one message to the native log sink as UTF-8.
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
        }
        finally
        {
            if (utf8 != IntPtr.Zero) Marshal.FreeCoTaskMem(utf8);
        }
    }
}
