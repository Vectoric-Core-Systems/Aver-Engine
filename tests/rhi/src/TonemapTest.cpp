// TonemapTest -- no device. The extended tonemappers (Linear, Reinhard, filmic, ACES, AgX) and the display adjustments, as the CPU twin in rhi/Tonemap.hpp
// computes them, against reference values from the published formulas written out again here in double precision and in a different layout (the AgX
// matrices as column lists, the filmic bias as a scale of the input). Also: which settings select the AVER_POST_TONEMAP_EXT composite (the default must
// stay the ordinary one), the post constant block's packing, the project keys, and the HLSL read back as text (the old tonemap line and function are
// unchanged; the new code sits behind the define; the constants match the twin's). The GPU side is checked by the compile tests and, headed, by the owner.
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/rhi/FrameConstants.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/Tonemap.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace aver;
using namespace aver::rhi;

#ifndef AVER_REPO_ROOT
#error "AVER_REPO_ROOT must name the repository root"
#endif

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// ---- the reference, in double ----

struct V { double c[3]; };

// A matrix given as the list of its columns (how GLSL's mat3(vec3, vec3, vec3) is written).
static V byColumns(const double cols[3][3], const V& v) {
    V o{{0, 0, 0}};
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) o.c[i] += cols[j][i] * v.c[j];
    return o;
}

static V refReinhard(V c, double w) {
    for (double& x : c.c) x = x * (1.0 + x / (w * w)) / (1.0 + x);
    return c;
}

// Hable's curve with the exposure bias of 2 applied to the input rather than baked into A and B.
static double hable(double x) {
    const double A = 0.22, B = 0.30, C = 0.10, D = 0.20, E = 0.01, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}
static V refFilmic(V c, double w) {
    for (double& x : c.c) x = hable(2.0 * x) / hable(2.0 * w);
    return c;
}

static V refAces(V c, double w) {
    const double in[3][3] = {{0.59719, 0.35458, 0.04823}, {0.07600, 0.90834, 0.01566}, {0.02840, 0.13383, 0.83777}};
    const double out[3][3] = {{1.60475, -0.53108, -0.07367}, {-0.10208, 1.10813, -0.00605}, {-0.00327, -0.07276, 1.07602}};
    const auto fit = [](double v) { return (v * (v + 0.0245786) - 0.000090537) / (v * (0.983729 * v + 0.4329510) + 0.238081); };
    double r[3];
    for (int i = 0; i < 3; ++i) r[i] = fit(1.8 * (in[i][0] * c.c[0] + in[i][1] * c.c[1] + in[i][2] * c.c[2]));
    const double wt = fit(1.8 * w);
    V o;
    for (int i = 0; i < 3; ++i) o.c[i] = (out[i][0] * r[0] + out[i][1] * r[1] + out[i][2] * r[2]) / wt;
    return o;
}

static V refAgx(V c) {
    const double toRec2020[3][3] = {{0.6274039, 0.0690973, 0.0163914}, {0.3292830, 0.9195405, 0.0880133}, {0.0433131, 0.0113622, 0.8955953}};
    const double inset[3][3] = {{0.856627153315983, 0.137318972929847, 0.11189821299995},
                                {0.0951212405381588, 0.761241990602591, 0.0767994186031903},
                                {0.0482516061458583, 0.101439036467562, 0.811302368396859}};
    const double outset[3][3] = {{1.1271005818144368, -0.1413297634984383, -0.14132976349843826},
                                 {-0.11060664309660323, 1.157823702216272, -0.11060664309660294},
                                 {-0.016493938717834573, -0.016493938717834257, 1.2519364065950405}};
    const double fromRec2020[3][3] = {{1.6604910, -0.1245505, -0.0181508}, {-0.5876411, 1.1328999, -0.1005789}, {-0.0728499, -0.0083494, 1.1187297}};
    V v = byColumns(inset, byColumns(toRec2020, c));
    const double minEv = std::log2(std::pow(2.0, -10.0) * 0.18), maxEv = std::log2(std::pow(2.0, 6.5) * 0.18);
    for (double& x : v.c) {
        x = std::min(std::max(std::log2(std::max(x, 2e-10)), minEv), maxEv);
        x = (x - minEv) / (maxEv - minEv);
        // The 6th-order fit of the AgX sigmoid, Horner form.
        x = x * (0.021 + x * (4.0111 + x * (-25.682 + x * (70.359 + x * (-74.778 + x * 27.069)))));
        x = std::pow(std::max(x, 0.0), 2.4);
    }
    return byColumns(fromRec2020, byColumns(outset, v));
}

static tmap::Vec3f f3(const V& v) { return tmap::Vec3f{{static_cast<f32>(v.c[0]), static_cast<f32>(v.c[1]), static_cast<f32>(v.c[2])}}; }

static double worst(const tmap::Vec3f& got, const V& want) {
    double w = 0;
    for (int i = 0; i < 3; ++i) w = std::max(w, std::fabs(static_cast<double>(got.v[i]) - want.c[i]));
    return w;
}

static std::string slurp(const std::string& rel) {
    std::string s;
    readFileText(std::string(AVER_REPO_ROOT) + "/" + rel, s);
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

static bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

int main() {
    AVER_INFO("TonemapTest");
    const V inputs[] = {{{0, 0, 0}}, {{0.01, 0.01, 0.01}}, {{0.18, 0.18, 0.18}}, {{0.5, 0.5, 0.5}}, {{1, 1, 1}}, {{2, 2, 2}}, {{8, 8, 8}},
                        {{0.9, 0.4, 0.1}}, {{0.05, 0.3, 0.8}}, {{3.0, 0.2, 0.0}}};
    const double whites[] = {1.0, 2.0, 6.0};

    AVER_INFO("-- hand values --");
    check(near(tmap::reinhard(tmap::Vec3f{{0.5f, 0.5f, 0.5f}}, 2.0f).v[0], 0.375, 1e-6), "Reinhard(0.5, white 2) = 0.5 * (1 + 0.5 / 4) / 1.5 = 0.375");
    check(near(tmap::reinhard(tmap::Vec3f{{0.18f, 0.5f, 3.0f}}, 1.0f).v[1], 0.5, 1e-6) && near(tmap::reinhard(tmap::Vec3f{{0.18f, 0.5f, 3.0f}}, 1.0f).v[2], 3.0, 1e-5),
          "Reinhard at the default white 1 is the identity (so the default Reinhard is a clipped linear)");
    check(near(tmap::ext(kTonemapExtLinear, tmap::Vec3f{{0.18f, 0.5f, 3.0f}}, 1.0f).v[2], 3.0, 1e-6), "linear returns the colour untouched (the encode clips it)");
    for (double w : whites) {
        const tmap::Vec3f white{{static_cast<f32>(w), static_cast<f32>(w), static_cast<f32>(w)}};
        check(near(tmap::filmic(white, static_cast<f32>(w)).v[1], 1.0, 1e-5), "filmic maps its white point to 1 (white " + std::to_string(w) + ")");
        check(near(tmap::acesExt(white, static_cast<f32>(w)).v[1], 1.0, 2e-4), "ACES maps its white point to 1 (white " + std::to_string(w) + ")");
        check(near(tmap::reinhard(white, static_cast<f32>(w)).v[1], 1.0, 1e-5), "Reinhard maps its white point to 1 (white " + std::to_string(w) + ")");
    }
    check(near(tmap::filmic(tmap::Vec3f{{0, 0, 0}}, 1.0f).v[0], 0.0, 1e-6), "filmic(0) = 0");
    {
        const tmap::Vec3f g = tmap::agx(tmap::Vec3f{{0.18f, 0.18f, 0.18f}}, 1.0f);
        check(g.v[0] > 0.15f && g.v[0] < 0.21f && near(g.v[0], g.v[1], 2e-3) && near(g.v[1], g.v[2], 2e-3), "AgX puts 0.18 grey at about 0.18 and keeps it neutral");
        const tmap::Vec3f lo = tmap::agx(tmap::Vec3f{{0, 0, 0}}, 1.0f), hi = tmap::agx(tmap::Vec3f{{1000, 1000, 1000}}, 1.0f);
        check(lo.v[1] < 0.01f && hi.v[1] > 0.95f && hi.v[1] < 1.05f, "AgX maps black to ~0 and a huge value to ~1");
    }

    AVER_INFO("-- against the reference formulas --");
    for (double w : whites) {
        for (const V& in : inputs) {
            const tmap::Vec3f c = f3(in);
            const f32 wf = static_cast<f32>(w);
            char label[96];
            std::snprintf(label, sizeof label, "(%.2f %.2f %.2f) white %.0f", in.c[0], in.c[1], in.c[2], w);
            check(worst(tmap::reinhard(c, wf), refReinhard(in, w)) < 1e-4 * std::max(1.0, in.c[0] + in.c[1] + in.c[2]), std::string("Reinhard ") + label);
            check(worst(tmap::filmic(c, wf), refFilmic(in, w)) < 2e-4, std::string("filmic ") + label);
            check(worst(tmap::acesExt(c, wf), refAces(in, w)) < 5e-4, std::string("ACES ") + label);
            if (w == 1.0) check(worst(tmap::agx(c, wf), refAgx(in)) < 5e-4, std::string("AgX ") + label);
        }
    }
    {
        // The dispatcher floors negatives, takes the white point from its argument, and ignores it for AgX.
        const tmap::Vec3f neg{{-1.0f, 0.5f, 0.5f}};
        check(tmap::ext(kTonemapExtFilmic, neg, 1.0f).v[0] == tmap::ext(kTonemapExtFilmic, tmap::Vec3f{{0, 0.5f, 0.5f}}, 1.0f).v[0], "a negative channel is floored at zero before the curve");
        check(near(tmap::ext(kTonemapExtAgx, tmap::Vec3f{{0.5f, 0.5f, 0.5f}}, 1.0f).v[0], tmap::ext(kTonemapExtAgx, tmap::Vec3f{{0.5f, 0.5f, 0.5f}}, 9.0f).v[0], 1e-7), "AgX has no white point");
        check(tmap::ext(kTonemapExtReinhard, tmap::Vec3f{{1, 1, 1}}, 0.0f).v[0] == tmap::ext(kTonemapExtReinhard, tmap::Vec3f{{1, 1, 1}}, 1e-3f).v[0], "a zero white point is held at 0.001");
    }
    {
        // Monotone and bounded on a neutral ramp, per mode.
        for (u32 mode = kTonemapExtReinhard; mode <= kTonemapExtAgx; ++mode) {
            bool mono = true, bounded = true;
            f32 prev = -1.0f;
            for (f32 x = 0.0f; x <= 16.0f; x += 0.05f) {
                const f32 y = tmap::ext(mode, tmap::Vec3f{{x, x, x}}, 1.0f).v[1];
                if (y < prev - 1e-5f) mono = false;
                // Before the display clip, filmic and ACES overshoot 1 above their white point; Reinhard at white 1 is the identity.
                if (!(y >= -1e-3f) || (mode != kTonemapExtReinhard && y > 2.0f)) bounded = false;
                prev = y;
            }
            check(mono && bounded, "mode " + std::to_string(mode) + " is monotone and non-negative on a neutral ramp (bounded by 2 where it is not the identity)");
        }
    }

    AVER_INFO("-- display adjustments --");
    {
        const tmap::Vec3f id = tmap::adjust(tmap::Vec3f{{0.8f, 0.6f, 0.4f}}, 1, 1, 1);
        check(near(id.v[0], 0.8, 1e-6) && near(id.v[1], 0.6, 1e-6) && near(id.v[2], 0.4, 1e-6), "all three at 1 is the identity");
        const tmap::Vec3f b = tmap::adjust(tmap::Vec3f{{0.8f, 0.6f, 0.4f}}, 0.5f, 1, 1);
        check(near(b.v[0], 0.4, 1e-6) && near(b.v[2], 0.2, 1e-6), "brightness scales");
        check(near(tmap::adjust(tmap::Vec3f{{0.7f, 0.7f, 0.7f}}, 1, 2.0f, 1).v[0], 0.9, 1e-6) && near(tmap::adjust(tmap::Vec3f{{0.7f, 0.2f, 0.5f}}, 1, 0.0f, 1).v[1], 0.5, 1e-6),
              "contrast pivots on 0.5 (0.7 at 2x is 0.9; at 0 everything is 0.5)");
        const tmap::Vec3f s = tmap::adjust(tmap::Vec3f{{0.9f, 0.3f, 0.0f}}, 1, 1, 0.0f);
        check(near(s.v[0], 0.4, 1e-4) && near(s.v[1], 0.4, 1e-4) && near(s.v[2], 0.4, 1e-4), "saturation 0 collapses to the channel mean");
    }

    AVER_INFO("-- which composite --");
    {
        PostSettings p;
        check(!postNeedsExtComposite(p), "the default settings use the ordinary composite");
        for (u32 m = 0; m <= 2; ++m) { p.tonemap = m; check(!postNeedsExtComposite(p), "Aver tonemap " + std::to_string(m) + " uses the ordinary composite"); }
        for (u32 m = 3; m <= 7; ++m) { p.tonemap = m; check(postNeedsExtComposite(p), "extended tonemap " + std::to_string(m) + " selects the EXT composite"); }
        p = PostSettings{};
        p.adjustContrast = 1.1f;
        check(postNeedsExtComposite(p), "an adjustment off 1 selects the EXT composite");
        p = PostSettings{};
        p.tonemapWhite = 6.0f;
        check(!postNeedsExtComposite(p), "a white point alone, with an Aver tonemapper, does not");
        PostSettings q;
        check(postSettingsEqual(PostSettings{}, q), "equal settings compare equal");
        PostSettings r = q; r.tonemapWhite = 2.0f; check(!postSettingsEqual(q, r), "postSettingsEqual sees the white point");
        r = q; r.adjustBrightness = 2.0f; check(!postSettingsEqual(q, r), "postSettingsEqual sees brightness");
        r = q; r.adjustContrast = 2.0f; check(!postSettingsEqual(q, r), "postSettingsEqual sees contrast");
        r = q; r.adjustSaturation = 2.0f; check(!postSettingsEqual(q, r), "postSettingsEqual sees saturation");
        check(PostSettings{}.tonemap == 1 && PostSettings{}.tonemapWhite == 1.0f, "the default tonemap is still Aver's matrixed ACES (1)");
        check(clampTonemapMode(9) == 7 && clampTonemapMode(-3) == 0 && tonemapModeFromExtIndex(0) == 3 && tonemapModeFromExtIndex(4) == 7 && tonemapModeFromExtIndex(9) == 7, "mode clamping and the extended offset");
    }

    AVER_INFO("-- the post constants --");
    {
        check(sizeof(PostCB) == 304 && sizeof(PostSettings) == 196, "PostCB grew by the one ext row");
        PostCB cb{};
        fillPostLens(cb, PostSettings{}, 1920, 1080, false, 0, 0);
        check(cb.ext[0] == 1.0f && cb.ext[1] == 1.0f && cb.ext[2] == 1.0f && cb.ext[3] == 1.0f, "the defaults pack to white 1 and identity adjustments");
        PostSettings s;
        s.tonemapWhite = 6.0f; s.adjustBrightness = 1.2f; s.adjustContrast = -3.0f; s.adjustSaturation = 0.8f;
        fillPostLens(cb, s, 1920, 1080, false, 0, 0);
        check(cb.ext[0] == 6.0f && cb.ext[1] == 1.2f && cb.ext[2] == 0.0f && cb.ext[3] == 0.8f, "settings pack in order, a negative adjustment clamps to 0");
    }

    AVER_INFO("-- the project keys --");
    {
        PostSettings p;
        check(!applyProjectTonemapKeys(p, -1, -1.0f, -1.0f, -1.0f, -1.0f, true) && postSettingsEqual(p, PostSettings{}), "nothing stated changes nothing");
        check(applyProjectTonemapKeys(p, 5, 4.0f, 1.5f, -1.0f, 0.5f, true) && p.tonemap == 5 && p.tonemapWhite == 4.0f && p.adjustBrightness == 1.5f &&
                  p.adjustContrast == 1.0f && p.adjustSaturation == 0.5f,
              "stated keys apply, unstated ones stay");
        PostSettings q;
        applyProjectTonemapKeys(q, 99, -1.0f, -1.0f, -1.0f, -1.0f, true);
        check(q.tonemap == 7, "a mode past the last is clamped");
        PostSettings r;
        r.tonemap = 2;
        check(applyProjectTonemapKeys(r, 6, 3.0f, -1.0f, -1.0f, -1.0f, false) && r.tonemap == 2 && r.tonemapWhite == 3.0f, "a mode the command line set is kept (modeAllowed false), the white still applies");
    }

    AVER_INFO("-- the HLSL --");
    {
        const std::string color = slurp("modules/rhi/shaders/color.hlsli");
        const std::string post = slurp("modules/rhi/shaders/post.hlsl");
        check(!color.empty() && !post.empty(), "the shader files were read from the repository");
        check(has(color, "float3 averTonemap(float3 x, float mode) {\n    if (mode > 1.5) return acesLumaTonemap(x);\n    return mode > 0.5 ? acesFittedTonemap(x) : acesTonemap(x);\n}"),
              "averTonemap is the function it always was");
        const size_t extBegin = color.find("#ifdef AVER_POST_TONEMAP_EXT");
        const size_t extAt = color.find("float3 averTonemapExt(");
        const size_t extEnd = color.find("#endif", extAt == std::string::npos ? 0 : extAt);
        check(extBegin != std::string::npos && extAt != std::string::npos && extEnd != std::string::npos && extBegin < extAt, "the extended tonemappers sit inside #ifdef AVER_POST_TONEMAP_EXT");
        check(color.find("float3 toGamma(") > extEnd && color.find("float3 acesTonemap(") < extBegin, "and every other function stays outside it");
        check(has(post, "#else\n    float3 outc = toGamma(averTonemap(c, gPostMisc.w));\n#endif"), "PSComposite's ordinary branch is the old line");
        const size_t cb0 = post.find("float4 gPostAccess;");
        const size_t ext0 = post.find("#ifdef AVER_POST_TONEMAP_EXT", cb0);
        const size_t ext1 = post.find("float4 gPostExt;", cb0);
        const size_t end1 = post.find("#endif", ext1);
        check(cb0 != std::string::npos && ext0 != std::string::npos && ext0 < ext1 && ext1 < end1 && post.find("};", cb0) > end1, "gPostExt is declared only under the define, after gPostAccess, inside the cbuffer");
        check(has(post, "gPostMisc.w > 2.5 ? averTonemapExt(c, gPostMisc.w, gPostExt.x) : averTonemap(c, gPostMisc.w)"), "modes above 2 take the extended path");
        // The mode thresholds in averTonemapExt are the enum.
        check(kTonemapExtLinear == 3 && kTonemapExtReinhard == 4 && kTonemapExtFilmic == 5 && kTonemapExtAces == 6 && kTonemapExtAgx == 7 &&
                  has(color, "if (mode > 6.5) return saturate(averTonemapAgxExt(c));") && has(color, "if (mode > 5.5) return saturate(averTonemapAcesExt(c, white));") &&
                  has(color, "if (mode > 4.5) return saturate(averTonemapFilmicExt(c, white));") && has(color, "if (mode > 3.5) return saturate(averTonemapReinhardExt(c, white));"),
              "the HLSL dispatch thresholds are the enum values 4..7 (3 is linear)");
        for (const char* k : {"-12.4739311883324", "4.02606881166759", "0.856627153315983", "0.137318972929847", "0.11189821299995", "1.1271005818144368", "-0.1413297634984383",
                              "1.2519364065950405", "0.6274039", "1.6604910", "0.021 * v", "4.0111 * v2", "25.682", "70.359", "74.778", "27.069", "2.4", "0.0245786",
                              "0.000090537", "0.983729", "0.4329510", "0.238081", "const float bias = 1.8", "0.22 * 4.0", "0.30 * 2.0", "0.33333"})
            check(has(color, k), std::string("color.hlsli carries the constant ") + k);
        check(has(color, "white * white") && has(color, "float w = white * bias;"), "Reinhard divides by white squared and ACES scales the white by the bias");
    }

    AVER_INFO("TonemapTest: {} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
