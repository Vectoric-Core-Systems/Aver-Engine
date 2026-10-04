# UiRenderTest — the UI renderer, against a RECORDING DEVICE rather than a GPU.
#
# It links Aver.RHI for the interfaces alone: the test implements IDevice, IResourceFactory and
# IRenderContext itself and logs every call. That is possible because Aver.Render.UI talks to the
# generic RHI and never to a backend — the same property the module's link line enforces, verified
# here by a test that would not build if it were false.
add_executable(UiRenderTest src/UiRenderTest.cpp)
target_link_libraries(UiRenderTest PRIVATE Aver.Render.UI Aver.UI Aver.RHI Aver.Core Aver.Platform)
set_target_properties(UiRenderTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
