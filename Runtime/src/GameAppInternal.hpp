#pragma once
// Shared by GameApp.cpp and its per-feature parts (GameApp*.cpp, GameArgs.cpp): the include set, the module-macro defaults and the few helpers the parts share.
#include "aver/game/GameApp.hpp"
#include "aver/core/Types.hpp"   // AVER_PATH_SEP
#include "aver/game/GameCamera.hpp"
#include "aver/game/GameTick.hpp"
#include "aver/game/RuntimeMeshBudget.hpp"
#include "aver/game/GameSystemsWiring.hpp"   // Content/Physics/Surfaces.ocsurf -> the physics surface registry
#include "aver/game/PhysicalSurfaces.hpp"   // Content/Physics/Surfaces.ocsurf -> the physics surface registry
#include "aver/game/SoftBodyEffects.hpp"
#include "aver/game/SoftBodySave.hpp"   // soft-body damage on the $State channel
#include "aver/game/GameUiInput.hpp"
#include "aver/game/GameUiText.hpp"
#include "aver/game/GameScene2D.hpp"
#include "aver/game/GameUiImages.hpp"
#include "aver/game/PhysicalSurfaces.hpp"
#include "aver/game/LevelDecals.hpp"
#include "aver/game/LevelLights.hpp"
#include "aver/game/SequenceHost.hpp"   // hooks, level bindings, the ABI provider
#include "aver/game/LevelInstancesFramework.hpp"

#include <filesystem>

#include "aver/runtime/Engine.hpp"
#include "aver/platform/Window.hpp"
#include "aver/platform/FileSystem.hpp"
#if AVER_MODULE_FRAMEWORK
#  include "aver/game/ScriptDebugLines.hpp"
#  include "aver/game/ContentPrefetch.hpp"
#endif
#include "aver/core/Log.hpp"
#include "aver/extensions/Extensions.hpp"   // warnMissingProjectExtensions
// GPU-timing tree formatter
#include "aver/rhi/GpuTimingFormat.hpp"
#include "aver/rhi/Tonemap.hpp"


#if AVER_WITH_AUDIO_ABI
#  include "aver/audio/audio_abi.h"
#endif
#if AVER_WITH_UI_ABI
#  include "aver/ui/ui_abi.h"
// aver::ui::parseOcfont, for loadGameUiFont below (same .ocfont grammar SandboxApp.cpp parses).
#  include "aver/ui/UiFont.hpp"
// Decode font atlas PNG
#  include "aver/platform/Image.hpp"
#endif
#if AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"
#  include "aver/anim/AnimSystem.hpp"
#  include "aver/anim/ControlRig.hpp"
#  include "aver/save/SaveWorld.hpp"
#  include "aver/formats/OcSave.hpp"
#endif
#if AVER_MODULE_RAGDOLL
#  include "aver/ragdoll/RagdollSystem.hpp"
#endif
#if AVER_MODULE_SYNAPSE_SCENE
#  include "aver/synapse/SynapseAgent.hpp"
#  include "aver/synapse/SynapsePerception.hpp"
#  include "aver/synapse/SynapseBt.hpp"
#endif
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
#  include "aver/particles/ParticleSystem.hpp"
// Needed for spawnParticleTestContent
#  include "aver/scene/Components.hpp"
#  include "aver/scene/scene_abi.h"
#  include "aver/core/Hash.hpp"
#endif
#include "aver/assets/LevelSky.hpp"
// Compare editor and game render output (header-only)
#include "aver/world/SceneCensus.hpp"
#if AVER_MODULE_VOXI
#  include "aver/voxi/Voxi.hpp"
// Two-phase render settings apply from manifest
#  include "aver/voxi/ProjectRenderApply.hpp"
// AverSR scalability settings and resolution
#  include "aver/voxi/Scalability.hpp"
#endif
#if AVER_MODULE_VOXI && AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
// Borrowed HLSL for particleGiPrepare/particleGiBind
#  include "aver/voxi/VoxiGiShaders.hpp"
#endif
#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#endif
#if AVER_MODULE_FRAMEWORK
#  include "aver/framework/framework_abi.h"
#  include "aver/framework/framework_state_abi.h"
#  include "aver/framework/framework_hooks.h"
#  include "aver/framework/framework_anim_abi.h"
#  include "aver/framework/framework_ik_abi.h"   // aver_fw_set_ik_provider
#  include "aver/framework/framework_pose_abi.h" // aver_fw_set_pose_provider (script bone poses)
#  include "aver/framework/framework_physanim_abi.h"   // aver_fw_set_physanim_provider (hit reactions, death, get-up)

#  include "aver/framework/framework_morph_abi.h"   // aver_fw_set_morph_provider
#  include "aver/framework/framework_worldfx_abi.h"   // script sun / fog / post layers
#  include "aver/framework/framework_render_abi.h"    // P17: script render settings and statistics
#  if AVER_MODULE_VOXI
#    define AVER_SCRIPTRENDER_HAS_ABI 1
#    include "aver/voxi/ScriptRenderSettings.hpp"
#  endif
#endif
// GamePawn.hpp (game::placePossessedPawn): placePawnAtSpawn's pawn lookup and placement.
#include "aver/game/GamePawn.hpp"
#include "aver/game/GameIkProbe.hpp"   // the foot-IK ground probe
// Culture choice and string/data-table stores (docs/LOCALIZATION.md).
#include "aver/game/GameLocalization.hpp"
#if AVER_MODULE_FRAMEWORK
#  include "aver/framework/framework_loc_abi.h"
#endif
#if AVER_MODULE_VOXI
// For loading instanced foliage
#  include "aver/game/GameFoliage.hpp"
#endif
#if AVER_MODULE_PBR
// For texture cache setup
#  include "aver/assets/TextureUpload.hpp"
#endif

// Gated on _WIN32 alone, not nested in a module guard (unlike SandboxApp.hpp's copy of this comment):
// onUpdate's capture-decision check below needs it regardless of which modules are linked.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <cmath>
#include <vector>
#include <cstdlib>
#include <cstring>

// These module macros are PUBLIC compile defs reaching this TU only via Aver.Runtime.Game's link
// interface. Defaulting to 0 lets a missing link interface print as "off" instead of failing to
// compile or silently compiling to nothing.
#ifndef AVER_MODULE_PBR
#  define AVER_MODULE_PBR 0
#endif
#ifndef AVER_MODULE_SCENE
#  define AVER_MODULE_SCENE 0
#endif
#ifndef AVER_MODULE_VOXI
#  define AVER_MODULE_VOXI 0
#endif
#ifndef AVER_MODULE_PHYSICS
#  define AVER_MODULE_PHYSICS 0
#endif
#ifndef AVER_MODULE_FRAMEWORK
#  define AVER_MODULE_FRAMEWORK 0
#endif
#ifndef AVER_MODULE_SCRIPTING
#  define AVER_MODULE_SCRIPTING 0
#endif
#ifndef AVER_MODULE_TRIFACTOR
#  define AVER_MODULE_TRIFACTOR 0
#endif
#ifndef AVER_MODULE_PARTICLES
#  define AVER_MODULE_PARTICLES 0
#endif

namespace aver::game::app_detail {

#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
// The save provider seams aver_fw_set_save_provider takes (GameAppSave.cpp).
i32 saveWriteProvider(const char* path, void* user);
i32 saveLoadProvider(const char* path, void* user);
#endif

// ASCII case-insensitive equality (GameArgs.cpp).
bool equalsAsciiCI(std::string_view a, std::string_view b);

// The player's Settings.ini: scripting opens it for input bindings, localization reads the saved culture from it (GameAppProject.cpp).
std::string gameSettingsPath(const fmt::ProjectDesc& project);

} // namespace aver::game::app_detail

namespace aver::game { using namespace app_detail; }
