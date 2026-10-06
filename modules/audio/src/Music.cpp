// Music crossfading and the occlusion ray probe.
#include "aver/audio/Music.hpp"

#include <cmath>

namespace aver::audio {

VoiceHandle MusicCrossfader::play(std::unique_ptr<StreamSource> source, const Options& o) {
    if (!mixer_ || !source) return 0;

    PlayDesc d;
    d.volume = o.volume;
    d.looping = o.looping;
    d.bus = o.bus;
    d.fadeInSeconds = o.fadeSeconds;
    d.fadeCurve = o.curve;
    const VoiceHandle next = mixer_->playStream(std::move(source), d, o.stream);
    if (next == 0) return 0;    // keep the old track rather than going silent on a failed start

    if (current_ != 0) mixer_->fadeVoice(current_, 0.0f, o.fadeSeconds, o.curve, true);
    current_ = next;
    return next;
}

void MusicCrossfader::stop(f32 fadeSeconds, FadeCurve curve) {
    if (!mixer_ || current_ == 0) return;
    mixer_->fadeVoice(current_, 0.0f, fadeSeconds, curve, true);
    current_ = 0;
}

VoiceHandle MusicCrossfader::current() const {
    return mixer_ && current_ != 0 && mixer_->playing(current_) ? current_ : 0;
}

// ---------------------------------------------------------------- occlusion

f32 measureOcclusion(RayBlockFn fn, void* user, const f32 listener[3], const f32 source[3],
                     const OcclusionProbe& probe) {
    if (!fn) return 0.0f;
    f32 d[3] = {source[0] - listener[0], source[1] - listener[1], source[2] - listener[2]};
    const f32 len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1.0e-3f) return 0.0f;
    for (f32& c : d) c /= len;

    // A frame around the line of sight: `right` and `up` span the plane the probe ring lies in.
    const f32 refUp[3] = {std::fabs(d[2]) > 0.99f ? 1.0f : 0.0f, 0.0f, std::fabs(d[2]) > 0.99f ? 0.0f : 1.0f};
    f32 right[3] = {d[1] * refUp[2] - d[2] * refUp[1], d[2] * refUp[0] - d[0] * refUp[2],
                    d[0] * refUp[1] - d[1] * refUp[0]};
    const f32 rl = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    for (f32& c : right) c /= rl;
    const f32 up[3] = {right[1] * d[2] - right[2] * d[1], right[2] * d[0] - right[0] * d[2],
                       right[0] * d[1] - right[1] * d[0]};

    u32 n = probe.rayCount < 1 ? 1 : (probe.rayCount > 16 ? 16 : probe.rayCount);
    f32 sum = 0.0f;
    for (u32 i = 0; i < n; ++i) {
        f32 to[3] = {source[0], source[1], source[2]};
        if (i > 0) {
            const f32 a = 6.28318530718f * static_cast<f32>(i - 1) / static_cast<f32>(n - 1);
            const f32 ca = std::cos(a) * probe.probeRadiusCm, sa = std::sin(a) * probe.probeRadiusCm;
            for (int k = 0; k < 3; ++k) to[k] += right[k] * ca + up[k] * sa;
        }
        f32 b = fn(user, listener, to);
        sum += b < 0.0f ? 0.0f : (b > 1.0f ? 1.0f : b);
    }
    return sum / static_cast<f32>(n);
}

f32 smoothOcclusion(f32 current, f32 measured, f32 dt, f32 riseSec, f32 fallSec) {
    const f32 tau = measured > current ? riseSec : fallSec;
    if (dt <= 0.0f) return current;
    if (tau <= 1.0e-6f) return measured;
    return current + (measured - current) * (1.0f - std::exp(-dt / tau));
}

} // namespace aver::audio
