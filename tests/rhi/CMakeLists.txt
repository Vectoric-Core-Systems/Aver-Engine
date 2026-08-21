# AtmosphereTest -- the scattering model with no GPU.
#
# It links Aver.RHI and Aver.Core and nothing else, which is the claim the model makes about itself:
# an atmosphere that needed a device could not be checked against a numeric integral, and every
# property this test asserts is one no screenshot can decide.
add_executable(AtmosphereTest src/AtmosphereTest.cpp)
target_link_libraries(AtmosphereTest PRIVATE Aver.RHI Aver.Core Aver.Platform)
set_target_properties(AtmosphereTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# LineMeshTest -- destroyLineMesh's contract, against a real backend.
#
# The only RHI test here that creates a device. It has to: the rules under test (a slot is kept and
# never recycled, a second destroy is refused, a dead handle draws nothing) live in the backends, and
# a mock repeating them would only prove the mock. It asks for WARP, so no GPU is required, and it
# reports SKIPPED rather than failing when neither D3D12 nor Vulkan is present.
add_executable(LineMeshTest src/LineMeshTest.cpp)
target_link_libraries(LineMeshTest PRIVATE Aver.RHI Aver.Core Aver.Platform)

# This test is its OWN composition root, the same way Aver.Runtime is: Aver.RHI calls
# detail::createD3D12Device et al behind AVER_HAS_* but does not link the backends, so a consumer
# has to. Mirroring the guards here rather than linking Aver.Runtime keeps Engine.cpp -- a window,
# a main loop and a settings file -- out of a test about one vector of buffers.
if(AVER_RHI_D3D12)
  target_link_libraries(LineMeshTest PRIVATE Aver.RHI.D3D12)
endif()
if(AVER_RHI_D3D11)
  target_link_libraries(LineMeshTest PRIVATE Aver.RHI.D3D11)
endif()
if(AVER_RHI_VULKAN)
  target_link_libraries(LineMeshTest PRIVATE Aver.RHI.Vulkan)
endif()
set_target_properties(LineMeshTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
