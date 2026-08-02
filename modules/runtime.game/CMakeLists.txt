# Aver.Runtime.Game -- the game half of what currently lives inside SandboxApp.
#
# Sandbox.exe IS the editor: it is the only aver::Application in the tree, and roughly two thousand
# lines of what a GAME needs are interleaved with dockspaces and inspector panels inside it. This
# library is where that half moves to, one subsystem per commit, so the editor keeps working while
# the game executable grows. The lift is a COPY: sandbox/src/SandboxApp.cpp is not edited by it, so
# the editor cannot regress. De-duplication is a later slice, proven by the gates staying identical.
#
# It links NO UI. That is the whole point of the module existing, and it is currently true by
# construction rather than by enforcement: ImGui is linked PUBLIC from Aver.RHI.D3D12, so an
# AVER_ENABLE_UI=ON tree propagates it here regardless. scripts/stage-game.ps1 is what refuses to
# ship a binary built that way, until the ImGui/RHI split makes it structural.
aver_add_module(Aver.Runtime.Game
  SOURCES
    src/GameApp.cpp
    src/GameContent.cpp
    src/GameLevel.cpp
  DEPS
    Aver.Runtime
    Aver.Platform
    Aver.Core
)

# The optional half of the link interface, mirroring sandbox/CMakeLists.txt.
#
# WHY if(TARGET ...) AND NOT if(AVER_MODULE_...). An unconditional `Aver.Scene` is a hard configure
# error in a tree built with AVER_MODULE_SCENE=OFF, because the target simply does not exist -- and a
# game tree is exactly the kind of tree someone trims. Testing the target is the same question asked
# in a form that cannot fail the configure.
#
# WHY THIS BLOCK IS NEEDED AT ALL, which is not obvious: the AVER_MODULE_* macros are PUBLIC compile
# definitions on the module targets themselves (AVER_MODULE_PBR=1 on Aver.Render.PBR, SCENE=1 on
# Aver.Scene, VOXI=1 on Aver.Render.Voxi). They reach a translation unit only through the link
# interface. Until this block existed, every #if AVER_MODULE_* inside GameApp.cpp was FALSE no
# matter how the tree was configured -- so lifted code would have compiled to nothing at all and the
# only symptom would have been a game that draws an empty world.
#
# Aver.RHI is deliberately absent: it is already PUBLIC through Aver.Runtime.
target_link_libraries(Aver.Runtime.Game PUBLIC Aver.Formats Aver.Render.Skin)

if(TARGET Aver.Render.PBR)
  target_link_libraries(Aver.Runtime.Game PUBLIC
    Aver.Render.PBR Aver.Render.PBR.Materials Aver.Formats.Material Aver.Assets.Gpu)
endif()
if(TARGET Aver.Render.Voxi)
  target_link_libraries(Aver.Runtime.Game PUBLIC Aver.Render.Voxi Aver.Render.Voxi.Renderer)
endif()
if(TARGET Aver.Scene)
  target_link_libraries(Aver.Runtime.Game PUBLIC Aver.Scene)
endif()
if(TARGET Aver.Anim.Scene)
  target_link_libraries(Aver.Runtime.Game PUBLIC Aver.Anim.Scene)
endif()
if(TARGET Aver.Physics)
  target_link_libraries(Aver.Runtime.Game PUBLIC Aver.Physics)
endif()
if(TARGET Aver.Framework)
  target_link_libraries(Aver.Runtime.Game PUBLIC Aver.Framework)
endif()
# The lift plan's C1 list omitted this while its own DONE-WHEN expected SCRIPTING=1, and the module
# report caught the contradiction on the first run. A game runs the project's C# gameplay, so it
# needs the CLR host as much as the editor does -- more, since it has no other way to run anything.
if(TARGET Aver.Scripting.Host)
  target_link_libraries(Aver.Runtime.Game PUBLIC Aver.Scripting.Host)
endif()
