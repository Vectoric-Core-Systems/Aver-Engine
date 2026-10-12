// Level-sequence records and the .ocseq file. See OcSeq.hpp.
#include "aver/formats/OcSeq.hpp"
#include "aver/core/CharConv.hpp"   // floating from_chars where libc++ has none
#include "aver/core/Log.hpp"
#include "aver/formats/detail/TextEscape.hpp"
#include "aver/formats/detail/TextScan.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace aver::fmt {
using namespace aver::fmt::detail;

namespace {

// A token that is entirely one number (parseF64 would take "12abc" as 12; the run must stop at a word).
bool asNumber(std::string_view s, f64& v) {
    if (s.empty()) return false;
    const auto r = aver::fromChars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

// %.6g, widened to %.9g when six digits lose more than 1e-6 relative (a time of 1234.567 survives; every
// value a file can already hold writes as before).
std::string num(f64 v) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    if (std::fabs(std::strtod(buf, nullptr) - v) > 1e-6 * std::max(1.0, std::fabs(v)))
        std::snprintf(buf, sizeof buf, "%.9g", v);
    return buf;
}

// A fresh key of this kind: the OcSeqKey defaults, except that a visibility key applies to descendants.
OcSeqKey freshKey(OcSeqTrackKind k) {
    OcSeqKey key;
    if (k == OcSeqTrackKind::Visibility) key.v[1] = 1;
    return key;
}

bool kindFromName(std::string_view n, OcSeqTrackKind& k) {
    static const struct { const char* name; OcSeqTrackKind kind; } kTable[] = {
        {"transform", OcSeqTrackKind::Transform}, {"camera", OcSeqTrackKind::Camera},
        {"material", OcSeqTrackKind::Material},   {"property", OcSeqTrackKind::Property},
        {"event", OcSeqTrackKind::Event},         {"audio", OcSeqTrackKind::Audio},
        {"animation", OcSeqTrackKind::Animation}, {"visibility", OcSeqTrackKind::Visibility},
        {"cut", OcSeqTrackKind::CameraCut},       {"sub", OcSeqTrackKind::SubSequence},
    };
    for (const auto& e : kTable)
        if (equalsCI(n, e.name)) { k = e.kind; return true; }
    return false;
}

const char* interpName(OcSeqInterp i) {
    switch (i) {
        case OcSeqInterp::Linear: return "linear";
        case OcSeqInterp::Step:   return "step";
        case OcSeqInterp::Cubic:  return "cubic";
        default:                  return "smooth";
    }
}

bool isCoreKind(OcSeqTrackKind k) {
    return k == OcSeqTrackKind::Transform || k == OcSeqTrackKind::Camera || k == OcSeqTrackKind::Material;
}

} // namespace

const char* seqTrackKindName(OcSeqTrackKind k) {
    switch (k) {
        case OcSeqTrackKind::Transform:   return "transform";
        case OcSeqTrackKind::Camera:      return "camera";
        case OcSeqTrackKind::Material:    return "material";
        case OcSeqTrackKind::Property:    return "property";
        case OcSeqTrackKind::Event:       return "event";
        case OcSeqTrackKind::Audio:       return "audio";
        case OcSeqTrackKind::Animation:   return "animation";
        case OcSeqTrackKind::Visibility:  return "visibility";
        case OcSeqTrackKind::CameraCut:   return "cut";
        case OcSeqTrackKind::SubSequence: return "sub";
    }
    return "transform";
}

bool seqTrackNeedsBinding(OcSeqTrackKind k) {
    return k == OcSeqTrackKind::Transform || k == OcSeqTrackKind::Material || k == OcSeqTrackKind::Property ||
           k == OcSeqTrackKind::Visibility || k == OcSeqTrackKind::Animation;
}

usize seqKeyMinValues(OcSeqTrackKind k) {
    switch (k) {
        case OcSeqTrackKind::Transform:  return 9;
        case OcSeqTrackKind::Camera:     return 5;
        case OcSeqTrackKind::Material:   return 4;
        case OcSeqTrackKind::Visibility: return 2;
        default:                         return 1;
    }
}

bool parseSeqRecord(std::string_view key, const std::vector<std::string_view>& t,
                    std::vector<OcSequence>& out, SeqParseState& st) {
    if (equalsCI(key, "SEQUENCE")) {
        OcSequence sq;
        for (usize i = 1; i < t.size(); ++i) {
            if      (equalsCI(t[i], "name")     && i + 1 < t.size()) sq.name     = pctDecode(t[++i]);
            else if (equalsCI(t[i], "length")   && i + 1 < t.size()) sq.length   = parseF64(t[++i], sq.length);
            else if (equalsCI(t[i], "fps")      && i + 1 < t.size()) sq.fps      = std::clamp(parseI32(t[++i], 60), 1, 1000);
            else if (equalsCI(t[i], "loop")     && i + 1 < t.size()) sq.loop     = parseI32(t[++i], 1) != 0;
            else if (equalsCI(t[i], "autoplay") && i + 1 < t.size()) sq.autoplay = parseI32(t[++i], 1) != 0;
            else if (equalsCI(t[i], "camera")   && i + 1 < t.size()) sq.camera   = parseI32(t[++i], 1) != 0;
        }
        st.seqOpen = static_cast<i32>(out.size());
        st.trackOpen = -1;
        out.push_back(std::move(sq));
        return true;
    }
    if (equalsCI(key, "SEQTRACK")) {
        st.trackOpen = -1;
        if (st.seqOpen < 0 || t.size() < 2) return true;
        OcSeqTrack tr;
        if (!kindFromName(t[1], tr.kind)) return true;   // unknown kind: it and its keys are ignored
        for (usize i = 2; i + 1 < t.size(); ++i) {
            if      (equalsCI(t[i], "target")) tr.target   = parseI32(t[++i], -1);
            else if (equalsCI(t[i], "bind"))   tr.bind     = pctDecode(t[++i]);
            else if (equalsCI(t[i], "name"))   tr.name     = pctDecode(t[++i]);
            else if (equalsCI(t[i], "prop"))   tr.property = pctDecode(t[++i]);
            else if (equalsCI(t[i], "mute"))   tr.mute     = parseI32(t[++i], 0) != 0;
        }
        std::vector<OcSeqTrack>& tracks = out[static_cast<usize>(st.seqOpen)].tracks;
        st.trackOpen = static_cast<i32>(tracks.size());
        tracks.push_back(std::move(tr));
        return true;
    }
    if (equalsCI(key, "SEQKEY")) {
        if (st.seqOpen < 0 || st.trackOpen < 0) return true;
        OcSeqTrack& tr = out[static_cast<usize>(st.seqOpen)].tracks[static_cast<usize>(st.trackOpen)];
        OcSeqKey k = freshKey(tr.kind);
        k.t = t.size() > 1 ? parseF64(t[1]) : 0.0;
        usize i = 2;
        if (i < t.size()) {   // an unknown word is Smooth, as it always was
            if      (equalsCI(t[i], "linear")) k.interp = OcSeqInterp::Linear;
            else if (equalsCI(t[i], "step"))   k.interp = OcSeqInterp::Step;
            else if (equalsCI(t[i], "cubic"))  k.interp = OcSeqInterp::Cubic;
            ++i;
        }
        // The numeric run ends at the first token that is not a number; the tags follow it.
        f64 v;
        for (usize c = 0; c < 9 && i < t.size() && asNumber(t[i], v); ++c, ++i) k.v[c] = v;
        for (; i < t.size(); ++i) {
            if (equalsCI(t[i], "dur") && i + 1 < t.size())       k.dur = parseF64(t[++i]);
            else if (equalsCI(t[i], "text") && i + 1 < t.size()) k.text = pctDecode(t[++i]);
            else if (equalsCI(t[i], "arg") && i + 1 < t.size())  k.arg = pctDecode(t[++i]);
            else if (equalsCI(t[i], "tan")) {
                while (i + 1 < t.size() && asNumber(t[i + 1], v)) { k.tan.push_back(v); ++i; }
            }
        }
        tr.keys.push_back(std::move(k));
        return true;
    }
    if (equalsCI(key, "ENDSEQUENCE")) {
        st.seqOpen = st.trackOpen = -1;
        return true;
    }
    return false;
}

void sortSeqKeys(std::vector<OcSequence>& seqs) {
    for (OcSequence& sq : seqs)
        for (OcSeqTrack& tr : sq.tracks)
            std::stable_sort(tr.keys.begin(), tr.keys.end(),
                             [](const OcSeqKey& a, const OcSeqKey& b) { return a.t < b.t; });
}

void appendSeqRecords(std::string& s, const std::vector<OcSequence>& seqs,
                      const std::function<i32(i32)>& remapTarget) {
    for (const OcSequence& sq : seqs) {
        s += "SEQUENCE";
        if (!sq.name.empty()) { s += " name "; s += pctEncode(sq.name); }
        s += " length " + num(sq.length);
        if (sq.fps != 60) s += " fps " + std::to_string(sq.fps);
        s += std::string(" loop ") + (sq.loop ? "1" : "0") +
             " autoplay " + (sq.autoplay ? "1" : "0") +
             " camera " + (sq.camera ? "1" : "0") + "\n";
        for (const OcSeqTrack& tr : sq.tracks) {
            const bool hadBinding = tr.target >= 0 || !tr.bind.empty();
            if (seqTrackNeedsBinding(tr.kind) && !hadBinding) continue;
            i32 target = -1;
            if (tr.target >= 0) target = remapTarget ? remapTarget(tr.target) : tr.target;
            if (hadBinding && target < 0 && tr.bind.empty()) continue;   // the actor is gone
            s += "  SEQTRACK "; s += seqTrackKindName(tr.kind);
            if (target >= 0) s += " target " + std::to_string(target);
            if (!tr.bind.empty())     { s += " bind "; s += pctEncode(tr.bind); }
            if (!tr.name.empty())     { s += " name "; s += pctEncode(tr.name); }
            if (!tr.property.empty()) { s += " prop "; s += pctEncode(tr.property); }
            if (tr.mute) s += " mute 1";
            s += "\n";
            std::vector<const OcSeqKey*> keys;
            keys.reserve(tr.keys.size());
            for (const OcSeqKey& k : tr.keys) keys.push_back(&k);
            std::stable_sort(keys.begin(), keys.end(),
                             [](const OcSeqKey* x, const OcSeqKey* y) { return x->t < y->t; });
            const OcSeqKey neutral = freshKey(tr.kind);
            for (const OcSeqKey* k : keys) {
                usize cnt = seqKeyMinValues(tr.kind);
                if (!isCoreKind(tr.kind))   // trailing neutral values are not written
                    for (usize i = 9; i > cnt; --i)
                        if (k->v[i - 1] != neutral.v[i - 1]) { cnt = i; break; }
                s += "    SEQKEY " + num(k->t) + " "; s += interpName(k->interp);
                for (usize i = 0; i < cnt; ++i) { s += " "; s += num(k->v[i]); }
                if (k->dur != 0.0)    s += " dur " + num(k->dur);
                if (!k->text.empty()) { s += " text "; s += pctEncode(k->text); }
                if (!k->arg.empty())  { s += " arg "; s += pctEncode(k->arg); }
                if (!k->tan.empty()) {
                    s += " tan";
                    for (const f64 v : k->tan) { s += " "; s += num(v); }
                }
                s += "\n";
            }
        }
        s += "ENDSEQUENCE\n";
    }
}

bool parseOcseq(std::string_view text, OcSeqFile& out, std::string* err) {
    out = OcSeqFile{};
    if (err) err->clear();
    SeqParseState st;
    bool sawHeader = false;
    usize pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        const std::string_view rawLine = text.substr(pos, nl - pos);
        pos = nl + 1;
        const std::string_view line = stripTrailingSemicolon(truncateHash(rawLine));
        if (line.empty()) continue;
        const std::vector<std::string_view> t = splitWhitespace(line);
        if (t.empty()) continue;
        if (!sawHeader) {
            if (!equalsCI(t[0], "OCSEQ")) { if (err) *err = "not an .ocseq file (missing OCSEQ header)"; return false; }
            const i32 ver = t.size() > 1 ? parseI32(t[1], 0) : 0;
            if (ver < 1 || ver > kOcSeqVersion) {
                if (err) *err = "unsupported .ocseq version " + std::to_string(ver);
                return false;
            }
            sawHeader = true;
            continue;
        }
        parseSeqRecord(t[0], t, out.sequences, st);
    }
    if (!sawHeader) { if (err) *err = "empty .ocseq file"; return false; }
    sortSeqKeys(out.sequences);
    // Placement indices mean nothing outside a level: actors are bound by name only.
    usize dropped = 0;
    for (OcSequence& sq : out.sequences) {
        for (OcSeqTrack& tr : sq.tracks) tr.target = -1;
        const usize before = sq.tracks.size();
        sq.tracks.erase(std::remove_if(sq.tracks.begin(), sq.tracks.end(),
                                       [](const OcSeqTrack& tr) { return seqTrackNeedsBinding(tr.kind) && tr.bind.empty(); }),
                        sq.tracks.end());
        dropped += before - sq.tracks.size();
    }
    if (dropped > 0) {
        const std::string msg = std::to_string(dropped) + " track(s) dropped: an .ocseq track needs a `bind` name";
        AVER_WARN("[Sequencer] {}", msg);
        if (err) *err = msg;
    }
    return true;
}

std::string writeOcseq(const OcSeqFile& f) {
    std::string s = "OCSEQ " + std::to_string(kOcSeqVersion) + "\n";
    appendSeqRecords(s, f.sequences, [](i32) { return -1; });
    return s;
}

} // namespace aver::fmt
