// GerstnerWave.hpp's wave math, against numbers this test computes for itself. CPU-only: no RHI, no
// device, no window -- runnable in the same headless-suite style as ParticleSystemTest and
// SoftBodyTest (see tests/water/CMakeLists.txt's own comment on why this target links Aver.Core
// only, never Aver.Water).
#include "aver/water/GerstnerWave.hpp"

#include "aver/core/Log.hpp"

#include <cmath>
#include <string>

using namespace aver;
using namespace aver::water;

static int g_checks = 0;
static int g_failures = 0;

// Matches SoftBodyTest.cpp's exact idiom: call the condition FIRST, assert second. MSVC evaluates a
// function call's arguments in an unspecified order, and building the message string ahead of the
// condition it describes is the trap documented project-wide (aver-unbacked-verification /
// aver-gates-probe-rule sibling: see the SoftBodyTest.cpp file this mirrors) -- every call site
// below computes its message from values already known before `check` is called, never from a
// reference that `check`'s own condition argument could mutate first.
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(float a, float b, float eps) { return std::abs(a - b) <= eps; }

// Case 1: a single wave with amplitude A at t=0, x=z=0 gives a known phase and the returned height
// matches an independently-computed A*cos(phase).
static void testSinglePhaseAtOrigin() {
    GerstnerWave w;
    w.dirX = 1.0f; w.dirZ = 0.0f;
    w.wavelengthCm = 800.0f;
    w.amplitudeCm = 25.0f;
    w.steepness = 0.5f;

    // At x=z=0 the spatial term of the phase (omega * dot(dir, pos)) is zero, so the phase at t=0 is
    // zero too, and sin(phase) is 0 -- gerstnerHeightCm should read exactly that, not A*cos(phase)
    // (the height term is a SINE, per the standard Gerstner formulation; this case exists to pin
    // that down independently of gerstnerDisplaceCm's own normal, which uses the same sine).
    const f32 h = gerstnerHeightCm(&w, 1, 0.0f, 0.0f, 0.0f);
    const f32 expected = w.amplitudeCm * std::sin(0.0f);
    check(near(h, expected, 1e-3f), "height at x=z=t=0 matches A*sin(0) computed independently");

    // A second, non-trivial instant: t=0.37s, still x=z=0 so the spatial term stays zero and the
    // whole phase is just the dispersion term computed the same way the header itself derives it
    // (phaseSpeed = sqrt(g*omega), phase = phaseSpeed*t), giving an independent cross-check that the
    // frequency wiring inside gerstnerHeightCm is the one this test expects, not some other constant.
    const f32 omega = 2.0f * 3.14159265358979f / w.wavelengthCm;
    const f32 phaseSpeed = std::sqrt(981.0f * omega);
    const f32 t = 0.37f;
    const f32 hT = gerstnerHeightCm(&w, 1, 0.0f, 0.0f, t);
    const f32 expectedT = w.amplitudeCm * std::sin(phaseSpeed * t);
    check(near(hT, expectedT, 1e-2f), "height at x=z=0, t=0.37s matches A*sin(sqrt(981*omega)*t)");
}

// Case 2: a wave list with amplitude 0 for every entry returns height 0 everywhere -- guards
// against a formula that adds a spurious DC offset regardless of amplitude.
static void testZeroAmplitudeIsFlat() {
    GerstnerWave waves[kMaxGerstnerWaves];
    for (size_t i = 0; i < kMaxGerstnerWaves; ++i) {
        waves[i].dirX = static_cast<f32>(i) + 1.0f;   // varied, non-trivial directions
        waves[i].dirZ = static_cast<f32>(i) * 0.5f;
        waves[i].wavelengthCm = 200.0f + 100.0f * static_cast<f32>(i);
        waves[i].amplitudeCm = 0.0f;                  // the case under test
        waves[i].steepness = 0.9f;
    }

    const f32 samples[][3] = {
        {0.0f, 0.0f, 0.0f}, {123.0f, -456.0f, 0.0f}, {-789.0f, 42.0f, 5.5f}, {1000.0f, 1000.0f, 2.0f},
    };
    for (const auto& s : samples) {
        const f32 h = gerstnerHeightCm(waves, kMaxGerstnerWaves, s[0], s[1], s[2]);
        check(h == 0.0f, "zero-amplitude wave set returns exactly 0 height at x=" +
                          std::to_string(s[0]) + " z=" + std::to_string(s[1]) + " t=" + std::to_string(s[2]));
    }

    // The same must hold for the displaced position: x and z should come back UNDISPLACED, and the
    // normal should read straight up -- {0, 0, 1} -- with no wave to lean it over.
    f32 pos[3], nrm[3];
    gerstnerDisplaceCm(waves, kMaxGerstnerWaves, 300.0f, -150.0f, 1.25f, pos, nrm);
    check(near(pos[0], 300.0f, 1e-3f) && near(pos[1], -150.0f, 1e-3f) && pos[2] == 0.0f,
          "zero-amplitude displacement leaves x/z unmoved and height at 0");
    check(near(nrm[0], 0.0f, 1e-4f) && near(nrm[1], 0.0f, 1e-4f) && near(nrm[2], 1.0f, 1e-4f),
          "zero-amplitude normal reads straight up (0,0,1)");
}

// Case 3: the steepness clamp. kMaxGerstnerWaves waves each requesting steepness 1.0 must never
// fold the surface over itself -- checked from OUTSIDE by the displacement magnitude staying well
// inside the wavelength, not by reaching into clampedSteepness (a private implementation detail) to
// read back whatever value it actually used.
static void testSteepnessClampPreventsFolding() {
    // THE ASSERTION THIS TEST USED TO MAKE COULD NOT FAIL. It sampled four 25cm waves at steepness
    // 1.0 and checked the horizontal displacement stayed under half a wavelength (400cm) -- but the
    // UNCLAMPED worst case for four 25cm waves is bounded by the triangle inequality at 100cm, so
    // the check passed whether or not clampedSteepness() existed. Deleting the clamp entirely left
    // it green.
    //
    // What replaces it tests the clamp's ACTUAL RULE -- each of `count` waves gets at most 1/count
    // of the steepness budget -- without reimplementing the wave formula in the test, which would
    // only have moved the risk rather than removed it:
    //
    //   N IDENTICAL CO-DIRECTIONAL WAVES AT STEEPNESS 1 MUST DISPLACE EXACTLY AS FAR AS ONE DOES.
    //
    // With the clamp, each of the N contributes Qi = 1/N, so the sum is N * (1/N) * A = A, whatever
    // N is. Without it, each contributes Qi = 1 and the sum is N * A -- four times larger here. The
    // two cases are therefore separated by a factor of N, which no tolerance can paper over.
    const f32 wavelengthCm = 800.0f;
    const f32 amplitudeCm = 25.0f;

    auto makeWaves = [&](GerstnerWave* out, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            out[i].dirX = 1.0f;            // co-directional ON PURPOSE: spreading the directions
            out[i].dirZ = 0.0f;            // would let cancellation hide an unclamped sum.
            out[i].wavelengthCm = wavelengthCm;
            out[i].amplitudeCm = amplitudeCm;
            out[i].steepness = 1.0f;       // every wave asks for the maximum authored value
        }
    };

    GerstnerWave one[1];
    GerstnerWave many[kMaxGerstnerWaves];
    makeWaves(one, 1);
    makeWaves(many, kMaxGerstnerWaves);

    const f32 samples[][3] = {
        {0.0f, 0.0f, 0.0f}, {50.0f, -50.0f, 0.1f}, {400.0f, 100.0f, 0.73f}, {-200.0f, 600.0f, 1.9f},
    };

    for (const auto& sample : samples) {
        f32 p1[3], n1[3], pN[3], nN[3];
        gerstnerDisplaceCm(one,  1,                  sample[0], sample[1], sample[2], p1, n1);
        gerstnerDisplaceCm(many, kMaxGerstnerWaves,  sample[0], sample[1], sample[2], pN, nN);

        const f32 off1 = std::sqrt((p1[0] - sample[0]) * (p1[0] - sample[0]) +
                                   (p1[1] - sample[1]) * (p1[1] - sample[1]));
        const f32 offN = std::sqrt((pN[0] - sample[0]) * (pN[0] - sample[0]) +
                                   (pN[1] - sample[1]) * (pN[1] - sample[1]));

        // Equal to within float noise. Unclamped this would be kMaxGerstnerWaves times larger.
        check(std::abs(offN - off1) < 0.01f,
              "steepness budget is shared, not multiplied: " + std::to_string(kMaxGerstnerWaves) +
              " co-directional waves displace " + std::to_string(offN) + "cm, one displaces " +
              std::to_string(off1) + "cm");
    }

    // And the bound the clamp exists to enforce: the horizontal displacement of a summed wave train
    // must never approach a wavelength, because that is the point at which a crest folds through
    // itself and the surface self-intersects.
    f32 pos[3], nrm[3];
    gerstnerDisplaceCm(many, kMaxGerstnerWaves, 0.0f, 0.0f, 0.37f, pos, nrm);
    const f32 offsetMagCm = std::sqrt(pos[0] * pos[0] + pos[1] * pos[1]);
    check(offsetMagCm <= amplitudeCm * 1.02f,
          "and it stays inside the single-wave amplitude (" + std::to_string(offsetMagCm) +
          "cm <= " + std::to_string(amplitudeCm) + "cm)");
}

// Case 4: snapWorldToGridCm is idempotent and monotonic.
static void testSnapToGridIsIdempotentAndMonotonic() {
    const f32 cellSizeCm = 200.0f;
    const f32 samples[] = {0.0f, 1.0f, 199.9f, 200.0f, 200.1f, -1.0f, -200.1f, 12345.678f, -12345.678f};

    for (f32 v : samples) {
        const f32 snapped = snapWorldToGridCm(v, cellSizeCm);
        const f32 snappedAgain = snapWorldToGridCm(snapped, cellSizeCm);
        check(snapped == snappedAgain,
              "snapping an already-snapped value (" + std::to_string(snapped) + ") is a no-op");
    }

    for (size_t i = 0; i + 1 < sizeof(samples) / sizeof(samples[0]); ++i) {
        for (size_t j = i + 1; j < sizeof(samples) / sizeof(samples[0]); ++j) {
            const f32 a = samples[i], b = samples[j];
            const f32 lo = a < b ? a : b, hi = a < b ? b : a;
            const f32 snapLo = snapWorldToGridCm(lo, cellSizeCm);
            const f32 snapHi = snapWorldToGridCm(hi, cellSizeCm);
            check(snapLo <= snapHi,
                  "a larger world coordinate (" + std::to_string(hi) +
                  ") never snaps below a smaller one's origin (" + std::to_string(lo) +
                  " -> " + std::to_string(snapLo) + " vs " + std::to_string(snapHi) + ")");
        }
    }
}

int main() {
    AVER_INFO("GerstnerWaveTest");
    testSinglePhaseAtOrigin();
    testZeroAmplitudeIsFlat();
    testSteepnessClampPreventsFolding();
    testSnapToGridIsIdempotentAndMonotonic();
    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return g_failures;
}
