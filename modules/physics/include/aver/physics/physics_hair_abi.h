#pragma once
// Aver.Physics -- STRAND HAIR on Jolt 5.6's Hair (wave 11). docs/plans/wave11/SOFTBODY_HAIR.md section 2, docs/HAIR.md.
//
// ADDITIVE. Nothing here changes an existing header. Jolt's Hair, HairSettings and HairShaders live inside Aver.Physics (so the
// module still links Jolt PRIVATE and depends on Core only); the compute work they record goes through a plain-C function table
// the HOST installs (AverPhysComputeHostV1, below) or, with none installed, through Jolt's CPU compute backend. The CPU backend
// runs the very same HLSL as C++ on the calling thread: it is the reference for the tests, a headless server's option, and slow.
//
// UNITS AND AXES are the engine's everywhere: centimetres, cm/s, +X forward, +Y right, +Z up, left-handed. Hair space is the space
// of the scalp mesh (the owning entity's model space). The one exception is the render positions buffer a host reads
// (aver_phys_hair_render_buffer): float3 per render vertex in hair-local JOLT space (metres, Y up, right-handed), the conversion
// to engine axes being the ribbon pass' job (positions (x, y, z)_Jolt are (-z, x, y) * 100 in the engine).
//
// Types are int32_t, uint64_t, float, const float* and const int32_t* only. Every function returns 1 on success and 0 on refusal
// (aver_phys_last_error says why) unless it says it returns a handle or a count.
//
// CALL FROM THE MAIN THREAD BETWEEN aver_phys_step CALLS (hair reads the broad phase), never from a step listener or a contact
// callback. aver_phys_hair_settings_finish may take a second or so for a big groom and may run on a worker thread while the world
// is not stepping; every other call is cheap. A hair needs a world: aver_phys_init first. aver_phys_shutdown destroys every
// settings object and hair and forgets the installed host.
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

// ---- The compute host (section 2.10) --------------------------------------------------------------------------------------
// A buffer type for create_buffer.
#define AVER_PHYS_CB_UPLOAD    0   /* CPU writes, GPU reads; map(write) discards; ringed by the host */
#define AVER_PHYS_CB_READBACK  1   /* GPU writes, CPU reads (debug; a GPU host may return NULL from map) */
#define AVER_PHYS_CB_CONSTANT  2   /* small; the host snapshots it at bind time */
#define AVER_PHYS_CB_BUFFER    3   /* read-only after creation, must carry data */
#define AVER_PHYS_CB_RW        4   /* read/write on the GPU, optional initial data */
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
    /* name is Jolt's binding name ("gPositions"); the host resolves the register for the shader set last. barrier is 1 when the
       caller wants all earlier writes to the buffer visible (Jolt's EBarrier::Yes), only meaningful for BIND_RW. */
    void (*bind)(void* user, AverPhysHostHandle queue, const char* name, AverPhysHostHandle buffer, int32_t role, int32_t barrier);
    void (*dispatch)(void* user, AverPhysHostHandle queue, uint32_t gx, uint32_t gy, uint32_t gz);
    void (*readback)(void* user, AverPhysHostHandle queue, AverPhysHostHandle dst, AverPhysHostHandle src);
    void (*execute)(void* user, AverPhysHostHandle queue);   /* seal what was recorded */
    void (*wait)(void* user, AverPhysHostHandle queue);      /* a GPU host returns at once; the CPU host has already run it */
} AverPhysComputeHostV1;

// Installs the host (copied). NULL = Jolt's CPU backend. Refused (0) while any settings object exists, so install before the first
// aver_phys_hair_settings_finish; aver_phys_shutdown resets it. A structSize or version this build does not know is refused.
AVER_PHYS_API int32_t aver_phys_hair_set_compute_host(const AverPhysComputeHostV1* host);
// 0 = Jolt's CPU backend, 1 = an installed host.
AVER_PHYS_API int32_t aver_phys_hair_host_kind(void);

// ---- Parameter blocks (engine units; NaN = leave as is, a block shorter than COUNT keeps the defaults of the rest, longer is refused) ----
// Settings (aver_phys_hair_settings_params). Defaults in brackets.
#define AVER_PHYS_HAIRP_ITERATIONS_PER_SECOND 0   /* [360] */
#define AVER_PHYS_HAIRP_MAX_DELTA_TIME        1   /* [0.0333] s; Jolt clamps the frame time to it, so time slows below 30 fps */
#define AVER_PHYS_HAIRP_GRID_X                2   /* [32] velocity grid cells */
#define AVER_PHYS_HAIRP_GRID_Y                3   /* [32] */
#define AVER_PHYS_HAIRP_GRID_Z                4   /* [32] */
#define AVER_PHYS_HAIRP_BOUNDS_PADDING_CM     5   /* [10] added on every side of the neutral-pose bounds */
#define AVER_PHYS_HAIRP_SIM_FRACTION          6   /* [0.1] default of a material's simulated strand fraction */
#define AVER_PHYS_HAIRP_INITIAL_GRAVITY_X     7   /* [0] cm/s^2, engine axes: the gravity the modelled pose already carries */
#define AVER_PHYS_HAIRP_INITIAL_GRAVITY_Y     8   /* [0] */
#define AVER_PHYS_HAIRP_INITIAL_GRAVITY_Z     9   /* [-981] */
#define AVER_PHYS_HAIRP_COUNT                 10

// Per material (aver_phys_hair_settings_material and _set_material). A gradient is four floats: min, max, minFraction, maxFraction
// (the value is min at minFraction along the strand, max at maxFraction, clamped outside; the two fractions must differ).
#define AVER_PHYS_HAIRM_ENABLE_COLLISION      0   /* [1] */
#define AVER_PHYS_HAIRM_ENABLE_LRA            1   /* [1] long range attachments: keep strands near the modelled pose */
#define AVER_PHYS_HAIRM_LINEAR_DAMPING        2   /* [2] 1/s */
#define AVER_PHYS_HAIRM_ANGULAR_DAMPING       3   /* [2] 1/s */
#define AVER_PHYS_HAIRM_MAX_LINEAR_CMS        4   /* [1000] cm/s */
#define AVER_PHYS_HAIRM_MAX_ANGULAR_RADS      5   /* [50] rad/s */
#define AVER_PHYS_HAIRM_GRAVITY               6   /* gradient [0.1 1 0.2 0.8]: 6,7,8,9 */
#define AVER_PHYS_HAIRM_FRICTION              10  /* [0.2] */
#define AVER_PHYS_HAIRM_BEND_COMPLIANCE       11  /* [1e-7] */
#define AVER_PHYS_HAIRM_BEND_MULTIPLIER       12  /* 4 floats at 0%, 33%, 66%, 100% of the strand [1 100 100 1]: 12,13,14,15 */
#define AVER_PHYS_HAIRM_STRETCH_COMPLIANCE    16  /* [1e-8] */
#define AVER_PHYS_HAIRM_INERTIA_MULTIPLIER    17  /* [10] */
#define AVER_PHYS_HAIRM_RADIUS_CM             18  /* gradient [0.1 0.1 0 1]: 18,19,20,21 (collision radius, cm) */
#define AVER_PHYS_HAIRM_WORLD_INFLUENCE       22  /* gradient [0 1 0 1]: 22,23,24,25 how much turning the head moves the hair */
#define AVER_PHYS_HAIRM_GRID_VELOCITY         26  /* gradient [0.05 0.01 0 1]: 26,27,28,29 */
#define AVER_PHYS_HAIRM_GRID_DENSITY_FORCE    30  /* [0] */
#define AVER_PHYS_HAIRM_GLOBAL_POSE           31  /* gradient [0.01 0 0 0.3]: 31,32,33,34 pull towards the modelled pose */
#define AVER_PHYS_HAIRM_SKIN_GLOBAL_POSE      35  /* gradient [1 0 0 0.1]: 35,36,37,38 */
#define AVER_PHYS_HAIRM_SIM_FRACTION          39  /* [the settings default] fraction of the material's strands that are simulated */
#define AVER_PHYS_HAIRM_GRAVITY_PRELOAD       40  /* [0] */
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
#define AVER_PHYS_HAIRF_ROOT_TO_SCALP_CM      0   /* the largest root-to-scalp distance */
#define AVER_PHYS_HAIRF_BOUNDS_MIN_X          1   /* simulation bounds, cm, hair space, engine axes: 1..3 min, 4..6 max */
#define AVER_PHYS_HAIRF_BOUNDS_MIN_Y          2
#define AVER_PHYS_HAIRF_BOUNDS_MIN_Z          3
#define AVER_PHYS_HAIRF_BOUNDS_MAX_X          4
#define AVER_PHYS_HAIRF_BOUNDS_MAX_Y          5
#define AVER_PHYS_HAIRF_BOUNDS_MAX_Z          6
#define AVER_PHYS_HAIRF_COUNT                 7

// Instance info (aver_phys_hair_info), about the last aver_phys_hair_update.
#define AVER_PHYS_HAIRUI_ITERATIONS           0   /* solver iterations the update recorded (0 when dt rounded to none) */
#define AVER_PHYS_HAIRUI_DISPATCHES           1   /* compute dispatches the update recorded, wind included */
#define AVER_PHYS_HAIRUI_UPDATES              2   /* updates since create */
#define AVER_PHYS_HAIRUI_WIND                 3   /* 1 when the update recorded the wind kernel */
#define AVER_PHYS_HAIRUI_COUNT                4
#define AVER_PHYS_HAIRUF_DT_USED              0   /* seconds after Jolt's clamp */
#define AVER_PHYS_HAIRUF_COUNT                1

// ---- Settings (shared by instances) ------------------------------------------------------------------------------------------
// A builder id. Stage strands, scalp, materials and params in any order, then finish. A cancelled or finished builder id is dead.
AVER_PHYS_API int32_t aver_phys_hair_settings_begin(void);

// Render strands. pointsXyz: cm, scalp-mesh space, engine axes, 3 floats per vertex. strandStart has strandCount + 1 entries (first
// 0, last vertexCount, strictly increasing by at least 3: a strand has 3..255 vertices and no two neighbours coincide).
// strandMaterial: 0..255 per strand (null = all 0). invMass: per vertex or null (1 each; 0 pins; the first vertex of every strand is
// always pinned to the scalp whatever is given). Repeated calls replace the earlier strands.
AVER_PHYS_API int32_t aver_phys_hair_settings_strands(int32_t s, const float* pointsXyz, const float* invMass, int32_t vertexCount,
                                                      const int32_t* strandStart, const int32_t* strandMaterial, int32_t strandCount);

// The scalp: a triangle mesh the roots attach to, skinned by up to 4 joints per vertex. joints4 / weights4: 4 per vertex, a joint is
// an index into the SCALP JOINT LIST (0..jointCount-1), the list aver_phys_hair_set_palette fills in the same order (weights are
// renormalised; both null = a single joint 0). jointCount 0 is treated as 1. Every root must be within reach of a triangle
// (finish reports the largest distance). Repeated calls replace the scalp.
AVER_PHYS_API int32_t aver_phys_hair_settings_scalp(int32_t s, const float* vertsXyz, int32_t vertexCount, const int32_t* tri3, int32_t triCount,
                                                    const int32_t* joints4, const float* weights4, int32_t jointCount);

// A material's parameters (AVER_PHYS_HAIRM_*, n <= AVER_PHYS_HAIRM_COUNT). A material index used by a strand and never described
// keeps the defaults.
AVER_PHYS_API int32_t aver_phys_hair_settings_material(int32_t s, int32_t index, const float* p, int32_t n);
// The settings' parameters (AVER_PHYS_HAIRP_*, n <= AVER_PHYS_HAIRP_COUNT).
AVER_PHYS_API int32_t aver_phys_hair_settings_params(int32_t s, const float* p, int32_t n);

// Splits the strands into render and simulation strands, optionally resamples every strand to resampleVertices (0 = none, else
// 3..255), initialises and creates the compute buffers on the installed host. Returns the SETTINGS handle (> 0), 0 on refusal.
// outRootToScalpCm (may be null) receives the largest root-to-scalp distance. The builder id is consumed whatever the outcome.
// The split is quadratic in render strands times simulated strands (UNVERIFIED: 0.2 to 1 s for 20k strands): call on a worker.
AVER_PHYS_API int32_t aver_phys_hair_settings_finish(int32_t s, int32_t resampleVertices, float* outRootToScalpCm);
AVER_PHYS_API int32_t aver_phys_hair_settings_cancel(int32_t s);
// Refused while an instance uses the settings.
AVER_PHYS_API int32_t aver_phys_hair_settings_destroy(int32_t settings);
// Live: re-read by every update (the modelled pose, the strand split and anything baked at finish are not).
AVER_PHYS_API int32_t aver_phys_hair_settings_set_material(int32_t settings, int32_t index, const float* p, int32_t n);
// AVER_PHYS_HAIRI_* ints and AVER_PHYS_HAIRF_* floats; either array may be null.
AVER_PHYS_API int32_t aver_phys_hair_settings_info(int32_t settings, int32_t* outI, int32_t nI, float* outF, int32_t nF);
// The render strand table, in the order the render positions buffer holds the strands: {first render vertex, vertex count} per strand.
// Writes up to maxStrands pairs (null = count only) and returns the number of render strands.
AVER_PHYS_API int32_t aver_phys_hair_settings_render_strands(int32_t settings, int32_t* outStartCount2, int32_t maxStrands);

// ---- Instances ---------------------------------------------------------------------------------------------------------------
// world16: the hair space to world matrix, row-major, the engine's row-vector convention (the head entity's world); a scale in it is
// stripped. layer: user collision layer 0..15 (the layer hair collides as). Returns the hair handle, 0 on refusal. The hair is
// teleported (rest pose, zero velocity) by its first update.
AVER_PHYS_API int32_t aver_phys_hair_create(int32_t settings, const float* world16, int32_t layer);
AVER_PHYS_API int32_t aver_phys_hair_destroy(int32_t hair);
AVER_PHYS_API int32_t aver_phys_hair_set_transform(int32_t hair, const float* world16);
// Default pose and zero velocity at the next update, wherever the hair now is.
AVER_PHYS_API int32_t aver_phys_hair_teleport(int32_t hair);
// The engine's skinning matrices (model x inverse bind, row-major, engine convention, 16 floats each) of the scalp joints, in scalp
// joint list order. jointCount must equal the settings' count. Until the first call the palette is identity.
AVER_PHYS_API int32_t aver_phys_hair_set_palette(int32_t hair, const float* joints16, int32_t jointCount);
// World wind in cm/s (engine axes), a drag rate in 1/s and a turbulence amplitude in cm/s. Zero wind, or zero drag, records no kernel.
AVER_PHYS_API int32_t aver_phys_hair_set_wind(int32_t hair, const float* windCmS3, float dragPerS, float turbulenceCmS);
// Records one simulation step for this hair (wind kernel, then Jolt's Hair::Update) into the shared queue. dt is the frame time in
// seconds; Jolt clamps it to the settings' MAX_DELTA_TIME.
AVER_PHYS_API int32_t aver_phys_hair_update(int32_t hair, float dt);
// Executes and waits for everything the updates of this tick recorded. Once per tick, after all updates.
AVER_PHYS_API int32_t aver_phys_hair_flush(void);
// The host handle of the render positions buffer (float3 per render vertex, hair-local Jolt space: metres, Y up, right-handed),
// 0 for an unknown hair. With the CPU backend (no host installed) there is no host handle and the answer is 0.
AVER_PHYS_API uint64_t aver_phys_hair_render_buffer(int32_t hair);
// AVER_PHYS_HAIRUI_* ints and AVER_PHYS_HAIRUF_* floats about the last update; either array may be null.
AVER_PHYS_API int32_t aver_phys_hair_info(int32_t hair, int32_t* outI, int32_t nI, float* outF, int32_t nF);
// Debug and tests: reads the simulated vertices back (cm, hair space, engine axes, in the settings' simulated vertex order) and the
// render positions (the same space, in render buffer order). Either output may be null. Slow (it waits for the queue). Refused when the
// installed host cannot map a readback buffer.
AVER_PHYS_API int32_t aver_phys_hair_read_positions(int32_t hair, float* outSimXyz, int32_t maxSim, float* outRenderXyz, int32_t maxRender);

#ifdef __cplusplus
}
#endif
