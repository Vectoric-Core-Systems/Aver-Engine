#pragma once
// A derived-data cache of GPU-READY, BC7-compressed mip chains. modules/formats/src/Bc7.cpp does the
// encoding (see Bc7.hpp); this remembers the result under textureCacheDir() so a texture pays for
// the decode-and-compress work once per (file, usage) rather than on every load.
//
// setTextureCacheDir/textureCacheDir themselves are declared in TextureUpload.hpp, not here: a
// caller that only points the cache at a project (SandboxProject.cpp, Runtime/src/GameApp.cpp) has
// no reason to see the on-disk format or the encoder this header pulls in. They are IMPLEMENTED in
// TextureCache.cpp, since the directory is exactly the state this file's read/write pair needs.
//
// THE WHOLE POINT OF A HIT IS ZERO DECODE -- "hit -> upload the cached chain directly, no image
// decode at all" is the load-time win TextureUpload.cpp's caller sees. That constrains the cache KEY
// to whatever a stat() of the source file and the caller's own arguments can say: the path, its size
// and last-write time, the requested usage, and the encoder/container versions. Nothing that only
// the DECODED PIXELS could answer -- in particular alphaLooksLikeCutout's verdict -- may feed it;
// see TextureCache.cpp's cacheKey() for exactly what goes in and why each field does.
#include "aver/assets/TextureUpload.hpp"   // TextureUsage
#include "aver/core/Types.hpp"
#include "aver/rhi/RHIResources.hpp"       // rhi::Format

#include <string>
#include <vector>

namespace aver::assets {

// Bumped whenever the .octex CONTAINER changes shape (header fields, mip packing) -- independent of
// fmt::kBc7EncoderVersion, which the key also carries, so a better encoder and a changed container
// each invalidate the cache on their own rather than one silently standing in for the other.
inline constexpr u32 kTextureCacheFormatVersion = 1;

// True unless AVER_TEXTURE_COMPRESSION=0 -- the A/B switch the whole feature reads before touching
// either the encoder or the cache. Decided once (the environment cannot change under a running
// process) and logged exactly once, from whichever thread asks first; see the .cpp for why a
// function-local magic static is enough to make that safe without a lock of its own.
bool textureCompressionEnabled();

// width/height eligible for BC7 at all: block-compressible extents (multiples of 4) and big enough
// that a lossy re-encode is worth it -- a small UI icon (crosshair, cursor) stays exact RGBA8.
bool textureCompressionEligible(u32 width, u32 height);

// One decoded-and-compressed mip chain, ready to become a TextureDesc's initialData as-is.
struct CachedTexture {
    rhi::Format format = rhi::Format::Unknown;   // BC7Unorm (data/normal) or BC7UnormSrgb (colour)
    u32 width = 0, height = 0, mips = 0;
    f32 averageLinear[3] = {1.0f, 1.0f, 1.0f};   // see TextureUploadInfo::averageLinear
    // Whether THIS entry's mips were baked with coverage-preserving alpha. Purely informational --
    // stored in the .octex header's flags for a tool or a log line to read, never consulted by
    // loadCachedTexture to decide anything, because nothing here may depend on the pixels a hit did
    // not decode.
    bool coveragePreserved = false;
    std::vector<std::vector<u8>> mipBlocks;      // one tightly-packed BC7 block buffer per level
};

// Reads the cache entry for `path`+`usage` into `out`. False whenever it is not there, not usable,
// or textureCacheDir() is unset -- every case is an ordinary MISS, not an error, and none of them
// touch `path`'s CONTENTS: only stat() (size, last-write time) informs the lookup. See
// TextureCache.cpp for exactly what "not usable" checks (a bad magic/version, a format this cache
// never writes, a byte count that does not add up to the header's own mips/width/height).
bool loadCachedTexture(const std::string& path, TextureUsage usage, CachedTexture& out);

// Writes `in` as the cache entry for `path`+`usage`, atomically (a temporary beside the target,
// swapped in only whole -- see FileSystem.hpp's writeFileBytesAtomic). Safe to call concurrently
// for DIFFERENT paths (no shared mutable state between them); two threads baking the SAME path at
// once serialise on that one entry rather than racing each other's temporary file -- see the
// striped lock in the .cpp.
bool saveCachedTexture(const std::string& path, TextureUsage usage, const CachedTexture& in);

} // namespace aver::assets
