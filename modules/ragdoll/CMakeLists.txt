# Aver.Ragdoll -- skinned entities that go limp: a skeleton's bind pose becomes a physics ragdoll,
# and the simulated pose is written back over the animated one through AnimSystem's pose-modifier seam.
#
# A SECOND TIER ABOVE Aver.Anim.Scene for the reason that module is one: it needs the scene (to find
# the entity), the animation system (to hook its pose) and the physics ABI (to build the bodies),
# and none of those three may learn about the others. Only a composition root links it.
aver_add_module(Aver.Ragdoll
  SOURCES
    src/RagdollBuilder.cpp
    src/PhysAnimProfile.cpp
    src/HitReaction.cpp
    src/RagdollLife.cpp
    src/RagdollSystem.cpp
  DEPS
    Aver.Anim.Scene
    Aver.Anim
    Aver.Scene
    Aver.Formats
    Aver.Physics
    Aver.Core
  # Carried PUBLIC so a composition root can #if around the install and the per-frame tick in a
  # build configured without ragdolls.
  PUBLIC_DEFS
    AVER_MODULE_RAGDOLL=1
)
