# Aver.Render.Pcg -- the GPU half of procedural generation: a compute shader that fills a density
# volume from the parameter block F# produces.
#
# Depends on the RHI and nothing above it. It knows nothing about scenes, entities or gameplay:
# given a spec it produces a field, and what anyone does with that field is their business.
aver_add_module(Aver.Render.Pcg
  SOURCES
    src/PcgVolume.cpp
  DEPS
    Aver.RHI
    Aver.Core
)
