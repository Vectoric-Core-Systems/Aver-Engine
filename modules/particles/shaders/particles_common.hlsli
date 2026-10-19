// Aver.Particles: the simulation's shared HLSL. A line-for-line twin of ParticleRandom.hpp (the counter-based RNG and the
// lattice noise), EmitterSim.cpp (shape, cone, spawn) and ParticleModules.cpp (the 13 modules). Change one, change the
// other: ParticleModulesTest pins the C++ and ParticleGpuParityTest steps both on a device and compares them.
//
// Everything here is 32-bit integer and float math, groupshared and plain structured buffers (no wave intrinsics, no
// 64-bit atomics), so the same file compiles for D3D12 (DXIL) and Vulkan (SPIR-V).
#ifndef AVER_PARTICLES_COMMON_HLSLI
#define AVER_PARTICLES_COMMON_HLSLI

#define P_PI     3.14159265358979323846
#define P_TWO_PI 6.28318530717958647692
#define P_DEG2RAD 0.01745329251994329577

// RandStream ids (ParticleRandom.hpp).
#define PSTREAM_SHAPE_A   0u
#define PSTREAM_SHAPE_B   1u
#define PSTREAM_SHAPE_C   2u
#define PSTREAM_CONE_COS  3u
#define PSTREAM_CONE_PHI  4u
#define PSTREAM_SPEED     5u
#define PSTREAM_LIFETIME  6u
#define PSTREAM_START_FRAME 7u
#define PSTREAM_SIZE_SCALE 8u
#define PSTREAM_TINT      9u
#define PSTREAM_ROTATION  10u
#define PSTREAM_SPIN      11u

// ModuleKind (ParticleTypes.hpp) and ModuleStage.
#define PMOD_SIZESCALE 0u
#define PMOD_COLORTINT 1u
#define PMOD_ROTATION 2u
#define PMOD_INHERITVEL 3u
#define PMOD_FORCE 4u
#define PMOD_DRAG 5u
#define PMOD_ATTRACTOR 6u
#define PMOD_VORTEX 7u
#define PMOD_CURLNOISE 8u
#define PMOD_SPEEDLIMIT 9u
#define PMOD_COLLIDEPLANE 10u
#define PMOD_COLLIDESPHERE 11u
#define PMOD_KILLVOLUME 12u

#define PFLAG_COLLIDED 1u

// GpuSimHeader.flags
#define PGPU_FLAG_LINKED    1u
#define PGPU_FLAG_TRAIL     2u
#define PGPU_FLAG_DEATH_EVT 4u
#define PGPU_FLAG_COLLIDE_EVT 8u
#define PGPU_FLAG_SUBUV_START 16u
#define PGPU_FLAG_HAS_EVENTS 32u

// counters[]
#define PCTR_DEAD 0u
#define PCTR_ALIVE 1u
#define PCTR_ALIVE_NEXT 2u
#define PCTR_STAT_ALIVE 3u
#define PCTR_SPAWNED 4u
#define PCTR_DROPPED 5u
#define PCTR_EV_TOTAL 6u
#define PCTR_EV_ID_BASE 7u
#define PCTR_EV_ID_THIS 8u

// args[]
#define PARG_DRAW 0u
#define PARG_SPAWN 8u
#define PARG_UPDATE 12u

// Events buffer (ParticleGpuTypes.hpp): header uints then death records then collide records, eight uints each.
#define PEV_CAP 2048u
#define PEV_REC 8u
#define PEV_HEADER 4u

// ParticleTypes.hpp Particle: 64 bytes, identical layout.
struct PParticle {
    float3 position; float age;
    float3 velocity; float lifetime;
    uint seed; uint id; float rotation; float angularVelocity;
    float sizeScale; uint tint; uint spawnInfo; uint trail;
};

// GpuSimHeader (ParticleGpuTypes.hpp): twelve float4 rows, no member straddling a row.
struct PSimHeader {
    float dt; float time; uint capacity; uint spawnTotal;
    uint firstId; uint emitterSeed; uint requestCount; uint moduleCount;
    float3 gravity; float damping;
    float3 origin; uint flags;
    uint shape; uint shell; uint aliveRead; uint aliveWrite;
    float3 shapeSize; float spreadDeg;
    float3 direction; float speedMin;
    float speedMax; float lifeMin; float lifeMax; uint trailRing;
    float trailSegment; uint subuvFrames; float subuvFps; uint pad8;
    uint evType; uint evCount; float evInherit; uint evActive;
    uint drawMult; uint indexCount; uint sortEnabled; uint evWriteBase;
    uint sortedBase; uint evReadBase; uint pad11a; uint pad11b;
};

// ---------------------------------------------------------------------------------------------------------- RNG
uint pHash32(uint x) {
    uint z = x + 0x9E3779B9u;
    z = (z ^ (z >> 16)) * 0x21F0AAADu;
    z = (z ^ (z >> 15)) * 0x735A2D97u;
    return z ^ (z >> 15);
}
float pRand01(uint seed, uint stream) {
    return (float)(pHash32(seed + 0x9E3779B9u * (stream + 1u)) >> 8) * (1.0 / 16777216.0);
}
uint pParticleSeed(uint emitterSeed, uint id) { return pHash32(emitterSeed ^ (id * 0x85EBCA6Bu)); }

// ---------------------------------------------------------------------------------------------------------- noise
static const float3 kPGrad[12] = {
    float3(1, 1, 0), float3(-1, 1, 0), float3(1, -1, 0), float3(-1, -1, 0), float3(1, 0, 1), float3(-1, 0, 1),
    float3(1, 0, -1), float3(-1, 0, -1), float3(0, 1, 1), float3(0, -1, 1), float3(0, 1, -1), float3(0, -1, -1)
};
float pNoiseCorner(int ix, int iy, int iz, float3 f, uint seed) {
    uint h = pHash32(pHash32(pHash32(seed + (uint)ix) + (uint)iy) + (uint)iz);
    return dot(kPGrad[(h >> 8) % 12u], f);
}
float pNoiseFade(float t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
float pGradNoise3(float3 p, uint seed) {
    float3 fl = floor(p);
    int ix = (int)fl.x, iy = (int)fl.y, iz = (int)fl.z;
    float3 f = p - fl;
    float u = pNoiseFade(f.x), v = pNoiseFade(f.y), w = pNoiseFade(f.z);
    float c000 = pNoiseCorner(ix,     iy,     iz,     f,                         seed);
    float c100 = pNoiseCorner(ix + 1, iy,     iz,     f - float3(1, 0, 0), seed);
    float c010 = pNoiseCorner(ix,     iy + 1, iz,     f - float3(0, 1, 0), seed);
    float c110 = pNoiseCorner(ix + 1, iy + 1, iz,     f - float3(1, 1, 0), seed);
    float c001 = pNoiseCorner(ix,     iy,     iz + 1, f - float3(0, 0, 1), seed);
    float c101 = pNoiseCorner(ix + 1, iy,     iz + 1, f - float3(1, 0, 1), seed);
    float c011 = pNoiseCorner(ix,     iy + 1, iz + 1, f - float3(0, 1, 1), seed);
    float c111 = pNoiseCorner(ix + 1, iy + 1, iz + 1, f - float3(1, 1, 1), seed);
    float x00 = c000 + (c100 - c000) * u, x10 = c010 + (c110 - c010) * u;
    float x01 = c001 + (c101 - c001) * u, x11 = c011 + (c111 - c011) * u;
    float y0 = x00 + (x10 - x00) * v, y1 = x01 + (x11 - x01) * v;
    return y0 + (y1 - y0) * w;
}
float pPotential(uint channel, float3 n, int octaves, uint seed) {
    float sum = 0.0, amp = 1.0, freq = 1.0;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * pGradNoise3(n * freq, seed + channel);
        amp *= 0.5;
        freq *= 2.0;
    }
    return sum;
}
float3 pCurlNoiseField(float3 worldPos, float freq, float time, float scroll, int octaves, uint seed) {
    int oct = clamp(octaves, 1, 4);
    uint ns = pHash32(seed);
    float3 n = float3(worldPos.x * freq, worldPos.y * freq, worldPos.z * freq + scroll * time);
    const float e = 0.01, inv2e = 0.5 / e;
    float3 c;
    c.x = (pPotential(2u, n + float3(0, e, 0), oct, ns) - pPotential(2u, n - float3(0, e, 0), oct, ns)
         - (pPotential(1u, n + float3(0, 0, e), oct, ns) - pPotential(1u, n - float3(0, 0, e), oct, ns))) * inv2e;
    c.y = (pPotential(0u, n + float3(0, 0, e), oct, ns) - pPotential(0u, n - float3(0, 0, e), oct, ns)
         - (pPotential(2u, n + float3(e, 0, 0), oct, ns) - pPotential(2u, n - float3(e, 0, 0), oct, ns))) * inv2e;
    c.z = (pPotential(1u, n + float3(e, 0, 0), oct, ns) - pPotential(1u, n - float3(e, 0, 0), oct, ns)
         - (pPotential(0u, n + float3(0, e, 0), oct, ns) - pPotential(0u, n - float3(0, e, 0), oct, ns))) * inv2e;
    return c;
}

// ---------------------------------------------------------------------------------------------------------- spawn
uint pPackRgba8(float4 c) {
    uint4 q = (uint4)(saturate(c) * 255.0 + 0.5);
    return q.x | (q.y << 8) | (q.z << 16) | (q.w << 24);
}

float3 pTransformPoint(float4 r0, float4 r1, float4 r2, float4 r3, float3 p) {
    return p.x * r0.xyz + p.y * r1.xyz + p.z * r2.xyz + r3.xyz;
}
float3 pTransformDir(float4 r0, float4 r1, float4 r2, float3 d) {
    return d.x * r0.xyz + d.y * r1.xyz + d.z * r2.xyz;
}

float3 pShapeOffset(PSimHeader h, uint seed) {
    float a = pRand01(seed, PSTREAM_SHAPE_A), b = pRand01(seed, PSTREAM_SHAPE_B), c = pRand01(seed, PSTREAM_SHAPE_C);
    if (h.shape == 1u) {   // Sphere
        float z = 2.0 * a - 1.0;
        float phi = P_TWO_PI * b;
        float s = sqrt(max(0.0, 1.0 - z * z));
        float r = (h.shell != 0u) ? 1.0 : pow(c, 1.0 / 3.0);
        float k = r * h.shapeSize.x;
        return float3(s * cos(phi) * k, s * sin(phi) * k, z * k);
    }
    if (h.shape == 2u) {   // Box
        return float3(h.shapeSize.x > 0.0 ? (2.0 * a - 1.0) * h.shapeSize.x : 0.0,
                      h.shapeSize.y > 0.0 ? (2.0 * b - 1.0) * h.shapeSize.y : 0.0,
                      h.shapeSize.z > 0.0 ? (2.0 * c - 1.0) * h.shapeSize.z : 0.0);
    }
    if (h.shape == 3u) {   // Cylinder
        float phi = P_TWO_PI * a;
        float r = ((h.shell != 0u) ? 1.0 : sqrt(b)) * h.shapeSize.x;
        return float3(cos(phi) * r, sin(phi) * r, (2.0 * c - 1.0) * 0.5 * h.shapeSize.y);
    }
    if (h.shape == 4u) {   // Disc
        float phi = P_TWO_PI * a;
        float r = ((h.shell != 0u) ? 1.0 : sqrt(b)) * h.shapeSize.x;
        return float3(cos(phi) * r, sin(phi) * r, 0.0);
    }
    return float3(0, 0, 0);
}

float3 pConeSample(float3 axis, float spreadDeg, uint seed) {
    float spreadRad = spreadDeg * (float)P_DEG2RAD;
    float cosSpread = cos(clamp(spreadRad, 0.0, (float)P_PI));
    float cosTheta = lerp(cosSpread, 1.0, pRand01(seed, PSTREAM_CONE_COS));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float phi = P_TWO_PI * pRand01(seed, PSTREAM_CONE_PHI);
    float3 hint = abs(axis.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 right = cross(hint, axis);
    float rl = length(right);
    right = rl > 1e-4 ? right / rl : float3(1, 0, 0);
    float3 up = cross(axis, right);
    return axis * cosTheta + right * (sinTheta * cos(phi)) + up * (sinTheta * sin(phi));
}

// ---------------------------------------------------------------------------------------------------------- modules
struct PMod { uint kind; uint stage; float p[8]; };

void pRunSpawnModule(PMod m, inout PParticle p, float3 emitterVelocity) {
    if (m.kind == PMOD_SIZESCALE) {
        p.sizeScale *= lerp(m.p[0], m.p[1], pRand01(p.seed, PSTREAM_SIZE_SCALE));
    } else if (m.kind == PMOD_COLORTINT) {
        float t = pRand01(p.seed, PSTREAM_TINT);
        p.tint = pPackRgba8(float4(lerp(m.p[0], m.p[3], t), lerp(m.p[1], m.p[4], t), lerp(m.p[2], m.p[5], t), 1.0));
    } else if (m.kind == PMOD_ROTATION) {
        p.rotation = lerp(m.p[0], m.p[1], pRand01(p.seed, PSTREAM_ROTATION)) * (float)P_DEG2RAD;
        p.angularVelocity = lerp(m.p[2], m.p[3], pRand01(p.seed, PSTREAM_SPIN)) * (float)P_DEG2RAD;
    } else if (m.kind == PMOD_INHERITVEL) {
        p.velocity += emitterVelocity * m.p[0];
    }
}

void pRunVelocityModule(PMod m, float3 origin, float time, float dt, inout PParticle p) {
    if (m.kind == PMOD_FORCE) {
        p.velocity += float3(m.p[0], m.p[1], m.p[2]) * dt;
    } else if (m.kind == PMOD_DRAG) {
        float speed = length(p.velocity);
        float keep = 1.0 - min(speed * m.p[0] * dt, 1.0);
        p.velocity *= max(keep, 0.0);
    } else if (m.kind == PMOD_ATTRACTOR) {
        float3 target = float3(m.p[0], m.p[1], m.p[2]) + (m.p[6] != 0.0 ? origin : float3(0, 0, 0));
        float3 d = target - p.position;
        float len = length(d);
        if (len > 1e-4) {
            float w = 1.0;
            float radius = m.p[4];
            if (radius > 0.0) w = len >= radius ? 0.0 : pow(1.0 - len / radius, max(m.p[5], 0.0));
            p.velocity += d * (m.p[3] * w / len * dt);
        }
    } else if (m.kind == PMOD_VORTEX) {
        float3 ax = float3(m.p[0], m.p[1], m.p[2]);
        float al = length(ax);
        if (al * al >= 1e-8) {
            float3 axis = ax / al;
            float3 r = p.position - origin;
            float3 rp = r - axis * dot(r, axis);
            float rl = length(rp);
            if (rl > 1e-4) {
                float radius = m.p[4];
                float w = radius > 0.0 ? saturate(1.0 - rl / radius) : 1.0;
                float3 tangent = cross(axis, rp) * (1.0 / rl);
                p.velocity += (tangent * (m.p[3] * w) + axis * m.p[5]) * dt;
            }
        }
    } else if (m.kind == PMOD_CURLNOISE) {
        float3 c = pCurlNoiseField(p.position, m.p[0], time, m.p[2], (int)m.p[3], (uint)((int)m.p[4]));
        p.velocity += c * (m.p[1] * dt);
    } else if (m.kind == PMOD_SPEEDLIMIT) {
        float speed = length(p.velocity);
        if (speed > 1e-6) {
            if (m.p[0] > 0.0 && speed > m.p[0]) p.velocity *= m.p[0] / speed;
            else if (m.p[1] > 0.0 && speed < m.p[1]) p.velocity *= m.p[1] / speed;
        }
    }
}

void pBounce(inout PParticle p, float3 n, float restitution, float friction) {
    float vn = dot(p.velocity, n);
    if (vn >= 0.0) return;
    float3 vt = p.velocity - n * vn;
    p.velocity = vt * (1.0 - saturate(friction)) - n * (vn * max(0.0, restitution));
}

// Bit 0: killed. Bit 1: the first collision of this particle.
uint pCollideResult(inout PParticle p, bool kill) {
    uint r = 0u;
    if (kill) { p.age = p.lifetime; r |= 1u; }
    uint flags = p.spawnInfo >> 16;
    if ((flags & PFLAG_COLLIDED) == 0u) {
        p.spawnInfo |= (PFLAG_COLLIDED << 16);
        r |= 2u;
    }
    return r;
}

uint pRunPositionModule(PMod m, inout PParticle p) {
    if (m.kind == PMOD_COLLIDEPLANE) {
        float3 nv = float3(m.p[0], m.p[1], m.p[2]);
        float nl = length(nv);
        if (nl * nl < 1e-8) return 0u;
        float3 n = nv / nl;
        float s = dot(n, p.position) - m.p[3];
        if (s >= 0.0) return 0u;
        p.position -= n * s;
        pBounce(p, n, m.p[4], m.p[5]);
        return pCollideResult(p, m.p[6] != 0.0);
    }
    if (m.kind == PMOD_COLLIDESPHERE) {
        float r = m.p[3];
        if (!(r > 0.0)) return 0u;
        float3 c = float3(m.p[0], m.p[1], m.p[2]);
        float3 d = p.position - c;
        float len = length(d);
        if (len >= r) return 0u;
        float3 n = len > 1e-5 ? d * (1.0 / len) : float3(0, 0, 1);
        p.position = c + n * r;
        pBounce(p, n, m.p[4], m.p[5]);
        return pCollideResult(p, m.p[6] != 0.0);
    }
    if (m.kind == PMOD_KILLVOLUME) {
        float3 d = p.position - float3(m.p[1], m.p[2], m.p[3]);
        bool inside;
        if (m.p[0] < 0.5) inside = abs(d.x) <= m.p[4] && abs(d.y) <= m.p[5] && abs(d.z) <= m.p[6];
        else inside = dot(d, d) <= m.p[4] * m.p[4];
        bool invert = m.p[7] != 0.0;
        if (inside != invert) { p.age = p.lifetime; return 1u; }
        return 0u;
    }
    return 0u;
}

#endif
