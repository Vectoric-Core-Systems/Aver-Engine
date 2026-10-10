# LevelInstanceTest -- the placement -> entity loop both hosts now share.
#
# It needs no .ocworld on disk (there is none in this repository), no GPU, no device and no .NET: the
# world is a string literal and every assertion is on entity state. That is what makes it runnable in
# the headless sweep alongside everything else.
add_executable(LevelInstanceTest src/LevelInstanceTest.cpp)
target_link_libraries(LevelInstanceTest PRIVATE Aver.World Aver.Scene Aver.Formats Aver.Core)
set_target_properties(LevelInstanceTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# AnimatedPlacementTest -- `anim` placements: the CAnimator instantiate attaches, the kinematic body a
# colliding one gets, driveKinematicBodies, and the physics ABI they stand on (a static body made
# kinematic or dynamic, aver_phys_body_move_kinematic). Physics comes in through Aver.World's link
# interface, like LevelInstanceTest's own.
add_executable(AnimatedPlacementTest src/AnimatedPlacementTest.cpp)
target_link_libraries(AnimatedPlacementTest PRIVATE Aver.World Aver.Scene Aver.Formats Aver.Core)
set_target_properties(AnimatedPlacementTest PROPERTIES
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

# ChunkStreamTest -- residency, a camera flight, and CHUNKED EQUALS FLAT.
#
# The last of those is what docs/CHUNKS.md section 10 calls the strongest single test in the plan,
# and the reason section 3.1 keeps flat loading permanently: delete the flat path and streaming can
# only be compared against itself.
add_executable(ChunkStreamTest src/ChunkStreamTest.cpp)
target_link_libraries(ChunkStreamTest PRIVATE Aver.World Aver.Scene Aver.Formats Aver.Platform Aver.Core)
set_target_properties(ChunkStreamTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# RegionWriteTest -- runtime mutation: the sector allocator, the free list, crash safety, compaction.
#
# The crash case is checked at EVERY sector boundary rather than a sampled few, because a crash does
# not sample. The property is not "a truncated region opens" but "a truncated region never serves
# something plausible and wrong".
add_executable(RegionWriteTest src/RegionWriteTest.cpp)
target_link_libraries(RegionWriteTest PRIVATE Aver.World Aver.Scene Aver.Formats Aver.Platform Aver.Core)
set_target_properties(RegionWriteTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ChunkGenerateTest -- generate-as-you-go, and the CROSS-PROCESS round trip slice 5 owed.
#
# It re-executes itself. That is not exotic, it is the only way to check the thing that matters: the
# five process-local values a chunk must never carry all round-trip perfectly INSIDE the process that
# minted them, so a same-process test passes on a file nothing else can load.
add_executable(ChunkGenerateTest src/ChunkGenerateTest.cpp)
target_link_libraries(ChunkGenerateTest PRIVATE
  Aver.World Aver.Scene Aver.Formats Aver.Platform Aver.Render.Pcg Aver.Core)
set_target_properties(ChunkGenerateTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ChunkActorStreamTest -- actors as streaming sources: predictive load and the boundary hold.
#
# It has a CONTROL: the same flight with the lead and the budget removed must FAIL to keep up. Without
# that, "never arrives unloaded" could just mean the radius was generous and the prediction untested.
add_executable(ChunkActorStreamTest src/ChunkActorStreamTest.cpp)
target_link_libraries(ChunkActorStreamTest PRIVATE
  Aver.World Aver.Scene Aver.Formats Aver.Platform Aver.Render.Pcg Aver.Core)
set_target_properties(ChunkActorStreamTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ChunkWorldTest -- ChunkWorld, the generator+override+streamer object bound together and driven the
# way a host actually would: one call a frame with the viewer's position and velocity.
#
# Not a re-run of ChunkActorStreamTest -- that test proves the STREAMER's own residency and
# boundary-hold properties. This one proves ChunkWorld does not lose or misrepresent any of that once
# it is the thing assembling the pieces, and that its owned-entity view never drifts from the
# streamer's own resident-entity count across a whole flight.
add_executable(ChunkWorldTest src/ChunkWorldTest.cpp)
target_link_libraries(ChunkWorldTest PRIVATE
  Aver.World Aver.Scene Aver.Formats Aver.Platform Aver.Render.Pcg Aver.Core)
set_target_properties(ChunkWorldTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(ChunkCoordTest src/ChunkCoordTest.cpp)
target_link_libraries(ChunkCoordTest PRIVATE Aver.World Aver.Core)
set_target_properties(ChunkCoordTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ScatterPaletteTest -- OcScatterSpecies -> ScatterSpecies, the conversion both the editor and the
# game runtime call (see modules/world/include/aver/world/ScatterPalette.hpp), and the validation
# that names a bad species rather than letting a level silently scatter nothing.
add_executable(ScatterPaletteTest src/ScatterPaletteTest.cpp)
target_link_libraries(ScatterPaletteTest PRIVATE
  Aver.World Aver.Scene Aver.Formats Aver.Platform Aver.Core)
set_target_properties(ScatterPaletteTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# InfiniteHeightFieldTest -- pcg::sampleInfiniteHeight/sampleInfiniteHeightCm (modules/render.pcg) and
# their wiring into GeneratorSettings::heightSource via makeInfiniteHeightSource: determinism across
# call order, continuity across a chunk boundary (with sampleInfinite itself as the jump-having
# control), and that leaving heightSource unset stays byte-identical to today's flat generator.
add_executable(InfiniteHeightFieldTest src/InfiniteHeightFieldTest.cpp)
target_link_libraries(InfiniteHeightFieldTest PRIVATE
  Aver.World Aver.Scene Aver.Formats Aver.Platform Aver.Render.Pcg Aver.Core)
set_target_properties(InfiniteHeightFieldTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ScatterBudgetTool -- not a test. Runs the real generator against a level's real SCATTER records and
# real cooked meshes and reports a measured resident-triangle count for a streaming radius, for the
# same reason LevelInspect (tests/formats) and ActorSweep exist: a fixture agrees with its own parser
# by construction, and this answers a question about a REAL level that a synthetic test cannot -- see
# the file's own header comment for why this exists alongside (not instead of) an editor screenshot.
#
#     ScatterBudgetTool.exe "<project>\Content\Maps\Default.ocmap" "<project>\Content" [radius]
add_executable(ScatterBudgetTool src/ScatterBudgetTool.cpp)
target_link_libraries(ScatterBudgetTool PRIVATE
  Aver.World Aver.Scene Aver.Formats Aver.Platform Aver.Core)
set_target_properties(ScatterBudgetTool PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# VehicleLaneTest -- the vehicle driver's lane maths (world::lanes in VehicleSystem.hpp): projection onto a
# lane, which lane a car belongs to, looking ahead across a lane boundary, the curvature speed cap,
# pure-pursuit steering, the leader-gap and pedal policy.
#
# Pure maths, so no world and no physics: the Jolt vehicle half of the system needs the physics ABI's
# vehicle entry points and is covered where those are (tests/physics).
add_executable(VehicleLaneTest src/VehicleLaneTest.cpp)
target_link_libraries(VehicleLaneTest PRIVATE Aver.World Aver.Formats Aver.Core)
set_target_properties(VehicleLaneTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# PlacementStreamerTest -- distance residency of authored placements (pure logic).
add_executable(PlacementStreamerTest src/PlacementStreamerTest.cpp)
target_link_libraries(PlacementStreamerTest PRIVATE Aver.World Aver.Core)
set_target_properties(PlacementStreamerTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
