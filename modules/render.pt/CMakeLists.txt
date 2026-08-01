# Aver.Render.PathTracer — a brute-force path tracer, and the energy oracle that measures it.
#
# It exists to be CHECKED, not to be fast. Cosine-weighted hemisphere sampling makes the estimator
# exactly `albedo` -- the 1/PI of the Lambertian BRDF, the cosine of the rendering equation and the
# cos/PI of the pdf all cancel -- which leaves no loose constant to get wrong and, more usefully,
# leaves a WHITE FURNACE with a known answer: albedo 1 in a uniform environment of radiance L must
# read exactly L, at any bounce count, whatever the geometry.
#
# A SEPARATE MODULE rather than another pass inside Voxi, because it shares nothing with the voxel
# GI path and needs none of it: no volume, no cascades, no material system. It links the GENERIC RHI
# and never a backend, so the same rule that lets Aver.Render.Skin be driven by a recording device
# applies here, and it owns its own geometry -- meshes and world matrices in, traceable scenes out --
# so the self-test can construct exactly the geometry its arithmetic needs rather than borrowing a
# scene and hoping.
aver_add_module(Aver.Render.PathTracer
  SOURCES src/PathTracer.cpp src/PtFurnaceTest.cpp
  DEPS    Aver.RHI Aver.Core
)
