#pragma once
// Aver.Physics: strand hair on Jolt 5.6's Hair. Additive; no existing header changes.
// Units and axes are the engine's (cm, cm/s, +X forward, +Y right, +Z up). Hair space is the scalp mesh's model space.
// Exception: aver_phys_hair_render_buffer is float3 in hair-local Jolt space (metres, Y up, right-handed).
// Functions return 1 on success, 0 on refusal (aver_phys_last_error says why), unless they return a handle or count.
#include <stdint.h>

#if defined(_WIN32)
#  if defined(AVER_PHYS_BUILD)
#    define AVER_PHYS_API __declspec(dllexport)
#  else
#    define AVER_PHYS_API __declspec(dllimport)
#  endif
#else
#  define AVER_PHYS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_PHYS_HAIR_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_hair_abi_version(void);

// ---- The compute host ----
// Buffer types: UPLOAD is ringed by the host; READBACK may map to NULL on a GPU host; CONSTANT is snapshotted at bind; BUFFER must carry data.
#define AVER_PHYS_CB_UPLOAD    0
#define AVER_PHYS_CB_READBACK  1
#define AVER_PHYS_CB_CONSTANT  2
#define AVER_PHYS_CB_BUFFER    3
#define AVER_PHYS_CB_RW        4
// A bind role.
#define AVER_PHYS_BIND_CONSTANT 0
#define AVER_PHYS_BIND_READ     1
#define AVER_PHYS_BIND_RW       2

typedef uint64_t AverPhysHostHandle;     /* opaque, chosen by the host; 0 is failure */

typedef struct AverPhysComputeHostV1 {
    uint32_t structSize;                 /* sizeof(AverPhysComputeHostV1), checked */
    uint32_t version;                    /* 1 */
    void*    user;                       /* passed back as the first argument of every callback */
    /* name is one of Jolt's fifteen ("HairIntegrate", ...) or "AverHairWind"; an unknown name returns 0 (the wind kernel is then off). */
    AverPhysHostHandle (*create_shader)(void* user, const char* name, uint32_t gx, uint32_t gy, uint32_t gz);
    /* count elements of stride bytes; data is null or count * stride bytes (required for CB_BUFFER). */
    AverPhysHostHandle (*create_buffer)(void* user, int32_t type, uint64_t count, uint32_t stride, const void* data);
    void*              (*map)(void* user, AverPhysHostHandle buffer, int32_t write);
    void               (*unmap)(void* user, AverPhysHostHandle buffer);
    void               (*release)(void* user, AverPhysHostHandle object);          /* shader, buffer or queue */
    AverPhysHostHandle (*create_queue)(void* user);
    void (*set_shader)(void* user, AverPhysHostHandle queue, AverPhysHostHandle shader);
    /* name is Jolt's binding name ("gPositions"); the host resolves the register. barrier 1 = earlier writes visible, BIND_RW only. */
    void (*bind)(void* user, AverPhysHostHandle queue, const char* name, AverPhysHostHandle buffer, int32_t role, int32_t barrier);
    void (*dispatch)(void* user, AverPhysHostHandle queue, uint32_t gx, uint32_t gy, uint32_t gz);
    void (*readback)(void* user, AverPhysHostHandle queue, AverPhysHostHandle dst, AverPhysHostHandle src);
    void (*execute)(void* user, AverPhysHostHandle queue);
    void (*wait)(void* user, AverPhysHostHandle queue);
} AverPhysComputeHostV1;

// Installs the host (copied). NULL = Jolt's CPU backend. Refused while any settings object exists: install before the first settings_finish.
AVER_PHYS_API int32_t aver_phys_hair_set_compute_host(const AverPhysComputeHostV1* host);
// 0 = Jolt's CPU backend, 1 = an installed host.
AVER_PHYS_API int32_t aver_phys_hair_host_kind(void);

// ---- Parameter blocks ----
// NaN = leave as is; a shorter block keeps the defaults of the rest; a longer block is refused.
#define AVER_PHYS_HAIRP_ITERATIONS_PER_SECOND 0
#define AVER_PHYS_HAIRP_MAX_DELTA_TIME        1   /* s; Jolt clamps the frame time to it, so time slows below 30 fps */
#define AVER_PHYS_HAIRP_GRID_X                2
#define AVER_PHYS_HAIRP_GRID_Y                3
#define AVER_PHYS_HAIRP_GRID_Z                4
#define AVER_PHYS_HAIRP_BOUNDS_PADDING_CM     5   /* added on every side of the neutral-pose bounds */
#define AVER_PHYS_HAIRP_SIM_FRACTION          6
#define AVER_PHYS_HAIRP_INITIAL_GRAVITY_X     7   /* cm/s^2, engine axes: the gravity the modelled pose already carries */
#define AVER_PHYS_HAIRP_INITIAL_GRAVITY_Y     8
#define AVER_PHYS_HAIRP_INITIAL_GRAVITY_Z     9
#define AVER_PHYS_HAIRP_COUNT                 10

// Per material. Gradients are four floats (min, max, minFraction, maxFraction): min at minFraction along the strand, max at
// maxFraction, clamped outside; the fractions must differ. Units: damping 1/s, max linear cm/s, max angular rad/s, radius cm.
// Gradient slots: BEND_MULTIPLIER 12..15 (0/33/66/100% of the strand), GRAVITY 6..9, RADIUS_CM 18..21, WORLD_INFLUENCE 22..25 (how much
// turning the head moves the hair), GRID_VELOCITY 26..29, GLOBAL_POSE 31..34 (pull towards the modelled pose), SKIN_GLOBAL_POSE 35..38.
#define AVER_PHYS_HAIRM_ENABLE_COLLISION      0
#define AVER_PHYS_HAIRM_ENABLE_LRA            1   /* long range attachments: keep strands near the modelled pose */
#define AVER_PHYS_HAIRM_LINEAR_DAMPING        2
#define AVER_PHYS_HAIRM_ANGULAR_DAMPING       3
#define AVER_PHYS_HAIRM_MAX_LINEAR_CMS        4
#define AVER_PHYS_HAIRM_MAX_ANGULAR_RADS      5
#define AVER_PHYS_HAIRM_GRAVITY               6
#define AVER_PHYS_HAIRM_FRICTION              10
#define AVER_PHYS_HAIRM_BEND_COMPLIANCE       11
#define AVER_PHYS_HAIRM_BEND_MULTIPLIER       12
#define AVER_PHYS_HAIRM_STRETCH_COMPLIANCE    16
#define AVER_PHYS_HAIRM_INERTIA_MULTIPLIER    17
#define AVER_PHYS_HAIRM_RADIUS_CM             18
#define AVER_PHYS_HAIRM_WORLD_INFLUENCE       22
#define AVER_PHYS_HAIRM_GRID_VELOCITY         26
#define AVER_PHYS_HAIRM_GRID_DENSITY_FORCE    30
#define AVER_PHYS_HAIRM_GLOBAL_POSE           31
#define AVER_PHYS_HAIRM_SKIN_GLOBAL_POSE      35
#define AVER_PHYS_HAIRM_SIM_FRACTION          39
#define AVER_PHYS_HAIRM_GRAVITY_PRELOAD       40
#define AVER_PHYS_HAIRM_COUNT                 41

// Settings info (aver_phys_hair_settings_info): ints, then floats.
#define AVER_PHYS_HAIRI_SIM_STRANDS           0
#define AVER_PHYS_HAIRI_SIM_VERTICES          1
#define AVER_PHYS_HAIRI_RENDER_STRANDS        2
#define AVER_PHYS_HAIRI_RENDER_VERTICES       3
#define AVER_PHYS_HAIRI_MAX_VERTICES_PER_STRAND 4
#define AVER_PHYS_HAIRI_MATERIALS             5
#define AVER_PHYS_HAIRI_GRID_CELLS            6
#define AVER_PHYS_HAIRI_SCALP_JOINTS          7
#define AVER_PHYS_HAIRI_COUNT                 8
#define AVER_PHYS_HAIRF_ROOT_TO_SCALP_CM      0
#define AVER_PHYS_HAIRF_BOUNDS_MIN_X          1   /* simulation bounds, cm, hair space, engine axes: 1..3 min, 4..6 max */
#define AVER_PHYS_HAIRF_BOUNDS_MIN_Y          2
#define AVER_PHYS_HAIRF_BOUNDS_MIN_Z          3
#define AVER_PHYS_HAIRF_BOUNDS_MAX_X          4
#define AVER_PHYS_HAIRF_BOUNDS_MAX_Y          5
#define AVER_PHYS_HAIRF_BOUNDS_MAX_Z          6
#define AVER_PHYS_HAIRF_COUNT                 7

// Instance info (aver_phys_hair_info), about the last aver_phys_hair_update.
#define AVER_PHYS_HAIRUI_ITERATIONS           0
#define AVER_PHYS_HAIRUI_DISPATCHES           1
#define AVER_PHYS_HAIRUI_UPDATES              2
#define AVER_PHYS_HAIRUI_WIND                 3
#define AVER_PHYS_HAIRUI_COUNT                4
#define AVER_PHYS_HAIRUF_DT_USED              0
#define AVER_PHYS_HAIRUF_COUNT                1

// ---- Settings (shared by instances) ----
// A builder id. Stage strands, scalp, materials and params in any order, then finish. A cancelled or finished id is dead.
AVER_PHYS_API int32_t aver_phys_hair_settings_begin(void);

// Render strands: pointsXyz in cm, scalp-mesh space. strandStart has strandCount + 1 entries, strictly increasing by at least 3
// (3..255 vertices per strand). The first vertex of each strand is always pinned. Repeated calls replace the earlier strands.
AVER_PHYS_API int32_t aver_phys_hair_settings_strands(int32_t s, const float* pointsXyz, const float* invMass, int32_t vertexCount,
                                                      const int32_t* strandStart, const int32_t* strandMaterial, int32_t strandCount);

// The scalp: a triangle mesh the roots attach to, skinned by joints indexing the palette list. Every root must reach a triangle.
AVER_PHYS_API int32_t aver_phys_hair_settings_scalp(int32_t s, const float* vertsXyz, int32_t vertexCount, const int32_t* tri3, int32_t triCount,
                                                    const int32_t* joints4, const float* weights4, int32_t jointCount);

// A material's parameters. A material index used by a strand and never described keeps the defaults.
AVER_PHYS_API int32_t aver_phys_hair_settings_material(int32_t s, int32_t index, const float* p, int32_t n);
// The settings' parameters.
AVER_PHYS_API int32_t aver_phys_hair_settings_params(int32_t s, const float* p, int32_t n);

// Splits strands into render and simulation strands, resamples to resampleVertices (0 = none, else 3..255) and creates the compute
// buffers. Returns the SETTINGS handle (> 0), 0 on refusal; the builder id is consumed either way. outRootToScalpCm may be null.
// The split is quadratic in render times simulated strands (UNVERIFIED: 0.2 to 1 s for 20k strands): call it on a worker.
AVER_PHYS_API int32_t aver_phys_hair_settings_finish(int32_t s, int32_t resampleVertices, float* outRootToScalpCm);
AVER_PHYS_API int32_t aver_phys_hair_settings_cancel(int32_t s);
// Refused while an instance uses the settings.
AVER_PHYS_API int32_t aver_phys_hair_settings_destroy(int32_t settings);
// Live: re-read by every update. The modelled pose, the strand split and anything baked at finish are not.
AVER_PHYS_API int32_t aver_phys_hair_settings_set_material(int32_t settings, int32_t index, const float* p, int32_t n);
AVER_PHYS_API int32_t aver_phys_hair_settings_info(int32_t settings, int32_t* outI, int32_t nI, float* outF, int32_t nF);
AVER_PHYS_API int32_t aver_phys_hair_settings_render_strands(int32_t settings, int32_t* outStartCount2, int32_t maxStrands);

// ---- Instances ----
// world16: hair space to world, row-major, row-vector convention; a scale in it is stripped. layer 0..15.
AVER_PHYS_API int32_t aver_phys_hair_create(int32_t settings, const float* world16, int32_t layer);
AVER_PHYS_API int32_t aver_phys_hair_destroy(int32_t hair);
AVER_PHYS_API int32_t aver_phys_hair_set_transform(int32_t hair, const float* world16);
AVER_PHYS_API int32_t aver_phys_hair_teleport(int32_t hair);
// Skinning matrices (model x inverse bind, row-major, 16 floats each) in scalp joint list order. Identity until the first call.
AVER_PHYS_API int32_t aver_phys_hair_set_palette(int32_t hair, const float* joints16, int32_t jointCount);
// World wind in cm/s, drag in 1/s, turbulence in cm/s. Zero wind or zero drag records no kernel.
AVER_PHYS_API int32_t aver_phys_hair_set_wind(int32_t hair, const float* windCmS3, float dragPerS, float turbulenceCmS);
// Records one step (wind kernel, then Jolt's Hair::Update) into the shared queue. dt in seconds, clamped to MAX_DELTA_TIME.
AVER_PHYS_API int32_t aver_phys_hair_update(int32_t hair, float dt);
// Executes and waits for everything recorded this tick. Once per tick, after all updates.
AVER_PHYS_API int32_t aver_phys_hair_flush(void);
// Host handle of the render positions buffer; 0 for an unknown hair or the CPU backend.
AVER_PHYS_API uint64_t aver_phys_hair_render_buffer(int32_t hair);
AVER_PHYS_API int32_t aver_phys_hair_info(int32_t hair, int32_t* outI, int32_t nI, float* outF, int32_t nF);
// Debug and tests: reads simulated (cm, hair space, engine axes) and render positions back. Slow: waits for the queue.
AVER_PHYS_API int32_t aver_phys_hair_read_positions(int32_t hair, float* outSimXyz, int32_t maxSim, float* outRenderXyz, int32_t maxRender);

#ifdef __cplusplus
}
#endif
