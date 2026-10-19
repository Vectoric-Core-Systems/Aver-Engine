// The module stack: a closed set of 13 data-driven modules (spawn and update stages), evaluated by the CPU
// executor (EmitterSim) from this file's functions and by the GPU kernels from particles_common.hlsli, which
// evaluates the same table with the same arithmetic. ParticleModulesTest and ParticleGpuParityTest pin the two.
//
// Execution order per tick, identical on both executors:
//   age += dt; die if age >= lifetime; v += gravity*dt; v *= max(0, 1 - damping*dt);
//   velocity-phase modules in authored order; p += v*dt; position-phase modules in authored order;
//   rotation += angularVelocity*dt; trail push; events.
#pragma once
#include "aver/particles/ParticleTypes.hpp"

#include <string_view>
#include <vector>

namespace aver::particles {

enum class ModuleStage : u8 { Spawn = 0, Velocity = 1, Position = 2 };

struct ModuleInfo {
    ModuleKind  kind;
    const char* key;            // the .ocparticle MODULE key
    ModuleStage stage;
    u8          paramCount;
    const char* names[kMaxModuleParams];     // file keys and editor labels
    f32         defaults[kMaxModuleParams];  // an unconfigured module (see the exceptions in the README)
};

const ModuleInfo& moduleInfo(ModuleKind kind);
// nullptr when `key` is not one of the 13 (case-insensitive).
const ModuleInfo* findModuleInfo(std::string_view key);
// Index of the parameter named `name` (case-insensitive), or -1.
int findModuleParam(const ModuleInfo& info, std::string_view name);
// A module with every parameter at its default, enabled.
Module makeDefaultModule(ModuleKind kind);

// One module with its parameters resolved to numbers (user-parameter references read from `user`).
struct ResolvedModule {
    ModuleKind  kind = ModuleKind::SpawnSizeScale;
    ModuleStage stage = ModuleStage::Spawn;
    f32         p[kMaxModuleParams] = {};
};

// Enabled modules only, authored order. `user` may be null (every reference then reads 0).
void resolveModules(const std::vector<Module>& modules, const f32 (*user)[4], std::vector<ResolvedModule>& out);

// Per-step context for the update stages.
struct ModuleEnv {
    Vec3 origin{0, 0, 0};   // the emitter's world position (local attractors, vortex axis)
    f32  time = 0;          // emitter age, seconds (scrolls curl noise)
    f32  dt = 0;
};

// Bits runPositionModule returns.
inline constexpr u32 kModuleKilled = 1u;
inline constexpr u32 kModuleCollided = 2u;   // the first collision of this particle (kParticleFlagCollided was clear)

void runSpawnModule(const ResolvedModule& m, Particle& p, const Vec3& emitterVelocity);
void runVelocityModule(const ResolvedModule& m, const ModuleEnv& env, Particle& p);
u32  runPositionModule(const ResolvedModule& m, const ModuleEnv& env, Particle& p);

// Curl of the module's 3D noise potential at `p` (noise-space units); update.curlnoise multiplies it by strength.
Vec3 curlNoiseField(const Vec3& worldPos, f32 freq, f32 time, f32 scroll, int octaves, u32 seed);

// ---- curves ----
// True and fills out[0..3] when the curve has >= 2 stops; clamps outside [t0, tLast].
bool evalCurve(const Curve4& c, f32 t, f32 out[4]);
f32  evalSize(const ParticleEffect& fx, f32 t);                 // sizeCurve, else sizeStart -> sizeEnd
void evalColor(const ParticleEffect& fx, f32 t, f32 out[4]);    // colorCurve, else colorStart -> colorEnd

inline u32 packRgba8(f32 r, f32 g, f32 b, f32 a) {
    auto q = [](f32 v) { v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); return static_cast<u32>(v * 255.0f + 0.5f); };
    return q(r) | (q(g) << 8) | (q(b) << 16) | (q(a) << 24);
}

} // namespace aver::particles
