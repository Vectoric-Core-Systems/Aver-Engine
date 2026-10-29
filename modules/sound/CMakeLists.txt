# Aver.Sound — turning a .ocsnd node graph into PCM, with no device and no mixer.
#
# PURE by construction, the same split Aver.Synapse makes against Aver.Synapse.Scene and for the
# same payoff: a graph and arithmetic go in and a buffer comes out, so the whole synthesiser is
# checkable against numbers a test computes for itself — a 440 Hz sine really crossing zero 440
# times a second — on a machine with no sound card. It links the mixer LIBRARY (Aver.Audio, Core-only,
# never a device) for one reason: the evaluator lives there, so the offline render and the audio thread
# run the same code. See the header.
aver_add_module(Aver.Sound
  SOURCES
    src/Synth.cpp
  DEPS
    Aver.Formats
    # The evaluator: renderSound runs the same SoundRunner the mixer runs on the audio thread (modules/audio/include/aver/audio/SoundGraph.hpp).
    Aver.Audio
    Aver.Core
)

# Carried PUBLIC so a composition root can #if around procedural audio entirely, matching
# Aver.Synapse's own definition.
target_compile_definitions(Aver.Sound PUBLIC AVER_MODULE_SOUND=1)
