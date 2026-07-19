add_executable(Sandbox src/SandboxApp.cpp Sandbox.rc)
target_link_libraries(Sandbox PRIVATE Aver.Runtime Aver.Formats)
target_include_directories(Sandbox PRIVATE ${CMAKE_SOURCE_DIR}/third_party/stb)
set_target_properties(Sandbox PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Ship the splash next to the executable so the engine can show it at startup.
add_custom_command(TARGET Sandbox POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/branding/splash.png $<TARGET_FILE_DIR:Sandbox>/splash.png
  VERBATIM)
