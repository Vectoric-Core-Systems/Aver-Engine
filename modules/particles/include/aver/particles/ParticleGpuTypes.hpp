// Plain-data types shared with the GPU tier: the b3 block the compute kernels read (particles_sim.hlsl) and GpuJob, the per-tick job for ParticleGpu::record.
// C++ and HLSL declare the same layout (particles_common.hlsli is the HLSL twin): change a struct here, the HLSL and the static_asserts too.
#pragma once
#include "aver/particles/ParticleTypes.hpp"

#include <cstddef>
#include <cstring>
#include <vector>

namespace aver::particles {

inline constexpr u32 kGpuMaxRequests = 64;       // spawn requests per dispatch batch
inline constexpr u32 kGpuMaxModules  = kMaxModulesPerEmitter;
inline constexpr u32 kGpuModuleRows  = 3;
inline constexpr u32 kGpuEventCapacity = 2048;   // records per event type per emitter instance
inline constexpr u32 kGpuCounterCount = 16;
// Events buffer: two regions; the frame parity picks the one written and a child reads the other. A region is four header uints
// (death count, collide count), then kGpuEventCapacity death and as many collide records of eight uints (position xyz, id, velocity xyz, unused) as float bits.
inline constexpr u32 kGpuEventRecordUints = 8;
inline constexpr u32 kGpuEventHeaderUints = 4;
inline constexpr u32 kGpuEventRegionUints = kGpuEventHeaderUints + 2 * kGpuEventCapacity * kGpuEventRecordUints;

inline constexpr u32 kGpuFlagLinked      = 1u << 0;   // linked ribbon: slot = id % capacity, no dead list
inline constexpr u32 kGpuFlagTrail       = 1u << 1;   // trail ribbon: push to the trail ring
inline constexpr u32 kGpuFlagDeathEvent  = 1u << 2;
inline constexpr u32 kGpuFlagCollideEvent = 1u << 3;
inline constexpr u32 kGpuFlagSubuvStart  = 1u << 4;
inline constexpr u32 kGpuFlagHasEvents   = 1u << 5;   // an events buffer is bound (this emitter raises events or has children)

inline constexpr u32 kCtrDead = 0;
inline constexpr u32 kCtrAlive = 1;
inline constexpr u32 kCtrAliveNext = 2;
inline constexpr u32 kCtrStatAlive = 3;   // CSFinish copy for the stats readback
inline constexpr u32 kCtrSpawnedTotal = 4;
inline constexpr u32 kCtrDropped = 5;     // spawns refused (dead list empty) since init
inline constexpr u32 kCtrEvTotal = 6;     // event-driven spawns this frame (CSBegin -> CSSpawn)
inline constexpr u32 kCtrEvIdBase = 7;
inline constexpr u32 kCtrEvIdThis = 8;

inline constexpr u32 kArgDraw = 0;          // DrawIndexedIndirectArgs: indexCount, instanceCount, firstIndex, baseVertex, firstInstance
inline constexpr u32 kArgSpawn = 8;
inline constexpr u32 kArgUpdate = 12;
inline constexpr u32 kArgsUints = 16;

struct GpuSortCB {
    f32 viewRow[4];     // clip.w as a dot with (position, 1): view depth
    u32 aliveWrite;
    u32 sortedBase;
    u32 k, j;           // the merge size and stride of a Global / LocalMerge pass
};
static_assert(sizeof(GpuSortCB) == 32, "GpuSortCB is two float4 rows");

struct GpuSpawnRequest {
    f32 xf[16];           // emitter world matrix, row-major: rows 0..2 rotate/scale, row 3 translates
    f32 velocity[3];  u32 count;
    f32 addVelocity[3];   f32 speedScale;
    f32 sizeScale;    f32 lifeScale;   u32 tint;   u32 seed;
};
static_assert(sizeof(GpuSpawnRequest) == 112, "GpuSpawnRequest is 7 float4s; particles_common.hlsli reads gReq[i * 7 + k]");

// The header: twelve float4 rows, packed the way an HLSL cbuffer packs a struct (no member straddles a row).
struct GpuSimHeader {
    f32 dt;  f32 time;  u32 capacity;  u32 spawnTotal;
    u32 firstId;  u32 emitterSeed;  u32 requestCount;  u32 moduleCount;
    f32 gravity[3];  f32 damping;
    f32 origin[3];  u32 flags;
    u32 shape;  u32 shell;  u32 aliveRead;  u32 aliveWrite;             // alive* are element offsets into the alive buffer
    f32 shapeSize[3];  f32 spreadDeg;
    f32 direction[3];  f32 speedMin;
    f32 speedMax;  f32 lifeMin;  f32 lifeMax;  u32 trailRing;
    f32 trailSegment;  u32 subuvFrames;  f32 subuvFps;  u32 pad8;
    u32 evType;  u32 evCount;  f32 evInherit;  u32 evActive;            // evActive: this emitter consumes its parent's events
    u32 drawMult;  u32 indexCount;  u32 sortEnabled;  u32 evWriteBase;  // evWriteBase: this frame's event region (uint offset)
    u32 sortedBase;  u32 evReadBase;  u32 pad11a;  u32 pad11b;          // sortedBase: sorted-order region; evReadBase: the parent's region this emitter consumes
};
static_assert(sizeof(GpuSimHeader) == 192, "GpuSimHeader is twelve float4 rows");
static_assert(offsetof(GpuSimHeader, gravity) == 32 && offsetof(GpuSimHeader, origin) == 48 &&
              offsetof(GpuSimHeader, shape) == 64 && offsetof(GpuSimHeader, shapeSize) == 80 &&
              offsetof(GpuSimHeader, direction) == 96 && offsetof(GpuSimHeader, speedMax) == 112 &&
              offsetof(GpuSimHeader, trailSegment) == 128 && offsetof(GpuSimHeader, evType) == 144 &&
              offsetof(GpuSimHeader, drawMult) == 160 && offsetof(GpuSimHeader, sortedBase) == 176,
              "GpuSimHeader row offsets drifted from particles_common.hlsli");

struct GpuSimCB {
    GpuSimHeader h;
    f32 user[kMaxUserParams][4];
    u32 mod[kGpuMaxModules * kGpuModuleRows][4];   // per module: 8 parameter values (bits), packed user refs, kind/stage
    GpuSpawnRequest req[kGpuMaxRequests];
};
static_assert(offsetof(GpuSimCB, user) == 192 && offsetof(GpuSimCB, mod) == 448 && offsetof(GpuSimCB, req) == 1216,
              "GpuSimCB block offsets drifted from particles_common.hlsli");
static_assert(sizeof(GpuSimCB) == 8384, "GpuSimCB size drifted from particles_common.hlsli");

struct GpuJob {
    u32 handle = 0;
    u32 parent = 0;                      // handle whose events this instance consumes (0 none)
    u32 order = 0;                       // recording order key; children record before parents
    bool visible = true;                 // a culled instance is neither dispatched nor drawn
    f32 center[3] = {0, 0, 0};
    f32 boundsRadius = 0;                // ParticleEffect::boundsRadiusCm (0: never culled by frustum)
    f32 cullDistance = 0;
    GpuSimCB cb;
    std::vector<GpuSpawnRequest> extra;  // requests past the first 64 (more spawn dispatches)
};

// Floating-origin shift of one GPU instance's resident state (LARGE_WORLD 2.2.6): particle slots, trail points and pending event
// records all move by -delta. ParticleSystem::shiftWorld queues one per live GPU instance; ParticleGpu runs them (CSShift) before any job of the frame.
struct GpuShift {
    u32 handle = 0;
    f32 delta[3] = {0, 0, 0};
};

// CPU mirrors of CSShift's arithmetic (particles_sim.hlsl), so ParticleShiftTest can check the HLSL without a device. Only the fields the
// kernel writes shift. A trail point keeps its w (the particle's age); an event record shifts its position, not its velocity. Dead slots shift too (harmless).
inline void gpuShiftParticles(Particle* p, usize n, const f32 delta[3]) {
    for (usize i = 0; i < n; ++i) { p[i].position.x -= delta[0]; p[i].position.y -= delta[1]; p[i].position.z -= delta[2]; }
}
inline void gpuShiftTrail(Vec4* t, usize n, const f32 delta[3]) {
    for (usize i = 0; i < n; ++i) { t[i].x -= delta[0]; t[i].y -= delta[1]; t[i].z -= delta[2]; }
}
// `events` is the instance's whole events buffer: two regions of kGpuEventRegionUints uints. Only the first
// min(count, kGpuEventCapacity) records of each type are live; the rest is stale and left alone.
inline void gpuShiftEvents(u32* events, const f32 delta[3]) {
    for (u32 region = 0; region < 2; ++region) {
        u32* base = events + region * kGpuEventRegionUints;
        for (u32 type = 0; type < 2; ++type) {
            const u32 n = base[type] < kGpuEventCapacity ? base[type] : kGpuEventCapacity;
            u32* rec = base + kGpuEventHeaderUints + (type == 0 ? 0u : kGpuEventCapacity * kGpuEventRecordUints);
            for (u32 i = 0; i < n; ++i, rec += kGpuEventRecordUints)
                for (u32 a = 0; a < 3; ++a) {
                    f32 v;
                    std::memcpy(&v, &rec[a], sizeof v);
                    v -= delta[a];
                    std::memcpy(&rec[a], &v, sizeof v);
                }
        }
    }
}

// Packs `fx`'s enabled modules into the uint4 table (kGpuModuleRows rows each); returns the module count.
u32 packGpuModules(const ParticleEffect& fx, u32 (*outRows)[4]);

} // namespace aver::particles
