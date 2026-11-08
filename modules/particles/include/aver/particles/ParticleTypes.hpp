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
    Ring     = 5,   // an annulus about shapeAxis: outer radius shapeSize.x, full height shapeSize.y centred on the origin, inner radius shapeSize.z (shell = the outer rim)
};

// Where a given emitter instance simulates. Auto picks per instance when it is created and never changes.
enum class SimTier : u8 { Auto = 0, Cpu = 1, Gpu = 2 };
enum class RenderKind : u8 { Sprite = 0, Ribbon = 1, Mesh = 2 };
enum class SpriteAlign : u8 { Camera = 0, Velocity = 1, Axis = 2 };
// receivesGI true == Ambient (see effectiveLighting).
enum class Lighting : u8 { Unlit = 0, Ambient = 1, Lit = 2 };
enum class SortMode : u8 { Auto = 0, None = 1, Depth = 2 };   // Auto: Depth for AlphaBlend / PremultipliedAlpha

// The closed set of 21 data-driven modules (ParticleModules.hpp has the table). Wave 6 appended the last eight; ids never change.
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
    UpdateLinearAccel,       // along the velocity, a per-particle acceleration drawn from [min, max]
    UpdateRadialAccel,       // away from the emitter origin, per-particle [min, max]
    UpdateTangentialAccel,   // around an axis through the emitter origin, per-particle [min, max]
    UpdateDamping,           // a constant per-particle deceleration [min, max] that stops at zero speed
    UpdateOrbit,             // turns the position about an axis through the emitter origin, per-particle revolutions per second [min, max]
    SpawnHueVariation,       // a per-particle hue turn drawn from [min, max], applied to the drawn colour
    SpawnAnimStart,          // the sprite sheet's first frame drawn from [min, max] of the sheet
    UpdateCollideBox,        // an axis-aligned solid box in world space
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

// The engine's quality ladder, as particles see it: how much of an effect plays. Epic is the effect as authored.
enum class ScalabilityTier : u8 { Low = 0, Medium = 1, High = 2, Epic = 3 };
inline constexpr u32 kScalabilityTiers = 4;

// What one tier does to an emitter. `set` marks a tier the author wrote (an emitter's own override only counts when set; the file
// round-trips exactly the tiers that were set). rateScale multiplies the emission rate and burst count, maxScale the particle cap.
struct TierScale {
    bool set = false;
    bool enabled = true;
    f32 rateScale = 1.0f;
    f32 maxScale = 1.0f;
};
struct Scalability {
    TierScale tier[kScalabilityTiers];
};

// A named, script-settable value. kind 0 float (v[0]), 1 vec3 (v[0..2]), 2 colour (v[0..3]).
struct UserParam {
    std::string name;
    u8  kind = 0;
    f32 v[4] = {0, 0, 0, 0};
};

// What fires an event rule. Death and Collide are the 0.7 triggers (ONEVENT records); the rest are CPU tier only (TRIGGER records).
enum class EventOn : u8 { Death = 0, Collide = 1, Spawn = 2, Lifetime = 3, Timer = 4 };

// Payload fields a child inherits from the parent particle at the event (EventSpawn::payload). Position is always inherited
// and velocity is the `inheritVelocity` fraction, so those two need no bit.
inline constexpr u8 kEventPayloadColor = 1;   // the child's tint is the parent's colour at the event (curve times tint)
inline constexpr u8 kEventPayloadSize  = 2;   // the child's size scale is the parent's size at the event relative to its birth size
inline constexpr u8 kEventPayloadAge   = 4;   // the child is born as far through its life as the parent was through its own
inline constexpr u8 kEventPayloadAll   = 7;

// ONEVENT death|collide SPAWN child count [inherit k]; TRIGGER <kind> ... SPAWN child count [inherit k] [color] [size] [age] [user v].
struct EventSpawn {
    u8  on = 0;               // EventOn
    u32 emitter = 0;          // child emitter index inside the same system
    u32 count = 1;
    f32 inheritVelocity = 0;  // fraction of the parent particle's velocity the children start with
    f32 at = 0;               // Lifetime: the life fraction in (0,1) it fires at. Timer: the period in seconds
    u8  payload = 0;          // kEventPayload* bits
    f32 user = 0;             // user scalar: multiplies the children's speed when non-zero
};

// The rule is a TRIGGER / payload rule (not a plain death or collide rule): it is written as a TRIGGER record, and both tiers run it.
inline bool eventIsExtended(const EventSpawn& ev) { return ev.on > static_cast<u8>(EventOn::Collide) || ev.payload != 0 || ev.user != 0.0f; }

// A rule that can ever fire: death, collide and spawn always; a life fraction strictly inside (0,1); a timer period above a millisecond.
inline bool eventRuleFires(const EventSpawn& ev) {
    switch (static_cast<EventOn>(ev.on)) {
    case EventOn::Death: case EventOn::Collide: case EventOn::Spawn: return true;
    case EventOn::Lifetime: return ev.at > 0.0f && ev.at < 1.0f;
    case EventOn::Timer: return ev.at > 1e-3f;
    }
    return false;
}

// GPU event channels. An emitter's events buffer holds one list per channel: 0 death, 1 collide, then one per firing trigger rule
// (spawn / lifetime / timer) in rule order. A child consumes the channel of the rule that names it.
inline constexpr u32 kGpuTriggerChannels = 4;
inline constexpr u32 kGpuEventChannels = 2 + kGpuTriggerChannels;
inline constexpr int kNoEventChannel = -1;

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
    bool shapeShell = false;            // sphere / cylinder / disc / ring: spawn on the surface only
    Vec3 shapeAxis{0, 0, 1};            // ring: the axis it turns about (any length; a zero vector reads as +Z)
    Curve4 sizeCurve, colorCurve;       // override sizeStart/End and colorStart/End when n >= 2
    std::vector<Module> modules;        // authored order is execution order within a phase
    std::vector<EventSpawn> events;
    std::string name;                   // emitter name (events and the editor refer to it)
    // The name of a registered embedded particles program (setCustomParticlePrograms). Its start() runs when a particle is born and its process() on every
    // update, replacing the built-in integration; it needs the GPU tier (an emitter that lands on the CPU tier simulates without it). Empty = none, today's emitter.
    std::string customShader;
    Scalability scalability;            // this emitter's per-tier overrides of the effect's default (format v2: SCALE records)
};

// A whole authored effect: one or more emitters sharing user parameters.
struct ParticleSystemDef {
    std::string name;
    std::vector<ParticleEffect> emitters;
    std::vector<UserParam> params;
    Scalability scalability;            // the effect's default per tier (format v2: SCALEALL records)
};

// The channel rule `rule` of `fx` writes to, or kNoEventChannel when it never fires or is past the trigger table.
inline int gpuEventChannel(const ParticleEffect& fx, usize rule) {
    if (rule >= fx.events.size() || !eventRuleFires(fx.events[rule])) return kNoEventChannel;
    const u8 on = fx.events[rule].on;
    if (on <= static_cast<u8>(EventOn::Collide)) return static_cast<int>(on);
    u32 ordinal = 0;
    for (usize i = 0; i < rule; ++i)
        if (fx.events[i].on > static_cast<u8>(EventOn::Collide) && eventRuleFires(fx.events[i])) ++ordinal;
    return ordinal < kGpuTriggerChannels ? static_cast<int>(2 + ordinal) : kNoEventChannel;
}

// The only rules the GPU tier cannot run: more firing trigger rules on one emitter than there are trigger channels.
inline bool emitterEventsNeedCpu(const ParticleEffect& fx) {
    u32 triggers = 0;
    for (const EventSpawn& ev : fx.events)
        if (ev.on > static_cast<u8>(EventOn::Collide) && eventRuleFires(ev)) ++triggers;
    return triggers > kGpuTriggerChannels;
}

// Event-linked emitters share one tier, so one the GPU cannot run puts the whole system on the CPU: an emitter with more trigger rules than
// channels, or a child fed by more than one rule (a GPU child consumes the one channel of the first rule that names it).
inline bool systemEventsNeedCpu(const ParticleSystemDef& d) {
    std::vector<u32> feeders(d.emitters.size(), 0u);
    for (usize p = 0; p < d.emitters.size(); ++p) {
        if (emitterEventsNeedCpu(d.emitters[p])) return true;
        for (const EventSpawn& ev : d.emitters[p].events)
            if (eventRuleFires(ev) && ev.emitter < feeders.size() && ev.emitter != p && ++feeders[ev.emitter] > 1u) return true;
    }
    return false;
}

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
// spawnInfo bits 17..31: the particle's hue turn, 0..32767 for [0, 1) of a full turn (spawn.huevariation). 0 draws the colour as authored.
inline constexpr u32 kParticleHueShift = 17;
inline constexpr u32 kParticleHueMask = 0x7FFFu;
inline constexpr u32 kParticleHueSteps = 32768u;

inline bool particleAlive(const Particle& p) { return p.age < p.lifetime; }

} // namespace aver::particles
