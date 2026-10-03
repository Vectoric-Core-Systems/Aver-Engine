# NeuralMlpTest -- Aver.Render.Neural's CPU reference, headless: no GPU, no device, no backend.
#
# The GPU network cannot be tested without a device, so the maths it must reproduce lives in
# MlpReference (the spec for aver_neural_mlp.hlsl) and the properties are checked there: the
# backward pass against finite differences, a toy function being learned, the init and the weight
# file, and the order-independence of the fixed-point gradient accumulation.
add_executable(NeuralMlpTest src/NeuralMlpTest.cpp)
target_link_libraries(NeuralMlpTest PRIVATE Aver.Render.Neural Aver.Core)
set_target_properties(NeuralMlpTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
