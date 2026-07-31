// Converts a landscape section into physics-order heightfield samples. See PhysicsBridge.hpp.
#include "aver/landscape/PhysicsBridge.hpp"

namespace aver::landscape {

// Converts a section. Returns false only for a section that is not internally consistent.
bool toPhysicsHeightfield(const fmt::OcLandData& d, PhysicsHeightfield& out) {
    if (!d.valid()) return false;
    const u32 n = d.sampleCount;

    out.sampleCount = n;
    out.spacingCm = d.spacingCm;
    out.samples.assign(static_cast<usize>(n) * n, 0.0f);

    // The index map: physics column x = iy (engine +Y), physics row y = (n-1) - ix (engine -X).
    for (u32 iy = 0; iy < n; ++iy) {
        for (u32 ix = 0; ix < n; ++ix) {
            const u32 px = iy;
            const u32 py = (n - 1) - ix;
            out.samples[static_cast<usize>(py) * n + px] = d.heightAt(ix, iy);
        }
    }

    // The corner is the field's +X end, not its minimum.
    out.cornerCm[0] = d.originCm[0] + d.extentCm();
    out.cornerCm[1] = d.originCm[1];
    // Heights already carry origin.z, so the body's z offset is zero.
    out.cornerCm[2] = 0.0f;
    return true;
}

} // namespace aver::landscape
