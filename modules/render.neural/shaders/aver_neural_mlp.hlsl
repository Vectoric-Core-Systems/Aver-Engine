// aver_neural_mlp.hlsl -- Aver.Render.Neural's fully connected network: inference, training
// (gradient accumulation + Adam + weight EMA) and the state reset, in portable fp32 HLSL.
//
// KEEP IN STEP WITH include/aver/render/neural/MlpReference.hpp / src/MlpReference.cpp. The CPU
// reference is the specification of this file: same weight layout, same init, same forward, same
// losses, same backward, same fixed-point quantisation, same Adam and EMA. Every function here
// names its CPU twin. A change to one is a change to the other, in the same commit.
//
// ONE COMPILE PER NETWORK. The shape arrives as DXC defines, so every network gets pipelines whose
// loop bounds are compile-time constants (Mlp::create passes them):
//   AVER_NN_IN          input floats per record                      [1, 64]
//   AVER_NN_OUT         output floats per record                     [1, 16]
//   AVER_NN_WIDTH       hidden width, a multiple of 4                [4, 64]
//   AVER_NN_LAYERS      hidden layer count                           [1, 6]
//   AVER_NN_HIDDEN_ACT  0 none, 1 relu, 2 sigmoid, 3 exp
//   AVER_NN_OUT_ACT     same numbering
//   AVER_NN_BIAS        0 / 1
//   AVER_NN_LOSS        0 L2, 1 RelativeL2 ((y-p)^2 / (stopgrad(p)^2 + 0.01), per channel)
//
// ENTRY POINTS, all [numthreads(64,1,1)]:
//   CSInfer      one thread per RECORD: records -> outputs
//   CSTrainGrad  one thread per RECORD: forward, backward, accumulate quantised weight gradients
//   CSAdam       one thread per WEIGHT: accumulator -> Adam step -> EMA -> clear the accumulator
//   CSResetState one thread per WEIGHT: zero Adam's moments and the accumulator (after a weight
//                upload, and once at creation -- a fresh buffer's contents are not promised)
//
// NO WAVE INTRINSICS, NO ASSUMPTION ABOUT 32 OR 64 LANES. Behaviour depends on the thread GROUP
// (64 threads, synchronised with GroupMemoryBarrierWithGroupSync) and on nothing finer, so AMD
// wave32/wave64, NVIDIA 32 and Intel 8-32 compute the same thing. Every barrier sits in
// control flow that depends only on compile-time constants: threads past the live record count
// stay in the loops (computing on zeros, contributing zeros) rather than returning early, because
// an early exit would leave the barrier short a thread.
//
// FP32 ONLY. No min16float, no -enable-16bit-types: v1 is the portable baseline (design doc
// docs/rendering/RADIANCE_CACHE.md section 7); packed fp16 comes behind a device caps query.
//
// ---------------------------------------------------------------------------------------------
// KERNEL STRUCTURE -- A DELIBERATE CHOICE, DO NOT "OPTIMISE" IT (docs/rendering/RADIANCE_CACHE.md
// section 9). The kernel STREAMS WEIGHTS LAYER BY LAYER THROUGH GROUPSHARED MEMORY: the thread
// group cooperatively loads ONE layer's weights into groupshared, syncs, every thread computes
// that layer for ITS OWN record with its activations held in a thread-local array, and only then
// is the next layer's slice loaded over the same memory. Records never exchange intermediates
// through shared memory, and the network's weights are never held resident in registers across
// layers. This is NOT the "all weights loaded once into registers, intermediates exchanged
// through shared memory" shape of a fully fused MLP kernel, and it is kept different on purpose
// for patent distance (NVIDIA US 11,631,210 / 11,935,179 describe that fused shape). Changing
// this file toward that shape -- register-resident weights, activations passed between threads
// through groupshared, tiles sized to a matrix unit -- needs a licensing review first, not just a
// benchmark. The cost of the chosen shape is more groupshared traffic per layer; at cache scale
// (design doc section 2) the maths is cheap and memory traffic is what matters, so that is the
// right trade.
// ---------------------------------------------------------------------------------------------

#ifndef AVER_NN_IN
#define AVER_NN_IN 4
#endif
#ifndef AVER_NN_OUT
#define AVER_NN_OUT 3
#endif
#ifndef AVER_NN_WIDTH
#define AVER_NN_WIDTH 32
#endif
#ifndef AVER_NN_LAYERS
#define AVER_NN_LAYERS 2
#endif
#ifndef AVER_NN_HIDDEN_ACT
#define AVER_NN_HIDDEN_ACT 1
#endif
#ifndef AVER_NN_OUT_ACT
#define AVER_NN_OUT_ACT 0
#endif
#ifndef AVER_NN_BIAS
#define AVER_NN_BIAS 1
#endif
#ifndef AVER_NN_LOSS
#define AVER_NN_LOSS 0
#endif

#define AVER_NN_GROUP 64

// Weight layers = hidden layers + 1. Layer l maps layerIn(l) -> layerOut(l); the last is l = AVER_NN_LAYERS.
#define NN_LAYER_COUNT (AVER_NN_LAYERS + 1)
#define NN_MAX2(a, b) ((a) > (b) ? (a) : (b))
// Widest delta vector a thread keeps (hidden width or the outputs, whichever is larger).
#define NN_DELTA_MAX NN_MAX2(AVER_NN_WIDTH, AVER_NN_OUT)
// One record's activations: the input, then every layer's output (MlpLayout::actTotal).
#define NN_ACT_TOTAL (AVER_NN_IN + AVER_NN_LAYERS * AVER_NN_WIDTH + AVER_NN_OUT)
// Floats in the biggest layer slice (W plus bias): what the weight tile must hold.
#define NN_BIAS_N(n) (AVER_NN_BIAS ? (n) : 0)
#define NN_TILE_FIRST (AVER_NN_IN * AVER_NN_WIDTH + NN_BIAS_N(AVER_NN_WIDTH))
#define NN_TILE_MID   (AVER_NN_WIDTH * AVER_NN_WIDTH + NN_BIAS_N(AVER_NN_WIDTH))
#define NN_TILE_LAST  (AVER_NN_WIDTH * AVER_NN_OUT + NN_BIAS_N(AVER_NN_OUT))
// The gradient reduction wants 64 rows (one per weight of a chunk) of 64 per-record values, each
// row padded to 65 so the transposed read is bank-conflict free: 64 * 65 ints.
#define NN_REDUCE_N (AVER_NN_GROUP * (AVER_NN_GROUP + 1))
#define NN_SCRATCH_N NN_MAX2(NN_MAX2(NN_TILE_FIRST, NN_TILE_MID), NN_MAX2(NN_TILE_LAST, NN_REDUCE_N))

// ONE groupshared array, used as two things at two different times and never both at once: a
// float tile holding the current layer's weights (forward, and the delta propagation of backward),
// and an int grid for the gradient reduction. Aliasing them keeps the group at ~16.6 KB, under
// the 32 KB groupshared limit with room to spare; as two arrays they would be ~33 KB and not fit.
// Stored as uint and reinterpreted with asfloat / asint.
groupshared uint gScratch[NN_SCRATCH_N];

// ---- bindings (every pipeline shares one layout; a pass leaves the slots it does not use null) ----
StructuredBuffer<float>   gRecords : register(t0);   // count * IN floats
StructuredBuffer<float>   gTargets : register(t1);   // count * OUT floats (training)
StructuredBuffer<uint>    gCountBuf: register(t2);   // element 0 = live record count (GPU-written)
StructuredBuffer<float>   gWeights : register(t3);   // master weights (read)
StructuredBuffer<float>   gEma     : register(t4);   // EMA weights (read)
RWStructuredBuffer<float> gOut     : register(u0);   // count * OUT floats
RWStructuredBuffer<int>   gGrad    : register(u1);   // fixed-point gradient accumulator
RWStructuredBuffer<float> gWeightsRW : register(u2);
RWStructuredBuffer<float> gEmaRW   : register(u3);
RWStructuredBuffer<float> gM       : register(u4);   // Adam first moment
RWStructuredBuffer<float> gV       : register(u5);   // Adam second moment

// b1: root CBV. Slot 0 is the engine's per-frame block. Mlp.cpp's Constants, field for field.
cbuffer AverNeuralCB : register(b1) {
    uint  gCount;         // live record count when no count buffer is bound
    uint  gMaxCount;      // upper bound on the live count (clamps a GPU-written count)
    uint  gUseCountBuf;   // 1: the count is gCountBuf[0]; 0: gCount
    uint  gUseEma;        // inference: read the EMA weights
    uint  gWeightCount;
    float gLr;
    float gBeta1;
    float gBeta2;
    float gEps;
    float gBc1;           // 1 - beta1^t, from the CPU (adamBiasCorrection) so both sides agree
    float gBc2;           // 1 - beta2^t
    float gEmaDecay;
    float gL2;
    float gGradScale;     // gradFixedScale
    float gGradClamp;
    float gPad0;
};

// ---------------------------------------------------------------- layer geometry
// MlpLayout::make's twin. Layer offsets are sums of constant slice sizes, so DXC folds them.
uint layerIn(uint l)   { return l == 0 ? AVER_NN_IN : AVER_NN_WIDTH; }
uint layerOut(uint l)  { return l == AVER_NN_LAYERS ? AVER_NN_OUT : AVER_NN_WIDTH; }
uint layerSize(uint l) { return layerOut(l) * layerIn(l) + NN_BIAS_N(layerOut(l)); }
uint layerBase(uint l) {
    uint s = 0;
    [unroll] for (uint k = 0; k < NN_LAYER_COUNT; ++k) { if (k < l) s += layerSize(k); }
    return s;
}
// Where array l of the activation vector starts: array 0 is the input, array l+1 layer l's output.
uint actOff(uint l) { return l == 0 ? 0 : AVER_NN_IN + (l - 1) * AVER_NN_WIDTH; }

// ---------------------------------------------------------------- scalar maths
// activate() / activationDerivative() / lossGrad() / quantise() are the twins of the same-named
// functions in MlpReference.cpp. Derivatives take the POST-activation value.
float activate(uint act, float z) {
    if (act == 1) return max(z, 0.0);
    if (act == 2) return 1.0 / (1.0 + exp(-z));
    if (act == 3) return exp(min(z, 20.0));   // kExpMaxArg
    return z;
}
float activationDerivative(uint act, float y) {
    if (act == 1) return y > 0.0 ? 1.0 : 0.0;
    if (act == 2) return y * (1.0 - y);
    if (act == 3) return y;
    return 1.0;
}
float lossGrad(float p, float y) {
#if AVER_NN_LOSS == 1
    return 2.0 * (p - y) / (p * p + 0.01);   // denominator is stopgrad: a constant when differentiating
#else
    return 2.0 * (p - y);
#endif
}

// THE FIXED-POINT STEP. int(clamp(g, -gradClamp, gradClamp) * gradFixedScale), truncating toward
// zero. A NaN gradient quantises to 0 rather than to an unspecified int.
int quantise(float g) {
    if (!(g == g)) g = 0.0;
    return (int)(clamp(g, -gGradClamp, gGradClamp) * gGradScale);
}

uint liveCount() {
    uint n = gUseCountBuf != 0 ? gCountBuf[0] : gCount;
    return min(n, gMaxCount);
}

float fetchWeight(uint idx, uint useEma) { return useEma != 0 ? gEma[idx] : gWeights[idx]; }

// ---------------------------------------------------------------- the weight tile
// Cooperatively copies one layer's slice of the flat weight array into groupshared. The leading
// barrier protects the PREVIOUS layer's readers; the trailing one publishes this load. Both are
// reached by every thread of the group, active record or not.
void streamLayerTile(uint layer, uint tid, uint useEma) {
    const uint base = layerBase(layer);
    const uint size = layerSize(layer);
    GroupMemoryBarrierWithGroupSync();
    for (uint k = tid; k < size; k += AVER_NN_GROUP) gScratch[k] = asuint(fetchWeight(base + k, useEma));
    GroupMemoryBarrierWithGroupSync();
}

// Forward pass of this thread's record, layer by layer. `a` holds the input in array 0 on entry and
// every layer's post-activation output on return. forwardImpl()'s twin; z = bias, then += w * a
// over ascending i. The tile layout is the flat layout: W[o * in + i], then the out biases.
void forwardRecord(uint tid, uint useEma, inout float a[NN_ACT_TOTAL]) {
    [loop] for (uint l = 0; l < NN_LAYER_COUNT; ++l) {
        streamLayerTile(l, tid, useEma);
        const uint nin  = layerIn(l);
        const uint nout = layerOut(l);
        const uint inOff  = actOff(l);
        const uint outOff = actOff(l + 1);
        const uint act = (l + 1 < NN_LAYER_COUNT) ? AVER_NN_HIDDEN_ACT : AVER_NN_OUT_ACT;
        [loop] for (uint o = 0; o < nout; ++o) {
            float z = 0.0;
#if AVER_NN_BIAS
            z = asfloat(gScratch[nout * nin + o]);
#endif
            [loop] for (uint i = 0; i < nin; ++i) z += asfloat(gScratch[o * nin + i]) * a[inOff + i];
            a[outOff + o] = activate(act, z);
        }
    }
}

// ---------------------------------------------------------------- CSInfer
[numthreads(AVER_NN_GROUP, 1, 1)]
void CSInfer(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    const uint tid = gtid.x;
    const uint rec = gid.x * AVER_NN_GROUP + tid;
    const bool active = rec < liveCount();

    float a[NN_ACT_TOTAL];
    [unroll] for (uint i = 0; i < AVER_NN_IN; ++i) a[i] = active ? gRecords[rec * AVER_NN_IN + i] : 0.0;
    forwardRecord(tid, gUseEma, a);

    if (active) {
        const uint outBase = actOff(NN_LAYER_COUNT);
        [unroll] for (uint o = 0; o < AVER_NN_OUT; ++o) gOut[rec * AVER_NN_OUT + o] = a[outBase + o];
    }
}

// ---------------------------------------------------------------- CSTrainGrad
// Per record: forward on the MASTER weights, loss gradient, backward layer by layer, and for each
// layer a group-wide reduction of the quantised per-weight gradients, ending in ONE InterlockedAdd
// per weight per GROUP -- not per record.
//
// ORDER-INDEPENDENCE. Every value that is summed is an int, and integer addition is associative and
// commutative (it wraps, it does not round), so the accumulator holds the same bits however the
// records are grouped, however the groups are scheduled, and whichever order the atomics land in.
// Training is therefore deterministic run to run on one device, without any ordering discipline in
// this file. (Floating-point sums would not be: their result depends on the order.)
//
// OVERFLOW HEADROOM. One record contributes at most gradClamp * gradFixedScale per weight: 2^20
// at the defaults (16 * 65536). A group sums 64 records: at most 2^26, comfortably inside int32.
// The accumulator across the whole batch is the part that can overflow: worst case (every record
// saturating the clamp, same sign) is maxCount * 2^20, which fits int32 only up to 2047 records at
// the defaults (MlpReference.hpp safeBatchLimit). That is a worst case -- real gradients sit far
// below the clamp, so 10^5-record batches are ordinarily fine -- but an atomic add wraps rather
// than saturates, so Mlp warns when a batch exceeds the limit and the knob is gradFixedScale
// (halve it to double the headroom, at the cost of resolution for tiny gradients).

// Sums, over the 64 records of the group, the quantised gradient of each weight of layer `l`, and
// adds each total to the global accumulator. Chunks of 64 weights: every thread writes its
// record's value for each of the chunk's 64 weights into row k of the grid, the group syncs, then
// thread t sums row t -- a transpose through groupshared that turns a 64-way reduction per weight
// into 64 independent sums, one per thread.
void reduceLayerGradient(uint l, uint tid, bool active, float delta[NN_DELTA_MAX],
                         float a[NN_ACT_TOTAL]) {
    const uint base = layerBase(l);
    const uint size = layerSize(l);
    const uint nin  = layerIn(l);
    const uint wcount = layerOut(l) * nin;   // weights ahead of the biases in the slice
    const uint inOff = actOff(l);

    GroupMemoryBarrierWithGroupSync();   // every thread is done reading the float tile
    [loop] for (uint chunk = 0; chunk < size; chunk += AVER_NN_GROUP) {
        [loop] for (uint k = 0; k < AVER_NN_GROUP; ++k) {
            const uint idx = chunk + k;
            int q = 0;
            if (active && idx < size) {
                float g;
                if (idx < wcount) {
                    const uint o = idx / nin;
                    g = delta[o] * a[inOff + (idx - o * nin)];
                } else {
                    g = delta[idx - wcount];
                }
                q = quantise(g);
            }
            gScratch[k * (AVER_NN_GROUP + 1) + tid] = asuint(q);
        }
        GroupMemoryBarrierWithGroupSync();
        int s = 0;
        [loop] for (uint t = 0; t < AVER_NN_GROUP; ++t) s += asint(gScratch[tid * (AVER_NN_GROUP + 1) + t]);
        const uint w = chunk + tid;
        if (w < size && s != 0) InterlockedAdd(gGrad[base + w], s);
        GroupMemoryBarrierWithGroupSync();   // the grid is rewritten by the next chunk
    }
}

[numthreads(AVER_NN_GROUP, 1, 1)]
void CSTrainGrad(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    const uint tid = gtid.x;
    const uint rec = gid.x * AVER_NN_GROUP + tid;
    const bool active = rec < liveCount();

    float a[NN_ACT_TOTAL];
    [unroll] for (uint i = 0; i < AVER_NN_IN; ++i) a[i] = active ? gRecords[rec * AVER_NN_IN + i] : 0.0;
    forwardRecord(tid, 0, a);   // training always runs on the master weights, never the EMA

    // Output delta = dLoss/dPrediction * act'(prediction). An inactive thread carries zeros.
    float delta[NN_DELTA_MAX];
    float prev[NN_DELTA_MAX];
    [unroll] for (uint z = 0; z < NN_DELTA_MAX; ++z) { delta[z] = 0.0; prev[z] = 0.0; }
    const uint outBase = actOff(NN_LAYER_COUNT);
    [unroll] for (uint o = 0; o < AVER_NN_OUT; ++o) {
        if (active) {
            const float p = a[outBase + o];
            delta[o] = lossGrad(p, gTargets[rec * AVER_NN_OUT + o]) * activationDerivative(AVER_NN_OUT_ACT, p);
        }
    }

    [loop] for (uint step = 0; step < NN_LAYER_COUNT; ++step) {
        const uint l = NN_LAYER_COUNT - 1 - step;
        const uint nin  = layerIn(l);
        const uint nout = layerOut(l);
        if (l > 0) {
            // delta for the layer below: W^T delta, scaled by the hidden activation's derivative
            // at that layer's output (array l of the activation vector).
            streamLayerTile(l, tid, 0);
            const uint inOff = actOff(l);
            [loop] for (uint i = 0; i < nin; ++i) {
                float s = 0.0;
                [loop] for (uint oo = 0; oo < nout; ++oo) s += asfloat(gScratch[oo * nin + i]) * delta[oo];
                prev[i] = s * activationDerivative(AVER_NN_HIDDEN_ACT, a[inOff + i]);
            }
        }
        reduceLayerGradient(l, tid, active, delta, a);
        if (l > 0) {
            [loop] for (uint i2 = 0; i2 < nin; ++i2) delta[i2] = prev[i2];
        }
    }
}

// ---------------------------------------------------------------- CSAdam
// One thread per weight, in the order MlpReference::adamStep documents: accumulator -> mean over
// the live count -> clamp -> L2 -> Adam with bias correction -> EMA -> clear the accumulator.
// The accumulator is cleared even when the count is zero, so the next batch starts from nothing.
// A zero live count leaves the weights, moments and EMA alone (the CPU counted the step already;
// at worst Adam's bias correction runs one step ahead of the updates actually applied).
[numthreads(AVER_NN_GROUP, 1, 1)]
void CSAdam(uint3 dtid : SV_DispatchThreadID) {
    const uint k = dtid.x;
    if (k >= gWeightCount) return;   // no barriers in this kernel: an early exit is safe
    const int acc = gGrad[k];
    gGrad[k] = 0;
    const uint n = liveCount();
    if (n == 0) return;

    float g = ((float)acc / gGradScale) / (float)n;
    g = clamp(g, -gGradClamp, gGradClamp);
    const float w = gWeightsRW[k];
    g += gL2 * w;
    const float m = gBeta1 * gM[k] + (1.0 - gBeta1) * g;
    const float v = gBeta2 * gV[k] + (1.0 - gBeta2) * g * g;
    gM[k] = m;
    gV[k] = v;
    const float mhat = m / gBc1;
    const float vhat = v / gBc2;
    const float wn = w - gLr * mhat / (sqrt(vhat) + gEps);
    gWeightsRW[k] = wn;
    gEmaRW[k] = gEmaDecay * gEmaRW[k] + (1.0 - gEmaDecay) * wn;
}

// ---------------------------------------------------------------- CSResetState
[numthreads(AVER_NN_GROUP, 1, 1)]
void CSResetState(uint3 dtid : SV_DispatchThreadID) {
    const uint k = dtid.x;
    if (k >= gWeightCount) return;
    gGrad[k] = 0;
    gM[k] = 0.0;
    gV[k] = 0.0;
}
