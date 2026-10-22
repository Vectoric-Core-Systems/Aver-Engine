# Aver.Hair.Scene -- strand hair on scene entities, over Jolt's Hair.
#
# A SECOND TARGET above Aver.Hair for the reason Aver.SoftBody.Scene sits above Aver.SoftBody: the pure library (the .ochair groom, the generator,
# curve import, the settings builder) stays Core + Formats + the physics C ABI, while owning a physics hair per entity needs the scene (components,
# transforms, the draw child), the animation system (the skeleton and the skinning palette of the scalp's joints) and the physics ABI for the
# collision proxies. Only a composition root links it; Aver.Render.Hair links it to draw what it publishes (HairView).
#
# Added by the root CMakeLists.txt behind AVER_MODULE_HAIR_SCENE; docs/HAIR.md.
aver_add_module(Aver.Hair.Scene
  SOURCES
    src/HairSystem.cpp
    src/HairProxies.cpp
    src/HairProvider.cpp
  DEPS
    Aver.Hair
    Aver.Physics
    Aver.Scene
    Aver.Anim.Scene
    Aver.Anim
    Aver.Formats
    Aver.Core
  PUBLIC_DEFS
    AVER_MODULE_HAIR_SCENE=1
)
