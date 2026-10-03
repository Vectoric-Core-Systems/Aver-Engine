#include "aver/render/neural/MlpReference.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

// KEEP IN STEP WITH shaders/aver_neural_mlp.hlsl. Every function here has an HLSL twin; the
// comments name it. See MlpReference.hpp for what "the same maths" does and does not promise.

namespace aver::render::neural {

namespace {

bool fail(std::string* why, const char* msg) {
    if (why) *why = msg;
    return false;
}

// Forward pass over the flat weight array; `acts` holds layout.actTotal floats and receives every
// layer's post-activation output. HLSL twin: forwardLayer() in the CSInfer / CSTrainGrad bodies.
// z starts at the bias and accumulates w * a in ascending i, the order the GPU loop uses.
void forwardImpl(const MlpDesc& d, const MlpLayout& L, const f32* w, const f32* in, f32* acts) {
    for (u32 i = 0; i < d.inputs; ++i) acts[i] = in[i];
    for (u32 l = 0; l < L.layers; ++l) {
        const Activation act = (l + 1 < L.layers) ? d.hidden : d.output;
        const f32* a = acts + L.actOffset[l];
        f32* y = acts + L.actOffset[l + 1];
        for (u32 o = 0; o < L.outDim[l]; ++o) {
            f32 z = d.bias ? w[L.bOffset[l] + o] : 0.0f;
            const f32* row = w + L.wOffset[l] + o * L.inDim[l];
            for (u32 i = 0; i < L.inDim[l]; ++i) z += row[i] * a[i];
            y[o] = activate(act, z);
        }
    }
}

}  // namespace

bool validate(const MlpDesc& d, std::string* why) {
    if (d.inputs < 1 || d.inputs > kMaxInputs) return fail(why, "inputs must be in [1, 64]");
    if (d.outputs < 1 || d.outputs > kMaxOutputs) return fail(why, "outputs must be in [1, 16]");
    if (d.hiddenWidth < 4 || d.hiddenWidth > kMaxHiddenWidth || (d.hiddenWidth % 4) != 0)
        return fail(why, "hiddenWidth must be a multiple of 4 in [4, 64]");
    if (d.hiddenLayers < 1 || d.hiddenLayers > kMaxHiddenLayers)
        return fail(why, "hiddenLayers must be in [1, 6]");
    if (static_cast<u32>(d.hidden) > 3u || static_cast<u32>(d.output) > 3u)
        return fail(why, "unknown activation");
    return true;
}

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
    // One record's quantised gradient must itself fit an int32 with room to sum.
    if (!(o.gradClamp * o.gradFixedScale <= 1.0e9f)) return fail(why, "gradClamp * gradFixedScale must be <= 1e9");
    return true;
}

MlpLayout MlpLayout::make(const MlpDesc& d) {
    MlpLayout L;
    L.layers = d.hiddenLayers + 1;
    u32 offset = 0;
    for (u32 l = 0; l < L.layers; ++l) {
        L.inDim[l]  = (l == 0) ? d.inputs : d.hiddenWidth;
        L.outDim[l] = (l + 1 == L.layers) ? d.outputs : d.hiddenWidth;
        L.wOffset[l] = offset;
        L.bOffset[l] = offset + L.outDim[l] * L.inDim[l];
        L.layerSize[l] = L.outDim[l] * L.inDim[l] + (d.bias ? L.outDim[l] : 0u);
        offset += L.layerSize[l];
    }
    L.total = offset;
    L.actOffset[0] = 0;
    L.actOffset[1] = d.inputs;
    for (u32 l = 1; l < L.layers; ++l) L.actOffset[l + 1] = L.actOffset[l] + L.outDim[l - 1];
    // actOffset[layers] is the last layer's output; actTotal runs past it.
    L.actTotal = L.actOffset[L.layers] + L.outDim[L.layers - 1];
    return L;
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

std::vector<f32> initWeights(const MlpDesc& d) {
    const MlpLayout L = MlpLayout::make(d);
    std::vector<f32> w(L.total, 0.0f);   // biases stay zero
    for (u32 l = 0; l < L.layers; ++l) {
        const f32 bound = std::sqrt(6.0f / static_cast<f32>(L.inDim[l]));
        const u32 n = L.outDim[l] * L.inDim[l];
        for (u32 k = 0; k < n; ++k) {
            const u32 idx = L.wOffset[l] + k;
            const f32 u = static_cast<f32>(initHash(d.seed * 0x9E3779B9u + idx) >> 8) * (1.0f / 16777216.0f);
            w[idx] = (2.0f * u - 1.0f) * bound;
        }
    }
    return w;
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

// ---------------------------------------------------------------- MlpReference

MlpReference::MlpReference(const MlpDesc& d, const OptimiserDesc& o)
    : desc_(d), opt_(o), layout_(MlpLayout::make(d)) {
    w_ = initWeights(d);
    ema_ = w_;
    m_.assign(layout_.total, 0.0f);
    v_.assign(layout_.total, 0.0f);
    acc_.assign(layout_.total, 0);
}

bool MlpReference::setWeights(std::span<const f32> w) {
    if (w.size() != layout_.total) return false;
    w_.assign(w.begin(), w.end());
    ema_ = w_;
    std::fill(m_.begin(), m_.end(), 0.0f);
    std::fill(v_.begin(), v_.end(), 0.0f);
    clearAccumulator();
    step_ = 0;
    return true;
}

void MlpReference::forward(std::span<const f32> in, std::span<f32> out, bool useEma,
                           std::vector<f32>* acts) const {
    std::vector<f32> local;
    std::vector<f32>& a = acts ? *acts : local;
    a.assign(layout_.actTotal, 0.0f);
    forwardImpl(desc_, layout_, (useEma ? ema_ : w_).data(), in.data(), a.data());
    const f32* p = a.data() + layout_.actOffset[layout_.layers];
    for (u32 o = 0; o < desc_.outputs && o < out.size(); ++o) out[o] = p[o];
}

// HLSL twin: the backward half of CSTrainGrad. Walks the layers last to first; at each, the
// weight gradient is delta[o] * a_in[i], the bias gradient is delta[o], and (below the first
// layer) the next delta is W^T delta scaled by the hidden activation's derivative.
f32 MlpReference::backward(std::span<const f32> in, std::span<const f32> target,
                           std::span<f32> grad) const {
    const MlpLayout& L = layout_;
    std::vector<f32> acts(L.actTotal);
    forwardImpl(desc_, L, w_.data(), in.data(), acts.data());
    const f32* p = acts.data() + L.actOffset[L.layers];

    f32 loss = 0.0f;
    f32 delta[kMaxHiddenWidth] = {};
    f32 prev[kMaxHiddenWidth] = {};
    for (u32 o = 0; o < desc_.outputs; ++o) {
        loss += lossValue(opt_.loss, p[o], target[o]);
        delta[o] = lossGradient(opt_.loss, p[o], target[o]) * activationDerivative(desc_.output, p[o]);
    }
    std::fill(grad.begin(), grad.end(), 0.0f);
    for (u32 l = L.layers; l-- > 0;) {
        const f32* aIn = acts.data() + L.actOffset[l];
        for (u32 o = 0; o < L.outDim[l]; ++o) {
            for (u32 i = 0; i < L.inDim[l]; ++i)
                grad[L.wOffset[l] + o * L.inDim[l] + i] = delta[o] * aIn[i];
            if (desc_.bias) grad[L.bOffset[l] + o] = delta[o];
        }
        if (l == 0) break;
        for (u32 i = 0; i < L.inDim[l]; ++i) {
            f32 s = 0.0f;
            for (u32 o = 0; o < L.outDim[l]; ++o) s += w_[L.wOffset[l] + o * L.inDim[l] + i] * delta[o];
            prev[i] = s * activationDerivative(desc_.hidden, aIn[i]);
        }
        for (u32 i = 0; i < L.inDim[l]; ++i) delta[i] = prev[i];
    }
    return loss;
}

// HLSL twin: quantise() in aver_neural_mlp.hlsl.
i32 MlpReference::quantise(f32 g, const OptimiserDesc& o) {
    if (!(g == g)) g = 0.0f;   // a NaN gradient quantises to 0, as on the GPU
    const f32 c = std::min(std::max(g, -o.gradClamp), o.gradClamp);
    return static_cast<i32>(c * o.gradFixedScale);
}

f32 MlpReference::accumulateRecord(std::span<const f32> in, std::span<const f32> target) {
    std::vector<f32> grad(layout_.total);
    const f32 loss = backward(in, target, grad);
    for (u32 k = 0; k < layout_.total; ++k) {
        // u32 arithmetic: defined wraparound, the same bits InterlockedAdd leaves behind.
        acc_[k] = static_cast<i32>(static_cast<u32>(acc_[k]) + static_cast<u32>(quantise(grad[k], opt_)));
    }
    return loss;
}

void MlpReference::clearAccumulator() { std::fill(acc_.begin(), acc_.end(), 0); }

// HLSL twin: CSAdam. One GPU thread per weight, in exactly this order.
void MlpReference::adamStep(u32 liveCount) {
    if (liveCount == 0) { clearAccumulator(); return; }
    ++step_;
    const f32 bc1 = adamBiasCorrection(opt_.beta1, step_);
    const f32 bc2 = adamBiasCorrection(opt_.beta2, step_);
    for (u32 k = 0; k < layout_.total; ++k) {
        f32 g = (static_cast<f32>(acc_[k]) / opt_.gradFixedScale) / static_cast<f32>(liveCount);
        g = std::min(std::max(g, -opt_.gradClamp), opt_.gradClamp);
        g += opt_.l2 * w_[k];
        m_[k] = opt_.beta1 * m_[k] + (1.0f - opt_.beta1) * g;
        v_[k] = opt_.beta2 * v_[k] + (1.0f - opt_.beta2) * g * g;
        const f32 mhat = m_[k] / bc1;
        const f32 vhat = v_[k] / bc2;
        w_[k] -= opt_.learningRate * mhat / (std::sqrt(vhat) + opt_.epsilon);
        ema_[k] = opt_.weightEma * ema_[k] + (1.0f - opt_.weightEma) * w_[k];
    }
    clearAccumulator();
}

f32 MlpReference::trainBatch(std::span<const f32> records, std::span<const f32> targets, u32 count) {
    f32 total = 0.0f;
    for (u32 r = 0; r < count; ++r)
        total += accumulateRecord(records.subspan(static_cast<usize>(r) * desc_.inputs, desc_.inputs),
                                  targets.subspan(static_cast<usize>(r) * desc_.outputs, desc_.outputs));
    adamStep(count);
    return count ? total / static_cast<f32>(count) : 0.0f;
}

f32 MlpReference::evaluate(std::span<const f32> records, std::span<const f32> targets, u32 count,
                           bool useEma) const {
    if (count == 0) return 0.0f;
    std::vector<f32> out(desc_.outputs), acts;
    f32 total = 0.0f;
    for (u32 r = 0; r < count; ++r) {
        forward(records.subspan(static_cast<usize>(r) * desc_.inputs, desc_.inputs), out, useEma, &acts);
        for (u32 o = 0; o < desc_.outputs; ++o)
            total += lossValue(opt_.loss, out[o], targets[static_cast<usize>(r) * desc_.outputs + o]);
    }
    return total / static_cast<f32>(count);
}

// ---------------------------------------------------------------- weight file

namespace {
constexpr usize kHeaderWords = 10;   // magic, version, 7 shape words, weightCount
}

bool saveWeightFile(const std::string& path, const MlpDesc& d, std::span<const f32> weights) {
    if (!validate(d)) return false;
    if (weights.size() != MlpLayout::make(d).total) return false;
    const u32 header[kHeaderWords] = {
        kWeightFileMagic, kWeightFileVersion, d.inputs, d.outputs, d.hiddenWidth, d.hiddenLayers,
        static_cast<u32>(d.hidden), static_cast<u32>(d.output), d.bias ? 1u : 0u,
        static_cast<u32>(weights.size())};
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(header), sizeof(header));
    f.write(reinterpret_cast<const char*>(weights.data()),
            static_cast<std::streamsize>(weights.size() * sizeof(f32)));
    return static_cast<bool>(f);
}

bool loadWeightFile(const std::string& path, MlpDesc& d, std::vector<f32>& weights) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize size = f.tellg();
    constexpr std::streamsize kHeaderBytes = (kHeaderWords) * sizeof(u32);
    if (size < kHeaderBytes) return false;
    f.seekg(0);
    u32 h[kHeaderWords] = {};
    f.read(reinterpret_cast<char*>(h), sizeof(h));
    if (!f || h[0] != kWeightFileMagic || h[1] != kWeightFileVersion) return false;
    MlpDesc nd;
    nd.inputs = h[2]; nd.outputs = h[3]; nd.hiddenWidth = h[4]; nd.hiddenLayers = h[5];
    if (h[6] > 3u || h[7] > 3u || h[8] > 1u) return false;
    nd.hidden = static_cast<Activation>(h[6]);
    nd.output = static_cast<Activation>(h[7]);
    nd.bias = h[8] != 0;
    if (!validate(nd)) return false;
    const u32 count = MlpLayout::make(nd).total;
    if (h[9] != count) return false;
    if (size != kHeaderBytes + static_cast<std::streamsize>(count) * static_cast<std::streamsize>(sizeof(f32)))
        return false;
    std::vector<f32> w(count);
    f.read(reinterpret_cast<char*>(w.data()), static_cast<std::streamsize>(count * sizeof(f32)));
    if (!f) return false;
    nd.seed = d.seed;   // not stored in the file; keep the caller's
    d = nd;
    weights = std::move(w);
    return true;
}

}  // namespace aver::render::neural
