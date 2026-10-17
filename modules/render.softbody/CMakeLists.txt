# Aver.Render.SoftBody -- drawing what the soft-body system simulates, in place of the authored mesh.
#
# SEPARATE FROM Aver.Render.Skin even though it mirrors SkinnedScene almost line for line, because the two
# have genuinely different dependencies: skinning needs Aver.Anim and a compute shader, this needs the
# soft-body system and no shader at all (the vertices arrive on the CPU already simulated).
#
# DRAW-ONLY. The simulation, the tears and the pieces belong to Aver.SoftBody.Scene (which owns every Jolt
# soft body that belongs to an entity); this module reads the PieceViews it publishes. Aver.SoftBody.Scene
# brings the physics ABI with it, which is why Aver.Physics is no longer named here.
aver_add_module(Aver.Render.SoftBody
  SOURCES
    src/SoftBodyScene.cpp
  DEPS
    Aver.Core
    Aver.RHI
    Aver.Scene
    Aver.Formats
    Aver.SoftBody
    Aver.SoftBody.Scene
  PUBLIC_DEFS
    AVER_MODULE_RENDER_SOFTBODY=1
)
