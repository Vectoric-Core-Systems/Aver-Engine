# DestructTest -- Aver.Destruct in a real Jolt world with no device: anchors, strain accumulation, splitting into piece bodies, momentum, the debris cap,
# sleeping despawn, breakable joints and cleanup. The fractures are made in the test with aver/formats/Fracture.hpp (no asset on disk).
add_executable(DestructTest src/DestructTest.cpp)
target_link_libraries(DestructTest PRIVATE Aver.Destruct Aver.Physics Aver.Formats Aver.Core Aver.Platform)
set_target_properties(DestructTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
