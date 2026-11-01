#pragma once
// The decode / resize / mip helpers behind uploadTexture, shared with the atlas builder
// (TextureAtlas.cpp) so both treat a usage the same way. Private to Aver.Assets.Gpu.
#include "aver/assets/TextureCache.hpp"
#include "aver/assets/TextureUpload.hpp"
#include "aver/formats/Texture.hpp"

namespace aver::assets::detail {

// Decodes `path` for `usage` and stops at level 0 (no mips). Colour is sRGB, NormalMap is linear and
// filtered as vectors, Data is linear.
bool decodeTexture(const std::string& path, TextureUsage usage, fmt::TextureData& out, std::string* err);

// Builds levels 1..N down to 1x1 with the usage-aware filter (linear light for Colour, vectors for a
// normal map). No coverage preservation.
void buildMips(fmt::TextureData& t, TextureUsage usage);

// Resamples `src` to w x h: an area average when shrinking (in linear light for sRGB Colour), bilinear
// with wrap when growing, so a tileable source stays tileable. `out` keeps src's srgb annotation.
void resizeBox(const ImageData& src, u32 w, u32 h, TextureUsage usage, ImageData& out);

// A texture ready to become a GPU object: the BC7 chain (read from the derived-data cache or just
// compressed), else the decoded RGBA8 chain.
struct PreparedTexture {
    TextureUsage usage = TextureUsage::Colour;
    bool bc7 = false;
    bool fromCache = false;     // the chain came from the cache (or a prefetch), not from a decode
    CachedTexture cached;       // bc7
    fmt::TextureData rgba;      // !bc7
    TextureUploadInfo info;
};

// Everything uploadTexture does short of the GPU: the cache read or the decode, mips and BC7 encode.
// Safe on any thread. `allowCompressed` false skips the cache and the encoder (the RGBA8 path).
bool prepareTexture(const std::string& path, TextureUsage usage, PreparedTexture& out, std::string* err,
                    bool allowCompressed = true);
// The GPU half: creates the texture. 0 with `err` set when the RHI refuses it.
rhi::TextureHandle uploadPrepared(rhi::IResourceFactory& res, const std::string& path, PreparedTexture& p,
                                  std::string* err, TextureUploadInfo* info);

} // namespace aver::assets::detail
