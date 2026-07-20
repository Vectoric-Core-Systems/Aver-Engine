# Aver.Render.Voxi — optional render-feature module (see README.md).
#
# Built SHARED on purpose: the C# scripting layer P/Invokes this DLL, and keeping the settings
# state in one binary means the editor and the bindings agree on the contract. Depends only on
# Aver.Core, so nothing of the RHI leaks into the DLL.
add_library(Aver.Render.Voxi SHARED
  src/Voxi.cpp)

target_include_directories(Aver.Render.Voxi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(Aver.Render.Voxi PUBLIC cxx_std_20)
target_link_libraries(Aver.Render.Voxi PUBLIC Aver.Core)

# AVER_VOXI_BUILD flips the export macros to dllexport while building this target.
target_compile_definitions(Aver.Render.Voxi PRIVATE AVER_VOXI_BUILD)
# Consumers get AVER_MODULE_VOXI so they can #if around the whole feature.
target_compile_definitions(Aver.Render.Voxi PUBLIC AVER_MODULE_VOXI=1)

set_target_properties(Aver.Render.Voxi PROPERTIES
  FOLDER "modules"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)   # DLL next to the exe
