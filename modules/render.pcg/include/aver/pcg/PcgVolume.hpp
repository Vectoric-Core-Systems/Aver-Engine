// The GPU half of the PCG density volume: F# decides the rules, a compute shader fills the field.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/rhi/RHI.hpp"

#include <vector>

namespace aver::pcg {

// The most layers a volume may have. Fixed because the parameters ride in root constants, and a
// variable count would mean a structured buffer and a descriptor for what is under a kilobyte.
inline constexpr u32 kMaxLayers = 4;

// One fBm layer. Mirrors Aver.Pcg's NoiseLayer field for field.
struct NoiseLayer {
    f32 frequency  = 4.0f;
    f32 amplitude  = 1.0f;
    i32 octaves    = 4;
    f32 lacunarity = 2.0f;
    f32 gain       = 0.5f;
    i32 seedOffset = 0;
};

// The recipe for a density volume. Mirrors Aver.Pcg's VolumeSpec.
//
// THIS IS KILOBYTES THAT DESCRIBE A VOLUME, never the volume itself. A 128^3 field is 2 million
// voxels and 256^3 is 16 million; filling those on the CPU in any language is the cost the whole
// rules-in-F#/volume-on-GPU split exists to avoid.
struct VolumeSpec {
    u32 resX = 64, resY = 64, resZ = 64;
    i32 seed = 0;
    u32 layerCount = 1;
    NoiseLayer layers[kMaxLayers]{};
    f32 coverageFloor = 0.0f;
    f32 coverageBias  = 1.0f;
};

// A density field with NO BOUNDS, sampled by world position. Mirrors Aver.Pcg's InfiniteSpec.
struct InfiniteSpec {
    i32 seed = 0;
    u32 layerCount = 1;
    NoiseLayer layers[kMaxLayers]{};
    // World centimetres spanned by one lattice cell.
    f32 cellSizeCm = 1600.0f;
    f32 coverageFloor = 0.0f;
    f32 coverageBias  = 1.0f;
};

// Samples an infinite field at a WORLD position, in centimetres.
//
// FLOOR, NOT TRUNCATION, and it is the one line where this differs from the bounded sampler. A
// bounded volume indexes from 0 up, where the two agree. An infinite field is sampled at negative
// coordinates too, and there a cast toward zero puts -0.5 and +0.5 in the same cell -- one
// double-width cell straddling the origin, which is a visible seam at world zero and nowhere else.
// The F# mirror does the same, for the same reason.
f32 sampleInfinite(const InfiniteSpec& spec, f32 wx, f32 wy, f32 wz);

// Samples an INFINITE, C1-CONTINUOUS scalar field at a WORLD (x, y), in [0, 1]. Same InfiniteSpec as
// sampleInfinite -- same seed, layers, cellSizeCm -- but NOT the same result: sampleInfinite hashes
// only the lattice CELL a position falls in, a per-cell constant with a visible step at every cell
// boundary of every octave. This instead hashes the four lattice CORNERS surrounding the position
// and blends them with a smoothstep fade, the technique tests/landscape/src/TerrainGenTool.cpp's own
// valueNoise() already uses for the .ocland heightfield it writes. That is what makes this usable AS
// a height: sampleInfinite would render as a field of vertical cliffs, one at every noise-cell edge.
//
// coverageFloor/coverageBias ARE NOT READ HERE. That remap is a density-sculpting concept -- push
// more of the field toward 0 or toward 1 -- and means nothing for an elevation signal. Dropping it
// also drops the one operation sampleInfinite has that IEEE 754 does not pin exactly across libm
// implementations (std::pow, PcgShaders.hpp:8-12), so this function is MORE strictly, not less,
// reproducible bit-for-bit cross-platform: nothing here is anything but integer hashing, multiplies
// and adds.
//
// wz IS NOT A PARAMETER, deliberately: an elevation function of (x, y) cannot sensibly also depend on
// the z it is being asked to define. A single fixed z (0) feeds the shared hash3 mixer instead, so
// this reuses that mixer exactly rather than a 2D-only copy of it.
//
// THE FUNCTION TERRAIN GEOMETRY WILL LATER CALL, alongside a chunk generator's heightSource (see
// aver::world::makeInfiniteHeightSource, modules/world/include/aver/world/ChunkGenerator.hpp) --
// living here, in Aver.Render.Pcg, is what lets both reach it with zero new module dependencies:
// Aver.World already depends on this module, and Aver.Landscape must never depend on Aver.World.
f32 sampleInfiniteHeight(const InfiniteSpec& spec, f32 wx, f32 wy);

// Convenience: sampleInfiniteHeight's [0, 1] remapped into a world Z in centimetres, centred on
// `baseZCm` and spanning roughly [baseZCm - amplitudeCm, baseZCm + amplitudeCm] -- the same
// half-amplitude convention TerrainGenTool.cpp's own ridged fBm uses (there, the noise is remapped to
// [-1, 1] before the caller's own `* amplitude`; here the remap and the scale are folded into one
// call because sampleInfiniteHeight's raw [0, 1] has no reason to be exposed to two separate
// multiplications at every call site).
//
// CALL THIS, NOT sampleInfiniteHeight DIRECTLY, wherever a caller wants a WORLD Z rather than a raw
// [0, 1] sample -- it is the one place the remap is written, so two callers configured with the same
// spec/amplitudeCm/baseZCm can never compute two different Z values for the same (wx, wy) through a
// transcription slip in the remap arithmetic.
f32 sampleInfiniteHeightCm(const InfiniteSpec& spec, f32 amplitudeCm, f32 baseZCm, f32 wx, f32 wy);

// THE CPU REFERENCE, and the reason this header exists at all.
//
// Mirrors the HLSL in PcgShaders.hpp function for function, exactly as the atmosphere model is
// mirrored between Atmosphere.cpp and RHIShaders.cpp. A GPU implementation with no CPU reference is
// one nobody can prove correct: "the volume looks like noise" is true of a correct volume and of
// almost every incorrect one.
//
// Not for filling volumes. Calling this per voxel is precisely the cost the design avoids.
f32 sampleDensity(const VolumeSpec& spec, u32 x, u32 y, u32 z);

// Fills a density field on the GPU.
//
// A RENDER FEATURE, because that is the only place an IRenderContext exists: the RHI hands one to
// prePass/scenePass/overlayPass and offers no immediate or one-shot submit. Following that rather
// than working around it also puts the dispatch where SkinningPass's is, which is where anyone
// looking for a compute pass will look.
//
// init compiles HLSL at RUNTIME, so a green C++ build proves nothing about whether this works.
// False leaves the object inert rather than broken.
class VolumeBuilder final : public rhi::IRenderFeature {
public:
    ~VolumeBuilder() override;

    // Named for the frame-marker and any feature listing. IRenderFeature's only pure virtual.
    const char* name() const override { return "Aver.Pcg.Volume"; }

    bool init(rhi::IDevice& dev);
    void shutdown();
    bool ready() const { return pipeline_ != 0; }

    // Queues a build. Takes effect on the next prePass; the result is readable a frame later, once
    // the GPU has actually run it. False when the builder is not ready or the spec is empty.
    bool request(const VolumeSpec& spec);

    // True once a requested build has been read back. Stays true until the next request().
    bool done() const { return state_ == State::Done; }

    // Copies the finished field out. `count` must be resX*resY*resZ. False before done().
    bool read(f32* out, usize count) const;

    // The dispatch, and the copy that makes the result readable.
    void prePass(rhi::IRenderContext& ctx) override;

private:
    // A build takes three frames, and each is a real wait rather than caution: the dispatch is
    // recorded in one, the copy to a readback buffer cannot be read until the GPU has passed it,
    // and reading before that returns whatever the buffer held before.
    enum class State { Idle, Dispatch, Copy, Done };

    // How many prePasses to let go by between recording the readback copy and reading it.
    //
    // The RHI's readBuffer synchronises nothing, and the device is double buffered: at frame N+1
    // the GPU is only guaranteed to have finished frame N-1, so a copy recorded in frame N can
    // still be in flight. Three covers double buffering with a frame to spare. Raise it if the
    // device ever triple buffers; never lower it.
    static constexpr u32 kReadbackFrames = 3;
    u32 copyWaited_ = 0;

    rhi::IResourceFactory* res_ = nullptr;
    rhi::ShaderHandle      cs_ = 0;
    rhi::PipelineHandle    pipeline_ = 0;
    rhi::BufferHandle      out_ = 0;        // Default, UAV-writable: what the shader fills
    rhi::BufferHandle      readback_ = 0;   // Readback: what the CPU reads
    rhi::BindingSetHandle  set_ = 0;
    u64                    bytes_ = 0;
    VolumeSpec             spec_{};
    // Tracked by hand, because bufferBarrier takes an explicit FROM state: nothing in this RHI
    // transitions implicitly, and a barrier whose from-state is wrong is undefined rather than
    // merely slow.
    rhi::ResourceState     outState_ = rhi::ResourceState::Common;
    State                  state_ = State::Idle;
    mutable std::vector<f32> host_;
};

} // namespace aver::pcg
