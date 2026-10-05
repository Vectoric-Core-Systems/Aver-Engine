// WeightFile -- AVNN version 2: the convolutional network's weight file. The v1 MLP file stays
// saveWeightFile / loadWeightFile in MlpReference.hpp, unchanged; peekWeightFile tells the two apart.
//
// Binary format, little-endian:
//   u32 magic 'AVNN' (0x4E4E5641), u32 version 2, u32 kind (1 = ConvNet), u32 headerBytes
//       headerBytes = byte offset of the totalWeights field (everything before it), so a reader can
//       tell the header it understands from one it does not
//   u32 inChannels, layerCount, flags (bit0: io affine present), reserved
//   layerCount x { u32 cin, cout, kernel, stride, activation, bias, weightCount, reserved }
//   if bit0: f32 inScale[inChannels], inBias[inChannels], outScale[outC], outBias[outC]  (outC = last cout)
//   u32 totalWeights
//   f32 weights[totalWeights]       (ConvLayout order)
//   u32 crc32                       (CRC-32 IEEE over every preceding byte)
// Readers reject a wrong magic, version or kind, an invalid shape, a count that disagrees with the shape,
// truncation, trailing bytes and a bad CRC.
#pragma once

#include <aver/core/Types.hpp>
#include <aver/render/neural/ConvNetReference.hpp>

#include <span>
#include <string>
#include <vector>

namespace aver::render::neural {

// kWeightFileMagic and the v1 version are in MlpReference.hpp.
inline constexpr u32 kWeightFileVersion2 = 2u;
inline constexpr u32 kWeightKindMlp      = 0u;   // what peekWeightFile reports for a v1 file
inline constexpr u32 kWeightKindConvNet  = 1u;

// Input / output standardisation the network was trained with: x' = x * inScale + inBias on the way in,
// y = y' * outScale + outBias on the way out. Empty vectors mean absent.
struct ConvIoAffine {
    std::vector<f32> inScale, inBias, outScale, outBias;
};

// CRC-32 IEEE (reflected, poly 0xEDB88320, init and final xor 0xFFFFFFFF); crc32Ieee("123456789") == 0xCBF43926.
u32 crc32Ieee(std::span<const u8> bytes);

// Reads only the magic and version (and the kind, v2). v1 files report kind kWeightKindMlp. False when the
// file is missing, too short or not AVNN.
bool peekWeightFile(const std::string& path, u32& version, u32& kind);

// `io` null writes no affine; otherwise its four vectors must be inChannels, inChannels, outC, outC long.
// False on an invalid descriptor, a weight count that disagrees with it, a bad affine or a write failure.
bool saveConvWeightFile(const std::string& path, const ConvNetDesc& d, std::span<const f32> weights,
                        const ConvIoAffine* io = nullptr);

// Fills d (keeping the caller's seed), weights and, when io is non-null, the affine (cleared when the file
// has none). Outputs are untouched on failure.
bool loadConvWeightFile(const std::string& path, ConvNetDesc& d, std::vector<f32>& weights,
                        ConvIoAffine* io = nullptr);

}  // namespace aver::render::neural
