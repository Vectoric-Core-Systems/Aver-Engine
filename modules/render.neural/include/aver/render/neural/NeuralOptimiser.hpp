// NeuralOptimiser -- the pieces of Aver.Render.Neural's CPU maths that every network type shares:
// activations, losses, the Adam optimiser with fixed-point gradient accumulation, and the init hash.
//
// CPU twin of the optimiser half of shaders (CSAdam); defines the spec both must match exactly.
// RHI-free by design; no device/backend/GPU headers needed.
#pragma once

#include <aver/core/Types.hpp>

#include <span>
#include <string>
#include <vector>

namespace aver::render::neural {

// Numeric values are the AVER_NN_*_ACT defines the HLSL is compiled with. Do not renumber.
enum class Activation : u32 { None = 0, ReLU = 1, Sigmoid = 2, Exp = 3 };

// RelativeL2 is NRC's loss: (y - p)^2 / (stopgrad(p)^2 + 0.01) per channel; d/dp = 2(p - y) / (p^2 + 0.01).
enum class Loss : u32 { L2 = 0, RelativeL2 = 1 };

// Adam with fixed-point gradient accumulation (GPU needs integer adds; D3D12 has no float atomic).
struct OptimiserDesc {
    f32 learningRate = 1e-3f;
    f32 beta1 = 0.9f, beta2 = 0.99f, epsilon = 1e-8f;
    // Inference reads EMA weights, not raw weights (avoid flicker).
    f32 weightEma = 0.99f;
    f32 l2 = 0.0f;   // L2 weight decay
    Loss loss = Loss::L2;
    // Per-record gradient clamp and fixed-point scale; see safeBatchLimit for overflow headroom.
    f32 gradFixedScale = 65536.0f;
    f32 gradClamp = 16.0f;
};

bool validate(const OptimiserDesc& o, std::string* why = nullptr);

// ---- scalar maths
// Activation derivatives in POST-activation form y = act(z): relu' = y > 0, sigmoid' = y(1-y), exp' = y, identity' = 1.
inline constexpr f32 kExpMaxArg = 20.0f;
f32 activate(Activation a, f32 z);
f32 activationDerivative(Activation a, f32 y);

// d(loss)/d(prediction) for one channel; RelativeL2 holds denominator constant when differentiating.
f32 lossGradient(Loss l, f32 p, f32 y);
f32 lossValue(Loss l, f32 p, f32 y);

// Same weights on every machine (PCG hash of seed and index); the networks' He-uniform inits draw from it.
u32 initHash(u32 x);

// Adam bias-correction 1 - beta^t in double, rounded once for CPU/GPU agreement.
f32 adamBiasCorrection(f32 beta, u32 step);

// Worst-case largest batch before per-weight int32 accumulator overflows (clamp * gradFixedScale).
u32 safeBatchLimit(const OptimiserDesc& o);

// GPU quantisation: int(clamp(g, -gradClamp, gradClamp) * gradFixedScale), truncating toward zero like HLSL/C++.
// A NaN gradient quantises to 0.
i32 quantise(f32 g, const OptimiserDesc& o);

// HLSL twin: CSAdam. One GPU thread per weight, in exactly this order:
//   g = (acc / gradFixedScale) / liveCount, clamped +-gradClamp; g += l2 * w;
//   m = b1 m + (1-b1) g; v = b2 v + (1-b2) g g; w -= lr * (m / bc1) / (sqrt(v / bc2) + eps);
//   ema = d ema + (1-d) w.
// `step` is the 1-based step number for the bias correction (the caller counts it). Clears acc at the end.
// liveCount == 0 only clears acc. All spans must be the same length.
void adamStep(std::span<f32> w, std::span<f32> ema, std::span<f32> m, std::span<f32> v, std::span<i32> acc,
              const OptimiserDesc& o, u32 step, u32 liveCount);

}  // namespace aver::render::neural
