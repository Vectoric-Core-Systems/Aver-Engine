# Aver.Hair -- strand hair authoring and the bridge to Jolt's Hair, with no scene, no GPU and no Jolt header.
#
# Pure by construction, like Aver.SoftBody: the procedural groom (HairGenerate), glTF curves to strands (HairCurves), collision proxy hull points
# and the default humanoid proxy set (HairProxy) are arithmetic on arrays, so tests/hair decides them on a machine with no device. The one
# thing that is not arithmetic is HairSettingsBuild: it drives aver_phys_hair_settings_* of Aver.Physics, through the plain-C ABI only (the
# physics header, never a Jolt one), so the module still depends on Core, Formats and Physics's C ABI and nothing above.
#
# The simulation itself is Jolt's, inside Aver.Physics; the scene half (CHair, HairSystem) is Aver.Hair.Scene and the drawing half is
# Aver.Render.Hair (later phases of docs/plans/wave11/SOFTBODY_HAIR.md).
#
# Added by the root CMakeLists.txt behind AVER_MODULE_HAIR (default on); docs/HAIR.md.
aver_add_module(Aver.Hair
  SOURCES
    src/HairGenerate.cpp
    src/HairCurves.cpp
    src/HairProxy.cpp
    src/HairSettingsBuild.cpp
  DEPS
    Aver.Core
    Aver.Formats
    Aver.Physics
  PUBLIC_DEFS
    AVER_MODULE_HAIR=1
)
