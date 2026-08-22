# Aver.Water -- a Gerstner-wave water surface drawn through the transparent pass, and the fog
# override that makes being under it look like being under it. See README.md.
#
# NO Aver.Scene DEPENDENCY, unlike Aver.Particles beside it, and the difference is the point: a
# particle emitter is a scene component and nothing simulates particles without one, whereas water in
# this slice is a single global plane owned by whichever host registers the render feature. That
# keeps this module buildable in a tree configured with the scene off, and it is also what lets the
# wave math be tested with no world at all.
#
# THE BUOYANCY HALF IS NOT HERE. It lives in Aver.Physics (modules/physics/src/Buoyancy.*), because
# it is Jolt's ApplyBuoyancyImpulse and this module must not learn what Jolt is -- modules/physics's
# own CMakeLists keeps JPH:: private to that target, and reaching across would break the one rule
# that makes the physics backend swappable. The two halves meet in the composition root, which knows
# both, and nowhere else.
aver_add_module(Aver.Water
  SOURCES
    src/WaterRenderer.cpp
    src/Underwater.cpp
  DEPS
    Aver.Core
    Aver.RHI
  PUBLIC_DEFS
    AVER_MODULE_WATER=1
)
