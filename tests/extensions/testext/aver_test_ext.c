/* A tiny extension written in C against aver_ext.h only (no engine library): the library ExtensionsTest loads.
 * Commands: echo (returns the number of arguments), exit7, log (one line per level), dirs, model (asks the host to import a model).
 * Built twice: as is, and with AVER_TEST_EXT_REFUSE, whose entry refuses every host (the "does not support this ABI" answer). */
#include "aver/extensions/aver_ext.h"

#include <stdio.h>
#include <string.h>

static const AverExtHost* g_host = 0;

static void say(int32_t level, const char* text) {
    if (g_host && g_host->log) g_host->log(level, text);
}

static int32_t runCommand(const char* name, int32_t argc, const char* const* argv) {
    char line[512];
    if (strcmp(name, "echo") == 0) {
        int32_t i;
        line[0] = 0;
        for (i = 1; i < argc; ++i) {
            strncat(line, argv[i], sizeof(line) - strlen(line) - 2);
            strncat(line, "|", sizeof(line) - strlen(line) - 1);
        }
        say(AVER_EXT_LOG_INFO, line);
        return argc - 1;
    }
    if (strcmp(name, "exit7") == 0) return 7;
    if (strcmp(name, "log") == 0) {
        say(AVER_EXT_LOG_TRACE, "level-trace");
        say(AVER_EXT_LOG_INFO, "level-info");
        say(AVER_EXT_LOG_WARN, "level-warn");
        say(AVER_EXT_LOG_ERROR, "level-error");
        return 0;
    }
    if (strcmp(name, "dirs") == 0) {
        snprintf(line, sizeof(line), "exe=%s", g_host->executableDir); say(AVER_EXT_LOG_INFO, line);
        snprintf(line, sizeof(line), "ext=%s", g_host->extensionDir); say(AVER_EXT_LOG_INFO, line);
        snprintf(line, sizeof(line), "engine=%s", g_host->engineVersion); say(AVER_EXT_LOG_INFO, line);
        snprintf(line, sizeof(line), "abi=%d size=%u", (int)g_host->abi, (unsigned)g_host->size); say(AVER_EXT_LOG_INFO, line);
        g_host->progress(0.5, "halfway");
        return 0;
    }
    if (strcmp(name, "model") == 0) {
        AverExtModelArgs args;
        AverExtModelResult res;
        uint32_t i;
        int32_t rc;
        if (argc < 5) return 2;
        memset(&args, 0, sizeof(args));
        memset(&res, 0, sizeof(res));
        args.size = sizeof(args);
        args.input = argv[1];
        args.contentDir = argv[2];
        args.outSubdir = argv[3];
        args.base = argv[4];
        res.size = sizeof(res);
        rc = g_host->importModel(&args, &res);
        snprintf(line, sizeof(line), "model rc=%d ok=%d error=%s parts=%u notes=%u", (int)rc, (int)res.ok, res.error ? res.error : "(null)",
                 (unsigned)res.partCount, (unsigned)res.noteCount);
        say(AVER_EXT_LOG_INFO, line);
        for (i = 0; i < res.partCount; ++i) {
            snprintf(line, sizeof(line), "part %s|%s|%g|%g|%g|%d", res.parts[i].meshAsset, res.parts[i].name, res.parts[i].x, res.parts[i].y,
                     res.parts[i].z, (int)res.parts[i].node);
            say(AVER_EXT_LOG_INFO, line);
        }
        for (i = 0; i < res.noteCount; ++i) {
            snprintf(line, sizeof(line), "note %s", res.notes[i]);
            say(AVER_EXT_LOG_INFO, line);
        }
        rc = res.ok;
        g_host->freeModelResult(&res);
        return rc ? 0 : 1;
    }
    return 99;
}

static void shutdownExt(void) { say(AVER_EXT_LOG_INFO, "test extension shutdown"); }

AVER_EXT_EXPORT int32_t aver_extension_entry(const AverExtHost* host, AverExtApi* out) {
#ifdef AVER_TEST_EXT_REFUSE
    (void)host; (void)out;
    return 1;
#else
    if (!host || !out || host->abi != AVER_EXT_ABI) return 1;
    g_host = host;
    out->runCommand = runCommand;
    out->shutdown = shutdownExt;
    say(AVER_EXT_LOG_INFO, "test extension loaded");
    return 0;
#endif
}
