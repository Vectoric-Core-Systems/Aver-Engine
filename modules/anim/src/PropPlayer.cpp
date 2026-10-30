// PropPlayer: animation-player playback and blending over property tracks (see PropAnim.hpp).

#include "aver/anim/PropAnim.hpp"

#include <algorithm>
#include <cmath>

namespace aver::anim {
namespace {

using namespace fmt;

bool approxEq(double a, double b) { return std::fabs(a - b) <= kPropEps * std::max(1.0, std::fabs(a)); }
double fposmod(double x, double y) {
    if (y == 0) return 0;
    double r = std::fmod(x, y);
    if (r < 0) r += y;
    return r;
}
int comps(OcPropKind k) {
    switch (k) {
        case OcPropKind::Bool: case OcPropKind::Int: case OcPropKind::Float: return 1;
        case OcPropKind::Vec2: return 2;
        case OcPropKind::Vec3: return 3;
        case OcPropKind::Vec4: case OcPropKind::Quat: case OcPropKind::Color: return 4;
        default: return 0;
    }
}

struct Q { double x, y, z, w; };
Q qmul(const Q& a, const Q& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y + a.y * b.w + a.z * b.x - a.x * b.z,
            a.w * b.z + a.z * b.w + a.x * b.y - a.y * b.x, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Q qslerp(const Q& a, const Q& b, double t) {
    double c = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    Q to = b;
    if (c < 0) { c = -c; to = {-b.x, -b.y, -b.z, -b.w}; }
    double s0, s1;
    if (1.0 - c > 1e-6) {
        const double om = std::acos(std::min(c, 1.0)), so = std::sin(om);
        s0 = std::sin((1.0 - t) * om) / so; s1 = std::sin(t * om) / so;
    } else { s0 = 1.0 - t; s1 = t; }
    return {s0 * a.x + s1 * to.x, s0 * a.y + s1 * to.y, s0 * a.z + s1 * to.z, s0 * a.w + s1 * to.w};
}
Q qnorm(Q q) {
    const double l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return l > 0 ? Q{q.x / l, q.y / l, q.z / l, q.w / l} : Q{0, 0, 0, 1};
}
Q qinv(const Q& q) { return {-q.x, -q.y, -q.z, q.w}; }
Q qv(const PropValue& v) { return {v.n[0], v.n[1], v.n[2], v.n[3]}; }

// Moves a playhead by `d` seconds under the clip's loop mode: the new position, the segments crossed (a wrap adds the part after the
// loop point with incStart), and how many ping-pong bounces happened. Returns true when it wrapped or bounced.
bool stepClip(const OcAnimation& c, double prev, double d, double& next, std::vector<PropSegment>& segs, int& bounces) {
    const double len = c.duration;
    bool looped = false;
    next = prev + d;
    bounces = 0;
    switch (c.propLoop) {
    case OcPropLoop::None:
        next = std::clamp(next, 0.0, std::max(len, 0.0));
        segs.push_back({prev, next, false});
        break;
    case OcPropLoop::Linear:
        if (len <= 0) { next = 0; segs.push_back({prev, 0, false}); }
        else if (d == 0) {
            if (next < 0 || next > len) next = fposmod(next, len);
            segs.push_back({prev, next, false});
        } else if (d > 0 && next >= len) {
            segs.push_back({prev, len, false});
            next = fposmod(next - len, len);
            segs.push_back({0, next, true});
            looped = true;
        } else if (d < 0 && next <= 0) {
            segs.push_back({prev, 0, false});
            next = len - fposmod(-next, len);
            segs.push_back({len, next, true});
            looped = true;
        } else segs.push_back({prev, next, false});
        break;
    case OcPropLoop::PingPong: {
        if (len <= 0) { next = 0; segs.push_back({prev, 0, false}); break; }
        double pos = prev;
        if (pos < 0 || pos > len) { pos = fposmod(pos, 2 * len); if (pos > len) pos = 2 * len - pos; }
        double dir = d >= 0 ? 1.0 : -1.0;
        if (d == 0) { next = pos; segs.push_back({prev, pos, false}); break; }
        double remaining = std::fabs(d);
        for (int guard = 0; remaining > 0 && guard < 16; ++guard) {
            const double room = dir > 0 ? len - pos : pos;
            if (remaining <= room) {
                const double to = pos + dir * remaining;
                segs.push_back({pos, to, false});
                pos = to;
                remaining = 0;
                break;
            }
            const double to = dir > 0 ? len : 0.0;
            segs.push_back({pos, to, false});
            remaining -= room;
            pos = to;
            dir = -dir;
            ++bounces;
            looped = true;
        }
        next = pos;
        break;
    }
    }
    return looped;
}

// One blended target: the accumulated value and what touched it.
struct Acc {
    PropValue value;
    PropValue init;
    bool any = false, direct = false, discreteWritten = false, angle = false;
};

} // namespace

// ---- library ----

void PropPlayer::setClip(const std::string& key, Clip clip) {
    for (auto& e : clips_) if (e.first == key) { e.second = std::move(clip); return; }
    clips_.emplace_back(key, std::move(clip));
}

bool PropPlayer::removeClip(const std::string& key) {
    for (usize i = 0; i < clips_.size(); ++i) {
        if (clips_[i].first != key) continue;
        if (cur_.clip && cur_.name == key) stop();
        for (auto& b : blends_) if (b.data.name == key) b.left = 0;
        clips_.erase(clips_.begin() + isize(i));
        return true;
    }
    return false;
}

PropPlayer::Clip PropPlayer::clip(const std::string& key) const {
    for (const auto& e : clips_) if (e.first == key) return e.second;
    return nullptr;
}

std::vector<std::string> PropPlayer::clipKeys() const {
    std::vector<std::string> r;
    for (const auto& e : clips_) r.push_back(e.first);
    return r;
}

void PropPlayer::setBlendTime(const std::string& from, const std::string& to, double sec) { blendTimes_[{from, to}] = sec; }

double PropPlayer::explicitBlendTime(const std::string& from, const std::string& to) const {
    for (const auto& k : {std::pair<std::string, std::string>{from, to}, {std::string(), to}, {from, std::string()}}) {
        auto it = blendTimes_.find(k);
        if (it != blendTimes_.end()) return it->second;
    }
    return -1.0;
}

double PropPlayer::blendTime(const std::string& from, const std::string& to) const {
    const double e = explicitBlendTime(from, to);
    return e < 0 ? 0.0 : e;
}

void PropPlayer::setNext(const std::string& from, const std::string& to) {
    if (to.empty()) next_.erase(from); else next_[from] = to;
}

std::string PropPlayer::next(const std::string& from) const {
    auto it = next_.find(from);
    return it == next_.end() ? std::string() : it->second;
}

// ---- transport ----

void PropPlayer::push(PropEvent::Type t, std::string a, std::string b) {
    PropEvent e;
    e.type = t;
    e.a = std::move(a);
    e.b = std::move(b);
    events_.push_back(std::move(e));
}

double PropPlayer::currentBlendAmount() const {
    double s = 0;
    for (const auto& b : blends_) s += b.left;
    return std::max(0.0, 1.0 - s);
}

void PropPlayer::emitAudioStops() {
    if (!cur_.clip) return;
    for (const OcPropTrack& t : cur_.clip->propTracks) {
        if (t.type != OcPropTrackType::Audio || !t.enabled) continue;
        PropEvent e;
        e.type = PropEvent::AudioStop;
        e.path = t.path;
        events_.push_back(std::move(e));
    }
}

bool PropPlayer::play(const std::string& nameIn, double customBlend, double customSpeed, bool fromEnd) {
    std::string name = nameIn.empty() ? assigned_ : nameIn;
    Clip c = clip(name);
    if (!c) return false;
    const double len = c->duration;

    if (assigned_ != name || !cur_.clip) {
        double bt = customBlend;
        if (bt < 0) {
            bt = std::max(0.0, explicitBlendTime(cur_.clip ? cur_.name : assigned_, name));
            if (bt <= 0) bt = defaultBlendTime;
        }
        if (cur_.clip && bt > 0) {
            Blend b;
            b.data = cur_;
            b.left = currentBlendAmount();
            b.time = bt;
            blends_.push_back(std::move(b));
        } else if (cur_.clip) {                     // no fade: the outgoing clips go, a capture snapshot stays
            blends_.erase(std::remove_if(blends_.begin(), blends_.end(), [](const Blend& b) { return !b.snapshot; }), blends_.end());
        }
        const bool changed = assigned_ != name;
        cur_ = Playing{c, fromEnd ? len : 0.0, 1.0, name};
        assigned_ = name;
        if (changed || !playing_) push(PropEvent::CurrentChanged, name);
    } else {
        if (fromEnd && approxEq(cur_.pos, 0.0)) cur_.pos = len;
        else if (!fromEnd && approxEq(cur_.pos, len)) cur_.pos = 0.0;
        else if (playing_) return true;     // already playing it: nothing restarts
        cur_.clip = c;
    }
    cur_.speed = customSpeed;
    seeked_ = false;
    started_ = true;
    playing_ = true;
    push(PropEvent::Started, name);
    if (!endReached_) queue_.clear();
    const std::string chained = next(name);
    if (!chained.empty() && clip(chained)) queue_.push_back(chained);
    return true;
}

void PropPlayer::stop(bool keepState) {
    const bool had = cur_.clip != nullptr;
    emitAudioStops();
    blends_.clear();
    queue_.clear();
    if (had) {
        cur_.pos = 0;
        if (!keepState) {
            seeked_ = true; internalSeeked_ = true; started_ = false;
            process(0.0, true);
        }
    }
    cur_ = Playing{};
    queue_.clear();
    playing_ = false;
    started_ = false;
    seeked_ = false;
    if (had) push(PropEvent::CurrentChanged, std::string());
}

void PropPlayer::pause() {
    if (playing_) emitAudioStops();
    queue_.clear();
    playing_ = false;
}

void PropPlayer::queue(const std::string& name) {
    if (!isPlaying()) play(name); else queue_.push_back(name);
}

void PropPlayer::seek(double time, bool update, bool updateOnly) {
    if (!active || !cur_.clip) return;
    const bool backward = time < cur_.pos && !approxEq(time, cur_.pos);
    if (started_) process(0.0, false);          // a seek right after play() lets the first frame happen first
    cur_.pos = time;
    seeked_ = true;
    internalSeeked_ = false;
    if (update) {
        process(backward ? -0.0 : 0.0, updateOnly);
        seeked_ = false;
    }
}

void PropPlayer::setAssigned(const std::string& name) {
    Clip c = clip(name);
    if (!c) return;
    if (isPlaying()) { play(name); return; }
    cur_ = Playing{c, 0.0, 1.0, name};
    assigned_ = name;
    push(PropEvent::CurrentChanged, name);
}

void PropPlayer::advance(double delta) { process(delta, false); }

std::vector<std::pair<OcPropTrackType, std::string>> PropPlayer::captureTracks(const std::string& name) const {
    std::vector<std::pair<OcPropTrackType, std::string>> r;
    Clip c = clip(name);
    if (!c) return r;
    for (const OcPropTrack& t : c->propTracks)
        if (t.update == OcPropUpdate::Capture && t.enabled) r.emplace_back(t.type, t.path);
    return r;
}

void PropPlayer::capture(const std::vector<std::pair<std::pair<OcPropTrackType, std::string>, PropValue>>& values, double duration,
                         double, double) {
    if (values.empty()) return;
    auto snap = std::make_shared<OcAnimation>();
    snap->duration = float(std::max(duration, 1e-6));
    for (const auto& v : values) {
        OcPropTrack t;
        t.type = v.first.first;
        t.path = v.first.second;
        OcPropKey k;
        k.value = v.second;
        t.keys.push_back(std::move(k));
        snap->propTracks.push_back(std::move(t));
    }
    Blend b;
    b.data = Playing{snap, 0.0, 1.0, std::string()};
    b.left = currentBlendAmount();
    b.time = std::max(duration, 1e-6);
    b.snapshot = true;
    blends_.push_back(std::move(b));
}

// ---- one pass ----

u32 PropPlayer::intern(OcPropTrackType type, const std::string& path) {
    const auto key = std::make_pair(u8(type), path);
    auto it = regIndex_.find(key);
    if (it != regIndex_.end()) return it->second;
    const u32 id = u32(reg_.size());
    reg_.push_back(TrackReg{type, path, false, {}});
    regIndex_.emplace(key, id);
    return id;
}

void PropPlayer::process(double delta, bool updateOnly) {
    if (!active || !cur_.clip) return;
    const bool started = started_;
    const bool seeked = seeked_, internal = internalSeeked_;
    started_ = false;
    endReached_ = false;
    endNotify_ = false;
    outputs_.clear();

    for (auto& b : blends_) b.left = std::max(0.0, b.left - std::fabs(speedScale * delta) / b.time);
    std::vector<Inst> insts;
    playbackData(cur_, delta, currentBlendAmount(), seeked, internal, started, true, insts);
    const bool external = seeked && !internal && !started;
    if (endReached_) blends_.clear();
    else for (auto& b : blends_) playbackData(b.data, delta, b.left, false, false, false, false, insts);
    blends_.erase(std::remove_if(blends_.begin(), blends_.end(), [](const Blend& b) { return b.left <= 0.0; }), blends_.end());
    seeked_ = false;
    internalSeeked_ = false;

    mix(insts, updateOnly, external);

    if (endReached_) {
        if (!queue_.empty()) {
            const std::string old = assigned_;
            const std::string nn = queue_.front();
            const bool notify = endNotify_;
            play(nn);
            if (!queue_.empty() && queue_.front() == nn) queue_.erase(queue_.begin());
            if (notify) push(PropEvent::Changed, old, nn);
        } else {
            playing_ = false;
            emitAudioStops();
            if (endNotify_) push(PropEvent::Finished, assigned_);
        }
        endReached_ = false;
        endNotify_ = false;
    }
    push(PropEvent::MixerApplied);
}

void PropPlayer::playbackData(Playing& p, double delta, double weight, bool seeked, bool internalSeeked, bool started, bool isCurrent,
                              std::vector<Inst>& out) {
    const OcAnimation& c = *p.clip;
    const double len = c.duration;
    const double speed = speedScale * p.speed;
    const bool backwards = std::signbit(speed);
    const double raw = delta * speed;
    const double d = started ? 0.0 : raw;
    const double prev = p.pos;
    double next = prev + d;

    Inst in;
    in.clip = p.clip;
    in.name = p.name;
    in.weight = weight;
    in.backward = std::signbit(raw);

    int bounces = 0;
    const bool looped = stepClip(c, prev, d, next, in.segs, bounces);
    if (bounces & 1) p.speed *= -1.0;
    if (looped && isCurrent) push(PropEvent::Looped, p.name);

    double w = weight;
    if (isCurrent && c.propLoop == OcPropLoop::None) {
        if (!backwards && (prev < len || approxEq(prev, len)) && approxEq(next, len)) {
            endReached_ = true;
            endNotify_ = prev < len && !approxEq(prev, len);
            next = len;
            w = 1.0;
        } else if (backwards && (prev > 0 || approxEq(prev, 0.0)) && approxEq(next, 0.0)) {
            endReached_ = true;
            endNotify_ = prev > 0 && !approxEq(prev, 0.0);
            next = 0;
            w = 1.0;
        }
    }
    in.weight = w;
    p.pos = next;
    if (started) { in.time = prev; in.delta = 0; in.seeked = true; in.external = false; }
    else { in.time = next; in.delta = d; in.seeked = seeked; in.external = seeked && !internalSeeked; }
    out.push_back(std::move(in));
}

void PropPlayer::mix(std::vector<Inst>& insts, bool updateOnly, bool) {
    // Total weight per target, each animation counting a target once.
    std::vector<double> total;
    for (const Inst& in : insts) {
        std::vector<char> seen;
        for (usize ti = 0; ti < in.clip->propTracks.size(); ++ti) {
            const OcPropTrack& t = in.clip->propTracks[ti];
            if (!t.enabled) continue;
            const u32 id = intern(t.type, t.path);
            if (total.size() <= id) total.resize(reg_.size(), 0.0);
            if (seen.size() <= id) seen.resize(reg_.size(), 0);
            if (seen[id]) continue;
            seen[id] = 1;
            total[id] += in.weight * in.trackWeight(ti);
        }
    }
    total.resize(reg_.size(), 0.0);
    rootMotion_ = PropRootMotion{};

    // The RESET clip's first keys are the initial values of the targets it names.
    Clip reset = clip("RESET");
    if (!reset) for (const auto& e : clips_) if (e.first.size() > 6 && e.first.compare(e.first.size() - 6, 6, "/RESET") == 0) { reset = e.second; break; }

    std::vector<Acc> acc(reg_.size());
    std::vector<u32> order;

    for (const Inst& in : insts) {
        const OcAnimation& c = *in.clip;
        for (usize ti = 0; ti < c.propTracks.size(); ++ti) {
            const OcPropTrack& t = c.propTracks[ti];
            if (!t.enabled || t.keys.empty()) continue;
            const u32 id = intern(t.type, t.path);
            const double w = in.weight * in.trackWeight(ti);
            if (std::fabs(w) < 1e-9) continue;
            double blend = w;
            if (!deterministic) {
                if (std::fabs(total[id]) < 1e-9) continue;
                blend = w / total[id];
            }

            if (!rootPath_.empty() && t.path == rootPath_ &&
                (t.type == OcPropTrackType::Position3D || t.type == OcPropTrackType::Rotation3D || t.type == OcPropTrackType::Scale3D)) {
                rootMotionStep(in, c, t, blend);
                continue;
            }

            switch (t.type) {
            case OcPropTrackType::Method: {
                if (updateOnly) break;
                std::vector<int> idx;
                if (in.seeked) { const int k = propFindKey(t, in.time, in.external ? PropFind::Nearest : PropFind::Exact, in.backward, c.duration); if (k >= 0) idx.push_back(k); }
                else propKeysInSegments(t, in.segs, idx);
                for (int k : idx) {
                    PropEvent e;
                    e.type = PropEvent::Method;
                    e.a = t.keys[usize(k)].value.s;
                    e.path = t.path;
                    e.args = t.keys[usize(k)].args;
                    events_.push_back(std::move(e));
                }
                break;
            }
            case OcPropTrackType::Audio: {
                if (updateOnly) break;
                int k = -1;
                if (in.seeked) k = propFindKey(t, in.time, in.external ? PropFind::Nearest : PropFind::Exact, in.backward, c.duration);
                else { std::vector<int> idx; propKeysInSegments(t, in.segs, idx); if (!idx.empty()) k = idx.back(); }
                if (k < 0) break;
                const OcPropKey& key = t.keys[usize(k)];
                PropEvent e;
                e.type = PropEvent::AudioPlay;
                e.path = t.path;
                e.a = key.value.s;
                e.t0 = key.startOffset + (in.seeked ? std::max(0.0, in.time - key.time) : 0.0);
                e.t1 = key.endOffset;
                events_.push_back(std::move(e));
                break;
            }
            case OcPropTrackType::Animation: {
                if (updateOnly) break;
                int k = -1;
                if (in.seeked) k = propFindKey(t, in.time, in.external ? PropFind::Nearest : PropFind::Exact, in.backward, c.duration);
                else { std::vector<int> idx; propKeysInSegments(t, in.segs, idx); if (!idx.empty()) k = idx.back(); }
                if (k < 0) break;
                const OcPropKey& key = t.keys[usize(k)];
                PropEvent e;
                e.path = t.path;
                if (key.value.s == "[stop]") e.type = PropEvent::AnimStop;
                else {
                    e.type = PropEvent::AnimPlay;
                    e.a = key.value.s;
                    e.t0 = in.seeked ? std::max(0.0, in.time - key.time) : 0.0;
                    e.t1 = in.seeked ? 1.0 : 0.0;
                }
                events_.push_back(std::move(e));
                break;
            }
            default: {
                const bool discreteTrack = t.type == OcPropTrackType::Value && t.update == OcPropUpdate::Discrete && discrete != PropDiscrete::ForceContinuous;
                Acc& a = acc[id];
                if (discreteTrack) {
                    std::vector<int> idx;
                    if (in.seeked) { const int k = propFindKey(t, in.time, in.external ? PropFind::Nearest : PropFind::Exact, in.backward, c.duration); if (k >= 0) idx.push_back(k); }
                    else propKeysInSegments(t, in.segs, idx);
                    for (int k : idx) {
                        PropOutput o;
                        o.id = id;
                        o.value = t.keys[usize(k)].value;
                        o.discrete = true;
                        outputs_.push_back(std::move(o));
                        a.discreteWritten = true;
                    }
                    break;
                }
                PropValue v;
                if (t.type == OcPropTrackType::Bezier) { v.kind = OcPropKind::Float; v.n[0] = propSampleBezier(t, in.time); }
                else {
                    PropSample s = propSample(c, t, in.time, in.backward);
                    if (!s.ok) break;
                    v = s.value;
                }
                if (!a.any) {
                    TrackReg& r = reg_[id];
                    if (!r.hasInit) {
                        r.init = t.keys[0].value;
                        if (t.type == OcPropTrackType::Bezier) { r.init = PropValue{}; r.init.kind = OcPropKind::Float; r.init.n[0] = t.keys[0].value.n[0]; }
                        if (reset) for (const OcPropTrack& rt : reset->propTracks)
                            if (rt.type == t.type && rt.path == t.path && !rt.keys.empty()) { r.init = rt.keys[0].value; break; }
                        r.hasInit = true;
                    }
                    a.init = r.init;
                    a.value = r.init;
                    a.any = true;
                    a.angle = t.interp == OcPropInterp::LinearAngle || t.interp == OcPropInterp::CubicAngle;
                    order.push_back(id);
                }
                if (!propInterpolable(v.kind) || v.kind != a.init.kind) { a.value = v; a.direct = true; break; }
                if (a.direct) { a.value = v; break; }
                if (v.kind == OcPropKind::Quat) {
                    const Q delta = qmul(qinv(qv(a.init)), qv(v));
                    const Q r = qnorm(qmul(qv(a.value), qslerp({0, 0, 0, 1}, delta, blend)));
                    a.value.n[0] = r.x; a.value.n[1] = r.y; a.value.n[2] = r.z; a.value.n[3] = r.w;
                } else if (a.angle && (v.kind == OcPropKind::Float || v.kind == OcPropKind::Int)) {
                    a.value.n[0] = fposmod(a.value.n[0] + (v.n[0] - a.init.n[0]) * blend, kPropTau);
                } else {
                    for (int i = 0; i < comps(v.kind); ++i) a.value.n[i] += (v.n[i] - a.init.n[i]) * blend;
                    if (v.kind == OcPropKind::Int) a.value.n[0] = std::trunc(a.value.n[0]);
                }
                break;
            }
            }
        }
    }

    for (u32 id : order) {
        const Acc& a = acc[id];
        if (a.discreteWritten && discrete == PropDiscrete::Dominant) continue;
        PropOutput o;
        o.id = id;
        o.value = a.value;
        outputs_.push_back(std::move(o));
    }
    push(PropEvent::Applied);
}

// ---- root motion ----

void PropPlayer::rootMotionStep(const Inst& in, const OcAnimation& c, const OcPropTrack& t, double blend) {
    PropRootMotion& r = rootMotion_;
    auto at = [&](double time, double* out) {
        const PropSample s = propSample(c, t, time, in.backward);
        if (!s.ok) return false;
        for (int i = 0; i < 4; ++i) out[i] = s.value.n[i];
        return true;
    };
    double cur[4];
    if (!at(in.time, cur)) return;
    // The movement over the segments the pass crossed (a wrapped pass: up to the loop point, then from the start). A seek moves nothing.
    auto each = [&](auto&& fn) {
        if (in.seeked) return;
        for (const PropSegment& s : in.segs) {
            double a[4], b[4];
            if (at(s.a, a) && at(s.b, b)) fn(a, b);
        }
    };
    switch (t.type) {
    case OcPropTrackType::Position3D:
        r.hasPosition = true;
        for (int i = 0; i < 3; ++i) r.positionAccum[i] += cur[i] * blend;
        each([&](const double* a, const double* b) { for (int i = 0; i < 3; ++i) r.position[i] += (b[i] - a[i]) * blend; });
        break;
    case OcPropTrackType::Scale3D:
        r.hasScale = true;
        for (int i = 0; i < 3; ++i) r.scaleAccum[i] += cur[i] * blend;
        each([&](const double* a, const double* b) { for (int i = 0; i < 3; ++i) r.scale[i] += (b[i] - a[i]) * blend; });
        break;
    case OcPropTrackType::Rotation3D: {
        r.hasRotation = true;
        const Q id{0, 0, 0, 1};
        const Q acc = qnorm(qmul(Q{r.rotationAccum[0], r.rotationAccum[1], r.rotationAccum[2], r.rotationAccum[3]}, qslerp(id, qnorm(Q{cur[0], cur[1], cur[2], cur[3]}), blend)));
        r.rotationAccum[0] = acc.x; r.rotationAccum[1] = acc.y; r.rotationAccum[2] = acc.z; r.rotationAccum[3] = acc.w;
        each([&](const double* a, const double* b) {
            const Q d = qmul(qinv(qnorm(Q{a[0], a[1], a[2], a[3]})), qnorm(Q{b[0], b[1], b[2], b[3]}));
            const Q n = qnorm(qmul(Q{r.rotation[0], r.rotation[1], r.rotation[2], r.rotation[3]}, qslerp(id, d, blend)));
            r.rotation[0] = n.x; r.rotation[1] = n.y; r.rotation[2] = n.z; r.rotation[3] = n.w;
        });
        break;
    }
    default: break;
    }
}

// ---- tree mode ----

PropTreeStep PropPlayer::treeAdd(const std::string& key, double prev, double delta, double weight, const std::vector<double>& trackWeights,
                                 bool seeked, bool external, bool includeStart) {
    PropTreeStep st;
    Clip c = clip(key);
    if (!c) return st;
    const double d = seeked ? 0.0 : delta;
    Inst in;
    in.clip = c;
    in.name = key;
    in.weight = weight;
    in.tw = trackWeights;
    in.backward = std::signbit(delta);
    int bounces = 0;
    double next = prev;
    st.looped = stepClip(*c, prev, d, next, in.segs, bounces);
    if (includeStart && !seeked && !in.segs.empty()) in.segs.front().incStart = true;   // the key sitting at the start of a restarted playback fires
    st.flipped = (bounces & 1) != 0;
    st.next = next;
    st.ok = true;
    in.time = next;
    in.delta = d;
    in.seeked = seeked;
    in.external = seeked && external;
    treeInsts_.push_back(std::move(in));
    return st;
}

void PropPlayer::treeMix(bool updateOnly) {
    outputs_.clear();
    std::vector<Inst> insts = std::move(treeInsts_);
    treeInsts_.clear();
    mix(insts, updateOnly, false);
    push(PropEvent::MixerApplied);
}

} // namespace aver::anim
