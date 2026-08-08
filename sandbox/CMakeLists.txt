add_executable(Sandbox
  src/SandboxApp.cpp
  src/AssetEditor.cpp
  src/ActorEditor.cpp
  src/AnimEditor.cpp
  src/SkinDrawTest.cpp
  src/SkinSceneTest.cpp
  src/ReflTest.cpp
  src/ProjectBrowser.cpp
  src/ProjectScaffold.cpp
  src/EditorPrefs.cpp
  src/EngineScaffold.cpp
  src/IdeIntegration.cpp
  src/ShellIntegration.cpp
  src/ToolsMenu.cpp
  Sandbox.rc)
target_link_libraries(Sandbox PRIVATE Aver.Runtime Aver.Formats)
# The level/world runtime. The editor and the game runtime BOTH link this, which is the whole reason
# it is not inside Aver.Runtime.Game: turning a .ocworld placement into an entity used to be written
# out twice, and every future change to it -- chunk ownership first -- would have had to be made
# twice and stay agreeing.
target_link_libraries(Sandbox PRIVATE Aver.World)
# The Roslyn escalation for an actor script the built-in scanner declines. Linked here rather
# than pulled in by Aver.Formats, which must stay dependency-free -- see its own CMakeLists.
if(TARGET Aver.Formats.Roslyn)
  target_link_libraries(Sandbox PRIVATE Aver.Formats.Roslyn)
  # The tab compiles either way; this is what decides whether it can ESCALATE. A build without the
  # backend still opens every conforming actor, which is the point of the scanner being sufficient
  # on its own.
  target_compile_definitions(Sandbox PRIVATE AVER_HAVE_ROSLYN=1)
endif()
# The audio importer, behind the Content Browser's Import button: .wav/.mp3/.m4a/.flac -> .ocaudio.
# A separate target like the mesh and material readers, and for the same reason: it pulls in Media
# Foundation, which nothing else in the editor needs.
if(TARGET Aver.Formats.Audio)
  target_link_libraries(Sandbox PRIVATE Aver.Formats.Audio)
  target_compile_definitions(Sandbox PRIVATE AVER_HAVE_AUDIO_IMPORT=1)
endif()
# The retained game UI and the render feature that draws it. Both unconditional: neither depends on
# an optional module, and the renderer declines at create() on a backend with no GPU support.
target_link_libraries(Sandbox PRIVATE Aver.UI Aver.Render.UI)
# ...and the C seam onto it. The editor draws its own demo through this rather than against a
# UiDrawList directly, so the seam a game's HUD uses is exercised by the editor too. Aver.UI is
# still named above because the host casts aver_ui_draw_list() back to that type.
target_link_libraries(Sandbox PRIVATE Aver.UI.Abi)
# The actor editor's preview. Its own mesh registry, deliberately: an asset editor that reached
# into the level editor's table could not exist or be tested without one.
target_link_libraries(Sandbox PRIVATE Aver.Render.ActorPreview)
# GPU skinning, for --skin-test. The pass has no other consumer yet -- the mesh draw path cannot
# take a substituted position stream -- so this link line is what keeps it compiled and checkable.
target_link_libraries(Sandbox PRIVATE Aver.Render.Skin)
# The path tracer, for --pt-furnace. Linked here for the same reason skinning is: the editor is the
# only host that drives it, and this link line is what keeps its shader compiled and its energy
# oracle runnable on a real device.
target_link_libraries(Sandbox PRIVATE Aver.Render.PathTracer)
if(TARGET Aver.Render.Voxi)
  # Settings DLL (brings AVER_MODULE_VOXI=1) + the static render feature that drives the RHI.
  target_link_libraries(Sandbox PRIVATE Aver.Render.Voxi Aver.Render.Voxi.Renderer)
endif()
if(TARGET Aver.Render.PBR)
  # The material DLL (brings AVER_MODULE_PBR=1). Named explicitly rather than left to arrive
  # transitively through Voxi's renderer: the executable is what the C# layer P/Invokes into, so it
  # is the executable that must depend on the material system whether or not a render feature does.
  # ...and the GPU half, which is what turns an authored material into a binding set and a b2 block.
  # Both are named here for the same reason Voxi's two are: the executable is what the C# layer
  # P/Invokes, so it must depend on the material system whether or not a render feature does.
  target_link_libraries(Sandbox PRIVATE Aver.Render.PBR Aver.Render.PBR.Materials)
  # The .ocmat reader and the decode-to-GPU step. Both are only meaningful with a material system,
  # so they hang off the same guard rather than being linked unconditionally: with PBR off the
  # editor has no material to load a file into and nothing to upload a texture for.
  target_link_libraries(Sandbox PRIVATE Aver.Formats.Material Aver.Assets.Gpu)
endif()
if(TARGET Aver.Scene)
  # The world DLL (brings AVER_MODULE_SCENE=1). Named explicitly for the reason the material system
  # is: the executable is what the C# layer P/Invokes into, so it must depend on the world whether
  # or not anything else in the link already does.
  target_link_libraries(Sandbox PRIVATE Aver.Scene)
endif()
if(TARGET Aver.Anim.Scene)
  # The scene/animation join. Named here because the app owns the frame loop, and the frame loop is
  # what decides when the animation clock advances.
  target_link_libraries(Sandbox PRIVATE Aver.Anim.Scene)
endif()
if(TARGET Aver.Physics)
  # The simulation (brings AVER_MODULE_PHYSICS=1). Named on the executable for the same reason the
  # scene and material systems are: the app owns the frame loop, and the frame loop is what decides
  # when the world is stepped.
  target_link_libraries(Sandbox PRIVATE Aver.Physics)
endif()
if(TARGET Aver.Framework)
  # Gameplay (brings AVER_MODULE_FRAMEWORK=1). It carries Aver.Scene transitively, but the scene is
  # still named above on its own: the framework is optional and the world is not meant to become
  # unreachable the day someone turns it off.
  target_link_libraries(Sandbox PRIVATE Aver.Framework)
endif()
if(TARGET Aver.Scripting.Host)
  # The in-process CLR host (brings AVER_MODULE_SCRIPTING=1). The executable owns the instance and
  # ticks it, the same way it owns the Voxi feature -- a subsystem the app configures is not
  # something the composition root should be reaching into.
  target_link_libraries(Sandbox PRIVATE Aver.Scripting.Host)
endif()
target_include_directories(Sandbox PRIVATE ${CMAKE_SOURCE_DIR}/third_party/stb)
set_target_properties(Sandbox PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Ship the splash next to the executable so the engine can show it at startup.
#
# Driven by an OUTPUT rule rather than POST_BUILD: a POST_BUILD command only runs when the target
# actually relinks, so editing the artwork alone left the old splash shipped next to the exe with a
# clean build reporting success.
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/bin/splash.png
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/branding/splash.png ${CMAKE_BINARY_DIR}/bin/splash.png
  DEPENDS ${CMAKE_SOURCE_DIR}/branding/splash.png
  COMMENT "Staging splash.png"
  VERBATIM)
add_custom_target(Sandbox.Splash DEPENDS ${CMAKE_BINARY_DIR}/bin/splash.png)
add_dependencies(Sandbox Sandbox.Splash)
set_target_properties(Sandbox.Splash PROPERTIES FOLDER "sandbox")

# Ship the project browser's mark next to the executable. Same OUTPUT/DEPENDS rule as the splash
# above, for the same reason.
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/bin/logo.png
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/branding/logo.png ${CMAKE_BINARY_DIR}/bin/logo.png
  DEPENDS ${CMAKE_SOURCE_DIR}/branding/logo.png
  COMMENT "Staging logo.png"
  VERBATIM)
add_custom_target(Sandbox.Logo DEPENDS ${CMAKE_BINARY_DIR}/bin/logo.png)
add_dependencies(Sandbox Sandbox.Logo)
set_target_properties(Sandbox.Logo PROPERTIES FOLDER "sandbox")

# Ship the Compile C# status sprite sheet (three tiles: built / failed / stale). Same OUTPUT/DEPENDS
# rule as the splash and logo above, so editing the artwork alone re-stages it without a relink.
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/bin/compile-status.png
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/branding/compile-status.png ${CMAKE_BINARY_DIR}/bin/compile-status.png
  DEPENDS ${CMAKE_SOURCE_DIR}/branding/compile-status.png
  COMMENT "Staging compile-status.png"
  VERBATIM)
add_custom_target(Sandbox.CompileIcon DEPENDS ${CMAKE_BINARY_DIR}/bin/compile-status.png)
add_dependencies(Sandbox Sandbox.CompileIcon)
set_target_properties(Sandbox.CompileIcon PROPERTIES FOLDER "sandbox")

# Ship the Content Browser file-type icon sheet (four tiles: C# Script / C# Class / C++ Class / C++ Module).
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/bin/file-icons.png
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/branding/file-icons.png ${CMAKE_BINARY_DIR}/bin/file-icons.png
  DEPENDS ${CMAKE_SOURCE_DIR}/branding/file-icons.png
  COMMENT "Staging file-icons.png"
  VERBATIM)
add_custom_target(Sandbox.FileIcons DEPENDS ${CMAKE_BINARY_DIR}/bin/file-icons.png)
add_dependencies(Sandbox Sandbox.FileIcons)
set_target_properties(Sandbox.FileIcons PROPERTIES FOLDER "sandbox")

# Ship the Content Browser folder icon sheet (two tiles: plain folder / engine-or-module folder).
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/bin/folder-icons.png
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/branding/folder-icons.png ${CMAKE_BINARY_DIR}/bin/folder-icons.png
  DEPENDS ${CMAKE_SOURCE_DIR}/branding/folder-icons.png
  COMMENT "Staging folder-icons.png"
  VERBATIM)
add_custom_target(Sandbox.FolderIcons DEPENDS ${CMAKE_BINARY_DIR}/bin/folder-icons.png)
add_dependencies(Sandbox Sandbox.FolderIcons)
set_target_properties(Sandbox.FolderIcons PROPERTIES FOLDER "sandbox")

# The generated asset-type sheet (anim / skeleton / mesh). Staged the same way and for the same
# reason as the others: POST_BUILD would not re-copy a sheet that changed without the exe relinking.
# Drawn by scripts/make-asset-icons.py, NOT hand-authored -- see branding/ASSETS.md.
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/bin/asset-icons.png
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/branding/asset-icons.png ${CMAKE_BINARY_DIR}/bin/asset-icons.png
  DEPENDS ${CMAKE_SOURCE_DIR}/branding/asset-icons.png
  COMMENT "Staging asset-icons.png"
)
add_custom_target(Sandbox.AssetIcons DEPENDS ${CMAKE_BINARY_DIR}/bin/asset-icons.png)
add_dependencies(Sandbox Sandbox.AssetIcons)
set_target_properties(Sandbox.AssetIcons PROPERTIES FOLDER "sandbox")

# Ship the editor UI typeface next to the executable. Same OUTPUT/DEPENDS rule as the splash, and
# for the same reason -- POST_BUILD would not re-copy a font that changed without the exe relinking.
set(AVER_UI_FONTS Roboto-Regular.ttf Roboto-Medium.ttf)
set(AVER_UI_FONT_OUTPUTS)
foreach(font ${AVER_UI_FONTS})
  add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/bin/${font}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/third_party/fonts/${font} ${CMAKE_BINARY_DIR}/bin/${font}
    DEPENDS ${CMAKE_SOURCE_DIR}/third_party/fonts/${font}
    COMMENT "Staging ${font}"
    VERBATIM)
  list(APPEND AVER_UI_FONT_OUTPUTS ${CMAKE_BINARY_DIR}/bin/${font})
endforeach()
add_custom_target(Sandbox.Fonts DEPENDS ${AVER_UI_FONT_OUTPUTS})
add_dependencies(Sandbox Sandbox.Fonts)
set_target_properties(Sandbox.Fonts PROPERTIES FOLDER "sandbox")

# The resource script embeds the icon, but nothing tells the generator that it reads the .ico -- so
# replacing the icon would not re-run the resource compiler and the exe would keep the old one.
set_property(SOURCE Sandbox.rc APPEND PROPERTY
  OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/branding/icon.ico)

# The editor control channel. OPTIONAL in the strongest sense: without it the editor has no idea it
# ever existed, which is what `if(TARGET ...)` here and AVER_MODULE_MCP in the module both enforce.
if(TARGET Aver.Mcp)
  target_link_libraries(Sandbox PRIVATE Aver.Mcp)
endif()
