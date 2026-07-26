aver_add_module(Aver.Assets
  SOURCES
    src/AssetId.cpp
  DEPS
    Aver.Core
    Aver.Platform
)

# Aver.Assets.Gpu — the decode-to-GPU join, deliberately a SECOND target.
#
# It links Aver.Formats (which links Aver.Assets above, so this must not be the same target) and the
# GENERIC Aver.RHI. Keeping it separate is what lets Aver.Assets stay a leaf: a headless tool that
# only needs asset ids never drags in a decoder or the RHI.
aver_add_module(Aver.Assets.Gpu
  SOURCES
    src/TextureUpload.cpp
  DEPS
    Aver.Core
    Aver.Formats
    Aver.RHI
)
