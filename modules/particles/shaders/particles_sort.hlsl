// Aver.Particles: back-to-front sorting of one GPU emitter's survivors with a bitonic network (Batcher 1968), implemented from
// the network definition in ParticleSortNetwork.hpp (the same index math). Dispatches follow that file's plan:
//   CSSortKeys        {~depth bits, slot} per survivor, padded to a power of two with 0xFFFFFFFF keys
//   CSSortLocal       every step with k <= the tile, in groupshared (one dispatch)
//   CSSortGlobal      one step with j >= the tile (one dispatch per step)
//   CSSortLocalMerge  the remaining j < tile steps of one k > tile, in groupshared
//   CSSortFinal       writes the sorted slots to the alive buffer's sorted-order region (what the draw reads)
// The host sizes the dispatches for the pool capacity; a thread or group beyond the actual power of two returns at once.
// Same compute layout and binding set as particles_sim.hlsl.
#include "particles_common.hlsli"

cbuffer SortCB : register(b3) {
    float4 gViewRow;
    uint gAliveWrite;
    uint gSortedBase;
    uint gK;
    uint gJ;
};

RWStructuredBuffer<PParticle> gParticles : register(u0);
RWStructuredBuffer<uint>      gDead      : register(u1);
RWStructuredBuffer<uint>      gAlive     : register(u2);
RWStructuredBuffer<uint>      gCtr       : register(u3);
RWStructuredBuffer<uint>      gArgs      : register(u4);
RWStructuredBuffer<uint>      gEvents    : register(u5);
RWStructuredBuffer<uint>      gParentEvents : register(u6);
RWStructuredBuffer<uint2>     gSortKeys  : register(u7);
RWStructuredBuffer<float4>    gTrail     : register(u8);

#define SORT_TILE 2048u

groupshared uint2 gTile[SORT_TILE];

uint sortNextPow2(uint n) {
    uint p = 1u;
    while (p < n) p <<= 1u;
    return p;
}
bool sortLess(uint2 a, uint2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); }

// The lower element of pair t at stride j (bit j of the index clear).
uint sortLower(uint t, uint j) { return ((t & ~(j - 1u)) << 1u) | (t & (j - 1u)); }

[numthreads(256, 1, 1)]
void CSSortKeys(uint3 dtid : SV_DispatchThreadID) {
    uint n = gCtr[PCTR_ALIVE_NEXT];
    uint N = sortNextPow2(max(n, 1u));
    uint i = dtid.x;
    if (i >= N) return;
    if (i < n) {
        uint slot = gAlive[gAliveWrite + i];
        float3 pos = gParticles[slot].position;
        float depth = max(dot(gViewRow, float4(pos, 1.0)), 0.0);
        gSortKeys[i] = uint2(min(~asuint(depth), 0xFFFFFFFEu), slot);
    } else {
        gSortKeys[i] = uint2(0xFFFFFFFFu, i);
    }
}

[numthreads(1024, 1, 1)]
void CSSortLocal(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    uint n = gCtr[PCTR_ALIVE_NEXT];
    uint N = sortNextPow2(max(n, 1u));
    uint tile = min(N, SORT_TILE);
    uint base = gid.x * tile;
    if (base >= N) return;
    for (uint a = gtid.x; a < tile; a += 1024u) gTile[a] = gSortKeys[base + a];
    GroupMemoryBarrierWithGroupSync();
    for (uint k = 2u; k <= tile; k <<= 1u) {
        for (uint j = k >> 1u; j > 0u; j >>= 1u) {
            uint t = gtid.x;
            if (t < tile / 2u) {
                uint lo = sortLower(t, j);
                uint hi = lo | j;
                bool asc = (((base + lo) & k) == 0u);
                uint2 x = gTile[lo], y = gTile[hi];
                bool doSwap = asc ? sortLess(y, x) : sortLess(x, y);
                if (doSwap) { gTile[lo] = y; gTile[hi] = x; }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }
    for (uint b = gtid.x; b < tile; b += 1024u) gSortKeys[base + b] = gTile[b];
}

[numthreads(256, 1, 1)]
void CSSortGlobal(uint3 dtid : SV_DispatchThreadID) {
    uint n = gCtr[PCTR_ALIVE_NEXT];
    uint N = sortNextPow2(max(n, 1u));
    if (gK > N) return;
    uint t = dtid.x;
    if (t >= N / 2u) return;
    uint lo = sortLower(t, gJ);
    uint hi = lo | gJ;
    bool asc = ((lo & gK) == 0u);
    uint2 x = gSortKeys[lo], y = gSortKeys[hi];
    bool doSwap = asc ? sortLess(y, x) : sortLess(x, y);
    if (doSwap) { gSortKeys[lo] = y; gSortKeys[hi] = x; }
}

[numthreads(1024, 1, 1)]
void CSSortLocalMerge(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    uint n = gCtr[PCTR_ALIVE_NEXT];
    uint N = sortNextPow2(max(n, 1u));
    if (gK > N || N <= SORT_TILE) return;
    uint base = gid.x * SORT_TILE;
    if (base >= N) return;
    for (uint a = gtid.x; a < SORT_TILE; a += 1024u) gTile[a] = gSortKeys[base + a];
    GroupMemoryBarrierWithGroupSync();
    for (uint j = SORT_TILE >> 1u; j > 0u; j >>= 1u) {
        uint t = gtid.x;
        uint lo = sortLower(t, j);
        uint hi = lo | j;
        bool asc = (((base + lo) & gK) == 0u);
        uint2 x = gTile[lo], y = gTile[hi];
        bool doSwap = asc ? sortLess(y, x) : sortLess(x, y);
        if (doSwap) { gTile[lo] = y; gTile[hi] = x; }
        GroupMemoryBarrierWithGroupSync();
    }
    for (uint b = gtid.x; b < SORT_TILE; b += 1024u) gSortKeys[base + b] = gTile[b];
}

[numthreads(256, 1, 1)]
void CSSortFinal(uint3 dtid : SV_DispatchThreadID) {
    uint n = gCtr[PCTR_ALIVE_NEXT];
    uint i = dtid.x;
    if (i >= n) return;
    gAlive[gSortedBase + i] = gSortKeys[i].y;
}
