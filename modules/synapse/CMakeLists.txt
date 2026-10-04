# Aver.Synapse — navigation and pathfinding, with no scene and no physics.
#
# PURE by construction: a baked grid and arithmetic. That is what lets the entire pathfinder be
# tested against a hand-authored grid with a wall in it, on a machine with no GPU and no world. The
# join to a live scene is Aver.Synapse.Scene one tier up — the same split Aver.Anim / Aver.Anim.Scene
# already uses, and for the reason that module's own CMakeLists states.
aver_add_module(Aver.Synapse
  SOURCES
    src/Nav.cpp
    src/NavBake.cpp
    # The behaviour-tree evaluator -- pure for the identical reason Nav.cpp is: a hand-built tree
    # and a pair of fake Condition/Action functions test the whole algorithm with no world at all.
    src/Bt.cpp
  DEPS
    Aver.Formats
    Aver.Core
)


# Carried PUBLIC so anything that links Synapse can #if on it, matching Aver.Physics. The editor
# needs it: SandboxApp holds a baked grid and an overlay handle, and both have to compile away
# entirely in a build configured without navigation.
target_compile_definitions(Aver.Synapse PUBLIC AVER_MODULE_SYNAPSE=1)
