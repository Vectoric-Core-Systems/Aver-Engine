// CaptionEngine.hpp: pattern matching, mode and radius filtering, cooldown, timed and voice-bound lines, the line cap,
// speaker changes, colours, direction hints, fade and the show/stop calls. Header-only, no audio, no wall clock: the
// engine's own time is the sum of the dt it is fed. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/formats/OcCaptions.hpp"
#include "aver/framework/CaptionEngine.hpp"

#include <cmath>
#include <set>
#include <string>

using namespace aver;
using namespace aver::fw::access;
namespace fm = aver::fmt;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }

static fm::OcCaptionCue cue(const char* id, fm::OcCaptionKind kind, const char* sound, const char* tag = "", i32 priority = 0) {
    fm::OcCaptionCue c;
    c.id = id;
    c.kind = kind;
    if (sound && *sound) c.sounds.push_back(sound);
    c.tag = tag;
    c.priority = priority;
    return c;
}

static fm::OcCaptionLine line(f32 start, f32 dur, const char* text) {
    fm::OcCaptionLine l;
    l.start = start; l.dur = dur; l.text = text;
    return l;
}

// A voice set the test controls: voices in `alive` are playing.
static std::set<i32> g_alive;
static bool probe(i32 v) { return g_alive.count(v) != 0; }

static CaptionSoundStart snd(const char* path, i32 voice = 0, bool pos = false, f32 x = 0, f32 y = 0, f32 z = 0) {
    CaptionSoundStart s;
    s.path = path; s.voice = voice; s.positional = pos;
    s.pos[0] = x; s.pos[1] = y; s.pos[2] = z;
    return s;
}

static const CaptionListener kOrigin{};

static void testPatterns() {
    AVER_INFO("=== patterns ===");
    check(fm::captionPatternMatches("Audio/VO/intro_01.ocaudio", "D:/proj/Content/Audio/VO/intro_01.ocaudio"), "a content-relative path matches the tail of an absolute one");
    check(fm::captionPatternMatches("audio/sfx/*.ocaudio", "Audio\\SFX\\boom.OCAUDIO"), "case-insensitive, either slash, * inside a segment");
    check(fm::captionPatternMatches("explosion_?.ocaudio", "x/explosion_2.ocaudio") && !fm::captionPatternMatches("explosion_?.ocaudio", "x/explosion_12.ocaudio"), "? is one character");
    check(!fm::captionPatternMatches("a/b/c.ocaudio", "b/c.ocaudio"), "a pattern with more segments than the path cannot match");
    check(!fm::captionPatternMatches("SFX/boom.ocaudio", "SFX/other/boom.ocaudio"), "no match: the segments must be the tail");
    check(fm::captionPatternExact("a/b.ocaudio", "x/a/b.ocaudio") && !fm::captionPatternExact("a/*.ocaudio", "x/a/b.ocaudio"), "exact means no wildcard");
    check(!fm::captionPatternMatches("", "x"), "an empty pattern matches nothing");

    CaptionEngine e;
    fm::OcCaptionData d;
    d.cues.push_back(cue("pat", fm::OcCaptionKind::Sound, "Audio/SFX/*.ocaudio", "[pattern]", 5));
    d.cues.push_back(cue("exact", fm::OcCaptionKind::Sound, "Audio/SFX/boom.ocaudio", "[exact]", 1));
    d.cues.push_back(cue("hi", fm::OcCaptionKind::Sound, "Audio/SFX/*.ocaudio", "[hi]", 9));
    e.load(d);
    check(e.matchSound("C:/p/Content/Audio/SFX/boom.ocaudio")->id == "exact", "an exact path wins over a higher-priority pattern");
    check(e.matchSound("C:/p/Content/Audio/SFX/door.ocaudio")->id == "hi", "among patterns the highest priority wins");
    check(e.matchSound("Audio/Music/theme.ocaudio") == nullptr, "no match, no cue");
}

static void testModes() {
    AVER_INFO("=== mode, radius, cooldown ===");
    CaptionEngine e;
    fm::OcCaptionData d;
    d.cues.push_back(cue("vo", fm::OcCaptionKind::Dialogue, "VO/a.ocaudio", "hi"));
    d.cues.push_back(cue("fx", fm::OcCaptionKind::Sound, "SFX/a.ocaudio", "[fx]"));
    d.cues.push_back(cue("mus", fm::OcCaptionKind::Music, "Music/a.ocaudio", "[music]"));
    d.cues[1].radius = 1000;
    d.cues[1].cooldown = 2.0f;
    e.load(d);

    CaptionOptions o;
    o.mode = 1;
    e.setOptions(o);
    check(e.onSound(snd("VO/a.ocaudio")) && !e.onSound(snd("SFX/a.ocaudio")) && !e.onSound(snd("Music/a.ocaudio")), "Subtitles keep dialogue and drop sound and music");
    o.mode = 0;
    e.setOptions(o);
    check(!e.onSound(snd("VO/a.ocaudio")), "Off shows nothing");
    o.mode = 2;
    e.setOptions(o);
    e.update(0.0f, kOrigin);
    check(e.onSound(snd("Music/a.ocaudio")), "Captions show every kind");

    check(!e.onSound(snd("SFX/a.ocaudio", 0, true, 5000, 0, 0)), "a positional sound beyond the radius is skipped");
    check(e.onSound(snd("SFX/a.ocaudio", 0, true, 500, 0, 0)), "and inside it is shown");
    check(!e.onSound(snd("SFX/a.ocaudio", 0, true, 500, 0, 0)), "a repeat inside the cooldown is dropped");
    e.update(1.9f, kOrigin);
    check(!e.onSound(snd("SFX/a.ocaudio", 0, true, 500, 0, 0)), "still dropped at 1.9 s");
    e.update(0.2f, kOrigin);
    check(e.onSound(snd("SFX/a.ocaudio", 0, true, 500, 0, 0)), "and shown again once the cooldown has passed (an injected clock: the sum of dt)");
    check(!e.show("missing") && e.show("mus"), "show() by id: unknown cue refused");
}

static void testTiming() {
    AVER_INFO("=== timed lines and voices ===");
    CaptionEngine e;
    e.setVoiceProbe(probe);
    CaptionOptions o;
    o.mode = 2;
    e.setOptions(o);
    fm::OcCaptionData d;
    fm::OcCaptionCue c = cue("t", fm::OcCaptionKind::Sound, "", "");
    c.lines.push_back(line(0.0f, 1.0f, "one"));
    c.lines.push_back(line(1.5f, 1.0f, "two"));
    d.cues.push_back(c);
    e.load(d);
    check(e.show("t"), "a timed cue starts");
    e.update(0.1f, kOrigin);
    check(e.lines().size() == 1 && e.lines()[0].textRef == "one", "the first line shows at its start");
    check(e.newLines().size() == 1, "and is a new line once");
    e.update(0.1f, kOrigin);
    check(e.newLines().empty(), "not again on the next update");
    e.update(1.0f, kOrigin);   // t = 1.2
    check(e.lines().empty(), "between lines nothing shows");
    e.update(0.5f, kOrigin);   // t = 1.7
    check(e.lines().size() == 1 && e.lines()[0].textRef == "two", "the second line shows from 1.5");
    e.update(1.5f, kOrigin);   // t = 3.2: past 2.5
    check(e.lines().empty() && e.activeCount() == 0, "a cue with no voice ends at its last line");

    // dur 0 follows the voice: voice ends, +0.35 s, never less than hold.
    CaptionEngine v;
    v.setVoiceProbe(probe);
    v.setOptions(o);
    fm::OcCaptionData dv;
    fm::OcCaptionCue vc = cue("v", fm::OcCaptionKind::Sound, "S/a.ocaudio", "[boom]");
    vc.hold = 1.0f;
    dv.cues.push_back(vc);
    v.load(dv);
    g_alive = {7};
    check(v.onSound(snd("S/a.ocaudio", 7)), "a voice-bound cue starts");
    v.update(0.2f, kOrigin);
    check(v.lines().size() == 1, "it shows while the voice plays");
    v.update(5.0f, kOrigin);
    check(v.lines().size() == 1 && v.lines()[0].alpha == 1.0f, "and stays as long as the voice plays");
    g_alive.clear();   // the voice ends at t = 5.2
    v.update(0.1f, kOrigin);
    check(v.lines().size() == 1, "after the voice ends it lingers");
    v.update(0.4f, kOrigin);   // 0.4 after the end
    check(v.lines().empty(), "and goes 0.35 s later (hold 1.0 s counts from the line start, long past)");

    // A short sound keeps its caption for at least `hold`.
    CaptionEngine h;
    h.setVoiceProbe(probe);
    h.setOptions(o);
    fm::OcCaptionData dh;
    fm::OcCaptionCue hc = cue("h", fm::OcCaptionKind::Sound, "S/b.ocaudio", "[pop]");
    hc.hold = 1.8f;
    dh.cues.push_back(hc);
    h.load(dh);
    g_alive = {9};
    h.onSound(snd("S/b.ocaudio", 9));
    h.update(0.05f, kOrigin);
    g_alive.clear();
    h.update(0.1f, kOrigin);
    h.update(1.0f, kOrigin);   // t = 1.15
    check(h.lines().size() == 1, "a short sound stays up for its hold time");
    h.update(1.0f, kOrigin);   // t = 2.15
    check(h.lines().empty(), "and goes after it");

    // Dialogue cut short keeps its current line for at least 0.8 s.
    CaptionEngine dl;
    dl.setVoiceProbe(probe);
    dl.setOptions(o);
    fm::OcCaptionData dd;
    fm::OcCaptionCue dc = cue("d", fm::OcCaptionKind::Dialogue, "VO/c.ocaudio", "");
    dc.lines.push_back(line(0.0f, 5.0f, "long line"));
    dc.lines.push_back(line(5.0f, 0.0f, "never shown"));
    dd.cues.push_back(dc);
    dl.load(dd);
    g_alive = {3};
    dl.onSound(snd("VO/c.ocaudio", 3));
    dl.update(0.1f, kOrigin);
    g_alive.clear();           // cut at t = 0.2
    dl.update(0.1f, kOrigin);
    check(dl.lines().size() == 1 && dl.lines()[0].textRef == "long line", "a cut-short line is still there");
    dl.update(0.4f, kOrigin);  // t = 0.6
    check(dl.lines().size() == 1, "and keeps its minimum dwell");
    dl.update(0.5f, kOrigin);  // t = 1.1
    check(dl.lines().empty(), "then goes, and the line after the cut never shows");
}

static void testCapAndSpeakers() {
    AVER_INFO("=== line cap, speakers, colours ===");
    CaptionEngine e;
    CaptionOptions o;
    o.mode = 2;
    o.maxLines = 2;
    e.setOptions(o);
    fm::OcCaptionData d;
    fm::OcCaptionSpeaker g;
    g.id = "guard"; g.name = "@Cap:spk.guard=Guard"; g.rgba = 0x7DD3FCFFu;
    d.speakers.push_back(g);
    fm::OcCaptionSpeaker h;
    h.id = "hero"; h.name = "Hero"; h.rgba = 0xFFD166FFu;
    d.speakers.push_back(h);
    fm::OcCaptionCue a = cue("a", fm::OcCaptionKind::Dialogue, "", "", 8);
    a.speaker = "guard";
    a.lines.push_back(line(0.0f, 3.0f, "first"));
    a.lines.push_back(line(0.0f, 3.0f, "second"));
    d.cues.push_back(a);
    fm::OcCaptionCue b = cue("b", fm::OcCaptionKind::Dialogue, "", "", 8);
    b.speaker = "hero";
    b.lines.push_back(line(0.0f, 3.0f, "third"));
    d.cues.push_back(b);
    d.cues.push_back(cue("s1", fm::OcCaptionKind::Sound, "", "[s1]", 1));
    d.cues.push_back(cue("s2", fm::OcCaptionKind::Sound, "", "[s2]", 9));
    e.load(d);

    e.show("a");
    e.update(0.1f, kOrigin);
    check(e.lines().size() == 2, "two lines at once");
    check(e.lines()[0].speakerRef == "@Cap:spk.guard=Guard" && e.lines()[1].speakerRef.empty(), "the speaker shows once while the same person keeps talking");
    check(e.lines()[0].rgba == 0x7DD3FCFFu, "in the speaker's colour");

    e.show("b");
    e.update(0.1f, kOrigin);
    check(e.lines().size() == 2, "the cap holds at 2");
    // a and b share priority 8: the oldest dialogue line goes first.
    bool hasThird = false;
    for (const auto& l : e.lines()) if (l.textRef == "third") hasThird = true;
    check(hasThird, "the newest dialogue line makes room by evicting the oldest");

    e.show("s2");
    e.update(0.1f, kOrigin);
    int dlg = 0, snd2 = 0;
    for (const auto& l : e.lines()) { if (l.kind == 0) ++dlg; else ++snd2; }
    check(dlg == 2 && snd2 == 0, "a sound caption never evicts dialogue (even at a higher priority)");

    o.colours = false;
    o.speaker = false;
    e.setOptions(o);
    e.update(0.0f, kOrigin);
    check(e.lines()[0].rgba == 0xFFFFFFFFu && e.lines()[0].speakerRef.empty(), "colours and speaker names can be turned off");
    o.speaker = true;
    e.setOptions(o);
    e.update(0.0f, kOrigin);
    check(!e.lines().empty() && e.lines().front().speakerRef.size() > 0, "and back on");

    // Ad-hoc text.
    CaptionEngine t;
    CaptionOptions to;
    to.mode = 2;
    t.setOptions(to);
    check(t.showText("Narrator", "@Cap:x=Hello", 2.0f, 0xFF0000FFu, 0), "showText raises a line with no cue");
    t.update(0.1f, kOrigin);
    check(t.lines().size() == 1 && t.lines()[0].speakerRef == "Narrator" && t.lines()[0].rgba == 0xFF0000FFu && t.lines()[0].textRef == "@Cap:x=Hello", "with the speaker, colour and raw reference");
    t.update(2.5f, kOrigin);
    check(t.lines().empty(), "that ends after its seconds");
    t.showText("", "again", 5.0f, 0xFFFFFFFFu, 1);
    t.stop("");
    t.update(0.1f, kOrigin);
    check(t.lines().empty(), "stop with no cue ends everything");
    CaptionOptions sub;
    sub.mode = 1;
    t.setOptions(sub);
    check(!t.showText("", "sound text", 2.0f, 0xFFFFFFFFu, 1) && t.showText("", "speech", 2.0f, 0xFFFFFFFFu, 0), "showText obeys the mode filter");
}

static void testDirectionAndFade() {
    AVER_INFO("=== direction hints and fade ===");
    CaptionEngine e;
    CaptionOptions o;
    o.mode = 2;
    o.direction = true;
    e.setOptions(o);
    fm::OcCaptionData d;
    d.cues.push_back(cue("p", fm::OcCaptionKind::Sound, "", "[p]"));
    d.cues[0].cooldown = 0.0f;
    e.load(d);
    CaptionListener l;   // at the origin, +X forward, +Y right
    const struct { f32 deg; i32 want; } cases[] = {{-90, -1}, {-20, 0}, {0, 0}, {20, 0}, {90, 1}, {170, 2}, {-170, 2}};
    for (const auto& c : cases) {
        e.stop("");
        const f32 r = c.deg * 3.14159265f / 180.0f;
        const f32 p[3] = {std::cos(r) * 100.0f, std::sin(r) * 100.0f, 0.0f};
        e.show("p", p);
        e.update(0.0f, l);
        check(e.lines().size() == 1 && e.lines()[0].dir == c.want, "direction at " + std::to_string(static_cast<int>(c.deg)) + " degrees");
    }
    e.stop("");
    e.show("p");   // not positional
    e.update(0.0f, l);
    check(e.lines().size() == 1 && e.lines()[0].dir == 0, "a source with no position has no hint");

    // Fade over the last 0.25 s; reduce motion forces 1.
    CaptionEngine f;
    f.setOptions(o);
    fm::OcCaptionData df;
    fm::OcCaptionCue fc = cue("f", fm::OcCaptionKind::Sound, "", "");
    fc.lines.push_back(line(0.0f, 1.0f, "fade"));
    df.cues.push_back(fc);
    f.load(df);
    f.show("f");
    f.update(0.5f, kOrigin);
    check(f.lines()[0].alpha == 1.0f, "full alpha mid-line");
    f.update(0.375f, kOrigin);   // 0.125 s left
    check(near(f.lines()[0].alpha, 0.5f), "half alpha halfway through the fade");
    o.reduceMotion = true;
    f.setOptions(o);
    f.update(0.0f, kOrigin);
    check(f.lines()[0].alpha == 1.0f, "reduce motion keeps alpha at 1");
}

static void testReload() {
    AVER_INFO("=== reload, clear, determinism ===");
    CaptionEngine e;
    CaptionOptions o;
    o.mode = 2;
    e.setOptions(o);
    fm::OcCaptionData d;
    fm::OcCaptionCue c = cue("keep", fm::OcCaptionKind::Sound, "", "");
    c.lines.push_back(line(0.0f, 5.0f, "old text"));
    d.cues.push_back(c);
    d.cues.push_back(cue("gone", fm::OcCaptionKind::Sound, "", "[gone]"));
    e.load(d);
    e.show("keep");
    e.show("gone");
    e.update(0.1f, kOrigin);
    check(e.lines().size() == 2, "two actives");

    fm::OcCaptionData d2;
    fm::OcCaptionCue c2 = cue("keep", fm::OcCaptionKind::Sound, "", "");
    c2.lines.push_back(line(0.0f, 5.0f, "new text"));
    d2.cues.push_back(c2);
    e.load(d2);
    e.update(0.0f, kOrigin);
    check(e.lines().size() == 1 && e.lines()[0].textRef == "new text", "a reload keeps the actives whose cue still exists, with the new text, and drops the rest");

    fm::OcCaptionData dup;
    dup.cues.push_back(cue("same", fm::OcCaptionKind::Sound, "", "[first]"));
    dup.cues.push_back(cue("same", fm::OcCaptionKind::Sound, "", "[second]"));
    e.load(dup);
    check(e.cueCount() == 1 && e.duplicateCount() == 1 && e.findCue("same")->tag == "[second]", "a duplicate cue id: the later one wins");

    e.clear();
    check(e.cueCount() == 0 && e.activeCount() == 0, "clear drops everything");

    // Two engines fed the same sequence agree exactly.
    CaptionEngine x, y;
    x.setOptions(o); y.setOptions(o);
    x.load(d2); y.load(d2);
    x.show("keep"); y.show("keep");
    bool same = true;
    for (int i = 0; i < 20; ++i) {
        x.update(0.07f, kOrigin);
        y.update(0.07f, kOrigin);
        if (x.lines().size() != y.lines().size() || (!x.lines().empty() && x.lines()[0].alpha != y.lines()[0].alpha)) same = false;
    }
    check(same, "deterministic: no wall clock anywhere");
}

int main() {
    AVER_INFO("CaptionEngineTest");
    testPatterns();
    testModes();
    testTiming();
    testCapAndSpeakers();
    testDirectionAndFade();
    testReload();
    AVER_INFO(g_failures == 0 ? "CaptionEngineTest: PASS" : "CaptionEngineTest: FAIL");
    return g_failures == 0 ? 0 : 1;
}
