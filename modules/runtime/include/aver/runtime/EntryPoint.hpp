// Include this in exactly ONE translation unit of an application to generate main().
#pragma once
#include "Engine.hpp"
#include "Application.hpp"

// Builds the application, runs the engine, and returns the process code.
int main(int argc, char** argv) {
    aver::Application* app = aver::createApplication(argc, argv);
    if (!app) return 1;
    aver::Engine engine;
    const int rc = engine.run(app);
    delete app;
    return rc;
}
