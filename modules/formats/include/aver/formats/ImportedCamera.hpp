#pragma once
// A camera a model file carried, already in engine space (centimetres, left-handed Z up). The shape
// is shared so another importer (USD) can adopt it: the editor and AverAssetC turn the first one into
// the level's start camera.
#include "aver/core/Math.hpp"

#include <string>

namespace aver::fmt {

struct ImportedCamera {
    std::string name;
    Vec3 position{0, 0, 0};
    f32  yawDeg   = 0.0f;
    f32  pitchDeg = 0.0f;
    f32  fovDeg   = 60.0f;     // vertical
};

} // namespace aver::fmt
