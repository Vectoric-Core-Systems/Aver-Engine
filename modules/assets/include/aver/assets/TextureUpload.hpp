#pragma once
// The one place a decoded image becomes a GPU texture.
#include "aver/core/Types.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <string>

namespace aver::assets {

// The derived-data cache directory for BC7-compressed mip chains (TextureCache.hpp/.cpp), e.g.
// "<project>\Saved\DerivedDataCache\Textures". Empty -- the default -- means "no cache":
// uploadTexture decodes and uploads RGBA8 exactly as it always has, unconditionally. Declared here
// rather than in TextureCache.hpp so a caller that only points the cache at a project need not see
// the encoder or the on-disk format; implemented in TextureCache.cpp, which owns that state.
//
// Set ONCE PER PROJECT -- SandboxProject.cpp's applyProject and Runtime/src/GameApp.cpp's project
// open both call this before any material resolves a texture -- and safe to call or read from any
// thread meanwhile (guarded internally with a mutex, not merely documented as such).
void setTextureCacheDir(const std::string& dir);
std::string textureCacheDir();

// What the pixels mean. Decides both the mip filter and the view format.
enum class TextureUsage : u32 {
    Colour = 0,
    Data,
    NormalMap,
};

// What an upload produced, so a caller can log or budget without decoding again.
struct TextureUploadInfo {
    u32   width  = 0;
    u32   height = 0;
    u32   mips   = 0;
    usize bytes  = 0;   // total across the whole chain, as uploaded
    // The texture's MEAN colour, in LINEAR space, for a consumer that cannot sample it -- the
    // path tracer shades from a flat per-surface albedo and has no texture units at all.
    //
    // Free to compute: generateMipChain() already runs to 1x1 and downsample() averages in linear
    // space for an sRGB texture, so the smallest level IS this number. Taking it from the mip
    // chain rather than re-averaging level 0 also means it agrees exactly with what a shader
    // sampling the lowest mip would read.
    f32   averageLinear[3] = {1.0f, 1.0f, 1.0f};
};

// Decodes `path` and uploads it. Returns 0 on failure, with the reason in `err`.
rhi::TextureHandle uploadTexture(rhi::IResourceFactory& res, const std::string& path,
                                 TextureUsage usage, std::string* err = nullptr,
                                 TextureUploadInfo* info = nullptr);

} // namespace aver::assets
