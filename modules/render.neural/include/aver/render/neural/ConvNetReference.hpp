// ConvNetReference -- CPU twin of the convolution kernels (shaders/aver_neural_conv.hlsl); defines the
// spec both must match. KEEP IN SYNC WITH aver_neural_conv.hlsl.
//
// RHI-free by design. The GPU fuses multiply-adds, so forward values agree to ~1e-5 relative, not bit
// for bit; training is bit-deterministic per device (ordered partials, no atomics).
//
// ---- THE CONTRACT THE HLSL MIRRORS
//
// Tensors: NCHW, row-major, one flat f32 array each. Layer l maps [n][cin][h][w] to [n][cout][oh][ow]
//   with oh = ceil(h / stride), ow = ceil(w / stride).
//
// Weights: ONE FLAT f32 ARRAY, layer by layer: W[co][ci][ky][kx] (OIHW, row-major), then b[co]
//   (omitted when ConvLayerDesc::bias is false). Offsets in ConvLayout.
//
// Padding is zeros. 3x3: output (oy, ox) reads input row oy*stride - 1 + ky, column ox*stride - 1 + kx,
//   ky, kx in 0..2 (PyTorch pad = 1). 1x1: input (oy, ox). Out-of-range taps read 0 and are still added
//   (so out-of-range GPU threads "compute on zeros").
//
// Forward summation order (canonical), per output element, fp32:
//     acc = bias (or 0)
//     for ci ascending { for ky ascending { for kx ascending { acc += w * x } } }
//     y = act(acc)          // ReLU is `z > 0 ? z : 0` (NaN -> 0); None is the identity
//
// Backward, standard conv backward, with dz = (act == ReLU ? (y > 0 ? dy : 0) : dy), y the post-activation:
//   dX (gather form, canonical order): for each input element (iy, ix) of channel ci:
//     acc = 0; for co ascending { for ky ascending { for kx ascending {
//         ty = iy + pad - ky; tx = ix + pad - kx;                 // pad = 1 for 3x3, 0 for 1x1
//         if (ty, tx >= 0 and divisible by stride and ty/stride < oh and tx/stride < ow)
//             acc += w[co][ci][ky][kx] * dz[co][ty/stride][tx/stride]; } } }
//   dW[co][ci][ky][kx] = sum over (n, oy, ox) of dz[co][oy][ox] * x(ci, oy*stride - pad + ky, ox*stride - pad + kx)
//   db[co]             = sum over (n, oy, ox) of dz[co][oy][ox]
//   dX is only needed for layers > 0 on the GPU; the CPU can also return the first layer's.
//
// Loss: weighted L2 over the head output with a per-POSITION weight pw[n][y][x] shared across channels:
//     L = sum pw * (p - t)^2 / lossNorm           lossNorm is passed explicitly (e.g. n * outH * outW)
//     dL/dp = (2 * pw * (p - t)) / lossNorm       (fp32, in that order)
//   The CPU sums L in fp64 (the GPU reduces in a fixed-order tree); only the gradient is spec.
//
// GPU-order gradient accumulation (accumulateBatch / backwardGpuOrder): per layer, partials per
//   (record n, 8x8 output tile (ty, tx) of THAT layer's output, weight w): the sum over the tile's
//   output positions in row-major order (y then x, clipped to the output) of dz * x (bias: dz).
//   Partial index p = (n * tilesY + ty) * tilesX + tx, tilesX = ceil(ow / 8), tilesY = ceil(oh / 8).
//   Per weight: g = sum over p ascending (fp32). The gradient is then quantised ONCE:
//   acc[w] = quantise(g) (clamp +-gradClamp, * gradFixedScale, truncate toward zero) and adamStep runs
//   with liveCount = 1 (the loss is already normalised by lossNorm).
//
// Init: He-uniform (2u - 1) * sqrt(6 / fan_in), fan_in = cin * k * k, u = (initHash(seed * 0x9E3779B9 +
//   globalWeightIndex) >> 8) / 2^24, exactly as initWeights does for the MLP. Biases 0. The LAST layer
//   (head) is all zero, weights and bias (give it act None: a zero ReLU head has no gradient).
//
// Groupshared budget (the GPU's constraint, checked by validate): one block holds CO_BLOCK = 8 output
//   channels x min(cin, CI_CHUNK = 16) input channels x k*k weights, 4 bytes each (convSharedBytes) <= 16 KB.
#pragma once

#include <aver/core/Types.hpp>
#include <aver/render/neural/NeuralOptimiser.hpp>

#include <span>
#include <string>
#include <vector>

namespace aver::render::neural {

struct ConvLayerDesc {
    u32 cin = 4, cout = 4;
    u32 kernel = 3;   // 1 or 3
    u32 stride = 1;   // 1 or 2 (1x1 needs 1)
    Activation act = Activation::ReLU;   // None or ReLU only
    bool bias = true;
};

struct ConvNetDesc {
    u32 inChannels = 1;
    std::vector<ConvLayerDesc> layers;   // 1..kConvMaxLayers
    u32 seed = 1;   // Deterministic init seed; not part of the weight file.
};

struct TensorShape {
    u32 n = 1, c = 1, h = 1, w = 1;
    usize count() const { return static_cast<usize>(n) * c * h * w; }
    usize planeCount() const { return static_cast<usize>(h) * w; }   // one (n, c) plane
    bool operator==(const TensorShape&) const = default;
};

inline constexpr u32 kConvMaxLayers   = 8;
inline constexpr u32 kConvMaxChannels = 64;
inline constexpr u32 kConvCoBlock     = 8;    // output channels per GPU block
inline constexpr u32 kConvCiChunk     = 16;   // input channels staged per GPU chunk
inline constexpr u32 kConvTile        = 8;    // gradient-partial tile edge, in output positions
inline constexpr u32 kConvSharedLimitBytes = 16384;

// Groupshared bytes of one layer's weight block on the GPU: 4 * CO_BLOCK * min(cin, CI_CHUNK) * k * k.
u32 convSharedBytes(const ConvLayerDesc& l);

// Output size along one axis: ceil(size / stride).
inline u32 convOutSize(u32 size, u32 stride) { return (size + stride - 1u) / stride; }

// True when the descriptor is inside the ranges the kernels are written for. On false, `why` (if given)
// says which limit was hit. Rules: 1..8 layers; cin chain matches (layer 0 cin == inChannels, then the
// previous cout); inChannels 1..64; every cout a multiple of 4 in [4, 64]; kernel 1|3; kernel 1 needs
// stride 1; stride 1|2; act None|ReLU; convSharedBytes <= 16384.
bool validate(const ConvNetDesc& d, std::string* why = nullptr);

// Optimiser defaults for conv nets: gradFixedScale 2^24, gradClamp 32 (inside validate(OptimiserDesc)'s
// 1e9 cap), everything else default.
OptimiserDesc convDefaults();

// ---- weight layout
struct ConvLayout {
    u32 layers = 0;
    u32 cin[kConvMaxLayers]    = {};
    u32 cout[kConvMaxLayers]   = {};
    u32 kernel[kConvMaxLayers] = {};
    u32 stride[kConvMaxLayers] = {};
    u32 wOffset[kConvMaxLayers] = {};    // first W element
    u32 bOffset[kConvMaxLayers] = {};    // first bias element (omitted from the array if !bias)
    u32 layerSize[kConvMaxLayers] = {};  // floats in the layer's slice (W + b when present)
    u32 total = 0;                       // total weight count

    // On an invalid descriptor (more than kConvMaxLayers) only the first kConvMaxLayers are laid out.
    static ConvLayout make(const ConvNetDesc& d);

    // Output shape of `layer` for that layer's input shape: {n, cout, ceil(h/stride), ceil(w/stride)}.
    TensorShape outDims(u32 layer, const TensorShape& in) const;
};

// He-uniform init with the head layer zeroed (see the contract above).
std::vector<f32> initConvWeights(const ConvNetDesc& d);

// ---- the network
class ConvNetReference {
public:
    // An invalid descriptor leaves valid() false and the network empty (no weights).
    ConvNetReference(const ConvNetDesc& d, const OptimiserDesc& o);

    bool valid() const { return valid_; }
    const ConvNetDesc& desc() const { return desc_; }
    const OptimiserDesc& optimiser() const { return opt_; }
    // Next step's learning rate; returns false if not finite and > 0.
    bool setLearningRate(f32 lr) {
        if (!(lr > 0.0f) || !(lr < 3.4e38f)) return false;
        opt_.learningRate = lr;
        return true;
    }
    const ConvLayout& layout() const { return layout_; }
    u32 weightCount() const { return layout_.total; }

    // Master weights, EMA weights, Adam moments, integer gradient accumulator, step counter.
    std::vector<f32>& weights() { return w_; }
    const std::vector<f32>& weights() const { return w_; }
    const std::vector<f32>& ema() const { return ema_; }
    const std::vector<f32>& moment1() const { return m_; }
    const std::vector<f32>& moment2() const { return v_; }
    const std::vector<i32>& accumulator() const { return acc_; }
    u32 step() const { return step_; }

    // Set master = EMA = w; reset Adam moments, accumulator, step counter. Sizes must match.
    bool setWeights(std::span<const f32> w);

    // Head output shape for an input shape (n, h and w carry through the strides).
    TensorShape outputShape(const TensorShape& in) const;

    // Forward pass. `input` has in.count() floats, `out` outputShape(in).count(); a size or channel
    // mismatch does nothing. acts (optional) receives every layer's post-activation output, acts[l] for layer l.
    void forward(const TensorShape& in, std::span<const f32> input, std::span<f32> out, bool useEma = false,
                 std::vector<std::vector<f32>>* acts = nullptr) const;

    // Gradient of sum(dOut * head output) w.r.t. every master weight, UNQUANTISED, plain summation (one
    // running sum per weight over n, y, x). dOut has outputShape(in).count() floats; gradW weightCount().
    // dIn (optional) receives the gradient w.r.t. the input (the first layer's dX).
    void backward(const TensorShape& in, std::span<const f32> input, std::span<const f32> dOut,
                  std::span<f32> gradW, std::vector<f32>* dIn = nullptr) const;

    // The same gradient summed in the GPU's partial order (see the contract); still unquantised.
    void backwardGpuOrder(const TensorShape& in, std::span<const f32> input, std::span<const f32> dOut,
                          std::span<f32> gradW) const;

    // Forward on the MASTER weights, loss gradient, backward in GPU partial order, quantise once into
    // accumulator() (overwritten). target is outputShape(in).count() floats; posWeight n * oh * ow.
    // Returns the loss L (already divided by lossNorm).
    f32 accumulateBatch(const TensorShape& in, std::span<const f32> input, std::span<const f32> target,
                        std::span<const f32> posWeight, f32 lossNorm);

    // adamStep(liveCount = 1) over the accumulator: counts the step, updates master, moments and EMA,
    // clears the accumulator.
    void adamStep();

    // accumulateBatch then adamStep. Returns the loss before the step.
    f32 trainBatch(const TensorShape& in, std::span<const f32> input, std::span<const f32> target,
                   std::span<const f32> posWeight, f32 lossNorm);

    // The loss L over a batch with no state change (EMA weights by default).
    f32 evaluate(const TensorShape& in, std::span<const f32> input, std::span<const f32> target,
                 std::span<const f32> posWeight, f32 lossNorm, bool useEma = true) const;

private:
    ConvNetDesc desc_;
    OptimiserDesc opt_;
    ConvLayout layout_;
    bool valid_ = false;
    std::vector<f32> w_, ema_, m_, v_;
    std::vector<i32> acc_;
    u32 step_ = 0;
};

}  // namespace aver::render::neural
