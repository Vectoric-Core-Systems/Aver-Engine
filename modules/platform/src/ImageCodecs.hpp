#pragma once
// The three decoders behind aver::decodeImage that are not stb_image. Each lives in its own translation
// unit so the vendored library it wraps never leaks a header into another. Internal to Aver.Platform.
#include "aver/platform/Image.hpp"

namespace aver::detail {

// libwebp. `err` is filled on failure.
bool decodeWebp(const u8* data, usize size, ImageData& out, std::string* err);
// NanoSVG. reqW/reqH as ImageDecodeOptions.
bool decodeSvg(const u8* data, usize size, u32 reqW, u32 reqH, ImageData& out, std::string* err);
// tinyexr.
bool decodeExr(const u8* data, usize size, FloatImageData& out, std::string* err);

// Float RGBA to the 8-bit form decodeImage hands out (clamp, 1/2.2 power curve on colour, linear alpha).
void floatToRgba8(const FloatImageData& in, ImageData& out);

} // namespace aver::detail
