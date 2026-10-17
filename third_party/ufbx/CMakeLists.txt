# ufbx (MIT or public domain, used under MIT) -- vendored FBX loader. One C translation unit.
# Third-party: warnings silenced, like third_party/meshoptimizer. Consumed only by Aver.Formats.Fbx
# (modules/formats), so it never reaches a packaged game.
add_library(ufbx STATIC ufbx.c)
# SYSTEM: the header is included from C++ in the importer, and its anonymous structs trip /W4.
target_include_directories(ufbx SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
  target_compile_options(ufbx PRIVATE /w)
else()
  target_compile_options(ufbx PRIVATE -w)
endif()
set_target_properties(ufbx PROPERTIES FOLDER "third_party" POSITION_INDEPENDENT_CODE ON)
