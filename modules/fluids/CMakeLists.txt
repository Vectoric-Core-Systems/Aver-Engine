# Aver.Fluids -- everything this engine knows about liquid, in one module: the analytic Gerstner
# surface and its underwater fog override, the simulated fluid VOLUME (a CPU position-based particle
# solver plus its surface mesher), and the rendering of both. See README.md.
#
# ONE MODULE BECAUSE IT IS ONE SUBJECT. This was Aver.Water and Aver.Render.Fluid, split along a line
# that turned out to describe the implementation rather than the domain: an author placing a WATER
# record does not care whether the surface they get is evaluated in a vertex shader or stepped by a
# solver, and the two shared a format record, a composition-root call site and a set of colours
# already. Keeping them apart meant a "which module owns this?" question on every change that touched
# both, and a name (Water) narrower than what the code does -- it simulates a volume of arbitrary
# compliance and pressure, which is a fluid, not specifically water.
#
# THE PHYSICS DEPENDENCY IS SCOPED, NOT INHERITED WHOLESALE, and that is the one thing this merge had
# to be careful about. Aver.Water's own README said it "must never learn what Jolt is", and that was
# not decoration: it is what let the wave math be tested with no world at all and what kept this
# buildable in a tree with the scene or the physics off. Merging naively would have made the Gerstner
# grid unbuildable without Jolt for no reason. So FluidScene.cpp -- the only half that touches the
# physics world (the overlap query that voxelises scene colliders) -- is added ONLY when physics is
# present, and the analytic surface, the underwater override, FluidVolume's desc/preview arithmetic and
# the whole particle solver and mesher (PbfFluid, FluidSurface: pure CPU, no physics, no device) build
# without it, which is also what lets tests/fluids run the solver with nothing else linked.
set(_fluids_sources
  src/WaterRenderer.cpp
  # The displaced mesh surface (compute-waved grid drawn through the material path); no physics.
  src/WaterSurface.cpp
  src/Underwater.cpp
  src/FluidVolume.cpp
  # The position-based fluid solver, its static-collider distance field and the surface mesher.
  src/PbfFluid.cpp
  src/FluidSurface.cpp
  # Spline rivers and closed-outline lakes: the CPU spline, their meshes and the water point query.
  src/WaterBody.cpp
)
set(_fluids_deps Aver.Core Aver.RHI)

if(AVER_MODULE_PHYSICS)
  # The join between a simulated volume and the rasteriser (it also asks the physics world which
  # colliders the volume touches). render.softbody's CMakeLists sets the precedent: a module may own
  # both the simulation seam and the drawing, because the alternative is an indirection through a
  # composition root with no second implementer behind it.
  list(APPEND _fluids_sources src/FluidScene.cpp)
  list(APPEND _fluids_deps Aver.Physics)
endif()

aver_add_module(Aver.Fluids
  SOURCES
    ${_fluids_sources}
  DEPS
    ${_fluids_deps}
  PUBLIC_DEFS
    AVER_MODULE_FLUIDS=1
)

# A SECOND, NARROWER DEFINE, because "the fluids module exists" and "the fluids module can simulate"
# are different questions and callers ask both. Guarding a FluidScene call site on AVER_MODULE_FLUIDS
# alone would compile it into a physics-less build where FluidScene.cpp was never added -- a link
# error at best. Everything touching FluidScene guards on this one instead.
if(AVER_MODULE_PHYSICS)
  target_compile_definitions(Aver.Fluids PUBLIC AVER_FLUIDS_SIMULATED=1)
endif()

# HLSL beside the executable -- see cmake/AvModule.cmake.
aver_deploy_shaders(Aver.Fluids "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
