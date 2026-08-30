#ifndef AVER_SCRIPTING_ABI_H
#define AVER_SCRIPTING_ABI_H

/* Aver scripting — the host <-> managed-bridge contract.
 *
 * Plain C: AverScriptHostApi is read by C# as a blittable [StructLayout(LayoutKind.Sequential)]
 * type, so only int32_t and pointers may appear in it. Strings are UTF-8 const char* both ways.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Host <-> bridge contract version. Bump whenever the shape of AverScriptHostApi, the entry-point
 * names, or their signatures change; the bridge refuses to bootstrap on a mismatch. */
#define AVER_SCRIPTING_CONTRACT_VERSION 3

/* Log levels — must match aver::LogLevel. */
#define AVER_SCRIPT_LOG_TRACE 0
#define AVER_SCRIPT_LOG_INFO  1
#define AVER_SCRIPT_LOG_WARN  2
#define AVER_SCRIPT_LOG_ERROR 3
/* Appended, never inserted: these are bare integers on both sides of an ABI boundary, so a value
   added in the middle silently renumbers every level for any bridge built against an older header.
   FATAL does not return on the engine side -- it writes a crash report and terminates the process. */
#define AVER_SCRIPT_LOG_CRITICAL 4
#define AVER_SCRIPT_LOG_FATAL 5

/* Writes one managed log line into the engine's log. */
typedef void(__cdecl* aver_script_log_fn)(int32_t level, const char* utf8Message);

/* The table of host functions handed to the bridge at bootstrap. */
typedef struct AverScriptHostApi {
    int32_t structBytes;      /* sizeof(AverScriptHostApi) */
    int32_t contractVersion;  /* AVER_SCRIPTING_CONTRACT_VERSION as the host was built with */
    aver_script_log_fn log;
} AverScriptHostApi;

/* The bridge's [UnmanagedCallersOnly] entry points, in the order the host binds them: Bootstrap,
 * LoadScripts, UnloadScripts, Update, Shutdown, HudCount, HudName, HudDraw. The HUD three are
 * optional at bind time; UnloadScripts returns 1 when the old load context was fully collected and
 * 0 when it is still finalising, and both are success. */

/* Return codes from the bridge's Bootstrap entry point. Negative is failure. */
#define AVER_SCRIPT_OK                    0
#define AVER_SCRIPT_ERR_CONTRACT         (-1) /* contractVersion / structBytes disagree */
#define AVER_SCRIPT_ERR_MANAGED_FAULT    (-2) /* an exception escaped inside the bridge itself */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AVER_SCRIPTING_ABI_H */
