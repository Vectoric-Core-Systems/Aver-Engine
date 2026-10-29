// Uniformly partitioned FFT convolution: own radix-2 FFT, overlap-save, both channels of a stereo signal in one complex transform.
#include "aver/audio/Convolver.hpp"

#include <algorithm>
#include <cmath>

namespace aver::audio {
namespace {

constexpr f64 kTwoPi = 6.283185307179586476925286766559;

bool isPow2(u32 n) { return n >= 2 && (n & (n - 1)) == 0; }

} // namespace

// ---------------------------------------------------------------- FFT

Fft::Fft(u32 n) : n_(isPow2(n) ? n : 2) {
    cos_.resize(n_ / 2);
    sin_.resize(n_ / 2);
    for (u32 k = 0; k < n_ / 2; ++k) {
        cos_[k] = static_cast<f32>(std::cos(kTwoPi * static_cast<f64>(k) / static_cast<f64>(n_)));
        sin_[k] = static_cast<f32>(std::sin(kTwoPi * static_cast<f64>(k) / static_cast<f64>(n_)));
    }
    rev_.resize(n_);
    u32 bits = 0;
    while ((1u << bits) < n_) ++bits;
    for (u32 i = 0; i < n_; ++i) {
        u32 r = 0;
        for (u32 b = 0; b < bits; ++b) if (i & (1u << b)) r |= 1u << (bits - 1 - b);
        rev_[i] = r;
    }
}

void Fft::run(f32* re, f32* im, bool inv) const {
    for (u32 i = 0; i < n_; ++i) {
        const u32 j = rev_[i];
        if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    const f32 sgn = inv ? 1.0f : -1.0f;
    for (u32 len = 2; len <= n_; len <<= 1) {
        const u32 half = len >> 1, step = n_ / len;
        for (u32 i = 0; i < n_; i += len) {
            for (u32 j = 0; j < half; ++j) {
                const f32 wr = cos_[j * step], wi = sgn * sin_[j * step];
                const u32 a = i + j, b = a + half;
                const f32 tr = re[b] * wr - im[b] * wi;
                const f32 ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
            }
        }
    }
    if (inv) {
        const f32 s = 1.0f / static_cast<f32>(n_);
        for (u32 i = 0; i < n_; ++i) { re[i] *= s; im[i] *= s; }
    }
}

void Fft::forward(f32* re, f32* im) const { run(re, im, false); }
void Fft::inverse(f32* re, f32* im) const { run(re, im, true); }

// ---------------------------------------------------------------- resampling

std::vector<f32> resampleInterleaved(const f32* in, u32 frames, u32 channels, u32 srcRate, u32 dstRate) {
    std::vector<f32> out;
    if (!in || frames == 0 || channels == 0 || srcRate == 0 || dstRate == 0) return out;
    if (srcRate == dstRate) { out.assign(in, in + static_cast<usize>(frames) * channels); return out; }
    const f64 ratio = static_cast<f64>(srcRate) / static_cast<f64>(dstRate);
    u32 n = static_cast<u32>(static_cast<f64>(frames) / ratio + 0.5);
    if (n < 1) n = 1;
    out.resize(static_cast<usize>(n) * channels);
    for (u32 i = 0; i < n; ++i) {
        const f64 p = static_cast<f64>(i) * ratio;
        u32 i0 = static_cast<u32>(p);
        if (i0 >= frames) i0 = frames - 1;
        const u32 i1 = i0 + 1 < frames ? i0 + 1 : i0;
        const f32 fr = static_cast<f32>(p - static_cast<f64>(i0));
        for (u32 c = 0; c < channels; ++c) {
            const f32 a = in[static_cast<usize>(i0) * channels + c], b = in[static_cast<usize>(i1) * channels + c];
            out[static_cast<usize>(i) * channels + c] = a + (b - a) * fr;
        }
    }
    return out;
}

// ---------------------------------------------------------------- engine

// One impulse response in the frequency domain, with the history of input spectra it convolves against. parts == 0 is silence.
struct Convolver::Engine {
    u32 parts = 0;
    u32 irChannels = 1;
    std::vector<f32> hRe[2], hIm[2];            // [part * bins + k]
    std::vector<f32> fLr, fLi, fRr, fRi;        // input spectra ring, [slot * bins + k]
    u32 head = 0, zeroRun = 0;

    void clearHistory() {
        std::fill(fLr.begin(), fLr.end(), 0.0f); std::fill(fLi.begin(), fLi.end(), 0.0f);
        std::fill(fRr.begin(), fRr.end(), 0.0f); std::fill(fRi.begin(), fRi.end(), 0.0f);
        head = 0;
        zeroRun = 0;
    }
};

Convolver::Convolver(u32 sampleRate, u32 blockFrames)
    : rate_(sampleRate ? sampleRate : 48000),
      block_(isPow2(blockFrames) && blockFrames >= 32 && blockFrames <= 8192 ? blockFrames : kDefaultBlock),
      fftSize_(block_ * 2), bins_(block_ + 1), fft_(block_ * 2) {
    for (auto& r : retired_) r.store(nullptr, std::memory_order_relaxed);
    inL_.assign(block_, 0.0f); inR_.assign(block_, 0.0f);
    prevL_.assign(block_, 0.0f); prevR_.assign(block_, 0.0f);
    outL_.assign(block_, 0.0f); outR_.assign(block_, 0.0f);
    fadeL_.assign(block_, 0.0f); fadeR_.assign(block_, 0.0f);
    re_.assign(fftSize_, 0.0f); im_.assign(fftSize_, 0.0f);
    xLr_.assign(bins_, 0.0f); xLi_.assign(bins_, 0.0f); xRr_.assign(bins_, 0.0f); xRi_.assign(bins_, 0.0f);
    yLr_.assign(bins_, 0.0f); yLi_.assign(bins_, 0.0f); yRr_.assign(bins_, 0.0f); yRi_.assign(bins_, 0.0f);
    preSize_ = static_cast<u32>(kMaxPredelaySeconds * static_cast<f32>(rate_)) + 2;
    preL_.assign(preSize_, 0.0f);
    preR_.assign(preSize_, 0.0f);
}

Convolver::~Convolver() {
    collect();
    delete pending_.exchange(nullptr);
    delete active_;
    delete fading_;
}

void Convolver::collect() {
    for (auto& r : retired_) delete r.exchange(nullptr, std::memory_order_acq_rel);
}

std::unique_ptr<Convolver::Engine> Convolver::build(const f32* ir, u32 frames, u32 channels) const {
    auto e = std::make_unique<Engine>();
    if (!ir || frames == 0) return e;
    const u32 ch = channels >= 2 ? 2 : 1;
    e->irChannels = ch;
    e->parts = (frames + block_ - 1) / block_;
    const usize cells = static_cast<usize>(e->parts) * bins_;
    std::vector<f32> re(fftSize_), im(fftSize_);
    for (u32 c = 0; c < ch; ++c) {
        e->hRe[c].assign(cells, 0.0f);
        e->hIm[c].assign(cells, 0.0f);
        for (u32 p = 0; p < e->parts; ++p) {
            std::fill(re.begin(), re.end(), 0.0f);
            std::fill(im.begin(), im.end(), 0.0f);
            for (u32 i = 0; i < block_; ++i) {
                const usize f = static_cast<usize>(p) * block_ + i;
                if (f >= frames) break;
                const f32 v = ir[f * channels + c];
                re[i] = std::isfinite(v) ? v : 0.0f;
            }
            fft_.forward(re.data(), im.data());
            std::copy(re.begin(), re.begin() + bins_, e->hRe[c].begin() + static_cast<usize>(p) * bins_);
            std::copy(im.begin(), im.begin() + bins_, e->hIm[c].begin() + static_cast<usize>(p) * bins_);
        }
    }
    e->fLr.assign(cells, 0.0f); e->fLi.assign(cells, 0.0f);
    e->fRr.assign(cells, 0.0f); e->fRi.assign(cells, 0.0f);
    return e;
}

bool Convolver::setImpulse(const f32* interleaved, u32 frames, u32 channels, u32 srcRate) {
    if (!interleaved || frames == 0 || channels == 0 || channels > 16 || srcRate == 0) return false;
    // Keep two channels at most, truncated to the longest impulse the convolver takes.
    const u32 keepCh = channels >= 2 ? 2 : 1;
    const u32 maxSrc = static_cast<u32>(kMaxImpulseSeconds * static_cast<f32>(srcRate));
    const u32 keepFrames = frames < maxSrc ? frames : maxSrc;
    std::vector<f32> src(static_cast<usize>(keepFrames) * keepCh);
    for (u32 f = 0; f < keepFrames; ++f)
        for (u32 c = 0; c < keepCh; ++c) {
            const f32 v = interleaved[static_cast<usize>(f) * channels + c];
            src[static_cast<usize>(f) * keepCh + c] = std::isfinite(v) ? v : 0.0f;
        }
    const std::vector<f32> ir = resampleInterleaved(src.data(), keepFrames, keepCh, srcRate, rate_);
    if (ir.empty()) return false;
    const u32 irFrames = static_cast<u32>(ir.size() / keepCh);

    std::unique_ptr<Engine> e = build(ir.data(), irFrames, keepCh);
    collect();
    src_ = std::move(src);
    srcChannels_ = keepCh;
    srcRate_ = srcRate;
    irFrames_.store(irFrames, std::memory_order_relaxed);
    parts_.store(e->parts, std::memory_order_relaxed);
    has_.store(true, std::memory_order_release);
    delete pending_.exchange(e.release(), std::memory_order_acq_rel);
    return true;
}

bool Convolver::adoptImpulse(const Convolver& other) {
    if (other.src_.empty() || other.srcChannels_ == 0) return false;
    return setImpulse(other.src_.data(), static_cast<u32>(other.src_.size() / other.srcChannels_), other.srcChannels_, other.srcRate_);
}

void Convolver::clearImpulse() {
    collect();
    src_.clear();
    srcChannels_ = srcRate_ = 0;
    irFrames_.store(0, std::memory_order_relaxed);
    parts_.store(0, std::memory_order_relaxed);
    has_.store(false, std::memory_order_release);
    delete pending_.exchange(new Engine(), std::memory_order_acq_rel);
}

void Convolver::setPredelayFrames(u32 frames) {
    const u32 cap = preSize_ > 2 ? preSize_ - 2 : 0;
    predelay_.store(frames < cap ? frames : cap, std::memory_order_relaxed);
}

void Convolver::setSampleRate(u32 rate) {
    if (rate == 0 || rate == rate_) return;
    collect();
    delete pending_.exchange(nullptr, std::memory_order_acq_rel);
    delete active_; active_ = nullptr;
    delete fading_; fading_ = nullptr;
    rate_ = rate;
    preSize_ = static_cast<u32>(kMaxPredelaySeconds * static_cast<f32>(rate_)) + 2;
    preL_.assign(preSize_, 0.0f);
    preR_.assign(preSize_, 0.0f);
    prePos_ = 0;
    setPredelayFrames(predelay_.load(std::memory_order_relaxed));
    if (!src_.empty()) {
        const u32 srcFrames = static_cast<u32>(src_.size() / srcChannels_);
        const std::vector<f32> ir = resampleInterleaved(src_.data(), srcFrames, srcChannels_, srcRate_, rate_);
        if (!ir.empty()) {
            std::unique_ptr<Engine> e = build(ir.data(), static_cast<u32>(ir.size() / srcChannels_), srcChannels_);
            irFrames_.store(static_cast<u32>(ir.size() / srcChannels_), std::memory_order_relaxed);
            parts_.store(e->parts, std::memory_order_relaxed);
            active_ = e.release();
        }
    }
    reset();
}

void Convolver::reset() {
    pos_ = 0;
    prePos_ = 0;
    for (auto* v : {&inL_, &inR_, &prevL_, &prevR_, &outL_, &outR_, &preL_, &preR_}) std::fill(v->begin(), v->end(), 0.0f);
    if (active_) active_->clearHistory();
    if (fading_) fading_->clearHistory();
}

void Convolver::retire(Engine* e) {
    if (!e) return;
    for (auto& r : retired_) {
        Engine* expected = nullptr;
        if (r.compare_exchange_strong(expected, e, std::memory_order_acq_rel)) return;
    }
    // takePending() only swaps with a free slot, so this is unreachable; leaking is the one thing that cannot hurt the audio thread.
}

void Convolver::takePending() {
    if (!pending_.load(std::memory_order_relaxed)) return;
    bool room = false;
    for (auto& r : retired_) if (!r.load(std::memory_order_acquire)) { room = true; break; }
    if (!room) return;                      // the game thread has not collected yet: keep the new impulse waiting
    Engine* n = pending_.exchange(nullptr, std::memory_order_acq_rel);
    if (!n) return;
    fading_ = active_;
    active_ = n;
}

// One engine's output for the block whose spectrum is in xLr_.. : adds the spectrum to its ring and sums the partitions.
void Convolver::runEngine(Engine& e, bool inputZero, f32* outL, f32* outR) {
    if (e.parts == 0) {
        std::fill(outL, outL + block_, 0.0f);
        std::fill(outR, outR + block_, 0.0f);
        return;
    }
    e.zeroRun = inputZero ? e.zeroRun + 1 : 0;
    if (e.zeroRun > e.parts + 1) {          // every spectrum in the ring is zero: the tail has finished
        std::fill(outL, outL + block_, 0.0f);
        std::fill(outR, outR + block_, 0.0f);
        return;
    }

    const u32 bins = bins_;
    e.head = e.head + 1 >= e.parts ? 0 : e.head + 1;
    const usize base = static_cast<usize>(e.head) * bins;
    std::copy(xLr_.begin(), xLr_.end(), e.fLr.begin() + base);
    std::copy(xLi_.begin(), xLi_.end(), e.fLi.begin() + base);
    std::copy(xRr_.begin(), xRr_.end(), e.fRr.begin() + base);
    std::copy(xRi_.begin(), xRi_.end(), e.fRi.begin() + base);

    std::fill(yLr_.begin(), yLr_.end(), 0.0f); std::fill(yLi_.begin(), yLi_.end(), 0.0f);
    std::fill(yRr_.begin(), yRr_.end(), 0.0f); std::fill(yRi_.begin(), yRi_.end(), 0.0f);
    const u32 hr = e.irChannels == 2 ? 1u : 0u;
    u32 slot = e.head;
    for (u32 p = 0; p < e.parts; ++p) {
        const usize s = static_cast<usize>(slot) * bins, h = static_cast<usize>(p) * bins;
        const f32* flr = e.fLr.data() + s; const f32* fli = e.fLi.data() + s;
        const f32* frr = e.fRr.data() + s; const f32* fri = e.fRi.data() + s;
        const f32* h0r = e.hRe[0].data() + h; const f32* h0i = e.hIm[0].data() + h;
        const f32* h1r = e.hRe[hr].data() + h; const f32* h1i = e.hIm[hr].data() + h;
        f32* ylr = yLr_.data(); f32* yli = yLi_.data(); f32* yrr = yRr_.data(); f32* yri = yRi_.data();
        for (u32 k = 0; k < bins; ++k) {
            ylr[k] += flr[k] * h0r[k] - fli[k] * h0i[k];
            yli[k] += flr[k] * h0i[k] + fli[k] * h0r[k];
            yrr[k] += frr[k] * h1r[k] - fri[k] * h1i[k];
            yri[k] += frr[k] * h1i[k] + fri[k] * h1r[k];
        }
        slot = slot == 0 ? e.parts - 1 : slot - 1;
    }

    // Z = YL + i YR over the full spectrum; the upper half comes from the conjugate symmetry of two real signals.
    const u32 n = fftSize_, half = block_;
    for (u32 k = 0; k <= half; ++k) {
        re_[k] = yLr_[k] - yRi_[k];
        im_[k] = yLi_[k] + yRr_[k];
    }
    for (u32 k = half + 1; k < n; ++k) {
        const u32 m = n - k;
        re_[k] = yLr_[m] + yRi_[m];
        im_[k] = yRr_[m] - yLi_[m];
    }
    fft_.inverse(re_.data(), im_.data());
    for (u32 i = 0; i < block_; ++i) { outL[i] = re_[block_ + i]; outR[i] = im_[block_ + i]; }
}

// A full input block is in inL_ / inR_: turn it into the next block of output.
void Convolver::computeBlock() {
    bool zero = true;
    for (u32 i = 0; i < block_; ++i) if (inL_[i] != 0.0f || inR_[i] != 0.0f) { zero = false; break; }

    takePending();
    const bool run = (active_ && active_->parts) || (fading_ && fading_->parts);
    if (run) {
        // Both channels in one transform: L in the real part, R in the imaginary part, previous block first.
        for (u32 i = 0; i < block_; ++i) {
            re_[i] = prevL_[i]; im_[i] = prevR_[i];
            re_[block_ + i] = inL_[i]; im_[block_ + i] = inR_[i];
        }
        fft_.forward(re_.data(), im_.data());
        const u32 n = fftSize_;
        for (u32 k = 0; k <= block_; ++k) {
            const u32 m = (n - k) & (n - 1);
            xLr_[k] = 0.5f * (re_[k] + re_[m]);
            xLi_[k] = 0.5f * (im_[k] - im_[m]);
            xRr_[k] = 0.5f * (im_[k] + im_[m]);
            xRi_[k] = -0.5f * (re_[k] - re_[m]);
        }
    }
    std::copy(inL_.begin(), inL_.end(), prevL_.begin());
    std::copy(inR_.begin(), inR_.end(), prevR_.begin());

    if (active_) runEngine(*active_, zero, outL_.data(), outR_.data());
    else { std::fill(outL_.begin(), outL_.end(), 0.0f); std::fill(outR_.begin(), outR_.end(), 0.0f); }

    if (fading_) {
        runEngine(*fading_, zero, fadeL_.data(), fadeR_.data());
        for (u32 i = 0; i < block_; ++i) {
            const f32 t = static_cast<f32>(i + 1) / static_cast<f32>(block_);
            outL_[i] = fadeL_[i] * (1.0f - t) + outL_[i] * t;
            outR_[i] = fadeR_[i] * (1.0f - t) + outR_[i] * t;
        }
        retire(fading_);
        fading_ = nullptr;
    }
}

void Convolver::process(const f32* inL, const f32* inR, f32* outL, f32* outR, u32 frames) {
    const u32 pd = predelay_.load(std::memory_order_relaxed);
    for (u32 i = 0; i < frames; ++i) {
        f32 xl = inL[i], xr = inR[i];
        preL_[prePos_] = xl;
        preR_[prePos_] = xr;
        if (pd) {
            const u32 rd = prePos_ >= pd ? prePos_ - pd : prePos_ + preSize_ - pd;
            xl = preL_[rd];
            xr = preR_[rd];
        }
        if (++prePos_ >= preSize_) prePos_ = 0;

        outL[i] = outL_[pos_];
        outR[i] = outR_[pos_];
        inL_[pos_] = xl;
        inR_[pos_] = xr;
        if (++pos_ >= block_) {
            pos_ = 0;
            computeBlock();
        }
    }
}

} // namespace aver::audio
