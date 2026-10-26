# InputBridgeTest -- the ImGui-free input path, end to end.
#
# Feeds synthetic window events through InputState, publishes into the framework, and reads the
# framework's own state back through the same C ABI gameplay uses. No window, no device, no ImGui
# and no human, which is the point: the defect it guards (an AVER_ENABLE_UI=OFF build having no
# input at all) compiles cleanly and only shows up when somebody presses a key.
add_executable(InputBridgeTest src/InputBridgeTest.cpp)
target_link_libraries(InputBridgeTest PRIVATE Aver.Runtime.Game.Core Aver.Framework Aver.Platform Aver.Core)
set_target_properties(InputBridgeTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# PlayMobilityTest -- which entities count as moving during a play session. The rule's own cases, then a
# few hundred frames of random moves, spawns, deaths, reparenting and pawn changes compared against the
# rule as first written (hash every matrix, walk every ancestor chain), so the cheap version cannot have
# changed an answer. Needs a real scene::World and nothing else: no device, no window.
add_executable(PlayMobilityTest src/PlayMobilityTest.cpp)
target_link_libraries(PlayMobilityTest PRIVATE Aver.Runtime.Game.Core Aver.Scene Aver.Core)
set_target_properties(PlayMobilityTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# OriginParticipantsTest -- the per-system halves of a floating-origin shift (docs/plans/wave11/LARGE_WORLD.md 2.2.5, 5.2): PlayMobility, the anim
# object base, the sequencer base store and origin-relative keys, the camera rig, soft body, the kinematic driver, Synapse and the mixer, each against
# its real system with the scene shifted by hand (World::shiftRoots stands in for the scene participant). A section compiles in only when its module's
# target exists (ORIGINTEST_* below). Real Jolt world for the camera, soft body and kinematic sections; no GPU, no window.
add_executable(OriginParticipantsTest src/OriginParticipantsTest.cpp)
target_link_libraries(OriginParticipantsTest PRIVATE Aver.Runtime.Game.Core Aver.World Aver.Scene Aver.Formats Aver.Core)
foreach(_pair ANIM:Aver.Anim.Scene CAMERA:Aver.Camera SOFTBODY:Aver.SoftBody.Scene HAIR:Aver.Hair.Scene SYNAPSE:Aver.Synapse.Scene AUDIO:Aver.Audio PHYSICS:Aver.Physics)
  string(REPLACE ":" ";" _parts "${_pair}")
  list(GET _parts 0 _flag)
  list(GET _parts 1 _target)
  if(TARGET ${_target})
    target_link_libraries(OriginParticipantsTest PRIVATE ${_target})
    target_compile_definitions(OriginParticipantsTest PRIVATE ORIGINTEST_${_flag}=1)
  endif()
endforeach()
set_target_properties(OriginParticipantsTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# AuthoredStreamingTest -- game::AuthoredStreaming: the choice (fresh / stale / missing), the cooked set streamed through ItemLoader, a rebase between ticks loads
# and evicts nothing, a teleport, end(). A real scene::World and an empty GameContent; no device, no window.
add_executable(AuthoredStreamingTest src/AuthoredStreamingTest.cpp)
target_link_libraries(AuthoredStreamingTest PRIVATE Aver.Runtime.Game.Core Aver.World Aver.Scene Aver.Formats Aver.Platform Aver.Core)
set_target_properties(AuthoredStreamingTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ProjectPhysicsApplyTest -- the manifest's PHYSICS.* keys from file to running world: the name mirror in OcProject.cpp against the
# physics table, parse / write / re-save, configureProjectPhysics (PhysicsProjectApply.hpp) and the rebuild of an empty world.
# Links Formats, Physics and the Runtime headers; no device, no window.
add_executable(ProjectPhysicsApplyTest src/ProjectPhysicsApplyTest.cpp)
target_link_libraries(ProjectPhysicsApplyTest PRIVATE Aver.Runtime.Game.Core Aver.Formats Aver.Physics Aver.Core)
set_target_properties(ProjectPhysicsApplyTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# LevelSequenceTest -- the level sequence samplers (interpolation, wrap, key round trips) and the player clock.
# Samplers are pure; the player case needs a real scene::World and nothing else.
add_executable(LevelSequenceTest src/LevelSequenceTest.cpp)
target_link_libraries(LevelSequenceTest PRIVATE Aver.Runtime.Game.Core Aver.Scene Aver.Core)
set_target_properties(LevelSequenceTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# [wave5/sequencer] SequenceTracksTest -- what each track kind writes into a real World (properties through the
# field registry, visibility, the base store, actor cameras and cuts, sub-sequences, events and audio with
# recording hooks); SequenceDirectorTest -- several sequences, script controls, lifecycle events, the camera owner.
add_executable(SequenceTracksTest src/SequenceTracksTest.cpp)
target_link_libraries(SequenceTracksTest PRIVATE Aver.Runtime.Game.Core Aver.Scene Aver.Core)
set_target_properties(SequenceTracksTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_executable(SequenceDirectorTest src/SequenceDirectorTest.cpp)
target_link_libraries(SequenceDirectorTest PRIVATE Aver.Runtime.Game.Core Aver.Scene Aver.Core)
set_target_properties(SequenceDirectorTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# PostVolumesTest -- the post-process volume blend: weights, per-field lerp, priority, the grading-LUT
# crossfade (against a fake resolver), and that every POSTVOLUME key reads back and re-applies as a no-op.
# Arithmetic over rhi::PostSettings only: no device, no window.
add_executable(PostVolumesTest src/PostVolumesTest.cpp)
target_link_libraries(PostVolumesTest PRIVATE Aver.Runtime.Game.Core Aver.Core)
set_target_properties(PostVolumesTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# GameLocalizationTest -- culture selection (override > saved setting > OS language > LOCALE.DEFAULT), bestCultureMatch, and
# content hot reload routed to the string and data-table stores. A temp project; no window, no device.
add_executable(GameLocalizationTest src/GameLocalizationTest.cpp)
target_link_libraries(GameLocalizationTest PRIVATE Aver.Runtime.Game.Core Aver.Framework Aver.Platform Aver.Core)
set_target_properties(GameLocalizationTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# [wave3/level-instances begin]
# LevelLightsTest -- game::spawnLevelLights and the light helpers that moved to Runtime (aver/game/LevelLights.hpp): the
# packaged game spawns LIGHT records now, so what a record means is pinned (ids, cone cosines, the component fields,
# gatherLights). A real scene::World; no device.
add_executable(LevelLightsTest src/LevelLightsTest.cpp)
target_link_libraries(LevelLightsTest PRIVATE Aver.Runtime.Game.Core Aver.Scene Aver.Formats Aver.Core)
set_target_properties(LevelLightsTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# [wave3/level-instances end]

# LevelComponentsTest -- game::applyPlacedComponent / capturePlacedComponent / applyLevelComponents (aver/game/LevelComponents.hpp):
# the COMPONENT record -> live Skeletal Mesh, Animator, Root Motion, Motion Matching, Control Rig, IK Rig, Brain, Smart Object and Soft
# Body components and back. A real scene::World; no device. The AI cases compile only where Aver.Synapse.Scene exists (it brings
# AVER_MODULE_SYNAPSE_SCENE); Game.Core links it PRIVATE, so the test links it itself.
add_executable(LevelComponentsTest src/LevelComponentsTest.cpp)
target_link_libraries(LevelComponentsTest PRIVATE Aver.Runtime.Game.Core Aver.Anim.Scene Aver.Scene Aver.Formats Aver.Core)
if(TARGET Aver.Synapse.Scene)
  target_link_libraries(LevelComponentsTest PRIVATE Aver.Synapse.Scene)
endif()
set_target_properties(LevelComponentsTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# GameLandscapeLayersTest -- GameLandscape's paint layers headless (wave 3, landscape layers): the by-name layer remap,
# ground samples, weight strokes with exact undo / redo, save and reload, carve, and the derived section / ring materials.
# A temp .ocland; no device, no window, no physics.
add_executable(GameLandscapeLayersTest src/GameLandscapeLayersTest.cpp)
target_link_libraries(GameLandscapeLayersTest PRIVATE Aver.Runtime.Game.Core Aver.Landscape Aver.Formats Aver.Platform Aver.Core)
set_target_properties(GameLandscapeLayersTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# GameAccessibilityTest -- the host wiring for game accessibility (Runtime/src/GameAccessibility.cpp): the post override,
# the camera wrapper, initAccessibility from a temp project, the per-frame caption list, and the pin that keeps
# Aver.Framework's key table and Aver.UI's default settings model the same list. No window, no device.
add_executable(GameAccessibilityTest src/GameAccessibilityTest.cpp)
target_link_libraries(GameAccessibilityTest PRIVATE Aver.Runtime.Game.Core Aver.Framework Aver.UI Aver.Platform Aver.Core)
set_target_properties(GameAccessibilityTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ItemLoaderTest -- game::ItemLoader, the mechanism half of LevelStreaming (LARGE_WORLD 2.4.1): a batch makes its entities under the right
# parents and units, a unit evicts alone, an entity the host made can be adopted and forgotten. A real scene::World and an empty
# GameContent with a stand-in device; no window, no GPU, no project.
add_executable(ItemLoaderTest src/ItemLoaderTest.cpp)
target_link_libraries(ItemLoaderTest PRIVATE Aver.Runtime.Game.Core Aver.World Aver.Scene Aver.Formats Aver.Core)
set_target_properties(ItemLoaderTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# GameCameraTest -- GameCamera.hpp (wave 10 gameplay cameras): pushCamera with a PlayView (the default is today's camera bit for
# bit; FOV and roll are inputs now), the legacy boom, and drivePlayCamera with and without a camera rig. A recording stand-in device
# and a real scene::World; no window, no GPU. The rig cases compile where Aver.Camera exists (it brings AVER_MODULE_CAMERA).
add_executable(GameCameraTest src/GameCameraTest.cpp)
target_link_libraries(GameCameraTest PRIVATE Aver.Runtime.Game.Core Aver.Framework Aver.Scene Aver.Core)
if(TARGET Aver.Camera)
  target_link_libraries(GameCameraTest PRIVATE Aver.Camera Aver.Physics)
endif()
set_target_properties(GameCameraTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# CameraRigRecordTest -- the COMPONENT CameraRig record (docs/CAMERAS.md): parse, apply, capture and the text round trip with golden
# canonical strings. Needs the camera module; a real scene::World; no device.
if(TARGET Aver.Camera)
  add_executable(CameraRigRecordTest src/CameraRigRecordTest.cpp)
  target_link_libraries(CameraRigRecordTest PRIVATE Aver.Runtime.Game.Core Aver.Camera Aver.Physics Aver.Scene Aver.Formats Aver.Core)
  set_target_properties(CameraRigRecordTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# PhysicalSurfacesTest -- Content/Physics/Surfaces.ocsurf -> the physics registry, an authored number beating a landscape layer's
# find-or-add, the landscape heightfield's per-cell surface palette against a brute-force argmax of the paint weights, and (with
# physics) rays on a painted landscape reporting each layer's surface (SHAPES_MATERIALS phase D). A temp project; no device.
# OriginParticipantsTest's WorldOrigin-with-fake-participants section (ORIGINTEST_WORLD) needs Aver.World. Deferred to the end of
# this directory so it works whichever order the OriginParticipantsTest target and this block land in.
function(_aver_originparticipants_world)
  if(TARGET OriginParticipantsTest AND TARGET Aver.World)
    target_compile_definitions(OriginParticipantsTest PRIVATE ORIGINTEST_WORLD=1)
    target_link_libraries(OriginParticipantsTest PRIVATE Aver.World)
  endif()
endfunction()
cmake_language(DEFER CALL _aver_originparticipants_world)
add_executable(PhysicalSurfacesTest src/PhysicalSurfacesTest.cpp)
target_link_libraries(PhysicalSurfacesTest PRIVATE Aver.Runtime.Game.Core Aver.Landscape Aver.Formats Aver.Platform Aver.Core)
if(TARGET Aver.Physics)
  target_link_libraries(PhysicalSurfacesTest PRIVATE Aver.Physics)
endif()
set_target_properties(PhysicalSurfacesTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# CollisionMeshSurfaceTest -- GameContent::collisionMeshFor with a material slot's PHYSSURFACE: the palette and per-triangle
# indices, the seam between two surfaces staying welded, no palette when no slot names a surface, the 32-surface cap, and the disk
# cache (round trip, a version-1 entry is a miss, an edited .ocmat makes an entry stale). A temp project; no device, no physics.
add_executable(CollisionMeshSurfaceTest src/CollisionMeshSurfaceTest.cpp)
target_link_libraries(CollisionMeshSurfaceTest PRIVATE Aver.Runtime.Game.Core Aver.Scene Aver.Formats Aver.Platform Aver.Core)
set_target_properties(CollisionMeshSurfaceTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ColliderShapesTest -- an .ocshape tree built into physics shapes (the chair, surfaces by leaf / default / mesh slot, hull mesh and mesh
# leaves through the injected mesh seam, decorators, a mutable compound, a refusal that leaves nothing allocated) and the CollisionShape
# component's apply / capture with golden keys (SHAPES_MATERIALS phase E). A real physics world and scene::World; no device.
if(TARGET Aver.Physics AND TARGET Aver.Scene)
  add_executable(ColliderShapesTest src/ColliderShapesTest.cpp)
  target_link_libraries(ColliderShapesTest PRIVATE Aver.Runtime.Game.Core Aver.Physics Aver.Scene Aver.Formats Aver.Platform Aver.Core)
  set_target_properties(ColliderShapesTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
