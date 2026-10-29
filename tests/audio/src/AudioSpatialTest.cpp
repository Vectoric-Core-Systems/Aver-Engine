// Headless test for Godot's AudioStreamPlayer3D maths (G5 S1): the four attenuation models, unit_size / max_db / max_distance, the panning law,
// the emission cone and attenuation filter, Doppler, and the same functions applied by the mixer to a voice. Reference values are worked out
// from the formulas by hand (stated beside each check).
#include "aver/audio/Mixer.hpp"
#include "aver/audio/Spatial.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
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
static bool near(f32 a, f32 b, f32 eps = 1.0e-3f) { return std::fabs(a - b) <= eps; }

constexpr u32 kRate = 48000;
// Listener at the origin facing +X with +Y to its right (the engine's frame).
static const f32 kPos[3] = {0, 0, 0}, kFwd[3] = {1, 0, 0}, kRight[3] = {0, 1, 0};

static void testModels() {
    AVER_INFO("=== attenuation models ===");
    using M = AttenuationModel;
    check(near(attenuationDb(M::InverseDistance, 10.0f, 10.0f), 0.0f, 0.01f), "inverse distance: 0 dB at one unit");
    check(near(attenuationDb(M::InverseDistance, 20.0f, 10.0f), -6.0206f, 0.01f), "inverse distance: -6.02 dB at two units");
    check(near(attenuationDb(M::InverseDistance, 100.0f, 10.0f), -20.0f, 0.01f), "inverse distance: -20 dB at ten units");
    check(near(attenuationDb(M::InverseSquareDistance, 20.0f, 10.0f), -12.0412f, 0.01f), "inverse square: -12.04 dB at two units (twice the dB of inverse)");
    check(near(attenuationDb(M::InverseSquareDistance, 100.0f, 10.0f), -40.0f, 0.01f), "inverse square: -40 dB at ten units");
    check(near(attenuationDb(M::Logarithmic, 100.0f, 10.0f), -20.0f * std::log(10.0f), 0.01f), "logarithmic: -20 ln(d/u) = -46.05 dB at ten units");
    check(near(attenuationDb(M::Disabled, 1000.0f, 10.0f), 0.0f), "disabled: no distance attenuation");
    check(near(attenuationDb(M::InverseDistance, 20.0f, 20.0f), 0.0f, 0.01f), "unit_size moves the 0 dB point");
}

static Spatial3D base() {
    Spatial3D s;
    s.unitSize = 1.0f;
    s.maxDb = 3.0f;
    return s;
}

static void testSpatialize() {
    AVER_INFO("=== spatialize ===");
    Spatial3D s = base();
    const f32 front[3] = {500, 0, 0};         // 5 m ahead
    SpatialResult r = spatialize(s, 1.0f, kPos, kFwd, kRight, front);
    check(near(r.distanceGain, 0.2f, 1.0e-3f), "5 m at unit_size 1: gain 1/5");
    check(near(r.gainL, r.gainR, 1.0e-5f) && near(r.gainL, 0.7071f * 0.2f, 1.0e-3f), "dead ahead pans to the centre (sqrt 0.5 each side)");
    const f32 behind[3] = {-500, 0, 0};
    r = spatialize(s, 1.0f, kPos, kFwd, kRight, behind);
    check(near(r.gainL, r.gainR, 1.0e-5f), "dead behind pans to the centre too (Godot has no front/back cue)");
    const f32 rightSide[3] = {0, 500, 0};
    r = spatialize(s, 1.0f, kPos, kFwd, kRight, rightSide);
    // strength 1 * global 0.5 -> g = (1-0.5)^2 = 0.25, f = 0.6; L = sqrt((1-0.6)/2) = 0.4472, R = sqrt(1.6/2) = 0.8944.
    check(near(r.gainL / r.distanceGain, 0.4472f, 1.0e-3f) && near(r.gainR / r.distanceGain, 0.8944f, 1.0e-3f), "hard right with the default strengths: 0.447 / 0.894");
    const f32 leftSide[3] = {0, -500, 0};
    r = spatialize(s, 1.0f, kPos, kFwd, kRight, leftSide);
    check(near(r.gainL / r.distanceGain, 0.8944f, 1.0e-3f) && near(r.gainR / r.distanceGain, 0.4472f, 1.0e-3f), "and hard left is the mirror");
    Spatial3D s0 = base();
    s0.panningStrength = 0.0f;
    r = spatialize(s0, 1.0f, kPos, kFwd, kRight, rightSide);
    check(near(r.gainL, r.gainR, 1.0e-5f), "panning_strength 0 does not pan");
    Spatial3D s2 = base();
    s2.panningStrength = 2.0f;
    r = spatialize(s2, 1.0f, kPos, kFwd, kRight, rightSide);
    check(near(r.gainL, 0.0f, 1.0e-3f) && near(r.gainR, r.distanceGain, 1.0e-3f), "panning_strength 2 (global 0.5) pans fully: the left side is empty");

    // max_db clamps the close-up gain; volume adds before the clamp.
    const f32 close[3] = {1, 0, 0};           // 1 cm
    r = spatialize(s, 1.0f, kPos, kFwd, kRight, close);
    check(near(r.distanceGain, std::pow(10.0f, 3.0f / 20.0f), 1.0e-3f), "up close the gain is clamped to max_db (+3 dB)");
    r = spatialize(s, 0.1f, kPos, kFwd, kRight, front);
    check(near(r.distanceGain, 0.02f, 1.0e-3f), "volume_db is added to the attenuation (-20 dB volume at 5 m)");
    r = spatialize(s, 4.0f, kPos, kFwd, kRight, front);
    check(near(r.distanceGain, 0.2f * 4.0f, 1.0e-3f), "a louder volume stays under the clamp at 5 m (+12 dB adds up)");

    // max_distance: linear fade to zero, silent beyond.
    Spatial3D md = base();
    md.model = static_cast<i32>(AttenuationModel::Disabled);
    md.maxDistance = 10.0f;
    const f32 half[3] = {500, 0, 0}, past[3] = {1100, 0, 0};
    r = spatialize(md, 1.0f, kPos, kFwd, kRight, half);
    check(near(r.distanceGain, 0.5f, 1.0e-3f) && !r.silent, "max_distance: halfway is half gain");
    r = spatialize(md, 1.0f, kPos, kFwd, kRight, past);
    check(r.silent && r.distanceGain == 0.0f, "and beyond it nothing plays");

    // The attenuation filter.
    Spatial3D fl = base();
    r = spatialize(fl, 1.0f, kPos, kFwd, kRight, front);
    check(near(r.filterGain, std::pow(10.0f, (1.0f - 0.2f) * -24.0f / 20.0f), 1.0e-3f), "the filter's gain follows (1 - distance gain) * attenuation_filter_db (default -24)");
    r = spatialize(fl, 1.0f, kPos, kFwd, kRight, close);
    check(near(r.filterGain, 1.0f), "close up (gain >= 1) there is no filtering");
    fl.filterCutoffHz = 20500.0f;
    r = spatialize(fl, 1.0f, kPos, kFwd, kRight, front);
    check(near(r.filterGain, 1.0f), "a cutoff at 20500 Hz turns the filter off");

    // The cone: the emitter faces +X (away from a listener behind it).
    Spatial3D cone = base();
    cone.coneEnabled = true;
    cone.coneAngleDeg = 45.0f;
    cone.coneFilterDb = -12.0f;
    cone.emitDir[0] = 1.0f; cone.emitDir[1] = 0.0f; cone.emitDir[2] = 0.0f;
    const f32 emitterAt[3] = {500, 0, 0};          // listener at the origin is BEHIND an emitter that projects along +X
    r = spatialize(cone, 1.0f, kPos, kFwd, kRight, emitterAt);
    check(near(r.coneAngleDeg, 180.0f, 0.01f), "the angle is between the emitter's axis and the direction to the listener (180 degrees here)");
    check(near(r.filterGain, std::pow(10.0f, ((1.0f - 0.2f) * -24.0f - 12.0f) / 20.0f), 1.0e-4f), "outside the cone the cone's filter dB is added");
    cone.emitDir[0] = -1.0f;                       // now the sound is projected towards the listener
    r = spatialize(cone, 1.0f, kPos, kFwd, kRight, emitterAt);
    check(near(r.coneAngleDeg, 0.0f, 0.05f) && near(r.filterGain, std::pow(10.0f, (1.0f - 0.2f) * -24.0f / 20.0f), 1.0e-4f), "inside the cone only the distance filter applies");
    cone.emitDir[0] = 0.0f; cone.emitDir[1] = 1.0f;
    r = spatialize(cone, 1.0f, kPos, kFwd, kRight, emitterAt);
    check(near(r.coneAngleDeg, 90.0f, 0.05f), "an emitter facing sideways is 90 degrees off");
}

static void testDoppler() {
    AVER_INFO("=== doppler ===");
    const f32 src[3] = {10000, 0, 0};              // 100 m ahead
    const f32 zero[3] = {0, 0, 0};
    f32 toward[3] = {-3430, 0, 0}, away[3] = {3430, 0, 0}, side[3] = {0, 3430, 0}, fast[3] = {-34000, 0, 0}, supersonic[3] = {-1000000, 0, 0};
    check(near(dopplerPitch(kPos, zero, src, toward), 343.0f / (343.0f - 34.3f), 1.0e-3f), "approaching at 34.3 m/s raises the pitch by 343 / (343 - 34.3) = 1.111");
    check(near(dopplerPitch(kPos, zero, src, away), 343.0f / (343.0f + 34.3f), 1.0e-3f), "receding lowers it to 0.909");
    check(near(dopplerPitch(kPos, zero, src, side), 1.0f, 1.0e-4f), "moving across the line of sight changes nothing");
    check(near(dopplerPitch(kPos, zero, src, zero), 1.0f), "standing still changes nothing");
    check(near(dopplerPitch(kPos, away, src, zero), 343.0f / (343.0f - 34.3f), 1.0e-3f), "a listener running away from a static source hears the same shift as the opposite motion (relative velocity)");
    check(near(dopplerPitch(kPos, zero, src, fast), 8.0f), "the factor is clamped to 8 (343 / (343 - 340) = 114)");
    check(near(dopplerPitch(kPos, zero, src, supersonic), 0.125f), "and to 1/8 when the formula goes negative (faster than sound, as Godot's does)");
}

static void testMixerApplies() {
    AVER_INFO("=== the mixer applies it ===");
    Mixer m;
    m.init(kRate, 2, 8, 8, 2);
    SoundData d;
    d.channels = 1; d.sampleRate = kRate;
    d.samples.assign(kRate * 4, 0.5f);
    const SoundHandle snd = m.addSound(std::move(d));
    Listener l;
    m.setListener(l);

    PlayDesc p;
    p.sound = snd;
    p.looping = true;
    p.positional = true;
    p.godotSpatial = true;
    p.spatial = base();
    p.position[0] = 0.0f; p.position[1] = 500.0f; p.position[2] = 0.0f;       // 5 m to the right
    std::vector<f32> out(480 * 2);
    VoiceHandle v = m.play(p);
    m.mix(out.data(), 480);
    m.mix(out.data(), 480);
    check(near(out[2 * 200], 0.5f * 0.2f * 0.4472f, 1.0e-3f) && near(out[2 * 200 + 1], 0.5f * 0.2f * 0.8944f, 1.0e-3f), "a Godot-spatial voice is attenuated and panned by the formulas");
    m.setVoicePosition(v, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 4; ++i) m.mix(out.data(), 480);
    check(near(out[2 * 200], 0.5f * std::pow(10.0f, 3.0f / 20.0f) * 0.7071f, 2.0e-3f), "moving it onto the listener hits the max_db clamp and the centre");
    m.stop(v);
    for (int i = 0; i < 4; ++i) m.mix(out.data(), 480);

    // High-frequency content falls faster than low-frequency content when the voice is far (the attenuation filter).
    auto tone = [&](f32 hz) {
        SoundData t;
        t.channels = 1; t.sampleRate = kRate;
        for (u32 i = 0; i < kRate; ++i) t.samples.push_back(std::sin(6.2831853f * hz * static_cast<f32>(i) / static_cast<f32>(kRate)));
        return m.addSound(std::move(t));
    };
    const SoundHandle lo = tone(200.0f), hi = tone(10000.0f);
    auto measure = [&](SoundHandle h) {
        PlayDesc q = p;
        q.sound = h;
        q.position[1] = 500.0f;
        q.spatial.panningStrength = 0.0f;
        VoiceHandle vv = m.play(q);
        std::vector<f32> buf(4800 * 2);
        m.mix(buf.data(), 4800);
        f64 s = 0.0;
        for (u32 i = 1200; i < 4800; ++i) s += buf[2 * i] * buf[2 * i];
        m.stop(vv);
        std::vector<f32> tail(480 * 2);
        for (int i = 0; i < 4; ++i) m.mix(tail.data(), 480);
        return static_cast<f32>(std::sqrt(s / 3600.0));
    };
    const f32 rl = measure(lo), rh = measure(hi);
    check(rl > 0.1f * 0.7071f * 0.2f * 0.9f && rh < rl * 0.1f, "at 5 m a 10 kHz tone is far below a 200 Hz one (the high-shelf attenuation filter)");

    // Doppler: a source approaching at 34.3 m/s plays 1.111x faster.
    SoundData longs;
    longs.channels = 1; longs.sampleRate = kRate;
    longs.samples.assign(kRate * 4, 0.1f);
    PlayDesc dp;
    dp.sound = m.addSound(std::move(longs));
    dp.positional = true;
    dp.godotSpatial = true;
    dp.spatial = base();
    dp.spatial.doppler = true;
    dp.spatial.velocity[0] = -3430.0f;
    dp.position[0] = 10000.0f;
    VoiceHandle dv = m.play(dp);
    for (int i = 0; i < 100; ++i) m.mix(out.data(), 480);       // 1 s
    check(near(static_cast<f32>(m.voicePositionSeconds(dv)), 1.1111f, 0.01f), "the voice's playhead runs 1.111x ahead of the clock while the source approaches");
    Spatial3D still = dp.spatial;
    still.velocity[0] = 0.0f;
    m.setVoiceSpatial(dv, still);
    const f64 at = m.voicePositionSeconds(dv);
    for (int i = 0; i < 100; ++i) m.mix(out.data(), 480);
    check(near(static_cast<f32>(m.voicePositionSeconds(dv) - at), 1.0f, 0.01f), "and at the normal rate once it stops");
}

int main() {
    testModels();
    testSpatialize();
    testDoppler();
    testMixerApplies();
    if (g_failures) AVER_ERROR("AudioSpatialTest: {} check(s) FAILED", g_failures);
    else AVER_INFO("AudioSpatialTest: all checks passed");
    return g_failures ? 1 : 0;
}
