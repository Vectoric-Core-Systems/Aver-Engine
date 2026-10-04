// Brush edits to a landscape section's heights: see Sculpt.hpp.
#include "aver/landscape/Sculpt.hpp"

#include "aver/landscape/TerrainNoise.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace aver::landscape {

// The inclusive sample-space rectangle a brush can reach, clamped to the section's grid.
BrushRect brushRect(const fmt::OcLandData& d, const BrushParams& p) {
    BrushRect r;
    if (!d.valid() || d.spacingCm <= 0.0f || p.radiusCm <= 0.0f) return r;

    const f32 minX = p.centerCm[0] - p.radiusCm, maxX = p.centerCm[0] + p.radiusCm;
    const f32 minY = p.centerCm[1] - p.radiusCm, maxY = p.centerCm[1] + p.radiusCm;
    const f32 gridMaxX = d.originCm[0] + d.extentCm(), gridMaxY = d.originCm[1] + d.extentCm();
    if (maxX < d.originCm[0] || minX > gridMaxX || maxY < d.originCm[1] || minY > gridMaxY)
        return r;   // the brush's box never touches the grid's box at all

    const i32 maxIdx = static_cast<i32>(d.sampleCount) - 1;
    auto toIndex = [&](f32 worldCoord, f32 originCoord, bool roundUp) -> i32 {
        const f32 f = (worldCoord - originCoord) / d.spacingCm;
        const f32 rounded = roundUp ? std::ceil(f) : std::floor(f);
        return std::clamp(static_cast<i32>(rounded), 0, maxIdx);
    };
    const i32 x0 = toIndex(minX, d.originCm[0], false);
    const i32 x1 = toIndex(maxX, d.originCm[0], true);
    const i32 y0 = toIndex(minY, d.originCm[1], false);
    const i32 y1 = toIndex(maxY, d.originCm[1], true);
    if (x1 < x0 || y1 < y0) return r;

    r.x0 = static_cast<u32>(x0); r.x1 = static_cast<u32>(x1);
    r.y0 = static_cast<u32>(y0); r.y1 = static_cast<u32>(y1);
    r.empty = false;
    return r;
}

// Applies one stroke tick in place. See Sculpt.hpp.
BrushRect applyBrush(fmt::OcLandData& d, const BrushParams& p, f32 amount) {
    const BrushRect r = brushRect(d, p);
    if (r.empty || amount <= 0.0f) return r;

    // Smooth reads its neighbours, so it needs a stable PRE-EDIT snapshot of the touched region,
    // padded by one sample so an edge cell's neighbours are pre-edit too -- otherwise a raster-order
    // sweep would blend some cells against values this very call already changed.
    std::vector<f32> snapshot;
    u32 sx0 = 0, sy0 = 0, sw = 0;
    if (p.mode == BrushMode::Smooth) {
        sx0 = r.x0 > 0 ? r.x0 - 1 : 0;
        sy0 = r.y0 > 0 ? r.y0 - 1 : 0;
        const u32 sx1 = std::min(r.x1 + 1, d.sampleCount - 1);
        const u32 sy1 = std::min(r.y1 + 1, d.sampleCount - 1);
        sw = sx1 - sx0 + 1;
        const u32 sh = sy1 - sy0 + 1;
        snapshot.resize(static_cast<usize>(sw) * sh);
        for (u32 y = sy0; y <= sy1; ++y)
            for (u32 x = sx0; x <= sx1; ++x)
                snapshot[static_cast<usize>(y - sy0) * sw + (x - sx0)] = d.heightAt(x, y);
    }
    auto snap = [&](u32 x, u32 y) { return snapshot[static_cast<usize>(y - sy0) * sw + (x - sx0)]; };

    bool changed = false;
    for (u32 y = r.y0; y <= r.y1; ++y) {
        for (u32 x = r.x0; x <= r.x1; ++x) {
            const f32 wx = d.originCm[0] + static_cast<f32>(x) * d.spacingCm;
            const f32 wy = d.originCm[1] + static_cast<f32>(y) * d.spacingCm;
            const f32 dx = wx - p.centerCm[0], dy = wy - p.centerCm[1];
            const f32 dist = std::sqrt(dx*dx + dy*dy);
            if (dist > p.radiusCm) continue;

            // Smoothstep falloff: 1 at the centre, 0 at the rim, and flat-topped near the centre --
            // unlike a linear falloff this does not leave a visible cone tip under the cursor.
            //
            // NOW SHAPED BY p.falloff, which was not previously expressible. The smoothstep is
            // evaluated over a REMAPPED radius: everything inside (1 - falloff) of the rim gets full
            // weight, and the shoulder is compressed into what remains. At falloff = 1 the remap is
            // the identity and this is bit-for-bit the original curve, which is what lets the new
            // parameter default to the old behaviour rather than changing every existing caller. At
            // falloff = 0 the shoulder has zero width and the brush is a hard disc.
            const f32 t    = dist / p.radiusCm;
            const f32 soft = p.falloff < 0.0f ? 0.0f : (p.falloff > 1.0f ? 1.0f : p.falloff);
            // The plateau ends at (1 - soft); past it, s runs 0..1 across the shoulder.
            const f32 s = soft <= 1e-4f ? (t >= 1.0f ? 1.0f : 0.0f)
                                        : (t <= 1.0f - soft ? 0.0f : (t - (1.0f - soft)) / soft);
            const f32 falloff = 1.0f - s*s*(3.0f - 2.0f*s);
            const f32 w = falloff * amount;
            if (w <= 0.0f) continue;

            f32& h = d.heights[static_cast<usize>(y) * d.sampleCount + x];
            switch (p.mode) {
                case BrushMode::Raise: h += p.strength * w; break;
                case BrushMode::Lower: h -= p.strength * w; break;
                case BrushMode::Flatten: {
                    const f32 rate = std::min(1.0f, w * 4.0f);   // converges over a few ticks, not one
                    h += (p.flattenTargetCm - h) * rate;
                    break;
                }
                case BrushMode::Smooth: {
                    const u32 xm = x > 0 ? x - 1 : x, xp = x + 1 < d.sampleCount ? x + 1 : x;
                    const u32 ym = y > 0 ? y - 1 : y, yp = y + 1 < d.sampleCount ? y + 1 : y;
                    const f32 avg = (snap(xm, y) + snap(xp, y) + snap(x, ym) + snap(x, yp) + snap(x, y)) / 5.0f;
                    const f32 rate = std::min(1.0f, w * 4.0f);
                    h = snap(x, y) + (avg - snap(x, y)) * rate;
                    break;
                }
                case BrushMode::Ramp: {
                    // Project this sample onto the axis from rampStartCm to the CURRENT centerCm and
                    // interpolate HEIGHT along that projection: t=0 at the start, t=1 at the current
                    // brush position, clamped beyond either end so the ramp does not extrapolate past
                    // its own endpoints. The radial weight `w` computed above (falloff + amount, from
                    // distance to centerCm) is UNCHANGED -- it still decides how localized to the
                    // brush footprint the edit is and how fast it converges, exactly the Flatten
                    // idiom, only the target height now varies along one axis instead of being the
                    // same constant everywhere.
                    const f32 ax = p.centerCm[0] - p.rampStartCm[0];
                    const f32 ay = p.centerCm[1] - p.rampStartCm[1];
                    const f32 axisLenSq = ax * ax + ay * ay;
                    // Named axisT, not t: the shared falloff block above already owns a `t`
                    // (distance / radius) that every brush mode uses, and shadowing it here was a
                    // real /W4 warning even though the two never mix.
                    f32 axisT = 0.0f;
                    if (axisLenSq > 1e-6f) {
                        axisT = ((wx - p.rampStartCm[0]) * ax + (wy - p.rampStartCm[1]) * ay) / axisLenSq;
                        axisT = std::clamp(axisT, 0.0f, 1.0f);
                    }
                    // lerp(rampStartHeightCm, rampStartHeightCm + strength, axisT): `strength` IS the
                    // ramp's total rise, per BrushParams::rampStartCm's own comment.
                    const f32 target = p.rampStartHeightCm + p.strength * axisT;
                    const f32 rate = std::min(1.0f, w * 4.0f);
                    h += (target - h) * rate;
                    break;
                }
                case BrushMode::Noise: {
                    // Deterministic per-stroke noise: TerrainNoise.hpp's ridged fBm is already a PURE
                    // function of world (x, y) plus a seed (no internal state, no clock read), which
                    // is exactly what "same stroke twice, identical result" needs -- reused rather
                    // than inventing a second noise function. amplitudeCm is fixed at 1.0 here (NOT
                    // p.strength) so the raw sample normalises to [-1, 1]; `w` (falloff * amount)
                    // then applies p.strength as the bound, the same way every other mode turns
                    // strength into a per-tick delta -- so Noise is bounded by p.strength exactly
                    // like Raise/Lower are, not by TerrainNoise's own unrelated amplitude knob.
                    // featureSizeCm scales with the BRUSH radius, not a fixed world size, so the
                    // pattern stays "a few ripples across the brush" (localized) at any brush size.
                    TerrainNoiseParams np;
                    np.seed = p.noiseSeed;
                    np.amplitudeCm = 1.0f;
                    np.featureSizeCm = std::max(p.radiusCm * 0.4f, 1.0f);
                    np.octaves = 4;
                    const f32 n = terrainHeightAt(wx, wy, np);
                    h += p.strength * n * w;
                    break;
                }
            }
            changed = true;
        }
    }

    if (changed) {
        f32 lo = d.heights[0], hi = d.heights[0];
        for (f32 v : d.heights) { lo = std::fmin(lo, v); hi = std::fmax(hi, v); }
        d.boundsMin[0] = d.originCm[0];
        d.boundsMin[1] = d.originCm[1];
        d.boundsMin[2] = lo;
        d.boundsMax[0] = d.originCm[0] + d.extentCm();
        d.boundsMax[1] = d.originCm[1] + d.extentCm();
        d.boundsMax[2] = hi;
    }
    return r;
}

} // namespace aver::landscape
