# Aver.Audio.Wasapi — the device under Aver.Audio.
#
# The counterpart to Aver.RHI.D3D12 under Aver.RHI, and to Aver.Render.UI under Aver.UI: the ONLY
# thing in the audio stack that knows a sound card exists. That is what keeps the mixer testable
# with no hardware and replaceable with a second backend.
#
# WASAPI shared mode, chosen over XAudio2 because XAudio2 would own the mixing, the 3D and the DSP --
# which would make Aver.Audio a wrapper around a thing it cannot test and cannot port. See
# docs/AUDIO.md section 2.
aver_add_module(Aver.Audio.Wasapi
  SOURCES
    src/AudioDeviceWasapi.cpp
  DEPS
    Aver.Core
    Aver.Audio
    # avrt is the Pro Audio scheduling class. Without it the render thread is an ordinary one, and an
    # ordinary thread preempted for 10 ms is a gap somebody hears.
    avrt
    ole32
)
