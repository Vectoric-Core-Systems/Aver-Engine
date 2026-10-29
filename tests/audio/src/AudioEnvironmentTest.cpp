// Environment audio (docs/AUDIO.md section 15), no device: the partitioned FFT convolver against a direct time-domain convolution, the
// convolution reverb effect, the zone send reverbs inside the mixer (independent slots, exact ramps, a clean restart, no allocation in
// mix()), and diffraction around an edge and through an opening. Everything is rendered into buffers and compared sample by sample.
#include "aver/audio/Convolver.hpp"
#include "aver/audio/Effects.hpp"
#include "aver/audio/Mixer.hpp"
#include "aver/audio/Music.hpp"
#include "aver/audio/Reverb.hpp"
#include "aver/core/Log.hpp"
#if AVER_TEST_HAS_FORMATS
#include "aver/formats/OcAudio.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

using namespace aver;

// ---- the allocation hook: every operator new made while a window is open is counted -----------------------------------------------------------
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
static bool near(f32 a, f32 b, f32 eps = 1.0e-4f) { return std::fabs(a - b) <= eps; }

constexpr u32 kRate = 48000;
constexpr u32 kBlock = audio::Convolver::kDefaultBlock;

// ---------------------------------------------------------------- helpers

static std::vector<f32> noise(u32 n, u32 seed, f32 amp = 1.0f) {
    std::vector<f32> v(n);
    u32 s = seed * 2654435761u + 12345u;
    for (u32 i = 0; i < n; ++i) {
        s = s * 1664525u + 1013904223u;
        v[i] = amp * (static_cast<f32>((s >> 8) & 0xFFFF) / 32768.0f - 1.0f);
    }
    return v;
}

// out[n] = sum_k h[k] x[n-k] for n < x.size(), in double.
static std::vector<f64> directConv(const std::vector<f32>& x, const std::vector<f32>& h) {
    std::vector<f64> y(x.size(), 0.0);
    for (usize n = 0; n < x.size(); ++n) {
        f64 s = 0.0;
        const usize kmax = std::min(n + 1, h.size());
        for (usize k = 0; k < kmax; ++k) s += static_cast<f64>(h[k]) * static_cast<f64>(x[n - k]);
        y[n] = s;
    }
    return y;
}

static audio::SoundData mono(const std::vector<f32>& s, u32 rate = kRate) {
    audio::SoundData d;
    d.channels = 1;
    d.sampleRate = rate;
    d.samples = s;
    return d;
}

static audio::SoundData sine(u32 frames, f32 hz, f32 amp = 0.5f) {
    audio::SoundData d;
    d.channels = 1;
    d.sampleRate = kRate;
    d.samples.resize(frames);
    for (u32 i = 0; i < frames; ++i) d.samples[i] = amp * std::sin(6.28318530718f * hz * static_cast<f32>(i) / static_cast<f32>(kRate));
    return d;
}

static audio::SoundData impulseAt(u32 frames, u32 at, f32 value) {
    audio::SoundData d;
    d.channels = 1;
    d.sampleRate = kRate;
    d.samples.assign(frames, 0.0f);
    d.samples[at] = value;
    return d;
}

struct Stereo { std::vector<f32> l, r; };

// Runs a convolver over padded stereo input in calls of `chunk` frames.
static Stereo runConv(audio::Convolver& c, const std::vector<f32>& xl, const std::vector<f32>& xr, u32 chunk) {
    Stereo y;
    y.l.assign(xl.size(), 0.0f);
    y.r.assign(xl.size(), 0.0f);
    for (usize i = 0; i < xl.size(); i += chunk) {
        const u32 n = static_cast<u32>(std::min<usize>(chunk, xl.size() - i));
        c.process(xl.data() + i, xr.data() + i, y.l.data() + i, y.r.data() + i, n);
    }
    return y;
}

static std::vector<f32> padded(std::vector<f32> v, u32 extra) { v.resize(v.size() + extra, 0.0f); return v; }

// The largest |y[n + latency] - ref[n]| over n < ref.size(), and the peak of ref.
static void compareLatent(const std::vector<f32>& y, const std::vector<f64>& ref, u32 latency, f64& maxErr, f64& peak) {
    maxErr = 0.0;
    peak = 0.0;
    for (usize n = 0; n < ref.size(); ++n) {
        maxErr = std::max(maxErr, std::fabs(static_cast<f64>(y[n + latency]) - ref[n]));
        peak = std::max(peak, std::fabs(ref[n]));
    }
}

// Mixes `blocks` blocks of `frames` frames, appending the mixer's interleaved output.
static void mixBlocks(audio::Mixer& m, std::vector<f32>& out, u32 blocks, u32 frames) {
    const usize ch = m.channels();
    for (u32 b = 0; b < blocks; ++b) {
        const usize base = out.size();
        out.resize(base + frames * ch);
        m.mix(out.data() + base, frames);
    }
}

// ---------------------------------------------------------------- the FFT and the convolver

static void testFft() {
    AVER_INFO("=== FFT ===");
    for (u32 n : {2u, 8u, 64u, 512u}) {
        audio::Fft fft(n);
        std::vector<f32> re = noise(n, 11), im = noise(n, 12);
        const std::vector<f32> re0 = re, im0 = im;
        // The naive DFT in double.
        f64 worst = 0.0, peak = 0.0;
        std::vector<f32> fr = re, fi = im;
        fft.forward(fr.data(), fi.data());
        for (u32 k = 0; k < n; ++k) {
            f64 sr = 0.0, si = 0.0;
            for (u32 t = 0; t < n; ++t) {
                const f64 a = -6.283185307179586 * static_cast<f64>(k) * static_cast<f64>(t) / static_cast<f64>(n);
                sr += re0[t] * std::cos(a) - im0[t] * std::sin(a);
                si += re0[t] * std::sin(a) + im0[t] * std::cos(a);
            }
            worst = std::max(worst, std::max(std::fabs(sr - fr[k]), std::fabs(si - fi[k])));
            peak = std::max(peak, std::max(std::fabs(sr), std::fabs(si)));
        }
        check(worst < 1.0e-5 * std::max(1.0, peak), "size " + std::to_string(n) + ": forward matches the naive DFT (max error " + std::to_string(worst) + ")");
        fft.inverse(fr.data(), fi.data());
        f64 rt = 0.0;
        for (u32 t = 0; t < n; ++t) rt = std::max(rt, std::max<f64>(std::fabs(fr[t] - re0[t]), std::fabs(fi[t] - im0[t])));
        check(rt < 1.0e-5, "size " + std::to_string(n) + ": inverse(forward(x)) returns x");
    }
}

static void testConvolverExact() {
    AVER_INFO("=== convolver against direct convolution ===");
    const u32 N = 6000;
    const std::vector<f32> xl = noise(N, 1), xr = noise(N, 2);
    const std::vector<f32> pl = padded(xl, kBlock), pr = padded(xr, kBlock);
    for (u32 len : {1u, 100u, 255u, 256u, 257u, 512u, 700u, 4000u}) {
        const std::vector<f32> hl = noise(len, 3, 0.5f), hr = noise(len, 4, 0.5f);
        std::vector<f32> ir(static_cast<usize>(len) * 2);
        for (u32 i = 0; i < len; ++i) { ir[2 * i] = hl[i]; ir[2 * i + 1] = hr[i]; }
        const std::vector<f64> refL = directConv(xl, hl), refR = directConv(xr, hr);

        audio::Convolver c(kRate);
        check(c.setImpulse(ir.data(), len, 2, kRate), "a " + std::to_string(len) + "-frame stereo impulse loads");
        check(c.partitions() == (len + kBlock - 1) / kBlock, "and takes ceil(len / block) partitions");
        const Stereo y = runConv(c, pl, pr, 512);
        f64 eL, pkL, eR, pkR;
        compareLatent(y.l, refL, kBlock, eL, pkL);
        compareLatent(y.r, refR, kBlock, eR, pkR);
        const f64 tol = 1.0e-5 * std::max(1.0, std::max(pkL, pkR));
        check(eL < tol && eR < tol, "len " + std::to_string(len) + ": both channels match the time-domain sum (max error " + std::to_string(std::max(eL, eR)) +
                                    " on a peak of " + std::to_string(std::max(pkL, pkR)) + ")");
        bool silentHead = true;
        for (u32 i = 0; i < kBlock; ++i) silentHead = silentHead && y.l[i] == 0.0f && y.r[i] == 0.0f;
        check(silentHead, "len " + std::to_string(len) + ": the first block of output is exactly silent (latency = one partition)");
    }

    // A mono impulse feeds both ears.
    {
        const std::vector<f32> h = noise(900, 5, 0.5f);
        audio::Convolver c(kRate);
        c.setImpulse(h.data(), 900, 1, kRate);
        const Stereo y = runConv(c, pl, pr, 300);
        f64 eL, pkL, eR, pkR;
        compareLatent(y.l, directConv(xl, h), kBlock, eL, pkL);
        compareLatent(y.r, directConv(xr, h), kBlock, eR, pkR);
        check(eL < 1.0e-5 * std::max(1.0, pkL) && eR < 1.0e-5 * std::max(1.0, pkR), "a one-channel impulse is applied to both channels");
    }

    // Every chunking gives the same samples, bit for bit.
    {
        const std::vector<f32> hl = noise(1500, 6, 0.5f), hr = noise(1500, 7, 0.5f);
        std::vector<f32> ir(3000);
        for (u32 i = 0; i < 1500; ++i) { ir[2 * i] = hl[i]; ir[2 * i + 1] = hr[i]; }
        audio::Convolver whole(kRate);
        whole.setImpulse(ir.data(), 1500, 2, kRate);
        const Stereo ref = runConv(whole, pl, pr, static_cast<u32>(pl.size()));
        bool same = true;
        for (u32 chunk : {1u, 7u, 100u, 255u, 256u, 257u, 1000u}) {
            audio::Convolver c(kRate);
            c.setImpulse(ir.data(), 1500, 2, kRate);
            const Stereo y = runConv(c, pl, pr, chunk);
            same = same && y.l == ref.l && y.r == ref.r;
        }
        check(same, "chunks of 1, 7, 100, 255, 256, 257 and 1000 frames render the same samples as one call");
    }

    // Partition boundaries: impulses in the impulse response and in the input straddling multiples of the block.
    {
        std::vector<f32> h(3 * kBlock + 5, 0.0f);
        const u32 ks[] = {0, kBlock - 1, kBlock, kBlock + 1, 2 * kBlock - 1, 2 * kBlock, 3 * kBlock + 4};
        for (u32 i = 0; i < 7; ++i) h[ks[i]] = 0.25f * static_cast<f32>(i + 1);
        std::vector<f32> x(5 * kBlock, 0.0f);
        const u32 ns[] = {0, kBlock - 1, kBlock, 2 * kBlock + 1, 3 * kBlock - 1, 4 * kBlock};
        for (u32 i = 0; i < 6; ++i) x[ns[i]] = 1.0f - 0.1f * static_cast<f32>(i);
        const std::vector<f32> px = padded(x, 4 * kBlock);
        audio::Convolver c(kRate);
        c.setImpulse(h.data(), static_cast<u32>(h.size()), 1, kRate);
        const Stereo y = runConv(c, px, px, 100);
        f64 e, pk;
        std::vector<f64> ref = directConv(px, h);
        ref.resize(x.size() + 3 * kBlock);                 // compare the part for which the padded input is complete
        compareLatent(y.l, ref, kBlock, e, pk);
        check(e < 1.0e-5, "impulses on every partition boundary of the input and the impulse response land exactly (max error " + std::to_string(e) + ")");
    }

    // Predelay moves the response by that many frames.
    {
        audio::Convolver c(kRate);
        const audio::SoundData ir = impulseAt(10, 3, 0.5f);
        c.setImpulse(ir);
        c.setPredelayFrames(480);
        std::vector<f32> x(4000, 0.0f);
        x[10] = 1.0f;
        const Stereo y = runConv(c, x, x, 256);
        usize at = 0;
        for (usize i = 0; i < y.l.size(); ++i) if (std::fabs(y.l[i]) > 1.0e-4f) { at = i; break; }
        check(at == 10 + 3 + kBlock + 480 && near(y.l[at], 0.5f, 1.0e-5f), "predelay 480 + impulse offset 3 + latency 256: the pulse arrives at 10 + 3 + 256 + 480");
        check(c.latencyFrames() == kBlock && c.blockFrames() == kBlock, "the latency is one partition");
    }
}

static void testConvolverEdges() {
    AVER_INFO("=== convolver: swaps, silence, hostile input, rates ===");
    // Swapping the impulse crossfades over one block, with no click.
    {
        audio::Convolver c(kRate);
        c.setImpulse(mono({1.0f}));
        const std::vector<f32> x = noise(8 * kBlock, 21, 0.9f);
        std::vector<f32> y(x.size());
        c.process(x.data(), x.data(), y.data(), y.data(), 3 * kBlock + 40);
        c.setImpulse(mono({2.0f}));
        c.process(x.data() + 3 * kBlock + 40, x.data() + 3 * kBlock + 40, y.data() + 3 * kBlock + 40, y.data() + 3 * kBlock + 40, static_cast<u32>(x.size()) - (3 * kBlock + 40));
        std::vector<f32> g;                         // the gain applied at each output frame
        for (usize n = kBlock; n < x.size(); ++n) if (std::fabs(x[n - kBlock]) > 0.05f) g.push_back(y[n] / x[n - kBlock]);
        bool bounded = true, nondecreasing = true;
        for (usize i = 0; i < g.size(); ++i) {
            bounded = bounded && g[i] > 0.999f && g[i] < 2.001f;
            if (i > 0 && g[i] < g[i - 1] - 1.0e-3f) nondecreasing = false;
        }
        usize transition = 0;
        for (const f32 v : g) if (v > 1.001f && v < 1.999f) ++transition;
        check(bounded && nondecreasing, "the gain goes 1 -> 2 monotonically, never overshooting");
        check(!g.empty() && near(g.front(), 1.0f, 1.0e-4f) && near(g.back(), 2.0f, 1.0e-4f), "and settles on the new impulse exactly");
        check(transition > 20 && transition <= kBlock, "over at most one block (" + std::to_string(transition) + " audible steps), not in one step");
        c.collect();
    }

    // The tail ends in exact silence, and the convolver wakes up correctly afterwards.
    {
        const std::vector<f32> h = noise(2000, 31, 0.5f);
        audio::Convolver c(kRate);
        c.setImpulse(h.data(), 2000, 1, kRate);
        std::vector<f32> x(30000, 0.0f);
        x[0] = 1.0f;
        x[20000] = 0.7f; x[20001] = -0.3f;
        const Stereo y = runConv(c, x, x, 480);
        bool dead = true;
        // Up to the start of the output block that holds the response to the late input (its block is computed in full: 20000 / 256 = 78 -> 20224).
        for (usize n = 14000; n < 78 * kBlock + kBlock; ++n) dead = dead && y.l[n] == 0.0f;
        check(dead, "after the impulse response has played out the output is exactly zero");
        std::vector<f64> ref = directConv(x, h);
        f64 e, pk;
        compareLatent(y.l, std::vector<f64>(ref.begin(), ref.begin() + 29000), kBlock, e, pk);
        check(e < 1.0e-5, "and an input after a long silence convolves correctly (max error " + std::to_string(e) + ")");
    }

    // Hostile and degenerate input.
    {
        audio::Convolver c(kRate);
        const f32 one = 1.0f;
        check(!c.setImpulse(nullptr, 10, 1, kRate) && !c.setImpulse(&one, 0, 1, kRate) && !c.setImpulse(&one, 1, 0, kRate) && !c.setImpulse(&one, 1, 1, 0),
              "no data, no frames, no channels or no rate are refused");
        check(!c.hasImpulse(), "a refused impulse leaves none loaded");
        std::vector<f32> bad(500, 0.25f);
        bad[3] = std::nanf(""); bad[4] = INFINITY; bad[5] = -INFINITY;
        check(c.setImpulse(bad.data(), 500, 1, kRate), "an impulse with NaN and infinities loads");
        const std::vector<f32> x = noise(3000, 41);
        const Stereo y = runConv(c, padded(x, kBlock), padded(x, kBlock), 256);
        bool finite = true;
        for (const f32 v : y.l) finite = finite && std::isfinite(v);
        check(finite, "and its output is finite (the bad samples became zeros)");
        std::vector<f32> hz = bad;
        hz[3] = hz[4] = hz[5] = 0.0f;
        audio::Convolver c2(kRate);
        c2.setImpulse(hz.data(), 500, 1, kRate);
        const Stereo y2 = runConv(c2, padded(x, kBlock), padded(x, kBlock), 256);
        check(y.l == y2.l, "exactly as if they had been zero");

        std::vector<f32> big(static_cast<usize>(13 * kRate), 0.0f);
        big[0] = 1.0f;
        audio::Convolver c3(kRate);
        check(c3.setImpulse(big.data(), static_cast<u32>(big.size()), 1, kRate) && c3.impulseFrames() == static_cast<u32>(audio::Convolver::kMaxImpulseSeconds * kRate),
              "a 13 s impulse is truncated to the 12 s cap");
        std::vector<f32> imp(2000, 0.0f);
        imp[0] = 1.0f;
        const Stereo y3 = runConv(c3, imp, imp, 500);
        check(near(y3.l[kBlock], 1.0f, 1.0e-5f), "and still plays (the truncated impulse's first sample at the latency)");

        audio::Convolver c4(kRate);
        c4.setImpulse(mono({1.0f}));
        c4.clearImpulse();
        check(!c4.hasImpulse(), "clearImpulse leaves no impulse");
        const Stereo y4 = runConv(c4, imp, imp, 500);
        bool silent = true;
        for (usize n = 2 * kBlock; n < y4.l.size(); ++n) silent = silent && y4.l[n] == 0.0f;
        check(silent, "and the wet output fades to silence");
    }

    // An impulse recorded at another rate is resampled to the convolver's.
    {
        audio::Convolver c(kRate);
        audio::SoundData ir;
        ir.channels = 1; ir.sampleRate = 24000; ir.samples.assign(1000, 1.0f);
        c.setImpulse(ir);
        check(c.impulseFrames() == 2000, "1000 frames at 24 kHz become 2000 at 48 kHz");
        std::vector<f32> imp(4000, 0.0f);
        imp[0] = 1.0f;
        const Stereo y = runConv(c, imp, imp, 500);
        bool ones = true;
        for (usize n = kBlock; n < kBlock + 2000; ++n) ones = ones && near(y.l[n], 1.0f, 1.0e-4f);
        check(ones && std::fabs(y.l[kBlock + 2000]) < 1.0e-4f, "a constant stays constant and ends where the resampled impulse ends");

        // The same convolver rebuilt for a new device rate keeps its source.
        c.setSampleRate(44100);
        check(c.sampleRate() == 44100 && c.hasImpulse() && (c.impulseFrames() == 1837 || c.impulseFrames() == 1838),
              "a device rate change rebuilds the impulse (1000 @ 24k -> ~1837 @ 44.1k): " + std::to_string(c.impulseFrames()));
    }

#if AVER_TEST_HAS_FORMATS
    // The impulse comes from a sound asset: written and read back through the engine's .ocaudio container.
    {
        audio::SoundData ir = mono(noise(3000, 51, 0.4f), 44100);
        std::vector<u8> bytes;
        audio::SoundData back;
        std::string why;
        const bool wrote = fmt::writeOcAudio(ir, "ir", bytes, &why);
        const bool read = wrote && fmt::parseOcAudio(bytes.data(), bytes.size(), back, &why);
        check(read && back.sampleRate == 44100 && back.frames() == 3000, "an impulse response round-trips through .ocaudio (" + why + ")");
        audio::Convolver a(kRate), b(kRate);
        a.setImpulse(ir);
        b.setImpulse(back);
        const std::vector<f32> x = padded(noise(4000, 52), kBlock);
        check(runConv(a, x, x, 256).l == runConv(b, x, x, 256).l, "and convolves identically to the original");
    }
#endif
}

static void testConvolverCost() {
    AVER_INFO("=== convolver cost (2 s stereo impulse at 48 kHz) ===");
    const u32 len = 2 * kRate;
    std::vector<f32> ir(static_cast<usize>(len) * 2);
    for (u32 i = 0; i < len; ++i) {
        const f32 env = std::exp(-3.0f * static_cast<f32>(i) / static_cast<f32>(len)) * 0.2f;
        const std::vector<f32> n = noise(2, i + 1);
        ir[2 * i] = env * n[0]; ir[2 * i + 1] = env * n[1];
    }
    audio::Convolver c(kRate);
    const bool loaded = c.setImpulse(ir.data(), len, 2, kRate);
    check(loaded && c.partitions() == (len + kBlock - 1) / kBlock,
          "a 2 s stereo impulse is " + std::to_string(c.partitions()) + " partitions of " + std::to_string(kBlock) + " frames");
    const std::vector<f32> in = noise(kBlock * 5, 61, 0.5f);
    std::vector<f32> outL(kBlock * 4), outR(kBlock * 4);
    for (int i = 0; i < 40; ++i) c.process(in.data(), in.data() + 0, outL.data(), outR.data(), kBlock);       // warm up

    const int blocks = 300;
    std::vector<f64> ms(blocks);
    f64 sum = 0.0;
    for (int b = 0; b < blocks; ++b) {
        const auto t0 = std::chrono::steady_clock::now();
        c.process(in.data(), in.data() + 1, outL.data(), outR.data(), kBlock);
        const auto t1 = std::chrono::steady_clock::now();
        ms[static_cast<usize>(b)] = std::chrono::duration<f64, std::milli>(t1 - t0).count();
        sum += ms[static_cast<usize>(b)];
    }
    std::sort(ms.begin(), ms.end());
    const f64 budget = 1000.0 * kBlock / kRate;
    AVER_INFO("  cost  {} frames per block: mean {:.3f} ms, median {:.3f} ms, p99 {:.3f} ms, max {:.3f} ms of a {:.3f} ms block ({:.1f}% of real time on one core)",
              kBlock, sum / blocks, ms[blocks / 2], ms[blocks * 99 / 100], ms.back(), budget, 100.0 * (sum / blocks) / budget);

    // Four partitions in one call, as a larger device buffer would ask for.
    const int calls = 80;
    f64 sum4 = 0.0;
    for (int b = 0; b < calls; ++b) {
        const auto t0 = std::chrono::steady_clock::now();
        c.process(in.data(), in.data() + 1, outL.data(), outR.data(), kBlock * 4);
        sum4 += std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    AVER_INFO("  cost  {} frames per call: mean {:.3f} ms of a {:.3f} ms buffer", kBlock * 4, sum4 / calls, budget * 4);
    bool finite = true;
    for (usize i = 0; i < outL.size(); ++i) finite = finite && std::isfinite(outL[i]) && std::isfinite(outR[i]);
    check(finite, "the output stays finite");
    // A debug build is several times slower than the figure that matters; this only catches a convolver that has become absurd.
    check(sum / blocks < budget * 6.0, "the mean block time is within 6x of real time even unoptimised (an optimised build is the figure above)");
}

// ---------------------------------------------------------------- the convolution reverb effect

static void testConvolutionEffect() {
    AVER_INFO("=== convolution reverb effect ===");
    using audio::Effect;
    using audio::EffectKind;
    auto fx = Effect::create(EffectKind::ConvolutionReverb, kRate);
    check(fx && fx->kind() == EffectKind::ConvolutionReverb && std::string(Effect::godotClassName(EffectKind::ConvolutionReverb)) == "AudioEffectConvolutionReverb",
          "the effect exists and has a class name");
    check(near(fx->paramByName("dry"), 1.0f) && near(fx->paramByName("wet"), 0.5f) && near(fx->paramByName("predelay_msec"), 0.0f) && near(fx->paramByName("gain_db"), 0.0f),
          "its defaults are dry 1, wet 0.5, no predelay, 0 dB");
    check(!fx->hasImpulse() && fx->latencyFrames() == kBlock, "no impulse until one is set; the wet latency is one partition");

    const u32 N = 5000;
    std::vector<f32> io = noise(2 * N, 71);
    std::vector<f32> x = io;
    fx->process(io.data(), N, nullptr);
    check(io == x, "with no impulse loaded the effect is the dry signal, bit for bit");

    fx->setImpulse(impulseAt(10, 0, 0.5f));
    fx->setParamByName("wet", 1.0f);
    io = x;
    fx->reset();
    fx->process(io.data(), 3000, nullptr);                  // longer than the effect's internal chunk, split across two calls
    fx->process(io.data() + 2 * 3000, N - 3000, nullptr);
    f64 worst = 0.0;
    for (u32 n = 0; n < N; ++n)
        for (u32 c = 0; c < 2; ++c) {
            const f32 want = x[2 * n + c] + (n >= kBlock ? 0.5f * x[2 * (n - kBlock) + c] : 0.0f);
            worst = std::max(worst, static_cast<f64>(std::fabs(io[2 * n + c] - want)));
        }
    check(worst < 1.0e-5, "dry 1, wet 1 with a 0.5 impulse: out[n] = in[n] + 0.5 in[n - 256] (max error " + std::to_string(worst) + ")");

    fx->setParamByName("dry", 0.0f);
    fx->setParamByName("gain_db", 6.0206f);
    fx->setParamByName("predelay_msec", 10.0f);
    io = x;
    fx->reset();
    fx->process(io.data(), N, nullptr);
    worst = 0.0;
    for (u32 n = 0; n < N; ++n) {
        const f32 want = n >= kBlock + 480 ? 1.0f * x[2 * (n - kBlock - 480)] : 0.0f;       // 0.5 * 2 (6.02 dB)
        worst = std::max(worst, static_cast<f64>(std::fabs(io[2 * n] - want)));
    }
    check(worst < 1.0e-4, "dry 0, +6.02 dB and 10 ms predelay: the wet signal alone, doubled and 480 frames later (max error " + std::to_string(worst) + ")");

    // A device rate change rebuilds the impulse for the new rate.
    audio::BusGraph g;
    g.init(kRate);
    const u32 id = g.addEffect(0, EffectKind::ConvolutionReverb);
    Effect* e = g.effect(id);
    e->setImpulse(impulseAt(500, 480, 1.0f));
    e->setParamByName("wet", 1.0f);
    e->setParamByName("dry", 0.0f);
    e->setParamByName("predelay_msec", 7.0f);
    g.setSampleRate(24000);
    e = g.effect(id);
    check(e && e->sampleRate() == 24000 && e->hasImpulse() && near(e->paramByName("predelay_msec"), 7.0f),
          "BusGraph::setSampleRate keeps the impulse and the parameters");
    std::vector<f32> imp(2 * 3000, 0.0f);
    imp[0] = 1.0f; imp[1] = 1.0f;
    e->process(imp.data(), 3000, nullptr);
    usize at = 0;
    for (usize n = 0; n < 3000; ++n) if (std::fabs(imp[2 * n]) > 0.5f) { at = n; break; }
    check(at == 240 + kBlock + 168, "the 480-frame impulse offset at 48 kHz is 240 at 24 kHz (plus the partition and 7 ms of predelay = 168 frames): " + std::to_string(at));
    g.shutdown();

    // Through the mixer: a sound is the impulse, on a graph bus.
    audio::Mixer m;
    m.init(kRate, 2, 8, 8, 2);
    m.graph().setActive(true);
    const u32 eid = m.graph().addEffect(0, EffectKind::ConvolutionReverb);
    const audio::SoundHandle irSnd = m.addSound(impulseAt(900, 600, 0.5f));
    check(eid != 0 && irSnd != 0 && !m.setEffectImpulse(eid, 0) && !m.setEffectImpulse(9999, irSnd) && m.setEffectImpulse(eid, irSnd),
          "Mixer::setEffectImpulse takes a loaded sound and refuses an unknown effect or sound");
    m.graph().effect(eid)->setParamByName("wet", 1.0f);
    audio::PlayDesc d;
    d.sound = m.addSound(impulseAt(10, 0, 1.0f));
    d.graphBus = 0;
    m.play(d);
    std::vector<f32> out;
    mixBlocks(m, out, 4, 480);
    usize first = 0;
    for (usize n = 1; n < out.size() / 2; ++n) if (std::fabs(out[2 * n]) > 0.05f) { first = n; break; }
    check(first == 600 + kBlock && near(out[2 * first], 0.5f, 1.0e-3f) && near(out[0], 1.0f, 1.0e-3f),
          "a sound played on the bus is heard dry and again through the impulse (at 600 + 256), at its level");
    m.removeSound(irSnd);
    m.collect();
    mixBlocks(m, out, 2, 480);
    check(m.graph().effect(eid)->hasImpulse(), "the effect keeps its own copy of the impulse when the sound is unloaded");
}

// ---------------------------------------------------------------- zone send reverbs in the mixer

static void testZoneSends() {
    AVER_INFO("=== zone send reverbs ===");
    audio::ZoneReverb off;      // wet 0

    // Slots that are off, and sends into them, change nothing.
    {
        auto render = [&](bool configure) {
            audio::Mixer m;
            m.init(kRate, 2, 8, 8, 2);
            audio::ReverbParams p; p.wet = 0.4f; p.decaySec = 1.2f;
            m.setReverb(p);
            audio::PlayDesc d;
            d.sound = m.addSound(sine(9600, 440.0f));
            d.looping = true;
            const audio::VoiceHandle v = m.play(d);
            audio::PlayDesc d2;
            d2.sound = m.addSound(impulseAt(100, 0, 0.8f));
            const audio::VoiceHandle v2 = m.play(d2);
            if (configure) {
                audio::ZoneReverb z; z.reverb.wet = 0.0f; z.reverb.decaySec = 3.0f;
                for (u32 s = 0; s < audio::kMaxZoneReverbs; ++s) { m.setZoneReverb(s, z); m.setVoiceZoneSend(v, s, 1.0f); m.setVoiceZoneSend(v2, s, 0.7f); }
            }
            std::vector<f32> out;
            mixBlocks(m, out, 60, 480);
            return out;
        };
        const std::vector<f32> a = render(false), b = render(true);
        check(a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(f32)) == 0,
              "slots configured at wet 0 with every voice sending: output identical to a mixer that never heard of zones, bit for bit");
        audio::Mixer m;
        m.init(kRate, 2, 8, 8, 2);
        check(!m.setZoneReverb(audio::kMaxZoneReverbs, off) && !m.setZoneReverb(0, [] { audio::ZoneReverb z; z.impulse = 77; return z; }()),
              "a bad slot or an unknown impulse sound is refused");
    }

    // An algorithmic zone: a tail only where voices send, slots independent.
    {
        auto render = [&](u32 slotA, f32 sendA, u32 slotB, f32 sendB, f32 decayA, f32 decayB) {
            audio::Mixer m;
            m.init(kRate, 1, 8, 8, 2);
            audio::ZoneReverb za, zb;
            za.reverb.wet = 1.0f; za.reverb.decaySec = decayA;
            zb.reverb.wet = 1.0f; zb.reverb.decaySec = decayB;
            m.setZoneReverb(slotA, za);
            m.setZoneReverb(slotB, zb);
            audio::PlayDesc da, db;
            da.sound = m.addSound(impulseAt(10, 0, 0.5f));
            db.sound = m.addSound(impulseAt(10, 0, 0.5f));
            const audio::VoiceHandle va = m.play(da), vb = m.play(db);
            m.setVoiceZoneSend(va, slotA, sendA);
            m.setVoiceZoneSend(vb, slotB, sendB);
            std::vector<f32> out;
            mixBlocks(m, out, 200, 480);
            return out;
        };
        const std::vector<f32> dry = render(0, 0.0f, 1, 0.0f, 0.3f, 3.0f);
        const std::vector<f32> onlyA = render(0, 1.0f, 1, 0.0f, 0.3f, 3.0f);
        const std::vector<f32> onlyB = render(0, 0.0f, 1, 1.0f, 0.3f, 3.0f);
        const std::vector<f32> both = render(0, 1.0f, 1, 1.0f, 0.3f, 3.0f);
        auto tail = [](const std::vector<f32>& v, const std::vector<f32>& base, usize from, usize to) {
            f64 s = 0.0;
            for (usize i = from; i < to; ++i) s += static_cast<f64>(v[i] - base[i]) * (v[i] - base[i]);
            return s;
        };
        check(tail(onlyA, dry, 480, 9600) > 1.0e-6 && tail(onlyB, dry, 480, 9600) > 1.0e-6, "each slot adds a tail where a voice sends to it");
        check(tail(onlyB, dry, 48000, 52800) > 10.0 * tail(onlyA, dry, 48000, 52800), "slot B (RT60 3 s) is still ringing a second later; slot A (0.3 s) is not");
        f64 worst = 0.0;
        for (usize i = 0; i < both.size(); ++i) worst = std::max(worst, static_cast<f64>(std::fabs(both[i] - (onlyA[i] + onlyB[i] - dry[i]))));
        check(worst < 1.0e-6, "two slots add up independently: both == A + B - dry (max error " + std::to_string(worst) + ")");
        // A send to a slot nobody turned on is nothing: the mixer below only has its dry impulse.
        audio::Mixer m;
        m.init(kRate, 1, 8, 8, 2);
        audio::PlayDesc d;
        d.sound = m.addSound(impulseAt(10, 0, 0.1f));
        const audio::VoiceHandle v = m.play(d);
        m.setVoiceZoneSend(v, 1, 1.0f);
        std::vector<f32> out;
        mixBlocks(m, out, 40, 480);
        f64 e = 0.0;
        for (usize i = 10; i < out.size(); ++i) e += static_cast<f64>(out[i]) * out[i];
        check(e == 0.0, "a send to a slot that is off adds nothing");
    }

    // A convolution zone: the slot plays a sound's impulse response, delayed by the partition and the predelay.
    {
        audio::Mixer m;
        m.init(kRate, 1, 8, 8, 4);
        const audio::SoundHandle ir = m.addSound(impulseAt(1000, 700, 0.5f));
        audio::ZoneReverb z;
        z.reverb.wet = 1.0f;
        z.impulse = ir;
        z.predelayMs = 10.0f;
        check(m.setZoneReverb(1, z) && m.zoneReverb(1).impulse == ir && near(m.zoneReverb(1).predelayMs, 10.0f), "a zone takes a loaded sound as its impulse response");
        audio::PlayDesc d;
        d.sound = m.addSound(impulseAt(10, 0, 1.0f));
        const audio::VoiceHandle v = m.play(d);
        m.setVoiceZoneSend(v, 1, 1.0f);
        std::vector<f32> out;
        mixBlocks(m, out, 10, 480);
        usize at = 0;
        for (usize i = 1; i < out.size(); ++i) if (std::fabs(out[i]) > 1.0e-3f) { at = i; break; }
        check(at == 700 + kBlock + 480 && near(out[at], 0.5f, 1.0e-4f), "the pulse arrives at 700 + 256 + 480 frames with its authored level: " + std::to_string(at));
        f64 rest = 0.0;
        for (usize i = 1; i < out.size(); ++i) if (i != at) rest += static_cast<f64>(out[i]) * out[i];
        check(rest < 1.0e-8, "and nothing else is heard");

        m.removeSound(ir);
        m.collect();
        check(m.zoneReverb(1).impulse == ir, "unloading the sound leaves the slot playing its own copy");
        z.impulse = 0;
        m.setZoneReverb(1, z);
        check(m.zoneReverb(1).impulse == 0, "impulse 0 returns the slot to the algorithmic reverb");
    }

    // A send changing mid-stream ramps over exactly one block, sample for sample.
    {
        const u32 kChunk = 480;
        auto render = [&](u32 stepAtBlock) {
            audio::Mixer m;
            m.init(kRate, 1, 8, 8, 2);
            audio::ZoneReverb z;
            z.reverb.wet = 1.0f;
            z.impulse = m.addSound(impulseAt(1, 0, 1.0f));
            m.setZoneReverb(0, z);
            audio::PlayDesc d;
            d.sound = m.addSound(sine(4800, 300.0f));
            d.looping = true;
            const audio::VoiceHandle v = m.play(d);
            std::vector<f32> out;
            for (u32 b = 0; b < 30; ++b) {
                if (stepAtBlock != 0xFFFFFFFFu) m.setVoiceZoneSend(v, 0, b >= stepAtBlock ? 1.0f : 0.0f);
                else m.setVoiceZoneSend(v, 0, 1.0f);
                mixBlocks(m, out, 1, kChunk);
            }
            return out;
        };
        const std::vector<f32> full = render(0xFFFFFFFFu), step = render(10);
        audio::Mixer dryM;
        dryM.init(kRate, 1, 8, 8, 2);
        audio::PlayDesc d;
        d.sound = dryM.addSound(sine(4800, 300.0f));
        d.looping = true;
        dryM.play(d);
        std::vector<f32> dry;
        mixBlocks(dryM, dry, 30, kChunk);
        f64 worst = 0.0;
        for (usize n = 0; n < full.size(); ++n) {
            // Sent at frame m = n - 256 with gain g(m): 0 before block 10, a linear ramp through it, 1 after.
            f64 g = 0.0;
            if (n >= kBlock) {
                const i64 m = static_cast<i64>(n) - kBlock;
                if (m >= 10 * static_cast<i64>(kChunk) + static_cast<i64>(kChunk)) g = 1.0;
                else if (m >= 10 * static_cast<i64>(kChunk)) g = static_cast<f64>(m - 10 * kChunk + 1) / kChunk;
            }
            const f64 wetFull = static_cast<f64>(full[n]) - dry[n];
            worst = std::max(worst, std::fabs((static_cast<f64>(step[n]) - dry[n]) - g * wetFull));
        }
        check(worst < 1.0e-5, "a send stepped from 0 to 1 is a linear one-block ramp at the send, exactly (max error " + std::to_string(worst) + ")");
    }

    // A slot that has been turned off restarts from silence, as a fresh mixer would.
    {
        auto burst = [&](audio::Mixer& m) {
            audio::PlayDesc d;
            d.sound = m.addSound(impulseAt(10, 0, 0.5f));
            const audio::VoiceHandle v = m.play(d);
            m.setVoiceZoneSend(v, 2, 1.0f);
        };
        audio::ZoneReverb z;
        z.reverb.wet = 0.8f; z.reverb.decaySec = 2.0f; z.reverb.damping = 0.5f;
        audio::Mixer a;
        a.init(kRate, 2, 8, 16, 2);
        a.setZoneReverb(2, z);
        burst(a);
        std::vector<f32> sink;
        mixBlocks(a, sink, 100, 480);
        audio::ZoneReverb zoff = z; zoff.reverb.wet = 0.0f;
        a.setZoneReverb(2, zoff);
        mixBlocks(a, sink, 4, 480);
        a.setZoneReverb(2, z);
        burst(a);
        std::vector<f32> again;
        mixBlocks(a, again, 100, 480);

        audio::Mixer b;
        b.init(kRate, 2, 8, 16, 2);
        b.setZoneReverb(2, z);
        burst(b);
        std::vector<f32> fresh;
        mixBlocks(b, fresh, 100, 480);
        check(again == fresh, "after wet 0 the old tail is gone: the same burst rings out exactly as on a fresh mixer");
    }

    // A device rate change keeps the zone's impulse, rebuilt for the new rate.
    {
        audio::Mixer m;
        m.init(kRate, 2, 8, 8, 2);
        audio::ZoneReverb z;
        z.reverb.wet = 1.0f;
        z.impulse = m.addSound(impulseAt(4800, 4799, 1.0f));
        m.setZoneReverb(3, z);
        check(m.setOutputFormat(44100, 2) && m.zoneReverb(3).impulse == z.impulse, "setOutputFormat keeps the zone slot");
        audio::PlayDesc d;
        d.sound = m.addSound(impulseAt(10, 0, 1.0f));
        const audio::VoiceHandle v = m.play(d);
        m.setVoiceZoneSend(v, 3, 1.0f);
        std::vector<f32> out;
        mixBlocks(m, out, 20, 441);
        usize at = 0;
        for (usize i = 1; i < out.size() / 2; ++i) if (std::fabs(out[2 * i]) > 0.01f) { at = i; break; }
        // 4799 frames at 48 kHz is frame 4408.5 at 44.1 kHz: linear interpolation puts half the pulse on each neighbour.
        // 4800 frames at 48 kHz are 4410 at 44.1 kHz; the single non-zero source frame (the last) lands on the last one, 4409.
        check(at >= 4408 + kBlock && at <= 4410 + kBlock, "and the impulse is rebuilt at the new rate (pulse at 4409 + 256): " + std::to_string(at));
    }

    // Nothing in mix() allocates with every part of this running, including an impulse swap picked up on the audio thread.
    {
        audio::Mixer m;
        m.init(kRate, 2, 16, 16, 2);
        m.graph().setActive(true);
        const u32 eid = m.graph().addEffect(0, audio::EffectKind::ConvolutionReverb);
        const audio::SoundHandle irA = m.addSound(mono(noise(5000, 81, 0.2f))), irB = m.addSound(mono(noise(7000, 82, 0.2f)));
        m.setEffectImpulse(eid, irA);
        audio::ZoneReverb z0, z1;
        z0.reverb.wet = 0.7f; z0.impulse = irA;
        z1.reverb.wet = 0.5f; z1.reverb.decaySec = 2.0f;
        m.setZoneReverb(0, z0);
        m.setZoneReverb(1, z1);
        std::vector<audio::VoiceHandle> vs;
        for (int i = 0; i < 6; ++i) {
            audio::PlayDesc d;
            d.sound = m.addSound(sine(9600, 200.0f + 90.0f * static_cast<f32>(i)));
            d.looping = true;
            d.positional = (i & 1) != 0;
            d.position[0] = 300.0f; d.position[1] = 200.0f * static_cast<f32>(i) - 400.0f;
            if (i % 3 == 0) d.graphBus = 0;
            vs.push_back(m.play(d));
        }
        std::vector<f32> out(960 * 2);
        g_allocs = 0; g_counting = true;
        { std::vector<int> control(4); (void)control; }
        g_counting = false;
        check(g_allocs.load() >= 1, "the counting hook sees an ordinary allocation (control)");

        z0.impulse = irB;
        m.setZoneReverb(0, z0);                        // a new impulse waits for the audio thread
        m.setEffectImpulse(eid, irB);
        g_allocs = 0; g_counting = true;
        for (int b = 0; b < 120; ++b) {
            for (usize i = 0; i < vs.size(); ++i) m.setVoiceZoneSend(vs[i], static_cast<u32>(i & 1), 0.5f + 0.5f * std::sin(0.05f * static_cast<f32>(b + static_cast<int>(i))));
            m.mix(out.data(), 960);
            if (b == 60) { /* nothing to do on the audio side: the swap below was already published */ }
        }
        g_counting = false;
        check(g_allocs.load() == 0, "120 blocks with two zone slots (one convolving), a convolution effect, an impulse swap picked up and changing sends: " + std::to_string(g_allocs.load()) + " allocations in mix()");
        bool finite = true;
        for (const f32 v : out) finite = finite && std::isfinite(v);
        check(finite, "and the output is finite");
        m.collect();
    }
}

// ---------------------------------------------------------------- diffraction

// A wall in the plane x = wx covering |y - cy| <= half and |z| <= 1e6, with an optional gap (open interval) in y. Blocks a segment that crosses
// the plane inside the wall.
struct Wall {
    f32 wx = 500.0f, half = 400.0f, gapLo = 0.0f, gapHi = 0.0f;
    int calls = 0;
};
static f32 wallRay(void* user, const f32 from[3], const f32 to[3]) {
    Wall& w = *static_cast<Wall*>(user);
    ++w.calls;
    const f32 a = from[0] - w.wx, b = to[0] - w.wx;
    if (a * b > 0.0f || a == b) return 0.0f;                           // wholly on one side (touching the plane counts as crossing it)
    const f32 t = a / (a - b);
    const f32 y = from[1] + (to[1] - from[1]) * t;
    if (w.gapHi > w.gapLo && y > w.gapLo && y < w.gapHi) return 0.0f;  // through the opening
    return std::fabs(y) <= w.half ? 1.0f : 0.0f;
}

static void testDiffraction() {
    AVER_INFO("=== diffraction ===");
    const f32 lis[3] = {0, 0, 0}, src[3] = {1000, 0, 0};
    audio::OcclusionProbe direct;
    audio::DiffractionProbe probe;
    const audio::OcclusionCurve curve;

    {
        Wall w;
        w.half = 100.0f;
        check(near(audio::measureOcclusion(wallRay, &w, lis, src, direct), 1.0f), "setup: the wall blocks the direct path (all five probe rays)");
        const audio::DiffractionPath p = audio::findDiffractionPath(wallRay, &w, lis, src, probe);
        check(p.found && p.occlusion < 1.0f && p.occlusion > 0.0f && p.raysUsed <= probe.maxRays, "a bent path around the edge is found within the ray budget (" + std::to_string(p.raysUsed) + " rays)");
        check(near(p.directLengthCm, 1000.0f, 1.0e-2f) && p.pathLengthCm > p.directLengthCm && near(p.detourCm, p.pathLengthCm - p.directLengthCm, 1.0e-2f),
              "its length is longer than the line of sight");
        check(std::fabs(std::fabs(p.waypoint[1]) - 100.0f) < 30.0f && near(p.waypoint[0], 500.0f, 1.0f), "and the waypoint sits just past the wall's edge (y = " + std::to_string(p.waypoint[1]) + ")");
    }

    // Gain falls as the blocker grows; a bent path is always better than the fully occluded value.
    {
        f32 prevGain = 2.0f, prevDetour = -1.0f, prevOcc = -1.0f, firstGain = 0.0f, lastGain = 0.0f;
        bool mono_ = true, better = true, foundAll = true, detourOk = true;
        for (f32 half : {50.0f, 100.0f, 200.0f, 400.0f, 800.0f, 1600.0f}) {
            Wall w;
            w.half = half;
            const audio::DiffractionPath p = audio::findDiffractionPath(wallRay, &w, lis, src, probe);
            foundAll = foundAll && p.found;
            const f32 occ = audio::applyDiffraction(1.0f, p);
            const f32 gain = audio::occlusionGain(occ, curve);
            if (gain > prevGain + 1.0e-6f || p.detourCm < prevDetour - 1.0e-3f || occ < prevOcc - 1.0e-6f) mono_ = false;
            better = better && gain > audio::occlusionGain(1.0f, curve) + 0.01f && occ < probe.maxOcclusion + 1.0e-6f;
            const f32 lo = 2.0f * std::sqrt(500.0f * 500.0f + half * half) - 1000.0f;
            const f32 hi = 2.0f * std::sqrt(500.0f * 500.0f + 1.25f * half * 1.25f * half) - 1000.0f;
            detourOk = detourOk && p.detourCm >= lo - 1.0f && p.detourCm <= hi + 1.0f;
            if (half == 50.0f) firstGain = gain;
            lastGain = gain;
            prevGain = gain; prevDetour = p.detourCm; prevOcc = occ;
        }
        check(foundAll, "a path is found around walls from 50 cm to 1600 cm half-width");
        check(mono_, "the gain only falls, and the detour and occlusion only rise, as the wall widens");
        check(lastGain < firstGain - 0.05f, "and the widest wall is clearly quieter than the narrowest (" + std::to_string(firstGain) + " -> " + std::to_string(lastGain) + ")");
        check(better, "every bent path is louder than the fully occluded value (" + std::to_string(audio::occlusionGain(1.0f, curve)) + ") and below the cap");
        check(detourOk, "the detour is the geometric one for the edge, to within the bisection's resolution");
    }

    // An opening in the wall: the path goes through it, not around the end.
    {
        Wall w;
        w.half = 3000.0f; w.gapLo = 200.0f; w.gapHi = 600.0f;
        const audio::DiffractionPath p = audio::findDiffractionPath(wallRay, &w, lis, src, probe);
        const f32 around = 2.0f * std::sqrt(500.0f * 500.0f + 3000.0f * 3000.0f) - 1000.0f;
        check(p.found && p.waypoint[1] > 200.0f && p.waypoint[1] < 600.0f && p.detourCm < 0.2f * around, "a doorway off to the side is found: detour " + std::to_string(p.detourCm) + " cm against " + std::to_string(around) + " around the end");
    }

    // Limits: an endless wall, a tiny budget, no ray function, nothing in the way.
    {
        Wall w;
        w.half = 1.0e9f;
        const audio::DiffractionPath p = audio::findDiffractionPath(wallRay, &w, lis, src, probe);
        check(!p.found && p.raysUsed <= probe.maxRays && audio::applyDiffraction(1.0f, p) == 1.0f && p.occlusion == 1.0f,
              "an endless wall has no bent path: the value stays fully occluded, and the search stops at its ray budget (" + std::to_string(p.raysUsed) + ")");
        Wall w2;
        w2.half = 800.0f;
        audio::DiffractionProbe tiny;
        tiny.maxRays = 4;
        const audio::DiffractionPath t = audio::findDiffractionPath(wallRay, &w2, lis, src, tiny);
        check(!t.found && t.raysUsed <= 4 && w2.calls <= 4, "a budget of 4 rays casts at most 4");
        check(!audio::findDiffractionPath(nullptr, nullptr, lis, src, probe).found && !audio::findDiffractionPath(wallRay, &w2, lis, lis, probe).found,
              "no ray function, or a source on top of the listener, finds nothing");
        Wall none;
        none.half = -1.0f;                                              // nothing is blocked
        direct.rayCount = 5;
        audio::DiffractionPath path;
        const f32 o = audio::measureOcclusionDiffracted(wallRay, &none, lis, src, direct, probe, &path);
        check(o == 0.0f && !path.found && none.calls == 5, "a clear line of sight reads 0 with only the five probe rays: no search is run");

        // Partially blocked: continuous, never above the direct value.
        bool cont = true, below = true;
        Wall w3;
        w3.half = 100.0f;
        const audio::DiffractionPath found = audio::findDiffractionPath(wallRay, &w3, lis, src, probe);
        f32 prev = 0.0f;
        for (int i = 0; i <= 100; ++i) {
            const f32 dOcc = static_cast<f32>(i) / 100.0f;
            const f32 r = audio::applyDiffraction(dOcc, found);
            if (r > dOcc + 1.0e-6f) below = false;
            if (r < prev - 1.0e-6f || std::fabs(r - prev) > 0.0101f) cont = false;
            prev = r;
        }
        check(below && cont, "applyDiffraction is continuous in the direct occlusion, never above it, and never falls as it rises");
    }

    // The formula's own limits.
    {
        check(audio::diffractionOcclusion(1000.0f, 1000.0f, 0.0f, 0.85f) == 0.0f, "a straight path (no detour, no bend) reads 0");
        check(near(audio::diffractionOcclusion(1000.0f, 1000.0f, 3.14159265f, 0.85f), 0.85f, 1.0e-4f), "doubling back reads the cap");
        check(audio::diffractionOcclusion(1000.0f, 2000.0f, 1.0f, 0.85f) > audio::diffractionOcclusion(1000.0f, 1500.0f, 1.0f, 0.85f) &&
              audio::diffractionOcclusion(1000.0f, 1500.0f, 1.5f, 0.85f) > audio::diffractionOcclusion(1000.0f, 1500.0f, 1.0f, 0.85f),
              "a longer path and a sharper bend each read higher");
    }

    // Through the mixer: the bent value is heard louder and brighter than a wall.
    {
        Wall w;
        w.half = 300.0f;
        const audio::DiffractionPath p = audio::findDiffractionPath(wallRay, &w, lis, src, probe);
        const f32 bent = audio::applyDiffraction(1.0f, p);
        auto rms = [&](f32 hz, f32 occlusion) {
            audio::Mixer m;
            m.init(kRate, 1, 8, 8, 2);
            audio::PlayDesc d;
            d.sound = m.addSound(sine(4800, hz));
            d.looping = true;
            const audio::VoiceHandle v = m.play(d);
            m.setVoiceOcclusion(v, occlusion);
            std::vector<f32> out;
            mixBlocks(m, out, 40, 480);
            f64 s = 0.0;
            for (usize i = 480 * 30; i < out.size(); ++i) s += static_cast<f64>(out[i]) * out[i];
            return static_cast<f32>(std::sqrt(s / static_cast<f64>(out.size() - 480 * 30)));
        };
        check(p.found && rms(300.0f, bent) > 1.15f * rms(300.0f, 1.0f), "at 300 Hz the bent path is at least 15% louder than the fully occluded wall");
        check(rms(4000.0f, bent) > 3.0f * rms(4000.0f, 1.0f), "and at 4 kHz it is several times brighter (the low-pass opens up)");
        check(rms(300.0f, bent) < rms(300.0f, 0.0f), "but still quieter than a clear path");
    }
}

// ---------------------------------------------------------------- the zone share blend

static void testZoneShares() {
    AVER_INFO("=== zone shares ===");
    f32 sh[3];
    f32 rem = 0.0f;
    {
        const f32 w[3] = {1.0f, 1.0f, 1.0f};
        const i32 p[3] = {0, 5, 2};
        audio::zoneShares(w, p, 3, sh, rem);
        check(sh[1] == 1.0f && sh[0] == 0.0f && sh[2] == 0.0f && rem == 0.0f, "the highest priority zone takes everything where it is fully present");
    }
    {
        const f32 w[3] = {0.5f, 0.5f, 0.0f};
        const i32 p[3] = {0, 0, 0};
        audio::zoneShares(w, p, 3, sh, rem);
        check(near(sh[0], 0.5f) && near(sh[1], 0.25f) && near(rem, 0.25f), "equal priorities keep input order; the rest is the remainder");
    }
    // The shares agree with the wet blend of blendReverbZones, and sum to 1 with the remainder, continuously along a sweep.
    {
        bool sum1 = true, agrees = true, smooth = true;
        f32 prev[3] = {0, 0, 0};
        for (int i = 0; i <= 200; ++i) {
            const f32 x = static_cast<f32>(i) * 5.0f;           // a listener walking from zone A (0) through the gap into zone B (1200)
            const f32 boxA[3] = {500, 500, 500}, boxB[3] = {500, 500, 500};
            const f32 pa[3] = {x, 0, 0}, pb[3] = {x - 1200.0f, 0, 0};
            const f32 w[3] = {audio::zoneWeight(audio::zoneSignedDistance(audio::ZoneShape::Box, boxA, pa), 200.0f),
                              audio::zoneWeight(audio::zoneSignedDistance(audio::ZoneShape::Box, boxB, pb), 200.0f), 0.0f};
            const i32 pr[3] = {1, 3, 0};
            audio::zoneShares(w, pr, 3, sh, rem);
            sum1 = sum1 && near(sh[0] + sh[1] + sh[2] + rem, 1.0f, 1.0e-5f);
            audio::ReverbZoneInput zin[2];
            zin[0].weight = w[0]; zin[0].priority = pr[0]; zin[0].params.wet = 0.6f;
            zin[1].weight = w[1]; zin[1].priority = pr[1]; zin[1].params.wet = 0.3f;
            audio::ReverbParams ambient; ambient.wet = 0.1f;
            const f32 viaShares = sh[0] * 0.6f + sh[1] * 0.3f + rem * 0.1f;
            agrees = agrees && near(viaShares, audio::blendReverbZones(zin, 2, ambient).wet, 1.0e-5f);
            if (i > 0) for (int k = 0; k < 3; ++k) if (std::fabs(sh[k] - prev[k]) > 0.08f) smooth = false;
            for (int k = 0; k < 3; ++k) prev[k] = sh[k];
        }
        check(sum1, "shares plus remainder are always 1");
        check(agrees, "shares times each zone's wet equal blendReverbZones' wet along the whole sweep");
        check(smooth, "no share moves by more than 0.08 per 5 cm while crossing between two zones (continuous; the blend is 200 cm)");
    }
}

int main() {
    AVER_INFO("AudioEnvironmentTest");
    testFft();
    testConvolverExact();
    testConvolverEdges();
    testConvolverCost();
    testConvolutionEffect();
    testZoneSends();
    testZoneShares();
    testDiffraction();
    if (g_failures == 0) AVER_INFO("=== all environment audio tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
