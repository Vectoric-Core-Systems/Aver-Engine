#include "aver/framework/framework_abi.h"

#include "aver/scene/scene_abi.h"

// The C ABI's translation unit. Everything here is `extern "C"` and holds no C++ in its signature.

extern "C" {

int32_t aver_fw_abi_version(void) {
    return AVER_FW_ABI_VERSION;
}

int32_t aver_fw_scene_abi_version(void) {
    // The value from the HEADER this DLL compiled against, deliberately not a call to
    // aver_scene_abi_version(). Calling would report whichever scene DLL happens to be loaded,
    // which is the thing the caller is trying to detect a mismatch with.
    return AVER_SCENE_ABI_VERSION;
}

int32_t aver_fw_scene_abi_matches(void) {
    // The one call in this module that crosses into Aver.Scene, and therefore the one thing making
    // the framework genuinely import from it. Compare MAJOR only: a newer minor is additive by the
    // contract scene_abi.h states, so refusing it would reject a scene DLL that is fine.
    const int32_t loaded = aver_scene_abi_version() >> 16;
    return loaded == AVER_SCENE_ABI_VERSION_MAJOR ? 1 : 0;
}

} // extern "C"
