// Fabric G6 SH3: Godot particles shaders as the GPU particle simulation's custom stage (docs/plans/fabric/G6_SHADERS.md, section SH3).
//
// Composed by aver::rhi::godotParticlesProgram into the prelude of the AVER_GD_PARTICLES variants of particles_sim.hlsl (CSSpawn runs start(), CSUpdate runs
// process()), ONLY for an emitter that carries a converted shader. Split at the source-marker line (the one with @GD_SOURCE@ in it, below): the head defines the state and the hook macros, the converted
// source goes in the middle, the tail wraps the entries. The simulation owns the buffers; this file sees one particle through GdPartIO, in Godot's space
// (metres, +Y up) and units, and the simulation converts at the boundary (particles_sim.hlsl, AVER_GD_PARTICLES).

struct GdPartIO {
    float4x4 xform;       // GLSL memory order: row i holds column i, row 3 the origin
    float3   velocity;
    float4   color;
    float4   custom;
    float    mass;
    bool     active;
    float    lifetime;
    float    delta;
    float    time;
    uint     number;
    uint     index;
    uint     seed;
    bool     restart;
    float4x4 emission;
};
static GdPartIO gGdPart;
// The uniform table: the particle system's user parameters (16 float4 slots), copied by the simulation before each call.
static float4 gGdPartU[16];
static float4 gGdPartG[16];

#define AVER_GD_TIME gGdPart.time
#define AVER_GD_LIFETIME gGdPart.lifetime
#define AVER_GD_DELTA gGdPart.delta
#define AVER_GD_INTERPOLATE_TO_END 0.0
#define AVER_GD_AMOUNT_RATIO 1.0
#define AVER_GD_NUMBER gGdPart.number
#define AVER_GD_INDEX gGdPart.index
#define AVER_GD_RANDOM_SEED gGdPart.seed
#define AVER_GD_FLAG_EMIT_POSITION 1u
#define AVER_GD_FLAG_EMIT_ROT_SCALE 2u
#define AVER_GD_FLAG_EMIT_VELOCITY 4u
#define AVER_GD_FLAG_EMIT_COLOR 8u
#define AVER_GD_FLAG_EMIT_CUSTOM 16u
#define AVER_GD_EMISSION_TRANSFORM gGdPart.emission
#define AVER_GD_EMITTER_VELOCITY float3(0.0, 0.0, 0.0)
#define AVER_GD_USERDATA1 float4(0.0, 0.0, 0.0, 0.0)
#define AVER_GD_USERDATA2 float4(0.0, 0.0, 0.0, 0.0)
#define AVER_GD_USERDATA3 float4(0.0, 0.0, 0.0, 0.0)
#define AVER_GD_USERDATA4 float4(0.0, 0.0, 0.0, 0.0)
#define AVER_GD_USERDATA5 float4(0.0, 0.0, 0.0, 0.0)
#define AVER_GD_USERDATA6 float4(0.0, 0.0, 0.0, 0.0)
#define AVER_GD_ACTIVE gGdPart.active
#define AVER_GD_COLOR gGdPart.color
#define AVER_GD_CUSTOM gGdPart.custom
#define AVER_GD_VELOCITY gGdPart.velocity
#define AVER_GD_TRANSFORM gGdPart.xform
#define AVER_GD_MASS gGdPart.mass
#define AVER_GD_RESTART_POSITION true
#define AVER_GD_RESTART_ROT_SCALE true
#define AVER_GD_RESTART_VELOCITY true
#define AVER_GD_RESTART_COLOR true
#define AVER_GD_RESTART_CUSTOM true
#define AVER_GD_RESTART gGdPart.restart
#define AVER_GD_COLLIDED false
#define AVER_GD_COLLISION_NORMAL float3(0.0, 0.0, 0.0)
#define AVER_GD_COLLISION_DEPTH 0.0
#define AVER_GD_ATTRACTOR_FORCE float3(0.0, 0.0, 0.0)
#define AVER_GD_UBUF gGdPartU
#define AVER_GD_GBUF gGdPartG
//@GD_SOURCE@
#undef AVER_GD_TIME
#undef AVER_GD_LIFETIME
#undef AVER_GD_DELTA
#undef AVER_GD_INTERPOLATE_TO_END
#undef AVER_GD_AMOUNT_RATIO
#undef AVER_GD_NUMBER
#undef AVER_GD_INDEX
#undef AVER_GD_RANDOM_SEED
#undef AVER_GD_FLAG_EMIT_POSITION
#undef AVER_GD_FLAG_EMIT_ROT_SCALE
#undef AVER_GD_FLAG_EMIT_VELOCITY
#undef AVER_GD_FLAG_EMIT_COLOR
#undef AVER_GD_FLAG_EMIT_CUSTOM
#undef AVER_GD_EMISSION_TRANSFORM
#undef AVER_GD_EMITTER_VELOCITY
#undef AVER_GD_USERDATA1
#undef AVER_GD_USERDATA2
#undef AVER_GD_USERDATA3
#undef AVER_GD_USERDATA4
#undef AVER_GD_USERDATA5
#undef AVER_GD_USERDATA6
#undef AVER_GD_ACTIVE
#undef AVER_GD_COLOR
#undef AVER_GD_CUSTOM
#undef AVER_GD_VELOCITY
#undef AVER_GD_TRANSFORM
#undef AVER_GD_MASS
#undef AVER_GD_RESTART_POSITION
#undef AVER_GD_RESTART_ROT_SCALE
#undef AVER_GD_RESTART_VELOCITY
#undef AVER_GD_RESTART_COLOR
#undef AVER_GD_RESTART_CUSTOM
#undef AVER_GD_RESTART
#undef AVER_GD_COLLIDED
#undef AVER_GD_COLLISION_NORMAL
#undef AVER_GD_COLLISION_DEPTH
#undef AVER_GD_ATTRACTOR_FORCE
#undef AVER_GD_UBUF
#undef AVER_GD_GBUF
#undef ddx
#undef ddy
#undef ddx_coarse
#undef ddy_coarse
#undef ddx_fine
#undef ddy_fine
#undef fwidth

// Godot's matrix memory order (row i = column i) <-> the engine's row-vector basis. Engine rows r0..r2 and a translation in centimetres.
float4x4 gdePartMatrixFromEngine(float3 r0, float3 r1, float3 r2, float3 originCm) {
    const float3 c0 = gdeE2G(r0 * gdeG2E(float3(1, 0, 0)).x + r1 * gdeG2E(float3(1, 0, 0)).y + r2 * gdeG2E(float3(1, 0, 0)).z);
    const float3 c1 = gdeE2G(r0 * gdeG2E(float3(0, 1, 0)).x + r1 * gdeG2E(float3(0, 1, 0)).y + r2 * gdeG2E(float3(0, 1, 0)).z);
    const float3 c2 = gdeE2G(r0 * gdeG2E(float3(0, 0, 1)).x + r1 * gdeG2E(float3(0, 0, 1)).y + r2 * gdeG2E(float3(0, 0, 1)).z);
    return float4x4(float4(c0, 0.0), float4(c1, 0.0), float4(c2, 0.0), float4(gdeE2G(originCm) * 0.01, 1.0));
}

// Runs start() (isStart) or process() on io. The simulation fills io (position / velocity / colour / custom / mass / transform) and the table gGdPartU / gGdPartG first.
#if AVER_GD_HAS_START
void averGdParticleStart(inout GdPartIO io) { gGdPart = io; AVER_GD_START_ENTRY(); io = gGdPart; }
#endif
#if AVER_GD_HAS_PROCESS
void averGdParticleProcess(inout GdPartIO io) { gGdPart = io; AVER_GD_PROCESS_ENTRY(); io = gGdPart; }
#endif
