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
# applies here.
#
# PathTracer itself still owns its own geometry -- meshes and world matrices in, traceable scenes out
# -- which is what lets PtFurnaceTest construct exactly the geometry its arithmetic needs rather than
# borrowing a scene and hoping. PtSceneView (src/PtSceneView.cpp) is the OTHER consumer beside it: it
# is handed the REAL scene's draw list and camera (through rhi::IRenderFeature/rhi::IDevice, both
# already GENERIC RHI) and repackages them into the exact same PathTracer scene shape -- so the module
# still depends on nothing but the RHI itself, PtSceneView is just a second, streaming caller of it.
#
# NO MATERIAL DEPENDENCY, DELIBERATELY, AND THIS DEPS LINE IS WHERE THAT IS ENFORCED. PtSceneView does
# need a real level's authored base colour, but it obtains it through PtSceneView::AlbedoResolver -- a
# callback the HOST installs -- rather than by naming a pbr:: type, exactly as
# LandscapeRenderer::setSurfaceBinding takes opaque bytes instead of a material. An earlier revision
# linked Aver.Render.PBR.Materials behind an if(TARGET) guard so it could read MaterialConstants
# directly; that is what the resolver replaced, and re-adding it would put a material dependency back
# into a module whose whole premise is not having one.
aver_add_module(Aver.Render.PathTracer
  SOURCES src/PathTracer.cpp src/PtFurnaceTest.cpp src/PtSceneView.cpp
  DEPS    Aver.RHI Aver.Core
)

# HLSL beside the executable -- see cmake/AvModule.cmake.
aver_deploy_shaders(Aver.Render.PathTracer "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
