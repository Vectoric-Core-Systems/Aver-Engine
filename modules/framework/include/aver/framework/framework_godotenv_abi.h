#ifndef AVER_FRAMEWORK_GODOTENV_ABI_H
#define AVER_FRAMEWORK_GODOTENV_ABI_H

/* Converted Godot sky, fog and particles shaders (Aver.Framework, Fabric G6 SH3): the script side registers a shader the converter wrote (a `DOMAIN godotenv`
 * .ocgraph with one GodotEnvShader node), sets its uniform registers and picks the active sky; the host side reads what changed every frame and hands it to the
 * renderers (Voxi's sky and volumetric fog, the GPU particle simulation). Same shape as framework_envfx_abi.h. Nothing exists or runs unless a shader is registered.
 *
 * Own version, separate from AVER_FW_ABI_VERSION_MINOR. int32_t / float / const char* only; 1 on success and 0 on refusal. All state lives in this DLL, single game
 * thread. A shader is named by KIND and NAME; registering a name that exists replaces its program (and resets its registers to the graph's defaults). Registers are
 * the shader's uniform table: four floats a register, at most 16 registers, normal block first then the global block (docs/plans/fabric/G6_SHADERS.md SH3). */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI */

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_FW_GENV_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_genv_abi_version(void);

#define AVER_FW_GENV_SKY       0
#define AVER_FW_GENV_FOG       1
#define AVER_FW_GENV_PARTICLES 2
#define AVER_FW_GENV_MAX       32   /* programs the registry keeps per kind (the renderer draws the first 4 fog shaders) */

/* ---- script side ------------------------------------------------------------------------------------------------ */

/* Parses `ocgraphText` (the converter's .ocgraph) and registers it under `name`. Returns 0 for a malformed graph, a kind that does not match the graph's, or a full registry. */
AVER_FW_ABI int32_t aver_fw_genv_register(int32_t kind, const char* name, const char* ocgraphText);
/* Replaces the uniform registers (floatCount floats, four a register; extra floats are ignored, missing registers keep their value). Moves only the registers revision. */
AVER_FW_ABI int32_t aver_fw_genv_set_registers(int32_t kind, const char* name, const float* regs, int32_t floatCount);
/* Makes the registered sky shader `name` the sky; null or empty goes back to the engine's own sky. Returns 0 when the name is not registered. */
AVER_FW_ABI int32_t aver_fw_genv_use_sky(const char* name);
AVER_FW_ABI int32_t aver_fw_genv_unregister(int32_t kind, const char* name);

/* ---- host side -------------------------------------------------------------------------------------------------- */

/* Moves when a program is registered, replaced or removed or the active sky changes: the host rebuilds the renderer's pipelines only then. */
AVER_FW_ABI int64_t aver_fw_genv_revision(void);
/* Moves when registers change: the host pushes values without rebuilding anything. */
AVER_FW_ABI int64_t aver_fw_genv_registers_revision(void);
AVER_FW_ABI int32_t aver_fw_genv_count(int32_t kind);
/* Copies the program's name / registered .ocgraph text into out (NUL-terminated, truncated to cap) and returns the full length; null out just measures. -1 for a bad index. */
AVER_FW_ABI int32_t aver_fw_genv_name(int32_t kind, int32_t index, char* out, int32_t cap);
AVER_FW_ABI int32_t aver_fw_genv_graph(int32_t kind, int32_t index, char* out, int32_t cap);
/* The current registers (four floats a register). Returns the float count; null out just measures. */
AVER_FW_ABI int32_t aver_fw_genv_registers(int32_t kind, int32_t index, float* out, int32_t capFloats);
/* Index (kind sky) of the active sky shader, -1 for the engine's own sky. */
AVER_FW_ABI int32_t aver_fw_genv_sky_index(void);
/* Session boundary, like aver_fw_envfx_reset (aver_fw_envfx_tick calls it when the play state falls back to the editor). */
AVER_FW_ABI void    aver_fw_genv_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* AVER_FRAMEWORK_GODOTENV_ABI_H */
