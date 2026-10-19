# Aver.Camera -- gameplay cameras: spring-arm rigs, blended view targets and the modifier stack on scene entities.
#
# The pure half (types, math, modifiers, presets) is header-only and needs Core alone; the system half owns the
# CCameraRig component, probes with the physics ABI (aver_phys_sphere_cast_q) and reads world transforms from the scene.
# Only a composition root links it (Runtime.Game.Core, the editor); the render side never sees it -- the hosts hand
# the matrices it produced to IDevice::setCamera as before. Design and API: docs/CAMERAS.md.
aver_add_module(Aver.Camera
  SOURCES
    src/CameraRigSystem.cpp
    src/CameraRigEval.cpp
    src/CameraRigProvider.cpp
  DEPS
    Aver.Physics
    Aver.Scene
    Aver.Core
  PUBLIC_DEFS
    AVER_MODULE_CAMERA=1
)
