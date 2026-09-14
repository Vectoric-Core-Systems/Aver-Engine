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

# ---------------------------------------------------------------------------------------------
# aver_deploy_shaders(<target> <dir>) -- copy a module's .hlsl/.hlsli into <build>/bin/shaders.
#
# WHY THIS IS NOT A POST_BUILD COMMAND, which is what all three call sites used to be and what the
# obvious reading of "refreshed on every build" suggests. A POST_BUILD command runs when its TARGET
# is relinked, and a .hlsl file is not a source of any target -- so editing a shader made nothing
# out of date, the library did not relink, the copy never ran, and bin/shaders kept the PREVIOUS
# build's text. The binary then compiled a shader the source tree no longer contained.
#
# That failed silently and in the worst possible direction: a shader edit appeared to do nothing,
# which reads exactly like "the change had no visual effect" rather than "the change was never
# deployed". It cost a full falsification cycle to notice -- a deliberately broken shader term
# rendered a correct picture, which is precisely the signature this repository has already been
# burned by twice (see ShaderFiles.hpp on the static-cache version of the same lie).
#
# The fix is a real dependency edge. The .hlsl files become DEPENDS of a custom command with an
# OUTPUT stamp, so the build graph knows the stamp is stale when any of them changes, and a custom
# target the library depends on forces it to be considered every build. Ninja then reruns the copy
# for a shader-only edit -- and, just as importantly, does NOT rerun it when nothing changed.
function(aver_deploy_shaders target dir)
    file(GLOB _av_shader_files CONFIGURE_DEPENDS "${dir}/*.hlsl" "${dir}/*.hlsli")
    if(NOT _av_shader_files)
        message(FATAL_ERROR "aver_deploy_shaders(${target}): no shaders in ${dir}. A module that "
                            "deploys nothing is a silent gap -- the shader loader would fall back "
                            "to an empty string and DXC would blame the missing declarations.")
    endif()
    # THE DEPLOYED FILES ARE THE OUTPUT, not just a stamp beside them.
    #
    # This used to declare only ${target}.shaders.stamp. The stamp records "the sources were last
    # seen at time T", which answers "did a shader change?" and cannot answer "is the shader still
    # THERE?" -- so deleting bin/shaders/voxi.hlsl left every later build reporting OK with the file
    # still gone. MEASURED: removed bin/shaders/viewport_icon.hlsl, rebuilt clean, still missing.
    # shaderFile()'s own comment says what that costs -- DXC then fails on missing declarations
    # rather than on a missing file, so the error names the wrong thing entirely.
    #
    # Naming them also makes a basename collision a CONFIGURE-TIME error instead of a silent
    # overwrite: all fourteen call sites copy into one shared bin/shaders, so two modules shipping
    # the same filename currently means one clobbers the other with nothing said. Ninja refuses two
    # rules generating one path, so that now cannot be built at all. (None collide today -- checked.)
    set(_av_deployed "")
    foreach(_av_src IN LISTS _av_shader_files)
        get_filename_component(_av_name "${_av_src}" NAME)
        list(APPEND _av_deployed "${CMAKE_BINARY_DIR}/bin/shaders/${_av_name}")
    endforeach()

    set(_av_stamp "${CMAKE_CURRENT_BINARY_DIR}/${target}.shaders.stamp")
    add_custom_command(
        OUTPUT  ${_av_deployed} "${_av_stamp}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_av_shader_files} "${CMAKE_BINARY_DIR}/bin/shaders"
        # TOUCHED BECAUSE copy_if_different PRESERVES THE SOURCE MTIME. Without this the deployed
        # copy can be OLDER than the file it came from -- a source touched by a branch switch with
        # no content change is copied over, so the destination keeps the PREVIOUS source's time.
        # Ninja reads output-older-than-input as dirty, so the rule would then re-run on every
        # single build, forever, and never come clean. Copying only on a real change and stamping
        # the result keeps both properties: no needless rewrite, no perpetual rebuild.
        COMMAND ${CMAKE_COMMAND} -E touch ${_av_deployed} "${_av_stamp}"
        DEPENDS ${_av_shader_files}
        COMMENT "${target}: shaders -> bin/shaders"
        VERBATIM)
    add_custom_target(${target}.Shaders DEPENDS ${_av_deployed} "${_av_stamp}")
    add_dependencies(${target} ${target}.Shaders)
    set_target_properties(${target}.Shaders PROPERTIES FOLDER "modules/shaders")
endfunction()
