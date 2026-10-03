// aver_neural_mlp.hlsl -- fully connected network: inference, training (gradient + Adam + EMA) and state reset
//
// KEEP IN SYNC WITH MlpReference.hpp/cpp: same weight layout, init, forward, losses, backward, quantisation, Adam, EMA.
//
// ONE COMPILE PER NETWORK. Shape arrives as DXC defines, loop bounds are compile-time constants (Mlp::create):
//   AVER_NN_IN          input floats per record                      [1, 64]
//   AVER_NN_OUT         output floats per record                     [1, 16]
//   AVER_NN_WIDTH       hidden width, multiple of 4                  [4, 64]
//   AVER_NN_LAYERS      hidden layer count                           [1, 6]
//   AVER_NN_HIDDEN_ACT  0 none, 1 relu, 2 sigmoid, 3 exp
//   AVER_NN_OUT_ACT     same numbering
//   AVER_NN_BIAS        0 / 1
//   AVER_NN_LOSS        0 L2, 1 RelativeL2 ((y-p)^2 / (stopgrad(p)^2 + 0.01), per channel)
//
// ENTRY POINTS [numthreads(64,1,1)]:
//   CSInfer      one thread per RECORD: records -> outputs
//   CSTrainGrad  one thread per RECORD: forward, backward, accumulate quantised weight gradients
//   CSAdam       one thread per WEIGHT: accumulator -> Adam step -> EMA -> clear
//   CSResetState one thread per WEIGHT: zero Adam moments and accumulator (post-upload and at creation)
//
// NO WAVE INTRINSICS, NO 32/64-LANE ASSUMPTION. Thread group is the synchronization boundary (64 threads,
// GroupMemoryBarrierWithGroupSync), so AMD wave32/64, NVIDIA 32, Intel 8-32 compute the same. Every barrier
// sits in control flow depending only on compile-time constants: inactive threads stay in loops (computing on
// zeros) rather than returning early, to keep the barrier full.
//
// FP32 ONLY. No min16float, no -enable-16bit-types (v1 is the portable baseline; packed fp16 behind device caps).

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

// Layer l maps layerIn(l) -> layerOut(l); weight layers = hidden layers + 1.
#define NN_LAYER_COUNT (AVER_NN_LAYERS + 1)
#define NN_MAX2(a, b) ((a) > (b) ? (a) : (b))
#define NN_DELTA_MAX NN_MAX2(AVER_NN_WIDTH, AVER_NN_OUT)
// One record's activations: input, then each layer's output.
#define NN_ACT_TOTAL (AVER_NN_IN + AVER_NN_LAYERS * AVER_NN_WIDTH + AVER_NN_OUT)
// Floats in the biggest layer slice (W + bias): what the weight tile must hold.
#define NN_BIAS_N(n) (AVER_NN_BIAS ? (n) : 0)
#define NN_TILE_FIRST (AVER_NN_IN * AVER_NN_WIDTH + NN_BIAS_N(AVER_NN_WIDTH))
#define NN_TILE_MID   (AVER_NN_WIDTH * AVER_NN_WIDTH + NN_BIAS_N(AVER_NN_WIDTH))
#define NN_TILE_LAST  (AVER_NN_WIDTH * AVER_NN_OUT + NN_BIAS_N(AVER_NN_OUT))
// Gradient reduction: 64 rows (one per weight chunk), 64 per-record values, padded to 65 for bank-conflict free transpose.
#define NN_REDUCE_N (AVER_NN_GROUP * (AVER_NN_GROUP + 1))
#define NN_SCRATCH_N NN_MAX2(NN_MAX2(NN_TILE_FIRST, NN_TILE_MID), NN_MAX2(NN_TILE_LAST, NN_REDUCE_N))

// ONE groupshared array, aliased between float tile (forward/backward) and int grid (gradient reduction).
// Aliasing keeps group at ~16.6 KB, under 32 KB limit; as two arrays would be ~33 KB. Stored as uint, reinterpreted with asfloat/asint.
groupshared uint gScratch[NN_SCRATCH_N];

// Bindings: every pipeline shares one layout; unused slots are null.
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

// Root CBV, slot 0 is the engine's per-frame block. Matches Mlp.cpp's Constants.
cbuffer AverNeuralCB : register(b1) {
    uint  gCount;         // live record count when no count buffer is bound
    uint  gMaxCount;      // upper bound on the live count
    uint  gUseCountBuf;   // 1: count = gCountBuf[0]; 0: count = gCount
    uint  gUseEma;        // inference: read the EMA weights
    uint  gWeightCount;
    float gLr;
    float gBeta1;
    float gBeta2;
    float gEps;
    float gBc1;           // 1 - beta1^t (from CPU adamBiasCorrection)
    float gBc2;           // 1 - beta2^t
    float gEmaDecay;
    float gL2;
    float gGradScale;     // gradFixedScale
    float gGradClamp;
    float gPad0;
};

// ---- layer geometry: MlpLayout::make's twin ----
uint layerIn(uint l)   { return l == 0 ? AVER_NN_IN : AVER_NN_WIDTH; }
uint layerOut(uint l)  { return l == AVER_NN_LAYERS ? AVER_NN_OUT : AVER_NN_WIDTH; }
uint layerSize(uint l) { return layerOut(l) * layerIn(l) + NN_BIAS_N(layerOut(l)); }
uint layerBase(uint l) {
    uint s = 0;
    [unroll] for (uint k = 0; k < NN_LAYER_COUNT; ++k) { if (k < l) s += layerSize(k); }
    return s;
}
// Activation vector: array 0 is input, array l+1 is layer l's output.
uint actOff(uint l) { return l == 0 ? 0 : AVER_NN_IN + (l - 1) * AVER_NN_WIDTH; }

// ---- scalar maths (twins of MlpReference.cpp functions; derivatives take post-activation value) ----
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
    return 2.0 * (p - y) / (p * p + 0.01);   // denominator is stopgrad: constant when differentiating
#else
    return 2.0 * (p - y);
#endif
}

// Quantise to fixed-point: clamp(g, -gradClamp, gradClamp) * gradFixedScale truncated to int, NaN -> 0.
int quantise(float g) {
    if (!(g == g)) g = 0.0;
    return (int)(clamp(g, -gGradClamp, gGradClamp) * gGradScale);
}

uint liveCount() {
    uint n = gUseCountBuf != 0 ? gCountBuf[0] : gCount;
    return min(n, gMaxCount);
}

float fetchWeight(uint idx, uint useEma) { return useEma != 0 ? gEma[idx] : gWeights[idx]; }

// ---- weight tile: cooperative load of one layer's weights into groupshared ----
// Leading barrier protects the previous layer's readers; trailing one publishes this load. Both are
// reached by all threads (active record or not).
void streamLayerTile(uint layer, uint tid, uint useEma) {
    const uint base = layerBase(layer);
    const uint size = layerSize(layer);
    GroupMemoryBarrierWithGroupSync();
    for (uint k = tid; k < size; k += AVER_NN_GROUP) gScratch[k] = asuint(fetchWeight(base + k, useEma));
    GroupMemoryBarrierWithGroupSync();
}

// Forward pass, layer by layer. `a` holds input on entry, post-activation outputs on return. z = bias, then += w * a.
// Tile layout is flat: W[o * in + i], then output biases (twin of forwardImpl).
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

// ---- CSInfer ----
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

// ---- CSTrainGrad ----
// Per record: forward on MASTER weights, loss gradient, backward layer by layer, and group-wide reduction of
// quantised per-weight gradients into ONE InterlockedAdd per weight per GROUP (not per record).
//
// ORDER-INDEPENDENCE. All summed values are int; integer addition is associative/commutative (wraps, does not
// round), so accumulator holds same bits regardless of grouping, scheduling, or atomic order. Training is thus
// deterministic per-device run (floating-point sums would not be).
//
// OVERFLOW HEADROOM. One record contributes at most gradClamp * gradFixedScale per weight: 2^20 at defaults
// (16 * 65536). Group sums 64 records: at most 2^26, inside int32. Whole-batch accumulator can overflow: worst
// case (all records saturating same sign) is maxCount * 2^20, fits int32 only to 2047 records at defaults
// (MlpReference.hpp safeBatchLimit). Real gradients sit far below clamp, so 10^5-record batches usually fine.
// Atomic add wraps, so Mlp warns if batch exceeds limit; knob is gradFixedScale (halve to double headroom).

// Sums, over 64 records, the quantised gradient of each weight of layer `l`, adds totals to global accumulator.
// Chunks of 64 weights: each thread writes its record's value into a grid row, group syncs, then thread t sums
// row t -- transpose through groupshared that turns 64-way reduction per weight into 64 independent sums.
void reduceLayerGradient(uint l, uint tid, bool active, float delta[NN_DELTA_MAX],
                         float a[NN_ACT_TOTAL]) {
    const uint base = layerBase(l);
    const uint size = layerSize(l);
    const uint nin  = layerIn(l);
    const uint wcount = layerOut(l) * nin;   // weights ahead of the biases
    const uint inOff = actOff(l);

    GroupMemoryBarrierWithGroupSync();   // all threads done reading float tile
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
        GroupMemoryBarrierWithGroupSync();   // grid rewritten by next chunk
    }
}

[numthreads(AVER_NN_GROUP, 1, 1)]
void CSTrainGrad(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    const uint tid = gtid.x;
    const uint rec = gid.x * AVER_NN_GROUP + tid;
    const bool active = rec < liveCount();

    float a[NN_ACT_TOTAL];
    [unroll] for (uint i = 0; i < AVER_NN_IN; ++i) a[i] = active ? gRecords[rec * AVER_NN_IN + i] : 0.0;
    forwardRecord(tid, 0, a);   // training always on master weights, never EMA

    // Output delta = dLoss/dPrediction * act'(prediction). Inactive thread carries zeros.
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
            // Delta for layer below: W^T delta, scaled by hidden activation derivative.
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

// ---- CSAdam ----
// One thread per weight: accumulator -> mean over live count -> clamp -> L2 -> Adam with bias correction -> EMA -> clear.
// Accumulator is cleared even at zero count (next batch starts clean). Zero count leaves weights, moments, EMA alone.
[numthreads(AVER_NN_GROUP, 1, 1)]
void CSAdam(uint3 dtid : SV_DispatchThreadID) {
    const uint k = dtid.x;
    if (k >= gWeightCount) return;   // no barriers: early exit is safe
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

// ---- CSResetState ----
[numthreads(AVER_NN_GROUP, 1, 1)]
void CSResetState(uint3 dtid : SV_DispatchThreadID) {
    const uint k = dtid.x;
    if (k >= gWeightCount) return;
    gGrad[k] = 0;
    gM[k] = 0.0;
    gV[k] = 0.0;
}
