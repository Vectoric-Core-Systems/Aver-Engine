/* A second tiny extension written in C against the headers alone: the library ExtensionsTest loads to exercise the parts of the contract the
 * first one leaves out. It uses a host service (aver.asset.import-model), offers an interface (com.example.greeter, version 1) and reports
 * through the command protocol (aver_ext_protocol.h) on stdout.
 * Commands: convert <input> <contentDir> <outSubdir> <base> (imports through the service, prints progress and a result line),
 *           fail (prints a result line with ok false), noresult (prints progress only). */
#include "aver/extensions/aver_ext.h"
#include "aver/extensions/aver_ext_protocol.h"
#include "aver/extensions/aver_ext_services.h"

#include <stdio.h>
#include <string.h>

static const AverExtHost* g_host = 0;

/* The interface this extension offers. */
typedef struct GreeterInterface {
    uint32_t size;
    const char* (*greet)(const char* who);
} GreeterInterface;

static const char* greet(const char* who) {
    static char buf[128];
    snprintf(buf, sizeof(buf), "hello, %s", who ? who : "nobody");
    return buf;
}

static const GreeterInterface g_greeter = {sizeof(GreeterInterface), greet};

static const void* getInterface(const char* name, uint32_t version) {
    if (name && strcmp(name, "com.example.greeter") == 0 && version == 1) return &g_greeter;
    return 0;
}

static void progress(double fraction, const char* text) {
    printf("{\"" AVER_EXT_MSG_KEY "\":\"" AVER_EXT_MSG_PROGRESS "\",\"fraction\":%g,\"text\":\"%s\"}\n", fraction, text);
    fflush(stdout);
}

static void result(int ok, const char* open, const char* report, int warnings, int errors, const char* message) {
    printf("{\"" AVER_EXT_MSG_KEY "\":\"" AVER_EXT_MSG_RESULT "\",\"ok\":%s,\"open\":\"%s\",\"report\":\"%s\",\"warnings\":%d,\"errors\":%d,\"message\":\"%s\"}\n",
           ok ? "true" : "false", open, report, warnings, errors, message);
    fflush(stdout);
}

static int32_t runCommand(const char* name, int32_t argc, const char* const* argv) {
    if (strcmp(name, "convert") == 0) {
        const AverExtImportModelService* svc;
        AverExtModelArgs args;
        AverExtModelResult res;
        char message[256];
        if (argc < 5) { result(0, "", "", 0, 1, "convert needs <input> <contentDir> <outSubdir> <base>"); return 2; }
        svc = (const AverExtImportModelService*)g_host->getService(AVER_EXT_SERVICE_IMPORT_MODEL, AVER_EXT_SERVICE_IMPORT_MODEL_VERSION);
        if (!svc) { result(0, "", "", 0, 1, "the host offers no aver.asset.import-model service"); return 1; }
        progress(0.0, "importing");
        memset(&args, 0, sizeof(args));
        memset(&res, 0, sizeof(res));
        args.size = sizeof(args);
        args.input = argv[1];
        args.contentDir = argv[2];
        args.outSubdir = argv[3];
        args.base = argv[4];
        res.size = sizeof(res);
        svc->importModel(&args, &res);
        progress(0.5, "imported");
        if (!res.ok) {
            snprintf(message, sizeof(message), "import failed: %s", res.error ? res.error : "");
            result(0, "", "", 0, 1, message);
            svc->freeResult(&res);
            return 1;
        }
        snprintf(message, sizeof(message), "%u part(s), first at source index %d", (unsigned)res.partCount, res.partCount ? (int)res.parts[0].sourceIndex : -1);
        progress(1.0, "done");
        result(1, "C:/out/Game/Game.ocproject", "C:/out/Report.md", (int)res.noteCount, 0, message);
        svc->freeResult(&res);
        return 0;
    }
    if (strcmp(name, "fail") == 0) {
        result(0, "", "", 1, 2, "it failed on purpose");
        return 0;   /* ok:false is a failure whatever the exit code says */
    }
    if (strcmp(name, "noresult") == 0) {
        progress(0.25, "started");
        return 0;
    }
    return 99;
}

static void shutdownExt(void) { g_host = 0; }

AVER_EXT_EXPORT int32_t aver_extension_entry(const AverExtHost* host, AverExtApi* out) {
    if (!host || !out || host->abi != AVER_EXT_ABI) return 1;
    g_host = host;
    out->runCommand = runCommand;
    out->shutdown = shutdownExt;
    out->getInterface = getInterface;
    return 0;
}
