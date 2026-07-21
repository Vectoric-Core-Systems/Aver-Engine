#pragma once
#include "aver/core/Types.hpp"

#include <string>
#include <vector>

namespace aver {

// A decoded image, ALWAYS expanded to 8-bit RGBA regardless of what the file held. One layout means
// every consumer -- the splash's DIB blit, the editor's start-screen mark, a material's base colour
// -- reads pixels the same way, and the expansion happens once inside the decoder rather than badly
// in three places.
struct ImageData {
    u32 width = 0;
    u32 height = 0;
    u32 channels = 0;        // what the FILE held, 1..4. pixels below is RGBA either way.
    bool srgb = false;       // ANNOTATION, never a decode result: no common image container states
                             // its colour space, so only the asset layer can answer this.
    std::vector<u8> pixels;  // width * height * 4, tightly packed, no row padding

    bool valid() const {
        return width > 0 && height > 0 && pixels.size() == static_cast<usize>(width) * height * 4;
    }
    u32 rowPitch() const { return width * 4; }
};

// Decode PNG/JPEG/TGA/BMP and friends. false on a missing or corrupt file, with the codec's own
// reason in err when one is asked for.
//
// This lives in Aver.Platform rather than anywhere nearer the renderer because the third-party
// decoder's single implementation TU has to live SOMEWHERE, and Platform is the lowest module that
// can own third_party/stb without a module depending upwards.
bool decodeImage(const std::string& path, ImageData& out, std::string* err = nullptr);
bool decodeImage(const u8* bytes, usize size, ImageData& out, std::string* err = nullptr);

} // namespace aver
