// voxi_neurac_io.hlsli -- the radiance cache's resource-bound half: the sample SCATTER (training)
// and the cell LOOKUP (read). Included ONLY by voxi_restir.hlsli, under `#if AVER_NEURAC`, AFTER
// voxi_neurac.hlsli and the three declarations it reads:
//   StructuredBuffer<RcInfo>     gRcInfo  : t22   (cascade origins, cell sizes, sample cap)
//   RWStructuredBuffer<int>      gRcAccum : u20   (fixed-point accumulator, layout in the pure file)
//   RWStructuredBuffer<RcCell>   gRcCells : u21   (resolved cells, read with plain UAV loads)
// and after the cbuffer field gAmbientParams. Cells are a UAV rather than an SRV so the staged lighting
// group stays barrier-free (UAV-only buffers need no state transition).
//
// WORLD UNITS: the cascades' cell sizes are in the engine's world unit (the cbuffer's gCamPos/hit
// positions), the same unit NeuRaC::beginFrame snaps the camera in.

// The lookup's last overall confidence (1 - the fraction left for the fallback), for the F2 path debug
// view. `static` hand-off like gGiPoisonPdfHit/gGiCbSkip; per invocation, never a resource.
static float gRcLastConf = 0.0;

// gAmbientParams.w bit 128 (givis::packAmbientW's neurac argument): the CPU sets it only on a frame
// the cache twin pipelines run with the cache live. A twin with it clear behaves as plain HalfResolution.
bool rcCacheOn() { return ((uint)gAmbientParams.w & 128u) != 0u; }

// ---- SCATTER: one second-bounce ray = one Monte Carlo sample of INCIDENT radiance at hitPos ----
// `indY` is that ray's result (sky on a miss, the voxel volume on a hit, 0 outside it -- F2's own
// semantics, so the cache inherits them). The ray was cosine-sampled about N, so its pdf is cos/PI with
// cos = cosDir2 and the unbiased projection weight is PI / cos; the floor at AVER_RC_MIN_COS under-weights
// the 1% grazing samples (P(cos < c) = c^2) instead of letting 1/cos blow the fixed-point headroom.
// Only the FINEST cascade containing hitPos trains. The count atomic comes first and is what enforces the
// per-cell cap: an over-cap sample costs one atomic, not sixteen.
void rcScatter(float3 hitPos, float3 N, float3 dir2, float3 indY, float cosDir2) {
    const float3 L = min(max(indY, 0.0), AVER_RC_LMAX);
    if (!all(isfinite(L)) || !all(isfinite(dir2)) || !all(isfinite(N))) return;
    const RcInfo info = gRcInfo[0];
    const uint cap = min(info.hdr0.w, (uint)AVER_RC_MAX_CAP);
    [unroll] for (uint c = 0; c < (uint)AVER_RC_CASCADES; ++c) {
        const float size = asfloat(info.cas[c].w);
        const int3  cell = (int3)floor(hitPos / size);
        const int3  rel  = cell - info.cas[c].xyz;
        if (all(rel >= 0) && all(rel < AVER_RC_RES)) {
            const uint idx = rcCellIndex(c, cell);
            int old;
            InterlockedAdd(gRcAccum[idx], 1, old);
            if ((uint)old >= cap) return;
            float y[4];
            rcShBasis(dir2, y);
            const float wgt  = 3.14159265 / max(cosDir2, AVER_RC_MIN_COS);
            const uint  base = (uint)AVER_RC_CELLS + idx * (uint)AVER_RC_PAYLOAD_INTS;
            int ignored;
            [unroll] for (int k = 0; k < 4; ++k) {
                const float3 t = L * (y[k] * wgt * AVER_RC_SH_SCALE);
                InterlockedAdd(gRcAccum[base + k * 3 + 0], (int)round(t.x), ignored);
                InterlockedAdd(gRcAccum[base + k * 3 + 1], (int)round(t.y), ignored);
                InterlockedAdd(gRcAccum[base + k * 3 + 2], (int)round(t.z), ignored);
            }
            const float3 nn = N * AVER_RC_N_SCALE;
            InterlockedAdd(gRcAccum[base + 12], (int)round(nn.x), ignored);
            InterlockedAdd(gRcAccum[base + 13], (int)round(nn.y), ignored);
            InterlockedAdd(gRcAccum[base + 14], (int)round(nn.z), ignored);
            return;
        }
    }
}

// ---- LOOKUP: cosine-convolved irradiance / PI at p for normal N, blended across cascades ----
// Per cascade (finest first): look half a cell out along N (the same idea as F2's shell-straddle fix),
// find the 8 surrounding cell centres, skip the cascade if any corner is within AVER_RC_MARGIN_CELLS of
// its window edge, and weight each VALID corner by trilinear * normal agreement * planarity *
// observation count * freshness. The cascade's value is the weight-normalised SH blend, its confidence
// the (<= 1) weight sum; a cascade contributes confidence * edge-fade of what is still `remaining`, so a
// cold or edge-adjacent fine cascade hands the rest to the next coarser one. What nothing covered is
// returned in `remaining` for the caller to fill with its own fallback (the F2 sky-ratio formula).
float3 rcLookup(float3 p, float3 N, out float remaining) {
    remaining = 1.0;
    float3 outV = 0.0;
    gRcLastConf = 0.0;
    const RcInfo info = gRcInfo[0];
    [loop] for (uint c = 0; c < (uint)AVER_RC_CASCADES; ++c) {
        if (remaining < 0.01) break;
        const float  size = asfloat(info.cas[c].w);
        const int3   org  = info.cas[c].xyz;
        const float3 q    = (p + N * (0.5 * size)) / size - (float3)org;   // position in window cells, [0, 64)
        const float3 g    = q - 0.5;
        const float3 fl   = floor(g);
        const float3 f    = g - fl;
        const int3   b    = (int3)fl;                                         // window-relative base corner
        // Both corners (b and b + 1) must sit at least MARGIN cells inside the window.
        if (any(b < AVER_RC_MARGIN_CELLS) || any(b > AVER_RC_RES - 2 - AVER_RC_MARGIN_CELLS)) continue;
        const float dEdge  = min(min(q.x, min(q.y, q.z)), min(AVER_RC_RES - q.x, min(AVER_RC_RES - q.y, AVER_RC_RES - q.z)));
        const float aEdge  = saturate((dEdge - (float)AVER_RC_MARGIN_CELLS) / (float)AVER_RC_CROSSFADE_CELLS);
        float3 acc[4] = { (float3)0, (float3)0, (float3)0, (float3)0 };
        float  wsum = 0.0;
        [unroll] for (int i = 0; i < 8; ++i) {
            const int3   o  = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
            const int3   wc = b + o + org;
            const float3 tw = lerp(1.0 - f, f, (float3)o);
            const float  tri = tw.x * tw.y * tw.z;
            const RcCell cell = gRcCells[rcCellIndex(c, wc)];
            const uint   meta = cell.b.w;
            const uint   neff = (meta >> 24) & 15u;
            const uint   age  = meta >> 28;
            if (neff == 0u || (meta & 0xFFFFFFu) != rcTagPack(wc) || age >= (uint)AVER_RC_AGE_MAX) continue;
            float len;
            const float3 cn = rcUnpackNormal(cell.b.z, len);
            const float  w  = tri * saturate(dot(cn, N) * AVER_RC_NORMAL_K) * len
                            * min((float)neff * 0.25, 1.0) * (1.0 - (float)age / (float)AVER_RC_AGE_MAX);
            if (w <= 0.0) continue;
            float3 cc[4];
            rcUnpackSh(cell.a, cell.b.xy, cc);
            [unroll] for (int k = 0; k < 4; ++k) acc[k] += w * cc[k];
            wsum += w;
        }
        if (wsum <= 1e-5) continue;
        float3 blended[4];
        [unroll] for (int k = 0; k < 4; ++k) blended[k] = acc[k] / wsum;
        const float3 value = rcShIrradianceOverPi(blended, N);
        const float  a     = aEdge * saturate(wsum);
        outV      += remaining * a * value;
        remaining *= 1.0 - a;
    }
    gRcLastConf = 1.0 - remaining;
    return outV;
}

// ---- VISUALISATION (gAmbientParams.w bits 8-10 mode, bit 11 cell grid; givis::packAmbientW) ----
// The editor's NeuRaC visualiser. giRestirIndirect returns this colour in place of indirect diffuse
// (after the denoiser-input write, like the path view, so it never enters history) and Stage B shows it
// as the pixel's final colour. Looked up at the PRIMARY surface: the cache is a world-space structure
// trained at second-bounce hits, and this shows what it holds where the camera can see.
//   1 cached light   -- rcLookup's irradiance/PI alone (no fallback): black where nothing is cached
//   2 coverage       -- the lookup's confidence: red (fallback does it all) -> green (cache does it all)
//   3 cascade        -- the finest cascade holding the point: cyan 25, yellow 100, orange 400 units
//   4 cell state     -- that cell: green = observations (n_eff), red = age; dark violet = empty or stale
uint rcViewMode() { return ((uint)gAmbientParams.w >> 8) & 7u; }
bool rcViewGrid() { return ((uint)gAmbientParams.w & 2048u) != 0u; }

float3 rcDebugColour(float3 p, float3 N) {
    const uint   mode = rcViewMode();
    const RcInfo info = gRcInfo[0];
    // The finest cascade whose window holds p (offset half a cell along N, as the lookup does).
    int    cas  = -1;
    float  size = 1.0;
    float3 qc   = 0.0;   // p in that cascade's cell units
    [loop] for (uint c = 0; c < (uint)AVER_RC_CASCADES; ++c) {
        const float  s = asfloat(info.cas[c].w);
        const float3 q = (p + N * (0.5 * s)) / s;
        const float3 rel = q - (float3)info.cas[c].xyz;
        if (all(rel >= (float)AVER_RC_MARGIN_CELLS) && all(rel < (float)(AVER_RC_RES - AVER_RC_MARGIN_CELLS))) {
            cas = (int)c; size = s; qc = q;
            break;
        }
    }
    float3 col = float3(0.25, 0.0, 0.0);   // outside every window
    if (mode == 1u || mode == 2u) {
        float rem;
        const float3 v = rcLookup(p, N, rem);
        col = mode == 1u ? v : lerp(float3(1.0, 0.08, 0.05), float3(0.1, 1.0, 0.2), 1.0 - rem);
    } else if (mode == 3u) {
        if (cas == 0) col = float3(0.1, 0.9, 1.0);
        else if (cas == 1) col = float3(1.0, 0.9, 0.1);
        else if (cas == 2) col = float3(1.0, 0.45, 0.05);
    } else if (mode == 4u && cas >= 0) {
        const int3   wc   = (int3)floor(qc);
        const RcCell cell = gRcCells[rcCellIndex((uint)cas, wc)];
        const uint   meta = cell.b.w;
        const uint   neff = (meta >> 24) & 15u;
        const uint   age  = meta >> 28;
        const bool   live = neff != 0u && (meta & 0xFFFFFFu) == rcTagPack(wc) && age < (uint)AVER_RC_AGE_MAX;
        col = live ? float3((float)age / (float)AVER_RC_AGE_MAX, (float)neff / (float)AVER_RC_NEFF_MAX, 0.1)
                   : float3(0.2, 0.0, 0.3);
    }
    // Cell edges: darken within 4% of a cell boundary on the two axes the surface runs along (the
    // axis closest to N would darken a whole face).
    if (rcViewGrid() && cas >= 0) {
        const float3 f  = frac(qc);
        const float3 d  = min(f, 1.0 - f);
        const float3 an = abs(N);
        const float  ex = an.x >= an.y && an.x >= an.z ? 1.0 : d.x;
        const float  ey = an.y > an.x && an.y >= an.z ? 1.0 : d.y;
        const float  ez = an.z > an.x && an.z > an.y ? 1.0 : d.z;
        if (min(ex, min(ey, ez)) < 0.04) col *= 0.2;
    }
    return col;
}
