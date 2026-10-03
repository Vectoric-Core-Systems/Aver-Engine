# Code notes: render.neural

Design decisions, measurements and history that used to live in code comments. Moved here in the
2026-10-03 comment strip so the code stays readable. Each section names the source file; symbols in
backticks are where the knowledge applies. Measurements are as originally recorded and may be stale.


## modules/render.neural/include/aver/render/neural/Mlp.hpp

- **CpuCount reason**: Distinct type (not bare u32) because BufferHandle is itself u32. Without it, `recordInfer(ctx, rec, out, 128u, true)` would be ambiguous between "128 is a count" vs "128 is a buffer handle".

- **Staging buffer frame-in-flight safety**: Staging is a ring of kStagingRing buffers. Upload is safe unless kStagingRing more uploads are queued while the first is still executing. That pattern is not produced by this code, so it's safe.

- **Weight synchronization**: saveWeights writes CPU-known copy only (initial init, last upload/loadWeights, or last collectWeights), NOT live GPU state. RHI gives module no fence so no safe moment to read GPU weights back independently. collectWeights waits for GPU with waitIdle.

- **Readback mechanism**: recordReadback copies GPU weights; collectWeights reads them after GPU finishes. This is the only safe async weight transfer pattern when RHI provides no synchronization primitives to the module.

## modules/render.neural/include/aver/render/neural/MlpReference.hpp

- **File purpose**: This header is both the CPU reference implementation and the specification that the GPU shader `shaders/aver_neural_mlp.hlsl` must match exactly. GPU path cannot be unit-tested headless (suite has no device), so the CPU path is where every property is actually checked: finite-difference gradients, toy function learning, order-independent accumulation, file format. When one changes, the other changes in the same commit. Drift between them means tests verify a network the engine does not run.

- **Float vs integer exactness**: Float forward values agree to a few ULP, not bit-for-bit. This is due to GPU mad fusion and exp() differences. The integer half — the quantised gradient accumulator — is where bit-exactness is promised. It holds as long as the float gradient each record produces is the same float on both sides; a one-ULP difference can move a value across a quantisation step and change one accumulator count by 1. That is noise at the 1e-5 level and why tests assert order-independence on CPU (a property of integer addition) rather than CPU == GPU equality.

- **Accumulator wraparound**: Accumulator WRAPS on overflow (atomic add cannot saturate). A wrapped weight gradient is a silent wrong answer. The safeBatchLimit function floor is: (2^31 - 1) / (gradClamp * gradFixedScale). With defaults (gradClamp=16, gradFixedScale=65536), this is 2047 — a floor not a typical limit.

- **RelativeL2 loss math**: RelativeL2 is NRC's loss formula; stopgrad means the denominator (p^2 + 0.01) is treated as constant when differentiating.

- **Weight file format**: Binary, little-endian. Magic 'AVNN' (0x4E4E5641), version 1, then MlpDesc fields minus seed (init only), then weightCount, then f32 weights array.

## modules/render.neural/shaders/aver_neural_mlp.hlsl

- **Kernel architecture patent distance**: The MLP kernel deliberately uses a layer-by-layer streaming design through groupshared memory, not the fused fully-connected shape of register-resident weights with activations passed through shared memory (NVIDIA US 11,631,210 / 11,935,179). This design choice trades more groupshared traffic per layer for register efficiency and patent distance. The cost is acceptable at cache scale where memory traffic dominates. Changing toward the fused shape requires licensing review.

- **Groupshared aliasing rationale**: The single groupshared array (`gScratch`) is aliased between a float tile (holding current layer weights for forward and backward) and an int grid (used for gradient reduction). This aliasing keeps the group at ~16.6 KB. As two separate arrays the group would be ~33 KB, exceeding the 32 KB groupshared limit with no room.

- **Early-exit barrier discipline**: Inactive threads (records beyond liveCount) stay in loops computing zeros rather than returning early, because an early exit would leave GroupMemoryBarrierWithGroupSync short a thread. This constraint applies to all barriers.

- **Overflow headroom in gradient accumulation**: One record contributes at most `gradClamp * gradFixedScale` (2^20 at defaults: 16 * 65536). A group sums 64 records (at most 2^26, inside int32). The whole-batch accumulator can overflow: worst case is `maxCount * 2^20` bits, which fits int32 only up to 2047 records at defaults (MlpReference.hpp `safeBatchLimit`). Real gradients sit far below the clamp so 10^5-record batches are ordinarily fine. The atomic add wraps rather than saturates, so Mlp warns when batch exceeds the limit; the knob is `gradFixedScale` (halve it to double headroom at cost of resolution for tiny gradients).

- **Deterministic training order-independence**: Integer addition is associative/commutative (wraps), so the accumulator holds the same bits regardless of record grouping, scheduling, or atomic order. Training is therefore deterministic per-device run without ordering discipline in this file. Floating-point sums would not have this property.
