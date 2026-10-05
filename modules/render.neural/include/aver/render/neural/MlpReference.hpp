// MlpReference -- CPU twin of Aver.Render.Neural GPU shader; defines the spec both must match exactly.
//
// CPU path is the test bed (GPU cannot be unit-tested headless), and the reference for bit-exact training
// (integer gradient accumulation). Float forward agrees to a few ULP; integer training is bit-exact when
// gradients match. RHI-free by design; no device/backend/GPU headers needed.
#pragma once

#include <aver/core/Types.hpp>
#include <aver/render/neural/NeuralOptimiser.hpp>

#include <span>
#include <string>
#include <vector>

namespace aver::render::neural {

// Fully connected network: inputs -> hiddenLayers x hiddenWidth -> outputs.
struct MlpDesc {
    u32 inputs = 1, outputs = 1, hiddenWidth = 8, hiddenLayers = 1;
    Activation hidden = Activation::ReLU;
    Activation output = Activation::None;
    bool bias = true;
    u32 seed = 1;  // Deterministic init seed; not part of weight file.
};

inline constexpr u32 kMaxHiddenWidth  = 64;
inline constexpr u32 kMaxHiddenLayers = 6;
inline constexpr u32 kMaxInputs       = 64;
inline constexpr u32 kMaxOutputs      = 16;
inline constexpr u32 kMaxWeightLayers = kMaxHiddenLayers + 1;

// True when the descriptor is inside the ranges the kernels are written for. On false, `why` (if
// given) says which limit was hit.
bool validate(const MlpDesc& d, std::string* why = nullptr);

// ---- weight layout
// ONE FLAT f32 ARRAY, layer by layer. Layer l maps in(l) -> out(l):
//     W[l]  out(l) x in(l), ROW-MAJOR BY OUTPUT: W[l][o * in(l) + i]
//     b[l]  out(l) values immediately after (omitted when MlpDesc::bias is false)
// GPU per-layer groupshared tile is one contiguous slice. HLSL recomputes these offsets; both must stay equal.
struct MlpLayout {
    u32 layers = 0;
    u32 inDim[kMaxWeightLayers]  = {};
    u32 outDim[kMaxWeightLayers] = {};
    u32 wOffset[kMaxWeightLayers] = {};   // first W element
    u32 bOffset[kMaxWeightLayers] = {};   // first bias element (omitted if !bias)
    u32 layerSize[kMaxWeightLayers] = {}; // floats in layer's slice (W + b when present)
    u32 total = 0;                        // total weight count
    // Activations of one forward pass in one array: actOffset[0] is input; actOffset[l+1] is layer l post-activation.
    u32 actOffset[kMaxWeightLayers + 1] = {};
    u32 actTotal = 0;

    static MlpLayout make(const MlpDesc& d);
};

// He-uniform init: w = (2u - 1) * sqrt(6 / fan_in); u from initHash(seed, index) (NeuralOptimiser.hpp).
std::vector<f32> initWeights(const MlpDesc& d);

// ---- the network
class MlpReference {
public:
    MlpReference(const MlpDesc& d, const OptimiserDesc& o);

    const MlpDesc& desc() const { return desc_; }
    const OptimiserDesc& optimiser() const { return opt_; }
    // Next step's learning rate; returns false if not finite and > 0.
    bool setLearningRate(f32 lr) {
        if (!(lr > 0.0f) || !(lr < 3.4e38f)) return false;
        opt_.learningRate = lr;
        return true;
    }
    const MlpLayout& layout() const { return layout_; }
    u32 weightCount() const { return layout_.total; }

    // Master weights, EMA weights, Adam moments m1/m2, and integer gradient accumulator.
    std::vector<f32>& weights() { return w_; }
    const std::vector<f32>& weights() const { return w_; }
    const std::vector<f32>& ema() const { return ema_; }
    const std::vector<f32>& moment1() const { return m_; }
    const std::vector<f32>& moment2() const { return v_; }
    const std::vector<i32>& accumulator() const { return acc_; }
    u32 step() const { return step_; }

    // Set master = EMA = w; reset Adam moments, accumulator, step counter. Sizes must match.
    bool setWeights(std::span<const f32> w);

    // Forward pass of one record. out has desc.outputs floats; useEma picks weight set. acts receives post-activation values.
    void forward(std::span<const f32> in, std::span<f32> out, bool useEma = false,
                 std::vector<f32>* acts = nullptr) const;

    // One record's loss and UNQUANTISED gradient w.r.t. every master weight. Runs on master weights like GPU kernel.
    f32 backward(std::span<const f32> in, std::span<const f32> target, std::span<f32> grad) const;

    // GPU quantisation: int(clamp(g, -gradClamp, gradClamp) * gradFixedScale), truncating toward zero like HLSL/C++.
    static i32 quantise(f32 g, const OptimiserDesc& o);

    // Add one record's quantised gradient to accumulator with integer wraparound (order-independent; GPU atomics are associative/commutative).
    f32 accumulateRecord(std::span<const f32> in, std::span<const f32> target);
    void clearAccumulator();

    // One optimiser step over liveCount records: g = acc/gradFixedScale/liveCount clamped ±gradClamp; g += l2*w; Adam (bias-corrected); ema = d*ema + (1-d)*w.
    void adamStep(u32 liveCount);

    // accumulateRecord over records [0, count), then adamStep(count). Returns MEAN loss before step.
    f32 trainBatch(std::span<const f32> records, std::span<const f32> targets, u32 count);

    // Mean loss over batch with no state change.
    f32 evaluate(std::span<const f32> records, std::span<const f32> targets, u32 count,
                 bool useEma = false) const;

private:
    MlpDesc desc_;
    OptimiserDesc opt_;
    MlpLayout layout_;
    std::vector<f32> w_, ema_, m_, v_;
    std::vector<i32> acc_;
    u32 step_ = 0;
};

// ---- weight file
// Binary format, little-endian:
//   u32 magic 'AVNN' (0x4E4E5641)
//   u32 version 1
//   u32 inputs, outputs, hiddenWidth, hiddenLayers, hidden (Activation), output (Activation), bias (0/1), weightCount
//   f32 weights[weightCount]
// Reader rejects wrong magic/version, invalid shape, count mismatch, and truncated files.
inline constexpr u32 kWeightFileMagic   = 0x4E4E5641u;
inline constexpr u32 kWeightFileVersion = 1u;
bool saveWeightFile(const std::string& path, const MlpDesc& d, std::span<const f32> weights);
bool loadWeightFile(const std::string& path, MlpDesc& d, std::vector<f32>& weights);

}  // namespace aver::render::neural
