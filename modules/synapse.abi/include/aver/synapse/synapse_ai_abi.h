#pragma once
/* Aver.Synapse.Abi -- the plain-C ABI for steering/crowds, hearing, cover and squads.
 *
 * Units and axes are the engine's: centimetres, +X forward, +Y right, +Z up. Entities are the
 * scene's 32-bit handles. Functions return 1 on success and 0 on failure unless stated.
 *
 * ONE INSTANCE. The systems behind this header live in this DLL and nowhere else; a host that also
 * links Aver.Synapse.Scene statically must drive them through these functions (or
 * aver_syn_ai_instance) rather than construct its own, or the C# side would talk to a different
 * crowd than the one being ticked.
 */
#include <stdint.h>

#if defined(_WIN32)
#  if defined(AVER_SYN_ABI_BUILD)
#    define AVER_SYN_API __declspec(dllexport)
#  else
#    define AVER_SYN_API __declspec(dllimport)
#  endif
#else
#  define AVER_SYN_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Lifecycle (host) ------------------------------------------------------------------------- */

/* Registers the components with scene::World::instance(). Idempotent. */
AVER_SYN_API int32_t aver_syn_ai_register(void);
/* Advances hearing, cover/squads, then crowds. `nav` is an aver::fmt::OcNavData* or NULL. Call
 * after AgentSystem::tick and before the behaviour-tree tick. */
AVER_SYN_API void    aver_syn_ai_tick(const void* nav, float dt);
/* Adds the tactics vocabulary (HeardNoise, TakeCover, SquadFlank, ...) to a BtRegistry*. */
AVER_SYN_API void    aver_syn_ai_register_behaviors(void* btRegistry);
/* The SynapseAi* behind everything, for a C++ host built with the same headers. */
AVER_SYN_API void*   aver_syn_ai_instance(void);
/* Drops every cover point, reservation, agent and memory. */
AVER_SYN_API void    aver_syn_ai_reset(void);

/* ---- Crowd ------------------------------------------------------------------------------------ */

#define AVER_SYN_BACKEND_CPU 0
#define AVER_SYN_BACKEND_GPU 1

#define AVER_SYN_MODE_FOLLOW_AGENT 0
#define AVER_SYN_MODE_SEEK   1
#define AVER_SYN_MODE_ARRIVE 2
#define AVER_SYN_MODE_FLEE   3
#define AVER_SYN_MODE_WANDER 4
#define AVER_SYN_MODE_HOLD   5

#define AVER_SYN_DRIVE_ADVISE 0
#define AVER_SYN_DRIVE_MOVE   1

/* Adds CSynapseCrowd with defaults (radius and speed from CSynapseAgent when present). */
AVER_SYN_API int32_t aver_syn_crowd_attach(int32_t entity);
AVER_SYN_API int32_t aver_syn_crowd_configure(int32_t entity, float radiusCm, float maxSpeedCm,
                                              float maxAccelCm, float priority);
AVER_SYN_API int32_t aver_syn_crowd_set_mode(int32_t entity, int32_t mode, float x, float y, float z);
AVER_SYN_API int32_t aver_syn_crowd_set_enabled(int32_t entity, int32_t enabled);
/* The avoided velocity. Returns 0 when the entity is not a simulated crowd agent. */
AVER_SYN_API int32_t aver_syn_crowd_velocity(int32_t entity, float* outVx, float* outVy, float* outSpeed);

/* CPU is the default and is deterministic. GPU needs a backend installed with
 * aver_syn_crowd_set_gpu_backend and falls back to CPU while none is available. */
AVER_SYN_API int32_t aver_syn_crowd_set_backend(int32_t backend);
AVER_SYN_API int32_t aver_syn_crowd_backend(void);                 /* the one requested */
AVER_SYN_API const char* aver_syn_crowd_backend_name(void);        /* the one actually in use */
/* `icrowdBackend` is an aver::synapse::ICrowdBackend* (Aver.Synapse.Gpu's GpuCrowdBackend). */
AVER_SYN_API void    aver_syn_crowd_set_gpu_backend(void* icrowdBackend);
AVER_SYN_API int32_t aver_syn_crowd_set_max_agents(int32_t maxAgents);
AVER_SYN_API int32_t aver_syn_crowd_max_agents(void);
AVER_SYN_API int32_t aver_syn_crowd_agent_count(void);             /* simulated now */
AVER_SYN_API int32_t aver_syn_crowd_overflow_count(void);          /* over the cap, not simulated */
AVER_SYN_API int32_t aver_syn_crowd_set_drive(int32_t drive);

/* ---- Hearing ---------------------------------------------------------------------------------- */

AVER_SYN_API int32_t aver_syn_hearing_attach(int32_t entity);
AVER_SYN_API int32_t aver_syn_hearing_configure(int32_t entity, float sensitivity, float maxRangeCm,
                                                float memorySec);
/* Queues a noise. `loudnessCm` is its audible radius for sensitivity 1 in the open. */
AVER_SYN_API int32_t aver_syn_emit_noise(float x, float y, float z, float loudnessCm, int32_t tag,
                                         int32_t sourceEntity);
/* The strongest remembered noise. Returns 0 when the entity remembers nothing. */
AVER_SYN_API int32_t aver_syn_hearing_get(int32_t entity, float* outX, float* outY, float* outZ,
                                          float* outLevel, int32_t* outTag, float* outConfidence,
                                          float* outTimeSinceSec);
AVER_SYN_API int32_t aver_syn_hearing_forget(int32_t entity);

/* Memory change callback: `forgotten` is 0 when a noise was heard or refreshed, 1 when an entry
 * decayed away. Install NULL to remove. */
typedef void (*aver_syn_memory_fn)(int32_t listener, float x, float y, float z, float level,
                                   int32_t tag, int32_t source, float confidence, int32_t forgotten,
                                   void* user);
AVER_SYN_API void    aver_syn_hearing_set_memory_callback(aver_syn_memory_fn fn, void* user);

/* ---- Cover ------------------------------------------------------------------------------------ */

/* An authored point not tied to an entity. Returns its id, 0 on failure. height: 0 low, 1 high. */
AVER_SYN_API int32_t aver_syn_cover_add(float x, float y, float dirX, float dirY, int32_t height,
                                        float arcHalfAngleDeg);
AVER_SYN_API int32_t aver_syn_cover_remove(int32_t coverId);
/* An entity marker: position = where to stand, local +X = toward the protecting obstacle. */
AVER_SYN_API int32_t aver_syn_cover_marker_attach(int32_t entity, int32_t height, float arcHalfAngleDeg);
AVER_SYN_API int32_t aver_syn_cover_set_auto_generate(int32_t on, float minSpacingCm);
AVER_SYN_API int32_t aver_syn_cover_count(void);

/* Finds and reserves the best point protecting `seeker` from the threat. Returns the cover id (> 0)
 * and its position, or 0 when none protects. */
AVER_SYN_API int32_t aver_syn_cover_find(int32_t seeker, float threatX, float threatY, float threatZ,
                                         float maxSeekCm, float minThreatDistCm, int32_t requireHigh,
                                         float* outX, float* outY);
AVER_SYN_API int32_t aver_syn_cover_release(int32_t seeker);
AVER_SYN_API int32_t aver_syn_cover_is_covered(int32_t seeker, float threatX, float threatY, float threatZ);

/* ---- Squads ----------------------------------------------------------------------------------- */

#define AVER_SYN_ROLE_NONE 0
#define AVER_SYN_ROLE_ANCHOR 1
#define AVER_SYN_ROLE_FLANK_LEFT 2
#define AVER_SYN_ROLE_FLANK_RIGHT 3
#define AVER_SYN_ROLE_SUPPORT 4

AVER_SYN_API int32_t aver_syn_squad_attach(int32_t entity, int32_t squadId, float spacingCm);
AVER_SYN_API int32_t aver_syn_squad_set_target(int32_t squadId, float x, float y, float z);
AVER_SYN_API int32_t aver_syn_squad_clear_target(int32_t squadId);
/* Role and slot for a member. Returns 0 until the squad has a target. */
AVER_SYN_API int32_t aver_syn_squad_slot(int32_t entity, int32_t* outRole, float* outX, float* outY,
                                         float* outZ);
AVER_SYN_API int32_t aver_syn_squad_spacing_push(int32_t entity, float* outDx, float* outDy);

#ifdef __cplusplus
}
#endif
