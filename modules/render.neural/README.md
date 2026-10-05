# Aver.Render.Neural

Small neural networks that run inside the frame: a fully connected MLP with GPU inference and GPU
training, in portable fp32 HLSL, plus a CPU reference of the same maths that is the spec for the shader
and what the tests check. Convolution layers (`ConvNet`, for NRD2) run on the GPU too, inference and
training, against their own CPU reference (below).

It is an RHI-only static module like `Aver.Render.Denoise`: it links `Aver.Core` and `Aver.RHI`,
owns its shader (deployed beside the executable by `aver_deploy_shaders`), records into a caller's
`IRenderContext`, and never sees Voxi types. Design: `docs/rendering/NEURAC.md`, section 6.

## Why it exists

The radiance cache and frame interpolation both need networks that live inside the frame: records
written by one compute pass, answers read by the next, training steps interleaved with rendering,
nothing crossing back to the CPU. So the interface is structured buffers in, structured buffers out.
The caller owns every buffer it feeds in; the network owns its weights, an EMA copy of them, Adam's
moments and a gradient accumulator.

## API

```cpp
#include <aver/render/neural/Mlp.hpp>
using namespace aver::render::neural;

MlpDesc d;  d.inputs = 8; d.outputs = 3; d.hiddenWidth = 32; d.hiddenLayers = 2;
d.output = Activation::Exp;                       // e.g. radiance >= 0
OptimiserDesc o;  o.loss = Loss::RelativeL2;

Mlp net;
if (!net.create(device, d, o)) { /* shader/pipeline failed: WARN logged once, run without it */ }

// records:  StructuredBuffer<float>, maxCount * inputs floats (caller-owned)
// outputs:  RW structured float buffer, maxCount * outputs floats (allowUnorderedAccess)
// count:    a uint buffer whose element 0 is the live count, written by a GPU producer pass
net.recordInfer(ctx, records, outputs, countBuffer, maxCount);          // reads the EMA weights
net.recordInfer(ctx, records, outputs, CpuCount{n});                    // count known on the CPU
net.recordTrain(ctx, records, targets, countBuffer, maxCount);          // one Adam step, mean gradient
net.recordTrain(ctx, records, targets, CpuCount{n});
```

* `MlpDesc`: `inputs` [1, 64], `outputs` [1, 16], `hiddenWidth` a multiple of 4 in [4, 64],
  `hiddenLayers` [1, 6], `hidden` / `output` activation (`None`, `ReLU`, `Sigmoid`, `Exp`), `bias`,
  `seed`.
* `OptimiserDesc`: Adam (`learningRate`, `beta1`, `beta2`, `epsilon`), `weightEma`, `l2`, `loss`
  (`L2` or `RelativeL2`, NRC's `(y - p)^2 / (stopgrad(p)^2 + 0.01)` per channel), and the fixed-point
  knobs `gradFixedScale` and `gradClamp`.
* Buffer states: nothing transitions implicitly in this RHI. The network's own buffers rest in
  `Common`; a caller says what state its buffers rest in with `IoStates` (default `Common`) and
  gets them back there. Caller buffers must be Default-kind, not Upload.
* Weights: `uploadWeights`, `weightCount`, `saveWeights` / `loadWeights` (format below),
  `recordReadback` + `collectWeights` (see "The readback gap").
* `backend()` is `Backend::PortableFp32`. `PortableFp16`, `VulkanCoopMatrix` and `D3D12LinAlg` are
  reserved values in the design doc and are not declared yet.
* `invalidateBindings()` must be called before the caller destroys or reallocates a buffer it has
  passed in (binding sets are cached per buffer tuple, and a set naming a dead buffer faults the
  device).

## Kernel structure, and why

`shaders/aver_neural_mlp.hlsl` has four entry points, all `[numthreads(64,1,1)]`: `CSInfer` and
`CSTrainGrad` (one thread per **record**), `CSAdam` and `CSResetState` (one thread per **weight**).
The shape arrives as DXC defines, so every network compiles its own pipelines with constant loop
bounds. There are no wave intrinsics and no assumption about 32 or 64 lanes; every barrier is in
control flow that depends only on compile-time constants, so AMD wave32/64, NVIDIA and Intel behave
alike. The constant block, activations, `quantise` and the Adam / reset bodies are shared with the conv
kernels through `shaders/aver_neural_common.hlsli`.

**The kernel streams weights layer by layer through groupshared memory.** The thread group
cooperatively loads one layer's weights into groupshared, syncs, each thread computes that layer for
its own record with its activations in a thread-local array, then the next layer's weights are loaded
over the same memory. Records never exchange intermediates through shared memory, and the weights
are never held in registers across layers.

This is a deliberate choice, recorded in the shader and in `docs/rendering/NEURAC.md` section
9. The patents US 11,631,210 and 11,935,179 describe a fully fused kernel: weights loaded once into
the register file, intermediates exchanged through shared memory, layer by layer. Layer-streaming is
the shape this module uses to stay clear of that. Whether it is far enough is counsel's call (this is
a flag, not legal advice), and the kernel **must not be "optimised" toward the fused shape** without
that review. At cache scale the maths is cheap and memory traffic is the cost, so the trade is
sound on its merits too.

### Training

Per record: forward on the master weights (activations kept in a local array), loss gradient,
backward. Per layer, the group reduces the quantised per-weight gradients in groupshared (a
transposed 64x64 integer grid, 64 weights at a time) and issues **one `InterlockedAdd` per weight per
group**, not per record. `CSAdam` then reads the accumulator, takes the mean over the live count,
clamps, adds L2, runs bias-corrected Adam, updates the EMA, and clears the accumulator.

* **Fixed point, so deterministic.** D3D12 has no portable float atomic add. Each per-record,
  per-weight gradient becomes `int(clamp(g, -gradClamp, gradClamp) * gradFixedScale)`, and integers
  add associatively, so the accumulator holds the same bits whatever the record order, grouping or
  atomic order. Training is deterministic run to run on a device. The CPU test asserts this
  directly.
* **Overflow headroom.** One record contributes at most `gradClamp * gradFixedScale` = 2^20 at the
  defaults (16 x 65536). A group of 64 sums at most 2^26. The batch total is what can overflow:
  worst case (every record saturating, same sign) is safe only up to 2047 records at the defaults.
  Real gradients sit far below the clamp, so large batches are normally fine, but an atomic add
  wraps instead of saturating, so `recordTrain` warns once above `safeBatchLimit()`. Halving
  `gradFixedScale` doubles the headroom and costs resolution on tiny gradients.
* **fp32 only.** No `min16float`, no `-enable-16bit-types` in v1.

## The CPU reference

`MlpReference` implements exactly the same maths on the CPU: init, forward, both losses, backward,
the quantisation, Adam and the EMA. It is the spec for the HLSL, and the two are kept in step (each
says so at the top). "Exactly" has an asterisk: the GPU fuses multiply-adds and has its own `exp`,
so forward values agree to a few ulp, not bit for bit. `tests/render.neural` (`NeuralMlpTest`, no GPU)
checks finite-difference gradients for every activation and both losses, that a toy function is
learned, Adam's first step, init determinism, the weight file, and order-independent accumulation.

Weight layout, one flat fp32 array, layer by layer: `W[l]` as `out x in` row-major by output, then
`b[l]` (when `bias`). He-uniform init `(2u - 1) * sqrt(6 / fan_in)` with `u` from a documented PCG
hash of `(seed, index)`; biases zero.

### Weight file

Little-endian: `u32 'AVNN'` (0x4E4E5641), `u32 version = 1`, then `inputs, outputs, hiddenWidth,
hiddenLayers, hidden, output, bias, weightCount` as u32 (the `MlpDesc` minus its seed), then
`weightCount` fp32 values. Readers reject a wrong magic or version, an invalid shape, a count that
disagrees with the shape, and any size mismatch. `saveWeights` writes the EMA weights by default.

## Convolution layers (CPU reference, AVNN v2)

`ConvNetReference` (`ConvNetReference.hpp`, `NeuralOptimiser.hpp`, `WeightFile.hpp`) is the CPU side of
convolution support: the **spec the GPU kernels follow** (`shaders/aver_neural_conv.hlsl`, below)
and what `tests/render.neural` (`NeuralConvTest`, no GPU) checks. The contract is the header comment in
`ConvNetReference.hpp`; the short version:

* `ConvLayerDesc {cin, cout, kernel 1|3, stride 1|2, act None|ReLU, bias}`, `ConvNetDesc` (at most 8
  layers), NCHW tensors, `oh = ceil(h / stride)`, zero padding (3x3 reads `oy*stride - 1 + ky`, PyTorch
  pad = 1). Channels are multiples of 4 and at most 64 (the input may be any 1..64).
* Weights are one flat f32 array, layer by layer: `W[co][ci][ky][kx]` (OIHW) then `b[co]`.
* Fixed summation order: `acc = bias; for ci { for ky { for kx { acc += w * x } } }` in fp32, so the CPU
  twin and every GPU agree to FMA-contraction level (~1e-5 relative).
* He-uniform init from the same PCG hash as the MLP; the head layer is zero-initialised.
* Loss is weighted L2 with a per-position weight shared across channels, normalised by an explicit
  `lossNorm`. Gradients accumulate in **GPU order**: per (record, 8x8 output tile, weight) partials, then
  a fixed-order sum, quantised once into the shared fixed-point accumulator (no atomics, bit-deterministic
  per device). Conv defaults: `gradFixedScale` 2^24, `gradClamp` 32 (`convDefaults()`).
* `NeuralOptimiser` holds what the MLP and the conv net share: `OptimiserDesc`, `Activation`, `Loss`,
  `initHash`, `quantise` and the Adam step (`adamStep`, which `MlpReference::adamStep` calls).
* Training records are patches (56x56 half-res, tile-aligned); a patch's core tiles are bit-identical to a
  full-frame forward over a larger frame, which `NeuralConvTest` asserts.

**AVNN v2** (`WeightFile.hpp`) is the conv net's file: kind 1, a layer list, an optional input/output
standardisation affine, the weights, and a trailing CRC-32. `peekWeightFile` tells v1 (MLP) from v2;
`saveWeightFile` / `loadWeightFile` still read and write v1 only. Layout is documented in the header.

### Portability rules (every kernel in this module)

* fp32 arithmetic only; fp16 only as packed storage. No wave intrinsics, no float or int atomics in the
  conv path, `numthreads(64,1,1)`, every barrier in compile-time-constant control flow, out-of-range
  threads compute on zeros (never early-return).
* Groupshared at most 16 KB per kernel (the Vulkan minimum); `validate` checks the conv weight block
  (`convSharedBytes`).
* No transcendentals inside the network (ReLU or None only); ReLU is written `z > 0 ? z : 0` on GPU and CPU.
* Tensors are `StructuredBuffer<float>`, NCHW, one buffer per tensor, explicit bounds checks.
* Layer-streaming kernels, never fused (see Kernel structure above).
* Constants are a root CBV at **b3**. Not b1: the Vulkan backend always folds b1 (the per-object block)
  into push constants, so a b1 CBV read zeros there (record count 0, no effect); b0/b2/b4 are the
  engine's frame, draw and feature blocks.

### Conv on the GPU (`ConvNet`)

```cpp
#include <aver/render/neural/ConvNet.hpp>
ConvNet net;
net.create(device, desc, convDefaults(), ConvMode::Train);   // Infer compiles the forward kernels only
const TensorShape shapes[] = {{32, 12, 56, 56}, {1, 12, 497, 883}};
net.reserve(shapes);                                          // network-owned tensors, max over the list
net.recordInfer(ctx, in, out, {1, 12, 497, 883});             // EMA weights by default
net.recordTrain(ctx, in, target, posWeight, {32, 12, 56, 56}, lossNorm);
net.recordEvaluate(ctx, in, target, posWeight, lossPerRecord, shape, lossNorm);   // n floats
```

Buffers are `StructuredBuffer<float>` NCHW, one per tensor. The caller owns the input, output, targets,
per-position weights and the per-record loss; the network owns weights, EMA, Adam m / v, the integer
accumulator, one activation and one gradient buffer per layer, and the gradient partials. `reserve` must
see every shape before it is recorded (a shape it has not seen fails with a warning) and reallocates only
with no recorded work pending. Weights, `uploadWeights`, `recordReadback` / `collectWeights`,
`setLearningRate`, `invalidateBindings` and the binding-set cache behave as in `Mlp`;
`saveWeights` / `loadWeights` use AVNN v2 and carry the `ConvIoAffine` (`ioAffine()` / `setIoAffine`).

Kernels, `shaders/aver_neural_conv.hlsl`, one compile per layer per kernel (shape as DXC defines, spatial
sizes and offsets in the b3 block; slots t0..t5, u0..u6):

| Kernel | Threads | Does |
|---|---|---|
| `CSConvForward` | 8x8 outputs x 8 output channels per group | bias, then ci / ky / kx ascending; weight block (8 x min(cin, 16) x k*k) through groupshared per input chunk, input straight from global memory |
| `CSConvLossL2` | one per head element | `dY = (2 * pw * (p - t)) / lossNorm` |
| `CSConvActBackward` | one per element | in place `dz = y > 0 ? dy : 0` (ReLU layers only) |
| `CSConvBackwardData` | 8x8 inputs x 8 input channels per group | dX gather form, co / ky / kx ascending, invalid taps skipped |
| `CSConvBackwardWeights` | one per (record, 8x8 output tile, weight) | row-major tile sum of `dz * x` (bias: `dz`) into partials `[p][w]`, `p = (n * tilesY + ty) * tilesX + tx` |
| `CSConvReduceGrad` | one per weight | partials summed `p` ascending in fp32, quantised once, plain store into the accumulator |
| `CSConvEvalReduce` | one group per record | strided per-thread sums of `pw * (p - t)^2`, fixed 64 -> 1 tree, `/ lossNorm` |
| `CSAdam`, `CSResetState` | one per weight | the common bodies, `liveCount = 1` |

A training step is forward (every layer, master weights), loss, then per layer last to first: activation
backward, weight partials, reduce, and dX for the layer below; then one Adam dispatch. Every dispatch is
one layer (layer-streaming, as above); activations and gradients go through global memory, and each
dispatch's resource transitions are the barrier to the next. No atomics: every accumulator slot has one
writer, so training is bit-deterministic per device. Each weight's gradient is clamped and quantised once
after the fp32 sum, so the accumulator cannot overflow at any batch size (32 x 2^24 < 2^31).

### GPU parity

`NeuralGpuParityTest` (tests/render.neural) runs the kernels on a device through
`IDevice::runStandaloneCompute` and compares with the CPU twins: `NeuralGpuParityTest` (WARP),
`hw` (the adapter), `vulkan` (also a ctest `.vulkan` row), `debug` (validation layer). MLP section:
inference, one training step, rerun bit-identical. Conv section, on the NRD2 net (12 -> 16 -> 16 -> 32 ->
32 -> 12) at 2x12x41x57 with random non-zero weights: each layer's forward and the whole net (rel 1e-5 /
abs 1e-6), `recordEvaluate` vs `evaluate`, master and EMA weights after 1 and 10 steps vs `trainBatch`
(rel 1e-4, abs 2e-5), forward and a 10-step training rerun bit-identical, and a toy task learned on the GPU
(held-out loss 1.55 -> 0.00026 in 150 steps, the CPU twin landing on the same value). Passing on D3D12
WARP, D3D12 and Vulkan on an RX 7800 XT, with the D3D12 debug layer and the Vulkan validation layer
clean (2026-10-05). Measured on the RX 7800 XT: forward max abs error 8.3e-7 end to end; weights after 10
steps within 1.1e-6 of the CPU. WARP matches the CPU forward exactly (no fused multiply-add).

**Vulkan, dispatch count per submission.** The Vulkan backend allocates two constants descriptor sets per
dispatch (pipeline bind and `setConstantBuffer`) from a pool budgeted at 512 dynamic-UBO descriptors and
frees them after the submission retires. A conv training step is ~25 dispatches, so the test submits one
step at a time; batching several steps into one frame on Vulkan needs that budget raised in the backend.

## The readback gap

The RHI gives a module no fence, so there is no point at which `Mlp` can know the GPU has finished and
read trained weights back on its own. `saveWeights` therefore writes the last **CPU-known** copy
(the init, the last upload or load, or the last `collectWeights()`), not the live GPU state. To save
trained weights: call `recordReadback(ctx)`, let the engine drain the GPU (or wait enough frames),
call `collectWeights()`, then `saveWeights()`.

Related limits of the same kind: weight uploads go through a ring of three staging buffers written
immediately and unsynchronised, and binding sets are cached per buffer tuple (16 per kernel, then
recycled with a warning). Both are safe for the patterns this module expects and documented where
they could bite.

## Not there yet

* **fp16.** Packed fp16 kernels, gated on a native-16-bit-ops caps query (`OPTIONS4`) and SM 6.2/6.4
  in the probed shader-model list.
* **Cooperative matrix / LinAlg backends.** Vulkan `VK_KHR_cooperative_matrix` (RDNA3 WMMA, NVIDIA
  tensor cores) after Vulkan parity; D3D12 LinAlg (SM 6.10) when retail. RDNA3 has no D3D12 matrix
  path.
* **Many small networks per dispatch** (`instances > 1` in the design sketch), and a GPU-side loss
  readout.
* **int8**: deliberately never (design doc section 2).
* **Conv performance pass** (M4): `CSConvBackwardWeights` reads straight from global memory, one thread
  per (tile, weight); no input-tile staging (that waits for a counsel note); no bench numbers yet.

## How the radiance cache and frame interpolation will use it

* **Radiance cache (design doc stages 1 and 2):** stage 1 has no network. Stage 2 trains tiny networks
  that learn blend weights and residuals over the non-neural cache: a GPU pass writes records and a
  live count, `recordInfer` answers them (EMA weights, so the cache does not flicker), and
  `recordTrain` runs each frame on a batch the sampling pass produced, with the count again
  GPU-written. Counsel should review stage 2 before it ships (US 11,610,360).
* **Frame interpolation:** weights are trained offline and shipped, which is what the AVNN file is
  for (`loadWeights`). Its network adds convolution layers on top of this module's per-layer weight
  streaming; conv nets use the AVNN v2 file (version bump done).
