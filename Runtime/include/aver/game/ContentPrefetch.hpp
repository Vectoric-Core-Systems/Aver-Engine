#pragma once
// ContentPrefetch: serves the preload hints scripts send (aver_fw_prefetch_content) by queueing the named mesh's or
// material's textures on the texture workers. Header-only: the editor (Play) and the shipped game call it once a frame.
#include "aver/framework/framework_abi.h"
#include "aver/game/GameContent.hpp"

namespace aver::game {

// Takes every waiting request (a frame never takes more than 256; the rest wait). The hint is dropped, not an error,
// when the host has no materials to load.
inline void pumpContentPrefetch(GameContent& content) {
    char name[512];
    for (int n = 0; n < 256; ++n) {
        int32_t kind = -1;
        if (aver_fw_take_prefetch(&kind, name, static_cast<int32_t>(sizeof name)) <= 0) return;
#if AVER_MODULE_PBR
        if (kind == AVER_FW_PREFETCH_MATERIAL) content.prefetchMaterial(name);
#  if AVER_MODULE_SCENE
        else if (kind == AVER_FW_PREFETCH_MESH) content.prefetchMeshMaterials(name);
#  endif
#else
        (void)content; (void)kind;
#endif
    }
}

}  // namespace aver::game
