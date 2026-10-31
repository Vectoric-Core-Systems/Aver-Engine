#pragma once
// The cost guard on meshes scripts build at run time (RENDER.RUNTIMEMESHES). The framework counts the meshes live at once and refuses past 256 until the host
// says otherwise; a project that builds many small meshes (voxel chunks) states a larger number. An unstated key is the default, not "keep the last project's".
#include "aver/formats/OcProject.hpp"

#if AVER_MODULE_FRAMEWORK
#  include "aver/framework/framework_rtmesh_abi.h"
#endif

namespace aver::game {

#if AVER_MODULE_FRAMEWORK
inline void configureRuntimeMeshBudget(const fmt::ProjectDesc& project) {
    // The vertex cap per mesh is not a project key: -1 keeps it.
    aver_fw_rtmesh_configure(project.runtimeMeshes > 0 ? project.runtimeMeshes : AVER_FW_RTMESH_DEFAULT_MAX_MESHES, -1);
}
#else
inline void configureRuntimeMeshBudget(const fmt::ProjectDesc&) {}
#endif

}  // namespace aver::game
