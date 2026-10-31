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

// A decoded image kept as floating point: linear RGBA, top row first, values not clamped.
struct FloatImageData {
    u32 width = 0;
    u32 height = 0;
    u32 channels = 0;        // what the FILE held (Y=1, Y+A=2, RGB=3, RGBA=4); rgba is RGBA either way
    std::vector<f32> rgba;   // width * height * 4; alpha is 1 when the file has none

    bool valid() const {
        return width > 0 && height > 0 && rgba.size() == static_cast<usize>(width) * height * 4;
    }
};

// Limits the decoders below apply to what a HEADER claims, before they allocate for it. Hostile or
// corrupt files state any size they like. stb_image's own formats keep stb's checks and one pixel count
// on top (kMaxStbImagePixels), read from the header before the pixels are decoded.
inline constexpr u32 kMaxImageDimension = 16384;            // either side
inline constexpr u64 kMaxImagePixels = u64(1) << 27;        // 8-bit output: 512 MB (WebP, SVG)
inline constexpr u64 kMaxStbImagePixels = u64(1) << 28;     // 8-bit output: 1 GB (a 16384 x 16384 image)
inline constexpr u64 kMaxFloatImagePixels = u64(1) << 25;   // float output: 512 MB (an 8192 x 4096 HDRI)
inline constexpr usize kMaxImageFileBytes = usize(256) << 20;   // a WebP / OpenEXR file read whole
inline constexpr usize kMaxSvgBytes = usize(8) << 20;       // an SVG document

// What the SVG rasteriser is asked for. Zero means "the document's own size" (its width/height, else its
// viewBox, at 96 dpi). One side set keeps the aspect ratio; both set fit the drawing inside that box
// (uniform scale, centred) and the image is exactly that size. Other formats ignore it.
struct ImageDecodeOptions {
    u32 svgWidth = 0;
    u32 svgHeight = 0;
};

// Decodes an image file into RGBA. False on a missing or corrupt file, with the reason in `err`.
//
// Reads PNG/JPEG/TGA/BMP/GIF/PSD/PIC/PNM/Radiance HDR through stb_image, and, chosen by what the BYTES
// are (the extension does not matter), WebP (lossy, lossless, alpha; an animated file gives its first
// frame at canvas size), SVG (rasterised, see ImageDecodeOptions) and OpenEXR (half or float; clamped to
// [0,1] and encoded with the 1/2.2 power curve stb_image gives Radiance HDR, so an HDR value above 1
// clips: use decodeFloatImage to keep it).
bool decodeImage(const std::string& path, ImageData& out, std::string* err = nullptr);
bool decodeImage(const std::string& path, ImageData& out, const ImageDecodeOptions& opt, std::string* err = nullptr);
// Decodes an in-memory image into RGBA. False on an empty or corrupt buffer.
bool decodeImage(const u8* bytes, usize size, ImageData& out, std::string* err = nullptr);
bool decodeImage(const u8* bytes, usize size, ImageData& out, const ImageDecodeOptions& opt, std::string* err = nullptr);

// Decodes an image that can carry values outside [0,1] into float RGBA. Today that is OpenEXR (half and
// float channels, scanline and one-level tiled; mipmapped / ripmapped, deep and multipart files are refused).
// False on any other container, or a corrupt file, with the reason in `err`.
bool decodeFloatImage(const std::string& path, FloatImageData& out, std::string* err = nullptr);
bool decodeFloatImage(const u8* bytes, usize size, FloatImageData& out, std::string* err = nullptr);

// Whether the bytes are one of the formats decodeImage picks by content rather than by stb_image.
bool isWebpData(const u8* bytes, usize size);
bool isOpenExrData(const u8* bytes, usize size);
bool isSvgData(const u8* bytes, usize size);
// ".webp", ".exr" or ".svg" when the bytes are one of those, else null.
const char* imageExtensionFor(const u8* bytes, usize size);

// Names a container this decoder positively cannot read, or null if it says nothing about the bytes.
//
// A REFUSAL LIST, NOT AN ACCEPT LIST, and deliberately: TGA has no magic bytes at all, so an
// accept-list would have to fall back on the extension and would reject every valid .tga whose name
// said something else. Anything unrecognised is let through to stb_image to judge. WebP and OpenEXR
// left this list when they gained decoders; TIFF, DDS, KTX, AVIF and HEIF stay refused.
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
// ORDER IS DELIBERATE: .png, .tga and .webp carry alpha, .jpg cannot, and a base colour's alpha is where
// this engine keeps a cutout -- so a JPEG sibling is a last resort, not a peer.
std::string findDecodableSibling(const std::string& absPath);

} // namespace aver
