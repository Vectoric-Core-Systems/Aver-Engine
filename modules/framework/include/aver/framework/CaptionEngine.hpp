#pragma once
// Caption runtime: matches played sounds (or explicit shows) to .occaptions cues, keeps the active ones on a
// clock, and produces the visible lines (priority, line cap, speaker change, colours, direction hints, fade).
// Header-only, no I/O, no audio, no wall clock: the host feeds dt and an injected voice probe. Text stays as raw
// localization references; the UI tree resolves them. docs/ACCESSIBILITY.md section 2.2.

#include "aver/core/Types.hpp"
#include "aver/formats/OcCaptions.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aver::fw::access {

struct CaptionSoundStart {
    std::string path;
    i32 voice = 0, bus = 0;
    bool positional = false, looping = false;
    f32 pos[3] = {0, 0, 0};
};

struct CaptionListener {
    f32 pos[3] = {0, 0, 0}, forward[3] = {1, 0, 0}, right[3] = {0, 1, 0};
};

struct CaptionOptions {
    i32 mode = 1;          // 0 off, 1 subtitles (dialogue only), 2 captions (all)
    i32 maxLines = 3;
    bool speaker = true, colours = true, direction = false, reduceMotion = false;
};

struct CaptionOutLine {
    std::string speakerRef, textRef;   // raw references; speakerRef is empty when the speaker is not shown
    u32 rgba = 0xFFFFFFFFu;            // 0xRRGGBBAA
    i32 kind = 1;                      // OcCaptionKind
    i32 dir = 0;                       // -1 left, 0 none, 1 right, 2 behind
    f32 alpha = 1.0f;
    i32 priority = 0;
    u64 id = 0;                        // stable while the line stays visible
};

class CaptionEngine {
public:
    using VoiceProbe = std::function<bool(i32 voice)>;

    static constexpr f32 kTail = 0.35f;         // a voice-bound line outlives its voice by this long
    static constexpr f32 kFade = 0.25f;
    static constexpr f32 kDialogueDwell = 0.8f;
    static constexpr usize kMaxActive = 32;

    void setVoiceProbe(VoiceProbe p) { probe_ = std::move(p); }
    void setOptions(const CaptionOptions& o) { opt_ = o; if (opt_.maxLines < 1) opt_.maxLines = 1; }
    const CaptionOptions& options() const { return opt_; }

    // Replaces the cue set. A duplicate cue id: the later one wins. Actives whose cue still exists are refreshed
    // from the new data, the rest are dropped.
    void load(const fmt::OcCaptionData& d) {
        cues_.clear();
        index_.clear();
        speakers_.clear();
        duplicates_ = 0;
        for (const fmt::OcCaptionSpeaker& s : d.speakers) speakers_[s.id] = s;
        for (const fmt::OcCaptionCue& c : d.cues) {
            const auto it = index_.find(c.id);
            if (it != index_.end()) { cues_[it->second] = c; ++duplicates_; }
            else { index_[c.id] = cues_.size(); cues_.push_back(c); }
        }
        for (usize i = active_.size(); i-- > 0;) {
            Active& a = active_[i];
            if (a.cueId.empty()) continue;
            const auto it = index_.find(a.cueId);
            if (it == index_.end()) { active_.erase(active_.begin() + static_cast<isize>(i)); continue; }
            refresh(a, cues_[it->second]);
        }
    }
    void clear() { cues_.clear(); index_.clear(); speakers_.clear(); active_.clear(); lines_.clear(); new_.clear(); prevIds_.clear(); duplicates_ = 0; }
    void unload(const std::vector<std::string>& cueIds) {
        for (const std::string& id : cueIds) {
            stop(id);
            const auto it = index_.find(id);
            if (it == index_.end()) continue;
            cues_.erase(cues_.begin() + static_cast<isize>(it->second));
            index_.clear();
            for (usize i = 0; i < cues_.size(); ++i) index_[cues_[i].id] = i;
        }
    }
    usize cueCount() const { return cues_.size(); }
    usize duplicateCount() const { return duplicates_; }
    usize activeCount() const { return active_.size(); }
    const fmt::OcCaptionCue* findCue(const std::string& id) const {
        const auto it = index_.find(id);
        return it == index_.end() ? nullptr : &cues_[it->second];
    }
    // The cue a played path raises: exact path first, then patterns; highest priority, ties by file order.
    const fmt::OcCaptionCue* matchSound(const std::string& path) const {
        const fmt::OcCaptionCue* bestExact = nullptr;
        const fmt::OcCaptionCue* bestPat = nullptr;
        for (const fmt::OcCaptionCue& c : cues_) {
            bool exact = false, pat = false;
            for (const std::string& p : c.sounds) {
                if (fmt::captionPatternExact(p, path)) exact = true;
                else if (fmt::captionPatternMatches(p, path)) pat = true;
            }
            if (exact && (!bestExact || c.priority > bestExact->priority)) bestExact = &c;
            if (pat && (!bestPat || c.priority > bestPat->priority)) bestPat = &c;
        }
        return bestExact ? bestExact : bestPat;
    }

    // A sound started. True when a caption was raised.
    bool onSound(const CaptionSoundStart& s) {
        if (opt_.mode <= 0 || s.path.empty()) return false;
        const fmt::OcCaptionCue* c = matchSound(s.path);
        if (!c) return false;
        return start(*c, s.voice, s.positional, s.pos);
    }
    // Script / graph / bus: a cue by id. `pos` null = not positional.
    bool show(const std::string& cueId, const f32* pos = nullptr) {
        const fmt::OcCaptionCue* c = findCue(cueId);
        if (!c) return false;
        return start(*c, 0, pos != nullptr, pos ? pos : kZero);
    }
    // An ad-hoc line with no cue. Both texts may be references.
    bool showText(const std::string& speakerRef, const std::string& textRef, f32 seconds, u32 rgba, i32 kind) {
        if (textRef.empty() || !allowed(kind)) return false;
        Active a;
        a.seq = ++seq_;
        a.kind = kind;
        a.hold = seconds > 0 ? seconds : 3.0f;
        a.adHocSpeaker = speakerRef;
        a.adHocRgba = rgba;
        Line l;
        l.text = textRef;
        l.dur = a.hold;
        a.lines.push_back(std::move(l));
        add(std::move(a));
        return true;
    }
    // Empty cueId = every caption.
    void stop(const std::string& cueId) {
        for (usize i = active_.size(); i-- > 0;)
            if (cueId.empty() || active_[i].cueId == cueId) active_.erase(active_.begin() + static_cast<isize>(i));
    }

    // dt is the UNSCALED frame time: captions follow audio, which keeps playing while the game is paused.
    void update(f32 dt, const CaptionListener& listener) {
        if (!(dt > 0)) dt = 0;
        now_ += dt;
        listener_ = listener;
        haveListener_ = true;
        for (usize i = active_.size(); i-- > 0;) {
            Active& a = active_[i];
            a.t += dt;
            if (a.voice != 0 && probe_ && !a.ended && !probe_(a.voice)) { a.ended = true; a.endT = a.t; }
            if (a.t >= activeEnd(a)) active_.erase(active_.begin() + static_cast<isize>(i));
        }
        build();
    }

    const std::vector<CaptionOutLine>& lines() const { return lines_; }
    const std::vector<CaptionOutLine>& newLines() const { return new_; }

private:
    struct Line { f32 start = 0, dur = 0; std::string text, speaker; };
    struct Active {
        std::string cueId;
        std::vector<Line> lines;
        i32 kind = 1, priority = 0, voice = 0;
        f32 t = 0, hold = 1.5f, endT = 0;
        bool ended = false, positional = false;
        f32 pos[3] = {0, 0, 0};
        u64 seq = 0;
        std::string cueSpeaker, adHocSpeaker;
        u32 adHocRgba = 0xFFFFFFFFu;
    };

    static constexpr f32 kZero[3] = {0, 0, 0};
    static constexpr f32 kInf = 1.0e30f;

    bool allowed(i32 kind) const {
        if (opt_.mode <= 0) return false;
        return opt_.mode >= 2 || kind == static_cast<i32>(fmt::OcCaptionKind::Dialogue);
    }

    void refresh(Active& a, const fmt::OcCaptionCue& c) const {
        a.lines.clear();
        for (const fmt::OcCaptionLine& l : fmt::captionCueLines(c)) a.lines.push_back(Line{l.start, l.dur, l.text, l.speaker});
        a.kind = static_cast<i32>(c.kind);
        a.priority = c.priority;
        a.hold = fmt::captionCueHold(c);
        a.cueSpeaker = c.speaker;
    }

    bool start(const fmt::OcCaptionCue& c, i32 voice, bool positional, const f32* pos) {
        const i32 kind = static_cast<i32>(c.kind);
        if (!allowed(kind)) return false;
        if (positional && c.radius > 0 && haveListener_) {
            const f32 dx = pos[0] - listener_.pos[0], dy = pos[1] - listener_.pos[1], dz = pos[2] - listener_.pos[2];
            if (dx * dx + dy * dy + dz * dz > c.radius * c.radius) return false;
        }
        const auto cd = lastFired_.find(c.id);
        if (cd != lastFired_.end() && now_ - cd->second < c.cooldown) return false;
        lastFired_[c.id] = now_;

        Active a;
        a.cueId = c.id;
        a.voice = voice;
        a.positional = positional;
        if (positional) for (int k = 0; k < 3; ++k) a.pos[k] = pos[k];
        refresh(a, c);
        for (Active& e : active_) {
            if (e.cueId != c.id) continue;   // the same cue again restarts in place instead of stacking
            const u64 seq = e.seq;
            a.seq = seq;
            e = std::move(a);
            return true;
        }
        a.seq = ++seq_;
        add(std::move(a));
        return true;
    }

    void add(Active a) {
        active_.push_back(std::move(a));
        while (active_.size() > kMaxActive) {
            usize victim = 0;
            for (usize i = 1; i < active_.size(); ++i)
                if (active_[i].priority < active_[victim].priority ||
                    (active_[i].priority == active_[victim].priority && active_[i].seq < active_[victim].seq)) victim = i;
            active_.erase(active_.begin() + static_cast<isize>(victim));
        }
    }

    f32 dwell(const Active& a) const { return a.kind == static_cast<i32>(fmt::OcCaptionKind::Dialogue) ? kDialogueDwell : a.hold; }
    bool voiceBound(const Active& a) const { return a.voice != 0 && probe_; }

    // False when the voice ended before the line began (the cue was cut short).
    bool lineLive(const Active& a, const Line& l) const { return !(voiceBound(a) && a.ended && l.start > a.endT + kTail); }

    // When the line leaves the screen (seconds from the cue start); kInf while its voice plays.
    f32 lineEnd(const Active& a, const Line& l) const {
        const bool vb = voiceBound(a);
        if (l.dur > 0) {
            const f32 natural = l.start + l.dur;
            if (vb && a.ended && l.start <= a.endT && natural > a.endT)   // cut short: keep it a short dwell
                return std::min(natural, std::max(a.endT + kTail, l.start + dwell(a)));
            return natural;
        }
        if (!vb) return l.start + a.hold;
        if (!a.ended) return kInf;
        return std::max(a.endT + kTail, l.start + a.hold);
    }

    f32 activeEnd(const Active& a) const {
        f32 e = 0;
        for (const Line& l : a.lines) if (lineLive(a, l)) e = std::max(e, lineEnd(a, l));
        if (voiceBound(a) && !a.ended) e = kInf;   // never drop a cue under its playing voice
        return e;
    }

    i32 directionOf(const Active& a) const {
        const f32 dx = a.pos[0] - listener_.pos[0], dy = a.pos[1] - listener_.pos[1];
        const f32 fwd = dx * listener_.forward[0] + dy * listener_.forward[1];
        const f32 rgt = dx * listener_.right[0] + dy * listener_.right[1];
        if (fwd == 0 && rgt == 0) return 0;
        const f32 az = std::atan2(rgt, fwd) * (180.0f / 3.14159265358979f);
        if (std::fabs(az) > 135.0f) return 2;
        if (az < -35.0f) return -1;
        if (az > 35.0f) return 1;
        return 0;
    }

    struct Vis { const Active* a; usize li; i32 kind, priority; u64 seq; std::string speakerId; };

    void build() {
        lines_.clear();
        new_.clear();
        std::vector<Vis> vis;
        for (const Active& a : active_) {
            if (!allowed(a.kind)) continue;
            for (usize li = 0; li < a.lines.size(); ++li) {
                const Line& l = a.lines[li];
                if (!lineLive(a, l) || a.t < l.start || a.t >= lineEnd(a, l)) continue;
                Vis v;
                v.a = &a; v.li = li; v.kind = a.kind; v.priority = a.priority; v.seq = a.seq;
                v.speakerId = !l.speaker.empty() ? l.speaker : a.cueSpeaker;
                vis.push_back(std::move(v));
            }
        }
        // Line cap: evict the lowest priority, then the oldest; dialogue only goes when nothing else is left.
        const usize cap = static_cast<usize>(opt_.maxLines);
        while (vis.size() > cap) {
            usize victim = vis.size();
            const auto worse = [&](usize x, usize y) {
                return vis[x].priority != vis[y].priority ? vis[x].priority < vis[y].priority : vis[x].seq < vis[y].seq;
            };
            const i32 dlg = static_cast<i32>(fmt::OcCaptionKind::Dialogue);
            for (int pass = 0; pass < 2 && victim == vis.size(); ++pass)
                for (usize i = 0; i < vis.size(); ++i) {
                    if ((vis[i].kind == dlg) != (pass == 1)) continue;
                    if (victim == vis.size() || worse(i, victim)) victim = i;
                }
            vis.erase(vis.begin() + static_cast<isize>(victim));
        }
        std::stable_sort(vis.begin(), vis.end(), [](const Vis& x, const Vis& y) {
            if (x.priority != y.priority) return x.priority > y.priority;
            if (x.seq != y.seq) return x.seq < y.seq;
            return x.li < y.li;
        });

        std::unordered_set<u64> ids;
        std::string prevSpeaker;
        bool first = true;
        for (const Vis& v : vis) {
            const Active& a = *v.a;
            const Line& l = a.lines[v.li];
            CaptionOutLine o;
            o.textRef = l.text;
            o.kind = a.kind;
            o.priority = a.priority;
            o.id = (a.seq << 8) | static_cast<u64>(v.li & 0xFF);
            std::string spkId = v.speakerId;
            std::string spkName;
            u32 rgba = 0xFFFFFFFFu;
            if (a.cueId.empty()) {   // ad-hoc line (showText)
                spkId = a.adHocSpeaker;
                spkName = a.adHocSpeaker;
                rgba = a.adHocRgba;
            } else if (!spkId.empty()) {
                const auto it = speakers_.find(spkId);
                if (it != speakers_.end()) { spkName = it->second.name; rgba = it->second.rgba; }
                else spkName = spkId;
            }
            if (opt_.speaker && !spkId.empty() && (first || spkId != prevSpeaker)) o.speakerRef = spkName;
            o.rgba = opt_.colours ? rgba : 0xFFFFFFFFu;
            if (opt_.direction && a.positional) o.dir = directionOf(a);
            const f32 end = lineEnd(a, l);
            o.alpha = (opt_.reduceMotion || end >= kInf) ? 1.0f : std::max(0.0f, std::min(1.0f, (end - a.t) / kFade));
            prevSpeaker = spkId;
            first = false;
            ids.insert(o.id);
            if (!prevIds_.count(o.id)) new_.push_back(o);
            lines_.push_back(std::move(o));
        }
        prevIds_ = std::move(ids);
    }

    CaptionOptions opt_;
    VoiceProbe probe_;
    std::vector<fmt::OcCaptionCue> cues_;
    std::unordered_map<std::string, usize> index_;
    std::unordered_map<std::string, fmt::OcCaptionSpeaker> speakers_;
    std::vector<Active> active_;
    std::unordered_map<std::string, f32> lastFired_;
    std::vector<CaptionOutLine> lines_, new_;
    std::unordered_set<u64> prevIds_;
    CaptionListener listener_;
    bool haveListener_ = false;
    f32 now_ = 0;
    u64 seq_ = 0;
    usize duplicates_ = 0;
};

} // namespace aver::fw::access
