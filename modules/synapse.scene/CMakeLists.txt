# Aver.Synapse.Scene — the join between the pure pathfinder and a live scene::World.
#
# A SECOND TARGET rather than an edge added to either side, for the reason Aver.Anim.Scene is one
# (see that module's own CMakeLists comment). Aver.Synapse stays pure grid-and-arithmetic so it can
# be tested against a hand-authored grid with no world at all; this module is what turns a goal into
# an actual entity's path.
aver_add_module(Aver.Synapse.Scene
  SOURCES
    src/SynapseAgent.cpp
    src/SynapsePerception.cpp
    src/SynapseBt.cpp
  DEPS
    Aver.Synapse
    Aver.Scene
    Aver.Core
)

# Physics, for SynapsePerception's own occlusion raycast -- OPTIONAL and plain DEPS, not a
# function-pointer relay: unlike Aver.Framework (which this module must never link, see
# aver_fw_set_synapse_target_provider's own comment in framework_abi.h), nothing states or implies
# the same constraint for Aver.Physics, and Aver.World already links it directly the identical way
# (modules/world/CMakeLists.txt) for the same "a join-tier module reading a small standalone C ABI"
# reason.
if(TARGET Aver.Physics)
  target_link_libraries(Aver.Synapse.Scene PUBLIC Aver.Physics)
endif()

# Carried PUBLIC for the same reason Aver.Synapse's own definition is: a composition root needs to
# #if around CSynapseAgent registration and the per-frame tick in a build configured without
# navigation at all.
target_compile_definitions(Aver.Synapse.Scene PUBLIC AVER_MODULE_SYNAPSE_SCENE=1)
