// See PhysicsBridge.hpp for why this is a transpose and a row flip rather than a copy.
#include "aver/landscape/PhysicsBridge.hpp"

namespace aver::landscape {

bool toPhysicsHeightfield(const fmt::OcLandData& d, PhysicsHeightfield& out) {
    if (!d.valid()) return false;
    const u32 n = d.sampleCount;

    out.sampleCount = n;
    out.spacingCm = d.spacingCm;
    out.samples.assign(static_cast<usize>(n) * n, 0.0f);

    // Solving the two placement equations in the header for the index map:
    //   physics column x -> engine +Y, so  x = iy
    //   physics row    y -> engine -X, so  y = (n-1) - ix
    // which is the transpose (x <-> y) composed with a flip of the new row axis.
    for (u32 iy = 0; iy < n; ++iy) {
        for (u32 ix = 0; ix < n; ++ix) {
            const u32 px = iy;
            const u32 py = (n - 1) - ix;
            out.samples[static_cast<usize>(py) * n + px] = d.heightAt(ix, iy);
        }
    }

    // The corner is the field's +X end, not its minimum: substituting the map above into
    // `cornerX - y*spacing` requires cornerX = origin.x + (n-1)*spacing for sample (0,0) to land on
    // origin.x. Getting this wrong shifts the whole collision surface by a section width, which is
    // exactly the kind of error that looks like a missing chunk rather than a wrong constant.
    out.cornerCm[0] = d.originCm[0] + d.extentCm();
    out.cornerCm[1] = d.originCm[1];
    // Heights already carry origin.z (OcLandData decodes it in), so the body's z offset is zero rather
    // than origin.z -- adding it here would apply it twice.
    out.cornerCm[2] = 0.0f;
    return true;
}

} // namespace aver::landscape
