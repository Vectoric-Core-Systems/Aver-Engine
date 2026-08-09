// Ray-vs-heightfield picking: see HeightfieldRay.hpp.
#include "aver/landscape/HeightfieldRay.hpp"

#include <algorithm>
#include <cmath>

namespace aver::landscape {
namespace {

// Bilinearly interpolated height at a world (wx, wy). Clamped to the section's own edge samples
// outside the grid -- a ray that clips a corner still gets a sane answer instead of a stray read.
f32 sampleHeight(const fmt::OcLandData& d, f32 wx, f32 wy) {
    const f32 maxIdx = static_cast<f32>(d.sampleCount - 1);
    f32 fx = (wx - d.originCm[0]) / d.spacingCm;
    f32 fy = (wy - d.originCm[1]) / d.spacingCm;
    fx = std::clamp(fx, 0.0f, maxIdx);
    fy = std::clamp(fy, 0.0f, maxIdx);

    u32 ix0 = static_cast<u32>(fx), iy0 = static_cast<u32>(fy);
    if (d.sampleCount >= 2) {
        if (ix0 > d.sampleCount - 2) ix0 = d.sampleCount - 2;
        if (iy0 > d.sampleCount - 2) iy0 = d.sampleCount - 2;
    } else {
        ix0 = 0; iy0 = 0;
    }
    const f32 tx = fx - static_cast<f32>(ix0), ty = fy - static_cast<f32>(iy0);

    const f32 h00 = d.heightAt(ix0, iy0),     h10 = d.heightAt(ix0 + 1, iy0);
    const f32 h01 = d.heightAt(ix0, iy0 + 1), h11 = d.heightAt(ix0 + 1, iy0 + 1);
    const f32 h0 = h00 + (h10 - h00) * tx;
    const f32 h1 = h01 + (h11 - h01) * tx;
    return h0 + (h1 - h0) * ty;
}

} // namespace

bool raycastHeightfield(const fmt::OcLandData& d, const f32 ro[3], const f32 rd[3],
                         HeightfieldHit& out, f32 maxDistCm) {
    if (!d.valid() || d.sampleCount < 2 || maxDistCm <= 0.0f) return false;

    const f32 len = std::sqrt(rd[0]*rd[0] + rd[1]*rd[1] + rd[2]*rd[2]);
    if (len < 1e-8f) return false;
    const f32 dir[3] = {rd[0]/len, rd[1]/len, rd[2]/len};

    const f32 minX = d.originCm[0], maxX = d.originCm[0] + d.extentCm();
    const f32 minY = d.originCm[1], maxY = d.originCm[1] + d.extentCm();

    // Slab-clip [0, maxDistCm] to where the ray is over the section's footprint at all.
    f32 tEnter = 0.0f, tExit = maxDistCm;
    auto clipAxis = [&](f32 o, f32 dr, f32 lo, f32 hi) -> bool {
        if (std::fabs(dr) < 1e-9f) return o >= lo && o <= hi;
        f32 t0 = (lo - o) / dr, t1 = (hi - o) / dr;
        if (t0 > t1) std::swap(t0, t1);
        tEnter = std::fmax(tEnter, t0);
        tExit  = std::fmin(tExit, t1);
        return true;
    };
    if (!clipAxis(ro[0], dir[0], minX, maxX)) return false;
    if (!clipAxis(ro[1], dir[1], minY, maxY)) return false;
    if (tEnter >= tExit) return false;

    // Half a sample apart: coarse enough to stay cheap over a whole section, fine enough that a
    // single-sample spike cannot be stepped clean over.
    const f32 step = std::fmax(d.spacingCm * 0.5f, 1.0f);

    auto diffAt = [&](f32 t) {
        const f32 wx = ro[0] + dir[0]*t, wy = ro[1] + dir[1]*t, wz = ro[2] + dir[2]*t;
        return wz - sampleHeight(d, wx, wy);
    };

    f32 prevT = tEnter;
    f32 prevDiff = diffAt(prevT);
    for (f32 t = tEnter + step; ; t += step) {
        const f32 ct = std::fmin(t, tExit);
        const f32 diff = diffAt(ct);

        const bool crossed = (prevDiff >= 0.0f && diff <= 0.0f) || (prevDiff <= 0.0f && diff >= 0.0f);
        if (crossed && (prevDiff != 0.0f || diff != 0.0f)) {
            // Bisect: 24 halvings of a half-spacing bracket is well under a hundredth of a
            // centimetre, far finer than the quantisation the format itself stores heights at.
            f32 a = prevT, b = ct, fa = prevDiff;
            f32 mid = b;
            for (int i = 0; i < 24; ++i) {
                mid = 0.5f * (a + b);
                const f32 fm = diffAt(mid);
                if ((fa > 0.0f) == (fm > 0.0f)) { a = mid; fa = fm; } else { b = mid; }
            }
            out.distCm = mid;
            out.posCm[0] = ro[0] + dir[0]*mid;
            out.posCm[1] = ro[1] + dir[1]*mid;
            out.posCm[2] = sampleHeight(d, out.posCm[0], out.posCm[1]);
            return true;
        }

        prevT = ct; prevDiff = diff;
        if (ct >= tExit) break;
    }
    return false;
}

} // namespace aver::landscape
