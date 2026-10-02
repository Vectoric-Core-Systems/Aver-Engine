// .oclanes text reader and writer. See OcLanes.hpp for what the format is and why it is text.
#include "aver/formats/OcLanes.hpp"
#include "aver/formats/detail/TextScan.hpp"
#include "aver/platform/FileSystem.hpp"

#include <charconv>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace aver::fmt {
using namespace aver::fmt::detail;

namespace {

bool fail(std::string* err, const std::string& why) {
    if (err) *err = why;
    return false;
}

// WHOLE TOKEN OR NOTHING. TextScan's parseF64/parseI32 return a fallback on failure and accept a
// numeric PREFIX ("12abc" reads as 12), which is right for a level where a typo should not lose the
// file and wrong here: a coordinate read as 0 because of a stray character is a lane that bends to
// the origin, three files away from its cause.
template <class T>
bool parseWhole(std::string_view s, T& out) {
    const char* const end = s.data() + s.size();
    const auto r = std::from_chars(s.data(), end, out);
    return r.ec == std::errc{} && r.ptr == end;
}

// from_chars reads "nan" and "inf" as numbers; a lane point that is neither is not one.
bool parseFinite(std::string_view s, f32& out) {
    return parseWhole(s, out) && std::isfinite(out);
}

// The shortest text that reads back as the SAME f32, in fixed notation so a hand reader sees
// "12345.5" and not "1.23455e+04". %g would be shorter to write and loses the round trip past six digits.
void appendNum(std::string& s, f32 v) {
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed);
    s.append(buf, r.ptr);
}

} // namespace

bool parseOcLanes(std::string_view text, OcLanesData& out, std::string* err) {
    out = OcLanesData{};

    OcLanesData d;
    bool sawHeader = false;
    std::unordered_map<i32, usize> laneIndex;   // lane id -> index into d.lanes

    // NEXT records are held until every LANE has been read: the file may list a lane's successors
    // before the lane, and a successor may be declared further down than the lane naming it.
    struct PendingNext { i32 id = 0; std::vector<i32> successors; int line = 0; };
    std::vector<PendingNext> pending;
    std::unordered_set<i32> sawNext;

    int lineNo = 0;
    usize pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        const std::string_view rawLine = text.substr(pos, nl - pos);
        pos = nl + 1;
        ++lineNo;

        // trim drops the CR a CRLF file leaves at the end of every line.
        const std::vector<std::string_view> t = splitWhitespace(trim(truncateHash(rawLine)));
        if (t.empty()) continue;

        const std::string at = " (line " + std::to_string(lineNo) + ")";

        if (equalsCI(t[0], "OCLANES")) {
            if (sawHeader) return fail(err, "a second OCLANES header" + at);
            if (t.size() < 2) return fail(err, "OCLANES needs a version" + at);
            if (t[1] != "1") return fail(err, "unsupported .oclanes version '" + std::string(t[1]) + "'" + at);
            sawHeader = true;
            continue;
        }
        if (!sawHeader) return fail(err, "the first record must be OCLANES 1" + at);

        if (equalsCI(t[0], "LANE")) {
            // LANE <id> <speedLimit> <width> <flags> <n> then 3n coordinates.
            if (t.size() < 6) return fail(err, "LANE needs <id> <speedLimit> <width> <flags> <n> and its points" + at);
            OcLane lane;
            u32 n = 0;
            if (!parseWhole(t[1], lane.id))           return fail(err, "bad lane id '" + std::string(t[1]) + "'" + at);
            if (!parseFinite(t[2], lane.speedLimit))  return fail(err, "bad speed limit '" + std::string(t[2]) + "'" + at);
            if (!parseFinite(t[3], lane.width))       return fail(err, "bad width '" + std::string(t[3]) + "'" + at);
            if (!parseWhole(t[4], lane.flags))        return fail(err, "bad flags '" + std::string(t[4]) + "'" + at);
            if (!parseWhole(t[5], n))                 return fail(err, "bad point count '" + std::string(t[5]) + "'" + at);

            // A speed of zero or a width of zero is a lane nothing can drive; refusing it here keeps
            // a divide-by-the-limit out of every consumer.
            if (lane.speedLimit <= 0.0f) return fail(err, "lane " + std::to_string(lane.id) + " has a speed limit that is not positive" + at);
            if (lane.width <= 0.0f)      return fail(err, "lane " + std::to_string(lane.id) + " has a width that is not positive" + at);
            if (n < 2) return fail(err, "lane " + std::to_string(lane.id) + " has " + std::to_string(n) +
                                        " point(s); a lane needs at least 2" + at);

            // Checked against the tokens actually present BEFORE anything is reserved, so a count of
            // two billion in a damaged file is an error and not an allocation.
            const usize coords = t.size() - 6;
            if (coords % 3 != 0 || coords / 3 != n)
                return fail(err, "lane " + std::to_string(lane.id) + " says " + std::to_string(n) +
                                 " point(s) but " + std::to_string(coords) + " coordinate value(s) follow" + at);

            lane.pts.reserve(n);
            for (u32 i = 0; i < n; ++i) {
                f32 c[3] = {0.0f, 0.0f, 0.0f};
                for (usize k = 0; k < 3; ++k)
                    if (!parseFinite(t[6 + i * 3 + k], c[k]))
                        return fail(err, "bad coordinate '" + std::string(t[6 + i * 3 + k]) + "' in lane " +
                                         std::to_string(lane.id) + at);
                lane.pts.emplace_back(c[0], c[1], c[2]);
            }

            if (!laneIndex.emplace(lane.id, d.lanes.size()).second)
                return fail(err, "lane id " + std::to_string(lane.id) + " is declared twice" + at);
            d.lanes.push_back(std::move(lane));
            continue;
        }

        if (equalsCI(t[0], "NEXT")) {
            if (t.size() < 2) return fail(err, "NEXT needs a lane id" + at);
            PendingNext p;
            p.line = lineNo;
            if (!parseWhole(t[1], p.id)) return fail(err, "bad lane id '" + std::string(t[1]) + "'" + at);
            for (usize i = 2; i < t.size(); ++i) {
                i32 s = 0;
                if (!parseWhole(t[i], s)) return fail(err, "bad successor id '" + std::string(t[i]) + "'" + at);
                p.successors.push_back(s);
            }
            // One NEXT per lane: with two, "replace" and "append" are both readings, and the file does
            // not say which. The writer emits exactly one, so a second is a hand edit or a generator bug.
            if (!sawNext.insert(p.id).second)
                return fail(err, "a second NEXT for lane " + std::to_string(p.id) + at);
            pending.push_back(std::move(p));
            continue;
        }

        return fail(err, "unknown record '" + std::string(t[0]) + "'" + at);
    }

    if (!sawHeader) return fail(err, "empty or missing OCLANES header");

    // Every id a NEXT mentions must now be a declared lane.
    for (PendingNext& p : pending) {
        const std::string at = " (line " + std::to_string(p.line) + ")";
        const auto from = laneIndex.find(p.id);
        if (from == laneIndex.end())
            return fail(err, "NEXT names lane " + std::to_string(p.id) + ", which no LANE declares" + at);
        for (const i32 s : p.successors)
            if (laneIndex.find(s) == laneIndex.end())
                return fail(err, "NEXT " + std::to_string(p.id) + " names successor " + std::to_string(s) +
                                 ", which no LANE declares" + at);
        d.lanes[from->second].next = std::move(p.successors);
    }

    out = std::move(d);
    return true;
}

std::string writeOcLanes(const OcLanesData& d) {
    std::string s = "OCLANES 1\n";
    for (const OcLane& lane : d.lanes) {
        s += "LANE " + std::to_string(lane.id) + " ";
        appendNum(s, lane.speedLimit);
        s += ' ';
        appendNum(s, lane.width);
        s += " " + std::to_string(lane.flags) + " " + std::to_string(lane.pts.size());
        for (const Vec3& p : lane.pts) {
            s += ' '; appendNum(s, p.x);
            s += ' '; appendNum(s, p.y);
            s += ' '; appendNum(s, p.z);
        }
        s += '\n';
        // Omitted for a dead end: a NEXT with no successors says nothing a missing one does not.
        if (!lane.next.empty()) {
            s += "NEXT " + std::to_string(lane.id);
            for (const i32 n : lane.next) s += " " + std::to_string(n);
            s += '\n';
        }
    }
    return s;
}

bool loadOcLanes(const std::string& path, OcLanesData& out, std::string* err) {
    // readFileText, not an ifstream of our own: it reports the open to the file trace, which is how a
    // packaged game proves it read this file from inside its package (platform/FileSystem.hpp).
    std::string text;
    if (!readFileText(path, text)) return fail(err, "cannot open " + path);
    return parseOcLanes(text, out, err);
}

} // namespace aver::fmt
