// aver_neural_conv.hlsl -- convolution layers: forward, weighted-L2 loss, backward, ordered gradient
// reduction, evaluation, Adam.
//
// KEEP IN SYNC WITH ConvNetReference.hpp/cpp: its header comment is the spec (layout, padding, summation
// order, partial order). Portability rules: README.md (fp32 only, no wave intrinsics, no atomics, 64
// threads per group, barriers only in compile-time-constant control flow, explicit bounds checks).
//
// LAYER-STREAMING: one dispatch per layer, activations through global memory; groupshared holds one
// layer's weight block only. Never fuse layers (US 11,631,210 / 11,935,179; README).
//
// ONE COMPILE PER LAYER PER KERNEL. Shape arrives as DXC defines (ConvNet::create); spatial sizes, offsets
// and counts arrive in the constant block.
//   AVER_CONV_CIN, AVER_CONV_COUT     channels                       [1, 64], cout multiple of 4
//   AVER_CONV_K, AVER_CONV_STRIDE     1|3, 1|2
//   AVER_CONV_ACT                     0 none, 1 relu
//   AVER_CONV_BIAS                    0 / 1
//   AVER_CONV_CO_BLOCK                output channels per forward group (8)
//   AVER_CONV_CI_CHUNK                input channels staged per weight block (16)
//
// Bindings (all StructuredBuffer, one tensor each):
//   t0 gX   tensor read: layer input (forward, backward weights), y (act backward), dz (backward data),
//           prediction (loss, eval)
//   t1 gT   target (loss, eval) or dz (backward weights)
//   t2 count, u1..u5 accumulator / weights / EMA / m / v: aver_neural_common.hlsli
//   t3 gW, t4 gWEma, t5 gPosW (per-position loss weight, n * oh * ow)
//   u0 gY   tensor written: y, dY (loss), dz in place (act backward), dX, loss per record (eval)
//   u6 gPartials  [p][w] per-tile gradient partials, p = (n * tilesY + ty) * tilesX + tx

#ifndef AVER_CONV_CIN
#define AVER_CONV_CIN 4
#endif
#ifndef AVER_CONV_COUT
#define AVER_CONV_COUT 4
#endif
#ifndef AVER_CONV_K
#define AVER_CONV_K 3
#endif
#ifndef AVER_CONV_STRIDE
#define AVER_CONV_STRIDE 1
#endif
#ifndef AVER_CONV_ACT
#define AVER_CONV_ACT 1
#endif
#ifndef AVER_CONV_BIAS
#define AVER_CONV_BIAS 1
#endif
#ifndef AVER_CONV_CO_BLOCK
#define AVER_CONV_CO_BLOCK 8
#endif
#ifndef AVER_CONV_CI_CHUNK
#define AVER_CONV_CI_CHUNK 16
#endif

#define AVER_NEURAL_CB_EXTRA \
    uint  gN;            \
    uint  gInH;          \
    uint  gInW;          \
    uint  gOutH;         \
    uint  gOutW;         \
    uint  gTilesX;       \
    uint  gTilesY;       \
    uint  gElemCount;    \
    uint  gWOffset;      \
    uint  gBOffset;      \
    uint  gLayerSize;    \
    uint  gPartialCount; \
    float gLossNorm;     \
    uint  gGroupsX;      \
    uint  gPad1;         \
    uint  gPad2;

#include "aver_neural_common.hlsli"

#define CONV_KK  (AVER_CONV_K * AVER_CONV_K)
#define CONV_PAD (AVER_CONV_K == 3 ? 1 : 0)
#define CONV_MIN(a, b) ((a) < (b) ? (a) : (b))
#define CONV_MAX(a, b) ((a) > (b) ? (a) : (b))
#define CONV_TILE 8
// Forward block: CO_BLOCK outputs x CI_BLK inputs x k*k (convSharedBytes).
#define CONV_CI_BLK    CONV_MIN(AVER_CONV_CIN, AVER_CONV_CI_CHUNK)
#define CONV_CO_BLOCKS ((AVER_CONV_COUT + AVER_CONV_CO_BLOCK - 1) / AVER_CONV_CO_BLOCK)
#define CONV_FWD_N     (AVER_CONV_CO_BLOCK * CONV_CI_BLK * CONV_KK)
// Backward-data block: the roles swapped (CO_BLOCK input channels x CO_CHK output channels x k*k).
#define CONV_CO_CHK    CONV_MIN(AVER_CONV_COUT, AVER_CONV_CI_CHUNK)
#define CONV_CI_BLOCKS ((AVER_CONV_CIN + AVER_CONV_CO_BLOCK - 1) / AVER_CONV_CO_BLOCK)
#define CONV_BWD_N     (AVER_CONV_CO_BLOCK * CONV_CO_CHK * CONV_KK)
#define CONV_SHARED_N  CONV_MAX(CONV_MAX(CONV_FWD_N, CONV_BWD_N), AVER_NN_GROUP)

#if CONV_SHARED_N * 4 > 16384
#error "aver_neural_conv.hlsl: groupshared block exceeds 16 KB (the Vulkan minimum)"
#endif

// One array, aliased by the kernels (each uses it for one purpose).
groupshared float gShared[CONV_SHARED_N];

StructuredBuffer<float>   gX        : register(t0);
StructuredBuffer<float>   gT        : register(t1);
StructuredBuffer<float>   gW        : register(t3);
StructuredBuffer<float>   gWEma     : register(t4);
StructuredBuffer<float>   gPosW     : register(t5);
RWStructuredBuffer<float> gY        : register(u0);
RWStructuredBuffer<float> gPartials : register(u6);

float fetchWeight(uint idx) { return gUseEma != 0 ? gWEma[idx] : gW[idx]; }

// Bounds-checked reads (no reliance on robust buffer access).
float readX(uint idx, bool ok) {
    float v = 0.0;
    if (ok) v = gX[idx];
    return v;
}
float readT(uint idx, bool ok) {
    float v = 0.0;
    if (ok) v = gT[idx];
    return v;
}

// 1D kernels dispatch (gGroupsX, ceil(groups / gGroupsX)) groups of 64.
uint linearIndex(uint3 gid, uint tid) { return (gid.y * gGroupsX + gid.x) * AVER_NN_GROUP + tid; }

// ---- CSConvForward ----
// Group = 8x8 output positions x CO_BLOCK output channels of one record (z = n * coBlocks + block).
// Weight block streamed through groupshared per input-channel chunk; input read from global memory.
// Summation per output: bias, then ci / ky / kx ascending (the spec's canonical order).
[numthreads(CONV_TILE, CONV_TILE, 1)]
void CSConvForward(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    const uint tid = gtid.y * CONV_TILE + gtid.x;
    const uint ox = gid.x * CONV_TILE + gtid.x;
    const uint oy = gid.y * CONV_TILE + gtid.y;
    const uint n = gid.z / CONV_CO_BLOCKS;
    const uint coBase = (gid.z - n * CONV_CO_BLOCKS) * AVER_CONV_CO_BLOCK;
    const bool active = ox < gOutW && oy < gOutH && n < gN;

    float acc[AVER_CONV_CO_BLOCK];
    [unroll] for (uint j = 0; j < AVER_CONV_CO_BLOCK; ++j) {
        acc[j] = 0.0;
#if AVER_CONV_BIAS
        if (coBase + j < AVER_CONV_COUT) acc[j] = fetchWeight(gBOffset + coBase + j);
#endif
    }

    [loop] for (uint c0 = 0; c0 < AVER_CONV_CIN; c0 += CONV_CI_BLK) {
        GroupMemoryBarrierWithGroupSync();   // previous chunk's readers are done
        for (uint k = tid; k < CONV_FWD_N; k += AVER_NN_GROUP) {
            const uint j = k / (CONV_CI_BLK * CONV_KK);
            const uint r = k - j * (CONV_CI_BLK * CONV_KK);
            const uint cil = r / CONV_KK;
            const uint t = r - cil * CONV_KK;
            const uint co = coBase + j, ci = c0 + cil;
            float w = 0.0;
            if (co < AVER_CONV_COUT && ci < AVER_CONV_CIN)
                w = fetchWeight(gWOffset + (co * AVER_CONV_CIN + ci) * CONV_KK + t);
            gShared[k] = w;
        }
        GroupMemoryBarrierWithGroupSync();

        [loop] for (uint cil = 0; cil < CONV_CI_BLK; ++cil) {
            const uint ci = c0 + cil;
            const bool ciOk = ci < AVER_CONV_CIN;
            const uint plane = (n * AVER_CONV_CIN + ci) * gInH;
            [unroll] for (uint ky = 0; ky < AVER_CONV_K; ++ky) {
                const int iy = (int)(oy * AVER_CONV_STRIDE) - CONV_PAD + (int)ky;
                [unroll] for (uint kx = 0; kx < AVER_CONV_K; ++kx) {
                    const int ix = (int)(ox * AVER_CONV_STRIDE) - CONV_PAD + (int)kx;
                    const bool inside = active && ciOk && iy >= 0 && iy < (int)gInH && ix >= 0 && ix < (int)gInW;
                    const float x = readX((plane + (uint)iy) * gInW + (uint)ix, inside);
                    [unroll] for (uint j = 0; j < AVER_CONV_CO_BLOCK; ++j)
                        acc[j] += gShared[(j * CONV_CI_BLK + cil) * CONV_KK + ky * AVER_CONV_K + kx] * x;
                }
            }
        }
    }

    if (active) {
        [unroll] for (uint j = 0; j < AVER_CONV_CO_BLOCK; ++j) {
            const uint co = coBase + j;
            if (co < AVER_CONV_COUT)
                gY[((n * AVER_CONV_COUT + co) * gOutH + oy) * gOutW + ox] = activate(AVER_CONV_ACT, acc[j]);
        }
    }
}

// ---- CSConvLossL2 ----
// One thread per head output element: dY = (2 * pw * (p - t)) / lossNorm. Compiled with the head's defines.
[numthreads(AVER_NN_GROUP, 1, 1)]
void CSConvLossL2(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    const uint i = linearIndex(gid, gtid.x);
    if (i >= gElemCount) return;   // no barriers
    const uint plane = gOutH * gOutW;
    const uint n = i / (AVER_CONV_COUT * plane);
    const uint pos = i % plane;
    const float w = gPosW[n * plane + pos];
    gY[i] = (2.0 * w * (gX[i] - gT[i])) / gLossNorm;
}

// ---- CSConvActBackward ----
// In place: dz = y > 0 ? dy : 0 (ReLU layers only; for None, dz = dy and nothing runs).
[numthreads(AVER_NN_GROUP, 1, 1)]
void CSConvActBackward(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    const uint i = linearIndex(gid, gtid.x);
    if (i >= gElemCount) return;
    const float dy = gY[i];
    gY[i] = gX[i] > 0.0 ? dy : 0.0;
}

// ---- CSConvBackwardData ----
// dX for one layer (> 0), gather form. Group = 8x8 input positions x CO_BLOCK input channels of one record
// (z = n * ciBlocks + block). Per element: co / ky / kx ascending, invalid taps skipped (the spec's order).
[numthreads(CONV_TILE, CONV_TILE, 1)]
void CSConvBackwardData(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    const uint tid = gtid.y * CONV_TILE + gtid.x;
    const uint ix = gid.x * CONV_TILE + gtid.x;
    const uint iy = gid.y * CONV_TILE + gtid.y;
    const uint n = gid.z / CONV_CI_BLOCKS;
    const uint ciBase = (gid.z - n * CONV_CI_BLOCKS) * AVER_CONV_CO_BLOCK;
    const bool active = ix < gInW && iy < gInH && n < gN;

    float acc[AVER_CONV_CO_BLOCK];
    [unroll] for (uint j = 0; j < AVER_CONV_CO_BLOCK; ++j) acc[j] = 0.0;

    [loop] for (uint c0 = 0; c0 < AVER_CONV_COUT; c0 += CONV_CO_CHK) {
        GroupMemoryBarrierWithGroupSync();
        for (uint k = tid; k < CONV_BWD_N; k += AVER_NN_GROUP) {
            const uint j = k / (CONV_CO_CHK * CONV_KK);
            const uint r = k - j * (CONV_CO_CHK * CONV_KK);
            const uint col = r / CONV_KK;
            const uint t = r - col * CONV_KK;
            const uint ci = ciBase + j, co = c0 + col;
            float w = 0.0;
            if (co < AVER_CONV_COUT && ci < AVER_CONV_CIN)
                w = gW[gWOffset + (co * AVER_CONV_CIN + ci) * CONV_KK + t];
            gShared[k] = w;
        }
        GroupMemoryBarrierWithGroupSync();

        [loop] for (uint col = 0; col < CONV_CO_CHK; ++col) {
            const uint co = c0 + col;
            const bool coOk = co < AVER_CONV_COUT;
            const uint plane = (n * AVER_CONV_COUT + co) * gOutH;
            [unroll] for (uint ky = 0; ky < AVER_CONV_K; ++ky) {
                const int ty = (int)iy + CONV_PAD - (int)ky;
                [unroll] for (uint kx = 0; kx < AVER_CONV_K; ++kx) {
                    const int tx = (int)ix + CONV_PAD - (int)kx;
                    bool ok = active && coOk && ty >= 0 && tx >= 0;
#if AVER_CONV_STRIDE == 2
                    ok = ok && ((ty & 1) == 0) && ((tx & 1) == 0);
#endif
                    const uint oy = (uint)ty / AVER_CONV_STRIDE, ox = (uint)tx / AVER_CONV_STRIDE;
                    ok = ok && oy < gOutH && ox < gOutW;
                    if (ok) {
                        const float d = gX[(plane + oy) * gOutW + ox];
                        [unroll] for (uint j = 0; j < AVER_CONV_CO_BLOCK; ++j)
                            acc[j] += gShared[(j * CONV_CO_CHK + col) * CONV_KK + ky * AVER_CONV_K + kx] * d;
                    }
                }
            }
        }
    }

    if (active) {
        [unroll] for (uint j = 0; j < AVER_CONV_CO_BLOCK; ++j) {
            const uint ci = ciBase + j;
            if (ci < AVER_CONV_CIN) gY[((n * AVER_CONV_CIN + ci) * gInH + iy) * gInW + ix] = acc[j];
        }
    }
}

// ---- CSConvBackwardWeights ----
// One thread per (record z, 8x8 output tile y, weight of this layer's slice x*64 + t): the tile's sum of
// dz * x (bias: dz) in row-major order, clipped to the output. Stored [p][w]. No groupshared, no barriers.
[numthreads(AVER_NN_GROUP, 1, 1)]
void CSConvBackwardWeights(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    const uint w = gid.x * AVER_NN_GROUP + gtid.x;
    const uint tileIdx = gid.y, n = gid.z;
    if (w >= gLayerSize || n >= gN || tileIdx >= gTilesX * gTilesY) return;
    const uint ty = tileIdx / gTilesX, tx = tileIdx - ty * gTilesX;
    const uint wCount = AVER_CONV_COUT * AVER_CONV_CIN * CONV_KK;
    const uint plane = gOutH * gOutW;

    float part = 0.0;
    if (w < wCount) {
        const uint co = w / (AVER_CONV_CIN * CONV_KK);
        const uint r = w - co * (AVER_CONV_CIN * CONV_KK);
        const uint ci = r / CONV_KK;
        const uint t = r - ci * CONV_KK;
        const int ky = (int)(t / AVER_CONV_K), kx = (int)(t % AVER_CONV_K);
        const uint dzBase = (n * AVER_CONV_COUT + co) * plane;
        const uint xBase = (n * AVER_CONV_CIN + ci) * gInH;
        [loop] for (uint yy = 0; yy < CONV_TILE; ++yy) {
            const uint y = ty * CONV_TILE + yy;
            if (y >= gOutH) break;
            const int iy = (int)(y * AVER_CONV_STRIDE) - CONV_PAD + ky;
            [loop] for (uint xx = 0; xx < CONV_TILE; ++xx) {
                const uint x = tx * CONV_TILE + xx;
                if (x >= gOutW) break;
                const int ix = (int)(x * AVER_CONV_STRIDE) - CONV_PAD + kx;
                const bool inside = iy >= 0 && iy < (int)gInH && ix >= 0 && ix < (int)gInW;
                const float xv = readX((xBase + (uint)iy) * gInW + (uint)ix, inside);
                part += gT[dzBase + y * gOutW + x] * xv;
            }
        }
    } else {
        const uint dzBase = (n * AVER_CONV_COUT + (w - wCount)) * plane;
        [loop] for (uint yy = 0; yy < CONV_TILE; ++yy) {
            const uint y = ty * CONV_TILE + yy;
            if (y >= gOutH) break;
            [loop] for (uint xx = 0; xx < CONV_TILE; ++xx) {
                const uint x = tx * CONV_TILE + xx;
                if (x >= gOutW) break;
                part += gT[dzBase + y * gOutW + x];
            }
        }
    }
    const uint p = (n * gTilesY + ty) * gTilesX + tx;
    gPartials[p * gLayerSize + w] = part;
}

// ---- CSConvReduceGrad ----
// One thread per weight of the layer slice: sum partials p ascending (fp32), quantise ONCE, plain store
// into the accumulator at gWOffset + w (no atomics: every weight is written by exactly one thread).
[numthreads(AVER_NN_GROUP, 1, 1)]
void CSConvReduceGrad(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    const uint w = linearIndex(gid, gtid.x);
    if (w >= gLayerSize) return;
    float g = 0.0;
    [loop] for (uint p = 0; p < gPartialCount; ++p) g += gPartials[p * gLayerSize + w];
    gGrad[gWOffset + w] = quantise(g);
}

// ---- CSConvEvalReduce ----
// One group per record: thread t sums pw * (p - t)^2 over elements t, t + 64, ... of the record, then a
// fixed 64 -> 1 tree; writes sum / lossNorm. Compiled with the head's defines.
[numthreads(AVER_NN_GROUP, 1, 1)]
void CSConvEvalReduce(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    const uint tid = gtid.x;
    const uint n = gid.y * gGroupsX + gid.x;
    const bool active = n < gN;
    const uint plane = gOutH * gOutW;
    const uint per = AVER_CONV_COUT * plane;

    float s = 0.0;
    if (active) {
        [loop] for (uint i = tid; i < per; i += AVER_NN_GROUP) {
            const float w = gPosW[n * plane + i % plane];
            const float e = gX[n * per + i] - gT[n * per + i];
            s += w * e * e;
        }
    }
    gShared[tid] = s;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = AVER_NN_GROUP / 2; stride > 0; stride >>= 1) {
        if (tid < stride) gShared[tid] += gShared[tid + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if (active && tid == 0) gY[n] = gShared[0] / gLossNorm;
}

// ---- CSAdam / CSResetState: bodies in aver_neural_common.hlsli (liveCount 1: the loss is normalised) ----
[numthreads(AVER_NN_GROUP, 1, 1)]
void CSAdam(uint3 dtid : SV_DispatchThreadID) { neuralAdam(dtid.x); }

[numthreads(AVER_NN_GROUP, 1, 1)]
void CSResetState(uint3 dtid : SV_DispatchThreadID) { neuralResetState(dtid.x); }
