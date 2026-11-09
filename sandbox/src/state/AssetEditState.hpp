// In-flight material and placed-component edit gestures, pinned materials, and graph-warmup / level-prep timers.
// Included from SandboxApp.hpp (inside namespace aver) after the engine headers; not a standalone header.
#pragma once

struct AssetEditState {
#if AVER_WITH_IMGUI
#if AVER_MODULE_SCENE
    std::optional<fmt::OcPlacedComponent> placedBefore_;                  // the open gesture's "before"
    const editor::PlacedKind* placedGesture_ = nullptr;                   // the kind that gesture edits; null = none open
    // Authored component state at Play start, so a save during Play writes it and not what a script did since.
    std::unordered_map<u32, std::vector<fmt::OcPlacedComponent>> placedBaseline_;
#endif
#endif
#if AVER_MODULE_PBR
    // One material edit: desc before interaction and which material.
    bool matEditActive_ = false;
    bool matEditDirty_  = false;
    pbr::MaterialHandle matEditHandle_ = 0;
    pbr::MaterialDesc   matEditBefore_{};
#endif
    u64 graphWarmRev_ = 0;               // the registry revision the running graph warm-up was started for
    bool graphWarmHeld_ = false;
    std::chrono::steady_clock::time_point graphWarmT0_{};
    // "Preparing <level>" after the loading screen while ray-tracing structures still build (SandboxRender.cpp).
    bool levelPrepArmed_ = false;
    u64 levelPrepToast_ = 0;
    std::chrono::steady_clock::time_point levelPrepT0_{};
    std::string matSaveStatus_;   // what the last 'Save to C#' did
#if AVER_MODULE_PBR
    // pinMaterialResident()/unpinMaterialResident()'s storage -- checked by bindMaterialsForLevel() so a level-scoped release never destroys a material an open tab holds.
    std::unordered_set<std::string> pinnedMaterialNames_;
#endif
};
