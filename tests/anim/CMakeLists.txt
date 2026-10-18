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

# Montages, root motion and the .ocasm slot/layered/additive pipeline (docs/ANIM_MONTAGES.md). Same
# link set and the same reason: sections, blend timing, notify crossings, root deltas and masked blends
# are arithmetic on structs, so a device would only hide a wrong answer.
foreach(t Montage RootMotion AnimPipeline)
  add_executable(${t}Test src/${t}Test.cpp)
  target_link_libraries(${t}Test PRIVATE Aver.Anim Aver.Formats Aver.Core Aver.Platform)
  set_target_properties(${t}Test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endforeach()

# Chain IK (FABRIK / CCD with limits), the .ocikrig asset, the pure rig stack (priorities, body, foot placement)
# and the retargeter (docs/plans/wave5/IK_RETARGETING.md). Same link set and the same reason: reach, bone lengths,
# bend side, limits and foot offsets are arithmetic on structs, so a device would only hide a wrong answer.
foreach(t IkSolvers IkRigAsset IkRigSolve Retarget)
  add_executable(${t}Test src/${t}Test.cpp)
  target_link_libraries(${t}Test PRIVATE Aver.Anim Aver.Formats Aver.Core Aver.Platform)
  set_target_properties(${t}Test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endforeach()

# Motion matching (docs/ANIM_MOTION_MATCHING.md): the .ocmotion asset, the pose database and its sidecar, exact
# search, trajectory prediction, inertialization and the matcher itself. Same link set and the same reason:
# synthetic clips with closed-form root travel make every property a number, and a device would hide a wrong one.
foreach(t MotionAsset PoseDatabase PoseSearch Trajectory Inertializer MotionMatcher)
  add_executable(${t}Test src/${t}Test.cpp)
  target_link_libraries(${t}Test PRIVATE Aver.Anim Aver.Formats Aver.Core Aver.Platform)
  set_target_properties(${t}Test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endforeach()

# [wave9/morph begin] Blend shapes and facial animation (docs/plans/wave9/MORPH_TARGETS.md): the pose curve channel and
# its blend rules, the CPU morph reference, the .ocface rig and the .ocvis lip-sync tracks. Same link set and the same
# reason: weights, envelopes and joint deltas are arithmetic on structs, so a device would only hide a wrong answer.
foreach(t PoseCurve Morph)
  add_executable(${t}Test src/${t}Test.cpp)
  target_link_libraries(${t}Test PRIVATE Aver.Anim Aver.Formats Aver.Core Aver.Platform)
  set_target_properties(${t}Test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endforeach()
# [wave9/morph end]

# [wave9/morph begin] (continued) the face rig and lip-sync tests.
foreach(t FaceRig Viseme)
  add_executable(${t}Test src/${t}Test.cpp)
  target_link_libraries(${t}Test PRIVATE Aver.Anim Aver.Formats Aver.Core Aver.Platform)
  set_target_properties(${t}Test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endforeach()
# [wave9/morph end]
