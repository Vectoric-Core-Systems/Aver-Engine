// The data shared by every emitter that plays the same effect, and the per-particle simulation
// state a CPU or a future GPU tier updates every tick. No entity, no world, no RHI device -- this
// header is pure data, included by both halves of the module (ParticleSystem, which never touches
// the RHI, and ParticleRenderer, which never touches scene::World) and by anything else that just
// wants to describe or read an effect (a future .ocparticle reader/writer, an editor panel).
#pragma once
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/rhi/RHIResources.hpp"   // rhi::BlendMode -- reused wholesale, no new blend infrastructure

namespace aver::particles {

// Where a new particle is placed, relative to the emitter's world transform. Three shapes, not one
// per effect: a widening smoke column, falling snow and a waterfall's mist all read from the SAME
// three depending only on which one and what size is authored -- see ParticleEffect's own comment.
enum class EmitterShape : u32 {
    Point  = 0,   // every particle spawns at the emitter's origin
    Sphere = 1,   // uniformly inside a sphere of radius shapeSize.x
    Box    = 2,   // uniformly inside a box of half-extents shapeSize
};

// One authored effect: everything about how particles are born, move and fade, shared by every
// emitter that plays it (DECIDED 3 -- effects are shared and edited once; an emitter only ever
// carries an id, see scene::CParticleEmitter). This is the whole parameter space this slice
// supports, and it is deliberately the SMALL ORTHOGONAL SET the brief asks for: emission shape and
// rate, lifetime, initial velocity and spread, gravity, damping, size and colour over life, blend
// mode, texture. Nothing here names what the particles ARE -- there is no "smoke" or "fire" field,
// only numbers. A rising smoke column, falling snow, a burst of embers and a waterfall's mist are
// four different VALUES of this same struct, never four code paths; see ParticleSystem.cpp's own
// comment on why that is exactly what makes it engine machinery and not a weapon-effects system.
struct ParticleEffect {
    EmitterShape shape = EmitterShape::Point;
    Vec3 shapeSize{0, 0, 0};   // Sphere: radius in .x. Box: half-extents. Point: unused.

    f32 emissionRate = 10.0f;   // particles/second, continuous, while playing. 0 = none (burst-only).
    u32 burstCount   = 0;       // spawned once on the stopped -> playing edge, on top of the rate.
    u32 maxParticles = 512;     // this effect's live-particle cap, PER EMITTER.

    f32 lifetimeMin = 1.0f, lifetimeMax = 1.0f;   // seconds, drawn per particle at spawn.

    Vec3 direction = {0, 0, 1};   // base emission direction; normalised on use.
    f32  spreadDeg = 0.0f;        // half-angle of the cone around `direction`, in degrees. 0 = exact
                                   // direction, 180 = a full sphere (fire's updraft vs. an omni burst).
    f32  speedMin = 100.0f, speedMax = 100.0f;   // cm/s, initial speed range.

    // DECIDED: constant gravity and velocity damping only -- no Jolt dependency of any kind, and
    // AVER_MODULE_PHYSICS=OFF must build and behave identically. A particle never queries the physics
    // world; it is not a rigid body.
    Vec3 gravity = {0, 0, 0};   // cm/s^2, constant and uniform.
    f32  damping = 0.0f;        // fraction of velocity removed per second, [0, 1).

    f32 sizeStart = 10.0f, sizeEnd = 10.0f;   // billboard edge length, centimetres.
    f32 colorStart[4] = {1, 1, 1, 1};           // straight (non-premultiplied) rgba, birth.
    f32 colorEnd[4]   = {1, 1, 1, 0};           // death.

    // See ParticleRenderer.hpp's own comment on pipelineFor(): the shader always premultiplies rgb
    // by alpha, so of rhi::BlendMode's four values only PremultipliedAlpha and Additive compose
    // correctly against it. Both are accepted as data regardless -- this is DATA, not a switch this
    // module branches on -- but Opaque/AlphaBlend currently render through the premultiplied pipeline.
    rhi::BlendMode blend = rhi::BlendMode::PremultipliedAlpha;

    // .ocparticle-adjacent texture reference (opaque; same idiom as scene::CMeshRenderer::mesh --
    // resolved by a tier above, never loaded here). UNUSED THIS SLICE: no asset reader writes it yet
    // (DECIDED 3 is a later slice), so ParticleRenderer always draws the same procedural soft-edged
    // dot regardless of this value -- see ParticleShaders.hpp. The field exists so that reader only
    // has to WRITE it, not invent where a texture id lives on the effect.
    u64 textureId = 0;

    // DECIDED 4: particles sample the Voxi GI volume so they sit in the scene instead of reading as
    // pasted-on -- but a particle that IS its own light source (an ember, a spark) must not be
    // dimmed by the ambient bounce light around it, so this is DATA on the effect, not a code path.
    // true (the default) means ParticleRenderer multiplies this effect's colour by the sampled GI
    // term whenever a GI seam is installed (see ParticleRenderer::setGiSeam); false means it never
    // does, regardless of whether a seam is installed -- the effect always draws at its own authored
    // colour, full bright, the same as every effect drew before this seam existed. With NO seam
    // installed at all (AVER_MODULE_VOXI=OFF, or Voxi simply never wired up this run) this field is
    // read but has no visible effect either way: every particle is unlit already.
    bool receivesGI = true;
};

// One simulated particle. LAID OUT FOR A FUTURE GPU COMPUTE TIER (DECIDED 2): this slice updates the
// array on the CPU every tick, but the fields are exactly what a StructuredBuffer<Particle> compute
// kernel would read and write in place, so swapping the update loop for a dispatch later touches
// neither scene::CParticleEmitter nor ParticleEffect above.
//
// WHICH OF modules/render.pcg's CHOICES A GPU TIER WOULD FOLLOW (see PcgVolume.cpp): the BUFFER
// SHAPE -- a BufferKind::Default buffer with allowUnorderedAccess, bound as a StructuredBuffer UAV
// the way PcgVolume.cpp's `out_`/`set_` are -- and passing the small per-dispatch parameter block as
// ROOT CONSTANTS rather than a second buffer, the way VolumeCB reaches CSVolume via
// `ctx.setConstants(0, ...)` instead of its own cbuffer resource.
//
// WHICH IT WOULD NOT: PcgVolume's buffer is written ONCE per request and read back to the CPU three
// frames later (VolumeBuilder::read, gated on kReadbackFrames) -- a build-once tool whose result the
// CPU consumes. A particle buffer is written AND read every frame (simulate, then draw), so it would
// stay entirely GPU-resident and be read directly by the vertex shader through an SRV -- no
// BufferKind::Readback, no copyBuffer, no multi-frame delay anywhere in that loop.
struct Particle {
    Vec3 position;   // world space, centimetres
    f32  age;        // seconds since spawn
    Vec3 velocity;   // world space, cm/s
    f32  lifetime;   // total seconds this particle lives, drawn once at spawn
    // Per-particle RNG stream, drawn once at spawn. The CPU tier never reads this back -- it already
    // drew velocity/lifetime with its own aver::particles RNG -- but a GPU spawn kernel has no CPU
    // generator to call, and would hash THIS field the way PcgVolume.cpp's hash32/hash3 hash a cell
    // coordinate, to regenerate the same spread/lifetime distribution from inside a compute thread.
    u32  seed;
    f32  _pad[3];    // rounds the struct to 48 bytes / three float4s: a StructuredBuffer stride with
                      // no cross-language packing surprises, matching VoxiRenderer::RtInstance's own
                      // "the stride handed to setSrvBuffer must agree with it" reasoning.
};
static_assert(sizeof(Particle) == 48, "Particle must stay a StructuredBuffer-friendly 3x float4");

} // namespace aver::particles
