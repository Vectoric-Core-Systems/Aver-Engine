# SoftBodySystemTest -- cloth and destruction on scene entities, headless. Meshes, .ocsoft assets and skin
# palettes are injected and nothing is drawn, so the whole system is decidable with no GPU and no device: a
# flag hanging and blowing, a cape following a palette, a dent that stays, a pane that shatters (Torn, then
# Detached, dust for the shards), explosions and cuts, debris caps, the rebuild budget, replay equality and the
# legacy CSoftBody path. Physics is the real Jolt world, through Aver.Physics.
add_executable(SoftBodySystemTest src/SoftBodySystemTest.cpp)
target_include_directories(SoftBodySystemTest PRIVATE src)
target_link_libraries(SoftBodySystemTest PRIVATE Aver.SoftBody.Scene Aver.Physics Aver.Scene Aver.SoftBody Aver.Formats Aver.Core)
set_target_properties(SoftBodySystemTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
