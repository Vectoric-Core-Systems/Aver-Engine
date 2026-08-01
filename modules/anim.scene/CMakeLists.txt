# Aver.Anim.Scene — the join between a scene and the animation sampler.
#
# A SECOND TARGET rather than an edge added to either side, for the reason Aver.Assets.Gpu is one.
# Aver.Scene links Core and Assets and may NOT gain a Formats edge: its components carry opaque asset
# ids so nothing unmarshallable reaches the P/Invoke boundary. Turning one of those ids into a loaded
# .ocskel therefore cannot live there, and putting it in Aver.Anim would give the sampler a scene it
# has no business knowing about. It lives here, one tier above both, and only a composition root
# links it.
#
# Still no RHI: a posed skeleton is a list of matrices, and drawing with them is a further tier up.
aver_add_module(Aver.Anim.Scene
  SOURCES
    src/AnimSystem.cpp
  DEPS
    Aver.Anim
    Aver.Scene
    Aver.Core
)
