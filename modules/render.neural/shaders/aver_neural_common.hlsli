// aver_neural_common.hlsli -- shared by every neural kernel: the constant block, activations,
// quantisation, and the Adam / reset bodies.
//
// KEEP IN SYNC WITH NeuralOptimiser.hpp/.cpp (activate, quantise, adamStep).
// Portability rules (fp32 only, no wave intrinsics, 64 threads, constant-flow barriers): README.md.

#ifndef AVER_NEURAL_COMMON_HLSLI
#define AVER_NEURAL_COMMON_HLSLI

#define AVER_NN_GROUP 64

// Bindings every kernel family shares (slots fixed: the C++ side binds them the same way).
StructuredBuffer<uint>    gCountBuf  : register(t2);   // element 0 = live record count (GPU-written)
RWStructuredBuffer<int>   gGrad      : register(u1);   // fixed-point gradient accumulator
RWStructuredBuffer<float> gWeightsRW : register(u2);
RWStructuredBuffer<float> gEmaRW     : register(u3);
RWStructuredBuffer<float> gM         : register(u4);   // Adam first moment
RWStructuredBuffer<float> gV         : register(u5);   // Adam second moment

// A kernel family appends its own fields by defining AVER_NEURAL_CB_EXTRA before the include.
#ifndef AVER_NEURAL_CB_EXTRA
#define AVER_NEURAL_CB_EXTRA
#endif

// Root CBV at b3 (not b1: Vulkan folds b1 into push constants). First 64 bytes = Mlp.cpp's Constants;
// ConvNet.cpp's Constants appends the conv fields.
cbuffer AverNeuralCB : register(b3) {
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
    AVER_NEURAL_CB_EXTRA
};

// ---- scalar maths (derivatives take the post-activation value) ----
// 0 none, 1 relu, 2 sigmoid, 3 exp. ReLU is `z > 0 ? z : 0` (NaN -> 0), as on the CPU.
float activate(uint act, float z) {
    if (act == 1) return z > 0.0 ? z : 0.0;
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

// Fixed point: clamp(g, -gradClamp, gradClamp) * gradFixedScale truncated to int, NaN -> 0.
int quantise(float g) {
    if (!(g == g)) g = 0.0;
    return (int)(clamp(g, -gGradClamp, gGradClamp) * gGradScale);
}

uint liveCount() {
    uint n = gUseCountBuf != 0 ? gCountBuf[0] : gCount;
    return min(n, gMaxCount);
}

// ---- Adam body: one thread per weight ----
// accumulator -> mean over live count -> clamp -> L2 -> bias-corrected Adam -> EMA -> clear.
// Cleared even at zero count; zero count leaves weights, moments and EMA alone.
void neuralAdam(uint k) {
    if (k >= gWeightCount) return;   // callers have no barriers: early exit is safe
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

// ---- reset body: zero Adam moments and the accumulator ----
void neuralResetState(uint k) {
    if (k >= gWeightCount) return;
    gGrad[k] = 0;
    gM[k] = 0.0;
    gV[k] = 0.0;
}

#endif   // AVER_NEURAL_COMMON_HLSLI
