# LevelInstanceTest -- the placement -> entity loop both hosts now share.
#
# It needs no .ocworld on disk (there is none in this repository), no GPU, no device and no .NET: the
# world is a string literal and every assertion is on entity state. That is what makes it runnable in
# the headless sweep alongside everything else.
add_executable(LevelInstanceTest src/LevelInstanceTest.cpp)
target_link_libraries(LevelInstanceTest PRIVATE Aver.World Aver.Scene Aver.Formats Aver.Core)
set_target_properties(LevelInstanceTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ChunkCoordTest -- the region -> chunk -> local-float hierarchy.
#
# Core ONLY, and that is worth keeping: ChunkCoord.hpp is a value header like Vec3, so if this test
# ever needs Scene or Formats to link, something has leaked into it that should not have.
add_executable(ChunkCoordTest src/ChunkCoordTest.cpp)
target_link_libraries(ChunkCoordTest PRIVATE Aver.World Aver.Core)
set_target_properties(ChunkCoordTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
