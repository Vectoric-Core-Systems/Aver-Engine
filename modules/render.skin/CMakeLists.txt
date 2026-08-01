# Aver.Render.Skin — GPU linear-blend skinning, as one compute dispatch per skinned mesh.
#
# The parity partner of Aver.Anim's CPU skinVertices: the shader implements the same contract term
# for term, and the two are checked against each other on a real device rather than asserted to
# agree. Divergence here is not a rendering artefact -- a skinned mesh whose GPU and CPU positions
# disagree breaks anything that hit-tests, attaches to, or replicates a bone.
#
# It links the GENERIC RHI and never a backend, which is what lets a recording device drive it with
# no GPU -- the property Aver.Render.UI established.
#
# RENDERING.md 7.5: this is the "separate skinning compute pass" that composes with the cage deform
# by feeding the same output buffer.
aver_add_module(Aver.Render.Skin
  SOURCES
    src/SkinningPass.cpp
    src/SkinSelfTest.cpp
  DEPS
    Aver.RHI
    Aver.Anim
    Aver.Formats
    Aver.Core
)
