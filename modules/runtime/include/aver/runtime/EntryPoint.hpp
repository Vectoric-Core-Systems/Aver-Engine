// Include this in exactly ONE translation unit of an application to generate main().
#pragma once
#include "Engine.hpp"
#include "Application.hpp"
#include "aver/core/CrashReport.hpp"
#include "aver/core/Version.hpp"

#include <string>

// Builds the application, runs the engine, and returns the process code.
int main(int argc, char** argv) {
    // FIRST STATEMENT, BEFORE ANYTHING ELSE, and that placement is the whole point. A fault before
    // this line produces the operating system's own "a program stopped working" box and no report at
    // all -- and startup, where a missing DLL or a driver that will not initialise lives, is exactly
    // where a first-run crash is most likely and least self-explanatory. createApplication() below
    // already parses the command line and touches the filesystem; it must not be the first thing to
    // run. See CrashReport.hpp for what a report contains and why the reporter is its own process.
    {
        aver::crash::Config cfg;
        cfg.appName       = "Aver";
        cfg.engineVersion = AVER_ENGINE_VERSION;
#if defined(NDEBUG)
        cfg.buildConfig = "Release";
#else
        cfg.buildConfig = "Debug";
#endif
        // Rebuilt rather than taken from GetCommandLineA(): argv is what this process actually
        // parsed, and a report that shows a different command line from the one the code read is
        // worse than no command line at all.
        for (int i = 0; i < argc; ++i) {
            if (i) cfg.commandLine += ' ';
            cfg.commandLine += argv[i];
        }
        aver::crash::install(cfg);
    }

    aver::Application* app = aver::createApplication(argc, argv);
    if (!app) return 1;
    aver::Engine engine;
    const int rc = engine.run(app);
    delete app;

    // Uninstalled on the way out so a clean exit is clean. It matters because of the standby
    // reporter: a Critical earlier in the session may have left AverCrashReporter.exe watching this
    // process, and it decides whether to say anything by looking at the exit code. Returning normally
    // from here is what tells it nothing went wrong.
    aver::crash::shutdown();
    return rc;
}
