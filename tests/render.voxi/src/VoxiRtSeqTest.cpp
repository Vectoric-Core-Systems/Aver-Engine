// The sun-disc sample sequence, checked for the one property the ray-count knob rests on: raising
// the count must ADD samples without moving the ones already there.
//
// WHY THIS IS A TEST AND NOT A COMMENT. rtShadow used to place sample k at radius
// sqrt((k + 0.5) / n), which depends on the TOTAL ray count -- so 2 rays and 4 rays were not a
// coarse and a fine version of one estimate, they were two unrelated estimates. Every ray count was
// its own oracle: an image recorded at 4 rays said nothing about the same scene at 8, a ray-count
// sweep could not be read as convergence, and any gate baseline was pinned to one count. Nothing on
// screen shows it. A shadow sampled by an entirely different set of rays looks like a shadow.
//
// TWO GENERATORS ARE KEPT HERE ON PURPOSE, and the test asserts that the old one FAILS. An
// instrument that has never been seen to fire is not known to work, and the cheapest way to know is
// to make it fire on every run against a defect that is still in the file. If someone weakens the
// prefix check into something that passes trivially, the negative control stops failing and the run
// goes red anyway.
//
// WHAT THIS DOES NOT COVER, stated because the gap is real: the arithmetic here is a C++ MIRROR of
// the HLSL, not the compiled shader. Nothing in this process runs DXC or a GPU. The mirror is tied
// to the shader by the source assertions at the bottom, which read the shader text the renderer
// actually compiles and fail if the expression drifts from the one modelled here.
#include <fstream>
#include <sstream>

#include "aver/core/Log.hpp"

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

struct Sample { f32 x, y; };

// ---------------------------------------------------------------- the two generators
//
// BOTH TAKE n, including the one that ignores it. That is what makes the prefix check below a real
// test rather than a restatement of a signature: the nested generator is handed the ray count and
// must be seen to disregard it.

// The base-2 radical inverse, mirroring rtRadicalInverse2 in VoxiShaders.hpp.
f32 radicalInverse2(u32 i) {
    u32 r = i;
    r = (r << 16) | (r >> 16);
    r = ((r & 0x00FF00FFu) << 8) | ((r & 0xFF00FF00u) >> 8);
    r = ((r & 0x0F0F0F0Fu) << 4) | ((r & 0xF0F0F0F0u) >> 4);
    r = ((r & 0x33333333u) << 2) | ((r & 0xCCCCCCCCu) >> 2);
    r = ((r & 0x55555555u) << 1) | ((r & 0xAAAAAAAAu) >> 1);
    return static_cast<f32>(r) * 2.3283064365386963e-10f;   // 1 / 2^32
}

// The sequence in the shader today: index-only, and therefore nested.
Sample nested(u32 k, u32 n, f32 ang0) {
    (void)n;
    const f32 rad = std::sqrt(radicalInverse2(k + 1));
    const f32 a   = ang0 + static_cast<f32>(k) * 2.39996323f;
    return Sample{std::cos(a) * rad, std::sin(a) * rad};
}

// The sequence this replaced. Stratified for its own n and unrelated to any other n. Kept as the
// negative control: the checks below must catch it.
Sample byCount(u32 k, u32 n, f32 ang0) {
    const f32 rad = std::sqrt((static_cast<f32>(k) + 0.5f) / static_cast<f32>(n < 1 ? 1 : n));
    const f32 a   = ang0 + static_cast<f32>(k) * 2.39996323f;
    return Sample{std::cos(a) * rad, std::sin(a) * rad};
}

using Generator = Sample (*)(u32, u32, f32);

// The ray counts a caller can actually ask for. --rt-rays takes any value in [1, 32], so the
// property is checked across the whole range and not only at powers of two.
const u32 kCounts[] = {1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 24, 32};

// The largest distance any sample moves when the ray count is raised from `m` to `n`, over the m
// samples the two counts share. Zero means the m-sample set is exactly a prefix of the n-sample set.
//
// BIT-EXACT is the standard, not "close": these feed ray directions, and the gate oracle compares
// nine configurations bit-exactly. A sample that moved by a float ulp did move.
f32 worstPrefixShift(Generator gen, u32 m, u32 n, f32 ang0) {
    f32 worst = 0.0f;
    for (u32 k = 0; k < m; ++k) {
        const Sample a = gen(k, m, ang0);
        const Sample b = gen(k, n, ang0);
        if (a.x == b.x && a.y == b.y) continue;             // identical bits: no shift at all
        const f32 dx = a.x - b.x, dy = a.y - b.y;
        const f32 d = std::sqrt(dx * dx + dy * dy);
        if (d > worst) worst = d;
    }
    return worst;
}

// The mean squared radius over the first n samples. 0.5 is the ideal for an area-uniform disc
// sampling -- it is what the sun's disc integrates to -- so the distance from 0.5 says how biased
// toward the centre or the rim a given prefix is.
f32 meanR2(Generator gen, u32 n, f32 ang0) {
    f32 sum = 0.0f;
    for (u32 k = 0; k < n; ++k) {
        const Sample s = gen(k, n, ang0);
        sum += s.x * s.x + s.y * s.y;
    }
    return sum / static_cast<f32>(n);
}

// The closest any two of the first n samples come to each other. A sequence that lands two rays on
// the same spot has spent one of them for nothing.
f32 minSeparation(Generator gen, u32 n, f32 ang0) {
    f32 worst = 3.0f;
    for (u32 i = 0; i < n; ++i)
        for (u32 j = i + 1; j < n; ++j) {
            const Sample a = gen(i, n, ang0), b = gen(j, n, ang0);
            const f32 dx = a.x - b.x, dy = a.y - b.y;
            const f32 d = std::sqrt(dx * dx + dy * dy);
            if (d < worst) worst = d;
        }
    return n < 2 ? 1.0f : worst;
}

// ---------------------------------------------------------------- the shader source assertions
//
// What ties the mirror above to the shader below it. Everything else here would keep passing if the
// HLSL changed underneath it.
// THE SHADER TEXT, READ FROM THE FILE THE RENDERER ACTUALLY COMPILES.
//
// This used to strstr voxi::kVoxiHLSL, a C++ raw string literal in VoxiShaders.hpp. The HLSL now
// lives in modules/render.voxi/shaders/voxi.hlsl and that literal is gone, so reading it here would
// be reading nothing. A test that ties a C++ mirror to shader text has to follow the text.
//
// Read through AVER_REPO_ROOT rather than linking anything: the point of this suite is that it needs
// no GPU and no RHI (see its CMakeLists -- "It links Aver.Core and nothing else"), and reaching for
// rhi::shaderFile() to get a string would trade that away for nothing. Same approach
// tests/repo/src/SeparationTest.cpp already uses to read source.
//
// AN UNREADABLE FILE MUST FAIL, NOT SKIP. Every assertion below is a substring search, so an empty
// string would make all of them "pass" by finding nothing to object to -- the exact shape of a test
// that proves nothing while reporting success.
const std::string& hlslText() {
    static const std::string s = [] {
        const std::string path = std::string(AVER_REPO_ROOT) + "/modules/render.voxi/shaders/voxi.hlsl";
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            AVER_ERROR("[VoxiRtSeq] cannot read {} -- every source assertion below would pass "
                       "vacuously against an empty string, so this is a failure, not a skip.", path);
            return std::string();
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }();
    return s;
}

bool hlslHas(const char* needle) {
    return hlslText().find(needle) != std::string::npos;
}

// The text of a shader function, from its signature to the first line that closes it. Used to argue
// about what a function CANNOT see, which is a claim the whole-file search cannot make.
std::string hlslBody(const char* signature) {
    const char* start = std::strstr(hlslText().c_str(), signature);
    if (!start) return {};
    const char* end = std::strstr(start, "\n}");
    return end ? std::string(start, static_cast<usize>(end - start)) : std::string(start);
}

} // namespace

int main() {
    AVER_INFO("[VoxiRtSeq] the sun-disc sample sequence");

    // A rotation that is not zero and not a multiple of the golden angle, so a bug that only shows
    // for an unrotated pattern cannot hide. The shader's ang0 is a per-pixel hash times 2*pi.
    const f32 ang0 = 1.2345f;

    // ---- 1. the radical inverse is the function it claims to be ----
    //
    // Checked against the bits of i read directly, which is a different route to the same number:
    // the mirror reverses and scales by 2^-32, this weighs bit j by 2^-(j+1). A wrong constant --
    // 2^-31, the classic -- makes every radius sqrt(2) too large and both routes disagree at once.
    // Up to 1023 the reversed value has at most ten significant bits, so float32 holds it exactly
    // and the comparison can be for equality rather than for closeness.
    {
        f32 worst = 0.0f;
        for (u32 i = 1; i < 1024; ++i) {
            f32 want = 0.0f, scale = 0.5f;
            for (u32 b = 0; b < 32; ++b, scale *= 0.5f)
                if ((i >> b) & 1u) want += scale;
            worst = std::fmax(worst, std::fabs(radicalInverse2(i) - want));
        }
        check(worst == 0.0f, "radical inverse matches a direct bit weighting exactly (worst " +
                             std::to_string(worst) + ")");
        check(radicalInverse2(1) == 0.5f, "phi(1) = 1/2, so a single ray lands on the disc's area median");
    }

    // ---- 2. NESTED: raising the ray count moves nothing ----
    {
        const u32 counts = sizeof(kCounts) / sizeof(kCounts[0]);
        f32 worst = 0.0f;
        u32 pairs = 0;
        for (u32 mi = 0; mi < counts; ++mi)
            for (u32 ni = mi + 1; ni < counts; ++ni, ++pairs)
                worst = std::fmax(worst, worstPrefixShift(nested, kCounts[mi], kCounts[ni], ang0));
        check(worst == 0.0f,
              "every sample is bit-identical at every ray count: " + std::to_string(pairs) +
              " count pairs from 1 to 32 rays, worst shift " + std::to_string(worst));
    }

    // ---- 3. the negative control: the sequence this replaced FAILS that ----
    //
    // Reported with the number, because the number is the point: a unit disc is 2 across, and the
    // old sample 0 slides most of a radius when the ray count goes from 1 to 32.
    {
        f32 worst = 0.0f;
        for (u32 mi = 0; mi < sizeof(kCounts) / sizeof(kCounts[0]); ++mi)
            for (u32 ni = mi + 1; ni < sizeof(kCounts) / sizeof(kCounts[0]); ++ni)
                worst = std::fmax(worst, worstPrefixShift(byCount, kCounts[mi], kCounts[ni], ang0));
        const f32 oneToEight = worstPrefixShift(byCount, 1, 8, ang0);
        check(worst > 0.5f,
              "NEGATIVE CONTROL: sqrt((k+0.5)/n) moves its samples when the count changes -- worst "
              + std::to_string(worst) + " over the range, " + std::to_string(oneToEight) +
              " going from 1 ray to 8. This check can see the defect it was written for");
    }

    // ---- 4. what nesting COST, measured rather than waved away ----
    //
    // The old sequence is exactly area-balanced for its own n -- mean r^2 = 1/2 for every count,
    // which is what stratification buys and precisely why it could not be nested. The nested one
    // gives that up. The trade is only acceptable if the residual bias is small at every prefix, so
    // it is bounded here instead of being described.
    {
        f32 worstErr = 0.0f;
        u32 worstAt = 0;
        for (u32 n : kCounts) {
            const f32 e = std::fabs(meanR2(nested, n, ang0) - 0.5f);
            if (e > worstErr) { worstErr = e; worstAt = n; }
        }
        check(worstErr <= 0.15f,
              "area balance holds at every prefix: mean r^2 is within " + std::to_string(worstErr) +
              " of 1/2 (worst at " + std::to_string(worstAt) + " rays)");
        check(std::fabs(meanR2(byCount, 8, ang0) - 0.5f) < 1e-6f,
              "and the stratified sequence is exact at its own count, which is what was traded away");
    }

    // ---- 5. no two rays are spent on the same place ----
    {
        f32 worst = 3.0f;
        u32 worstAt = 0;
        for (u32 n : kCounts) {
            const f32 d = minSeparation(nested, n, ang0);
            if (d < worst) { worst = d; worstAt = n; }
        }
        check(worst > 0.05f, "samples stay apart: closest pair " + std::to_string(worst) +
                             " at " + std::to_string(worstAt) + " rays");
    }

    // ---- 6. every sample is inside the unit disc ----
    {
        f32 worst = 0.0f;
        for (u32 k = 0; k < 64; ++k) {
            const Sample s = nested(k, 64, ang0);
            worst = std::fmax(worst, std::sqrt(s.x * s.x + s.y * s.y));
        }
        check(worst <= 1.0f, "no sample leaves the unit disc (largest radius " +
                             std::to_string(worst) + ")");
    }

    // ---- 7. the mirror still describes the shader ----
    //
    // The only tie between the arithmetic above and the HLSL the renderer hands to DXC. Without
    // these, this file would keep passing about a sequence the engine no longer runs.
    {
        check(hlslHas("float2 rtDiscSample(uint k, float ang0)"),
              "the shader still generates its samples in one named function");
        check(hlslHas("sqrt(rtRadicalInverse2(k + 1))"),
              "the shader's radius is the radical inverse this file mirrors");
        check(hlslHas("reversebits(i) * 2.3283064365386963e-10"),
              "the shader's radical inverse scales by 2^-32, as mirrored above");
        check(hlslHas("rtDiscSample(k, ang0)"),
              "the shadow loop draws from that function rather than its own copy");
        // THE STRUCTURAL ARGUMENT, and the tightest thing this file asserts. The generator's
        // parameters are an index and an angle; the ray count is a local inside rtShadow read from
        // gRtParams.y. A function that names neither cannot depend on the count however it is
        // written -- so nesting holds for reasons a reader can check rather than by inspection of
        // one expression.
        const std::string body = hlslBody("float2 rtDiscSample(uint k, float ang0)");
        check(!body.empty(), "the generator's body can be located in the shader source");
        check(body.find("gRtParams") == std::string::npos,
              "the generator does not read the constant buffer the ray count lives in");
        // Searched in the BODY and not the whole file, because the prose above the function quotes
        // the old expression on purpose and a file-wide search would fire on the explanation.
        check(body.find("k + 0.5") == std::string::npos,
              "and the count-dependent radius is not in the generator");
    }

    if (g_failures == 0) {
        AVER_INFO("[VoxiRtSeq] PASS: the sample sequence is nested, the check that says so has been "
                  "seen failing against the sequence it replaced, and the shader still matches the "
                  "arithmetic checked here");
        return 0;
    }
    AVER_ERROR("[VoxiRtSeq] FAIL: {} check(s)", g_failures);
    return 1;
}
