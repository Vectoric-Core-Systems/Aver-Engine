// Nrd2ResolveReference -- CPU twin of NRD2's fixed maths and of the phase 3 oracle fit: the pyramid
// (CSNrd2Pyramid), the resolve forward and backward (nrd2_resolve.hlsli), the temporal stabiliser's
// per-pixel blend and the per-tile fit (nrd2_capture.hlsl). The spec the shaders follow and what
// Nrd2ResolveTest checks.
//
// KEEP IN SYNC WITH nrd2.hlsl / nrd2_resolve.hlsli / nrd2_capture.hlsl.
//
// Frames here are one viewport at origin 0 (render target == viewport). Patent rule 4: the oracle
// scores only free per-tile parameters through the filter; no model is involved.
#pragma once

#include <aver/core/Types.hpp>

#include <array>
#include <vector>

namespace aver::render::denoise {

// What Stage B writes for one frame (texel layout of the GPU targets, RGBA floats per pixel).
struct Nrd2Frame {
    u32 width = 0, height = 0;
    std::vector<f32> d;        // rgb demodulated diffuse, a = albedo usable (0/1)
    std::vector<f32> s;        // rgb demodulated specular, a = hit distance (cm; 0 none, -1 unlit)
    std::vector<f32> viewZ;    // cm, one per pixel; outside (0, 1e6) = no surface
    std::vector<f32> normal;   // averPackNormalRoughness: xy octahedral, z roughness, w unused
};

// Levels 1/2, 1/4, 1/8 (index 0..2), RGBA texels. guide: xyz averaged normal, w view Z (m);
// value[signal]: rgb, a validity fraction.
struct Nrd2Pyramid {
    u32 width[3] = {}, height[3] = {};
    std::vector<f32> guide[3];
    std::vector<f32> value[2][3];
};

// halfStorage rounds every stored texel through fp16 (the GPU levels are RGBA16F); the in-group
// chain stays fp32 like the shader's groupshared.
void nrd2BuildPyramid(const Nrd2Frame& f, Nrd2Pyramid& out, bool halfStorage = true);

// Defaults (Nrd2Params) for signal 0 (D) or 1 (S).
std::array<f32, 6> nrd2DefaultTheta(u32 signal);

// The resolve of one pixel and signal with tile parameters theta (sanitised like the shader, defaults
// as the fallback). dOut, when given, receives d(output)/d(theta_k) for k = 0..5.
std::array<f32, 3> nrd2ResolvePixel(const Nrd2Frame& f, const Nrd2Pyramid& p, u32 x, u32 y, u32 signal,
                                    const f32 theta[6], f32 (*dOut)[3] = nullptr);

// ---- temporal stabiliser (nrd2_resolve.hlsli: nrd2StabMaxFrames / nrd2StabCombine) ----

// Frames of history allowed at this screen speed (px per frame): nStill at rest, min(nFast, nStill) from
// 8 px, none from 128 px (or for a non-finite speed).
f32 nrd2StabMaxFrames(f32 speed, f32 nStill, f32 nFast);
// Firefly clamp (nrd2StabFirefly): c scaled down to 2x the brightest neighbour's luminance when above it.
void nrd2StabFirefly(f32 c[3], f32 neighbourMaxLum);
// Weight of the history after `age` frames: 0, 1/2, 2/3 ... up to 1 - 1/nMax.
f32 nrd2StabAlpha(f32 age, f32 nMax);
// Per-channel min/max over the given rgb values (the clamp box of one signal).
void nrd2StabBox(const std::vector<std::array<f32, 3>>& values, f32 lo[3], f32 hi[3]);

struct Nrd2StabTap {
    f32 b = 0.0f, age = 0.0f, zm = 0.0f;   // bilinear weight, frames of history (0 = none), view Z (m)
    f32 d[3] = {}, s[3] = {};              // last frame's stabilised D'' and S''
};

struct Nrd2StabIn {
    f32 curD[3] = {}, curS[3] = {};
    f32 loD[3] = {}, hiD[3] = {}, loS[3] = {}, hiS[3] = {};   // the clamp box
    Nrd2StabTap tap[4];
    f32 zExp = 0.0f, zTol = 0.0f;   // the view depth (m) a tap must match, and by how much
    f32 nMax = 12.0f;               // nrd2StabMaxFrames of this pixel's speed
    f32 roughness = 1.0f;
    bool historyValid = true;
};

struct Nrd2StabOut {
    f32 d[3] = {}, s[3] = {};
    f32 age = 1.0f;                  // history age to store
    f32 alphaD = 0.0f, alphaS = 0.0f;
};

Nrd2StabOut nrd2StabilisePixel(const Nrd2StabIn& in);

// ---- oracle fit (one 8x8 tile, one signal) ----

enum class Nrd2OracleMode : u32 { Grad = 0, Grid = 1 };

struct Nrd2OracleDesc {
    Nrd2OracleMode mode = Nrd2OracleMode::Grad;
    u32 adamIters = 150;      // Grad: Adam steps after the grid
    u32 patternIters = 24;    // Grid: derivative-free pattern-search rounds after the grid
    f32 lr = 0.05f, beta1 = 0.9f, beta2 = 0.99f, adamEps = 1e-8f;
    f32 lambda = 1e-3f;       // lambda * ||theta - theta0||^2
};

inline constexpr u32 kNrd2GridStarts        = 27;   // 3 logit offsets x 3 depth x 3 luminance
inline constexpr u32 kNrd2GridDefault       = 13;   // the all-zero offset: theta0 itself
inline constexpr u32 kNrd2PatternCandidates = 12;   // +-step on each of the six parameters
inline constexpr f32 kNrd2PatternStep0      = 1.0f;
inline constexpr f32 kNrd2LossEps           = 1e-8f;
inline constexpr f32 kNrd2SplitHalfR0       = 0.1f;

void nrd2ClampTheta(f32 t[6]);
void nrd2GridCandidate(const f32 theta0[6], u32 c, f32 out[6]);
void nrd2PatternCandidate(const f32 theta[6], f32 step, u32 c, f32 out[6]);
// validity x split-half confidence; r = rms(lum(even mean) - lum(odd mean)) / (mean lum + 1e-6).
f32 nrd2TileWeight(f32 validFraction, f32 r);

// K frames of one pose and the target per frame (rgb per pixel). The capture uses one converged mean
// for every frame; tests may give each frame its own target.
struct Nrd2TileProblem {
    const std::vector<Nrd2Frame>* frames = nullptr;
    const std::vector<Nrd2Pyramid>* pyramids = nullptr;
    const std::vector<std::vector<f32>>* targets = nullptr;
    u32 tileX = 0, tileY = 0, signal = 0;
};

// Data loss: sum over frames, tile pixels and rgb of (resolve - target)^2 / (mean lum(target)^2 + eps),
// over K * count. Pixels count when they have a surface (and for D an albedo in frame 0). grad, when
// given, is d(loss)/d(theta). count 0 returns 0.
f32 nrd2TileLoss(const Nrd2TileProblem& p, const f32 theta[6], f32 grad[6] = nullptr, u32* count = nullptr);

struct Nrd2TileFit {
    f32 theta[6] = {};
    f32 loss = 0.0f, defaultLoss = 0.0f;   // data loss at theta and at theta0
    u32 count = 0;
};
// Grid of 27 starts around theta0, then Adam (Grad) or pattern search (Grid) on loss + lambda * reg;
// keeps the best objective seen. The GPU fit runs the same sequence.
Nrd2TileFit nrd2FitTile(const Nrd2TileProblem& p, const f32 theta0[6], const Nrd2OracleDesc& d);

}  // namespace aver::render::denoise
