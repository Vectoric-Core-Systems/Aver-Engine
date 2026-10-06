# Aver.Anim — sampling .ocanim clips into poses, and posing a skeleton.
#
# CORE AND FORMATS ONLY, and that is the module's claim about itself rather than an accident: posing
# is arithmetic on a bone hierarchy, so all of it is decidable with no GPU and no device. Skinning a
# MESH with these matrices is somebody else's job, one tier up.
#
# .ocskel and .ocanim have existed and round-tripped since the format work, and until this module
# nothing in the engine read them. This is their first consumer.
aver_add_module(Aver.Anim
  SOURCES
    src/Pose.cpp
    src/AnimSampler.cpp
    src/Ik.cpp
    # Blend spaces, state machines and their .ocblend/.ocasm assets: arithmetic and JSON only.
    src/BlendSpace.cpp
    src/AnimStateMachine.cpp
    src/AnimGraphAsset.cpp
  DEPS
    Aver.Core
    Aver.Formats
)
