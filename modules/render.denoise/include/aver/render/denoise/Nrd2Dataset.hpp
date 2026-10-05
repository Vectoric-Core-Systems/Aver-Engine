// Nrd2Dataset -- NRD2 training pose files (docs/rendering/NRD2.md, phase 3). CPU only.
//
// One file per captured pose, <dir>/pose_NNN.n2p; default dir %LOCALAPPDATA%\AverEngine\nrd2_dataset\<scene>.
// Binary, little-endian:
//   u32 magic 'N2PS' (0x5350324E), version (1), stageBVersion (kNrd2StageBVersion at capture),
//       poseIndex, heldOut (0/1), halfW, halfH, tilesX, tilesY, frames K, channels C, sceneBytes
//   u8  scene[sceneBytes], zero-padded to a multiple of 4
//   u16 features[K][C][halfH][halfW]          fp16 (CSNrd2Features, NCHW per frame)
//   f32 theta[12][tilesY][tilesX]             oracle parameters, plane = signal * 6 + field
//   f32 weights[2][tilesY][tilesX]            tile weight per signal (validity x split-half confidence)
//   f32 losses[4][tilesY][tilesX]             oracle D, oracle S, default D, default S (data loss)
//   u32 crc32                                 CRC-32 IEEE over every preceding byte
// halfW == 4 * tilesX and halfH == 4 * tilesY (whole tiles; texels past the viewport have validity 0).
#pragma once

#include <aver/core/Types.hpp>

#include <string>
#include <vector>

namespace aver::render::denoise {

inline constexpr u32 kNrd2PoseMagic    = 0x5350324Eu;   // "N2PS"
inline constexpr u32 kNrd2PoseVersion  = 1u;
inline constexpr u32 kNrd2FeatureCount = 12u;
inline constexpr u32 kNrd2PoseLosses   = 4u;

struct Nrd2Pose {
    u32 stageBVersion = 0;
    std::string scene;
    u32 poseIndex = 0;
    bool heldOut = false;
    u32 halfW = 0, halfH = 0, tilesX = 0, tilesY = 0;
    u32 frames = 0, channels = kNrd2FeatureCount;
    std::vector<u16> features;   // frames * channels * halfH * halfW
    std::vector<f32> theta;      // 12 * tiles
    std::vector<f32> weights;    // 2 * tiles
    std::vector<f32> losses;     // 4 * tiles
};

// False (with a reason) when the shape is inconsistent or the write fails.
bool writeNrd2Pose(const std::string& path, const Nrd2Pose& pose, std::string* why = nullptr);
// Rejects a wrong magic or version, an inconsistent shape, truncation, trailing bytes and a bad CRC.
// `pose` is untouched on failure.
bool readNrd2Pose(const std::string& path, Nrd2Pose& pose, std::string* why = nullptr);

// IEEE half, round to nearest even (HLSL f32tof16 semantics for finite values; NaN stays NaN).
u16 nrd2F32ToF16(f32 v);
f32 nrd2F16ToF32(u16 h);

}  // namespace aver::render::denoise
