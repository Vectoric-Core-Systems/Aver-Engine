# Aver.Render.NRD -- the engine's seam onto NVIDIA Real-Time Denoisers (see README.md).
#
# THIS TARGET EXISTS WHETHER OR NOT NRD DOES, and that is deliberate. cmake/AverNRD.cmake turns
# AVER_WITH_NRD off by itself on a machine with no offline shader compiler, and a module that
# vanished with it would take every consumer's link line with it -- the exact failure mode
# aver_check_module_dag() exists to catch. Instead the target is always built and always links; only
# the NRD edge and the AVER_WITH_NRD define are conditional, and NrdDenoiser.cpp compiles both ways.
# A caller writes `if (!denoiser.create(...)) { /* hand-written filter */ }` and needs no #if.
add_library(Aver.Render.NRD STATIC
  src/NrdDenoiser.cpp)

target_include_directories(Aver.Render.NRD PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(Aver.Render.NRD PUBLIC cxx_std_20)

# Aver.RHI is the GENERIC interface, for rhi::Format and nothing else. Never a backend: this module
# describes dispatches, it does not record them -- same rule as Aver.Occlusion.
target_link_libraries(Aver.Render.NRD PUBLIC Aver.Core Aver.RHI)

if(AVER_WITH_NRD AND TARGET NRD)
  # PRIVATE, both of them. NRD's headers are not permissively licensed (third_party/nrd/
  # AVER_README.md), so nothing downstream of this target gets them on its include path, and
  # NrdDenoiser.hpp is written so that nothing downstream needs them.
  target_link_libraries(Aver.Render.NRD PRIVATE NRD)
  target_compile_definitions(Aver.Render.NRD PRIVATE AVER_WITH_NRD=1)
else()
  target_compile_definitions(Aver.Render.NRD PRIVATE AVER_WITH_NRD=0)
endif()

# Consumers get AVER_MODULE_RENDER_NRD so they can #if around a feature that wants the seam at all,
# same idiom as every other optional module. It says the SEAM is present, not that NRD is -- that
# second question is Denoiser::available(), answered at runtime, because it is the one a fallback
# path actually has to ask.
target_compile_definitions(Aver.Render.NRD PUBLIC AVER_MODULE_RENDER_NRD=1)

set_target_properties(Aver.Render.NRD PROPERTIES FOLDER "modules")
