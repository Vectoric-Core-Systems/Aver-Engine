// Headless test for the bus graph and its effects (G5 S1): every response is checked against a value worked out from the maths (the filter
// formulas, the compressor's static curve, delay tap positions), and the routing rules (send order, solo chains, bypass).
// No sound card: effects run on buffers and the mixer renders offline.
#include "aver/audio/BusGraph.hpp"
#include "aver/audio/Effects.hpp"
#include "aver/audio/Mixer.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::audio;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}
static bool near(f32 a, f32 b, f32 eps = 1.0e-4f) { return std::fabs(a - b) <= eps; }
static bool nearRel(f32 a, f32 b, f32 rel) { return std::fabs(a - b) <= rel * std::fabs(b) + 1.0e-6f; }

constexpr u32 kRate = 48000;
constexpr f32 kTwoPi = 6.28318530718f;

static std::vector<f32> sineStereo(u32 frames, f32 hz, f32 amp, u32 rate = kRate) {
    std::vector<f32> v(static_cast<usize>(frames) * 2);
    for (u32 i = 0; i < frames; ++i) v[2 * i] = v[2 * i + 1] = amp * std::sin(kTwoPi * hz * static_cast<f32>(i) / static_cast<f32>(rate));
    return v;
}
static std::vector<f32> dcStereo(u32 frames, f32 l, f32 r) {
    std::vector<f32> v(static_cast<usize>(frames) * 2);
    for (u32 i = 0; i < frames; ++i) { v[2 * i] = l; v[2 * i + 1] = r; }
    return v;
}
static std::vector<f32> impulse(u32 frames) {
    std::vector<f32> v(static_cast<usize>(frames) * 2, 0.0f);
    v[0] = v[1] = 1.0f;
    return v;
}
// Runs an effect over `io` in blocks of `block` frames.
static void run(Effect& fx, std::vector<f32>& io, u32 block = 512, const std::vector<f32>* sc = nullptr) {
    const u32 frames = static_cast<u32>(io.size() / 2);
    for (u32 at = 0; at < frames; at += block) {
        const u32 n = std::min(block, frames - at);
        fx.process(io.data() + static_cast<usize>(at) * 2, n, sc ? sc->data() + static_cast<usize>(at) * 2 : nullptr);
    }
}
static f32 rmsRange(const std::vector<f32>& v, u32 ch, u32 from, u32 to) {
    f64 s = 0.0; u32 n = 0;
    for (u32 i = from; i < to; ++i) { const f64 x = v[2 * i + ch]; s += x * x; ++n; }
    return n ? static_cast<f32>(std::sqrt(s / n)) : 0.0f;
}
static f32 peakRange(const std::vector<f32>& v, u32 ch, u32 from, u32 to) {
    f32 m = 0.0f;
    for (u32 i = from; i < to; ++i) m = std::max(m, std::fabs(v[2 * i + ch]));
    return m;
}
// The frequency with the most energy in the left channel (Goertzel scan), in Hz.
static f32 dominantFreq(const std::vector<f32>& v, u32 from, u32 to, f32 lo, f32 hi, f32 stepHz) {
    f32 best = lo; f64 bestE = -1.0;
    for (f32 f = lo; f <= hi; f += stepHz) {
        const f64 w = 6.283185307179586 * f / kRate;
        const f64 c = 2.0 * std::cos(w);
        f64 s1 = 0.0, s2 = 0.0;
        for (u32 i = from; i < to; ++i) { const f64 s = v[2 * i] + c * s1 - s2; s2 = s1; s1 = s; }
        const f64 e = s1 * s1 + s2 * s2 - c * s1 * s2;
        if (e > bestE) { bestE = e; best = f; }
    }
    return best;
}
static std::unique_ptr<Effect> make(EffectKind k) { return Effect::create(k, kRate); }

// ---------------------------------------------------------------- effects

static void testParams() {
    AVER_INFO("=== parameters ===");
    bool all = true;
    std::set<std::string> classNames;
    for (u32 k = 0; k < static_cast<u32>(EffectKind::Count); ++k) {
        auto fx = Effect::create(static_cast<EffectKind>(k), kRate);
        all = all && fx && fx->kind() == static_cast<EffectKind>(k);
        classNames.insert(Effect::className(static_cast<EffectKind>(k)));
        all = all && Effect::kindFromClassName(Effect::className(static_cast<EffectKind>(k))) == static_cast<EffectKind>(k);
    }
    check(all, "every kind can be made and round-trips through its class name");
    check(classNames.size() == static_cast<usize>(EffectKind::Count), "all class names differ");
    check(Effect::kindFromClassName("AudioEffectNope") == EffectKind::Count, "an unknown class name is Count");

    auto rv = make(EffectKind::Reverb);
    check(near(rv->paramByName("room_size"), 0.8f) && near(rv->paramByName("wet"), 0.5f) && near(rv->paramByName("predelay_msec"), 150.0f), "Reverb defaults are the documented ones");
    rv->setParamByName("room_size", 5.0f);
    check(near(rv->paramByName("room_size"), 1.0f), "a value is clamped to the parameter's range");
    check(rv->findParam("nonsense") == -1, "an unknown parameter name is -1");
    auto dl = make(EffectKind::Delay);
    check(near(dl->paramByName("tap1_delay_ms"), 250.0f) && near(dl->paramByName("tap2_level_db"), -12.0f) && near(dl->paramByName("feedback_lowpass"), 16000.0f), "Delay defaults are the documented ones");
    auto cp = make(EffectKind::Compressor);
    check(near(cp->paramByName("ratio"), 4.0f) && near(cp->paramByName("attack_us"), 20.0f) && near(cp->paramByName("release_ms"), 250.0f), "Compressor defaults are the documented ones");
    check(make(EffectKind::EQ6)->paramCount() == 6 && make(EffectKind::EQ10)->paramCount() == 10 && make(EffectKind::EQ21)->paramCount() == 21, "EQ6 / EQ10 / EQ21 have 6 / 10 / 21 bands");
    check(make(EffectKind::EQ10)->findParam("band_db/1000_hz") >= 0, "EQ bands are named band_db/<hz>_hz");
    check(make(EffectKind::Chorus)->findParam("voice/2/rate_hz") >= 0, "Chorus voices are voice/N/...");
}

static void testFilters() {
    AVER_INFO("=== filters ===");
    const f32 r = static_cast<f32>(kRate);
    const f32 q = 0.70710678f;
    check(nearRel(filterMagnitude(FilterMode::LowPass, 1000, q, 1, 1, r, 1000), 0.70710678f, 0.002f), "low-pass |H(fc)| = Q (0.7071 for Butterworth)");
    check(nearRel(filterMagnitude(FilterMode::LowPass, 1000, 0.5f, 1, 1, r, 1000), 0.5f, 0.002f), "low-pass with the default resonance 0.5: |H(fc)| = 0.5");
    check(near(filterMagnitude(FilterMode::LowPass, 1000, q, 1, 1, r, 20), 1.0f, 0.002f), "low-pass passes 20 Hz");
    const f32 decade = filterMagnitude(FilterMode::LowPass, 1000, q, 1, 1, r, 10000);
    check(decade > 0.006f && decade < 0.02f, "low-pass is about 40 dB down a decade up (12 dB per octave; the bilinear transform bends it a little)");
    check(nearRel(filterMagnitude(FilterMode::HighPass, 1000, q, 1, 1, r, 1000), 0.70710678f, 0.002f), "high-pass |H(fc)| = Q");
    check(near(filterMagnitude(FilterMode::HighPass, 1000, q, 1, 1, r, 20000), 1.0f, 0.01f), "high-pass passes 20 kHz");
    check(filterMagnitude(FilterMode::HighPass, 1000, q, 1, 1, r, 100) < 0.012f, "high-pass is down a decade below");
    check(filterMagnitude(FilterMode::Notch, 1000, 2.0f, 1, 1, r, 1000) < 1.0e-3f, "notch is empty at its centre");
    check(near(filterMagnitude(FilterMode::Notch, 1000, 2.0f, 1, 1, r, 100), 1.0f, 0.03f), "notch passes a decade below");
    const f32 bpC = filterMagnitude(FilterMode::BandPass, 1000, 1.0f, 1, 1, r, 1000);
    check(bpC > filterMagnitude(FilterMode::BandPass, 1000, 1.0f, 1, 1, r, 100) * 3.0f && bpC > filterMagnitude(FilterMode::BandPass, 1000, 1.0f, 1, 1, r, 10000) * 3.0f, "band-pass peaks at its centre");
    // The shelves (A = gain, as the coefficients have it): DC and Nyquist gain of the shelf side is gain^2.
    check(nearRel(filterMagnitude(FilterMode::HighShelf, 4000, 1.0f, 0.5f, 1, r, 23000), 0.25f, 0.06f), "high shelf: the high end settles at gain^2 (0.5 -> 0.25)");
    check(near(filterMagnitude(FilterMode::HighShelf, 4000, 1.0f, 0.5f, 1, r, 50), 1.0f, 0.01f), "high shelf leaves the low end");
    check(nearRel(filterMagnitude(FilterMode::LowShelf, 500, 1.0f, 2.0f, 1, r, 20), 4.0f, 0.03f), "low shelf: the low end settles at gain^2");
    check(near(filterMagnitude(FilterMode::LowShelf, 500, 1.0f, 2.0f, 1, r, 20000), 1.0f, 0.03f), "low shelf leaves the high end");
    const f32 one = filterMagnitude(FilterMode::LowPass, 1000, q, 1, 1, r, 8000);
    const f32 four = filterMagnitude(FilterMode::LowPass, 1000, q, 1, 4, r, 8000);
    check(four < one * 0.1f, "four stages cut a decade-ish above much harder than one");

    // The processing filters agree with the formula.
    auto lp = make(EffectKind::LowPass);
    lp->setParamByName("cutoff_hz", 1000.0f);
    lp->setParamByName("resonance", q);
    auto hiTone = sineStereo(kRate, 10000.0f, 0.5f), loTone = sineStereo(kRate, 100.0f, 0.5f);
    run(*lp, hiTone);
    lp->reset();
    run(*lp, loTone);
    check(rmsRange(hiTone, 0, 4800, kRate) / 0.35355f < 0.015f, "AudioEffectLowPassFilter removes a 10 kHz tone");
    check(nearRel(rmsRange(loTone, 0, 4800, kRate) / 0.35355f, 1.0f, 0.02f), "and passes a 100 Hz tone");
    auto hp = make(EffectKind::HighPass);
    hp->setParamByName("cutoff_hz", 1000.0f);
    hp->setParamByName("resonance", q);
    auto lo2 = sineStereo(kRate, 100.0f, 0.5f);
    run(*hp, lo2);
    check(rmsRange(lo2, 0, 4800, kRate) / 0.35355f < 0.015f, "AudioEffectHighPassFilter removes a 100 Hz tone");
    auto nt = make(EffectKind::Notch);
    nt->setParamByName("cutoff_hz", 1000.0f);
    nt->setParamByName("resonance", 2.0f);
    auto at1k = sineStereo(kRate, 1000.0f, 0.5f);
    run(*nt, at1k);
    check(rmsRange(at1k, 0, 9600, kRate) < 0.01f, "AudioEffectNotchFilter silences its centre frequency");
}

static void testAmplifyDelayChorus() {
    AVER_INFO("=== amplify / delay / chorus ===");
    auto am = make(EffectKind::Amplify);
    am->setParamByName("volume_db", 6.0206f);
    auto dc = dcStereo(4800, 0.25f, -0.125f);
    run(*am, dc);
    check(near(dc[2 * 4000], 0.5f, 1.0e-3f) && near(dc[2 * 4000 + 1], -0.25f, 1.0e-3f), "Amplify +6.02 dB doubles the signal");

    auto dl = make(EffectKind::Delay);
    auto io = impulse(kRate);
    run(*dl, io);
    f32 gl1, gr1;
    {
        const f32 t = (0.2f + 1.0f) * 0.25f * 3.14159265f;
        gl1 = 1.41421356f * std::cos(t); gr1 = 1.41421356f * std::sin(t);
    }
    const f32 lvl1 = std::pow(10.0f, -6.0f / 20.0f), lvl2 = std::pow(10.0f, -12.0f / 20.0f);
    check(near(io[0], 1.0f) && near(io[1], 1.0f), "Delay: the dry impulse is there at 0");
    check(near(io[2 * 12000], lvl1 * gl1, 1.0e-4f) && near(io[2 * 12000 + 1], lvl1 * gr1, 1.0e-4f), "tap 1 lands at 250 ms with -6 dB and its pan");
    f32 gl2, gr2;
    {
        const f32 t = (-0.4f + 1.0f) * 0.25f * 3.14159265f;
        gl2 = 1.41421356f * std::cos(t); gr2 = 1.41421356f * std::sin(t);
    }
    check(near(io[2 * 24000], lvl2 * gl2, 1.0e-4f) && near(io[2 * 24000 + 1], lvl2 * gr2, 1.0e-4f), "tap 2 lands at 500 ms with -12 dB and its pan");
    check(near(io[2 * 6000], 0.0f) && near(io[2 * 18000], 0.0f), "and nothing between the taps");

    auto fb = make(EffectKind::Delay);
    fb->setParamByName("feedback_active", 1.0f);
    fb->setParamByName("feedback_delay_ms", 100.0f);
    fb->setParamByName("feedback_level_db", -6.0f);
    fb->setParamByName("feedback_lowpass", 16000.0f);
    auto io2 = impulse(kRate);
    run(*fb, io2);
    f32 e350 = 0.0f;
    for (u32 i = 16800 - 100; i < 16800 + 200; ++i) e350 += std::fabs(io2[2 * i]);
    check(e350 > 0.15f, "feedback repeats tap 1 100 ms later (an echo at 350 ms)");

    auto ch = make(EffectKind::Chorus);
    ch->setParamByName("wet", 0.0f);
    auto sig = sineStereo(4800, 440.0f, 0.5f);
    auto ref = sig;
    run(*ch, sig);
    check(sig == ref, "Chorus with wet 0 is the dry signal exactly");
    auto ch2 = make(EffectKind::Chorus);
    ch2->setParamByName("dry", 0.0f);
    ch2->setParamByName("wet", 1.0f);
    auto imp = impulse(4800);
    run(*ch2, imp);
    f32 early = 0.0f, around = 0.0f;
    for (u32 i = 0; i < 400; ++i) early += std::fabs(imp[2 * i]);
    for (u32 i = 400; i < 1200; ++i) around += std::fabs(imp[2 * i]) + std::fabs(imp[2 * i + 1]);
    check(early < 1.0e-6f && around > 0.01f, "Chorus voices answer ~15-20 ms after the input, not before");
}

static void testDynamics() {
    AVER_INFO("=== compressor / limiters ===");
    auto cp = make(EffectKind::Compressor);
    cp->setParamByName("threshold", -12.0f);
    cp->setParamByName("ratio", 4.0f);
    auto loud = dcStereo(kRate / 2, 0.5f, 0.5f);
    run(*cp, loud);
    const f32 inDb = 20.0f * std::log10(0.5f);
    const f32 wantDb = compressorOutputDb(inDb, -12.0f, 4.0f);
    check(near(compressorOutputDb(-6.0f, -12.0f, 4.0f), -10.5f, 1.0e-4f) && near(compressorOutputDb(-20.0f, -12.0f, 4.0f), -20.0f), "the static curve: -6 dB over a -12 dB threshold at 4:1 settles at -10.5 dB; below it nothing");
    check(near(20.0f * std::log10(loud[2 * 20000]), wantDb, 0.05f), "the compressor settles on the static curve (steady 0.5 -> -10.52 dB)");
    cp->reset();
    auto quiet = dcStereo(kRate / 4, 0.1f, 0.1f);
    run(*cp, quiet);
    check(near(quiet[2 * 5000], 0.1f, 1.0e-4f), "a signal under the threshold passes unchanged");
    auto flat = make(EffectKind::Compressor);
    flat->setParamByName("threshold", -30.0f);
    flat->setParamByName("ratio", 1.0f);
    auto l1 = dcStereo(4800, 0.5f, 0.5f);
    run(*flat, l1);
    check(near(l1[2 * 4000], 0.5f, 1.0e-4f), "ratio 1 leaves the level alone");
    auto mk = make(EffectKind::Compressor);
    mk->setParamByName("threshold", -12.0f);
    mk->setParamByName("gain", 6.0206f);
    mk->setParamByName("mix", 0.0f);
    auto dry = dcStereo(4800, 0.3f, 0.3f);
    run(*mk, dry);
    check(near(dry[2 * 4000], 0.3f, 1.0e-4f), "mix 0 is the dry signal");

    // Sidechain: a quiet signal ducked by a loud detector.
    auto sc = make(EffectKind::Compressor);
    sc->setParamByName("threshold", -20.0f);
    sc->setParamByName("ratio", 4.0f);
    auto quietSig = dcStereo(kRate / 2, 0.05f, 0.05f);
    const auto detector = dcStereo(kRate / 2, 0.8f, 0.8f);
    run(*sc, quietSig, 512, &detector);
    const f32 detDb = 20.0f * std::log10(0.8f);
    const f32 reduction = (detDb - (-20.0f)) * (1.0f - 0.25f);
    check(near(20.0f * std::log10(quietSig[2 * 20000] / 0.05f), -reduction, 0.1f), "a sidechain detector drives the gain reduction, not the signal's own level");

    // Attack and release.
    auto rel = make(EffectKind::Compressor);
    rel->setParamByName("threshold", -20.0f);
    rel->setParamByName("ratio", 8.0f);
    rel->setParamByName("release_ms", 250.0f);
    std::vector<f32> sig(static_cast<usize>(kRate * 3) * 2, 0.05f);
    for (u32 i = 0; i < kRate / 2; ++i) sig[2 * i] = sig[2 * i + 1] = 0.8f;
    run(*rel, sig, 256);
    const f32 gDuring = sig[2 * (kRate / 2 - 10)] / 0.8f;
    const f32 gShortAfter = sig[2 * (kRate / 2 + 480)] / 0.05f;     // 10 ms after the loud part ends
    const f32 gLongAfter = sig[2 * (kRate * 2 + 100)] / 0.05f;      // 1.5 s after
    check(gDuring < 0.5f, "the loud part is reduced");
    check(gShortAfter < 0.9f, "10 ms after the loud part ends the reduction is still on (release 250 ms)");
    check(nearRel(gLongAfter, 1.0f, 0.02f), "and it has let go 1.5 s later");

    // HardLimiter: nothing above the ceiling, quiet material passes.
    auto hl = make(EffectKind::HardLimiter);
    hl->setParamByName("ceiling_db", -3.0f);
    hl->setParamByName("pre_gain_db", 0.0f);
    const f32 ceiling = std::pow(10.0f, -3.0f / 20.0f);
    auto hot = sineStereo(kRate, 440.0f, 2.0f);
    for (u32 i = kRate / 2; i < kRate / 2 + 500; ++i) hot[2 * i] = hot[2 * i + 1] = 6.0f * (((i / 20) & 1) ? 1.0f : -1.0f);   // a burst of square-wave spikes
    run(*hl, hot, 333);
    check(peakRange(hot, 0, 0, kRate) <= ceiling + 1.0e-4f && peakRange(hot, 1, 0, kRate) <= ceiling + 1.0e-4f, "HardLimiter: no sample exceeds the ceiling, including a +15 dB burst");
    check(peakRange(hot, 0, 0, kRate) > ceiling * 0.9f, "and it does reach it");
    auto hl2 = make(EffectKind::HardLimiter);
    auto soft = sineStereo(9600, 440.0f, 0.3f);
    auto softRef = soft;
    run(*hl2, soft);
    const u32 lat = hl2->latencyFrames();
    bool same = true;
    for (u32 i = 0; i + lat < 9600; ++i) same = same && near(soft[2 * (i + lat)], softRef[2 * i], 1.0e-5f);
    check(lat > 0 && same, "HardLimiter: a signal under the ceiling comes out unchanged, delayed by its look-ahead");

    auto sl = make(EffectKind::Limiter);
    const f32 sceiling = std::pow(10.0f, -0.1f / 20.0f);
    auto big = sineStereo(9600, 300.0f, 4.0f);
    run(*sl, big);
    check(peakRange(big, 0, 0, 9600) < sceiling + 1.0e-5f && peakRange(big, 0, 0, 9600) > 0.95f * sceiling, "Limiter: a +12 dB sine is held just under the ceiling");
    auto mid = sineStereo(9600, 300.0f, 0.5f);
    auto midRef = mid;
    run(*sl, mid);
    check(mid == midRef, "and the part under the knee is untouched");
}

static void testEq() {
    AVER_INFO("=== equalisers ===");
    auto eq = make(EffectKind::EQ10);
    auto flat = sineStereo(9600, 1000.0f, 0.1f);
    auto flatRef = flat;
    run(*eq, flat);
    check(flat == flatRef, "EQ10 at 0 dB everywhere is bit-exact");
    eq->setParamByName("band_db/1000_hz", 6.0206f);
    auto tone = sineStereo(kRate / 2, 1000.0f, 0.1f);
    run(*eq, tone);
    check(nearRel(peakRange(tone, 0, 12000, 24000), 0.2f, 0.02f), "a +6 dB band doubles a tone at its centre (0.1 -> 0.2)");
    eq->reset();
    auto far = sineStereo(kRate / 2, 125.0f, 0.1f);
    run(*eq, far);
    check(nearRel(peakRange(far, 0, 12000, 24000), 0.1f, 0.06f), "and leaves a tone three octaves away within 0.5 dB");
    auto cut = make(EffectKind::EQ6);
    cut->setParamByName("band_db/320_hz", -12.0f);
    auto t2 = sineStereo(kRate / 2, 320.0f, 0.5f);
    run(*cut, t2);
    check(nearRel(peakRange(t2, 0, 12000, 24000), 0.5f * std::pow(10.0f, -12.0f / 20.0f), 0.03f), "EQ6: -12 dB at 320 Hz");
    auto e21 = make(EffectKind::EQ21);
    e21->setParamByName("band_db/2000_hz", 12.0f);
    auto t3 = sineStereo(kRate / 2, 2000.0f, 0.05f);
    run(*e21, t3);
    check(nearRel(peakRange(t3, 0, 12000, 24000), 0.05f * std::pow(10.0f, 12.0f / 20.0f), 0.03f), "EQ21: +12 dB at 2 kHz");
}

static void testShapers() {
    AVER_INFO("=== distortion / phaser / pitch shift / reverb / panner / stereo ===");
    auto ds = make(EffectKind::Distortion);
    ds->setParamByName("mode", 0.0f);
    ds->setParamByName("keep_hf_hz", 20500.0f);
    auto big = sineStereo(9600, 440.0f, 2.0f);
    run(*ds, big);
    check(peakRange(big, 0, 480, 9600) <= 1.0f + 1.0e-3f && peakRange(big, 0, 480, 9600) > 0.95f, "Distortion clip mode: a 2.0 sine is held at 1.0");
    auto lofi = make(EffectKind::Distortion);
    lofi->setParamByName("mode", 2.0f);
    lofi->setParamByName("drive", 1.0f);
    lofi->setParamByName("keep_hf_hz", 20500.0f);
    auto t = sineStereo(9600, 440.0f, 0.9f);
    run(*lofi, t);
    std::set<i32> levels;
    for (u32 i = 2000; i < 9600; ++i) levels.insert(static_cast<i32>(std::lround(t[2 * i] * 100.0f)));
    check(levels.size() <= 6, "Distortion lo-fi at drive 1 has only a handful of output levels");
    auto ph = make(EffectKind::Phaser);
    ph->setParamByName("depth", 0.0f);
    auto sig = sineStereo(4800, 440.0f, 0.5f);
    auto sigRef = sig;
    run(*ph, sig);
    check(sig == sigRef, "Phaser with depth 0 is the dry signal");
    auto ph2 = make(EffectKind::Phaser);
    const auto sig2Ref = sineStereo(kRate, 800.0f, 0.5f);
    auto sig2 = sig2Ref;
    run(*ph2, sig2);
    bool finite = true;
    f64 diff = 0.0;
    for (usize i = 0; i < sig2.size(); ++i) { finite = finite && std::isfinite(sig2[i]) && std::fabs(sig2[i]) < 4.0f; diff += std::fabs(sig2[i] - sig2Ref[i]); }
    check(finite && diff / static_cast<f64>(sig2.size()) > 0.05, "Phaser at depth 1 colours the signal and stays bounded");

    for (f32 ratio : {2.0f, 0.5f, 1.5f}) {
        auto ps = make(EffectKind::PitchShift);
        ps->setParamByName("pitch_scale", ratio);
        auto tone = sineStereo(kRate, 440.0f, 0.5f);
        run(*ps, tone);
        const f32 got = dominantFreq(tone, 12000, 44000, 150.0f, 1200.0f, 2.0f);
        check(nearRel(got, 440.0f * ratio, 0.06f), "PitchShift x" + std::to_string(ratio).substr(0, 4) + ": 440 Hz comes out near " + std::to_string(static_cast<int>(440 * ratio)) + " Hz (got " + std::to_string(static_cast<int>(got)) + ")");
    }
    auto id = make(EffectKind::PitchShift);
    auto tone1 = sineStereo(kRate, 440.0f, 0.5f);
    run(*id, tone1);
    check(nearRel(dominantFreq(tone1, 12000, 44000, 150.0f, 1200.0f, 2.0f), 440.0f, 0.01f), "PitchShift x1 keeps the pitch");

    auto rv = make(EffectKind::Reverb);
    rv->setParamByName("dry", 0.0f);
    rv->setParamByName("wet", 1.0f);
    auto imp = impulse(kRate * 3);
    run(*rv, imp);
    const f32 early = rmsRange(imp, 0, 7200, 24000), late = rmsRange(imp, 0, 96000, 144000);
    check(early > 1.0e-4f && late < early, "Reverb: an impulse leaves a tail that decays");
    auto small = make(EffectKind::Reverb);
    small->setParamByName("dry", 0.0f);
    small->setParamByName("wet", 1.0f);
    small->setParamByName("room_size", 0.05f);
    auto imp2 = impulse(kRate * 3);
    run(*small, imp2);
    check(rmsRange(imp2, 0, 96000, 144000) < late * 0.5f, "a small room's tail is gone sooner than a large one's");
    auto dryOnly = make(EffectKind::Reverb);
    dryOnly->setParamByName("wet", 0.0f);
    auto s3 = sineStereo(4800, 440.0f, 0.5f);
    auto s3ref = s3;
    run(*dryOnly, s3);
    check(s3 == s3ref, "Reverb with wet 0 is the dry signal");

    auto pn = make(EffectKind::Panner);
    pn->setParamByName("pan", -1.0f);
    auto d1 = dcStereo(480, 0.5f, 0.5f);
    run(*pn, d1);
    check(near(d1[0], 0.5f) && near(d1[1], 0.0f), "Panner -1: right silent, left untouched");
    pn->setParamByName("pan", 0.5f);
    auto d2 = dcStereo(480, 0.5f, 0.5f);
    run(*pn, d2);
    check(near(d2[0], 0.25f) && near(d2[1], 0.5f), "Panner 0.5: left halved, right untouched");

    auto se = make(EffectKind::StereoEnhance);
    auto wide = dcStereo(480, 0.8f, 0.2f);
    auto wideRef = wide;
    run(*se, wide);
    check(near(wide[0], wideRef[0], 1.0e-6f) && near(wide[1], wideRef[1], 1.0e-6f), "StereoEnhance at its defaults is transparent");
    se->setParamByName("pan_pullout", 0.0f);
    auto narrow = dcStereo(480, 0.8f, 0.2f);
    run(*se, narrow);
    check(near(narrow[0], 0.5f) && near(narrow[1], 0.5f), "pan_pullout 0 folds to mono");
}

static void testCaptureSpectrum() {
    AVER_INFO("=== capture / spectrum analyser ===");
    auto cap = make(EffectKind::Capture);
    auto in = sineStereo(1000, 440.0f, 0.5f);
    cap->process(in.data(), 1000, nullptr);
    check(cap->captureAvailable() == 1000, "Capture queues every frame it sees");
    std::vector<f32> got(1200);
    check(cap->captureRead(got.data(), 600) == 600 && std::equal(got.begin(), got.begin() + 1200, in.begin()), "and reads them back in order");
    check(cap->captureAvailable() == 400, "reading pops frames");
    auto big = sineStereo(6000, 440.0f, 0.5f);
    cap->process(big.data(), 6000, nullptr);
    check(cap->captureAvailable() == cap->captureCapacity() && cap->captureCapacity() == 4800, "Capture holds buffer_length (0.1 s) of frames");
    check(cap->captureDiscarded() == 1600, "frames that do not fit are dropped and counted (400 queued + 6000 offered - 4800 held)");
    cap->captureClear();
    check(cap->captureAvailable() == 0, "clear empties it");

    auto sp = make(EffectKind::SpectrumAnalyzer);
    const f32 f0 = static_cast<f32>(kRate) / 1024.0f * 21.0f;     // on a bin of the default 1024-point transform
    auto tone = sineStereo(4096, f0, 0.5f);
    sp->process(tone.data(), 4096, nullptr);
    f32 l = 0.0f, r = 0.0f;
    check(sp->spectrum(f0 - 20.0f, f0 + 20.0f, 1, l, r), "the analyser answers once it has a window of audio");
    check(nearRel(l, 0.5f, 0.05f) && nearRel(r, 0.5f, 0.05f), "a bin-centred 0.5 sine reads about 0.5");
    sp->spectrum(3000.0f, 4000.0f, 0, l, r);
    check(l < 0.01f, "and nothing is read where there is nothing");
    auto fresh = make(EffectKind::SpectrumAnalyzer);
    check(!fresh->spectrum(0, 1000, 0, l, r), "an analyser with no audio yet says so");
}

// ---------------------------------------------------------------- the graph

static void fillBus(BusGraph& g, i32 bus, f32 v, u32 frames) {
    f32* b = g.busBuffer(bus);
    for (u32 i = 0; i < frames * 2; ++i) b[i] = v;
}
// Runs the graph for a few blocks with the given constant on each listed bus; returns Master's left sample of the last block.
static f32 runGraph(BusGraph& g, std::vector<std::pair<i32, f32>> feeds, u32 blocks = 3, u32 frames = 256) {
    f32 out = 0.0f;
    for (u32 b = 0; b < blocks; ++b) {
        g.begin();
        g.clear(frames);
        for (auto& f : feeds) fillBus(g, f.first, f.second, frames);
        g.process(frames);
        out = g.busBuffer(0)[2 * (frames - 1)];
        g.end();
    }
    return out;
}

static void testGraph() {
    AVER_INFO("=== bus graph ===");
    BusGraph g;
    g.init(kRate);
    g.setActive(true);
    check(g.busCount() == 1 && g.busName(0) == "Master", "a new graph is just Master");
    check(g.addBus() == 1 && g.busName(1) == "New Bus", "add_bus appends 'New Bus'");
    check(g.addBus() == 2 && g.busName(2) == "New Bus 2", "the next one is 'New Bus 2'");
    g.setBusName(1, "Music");
    g.setBusName(2, "SFX");
    check(g.findBus("SFX") == 2 && g.findBus("nope") == -1, "buses are found by name");
    g.setBusName(2, "Music");
    check(g.busName(2) == "Music 2", "a clashing name gets a suffix");
    g.setBusName(2, "SFX");
    check(g.addBus(0) == 1 && g.busName(1) == "New Bus" && g.busCount() == 4, "add_bus at 0 inserts after Master, at 1");
    check(g.removeBus(1) && g.busCount() == 3, "remove_bus takes one out");
    check(!g.removeBus(0), "Master cannot be removed");
    check(g.busCount() == 3 && g.busName(2) == "SFX", "remove_bus closes the gap");
    g.setBusCount(5);
    check(g.busCount() == 5, "set bus count adds buses");
    g.setBusCount(3);
    check(g.busCount() == 3, "and removes them from the end");

    // Move: `to` is numbered before the move.
    g.addBus(); g.setBusName(3, "Ambience");
    check(g.moveBus(1, 3) && g.busName(1) == "SFX" && g.busName(2) == "Music" && g.busName(3) == "Ambience", "move_bus(1, 3) puts Music after SFX (insert-before, numbered before the move)");
    check(!g.moveBus(0, 2) && !g.moveBus(2, 0), "Master neither moves nor is moved over");
    g.moveBus(2, 1);
    check(g.busName(1) == "Music", "moving up works");

    // Sends.
    check(g.sendName(1) == "Master" && g.sendIndex(1) == 0, "the default send is Master");
    g.setSend(2, "Music");     // SFX is bus 2 now? (Music 1, SFX 2)
    check(g.busName(2) == "SFX" && g.sendIndex(2) == 1, "a bus can send to a bus above it");
    g.setSend(1, "SFX");
    check(g.sendIndex(1) == 0, "a send to a bus at a higher index falls back to Master");
    g.setSend(1, "nobody");
    check(g.sendIndex(1) == 0, "so does a send to a name that does not exist");
    g.setSend(1, "Master");
}

static void testMixingRules() {
    AVER_INFO("=== volume, mute, solo, bypass, peaks ===");
    BusGraph g;
    g.init(kRate);
    g.setActive(true);
    g.addBus(); g.setBusName(1, "A");
    g.addBus(); g.setBusName(2, "B");
    check(near(runGraph(g, {{1, 0.5f}}), 0.5f, 1.0e-5f), "a bus sends to Master at unity");
    g.setVolumeDb(1, -6.0206f);
    check(near(runGraph(g, {{1, 0.5f}}), 0.25f, 1.0e-3f), "bus volume -6.02 dB halves it");
    g.setVolumeDb(1, 0.0f);
    g.setVolumeDb(0, -6.0206f);
    check(near(runGraph(g, {{1, 0.5f}}), 0.25f, 1.0e-3f), "Master's volume applies to the sum");
    g.setVolumeDb(0, 0.0f);
    check(near(g.peakDb(1, 0), -6.0206f, 0.05f) && near(g.peakDb(2, 0), -200.0f, 0.01f), "peaks are read after the gain; a silent bus reads -200 dB");
    g.setMute(1, true);
    check(near(runGraph(g, {{1, 0.5f}, {2, 0.25f}}), 0.25f, 1.0e-5f), "a muted bus is silent and the others are not");
    g.setMute(1, false);

    // Solo: only soloed buses (and what they send through) are heard.
    g.setSolo(2, true);
    check(near(runGraph(g, {{1, 0.5f}, {2, 0.25f}}), 0.25f, 1.0e-5f), "solo: only the soloed bus is audible, Master still passes it");
    g.setMute(2, true);
    check(near(runGraph(g, {{1, 0.5f}, {2, 0.25f}}), 0.25f, 1.0e-5f), "mute is ignored while a solo is on");
    g.setMute(2, false);
    g.setSolo(2, false);

    // Solo keeps the send chain: A -> B -> Master with B in solo mode lets A through? No: A is not soloed. Solo A, B receives from A.
    g.setSend(1, "Master");
    g.setSend(2, "A");
    g.setSolo(2, true);
    check(near(runGraph(g, {{2, 0.25f}, {1, 0.5f}}), 0.75f, 1.0e-5f), "soloing B keeps the bus it sends through (A) and Master audible");
    g.setSolo(2, false);
    g.setSend(2, "Master");

    // Effects: Amplify +6 dB on A, then bypass and disable.
    const u32 id = g.addEffect(1, EffectKind::Amplify);
    check(id != 0 && g.effectCount(1) == 1 && g.effectId(1, 0) == id, "add_bus_effect returns an id");
    g.effect(id)->setParamByName("volume_db", 6.0206f);
    check(near(runGraph(g, {{1, 0.25f}}), 0.5f, 1.0e-3f), "a bus effect processes the bus");
    g.setBypassEffects(1, true);
    check(near(runGraph(g, {{1, 0.25f}}), 0.25f, 1.0e-5f), "bypass_fx skips the whole chain");
    g.setBypassEffects(1, false);
    g.setEffectEnabled(1, 0, false);
    check(near(runGraph(g, {{1, 0.25f}}), 0.25f, 1.0e-5f) && !g.effectEnabled(1, 0), "a disabled effect is skipped");
    g.setEffectEnabled(1, 0, true);
    // Order: Amplify then HardLimiter vs the other way round differ.
    const u32 id2 = g.addEffect(1, EffectKind::Panner, 0);
    g.effect(id2)->setParamByName("pan", -1.0f);
    check(g.effectId(1, 0) == id2 && g.effectId(1, 1) == id, "an effect can be inserted at an index");
    g.moveEffect(1, 0, 1);
    check(g.effectId(1, 1) == id2, "and moved");
    g.removeEffect(1, 1);
    check(g.effectCount(1) == 1 && g.effect(id2) == nullptr, "removed effects are gone");

    // A compressor listening to another bus (sidechain): B (index 2) is the detector for A's compressor.
    auto sg = std::make_unique<BusGraph>();
    sg->init(kRate);
    sg->setActive(true);
    sg->addBus(); sg->setBusName(1, "Music");
    sg->addBus(); sg->setBusName(2, "Voice");
    const u32 cid = sg->addEffect(1, EffectKind::Compressor);
    sg->effect(cid)->setParamByName("threshold", -20.0f);
    sg->effect(cid)->setParamByName("ratio", 4.0f);
    sg->setSidechain(1, 0, "Voice");
    check(sg->sidechain(1, 0) == "Voice" && sg->effect(cid)->sidechainBus.load() == 2, "a sidechain is resolved to a bus index");
    f32 plain = 0.0f, ducked = 0.0f;
    {
        BusGraph p;
        p.init(kRate); p.setActive(true);
        p.addBus();
        const u32 pc = p.addEffect(1, EffectKind::Compressor);
        p.effect(pc)->setParamByName("threshold", -20.0f);
        p.effect(pc)->setParamByName("ratio", 4.0f);
        plain = runGraph(p, {{1, 0.05f}}, 200, 256);
    }
    {
        BusGraph* q = sg.get();
        f32 out = 0.0f;
        for (u32 b = 0; b < 200; ++b) {
            q->begin(); q->clear(256);
            fillBus(*q, 1, 0.05f, 256);
            fillBus(*q, 2, 0.8f, 256);
            q->process(256);
            out = q->busBuffer(0)[2 * 255];
            q->end();
        }
        ducked = out - 0.8f;      // Master = Music (compressed) + Voice (0.8)
    }
    check(near(plain, 0.05f, 1.0e-3f), "without a sidechain a quiet bus under the threshold is not touched");
    check(ducked < 0.05f * 0.6f && ducked > 0.0f, "with the Voice bus as the sidechain, Music is pushed down");

    // Layouts round trip.
    BusLayout lay;
    lay.buses.resize(3);
    lay.buses[0].name = "Master"; lay.buses[0].volumeDb = -3.0f;
    lay.buses[1].name = "Music"; lay.buses[1].volumeDb = -6.0f; lay.buses[1].mute = true;
    lay.buses[1].effects.push_back({"AudioEffectReverb", true, "", {{"room_size", 0.3f}, {"wet", 0.2f}}});
    lay.buses[1].effects.push_back({"AudioEffectMystery", true, "", {}});
    lay.buses[2].name = "SFX"; lay.buses[2].send = "Music"; lay.buses[2].solo = true; lay.buses[2].bypass = true;
    lay.buses[2].effects.push_back({"AudioEffectCompressor", false, "Music", {{"threshold", -18.0f}}});
    BusGraph lg;
    lg.init(kRate);
    const u32 skipped = lg.applyLayout(lay);
    check(skipped == 1, "a layout effect of an unknown class is skipped and counted");
    check(lg.busCount() == 3 && lg.busName(1) == "Music" && near(lg.volumeDb(1), -6.0f) && lg.mute(1) && lg.solo(2) && lg.bypassEffects(2), "buses, volumes, mute, solo and bypass come from the layout");
    check(lg.sendIndex(2) == 1 && near(lg.volumeDb(0), -3.0f), "sends are resolved by name; Master keeps its volume");
    check(lg.effectCount(1) == 1 && near(lg.effectAt(1, 0)->paramByName("room_size"), 0.3f), "effects get their parameters by name");
    check(!lg.effectEnabled(2, 0) && lg.sidechain(2, 0) == "Music", "enabled flags and sidechains are kept");
    const BusLayout back = lg.exportLayout();
    check(back.buses.size() == 3 && back.buses[1].effects.size() == 1 && back.buses[1].effects[0].className == "AudioEffectReverb" && back.buses[2].send == "Music", "export gives the layout back");
}

// ---------------------------------------------------------------- the mixer on graph buses

static SoundData dcSound(u32 frames, f32 v, u32 rate = kRate, u32 channels = 1) {
    SoundData d;
    d.channels = channels;
    d.sampleRate = rate;
    d.samples.assign(static_cast<usize>(frames) * channels, v);
    return d;
}

static void testMixer() {
    AVER_INFO("=== mixer: graph voices (offline) ===");
    Mixer m;
    m.init(kRate, 2, 16, 16, 4);
    const SoundHandle snd = m.addSound(dcSound(kRate * 2, 0.5f));
    PlayDesc d;
    d.sound = snd;
    d.looping = true;
    d.graphBus = 0;
    std::vector<f32> out(512 * 2);
    VoiceHandle early = m.play(d);
    m.mix(out.data(), 512);
    check(near(out[2 * 100], 0.5f, 1.0e-4f), "before the graph is active a graph voice plays like any voice");
    m.stop(early);
    m.mix(out.data(), 512);
    m.mix(out.data(), 512);

    BusGraph& g = m.graph();
    g.setActive(true);
    g.addBus(); g.setBusName(1, "Music");
    g.addBus(); g.setBusName(2, "SFX");
    d.graphBus = 1;
    VoiceHandle v = m.play(d);
    for (int i = 0; i < 4; ++i) m.mix(out.data(), 512);
    check(near(out[2 * 300], 0.5f, 1.0e-4f) && near(out[2 * 300 + 1], 0.5f, 1.0e-4f), "a voice on a graph bus reaches Master");
    g.setVolumeDb(1, -6.0206f);
    for (int i = 0; i < 4; ++i) m.mix(out.data(), 512);
    check(near(out[2 * 300], 0.25f, 1.0e-3f), "the bus volume applies");
    g.setMute(1, true);
    for (int i = 0; i < 4; ++i) m.mix(out.data(), 512);
    check(near(out[2 * 300], 0.0f, 1.0e-4f), "a muted bus silences its voices");
    g.setMute(1, false);
    m.setVoiceGraphBus(v, 2);
    g.setVolumeDb(2, 0.0f);
    for (int i = 0; i < 4; ++i) m.mix(out.data(), 512);
    check(near(out[2 * 300], 0.5f, 1.0e-3f), "a voice can be moved to another bus while it plays");
    m.setVoiceGraphBus(v, 77);
    for (int i = 0; i < 4; ++i) m.mix(out.data(), 512);
    check(near(out[2 * 300], 0.5f, 1.0e-3f), "an out-of-range bus plays on Master");
    m.setVoiceGraphBus(v, 1);
    g.setVolumeDb(1, 0.0f);

    // A legacy voice still plays beside it, with the legacy bus gains.
    PlayDesc legacy;
    legacy.sound = m.addSound(dcSound(kRate, 0.1f));
    legacy.looping = true;
    legacy.bus = Bus::Ui;
    m.setBusVolume(Bus::Ui, 0.5f);
    VoiceHandle lv = m.play(legacy);
    for (int i = 0; i < 4; ++i) m.mix(out.data(), 512);
    check(near(out[2 * 300], 0.5f + 0.05f, 1.0e-3f), "a legacy voice mixes beside the graph with its own bus gain");
    m.stop(lv);
    for (int i = 0; i < 4; ++i) m.mix(out.data(), 512);

    // Chunking: a call longer than the graph block renders the same as shorter ones.
    std::vector<f32> longOut(9000 * 2);
    m.mix(longOut.data(), 9000);
    check(near(longOut[2 * 8999], 0.5f, 1.0e-3f) && near(longOut[2 * 4097], 0.5f, 1.0e-3f), "a 9000-frame call crosses the chunk boundary without a seam");

    // A graph effect on the bus.
    g.addEffect(1, EffectKind::Amplify);
    g.effectAt(1, 0)->setParamByName("volume_db", 6.0206f);
    for (int i = 0; i < 4; ++i) m.mix(out.data(), 512);
    check(near(out[2 * 300], 1.0f, 2.0e-3f), "effects run on voices' bus (0.5 doubled; Master clips at 1)");
    m.stop(v);

    // Pause and position.
    g.removeEffect(1, 0);
    SoundData ramp;
    ramp.channels = 1; ramp.sampleRate = kRate;
    for (u32 i = 0; i < kRate * 2; ++i) ramp.samples.push_back(static_cast<f32>(i) / static_cast<f32>(kRate * 2));
    PlayDesc p;
    p.sound = m.addSound(std::move(ramp));
    p.graphBus = 0;
    VoiceHandle pv = m.play(p);
    for (int i = 0; i < 20; ++i) m.mix(out.data(), 480);      // 0.2 s
    check(near(static_cast<f32>(m.voicePositionSeconds(pv)), 0.2f, 0.002f) && near(static_cast<f32>(m.voiceLengthSeconds(pv)), 2.0f, 1.0e-4f), "playback position and length are readable");
    m.setVoicePaused(pv, true);
    m.mix(out.data(), 480);
    const f64 atPause = m.voicePositionSeconds(pv);
    check(near(static_cast<f32>(atPause), 0.2f, 0.002f), "a pause holds the playhead (the fade-out block does not move it)");
    m.mix(out.data(), 480);
    m.mix(out.data(), 480);
    check(near(out[2 * 100], 0.0f, 1.0e-6f) && m.voicePositionSeconds(pv) == atPause && m.playing(pv), "paused: silent, position frozen, still alive");
    m.setVoicePaused(pv, false);
    for (int i = 0; i < 10; ++i) m.mix(out.data(), 480);
    check(near(static_cast<f32>(m.voicePositionSeconds(pv)), 0.3f, 0.002f) && std::fabs(out[2 * 300]) > 0.1f, "and resumes where it stopped");
    m.stop(pv);
    m.mix(out.data(), 480);

    // Start offset.
    SoundData ramp2;
    ramp2.channels = 1; ramp2.sampleRate = kRate;
    for (u32 i = 0; i < kRate; ++i) ramp2.samples.push_back(static_cast<f32>(i) / static_cast<f32>(kRate));
    PlayDesc s;
    s.sound = m.addSound(std::move(ramp2));
    s.graphBus = 0;
    s.startSeconds = 0.5f;
    VoiceHandle sv = m.play(s);
    m.mix(out.data(), 480);
    check(out[2 * 100] > 0.49f && out[2 * 100] < 0.52f, "play(from_position) starts inside the sound");
    m.stop(sv);
    for (int i = 0; i < 4; ++i) m.mix(out.data(), 480);

    // Speed scale.
    SoundData ramp3;
    ramp3.channels = 1; ramp3.sampleRate = kRate;
    ramp3.samples.assign(kRate * 4, 0.1f);
    PlayDesc sp;
    sp.sound = m.addSound(std::move(ramp3));
    sp.graphBus = 0;
    m.setPlaybackSpeedScale(2.0f);
    VoiceHandle spv = m.play(sp);
    for (int i = 0; i < 20; ++i) m.mix(out.data(), 480);
    check(near(static_cast<f32>(m.voicePositionSeconds(spv)), 0.4f, 0.004f), "AudioServer.playback_speed_scale 2 plays twice as fast");
    m.setPlaybackSpeedScale(1.0f);
    m.stop(spv);
    for (int i = 0; i < 4; ++i) m.mix(out.data(), 480);

    // Polyphony of live voices and the stolen counter are the mixer's own; max_polyphony is the player's (Aver.Fabric tests).
    check(m.activeVoices() <= 16, "the pool stays within its size");
}

static void testLive() {
    AVER_INFO("=== mixer: live (generator) voices ===");
    Mixer m;
    m.init(kRate, 2, 8, 8, 4);
    PlayDesc d;
    d.graphBus = -1;
    VoiceHandle v = m.playLive(d, kRate, 2, 4800);
    check(v != 0 && m.liveFree(v) == 4800, "a live voice has an empty ring of the size asked for");
    std::vector<f32> out(480 * 2);
    m.mix(out.data(), 480);
    check(near(out[10], 0.0f, 1.0e-6f), "nothing pushed: silence, and the voice stays alive");
    check(m.playing(v), "running dry does not end a generator voice");
    std::vector<f32> push(2400 * 2);
    for (u32 i = 0; i < 2400; ++i) { push[2 * i] = 0.3f; push[2 * i + 1] = -0.3f; }
    check(m.pushLive(v, push.data(), 2400) == 2400 && m.liveFree(v) == 2400, "frames pushed from the game thread are queued");
    check(m.pushLive(v, push.data(), 2400 + 100) == 2400, "a push beyond the free space takes what fits");
    m.mix(out.data(), 480);
    check(near(out[2 * 200], 0.3f, 1.0e-3f) && near(out[2 * 200 + 1], -0.3f, 1.0e-3f), "queued frames play in order");
    m.liveFlush(v);
    m.mix(out.data(), 480);
    check(m.liveFree(v) == 4800, "flush drops what was queued");
    m.pushLive(v, push.data(), 100);
    m.mix(out.data(), 480);
    check(m.voiceUnderruns(v) >= 1, "running dry mid-block is counted as a skip");
}

int main() {
    testParams();
    testFilters();
    testAmplifyDelayChorus();
    testDynamics();
    testEq();
    testShapers();
    testCaptureSpectrum();
    testGraph();
    testMixingRules();
    testMixer();
    testLive();
    if (g_failures) AVER_ERROR("AudioBusTest: {} check(s) FAILED", g_failures);
    else AVER_INFO("AudioBusTest: all checks passed");
    return g_failures ? 1 : 0;
}
