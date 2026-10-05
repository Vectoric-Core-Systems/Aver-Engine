#include "aver/render/neural/NeuralOptimiser.hpp"

#include <algorithm>
#include <cmath>

// Keep in step with shaders/aver_neural_common.hlsli (each function has an HLSL twin named in comments).

namespace aver::render::neural {

namespace {
bool fail(std::string* why, const char* msg) {
    if (why) *why = msg;
    return false;
}
}  // namespace

bool validate(const OptimiserDesc& o, std::string* why) {
    if (!(o.learningRate > 0.0f)) return fail(why, "learningRate must be > 0");
    if (!(o.beta1 >= 0.0f && o.beta1 < 1.0f)) return fail(why, "beta1 must be in [0, 1)");
    if (!(o.beta2 >= 0.0f && o.beta2 < 1.0f)) return fail(why, "beta2 must be in [0, 1)");
    if (!(o.epsilon > 0.0f)) return fail(why, "epsilon must be > 0");
    if (!(o.weightEma >= 0.0f && o.weightEma <= 1.0f)) return fail(why, "weightEma must be in [0, 1]");
    if (!(o.l2 >= 0.0f)) return fail(why, "l2 must be >= 0");
    if (!(o.gradFixedScale > 0.0f)) return fail(why, "gradFixedScale must be > 0");
    if (!(o.gradClamp > 0.0f)) return fail(why, "gradClamp must be > 0");
    if (static_cast<u32>(o.loss) > 1u) return fail(why, "unknown loss");
    if (!(o.gradClamp * o.gradFixedScale <= 1.0e9f)) return fail(why, "gradClamp * gradFixedScale must be <= 1e9");
    return true;
}

f32 activate(Activation a, f32 z) {
    switch (a) {
        case Activation::ReLU:    return z > 0.0f ? z : 0.0f;
        case Activation::Sigmoid: return 1.0f / (1.0f + std::exp(-z));
        case Activation::Exp:     return std::exp(std::min(z, kExpMaxArg));
        case Activation::None:    break;
    }
    return z;
}

f32 activationDerivative(Activation a, f32 y) {
    switch (a) {
        case Activation::ReLU:    return y > 0.0f ? 1.0f : 0.0f;
        case Activation::Sigmoid: return y * (1.0f - y);
        case Activation::Exp:     return y;
        case Activation::None:    break;
    }
    return 1.0f;
}

f32 lossGradient(Loss l, f32 p, f32 y) {
    if (l == Loss::RelativeL2) return 2.0f * (p - y) / (p * p + 0.01f);
    return 2.0f * (p - y);
}

f32 lossValue(Loss l, f32 p, f32 y) {
    const f32 e = p - y;
    if (l == Loss::RelativeL2) return e * e / (p * p + 0.01f);
    return e * e;
}

u32 initHash(u32 x) {
    const u32 state = x * 747796405u + 2891336053u;
    const u32 word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

f32 adamBiasCorrection(f32 beta, u32 step) {
    return static_cast<f32>(1.0 - std::pow(static_cast<f64>(beta), static_cast<f64>(step)));
}

u32 safeBatchLimit(const OptimiserDesc& o) {
    const f64 perRecord = static_cast<f64>(o.gradClamp) * static_cast<f64>(o.gradFixedScale);
    if (!(perRecord > 0.0)) return 0;
    const f64 n = std::floor(2147483647.0 / perRecord);
    return n >= 4294967295.0 ? 0xFFFFFFFFu : static_cast<u32>(n);
}

// HLSL twin: quantise() in aver_neural_common.hlsli.
i32 quantise(f32 g, const OptimiserDesc& o) {
    if (!(g == g)) g = 0.0f;   // a NaN gradient quantises to 0, as on the GPU
    const f32 c = std::min(std::max(g, -o.gradClamp), o.gradClamp);
    return static_cast<i32>(c * o.gradFixedScale);
}

// HLSL twin: neuralAdam() in aver_neural_common.hlsli (CSAdam).
void adamStep(std::span<f32> w, std::span<f32> ema, std::span<f32> m, std::span<f32> v, std::span<i32> acc,
              const OptimiserDesc& o, u32 step, u32 liveCount) {
    if (liveCount == 0) { std::fill(acc.begin(), acc.end(), 0); return; }
    const f32 bc1 = adamBiasCorrection(o.beta1, step);
    const f32 bc2 = adamBiasCorrection(o.beta2, step);
    for (usize k = 0; k < w.size(); ++k) {
        f32 g = (static_cast<f32>(acc[k]) / o.gradFixedScale) / static_cast<f32>(liveCount);
        g = std::min(std::max(g, -o.gradClamp), o.gradClamp);
        g += o.l2 * w[k];
        m[k] = o.beta1 * m[k] + (1.0f - o.beta1) * g;
        v[k] = o.beta2 * v[k] + (1.0f - o.beta2) * g * g;
        const f32 mhat = m[k] / bc1;
        const f32 vhat = v[k] / bc2;
        w[k] -= o.learningRate * mhat / (std::sqrt(vhat) + o.epsilon);
        ema[k] = o.weightEma * ema[k] + (1.0f - o.weightEma) * w[k];
    }
    std::fill(acc.begin(), acc.end(), 0);
}

}  // namespace aver::render::neural
