// GiVisibilityTest -- U1's shared bit definitions and reconstruction arithmetic
// (aver/voxi/GiVisibility.hpp): the giRestirVisibility mode/half-res-pair packing gAmbientParams.w
// carries (packAmbientW), the tracedPixel phase schedule the half-resolution history's write and
// reconstruction share, and the depth/normal reconstruction weight giVisReconstruct sums per tap
// (reconstructWeight). See optimisation-wave2-plan.md sections 2.9/2.10/2.12 and GiVisibility.hpp's
// own top comment for the full design this checks.
//
// NO GPU, NO RHI, same shape as GiDispatchBoundsTest/CameraFactorTest in this same directory: the
// header under test depends on nothing but aver/core/Types.hpp by design, so this links Aver.Core
// alone and reaches it with an include path.
//
// WHAT WOULD HAVE CAUGHT THE BUG THIS SUITE'S OWN VERIFICATION PASS ACTUALLY FOUND: the DXC harness
// (17-case matrix, material base 17), not this file -- this file's job is the ARITHMETIC (packing,
// weighting, the ratio/EMA estimators the reconstruction is built from) and that the shader's SOURCE
// TEXT still carries the exact strings the C2-7 contract specifies, not whether voxi_restir.hlsli
// actually compiles. A struct-typed ternary (HLSL has no conditional operator over non-numeric types)
// slipped past an earlier pass of this lane's edits and was only caught by the DXC harness; this
// suite could not have caught it either, which is exactly why the harness step is separate and
// mandatory, not a replacement for it.
#include "aver/core/Log.hpp"
#include "aver/voxi/GiVisibility.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::voxi::givis;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// ---------------------------------------------------------------- shader/CPU source text helpers
std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        // AN UNREADABLE FILE MUST FAIL EVERY ASSERTION AGAINST IT, NOT SKIP -- an empty string would
        // make every substring search below pass vacuously, same reasoning as GiDispatchBoundsTest's
        // own hlslText().
        AVER_ERROR("[GiVisibility] could not read {} -- every source assertion against it would pass "
                   "vacuously against an empty string, so this is a failure, not a skip.", path);
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

const std::string& restirText() {
    static const std::string s =
        readFile(std::string(AVER_REPO_ROOT) + "/modules/render.voxi/shaders/voxi_restir.hlsli");
    return s;
}
const std::string& rtText() {
    static const std::string s =
        readFile(std::string(AVER_REPO_ROOT) + "/modules/render.voxi/shaders/voxi_rt.hlsli");
    return s;
}
const std::string& voxiText() {
    static const std::string s =
        readFile(std::string(AVER_REPO_ROOT) + "/modules/render.voxi/shaders/voxi.hlsl");
    return s;
}
const std::string& giPreludeText() {
    static const std::string s =
        readFile(std::string(AVER_REPO_ROOT) + "/modules/render.voxi/shaders/voxi_gi.hlsli");
    return s;
}
const std::string& rendererCppText() {
    static const std::string s =
        readFile(std::string(AVER_REPO_ROOT) + "/modules/render.voxi/src/VoxiRenderer.cpp");
    return s;
}

bool has(const std::string& text, const std::string& needle) {
    return !needle.empty() && text.find(needle) != std::string::npos;
}

// Non-overlapping occurrence count -- the same walk-and-advance idiom used wherever this codebase's
// own tests count repeated substrings.
int countOccurrences(const std::string& text, const std::string& needle) {
    if (needle.empty()) return 0;
    int n = 0;
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) { ++n; pos += needle.size(); }
    return n;
}

// Is `needle` found with "if (gAverHistoryWrite" somewhere in the `window` characters right before
// it? Used to confirm PSRayDriven's two deliberately-ungated writes (voxi.hlsl's own sky-miss
// surface-history sentinel and AO hit-distance write) are NOT behind that gate -- a positive control
// on the negative claim, not merely "the total gate count is right".
bool precededByHistoryWriteGate(const std::string& text, const std::string& needle, size_t window = 200) {
    size_t pos = text.find(needle);
    if (pos == std::string::npos) return false;
    size_t start = pos > window ? pos - window : 0;
    return text.substr(start, pos - start).find("if (gAverHistoryWrite") != std::string::npos;
}

// Parses the numeric literal following "#define NAME " in `text` -- used to check GiVisibility.hpp's
// five UNMEASURED tunables against voxi_restir.hlsli's own #define literals without hand-copying
// either side's value into this file a third time.
double defineValue(const std::string& text, const std::string& name) {
    const std::string key = "#define " + name + " ";
    size_t pos = text.find(key);
    if (pos == std::string::npos) return std::numeric_limits<double>::quiet_NaN();
    pos += key.size();
    size_t end = pos;
    while (end < text.size() &&
           (std::isdigit(static_cast<unsigned char>(text[end])) || text[end] == '.' || text[end] == '-'))
        ++end;
    return std::atof(text.substr(pos, end - pos).c_str());
}

// The declared fields of one `cbuffer <marker>` block, one entry per non-comment, non-blank line,
// with any trailing "// ..." comment stripped -- so two mirrors of the same cbuffer can be compared
// on FIELD TEXT alone regardless of how their comments differ (2.9: "Update the comments, text only,
// in all three mirrors"; checklist item 14: "only comments changed"). Assumes the block's closing
// "};" sits alone on its own line, which is how voxi.hlsl and voxi_gi.hlsli both write VoxiFrame's.
std::vector<std::string> cbufferFieldLines(const std::string& text, const std::string& marker) {
    std::vector<std::string> out;
    size_t i = text.find("cbuffer " + marker);
    if (i == std::string::npos) return out;
    size_t open = text.find('{', i);
    if (open == std::string::npos) return out;
    size_t close = text.find("\n};", open);
    if (close == std::string::npos) return out;
    const std::string body = text.substr(open + 1, close - (open + 1));
    std::istringstream iss(body);
    std::string line;
    while (std::getline(iss, line)) {
        const size_t c = line.find("//");
        std::string field = c == std::string::npos ? line : line.substr(0, c);
        // trim
        size_t b = field.find_first_not_of(" \t\r");
        size_t e = field.find_last_not_of(" \t\r");
        if (b == std::string::npos) continue;   // blank or comment-only line
        out.push_back(field.substr(b, e - b + 1));
    }
    return out;
}

} // namespace

int main() {
    AVER_INFO("[GiVisibility] U1's shared bit packing, phase schedule, and reconstruction arithmetic");

    // ---- 1. tracedPixel: the half-res phase schedule (2.10 D/E) ----
    {
        struct P { u32 x, y; };
        const P block[4] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};

        int tracedOnceFailures = 0;
        for (const auto& p : block) {
            int tracedCount = 0;
            for (u32 f = 0; f < 4; ++f)
                if (tracedPixel(p.x, p.y, f)) ++tracedCount;
            if (tracedCount != 1) ++tracedOnceFailures;
        }
        check(tracedOnceFailures == 0, "every pixel of a 2x2 block traces on exactly one of frames "
                                        "0..3 (" + std::to_string(tracedOnceFailures) + " failure(s))");

        // every 2x2 block has exactly one writer per frame -- checked at several block origins so
        // only the low bit of each coordinate is shown to matter (a 2-periodic tiling), not merely
        // the block already probed above.
        const u32 origins[][2] = {{0, 0}, {2, 0}, {0, 2}, {4, 6}, {100, 200}, {8192, 4097}};
        int oneWriterFailures = 0, oneWriterChecks = 0;
        for (const auto& o : origins) {
            for (u32 f = 0; f < 4; ++f) {
                int writers = 0;
                for (u32 dy = 0; dy < 2; ++dy)
                    for (u32 dx = 0; dx < 2; ++dx)
                        if (tracedPixel(o[0] + dx, o[1] + dy, f)) ++writers;
                ++oneWriterChecks;
                if (writers != 1) ++oneWriterFailures;
            }
        }
        check(oneWriterChecks > 0 && oneWriterFailures == 0, "every 2x2 block has exactly one traced "
              "pixel per frame, over " + std::to_string(oneWriterChecks) + " (origin, frame) checks "
              "at several block origins (" + std::to_string(oneWriterFailures) + " failed)");

        // The writer at frame f-1 matches HLSL kGiVisPhase[(frameIdx - 1u) & 3u] for frameIdx 0
        // (unsigned wrap) -- giVisReconstruct's own step 4b (2.10 D), the exact arithmetic that
        // decides which full-resolution pixel wrote the half-res texel a frameIdx-0 reconstruction
        // reads.
        const u32 frameIdx = 0u;
        const u32 prevFrame = (frameIdx - 1u) & 3u;   // HLSL's own unsigned wrap: 0u - 1u == 0xFFFFFFFFu
        check(prevFrame == 3u, "(frameIdx - 1u) & 3u wraps to 3 at frameIdx == 0, matching HLSL's own "
                                "unsigned uint arithmetic (no signed UB, no accidental huge index)");
        // kGiVisPhase[3] == uint2(0, 1) (voxi_restir.hlsli's own literal -- checked again below by
        // source assertion), so pixel (0,1) must be the block's traced pixel at "frame 3".
        check(tracedPixel(0, 1, prevFrame), "tracedPixel(0, 1, (0u - 1u) & 3u) is true, matching "
                                             "voxi_restir.hlsli's kGiVisPhase[3] == uint2(0, 1)");
        int wrongWriterAtPrevFrame = 0;
        for (const auto& p : block)
            if (!(p.x == 0 && p.y == 1) && tracedPixel(p.x, p.y, prevFrame)) ++wrongWriterAtPrevFrame;
        check(wrongWriterAtPrevFrame == 0, "only (0,1), not any other pixel of the block, traces at "
                                            "frame (0u - 1u) & 3u");
    }

    // ---- 2. packAmbientW: round-trips every (mode, bound, valid, cone, replay, pathView,
    //         spatialSamples, maxHistory) ----
    // spatialSamples and maxHistory are each swept through their FULL bit range (0..15, 0..31), which
    // is exactly what Settings' own clamps (Voxi.cpp) restrict callers to, so there is no
    // "past the clamp" value left to add on top.
    {
        int checked = 0, failures = 0;
        for (u32 mode = 0; mode <= 3u; ++mode)
            for (int hb = 0; hb < 2; ++hb)
                for (int hv = 0; hv < 2; ++hv)
                    for (int bc = 0; bc < 2; ++bc)
                        for (int br = 0; br < 2; ++br)
                            for (int pv = 0; pv < 2; ++pv)
                                for (u32 spatialSamples = 0; spatialSamples <= 15u; ++spatialSamples)
                                    for (u32 maxHistory = 0; maxHistory <= 31u; ++maxHistory) {
                                        const bool histBound = hb != 0, histValid = hv != 0, cone = bc != 0,
                                                   replay = br != 0, path = pv != 0;
                                        const u32 w = packAmbientW(mode, histBound, histValid, cone, replay,
                                                                    path, spatialSamples, maxHistory);
                                        ++checked;
                                        const u32 want = (mode & 3u) | (histBound ? 4u : 0u) | (histValid ? 8u : 0u) |
                                                         (cone ? 16u : 0u) | (replay ? 32u : 0u) | (path ? 64u : 0u) |
                                                         ((spatialSamples & 15u) << 12) | ((maxHistory & 31u) << 18);
                                        if (w != want) ++failures;
                                    }
        check(checked == 4 * 2 * 2 * 2 * 2 * 2 * 16 * 32 && failures == 0,
              "packAmbientW round-trips every (mode 0..3, histBound, histValid, blendedCone, "
              "blendedReplay, pathView, spatialSamples 0..15, maxHistory 0..31) combination against "
              "2.9/W6's own bit table exactly -- " + std::to_string(failures) + " of " +
              std::to_string(checked) + " combinations disagreed");

        // Bits 12-15 must not disturb bits 0-6: fixing every OTHER argument and sweeping
        // spatialSamples alone must leave the low seven bits (mode | histBound | histValid |
        // blendedCone | blendedReplay | pathView) exactly as spatialSamples=0 produced them.
        const u32 base = packAmbientW(3u, true, true, true, true, true, 0u, 0u) & 0x7Fu;
        int lowBitsChecked = 0, lowBitsFailures = 0;
        for (u32 spatialSamples = 0; spatialSamples <= 15u; ++spatialSamples) {
            const u32 w = packAmbientW(3u, true, true, true, true, true, spatialSamples, 0u);
            ++lowBitsChecked;
            if ((w & 0x7Fu) != base) ++lowBitsFailures;
        }
        check(lowBitsChecked == 16 && lowBitsFailures == 0,
              "sweeping spatialSamples 0..15 alone never changes bits 0-6 of packAmbientW's result -- " +
              std::to_string(lowBitsFailures) + " of " + std::to_string(lowBitsChecked) + " disagreed");

        // Bits 18-22 must not disturb bits 0-17, and must stay under 2^24 so the float this packing
        // travels in (gAmbientParams.w) can hold it EXACTLY -- the trap that silently corrupted a
        // debug dial parked at bit 24 during the camera-motion fade investigation.
        const u32 base2 = packAmbientW(3u, true, true, true, true, true, 15u, 0u) & 0x3FFFFu;
        int histChecked = 0, histFailures = 0, tooWide = 0;
        for (u32 maxHistory = 0; maxHistory <= 31u; ++maxHistory) {
            const u32 w = packAmbientW(3u, true, true, true, true, true, 15u, maxHistory);
            ++histChecked;
            if ((w & 0x3FFFFu) != base2) ++histFailures;
            if (w >= (1u << 24)) ++tooWide;
            // The float round trip the renderer actually performs, exactly: pack -> f32 -> decode.
            if (static_cast<u32>(static_cast<f32>(w)) != w) ++tooWide;
        }
        check(histChecked == 32 && histFailures == 0 && tooWide == 0,
              "sweeping maxHistory 0..31 alone never changes bits 0-17, and every packed value "
              "survives the f32 round trip gAmbientParams.w puts it through -- " +
              std::to_string(histFailures) + " bit-disagreements and " + std::to_string(tooWide) +
              " values too wide, of " + std::to_string(histChecked));
    }

    // ---- 3. reconstructWeight: rejection tests, and the bilinear partition of unity ----
    {
        check(reconstructWeight(1.0f, 1.0f, 0.0f, 100.0f) > 0.0f,
              "reconstructWeight is positive for a full bilinear weight, an identical normal, and "
              "zero plane distance");
        check(reconstructWeight(1.0f, 1.0f, kPlaneTolCm + 0.5f, 0.0f) == 0.0f,
              "reconstructWeight is exactly zero once the plane distance exceeds "
              "kPlaneTolCm + kPlaneTolRel*viewDepth (viewDepth 0 here, tolerance == kPlaneTolCm)");
        check(reconstructWeight(1.0f, 1.0f, kPlaneTolCm - 0.1f, 0.0f) > 0.0f,
              "...and still positive just inside that same tolerance");
        check(reconstructWeight(1.0f, -0.5f, 0.0f, 0.0f) == 0.0f,
              "reconstructWeight is zero for an opposite-facing normal (nDot < 0)");
        check(reconstructWeight(1.0f, 0.0f, 0.0f, 0.0f) == 0.0f,
              "reconstructWeight is zero for an exactly perpendicular normal (nDot == 0, not > 0)");

        std::mt19937 rng(99);
        std::uniform_real_distribution<float> frac(0.0f, 1.0f);
        int trials = 0, failures = 0;
        for (int t = 0; t < 200; ++t) {
            const float fx = frac(rng), fy = frac(rng);
            const float bw[4] = {(1.0f - fx) * (1.0f - fy), fx * (1.0f - fy), (1.0f - fx) * fy, fx * fy};
            float sum = 0.0f;
            for (float w : bw) sum += reconstructWeight(w, 1.0f, 0.0f, 10.0f);
            ++trials;
            if (std::fabs(sum - 1.0f) > 1e-5f) ++failures;
        }
        check(trials > 0 && failures == 0,
              "a flat surface's four bilinear corner weights (identity normal, zero plane distance) "
              "still sum to exactly 1 through reconstructWeight, over " + std::to_string(trials) +
              " random fractional positions (" + std::to_string(failures) + " failed)");
    }

    // ---- 4. THE RATIO ESTIMATOR: sum(w*g)/sum(w*b) vs. the average of per-tap ratios ----
    //
    // giVisReconstruct computes rho2 = sum(w*g)/sum(w*b) (2.10 D.6), a RATIO OF EXPECTATIONS, not an
    // average of per-tap ratios -- the two estimators are only interchangeable when the sampling
    // weight and the per-tap brightness scale are uncorrelated, which a real half-res neighbourhood
    // has no reason to guarantee. This synthetic field makes them disagree on purpose: two equally
    // likely "kinds" of tap, one BRIGHT-AND-MOSTLY-OCCLUDED (b=10, true visibility V=0.1), one
    // DIM-AND-MOSTLY-OPEN (b=1, V=0.9), with g := V*b exactly (no added noise, so g/b == V on every
    // single tap and the "true expectation ratio" this section computes has a closed form).
    {
        constexpr int N = 4096;
        constexpr double bBright = 10.0, vBright = 0.1;
        constexpr double bDim = 1.0, vDim = 0.9;

        double sumWG = 0.0, sumWB = 0.0, sumRatio = 0.0;
        for (int i = 0; i < N; ++i) {
            const bool bright = (i % 2) == 0;   // exact 50/50 split: a deterministic field, not a
                                                 // Monte-Carlo draw, so there is no run-to-run flake
            const double b = bright ? bBright : bDim;
            const double v = bright ? vBright : vDim;
            const double g = v * b;
            const double w = 1.0;               // uniform sampling weight: the divergence below comes
                                                  // entirely from b's own correlation with v, exactly
                                                  // the "brightness, not sample count, does the
                                                  // weighting" property rho2's formula relies on
            sumWG += w * g;
            sumWB += w * b;
            sumRatio += g / b;   // == v exactly
        }
        // The population values both estimators are being checked against, in closed form:
        //   E[w*g] / E[w*b] == (0.5*vBright*bBright + 0.5*vDim*bDim) / (0.5*bBright + 0.5*bDim)
        //   E[g/b]          == 0.5*vBright + 0.5*vDim   (the naive, unweighted-by-brightness average)
        const double trueRatio = (0.5 * vBright * bBright + 0.5 * vDim * bDim) / (0.5 * bBright + 0.5 * bDim);
        const double naiveRatio = 0.5 * vBright + 0.5 * vDim;

        const double estRatio = sumWG / sumWB;
        const double avgRatio = sumRatio / N;

        check(std::fabs(estRatio - trueRatio) / trueRatio < 0.01,
              "sum(w*g)/sum(w*b) over " + std::to_string(N) + " synthetic taps converges to the true "
              "expectation ratio within 1% (got " + std::to_string(estRatio) + ", true " +
              std::to_string(trueRatio) + ")");
        check(std::fabs(avgRatio - trueRatio) / trueRatio > 0.05,
              "NEGATIVE CONTROL: the unweighted average of per-tap ratios (g_i/b_i) is biased by more "
              "than 5% against the same true expectation ratio on this skewed field -- confirming "
              "rho2's own ratio-of-sums formula is not an interchangeable simplification of the naive "
              "average (got " + std::to_string(avgRatio) + " vs. true " + std::to_string(trueRatio) + ")");
        check(std::fabs(avgRatio - naiveRatio) < 1e-9,
              "sanity check: the naive average equals the closed-form 0.5*vBright + 0.5*vDim exactly, "
              "confirming this test's own arithmetic (g/b == v on every tap by construction)");
    }

    // ---- 5. THE HALF-RES HISTORY'S EMA (2.10 E): converges toward a steady fresh value ----
    //
    // The write is `lerp(fresh, history, h)` (HLSL lerp(a,b,t) = a + t*(b-a) = a*(1-t) + b*t), i.e.
    // history_n = fresh*(1 - h) + history_{n-1}*h -- a standard exponential moving average with the
    // OLD value weighted by h = kHistWeight at rest. Geometric convergence means the error after n
    // steps toward a constant fresh value is exactly kHistWeight^n times the initial error.
    {
        const double target = 5.0;   // an arbitrary steady "fresh" value every step
        double hist = 0.0;           // the honest "nothing traced yet" prior (2.10 E's own r=1.0/g=b=0.0
                                      // defaults are different per-channel constants; 0.0 here since
                                      // only the CONVERGENCE RATE is under test, not any one channel's
                                      // specific starting value)
        for (int step = 0; step < 64; ++step)
            hist = target * (1.0 - kHistWeight) + hist * kHistWeight;
        check(std::fabs(hist - target) / target < 0.01,
              "the half-res history's own EMA shape (fresh*(1-kHistWeight) + history*kHistWeight) "
              "converges to a steady value within 1% after 64 steps (kHistWeight = " +
              std::to_string(kHistWeight) + ", got " + std::to_string(hist) + " vs target " +
              std::to_string(target) + ")");
    }

    // ---- 6. GiVisibility.hpp's five tunables equal voxi_restir.hlsli's own #define literals ----
    {
        struct Pair { const char* name; double value; };
        const Pair pairs[] = {
            {"AVER_GI_VIS_HIST_WEIGHT", static_cast<double>(kHistWeight)},
            {"AVER_GI_VIS_RHO_MAX", static_cast<double>(kRhoMax)},
            {"AVER_GI_VIS_NORMAL_POW", static_cast<double>(kNormalPow)},
            {"AVER_GI_VIS_PLANE_TOL_REL", static_cast<double>(kPlaneTolRel)},
            {"AVER_GI_VIS_PLANE_TOL_CM", static_cast<double>(kPlaneTolCm)},
        };
        for (const auto& p : pairs) {
            const double shaderVal = defineValue(restirText(), p.name);
            check(!std::isnan(shaderVal) && std::fabs(shaderVal - p.value) < 1e-6,
                  std::string("GiVisibility.hpp's own value for ") + p.name + " (" +
                  std::to_string(p.value) + ") equals voxi_restir.hlsli's #define literal (" +
                  std::to_string(shaderVal) + ")");
        }
    }

    // ---- 7. halfDim: rounds an odd full-resolution edge UP, matching ensureShadowHistory's own
    //         "(w+1)/2 x (h+1)/2" sizing (2.11) ----
    {
        check(halfDim(0) == 0, "halfDim(0) == 0");
        check(halfDim(1) == 1, "halfDim(1) == 1 -- a single leftover row/column still needs a texel");
        check(halfDim(2) == 1, "halfDim(2) == 1 (an even edge halves exactly)");
        check(halfDim(3) == 2, "halfDim(3) == 2 (odd rounds up, not down)");
        check(halfDim(3532) == 1766 && halfDim(1987) == 994,
              "halfDim matches 2.10 F's own native-resolution memory-table figures (3532x1987 -> "
              "1766x994)");
    }

    // ---- 8. SOURCE ASSERTIONS: voxi_restir.hlsli carries the exact C2-7 text ----
    {
        const std::string& t = restirText();
        check(has(t, "((uint)gAmbientParams.z & 4u) != 0u || f2Path == 0u"),
              "the F2 legacy branch's condition gains only `|| f2Path == 0u`");
        check(has(t, "indY = averSkyIrradiance(s.N) * gAmbient.r;"),
              "the F2 legacy branch's body is still the unchanged single assignment");
        check(has(t, "f3Path == 3u"), "the F3 ray's own condition gates on f3Path == 3u");
        check(has(t, "reuse.numSamples = 0u"),
              "Reconstructed forces temporal-only reuse (reuse.numSamples = 0u)");
        check(has(t, "halfBound && !tracedPx && rec.valid"),
              "the decode block's own fallback-to-Full test for a non-traced Half pixel with no valid "
              "reconstruction");
        check(has(t, "register(t16)"), "gGiVisHist is bound at t16");
        check(has(t, "register(u10)"), "gGiVisHistOut is bound at u10");
        check(has(t, "gGiRestirParams.w <= 0.5 && ((uint)gAmbientParams.w & 64u)"),
              "the path-view block is gated behind the poison view AND voxi.giVisPathView (bit 64)");
        check(has(t, "static const uint2 kGiVisPhase[4] = { uint2(0, 0), uint2(1, 1), uint2(1, 0), "
                     "uint2(0, 1) };"),
              "kGiVisPhase's literal matches givis::tracedPixel's own mirrored table exactly");

        // reuse.numSamples = 0u must come AFTER the motion-discount lerp (checklist item 9), never
        // before -- otherwise the discount's own numSamples write would silently undo Reconstructed's
        // forced temporal-only mode.
        const std::string lerpLine = "reuse.samplingRadius = lerp(32.0, 8.0, motionT);";
        const size_t lerpPos = t.find(lerpLine);
        const size_t zeroPos = t.find("reuse.numSamples = 0u");
        check(lerpPos != std::string::npos && zeroPos != std::string::npos && zeroPos > lerpPos,
              "`reuse.numSamples = 0u` appears textually AFTER the motion-discount lerp, never "
              "before it");

        // Exactly 4 gAverHistoryWrite WRITE gates (store, surface history, denoiser input, the new
        // visibility write) plus the denoised readback's TWO gates: the reprojected read b6a64126 added
        // (gAverHistoryWrite && denoisedReproject ...) and the decode it falls back to
        // (gAverHistoryWrite && gw > 0u) -- both keep a blended fragment from reading the opaque
        // surface's denoised answer.
        const int totalGates = countOccurrences(t, "if (gAverHistoryWrite");
        const int readbackGates = countOccurrences(t, "if (gAverHistoryWrite && gw > 0u");
        const int reprojectGates = countOccurrences(t, "if (gAverHistoryWrite && denoisedReproject");
        check(totalGates == 6 && readbackGates == 1 && reprojectGates == 1,
              "voxi_restir.hlsli has exactly 4 gAverHistoryWrite write gates (store/surface-history/"
              "denoiser-input/visibility-write) plus the denoised readback's two gates (reprojected read, decode) -- " +
              std::to_string(totalGates) + " total `if (gAverHistoryWrite` occurrences, " +
              std::to_string(readbackGates) + " decode + " + std::to_string(reprojectGates) +
              " reprojected-read gate(s)");

        // The reservoir is the engine's own (voxi_reservoir.hlsli): no vendored resampling header may
        // come back in through this file.
        check(has(t, "#include \"voxi_reservoir.hlsli\""), "voxi_restir.hlsli includes the in-house reservoir module");
        check(!has(t, "Rtxdi/") && !has(t, "RTXDI_") && !has(t, "RAB_"),
              "voxi_restir.hlsli names no RTXDI header, function or RAB_ callback");
    }

    // ---- 9. SOURCE ASSERTIONS: voxi_rt.hlsli's traceCone prototype and its own gate count ----
    {
        const std::string& t = rtText();
        check(has(t, "float4 traceCone(float3 originWS, float3 dir, float aperture);"),
              "the traceCone forward-declaration matches voxi_cone.hlsli:62's own signature verbatim");
        check(has(t, "averShadowLum"), "F2's traced-path luminance still uses this file's own "
                                        "averShadowLum reduction, not a second formula");

        // No unconditional denoised-occlusion readback gate here any more: df4122cc ("Ray-driven stops
        // reading a denoised occlusion it did not produce") put denoisedAoUsable in front of it.
        const int totalGates = countOccurrences(t, "if (gAverHistoryWrite");
        const int readbackGates = countOccurrences(t, "if (gAverHistoryWrite && dnW > 0u");
        check(totalGates == 4 && readbackGates == 0,
              "voxi_rt.hlsli has exactly 4 gAverHistoryWrite write gates (AO history/AO hit-distance/"
              "RT-shadow tiled/RT-shadow untiled) and no unconditional denoised readback gate (df4122cc) -- " +
              std::to_string(totalGates) + " total, " + std::to_string(readbackGates) + " readback");
    }

    // ---- 10. SOURCE ASSERTIONS: voxi.hlsl's gate count, the static flag, and PSRayDriven's two
    //          deliberately UNgated writes ----
    {
        const std::string& t = voxiText();
        check(has(t, "static bool gAverHistoryWrite = true;"), "the static flag exists with its "
                                                                 "documented true default");
        check(has(t, "bool averDrawIsTranslucent() { return (gMaterialFlags & AVER_MAT_ALPHA_BLEND) "
                     "!= 0u || gTransmission > 0.0; }"),
              "averDrawIsTranslucent includes gTransmission > 0.0, matching Material.cpp's own "
              "alphaMode==Blend || transmission>0 test (checklist item 13)");
        check(has(t, "gAverHistoryWrite = true;"), "PSRayDriven sets the flag explicitly true (never "
                                                     "left to the static default alone)");

        const int totalGates = countOccurrences(t, "if (gAverHistoryWrite");
        check(totalGates == 2, "voxi.hlsl has exactly 2 gAverHistoryWrite write gates (both "
                                "rtReflectionTemporal branches), no denoiser-style readback of its own -- "
                                "got " + std::to_string(totalGates));

        // PSRayDriven's own sky-miss surface-history sentinel and AO hit-distance writes are NOT
        // gated on gAverHistoryWrite at all (checklist item 12) -- checked positively (the lines still
        // exist) and negatively (nothing that looks like the gate sits immediately before either).
        const std::string skyMissSentinel = "gGiSurfNrmHistOut[uint2(i.pos.xy)] = float2(0.0, asfloat(0u));";
        check(has(t, skyMissSentinel), "PSRayDriven's sky-miss surface-history sentinel write exists");
        check(!precededByHistoryWriteGate(t, skyMissSentinel),
              "...and is NOT behind an `if (gAverHistoryWrite` gate");

        const std::string aoHitDistWrite = "gAoHitDistOut[uint2(i.pos.xy)] = amb.hitDist;";
        check(has(t, aoHitDistWrite), "PSRayDriven's AO hit-distance write exists");
        check(!precededByHistoryWriteGate(t, aoHitDistWrite),
              "...and is NOT behind an `if (gAverHistoryWrite` gate either (only gRtDenoiseParams.w, "
              "unrelated)");
    }

    // ---- 11. cbuffer VoxiFrame: voxi.hlsl and voxi_gi.hlsli declare the IDENTICAL field list ----
    //          (aver-voxi-cbuffer-three-mirrors) -- comments are allowed, indeed expected, to differ
    //          (2.9: "Update the comments, text only, in all three mirrors"); the FIELDS must not.
    {
        const std::vector<std::string> a = cbufferFieldLines(voxiText(), "VoxiFrame");
        const std::vector<std::string> b = cbufferFieldLines(giPreludeText(), "VoxiFrame");
        check(!a.empty() && !b.empty(), "both cbuffer VoxiFrame blocks were found and parsed (" +
              std::to_string(a.size()) + " fields in voxi.hlsl, " + std::to_string(b.size()) +
              " in voxi_gi.hlsli)");
        check(a == b, "voxi.hlsl's and voxi_gi.hlsli's cbuffer VoxiFrame declare byte-identical field "
                       "lines, comments stripped -- no layout change from this task, only comments");
    }

    // ---- 12. SOURCE ASSERTION: VoxiRenderer.cpp's binding-count static_assert (consumed from L4) ----
    {
        check(has(rendererCppText(), "kVoxiSrvCount == 20 && kVoxiUavCount == 20"),
              "VoxiRenderer.cpp asserts the widened binding counts (20 SRV slots, 20 UAV slots) this "
              "lane's t16/u10 registers depend on (u11-u15 are the staged ray-driven buffers, t17/u16 "
              "are the occlusion-aware fog design's air sky-visibility volume, u17/u18 are the "
              "sub-stage splits' own GI-trace candidate and shadow-probe tile buffers, t18/t19/u19 are "
              "the local-light list and its visibility history)");
    }

    if (g_failures == 0) {
        AVER_INFO("[GiVisibility] PASS: the phase schedule tiles 2x2 blocks with exactly one traced "
                  "pixel per frame, packAmbientW round-trips every bit combination, reconstructWeight's "
                  "rejection tests and partition-of-unity hold, the ratio-of-sums estimator converges "
                  "where the naive per-tap average is demonstrably biased, the EMA converges "
                  "geometrically, the five tunables match voxi_restir.hlsli's own #defines, and every "
                  "C2-7 source string, gate count, and cbuffer field list this header's callers depend "
                  "on is still exactly as specified");
        return 0;
    }
    AVER_ERROR("[GiVisibility] FAIL: {} check(s)", g_failures);
    return 1;
}
