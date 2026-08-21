# Aver.Save — turning a live scene::World into a .ocsave and back.
#
# A SECOND TARGET rather than an edge added to either side, for the reason Aver.Anim.Scene is one.
# Aver.Formats sits below Aver.Scene and must not reach it; Aver.Scene must not gain a Formats edge,
# because its components carry opaque ids precisely so nothing unmarshallable reaches the P/Invoke
# boundary. Turning one into the other therefore belongs in neither, and only a composition root
# links this.
#
# It does NOT link Aver.Framework. Spawning an actor by class name, recognising one, and destroying
# one through the path that runs OnEndPlay all arrive as host-installed function pointers — the same
# shape AnimSystem::setResolver and aver_fw_set_anim_curve_provider already use. This is also what
# makes the module testable: the tests install their own, and never boot a framework.
aver_add_module(Aver.Save
  SOURCES
    src/SaveWorld.cpp
  DEPS
    Aver.Formats
    Aver.Scene
    Aver.Core
)
