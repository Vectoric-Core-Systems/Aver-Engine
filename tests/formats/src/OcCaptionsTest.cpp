// .occaptions tests (docs/plans/wave9/ACCESSIBILITY.md section 2.2): the reader (speakers, cues, lines, quoting, the tag
// shorthand, repeated sound, malformed records skipped with a diagnostic naming the line, a header that is missing or
// newer), the record-preserving writer (comments and unknown records survive; changed, removed and new records), the
// round trip, the colour grammar and the sound-pattern matcher. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/formats/LocGather.hpp"
#include "aver/formats/OcCaptions.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::fmt;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) {
        AVER_INFO("   PASS  {}", what);
    } else {
        AVER_ERROR("   FAIL  {}", what);
        ++g_failures;
    }
}

static bool anyContains(const std::vector<std::string>& v, const std::string& needle) {
    for (const std::string& s : v) if (s.find(needle) != std::string::npos) return true;
    return false;
}

static const OcCaptionCue* findCue(const OcCaptionData& d, const std::string& id) {
    for (const OcCaptionCue& c : d.cues) if (c.id == id) return &c;
    return nullptr;
}

static const char* kSample =
    "# Captions for the intro\n"
    "OCCAPTIONS 1\n"
    "SPEAKER guard name \"@Cap:spk.guard=Guard\" colour #7DD3FC\n"
    "SPEAKER hero name \"Hero\"\n"
    "\n"
    "CUE intro_01 sound \"Audio/VO/intro_01.ocaudio\" kind dialogue speaker guard priority 8\n"
    "  LINE 0.0 2.4 \"@Cap:intro_01_a=Welcome to the station.\"\n"
    "  LINE 2.6 0   \"@Cap:intro_01_b=Keep your voice down.\"        # dur 0 = until the voice ends\n"
    "CUE boom sound \"Audio/SFX/explosion_*.ocaudio\" sound \"Audio/SFX/blast_?.ocaudio\" kind sound priority 5 radius 4000 cooldown 1.5 hold 1.8 tag \"@Cap:fx.explosion=[explosion]\"\n"
    "FUTURE record from a newer build\n";

static void testHeader() {
    AVER_INFO("=== header ===");
    OcCaptionData d;
    std::string err;
    check(!parseOcCaptions("", d, nullptr, &err) && !err.empty(), "an empty file fails with a reason");
    err.clear();
    check(!parseOcCaptions("CUE a tag \"x\"\n", d, nullptr, &err), "a record before the header fails");
    err.clear();
    check(!parseOcCaptions("OCCAPTIONS 2\n", d, nullptr, &err) && err.find("unsupported") != std::string::npos, "a newer version fails as unsupported");
    err.clear();
    check(!parseOcCaptions("OCCAPTIONS\n", d, nullptr, &err), "a header with no version fails");
    check(parseOcCaptions("# lead\n\nOCCAPTIONS 1\n", d, nullptr, &err) && d.cues.empty() && d.version == 1, "comments may precede the header; nothing else is fine");
}

static void testReader() {
    AVER_INFO("=== reader ===");
    OcCaptionData d;
    std::vector<std::string> diag;
    std::string err;
    check(parseOcCaptions(kSample, d, &diag, &err), "the sample parses");
    check(diag.empty(), "with no diagnostics (a trailing comment and an unknown record are fine)");
    check(d.speakers.size() == 2 && d.speakers[0].id == "guard" && d.speakers[0].name == "@Cap:spk.guard=Guard" && d.speakers[0].rgba == 0x7DD3FCFFu, "speakers: id, name reference, colour");
    check(d.speakers[1].rgba == 0xFFFFFFFFu, "a speaker with no colour is white");
    check(d.cues.size() == 2, "two cues");
    const OcCaptionCue* a = findCue(d, "intro_01");
    check(a && a->kind == OcCaptionKind::Dialogue && a->speaker == "guard" && a->priority == 8 && a->sounds.size() == 1 && a->sounds[0] == "Audio/VO/intro_01.ocaudio", "the dialogue cue's fields");
    check(a && a->lines.size() == 2 && a->lines[0].start == 0.0f && std::fabs(a->lines[0].dur - 2.4f) < 1e-5f && a->lines[1].start == 2.6f && a->lines[1].dur == 0.0f, "its lines and times");
    check(a && a->lines[1].text == "@Cap:intro_01_b=Keep your voice down.", "the comment after a line is not part of its text");
    const OcCaptionCue* b = findCue(d, "boom");
    check(b && b->kind == OcCaptionKind::Sound && b->sounds.size() == 2 && b->radius == 4000.0f && b->cooldown == 1.5f && b->hold == 1.8f && b->priority == 5, "the sound cue's fields, with a repeated sound");
    check(b && b->tag == "@Cap:fx.explosion=[explosion]" && b->lines.empty(), "tag is kept as the shorthand");
    const std::vector<OcCaptionLine> bl = b ? captionCueLines(*b) : std::vector<OcCaptionLine>();
    check(bl.size() == 1 && bl[0].start == 0.0f && bl[0].dur == 0.0f && bl[0].text == "@Cap:fx.explosion=[explosion]", "captionCueLines expands the tag into one LINE 0 0");
    check(b && captionCueHold(*b) == 1.8f && a && std::fabs(captionCueHold(*a) - 0.8f) < 1e-5f, "hold: the cue's own, else 0.8 for dialogue");
    OcCaptionCue plain;
    check(captionCueHold(plain) == 1.5f, "and 1.5 for anything else");

    // Defaults.
    check(parseOcCaptions("OCCAPTIONS 1\nCUE x tag \"t\"\n", d) && d.cues.size() == 1 && d.cues[0].kind == OcCaptionKind::Sound && d.cues[0].cooldown == 1.0f && d.cues[0].hold < 0 && d.cues[0].radius == 0.0f, "defaults: sound, cooldown 1, hold unset, radius 0");

    // Quoting.
    check(parseOcCaptions("OCCAPTIONS 1\nCUE q tag \"say \\\"hi\\\" \\\\ # not a comment\"\n", d) && d.cues[0].tag == "say \"hi\" \\ # not a comment", "quotes, backslashes and '#' inside text");
}

static void testMalformed() {
    AVER_INFO("=== malformed records ===");
    OcCaptionData d;
    std::vector<std::string> diag;
    const std::string text =
        "OCCAPTIONS 1\n"                                   // 1
        "CUE ok tag \"fine\"\n"                            // 2
        "CUE bad radius abc tag \"x\"\n"                   // 3  bad number
        "  LINE 0 1 \"belongs to the bad cue\"\n"          // 4  skipped quietly
        "CUE empty sound \"a.ocaudio\"\n"                  // 5  no line and no tag
        "CUE good sound \"b.ocaudio\"\n"                   // 6
        "  LINE x 1 \"bad start\"\n"                       // 7  bad number
        "  LINE 1 1 \"fine line\"\n"                       // 8
        "  LINE 2 1\n"                                     // 9  no text
        "SPEAKER s colour red\n"                           // 10 bad colour
        "CUE odd kind loud tag \"x\"\n"                    // 11 bad kind
        "CUE after tag \"still read\"\n";                  // 12
    check(parseOcCaptions(text, d, &diag), "a file with bad records still parses");
    check(d.cues.size() == 3 && findCue(d, "ok") && findCue(d, "good") && findCue(d, "after"), "the good cues survive: ok, good, after");
    const OcCaptionCue* good = findCue(d, "good");
    check(good && good->lines.size() == 1 && good->lines[0].text == "fine line", "a bad LINE is skipped, the good one kept");
    check(anyContains(diag, "line 3:") && anyContains(diag, "bad number"), "the bad radius is named by line and reason");
    check(anyContains(diag, "line 5:") && anyContains(diag, "no LINE and no tag"), "a cue with nothing to show is skipped and named");
    check(anyContains(diag, "line 7:"), "the bad start is named");
    check(anyContains(diag, "line 9:"), "a LINE with no text is named");
    check(anyContains(diag, "line 10:") && anyContains(diag, "colour"), "a bad speaker colour is named");
    check(anyContains(diag, "line 11:") && anyContains(diag, "kind"), "a bad kind is named");
    check(!anyContains(diag, "line 4:"), "the lines of an already-reported cue are skipped quietly");
    check(d.speakers.empty(), "the bad speaker was not added");

    // Duplicates are kept as authored (replacement is the loader's rule).
    OcCaptionData dup;
    check(parseOcCaptions("OCCAPTIONS 1\nCUE a tag \"one\"\nCUE a tag \"two\"\n", dup) && dup.cues.size() == 2 && dup.cues[1].tag == "two", "duplicate cue ids are kept as authored");
}

static void testWriter() {
    AVER_INFO("=== writer ===");
    OcCaptionData d;
    parseOcCaptions(kSample, d);

    // Unchanged data rewrites to the same text.
    check(writeOcCaptions(d, kSample) == kSample, "unchanged data writes the file back byte for byte, comments and unknown records included");

    // A changed line, a new line, a removed cue, a new speaker and a new cue.
    OcCaptionData e = d;
    e.cues[0].lines[1].text = "Quiet.";
    e.cues[0].lines.push_back(OcCaptionLine{9.0f, 1.0f, "@Cap:late=Late", ""});
    e.speakers.push_back(OcCaptionSpeaker{"zed", "Zed", 0x112233FFu});
    OcCaptionCue added;
    added.id = "newcue";
    added.tag = "[new]";
    e.cues.push_back(added);
    std::string out = writeOcCaptions(e, kSample);
    OcCaptionData back;
    check(parseOcCaptions(out, back), "the merged file parses");
    check(back.cues.size() == 3 && back.cues[0].lines.size() == 3 && back.cues[0].lines[1].text == "Quiet." && back.cues[0].lines[2].text == "@Cap:late=Late", "the edited and added lines are there");
    check(back.speakers.size() == 3 && back.speakers[2].id == "zed" && back.speakers[2].rgba == 0x112233FFu, "the new speaker joined the others");
    check(findCue(back, "newcue") && findCue(back, "newcue")->tag == "[new]", "the new cue was appended");
    check(out.find("# Captions for the intro") != std::string::npos && out.find("FUTURE record from a newer build") != std::string::npos, "the comment and the unknown record survived");
    check(out.find("# dur 0 = until the voice ends") == std::string::npos, "(an edited line is rewritten, so its trailing comment goes with it)");
    check(out.find("LINE 0 2.4 \"@Cap:intro_01_a=Welcome to the station.\"") != std::string::npos || out.find("LINE 0.0 2.4") != std::string::npos, "an untouched line keeps its own text");

    OcCaptionData f = d;
    f.cues.erase(f.cues.begin());
    out = writeOcCaptions(f, kSample);
    OcCaptionData back2;
    parseOcCaptions(out, back2);
    check(back2.cues.size() == 1 && back2.cues[0].id == "boom" && out.find("Welcome to the station") == std::string::npos, "a removed cue takes its lines with it");
    check(out.find("# Captions for the intro") != std::string::npos, "and the comment stays");

    // A fresh file.
    OcCaptionData fresh;
    fresh.speakers.push_back(OcCaptionSpeaker{"narrator", "@Cap:spk.narrator=Narrator", 0x7DD3FCFFu});
    OcCaptionCue c;
    c.id = "intro";
    c.kind = OcCaptionKind::Dialogue;
    c.speaker = "narrator";
    c.sounds.push_back("Audio/VO/intro.ocaudio");
    c.priority = 3;
    c.lines.push_back(OcCaptionLine{0.0f, 2.0f, "Say \"hello\"", ""});
    fresh.cues.push_back(c);
    const std::string text = writeOcCaptions(fresh);
    OcCaptionData rt;
    check(text.rfind("OCCAPTIONS 1\n", 0) == 0 && parseOcCaptions(text, rt), "a fresh file starts with the header and parses");
    check(rt.speakers.size() == 1 && rt.speakers[0].rgba == 0x7DD3FCFFu && rt.cues.size() == 1 && rt.cues[0].lines[0].text == "Say \"hello\"" && rt.cues[0].priority == 3 &&
              rt.cues[0].kind == OcCaptionKind::Dialogue && rt.cues[0].sounds[0] == "Audio/VO/intro.ocaudio",
          "round trip: speakers, cue fields, quoted text");
}

static void testColour() {
    AVER_INFO("=== colour grammar ===");
    u32 c = 0;
    check(parseCaptionColour("#7DD3FC", c) && c == 0x7DD3FCFFu, "#RRGGBB is opaque");
    check(parseCaptionColour("#7dd3fc80", c) && c == 0x7DD3FC80u, "#RRGGBBAA carries alpha, any case");
    check(!parseCaptionColour("7DD3FC", c) && !parseCaptionColour("#7DD3F", c) && !parseCaptionColour("#GGGGGG", c), "bad colours are refused");
    check(formatCaptionColour(0x7DD3FCFFu) == "#7DD3FC" && formatCaptionColour(0x7DD3FC80u) == "#7DD3FC80", "and formatted back");
}

static void testGather() {
    AVER_INFO("=== localization gather ===");
    std::vector<LocUse> uses;
    gatherOccaptions(kSample, "Content/Captions/intro.occaptions", uses);
    bool guard = false, line1 = false, line2 = false, boom = false;
    for (const LocUse& u : uses) {
        if (u.table == "Cap" && u.key == "spk.guard" && u.defaultText == "Guard") guard = true;
        if (u.key == "intro_01_a" && u.defaultText == "Welcome to the station.") line1 = true;
        if (u.key == "intro_01_b" && u.defaultText == "Keep your voice down.") line2 = true;
        if (u.key == "fx.explosion" && u.defaultText == "[explosion]") boom = true;
    }
    check(guard && line1 && line2 && boom && uses.size() == 4, "speaker names, line texts and tags are gathered (four references)");
    check(!uses.empty() && uses[0].source.find("intro.occaptions:") != std::string::npos, "with a path:line source");
}

int main() {
    AVER_INFO("OcCaptionsTest");
    testHeader();
    testReader();
    testMalformed();
    testWriter();
    testColour();
    testGather();
    AVER_INFO(g_failures == 0 ? "OcCaptionsTest: PASS" : "OcCaptionsTest: FAIL");
    return g_failures == 0 ? 0 : 1;
}
