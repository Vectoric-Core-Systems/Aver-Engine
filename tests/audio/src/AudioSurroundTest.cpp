// Surround output and binaural rendering, with no sound card (docs/AUDIO.md section 14): the pairwise pan across 5.1 and 7.1, its energy,
// where sources at known angles land, the stereo upmix, the LFE send, the device layout choice and channel map, the output policy changing
// the layout, the analytic head model (interaural delay, level, front/back and elevation cues, smooth interpolation), the real-time rule
// (no allocation in mix()), and that the defaults render exactly what the stereo mixer always did.
#include "aver/audio/Binaural.hpp"
#include "aver/audio/Mixer.hpp"
#include "aver/audio/OutputDriver.hpp"
#include "aver/audio/Speakers.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::audio;

// ---- the allocation hook: every operator new made while a window is open is counted ------------------------------------------------------
static std::atomic<bool> g_counting{false};
static std::atomic<u64>  g_allocs{0};
void* operator new(std::size_t n) {
    if (g_counting.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    if (g_counting.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}
static bool near(f64 a, f64 b, f64 eps) { return std::fabs(a - b) <= eps; }
static std::string num(f64 v) { return std::to_string(v); }

constexpr u32 kRate = 48000;
constexpr f64 kPi = 3.14159265358979323846;

static SoundData constant(f32 value, u32 frames = kRate) {
    SoundData d; d.channels = 1; d.sampleRate = kRate; d.samples.assign(frames, value); return d;
}
static SoundData sine(f32 hz, f32 amp, u32 frames = kRate) {
    SoundData d; d.channels = 1; d.sampleRate = kRate; d.samples.resize(frames);
    for (u32 i = 0; i < frames; ++i) d.samples[i] = amp * std::sin(6.28318530718f * hz * static_cast<f32>(i) / static_cast<f32>(kRate));
    return d;
}
static SoundData impulse(u32 frames) {
    SoundData d; d.channels = 1; d.sampleRate = kRate; d.samples.assign(frames, 0.0f); d.samples[0] = 1.0f; return d;
}

// Renders `frames` frames in blocks of 480.
static std::vector<f32> render(Mixer& m, u32 frames) {
    const u32 ch = m.channels();
    std::vector<f32> out;
    out.reserve(static_cast<usize>(frames) * ch);
    std::vector<f32> blk(static_cast<usize>(480) * ch);
    for (u32 done = 0; done < frames;) {
        const u32 n = std::min<u32>(480, frames - done);
        m.mix(blk.data(), n);
        out.insert(out.end(), blk.begin(), blk.begin() + static_cast<usize>(n) * ch);
        done += n;
    }
    return out;
}
// Mean of channel c over the last `count` frames.
static f64 chanMean(const std::vector<f32>& v, u32 ch, u32 c, u32 count) {
    const usize frames = v.size() / ch;
    f64 s = 0.0;
    for (usize f = frames - count; f < frames; ++f) s += v[f * ch + c];
    return s / count;
}
static f64 chanRms(const std::vector<f32>& v, u32 ch, u32 c, usize from, usize to) {
    f64 s = 0.0;
    for (usize f = from; f < to; ++f) s += static_cast<f64>(v[f * ch + c]) * v[f * ch + c];
    return std::sqrt(s / static_cast<f64>(to - from));
}
static void listener(Mixer& m) {
    Listener l;   // at the origin, forward +X, right +Y
    m.setListener(l);
}
static VoiceHandle playAt(Mixer& m, SoundHandle s, f32 x, f32 y, f32 z, bool looping = true) {
    PlayDesc d; d.sound = s; d.looping = looping; d.positional = true; d.position[0] = x; d.position[1] = y; d.position[2] = z;
    return m.play(d);
}
// A source `deg` degrees from straight ahead (positive to the right) on the horizon, 100 cm away (inside the default inner radius: full level).
static void atAzimuth(f64 deg, f32& x, f32& y) { x = static_cast<f32>(100.0 * std::cos(deg * kPi / 180.0)); y = static_cast<f32>(100.0 * std::sin(deg * kPi / 180.0)); }

// ---------------------------------------------------------------------------------------------------------------------------------------

static void testPanLaw() {
    AVER_INFO("=== the pairwise pan: energy, placement, spread ===");
    for (u32 n : {6u, 8u}) {
        f64 worst = 0.0;
        bool lfeSilent = true;
        for (int deg = -360; deg <= 360; ++deg) {
            f32 g[kMaxSpeakers];
            speakerGains(n, static_cast<f32>(deg * kPi / 180.0), 0.0f, g);
            f64 p = 0.0;
            for (u32 c = 0; c < n; ++c) p += static_cast<f64>(g[c]) * g[c];
            worst = std::max(worst, std::fabs(p - 1.0));
            if (g[kSpeakerLfe] != 0.0f) lfeSilent = false;
        }
        check(worst < 1.0e-5, std::to_string(n) + " channels: sum of squared gains is 1 at every degree from -360 to 360 (worst error " + num(worst) + ")");
        check(lfeSilent, std::to_string(n) + " channels: the LFE never receives a panned source");

        bool exact = true;
        for (u32 c = 0; c < n; ++c) {
            if (c == kSpeakerLfe) continue;
            f32 g[kMaxSpeakers];
            speakerGains(n, static_cast<f32>(speakerAzimuthDeg(n, c) * kPi / 180.0), 0.0f, g);
            for (u32 o = 0; o < n; ++o) {
                const f32 want = o == c ? 1.0f : 0.0f;
                if (std::fabs(g[o] - want) > 2.0e-4f) exact = false;
            }
        }
        check(exact, std::to_string(n) + " channels: a source exactly at a speaker's angle feeds only that speaker");

        bool equalSpread = true;
        f32 g[kMaxSpeakers];
        speakerGains(n, 1.0f, 1.0f, g);
        const f32 expect = 1.0f / std::sqrt(n == 6 ? 5.0f : 7.0f);
        for (u32 c = 0; c < n; ++c) {
            if (c == kSpeakerLfe) { if (g[c] != 0.0f) equalSpread = false; }
            else if (std::fabs(g[c] - expect) > 1.0e-5f) equalSpread = false;
        }
        check(equalSpread, std::to_string(n) + " channels: spread 1 feeds every directional speaker equally at 1/sqrt(" + std::to_string(n == 6 ? 5 : 7) + ")");

        f64 worstSpread = 0.0;
        for (int deg = -180; deg <= 180; deg += 7)
            for (f32 s : {0.2f, 0.5f, 0.9f}) {
                f32 q[kMaxSpeakers];
                speakerGains(n, static_cast<f32>(deg * kPi / 180.0), s, q);
                f64 p = 0.0;
                for (u32 c = 0; c < n; ++c) p += static_cast<f64>(q[c]) * q[c];
                worstSpread = std::max(worstSpread, std::fabs(p - 1.0));
            }
        check(worstSpread < 1.0e-5, std::to_string(n) + " channels: partial spread keeps the power at 1 (worst " + num(worstSpread) + ")");

        // Continuity: gains change by a bounded amount per degree (no jump anywhere on the ring, including across the back).
        f64 maxStep = 0.0;
        f32 prev[kMaxSpeakers];
        speakerGains(n, static_cast<f32>(-181.0 * kPi / 180.0), 0.0f, prev);
        for (int tenth = -1800; tenth <= 1800; ++tenth) {
            f32 q[kMaxSpeakers];
            speakerGains(n, static_cast<f32>(tenth * 0.1 * kPi / 180.0), 0.0f, q);
            for (u32 c = 0; c < n; ++c) maxStep = std::max(maxStep, static_cast<f64>(std::fabs(q[c] - prev[c])));
            for (u32 c = 0; c < n; ++c) prev[c] = q[c];
        }
        check(maxStep < 0.02, std::to_string(n) + " channels: no gain moves more than " + num(maxStep) + " per tenth of a degree");
    }
    {
        // Stereo through the same function is the legacy constant-power pan on the sine of the azimuth.
        f32 g[kMaxSpeakers], l, r;
        speakerGains(2, static_cast<f32>(30.0 * kPi / 180.0), 0.0f, g);
        panGains(0.5f, l, r);
        check(near(g[0], l, 1.0e-5) && near(g[1], r, 1.0e-5), "two channels: the same gains as panGains for the sine of the azimuth");
    }
}

static void testPlacement() {
    AVER_INFO("=== sources at known angles, rendered through the mixer ===");
    struct Case { u32 n; f64 deg; int dominant; const char* name; };
    const Case cases[] = {
        {6, 0, 2, "5.1 straight ahead: the centre"}, {6, 30, 1, "5.1 at +30: the front right"}, {6, -30, 0, "5.1 at -30: the front left"},
        {6, 110, 5, "5.1 at +110: the right surround"}, {6, -110, 4, "5.1 at -110: the left surround"},
        {8, 0, 2, "7.1 straight ahead: the centre"}, {8, 90, 7, "7.1 at +90: the right side"}, {8, -90, 6, "7.1 at -90: the left side"},
        {8, 150, 5, "7.1 at +150: the right back"}, {8, -150, 4, "7.1 at -150: the left back"}, {8, 30, 1, "7.1 at +30: the front right"},
    };
    for (const Case& k : cases) {
        Mixer m; m.init(kRate, k.n, 8, 8, 2);
        listener(m);
        f32 x, y; atAzimuth(k.deg, x, y);
        playAt(m, m.addSound(constant(0.5f)), x, y, 0.0f);
        const std::vector<f32> out = render(m, 2400);
        f64 best = 0.0; int bi = -1; f64 power = 0.0;
        for (u32 c = 0; c < k.n; ++c) {
            const f64 v = chanMean(out, k.n, c, 480);
            power += v * v;
            if (std::fabs(v) > best) { best = std::fabs(v); bi = static_cast<int>(c); }
        }
        // A mono source at unit level and a stereo-centre pan puts 2 * 0.5^2 of power in the mix; the speakers share it.
        check(bi == k.dominant && near(power, 0.5, 2.0e-3), std::string(k.name) + " (loudest channel " + std::to_string(bi) + ", total power " + num(power) + ")");
        check(near(chanMean(out, k.n, kSpeakerLfe, 480), 0.0, 1.0e-9), "  and the LFE is silent");
    }
    {
        // Between two speakers the gains follow the pair: 5.1 at +90 is 75% of the way from the front right (+30) to the right surround (+110).
        Mixer m; m.init(kRate, 6, 8, 8, 2); listener(m);
        f32 x, y; atAzimuth(90.0, x, y);
        playAt(m, m.addSound(constant(0.5f)), x, y, 0.0f);
        const std::vector<f32> out = render(m, 2400);
        const f64 t = (90.0 - 30.0) / 80.0, gFR = std::cos(t * kPi / 2), gSR = std::sin(t * kPi / 2);
        const f64 k = 0.5 * 1.41421356;
        check(near(chanMean(out, 6, 1, 480), gFR * k, 1.0e-3) && near(chanMean(out, 6, 5, 480), gSR * k, 1.0e-3) &&
                  near(chanMean(out, 6, 0, 480), 0.0, 1.0e-4) && near(chanMean(out, 6, 2, 480), 0.0, 1.0e-4),
              "5.1 at +90 splits between the front right and the right surround by the pair's constant-power law");
    }
    {
        // Directly behind in 5.1: the two surrounds equally. Overhead: every speaker equally.
        Mixer m; m.init(kRate, 6, 8, 8, 2); listener(m);
        playAt(m, m.addSound(constant(0.5f)), -100.0f, 0.0f, 0.0f);
        const std::vector<f32> out = render(m, 2400);
        check(near(chanMean(out, 6, 4, 480), chanMean(out, 6, 5, 480), 1.0e-4) && chanMean(out, 6, 4, 480) > 0.3 && near(chanMean(out, 6, 2, 480), 0.0, 1.0e-4),
              "5.1 directly behind: left and right surround equal, centre silent");
        Mixer o; o.init(kRate, 6, 8, 8, 2); listener(o);
        playAt(o, o.addSound(constant(0.5f)), 0.0f, 0.0f, 100.0f);
        const std::vector<f32> up = render(o, 2400);
        bool equal = true;
        for (u32 c = 0; c < 6; ++c) if (c != kSpeakerLfe && !near(chanMean(up, 6, c, 480), chanMean(up, 6, 0, 480), 1.0e-4)) equal = false;
        check(equal && chanMean(up, 6, 0, 480) > 0.2, "5.1 directly overhead: every directional speaker equal");
    }
    {
        // The listener turning moves the sound across the speakers: a source at +X is at the front, then at the left after a 90 degree turn to face +Y.
        Mixer m; m.init(kRate, 6, 8, 8, 2); listener(m);
        playAt(m, m.addSound(constant(0.5f)), 100.0f, 0.0f, 0.0f);
        render(m, 2400);
        Listener l; l.forward[0] = 0.0f; l.forward[1] = 1.0f;
        l.right[0] = -1.0f; l.right[1] = 0.0f; l.right[2] = 0.0f;       // facing +Y: its right is -X (forward x right = +Z up)
        m.setListener(l);
        const std::vector<f32> out = render(m, 4800);
        // Facing +Y the source that was dead ahead is 90 degrees to the left: 25% of the way from the left surround (-110) to the front left (-30).
        check(chanMean(out, 6, 4, 480) > chanMean(out, 6, 0, 480) && chanMean(out, 6, 0, 480) > 0.2 && near(chanMean(out, 6, 2, 480), 0.0, 1.0e-3) &&
                  near(chanMean(out, 6, 1, 480), 0.0, 1.0e-3) && near(chanMean(out, 6, 5, 480), 0.0, 1.0e-3),
              "turning the listener to face +Y puts the +X source on the left (left surround and front left play; centre and the right side do not)");
    }
}

static void testUpmix() {
    AVER_INFO("=== stereo assets upmixed ===");
    for (u32 n : {6u, 8u})
        for (StereoUpmix mode : {StereoUpmix::Front, StereoUpmix::Wrap}) {
            const UpmixMatrix um = stereoUpmix(n, mode);
            f64 pl = 0.0, pr = 0.0;
            for (u32 c = 0; c < n; ++c) { pl += static_cast<f64>(um.l[c]) * um.l[c]; pr += static_cast<f64>(um.r[c]) * um.r[c]; }
            check(near(pl, 1.0, 1.0e-6) && near(pr, 1.0, 1.0e-6) && um.l[2] == 0.0f && um.r[2] == 0.0f && um.l[3] == 0.0f && um.r[3] == 0.0f &&
                      um.l[1] == 0.0f && um.r[0] == 0.0f,
                  std::string(n == 6 ? "5.1 " : "7.1 ") + (mode == StereoUpmix::Wrap ? "wrap" : "front") +
                      ": each input channel keeps unit power, the centre and LFE get nothing, no left-to-right leakage");
        }
    for (u32 n : {6u, 8u}) {
        Mixer m; m.init(kRate, n, 8, 8, 2);
        SoundData st; st.channels = 2; st.sampleRate = kRate; st.samples.resize(static_cast<usize>(kRate) * 2);
        for (u32 i = 0; i < kRate; ++i) { st.samples[i * 2] = 0.6f; st.samples[i * 2 + 1] = 0.0f; }
        PlayDesc d; d.sound = m.addSound(std::move(st)); d.looping = true; m.play(d);
        const std::vector<f32> out = render(m, 2400);
        f64 power = 0.0;
        for (u32 c = 0; c < n; ++c) { const f64 v = chanMean(out, n, c, 480); power += v * v; }
        check(near(chanMean(out, n, 0, 480), 0.6 * std::sqrt(0.75), 1.0e-4) && near(chanMean(out, n, 1, 480), 0.0, 1.0e-6) &&
                  near(chanMean(out, n, 2, 480), 0.0, 1.0e-6) && near(power, 0.36, 1.0e-4),
              std::string(n == 6 ? "5.1" : "7.1") + ": a left-only stereo asset keeps its power (0.36) and stays on the left side");
        check(chanMean(out, n, 4, 480) > 0.15 && (n == 6 ? near(chanMean(out, n, 5, 480), 0.0, 1.0e-6) : chanMean(out, n, 6, 480) > 0.15 && near(chanMean(out, n, 7, 480), 0.0, 1.0e-6)),
              "  with the surrounds on the left only");
        m.setStereoUpmix(StereoUpmix::Front);
        const std::vector<f32> fr = render(m, 2400);
        check(near(chanMean(fr, n, 0, 480), 0.6, 1.0e-4) && near(chanMean(fr, n, 4, 480), 0.0, 1.0e-6), "  and Front puts it on the front left alone");
    }
    {
        // The reverb return reaches a surround mixer through the same matrix.
        Mixer m; m.init(kRate, 6, 8, 8, 2); listener(m);
        m.setReverb({0.8f, 1.0f, 0.3f});
        PlayDesc d; d.sound = m.addSound(sine(300.0f, 0.5f)); d.looping = true; m.play(d);
        const std::vector<f32> out = render(m, 48000);
        const f64 fl = chanRms(out, 6, 0, 24000, 48000), ls = chanRms(out, 6, 4, 24000, 48000), c = chanRms(out, 6, 2, 24000, 48000);
        check(ls > 0.01 && c < 1.0e-6 && fl > ls, "the reverb return and a flat stereo-upmixed voice reach the surrounds, not the centre (front " + num(fl) + ", surround " + num(ls) + ")");
    }
}

static void testLfe() {
    AVER_INFO("=== the LFE send ===");
    {
        Mixer m; m.init(kRate, 6, 8, 8, 2);
        PlayDesc d; d.sound = m.addSound(sine(40.0f, 0.5f)); d.looping = true; m.play(d);
        const std::vector<f32> off = render(m, 9600);
        check(chanRms(off, 6, kSpeakerLfe, 0, 9600) == 0.0, "by default the LFE is exactly silent even with a 40 Hz tone playing");
        m.setLfeSend(0.8f);
        const std::vector<f32> on = render(m, 24000);
        const f64 low = chanRms(on, 6, kSpeakerLfe, 12000, 24000);
        check(low > 0.15, "with a send of 0.8 a 40 Hz tone reaches the LFE (rms " + num(low) + ")");
        // A high tone is cut by the 120 Hz low-pass.
        Mixer h; h.init(kRate, 6, 8, 8, 2); h.setLfeSend(0.8f);
        PlayDesc e; e.sound = h.addSound(sine(2000.0f, 0.5f)); e.looping = true; h.play(e);
        const std::vector<f32> hi = render(h, 24000);
        const f64 high = chanRms(hi, 6, kSpeakerLfe, 12000, 24000);
        check(high < 0.01, "a 2 kHz tone is low-passed out of the LFE (rms " + num(high) + ")");
        m.setLfeSend(0.0f);
        const std::vector<f32> after = render(m, 24000);
        f64 peak = 0.0;
        for (usize f = 12000; f < 24000; ++f) peak = std::max(peak, static_cast<f64>(std::fabs(after[f * 6 + kSpeakerLfe])));
        check(peak == 0.0, "turning the send back to 0 ramps it out and leaves the LFE exactly silent again");
        // Smoothness of the switch-off: no step in the LFE larger than the tone itself could make.
        f64 maxStep = 0.0;
        for (usize f = 1; f < 24000; ++f) maxStep = std::max(maxStep, static_cast<f64>(std::fabs(after[f * 6 + kSpeakerLfe] - after[(f - 1) * 6 + kSpeakerLfe])));
        check(maxStep < 0.02, "  without a click (largest LFE step " + num(maxStep) + ")");
    }
}

static void testDeviceChoice() {
    AVER_INFO("=== which layout a device gets, and which device channel each mixer channel feeds ===");
    using namespace chmask;
    const u32 m71 = FrontLeft | FrontRight | FrontCentre | Lfe | BackLeft | BackRight | SideLeft | SideRight;     // 0x63F
    const u32 m51back = FrontLeft | FrontRight | FrontCentre | Lfe | BackLeft | BackRight;                          // 0x3F
    const u32 m51side = FrontLeft | FrontRight | FrontCentre | Lfe | SideLeft | SideRight;                          // 0x60F
    check(chooseMixChannels(2, 0, 2) == 2 && chooseMixChannels(2, 0, 8) == 2 && chooseMixChannels(1, 0, 6) == 1, "a stereo device stays stereo whatever is asked; a mono device is mono");
    check(chooseMixChannels(6, m51back, 2) == 2 && chooseMixChannels(8, m71, 2) == 2, "the default request (2) never changes the layout of a surround device");
    check(chooseMixChannels(6, m51back, 6) == 6 && chooseMixChannels(6, m51side, 8) == 6 && chooseMixChannels(6, 0, 6) == 6, "a 5.1 device gets 5.1 for a request of 6 or 8");
    check(chooseMixChannels(8, m71, 8) == 8 && chooseMixChannels(8, m71, 6) == 6 && chooseMixChannels(8, 0, 8) == 8, "a 7.1 device gets 7.1 only when 7.1 is asked");
    check(chooseMixChannels(8, FrontLeft | FrontRight | FrontCentre | Lfe | BackLeft | BackRight | 0x40 | 0x80, 8) == 6, "an 8-channel device that is not 7.1 (no sides) is played as 5.1");
    check(chooseMixChannels(4, FrontLeft | FrontRight | BackLeft | BackRight, 6) == 2 && chooseMixChannels(6, FrontLeft | FrontRight | FrontCentre | BackLeft | BackRight | 0x100, 6) == 2,
          "a quad device, or a 6-channel mask without an LFE, stays stereo");

    i8 map[8];
    buildDeviceMap(6, 8, m71, map, 8);
    check(map[0] == 0 && map[1] == 1 && map[2] == 2 && map[3] == 3 && map[4] == -1 && map[5] == -1 && map[6] == 4 && map[7] == 5,
          "a 5.1 mix on a 7.1 device puts its surrounds on the side speakers and leaves the backs silent");
    buildDeviceMap(8, 8, m71, map, 8);
    bool id = true; for (int i = 0; i < 8; ++i) if (map[i] != i) id = false;
    check(id, "a 7.1 mix on a 7.1 device is the identity (WAVE order is L R C LFE Lb Rb Ls Rs)");
    buildDeviceMap(6, 6, m51side, map, 8);
    id = true; for (int i = 0; i < 6; ++i) if (map[i] != i) id = false;
    check(id, "5.1 on a side-surround 5.1 device and (below) on a back-surround one are both the identity");
    buildDeviceMap(6, 6, m51back, map, 8);
    id = true; for (int i = 0; i < 6; ++i) if (map[i] != i) id = false;
    check(id, "  (back variant)");
    buildDeviceMap(2, 8, m71, map, 8);
    check(map[0] == 0 && map[1] == 1 && map[2] == -1 && map[7] == -1, "a stereo mix on a 7.1 device feeds the first two channels, as it always did");
    buildDeviceMap(6, 8, 0, map, 8);
    check(map[5] == 5 && map[6] == -1 && map[7] == -1, "with no mask the mix goes to the first channels in order");
    buildDeviceMap(6, 8, m71, map, 4);
    check(map[3] == 3, "the map never writes past the capacity it is given");
}

// ---- the fake device ----

struct FakeClock final : OutputClock {
    f64 t = 10.0;
    f64 now() const override { return t; }
    void sleep(f64 s) override { if (s > 0.0) t += s; }
};

struct FakeBackend final : OutputBackend {
    FakeClock& clock;
    OutputInfo fmt;
    bool exists = true, loseNext = false;
    OutputSource* src = nullptr;
    u32 mixCh = 0;
    int opens = 0, starts = 0, closes = 0;
    std::vector<f32> buf;
    explicit FakeBackend(FakeClock& c) : clock(c) {}
    bool open(OutputInfo& info) override { ++opens; if (!exists) return false; info = fmt; return true; }
    bool start(const OutputInfo& info, OutputSource& s) override { ++starts; src = &s; mixCh = info.mixChannels; return true; }
    void close() override { ++closes; src = nullptr; }
    ServiceResult service(u32) override {
        if (loseNext) { loseNext = false; return ServiceResult::Lost; }
        buf.assign(static_cast<usize>(480) * mixCh, 0.0f);
        src->render(buf.data(), 480);
        clock.sleep(480.0 / fmt.rate);
        return ServiceResult::Ok;
    }
    const char* lastError() const override { return "none"; }
};

static OutputInfo devInfo(u32 channels, u32 mask, u32 rate = 48000) {
    OutputInfo i; i.name = "Fake"; i.rate = rate; i.deviceChannels = channels; i.channelMask = mask; i.bufferFrames = 480; return i;
}

static void testDriver() {
    AVER_INFO("=== the output policy and the layout ===");
    using namespace chmask;
    const u32 m71 = FrontLeft | FrontRight | FrontCentre | Lfe | BackLeft | BackRight | SideLeft | SideRight;
    {
        FakeClock clock; FakeBackend be(clock); be.fmt = devInfo(8, m71);
        Mixer m;
        OutputDriver d(m, be, clock);
        check(d.begin() && m.channels() == 2 && d.info().mixChannels == 2, "a 7.1 device with the default request is mixed in stereo, as before");
        d.end();
    }
    {
        FakeClock clock; FakeBackend be(clock); be.fmt = devInfo(8, m71);
        Mixer m; m.setSpeakerRequest(6);
        OutputDriver d(m, be, clock);
        check(d.begin() && m.channels() == 6 && d.info().mixChannels == 6 && d.info().deviceChannels == 8, "request 6 on a 7.1 device: the mixer renders 5.1 (the backend maps it onto the sides)");
        d.step();
        check(be.buf.size() == 480u * 6u, "  and the backend is asked for 6-channel blocks");
        d.end();
    }
    {
        FakeClock clock; FakeBackend be(clock); be.fmt = devInfo(2, 0);
        Mixer m; m.setSpeakerRequest(8);
        OutputDriver d(m, be, clock);
        check(d.begin() && m.channels() == 2, "request 8 on a stereo device: stereo");
        m.setSpeakerRequest(6);
        for (int i = 0; i < 4; ++i) d.step();
        check(d.deviceChanges() == 0 && be.opens == 1, "  and changing the request does not reopen a device that cannot use it");
        d.end();
    }
    {
        FakeClock clock; FakeBackend be(clock); be.fmt = devInfo(8, m71);
        Mixer m;
        OutputDriver d(m, be, clock);
        d.begin();
        PlayDesc pd; pd.sound = m.addSound(constant(0.4f)); pd.looping = true;
        const VoiceHandle v = m.play(pd);
        m.setBusVolume(Bus::Music, 0.5f);
        for (int i = 0; i < 3; ++i) d.step();
        m.setSpeakerRequest(8);
        d.step();
        check(d.deviceChanges() == 1 && m.channels() == 8 && d.state() == OutputState::Running, "asking for 7.1 with the device open reopens it as 7.1");
        check(m.playing(v) && near(m.busVolume(Bus::Music), 0.5, 1.0e-6), "  the voice and the bus volume are kept");
        d.step();
        check(be.buf.size() == 480u * 8u && be.buf[479 * 8 + 0] > 0.2f && be.buf[479 * 8 + 1] > 0.2f, "  and the next block is 8 channels with the voice on the front pair");
        m.setSpeakerRequest(2);
        d.step();
        check(d.deviceChanges() == 2 && m.channels() == 2 && m.playing(v), "back to the default: reopened as stereo, the voice kept");
        // The request is honoured even right after another rebuild (a layout change is not a flapping device).
        m.setSpeakerRequest(6);
        d.step();
        m.setSpeakerRequest(8);
        d.step();
        check(d.deviceChanges() == 4 && m.channels() == 8, "layout changes in quick succession are all applied");
        d.end();
    }
    {
        // No device: the idle sink must hold the widest layout.
        FakeClock clock; FakeBackend be(clock); be.fmt = devInfo(8, m71);
        Mixer m; m.setSpeakerRequest(8);
        OutputDriver d(m, be, clock);
        d.begin();
        PlayDesc pd; pd.sound = m.addSound(constant(0.4f, kRate * 5)); pd.looping = true;
        const VoiceHandle v = m.play(pd);
        be.exists = false; be.loseNext = true;
        for (int i = 0; i < 6; ++i) d.step();
        check(d.state() == OutputState::NoDevice && m.channels() == 8, "a 7.1 device that goes away: no device, still an 8-channel mixer");
        const f64 before = m.voicePositionSeconds(v);
        for (int i = 0; i < 300; ++i) d.step();
        check(m.voicePositionSeconds(v) > before + 1.0, "  the mixer keeps running into the idle sink at real time (position +" + num(m.voicePositionSeconds(v) - before) + " s)");
        be.exists = true;
        for (int i = 0; i < 400 && d.state() != OutputState::Running; ++i) d.step();
        check(d.state() == OutputState::Running && m.channels() == 8, "  and the device coming back is taken as 7.1 again");
        d.end();
    }
}

static void testSetOutputFormat() {
    AVER_INFO("=== Mixer::setOutputFormat between layouts ===");
    Mixer m; m.init(kRate, 2, 8, 8, 2); listener(m);
    check(!m.setOutputFormat(kRate, 3) && !m.setOutputFormat(kRate, 4) && !m.setOutputFormat(kRate, 7), "3, 4 and 7 channels are refused");
    const VoiceHandle v = playAt(m, m.addSound(constant(0.5f)), 100.0f, 0.0f, 0.0f);
    render(m, 960);
    check(m.setOutputFormat(kRate, 6) && m.channels() == 6 && m.playing(v), "stereo to 5.1 keeps the voice");
    std::vector<f32> out = render(m, 960);
    check(chanMean(out, 6, 2, 480) > 0.6 && near(chanMean(out, 6, 0, 480), 0.0, 1.0e-3), "  and it is on the centre from the first block (no ramp from a stale stereo gain)");
    check(m.setOutputFormat(44100, 8) && m.channels() == 8 && m.playing(v), "5.1 to 7.1 at another rate");
    out = render(m, 960);
    check(chanMean(out, 8, 2, 480) > 0.6, "  still on the centre");
    check(m.setOutputFormat(kRate, 2) && m.playing(v), "and back to stereo");
    out = render(m, 960);
    check(chanMean(out, 2, 0, 480) > 0.4 && near(chanMean(out, 2, 0, 480), chanMean(out, 2, 1, 480), 1.0e-3), "  centred in both ears");
}

// ---- the head model ----

// Cross-correlation lag (in samples, fractional) at which `a` best matches `b` shifted: positive when `a` is later than `b`.
static f64 lagOf(const std::vector<f32>& a, const std::vector<f32>& b, int maxLag, usize from, usize to) {
    std::vector<f64> c(static_cast<usize>(2 * maxLag + 1), 0.0);
    for (int lag = -maxLag; lag <= maxLag; ++lag) {
        f64 s = 0.0;
        for (usize n = from; n < to; ++n) {
            const long m = static_cast<long>(n) - lag;
            if (m >= 0 && m < static_cast<long>(b.size())) s += static_cast<f64>(a[n]) * b[static_cast<usize>(m)];
        }
        c[static_cast<usize>(lag + maxLag)] = s;
    }
    usize best = 0;
    for (usize i = 1; i < c.size(); ++i) if (c[i] > c[best]) best = i;
    f64 frac = 0.0;
    if (best > 0 && best + 1 < c.size()) {
        const f64 y0 = c[best - 1], y1 = c[best], y2 = c[best + 1];
        const f64 den = y0 - 2.0 * y1 + y2;
        if (den != 0.0) frac = 0.5 * (y0 - y2) / den;
    }
    return static_cast<f64>(static_cast<int>(best) - maxLag) + frac;
}

struct Ears { std::vector<f32> l, r; };
static Ears impulseResponse(f64 azDeg, f64 elDeg, bool binauralOn = true, u32 frames = 2048) {
    Mixer m; m.init(kRate, 2, 8, 8, 2); listener(m);
    m.setBinaural(binauralOn);
    const f64 az = azDeg * kPi / 180.0, el = elDeg * kPi / 180.0;
    PlayDesc d; d.sound = m.addSound(impulse(frames)); d.positional = true; d.looping = false;
    d.position[0] = static_cast<f32>(100.0 * std::cos(el) * std::cos(az));
    d.position[1] = static_cast<f32>(100.0 * std::cos(el) * std::sin(az));
    d.position[2] = static_cast<f32>(100.0 * std::sin(el));
    m.play(d);
    const std::vector<f32> out = render(m, frames);
    Ears e;
    for (usize f = 0; f < out.size() / 2; ++f) { e.l.push_back(out[f * 2]); e.r.push_back(out[f * 2 + 1]); }
    return e;
}
static f64 energy(const std::vector<f32>& v) { f64 s = 0.0; for (f32 x : v) s += static_cast<f64>(x) * x; return s; }
static f64 hfEnergy(const std::vector<f32>& v) { f64 s = 0.0; for (usize i = 1; i < v.size(); ++i) { const f64 d = static_cast<f64>(v[i]) - v[i - 1]; s += d * d; } return s; }
// |H| of an impulse response at `hz`.
static f64 magAt(const std::vector<f32>& h, f64 hz) {
    f64 re = 0.0, im = 0.0;
    for (usize n = 0; n < h.size(); ++n) { const f64 w = 2.0 * kPi * hz * static_cast<f64>(n) / kRate; re += h[n] * std::cos(w); im -= h[n] * std::sin(w); }
    return std::sqrt(re * re + im * im);
}

static void testBinauralModel() {
    AVER_INFO("=== the analytic head model ===");
    check(near(binauralMaxItd() * 1000.0, 0.656, 0.002), "the largest interaural time difference is " + num(binauralMaxItd() * 1000.0) + " ms (spherical head, 8.75 cm)");
    check(near(binauralItd(static_cast<f32>(kPi / 2), 0.0f), binauralMaxItd(), 1.0e-7) && near(binauralItd(static_cast<f32>(-kPi / 2), 0.0f), -binauralMaxItd(), 1.0e-7) &&
              near(binauralItd(0.0f, 0.0f), 0.0, 1.0e-9) && near(binauralItd(static_cast<f32>(kPi), 0.0f), 0.0, 1.0e-6),
          "the delay is +max at 90 degrees right, -max at 90 left, and 0 straight ahead and straight behind");
    check(near(binauralItd(static_cast<f32>(kPi / 6), 0.0f) * 1000.0, 0.2551 * (kPi / 6 + 0.5), 0.002), "at 30 degrees it follows Woodworth's (a/c)(angle + sin angle): " + num(binauralItd(static_cast<f32>(kPi / 6), 0.0f) * 1000.0) + " ms");
    check(near(binauralItd(static_cast<f32>(kPi / 4), 0.0f), binauralItd(static_cast<f32>(3 * kPi / 4), 0.0f), 1.0e-6), "front and back mirror images (45 and 135 degrees) have the same delay: the cone of confusion");
    check(binauralItd(static_cast<f32>(kPi / 2), static_cast<f32>(kPi / 3)) < 0.6f * binauralMaxItd(), "a source lifted 60 degrees has a smaller delay at the same azimuth");

    {
        const BinauralTargets t = binauralTargets(static_cast<f32>(kPi / 2), 0.0f, kRate);
        check(near(t.delay[0] - t.delay[1], binauralMaxItd() * kRate, 1.0e-3) && t.delay[1] >= 0.0f && t.delay[0] <= 40.0f,
              "the ear delays differ by the ITD in samples (" + num(t.delay[0] - t.delay[1]) + ") and are never negative");
        check(t.shadow[1] > 1.5f && t.shadow[0] < 0.3f, "a source on the right: the right ear's head shadow boosts the highs (" + num(t.shadow[1]) + "), the left cuts them (" + num(t.shadow[0]) + ")");
        const BinauralTargets c = binauralTargets(0.0f, 0.0f, kRate);
        check(near(c.shadow[0], c.shadow[1], 1.0e-5) && near(c.delay[0], c.delay[1], 1.0e-5), "straight ahead the two ears get identical parameters");
        const BinauralTargets lo = binauralTargets(0.0f, static_cast<f32>(-kPi / 3), kRate), hi = binauralTargets(0.0f, static_cast<f32>(kPi / 3), kRate);
        check(hi.pinna < lo.pinna, "the pinna reflection arrives sooner for a source above than below (notch moves up)");
    }
    {
        // The slew limit: a teleport of the target never moves a delay faster than half a sample per sample.
        BinauralState s;
        BinauralTargets a = binauralTargets(static_cast<f32>(-kPi / 2), 0.0f, kRate), b = binauralTargets(static_cast<f32>(kPi / 2), 0.0f, kRate);
        s.begin(a, 480, kRate, true);
        s.begin(b, 480, kRate, false);
        f32 prev0 = s.delay[0]; f64 worst = 0.0; f32 l, r;
        for (int i = 0; i < 480; ++i) { s.tick(0.0f, l, r); worst = std::max(worst, static_cast<f64>(std::fabs(s.delay[0] - prev0))); prev0 = s.delay[0]; }
        check(worst <= 0.5 + 1.0e-4, "a 180 degree jump in one block moves the delay at most " + num(worst) + " samples per sample");
    }

    AVER_INFO("=== binaural rendering through the mixer ===");
    {
        struct C { f64 az; };
        for (f64 az : {90.0, -90.0, 30.0, -60.0, 0.0}) {
            const Ears e = impulseResponse(az, 0.0);
            const f64 lag = lagOf(e.l, e.r, 60, 0, 300);                     // positive: the left ear is later
            const f64 want = binauralItd(static_cast<f32>(az * kPi / 180.0), 0.0f) * kRate;
            check(near(lag, want, 1.5), "source at " + num(az) + " degrees: the left ear is " + num(lag) + " samples after the right (the model says " + num(want) + ")");
            if (az == 90.0) check(lag > 25.0 && lag < 36.0 && near(lag / kRate * 1000.0, 0.656, 0.04), "  +90 is about 0.66 ms with the left ear later");
            if (az == -90.0) check(lag < -25.0, "  -90 is the mirror: the right ear is later");
            if (az == 0.0) check(std::fabs(lag) < 0.25, "  straight ahead the ears agree");
        }
        const Ears right = impulseResponse(90.0, 0.0), left = impulseResponse(-90.0, 0.0), ahead = impulseResponse(0.0, 0.0);
        check(energy(right.r) > 1.8 * energy(right.l) && energy(left.l) > 1.8 * energy(left.r), "the near ear is louder: right source " + num(energy(right.r) / energy(right.l)) + "x, left source " + num(energy(left.l) / energy(left.r)) + "x");
        check(near(energy(ahead.l), energy(ahead.r), 1.0e-6 * energy(ahead.l)), "straight ahead the two ears carry equal energy");
        check(hfEnergy(right.r) / energy(right.r) > 2.0 * hfEnergy(right.l) / energy(right.l), "the far ear is also duller (high-frequency share: near " + num(hfEnergy(right.r) / energy(right.r)) + ", far " + num(hfEnergy(right.l) / energy(right.l)) + ")");

        const Ears back = impulseResponse(180.0, 0.0);
        check(hfEnergy(ahead.l) / energy(ahead.l) > 1.15 * hfEnergy(back.l) / energy(back.l), "front is brighter than back (high-frequency share ahead " + num(hfEnergy(ahead.l) / energy(ahead.l)) + ", behind " + num(hfEnergy(back.l) / energy(back.l)) + ")");

        // Elevation: the pinna notch sits lower for a source below than above.
        const Ears below = impulseResponse(0.0, -60.0, true, 1024), above = impulseResponse(0.0, 60.0, true, 1024);
        auto notch = [](const std::vector<f32>& h) { f64 bestHz = 0.0, bestMag = 1.0e30; for (f64 hz = 4000.0; hz <= 11500.0; hz += 100.0) { const f64 g = magAt(h, hz); if (g < bestMag) { bestMag = g; bestHz = hz; } } return bestHz; };
        const f64 nb = notch(below.l), na = notch(above.l);
        check(na > nb + 1000.0, "the pinna notch moves up with elevation (below: " + num(nb) + " Hz, above: " + num(na) + " Hz)");

        // Off by default and ignored where it does not apply: identical samples.
        const Ears plain = impulseResponse(90.0, 0.0, false);
        check(plain.l != right.l && plain.r != right.r, "binaural off gives the plain pan, not the head model");
    }
}

static void testBinauralSmooth() {
    AVER_INFO("=== a moving source: smooth parameters ===");
    auto steps = [](auto positionAt) {
        Mixer m; m.init(kRate, 2, 8, 8, 2); listener(m); m.setBinaural(true);
        PlayDesc d; d.sound = m.addSound(sine(440.0f, 0.5f)); d.looping = true; d.positional = true; d.position[0] = 100.0f;
        const VoiceHandle v = m.play(d);
        f64 maxStep = 0.0; f32 pl = 0.0f, pr = 0.0f; std::vector<f32> blk(480 * 2);
        for (int b = 0; b < 200; ++b) {
            f32 x, y; positionAt(b, x, y);
            m.setVoicePosition(v, x, y, 0.0f);
            m.mix(blk.data(), 480);
            for (int f = 0; f < 480; ++f) {
                if (b > 4) { maxStep = std::max(maxStep, static_cast<f64>(std::fabs(blk[f * 2] - pl))); maxStep = std::max(maxStep, static_cast<f64>(std::fabs(blk[f * 2 + 1] - pr))); }
                pl = blk[f * 2]; pr = blk[f * 2 + 1];
            }
        }
        return maxStep;
    };
    f64 staticMax = 0.0;
    for (int deg = 0; deg < 360; deg += 30)
        staticMax = std::max(staticMax, steps([deg](int, f32& x, f32& y) { atAzimuth(deg, x, y); }));
    const f64 sweepMax = steps([](int b, f32& x, f32& y) { atAzimuth(360.0 * (b * 480.0 / kRate) / 2.0, x, y); });
    const f64 jumpMax = steps([](int b, f32& x, f32& y) { atAzimuth(b < 100 ? -90.0 : 90.0, x, y); });
    AVER_INFO("  largest sample step: static sources {}, one revolution in 2 s {}, a 180 degree jump {}", staticMax, sweepMax, jumpMax);
    check(staticMax > 0.02 && staticMax < 0.2, "the static reference is a plausible 440 Hz step (" + num(staticMax) + ")");
    check(sweepMax <= 1.5 * staticMax, "a source circling the head makes no larger steps than a still one does (" + num(sweepMax) + " against " + num(staticMax) + "): no zipper noise");
    check(jumpMax <= 2.0 * staticMax, "even a 180 degree teleport stays within twice a static step (" + num(jumpMax) + "): delays slew, they do not click");

    // The same in 5.1: gains ramp across blocks, so a circling source makes no larger steps than the loudest static one.
    auto steps6 = [](auto positionAt) {
        Mixer m; m.init(kRate, 6, 8, 8, 2); listener(m);
        PlayDesc d; d.sound = m.addSound(sine(440.0f, 0.5f)); d.looping = true; d.positional = true; d.position[0] = 100.0f;
        const VoiceHandle v = m.play(d);
        f64 maxStep = 0.0; f32 prev[6] = {}; std::vector<f32> blk(480 * 6);
        for (int b = 0; b < 200; ++b) {
            f32 x, y; positionAt(b, x, y);
            m.setVoicePosition(v, x, y, 0.0f);
            m.mix(blk.data(), 480);
            for (int f = 0; f < 480; ++f)
                for (int c = 0; c < 6; ++c) { if (b > 4) maxStep = std::max(maxStep, static_cast<f64>(std::fabs(blk[f * 6 + c] - prev[c]))); prev[c] = blk[f * 6 + c]; }
        }
        return maxStep;
    };
    f64 staticMax6 = 0.0;
    for (int deg = 0; deg < 360; deg += 15)
        staticMax6 = std::max(staticMax6, steps6([deg](int, f32& x, f32& y) { atAzimuth(deg, x, y); }));
    const f64 sweep6 = steps6([](int b, f32& x, f32& y) { atAzimuth(360.0 * (b * 480.0 / kRate) / 2.0, x, y); });
    const f64 jump6 = steps6([](int b, f32& x, f32& y) { atAzimuth(b < 100 ? -90.0 : 90.0, x, y); });
    check(sweep6 <= 1.2 * staticMax6, "5.1: a circling source makes no larger steps than the loudest still one (" + num(sweep6) + " against " + num(staticMax6) + ")");
    check(jump6 <= 1.2 * staticMax6, "5.1: a teleport across the room is a gain ramp over one block, not a click (" + num(jump6) + ")");
}

// ---- defaults ----

static void scene(Mixer& m) {
    listener(m);
    PlayDesc a; a.sound = m.addSound(sine(440.0f, 0.4f)); a.looping = true; m.play(a);
    PlayDesc b; b.sound = m.addSound(sine(1234.0f, 0.3f)); b.looping = true; b.positional = true; b.position[1] = 300.0f; b.position[0] = -150.0f; b.position[2] = 80.0f;
    const VoiceHandle vb = m.play(b);
    m.setVoiceOcclusion(vb, 0.3f);
    PlayDesc c; c.sound = m.addSound(sine(90.0f, 0.5f)); c.looping = true; c.bus = Bus::Music; c.fadeInSeconds = 0.1f; m.play(c);
    m.setReverb({0.3f, 1.2f, 0.4f});
}

static void testDefaults() {
    AVER_INFO("=== defaults unchanged ===");
    check(Mixer().speakerRequest() == 2 && !Mixer().binaural() && Mixer().lfeSend() == 0.0f, "a new mixer asks for stereo, has binaural off and no LFE send");
    Mixer ref; ref.init(kRate, 2, 16, 16, 2); scene(ref);
    const std::vector<f32> a = render(ref, 24000);

    // Settings that only concern other layouts leave a stereo mixer's samples alone.
    Mixer other; other.init(kRate, 2, 16, 16, 2);
    other.setSpeakerRequest(8); other.setLfeSend(0.9f); other.setStereoUpmix(StereoUpmix::Front);
    scene(other);
    check(render(other, 24000) == a, "a stereo mixer with a speaker request of 8, an LFE send and the Front upmix renders bit-identical samples");

    // Binaural toggled on and back off before play: identical. And with it on, voices that are not positional are untouched.
    Mixer toggled; toggled.init(kRate, 2, 16, 16, 2);
    toggled.setBinaural(true); toggled.setBinaural(false);
    scene(toggled);
    check(render(toggled, 24000) == a, "binaural switched on and off again changes nothing");

    Mixer flat1; flat1.init(kRate, 2, 16, 16, 2); Mixer flat2; flat2.init(kRate, 2, 16, 16, 2);
    flat2.setBinaural(true);
    for (Mixer* mm : {&flat1, &flat2}) {
        PlayDesc d; d.sound = mm->addSound(sine(500.0f, 0.4f)); d.looping = true; mm->play(d);
        PlayDesc e; e.sound = mm->addSound(sine(800.0f, 0.2f)); e.looping = true; e.bus = Bus::Ui; mm->play(e);
    }
    check(render(flat1, 12000) == render(flat2, 12000), "with binaural on, non-positional voices render bit-identical samples");

    // A mono mixer and a mixer on a spatial-model voice are untouched by binaural off; with a mono mixer it is ignored when on.
    Mixer mono1; mono1.init(kRate, 1, 16, 16, 2); Mixer mono2; mono2.init(kRate, 1, 16, 16, 2); mono2.setBinaural(true);
    for (Mixer* mm : {&mono1, &mono2}) { listener(*mm); playAt(*mm, mm->addSound(sine(300.0f, 0.4f)), 0.0f, 200.0f, 0.0f); }
    check(render(mono1, 6000) == render(mono2, 6000), "binaural is ignored on a mono mixer");

    // 5.1 with binaural on is the same as 5.1 with it off.
    Mixer s1; s1.init(kRate, 6, 16, 16, 2); Mixer s2; s2.init(kRate, 6, 16, 16, 2); s2.setBinaural(true);
    scene(s1); scene(s2);
    check(render(s1, 12000) == render(s2, 12000), "binaural is ignored on a 5.1 mixer");
}

static void testRealTime() {
    AVER_INFO("=== the real-time rule ===");
    for (u32 n : {2u, 6u, 8u}) {
        Mixer m; m.init(kRate, n, 32, 32, 2); scene(m);
        m.setLfeSend(0.5f);
        if (n == 2) m.setBinaural(true);
        std::vector<VoiceHandle> vs;
        for (int i = 0; i < 6; ++i) {
            f32 x, y; atAzimuth(i * 60.0, x, y);
            vs.push_back(playAt(m, m.addSound(sine(200.0f + 90.0f * i, 0.2f)), x, y, 0.0f));
        }
        std::vector<f32> blk(static_cast<usize>(480) * n);
        for (int i = 0; i < 20; ++i) m.mix(blk.data(), 480);
        g_allocs = 0; g_counting = true;
        for (int b = 0; b < 300; ++b) {
            for (usize i = 0; i < vs.size(); ++i) { f32 x, y; atAzimuth(b * 3.0 + i * 60.0, x, y); m.setVoicePosition(vs[i], x, y, 20.0f * static_cast<f32>(i)); }
            if (b == 100) m.setLfeSend(0.0f);
            if (b == 200) m.setLfeSend(0.7f);
            m.mix(blk.data(), 480);
        }
        g_counting = false;
        check(g_allocs.load() == 0, std::to_string(n) + " channels: 300 blocks of moving voices, LFE changes, reverb and occlusion allocate " + std::to_string(g_allocs.load()) + " times in mix()");
    }
    g_allocs = 0; g_counting = true;
    { auto* p = new int(3); delete p; }
    g_counting = false;
    check(g_allocs.load() >= 1, "the counting hook sees an ordinary allocation (control)");
}

int main() {
    testPanLaw();
    testPlacement();
    testUpmix();
    testLfe();
    testDeviceChoice();
    testDriver();
    testSetOutputFormat();
    testBinauralModel();
    testBinauralSmooth();
    testDefaults();
    testRealTime();
    if (g_failures) { AVER_ERROR("{} check(s) FAILED", g_failures); return 1; }
    AVER_INFO("all surround and binaural checks passed");
    return 0;
}
