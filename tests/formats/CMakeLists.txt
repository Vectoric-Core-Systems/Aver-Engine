add_executable(FormatTest src/FormatTest.cpp)
target_link_libraries(FormatTest PRIVATE Aver.Formats Aver.Assets Aver.Core Aver.Platform)
set_target_properties(FormatTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# MaterialTest — the .ocmat reader/writer and the mip filter. A SECOND executable rather than more
# cases in FormatTest, because it is the only one that needs the material DLL: a machine where
# Aver.Render.PBR.dll fails to load would otherwise report the .ocbeam parser as broken.
if(TARGET Aver.Formats.Material)
  add_executable(MaterialTest src/MaterialTest.cpp)
  # Aver.Render.PBR.Materials is here for packMaterial alone. It carries Aver.RHI as a link
  # dependency, but nothing in this test creates a device: the GPU block is pure CPU-side packing,
  # and it is precisely the kind of hand-maintained cross-module layout that needs a test.
  target_link_libraries(MaterialTest PRIVATE
    Aver.Formats.Material Aver.Formats Aver.Render.PBR.Materials Aver.Core Aver.Platform)
  set_target_properties(MaterialTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
