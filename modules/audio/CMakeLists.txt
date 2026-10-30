# Aver.Audio — the mixer, and nothing that touches a device.
#
# CORE ONLY, which is the point of the module rather than an accident of what it happens to need.
# mix() fills a buffer the caller supplies; a backend hands that buffer to hardware. It is the same
# split that makes Aver.UI testable with no GPU, and audio needs it more: a wrong pan law and a right
# one are the same waveform to a reader and the same silence to a screenshot, so if the mixer cannot
# be run headlessly and asserted on sample by sample, it cannot be checked at all.
#
# See docs/AUDIO.md for the plan this implements, and modules/audio/README.md for what is and is not
# in it.
aver_add_module(Aver.Audio
  SOURCES
    src/Mixer.cpp
    src/Dsp.cpp
    src/Stream.cpp
    src/WavStream.cpp
    src/Reverb.cpp
    src/Music.cpp
    # Real-time sound graphs: the .ocsnd evaluator shared by the audio thread and Aver.Sound's offline render (docs/AUDIO.md section 13).
    src/SoundGraph.cpp
    # Bus graph (docs/AUDIO.md "Bus graph"): the AudioEffect* set, the bus graph and the 3D player maths.
    src/Effects.cpp
    src/BusGraph.cpp
    src/Spatial.cpp
    # Surround output (5.1 / 7.1 speaker panning, upmix, device layout choice) and the analytic binaural head model (docs/AUDIO.md section 14).
    src/Speakers.cpp
    src/Binaural.cpp
    # Environment (docs/AUDIO.md section 15): the partitioned FFT convolver behind the convolution reverb effect and the zone send reverbs.
    src/Convolver.cpp
    # Device robustness: the output policy (loss, default change, no device, retry) and the render thread, behind a backend seam a test can fake.
    src/OutputDriver.cpp
    src/AudioDevice.cpp
  DEPS
    Aver.Core
)

# The stream decoder runs on its own std::thread.
find_package(Threads QUIET)
if(Threads_FOUND)
  target_link_libraries(Aver.Audio PUBLIC Threads::Threads)
endif()
