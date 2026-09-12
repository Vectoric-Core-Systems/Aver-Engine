// .ocanim: the NOTIFY DURATION chunk (NTFD), item 7.1's format half.
//
// THE ROUND TRIP IS THE POINT, same reason OcRigTest gives for its own: a writer and reader that
// disagree corrupt a file quietly, on every save. What matters MORE here is the negative claim --
// that a clip with no notify state writes NO chunk at all, so a file saved by an engine that
// predates notify states and one saved by this engine, for the same content, are the SAME BYTES.
// See OcAnimation::notifyDurations in OcAnim.hpp for why that is a separate optional chunk rather
// than a wider NOTF, and modules/anim.scene/src/AnimSystem.cpp for the runtime half this exists to
// feed.
#include "aver/formats/OcAnim.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
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

    AVER_INFO("=== a whole file round-trips through disk, not just through memory ===");
    {
        const std::string dir = (std::filesystem::temp_directory_path() / "aver-ocanim-test").string();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::string path = dir + "/state.ocanim";

        fmt::OcAnimation a = baseClip(3.0f);
        a.notifies = {{1.0f, "WindUp"}, {1.8f, "Impact"}};
        a.notifyDurations = {0.6f, 0.0f};   // WindUp is a hit window; Impact is instant

        std::string why;
        check(fmt::saveOcAnim(path, a, &why), "it saves: " + why);

        fmt::OcAnimation back;
        check(fmt::loadOcAnim(path, back, &why), "and loads back: " + why);
        check(back.notifies.size() == 2 && back.notifyDurations.size() == 2, "shape survives disk");
        check(near(back.notifyDuration(0), 0.6f), "WindUp's duration survives disk");

        // The SAME shape AnimEditor::save() relies on: rewriting the WHOLE parsed struct must not
        // drop the field, which is exactly the failure mode the task brief calls out by name.
        check(fmt::saveOcAnim(path, back, &why), "re-saving the parsed struct succeeds: " + why);
        fmt::OcAnimation again;
        check(fmt::loadOcAnim(path, again, &why), "and loads back a second time: " + why);
        check(again.notifyDurations.size() == 2 && near(again.notifyDuration(0), 0.6f),
              "the duration is STILL there -- a save that only forwarded `notifies` would have lost it");

        std::filesystem::remove_all(dir, ec);
    }

    AVER_INFO(g_failures == 0 ? "OcAnimTest: {}/{} checks passed" : "OcAnimTest: {} FAILURES of {}",
              g_failures == 0 ? g_checks : g_failures, g_checks);
    return g_failures ? 1 : 0;
}
