// aver_neural_conv.hlsl -- convolution layers: forward, weighted-L2 loss, backward, ordered gradient
// reduction, evaluation, Adam.
//
// KEEP IN SYNC WITH ConvNetReference.hpp/cpp: its header comment is the spec (layout, padding, summation
// order, partial order). Portability rules: README.md (fp32 only, no wave intrinsics, no atomics, 64
// threads per group, barriers only in compile-time-constant control flow, explicit bounds checks).
//
// LAYER-STREAMING: one dispatch per layer, activations through global memory; groupshared holds one
// layer's weight block (forward, backward data) or that layer's own dz / input tile (backward weights),
// never anything across layers. No input-tile staging in the forward (awaits a counsel note). Never fuse
// layers (US 11,631,210 / 11,935,179; README).
//
// ONE COMPILE PER LAYER PER KERNEL. Shape arrives as DXC defines (ConvNet::create); spatial sizes, offsets
// and counts arrive in the constant block.
//   AVER_CONV_CIN, AVER_CONV_COUT     channels                       [1, 64], cout multiple of 4
//   AVER_CONV_K, AVER_CONV_STRIDE     1|3, 1|2
//   AVER_CONV_ACT                     0 none, 1 relu
//   AVER_CONV_BIAS                    0 / 1
//   AVER_CONV_CO_BLOCK                channels per forward / backward-data group (8)
//   AVER_CONV_CI_CHUNK                channels per staged weight block (16)
//   AVER_CONV_PX, AVER_CONV_BPX       outputs (forward) / inputs (backward data) per thread along x
//   AVER_CONV_BW_COB, AVER_CONV_BW_CIC  output x input channels per backward-weights group (16 x 4)
//   AVER_CONV_ENTRY_<entry>           sizes groupshared for that kernel only
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
#ifndef AVER_CONV_PX
#define AVER_CONV_PX 4
#endif
#ifndef AVER_CONV_BPX
#define AVER_CONV_BPX 2
#endif
#ifndef AVER_CONV_BW_COB
#define AVER_CONV_BW_COB 16
#endif
#ifndef AVER_CONV_BW_CIC
#define AVER_CONV_BW_CIC 4
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
// Forward block: CO_BLOCK outputs x CI_BLK inputs x k*k (convSharedBytes); PX outputs per thread.
#define CONV_CI_BLK    CONV_MIN(AVER_CONV_CIN, AVER_CONV_CI_CHUNK)
#define CONV_CO_BLOCKS ((AVER_CONV_COUT + AVER_CONV_CO_BLOCK - 1) / AVER_CONV_CO_BLOCK)
#define CONV_FWD_N     (AVER_CONV_CO_BLOCK * CONV_CI_BLK * CONV_KK)
#define CONV_PX        AVER_CONV_PX
#define CONV_BPX       AVER_CONV_BPX   // backward data: inputs per thread along x (a multiple of the stride)
#define CONV_SPAN      ((CONV_PX - 1) * AVER_CONV_STRIDE + AVER_CONV_K)   // input columns per thread row
// Backward-data block: the roles swapped (CO_BLOCK input channels x CO_CHK output channels x k*k).
#define CONV_CO_CHK    CONV_MIN(AVER_CONV_COUT, AVER_CONV_CI_CHUNK)
#define CONV_CI_BLOCKS ((AVER_CONV_CIN + AVER_CONV_CO_BLOCK - 1) / AVER_CONV_CO_BLOCK)
#define CONV_BWD_N     (AVER_CONV_CO_BLOCK * CONV_CO_CHK * CONV_KK)
// Backward-weights staging: dz of BW_COB output channels (rows padded against bank conflicts), then the
// zero-padded input patch of BW_CIC input channels.
#define CONV_BW_COB        AVER_CONV_BW_COB
#define CONV_BW_CIC        AVER_CONV_BW_CIC
#define CONV_BW_CI_BLOCKS  ((AVER_CONV_CIN + CONV_BW_CIC - 1) / CONV_BW_CIC)
#define CONV_TSPAN         ((CONV_TILE - 1) * AVER_CONV_STRIDE + AVER_CONV_K)   // input patch edge per tile
#define CONV_BW_DZ_PITCH   (CONV_TILE * CONV_TILE + 1)
#define CONV_BW_X_BASE     (CONV_BW_COB * CONV_BW_DZ_PITCH)
#define CONV_BW_N          (CONV_BW_X_BASE + CONV_BW_CIC * CONV_TSPAN * CONV_TSPAN)
#define CONV_SHARED_MAX CONV_MAX(CONV_MAX(CONV_MAX(CONV_FWD_N, CONV_BWD_N), CONV_BW_N), AVER_NN_GROUP)

#if CONV_SHARED_MAX * 4 > 16384
#error "aver_neural_conv.hlsl: groupshared block exceeds 16 KB (the Vulkan minimum)"
#endif

// One array, aliased by the kernels (each uses it for one purpose). ConvNet::compile names the entry
// (AVER_CONV_ENTRY_<name>) so each pipeline allocates only what its kernel uses: groupshared bounds how
// many groups share a compute unit. The forward block is floored at 4.5 KB: at higher residency the
// 12-channel input layer ran 1.6x slower on D3D12 (RX 7800 XT; README bench), no other layer moved.
#define CONV_FWD_SHARED_MIN 1152
#if defined(AVER_CONV_ENTRY_CSConvForward)
#define CONV_SHARED_N CONV_MAX(CONV_FWD_N, CONV_FWD_SHARED_MIN)
#elif defined(AVER_CONV_ENTRY_CSConvBackwardData)
#define CONV_SHARED_N CONV_BWD_N
#elif defined(AVER_CONV_ENTRY_CSConvBackwardWeights)
#define CONV_SHARED_N CONV_BW_N
#elif defined(AVER_CONV_ENTRY_CSConvEvalReduce)
#define CONV_SHARED_N AVER_NN_GROUP
#else
#define CONV_SHARED_N CONV_SHARED_MAX
#endif
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
// Group = 8 x CONV_PX by 8 output positions x FWD_CO_BLOCK output channels of record z (x = column block *
// fwdCoBlocks + channel block: a tile's channel blocks run back to back and share its input in cache); each thread owns CONV_PX consecutive outputs of one row, so one input row
// segment in registers serves all of them. Weight block streamed through groupshared per input-channel
// chunk as [ci][tap][co] (the co run is contiguous: vector LDS reads); input read from global memory.
// Summation per output: bias, then ci / ky / kx ascending (the spec's canonical order).
[numthreads(CONV_TILE, CONV_TILE, 1)]
void CSConvForward(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    const uint tid = gtid.y * CONV_TILE + gtid.x;
    const uint bx = gid.x / CONV_CO_BLOCKS;
    const uint coBase = (gid.x - bx * CONV_CO_BLOCKS) * AVER_CONV_CO_BLOCK;
    const uint ox0 = (bx * CONV_TILE + gtid.x) * CONV_PX;
    const uint oy = gid.y * CONV_TILE + gtid.y;
    const uint n = gid.z;
    const bool rowOk = oy < gOutH && n < gN;
    const int ix0 = (int)(ox0 * AVER_CONV_STRIDE) - CONV_PAD;

    float acc[CONV_PX][AVER_CONV_CO_BLOCK];
    [unroll] for (uint j = 0; j < AVER_CONV_CO_BLOCK; ++j) {
        float b = 0.0;
#if AVER_CONV_BIAS
        if (coBase + j < AVER_CONV_COUT) b = fetchWeight(gBOffset + coBase + j);
#endif
        [unroll] for (uint p = 0; p < CONV_PX; ++p) acc[p][j] = b;
    }

    [loop] for (uint c0 = 0; c0 < AVER_CONV_CIN; c0 += CONV_CI_BLK) {
        GroupMemoryBarrierWithGroupSync();   // previous chunk's readers are done
        for (uint k = tid; k < CONV_FWD_N; k += AVER_NN_GROUP) {
            const uint j = k % AVER_CONV_CO_BLOCK;
            const uint r = k / AVER_CONV_CO_BLOCK;   // cil * KK + t
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
            const bool ciOk = rowOk && ci < AVER_CONV_CIN;
            const uint plane = (n * AVER_CONV_CIN + ci) * gInH;
            [unroll] for (uint ky = 0; ky < AVER_CONV_K; ++ky) {
                const int iy = (int)(oy * AVER_CONV_STRIDE) - CONV_PAD + (int)ky;
                const bool rowIn = ciOk && iy >= 0 && iy < (int)gInH;
                const uint rowBase = (plane + (uint)iy) * gInW;
                float xr[CONV_SPAN];
                [unroll] for (uint s = 0; s < CONV_SPAN; ++s) {
                    const int ix = ix0 + (int)s;
                    xr[s] = readX(rowBase + (uint)ix, rowIn && ix >= 0 && ix < (int)gInW);
                }
                [unroll] for (uint kx = 0; kx < AVER_CONV_K; ++kx) {
                    const uint wb = ((cil * AVER_CONV_K + ky) * AVER_CONV_K + kx) * AVER_CONV_CO_BLOCK;
                    float w[AVER_CONV_CO_BLOCK];
                    [unroll] for (uint j = 0; j < AVER_CONV_CO_BLOCK; ++j) w[j] = gShared[wb + j];
                    [unroll] for (uint p = 0; p < CONV_PX; ++p) {
                        const float x = xr[p * AVER_CONV_STRIDE + kx];
                        [unroll] for (uint j = 0; j < AVER_CONV_CO_BLOCK; ++j) acc[p][j] += w[j] * x;
                    }
                }
            }
        }
    }

    if (rowOk) {
        [unroll] for (uint p = 0; p < CONV_PX; ++p) {
            const uint ox = ox0 + p;
            [unroll] for (uint j = 0; j < AVER_CONV_CO_BLOCK; ++j) {
                const uint co = coBase + j;
                if (ox < gOutW && co < AVER_CONV_COUT)
                    gY[((n * AVER_CONV_COUT + co) * gOutH + oy) * gOutW + ox] = activate(AVER_CONV_ACT, acc[p][j]);
            }
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
// dX for one layer (> 0), gather form. Group = 8 x CONV_BPX by 8 input positions x CO_BLOCK input channels of
// record z (x = column block * ciBlocks + channel block); each thread owns CONV_BPX consecutive inputs of one
// row and reads one dz row segment per (co, ky). Weight block in groupshared as [co][tap][ci].
// Per element: co / ky / kx ascending, invalid taps skipped (the spec's order).
#define CONV_FLOOR_DIV(a, b) ((a) >= 0 ? (a) / (b) : -((-(a) + (b) - 1) / (b)))
#define CONV_SEG_LO CONV_FLOOR_DIV(CONV_PAD - (AVER_CONV_K - 1), AVER_CONV_STRIDE)
#define CONV_SEG_N  (CONV_FLOOR_DIV(CONV_BPX - 1 + CONV_PAD, AVER_CONV_STRIDE) - CONV_SEG_LO + 1)
[numthreads(CONV_TILE, CONV_TILE, 1)]
void CSConvBackwardData(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    const uint tid = gtid.y * CONV_TILE + gtid.x;
    const uint bx = gid.x / CONV_CI_BLOCKS;
    const uint ciBase = (gid.x - bx * CONV_CI_BLOCKS) * AVER_CONV_CO_BLOCK;
    const uint ix0 = (bx * CONV_TILE + gtid.x) * CONV_BPX;   // a multiple of the stride
    const uint iy = gid.y * CONV_TILE + gtid.y;
    const uint n = gid.z;
    const bool rowOk = iy < gInH && n < gN;
    const int ox0 = (int)(ix0 / AVER_CONV_STRIDE) + CONV_SEG_LO;

    float acc[CONV_BPX][AVER_CONV_CO_BLOCK];
    [unroll] for (uint p = 0; p < CONV_BPX; ++p) {
        [unroll] for (uint j = 0; j < AVER_CONV_CO_BLOCK; ++j) acc[p][j] = 0.0;
    }

    [loop] for (uint c0 = 0; c0 < AVER_CONV_COUT; c0 += CONV_CO_CHK) {
        GroupMemoryBarrierWithGroupSync();
        for (uint k = tid; k < CONV_BWD_N; k += AVER_NN_GROUP) {
            const uint j = k % AVER_CONV_CO_BLOCK;
            const uint r = k / AVER_CONV_CO_BLOCK;   // col * KK + t
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
            const bool coOk = rowOk && co < AVER_CONV_COUT;
            const uint plane = (n * AVER_CONV_COUT + co) * gOutH;
            [unroll] for (uint ky = 0; ky < AVER_CONV_K; ++ky) {
                const int ty = (int)iy + CONV_PAD - (int)ky;
                bool rowIn = coOk && ty >= 0;
#if AVER_CONV_STRIDE == 2
                rowIn = rowIn && ((ty & 1) == 0);
#endif
                const uint oy = (uint)ty / AVER_CONV_STRIDE;
                rowIn = rowIn && oy < gOutH;
                const uint rowBase = (plane + oy) * gOutW;
                float d[CONV_SEG_N];
                bool dOk[CONV_SEG_N];
                [unroll] for (uint s = 0; s < CONV_SEG_N; ++s) {
                    const int ox = ox0 + (int)s;
                    dOk[s] = rowIn && ox >= 0 && ox < (int)gOutW;
                    d[s] = readX(rowBase + (uint)ox, dOk[s]);
                }
                [unroll] for (uint kx = 0; kx < AVER_CONV_K; ++kx) {
                    const uint wb = (col * CONV_KK + ky * AVER_CONV_K + kx) * AVER_CONV_CO_BLOCK;
                    float w[AVER_CONV_CO_BLOCK];
                    [unroll] for (uint j = 0; j < AVER_CONV_CO_BLOCK; ++j) w[j] = gShared[wb + j];
                    [unroll] for (uint p = 0; p < CONV_BPX; ++p) {
                        const int off = (int)p + CONV_PAD - (int)kx;   // tx - ix0, compile-time
                        const bool tap = AVER_CONV_STRIDE == 1 || (off & 1) == 0;
                        const uint s = (uint)(CONV_FLOOR_DIV(off, AVER_CONV_STRIDE) - CONV_SEG_LO);
                        if (tap && dOk[s]) {
                            [unroll] for (uint j = 0; j < AVER_CONV_CO_BLOCK; ++j) acc[p][j] += w[j] * d[s];
                        }
                    }
                }
            }
        }
    }

    if (rowOk) {
        [unroll] for (uint p = 0; p < CONV_BPX; ++p) {
            const uint ix = ix0 + p;
            [unroll] for (uint j = 0; j < AVER_CONV_CO_BLOCK; ++j) {
                const uint ci = ciBase + j;
                if (ix < gInW && ci < AVER_CONV_CIN) gY[((n * AVER_CONV_CIN + ci) * gInH + iy) * gInW + ix] = acc[p][j];
            }
        }
    }
}

// ---- CSConvBackwardWeights ----
// Group = (record z, 8x8 output tile y, block x of BW_COB output x BW_CIC input channels); thread t owns the
// pair (co, ci) = (block co + t / BW_CIC, block ci + t % BW_CIC) and sums its k*k weight partials side by
// side; in ci-block 0 the first BW_COB threads also sum the biases. The group first stages this layer's own
// tile in groupshared: the dz tile of its output channels and the zero-padded input patch of its input
// channels. Each partial is the tile's row-major sum of dz * x (bias: dz), clipped to the output.
// Stored [p][w], p = (n * tilesY + ty) * tilesX + tx.
[numthreads(AVER_NN_GROUP, 1, 1)]
void CSConvBackwardWeights(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    const uint tid = gtid.x;
    const uint tileIdx = gid.y, n = gid.z;
    const uint coBlk = gid.x / CONV_BW_CI_BLOCKS;
    const uint ciBlk = gid.x - coBlk * CONV_BW_CI_BLOCKS;
    const uint coBase = coBlk * CONV_BW_COB, ciBase = ciBlk * CONV_BW_CIC;
    const bool groupOk = n < gN && tileIdx < gTilesX * gTilesY;
    const uint ty = tileIdx / gTilesX, tx = tileIdx - ty * gTilesX;
    const uint x0 = tx * CONV_TILE, y0 = ty * CONV_TILE;
    const int iy0 = (int)(y0 * AVER_CONV_STRIDE) - CONV_PAD, ix0 = (int)(x0 * AVER_CONV_STRIDE) - CONV_PAD;
    const uint plane = gOutH * gOutW;

    // Stage: dz [co][y][x] (row pitch CONV_BW_DZ_PITCH), then x [ci][py][px]; zeros outside.
    for (uint k = tid; k < CONV_BW_COB * CONV_TILE * CONV_TILE; k += AVER_NN_GROUP) {
        const uint col = k / (CONV_TILE * CONV_TILE);
        const uint r = k - col * (CONV_TILE * CONV_TILE);
        const uint yy = r / CONV_TILE, xx = r - yy * CONV_TILE;
        const uint co = coBase + col, y = y0 + yy, x = x0 + xx;
        const bool ok = groupOk && co < AVER_CONV_COUT && y < gOutH && x < gOutW;
        gShared[col * CONV_BW_DZ_PITCH + r] = readT((n * AVER_CONV_COUT + co) * plane + y * gOutW + x, ok);
    }
    for (uint k = tid; k < CONV_BW_CIC * CONV_TSPAN * CONV_TSPAN; k += AVER_NN_GROUP) {
        const uint cil = k / (CONV_TSPAN * CONV_TSPAN);
        const uint r = k - cil * (CONV_TSPAN * CONV_TSPAN);
        const uint py = r / CONV_TSPAN, px = r - py * CONV_TSPAN;
        const uint ci = ciBase + cil;
        const int iy = iy0 + (int)py, ix = ix0 + (int)px;
        const bool ok = groupOk && ci < AVER_CONV_CIN && iy >= 0 && iy < (int)gInH && ix >= 0 && ix < (int)gInW;
        gShared[CONV_BW_X_BASE + k] = readX(((n * AVER_CONV_CIN + ci) * gInH + (uint)iy) * gInW + (uint)ix, ok);
    }
    GroupMemoryBarrierWithGroupSync();

    const uint col = tid / CONV_BW_CIC, cil = tid - col * CONV_BW_CIC;
    const uint co = coBase + col, ci = ciBase + cil;
    const uint pOut = ((n * gTilesY + ty) * gTilesX + tx) * gLayerSize;
    const uint dzRow0 = col * CONV_BW_DZ_PITCH;
    const uint xPlane = CONV_BW_X_BASE + cil * (CONV_TSPAN * CONV_TSPAN);

    if (groupOk && co < AVER_CONV_COUT && ci < AVER_CONV_CIN) {
        float part[CONV_KK];
        [unroll] for (uint t = 0; t < CONV_KK; ++t) part[t] = 0.0;
        [loop] for (uint yy = 0; yy < CONV_TILE; ++yy) {
            if (y0 + yy >= gOutH) break;
            float dz[CONV_TILE];
            [unroll] for (uint xx = 0; xx < CONV_TILE; ++xx) dz[xx] = gShared[dzRow0 + yy * CONV_TILE + xx];
            [unroll] for (uint ky = 0; ky < AVER_CONV_K; ++ky) {
                const uint rowBase = xPlane + (yy * AVER_CONV_STRIDE + ky) * CONV_TSPAN;
                float xr[CONV_TSPAN];
                [unroll] for (uint s = 0; s < CONV_TSPAN; ++s) xr[s] = gShared[rowBase + s];
                [unroll] for (uint xx = 0; xx < CONV_TILE; ++xx) {
                    if (x0 + xx < gOutW) {
                        [unroll] for (uint kx = 0; kx < AVER_CONV_K; ++kx)
                            part[ky * AVER_CONV_K + kx] += dz[xx] * xr[xx * AVER_CONV_STRIDE + kx];
                    }
                }
            }
        }
        const uint w = (co * AVER_CONV_CIN + ci) * CONV_KK;
        [unroll] for (uint t = 0; t < CONV_KK; ++t) gPartials[pOut + w + t] = part[t];
    }

#if AVER_CONV_BIAS
    const uint bco = coBase + tid;
    if (groupOk && ciBlk == 0 && tid < CONV_BW_COB && bco < AVER_CONV_COUT) {
        float part = 0.0;
        [loop] for (uint yy = 0; yy < CONV_TILE; ++yy) {
            if (y0 + yy >= gOutH) break;
            [unroll] for (uint xx = 0; xx < CONV_TILE; ++xx) {
                if (x0 + xx < gOutW) part += gShared[tid * CONV_BW_DZ_PITCH + yy * CONV_TILE + xx];
            }
        }
        gPartials[pOut + AVER_CONV_COUT * AVER_CONV_CIN * CONV_KK + bco] = part;
    }
#endif
}

// ---- CSConvReduceGrad ----
// One thread per weight of the layer slice: sum partials p ascending (fp32), quantise ONCE, plain store
// into the accumulator at gWOffset + w (no atomics: every weight is written by exactly one thread). Loads
// are issued eight at a time; the adds stay in p order.
[numthreads(AVER_NN_GROUP, 1, 1)]
void CSConvReduceGrad(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    const uint w = linearIndex(gid, gtid.x);
    if (w >= gLayerSize) return;
    float g = 0.0;
    uint p = 0;
    [loop] for (; p + 8 <= gPartialCount; p += 8) {
        float v[8];
        [unroll] for (uint i = 0; i < 8; ++i) v[i] = gPartials[(p + i) * gLayerSize + w];
        [unroll] for (uint i2 = 0; i2 < 8; ++i2) g += v[i2];
    }
    [loop] for (; p < gPartialCount; ++p) g += gPartials[p * gLayerSize + w];
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
