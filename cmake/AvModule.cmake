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

  if(ARG_DEPS)
    target_link_libraries(${MODNAME} PUBLIC ${ARG_DEPS})
  endif()
  if(ARG_PUBLIC_DEFS)
    target_compile_definitions(${MODNAME} PUBLIC ${ARG_PUBLIC_DEFS})
  endif()

  set_target_properties(${MODNAME} PROPERTIES FOLDER "modules")
endfunction()
