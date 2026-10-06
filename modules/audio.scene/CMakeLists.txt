# Aver.Audio.Scene -- the join between the pure mixer and a live scene::World.
#
# A SECOND TARGET for the reason Aver.Synapse.Scene and Aver.Anim.Scene are: Aver.Audio stays
# Core-only so reverb-zone blending, the occlusion probe and the filter can be asserted headlessly,
# and this module is what reads CReverbZone / CAudioOcclusion off entities each frame.
aver_add_module(Aver.Audio.Scene
  SOURCES
    src/AudioScene.cpp
  DEPS
    Aver.Audio
    Aver.Scene
    Aver.Core
)

# The default control channel is the C ABI (listener read-back, reverb, per-voice occlusion).
# Optional: without it a composition root installs its own AudioControl.
if(TARGET Aver.Audio.Abi)
  target_link_libraries(Aver.Audio.Scene PUBLIC Aver.Audio.Abi)
  target_compile_definitions(Aver.Audio.Scene PUBLIC AVER_MODULE_AUDIO_ABI=1)
endif()

# Physics, for the default occlusion raycast. Optional, as in Aver.Synapse.Scene.
if(TARGET Aver.Physics)
  target_link_libraries(Aver.Audio.Scene PUBLIC Aver.Physics)
endif()

target_compile_definitions(Aver.Audio.Scene PUBLIC AVER_MODULE_AUDIO_SCENE=1)
