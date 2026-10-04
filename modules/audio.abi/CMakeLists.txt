# Aver.Audio.Abi — the plain-C seam onto the audio stack, so a game triggers its own sounds.
#
# SHARED because the .NET host P/Invokes it, which is the only reason any module here is a DLL. It
# links the DEVICE rather than just the mixer, because "play a sound" only means anything once
# something is driving a sound card.
#
# The file name is Aver.Audio.Abi and the managed contract assembly is Aver.Audio -- deliberately
# different, the way Aver.UI.Abi and Aver.UI are. Aver.Scene and Aver.Framework share a name with
# their managed halves and each needs a NativeResolver to stop DllImport loading the managed
# assembly as a native library; naming these apart removes the problem instead of working around it.
if(WIN32 AND TARGET Aver.Audio.Wasapi AND TARGET Aver.Formats.Audio)
  add_library(Aver.Audio.Abi SHARED src/AudioAbi.cpp)
  target_include_directories(Aver.Audio.Abi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_features(Aver.Audio.Abi PUBLIC cxx_std_20)
  # PRIVATE: nothing that includes this header needs the mixer on its own link line.
  target_link_libraries(Aver.Audio.Abi PRIVATE Aver.Core Aver.Audio Aver.Audio.Wasapi Aver.Formats.Audio)
  target_compile_definitions(Aver.Audio.Abi PRIVATE AVER_AUDIO_ABI_BUILD)
  set_target_properties(Aver.Audio.Abi PROPERTIES
    FOLDER "modules"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)   # beside the exe, where DllImport looks
endif()
