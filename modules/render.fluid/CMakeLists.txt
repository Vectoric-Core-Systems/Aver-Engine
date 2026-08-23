# Aver.Render.Fluid -- drawing a water::FluidVolume whose shape came from Jolt instead of from an
# authored mesh.
#
# DEPENDS ON BOTH Aver.Water AND Aver.Physics, which is the one thing worth noticing on this line.
# modules/render.softbody/CMakeLists.txt sets the precedent for exactly this decision, joining a
# soft body's SIMULATION and its DRAWING in one module "rather than in a composition root", because
# "the vertices this module uploads have no source other than the physics module". This module makes
# the identical argument with water standing in on the third side: Aver.Water's own README says
# outright that it "must never learn what Jolt is", and modules/physics keeps JPH:: private to its
# own target so the backend stays swappable -- so the two must not reach for each other directly, and
# yet something has to hand water's seed shell to the solver and the solver's output back to a
# renderer. Pretending that seam belongs in a composition root instead would buy an indirection with
# no second implementer behind it, the same conclusion render.softbody's own comment reaches.
#
# WHAT THIS DOES NOT DRAG TOGETHER. Unlike its render.softbody sibling, this module depends on
# NEITHER Aver.Scene NOR Aver.Formats. Aver.Scene is absent because a fluid volume is not discovered
# by walking a world for a component the way a soft-body entity is -- it is spawned directly from a
# water::FluidVolumeDesc by whoever wants one, so there is no ECS discovery for this module to own.
# Aver.Formats is absent because a fluid volume's shell is generated arithmetic
# (water::generateFluidSeedShell), never a decoded .ocmesh, so there is no asset to load and nothing
# here ever opens a file.
aver_add_module(Aver.Render.Fluid
  SOURCES
    src/FluidScene.cpp
  DEPS
    Aver.Core
    Aver.RHI
    Aver.Water
    Aver.Physics
  PUBLIC_DEFS
    AVER_MODULE_RENDER_FLUID=1
)
