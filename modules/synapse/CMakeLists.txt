# Aver.Synapse — navigation and pathfinding, with no scene and no physics.
#
# PURE by construction: a baked grid and arithmetic. That is what lets the entire pathfinder be
# tested against a hand-authored grid with a wall in it, on a machine with no GPU and no world. The
# join to a live scene is Aver.Synapse.Scene one tier up — the same split Aver.Anim / Aver.Anim.Scene
# already uses, and for the reason that module's own CMakeLists states.
aver_add_module(Aver.Synapse
  SOURCES
    src/Nav.cpp
  DEPS
    Aver.Formats
    Aver.Core
)
