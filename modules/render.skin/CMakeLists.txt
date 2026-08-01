# Aver.Render.Skin — GPU linear-blend skinning, as one compute dispatch per skinned instance.
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
set(AVER_SKIN_SOURCES src/SkinningPass.cpp src/SkinSelfTest.cpp)
set(AVER_SKIN_DEPS Aver.RHI Aver.Anim Aver.Formats Aver.Core)

# The scene join is CONDITIONAL, guarded the way modules/anim.scene is and for the same reason: it
# is the edge between skinning and the entity world, neither owns it, and a build with the scene
# switched off simply does not have it. The pass itself still exists there -- a game that drives
# skinning by hand needs no World.
if(TARGET Aver.Anim.Scene)
  list(APPEND AVER_SKIN_SOURCES src/SkinnedScene.cpp)
  # CSkeletalMesh had no reader anywhere in the tree until SkinnedScene. Naming these here is what
  # makes "which entities are skinned" a question this module answers rather than one every host
  # has to answer again.
  list(APPEND AVER_SKIN_DEPS Aver.Anim.Scene Aver.Scene)
endif()

aver_add_module(Aver.Render.Skin
  SOURCES ${AVER_SKIN_SOURCES}
  DEPS    ${AVER_SKIN_DEPS}
)
