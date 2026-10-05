# Aver.Render.Neural -- small neural networks that run inside the frame (see README.md): a fully
# connected MLP and a convolutional net (ConvNet), each with GPU inference and training in portable fp32
# HLSL, plus CPU references of the same maths that are the spec for the shaders and what the tests exercise.
#
# RHI-only and unconditional, like Aver.Render.Denoise: nothing here is optional at build time.
# Whether a network runs is a RUNTIME question -- the shaders compile when Mlp::create() is called
# -- and Mlp::create() says so with a false return. MlpReference never touches the RHI at all.
add_library(Aver.Render.Neural STATIC
  src/ConvNet.cpp
  src/Mlp.cpp
  src/MlpReference.cpp
  src/NeuralOptimiser.cpp
  src/ConvNetReference.cpp
  src/WeightFile.cpp)

target_include_directories(Aver.Render.Neural PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(Aver.Render.Neural PUBLIC cxx_std_20)
target_link_libraries(Aver.Render.Neural PUBLIC Aver.Core Aver.RHI)
set_target_properties(Aver.Render.Neural PROPERTIES FOLDER "modules")

# aver_neural_mlp.hlsl beside the executable, like every module's shaders.
aver_deploy_shaders(Aver.Render.Neural "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
