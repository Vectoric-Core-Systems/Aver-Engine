# NavTest — the grid, the regions and the search, against hand-authored ASCII maps.
#
# Links no scene, no physics and no device: the point of Aver.Synapse being pure is that its
# arithmetic can be checked against a picture rather than against a screenshot.
add_executable(NavTest src/NavTest.cpp)
target_link_libraries(NavTest PRIVATE Aver.Synapse Aver.Formats Aver.Core Aver.Platform)
set_target_properties(NavTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)


# BtTest -- the behaviour-tree evaluator, against hand-built trees and fake Condition/Action
# functions. Links no scene, matching NavTest's own reasoning: the algorithm (composites, Cooldown,
# Running-state resume) is checked with no world at all.
add_executable(BtTest src/BtTest.cpp)
target_link_libraries(BtTest PRIVATE Aver.Synapse Aver.Formats Aver.Core Aver.Platform)
set_target_properties(BtTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# NavBakeTest -- turning a world into a grid, against a world made of arithmetic.
#
# Links no physics either. bakeNav takes two function pointers rather than calling
# aver_phys_raycast, so the "level" this test bakes is a floor function and a headroom function
# written out at the top of the file: a ramp, a cliff, a wall and a hole, all checkable by hand.
add_executable(NavBakeTest src/NavBakeTest.cpp)
target_link_libraries(NavBakeTest PRIVATE Aver.Synapse Aver.Formats Aver.Core Aver.Platform)
set_target_properties(NavBakeTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# AgentTest -- CSynapseAgent + AgentSystem::tick, against a real scene::World. The one test in this
# directory that links Aver.Scene: everything above stays pure so ITS tests can run with no world at
# all, and this is the "somebody" that proves the join between pathfinding and an actual entity.
if(TARGET Aver.Synapse.Scene)
  add_executable(AgentTest src/AgentTest.cpp)
  target_link_libraries(AgentTest PRIVATE Aver.Synapse.Scene Aver.Synapse Aver.Scene Aver.Formats
                                          Aver.Core Aver.Platform)
  set_target_properties(AgentTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# PerceptionTest -- CSynapsePerception + PerceptionSystem::tick, against a REAL Jolt world: the
# occlusion check is the whole point, and Aver.Synapse has no raycast of its own to fake it with.
if(TARGET Aver.Synapse.Scene AND TARGET Aver.Physics)
  add_executable(PerceptionTest src/PerceptionTest.cpp)
  target_link_libraries(PerceptionTest PRIVATE Aver.Synapse.Scene Aver.Synapse Aver.Scene
                                               Aver.Physics Aver.Formats Aver.Core Aver.Platform)
  set_target_properties(PerceptionTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# BtSystemTest -- CSynapseBehavior + BtSystem::tick, against a real scene::World: the real built-in
# Wait action, a real .ocbt file round-tripped through disk, and a recycled entity handle that must
# not inherit the previous occupant's Running state.
if(TARGET Aver.Synapse.Scene)
  add_executable(BtSystemTest src/BtSystemTest.cpp)
  target_link_libraries(BtSystemTest PRIVATE Aver.Synapse.Scene Aver.Synapse Aver.Scene Aver.Formats
                                             Aver.Core Aver.Platform)
  set_target_properties(BtSystemTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
