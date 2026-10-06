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
if(TARGET Aver.Audio.Wasapi)
  add_executable(AudioProbe src/AudioProbe.cpp)
  target_link_libraries(AudioProbe PRIVATE Aver.Audio Aver.Audio.Wasapi Aver.Core Aver.Platform)
  set_target_properties(AudioProbe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# AudioStreamTest -- streamed playback (under-run handling, ring wrap, looping, WAV), crossfades,
# the occlusion filter and reverb zones. Links only the mixer: the decoder thread and the file
# source are in Aver.Audio, so none of it needs a device or a scene.
add_executable(AudioStreamTest src/AudioStreamTest.cpp)
target_link_libraries(AudioStreamTest PRIVATE Aver.Audio Aver.Core Aver.Platform)
set_target_properties(AudioStreamTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# AudioSceneTest -- CReverbZone and CAudioOcclusion on a real scene::World, mixer and raycast faked.
if(TARGET Aver.Audio.Scene)
  add_executable(AudioSceneTest src/AudioSceneTest.cpp)
  target_link_libraries(AudioSceneTest PRIVATE Aver.Audio.Scene Aver.Audio Aver.Scene Aver.Core Aver.Platform)
  set_target_properties(AudioSceneTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
