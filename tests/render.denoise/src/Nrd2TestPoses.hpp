// Synthetic NRD2 pose files for Nrd2TrainerTest and Nrd2TrainerGpuTest: smooth feature fields whose
// per-tile oracle parameters are a known function of the tile's features (so a network can learn them).
#pragma once

#include "aver/render/denoise/Nrd2.hpp"
#include "aver/render/denoise/Nrd2Dataset.hpp"
#include "aver/render/denoise/Nrd2Trainer.hpp"

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace nrd2test {

using namespace aver;
using namespace aver::render::denoise;

// Features (12 channels, `frames` frames, a little per-frame variation), theta = defaults + 0.6 x the
// tile mean of channel (p % 12) of frame 0, tile weights in [0, 1] with every 7th tile 0.
inline Nrd2Pose makePose(const std::string& scene, u32 index, bool heldOut, u32 tilesX, u32 tilesY, u32 frames,
                         u32 seed) {
    Nrd2Pose p;
    p.stageBVersion = kNrd2StageBVersion;
    p.scene = scene;
    p.poseIndex = index;
    p.heldOut = heldOut;
    p.tilesX = tilesX; p.tilesY = tilesY;
    p.halfW = 4 * tilesX; p.halfH = 4 * tilesY;
    p.frames = frames;
    p.channels = kNrd2FeatureCount;
    const usize plane = static_cast<usize>(p.halfW) * p.halfH, tiles = static_cast<usize>(tilesX) * tilesY;
    p.features.resize(static_cast<usize>(frames) * 12 * plane);
    const f32 ph = 0.37f * static_cast<f32>(seed);
    for (u32 k = 0; k < frames; ++k)
        for (u32 c = 0; c < 12; ++c)
            for (u32 y = 0; y < p.halfH; ++y)
                for (u32 x = 0; x < p.halfW; ++x) {
                    const f32 v = std::sin(0.11f * (c + 1) * x + 0.07f * y + ph + c) +
                                  0.5f * std::cos(0.05f * x - 0.13f * (c + 2) * y + 2.0f * ph) + 0.02f * k +
                                  0.3f * static_cast<f32>(c % 3);
                    p.features[(static_cast<usize>(k) * 12 + c) * plane + static_cast<usize>(y) * p.halfW + x] =
                        nrd2F32ToF16(v);
                }
    f32 def[12];
    nrd2DefaultParams(def);
    p.theta.resize(12 * tiles);
    p.weights.resize(2 * tiles);
    p.losses.assign(4 * tiles, 0.0f);
    for (u32 ty = 0; ty < tilesY; ++ty)
        for (u32 tx = 0; tx < tilesX; ++tx) {
            const usize t = static_cast<usize>(ty) * tilesX + tx;
            for (u32 q = 0; q < 12; ++q) {
                f32 m = 0.0f;
                for (u32 y = 0; y < 4; ++y)
                    for (u32 x = 0; x < 4; ++x)
                        m += nrd2F16ToF32(p.features[static_cast<usize>(q) * plane + (ty * 4 + y) * p.halfW + tx * 4 + x]);
                p.theta[q * tiles + t] = def[q] + 0.6f * m / 16.0f;
            }
            p.weights[t] = (t % 7 == 3) ? 0.0f : 0.5f + 0.5f * std::sin(0.3f * tx + 0.2f * ty + ph);
            p.weights[tiles + t] = 0.25f + 0.75f * (0.5f + 0.5f * std::cos(0.2f * tx - 0.4f * ty));
        }
    return p;
}

inline std::string poseName(u32 i) {
    char b[32];
    std::snprintf(b, sizeof b, "pose_%03u.n2p", i);
    return b;
}

}  // namespace nrd2test
