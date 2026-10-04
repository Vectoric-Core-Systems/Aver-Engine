# Aver.Mcp -- the editor control channel (see README.md).
#
# STATIC and Core-only. It holds a socket and a queue and knows nothing about ImGui, the RHI or the
# editor -- which is precisely what lets the editor be built entirely without it.
#
# WINDOWS-ONLY, guarded the way modules/audio.wasapi is rather than being unconditional: it posts Win32
# messages, and on any other platform start() declines and says so instead of failing to link.
add_library(Aver.Mcp STATIC
  src/McpBridge.cpp)

target_include_directories(Aver.Mcp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(Aver.Mcp PUBLIC cxx_std_20)
target_link_libraries(Aver.Mcp PUBLIC Aver.Core)

# Consumers get AVER_MODULE_MCP so every call site can #if around the whole feature. That macro is what
# makes "the editor works without this module" checkable rather than merely intended.
target_compile_definitions(Aver.Mcp PUBLIC AVER_MODULE_MCP=1)

set_target_properties(Aver.Mcp PROPERTIES FOLDER "modules")
