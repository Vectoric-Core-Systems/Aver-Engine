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
#
# The device is whichever of Aver.Audio.Wasapi (Windows) and Aver.Audio.Sdl (everywhere else) this
# build has; both implement the AudioDevice that Aver.Audio declares.
if(TARGET Aver.Audio.Wasapi)
  set(_aver_audio_device Aver.Audio.Wasapi)
elseif(TARGET Aver.Audio.Sdl)
  set(_aver_audio_device Aver.Audio.Sdl)
endif()
if(_aver_audio_device AND TARGET Aver.Formats.Audio)
  add_library(Aver.Audio.Abi SHARED src/AudioAbi.cpp src/StreamSources.cpp)
  target_include_directories(Aver.Audio.Abi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_features(Aver.Audio.Abi PUBLIC cxx_std_20)
  # PRIVATE: nothing that includes this header needs the mixer on its own link line.
  # Aver.Sound: aver_audio_graph_* compiles a .ocsnd into the program the mixer evaluates on the audio thread.
  target_link_libraries(Aver.Audio.Abi PRIVATE Aver.Core Aver.Audio ${_aver_audio_device} Aver.Formats.Audio Aver.Sound)
  target_compile_definitions(Aver.Audio.Abi PRIVATE AVER_AUDIO_ABI_BUILD)
  # Media Foundation, read incrementally for streamed music (the importer already links it for whole-file decode).
  if(WIN32)
    target_link_libraries(Aver.Audio.Abi PRIVATE mfplat mfreadwrite mfuuid ole32)
  endif()
  set_target_properties(Aver.Audio.Abi PROPERTIES
    FOLDER "modules"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)   # beside the exe, where DllImport looks
endif()
