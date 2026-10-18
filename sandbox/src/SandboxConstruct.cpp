// SandboxApp::SandboxApp: the constructor. Split from SandboxApp.cpp, which was nearing the COFF section cap in Debug
// (the constructor's exception-cleanup code for the class's member set is most of it; sandbox/CMakeLists.txt: split, never /bigobj).
#include "aver/core/HitchMarks.hpp"
#include "aver/core/Types.hpp"   // AVER_PATH_SEP
#include "SandboxApp.hpp"
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
#  include "aver/framework/framework_anim_abi.h"   // aver_fw_set_anim_provider (montages)
#  include "aver/framework/framework_ik_abi.h"     // [wave5/ik-retarget] aver_fw_set_ik_provider

#  include "aver/framework/framework_morph_abi.h"   // [wave9/morph] aver_fw_set_morph_provider
#  include "aver/framework/framework_worldfx_abi.h"   // [wave9/worldfx] aver_fw_worldfx_tick
#endif
#if AVER_MODULE_FRAMEWORK
#  include "aver/framework/framework_loc_abi.h"    // the UI layout editor's culture preview
#endif
#if AVER_MODULE_RAGDOLL
#  include "aver/ragdoll/RagdollSystem.hpp"
#endif
#include "aver/assets/LevelSky.hpp"
#include "TextureEditor.hpp"   // makeTextureEditor; SandboxApp.hpp does not pull this one in
#include "InstanceSetEditor.hpp"   // makeInstanceSetEditor (.ocinst); not pulled in by SandboxApp.hpp
#include "aver/game/GameCamera.hpp"
#include "aver/game/GameIkProbe.hpp"   // [wave5/ik-retarget] the foot-IK ground probe
#include "aver/game/GameTick.hpp"
#include "aver/game/GameSystemsWiring.hpp"
#include "aver/game/SequenceHost.hpp"   // [wave5/sequencer] the ABI provider
#include "aver/game/GameUiInput.hpp"
#include "aver/game/GameUiText.hpp"
#include "aver/platform/FileSystem.hpp"   // userDataDir: where the trajectory network's weights live
#if AVER_MODULE_VOXI
#include "ModeSwitchNotice.hpp"
#endif

namespace aver {

SandboxApp::SandboxApp(u64 maxFrames, bool headless, std::string beamPath, std::string shot, Tool initialTool)
    : maxFrames_(maxFrames), headless_(headless), beamPath_(std::move(beamPath)), shot_(std::move(shot)), initialTool_(initialTool) {
    setLogSink(&SandboxApp::logSink, this);
}

} // namespace aver
