# Aver.Synapse.Scene — the join between the pure pathfinder and a live scene::World.
#
# A SECOND TARGET rather than an edge added to either side, for the reason Aver.Anim.Scene is one
# (see that module's own CMakeLists comment). Aver.Synapse stays pure grid-and-arithmetic so it can
# be tested against a hand-authored grid with no world at all; this module is what turns a goal into
# an actual entity's path.
aver_add_module(Aver.Synapse.Scene
  SOURCES
    src/SynapseAgent.cpp
  DEPS
    Aver.Synapse
    Aver.Scene
    Aver.Core
)

# Carried PUBLIC for the same reason Aver.Synapse's own definition is: a composition root needs to
# #if around CSynapseAgent registration and the per-frame tick in a build configured without
# navigation at all.
target_compile_definitions(Aver.Synapse.Scene PUBLIC AVER_MODULE_SYNAPSE_SCENE=1)
