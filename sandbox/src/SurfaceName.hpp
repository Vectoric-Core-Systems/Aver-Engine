#pragma once
// What to call a surface in a message, in EVERY module configuration.
//
// A surface token is an i32 the scene hands out; the name behind it lives in the scene's own
// registry and is read through aver_scene_material_name (aver/scene/scene_abi.h). Two warnings --
// SandboxApp::warnDeadMaterialHandle and the missing-.ocmat warning in resolveSurface -- called
// that function directly from bodies compiled in every configuration, so `scene-off` failed on
// them with C3861. Both were found by scripts/module-matrix.ps1 and by nothing else.
//
// GUARDING THE WARNINGS WAS THE WRONG FIX and was considered first. Neither warning is about the
// scene: one says a material handle went stale, the other says a named surface has no .ocmat
// anywhere. Both are exactly as true, and exactly as worth printing, in a build with no entity
// world -- so the thing to make configuration-independent is the NAME LOOKUP, not the message.
//
// WITH NO SCENE, THE TOKEN IS THE NAME. A build with no registry to ask cannot produce the
// author's spelling, and printing "" or "(unknown)" would turn a diagnostic that names one
// surface into one that names none. The number is what the caller has, it is stable, and it is
// enough to find the surface in the level file -- so the message stays useful rather than staying
// well-formed. The `#{}` spelling is there so a reader can tell a token from a name at a glance.
//
// A std::string BY VALUE, not a const char*, because the AVER_MODULE_SCENE=0 arm has to own the
// text it formats. The scene arm copies one short registry string per warning, and every caller
// here is a once-per-name warning behind a std::unordered_set -- this is not on any hot path.
#include "aver/core/Types.hpp"

#if AVER_MODULE_SCENE
#include "aver/scene/scene_abi.h"
#endif

#include <string>

namespace aver::editor {

inline std::string surfaceDisplayName(i32 token) {
#if AVER_MODULE_SCENE
    if (const char* n = aver_scene_material_name(token); n && *n) return n;
#endif
    return "#" + std::to_string(token);
}

} // namespace aver::editor
