# Aver.UI.Abi — the plain-C seam onto the retained UI, so a HUD can be authored in C#.
#
# SHARED because the .NET host P/Invokes it, which is the only reason any module in this engine is a
# DLL. It links Aver.UI STATICALLY and Aver.Core, and nothing else: no RHI, no device, no window.
# That is what lets the same seam serve a headless tool as serve the editor.
#
# The file name is Aver.UI.Abi and the MANAGED contract assembly is Aver.UI, which is a deliberate
# difference. Aver.Scene and Aver.Framework share a file name with their managed halves and each
# needs a NativeResolver to stop DllImport loading the managed assembly as a native library; naming
# these two apart means the default probe finds the right file and no resolver is needed at all.
add_library(Aver.UI.Abi SHARED
  src/UiAbi.cpp
  src/UiWidgetAbi.cpp)   # the retained widget tree's C ABI, ui_widget_abi.h

target_include_directories(Aver.UI.Abi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(Aver.UI.Abi PUBLIC cxx_std_20)
# PRIVATE: nothing that consumes this header needs Aver.UI on its own link line, and a consumer that
# does need the draw list type (the host, which casts aver_ui_draw_list back) names it itself.
target_link_libraries(Aver.UI.Abi PRIVATE Aver.Core Aver.UI)

# Flips the export macro to dllexport while building this target.
target_compile_definitions(Aver.UI.Abi PRIVATE AVER_UI_ABI_BUILD)

set_target_properties(Aver.UI.Abi PROPERTIES
  FOLDER "modules"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)   # the DLL goes next to the exe, where DllImport looks
