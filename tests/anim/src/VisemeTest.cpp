// .ocvis tracks, the lip-sync envelope at hand-computed times, and the baker. No scene and no GPU.
#include "aver/anim/AnimSampler.hpp"
#include "aver/anim/Viseme.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}
static void checkNear(f32 got, f32 want, f32 eps, const std::string& what) {
    if (std::fabs(got - want) <= eps) { AVER_INFO("  ok    {} ({:.5f})", what, got); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {} (got {:.6f}, want {:.6f})", what, got, want);
}

static u64 idOf(std::string_view n) { return fnv1a64(n); }

static anim::VisemeEvent ev(f32 s, f32 e, const char* sym, f32 in = 1.0f) {
    anim::VisemeEvent v;
    v.start = s; v.end = e; v.symbol = sym; v.intensity = in;
    return v;
}

static f32 weightOf(const std::vector<anim::VisemeSample>& v, u64 pose) {
    for (const anim::VisemeSample& s : v) if (s.pose == pose) return s.weight;
    return 0.0f;
}

static void testFile() {
    AVER_INFO("=== .ocvis text ===");
    const char* text =
        "# a comment\n"
        "OCVIS 1\n"
        "SET phoneme\n"
        "EVENT 0.12 0.31 HH 0.8   # trailing comment\n"
        "EVENT 0.00 0.12 sil\n"
        "EVENT 0.5 0.4 AA\n"                 // ends before it starts: skipped
        "EVENT 0.5 0.9\n"                    // no symbol: skipped
        "EVENT abc 0.9 AA\n"                 // not a number: skipped
        "FUTURE record is ignored\n"
        "EVENT 0.31 0.6 AH\n";
    anim::VisemeTrack t;
    std::vector<std::string> diag;
    std::string why;
    check(anim::parseVisemeTrack(text, t, &diag, &why), "parses: " + why);
    check(t.events.size() == 3, "three good events");
    check(diag.size() == 3, "three bad lines reported (" + std::to_string(diag.size()) + ")");
    if (t.events.size() == 3) {
        check(t.events[0].symbol == "sil" && t.events[1].symbol == "HH" && t.events[2].symbol == "AH", "events are sorted by start");
        checkNear(t.events[1].intensity, 0.8f, 1e-6f, "intensity");
        checkNear(t.events[0].intensity, 1.0f, 1e-6f, "default intensity is 1");
    }
    check(t.set == anim::VisemeSet::Phoneme, "the default symbol set is phoneme");

    const std::string round = anim::writeVisemeTrack(t);
    anim::VisemeTrack b;
    check(anim::parseVisemeTrack(round, b, nullptr, &why) && b.events.size() == 3 && b.events[1].symbol == "HH" &&
          b.events[1].intensity == 0.8f, "writes and parses back");

    anim::VisemeTrack v;
    check(anim::parseVisemeTrack("OCVIS 1\nSET viseme\nEVENT 0 1 aa\n", v, nullptr, &why) && v.set == anim::VisemeSet::Viseme, "SET viseme switches the symbol set");
    check(!anim::parseVisemeTrack("EVENT 0 1 aa\n", v, nullptr, &why), "no header fails: " + why);
    check(!anim::parseVisemeTrack("OCVIS 9\n", v, nullptr, &why), "a newer version fails: " + why);
    check(anim::parseVisemeTrack("", v, nullptr, &why) == false, "an empty file has no header");

    std::string many = "OCVIS 1\n";
    for (int i = 0; i < 65537; ++i) many += "EVENT " + std::to_string(i) + " " + std::to_string(i + 1) + " aa\n";
    check(!anim::parseVisemeTrack(many, v, nullptr, &why), "more than 65536 events fails: " + why);
    check(t.valid(&why), "a parsed track is valid");
    anim::VisemeTrack bad = t;
    bad.events[0].end = bad.events[0].start;
    check(!bad.valid(&why), "an event that does not end after it starts is invalid: " + why);
}

static void testEnvelope() {
    AVER_INFO("=== evalVisemeWeights ===");
    const anim::LipSyncParams p;   // lead .04, attack .05, release .08, minHold .06, maxTotal 1
    const u64 aa = idOf("viseme_aa"), oh = idOf("viseme_oh");
    std::vector<anim::VisemeEvent> events = {ev(1.0f, 1.5f, "aa")};
    anim::resolveVisemeEvents(events, nullptr, true);
    check(events[0].pose == aa, "a viseme symbol resolves to its pose id");
    std::vector<anim::VisemeSample> out;

    anim::evalVisemeWeights(events, p, 0.9f, out);
    check(out.empty(), "before the lead: nothing");
    anim::evalVisemeWeights(events, p, 0.96f, out);
    check(weightOf(out, aa) < 1e-4f, "at the start of the rise: still zero");
    anim::evalVisemeWeights(events, p, 1.005f, out);
    checkNear(weightOf(out, aa), 0.5f, 1e-4f, "mid attack: smoothstep(0.5) = 0.5");
    anim::evalVisemeWeights(events, p, 1.2f, out);
    checkNear(weightOf(out, aa), 1.0f, 1e-5f, "hold");
    anim::evalVisemeWeights(events, p, 1.54f, out);
    checkNear(weightOf(out, aa), 0.5f, 1e-4f, "mid release");
    anim::evalVisemeWeights(events, p, 1.6f, out);
    check(out.empty(), "past the release: gone");

    std::vector<anim::VisemeEvent> shortEv = {ev(2.0f, 2.01f, "aa")};
    anim::resolveVisemeEvents(shortEv, nullptr, true);
    anim::evalVisemeWeights(shortEv, p, 2.05f, out);
    checkNear(weightOf(out, aa), 1.0f, 1e-5f, "an event is held for at least minHold (2.0 + 0.06)");
    anim::evalVisemeWeights(shortEv, p, 2.06f + 0.04f, out);
    checkNear(weightOf(out, aa), 0.5f, 1e-4f, "and releases from there");

    std::vector<anim::VisemeEvent> half = {ev(0.0f, 1.0f, "aa", 0.5f)};
    anim::resolveVisemeEvents(half, nullptr, true);
    anim::evalVisemeWeights(half, p, 0.5f, out);
    checkNear(weightOf(out, aa), 0.5f, 1e-5f, "intensity scales the weight");

    std::vector<anim::VisemeEvent> overlap = {ev(0.0f, 1.0f, "aa", 0.4f), ev(0.0f, 1.0f, "aa", 0.9f)};
    anim::resolveVisemeEvents(overlap, nullptr, true);
    anim::evalVisemeWeights(overlap, p, 0.5f, out);
    check(out.size() == 1, "two events of one viseme share one entry");
    checkNear(weightOf(out, aa), 0.9f, 1e-5f, "the larger wins (max, not sum)");

    std::vector<anim::VisemeEvent> two = {ev(0.0f, 1.0f, "aa"), ev(0.0f, 1.0f, "oh")};
    anim::resolveVisemeEvents(two, nullptr, true);
    anim::evalVisemeWeights(two, p, 0.5f, out);
    checkNear(weightOf(out, aa), 0.5f, 1e-5f, "two full visemes are scaled to sum to maxTotal (1): each 0.5");
    checkNear(weightOf(out, oh), 0.5f, 1e-5f, "(second)");
    anim::LipSyncParams loose = p;
    loose.maxTotal = 2.0f;
    anim::evalVisemeWeights(two, loose, 0.5f, out);
    checkNear(weightOf(out, aa), 1.0f, 1e-5f, "with room, both stay whole");
    check(out.size() == 2 && out[0].pose < out[1].pose, "the output is sorted by pose id");

    std::vector<anim::VisemeEvent> unknown = {ev(0.0f, 1.0f, "zzz"), ev(0.0f, 1.0f, "HH")};
    std::vector<std::string> names;
    const u32 n = anim::resolveVisemeEvents(unknown, nullptr, false, &names);
    check(n == 1 && names.size() == 1 && names[0] == "zzz", "an unknown symbol is reported once and not resolved");
    anim::evalVisemeWeights(unknown, p, 0.5f, out);
    check(out.size() == 1 && out[0].pose == idOf("viseme_ih"), "and is ignored by the envelope");
}

static void testBake() {
    AVER_INFO("=== bakeVisemeTrack ===");
    anim::VisemeTrack t;
    t.set = anim::VisemeSet::Phoneme;
    f32 at = 0.0f;
    for (const char* s : {"HH", "AH", "L", "OW"}) {
        t.events.push_back(ev(at, at + 0.08f, s));
        at += 0.08f;
    }
    const fmt::OcAnimation clip = anim::bakeVisemeTrack(t, nullptr, 30.0f);
    check(clip.tracks.empty() && !clip.curves.empty() && clip.valid(), "a curve-only clip that passes valid()");
    check(clip.flags == 0, "flags 0");
    checkNear(clip.duration, 0.32f + 0.08f, 1e-5f, "duration = last event end + release");
    check(clip.curves.size() == 4, "one curve per viseme used");
    bool named = true;
    for (const fmt::OcCurve& c : clip.curves) if (c.name.rfind("pose.viseme_", 0) != 0 || c.interp != fmt::OcInterp::Linear) named = false;
    check(named, "named pose.viseme_* and linear");

    std::vector<anim::VisemeEvent> events = t.events;
    anim::resolveVisemeEvents(events, nullptr, false);
    const anim::LipSyncParams params;
    std::vector<anim::VisemeSample> direct;
    f32 worst = 0.0f;
    for (int i = 0; i * (1.0f / 30.0f) <= clip.duration; ++i) {
        const f32 time = std::fmin(f32(i) / 30.0f, clip.duration);
        anim::evalVisemeWeights(events, params, time, direct);
        for (const fmt::OcCurve& c : clip.curves)
            worst = std::fmax(worst, std::fabs(anim::sampleCurve(c, time) - weightOf(direct, c.poseId)));
    }
    check(worst < 1.5e-3f, "the baked curves reproduce the envelope at every frame (worst " + std::to_string(worst) + ")");

    // keys are dropped where a line fits
    usize keys = 0;
    for (const fmt::OcCurve& c : clip.curves) keys += c.times.size();
    check(keys < clip.curves.size() * (usize(clip.duration * 30.0f) + 2), "redundant keys are dropped");

    // round trip through the container
    std::vector<u8> bytes; std::string why;
    check(fmt::writeOcAnim(clip, bytes, &why), "the clip writes: " + why);
    fmt::OcAnimation back;
    check(fmt::parseOcAnim(bytes.data(), bytes.size(), back, &why) && back.valid() && back.curves.size() == 4, "and reads back");

    // a rig override changes which curve a symbol drives
    anim::FaceRigAsset rig;
    rig.phonemes = {{"HH", "viseme_oh"}};
    const fmt::OcAnimation over = anim::bakeVisemeTrack(t, &rig, 30.0f);
    bool hasIh = false;
    for (const fmt::OcCurve& c : over.curves) if (c.name == "pose.viseme_ih") hasIh = true;
    check(!hasIh && over.curves.size() == 3, "a rig's phoneme override redirects HH to viseme_oh (so ih disappears, oh merges)");

    anim::VisemeTrack empty;
    check(anim::bakeVisemeTrack(empty, nullptr).curves.empty(), "an empty track bakes to no curves");
}

int main() {
    testFile();
    testEnvelope();
    testBake();
    if (g_failures == 0) AVER_INFO("=== all viseme tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
