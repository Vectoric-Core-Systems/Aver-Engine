# Aver.Audio.Sdl -- the audio device everywhere but Windows: aver::audio::AudioDevice (declared in
# Aver.Audio) over an SDL 3 audio stream. The Windows twin is Aver.Audio.Wasapi, and exactly one of the
# two exists in any build. SDL's own audio thread is the render thread; the mixer is the same.
aver_add_module(Aver.Audio.Sdl
  SOURCES
    src/AudioDeviceSdl.cpp
  DEPS
    Aver.Core
    Aver.Audio
)
target_link_libraries(Aver.Audio.Sdl PRIVATE ${AVER_SDL3_TARGET})
