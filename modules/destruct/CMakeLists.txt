# Aver.Destruct -- brittle rigid destruction at run time: an .ocfrac asset (Aver.Formats) becomes piece bodies in the physics C ABI.
#
# PURE BY CONSTRUCTION, like Aver.Vehicle: no scene, no GPU, no Jolt header. It links Aver.Physics for the C ABI only, so
# tests/destruct decides strain, anchors, splitting, debris limits and breakable joints in a real Jolt world with no device. The scene component and
# the render side (piece meshes, shared material, inside material) are a later layer over Destructible::piecePose.
#
# Added by the root CMakeLists.txt inside if(AVER_MODULE_PHYSICS), after modules/physics.
aver_add_module(Aver.Destruct
  SOURCES
    src/Destructible.cpp
  DEPS
    Aver.Core
    Aver.Formats
    Aver.Physics
  PUBLIC_DEFS
    AVER_MODULE_DESTRUCT=1
)
