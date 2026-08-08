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
# ChunkPartitionTest -- scene -> chunks -> payload -> scene, and prove nothing moved.
#
# The precursor to docs/CHUNKS.md's "chunked equals flat", which is the strongest test in the whole
# streaming plan. Needs Aver.Scene because it builds a real world; still no renderer, no GPU, no file.
add_executable(ChunkPartitionTest src/ChunkPartitionTest.cpp)
target_link_libraries(ChunkPartitionTest PRIVATE Aver.World Aver.Scene Aver.Formats Aver.Core)
set_target_properties(ChunkPartitionTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# RegionFileTest -- the .avrgn archive and its .ocindex, against real files.
#
# Real files for the same reason FileRangeTest uses them: the properties being checked are about what
# is actually on disk. Every one is baseline-free -- cook determinism by memcmp, round trip by
# comparison, corruption by refusal -- so none of it needs a recorded reference to be meaningful.
add_executable(RegionFileTest src/RegionFileTest.cpp)
target_link_libraries(RegionFileTest PRIVATE Aver.World Aver.Scene Aver.Formats Aver.Platform Aver.Core)
set_target_properties(RegionFileTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(ChunkCoordTest src/ChunkCoordTest.cpp)
target_link_libraries(ChunkCoordTest PRIVATE Aver.World Aver.Core)
set_target_properties(ChunkCoordTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
