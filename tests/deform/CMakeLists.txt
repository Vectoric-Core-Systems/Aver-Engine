# DeformTest — the cage, with no GPU.
#
# It links Aver.Deform, Aver.Anim, Aver.Formats and Aver.Core and nothing else, which is the module's
# claim made into something the build enforces. Every property here -- partition of unity, rigid
# motion reproduced exactly, influence staying local, and the CPU/GPU parity contract's operation
# order -- is one a screenshot cannot decide, and one the GPU port will have to reproduce.
add_executable(DeformTest src/DeformTest.cpp)
target_link_libraries(DeformTest PRIVATE Aver.Deform Aver.Anim Aver.Formats Aver.Core Aver.Platform)
set_target_properties(DeformTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
