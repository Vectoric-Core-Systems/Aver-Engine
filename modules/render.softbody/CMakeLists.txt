# Aver.Render.SoftBody -- drawing a mesh whose vertices came from Jolt instead of from its asset.
#
# SEPARATE FROM Aver.Render.Skin even though it mirrors SkinnedScene almost line for line, because
# the two have genuinely different dependencies: skinning needs Aver.Anim and a compute shader,
# this needs Aver.Physics and no shader at all (the vertices arrive on the CPU already simulated).
# Folding them together would make every tree that wants one link the other.
#
# DEPENDS ON Aver.Physics, which is the one thing worth noticing on this line: it means the soft
# body's SIMULATION and its DRAWING are joined here rather than in a composition root. That is the
# opposite of the split Aver.Water makes, and the reason is that there is nothing to decouple -- the
# vertices this module uploads have no source other than the physics module, so pretending otherwise
# would buy an indirection with no second implementer behind it.
aver_add_module(Aver.Render.SoftBody
  SOURCES
    src/SoftBodyScene.cpp
  DEPS
    Aver.Core
    Aver.RHI
    Aver.Scene
    Aver.Formats
    Aver.Physics
  PUBLIC_DEFS
    AVER_MODULE_RENDER_SOFTBODY=1
)
