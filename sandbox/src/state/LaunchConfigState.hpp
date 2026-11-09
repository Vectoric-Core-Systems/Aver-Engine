// Launch configuration and prefs-load flags: backend/windowing CLI choices, culture, scripts dir, single-instance and chrome toggles.
// Included from SandboxApp.hpp (inside namespace aver) after the engine headers; not a standalone header.
#pragma once

struct LaunchConfigState {
    // --no-editor-chrome: suppress editor chrome for comparison with AverEngineRuntime.exe.
    bool noEditorChrome_ = false;
    bool hideEditorScene_ = false;
    rhi::IDevice* prefsDevice_ = nullptr;
    std::string prefIdeName_;
    bool prefsLoaded_ = false;
    // Set by loadLevel when level supplied CAMERA record; read by onAttach.
    bool levelCameraRestored_ = false;
    bool wantMeshReload_ = false;
    std::string startMode_;
    // --set NAME VALUE pairs, applied once through runConsoleLine on the first frame with a device.
    std::vector<std::pair<std::string, std::string>> consoleSets_;
    bool useWarp_=false;             // --warp: run on the D3D12 software rasteriser
    // The engine, for the log sink to write the startup splash through. Set at the top of onInit.
    Engine* engineForSplash_ = nullptr;
    std::thread::id mainThreadId_{};
    bool windowedOverride_=false;    // --windowed: kept so the flag still parses; windowed is the default now
    bool fullscreenOverride_=false;  // --fullscreen: opt an interactive run INTO borderless fullscreen
    std::string backendName_;   // --backend: which RHI backend to ask for first
    std::string cultureOverride_;   // --culture: beats the saved preview culture (editor pref loc.previewCulture)
    // RENDER.BACKEND as the OPEN PROJECT states it. Separate from backendName_ (what this run launched with).
    std::string projectBackend_;
    std::string runningBackend_ = "?";   // what the device actually came back as, latched at init
    bool debugLayer_=false;          // --debug-layer: validate every graphics call (a real per-call tax)
    std::string scriptsDir_;         // --scripts <dir>: where to look for user script assemblies
    // Whether THIS launch's own command line was eligible to register as the single-instance primary.
    bool singleInstanceEligible_ = false;
};
