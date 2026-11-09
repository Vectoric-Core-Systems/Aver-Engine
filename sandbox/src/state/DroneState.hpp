// Editor drone pawn (graph-driven test pawn) runtime state.
// Included from SandboxApp.hpp (inside namespace aver) after the engine headers; not a standalone header.
#pragma once

struct DroneState {
#if AVER_MODULE_SCENE
    // Graph-driven drone (opt-in, transient). Spawned by setDroneEnabled, ticked via scripts_.graphTick.
    scene::Entity droneEntity_ = scene::kInvalidEntity;
    // True if Play started this drone (vs user enabling from Window > Drone).
    bool droneStartedByPlay_ = false;
    // .ocgraph path (relative to Content; set only by --drone-graph).
    std::string droneGraphRel_;
    bool droneGraphLoaded_ = false;
    f32  droneTimeSeconds_ = 0.0f;
    Vec3 dronePos_{};
    Vec3 droneVel_{};
    Vec3 droneLastPos_{};
    bool droneHaveLastPos_ = false;         // False right after enabling; first velocity sample is zero.
    u32  droneLogsLeft_ = 30;               // More than streaming's 8.
#endif
};
