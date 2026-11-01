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

// Streaming: reads the texture's BC7 chain from the derived-data cache (on a first visit decodes,
// compresses and caches it) on the calling worker thread, and holds it for the next uploadTexture of
// the same file and usage, which then only copies it to the GPU. Thread-safe; a second request for a
// texture being read waits for that read. False when there is nothing to hold: compression off, an
// extent BC7 does not take, a file that does not decode. Holds at most 1 GiB, oldest dropped first.
bool prefetchTexture(const std::string& path, TextureUsage usage);
// Drops every held texture and forgets which were uploaded: a project or level change, after which textures
// released by the material system may be needed again.
void dropPrefetchedTextures();
// Drops held textures nobody uploaded within 30 s (read for a mesh that never loaded). Call now and then.
void trimPrefetchedTextures();
// Forgets which were uploaded, keeping what is held: textures the material system released may be read
// ahead again.
void forgetUploadedTextures();
// Bytes held now.
u64 prefetchedTextureBytes();


// ---- background loading: no frame waits on a decode or a BC7 encode ----
//
// A texture's CPU work (the derived-data cache read, or the decode, mip chain and BC7 encode on a first visit)
// runs on a small pool of worker threads; only the GPU copy is left for the calling thread, which takes it
// when it has upload budget. Workers never touch the RHI. Results are keyed by (path, usage), so any number of
// materials asking for one file share one job.
enum class TextureState : u32 {
    Pending,   // queued or being prepared; ask again later
    Ready,     // takeTexture() now costs only the GPU copy
    Failed,    // the file does not decode (reported once; the next request tries again)
};
// Who is waiting: a prefetch fills the queue behind everything a draw has asked for.
enum class TexturePriority : u32 { Demand = 0, Prefetch = 1 };

// Queues `path` for preparation if it is not already, and says where it stands. Idempotent; a Demand request
// moves a queued Prefetch one to the front. A Failed answer is given once (with `err`) and forgets the entry.
TextureState requestTexture(const std::string& path, TextureUsage usage, TexturePriority priority,
                            std::string* err = nullptr);
// The GPU texture for a Ready request, consuming it. 0 (with `err`) when it is not Ready or the RHI refuses.
rhi::TextureHandle takeTexture(rhi::IResourceFactory& res, const std::string& path, TextureUsage usage,
                               std::string* err = nullptr, TextureUploadInfo* info = nullptr);
// A project or level change: queued requests are dropped, running ones are discarded when they finish, and
// every prepared result is freed. A later request starts again (and hits the derived-data cache).
void cancelTextureRequests();

struct TextureLoadStats {
    u64 requested = 0;     // distinct jobs queued
    u64 prepared = 0;      // jobs finished
    u64 cacheHits = 0;     // ... that read the derived-data cache (no decode)
    u64 compressed = 0;    // ... that decoded and BC7-encoded
    u64 failed = 0;
    u64 uploaded = 0;      // takeTexture() successes
    u32 queued = 0, running = 0, ready = 0;   // right now
    u64 readyBytes = 0;
};
TextureLoadStats textureLoadStats();
// Shrinks the pool for a test or a tool that wants none (0 = run nothing in the background; requests stay Pending
// until runTextureJobsHere()). Call before the first request.
void setTextureWorkerCount(u32 count);
// Runs up to `maxJobs` queued jobs on the calling thread, most urgent first; for tests with no workers. Returns how many ran.
u32 runTextureJobsHere(u32 maxJobs = ~0u);

// Decodes `path` and uploads it. Returns 0 on failure, with the reason in `err`.
rhi::TextureHandle uploadTexture(rhi::IResourceFactory& res, const std::string& path,
                                 TextureUsage usage, std::string* err = nullptr,
                                 TextureUploadInfo* info = nullptr);

} // namespace aver::assets
