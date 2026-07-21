#ifndef AVER_SCRIPTING_ABI_H
#define AVER_SCRIPTING_ABI_H

/* Aver scripting — the host <-> managed-bridge contract.
 *
 * Deliberately plain C, for the same reason as voxi_abi.h: this struct is read by C# as a
 * blittable `[StructLayout(LayoutKind.Sequential)]` type, so only int32_t and pointers may
 * appear in it. Strings are UTF-8 `const char*` in BOTH directions — the managed side gets
 * `Marshal.PtrToStringUTF8` / `Marshal.StringToCoTaskMemUTF8` for free, and the native side
 * needs no wide-character conversion to reach the engine log.
 *
 * There is no dllexport here. The host does not export symbols for the bridge to P/Invoke back
 * into: it hands the bridge a table of function pointers at bootstrap instead. A P/Invoke would
 * have to name the loaded module, which is the EXECUTABLE (`Sandbox.exe` today), and that would
 * tie a bridge assembly shipped with the engine to whatever host happens to embed it.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Host <-> bridge contract version. Bump when the shape of AverScriptHostApi, the entry-point
 * names, or their signatures change. The bridge compares this against its own compiled-in
 * constant and refuses to bootstrap on a mismatch, so a stale Aver.Scripting.Bridge.dll next to
 * a newer executable is reported rather than crashed through.
 *
 * This is NOT the version a user script is checked against — that one is the assembly version of
 * Aver.Scripting, checked by the bridge per loaded assembly. Two boundaries, two checks.
 *
 * v2 adds `UnloadScripts`, the drain half of hot reload. A v1 bridge next to a v2 host would bind
 * every entry point it does have and then simply not reload, which is the failure this constant
 * exists to turn into a message. */
#define AVER_SCRIPTING_CONTRACT_VERSION 2

/* Log levels — must match aver::LogLevel. */
#define AVER_SCRIPT_LOG_TRACE 0
#define AVER_SCRIPT_LOG_INFO  1
#define AVER_SCRIPT_LOG_WARN  2
#define AVER_SCRIPT_LOG_ERROR 3

/* Managed code logs through the engine's log, never through Console: the editor's Output Log is
 * the only place a user will look, and a hosted CLR has no console attached in a GUI process. */
typedef void(__cdecl* aver_script_log_fn)(int32_t level, const char* utf8Message);

typedef struct AverScriptHostApi {
    int32_t structBytes;      /* sizeof(AverScriptHostApi); lets the bridge reject a short struct */
    int32_t contractVersion;  /* AVER_SCRIPTING_CONTRACT_VERSION as the host was built with */
    aver_script_log_fn log;
} AverScriptHostApi;

/* The bridge's [UnmanagedCallersOnly] entry points, in the order the host binds them:
 *
 *   int32_t Bootstrap(const AverScriptHostApi*)  install the host API, check the contract
 *   int32_t LoadScripts(const char* utf8Dir)     load a directory of assemblies; live count back
 *   int32_t UnloadScripts(void)                  drain OnShutdown and unload the collectible ALC
 *   void    Update(float dt)                     drive OnUpdate on every live behaviour
 *   void    Shutdown(void)                       drain, unload, drop the host API
 *
 * Hot reload is UnloadScripts -> (the host rebuilds the assemblies) -> LoadScripts. The rebuild
 * step is deliberately native: the host already owns the `dotnet build` shell-out, and a managed
 * side that spawned compilers would be doing a job it has no business knowing about.
 *
 * UnloadScripts returns 1 when the old load context was fully collected and 0 when it is still
 * finalising. Both are success — unloading in .NET is a REQUEST, satisfied only once every
 * reference is dropped and a GC has run, so a 0 means "the old context is still costing memory",
 * never "the reload failed". Assemblies are loaded from memory streams, so nothing on disk is
 * locked either way and the rebuild does not have to wait for the answer. */

/* Return codes from the bridge's Bootstrap entry point. Negative is failure, and the host maps
 * each to a message naming what is stale, because "scripting failed" sends nobody anywhere. */
#define AVER_SCRIPT_OK                    0
#define AVER_SCRIPT_ERR_CONTRACT         (-1) /* contractVersion / structBytes disagree */
#define AVER_SCRIPT_ERR_MANAGED_FAULT    (-2) /* an exception escaped inside the bridge itself */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AVER_SCRIPTING_ABI_H */
