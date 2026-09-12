// .ocanim: the NOTIFY DURATION chunk (NTFD, item 7.1's format half) and the CURVE TANGENT chunk
// (CTAN, item 7.2's).
//
// THE ROUND TRIP IS THE POINT, same reason OcRigTest gives for its own: a writer and reader that
// disagree corrupt a file quietly, on every save. What matters MORE here is the negative claim --
// that a clip with no notify state, or no curve tangent, writes NO chunk at all, so a file saved by
// an engine that predates the feature and one saved by this engine, for the same content, are the
// SAME BYTES. See OcAnimation::notifyDurations and OcCurve::inTangents/outTangents in OcAnim.hpp for
// why each is a separate optional chunk rather than a wider NOTF/CRVE, modules/anim.scene/src/
// AnimSystem.cpp for the notify-duration runtime half, and modules/anim/src/AnimSampler.cpp
// (sampleCurve) for the tangent runtime half.
#include "aver/formats/OcAnim.hpp"
#include "aver/formats/Avr1.hpp"   // for the hand-corrupted CTAN/CRVE-mismatch fixture below
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool near(f32 a, f32 b) { return std::fabs(a - b) < 1e-5f; }

// True when `haystack` contains the four literal bytes of a chunk fourCC -- a cheap, honest way to
// ask "is this chunk really absent" without reaching into AVR1's private directory-parsing code.
// avrFourCC packs the characters little-endian (see Avr1.hpp), so the ASCII bytes appear on disk in
// the order they are named, exactly as this searches for them.
static bool containsFourCC(const std::vector<u8>& haystack, const char (&cc)[5]) {
    const u8 needle[4] = {u8(cc[0]), u8(cc[1]), u8(cc[2]), u8(cc[3])};
    return std::search(haystack.begin(), haystack.end(), needle, needle + 4) != haystack.end();
}

// One bone, one track, sliding it 100cm along X over `duration` seconds. Deliberately the smallest
// clip writeOcAnim will accept -- valid() and the writer both refuse an empty track list.
static fmt::OcAnimation baseClip(f32 duration) {
    fmt::OcAnimation a;
    a.duration = duration;
    a.flags = fmt::kOcAnimLoop;
    fmt::OcTrack t;
    t.boneIndex = 0;
    t.channels = fmt::kOcChannelTranslation;
    t.interp = fmt::OcInterp::Linear;
    t.times = {0.0f, duration};
    t.values = {0, 0, 0,  100, 0, 0};
    a.tracks.push_back(t);
    return a;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO(".ocanim notify durations (NTFD)");

    AVER_INFO("=== a plain notify, no state: the chunk is OMITTED ===");
    {
        fmt::OcAnimation a = baseClip(2.0f);
        a.notifies = {{0.0f, "Start"}, {1.0f, "Mid"}, {2.0f, "End"}};
        // notifyDurations is left default-constructed -- EMPTY, not a same-length column of zeros --
        // which is exactly what an untouched clip looks like after this feature landed.
        check(a.notifyDurations.empty(), "the fixture never touched durations");

        std::vector<u8> bytes;
        std::string why;
        check(fmt::writeOcAnim(a, bytes, &why), "it writes: " + why);
        check(!containsFourCC(bytes, "NTFD"), "and the NTFD chunk is not in the file at all");

        fmt::OcAnimation back;
        check(fmt::parseOcAnim(bytes.data(), bytes.size(), back, &why), "it reads back: " + why);
        check(back.notifies.size() == 3, "all three notifies survived");
        check(back.notifyDurations.empty(), "an absent chunk parses as an EMPTY vector, not zeros");
        for (u32 i = 0; i < 3; ++i)
            check(near(back.notifyDuration(i), 0.0f), "notifyDuration(" + std::to_string(i) + ") reads instant");

        // BYTE-IDENTICAL REWRITE: this is the guarantee the whole design exists for. A clip that
        // predates notify states -- which this one now stands in for, having never set a duration --
        // must come back out of a load/save cycle unchanged, bit for bit.
        std::vector<u8> again;
        check(fmt::writeOcAnim(back, again, &why), "and it writes again: " + why);
        check(bytes == again, "byte-identical: " + std::to_string(bytes.size()) + " vs " +
                              std::to_string(again.size()) + " bytes");
    }

    AVER_INFO("=== a notify STATE: the chunk is dense, parallel-indexed, and round-trips ===");
    {
        fmt::OcAnimation a = baseClip(2.0f);
        a.notifies = {{0.5f, "Swing"}, {1.0f, "Footstep"}, {1.5f, "Recover"}};
        // ONE of the three is a state (the hit window on the swing); the other two stay instant.
        // Dense: an entry for every notify, not just the one that has something to say.
        a.notifyDurations = {0.4f, 0.0f, 0.0f};

        std::vector<u8> bytes;
        std::string why;
        check(fmt::writeOcAnim(a, bytes, &why), "it writes: " + why);
        check(containsFourCC(bytes, "NTFD"), "and this time the chunk IS in the file");

        fmt::OcAnimation back;
        check(fmt::parseOcAnim(bytes.data(), bytes.size(), back, &why), "it reads back: " + why);
        check(back.notifyDurations.size() == 3, "one duration per notify, got " +
                                                std::to_string(back.notifyDurations.size()));
        check(near(back.notifyDuration(0), 0.4f), "the state's own duration survives");
        check(near(back.notifyDuration(1), 0.0f), "Footstep stayed instant");
        check(near(back.notifyDuration(2), 0.0f), "so did Recover");
        check(near(back.notifyDuration(99), 0.0f), "and an out-of-range index reads instant rather than crashing");

        std::vector<u8> again;
        check(fmt::writeOcAnim(back, again, &why), "it writes a second time: " + why);
        check(bytes == again, "and that rewrite is byte-identical too");
    }

    AVER_INFO("=== all-zero durations collapse to the SAME absent chunk, whatever shape the caller built ===");
    {
        // An editor that always sizes this array to match notifies (rather than leaving it empty
        // until the first state is authored) must still get the old file back when nobody has set
        // one above zero -- checked by VALUE in the writer, not by "is the vector empty".
        fmt::OcAnimation zeroFilled = baseClip(1.0f);
        zeroFilled.notifies = {{0.0f, "A"}, {0.5f, "B"}};
        zeroFilled.notifyDurations = {0.0f, 0.0f};

        fmt::OcAnimation untouched = baseClip(1.0f);
        untouched.notifies = zeroFilled.notifies;
        // notifyDurations left empty entirely.

        std::vector<u8> a, b;
        std::string why;
        check(fmt::writeOcAnim(zeroFilled, a, &why), "the zero-filled shape writes: " + why);
        check(fmt::writeOcAnim(untouched, b, &why), "the empty-vector shape writes: " + why);
        check(!containsFourCC(a, "NTFD"), "the zero-filled shape ALSO omits the chunk");
        check(a == b, "and the two shapes produce the identical file");
    }

    AVER_INFO("=== a mismatched or invalid duration list is refused at write, not silently reshaped ===");
    {
        fmt::OcAnimation a = baseClip(1.0f);
        a.notifies = {{0.0f, "A"}, {0.5f, "B"}};

        a.notifyDurations = {0.2f};   // one entry for two notifies
        std::vector<u8> bytes;
        std::string why;
        check(!fmt::writeOcAnim(a, bytes, &why), "a short duration list is refused");
        check(!why.empty(), "with a reason: " + why);

        a.notifyDurations = {0.2f, 0.1f, 0.3f};   // one too many
        check(!fmt::writeOcAnim(a, bytes, &why), "and a long one is refused too");

        a.notifyDurations = {0.2f, -1.0f};   // negative
        check(!fmt::writeOcAnim(a, bytes, &why), "a negative duration is refused");

        a.notifyDurations = {0.2f, std::nanf("")};
        check(!fmt::writeOcAnim(a, bytes, &why), "and NaN is refused for the same reason invalid() rejects it");

        a.notifyDurations = {0.2f, 0.0f};   // finally a valid, matching pair
        check(fmt::writeOcAnim(a, bytes, &why), "a correctly-shaped list writes fine: " + why);
    }

    AVER_INFO(".ocanim curve tangents (CTAN)");

    AVER_INFO("=== a curve with no tangents: the chunk is OMITTED ===");
    {
        fmt::OcAnimation a = baseClip(2.0f);
        fmt::OcCurve reload;
        reload.name = "ReloadProgress";
        reload.interp = fmt::OcInterp::CubicSpline;
        reload.times = {0.0f, 1.0f, 2.0f};
        reload.values = {0.0f, 0.5f, 1.0f};
        // inTangents/outTangents left default-constructed -- EMPTY, exactly what a curve saved before
        // tangents existed looks like, CubicSpline-tagged or not.
        a.curves = {reload};
        check(a.curves[0].inTangents.empty() && a.curves[0].outTangents.empty(),
              "the fixture never touched tangents");

        std::vector<u8> bytes;
        std::string why;
        check(fmt::writeOcAnim(a, bytes, &why), "it writes: " + why);
        check(!containsFourCC(bytes, "CTAN"), "and the CTAN chunk is not in the file at all");

        fmt::OcAnimation back;
        check(fmt::parseOcAnim(bytes.data(), bytes.size(), back, &why), "it reads back: " + why);
        check(back.curves.size() == 1, "the curve survived");
        check(back.curves[0].inTangents.empty() && back.curves[0].outTangents.empty(),
              "an absent chunk parses as EMPTY tangent vectors, not zeros");

        // BYTE-IDENTICAL REWRITE, same guarantee NTFD's own test makes: a clip whose curves predate
        // tangents must come back out of a load/save cycle bit-for-bit unchanged.
        std::vector<u8> again;
        check(fmt::writeOcAnim(back, again, &why), "and it writes again: " + why);
        check(bytes == again, "byte-identical: " + std::to_string(bytes.size()) + " vs " +
                              std::to_string(again.size()) + " bytes");
    }

    AVER_INFO("=== real tangents: the chunk is dense, parallel-indexed to CRVE, and round-trips ===");
    {
        fmt::OcAnimation a = baseClip(2.0f);
        fmt::OcCurve tangented;
        tangented.name = "Swing";
        tangented.interp = fmt::OcInterp::CubicSpline;
        tangented.times = {0.0f, 1.0f, 2.0f};
        tangented.values = {0.0f, 5.0f, 0.0f};
        tangented.inTangents  = {0.0f, -3.0f, 2.0f};
        tangented.outTangents = {4.0f,  3.0f, 0.0f};
        // A SECOND curve in the same clip that never got a tangent of its own -- proving the chunk's
        // per-clip granularity does not force every curve to author one, only to receive a dense
        // (zero-filled) entry once ANY curve in the clip needs the chunk written at all.
        fmt::OcCurve untouched;
        untouched.name = "FootPlanted";
        untouched.interp = fmt::OcInterp::Step;
        untouched.times = {0.0f, 1.0f};
        untouched.values = {0.0f, 1.0f};
        a.curves = {tangented, untouched};

        std::vector<u8> bytes;
        std::string why;
        check(fmt::writeOcAnim(a, bytes, &why), "it writes: " + why);
        check(containsFourCC(bytes, "CTAN"), "and this time the chunk IS in the file");

        fmt::OcAnimation back;
        check(fmt::parseOcAnim(bytes.data(), bytes.size(), back, &why), "it reads back: " + why);
        check(back.curves.size() == 2, "both curves survive");
        if (const fmt::OcCurve* c = back.curve("Swing")) {
            check(c->inTangents.size() == 3 && c->outTangents.size() == 3, "Swing's tangents keep their count");
            check(near(c->inTangents[1], -3.0f), "an in-tangent value survives");
            check(near(c->outTangents[0], 4.0f), "and an out-tangent value");
        } else check(false, "Swing resolves by name");
        if (const fmt::OcCurve* c = back.curve("FootPlanted")) {
            // DENSE, NOT ABSENT: FootPlanted authored no tangent of its own, but the chunk exists
            // because Swing needed it, so FootPlanted gets a same-length, all-zero pair back rather
            // than an empty one. See OcCurve::inTangents' own comment on this exact consequence.
            check(c->inTangents.size() == 2 && c->outTangents.size() == 2,
                  "FootPlanted comes back DENSE, not empty, once another curve needed the chunk");
            check(near(c->inTangents[0], 0.0f) && near(c->inTangents[1], 0.0f) &&
                  near(c->outTangents[0], 0.0f) && near(c->outTangents[1], 0.0f),
                  "and every value in that dense pair is exactly zero");
        } else check(false, "FootPlanted resolves by name");

        std::vector<u8> again;
        check(fmt::writeOcAnim(back, again, &why), "it writes a second time: " + why);
        check(bytes == again, "and that rewrite is byte-identical too");
    }

    AVER_INFO("=== all-zero tangents collapse to the SAME absent chunk, whatever shape the caller built ===");
    {
        // An editor that always sizes a curve's tangent arrays (rather than leaving them empty until
        // a handle is first dragged) must still get the old file back when nobody has moved one off
        // zero -- checked by VALUE in the writer, not by "are the vectors empty".
        fmt::OcAnimation zeroFilled = baseClip(1.0f);
        fmt::OcCurve zc;
        zc.name = "C";
        zc.interp = fmt::OcInterp::CubicSpline;
        zc.times = {0.0f, 1.0f};
        zc.values = {0.0f, 1.0f};
        zc.inTangents = {0.0f, 0.0f};
        zc.outTangents = {0.0f, 0.0f};
        zeroFilled.curves = {zc};

        fmt::OcAnimation untouched = baseClip(1.0f);
        fmt::OcCurve uc = zc;
        uc.inTangents.clear();
        uc.outTangents.clear();
        untouched.curves = {uc};

        std::vector<u8> a, b;
        std::string why;
        check(fmt::writeOcAnim(zeroFilled, a, &why), "the zero-filled shape writes: " + why);
        check(fmt::writeOcAnim(untouched, b, &why), "the empty-vector shape writes: " + why);
        check(!containsFourCC(a, "CTAN"), "the zero-filled shape ALSO omits the chunk");
        check(a == b, "and the two shapes produce the identical file");
    }

    AVER_INFO("=== a mismatched, one-sided, or non-finite tangent list is refused at write ===");
    {
        fmt::OcAnimation a = baseClip(1.0f);
        fmt::OcCurve c;
        c.name = "C";
        c.interp = fmt::OcInterp::CubicSpline;
        c.times = {0.0f, 0.5f, 1.0f};
        c.values = {0.0f, 1.0f, 0.0f};
        a.curves = {c};
        std::vector<u8> bytes;
        std::string why;

        a.curves[0].inTangents = {0.1f, 0.2f};    // two entries for three keys
        a.curves[0].outTangents = {0.1f, 0.2f, 0.3f};
        check(!fmt::writeOcAnim(a, bytes, &why), "a short in-tangent list is refused");
        check(!why.empty(), "with a reason: " + why);

        a.curves[0].inTangents = {0.1f, 0.2f, 0.3f, 0.4f};   // one too many
        check(!fmt::writeOcAnim(a, bytes, &why), "and a long one is refused too");

        a.curves[0].inTangents.clear();    // one side set, the other not
        a.curves[0].outTangents = {0.1f, 0.2f, 0.3f};
        check(!fmt::writeOcAnim(a, bytes, &why), "tangents on only one side are refused");

        a.curves[0].inTangents = {0.1f, 0.2f, 0.3f};
        a.curves[0].outTangents = {0.1f, std::nanf(""), 0.3f};
        check(!fmt::writeOcAnim(a, bytes, &why), "and NaN is refused, same as a notify duration");

        a.curves[0].outTangents = {0.1f, std::numeric_limits<f32>::infinity(), 0.3f};
        check(!fmt::writeOcAnim(a, bytes, &why), "so is infinity");

        a.curves[0].outTangents = {0.1f, 0.2f, 0.3f};   // finally a valid, matching pair
        why.clear();   // writeOcAnim never touches `why` on success, so a stale FAILURE reason from
                        // the checks above would otherwise print here despite this call succeeding.
        check(fmt::writeOcAnim(a, bytes, &why), "a correctly-shaped tangent pair writes fine: " + why);
    }

    AVER_INFO("=== a CTAN that disagrees with CRVE is a malformed file, not a guess ===");
    {
        // Built by hand-corrupting a real container rather than through writeOcAnim, which would
        // never produce this shape: this is what a hand-edited or corrupt file looks like, and the
        // reader must name it rather than silently lining tangents up against the wrong curve.
        fmt::OcAnimation a = baseClip(1.0f);
        fmt::OcCurve c;
        c.name = "Only";
        c.times = {0.0f, 1.0f};
        c.values = {0.0f, 1.0f};
        a.curves = {c};
        std::vector<u8> bytes;
        std::string why;
        check(fmt::writeOcAnim(a, bytes, &why), "a one-curve clip writes: " + why);

        fmt::Avr1File f;
        check(fmt::parseAvr1(bytes.data(), bytes.size(), f, &why), "and its container parses: " + why);

        // A hand-built CTAN claiming TWO curves' worth of tangents when CRVE holds only one.
        auto makeCtan = [](u32 curveCount, u32 keyCount) {
            std::vector<u8> ctan;
            auto put32 = [&](u32 v) { for (int i = 0; i < 4; ++i) ctan.push_back(u8(v >> (i * 8))); };
            put32(curveCount);
            put32(keyCount);
            for (u32 k = 0; k < keyCount; ++k) put32(0);   // in-tangents, all zero
            for (u32 k = 0; k < keyCount; ++k) put32(0);   // out-tangents, all zero
            return ctan;
        };

        fmt::Avr1File badCount = f;
        badCount.add(fmt::avrFourCC("CTAN"), makeCtan(2, 2));   // 2 curves claimed; CRVE has 1
        std::vector<u8> corruptCount;
        why.clear();
        check(fmt::writeAvr1(badCount, corruptCount, &why), "the curve-count fixture re-serialises: " + why);
        why.clear();
        fmt::OcAnimation back1;
        check(!fmt::parseOcAnim(corruptCount.data(), corruptCount.size(), back1, &why),
              "a CTAN curve count that disagrees with CRVE's is refused");
        check(!why.empty(), "with a reason: " + why);

        fmt::Avr1File badKeys = f;
        badKeys.add(fmt::avrFourCC("CTAN"), makeCtan(1, 5));   // 1 curve, but 5 keys where CRVE has 2
        std::vector<u8> corruptKeys;
        why.clear();
        check(fmt::writeAvr1(badKeys, corruptKeys, &why), "the key-count fixture re-serialises: " + why);
        why.clear();
        fmt::OcAnimation back2;
        check(!fmt::parseOcAnim(corruptKeys.data(), corruptKeys.size(), back2, &why),
              "and a per-curve key count that disagrees with CRVE's is refused too");
        check(!why.empty(), "with a reason: " + why);
    }

    AVER_INFO("=== a whole file round-trips through disk, not just through memory ===");
    {
        const std::string dir = (std::filesystem::temp_directory_path() / "aver-ocanim-test").string();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::string path = dir + "/state.ocanim";

        fmt::OcAnimation a = baseClip(3.0f);
        a.notifies = {{1.0f, "WindUp"}, {1.8f, "Impact"}};
        a.notifyDurations = {0.6f, 0.0f};   // WindUp is a hit window; Impact is instant
        fmt::OcCurve c;
        c.name = "Weight";
        c.interp = fmt::OcInterp::CubicSpline;
        c.times = {0.0f, 1.5f, 3.0f};
        c.values = {0.0f, 1.0f, 0.0f};
        c.inTangents  = {0.0f, -1.5f, 2.5f};
        c.outTangents = {2.0f,  1.5f, 0.0f};
        a.curves = {c};

        std::string why;
        check(fmt::saveOcAnim(path, a, &why), "it saves: " + why);

        fmt::OcAnimation back;
        check(fmt::loadOcAnim(path, back, &why), "and loads back: " + why);
        check(back.notifies.size() == 2 && back.notifyDurations.size() == 2, "notify shape survives disk");
        check(near(back.notifyDuration(0), 0.6f), "WindUp's duration survives disk");
        check(back.curves.size() == 1 && back.curves[0].inTangents.size() == 3,
              "curve tangent shape survives disk");
        check(near(back.curves[0].inTangents[1], -1.5f), "and an actual tangent value survives disk");

        // The SAME shape AnimEditor::save() relies on: rewriting the WHOLE parsed struct must not
        // drop either field, which is exactly the failure mode the task brief calls out by name --
        // for notify durations in 7.1, and now for curve tangents in 7.2.
        check(fmt::saveOcAnim(path, back, &why), "re-saving the parsed struct succeeds: " + why);
        fmt::OcAnimation again;
        check(fmt::loadOcAnim(path, again, &why), "and loads back a second time: " + why);
        check(again.notifyDurations.size() == 2 && near(again.notifyDuration(0), 0.6f),
              "the notify duration is STILL there -- a save that only forwarded `notifies` would have lost it");
        check(again.curves.size() == 1 && again.curves[0].inTangents.size() == 3 &&
              near(again.curves[0].inTangents[1], -1.5f),
              "and the tangent is STILL there -- a save that only forwarded times/values would have lost it");

        std::filesystem::remove_all(dir, ec);
    }

    AVER_INFO(g_failures == 0 ? "OcAnimTest: {}/{} checks passed" : "OcAnimTest: {} FAILURES of {}",
              g_failures == 0 ? g_checks : g_failures, g_checks);
    return g_failures ? 1 : 0;
}
