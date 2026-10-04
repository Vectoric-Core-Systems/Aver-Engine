// The lighting-contrast fix's F4 sky-ownership identity (contrast-fix plan, root cause R1): giMode 1
// used to count a receiver's own sky twice, once traced through ReSTIR and once again through the
// ambient term. F4 removes the second copy by subtracting it back out of ind.diffuse rather than by
// touching ind.ambient (which FmsEms still needs) -- checked here as a pure algebraic identity, with
// no GPU, over 10k random tuples.
//
// WHY THIS IS A TEST AND NOT A COMMENT. The plan derives the identity
//   diffAmbient + diffBounce = kD*est + FmsEms*A + (1-g)*kD*A
// by hand, from material_prelude.hlsl's own (unchanged) diffAmbient/diffBounce formulas and F4's
// subtraction -- a derivation is exactly the kind of thing that is right until someone edits one line
// six months from now and no longer is. Running the two sides of that identity against 10k random
// (kD, FmsEms, A, est, g) tuples, including the cases where F4's subtraction drives the intermediate
// diffuse term negative, is what actually PROVES the algebra rather than merely asserting it once.
//
// STYLE: same shape as VoxiRtSeqTest.cpp/VoxiHemiSampleTest.cpp/ConeWeightTest.cpp beside this file --
// links Aver.Core and nothing else, no GPU, no RHI; a C++ mirror of the identity; source assertions
// tying the mirror's premises to the actual shader/material text.
//
// LANE NOTE: this file's author (Lane D) owns no shader files. Lane B owns voxi.hlsl (F4's two
// subtraction sites) and Lane C owns pt_pathtrace.hlsl (F6's lastDiffuse gate); material_prelude.hlsl
// is on the MUST-NOT-CHANGE list, so its two lines below should always be found untouched -- that
// assertion exists to catch anyone who touches it by accident, not because this task expects it to
// move. If Lane B or Lane C's landed text ever reads differently, the fix is to update the strings
// asserted HERE, not to change their code to match text written before it existed.
#include <fstream>
#include <sstream>

#include "aver/core/Log.hpp"

#include <cmath>
#include <filesystem>
#include <random>
#include <string>

using namespace aver;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// ---------------------------------------------------------------- the identity, mirrored
//
// material_prelude.hlsl's own (unchanged) formulas, restated here with F4's subtraction already
// folded into `diffuse` -- exactly what PSMainVoxi/PSRayDriven hand material_prelude.hlsl AFTER F4
// runs, per channel. `g` is gVoxelParams.y (giIntensity); `A` stands for
// ind.ambient * ind.ambientScale * ind.occlusion * s.occlusion (voxi.hlsl's own subtraction operand,
// matching the plan's "ambient*scale*occ*matAO"); `est` is ind.diffuse BEFORE F4's subtraction, i.e.
// the ReSTIR estimate voxi_restir.hlsli's giRestirIndirect actually returned.
struct Terms { f64 kD, fmsEms, A, est, g; };

// F4's own subtraction: ind.diffuse = est - A*g (voxi.hlsl's two sites, bit 2 clear). This is the one
// place a per-pixel value can go negative -- estNonNeg only bounds `est` itself, not est minus a
// second, independently-signed term.
f64 newDiffuse(const Terms& t) { return t.est - t.A * t.g; }

// material_prelude.hlsl:999/1000, unchanged: diffAmbient = (FmsEms+kD)*A, diffBounce = kD*diffuse.
f64 diffAmbient(const Terms& t) { return (t.fmsEms + t.kD) * t.A; }
f64 sumWithDiffuse(const Terms& t, f64 diffuse) { return diffAmbient(t) + t.kD * diffuse; }

f64 legacySum(const Terms& t) { return sumWithDiffuse(t, t.est); }              // no subtraction at all
f64 fixedSum(const Terms& t)  { return sumWithDiffuse(t, newDiffuse(t)); }      // F4's subtraction applied

// ---------------------------------------------------------------- the shader source assertions
std::string readFileRaw(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Concatenates voxi.hlsl with every voxi_*.hlsli beside it, the same reader VoxiRtSeqTest.cpp and
// VoxiHemiSampleTest.cpp both use (see either file's own long comment for why: the shader is a
// directory now, and code moving between its files should not make this suite notice).
const std::string& voxiShaderText() {
    static const std::string s = [] {
        const std::string dir = std::string(AVER_REPO_ROOT) + "/modules/render.voxi/shaders";
        std::ostringstream ss;
        u32 read = 0;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            const std::string name = e.path().filename().string();
            const bool wanted = name == "voxi.hlsl" ||
                                (name.rfind("voxi_", 0) == 0 && e.path().extension() == ".hlsli");
            if (!wanted) continue;
            const std::string text = readFileRaw(e.path().string());
            if (text.empty()) continue;
            ss << text << '\n';
            ++read;
        }
        if (read == 0) {
            AVER_ERROR("[SkyOwnershipIdentity] read no shader source from {} -- every source "
                       "assertion below would pass vacuously against an empty string, so this is a "
                       "failure, not a skip.", dir);
            return std::string();
        }
        return ss.str();
    }();
    return s;
}

} // namespace

int main() {
    AVER_INFO("[SkyOwnershipIdentity] F4's sky-ownership identity (root cause R1)");

    // ---- 1. the identity, over 10k random tuples ----
    //
    // A FIXED SEED, deliberately: this is a proof over a random SAMPLE of the identity's domain, not
    // a Monte Carlo estimate of anything -- a flaky pass/fail on reseed would mean the identity is
    // only APPROXIMATELY true, which is not what F4 claims. g's range (0..8) matches gVoxelParams.y's
    // own documented ceiling (giIntensity, VoxiRenderer.hpp); A and est both range wide enough that
    // g*A regularly exceeds est, which is what drives the intermediate `diffuse` term negative --
    // exactly the case F4's own comment says the sum must still handle correctly.
    {
        std::mt19937 rng(0xA9E5u);
        std::uniform_real_distribution<f64> kdDist(0.0, 1.0);
        std::uniform_real_distribution<f64> fmsDist(0.0, 1.0);
        std::uniform_real_distribution<f64> aDist(0.0, 3.0);
        std::uniform_real_distribution<f64> estDist(0.0, 3.0);
        std::uniform_real_distribution<f64> gDist(0.0, 8.0);

        constexpr int kTuples = 10000;
        int negativeDiffuseSeen = 0;
        f64 worstNewSumErr = 0.0, worstDiffErr = 0.0;
        for (int i = 0; i < kTuples; ++i) {
            const Terms t{kdDist(rng), fmsDist(rng), aDist(rng), estDist(rng), gDist(rng)};
            const f64 diffuse = newDiffuse(t);
            if (diffuse < 0.0) ++negativeDiffuseSeen;

            const f64 got = fixedSum(t);
            const f64 expected = t.kD * t.est + t.fmsEms * t.A + (1.0 - t.g) * t.kD * t.A;
            const f64 relErr = std::fabs(got - expected) / (std::fabs(expected) > 1e-9 ? std::fabs(expected) : 1.0);
            worstNewSumErr = std::fmax(worstNewSumErr, relErr);

            const f64 diffBetween = legacySum(t) - got;
            const f64 expectedDiff = t.g * t.kD * t.A;
            const f64 diffErr = std::fabs(diffBetween - expectedDiff) /
                                 (std::fabs(expectedDiff) > 1e-9 ? std::fabs(expectedDiff) : 1.0);
            worstDiffErr = std::fmax(worstDiffErr, diffErr);
        }
        check(worstNewSumErr < 1e-6,
              "new sum = kD*est + FmsEms*A + (1-g)*kD*A over " + std::to_string(kTuples) +
              " random tuples (worst relative error " + std::to_string(worstNewSumErr) + ")");
        check(worstDiffErr < 1e-6,
              "legacy - new = g*kD*A over the same " + std::to_string(kTuples) + " tuples (worst "
              "relative error " + std::to_string(worstDiffErr) + ")");
        check(negativeDiffuseSeen > kTuples / 20,
              "the sweep actually exercises a negative intermediate diffuse term (" +
              std::to_string(negativeDiffuseSeen) + " of " + std::to_string(kTuples) +
              " tuples) -- the identity's real test is that the SUM stays correct even then, per "
              "F4's own comment that ind.diffuse may go negative while the total may not");
    }

    // ---- 2. the furnace special case: A = est = L, g = 1 collapses to (FmsEms + kD)*L / a ratio ----
    //
    // This is the GI-on furnace's own oracle (contrast-fix plan section 3.C): with the ambient term
    // and the traced ReSTIR estimate both reading the SAME uniform furnace radiance L, and giIntensity
    // at its identity value 1, F4's subtraction removes the ambient term from diffuse exactly, leaving
    // the corrected sum at (FmsEms+kD)*L -- the single-copy answer a conserving BRDF is supposed to
    // read under a uniform environment. The legacy sum keeps the doubled kD*L term, so the two differ
    // by exactly the ratio the plan states.
    {
        std::mt19937 rng(0x517Eu);
        std::uniform_real_distribution<f64> kdDist(0.01, 1.0);     // away from 0: it is a denominator below
        std::uniform_real_distribution<f64> fmsDist(0.0, 1.0);
        std::uniform_real_distribution<f64> lDist(0.1, 5.0);
        f64 worstSumErr = 0.0, worstRatioErr = 0.0;
        for (int i = 0; i < 2000; ++i) {
            const f64 kD = kdDist(rng), fmsEms = fmsDist(rng), L = lDist(rng);
            const Terms t{kD, fmsEms, /*A=*/L, /*est=*/L, /*g=*/1.0};
            const f64 fixed = fixedSum(t);
            const f64 legacy = legacySum(t);
            const f64 ratio = legacy / fixed;
            const f64 expectedRatio = (fmsEms + 2.0 * kD) / (fmsEms + kD);
            worstSumErr = std::fmax(worstSumErr, std::fabs(fixed - (fmsEms + kD) * L) / ((fmsEms + kD) * L));
            worstRatioErr = std::fmax(worstRatioErr, std::fabs(ratio - expectedRatio) / expectedRatio);
        }
        check(worstSumErr < 1e-9, "furnace fixed sum matches (FmsEms+kD)*L over 2000 samples");
        check(worstRatioErr < 1e-9,
              "furnace ratio legacy/fixed matches (FmsEms + 2kD)/(FmsEms + kD) over 2000 samples "
              "(worst relative error " + std::to_string(worstRatioErr) + ")");
    }

    // ---- 3. the shader/material source assertions ----
    {
        // material_prelude.hlsl is on the MUST-NOT-CHANGE list: this exists to catch anyone who
        // touches it by accident, and should always pass.
        const std::string matPath = std::string(AVER_REPO_ROOT) +
                                     "/modules/render.pbr/shaders/material_prelude.hlsl";
        const std::string matText = readFileRaw(matPath);
        check(!matText.empty(), "material_prelude.hlsl was read (non-empty)");
        check(matText.find("diffAmbient = (FmsEms + kD) * ind.ambient * ind.ambientScale * diffOcc;") != std::string::npos,
              "material_prelude.hlsl's diffAmbient line is unchanged -- the premise F4's identity rests on");
        check(matText.find("diffBounce  = kD * ind.diffuse;") != std::string::npos,
              "material_prelude.hlsl's diffBounce line is unchanged -- the other premise");

        // Lane B's F4 subtraction sites, voxi.hlsl (via the concatenated voxi shader reader).
        const std::string& voxiText = voxiShaderText();
        check(!voxiText.empty(), "the voxi shader directory was read (non-empty)");
        check(voxiText.find(
            "ind4.diffuse = ind - ind4.ambient * ind4.ambientScale * ind4.occlusion * s.occlusion * gVoxelParams.y;") != std::string::npos,
            "PSMainVoxi's F4 subtraction (voxi.hlsl) matches this file's mirror");
        check(voxiText.find(
            "ind.diffuse -= ind.ambient * ind.ambientScale * ind.occlusion * s.occlusion * gVoxelParams.y;") != std::string::npos,
            "PSRayDriven's F4 subtraction (voxi.hlsl) matches this file's mirror");

        // Lane C's F6 gate, pt_pathtrace.hlsl -- unrelated to the identity above, but part of this
        // suite's contract-string coverage per the brief that named this file T3.
        const std::string ptPath = std::string(AVER_REPO_ROOT) + "/modules/render.pt/shaders/pt_pathtrace.hlsl";
        const std::string ptText = readFileRaw(ptPath);
        check(!ptText.empty(), "pt_pathtrace.hlsl was read (non-empty)");
        check(ptText.find("gPtTrace.z < 0.5 && lastDiffuse") != std::string::npos,
              "pt_pathtrace.hlsl's F6 miss gate (root cause R5) matches the contract string");
    }

    if (g_failures == 0) {
        AVER_INFO("[SkyOwnershipIdentity] PASS: the sky-ownership identity holds over 10k random "
                  "tuples including negative intermediate diffuse, the furnace special case matches "
                  "the plan's own ratio, and every source string this file depends on is present");
        return 0;
    }
    AVER_ERROR("[SkyOwnershipIdentity] FAIL: {} check(s)", g_failures);
    return 1;
}
