// The data shared by every emitter that plays the same effect, and the per-particle simulation
// state both executors (the CPU EmitterSim and the GPU kernels) update every tick. No entity, no
// world, no RHI device -- this header is pure data, included by both halves of the module
// (ParticleSystem, which never touches the RHI, and ParticleRenderer, which never touches
// scene::World) and by anything else that just wants to describe or read an effect (the
// .ocparticle reader/writer, the editor panel).
#pragma once
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/rhi/RHIResources.hpp"   // rhi::BlendMode -- reused wholesale, no new blend infrastructure

#include <cstddef>
#include <string>
#include <vector>

namespace aver::particles {

// Where a new particle is placed, relative to the emitter's world transform. The first three are the
// 0.7 shapes (values unchanged); a widening smoke column, falling snow and a waterfall's mist all
// read from the same shapes depending only on which one and what size is authored.
enum class EmitterShape : u32 {
    Point    = 0,   // every particle spawns at the emitter's origin
    Sphere   = 1,   // uniformly inside a sphere of radius shapeSize.x
    Box      = 2,   // uniformly inside a box of half-extents shapeSize
    Cylinder = 3,   // radius shapeSize.x, full height shapeSize.y centred on the origin, axis +Z
    Disc     = 4,   // radius shapeSize.x in the XY plane
};

// Where a given emitter instance simulates. Auto picks per instance when it is created and never changes.
enum class SimTier : u8 { Auto = 0, Cpu = 1, Gpu = 2 };
enum class RenderKind : u8 { Sprite = 0, Ribbon = 1, Mesh = 2 };
enum class SpriteAlign : u8 { Camera = 0, Velocity = 1, Axis = 2 };
// receivesGI true == Ambient (see effectiveLighting).
enum class Lighting : u8 { Unlit = 0, Ambient = 1, Lit = 2 };
enum class SortMode : u8 { Auto = 0, None = 1, Depth = 2 };   // Auto: Depth for AlphaBlend / PremultipliedAlpha

// The closed set of 13 data-driven modules (ParticleModules.hpp has the table).
enum class ModuleKind : u8 {
    SpawnSizeScale = 0,
    SpawnColorTint,
    SpawnRotation,
    SpawnInheritVelocity,
    UpdateForce,
    UpdateDrag,
    UpdateAttractor,
    UpdateVortex,
    UpdateCurlNoise,
    UpdateSpeedLimit,
    UpdateCollidePlane,
    UpdateCollideSphere,
    UpdateKillVolume,
    Count
};

inline constexpr u32 kMaxModulesPerEmitter = 16;
inline constexpr u32 kMaxEmittersPerSystem = 8;
inline constexpr u32 kMaxUserParams = 16;
inline constexpr u32 kMaxModuleParams = 8;
inline constexpr u32 kMaxTrailPoints = 32;
inline constexpr u32 kMaxLinkedCapacity = 4096;

// A module parameter: a literal, or a reference to one component of a user-parameter slot.
struct ModuleParam {
    f32 value = 0.0f;
    i8  userSlot = -1;   // -1: the literal; else index into the system's user parameters
    u8  comp = 0;        // 0..3 = x y z w
};

struct Module {
    ModuleKind kind = ModuleKind::SpawnSizeScale;
    bool enabled = true;
    ModuleParam p[kMaxModuleParams];
};

// Up to four (t, value[4]) stops. n == 0: unused (the start/end pair applies). n >= 2: a piecewise-linear curve.
struct Curve4 {
    u8  n = 0;
    f32 t[4] = {0, 0, 0, 0};
    f32 v[4][4] = {};
};

// A named, script-settable value. kind 0 float (v[0]), 1 vec3 (v[0..2]), 2 colour (v[0..3]).
struct UserParam {
    std::string name;
    u8  kind = 0;
    f32 v[4] = {0, 0, 0, 0};
};

// ONEVENT death|collide SPAWN child count [inherit k].
struct EventSpawn {
    u8  on = 0;               // 0 death, 1 collide
    u32 emitter = 0;          // child emitter index inside the same system
    u32 count = 1;
    f32 inheritVelocity = 0;  // fraction of the dying particle's velocity the children start with
};

// One authored EMITTER: everything about how particles are born, move and fade. The members above the
// "----" divider below keep their 0.7 names, defaults and meaning, so every existing caller compiles.
struct ParticleEffect {
    EmitterShape shape = EmitterShape::Point;
    Vec3 shapeSize{0, 0, 0};   // Sphere: radius in .x. Box: half-extents. Cylinder: radius .x, height .y. Disc: radius .x.

    f32 emissionRate = 10.0f;   // particles/second, continuous, while playing. 0 = none (burst-only).
    u32 burstCount   = 0;       // spawned once on the stopped -> playing edge, on top of the rate.
    u32 maxParticles = 512;     // this effect's live-particle cap, PER EMITTER.

    f32 lifetimeMin = 1.0f, lifetimeMax = 1.0f;   // seconds, drawn per particle at spawn.

    Vec3 direction = {0, 0, 1};   // base emission direction; normalised on use.
    f32  spreadDeg = 0.0f;        // half-angle of the cone around `direction`, in degrees (180 = a full sphere).
    f32  speedMin = 100.0f, speedMax = 100.0f;   // cm/s, initial speed range.

    Vec3 gravity = {0, 0, 0};   // cm/s^2, constant and uniform.
    f32  damping = 0.0f;        // fraction of velocity removed per second, [0, 1).

    f32 sizeStart = 10.0f, sizeEnd = 10.0f;   // billboard edge length, centimetres.
    f32 colorStart[4] = {1, 1, 1, 1};           // straight (non-premultiplied) rgba, birth.
    f32 colorEnd[4]   = {1, 1, 1, 0};           // death.

    // The shader premultiplies rgb by alpha, so only PremultipliedAlpha and Additive compose correctly; the
    // other two values are accepted as data and draw through the premultiplied pipeline.
    rhi::BlendMode blend = rhi::BlendMode::PremultipliedAlpha;

    // fnv1a64 id of a texture the host resolves (ParticleRenderer::setTextureResolver); 0, or an id the host
    // cannot resolve, draws the procedural soft dot.
    u64 textureId = 0;

    // Sample the GI term (Voxi) whenever a seam is installed. A particle that is its own light source
    // (an ember, a spark) writes false. See effectiveLighting.
    bool receivesGI = true;

    // ---- added after 0.7 ----
    SimTier simTier = SimTier::Auto;
    RenderKind render = RenderKind::Sprite;
    SpriteAlign align = SpriteAlign::Camera;
    Vec3 alignAxis{0, 0, 1};
    f32 stretch = 0;                    // Velocity alignment: length *= 1 + stretch * speed / 100
    u16 subuvCols = 1, subuvRows = 1, subuvFrames = 1;
    f32 subuvFps = 0;                   // 0: play the sheet once over the particle's life
    bool subuvBlend = false;
    u64 meshId = 0;                     // fnv1a64(meshPath); the host resolves it
    std::string meshPath;               // content-relative .ocmesh path (round-trips through the file)
    u8  meshOrient = 0;                 // 0 velocity, 1 spin, 2 fixed
    u8  trailPoints = 12;               // ribbon trail polyline points per particle (2..32)
    f32 trailSegmentCm = 4;             // a trail point is pushed when the particle has moved this far
    f32 trailFade = 1;                  // alpha falloff along the trail (0 none, 1 linear)
    bool linked = false;                // ribbon: consecutive spawns joined, ring-allocated
    u16 linkedCapacity = 1024;
    Lighting lighting = Lighting::Ambient;
    SortMode sort = SortMode::Auto;
    bool fog = true;
    f32 boundsRadiusCm = 0;             // 0: unbounded (the editor's Compute bounds fills it)
    f32 cullDistanceCm = 0;             // 0: never
    bool spawnAtTarget = true;          // takes part in spawnAt requests
    bool shapeShell = false;            // sphere / cylinder / disc: spawn on the surface only
    Curve4 sizeCurve, colorCurve;       // override sizeStart/End and colorStart/End when n >= 2
    std::vector<Module> modules;        // authored order is execution order within a phase
    std::vector<EventSpawn> events;
    std::string name;                   // emitter name (events and the editor refer to it)
    // G6 SH3: the name of a registered converted Godot particles shader (setGodotParticlePrograms). Its start() runs when a particle is born and its process() on every
    // update, replacing the built-in integration; it needs the GPU tier (an emitter that lands on the CPU tier simulates without it). Empty = none, today's emitter.
    std::string godotShader;
};

// A whole authored effect: one or more emitters sharing user parameters.
struct ParticleSystemDef {
    std::string name;
    std::vector<ParticleEffect> emitters;
    std::vector<UserParam> params;
};

// receivesGI false is Unlit regardless of `lighting`; true means `lighting` (Ambient or Lit).
inline Lighting effectiveLighting(const ParticleEffect& e) {
    if (!e.receivesGI) return Lighting::Unlit;
    return e.lighting == Lighting::Unlit ? Lighting::Ambient : e.lighting;
}

// One simulated particle: 64 bytes, four float4s, identical in particles_common.hlsli.
//   id          per-instance spawn counter, unique and never reused (slot order on the GPU is not deterministic)
//   tint        RGBA8, multiplies the colour curve
//   spawnInfo   low 16 bits start frame, high 16 bits flags (kParticleFlagCollided)
//   trail       low 8 bits ring head, next 8 bits point count (trail ribbons)
struct Particle {
    Vec3 position;   f32 age;
    Vec3 velocity;   f32 lifetime;
    u32  seed;       u32 id;        f32 rotation;   f32 angularVelocity;
    f32  sizeScale;  u32 tint;      u32 spawnInfo;  u32 trail;
};
static_assert(sizeof(Particle) == 64, "Particle must stay four float4s: the HLSL twin reads it as a 64-byte stride");
static_assert(offsetof(Particle, seed) == 32 && offsetof(Particle, sizeScale) == 48, "Particle layout drifted from particles_common.hlsli");

inline constexpr u32 kParticleFlagCollided = 1u;   // in spawnInfo >> 16: the collide event has fired for this particle

inline bool particleAlive(const Particle& p) { return p.age < p.lifetime; }

} // namespace aver::particles
