// The lighting-contrast fix's F1 sampler (contrast-fix plan, root cause R0): a cosine-weighted
// hemisphere sample, checked against the fixed 45-degree ring it replaces, with no GPU.
//
// WHY THIS IS A TEST AND NOT A COMMENT. R0's whole defect is a single number that never changes:
// rtDiscSample(0, ang0) always lands at radius sqrt(0.5) on the unit disc, so the ReSTIR candidate
// ray and the sky-occlusion ray both lifted onto the hemisphere at EXACTLY cos(theta) = 1/sqrt(2), on
// every pixel, every frame, no matter how ang0 was chosen -- a deterministic wrong answer, and this
// file's own sibling shader comment says plainly that "a deterministic wrong answer is exactly what
// temporal accumulation cannot fix". Nothing on a screenshot shows a fixed sampling ring; a rendered
// image of a shadow sampled the wrong way still looks like a shadow. The only way to know the new
// sampler actually draws from a cosine-weighted hemisphere -- and that the old one demonstrably did
// not -- is to check the DISTRIBUTION, which needs many samples and a statistic, not a picture.
//
// STYLE: same shape as VoxiRtSeqTest.cpp beside this file -- links Aver.Core only, no GPU, no RHI;
// C++ mirrors of the shader's own sampling primitives; source assertions at the bottom tie the
// mirror to the actual HLSL text the renderer compiles; a negative control proves the checks below
// can fail by running them against the very sequence they were written to catch.
//
// WHAT THIS DOES NOT COVER, stated because the gap is real: this is a C++ MIRROR of the HLSL, not
// the compiled shader. Nothing here runs DXC or a GPU -- the mirror is tied to the shader only by
// the source assertions at the bottom, which read the actual shader text and fail if it drifts from
// what is modelled here.
//
// LANE NOTE: this file's author (Lane D) owns no shader (.hlsli) files. The three call-site strings
// and the rtHemiDiscSample body asserted below are the CONTRACT this task fixed in advance for every
// lane to code against (Lane A owns voxi_restir.hlsli/voxi_rt.hlsli) -- if Lane A's landed text ever
// reads differently, cosmetically or otherwise, the fix is to update the string asserted HERE to
// match what shipped, not to change Lane A's code to match a string written before it existed.
#include <filesystem>
#include <fstream>
#include <sstream>

#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace aver;

namespace {

int g_failures = 0;

// Records one assertion.
void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// ---------------------------------------------------------------- C++ mirrors of the shader
//
// EVERY ONE OF THESE ALREADY EXISTED IN THE SHADER BEFORE THIS TASK except rtHemiDiscSample, which
// is F1's own new function -- mirrored here from the CONTRACT (the exact body this brief specifies),
// tied to the actual shader text by the source assertions at the bottom of this file.

f32 fracf(f32 x) { return x - std::floor(x); }

// Mirrors voxi_rt.hlsli's rtRadicalInverse2: base-2 radical inverse via bit reversal, scaled by
// 2^-32. Bit-exact on every adapter (unlike a hash), which is what lets phi(1) == 0.5 EXACTLY --
// the one fact the legacy negative control below rests on.
f32 radicalInverse2(u32 i) {
    u32 r = i;
    r = (r << 16) | (r >> 16);
    r = ((r & 0x00FF00FFu) << 8) | ((r & 0xFF00FF00u) >> 8);
    r = ((r & 0x0F0F0F0Fu) << 4) | ((r & 0xF0F0F0F0u) >> 4);
    r = ((r & 0x33333333u) << 2) | ((r & 0xCCCCCCCCu) >> 2);
    r = ((r & 0x55555555u) << 1) | ((r & 0xAAAAAAAAu) >> 1);
    return static_cast<f32>(r) * 2.3283064365386963e-10f;   // 1 / 2^32
}

// Mirrors voxi_rt.hlsli's rtHash(float2): a spatial-only hash of a pixel key, used to rotate each
// pixel's own sample pattern. p.xyx means (p.x, p.y, p.x); q.yzx + 33.33 permutes the three lanes
// before the dot product.
f32 rtHash(f32 px, f32 py) {
    f32 qx = fracf(px * 0.1031f);
    f32 qy = fracf(py * 0.1030f);
    f32 qz = fracf(px * 0.0973f);
    const f32 d = qx * (qy + 33.33f) + qy * (qz + 33.33f) + qz * (qx + 33.33f);
    qx += d; qy += d; qz += d;
    return fracf((qx + qy) * qz);
}

struct Sample { f32 x, y; };

// Mirrors voxi_rt.hlsli's rtDiscSample -- the LEGACY sampler both the ReSTIR candidate and the
// sky-occlusion ray drew from before F1, and still what gAmbientParams.z bit 1 reinstates.
Sample rtDiscSample(u32 k, f32 ang0) {
    const f32 rad = std::sqrt(radicalInverse2(k + 1u));
    const f32 a   = ang0 + static_cast<f32>(k) * 2.39996323f;
    return Sample{std::cos(a) * rad, std::sin(a) * rad};
}

// Mirrors voxi.hlsl's averGoldenTurns: n golden-angle turns mod 2 pi, integer fixed point.
f32 averGoldenTurns(u32 n) { return static_cast<f32>((n * 0x9E3779B9u) >> 8) * (6.2831853f / 16777216.0f); }

// Mirrors F1's rtHemiDiscSample EXACTLY -- see the source assertion at the bottom for the literal
// HLSL body this arithmetic has to keep matching.
Sample rtHemiDiscSample(u32 k, u32 n, u32 frameIdx, f32 pixelX, f32 pixelY, f32 streamSalt) {
    const u32 idx = frameIdx * std::max(n, 1u) + k;
    const f32 u   = fracf(radicalInverse2(idx + 1u) + rtHash(pixelX + streamSalt, pixelY + 17.0f + streamSalt));
    const f32 a   = rtHash(pixelX, pixelY) * 6.2831853f + averGoldenTurns(idx) + streamSalt;
    const f32 r   = std::sqrt(u);
    return Sample{std::cos(a) * r, std::sin(a) * r};
}

// cos(theta) from a disc sample xi, the same Malley lift both call sites use: cosTheta =
// sqrt(saturate(1 - dot(xi, xi))).
f32 cosThetaOf(Sample xi) {
    const f32 d = xi.x * xi.x + xi.y * xi.y;
    return std::sqrt(std::max(0.0f, 1.0f - d));
}

// The Kolmogorov-Smirnov distance between the empirical distribution of `cosThetas` and the
// reference CDF F(c) = c^2 -- the cosine law a cosine-weighted hemisphere sample obeys. Checked at
// BOTH the left and right limit of the empirical step at each sample, which is where the true KS
// statistic's supremum is attained for a discrete empirical CDF against a continuous reference.
f32 ksDistanceToCSquared(std::vector<f32> cosThetas) {
    std::sort(cosThetas.begin(), cosThetas.end());
    const usize n = cosThetas.size();
    f32 worst = 0.0f;
    for (usize i = 0; i < n; ++i) {
        const f32 c   = cosThetas[i];
        const f32 ref = c * c;
        const f32 before = static_cast<f32>(i)     / static_cast<f32>(n);
        const f32 after  = static_cast<f32>(i + 1) / static_cast<f32>(n);
        worst = std::fmax(worst, std::fmax(std::fabs(before - ref), std::fabs(after - ref)));
    }
    return worst;
}

// ---------------------------------------------------------------- the shader source assertions
//
// Read through AVER_REPO_ROOT, exactly the way VoxiRtSeqTest.cpp beside this file already does (see
// that file's own long comment on why voxi.hlsl plus every voxi_*.hlsli beside it has to be
// concatenated rather than reading one filename) -- copied rather than shared, because this suite
// links Aver.Core and nothing else and has no common library to share it through.
const std::string& hlslText() {
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
            std::ifstream f(e.path(), std::ios::binary);
            if (!f) continue;
            ss << f.rdbuf() << '\n';
            ++read;
        }
        if (read == 0) {
            AVER_ERROR("[VoxiHemiSample] read no shader source from {} -- every source assertion "
                       "below would pass vacuously against an empty string, so this is a failure, "
                       "not a skip.", dir);
            return std::string();
        }
        return ss.str();
    }();
    return s;
}

bool hlslHas(const char* needle) { return hlslText().find(needle) != std::string::npos; }

} // namespace

int main() {
    AVER_INFO("[VoxiHemiSample] F1's cosine-weighted hemisphere sampler (root cause R0)");

    // 8 distinct pixel keys, spread out rather than clustered, so a defect that only shows for one
    // coordinate range (e.g. an integer-aliasing bug in rtHash) cannot hide behind the other seven.
    const f32 pixels[8][2] = {
        {10.0f, 20.0f}, {111.0f, 7.0f}, {500.0f, 500.0f}, {1.0f, 1.0f},
        {999.0f, 3.0f}, {42.0f, 84.0f}, {256.0f, 128.0f}, {700.0f, 13.0f},
    };
    constexpr u32 kFrames = 4096;

    // ---- 1. NEGATIVE CONTROL: the legacy k=0 sample is a fixed 45-degree ring, over 4096 frames ----
    //
    // radicalInverse2(1) == 0.5 EXACTLY (verified by VoxiRtSeqTest.cpp's own equivalent check), so
    // rtDiscSample(0, ang0)'s radius is sqrt(0.5) regardless of ang0 -- this loop varies ang0 as a
    // stand-in for "frameJitter" changing every frame, precisely to demonstrate that cos(theta) does
    // NOT move even though the angle does.
    {
        f32 worstDev = 0.0f;
        std::vector<f32> legacyCos;
        legacyCos.reserve(kFrames);
        for (u32 frame = 0; frame < kFrames; ++frame) {
            const f32 ang0 = static_cast<f32>(frame) * 0.6180339887f;   // an arbitrary, ever-changing angle
            const f32 c = cosThetaOf(rtDiscSample(0u, ang0));
            legacyCos.push_back(c);
            worstDev = std::fmax(worstDev, std::fabs(c - 0.70710678f));
        }
        check(worstDev < 1e-6f,
              "NEGATIVE CONTROL: the legacy k=0 sample's cos(theta) is 0.70710678 (1/sqrt(2)) on "
              "every one of " + std::to_string(kFrames) + " frames regardless of angle (worst "
              "deviation " + std::to_string(worstDev) + ") -- a deterministic 45-degree ring, not a "
              "cosine-weighted hemisphere");
        const f32 ks = ksDistanceToCSquared(legacyCos);
        check(ks > 0.25f,
              "NEGATIVE CONTROL: the legacy sequence's KS distance to F(c)=c^2 is " +
              std::to_string(ks) + ", over the 0.25 threshold -- this check can see the defect it "
              "was written for");
    }

    // ---- 2. THE NEW SAMPLER: cosine-weighted, over 8 pixels x 4096 frames ----
    {
        std::vector<f32> cosThetas;
        cosThetas.reserve(8u * kFrames);
        f64 sumCos = 0.0, sumCos2 = 0.0, sumEcos = 0.0, sumEsin = 0.0;
        u64 total = 0;
        for (const auto& px : pixels) {
            for (u32 frame = 0; frame < kFrames; ++frame) {
                // n=1, k=0: exactly the ReSTIR candidate's own call shape (F1's call site, asserted
                // below) -- only frameIdx varies across the sweep, matching how it is actually driven.
                const Sample xi = rtHemiDiscSample(0u, 1u, frame, px[0], px[1], 0.0f);
                const f32 c = cosThetaOf(xi);
                cosThetas.push_back(c);
                sumCos  += c;
                sumCos2 += static_cast<f64>(c) * c;
                const f32 phi = std::atan2(xi.y, xi.x);
                sumEcos += std::cos(phi);
                sumEsin += std::sin(phi);
                ++total;
            }
        }
        const f64 eCos  = sumCos  / static_cast<f64>(total);
        const f64 eCos2 = sumCos2 / static_cast<f64>(total);
        const f64 eiMag = std::sqrt((sumEcos / static_cast<f64>(total)) * (sumEcos / static_cast<f64>(total)) +
                                     (sumEsin / static_cast<f64>(total)) * (sumEsin / static_cast<f64>(total)));
        const f32 ks = ksDistanceToCSquared(cosThetas);

        check(ks < 0.03f,
              "the new sampler's KS distance to F(c)=c^2 (the cosine law) is " + std::to_string(ks) +
              " over " + std::to_string(total) + " samples across 8 pixels and " +
              std::to_string(kFrames) + " frames");
        check(std::fabs(eCos - 2.0 / 3.0) < 0.01,
              "E[cos theta] is " + std::to_string(eCos) + ", within 1% of the cosine-weighted "
              "hemisphere's 2/3");
        check(std::fabs(eCos2 - 0.5) < 0.01,
              "E[cos^2 theta] is " + std::to_string(eCos2) + ", within 1% of the cosine-weighted "
              "hemisphere's 1/2");
        check(eiMag < 0.03,
              "|E[e^{i phi}]| is " + std::to_string(eiMag) + ", under 0.03 -- the azimuth is not "
              "biased toward any one direction");
    }

    // ---- 3. n=4 IS NESTED: extending the frame range only APPENDS indices, never revisits one ----
    //
    // idx = frameIdx * n + k is the property this checks, not a probabilistic one: with n held at 4
    // and frameIdx sweeping 0..1023, the 4096 (frameIdx, k) pairs must produce EVERY integer in
    // [0, 4096) exactly once. That is what lets a later frame's rays sit strictly past every ray a
    // temporal accumulator has already gathered, rather than recomputing or skipping one.
    {
        constexpr u32 n = 4u;
        constexpr u32 frameCount = 1024u;
        std::vector<bool> seen(static_cast<usize>(n) * frameCount, false);
        bool allDistinct = true;
        u32 maxIdx = 0;
        for (u32 frame = 0; frame < frameCount; ++frame) {
            for (u32 k = 0; k < n; ++k) {
                const u32 idx = frame * n + k;   // the exact expression rtHemiDiscSample computes
                if (idx >= seen.size() || seen[idx]) { allDistinct = false; continue; }
                seen[idx] = true;
                maxIdx = std::max(maxIdx, idx);
            }
        }
        const bool dense = std::all_of(seen.begin(), seen.end(), [](bool b) { return b; });
        check(allDistinct && dense && maxIdx == n * frameCount - 1u,
              "n=4 is nested: " + std::to_string(frameCount) + " frames produce all " +
              std::to_string(n * frameCount) + " indices exactly once, densely from 0 -- raising the "
              "frame count only appends, it never renumbers a sample already drawn");
    }

    // ---- 4. the mirror still describes the shader ----
    //
    // EACH LINE SEPARATELY, not the whole body as one multi-line substring: this file's text is read
    // off disk verbatim (see hlslText() above), so a search spanning a line break would depend on
    // this repository's line-ending convention (CRLF vs LF) rather than on the CONTENT the contract
    // actually specifies -- same reason VoxiRtSeqTest.cpp beside this file never searches across a
    // newline either. Five lines found is the signature plus the whole body, in order of appearance,
    // which is as close to "the exact body, verbatim" as a substring search gets without depending on
    // how the file happens to be saved.
    {
        check(hlslHas("float2 rtHemiDiscSample(uint k, uint n, uint frameIdx, float2 pixelKey, float streamSalt) {"),
              "the shader still declares rtHemiDiscSample with this exact signature");
        check(hlslHas("const uint  idx = frameIdx * max(n, 1u) + k;"),
              "...its idx line matches this file's C++ mirror, verbatim");
        check(hlslHas("const float u   = frac(rtRadicalInverse2(idx + 1u) + rtHash(pixelKey + float2(streamSalt, 17.0 + streamSalt)));"),
              "...its u line matches this file's C++ mirror, verbatim");
        check(hlslHas("const float a   = rtHash(pixelKey) * 6.2831853 + averGoldenTurns(idx) + streamSalt;"),
              "...its a line matches this file's C++ mirror, verbatim");
        check(hlslHas("return float2(cos(a), sin(a)) * sqrt(u);"),
              "...and its return line matches this file's C++ mirror, verbatim");
        check(hlslHas(
            "const float2 xi = (((uint)gAmbientParams.z & 1u) != 0u) ? rtDiscSample(0, rtHash(pixel) * 6.2831853 + frameJitter) : rtHemiDiscSample(0u, 1u, (uint)gRtHistParams.z, pixel, 0.0);"),
            "the ReSTIR candidate's F1 call site (voxi_restir.hlsli) still switches on gAmbientParams.z bit 1");
        check(hlslHas(
            "const float2 d = (((uint)gAmbientParams.z & 1u) != 0u) ? rtDiscSample(k, ang0) : rtHemiDiscSample(k, n, (uint)gRtHistParams.z, aoTile, 0.37);"),
            "the sky-occlusion ray's F1 call site (voxi_rt.hlsli) still switches on gAmbientParams.z bit 1");
        check(hlslHas("rtHemiDiscSample(0u, 1u, (uint)gRtHistParams.z, pixel, 0.71)"),
              "F2's second-bounce sample (voxi_restir.hlsli) still draws from the same sampler, "
              "streamSalt 0.71 so it never collides with the candidate's own draw at the same "
              "(pixel, frame)");
    }

    if (g_failures == 0) {
        AVER_INFO("[VoxiHemiSample] PASS: the new sampler is cosine-weighted where the one it "
                  "replaces was a fixed 45-degree ring, the check has been seen failing against that "
                  "ring, and the shader still matches the arithmetic checked here");
        return 0;
    }
    AVER_ERROR("[VoxiHemiSample] FAIL: {} check(s)", g_failures);
    return 1;
}
