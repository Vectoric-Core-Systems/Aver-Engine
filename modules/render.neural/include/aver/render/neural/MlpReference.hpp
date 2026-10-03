// MlpReference -- the CPU twin of Aver.Render.Neural's GPU network, and the SPEC for its HLSL.
//
// WHAT THIS FILE IS FOR. shaders/aver_neural_mlp.hlsl and this reference implement EXACTLY the same
// maths -- weight layout, He-uniform init, forward, both losses, backward, the fixed-point gradient
// quantisation, Adam with bias correction, and the weight EMA. The GPU path cannot be unit-tested
// headless (the suite has no device), so the CPU path is where every property is actually checked:
// finite-difference gradients, a toy function being learned, order-independent accumulation, the
// file format. When one of the two changes, the other changes in the same commit; a drift between
// them means the tests are verifying a network the engine does not run.
//
// "EXACTLY" has one honest asterisk: the GPU may fuse a multiply and an add (mad) and uses its own
// exp(), so forward values agree to a few ulp, not bit for bit. The INTEGER half of training --
// the quantised gradient accumulator -- is where bit-exactness is promised, and it holds as long as
// the float gradient each record produces is the same float on both sides; a one-ulp difference can
// move a value across a quantisation step and change one accumulator count by 1. That is noise at
// the 1e-5 level of a gradient, and it is why the tests assert order-independence on the CPU side
// (a property of integer addition) rather than CPU == GPU equality.
//
// This header is RHI-free on purpose: it carries the descriptor types (Mlp.hpp includes it), so the
// reference and its test build and run without a device, a backend or any GPU header.
#pragma once

#include <aver/core/Types.hpp>

#include <span>
#include <string>
#include <vector>

namespace aver::render::neural {

// Numeric values are the AVER_NN_*_ACT defines the HLSL is compiled with. Do not renumber.
enum class Activation : u32 { None = 0, ReLU = 1, Sigmoid = 2, Exp = 3 };

// RelativeL2 is NRC's loss: (y - p)^2 / (stopgrad(p)^2 + 0.01) per channel, which keeps a bright
// and a dim sample pulling with comparable force. "stopgrad" means the denominator is treated as a
// constant when differentiating: d/dp = 2 (p - y) / (p^2 + 0.01). A record's loss is the SUM over
// its channels; a batch's loss is the MEAN over records.
enum class Loss : u32 { L2 = 0, RelativeL2 = 1 };

// A fully connected network: `inputs` -> hiddenLayers x hiddenWidth -> `outputs`. So there are
// hiddenLayers + 1 weight layers. `hidden` follows every hidden layer, `output` the last one.
struct MlpDesc {
    u32 inputs = 1, outputs = 1, hiddenWidth = 8, hiddenLayers = 1;
    Activation hidden = Activation::ReLU;
    Activation output = Activation::None;
    bool bias = true;
    // Seeds the deterministic init (initWeights). Not part of what a weight file must match.
    u32 seed = 1;
};

// Adam, with the fixed-point gradient accumulation the GPU needs (D3D12 has no portable float
// atomic add, so gradients are summed as integers -- see MlpReference::quantise).
struct OptimiserDesc {
    f32 learningRate = 1e-3f;
    f32 beta1 = 0.9f, beta2 = 0.99f, epsilon = 1e-8f;
    // Inference reads the exponential moving average of the weights, not the raw weights: the raw
    // ones jitter step to step and a radiance cache that flickers with them is visible.
    f32 weightEma = 0.99f;
    f32 l2 = 0.0f;   // L2 weight decay, added to the gradient (not decoupled)
    Loss loss = Loss::L2;
    // Per-record, per-weight gradient: clamp(g, -gradClamp, gradClamp) * gradFixedScale, truncated
    // to int. See MlpReference::safeBatchLimit for the headroom this buys and costs.
    f32 gradFixedScale = 65536.0f;
    f32 gradClamp = 16.0f;
};

inline constexpr u32 kMaxHiddenWidth  = 64;
inline constexpr u32 kMaxHiddenLayers = 6;
inline constexpr u32 kMaxInputs       = 64;
inline constexpr u32 kMaxOutputs      = 16;
inline constexpr u32 kMaxWeightLayers = kMaxHiddenLayers + 1;

// True when the descriptor is inside the ranges the kernels are written for. On false, `why` (if
// given) says which limit was hit.
bool validate(const MlpDesc& d, std::string* why = nullptr);
bool validate(const OptimiserDesc& o, std::string* why = nullptr);

// ---------------------------------------------------------------- weight layout
// ONE FLAT f32 ARRAY, layer by layer. Layer l maps in(l) -> out(l) values:
//     W[l]  out(l) x in(l), ROW-MAJOR BY OUTPUT: W[l][o * in(l) + i]
//     b[l]  out(l) values, immediately after W[l] (absent when MlpDesc::bias is false)
// then layer l+1 starts. The GPU's per-layer groupshared tile is one contiguous slice of this
// array, which is why W and b sit together. The HLSL recomputes these offsets from its defines;
// both must stay equal to what this struct says.
struct MlpLayout {
    u32 layers = 0;
    u32 inDim[kMaxWeightLayers]  = {};
    u32 outDim[kMaxWeightLayers] = {};
    u32 wOffset[kMaxWeightLayers] = {};   // first W element of the layer
    u32 bOffset[kMaxWeightLayers] = {};   // first bias element (== wOffset + out*in); unused if !bias
    u32 layerSize[kMaxWeightLayers] = {}; // floats in the layer's slice (W, plus b when present)
    u32 total = 0;                        // weightCount
    // Activations of one forward pass are kept in one array: actOffset[0] is the input, and
    // actOffset[l + 1] is layer l's post-activation output. actTotal floats in all.
    u32 actOffset[kMaxWeightLayers + 1] = {};
    u32 actTotal = 0;

    static MlpLayout make(const MlpDesc& d);
};

// ---------------------------------------------------------------- scalar maths
// Activations, and their derivatives expressed in the POST-activation value y = act(z), so a
// forward pass need only keep y: relu' = y > 0, sigmoid' = y (1 - y), exp' = y, identity' = 1.
// Exp clamps its argument at kExpMaxArg as an overflow guard; the gradient is still passed through
// there (derivative y), a deliberate simplification -- a network sitting on the clamp is already
// far outside anything the loss can use.
inline constexpr f32 kExpMaxArg = 20.0f;
f32 activate(Activation a, f32 z);
f32 activationDerivative(Activation a, f32 y);

// d(loss)/d(prediction) for one channel, with the denominator held constant for RelativeL2.
f32 lossGradient(Loss l, f32 p, f32 y);
// The loss of one channel.
f32 lossValue(Loss l, f32 p, f32 y);

// Deterministic init. He-uniform: w = (2u - 1) * sqrt(6 / fan_in), u in [0,1) from initHash;
// biases are zero. `u` for weight index k is initHash(seed * 0x9E3779B9 + k) >> 8, over 2^24 --
// a PCG output hash (O'Neill's pcg_hash, public domain), an integer function of (seed, k) only,
// so the same descriptor gives the same weights on every machine and build.
u32 initHash(u32 x);
std::vector<f32> initWeights(const MlpDesc& d);

// Adam's bias-correction denominators 1 - beta^t, computed in double and rounded once so the CPU
// reference and the GPU (which is handed this value in its constant buffer) agree exactly.
f32 adamBiasCorrection(f32 beta, u32 step);

// The worst-case largest batch whose per-weight accumulator cannot overflow an int32: every record
// would have to saturate the clamp with the same sign. Real gradients are far below the clamp, so
// this is a floor, not a typical limit -- but the accumulator WRAPS on overflow (an atomic add
// cannot saturate), and a wrapped weight gradient is a silent wrong answer, so Mlp warns when a
// batch exceeds it. Defaults: floor((2^31 - 1) / (16 * 65536)) = 2047.
u32 safeBatchLimit(const OptimiserDesc& o);

// ---------------------------------------------------------------- the network
class MlpReference {
public:
    MlpReference(const MlpDesc& d, const OptimiserDesc& o);

    const MlpDesc& desc() const { return desc_; }
    const OptimiserDesc& optimiser() const { return opt_; }
    const MlpLayout& layout() const { return layout_; }
    u32 weightCount() const { return layout_.total; }

    // Master weights, EMA weights, Adam moments, and the integer gradient accumulator.
    std::vector<f32>& weights() { return w_; }
    const std::vector<f32>& weights() const { return w_; }
    const std::vector<f32>& ema() const { return ema_; }
    const std::vector<f32>& moment1() const { return m_; }
    const std::vector<f32>& moment2() const { return v_; }
    const std::vector<i32>& accumulator() const { return acc_; }
    u32 step() const { return step_; }

    // Master = EMA = `w`; Adam moments, accumulator and step counter reset. Sizes must match.
    bool setWeights(std::span<const f32> w);

    // Forward pass of one record. `out` has desc.outputs floats; `useEma` picks the weight set.
    // `acts`, when given, receives every layer's post-activation values (layout().actTotal).
    void forward(std::span<const f32> in, std::span<f32> out, bool useEma = false,
                 std::vector<f32>* acts = nullptr) const;

    // One record's loss, and its UNQUANTISED gradient w.r.t. every master weight (grad is
    // weightCount floats, overwritten). Runs on the master weights, like the GPU training kernel.
    f32 backward(std::span<const f32> in, std::span<const f32> target, std::span<f32> grad) const;

    // The quantisation the GPU applies to every per-record, per-weight gradient before the integer
    // add: int(clamp(g, -gradClamp, gradClamp) * gradFixedScale). Truncates toward zero, exactly as
    // HLSL's and C++'s float-to-int conversions do.
    static i32 quantise(f32 g, const OptimiserDesc& o);

    // Adds one record's quantised gradient to the accumulator. INTEGER ADDS, WRAPPING at 2^32 (done
    // in u32 so the C++ is defined, and bit-identical to the GPU's InterlockedAdd). Because integer
    // addition is associative and commutative the final accumulator does not depend on the order
    // records arrive in, nor on how they are grouped -- the GPU sums within a thread group first,
    // then atomically across groups, and this is why that is safe. Returns the record's loss.
    f32 accumulateRecord(std::span<const f32> in, std::span<const f32> target);
    void clearAccumulator();

    // One optimiser step from the accumulator over `liveCount` records, then clears the
    // accumulator. liveCount == 0 changes nothing but the clear. Order per weight:
    //   g = acc / gradFixedScale / liveCount, clamped to +-gradClamp, then g += l2 * w;
    //   Adam (bias-corrected, step counted from 1); then ema = d * ema + (1 - d) * w.
    void adamStep(u32 liveCount);

    // accumulateRecord over records [0, count), then adamStep(count). Returns the MEAN loss of the
    // batch, measured before the step. `records` is count * inputs floats, `targets` count * outputs.
    f32 trainBatch(std::span<const f32> records, std::span<const f32> targets, u32 count);

    // Mean loss over a batch with no state change.
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

// ---------------------------------------------------------------- weight file
// A tiny binary format, little-endian, so frame-generation weights can ship later:
//   u32 magic   'AVNN' (bytes 41 56 4E 4E, i.e. 0x4E4E5641 as a little-endian u32)
//   u32 version 1
//   u32 inputs, outputs, hiddenWidth, hiddenLayers, hidden (Activation), output (Activation),
//       bias (0/1), weightCount   -- the MlpDesc, minus its seed (init only), then the count
//   f32 weights[weightCount]      -- the flat layout above, biases included
// A reader rejects a wrong magic or version, an invalid shape, a count that disagrees with the
// shape, and a file shorter or longer than its header says.
inline constexpr u32 kWeightFileMagic   = 0x4E4E5641u;
inline constexpr u32 kWeightFileVersion = 1u;
bool saveWeightFile(const std::string& path, const MlpDesc& d, std::span<const f32> weights);
bool loadWeightFile(const std::string& path, MlpDesc& d, std::vector<f32>& weights);

}  // namespace aver::render::neural
