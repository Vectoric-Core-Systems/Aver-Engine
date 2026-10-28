#ifndef AVER_FRAMEWORK_RTMESH_ABI_H
#define AVER_FRAMEWORK_RTMESH_ABI_H

/* Runtime meshes and MultiMeshes (Aver.Framework, G4 R7): meshes a script builds from arrays, and instance tables drawn as one mesh. Same shape as
 * framework_subview_abi.h: the script PUBLISHES, the host READS each frame and uploads what changed. Nothing exists, and nothing is uploaded, until a script
 * or a converted level makes one.
 *
 * Own version, separate from AVER_FW_ABI_VERSION_MINOR. int32_t/uint32_t/uint64_t/float/const char* only; 1 on success and 0 on refusal. All state lives
 * in this DLL, single game thread. Everything is in ENGINE space: centimetres, +Z up, counter-clockwise front faces seen from outside (Aver.Fabric does
 * the Godot conversion before it calls).
 *
 * A mesh is named by a KEY (UTF-8, 1..120 bytes). An entity draws it by setting its mesh renderer's mesh to the asset path AVER_FW_RTMESH_PATH_PREFIX + key
 * ("@rtmesh/<key>"; the id the renderer looks up is fnv1a64 of that path, as for every asset), and its material as usual. The material of a surface is
 * the NAME the script gave it (the mesh's material slots), resolved by the project's content like an .ocmesh's slot names.
 *
 * Cost guard: at most AVER_FW_RTMESH_DEFAULT_MAX_MESHES meshes live and AVER_FW_RTMESH_DEFAULT_MAX_VERTICES vertices in any one of them unless the host
 * says otherwise (aver_fw_rtmesh_configure). A MultiMesh bakes every visible instance into one mesh, so its instance count times the source's vertex count
 * is held to that limit (the visible count is lowered to fit and aver_fw_multimesh_info says how many were drawn). */

#include "aver/framework/framework_abi.h"   /* AVER_FW_ABI */

#ifdef __cplusplus
extern "C" {
#endif

#define AVER_FW_RTMESH_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_rtmesh_abi_version(void);

#define AVER_FW_RTMESH_PATH_PREFIX "@rtmesh/"
#define AVER_FW_RTMESH_DEFAULT_MAX_MESHES 256
#define AVER_FW_RTMESH_DEFAULT_MAX_VERTICES 2000000

/* ---- script side: runtime meshes -------------------------------------------------------------------------------- */

/* Makes the mesh named `key`, or returns the id it already has. 0 when refused (bad key, or the budget of meshes is full). A new mesh has no surfaces and is
 * not uploaded until it has a triangle. */
AVER_FW_ABI int32_t aver_fw_rtmesh_create(const char* key);
AVER_FW_ABI int32_t aver_fw_rtmesh_destroy(int32_t id);
AVER_FW_ABI int32_t aver_fw_rtmesh_find(const char* key);
/* Removes every surface (the mesh keeps its key and id) and marks it changed. */
AVER_FW_ABI int32_t aver_fw_rtmesh_clear(int32_t id);
/* Appends a surface: vertexCount positions (3 floats each, required); nrm (3 each; NULL = smooth normals are computed), uv (2 each; NULL = zero), col (4 each,
 * linear; NULL = none), uv1 (2 each; NULL = none) and tan (4 each: unit tangent xyz and the handedness w = +1/-1 per the G4 R1 tangent contract; NULL = none)
 * are optional. indexCount 0 draws the vertices in order, else idx is a triangle list. material may be NULL. Returns the surface's index or -1 on refusal
 * (bad key/id, an index past the vertices, a budget overrun). Marks the mesh changed. */
AVER_FW_ABI int32_t aver_fw_rtmesh_add_surface(int32_t id, int32_t vertexCount, const float* pos, const float* nrm, const float* uv, const float* col,
                                               const float* uv1, const float* tan, int32_t indexCount, const uint32_t* idx, const char* material);
AVER_FW_ABI int32_t aver_fw_rtmesh_surface_count(int32_t id);
/* Four ints: surfaces, vertices, triangles, generation (counts changes). */
AVER_FW_ABI int32_t aver_fw_rtmesh_info(int32_t id, int32_t* out4);
/* The id the mesh renderer holds for this mesh: fnv1a64 of "@rtmesh/<key>". 0 for an unknown id. */
AVER_FW_ABI uint64_t aver_fw_rtmesh_object_id(int32_t id);
/* The key, UTF-8, NUL-terminated into out (cap bytes including the NUL); returns the key's length, 0 for an unknown id. */
AVER_FW_ABI int32_t aver_fw_rtmesh_key(int32_t id, char* out, int32_t cap);

/* ---- script side: MultiMeshes ----------------------------------------------------------------------------------- */

/* Makes the MultiMesh named `key` or returns its id. Its baked mesh is the runtime mesh "multimesh/<key>" (aver_fw_multimesh_mesh), which an entity draws
 * through its path "@rtmesh/multimesh/<key>". */
AVER_FW_ABI int32_t aver_fw_multimesh_create(const char* key);
AVER_FW_ABI int32_t aver_fw_multimesh_destroy(int32_t id);
/* The mesh to instance: a content-relative .ocmesh path ("Meshes/Rock.ocmesh"), or a runtime mesh by its path ("@rtmesh/<key>"). */
AVER_FW_ABI int32_t aver_fw_multimesh_set_source(int32_t id, const char* meshPath);
/* useColors: per-instance colours multiply the mesh's vertex colour (the material needs vertex colour as albedo, as Godot's does). useCustom: custom.xy
 * become the mesh's second UV set. */
AVER_FW_ABI int32_t aver_fw_multimesh_set_flags(int32_t id, int32_t useColors, int32_t useCustom);
/* Resizes the instance table (new instances are identity transforms, white, zero custom data). */
AVER_FW_ABI int32_t aver_fw_multimesh_set_count(int32_t id, int32_t instanceCount);
/* How many instances are drawn: the first N. -1 = all. */
AVER_FW_ABI int32_t aver_fw_multimesh_set_visible(int32_t id, int32_t visibleCount);
/* t12 is the engine's row-vector world matrix with its 4th column dropped (the .ocinst layout): three basis rows then the translation. */
AVER_FW_ABI int32_t aver_fw_multimesh_set_transform(int32_t id, int32_t index, const float* t12);
AVER_FW_ABI int32_t aver_fw_multimesh_set_color(int32_t id, int32_t index, float r, float g, float b, float a);
AVER_FW_ABI int32_t aver_fw_multimesh_set_custom(int32_t id, int32_t index, float x, float y, float z, float w);
/* All at once: `count` instances (the table is resized to it); colors4 / custom4 may be NULL. */
AVER_FW_ABI int32_t aver_fw_multimesh_set_buffer(int32_t id, int32_t count, const float* transforms12, const float* colors4, const float* custom4);
/* Four ints: instances, visible (requested), drawn (after the vertex budget; 0 until baked), generation of the baked mesh. */
AVER_FW_ABI int32_t aver_fw_multimesh_info(int32_t id, int32_t* out4);
/* The runtime mesh id the MultiMesh bakes into (0 for an unknown id). */
AVER_FW_ABI int32_t aver_fw_multimesh_mesh(int32_t id);

/* ---- host side -------------------------------------------------------------------------------------------------- */

/* Sets the cost guard. maxMeshes < 1 and maxVertices < 1 keep the defaults. Called once from the host. */
AVER_FW_ABI void aver_fw_rtmesh_configure(int32_t maxMeshes, int32_t maxVertices);
/* Bakes every MultiMesh whose table or source changed (once each). Returns how many were baked. Call once a frame before aver_fw_rtmesh_take_dirty. Also
 * drops everything when a play session has ended (the state fell back to the editor), queueing the removals. */
AVER_FW_ABI int32_t aver_fw_rtmesh_pump(void);
/* Meshes changed since the host last took them: ids into outIds (at most max); the returned meshes are no longer dirty. */
AVER_FW_ABI int32_t aver_fw_rtmesh_take_dirty(int32_t* outIds, int32_t max);
/* Meshes destroyed since the host last asked: their object ids (fnv1a64 of the path), so the host can unload them. */
AVER_FW_ABI int32_t aver_fw_rtmesh_take_removed(uint64_t* outObjectIds, int32_t max);

/* The built mesh, pointers valid until the next call that changes any mesh. A mesh with no triangle answers 0. */
typedef struct AverFwRtMeshView {
    int32_t vertexCount;
    int32_t indexCount;
    int32_t submeshCount;
    const float* pos;        /* 3 per vertex */
    const float* nrm;        /* 3 per vertex */
    const float* uv;         /* 2 per vertex */
    const float* col;        /* 4 per vertex, or NULL */
    const float* uv1;        /* 2 per vertex, or NULL */
    const float* tan;        /* 4 per vertex, or NULL */
    const uint32_t* idx;     /* indexCount, a triangle list over all submeshes */
    const int32_t* subStart; /* submeshCount: first index of each submesh */
    const int32_t* subCount; /* submeshCount: index count of each submesh */
    const char* const* subMaterial; /* submeshCount: the material NAME of each (UTF-8) */
    uint64_t objectId;
} AverFwRtMeshView;
AVER_FW_ABI int32_t aver_fw_rtmesh_view(int32_t id, AverFwRtMeshView* out);

AVER_FW_ABI int32_t aver_fw_rtmesh_count(void);
/* Session boundary: forgets every mesh and MultiMesh and queues their removals. */
AVER_FW_ABI void aver_fw_rtmesh_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* AVER_FRAMEWORK_RTMESH_ABI_H */
