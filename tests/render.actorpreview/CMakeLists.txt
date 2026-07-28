# ActorPreviewTest — the actor preview, against a recording device rather than a GPU.
#
# It links Aver.RHI for the interfaces alone and implements IDevice / IResourceFactory /
# IRenderContext itself. That is possible because the feature talks to the generic RHI and never to a
# backend, and this test would not build if that stopped being true.
add_executable(ActorPreviewTest src/ActorPreviewTest.cpp)
target_link_libraries(ActorPreviewTest PRIVATE Aver.Render.ActorPreview Aver.RHI Aver.Core Aver.Platform)
set_target_properties(ActorPreviewTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
