# AudioTest — the mixer, with no sound card.
#
# It links Aver.Audio and Aver.Core and nothing else, which is the module's whole architectural claim
# made into something the build enforces: a mixer that needed a device could not be tested here, and
# every property this file checks is one that no screenshot and no reading can decide.
add_executable(AudioTest src/AudioTest.cpp)
target_link_libraries(AudioTest PRIVATE Aver.Audio Aver.Core Aver.Platform)
set_target_properties(AudioTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# OcAudioTest — the container round-trip (headless) and the importer against whatever files this
# machine happens to carry. It ships no media of its own: a test that carries an audio file carries
# somebody's licence with it, so it LOOKS for one and reports a skip rather than a pass when there
# is none.
if(TARGET Aver.Formats.Audio)
  add_executable(OcAudioTest src/OcAudioTest.cpp)
  target_link_libraries(OcAudioTest PRIVATE Aver.Formats.Audio Aver.Formats Aver.Audio Aver.Core Aver.Platform)
  set_target_properties(OcAudioTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# AudioProbe — the other half, and deliberately NOT part of the headless suite: it needs a real
# device and it makes a noise. AudioTest checks the mixer without hardware, which is the right way to
# check a mixer and says nothing at all about a sound card.
if(TARGET Aver.Audio.Wasapi OR TARGET Aver.Audio.Sdl)
  add_executable(AudioProbe src/AudioProbe.cpp)
  target_link_libraries(AudioProbe PRIVATE Aver.Audio $<IF:$<TARGET_EXISTS:Aver.Audio.Wasapi>,Aver.Audio.Wasapi,Aver.Audio.Sdl>
                        Aver.Core Aver.Platform)
  set_target_properties(AudioProbe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# AudioStreamTest -- streamed playback (under-run handling, ring wrap, looping, WAV), crossfades,
# the occlusion filter and reverb zones. Links only the mixer: the decoder thread and the file
# source are in Aver.Audio, so none of it needs a device or a scene.
add_executable(AudioStreamTest src/AudioStreamTest.cpp)
target_link_libraries(AudioStreamTest PRIVATE Aver.Audio Aver.Core Aver.Platform)
set_target_properties(AudioStreamTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# AudioBusTest / AudioSpatialTest -- G5 S1 (Godot audio parity, docs/AUDIO.md "Bus graph"): the AudioEffect* set against reference responses,
# the bus graph's routing rules (send order, solo chains, bypass, sidechain, layouts) and Godot's AudioStreamPlayer3D maths, all offline.
add_executable(AudioBusTest src/AudioBusTest.cpp)
target_link_libraries(AudioBusTest PRIVATE Aver.Audio Aver.Core Aver.Platform)
set_target_properties(AudioBusTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_executable(AudioSpatialTest src/AudioSpatialTest.cpp)
target_link_libraries(AudioSpatialTest PRIVATE Aver.Audio Aver.Core Aver.Platform)
set_target_properties(AudioSpatialTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# AudioDeviceTest -- device robustness with no sound card: the output policy (loss mid-block, default-device change to another rate, no device,
# device back, a wedged or flapping device) against a fake device, the render thread against a fake, and the sample-rate rule.
add_executable(AudioDeviceTest src/AudioDeviceTest.cpp)
target_link_libraries(AudioDeviceTest PRIVATE Aver.Audio Aver.Core Aver.Platform)
set_target_properties(AudioDeviceTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# SoundGraphTest -- real-time sound graphs (docs/AUDIO.md section 13): the audio-thread path against the offline render, live inputs, note-off, the voice
# pool, and no allocation in mix() (counted with a replacement operator new). Offline; no device.
add_executable(SoundGraphTest src/SoundGraphTest.cpp)
target_link_libraries(SoundGraphTest PRIVATE Aver.Sound Aver.Audio Aver.Formats Aver.Core Aver.Platform)
set_target_properties(SoundGraphTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# AudioAbiTest -- the same through the plain-C seam in OFFLINE mode (the real mixer, no device), plus the .ocbus layout format.
if(TARGET Aver.Audio.Abi)
  add_executable(AudioAbiTest src/AudioAbiTest.cpp)
  target_link_libraries(AudioAbiTest PRIVATE Aver.Audio.Abi Aver.Formats.Audio Aver.Formats Aver.Audio Aver.Core Aver.Platform)
  set_target_properties(AudioAbiTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# AudioSceneTest -- CReverbZone and CAudioOcclusion on a real scene::World, mixer and raycast faked.
if(TARGET Aver.Audio.Scene)
  add_executable(AudioSceneTest src/AudioSceneTest.cpp)
  target_link_libraries(AudioSceneTest PRIVATE Aver.Audio.Scene Aver.Audio Aver.Scene Aver.Core Aver.Platform)
  set_target_properties(AudioSceneTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# AudioDecodeTest -- Ogg Vorbis / FLAC / MP3 (docs/AUDIO.md "Decoders"): FLAC and MP3 streams built in code and decoded exactly, the gapless
# trim, windows and loop points, hostile input for all three decoders, and (with the ABI target) a streamed FLAC against the same audio as a
# WAV through the offline mixer. Ships no audio file and needs no encoder. `AudioDecodeTest --file <path>` decodes one outside file and reports it.
if(TARGET Aver.Formats.Audio)
  add_executable(AudioDecodeTest src/AudioDecodeTest.cpp)
  target_link_libraries(AudioDecodeTest PRIVATE Aver.Formats.Audio Aver.Formats Aver.Audio Aver.Core Aver.Platform)
  if(TARGET Aver.Audio.Abi)
    target_link_libraries(AudioDecodeTest PRIVATE Aver.Audio.Abi)
    target_compile_definitions(AudioDecodeTest PRIVATE AVER_TEST_HAS_ABI=1)
  endif()
  set_target_properties(AudioDecodeTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# AudioSurroundTest -- surround output and binaural (docs/AUDIO.md section 14), no device: the pairwise pan and its energy, sources at known angles in
# 5.1 / 7.1, the stereo upmix, the LFE send, the device layout choice and channel map, the output policy changing layout against a fake device, the
# analytic head model (ITD sign and size, level, front/back and elevation cues, smooth interpolation), defaults unchanged, and no allocation in mix().
add_executable(AudioSurroundTest src/AudioSurroundTest.cpp)
target_link_libraries(AudioSurroundTest PRIVATE Aver.Audio Aver.Core Aver.Platform)
set_target_properties(AudioSurroundTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# AudioEnvironmentTest -- environment audio (docs/AUDIO.md section 15), no device: the partitioned FFT convolver against a direct time-domain
# convolution (exact within float tolerance, partition boundaries, chunking, swaps, tails, hostile input, rate changes) with a ms-per-block cost
# report for a 2 s impulse, the convolution reverb effect, the zone send reverbs in the mixer (independent slots, exact ramps, clean restart, no
# allocation in mix(), defaults unchanged), the zone share blend, and diffraction (monotonic with the blocker, through an opening, limits).
add_executable(AudioEnvironmentTest src/AudioEnvironmentTest.cpp)
target_link_libraries(AudioEnvironmentTest PRIVATE Aver.Audio Aver.Core Aver.Platform)
if(TARGET Aver.Formats.Audio)
  target_link_libraries(AudioEnvironmentTest PRIVATE Aver.Formats.Audio Aver.Formats)
  target_compile_definitions(AudioEnvironmentTest PRIVATE AVER_TEST_HAS_FORMATS=1)
endif()
set_target_properties(AudioEnvironmentTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
