# A startup module-ABI/version check for Aver.Core — a plan, not a patch

This document is a design for review, not a description of code that exists. Nothing here has been
implemented. Every claim below was checked against the live tree at the file:line cited; where two
sources disagreed, the live source wins and the disagreement is called out rather than smoothed over.

## 1. What the check can honestly be

The request was: discover every *active* module in the engine folder, and verify every module's ABI
and version. Read literally, that sentence describes scanning a directory for a set of peer files and
interrogating each one. That is not what this engine's build produces, and the plan has to say so
before it says anything else.

`cmake/AvModule.cmake:14` — the helper that declares the overwhelming majority of Aver.* targets —
reads `add_library(${MODNAME} STATIC ${ARG_SOURCES})`. Every module declared through
`aver_add_module()` becomes a static archive, not a file that ships beside the executable. The root
`CMakeLists.txt:23-25` even routes the two kinds of build product to different directories —
`set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)` for DLLs and EXEs,
`set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)` for static archives — so a static
module's `.lib` does not even land in the folder a "scan the engine folder" design would look in. Once
linked, a static module has no separate on-disk identity, no separate load event, and nothing a
process can ask about it after the fact beyond ordinary C++ ODR. Grepping the whole tree for
`add_library(Aver\.[A-Za-z.]* SHARED` turns up exactly eight hits: `modules/audio.abi/CMakeLists.txt:12`
(Aver.Audio.Abi), `modules/framework/CMakeLists.txt:12` (Aver.Framework),
`modules/physics/CMakeLists.txt:10` (Aver.Physics), `modules/render.pbr/CMakeLists.txt:9`
(Aver.Render.PBR), `modules/render.voxi/CMakeLists.txt:6` (Aver.Render.Voxi),
`modules/scene/CMakeLists.txt:12` (Aver.Scene), `modules/settings/CMakeLists.txt:7` (Aver.Settings),
`modules/ui.abi/CMakeLists.txt:11` (Aver.UI.Abi). Everything else under `modules/` — Core, Platform,
Assets, Landscape, Occlusion, Trifactor, Synapse, Sound, Anim, roughly thirty targets in total — is
static, absorbed into whichever executable links it.

The root `CMakeLists.txt` itself gets this count wrong today, which is worth citing precisely because
it is the exact failure mode this whole plan exists to prevent, one layer up: `CMakeLists.txt:48-49`
says *"Five modules are already SHARED (Voxi, PBR, Scene, Framework, Physics)"* — a sentence sitting in
the very file that configures the other three (Settings, UI.Abi, Audio.Abi) as SHARED. Nobody updated
the prose when those three were added. A directory scan is not the only thing that can drift from the
truth; a comment can too, and this one already has.

So "discovers every active module in the engine folder" cannot mean what it sounds like. It has to mean
something narrower, and this plan proposes the narrowest honest reading: **the only things a runtime
check can discover as separate, independently-versionable units are the eight SHARED DLLs above, plus,
on a different timeline entirely, the .NET scripting bridge that `modules/scripting/src/ScriptHost.cpp`
loads via `hostfxr` well after the native DLLs are already resolved.** Everything static is out of
scope for *discovery* — not because it doesn't matter, but because there is no boundary for a runtime
check to stand on. The engine already has a mechanism for the static half of this problem
(`modules/core/include/aver/core/ModuleCheck.hpp`'s `AVER_REQUIRE_MODULE`, `cmake/AvModule.cmake:55-98`'s
`aver_check_module_dag()`, and `scripts/module-matrix.ps1`), and this plan does not try to duplicate or
replace it — see §3.

"Active" needs the same tightening. It cannot mean "present in `bin/`", because `bin/` is not a clean
set: it holds `dxcompiler.dll`, `dxil.dll` and `nethost.dll` — third-party binaries pulled from the
local Windows SDK / .NET SDK install at configure time (`modules/rhi.d3d12/CMakeLists.txt:36,38`;
`modules/scripting/CMakeLists.txt:138`), foreign to this project and unversioned by it — and one level
down, `bin/Scripting/` holds *managed* assemblies named `Aver.Framework.dll` and `Aver.Scene.dll` that
share a file name with the *native* DLLs sitting in `bin/` itself. That collision is not hypothetical:
`scripting/csharp/Aver.Framework/NativeResolver.cs:13` exists specifically to redirect P/Invokes past
it, and `Native.cs`'s own history (see §8, incident 2) records a real bug from exactly this ambiguity.
A folder scan that treats "every `Aver.*.dll` under `bin/`" as the module set will misclassify at least
two files on every single build. "Active" also cannot mean "declared by an `option()`" —
`modules/audio.abi/CMakeLists.txt:11` builds Aver.Audio.Abi only `if(WIN32 AND TARGET Aver.Audio.Wasapi
AND TARGET Aver.Formats.Audio)`, and `modules/scripting/CMakeLists.txt:25-30` stages nothing at all when
`find_program(AVER_DOTNET_EXE dotnet)` fails to find a toolchain — both are legitimate, working,
by-design absences, not failures. This plan defines **active** the only way that survives all of the
above: *a module is active if and only if this specific compiled executable actually linked against
it* — the same fact `AVER_MODULE_SCENE`, `AVER_MODULE_FRAMEWORK` etc. already encode as compile
definitions and that `sandbox/CMakeLists.txt`'s `if(TARGET Aver.X)` blocks and `SandboxApp.cpp`'s
`#if AVER_MODULE_X` guards (both confirmed throughout that file, e.g. lines 63, 78, 153, 164, 183, 186,
201 of `SandboxApp.cpp`) already act on, everywhere else in this codebase, for everything except a
version check.

One consequence follows immediately and should be stated before the boundary table, not after it:
because the Windows loader resolves every implicitly-linked DLL's import table before `main()` runs at
all, a *missing* SHARED module already crashes the process before a single line of Aver code executes —
there is no Aver log line, no splash, nothing. A startup check cannot make "missing" a case it handles;
it was never going to see it. Its entire addressable value is the case the loader is structurally blind
to: **present, resolved, calling cleanly — and the wrong shape or the wrong build.**

## 2. The boundaries, ranked

| # | Boundary | What it would take to verify | What it structurally CANNOT catch |
|---|---|---|---|
| 1 | **Native SHARED DLL C ABI** — Scene, Framework, Physics, PBR, Voxi, Audio.Abi, Settings, UI.Abi, each resolved via the exe's import table before `main()` (`docs/ABI.md:975`) | A version/shape query the exe calls on each DLL it actually linked, right after those DLLs are guaranteed resident. Two of eight already export something to call: `aver_scene_abi_version()` (`scene_abi.h:35`) and `aver_fw_abi_version()`/`aver_fw_scene_abi_version()`/`aver_fw_scene_abi_matches()` (`FrameworkAbi.cpp:353-367`). Six export nothing — confirmed by grepping `VERSION`/`version` across `physics_abi.h`, `pbr_abi.h`, `voxi_abi.h`, `audio_abi.h`, `settings_abi.h`, `ui_abi.h`: zero hits in every one. | A field **reordered** inside a struct whose overall byte size the check happens to compare as unchanged (see row 6). A struct that lives entirely outside the `*_abi.h` C surface (MaterialConstants is exactly this — see row 6). Anything about a raw C++ class crossing the same DLL edge (`pbr::MaterialLibrary`, `aver::scene::World`) rather than the plain-C seam — no portable runtime signal exists for STL layout or calling-convention agreement between two builds of the same compiler. |
| 2 | **Host ↔ managed scripting bridge** — `AverScriptHostApi` (`scripting_abi.h:16-34`), the one boundary in the tree that already works end to end | Nothing — it is built and enforced today. `ScriptHost.cpp:242-244` stamps `structBytes`/`contractVersion`; the bridge checks both (`HostBridge.cs:209`) and the host declines by name on mismatch (`ScriptHost.cpp:253-257`). | A **stale but shape-identical** bridge DLL — see §8, incident 1. `structBytes`+`contractVersion` prove the two sides agree on *shape*; they say nothing about *which build* produced that shape, so a bridge rebuilt from an older commit with an unchanged contract passes clean. |
| 3 | **Managed dispatch install** — `AvManagedDispatch` (`framework_hooks.h:65-78`) | Nothing — also built and enforced today, per `docs/ABI.md:956`'s citation of `FrameworkAbi.cpp:767-778` rejecting a short or wrong-version table. | Same limit as row 2: a shape check, not a build-identity check. |
| 4 | **Actor vtable install** — `AvActorVTable` (`framework_hooks.h:40-47`), `AVER_FW_VTABLE_VERSION 1` at `:33` | Nothing yet, and nothing to verify: no `aver_fw_install_actor_vtable`-shaped function exists anywhere in this header or any `.cpp` grepped for it. This is declared scaffolding, not a working boundary — `docs/ABI.md:957` labels it PLANNED and this plan agrees. | Everything, because the path it would guard does not exist. Do not count this as coverage. |
| 5 | **Managed contract assembly ↔ native DLL, by absolute path** — `NativeResolver.cs`'s `Resolve()` in both `Aver.Scene` and `Aver.Framework`'s C# contract assemblies | A version-report P/Invoke call made immediately after `NativeLibrary.TryLoad` succeeds (`NativeResolver.cs:40`), compared against a constant the managed assembly was built with. Nothing today does this: `Resolve()` returns the loaded handle unconditionally, on a bare "did the file open" basis. | A signature drift the version number was never bumped for (same limit as row 1, transplanted). Also: this check runs once, lazily, at first P/Invoke — a DLL swapped in *after* that first call is invisible to it, same as every other one-shot check in this table. |
| 6 | **Hand-mirrored C++/HLSL cbuffers** — `pbr::MaterialConstants` (`MaterialGpu.hpp:32-90`), `pt::FrameCB` (`PathTracer.cpp:54-62`), `pcg::VolumeCB`, `voxi::FrameConstants`, `WaterVertex`/`WaterFrameCB`, RHI's `PostCB`/`PerFrameCB` | Nothing a module-ABI check can do. These never cross a DLL load boundary — they are same-binary, cross-*language* (C++ struct vs. an HLSL string literal compiled at a different time, by a different compiler, that never sees the C++ definition). The only existing defence is a `static_assert(sizeof(...) == N, ...)` inside the one translation unit that owns the C++ half — e.g. `MaterialGpu.hpp:92`, `PathTracer.cpp:69`. | **A field reordered within the same total size** — `PathTracer.cpp:64-68` says this in the file itself: *"It cannot catch a field REORDERED within the same size, but it does catch the common case: a row added on one side and not the other."* This is precisely the incident class named in the task brief, and it is fully out of reach here: there is no load event to hook, no `*_abi.h` header to hash, and no shader-reflection code anywhere in `modules/rhi.d3d12` or `modules/rhi.vulkan` that could compare a compiled cbuffer's real offsets to the struct's `offsetof()` values. |
| 7 | **Hand-mirrored native/C# enum and constant blocks** — component ids, class flags, tick groups, play states, begin/end reasons, key codes, PBR/Voxi feature enums (`docs/ABI.md:1039-1057` tabulates at least twelve pairs) | Rows 4, 6–12 of that table live *inside* a `*_abi.h`/`framework_hooks.h` header, so a whole-header content hash (§4) would at least prove "the native side of this pair changed" — a strictly larger net than today's nothing. | Whether the **C# mirror** was updated to match. A hash over the native header cannot see `Enums.cs`, `SceneIds.cs`, `Pbr.cs` or `Voxi.cs` at all; catching a native/managed pair drifting apart needs a check that spans both files, which nothing here proposes to build. Treat this row as a promising side-effect of §4's header hash, not a solved problem. |
| 8 | **RHI backend-to-backend cbuffer drift** — `PostCB`/`PerFrameCB` defined once per backend (`D3D12Device.cpp:532,2342`; `VulkanDevice.cpp:1228`; `VulkanCommon.hpp:898`) | A shared header both backends `static_assert` against, so the two copies cannot silently diverge. This is a compile-time fix, not a runtime one. | Nothing a runtime module check does reaches this either way — the two backends are alternative static libraries selected by `AVER_RHI_D3D12`/`AVER_RHI_VULKAN` at compile time, never loaded side by side, so there is no "wrong one loaded" moment for a runtime check to catch even in principle. |
| 9 | **Third-party `dxcompiler.dll`/`dxil.dll` identity** | Nothing this plan proposes touches it. `modules/rhi.d3d12/CMakeLists.txt:29-31`'s own comment calls two copies "a coin toss over which `LoadLibraryW("dxcompiler.dll")` resolves" — a real, adjacent risk, deliberately out of scope because it is not an Aver module and its identity is outside this codebase's control. | Everything, by design of this plan. Named here only so it is not mistaken for covered. |
| 10 | **Content/project compatibility** — `.ocproject`'s `ENGINE` line vs. `kEngineVersion` (`OcProject.cpp:124-127`) | Already solved, already working, at a different layer (content vs. binary) and a different time (project-open, not engine-start). | Not applicable — this is not a module-ABI boundary and this plan does not try to become it. |
| 11 | **Asset container format** — the AVR1 magic+version every `.ocmesh`/`.ocanim`/etc. carries (`Avr1.hpp:11,19,30`) | Already solved, already working, at yet another layer (per-file content, checked at load time). | Not applicable, same reasoning as row 10. |

Row 1 is the actual target of this plan. Rows 2–3 are working precedents to imitate, not gaps to fill.
Row 4 is dead scaffolding worth naming so nobody credits it. Row 5 is the sharpest fit to one of the
two managed-boundary incidents in the brief and is proposed only as a later stage (§6, Stage 4) because
it is a different mechanism on a different boundary from row 1's native DLLs. Rows 6–9 are named and
explicitly disclaimed: this plan does not solve them, cannot solve them as designed, and should not be
read as having solved them by proximity. Rows 10–11 are cited only to rule them out as places this
proposal might accidentally re-solve or silently absorb.

## 3. The Core dependency problem, and how the design inverts it

Aver.Core sits at the bottom of the DAG by construction: `modules/core/CMakeLists.txt:1-11` declares
`aver_add_module(Aver.Core SOURCES ... PUBLIC_DEFS AVER_ENGINE_VERSION=...)` with no `DEPS` argument at
all — the only module in the tree without one. That is exactly why `ModuleCheck.hpp` cannot supply its
own facts: *"Core is built first and links none of the optional modules, so a list of DAG facts written
HERE would see every macro undefined and check nothing at all, while looking thorough"* (`ModuleCheck.hpp:10-12`).
The existing fix is that each dependent module invokes `AVER_REQUIRE_MODULE` from its **own** public
header, so the macro fires only in a translation unit that has already linked what it is asking about.
A version/ABI check has the identical inversion problem, one level worse: it is not a compile-time
`static_assert` that can be dropped into an arbitrary header and left to fire wherever it's included —
it needs to make an actual function call, at actual runtime, into an actual DLL that is actually
resident in this process. That rules out the two designs that look obvious first.

**Design A — self-registration into a Core-owned registry, at static-init time.** Each module's own
translation unit would carry a global object whose constructor calls into Aver.Core to add itself to a
list; whichever code walks the list later does the comparing. This looks like the natural generalization
of "supply facts from the module that has them," but it is unsound here for a reason this codebase can
already demonstrate on itself: **Aver.Core is STATIC, and gets compiled into every SHARED module DLL
separately.** `modules/scene/CMakeLists.txt:21` links `Aver.Scene PUBLIC Aver.Core`; `Aver.Core` is
built via `aver_add_module()`, i.e. as a static archive (`AvModule.cmake:14`). That means Aver.Scene.dll
does not share Aver.Core's globals with the executable — it gets its **own private copy**, embedded at
link time. `modules/core/src/Log.cpp:9-13` proves this concretely: `g_sink` is an ordinary namespace-scope
static, and `SandboxApp.cpp:991`'s `setLogSink(&SandboxApp::logSink, this)` installs the sink only in
the executable's own copy of that static. Any `AVER_ERROR`/`AVER_WARN` logged from **inside**
Aver.Scene.dll reaches stdout/stderr through that DLL's private mutex and private `g_sink`, which was
never set, so it never reaches whatever sink the host (the editor console, an MCP capture) is actually
watching. A "Core registry" that a DLL writes into at static-init time is really N independent
registries, invisible to each other and to the executable's own copy — the executable would query an
empty list every time. Design A is not merely inconvenient here; it silently does nothing, in exactly
the "looks thorough, checks nothing" shape `ModuleCheck.hpp:12` already warns about for a different
mechanism.

**Design B — explicit registration during startup, driven from the composition root.** Instead of a
module pushing a fact into a shared object it does not actually share, the executable — the one binary
that genuinely has one live copy of everything, because it is the thing the DLLs are loaded *into* —
calls **out** to each DLL it linked, through an exported function that DLL supplies, and collects the
answers itself. This is a pull, not a push, and it is not a new idea in this tree: it is exactly the
shape of `aver_fw_scene_abi_matches()`, which already exists, already works, and already crosses this
precise boundary correctly — *"The one call into Aver.Scene"* (`FrameworkAbi.cpp:363`), calling
`aver_scene_abi_version()` from inside Aver.Framework.dll and comparing the result to its own compiled-in
constant. The only thing wrong with it is that nothing calls it outside a unit test
(`tests/framework/src/FrameworkTest.cpp:746`). Generalizing this shape — one exported report function
per module, called from wherever the process already knows which modules it linked — costs nothing new
in terms of soundness (each call happens inside a single process, through an import table already
resolved) and matches the mechanism-vs-facts split `ModuleCheck.hpp` already established: **Aver.Core
supplies the comparison primitive and the struct shape (mechanism); the composition root — Sandbox.exe
today, and any future `Aver.Runtime.Game`-hosting executable — supplies the list of which modules to
ask, because it is the only thing in the process that legitimately knows (facts).** This is the design
this plan adopts. `Aver.Runtime::Engine::run` itself cannot be the caller, for the same reason:
`modules/runtime/CMakeLists.txt`'s own `DEPS` are only `Aver.Core Aver.Platform Aver.RHI` plus whichever
RHI backend is compiled in — that library never names Scene, Framework, Physics, PBR, or Voxi, so
`Engine.cpp` cannot call any of their symbols even if it wanted to. The composition root already has the
vocabulary for this: `SandboxApp.cpp` gates roughly twenty other features on `#if AVER_MODULE_X`
throughout its own source (confirmed at lines 63, 78, 95, 99, 124, 132, 136, 139, 140, 153, 164, 172,
183, 186, 201, 216, 263, 658, 704, and more), and `modules/runtime.game/src/GameApp.cpp:18-54` uses the
identical `#if AVER_MODULE_SCENE` / `#if AVER_MODULE_PHYSICS` / `#if AVER_MODULE_FRAMEWORK` pattern for
its own includes. Extending that same list with one more line per module — a call to that module's
report function, guarded by the macro already guarding everything else about it — is additive to a
place that is already the accepted owner of "which optional modules does this specific executable have."

The trade-off, honestly: Design A would have been more convenient for a module author (write the
registration once, in the module's own file, and forget it) if it worked; Design B requires whichever
executable hosts modules to maintain one line per module at its own call site, which is a second place
to remember besides the module's own header. But that second place already exists and is already
maintained today — `sandbox/CMakeLists.txt`'s `if(TARGET Aver.X)` link gates and `SandboxApp.cpp`'s
matching `#if AVER_MODULE_X` blocks are not new bookkeeping this plan invents; they are bookkeeping this
plan piggybacks one more line onto, in a file whose entire job is already "know what this build has."

## 4. What a module declares

Aver.Core gains one new header, `modules/core/include/aver/core/ModuleVerify.hpp`, holding the
comparison mechanism and nothing else — no per-module facts, matching `ModuleCheck.hpp`'s own rule about
itself:

```c
/* aver/core/ModuleVerify.hpp — the mechanism, not the facts. Core links nothing optional, so this
 * header defines the SHAPE every module reports and the comparison logic, never a list of modules. */

typedef struct AverModuleReport {
    int32_t     structBytes;   /* sizeof(AverModuleReport), as the REPORTING DLL was built */
    int32_t     abiMajor;      /* hand-bumped: breaks an existing caller */
    int32_t     abiMinor;      /* hand-bumped: additive only */
    uint64_t    headerHash;    /* derived: FNV-1a64 over this module's own *_abi.h, at build time */
    const char* buildId;       /* derived: short git hash + UTC build timestamp, at build time */
    const char* moduleName;    /* literal, e.g. "Aver.Scene" — for the log line only */
} AverModuleReport;

typedef AverModuleReport (*aver_module_report_fn)(void);

typedef enum AverModuleVerdict {
    AVER_MODVERIFY_OK = 0,
    AVER_MODVERIFY_MINOR_SKEW,       /* minor differs, major and hash agree: tolerate, log at INFO */
    AVER_MODVERIFY_HASH_DRIFT,       /* header bytes differ, major/minor UNCHANGED: log at ERROR, do not disable */
    AVER_MODVERIFY_MAJOR_MISMATCH,   /* major differs: disable this module's surface, log at ERROR */
    AVER_MODVERIFY_STRUCT_MISMATCH,  /* structBytes differs: the two sides cannot even agree on THIS struct's shape; disable */
} AverModuleVerdict;

/* Pure comparison, no I/O, so it is testable without ever loading a real DLL — see §7. */
AverModuleVerdict aver_module_verify(AverModuleReport expected, AverModuleReport actual);
```

Each SHARED module's *own* public header (`scene_abi.h`, `framework_abi.h`, `physics_abi.h`, ...) adds
one macro-generated export, in the same file that already declares that module's other C entry points —
never in Core, for the reason argued in §3:

```c
/* In each module's own *_abi.h, beside its existing version constants. */
#define AVER_DECLARE_MODULE_REPORT(EXPORT_MACRO, PREFIX, MAJOR, MINOR)                        \
    EXPORT_MACRO AverModuleReport PREFIX##_module_report(void) {                              \
        AverModuleReport r;                                                                   \
        r.structBytes = (int32_t)sizeof(AverModuleReport);                                    \
        r.abiMajor    = (MAJOR);                                                              \
        r.abiMinor    = (MINOR);                                                              \
        r.headerHash  = PREFIX##_ABI_HEADER_HASH;   /* from the generated stamp, see below */  \
        r.buildId     = PREFIX##_BUILD_ID;          /* from the generated stamp, see below */  \
        r.moduleName  = #PREFIX;                                                              \
        return r;                                                                              \
    }

/* e.g. in scene_abi.h: */
AVER_DECLARE_MODULE_REPORT(AVER_SCENE_ABI, aver_scene, AVER_SCENE_ABI_VERSION_MAJOR, AVER_SCENE_ABI_VERSION_MINOR)
```

**Who bumps what, and who is responsible for remembering.** `abiMajor`/`abiMinor` stay exactly what they
are today — a human types a new number and a one-line justification beside it, the way
`scene_abi.h:26-28`'s three MINOR-bump comments already do (*"MINOR 3 adds
AVER_SCENE_COMP_ATTACHMENT... Additive only"*). That discipline is real but, as `docs/ABI.md:946` already
states about the existing scheme, **unenforced** — an entry point can change shape with the major
untouched and the build stays green, because nothing ties the constant to the surface. A scheme that
only relies on a human remembering to bump a number is worth exactly as much as that human's discipline,
which is the same risk class as the hand-mirrored HLSL cbuffers this plan explicitly cannot fix (row 6,
§2) — just one layer coarser. So `headerHash` is proposed specifically to remove the "forgot to bump
it" failure mode from the one place it is cheap to remove: **hash the module's own `*_abi.h` file at
build time and compare the hash, not the human-typed number.** If the header's bytes change at all — a
function signature, a macro constant, a struct's field order — the hash changes with zero human action,
which is exactly what a MaterialConstants-shaped mistake would need to be caught automatically, *for
anything that lives inside a `*_abi.h` header* (see the honest limit below). This needs new,
non-trivial infrastructure: a small build-time script (the tree already has this convention —
`scripts/module-matrix.ps1`, `scripts/gates.ps1`, `scripts/verify-payload.ps1`, `scripts/publish-release.ps1`
all exist as precedent) that reads each `*_abi.h` file's bytes, computes an FNV-1a64 (cheap, no
dependency, already used for interning — see `aver::core::Hash` per `modules/core/CMakeLists.txt:6`'s
`src/Hash.cpp`), and writes it into a generated header per module (e.g.
`${CMAKE_BINARY_DIR}/generated/aver/scene/AbiStamp.hpp` defining `aver_scene_ABI_HEADER_HASH` and
`aver_scene_BUILD_ID`), included by both the DLL's own build and — critically — by anything that wants
to state an *expectation* about that DLL without itself linking it (the composition root can include
the same generated header and compare against the same hash it would have produced, had it been the one
building the DLL). Each of the nine `*_abi.h`/`framework_hooks.h` headers needs its own CMake wiring for
this: nine small `add_custom_command` steps, one generated-headers include path added to whichever
target consumes it, and one real new failure mode to watch for — **line-ending drift.** A raw byte hash
over a text file is sensitive to CRLF vs. LF; if `git`'s `autocrlf` setting differs between the machine
that built the DLL and the machine (or CI runner) that built the composition root's expectation from the
"same" commit, the hash will legitimately differ for a reason that has nothing to do with the ABI. This
is a real, concrete cost of the derive-it approach, not a hypothetical one, and it argues for hashing a
newline-normalized read of the file rather than its raw bytes — small extra work, worth doing before
Stage 3 ships (§6).

`buildId` (a short git hash plus a UTC build timestamp, generated the same way, at *build* time rather
than configure time so an incremental rebuild after a fresh commit still gets a fresh stamp) is not a
shape signal at all — two builds can share an identical `headerHash`/`abiMajor`/`abiMinor` and still
carry different `buildId`s, because nothing about the header changed even though the module's `.cpp`
did. This is the one field aimed squarely at "a managed DLL went stale and the engine ran against the
old one without saying so" (see §8) — but note plainly that a same-shape rebuild is not a *shape*
problem, and this plan's own comparison logic (below) treats a `buildId`-only mismatch as advisory, not
fatal, because a differing timestamp on an otherwise-identical ABI is not evidence of anything broken —
only evidence that the two files did not come from the same build, which might be completely benign.

**What this cannot do, stated as bluntly as row 6 of the boundary table:** hashing `pbr_abi.h` does
**not** touch `pbr::MaterialConstants`. `MaterialConstants` is declared in `MaterialGpu.hpp`, a
different header, consumed by `pbr::MaterialLibrary`'s static callers (`OcMat.cpp`, `GameContent.cpp`,
`GameRender.cpp`, `SandboxApp.cpp` — all statically linked, none of them going through the SHARED
`Aver.Render.PBR.dll`'s C ABI at all) and mirrored by hand into an HLSL string in `PbrShaders.cpp` that
no C++ compiler, and no header hash, ever reads. The `*_abi.h` hash this plan proposes is a real,
derived, zero-human-effort improvement over a hand-typed version number — but only for whatever
actually lives inside the nine C headers it hashes. It is not a general answer to "did a mirrored struct
drift," and this document does not claim it is.

## 5. Failure policy

One answer for every mismatch is wrong, because the four fields above carry genuinely different weight:

- **`structBytes` mismatch.** The two sides cannot even parse `AverModuleReport` itself the same way,
  which means every other field in it is potentially garbage. Treat exactly like `ScriptHost.cpp:253-257`
  already treats a scripting contract mismatch: disable that module's surface (null its function
  pointers / flip its "available" flag, whatever that module's own consumer already checks before
  calling it), log at `AVER_ERROR` naming the module and both `structBytes` values, and **keep running
  everything else.** There is no precedent anywhere in this engine for a hard process exit on a
  degraded-but-survivable startup condition — `modules/rhi/src/RHI.cpp:94-95` falls back to a Null
  device rather than fail; `Engine.cpp:181-186`'s own comment on a lost GPU device is explicit that
  exiting "would take the explanation off the screen along with everything else"; `Assert.cpp`'s
  `abort()` is reserved for programmer-invariant violations, not degraded startup. This check should not
  be the first thing in this codebase to reach for a exit call it has never needed before.
- **`abiMajor` mismatch.** Same treatment as `structBytes` — disable, log loudly, keep running — with a
  message in the shape `HostBridge.cs` already uses for the one real precedent of this exact sentence
  structure: *"...was built against {name} {referenced} but this engine provides {loaded} — the
  assembly was rejected"* (`HostBridge.cs:447-449`). Name the module, both majors, and point at the fix
  (rebuild the stale side against this header).
- **`abiMinor` skew alone** (major and hash agree, minor differs). Tolerate — this is exactly the
  additive-only contract `scene_abi.h`'s own comments already describe. Log at `INFO`, not `ERROR`; do
  not disable anything. A caller expecting a newer minor than the loaded DLL provides may find one
  specific feature unavailable, which is a narrower and more honest statement than disabling the whole
  module over it.
- **`headerHash` differs but `abiMajor`/`abiMinor` do not.** This is the single most important and most
  dangerous bucket, because it is the one case that proves a human's bookkeeping failed — the header
  changed and nobody bumped the number. It must be logged loudly (`AVER_ERROR`, naming the module and
  that the hash disagrees despite an unchanged declared version) precisely because it is a red flag a
  person should read. It must **not** disable the module by default, for a reason that has nothing to do
  with charity: a raw byte hash cannot distinguish a real ABI change from a changed comment, a
  reordered `#include`, or (per §4) a line-ending difference across machines. Gating on it by default
  risks becoming exactly the kind of false-positive-that-takes-everything-down the task explicitly warns
  about for the 95 headless test suites (`aver-running-the-test-suite.md`). An opt-in strict mode
  (an env var or CLI token, in the same vocabulary as `--force-caps`) may promote this bucket to
  `MAJOR_MISMATCH` severity for CI/matrix builds that want to fail loudly on any drift — but that is an
  opt-in, not the default.
- **`buildId` differs, everything else identical.** Log at `INFO` only, exactly matching the existing,
  explicitly-diagnostic precedent at `HostBridge.cs:116-118`'s *"managed bridge online (contract v2,
  10.0.10, API v1.0.0.0)"* line, which `docs/ABI.md:965` itself calls out as informational rather than
  enforced. A differing build stamp on an identical ABI is a fact worth putting in a bug report, not a
  reason to change behaviour.

**On the 95 headless test executables and the MCP tooling.** None of the *Test.exe suites call
`Engine::run` at all — `tests/game/src/InputBridgeTest.cpp`'s own header comment states *"No window, no
device, no ImGui, no human"*, and every test executable in `tests/` calls into a module's C ABI or C++
API directly, never through `aver::Application`/`aver::Engine`. Because the check as designed here is
driven from the composition root's own `onInit` (§3), and no test executable is a composition root, the
check by construction does not run inside any of them — it cannot flake a suite it never touches. The
two things that *do* host `aver::Application` — Sandbox.exe and, prospectively, whatever eventually
wraps `Aver.Runtime.Game` as a second executable — are where this check runs, and even there, a
per-module hard-disable never takes the *process* down: a suite of one, `Sandbox.exe --frames N`, that
happens to touch a genuinely mismatched module gets an attributable, specific failure (that module's
calls return neutral values or are refused) rather than a crash, a hang, or a flake blamed on something
unrelated. A CLI escape hatch (matching the existing `--force-caps`/`--furnace-test`-style vocabulary,
e.g. `--skip-abi-check`) is worth adding for a developer deliberately running one freshly-rebuilt DLL
against an otherwise-unrebuilt tree while iterating — but it should default to unnecessary, because a
healthy, fully-rebuilt tree should never need it, and a flag whose entire purpose is "get past a check
that is usually right" is worth being suspicious of if it is reached for often.

`Application::exitCode()` (`Application.hpp:44-51`) is the existing, purpose-built vehicle for turning a
detected problem into something a script can see — *"THIS EXISTS SO A TEST MODE CAN FAIL."* A dedicated
test mode for this check (§7) should be the one thing that turns a *deliberately injected* mismatch into
a non-zero exit code. An ordinary run should never fail merely because the check executed — only
because a module it actually disabled was then asked to do something it could no longer do.

## 6. Staging

**Stage 0 — fill the gap that has to exist before anything else can.** Add `AVER_X_ABI_VERSION_MAJOR`/
`_MINOR` and an `aver_x_abi_version()` accessor to the six seams that have neither today: Physics, PBR,
Voxi, Audio.Abi, Settings, UI.Abi — following exactly the pattern already in `scene_abi.h:25-35`. This
is real, additive, per-module work, not incidental plumbing to assume already exists; it is independently
valuable (six ABI surfaces become self-describing for the first time) and independently verifiable (one
unit test per module asserting the accessor returns the compiled-in constant, the same shape as the one
real existing caller at `tests/scene/src/SceneTest.cpp:682`), even with nothing yet calling any of them
from a shipping path.

**Stage 1 — wire the two accessors that already exist and already work.** `aver_fw_scene_abi_matches()`
is fully implemented (`FrameworkAbi.cpp:363-367`) and has exactly zero shipping callers. The cheapest
possible real win is a single call site — inside `SandboxApp.cpp`'s `onInit`, guarded by
`#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE`, right after device creation and before the scripting
host bootstraps — that calls it, logs the result at `AVER_INFO`/`AVER_ERROR` as appropriate, and does
nothing else. This requires zero new native logic beyond the call and the log branch, proves the
call-site pattern before any other module is asked to add version symbols, and turns *"the repair
function was written, it works, and nobody calls it outside a test"* (`docs/ABI.md:961`) into something
that is no longer true for at least this one pair. Independently valuable on its own; independently
verifiable by the smoke test in §7.

**Stage 2 — generalize the crude comparison to all eight SHARED modules.** Using Stage 0's new
accessors, add the equivalent single-module version-and-report call for Physics, PBR, Voxi, Audio.Abi,
Settings, and UI.Abi at the same call site, still comparing hand-typed `abiMajor`/`abiMinor` only — no
`headerHash`, no `buildId` yet. This is "extend the mechanism that already proved itself in Stage 1 to
every module that has the raw material," using the crude, cheap, already-validated approach before
asking for new infrastructure.

**Stage 3 — replace the hand-typed comparison with the derived one.** Introduce `AverModuleReport`,
`aver_module_verify()`, the `AVER_DECLARE_MODULE_REPORT` macro, and the build-time hashing/stamping
script from §4, for all eight modules. This is the stage that actually removes "a human forgot to bump
the number" from the failure mode, and it is staged last deliberately: it is genuinely new
infrastructure (a hashing tool, nine CMake wiring edits, a generated-headers convention, the
line-ending-normalization fix called out in §4) rather than wiring together things that already exist,
and it should not gate the value of Stages 1–2 landing first.

**Stage 4 (named, not committed) — extend the same report/compare idea to `NativeResolver.cs`.** Row 5
of the boundary table is the sharpest fit to the "wrong DLL resolved" class of incident, and it lives on
a different timeline (lazy, first-P/Invoke-call, deep inside the managed runtime) from the native
DLLs Stages 1–3 cover. This is a real follow-on, not "the same startup check, later" — it needs its own
call site inside `Resolve()` (or immediately after it), its own managed-side constant, and its own
decision about failure policy (§5's buckets do not automatically transfer to C#). Flagged here as a
distinct, plausible Stage 4 and left for the user to decide whether it belongs in this initiative at all
(see §9).

**Stage 5 (named, not committed) — the hand-mirrored native/C# enum pairs.** Row 7 of the boundary table.
A whole-header hash from Stage 3 incidentally proves "the native side of `AVER_SCENE_COMP_*` changed";
it does not prove the C# mirror in `SceneIds.cs` was updated to match. Closing that gap needs a
mechanism this document does not design — most plausibly, hashing the pair of files together, or
generating one side from the other — and is named here only so it is not mistaken for something Stage 3
already solves.

## 7. How it is tested

The rule this repo has already learned the hard way applies here with unusual force: **by construction,
every one of these checks passes silently on a healthy build, because a healthy build is exactly one
where every module agrees with itself.** A test suite that only ever runs against a healthy build proves
nothing about whether the comparison logic works — it proves only that nothing was ever asked to
disagree. `framework_abi.h`'s own comment about `aver_fw_scene_abi_matches()` names this risk directly:
the check *"must keep genuinely calling into Aver.Scene or the check becomes a tautology"*
(`framework_abi.h:70-77` per `docs/ABI.md:1036`) — a check that always reports "fine" because it was
quietly turned into a comparison against itself is worse than no check, because it *looks* like coverage.

This engine already has two precedents for exactly this problem, and this plan copies both rather than
inventing a third. `modules/render.pt/include/aver/pt/PtFurnaceTest.hpp:85-98` declares an enum of
furnace-test configurations that deliberately includes a broken one — `kOpenDielectricDefect` at line
92, whose `Config::defect` field (`:106`) is set to a real, non-`PtDefect::None` value specifically so
the white-furnace oracle has at least one run it is *supposed* to fail, proving the oracle can actually
detect an energy-conservation violation rather than always reporting "fine" by construction.
`modules/rhi/include/aver/rhi/RHI.hpp:98-121`'s `CapsOverride`/`setCapsOverride()`/`--force-caps` gives
the RHI's capability-gated code paths a way to be exercised against hardware the test machine does not
actually have, by overriding what `DeviceCaps` reports rather than requiring a second physical GPU.

The proposed test suite for this check follows the same shape, in a new headless executable —
`tests/core/src/ModuleVerifyTest.cpp` — built around `aver_module_verify()`'s pure, I/O-free signature
(§4): it takes two `AverModuleReport` values and returns a verdict, so **the test constructs both by
hand and never needs a second, deliberately-stale DLL sitting in `bin/`.** This is the load-bearing
design choice for testability: because the comparison is a pure function of two plain-old-data structs,
injecting a defect is as simple as mutating one field of a `Config`-style test fixture before calling
it, exactly as `PtFurnaceTest`'s `Config::defect` field does:

- **Clean case:** `expected == actual` in every field → assert `AVER_MODVERIFY_OK`, and separately
  assert that whatever "is this module usable" flag the caller checks reads true — not just that no
  error was logged, since a test that only checks for an absent log line is a weaker oracle than one
  that checks the actual behavioural consequence.
- **`structBytes` off by one field's worth** → assert `AVER_MODVERIFY_STRUCT_MISMATCH`, and assert the
  simulated caller's "usable" flag flips to false.
- **`abiMajor` incremented on one side only** → assert `AVER_MODVERIFY_MAJOR_MISMATCH`, disable.
- **`abiMinor` incremented on one side, `abiMajor`/`headerHash` equal** → assert `AVER_MODVERIFY_MINOR_SKEW`,
  and assert the module stays enabled — this bucket is as important to prove *does not* disable
  anything as the mismatch buckets are to prove *do*, because an over-eager implementation that disables
  on any difference at all would silently break the additive-minor contract every existing version
  scheme in this tree relies on.
- **One bit of `headerHash` flipped, `abiMajor`/`abiMinor` both equal** → assert `AVER_MODVERIFY_HASH_DRIFT`,
  and explicitly assert the module **stays enabled** by default — this is the bucket most likely to be
  implemented backwards (silently promoted to fatal), and the one that would take down every build where
  someone merely edited a comment in a `*_abi.h` header if it were.
- **`buildId` differs alone, everything else identical** → assert the verdict carries no disabling
  consequence at all, only a log line.

That covers the comparison logic exhaustively and cheaply, with no live DLL involved. It does not by
itself prove the *wiring* reaches a real loaded DLL rather than a mocked struct — that needs a heavier,
end-to-end smoke test, run at least once per stage landing rather than on every build: build the tree
once; bump `AVER_SCENE_ABI_VERSION_MINOR` (or, once Stage 3 lands, edit one field's position in
`scene_abi.h`) and rebuild **only** `Aver.Scene.dll`; copy that one DLL over the otherwise-unrebuilt
`bin/Aver.Scene.dll`; launch `Sandbox.exe --frames 1` and grep its log for the specific decline message
this plan's failure policy (§5) specifies. This is the direct analogue of physically swapping in a stale
DLL — the actual shape of the recorded incidents in §8 — and it is the only test in this design that
proves the check fires against a *real* mismatched artifact rather than a value the test constructed by
hand. It is also the test most likely to be skipped in practice because it requires a partial rebuild
discipline nothing else in the suite needs, which is precisely why it should be named explicitly as a
required manual/CI step per stage rather than left implicit.

## 8. What this would and would NOT have caught

Checked against the four incidents named in the task, in order, and stated plainly where the answer is
uncomfortable: **none of the four would have been caught by the mechanism this plan proposes, because
all four live on a boundary other than "a native SHARED module's own declared ABI/version."** That is
worth sitting with before describing the theoretical value this check does have.

1. **"A managed DLL went stale and the engine ran against the old one without saying so."** Per
   `aver-managed-dll-stale.md`, the root cause was a missing `add_dependencies(Aver.Scripting.Host
   Aver.Scripting.Bridge)` — the bridge simply was not rebuilt, and the fix was a build-graph edge,
   verified by comparing file mtimes, not a version mismatch. The scripting contract check
   (`AverScriptHostApi`, row 2 of the boundary table) already existed at the time and would only have
   caught this if the stale bridge also happened to disagree on `structBytes`/`contractVersion` — a
   stale-but-shape-identical rebuild sails through that check exactly as cleanly as a fresh one, because
   the check verifies shape, not build recency. This plan's native module-report mechanism (§4) does not
   even reach this boundary — it targets the eight native SHARED DLLs, and the scripting bridge is a
   managed assembly loaded through `hostfxr`, not one of them. The one field in this design aimed at
   exactly this failure mode is `buildId` — but as staged, `buildId` is proposed only for the native
   report struct, not extended to `AverScriptHostApi`. **Not caught, as staged.** Extending
   `buildId`-style stamping to the scripting contract (a variant of Stage 4) is the closest this plan
   comes to addressing it, and is named, not committed, for exactly that reason.
2. **"A DllImport in the C# bridge resolved to the WRONG DLL, and the symptom was a gameplay bug."** Per
   `aver-bridge-dllimport-wrong-dll.md`, the cause was a raw `[DllImport("Aver.Framework")]` declared
   outside the one class (`Fw` in `Aver.Framework/Native.cs`) whose module initializer installs
   `NativeResolver`'s `SetDllImportResolver` — every P/Invoke declared anywhere else in that assembly
   falls through to the CLR's default probing, finds the managed `bin/Scripting/Aver.Framework.dll`
   first, and fails `GetProcAddress` against a pure-IL PE for every symbol. This is an authoring-discipline
   bug about *where a declaration lives in a C# file*, not a shape or version mismatch — a version-report
   P/Invoke call added under this plan's design would be subject to **the exact same misplacement risk**:
   if the report call itself were declared in the wrong file, it would fail to resolve identically to how
   the original bug's symbols failed, proving nothing except that the checker's own call site was placed
   correctly (or not). And even a correctly-placed report call proves only that *its own* P/Invoke
   resolved through the right path — it cannot audit every *other* declaration in the same assembly for
   the identical mistake. **Not reliably caught, and not the right tool for this incident** — this is
   closer to a code-review or Roslyn-analyzer problem (flag any `[DllImport]` outside the class that owns
   the resolver) than to anything a runtime version check can see.
3. **"pbr::MaterialConstants... grew 80 → 96 bytes TODAY and only a runtime check caught it."** Whatever
   caught this in practice was necessarily a rendering-correctness oracle (something like a furnace/energy
   test reading back shaded pixels and noticing the numbers were wrong) — a downstream symptom of the
   upstream cause, not a startup ABI check. This defect sits entirely inside row 6 of the boundary table:
   a same-binary, cross-language mismatch between a C++ struct (`MaterialGpu.hpp:32-90`) and an HLSL
   `cbuffer` string (`PbrShaders.cpp`) that both exist inside one already-linked static target
   (`Aver.Render.PBR.Materials`) before a DLL even enters the picture. There is no load event, no
   `*_abi.h` header, and no exported function this check's mechanism could hook. **Squarely and
   unambiguously not caught, and not catchable by any version of the module-report design in this
   document** — this is the incident the task brief leans on hardest as motivation, and the honest answer
   is that a module-ABI check, however well built, operates one layer away from where this defect lives.
4. **"...a static_assert on size that... cannot catch a field REORDERED within the same size."** The
   same answer as incident 3, for the same reason: `PathTracer.cpp:64-68`'s own admission is about a
   compile-time, single-translation-unit, cross-language mirror with no loadable-module boundary in
   sight. **Not caught, by the same structural argument.**

The honest summary: this plan closes a real, currently-unenforced gap — the tree already has two working
version accessors (Scene, Framework) that nothing calls, and six modules with no version surface at all
— and doing so removes a *theoretical* risk that has not yet produced one of this project's recorded
incidents but plausibly could (a Scene/Framework major-version drift going unnoticed is exactly the
scenario `framework_abi.h:64-67` was written to worry about, even though it has not yet happened). It
does not retroactively fix any of the four incidents the task cites, because all four happened on
boundaries — the managed scripting bridge's build recency, a misplaced C# declaration, and a same-binary
C++/HLSL mirror — that a native module-ABI/version check does not reach, by the nature of what it is.
Selling it as covering those four would be exactly the kind of overclaim this codebase's history warns
against.

## 9. Open questions

**Does Stage 4 (managed-bridge buildId stamping) belong in this initiative at all?** It is the only piece
of this plan that touches incident 1 even partially, but it operates on a different mechanism (a
managed-side P/Invoke call inside `NativeResolver.Resolve`) and a different failure model (C# exceptions,
not a C function returning a struct) than Stages 1–3. Folding it in makes the plan's scope match the
brief's motivating incidents more closely; leaving it out keeps "the startup module check" honestly
scoped to the native DLL boundary the title describes. This is a scope call for the user, not an
engineering one.

**Raw byte hash now, or a comment/whitespace-normalized hash first?** §4 already flags line-ending drift
as a real false-positive source for a raw hash. Shipping the crude version first (Stage 3) is faster and
matches this document's "smallest slice first" bias, but means the very first release of this feature can
misfire on a machine with different `git autocrlf` settings. Building the normalized version first delays
Stage 3 but avoids shipping a known false-positive source. The user's tolerance for a noisy first release
versus a slower one should decide this, not an engineering default.

**Where exactly does the report-and-compare call live?** This plan assumes `SandboxApp.cpp`'s `onInit`,
mirroring its existing `#if AVER_MODULE_X` vocabulary. But `Aver.Runtime.Game` is a second, growing
composition root (per the standing runtime-build-plan), and if it eventually hosts its own
`aver::Application`, it will need the identical call site duplicated into its own `onInit` — which is
exactly the kind of "declared but unread because nobody remembered to also wire it here" duplication this
engine has been bitten by more than once. A small shared helper function both roots call (living where?
Aver.Runtime, if it can be given a way to call into optional modules without linking them directly — an
open design question in itself) versus accepting the duplication as the cost of the mechanism-vs-facts
split is a real decision this document does not make on the user's behalf.

**How surgical should "disable" be, per module?** For Physics, disabling might reasonably mean "skip
physics stepping and character queries entirely." For PBR, it might mean "fall back to the flat-shaded
default." For Settings, disabling entry points that would otherwise silently no-op might be worse than
leaving them alone. This plan proposes a uniform "null the affected pointers / flip an availability flag"
policy in §5, but what a caller *does* with a disabled module is necessarily module-specific product
judgement this document should not make unilaterally for six different subsystems.

**Should the check be mandatory in every build configuration, or opt-in per composition root?** This
plan assumes it runs unconditionally inside any executable that hosts `aver::Application`, on the theory
that a passing check costs nothing and a failing one is exactly the information worth having. A user who
wants it configurable (e.g. off in a Release build, on only in Debug/CI) should say so before Stage 1
lands, since the call site's placement and guard conditions are easiest to get right the first time.

**Is Stage 5 (the ~12 hand-mirrored native/C# enum pairs from `docs/ABI.md` §18) worth pursuing under
this initiative's name at all**, given it needs infrastructure — a cross-language pairing check, or
codegen from one side to the other — that this document deliberately does not design? It is named as a
known, adjacent gap; whether closing it is this project's next priority or a separate piece of work
entirely is a scoping decision, not an engineering one.
