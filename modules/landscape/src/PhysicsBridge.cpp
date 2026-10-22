// Converts a landscape section into physics-order heightfield samples. See PhysicsBridge.hpp.
#include "aver/landscape/PhysicsBridge.hpp"

#include <algorithm>

namespace aver::landscape {

namespace {

constexpr u32 kMaxLayers = 8;

// The cell palette: each cell's dominant paint layer, as an index into the distinct surfaces of the section's layers.
void buildCellPalette(const fmt::OcLandData& d, const std::vector<i32>& layerSurface, PhysicsHeightfield& out) {
    const u32 n = d.sampleCount;
    const usize L = std::min<usize>(std::max<usize>(d.layerNames.size(), 1), kMaxLayers);
    if (layerSurface.size() < L || n < 2) return;
    bool any = false;
    for (usize s = 0; s < L; ++s) any = any || layerSurface[s] != 0;
    if (!any) return;

    // Section layer -> palette index (distinct surfaces, in layer order).
    std::vector<i32> palette;
    u8 paletteOf[kMaxLayers] = {};
    for (usize s = 0; s < L; ++s) {
        const auto it = std::find(palette.begin(), palette.end(), layerSurface[s]);
        paletteOf[s] = static_cast<u8>(it - palette.begin());
        if (it == palette.end()) palette.push_back(layerSurface[s]);
    }

    const bool painted = d.layersValid() && d.layerNames.size() >= 2;
    const usize nn = static_cast<usize>(n) * n;
    const u32 cells = n - 1;
    out.cellPalette.assign(static_cast<usize>(cells) * cells, 0);
    if (palette.size() > 1 && painted) {
        for (u32 cy = 0; cy < cells; ++cy) {
            // Physics row cy .. cy+1 is section column (n-2-cy) .. (n-1-cy); physics column cx .. cx+1 is section row cx .. cx+1.
            const u32 ix0 = n - 2 - cy, ix1 = n - 1 - cy;
            for (u32 cx = 0; cx < cells; ++cx) {
                const usize corner[4] = {static_cast<usize>(cx) * n + ix0, static_cast<usize>(cx) * n + ix1,
                                         static_cast<usize>(cx + 1) * n + ix0, static_cast<usize>(cx + 1) * n + ix1};
                int sum[kMaxLayers] = {};   // by palette index
                for (const usize at : corner) {
                    // The stored planes are layers 1..L-1; layer 0 is what is left of 255 (OcLandData::weightAt).
                    int stored = 0;
                    for (usize s = 1; s < L; ++s) {
                        const int w = d.weights[(s - 1) * nn + at];
                        stored += w;
                        sum[paletteOf[s]] += w;
                    }
                    sum[paletteOf[0]] += stored >= 255 ? 0 : 255 - stored;
                }
                u8 best = 0;
                for (usize p = 1; p < palette.size(); ++p)
                    if (sum[p] > sum[best]) best = static_cast<u8>(p);   // ties keep the lower index
                out.cellPalette[static_cast<usize>(cy) * cells + cx] = best;
            }
        }
    }
    out.palette = std::move(palette);
}

} // namespace

// Converts a section. Returns false only for a section that is not internally consistent.
bool toPhysicsHeightfield(const fmt::OcLandData& d, PhysicsHeightfield& out, const std::vector<i32>* sectionLayerSurface) {
    if (!d.valid()) return false;
    const u32 n = d.sampleCount;

    out.sampleCount = n;
    out.spacingCm = d.spacingCm;
    out.samples.assign(static_cast<usize>(n) * n, 0.0f);
    out.cellPalette.clear();
    out.palette.clear();

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

    if (sectionLayerSurface && !sectionLayerSurface->empty()) buildCellPalette(d, *sectionLayerSurface, out);
    return true;
}

} // namespace aver::landscape
