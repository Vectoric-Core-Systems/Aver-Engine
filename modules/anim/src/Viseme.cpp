// .ocvis lip-sync tracks, the shared envelope function and the clip baker. See Viseme.hpp.
#include "aver/anim/Viseme.hpp"

#include "AnimJson.hpp"
#include "aver/core/CharConv.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace aver::anim {

namespace {

f32 clamp01(f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
f32 smooth(f32 x) { x = clamp01(x); return x * x * (3.0f - 2.0f * x); }

bool parseF32(std::string_view tok, f32& out) {
    f32 v = 0.0f;
    const auto r = fromChars(tok.data(), tok.data() + tok.size(), v);
    if (r.ec != std::errc() || r.ptr != tok.data() + tok.size() || !std::isfinite(v)) return false;
    out = v;
    return true;
}

std::vector<std::string_view> tokens(std::string_view line) {
    std::vector<std::string_view> t;
    usize i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
        const usize b = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') ++i;
        if (i > b) t.push_back(line.substr(b, i - b));
    }
    return t;
}

} // namespace

f32 VisemeTrack::length() const {
    f32 l = 0.0f;
    for (const VisemeEvent& e : events) l = std::max(l, e.end);
    return l;
}

bool VisemeTrack::valid(std::string* why) const {
    const auto fail = [&](const std::string& m) { if (why) *why = m; return false; };
    if (events.size() > kVisemeMaxEvents) return fail("more than 65536 events");
    for (usize i = 0; i < events.size(); ++i) {
        const VisemeEvent& e = events[i];
        if (!std::isfinite(e.start) || !std::isfinite(e.end) || !std::isfinite(e.intensity))
            return fail("event " + std::to_string(i) + " has a non-finite value");
        if (!(e.end > e.start)) return fail("event " + std::to_string(i) + " does not end after it starts");
        if (e.symbol.empty()) return fail("event " + std::to_string(i) + " has no symbol");
    }
    return true;
}

std::string writeVisemeTrack(const VisemeTrack& t) {
    std::string s = "OCVIS " + std::to_string(kVisemeTrackVersion) + "\n";
    s += t.set == VisemeSet::Viseme ? "SET viseme\n" : "SET phoneme\n";
    for (const VisemeEvent& e : t.events) {
        s += "EVENT " + ajson::num(e.start) + " " + ajson::num(e.end) + " " + e.symbol;
        if (e.intensity != 1.0f) s += " " + ajson::num(e.intensity);
        s += "\n";
    }
    return s;
}

bool parseVisemeTrack(std::string_view text, VisemeTrack& out, std::vector<std::string>* diag, std::string* why) {
    VisemeTrack t;
    bool header = false;
    usize lineNo = 0, pos = 0;
    const auto note = [&](const std::string& m) { if (diag) diag->push_back("line " + std::to_string(lineNo) + ": " + m); };
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(pos, nl - pos);
        pos = nl + 1;
        ++lineNo;
        if (const usize h = line.find('#'); h != std::string_view::npos) line = line.substr(0, h);
        const std::vector<std::string_view> tk = tokens(line);
        if (tk.empty()) { if (pos > text.size()) break; continue; }
        if (!header) {
            if (tk[0] != "OCVIS") { if (why) *why = "not an .ocvis file (no OCVIS header)"; return false; }
            f32 ver = 1.0f;
            if (tk.size() < 2 || !parseF32(tk[1], ver)) { if (why) *why = "the OCVIS header has no version"; return false; }
            if (ver > f32(kVisemeTrackVersion)) { if (why) *why = "file version is newer than this build"; return false; }
            header = true;
        } else if (tk[0] == "SET") {
            if (tk.size() >= 2 && tk[1] == "viseme") t.set = VisemeSet::Viseme;
            else if (tk.size() >= 2 && tk[1] == "phoneme") t.set = VisemeSet::Phoneme;
            else note("SET needs phoneme or viseme");
        } else if (tk[0] == "EVENT") {
            VisemeEvent e;
            f32 s = 0, en = 0, in = 1.0f;
            if (tk.size() < 4 || !parseF32(tk[1], s) || !parseF32(tk[2], en)) { note("EVENT needs start end symbol"); }
            else if (tk.size() >= 5 && !parseF32(tk[4], in)) { note("EVENT intensity is not a number"); }
            else if (!(en > s)) { note("EVENT must end after it starts"); }
            else {
                e.start = s; e.end = en; e.symbol = std::string(tk[3]); e.intensity = clamp01(in);
                if (t.events.size() >= kVisemeMaxEvents) { if (why) *why = "more than 65536 events"; return false; }
                t.events.push_back(std::move(e));
            }
        }   // any other record is ignored
        if (pos > text.size()) break;
    }
    if (!header) { if (why) *why = "not an .ocvis file (no OCVIS header)"; return false; }
    std::stable_sort(t.events.begin(), t.events.end(), [](const VisemeEvent& a, const VisemeEvent& b) { return a.start < b.start; });
    out = std::move(t);
    return true;
}

bool saveVisemeTrack(const std::string& path, const VisemeTrack& t, std::string* why) {
    return ajson::writeFile(path, writeVisemeTrack(t), why);
}

bool loadVisemeTrack(const std::string& path, VisemeTrack& out, std::vector<std::string>* diag, std::string* why) {
    std::string text;
    return ajson::readFile(path, text, why) && parseVisemeTrack(text, out, diag, why);
}

u32 resolveVisemeEvents(std::vector<VisemeEvent>& events, const FaceRigAsset* rig, bool visemeOnly,
                        std::vector<std::string>* unknown) {
    u32 resolved = 0;
    std::set<std::string> seen;
    for (VisemeEvent& e : events) {
        const std::string name = resolveVisemeName(rig, e.symbol, visemeOnly);
        if (name.empty()) {
            e.pose = 0;
            if (unknown && seen.insert(e.symbol).second) unknown->push_back(e.symbol);
            continue;
        }
        e.pose = fnv1a64(std::string_view(name));
        ++resolved;
    }
    return resolved;
}

void evalVisemeWeights(const std::vector<VisemeEvent>& events, const LipSyncParams& p, f32 t,
                       std::vector<VisemeSample>& out) {
    out.clear();
    const f32 riseSpan = std::max(p.lead + p.attack, 1e-6f);
    for (const VisemeEvent& e : events) {
        if (e.pose == 0) continue;
        const f32 rise = smooth((t - (e.start - p.lead)) / riseSpan);
        if (rise <= 0.0f) continue;
        const f32 end = std::max(e.end, e.start + p.minHold);
        const f32 fall = p.release > 1e-6f ? 1.0f - smooth((t - end) / p.release) : (t > end ? 0.0f : 1.0f);
        const f32 w = clamp01(e.intensity * rise * fall);
        if (w <= 0.0f) continue;
        bool merged = false;
        for (VisemeSample& s : out)
            if (s.pose == e.pose) { s.weight = std::max(s.weight, w); merged = true; break; }
        if (!merged) out.push_back({e.pose, w});
    }
    f32 sum = 0.0f;
    for (const VisemeSample& s : out) sum += s.weight;
    if (p.maxTotal > 0.0f && sum > p.maxTotal) {
        const f32 k = p.maxTotal / sum;
        for (VisemeSample& s : out) s.weight *= k;
    }
    std::sort(out.begin(), out.end(), [](const VisemeSample& a, const VisemeSample& b) { return a.pose < b.pose; });
}

fmt::OcAnimation bakeVisemeTrack(const VisemeTrack& track, const FaceRigAsset* rig, f32 fps) {
    fmt::OcAnimation clip;
    clip.storage = fmt::OcAnimStorage::Keyframed;
    clip.flags = 0;
    const LipSyncParams params = rig ? rig->lipsync : LipSyncParams{};
    std::vector<VisemeEvent> events = track.events;
    resolveVisemeEvents(events, rig, track.set == VisemeSet::Viseme);

    // The curve names, in order of first appearance.
    std::vector<std::pair<u64, std::string>> names;
    for (const VisemeEvent& e : events) {
        if (e.pose == 0) continue;
        bool have = false;
        for (const auto& n : names) if (n.first == e.pose) { have = true; break; }
        if (!have) names.push_back({e.pose, resolveVisemeName(rig, e.symbol, track.set == VisemeSet::Viseme)});
    }
    if (names.empty()) return clip;

    f32 last = 0.0f;
    for (const VisemeEvent& e : events) if (e.pose != 0) last = std::max(last, std::max(e.end, e.start + params.minHold));
    clip.duration = last + params.release;
    const f32 step = 1.0f / std::max(fps, 1.0f);
    const usize frames = static_cast<usize>(std::ceil(clip.duration / step)) + 1;
    std::vector<f32> times(frames);
    for (usize i = 0; i < frames; ++i) times[i] = std::min(f32(i) * step, clip.duration);

    std::vector<std::vector<f32>> values(names.size(), std::vector<f32>(frames, 0.0f));
    std::vector<VisemeSample> samples;
    for (usize i = 0; i < frames; ++i) {
        evalVisemeWeights(events, params, times[i], samples);
        for (const VisemeSample& s : samples)
            for (usize n = 0; n < names.size(); ++n)
                if (names[n].first == s.pose) { values[n][i] = s.weight; break; }
    }

    for (usize n = 0; n < names.size(); ++n) {
        const std::vector<f32>& v = values[n];
        // Greedy simplification: extend a straight segment from the last kept key while every key it skips
        // stays within 1e-3 of the line.
        std::vector<usize> keep{0};
        usize a = 0;
        for (usize b = 2; b < frames; ++b) {
            bool fits = true;
            for (usize j = a + 1; j < b && fits; ++j) {
                const f32 u = (times[j] - times[a]) / std::max(times[b] - times[a], 1e-9f);
                fits = std::fabs(v[j] - (v[a] + (v[b] - v[a]) * u)) <= 1e-3f;
            }
            if (!fits) { keep.push_back(b - 1); a = b - 1; }
        }
        if (frames > 1) keep.push_back(frames - 1);
        fmt::OcCurve c;
        c.name = std::string(fmt::kOcCurvePrefixPose) + names[n].second;
        c.interp = fmt::OcInterp::Linear;
        c.poseId = names[n].first;
        for (const usize k : keep) { c.times.push_back(times[k]); c.values.push_back(v[k]); }
        clip.curves.push_back(std::move(c));
    }
    return clip;
}

} // namespace aver::anim
