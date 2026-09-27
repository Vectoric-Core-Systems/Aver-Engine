// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
// The managed side of the CLR host: bootstrap, script loading, actor dispatch thunks and the log bridge.

using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;

using Aver.Framework;
using Aver.Graph;
// Assets.ObjectIdOf is the ONE managed spelling of the engine's fnv1a64 -- do not add a second
// copy here; Hash.hpp requires this match the "C# side's spelling" it names in Aver.Scene/Native.cs.
using Aver.Scene;

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

    // ---- graph classes: a .ocgraph with a CLASS record, registered the SAME way a C# actor class
    // is (see DeclareGraphClasses, below) --------------------------------------------------------

    // One declared graph class: which file to (re)compile a fresh GraphHost from, per instance, and
    // whether it wants ticking (OnStart/OnTick ENTRY). Deliberately NOT a cached Graph/delegate --
    // see DispBind's own comment: every instance reloads/recompiles fresh, like every other GraphHost caller.
    private sealed class GraphClassInfo
    {
        public required string Path;
        public required string Name;
        public required bool Ticks;

        // Raw text of the CLASS record's `view=` attribute (Graph.ClassView), or null if omitted.
        // Applied in DispBind to the ancestor's CameraViewMode; unused when the native ancestor isn't AverCharacter.
        public string? View;

        // The class graph's COMPONENT TREE, parsed ONCE at registration and replayed per spawned
        // instance in DispBind. Empty for a class with no COMP records. Kept here rather than re-read
        // from the GraphHost each bind: a component tree is a property of the CLASS, not the instance.
        public required List<GraphComponent> Components;
    }

    // Walks the NATIVE parent chain (aver_fw_class_parent, not this graph's own declared-parent
    // string) until it finds a name in s_classes, or the chain ends. Resolved at BIND time, not
    // DECLARE time, since a graph's parent may itself be an as-yet-undeclared graph class (declare
    // order is file-path-sorted, not parent-before-child) -- matches aver_fw_class_seal's own
    // "resolved by name" flatten() contract; by bind time every class, C# or graph, has been
    // declared and sealed, so the walk cannot dead-end on an ordering accident. THE MECHANISM
    // behind "CLASS AN_Player Character" producing a drivable character: without it,
    // CharacterMoveForGraph's Actors.Get(entity) resolves null for a graph-declared entity. At
    // most ONE ancestor is constructed -- the NEAREST -- single inheritance.
    private static Type? FindNativeAncestorType(string className)
    {
        var visited = new HashSet<int>();
        int walk = Fw.aver_fw_class_parent(Fw.aver_fw_class_find(className));
        while (walk != 0 && visited.Add(walk))
        {
            string name = Fw.Str(Fw.aver_fw_class_name(walk));
            if (s_classes.TryGetValue(Assets.ObjectIdOf(name), out ClassInfo? info))
                return info.Type;
            walk = Fw.aver_fw_class_parent(walk);
        }
        return null;
    }

    // Declared graph classes, keyed by fnv1a64 of the class name (the SAME hash bind() gets); same
    // table shape as s_classes above, disjoint key space (a class is EITHER a C# actor class or a graph class, never both).
    private static readonly Dictionary<long, GraphClassInfo> s_graphClasses = new();

    // One graph-class INSTANCE: its own GraphHost (independent VAR storage per instance -- see
    // GraphVarStore's "two hosts, two stores" comment) and whether it ticks automatically per frame.
    private sealed class GraphInstanceLive
    {
        public required GraphHost Host;
        public required bool Ticks;
    }

    // Every LIVE graph-class instance, keyed by entity -- populated by DispBind, dropped by
    // DispUnbind, walked once a frame by GraphTickBoundInstances (UNGATED, unlike the C# actor tick
    // buckets above -- its own comment). Deliberately separate from s_graphs (below), the drone/
    // MCP-harness "manage this entity's graph by hand" table: mixing a class-spawned instance in would double-tick it or force filtering.
    private static readonly Dictionary<int, GraphInstanceLive> s_graphInstances = new();

    // ---- GAP 3: FireEvent's entity-to-GraphHost router: (entity, eventName) pairs already warned
    // "no live graph"/"no such event" for, so a permanently-dead FireEvent target warns ONCE, not
    // every frame -- cleared in DispUnbind since a reused id deserves its own first warning. --------
    private static readonly HashSet<(int Entity, string EventName)> s_fireWarnedOnce = new();

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

            // Registered BEFORE the enumeration below: a dependency resolves lazily at first use, and
            // OnStart runs inside the enumeration. An F# assembly sorts ahead of its own FSharp.Core.dll,
            // so a probe list filled afterward would be empty exactly when needed.
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
        s_graphClasses.Clear();
        s_graphInstances.Clear();

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

    // ------------------------------------------------------------------ input scheme

    /// <summary>Configures rebindable input for the current project: opens the settings store rebinds
    /// save to/load from, then loads (or with an empty path, unloads) the project's .ocinput scheme as
    /// a pushed <see cref="InputScheme"/> context. The runtime calls it once after
    /// <see cref="DeclareGraphClasses"/> (GameApp.cpp); the editor calls it at every Play start
    /// (SandboxPlay.cpp) since a saved edit only takes effect on a fresh load. Returns the scheme's
    /// action count (0 = unloaded), or -1 if it failed to parse.</summary>
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int ConfigureInput(IntPtr utf8SchemePath, IntPtr utf8SettingsPath)
    {
        try
        {
            string? settingsPath = Marshal.PtrToStringUTF8(utf8SettingsPath);
            if (!string.IsNullOrEmpty(settingsPath) && !Settings.Open(settingsPath))
                Emit((int)Log.Level.Warn,
                     $"[Scripting] settings store '{settingsPath}' could not be opened - rebinds will not persist");

            string? schemePath = Marshal.PtrToStringUTF8(utf8SchemePath);
            if (string.IsNullOrEmpty(schemePath))
            {
                InputScheme.Unload();
                return 0;
            }

            if (!InputScheme.Load(schemePath))
            {
                Emit((int)Log.Level.Error,
                     $"[Scripting] input scheme '{schemePath}' failed to load: {InputScheme.LastError}");
                return -1;
            }

            // The host logs the outcome with its own context (GameApp.cpp, SandboxPlay.cpp).
            return Fw.aver_fw_input_scheme_action_count();
        }
        catch (Exception ex)
        {
            Emit((int)Log.Level.Error, $"[Scripting] ConfigureInput threw: {Describe(ex)}");
            return -1;
        }
    }

    // ------------------------------------------------------------------ graph hosting

    // One GraphHost per driven entity. GraphHost.Load compiles the .ocgraph EXACTLY ONCE (own doc
    // comment); its default PositionSink P/Invokes straight into aver_scene_set_vec against
    // CLocal.position, so GraphTick alone drives the entity -- nothing else here needs to know where the numbers go.
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

    /// <summary>Ticks the graph bound to <paramref name="entity"/>, if any -- a silent no-op otherwise.
    /// A tick that throws unloads the graph rather than retrying it every frame: RUNTIME ERRORS are not
    /// swallowed at the GraphHost.Tick level (own doc comment), but this per-entity bridge cannot let
    /// one bad graph take the whole Update() loop down.</summary>
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

    /// <summary>Raises <paramref name="utf8EventName"/> on whatever graph is bound to
    /// <paramref name="entity"/>. Returns 1 if a handler ran, 0 otherwise. THE NATIVE ENTRY TO THE
    /// EVENT ROUTER: unwraps the UTF-8 pointer and hands both arguments to
    /// <see cref="FireEventRouter"/> -- the SAME closure a FireEvent node's compiled IL reaches
    /// through GraphEvents.Router, so a notify resolves its target exactly as a graph-fired event
    /// does, on the same once-per-pair log rule. Does NOT go through GraphEvents.FireEventForGraph:
    /// its depth guard counts nesting on one call stack, and a notify is a fresh stack at depth
    /// zero -- routing through it would spend one of the eight allowed recursion levels on a call
    /// that cannot itself recurse.</summary>
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int GraphFire(int entity, IntPtr utf8EventName)
    {
        try
        {
            string? name = Marshal.PtrToStringUTF8(utf8EventName);
            if (string.IsNullOrEmpty(name)) return 0;
            return FireEventRouter(entity, name) ? 1 : 0;
        }
        catch (Exception ex)
        {
            // NEVER THROWS ACROSS THE ABI: an exception escaping an UnmanagedCallersOnly frame tears
            // the process down with no diagnostic, and this is called from an animation tick with no idea it's talking to managed code.
            Emit((int)Log.Level.Error, $"[Graph] entity {entity}: fire threw: {Describe(ex)}");
            return 0;
        }
    }

    /// <summary>Parses and validates .ocgraph TEXT and reports the first thing wrong with it.
    /// Returns 0 when the graph is valid, 1 when it is not (with <paramref name="buffer"/> filled),
    /// and -1 on a bad argument. Nothing is loaded, compiled, spawned or ticked -- this reads text.
    /// WHY THIS EXPORT EXISTS: Graph.Validate()/OcGraphParser carry ~30 specific, actionable errors
    /// naming the offending node (e.g. "Node 'x' is a Param node but has no param= attribute naming
    /// which parameter it reads"), but the editor (C++) had no channel to reach them (C#) -- an
    /// author's first sight was the engine log at project open, long after the mistake. TEXT IN,
    /// NOT A PATH: the editor validates the graph ON THE CANVAS including unsaved edits; a path
    /// would validate the last saved version and disagree with what the author is looking at.
    /// OPTIONAL on the host side (same graceful-degradation rule as GraphFire), so an older bridge
    /// still boots and the editor just reports validation unavailable.</summary>
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static unsafe int GraphValidate(IntPtr utf8Text, byte* buffer, int capacity)
    {
        try
        {
            if (buffer is null || capacity <= 1) return -1;
            buffer[0] = 0;
            string? text = Marshal.PtrToStringUTF8(utf8Text);
            if (text is null) return -1;

            // Parse() runs Validate() itself at the end, so one call covers both vocabularies of
            // error -- a malformed record and a well-formed graph that does not hang together.
            string? err;
            if (OcGraphParser.Parse(text, out _, out err)) return 0;

            byte[] utf8 = System.Text.Encoding.UTF8.GetBytes(err ?? "the graph is not valid");
            int n = Math.Min(utf8.Length, capacity - 1);
            for (int i = 0; i < n; ++i) buffer[i] = utf8[i];
            buffer[n] = 0;
            return 1;
        }
        catch (Exception ex)
        {
            // NEVER THROWS ACROSS THE ABI (GraphFire's reason, above). Reported as "not valid" with
            // the exception described -- true and actionable: a graph whose validation crashed is not trustworthy.
            try
            {
                byte[] utf8 = System.Text.Encoding.UTF8.GetBytes($"validation threw: {Describe(ex)}");
                int n = Math.Min(utf8.Length, capacity - 1);
                for (int i = 0; i < n; ++i) buffer[i] = utf8[i];
                buffer[n] = 0;
            }
            catch { }
            return 1;
        }
    }

    /// <summary>Turns per-node execution recording on or off. The editor arms it ONCE at startup and
    /// leaves it armed: the record call is a static bool test when off, cheaper than tracking graph-tab
    /// lifetimes to switch it. A packaged game never calls this, so the hot-path cost per exec node of
    /// every live instance stays at that one test. (NOT "when a graph tab opens/closes" -- SandboxApp's
    /// arming site says otherwise; this was the third copy of that wrong claim.) Returns 1 when
    /// applied, 0 if the framework could not be reached.</summary>
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int GraphSetHitRecording(int on)
    {
        try
        {
            Aver.Framework.GraphInterop.SetNodeHitRecording(on != 0);
            return 1;
        }
        catch (Exception ex)
        {
            // NEVER THROWS ACROSS THE ABI, for GraphFire's reason.
            Emit((int)Log.Level.Error, $"[Graph] hit recording could not be {(on != 0 ? "enabled" : "disabled")}: {Describe(ex)}");
            return 0;
        }
    }

    /// <summary>Writes the nodes of `utf8GraphName` that ran within `maxAgeSeconds` into
    /// <paramref name="buffer"/> as "nodeId:age;nodeId:age", UTF-8 and NUL-terminated. Returns the
    /// byte count written, or 0. AGES, NOT TIMESTAMPS: the two sides do not share a clock (managed
    /// Stopwatch vs. ImGui frame time), so a raw timestamp would make the editor subtract two
    /// unrelated origins. BY GRAPH NAME, not entity: the canvas shows a CLASS and any running
    /// instance should light its node; it is also the only key available, since a compiled graph's
    /// arguments come from its own PARAM list with no entity at the instrumentation point.</summary>
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static unsafe int GraphGetHits(IntPtr utf8GraphName, byte* buffer, int capacity, float maxAgeSeconds)
    {
        try
        {
            if (buffer is null || capacity <= 1) return 0;
            buffer[0] = 0;
            string? name = Marshal.PtrToStringUTF8(utf8GraphName);
            if (string.IsNullOrEmpty(name)) return 0;

            string joined = Aver.Framework.GraphInterop.CollectNodeHits(name, maxAgeSeconds);
            if (joined.Length == 0) return 0;
            byte[] utf8 = System.Text.Encoding.UTF8.GetBytes(joined);
            int n = Math.Min(utf8.Length, capacity - 1);
            // TRUNCATED AT A SEPARATOR, never mid-entry (a half-written "nodeId:0.1" would parse as a
            // node id nothing matches -- silent, not visibly wrong), and ONLY WHEN IT DID NOT FIT: an
            // earlier version walked back unconditionally and lost a perfectly-fitting payload's last
            // entry, invisible to unit tests since they call CollectNodeHits directly, not through this.
            if (n < utf8.Length)
            {
                while (n > 0 && utf8[n - 1] != (byte)';') --n;
                if (n > 0) --n;   // drop the trailing separator itself
            }
            for (int i = 0; i < n; ++i) buffer[i] = utf8[i];
            buffer[n] = 0;
            return n;
        }
        catch (Exception ex)
        {
            Emit((int)Log.Level.Error, $"[Graph] collecting node hits threw: {Describe(ex)}");
            return 0;
        }
    }

    // ------------------------------------------------------------------ graph classes
    // GRAPH-AS-CLASS: a .ocgraph carrying a CLASS record becomes a real registered actor class, via
    // the same aver_fw_class_declare / set_flags(MANAGED) / seal sequence DeclareActorClass uses for a
    // C# type. What differs is what a spawned instance binds to: a C# AverActor for a C# class, or a
    // fresh GraphHost per instance for a graph class (DispBind, its own comment, below).

    /// <summary>Scans <paramref name="utf8ContentDir"/> recursively for *.ocgraph files and declares
    /// one framework class per file that carries a CLASS record (OcGraphParser, Graph.ClassName).
    /// Returns the number of classes declared. A file with no CLASS record is skipped here --
    /// GameApp's own discoverProjectGraphs (native side) drives a CLASS-less graph against a synthetic
    /// entity, as it always has. SAFE WITH NO GRAPHS OR NO DIRECTORY: returns 0, changes nothing --
    /// same "empty is a no-op" contract LoadScripts has for a missing scripts directory.</summary>
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int DeclareGraphClasses(IntPtr utf8ContentDir)
    {
        try
        {
            string? dir = Marshal.PtrToStringUTF8(utf8ContentDir);
            if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir))
                return 0;

            int declared = 0;
            // `pawn=` assignments, in a SECOND pass below (Graph.ClassPawn): aver_fw_class_set_default_pawn
            // resolves the name at SEAL, so a GameMode naming a not-yet-declared pawn would
            // non-deterministically possess nothing. Deferring until every class exists avoids that.
            var pendingRoles = new List<(int Handle, string ClassName, string? Pawn, string? Controller, string Path)>();
            // Sorted, like LoadScripts' own assembly enumeration: aver_fw_class_declare is idempotent by
            // name (LAST declare wins), so which file wins when two disagree must not depend on filesystem order.
            foreach (string path in Directory.EnumerateFiles(dir, "*.ocgraph", SearchOption.AllDirectories)
                                              .OrderBy(p => p, StringComparer.Ordinal))
            {
                string text;
                try { text = File.ReadAllText(path); }
                catch (Exception ex)
                {
                    Emit((int)Log.Level.Warn, $"[Graph] could not read '{path}': {Describe(ex)}");
                    continue;
                }

                if (!OcGraphParser.Parse(text, out Aver.Graph.Graph graph, out string? parseErr))
                {
                    // Only worth a warning when the file LOOKS like it wanted to declare a class -- an
                    // ordinary (non-class) parse failure is discoverProjectGraphs' own concern, already reported there.
                    if (text.Contains("\nCLASS ", StringComparison.OrdinalIgnoreCase) ||
                        text.StartsWith("CLASS ", StringComparison.OrdinalIgnoreCase))
                        Emit((int)Log.Level.Warn,
                             $"[Graph] '{path}' looks like it declares a CLASS but failed to parse: {parseErr}");
                    continue;
                }

                // NOT EVERY .ocgraph IS A GAMEPLAY GRAPH: this loop reaches every one under the content
                // directory, and a material graph's nodes mean nothing here (Graph.DomainKind). Checked
                // before ClassName so a foreign graph carrying a CLASS record is skipped, not declared.
                if (graph.DomainKind != Aver.Graph.GraphDomain.Gameplay)
                    continue;

                if (string.IsNullOrEmpty(graph.ClassName))
                    continue;   // no CLASS record -- not this pass's file

                string parent = string.IsNullOrEmpty(graph.ClassParent) ? "Actor" : graph.ClassParent;
                int c = Fw.aver_fw_class_declare(graph.ClassName, parent);
                if (c == 0)
                {
                    Emit((int)Log.Level.Error, $"[Graph] could not declare class '{graph.ClassName}' from '{path}'");
                    continue;
                }

                // Same seams the C# actor path uses (DeclareActorClass, above): a ClassBuilder for the
                // optional mesh/material class defaults, then MANAGED so DispBind actually fires on
                // spawn, then seal.
                var builder = new ClassBuilder(c);
                if (!string.IsNullOrEmpty(graph.ClassMesh))
                    builder.Mesh(graph.ClassMesh, graph.ClassMaterial ?? "");

                Fw.aver_fw_class_set_flags(c, Fw.aver_fw_class_get_flags(c) | ClassFlags.Managed);

                if (Fw.aver_fw_class_seal(c) == 0)
                {
                    Emit((int)Log.Level.Warn,
                         $"[Graph] class '{graph.ClassName}' (from '{path}') did not seal -- check its "
                         + $"parent '{parent}' is a declared class");
                    continue;
                }

                // Ticks (has an OnStart or OnTick ENTRY) decides whether GraphTickBoundInstances drives
                // this class's instances every frame -- a flat, UNGATED walk, not a tick-group bucket
                // (that method's own comment), because a graph-only project never calls
                // aver_fw_begin_play (no C# GameMode -- GameApp.hpp's beginPlayIfGameModeDeclared), so
                // aver_fw_play_state() would stay EDITOR forever and OnTick would never run -- the same
                // trap tickProjectGraphs names for the synthetic-entity path, one layer down.
                bool ticks = graph.EntryPoints.Any(e => e.EventName == "OnStart" || e.EventName == "OnTick");

                // TWO FILES CLAIMING ONE CLASS NAME IS SAID OUT LOUD: declare order is sorted so which
                // file wins is deterministic, but without this warning both logged an identical
                // "declared class" line and only the last graph ran, indistinguishable from success --
                // the symptom looked like a bug in the graph. A warning, not a refusal: last-wins is
                // aver_fw_class_declare's own idempotent-by-name behaviour; refusing both would block a rename-in-progress entirely.
                long classKey = Assets.ObjectIdOf(graph.ClassName);
                if (s_graphClasses.TryGetValue(classKey, out GraphClassInfo prior))
                    Emit((int)Log.Level.Warn,
                         $"[Graph] class '{graph.ClassName}' is declared by more than one graph: "
                         + $"'{prior.Path}' is superseded by '{path}'. Only the latter will run -- "
                         + "rename one of them.");
                s_graphClasses[classKey] =
                    new GraphClassInfo { Path = path, Name = graph.ClassName, Ticks = ticks, View = graph.ClassView,
                                         Components = graph.Components };
                if (!string.IsNullOrEmpty(graph.ClassPawn) || !string.IsNullOrEmpty(graph.ClassController))
                    pendingRoles.Add((c, graph.ClassName, graph.ClassPawn, graph.ClassController, path));

                ++declared;
                Emit((int)Log.Level.Info,
                     $"[Graph] declared class '{graph.ClassName}' (parent '{parent}'{(ticks ? ", ticks" : "")}) from '{path}'");
            }

            // ---- second pass: `pawn=`, now that every class name above exists --------------------
            // Re-sealing is documented, not a workaround: aver_fw_class_set_default_pawn clears
            // `sealed` so the name resolves, and spawning auto-seals anyway -- sealing here reports a
            // bad name NOW instead of a GameMode silently possessing nothing.
            foreach (var (handle, className, pawnName, controllerName, path) in pendingRoles)
            {
                // GameMode ONLY: ClassRecord::defaultPawn/::playerController are read by aver_fw_begin_play
                // off the GameMode class and nowhere else, so saying so beats silently doing nothing. A
                // warning, not a refusal -- matches how `view=` on a non-Character class is tolerated.
                if ((Fw.aver_fw_class_get_flags(handle) & ClassFlags.GameMode) == 0)
                {
                    Emit((int)Log.Level.Warn,
                         $"[Graph] class '{className}' (from '{path}') sets pawn=/controller=, but only a "
                         + "GameMode has those -- the attributes are ignored here");
                    continue;
                }

                bool changed = false;
                if (!string.IsNullOrEmpty(pawnName))
                {
                    if (Fw.aver_fw_class_find(pawnName) == 0)
                        Emit((int)Log.Level.Warn,
                             $"[Graph] GameMode '{className}' (from '{path}') names pawn='{pawnName}', which "
                             + "is not a declared class -- nothing will be possessed. Check it against the "
                             + "CLASS record of the graph that declares it");
                    else { Fw.aver_fw_class_set_default_pawn(handle, pawnName); changed = true; }
                }
                if (!string.IsNullOrEmpty(controllerName))
                {
                    if (Fw.aver_fw_class_find(controllerName) == 0)
                        Emit((int)Log.Level.Warn,
                             $"[Graph] GameMode '{className}' (from '{path}') names controller="
                             + $"'{controllerName}', which is not a declared class");
                    else { Fw.aver_fw_class_set_player_controller(handle, controllerName); changed = true; }
                }

                if (!changed) continue;

                if (Fw.aver_fw_class_seal(handle) == 0)
                {
                    Emit((int)Log.Level.Warn,
                         $"[Graph] GameMode '{className}' (from '{path}') would not re-seal after pawn/controller");
                    continue;
                }

                // THE HALF-WIRED CASE IS CALLED OUT because it looks correct and does nothing:
                // aver_fw_begin_play possesses only with BOTH ("if (ctrl && pawn)"), and the built-in
                // PlayerController is ABSTRACT so it can't be a fallback -- a pawn with no controller
                // spawns but is never possessed, leaving GameApp's camera following nothing. This
                // attribute pair exists to end that failure; don't let it silently return.
                if (!string.IsNullOrEmpty(pawnName) && string.IsNullOrEmpty(controllerName))
                    Emit((int)Log.Level.Warn,
                         $"[Graph] GameMode '{className}' names a pawn but no controller, so the pawn is "
                         + "spawned and never possessed (and no camera follows it). Add "
                         + "controller=<YourController>, declared by a graph whose CLASS parent is "
                         + "PlayerController");

                Emit((int)Log.Level.Info,
                     $"[Graph] GameMode '{className}' begins play with pawn='{pawnName ?? "(none)"}' "
                     + $"controller='{controllerName ?? "(none)"}'");
            }

            return declared;
        }
        catch (Exception ex)
        {
            Emit((int)Log.Level.Error, $"[Graph] class declaration scan failed: {Describe(ex)}");
            return 0;
        }
    }

    /// <summary>Ticks every LIVE graph-class instance (s_graphInstances) once. Called every frame from
    /// BOTH composition roots, UNGATED on aver_fw_play_state() -- see DeclareGraphClasses' own comment
    /// on `ticks`: a graph-only project has no C# GameMode to begin play with, so gating on PLAYING
    /// would make graph-as-class silently inert in the one configuration it exists for. A per-instance
    /// exception unloads just that instance, mirroring GraphTick's own per-entity isolation.</summary>
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static void GraphTickBoundInstances(float dt)
    {
        if (s_graphInstances.Count == 0) return;

        // A SNAPSHOT, not the live table -- mirrors DispTickAll's own reasoning: an instance destroyed
        // (DispUnbind) earlier in THIS walk must not be ticked, and one spawned during it ticks next
        // frame. The ReferenceEquals re-check also catches an entity id reused within the same tick.
        var snapshot = s_graphInstances.ToArray();
        foreach (var kv in snapshot)
        {
            int entity = kv.Key;
            if (!kv.Value.Ticks) continue;
            if (!s_graphInstances.TryGetValue(entity, out GraphInstanceLive? cur) || !ReferenceEquals(cur, kv.Value))
                continue;
            try
            {
                cur.Host.Tick(entity, dt);
            }
            catch (Exception ex)
            {
                Emit((int)Log.Level.Error,
                     $"[Graph] class instance entity {entity}: tick threw: {Describe(ex)} - unloaded");
                s_graphInstances.Remove(entity);
            }
        }
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
            // GAP 3: installs the FireEvent router (GraphEvents.Router's own comment says why --
            // Aver.Graph cannot see this file). Mirrors Actors.Resolver in shape, but checks TWO
            // tables in order: s_graphInstances (a class-spawned instance's own GraphHost -- "graph
            // classes" region above) FIRST, then s_graphs ("graph hosting" region). Collision is
            // structurally impossible for GameApp's project graphs (discoverProjectGraphs keys
            // s_graphs with NEGATIVE synthetic ids), but SandboxApp's drone CAN hold a real positive
            // id there too, so the order is a real choice: a class-spawned instance's host wins if an id ever appears in both.
            GraphEvents.Router = FireEventRouter;
            InstallGraphVarProvider();
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

    // ================================================================== graph-VAR persistence
    // A SEPARATE provider pair from ManagedDispatch above: answers a save, reusing
    // aver_fw_set_save_provider/aver_fw_set_anim_curve_provider's own small, independently-installable
    // shape (framework_abi.h) rather than growing ManagedDispatch's struct (GraphVarStore is pure
    // managed state with no native representation -- SaveWorld.hpp's GraphVarCountFn explains why).
    // The installer being C# here is fine though every prior set_provider call is C++-to-C++: it's
    // a plain cdecl function pointer + void* user (null provider answers 0/false), so only ONE
    // composition root calling it matters, not which language. SCOPED TO GameInstance (explicit,
    // later user decision): VARs persist ONLY for GameInstance-or-subclass entities, via
    // FindPersistedGraphHost, never FindGraphHost directly. Routed through
    // Fw.aver_fw_set_graph_var_provider (Aver.Framework/Native.cs), NOT a raw [DllImport] here --
    // that resolves to the wrong same-name DLL (EntryPointNotFoundException).
    private static unsafe void InstallGraphVarProvider()
    {
        Fw.aver_fw_set_graph_var_provider(
            (IntPtr)(delegate* unmanaged[Cdecl]<int, IntPtr, int>)&GraphVarCountProvider,
            (IntPtr)(delegate* unmanaged[Cdecl]<int, int, byte*, int, int*, float*, int*, IntPtr, int>)&GraphVarAtProvider,
            (IntPtr)(delegate* unmanaged[Cdecl]<int, byte*, int, float, int, IntPtr, int>)&GraphVarSetProvider,
            IntPtr.Zero);
    }

    // A VAR's declared PinType, as the AVER_SCENE_KIND_* the save format's OcSaveField.kind carries
    // (scene_abi.h) -- NOT PinType's own ordinal (Float=0,Int=1,Bool=2 there; F32=0,I32=3,BOOL=4
    // here), a real translation. -1 for Exec: a VAR can never declare it (OcGraphParser rejects
    // "VAR ... exec" at parse time); callers treat -1 as a bug, not a refusal.
    private static int SceneKindOf(PinType t) => t switch
    {
        PinType.Float => 0,   // AVER_SCENE_KIND_F32
        PinType.Int   => 3,   // AVER_SCENE_KIND_I32
        PinType.Bool  => 4,   // AVER_SCENE_KIND_BOOL
        _             => -1,
    };

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static int GraphVarCountProvider(int entity, IntPtr user)
    {
        // Graph.Variables, not VarStore: the STORE holds only what's been written, while Variables is
        // the DECLARED list -- every VAR in file order, touched or not; capture must see all of them
        // or a save silently omits one. FindPersistedGraphHost: a non-GameInstance entity reports zero,
        // so SaveWorld.cpp's captureGraphVars sees it as if it had no GraphHost at all.
        return FindPersistedGraphHost(entity)?.Graph?.Variables.Count ?? 0;
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static unsafe int GraphVarAtProvider(int entity, int index, byte* nameBuf, int nameBufLen,
                                                 int* outKind, float* outF, int* outI, IntPtr user)
    {
        GraphHost? host = FindPersistedGraphHost(entity);
        List<GraphVariable>? vars = host?.Graph?.Variables;
        if (vars == null || (uint)index >= (uint)vars.Count || nameBuf == null || nameBufLen <= 0)
            return 0;

        GraphVariable v = vars[index];
        int kind = SceneKindOf(v.Type);
        if (kind < 0) return 0;   // unreachable in practice -- see SceneKindOf's own comment

        // A caller-owned buffer, not a returned pointer: the name lives in MANAGED memory
        // (GraphVariable.Name), and a raw pointer into it would be a GC hazard the moment anything
        // moved -- crosses the ABI by copy, not by value like aver_scene_get_str; the first string here to cross this direction.
        byte[] utf8 = Encoding.UTF8.GetBytes(v.Name);
        if (utf8.Length >= nameBufLen)
        {
            // A VAR name longer than the buffer -- vanishingly unlikely (63 UTF8 bytes is a very long
            // identifier) but handled, not overflowed: this ONE var is skipped; SaveWorld.cpp counts
            // at() successes against count() so a caller can tell fewer arrived than promised.
            return 0;
        }
        Marshal.Copy(utf8, 0, (IntPtr)nameBuf, utf8.Length);
        nameBuf[utf8.Length] = 0;

        *outKind = kind;
        GraphVarStore store = host!.VarStore!;   // non-null: Graph is non-null, so Load() ran CreateFor
        switch (v.Type)
        {
            case PinType.Float: *outF = store.GetFloat(v.Name); *outI = 0;    break;
            case PinType.Int:   *outI = store.GetInt(v.Name);   *outF = 0f;   break;
            case PinType.Bool:  *outI = store.GetBool(v.Name) ? 1 : 0; *outF = 0f; break;
            default: return 0;   // unreachable -- SceneKindOf already refused any other PinType
        }
        return 1;
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static unsafe int GraphVarSetProvider(int entity, byte* namePtr, int kind, float f, int i, IntPtr user)
    {
        GraphHost? host = FindPersistedGraphHost(entity);
        GraphVarStore? store = host?.VarStore;
        if (store == null || namePtr == null) return 0;

        string? name = Marshal.PtrToStringUTF8((IntPtr)namePtr);
        if (string.IsNullOrEmpty(name)) return 0;

        // DECLARED, not merely "the store has this key": CreateFor seeds every declared VAR at Load(),
        // so an unrecognised name is one this graph never declared -- applyField's (C++) "field no longer exists" case, matched here for the same reason.
        GraphVariable? declared = host!.Graph?.Variables.Find(v => v.Name == name);
        if (declared == null) return 0;

        // A KIND MISMATCH IS A DROPPED FIELD, not a crash -- applyField's (SaveWorld.cpp) rule for
        // component fields: a save whose VAR changed type between builds must not reinterpret four saved bytes as the wrong kind.
        if (SceneKindOf(declared.Type) != kind) return 0;

        switch (declared.Type)
        {
            case PinType.Float: store.SetFloat(name, f);        break;
            case PinType.Int:   store.SetInt(name, i);          break;
            case PinType.Bool:  store.SetBool(name, i != 0);    break;
            default: return 0;   // unreachable -- SceneKindOf already refused any other PinType
        }
        return 1;
    }

    // Declares the five framework base types as lineage roots (abstract, never MANAGED), then declares
    // "Character" the SAME way a project's [AverClass] type would (DeclareActorClass, below) -- the
    // one base row that is CONCRETE (see AverCharacter's own class comment for why).
    private static void DeclareBaseClasses()
    {
        DeclareBase("Actor", "", ClassFlags.Abstract);
        DeclareBase("Pawn", "Actor", ClassFlags.Pawn | ClassFlags.Abstract);
        DeclareBase("PlayerController", "Actor", ClassFlags.Controller | ClassFlags.Abstract);
        DeclareBase("GameMode", "Actor", ClassFlags.GameMode | ClassFlags.Abstract);
        DeclareBase("GameInstance", "Actor", ClassFlags.GameInstance | ClassFlags.Abstract);
        DeclareActorClass(typeof(AverCharacter));
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

        s_classes[Assets.ObjectIdOf(name)] =
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
        // Checked AHEAD of the plain Pawn arm: AverCharacter derives AverPawn, so without this a
        // character subclass would resolve to the abstract "Pawn" row, missing Character's own
        // archetype/flags. NOT reached for AverCharacter's OWN declaration (its explicit non-"Actor"
        // parent means ResolveClassIdentity never calls this for it) -- if it did, this arm would be
        // reflexively true (IsAssignableFrom accepts the exact type) and Character would parent to
        // itself. See Character.cs's own comment.
        if (typeof(AverCharacter).IsAssignableFrom(type)) return "Character";
        if (typeof(AverPawn).IsAssignableFrom(type)) return "Pawn";
        if (typeof(AverPlayerController).IsAssignableFrom(type)) return "PlayerController";
        if (typeof(AverGameMode).IsAssignableFrom(type)) return "GameMode";
        if (typeof(AverGameInstance).IsAssignableFrom(type)) return "GameInstance";
        return "Actor";
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

    // Constructs the managed instance for a spawned entity. Returns 1 when one was bound. Checks
    // s_classes (C# actor class) FIRST, s_graphClasses (CLASS-declaring .ocgraph) on a miss -- they
    // share one flat aver_fw_class_declare registry by name, so a hash matches at most one table.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static int DispBind(long classNameHash, int entity)
    {
        try
        {
            if (s_classes.TryGetValue(classNameHash, out ClassInfo? info))
            {
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

            if (s_graphClasses.TryGetValue(classNameHash, out GraphClassInfo? ginfo))
            {
                // A FRESH GraphHost PER SPAWNED INSTANCE, reloaded/recompiled from disk, not shared --
                // gives each instance independent VAR storage (GraphVarStore.CreateFor runs once per
                // Load(), "two hosts, two stores"). Cost: re-parsing/re-JITing per instance; a hot path
                // spawning many instances would want a compiled-Graph cache shared across GraphHosts
                // (mirroring ClassInfo's one-declare-many-bind) -- accepted for now since today's scale
                // is tens of instances, not thousands.
                var host = new GraphHost();
                if (!host.Load(ginfo.Path, out string? err))
                {
                    Emit((int)Log.Level.Error,
                         $"[Graph] class '{ginfo.Name}' entity {entity}: its graph failed to load: {err}");
                    return 0;
                }

                // THE COMPONENT TREE, BEFORE ANY OF THIS INSTANCE'S OWN CODE RUNS: OnStart may look up
                // a component by name, so every child must exist first. Not conditional on play state:
                // aver_fw_spawn_preview binds without OnBeginPlay, so a preview instance runs no graph code -- exactly where seeing what the actor is MADE of matters most.
                if (ginfo.Components.Count > 0)
                    GraphComponentTree.Build(new Entity(entity), ginfo.Components, ginfo.Name);

                // THE OTHER HALF OF THE GRAPH BRANCH, and why a graph class can end up "BOTH TABLES,
                // ONE ENTITY": if this graph's native parent chain reaches a C#-declared class
                // (s_classes) -- today, "Character" -- construct that type too and bind it into
                // s_actorsByEntity like the plain C# branch, so CharacterMoveForGraph's
                // Actors.Get(entity) finds a real AverCharacter with a capsule and a view, not
                // nothing. Ticks is hardcoded FALSE (no ClassInfo for an on-the-fly ancestor): the
                // GRAPH owns the tick, and AverCharacter overrides no OnTick of its own
                // (FindNativeAncestorType's own comment) -- this bound-but-never-bucketed instance is enough for DriveFromGraph.
                Type? ancestorType = FindNativeAncestorType(ginfo.Name);
                if (ancestorType is not null)
                {
                    var ancestor = (AverActor)Activator.CreateInstance(ancestorType)!;
                    ancestor.Self = new Entity(entity);

                    // view= is the ONLY CLASS attribute targeting a specific ancestor type (mesh=/
                    // material= apply to any class via the native component ABI; this sets a plain C#
                    // field only AverCharacter declares) -- `ancestor is AverCharacter` is that check, not an error.
                    if (!string.IsNullOrEmpty(ginfo.View) && ancestor is AverCharacter character)
                    {
                        if (string.Equals(ginfo.View, "firstperson", StringComparison.OrdinalIgnoreCase))
                            character.CameraViewMode = CameraView.FirstPerson;
                        else if (string.Equals(ginfo.View, "thirdperson", StringComparison.OrdinalIgnoreCase))
                            character.CameraViewMode = CameraView.ThirdPerson;
                        else
                            Emit((int)Log.Level.Warn,
                                 $"[Graph] class '{ginfo.Name}': view='{ginfo.View}' is neither "
                                 + "'firstperson' nor 'thirdperson' -- camera stays at its C# default");
                    }

                    s_actorsByEntity[entity] = new ActorLive
                    {
                        Instance = ancestor,
                        Entity = entity,
                        TickGroup = 0,
                        Ticks = false,
                        Name = ginfo.Name,
                    };
                }

                s_graphInstances[entity] = new GraphInstanceLive { Host = host, Ticks = ginfo.Ticks };
                return 1;
            }

            return 0;
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

    // Ticks every actor in one group.
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void DispTickAll(int group, float dt)
    {
        if (group < 0 || group >= TickGroupCount) return;

        // EnhancedInput needs no per-frame snapshot here: that call is gone, not relocated -- it's now
        // a thin wrapper over aver_fw_action_held/pressed/released/value2 (framework_abi.h NAMED
        // ACTIONS, minor 5), reading InputState's cur/prev/mouse/prevMouse ON DEMAND (EnhancedInput.cs),
        // the same bytes aver_fw_input_key reads, so every actor agrees on input state regardless of
        // tick order. A SNAPSHOT, not the live list: DispUnbind removes from s_tickBuckets[group]
        // mid-walk, so destroying an actor during OnTick would otherwise shift later elements down and
        // skip whoever slid in -- a silent lost frame (a bounds check alone wouldn't have caught it);
        // a spawned actor ticks next frame instead.
        ActorLive[] snapshot = s_tickBuckets[group].ToArray();
        foreach (ActorLive live in snapshot)
        {
            if (live.Disabled) continue;
            // Destroyed earlier in THIS walk: DispUnbind drops it from s_actorsByEntity, so absence
            // is the liveness test. Reference equality, not mere presence -- a reused entity id may
            // now hold a different actor.
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

    // Drops the instance from the entity map and tick bucket, then calls OnUnbound. Also the teardown
    // edge for a graph-class instance (DispBind's own comment): ClassFlags.Managed means
    // aver_fw_destroy/end_play's sweep calls this as for a C# actor, stopping ticking of a destroyed
    // entity and releasing its GraphHost/VAR storage (GraphHost's only entry points are
    // OnStart/OnTick, none meaning "being destroyed", so it has no OnUnbound-equivalent hook to call).
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void DispUnbind(int entity)
    {
        if (s_actorsByEntity.Remove(entity, out ActorLive? live))
        {
            if (live.Ticks && live.TickGroup >= 0 && live.TickGroup < TickGroupCount)
                s_tickBuckets[live.TickGroup].Remove(live);

            try { live.Instance.OnUnbound(); }
            catch (Exception ex) { Emit(3, $"[bridge] OnUnbound threw for entity {entity}: {ex.Message}"); }
        }

        // NOT an "else" and NOT an early return after the block above: DispBind's graph branch can
        // populate BOTH s_actorsByEntity (a Character-parented graph's ancestor) AND s_graphInstances
        // (its own GraphHost) for the SAME entity. An early return here was correct only while the two
        // tables were mutually exclusive -- once both held one entity, it leaked the GraphHost (and its
        // VAR storage) and left GraphTickBoundInstances ticking a destroyed entity forever.
        s_graphInstances.Remove(entity);

        // Drop this entity's warn-once memory (s_fireWarnedOnce's own comment): a reused entity id
        // deserves its own first FireEvent warning, not inherited silence. RemoveWhere has no
        // per-entity index, but this set only ever holds a handful of misfired-at entities -- not worth a second index.
        s_fireWarnedOnce.RemoveWhere(pair => pair.Entity == entity);
    }

    // GAP 3: FireEvent's router -- installed onto Aver.Graph.GraphEvents.Router by SetupManagedActors
    // (see that method's own comment for why, and the two-table check order). Logs once per distinct
    // (entity, event) pair; never throws -- GraphEvents.FireEventForGraph runs inside compiled IL with
    // no try/catch, so an escaping exception would propagate into whatever Tick()/Fire() is running --
    // the same "runtime errors are not swallowed" contract GraphHost's own class comment documents.
    // FindGraphHost: Entity -> live GraphHost, checking BOTH tables (factored out of FireEventRouter so
    // the graph-VAR save provider below can reach the same entity); s_graphInstances first, s_graphs
    // second so a class-spawned host wins on collision -- s_graphs is the drone/MCP "caller manages
    // this graph by hand" table and in ordinary play never collides with a real spawned entity's id.
    private static GraphHost? FindGraphHost(int entity)
    {
        if (s_graphInstances.TryGetValue(entity, out GraphInstanceLive? instanceLive))
            return instanceLive.Host;
        if (s_graphs.TryGetValue(entity, out GraphHost? bareHost))
            return bareHost;
        return null;
    }

    // GraphVarCountProvider/At/Set must see ONLY GameInstance-or-subclass entities -- an explicit,
    // later user decision narrowing "every entity with a live GraphHost" (same aver_fw_class_get_flags
    // check other class-kind gates use; flags propagate through inheritance, so this also matches the
    // built-in GameInstance base, not just a C# AverGameInstance subclass -- no new ABI needed).
    // Deliberately NOT used by FireEventRouter, which must reach every entity.
    private static GraphHost? FindPersistedGraphHost(int entity)
    {
        int c = Fw.aver_fw_class_of(entity);
        if (c == 0 || (Fw.aver_fw_class_get_flags(c) & ClassFlags.GameInstance) == 0)
            return null;
        return FindGraphHost(entity);
    }

    private static bool FireEventRouter(int targetEntity, string eventName)
    {
        GraphHost? host = FindGraphHost(targetEntity);

        if (host == null)
        {
            // A C# ACTOR IS THE THIRD KIND OF THING THAT CAN RECEIVE ONE, checked only after both graph
            // tables miss. Not arbitrary: an entity binds to a graph OR a C# actor, never both (DispBind
            // constructs one or the other), so this is a fallback, not a second delivery. An animation
            // notify aimed at a C# character used to die here with a "no live graph" warning -- the wrong problem named.
            if (s_actorsByEntity.TryGetValue(targetEntity, out ActorLive? live) && !live.Disabled)
            {
                try
                {
                    if (live.Instance.OnEvent(eventName)) return true;
                }
                catch (Exception ex)
                {
                    // Same treatment as every other actor hook: one bad actor is disabled, the caller
                    // is told it was not handled, and nothing propagates into compiled IL or the ABI.
                    DisableActor(live, "OnEvent", ex);
                    return false;
                }
                if (s_fireWarnedOnce.Add((targetEntity, eventName)))
                    Emit((int)Log.Level.Warn,
                         $"[Graph] FireEvent: entity {targetEntity} is a C# actor ({live.Name}) whose " +
                         $"OnEvent did not handle '{eventName}'");
                return false;
            }

            if (s_fireWarnedOnce.Add((targetEntity, eventName)))
                Emit((int)Log.Level.Warn,
                     $"[Graph] FireEvent: entity {targetEntity} has no live graph or actor to fire '{eventName}' at");
            return false;
        }

        bool ok = host.FireForEntity(eventName, targetEntity, out _, out string? refusal);
        if (!ok && s_fireWarnedOnce.Add((targetEntity, eventName)))
            Emit((int)Log.Level.Warn,
                 $"[Graph] FireEvent: entity {targetEntity}: " +
                 (refusal ?? $"no on-demand event '{eventName}'"));
        return ok;
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
