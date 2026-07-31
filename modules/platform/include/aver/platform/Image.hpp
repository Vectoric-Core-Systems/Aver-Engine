#pragma once
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver {

// A decoded image, always expanded to 8-bit RGBA whatever the file held.
struct ImageData {
    u32 width = 0;
    u32 height = 0;
    u32 channels = 0;        // what the FILE held, 1..4; pixels is RGBA either way
    bool srgb = false;       // annotation only; the asset layer sets it, never the decoder
    std::vector<u8> pixels;  // width * height * 4, tightly packed, no row padding

    bool valid() const {
        return width > 0 && height > 0 && pixels.size() == static_cast<usize>(width) * height * 4;
    }
    u32 rowPitch() const { return width * 4; }
};

// Decodes an image file into RGBA. False on a missing or corrupt file, with the reason in `err`.
bool decodeImage(const std::string& path, ImageData& out, std::string* err = nullptr);
// Decodes an in-memory image into RGBA. False on an empty or corrupt buffer.
bool decodeImage(const u8* bytes, usize size, ImageData& out, std::string* err = nullptr);

} // namespace aver
