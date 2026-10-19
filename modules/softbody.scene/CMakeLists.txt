# Aver.SoftBody.Scene -- cloth and destruction on scene entities, over Jolt soft bodies.
#
# A SECOND TARGET above Aver.SoftBody for the reason Aver.Anim.Scene sits above Aver.Anim: the pure library
# (cage, damage, islands, the .ocsoft asset) must stay Core + Formats, while owning a body per entity needs the
# scene (components, transforms), the physics ABI (aver_phys_sb_*) and the animation system (the skin palette).
# Only a composition root links it; Aver.Render.SoftBody links it to draw what it publishes.
aver_add_module(Aver.SoftBody.Scene
  SOURCES
    src/SoftBodySystem.cpp
    src/SoftBodyBuild.cpp
    src/SoftBodyDamage.cpp
    src/SoftBodyCloth.cpp
    src/SoftBodyProvider.cpp
    # Wave 10 (docs/plans/wave10/SOFTBODY_DEPTH.md): each a new translation unit so the older ones do not grow.
    src/SoftBodyTear.cpp
    src/SoftBodyFields.cpp
    src/SoftBodyCaps.cpp
    src/SoftBodyFx.cpp
    src/SoftBodySave.cpp
    src/SoftBodyProxy.cpp
  DEPS
    Aver.SoftBody
    Aver.Physics
    Aver.Scene
    Aver.Anim.Scene
    Aver.Anim
    Aver.Formats
    Aver.Core
  PUBLIC_DEFS
    AVER_MODULE_SOFTBODY_SCENE=1
)
