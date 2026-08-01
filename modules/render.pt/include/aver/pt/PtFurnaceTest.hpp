#pragma once
// THE WHITE FURNACE: an energy oracle for the path tracer, read as LINEAR FLOATS.
//
// A surface of albedo 1 inside a uniform environment of radiance L must have exitant radiance
// EXACTLY L -- at any bounce count, at any orientation, and regardless of what surrounds it, because
// whatever occludes it is emitting L as well. That is an ABSOLUTE claim, and it has to be: a missing
// 1/PI is a GLOBAL SCALE, so no ratio between two pixels, no difference between two frames and no
// relational check of any kind can see one. Only a comparison against a known number can, and only
// if the number is read out of a float buffer rather than off a tonemapped, exposed 8-bit pixel.
//
// SIX CONFIGURATIONS, and two of them are DELIBERATELY BROKEN:
//
//   open/albedo-1/correct    a single flat quad, so every path bounces exactly ONCE and escapes.
//                            Must read L, and the measured mean bounce count is what PROVES the
//                            configuration really is single-bounce rather than nearly so.
//   open/albedo-1/xPI        the same, with the estimator scaled by PI -- which is also what
//                            dropping the 1/PI out of the Lambertian BRDF does. Must read PI*L.
//   open/albedo-1/no-cos     the same, with the cosine of the rendering equation dropped while the
//                            cosine-weighted pdf stays. Must read 2L.
//   open/albedo-0.5/correct  must read L/2, which is what says the albedo multiplies once.
//   cave/4-bounce/correct    a box open on one face, so paths bounce several times and some run
//                            out. Must read L times the fraction that ESCAPED -- measured by the
//                            same paths, so the identity needs no baseline and no analytic model.
//   cave/32-bounce/correct   the same box with the truncation pushed out of the way: the escaped
//                            fraction goes to ~1 and the reading goes to L.
//
// The two deliberate defects are the whole reason this is worth anything. A check that has never
// been shown FAILING proves nothing, so they are shipped and run on every invocation: if
// open/xPI does not read PI*L, the oracle itself is broken and says so.
//
// A SEVENTH AND EIGHTH configuration check DETERMINISM: the first block of samples is traced again
// at the end of the run, into a second accumulator, and the two must be BIT-IDENTICAL. That is what
// a seed drawn from a frame counter or a wall clock would fail, and nothing in an image would show.
//
// Those two trace the CAVE, and the first attempt at them traced the quad and was worthless. A
// correct albedo-1 furnace has exactly zero variance -- every path carries L whatever it drew -- so
// the accumulator held no trace of the sample stream, and deliberately reseeding the replay left
// the comparison bit-identical anyway. evaluate() now refuses the comparison outright unless the
// block actually varies from pixel to pixel, so that hole cannot reopen unnoticed.
#include "aver/pt/PathTracer.hpp"

#include <vector>

namespace aver::pt {

// Builds the furnace scenes, accumulates them, and reports the numbers it read.
class PtFurnaceTest final : public rhi::IRenderFeature {
public:
    ~PtFurnaceTest() override;

    // Builds the meshes, the scenes and the accumulators. False means the check cannot run here --
    // reported as "unavailable", never as a wrong answer.
    bool init(rhi::IDevice& dev);
    void shutdown();

    const char* name() const override { return "Aver.PathTracer.Furnace"; }

    // The whole experiment, one stage per frame: build, then accumulate, then copy, then read.
    // The readback is a frame behind the copy on purpose -- readBuffer synchronises nothing by
    // contract, and a copy recorded this frame has not run yet.
    void prePass(rhi::IRenderContext& ctx) override;

    bool finished() const { return finished_; }
    bool passed() const { return passed_; }

private:
    // The configurations, in the order they are reported. See the class comment.
    enum Run : u32 {
        kOpenCorrect = 0,
        kOpenTimesPi,
        kOpenNoCosine,
        kOpenHalfAlbedo,
        kCaveShallow,
        kCaveDeep,
        kSeedFirst,     // the first sample block, traced on the FIRST accumulation frame
        kSeedReplay,    // the same sample block, traced on the LAST one
        kRuns
    };

    // What one configuration is.
    struct Config {
        const char* label = "";
        u32      scene = 0;
        u32      camera = 0;
        u32      maxBounces = 4;
        PtDefect defect = PtDefect::None;
        // 0 = every accumulation step; otherwise the single step this run dispatches on, plus one.
        u32      onlyStep = 0;
    };

    // What came back out of one accumulator.
    struct Result {
        f64 radiance[3] = {0, 0, 0};   // summed over every pixel and sample
        f64 escaped = 0, bounceEvents = 0, traced = 0;
        f64 mean(u32 c) const { return traced > 0.0 ? radiance[c] / traced : 0.0; }
        // How far the three channels drift apart. A grey environment must come back grey, so
        // anything here is a swizzle and not an energy error.
        f64 channelSpread() const;
        f64 escapeFraction() const { return traced > 0.0 ? escaped / traced : 0.0; }
        f64 meanBounces() const { return traced > 0.0 ? bounceEvents / traced : 0.0; }
    };

    bool buildGeometry();
    void evaluate();
    // One "measured == expected" line, and whether it held. `why` names what a failure would mean.
    bool check(const char* label, f64 measured, f64 expected, f64 relTolerance, const char* why);

    // 32x32 probes at 16 samples each over 8 frames: 131072 paths per configuration. The furnace
    // has ZERO variance where the estimator is right -- every path carries exactly L -- so this is
    // far more than the correct configurations need. It is the no-cosine defect that wants them:
    // its estimator is albedo/cos, whose spread is wide, and 1e5 paths is what pins its mean near
    // 2L closely enough to be stated as a number.
    static constexpr u32 kProbeDim = 32;
    static constexpr u32 kSamplesPerStep = 16;
    static constexpr u32 kSteps = 8;

    PathTracer  pt_;
    PtCamera    cameras_[2];
    PtTarget    targets_[kRuns];
    Result      results_[kRuns];
    Config      configs_[kRuns];
    // The raw floats of the two determinism runs, kept so they can be compared BIT-EXACTLY rather
    // than through an average that would hide a compensating difference.
    std::vector<f32> seedFirstRaw_, seedReplayRaw_;

    rhi::IDevice*          dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    rhi::BufferHandle      readback_ = 0;
    std::vector<rhi::MeshHandle> meshes_;

    f32  furnaceL_ = 0.0f;
    u32  stage_ = 0;
    bool finished_ = false;
    bool passed_ = false;
    u32  failures_ = 0;
};

} // namespace aver::pt
