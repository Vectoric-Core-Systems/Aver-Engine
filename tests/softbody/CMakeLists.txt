# The soft-body library with no GPU, no scene and no Jolt: topology, damage, islands, the mesh weld and
# the .ocsoft asset. Links Aver.SoftBody, Aver.Formats (Json) and Core/Platform only, which is the
# module's claim made into something the build enforces. Every property here -- a beam held past its
# yield keeping the stretch, a strip cut into exactly two islands, a UV seam welding to one particle,
# write -> parse -> write being byte-identical -- is one a screenshot cannot decide. The simulation is
# Jolt's and is checked through its ABI in tests/physics (SoftBodyBuilderTest) and tests/softbody.scene.
# Wave 10 adds FieldsTest (volumes and their weights), CapsTest (thick breaks), SoftStateTest (the saved-damage codec and the
# tear-log replay) and ProxyBindTest (render mesh to proxy cage).
foreach(_t SoftBodyTest SoftAssetTest SimMeshTest DamageTest IslandsTest FieldsTest CapsTest SoftStateTest ProxyBindTest)
  # SoftBodyTest.cpp keeps its old target name: tests/physics already owns a target called SoftBodyTest.
  if(_t STREQUAL "SoftBodyTest")
    set(_target PlasticSoftBodyTest)
  else()
    set(_target ${_t})
  endif()
  add_executable(${_target} src/${_t}.cpp)
  target_link_libraries(${_target} PRIVATE Aver.SoftBody Aver.Formats Aver.Core Aver.Platform)
  set_target_properties(${_target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endforeach()
