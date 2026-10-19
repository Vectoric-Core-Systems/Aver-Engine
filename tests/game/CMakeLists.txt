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
