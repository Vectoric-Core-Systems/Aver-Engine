# AvModule.cmake — helper for declaring Aver Engine modules as strict-DAG static libs.
#
#   aver_add_module(<TargetName>
#       SOURCES     <files...>
#       DEPS        <other Aver.* or system libs...>   # PUBLIC link deps
#       PUBLIC_DEFS <compile definitions...>)
#
# Convention: every module owns an include/ dir exposed PUBLICly, and depends only
# on modules in a lower tier (the DAG). Keeping deps explicit here lets a future CI
# "DAG lint" step verify no edge points up a tier.
function(aver_add_module MODNAME)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS;PUBLIC_DEFS" ${ARGN})

  add_library(${MODNAME} STATIC ${ARG_SOURCES})
  target_include_directories(${MODNAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_features(${MODNAME} PUBLIC cxx_std_20)

  # NO PER-MODULE DEPENDENCY CHECK HERE, and the reason is worth writing down because the obvious
  # design does not work.
  #
  # Checking `if(NOT TARGET ${dep})` at this point looks right and rejects valid configurations:
  # CMake permits FORWARD REFERENCES, and this tree relies on them deliberately. Aver.Assets.Gpu
  # (modules/assets, added at root CMakeLists.txt:77) names Aver.Formats (:87) and Aver.RHI (:109),
  # both of which are added later; the link is resolved at generate time, not here. A check in this
  # function cannot tell "declared later" from "optional and absent", so it fails the DEFAULT build.
  #
  # The check therefore lives in aver_check_module_dag() below, called once after every
  # add_subdirectory, which is the earliest point at which the question is actually answerable.

  if(ARG_DEPS)
    target_link_libraries(${MODNAME} PUBLIC ${ARG_DEPS})
  endif()
  if(ARG_PUBLIC_DEFS)
    target_compile_definitions(${MODNAME} PUBLIC ${ARG_PUBLIC_DEFS})
  endif()

  set_target_properties(${MODNAME} PROPERTIES FOLDER "modules")
endfunction()

# The same check over EVERY target, including the ones that never went through aver_add_module.
#
# Several targets are hand-written add_library + target_link_libraries (modules/render.pbr,
# modules/landscape, the two executables), so the per-module check above cannot see them. Called once
# from the root CMakeLists.txt after every add_subdirectory, when the full target set exists.
#
# FAILS rather than warns. This is the ONLY place the question is answerable -- see the note in
# aver_add_module about forward references -- so a warning here would be a warning nobody reads about
# a build that is going to fail anyway, forty edges away, as LNK1104 on a library no source file
# mentions. That is exactly how Aver.Render.ActorPreview naming Aver.Formats.Material unconditionally
# broke every AVER_MODULE_PBR=OFF build while looking like a linker problem.
#
# Entries that are not Aver.* are skipped: system libraries (user32, gdi32) and vendored ones (imgui)
# legitimately may not be CMake targets. Generator expressions are skipped too -- a $<...> entry is
# not a target name and cannot be checked here.
function(aver_check_module_dag)
  get_property(_all_targets DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY BUILDSYSTEM_TARGETS)
  set(_dirs ${CMAKE_SOURCE_DIR})
  set(_seen "")
  while(_dirs)
    list(POP_FRONT _dirs _dir)
    get_property(_subs DIRECTORY ${_dir} PROPERTY SUBDIRECTORIES)
    list(APPEND _dirs ${_subs})
    get_property(_tgts DIRECTORY ${_dir} PROPERTY BUILDSYSTEM_TARGETS)
    foreach(t IN LISTS _tgts)
      get_target_property(_type ${t} TYPE)
      if(_type STREQUAL "INTERFACE_LIBRARY" OR _type STREQUAL "UTILITY")
        continue()
      endif()
      get_target_property(_libs ${t} LINK_LIBRARIES)
      if(NOT _libs)
        continue()
      endif()
      foreach(l IN LISTS _libs)
        if(l MATCHES "^Aver\\." AND NOT TARGET ${l})
          list(APPEND _seen "${t} -> ${l}")
        endif()
      endforeach()
    endforeach()
  endwhile()
  list(LENGTH _seen _n)
  if(_n GREATER 0)
    string(REPLACE ";" "
    " _pretty "${_seen}")
    message(FATAL_ERROR
      "Module DAG: ${_n} link edge(s) name an Aver.* target that does not exist in this configuration:
"
      "    ${_pretty}
"
      "  Each is an OPTIONAL module named without a guard. Wrap it:
"
      "    if(TARGET Aver.Something)
      target_link_libraries(<target> PUBLIC Aver.Something)
    endif()
"
      "  Unguarded, this does not fail here -- it fails as LNK1104 in some unrelated target.")
  endif()
  message(STATUS "Aver: module DAG check passed -- every Aver.* link edge resolves in this configuration")
endfunction()
