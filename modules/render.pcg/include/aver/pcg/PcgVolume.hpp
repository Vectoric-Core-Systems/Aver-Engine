// The GPU half of the PCG density volume: F# decides the rules, a compute shader fills the field.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/rhi/RHI.hpp"

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

// THE CPU REFERENCE, and the reason this header exists at all.
//
// Mirrors the HLSL in PcgShaders.hpp function for function, exactly as the atmosphere model is
// mirrored between Atmosphere.cpp and RHIShaders.cpp. A GPU implementation with no CPU reference is
// one nobody can prove correct: "the volume looks like noise" is true of a correct volume and of
// almost every incorrect one.
//
// Not for filling volumes. Calling this per voxel is precisely the cost the design avoids.
f32 sampleDensity(const VolumeSpec& spec, u32 x, u32 y, u32 z);

// Owns the density-volume shader and pipeline.
//
// init compiles HLSL at RUNTIME, so a green C++ build proves nothing about whether this works --
// which is exactly why init() existing and being tested is worth having before the dispatch does.
// False leaves the object inert rather than broken.
//
// WHAT IS NOT HERE YET, stated rather than stubbed: build() and buildAndRead(), which would create
// the Tex3D, bind it as a UAV, dispatch, and read the result back for comparison against
// sampleDensity above. Declaring them now and leaving them unimplemented would be the same
// "declared with no implementation" pattern this repo's own dead-code sweep flags, so they are
// absent until they work. The shader they would dispatch is written, compiled and mirrored; the
// dispatch is the remaining piece.
class VolumeBuilder {
public:
    bool init(rhi::IDevice& dev);
    void shutdown();
    bool ready() const { return pipeline_ != 0; }

private:
    rhi::IResourceFactory* res_ = nullptr;
    rhi::ShaderHandle   cs_ = 0;
    rhi::PipelineHandle pipeline_ = 0;
};

} // namespace aver::pcg
