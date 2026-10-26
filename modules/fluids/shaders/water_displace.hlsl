// Aver.Fluids WaterSurface: the compute pass that displaces a flat water grid into a real mesh.
//
// The vertices it writes are rhi::MeshVertex (position, normal, uv0 -- 32 bytes), the buffer
// createSkinTargetMesh hands out, so the water is ordinary geometry to the rasteriser AND to the
// ray-traced primary visibility: both see the identical waves because there is only one copy of
// them, in this buffer.
//
// THE WAVE MATHS IS aver::fluids::averGerstnerDisplace (water.hlsl, the analytic ocean) AND
// GerstnerWave.hpp's gerstnerDisplaceCm, ported by hand a third time -- there is no shared source
// between HLSL files. Change one, change all three. Axis note as in water.hlsl: the engine is +Z-up,
// so the grid plane is XY and "dir.y" below is the engine Y component.
//
// Positions are written MESH-LOCAL about the grid origin (the host draws with a translation of
// {origin.xy, level}), which keeps float precision near the vertices and gives the blended flush
// a true position to sort by. Normals need no offset. UVs are the UNDISPLACED world XY, so a
// texture stays glued to the lattice as a camera-following grid is re-centred.

#ifndef AVER_MAX_WAVES
#define AVER_MAX_WAVES 4
#endif

static const float kPi = 3.14159265;

struct WaterVertex { float3 pos; float3 nrm; float2 uv; };

RWStructuredBuffer<WaterVertex> gOut : register(u0);

// Root CBV at b3. MIRRORS WaterDisplaceCB in WaterSurface.cpp field for field.
cbuffer WaterDisplace : register(b3) {
    float4 gWaveDirSteep[AVER_MAX_WAVES];   // {dirX, dirY, steepness, unused}; dir not pre-normalised
    float4 gWaveLenAmp[AVER_MAX_WAVES];     // {wavelengthCm, amplitudeCm, phaseOffset (radians, floating origin), unused}
    float4 gOriginTime;                     // {originX, originY (world cm), seconds, waveCount}
    float4 gGrid;                           // {cellX cm, cellY cm, uv per cm, skirt scale}
    uint4  gDims;                           // {vertsX, vertsY, skirt vertex count, skirt rings}
};

[numthreads(64, 1, 1)]
void CSWaterDisplace(uint3 tid : SV_DispatchThreadID) {
    const uint v = tid.x;
    const uint vx = gDims.x, vy = gDims.y;
    const uint grid = vx * vy;
    if (v >= grid + gDims.z) return;

    // Horizon rings: grid boundary vertex k pushed out by ring r's scale, flat (buildWaterGrid's order).
    if (v >= grid) {
        const uint m = vx - 1, ringVerts = 4 * m;
        const uint r = (v - grid) / ringVerts + 1, k = (v - grid) % ringVerts, side = k / m, t = k % m;
        const uint bx = side == 0 ? t : (side == 1 ? m : (side == 2 ? m - t : 0));
        const uint by = side == 0 ? 0 : (side == 1 ? t : (side == 2 ? m : m - t));
        const float2 edge = float2((float(bx) - 0.5 * float(m)) * gGrid.x, (float(by) - 0.5 * float(vy - 1)) * gGrid.y);
        const float scale = pow(gGrid.w, float(r) / float(max(gDims.w, 1u)));
        WaterVertex o;
        o.pos = float3(edge * scale, 0.0);
        o.nrm = float3(0.0, 0.0, 1.0);
        o.uv  = (edge * scale + gOriginTime.xy) * gGrid.z;
        gOut[v] = o;
        return;
    }

    const uint ix = v % vx;
    const uint iy = v / vx;
    // Centred on the origin: vertex (vx-1)/2 sits on it, so a re-centre by whole cells moves every
    // vertex onto another lattice point.
    const float2 local = float2((float(ix) - 0.5 * float(vx - 1)) * gGrid.x,
                                (float(iy) - 0.5 * float(vy - 1)) * gGrid.y);
    // With a horizon ring the waves fade out over the grid's outer 15%, so its boundary is flat.
    float fade = 1.0;
    if (gDims.z > 0) {
        const float2 half = 0.5 * float2(float(vx - 1) * gGrid.x, float(vy - 1) * gGrid.y);
        const float edge = max(abs(local.x) / half.x, abs(local.y) / half.y);
        fade = saturate((1.0 - edge) / 0.15);
    }
    const float2 world = local + gOriginTime.xy;
    const float t = gOriginTime.z;
    const uint waveCount = uint(gOriginTime.w + 0.5);

    float dispX = 0.0, dispY = 0.0, height = 0.0;
    float dHdx = 0.0, dHdy = 0.0, qWaSin = 0.0;
    const float qiMax = waveCount > 0 ? 1.0 / float(waveCount) : 0.0;

    [loop] for (uint i = 0; i < AVER_MAX_WAVES; ++i) {
        if (i >= waveCount) break;

        float2 dir = gWaveDirSteep[i].xy;
        const float dirLenSq = dot(dir, dir);
        dir = dirLenSq > 1e-12 ? dir * rsqrt(dirLenSq) : float2(0.0, 0.0);

        const float wavelengthCm = max(gWaveLenAmp[i].x, 1e-3);
        const float amplitudeCm = gWaveLenAmp[i].y * fade;
        const float omega = 2.0 * kPi / wavelengthCm;
        // Deep-water dispersion, g = 981 cm/s^2 (kGravityCmPerS2).
        const float phaseSpeed = sqrt(981.0 * omega);
        // gWaveLenAmp[i].z: the floating origin's f64-reduced phase offset (WaterSurface.cpp), 0 at origin zero.
        const float phase = omega * dot(dir, world) + phaseSpeed * t + gWaveLenAmp[i].z;
        const float s = sin(phase), c = cos(phase);
        const float qi = clamp(gWaveDirSteep[i].z, 0.0, qiMax);

        dispX  += qi * amplitudeCm * dir.x * c;
        dispY  += qi * amplitudeCm * dir.y * c;
        height += amplitudeCm * s;

        const float wa = omega * amplitudeCm;
        dHdx   += wa * dir.x * c;
        dHdy   += wa * dir.y * c;
        qWaSin += qi * wa * s;
    }

    WaterVertex o;
    o.pos = float3(local.x + dispX, local.y + dispY, height);
    o.nrm = normalize(float3(-dHdx, -dHdy, 1.0 - qWaSin));
    o.uv  = world * gGrid.z;
    gOut[v] = o;
}
