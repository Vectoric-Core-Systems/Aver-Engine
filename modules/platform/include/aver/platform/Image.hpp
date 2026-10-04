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

// Names a container this decoder positively cannot read, or null if it says nothing about the bytes.
//
// A REFUSAL LIST, NOT AN ACCEPT LIST, and deliberately: TGA has no magic bytes at all, so an
// accept-list would have to fall back on the extension and would reject every valid .tga whose name
// said something else. Anything unrecognised is let through to stb_image to judge.
//
// LIVES BESIDE decodeImage because it is a statement about decodeImage's reach, and because two
// callers now need it: the material cook, which refuses to bind a slot it cannot decode, and the
// importers, which look for a sibling they can.
const char* undecodableContainer(const u8* bytes, usize size);
const char* undecodableContainer(const std::vector<u8>& bytes);

// Given an absolute path whose contents this decoder cannot read, finds a same-stem sibling beside
// it that it can -- `Leaf_BaseColor.tif` -> `Leaf_BaseColor.png`. Returns the path found, or empty.
//
// WHY THIS EXISTS. The cook's own advice was "convert it to PNG or TGA and re-import", which could
// never work: re-importing reads the .tif path back out of the source document, so the converted
// file was never looked at. This is that advice, automated.
//
// ORDER IS DELIBERATE: .png and .tga carry alpha, .jpg cannot, and a base colour's alpha is where
// this engine keeps a cutout -- so a JPEG sibling is a last resort, not a peer.
std::string findDecodableSibling(const std::string& absPath);

} // namespace aver
