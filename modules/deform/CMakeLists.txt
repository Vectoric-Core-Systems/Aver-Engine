# Aver.Deform — sub-bone vertex deformation.
#
# OPTIONAL by construction (-DAVER_MODULE_DEFORM=OFF), because a game that only needs bone skinning
# should not carry a cage: nothing else links this, and not building it removes the feature and its
# cost entirely.
#
# Core, Formats and Anim only. Generating a cage, binding vertices to it and applying displacements
# is arithmetic on points, so all of it is decidable with no GPU -- which is what makes the CPU/GPU
# parity contract in docs/rendering/RENDERING.md 7.2 checkable at all. The GPU half, when it lands,
# has to reproduce this byte for byte.
aver_add_module(Aver.Deform
  SOURCES
    src/Cage.cpp
  DEPS
    Aver.Core
    Aver.Formats
    Aver.Anim
)
