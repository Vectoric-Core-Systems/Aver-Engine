// The lighting-contrast fix's F5 cone weights (contrast-fix plan, root cause R6): the voxel cone
// gather's directions are drawn from a cosine-weighted hemisphere and then weighted by cosine a
// SECOND time on the way in, which squares the distribution the gather is supposed to integrate
// against. Checked here with no GPU, the same way VoxiHemiSampleTest.cpp beside this file checks F1.
//
// WHY THIS IS A TEST AND NOT A COMMENT. The bug is exact and closed-form -- coneTracedIndirect draws
// `cones` fixed directions every pixel, every frame, so there is nothing for temporal accumulation to
// average out and nothing a screenshot shows directly: a gather biased toward the surface normal
// still looks like ambient light, just the wrong AMOUNT of it, in a way that folds into every other
// approximation already in the same pixel. The only way to see the defect is to compute the same
// weighted moment the shader computes and compare it to the true cosine-hemisphere answer (2/3, 1/2).
//
// STYLE: same shape as VoxiRtSeqTest.cpp/VoxiHemiSampleTest.cpp beside this file -- links Aver.Core
// and nothing else, no GPU, no RHI; a C++ mirror of the shader's own weighted average; a negative
// control that must fail; source assertions tying the mirror to the actual HLSL text.
//
// WHAT THIS MIRRORS, EXACTLY, AND WHAT IT SIMPLIFIES: coneTracedIndirect draws `ring = cones - 1`
// directions around N plus the axial cone itself, at a golden-angle azimuth and a stratified
// elevation, and accumulates a WEIGHTED average `sum(w_i * value_i) / sum(w_i)`. Because N is the
// only axis cos(theta) depends on, and every direction's azimuth is rotationally symmetric around N
// by construction, cos(theta)_i = dot(N, d_i) is fully determined by each cone's ELEVATION term
// alone -- so this file mirrors the elevation/weight pair (t, cosT, w) per cone directly, rather than
// building 3D direction vectors and an azimuth this test has no use for. Azimuth is exercised
// instead, over actual 3D directions, by VoxiHemiSampleTest.cpp's |E[e^{i phi}]| check for the
// unrelated ReSTIR/sky-occlusion sampler -- this file's own claim is about elevation only.
//
// LANE NOTE: this file's author (Lane D) owns no shader (.hlsli) files. Lane C owns voxi_cone.hlsli
// and voxi_gi.hlsli; the elevation/weight formulas mirrored below are read from voxi_cone.hlsli as it
// stood while this file was written, which is also the CONTRACT text F5 specifies. If Lane C's
// landed text ever reads differently, cosmetically or otherwise, the fix is to update the mirror and
// the source assertions HERE to match what shipped, not to change Lane C's code to match text written
// before it existed.
//
// FOLLOW-UP (gi-cones lane): section 4 below used to end with a note that voxi_gi.hlsli's own
// coneTracedIndirect (the cluster-material and particle passes) still ran a THIRD, uncorrected
// gather -- a fixed 60-degree ring at one elevation, not stratified at all, nothing like either of
// voxi_cone.hlsli's two schemes above. That gap is closed: voxi_gi.hlsli now carries the SAME
// corrected gather behind the SAME gAmbientParams.z bit 16, with its own legacy branch restoring
// its own old fixed ring for comparison. This file now also mirrors THAT old scheme (giLegacyMoments
// below) the same way it already mirrors voxi_cone.hlsli's legacy/new pair, and the old section-4
// "byte-identical" question is answered rather than deferred a second time.
#include <fstream>
#include <sstream>

#include "aver/core/Log.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

using namespace aver;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// ---------------------------------------------------------------- C++ mirrors of the shader
//
// One cone's (cosTheta, weight) pair, legacy or new, mirroring coneTracedIndirect's ring loop body
// (voxi_cone.hlsli) elevation-for-elevation.
struct ConeSample { f64 cosTheta; f64 weight; };

// `k` in [0, ring), `ring` = N - 1, `legacy` selects gAmbientParams.z bit 16's reinstated behaviour.
ConeSample ringCone(u32 k, u32 ring, u32 cones, bool legacy) {
    const f64 t = legacy
        ? (static_cast<f64>(k) + 0.5) / static_cast<f64>(ring)                    // legacy: t over `ring`
        : (static_cast<f64>(k) + 1.0 + 0.5) / static_cast<f64>(cones);            // new: t over `cones`, stratum k+1
    const f64 cosT = std::sqrt(std::max(0.0, 1.0 - t));
    // Legacy weights by dot(N,d) == cosT a SECOND time (the bug); new weights every ring cone 1, the
    // same as the axial cone -- the cosine weighting already lives in how t (and so cosT) was drawn.
    const f64 w = legacy ? cosT : 1.0;
    return ConeSample{cosT, w};
}

struct Moments { f64 eCos; f64 eCos2; };

// The full `cones`-direction weighted average coneTracedIndirect actually accumulates: the axial
// cone (cosTheta = 1, weight = 1, traced once before the ring loop) plus `ring` = cones - 1 ring
// cones from ringCone() above. N = 1 means axial only, matching the shader's own `ring = cones - 1`
// underflowing to 0 cones traced in the loop.
Moments coneMoments(u32 cones, bool legacy) {
    const u32 n = std::max(cones, 1u);
    f64 sumW = 1.0, sumWCos = 1.0, sumWCos2 = 1.0;   // the axial cone
    const u32 ring = n - 1u;
    for (u32 k = 0; k < ring; ++k) {
        const ConeSample s = ringCone(k, ring, n, legacy);
        sumW     += s.weight;
        sumWCos  += s.weight * s.cosTheta;
        sumWCos2 += s.weight * s.cosTheta * s.cosTheta;
    }
    return Moments{sumWCos / sumW, sumWCos2 / sumW};
}

// Relative error against the true cosine-hemisphere moments (2/3, 1/2) -- the SAME normalisation the
// contrast-fix plan's own F5/R6 tables use ("+16.5%" etc.), confirmed by hand against this file's own
// worked example below for N=13.
f64 relErr(f64 got, f64 target) { return std::fabs(got - target) / target; }

// voxi_gi.hlsli's OWN pre-fix ring, mirrored the same way ringCone()/coneMoments() above mirror
// voxi_cone.hlsli's: every ring cone there was `normalize(N * 0.5 + tangent * 0.866)`, and N*0.5 and
// tangent*0.866 are already orthogonal with |N*0.5|^2 + |tangent*0.866|^2 = 0.25 + 0.75 = 1, so the
// vector is unit length BEFORE normalize() touches it and normalize() is a no-op. That makes
// cos(theta) = dot(N, d) = 0.5 for EVERY ring cone, independent of k and of the cone count entirely --
// not cosine-distributed at all, just one fixed 60-degree elevation repeated `ring` times -- and the
// weight (dot(N,d) again) is the same constant 0.5. Unlike voxi_cone.hlsli's ringCone(), there is no
// `legacy` parameter: this scheme's constant {cosTheta, weight} = {0.5, 0.5} never depended on the
// stratified formula it now sits alongside, so there is nothing to select between here.
ConeSample giLegacyRingCone() { return ConeSample{0.5, 0.5}; }

// Same accumulation shape as coneMoments() above (axial cone at weight 1, plus `ring` = cones - 1
// ring cones), but every ring cone is voxi_gi.hlsli's own constant sample rather than ringCone()'s.
Moments giLegacyMoments(u32 cones) {
    const u32 n = std::max(cones, 1u);
    f64 sumW = 1.0, sumWCos = 1.0, sumWCos2 = 1.0;   // the axial cone
    const u32 ring = n - 1u;
    for (u32 k = 0; k < ring; ++k) {
        (void)k;   // every ring cone is the same sample -- k only selects azimuth, which cos(theta) never depends on
        const ConeSample s = giLegacyRingCone();
        sumW     += s.weight;
        sumWCos  += s.weight * s.cosTheta;
        sumWCos2 += s.weight * s.cosTheta * s.cosTheta;
    }
    return Moments{sumWCos / sumW, sumWCos2 / sumW};
}

// ---------------------------------------------------------------- the shader source assertions
//
// UNLIKE VoxiRtSeqTest.cpp/VoxiHemiSampleTest.cpp beside this file, this reads voxi_cone.hlsli and
// voxi_gi.hlsli SEPARATELY rather than concatenating every voxi_*.hlsli into one blob -- the claims
// section 5 below needs to check (which file's legacy branch has which old code; whether a given
// slice of the corrected math matches between the two) are exactly the kind a concatenated blob
// cannot express, since it would erase which file each copy came from.
std::string readFileRaw(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// The text of a shader function, from its signature to the function's OWN closing brace -- found as
// the first "\n}" (a closing brace alone on its line, at column 0) after the signature, exactly like
// VoxiRtSeqTest.cpp's own hlslBody() helper. That is unambiguous here even though the function
// contains a nested `for` block, because every line inside the function is indented and only the
// function's own closing brace sits at column 0.
std::string functionBody(const std::string& text, const char* signature) {
    const std::size_t start = text.find(signature);
    if (start == std::string::npos) return {};
    const std::size_t end = text.find("\n}", start);
    return end == std::string::npos ? text.substr(start) : text.substr(start, end - start);
}

// Extracts the substring from the first occurrence of `startAnchor` through (and including) the next
// occurrence of `endAnchor` after it. Used below to pull a stable, precisely-bounded slice of code out
// of a function body -- not the whole body -- for comparison between two files.
std::string sliceBetween(const std::string& text, const std::string& startAnchor, const std::string& endAnchor) {
    const std::size_t start = text.find(startAnchor);
    if (start == std::string::npos) return {};
    const std::size_t endAnchorPos = text.find(endAnchor, start);
    if (endAnchorPos == std::string::npos) return {};
    const std::size_t end = endAnchorPos + endAnchor.size();
    return text.substr(start, end - start);
}

// Collapses every run of whitespace (spaces, tabs, newlines) to a single space and drops it entirely
// at the ends -- used below to compare the SAME code sitting at two different indentation depths in
// two files, where raw substring equality would fail on indentation alone.
std::string normalizeWs(const std::string& text) {
    std::string out;
    bool pendingSpace = false;
    for (unsigned char c : text) {
        if (std::isspace(c)) {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) { out.push_back(' '); pendingSpace = false; }
        out.push_back(static_cast<char>(c));
    }
    return out;
}

} // namespace

int main() {
    AVER_INFO("[ConeWeight] F5's cone weights (root cause R6): cosine-distributed AND cosine-weighted");

    // ---- 1. worked example: N=13 (Epic), against the exact numbers voxi_cone.hlsli's own comment
    // and the contrast-fix plan's F5 table both cite, so a transcription error in the mirror above
    // shows up immediately rather than only inside a threshold check. ----
    {
        const Moments legacy13 = coneMoments(13, true);
        const Moments new13    = coneMoments(13, false);
        AVER_INFO("  N=13 legacy: E[cos]={:.4f} E[cos^2]={:.4f}  (plan: 0.7764 / 0.6428)",
                  legacy13.eCos, legacy13.eCos2);
        AVER_INFO("  N=13 new:    E[cos]={:.4f} E[cos^2]={:.4f}  (plan: 0.6694 / 0.5030)",
                  new13.eCos, new13.eCos2);
        check(std::fabs(legacy13.eCos - 0.7764) < 5e-4, "legacy E[cos] at 13 cones matches the plan's worked example");
        check(std::fabs(legacy13.eCos2 - 0.6428) < 5e-4, "legacy E[cos^2] at 13 cones matches the plan's worked example");
        check(std::fabs(new13.eCos - 0.6694) < 5e-4, "new E[cos] at 13 cones matches the plan's worked example");
        check(std::fabs(new13.eCos2 - 0.5030) < 5e-4, "new E[cos^2] at 13 cones matches the plan's worked example");
    }

    // ---- 2. the full N = 1..16 sweep ----
    {
        constexpr f64 kTrueCos  = 2.0 / 3.0;
        constexpr f64 kTrueCos2 = 0.5;
        bool newBelowLegacyForAll = true;
        for (u32 n = 1; n <= 16; ++n) {
            const Moments legacy = coneMoments(n, true);
            const Moments fixed  = coneMoments(n, false);
            const f64 legacyErrCos = relErr(legacy.eCos, kTrueCos);
            const f64 newErrCos    = relErr(fixed.eCos, kTrueCos);
            AVER_INFO("  N={:>2}  legacy E[cos]={:.4f} ({:+.1f}%)   new E[cos]={:.4f} ({:+.1f}%)",
                      n, legacy.eCos, (legacy.eCos / kTrueCos - 1.0) * 100.0,
                      fixed.eCos, (fixed.eCos / kTrueCos - 1.0) * 100.0);

            if (n >= 13) check(newErrCos <= 0.01, "N=" + std::to_string(n) + ": new E[cos theta] error <= 1%");
            else if (n >= 6) check(newErrCos <= 0.03, "N=" + std::to_string(n) + ": new E[cos theta] error <= 3%");
            else if (n >= 3) check(newErrCos <= 0.12, "N=" + std::to_string(n) + ": new E[cos theta] error <= 12%");

            const f64 legacyErrCos2 = relErr(legacy.eCos2, kTrueCos2);
            const f64 newErrCos2    = relErr(fixed.eCos2, kTrueCos2);
            if (n >= 13) check(newErrCos2 <= 0.01, "N=" + std::to_string(n) + ": new E[cos^2 theta] error <= 1%");
            else if (n >= 6) check(newErrCos2 <= 0.03, "N=" + std::to_string(n) + ": new E[cos^2 theta] error <= 3%");
            else if (n >= 3) check(newErrCos2 <= 0.12, "N=" + std::to_string(n) + ": new E[cos^2 theta] error <= 12%");

            if (n >= 2 && !(newErrCos < legacyErrCos && newErrCos2 < legacyErrCos2)) {
                newBelowLegacyForAll = false;
                AVER_ERROR("  FAIL  N={}: new error ({}, {}) is not below legacy ({}, {})",
                           n, newErrCos, newErrCos2, legacyErrCos, legacyErrCos2);
            }

            // NEGATIVE CONTROL, for N in 3..16: the legacy scheme's E[cos theta] error must stay
            // above 10% -- proof this check can see the very defect F5 fixes.
            if (n >= 3 && n <= 16) {
                check(legacyErrCos > 0.10,
                      "NEGATIVE CONTROL: N=" + std::to_string(n) + " legacy E[cos theta] error " +
                      std::to_string(legacyErrCos * 100.0) + "% is over 10%");
            }
        }
        check(newBelowLegacyForAll,
              "the new scheme's error is below the legacy scheme's, in BOTH moments, for every N in [2, 16]");
    }

    // ---- 3. N=1 is axial only, for both schemes ----
    {
        const Moments legacy1 = coneMoments(1, true);
        const Moments new1    = coneMoments(1, false);
        check(legacy1.eCos == 1.0 && legacy1.eCos2 == 1.0 && new1.eCos == 1.0 && new1.eCos2 == 1.0,
              "N=1 traces only the axial cone (cos theta = 1) under either scheme");
    }

    // ---- 4. voxi_gi.hlsli's OWN legacy scheme: the full N = 1..16 sweep and a negative control ----
    //
    // voxi_gi.hlsli's coneTracedIndirect (the cluster-material and particle passes) never ran either
    // of the two schemes above -- see giLegacyRingCone()'s own comment for why its ring collapses to a
    // CONSTANT cos(theta) = 0.5 regardless of k or the cone count: a fixed 60-degree ring, not a
    // cosine-distributed one at all. That is a DIFFERENT failure mode from voxi_cone.hlsli's legacy
    // scheme above (which over-weights toward the normal and grows steadily worse as N grows): this one
    // over- or under-estimates depending on N, crossing the true value near N=5 by coincidence of its
    // own constants (worked out below), so its negative control is scoped to the N range where the
    // error demonstrably stays large, not claimed for the whole 1..16 sweep the way section 2's is.
    {
        constexpr f64 kTrueCos  = 2.0 / 3.0;
        constexpr f64 kTrueCos2 = 0.5;
        for (u32 n = 1; n <= 16; ++n) {
            const Moments giLegacy = giLegacyMoments(n);
            const Moments fixed    = coneMoments(n, false);  // the corrected scheme, now shared by both files
            AVER_INFO("  N={:>2}  voxi_gi legacy E[cos]={:.4f} ({:+.1f}%) E[cos^2]={:.4f} ({:+.1f}%)   "
                      "corrected E[cos]={:.4f} E[cos^2]={:.4f}",
                      n, giLegacy.eCos, (giLegacy.eCos / kTrueCos - 1.0) * 100.0,
                      giLegacy.eCos2, (giLegacy.eCos2 / kTrueCos2 - 1.0) * 100.0,
                      fixed.eCos, fixed.eCos2);
        }

        // WORKED EXAMPLE at N=13 (the project's own tier, same N section 1 already worked for the other
        // scheme): sumW = 1 + 12*0.5 = 7, sumWCos = 1 + 12*(0.5*0.5) = 4, sumWCos2 = 1 + 12*(0.5*0.25) = 2.5.
        const Moments giLegacy13 = giLegacyMoments(13);
        AVER_INFO("  N=13 voxi_gi legacy: E[cos]={:.4f} E[cos^2]={:.4f}  (worked: 4/7=0.5714 / 2.5/7=0.3571)",
                  giLegacy13.eCos, giLegacy13.eCos2);
        check(std::fabs(giLegacy13.eCos - 4.0 / 7.0) < 5e-4,
              "voxi_gi legacy E[cos] at 13 cones matches the worked example (4/7)");
        check(std::fabs(giLegacy13.eCos2 - 2.5 / 7.0) < 5e-4,
              "voxi_gi legacy E[cos^2] at 13 cones matches the worked example (2.5/7)");

        // NEGATIVE CONTROL: for N in [10, 16] this scheme's error is comfortably over 8%/15% in the two
        // moments respectively (actual values run ~11%-16% and ~23%-32%; the bound leaves margin) --
        // proof this check can see voxi_gi.hlsli's OWN pre-fix defect, not only voxi_cone.hlsli's.
        for (u32 n = 10; n <= 16; ++n) {
            const Moments giLegacy = giLegacyMoments(n);
            const f64 errCos  = relErr(giLegacy.eCos, kTrueCos);
            const f64 errCos2 = relErr(giLegacy.eCos2, kTrueCos2);
            check(errCos > 0.08,
                  "NEGATIVE CONTROL: N=" + std::to_string(n) + " voxi_gi legacy E[cos] error " +
                  std::to_string(errCos * 100.0) + "% is over 8%");
            check(errCos2 > 0.15,
                  "NEGATIVE CONTROL: N=" + std::to_string(n) + " voxi_gi legacy E[cos^2] error " +
                  std::to_string(errCos2 * 100.0) + "% is over 15%");
        }

        // THE CORRECTED SCHEME voxi_gi.hlsli NOW SHARES is exactly coneMoments(n, false), already
        // checked against the plan's error bounds in section 2 above -- there is only ONE corrected
        // scheme now, not a third one needing its own bounds, so nothing further to assert here.
    }

    // ---- 5. the shader source assertions: the corrected weights and legacy bit are in BOTH files ----
    //
    // READ INDEPENDENTLY, not through the concatenated-blob reader VoxiRtSeqTest.cpp/
    // VoxiHemiSampleTest.cpp use, so each file's copy can be told apart.
    //
    // THIS USED TO ASSERT THE TWO BODIES WERE NECESSARILY DIFFERENT (voxi_gi.hlsli had no legacy bit at
    // all and an uncorrected ring unlike either of voxi_cone.hlsli's two). That gap is closed: both
    // files now read the same bit and run the same corrected math when it is off. The two FUNCTIONS are
    // still not whole-body-identical -- voxi_cone.hlsli branches only its ring loop, voxi_gi.hlsli
    // branches the whole gather, because the two files' PRE-FIX code differed in scope (see
    // voxi_gi.hlsli's own coneTracedIndirect comment for why) -- so this section checks what is
    // actually true: each file's legacy branch preserves ITS OWN old code, and the corrected MATH (not
    // the raw bytes, which sit at different nesting depths in the two files) is identical once
    // whitespace is normalised.
    {
        const std::string coneDir = std::string(AVER_REPO_ROOT) + "/modules/render.voxi/shaders";
        const std::string coneText = readFileRaw(coneDir + "/voxi_cone.hlsli");
        const std::string giText   = readFileRaw(coneDir + "/voxi_gi.hlsli");
        check(!coneText.empty(), "voxi_cone.hlsli was read (non-empty)");
        check(!giText.empty(), "voxi_gi.hlsli was read (non-empty)");

        const char* sig = "float3 coneTracedIndirect(float3 wpos, float3 N, out float ao) {";
        const std::string coneBody = functionBody(coneText, sig);
        const std::string giBody   = functionBody(giText, sig);
        check(!coneBody.empty(), "voxi_cone.hlsli's coneTracedIndirect body was located");
        check(!giBody.empty(), "voxi_gi.hlsli's coneTracedIndirect body was located");

        // BOTH FILES NOW READ THE SAME BIT.
        check(coneBody.find("((uint)gAmbientParams.z & 16u) != 0u") != std::string::npos,
              "voxi_cone.hlsli's coneTracedIndirect reads legacy bit 16 (voxi.legacyConeWeights)");
        check(giBody.find("((uint)gAmbientParams.z & 16u) != 0u") != std::string::npos,
              "voxi_gi.hlsli's coneTracedIndirect ALSO now reads legacy bit 16");

        // EACH FILE'S LEGACY BRANCH PRESERVES ITS OWN OLD CODE -- checked against its OWN pre-fix
        // formula, not the other file's, because the two pre-fix gathers genuinely differed.
        check(coneBody.find("if (legacyConeWeights)") != std::string::npos,
              "voxi_cone.hlsli's coneTracedIndirect keeps ITS OWN legacy cone weights behind that bit");
        check(giBody.find("N * 0.5 + (T * cos(ang) + B * sin(ang)) * 0.866") != std::string::npos,
              "voxi_gi.hlsli's legacy branch keeps ITS OWN pre-fix fixed ring, byte-for-byte");
        check(giBody.find("const float aperture = 0.577;") != std::string::npos,
              "voxi_gi.hlsli's legacy branch keeps ITS OWN pre-fix fixed aperture, byte-for-byte");

        // THE CORRECTED MATH IS NOW THE SAME IN BOTH FILES -- the actual gap this lane closes. Compared
        // as whitespace-normalised slices (runs of whitespace collapsed to one space) rather than whole-
        // function equality, because voxi_cone.hlsli's corrected ring lives one nesting level deeper
        // (inside its own `if (legacyConeWeights) {...} else { HERE }`) than voxi_gi.hlsli's does (a
        // top-level `if (...) {legacy...} HERE`, no enclosing else) -- see both files' own
        // coneTracedIndirect comments for why the shapes differ even though this text no longer does.
        const std::string coneAperture = normalizeWs(sliceBetween(coneBody, "const float cosHalf", "// tan"));
        const std::string giAperture   = normalizeWs(sliceBetween(giBody,   "const float cosHalf", "// tan"));
        check(!coneAperture.empty() && !giAperture.empty(),
              "both files' cone-count-derived aperture formulas were located");
        check(coneAperture == giAperture,
              "the cone-count-derived aperture formula is identical (whitespace aside) in both files");

        const std::string coneRing = normalizeWs(sliceBetween(coneBody,
              "float t    = ((float)(k + 1u)", "occ += c.a * cw; occWsum += cw;"));
        const std::string giRing = normalizeWs(sliceBetween(giBody,
              "float t    = ((float)(k + 1u)", "occ += c.a * cw; occWsum += cw;"));
        check(!coneRing.empty() && !giRing.empty(), "both files' corrected ring-loop bodies were located");
        check(coneRing == giRing,
              "the corrected ring-loop's direction/weight formula is identical (whitespace aside) in both files");
    }

    if (g_failures == 0) {
        AVER_INFO("[ConeWeight] PASS: the new cone weights are within the plan's error bounds at "
                  "every cone count that matters, strictly better than the legacy scheme from N=2 up, "
                  "the check has been seen failing against the legacy scheme, and both files agree");
        return 0;
    }
    AVER_ERROR("[ConeWeight] FAIL: {} check(s)", g_failures);
    return 1;
}
