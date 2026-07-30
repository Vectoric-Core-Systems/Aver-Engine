# AtmosphereTest -- the scattering model with no GPU.
#
# It links Aver.RHI and Aver.Core and nothing else, which is the claim the model makes about itself:
# an atmosphere that needed a device could not be checked against a numeric integral, and every
# property this test asserts is one no screenshot can decide.
add_executable(AtmosphereTest src/AtmosphereTest.cpp)
target_link_libraries(AtmosphereTest PRIVATE Aver.RHI Aver.Core Aver.Platform)
set_target_properties(AtmosphereTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
