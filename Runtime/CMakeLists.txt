# Aver.Runtime.Game -- the runtime library, in Runtime/ (was modules/runtime.game until 2026-09-16).
#
# Two hosts are meant to share it: the standalone AverEngineRuntime.exe (Runtime/host) and, as its own
# private copy inside SandboxApp is retired subsystem by subsystem, the editor. What follows is how the
# library began, when it was still only a copy.
#
# The game half of what currently lives inside SandboxApp.
#
# Sandbox.exe IS the editor: it is the only aver::Application in the tree, and roughly two thousand
# lines of what a GAME needs are interleaved with dockspaces and inspector panels inside it. This
# library is where that half moves to, one subsystem per commit, so the editor keeps working while
# the game executable grows. The lift is a COPY: sandbox/src/SandboxApp.cpp is not edited by it, so
# the editor cannot regress. De-duplication is a later slice, proven by the gates staying identical.
#
# It links NO UI, and now that is true BY CONSTRUCTION, not merely by scripts/stage-game.ps1's
# refusal. Aver.RHI.D3D12 no longer links imgui at all, in any configuration (see its own
# CMakeLists.txt and include/aver/rhi/d3d12/UiBackend.hpp) -- the concrete Dear ImGui implementation
# lives in the separate Aver.RHI.D3D12.ImGui module, which only Sandbox links. An AVER_ENABLE_UI=ON
# tree's AverEngineRuntime.exe is therefore clean regardless of this switch.
#
# VERIFIED BY BYTE-SCANNING THE BINARY, not by dumpbin /imports and not inferred from the CMake edit.
# The distinction matters and cost a review round to pin down: imgui is a STATIC library, so neither
# executable ever carries an imgui*.dll import entry whether or not ImGui code is compiled in --
# dumpbin /imports answers a different question and would have passed either way. Scanning the built
# files for the symbol/string names themselves is what actually settles it. From one
# AVER_ENABLE_UI=ON tree: Sandbox.exe has ImGui_ImplDX12_Init x3, ImGui_ImplWin32_Init x2,
# "Dear ImGui" x6 and "docking" x30; AverGame.exe (the runtime's name then) has zero of each, and its single case-insensitive
# "imgui" hit is the embedded PDB path naming the build directory, not code.
#
# TWO LIBRARIES. Aver.Runtime.Game.Core holds the components both hosts are to share -- content,
# level, the world draw, input, terrain, water, streaming -- and is built whenever either host is.
# Aver.Runtime.Game holds GameApp alone, the application the standalone host runs. The editor links
# Core and never Aver.Runtime.Game, so GameApp.cpp cannot be pulled into Sandbox.exe: it defines
# stb_image_write's implementation, as SandboxApp.cpp does, and a static library pulls an object in
# as soon as anything names one of its symbols.
aver_add_module(Aver.Runtime.Game.Core
  SOURCES
    src/GameContent.cpp
    src/GameLevel.cpp
    src/GameFoliage.cpp
    src/GameLandscape.cpp
    src/GameRender.cpp
    src/GameInput.cpp
    src/GameStreaming.cpp
    src/GameWater.cpp
    src/MouseCapture.cpp
    src/PlayMobility.cpp
  DEPS
    Aver.RHI
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
target_link_libraries(Aver.Runtime.Game.Core PUBLIC Aver.Formats Aver.Render.Skin Aver.Render.Pcg)
# Aver.World owns the placement -> entity loop that GameLevel.cpp and SandboxApp.cpp each used to
# carry a copy of. This is the de-duplication this file's header called "a later slice, proven by
# the gates staying identical".
target_link_libraries(Aver.Runtime.Game.Core PUBLIC Aver.World)
# Terrain (GameLandscape.cpp). The .Renderer target PUBLIC-links Aver.Landscape, which carries
# AVER_MODULE_LANDSCAPE=1 -- the same single link sandbox/CMakeLists.txt uses.
if(TARGET Aver.Landscape.Renderer)
  target_link_libraries(Aver.Runtime.Game.Core PUBLIC Aver.Landscape.Renderer)
endif()
# Level water (GameWater.cpp): the analytic surface, and the simulated volume, which Aver.Fluids
# itself enables (AVER_FLUIDS_SIMULATED) only when physics is in the tree.
if(TARGET Aver.Fluids)
  target_link_libraries(Aver.Runtime.Game.Core PUBLIC Aver.Fluids)
endif()

if(TARGET Aver.Render.PBR)
  target_link_libraries(Aver.Runtime.Game.Core PUBLIC
    Aver.Render.PBR Aver.Render.PBR.Materials Aver.Formats.Material Aver.Assets.Gpu)
endif()
if(TARGET Aver.Render.Voxi)
  target_link_libraries(Aver.Runtime.Game.Core PUBLIC Aver.Render.Voxi Aver.Render.Voxi.Renderer)
endif()
if(TARGET Aver.Scene)
  target_link_libraries(Aver.Runtime.Game.Core PUBLIC Aver.Scene)
endif()
# meshoptimizer (MIT): collisionMeshFor's simplification step (GameContent.cpp), which is scene-gated
# in the source (`#if AVER_MODULE_SCENE`) because collision triangles are a scene concept -- so the
# link is gated on the same target rather than being unconditional, and a tree built with
# AVER_MODULE_SCENE=OFF never needs third_party/meshoptimizer vendored at all.
#
# THE SAME VENDORED TARGET modules/trifactor/CMakeLists.txt already builds as `meshoptimizer` -- see
# that file's own comment for why this repo vendors rather than fetches it. `if(NOT TARGET
# meshoptimizer)` is what lets both modules add that identical vendored subdirectory without either
# one failing on a second add_subdirectory of the same path, whichever configures first.
if(TARGET Aver.Scene)
  if(NOT TARGET meshoptimizer)
    if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/meshoptimizer/CMakeLists.txt)
      add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/meshoptimizer ${CMAKE_BINARY_DIR}/third_party/meshoptimizer)
    else()
      message(FATAL_ERROR
        "Aver.Runtime.Game.Core's collision build (GameContent.cpp collisionMeshFor) needs "
        "meshoptimizer (MIT, https://github.com/zeux/meshoptimizer) vendored at "
        "third_party/meshoptimizer, and it is not there. Vendor it (its own CMakeLists.txt exposing a "
        "`meshoptimizer` target is enough) and reconfigure.")
    endif()
  endif()
  # PRIVATE: meshopt types never cross GameContent.hpp, so nothing above this library needs to see them.
  target_link_libraries(Aver.Runtime.Game.Core PRIVATE meshoptimizer)
endif()
if(TARGET Aver.Anim.Scene)
  target_link_libraries(Aver.Runtime.Game.Core PUBLIC Aver.Anim.Scene)
endif()
if(TARGET Aver.Physics)
  target_link_libraries(Aver.Runtime.Game.Core PUBLIC Aver.Physics)
endif()
if(TARGET Aver.Framework)
  target_link_libraries(Aver.Runtime.Game.Core PUBLIC Aver.Framework)
endif()
if(TARGET Aver.Trifactor)
  target_link_libraries(Aver.Runtime.Game.Core PUBLIC Aver.Trifactor)
endif()
# Particles (modules/particles): brings AVER_MODULE_PARTICLES=1 through its link interface, so
# GameApp.cpp's `#if AVER_MODULE_PARTICLES` reads as a real zero, not an accidental always-off, in a
# build with the module disabled -- the exact bug this whole block exists to prevent (see its header
# comment). Without this line the game runtime would tick and register nothing a project's
# CParticleEmitter placements could ever draw through, the same gap aee2404 fixed for four other seams.
if(TARGET Aver.Particles)
  target_link_libraries(Aver.Runtime.Game.Core PUBLIC Aver.Particles)
endif()
# .ocparticle reader/writer (DECIDED 3, slice 3): same guard as Aver.Particles itself, for
# Aver.Formats.Material's exact reason above -- it parses straight into particles::ParticleEffect, so
# it is only meaningful once that target exists. Without this a packaged game could register and tick
# CParticleEmitter placements but never resolve one to a real authored effect on disk (GameContent::
# loadProjectParticleEffects, slice 5), the same "wired in the editor, never in the game" gap aee2404
# closed for four other seams.
if(TARGET Aver.Formats.Particles)
  target_link_libraries(Aver.Runtime.Game.Core PUBLIC Aver.Formats.Particles)
endif()

# The IDE groups the library with its host rather than under modules/, where aver_add_module puts it.
set_target_properties(Aver.Runtime.Game.Core PROPERTIES FOLDER "Runtime")

# GameApp and the standalone host, only when the runtime itself is built: the editor needs Core alone.
if(AVER_BUILD_GAME)
  aver_add_module(Aver.Runtime.Game
    SOURCES
      src/GameApp.cpp
    DEPS
      Aver.Runtime.Game.Core
      Aver.Runtime
      Aver.Platform
      Aver.Core
  )
  # stb_image_write, for GameApp.cpp's --screenshot (captureScreenshotIfDue). Matching
  # sandbox/CMakeLists.txt's own `target_include_directories(Sandbox PRIVATE .../third_party/stb)`.
  target_include_directories(Aver.Runtime.Game PRIVATE ${CMAKE_SOURCE_DIR}/third_party/stb)
  # Frame interpolation (docs/rendering/FRAME_INTERPOLATION.md): GameApp builds and installs the
  # generator when the project's RENDER.FRAMEGEN (or --frame-gen) asks for it.
  target_link_libraries(Aver.Runtime.Game PUBLIC Aver.Render.FrameGen)
  # Save/load of a whole world. A composition root is the only thing that may link BOTH this and
  # Aver.Framework -- Aver.Save deliberately takes function pointers instead of the framework.
  if(TARGET Aver.Save)
    target_link_libraries(Aver.Runtime.Game PUBLIC Aver.Save)
  endif()
  if(TARGET Aver.Synapse.Scene)
    target_link_libraries(Aver.Runtime.Game PUBLIC Aver.Synapse.Scene)
  endif()
  # THE MIXER SEAM, WHICH THIS HOST HAS NEVER LINKED. Mirrors sandbox/CMakeLists.txt's own block --
  # see the longer note there for why the macro is a capability rather than an editor tab, and what it
  # cost while it was named after one: every audio call site in the tree sat behind a definition that
  # existed on the editor target alone, so aver_audio_init was unreachable from a packaged game. The
  # device was never opened, Audio.Load succeeded into nothing and a shipped game was silent.
  if(TARGET Aver.Audio.Abi)
    target_link_libraries(Aver.Runtime.Game PUBLIC Aver.Audio.Abi)
    target_compile_definitions(Aver.Runtime.Game PUBLIC AVER_WITH_AUDIO_ABI=1)
  endif()
  # The HUD renderer: the same three libraries Sandbox links for it. Aver.Render.UI links only
  # Aver.Core/Aver.UI/Aver.RHI, no ImGui, so stage-game.ps1's ImGui byte-scan of the exe still passes.
  # Aver.UI.Abi.dll is already on game.allowlist. Without this a HUD script's draw list was never
  # turned into pixels in a packaged game -- see GameApp's attachGameUi/submitGameUi.
  if(TARGET Aver.Render.UI AND TARGET Aver.UI.Abi)
    target_link_libraries(Aver.Runtime.Game PUBLIC Aver.UI Aver.Render.UI Aver.UI.Abi)
    target_compile_definitions(Aver.Runtime.Game PUBLIC AVER_WITH_UI_ABI=1)
  endif()
  # The lift plan's C1 list omitted this while its own DONE-WHEN expected SCRIPTING=1, and the module
  # report caught the contradiction on the first run. A game runs the project's C# gameplay, so it
  # needs the CLR host as much as the editor does -- more, since it has no other way to run anything.
  if(TARGET Aver.Scripting.Host)
    target_link_libraries(Aver.Runtime.Game PUBLIC Aver.Scripting.Host)
  endif()
  set_target_properties(Aver.Runtime.Game PROPERTIES FOLDER "Runtime")

  # The standalone host, AverEngineRuntime.exe.
  add_subdirectory(host)
endif()
