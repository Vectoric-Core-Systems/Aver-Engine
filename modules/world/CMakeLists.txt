# Aver.World -- the level/world runtime: .ocworld placements into live entities, and (later) the
# region/chunk streaming this module's README has always been scoped to.
#
# UNCONDITIONAL, and that is the point. Both hosts link it -- the editor and the game runtime -- and
# it is what stops "how a placement becomes an entity" from existing twice. A tree with
# AVER_MODULE_SCENE=OFF still configures and links it; LevelInstance.cpp simply compiles to nothing,
# the same way GameLevel.cpp does.
#
# NOT in Aver.Runtime.Game, deliberately: the editor would then depend on AVER_BUILD_GAME, and the
# root CMakeLists calls those two switches "opposites -- one is the editor, one is what a shipped
# game runs, and a tree may reasonably want either alone".
aver_add_module(Aver.World
  SOURCES
    src/LevelInstance.cpp
    src/ChunkPayload.cpp
    src/ChunkPartition.cpp
    src/ChunkCodec.cpp
    src/RegionFile.cpp
    src/RegionIndex.cpp
    src/ChunkSource.cpp
    src/ChunkStreamer.cpp
    src/ChunkGenerator.cpp
    src/ChunkWorld.cpp
  DEPS
    Aver.Core
    Aver.Formats
    # For aver::File -- ranged, non-truncating, durable. The region format is not expressible on
    # whole-file read/write, which is what docs/CHUNKS.md calls B1.
    Aver.Platform
    # For pcg::sampleInfinite -- the deterministic CPU noise the chunk generator is the first caller
    # and the first test of. RHI comes with it, which is why this sits at the end of the list.
    Aver.Render.Pcg
)

# The optional half of the link interface, mirroring modules/runtime.game/CMakeLists.txt.
#
# WHY if(TARGET ...) AND NOT if(AVER_MODULE_...): an unconditional `Aver.Scene` is a hard configure
# error in a tree built with AVER_MODULE_SCENE=OFF, because the target does not exist. Testing the
# target asks the same question in a form that cannot fail the configure.
#
# WHY THIS BLOCK IS LOAD-BEARING, which is not obvious: the AVER_MODULE_* macros are PUBLIC compile
# definitions on the module targets themselves, and they reach a translation unit ONLY through the
# link interface. Without these lines every `#if AVER_MODULE_*` in LevelInstance.cpp would be false
# however the tree was configured -- so the whole file would compile to nothing and the only symptom
# would be an empty world. modules/runtime.game hit exactly this and records it.
if(TARGET Aver.Scene)
  target_link_libraries(Aver.World PUBLIC Aver.Scene)
endif()
if(TARGET Aver.Physics)
  target_link_libraries(Aver.World PUBLIC Aver.Physics)
endif()

# Aver.Render.PBR is deliberately ABSENT. Resolving a surface name to a material handle is the
# HOST's job -- the game goes through GameContent, the editor through its own map -- and this module
# only tells them when to do it, through InstantiateOptions::bindMaterial. Taking the dependency
# would buy nothing and would put the material system into the dependency graph of a module that
# never touches one.
