// ColourVision.hpp (no device): every matrix row sums to 1, grey is grey, strength 0 is the identity, correction moves
// colours the way the Fidaner shift says, simulation is idempotent in linear space, output clamps to [0,1], the packed
// float round-trips, and the post constants carry it (PostSettings -> fillPostLens -> PostCB::access) while the defaults
// leave every row zero. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/rhi/ColourVision.hpp"
#include "aver/rhi/FrameConstants.hpp"
#include "aver/rhi/RHI.hpp"

#include <cmath>
#include <string>

using namespace aver;
using namespace aver::rhi;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

static f32 toLin(f32 v) { return std::pow(v, 2.2f); }

int main() {
    AVER_INFO("ColourVisionTest");

    // ---- the matrices ----
    {
        bool rows = true;
        for (int m = 0; m < 3; ++m)
            for (int r = 0; r < 3; ++r) {
                const f32 sum = kColourVisionMatrix[m][r][0] + kColourVisionMatrix[m][r][1] + kColourVisionMatrix[m][r][2];
                if (!near(sum, 1.0f, 1e-5f)) { rows = false; AVER_ERROR("   matrix {} row {} sums to {}", m, r, sum); }
            }
        check(rows, "every matrix row sums to 1 within 1e-5 (grey is preserved; a mistyped digit shows here)");
    }

    // ---- packing ----
    {
        check(packColourVision(0, true, 1.0f) == 0.0f && packColourVision(4, false, 1.0f) == 0.0f, "mode Off or out of range packs to 0");
        const f32 p = packColourVision(2, true, 0.37f);
        check(colourVisionMode(p) == 2 && colourVisionSimulate(p) && near(colourVisionStrength(p), 0.37f, 1e-6f), "mode, simulate and strength round-trip (strength to 1/100)");
        check(colourVisionMode(packColourVision(3, false, 1.0f)) == 3 && !colourVisionSimulate(packColourVision(3, false, 1.0f)), "simulate off round-trips");
        check(packColourVision(1, false, 2.0f) == packColourVision(1, false, 1.0f), "strength clamps to 1");
        PostSettings ps;
        check(packColourVision(ps) == 0.0f, "default PostSettings pack to 0");
        ps.colourVision = 3u | 4u;
        ps.colourVisionStrength = 0.5f;
        check(packColourVision(ps) == packColourVision(3, true, 0.5f), "and PostSettings::colourVision is mode | simulate bit");
    }

    // ---- grey, strength 0, clamping ----
    {
        bool greyOk = true, idOk = true, clampOk = true;
        for (u32 mode = 1; mode <= 3; ++mode)
            for (int sim = 0; sim < 2; ++sim) {
                for (f32 g : {0.0f, 0.1f, 0.5f, 0.9f, 1.0f}) {
                    const f32 in[3] = {g, g, g};
                    f32 out[3];
                    applyColourVision(packColourVision(mode, sim != 0, 1.0f), in, out);
                    if (!near(out[0], g, 2e-3f) || !near(out[1], g, 2e-3f) || !near(out[2], g, 2e-3f)) greyOk = false;
                }
                const f32 col[3] = {0.8f, 0.2f, 0.4f};
                f32 z[3];
                applyColourVision(packColourVision(mode, sim != 0, 0.0f), col, z);
                for (int k = 0; k < 3; ++k) if (!near(z[k], col[k], 1e-4f)) idOk = false;
                const f32 hot[3] = {1.0f, 0.0f, 1.0f};
                f32 h[3];
                applyColourVision(packColourVision(mode, sim != 0, 1.0f), hot, h);
                for (int k = 0; k < 3; ++k) if (h[k] < 0.0f || h[k] > 1.0f) clampOk = false;
            }
        check(greyOk, "grey in = grey out for every mode, simulation and correction");
        check(idOk, "strength 0 is the identity");
        check(clampOk, "output stays inside [0,1] even for saturated input");
        const f32 in[3] = {0.3f, 0.6f, 0.9f};
        f32 out[3];
        applyColourVision(0.0f, in, out);
        check(out[0] == in[0] && out[1] == in[1] && out[2] == in[2], "packed 0 (off) is exactly the identity");
    }

    // ---- correction moves colour toward the channels the viewer can use ----
    {
        // Pure red, protanopia: the red error is shifted into green and blue.
        const f32 red[3] = {1.0f, 0.0f, 0.0f};
        f32 out[3];
        applyColourVision(packColourVision(1, false, 1.0f), red, out);
        check(out[1] > 0.05f && out[2] > 0.05f, "protanopia correction pushes a pure red's error into green and blue");
        applyColourVision(packColourVision(2, false, 1.0f), red, out);
        check(out[1] > 0.05f && out[2] > 0.05f, "so does deuteranopia");
        // Pure blue, tritanopia: the blue error is shifted into red and green.
        const f32 blue[3] = {0.0f, 0.0f, 1.0f};
        applyColourVision(packColourVision(3, false, 1.0f), blue, out);
        check(out[0] > 0.05f && out[1] > 0.05f, "tritanopia correction pushes a pure blue's error into red and green");
        // A red/green pair becomes more different in blue (the channel a protan still sees).
        const f32 r2[3] = {0.8f, 0.3f, 0.3f}, g2[3] = {0.3f, 0.8f, 0.3f};
        f32 ro[3], go[3];
        applyColourVision(packColourVision(1, false, 1.0f), r2, ro);
        applyColourVision(packColourVision(1, false, 1.0f), g2, go);
        check(std::fabs(ro[2] - go[2]) > std::fabs(r2[2] - g2[2]) + 0.02f, "a red/green pair separates in blue after protanopia correction");
    }

    // ---- simulation ----
    {
        // Simulating twice barely moves a colour: Machado's matrices are close to, not exactly, projections (and the clamp
        // to [0,1] breaks exactness), so the bound is loose; it catches a matrix that was mistyped into something wild.
        bool idem = true;
        const f32 samples[][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0.7f, 0.4f, 0.2f}, {0.2f, 0.6f, 0.9f}};
        for (u32 mode = 1; mode <= 3; ++mode)
            for (const auto& s : samples) {
                f32 once[3], twice[3];
                applyColourVision(packColourVision(mode, true, 1.0f), s, once);
                applyColourVision(packColourVision(mode, true, 1.0f), once, twice);
                // Linear-space comparison: the second pass sees an already-simulated colour.
                for (int k = 0; k < 3; ++k) if (std::fabs(toLin(once[k]) - toLin(twice[k])) > 0.2f) idem = false;
            }
        check(idem, "simulating twice stays within 0.2 in linear space of simulating once");
        const f32 red[3] = {1.0f, 0.0f, 0.0f};
        f32 o[3];
        applyColourVision(packColourVision(1, true, 1.0f), red, o);
        check(o[0] < 0.7f && o[1] > 0.1f, "a protanope sees pure red darker and with a green component");
    }

    // ---- the post constants ----
    {
        PostCB cb{};
        fillPostLens(cb, PostSettings{}, 1920, 1080, true, 0, 3);
        check(cb.access[0] == 0.0f && cb.access[1] == 0.0f && cb.access[2] == 0.0f && cb.access[3] == 0.0f, "default PostSettings leave PostCB::access at zero");
        PostSettings p;
        p.colourVision = 2u;
        p.colourVisionStrength = 0.5f;
        fillPostLens(cb, p, 1920, 1080, true, 0, 3);
        check(cb.access[0] == packColourVision(2, false, 0.5f), "fillPostLens writes the packed filter to access.x");
        check(sizeof(PostCB) == 288 && sizeof(PostSettings) == 180, "the constant block and the settings grew by exactly the new fields");
        PostSettings a, b;
        check(postSettingsEqual(a, b), "equal settings compare equal");
        b.colourVision = 1u;
        check(!postSettingsEqual(a, b), "postSettingsEqual sees the new filter field");
        b = a;
        b.colourVisionStrength = 0.5f;
        check(!postSettingsEqual(a, b), "and its strength");
    }

    if (g_failures != 0) { AVER_ERROR("ColourVisionTest: FAIL ({})", g_failures); return 1; }
    AVER_INFO("ColourVisionTest: PASS");
    return 0;
}
