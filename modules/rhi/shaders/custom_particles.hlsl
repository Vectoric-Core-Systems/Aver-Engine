// Custom particles programs as the GPU particle simulation's custom stage.
//
// Composed by aver::rhi::customParticlesProgram into the prelude of the AVER_CUSTOM_PARTICLES variants of particles_sim.hlsl (CSSpawn runs start(), CSUpdate runs
// process()), ONLY for an emitter that carries a registered program. Split at the source-marker line (the one with @CUSTOM_SOURCE@ in it, below): the head defines the state and the hook macros, the translated
// source goes in the middle, the tail wraps the entries. The simulation owns the buffers; this file sees one particle through CustomPartIO, in the program's own space
// (metres, +Y up) and units, and the simulation converts at the boundary (particles_sim.hlsl, AVER_CUSTOM_PARTICLES).

struct CustomPartIO {
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
static CustomPartIO gCustomPart;
// The uniform table: the particle system's user parameters (16 float4 slots), copied by the simulation before each call.
static float4 gCustomPartU[16];
static float4 gCustomPartG[16];

#define AVER_CUSTOM_TIME gCustomPart.time
#define AVER_CUSTOM_LIFETIME gCustomPart.lifetime
#define AVER_CUSTOM_DELTA gCustomPart.delta
#define AVER_CUSTOM_INTERPOLATE_TO_END 0.0
#define AVER_CUSTOM_AMOUNT_RATIO 1.0
#define AVER_CUSTOM_NUMBER gCustomPart.number
#define AVER_CUSTOM_INDEX gCustomPart.index
#define AVER_CUSTOM_RANDOM_SEED gCustomPart.seed
#define AVER_CUSTOM_FLAG_EMIT_POSITION 1u
#define AVER_CUSTOM_FLAG_EMIT_ROT_SCALE 2u
#define AVER_CUSTOM_FLAG_EMIT_VELOCITY 4u
#define AVER_CUSTOM_FLAG_EMIT_COLOR 8u
#define AVER_CUSTOM_FLAG_EMIT_CUSTOM 16u
#define AVER_CUSTOM_EMISSION_TRANSFORM gCustomPart.emission
#define AVER_CUSTOM_EMITTER_VELOCITY float3(0.0, 0.0, 0.0)
#define AVER_CUSTOM_USERDATA1 float4(0.0, 0.0, 0.0, 0.0)
#define AVER_CUSTOM_USERDATA2 float4(0.0, 0.0, 0.0, 0.0)
#define AVER_CUSTOM_USERDATA3 float4(0.0, 0.0, 0.0, 0.0)
#define AVER_CUSTOM_USERDATA4 float4(0.0, 0.0, 0.0, 0.0)
#define AVER_CUSTOM_USERDATA5 float4(0.0, 0.0, 0.0, 0.0)
#define AVER_CUSTOM_USERDATA6 float4(0.0, 0.0, 0.0, 0.0)
#define AVER_CUSTOM_ACTIVE gCustomPart.active
#define AVER_CUSTOM_COLOR gCustomPart.color
#define AVER_CUSTOM_CUSTOM gCustomPart.custom
#define AVER_CUSTOM_VELOCITY gCustomPart.velocity
#define AVER_CUSTOM_TRANSFORM gCustomPart.xform
#define AVER_CUSTOM_MASS gCustomPart.mass
#define AVER_CUSTOM_RESTART_POSITION true
#define AVER_CUSTOM_RESTART_ROT_SCALE true
#define AVER_CUSTOM_RESTART_VELOCITY true
#define AVER_CUSTOM_RESTART_COLOR true
#define AVER_CUSTOM_RESTART_CUSTOM true
#define AVER_CUSTOM_RESTART gCustomPart.restart
#define AVER_CUSTOM_COLLIDED false
#define AVER_CUSTOM_COLLISION_NORMAL float3(0.0, 0.0, 0.0)
#define AVER_CUSTOM_COLLISION_DEPTH 0.0
#define AVER_CUSTOM_ATTRACTOR_FORCE float3(0.0, 0.0, 0.0)
#define AVER_CUSTOM_UBUF gCustomPartU
#define AVER_CUSTOM_GBUF gCustomPartG
//@CUSTOM_SOURCE@
#undef AVER_CUSTOM_TIME
#undef AVER_CUSTOM_LIFETIME
#undef AVER_CUSTOM_DELTA
#undef AVER_CUSTOM_INTERPOLATE_TO_END
#undef AVER_CUSTOM_AMOUNT_RATIO
#undef AVER_CUSTOM_NUMBER
#undef AVER_CUSTOM_INDEX
#undef AVER_CUSTOM_RANDOM_SEED
#undef AVER_CUSTOM_FLAG_EMIT_POSITION
#undef AVER_CUSTOM_FLAG_EMIT_ROT_SCALE
#undef AVER_CUSTOM_FLAG_EMIT_VELOCITY
#undef AVER_CUSTOM_FLAG_EMIT_COLOR
#undef AVER_CUSTOM_FLAG_EMIT_CUSTOM
#undef AVER_CUSTOM_EMISSION_TRANSFORM
#undef AVER_CUSTOM_EMITTER_VELOCITY
#undef AVER_CUSTOM_USERDATA1
#undef AVER_CUSTOM_USERDATA2
#undef AVER_CUSTOM_USERDATA3
#undef AVER_CUSTOM_USERDATA4
#undef AVER_CUSTOM_USERDATA5
#undef AVER_CUSTOM_USERDATA6
#undef AVER_CUSTOM_ACTIVE
#undef AVER_CUSTOM_COLOR
#undef AVER_CUSTOM_CUSTOM
#undef AVER_CUSTOM_VELOCITY
#undef AVER_CUSTOM_TRANSFORM
#undef AVER_CUSTOM_MASS
#undef AVER_CUSTOM_RESTART_POSITION
#undef AVER_CUSTOM_RESTART_ROT_SCALE
#undef AVER_CUSTOM_RESTART_VELOCITY
#undef AVER_CUSTOM_RESTART_COLOR
#undef AVER_CUSTOM_RESTART_CUSTOM
#undef AVER_CUSTOM_RESTART
#undef AVER_CUSTOM_COLLIDED
#undef AVER_CUSTOM_COLLISION_NORMAL
#undef AVER_CUSTOM_COLLISION_DEPTH
#undef AVER_CUSTOM_ATTRACTOR_FORCE
#undef AVER_CUSTOM_UBUF
#undef AVER_CUSTOM_GBUF
#undef ddx
#undef ddy
#undef ddx_coarse
#undef ddy_coarse
#undef ddx_fine
#undef ddy_fine
#undef fwidth

// The program's matrix memory order (row i = column i) <-> the engine's row-vector basis. Engine rows r0..r2 and a translation in centimetres.
float4x4 customPartMatrixFromEngine(float3 r0, float3 r1, float3 r2, float3 originCm) {
    const float3 c0 = cenvE2S(r0 * cenvS2E(float3(1, 0, 0)).x + r1 * cenvS2E(float3(1, 0, 0)).y + r2 * cenvS2E(float3(1, 0, 0)).z);
    const float3 c1 = cenvE2S(r0 * cenvS2E(float3(0, 1, 0)).x + r1 * cenvS2E(float3(0, 1, 0)).y + r2 * cenvS2E(float3(0, 1, 0)).z);
    const float3 c2 = cenvE2S(r0 * cenvS2E(float3(0, 0, 1)).x + r1 * cenvS2E(float3(0, 0, 1)).y + r2 * cenvS2E(float3(0, 0, 1)).z);
    return float4x4(float4(c0, 0.0), float4(c1, 0.0), float4(c2, 0.0), float4(cenvE2S(originCm) * 0.01, 1.0));
}

// Runs start() (isStart) or process() on io. The simulation fills io (position / velocity / colour / custom / mass / transform) and the table gCustomPartU / gCustomPartG first.
#if AVER_CUSTOM_HAS_START
void averCustomParticleStart(inout CustomPartIO io) { gCustomPart = io; AVER_CUSTOM_START_ENTRY(); io = gCustomPart; }
#endif
#if AVER_CUSTOM_HAS_PROCESS
void averCustomParticleProcess(inout CustomPartIO io) { gCustomPart = io; AVER_CUSTOM_PROCESS_ENTRY(); io = gCustomPart; }
#endif
