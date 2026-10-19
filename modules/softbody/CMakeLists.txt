# Aver.SoftBody -- the topology, damage and asset library behind Jolt-driven cloth and destructibles.
#
# Pure by construction: no scene, no GPU, no Jolt. The cage (particles, beams, dihedral bends,
# triangles), the material yield model, tear resolution into duplicate particles, island planning, the
# render binding that follows tears, the .ocsoft asset, the thick-break caps, the fields, the saved-damage codec and the
# proxy binding are all arithmetic on arrays, so
# tests/softbody decides them on a machine with no device. The simulation itself is Jolt's, driven by
# Aver.SoftBody.Scene through Aver.Physics' aver_phys_sb_* ABI.
#
# Needs Aver.Formats only for Json.hpp (.ocsoft) and the file helpers it brings in.
#
# Added by the root CMakeLists.txt and default-on; docs/SOFTBODY.md lists the rest.
aver_add_module(Aver.SoftBody
  SOURCES
    src/Cage.cpp
    src/RenderBinding.cpp
    src/SoftAsset.cpp
    src/SimMesh.cpp
    src/Damage.cpp
    src/Islands.cpp
    src/Caps.cpp
    src/Fields.cpp
    src/SoftState.cpp
    src/ProxyBind.cpp
  DEPS
    Aver.Core
    Aver.Formats
)

target_compile_definitions(Aver.SoftBody PUBLIC AVER_MODULE_SOFTBODY=1)
