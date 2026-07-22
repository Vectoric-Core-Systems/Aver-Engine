add_executable(Sandbox
  src/SandboxApp.cpp
  src/ProjectBrowser.cpp
  src/ProjectScaffold.cpp
  src/EngineScaffold.cpp
  src/IdeIntegration.cpp
  src/ToolsMenu.cpp
  Sandbox.rc)
target_link_libraries(Sandbox PRIVATE Aver.Runtime Aver.Formats)
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
endif()
if(TARGET Aver.Scene)
  # The world DLL (brings AVER_MODULE_SCENE=1). Named explicitly for the reason the material system
  # is: the executable is what the C# layer P/Invokes into, so it must depend on the world whether
  # or not anything else in the link already does.
  target_link_libraries(Sandbox PRIVATE Aver.Scene)
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
