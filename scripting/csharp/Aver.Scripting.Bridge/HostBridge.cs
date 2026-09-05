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
// Assets.ObjectIdOf -- the ONE managed spelling of the engine's fnv1a64. This file used to carry a
// private copy of the algorithm, which is the thing an asset id must never have two of: Hash.hpp's
// own comment says it "must match the C# side's spelling in Aver.Scene/Native.cs", and a third
// spelling here made that sentence untrue.
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
    // whether that graph wants ticking at all (declares an OnStart or OnTick ENTRY). Deliberately NOT
    // a compiled Graph/delegate cache -- see DispBind's own comment for why every spawned instance
    // reloads and recompiles the file fresh, exactly the same "each GraphHost gets its own compile"
    // shape the drone and every other GraphHost caller already has.
    private sealed class GraphClassInfo
    {
        public required string Path;
        public required string Name;
        public required bool Ticks;

        // Raw text of the CLASS record's `view=` attribute (see Graph.ClassView), or null when the
        // record omitted it. Applied in DispBind, once per spawned instance, to the ancestor's
        // CameraViewMode -- meaningless (and simply unused) when this class's native ancestor is not
        // AverCharacter.
        public string? View;

        // The class graph's COMPONENT TREE, parsed ONCE here at registration and replayed per
        // spawned instance in DispBind. Empty for a class that declares no COMP records, which is
        // every class that predates them.
        //
        // Kept here rather than re-read from the GraphHost each bind for a reason worth stating: the
        // host is reloaded and recompiled from disk per instance (see DispBind's own comment on why),
        // so reaching through it for this would tie the component tree to that per-instance reparse.
        // A component tree is a property of the CLASS -- every instance gets the same one -- so it is
        // read where the class is declared.
        public required List<GraphComponent> Components;
    }

    // Walks the NATIVE parent chain from `className` (via aver_fw_class_parent, not this graph's OWN
    // declared-parent string) until it finds a name registered in s_classes, or runs out of chain.
    // Native rather than a precomputed field on GraphClassInfo, and resolved at BIND time rather than
    // DECLARE time, because a graph's parent may itself be another graph class not yet declared when
    // DeclareGraphClasses processes this file (declare order is file-path-sorted, not
    // parent-before-child) -- exactly the same "resolved by name, not by declare order" contract
    // aver_fw_class_seal's own flatten() already relies on. By bind time every class -- C# or graph --
    // that will ever exist this session has been declared and sealed, so the walk cannot dead-end on
    // an ordering accident.
    //
    // THIS IS THE MECHANISM THAT MAKES "CLASS AN_Player Character" PRODUCE A REAL, DRIVABLE CHARACTER:
    // a graph class carries no C# type of its own, so without this, CharacterMoveForGraph's
    // Actors.Get(entity) would always resolve null for a graph-declared entity, exactly as it did
    // before this change (see this method's own probe evidence in the out-of-box design notes). One
    // C# ancestor at most is constructed -- the NEAREST one -- mirroring single inheritance: a graph
    // parented to a graph parented to "Character" still finds Character, not some closer non-C# link
    // in between.
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

    // Declared graph classes, keyed by fnv1a64 of the class name -- the SAME hash native spawn hands
    // bind(), and the SAME table shape as s_classes just above (a disjoint key space: a class name is
    // registered as EITHER a C# actor class OR a graph class, never both, since aver_fw_class_declare
    // is one flat registry by name).
    private static readonly Dictionary<long, GraphClassInfo> s_graphClasses = new();

    // One graph-class INSTANCE: its own GraphHost (so its VAR storage is independent of every other
    // instance of the same class -- see GraphVarStore's own "two hosts, two stores" comment) and
    // whether it wants automatic per-frame ticking.
    private sealed class GraphInstanceLive
    {
        public required GraphHost Host;
        public required bool Ticks;
    }

    // Every LIVE graph-class instance, keyed by entity -- populated by DispBind, dropped by
    // DispUnbind. Walked once a frame by GraphTickBoundInstances (see its own comment for why that
    // walk is UNGATED on aver_fw_play_state(), unlike the C# actor tick buckets above). Deliberately a
    // SEPARATE table from s_graphs (below): s_graphs is the drone/MCP-harness "a caller manages this
    // entity's graph by hand" table, ticked only when that caller explicitly calls GraphTick: mixing a
    // class-spawned instance into it would either double-tick it (once here, once by
    // GraphTickBoundInstances) or require every existing s_graphs caller to start filtering out
    // entities it never bound itself.
    private static readonly Dictionary<int, GraphInstanceLive> s_graphInstances = new();

    // ---- GAP 3: FireEvent's entity-to-GraphHost router ------------------------------------------
    //
    // (entity, eventName) pairs this process has already logged a "no live graph" / "no such event"
    // refusal for, so an OnTick-driven FireEvent aimed at a permanently-dead target warns exactly
    // ONCE rather than flooding the log every single frame thereafter -- a real hazard the design
    // phase for this slice named explicitly (a stray FireEvent on an OnTick chain, aimed at a target
    // that will never exist, is exactly the shape a level author is most likely to actually author by
    // mistake). Cleared per-entity in DispUnbind: an entity id can be REUSED by a later spawn within
    // the same session, and a fresh occupant of that id deserves its own first warning, not silence
    // inherited from whatever used to live there.
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

    /// <summary>Raises <paramref name="utf8EventName"/> on whatever graph is bound to
    /// <paramref name="entity"/>. Returns 1 if a handler ran, 0 otherwise.
    ///
    /// THE NATIVE ENTRY TO THE EVENT ROUTER, and deliberately nothing more than that: it unwraps a
    /// UTF-8 pointer and hands both arguments to <see cref="FireEventRouter"/> -- the SAME closure
    /// a FireEvent node's compiled IL reaches through GraphEvents.Router. One router, so a footstep
    /// fired by an animation notify resolves its target exactly as one fired by a graph does
    /// (s_graphInstances first, then s_graphs), logs on the same once-per-pair rule, and cannot
    /// drift from it.
    ///
    /// It does NOT go through GraphEvents.FireEventForGraph, whose depth guard counts nesting on
    /// one call stack. A notify is a fresh stack from the native tick, at depth zero by
    /// construction; anything the handler fires onward enters that guard normally at the node that
    /// fires it. Routing through it here would have spent one of the eight allowed levels on the
    /// call that cannot recurse.</summary>
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
            // NEVER THROWS ACROSS THE ABI. An exception escaping an UnmanagedCallersOnly frame
            // tears the process down with no usable diagnostic, and this one is called from inside
            // an animation tick that has no idea it is talking to managed code.
            Emit((int)Log.Level.Error, $"[Graph] entity {entity}: fire threw: {Describe(ex)}");
            return 0;
        }
    }

    // ------------------------------------------------------------------ graph classes
    //
    // GRAPH-AS-CLASS: a .ocgraph carrying a CLASS record becomes a real registered actor class,
    // exactly the way DeclareActorClass registers a C# type -- same aver_fw_class_declare /
    // set_flags(MANAGED) / seal sequence, same class registry the framework spawns out of. What is
    // different is WHAT gets bound to a spawned instance: DeclareActorClass's DispBind constructs a
    // C# AverActor; a graph class's DispBind (see its own comment, below) constructs a fresh GraphHost
    // per instance instead, bound to that instance's own real entity.

    /// <summary>Scans <paramref name="utf8ContentDir"/> recursively for *.ocgraph files and declares
    /// one framework class per file that carries a CLASS record (see OcGraphParser's own comment on
    /// that record, and Graph.ClassName). Returns the number of classes declared. A file with no
    /// CLASS record is silently skipped here -- it is not this pass's concern; GameApp's own
    /// discoverProjectGraphs (native side) is what drives a CLASS-less graph, against a synthetic
    /// entity, exactly as it always has.
    ///
    /// SAFE TO CALL WITH NO GRAPHS, OR NO DIRECTORY AT ALL: returns 0, changes nothing else -- the
    /// same "empty is a no-op, not an error" contract LoadScripts already has for an empty/missing
    /// scripts directory.</summary>
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int DeclareGraphClasses(IntPtr utf8ContentDir)
    {
        try
        {
            string? dir = Marshal.PtrToStringUTF8(utf8ContentDir);
            if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir))
                return 0;

            int declared = 0;
            // `pawn=` assignments, applied in a SECOND pass below rather than inline. See Graph.ClassPawn:
            // aver_fw_class_set_default_pawn resolves the name at SEAL, so a GameMode naming a pawn whose
            // own class had not been declared yet would resolve to 0 and possess nothing -- and which
            // graphs suffered that would depend on the filename order this very loop sorts to make
            // deterministic. Deferring until every class exists removes the ordering question entirely.
            var pendingRoles = new List<(int Handle, string ClassName, string? Pawn, string? Controller, string Path)>();
            // Sorted, like LoadScripts' own assembly enumeration: deterministic declare order matters
            // when two files disagree about the same class name (aver_fw_class_declare is idempotent
            // by name -- the LAST declare wins), so which one wins must not depend on the filesystem's
            // own enumeration order.
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
                    // Only worth a warning when the file LOOKS like it wanted to declare a class --
                    // a plain parse failure in an ordinary (non-class) graph is discoverProjectGraphs'
                    // own concern, and it already reports it when it tries to load the same file.
                    if (text.Contains("\nCLASS ", StringComparison.OrdinalIgnoreCase) ||
                        text.StartsWith("CLASS ", StringComparison.OrdinalIgnoreCase))
                        Emit((int)Log.Level.Warn,
                             $"[Graph] '{path}' looks like it declares a CLASS but failed to parse: {parseErr}");
                    continue;
                }

                // NOT EVERY .ocgraph IS A GAMEPLAY GRAPH. This loop reaches every one under the
                // project's content directory, and a material graph's nodes mean nothing to this
                // compiler -- see Graph.DomainKind. Checked before ClassName rather than after so
                // that a foreign graph carrying a CLASS record (a material graph named after the
                // material it shades, say) is skipped rather than declared as an actor class.
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
                // this class's instances every frame -- see that method's own comment for why that walk
                // is a flat, UNGATED-on-play-state loop rather than a tick-group bucket the way a C#
                // actor class's Ticks/TickGroup pair would be: a graph-only project (this feature's own
                // reason to exist) never calls aver_fw_begin_play at all (no C# GameMode to find --
                // see GameApp.hpp's beginPlayIfGameModeDeclared comment), so aver_fw_play_state() would
                // stay EDITOR forever and a tick-group-routed instance would never once run OnTick --
                // exactly the "silently inert in exactly the configuration most likely to be the only
                // gameplay a project has" trap tickProjectGraphs' own comment already names for the
                // synthetic-entity path. The SAME reasoning applies here, one layer down the stack.
                bool ticks = graph.EntryPoints.Any(e => e.EventName == "OnStart" || e.EventName == "OnTick");

                // TWO FILES CLAIMING ONE CLASS NAME IS SAID OUT LOUD. The declare order above is
                // sorted so that WHICH file wins is deterministic rather than filesystem-dependent,
                // but determinism is not the same as visibility: without this, both files logged an
                // identical-looking "declared class" line, the count said two classes were declared,
                // and only the alphabetically-last graph ever ran. Nothing distinguished that from
                // two independent classes declaring successfully, so the symptom -- one graph simply
                // never executing -- looked like a bug in the graph rather than in the naming.
                //
                // A warning rather than a refusal: the last-wins behaviour is aver_fw_class_declare's
                // own (it is idempotent by name), it is deterministic here, and refusing both would
                // turn a rename-in-progress into a level that cannot load at all.
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

            // ---- second pass: `pawn=`, now that every class name above exists ----------------------
            //
            // Re-sealing is the documented way to do this, not a workaround: aver_fw_class_set_default_pawn
            // clears `sealed` itself precisely so the name can be (re)resolved, and spawning auto-seals
            // anyway. Sealing here rather than leaving it to the spawn means a bad name is reported NOW,
            // by this loop, instead of becoming a GameMode that silently possesses nothing at begin-play.
            foreach (var (handle, className, pawnName, controllerName, path) in pendingRoles)
            {
                // GameMode ONLY. Everywhere else these are meaningless -- ClassRecord::defaultPawn and
                // ::playerController are read by aver_fw_begin_play off the GameMode class and nowhere
                // else -- so saying so is more useful than silently doing nothing. A warning, not a
                // refusal, matching how `view=` on a non-Character class is tolerated: one misapplied
                // attribute should not cost a project its class.
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

                // THE HALF-WIRED CASE IS CALLED OUT, because it looks correct and does nothing.
                // aver_fw_begin_play possesses only when it has BOTH ("if (ctrl && pawn)"), and the
                // built-in PlayerController is ABSTRACT so it cannot be the fallback -- a GameMode with
                // a pawn and no controller of its own spawns the pawn, never possesses it, and leaves
                // GameApp's camera following nothing. That is exactly the failure this attribute pair
                // was added to end, so it must not be reintroduced silently by naming only one of them.
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

    /// <summary>Ticks every LIVE graph-class instance (see s_graphInstances) once. Called every frame
    /// from BOTH composition roots, UNGATED on aver_fw_play_state() -- see DeclareGraphClasses' own
    /// comment on `ticks` for exactly why: a graph-only project has no C# GameMode to ever begin a
    /// play session with, so gating this on PLAYING would make graph-as-class silently inert in the
    /// one configuration it exists for. A per-instance exception unloads just that instance (drops it
    /// from s_graphInstances) rather than the whole walk, mirroring GraphTick's own per-entity
    /// isolation.</summary>
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static void GraphTickBoundInstances(float dt)
    {
        if (s_graphInstances.Count == 0) return;

        // A SNAPSHOT, not the live table -- mirrors DispTickAll's own reasoning exactly: an instance
        // destroyed (DispUnbind) earlier in THIS walk must not be ticked, and one spawned during this
        // walk should tick next frame, not this one. The ReferenceEquals re-check below additionally
        // catches an entity id reused by a new spawn within the same tick, the identical hazard
        // DispTickAll's own comment documents.
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
            // GAP 3: installs the FireEvent router -- see GraphEvents.Router's own doc comment for why
            // this indirection exists at all (Aver.Graph cannot see this file). Mirrors Actors.Resolver
            // immediately above in shape (a closure over this file's own tables, installed once at
            // bootstrap) but checks TWO tables, in a deliberate order: s_graphInstances (a class-
            // spawned instance's own GraphHost -- see the "graph classes" region above) FIRST, then
            // s_graphs (the drone/MCP-harness/project-graph table -- see the "graph hosting" region).
            // Collision between the two is structurally impossible for GameApp's own project graphs
            // (GameApp.discoverProjectGraphs keys s_graphs with strictly NEGATIVE synthetic ids, never
            // a real entity), but SandboxApp's own graph-driven drone CAN legitimately hold a real,
            // positive entity id in s_graphs, so the order is still a real, stated choice: a class-
            // spawned instance's own host wins if an id were ever to appear in both.
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
    //
    // A SEPARATE provider pair from ManagedDispatch above, deliberately: this answers a save, not an
    // actor lifecycle event, and reuses aver_fw_set_save_provider/aver_fw_set_anim_curve_provider's
    // OWN shape (framework_abi.h) -- a small, independently-installable trio -- rather than growing
    // ManagedDispatch's versioned struct for a concern that has nothing to do with bind/tick/endPlay.
    // Nothing stops the INSTALLER being C# here where every prior "set_provider" call in this engine
    // happens to be C++-to-C++: the mechanism (a plain cdecl function pointer plus a void* user, with
    // a null provider answering 0/false) does not care which language calls it, only that ONE
    // composition root calls it. See modules/save/include/aver/save/SaveWorld.hpp's own comment on
    // GraphVarCountFn for why the save module needs this seam at all -- GraphVarStore is pure managed
    // state with no representation in the native scene.
    //
    // SCOPED TO GameInstance: an explicit, later user decision -- VARs persist ONLY for entities
    // whose class is GameInstance or a subclass, not every graph-hosted entity. All three provider
    // methods below resolve the host through FindPersistedGraphHost (which layers the class-flag
    // check on top of FindGraphHost), never FindGraphHost directly.
    // Routed through Fw.aver_fw_set_graph_var_provider (Aver.Framework/Native.cs), NOT a raw
    // [DllImport("Aver.Framework")] declared in this assembly -- see that declaration's own comment
    // for why a bridge-local DllImport resolves to the wrong same-name DLL and fails with
    // EntryPointNotFoundException despite the native export genuinely existing.
    private static unsafe void InstallGraphVarProvider()
    {
        Fw.aver_fw_set_graph_var_provider(
            (IntPtr)(delegate* unmanaged[Cdecl]<int, IntPtr, int>)&GraphVarCountProvider,
            (IntPtr)(delegate* unmanaged[Cdecl]<int, int, byte*, int, int*, float*, int*, IntPtr, int>)&GraphVarAtProvider,
            (IntPtr)(delegate* unmanaged[Cdecl]<int, byte*, int, float, int, IntPtr, int>)&GraphVarSetProvider,
            IntPtr.Zero);
    }

    // A VAR's declared PinType, as the AVER_SCENE_KIND_* the save format's OcSaveField.kind carries
    // (scene_abi.h) -- NOT the same integer as PinType's own ordinal (Float=0,Int=1,Bool=2 there;
    // F32=0,I32=3,BOOL=4 here), so this is a real translation, not a cast. -1 for Exec, which a VAR
    // can never declare (OcGraphParser rejects "VAR ... exec" at parse time) and which every caller
    // below therefore treats as "this provider has a bug", not a normal refusal.
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
        // Graph.Variables, not VarStore: the STORE holds only whatever has been written so far
        // (nothing, on a freshly-bound instance), while Variables is the DECLARED list -- every VAR
        // this graph has, in file order, whether or not anything has touched it yet. Capture must see
        // all of them, seeded default or not, or a save silently omits a variable nobody has written
        // this session. FindPersistedGraphHost, not FindGraphHost: a non-GameInstance entity reports
        // zero VARs here, so SaveWorld.cpp's captureGraphVars sees "nothing to capture" for it, exactly
        // as if it had no GraphHost at all.
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
        // (GraphVariable.Name), and handing native code a raw pointer into it would be a GC hazard
        // the moment anything moved -- the same reason every other string in this file crosses the
        // ABI by value (aver_scene_get_str) or, here for the first time in this direction, by copy.
        byte[] utf8 = Encoding.UTF8.GetBytes(v.Name);
        if (utf8.Length >= nameBufLen)
        {
            // A VAR name longer than the buffer -- vanishingly unlikely (63 UTF8 bytes is a very
            // long identifier) but handled rather than overflowing: this ONE var is skipped: entity
            // '{}': the SaveWorld.cpp code counts how many at() calls actually succeeded against the
            // count() this call belongs to, so a caller can tell fewer arrived than were promised.
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

        // DECLARED, not merely "the store happens to have this key" -- CreateFor seeds every
        // declared VAR at Load() time, so a name the store does not recognise is a name this
        // graph never declared, exactly the "field no longer exists" case applyField (C++) treats
        // as a dropped field rather than a crash. Matched here for the identical reason.
        GraphVariable? declared = host!.Graph?.Variables.Find(v => v.Name == name);
        if (declared == null) return 0;

        // A KIND MISMATCH IS A DROPPED FIELD, not a crash -- the same rule applyField (SaveWorld.cpp)
        // already enforces for ordinary component fields, applied here for the identical reason: a
        // save whose VAR changed declared type between builds must not reinterpret four saved bytes
        // as the wrong kind.
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

    // Declares the five framework base types as lineage roots. Abstract, and never MANAGED. Then
    // declares "Character" the SAME way a project's own [AverClass] type would (DeclareActorClass,
    // below) -- it is the one base row that is CONCRETE rather than an anchor, so it goes through the
    // real actor-class path (Configure, flags, seal, s_classes registration) instead of DeclareBase's
    // bare declare-and-seal. See AverCharacter's own class comment for why concretising it, rather than
    // shipping a second empty subclass, is the shape this engine uses.
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
        // Checked AHEAD of the plain Pawn arm: AverCharacter itself derives AverPawn, so without this a
        // character subclass (DemoPawn, say) would resolve to the abstract "Pawn" row and never pick up
        // "Character" 's own archetype/flags. NOT reached by AverCharacter's OWN declaration: its
        // [AverClass("Character", Parent = "Pawn")] states an explicit, non-"Actor" parent, so
        // ResolveClassIdentity never calls BaseRegistryName for it at all -- if it did, this arm would
        // be reflexively true for AverCharacter itself (IsAssignableFrom accepts the exact type) and
        // Character would parent to Character. See Character.cs's own comment on that attribute.
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
    // s_classes (a C# actor class) FIRST and s_graphClasses (a CLASS-declaring .ocgraph) on a miss --
    // the two share one flat aver_fw_class_declare registry by name, so a class-name hash can only
    // ever match one of the two tables, never both.
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
                // A FRESH GraphHost PER SPAWNED INSTANCE, reloaded and recompiled from disk here --
                // not a shared compiled-Graph cache reused across instances. This is what gives two
                // instances of the same graph class independent VAR storage: GraphVarStore.CreateFor
                // is called once per Load() (see GraphHost's own doc comment on "two hosts, two
                // stores"), so two separate GraphHost objects are the entire mechanism, with nothing
                // extra needed here to keep them apart. The cost is re-parsing and re-JITing the same
                // file once per instance; a hot path spawning many instances of one graph class would
                // want a compiled-Graph cache shared across GraphHosts (mirroring ClassInfo's own
                // one-declare-many-bind shape) -- a real, named limitation this slice accepts rather
                // than hides, since the scale visual scripting targets today is tens of instances, not
                // thousands.
                var host = new GraphHost();
                if (!host.Load(ginfo.Path, out string? err))
                {
                    Emit((int)Log.Level.Error,
                         $"[Graph] class '{ginfo.Name}' entity {entity}: its graph failed to load: {err}");
                    return 0;
                }

                // THE COMPONENT TREE, BEFORE ANY OF THIS INSTANCE'S OWN CODE RUNS. Ordering matters
                // in one direction only, and this is it: OnStart may reasonably look up a component
                // by name (a muzzle to fire from, a mesh to hide), so every child has to exist before
                // the graph gets a chance to ask. Nothing here depends on the graph having run.
                //
                // Not conditional on play state, and deliberately: aver_fw_spawn_preview binds without
                // dispatching OnBeginPlay, so a preview-spawned instance runs no graph code at all --
                // and it is exactly the case where seeing what the actor is MADE of matters most,
                // because that is the editor placing one.
                if (ginfo.Components.Count > 0)
                    GraphComponentTree.Build(new Entity(entity), ginfo.Components, ginfo.Name);

                // THE OTHER HALF OF THE GRAPH BRANCH, AND THE REASON A GRAPH CLASS CAN NOW END UP
                // "BOTH TABLES, ONE ENTITY": if this graph's native parent chain reaches a class C#
                // actually declared (s_classes) -- today, in practice, "Character" -- construct that
                // C# type too and bind it into s_actorsByEntity exactly as the plain C# branch above
                // does, so CharacterMoveForGraph's Actors.Get(entity) finds a real AverCharacter with a
                // capsule and a view, not nothing. Ticks is hardcoded FALSE here, never info.Ticks off
                // some ClassInfo -- there is no ClassInfo for an on-the-fly ancestor construction, and
                // deliberately so: the GRAPH owns the tick (GraphTickBoundInstances), and AverCharacter
                // overrides no OnTick of its own (see FindNativeAncestorType's own comment) -- a
                // bound-but-never-bucketed instance is exactly enough for DriveFromGraph to reach.
                Type? ancestorType = FindNativeAncestorType(ginfo.Name);
                if (ancestorType is not null)
                {
                    var ancestor = (AverActor)Activator.CreateInstance(ancestorType)!;
                    ancestor.Self = new Entity(entity);

                    // view= is the ONLY CLASS attribute that targets a specific ancestor type rather
                    // than every actor alike (mesh=/material= apply to any class's entity via the
                    // native component ABI; this one sets a plain C# field that only AverCharacter
                    // declares). A graph parented to something other than Character simply has no field
                    // to set here -- `ancestor is AverCharacter` is that check, not an error path.
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

        // GROUP 0 USED TO ALSO CALL EnhancedInput.Update() HERE, refreshing every action's value
        // before the first tick group ran so every actor in the frame agreed on a "was pressed" edge
        // regardless of tick order. That call is GONE, not just relocated: EnhancedInput.cs is now a
        // thin wrapper over aver_fw_action_held/pressed/released/value2 (framework_abi.h's NAMED
        // ACTIONS section, minor 5), which read InputState's cur/prev/mouse/prevMouse ON DEMAND --
        // the SAME bytes aver_fw_input_key already reads -- so there is no separate per-frame copy
        // left for this dispatcher to roll. The cross-actor-agreement guarantee above still holds; it
        // now falls out of every actor reading the identical native state instead of a C#-side
        // snapshot this method used to take once per frame. See EnhancedInput.cs's own top-of-file
        // comment for the rest of the reasoning.
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

    // Drops the instance from the entity map and its tick bucket, then calls its OnUnbound. Also the
    // teardown edge for a graph-class instance (see DispBind's own comment): a graph class carries
    // ClassFlags.Managed, so aver_fw_destroy/aver_fw_end_play's sweep call this exactly as they would
    // for a C# actor -- dropping it from s_graphInstances is what stops GraphTickBoundInstances from
    // ticking a destroyed entity, and what releases its GraphHost (and, with it, its VAR storage) for
    // collection. GraphHost has no OnUnbound-equivalent hook to call -- an event-driven graph's only
    // declared entry points are OnStart/OnTick(/on-demand), none of which mean "I am being destroyed".
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

        // NOT an "else" and NOT an early return after the block above: DispBind's graph branch can now
        // populate BOTH s_actorsByEntity (a Character-parented graph's constructed ancestor) AND
        // s_graphInstances (its own GraphHost) for the SAME entity. This used to be `if (...Remove(...))
        // { ...; return; }` then unconditionally remove from s_graphInstances -- correct when the two
        // tables were mutually exclusive, but with both populated for one entity that early return
        // skipped this line entirely: the GraphHost (and its VAR storage) leaked, and worse,
        // GraphTickBoundInstances' snapshot walk had no way to know the entity died, so it kept calling
        // Host.Tick(entity, dt) against a destroyed entity every frame thereafter.
        s_graphInstances.Remove(entity);

        // Drop this entity's own warn-once memory (see s_fireWarnedOnce's own comment) -- an entity id
        // CAN be reused by a later spawn within the same session, and whatever occupies it next
        // deserves its own first FireEvent warning, not silence left over from whoever used to live
        // here. RemoveWhere over a HashSet<(int,string)> has no per-entity index to key off, but this
        // set only ever holds entries for entities a FireEvent node has actually misfired at, which in
        // practice is a handful at most -- not a hot path worth a second index.
        s_fireWarnedOnce.RemoveWhere(pair => pair.Entity == entity);
    }

    // GAP 3: FireEvent's router -- installed onto Aver.Graph.GraphEvents.Router by SetupManagedActors
    // (see that method's own comment for why the installation lives there and why the two tables are
    // checked in this order). Logs (once per distinct (entity, event) pair -- see s_fireWarnedOnce)
    // and returns false on refusal; never throws -- GraphEvents.FireEventForGraph is itself called
    // from inside compiled IL with no surrounding try/catch of its own, so an exception escaping THIS
    // closure would propagate out of whatever Tick()/Fire() call is currently running, exactly the
    // same "runtime errors are not swallowed" contract GraphHost's own class comment already documents
    // for every other node -- this method simply never manufactures one of its own to swallow.
    // Entity -> live GraphHost, checking BOTH tables a graph can be reached through -- factored out
    // of FireEventRouter (its own original home) so the graph-VAR save provider below can reach the
    // identical entity, with no second, possibly-drifting notion of "which host owns this entity".
    // s_graphInstances first, s_graphs second: a class-spawned instance's own host wins if an id
    // were ever to appear in both (s_graphs is the drone/MCP-harness "a caller manages this entity's
    // graph by hand" table and, in ordinary play, never collides with a real spawned entity's id --
    // see s_graphInstances' own field comment for the fuller account).
    private static GraphHost? FindGraphHost(int entity)
    {
        if (s_graphInstances.TryGetValue(entity, out GraphInstanceLive? instanceLive))
            return instanceLive.Host;
        if (s_graphs.TryGetValue(entity, out GraphHost? bareHost))
            return bareHost;
        return null;
    }

    // GraphVarCountProvider/At/Set below must see ONLY entities whose class is GameInstance or a
    // subclass -- an explicit, later user decision narrowing what was originally "every entity with
    // a live GraphHost". Reuses the class-flag check every other class-kind gate in this file already
    // uses (see the aver_fw_class_get_flags(... & ClassFlags.Managed/GameMode ...) calls elsewhere) --
    // flags propagate through inheritance at class-declare time, so this also matches a graph class
    // parented to the built-in "GameInstance" base (DeclareBaseClasses, ClassFlags.GameInstance),
    // not just a C# AverGameInstance subclass. No new native ABI: aver_fw_class_of/get_flags already
    // exist. Deliberately NOT used by FireEventRouter -- FireEvent must keep reaching every entity.
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
            // A C# ACTOR IS THE THIRD KIND OF THING THAT CAN RECEIVE ONE, checked only after both
            // graph tables miss. The order is not arbitrary: an entity is bound to a graph OR to a
            // C# actor, never both (DispBind constructs one or the other -- see its own comment), so
            // this is a fallback rather than a second delivery. Reaching it means the entity is
            // scripted in C#, and an animation notify aimed at a C# character used to die here with
            // a "no live graph" warning that named the wrong problem.
            if (s_actorsByEntity.TryGetValue(targetEntity, out ActorLive? live) && !live.Disabled)
            {
                try
                {
                    if (live.Instance.OnEvent(eventName)) return true;
                }
                catch (Exception ex)
                {
                    // Same treatment as every other actor hook: one bad actor is disabled, the
                    // caller is told it was not handled, and nothing propagates into compiled IL or
                    // back across the ABI.
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
