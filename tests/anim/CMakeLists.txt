# AnimTest — sampling and posing, with no GPU.
#
# It links Aver.Anim, Aver.Formats and Aver.Core and nothing else, which is the module's whole
# architectural claim made into something the build enforces: a sampler that needed a device could
# not be checked this way, and every property here is one no screenshot can decide.
add_executable(AnimTest src/AnimTest.cpp)
target_link_libraries(AnimTest PRIVATE Aver.Anim Aver.Formats Aver.Core Aver.Platform)
set_target_properties(AnimTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# IkTest -- two-bone IK. Same link set and the same reason: a triangle solved by the law of cosines
# needs no device, so the whole solver is checkable by asserting where a chain ended up.
add_executable(IkTest src/IkTest.cpp)
target_link_libraries(IkTest PRIVATE Aver.Anim Aver.Formats Aver.Core Aver.Platform)
set_target_properties(IkTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Blend spaces + animation state machines (docs/ANIM_BLEND_STATE.md). Same link set and the same
# reason as AnimTest: weights, sync phase, transition timing and the asset round trip are all
# arithmetic on structs, so a device would only hide a wrong answer.
add_executable(AnimBlendSpaceTest src/BlendSpaceTest.cpp)
target_link_libraries(AnimBlendSpaceTest PRIVATE Aver.Anim Aver.Formats Aver.Core Aver.Platform)
set_target_properties(AnimBlendSpaceTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_executable(AnimStateMachineTest src/AnimStateMachineTest.cpp)
target_link_libraries(AnimStateMachineTest PRIVATE Aver.Anim Aver.Formats Aver.Core Aver.Platform)
set_target_properties(AnimStateMachineTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_executable(AnimGraphAssetTest src/AnimGraphAssetTest.cpp)
target_link_libraries(AnimGraphAssetTest PRIVATE Aver.Anim Aver.Formats Aver.Core Aver.Platform)
set_target_properties(AnimGraphAssetTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
