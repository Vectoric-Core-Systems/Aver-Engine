using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;

using Aver.Framework;   // AverActor, ClassBuilder, ClassFlags, Fw, ManagedDispatch, Entity, the attributes

namespace Aver.Scripting.Bridge;

/// <summary>
/// The managed end of the engine's in-process CLR host. Everything the native side calls lives
/// here, and nothing here may ever throw across that boundary.
/// </summary>
/// <remarks>
/// The five host entry points (Bootstrap, LoadScripts, UnloadScripts, Update, Shutdown) plus the ten
/// managed-dispatch thunks near the bottom of this file all carry <see cref="UnmanagedCallersOnlyAttribute"/>,
/// so the host binds the entry points with <c>load_assembly_and_get_function_pointer</c> and
/// <c>UNMANAGEDCALLERSONLY_METHOD</c> and the framework calls the thunks through a raw function-pointer
/// table — no delegate marshalling in the way. That is also why every one of them is wrapped whole in a
/// try/catch: a managed exception escaping an <c>[UnmanagedCallersOnly]</c> method does not become a C++
/// exception the engine could catch, it terminates the process.
/// </remarks>
public static class HostBridge
{
    // Must match AVER_SCRIPTING_CONTRACT_VERSION in modules/scripting/include/aver/scripting/scripting_abi.h.
    private const int ContractVersion = 2;

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

    // ------------------------------------------------------------------ actor state (step 11)
    //
    // The two discovery WORLDS are kept apart: AverBehaviour instances live in s_live above (driven by
    // Update), AverActor instances live in the maps below (driven by the native managed dispatch through
    // the thunks near the bottom of this file). One host, two lifecycles.

    private const int TickGroupCount = 3;   // PrePhysics, Physics, PostPhysics (framework_abi.h)

    // A declared managed class: the C# type bind() constructs, plus the tick wiring the bridge captured
    // from Configure so it can bucket the instance. Keyed by fnv1a64(registry name) — the same hash the
    // native spawn hands bind(), so the reverse lookup is a dictionary hit, not a scan.
    private sealed class ClassInfo
    {
        public required Type Type;
        public required bool Ticks;
        public required int TickGroup;
        public required string RegistryName;
        public required int Handle;   // the native class handle, for the post-load re-seal pass
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

    private static readonly Dictionary<long, ClassInfo> s_classes = new();
    private static readonly Dictionary<int, ActorLive> s_actorsByEntity = new();
    // Dense per-group lists tick_all walks; only ticking actors are added.
    private static readonly List<ActorLive>[] s_tickBuckets =
        { new List<ActorLive>(), new List<ActorLive>(), new List<ActorLive>() };

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

            // Wire the actor half (step 11): install the dispatch table so the framework can call up into
            // managed actor code, and declare the framework base classes so a user class naming "Pawn" or
            // "Actor" as its parent has a row to resolve at seal. Guarded on its own so a framework that is
            // absent or refuses the table disables ACTORS only — AverBehaviour scripting keeps working.
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

            // Parented to the context the bridge itself lives in, which is NOT the default one:
            // hostfxr loads a component into its own isolated context. See ScriptLoadContext.Load.
            s_context ??= new ScriptLoadContext(
                AssemblyLoadContext.GetLoadContext(typeof(HostBridge).Assembly) ?? AssemblyLoadContext.Default);

            // Ordered so a run is reproducible; the discovery order decides OnStart order and an
            // unordered directory enumeration would make that vary between machines.
            foreach (string path in Directory.GetFiles(dir, "*.dll").OrderBy(p => p, StringComparer.Ordinal))
                TryLoadAssembly(path);

            // Re-seal every declared actor class now that ALL assemblies in this load have declared theirs.
            // A class seals at declaration, but a GameMode's default pawn/controller — and a class's parent —
            // is resolved BY NAME at seal, and seal SUCCEEDS on the parent alone: a pawn/controller name that
            // points at a sibling GetTypes() returned later, or at a class in a later-loaded assembly, would
            // otherwise seal with that reference permanently 0 and never re-resolve (the row is already
            // sealed, so spawn's auto-seal never re-runs). Re-sealing once here, after the whole load, makes
            // every name resolve regardless of GetTypes() or assembly order. Idempotent: seal re-runs flatten
            // from scratch and re-resolves the wiring, so a class that already sealed cleanly is unchanged.
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

    /// <summary>
    /// Drains every live behaviour and unloads the collectible load context. The drain half of
    /// hot reload; the host rebuilds and calls <see cref="LoadScripts"/> again afterwards.
    /// </summary>
    /// <returns>
    /// 1 when the old context was fully collected, 0 when it is still finalising. Both are
    /// success — see the note on UnloadScripts in scripting_abi.h.
    /// </returns>
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int UnloadScripts()
    {
        try
        {
            WeakReference? old = DrainAndUnload();
            if (old is null)
                return 1; // nothing was loaded; there is no context to wait for

            // Bounded, and NOT a spin until it succeeds. A behaviour that parked a reference to
            // one of its own types somewhere the engine still holds — a thread, a timer, a static
            // in another context — keeps the old context alive forever, and blocking the editor's
            // main thread on that would turn a leak into a hang. Two cycles is what a context with
            // no stray references needs; anything more is a real reference and is reported.
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

    /// <summary>
    /// Calls OnShutdown on everything live, drops the behaviour list and unloads the context.
    /// </summary>
    /// <remarks>
    /// Its own <b>non-inlined</b> method, and that is load-bearing rather than tidy. The context
    /// can only be collected once no stack frame holds a reference to it, and a JIT that inlined
    /// this into the caller would leave the local alive for the whole of the calling frame — so
    /// the collect below would report a leak that only the inlining had created. Returning a
    /// WeakReference is the only thing that crosses back out.
    /// </remarks>
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

        // The actor instances and the class map hold TYPES from the collectible context; dropping them
        // here too is what lets the ALC actually collect. The native side keeps its entities (still flagged
        // MANAGED) but tick_all now walks empty buckets and bind() no longer knows their class, so nothing
        // is ticked or double-constructed. Rebinding those entities on the next load is the OnRebound
        // follow-up (the hot-reload rebind path), out of scope here.
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
            // Clear the native dispatch FIRST so a stray late tick_all/bind cannot reach a torn-down
            // bridge, then drain. The framework stores the table by value, so this is a NULL store its
            // call sites already guard — after it, spawn/tick/destroy fire no managed hook.
            ManagedDispatch.Clear();

            // The same drain the reload path uses. Nothing here waits for the collection: a host
            // shutting down has no reason to care whether the context went away a millisecond
            // before the process did, and blocking on a GC would be strictly worse.
            DrainAndUnload();
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
            // The engine's own assemblies, which a user's build output copies alongside their own
            // DLL. Loading a second copy into the collectible context would give their types
            // (AverBehaviour, AverActor, Entity) two identities and match nothing. Aver.Framework and
            // Aver.Scene join the list now that scripts reference them for the actor types.
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

            // The SECOND discovery world: gameplay actor classes. Declared, not instantiated — a managed
            // actor instance is constructed later, by bind(), when the framework spawns one. Run per
            // assembly right after the behaviour pass so both worlds are driven from the same load.
            int actors = DeclareActors(asm, file);

            // Only warn about near-misses when the assembly yielded NEITHER a behaviour nor an actor: an
            // actor class is an intentional non-behaviour, not a class that forgot to derive AverBehaviour.
            if (found == 0 && actors == 0) WarnAboutNearMisses(asm, file);
        }
        catch (Exception ex)
        {
            // A DLL in the scripts folder that is not a managed assembly at all lands here, as does
            // one built for a different architecture. Neither is worth stopping the editor for.
            Emit((int)Log.Level.Warn, $"[Scripting] {file} could not be loaded: {Describe(ex)}");
        }
    }

    /// <summary>
    /// Names types that look like a behaviour but are not one, when an assembly yielded none.
    /// </summary>
    /// <remarks>
    /// Discovery is <c>IsAssignableFrom(AverBehaviour)</c>, so a class that merely declares
    /// <c>OnStart</c> and <c>OnUpdate</c> without deriving from the base compiles cleanly and is
    /// skipped in silence — which is the one failure mode this whole layer is meant not to have,
    /// and is exactly what the editor's own script template produced before hosting existed. Only
    /// runs when the assembly produced nothing at all: an assembly with behaviours in it may
    /// legitimately also hold helper classes with a method of the same name.
    /// </remarks>
    private static void WarnAboutNearMisses(Assembly asm, string file)
    {
        foreach (Type type in asm.GetTypes())
        {
            if (type.IsAbstract || !type.IsClass) continue;
            if (type.GetMethod("OnStart", Type.EmptyTypes) is null &&
                type.GetMethod("OnUpdate", new[] { typeof(float) }) is null &&
                type.GetMethod("OnTick", new[] { typeof(float) }) is null)
                continue;

            // A gameplay class (Actor/Pawn/GameMode...) is an INTENTIONAL non-behaviour, not a
            // near-miss, and it carries an Aver.Framework attribute. Checked by attribute NAME
            // because the bridge does not, and must not, reference Aver.Framework — a name match
            // over metadata needs no such reference and cannot pull the wrong assembly in.
            if (type.GetCustomAttributesData().Any(a =>
                    a.AttributeType.Name is "AverClassAttribute" or "AverGameModeAttribute"))
                continue;

            Emit((int)Log.Level.Warn,
                 $"[Scripting] {file}: {type.FullName} has lifecycle-shaped methods but does not derive "
                 + "from AverBehaviour, so nothing will call them. Add ': AverBehaviour' and mark the "
                 + "hooks 'override'.");
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
        // An assembly is a candidate if it references EITHER contract assembly: Aver.Scripting (a
        // behaviour) or Aver.Framework (an actor). A pure-actor assembly need not touch Aver.Scripting at
        // all, so gating solely on it would silently skip every actor-only script. An assembly that
        // references neither cannot contain a behaviour or an actor, so it is skipped silently — a scripts
        // folder legitimately holds support libraries.
        AssemblyName[] refs = asm.GetReferencedAssemblies();
        if (!VersionMatches(refs, "Aver.Scripting", typeof(AverBehaviour).Assembly, file, out bool sawScripting)
            && sawScripting)
            return false;   // referenced Aver.Scripting but at an incompatible version — already logged
        if (!VersionMatches(refs, "Aver.Framework", typeof(AverActor).Assembly, file, out bool sawFramework)
            && sawFramework)
            return false;   // referenced Aver.Framework but at an incompatible version — already logged

        return sawScripting || sawFramework;
    }

    // True unless the assembly references <name> at an INCOMPATIBLE version (different major, or newer than
    // the engine provides), in which case it logs and returns false. <saw> reports whether the reference
    // was present at all, so an absent-but-otherwise-fine reference is not mistaken for a rejection.
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

    private static void Disable(Live b, string hook, Exception ex)
    {
        b.Disabled = true;
        Emit((int)Log.Level.Error,
             $"[Scripting] {b.Name}.{hook} threw: {Describe(ex)} - the behaviour has been disabled");
    }

    private static string Describe(Exception ex) =>
        $"{ex.GetType().Name}: {ex.Message}".ReplaceLineEndings(" ");

    // ================================================================== actor integration (step 11)

    /// <summary>Installs the managed dispatch table and declares the framework base classes.</summary>
    /// <remarks>
    /// Its own guarded step so a framework that is missing or refuses the table takes ACTORS down, never
    /// AverBehaviour scripting. A failure here is logged and swallowed; the behaviour half is independent.
    /// </remarks>
    private static void SetupManagedActors()
    {
        try
        {
            if (!InstallManagedDispatch())
                return;   // logged inside; actors are disabled, behaviours are not
            DeclareBaseClasses();
            // Let any script resolve an Entity back to its live managed instance (Actors.Get / Entity.As<T>
            // / Game.ModeAs<T> / Spawn<T>). The host owns the table; the framework holds only this delegate,
            // keeping the dependency one-way. Disabled instances (a script that threw) resolve to null.
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

    // Build the dispatch table from function pointers to the [UnmanagedCallersOnly] thunks below and hand
    // it to the framework. unsafe only for the `&Thunk` address-of; the thunks themselves are ordinary
    // static methods the CLR never moves, so the pointers are stable for the life of the process.
    private static unsafe bool InstallManagedDispatch()
    {
        if (ManagedDispatch.Installed)
        {
            // Only the executable wires this, once, right after bootstrap; a second live install is
            // refused by the framework. Reaching here means a prior install was never cleared.
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

    // The framework class registry resolves a parent BY NAME at seal, so a user class whose parent is
    // "Pawn" needs a "Pawn" row to exist. The five framework base types are abstract C# types the actor
    // discovery pass never instantiates; declare them once here as the lineage roots. They carry their
    // identifying flag but NOT MANAGED — they are never spawned, so nothing dispatches into them.
    private static void DeclareBaseClasses()
    {
        // Abstract: these are lineage anchors, never spawned themselves. The flag keeps them out of
        // aver_fw_find_class_with_flags, so "find the GameMode to start" lands on a user class, not this.
        DeclareBase("Actor", "", ClassFlags.Abstract);
        DeclareBase("Pawn", "Actor", ClassFlags.Pawn | ClassFlags.Abstract);
        DeclareBase("PlayerController", "Actor", ClassFlags.Controller | ClassFlags.Abstract);
        DeclareBase("GameMode", "Actor", ClassFlags.GameMode | ClassFlags.Abstract);
        DeclareBase("GameInstance", "Actor", ClassFlags.GameInstance | ClassFlags.Abstract);
    }

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

    // Declare one class: register it, run its Configure recipe, set flags from the base type, resolve a
    // GameMode's pawn/controller, seal, and record what bind() needs to construct and bucket an instance.
    private static void DeclareActorClass(Type type)
    {
        (string name, string parent, int baseFlags) = ResolveClassIdentity(type);

        int c = Fw.aver_fw_class_declare(name, parent);   // idempotent by name across a reload
        if (c == 0)
        {
            Emit((int)Log.Level.Error, $"[Scripting] could not declare class '{name}'");
            return;
        }

        // The archetype recipe, if any: static void Configure(ClassBuilder). The builder captures whether
        // the class ticks and in which group so the bridge can bucket its instances without a native getter.
        var builder = new ClassBuilder(c);
        MethodInfo? configure = type.GetMethod(
            "Configure",
            BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
            binder: null, types: new[] { typeof(ClassBuilder) }, modifiers: null);
        configure?.Invoke(null, new object[] { builder });

        // The base type sets MANAGED (always — that flag is what makes native spawn/destroy dispatch up
        // into this bridge) plus the identifying flag; Configure may already have OR'd in TICKS. set_flags
        // OVERWRITES, so read-modify-write to keep whatever Configure set.
        Fw.aver_fw_class_set_flags(c, Fw.aver_fw_class_get_flags(c) | ClassFlags.Managed | baseFlags);

        // A GameMode names its pawn and controller BY STRING; the framework resolves them by name at seal.
        if (type.GetCustomAttribute<AverGameModeAttribute>(inherit: false) is { } gm)
        {
            Fw.aver_fw_class_set_default_pawn(c, gm.DefaultPawnClass);
            Fw.aver_fw_class_set_player_controller(c, gm.PlayerControllerClass);
        }

        if (Fw.aver_fw_class_seal(c) == 0)
            Emit((int)Log.Level.Warn,
                 $"[Scripting] class '{name}' did not seal - check its parent '{parent}' is a declared class");

        // Record it under the SAME hash the native spawn hands bind(): fnv1a64 of the registry name.
        s_classes[unchecked((long)Fnv1a64(name))] =
            new ClassInfo { Type = type, Ticks = builder.WantsTick, TickGroup = builder.TickGroupId, RegistryName = name, Handle = c };
    }

    // The registry (name, parent, base-type flags) for a discovered actor type. [AverClass] and
    // [AverGameMode] name the class and (for AverClass) its parent; without a marker attribute a class
    // registers under its C# type name with the nearest framework base type as parent.
    private static (string name, string parent, int baseFlags) ResolveClassIdentity(Type type)
    {
        if (type.GetCustomAttribute<AverClassAttribute>(inherit: false) is { } cls)
        {
            // If the author left Parent at its "Actor" default but the C# base is more specific
            // (AverPawn, AverGameMode, ...), register under the REAL base's lineage — otherwise the
            // lineage says "Actor" while BaseFlagsOf below stamps the PAWN/CONTROLLER flag, and the
            // two disagree. An EXPLICIT Parent is always honoured.
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

    // FNV-1a 64-bit over the UTF-8 bytes of the name — byte-for-byte the native aver::fnv1a64 (Hash.cpp),
    // so the hash the framework computes for bind() and the hash this bridge stores its classes under agree.
    // The offset basis is the canonical 0xcbf29ce484222325; it once matched a native constant that had a
    // dropped digit, and both were corrected together (Hash.hpp) so class dispatch keeps agreeing.
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

    private static void DisableActor(ActorLive a, string hook, Exception ex)
    {
        a.Disabled = true;
        Emit((int)Log.Level.Error,
             $"[Scripting] {a.Name}.{hook} threw: {Describe(ex)} - the actor has been disabled");
    }

    // ------------------------------------------------------------------ dispatch thunks
    //
    // The ten entries of AvManagedDispatch. Each is a raw [UnmanagedCallersOnly] pointer the framework
    // calls; each wraps its whole body so a managed exception can NEVER cross back into native code — it
    // disables that one actor and returns, exactly as the AverBehaviour contract does for OnUpdate.

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static int DispBind(long classNameHash, int entity)
    {
        try
        {
            if (!s_classes.TryGetValue(classNameHash, out ClassInfo? info))
                return 0;   // no such managed class known here; the framework fires no begin/build

            var instance = (AverActor)Activator.CreateInstance(info.Type)!;
            instance.Self = new Entity(entity);   // internal setter, reachable via InternalsVisibleTo

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
            return 1;   // an instance now exists and is bound to `entity`
        }
        catch (Exception ex)
        {
            Emit((int)Log.Level.Error, $"[Scripting] bind of entity {entity} failed: {Describe(ex)}");
            return 0;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void DispBuildModels(int entity)
    {
        if (!s_actorsByEntity.TryGetValue(entity, out ActorLive? live) || live.Disabled) return;
        try { live.Instance.InvokeBuildModels(new ActorBuilder(live.Instance.Self)); }
        catch (Exception ex) { DisableActor(live, "BuildModels", ex); }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void DispBeginPlay(int entity, int reason)
    {
        if (!s_actorsByEntity.TryGetValue(entity, out ActorLive? live) || live.Disabled) return;
        try { live.Instance.OnBeginPlay((BeginReason)reason); }
        catch (Exception ex) { DisableActor(live, "OnBeginPlay", ex); }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void DispTickAll(int group, float dt)
    {
        if (group < 0 || group >= TickGroupCount) return;

        // Resolve the frame's input ONCE, before the first tick group, rather than letting each actor
        // poll the device as it ticks. Two pawns polling separately can disagree about whether a key
        // went down this frame purely because of the order they tick in, and a "was pressed" edge read
        // twice in one frame is read wrongly at least once. Evaluating up front makes the frame's input
        // a single fact that every actor in it shares.
        if (group == 0)
        {
            try { EnhancedInput.Update(); }
            catch (Exception ex) { Emit(3, $"[bridge] input update threw: {ex.Message}"); }
        }
        // No try/catch around the whole loop on purpose — one actor throwing must not stop the ones after
        // it in the group, so the guard is per actor, inside (the same rule Update uses for behaviours).
        //
        // Snapshot the bucket length BEFORE walking. An actor's OnTick is free to Spawn<T>() another actor,
        // and a same-group spawn runs synchronously through native aver_fw_spawn -> DispBind, which appends
        // the newborn to THIS very list mid-walk. Re-reading bucket.Count each iteration would then tick the
        // just-born actor in the same frame it was spawned and — for a self-propagating spawner — never
        // terminate inside a single tick_all call (a frame hang / unbounded allocation, not next-frame
        // growth). Freezing the count honours the universal spawn-this-frame / tick-next-frame convention.
        // The extra `i < bucket.Count` guard keeps the index in range should a future in-tick unbind ever
        // shrink the list, so an IndexOutOfRange can never cross this [UnmanagedCallersOnly] boundary.
        List<ActorLive> bucket = s_tickBuckets[group];
        int count = bucket.Count;
        for (int i = 0; i < count && i < bucket.Count; ++i)
        {
            ActorLive live = bucket[i];
            if (live.Disabled) continue;
            try { live.Instance.OnTick(dt); }
            catch (Exception ex) { DisableActor(live, "OnTick", ex); }
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void DispEndPlay(int entity, int reason)
    {
        if (!s_actorsByEntity.TryGetValue(entity, out ActorLive? live) || live.Disabled) return;
        try { live.Instance.OnEndPlay((EndReason)reason); }
        catch (Exception ex) { DisableActor(live, "OnEndPlay", ex); }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void DispRebound(int entity)
    {
        if (!s_actorsByEntity.TryGetValue(entity, out ActorLive? live) || live.Disabled) return;
        try { live.Instance.OnRebound(); }
        catch (Exception ex) { DisableActor(live, "OnRebound", ex); }
    }

    // v2 possession/session hooks. Each targets a specific base type — a non-pawn possessed, or a
    // non-GameMode post-login, cannot happen (the framework only calls these for the right flag), but the
    // type test keeps the cast total and simply no-ops if it ever did.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void DispPossessed(int pawnEntity, int controllerEntity)
    {
        if (!s_actorsByEntity.TryGetValue(pawnEntity, out ActorLive? live) || live.Disabled) return;
        if (live.Instance is not AverPawn pawn) return;
        try { pawn.OnPossessed(new Entity(controllerEntity)); }
        catch (Exception ex) { DisableActor(live, "OnPossessed", ex); }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void DispUnpossessed(int pawnEntity)
    {
        if (!s_actorsByEntity.TryGetValue(pawnEntity, out ActorLive? live) || live.Disabled) return;
        if (live.Instance is not AverPawn pawn) return;
        try { pawn.OnUnpossessed(); }
        catch (Exception ex) { DisableActor(live, "OnUnpossessed", ex); }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void DispPostLogin(int gameModeEntity, int controllerEntity)
    {
        if (!s_actorsByEntity.TryGetValue(gameModeEntity, out ActorLive? live) || live.Disabled) return;
        if (live.Instance is not AverGameMode mode) return;
        try { mode.OnPostLogin(new Entity(controllerEntity)); }
        catch (Exception ex) { DisableActor(live, "OnPostLogin", ex); }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void DispUnbind(int entity)
    {
        // Drop the instance from both the entity map and its tick bucket.
        if (!s_actorsByEntity.Remove(entity, out ActorLive? live)) return;
        if (live.Ticks && live.TickGroup >= 0 && live.TickGroup < TickGroupCount)
            s_tickBuckets[live.TickGroup].Remove(live);

        // Then let the actor release anything NATIVE it owns. This is the framework's own hook, not a
        // user one, and it runs however the actor left -- ended, destroyed, reloaded, or disabled after
        // throwing -- which is exactly why a base type's native handle is freed here rather than in the
        // public OnEndPlay a subclass can override and forget to chain.
        //
        // Guarded even so: a base type is still managed code, and an exception crossing back into the
        // native unbind would take the process with it.
        try { live.Instance.OnUnbound(); }
        catch (Exception ex) { Emit(3, $"[bridge] OnUnbound threw for entity {entity}: {ex.Message}"); }
    }

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
