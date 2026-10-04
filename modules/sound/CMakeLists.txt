# Aver.Sound — turning a .ocsnd node graph into PCM, with no device and no mixer.
#
# PURE by construction, the same split Aver.Synapse makes against Aver.Synapse.Scene and for the
# same payoff: a graph and arithmetic go in and a buffer comes out, so the whole synthesiser is
# checkable against numbers a test computes for itself — a 440 Hz sine really crossing zero 440
# times a second — on a machine with no sound card. See the header for why this renders offline
# rather than on the audio thread, and what that costs.
aver_add_module(Aver.Sound
  SOURCES
    src/Synth.cpp
  DEPS
    Aver.Formats
    Aver.Core
)

# Carried PUBLIC so a composition root can #if around procedural audio entirely, matching
# Aver.Synapse's own definition.
target_compile_definitions(Aver.Sound PUBLIC AVER_MODULE_SOUND=1)
