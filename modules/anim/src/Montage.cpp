// Montage asset checks and the player. See Montage.hpp.
#include "aver/anim/Montage.hpp"
#include "aver/anim/AnimSampler.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace aver::anim {

namespace {

f32 clamp01(f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

f32 applyCurve(BlendCurve c, f32 a) {
    a = clamp01(a);
    return c == BlendCurve::Smooth ? a * a * (3.0f - 2.0f * a) : a;
}

} // namespace

bool MontageAsset::valid(std::string* why) const {
    auto fail = [&](const std::string& m) { if (why) *why = m; return false; };
    if (segments.empty()) return fail("a montage needs at least one segment");
    for (const MontageSegment& s : segments) {
        if (s.clip.empty()) return fail("a segment has no clip");
        if (!(s.rate > 0.0f) || !std::isfinite(s.rate)) return fail("a segment rate must be positive");
        if (!std::isfinite(s.start) || s.start < 0.0f) return fail("a segment start must be >= 0");
        if (!std::isfinite(s.end) || !(s.end < 0.0f || s.end > s.start)) return fail("a segment end must be after its start");
    }
    if (!std::isfinite(blendIn) || !std::isfinite(blendOut) || blendIn < 0.0f || blendOut < 0.0f)
        return fail("blend times must be >= 0");
    if (!std::isfinite(blendOutTrigger)) return fail("blendOutTrigger is not finite");
    if (!std::isfinite(rate) || rate < 0.0f) return fail("the montage rate must be >= 0");

    std::set<std::string> names;
    f32 first = 0.0f;
    bool any = false;
    for (const MontageSection& s : sections) {
        if (s.name.empty() || !names.insert(s.name).second) return fail("section names must be unique and non-empty");
        if (!std::isfinite(s.start)) return fail("a section start is not finite");
        first = any ? std::min(first, s.start) : s.start;
        any = true;
    }
    if (any && first != 0.0f) return fail("the first section must start at 0");
    for (const MontageSection& s : sections)
        if (!s.next.empty() && s.next != kMontageEnd && names.find(s.next) == names.end())
            return fail("section '" + s.name + "' links to unknown section '" + s.next + "'");
    for (const MontageNotify& n : notifies) {
        if (!std::isfinite(n.time)) return fail("a notify time is not finite");
        if (!std::isfinite(n.duration) || n.duration < 0.0f) return fail("a notify duration must be >= 0");
    }
    return true;
}

i32 MontageAsset::sectionIndex(std::string_view n) const {
    for (usize i = 0; i < sections.size(); ++i)
        if (sections[i].name == n) return static_cast<i32>(i);
    return -1;
}

bool MontagePlayer::bind(const MontageAsset* asset, MontageClipFn clips) {
    closeStates();   // while the previous asset is still the one the open flags index
    asset_ = nullptr;
    segs_.clear();
    secs_.clear();
    override_.clear();
    noteTimes_.clear();
    open_.clear();
    length_ = pos_ = 0.0f;
    cur_ = 0;
    a_ = 0.0f;
    state_ = State::Idle;
    additive_ = rootMotion_ = atEnd_ = inclusive_ = notifying_ = refReady_ = driven_ = false;
    rootAcc_ = RootDelta{};
    if (!asset) return false;

    bool any = false;
    f32 t = 0.0f;
    for (const MontageSegment& ms : asset->segments) {
        Seg s;
        s.clip = clips ? clips(ms.clip) : nullptr;
        s.rate = ms.rate > 0.0f ? ms.rate : 1.0f;
        if (s.clip) {
            any = true;
            const f32 dur = s.clip->duration;
            s.ce = ms.end < 0.0f ? dur : (dur > 0.0f ? std::min(ms.end, dur) : ms.end);
            s.cs = std::min(std::max(ms.start, 0.0f), s.ce);
        } else {
            // An unresolved clip with an open end has no known length; one with an explicit end still
            // occupies its time (silently, with no pose).
            s.cs = std::max(ms.start, 0.0f);
            s.ce = ms.end < 0.0f ? s.cs : ms.end;
        }
        s.len = std::max(0.0f, (s.ce - s.cs) / s.rate);
        s.ms = t;
        t += s.len;
        segs_.push_back(s);
    }
    if (!any || !(t > 0.0f)) { segs_.clear(); return false; }
    length_ = t;
    asset_ = asset;

    std::vector<MontageSection> sorted = asset->sections;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const MontageSection& a, const MontageSection& b) { return a.start < b.start; });
    if (sorted.empty()) sorted.push_back({"Default", 0.0f, ""});
    for (usize i = 0; i < sorted.size(); ++i) {
        Sec s;
        s.name = sorted[i].name;
        s.start = i == 0 ? 0.0f : std::min(std::max(sorted[i].start, 0.0f), length_);
        secs_.push_back(std::move(s));
    }
    for (usize i = 0; i < secs_.size(); ++i) {
        secs_[i].end = i + 1 < secs_.size() ? secs_[i + 1].start : length_;
        const std::string& nx = sorted[i].next;
        secs_[i].link = nx.empty() ? kFollow : nx == kMontageEnd ? kEnd : kFollow;
        if (!nx.empty() && nx != kMontageEnd)
            for (usize k = 0; k < sorted.size(); ++k)
                if (sorted[k].name == nx) { secs_[i].link = static_cast<i32>(k); break; }
    }
    override_.assign(secs_.size(), kNoOverride);

    for (const MontageNotify& n : asset->notifies) noteTimes_.push_back(n.time);
    open_.assign(asset->notifies.size(), 0);

    if (asset->additive >= 0) {
        additive_ = asset->additive == 1;
    } else {
        for (const Seg& s : segs_)
            if (s.clip) { additive_ = (s.clip->flags & fmt::kOcAnimAdditiveBase) != 0; break; }
    }
    refreshRootFlag();
    return true;
}

void MontagePlayer::refreshRootFlag() {
    rootMotion_ = false;
    if (!hasSpec_) return;
    for (const Seg& s : segs_)
        if (s.clip && clipHasRootMotion(*s.clip, spec_)) { rootMotion_ = true; return; }
}

void MontagePlayer::setRootMotion(const RootSpec* spec) {
    hasSpec_ = spec && spec->valid();
    if (hasSpec_) spec_ = *spec;
    refreshRootFlag();
}

void MontagePlayer::raise(MontageEvent::Kind k, const std::string& name) {
    events_.push_back({k, name});
}

void MontagePlayer::closeStates() {
    if (!asset_) return;
    for (usize i = 0; i < open_.size() && i < asset_->notifies.size(); ++i) {
        if (!open_[i]) continue;
        open_[i] = 0;
        raise(MontageEvent::Kind::NotifyEnd, asset_->notifies[i].name);
    }
}

void MontagePlayer::finishEnded() {
    closeStates();
    a_ = 0.0f;
    state_ = State::Done;
    raise(MontageEvent::Kind::Ended);
}

void MontagePlayer::beginBlendOut(f32 seconds) {
    state_ = State::Out;
    outTime_ = seconds;
    raise(MontageEvent::Kind::BlendOut);
    if (seconds <= 0.0f) finishEnded();
}

void MontagePlayer::start(i32 section, f32 rate, f32 blendInOverride) {
    if (!asset_ || secs_.empty()) return;
    if (state_ != State::Idle && state_ != State::Done) {
        closeStates();
        if (state_ != State::Out) raise(MontageEvent::Kind::Interrupted);
        raise(MontageEvent::Kind::Ended);
    }
    rate_ = std::max(rate, 0.0f);
    cur_ = (section >= 0 && static_cast<usize>(section) < secs_.size()) ? section : 0;
    pos_ = secs_[static_cast<usize>(cur_)].start;
    inTime_ = blendInOverride >= 0.0f ? blendInOverride : asset_->blendIn;
    a_ = inTime_ > 0.0f ? 0.0f : 1.0f;
    state_ = inTime_ > 0.0f ? State::In : State::Hold;
    std::fill(open_.begin(), open_.end(), u8{0});
    std::fill(override_.begin(), override_.end(), kNoOverride);
    inclusive_ = true;
    atEnd_ = false;
    notifying_ = true;
    rootAcc_ = RootDelta{};
    raise(MontageEvent::Kind::Started);
    raise(MontageEvent::Kind::SectionStarted, secs_[static_cast<usize>(cur_)].name);
    checkAutoBlendOut();
}

void MontagePlayer::stop(f32 blendOutOverride) {
    if (!asset_ || state_ == State::Idle || state_ == State::Done) return;
    if (state_ == State::Out) {
        if (blendOutOverride == 0.0f) finishEnded();
        return;
    }
    closeStates();
    notifying_ = false;
    raise(MontageEvent::Kind::Interrupted);
    beginBlendOut(blendOutOverride >= 0.0f ? blendOutOverride : asset_->blendOut);
}

void MontagePlayer::finish() {
    if (!asset_ || state_ == State::Idle || state_ == State::Done) return;
    finishEnded();
}

void MontagePlayer::enterSection(i32 idx) {
    cur_ = idx;
    const f32 st = secs_[static_cast<usize>(idx)].start;
    // A jump that does not land where the clock already is includes its own start point, so a notify
    // sitting exactly at the section start fires on every loop.
    if (st != pos_) inclusive_ = true;
    pos_ = st;
    raise(MontageEvent::Kind::SectionStarted, secs_[static_cast<usize>(idx)].name);
}

bool MontagePlayer::jumpToSection(i32 section) {
    if (!asset_ || state_ == State::Idle || state_ == State::Done) return false;
    if (section < 0 || static_cast<usize>(section) >= secs_.size()) return false;
    atEnd_ = false;
    inclusive_ = true;
    cur_ = section;
    pos_ = secs_[static_cast<usize>(section)].start;
    raise(MontageEvent::Kind::SectionStarted, secs_[static_cast<usize>(section)].name);
    closeOutside();
    return true;
}

bool MontagePlayer::setNextSection(i32 from, i32 to) {
    if (from < 0 || static_cast<usize>(from) >= secs_.size()) return false;
    if (to >= 0 && static_cast<usize>(to) >= secs_.size()) return false;
    override_[static_cast<usize>(from)] = to < 0 ? kNoOverride : to;
    return true;
}

void MontagePlayer::setRate(f32 r) { rate_ = std::max(r, 0.0f); }

void MontagePlayer::drive(f32 position, f32 w) {
    if (!asset_ || secs_.empty()) return;
    driven_ = true;
    events_.clear();
    notifying_ = atEnd_ = inclusive_ = false;
    rootAcc_ = RootDelta{};
    pos_ = std::min(std::max(position, 0.0f), length_);
    cur_ = 0;
    for (usize i = 0; i < secs_.size(); ++i)
        if (secs_[i].start <= pos_) cur_ = static_cast<i32>(i);
    a_ = clamp01(w);
    state_ = State::Hold;
}

i32 MontagePlayer::nextOf(i32 cur) const {
    const usize c = static_cast<usize>(cur);
    const i32 link = override_[c] != kNoOverride ? override_[c] : secs_[c].link;
    if (link == kEnd) return kEnd;
    if (link == kFollow) return static_cast<usize>(cur) + 1 < secs_.size() ? cur + 1 : kEnd;
    return link;
}

void MontagePlayer::checkAutoBlendOut() {
    if (state_ != State::In && state_ != State::Hold) return;
    if (nextOf(cur_) != kEnd) return;
    const Sec& s = secs_[static_cast<usize>(cur_)];
    f32 trig = asset_->blendOutTrigger >= 0.0f ? asset_->blendOutTrigger : asset_->blendOut;
    trig = std::min(trig, s.end - s.start);
    if (s.end - pos_ <= trig) beginBlendOut(asset_->blendOut);
}

void MontagePlayer::advance(f32 dt) {
    if (!asset_ || state_ == State::Idle || state_ == State::Done) return;

    // The weight runs on real time, the position on montage time. A blend-out that starts this tick
    // begins fading on the next one.
    if (state_ == State::In) {
        a_ += inTime_ > 0.0f ? dt / inTime_ : 1.0f;
        if (a_ >= 1.0f) { a_ = 1.0f; state_ = State::Hold; }
    } else if (state_ == State::Out) {
        a_ -= outTime_ > 0.0f ? dt / outTime_ : 1.0f;
        if (a_ <= 0.0f) { finishEnded(); return; }
    }
    if (atEnd_) return;
    travel(dt * asset_->rate * rate_);
}

void MontagePlayer::travel(f32 amount) {
    f32 remaining = amount;
    const usize maxHops = secs_.size() + 1;
    for (usize hops = 0;; ++hops) {
        const Sec& sec = secs_[static_cast<usize>(cur_)];
        const f32 target = pos_ + remaining;
        if (target < sec.end) {
            moveTo(target);
            checkAutoBlendOut();
            return;
        }
        const f32 over = target - sec.end;
        moveTo(sec.end);
        const i32 nxt = nextOf(cur_);
        if (nxt == kEnd) {
            atEnd_ = true;
            closeStates();
            if (state_ != State::Out) beginBlendOut(asset_->blendOut);
            return;
        }
        if (hops >= maxHops) return;   // a loop of empty sections: stop rather than spin
        enterSection(nxt);
        remaining = over;
    }
}

void MontagePlayer::moveTo(f32 np) {
    const f32 p0 = pos_;
    const bool inc = inclusive_;
    inclusive_ = false;
    if (!(np > p0) && !inc) { pos_ = np; return; }

    if (notifying_ && !noteTimes_.empty()) {
        ClipStep step;
        step.prev = p0;
        step.now = np;
        step.inclusiveStart = inc;
        crossed_.clear();
        notifiesCrossedTimes(noteTimes_, length_, step, crossed_);
        for (const u32 i : crossed_) {
            const MontageNotify& n = asset_->notifies[i];
            if (!(n.duration > 0.0f)) { raise(MontageEvent::Kind::Notify, n.name); continue; }
            if (!open_[i]) { open_[i] = 1; raise(MontageEvent::Kind::NotifyBegin, n.name); }
        }
    }

    // Segment overlap with (p0, np]: clip-embedded instant notifies and root motion. A segment entered
    // inside this interval includes its own first instant, so a notify at a segment boundary fires once.
    for (usize si = 0; si < segs_.size(); ++si) {
        const Seg& s = segs_[si];
        if (!s.clip || !(s.len > 0.0f)) continue;
        const f32 e = s.ms + s.len;
        const f32 lo = std::max(p0, s.ms), hi = std::min(np, e);
        if (lo > hi) continue;
        const f32 c0 = s.cs + (lo - s.ms) * s.rate, c1 = s.cs + (hi - s.ms) * s.rate;
        if (notifying_ && !s.clip->notifies.empty()) {
            ClipStep step;
            step.prev = c0;
            step.now = c1;
            step.inclusiveStart = lo > p0 || inc;
            crossed_.clear();
            notifiesCrossedIn(s.clip->notifies, s.clip->duration, step, crossed_);
            const bool endsHere = hi >= e && si + 1 < segs_.size();
            for (const u32 i : crossed_) {
                if (s.clip->notifyDuration(i) > 0.0f) continue;   // clip-embedded states are ignored in v1
                // A segment plays [start, end): the instant it ends belongs to the next one, which
                // includes its own start, so a notify on a shared boundary fires once.
                if (endsHere && s.clip->notifies[i].time >= s.ce) continue;
                raise(MontageEvent::Kind::Notify, s.clip->notifies[i].name);
            }
        }
        if (hasSpec_ && hi > lo && clipHasRootMotion(*s.clip, spec_))
            composeDelta(rootAcc_, rootDeltaBetween(*s.clip, spec_, c0, c1, false));
    }
    pos_ = np;
    closeOutside();
}

// A notify state is open only while the position is inside [begin, begin + duration).
void MontagePlayer::closeOutside() {
    for (usize i = 0; i < open_.size(); ++i) {
        if (!open_[i]) continue;
        const MontageNotify& n = asset_->notifies[i];
        const f32 b = std::min(std::max(n.time, 0.0f), length_);
        const f32 e = std::min(b + n.duration, length_);
        if (pos_ >= b && pos_ < e) continue;
        open_[i] = 0;
        raise(MontageEvent::Kind::NotifyEnd, n.name);
    }
}

f32 MontagePlayer::weight() const {
    if (state_ == State::Idle || state_ == State::Done || !asset_) return 0.0f;
    if (driven_) return clamp01(a_);
    return applyCurve(state_ == State::Out ? asset_->outCurve : asset_->inCurve, a_);
}

const std::string& MontagePlayer::sectionName(i32 i) const {
    static const std::string none;
    return (i >= 0 && static_cast<usize>(i) < secs_.size()) ? secs_[static_cast<usize>(i)].name : none;
}

void MontagePlayer::evaluate(const fmt::OcSkeleton& skel, const Pose& base, Pose& out) const {
    if (&out != &base) out = base;
    if (!asset_ || segs_.empty() || !base.matches(skel)) return;

    const Seg* seg = nullptr;
    for (const Seg& s : segs_) {
        if (!(s.len > 0.0f)) continue;
        seg = &s;
        if (pos_ < s.ms + s.len) break;
    }
    if (!seg || !seg->clip) return;
    const f32 c = seg->cs + std::min(std::max(pos_ - seg->ms, 0.0f), seg->len) * seg->rate;

    if (additive_) {
        if (!refReady_ || ref_.local.size() != skel.bones.size()) {
            restPose(skel, ref_);
            for (const Seg& s : segs_)
                if (s.clip) { sampleAnimation(*s.clip, s.cs, ref_); break; }
            refReady_ = true;
        }
        restPose(skel, sampled_);
        sampleAnimation(*seg->clip, c, sampled_);
        // `out` may alias `base`; addPose copies base into its output first, so go through a temp.
        Pose result;
        addPose(base, sampled_, ref_, 1.0f, result);
        out.swap(result);
        return;
    }
    sampleAnimation(*seg->clip, c, out);
    if (hasSpec_ && clipHasRootMotion(*seg->clip, spec_)) lockRoot(out, spec_);
}

void MontagePlayer::drainEvents(std::vector<MontageEvent>& out) {
    for (MontageEvent& e : events_) out.push_back(std::move(e));
    events_.clear();
}

RootDelta MontagePlayer::consumeRootDelta() {
    const RootDelta d = rootAcc_;
    rootAcc_ = RootDelta{};
    return d;
}

} // namespace aver::anim
