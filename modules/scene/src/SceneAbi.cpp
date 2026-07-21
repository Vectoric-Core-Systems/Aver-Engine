#include "aver/scene/scene_abi.h"

// The C ABI's translation unit. Everything here is `extern "C"` and holds no C++ in its signature,
// so this file stays compilable as the surface grows and the rest of the module does not have to.

extern "C" {

int32_t aver_scene_abi_version(void) {
    // Compiled INTO the DLL, so it reports what this binary was built with rather than what the
    // caller's header says. A caller comparing the two is the only way a stale DLL is caught.
    return AVER_SCENE_ABI_VERSION;
}

} // extern "C"
