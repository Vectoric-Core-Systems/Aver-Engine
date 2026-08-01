# AnimTest — sampling and posing, with no GPU.
#
# It links Aver.Anim, Aver.Formats and Aver.Core and nothing else, which is the module's whole
# architectural claim made into something the build enforces: a sampler that needed a device could
# not be checked this way, and every property here is one no screenshot can decide.
add_executable(AnimTest src/AnimTest.cpp)
target_link_libraries(AnimTest PRIVATE Aver.Anim Aver.Formats Aver.Core Aver.Platform)
set_target_properties(AnimTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
