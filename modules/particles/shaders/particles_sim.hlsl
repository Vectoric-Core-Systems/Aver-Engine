// Aver.Particles: the GPU tier's simulation kernels. One compute layout shared by all (9 UAVs, root CBV b3), so every
// kernel of every emitter instance runs from the same binding set. Written against ParticleGpuTypes.hpp's GpuSimCB.
//
// Per frame and instance: CSBegin (swap the alive lists, size the spawn dispatch) -> CSSpawn (pop dead slots, initialise
// particles, append to the current alive list) -> CSArgsUpdate -> CSUpdate (age, integrate, run the modules, survivors
// append to the next list, the dead push their slot back) -> CSFinish (draw arguments, stats). CSInit runs once when an
// instance is acquired. CSProbe writes every field it reads so the layout can be checked against the C++ twin.
//
// No wave intrinsics, no 64-bit atomics: InterlockedAdd on 32-bit structured buffers only.
#include "particles_common.hlsli"

cbuffer SimCB : register(b3) {
    PSimHeader gS;
    float4 gUser[16];
    uint4  gMod[48];
    float4 gReq[448];
};

RWStructuredBuffer<PParticle> gParticles   : register(u0);
RWStructuredBuffer<uint>      gDead        : register(u1);
RWStructuredBuffer<uint>      gAlive       : register(u2);   // two survivor lists then the sorted-order region
RWStructuredBuffer<uint>      gCtr         : register(u3);
RWStructuredBuffer<uint>      gArgs        : register(u4);
RWStructuredBuffer<uint>      gEvents      : register(u5);   // this instance's two event regions
RWStructuredBuffer<uint>      gParentEvents: register(u6);   // the parent's, read-only here
RWStructuredBuffer<uint2>     gSortKeys    : register(u7);   // unused by the simulation, bound for the sort kernels
RWStructuredBuffer<float4>    gTrail       : register(u8);   // trail ribbons: ring points per slot

#if AVER_CUSTOM_PARTICLES
// ------------------------------------------------------------------------------------------------ a custom particles program as the custom stage
// Compiled only for an emitter that carries one (custom_particles.hlsl is in the prelude, aver::rhi::customParticlesProgram). The program sees the particle's state in
// its own space and units (metres, +Y up); the particle keeps position, velocity, age and lifetime, and gTrail -- unused by a sprite emitter -- holds what the
// 64-byte particle has no room for: per slot five float4s, COLOR, CUSTOM, then the transform's three basis columns with MASS in the first one's w.
// The draw reads colour (as 8 bits per channel), uniform scale and a spin from the particle; the full state stays here. The program's process() does the integrating
// (TRANSFORM[3].xyz += VELOCITY * DELTA), so the built-in gravity, damping, modules and position step are skipped for such an emitter.
#define PCUSTOM_VECS 5u
void pCustomLoadTable() {
    [unroll] for (uint i = 0u; i < 16u; ++i) {
        gCustomPartU[i] = gUser[i];
        gCustomPartG[i] = gUser[min(i + (uint)AVER_CUSTOM_PART_REGS, 15u)];
    }
}
void pCustomStore(uint slot, inout PParticle p, CustomPartIO io) {
    p.position = cenvS2E(io.xform[3].xyz) * 100.0;
    p.velocity = cenvS2E(io.velocity) * 100.0;
    const float3 c0 = io.xform[0].xyz, c1 = io.xform[1].xyz, c2 = io.xform[2].xyz;
    p.sizeScale = (length(c0) + length(c1) + length(c2)) * (1.0 / 3.0);
    p.rotation = atan2(c0.y, c0.x);
    p.tint = pPackRgba8(saturate(io.color));
    const uint b = slot * PCUSTOM_VECS;
    gTrail[b + 0u] = io.color;
    gTrail[b + 1u] = io.custom;
    gTrail[b + 2u] = float4(c0, io.mass);
    gTrail[b + 3u] = float4(c1, 0.0);
    gTrail[b + 4u] = float4(c2, 0.0);
    if (!io.active) p.age = p.lifetime;   // dies at the next update
}
CustomPartIO pCustomBase(uint slot, PParticle p) {
    CustomPartIO io = (CustomPartIO)0;
    io.lifetime = p.lifetime;
    io.delta = gS.dt;
    io.time = gS.time;
    io.number = p.id;
    io.index = slot;
    io.seed = p.seed;
    io.active = true;
    io.mass = 1.0;
    io.color = float4(1.0, 1.0, 1.0, 1.0);
    io.custom = float4(0.0, 0.0, 0.0, 0.0);
    io.emission = customPartMatrixFromEngine(float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1), float3(0, 0, 0));
    return io;
}
void pCustomSpawn(uint slot, inout PParticle p, float4 xf0, float4 xf1, float4 xf2, float4 xf3) {
    pCustomLoadTable();
    CustomPartIO io = pCustomBase(slot, p);
    io.restart = true;
    io.emission = customPartMatrixFromEngine(xf0.xyz, xf1.xyz, xf2.xyz, xf3.xyz);
    io.xform = customPartMatrixFromEngine(xf0.xyz, xf1.xyz, xf2.xyz, p.position);
    io.velocity = cenvE2S(p.velocity) * 0.01;
#if AVER_CUSTOM_HAS_START
    averCustomParticleStart(io);
#endif
    pCustomStore(slot, p, io);
}
// False when the shader cleared ACTIVE (the particle dies).
bool pCustomProcess(uint slot, inout PParticle p) {
#if AVER_CUSTOM_HAS_PROCESS
    pCustomLoadTable();
    const uint b = slot * PCUSTOM_VECS;
    const float4 r0 = gTrail[b + 2u], r1 = gTrail[b + 3u], r2 = gTrail[b + 4u];
    CustomPartIO io = pCustomBase(slot, p);
    io.restart = (p.age - gS.dt) <= 1.0e-6;
    io.color = gTrail[b + 0u];
    io.custom = gTrail[b + 1u];
    io.mass = r0.w;
    io.xform = float4x4(float4(r0.xyz, 0.0), float4(r1.xyz, 0.0), float4(r2.xyz, 0.0), float4(cenvE2S(p.position) * 0.01, 1.0));
    io.velocity = cenvE2S(p.velocity) * 0.01;
    averCustomParticleProcess(io);
    pCustomStore(slot, p, io);
    return io.active;
#else
    return true;
#endif
}
#endif

float pUserValue(uint refByte, float literal) {
    if (refByte == 0u) return literal;
    uint r = refByte - 1u;
    return gUser[r >> 2][r & 3u];
}

PMod pLoadModule(uint index) {
    PMod m;
    uint4 r0 = gMod[index * 3u + 0u], r1 = gMod[index * 3u + 1u], r2 = gMod[index * 3u + 2u];
    uint refs[8] = {
        r2.x & 255u, (r2.x >> 8) & 255u, (r2.x >> 16) & 255u, (r2.x >> 24) & 255u,
        r2.y & 255u, (r2.y >> 8) & 255u, (r2.y >> 16) & 255u, (r2.y >> 24) & 255u
    };
    float lit[8] = { asfloat(r0.x), asfloat(r0.y), asfloat(r0.z), asfloat(r0.w), asfloat(r1.x), asfloat(r1.y), asfloat(r1.z), asfloat(r1.w) };
    m.kind = r2.z;
    m.stage = r2.w;
    [unroll] for (uint i = 0; i < 8u; ++i) m.p[i] = pUserValue(refs[i], lit[i]);
    return m;
}

float4 pReq(uint req, uint row) { return gReq[req * 7u + row]; }

// ------------------------------------------------------------------------------------------------ event records
uint pEventBase(uint region, uint type) {
    return region + PEV_HEADER + (type == 0u ? 0u : PEV_CAP * PEV_REC);
}
void pRaiseEvent(uint type, PParticle p) {
    uint idx;
    InterlockedAdd(gEvents[gS.evWriteBase + type], 1u, idx);
    if (idx >= PEV_CAP) return;
    uint b = pEventBase(gS.evWriteBase, type) + idx * PEV_REC;
    gEvents[b + 0u] = asuint(p.position.x); gEvents[b + 1u] = asuint(p.position.y); gEvents[b + 2u] = asuint(p.position.z);
    gEvents[b + 3u] = p.id;
    gEvents[b + 4u] = asuint(p.velocity.x); gEvents[b + 5u] = asuint(p.velocity.y); gEvents[b + 6u] = asuint(p.velocity.z);
    gEvents[b + 7u] = 0u;
}

// ------------------------------------------------------------------------------------------------ CSInit
// Once per acquired instance: dead list = 0..capacity-1, counters zero, particle slots zero (a dead slot is lifetime 0).
[numthreads(64, 1, 1)]
void CSInit(uint3 dtid : SV_DispatchThreadID) {
    uint i = dtid.x;
    if (i >= gS.capacity) return;
    gDead[i] = i;
    PParticle z = (PParticle)0;
    gParticles[i] = z;
    if (i == 0u) {
        for (uint c = 0u; c < 16u; ++c) gCtr[c] = 0u;
        gCtr[PCTR_DEAD] = gS.capacity;
        for (uint a = 0u; a < 16u; ++a) gArgs[a] = 0u;
        gArgs[PARG_SPAWN + 1u] = 1u; gArgs[PARG_SPAWN + 2u] = 1u;
        gArgs[PARG_UPDATE + 1u] = 1u; gArgs[PARG_UPDATE + 2u] = 1u;
        if ((gS.flags & PGPU_FLAG_HAS_EVENTS) != 0u) {
            gEvents[0] = 0u; gEvents[1] = 0u;
            gEvents[ (PEV_HEADER + 2u * PEV_CAP * PEV_REC) + 0u] = 0u;
            gEvents[ (PEV_HEADER + 2u * PEV_CAP * PEV_REC) + 1u] = 0u;
        }
    }
}

// ------------------------------------------------------------------------------------------------ CSBegin
[numthreads(1, 1, 1)]
void CSBegin(uint3 dtid : SV_DispatchThreadID) {
    if ((gS.flags & PGPU_FLAG_LINKED) == 0u) {
        gCtr[PCTR_ALIVE] = gCtr[PCTR_ALIVE_NEXT];
        gCtr[PCTR_ALIVE_NEXT] = 0u;
    }
    uint evTotal = 0u;
    if (gS.evActive != 0u) {
        uint n = min(gParentEvents[gS.evReadBase + gS.evType], PEV_CAP);
        evTotal = n * gS.evCount;
    }
    gCtr[PCTR_EV_TOTAL] = evTotal;
    gCtr[PCTR_EV_ID_THIS] = gCtr[PCTR_EV_ID_BASE];
    gCtr[PCTR_EV_ID_BASE] = gCtr[PCTR_EV_ID_BASE] + evTotal;
    uint total = gS.spawnTotal + evTotal;
    gArgs[PARG_SPAWN + 0u] = (total + 63u) / 64u;
    gArgs[PARG_SPAWN + 1u] = 1u;
    gArgs[PARG_SPAWN + 2u] = 1u;
    if ((gS.flags & PGPU_FLAG_HAS_EVENTS) != 0u) {
        gEvents[gS.evWriteBase + 0u] = 0u;
        gEvents[gS.evWriteBase + 1u] = 0u;
    }
}

// ------------------------------------------------------------------------------------------------ CSSpawn
[numthreads(64, 1, 1)]
void CSSpawn(uint3 dtid : SV_DispatchThreadID) {
    uint gid = dtid.x;
    uint cpuTotal = gS.spawnTotal;
    uint evTotal = (gS.evActive != 0u) ? gCtr[PCTR_EV_TOTAL] : 0u;
    if (gid >= cpuTotal + evTotal) return;

    float4 xf0, xf1, xf2, xf3;
    float3 velocity, addVelocity;
    float speedScale, sizeScale, lifeScale;
    uint tint, reqSeed, id;

    if (gid < cpuTotal) {
        uint acc = 0u, k = 0u;
        for (uint r = 0u; r < gS.requestCount; ++r) {
            uint c = asuint(pReq(r, 4u).w);
            if (gid < acc + c) { k = r; break; }
            acc += c;
        }
        xf0 = pReq(k, 0u); xf1 = pReq(k, 1u); xf2 = pReq(k, 2u); xf3 = pReq(k, 3u);
        float4 r4 = pReq(k, 4u), r5 = pReq(k, 5u), r6 = pReq(k, 6u);
        velocity = r4.xyz;
        addVelocity = r5.xyz;
        speedScale = r5.w;
        sizeScale = r6.x; lifeScale = r6.y;
        tint = asuint(r6.z); reqSeed = asuint(r6.w);
        id = gS.firstId + gid;
    } else {
        uint e = gid - cpuTotal;
        uint rec = e / max(gS.evCount, 1u);
        uint b = pEventBase(gS.evReadBase, gS.evType) + rec * PEV_REC;
        float3 evPos = float3(asfloat(gParentEvents[b + 0u]), asfloat(gParentEvents[b + 1u]), asfloat(gParentEvents[b + 2u]));
        uint evId = gParentEvents[b + 3u];
        float3 evVel = float3(asfloat(gParentEvents[b + 4u]), asfloat(gParentEvents[b + 5u]), asfloat(gParentEvents[b + 6u]));
        xf0 = float4(1, 0, 0, 0); xf1 = float4(0, 1, 0, 0); xf2 = float4(0, 0, 1, 0); xf3 = float4(evPos, 1);
        velocity = float3(0, 0, 0);
        addVelocity = evVel * gS.evInherit;
        speedScale = 1.0; sizeScale = 1.0; lifeScale = 1.0;
        tint = 0xFFFFFFFFu;
        reqSeed = (evId * 2654435761u) | 1u;
        id = 0x80000000u | (gCtr[PCTR_EV_ID_THIS] + e);
    }

    uint slot;
    if ((gS.flags & PGPU_FLAG_LINKED) != 0u) {
        slot = id % gS.capacity;
    } else {
        uint old;
        InterlockedAdd(gCtr[PCTR_DEAD], 0xFFFFFFFFu, old);
        if ((int)old <= 0) {
            InterlockedAdd(gCtr[PCTR_DEAD], 1u);
            InterlockedAdd(gCtr[PCTR_DROPPED], 1u);
            return;
        }
        slot = gDead[old - 1u];
    }

    uint effSeed = reqSeed != 0u ? (gS.emitterSeed ^ pHash32(reqSeed)) : gS.emitterSeed;
    PParticle p = (PParticle)0;
    p.seed = pParticleSeed(effSeed, id);
    p.id = id;
    p.position = pTransformPoint(xf0, xf1, xf2, xf3, pShapeOffset(gS, p.seed));

    float3 axis = pTransformDir(xf0, xf1, xf2, gS.direction);
    float al = length(axis);
    axis = (al * al >= 1e-8) ? axis / al : float3(0, 0, 1);
    if (dot(axis, axis) < 0.5) axis = float3(0, 0, 1);
    float3 dir = pConeSample(axis, gS.spreadDeg, p.seed);
    float speed = lerp(gS.speedMin, gS.speedMax, pRand01(p.seed, PSTREAM_SPEED)) * speedScale;
    p.velocity = dir * speed + addVelocity;
    p.age = 0.0;
    p.lifetime = max(0.001, lerp(gS.lifeMin, gS.lifeMax, pRand01(p.seed, PSTREAM_LIFETIME)) * lifeScale);
    p.sizeScale = sizeScale;
    p.tint = tint;
    uint frame = 0u;
    if ((gS.flags & PGPU_FLAG_SUBUV_START) != 0u)
        frame = min((uint)(pRand01(p.seed, PSTREAM_START_FRAME) * (float)gS.subuvFrames), gS.subuvFrames - 1u);
    p.spawnInfo = frame & 0xFFFFu;

    for (uint mi = 0u; mi < gS.moduleCount; ++mi) {
        PMod m = pLoadModule(mi);
        if (m.stage == 0u) pRunSpawnModule(m, p, velocity);
    }
#if AVER_CUSTOM_PARTICLES
    if ((gS.flags & PGPU_FLAG_CUSTOM) != 0u) pCustomSpawn(slot, p, xf0, xf1, xf2, xf3);
#endif

    if (gS.trailRing != 0u) {
        gTrail[slot * gS.trailRing] = float4(p.position, 0.0);
        p.trail = 1u << 8;
    }
    gParticles[slot] = p;
    InterlockedAdd(gCtr[PCTR_SPAWNED], 1u);

    if ((gS.flags & PGPU_FLAG_LINKED) == 0u) {
        uint idx;
        InterlockedAdd(gCtr[PCTR_ALIVE], 1u, idx);
        gAlive[gS.aliveRead + idx] = slot;
    }
}

// ------------------------------------------------------------------------------------------------ CSArgsUpdate
[numthreads(1, 1, 1)]
void CSArgsUpdate(uint3 dtid : SV_DispatchThreadID) {
    uint n = ((gS.flags & PGPU_FLAG_LINKED) != 0u) ? gS.capacity : gCtr[PCTR_ALIVE];
    gArgs[PARG_UPDATE + 0u] = (n + 63u) / 64u;
    gArgs[PARG_UPDATE + 1u] = 1u;
    gArgs[PARG_UPDATE + 2u] = 1u;
}

// ------------------------------------------------------------------------------------------------ CSUpdate
[numthreads(64, 1, 1)]
void CSUpdate(uint3 dtid : SV_DispatchThreadID) {
    const bool linked = (gS.flags & PGPU_FLAG_LINKED) != 0u;
    uint i = dtid.x;
    uint slot;
    if (linked) {
        if (i >= gS.capacity) return;
        slot = i;
    } else {
        if (i >= gCtr[PCTR_ALIVE]) return;
        slot = gAlive[gS.aliveRead + i];
    }
    PParticle p = gParticles[slot];
    if (linked && !(p.age < p.lifetime)) return;   // an empty ring slot

    p.age += gS.dt;
    bool dead = false;
    if (p.age >= p.lifetime) {
        if ((gS.flags & PGPU_FLAG_DEATH_EVT) != 0u) pRaiseEvent(0u, p);
        dead = true;
    }
#if AVER_CUSTOM_PARTICLES
    else if ((gS.flags & PGPU_FLAG_CUSTOM) != 0u) {
        // The program's process() is the whole update (see pCustomProcess).
        if (!pCustomProcess(slot, p)) {
            if ((gS.flags & PGPU_FLAG_DEATH_EVT) != 0u) pRaiseEvent(0u, p);
            dead = true;
        }
    }
#endif
    else {
        p.velocity += gS.gravity * gS.dt;
        if (gS.damping > 0.0) p.velocity *= max(0.0, 1.0 - gS.damping * gS.dt);
        for (uint mi = 0u; mi < gS.moduleCount; ++mi) {
            PMod m = pLoadModule(mi);
            if (m.stage == 1u) pRunVelocityModule(m, gS.origin, gS.time, gS.dt, p);
        }
        p.position += p.velocity * gS.dt;
        for (uint mj = 0u; mj < gS.moduleCount; ++mj) {
            PMod m = pLoadModule(mj);
            if (m.stage != 2u) continue;
            uint bits = pRunPositionModule(m, p);
            if ((bits & 2u) != 0u && (gS.flags & PGPU_FLAG_COLLIDE_EVT) != 0u) pRaiseEvent(1u, p);
            if ((bits & 1u) != 0u) {
                if ((gS.flags & PGPU_FLAG_DEATH_EVT) != 0u) pRaiseEvent(0u, p);
                dead = true;
                break;
            }
        }
    }

    if (dead) {
        if (linked) {
            p.age = 0.0; p.lifetime = 0.0;
            gParticles[slot] = p;
        } else {
            uint k;
            InterlockedAdd(gCtr[PCTR_DEAD], 1u, k);
            gDead[k] = slot;
        }
        return;
    }

    p.rotation += p.angularVelocity * gS.dt;

    if (gS.trailRing != 0u) {
        uint head = p.trail & 255u, count = (p.trail >> 8) & 255u;
        float4 newest = gTrail[slot * gS.trailRing + head];
        float3 d = p.position - newest.xyz;
        if (count == 0u || dot(d, d) >= gS.trailSegment * gS.trailSegment) {
            uint nh = count == 0u ? 0u : (head + 1u) % gS.trailRing;
            gTrail[slot * gS.trailRing + nh] = float4(p.position, p.age);
            p.trail = nh | (min(count + 1u, gS.trailRing) << 8);
        }
    }
    gParticles[slot] = p;

    if (!linked) {
        uint j;
        InterlockedAdd(gCtr[PCTR_ALIVE_NEXT], 1u, j);
        gAlive[gS.aliveWrite + j] = slot;
    }
}

// ------------------------------------------------------------------------------------------------ CSShift
// The floating origin moved (LARGE_WORLD 2.2.6): every resident world position of this instance is gS.origin.xyz smaller
// (the header's origin field carries the DELTA for this kernel; nothing else reads a header here). One thread per slot:
// the particle, its trail point, and event record i of each type in both event regions. Mirrors gpuShiftParticles /
// gpuShiftTrail / gpuShiftEvents in ParticleGpuTypes.hpp. Dispatched with max(capacity, trail points, PEV_CAP) threads.
[numthreads(64, 1, 1)]
void CSShift(uint3 dtid : SV_DispatchThreadID) {
    const uint i = dtid.x;
    const float3 d = gS.origin;
    if (i < gS.capacity) {
        const float3 pos = gParticles[i].position;
        gParticles[i].position = pos - d;
    }
    if ((gS.flags & PGPU_FLAG_TRAIL) != 0u && i < gS.capacity * gS.trailRing) {
        const float4 t = gTrail[i];
        gTrail[i] = float4(t.xyz - d, t.w);
    }
    if ((gS.flags & PGPU_FLAG_HAS_EVENTS) != 0u && i < PEV_CAP) {
        [unroll] for (uint r = 0u; r < 2u; ++r) {
            const uint region = r * (PEV_HEADER + 2u * PEV_CAP * PEV_REC);
            [unroll] for (uint type = 0u; type < 2u; ++type) {
                if (i < min(gEvents[region + type], PEV_CAP)) {
                    const uint b = pEventBase(region, type) + i * PEV_REC;
                    gEvents[b + 0u] = asuint(asfloat(gEvents[b + 0u]) - d.x);
                    gEvents[b + 1u] = asuint(asfloat(gEvents[b + 1u]) - d.y);
                    gEvents[b + 2u] = asuint(asfloat(gEvents[b + 2u]) - d.z);
                }
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------ CSFinish
[numthreads(1, 1, 1)]
void CSFinish(uint3 dtid : SV_DispatchThreadID) {
    const bool linked = (gS.flags & PGPU_FLAG_LINKED) != 0u;
    uint alive = linked ? 0u : gCtr[PCTR_ALIVE_NEXT];
    gArgs[PARG_DRAW + 0u] = gS.indexCount;
    gArgs[PARG_DRAW + 1u] = linked ? gS.capacity : alive * gS.drawMult;
    gArgs[PARG_DRAW + 2u] = 0u;
    gArgs[PARG_DRAW + 3u] = 0u;
    gArgs[PARG_DRAW + 4u] = 0u;
    gCtr[PCTR_STAT_ALIVE] = alive;
}

// ------------------------------------------------------------------------------------------------ CSProbe
// Copies every field the kernels read, in declaration order, into gDead (at least 160 uints). The parity test fills a
// GpuSimCB with a distinct bit pattern per field and compares: a drifted mirror fails by index.
[numthreads(1, 1, 1)]
void CSProbe(uint3 dtid : SV_DispatchThreadID) {
    uint o = 0u;
    gDead[o++] = asuint(gS.dt); gDead[o++] = asuint(gS.time); gDead[o++] = gS.capacity; gDead[o++] = gS.spawnTotal;
    gDead[o++] = gS.firstId; gDead[o++] = gS.emitterSeed; gDead[o++] = gS.requestCount; gDead[o++] = gS.moduleCount;
    gDead[o++] = asuint(gS.gravity.x); gDead[o++] = asuint(gS.gravity.y); gDead[o++] = asuint(gS.gravity.z); gDead[o++] = asuint(gS.damping);
    gDead[o++] = asuint(gS.origin.x); gDead[o++] = asuint(gS.origin.y); gDead[o++] = asuint(gS.origin.z); gDead[o++] = gS.flags;
    gDead[o++] = gS.shape; gDead[o++] = gS.shell; gDead[o++] = gS.aliveRead; gDead[o++] = gS.aliveWrite;
    gDead[o++] = asuint(gS.shapeSize.x); gDead[o++] = asuint(gS.shapeSize.y); gDead[o++] = asuint(gS.shapeSize.z); gDead[o++] = asuint(gS.spreadDeg);
    gDead[o++] = asuint(gS.direction.x); gDead[o++] = asuint(gS.direction.y); gDead[o++] = asuint(gS.direction.z); gDead[o++] = asuint(gS.speedMin);
    gDead[o++] = asuint(gS.speedMax); gDead[o++] = asuint(gS.lifeMin); gDead[o++] = asuint(gS.lifeMax); gDead[o++] = gS.trailRing;
    gDead[o++] = asuint(gS.trailSegment); gDead[o++] = gS.subuvFrames; gDead[o++] = asuint(gS.subuvFps); gDead[o++] = gS.pad8;
    gDead[o++] = gS.evType; gDead[o++] = gS.evCount; gDead[o++] = asuint(gS.evInherit); gDead[o++] = gS.evActive;
    gDead[o++] = gS.drawMult; gDead[o++] = gS.indexCount; gDead[o++] = gS.sortEnabled; gDead[o++] = gS.evWriteBase;
    gDead[o++] = gS.sortedBase; gDead[o++] = gS.evReadBase; gDead[o++] = gS.pad11a; gDead[o++] = gS.pad11b;
    // 48 uints so far. Then the first and last user value, the first and last module row and the first and last request row.
    gDead[o++] = asuint(gUser[0].x); gDead[o++] = asuint(gUser[0].w); gDead[o++] = asuint(gUser[15].x); gDead[o++] = asuint(gUser[15].w);
    gDead[o++] = gMod[0].x; gDead[o++] = gMod[0].w; gDead[o++] = gMod[47].x; gDead[o++] = gMod[47].w;
    gDead[o++] = asuint(gReq[0].x); gDead[o++] = asuint(gReq[0].w); gDead[o++] = asuint(gReq[447].x); gDead[o++] = asuint(gReq[447].w);
    // Request 1 starts at row 7: its count and seed live in rows 11 and 13.
    gDead[o++] = asuint(gReq[11].w); gDead[o++] = asuint(gReq[13].w);
}

// ------------------------------------------------------------------------------------------------ CSRngProbe
// 32 (seed, stream) pairs arrive as the first 32 float4s of gReq (x = seed bits, y = stream bits); gDead[i] gets the top 24 bits
// of the draw, which ParticleRandom.hpp's kRandGolden pins on the CPU.
[numthreads(32, 1, 1)]
void CSRngProbe(uint3 dtid : SV_DispatchThreadID) {
    uint i = dtid.x;
    float4 r = gReq[i];
    gDead[i] = pHash32(asuint(r.x) + 0x9E3779B9u * (asuint(r.y) + 1u)) >> 8;
}
