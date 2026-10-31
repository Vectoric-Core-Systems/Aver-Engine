#pragma once
// Feeds the framework's local fog volumes (framework_envfx_abi.h, G4 R8) to Voxi's volumetric fog every frame, for either host. A script or a converted level
// PUBLISHES volumes; this READS them: the 16 nearest the camera as one FogVolumeBlock. With nothing registered it does nothing beyond clearing what an earlier
// frame pushed, so a project without fog volumes runs exactly today's froxel passes. Reflection probes are not fed anywhere yet (the renderer samples no
// capture); the first frame that has any logs that once.
// Include under AVER_MODULE_VOXI.
#if AVER_MODULE_VOXI

#include "aver/core/FogVolume.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/framework/framework_envfx_abi.h"
#include "aver/game/CustomEnvFeed.hpp"   // custom sky / fog / particles programs
#include "aver/voxi/VoxiRenderer.hpp"

namespace aver::game {

class EnvFxFeed {
public:
    // Once per frame, before the frame renders.
    void update(voxi::VoxiRenderer& voxi, const Vec3& eye) {
        aver_fw_envfx_tick();
        customEnv_.update(voxi);   // before the fog volumes: a volume's shader index names the table this just pushed
        const bool any = aver_fw_fogvol_count() > 0;
        if (!any) {
            if (pushed_) { voxi.setFogVolumes(nullptr); pushed_ = false; }
        } else {
            FogVolumeBlock block;
            static_assert(sizeof(FogVolumeBlock) % sizeof(float) == 0, "packed as floats");
            const int32_t n = aver_fw_fogvol_pack(eye.x, eye.y, eye.z, reinterpret_cast<float*>(&block),
                                                  static_cast<int32_t>(sizeof(FogVolumeBlock) / sizeof(float)));
            voxi.setFogVolumes(n > 0 ? &block : nullptr);
            pushed_ = n > 0;
            if (n != lastCount_) {
                lastCount_ = n;
                AVER_INFO("[EnvFx] {} fog volume(s) drawn{}", n, voxi.volumetricFogOn() ? "" : " (volumetric fog is off: RENDER.VOLFOG 1 shows them)");
            }
        }
        if (!probeLogged_ && aver_fw_probe_count() > 0) {
            probeLogged_ = true;
            AVER_INFO("[EnvFx] {} reflection probe(s) registered; the renderer samples no captured environment yet, so reflections stay Aver's own (R8, https://github.com/Vectoric-Core-Systems/Fabric-Connector/blob/main/docs/plans/G4_RENDER.md)",
                      aver_fw_probe_count());
        }
    }

private:
    CustomEnvFeed customEnv_;
    bool pushed_ = false;
    bool probeLogged_ = false;
    int32_t lastCount_ = -1;
};

} // namespace aver::game

#endif // AVER_MODULE_VOXI
