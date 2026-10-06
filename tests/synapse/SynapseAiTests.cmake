# Steering, crowds, hearing, cover and squads. Pulled in from tests/synapse/CMakeLists.txt with
#   include(${CMAKE_CURRENT_LIST_DIR}/SynapseAiTests.cmake)
# so the targets below are all this feature adds to that file.

# SteeringTest / CrowdTest / HearingTest / CoverTest -- pure arithmetic against hand-drawn grids.
# Like NavTest they link no scene and no physics.
foreach(_t SteeringTest CrowdTest HearingTest CoverTest)
  add_executable(${_t} src/${_t}.cpp)
  target_link_libraries(${_t} PRIVATE Aver.Synapse Aver.Formats Aver.Core Aver.Platform)
  set_target_properties(${_t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endforeach()

# SynapseAiSceneTest -- the systems against a real scene::World (hearing with a fake occlusion,
# crowds in both drive modes, cover markers, squads, the behaviour-tree actions).
if(TARGET Aver.Synapse.Scene)
  add_executable(SynapseAiSceneTest src/SynapseAiSceneTest.cpp)
  target_link_libraries(SynapseAiSceneTest PRIVATE Aver.Synapse.Scene Aver.Synapse Aver.Scene
                                                   Aver.Formats Aver.Core Aver.Platform)
  set_target_properties(SynapseAiSceneTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
